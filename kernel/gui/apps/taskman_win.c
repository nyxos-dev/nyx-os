#include "../core/theme.h"
#include "../../core/kernel.h"
#include "../core/compositor.h"
#include "taskman_win.h"
#include "../../drivers/video/font.h"

// Nyx Monitor — NyxOS's performance monitor. A "Performance" section of live scrolling CPU% and
// memory% graphs (fed by the scheduler's utilization accountant, perf_cpu_percent(), and the
// physical allocator's frame counts) sits above the classic process table. The graphs update at
// ~4 Hz via the compositor's on_tick; the readouts and table are computed fresh each draw.

#define HEADER_H  18
#define ROW_H     FONT_HEIGHT
#define COL_PID   40          // x offset where the Name column starts (leaves room for "PID")
#define COL_NAME  190         // ...where State starts
#define COL_STATE 300         // ...where CPU starts
#define COL_CPU   380         // ...where HEAP (per-process heap size) starts

#define GRAPH_H   56          // plot height of each performance graph
#define LABEL_H   15          // caption row above each graph
#define NET_FULL_PPS 200      // packets/second that fills the network graph to 100% (a busy link)

taskman_win_t* taskman_create_ctx(void) {
    taskman_win_t* tm = (taskman_win_t*)kmalloc(sizeof(taskman_win_t));
    if (!tm) return NULL;
    memset_asm(tm, 0, sizeof(taskman_win_t));
    return tm;
}

// Correct process-state names — matches the PROC_* enum (kernel.h) and `ps`/`top`. (The earlier
// mapping here mislabelled states, e.g. calling PROC_ZOMBIE "Running".)
static const char* state_str(uint32_t state) {
    switch (state) {
        case PROC_RUN:     return "Running";
        case PROC_ZOMBIE:  return "Zombie";
        case PROC_BLOCKED: return "Blocked";
        case PROC_STOPPED: return "Stopped";
        default:           return "New";      // 0 == parked / never scheduled
    }
}

// Sample the live memory pool via the one honest source shared with top / nyxfetch / mem:
// used = the managed pool minus free (counts the reserved kernel + low memory, unlike memory_used).
static void mon_mem(uint32_t* pct, uint32_t* used_mb, uint32_t* total_mb) {
    uint32_t used_kb, total_kb;
    mem_pool_kb(&used_kb, 0, &total_kb);
    *pct      = total_kb ? (uint32_t)(((uint64_t)used_kb * 100) / total_kb) : 0;
    *used_mb  = used_kb / 1024;
    *total_mb = total_kb / 1024;
}

// Keep the allocator's usable pool separate from RAM that firmware detected but
// this kernel cannot yet allocate. A large VM must not look fully occupied just
// because the physical allocator currently manages only its first 512 MiB.
static void mon_size(char* out, size_t size, uint64_t bytes) {
    uint64_t mib = bytes / (1024 * 1024);
    if (mib < 1024) snprintf(out, size, "%u MiB", (uint32_t)mib);
    else {
        uint64_t tenths = (bytes / (1024 * 1024) * 10 + 512) / 1024;
        snprintf(out, size, "%llu.%u GiB", tenths / 10, (uint32_t)(tenths % 10));
    }
}

// ~30 fps tick from the compositor. Sample the metrics ~4 Hz into the scrolling rings and ask for
// a repaint only then, so an open monitor doesn't force a full 30 fps recomposite.
int taskman_win_tick(window_t* win) {
    taskman_win_t* tm = (taskman_win_t*)win->reserved;
    if (!tm) return 0;
    if (++tm->tick_accum < 8) return 0;       // ~8 * 33 ms ~= 264 ms
    tm->tick_accum = 0;

    uint32_t cpu = perf_cpu_percent();
    uint32_t mem, u, t;
    mon_mem(&mem, &u, &t);

    // Network activity: packets/second since the last sample, from the honest per-interface
    // rx/tx counters (v6.4.405). Sum both directions across every interface, diff against the
    // previous total, and divide by the real elapsed time (tick_count ms) for an accurate rate.
    extern volatile uint32_t tick_count;
    uint32_t net_total = 0;
    for (int i = 0; i < 8; i++)
        if (net_interfaces[i].name[0])
            net_total += net_interfaces[i].rx_packets + net_interfaces[i].tx_packets;
    uint32_t now = tick_count;
    uint32_t rate = 0;
    if (tm->net_primed) {
        uint32_t dpkt = net_total - tm->net_prev;          // wraps cleanly in uint32
        uint32_t dms  = now - tm->net_last_tick;
        if (dms > 0) rate = (dpkt * 1000u) / dms;          // packets per second
    }
    tm->net_prev = net_total;
    tm->net_last_tick = now;
    tm->net_primed = 1;
    tm->net_rate = rate;
    uint32_t nv = rate >= NET_FULL_PPS ? 100 : (rate * 100u) / NET_FULL_PPS;

    tm->cpu_hist[tm->hist_head] = (unsigned char)(cpu > 100 ? 100 : cpu);
    tm->mem_hist[tm->hist_head] = (unsigned char)(mem > 100 ? 100 : mem);
    tm->net_hist[tm->hist_head] = (unsigned char)nv;
    tm->hist_head = (tm->hist_head + 1) % MON_HISTORY;
    if (tm->hist_count < MON_HISTORY) tm->hist_count++;
    return 1;
}

// Draw one filled-area history graph: dark inset + 25/50/75% gridlines, then one vertical
// gradient column per pixel (bright at the recent value, dim toward the baseline) with a bright
// cap pixel so the trend reads as a line, then a thin border. Oldest sample at the left edge.
static void mon_draw_graph(int gx, int gy, int gw, int gh,
                           const unsigned char* hist, int count, int head,
                           uint32_t top_col, uint32_t bot_col, uint32_t cap_col) {
    fb_fill_rect(gx, gy, gw, gh, fb_rgb(18, 16, 28));
    for (int q = 1; q < 4; q++) {
        int yy = gy + gh - 1 - (gh - 2) * q / 4;
        fb_fill_rect(gx, yy, gw, 1, fb_rgb(38, 32, 54));
    }
    for (int col = 0; col < gw; col++) {
        int v = 0;
        if (count > 0) {
            int si = (col * count) / gw;                 // 0..count-1, oldest at left
            if (si >= count) si = count - 1;
            int idx = (head - count + si) % MON_HISTORY;
            if (idx < 0) idx += MON_HISTORY;
            v = hist[idx];
        }
        if (v > 100) v = 100;
        int barh = (v * (gh - 2)) / 100;
        if (barh > 0) {
            fb_fill_vgrad(gx + col, gy + gh - barh, 1, barh, top_col, bot_col);
            fb_put_pixel(gx + col, gy + gh - barh, cap_col);
        }
    }
    fb_fill_rect(gx, gy, gw, 1, fb_rgb(70, 58, 110));
    fb_fill_rect(gx, gy + gh - 1, gw, 1, fb_rgb(70, 58, 110));
    fb_fill_rect(gx, gy, 1, gh, fb_rgb(70, 58, 110));
    fb_fill_rect(gx + gw - 1, gy, 1, gh, fb_rgb(70, 58, 110));
}

void taskman_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch) {
    taskman_win_t* tm = (taskman_win_t*)win->reserved;
    if (!tm) return;

    fb_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG);   // clean client background

    uint32_t cpu = perf_cpu_percent();
    uint32_t mem_pct, mem_used, mem_total;
    mon_mem(&mem_pct, &mem_used, &mem_total);

    int gx = cx + 8;
    int gw = (int)cw - 16;
    char lbl[56];

    // --- CPU performance graph (lilac) ---
    int y = cy + 5;
    snprintf(lbl, sizeof(lbl), "CPU  %u%%", cpu);
    font_draw_string(gx, y, lbl, fb_rgb(200, 175, 255), THEME_WINDOW_BG);
    mon_draw_graph(gx, y + LABEL_H, gw, GRAPH_H, tm->cpu_hist, tm->hist_count, tm->hist_head,
                   fb_rgb(170, 130, 255), fb_rgb(58, 40, 110), fb_rgb(215, 190, 255));

    // --- Memory performance graph (teal) ---
    y = y + LABEL_H + GRAPH_H + 5;
    snprintf(lbl, sizeof(lbl), "Managed RAM  %u%%  (%u / %u MiB)", mem_pct, mem_used, mem_total);
    font_draw_string(gx, y, lbl, fb_rgb(150, 235, 215), THEME_WINDOW_BG);
    mon_draw_graph(gx, y + LABEL_H, gw, GRAPH_H, tm->mem_hist, tm->hist_count, tm->hist_head,
                   fb_rgb(90, 210, 180), fb_rgb(25, 85, 72), fb_rgb(150, 240, 220));

    // --- Network activity graph (amber) ---
    y = y + LABEL_H + GRAPH_H + 5;
    snprintf(lbl, sizeof(lbl), "NET  %u pkt/s", tm->net_rate);
    font_draw_string(gx, y, lbl, fb_rgb(255, 205, 130), THEME_WINDOW_BG);
    mon_draw_graph(gx, y + LABEL_H, gw, GRAPH_H, tm->net_hist, tm->hist_count, tm->hist_head,
                   fb_rgb(250, 175, 70), fb_rgb(110, 70, 20), fb_rgb(255, 210, 150));

    // --- Process table ---
    int list_y = y + LABEL_H + GRAPH_H + 6;
    fb_fill_rect(cx, list_y, cw, HEADER_H, THEME_PANEL_HEADER);
    font_draw_string(cx + 4,             list_y + 2, "PID",   fb_rgb(255,200,100), THEME_PANEL_HEADER);
    font_draw_string(cx + COL_PID + 4,   list_y + 2, "Name",  fb_rgb(255,200,100), THEME_PANEL_HEADER);
    font_draw_string(cx + COL_NAME + 4,  list_y + 2, "State", fb_rgb(255,200,100), THEME_PANEL_HEADER);
    font_draw_string(cx + COL_STATE + 4, list_y + 2, "CPU",   fb_rgb(255,200,100), THEME_PANEL_HEADER);
    font_draw_string(cx + COL_CPU + 4,   list_y + 2, "HEAP",  fb_rgb(255,200,100), THEME_PANEL_HEADER);

    int rows_y   = list_y + HEADER_H;
    int status_y = cy + (int)ch - HEADER_H;
    int avail_h  = status_y - rows_y;
    int max_rows = avail_h / ROW_H;
    if (max_rows < 1) max_rows = 1;

    for (int i = 0; i < max_rows; i++) {
        int idx = i + tm->scroll_offset;
        if (idx >= process_count) break;
        process_t* p = process_table[idx];
        if (!p) continue;

        int ry = rows_y + i * ROW_H;
        uint32_t bg = (i & 1) ? fb_rgb(50, 50, 55) : THEME_WINDOW_BG;
        fb_fill_rect(cx, ry, cw, ROW_H, bg);

        char pid_str[8];
        snprintf(pid_str, sizeof(pid_str), "%u", p->pid);
        font_draw_string(cx + 4, ry + 1, pid_str, THEME_TEXT, bg);

        char name[25];
        strncpy(name, p->comm, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        font_draw_string(cx + COL_PID + 4, ry + 1, name, fb_rgb(180,220,180), bg);

        font_draw_string(cx + COL_NAME + 4, ry + 1, state_str(p->state), fb_rgb(200,200,200), bg);

        char cpu_str[16];
        snprintf(cpu_str, sizeof(cpu_str), "%u", p->cpu_time);
        font_draw_string(cx + COL_STATE + 4, ry + 1, cpu_str, fb_rgb(200,200,200), bg);

        // HEAP: the process's heap footprint (program_break - heap_start), in KB. Scalar reads
        // only (no page-table walk), so it is as cheap and safe as the CPU column; a kernel
        // thread with no user heap shows 0K.
        char mem_str[16];
        uint64_t heap_b = (p->program_break > p->heap_start) ? (p->program_break - p->heap_start) : 0;
        snprintf(mem_str, sizeof(mem_str), "%uK", (uint32_t)(heap_b / 1024));
        font_draw_string(cx + COL_CPU + 4, ry + 1, mem_str, fb_rgb(200,200,200), bg);
    }

    // --- Status bar ---
    fb_fill_rect(cx, status_y, cw, HEADER_H, THEME_PANEL_HEADER);
    char status[128];
    char detected[24], managed[24];
    mon_size(detected, sizeof(detected), memory_total);
    mon_size(managed, sizeof(managed), (uint64_t)mem_total * 1024 * 1024);
    snprintf(status, sizeof(status), "Procs: %d  RAM detected: %s  Pool: %s",
             process_count, detected, managed);
    font_draw_string(cx + 4, status_y + 2, status, fb_rgb(180,200,220), THEME_PANEL_HEADER);
}

void taskman_win_click(window_t* win, int mx, int my, int btn) {
    (void)win;
    (void)mx;
    (void)my;
    (void)btn;
    // Future: select process row, kill button
}

void taskman_win_key(window_t* win, int key) {
    taskman_win_t* tm = (taskman_win_t*)win->reserved;
    if (!tm) return;

    if (key == KEY_UP && tm->scroll_offset > 0)
        tm->scroll_offset--;
    else if (key == KEY_DOWN && tm->scroll_offset < process_count - 1)
        tm->scroll_offset++;
}
