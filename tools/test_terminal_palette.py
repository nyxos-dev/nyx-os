#!/usr/bin/env python3
"""Exercise terminal rendering, capture ownership and kernel pipe failure handling."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@ROOT@/kernel/gui/apps/terminal_win.c"
#include "@ROOT@/kernel/core/ansi.c"
extern int puts(const char*);
void memset_asm(void* p, uint8_t v, size_t n) { unsigned char* d=p; while(n--) *d++=v; }
void memcpy_asm(void* p, const void* q, size_t n) { unsigned char* d=p; const unsigned char* s=q; while(n--) *d++=*s++; }
static const char* queued;
static uint32_t lost;
static int reads;
static uint64_t closed;
int console_session_read(uint64_t id, char* out, int cap, uint32_t* omitted) {
    (void)id; reads++; int n=0;
    while (queued && *queued && n<cap) out[n++]=*queued++;
    *omitted=lost; lost=0; return n;
}
void console_session_close(uint64_t id) { closed=id; }
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
    static terminal_win_t other;
    memset_asm(&term,0,sizeof(term));
    term.cur_color=other.cur_color=TERM_DEFAULT_COLOR;
    const char* a="\033[3";
    while (*a) terminal_output_putchar(&term,*a++);
    terminal_output_putchar(&other,'B'); terminal_output_putchar(&other,'\n');
    a="1mA\n"; while (*a) terminal_output_putchar(&term,*a++);
    CHECK(term.lines[0][0]=='A' && term.colors[0][0]==VGA_RED);
    CHECK(other.lines[0][0]=='B' && other.colors[0][0]==TERM_DEFAULT_COLOR);
    term.session_id=42; term.input[0]='x'; term.input_len=term.cursor_pos=1;
    queued="BG\n"; term.capturing=1; int before=reads;
    CHECK(terminal_win_tick(&win)==0 && reads==before);
    term.capturing=0; CHECK(terminal_win_tick(&win)==1);
    CHECK(term.lines[1][0]=='B' && term.lines[1][1]=='G');
    CHECK(term.input[0]=='x' && term.input_len==1 && term.cursor_pos==1);
    queued="tail"; CHECK(terminal_win_tick(&win)==1 && term.output_len==4);
    terminal_win_draw(&win,0,0,8,32);
    CHECK(fg[0]==vga_to_rgb(VGA_RED));
    terminal_flush_pending(&term);
    CHECK(term.output_len==0 && !strncmp(term.lines[2],"tail",4));
    term.esc_state=2; lost=17;
    CHECK(terminal_win_tick(&win)==1 && term.esc_state==0);
    CHECK(!strncmp(term.lines[3],"[terminal: 17 bytes omitted;",27));
    terminal_win_close(&win); CHECK(closed==42 && term.session_id==0);
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
static char serial_bytes[65536], hook_bytes[65536];
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
    // Oversized captures fail, while the exact boundary remains usable.
    current = &owner; pipe_start(); console_capture_inherit(&child); current = &child;
    for (int i=0; i<4096; i++) screen_putchar('p');
    current = &owner; CHECK(pipe_stop() == 4096);
    pipe_start(); console_capture_inherit(&child); current = &child;
    for (int i=0; i<4097; i++) screen_putchar('p');
    current = &owner; CHECK(pipe_stop() == -1);
    CHECK(pipe_get_len() == 4096);
    pipe_start(); CHECK(pipe_stop() == 0);
    pipe_start(); current = &unrelated;
    for (int i=0; i<4100; i++) screen_putchar('u');
    current = &owner; CHECK(pipe_stop() == 0);
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
    uint64_t sa=console_session_open(), sb=console_session_open();
    CHECK(sa && sb && sa!=sb);
    current=&owner; set_putchar_hook_session(hook,sa); console_capture_inherit(&child);
    current=&child; console_capture_inherit(&descendant);
    CHECK(child.console_session_id==sa && descendant.console_session_id==sa);
    current=&owner; set_putchar_hook(NULL); current=&child; screen_putchar('a');
    current=&owner; set_putchar_hook_session(hook,sb); console_capture_inherit(&later);
    current=&child; screen_putchar('b');
    clear_recording(); current=&later; screen_putchar('d');
    CHECK(hook_n==1 && hook_bytes[0]=='d');
    current=&owner; set_putchar_hook(NULL); current=&later; screen_putchar('e');
    char buffer[511]; uint32_t omitted=99;
    CHECK(console_session_read(sa,buffer,sizeof(buffer),&omitted)==2 && !omitted && buffer[0]=='a' && buffer[1]=='b');
    current=&descendant; screen_putchar('c');
    CHECK(console_session_read(sa,buffer,sizeof(buffer),&omitted)==1 && buffer[0]=='c');
    CHECK(console_session_read(sb,buffer,sizeof(buffer),&omitted)==1 && buffer[0]=='e');
    console_session_close(sa); uint64_t sc=console_session_open(); CHECK(sc && sc!=sa);
    current=&child; screen_putchar('x');
    CHECK(console_session_read(sc,buffer,sizeof(buffer),&omitted)==0 && !omitted);
    clear_recording(); current=&later;
    for (int i=0;i<8192+17;i++) screen_putchar('q');
    CHECK(serial_n==8192+17);
    CHECK(console_session_read(sb,NULL,5,&omitted)==0 && !omitted);
    CHECK(console_session_read(sb,buffer,0,&omitted)==0 && !omitted);
    int retained=0, n;
    while ((n=console_session_read(sb,buffer,sizeof(buffer),&omitted))) {
        retained+=n;
        for (int i=0;i<n;i++) CHECK(buffer[i]=='q');
        CHECK(omitted==(retained==8192 ? 17u : 0u));
    }
    CHECK(retained==8192 && !omitted);
    screen_putchar('z');
    CHECK(console_session_read(sb,buffer,sizeof(buffer),&omitted)==1 && buffer[0]=='z' && !omitted);
    for (int i=0;i<6000;i++) screen_putchar('a');
    for (int i=0;i<10;i++) CHECK(console_session_read(sb,buffer,500,&omitted)==500 && !omitted);
    for (int i=0;i<6000;i++) screen_putchar('b');
    retained=0;
    while ((n=console_session_read(sb,buffer,sizeof(buffer),&omitted))) {
        for (int i=0;i<n;i++) { CHECK(buffer[i]==(retained<1000 ? 'a' : 'b')); retained++; }
        CHECK(!omitted);
    }
    CHECK(retained==7000);
    console_session_close(sb); console_session_close(sc);
    uint64_t slots[32];
    for (int i=0;i<32;i++) { slots[i]=console_session_open(); CHECK(slots[i]); }
    CHECK(console_session_open()==0);
    for (int i=0;i<32;i++) console_session_close(slots[i]);
    session_sequence=~(uint64_t)0-1; sa=console_session_open(); CHECK(sa==~(uint64_t)0);
    console_session_close(sa); CHECK(console_session_open()==0);
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


PIPE_DISPATCH = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define O_CREAT 64
#define O_TRUNC 512
static int length, open_result, write_result;
static int calls, starts, stops, opens, writes, closes, checks;
static char command[256];
static char* shell_find_pipe(char* s) { return strchr(s, '|'); }
static void execute_one_command(const char* s) { calls++; snprintf(command,sizeof command,"%s",s); }
static void pipe_start(void) { starts++; }
static int pipe_stop(void) { stops++; return length; }
static const char* pipe_get_data(void) { return "data"; }
static int vfs_open(const char* path, int flags, int mode) {
    (void)path; (void)flags; (void)mode; opens++; return open_result;
}
static int vfs_pwrite(int fd, const void* data, uint32_t count, uint32_t offset) {
    (void)fd; (void)data; (void)count; (void)offset; writes++; return write_result;
}
static void vfs_close(int fd) { (void)fd; closes++; }
@SEGMENT@
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL pipe dispatch: %s\n",#x); return 1; } } while (0)
static void run(int len,int fd,int written) {
    calls=starts=stops=opens=writes=closes=0;
    length=len;open_result=fd;write_result=written;
    char segment[]="left | cat > protected.txt"; execute_segment(segment);
}
int main(void) {
    run(-1,3,4); CHECK(calls==1 && opens==0 && writes==0 && closes==0);
    CHECK(starts==1 && stops==1);
    run(4,-1,4); CHECK(calls==1 && opens==1 && writes==0 && closes==0);
    for (int n=-1;n<4;n++) {
        run(4,3,n); CHECK(calls==1 && writes==1 && closes==1);
    }
    run(4,3,5); CHECK(calls==1 && writes==1 && closes==1);
    run(0,3,0); CHECK(calls==2 && writes==0 && closes==1);
    CHECK(!strcmp(command,"cat > protected.txt /tmp/pipe"));
    run(4,3,4); CHECK(calls==2 && writes==1 && closes==1);
    run(4096,3,4096); CHECK(calls==2 && writes==1 && closes==1);
    calls=starts=stops=0; char plain[]="normal"; execute_segment(plain);
    CHECK(calls==1 && starts==0 && stops==0 && !strcmp(command,"normal"));
    char longcmd[256]; longcmd[0]='|'; memset(longcmd+1,'a',246);longcmd[247]=0;
    calls=starts=0;execute_segment(longcmd);CHECK(calls==0 && starts==0);
    longcmd[0]='|';longcmd[246]=0;calls=starts=0;execute_segment(longcmd);
    CHECK(calls==2 && starts==1 && strlen(command)==255);
    printf("PASS %d kernel pipe dispatch checks\n",checks);return 0;
}
'''


def pipe_dispatch_suite(directory, reference=None):
    source = (subprocess.check_output(['git','show',reference+':kernel/core/kernel.c'],cwd=ROOT,text=True)
              if reference else (ROOT/'kernel/core/kernel.c').read_text(encoding='utf-8'))
    start=source.index('static void execute_segment(char* seg) {')
    end=source.index('\nstatic int shell_dispatch_selftest',start)
    path=directory/'dispatch.c';binary=directory/'dispatch'
    path.write_text(PIPE_DISPATCH.replace('@SEGMENT@',source[start:end]),encoding='utf-8')
    subprocess.run(['gcc','-std=gnu99','-O1','-Wall','-Wextra','-Werror','-no-pie',
                    '-fsanitize=address,undefined',str(path),'-o',str(binary)],check=True)
    return subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-capture-ref')
    parser.add_argument('--compare-pipe-ref')
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
        result=pipe_dispatch_suite(directory)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        print(result.stdout.strip())
        if args.compare_pipe_ref:
            result=pipe_dispatch_suite(directory,args.compare_pipe_ref)
            assert result.returncode and 'FAIL pipe dispatch: calls==1 && opens==0' in result.stdout,result.stdout+result.stderr
            print('PASS baseline runs the right command after failed capture')


if __name__ == '__main__':
    main()
