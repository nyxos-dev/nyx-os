#include "libc.h"

/* rm — unlink operands; -- permits names beginning with '-'. */
int main(int argc, char** argv) {
    int ai = 1;
    if (ai < argc && strcmp(argv[ai], "--") == 0) ai++;
    else if (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        printf("rm: unsupported option '%s'; use -- before a dash-prefixed filename\n", argv[ai]);
        return 2;
    }
    if (ai >= argc) { printf("usage: rm [--] PATH...\n"); return 1; }
    int rc = 0;
    for (int i = ai; i < argc; i++) {
        if (unlink(argv[i]) != 0) {
            printf("rm: cannot remove '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
