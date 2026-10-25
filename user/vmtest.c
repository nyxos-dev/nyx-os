#include "libc.h"

/* vmtest — prove the user/kernel address boundary actually holds.
 *
 * Before this test's fix, user_v2p() walked the user PML4 with no address bound.
 * Every user PML4 mirrors kernel_pml4[511], which is the identity map of all
 * physical RAM, and KERNEL_BASE is exactly -2^39 — so KERNEL_BASE + X resolved
 * to physical X. Any syscall that wrote through an unvalidated user pointer was
 * therefore an arbitrary write into kernel memory, from an ordinary ring-3
 * program, with no privileges required.
 *
 * sigprocmask's `oldset` pointer was one such path, so it is what we aim here.
 *
 * A test that only checked "the kernel still works" would pass on a vulnerable
 * build. This one checks both directions: the attack must be REFUSED, and a
 * legitimate pointer must still be ACCEPTED. Either half alone proves nothing —
 * a kernel that rejected every pointer would pass the first and fail the second.
 */

#define KERNEL_BASE 0xFFFFFF8000000000UL

static int fail = 0;

/* No width specifier: this libc's printf does not implement one (it prints the
 * "%-46s" literally), which this very test surfaced. */
static void check(const char* what, int ok) {
    printf("vmtest: [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) fail = 1;
}

static unsigned long free_kib(void) {
    char info[512];
    long fd = open("/proc/meminfo", 0, 0);
    if (fd < 0) return 0;
    long n = read((int)fd, info, sizeof(info) - 1);
    close((int)fd);
    if (n <= 0) return 0;
    info[n] = '\0';
    char* field = strstr(info, "MemFree:");
    return field ? strtoul(field + 8, 0, 10) : 0;
}

static int span_test(void) {
    const unsigned long bytes = 600UL * 1024 * 1024, pages = bytes / 4096;
    unsigned long before = free_kib();
    if (before < 640UL * 1024) {
        printf("vmtest: --span requires at least 640 MiB free managed RAM\n");
        return 1;
    }
    unsigned char* base = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { printf("vmtest: span mmap failed\n"); return 1; }
    int ok = 1;
    for (unsigned long i = 0; i < pages; i++) {
        volatile unsigned long* p = (volatile unsigned long*)(base + i * 4096);
        if (p[0] || p[511]) ok = 0;
        p[0] = i ^ 0x4e59585350414eUL;
        p[511] = ~p[0];
    }
    check("600 MiB demand-zero pages materialised", ok);
    unsigned long occupied = free_kib();
    check("managed free RAM decreased by at least 600 MiB", before >= occupied + bytes / 1024);
    long child = fork();
    if (!child) {
        const unsigned long probes[] = {0, pages / 2, pages - 1};
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            volatile unsigned long* p = (volatile unsigned long*)(base + probes[i] * 4096);
            if (p[0] != (probes[i] ^ 0x4e59585350414eUL) || p[511] != ~p[0]) exit(2);
            p[0] ^= 0x1234; p[511] = ~p[0];
        }
        exit(0);
    }
    int status = -1;
    check("large COW child reaped", child > 0 && waitpid((int)child, &status) == child && status == 0);
    ok = 1;
    for (unsigned long i = 0; i < pages; i++) {
        volatile unsigned long* p = (volatile unsigned long*)(base + i * 4096);
        unsigned long expected = i ^ 0x4e59585350414eUL;
        if (p[0] != expected || p[511] != ~expected) ok = 0;
    }
    check("all parent page markers preserved after COW", ok);
    check("600 MiB mapping unmapped", munmap(base, bytes) == 0);
    unsigned long after = free_kib();
    check("mapped page storage recovered", after >= occupied + bytes / 1024);
    printf("vmtest: span free KiB %lu -> %lu -> %lu (page tables reclaimed at exit)\n", before, occupied, after);
    printf("vmtest: span %s\n", fail ? "FAIL" : "PASS");
    return fail;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--span")) return span_test();
    if (argc != 1) { printf("Usage: vmtest [--span]\n"); return 1; }
    unsigned long legit = 0;

    /* Baseline: a real pointer into our own stack must work. If this fails the
     * guard is too strict and the other results below are meaningless. */
    long r = sigprocmask(0 /* SIG_BLOCK */, 0, &legit);
    check("legitimate oldset pointer accepted", r == 0);

    /* The attack. Aim oldset at the higher-half alias of low physical memory —
     * the kernel image lives at physical 0x100000. On a vulnerable kernel this
     * returns 0 and silently writes 8 bytes over kernel memory. */
    unsigned long* attack = (unsigned long*)(KERNEL_BASE + 0x100000);
    r = sigprocmask(0, 0, attack);
    check("higher-half kernel pointer REFUSED", r != 0);

    /* Same idea, aimed at the very start of physical RAM. */
    r = sigprocmask(0, 0, (unsigned long*)KERNEL_BASE);
    check("physical-zero alias REFUSED", r != 0);

    /* A non-canonical address must not translate either. */
    r = sigprocmask(0, 0, (unsigned long*)0x0000800000000000UL);
    check("first non-canonical address REFUSED", r != 0);

    /* The NULL page stays unmapped for user space. */
    r = sigprocmask(0, 0, (unsigned long*)0x10);
    check("null-page pointer REFUSED", r != 0);

    printf("vmtest: %s\n", fail ? "BOUNDARY BROKEN" : "USER/KERNEL BOUNDARY HOLDS");
    return fail;
}
