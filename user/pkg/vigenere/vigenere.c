#include "libc.h"

/* vigenere — the classic Vigenere polyalphabetic cipher, a keyword sibling of `caesar`.
 * Each ASCII letter of the message is shifted forward (encrypt) or backward (decrypt) by
 * the value (A/a=0 .. Z/z=25) of the next KEY letter, wrapping within its own case; digits,
 * punctuation and whitespace pass through untouched and do NOT consume a key letter. The
 * KEY is case-insensitive and any non-letters in it are ignored.
 *
 *   vigenere LEMON "ATTACKATDAWN"     -> LXFOPVEFRNHR
 *   vigenere -d LEMON "LXFOPVEFRNHR"  -> ATTACKATDAWN
 *   echo Hello | vigenere key         -> Rijvs
 *
 * The text comes from the ARGS (joined with spaces) or, when only the KEY is given, from
 * stdin. Pure argv/stdin -> stdout; built INSIDE the OS by `cc` — installs via
 * `xbm install vigenere`.
 */

static unsigned char keyshift[256];   /* the KEY's letter values, 0..25 */
static int keylen = 0;

/* Fill keyshift[] from KEY's letters (case-insensitive); returns the count, 0 if none. */
static int build_key(const char* key) {
    int n = 0;
    for (const char* p = key; *p && n < 256; p++) {
        if (*p >= 'a' && *p <= 'z')      keyshift[n++] = (unsigned char)(*p - 'a');
        else if (*p >= 'A' && *p <= 'Z') keyshift[n++] = (unsigned char)(*p - 'A');
    }
    keylen = n;
    return n;
}

/* Shift one string; *ki is the running key index, advanced only on letters. dir = +1 / -1. */
static void emit(const char* s, int dir, int* ki) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        int k = keyshift[*ki % keylen];
        if (c >= 'a' && c <= 'z') {
            putchar('a' + (((c - 'a') + dir * k + 26) % 26));
            (*ki)++;
        } else if (c >= 'A' && c <= 'Z') {
            putchar('A' + (((c - 'A') + dir * k + 26) % 26));
            (*ki)++;
        } else {
            putchar(c);
        }
    }
}

int main(int argc, char** argv) {
    int dir = 1;             /* +1 encrypt, -1 decrypt */
    int i = 1;
    if (i < argc && strcmp(argv[i], "-d") == 0) { dir = -1; i++; }

    if (i >= argc) { fprintf(stderr, "usage: vigenere [-d] KEY [text...]\n"); return 1; }
    const char* key = argv[i++];
    if (!build_key(key)) { fprintf(stderr, "vigenere: KEY must contain at least one letter\n"); return 1; }

    int ki = 0;
    if (i < argc) {
        for (int a = i; a < argc; a++) {
            if (a > i) putchar(' ');   /* a space is a non-letter: passes through, key unchanged */
            emit(argv[a], dir, &ki);
        }
        putchar('\n');
    } else {
        char line[512];
        while (fgets(line, sizeof(line), stdin)) emit(line, dir, &ki);
    }
    return 0;
}
