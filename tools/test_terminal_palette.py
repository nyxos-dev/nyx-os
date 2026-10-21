#!/usr/bin/env python3
"""Exercise the real terminal renderer and ANSI state with a recording framebuffer."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@ROOT@/kernel/gui/apps/terminal_win.c"
extern int puts(const char*);
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) { return (r<<16)|(g<<8)|b; }
static uint32_t fill, fg[2], bg[2];
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    (void)x; (void)y; (void)w; (void)h; fill = color;
}
void font_draw_char(uint32_t x, uint32_t y, unsigned char c, uint32_t f, uint32_t b) {
    (void)x; (void)c; if (y < 32) { fg[y/16] = f; bg[y/16] = b; }
}
static terminal_win_t term;
static int checks;
#define CHECK(expr) do { checks++; if (!(expr)) { puts("FAIL: " #expr); return 1; } } while (0)
int main(void) {
    static const uint32_t classic[16] = {
        0x000000,0x0000aa,0x00aa00,0x00aaaa,0xaa0000,0xaa00aa,0xaa5500,0xaaaaaa,
        0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff
    };
    window_t win = { .reserved = &term };
    term.line_count = 1; term.lines[0][0] = 'X';
    term.input[0] = 'i'; term.input_len = 1; term.cursor_pos = 1;
    for (int mode = 0; mode < 2; mode++) {
        terminal_set_palette(mode ? "nightfall" : "classic");
        uint32_t canvas = mode ? 0x181825 : 0;
        CHECK(vga_to_rgb(0) == canvas);
        for (int i = 0; i < 16; i++) {
            if (!mode) CHECK(vga_to_rgb(i) == classic[i]);
            else if (i) CHECK(vga_to_rgb(i) != canvas);
            CHECK(vga_to_rgb(i) == vga_to_rgb(i + 16));
            int sgr[3] = {0, (i < 8 ? 30 : 90) + (i & 7), (i < 8 ? 40 : 100) + ((i + 1) & 7)};
            term_apply_sgr(&term, sgr, 3);
            uint8_t expected = ansi2vga[i & 7] | (i >= 8 ? 8 : 0);
            uint8_t expected_bg = ansi2vga[(i + 1) & 7] | (i >= 8 ? 8 : 0);
            CHECK(term.cur_color == (expected | (expected_bg << 4)));
            int reverse = 7;
            term_apply_sgr(&term, &reverse, 1);
            CHECK(term.cur_color == (expected_bg | (expected << 4)));
            term_apply_sgr(&term, &reverse, 1);
            term.colors[0][0] = term.cur_color;
            terminal_win_draw(&win, 0, 0, 8, 32);
            CHECK(fill == canvas && fg[0] == vga_to_rgb(expected) && bg[0] == vga_to_rgb(expected_bg));
            CHECK(bg[1] == canvas && fg[1] == (mode ? vga_to_rgb(10) : 0x00ff00));
            term.screen_mode = 1; term.out_row = term.out_col = 0;
            terminal_win_draw(&win, 0, 0, 8, 16);
            CHECK(fg[0] == vga_to_rgb(expected_bg) && bg[0] == vga_to_rgb(expected));
            term.screen_mode = 0;
        }
        term.cursor_pos = 0;
        terminal_win_draw(&win, 0, 0, 8, 32);
        CHECK(fg[1] == canvas && bg[1] == (mode ? vga_to_rgb(10) : 0x00ff00));
        term.cursor_pos = 1;
    }
    terminal_set_palette("unknown"); terminal_set_palette(NULL);
    CHECK(!strcmp(terminal_palette_name(), "nightfall"));
    terminal_set_palette("classic");
    CHECK(!strcmp(terminal_palette_name(), "classic"));
    char result[64]; snprintf(result, sizeof result, "PASS %d terminal palette checks", checks); puts(result);
    return 0;
}
'''


CAPTURE_LOCK = r'''
#include <pthread.h>
extern int usleep(unsigned int);
static void host_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static __attribute__((unused)) uint64_t spin_lock_irqsave(spinlock_t* lock) { pthread_mutex_lock(lock); return 0; }
static __attribute__((unused)) void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    (void)flags; pthread_mutex_unlock(lock);
}
'''

CAPTURE_TEST = r'''
static __thread process_t* current;
process_t* get_current_process(void) { return current; }
static char serial_bytes[16384], hook_bytes[16384];
static int serial_n, hook_n, checks;
static uint16_t display[80*25];
void serial_putchar(char c) { serial_bytes[serial_n++] = c; }
static int hook(int c) { hook_bytes[hook_n++] = (char)c; return c; }
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL capture: %s\n", #x); return 1; } } while (0)
static void clear_recording(void) { serial_n = hook_n = 0; }
#ifndef BASELINE
static volatile int entered, release_hook, clear_started, clear_done;
static process_t concurrent_child, concurrent_owner;
static int blocking_hook(int c) {
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_hook, __ATOMIC_ACQUIRE)) usleep(1000);
    return c;
}
static void* producer(void* arg) {
    (void)arg; current = &concurrent_child; capture_putchar('x'); return NULL;
}
static void* remover(void* arg) {
    (void)arg; current = &concurrent_owner;
    __atomic_store_n(&clear_started, 1, __ATOMIC_RELEASE);
    set_putchar_hook(NULL);
    __atomic_store_n(&clear_done, 1, __ATOMIC_RELEASE); return NULL;
}
#endif
int main(void) {
    process_t owner = {.pid=42}, child = {.pid=43}, descendant = {.pid=44};
    process_t unrelated = {.pid=45}, later = {.pid=46};
    vga_buffer = display;
    current = &owner; set_putchar_hook(hook);
    screen_putchar('O'); CHECK(hook_n == 1 && hook_bytes[0] == 'O' && serial_n == 1);
    console_capture_inherit(&child); current = &child;
    console_capture_inherit(&descendant); screen_putchar('C');
    CHECK(hook_n == 2 && hook_bytes[1] == 'C' && serial_n == 2);
    current = &descendant; screen_putchar('D'); CHECK(hook_n == 3 && serial_n == 3);
    current = &owner; set_putchar_hook(NULL); set_putchar_hook(hook);
    clear_recording(); current = &child; screen_putchar('L');
    CHECK(hook_n == 0 && serial_n == 1 && serial_bytes[0] == 'L');
    current = &descendant; screen_putchar('E'); CHECK(hook_n == 0 && serial_n == 2);
    current = &unrelated; screen_putchar('U'); CHECK(hook_n == 0 && serial_n == 3);
    current = &owner; console_capture_inherit(&later);
    current = &later; screen_putchar('N'); CHECK(hook_n == 1 && hook_bytes[0] == 'N');
    // Active pipe admits its owner and inherited descendants, not older children.
    current = &owner; pipe_start(); console_capture_inherit(&child);
    current = &child; console_capture_inherit(&descendant);
    clear_recording(); screen_putchar('P');
    current = &descendant; screen_putchar('Q');
    CHECK(pipe_get_len() == 2 && hook_n == 0 && serial_n == 0);
    current = &unrelated; screen_putchar('U'); CHECK(pipe_get_len() == 2 && serial_n == 1 && hook_n == 0);
    current = &later; screen_putchar('T'); CHECK(pipe_get_len() == 2 && serial_n == 2 && hook_n == 1);
    current = &owner; CHECK(pipe_stop() == 2); CHECK(pipe_get_data()[0] == 'P' && pipe_get_data()[1] == 'Q');
    pipe_start(); clear_recording(); current = &child; screen_putchar('S');
    CHECK(pipe_get_len() == 0 && serial_n == 1 && hook_n == 1);
    current = &owner; screen_putchar('B'); CHECK(pipe_stop() == 1 && pipe_get_data()[0] == 'B');
    // Reusing a child PID never grants access to its previous capture.
    unrelated.pid = child.pid; current = &unrelated; clear_recording(); screen_putchar('R');
    CHECK(hook_n == 0 && serial_n == 1);
    current = &owner; set_putchar_hook(NULL); clear_recording(); current = &later;
    screen_putchar('Z'); CHECK(hook_n == 0 && serial_n == 1);
#ifndef BASELINE
    // Full kernel-pipe behavior stays bounded at its existing 4096-byte limit.
    current = &owner; pipe_start(); console_capture_inherit(&child); current = &child;
    for (int i=0; i<4100; i++) screen_putchar('p');
    current = &owner; CHECK(pipe_stop() == 4096);
    // No token wrap can make a stale descendant eligible again.
    capture_sequence = ~(uint64_t)0 - 1;
    set_putchar_hook(hook); console_capture_inherit(&child);
    CHECK(child.console_capture_id == ~(uint64_t)0);
    set_putchar_hook(NULL); set_putchar_hook(hook);
    CHECK(console_capture_id == 0); clear_recording(); current = &child;
    screen_putchar('W'); CHECK(hook_n == 0 && serial_n == 1);
    current = &owner; screen_putchar('o'); CHECK(hook_n == 1);
    set_putchar_hook(NULL); capture_sequence = 100;
    // Removing a hook waits for a callback already in flight on another CPU.
    concurrent_owner.pid=100; concurrent_child.pid=101; current=&concurrent_owner;
    set_putchar_hook(blocking_hook); console_capture_inherit(&concurrent_child);
    pthread_t writer, closer;
    CHECK(pthread_create(&writer,NULL,producer,NULL)==0);
    while (!__atomic_load_n(&entered,__ATOMIC_ACQUIRE)) usleep(1000);
    CHECK(pthread_create(&closer,NULL,remover,NULL)==0);
    while (!__atomic_load_n(&clear_started,__ATOMIC_ACQUIRE)) usleep(1000);
    usleep(20000); CHECK(!__atomic_load_n(&clear_done,__ATOMIC_ACQUIRE));
    __atomic_store_n(&release_hook,1,__ATOMIC_RELEASE);
    CHECK(pthread_join(writer,NULL)==0 && pthread_join(closer,NULL)==0);
    CHECK(clear_done && putchar_hook == NULL);
#endif
    printf("PASS %d console capture ownership checks\n",checks); return 0;
}
'''


def capture_suite(directory, reference=None):
    code = (subprocess.check_output(['git','show',reference+':kernel/drivers/video/screen.c'],cwd=ROOT,text=True)
            if reference else (ROOT/'kernel/drivers/video/screen.c').read_text(encoding='utf-8'))
    code = code[:code.index('\nint puts(')]
    code = code.replace('#include "../../core/kernel.h"', '#include "'+(ROOT/'kernel/core/kernel.h').as_posix()+'"\n'+CAPTURE_LOCK)
    code = code.replace('#include "../../core/spinlock.h"', '')
    code = code.replace('int putchar(int c)', 'int screen_putchar(int c)')
    code = code.replace('    outb(', '    host_outb(')
    if reference:
        code += '\n#define BASELINE\nvoid console_capture_inherit(process_t* child) {(void)child;}\n'
    source=directory/('capture-old.c' if reference else 'capture.c')
    source.write_text(code+CAPTURE_TEST,encoding='utf-8')
    binary=source.with_suffix('')
    subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-pthread','-no-pie',str(source),'-o',str(binary)],check=True)
    return subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-capture-ref')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-terminal-palette-') as temp:
        directory = Path(temp)
        source = directory / 'test.c'
        source.write_text(HARNESS.replace('@ROOT@', ROOT.as_posix()))
        subprocess.run(['gcc', '-std=gnu99', '-O1', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
                        '-no-pie', '-Wl,--gc-sections', str(source), '-o', str(directory / 'test')], check=True)
        subprocess.run([str(directory / 'test')], check=True, timeout=30)
        result=capture_suite(directory)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        print(result.stdout.strip())
        if args.compare_capture_ref:
            result=capture_suite(directory,args.compare_capture_ref)
            assert result.returncode and 'FAIL capture: hook_n == 0 && serial_n == 1' in result.stdout,result.stdout+result.stderr
            print('PASS baseline routes an expired child into the next terminal capture')


if __name__ == '__main__':
    main()
