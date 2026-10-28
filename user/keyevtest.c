#include "libc.h"

/* keyevtest — exercise the v5.9.30 raw key-event path (DOOM's DG_GetKey enabler).
 * Polls SYS_GETKEYEVENT and prints every make/break. Unlike stdin (cooked, press-
 * only), this reports BOTH press and release, with layout-independent Set-1 scancodes
 * (bit 0x80 = E0-extended arrow/nav). A game reads keys this way. */

static long wait_collect(int pid, int* status, int flags);

static int foreground_test(void) {
    unsigned long self = getpid();
    const unsigned long invalid[] = {0x100000000ULL | self, 0x100000000ULL,
                               0xffffffff00000000ULL | self, ~0ULL};
    int failed = 0;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (setfg((long)self) != 0) return 1;
        long rc = setfg((long)invalid[i]);
        printf("keyevtest: foreground invalid %u -> %ld %s\n", i, rc, rc == -1 ? "PASS" : "FAIL");
        if (rc != -1) failed++;
    }
    for (int i = 0; i < 32; i++) {
        int status = -1;
        long child = fork();
        if (child < 0) return 1;
        if (!child) exit(0);
        int ok = setfg(child) == 0;
        if (wait_collect((int)child, &status, 0) != child || status != 0) ok = 0;
        if (setfg((long)self) != 0 || setfg(child) != -1) ok = 0;
        if (!ok) failed++;
    }
    printf("keyevtest: foreground collection %s (32 children)\n", failed ? "FAIL" : "PASS");
    if (setfg(0) != 0 || setfg((long)self) != 0) failed++;
    printf("keyevtest: foreground %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}

static volatile int foreground_interrupts;
static nyx_procinfo_t foreground_procs[128];
static void foreground_interrupt(int sig) {
    if (sig == SIGINT) foreground_interrupts++;
}

static int foreground_signal_test(void) {
    if (foreground_test() != 0) return 1;
    signal(SIGINT, foreground_interrupt);
    unsigned long self = getpid(), other = 0;
    long count = getprocs(foreground_procs, 128);
    for (long i = 0; i < count; i++) {
        if (foreground_procs[i].pid && foreground_procs[i].pid != self && foreground_procs[i].state != 2) {
            other = foreground_procs[i].pid;
            break;
        }
    }
    if (!other) return 1;
    if (setfg((long)(0x100000000ULL | other)) != -1) {
        setfg((long)self);
        return 1;
    }
    printf("keyevtest: foreground signal ready; press Ctrl+C\n");
    for (int i = 0; i < 2000 && !foreground_interrupts; i++) usleep(5000);
    printf("keyevtest: foreground signal %s (%d interrupts)\n",
           foreground_interrupts == 1 ? "PASS" : "FAIL", foreground_interrupts);
    return foreground_interrupts == 1 ? 0 : 1;
}

static volatile int wait_interrupts;

static void wait_interrupt(int sig) {
    if (sig == SIGUSR1) wait_interrupts++;
}

static long wait_collect(int child, int* status, int options) {
    for (int i = 0; i < 500; i++) {
        long result = waitpid3(child, status, options | WNOHANG);
        if (result) return result;
        usleep(10000);
    }
    return 0;
}

/* Run from a terminal: type 'a' at each of the three "resumed" prompts. */
static int wait_return_test(void) {
    int failed = 0;
    for (int kind = 0; kind < 3; kind++) {
        for (int action = 0; action < 3; action++) {
            int ready[2], status = -1;
            if (pipe(ready)) return 1;
            int child = (int)fork();
            if (child < 0) { close(ready[0]); close(ready[1]); return 1; }
            if (!child) {
                close(ready[0]);
                wait_interrupts = 0;
                if (signal(SIGUSR1, wait_interrupt) == SIG_ERR ||
                    ttymode(kind == 1 ? TTY_RAW : TTY_CANON) < 0) exit(2);
                if (write(ready[1], "R", 1) != 1) exit(3);
                close(ready[1]);
                char byte = '?';
                long n = kind == 2 ? readkey(0) : read(0, &byte, 1);
                int ok = action == 0 ? (n == -4 && wait_interrupts == 1) :
                         action == 1 && !wait_interrupts &&
                         (kind == 2 ? n == 'a' : n == 1 && byte == 'a');
                printf("keyevtest: wait child kind=%d action=%d result=%ld signals=%d %s\n",
                       kind, action, n, wait_interrupts, ok ? "PASS" : "FAIL");
                exit(ok ? 0 : 4);
            }
            close(ready[1]);
            char byte;
            int ok = read(ready[0], &byte, 1) == 1 && byte == 'R';
            close(ready[0]);
            if (setfg(child)) ok = 0;
            usleep(200000);  /* child enters its stdin wait before the signal */
            if (action == 0) {
                if (kill(child, SIGUSR1)) ok = 0;
            } else {
                if (kill(child, SIGSTOP)) ok = 0;
                long stopped = wait_collect(child, &status, WUNTRACED);
                if (stopped != child || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) ok = 0;
                if (kill(child, action == 1 ? SIGCONT : SIGKILL)) ok = 0;
                if (action == 1) printf("keyevtest: wait kind=%d resumed; type a\n", kind);
            }
            long reaped = wait_collect(child, &status, 0);
            if (reaped != child || status != (action == 2 ? 137 : 0)) ok = 0;
            if (!reaped) {
                kill(child, SIGKILL);
                if (wait_collect(child, &status, 0) != child) return 1;
            }
            if (setfg((long)getpid())) ok = 0;
            printf("keyevtest: wait kind=%d action=%d %s\n", kind, action, ok ? "PASS" : "FAIL");
            if (!ok) failed++;
        }
    }
    printf("keyevtest: wait returns %s (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}

static int raw_read_test(int ordered) {
    const int capacities[] = {1, 2, 3, 4, 16};
    const char* expected[] = {"\033[Az", "\033[Bz", "\033[Cz", "\033[Daz", "\033[A\033[B\033[C\033[D\033[H\033[Fz"};
    const char* mixed[] = {"a\033[Az", "a\033[Bz", "a\033[Cz", "a\033[Dz",
                           "a\033[Ab\033[Bc\033[Cd\033[De\033[Hf\033[Fz"};
    if (ordered) for (int i = 0; i < 5; i++) expected[i] = mixed[i];
    long previous = ttymode(TTY_RAW);
    if (previous < 0) return 1;
    int failed = 0;
    for (int test = 0; test < 5; test++) {
        printf("keyevtest: raw capacity %d ready\n", capacities[test]);
        usleep(6000000);  /* allow the documented key sequence to be buffered */
        char result[32];
        int used = 0, want = (int)strlen(expected[test]);
        while (used < want) {
            struct pollfd input = {0, POLLIN, 0};
            if (poll(&input, 1, 1000) != 1 || !(input.revents & POLLIN)) break;
            char chunk[16];
            long n = read(0, chunk, capacities[test]);
            if (n <= 0 || n > (long)sizeof(chunk) || used + n > (long)sizeof(result)) break;
            memcpy(result + used, chunk, n);
            used += (int)n;
        }
        int ok = used == want && !memcmp(result, expected[test], want);
        printf("keyevtest: raw capacity %d %s (%d/%d bytes)\n",
               capacities[test], ok ? "PASS" : "FAIL", used, want);
        if (!ok) {
            for (int i = 0; i < used; i++) printf(" %02x", (unsigned char)result[i]);
            printf("\n"); failed++; break;
        }
    }
    ttymode((int)previous);
    printf("keyevtest: raw reads %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--wait-return")) return wait_return_test();
    if (argc == 2 && !strcmp(argv[1], "--key-order")) return raw_read_test(1);
    if (argc == 2 && !strcmp(argv[1], "--raw-reads")) return raw_read_test(0);
    if (argc == 2 && !strcmp(argv[1], "--foreground")) return foreground_test();
    if (argc == 2 && !strcmp(argv[1], "--foreground-signal")) return foreground_signal_test();
    int p, c;
    /* Drain events buffered while the command line itself was typed, so the report
     * below only shows keys pressed during the polling window. */
    while (getkeyevent(&p, &c)) { }

    printf("keyevtest: polling raw key events for ~4s — press keys now\n");
    int count = 0;
    for (int i = 0; i < 800; i++) {
        while (getkeyevent(&p, &c)) {
            printf("keyevtest: EVENT pressed=%d code=0x%x\n", p, c);
            count++;
        }
        usleep(5000);   /* 5 ms between polls */
    }
    printf("keyevtest: done (%d events)\n", count);
    return 0;
}
