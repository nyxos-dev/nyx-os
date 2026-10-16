#include "libc.h"

/* Concatenate bytes without adding a final newline. Numbering is one continuous
 * stream across files/stdin; -b numbers nonempty lines and overrides -n. */
#define CAT_EINTR (-4)                   /* raw NyxOS syscall return, no errno */
#define CAT_READ_ERROR 1
#define CAT_WRITE_ERROR 2

typedef struct {
    int number, nonblank, line_start;
    unsigned long next_line;
} cat_state;

static int write_all(const char* data, long size) {
    while (size > 0) {
        long written = write(1, data, size);
        if (written == CAT_EINTR) continue;
        if (written <= 0 || written > size) return -1;
        data += written;
        size -= written;
    }
    return 0;
}

static int cat_fd(int fd, cat_state* state) {
    char buf[512];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n == CAT_EINTR) continue;
        if (n < 0) return CAT_READ_ERROR;
        if (n == 0) return 0;
        if (!state->number) {
            if (write_all(buf, n) < 0) return CAT_WRITE_ERROR;
            continue;
        }
        for (long i = 0; i < n;) {
            if (state->line_start && (!state->nonblank || buf[i] != '\n')) {
                char prefix[32];
                int length = snprintf(prefix, sizeof(prefix), "%6lu\t", state->next_line++);
                if (write_all(prefix, length) < 0) return CAT_WRITE_ERROR;
            }
            long start = i;
            while (i < n && buf[i++] != '\n');
            if (write_all(buf + start, i - start) < 0) return CAT_WRITE_ERROR;
            state->line_start = buf[i - 1] == '\n';
        }
    }
}

int main(int argc, char** argv) {
    cat_state state = { 0, 0, 1, 1 };
    int operands = 0, options = 1;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (options && !strcmp(arg, "--")) { options = 0; continue; }
        if (options && arg[0] == '-' && arg[1]) {
            for (const char* flag = arg + 1; *flag; flag++) {
                if (*flag == 'n') state.number = 1;
                else if (*flag == 'b') state.number = state.nonblank = 1;
                else { fprintf(stderr, "cat: invalid option -%c\n", *flag); return 1; }
            }
        } else argv[++operands] = argv[i];
    }

    int rc = 0;
    for (int i = 0; i < (operands ? operands : 1); i++) {
        const char* name = operands ? argv[i + 1] : "-";
        int from_stdin = !strcmp(name, "-");
        long fd = from_stdin ? 0 : open(name, O_RDONLY, 0);
        if (fd < 0) { fprintf(stderr, "cat: %s: cannot open\n", name); rc = 1; continue; }
        int result = cat_fd((int)fd, &state);
        if (!from_stdin && close((int)fd) < 0) {
            fprintf(stderr, "cat: %s: close error\n", name); rc = 1;
        }
        if (result == CAT_WRITE_ERROR) { fprintf(stderr, "cat: write error\n"); return 1; }
        if (result == CAT_READ_ERROR) { fprintf(stderr, "cat: %s: read error\n", name); rc = 1; }
    }
    return rc;
}
