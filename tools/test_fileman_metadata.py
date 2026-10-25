#!/usr/bin/env python3
"""Exercise File Manager metadata errors, sorting and labels with the kernel formatter."""
from pathlib import Path
import subprocess
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "kernel/gui/apps/fileman_win.h"
@FORMAT@
void memcpy_asm(void* d,const void* s,size_t n) { for(size_t i=0;i<n;i++)((char*)d)[i]=((const char*)s)[i]; }
static dirent_t entries[FILEMAN_MAX_ENTRIES];
static int listing_count, listing_pos, open_error, stat_calls;
static uint32_t values[FILEMAN_MAX_ENTRIES];
static int errors[FILEMAN_MAX_ENTRIES], types[FILEMAN_MAX_ENTRIES];
int vfs_open(const char* p,int f,mode_t m) { (void)p;(void)f;(void)m;listing_pos=0;return open_error?-1:1; }
int vfs_close(int fd) { (void)fd;return 0; }
dirent_t* vfs_readdir(int fd) { (void)fd;return listing_pos<listing_count?&entries[listing_pos++]:NULL; }
int vfs_stat(const char* p,uint32_t* size,int* dir) {
    const char* name=p;for(int i=0;p[i];i++)if(p[i]=='/')name=p+i+1;
    stat_calls++;
    for(int i=0;i<listing_count;i++)if(!strcmp(entries[i].name,name)) {
        *size=values[i];*dir=types[i];return errors[i];
    }
    return -1;
}
static void fileman_apply_search(fileman_win_t* fm) { (void)fm; }
@FUNCTIONS@
static int checks;
#define CHECK(x) do { checks++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static int index_of(fileman_win_t* fm,const char* name) {
    for(int i=0;i<fm->entry_count;i++)if(!strcmp(fm->entries[i],name))return i;
    return -1;
}
int main(void) {
    static fileman_win_t fm;char text[128];strcpy(fm.cwd,"/mnt");
    const char* names[]={"empty","failed","large","other","wrongtype","folder"};
    listing_count=6;
    for(int i=0;i<listing_count;i++)strcpy(entries[i].name,names[i]);
    entries[5].type=1;errors[1]=-1;types[4]=1;
    values[2]=values[3]=0xffffffffu;values[1]=123;values[4]=456;
    fileman_refresh(&fm);
    CHECK(fm.summ_dirs==1 && fm.summ_files==5 && fm.summ_unknown==2);
    CHECK(fm.summ_bytes==8589934590ULL && fm.sel_index==-1);
    CHECK(!fm.entry_size_known[index_of(&fm,"failed")] && !fm.entry_size_known[index_of(&fm,"wrongtype")]);
    CHECK(fm.entry_size_known[index_of(&fm,"empty")] && !fm.entry_sizes[index_of(&fm,"empty")]);
    fileman_size_label(&fm,index_of(&fm,"empty"),text,sizeof text);CHECK(!strcmp(text,"0 B"));
    fileman_size_label(&fm,index_of(&fm,"failed"),text,sizeof text);CHECK(!strcmp(text,"Unknown"));
    fileman_size_label(&fm,index_of(&fm,"folder"),text,sizeof text);CHECK(!strcmp(text,"<DIR>"));
    fileman_size_label(&fm,index_of(&fm,"large"),text,sizeof text);CHECK(!strcmp(text,"4096 MB"));
    fileman_summary(&fm,text,sizeof text);CHECK(!strcmp(text,"1 dirs, 5 files, 8192 MB known"));
    for(int desc=0;desc<2;desc++) {
        fm.sort_key=FM_SORT_SIZE;fm.sort_desc=desc;fileman_sort(&fm);
        CHECK(!strcmp(fm.entries[0],"folder"));
        CHECK(fm.entry_size_known[1] && fm.entry_size_known[2] && fm.entry_size_known[3]);
        CHECK(!fm.entry_size_known[4] && !fm.entry_size_known[5]);
        CHECK(!strcmp(fm.entries[desc?3:1],"empty"));
        fm.sel_index=index_of(&fm,"failed");fileman_show_properties(&fm);
        CHECK(fm.props_open && !fm.props_size_known);fileman_property_size(&fm,text,sizeof text);CHECK(!strcmp(text,"Size: Unknown"));
        fm.sel_index=index_of(&fm,"large");fileman_show_properties(&fm);
        CHECK(fm.props_size_known && fm.props_size==0xffffffffu);
        fileman_property_size(&fm,text,sizeof text);CHECK(!strcmp(text,"Size: 4096 MB (4294967295 bytes)"));
    }
    fm.sel_index=index_of(&fm,"folder");fileman_show_properties(&fm);
    fileman_property_size(&fm,text,sizeof text);CHECK(!strcmp(text,"Size: --"));
    listing_count=1;strcpy(entries[0].name,"note");values[0]=9;
    for(int len=1;len<=255;len++) {
        for(int i=0;i<len;i++)fm.cwd[i]='d';
        fm.cwd[0]='/';fm.cwd[len]=0;
        stat_calls=0;fileman_refresh(&fm);int fits=len==1||len+5<256;
        CHECK(stat_calls==fits && fm.entry_size_known[0]==fits);
        CHECK(fm.summ_unknown==!fits && fm.summ_bytes==(fits?9u:0u));
    }
    strcpy(fm.cwd,"/mnt");open_error=1;fm.sel_index=0;fileman_refresh(&fm);
    CHECK(!fm.entry_count && fm.sel_index==-1 && !fm.summ_bytes && !fm.summ_files && !fm.summ_dirs && !fm.summ_unknown);
    open_error=0;listing_count=FILEMAN_MAX_ENTRIES;
    for(int i=0;i<listing_count;i++){snprintf(entries[i].name,sizeof entries[i].name,"file%03d",i);entries[i].type=0;errors[i]=types[i]=0;values[i]=0xffffffffu;}
    fileman_refresh(&fm);CHECK(fm.summ_bytes==2199023255040ULL && !fm.summ_unknown && fm.summ_files==512);
    fileman_summary(&fm,text,sizeof text);CHECK(!strcmp(text,"0 dirs, 512 files, 2097152 MB"));
    printf("File Manager metadata: %d checks, 0 failures\n",checks);return 0;
}
'''


def main():
    source=(ROOT/'kernel/gui/apps/fileman_win.c').read_text(encoding='utf-8')
    kernel=(ROOT/'kernel/core/kernel.c').read_text(encoding='utf-8')
    formatter='\n'.join(function(kernel,s) for s in ['static int u_fmt(', 'int vsnprintf(', 'int snprintf('])
    signatures=['static int fm_namecmp(', 'static int fm_entry_less(', 'static void fileman_sort(',
                'static int fileman_join_path(', 'static int fileman_get_path(', 'void fileman_refresh(',
                'static void fileman_fmt_size(', 'static void fileman_size_label(',
                'static void fileman_summary(', 'static void fileman_property_size(', 'static void fileman_show_properties(']
    text=HARNESS.replace('@FORMAT@',formatter).replace('@FUNCTIONS@','\n'.join(function(source,s) for s in signatures))
    with tempfile.TemporaryDirectory(prefix='nyx-fileman-metadata-') as tmp:
        p=Path(tmp);(p/'check.c').write_text(text,encoding='utf-8')
        subprocess.run(['gcc','-std=gnu99','-ffreestanding','-O1','-Wall','-Wextra','-Werror','-no-pie',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',ROOT,
                        p/'check.c','-o',p/'check'],check=True)
        subprocess.run([p/'check'],check=True)


if __name__=='__main__':
    main()
