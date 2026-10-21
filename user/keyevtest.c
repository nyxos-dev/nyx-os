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

int main(int argc, char** argv) {
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
