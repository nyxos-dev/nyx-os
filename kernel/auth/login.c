#include "../core/kernel.h"
#include "login.h"
#include "auth.h"
#include "../drivers/video/font.h"
#include "../gui/core/nyx_logo.h"

#define LOGIN_MAX_ATTEMPTS 3   /* lock out (reboot) after this many failed logins */
#define LOGIN_MIN_PASS     4   /* mirrors auth.c AUTH_MIN_PASS for the sign-up UI  */

// The account that logged in (guest 'nyx' by default), read by the desktop
// taskbar to show "who am I" with a profile picture.
char g_login_user[32] = "nyx";
int  g_login_avatar = 0;
char g_login_home[64] = "/";     // this session's home directory (/home/<user>)

// Give a brand-new home directory some starter content: a couple of folders and a
// welcome note. Only run the first time the home is created (see setup_user_home),
// so it never clobbers a user's own files on a later login.
static void populate_home(const char* home) {
    char p[96];
    snprintf(p, sizeof(p), "%s/Documents", home); vfs_mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Downloads", home); vfs_mkdir(p, 0755);
    char welcome[512];
    int n = snprintf(welcome, sizeof(welcome),
        "Welcome to NyxOS, %s!\n\n"
        "This is your home directory (%s).\n"
        "  - Double-click a file here to open it in the Text Editor.\n"
        "  - Documents/ and Downloads/ are yours to fill.\n"
        "  - Click your badge (bottom-right) to change your picture or log out.\n",
        g_login_user, home);
    snprintf(p, sizeof(p), "%s/welcome.txt", home);
    vfs_write_file(p, welcome, (uint32_t)n);
}

// Ensure /home/<user> exists and record it as this session's home directory, so
// new Terminal and File Manager windows start there instead of at /. On the first
// login that creates it, seed it with starter folders + a welcome file.
static void setup_user_home(void) {
    // Prefer the PERSISTENT ext2 disk (mounted read-write at /mnt) so the user's home
    // and its files survive a reboot; fall back to the RAM /home if no disk is mounted.
    // On a first-ever login the home is created + seeded; on later boots the on-disk
    // home already exists (mkdir fails), so `fresh` is 0 and the user's files are kept.
    uint32_t msz = 0; int mdir = 0;
    int have_disk = (vfs_stat("/mnt", &msz, &mdir) == 0 && mdir);
    char path[64];
    if (have_disk) {
        vfs_mkdir("/mnt/home", 0755);          // idempotent (already there after first boot)
        snprintf(path, sizeof(path), "/mnt/home/%s", g_login_user);
    } else {
        vfs_mkdir("/home", 0755);
        snprintf(path, sizeof(path), "/home/%s", g_login_user);
    }
    int fresh = (vfs_mkdir(path, 0755) == 0);  // 0 => created now (first ever login for this user)
    strncpy(g_login_home, path, sizeof(g_login_home) - 1);
    g_login_home[sizeof(g_login_home) - 1] = '\0';
    // Sync the shell env HOME to the real session home so `~`, $HOME and programs launched
    // from the shell all agree with it (HOME was seeded to a fixed "/home/user" at init,
    // which did not match /home/<user> or /mnt/home/<user>).
    extern void shell_set_env(const char* name, const char* value);
    shell_set_env("HOME", g_login_home);
    if (fresh) populate_home(path);
}

// The current session's home directory node (or the root as a fallback), used as
// the initial working directory for new Terminal windows.
void* login_home_node(void) {
    void* n = g_login_home[0] ? vfs_path_node(g_login_home) : 0;
    return n ? n : vfs_root_node();
}

// The login background is a vertical gradient. Sampling its color at a given row
// lets the moon logo blend in seamlessly.
static uint32_t login_grad(uint32_t y, uint32_t fh) {
    uint32_t t = y * 255 / fh;
    uint32_t r = 16 + t * 22 / 255, g = 12 + t * 12 / 255, b = 30 + t * 26 / 255;
    if (r > 38) r = 38;
    if (g > 24) g = 24;
    if (b > 62) b = 62;
    return fb_rgb(r, g, b);
}


// ---- Profile pictures -----------------------------------------------------
// Four simple procedural "avatars": a coloured tile with a blocky face, each
// recognisably different. Drawn at any size (login picker ~46px, taskbar ~22px),
// so feature positions are fractions of `s`. Shared with the compositor taskbar.
void draw_avatar(int x, int y, int s, int id, int selected) {
    id &= 3;
    uint32_t col, eye = fb_rgb(28, 24, 40), white = fb_rgb(245, 245, 255);
    switch (id) {
        case 0:  col = fb_rgb(150, 120, 240); break;  // purple (nyx / moon)
        case 1:  col = fb_rgb(70, 150, 235);  break;  // blue   (smiley)
        case 2:  col = fb_rgb(80, 200, 130);  break;  // green  (cool)
        default: col = fb_rgb(240, 150, 60);  break;  // orange (cat)
    }
    fb_fill_rect(x - 3, y - 3, s + 6, s + 6, selected ? white : fb_rgb(70, 60, 90));  // ring
    if (id == 3) {                                   // cat: ears poke above the tile
        fb_fill_rect(x + s * 12 / 100, y - s * 12 / 100, s * 22 / 100, s * 20 / 100, col);
        fb_fill_rect(x + s * 66 / 100, y - s * 12 / 100, s * 22 / 100, s * 20 / 100, col);
    }
    fb_fill_rect(x, y, s, s, col);                   // tile

    int ew = s / 6; if (ew < 2) ew = 2;
    int lex = x + s * 27 / 100, rex = x + s * 58 / 100, ey = y + s * 33 / 100;
    int mw = s * 34 / 100, mx = x + (s - mw) / 2, my = y + s * 62 / 100;
    int mh = s / 12; if (mh < 2) mh = 2;

    switch (id) {
        case 0:  // moon: calm neutral face
            fb_fill_rect(lex, ey, ew, ew, eye);
            fb_fill_rect(rex, ey, ew, ew, eye);
            fb_fill_rect(mx, my, mw, mh, eye);
            break;
        case 1:  // smiley: eyes + a wide smile (bar with lowered ends)
            fb_fill_rect(lex, ey, ew, ew, eye);
            fb_fill_rect(rex, ey, ew, ew, eye);
            fb_fill_rect(mx, my, mw, ew, eye);
            fb_fill_rect(mx - ew / 2, my - ew / 2, ew, ew, eye);
            fb_fill_rect(mx + mw - ew / 2, my - ew / 2, ew, ew, eye);
            break;
        case 2:  // cool: a single visor bar over both eyes + small mouth
            fb_fill_rect(lex, ey, (rex + ew) - lex, ew, eye);
            fb_fill_rect(mx + mw / 4, my, mw / 2, mh, eye);
            break;
        default: // cat: eyes + a little nose
            fb_fill_rect(lex, ey, ew, ew, eye);
            fb_fill_rect(rex, ey, ew, ew, eye);
            fb_fill_rect(x + s / 2 - ew / 2, my, ew, ew, fb_rgb(235, 120, 150));
            break;
    }
}

const char* avatar_name(int id) {
    static const char* names[] = { "Moon", "Smiley", "Cool", "Cat" };
    return names[id & 3];
}

static void draw_field(int x, int y, int w, int active, const char* text, int masked) {
    uint32_t bd = active ? fb_rgb(150, 120, 240) : fb_rgb(68, 58, 92);
    uint32_t bg = active ? fb_rgb(52, 44, 80) : fb_rgb(40, 34, 56);
    fb_fill_rect(x, y, w, 20, bg);
    fb_fill_rect(x, y, w, 1, bd);
    fb_fill_rect(x, y + 19, w, 1, bd);
    fb_fill_rect(x, y, 1, 20, bd);
    fb_fill_rect(x + w - 1, y, 1, 20, bd);
    if (text && text[0]) {
        if (masked) {
            char m[64]; int len = 0;
            while (text[len]) len++;
            for (int i = 0; i < len && i < 63; i++) m[i] = '*';
            m[len < 63 ? len : 63] = '\0';
            font_draw_string(x + 4, y + 2, m, fb_rgb(200, 200, 220), bg);
        } else {
            font_draw_string(x + 4, y + 2, text, fb_rgb(200, 200, 220), bg);
        }
    }
}

#define BOX_W 380
#define BOX_H 300

// Rounded-corner helpers (fb_corner_inset / fb_fill_round_rect /
// fb_stroke_round_rect) live in fb.c, shared with the compositor.

// Repaint the whole login/create panel for the current state.
static void draw_panel(int px, int py, int mode, int field, int avatar,
                       const char* user, const char* pass, const char* msg) {
    uint32_t bg = fb_rgb(30, 24, 46);
    const int R = 8, HDR = 34;
    fb_fill_round_rect(px, py, BOX_W, BOX_H, R, bg);

    // Accent brand header carrying the title, a top-lit purple gradient rounded to
    // the card's top corners (transparent title so the gradient shows through). The
    // old flat title + divider lived in this same 34px band, so nothing below moves.
    for (int row = 0; row < HDR; row++) {
        int in = (row < R) ? fb_corner_inset(row, R) : 0;
        int t = (HDR > 1) ? row * 255 / (HDR - 1) : 0;
        uint32_t r = (uint32_t)(150 + (112 - 150) * t / 255);
        uint32_t g = (uint32_t)(108 + ( 74 - 108) * t / 255);
        uint32_t b = (uint32_t)(224 + (180 - 224) * t / 255);
        fb_fill_rect(px + in, py + row, BOX_W - 2 * in, 1, fb_rgb(r, g, b));
    }
    const char* title = mode ? "Create your account" : "NyxOS Login";
    font_draw_string_trans(px + (BOX_W - (int)strlen(title) * 8) / 2,
                           py + (HDR - 16) / 2, title, fb_rgb(255, 255, 255));   /* 16 = font height */

    font_draw_string(px + 20, py + 46, "Username:", fb_rgb(180, 165, 215), bg);
    draw_field(px + 20, py + 62, BOX_W - 40, field == 0, user, 0);
    font_draw_string(px + 20, py + 98, "Password:", fb_rgb(180, 165, 215), bg);
    draw_field(px + 20, py + 114, BOX_W - 40, field == 1, pass, 1);

    if (mode) {
        font_draw_string(px + 20, py + 150, "Choose a profile picture:", fb_rgb(180, 165, 215), bg);
        int s = 46, gap = 30, total = AVATAR_COUNT * s + (AVATAR_COUNT - 1) * gap;
        int ax = px + (BOX_W - total) / 2, ay = py + 172;
        for (int i = 0; i < AVATAR_COUNT; i++) {
            int cx = ax + i * (s + gap);
            draw_avatar(cx, ay, s, i, i == avatar);
            char lbl[8]; snprintf(lbl, sizeof(lbl), "%d.%s", i + 1, avatar_name(i));
            uint32_t lc = (i == avatar) ? fb_rgb(255, 255, 150) : fb_rgb(150, 150, 170);
            font_draw_string(cx + (s - (int)strlen(lbl) * 8) / 2, ay + s + 6, lbl, lc, bg);
        }
        if (field == 2)
            font_draw_string(px + 20, py + 250, "Press 1-4 to pick, Enter to create.", fb_rgb(150, 200, 150), bg);
    } else {
        // Centered and short enough to stay inside the box (a longer, left-aligned
        // line overflowed the right edge and left artifacts when toggling modes).
        const char* h = "New here? Press TAB to create an account.";
        font_draw_string(px + (BOX_W - (int)strlen(h) * 8) / 2, py + 160, h, fb_rgb(150, 150, 180), bg);
    }

    if (msg && msg[0])
        font_draw_string(px + (BOX_W - (int)strlen(msg) * 8) / 2, py + BOX_H - 40, msg, fb_rgb(235, 110, 110), bg);
    const char* hint = mode ? "TAB: back to login" : "TAB: create account    guest: nyx / nyx";
    font_draw_string(px + (BOX_W - (int)strlen(hint) * 8) / 2, py + BOX_H - 20, hint, fb_rgb(130, 120, 160), bg);

    fb_stroke_round_rect(px, py, BOX_W, BOX_H, R, fb_rgb(100, 82, 150));   // rounded brand-purple border
    fb_present();   // no-op unless the display is rotated (then this publishes the rotated frame)
}

// Redraw ONLY the field currently being typed into. Typing changes just the field
// text, so there's no need to repaint the whole panel (box + all four avatars +
// labels) on every keystroke — doing that hammered the framebuffer directly and
// destabilised the login on real hardware. Field geometry matches draw_panel.
static void draw_active_field(int px, int py, int field, const char* user, const char* pass) {
    if (field == 0)      draw_field(px + 20, py + 62,  BOX_W - 40, 1, user, 0);
    else if (field == 1) draw_field(px + 20, py + 114, BOX_W - 40, 1, pass, 1);
    fb_present();   // no-op unless rotated
}

// Live clock for the login screen — HH:MM:SS over the date, centred near the bottom
// like a lock screen. It repaints only its own gradient strip before drawing, so it
// ticks every second with no flicker of the panel or crescent-moon logo above it.
static void draw_login_clock(uint32_t fw, uint32_t fh) {
    rtc_time_t t;
    rtc_read_time(&t);
    char tm[16], dt[24];
    snprintf(tm, sizeof(tm), "%02u:%02u:%02u", t.hour, t.minute, t.second);
    snprintf(dt, sizeof(dt), "%04u-%02u-%02u", t.year, t.month, t.day);
    int cy = (int)fh - 54;
    for (int y = cy - 3; y < cy + 36 && y < (int)fh; y++)          // seamless gradient backing
        fb_fill_rect(0, y, fw, 1, login_grad((uint32_t)y, fh));
    int tw = (int)strlen(tm) * FONT_WIDTH;
    int dw = (int)strlen(dt) * FONT_WIDTH;
    font_draw_string_trans(((int)fw - tw) / 2, cy,      tm, fb_rgb(232, 224, 250));
    font_draw_string_trans(((int)fw - dw) / 2, cy + 18, dt, fb_rgb(150, 130, 205));
}

int login_screen(void) {
    uint32_t fw = fb_get_width(), fh = fb_get_height();

    serial_puts("[LOGIN] Drawing...\n");

    for (uint32_t y = 0; y < fh; y++)
        fb_fill_rect(0, y, fw, 1, login_grad(y, fh));

    int px = ((int)fw - BOX_W) / 2, py = (int)fh / 2 - 140;

    // NyxOS crescent-moon logo above the panel, in brand purple.
    const uint32_t moon = fb_rgb(168, 138, 245);
    int moon_w = NYX_LOGO_COLS * FONT_WIDTH;
    int moon_x = ((int)fw - moon_w) / 2;
    int moon_top = py - NYX_LOGO_ROWS * FONT_HEIGHT - 14;
    if (moon_top < 4) moon_top = 4;
    for (int i = 0; i < NYX_LOGO_ROWS; i++) {
        int y = moon_top + i * FONT_HEIGHT;
        font_draw_string(moon_x, y, NYX_LOGO[i], moon, login_grad((uint32_t)y, fh));
    }

    int mode = 0;         // 0 = login, 1 = create account
    int avatar = 0;       // selected profile picture (create mode)
    int attempts = 0;
    const char* msg = "";

    int last_sec = -1;    // login-screen clock: redraw only when the second changes
    draw_login_clock(fw, fh);

    for (;;) {
        char user[32], pass[64];
        int user_pos = 0, pass_pos = 0, field = 0;
        user[0] = pass[0] = '\0';

        draw_panel(px, py, mode, field, avatar, user, pass, msg);
        serial_puts("[LOGIN] Ready.\n");

        int submitted = 0;
        while (!submitted) {
            char c = getchar_poll();

            if (!c) {
                rtc_time_t ct; rtc_read_time(&ct);        // tick the clock once per second
                if ((int)ct.second != last_sec) { last_sec = (int)ct.second; draw_login_clock(fw, fh); }
                for (volatile int i = 0; i < 10000; i++);
                continue;
            }

            if (c == '\t') {                          // toggle login / create
                mode ^= 1; field = 0; user_pos = pass_pos = 0;
                user[0] = pass[0] = '\0'; msg = "";
                draw_panel(px, py, mode, field, avatar, user, pass, msg);
                continue;
            }
            if (c == '\n' || c == '\r') {
                if (field == 0) field = 1;
                else if (field == 1) { if (mode == 0) submitted = 1; else field = 2; }
                else submitted = 1;                   // create: avatar field -> submit
                if (!submitted) draw_panel(px, py, mode, field, avatar, user, pass, msg);
                continue;
            }
            if (field == 2) {                         // avatar picker
                if (c >= '1' && c <= '4') { avatar = c - '1'; draw_panel(px, py, mode, field, avatar, user, pass, msg); }
                else if (c == '\b' || c == 0x7F) { field = 1; draw_panel(px, py, mode, field, avatar, user, pass, msg); }
                continue;
            }
            if (c == '\b' || c == 0x7F) {
                if (field == 0 && user_pos > 0) user[--user_pos] = '\0';
                else if (field == 1 && pass_pos > 0) pass[--pass_pos] = '\0';
                draw_active_field(px, py, field, user, pass);   // light: just this field
                continue;
            }
            if (c >= 32 && c < 127) {
                if (field == 0 && user_pos < 31) { user[user_pos++] = c; user[user_pos] = '\0'; }
                else if (field == 1 && pass_pos < 63) { pass[pass_pos++] = c; pass[pass_pos] = '\0'; }
                draw_active_field(px, py, field, user, pass);   // light: just this field
                continue;
            }
        }

        if (mode == 1) {                              // ---- create account ----
            serial_puts("[LOGIN] Creating account...\n");
            if (user[0] == '\0') { msg = "Username required"; continue; }
            if ((int)strlen(pass) < LOGIN_MIN_PASS) { msg = "Password too short (min 4)"; field = 1; continue; }
            auth_add_user(user, pass, avatar);
            if (auth_verify(user, pass)) {            // confirm it was actually created
                strncpy(g_login_user, user, sizeof(g_login_user) - 1);
                g_login_user[sizeof(g_login_user) - 1] = '\0';
                g_login_avatar = avatar;
                setup_user_home();
                serial_puts("[LOGIN] OK.\n");
                secure_zero(pass, sizeof pass);   // wipe the plaintext before entering the session (CWE-316)
                return 1;
            }
            msg = "Could not create (name taken?)"; mode = 1; continue;
        }

        // ---- login ----
        serial_puts("[LOGIN] Verifying...\n");
        if (auth_verify(user, pass)) {
            strncpy(g_login_user, user, sizeof(g_login_user) - 1);
            g_login_user[sizeof(g_login_user) - 1] = '\0';
            g_login_avatar = auth_get_avatar(user);
            setup_user_home();
            serial_puts("[LOGIN] OK.\n");
            secure_zero(pass, sizeof pass);   // wipe the plaintext before entering the session (CWE-316)
            return 1;
        }

        secure_zero(pass, sizeof pass);   // a rejected password must not linger either
        attempts++;
        serial_puts("[LOGIN] FAIL.\n");
        // Brute-force defence: escalate the delay, and after LOGIN_MAX_ATTEMPTS
        // impose a longer cooldown — but NEVER reboot. Returning 0 here made the
        // caller reboot, which under `-no-reboot` just closes the VM (and a user
        // fumbling the new sign-up could trip it). The login stays up instead, so
        // they can retry or press TAB to create an account.
        if (attempts >= LOGIN_MAX_ATTEMPTS) {
            msg = "Too many attempts - locked briefly";
            draw_panel(px, py, mode, 0, avatar, "", "", msg);
            serial_puts("[LOGIN] LOCKED (cooldown).\n");
            for (volatile long i = 0; i < 150000000L; i++);   // cooldown, then reset
            attempts = 0;
            msg = "You can try again, or press TAB to sign up";
            continue;
        }
        msg = (attempts == 1) ? "Invalid credentials (1/3)" : "Invalid credentials (2/3)";
        for (volatile long i = 0; i < 25000000L * attempts; i++);   // graduated throttle
    }
}
