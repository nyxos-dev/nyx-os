#!/usr/bin/env python3
"""Exercise the actual rectangle filler with wrapped dimensions and both pixel formats."""
from pathlib import Path
import argparse
import subprocess
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t storage[8*6+2], expected[8*6+2];
static uintptr_t fb_addr;
static uint32_t fb_width=8,fb_height=6,fb_bpp;
static int clip_on,region_on,spans;
static void clip_span(int y,int* lo,int* hi) {
    if(++spans>6){puts("FAIL: unbounded row iteration");exit(1);}
    *lo=0;*hi=8;
    if(clip_on){*lo=1;*hi=7;if(y<1||y>4)*hi=*lo;}
    if(region_on){if(*lo<3)*lo=3;if(*hi>6)*hi=6;if(y<2||y>3)*hi=*lo;}
}
static void memset32_asm(uint32_t* p,uint32_t c,size_t n) {
    uintptr_t start=(uintptr_t)p,end=fb_addr+8*6*4;
    if(start<fb_addr||start>end||n>(end-start)/4){puts("FAIL: write outside framebuffer");exit(1);}
    for(size_t i=0;i<n;i++)p[i]=c;
}
@FILL@
int main(void) {
    const uint32_t dimensions[]={0,1,5,6,7,8,9,31,0xfffffff8u,0xffffffffu};
    int checks=0;
    for(int format=0;format<2;format++)for(int mask=0;mask<4;mask++)
    for(uint32_t x=0;x<=9;x++)for(uint32_t y=0;y<=7;y++)
    for(size_t wi=0;wi<sizeof(dimensions)/sizeof(dimensions[0]);wi++)
    for(size_t hi=0;hi<sizeof(dimensions)/sizeof(dimensions[0]);hi++) {
        uint32_t w=dimensions[wi],h=dimensions[hi];
        memset(storage,0x5a,sizeof(storage));memcpy(expected,storage,sizeof(storage));
        fb_addr=(uintptr_t)(storage+1);fb_bpp=format?32:8;
        clip_on=mask&1;region_on=mask&2;spans=0;
        for(uint32_t py=0;py<6;py++)for(uint32_t px=0;px<8;px++) {
            // Widen the reference arithmetic independently of the implementation.
            if(px<x||py<y||(uint64_t)px>=(uint64_t)x+w||(uint64_t)py>=(uint64_t)y+h)continue;
            // The existing indexed-colour path does not implement region clipping.
            if(format&&clip_on&&(px<1||px>=7||py<1||py>4))continue;
            if(format&&region_on&&(px<3||px>=6||py<2||py>3))continue;
            if(format)expected[1+py*8+px]=0x12345678;
            else ((uint8_t*)(expected+1))[py*8+px]=0x78;
        }
        fb_fill_rect(x,y,w,h,0x12345678);
        if(memcmp(storage,expected,sizeof(storage))){printf("FAIL: format%u mask%d origin%u,%u size%u,%u\n",fb_bpp,mask,x,y,w,h);return 1;}
        checks++;
    }
    fb_addr=0;fb_fill_rect(0,0,0xffffffffu,0xffffffffu,0);
    printf("Framebuffer bounds: %d cases, 0 failures\n",checks+1);return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', help='Check an earlier committed implementation')
    args = parser.parse_args()
    path = 'kernel/drivers/video/fb.c'
    source = subprocess.check_output(['git', 'show', args.ref + ':' + path], cwd=ROOT, text=True) if args.ref else (ROOT/path).read_text()
    body = function(source, 'void fb_fill_rect(')
    with tempfile.TemporaryDirectory(prefix='nyx-framebuffer-bounds-') as tmp:
        p = Path(tmp)
        (p/'check.c').write_text(HARNESS.replace('@FILL@', body))
        subprocess.run(['gcc', '-std=gnu99', '-O1', '-Wall', '-Wextra', '-Werror', '-no-pie',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        p/'check.c', '-o', p/'check'], check=True)
        subprocess.run([p/'check'], check=True, timeout=15)


if __name__ == '__main__':
    main()
