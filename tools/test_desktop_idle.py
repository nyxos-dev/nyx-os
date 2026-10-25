#!/usr/bin/env python3
"""Exercise the compositor's actual idle decision with simulated input devices."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "kernel/gui/core/compositor.c"


def harness(source):
    start = source.index("while (!quit) {", source.index("void compositor_run(void)"))
    start = source.index("{", start) + 1
    body = source[start:source.index("kernel_poll_net();", start)]
    return r'''
#include <stdint.h>
#include <stdio.h>
static int mouse_x = 100, mouse_y = 200, mouse_z = 3;
static int x, y, z, buttons, queued, sleeps, wrong_delay;
static int drag_dmg_valid, drag_id, resize_id, drag_icon_idx;
static int ctx_menu_open, start_menu_open;
static int keyboard_has_input(void) { return queued; }
static int mouse_get_x(void) { return x; }
static int mouse_get_y(void) { return y; }
static int mouse_get_z(void) { return z; }
static int mouse_get_buttons(void) { return buttons; }
static void sleep(unsigned ms) { sleeps++; wrong_delay += ms != 5; }
static void iteration(uint8_t prev_btns) {
    (void)prev_btns;
''' + body + r'''
}
int main(void) {
    int failures = 0, cases = 0;
    /* Input contract: unchanged device state may rest regardless of UI mode.
     * Each transition must be processed without the optional idle delay. */
    const int previous[] = {0,1,2,3,0,1,2,1,0,0,0,0};
    const int current[]  = {0,1,2,3,1,0,1,3,0,0,0,0};
    for (int ui = 0; ui < 6; ui++) {
        drag_id = ui == 3; resize_id = ui == 4;
        drag_icon_idx = ui == 5 ? 0 : -1;
        start_menu_open = ui == 1; ctx_menu_open = ui == 2;
        for (int input = 0; input < 12; input++) {
            x = mouse_x + (input == 9); y = mouse_y - (input == 10);
            z = mouse_z + (input == 11); queued = input == 8;
            buttons = current[input]; sleeps = wrong_delay = 0;
            iteration((uint8_t)previous[input]);
            int expected = input < 4;
            if (sleeps != expected || wrong_delay || drag_dmg_valid) {
                failures++;
                fprintf(stderr, "idle decision ui=%d input=%d sleeps=%d expected=%d\n",
                        ui, input, sleeps, expected);
            }
            cases++;
        }
    }
    printf("desktop idle: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
'''


def run(source, expected_failure=False):
    with tempfile.TemporaryDirectory(prefix="nyx-desktop-idle-") as tmp:
        path = Path(tmp)
        (path / "test.c").write_text(harness(source), encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        result = subprocess.run([str(path / "test")], capture_output=True, text=True, timeout=10)
        print(result.stdout, end="")
        if result.returncode != int(expected_failure):
            raise AssertionError(result.stderr)
        if expected_failure:
            assert "idle decision" in result.stderr


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare-ref")
    args = parser.parse_args()
    run((ROOT / SOURCE).read_text(encoding="utf-8"))
    if args.compare_ref:
        baseline = subprocess.check_output(["git", "show", f"{args.compare_ref}:{SOURCE}"],
                                           cwd=ROOT, text=True, encoding="utf-8")
        run(baseline, expected_failure=True)
