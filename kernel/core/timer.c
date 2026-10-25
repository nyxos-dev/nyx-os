// ============================================================
// timer.c - Temporizador del sistema NyxOS (PIT a 1000 Hz, IRQ)
// ============================================================
#include "kernel.h"

#define PIT_CHANNEL0_RATE_GENERATOR 0x34

static uint32_t timer_ticks = 0;
static uint32_t timer_frequency = 1000;
static uint32_t pit_divisor = 0;

// Set while the CPU is halted in an idle wait (see sleep()); read by the CPU-utilization
// accountant in irq_scheduler_tick. In NyxOS's cooperative desktop a sleeping task keeps
// state PROC_RUN, so this flag — not the task state — is the true "CPU is idle" signal.
volatile int g_cpu_idle = 0;

// ---- Uptime from the wall clock, NOT the tick counter --------------------------------------
// tick_count is incremented by the PIT IRQ, so it STALLS whenever interrupts are masked (cli) —
// heavily during boot and the cli-driven login screen (measured: ~68 of every 1000 ms counted
// there). Deriving uptime from it under-reports real elapsed time by however long the user sat
// at the login prompt. The RTC keeps real time regardless of cli, so uptime = now - boot in
// wall-clock seconds is honest. tick_count still drives scheduling/sleep (relative, desktop-time)
// where the loss doesn't matter.
static uint64_t boot_epoch_sec = 0;
static int      boot_epoch_set = 0;

// Seconds since 1970-01-01 for a civil date/time (days-from-civil, Hinnant's algorithm;
// proleptic Gregorian, UTC). PURE so a KAT can pin it against known Unix-epoch vectors. Public
// (declared in kernel.h): the single epoch-conversion helper shared by uptime, SYS_GETTIMEOFDAY
// and `totp`, so the civil-days maths lives in exactly one place. Pinned by uptime_epoch_selftest.
uint64_t civil_to_epoch(int y, int m, int d, int h, int mi, int s) {
    if (m < 1) m = 1;
    if (m > 12) m = 12;
    int yy = y - (m <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;      // days since 1970-01-01
    return (uint64_t)days * 86400 + (uint64_t)h * 3600 + (uint64_t)mi * 60 + (uint64_t)s;
}

static uint64_t rtc_epoch_seconds(void) {
    rtc_time_t t; rtc_read_time(&t);
    return civil_to_epoch((int)t.year, (int)t.month, (int)t.day,
                          (int)t.hour, (int)t.minute, (int)t.second);
}

// KAT: pins civil_to_epoch against well-known Unix timestamps (leap day, Y2038, epoch), so the
// wall-clock uptime it backs stays correct. 0 = PASS, else the failing case number.
int uptime_epoch_selftest(void) {
    if (civil_to_epoch(1970, 1, 1, 0, 0, 0)  != 0ULL)          return 1;
    if (civil_to_epoch(1970, 1, 2, 0, 0, 0)  != 86400ULL)      return 2;
    if (civil_to_epoch(2000, 1, 1, 0, 0, 0)  != 946684800ULL)  return 3;
    if (civil_to_epoch(2020, 2, 29, 0, 0, 0) != 1582934400ULL) return 4;   // leap day
    if (civil_to_epoch(2021, 1, 1, 0, 0, 0)  != 1609459200ULL) return 5;
    if (civil_to_epoch(2038, 1, 19, 3, 14, 7) != 2147483647ULL) return 6;  // Y2038 boundary
    if (civil_to_epoch(2000, 1, 1, 1, 1, 1) - civil_to_epoch(2000, 1, 1, 0, 0, 0) != 3661ULL) return 7;
    // Leap-rule edges (this helper now also backs SYS_GETTIMEOFDAY + `totp`, so pin them here):
    // 2100 is a century, NOT /400 -> no 29 Feb, so 28 Feb -> 1 Mar is one day...
    if (civil_to_epoch(2100, 3, 1, 0, 0, 0) - civil_to_epoch(2100, 2, 28, 0, 0, 0) != 86400ULL)  return 8;
    // ...but 2000 IS /400 -> 29 Feb exists, so the same span is two days.
    if (civil_to_epoch(2000, 3, 1, 0, 0, 0) - civil_to_epoch(2000, 2, 28, 0, 0, 0) != 172800ULL) return 9;
    if (civil_to_epoch(2026, 8, 31, 0, 0, 0) != 1788134400ULL) return 10;   // a present-day wall-clock date
    return 0;
}

// Capture the boot wall-clock once, as early in kernel_main as the RTC is readable.
void uptime_mark_boot(void) { boot_epoch_sec = rtc_epoch_seconds(); boot_epoch_set = 1; }

// Honest uptime: real wall-clock seconds since boot (the one source every uptime reader uses).
uint32_t get_uptime_seconds(void) {
    if (!boot_epoch_set) return 0;
    uint64_t now = rtc_epoch_seconds();
    return now > boot_epoch_sec ? (uint32_t)(now - boot_epoch_sec) : 0;
}

void init_timer(uint32_t frequency) {
    timer_frequency = frequency;
    timer_ticks = 0;
    pit_divisor = 1193180 / frequency;

    // Channel 0, low/high divisor bytes, mode 2, binary counter.
    outb(0x43, PIT_CHANNEL0_RATE_GENERATOR);
    outb(0x40, (uint8_t)(pit_divisor & 0xFF));
    outb(0x40, (uint8_t)((pit_divisor >> 8) & 0xFF));

    printf("[TIMER] %d Hz (interrupt-driven)\n", frequency);
}

uint32_t get_ticks(void) {
    return tick_count;
}

void sleep(uint32_t milliseconds) {
    uint32_t ticks_to_wait = (milliseconds * timer_frequency) / 1000;
    uint32_t target = tick_count + ticks_to_wait;

    // With the preemptive scheduler running, block on the timer wait queue instead
    // of busy-waiting: mark ourselves PROC_BLOCKED with a wake_tick and yield, so
    // the scheduler runs other threads and wakes us (irq_scheduler_tick) once
    // tick_count reaches the target. Only the compositor/kernel-thread contexts
    // call sleep(), all with interrupts enabled — never an IRQ handler.
    process_t* self = sched_is_enabled() ? get_current_process() : NULL;
    // Mark the CPU idle for the whole wait: NyxOS runs the desktop cooperatively (the preemptive
    // scheduler is usually off), so a sleeping task halts in ITS OWN context and keeps state
    // PROC_RUN — the only honest "the CPU is doing nothing" signal is this flag around the hlt.
    // The performance accountant (irq_scheduler_tick) reads it so an idle-yielding compositor
    // isn't miscounted as 100% busy.
    g_cpu_idle = 1;
    if (self) {
        for (;;) {
            // cli makes the deadline check + block atomic vs. the waking tick.
            __asm__ volatile("cli");
            if ((int32_t)(tick_count - target) >= 0) { __asm__ volatile("sti"); break; }
            self->wake_tick = target;
            self->state = PROC_BLOCKED;
            __asm__ volatile("sti; hlt");   // parked until the scheduler wakes us
        }
        self->wake_tick = 0;
    } else {
        // No scheduler yet (early boot): nothing else to run, just idle to the tick.
        while ((int32_t)(tick_count - target) < 0) __asm__ volatile("hlt");
    }
    g_cpu_idle = 0;
}