#!/usr/bin/env python3
"""Host allocator metadata regression; locks mocked, physical RAM tested in QEMU."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[1]
source=(root/'kernel/mm/memory.c').read_text(encoding='utf-8')
source=re.sub(r'#include "([^"]+)"',lambda m:'#include "'+(root/'kernel/mm'/m[1]).resolve().as_posix()+'"',source)
pre='''#include "@ROOT@/kernel/core/types.h"
#define SPINLOCK_H
typedef struct { uint32_t locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static uint64_t spin_lock_irqsave(spinlock_t* l) { (void)l; return 0; }
static void spin_unlock_irqrestore(spinlock_t* l,uint64_t f) { (void)l; (void)f; }
'''.replace('@ROOT@',root.as_posix())
post=r'''
uint64_t memory_total,memory_used;
void memset_asm(void* d,uint8_t v,size_t n) { unsigned char* p=d;while(n--) *p++=v; }
static void* frames[262144];
static unsigned checks;
#define CHECK(c) do { checks++; if(!(c)){printf("pool FAIL line%d\n",__LINE__);return 1;} }while(0)
#define MIB (1024ULL*1024)
int main(void) {
    for(int layout=0;layout<2;layout++) {
        mb_mmap_entry_t map[]={{0,64*MIB,1},{64*MIB,32*MIB,2},
            {96*MIB,928*MIB,1},{4ULL*1024*MIB,1024*MIB,1}};
        if(!layout){map[0].len=1024*MIB;map[1].len=map[2].len=0;}
        init_memory(5ULL*1024*MIB,map,4);
        CHECK(get_total_pages()==262144);
        uint32_t before=get_free_pages(),n=0,high=0;void* p;
        while((p=alloc_page())) {
            uintptr_t a=(uintptr_t)p;
            CHECK(a>=MIB && a<1024*MIB && a%4096==0);
            CHECK(!layout || a<64*MIB || a>=96*MIB);
            CHECK(!n || (uintptr_t)frames[n-1]<a);
            CHECK(page_get_refcount(p)==1);
            if(a>=512*MIB) {
                high++;
                page_incref(p);CHECK(page_get_refcount(p)==2);
                free_page(p);CHECK(page_get_refcount(p)==1);
            }
            frames[n++]=p;
        }
        CHECK(high==131072 && n==before && !get_free_pages());
        CHECK(memory_used==(uint64_t)n*4096);
        for(uint32_t i=0;i<n;i++) free_page(frames[i]);
        CHECK(get_free_pages()==before && !memory_used);
        free_page(frames[n-1]);CHECK(get_free_pages()==before);
        printf("pool layout%d: %u frames, %u above512MiB, recovered%u\n",layout,n,high,before);
    }
    printf("PASS %u allocator metadata checks (host mocks locks; no physical dereference)\n",checks);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
    p=Path(t);(p/'p.c').write_text(pre+source+post,encoding='utf-8')
    subprocess.run(['gcc','-std=gnu99','-O1','-Wall','-Wextra','-Werror','-Wno-comment','-ffreestanding',
        '-ffunction-sections','-fdata-sections','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-Wl,--gc-sections','-Wl,--defsym,_kernel_end=0x100000',str(p/'p.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
