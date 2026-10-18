#!/usr/bin/env python3
"""Exercise the real terminal renderer and ANSI state with a recording framebuffer."""
from pathlib import Path
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


def main():
    with tempfile.TemporaryDirectory(prefix='nyx-terminal-palette-') as temp:
        directory = Path(temp)
        source = directory / 'test.c'
        source.write_text(HARNESS.replace('@ROOT@', ROOT.as_posix()))
        subprocess.run(['gcc', '-std=gnu99', '-O1', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
                        '-no-pie', '-Wl,--gc-sections', str(source), '-o', str(directory / 'test')], check=True)
        subprocess.run([str(directory / 'test')], check=True, timeout=30)


if __name__ == '__main__':
    main()
