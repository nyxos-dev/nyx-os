#!/usr/bin/env python3
"""Check move status at the actual syscall, userspace command and File Manager path/copy boundaries."""
from pathlib import Path
import argparse
import subprocess
import re
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "kernel/gui/apps/fileman_win.h"
#include "kernel/drivers/video/font.h"
@GEOMETRY@
static int drop_target, physical_ctrl;
static unsigned captured_modifiers;
int is_ctrl_pressed(void) { return physical_ctrl; }
unsigned window_key_modifiers(void) { return captured_modifiers; }
static int fileman_row_at(fileman_win_t* fm, window_t* win, int my) { (void)fm;(void)win;(void)my;return drop_target; }
void memcpy_asm(void* d, const void* s, size_t n) {
    for(size_t i=0;i<n;i++) ((unsigned char*)d)[i]=((const unsigned char*)s)[i];
}
void memset_asm(void* d, uint8_t v, size_t n) {
    for(size_t i=0;i<n;i++) ((unsigned char*)d)[i]=v;
}
static dirent_t listing[2];
static int listing_open, listing_pos;
int vfs_stat(const char* p, uint32_t* n, int* d) { (void)p;(void)n;(void)d;return -1; }
dirent_t* vfs_readdir(int fd) { (void)fd; return listing_pos<2 ? &listing[listing_pos++] : NULL; }
static void fileman_sort(fileman_win_t* fm) { (void)fm; }
@SEARCH@
static void fileman_cd(fileman_win_t* fm, const char* dir, window_t* win) { (void)fm;(void)dir;(void)win; }
static void fileman_activate_file(fileman_win_t* fm, const char* path, const char* name) { (void)fm;(void)path;(void)name; }
int vfs_mkdir(const char* path, mode_t mode) { (void)path;(void)mode;return -1; }
static int rc, calls, refreshes, path_error, copy_calls, occupied, copy_error, deleted;
static char source[256], destination[256];
int vfs_rename(const char* s, const char* d) { calls++; strcpy(source,s); strcpy(destination,d); return rc; }
int vfs_cp_new(const char* s, const char* d) {
    strcpy(source,s);strcpy(destination,d);copy_calls++;
    return copy_error ? -1 : copy_calls<=occupied ? 1 : 0;
}
int vfs_unlink(const char* p) { strcpy(destination,p);deleted++;return 0; }
int vfs_open(const char* p, int f, mode_t m) { (void)p; (void)f; (void)m; return listing_open ? 1 : -1; }
int vfs_close(int fd) { (void)fd; return 0; }
void fileman_refresh(fileman_win_t* fm) { refreshes++; strcpy(fm->status,"entries"); fm->sel_index=-1; }
static int user_str_ok(uint64_t p) { return p!=0; }
static int copy_path_from_user(char* o, int n, uint64_t p) { (void)n; if(path_error)return -1; strcpy(o,(const char*)(uintptr_t)p); return 0; }
@REFRESH@
@PATH@
@ACTUAL_REFRESH@
@COPY@
@CUT@
@DELETE@
@PASTE@
@MOUSE@
@KEY@
static uint64_t rename_syscall(uint64_t a1, uint64_t a2) @SYSCALL@
static int command_rename(const char* s, const char* d) { return vfs_rename(s,d); }
@COMMAND@
static int checks;
#define CHECK(x) do { checks++; if(!(x)) { printf("line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    static fileman_win_t fm;
    for(int fail=0;fail<2;fail++) {
        strcpy(fm.cwd,"/mnt/dest"); strcpy(fm.clipboard_path,"/mnt/source/note"); fm.clipboard_mode=2;
        rc=fail?-1:0; calls=refreshes=0; ctx_paste(&fm);
        CHECK(calls==1 && refreshes==1); CHECK(!strcmp(source,"/mnt/source/note") && !strcmp(destination,"/mnt/dest/note"));
        CHECK(fm.clipboard_mode==(fail?2:0)); CHECK(fail?!strcmp(fm.clipboard_path,source):!fm.clipboard_path[0]);
        CHECK(fail?!strcmp(fm.status,"Move failed; cut retained"):!strcmp(fm.status,"Moved: note"));
        calls=0; CHECK((int64_t)rename_syscall((uint64_t)source,(uint64_t)destination)==rc); CHECK(calls==1);
        char* argv[]={"mv","/mnt/source/note","/mnt/dest/note",0}; calls=0;
        CHECK(move_command(3,argv)==fail && calls==1);
    }
    calls=0; CHECK((int64_t)rename_syscall(0,(uint64_t)destination)==-1 && !calls);
    path_error=1; CHECK((int64_t)rename_syscall((uint64_t)source,(uint64_t)destination)==-1 && !calls);
    char* args[]={"mv",0}; CHECK(move_command(1,args)==1 && !calls);
    for(int i=0;i<255;i++)fm.cwd[i]='a';
    fm.cwd[255]=0;
    strcpy(fm.clipboard_path,"/mnt/source/note");fm.clipboard_mode=2;ctx_paste(&fm);
    CHECK(!calls && fm.clipboard_mode==2 && !strcmp(fm.clipboard_path,"/mnt/source/note"));
    CHECK(!strcmp(fm.status,"Path too long"));
    char out[256];
    for(int length=1;length<=255;length++) {
        memset_asm(fm.cwd,'d',length);fm.cwd[0]='/';fm.cwd[length]=0;
        strcpy(out,"UNCHANGED");
        int fit=length==1 || length+5<256;
        CHECK(fileman_get_path(&fm,"note",out,sizeof out)==(fit?0:-1));
        CHECK(fit ? strlen(out)==(unsigned)(length==1?5:length+5) : !strcmp(out,"UNCHANGED"));
    }
    strcpy(fm.cwd,"/mnt"); strcpy(out,"UNCHANGED");
    CHECK(fileman_get_path(&fm,"note",out,0)==-1 && !strcmp(out,"UNCHANGED"));
    fm.sel_index=0;fm.entry_count=1;strcpy(fm.entries[0],"note");
    memset_asm(fm.cwd,'d',251);fm.cwd[0]='/';fm.cwd[251]=0;
    strcpy(fm.clipboard_path,"/saved/cut");fm.clipboard_mode=2;
    ctx_copy(&fm); CHECK(fm.clipboard_mode==2 && !strcmp(fm.clipboard_path,"/saved/cut"));
    ctx_cut(&fm); CHECK(fm.clipboard_mode==2 && !strcmp(fm.clipboard_path,"/saved/cut"));
    fileman_delete(&fm); CHECK(!deleted);
    for(int collision=0;collision<=3;collision++) {
        strcpy(fm.cwd,"/mnt");strcpy(fm.clipboard_path,"/source/note");fm.clipboard_mode=1;
        copy_calls=0;occupied=collision;copy_error=0;ctx_paste(&fm);
        const char* expected[]={"/mnt/note","/mnt/note_copy","/mnt/note_copy2","/mnt/note_copy3"};
        CHECK(copy_calls==collision+1 && !strcmp(destination,expected[collision]));
        CHECK(fm.clipboard_mode==1 && !strcmp(fm.clipboard_path,"/source/note"));
    }
    copy_calls=0;occupied=3;copy_error=1;ctx_paste(&fm);
    CHECK(copy_calls==1 && !strcmp(fm.status,"Paste failed"));
    copy_calls=0;occupied=100;copy_error=0;ctx_paste(&fm);
    CHECK(copy_calls==100 && !strcmp(fm.status,"Copy names exhausted"));
    memset_asm(fm.clipboard_path,'a',63);fm.clipboard_path[63]=0;
    copy_calls=0;occupied=1;ctx_paste(&fm);
    CHECK(copy_calls==1 && !strcmp(fm.status,"Copy name too long"));
    memset_asm(fm.cwd,'d',246);fm.cwd[0]='/';fm.cwd[246]=0;strcpy(fm.clipboard_path,"/source/note");
    copy_calls=0;occupied=1;ctx_paste(&fm);
    CHECK(copy_calls==1 && !strcmp(fm.status,"Path too long"));
    window_t win={0};win.reserved=&fm;win.h=400;
    fm.entry_count=2;fm.sel_index=0;fm.ctx_open=0;fm.entry_types[1]=1;drop_target=1;
    for(int fault=0;fault<2;fault++) {
        int length=fault ? 246 : 251;
        memset_asm(fm.cwd,'d',length);fm.cwd[0]='/';fm.cwd[length]=0;
        strcpy(fm.entries[0],fault?"a":"note");strcpy(fm.entries[1],"folder88");
        fm.drag_active=fm.mouse_down=1;fm.drag_file_idx=0;fm.drag_mode=2;calls=copy_calls=0;
        fileman_win_mousemove(&win,0,0,0);
        CHECK(!calls && !copy_calls && !strcmp(fm.status,"Path too long"));
        CHECK(!fm.mouse_down && !fm.drag_active && fm.drag_file_idx==-1);
    }
    strcpy(fm.cwd,"/mnt");fm.drag_active=fm.mouse_down=1;fm.drag_file_idx=0;
    calls=copy_calls=0;occupied=1;fileman_win_mousemove(&win,0,0,0);
    CHECK(!calls && copy_calls==1 && !strcmp(fm.status,"Copy failed (name exists?)"));
    CHECK(!fm.mouse_down && !fm.drag_active && fm.drag_file_idx==-1);
    listing_open=1;listing_pos=0;strcpy(fm.cwd,"/mnt");
    memset_asm(listing[0].name,'x',64);listing[0].name[64]=0;
    memset_asm(listing[1].name,'x',63);listing[1].name[63]=0;
    actual_refresh(&fm);
    CHECK(fm.entry_count==1 && strlen(fm.entries[0])==63);
    CHECK(!strcmp(fm.status,"Names over 63 bytes hidden: 1"));
    memset_asm(&fm,0,sizeof fm);strcpy(fm.cwd,"/mnt");fm.entry_count=3;
    strcpy(fm.entries[0],"alpha");strcpy(fm.entries[1],"note");strcpy(fm.entries[2],"zulu");
    CHECK(fm_disp_count(&fm)==3);
    const int copy_keys[]={3,'c','C'}, cut_keys[]={24,'x','X'};
    for(int form=0;form<3;form++) {
        fm.sel_index=1;physical_ctrl=0;captured_modifiers=KEY_MOD_CTRL;
        fileman_win_key(&win,copy_keys[form]);
        CHECK(fm.clipboard_mode==1 && !strcmp(fm.clipboard_path,"/mnt/note"));
        fileman_win_key(&win,cut_keys[form]);
        CHECK(fm.clipboard_mode==2 && !strcmp(fm.clipboard_path,"/mnt/note"));
        rc=-1;calls=0;fileman_win_key(&win,22);
        CHECK(calls==1 && fm.clipboard_mode==2 && !strcmp(fm.status,"Move failed; cut retained"));
    }
    fm.sel_index=1;fileman_win_key(&win,3);occupied=1;copy_error=0;copy_calls=0;
    fileman_win_key(&win,22);
    CHECK(copy_calls==2 && !strcmp(destination,"/mnt/note_copy") && fm.clipboard_mode==1);
    fm.sel_index=1;fileman_win_key(&win,24);rc=0;calls=0;fileman_win_key(&win,22);
    CHECK(calls==1 && !fm.clipboard_mode && !fm.clipboard_path[0]);
    for(int bad=-1;bad<=FILEMAN_MAX_ENTRIES;bad+=FILEMAN_MAX_ENTRIES+1) {
        fm.sel_index=bad;strcpy(fm.clipboard_path,"/prior");fm.clipboard_mode=2;
        fileman_win_key(&win,3);fileman_win_key(&win,24);
        CHECK(fm.clipboard_mode==2 && !strcmp(fm.clipboard_path,"/prior"));
    }
    fm.sel_index=1;memset_asm(fm.cwd,'d',251);fm.cwd[0]='/';fm.cwd[251]=0;
    fileman_win_key(&win,3);CHECK(!strcmp(fm.clipboard_path,"/prior") && !strcmp(fm.status,"Path too long"));
    strcpy(fm.cwd,"/mnt");fm.clipboard_mode=0;fm.clipboard_path[0]=0;
    captured_modifiers=0;physical_ctrl=1;calls=copy_calls=0;
    fileman_win_key(&win,'c');fileman_win_key(&win,3);fileman_win_key(&win,24);fileman_win_key(&win,22);fileman_win_key(&win,6);
    CHECK(!fm.clipboard_mode && !calls && !copy_calls && !fm.search_active);
    captured_modifiers=KEY_MOD_CTRL|KEY_MOD_ALT;fileman_win_key(&win,3);fileman_win_key(&win,6);
    CHECK(!fm.clipboard_mode && !fm.search_active);
    physical_ctrl=0;captured_modifiers=KEY_MOD_CTRL;fileman_win_key(&win,6);
    CHECK(fm.search_active && !fm.search_pattern[0]);
    captured_modifiers=0;fileman_win_key(&win,'N');fileman_win_key(&win,'o');
    CHECK(!strcmp(fm.search_pattern,"No") && fm.search_count==1 && fm.search_indices[0]==1);
    captured_modifiers=KEY_MOD_CTRL;fileman_win_key(&win,1);
    CHECK(fm.sel_index==1 && !strcmp(fm.status,"Selected: note"));
    fileman_win_key(&win,3);CHECK(!strcmp(fm.clipboard_path,"/mnt/note"));
    captured_modifiers=0;fileman_win_key(&win,'q');CHECK(fm.search_count==0 && fm.sel_index==-1);
    captured_modifiers=KEY_MOD_CTRL;fileman_win_key(&win,1);fileman_win_key(&win,24);
    CHECK(fm.sel_index==-1 && fm.clipboard_mode==1 && !strcmp(fm.clipboard_path,"/mnt/note"));
    fileman_win_key(&win,6);CHECK(!fm.search_active && !fm.search_pattern[0]);
    fileman_win_key(&win,1);CHECK(fm.sel_index==0);
    for(int mode=1;mode<=3;mode++) {
        fm.input_mode=mode;strcpy(fm.input_buf,"draft");fm.input_pos=5;calls=copy_calls=0;
        const int controls[]={1,3,6,22,24};
        for(unsigned i=0;i<sizeof controls/sizeof controls[0];i++)fileman_win_key(&win,controls[i]);
        CHECK(fm.input_mode==mode && !strcmp(fm.input_buf,"draft") && !fm.search_active);
        CHECK(!calls && !copy_calls && fm.clipboard_mode==1 && !strcmp(fm.clipboard_path,"/mnt/note"));
        captured_modifiers=0;fileman_win_key(&win,'x');CHECK(!strcmp(fm.input_buf,"draftx"));
        captured_modifiers=KEY_MOD_CTRL;
    }
    printf("Move callers: %d checks, 0 failures\n",checks); return 0;
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fileman-source',type=Path,default=ROOT/'kernel/gui/apps/fileman_win.c')
    args=parser.parse_args()
    fm=args.fileman_source.read_text(encoding='utf-8')
    syscall=(ROOT/'kernel/core/syscall.c').read_text(encoding='utf-8')
    command=(ROOT/'user/mv.c').read_text(encoding='utf-8').replace('#include "libc.h"','')
    command=command.replace('int main(', 'static int move_command(').replace('rename(', 'command_rename(')
    code=HARNESS.replace('@REFRESH@',function(fm,'static void fileman_refresh_status('))
    geometry='\n'.join(line for line in fm.splitlines() if re.match(r'#define (TOOLBAR_H|HEADER_H|CTX_W|CTX_ITEM_H|CTX_ITEMS_COUNT) ',line))
    items=next(line for line in fm.splitlines() if line.startswith('static const char* ctx_items[]'))
    code=code.replace('@GEOMETRY@',items+'\n'+geometry)
    code=code.replace('@MOUSE@',function(fm,'void fileman_win_mousemove('))
    code=code.replace('@KEY@',function(fm,'void fileman_win_key('))
    search='\n'.join(function(fm,sig) for sig in ['static int str_contains(', 'static void fileman_apply_search(', 'static int fm_disp_count(', 'static int fm_pos_to_real(', 'static int fm_sel_to_pos('])
    code=code.replace('@SEARCH@',search)
    code=code.replace('@ACTUAL_REFRESH@',function(fm,'void fileman_refresh(').replace('void fileman_refresh(', 'static void actual_refresh('))
    for marker, sig in [('PATH','static int fileman_join_path('),('COPY','static void ctx_copy('),('CUT','static void ctx_cut('),('DELETE','static void fileman_delete(')]:
        body=function(fm,sig)
        if marker=='PATH':body+='\n'+function(fm,'static int fileman_get_path(')
        code=code.replace('@'+marker+'@',body)
    code=code.replace('@PASTE@',function(fm,'static void ctx_paste('))
    code=code.replace('@SYSCALL@',function(syscall,'case SYS_RENAME:').split(':',1)[1].strip())
    code=code.replace('@COMMAND@',command)
    with tempfile.TemporaryDirectory(prefix='nyx-move-callers-') as tmp:
        p=Path(tmp);(p/'test.c').write_text(code,encoding='utf-8')
        subprocess.run(['gcc','-std=gnu99','-ffreestanding','-O1','-Wall','-Wextra','-Werror','-no-pie',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',ROOT,
                        p/'test.c','-o',p/'test'],check=True)
        subprocess.run([p/'test'],check=True)


if __name__ == '__main__':
    main()
