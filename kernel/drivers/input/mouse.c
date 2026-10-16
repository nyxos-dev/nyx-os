#include "../../core/kernel.h"

#define MOUSE_BUFFER 16

static volatile int mouse_x = 0;
static volatile int mouse_y = 0;
static volatile int mouse_z = 0;             // accumulated wheel notches (+up / -down)
static volatile uint8_t mouse_buttons = 0;
static int mouse_has_wheel = 0;              // 1 = IntelliMouse: 4-byte packets with Z

static volatile uint8_t packet[4];
static volatile int packet_idx = 0;

// Save RFLAGS + disable interrupts / restore. Lets mouse_poll() drain the i8042 without the
// mouse IRQ re-entering the packet parser, and safely from any context (restores prior IF).
static inline uint64_t mouse_irq_save(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void mouse_irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

// Every wait is BOUNDED: on real hardware whose i8042 has no auxiliary (mouse) device —
// common on modern laptops/UMPCs where the touchpad is USB-HID — the status bit we poll
// never changes, so an unbounded `while` here hangs the whole boot. ~100k port reads is
// far longer than a present mouse ever needs to answer, yet still bails on a dead port.
#define PS2_TIMEOUT 100000

// Wait for the input buffer to drain (bit 1 clear) before writing. Returns 0, or -1 on timeout.
static int ps2_wait_write(void) {
    for (int i = 0; i < PS2_TIMEOUT; i++)
        if (!(inb(0x64) & 2)) return 0;
    return -1;
}

// Wait for the output buffer to fill (bit 0 set) before reading. Returns 0, or -1 on timeout.
static int ps2_wait_read(void) {
    for (int i = 0; i < PS2_TIMEOUT; i++)
        if (inb(0x64) & 1) return 0;
    return -1;
}

static uint8_t ps2_read(void) {
    if (ps2_wait_read() != 0) return 0xFF;   // timeout -> no data; 0xFF != 0xFA ACK, so callers bail
    return inb(0x60);
}

static void ps2_write(uint16_t port, uint8_t val) {
    ps2_wait_write();                        // best-effort; write anyway on timeout
    outb(port, val);
}

static void mouse_write(uint8_t val) {
    ps2_write(0x64, 0xD4);
    ps2_write(0x60, val);
}

// Enable the IntelliMouse scroll wheel: the standard "magic knock" — set the sample
// rate to 200, then 100, then 80 — makes a wheel mouse switch its device ID to 0x03
// and start sending 4-byte packets (the 4th byte is a signed Z/wheel delta). Returns
// 1 if the device reports the wheel ID. QEMU's PS/2 mouse implements this.
static int mouse_enable_wheel(void) {
    mouse_write(0xF3); ps2_read();   // set sample rate
    mouse_write(200);  ps2_read();
    mouse_write(0xF3); ps2_read();
    mouse_write(100);  ps2_read();
    mouse_write(0xF3); ps2_read();
    mouse_write(80);   ps2_read();
    mouse_write(0xF2); ps2_read();   // get device ID (ACK)
    uint8_t id = ps2_read();         // 0x03 = wheel mouse
    return id == 0x03;
}

int mouse_init(void) {
    // Flush any stale data from PS/2 controller (bounded — see PS2_TIMEOUT above)
    for (int i = 0; i < PS2_TIMEOUT && (inb(0x64) & 1); i++) inb(0x60);
    for (int i = 0; i < PS2_TIMEOUT && (inb(0x64) & 2); i++);  // wait for input buffer empty

    // Enable auxiliary device
    ps2_write(0x64, 0xA8);

    // Read controller config, enable IRQ12 + mouse clock
    ps2_write(0x64, 0x20);
    uint8_t config = ps2_read();
    config |= 0x02;  // Enable IRQ12
    config |= 0x20;  // Enable mouse clock
    ps2_write(0x64, 0x60);
    ps2_write(0x60, config);

    // Set mouse defaults
    mouse_write(0xF6);
    ps2_read();

    // Try to turn on the scroll wheel while reporting is still off (defaults disable
    // it), so the knock's replies aren't interleaved with movement packets.
    mouse_has_wheel = mouse_enable_wheel();

    // Enable mouse data reporting
    mouse_write(0xF4);
    if (ps2_read() != 0xFA) {
        serial_puts("[MOUSE] No ACK\n");
        return -1;
    }

    serial_puts(mouse_has_wheel ? "[MOUSE] Initialized (wheel)\n" : "[MOUSE] Initialized\n");
    return 0;
}

// Pure PS/2 packet decode: from a 3- or 4-byte packet produce the button bitmap and the
// movement/wheel deltas the driver applies. Y is negated (screen Y grows downward); dx/dy are
// int8 sign-extended. If either overflow flag (byte0 bits 6/7) is set the movement counters
// wrapped and dx/dy are meaningless, so the movement is DROPPED (deltas 0) while the buttons stay
// valid. The 4-bit wheel field is sign-extended and collapsed to one notch (±1/0). Split out so a
// KAT can pin the sign + overflow handling with no live hardware. Any out pointer may be NULL.
void mouse_decode_packet(const uint8_t pkt[4], int has_wheel,
                         int* out_buttons, int* out_dx, int* out_dy, int* out_dz) {
    if (out_buttons) *out_buttons = pkt[0] & 0x07;
    int dx = 0, dy = 0;
    if (!(pkt[0] & 0xC0)) {                       // no overflow -> movement is valid
        dx = (int)(int8_t)pkt[1];
        dy = -(int)(int8_t)pkt[2];
    }
    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
    int dz = 0;
    if (has_wheel) {
        int8_t z = (int8_t)(pkt[3] << 4) >> 4;    // sign-extend the low 4-bit Z field
        dz = (z > 0) ? 1 : (z < 0 ? -1 : 0);      // one accumulated notch, regardless of magnitude
    }
    if (out_dz) *out_dz = dz;
}

// The driver owns the cursor position, so it must share the display's bounds.
// Accumulating motion beyond an edge makes reversing direction feel stuck until
// that invisible distance is paid back. Clamp the old position too, for a resize.
static int mouse_axis_position(int position, int delta, uint32_t extent) {
    int limit = (extent > 0 && extent <= 0x7FFFFFFFu) ? (int)extent - 1 : 4095;
    if (position < 0) position = 0;
    if (position > limit) position = limit;
    int64_t next = (int64_t)position + delta;
    if (next < 0) return 0;
    if (next > limit) return limit;
    return (int)next;
}

// Parse one byte of the PS/2 packet stream. Called both from the IRQ handler and from
// mouse_poll() (the polling fallback for machines where IRQ12 is never delivered); the
// callers serialise access so packet_idx/packet[] are never touched re-entrantly.
static void mouse_process_byte(uint8_t data) {
    int last = mouse_has_wheel ? 3 : 2;      // index of the final byte in a packet

    if (packet_idx == 0) {
        if (!(data & 0x08)) return;          // byte 1 always has bit 3 set — resync
        packet[0] = data;
        packet_idx = 1;
    } else if (packet_idx < last) {
        packet[packet_idx++] = data;
    } else {
        packet[packet_idx] = data;           // final byte -> apply the whole packet
        packet_idx = 0;

        int buttons, dx, dy, dz;
        mouse_decode_packet((const uint8_t*)packet, mouse_has_wheel, &buttons, &dx, &dy, &dz);
        mouse_buttons = buttons;
        mouse_x = mouse_axis_position(mouse_x, dx, fb_get_width());
        mouse_y = mouse_axis_position(mouse_y, dy, fb_get_height());
        mouse_z += dz;
    }
}

// IRQ12 path (works under QEMU / when the firmware routes the legacy mouse IRQ).
void mouse_irq_handler(void* unused) {
    (void)unused;
    uint8_t st = inb(0x64);
    if ((st & 0x21) != 0x21) return;   // OBF=1 AND aux bit=1 → a mouse byte is waiting
    mouse_process_byte(inb(0x60));
}

// Polling fallback: drain any pending aux (mouse) bytes from the i8042 right now. Some real
// firmware/APIC routings never deliver IRQ12 even though a USB mouse is emulated on the aux
// port (the keyboard, IRQ1, still works) — the cursor then appears but never moves. Calling
// this from every mouse getter makes the pointer live regardless of IRQ delivery. Runs with
// interrupts off so it never races the IRQ handler over packet_idx/packet[]; keyboard bytes
// (aux bit clear) are left untouched for the keyboard IRQ.
void mouse_poll(void) {
    uint64_t f = mouse_irq_save();
    for (int guard = 0; guard < 64; guard++) {     // bounded burst so a stuck OBF can't spin
        uint8_t st = inb(0x64);
        if ((st & 0x21) != 0x21) break;            // no aux byte pending
        mouse_process_byte(inb(0x60));
    }
    mouse_irq_restore(f);
}

// MouseKeys: a keyboard-driven synthetic left/right click, held ~60 ms so the compositor's
// press→release edge detection registers it as a real click. Lets the desktop be driven with
// no pointing device at all (the compositor moves the cursor via mouse_set_pos on Alt+WASD).
static volatile uint32_t synth_lclick_until = 0;
static volatile uint32_t synth_rclick_until = 0;
void mouse_kbd_click(int right) {
    uint32_t until = get_ticks() + 60;   // ms
    if (right) synth_rclick_until = until; else synth_lclick_until = until;
}

int mouse_get_x(void) { mouse_poll(); return mouse_x; }
int mouse_get_y(void) { mouse_poll(); return mouse_y; }
int mouse_get_buttons(void) {
    mouse_poll();
    uint8_t b = mouse_buttons;
    uint32_t now = get_ticks();
    if (synth_lclick_until && now < synth_lclick_until) b |= 0x01;   // synthetic left
    if (synth_rclick_until && now < synth_rclick_until) b |= 0x02;   // synthetic right
    return b;
}
int mouse_get_z(void) { mouse_poll(); return mouse_z; }
void mouse_set_pos(int x, int y) {
    uint64_t flags = mouse_irq_save();
    mouse_x = mouse_axis_position(x, 0, fb_get_width());
    mouse_y = mouse_axis_position(y, 0, fb_get_height());
    mouse_irq_restore(flags);
}

// Pure edge/resize KAT: after pushing past an edge, one reverse pixel must move
// immediately. Covers both axes, a resized display, and pre-framebuffer input.
int mouse_bounds_selftest(void) {
    int y = mouse_axis_position(767, 60, 768);
    if (y != 767 || mouse_axis_position(y, -1, 768) != 766) return 1;
    int x = mouse_axis_position(1023, 127, 1024);
    if (x != 1023 || mouse_axis_position(x, -1, 1024) != 1022) return 2;
    x = mouse_axis_position(0, -128, 1024);
    if (x != 0 || mouse_axis_position(x, 1, 1024) != 1) return 3;
    if (mouse_axis_position(4095, -1, 768) != 766) return 4;
    if (mouse_axis_position(4999, 1, 5000) != 4999) return 5;
    if (mouse_axis_position(12, 1, 1) != 0) return 6;
    if (mouse_axis_position(4095, 1, 0) != 4095) return 7;
    if (mouse_axis_position(1, 0x7FFFFFFF, 1024) != 1023) return 8;
    return 0;
}

// KAT (`mouse-decode`): pins the pure PS/2 packet decode (mouse_decode_packet) — the int8 sign
// extension of dx/dy, the Y negation, the overflow drop-movement-keep-buttons rule, and the 4-bit
// wheel sign-extend + one-notch collapse. A regression here would teleport or invert the cursor,
// or scroll the wrong way, with no other test catching it. 0 = PASS, else the failing case number.
int mouse_decode_selftest(void) {
    uint8_t pkt[4]; int btn, dx, dy, dz;
    // 1. small positive move, no buttons — Y is negated
    pkt[0] = 0x08; pkt[1] = 5; pkt[2] = 3; pkt[3] = 0;
    mouse_decode_packet(pkt, 0, &btn, &dx, &dy, &dz);
    if (btn != 0 || dx != 5 || dy != -3 || dz != 0) return 1;
    // 2. negative deltas sign-extend: X = 0xFF (-1), Y = 0xFE (-2) -> dy = +2
    pkt[1] = 0xFF; pkt[2] = 0xFE;
    mouse_decode_packet(pkt, 0, &btn, &dx, &dy, &dz);
    if (dx != -1 || dy != 2) return 2;
    // 3. buttons (left+right = bits 0/1) survive alongside a move
    pkt[0] = 0x08 | 0x03; pkt[1] = 1; pkt[2] = 0;
    mouse_decode_packet(pkt, 0, &btn, &dx, &dy, &dz);
    if (btn != 0x03 || dx != 1) return 3;
    // 4. overflow (bit 6) drops the movement but keeps the (middle) button
    pkt[0] = 0x08 | 0x40 | 0x04; pkt[1] = 0x7F; pkt[2] = 0x7F;
    mouse_decode_packet(pkt, 0, &btn, &dx, &dy, &dz);
    if (btn != 0x04 || dx != 0 || dy != 0) return 4;
    // 5. wheel down: Z nibble 0xF (-1) -> one notch down
    pkt[0] = 0x08; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0x0F;
    mouse_decode_packet(pkt, 1, &btn, &dx, &dy, &dz);
    if (dz != -1) return 5;
    // 6. wheel up, and any positive magnitude collapses to +1
    pkt[3] = 0x01; mouse_decode_packet(pkt, 1, &btn, &dx, &dy, &dz); if (dz != 1) return 6;
    pkt[3] = 0x07; mouse_decode_packet(pkt, 1, &btn, &dx, &dy, &dz); if (dz != 1) return 7;
    // 7. with no wheel negotiated, the 4th byte is ignored -> dz always 0
    pkt[3] = 0x0F; mouse_decode_packet(pkt, 0, &btn, &dx, &dy, &dz); if (dz != 0) return 8;
    return 0;
}
