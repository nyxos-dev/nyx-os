#ifndef TASKMAN_WIN_H
#define TASKMAN_WIN_H

#include "../../core/kernel.h"
#include "../core/compositor.h"

// Samples kept in each scrolling performance graph (ring buffer). At ~4 Hz this is ~24 s of
// history — enough to see a trend without holding a lot of state.
#define MON_HISTORY 96
#define MON_ROWS 64

typedef struct {
    int scroll_offset;
    int process_total;
    process_monitor_info_t rows[MON_ROWS];
    unsigned char cpu_hist[MON_HISTORY];   // CPU utilization samples, 0..100
    unsigned char mem_hist[MON_HISTORY];   // memory-in-use samples, 0..100
    unsigned char net_hist[MON_HISTORY];   // network activity samples, 0..100 (pkt/s, scaled)
    int hist_count;        // valid samples so far (ramps up to MON_HISTORY)
    int hist_head;         // ring write cursor (next slot to fill)
    int tick_accum;        // on_tick frames since the last sample was taken
    uint32_t net_prev;     // previous cumulative rx+tx packet total (all interfaces)
    uint32_t net_last_tick;// tick_count at the previous sample (for an accurate pkt/s)
    uint32_t net_rate;     // packets/second over the last interval (shown in the label)
    int net_primed;        // 1 once a baseline exists, so the first delta isn't the whole boot total
} taskman_win_t;

taskman_win_t* taskman_create_ctx(void);
void taskman_win_draw(window_t* win, int cx, int cy, uint32_t cw, uint32_t ch);
void taskman_win_click(window_t* win, int mx, int my, int btn);
void taskman_win_key(window_t* win, int key);
int  taskman_win_tick(window_t* win);   // ~30 fps; samples at ~4 Hz, returns 1 on a new sample

#endif
