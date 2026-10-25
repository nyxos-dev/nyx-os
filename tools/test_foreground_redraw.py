#!/usr/bin/env python3
"""Actual foreground repaint gate; mocked rendering, windows, clock and capture."""
from pathlib import Path
import argparse,re,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-dir',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1]
folder=a.source_dir
comp=(folder/'compositor.c' if folder else root/'kernel/gui/core/compositor.c').read_text(encoding='utf-8')
term=(folder/'terminal_win.c' if folder else root/'kernel/gui/apps/terminal_win.c').read_text(encoding='utf-8')
def function(text,signature):
    start=text.index(signature);opening=text.index('{',start);depth=1;i=opening+1
    while depth:
        depth += (text[i]=='{')-(text[i]=='}');i+=1
    return text[start:i]
gate=(function(comp,'void compositor_foreground_tick(void)') if 'void compositor_foreground_tick(void)' in comp
      else 'void compositor_foreground_tick(void){compositor_redraw_now();}')
capture=(term[term.index('static uint32_t capture_revision;'):term.index('static void terminal_flush_pending')]
         if 'static uint32_t capture_revision;' in term else
         'static uint32_t capture_revision;\nuint32_t terminal_capture_revision(void){return capture_revision;}\n'+function(term,'int terminal_capture_putchar(int c)'))
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#define MAX_WINDOWS 4
#define WSTATE_MINIMIZED 1
#define FOREGROUND_IDLE_REFRESH_MS 500u
typedef struct {int visible,state,workspace;void (*draw)(void);} window_t;
static window_t* windows[MAX_WINDOWS];static int current_workspace;
static uint32_t ticks;static int width=1024,height=768,animated,toasts,draws,change_during_draw;
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line %d\n",__LINE__);}}while(0)
static int fb_get_width(void){return width;}static int fb_get_height(void){return height;}
static uint32_t get_ticks(void){return ticks;}
static int wallpaper_animated(void){return animated;}static int notify_active_count(uint32_t t){(void)t;return toasts;}
static void terminal_win_draw(void){}static void other_draw(void){}static void welcome_draw_fn(void){}
typedef struct {int capturing;} terminal_win_t;
static terminal_win_t* capture_term;
static int terminal_output_putchar(terminal_win_t* t,int c){(void)t;return c;}
static void compositor_redraw_now(void);
'''
test=r'''
static void compositor_redraw_now(void){if(!width||!height)return;draws++;if(change_during_draw){terminal_capture_putchar('x');change_during_draw=0;}}
int main(void){
 (void)fb_get_width;(void)fb_get_height;(void)get_ticks;(void)wallpaper_animated;
 (void)notify_active_count;(void)current_workspace;
 window_t term={1,0,0,terminal_win_draw},other={1,0,0,other_draw};windows[0]=&term;
 terminal_win_t ctx={1};
 CHECK(terminal_capture_putchar('a')=='a'&&terminal_capture_revision()==0);
 capture_term=&ctx;ctx.capturing=0;terminal_capture_putchar('a');CHECK(terminal_capture_revision()==0);
 ctx.capturing=1;terminal_capture_putchar('a');CHECK(terminal_capture_revision()==1);
 width=0;compositor_foreground_tick();CHECK(draws==0);width=1024;height=0;compositor_foreground_tick();CHECK(draws==0);height=768;
 compositor_foreground_tick();CHECK(draws==1);
 ticks=60;compositor_foreground_tick();CHECK(draws==1);
 ticks=499;compositor_foreground_tick();CHECK(draws==1);
 ticks=500;compositor_foreground_tick();CHECK(draws==2);
 ticks=501;terminal_capture_putchar('b');compositor_foreground_tick();CHECK(draws==3);
 for(int i=0;i<100;i++)terminal_capture_putchar('c');compositor_foreground_tick();CHECK(draws==4);
 compositor_foreground_tick();CHECK(draws==4);
 windows[1]=&other;compositor_foreground_tick();CHECK(draws==5);compositor_foreground_tick();CHECK(draws==6);
 other.visible=0;compositor_foreground_tick();CHECK(draws==6);
 other.visible=1;other.state=WSTATE_MINIMIZED;compositor_foreground_tick();CHECK(draws==6);
 other.state=0;other.workspace=1;compositor_foreground_tick();CHECK(draws==6);
 other.workspace=0;other.draw=welcome_draw_fn;compositor_foreground_tick();CHECK(draws==6);
 animated=1;compositor_foreground_tick();CHECK(draws==7);compositor_foreground_tick();CHECK(draws==8);animated=0;
 toasts=1;compositor_foreground_tick();CHECK(draws==9);toasts=0;
 ticks=UINT32_MAX-200;compositor_foreground_tick();CHECK(draws==10);
 ticks=298;compositor_foreground_tick();CHECK(draws==10);ticks=299;compositor_foreground_tick();CHECK(draws==11);
 change_during_draw=1;terminal_capture_putchar('d');compositor_foreground_tick();CHECK(draws==12);
 compositor_foreground_tick();CHECK(draws==13);compositor_foreground_tick();CHECK(draws==13);
 capture_revision=UINT32_MAX;terminal_capture_putchar('e');compositor_foreground_tick();CHECK(draws==14);
 compositor_redraw_now();CHECK(draws==15);
 printf("Foreground repaint: %u checks, %u failures (mocked rendering/time)\n",checks,failures);
 return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='nyx-foreground-redraw-') as d:
    f=Path(d);(f/'test.c').write_text(prefix+capture+gate+test,encoding='utf-8')
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(f/'test.c'),'-o',str(f/'test')],check=True)
    subprocess.run([str(f/'test')],check=True)
