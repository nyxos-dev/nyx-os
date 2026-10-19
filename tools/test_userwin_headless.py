#!/usr/bin/env python3
"""Check user-window creation without a framebuffer and normal desktop binding."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "@SOURCE@"
extern void* malloc(size_t);
extern void free(void*);
static uint32_t fw,fh;
static int creates,destroys,checks,unavailable;
static window_t bound;
uint32_t fb_get_width(void) { return fw; }
uint32_t fb_get_height(void) { return fh; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
void fb_blit(const void* s,uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t dx,uint32_t dy) {
    (void)s;(void)x;(void)y;(void)w;(void)h;(void)dx;(void)dy;
}
void fb_fill_rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c) {
    (void)x;(void)y;(void)w;(void)h;(void)c;
}
window_t* window_create(int x,int y,uint32_t w,uint32_t h,const char* t,window_draw_fn draw) {
    (void)x;(void)y;(void)w;(void)h;(void)t;(void)draw;creates++;
    if(unavailable)return 0;
    bound.id=42;return &bound;
}
void window_destroy(int id) { (void)id;destroys++;if(bound.on_close)bound.on_close(&bound); }
int window_set_title(int id,const char* t) { (void)id;(void)t;return 0; }
void window_resize(int id,uint32_t w,uint32_t h) { (void)id;(void)w;(void)h; }
#define CHECK(x) do {checks++;if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}}while(0)
int main(void) {
    uint32_t pixels[64]={0};uwin_event_t ev;
    const uint32_t dimensions[][2]={{0,0},{0,768},{1024,0}};
    for(int k=0;k<3;k++) {
        fw=dimensions[k][0];fh=dimensions[k][1];creates=destroys=0;
        for(int i=0;i<32;i++) {
            int id=uwin_create(8,8,"headless");CHECK(id>=0 && creates==0);
            CHECK(uwin_present(id,pixels,8,8)==0);
            CHECK(uwin_poll_event(id,&ev)==0);
            CHECK(uwin_set_title(id,"new")==0);
            CHECK(uwin_resize(id,4,4)==0);
            CHECK(uwin_destroy(id)==0 && destroys==0);
        }
    }
    fw=1024;fh=768;creates=destroys=0;
    int id=uwin_create(8,8,"desktop");
    CHECK(id>=0 && creates==1 && uwin_slot(id)->win_id==42);
    CHECK(bound.on_key && bound.on_close && bound.on_resize && bound.reserved==uwin_slot(id));
    CHECK(uwin_present(id,pixels,8,8)==0);
    bound.on_key(&bound,'x');CHECK(uwin_poll_event(id,&ev)==1 && ev.a=='x');
    CHECK(uwin_destroy(id)==0 && destroys==1 && !uwin_slot(id));
    unavailable=1;id=uwin_create(8,8,"full compositor");
    CHECK(id>=0 && uwin_slot(id)->win_id==-1);
    CHECK(uwin_destroy(id)==0 && destroys==1);
    CHECK(uwin_create(0,8,"bad")==-1);
    CHECK(uwin_selftest()==0);
    printf("PASS %d headless/desktop checks plus existing userwin KAT\n",checks);
    return 0;
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    args=parser.parse_args()
    original=ROOT/'kernel/gui/core/userwin.c'
    with tempfile.TemporaryDirectory(prefix='nyx-userwin-') as temp:
        work=Path(temp)
        def run(source,label):
            src=work/(label+'-source.c');src.write_text(source)
            h=work/(label+'.c');h.write_text(HARNESS.replace('@SOURCE@',str(src)))
            exe=work/label
            subprocess.run(['gcc','-std=gnu99','-O1','-ffreestanding','-Wall','-Wextra','-Werror',
                '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(original.parent),
                str(h),'-o',str(exe)],check=True)
            return subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        result=run(original.read_text(),'current')
        assert result.returncode==0,result.stdout+result.stderr
        print(result.stdout,end='')
        if args.compare_ref:
            old=subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/core/userwin.c'],text=True)
            result=run(old,'previous')
            assert result.returncode!=0 and 'FAIL' in result.stdout,result.stdout+result.stderr
            print('PASS baseline attempts compositor binding without framebuffer')

if __name__=='__main__':
    main()
