#include "libc.h"

/* NyxOS toolchain self-test (Phase 2 — in-OS compiler groundwork).
 * Exercises the libc primitives that a C compiler (and most nontrivial
 * programs) need. Prints one PASS/FAIL line per function to stdout, which the
 * kernel routes to the serial console at boot. This file grows as the libc
 * grows toward hosting the compiler. */
static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static int move_bytes(const char* path, const char* expected) {
    char buf[16];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = read(fd, buf, sizeof(buf));
    int closed = close(fd);
    return closed == 0 && n == (long)strlen(expected) && !memcmp(buf, expected, n);
}

static int test_moves(void) {
    char dir[64], a[80], b[80];
    snprintf(dir, sizeof(dir), "/mnt/libctest-move-%d", (int)getpid());
    if (mkdir(dir, 0755) < 0) return 0;
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
    const char* paths[] = {a, b};
    const char* data[] = {"KEEP", "STAY"};
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { ok = 0; break; }
        long written = write(fd, data[i], 4);
        if (close(fd) < 0 || written != 4) { ok = 0; break; }
    }
    if (ok) ok = rename(a, a) == 0 && rename(a, b) < 0 &&
                 move_bytes(a, "KEEP") && move_bytes(b, "STAY");
    if (ok) ok = unlink(b) == 0;
    for (int i = 0; ok && i < 16; i++) {
        struct stat st;
        ok = rename(a, b) == 0 && stat(a, &st) < 0 && move_bytes(b, "KEEP") &&
             rename(b, a) == 0 && stat(b, &st) < 0 && move_bytes(a, "KEEP");
    }
    unlink(a);
    unlink(b);
    if (unlink(dir) < 0) ok = 0;
    return ok;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    int ok = 1;

    /* realloc: grow a buffer; old contents preserved, grown region writable. */
    char* p = (char*)malloc(8);
    for (int i = 0; i < 8; i++) p[i] = (char)('A' + i);
    p = (char*)realloc(p, 64);
    int r_ok = (p != 0);
    for (int i = 0; i < 8; i++) if (p[i] != (char)('A' + i)) r_ok = 0;
    if (p) p[63] = 'Z';
    printf("LIBCTEST: realloc %s\n", r_ok ? "PASS" : "FAIL");
    if (!r_ok) ok = 0;
    free(p);

    /* calloc: zero-filled allocation. */
    int* z = (int*)calloc(16, sizeof(int));
    int c_ok = (z != 0);
    if (z) for (int i = 0; i < 16; i++) if (z[i] != 0) c_ok = 0;
    printf("LIBCTEST: calloc %s\n", c_ok ? "PASS" : "FAIL");
    if (!c_ok) ok = 0;
    free(z);

    /* memmove: overlapping forward shift (a plain memcpy would corrupt this). */
    char buf[16];
    for (int i = 0; i < 16; i++) buf[i] = (char)('0' + i);
    memmove(buf + 2, buf, 10);
    int m_ok = 1;
    for (int i = 0; i < 10; i++) if (buf[i + 2] != (char)('0' + i)) m_ok = 0;
    printf("LIBCTEST: memmove %s\n", m_ok ? "PASS" : "FAIL");
    if (!m_ok) ok = 0;

    /* File I/O wrappers (open/write/stat/lseek/read) on the persistent /mnt mount:
     * write a file, stat it (size + regular-file mode), then reopen, seek past the
     * first 7 bytes and read the tail back, comparing it to the original. */
    {
        const char* path = "/mnt/libctest_io.txt";
        const char* msg  = "NyxOS toolchain file I/O test\n";
        long mlen = (long)strlen(msg);
        int io_ok = 1;
        int fd = (int)open(path, O_CREAT | O_TRUNC, 0644);   /* NyxOS: bit0=O_CREAT, no access mode */
        if (fd < 0) io_ok = 0;
        else { if (write(fd, msg, mlen) != mlen) io_ok = 0; close(fd); }

        struct stat st;
        if (io_ok && (stat(path, &st) != 0 || st.st_size != (unsigned)mlen || !S_ISREG(st.st_mode))) io_ok = 0;

        fd = (int)open(path, O_RDONLY, 0);
        if (fd < 0) io_ok = 0;
        else {
            char rb[64];
            if (lseek(fd, 7, SEEK_SET) != 7) io_ok = 0;
            long n = read(fd, rb, (long)sizeof(rb) - 1);
            if (n < 0) { io_ok = 0; n = 0; }
            rb[n] = '\0';
            if (io_ok && (n != mlen - 7 || memcmp(rb, msg + 7, (unsigned long)n) != 0)) io_ok = 0;
            close(fd);
        }
        printf("LIBCTEST: fileio(open/write/stat/lseek/read) %s\n", io_ok ? "PASS" : "FAIL");
        if (!io_ok) ok = 0;
    }

    {
        int move_ok = test_moves();
        printf("LIBCTEST: rename(no-op/collision/32 moves) %s\n", move_ok ? "PASS" : "FAIL");
        if (!move_ok) ok = 0;
    }

    /* ctype (character classification/conversion) — a lexer's staples. */
    {
        int ct_ok = isalpha('A') && isalpha('z') && !isalpha('7') &&
                    isdigit('0') && isdigit('9') && !isdigit('x') &&
                    isspace(' ') && isspace('\n') && !isspace('a') &&
                    isxdigit('f') && isxdigit('C') && !isxdigit('g') &&
                    isalnum('q') && isalnum('5') && !isalnum('#') &&
                    ispunct('#') && !ispunct('a') &&
                    toupper('a') == 'A' && tolower('Z') == 'z' && toupper('7') == '7';
        printf("LIBCTEST: ctype %s\n", ct_ok ? "PASS" : "FAIL");
        if (!ct_ok) ok = 0;
    }

    /* strtol / strtoul — string to integer, with sign/base/prefix handling. */
    {
        char* end = 0;
        long a = strtol("  -42xyz", &end, 10);           /* signed decimal, stops at 'x' */
        int s_ok = (a == -42) && end && (*end == 'x');
        s_ok = s_ok && (strtoul("0x1A", 0, 0) == 26);    /* base 0 auto-detects hex */
        s_ok = s_ok && (strtol("777", 0, 8) == 511);     /* explicit octal */
        /* overflow saturates like C, instead of wrapping (pre-v6.5.102 bug) */
        s_ok = s_ok && (strtol("99999999999999999999", 0, 10) == 0x7FFFFFFFFFFFFFFFL);
        s_ok = s_ok && (strtol("-99999999999999999999", 0, 10) == (-0x7FFFFFFFFFFFFFFFL - 1L));
        s_ok = s_ok && (strtoul("99999999999999999999999", 0, 10) == 0xFFFFFFFFFFFFFFFFUL);
        printf("LIBCTEST: strtol/strtoul %s\n", s_ok ? "PASS" : "FAIL");
        if (!s_ok) ok = 0;
    }

    /* atoi — must behave as (int)strtol(s,NULL,10): skip ALL whitespace (not
     * just ' ') and clamp on overflow (pre-v6.5.103 skipped only ' ' + wrapped). */
    {
        int a_ok = (atoi("42") == 42) && (atoi("-42") == -42) && (atoi("+42") == 42);
        a_ok = a_ok && (atoi("\t42") == 42) && (atoi("\n42") == 42);   /* tab/newline */
        a_ok = a_ok && (atoi(" \t\n\v\f\r 42") == 42);                 /* mixed leading ws */
        a_ok = a_ok && (atoi("12abc") == 12) && (atoi("abc") == 0);
        a_ok = a_ok && (atoi("9999999999999999999999") == -1);         /* >LONG_MAX -> clamp+trunc */
        printf("LIBCTEST: atoi %s\n", a_ok ? "PASS" : "FAIL");
        if (!a_ok) ok = 0;
    }

    /* sscanf — subset: %d/%u/%x/%i/%c/%s/%n/%%/width/'*'/length, matching C sscanf
     * (verified byte-exact vs host glibc across 31 cases; new in v6.5.104). */
    {
        int sc_ok = 1;
        int a, b, c2;
        if (sscanf("42", "%d", &a) != 1 || a != 42) sc_ok = 0;
        if (sscanf("  -7xyz", "%d", &a) != 1 || a != -7) sc_ok = 0;
        if (sscanf("abc", "%d", &a) > 0) sc_ok = 0;               /* matching failure: %d must not convert (> 0, not != 0 which conflates EOF) */
        if (sscanf("", "%d", &a) != -1) sc_ok = 0;                 /* EOF before first */
        if (sscanf("12:34:56", "%d:%d:%d", &a, &b, &c2) != 3 || a != 12 || b != 34 || c2 != 56) sc_ok = 0;
        unsigned u;
        if (sscanf("0x1A", "%x", &u) != 1 || u != 0x1A) sc_ok = 0;
        if (sscanf("010", "%i", &a) != 1 || a != 8) sc_ok = 0;     /* base-0 octal */
        long lv;
        if (sscanf("9999999999", "%ld", &lv) != 1 || lv != 9999999999L) sc_ok = 0;
        if (sscanf("12345", "%3d", &a) != 1 || a != 123) sc_ok = 0; /* width */
        if (sscanf("10 20", "%*d %d", &a) != 1 || a != 20) sc_ok = 0; /* suppression */
        char buf1[32], buf2[32];
        if (sscanf("  foo bar", "%s %s", buf1, buf2) != 2 || strcmp(buf1, "foo") || strcmp(buf2, "bar")) sc_ok = 0;
        if (sscanf("hello", "%3s", buf1) != 1 || strcmp(buf1, "hel")) sc_ok = 0;
        char ch = 0; int n = 0;
        if (sscanf("Q", "%c", &ch) != 1 || ch != 'Q') sc_ok = 0;
        if (sscanf("123abc", "%d%n", &a, &n) != 1 || a != 123 || n != 3) sc_ok = 0;
        if (sscanf("val=42", "val=%d", &a) != 1 || a != 42) sc_ok = 0; /* literal prefix */
        if (sscanf("50% off", "%d%% off", &a) != 1 || a != 50) sc_ok = 0; /* %% */
        printf("LIBCTEST: sscanf %s\n", sc_ok ? "PASS" : "FAIL");
        if (!sc_ok) ok = 0;
    }

    /* strtod / atof — correctly rounded for the common range; the fast path returns
     * the same double the compiler makes for the literal, so == holds (new v6.5.105).
     * Also exercises the freshly-wired sscanf %f/%lf. */
    {
        int d_ok = 1;
        char* e = 0;
        if (strtod("3.14", &e) != 3.14 || *e != '\0') d_ok = 0;
        if (strtod("0.5", 0) != 0.5) d_ok = 0;
        if (strtod("2.5e3", 0) != 2500.0) d_ok = 0;
        if (strtod("-0.001", 0) != -0.001) d_ok = 0;
        if (strtod("1e10", 0) != 1e10) d_ok = 0;
        if (strtod("123.456", 0) != 123.456) d_ok = 0;
        if (strtod("42", 0) != 42.0) d_ok = 0;
        if (strtod("  2.5xyz", &e) != 2.5 || *e != 'x') d_ok = 0;   /* leading ws + endptr */
        if (atof("6.25") != 6.25) d_ok = 0;
        float fv = 0; double dv = 0;
        if (sscanf("3.14", "%f", &fv) != 1 || fv != 3.14f) d_ok = 0;
        if (sscanf("2.5e3", "%lf", &dv) != 1 || dv != 2500.0) d_ok = 0;
        printf("LIBCTEST: strtod/atof %s\n", d_ok ? "PASS" : "FAIL");
        if (!d_ok) ok = 0;
    }

    /* setenv/unsetenv/putenv — POSIX environment modification (new in v6.5.110). */
    {
        int ev_ok = 1;
        if (setenv("NYXTEST", "one", 1) != 0 || strcmp(getenv("NYXTEST"), "one")) ev_ok = 0;
        if (setenv("NYXTEST", "two", 0) != 0 || strcmp(getenv("NYXTEST"), "one")) ev_ok = 0; /* no overwrite */
        if (setenv("NYXTEST", "two", 1) != 0 || strcmp(getenv("NYXTEST"), "two")) ev_ok = 0; /* overwrite */
        if (setenv("BAD=NAME", "x", 1) != -1) ev_ok = 0;                                     /* '=' rejected */
        if (unsetenv("NYXTEST") != 0 || getenv("NYXTEST") != 0) ev_ok = 0;                   /* removed */
        if (putenv((char*)"NYXPUT=yes") != 0 || strcmp(getenv("NYXPUT"), "yes")) ev_ok = 0;
        if (putenv((char*)"NYXPUT=no") != 0 || strcmp(getenv("NYXPUT"), "no")) ev_ok = 0;    /* replace */
        printf("LIBCTEST: setenv/unsetenv/putenv %s\n", ev_ok ? "PASS" : "FAIL");
        if (!ev_ok) ok = 0;
    }

    /* gmtime/localtime/strftime — calendar time (new v6.5.111; UTC). Verified byte-exact
     * vs glibc gmtime_r/strftime in host-sim; these anchors pin known epochs. */
    {
        int tm_ok = 1;
        char tb[64];
        time_t t0 = 0;                               /* 1970-01-01 00:00:00 Thu */
        struct tm r;
        gmtime_r(&t0, &r);
        if (r.tm_year != 70 || r.tm_mon != 0 || r.tm_mday != 1 || r.tm_hour != 0 ||
            r.tm_min != 0 || r.tm_sec != 0 || r.tm_wday != 4 || r.tm_yday != 0) tm_ok = 0;
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &r);
        if (strcmp(tb, "1970-01-01 00:00:00")) tm_ok = 0;
        time_t t1 = 1234567890;                       /* 2009-02-13 23:31:30 Fri */
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S %A", gmtime(&t1));
        if (strcmp(tb, "2009-02-13 23:31:30 Friday")) tm_ok = 0;
        time_t t2 = 1709164800;                       /* 2024-02-29 (leap day) */
        strftime(tb, sizeof tb, "%F %a", gmtime(&t2));
        if (strcmp(tb, "2024-02-29 Thu")) tm_ok = 0;
        strftime(tb, sizeof tb, "%I:%M %p", gmtime(&t1));   /* 12h clock */
        if (strcmp(tb, "11:31 PM")) tm_ok = 0;
        printf("LIBCTEST: gmtime/strftime %s\n", tm_ok ? "PASS" : "FAIL");
        if (!tm_ok) ok = 0;
    }

    /* mktime/timegm/difftime — the inverse of gmtime, with field normalization (new
     * v6.5.113; UTC, so mktime == timegm). Verified 20/20 vs glibc timegm in host-sim. */
    {
        int mk_ok = 1;
        struct tm r;
        /* round-trip: epoch -> gmtime -> mktime -> epoch, across several anchors */
        time_t anchors[] = { 0, 1234567890, 1709164800, -1, 951825600 /*2000 leap-day*/ };
        for (unsigned i = 0; i < sizeof(anchors)/sizeof(anchors[0]); i++) {
            gmtime_r(&anchors[i], &r);
            if (mktime(&r) != anchors[i]) mk_ok = 0;
        }
        /* known build: 2009-02-13 23:31:30 UTC == 1234567890 */
        { struct tm t; memset(&t, 0, sizeof t); t.tm_year = 109; t.tm_mon = 1;
          t.tm_mday = 13; t.tm_hour = 23; t.tm_min = 31; t.tm_sec = 30;
          if (timegm(&t) != 1234567890) mk_ok = 0; }
        /* normalization: 2020-01-32 -> 2020-02-01, and the struct is rewritten in place */
        { struct tm t; memset(&t, 0, sizeof t); t.tm_year = 120; t.tm_mon = 0; t.tm_mday = 32;
          time_t e = mktime(&t);
          struct tm f; memset(&f, 0, sizeof f); f.tm_year = 120; f.tm_mon = 1; f.tm_mday = 1;
          if (e != timegm(&f) || t.tm_mon != 1 || t.tm_mday != 1) mk_ok = 0; }
        /* month overflow: mon 12 (a 13th month) carries into the next year's January */
        { struct tm t; memset(&t, 0, sizeof t); t.tm_year = 120; t.tm_mon = 12; t.tm_mday = 1;
          mktime(&t);
          if (t.tm_year != 121 || t.tm_mon != 0) mk_ok = 0; }
        if (difftime(1234567890, 1234567800) != 90.0) mk_ok = 0;
        printf("LIBCTEST: mktime/timegm/difftime %s\n", mk_ok ? "PASS" : "FAIL");
        if (!mk_ok) ok = 0;
    }

    /* strptime — parse broken-down time (new v6.5.118); paired with timegm to a known epoch.
     * Verified field/offset-exact vs glibc strptime in host-sim. */
    {
        int sp_ok = 1;
        struct tm t; char* r;
        memset(&t, 0, sizeof t);
        r = strptime("2009-02-13 23:31:30", "%Y-%m-%d %H:%M:%S", &t);
        if (!r || *r != '\0') sp_ok = 0;
        if (t.tm_year != 109 || t.tm_mon != 1 || t.tm_mday != 13 ||
            t.tm_hour != 23 || t.tm_min != 31 || t.tm_sec != 30) sp_ok = 0;
        if (timegm(&t) != 1234567890) sp_ok = 0;                 /* strptime -> timegm round-trip */
        memset(&t, 0, sizeof t);
        r = strptime("Feb 13, 2009", "%b %d, %Y", &t);
        if (!r || t.tm_mon != 1 || t.tm_mday != 13 || t.tm_year != 109) sp_ok = 0;
        memset(&t, 0, sizeof t);
        r = strptime("11:31 PM", "%I:%M %p", &t);
        if (!r || t.tm_hour != 23 || t.tm_min != 31) sp_ok = 0;
        memset(&t, 0, sizeof t);
        r = strptime("2026-09-05rest", "%Y-%m-%d", &t);          /* returns past the parsed date */
        if (!r || strcmp(r, "rest") != 0) sp_ok = 0;
        if (strptime("nope", "%Y", &t) != 0) sp_ok = 0;          /* mismatch -> NULL */
        {   /* strftime -> strptime -> timegm round-trip */
            char buf[32]; time_t e = 1700000000; struct tm g, h;
            strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", gmtime_r(&e, &g));
            memset(&h, 0, sizeof h);
            if (!strptime(buf, "%Y-%m-%d %H:%M:%S", &h) || timegm(&h) != e) sp_ok = 0;
        }
        printf("LIBCTEST: strptime %s\n", sp_ok ? "PASS" : "FAIL");
        if (!sp_ok) ok = 0;
    }

    /* string/stdlib extras: memchr, strrchr, strncat, strdup, qsort. */
    {
        int se_ok = 1;
        const char* hay = "abc/def/ghi";
        if (strrchr(hay, '/') != hay + 7) se_ok = 0;        /* LAST '/' */
        if (memchr(hay, 'd', 11) != hay + 4) se_ok = 0;     /* first 'd' */
        if (memchr(hay, 'z', 11) != 0) se_ok = 0;           /* absent */

        char nb[16]; nb[0] = '\0';
        strncat(nb, "foo", 16);
        strncat(nb, "bar", 2);                              /* bounded: only "ba" */
        if (strcmp(nb, "fooba") != 0) se_ok = 0;

        char* dup = strdup("duplicate me");
        if (!dup || strcmp(dup, "duplicate me") != 0) se_ok = 0;
        free(dup);

        int arr[8] = { 5, 2, 9, 1, 7, 3, 8, 4 };
        qsort(arr, 8, sizeof(int), cmp_int);
        for (int i = 0; i < 7; i++) if (arr[i] > arr[i + 1]) se_ok = 0;
        if (arr[0] != 1 || arr[7] != 9) se_ok = 0;

        printf("LIBCTEST: str/stdlib(memchr/strrchr/strncat/strdup/qsort) %s\n", se_ok ? "PASS" : "FAIL");
        if (!se_ok) ok = 0;
    }

    /* FILE* stdio core: write a file with fwrite+fputc, then read it back with
     * fread+fgetc and confirm the bytes + feof at end (separate FILEs, no r+). */
    {
        const char* path = "/mnt/libctest_stdio.txt";
        const char* body = "line one\nline two\n";   /* 18 bytes */
        int st_ok = 1;
        FILE* fw = fopen(path, "w");
        if (!fw) st_ok = 0;
        else {
            size_t wl = strlen(body);
            if (fwrite(body, 1, wl, fw) != wl) st_ok = 0;   /* bulk write */
            if (fputc('!', fw) != '!') st_ok = 0;            /* one trailing byte */
            if (fclose(fw) != 0) st_ok = 0;
        }
        FILE* fr = fopen(path, "r");
        if (!fr) st_ok = 0;
        else {
            char rb[64];
            size_t got = fread(rb, 1, sizeof(rb) - 1, fr);   /* reads through EOF */
            rb[got] = '\0';
            size_t expect = strlen(body) + 1;                /* body + '!' */
            if (got != expect) st_ok = 0;
            if (st_ok && (strncmp(rb, body, strlen(body)) != 0 || rb[strlen(body)] != '!')) st_ok = 0;
            if (st_ok && !feof(fr)) st_ok = 0;               /* fread reached EOF */
            if (fgetc(fr) != EOF) st_ok = 0;                 /* nothing left */
            fclose(fr);
        }
        printf("LIBCTEST: stdio(fopen/fwrite/fputc/fread/fgetc/feof) %s\n", st_ok ? "PASS" : "FAIL");
        if (!st_ok) ok = 0;
    }

    /* printf family: snprintf correctness + width/flags, bounded truncation with the
     * C99 would-be return, and fprintf to a FILE round-tripped through the disk. */
    {
        int pf_ok = 1;
        char b[32];
        int n = snprintf(b, sizeof(b), "%d/%s/%x/%c/%05d", -7, "hi", 255, 'Z', 42);
        if (strcmp(b, "-7/hi/ff/Z/00042") != 0 || n != 16) pf_ok = 0;
        char t[5];
        int n2 = snprintf(t, sizeof(t), "abcdefgh");     /* truncates to "abcd", returns 8 */
        if (strcmp(t, "abcd") != 0 || n2 != 8) pf_ok = 0;
        /* uppercase hex must be UPPERCASE (regression: %X/%lX once aliased %x/%lx). */
        char x[40];
        int n3 = snprintf(x, sizeof(x), "%X-%08X-%lX", 0xabcu, 0x1a2bu, 0xDEADBEEFCAFEUL);
        if (strcmp(x, "ABC-00001A2B-DEADBEEFCAFE") != 0 || n3 != 25) pf_ok = 0;
        /* 64-bit length modifiers: %z (size_t) / %ll (long long) must not truncate or drop
         * the arg (regression: format_core had no z/t case and only handled a single 'l'). */
        char z[64];
        snprintf(z, sizeof(z), "%zu/%zx/%llu/%lld", (size_t)0x1FFFFFFFFULL,
                 (size_t)0xDEADBEEFCAFEULL, 18446744073709551615ULL, -9000000000LL);
        if (strcmp(z, "8589934591/deadbeefcafe/18446744073709551615/-9000000000") != 0) pf_ok = 0;

        const char* fp = "/mnt/libctest_fprintf.txt";
        FILE* f = fopen(fp, "w");
        if (!f) pf_ok = 0;
        else { fprintf(f, "x=%d y=%s\n", 100, "end"); fclose(f); }
        FILE* fr = fopen(fp, "r");
        if (!fr) pf_ok = 0;
        else {
            char rb[32];
            size_t g = fread(rb, 1, sizeof(rb) - 1, fr);
            rb[g] = '\0';
            if (strcmp(rb, "x=100 y=end\n") != 0) pf_ok = 0;
            fclose(fr);
        }
        printf("LIBCTEST: printf-family(snprintf/vsnprintf/fprintf) %s\n", pf_ok ? "PASS" : "FAIL");
        if (!pf_ok) ok = 0;
    }

    /* stdio standard streams + fgets/fputs/fseek/ftell (v6.1.5). Round-trip a
     * two-line file: fputs to write, fgets to read it back line-by-line, fseek to
     * rewind + seek absolute, ftell to confirm the logical offset at each step.
     * Then verify the predefined stdin/stdout/stderr globals wrap fds 0/1/2 with
     * the right direction, and show fputs/fprintf reach the (unbuffered) stdout. */
    {
        const char* path = "/mnt/libctest_streams.txt";
        const char* l1 = "alpha line\n";
        const char* l2 = "beta line\n";
        int ss_ok = 1;

        FILE* fw = fopen(path, "w");
        if (!fw) ss_ok = 0;
        else {
            if (fputs(l1, fw) < 0 || fputs(l2, fw) < 0) ss_ok = 0;
            if (ftell(fw) != (long)(strlen(l1) + strlen(l2))) ss_ok = 0;   /* write-side offset */
            if (fclose(fw) != 0) ss_ok = 0;
        }

        FILE* fr = fopen(path, "r");
        if (!fr) ss_ok = 0;
        else {
            char line[32];
            if (!fgets(line, sizeof(line), fr) || strcmp(line, l1) != 0) ss_ok = 0;   /* line 1 */
            if (ftell(fr) != (long)strlen(l1)) ss_ok = 0;                             /* offset past line 1 */
            if (!fgets(line, sizeof(line), fr) || strcmp(line, l2) != 0) ss_ok = 0;   /* line 2 */
            if (fgets(line, sizeof(line), fr) != NULL) ss_ok = 0;                     /* EOF -> NULL */

            if (fseek(fr, 0, SEEK_SET) != 0 || ftell(fr) != 0) ss_ok = 0;            /* rewind */
            if (!fgets(line, sizeof(line), fr) || strcmp(line, l1) != 0) ss_ok = 0;   /* re-read line 1 */
            if (fseek(fr, 5, SEEK_SET) != 0 || ftell(fr) != 5) ss_ok = 0;            /* absolute seek */
            fclose(fr);
        }

        /* Predefined standard streams wrap fds 0/1/2 with the correct direction. */
        if (stdin->fd  != 0 || !stdin->can_read  || stdin->can_write)  ss_ok = 0;
        if (stdout->fd != 1 || !stdout->can_write || stdout->can_read) ss_ok = 0;
        if (stderr->fd != 2 || !stderr->can_write)                     ss_ok = 0;

        /* Unbuffered stdout: these reach the serial console immediately (visible proof). */
        fputs("LIBCTEST: fputs->stdout OK\n", stdout);
        fprintf(stdout, "LIBCTEST: fprintf->stdout n=%d OK\n", 42);

        printf("LIBCTEST: streams(std*/fgets/fputs/fseek/ftell) %s\n", ss_ok ? "PASS" : "FAIL");
        if (!ss_ok) ok = 0;
    }

    /* strnlen / stpcpy / strchrnul / memrchr / memmem / strsep (added v6.5.131) */
    {
        int sx_ok = 1;
        if (strnlen("hello", 10) != 5 || strnlen("hello", 3) != 3 || strnlen("", 4) != 0) sx_ok = 0;
        char sb[16]; char* se = stpcpy(sb, "abc");
        if (se != sb + 3 || strcmp(sb, "abc") != 0) sx_ok = 0;
        const char* h = "hello";
        if (strchrnul(h, 'l') != h + 2 || strchrnul(h, 'z') != h + 5) sx_ok = 0;   /* absent -> the NUL */
        static const char m[5] = { 'a','b','a','b','a' };
        if (memrchr(m, 'a', 5) != m + 4 || memrchr(m, 'b', 5) != m + 3 || memrchr(m, 'z', 5) != 0) sx_ok = 0;
        const char* hay = "the quick brown fox";
        if (memmem(hay, 19, "quick", 5) != hay + 4 || memmem(hay, 19, "zzz", 3) != 0 ||
            memmem(hay, 19, "", 0) != hay) sx_ok = 0;                              /* empty needle -> start */
        char buf[16]; strcpy(buf, "a,,b");
        char* sp = buf;
        char* t0 = strsep(&sp, ",");    /* "a" */
        char* t1 = strsep(&sp, ",");    /* ""  (empty field preserved) */
        char* t2 = strsep(&sp, ",");    /* "b" */
        char* t3 = strsep(&sp, ",");    /* NULL */
        if (!t0 || strcmp(t0, "a") || !t1 || t1[0] != '\0' || !t2 || strcmp(t2, "b") || t3 != 0) sx_ok = 0;
        printf("LIBCTEST: strnlen/stpcpy/strchrnul/memrchr/memmem/strsep %s\n", sx_ok ? "PASS" : "FAIL");
        if (!sx_ok) ok = 0;
    }

    printf("LIBCTEST: %s\n", ok ? "ALL PASS" : "SOME FAIL");
    return ok ? 0 : 1;
}
