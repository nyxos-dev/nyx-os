#include "libc.h"

/* keyevtest — exercise the v5.9.30 raw key-event path (DOOM's DG_GetKey enabler).
 * Polls SYS_GETKEYEVENT and prints every make/break. Unlike stdin (cooked, press-
 * only), this reports BOTH press and release, with layout-independent Set-1 scancodes
 * (bit 0x80 = E0-extended arrow/nav). A game reads keys this way. */

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
