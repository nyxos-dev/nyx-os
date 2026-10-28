#!/usr/bin/env python3
"""Check actual Properties drawing bounds, wrapping and snapshot preservation."""
from pathlib import Path
import argparse
import subprocess
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "kernel/gui/apps/fileman_win.h"
#include "kernel/drivers/video/font.h"
void* memset(void*,int,size_t);
int memcmp(const void*,const void*,size_t);
#define THEME_PANEL_HEADER 0
@FORMAT@
static int cx0,cy0,cw0,ch0,px0,py0,pw0,ph0,rects,bad,checks;
static char rendered[4096];
uint32_t fb_rgb(uint8_t r,uint8_t g,uint8_t b) { (void)r;(void)g;(void)b;return 0; }
void fb_fill_rect(uint32_t ux,uint32_t uy,uint32_t uw,uint32_t uh,uint32_t c) {
    int x=ux,y=uy,w=uw,h=uh;
    (void)c;
    if(!rects++){px0=x;py0=y;pw0=w;ph0=h;}
    if(w<=0||h<=0||x<cx0||y<cy0||x+w>cx0+cw0||y+h>cy0+ch0)bad++;
}
void font_draw_string(uint32_t x,uint32_t y,const char* text,uint32_t f,uint32_t b) {
    (void)f;(void)b;int n=strlen(text);
    if(!rects||(int)x<px0||(int)y<py0||(int)x+n*FONT_WIDTH>px0+pw0||(int)y+FONT_HEIGHT>py0+ph0)bad++;
    for(int i=0;i<n;i++)if((unsigned char)text[i]<32||(unsigned char)text[i]>126)bad++;
    if(strlen(rendered)+n+1<sizeof(rendered))strcat(rendered,text);else bad++;
}
@FUNCTIONS@
#define CHECK(x) do { checks++;if(!(x)){printf("FAIL line %d: %s (%dx%d)\n",__LINE__,#x,cw0,ch0);return 1;} } while(0)
static void draw(fileman_win_t* fm,int w,int h) {
    cx0=23;cy0=37;cw0=w;ch0=h;rects=bad=0;rendered[0]=0;
    fileman_draw_properties(fm,cx0,cy0,w,h);
}
int main(void) {
    static fileman_win_t fm,before;fm.props_open=1;fm.props_size_known=1;fm.props_size=0xffffffffu;
    memset(fm.props_name,'n',63);memset(fm.props_path,'p',319);before=fm;
    draw(&fm,550,400);CHECK(!bad);
    for(int w=0;w<=800;w+=4)for(int h=0;h<=600;h+=4) {
        draw(&fm,w,h);CHECK(!bad);CHECK(!memcmp(&fm,&before,sizeof(fm)));
    }
    draw(&fm,550,400);CHECK(!bad);CHECK(strstr(rendered,fm.props_name));CHECK(strstr(rendered,fm.props_path));
    CHECK(strstr(rendered,"Size: 4096 MB (4294967295 bytes)"));CHECK(!strstr(rendered,"Resize"));
    draw(&fm,300,180);CHECK(!bad);CHECK(strstr(rendered,"..."));CHECK(strstr(rendered,"Resize"));
    draw(&fm,116,54);CHECK(!bad);CHECK(strstr(rendered,"Resize/Esc"));
    fm.props_is_dir=1;draw(&fm,550,400);CHECK(strstr(rendered,"Type: DirectorySize: --"));
    fm.props_is_dir=0;fm.props_size_known=0;draw(&fm,550,400);CHECK(strstr(rendered,"Size: Unknown"));
    strcpy(fm.props_name,"a\nb\tc\177d\200");draw(&fm,550,400);CHECK(!bad);CHECK(strstr(rendered,"Name: a?b?c?d?"));
    const int counts[]={0,1,2,32,512};
    for(int h=-64;h<=512;h++)for(int c=0;c<5;c++)for(int rows=1;rows<=16;rows+=15) {
        cx0=23;cy0=37;cw0=SCROLL_W;ch0=h;rects=bad=0;
        int offset=counts[c]>rows?counts[c]-rows:0;
        fileman_draw_scrollbar(cx0,cy0,h,counts[c],rows,offset);
        CHECK(!bad);CHECK(h>0?rects>0:rects==0);
    }
    printf("File Manager Properties: %d checks, 0 failures\n",checks);return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', help='Compare an earlier committed draw implementation')
    args = parser.parse_args()
    source = subprocess.check_output(['git', 'show', args.ref + ':kernel/gui/apps/fileman_win.c'], cwd=ROOT, text=True) if args.ref else (ROOT/'kernel/gui/apps/fileman_win.c').read_text()
    kernel = (ROOT/'kernel/core/kernel.c').read_text()
    formatter = '\n'.join(function(kernel, s) for s in ['static int u_fmt(', 'int vsnprintf(', 'int snprintf('])
    bodies = '\n'.join(function(source, s) for s in ['static void fileman_fmt_size(', 'static void fileman_property_size('])
    bodies += '\n' + next(line for line in source.splitlines() if line.startswith('#define SCROLL_W'))
    if 'static void fileman_draw_scrollbar(' in source:
        bodies += '\n' + function(source, 'static void fileman_draw_scrollbar(')
    else:
        start = source.index('    int sb_x =', source.index('// Scrollbar'))
        end = source.index('    // Status bar', start)
        bodies += '\nstatic void fileman_draw_scrollbar(int cx,int list_y,int avail_h,int disp_count,int max_rows,int offset) { fileman_win_t storage={0},*fm=&storage;int list_w=0;fm->scroll_offset=offset;\n' + source[start:end] + '\n}\n'
    if 'static void fileman_draw_properties(' in source:
        bodies += '\n' + '\n'.join(line for line in source.splitlines() if line.startswith('#define PROPS_'))
        bodies += '\n' + '\n'.join(function(source, s) for s in ['static void fileman_property_line(', 'static void fileman_property_row(', 'static void fileman_draw_properties('])
    else:
        start = source.index('    if (fm->props_open) {', source.index('// Properties modal'))
        end = source.index('\n}\n\nvoid fileman_win_click', start)
        bodies += '\nstatic void fileman_draw_properties(fileman_win_t* fm,int cx,int cy,uint32_t cw,uint32_t ch) { uint32_t char_h=FONT_HEIGHT;\n' + source[start:end] + '\n}\n'
    with tempfile.TemporaryDirectory(prefix='nyx-properties-') as tmp:
        p = Path(tmp)
        (p/'check.c').write_text(HARNESS.replace('@FORMAT@', formatter).replace('@FUNCTIONS@', bodies))
        subprocess.run(['gcc','-std=gnu99','-ffreestanding','-O1','-Wall','-Wextra','-Werror','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',ROOT,p/'check.c','-o',p/'check'],check=True)
        subprocess.run([p/'check'],check=True)


if __name__ == '__main__':
    main()
