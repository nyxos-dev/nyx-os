#include "libc.h"

/* free — show the managed memory pool from /proc/meminfo. The default unit is
 * KiB; -h selects readable binary units without overflowing at multi-GiB sizes. */

static char buf[256];

/* Find a named counter, so extra fields or a reordered proc file cannot silently
 * become the total/used/free columns. Refuse missing or overflowing values. */
static int mem_value(const char* text, const char* key, unsigned long* value) {
    size_t len = strlen(key);
    for (const char* p = text; *p; ) {
        if (strncmp(p, key, len) == 0 && p[len] == ':') {
            p += len + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p < '0' || *p > '9') return 0;
            unsigned long v = 0;
            while (*p >= '0' && *p <= '9') {
                unsigned long digit = (unsigned long)(*p++ - '0');
                if (v > (~0UL - digit) / 10) return 0;
                v = v * 10 + digit;
            }
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] != 'k' || p[1] != 'B') return 0;
            p += 2;
            while (*p == ' ' || *p == '\t' || *p == '\r') p++;
            if (*p && *p != '\n') return 0;
            *value = v;
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

static void format_amount(char* out, size_t size, unsigned long kb, int unit) {
    if (unit != 'h') {
        unsigned long divisor = unit == 'g' ? 1024UL * 1024 : unit == 'm' ? 1024 : 1;
        snprintf(out, size, "%lu", kb / divisor);
        return;
    }
    static const char* const units[] = {"KiB", "MiB", "GiB", "TiB"};
    unsigned long divisor = 1;
    int index = 0;
    while (index < 3 && kb / divisor >= 1024) { divisor *= 1024; index++; }
    if (index == 0) snprintf(out, size, "%lu%s", kb, units[index]);
    else {
        unsigned long whole = kb / divisor;
        unsigned long tenth = ((kb % divisor) * 10 + divisor / 2) / divisor;
        if (tenth == 10) { whole++; tenth = 0; }
        snprintf(out, size, "%lu.%lu%s", whole, tenth, units[index]);
    }
}

int main(int argc, char** argv) {
    int unit = 'k';
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: free [-h|-k|-m|-g]\n  -h  readable units; -k KiB (default); -m MiB; -g GiB\n");
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2] &&
            (argv[i][1] == 'h' || argv[i][1] == 'k' || argv[i][1] == 'm' || argv[i][1] == 'g'))
            unit = argv[i][1];
        else { printf("free: unknown option '%s' (use --help)\n", argv[i]); return 1; }
    }

    long fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) { printf("free: cannot read /proc/meminfo\n"); return 1; }
    long n = read((int)fd, buf, sizeof(buf) - 1);
    close((int)fd);
    if (n <= 0) { printf("free: cannot read /proc/meminfo\n"); return 1; }
    buf[n] = '\0';

    unsigned long total, used, freeb;
    if (!mem_value(buf, "MemTotal", &total) || !mem_value(buf, "MemUsed", &used) ||
        !mem_value(buf, "MemFree", &freeb) || used > total || freeb > total || used != total - freeb) {
        printf("free: invalid /proc/meminfo\n");
        return 1;
    }

    /* Right-justified columns (the user printf has no left-justify). */
    char totals[32], useds[32], frees[32];
    format_amount(totals, sizeof(totals), total, unit);
    format_amount(useds, sizeof(useds), used, unit);
    format_amount(frees, sizeof(frees), freeb, unit);
    const char* label = unit == 'h' ? "Mem:" : unit == 'g' ? "Mem: (GiB)" :
                        unit == 'm' ? "Mem: (MiB)" : "Mem: (KiB)";
    printf("%12s%12s%12s%12s\n", "", "total", "used", "free");
    printf("%12s%12s%12s%12s\n", label, totals, useds, frees);
    unsigned long detected;
    if (mem_value(buf, "MemDetected", &detected) && detected > total) {
        format_amount(totals, sizeof(totals), total, 'h');
        format_amount(useds, sizeof(useds), detected, 'h');
        printf("RAM detected: %s; managed pool: %s\n", useds, totals);
    }
    return 0;
}
