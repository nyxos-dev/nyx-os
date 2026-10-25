// ============================================================
// kernel.h - Cabecera principal del kernel NyxOS x86_64
// ============================================================
#ifndef KERNEL_H
#define KERNEL_H

// ============================================================
// Base types (fixed-width typedefs, bool, stdarg, NULL) live in their own
// zero-dependency header now, so a per-subsystem public header can pull in just
// the primitives instead of all of kernel.h — first step of the modular-header
// split (see the architecture-modularity note). Included first; every existing
// includer of kernel.h still sees these exactly as before.
// ============================================================
#include "types.h"
#include "../mm/paging.h"   // paging subsystem public interface (PTE flags + VM API), split out of this header

// ============================================================
// Constantes
// ============================================================
#define KERNEL_NAME    "NyxOS"
#define KERNEL_VERSION "6.5.267"
#define KERNEL_CODENAME "GUI Suite"
#define KERNEL_DATE    "2026"

// Component names, in the Nyx (Greek night) family alongside the Selene browser.
// Hemera (daughter of Nyx, goddess of daylight) is the compositor / window manager;
// Erebus (primordial darkness) is the terminal emulator. Used wherever these
// components identify themselves (nyxfetch, the terminal banner).
#define COMPOSITOR_NAME    "Hemera"
#define COMPOSITOR_VERSION "3.34"      // Hemera: the windowing compositor (2nd-gen daedalOS-style desktop)
#define TERMINAL_NAME      "Erebus"
#define TERMINAL_VERSION   "1.4"      // Erebus: the terminal emulator
#define SELENE_NAME        "Selene"
#define SELENE_VERSION     "1.3"      // Selene: the web browser
#define EDITOR_NAME        "Mnemosyne"
#define EDITOR_VERSION     "1.3"      // Mnemosyne: the text editor (goddess of memory)
// LOOP POLICY (user 2026-08-21): a loop increment that CHANGES a component's code bumps
// THAT component's version too — compositor.c -> COMPOSITOR_VERSION, terminal_win.c ->
// TERMINAL_VERSION, selene_win.c -> SELENE_VERSION, editor_win.c -> EDITOR_VERSION.
// Rhythm: +0.1 (minor) per meaningful change.

#define ARCH_X86 1
#define BITS_64  1

#define PAGE_SIZE        4096
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
/* Max whitespace-separated tokens the in-kernel command parser accepts (both
 * execute_command and the interactive shell path in kernel.c). Matches the
 * exec/spawn path's own 32-arg cap (process.c uargv[33], syscall.c kargv[32]),
 * so a full-length command line is passed through without silent truncation. */
#define MAX_CMD_ARGS     32
#define KERNEL_BASE      0xFFFFFF8000000000
#define KERNEL_HEAP_START 0xFFFFFFFF90000000
#define __pa(vaddr)      ((uint64_t)(vaddr) - KERNEL_BASE)
#define __va(paddr)      ((void*)((uint64_t)(paddr) + KERNEL_BASE))
#define KERNEL_HEAP_SIZE  (16 * 1024 * 1024)

// Segment selectors (same as 32-bit)
#define KERNEL_CS  0x08
#define KERNEL_DS  0x10
#define USER_CS   0x1B
#define USER_DS   0x23
#define TSS_SEL   0x28

#define MAX_PROCESSES    512
#define MAX_THREADS      1024
#define MAX_FILES        256
#define PROC_MAX_FDS     32     // per-process open file descriptors (UFD_BASE..)
#define MAX_MOUNTS       16
#define SYS_TABLE_SIZE   256

// Syscall numbers
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
#define SYS_STAT     44   /* stat(path, struct stat*) — size + mode(dir/reg) */
#define SYS_FSTAT    45   /* fstat(fd, struct stat*) */
#define SYS_LSEEK    46   /* lseek(fd, offset, whence) — SEEK_SET/CUR/END */
#define SYS_GETPPID  47   /* getppid() */
#define SYS_DUP      48   /* dup(oldfd) — lowest-available fd aliasing oldfd */
#define SYS_RENAME   49   /* rename(oldpath, newpath) — VFS move/rename */
#define SYS_CLONE    50   /* clone(fn, stack, arg, flags) — CLONE_VM thread */
#define SYS_FUTEX    51   /* futex(uaddr, op, val) — FUTEX_WAIT / FUTEX_WAKE */
#define SYS_GETTIMEOFDAY 52 /* gettimeofday(struct timeval*, tz) — epoch sec+usec */
#define SYS_NANOSLEEP 53  /* nanosleep(const timespec* req, timespec* rem) — sub-second sleep */
#define SYS_FBINFO    54  /* fbinfo(uint32_t out[3]) — screen width/height/bpp for a fullscreen app */
#define SYS_FBPRESENT 55  /* fbpresent(buf, w, h) — blit a 32bpp buffer to the screen, scaled */
#define SYS_GETKEYEVENT 56 /* getkeyevent() — next raw key event (pressed<<8)|scancode, or -1 */
#define SYS_WIN_CREATE    57 /* win_create(w, h, title_ptr, title_len) -> id / -1 */
#define SYS_WIN_DESTROY   58 /* win_destroy(id) -> 0 (idempotent) */
#define SYS_WIN_PRESENT   59 /* win_present(id, buf, w, h) -> 0/-1 (whole XRGB client-area blit) */
#define SYS_WIN_POLL_EVENT 60 /* win_poll_event(id, ev[4 i64]) -> 1 got / 0 empty / -1 bad id */
#define SYS_WIN_PRESENT_RECT 61 /* win_present_rect(id, buf, (y<<16)|x, (h<<16)|w) -> blit a w*h XRGB sub-rect at (x,y) of the client area */
#define SYS_WIN_SET_TITLE 62 /* win_set_title(id, title_ptr, title_len) -> 0/-1 (update the title-bar text after creation) */
#define SYS_WIN_RESIZE 63 /* win_resize(id, w, h) -> 0/-1 (app-initiated resize of the client area; drops the backing so the next present is at the new size) */
#define SYS_FONT_GLYPH 64 /* font_glyph(c, out16) -> 0/-1 (copy the 16-byte 8x16 bitmap for byte c to a user buffer, so ring-3 clients render text from the one kernel font) */

const uint8_t* font_glyph_bitmap(unsigned char c);   // drivers/video/font.c: the 16-byte glyph for c

/* ------------------------------------------------------------------ */
/*  Threads (v5.8.89) — clone(CLONE_VM) + futex                        */
/* ------------------------------------------------------------------ */
/* CLONE_VM: the new task SHARES the caller's address space (same PML4) instead of
 * getting a COW copy — i.e. a real thread. It runs `fn(arg)` on the caller-supplied
 * `stack` (top of a userspace-allocated block) with its own kernel stack. Without
 * CLONE_VM, clone() is refused (use fork() for a separate address space). */
#define CLONE_VM     0x00000100UL

/* futex ops. WAIT blocks the caller while *uaddr == val (returns immediately if it
 * differs — the classic race-free compare-and-block); WAKE wakes up to `val` waiters
 * on that address. The wait queue is keyed by the PHYSICAL address of the word, so it
 * works across every task sharing the page. */
#define FUTEX_WAIT   0
#define FUTEX_WAKE   1

/* SYS_TTYMODE modes. Canonical: read(0) returns a full line, echoed + backspace-
 * edited by the kernel. Raw: read(0) returns bytes as they arrive, NO echo, and
 * extended keys are delivered as ANSI escapes (ESC [ A/B/C/D = up/down/right/left,
 * H/F = home/end) — the shell's line editor runs on this. */
#define TTY_CANON   0
#define TTY_RAW     1

/* waitpid() options (a3). WNOHANG makes SYS_WAITPID return 0 immediately instead
 * of blocking when a matching child exists but has not exited yet — the shell
 * uses it to reap finished `&` background jobs without stalling at the prompt.
 * WUNTRACED additionally reports a child that has STOPPED (Ctrl-Z / SIGTSTP) — the
 * status is the sentinel WSTOPPED|sig, so the shell can build job control. */
#define WNOHANG     1
#define WUNTRACED   2
/* waitpid status sentinel: a stopped (not exited) child. Bit 16 can't appear in a
 * normal exit status (0-255) or a killed status (128+signo), so it's unambiguous;
 * the low byte carries the stop signal. */
#define WSTOPPED    0x10000

/* ------------------------------------------------------------------ */
/*  Signals (v5.8.11) — a subset of POSIX                             */
/* ------------------------------------------------------------------ */
/* Signal numbers fit in a u32 bitmask (bit i = signal i), so NSIG <= 32. */
#define NSIG        32
#define SIGHUP      1
#define SIGINT      2    /* Ctrl-C */
#define SIGQUIT     3
#define SIGILL      4
#define SIGABRT     6
#define SIGFPE      8
#define SIGKILL     9    /* uncatchable — always the default (terminate) action */
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19   /* uncatchable */
#define SIGTSTP     20   /* terminal stop (Ctrl-Z) — catchable, default = stop */

/* Handler sentinels (a real handler is a ring-3 code address, always > 1). */
#define SIG_DFL     0UL  /* default action (terminate, or ignore for SIGCHLD) */
#define SIG_IGN     1UL  /* ignore the signal */

/* read()/etc. return this (negated) when a signal interrupts a blocking wait. */
#define EINTR       4

/* open() flags. O_CREAT is the long-standing `flags & 1`; O_TRUNC empties an
 * existing file (shell `>`), O_APPEND starts writes at EOF (shell `>>`). */
#define O_RDONLY    0
#define O_CREAT     1
#define O_TRUNC     2
#define O_APPEND    4

/* ------------------------------------------------------------------ */
/*  mmap (v5.8.12) — anonymous, demand-zero memory mappings           */
/* ------------------------------------------------------------------ */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
/* Mappings live in [MMAP_BASE, MMAP_MAX): above the 4 GiB heap cap and well below
 * the 128 TiB user stack (0x7FFFFFFFE000), so they never collide with either. */
#define MMAP_BASE  0x100000000ULL
#define MMAP_MAX   0x0000700000000000ULL
#define PROC_MAX_VMAS 16
/* ASLR: each process's mmap area starts at MMAP_BASE + a random page-aligned slide in
 * [0, MMAP_ASLR_PAGES*4KiB) = [0, 4 GiB) — 20 bits of entropy, tiny against the 112 TiB
 * mmap window, so mmap'd regions (shared libc, big allocations) land unpredictably. */
#define MMAP_ASLR_PAGES (1u << 20)
uint64_t mmap_aslr_slide(uint32_t rnd);   /* pure page-aligned bounded slide; see mm/mmap.c */
int      mmap_aslr_selftest(void);

/* Shared libc (v5.8.28). One copy of libc's code is loaded at boot and mapped
 * read-only into every user process at this fixed VA, so programs link against
 * it (ld --just-symbols) instead of each bundling ~14 KB of libc. It sits within
 * ±2 GiB of the program at 0x10000 (so direct rel32 calls reach it), and the heap
 * cap (SYS_SBRK) is kept just below it. libc's tiny writable .bss (the malloc
 * freelist head) is given a private per-process page. */
#define SHARED_LIBC_BASE 0x30000000ULL
void shared_libc_load(void);            // parse + load /libc.so into shared frames (boot)
void shared_libc_map(uint64_t* pml4);   // map it into a process's address space
int  shared_libc_is_ready(void);

// A mapped virtual-address region (contiguous present user pages, same perms).
typedef struct { uint64_t start, end; int writable, exec; } vm_region_t;
// Walk a process address space, coalescing runs of present user pages with the
// same permissions into regions (for /proc/<pid>/maps). Returns the count.
int vm_collect_regions(uint64_t* pml4, vm_region_t* out, int max);
long     do_dlopen(const char* path);              // SYS_DLOPEN: map a .so into this process
uint64_t do_dlsym(long handle, const char* name);  // SYS_DLSYM: resolve a symbol to its VA

typedef struct {
    uint64_t start;      // page-aligned base VA (inclusive)
    uint64_t end;        // page-aligned end VA (exclusive)
    uint32_t prot;       // PROT_* bits (honored per-page by vm_handle_fault; mprotect updates it)
    uint8_t  used;       // 1 = slot in use
    // File-backed mapping: a private kernel snapshot of the file (from offset 0).
    // A demand-faulted page copies its slice from here; NULL = anonymous (zero-fill).
    // Owned by the VMA: freed on munmap/execve, deep-copied on fork.
    uint8_t* file_buf;
    uint32_t file_size;
} vma_t;

// Pipe fds are stored in the per-process fd table (ufd_handle) with this flag set,
// so SYS_READ/WRITE/CLOSE route to the pipe layer instead of the VFS. The low bits
// carry (pipe_id << 1) | end (end: 0 = read, 1 = write). A real VFS handle is a
// small index (< MAX_FILES) so it never collides with the flag.
#define UFD_PIPE_FLAG        0x40000000
#define UFD_PIPE_MAKE(id, w) (UFD_PIPE_FLAG | ((id) << 1) | ((w) ? 1 : 0))
#define UFD_PIPE_ID(h)       (((h) & 0x3FFFFFFF) >> 1)
#define UFD_PIPE_IS_WRITE(h) ((h) & 1)

// Socket fds share the same fd table with this (distinct) flag set, so
// SYS_READ/WRITE/CLOSE route to the TCP socket layer (net.c) instead of the VFS.
// The low bits carry the net-socket id (< MAX_SOCKETS). 0x20000000 doesn't
// collide with the pipe flag (0x40000000) or a small VFS handle.
#define UFD_SOCK_FLAG        0x20000000
#define UFD_SOCK_MAKE(id)    (UFD_SOCK_FLAG | ((id) & 0xFFFF))
#define UFD_SOCK_ID(h)       ((h) & 0xFFFF)

#define MAX_PATH         256
#define MAX_FILENAME     128

// Extended keycodes (> 0x7F to avoid ASCII overlap)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_HOME    0x84
#define KEY_END     0x85
#define KEY_PGUP    0x86
#define KEY_PGDN    0x87
#define KEY_INSERT  0x88
#define KEY_DEL     0x89
// Synthetic keycodes the compositor injects from mouse-wheel notches, delivered to
// the focused window's on_key like any other key (scrollback nav in the terminal).
#define KEY_WHEEL_UP   0x8A
#define KEY_WHEEL_DOWN 0x8B
// Function keys F1-F12. Contiguous so `KEY_F1 + n` indexes Fn.
#define KEY_F1      0x90
#define KEY_F2      0x91
#define KEY_F3      0x92
#define KEY_F4      0x93
#define KEY_F5      0x94
#define KEY_F6      0x95
#define KEY_F7      0x96
#define KEY_F8      0x97
#define KEY_F9      0x98
#define KEY_F10     0x99
#define KEY_F11     0x9A
#define KEY_F12     0x9B

// CR4 flags
#define CR4_PAE     (1 << 5)
#define CR4_SMEP    (1 << 20)

// Page-table entry flags (PAGE_*) live in mm/paging.h now.

// EFER MSR (0xC0000080)
#define MSR_EFER    0xC0000080
#define EFER_SCE    (1 << 0)     // System Call Extensions — enables syscall/sysret
#define EFER_LME    (1 << 8)
#define EFER_LMA    (1 << 10)
#define EFER_NXE    (1 << 11)

// STAR, LSTAR, SF_MASK MSRs for syscall
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_CSTAR    0xC0000083
#define MSR_SF_MASK  0xC0000084

// ============================================================
// ESTRUCTURAS
// ============================================================
typedef struct process {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;
    char comm[32];
    char cmdline[256];
    void* page_directory;
    void* stack;
    void* kernel_stack;
    uint32_t state;         // PROC_PARKED / PROC_RUN / PROC_ZOMBIE
    uint32_t priority;
    uint32_t cpu_time;
    uint32_t start_time;
    void* files[MAX_FILES];
    uint64_t program_break;  // top of the heap (moved by SYS_SBRK)
    uint64_t heap_start;     // initial break; the lazy-sbrk fault-in window is
                             // [heap_start, program_break) — a not-present user
                             // fault there gets a fresh zeroed page on first touch
    uint32_t sched_managed;  // 1 = round-robined by the preemptive scheduler (spawn_user_path);
                             // blocking-exec and unstarted procs leave this 0 so they're skipped
    uint32_t pipe_wait_id;   // pipe index + 1 while waiting to read; 0 = none
    uint32_t waiting_for;    // pid this proc is blocked in kwait() on (0 = not waiting)
    uint32_t blocked_in_kernel; // 1 = parked mid-syscall (ring 0): the scheduler must
                             // resume it on the KERNEL CR3, since -mcmodel=large kernel
                             // code runs at low link addresses only mapped there
    uint32_t wake_tick;      // tick_count to wake a sleep()-blocked proc (0 = not sleeping)
    uint64_t futex_key;      // physical address this proc is FUTEX_WAIT-blocked on (0 = not)
    uint32_t tgid;           // thread group: pid of the LEADER that owns this group's shared
                             // heap/VMA state (0 or == own pid => this task IS the leader).
                             // CLONE_VM threads defer to it via tg_leader(), so sbrk/mmap
                             // performed by ANY thread are seen by all of them.
    uint32_t stop_sig;       // job control: signal that stopped us (SIGTSTP/SIGSTOP), 0 = not stopped
    uint32_t stopped_reported; // 1 = this stop was already reported to the parent's waitpid(WUNTRACED)
    uint32_t sched_weight;   // ticks per turn in the weighted round-robin (0 => 1)
    uint32_t sched_quantum;  // ticks left in the current turn (scheduler-internal)
    // Which CPU may run this task. 0 (the memset default, so every existing task
    // keeps its behaviour) = the BSP; N > 0 = that AP, exclusively. Pinning is
    // what keeps two cores from picking the same task — no core but N will even
    // look at it, so no "currently running on" flag is needed.
    int32_t  sched_cpu;
    int      exit_code;      // status passed to SYS_EXIT, collected by kwait()
    // Per-process file-descriptor table: opaque small ints (UFD_BASE + slot) each
    // mapping to an internal VFS handle + byte offset. Isolated per process and
    // closed when the process is reaped, so fds neither leak nor cross processes.
    int      ufd_handle[PROC_MAX_FDS];
    uint32_t ufd_offset[PROC_MAX_FDS];
    uint8_t  ufd_inuse[PROC_MAX_FDS];
    // Signals (v5.8.11). Delivered at return-to-ring-3 (signal_dispatch, from the
    // syscall path). sig_handlers: 0=SIG_DFL, 1=SIG_IGN, else a ring-3 handler VA.
    uint32_t sig_pending;             // bit i set = signal i is pending
    uint32_t sig_mask;                // bit i set = signal i is blocked (deferred)
    uint32_t sig_active;              // bit i set = handler for signal i is on the user stack now
    uint64_t sig_handlers[NSIG];      // per-signal disposition (SIG_DFL/SIG_IGN/handler VA)
    uint64_t sig_trampoline;          // ring-3 sigreturn trampoline VA (libc __sigreturn)
    // Ring-3 context saved when a handler is entered, restored by SYS_SIGRETURN:
    // [0..14]=GPRs r15..rax (frame order), [15]=RFLAGS, [16]=RIP, [17]=user RSP.
    uint64_t sig_saved[18];
    uint32_t alarm_tick;              // alarm(2) deadline in ticks (0 = none); irq_scheduler_tick
                                      // posts SIGALRM once tick_count passes it
    // Anonymous mmap regions (v5.8.12). A fault inside a vma gets a fresh zeroed
    // page with the vma's prot (vm_handle_fault); mmap_next bumps up per mapping.
    vma_t    mmap_vmas[PROC_MAX_VMAS];
    uint64_t mmap_next;
    // Per-process current working directory (absolute, normalized). Relative paths
    // in open()/getdents() resolve against it; inherited on fork, kept across execve.
    // An empty string is treated as "/".
    char     cwd[MAX_PATH];
    // stdin discipline (SYS_TTYMODE): 0 = canonical line reads, 1 = raw single-byte
    // reads with arrows as ANSI escapes. NOT inherited on fork, reset by execve —
    // only the process that asked for raw mode sees it.
    uint32_t tty_raw;
    uint8_t tty_pending[3];
    uint8_t tty_pending_len;
    uint64_t console_capture_id;
    uint64_t pipe_capture_id;
    uint64_t console_session_id;
    // FPU/SSE (XMM) state saved across context switches (#40). fxsave/fxrstor need a
    // 16-byte-aligned 512-byte area; process_t is kmalloc'd (only 8-aligned), so we keep
    // 16 bytes of slack and align at runtime (fpu_area_of()). fpu_ready is 0 until this
    // task's FP state has been initialised from the clean template on its first switch-in.
    uint8_t  fpu_area[512 + 16];
    uint32_t fpu_ready;
    uint32_t reap_claimed;  // sched_lock: exactly one collector owns cleanup
    struct process* next;
    struct process* parent;
    struct process* children;
} process_t;

// SYS_GETPROCS ABI: owned values only; no process pointers escape the table lock.
typedef struct {
    uint32_t pid, ppid, state, cpu_time;
    char comm[32];
} process_info_t;
typedef char process_info_abi_is_48_bytes[
    (sizeof(process_info_t) == 48 && __builtin_offsetof(process_info_t, comm) == 16) ? 1 : -1];

// Process states (kept numeric-compatible with the existing 0/1 usage).
#define PROC_PARKED  0   // not runnable (retired kernel thread); scheduler skips
#define PROC_RUN     1   // runnable or running
#define PROC_ZOMBIE  2   // scheduled user proc that exited; awaiting reap_zombies()
#define PROC_BLOCKED 3   // blocked in kwait()/sleep(); scheduler skips
#define PROC_STOPPED 4   // job-control stopped (SIGTSTP/SIGSTOP); parked until SIGCONT

// Weighted round-robin: the compositor (GUI) runs several ticks per turn so it
// stays responsive while background jobs (weight 1) run.
#define SCHED_WEIGHT_GUI 4

// x86_64 TSS (102 bytes)
// Layout per Intel Vol 3, Figure 7-9
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    // Intel Vol 3, Fig 7-11: exactly EIGHT reserved bytes here, not twelve.
    // This used to carry three dwords (a leftover of the 32-bit SS0/SS1/SS2
    // fields), which pushed every IST slot four bytes too high: tss_set_ist()
    // wrote IST1 at offset 40 while the CPU reads it from 36. The double-fault
    // handler's stack pointer was therefore always garbage, so a double fault
    // triple-faulted and rebooted instead of reaching the panic screen — the IST
    // mechanism has never actually worked.
    uint64_t reserved1;       // offset 28
    uint64_t ist1;            // offset 36
    uint64_t ist2;            // offset 44
    uint64_t ist3;            // offset 52
    uint64_t ist4;            // offset 60
    uint64_t ist5;            // offset 68
    uint64_t ist6;            // offset 76
    uint64_t ist7;            // offset 84
    uint64_t reserved2;       // offset 92
    uint16_t reserved3;       // offset 100
    uint16_t iomap_base;      // offset 102 -> struct is 104 bytes
} __attribute__((packed)) tss_entry_t;

// RTC time structure (also in rtc.h)
#ifndef RTC_TIME_T_DEFINED
#define RTC_TIME_T_DEFINED
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;
#endif

typedef struct {
    uint32_t tid;
    uint32_t pid;
    void* stack;
    void* kernel_stack;
    uint32_t state;
    void* entry_point;
    void* arg;
    void* return_value;
} thread_t;

typedef struct {
    uint32_t fd;
    uint32_t flags;
    uint32_t mode;
    uint32_t offset;
    void* inode;
    void* ops;
    uint32_t ref_count;
} file_t;

typedef struct {
    uint32_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t blocks;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    void* data;
} inode_t;

typedef struct {
    uint32_t ino;
    char name[MAX_FILENAME];
    uint32_t type;
} dirent_t;

typedef struct {
    char device[64];
    char mount_point[MAX_PATH];
    char fs_type[16];
    uint32_t flags;
    void* fs_data;
} mount_t;

typedef struct {
    uint8_t* data;
    uint32_t length;
    uint32_t protocol;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
} net_packet_t;

typedef struct {
    char name[16];
    uint8_t mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t mtu;
    uint32_t flags;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint64_t tx_bytes;      // total frame bytes sent (eth_send) — for the desktop NET monitor
    uint64_t rx_bytes;      // total frame bytes accepted for this host (eth_poll)
    void* driver_data;
} net_iface_t;

uint64_t net_total_bytes(void);   // tx+rx bytes summed across all interfaces (live NET-rate source)

// ============================================================
// Network byte order (host x86 is little-endian; the wire is big-endian)
// ============================================================
// Centralizes the byte swaps every protocol used to open-code inline (the
// ((x<<8)&0xFF00)|((x>>8)&0xFF) idiom, and uglier 32-bit versions). The
// __builtin_bswap* forms compile to a single bswap/xchg. NOTE: IPv4 addresses
// in this stack are stored in *network* order already (low byte = first octet,
// e.g. 127.0.0.1 == 0x0100007F), so htonl/ntohl are only for fields that are
// genuine host integers on the wire (TCP seq/ack) — IP addresses never pass
// through them.
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }

// Expand a network-order IPv4 address into four %d printf arguments, first octet
// first:  printf("%d.%d.%d.%d", IP4_OCTETS(ip)).  Replaces the
// ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF quad that was copy-pasted
// across every network printer.
#define IP4_OCTETS(ip) (int)((ip) & 0xFF), (int)(((ip) >> 8) & 0xFF), \
                       (int)(((ip) >> 16) & 0xFF), (int)(((ip) >> 24) & 0xFF)

// ============================================================
// COLORES VGA
// ============================================================
enum vga_color {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN   = 14,
    VGA_WHITE         = 15,
};

// ============================================================
// VARIABLES GLOBALES EXTERNAS
// ============================================================
extern process_t* process_table[MAX_PROCESSES];
extern uint64_t memory_total;
extern uint64_t memory_used;
extern volatile uint32_t tick_count;
extern int process_count;
extern int current_idx;
uint32_t perf_cpu_percent(void);   // most recent completed-window BSP CPU utilization, 0..100
int perf_cpu_selftest(void);       // KAT for the utilization accumulator
extern volatile int g_cpu_idle;    // 1 while the CPU is halted in an idle wait (sleep())
extern net_iface_t net_interfaces[8];
extern uint64_t saved_rsp;
extern uint64_t next_rsp;
extern uint64_t next_cr3;
extern uint64_t kernel_pml4_phys;

// ============================================================
// DECLARACIONES DE FUNCIONES ENSAMBLADOR
// ============================================================
extern void memcpy_asm(void* dest, const void* src, size_t n);
extern void memset_asm(void* dest, uint8_t val, size_t n);
extern void memset32_asm(void* dest, uint32_t val, size_t count);   // fill `count` 32bpp pixels (rep stosd)
extern void _gdt_flush(uint64_t gdt_ptr);
extern void _idt_flush(uint64_t idt_ptr);

// ============================================================
// FUNCIONES DE E/S INLINE  (moved to cpuio.h — first slice of the header split)
// ============================================================
#include "cpuio.h"

// ============================================================
// FUNCIONES UTILITARIAS
// ============================================================
static inline void *memcpy(void *dest, const void *src, size_t n) {
    memcpy_asm(dest, src, n);
    return dest;
}
static inline void *memmove(void *dest, const void *src, size_t n) {
    if (dest == src) return dest;
    if ((uintptr_t)dest < (uintptr_t)src) {
        for (size_t i = 0; i < n; i++) ((uint8_t*)dest)[i] = ((const uint8_t*)src)[i];
    } else {
        for (size_t i = n; i > 0; i--) ((uint8_t*)dest)[i-1] = ((const uint8_t*)src)[i-1];
    }
    return dest;
}
static inline char *strcpy(char *dest, const char *src) {
    char *orig = dest;
    while ((*dest++ = *src++) != '\0');
    return orig;
}
static inline char *strcat(char *dest, const char *src) {
    char *orig = dest;
    while (*dest) dest++;
    while ((*dest++ = *src++) != '\0');
    return orig;
}
static inline char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}
static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}
// Overwrite a buffer holding a secret (a typed password, key material) with zeros in a
// way the compiler may NOT optimise away: the `volatile` pointer forces every store to
// really happen, whereas a plain memset to a soon-out-of-scope/freed buffer is a legal
// dead-store the optimiser can delete — leaving the secret in RAM (CWE-316). This is the
// kernel's explicit_bzero: call it the moment a plaintext secret is no longer needed.
static inline void secure_zero(void *p, size_t n) {
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}
static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
static inline int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
static inline int atoi(const char *s) {
    int sign = 1, result = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { result = result * 10 + (*s - '0'); s++; }
    return sign * result;
}
static inline char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char*)s;   // c == '\0' matches the terminating NUL (C standard);
        if (!*s) return NULL;            // the old `while (*s)` returned NULL for c=='\0' — a
    }                                    // deviation from strchr(3) and from user/libc.c's version.
}
char *strstr(const char *haystack, const char *needle);
static inline char *strtok(char *str, const char *delim) {
    static char *next = NULL;
    if (str) next = str;
    if (!next) return NULL;
    while (*next && strchr(delim, *next)) next++;
    if (*next == '\0') return NULL;
    char *start = next;
    while (*next && !strchr(delim, *next)) next++;
    if (*next) *next++ = '\0';
    return start;
}
static inline long strtol(const char *s, char **end, int base) {
    long result = 0;
    int sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    }
    if (base == 0) base = 10;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }
    if (end) *end = (char*)s;
    return sign * result;
}

// ============================================================
// PROTOTIPOS DE FUNCIONES GLOBALES
// ============================================================
void kernel_main(uint64_t magic, void* mboot_ptr);
void kernel_panic(const char* msg, ...);
void launch_shell(void);
void nyxfetch(void);
void execute_command(const char* cmd_line);
void command_complete(const char* partial, char* out, int out_size, int* match_count);
void command_list_matches(const char* partial, char* out, int out_size);

void init_gdt(void);
void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags);
void tss_set_stack(uint64_t rsp0);
void tss_set_ist_cpu(uint32_t cpu, uint8_t ist_idx, uint64_t stack_top);
void load_tss(void);
void load_tss_for_cpu(uint32_t cpu);   // each core loads its OWN TSS (see gdt.c)
void sched_target(process_t* p);       // aim this CPU's next_rsp/next_cr3/TSS at p
extern volatile uint64_t g_ctx_switches;   // total context switches since boot (/proc/stat ctxt)
uint64_t total_procs_created(void);        // monotonic count of processes ever created (/proc/stat processes)
void sched_forget(process_t* p);       // clear p from every core's sched_cur before freeing it
void cpu_enable_sse_fpu(void);         // per-CPU CR0/CR4 FPU+SSE enable (kernel.c); every core calls it
extern uint8_t g_fpu_initial[];        // clean 512-byte fxsave template (process.c), 16-byte aligned
void init_idt(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void idt_set_gate_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist);
void gdt_load_on_ap(void);     // point an AP at the BSP's GDT (no TSS — see gdt.c)
void idt_load_on_ap(void);     // point an AP at the BSP's IDT (shared, read-only)

// User ring-3 stack (v5.8.89: demand-grown with a guard page). The TOP page is at
// USER_STACK_TOP (the SysV entry frame sits at its top); the stack grows DOWN. Only
// USER_STACK_INIT_PAGES are committed at load (elf_load_image / create_user_process);
// vm_handle_fault materialises further pages on touch, down to a floor of
// USER_STACK_MAX_PAGES total (USER_STACK_LOW). This lets a deep program use far more
// stack than the old fixed 64 KB while committing almost nothing up front. A fault
// BELOW USER_STACK_LOW is a genuine stack overflow → SIGSEGV; the page just under the
// floor [USER_STACK_LOW-PAGE_SIZE, USER_STACK_LOW) is a permanent unmapped GUARD, and a
// fault there is reported ("[stack overflow]") for a clear diagnostic before the kill.
#define USER_STACK_TOP        0x00007FFFFFFFE000ULL   // base of the top stack page
#define USER_STACK_INIT_PAGES 4                        // committed at load (16 KB)
#define USER_STACK_MAX_PAGES  128                      // demand-grow ceiling (512 KB)
#define USER_STACK_LOW        (USER_STACK_TOP - (uint64_t)(USER_STACK_MAX_PAGES - 1) * PAGE_SIZE)

// IST stack sizes
#define IST_STACK_SIZE 8192
#define IST_DOUBLE_FAULT 1
void init_isr(void);
// fault_cr3: the CR3 that was live when the exception was taken. isr_common must
// pass it because it switches to the kernel tables before calling in — read_cr3()
// inside the handler would report the KERNEL's CR3 for every ring-3 fault.
void isr_handler(uint64_t int_no, uint64_t rip, uint64_t error, uint64_t cs, uint64_t* frame,
                 uint64_t fault_cr3);
void init_irq(void);
void irq_handler(uint64_t irq_no);
void irq_install_handler(int irq, void (*handler)(void*));
void irq_unmask(int irq);
void irq_mask(int irq);
void irq_eoi(uint64_t int_no);

// One firmware physical-memory-map region (from the multiboot2 type-6 tag). `type`
// mirrors the E820/multiboot classes: 1 = available RAM; anything else (2 reserved,
// 3 ACPI-reclaimable, 4 ACPI-NVS, 5 bad) is NOT usable and must stay out of the
// physical allocator. init_memory frees ONLY type-1 pages, so every hole (the low
// VGA/BIOS window, the high SeaBIOS/ACPI regions, PCI MMIO) is reserved automatically.
typedef struct { uint64_t base; uint64_t len; uint32_t type; } mb_mmap_entry_t;
void init_memory(uint64_t mem_size, const mb_mmap_entry_t* mmap, int mmap_count);
#include "smp.h"

/* The syscall entry/exit path's scratch slots. They were three plain globals in
 * isr_stubs.asm; as of v5.8.91 they live in the PER-CPU block, and these names
 * resolve to the calling core's copy so the C code that reads and writes them
 * keeps its exact shape.
 *
 * What this fixes: cross-CPU clobbering. Two cores in a syscall at once would
 * have overwritten each other's saved user CR3 and RSP — the wild-writer class
 * that made the v5.8.83 hunt so painful, multiplied by every core.
 *
 * What this does NOT fix, on purpose: they are still shared between processes
 * time-slicing on the SAME core, so the save/restore dances around every
 * blocking wait (do_waitpid, pipe reads, futex, signal parking) remain
 * necessary. Ending those needs the slots saved into the process at context
 * switch time, which is a separate change from making the machine SMP-safe.
 */
#define user_rsp          (cpu_self()->sc_user_rsp)
#define kernel_rsp        (cpu_self()->sc_kernel_rsp)
#define user_cr3          (cpu_self()->sc_user_cr3)
#define syscall_frame_ptr (cpu_self()->sc_frame_ptr)

/* The context-switch hand-off slots, per-CPU as of v5.8.92: the ISR parks the
 * interrupted RSP in saved_rsp, the scheduler answers with next_rsp/next_cr3.
 * One shared set was fine while only the BSP ever switched; with a second core
 * scheduling, they would have described each other's switch. */
#define saved_rsp         (cpu_self()->sc_saved_rsp)
#define next_rsp          (cpu_self()->sc_next_rsp)
#define next_cr3          (cpu_self()->sc_next_cr3)

/* THE user/kernel address boundary. Canonical user space is the lower half:
 * [USER_SPACE_MIN, USER_SPACE_END). Anything at or above USER_SPACE_END is
 * non-canonical or the higher-half kernel mirror. Defined here rather than in
 * syscall.c because it is not a syscall concept — the ELF loader needs the same
 * bound (a PT_LOAD above it maps into the kernel's own page tables), and so does
 * anything else that accepts an address from userspace. One definition, so the
 * boundary cannot drift between the places that enforce it. */
/* Largest single PT_LOAD we will honour. Bounds the per-page allocation loop
 * in elf_load_image at 65536 iterations; no NyxOS binary comes close. */
#define ELF_MAX_SEGMENT (256ULL * 1024 * 1024)
#define USER_SPACE_MIN 0x1000ULL
#define USER_SPACE_END 0x0000800000000000ULL

uint32_t get_free_pages(void); // free physical frames (SMP stress invariant)
uint32_t get_total_pages(void); // total physical frames the allocator manages (capped at MAX_PAGES)
void mem_pool_kb(uint32_t* used_kb, uint32_t* free_kb, uint32_t* total_kb); // one honest memory-usage source
process_t* create_kernel_thread_on_cpu(const char* name, void* entry, int cpu);
void enable_smep_smap(void);   // CR4.SMEP/SMAP if the CPU supports them (kernel.c)
int  cpu_apply_smep_smap(void); // same, silent + returns bit0=SMEP bit1=SMAP (APs use this)
void* kmalloc(size_t size);
void* kmalloc_aligned(size_t size, uint32_t align);
void kfree(void* ptr);
int  kfree_selftest(void);   // double-free-poison KAT (see memory.c)
int  slab_selftest(void);    // slab allocator KAT: distinctness/integrity/reuse/routing (memory.c)
int  page_alloc_selftest(void); // physical page allocator refcount/COW KAT (#50/#53/#54 + net-zero; memory.c)
int  wx_selftest(void);      // W^X policy KAT: mmap/mprotect reject writable+executable prot (mmap.c)
void* krealloc(void* ptr, size_t size);
uint32_t ksize(void* ptr);   // usable bytes of a kmalloc'd block (from its header)
void* alloc_page(void);
void free_page(void* addr);
void page_incref(void* addr);           // COW: share an allocated page (fork)
uint32_t page_get_refcount(void* addr); // COW: how many PTEs point at this page
void page_pin(void* addr);              // mark a frame un-freeable (shared-libc masters)
void slab_init_all(void);

// Paging / virtual-memory API (map_page*, PML4 lifecycle, vm_handle_fault, COW)
// lives in mm/paging.h now (included at the top of this header).

void init_heap(void);
void* heap_alloc(size_t size);
void heap_free(void* ptr);
// Live kernel-heap stats (bytes; any out-arg may be NULL): used = committed to live allocations,
// total = fixed heap size, hiwater = peak used, fails = allocations that found no fit. See heap.c.
void heap_stats(size_t* used, size_t* total, size_t* hiwater, uint32_t* fails);

void init_process(void);
int process_snapshot(process_info_t* records, int max);
process_t* create_process(const char* name, void* entry, uint64_t flags);
process_t* create_user_process(const char* name, void* entry, void* user_stack, uint64_t* page_dir);
int do_fork(void);   // SYS_FORK: COW-clone the caller; returns child pid to parent, 0 in child
int do_waitpid(int pid, int* out_code, int options); // SYS_WAITPID: reap a child; -1 none, 0 WNOHANG-none-ready, >0 pid
// Signals (signal.c). signal_dispatch is called from syscall_entry (isr_stubs.asm)
// on the return path, with the saved user frame; it delivers one pending signal.
void signal_dispatch(uint64_t* frame);
int  signal_deliver_fault(uint64_t* frame, int sig); // catchable CPU-exception signal (isr.c) -> handler; 1 if delivered
int  do_kill(int pid, int sig);                      // SYS_KILL: post `sig` to `pid`
long do_signal(int sig, uint64_t handler, uint64_t trampoline); // SYS_SIGNAL: install disposition
void do_sigreturn(void);                             // SYS_SIGRETURN: restore pre-handler context
long do_sigprocmask(int how, uint64_t set, uint64_t oldset_ptr); // SYS_SIGPROCMASK: read/change block mask
unsigned int do_alarm(unsigned int seconds);                     // SYS_ALARM: schedule SIGALRM after N s
void signal_raise(process_t* p, int sig);            // caller owns lifetime or holds sched_lock; no relocking
int  signal_pending(process_t* p);                   // 1 if a deliverable (unblocked) signal waits
int  signal_check_stop(process_t* p);                // park on a pending stop signal; 1 if it did (restart)
void signal_send_foreground(int sig);                // keyboard Ctrl-C -> foreground process
// Anonymous mmap (mmap.c). Pages within a returned region demand-fault to zero.
uint64_t do_mmap(uint64_t addr, uint64_t length, int prot, int flags,
                 int file_handle, uint32_t file_size, uint32_t file_off); // SYS_MMAP; MAP_FAILED=(uint64_t)-1
int      do_munmap(uint64_t addr, uint64_t length);  // SYS_MUNMAP
int      do_mprotect(uint64_t addr, uint64_t length, int prot); // SYS_MPROTECT
vma_t*   vma_find(process_t* p, uint64_t addr);      // vm_handle_fault lookup
void     mmap_free_bufs(process_t* p);               // release file-backed snapshots at reap
int do_execve(const uint8_t* data, uint32_t size,
              char* const* kargv, int argc,
              char* const* kenvp, int envc, const char* path); // SYS_EXECVE: replace caller's image; -1 on failure
void proc_set_comm(process_t* p, const char* path); // set comm to basename(path) minus ".elf"
int copy_to_user(uint64_t udst, const void* src, uint64_t len); // via user_cr3 page walk (syscall.c)
int elf_load_image(const uint8_t* data, uint32_t size, uint64_t** out_pd,
                   uint64_t* out_entry, uint64_t* out_stack_top, uint64_t* out_brk);
int elf_load_args(const uint8_t* data, uint32_t size, char* const* argv, int argc,
                  process_t** out_proc);                 // elf_load + optional argv seed
uint64_t build_argv_stack(uint64_t* pd, uint64_t stack_top, char* const* kargv, int argc); // process.c

// Anonymous pipes (pipe.c)
int  pipe_new(void);                    // alloc a pipe (1 read ref + 1 write ref) -> id, or -1
int  pipe_read(int id, char* kbuf, int n);   // blocking read into kbuf; 0 = EOF
int  pipe_write(int id, const char* src, int n); // non-blocking write; -1 = broken pipe
void pipe_close_end(int id, int is_write);   // drop one ref; frees the pipe at zero
int  pipe_readable(int id);                  // poll(): 1 if read would return now
void pipe_incref(int id, int is_write);      // fork(): inherit a pipe fd
void reap_user_process(process_t* proc);
void close_proc_fds(process_t* proc);   // close a process's fds (called at exit + reap)
int  tg_reassign_leader(process_t* dying); // hand a dying thread's shared heap/mmap/fd table to a LIVE group member; 1 = handed off (do NOT close), 0 = last member out (caller closes fds). Run BEFORE close_proc_fds (issue #73)
void destroy_process(uint64_t pid);
process_t* find_process(uint64_t pid);
process_t* get_current_process(void);
void schedule(void);
void switch_context(uint64_t* old_rsp_ptr, uint64_t new_rsp);
uint64_t create_task_stack(uint64_t stack_top, uint64_t entry_point);
void ensure_idle_process(void);

void init_syscalls(void);
void setup_syscall_msrs(void);
void set_kernel_rsp(uint64_t rsp);
uint64_t syscall_handler(uint64_t syscall_no, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
extern void syscall_entry(void);

void init_vfs(void);
int vfs_open(const char* path, int flags, mode_t mode);
int vfs_read(int fd, void* buf, size_t count);
int vfs_write(int fd, const void* buf, size_t count);
int vfs_pread(int fd, void* buf, uint32_t count, uint32_t offset);
int vfs_pwrite(int fd, const void* buf, uint32_t count, uint32_t offset);
int nyxconf_get(const char* text, const char* key, char* out, int outsz);  // nyx.conf parser (rice config)
int vfs_close(int fd);
void vfs_dup(int fd);   // count an extra fd aliasing a mount-backed mirror
int vfs_stat(const char* path, uint32_t* size, int* is_dir);  // 0 ok / -1 not found
int vfs_fstat(int fd, uint32_t* size, int* is_dir);           // by internal vfs fd
int vfs_mkdir(const char* path, mode_t mode);
int vfs_unlink(const char* path);
int vfs_rmtree(const char* path);   // recursive remove (rm -r): path + its whole subtree
dirent_t* vfs_readdir(int fd);
int vfs_isdir(const char* path);   // 1 if `path` resolves to a directory, else 0
uint32_t vfs_fsize(int fd);
uint8_t* vfs_fdata(int fd);
int vfs_create_from_mem(const char* path, uint8_t* data, uint32_t size);
void init_load_modules(void);

void init_net(void);
void kernel_poll_net(void);
void init_background_tasks(void);
void run_background_tasks(void);
void irq_scheduler_tick(void);

// Preemptive scheduler (process.c): dormant until sched_enable(); mtdemo_start()
// spins up two demo kernel threads and turns preemption on for the self-test.
void sched_enable(void);
void sched_adopt_boot_process(process_t* p);
void sched_disable(void);
int  sched_is_enabled(void);
int  mtdemo_start(void);
extern volatile uint64_t mtdemo_a_count, mtdemo_b_count;

// Preemption critical sections: while the disable-count is nonzero the scheduler
// keeps the current thread (protects non-reentrant shared state — the heap).
void preempt_disable(void);
void preempt_enable(void);
// Preemptive user (ring-3) processes: spawn one from an ELF path into the
// scheduler (non-blocking), and reap the ones that have exited.
int  spawn_user_path(const char* path);
int  spawn_user_path_args(const char* path, char* const* argv, int argc); // spawn + forward argv
int  spawn_user_path_args_redir(const char* path, char* const* argv, int argc,
                                const char* out_path, int out_append,
                                const char* err_path, int err_append);  // + stdout `>`/`>>` (issue #88) and stderr `2>`/`2>>` redirects
void gui_launch_elf(const char* path);  // desktop-icon launch: foreground-run an ELF (kernel.c)
int  doom_window_present(const uint32_t* src, uint32_t w, uint32_t h); // reroute DOOM's frame into its window (compositor.c)
// Desktop window launchers (games / apps / P4 perf-demos, all in compositor.c) live in
// their own focused header now; kernel.h re-includes it so every includer is unaffected.
#include "../gui/core/launchers.h"
void reap_zombies(void);
int do_clone(uint64_t fn, uint64_t stack, uint64_t arg, uint64_t flags); // CLONE_VM thread
int do_futex(uint64_t uaddr, int op, uint32_t val);                     // FUTEX_WAIT/WAKE
// Does any live task OTHER than `except` still use this address space? CLONE_VM threads
// share one PML4, so only the last one out may free_page_directory() it.
int addr_space_shared(void* pml4, struct process* except);
uint64_t user_v2p(uint64_t vaddr);   // user VA -> physical (syscall.c); futex queue key
// Resolve the task that owns this thread group's SHARED heap/VMA state. A CLONE_VM
// thread returns its leader; anything else returns itself. Every sbrk/mmap/VMA lookup
// goes through this, so the whole group sees one heap and one mmap table.
struct process* tg_leader(struct process* p);
// Block the current thread until child `pid` exits; returns its exit code (or -1
// if there is no such process). Used by `exec` to run a foreground job.
int  kwait(uint32_t pid);
// Block the current thread until all of its children have exited (the `wait` cmd).
void kwait_all(void);
// Called from SYS_EXIT: wake a parent blocked in kwait() on this child.
void wake_waiters(process_t* child);

void init_timer(uint32_t frequency);
uint32_t get_ticks(void);
void uptime_mark_boot(void);        // capture the boot wall-clock (call once, early in kernel_main)
uint32_t get_uptime_seconds(void);  // honest uptime: real wall-clock seconds since boot (RTC, not ticks)
// Civil (proleptic-Gregorian, UTC) date/time -> Unix epoch seconds (Hinnant days-from-civil).
// The ONE epoch-conversion helper: uptime, SYS_GETTIMEOFDAY and `totp` all route through it, so
// the conversion can't drift between them. Pure; pinned by uptime_epoch_selftest (the `uptime` KAT).
uint64_t civil_to_epoch(int y, int m, int d, int h, int mi, int s);
void sleep(uint32_t milliseconds);

void init_serial(void);
void serial_putchar(char c);
void serial_puts(const char* str);
void klog_dump(void);                         // dump the captured kernel log ring (`dmesg`)
char serial_getchar(void);
char serial_getchar_nonblock(void);

void init_keyboard(void);
char getchar(void);
char getchar_poll(void);
int  keyboard_has_input(void);               // poll(): 1 if a key/serial byte is waiting
int  getkey_poll(void);
#define KEY_MOD_CTRL 1u
#define KEY_MOD_ALT  2u
int  getkey_poll_with_modifiers(unsigned* modifiers);
int  keyboard_next_event(void);              // SYS_GETKEYEVENT: raw key make/break, or -1
void keyboard_irq_handler(void* unused);
int is_ctrl_pressed(void);
int is_alt_pressed(void);

void init_screen(void);
int putchar(int c);
int puts(const char* str);
void set_putchar_hook(int (*hook)(int c));
void set_putchar_hook_session(int (*hook)(int c), uint64_t session);
void console_capture_inherit(process_t* child);
uint64_t console_session_open(void);
void console_session_close(uint64_t id);
int console_session_read(uint64_t id, char* out, int cap, uint32_t* omitted);

// System text clipboard (kernel.c). One shared buffer; the `clip` command and the
// GUI terminal/editor copy/paste through it. clipboard_get copies up to `cap` bytes
// from the start and returns how many it wrote.
#define CLIPBOARD_MAX 4096
void     clipboard_set(const char* data, uint32_t len);
uint32_t clipboard_get(char* out, uint32_t cap);
uint32_t clipboard_len(void);
void     clipboard_clear(void);

// Desktop notification / toast (gui/core/compositor.c). Shows `title`/`body` as
// a purple Nyx card in the top-right of the desktop for a few seconds; the
// compositor repaints while it is alive so it self-dismisses.
void notify_push(const char* title, const char* body);

int printf(const char* fmt, ...);
int vprintf(const char* fmt, va_list args);
void clear_screen(void);
char* itoa(int value, char* str, int base);
char* lltoa(long long value, char* str, int base);
uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg);
void set_terminal_color(uint8_t color);
uint8_t get_terminal_color(void);
void pipe_start(void);
int pipe_stop(void); // captured length, or -1 if output exceeded the fixed buffer
const char* pipe_get_data(void);
int pipe_get_len(void);

int elf_load(const uint8_t* data, uint32_t size, process_t** out_proc);
int elf_validate(const uint8_t* data, uint32_t size);

int initramfs_load(void);
void initramfs_boot(void);

void init_ext2(void);
int  ata_init(void);
int  ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void* buf);
int  ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void* buf);
int  ata_identify(uint8_t drive, uint16_t* buf);
int  ata_flush(void);   // flush the drive write cache once per operation
uint32_t crc32_calc(const uint8_t* data, uint32_t len);   // reflected IEEE CRC-32 (PNG/GPT)
int  ext2_mount(uint8_t drive, uint32_t part_lba);
int64_t ext2_resolve(const char* path);
int64_t ext2_get_size(const char* path);
int  ext2_read_file(const char* path, void* buf, uint32_t maxlen);
int  ext2_write_file(const char* path, const void* buf, uint32_t len);
int  ext2_create_file(const char* path);
int  ext2_readdir(const char* path, dirent_t* entries, uint32_t max_entries);

// Network stack public API (drivers/net + net/ protocols + net.c's TCP/socket layer) lives in
// its own focused header now; kernel.h re-includes it so every includer is unaffected.
#include "../net/netstack.h"

// SMP
extern uint32_t cpu_count;
void smp_init(void);

// VFS mount layer
#define FS_TYPE_EXT2  1
#define MAX_MOUNT_POINTS 8
typedef struct {
    int type;
    char mount_point[MAX_PATH];
    void* fs_data;
    int64_t (*resolve)(const char* path);   // inode > 0, absent = 0, error = -1
    int64_t (*get_size)(const char* path);  // bytes >= 0, error = -1
    int (*read_file)(const char* path, void* buf, uint32_t maxlen);
    int (*write_file)(const char* path, const void* buf, uint32_t len);
    int (*readdir)(const char* path, dirent_t* entries, uint32_t max_entries);
    int (*mkdir)(const char* path);
    int (*unlink)(const char* path);
} mount_entry_t;

int vfs_mount(const char* mount_point, int fs_type, void* fs_data);
mount_entry_t* vfs_find_mount(const char* path);
const char* vfs_getcwd(void);
int vfs_normalize_path(const char* cwd, const char* path, char* out, int outsz);
const char* vfs_realpath(const char* path, char* out, int outsz);  // canonical absolute path (. / .. collapsed)
int vfs_chdir(const char* path);
void* vfs_getcwd_node(void);      // opaque current-directory node (per-shell CWD)
void  vfs_setcwd_node(void* n);
void* vfs_root_node(void);
void* vfs_path_node(const char* path);   // opaque node for a path, or NULL (dirs only)
void vfs_list_dir(const char* path);
void vfs_cat_file(const char* path);
int vfs_touch(const char* path);
int vfs_symlink(const char* linkpath, const char* target);  // create a symlink -> target
int vfs_readlink(const char* path, char* buf, int cap);     // read a symlink's target (no follow)
int vfs_chmod(const char* path, uint16_t mode);             // set a node's permission bits
int vfs_getmode(const char* path);                          // read a node's mode bits, -1 if absent
int vfs_write_file(const char* path, const void* buf, uint32_t len);
int vfs_cp(const char* src, const char* dst);
int vfs_cp_new(const char* src, const char* dst);  // 0 copied, 1 occupied, -1 failed; not atomic
int vfs_rename(const char* old, const char* new);

extern int keyboard_layout;
void set_keyboard_layout(int layout);

int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
char* strcasestr(const char *haystack, const char *needle);

// Low-level graphics + input primitives (VBE mode set, the software framebuffer, PS/2 mouse,
// bitmap font) live in their own focused header now; kernel.h re-includes it so every includer
// is unaffected.
#include "../gui/core/gfx_prims.h"

void gui_demo(void);
void speaker_init(void);
void speaker_on(uint32_t frequency);
void speaker_off(void);
void speaker_beep(uint32_t frequency, uint32_t duration_ms);

int sb16_init(void);
int sb16_play_pcm(const uint8_t* data, uint32_t len, uint32_t sample_rate, int bits, int channels);
void sb16_stop(void);
int sb16_is_playing(void);

void rtc_init(void);
void rtc_read_time(rtc_time_t* time);

// Compositor
void compositor_init(void);
void compositor_run(void);
void compositor_redraw_now(void);   // forced one-shot recomposite
void compositor_foreground_tick(void); // output-aware foreground wait repaint

#endif // KERNEL_H
