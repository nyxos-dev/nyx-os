#!/usr/bin/env python3
"""Exercise actual held drag/resize code: unchanged geometry must not repaint."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "kernel/gui/core/compositor.c"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def harness(source):
    header = (ROOT / "kernel/gui/core/compositor.h").read_text()
    constants = "\n".join(re.search(r"^#define " + n + r"\s+\d+", header, re.M)[0]
                          for n in ("TITLE_H", "MIN_WIN_W", "MIN_WIN_H", "TASKBAR_H"))
    constants += "\n" + "\n".join(re.search(r"^#define " + n + r"\s+\d+", source, re.M)[0]
                                   for n in ("SHADOW_OFFSET", "SHADOW_RADIUS"))
    enums = header[header.index("enum {"):header.index("};", header.index("enum {")) + 2]
    start = source.index("if (drag_icon_idx >= 0) {", source.index("if (btns & 1) {", source.index("void compositor_run")))
    end = source.index("} else {\n                window_t* hit", start)
    body = source[start:end] + "}"
    return r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + constants + "\n" + enums + r'''
typedef struct {
    int x,y,id,resizing,drag_off_x,drag_off_y,resize_dir,resize_start_x,resize_start_y;
    uint32_t w,h,resize_start_w,resize_start_h;
} window_t;
static window_t window;
static int drag_id,resize_id,drag_icon_idx,drag_icon_ofs_x,drag_icon_ofs_y;
static int desktop_icon_x[1],desktop_icon_y[1];
static int redraw,drag_dmg_valid,ddx,ddy,ddw,ddh;
static window_t* find_window(int id) { return id==window.id ? &window : NULL; }
static uint32_t fb_get_width(void) { return 1024; }
static uint32_t fb_get_height(void) { return 768; }
''' + function(source, "static inline uint32_t win_total_h(") + "\n" + function(source, "void window_move(") + "\n" + function(source, "void window_resize(") + r'''
static void held(int mx,int my) {
''' + body + r'''
done_click:;
}
static int cases, failures;
#define CHECK(expr) do { cases++; if (!(expr)) { failures++; fprintf(stderr,"damage: line%d: %s\n",__LINE__,#expr); } } while(0)
static void reset(void) {
    memset(&window,0,sizeof window);
    window.id=1; window.x=80; window.y=80; window.w=640; window.h=400;
    drag_id=resize_id=0; drag_icon_idx=-1;
    redraw=drag_dmg_valid=ddx=ddy=ddw=ddh=0;
}
static void expect(int x,int y,unsigned w,unsigned h,int changed) {
    CHECK(window.x==x && window.y==y && window.w==w && window.h==h);
    CHECK(redraw==changed && drag_dmg_valid==changed);
}
int main(void) {
    reset();drag_id=1;window.drag_off_x=170;window.drag_off_y=10;
    held(250,90);expect(80,80,640,400,0);
    redraw=1;held(250,90);CHECK(redraw==1 && !drag_dmg_valid);
    redraw=0;held(310,120);expect(140,110,640,400,1);
    int margin=SHADOW_OFFSET+SHADOW_RADIUS+2;
    CHECK(ddx==80-margin && ddy==80-margin && ddw==700+2*margin && ddh==452+2*margin);
    redraw=drag_dmg_valid=0;held(310,120);expect(140,110,640,400,0);
    held(-50,-50);expect(0,0,640,400,1);
    redraw=drag_dmg_valid=0;held(-80,-80);expect(0,0,640,400,0);
    held(2000,2000);expect(384,310,640,400,1);
    redraw=drag_dmg_valid=0;held(3000,3000);expect(384,310,640,400,0);
    reset();drag_id=99;held(250,90);expect(80,80,640,400,0);
    reset();resize_id=99;held(250,90);expect(80,80,640,400,0);
    for(int dir=RESIZE_RIGHT;dir<=RESIZE_LEFT_BOTTOM;dir++) {
        reset();resize_id=1;window.resizing=1;window.resize_dir=dir;
        window.resize_start_x=80;window.resize_start_y=80;
        window.resize_start_w=640;window.resize_start_h=400;
        held(80,80);expect(80,80,640,400,0);
        redraw=1;held(80,80);CHECK(redraw==1 && !drag_dmg_valid);
        /* Exercise a real change, then the same geometry repeatedly. */
        redraw=0;held(90,90);CHECK(redraw==1 && drag_dmg_valid==1);
        int x=window.x,y=window.y;unsigned w=window.w,h=window.h;
        redraw=drag_dmg_valid=0;held(90,90);expect(x,y,w,h,0);
    }
    reset();resize_id=1;window.resizing=1;window.resize_dir=RESIZE_CORNER;
    window.resize_start_x=720;window.resize_start_y=502;
    window.resize_start_w=640;window.resize_start_h=400;
    held(780,532);expect(80,80,700,430,1);
    CHECK(ddx==80-margin && ddy==80-margin && ddw==700+2*margin && ddh==452+2*margin);
    redraw=drag_dmg_valid=0;held(600,442);expect(80,80,520,340,1);
    CHECK(ddw==700+2*margin && ddh==452+2*margin); /* vacated pixels */
    redraw=drag_dmg_valid=0;held(-200,-200);expect(80,80,120,80,1);
    redraw=drag_dmg_valid=0;held(-300,-300);expect(80,80,120,80,0);
    held(2000,2000);expect(80,80,944,630,1);
    redraw=drag_dmg_valid=0;held(3000,3000);expect(80,80,944,630,0);
    reset();drag_icon_idx=0;drag_icon_ofs_x=10;drag_icon_ofs_y=10;
    desktop_icon_x[0]=20;desktop_icon_y[0]=20;
    held(30,30);CHECK(!redraw && !drag_dmg_valid);
    held(40,50);CHECK(redraw && desktop_icon_x[0]==30 && desktop_icon_y[0]==40);
    redraw=0;held(-10,-10);CHECK(redraw && !desktop_icon_x[0] && !desktop_icon_y[0]);
    redraw=0;held(-20,-20);CHECK(!redraw && !drag_dmg_valid);
    redraw=1;held(-20,-20);CHECK(redraw==1);
    printf("desktop damage: %d checks, %d failures\n",cases,failures);
    return failures ? 1 : 0;
}
'''


def run(source, expected_failure=False):
    with tempfile.TemporaryDirectory(prefix="nyx-desktop-damage-") as temp:
        path = Path(temp)
        (path / "test.c").write_text(harness(source), encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        result = subprocess.run([str(path / "test")], capture_output=True, text=True, timeout=10)
        print(result.stdout, end="")
        if result.returncode != int(expected_failure):
            raise AssertionError(result.stderr)
        if expected_failure:
            assert "damage:" in result.stderr


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare-ref")
    args = parser.parse_args()
    run((ROOT / SOURCE).read_text(encoding="utf-8"))
    if args.compare_ref:
        baseline = subprocess.check_output(["git", "show", f"{args.compare_ref}:{SOURCE}"],
                                           cwd=ROOT, text=True, encoding="utf-8")
        run(baseline, expected_failure=True)
