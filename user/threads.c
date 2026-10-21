#include "libc.h"

/* threads — exercise clone(CLONE_VM) + futex (v5.8.87) and the SHARED thread-group
 * heap (v5.8.88).
 *
 * Two things are proven here:
 *  1) MUTEX + SHARED MEMORY: NTHREADS threads each bump a shared counter ITERS times
 *     behind a futex mutex. `counter` is deliberately non-atomic, so landing exactly on
 *     NTHREADS*ITERS means both the address-space sharing and the lock work.
 *  2) SHARED HEAP: each thread malloc()s its own block and signs it. Before the thread
 *     group shared one program_break, every thread bumped its OWN copy of the break and
 *     they were handed OVERLAPPING regions — so distinct, non-overlapping, still-intact
 *     blocks are the proof that multi-threaded malloc is now coherent.
 *
 * The lock is the textbook futex mutex: an atomic xchg fast path, entering the kernel
 * only when the lock was already held.
 */
#define NTHREADS 4
#define ITERS    2000
#define STACKSZ  8192
#define BLKSZ    256

static volatile int lock    = 0;   /* 0 = free, 1 = held */
static volatile int counter = 0;   /* what the threads fight over (non-atomic on purpose) */
static volatile int done    = 0;   /* how many workers have finished */
static char  stacks[NTHREADS][STACKSZ];
static void* blocks[NTHREADS];     /* each worker's malloc'd block */
static volatile int shared_fd = -1; /* opened by a THREAD, then used by main */

static void lock_acquire(void) {
    for (;;) {
        int prev;
        __asm__ volatile("xchgl %0, %1" : "=r"(prev), "+m"(lock) : "0"(1) : "memory");
        if (prev == 0) return;            /* it was free — ours now */
        futex_wait(&lock, 1);             /* held: sleep until the owner releases */
    }
}

static void lock_release(void) {
    __asm__ volatile("" ::: "memory");
    lock = 0;
    futex_wake(&lock, 1);
}

/* clone() delivers `arg` in RDI, matching the SysV ABI. A thread must never RETURN
 * (there is nowhere to return to) — it ends with exit(). */
static void worker(void* arg) {
    long id = (long)arg;

    /* Heap test: this malloc must come out of the thread GROUP's heap. */
    char* b = (char*)malloc(BLKSZ);
    blocks[id] = b;
    if (b) for (int i = 0; i < BLKSZ; i++) b[i] = (char)('A' + id);

    /* fd test: one thread opens a file and leaves it open. With a per-thread fd table
     * this descriptor would be invisible to main; with the group table it just works. */
    if (id == 0) {
        int fd = (int)open("/thrfd.txt", O_CREAT, 0);
        if (fd >= 0) { write(fd, "shared-fd", 9); shared_fd = fd; }
    }

    for (int i = 0; i < ITERS; i++) {
        lock_acquire();
        counter++;                        /* the lock is what makes this safe */
        lock_release();
    }
    lock_acquire();
    done++;
    lock_release();
    futex_wake(&done, 1);                 /* nudge main, which waits on `done` */
    exit(0);
}

static unsigned char fault_stack[16384] __attribute__((aligned(16)));
static int fault_fd, fault_leader;
static sigjmp_buf fault_recover;
static volatile int fault_caught;

static void fault_handler(int sig) {
    fault_caught = sig;
    siglongjmp(fault_recover, 1);
}

static void fault_survivor(void* unused) {
    (void)unused;
    /* Wait for the parent to reap the leader, rather than relying on a delay. */
    for (int i = 0; i < 200 && kill(fault_leader, 0) == 0; i++) sleep_ms(10);
    if (kill(fault_leader, 0) == 0) exit(19);
    int value = 73;
    exit(write(fault_fd, &value, sizeof(value)) == sizeof(value) ? 0 : 20);
}

static void fault_worker(void* unused) {
    (void)unused;
    __asm__ volatile("ud2");
    exit(21);
}

static int fault_case(int mode) {
    int fds[2];
    if (pipe(fds)) return 1;
    long child = fork();
    if (child < 0) { close(fds[0]); close(fds[1]); return 2; }
    if (!child) {
        close(fds[0]);
        fault_fd = fds[1];
        fault_leader = (int)getpid();
        if (mode == 5) __asm__ volatile("ud2");
        if (mode == 4) {
            int status = -1;
            int tid = clone((void*)fault_worker, fault_stack + sizeof(fault_stack), 0, CLONE_VM);
            if (tid < 0 || waitpid(tid, &status) != tid || status != 128 + SIGILL) exit(22);
            int value = 73;
            exit(write(fault_fd, &value, sizeof(value)) == sizeof(value) ? 0 : 23);
        }
        if (clone((void*)fault_survivor, fault_stack + sizeof(fault_stack), 0, CLONE_VM) < 0) exit(24);
        if (mode == 1) __asm__ volatile("ud2");
        if (mode == 2) __asm__ volatile("xor %%edx,%%edx; mov $1,%%eax; xor %%ecx,%%ecx; div %%ecx" ::: "rax", "rcx", "rdx");
        if (mode == 3) *(volatile unsigned long*)0x600000000000UL = 1;
        if (mode == 6) {
            fault_caught = 0;
            signal(SIGILL, fault_handler);
            if (!sigsetjmp(fault_recover, 1)) __asm__ volatile("ud2");
            if (fault_caught != SIGILL) exit(25);
        }
        exit(0);
    }
    close(fds[1]);
    int status = -1, value = -1;
    if (waitpid(child, &status) != child) { close(fds[0]); return 3; }
    int expected = (mode == 1 || mode == 5) ? 128 + SIGILL :
                   mode == 2 ? 128 + SIGFPE : mode == 3 ? 128 + SIGSEGV : 0;
    if (status != expected) { close(fds[0]); return 4; }
    long count = read(fds[0], &value, sizeof(value));
    close(fds[0]);
    if (mode == 5) return count == 0 ? 0 : 5;
    return count == sizeof(value) && value == 73 ? 0 : 6;
}

static int fault_tests(void) {
    static const char* names[] = {
        "normal", "leader-ill", "leader-fpe", "leader-segv",
        "worker-ill", "last-member", "handled-ill"
    };
    int failures = 0;
    for (int i = 0; i < 7; i++) {
        int result = fault_case(i);
        printf("threads: fault case %s -> %s (%d)\n", names[i], result ? "FAIL" : "OK", result);
        failures += result != 0;
    }
    printf("threads: fault lifecycle %d/7 passed\n", 7 - failures);
    return failures ? 1 : 0;
}

static void capture_worker(void* unused) {
    (void)unused;
    print("CAPTURE_CLONE\n");
    exit(0);
}

static int capture_tests(void) {
    if (write(1, "CAPTURE_STDOUT\n", 15) != 15 ||
        write(2, "CAPTURE_STDERR\n", 15) != 15) return 1;
    print("CAPTURE_LEGACY\n");
    int status = -1;
    long tid = clone((void*)capture_worker, fault_stack + sizeof(fault_stack), 0, CLONE_VM);
    if (tid < 0 || waitpid(tid, &status) != tid || status) return 2;
    long child = fork();
    if (child < 0) return 3;
    if (!child) {
        print("CAPTURE_FORK\n");
        char* args[] = {"sh", "-c", "echo CAPTURE_EXEC", NULL};
        execve("/sh.elf", args, NULL);
        exit(4);
    }
    status = -1;
    if (waitpid(child, &status) != child || status) return 5;
    print("CAPTURE_DONE\n");
    return 0;
}

static nyx_procinfo_t capture_procs[128];
static int capture_wait(void) {
    long self = getpid();
    print("CAPTURE_WAIT_READY\n");
    // Wait for the test's later foreground sleep, not a host wall-clock delay.
    for (int attempt = 0; attempt < 24000; attempt++) {
        long count = getprocs(capture_procs, 128);
        for (long i = 0; i < count; i++) {
            if (capture_procs[i].pid > (unsigned long)self &&
                !strcmp(capture_procs[i].comm, "sleep")) {
                print("BACKGROUND_ORIGIN_A\n");
                return 0;
            }
        }
        usleep(5000);
    }
    print("CAPTURE_WAIT_TIMEOUT\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--faults")) return fault_tests();
    if (argc == 2 && !strcmp(argv[1], "--capture")) return capture_tests();
    if (argc == 2 && !strcmp(argv[1], "--capture-wait")) return capture_wait();
    if (argc != 1) { printf("Usage: threads [--faults|--capture|--capture-wait]\n"); return 1; }
    printf("threads: spawning %d threads x %d increments each (shared address space)\n",
           NTHREADS, ITERS);
    for (long i = 0; i < NTHREADS; i++) {
        unsigned long top = (unsigned long)&stacks[i][STACKSZ];
        top &= ~15UL;                     /* stack grows down; keep it 16-aligned */
        int tid = clone((void*)worker, (void*)top, (void*)i, CLONE_VM);
        if (tid < 0) { printf("threads: clone #%ld FAILED\n", i); return 1; }
        printf("threads: started tid %d\n", tid);
    }

    /* Re-reading `done` each pass keeps the compare-and-sleep race-free. */
    while (done < NTHREADS) futex_wait(&done, done);

    int expected = NTHREADS * ITERS;
    printf("threads: counter = %d (expected %d) -> %s\n",
           counter, expected, counter == expected ? "OK" : "MISMATCH");

    /* Every block must exist, still hold its own signature, and not overlap another. */
    int heap_ok = 1;
    for (int i = 0; i < NTHREADS; i++) {
        char* bi = (char*)blocks[i];
        if (!bi) { heap_ok = 0; continue; }
        if (bi[0] != (char)('A' + i) || bi[BLKSZ - 1] != (char)('A' + i)) heap_ok = 0;
        for (int j = i + 1; j < NTHREADS; j++) {
            char* bj = (char*)blocks[j];
            if (bj && bi < bj + BLKSZ && bj < bi + BLKSZ) heap_ok = 0;   /* overlap! */
        }
    }
    printf("threads: heap    = %s (%d per-thread malloc blocks distinct + intact)\n",
           heap_ok ? "OK" : "OVERLAP/CORRUPT", NTHREADS);

    /* The fd was opened by thread 0; main reading through it proves the group shares
     * one descriptor table (with a per-thread table this fd simply wouldn't exist here). */
    int fd_ok = 0;
    if (shared_fd >= 0) {
        char buf[16];
        lseek(shared_fd, 0, SEEK_SET);
        int n = read(shared_fd, buf, 9);
        fd_ok = (n == 9 && buf[0] == 's' && buf[8] == 'd');
        close(shared_fd);
    }
    printf("threads: fds     = %s (main used fd %d opened by a thread)\n",
           fd_ok ? "OK" : "NOT SHARED", shared_fd);

    return (counter == expected && heap_ok && fd_ok) ? 0 : 1;
}
