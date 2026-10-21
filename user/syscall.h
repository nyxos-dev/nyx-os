#ifndef _NYXOS_SYSCALL_H
#define _NYXOS_SYSCALL_H

#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_PRINT   2
#define SYS_OPEN    3
#define SYS_READ    4
#define SYS_CLOSE   5
#define SYS_GETPID  6
#define SYS_SBRK    7
#define SYS_FSIZE   8
#define SYS_EXEC    9
#define SYS_FORK    10
#define SYS_WAITPID 11
#define SYS_PIPE    12
#define SYS_EXECVE  13
#define SYS_DUP2    14
#define SYS_GETDENTS 15
#define SYS_KILL     16
#define SYS_SIGNAL   17
#define SYS_SIGRETURN 18
#define SYS_MMAP     19
#define SYS_MUNMAP   20
#define SYS_CHDIR    21
#define SYS_GETCWD   22
#define SYS_MKDIR    23
#define SYS_UNLINK   24
#define SYS_TTYMODE  25
#define SYS_MPROTECT 26
#define SYS_GETPROCS 27
#define SYS_READKEY  28
#define SYS_DLOPEN   29
#define SYS_DLSYM    30
#define SYS_TIME     31
#define SYS_SLEEP    32
#define SYS_SETFG    33
#define SYS_SOCKET   34
#define SYS_CONNECT  35
#define SYS_BIND     36
#define SYS_LISTEN   37
#define SYS_ACCEPT   38
#define SYS_SENDTO   39
#define SYS_RECVFROM 40
#define SYS_SIGPROCMASK 41
#define SYS_ALARM    42
#define SYS_POLL     43
#define SYS_STAT     44
#define SYS_FSTAT    45
#define SYS_LSEEK    46
#define SYS_GETPPID  47
#define SYS_DUP      48
#define SYS_RENAME   49
#define SYS_CLONE    50
#define SYS_FUTEX    51
#define SYS_GETTIMEOFDAY 52
#define SYS_NANOSLEEP 53
#define SYS_FBINFO    54
#define SYS_FBPRESENT 55
#define SYS_GETKEYEVENT 56
#define SYS_WIN_CREATE     57
#define SYS_WIN_DESTROY    58
#define SYS_WIN_PRESENT    59
#define SYS_WIN_POLL_EVENT 60
#define SYS_WIN_PRESENT_RECT 61
#define SYS_WIN_SET_TITLE  62
#define SYS_WIN_RESIZE     63
#define SYS_FONT_GLYPH     64

/* Threads (v5.8.87). CLONE_VM makes the new task SHARE this address space — a real
 * thread — instead of getting fork()'s copy-on-write duplicate. */
#define CLONE_VM     0x00000100UL
#define FUTEX_WAIT   0
#define FUTEX_WAKE   1

#define TTY_CANON   0   /* kernel line discipline: echoed, backspace-edited lines */
#define TTY_RAW     1   /* byte-at-a-time, no echo, arrows as ESC [ A/B/C/D */

/* mmap prot/flags (anonymous mappings — see kernel/mmap.c). */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED  ((void*)-1)

#define WNOHANG     1   /* waitpid option: don't block if no child is ready */
#define WUNTRACED   2   /* waitpid option: also report a STOPPED child (Ctrl-Z) */
#define WSTOPPED    0x10000  /* status sentinel bit: the child stopped (not exited) */
/* Decode a waitpid status: WIFSTOPPED true if the child stopped, WSTOPSIG its signal. */
#define WIFSTOPPED(s) (((s) & WSTOPPED) != 0)
#define WSTOPSIG(s)   ((s) & 0xFF)

/* open() flags: O_CREAT makes a file, O_TRUNC empties it (shell `>`), O_APPEND
 * seeks to EOF before writing (shell `>>`). */
#define O_RDONLY    0
#define O_CREAT     1
#define O_TRUNC     2
#define O_APPEND    4

/* Signals (subset of POSIX) — see kernel/signal.c. */
#define SIGHUP   1
#define SIGINT   2   /* Ctrl-C */
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9   /* uncatchable */
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19  /* uncatchable */
#define SIGTSTP  20  /* terminal stop (Ctrl-Z) */

typedef void (*sighandler_t)(int);   /* signal handler: void handler(int signo) */

#define SIG_DFL  ((sighandler_t)0)   /* default action (terminate / ignore) */
#define SIG_IGN  ((sighandler_t)1)   /* ignore the signal */

/* One directory entry as returned by getdents(). Layout MUST match the kernel's
 * record in SYS_GETDENTS (syscall.c): 64-byte name + u32 type = 68 bytes, no
 * padding. type is the VFS node type (1 = directory, else regular file). */
typedef struct {
    char name[64];
    unsigned int type;
} nyx_dirent_t;

/* One process as returned by getprocs(). Layout MUST match the kernel's record
 * in SYS_GETPROCS (syscall.c): four u32 fields + a 32-byte comm = 48 bytes, no
 * padding. `state` is the PROC_* enum (0 parked, 1 run, 2 zombie, 3 blocked);
 * `cpu_time` is ticks of accumulated CPU time. */
typedef struct {
    unsigned int pid;
    unsigned int ppid;
    unsigned int state;
    unsigned int cpu_time;
    char comm[32];
} nyx_procinfo_t;

/* Broken-down local time from time() (SYS_TIME). Layout MUST match the kernel's
 * 6-int buffer in syscall.c: {sec, min, hour, mday, mon, year} — year is the full
 * 4-digit year, mon is 1..12, mday is 1..31. The record behind `date`. */
typedef struct {
    int sec;
    int min;
    int hour;
    int mday;
    int mon;
    int year;
} nyx_tm;

/* x86_64 syscall ABI:
 *   RAX = syscall number
 *   RDI = a1, RSI = a2, RDX = a3, R10 = a4, R8 = a5, R9 = a6
 *   Clobbers: RCX (return RIP), R11 (saved RFLAGS)
 *   Return: RAX
 */
#ifdef __TINYC__
/* TinyCC has no `register long r10 asm("r10")` binding and can't satisfy the "r"
 * constraints GCC uses below, so the tcc path (used only when this libc is compiled
 * IN-OS by tcc for self-hosting) puts a4/a5/a6 in any register and moves them into the
 * ABI-mandated r10/r8/r9 explicitly inside the asm. Same effect; GCC path unchanged. */
static inline long syscall6(long no, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    __asm__ volatile (
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "movq %7, %%r9\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(no), "D"(a1), "S"(a2), "d"(a3), "m"(a4), "m"(a5), "m"(a6)
        : "rcx", "r11", "r10", "r8", "r9", "memory"
    );
    return ret;
}
#else
static inline long syscall6(long no, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(no), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
#endif

static inline long syscall5(long no, long a1, long a2, long a3, long a4, long a5) {
    return syscall6(no, a1, a2, a3, a4, a5, 0);
}

static inline long syscall4(long no, long a1, long a2, long a3, long a4) {
    return syscall6(no, a1, a2, a3, a4, 0, 0);
}

static inline long syscall3(long no, long a1, long a2, long a3) {
    return syscall6(no, a1, a2, a3, 0, 0, 0);
}

static inline long syscall2(long no, long a1, long a2) {
    return syscall6(no, a1, a2, 0, 0, 0, 0);
}

static inline long syscall1(long no, long a1) {
    return syscall6(no, a1, 0, 0, 0, 0, 0);
}

static inline void exit(int status) {
    syscall1(SYS_EXIT, status);
    for (;;);
}

static inline long write(int fd, const void* buf, long len) {
    return syscall3(SYS_WRITE, fd, (long)buf, len);
}

static inline void print(const char* s) {
    syscall1(SYS_PRINT, (long)s);
}

/* Framebuffer for fullscreen apps (v5.9.29 — the DOOM graphics enabler). fbinfo()
 * fills out[0..2] = screen {width, height, bpp}. fbpresent() blits a w*h 32bpp
 * (0x00RRGGBB, i.e. BGRX in memory) buffer to the screen, nearest-neighbour scaled
 * to fullscreen; call it once per frame. While a program keeps calling fbpresent()
 * it owns the whole screen (the desktop compositor yields); the desktop returns
 * shortly after it stops (exits). */
static inline int fbinfo(unsigned int out3[3]) {
    return (int)syscall1(SYS_FBINFO, (long)out3);
}
static inline int fbpresent(const void* buf, unsigned int w, unsigned int h) {
    return (int)syscall3(SYS_FBPRESENT, (long)buf, (long)w, (long)h);
}

/* getkeyevent(pressed, code): non-blocking raw key event for a fullscreen app
 * (DOOM's DG_GetKey). Returns 1 and fills *pressed (1=down, 0=up) and *code (a
 * layout-independent Set-1 scancode; bit 7 flags an E0-extended arrow/nav key) if an
 * event was waiting, else 0 (ring empty). Poll it each frame; keys arrive as both a
 * press and a release. */
static inline int getkeyevent(int* pressed, int* code) {
    long r = syscall1(SYS_GETKEYEVENT, 0);
    if (r < 0) return 0;
    if (pressed) *pressed = (int)((r >> 8) & 1);
    if (code)    *code    = (int)(r & 0xFF);
    return 1;
}

/* --- User-space windows (v6.4.354, issue #77) ------------------------------
 * Open a REAL desktop window from a ring-3 program: unlike the fullscreen fb*
 * calls, the compositor keeps running and composites the window among the others.
 * win_create() returns a window id; win_present() blits a whole-client XRGB
 * (0x00RRGGBB) buffer; win_poll_event() pops one input event (non-blocking).
 * A window opened this way is drawn while the launcher pumps the desktop. */
#define UWE_KEY    1   /* a = keycode */
#define UWE_CLICK  2   /* a = x, b = y, c = button   (client-relative) */
#define UWE_MOVE   3   /* a = x, b = y, c = buttons  (client-relative) */
#define UWE_CLOSE  4   /* the window's close box was pressed */
#define UWE_RESIZE 5   /* a = w, b = h */

/* One event record — MUST match the kernel's four-i64 {kind,a,b,c} (syscall.c). */
typedef struct { long kind, a, b, c; } win_event_t;

/* win_create(w, h, title): open a window with a w x h client area. Returns a
 * window id (>= 0), or -1 (bad size / table full). */
static inline int win_create(unsigned int w, unsigned int h, const char* title) {
    int tlen = 0;
    if (title) while (title[tlen]) tlen++;
    return (int)syscall4(SYS_WIN_CREATE, (long)w, (long)h, (long)title, (long)tlen);
}
/* win_destroy(id): close the window. Idempotent (returns 0 even if already gone). */
static inline int win_destroy(int id) {
    return (int)syscall1(SYS_WIN_DESTROY, id);
}
/* win_present(id, buf, w, h): blit a w*h XRGB buffer as the whole client area.
 * w,h MUST equal the create-time size. Returns 0, or -1. Call once per frame. */
static inline int win_present(int id, const void* buf, unsigned int w, unsigned int h) {
    return (int)syscall4(SYS_WIN_PRESENT, id, (long)buf, (long)w, (long)h);
}
/* win_present_rect(id, buf, x, y, w, h): blit a w*h XRGB buffer into just the (x,y)
 * sub-rectangle of the client area — a damage-rect update, cheaper than re-sending the
 * whole window. The rect must lie inside the client area. Returns 0, or -1. */
static inline int win_present_rect(int id, const void* buf, unsigned int x, unsigned int y,
                                   unsigned int w, unsigned int h) {
    long xy = (long)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
    long wh = (long)(((h & 0xFFFF) << 16) | (w & 0xFFFF));
    return (int)syscall4(SYS_WIN_PRESENT_RECT, id, (long)buf, xy, wh);
}
/* win_poll_event(id, ev): pop one input event into *ev. Returns 1 (got one),
 * 0 (queue empty), or -1 (no such window — it was closed). Non-blocking. */
static inline int win_poll_event(int id, win_event_t* ev) {
    return (int)syscall2(SYS_WIN_POLL_EVENT, id, (long)ev);
}
/* win_set_title(id, title): replace the window's title-bar text after creation, so a
 * client can reflect state (the edited file, the current URL, the cwd). Truncated to
 * the kernel's title limit. Returns 0, or -1 for an unknown/headless window. */
static inline int win_set_title(int id, const char* title) {
    unsigned int n = 0;
    if (title) while (title[n]) n++;
    return (int)syscall3(SYS_WIN_SET_TITLE, id, (long)title, (long)n);
}
/* win_resize(id, w, h): app-initiated resize of the client area to w x h. The old backing
 * is dropped, so the caller must present a NEW w x h buffer afterwards. Returns 0, or -1
 * (unknown window / zero or too-large dimensions). */
static inline int win_resize(int id, unsigned int w, unsigned int h) {
    return (int)syscall3(SYS_WIN_RESIZE, id, (long)w, (long)h);
}
/* font_glyph(c, out16): copy the 16-byte 8x16 bitmap for byte c (row 0 = top, MSB =
 * leftmost) into out16, so a ring-3 window client can render text from the one kernel
 * font instead of embedding its own. Returns 0, or -1 (bad buffer). See uwin_text(). */
static inline int font_glyph(unsigned char c, unsigned char* out16) {
    return (int)syscall2(SYS_FONT_GLYPH, (long)c, (long)out16);
}

static inline long open(const char* path, int flags, int mode) {
    return syscall3(SYS_OPEN, (long)path, flags, mode);
}

static inline long read(int fd, void* buf, long count) {
    return syscall3(SYS_READ, fd, (long)buf, count);
}

static inline long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

static inline long getpid(void) {
    return syscall1(SYS_GETPID, 0);
}

static inline long sbrk(long increment) {
    return syscall1(SYS_SBRK, increment);
}

static inline long fsize(int fd) {
    return syscall1(SYS_FSIZE, fd);
}

static inline long exec(const char* path) {
    return syscall1(SYS_EXEC, (long)path);
}

/* Copy-on-write fork: returns the child's pid in the parent, 0 in the child, and
 * -1 on failure. Both processes then run preemptively, time-sliced by the kernel. */
static inline long fork(void) {
    return syscall1(SYS_FORK, 0);
}

/* Wait for a child to exit and collect its status. Returns the child's pid (with
 * its exit code written to *status) or -1 if there is no such child. The kernel
 * blocks the caller until a child exits (pid <= 0 waits for any child). */
static inline long waitpid(int pid, int* status) {
    return syscall2(SYS_WAITPID, pid, (long)status);
}

/* waitpid with options. waitpid3(pid, &st, WNOHANG) returns the pid if that child
 * has already exited (status in *status), 0 if it is still running, or -1 if there
 * is no such child — the non-blocking reap the shell uses for `&` background jobs. */
static inline long waitpid3(int pid, int* status, int options) {
    return syscall3(SYS_WAITPID, pid, (long)status, options);
}

/* Enumerate directory `path` into up to `max` nyx_dirent_t records at `buf`.
 * Returns the number of entries written, or -1 on error. The caller must ensure
 * every page of `buf` is resident before the call (the kernel copies into it and
 * cannot fault a lazy-sbrk heap page in) — memset the buffer first. */
static inline long getdents(const char* path, nyx_dirent_t* buf, int max) {
    return syscall3(SYS_GETDENTS, (long)path, (long)buf, max);
}

/* The sigreturn trampoline (crt0.asm): the handler's return address, which the
 * kernel pushes onto the user stack before entering a handler. */
extern void __sigreturn(void);

/* kill(pid, sig): post signal `sig` to process `pid`. sig 0 probes existence.
 * Returns 0, or -1 if there is no such process. */
static inline long kill(int pid, int sig) {
    return syscall2(SYS_KILL, pid, sig);
}

/* signal(sig, handler): set the disposition of `sig` to SIG_DFL, SIG_IGN, or a
 * handler function. Returns the previous disposition, or SIG_ERR ((sighandler_t)-1)
 * for an invalid/uncatchable signal. The kernel is told the __sigreturn trampoline
 * so the handler can return normally. */
#define SIG_ERR ((sighandler_t)-1)
static inline sighandler_t signal(int sig, sighandler_t handler) {
    return (sighandler_t)syscall3(SYS_SIGNAL, sig, (long)handler, (long)__sigreturn);
}

/* sigprocmask(how, set, oldset): read and/or change this process's blocked-signal
 * mask (a 32-bit set, bit 1<<signo). how = SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK;
 * oldset (may be NULL) receives the previous mask. The primitive behind
 * sigsetjmp/siglongjmp (libc.h) restoring the mask on a non-local jump. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2
static inline long sigprocmask(int how, unsigned long set, unsigned long* oldset) {
    return syscall3(SYS_SIGPROCMASK, how, (long)set, (long)oldset);
}

/* alarm(seconds): deliver SIGALRM to this process after `seconds` seconds (0
 * cancels a pending alarm). Returns the seconds remaining on any previous alarm,
 * or 0. Without a SIGALRM handler the default action terminates the process. */
static inline unsigned int alarm(unsigned int seconds) {
    return (unsigned int)syscall1(SYS_ALARM, seconds);
}

/* raise(sig): send `sig` to the calling process. */
static inline long raise(int sig) {
    return syscall2(SYS_KILL, (int)getpid(), sig);
}

/* mmap(addr, length, prot, flags, fd, offset): map anonymous demand-zero memory
 * (fd/offset ignored). Returns the base address, or MAP_FAILED. Pages fault in on
 * first touch. munmap(addr, length) releases a mapping. */
static inline void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long offset) {
    return (void*)syscall6(SYS_MMAP, (long)addr, (long)length, prot, flags, fd, offset);
}
static inline long munmap(void* addr, unsigned long length) {
    return syscall2(SYS_MUNMAP, (long)addr, (long)length);
}

/* mprotect(addr, len, prot): change the protection of a mapped range (e.g. make a
 * page read-only or writable). Returns 0, or -1. */
static inline long mprotect(void* addr, unsigned long len, int prot) {
    return syscall3(SYS_MPROTECT, (long)addr, len, prot);
}

/* chdir(path): set the working directory (relative paths in open()/getdents()
 * resolve against it). Returns 0, or -1 if path isn't a directory. getcwd(buf,size)
 * copies the current directory into buf and returns its length, or -1. */
static inline long chdir(const char* path) {
    return syscall1(SYS_CHDIR, (long)path);
}
static inline long getcwd(char* buf, long size) {
    return syscall2(SYS_GETCWD, (long)buf, size);
}

/* mkdir(path, mode): create a directory; unlink(path): remove a file. Both take
 * cwd-relative or absolute paths. Return 0, or -1. */
static inline long mkdir(const char* path, int mode) {
    return syscall2(SYS_MKDIR, (long)path, mode);
}
static inline long unlink(const char* path) {
    return syscall1(SYS_UNLINK, (long)path);
}

/* ttymode(TTY_RAW / TTY_CANON): switch this process's stdin discipline. Returns
 * the previous mode. Raw mode is what a line editor runs on; execve resets it. */
static inline long ttymode(int mode) {
    return syscall1(SYS_TTYMODE, mode);
}

/* Snapshot the process table into up to `max` nyx_procinfo_t records at `buf`.
 * Returns the number of live processes written, or -1. This is the enumeration
 * primitive behind `ps` — the process analogue of getdents(). As with getdents,
 * every page of `buf` must be resident before the call (use a .bss array or
 * memset first) so the kernel's copy never hits an unfaulted lazy-heap page. */
static inline long getprocs(nyx_procinfo_t* buf, int max) {
    return syscall2(SYS_GETPROCS, (long)buf, max);
}

/* readkey(timeout_ms): block up to timeout_ms for a single keypress. Returns the
 * key (an ASCII byte, or an extended keycode >= 0x80 for arrows), 0 if the timeout
 * elapsed with no key, or a negative value if interrupted. A timeout of 0 blocks
 * FOREVER until a key (for editors); a positive timeout suits TUI refresh loops
 * (e.g. `top`) — redraw when nothing was pressed. No echo, independent of ttymode. */
static inline long readkey(long timeout_ms) {
    return syscall1(SYS_READKEY, timeout_ms);
}

/* dlopen(path): load a shared object (a prelinked .so) and map it into this
 * process on demand. Returns a handle (>=1) to pass to dlsym, or -1 on failure.
 * dlsym(handle, name): resolve a symbol to its address; cast it to the right
 * function/data pointer and call/use it. This is NyxOS's runtime dynamic loading
 * — libraries are at fixed addresses, so there is no relocation. */
static inline long dlopen(const char* path) {
    return syscall1(SYS_DLOPEN, (long)path);
}
static inline void* dlsym(long handle, const char* name) {
    return (void*)syscall2(SYS_DLSYM, handle, (long)name);
}

/* Create a pipe: fds[0] is the read end, fds[1] the write end. Returns 0, or -1.
 * A read() on the read end blocks until a writer writes (or all writers close,
 * which yields EOF = 0). Survives fork(), so it connects a parent and child. */
static inline long pipe(int fds[2]) {
    return syscall1(SYS_PIPE, (long)fds);
}

/* Replace the current process image with the program at `path`. On success it does
 * not return (the new program runs with the same pid and open fds); returns -1 only
 * on failure. Both argv and envp (NULL-terminated string arrays) are passed to the
 * new program: crt0 reads argc/argv from the stack and sets `environ` from envp, so
 * the child inherits its parent's environment. Pass 0 for either to omit it. */
static inline long execve(const char* path, char* const argv[], char* const envp[]) {
    return syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

/* time(t): fill *t with the current broken-down local time from the RTC. Returns 0,
 * or -1 on error. The primitive behind `date`. */
static inline long time(nyx_tm* t) {
    return syscall1(SYS_TIME, (long)t);
}

/* sleep_ms(ms): block this process for `ms` milliseconds (the scheduler runs other
 * processes meanwhile). Returns 0, or a negative value if a signal interrupted it.
 * sleep_sec(s) is the whole-seconds convenience form used by `sleep`. */
static inline long sleep_ms(long ms) {
    return syscall1(SYS_SLEEP, ms);
}
static inline long sleep_sec(long s) {
    return syscall1(SYS_SLEEP, s * 1000);
}

/* Wall-clock time (gettimeofday) and sub-second sleep (nanosleep). The field order
 * MUST match the kernel's 2-long fill in SYS_GETTIMEOFDAY / SYS_NANOSLEEP. `long` is
 * 64-bit here, so tv_sec is y2038-safe. */
struct timeval  { long tv_sec; long tv_usec; };
struct timespec { long tv_sec; long tv_nsec; };

/* gettimeofday(tv, tz): wall-clock seconds + microseconds since the Unix epoch
 * (seconds from the RTC as UTC; microseconds from the 1000 Hz tick). tz is obsolete
 * and ignored. Returns 0, or -1 on a bad pointer. */
static inline int gettimeofday(struct timeval* tv, void* tz) {
    return (int)syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)tz);
}

/* nanosleep(req, rem): sleep req->tv_sec + req->tv_nsec, rounded UP to the 1 ms
 * timer tick (its finest resolution). rem (may be NULL) is zeroed — no precise
 * remainder is tracked. Returns 0, or a negative value (-EINTR) if a signal cut it
 * short. usleep(usec) is the microsecond convenience form. */
static inline int nanosleep(const struct timespec* req, struct timespec* rem) {
    return (int)syscall2(SYS_NANOSLEEP, (long)req, (long)rem);
}
static inline int usleep(long usec) {
    struct timespec req = { usec / 1000000L, (usec % 1000000L) * 1000L };
    return nanosleep(&req, 0);
}

/* setfg(pid): make `pid` the terminal foreground process, so keyboard signals (Ctrl-C
 * -> SIGINT, Ctrl-Z -> SIGTSTP) target it instead of the shell. The shell points this
 * at a job while it runs in the foreground, then back at itself (pid 0 clears it).
 * The mechanism behind job control. Returns 0, or -1 for an unknown pid or a
 * value outside the unsigned 32-bit PID range, without changing the target. */
static inline long setfg(long pid) {
    return syscall1(SYS_SETFG, pid);
}

/* Duplicate oldfd onto newfd (closing newfd first if open) — the redirection
 * primitive: dup2(pipefd[1], 1) makes a process's stdout flow into the pipe.
 * Pipe fds only for now (their ends are reference-counted). Returns newfd or -1. */
static inline long dup2(int oldfd, int newfd) {
    return syscall2(SYS_DUP2, oldfd, newfd);
}

/* dup(oldfd): lowest-available fd referring to the same stream as oldfd (both stay
 * open). Returns the new fd, or -1. */
static inline int dup(int oldfd) {
    return (int)syscall1(SYS_DUP, oldfd);
}

/* rename(oldpath, newpath): move/rename a VFS entry (paths are cwd-relative).
 * Returns 0, or -1 if the source is missing or the move didn't land. */
static inline int rename(const char* oldpath, const char* newpath) {
    return (int)syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
}

/* --- Threads (clone + futex) ----------------------------------------------- */
/* clone(fn, stack_top, arg, CLONE_VM): start a THREAD sharing this address space.
 * It runs fn(arg) on `stack_top` (the HIGH end of a block you own — the stack grows
 * down from there; 16-byte alignment keeps the SysV ABI happy). Returns the new tid,
 * or -1. The thread must finish with exit() — it never "returns" anywhere.
 * Without CLONE_VM this fails: use fork() for a separate address space. */
static inline int clone(void* fn, void* stack_top, void* arg, unsigned long flags) {
    return (int)syscall4(SYS_CLONE, (long)fn, (long)stack_top, (long)arg, (long)flags);
}

/* futex_wait(uaddr, val): sleep while *uaddr == val. Returns immediately (0) if the
 * value already differs — that compare-and-sleep is what makes a lock race-free. */
static inline int futex_wait(volatile int* uaddr, int val) {
    return (int)syscall3(SYS_FUTEX, (long)uaddr, FUTEX_WAIT, (long)val);
}
/* futex_wake(uaddr, n): wake up to n waiters on uaddr; returns how many were woken. */
static inline int futex_wake(volatile int* uaddr, int n) {
    return (int)syscall3(SYS_FUTEX, (long)uaddr, FUTEX_WAKE, (long)n);
}

/* --- TCP sockets (SOCK_STREAM only for now) -------------------------------- */
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

/* Build a network-order IPv4 address from dotted octets a.b.c.d — matches the
 * kernel convention (first octet in the low byte). Pass the result to connect(). */
static inline unsigned int inet_ipv4(int a, int b, int c, int d) {
    return (unsigned int)(a & 0xFF)        | ((unsigned int)(b & 0xFF) << 8) |
           ((unsigned int)(c & 0xFF) << 16) | ((unsigned int)(d & 0xFF) << 24);
}

/* socket(AF_INET, SOCK_STREAM, 0): create a TCP socket. Returns a file descriptor
 * usable with read()/write()/close() (send()/recv() are aliases), or -1. */
static inline long socket(int domain, int type, int protocol) {
    return syscall3(SYS_SOCKET, domain, type, protocol);
}

/* connect(fd, ip, port): open the TCP connection, blocking until the 3-way
 * handshake completes. ip is network order (from inet_ipv4), port is host order.
 * Returns 0 on success, -1 on failure. */
static inline long connect(int fd, unsigned int ip, int port) {
    return syscall3(SYS_CONNECT, fd, (long)ip, port);
}

/* send()/recv(): BSD-style aliases for write()/read() on a connected socket fd.
 * The flags argument is accepted for familiarity and ignored. */
static inline long send(int fd, const void* buf, long len, int flags) {
    (void)flags; return write(fd, buf, len);
}
static inline long recv(int fd, void* buf, long len, int flags) {
    (void)flags; return read(fd, buf, len);
}

/* Server side. bind(fd, INADDR_ANY, port) records the local port; listen() opens
 * it passively; accept() blocks until a client connects and returns a NEW fd for
 * that connection (the listener fd stays open for further clients). */
#define INADDR_ANY 0U
static inline long bind(int fd, unsigned int ip, int port) {
    return syscall3(SYS_BIND, fd, (long)ip, port);
}
static inline long listen(int fd, int backlog) {
    return syscall2(SYS_LISTEN, fd, backlog);
}
static inline long accept(int fd) {
    return syscall1(SYS_ACCEPT, fd);
}

/* --- UDP datagrams (SOCK_DGRAM) -------------------------------------------- */
/* sendto(fd, buf, len, flags, ip, port): send a UDP datagram to ip:port (ip from
 * inet_ipv4(), port host order; flags ignored). Returns bytes sent, or -1. The
 * socket auto-binds an ephemeral source port on the first send if not bind()'d. */
static inline long sendto(int fd, const void* buf, long len, int flags,
                          unsigned int ip, int port) {
    (void)flags;
    return syscall6(SYS_SENDTO, fd, (long)buf, len, 0, (long)ip, port);
}
/* recvfrom(fd, buf, len, flags, *ip, *port): block for a UDP datagram; on success
 * *ip and *port (each may be NULL) receive the sender's network-order IP and
 * host-order port. Returns the datagram length (truncated to len), or -1. */
static inline long recvfrom(int fd, void* buf, long len, int flags,
                            unsigned int* ip, int* port) {
    (void)flags;
    return syscall6(SYS_RECVFROM, fd, (long)buf, len, 0, (long)ip, (long)port);
}

/* --- I/O multiplexing: poll(2) ---------------------------------------------- */
/* Wait for events on a set of fds in one call — the primitive for full-duplex /
 * multi-fd programs. events/revents are masks of POLLIN (readable) / POLLOUT
 * (writable); the kernel also sets POLLNVAL for a bad fd. timeout is in ms
 * (0 = return immediately, <0 = block forever). Returns the number of fds with a
 * non-zero revents, 0 on timeout, or -1. Works over sockets, pipes and stdin. */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
struct pollfd { int fd; short events; short revents; };
static inline long poll(struct pollfd* fds, int nfds, int timeout) {
    return syscall3(SYS_POLL, (long)fds, nfds, timeout);
}

/* --- File metadata + seeking: stat/fstat/lseek, getppid (v5.8.80) ----------- */
/* NyxOS keeps a minimal struct stat: the fields the kernel actually tracks. The
 * layout MUST match the kernel's 3-u32 fill order in SYS_STAT/SYS_FSTAT. */
struct stat {
    unsigned int st_size;   /* file size in bytes (0 for directories) */
    unsigned int st_mode;   /* S_IFDIR|perms or S_IFREG|perms */
    unsigned int st_ino;    /* reserved (0 for now) */
};
#define S_IFMT   0xF000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* stat(path, &st) / fstat(fd, &st): fill st with size + mode. Return 0, or -1 if
 * the path/fd is invalid. */
static inline int stat(const char* path, struct stat* st) {
    return (int)syscall2(SYS_STAT, (long)path, (long)st);
}
static inline int fstat(int fd, struct stat* st) {
    return (int)syscall2(SYS_FSTAT, fd, (long)st);
}
/* lseek(fd, offset, whence): reposition the fd's read/write offset; SEEK_SET/CUR/
 * END. Positions fit uint32_t; out-of-range results return -1 without moving.
 * Returns the new absolute offset, or -1 (e.g. a pipe/socket isn't seekable).
 * Seeking beyond EOF does not grow a file or bypass VFS write-size limits. */
static inline long lseek(int fd, long offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
}
/* getppid(): the parent process id. */
static inline int getppid(void) {
    return (int)syscall1(SYS_GETPPID, 0);
}

#endif
