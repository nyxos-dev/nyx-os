#!/usr/bin/env python3
"""Check move status at the actual syscall, userspace command and cut/paste boundaries."""
from pathlib import Path
import subprocess
import tempfile
from test_vfs_replace import function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "kernel/gui/apps/fileman_win.h"
void memcpy_asm(void* d, const void* s, size_t n) {
    for(size_t i=0;i<n;i++) ((unsigned char*)d)[i]=((const unsigned char*)s)[i];
}
static int rc, calls, refreshes, path_error;
static char source[256], destination[256];
int vfs_rename(const char* s, const char* d) { calls++; strcpy(source,s); strcpy(destination,d); return rc; }
int vfs_cp(const char* s, const char* d) { (void)s; (void)d; return -1; }
int vfs_open(const char* p, int f, mode_t m) { (void)p; (void)f; (void)m; return -1; }
int vfs_close(int fd) { (void)fd; return 0; }
void fileman_refresh(fileman_win_t* fm) { refreshes++; strcpy(fm->status,"entries"); fm->sel_index=-1; }
static int user_str_ok(uint64_t p) { return p!=0; }
static int copy_path_from_user(char* o, int n, uint64_t p) { (void)n; if(path_error)return -1; strcpy(o,(const char*)(uintptr_t)p); return 0; }
@REFRESH@
@PASTE@
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
    CHECK(!strcmp(fm.status,"Paste failed: path too long"));
    printf("Move callers: %d checks, 0 failures\n",checks); return 0;
}
'''


def main():
    fm=(ROOT/'kernel/gui/apps/fileman_win.c').read_text(encoding='utf-8')
    syscall=(ROOT/'kernel/core/syscall.c').read_text(encoding='utf-8')
    command=(ROOT/'user/mv.c').read_text(encoding='utf-8').replace('#include "libc.h"','')
    command=command.replace('int main(', 'static int move_command(').replace('rename(', 'command_rename(')
    code=HARNESS.replace('@REFRESH@',function(fm,'static void fileman_refresh_status('))
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
