#!/usr/bin/env python3
"""Exercise real desktop config/load/save and picker code with a faulting host VFS."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@COMPOSITOR@"
#include "@ROOT@/kernel/gui/core/wallpaper_win.c"
@PARSER@
extern int puts(const char*);
char g_login_home[64] = "/mnt/home/alice";
uint32_t get_ticks(void) { return 1; }
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) { return (r<<16)|(g<<8)|b; }
void memcpy_asm(void* dst, const void* src, size_t n) {
    unsigned char* d = dst; const unsigned char* s = src; while (n--) *d++ = *s++;
}
static char system_config[4096], alice[4096], bob[4096], ram[4096];
static int fault, writes, opens, closes, mounted_stats, disk_reads;
static char last_path[128];
static char* lookup(const char* path) {
    if (!strcmp(path, "/etc/nyx.conf")) return system_config;
    if (!strcmp(path, "/mnt/home/alice/nyx.conf") || !strcmp(path, "/home/alice/nyx.conf")) return alice;
    if (!strcmp(path, "/mnt/home/bob/nyx.conf")) return bob;
    if (!strcmp(path, "/ram/nyx.conf")) return ram;
    return NULL;
}
static const char* active_read;
int vfs_stat(const char* path, uint32_t* size, int* dir) {
    if (!strncmp(path, "/mnt/", 5)) mounted_stats++;
    char* data = lookup(path);
    if (!data || !data[0]) return -1;
    *size = fault == 7 ? 4096 : (uint32_t)strlen(data);
    *dir = fault == 8;
    return 0;
}
int vfs_open(const char* path, int flags, mode_t mode) {
    (void)flags; (void)mode; active_read = lookup(path);
    if (!active_read || fault == 9) return -1;
    opens++; return 1;
}
int vfs_read(int fd, void* buf, size_t cap) {
    (void)fd;
    int n = (int)strlen(active_read);
    if (n > (int)cap) n = (int)cap;
    memcpy(buf, active_read, n);
    return n;
}
int vfs_close(int fd) { (void)fd; closes++; return 0; }
static char* mounted(const char* sub) {
    char path[128]; snprintf(path, sizeof path, "/mnt%s", sub); return lookup(path);
}
static uint32_t resolve(const char* sub) { char* data = mounted(sub); return fault != 10 && data && data[0]; }
static uint32_t disk_size(const char* sub) { return fault == 7 ? 4096 : (uint32_t)strlen(mounted(sub)) + (fault == 9); }
static int disk_readdir(const char* sub, dirent_t* entries, uint32_t max) {
    (void)sub; (void)entries; (void)max; return fault == 8 ? 0 : -1;
}
static int disk_read(const char* sub, void* dst, uint32_t cap) {
    disk_reads++;
    char* data = mounted(sub); int n = (int)strlen(data);
    if (n > (int)cap) n = (int)cap;
    memcpy(dst, data, n);
    if (fault == 3) return -1;
    if (fault == 4) return n - 1;
    if (fault == 5) ((char*)dst)[0] = '?';
    if (fault == 6) ((char*)dst)[0] = 0;
    return n;
}
int vfs_write_file(const char* path, const void* src, uint32_t n) {
    writes++; strcpy(last_path, path);
    if (fault == 1) return -1;
    if (fault == 2) return (int)n - 1;
    char* data = lookup(path); if (!data || n >= 4096) return -1;
    memcpy(data, src, n); data[n] = 0; return (int)n;
}
static int disk_write(const char* sub, const void* src, uint32_t n) {
    (void)sub; (void)src; return (int)n;
}
static mount_entry_t mount = { .mount_point = "/mnt", .resolve = resolve,
    .get_size = disk_size, .read_file = disk_read, .write_file = disk_write, .readdir = disk_readdir };
mount_entry_t* vfs_find_mount(const char* path) {
    return !strncmp(path, "/mnt/", 5) ? &mount : NULL;
}
static int checks;
#define CHECK(expr) do { checks++; if (!(expr)) { puts("FAIL: " #expr); return 1; } } while (0)
static const char* message(void) { return toasts[0].body; }
static void clear_message(void) { for (int i = 0; i < NOTIFY_MAX; i++) toasts[i].used = 0; }
int main(void) {
    strcpy(system_config, "terminal_palette = nightfall\n");
    apply_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "nightfall"));
    strcpy(alice, "terminal_palette = classic\n"); apply_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "classic"));
    strcpy(alice, "terminal_palette = unknown\n"); apply_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "nightfall"));
    clear_message(); save_nyx_config();
    CHECK(strstr(alice, "terminal_palette = nightfall\n"));
    reset_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "classic"));
    apply_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "nightfall"));
    system_config[0] = 0;
    strcpy(g_login_home, "/mnt/home/bob"); apply_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "classic"));
    strcpy(g_login_home, "/mnt/home/alice"); apply_nyx_config();
    CHECK(!strcmp(terminal_palette_name(), "nightfall"));
    strcpy(system_config, "wallpaper = Plano\naccent = Azul\ngaps = 8\n");
    strcpy(alice, "accent = #123456\nwidget = off\nclock = 12h\nicons = on\nrounding = 0\nshadow = off\ntitle_align = center\npanel_tint = 42\nwallpaper_dim = 30\nborder = Rojo\nborder_inactive = Verde\n");
    apply_nyx_config();
    CHECK(wallpaper_style() == WP_STYLE_FLAT && wallpaper_base_color() == 0x123456);
    CHECK(g_gaps == 8 && !g_widget_on && g_clock_12h && g_desktop_icons_visible);
    CHECK(!g_corner_radius && !g_shadows && g_title_center && g_panel_tint == 42 && g_wallpaper_dim == 30);
    CHECK(g_border_color == wallpaper_color_rgb(7) && g_border_inactive == wallpaper_color_rgb(3));
    clear_message(); save_nyx_config();
    CHECK(!strcmp(last_path, "/mnt/home/alice/nyx.conf"));
    CHECK(!strcmp(message(), "Saved in your home on disk"));
    CHECK(strstr(alice, "accent = #123456\n") && strstr(alice, "gaps = 8\n"));
    CHECK(!strcmp(system_config, "wallpaper = Plano\naccent = Azul\ngaps = 8\n"));
    strcpy(g_login_home, "/mnt/home/bob"); apply_nyx_config();
    CHECK(wallpaper_base_color() == wallpaper_color_rgb(1) && !wallpaper_is_rgb_override());
    CHECK(g_widget_on && !g_widget_pos && !g_clock_12h && !g_desktop_icons_visible);
    CHECK(g_corner_radius == WIN_RADIUS && g_shadows && !g_title_center && !g_panel_tint && !g_wallpaper_dim);
    CHECK(!g_border_color && !g_border_inactive && g_gaps == 8);
    strcpy(g_login_home, "/mnt/home/alice"); apply_nyx_config();
    CHECK(wallpaper_base_color() == 0x123456 && !g_widget_on && g_panel_tint == 42);
    window_t win = { .x = 0, .y = 0 };
    wallpaper_win_click(&win, WP_OX + WP_SW + WP_GAP + 1, TITLE_H + WP_OY + 1, 1);
    CHECK(!wallpaper_is_rgb_override() && wallpaper_base_color() == wallpaper_color_rgb(1));
    CHECK(strstr(alice, "accent = Azul\n"));
    wallpaper_set_color_rgb(0x112233);
    wallpaper_win_click(&win, WP_RST_X + 1, TITLE_H + WP_RST_OY + 1, 1);
    CHECK(!wallpaper_is_rgb_override() && wallpaper_style() == WP_STYLE_NIGHTFALL);
    CHECK(wallpaper_base_color() == wallpaper_color_rgb(0) && g_gaps == 0 && !g_border_color);
    CHECK(g_widget_on && g_corner_radius == WIN_RADIUS && g_shadows && !g_panel_tint && !g_title_center && !g_wallpaper_dim);
    CHECK(!strcmp(terminal_palette_name(), "classic"));
    CHECK(strstr(alice, "terminal_palette = classic\n"));
    for (int i = 1; i <= 10; i++) {
        fault = i; clear_message(); save_nyx_config();
        CHECK(strstr(message(), "failed") != NULL);
        CHECK(strlen(message()) <= 30);
    }
    fault = 0;
    mount.write_file = NULL; clear_message(); int before = writes; save_nyx_config();
    CHECK(writes == before && strstr(message(), "failed")); mount.write_file = disk_write;
    strcpy(g_login_home, "/home/alice"); clear_message(); save_nyx_config();
    CHECK(!strcmp(message(), "Saved in RAM; lost on reboot") && opens == closes);
    strcpy(g_login_home, "relative"); before = writes; clear_message(); save_nyx_config();
    CHECK(writes == before && strstr(message(), "Could not save"));
    strcpy(g_login_home, "/mnt/home/alice/"); clear_message(); save_nyx_config();
    CHECK(!strcmp(last_path, "/mnt/home/alice/nyx.conf"));
    apply_config_text("gaps = 999999999999999999999999999999999999999999999999999999\npanel_tint = 99999999999999999999999\nrounding = 999999999999999999999999\nwallpaper_dim = 999999999999999999999999\n");
    CHECK(g_gaps == 64 && g_panel_tint == 100 && g_corner_radius == WIN_RADIUS_MAX && g_wallpaper_dim == 80);
    for (int i = 0; i <= 150; i++) {
        char text[32]; snprintf(text, sizeof text, "%d", i);
        CHECK(config_uint(text, 64) == (i < 64 ? i : 64));
    }
    CHECK(config_uint(NULL, 64) == 0 && config_uint("-1", 64) == 0 && config_uint("7px", 64) == 7);
    for (int i = 3; i <= 8; i++) {
        if (i == 5) continue; /* Changed text is valid text, not a transport failure. */
        strcpy(alice, "accent = #123456\npanel_tint = 22\nclock = 12h\n");
        fault = i; reset_nyx_config(); apply_nyx_config();
        CHECK(!wallpaper_is_rgb_override());
        CHECK(g_panel_tint == 0 && g_clock_12h == 0);
    }
    fault = 0; char image[2048];
    for (int i = 0; i < 2047; i++) alice[i] = ' ';
    alice[2047] = 0;
    CHECK(read_nyx_config("/mnt/home/alice/nyx.conf", image, sizeof image) == 2047);
    alice[2047] = ' '; alice[2048] = 0; before = disk_reads;
    CHECK(read_nyx_config("/mnt/home/alice/nyx.conf", image, sizeof image) == -2 && disk_reads == before);
    CHECK(mounted_stats == 0); /* stat/open would allocate an unbounded mounted image. */
    char result[64]; snprintf(result, sizeof result, "PASS %d desktop config checks", checks); puts(result);
    return 0;
}
'''

def main():
    source = (ROOT / 'kernel/core/kernel.c').read_text()
    start = source.index('int nyxconf_get(')
    end = source.index('\nint nyxconf_selftest(', start)
    with tempfile.TemporaryDirectory(prefix='nyx-desktop-config-') as temp:
        directory = Path(temp)
        harness = directory / 'test.c'
        compositor = directory / 'compositor.c'
        text = (ROOT / 'kernel/gui/core/compositor.c').read_text()
        # Retain the real notification queue; omit its immediate graphical repaint.
        repaint = 'compositor_redraw_now();       // show it immediately (safe re-entrantly)'
        assert text.count(repaint) == 1
        compositor.write_text(text.replace(repaint, '/* Host test: no framebuffer repaint. */'))
        harness.write_text(HARNESS.replace('@ROOT@', ROOT.as_posix()).replace('@COMPOSITOR@', compositor.as_posix()).replace('@PARSER@', source[start:end]))
        command = ['gcc', '-std=gnu99', '-O0', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                   '-ffunction-sections', '-fdata-sections', '-no-pie', '-Wl,--gc-sections',
                   '-I', str(ROOT / 'kernel/gui/core'), str(harness),
                   str(ROOT / 'kernel/gui/apps/terminal_win.c'), '-o', str(directory / 'test')]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise RuntimeError(built.stdout + built.stderr)
        subprocess.run([str(directory / 'test')], check=True, timeout=30)

if __name__ == '__main__':
    main()
