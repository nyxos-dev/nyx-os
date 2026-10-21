#!/usr/bin/env python3
"""Exercise dirty-editor close/reload decisions and deferred compositor lifetime."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_editor_io import HARNESS, ROOT, function_source

REGISTRY = r'''
static window_t* windows[MAX_WINDOWS];
static int window_count, focused_id, destroyed, checks;
static window_t* find_window(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i] && windows[i]->id == id) return windows[i];
    return 0;
}
static void focus_next_window(void) { focused_id = 0; }
'''

CASES = r'''
#undef CHECK
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void release_editor(window_t* win) { destroyed++; editor_win_close(win); }
static window_t* create_editor(void) {
    window_t* w = kmalloc(sizeof(*w)); memset_asm(w, 0, sizeof(*w));
    w->reserved = editor_create_ctx(); w->id = 7; w->w = 600; w->h = 400;
    w->x = 150; w->y = 120; w->on_close = release_editor; w->can_close = editor_win_can_close;
    windows[0] = w; window_count = 1; focused_id = 7;
    seed(w->reserved); return w;
}
int main(void) {
    output_fd = open("/dev/null", 1);
    CHECK(output_fd >= 0);
    for (int failure = 0; failure < 3; failure++) {
        window_t* w = create_editor(); editor_win_t* ed = w->reserved;
        int old_undo = ed->undo_n, old_cursor = ed->cursor_x;
        int old_selection = ed->selection_active, old_writes = writes;
        window_request_close(7);
        CHECK(windows[0] && window_count == 1); /* old immediate close fails here */
        CHECK(window_process_close_requests() == 1 && windows[0] == w);
        CHECK(ed->close_pending && ed->modified && !ed->selecting);
        editor_win_key(w, 'x'); editor_win_key(w, 26); editor_win_key(w, '\n');
        editor_win_click(w, 190, 200, 1);
        CHECK(ed->undo_n == old_undo && ed->cursor_x == old_cursor);
        CHECK(ed->selection_active == old_selection && writes == old_writes);
        CHECK(!strcmp(ed->lines[0], "previous"));
        window_request_close(7); window_process_close_requests();
        CHECK(ed->close_pending && windows[0] == w);
        editor_win_key(w, 27);
        CHECK(!ed->close_pending && ed->modified && ed->undo_n == old_undo);
        window_request_close(7); window_process_close_requests();
        no_alloc = failure == 0; write_mode = failure;
        editor_win_key(w, 19); /* Ctrl+S */
        CHECK(ed->modified && ed->close_pending && ed->close_failed);
        CHECK(!w->close_requested && window_process_close_requests() == 0);
        CHECK(!strncmp(ed->status, "Save failed:", 12));
        CHECK(ed->undo_n == old_undo && !strcmp(ed->lines[0], "previous"));
        no_alloc = write_mode = 0;
        editor_win_key(w, 27); CHECK(!ed->close_failed && !ed->close_pending);
        window_request_close(7); window_process_close_requests();
        int x,y,width,bx,by,bw; editor_close_box(w,&x,&y,&width);
        editor_close_button(x,y,width,0,&bx,&by,&bw);
        editor_win_click(w,bx+1,by+1,1);
        CHECK(!ed->modified && w->close_requested && windows[0] == w);
        editor_win_pressed(w,bx+1,by+1,1); /* compositor's second callback remains safe */
        CHECK(window_process_close_requests() == 1 && !windows[0]);
        CHECK(live_allocs == 0 && !guard_error && window_count == 0);
    }
    for (int narrow = 0; narrow < 2; narrow++) {
        window_t* w = create_editor(); editor_win_t* ed = w->reserved;
        if (narrow) { w->w = 120; w->h = 80; }
        window_request_close(7); window_process_close_requests();
        int x,y,width,bx,by,bw; editor_close_box(w,&x,&y,&width);
        CHECK(x >= WIN_CLIENT_X(w) && x+width <= WIN_CLIENT_X(w)+(int)w->w);
        CHECK(y >= WIN_CLIENT_Y(w) && y+80 <= WIN_CLIENT_Y(w)+(int)w->h);
        for (int i=0;i<3;i++) {
            editor_close_button(x,y,width,i,&bx,&by,&bw);
            CHECK(bx >= x && bx+bw <= x+width && by >= y && by+16 <= y+80);
        }
        editor_close_button(x,y,width,2,&bx,&by,&bw);
        editor_win_click(w,bx+1,by+1,1); CHECK(!ed->close_pending && ed->modified);
        window_request_close(7); window_process_close_requests();
        int old_writes = writes;
        if (narrow) editor_win_key(w,'d');
        else {
            editor_close_button(x,y,width,1,&bx,&by,&bw);
            editor_win_click(w,bx+1,by+1,1);
            editor_win_pressed(w,bx+1,by+1,1);
        }
        CHECK(ed->close_discard && ed->modified && writes == old_writes);
        CHECK(windows[0] == w && window_process_close_requests() == 1 && !windows[0]);
        CHECK(live_allocs == 0 && !guard_error);
    }
    window_t* w = create_editor(); ((editor_win_t*)w->reserved)->modified = 0;
    window_request_close(7); CHECK(windows[0] == w);
    CHECK(window_process_close_requests() == 1 && !windows[0]);
    window_request_close(999); CHECK(window_process_close_requests() == 0);
    w = create_editor(); w->can_close = 0;
    window_request_close(7); CHECK(window_process_close_requests() == 1 && !windows[0]);
    w = create_editor(); window_destroy(w->id); /* forced cleanup ignores prompt */
    CHECK(!windows[0] && destroyed == 8 && live_allocs == 0 && !guard_error);
    close(output_fd);
    printf("PASS %d editor/deferred close checks\n",checks);
    return 0;
}
'''



RELOAD_CASES = r'''
extern int fflush(void* stream);
#undef CHECK
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); fflush(NULL); return 1; } } while (0)
static void release_editor(window_t* win) { destroyed++; editor_win_close(win); }
static void choose_reload_save_path(window_t* w) {
    editor_win_key(w,1);
    const char* path="/mnt/home/test/named.txt";
    while (*path) editor_win_key(w,*path++);
    editor_win_key(w,13); editor_win_key(w,13);
}
static void reload_request(window_t* w, int route) {
    if (route == 0) editor_win_key(w, 15);
    else editor_win_click(w, WIN_CLIENT_X(w)+10, WIN_CLIENT_Y(w)+5, 1);
}
int main(void) {
    static uint8_t disk[4096];
    output_fd = open("/dev/null", 1); CHECK(output_fd >= 0);
    input_data = disk;
    strcpy(mount.mount_point,"/mnt"); mount.resolve=backing_resolve; mount.get_size=backing_size;
    for (int route=0; route<3; route++) for (int choice=0; choice<7; choice++) {
        strcpy((char*)disk,"disk\nfile"); input_size=image_size=9;
        window_t* w=kmalloc(sizeof(*w)); CHECK(w);
        memset_asm(w,0,sizeof(*w)); w->id=7; w->w=600; w->h=400;
        w->reserved=editor_create_ctx(); w->on_close=release_editor; w->can_close=editor_win_can_close;
        windows[0]=w; window_count=1; focused_id=7;
        editor_win_t* ed=w->reserved; seed(ed);
        if (route==2) ed->filename[0]=0;
        editor_win_t* before=malloc(sizeof(*before)); CHECK(before);
        memcpy_asm(before,ed,sizeof(*ed)); before->selecting=0;
        int old_writes=writes, old_opens=opens;
        reload_request(w,route);
        CHECK(ed->close_pending && ed->reload_pending && ed->modified);
        CHECK(!strcmp(ed->reload_path,route==2 ? "/home/test.txt" : "/mnt/original.txt"));
        CHECK(ed->undo_n==before->undo_n && same(ed->lines,before->lines,sizeof(ed->lines)));
        CHECK(opens==old_opens+(route==2) && writes==old_writes && !handles);
        editor_win_key(w,'x'); editor_win_key(w,26); reload_request(w,route);
        window_request_close(7); window_process_close_requests();
        CHECK(windows[0]==w && ed->reload_pending && !w->close_requested);
        CHECK(ed->selection_active==before->selection_active && ed->cursor_x==before->cursor_x);
        if (choice==0) {
            editor_win_key(w,27);
            /* Ignore the inactive path's stale bytes and cursor blink tick only. */
            memcpy_asm(before->reload_path,ed->reload_path,sizeof(ed->reload_path));
            before->cursor_tick=ed->cursor_tick;
            CHECK(state_preserved(ed,before));
        } else if (choice<=3) {
            no_alloc=choice==1; write_mode=choice==2 ? 1 : choice==3 ? 2 : 0;
            editor_win_key(w,'s');
            if (route==2) { CHECK(ed->save_stage==1 && writes==old_writes); choose_reload_save_path(w); }
            CHECK(ed->close_pending && ed->reload_pending && (ed->close_failed || ed->save_failed) && ed->modified);
            CHECK(ed->undo_n==before->undo_n && same(ed->lines,before->lines,sizeof(ed->lines)));
            CHECK(!w->close_requested && windows[0]==w && !handles);
            no_alloc=write_mode=0; editor_win_key(w,27);
            CHECK(!ed->close_pending && !ed->reload_pending && !ed->close_discard);
        } else if (choice==4) {
            /* The target disappears after the prompt; Discard must not erase RAM. */
            no_open=1; editor_win_key(w,'d'); no_open=0;
            CHECK(ed->modified && ed->undo_n==before->undo_n);
            CHECK(same(ed->lines,before->lines,sizeof(ed->lines)) && !strcmp(ed->filename,before->filename));
            CHECK(writes==old_writes && !ed->close_pending && !ed->close_discard);
            /* A later Close must still ask, even after a failed Discard/reload. */
            window_request_close(7); window_process_close_requests();
            CHECK(windows[0]==w && ed->close_pending && !ed->reload_pending);
            editor_win_key(w,27);
        } else if (choice==5) {
            editor_win_key(w,'d');
            CHECK(!ed->modified && !ed->undo_n && !strcmp(ed->lines[0],"disk"));
            CHECK(writes==old_writes && !strcmp(ed->filename,route==2 ? "/home/test.txt" : "/mnt/original.txt"));
        } else {
            /* VFS write mock updates only the named target, never the fallback file. */
            editor_win_key(w,19);
            if (route==2) { CHECK(ed->save_stage==1 && writes==old_writes); choose_reload_save_path(w); }
            CHECK(writes==old_writes+1 && !ed->modified && !ed->undo_n);
            CHECK(!strcmp(saved_path,route==2 ? "/mnt/home/test/named.txt" : "/mnt/original.txt"));
            CHECK(!strcmp(ed->filename,route==2 ? "/home/test.txt" : "/mnt/original.txt"));
            CHECK(!strcmp(ed->lines[0],route==2 ? "disk" : "previous"));
        }
        CHECK(windows[0]==w && !ed->close_pending && !ed->reload_pending && !w->close_requested);
        editor_win_pressed(w,10,10,1); CHECK(!ed->selecting);
        window_destroy(7); free(before);
        CHECK(!live_allocs && !guard_error && !handles && !bad_fd);
    }
    /* Clean documents reload immediately; malformed input preserves a dirty one. */
    window_t w; memset_asm(&w,0,sizeof(w)); w.w=600; w.h=400;
    editor_win_t* ed=editor_create_ctx(); w.reserved=ed; seed(ed); ed->modified=0;
    strcpy((char*)disk,"clean"); input_size=image_size=5;
    reload_request(&w,0); CHECK(!ed->close_pending && !strcmp(ed->lines[0],"clean"));
    ed->modified=1; editor_push_undo(ed);
    reload_request(&w,1); disk[1]=0; editor_win_key(&w,'d');
    CHECK(ed->modified && ed->undo_n==1 && !strcmp(ed->lines[0],"clean"));
    editor_win_close(&w); kfree(ed);
    CHECK(!live_allocs && !guard_error && !handles && !bad_fd);
    close(output_fd); printf("PASS %d editor reload checks\n",checks); return 0;
}
'''


SAVEAS_CASES = r'''
extern int fflush(void* stream);
#undef CHECK
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); fflush(NULL); return 1; } } while (0)
static void release_editor(window_t* win) { destroyed++; editor_win_close(win); }
static void type_path(window_t* w, const char* path) {
    editor_win_key(w,1); editor_win_key(w,KEY_DEL); while (*path) editor_win_key(w,*path++);
}
int main(void) {
    static uint8_t original[]="original";
    input_data=original;input_size=image_size=8;
    strcpy(mount.mount_point,"/mnt"); mount.resolve=backing_resolve; mount.get_size=backing_size;
    output_fd=open("/dev/null",1); CHECK(output_fd>=0);
    for (int named=0;named<2;named++) for (int route=0;route<3;route++) for (int action=0;action<6;action++) {
        window_t* w=kmalloc(sizeof(*w)); CHECK(w);memset_asm(w,0,sizeof(*w));w->id=7;w->w=600;w->h=400;
        w->reserved=editor_create_ctx();w->can_close=editor_win_can_close;w->on_close=release_editor;
        windows[0]=w;window_count=1;focused_id=7;
        editor_win_t* ed=w->reserved;seed(ed);if(!named)ed->filename[0]=0;
        int old_undo=ed->undo_n,old_writes=writes;char old_name[64];strcpy(old_name,ed->filename);
        if(route==0) editor_win_key(w,KEY_F2);
        else if(route==1) editor_win_click(w,WIN_CLIENT_X(w)+130,WIN_CLIENT_Y(w)+5,1);
        else if(!named) editor_win_key(w,19);
        else { window_request_close(7);window_process_close_requests();editor_win_key(w,27);editor_win_key(w,KEY_F2); }
        CHECK(ed->save_stage==1 && writes==old_writes);
        CHECK(!strcmp(ed->filename,old_name) && ed->modified && ed->undo_n==old_undo);
        CHECK(ed->selection_active && !ed->selecting && !strcmp(ed->lines[0],"previous"));
        window_request_close(7);window_process_close_requests();CHECK(windows[0]==w && ed->save_stage==1);
        type_path(w,"/mnt/new.txt");CHECK(!strcmp(ed->save_input,"/mnt/new.txt"));
        editor_win_key(w,KEY_HOME);editor_win_key(w,KEY_END);editor_win_key(w,KEY_LEFT);editor_win_key(w,KEY_RIGHT);
        CHECK(ed->save_cursor==12);
        editor_win_key(w,13);CHECK(ed->save_stage==2 && writes==old_writes);
        CHECK(!strcmp(ed->save_path,"/mnt/new.txt"));
        editor_win_key(w,'x');editor_win_key(w,26);CHECK(!strcmp(ed->save_path,"/mnt/new.txt"));
        editor_win_key(w,KEY_HOME);CHECK(ed->save_cursor==0);editor_win_key(w,KEY_END);CHECK(ed->save_cursor==12);
        if(action==0) {
            editor_win_key(w,27);CHECK(!ed->save_stage && writes==old_writes && !strcmp(ed->filename,old_name));
        } else {
            no_alloc=action==1;write_mode=action==2 ? 1 : action==3 ? 2 : 0;
            if(action==5) { /* named clean SaveAs must report write failure too */
                ed->modified=0;no_alloc=1;
            }
            editor_win_key(w,13);
            if(action!=4) {
                CHECK(ed->save_stage==2 && ed->save_failed && !strcmp(ed->filename,old_name));
                CHECK(ed->undo_n==old_undo && !strcmp(ed->lines[0],"previous"));
                CHECK(!w->close_requested && windows[0]==w);
                no_alloc=write_mode=0;editor_win_key(w,13);
            }
            CHECK(!ed->save_stage && !ed->modified && !strcmp(ed->filename,"/mnt/new.txt"));
            CHECK(ed->undo_n==old_undo && !strcmp(last_saved,"previous\r\nkeep"));
        }
        CHECK(windows[0]==w && !w->close_requested && !ed->close_pending);
        editor_win_pressed(w,10,10,1);CHECK(!ed->selecting);
        window_destroy(7);CHECK(!live_allocs && !guard_error && !handles && !bad_fd);
    }
    /* First-save paths are explicit and independent for two new documents. */
    char first_path[MAX_PATH];
    for(int i=0;i<2;i++) {
        window_t w;memset_asm(&w,0,sizeof(w));w.w=600;w.h=400;w.reserved=editor_create_ctx();
        editor_win_t* ed=w.reserved;strcpy(ed->lines[0],i?"second note":"first note");ed->modified=1;
        int old_writes=writes;editor_win_key(&w,19);CHECK(ed->save_stage==1 && writes==old_writes);
        type_path(&w,i?"/mnt/second.txt":"/mnt/first.txt");editor_win_key(&w,13);CHECK(writes==old_writes);
        editor_win_key(&w,13);CHECK(!ed->modified && writes==old_writes+1 && !strcmp(last_saved,i?"second note":"first note"));
        if(!i)strcpy(first_path,saved_path);else CHECK(strcmp(first_path,saved_path));
        editor_win_close(&w);kfree(ed);
    }
    /* Pending Close waits for target and confirmation, then still defers destruction. */
    for(int cancel=0;cancel<2;cancel++) {
        window_t* w=kmalloc(sizeof(*w));memset_asm(w,0,sizeof(*w));w->id=7;w->w=120;w->h=80;
        w->reserved=editor_create_ctx();w->can_close=editor_win_can_close;w->on_close=release_editor;
        windows[0]=w;window_count=1;editor_win_t* ed=w->reserved;seed(ed);ed->filename[0]=0;
        window_request_close(7);window_process_close_requests();editor_win_key(w,'s');
        CHECK(ed->close_pending && ed->save_stage==1 && !w->close_requested);
        int x,y,width,bx,by,bw;editor_close_box(w,&x,&y,&width);
        for(int i=0;i<2;i++){editor_save_button(x,y,width,i,&bx,&by,&bw);CHECK(bx>=x && bx+bw<=x+width && by+16<=y+80);}
        type_path(w,"/mnt/close.txt");editor_save_button(x,y,width,0,&bx,&by,&bw);editor_win_click(w,bx+1,by+1,1);
        CHECK(ed->save_stage==2 && ed->close_pending);
        editor_save_button(x,y,width,cancel,&bx,&by,&bw);editor_win_click(w,bx+1,by+1,1);editor_win_pressed(w,bx+1,by+1,1);
        if(cancel){CHECK(!ed->save_stage && !ed->close_pending && !w->close_requested && ed->modified);window_destroy(7);}
        else {CHECK(windows[0]==w && w->close_requested);window_process_close_requests();CHECK(!windows[0]);}
        CHECK(!live_allocs && !guard_error);
    }
    window_t w;memset_asm(&w,0,sizeof(w));w.w=600;w.h=400;w.reserved=editor_create_ctx();editor_win_t* ed=w.reserved;seed(ed);
    editor_win_key(&w,KEY_F2);int old_writes=writes;
    const char* bad[]={"","/mnt/","/proc/x","/dev/x","/mnt/link/x"};
    linked_path="/mnt/link";
    for(int i=0;i<5;i++){type_path(&w,bad[i]);editor_win_key(&w,13);CHECK(ed->save_stage==1 && writes==old_writes && !strcmp(ed->filename,"/mnt/original.txt"));}
    linked_path=0;type_path(&w,"/");for(int i=0;i<200;i++)editor_win_key(&w,'q');
    CHECK(strlen(ed->save_input)==127 && ed->save_cursor==127);editor_win_key(&w,13);CHECK(ed->save_stage==1 && writes==old_writes);
    type_path(&w,"/mnt/ab.txt");editor_win_key(&w,KEY_HOME);editor_win_key(&w,KEY_RIGHT);editor_win_key(&w,KEY_DEL);CHECK(!strcmp(ed->save_input,"/nt/ab.txt"));
    editor_win_key(&w,'m');editor_win_key(&w,KEY_END);editor_win_key(&w,'x');editor_win_key(&w,'\b');CHECK(!strcmp(ed->save_input,"/mnt/ab.txt"));
    editor_win_key(&w,1);editor_win_key(&w,KEY_DEL);CHECK(!ed->save_input[0]);editor_win_key(&w,27);
    CHECK(!ed->save_stage && !strcmp(ed->lines[0],"previous") && ed->undo_n==1);
    editor_win_close(&w);kfree(ed);CHECK(!live_allocs && !guard_error && !handles && !bad_fd);
    close(output_fd);printf("PASS %d editor destination/save-as checks\n",checks);return 0;
}
'''

def extract(source, signature):
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref')
    parser.add_argument('--compare-reload-ref')
    parser.add_argument('--compare-saveas-ref')
    args = parser.parse_args()
    vfs = (ROOT/'kernel/fs/vfs.c').read_text()
    paths = function_source(vfs, 'join_mount_relative')+'\n'+function_source(vfs, 'vfs_realpath')
    core = (ROOT/'kernel/core/kernel.c').read_text()
    clip = core[core.index('#define CLIP_MAX'):core.index('// KAT: set/get round-trip')]
    base = HARNESS.replace('@SOURCE@', str(ROOT/'kernel/gui/apps/editor_win.c')).replace('@PATH_FUNCTIONS@',paths).replace('@CLIPBOARD@',clip)
    base = base.replace('int main(int argc, char** argv)', 'int io_fixture_main(int argc, char** argv)')
    compositor = (ROOT/'kernel/gui/core/compositor.c').read_text()
    destroy = extract(compositor, 'void window_destroy(int id) {')
    queue = extract(compositor, 'void window_request_close(int id) {')+'\n'+extract(compositor,'static int window_process_close_requests(void) {')
    with tempfile.TemporaryDirectory(prefix='nyx-editor-close-') as temp:
        work = Path(temp)
        def run(label, destroy_body, queue_body, editor_source=None, reload=False, saveas=False):
            source = base if editor_source is None else base.replace(str(ROOT/'kernel/gui/apps/editor_win.c'),str(editor_source))
            if reload:
                source = source.replace('return (int)write(output_fd, data, len);',
                    'if (!write_mode && !strcmp(path, "/mnt/original.txt")) { memcpy_asm(input_data,data,len); input_size=image_size=len; }\n    return (int)write(output_fd, data, len);')
            if saveas:
                source = source.replace('static int write_mode, ram_file;', 'static int write_mode, ram_file;\nstatic char last_saved[4096];')
                source = source.replace('return (int)write(output_fd, data, len);',
                    'if (len>=sizeof(last_saved)) { return -1; } memcpy_asm(last_saved,data,len); last_saved[len]=0; return (int)len;')
            src = work/(label+'.c'); src.write_text(source+REGISTRY+destroy_body+queue_body+(SAVEAS_CASES if saveas else RELOAD_CASES if reload else CASES))
            binary = work/label
            subprocess.run(['gcc','-std=gnu99','-O0','-ffreestanding','-Wall','-Wextra','-Werror',
                '-ffunction-sections','-fdata-sections','-no-pie','-Wl,--gc-sections','-fsanitize=address,undefined',
                '-DLOSSLESS_FIELDS','-DEDITOR_CLOSE_REGISTRY','-I',str(ROOT/'kernel/gui/apps'),str(src),'-o',str(binary)],check=True)
            return subprocess.run([str(binary)],capture_output=True,text=True)
        result=run('current',destroy,queue); print(result.stdout+result.stderr,end='')
        if result.returncode: raise SystemExit(result.returncode)
        result=run('reload',destroy,queue,reload=True); print(result.stdout+result.stderr,end='')
        if result.returncode: raise SystemExit(result.returncode)
        result=run('saveas',destroy,queue,saveas=True); print(result.stdout+result.stderr,end='')
        if result.returncode: raise SystemExit(result.returncode)
        if args.compare_saveas_ref:
            old_source=work/'old-saveas-editor.c'
            old_source.write_text(subprocess.check_output(['git','show',args.compare_saveas_ref+':kernel/gui/apps/editor_win.c'],cwd=ROOT,text=True))
            # Geometry helper is new; baseline only needs the initial F2 assertion.
            old_source.write_text(old_source.read_text()+'\nstatic void editor_save_button(int x,int y,int w,int i,int* bx,int* by,int* bw) { (void)w;*bx=x+i*56;*by=y+60;*bw=48; }\n')
            result=run('saveas-baseline',destroy,queue,editor_source=old_source,saveas=True)
            assert result.returncode and 'ed->save_stage==1 && writes==old_writes' in result.stdout,result.stdout+result.stderr
            print('PASS baseline lacks explicit destination choice')
        if args.compare_reload_ref:
            old_source=work/'old-editor.c'
            old_source.write_text(subprocess.check_output(['git','show',args.compare_reload_ref+':kernel/gui/apps/editor_win.c'],cwd=ROOT,text=True))
            result=run('reload-baseline',destroy,queue,editor_source=old_source,reload=True)
            assert result.returncode and 'ed->close_pending && ed->reload_pending && ed->modified' in result.stdout,result.stdout+result.stderr
            print('PASS baseline reload loses dirty buffer without confirmation')
        if args.compare_ref:
            old=subprocess.check_output(['git','show',args.compare_ref+':kernel/gui/core/compositor.c'],cwd=ROOT,text=True)
            assert 'window_destroy(hit->id);' in old and 'window_destroy(awin->id);' in old
            result=run('immediate-baseline',extract(old,'void window_destroy(int id) {'),
                'void window_request_close(int id) { window_destroy(id); }\nstatic int window_process_close_requests(void) { return 0; }\n')
            assert result.returncode and 'old immediate close fails here' not in result.stdout
            assert 'windows[0] && window_count == 1' in result.stdout,result.stdout+result.stderr
            print('PASS baseline immediate user-close route fails deferred-lifetime assertion')


if __name__ == '__main__':
    main()
