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
static uint8_t test_inb(uint16_t port) {
    if (port == 0x64) return (uint8_t)ready;
    if (port == 0x60) { ready = 0; return scancode; }
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
char serial_getchar_nonblock(void) { return 0; }
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
int main(void) {
    CHECK(kbd_control_selftest() == 0);
    CHECK(kbd_translate_selftest() == 0);
    for (int layout = 0; layout < 2; layout++) {
        keyboard_layout = layout;
        for (int ctrl_irq = 0; ctrl_irq < 2; ctrl_irq++)
            for (int char_irq = 0; char_irq < 2; char_irq++)
                for (int foreground = 0; foreground < 2; foreground++)
                    CHECK(sequence(ctrl_irq, char_irq, foreground) == 0);
    }
    printf("OK: US/ES Escape make/break and Ctrl keys through IRQ, polling and mixed paths; Ctrl-Z Undo/job signal; signals after unlock\n");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'kernel/drivers/input/keyboard.c')
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        raise SystemExit("Run this test on Linux x86_64 or WSL")
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="nyx-keyboard-") as temporary:
        work = Path(temporary)
        source = work / "keyboard_test.c"
        source.write_text(HARNESS.replace("@ROOT@", ROOT.as_posix()).replace("@SOURCE@", args.source.resolve().as_posix()), encoding="utf-8")
        flags = ["-std=gnu99", "-O2", "-ffreestanding", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                 "-ffunction-sections", "-fdata-sections", "-no-pie", "-Wl,--gc-sections",
                 "-I", str(ROOT / "kernel/drivers/input")]
        for command in ([*cc, *flags, str(source), "-o", str(work / "keyboard_test")],
                        [str(work / "keyboard_test")]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            if result.stdout:
                print(result.stdout.strip())


if __name__ == "__main__":
    main()
