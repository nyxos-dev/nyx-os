#include "libc.h"

/* seq — print a sequence of integers, the classic range generator.
 *
 *   seq LAST                1 2 ... LAST                 (step 1)
 *   seq FIRST LAST          FIRST ... LAST               (step 1)
 *   seq FIRST STEP LAST     FIRST, FIRST+STEP, ...       (ascends or descends; stops at LAST)
 *
 *   -s SEP   put SEP between numbers instead of a newline (a trailing newline still prints)
 *   -w       pad each number with leading zeros to the width of the widest endpoint
 *
 * Integer-only (the libc has no float parse); STEP 0 is rejected, and a range that can
 * never reach LAST just prints the trailing newline. Clean-room; installs via
 * `xbm install seq`.
 *
 *   seq 5           -> 1 2 3 4 5      (newline-separated)
 *   seq 2 2 10      -> 2 4 6 8 10
 *   seq -w 8 10     -> 08 09 10
 *   seq -s , 1 4    -> 1,2,3,4
 */

/* Parse a plain decimal integer; *ok is 0 if the whole token isn't one. */
static long pnum(const char* s, int* ok) {
    char* end = 0;
    long v = strtol(s, &end, 10);
    *ok = (end != s && *end == '\0');
    return v;
}

/* Print v with leading zeros to `width` printed columns (a '-' sign stays in front). */
static void print_padded(long v, int width) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", v);
    const char* digits = buf;
    int len = (int)strlen(buf);
    if (buf[0] == '-') { putchar('-'); digits = buf + 1; len--; width--; }
    for (int k = len; k < width; k++) putchar('0');
    printf("%s", digits);
}

int main(int argc, char** argv) {
    const char* sep = "\n";
    int wflag = 0;
    long nums[3];
    int nn = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)      sep = argv[++i];
        else if (strcmp(argv[i], "-w") == 0)                 wflag = 1;
        else if (nn < 3) {
            int ok;
            nums[nn] = pnum(argv[i], &ok);
            if (!ok) { fprintf(stderr, "seq: '%s' is not an integer\n", argv[i]); return 1; }
            nn++;
        }
    }
    if (nn == 0) { fprintf(stderr, "usage: seq [-w] [-s SEP] [FIRST [STEP]] LAST\n"); return 1; }

    long first, step, last;
    if (nn == 1)      { first = 1;       step = 1;       last = nums[0]; }
    else if (nn == 2) { first = nums[0]; step = 1;       last = nums[1]; }
    else              { first = nums[0]; step = nums[1]; last = nums[2]; }

    if (step == 0) { fprintf(stderr, "seq: step cannot be zero\n"); return 1; }

    int width = 0;
    if (wflag) {
        char a[24], b[24];
        snprintf(a, sizeof(a), "%ld", first);
        snprintf(b, sizeof(b), "%ld", last);
        int la = (int)strlen(a), lb = (int)strlen(b);
        width = la > lb ? la : lb;
    }

    int first_out = 1;
    for (long v = first; (step > 0) ? (v <= last) : (v >= last); v += step) {
        if (!first_out) printf("%s", sep);
        first_out = 0;
        if (wflag) print_padded(v, width);
        else       printf("%ld", v);
    }
    putchar('\n');
    return 0;
}
