#include "libc.h"

/* nato — spell text in the NATO/ICAO phonetic alphabet, a radio-spelling sibling of `morse`.
 *
 *   nato TEXT...     each letter becomes its NATO word, each digit its number word
 *
 *   nato SOS         -> Sierra Oscar Sierra
 *   nato "Nyx 42"    -> November Yankee Xray Four Two
 *   echo hi | nato   -> Hotel India
 *
 * Case-insensitive; letters A-Z and digits 0-9 map to words, everything else (spaces,
 * punctuation) is skipped and just separates words. Text comes from the ARGS (joined with
 * spaces) or, when none are given, from stdin. Pure argv/stdin -> stdout, deterministic;
 * built INSIDE the OS by `cc` — installs via `xbm install nato`.
 */

static const char* const ALPHA[26] = {
    "Alfa","Bravo","Charlie","Delta","Echo","Foxtrot","Golf","Hotel","India","Juliett",
    "Kilo","Lima","Mike","November","Oscar","Papa","Quebec","Romeo","Sierra","Tango",
    "Uniform","Victor","Whiskey","Xray","Yankee","Zulu"
};
static const char* const DIGIT[10] = {
    "Zero","One","Two","Three","Four","Five","Six","Seven","Eight","Nine"
};

static void put_str(const char* s) { while (*s) putchar(*s++); }

/* Emit the phonetic word for one char, if any; *first tracks the leading-space gap. */
static void emit_char(char c, int* first) {
    const char* w = 0;
    if (c >= 'a' && c <= 'z')      w = ALPHA[c - 'a'];
    else if (c >= 'A' && c <= 'Z') w = ALPHA[c - 'A'];
    else if (c >= '0' && c <= '9') w = DIGIT[c - '0'];
    if (!w) return;
    if (!*first) putchar(' ');
    *first = 0;
    put_str(w);
}

int main(int argc, char** argv) {
    int first = 1;
    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            for (const char* p = argv[i]; *p; p++) emit_char(*p, &first);
        putchar('\n');
    } else {
        char line[512];
        while (fgets(line, sizeof(line), stdin))
            for (const char* p = line; *p; p++) emit_char(*p, &first);
        putchar('\n');
    }
    return 0;
}
