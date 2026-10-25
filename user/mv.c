#include "libc.h"

int main(int argc, char** argv) {
    if (argc != 3) { printf("usage: mv SRC DST\n"); return 1; }
    if (rename(argv[1], argv[2]) < 0) {
        printf("mv: failed to move %s to %s (destination may exist)\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
