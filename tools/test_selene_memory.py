#!/usr/bin/env python3
"""Check real Selene tab/render allocation, cleanup and recovery under fault injection."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@SOURCE@"
extern void* malloc(size_t);
extern void free(void*);
extern void abort(void);
static size_t live, peak;
static int calls, fail_at, checks;
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) { return (r<<16)|(g<<8)|b; }
void* kmalloc(size_t n) {
    calls++;
    if (calls == fail_at) return NULL;
    size_t* p = malloc(sizeof(size_t) + n);
    if (!p) abort();
    *p = n; live += n; if (live > peak) peak = live;
    return p + 1;
}
void kfree(void* p) {
    if (!p) return;
    size_t* h = (size_t*)p - 1;
    if (*h > live) abort();
    live -= *h; free(h);
}
static int grids_empty(const selene_ctx_t* s) {
    return !s->lines && !s->link_of && !s->field_of && !s->img_of &&
           !s->color_of && !s->bgcolor_of && !s->bold_of;
}
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %s\n", #x); return 1; } } while (0)
int main(void) {
    selene_tabs_t* tabs = selene_create_ctx();
    CHECK(tabs && tabs->ntabs == 1 && grids_empty(tabs->tab[0]));
    int create_calls = calls;
    size_t one_tab = live;
    for (int i = 0; i < 10; i++) selene_tab_new(tabs);
    CHECK(tabs->ntabs == SEL_MAX_TABS && live < 1024 * 1024);
    size_t blank_tabs = live;
    for (int i = 0; i < tabs->ntabs; i++) CHECK(grids_empty(tabs->tab[i]));
    window_t win = {.reserved = tabs};
    selene_first_load(&win);
    CHECK(tabs->tab[tabs->active]->num_lines > 0);
    CHECK(!strcmp(tabs->tab[tabs->active]->title, "Welcome to Selene"));
    size_t rendered = live;
    CHECK(rendered > blank_tabs && rendered < 2 * 1024 * 1024);
    selene_first_load(&win);
    CHECK(live == rendered);
    static const char page[] = "<title>Form</title><form><input name='q' value='alice'></form>";
    selene_ctx_t* active = tabs->tab[tabs->active];
    render_html(active, (const uint8_t*)page, sizeof(page) - 1);
    CHECK(!strcmp(active->title, "Form") && active->num_forms == 1 && active->num_fields == 1);
    CHECK(!strcmp(active->fields[0].name, "q") && !strcmp(active->fields[0].value, "alice"));
    CHECK(live == rendered);
    printf("MEMORY one_blank=%lu six_blank=%lu one_rendered_five_blank=%lu\n",one_tab,blank_tabs,rendered);
    selene_win_close(&win); kfree(tabs);
    CHECK(!live);
    for (int i = 1; i <= create_calls; i++) {
        calls = 0; fail_at = i;
        tabs = selene_create_ctx();
        CHECK(!tabs && !live);
    }
    fail_at = 0; calls = 0; tabs = selene_create_ctx();
    CHECK(tabs && live == one_tab);
    for (int i = 1; i < create_calls; i++) {
        calls = 0; fail_at = i;
        selene_tab_new(tabs);
        CHECK(tabs->ntabs == 1 && live == one_tab);
    }
    fail_at = 0;
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int i = 1; i < SEL_MAX_TABS; i++) selene_tab_new(tabs);
        CHECK(tabs->ntabs == SEL_MAX_TABS);
        while (tabs->ntabs > 1) selene_tab_close(tabs, tabs->active);
        CHECK(live == one_tab);
    }
    win.reserved = tabs; selene_win_close(&win); kfree(tabs);
    CHECK(!live);
    /* Seven persistent grids and eleven required render scratch allocations. */
    for (int i = 1; i <= 18; i++) {
        fail_at = 0; calls = 0; tabs = selene_create_ctx(); win.reserved = tabs;
        CHECK(tabs && live == one_tab);
        calls = 0; fail_at = i; selene_first_load(&win);
        CHECK(strstr(tabs->tab[0]->status, "Out of memory") != NULL);
        CHECK(tabs->tab[0]->num_lines == 0);
        if (i <= 7) CHECK(grids_empty(tabs->tab[0]) && live == one_tab);
        else CHECK(live == one_tab + 7 * SEL_MAX_LINES * SEL_LINE_COLS);
        fail_at = 0; selene_first_load(&win);
        CHECK(strstr(tabs->tab[0]->status, "Ready") != NULL && tabs->tab[0]->num_lines > 0);
        CHECK(!strcmp(tabs->tab[0]->title, "Welcome to Selene"));
        selene_win_close(&win); kfree(tabs); CHECK(!live);
    }
    fail_at = 0; calls = 0; tabs = selene_create_ctx(); win.reserved = tabs;
    calls = 0; fail_at = 19; selene_first_load(&win); /* Optional table mapping. */
    CHECK(strstr(tabs->tab[0]->status,"Ready") && tabs->tab[0]->num_lines > 0);
    selene_win_close(&win); kfree(tabs); CHECK(!live);
    fail_at = 0;
    CHECK(selene_start_selftest() == 0 && !live);
    printf("PASS %d Selene memory checks, peak=%lu bytes\n",checks,peak);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'kernel/gui/apps/selene_win.c')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nyx-selene-memory-') as temp:
        directory = Path(temp)
        source = directory/'test.c'
        source.write_text(HARNESS.replace('@SOURCE@',args.source.resolve().as_posix()))
        subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
                        '-no-pie','-Wl,--gc-sections','-I',str(ROOT/'kernel/gui/apps'),str(source),
                        str(ROOT/'kernel/core/urlcodec.c'),
                        '-o',str(directory/'test')],check=True)
        subprocess.run([str(directory/'test')],check=True,timeout=60)


if __name__ == '__main__':
    main()
