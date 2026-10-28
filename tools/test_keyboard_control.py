#!/usr/bin/env python3
"""Run the real IRQ/polling keyboard paths with simulated ports and lock state.

Linux x86_64 or WSL only. Checks shared Ctrl translation and signal delivery after
unlocking; it does not simulate IRQ scheduling or test physical PS/2 hardware.
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile
from test_process_reap import function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include "@ROOT@/kernel/core/kernel.h"
#include "@ROOT@/kernel/core/spinlock.h"
static uint8_t test_inb(uint16_t port);
static uint64_t test_lock(spinlock_t* lock);
static void test_unlock(spinlock_t* lock, uint64_t flags);
#define inb test_inb
#define spin_lock_irqsave test_lock
#define spin_unlock_irqrestore test_unlock
#include "@SOURCE@"
uint32_t g_foreground_pid;

static int ready, bad_lock, signal_count, last_signal;
static uint8_t scancode;
static unsigned char serial_byte;
static int serial_ready;
static uint8_t test_inb(uint16_t port) {
    if (port == 0x64) return (uint8_t)ready;
    if (port == 0x3FD) return (uint8_t)serial_ready;
    if (port == 0x60) { if (!kbd_lock.locked) bad_lock = 1; ready = 0; return scancode; }
    return 0;
}
static uint64_t test_lock(spinlock_t* lock) {
    if (lock->locked) bad_lock = 1;
    lock->locked = 1;
    return 0x200;
}
static void test_unlock(spinlock_t* lock, uint64_t flags) {
    if (!lock->locked || flags != 0x200) bad_lock = 1;
    lock->locked = 0;
}
void signal_send_foreground(int sig) {
    if (kbd_lock.locked) bad_lock = 1;
    signal_count++; last_signal = sig;
}
char serial_getchar_nonblock(void) { char c = (char)serial_byte; serial_byte = 0; serial_ready = 0; return c; }
static int key(uint8_t sc, int irq) {
    ready = 1; scancode = sc;
    if (irq) keyboard_irq_handler(NULL);
    return (unsigned char)getchar_poll();
}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
static int sequence(int ctrl_irq, int char_irq, int foreground) {
    signal_count = 0; last_signal = 0;
    g_foreground_pid = foreground ? 42 : 0;
    CHECK(key(0x01, char_irq) == 27);          /* Escape make */
    CHECK(key(0x81, char_irq) == 0);           /* Escape break */
    CHECK(key(0x1D, ctrl_irq) == 0);           /* Ctrl down */
    CHECK(key(0x01, char_irq) == 27);
    CHECK(key(0x81, char_irq) == 0);
    CHECK(key(0x20, char_irq) == 4);           /* Ctrl-D */
    CHECK(signal_count == 0);
    CHECK(key(0x2A, ctrl_irq) == 0);           /* Shift down */
    CHECK(key(0x01, char_irq) == 27);
    CHECK(key(0x81, char_irq) == 0);
    CHECK(key(0x20, char_irq) == 4);           /* Ctrl-Shift-D */
    CHECK(key(0xAA, ctrl_irq) == 0);           /* Shift up */
    CHECK(key(0x2E, char_irq) == (foreground ? 0 : 3));
    CHECK(signal_count == foreground && last_signal == (foreground ? SIGINT : 0));
    CHECK(key(0x2C, char_irq) == (foreground ? 0 : 26)); /* Ctrl-Z: job signal or client Undo */
    CHECK(signal_count == 2 * foreground && last_signal == (foreground ? SIGTSTP : 0));
    CHECK(key(0x18, char_irq) == 15);          /* Ctrl-O */
    CHECK(key(0x2D, char_irq) == 24);          /* Ctrl-X */
    CHECK(key(0x9D, ctrl_irq) == 0);           /* Ctrl up */
    CHECK(key(0x20, char_irq) == 'd');
    CHECK(key(0xE0, ctrl_irq) == 0);
    CHECK(key(0x38, ctrl_irq) == 0);           /* AltGr down */
    CHECK(key(0x01, char_irq) == 27);
    CHECK(key(0x81, char_irq) == 0);
    CHECK(key(0xE0, ctrl_irq) == 0);
    CHECK(key(0xB8, ctrl_irq) == 0);           /* AltGr up */
    CHECK(signal_count == 2 * foreground && !bad_lock && !kbd_lock.locked);
    return 0;
}
static void irq_sc(unsigned char sc) {
    ready = 1; scancode = sc; keyboard_irq_handler(NULL);
}
static void irq_key(unsigned char sc, int extended) {
    if (extended) irq_sc(0xE0);
    irq_sc(sc);
    if (extended) irq_sc(0xE0);
    irq_sc(sc | 0x80);
}
static int ordering(void) {
    keyboard_layout = 0; g_foreground_pid = 0;
    irq_key(0x1E, 0); irq_key(0x4B, 1); irq_key(0x2C, 0);
    CHECK(getkey_poll() == 'a'); CHECK(getkey_poll() == KEY_LEFT);
    CHECK(getkey_poll() == 'z'); CHECK(getkey_poll() == 0);
    irq_key(0x48, 1);
    CHECK(keyboard_has_input() == 1);
    CHECK(getkey_poll() == KEY_UP); CHECK(keyboard_has_input() == 0);
    /* Wrap repeatedly, including a full ring that rejects the newest key. */
    for (int cycle = 0; cycle < 8; cycle++) {
        for (int i = 0; i < 255; i++) irq_key(i % 2 ? 0x4B : 0x1E, i % 2);
        irq_key(0x2C, 0);
        for (int i = 0; i < 255; i++) CHECK(getkey_poll() == (i % 2 ? KEY_LEFT : 'a'));
        CHECK(getkey_poll() == 0);
    }
    /* Byte-only login/canonical readers discard navigation; never leak it later. */
    irq_key(0x4B, 1); irq_key(0x1E, 0); irq_key(0x3B, 0);
    CHECK(getchar_poll() == 'a'); CHECK(getchar_poll() == 0);
    CHECK(getkey_poll() == 0);
    keyboard_layout = 1;
    irq_key(0x27, 0); CHECK((unsigned char)getchar_poll() == 0xF1);
    irq_key(0x27, 0); CHECK(getkey_poll() == (char)0xF1);
    irq_sc(0xE0); irq_sc(0x38); irq_key(0x06, 0);
    CHECK((unsigned char)getchar_poll() == 0x80); /* AltGr byte overlaps KEY_UP */
    irq_sc(0xE0); irq_sc(0xB8);
    keyboard_layout = 0;
    /* Buffered input wins over the polling fallback, in one critical section. */
    irq_key(0x1E, 0); ready = 1; scancode = 0x2C;
    CHECK(getkey_poll() == 'a' && ready == 1);
    CHECK(getkey_poll() == 'z' && ready == 0);
    ready = 1; scancode = 0xE0; CHECK(getkey_poll() == 0);
    ready = 1; scancode = 0x4B; CHECK(getkey_poll() == KEY_LEFT);
    ready = 0x21; scancode = 0x1E; /* AUX packet belongs to the mouse. */
    CHECK(!keyboard_has_input()); CHECK(getkey_poll() == 0 && ready == 0x21);
    keyboard_irq_handler(NULL); CHECK(ready == 0x21); ready = 0;
    serial_byte = 0x7F; serial_ready = 1;
    CHECK(keyboard_has_input()); CHECK(getchar_poll() == '\b');
    serial_byte = 's'; serial_ready = 1;
    irq_key(0x1E, 0); CHECK(getkey_poll() == 'a'); CHECK(getkey_poll() == 's');
    serial_byte = 0xF1; serial_ready = 1; CHECK(getkey_poll() == (char)0xF1);
    /* Cooked reads do not consume physical press/release events. */
    while (keyboard_next_event() != -1) { }
    irq_key(0x1E, 0); irq_key(0x4B, 1);
    CHECK(getkey_poll() == 'a'); CHECK(getkey_poll() == KEY_LEFT);
    CHECK(keyboard_next_event() == 0x11E); CHECK(keyboard_next_event() == 0x1E);
    CHECK(keyboard_next_event() == 0x1CB); CHECK(keyboard_next_event() == 0xCB);
    CHECK(keyboard_next_event() == -1);
    CHECK(!bad_lock && !kbd_lock.locked);
    return 0;
}
static int modifier_events(void) {
    keyboard_layout=0;g_foreground_pid=0;
    unsigned mods=99;
    irq_sc(0x1D);irq_key(0x02,0);irq_sc(0x9D);
    CHECK(getkey_poll_with_modifiers(&mods)=='1' && mods==1 && !is_ctrl_pressed());
    irq_sc(0x38);irq_key(0x3E,0);irq_sc(0xB8);
    CHECK(getkey_poll_with_modifiers(&mods)==KEY_F4 && mods==2 && !is_alt_pressed());
    // Modifier identity belongs to each queued key, not the latest key or live state.
    irq_key(0x02,0);irq_sc(0x1D);irq_key(0x02,0);irq_sc(0x38);irq_key(0x03,0);
    irq_sc(0x9D);irq_key(0x04,0);irq_sc(0xB8);irq_key(0x05,0);
    const int codes[]={'1','1','2','3','4'}, flags[]={0,1,3,2,0};
    for(int j=0;j<5;j++) CHECK(getkey_poll_with_modifiers(&mods)==codes[j] && mods==(unsigned)flags[j]);
    CHECK(getkey_poll_with_modifiers(&mods)==0 && mods==0);
    // Byte-only readers discard navigation and the discarded key's modifier metadata.
    irq_sc(0x38);irq_key(0x4B,1);irq_sc(0xB8);irq_key(0x1E,0);
    CHECK(getchar_poll()=='a');CHECK(getkey_poll_with_modifiers(&mods)==0 && mods==0);
    // Legacy key API and serial fallback never expose packed metadata.
    irq_sc(0x1D);irq_key(0x04,0);irq_sc(0x9D);CHECK(getkey_poll()=='3');
    irq_sc(0x38);serial_byte='4';serial_ready=1;
    CHECK(getkey_poll_with_modifiers(&mods)=='4' && mods==0 && is_alt_pressed());irq_sc(0xB8);
    for(int cycle=0;cycle<8;cycle++) {
        for(int j=0;j<255;j++) { if(j%2)irq_sc(0x1D);irq_key(0x02,0);if(j%2)irq_sc(0x9D); }
        irq_sc(0x38);irq_key(0x3E,0);irq_sc(0xB8);
        for(int j=0;j<255;j++) CHECK(getkey_poll_with_modifiers(&mods)=='1' && mods==(unsigned)(j%2));
        CHECK(getkey_poll_with_modifiers(&mods)==0 && mods==0);
    }
    for (int layout=0;layout<2;layout++) {
        keyboard_layout=layout;
        for (int irq=0;irq<2;irq++) {
            CHECK(key(0x1D,irq)==0);
            ready=1;scancode=0x0F;if(irq)keyboard_irq_handler(NULL);
            CHECK(getkey_poll_with_modifiers(&mods)=='\t' && mods==KEY_MOD_CTRL);
            CHECK(key(0x9D,irq)==0);
        }
    }
    keyboard_layout=0;
    // Later physical changes cannot retroactively turn a queued plain key into a chord.
    irq_key(0x02,0);irq_sc(0x1D);
    CHECK(getkey_poll_with_modifiers(&mods)=='1' && mods==0 && is_ctrl_pressed());
    irq_sc(0x9D);
    // Extended Ctrl and navigation preserve their snapshot after release.
    irq_sc(0xE0);irq_sc(0x1D);irq_key(0x49,1);irq_sc(0xE0);irq_sc(0x9D);
    CHECK(getkey_poll_with_modifiers(&mods)==KEY_PGUP && mods==KEY_MOD_CTRL && !is_ctrl_pressed());
    CHECK(getkey_poll_with_modifiers(NULL)==0);
    CHECK(!bad_lock && !kbd_lock.locked);
    puts("PASS queued modifiers: delayed chords, mixed queue, legacy/serial/byte readers, 8 full wraps");
    return 0;
}
int main(void) {
    CHECK(modifier_events()==0);
    CHECK(kbd_control_selftest() == 0);
    CHECK(kbd_translate_selftest() == 0);
    for (int layout = 0; layout < 2; layout++) {
        keyboard_layout = layout;
        for (int ctrl_irq = 0; ctrl_irq < 2; ctrl_irq++)
            for (int char_irq = 0; char_irq < 2; char_irq++)
                for (int foreground = 0; foreground < 2; foreground++)
                    CHECK(sequence(ctrl_irq, char_irq, foreground) == 0);
    }
    CHECK(ordering() == 0);
    printf("OK: ordered bytes/navigation, readiness, 8 full-ring wraps, byte-only readers, IRQ/poll, AUX, serial and independent events\n");
    printf("OK: US/ES Escape make/break and Ctrl keys through IRQ, polling and mixed paths; Ctrl-Z Undo/job signal; signals after unlock\n");
    return 0;
}
'''


SETFG_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {uint32_t pid,reap_claimed;} process_t;
static process_t targets[]={{42,0},{UINT32_MAX,0}};
static process_t *slots[]={targets,targets+1};
static int process_count=2, sched_lock, locked, locks, retire, reads;
uint32_t g_foreground_pid;
static int checks;
#define VERIFY(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL foreground: %s\n",#x);exit(1);}} while(0)
static uint64_t __attribute__((unused)) spin_lock_irqsave(int *lock) {
    VERIFY(lock==&sched_lock && !locked);locked=1;locks++;return 73;
}
static void __attribute__((unused)) spin_unlock_irqrestore(int *lock,uint64_t flags) {
    VERIFY(lock==&sched_lock && locked && flags==73);locked=0;
    if(retire){slots[0]=NULL;retire=0;}
}
static process_t **table_read(void) {
    reads++;
    if(retire && !locked && reads==2)slots[0]=NULL;
    return slots;
}
#define process_table table_read()
@FIND@
@SETFG@
static int setfg_call(uint64_t a1) {
@BODY@
}
int main(void) {
    const uint32_t low[]={0,42,UINT32_MAX,99};
    const uint32_t high[]={1,2,0x7fffffff,0x80000000,0xffffffff};
    for(unsigned i=0;i<sizeof(high)/sizeof(high[0]);i++)
        for(unsigned j=0;j<sizeof(low)/sizeof(low[0]);j++) {
            g_foreground_pid=42;locks=reads=0;
            uint64_t pid=((uint64_t)high[i]<<32)|low[j];
            VERIFY(setfg_call(pid)==-1);
            VERIFY(g_foreground_pid==42);
            VERIFY(!locks && !reads);
        }
    for(unsigned i=0;i<sizeof(low)/sizeof(low[0]);i++) {
        g_foreground_pid=42;locks=reads=0;
        int valid=low[i]!=99;
        VERIFY(setfg_call(low[i])==(valid?0:-1));
        VERIFY(g_foreground_pid==(valid?low[i]:42));
    }
#if TEST_LIFETIME
    targets[0].reap_claimed=1;g_foreground_pid=99;
    VERIFY(setfg_call(42)==-1);VERIFY(g_foreground_pid==99 && !locked);
    targets[0].reap_claimed=0;
    for(int i=0;i<512;i++) {
        slots[0]=targets;reads=locks=0;retire=1;g_foreground_pid=99;
        VERIFY(setfg_call(42)==0);VERIFY(g_foreground_pid==42 && !locked && locks==1);
        VERIFY(slots[0]==NULL);VERIFY(setfg_call(42)==-1);VERIFY(g_foreground_pid==42);
        VERIFY(setfg_call(0)==0);VERIFY(g_foreground_pid==0 && !locked);
    }
#endif
    printf("PASS %d foreground PID width, target and retirement checks\n",checks);
    return 0;
}
"""

def setfg_suite(work, cc, reference=None):
    code=(subprocess.check_output(['git','show',reference+':kernel/core/syscall.c'],cwd=ROOT,text=True)
          if reference else (ROOT/'kernel/core/syscall.c').read_text(encoding='utf-8'))
    start=code.index('case SYS_SETFG: {')+len('case SYS_SETFG: {')
    end=code.index('\n        case SYS_GETTIMEOFDAY:',start)
    body=code[start:end].rsplit('}',1)[0]
    process=(subprocess.check_output(['git','show',reference+':kernel/proc/process.c'],cwd=ROOT,text=True)
             if reference else (ROOT/'kernel/proc/process.c').read_text(encoding='utf-8'))
    protected='int process_set_foreground(' in process
    helper=function(process,'int process_set_foreground(') if protected else ''
    harness=SETFG_HARNESS.replace('@FIND@',function(process,'process_t* find_process(')).replace('@SETFG@',helper)
    src=work/('setfg-old.c' if reference else 'setfg.c')
    src.write_text('#define TEST_LIFETIME '+str(int(protected))+'\n'+harness.replace('@BODY@',body),encoding='utf-8')
    binary=src.with_suffix('')
    subprocess.run([*cc,'-std=gnu99','-Wall','-Wextra','-Werror','-O1','-no-pie',
                    '-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
    return subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'kernel/drivers/input/keyboard.c')
    parser.add_argument('--compare-setfg-ref')
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nyx-keyboard-") as temporary:
        work = Path(temporary)
        source = work / "keyboard_test.c"
        source.write_text(HARNESS.replace("@ROOT@", ROOT.as_posix()).replace("@SOURCE@", args.source.resolve().as_posix()), encoding="utf-8")
        flags = ["-std=gnu99", "-O2", "-ffreestanding", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                 "-ffunction-sections", "-fdata-sections", "-no-pie", "-fsanitize=address,undefined", "-Wl,--gc-sections",
                 "-I", str(ROOT / "kernel/drivers/input")]
        for command in ([*cc, *flags, str(source), "-o", str(work / "keyboard_test")],
                        [str(work / "keyboard_test")]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            if result.stdout:
                print(result.stdout.strip())
        result=setfg_suite(work,cc)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        print(result.stdout.strip())
        if args.compare_setfg_ref:
            result=setfg_suite(work,cc,args.compare_setfg_ref)
            assert result.returncode and 'FAIL foreground: setfg_call(pid)==-1' in result.stderr,result.stdout+result.stderr
            print('PASS baseline accepts truncated foreground PID')



if __name__ == "__main__":
    main()
