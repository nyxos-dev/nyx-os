/* nppc — the N++ front-end compiler (M6.2 skeleton).
 *
 * N++ is N's application-layer superset (lang/docs/design-npp.md). Per the
 * section-6 shape decision, nppc lowers .npp source to N SOURCE, and the
 * verified N pipeline — ncc, or the self-hosted toolbox — carries it the
 * rest of the way to C and the OS. The lowered .n is checked by ncc's own
 * type/ownership/capability checker, a soundness net under this front-end.
 *
 * THIS RUNG (M6.2) is the honest skeleton: the accepted dialect is exactly
 * the N subset, and lowering is the identity — the source is written out
 * byte-for-byte. What makes the rung real is the READ side: the whole file
 * is lexed with a faithful copy of ncc's lexer (same tokens, same escapes,
 * same interpolation mode stack, same attributes, same diagnostics), so
 * every .npp file nppc accepts is one the N pipeline will lex identically.
 * Each M6.3+ rung (generics, closures, modules) replaces a slice of the
 * identity with a real transform.
 *
 * Build (hosted, any C99 compiler):  gcc -O2 -Wall -Wextra -o nppc nppc.c
 * Use:                               nppc file.npp -o file.n
 */

#ifdef __TINYC__
#include "libc.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#endif

/* ------------------------------------------------------------------ */
/* utils                                                              */
/* ------------------------------------------------------------------ */
static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
static void* xmalloc(size_t n) {
    void* p = calloc(1, n ? n : 1);
    if (!p) die("nppc: out of memory");
    return p;
}
static char* xstrndup(const char* s, size_t n) {
    char* p = xmalloc(n + 1);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* lexer — a faithful copy of ncc's (lang/ncc/ncc.c). The N++ dialect */
/* adds no tokens at this rung; when M6.3+ syntax lands, it lands     */
/* HERE first and in the lowering, never in ncc.                      */
/* ------------------------------------------------------------------ */
typedef enum {
    T_EOF, T_INT, T_STR, T_STR_HEAD, T_STR_MID, T_STR_TAIL, T_INTERP_R,
    T_IDENT,
    T_KW_EXTERN, T_KW_SYSCALL, T_KW_FN, T_KW_MUT, T_KW_RETURN,
    T_KW_IF, T_KW_ELSE, T_KW_WHILE, T_KW_BREAK, T_KW_CONTINUE,
    T_KW_AS, T_KW_RAW, T_KW_TRUE, T_KW_FALSE, T_KW_STRUCT, T_KW_DEFER,
    T_KW_ENUM, T_KW_MATCH, T_FATARROW, T_KW_IMPL,
    T_KW_FOR, T_KW_IN, T_DOTDOT, T_ATTR_USER, T_ATTR_CAPS_SYSCALL, T_ATTR_DROP,
    T_LBRACK, T_RBRACK, T_KW_OWN,
    T_LP, T_RP, T_LB, T_RB, T_COMMA, T_SEMI, T_COLON,
    T_WALRUS, T_ARROW, T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_PLUSEQ, T_MINUSEQ,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_NOT, T_ANDAND, T_OROR, T_AMP, T_PIPE, T_CARET, T_SHL, T_SHR,
    T_DOT, T_QUESTION
} TK;

typedef struct {
    TK k;
    int line;
    long long ival;
    char* s;
    int slen;
} Tok;

static const char* FILENAME;
static const char* SRC;
static int POS, LEN, LINE = 1, COL = 1;

/* Source span of the token next_token() is about to return: TOK_START is set
 * past leading whitespace/comments, TOK_END is read as POS right after the
 * call. The generic pass (M6.3) needs these to splice edits over the verbatim
 * source, so everything it does not rewrite keeps its exact bytes. */
static int TOK_START;

/* string-interpolation mode stack */
static int istack[16], itop = -1, brace_depth = 0, resume_str = 0;

static int is_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_dg(int c)  { return c >= '0' && c <= '9'; }
static int is_hx(int c)  { return is_dg(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hxv(int c)    { return is_dg(c) ? c - '0' : (c | 32) - 'a' + 10; }

static Tok mk(TK k, int sl) {
    Tok t;
    memset(&t, 0, sizeof t);
    t.k = k;
    t.line = sl;
    return t;
}

static Tok scan_string_body(int cont) {
    int sl = LINE;
    char* out = xmalloc((size_t)LEN + 1);
    int on = 0;
    for (;;) {
        if (POS >= LEN) die("%s:%d: unterminated string", FILENAME, sl);
        char c = SRC[POS];
        if (c == '"') {
            POS++; COL++;
            Tok t = mk(cont ? T_STR_TAIL : T_STR, sl);
            t.s = out; t.slen = on;
            return t;
        }
        if (c == '{') {
            POS++; COL++;
            if (itop + 1 >= 16) die("%s:%d: interpolation too deep", FILENAME, sl);
            istack[++itop] = brace_depth;
            Tok t = mk(cont ? T_STR_MID : T_STR_HEAD, sl);
            t.s = out; t.slen = on;
            return t;
        }
        if (c == '\\') {
            POS++; COL++;
            if (POS >= LEN) die("%s:%d: bad escape", FILENAME, sl);
            char e = SRC[POS++]; COL++;
            int v;
            switch (e) {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = 0;    break;
                case '\\': v = '\\'; break;
                case '"': v = '"';  break;
                case '\'': v = '\''; break;
                case '{': v = '{';  break;
                case '}': v = '}';  break;
                default: die("%s:%d: bad escape '\\%c'", FILENAME, sl, e); v = 0;
            }
            out[on++] = (char)v;
            continue;
        }
        if (c == '\n') { LINE++; COL = 1; } else COL++;
        out[on++] = c;
        POS++;
    }
}

static TK kwlook(const char* s, int n) {
    static const struct { const char* w; TK k; } K[] = {
        {"extern", T_KW_EXTERN}, {"syscall", T_KW_SYSCALL}, {"fn", T_KW_FN},
        {"mut", T_KW_MUT}, {"return", T_KW_RETURN}, {"if", T_KW_IF},
        {"else", T_KW_ELSE}, {"while", T_KW_WHILE}, {"break", T_KW_BREAK},
        {"continue", T_KW_CONTINUE}, {"as", T_KW_AS}, {"raw", T_KW_RAW},
        {"true", T_KW_TRUE}, {"false", T_KW_FALSE}, {"struct", T_KW_STRUCT},
        {"defer", T_KW_DEFER}, {"enum", T_KW_ENUM}, {"match", T_KW_MATCH},
        {"impl", T_KW_IMPL}, {"for", T_KW_FOR}, {"in", T_KW_IN},
        {"own", T_KW_OWN},
    };
    for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++)
        if ((int)strlen(K[i].w) == n && !memcmp(K[i].w, s, (size_t)n)) return K[i].k;
    return T_IDENT;
}

static Tok next_token(void) {
    if (resume_str) {
        resume_str = 0;
        TOK_START = POS;
        return scan_string_body(1);
    }
    for (;;) {   /* skip whitespace + comments */
        if (POS >= LEN) return mk(T_EOF, LINE);
        char c = SRC[POS];
        if (c == ' ' || c == '\t' || c == '\r') { POS++; COL++; continue; }
        if (c == '\n') { POS++; LINE++; COL = 1; continue; }
        if (c == '/' && POS + 1 < LEN && SRC[POS + 1] == '/') {
            while (POS < LEN && SRC[POS] != '\n') POS++;
            continue;
        }
        if (c == '/' && POS + 1 < LEN && SRC[POS + 1] == '*') {
            int d = 1;
            POS += 2; COL += 2;
            while (POS < LEN && d) {
                if (SRC[POS] == '/' && POS + 1 < LEN && SRC[POS + 1] == '*') { d++; POS += 2; continue; }
                if (SRC[POS] == '*' && POS + 1 < LEN && SRC[POS + 1] == '/') { d--; POS += 2; continue; }
                if (SRC[POS] == '\n') { LINE++; COL = 1; }
                POS++;
            }
            continue;
        }
        break;
    }
    int sl = LINE;
    TOK_START = POS;
    char c = SRC[POS];

    if (is_idc(c)) {
        int st = POS;
        while (POS < LEN && (is_idc(SRC[POS]) || is_dg(SRC[POS]))) { POS++; COL++; }
        int n = POS - st;
        TK k = kwlook(SRC + st, n);
        Tok t = mk(k, sl);
        if (k == T_IDENT) { t.s = xstrndup(SRC + st, (size_t)n); t.slen = n; }
        return t;
    }
    if (is_dg(c)) {
        long long v = 0;
        if (c == '0' && POS + 1 < LEN && SRC[POS + 1] == 'x') {
            POS += 2; COL += 2;
            while (POS < LEN && (is_hx(SRC[POS]) || SRC[POS] == '_')) {
                if (SRC[POS] != '_') v = v * 16 + hxv(SRC[POS]);
                POS++; COL++;
            }
        } else {
            while (POS < LEN && (is_dg(SRC[POS]) || SRC[POS] == '_')) {
                if (SRC[POS] != '_') v = v * 10 + (SRC[POS] - '0');
                POS++; COL++;
            }
        }
        Tok t = mk(T_INT, sl);
        t.ival = v;
        return t;
    }
    if (c == '"') { POS++; COL++; return scan_string_body(0); }

    POS++; COL++;
    char n2 = POS < LEN ? SRC[POS] : 0;
    switch (c) {
        case '(': return mk(T_LP, sl);
        case ')': return mk(T_RP, sl);
        case ',': return mk(T_COMMA, sl);
        case ';': return mk(T_SEMI, sl);
        case '.': if (n2 == '.') { POS++; COL++; return mk(T_DOTDOT, sl); }
                  return mk(T_DOT, sl);
        case '[': return mk(T_LBRACK, sl);
        case ']': return mk(T_RBRACK, sl);
        case '?': return mk(T_QUESTION, sl);
        case '^': return mk(T_CARET, sl);
        case '%': return mk(T_PERCENT, sl);
        case '*': return mk(T_STAR, sl);
        case '/': return mk(T_SLASH, sl);
        case '{': brace_depth++; return mk(T_LB, sl);
        case '}':
            if (itop >= 0 && brace_depth == istack[itop]) {
                itop--;
                resume_str = 1;
                return mk(T_INTERP_R, sl);
            }
            brace_depth--;
            return mk(T_RB, sl);
        case ':': if (n2 == '=') { POS++; COL++; return mk(T_WALRUS, sl); } return mk(T_COLON, sl);
        case '-': if (n2 == '>') { POS++; COL++; return mk(T_ARROW, sl); }
                  if (n2 == '=') { POS++; COL++; return mk(T_MINUSEQ, sl); }
                  return mk(T_MINUS, sl);
        case '+': if (n2 == '=') { POS++; COL++; return mk(T_PLUSEQ, sl); } return mk(T_PLUS, sl);
        case '=': if (n2 == '=') { POS++; COL++; return mk(T_EQ, sl); }
                  if (n2 == '>') { POS++; COL++; return mk(T_FATARROW, sl); }
                  return mk(T_ASSIGN, sl);
        case '!': if (n2 == '=') { POS++; COL++; return mk(T_NE, sl); } return mk(T_NOT, sl);
        case '<': if (n2 == '=') { POS++; COL++; return mk(T_LE, sl); }
                  if (n2 == '<') { POS++; COL++; return mk(T_SHL, sl); }
                  return mk(T_LT, sl);
        case '>': if (n2 == '=') { POS++; COL++; return mk(T_GE, sl); }
                  if (n2 == '>') { POS++; COL++; return mk(T_SHR, sl); }
                  return mk(T_GT, sl);
        case '&': if (n2 == '&') { POS++; COL++; return mk(T_ANDAND, sl); } return mk(T_AMP, sl);
        case '|': if (n2 == '|') { POS++; COL++; return mk(T_OROR, sl); } return mk(T_PIPE, sl);
        case '#':                 /* attributes: #[user] (v0.12), #[caps(syscall)]
                                   * (v0.14), #[drop(fn)] (v0.19) */
            if (POS + 6 <= LEN && !strncmp(&SRC[POS], "[user]", 6)) {
                POS += 6; COL += 6;
                return mk(T_ATTR_USER, sl);
            }
            if (POS + 15 <= LEN && !strncmp(&SRC[POS], "[caps(syscall)]", 15)) {
                POS += 15; COL += 15;
                return mk(T_ATTR_CAPS_SYSCALL, sl);
            }
            if (POS + 6 <= LEN && !strncmp(&SRC[POS], "[drop(", 6)) {
                POS += 6; COL += 6;
                int st = POS;
                while (POS < LEN && (is_idc(SRC[POS]) || is_dg(SRC[POS]))) { POS++; COL++; }
                if (POS == st || POS + 2 > LEN || strncmp(&SRC[POS], ")]", 2) != 0)
                    die("%s:%d: #[drop(...)] names one function, like #[drop(close_file)]",
                        FILENAME, sl);
                Tok t = mk(T_ATTR_DROP, sl);
                t.s = xstrndup(SRC + st, (size_t)(POS - st));
                POS += 2; COL += 2;
                return t;
            }
            die("%s:%d: unknown attribute — #[user], #[caps(syscall)] and #[drop(fn)] are the attributes",
                FILENAME, sl);
    }
    die("%s:%d: unexpected character '%c'", FILENAME, sl, c);
    return mk(T_EOF, sl);
}

/* ------------------------------------------------------------------ */
/* token buffer — the generic pass (M6.3) needs random access + spans  */
/* ------------------------------------------------------------------ */
typedef struct { TK k; int start; int end; int line; char* s; int slen; } STok;
static STok* TOKS;
static int NTOK;

static void lex_all(void) {
    int cap = 1024;
    TOKS = xmalloc((size_t)cap * sizeof(STok));
    NTOK = 0;
    for (;;) {
        Tok t = next_token();
        if (NTOK >= cap) {
            cap *= 2;
            TOKS = realloc(TOKS, (size_t)cap * sizeof(STok));
            if (!TOKS) die("nppc: out of memory");
        }
        STok s;
        s.k = t.k; s.s = t.s; s.slen = t.slen; s.line = t.line;
        s.start = (t.k == T_EOF) ? LEN : TOK_START;
        s.end = POS;
        TOKS[NTOK++] = s;
        if (t.k == T_EOF) break;
    }
    if (itop >= 0) die("%s: unterminated interpolation at end of file", FILENAME);
}

/* Lex `text` as the current file. The lexer's state is reset per call, so
 * module resolution (M6.5) can lex each file in turn and the generic pass
 * can lex the resolved program afterwards. */
static void lex_text(const char* text, long len) {
    SRC = text; LEN = (int)len;
    POS = 0; LINE = 1; COL = 1; TOK_START = 0;
    itop = -1; brace_depth = 0; resume_str = 0;
    lex_all();
}

static int tokspan_eq(int a, int b) {      /* two IDENT tokens name the same word */
    return TOKS[a].k == T_IDENT && TOKS[b].k == T_IDENT &&
           TOKS[a].slen == TOKS[b].slen &&
           !memcmp(TOKS[a].s, TOKS[b].s, (size_t)TOKS[a].slen);
}

/* ------------------------------------------------------------------ */
/* the generic pass (M6.3a): monomorphize generic structs             */
/*                                                                    */
/* `struct Box<T> { val: T }` used as `Box<i64>` (in a type position   */
/* or a `Box<i64>{...}` literal) becomes a concrete `__g_Box_i64` with */
/* T substituted, and every use is rewritten to the mangled name. The  */
/* transform is span-local: only the generic declaration and its use   */
/* sites are edited, so all other bytes (comments, spacing) pass        */
/* through verbatim, and a program with no generics is the identity.    */
/* ------------------------------------------------------------------ */
#define MAXP 8
#define MAXF 64
#define MAXG 64
#define MAXI 2048
#define MAXNEST 32

/* A generic use inside another generic's template (M6.3g) — `Box<T>` in the
 * body of `fn wrap<T>`. It denotes one instantiation of the inner generic
 * per concrete instantiation of the template: each argument is either a
 * concrete type name or one of the template's type parameters (aparam). */
typedef struct {
    int gi;                               /* the inner generic */
    char* aname[MAXP]; int alen[MAXP];    /* argument names ... */
    int aparam[MAXP];                     /* ... or the template type-param they name (-1 if concrete) */
    int nargs;
    int start, end;                       /* the use's span inside the template */
} Nest;

typedef struct {
    int kind;                             /* 0 = struct, 1 = fn, 2 = enum */
    int nametok;                          /* the IDENT token of the generic name */
    int ptok[MAXP]; int nparams;          /* type-param IDENT tokens */
    int declstart, declend;               /* source span of the whole declaration */
    int own;                              /* struct-only: an `own struct` template (M6.4c6c) —
                                           * its instantiations are own structs too */
    int dropfn;                           /* struct-only: the `#[drop(f)]` token in front, or -1
                                           * (M6.4d) — the destructor travels with the template */
    int dropgi;                           /* ... and the generic function it names, resolved by
                                           * guard_templates: each instantiation of the struct
                                           * names the matching instantiation of f */
    /* struct-only: the field templates */
    struct { int fname; int ptrs; int base; int nest; int ts, te; } fields[MAXF];
    int nfields;                          /* base: token index of the field type name;
                                           * nest: its nested generic use, or -1 (M6.3h);
                                           * ts..te: a function or closure type's token
                                           * span, te = 0 otherwise (M6.4c3b) */
    /* fn-only: the token bounds the span-splice emit walks */
    int angleclose;                       /* token index of `>` closing the params */
    int lasttok;                          /* token index of the body's closing `}` */
    /* generic uses inside this template (M6.3g), instantiated per concrete
     * instantiation of it */
    Nest nested[MAXNEST]; int nnested;
} GStruct;
static GStruct GS[MAXG];
static int NGS;
static int skip_type(int t);              /* a type's token extent (below, with the lambdas) */

typedef struct {
    int gi;                               /* which GStruct */
    char* aname[MAXP]; int alen[MAXP];    /* type-arg names (a token's text for an
                                           * explicit arg, an inferred type for a
                                           * bare call) */
    int nargs;
    int start, end;                       /* span to rewrite to the mangled name */
} Inst;
static Inst INSTS[MAXI];
static int NINST;

typedef struct { int start, end; char* repl; } Edit;
static Edit EDITS[MAXG + MAXI];
static int NEDIT;
static int edit_cmp(const void* x, const void* y);

static int gfind(int nametok) {           /* generic index whose name == this IDENT */
    for (int i = 0; i < NGS; i++)
        if (tokspan_eq(GS[i].nametok, nametok)) return i;
    return -1;
}

/* The template whose declaration contains byte `pos`, or -1. Templates do
 * not nest, so there is at most one. */
static int enclosing_template(int pos) {
    for (int i = 0; i < NGS; i++)
        if (pos >= GS[i].declstart && pos < GS[i].declend) return i;
    return -1;
}

/* Collect `struct NAME<params> { fields }` declarations. Plain (non-generic)
 * structs have no `<` after the name and are left entirely alone. */
static void collect_generic_decls(void) {
    for (int i = 0; i + 2 < NTOK; i++) {
        if (TOKS[i].k != T_KW_STRUCT) continue;
        if (TOKS[i + 1].k != T_IDENT || TOKS[i + 2].k != T_LT) continue;
        if (NGS >= MAXG) die("%s: too many generic structs", FILENAME);
        GStruct* g = &GS[NGS];
        g->kind = 0;
        g->nametok = i + 1;
        g->nparams = 0;
        g->nnested = 0;
        g->own = i > 0 && TOKS[i - 1].k == T_KW_OWN;   /* `own struct S<T>` (M6.4c6c): the
                                                        * keyword travels with the template */
        g->declstart = g->own ? TOKS[i - 1].start : TOKS[i].start;
        {                                 /* `#[drop(f)] own struct S<T>` (M6.4d): the destructor
                                           * travels with the template too — the attribute is cut
                                           * out with it, and every instantiation names f's matching
                                           * instantiation (resolved by guard_templates) */
            int a = g->own ? i - 2 : i - 1;
            g->dropfn = (a >= 0 && TOKS[a].k == T_ATTR_DROP) ? a : -1;
            g->dropgi = -1;
            if (g->dropfn >= 0 && !g->own)
                die("%s:%d: #[drop(%s)] on generic '%.*s' needs an own struct — only an own struct has a destructor",
                    FILENAME, TOKS[a].line, TOKS[a].s, TOKS[i + 1].slen, TOKS[i + 1].s);
            if (g->dropfn >= 0) g->declstart = TOKS[a].start;
        }
        int j = i + 3;                    /* first param */
        for (;;) {
            if (TOKS[j].k != T_IDENT)
                die("%s:%d: generic parameter must be a name", FILENAME, TOKS[j].line);
            if (g->nparams >= MAXP) die("%s: too many type parameters", FILENAME);
            g->ptok[g->nparams++] = j++;
            if (TOKS[j].k == T_COMMA) { j++; continue; }
            if (TOKS[j].k == T_GT) { j++; break; }
            die("%s:%d: expected ',' or '>' in type-parameter list", FILENAME, TOKS[j].line);
        }
        if (TOKS[j].k != T_LB)
            die("%s:%d: expected '{' after generic struct header", FILENAME, TOKS[j].line);
        j++;                              /* into the body */
        g->nfields = 0;
        while (TOKS[j].k != T_RB) {
            if (TOKS[j].k == T_EOF)
                die("%s: unterminated generic struct body", FILENAME);
            if (TOKS[j].k != T_IDENT)
                die("%s:%d: expected a field name", FILENAME, TOKS[j].line);
            int fname = j++;
            if (TOKS[j].k != T_COLON)
                die("%s:%d: expected ':' after field name", FILENAME, TOKS[j].line);
            j++;
            if (TOKS[j].k == T_ATTR_USER || TOKS[j].k == T_KW_RAW)
                die("%s:%d: M6.3a: attributes on generic-struct fields are pending",
                    FILENAME, TOKS[j].line);
            int ptrs = 0;
            while (TOKS[j].k == T_STAR) { ptrs++; j++; }
            if (g->nfields >= MAXF) die("%s: too many fields", FILENAME);
            g->fields[g->nfields].fname = fname;
            g->fields[g->nfields].ptrs = ptrs;
            g->fields[g->nfields].nest = -1;
            g->fields[g->nfields].ts = g->fields[g->nfields].te = 0;
            if (TOKS[j].k == T_KW_FN || (TOKS[j].k == T_IDENT && TOKS[j + 1].k == T_LP)) {
                /* A function or closure type (M6.4c3b): kept as a token span
                 * the emit renders with the type parameters substituted. */
                int e = skip_type(j);
                if (e < 0) die("%s:%d: malformed function type", FILENAME, TOKS[j].line);
                g->fields[g->nfields].base = j;
                g->fields[g->nfields].ts = j;
                g->fields[g->nfields].te = e;
                g->nfields++;
                j = e;
                if (TOKS[j].k == T_COMMA) j++;
                continue;
            }
            if (TOKS[j].k != T_IDENT)
                die("%s:%d: expected a field type", FILENAME, TOKS[j].line);
            g->fields[g->nfields].base = j;
            int base = j++;
            if (TOKS[j].k == T_LT) {
                /* A generic type — `a: Box<T>` (M6.3h): a nested use of the
                 * struct, instantiated per concrete instantiation of it. The
                 * generic it names is resolved by guard_templates, once every
                 * generic (an enum, say) is known. */
                if (g->nnested >= MAXNEST) die("%s: too many nested generic uses", FILENAME);
                Nest* ns = &g->nested[g->nnested];
                ns->gi = -1;
                ns->nargs = 0;
                ns->start = TOKS[base].start;
                j++;
                for (;;) {
                    if (TOKS[j].k != T_IDENT || ns->nargs >= MAXP)
                        die("%s:%d: expected a type argument", FILENAME, TOKS[j].line);
                    ns->aname[ns->nargs] = TOKS[j].s;
                    ns->alen[ns->nargs] = TOKS[j].slen;
                    ns->aparam[ns->nargs] = -1;
                    for (int q = 0; q < g->nparams; q++)
                        if (tokspan_eq(g->ptok[q], j)) ns->aparam[ns->nargs] = q;
                    ns->nargs++;
                    j++;
                    if (TOKS[j].k == T_COMMA) { j++; continue; }
                    if (TOKS[j].k == T_GT) break;
                    die("%s:%d: expected ',' or '>' in type-argument list", FILENAME, TOKS[j].line);
                }
                ns->end = TOKS[j].end;
                j++;
                g->fields[g->nfields].nest = g->nnested++;
            }
            g->nfields++;
            if (TOKS[j].k == T_COMMA) j++;
        }
        g->declend = TOKS[j].end;         /* past the closing '}' */
        NGS++;
    }
}

/* Collect `NAME<args>` use sites of a declared generic. Lenient: a `<` that
 * is not a well-formed type-argument list is left as a comparison operator. */
static void collect_instantiations(void) {
    for (int i = 0; i + 1 < NTOK; i++) {
        if (TOKS[i].k != T_IDENT || TOKS[i + 1].k != T_LT) continue;
        if (i > 0 && (TOKS[i - 1].k == T_KW_STRUCT || TOKS[i - 1].k == T_KW_FN ||
                      TOKS[i - 1].k == T_KW_ENUM))
            continue;                      /* the decl header, not a use site */
        int gi = gfind(i);
        if (gi < 0) continue;             /* `<` after a non-generic name: comparison */
        int atok[MAXP], nargs = 0, ok = 1, j = i + 2;
        for (;;) {
            if (TOKS[j].k != T_IDENT) { ok = 0; break; }
            if (nargs >= MAXP) { ok = 0; break; }
            atok[nargs++] = j++;
            if (TOKS[j].k == T_COMMA) { j++; continue; }
            if (TOKS[j].k == T_GT) { j++; break; }
            ok = 0; break;
        }
        if (!ok || nargs != GS[gi].nparams) continue;   /* not this generic's shape */
        int outer = enclosing_template(TOKS[i].start);
        if (outer >= 0) {
            /* Inside another template (M6.3g): instantiated per concrete
             * instantiation of that template, by expand_nested. */
            GStruct* og = &GS[outer];
            if (og->nnested >= MAXNEST) die("%s: too many nested generic uses", FILENAME);
            Nest* ns = &og->nested[og->nnested++];
            ns->gi = gi;
            ns->nargs = nargs;
            for (int a = 0; a < nargs; a++) {
                ns->aname[a] = TOKS[atok[a]].s;
                ns->alen[a] = TOKS[atok[a]].slen;
                ns->aparam[a] = -1;
                for (int q = 0; q < og->nparams; q++)
                    if (tokspan_eq(og->ptok[q], atok[a])) ns->aparam[a] = q;
            }
            ns->start = TOKS[i].start;
            ns->end = TOKS[j - 1].end;
            i = j - 1;
            continue;
        }
        if (NINST >= MAXI) die("%s: too many generic instantiations", FILENAME);
        Inst* n = &INSTS[NINST++];
        n->gi = gi;
        n->nargs = nargs;
        for (int a = 0; a < nargs; a++) {
            n->aname[a] = TOKS[atok[a]].s;
            n->alen[a] = TOKS[atok[a]].slen;
        }
        n->start = TOKS[i].start;
        n->end = TOKS[j - 1].end;         /* past the closing '>' */
        i = j - 1;                        /* skip past the args */
    }
}

/* Bare generic-function calls `NAME(args)` with no explicit `<...>`: infer
 * each type parameter from a literal argument in the matching position, and
 * record an instantiation that rewrites just the name. Literal arguments only
 * for now (int -> i64, string -> str, bool -> bool); anything else asks for an
 * explicit `NAME<Type>(...)`. */
static void collect_inferred_calls(void) {
    for (int i = 0; i + 1 < NTOK; i++) {
        if (TOKS[i].k != T_IDENT || TOKS[i + 1].k != T_LP) continue;
        if (i > 0 && TOKS[i - 1].k == T_KW_FN) continue;   /* a decl header, not a call */
        int gi = gfind(i);
        if (gi < 0 || GS[gi].kind != 1) continue;          /* generic-fn calls only */
        GStruct* g = &GS[gi];
        /* Walk the fn's own parameter list `(name: type, ...)` from just after
         * `>`, mapping each value parameter to a type-param index (or -1). */
        int vptp[64], nv = 0, j = g->angleclose + 2;       /* first param name */
        while (TOKS[j].k != T_RP && TOKS[j].k != T_EOF) {
            if (TOKS[j].k != T_IDENT) break;
            j++;
            if (TOKS[j].k != T_COLON) break;
            j++;
            while (TOKS[j].k == T_ATTR_USER || TOKS[j].k == T_KW_RAW || TOKS[j].k == T_STAR) j++;
            int tp = -1;
            for (int p = 0; p < g->nparams; p++)
                if (tokspan_eq(g->ptok[p], j)) tp = p;
            if (nv < 64) vptp[nv++] = tp;
            j++;
            if (TOKS[j].k == T_COMMA) { j++; continue; }
            break;
        }
        /* First token of each top-level call argument. */
        int argfirst[64], na = 0, depth = 0, startarg = 1;
        for (int t = i + 1; TOKS[t].k != T_EOF; t++) {
            TK kk = TOKS[t].k;
            if (kk == T_LP || kk == T_LB || kk == T_LBRACK) depth++;
            else if (kk == T_RP || kk == T_RB || kk == T_RBRACK) { depth--; if (depth == 0) break; }
            else if (kk == T_COMMA && depth == 1) startarg = 1;
            else if (depth >= 1 && startarg) { if (na < 64) argfirst[na++] = t; startarg = 0; }
        }
        /* Infer each type parameter from the value parameter that uses it. */
        char* an[MAXP]; int al[MAXP], bound[MAXP];
        for (int p = 0; p < g->nparams; p++) bound[p] = 0;
        for (int v = 0; v < nv && v < na; v++) {
            int tp = vptp[v];
            if (tp < 0) continue;
            char* ty; int tl;
            TK ak = TOKS[argfirst[v]].k;
            if (ak == T_INT) { ty = "i64"; tl = 3; }
            else if (ak == T_STR || ak == T_STR_HEAD) { ty = "str"; tl = 3; }
            else if (ak == T_KW_TRUE || ak == T_KW_FALSE) { ty = "bool"; tl = 4; }
            else die("%s:%d: cannot infer a generic type from argument %d — write it "
                     "explicitly, %.*s<Type>(...)", FILENAME, TOKS[i].line, v + 1,
                     TOKS[g->nametok].slen, TOKS[g->nametok].s);
            if (bound[tp] && (al[tp] != tl || memcmp(an[tp], ty, (size_t)tl)))
                die("%s:%d: conflicting types inferred for one generic parameter",
                    FILENAME, TOKS[i].line);
            an[tp] = ty; al[tp] = tl; bound[tp] = 1;
        }
        for (int p = 0; p < g->nparams; p++)
            if (!bound[p])
                die("%s:%d: cannot infer every generic parameter of '%.*s' — write them "
                    "explicitly", FILENAME, TOKS[i].line, TOKS[g->nametok].slen, TOKS[g->nametok].s);
        int outer = enclosing_template(TOKS[i].start);
        if (outer >= 0) {                  /* inside a template: a nested use (M6.3g) */
            GStruct* og = &GS[outer];
            if (og->nnested >= MAXNEST) die("%s: too many nested generic uses", FILENAME);
            Nest* ns = &og->nested[og->nnested++];
            ns->gi = gi;
            ns->nargs = g->nparams;
            for (int p = 0; p < g->nparams; p++) {
                ns->aname[p] = an[p]; ns->alen[p] = al[p]; ns->aparam[p] = -1;
            }
            ns->start = TOKS[i].start;
            ns->end = TOKS[i].end;
            continue;
        }
        if (NINST >= MAXI) die("%s: too many generic instantiations", FILENAME);
        Inst* n = &INSTS[NINST++];
        n->gi = gi;
        n->nargs = g->nparams;
        for (int p = 0; p < g->nparams; p++) { n->aname[p] = an[p]; n->alen[p] = al[p]; }
        n->start = TOKS[i].start;
        n->end = TOKS[i].end;              /* rewrite just the name to the mangled name */
    }
}

/* Bare generic-enum constructions `NAME.Variant{...}` / `NAME.Variant` with
 * no `<...>` (M6.3f): the type arguments are read from the enclosing
 * function's declared return type, which must be an explicit instantiation of
 * the same enum (`-> NAME<args>`) — the `Result.Ok{ v: n }` idiom inside a
 * function returning `Result<i64, i64>`. The recorded instantiation rewrites
 * just the name, so it dedups against the return type's own. A construction
 * anywhere else — a function returning something different, no enclosing
 * function, or a generic function's body (emitted per instantiation, so it
 * cannot carry a global edit yet) — asks for the explicit
 * `NAME<Type, ...>.Variant` form. */
static void collect_inferred_constructions(void) {
    int ret_gi = -1;                      /* the enclosing fn's return instantiation */
    char* ran[MAXP]; int ral[MAXP]; int rparam[MAXP];
    int body_end = -1;                    /* token index of its body's closing `}` */
    int generic_fn = 0, outer_gi = -1;    /* a template's own GStruct, when inside one */
    for (int i = 0; i + 1 < NTOK; i++) {
        if (TOKS[i].k == T_KW_FN && TOKS[i + 1].k == T_IDENT) {
            int j = i + 2, depth = 0;
            ret_gi = -1; body_end = -1; generic_fn = 0; outer_gi = -1;
            if (TOKS[j].k == T_LT) {      /* fn NAME<params>(...) */
                generic_fn = 1;
                outer_gi = gfind(i + 1);
                while (TOKS[j].k != T_GT && TOKS[j].k != T_EOF) j++;
                j++;
            }
            if (TOKS[j].k != T_LP) continue;
            for (; TOKS[j].k != T_EOF; j++) {   /* past the parameter list */
                if (TOKS[j].k == T_LP) depth++;
                else if (TOKS[j].k == T_RP && --depth == 0) { j++; break; }
            }
            if (TOKS[j].k == T_ARROW && TOKS[j + 1].k == T_IDENT && TOKS[j + 2].k == T_LT) {
                int gi = gfind(j + 1);
                if (gi >= 0 && GS[gi].kind == 2) {
                    int n = 0, k = j + 3, ok = 1;
                    for (;;) {
                        if (TOKS[k].k != T_IDENT || n >= MAXP) { ok = 0; break; }
                        ran[n] = TOKS[k].s; ral[n] = TOKS[k].slen; rparam[n] = -1;
                        if (outer_gi >= 0)    /* an argument that is the template's own type-param */
                            for (int q = 0; q < GS[outer_gi].nparams; q++)
                                if (tokspan_eq(GS[outer_gi].ptok[q], k)) rparam[n] = q;
                        n++; k++;
                        if (TOKS[k].k == T_COMMA) { k++; continue; }
                        if (TOKS[k].k == T_GT) break;
                        ok = 0; break;
                    }
                    if (ok && n == GS[gi].nparams) ret_gi = gi;
                }
            }
            /* The body, if the function has one (extern fns end in `= N`). */
            while (TOKS[j].k != T_LB && TOKS[j].k != T_ASSIGN && TOKS[j].k != T_SEMI &&
                   TOKS[j].k != T_EOF) j++;
            if (TOKS[j].k == T_LB) {
                depth = 0;
                for (; TOKS[j].k != T_EOF; j++) {
                    if (TOKS[j].k == T_LB) depth++;
                    else if (TOKS[j].k == T_RB && --depth == 0) break;
                }
                body_end = j;
            }
            continue;
        }
        if (TOKS[i].k != T_IDENT || TOKS[i + 1].k != T_DOT) continue;
        if (i > 0 && TOKS[i - 1].k == T_DOT) continue;       /* a field, not a type */
        int gi = gfind(i);
        if (gi < 0 || GS[gi].kind != 2) continue;
        GStruct* g = &GS[gi];
        if (i > body_end)
            die("%s:%d: '%.*s.' names no type arguments and has no enclosing function "
                "to take them from — write %.*s<Type, ...>.Variant",
                FILENAME, TOKS[i].line, TOKS[i].slen, TOKS[i].s,
                TOKS[g->nametok].slen, TOKS[g->nametok].s);
        if (ret_gi != gi)
            die("%s:%d: cannot infer the type arguments of '%.*s.': the enclosing "
                "function does not return %.*s<...> — write %.*s<Type, ...>.Variant",
                FILENAME, TOKS[i].line, TOKS[i].slen, TOKS[i].s,
                TOKS[g->nametok].slen, TOKS[g->nametok].s,
                TOKS[g->nametok].slen, TOKS[g->nametok].s);
        if (generic_fn) {
            /* Inside a template (M6.3g): a nested use carrying the return
             * type's arguments — some of them the template's own parameters —
             * instantiated per concrete instantiation of the template. */
            GStruct* og = &GS[outer_gi];
            if (og->nnested >= MAXNEST) die("%s: too many nested generic uses", FILENAME);
            Nest* ns = &og->nested[og->nnested++];
            ns->gi = gi;
            ns->nargs = g->nparams;
            for (int p = 0; p < g->nparams; p++) {
                ns->aname[p] = ran[p]; ns->alen[p] = ral[p]; ns->aparam[p] = rparam[p];
            }
            ns->start = TOKS[i].start;
            ns->end = TOKS[i].end;
            continue;
        }
        if (NINST >= MAXI) die("%s: too many generic instantiations", FILENAME);
        Inst* n = &INSTS[NINST++];
        n->gi = gi;
        n->nargs = g->nparams;
        for (int p = 0; p < g->nparams; p++) { n->aname[p] = ran[p]; n->alen[p] = ral[p]; }
        n->start = TOKS[i].start;
        n->end = TOKS[i].end;              /* rewrite just the name to the mangled name */
    }
}

/* "__g_Box_i64" for Box<i64>. Args are bare type names, so the result is a
 * valid N/C identifier by construction. */
static char* mangle(Inst* it) {
    GStruct* g = &GS[it->gi];
    char* out = xmalloc(256);
    int n = 0;
    n += sprintf(out + n, "__g_%.*s", TOKS[g->nametok].slen, TOKS[g->nametok].s);
    for (int a = 0; a < g->nparams; a++)
        n += sprintf(out + n, "_%.*s", it->alen[a], it->aname[a]);
    return out;
}

/* The instantiation a nested generic use denotes for one concrete
 * instantiation of its template: the template's arguments substituted for
 * the type parameters the use names; concrete names pass through. It has no
 * use site of its own to rewrite (start = end = -1) — the template's emit
 * rewrites the use. */
static void nested_inst(Inst* outer, Nest* ns, Inst* out) {
    out->gi = ns->gi;
    out->nargs = ns->nargs;
    for (int a = 0; a < ns->nargs; a++) {
        if (ns->aparam[a] >= 0) {
            out->aname[a] = outer->aname[ns->aparam[a]];
            out->alen[a] = outer->alen[ns->aparam[a]];
        } else {
            out->aname[a] = ns->aname[a];
            out->alen[a] = ns->alen[a];
        }
    }
    out->start = -1;
    out->end = -1;
}

/* A function or closure type span of a struct template, rendered for one
 * instantiation: each type parameter replaced by its argument, a generic
 * use inside (`Box<T>`, recorded on the template by collect_instantiations
 * and instantiated by expand_nested) by the concrete name it denotes
 * (M6.4c3c), the rest spelled token by token (M6.4c3b). Returns the
 * characters written. */
static int span_subst(GStruct* g, Inst* it, int ts, int te, char* out) {
    int n = 0;
    for (int t = ts; t < te; t++) {
        if (t > ts && (TOKS[t - 1].k == T_COMMA || TOKS[t - 1].k == T_ARROW || TOKS[t].k == T_ARROW ||
                       TOKS[t - 1].k == T_ATTR_USER || TOKS[t - 1].k == T_KW_RAW))
            out[n++] = ' ';
        if (TOKS[t].k == T_IDENT && TOKS[t + 1].k == T_LT) {
            int u = 0;
            while (u < g->nnested && g->nested[u].start != TOKS[t].start) u++;
            if (u == g->nnested)
                die("%s:%d: '%.*s' is not a generic type", FILENAME, TOKS[t].line, TOKS[t].slen, TOKS[t].s);
            Inst ni;
            nested_inst(it, &g->nested[u], &ni);
            n += sprintf(out + n, "%s", mangle(&ni));
            while (TOKS[t].k != T_GT && TOKS[t].k != T_EOF) t++;
            continue;
        }
        int p = -1;
        if (TOKS[t].k == T_IDENT)
            for (int q = 0; q < g->nparams; q++) if (tokspan_eq(g->ptok[q], t)) { p = q; break; }
        if (p >= 0) n += sprintf(out + n, "%.*s", it->alen[p], it->aname[p]);
        else n += sprintf(out + n, "%.*s", TOKS[t].end - TOKS[t].start, SRC + TOKS[t].start);
    }
    return n;
}

/* Is the type text (n characters, not terminated) an `own struct` of this
 * program? */
static int is_own_span(const char* s, int n) {
    for (int t = 0; t + 2 < NTOK; t++)
        if (TOKS[t].k == T_KW_OWN && TOKS[t + 1].k == T_KW_STRUCT && TOKS[t + 2].k == T_IDENT &&
            TOKS[t + 2].slen == n && !memcmp(TOKS[t + 2].s, s, (size_t)n))
            return 1;
    return 0;
}

/* Does this instantiation of a struct template come out own? An own
 * template does (M6.4c6c); so does a plain one whose type argument for a
 * by-value field names an own struct, or whose field is a nested use of
 * an own template (M6.4c6d) — a struct that receives an own value is own,
 * which is N's rule that an own value lives only inside an own container.
 * A lambda's environment template `__E_N<T>` with a capture typed T is the
 * case that matters: nppc cannot know at the lambda whether T will be own,
 * and this decides it per instantiation. */
static int inst_own(GStruct* g, Inst* it) {
    if (g->own) return 1;
    for (int fi = 0; fi < g->nfields; fi++) {
        if (g->fields[fi].ptrs || g->fields[fi].te > 0) continue;
        if (g->fields[fi].nest >= 0) {
            if (GS[g->nested[g->fields[fi].nest].gi].own) return 1;
            continue;
        }
        for (int p = 0; p < g->nparams; p++)
            if (tokspan_eq(g->ptok[p], g->fields[fi].base) && is_own_span(it->aname[p], it->alen[p]))
                return 1;
    }
    return 0;
}

/* The concrete N struct for one instantiation: the template body with each
 * type parameter replaced by the matching argument. */
static char* concrete_struct(Inst* it) {
    GStruct* g = &GS[it->gi];
    char* mn = mangle(it);
    char* out = xmalloc(4096 + 512 * (size_t)g->nfields);
    int n = 0;
    if (g->dropgi >= 0) {                 /* the template's destructor, at this instantiation
                                           * (M6.4d): `#[drop(__g_f_i64)] own struct __g_S_i64` */
        Inst di = *it;
        di.gi = g->dropgi;
        n += sprintf(out + n, "#[drop(%s)]\n", mangle(&di));
    }
    n += sprintf(out + n, "%sstruct %s {\n", inst_own(g, it) ? "own " : "", mn);
    for (int fi = 0; fi < g->nfields; fi++) {
        int bt = g->fields[fi].base;
        int sub = -1;                     /* is the field type a type parameter? */
        for (int p = 0; p < g->nparams; p++)
            if (tokspan_eq(g->ptok[p], bt)) { sub = p; break; }
        int stars = g->fields[fi].ptrs;
        n += sprintf(out + n, "    %.*s: ", TOKS[g->fields[fi].fname].slen,
                     TOKS[g->fields[fi].fname].s);
        for (int s = 0; s < stars; s++) out[n++] = '*';
        if (g->fields[fi].te > 0) {       /* `fn(T) -> T` / `Fn(T) -> T` (M6.4c3b) */
            n += span_subst(g, it, g->fields[fi].ts, g->fields[fi].te, out + n);
        } else if (g->fields[fi].nest >= 0) {    /* `Box<T>` -> `__g_Box_i64` (M6.3h) */
            Inst ni;
            nested_inst(it, &g->nested[g->fields[fi].nest], &ni);
            n += sprintf(out + n, "%s", mangle(&ni));
        } else if (sub >= 0)
            n += sprintf(out + n, "%.*s", it->alen[sub], it->aname[sub]);
        else
            n += sprintf(out + n, "%.*s", TOKS[bt].slen, TOKS[bt].s);
        n += sprintf(out + n, ",\n");
    }
    n += sprintf(out + n, "}");
    return out;
}

/* Is token t a type-parameter of generic g used in a type position (a base
 * type after ':' , '->' or '*')? Returns the param index, or -1. */
/* Is IDENT token t a parameter type of a function TYPE — `fn(T, *U) -> R`
 * (N v0.24)? Walk back over what a parameter list holds (types, commas,
 * nested fn types with their own parentheses and arrows, generic
 * arguments) to the `(` that opens the list, and require `fn` before it. */
/* The IDENT opening a closure type: `Fn(` (M6.4c1) or the owning
 * `FnOnce(` (M6.4c6) — the same signature shape, two structs. */
static int fn_word(int t) {
    return TOKS[t].k == T_IDENT && TOKS[t + 1].k == T_LP &&
           ((TOKS[t].slen == 2 && !memcmp(TOKS[t].s, "Fn", 2)) ||
            (TOKS[t].slen == 6 && !memcmp(TOKS[t].s, "FnOnce", 6)));
}
static int is_once(int t) {
    return TOKS[t].k == T_IDENT && TOKS[t].slen == 6 && !memcmp(TOKS[t].s, "FnOnce", 6);
}

static int in_fn_type(int t) {
    int depth = 0;
    for (int j = t - 1; j > 0; j--) {
        TK k = TOKS[j].k;
        if (k == T_RP) depth++;
        else if (k == T_LP) {
            if (depth == 0)               /* `fn (` — or the closure type `Fn (` (M6.4c3) */
                return TOKS[j - 1].k == T_KW_FN || fn_word(j - 1);
            depth--;
        } else if (k != T_IDENT && k != T_COMMA && k != T_STAR && k != T_KW_RAW &&
                   k != T_ATTR_USER && k != T_ARROW && k != T_KW_FN && k != T_LT && k != T_GT)
            return 0;
    }
    return 0;
}

static int typaram_at(GStruct* g, int t) {
    if (TOKS[t].k != T_IDENT || t == 0) return -1;
    TK p = TOKS[t - 1].k;
    /* A base type follows ':' (a param/field type), '->' (a return type),
     * '*' (a pointer type), or 'as' (a cast target — the one type slot that
     * appears inside a function body, since N locals are always inferred).
     * An argument of a generic use — `Box<T>`, `Pair<T, U>` (M6.3g) — is
     * recognized by walking back to the `<` with a generic's name before it;
     * a parameter type of a function type — `fn(T) -> R` (M6.4b) — by
     * walking back to the `(` with `fn` before it. */
    int slot = (p == T_COLON || p == T_ARROW || p == T_STAR || p == T_KW_AS);
    if (!slot && (p == T_LT || p == T_COMMA)) {
        int j = t - 1;
        while (j > 0 && (TOKS[j].k == T_COMMA || TOKS[j].k == T_IDENT)) j--;
        slot = TOKS[j].k == T_LT && j > 0 && gfind(j - 1) >= 0;
    }
    if (!slot && (p == T_LP || p == T_COMMA)) slot = in_fn_type(t);
    if (!slot) return -1;
    for (int q = 0; q < g->nparams; q++)
        if (tokspan_eq(g->ptok[q], t)) return q;
    return -1;
}

/* Collect `fn NAME<params>(...) -> ret { body }` generic functions. Every
 * type-parameter use must sit in a handled type position; one elsewhere is
 * refused (M6.3c will lift that), so the emit substitutes every occurrence. */
static void collect_generic_fns(void) {
    for (int i = 0; i + 2 < NTOK; i++) {
        if (TOKS[i].k != T_KW_FN) continue;
        if (TOKS[i + 1].k != T_IDENT || TOKS[i + 2].k != T_LT) continue;
        if (NGS >= MAXG) die("%s: too many generics", FILENAME);
        GStruct* g = &GS[NGS];
        g->kind = 1;
        g->nametok = i + 1;
        g->nparams = 0;
        g->nfields = 0;
        g->dropfn = -1;
        g->dropgi = -1;
        /* A `#[caps(syscall)]` in front belongs to the declaration, so every
         * concrete instantiation carries it (a generated environment maker
         * is such a template, M6.4c3). */
        g->declstart = (i > 0 && TOKS[i - 1].k == T_ATTR_CAPS_SYSCALL) ? TOKS[i - 1].start : TOKS[i].start;
        int j = i + 3;                    /* first type parameter */
        for (;;) {
            if (TOKS[j].k != T_IDENT)
                die("%s:%d: generic parameter must be a name", FILENAME, TOKS[j].line);
            if (g->nparams >= MAXP) die("%s: too many type parameters", FILENAME);
            g->ptok[g->nparams++] = j++;
            if (TOKS[j].k == T_COMMA) { j++; continue; }
            if (TOKS[j].k == T_GT) { break; }
            die("%s:%d: expected ',' or '>' in type-parameter list", FILENAME, TOKS[j].line);
        }
        g->angleclose = j;                /* the `>` token */
        j++;
        if (TOKS[j].k != T_LP)
            die("%s:%d: expected '(' after generic function header", FILENAME, TOKS[j].line);
        /* skip to the body's opening brace */
        while (TOKS[j].k != T_LB) {
            if (TOKS[j].k == T_EOF)
                die("%s:%d: unterminated generic function header", FILENAME, TOKS[i].line);
            j++;
        }
        int depth = 0;
        for (;;) {
            if (TOKS[j].k == T_EOF) die("%s: unterminated generic function body", FILENAME);
            if (TOKS[j].k == T_LB) depth++;
            else if (TOKS[j].k == T_RB) { depth--; if (depth == 0) break; }
            j++;
        }
        g->lasttok = j;                   /* the body's closing `}` */
        g->declend = TOKS[j].end;
        g->nnested = 0;                   /* filled by the use collectors */
        NGS++;
    }
}

/* Collect `enum NAME<params> { variants }` generic enums. An enum body is
 * variants with optional `(field: type, ...)` payloads; type parameters live
 * in those payload type slots (after ':'), which the span-splice emit handles
 * exactly as it does a function's — so a generic enum reuses concrete_fn. The
 * body `{...}` follows the `>` directly (no parameter list). */
static void collect_generic_enums(void) {
    for (int i = 0; i + 2 < NTOK; i++) {
        if (TOKS[i].k != T_KW_ENUM) continue;
        if (TOKS[i + 1].k != T_IDENT || TOKS[i + 2].k != T_LT) continue;
        if (NGS >= MAXG) die("%s: too many generics", FILENAME);
        GStruct* g = &GS[NGS];
        g->kind = 2;
        g->nametok = i + 1;
        g->nparams = 0;
        g->nfields = 0;
        g->dropfn = -1;
        g->dropgi = -1;
        g->declstart = TOKS[i].start;
        int j = i + 3;
        for (;;) {
            if (TOKS[j].k != T_IDENT)
                die("%s:%d: generic parameter must be a name", FILENAME, TOKS[j].line);
            if (g->nparams >= MAXP) die("%s: too many type parameters", FILENAME);
            g->ptok[g->nparams++] = j++;
            if (TOKS[j].k == T_COMMA) { j++; continue; }
            if (TOKS[j].k == T_GT) { break; }
            die("%s:%d: expected ',' or '>' in type-parameter list", FILENAME, TOKS[j].line);
        }
        g->angleclose = j;
        j++;
        if (TOKS[j].k != T_LB)
            die("%s:%d: expected '{' after generic enum header", FILENAME, TOKS[j].line);
        int depth = 0;
        for (;;) {
            if (TOKS[j].k == T_EOF) die("%s: unterminated generic enum body", FILENAME);
            if (TOKS[j].k == T_LB) depth++;
            else if (TOKS[j].k == T_RB) { depth--; if (depth == 0) break; }
            j++;
        }
        g->lasttok = j;
        g->declend = TOKS[j].end;
        g->nnested = 0;
        NGS++;
    }
}

/* Every type-parameter occurrence inside a template must sit in a position
 * the emit rewrites — a type slot, or an argument of a generic use — so no
 * parameter survives into a concrete item. Checked once every generic is
 * known, since a use may name a generic declared later in the file. */
static void guard_templates(void) {
    for (int gi = 0; gi < NGS; gi++) {
        GStruct* g = &GS[gi];
        if (g->kind == 0) {               /* struct fields are parsed, not spliced: resolve
                                           * the generic each generic-typed field names */
            for (int f = 0; f < g->nfields; f++) {
                if (g->fields[f].te > 0) {    /* a function-typed field: its generic uses must
                                               * name a generic, with the right arity (M6.4c3c) */
                    for (int t = g->fields[f].ts; t < g->fields[f].te; t++) {
                        if (TOKS[t].k != T_IDENT || TOKS[t + 1].k != T_LT) continue;
                        int inner = gfind(t);
                        if (inner < 0)
                            die("%s:%d: '%.*s' is not a generic type", FILENAME, TOKS[t].line,
                                TOKS[t].slen, TOKS[t].s);
                        int na = 0;
                        for (int a = t + 2; TOKS[a].k == T_IDENT; a += 2) {
                            na++;
                            if (TOKS[a + 1].k != T_COMMA) break;
                        }
                        if (na != GS[inner].nparams)
                            die("%s:%d: '%.*s' takes %d type argument(s)", FILENAME, TOKS[t].line,
                                TOKS[t].slen, TOKS[t].s, GS[inner].nparams);
                    }
                    continue;
                }
                int ne = g->fields[f].nest;
                if (ne < 0) continue;
                int b = g->fields[f].base;
                int inner = gfind(b);
                if (inner < 0)
                    die("%s:%d: '%.*s' is not a generic type", FILENAME, TOKS[b].line,
                        TOKS[b].slen, TOKS[b].s);
                if (g->nested[ne].nargs != GS[inner].nparams)
                    die("%s:%d: '%.*s' takes %d type argument(s)", FILENAME, TOKS[b].line,
                        TOKS[b].slen, TOKS[b].s, GS[inner].nparams);
                g->nested[ne].gi = inner;
            }
            if (g->dropfn >= 0) {         /* `#[drop(f)]` on a template (M6.4d): f must be a
                                           * generic function over the same parameters whose
                                           * one parameter is `S<T…>` — then each instantiation
                                           * of S names the matching instantiation of f */
                const char* fname = TOKS[g->dropfn].s;
                int fl = (int)strlen(fname);
                int di = -1;
                for (int k = 0; k < NGS; k++)
                    if (GS[k].kind == 1 && TOKS[GS[k].nametok].slen == fl &&
                        !memcmp(TOKS[GS[k].nametok].s, fname, (size_t)fl)) { di = k; break; }
                int ok = di >= 0 && GS[di].nparams == g->nparams;
                if (ok) {                 /* the signature: `( name : S < P1, P2 > )` */
                    GStruct* d = &GS[di];
                    int t = d->angleclose + 1;
                    ok = TOKS[t].k == T_LP && TOKS[t + 1].k == T_IDENT && TOKS[t + 2].k == T_COLON &&
                         tokspan_eq(TOKS[t + 3].k == T_IDENT ? t + 3 : t, g->nametok) && TOKS[t + 4].k == T_LT;
                    t += 5;
                    for (int q = 0; ok && q < d->nparams; q++) {
                        ok = TOKS[t].k == T_IDENT && tokspan_eq(d->ptok[q], t);
                        t++;
                        if (ok && q + 1 < d->nparams) { ok = TOKS[t].k == T_COMMA; t++; }
                    }
                    ok = ok && TOKS[t].k == T_GT && TOKS[t + 1].k == T_RP;
                }
                if (!ok)
                    die("%s:%d: #[drop(%s)] on generic '%.*s' needs a generic drop function taking the template — fn %s<%.*s>(v: %.*s<%.*s>) (M6.4d)",
                        FILENAME, TOKS[g->dropfn].line, fname, TOKS[g->nametok].slen, TOKS[g->nametok].s,
                        fname, TOKS[g->ptok[0]].slen, TOKS[g->ptok[0]].s, TOKS[g->nametok].slen, TOKS[g->nametok].s,
                        TOKS[g->ptok[0]].slen, TOKS[g->ptok[0]].s);
                g->dropgi = di;
            }
            continue;
        }
        for (int t = g->angleclose + 1; t <= g->lasttok; t++) {
            if (TOKS[t].k != T_IDENT) continue;
            int isp = 0;
            for (int q = 0; q < g->nparams; q++)
                if (tokspan_eq(g->ptok[q], t)) { isp = 1; break; }
            if (!isp) continue;
            if (typaram_at(g, t) < 0)
                die("%s:%d: type parameter '%.*s' appears in a position the lowering "
                    "does not rewrite (a type slot after ':' / '->' / '*' / 'as', or a "
                    "generic's argument)", FILENAME, TOKS[t].line, TOKS[t].slen, TOKS[t].s);
        }
    }
}

/* The concrete N function or enum for one instantiation: a span-splice of the
 * source declaration with the header rewritten to the mangled name, every
 * type-parameter type slot replaced by its argument, and every generic use
 * inside the template replaced by the concrete name it denotes for this
 * instantiation — the body's other bytes (spacing and comments) pass through
 * untouched. */
static char* concrete_fn(Inst* it) {
    GStruct* g = &GS[it->gi];
    Edit ed[512]; int ne = 0;
    ed[ne].start = TOKS[g->nametok].start;    /* `NAME<params>` -> mangled name */
    ed[ne].end = TOKS[g->angleclose].end;
    ed[ne].repl = mangle(it); ne++;
    for (int u = 0; u < g->nnested; u++) {    /* `Box<T>` -> `__g_Box_i64` (M6.3g) */
        Inst n;
        nested_inst(it, &g->nested[u], &n);
        if (ne >= 512) die("%s: generic item too large", FILENAME);
        ed[ne].start = g->nested[u].start;
        ed[ne].end = g->nested[u].end;
        ed[ne].repl = mangle(&n); ne++;
    }
    for (int t = g->angleclose + 1; t <= g->lasttok; t++) {
        int q = typaram_at(g, t);
        if (q < 0) continue;
        int covered = 0;                      /* an argument of a nested use: its edit covers it */
        for (int u = 0; u < g->nnested; u++)
            if (TOKS[t].start >= g->nested[u].start && TOKS[t].start < g->nested[u].end)
                covered = 1;
        if (covered) continue;
        if (ne >= 512) die("%s: generic item too large", FILENAME);
        ed[ne].start = TOKS[t].start;
        ed[ne].end = TOKS[t].end;
        ed[ne].repl = xstrndup(it->aname[q], (size_t)it->alen[q]); ne++;
    }
    qsort(ed, (size_t)ne, sizeof(Edit), edit_cmp);
    char* out = xmalloc((size_t)(g->declend - g->declstart) + 512 + (size_t)ne * 64);
    int n = 0, prev = g->declstart;
    for (int e = 0; e < ne; e++) {
        memcpy(out + n, SRC + prev, (size_t)(ed[e].start - prev)); n += ed[e].start - prev;
        n += sprintf(out + n, "%s", ed[e].repl);
        prev = ed[e].end;
    }
    memcpy(out + n, SRC + prev, (size_t)(g->declend - prev)); n += g->declend - prev;
    out[n] = 0;
    return out;
}

static char* concrete_item(Inst* it) {
    return GS[it->gi].kind == 0 ? concrete_struct(it) : concrete_fn(it);
}

static int args_same(Inst* a, Inst* b) {
    if (a->gi != b->gi || a->nargs != b->nargs) return 0;
    for (int i = 0; i < a->nargs; i++)
        if (a->alen[i] != b->alen[i] || memcmp(a->aname[i], b->aname[i], (size_t)a->alen[i]))
            return 0;
    return 1;
}

static int edit_cmp(const void* x, const void* y) {
    return ((const Edit*)x)->start - ((const Edit*)y)->start;
}

/* Nested uses (M6.3g), to a fixpoint: every concrete instantiation of a
 * template instantiates the generics its body uses, with the template's
 * arguments substituted — and those instantiations may use others in turn. */
static void expand_nested(void) {
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 0; i < NINST; i++) {
            GStruct* g = &GS[INSTS[i].gi];
            for (int u = 0; u < g->nnested; u++) {
                Inst n;
                nested_inst(&INSTS[i], &g->nested[u], &n);
                int dup = 0;
                for (int k = 0; k < NINST; k++)
                    if (args_same(&n, &INSTS[k])) { dup = 1; break; }
                if (dup) continue;
                if (NINST >= MAXI) die("%s: too many generic instantiations", FILENAME);
                INSTS[NINST++] = n;
                changed = 1;
            }
            if (g->dropgi >= 0) {         /* a template's destructor (M6.4d): each instantiation
                                           * of the struct brings the matching instantiation of
                                           * its drop function into being, use site or not */
                Inst n = INSTS[i];
                n.gi = g->dropgi;
                n.start = n.end = -1;
                int dup = 0;
                for (int k = 0; k < NINST; k++)
                    if (args_same(&n, &INSTS[k])) { dup = 1; break; }
                if (!dup) {
                    if (NINST >= MAXI) die("%s: too many generic instantiations", FILENAME);
                    INSTS[NINST++] = n;
                    changed = 1;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* modules (M6.5): `use "file.npp";` and `pub`                         */
/*                                                                    */
/* A module is a file. A top-level `use "path";` (the path is relative */
/* to the using file) inlines that file's resolved text in its place — */
/* once per program: a second `use` of the same file leaves only a     */
/* marker comment — and a file still being resolved (a cycle) is       */
/* refused. Resolution runs BEFORE the generic pass, over the combined  */
/* text, so a generic declared in one file and instantiated in another  */
/* monomorphizes and dedups exactly as it would in a single file. `pub` */
/* marks an exported item and is dropped from the lowering; enforcing   */
/* visibility across modules is the next rung.                          */
/* ------------------------------------------------------------------ */
#define MAXUSE 64
static const char* USED[MAXUSE]; static int NUSED;    /* files already inlined */
static const char* CHAIN[MAXUSE]; static int NCHAIN;  /* the use chain being resolved */

/* Where each inlined module landed in the resolved program (byte range),
 * for the visibility pass: an item is visible in its own file, `pub` makes
 * it visible program-wide, and a module's private items are renamed so two
 * modules' privates never meet at the C level. */
typedef struct { const char* name; int id; int start, end; } Mod;
static Mod MODS[MAXUSE]; static int NMOD;

/* The use-graph: one edge per `use` directive, from the using file to the
 * used one, both by file id — the main file is 0, every other file the
 * index it was first inlined under (its USED slot). A `pub` item is visible
 * to the files that use its module directly. */
#define MAXEDGE 256
static int EFROM[MAXEDGE], ETO[MAXEDGE], EPUB[MAXEDGE]; static int NEDGE;

static void add_edge(int from, int to, int pub) {
    if (NEDGE >= MAXEDGE) die("nppc: too many use directives");
    EFROM[NEDGE] = from;
    ETO[NEDGE] = to;
    EPUB[NEDGE] = pub;                    /* a `pub use`: re-exported (M6.5d) */
    NEDGE++;
}

/* Does module r re-export module `to` — through a `pub use`, directly or
 * along a chain of them (M6.5d)? Cycles were refused at resolution; the
 * depth cap is belt and braces. */
static int reexports(int r, int to, int depth) {
    if (depth > 64) return 0;
    for (int e = 0; e < NEDGE; e++)
        if (EFROM[e] == r && EPUB[e] && (ETO[e] == to || reexports(ETO[e], to, depth + 1)))
            return 1;
    return 0;
}

/* Is module `to` visible from file `from`: used directly, or re-exported by
 * a module `from` uses directly? A plain `use` inside a module exports
 * nothing onward. */
static int visible(int from, int to) {
    for (int e = 0; e < NEDGE; e++)
        if (EFROM[e] == from && (ETO[e] == to || reexports(ETO[e], to, 0))) return 1;
    return 0;
}

static char* read_file(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return NULL; }
    fclose(f);
    buf[sz] = 0;
    *out_len = sz;
    return buf;
}

/* Module identity is lexical: collapse separators, '.' and cancellable '..'
 * components before opening a file or comparing it with USED/CHAIN. Leading
 * '..' in a relative path must survive; an absolute path cannot climb above
 * its root. This does not resolve symlinks or equate relative/absolute paths. */
static char* normalize_path(const char* path) {
    size_t len = strlen(path), n = 0, depth = 0;
    char* out = xmalloc(len + 2);
    size_t* starts = xmalloc((len + 1) * sizeof(size_t));
    const char* p = path;
    int rooted = *p == '/';
    if (rooted) out[n++] = '/';
    while (*p) {
        while (*p == '/') p++;
        const char* part = p;
        while (*p && *p != '/') p++;
        size_t plen = (size_t)(p - part);
        if (!plen || (plen == 1 && part[0] == '.')) continue;
        if (plen == 2 && part[0] == '.' && part[1] == '.') {
            if (depth && !(n - starts[depth - 1] == 2 &&
                           out[starts[depth - 1]] == '.' && out[starts[depth - 1] + 1] == '.')) {
                n = starts[--depth];
                if (n && out[n - 1] == '/' && !(rooted && n == 1)) n--;
                continue;
            }
            if (rooted) continue;
        }
        if (n && out[n - 1] != '/') out[n++] = '/';
        starts[depth++] = n;
        memcpy(out + n, part, plen);
        n += plen;
    }
    if (!n) out[n++] = '.';
    out[n] = 0;
    free(starts);
    return out;
}

/* `rel` is based at the using file's directory, or at '/' if absolute.
 * Both the returned path and the initial source path use the same identity. */
static char* join_path(const char* from, const char* rel) {
    if (*rel == '/') return normalize_path(rel);
    int dl = 0;
    for (int i = 0; from[i]; i++)
        if (from[i] == '/' || from[i] == '\\') dl = i + 1;
    char* out = xmalloc((size_t)dl + strlen(rel) + 1);
    memcpy(out, from, (size_t)dl);
    strcpy(out + dl, rel);
    char* normalized = normalize_path(out);
    free(out);
    return normalized;
}

static void app(char** b, size_t* n, size_t* cap, const char* s, size_t l) {
    if (*n + l + 1 > *cap) {
        while (*n + l + 1 > *cap) *cap *= 2;
        *b = realloc(*b, *cap);
        if (!*b) die("nppc: out of memory");
    }
    memcpy(*b + *n, s, l);
    *n += l;
    (*b)[*n] = 0;
}

static char* resolve_file(const char* path, const char* text, long len);

/* The text of one file with every top-level `use` directive replaced by
 * the used file's resolved text. */
static char* resolve_text(const char* path, const char* text, long len, int myid) {
    const char* saved = FILENAME;
    FILENAME = path;
    lex_text(text, len);
    /* Collect the directives first: resolving one re-lexes another file. */
    int us[MAXUSE], ue[MAXUSE], uline[MAXUSE], upub[MAXUSE]; char* upath[MAXUSE];
    int nu = 0, depth = 0;
    for (int i = 0; i + 2 < NTOK; i++) {
        if (TOKS[i].k == T_LB) depth++;
        else if (TOKS[i].k == T_RB) depth--;
        if (depth != 0 || TOKS[i].k != T_IDENT || TOKS[i].slen != 3 ||
            memcmp(TOKS[i].s, "use", 3)) continue;
        if (TOKS[i + 1].k != T_STR || TOKS[i + 2].k != T_SEMI)
            die("%s:%d: expected use \"file.npp\";", FILENAME, TOKS[i].line);
        if (nu >= MAXUSE) die("%s: too many use directives", FILENAME);
        /* `pub use "file";` re-exports the module (M6.5d); the directive then
         * starts at the `pub`, which is consumed here with it. */
        upub[nu] = i > 0 && TOKS[i - 1].k == T_IDENT && TOKS[i - 1].slen == 3 &&
                   !memcmp(TOKS[i - 1].s, "pub", 3);
        us[nu] = upub[nu] ? TOKS[i - 1].start : TOKS[i].start;
        ue[nu] = TOKS[i + 2].end;
        uline[nu] = TOKS[i].line;
        upath[nu] = xstrndup(TOKS[i + 1].s, (size_t)TOKS[i + 1].slen);
        nu++;
    }
    FILENAME = saved;
    if (nu == 0) return xstrndup(text, (size_t)len);
    /* Splice: verbatim between directives, the used file's text at each. */
    size_t cap = (size_t)len + 1024, n = 0;
    char* out = xmalloc(cap);
    int prev = 0;
    char mark[600];
    for (int u = 0; u < nu; u++) {
        app(&out, &n, &cap, text + prev, (size_t)(us[u] - prev));
        char* full = join_path(path, upath[u]);
        /* A file still on the chain is a cycle (checked before the
         * once-per-program rule, which would otherwise mask it). */
        for (int k = 0; k < NCHAIN; k++)
            if (!strcmp(CHAIN[k], full))
                die("%s:%d: cyclic use of \"%s\"", path, uline[u], upath[u]);
        int seen = -1;
        for (int k = 0; k < NUSED; k++)
            if (!strcmp(USED[k], full)) seen = k;
        if (seen >= 0) {
            add_edge(myid, seen, upub[u]);
            snprintf(mark, sizeof mark, "// use \"%s\" (already inlined)", upath[u]);
            app(&out, &n, &cap, mark, strlen(mark));
        } else {
            long ilen;
            char* itext = read_file(full, &ilen);
            if (!itext)
                die("%s:%d: cannot open \"%s\" (looked for %s)", path, uline[u], upath[u], full);
            int before = NMOD, cid = NUSED;   /* the id resolve_file will assign */
            add_edge(myid, cid, upub[u]);
            char* inner = resolve_file(full, itext, ilen);
            snprintf(mark, sizeof mark, "// use \"%s\" (inlined by nppc)\n", upath[u]);
            app(&out, &n, &cap, mark, strlen(mark));
            int at = (int)n;
            app(&out, &n, &cap, inner, strlen(inner));
            /* Modules inlined inside `inner` were recorded relative to it;
             * shift them to this text, then record `inner` itself. */
            for (int k = before; k < NMOD; k++) { MODS[k].start += at; MODS[k].end += at; }
            if (NMOD >= MAXUSE) die("nppc: too many modules");
            MODS[NMOD].name = upath[u];
            MODS[NMOD].id = cid;
            MODS[NMOD].start = at;
            MODS[NMOD].end = at + (int)strlen(inner);
            NMOD++;
            if (n > 0 && out[n - 1] != '\n') app(&out, &n, &cap, "\n", 1);
            snprintf(mark, sizeof mark, "// end of \"%s\"", upath[u]);
            app(&out, &n, &cap, mark, strlen(mark));
        }
        prev = ue[u];
    }
    app(&out, &n, &cap, text + prev, (size_t)(len - prev));
    return out;
}

/* Resolve one file: it counts as inlined from here on, and sits on the
 * chain while its own uses resolve (that is what a cycle runs into). */
static char* resolve_file(const char* path, const char* text, long len) {
    if (NUSED >= MAXUSE || NCHAIN >= MAXUSE) die("nppc: too many modules");
    int id = NUSED;                        /* the main file is 0 */
    USED[NUSED++] = path;
    CHAIN[NCHAIN++] = path;
    char* out = resolve_text(path, text, len, id);
    NCHAIN--;
    return out;
}

/* The innermost module whose text contains byte `pos`, or -1 for the main
 * file. */
static int mod_owner(int pos) {
    int best = -1;
    for (int m = 0; m < NMOD; m++)
        if (pos >= MODS[m].start && pos < MODS[m].end &&
            (best < 0 || MODS[m].end - MODS[m].start < MODS[best].end - MODS[best].start))
            best = m;
    return best;
}

/* "modlib" for "dir/modlib.npp": the basename without its extension, any
 * other character replaced by '_', so it can sit inside an identifier. */
static char* mod_stem(const char* name) {
    const char* b = name;
    for (const char* p = name; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    char* out = xmalloc(strlen(b) + 1);
    int n = 0;
    for (const char* p = b; *p && *p != '.'; p++)
        out[n++] = (is_idc(*p) || is_dg(*p)) ? *p : '_';
    out[n] = 0;
    return out;
}

/* The file id is assigned in import order, independently of its basename.
 * A readable stem alone is not unique: a/util.npp and b/util.npp (or
 * a-b.npp and a_b.npp) would otherwise give their private items one name.
 * Use the same mangling for ordinary references and #[drop] attributes. */
static char* private_name(int owner, const char* name, int len) {
    char* stem = mod_stem(MODS[owner].name);
    char* out = xmalloc(strlen(stem) + (size_t)len + 32);
    sprintf(out, "__m_%d_%s_%.*s", MODS[owner].id, stem, len, name);
    free(stem);
    return out;
}

/* Is IDENT token t shaped like a reference to a top-level item — a call
 * `x(`, a construction or generic use `x.` / `x{` / `x<`, a type slot after
 * ':' / '->' / '*' / 'as', or the type of an `impl`? A field or method
 * after '.' is not, nor is a binding or parameter name. */
static int names_item(int t) {
    if (TOKS[t].k != T_IDENT) return 0;
    TK p = t > 0 ? TOKS[t - 1].k : T_EOF, nx = TOKS[t + 1].k;
    if (p == T_DOT) return 0;
    if (nx == T_LP || nx == T_DOT || nx == T_LB || nx == T_LT) return 1;
    if (p == T_COLON || p == T_ARROW || p == T_STAR || p == T_KW_AS || p == T_KW_IMPL) return 1;
    /* A type argument — `Box<P>`, `Pair<A, B>`: walk back over the argument
     * list to the `<` and require a generic's name in front of it, so a
     * comparison `a < b` is not mistaken for one. */
    if ((p == T_LT || p == T_COMMA) && (nx == T_GT || nx == T_COMMA)) {
        int j = t - 1;
        while (j > 0 && (TOKS[j].k == T_COMMA || TOKS[j].k == T_IDENT)) j--;
        if (TOKS[j].k == T_LT && j > 0 && gfind(j - 1) >= 0) return 1;
    }
    return 0;
}

/* Visibility (M6.5b), over the resolved program before the generic pass.
 * An item — a top-level fn, struct, or enum — is visible in the file that
 * declares it; `pub` makes it visible program-wide. A name-shaped
 * reference (names_item) from another file to a non-pub item is refused.
 * Every non-pub item of a MODULE is then renamed `__m_<id>_<stem>_<name>` at
 * its declaration and every reference inside the module, so two modules'
 * private `helper`s stay two functions in the lowered N. The renaming is a
 * text splice: the generic pass runs on the result and sees the mangled
 * names as ordinary ones (a private generic, or a private type as a type
 * argument, works unchanged). Returns the renamed program, or `prog` itself
 * when nothing needed renaming. */
#define MAXITEMS 1024
static struct { int tok; int owner; int pub; } ITEMS[MAXITEMS];
static int NITEMS;

static char* modules_pass(const char* prog) {
    if (NMOD == 0) return (char*)prog;
    /* The generic declarations are needed to tell a type argument from a
     * comparison (names_item); the generic pass re-collects them after the
     * re-lex, so the table is emptied again before returning. */
    collect_generic_decls();
    collect_generic_fns();
    collect_generic_enums();
    /* Every file's top-level items, with their file and `pub` flag. */
    NITEMS = 0;
    int depth = 0;
    for (int i = 0; i + 1 < NTOK; i++) {
        if (TOKS[i].k == T_LB) depth++;
        else if (TOKS[i].k == T_RB) depth--;
        if (depth != 0) continue;
        if (TOKS[i].k != T_KW_FN && TOKS[i].k != T_KW_STRUCT && TOKS[i].k != T_KW_ENUM) continue;
        if (TOKS[i + 1].k != T_IDENT) continue;
        if (NITEMS >= MAXITEMS) die("%s: too many top-level items", FILENAME);
        ITEMS[NITEMS].tok = i + 1;
        ITEMS[NITEMS].owner = mod_owner(TOKS[i].start);
        int p = i - 1;                    /* `pub` may stand before `own` and an attribute —
                                           * `pub #[drop(f)] own struct S` (M6.5e) */
        while (p > 0 && (TOKS[p].k == T_KW_OWN || TOKS[p].k == T_ATTR_DROP || TOKS[p].k == T_ATTR_CAPS_SYSCALL)) p--;
        ITEMS[NITEMS].pub = p >= 0 && TOKS[p].k == T_IDENT && TOKS[p].slen == 3 &&
                            !memcmp(TOKS[p].s, "pub", 3);
        NITEMS++;
    }
    /* References: refuse a cross-file use of a private item; rename a
     * module's own private items. */
    int es[MAXITEMS * 4], ee[MAXITEMS * 4]; char* er[MAXITEMS * 4]; int ne = 0;
    depth = 0;
    for (int t = 0; t + 1 < NTOK; t++) {
        if (TOKS[t].k == T_LB) depth++;
        else if (TOKS[t].k == T_RB) depth--;
        if (TOKS[t].k == T_ATTR_DROP) {   /* `#[drop(f)]` names an item too (M6.5e): a private
                                           * f of this module is renamed inside the attribute
                                           * (the name sits between `#[drop(` and `)]`), and
                                           * another module's private f is refused */
            int aowner = mod_owner(TOKS[t].start);
            const char* fname = TOKS[t].s;
            int fl = (int)strlen(fname);
            int alocal = -1, aforeign = -1;
            for (int k = 0; k < NITEMS; k++) {
                if (TOKS[ITEMS[k].tok].slen != fl || memcmp(TOKS[ITEMS[k].tok].s, fname, (size_t)fl)) continue;
                if (ITEMS[k].owner == aowner) alocal = k;
                else if (!ITEMS[k].pub && aforeign < 0) aforeign = k;
            }
            if (alocal < 0 && aforeign >= 0) {
                int fo = ITEMS[aforeign].owner;
                die("%s:%d: '%s' is private to %s (mark it pub to use it here)",
                    FILENAME, TOKS[t].line, fname, fo >= 0 ? MODS[fo].name : FILENAME);
            }
            if (alocal >= 0 && !ITEMS[alocal].pub && aowner >= 0) {
                if (ne >= MAXITEMS * 4) die("%s: too many private references", FILENAME);
                char* mn = private_name(aowner, fname, fl);
                es[ne] = TOKS[t].start + 7; ee[ne] = TOKS[t].end - 2; er[ne] = mn; ne++;
            }
            continue;
        }
        if (TOKS[t].k != T_IDENT) continue;
        int isdecl = t > 0 && (TOKS[t - 1].k == T_KW_FN || TOKS[t - 1].k == T_KW_STRUCT ||
                               TOKS[t - 1].k == T_KW_ENUM);
        if (isdecl && depth != 0) continue;          /* a method: not an item */
        if (!isdecl && !names_item(t)) continue;
        int owner = mod_owner(TOKS[t].start);
        int sid = owner < 0 ? 0 : MODS[owner].id;
        /* Same-named items elsewhere: a private one, an exported one this
         * file may see (its module is used here), or an exported one it may
         * not. */
        int local = -1, foreign = -1, pubk = -1, pubok = 0;
        for (int k = 0; k < NITEMS; k++) {
            if (!tokspan_eq(ITEMS[k].tok, t)) continue;
            int ko = ITEMS[k].owner;
            if (ko == owner) local = k;
            else if (!ITEMS[k].pub) { if (foreign < 0) foreign = k; }
            else if (visible(sid, ko < 0 ? 0 : MODS[ko].id)) pubok = 1;
            else if (pubk < 0) pubk = k;
        }
        if (local < 0) {
            if (isdecl || pubok) continue;
            if (pubk >= 0) {
                int po = ITEMS[pubk].owner;
                die("%s:%d: '%.*s' is declared by %s, which %s does not use (add use \"%s\";)",
                    FILENAME, TOKS[t].line, TOKS[t].slen, TOKS[t].s,
                    po >= 0 ? MODS[po].name : FILENAME,
                    owner < 0 ? FILENAME : MODS[owner].name,
                    po >= 0 ? MODS[po].name : FILENAME);
            }
            if (foreign >= 0) {
                int fo = ITEMS[foreign].owner;
                die("%s:%d: '%.*s' is private to %s (mark it pub to use it here)",
                    FILENAME, TOKS[t].line, TOKS[t].slen, TOKS[t].s,
                    fo >= 0 ? MODS[fo].name : FILENAME);
            }
            continue;
        }
        if (ITEMS[local].pub || owner < 0) continue;  /* exported, or the main file's own */
        if (ne >= MAXITEMS * 4) die("%s: too many private references", FILENAME);
        char* mn = private_name(owner, TOKS[t].s, TOKS[t].slen);
        es[ne] = TOKS[t].start; ee[ne] = TOKS[t].end; er[ne] = mn; ne++;
    }
    NGS = 0;
    if (ne == 0) return (char*)prog;
    size_t cap = strlen(prog) + (size_t)ne * 64 + 1, n = 0;
    char* out = xmalloc(cap);
    int prev = 0;
    for (int e = 0; e < ne; e++) {      /* in token order already */
        app(&out, &n, &cap, prog + prev, (size_t)(es[e] - prev));
        app(&out, &n, &cap, er[e], strlen(er[e]));
        prev = ee[e];
    }
    app(&out, &n, &cap, prog + prev, strlen(prog) - (size_t)prev);
    return out;
}

/* `pub` marks an exported item (design §6.2). N has no visibility, so the
 * lowering drops the marker together with the spacing after it. */
static void strip_pub(void) {
    int depth = 0;
    for (int i = 0; i + 1 < NTOK; i++) {
        if (TOKS[i].k == T_LB) depth++;
        else if (TOKS[i].k == T_RB) depth--;
        if (depth != 0 || TOKS[i].k != T_IDENT || TOKS[i].slen != 3 ||
            memcmp(TOKS[i].s, "pub", 3)) continue;
        int j = i + 1;                    /* over `own` and attributes: `pub #[drop(f)] own
                                           * struct S`, `pub #[caps(syscall)] fn f` (M6.5e) */
        while (TOKS[j].k == T_KW_OWN || TOKS[j].k == T_ATTR_DROP || TOKS[j].k == T_ATTR_CAPS_SYSCALL) j++;
        TK nx = TOKS[j].k;
        if (nx != T_KW_FN && nx != T_KW_STRUCT && nx != T_KW_ENUM && nx != T_KW_IMPL)
            die("%s:%d: pub must precede fn, struct, enum, or impl", FILENAME, TOKS[i].line);
        if (NEDIT >= MAXG + MAXI) die("%s: too many rewrites", FILENAME);
        EDITS[NEDIT].start = TOKS[i].start;
        EDITS[NEDIT].end = TOKS[i + 1].start;
        EDITS[NEDIT].repl = "";
        NEDIT++;
    }
}

/* ------------------------------------------------------------------ */
/* the closure pass (M6.4a): lambda lifting, non-capturing              */
/*                                                                    */
/* `fn(x: i64) -> i64 { x * x }` in a VALUE position — a call argument, */
/* a struct-literal field, a binding's right-hand side, a return — is  */
/* lifted to a top-level `fn __c_N(x: i64) -> i64 { x * x }` placed     */
/* right after the item before the one it appears in, and the          */
/* expression becomes the name `__c_N`: a function value, typed by N   */
/* v0.24's function types exactly like a named function. The body is   */
/* spliced through verbatim. Lambdas capture nothing at this rung: a   */
/* body may use its parameters and the program's items, never a local  */
/* of the enclosing function (refused here when the local is visible   */
/* to a token scan; otherwise N refuses the lifted function). The pass */
/* is a text splice run to a fixpoint, innermost lambdas first, so a   */
/* lambda inside a lambda lifts before the one around it copies its    */
/* text. A lambda inside a generic template waits for M6.4b.          */
/* ------------------------------------------------------------------ */
static int NLAMBDA;                       /* lifted so far: the __c_N counter */

static int match_brace(int b) {           /* token of the `}` closing `{` at b */
    int depth = 0;
    for (int t = b; TOKS[t].k != T_EOF; t++) {
        if (TOKS[t].k == T_LB) depth++;
        else if (TOKS[t].k == T_RB && --depth == 0) return t;
    }
    die("%s:%d: unbalanced braces", FILENAME, TOKS[b].line);
    return -1;
}

/* Skip one type starting at token t — `#[user]? raw? *… NAME<args>?` or a
 * function type `fn(A, B) -> R` — and return the token after it (-1 if
 * the tokens do not shape a type). */
static int skip_type(int t) {
    if (TOKS[t].k == T_KW_OWN) t++;
    if (TOKS[t].k == T_ATTR_USER) t++;
    if (TOKS[t].k == T_KW_RAW) t++;
    while (TOKS[t].k == T_STAR) t++;
    if (TOKS[t].k == T_KW_FN) {
        if (TOKS[t + 1].k != T_LP) return -1;
        int depth = 0;
        for (t++; TOKS[t].k != T_EOF; t++) {
            if (TOKS[t].k == T_LP) depth++;
            else if (TOKS[t].k == T_RP && --depth == 0) { t++; break; }
        }
        if (TOKS[t].k == T_ARROW) return skip_type(t + 1);
        return t;
    }
    if (TOKS[t].k != T_IDENT) return -1;
    if (fn_word(t)) {
        int depth = 0;                    /* Fn(A, B) -> R / FnOnce(A, B) -> R: the closure types (M6.4c) */
        for (t++; TOKS[t].k != T_EOF; t++) {
            if (TOKS[t].k == T_LP) depth++;
            else if (TOKS[t].k == T_RP && --depth == 0) { t++; break; }
        }
        if (TOKS[t].k == T_ARROW) return skip_type(t + 1);
        return t;
    }
    t++;
    if (TOKS[t].k == T_LT) {              /* generic arguments */
        int depth = 0;
        for (; TOKS[t].k != T_EOF; t++) {
            if (TOKS[t].k == T_LT) depth++;
            else if (TOKS[t].k == T_GT && --depth == 0) { t++; break; }
        }
    }
    return t;
}

/* Is token i the `fn` of a lambda? A lambda reads `fn ( params ) [-> type]
 * {` in a value position; a function TYPE reads `fn ( types ) [-> type]`
 * in a type slot (after `->`, `as`, `*`, `raw`, `#[user]`) or with no body
 * behind it (a parameter or field declaration continues with `,` `)`
 * `}`). Returns the token of the body's `{`, or -1. */
static int lambda_body(int i) {
    if (TOKS[i].k != T_KW_FN || TOKS[i + 1].k != T_LP) return -1;
    TK p = i > 0 ? TOKS[i - 1].k : T_EOF;
    if (p == T_ARROW || p == T_KW_AS || p == T_STAR || p == T_KW_RAW || p == T_ATTR_USER)
        return -1;
    int t = i + 1, depth = 0;
    for (; TOKS[t].k != T_EOF; t++) {
        if (TOKS[t].k == T_LP) depth++;
        else if (TOKS[t].k == T_RP && --depth == 0) { t++; break; }
    }
    if (TOKS[t].k == T_ARROW) { t = skip_type(t + 1); if (t < 0) return -1; }
    return TOKS[t].k == T_LB ? t : -1;
}

/* Names bound by a binding form at token t: `x :=`, `mut x :=`, `for x in`,
 * and — inside a signature's parentheses — `x :`. Returns the IDENT or -1. */
static int binds_at(int t, int insig) {
    if (TOKS[t].k != T_IDENT) return -1;
    TK nx = TOKS[t + 1].k, p = t > 0 ? TOKS[t - 1].k : T_EOF;
    if (nx == T_WALRUS) return t;
    if (p == T_KW_FOR && nx == T_KW_IN) return t;
    if (insig && nx == T_COLON && p != T_DOT) return t;
    return -1;
}

/* The references a lambda makes to locals of the enclosing function: the
 * function's parameters (its signature's `x :` names) and every binding
 * before the lambda, minus what the lambda binds itself. A token scan
 * sees these; what it cannot see (a match arm's binds) N refuses in the
 * lifted function as an undeclared variable. Every reference token is
 * returned in order (a name may appear several times); with `refuse` set
 * the first one is an error — a lambda in a plain `fn(...)` slot has no
 * environment to keep a capture in. */
static int scan_captures(int itemfirst, int lam, int body, int end, int* refs, int max, int refuse) {
    int locals[512], nl = 0;
    int t = itemfirst, insig = 0;
    for (; t < lam; t++) {                /* the item's header parentheses */
        if (TOKS[t].k == T_LP) insig++;
        else if (TOKS[t].k == T_RP) insig--;
        else if (TOKS[t].k == T_LB) break;
        int b = binds_at(t, insig > 0);
        if (b >= 0 && nl < 512) locals[nl++] = b;
    }
    for (; t < lam; t++) {                /* the body before the lambda */
        int b = binds_at(t, 0);
        if (b >= 0 && nl < 512) locals[nl++] = b;
    }
    int bound[256], nb = 0;               /* the lambda's own names */
    int depth = 0;
    for (int u = lam + 1; u <= end; u++) {
        if (TOKS[u].k == T_LP) depth++;
        else if (TOKS[u].k == T_RP) depth--;
        int b = binds_at(u, u < body && depth > 0);
        if (b >= 0 && nb < 256) bound[nb++] = b;
    }
    int nref = 0;
    for (int u = body + 1; u < end; u++) {
        if (TOKS[u].k != T_IDENT) continue;
        if (TOKS[u - 1].k == T_DOT || TOKS[u + 1].k == T_COLON) continue;   /* a field */
        int captured = 0;
        for (int k = 0; k < nl && !captured; k++) if (tokspan_eq(locals[k], u)) captured = 1;
        if (!captured) continue;
        for (int k = 0; k < nb; k++) if (tokspan_eq(bound[k], u)) { captured = 0; break; }
        if (!captured) continue;
        if (refuse)
            die("%s:%d: lambda captures '%.*s', a local of the enclosing function — only a lambda in a closure slot (an Fn type) can capture; pass it as a parameter",
                FILENAME, TOKS[u].line, TOKS[u].slen, TOKS[u].s);
        if (nref < max) refs[nref++] = u;
    }
    return nref;
}

/* The name of a generic item starting at token itemfirst, or -1 for a
 * plain item: `[pub] [#[...]] fn|struct|enum NAME <`. */
static int generic_item_name(int itemfirst) {
    int t = itemfirst;
    while (TOKS[t].k == T_ATTR_CAPS_SYSCALL || TOKS[t].k == T_ATTR_DROP ||
           (TOKS[t].k == T_IDENT && TOKS[t].slen == 3 && !memcmp(TOKS[t].s, "pub", 3)))
        t++;
    if (TOKS[t].k != T_KW_FN && TOKS[t].k != T_KW_STRUCT && TOKS[t].k != T_KW_ENUM) return -1;
    if (TOKS[t + 1].k == T_IDENT && TOKS[t + 2].k == T_LT) return t + 1;
    return -1;
}

/* ---- the closure type (M6.4c1): Fn(A, B) -> R ------------------------ */
/* N's `fn(A) -> R` is a bare function value; it has nowhere to keep an   */
/* environment, so a closure needs a type of its own. `Fn(A, B) -> R` is  */
/* that type: it lowers to one N struct per distinct signature —          */
/* `struct __Fn_i64__i64 { env: addr, call: fn(addr, i64) -> i64 }` — and  */
/* a call through a closure value `f(a)` becomes `f.call(f.env, a)`. A    */
/* lambda written where a closure is expected takes the environment as   */
/* its first parameter and the expression becomes the closure value       */
/* `__Fn_i64__i64{ env: 0, call: __c_N }`; a named function there gets an  */
/* adapter of the same shape. At this rung the environment is always 0:  */
/* lambdas still capture nothing (M6.4c2 fills the env). The struct name  */
/* is the signature's spelling with its punctuation as underscores.       */

static int is_Fn(int t) {                 /* the IDENT `Fn` or `FnOnce` opening a closure type */
    return fn_word(t);
}

/* The program's named functions and structs, by token scan: each with its
 * parameter (or field) names and type token ranges, and a function's
 * return type range. Rebuilt whenever the token buffer changes. */
#define MAXTAB 512
#define MAXTP 32
typedef struct {
    int name;
    int np; int pname[MAXTP], pts[MAXTP], pte[MAXTP];
    int rts, rte;                         /* return type, or -1 */
    int body, end;                        /* the `{` and its `}` */
    int ntp, tp[MAXP];                    /* a template's type-parameter tokens */
} Item;
static Item FNT[MAXTAB]; static int NFNT;
static Item STT[MAXTAB]; static int NSTT;

static int fnt_find(int t) {
    for (int i = 0; i < NFNT; i++) if (tokspan_eq(FNT[i].name, t)) return i;
    return -1;
}
static int stt_find(int t) {
    for (int i = 0; i < NSTT; i++) if (tokspan_eq(STT[i].name, t)) return i;
    return -1;
}
static int range_is_Fn(int ts, int te) { return ts >= 0 && ts < te && is_Fn(ts); }

static void build_tables(void) {
    NFNT = NSTT = 0;
    for (int i = 0; i + 2 < NTOK; i++) {
        if (TOKS[i].k == T_KW_FN && TOKS[i + 1].k == T_IDENT) {   /* a named fn (a lambda has `(` here) */
            Item* f = &FNT[NFNT];
            f->name = i + 1; f->np = 0; f->rts = f->rte = -1; f->ntp = 0;
            int j = i + 2;
            if (TOKS[j].k == T_LT) {
                for (j++; TOKS[j].k != T_GT && TOKS[j].k != T_EOF; j++)
                    if (TOKS[j].k == T_IDENT && f->ntp < MAXP) f->tp[f->ntp++] = j;
                j++;
            }
            if (TOKS[j].k != T_LP) continue;
            j++;
            while (TOKS[j].k != T_RP && TOKS[j].k != T_EOF) {
                if (TOKS[j].k != T_IDENT) break;
                if (TOKS[j + 1].k == T_COLON) {
                    int ts = j + 2, te = skip_type(ts);
                    if (te < 0) break;
                    if (f->np < MAXTP) { f->pname[f->np] = j; f->pts[f->np] = ts; f->pte[f->np] = te; f->np++; }
                    j = te;
                } else j++;               /* `self` */
                if (TOKS[j].k == T_COMMA) j++;
            }
            if (TOKS[j].k != T_RP) continue;
            j++;
            if (TOKS[j].k == T_ARROW) {
                f->rts = j + 1; f->rte = skip_type(j + 1);
                if (f->rte < 0) continue;
                j = f->rte;
            }
            if (TOKS[j].k != T_LB) continue;     /* an extern declaration: no body */
            f->body = j; f->end = match_brace(j);
            if (NFNT < MAXTAB) NFNT++;
        } else if (TOKS[i].k == T_KW_STRUCT && TOKS[i + 1].k == T_IDENT) {
            Item* s = &STT[NSTT];
            s->name = i + 1; s->np = 0; s->rts = s->rte = -1; s->ntp = 0;
            int j = i + 2;
            if (TOKS[j].k == T_LT) {
                for (j++; TOKS[j].k != T_GT && TOKS[j].k != T_EOF; j++)
                    if (TOKS[j].k == T_IDENT && s->ntp < MAXP) s->tp[s->ntp++] = j;
                j++;
            }
            if (TOKS[j].k != T_LB) continue;
            s->body = j; s->end = match_brace(j);
            for (j++; j < s->end; ) {
                if (TOKS[j].k != T_IDENT || TOKS[j + 1].k != T_COLON) break;
                int ts = j + 2, te = skip_type(ts);
                if (te < 0) break;
                if (s->np < MAXTP) { s->pname[s->np] = j; s->pts[s->np] = ts; s->pte[s->np] = te; s->np++; }
                j = te;
                if (TOKS[j].k == T_COMMA) j++;
            }
            if (NSTT < MAXTAB) NSTT++;
        }
    }
}

static void apps(char** b, size_t* n, size_t* cap, const char* s) { app(b, n, cap, s, strlen(s)); }

/* The struct name of a closure signature: `__` + the type's spelling with
 * every `( ) , -> <` as `_`, `*` as `p`, `#[user]` as `u`, and a generic
 * type inside spelled as its instantiation's name (`Box<i64>` as
 * `__g_Box_i64`, which is what the generic pass renames it to — so the
 * name is the same whichever closure pass meets it, M6.4c3c) — so
 * `Fn(i64) -> i64` is `__Fn_i64__i64`, `Fn()` is `__Fn__`, and
 * `Fn(*u8, Box<i64>) -> bool` is `__Fn_pu8___g_Box_i64__bool`. */
static char* sig_name(int ts, int te) {
    size_t cap = 256, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    apps(&b, &n, &cap, "__");
    for (int t = ts; t < te; t++) {
        TK k = TOKS[t].k;
        if (k == T_IDENT) {
            if (TOKS[t + 1].k == T_LT) apps(&b, &n, &cap, "__g_");
            app(&b, &n, &cap, TOKS[t].s, (size_t)TOKS[t].slen);
        }
        else if (k == T_STAR) apps(&b, &n, &cap, "p");
        else if (k == T_ATTR_USER) apps(&b, &n, &cap, "u");
        else if (k == T_KW_FN) apps(&b, &n, &cap, "fn");
        else if (k == T_LP || k == T_RP || k == T_COMMA || k == T_ARROW || k == T_LT) apps(&b, &n, &cap, "_");
    }
    return b;
}

/* The parameter type ranges and the return type range of the closure
 * signature at [t, e): `Fn ( A , B ) -> R`. */
static void sig_parts(int t, int e, int* pts, int* pte, int* np, int* rts, int* rte) {
    int depth = 0, cur = t + 2, u;
    *np = 0;
    for (u = t + 2; u < e; u++) {
        TK k = TOKS[u].k;
        if (k == T_LP || k == T_LT) depth++;
        else if (k == T_GT) depth--;
        else if (k == T_RP) { if (depth == 0) break; depth--; }
        else if (k == T_COMMA && depth == 0) {
            if (*np < MAXTP) { pts[*np] = cur; pte[*np] = u; (*np)++; }
            cur = u + 1;
        }
    }
    if (u > cur && *np < MAXTP) { pts[*np] = cur; pte[*np] = u; (*np)++; }
    u++;
    if (u < e && TOKS[u].k == T_ARROW) { *rts = u + 1; *rte = e; }
    else { *rts = -1; *rte = -1; }
}

/* A type range as N text — with `nest`, a nested closure type spelled by
 * its struct name; and, when a substitution is given, each of the `np`
 * type-parameter tokens `ptok` replaced by the matching argument token
 * `atok` (a generic call's explicit arguments standing in for the callee's
 * parameters). */
static void render_subst(int ts, int te, const int* ptok, int np, const int* atok, int nest,
                         char** b, size_t* n, size_t* cap) {
    for (int t = ts; t < te; t++) {
        if (nest && is_Fn(t)) {
            int e = skip_type(t);
            apps(b, n, cap, sig_name(t, e));
            t = e - 1;
            continue;
        }
        if (t > ts && (TOKS[t - 1].k == T_COMMA || TOKS[t - 1].k == T_ARROW || TOKS[t].k == T_ARROW ||
                       TOKS[t - 1].k == T_ATTR_USER || TOKS[t - 1].k == T_KW_RAW))
            apps(b, n, cap, " ");
        int q = 0;
        if (TOKS[t].k == T_IDENT) for (; q < np; q++) if (tokspan_eq(ptok[q], t)) break;
        if (TOKS[t].k == T_IDENT && q < np) app(b, n, cap, TOKS[atok[q]].s, (size_t)TOKS[atok[q]].slen);
        else app(b, n, cap, SRC + TOKS[t].start, (size_t)(TOKS[t].end - TOKS[t].start));
    }
}

static void render_type(int ts, int te, char** b, size_t* n, size_t* cap) {
    render_subst(ts, te, NULL, 0, NULL, 1, b, n, cap);
}

/* A closure slot's type, verbatim (nested closure types included: the
 * closure pass rewrites the whole span at once), substituted. */
static char* type_text(int ts, int te, const int* ptok, int np, const int* atok) {
    size_t cap = 64, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    render_subst(ts, te, ptok, np, atok, 0, &b, &n, &cap);
    return b;
}

/* The closure value literal: the closure's TYPE as written (`Fn(i64) ->
 * i64`, or `Fn(T) -> T` inside a template), an environment expression (`0`
 * when it captures nothing) and the lifted function. The closure pass
 * turns the type into its struct name — after the generic pass has
 * substituted any type parameter in it (M6.4c3). */
static char* closure_value(const char* ty, const char* env, const char* fname) {
    size_t cap = 256, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    apps(&b, &n, &cap, ty);
    apps(&b, &n, &cap, "{ env: ");
    apps(&b, &n, &cap, env);
    apps(&b, &n, &cap, ", call: ");
    apps(&b, &n, &cap, fname);
    apps(&b, &n, &cap, " }");
    return b;
}

/* The owning closure value (M6.4c6): the same, plus the finaliser that
 * ends its environment when the closure drops. */
static char* once_value(const char* ty, const char* env, const char* fname, const char* fin) {
    size_t cap = 256, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    apps(&b, &n, &cap, ty);
    apps(&b, &n, &cap, "{ env: ");
    apps(&b, &n, &cap, env);
    apps(&b, &n, &cap, ", call: ");
    apps(&b, &n, &cap, fname);
    apps(&b, &n, &cap, ", fin: ");
    apps(&b, &n, &cap, fin);
    apps(&b, &n, &cap, " }");
    return b;
}

/* The name before the `(` of a call or the `{` of a struct literal at
 * token open — `NAME(`, `NAME<A, B>(`, `S{`, `S<A>{` — as its IDENT token,
 * or -1. */
static int name_before(int open) {
    int j = open - 1;
    if (j > 0 && TOKS[j].k == T_GT) {
        while (j > 0 && (TOKS[j].k == T_GT || TOKS[j].k == T_IDENT || TOKS[j].k == T_COMMA)) j--;
        if (TOKS[j].k != T_LT) return -1;
        j--;
    }
    return j >= 0 && TOKS[j].k == T_IDENT ? j : -1;
}

/* The explicit type arguments after name token c (`c<A, B>`), as tokens. */
static int explicit_args(int c, int* atok) {
    int na = 0;
    if (TOKS[c + 1].k == T_LT)
        for (int a = c + 2; TOKS[a].k != T_GT && TOKS[a].k != T_EOF; a++)
            if (TOKS[a].k == T_IDENT && na < MAXP) atok[na++] = a;
    return na;
}

/* Does the IDENT token t occur as a whole word in text? */
static int word_in(const char* text, int t) {
    const char* s = text;
    int len = TOKS[t].slen;
    while ((s = strstr(s, TOKS[t].s)) != NULL) {
        int before = s == text || !(is_idc(s[-1]) || is_dg(s[-1]));
        int after = !(is_idc(s[len]) || is_dg(s[len]));
        if (before && after && !memcmp(s, TOKS[t].s, (size_t)len)) return 1;
        s++;
    }
    return 0;
}

/* ---- captures by value (M6.4c2) --------------------------------------- */
/* A lambda in a closure slot may name the enclosing function's locals.  */
/* They are captured BY VALUE when the closure is born: an environment    */
/* struct `__E_N` holds them, a generated maker copies them onto the bump */
/* heap (`sys_sbrk`) and returns the address the closure carries as its  */
/* `env`, and the lifted function reads them back as `__e.name`. The     */
/* closure may then outlive the frame it was born in. A captured local's */
/* type must be evident to a token scan — a declared parameter, a       */
/* literal, a call to a known function, a struct literal, or a name so  */
/* typed; anything else is refused with the fix named.                   */

/* The type text of local `name` of the item starting at itemfirst, as
 * seen just before token lam, or NULL when it is not evident. */
static char* local_type(int itemfirst, int lam, int name, int depth) {
    char* ty = NULL;
    int t = itemfirst, insig = 0;
    for (; t < lam; t++) {                /* a declared parameter */
        if (TOKS[t].k == T_LP) insig++;
        else if (TOKS[t].k == T_RP) insig--;
        else if (TOKS[t].k == T_LB) break;
        if (insig > 0 && TOKS[t].k == T_IDENT && TOKS[t + 1].k == T_COLON && tokspan_eq(t, name)) {
            int ts = t + 2, te = skip_type(ts);
            if (te > 0) {
                size_t cap = 64, n = 0;
                ty = xmalloc(cap);
                ty[0] = 0;
                render_type(ts, te, &ty, &n, &cap);
            }
        }
    }
    for (; t < lam; t++) {                /* bindings before the lambda; the last one wins */
        if (TOKS[t].k != T_IDENT || TOKS[t + 1].k != T_WALRUS || !tokspan_eq(t, name)) continue;
        int r = t + 2;
        TK k = TOKS[r].k;
        char* nt = NULL;
        if (k == T_INT) nt = "i64";
        else if (k == T_STR || k == T_STR_HEAD) nt = "str";
        else if (k == T_KW_TRUE || k == T_KW_FALSE) nt = "bool";
        else if (k == T_IDENT && TOKS[r + 1].k == T_LP) {
            int g = fnt_find(r);
            if (g >= 0 && FNT[g].rts >= 0) {
                size_t cap = 64, n = 0;
                char* b = xmalloc(cap);
                b[0] = 0;
                render_type(FNT[g].rts, FNT[g].rte, &b, &n, &cap);
                nt = b;
            }
        } else if (k == T_IDENT && TOKS[r + 1].k == T_LB) {
            if (stt_find(r) >= 0) nt = xstrndup(TOKS[r].s, (size_t)TOKS[r].slen);
        } else if (k == T_IDENT && TOKS[r + 1].k == T_LT && stt_find(r) >= 0) {
            int te = skip_type(r);        /* `x := S<A>{ … }`: a generic struct literal
                                           * types x as S<A> (the generic pass concretes it) */
            if (te > 0 && TOKS[te].k == T_LB) {
                size_t cap = 64, n = 0;
                char* b = xmalloc(cap);
                b[0] = 0;
                render_type(r, te, &b, &n, &cap);
                nt = b;
            }
        } else if (k == T_IDENT && TOKS[r + 1].k == T_SEMI && depth < 4) {
            nt = local_type(itemfirst, t, r, depth + 1);
        }
        ty = nt;
    }
    return ty;
}

/* The environment of a capturing lambda: the struct `__E_N` over the
 * captured names, the maker `__mk_E_N` that copies them onto the heap
 * (16 bytes per field, `str` being the widest value — a bump allocation,
 * never freed), and the birth expression `__mk_E_N(a, b)`. */
static char* env_text(int itemfirst, int lam, int* caps, int ncap, int lamno, char** birth, char** tys,
                      const char* targs, int own) {
    for (int c = 0; c < ncap; c++) {
        tys[c] = local_type(itemfirst, lam, caps[c], 0);
        if (!tys[c])
            die("%s:%d: cannot capture '%.*s': its type is not evident — bind it with a literal, a call or a struct literal, or pass it as a parameter",
                FILENAME, TOKS[caps[c]].line, TOKS[caps[c]].slen, TOKS[caps[c]].s);
    }
    char en[96], mk[96], sz[32];          /* inside a template, `__E_N<T>` / `__mk_E_N<T>` are templates too */
    sprintf(en, "__E_%d%s", lamno, targs);
    sprintf(mk, "__mk_E_%d%s", lamno, targs);
    sprintf(sz, "%d", 16 * ncap + 16);
    size_t cap = 256, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    if (own) apps(&b, &n, &cap, "own ");  /* M6.4c6: an own capture makes the environment own */
    apps(&b, &n, &cap, "struct ");
    apps(&b, &n, &cap, en);
    apps(&b, &n, &cap, " {\n");
    for (int c = 0; c < ncap; c++) {
        apps(&b, &n, &cap, "    ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
        apps(&b, &n, &cap, ": ");
        apps(&b, &n, &cap, tys[c]);
        apps(&b, &n, &cap, ",\n");
    }
    apps(&b, &n, &cap, "}\n\n#[caps(syscall)]\nfn ");
    apps(&b, &n, &cap, mk);
    apps(&b, &n, &cap, "(");
    for (int c = 0; c < ncap; c++) {
        if (c) apps(&b, &n, &cap, ", ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
        apps(&b, &n, &cap, ": ");
        apps(&b, &n, &cap, tys[c]);
    }
    apps(&b, &n, &cap, ") -> addr {\n    __m := sys_sbrk(");   /* `__m`: no captured name can clash */
    apps(&b, &n, &cap, sz);
    apps(&b, &n, &cap, ") as *");
    apps(&b, &n, &cap, en);
    apps(&b, &n, &cap, ";\n    __m[0] = ");
    apps(&b, &n, &cap, en);
    apps(&b, &n, &cap, "{ ");
    for (int c = 0; c < ncap; c++) {
        if (c) apps(&b, &n, &cap, ", ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
        apps(&b, &n, &cap, ": ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
    }
    apps(&b, &n, &cap, " };\n    __m as addr\n}");
    size_t bcap = 128, bn = 0;
    char* bt = xmalloc(bcap);
    bt[0] = 0;
    apps(&bt, &bn, &bcap, mk);
    apps(&bt, &bn, &bcap, "(");
    for (int c = 0; c < ncap; c++) {
        if (c) apps(&bt, &bn, &bcap, ", ");
        app(&bt, &bn, &bcap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
    }
    apps(&bt, &bn, &bcap, ")");
    *birth = bt;
    return b;
}

/* Is the type text an `own struct` of this program? (M6.4c4: an own
 * capture cannot live behind the heap environment's pointer.) */
static int is_own_type(const char* ty) {
    return is_own_span(ty, (int)strlen(ty));
}

/* The environment of a call-once closure (M6.4c4): an `own struct __E_N`
 * over the captured names — own fields among them, which N v0.25 allows
 * inside an own container — and the closure value's own struct
 * `__O_N { env: __E_N }`. No maker: the closure is built by value, in
 * place, and is consumed by its one call. */
static char* once_text(int* caps, int ncap, int lamno, char** tys, const char* targs) {
    char en[96], on[96];                  /* inside a template (M6.4c4b) both are templates
                                           * over the parameters the lambda or a capture names */
    sprintf(en, "__E_%d%s", lamno, targs);
    sprintf(on, "__O_%d%s", lamno, targs);
    size_t cap = 256, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    apps(&b, &n, &cap, "own struct ");
    apps(&b, &n, &cap, en);
    apps(&b, &n, &cap, " {\n");
    for (int c = 0; c < ncap; c++) {
        apps(&b, &n, &cap, "    ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
        apps(&b, &n, &cap, ": ");
        apps(&b, &n, &cap, tys[c]);
        apps(&b, &n, &cap, ",\n");
    }
    apps(&b, &n, &cap, "}\n\nown struct ");
    apps(&b, &n, &cap, on);
    apps(&b, &n, &cap, " {\n    env: ");
    apps(&b, &n, &cap, en);
    apps(&b, &n, &cap, ",\n}");
    return b;
}

/* Its birth: `__O_N{ env: __E_N{ a: a, b: b } }` — every captured own
 * local moves in, as N's struct-literal rule says. */
static char* once_birth(int* caps, int ncap, int lamno, const char* targs) {
    size_t cap = 128, n = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    char en[96], on[96];                  /* `__O_N<T>{ env: __E_N<T>{ … } }` inside a template:
                                           * nested generic uses the generic pass instantiates
                                           * with the enclosing function (M6.4c4b) */
    sprintf(en, "__E_%d%s", lamno, targs);
    sprintf(on, "__O_%d%s", lamno, targs);
    apps(&b, &n, &cap, on);
    apps(&b, &n, &cap, "{ env: ");
    apps(&b, &n, &cap, en);
    apps(&b, &n, &cap, "{ ");
    for (int c = 0; c < ncap; c++) {
        if (c) apps(&b, &n, &cap, ", ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
        apps(&b, &n, &cap, ": ");
        app(&b, &n, &cap, TOKS[caps[c]].s, (size_t)TOKS[caps[c]].slen);
    }
    apps(&b, &n, &cap, " } }");
    return b;
}

/* Does the program declare `sys_sbrk` (in an extern block)? */
static int declares_sbrk(void) {
    for (int t = 1; t + 1 < NTOK; t++)
        if (TOKS[t].k == T_IDENT && TOKS[t - 1].k == T_KW_FN && TOKS[t].slen == 8 &&
            !memcmp(TOKS[t].s, "sys_sbrk", 8)) return 1;
    return 0;
}

/* Does the lambda at token i (body `{`..`}` ending at token end) sit where
 * a closure is expected? A call argument whose parameter is Fn-typed (a
 * generic call's explicit type arguments substituted into it), a
 * struct-literal field of Fn type, or the `return` / tail value of a
 * function returning Fn. Yields the Fn type as text, or NULL. */
static char* fn_slot(int i, int end) {
    TK p = i > 0 ? TOKS[i - 1].k : T_EOF;
    if (p == T_LP || p == T_COMMA) {      /* an argument: which parameter? */
        int depth = 0, k = 0, j = i - 1;
        for (; j > 0; j--) {
            TK q = TOKS[j].k;
            if (q == T_RP || q == T_RB || q == T_RBRACK) depth++;
            else if (q == T_LB || q == T_LBRACK) { if (depth == 0) return NULL; depth--; }
            else if (q == T_LP) { if (depth == 0) break; depth--; }
            else if (q == T_COMMA && depth == 0) k++;
        }
        if (j <= 0) return NULL;
        int c = name_before(j);
        if (c < 0) return NULL;
        int f = fnt_find(c);
        if (f < 0 || k >= FNT[f].np || !range_is_Fn(FNT[f].pts[k], FNT[f].pte[k])) return NULL;
        int atok[MAXP], na = explicit_args(c, atok);   /* `NAME<A, B>(`: the arguments stand in */
        int np = na == FNT[f].ntp ? na : 0;
        return type_text(FNT[f].pts[k], FNT[f].pte[k], FNT[f].tp, np, atok);
    }
    if (p == T_COLON && i >= 2 && TOKS[i - 2].k == T_IDENT) {   /* a struct-literal field */
        int depth = 0, j = i - 3;
        for (; j > 0; j--) {
            TK q = TOKS[j].k;
            if (q == T_RB) depth++;
            else if (q == T_LB) { if (depth == 0) break; depth--; }
        }
        if (j <= 0) return NULL;
        int c = name_before(j);           /* `S{` or `S<A>{` */
        if (c < 0) return NULL;
        int s = stt_find(c);
        if (s < 0) return NULL;
        int atok[MAXP], na = explicit_args(c, atok);
        int np = na == STT[s].ntp ? na : 0;
        for (int q = 0; q < STT[s].np; q++)
            if (tokspan_eq(STT[s].pname[q], i - 2) && range_is_Fn(STT[s].pts[q], STT[s].pte[q]))
                return type_text(STT[s].pts[q], STT[s].pte[q], STT[s].tp, np, atok);
        return NULL;
    }
    int f = -1;                           /* `return` of, or the tail of, a fn returning Fn */
    for (int q = 0; q < NFNT; q++) if (FNT[q].body < i && i < FNT[q].end) { f = q; break; }
    if (f < 0 || !range_is_Fn(FNT[f].rts, FNT[f].rte)) return NULL;
    if (p == T_KW_RETURN || end + 1 == FNT[f].end) return type_text(FNT[f].rts, FNT[f].rte, NULL, 0, NULL);
    return NULL;
}

static char* lambda_pass(const char* prog) {
    static Edit led[MAXI];
    int ne = 0, madeenv = 0;
    build_tables();
    int depth = 0, prevclose = -1, itemfirst = 0;
    int accitem = -1, accprev = -1;       /* the item whose lifted text is accumulating */
    size_t an = 0, acap = 256;
    char* acc = xmalloc(acap);
    acc[0] = 0;
    for (int i = 0; i < NTOK; i++) {
        TK k = TOKS[i].k;
        if (k == T_LB) { depth++; continue; }
        if (k == T_RB) {
            if (--depth == 0) { prevclose = i; itemfirst = i + 1; }
            continue;
        }
        if (depth == 0) continue;
        int body = lambda_body(i);
        if (body < 0) continue;
        int end = match_brace(body);
        int inner = 0;                    /* innermost first: an outer lambda waits a round */
        for (int j = body + 1; j < end && !inner; j++) if (lambda_body(j) >= 0) inner = 1;
        if (inner) continue;
        /* Inside a generic function template (M6.4b): the lambda lifts to a
         * template of its own over the type parameters it names, and its
         * use site `__c_N<T>` is a nested generic use — instantiated with
         * the enclosing template by the generic pass, like `Box<T>`. */
        int gname = generic_item_name(itemfirst);
        int tp[MAXP], ntp = 0;            /* the enclosing template's params */
        if (gname >= 0) {
            if (TOKS[gname - 1].k != T_KW_FN)
                die("%s:%d: a lambda inside generic '%.*s'", FILENAME, TOKS[i].line,
                    TOKS[gname].slen, TOKS[gname].s);
            for (int t = gname + 2; TOKS[t].k == T_IDENT; t += 2) {   /* NAME < A , B > */
                if (ntp < MAXP) tp[ntp++] = t;
                if (TOKS[t + 1].k != T_COMMA) break;
            }
        }
        char* slot = fn_slot(i, end);     /* a closure slot (M6.4c1)? its type text */
        int closure = slot != NULL;
        int oslot = closure && !strncmp(slot, "FnOnce", 6);   /* an owning slot (M6.4c6) */
        char fin[160];                    /* its finaliser's name (a template's carries <T>) */
        fin[0] = 0;
        /* `h := fn(...) { ... }` (M6.4c4): a bound lambda may capture own
         * locals — it becomes a call-once closure, consumed by its call. */
        int bname = (i >= 2 && TOKS[i - 1].k == T_WALRUS && TOKS[i - 2].k == T_IDENT) ? i - 2 : -1;
        int refs[256];                    /* references to enclosing locals (M6.4c2) */
        int nref = scan_captures(itemfirst, i, body, end, refs, 256, !closure && bname < 0);
        int caps[32], ncap = 0;           /* the distinct captured names, first use first */
        for (int r = 0; r < nref; r++) {
            int dup = 0;
            for (int c = 0; c < ncap && !dup; c++) if (tokspan_eq(caps[c], refs[r])) dup = 1;
            if (!dup && ncap < 32) caps[ncap++] = refs[r];
        }
        int used[MAXP], nused = 0;        /* the ones the lambda — its closure type, its tokens,
                                           * or a capture's type (M6.4c6d) — mentions, in order */
        for (int q = 0; q < ntp; q++) {
            int m = closure && word_in(slot, tp[q]);
            for (int u = i + 1; u <= end && !m; u++) if (tokspan_eq(tp[q], u)) m = 1;
            for (int c = 0; c < ncap && !m; c++) {   /* `v: T` captured by a lambda whose own
                                                     * signature names no T: its environment
                                                     * is a template over T all the same */
                char* t = local_type(itemfirst, i, caps[c], 0);
                if (t && word_in(t, tp[q])) m = 1;
            }
            if (m) used[nused++] = q;
        }
        int once = 0;                     /* M6.4c4: an own capture makes a call-once closure */
        char* otys[32] = {0};
        if (ncap && !closure) {           /* a bound lambda: only with an own capture */
            int anyown = 0;
            for (int c = 0; c < ncap; c++) {
                otys[c] = local_type(itemfirst, i, caps[c], 0);
                if (otys[c] && is_own_type(otys[c])) anyown = 1;
            }
            if (!anyown)
                die("%s:%d: lambda captures '%.*s', a local of the enclosing function — only a lambda in a closure slot (an Fn type) can capture; pass it as a parameter",
                    FILENAME, TOKS[caps[0]].line, TOKS[caps[0]].slen, TOKS[caps[0]].s);
            for (int c = 0; c < ncap; c++)
                if (!otys[c])
                    die("%s:%d: cannot capture '%.*s': its type is not evident — bind it with a literal, a call or a struct literal, or pass it as a parameter",
                        FILENAME, TOKS[caps[c]].line, TOKS[caps[c]].slen, TOKS[caps[c]].s);
            once = 1;
        }
        int ownenv = 0;                   /* M6.4c6: an own capture in a FnOnce slot makes its environment own */
        if (closure)                      /* an Fn slot keeps its environment behind a pointer */
            for (int c = 0; c < ncap; c++) {
                char* t = local_type(itemfirst, i, caps[c], 0);
                if (t && is_own_type(t) && oslot) ownenv = 1;
                else if (t && is_own_type(t))
                    die("%s:%d: lambda captures own value '%.*s' — an own capture needs a call-once closure: bind the lambda with := and call it once, or make the slot FnOnce(...) (M6.4c6)",
                        FILENAME, TOKS[caps[c]].line, TOKS[caps[c]].slen, TOKS[caps[c]].s);
            }
        /* M6.4c6c: an own environment inside a template is an `own struct __E_N<T>`
         * template — the generic pass carries the keyword into every instantiation. */
        int lamno = NLAMBDA++;
        char* targs = xmalloc(4 + (size_t)nused * 64);   /* `<A, B>`, or "" */
        int tn = 0;
        targs[0] = 0;
        if (nused) {                      /* the lifted items are templates over these */
            tn += sprintf(targs + tn, "<");
            for (int q = 0; q < nused; q++)
                tn += sprintf(targs + tn, "%s%.*s", q ? ", " : "", TOKS[tp[used[q]]].slen, TOKS[tp[used[q]]].s);
            sprintf(targs + tn, ">");
        }
        char* name = xmalloc(24 + strlen(targs));
        sprintf(name, "__c_%d%s", lamno, targs);
        if (accitem != itemfirst) {       /* flush the previous item's lifted text */
            if (accitem >= 0) {
                if (ne >= MAXI) die("%s: too many lambdas", FILENAME);
                if (accprev >= 0) {
                    led[ne].start = led[ne].end = TOKS[accprev].end;
                    char* r = xmalloc(an + 3); sprintf(r, "\n\n%s", acc); led[ne].repl = r;
                } else {
                    led[ne].start = led[ne].end = TOKS[accitem].start;
                    char* r = xmalloc(an + 3); sprintf(r, "%s\n\n", acc); led[ne].repl = r;
                }
                ne++;
            }
            an = 0; acap = 256;
            acc = xmalloc(acap);
            acc[0] = 0;
            accitem = itemfirst; accprev = prevclose;
        }
        if (an) app(&acc, &an, &acap, "\n\n", 2);
        char* birth = "0";
        char* tys[32] = {0};
        if (ncap && !once) {              /* the environment struct and its maker */
            apps(&acc, &an, &acap, env_text(itemfirst, i, caps, ncap, lamno, &birth, tys, targs, ownenv));
            apps(&acc, &an, &acap, "\n\n");
            madeenv = 1;
        }
        if (oslot) {                      /* M6.4c6: the finaliser — takes the environment out
                                           * so its captures drop; a no-op when nothing was
                                           * captured. Every capturing lambda gets the real
                                           * one (M6.4c6d): a plain environment takes out a
                                           * plain struct and nothing drops, and a template
                                           * environment may instantiate own (a capture typed
                                           * T at an own T), which only this shape finalises */
            char ft[400];                 /* inside a template (M6.4c6b) the finaliser is a
                                           * template too, over the environment's parameters */
            if (ncap) {
                sprintf(fin, "__fin_E_%d%s", lamno, targs);
                sprintf(ft, "fn %s(_env: addr) {\n    __p := _env as *__E_%d%s;\n    __e := __p[0];\n}\n\n", fin, lamno, targs);
            } else {
                sprintf(fin, "__nop_fin_%d", lamno);
                sprintf(ft, "fn %s(_env: addr) { }\n\n", fin);
            }
            apps(&acc, &an, &acap, ft);
        }
        if (once) {                       /* the own environment and the closure's own struct */
            apps(&acc, &an, &acap, once_text(caps, ncap, lamno, otys, targs));
            apps(&acc, &an, &acap, "\n\n");
        }
        app(&acc, &an, &acap, "fn ", 3);
        app(&acc, &an, &acap, name, strlen(name));
        if (once) {                       /* the closure itself comes first, held: its
                                           * fields — the captured owns — drop at the end */
            char on[96];
            sprintf(on, "__O_%d%s", lamno, targs);
            apps(&acc, &an, &acap, "(__self: ");
            apps(&acc, &an, &acap, on);
            if (TOKS[i + 2].k != T_RP) app(&acc, &an, &acap, ", ", 2);
            app(&acc, &an, &acap, SRC + TOKS[i + 2].start, (size_t)(TOKS[body].end - TOKS[i + 2].start));
            int pos = TOKS[body].end;
            for (int r = 0; r < nref; r++) {  /* the body reads its captures as __self.env.name */
                int u = refs[r];
                app(&acc, &an, &acap, SRC + pos, (size_t)(TOKS[u].start - pos));
                apps(&acc, &an, &acap, "__self.env.");
                app(&acc, &an, &acap, TOKS[u].s, (size_t)TOKS[u].slen);
                pos = TOKS[u].end;
                int c = 0;                /* a captured closure, called: through its call field */
                while (c < ncap && !tokspan_eq(caps[c], u)) c++;
                if (TOKS[u + 1].k == T_LP && c < ncap && !strncmp(otys[c], "__Fn_", 5)) {
                    apps(&acc, &an, &acap, ".call(__self.env.");
                    app(&acc, &an, &acap, TOKS[u].s, (size_t)TOKS[u].slen);
                    apps(&acc, &an, &acap, TOKS[u + 2].k == T_RP ? ".env" : ".env, ");
                    pos = TOKS[u + 1].end;
                }
            }
            app(&acc, &an, &acap, SRC + pos, (size_t)(TOKS[end].end - pos));
        } else if (closure) {             /* the environment comes first */
            app(&acc, &an, &acap, "(_env: addr", 11);
            if (TOKS[i + 2].k != T_RP) app(&acc, &an, &acap, ", ", 2);
            if (ncap) {                   /* the body reads its captures as __e.name */
                char en[96];
                sprintf(en, "__E_%d%s", lamno, targs);
                app(&acc, &an, &acap, SRC + TOKS[i + 2].start, (size_t)(TOKS[body].end - TOKS[i + 2].start));
                apps(&acc, &an, &acap, " __p := _env as *");
                apps(&acc, &an, &acap, en);
                apps(&acc, &an, &acap, oslot ? ";" : "; __e := __p[0];");   /* a FnOnce body only PEEKS
                                                                             * (a take would end the captures) */
                int pos = TOKS[body].end;
                for (int r = 0; r < nref; r++) {
                    int u = refs[r];
                    app(&acc, &an, &acap, SRC + pos, (size_t)(TOKS[u].start - pos));
                    apps(&acc, &an, &acap, oslot ? "__p[0]." : "__e.");
                    app(&acc, &an, &acap, TOKS[u].s, (size_t)TOKS[u].slen);
                    pos = TOKS[u].end;
                    int c = 0;            /* a captured closure, called: through its call field */
                    while (c < ncap && !tokspan_eq(caps[c], u)) c++;
                    if (TOKS[u + 1].k == T_LP && c < ncap && !strncmp(tys[c], "__Fn_", 5)) {
                        apps(&acc, &an, &acap, oslot ? ".call(__p[0]." : ".call(__e.");
                        app(&acc, &an, &acap, TOKS[u].s, (size_t)TOKS[u].slen);
                        apps(&acc, &an, &acap, TOKS[u + 2].k == T_RP ? ".env" : ".env, ");
                        pos = TOKS[u + 1].end;
                    }
                }
                app(&acc, &an, &acap, SRC + pos, (size_t)(TOKS[end].end - pos));
            } else
                app(&acc, &an, &acap, SRC + TOKS[i + 2].start, (size_t)(TOKS[end].end - TOKS[i + 2].start));
        } else
            app(&acc, &an, &acap, SRC + TOKS[i + 1].start, (size_t)(TOKS[end].end - TOKS[i + 1].start));
        if (ne >= MAXI) die("%s: too many lambdas", FILENAME);
        led[ne].start = TOKS[i].start;
        led[ne].end = TOKS[end].end;
        led[ne].repl = oslot ? once_value(slot, birth, name, fin)
                     : closure ? closure_value(slot, birth, name)
                     : once ? once_birth(caps, ncap, lamno, targs) : name;
        ne++;
        if (once) {                       /* every call `h(args)` after the binding, in this
                                           * function: `__c_N(h, args)` — the call consumes h
                                           * (`__c_N<T>(h, args)` inside a template, M6.4c4b) */
            int depth2 = 0;
            for (int u = end + 1; u + 1 < NTOK; u++) {
                if (TOKS[u].k == T_LB) depth2++;
                else if (TOKS[u].k == T_RB) { if (--depth2 < 0) break; }
                int lb = lambda_body(u);  /* a nested lambda's text is its own edit */
                if (lb >= 0) { u = match_brace(lb); continue; }
                if (TOKS[u].k != T_IDENT || TOKS[u + 1].k != T_LP || !tokspan_eq(u, bname)) continue;
                if (u > 0 && TOKS[u - 1].k == T_DOT) continue;
                if (ne >= MAXI) die("%s: too many lambdas", FILENAME);
                char* r = xmalloc(48 + (size_t)TOKS[u].slen + strlen(targs));
                sprintf(r, "__c_%d%s(%.*s%s", lamno, targs, TOKS[u].slen, TOKS[u].s, TOKS[u + 2].k == T_RP ? "" : ", ");
                led[ne].start = TOKS[u].start;
                led[ne].end = TOKS[u + 1].end;
                led[ne].repl = r;
                ne++;
                u++;
            }
        }
        i = end;                          /* the body's braces are balanced */
    }
    if (accitem >= 0) {
        if (ne >= MAXI) die("%s: too many lambdas", FILENAME);
        if (accprev >= 0) {
            led[ne].start = led[ne].end = TOKS[accprev].end;
            char* r = xmalloc(an + 3); sprintf(r, "\n\n%s", acc); led[ne].repl = r;
        } else {
            led[ne].start = led[ne].end = TOKS[accitem].start;
            char* r = xmalloc(an + 3); sprintf(r, "%s\n\n", acc); led[ne].repl = r;
        }
        ne++;
    }
    if (ne == 0) return (char*)prog;
    if (madeenv && !declares_sbrk()) {    /* the makers need the syscall declared */
        if (ne >= MAXI) die("%s: too many lambdas", FILENAME);
        led[ne].start = led[ne].end = TOKS[0].start;
        led[ne].repl = "extern syscall {\n    fn sys_sbrk(incr: i64) -> i64 = 7\n}\n\n";
        ne++;
    }
    qsort(led, (size_t)ne, sizeof(Edit), edit_cmp);
    size_t cap = strlen(prog) + (size_t)ne * 64 + 1, n = 0;
    char* out = xmalloc(cap);
    int prev = 0;
    for (int e = 0; e < ne; e++) {
        app(&out, &n, &cap, prog + prev, (size_t)(led[e].start - prev));
        app(&out, &n, &cap, led[e].repl, strlen(led[e].repl));
        prev = led[e].end;
    }
    app(&out, &n, &cap, prog + prev, strlen(prog) - (size_t)prev);
    return out;
}

/* Text accumulating before one top-level item (lifted adapters), flushed
 * as one insert after the item before it — or at the item's start when
 * there is none. */
typedef struct { int item, prev; char* acc; size_t n, cap; } Acc;

static void acc_flush(Acc* a, Edit* ed, int* ne) {
    if (a->item < 0) return;
    if (*ne >= MAXI) die("%s: too many rewrites", FILENAME);
    char* r = xmalloc(a->n + 3);
    if (a->prev >= 0) { ed[*ne].start = ed[*ne].end = TOKS[a->prev].end; sprintf(r, "\n\n%s", a->acc); }
    else { ed[*ne].start = ed[*ne].end = TOKS[a->item].start; sprintf(r, "%s\n\n", a->acc); }
    ed[*ne].repl = r;
    (*ne)++;
    a->item = -1; a->n = 0; a->acc[0] = 0;
}

static void acc_add(Acc* a, int item, int prev, const char* text, Edit* ed, int* ne) {
    if (a->item != item) { acc_flush(a, ed, ne); a->item = item; a->prev = prev; }
    if (a->n) apps(&a->acc, &a->n, &a->cap, "\n\n");
    apps(&a->acc, &a->n, &a->cap, text);
}

/* One closure signature's struct: nested closure types register first, so
 * their layouts precede the one that holds them. */
#define MAXSIG 64
static char* DECLARED[MAXSIG];            /* struct names declared by an earlier pass */
static int NDECL;
static void reg_sigs(int t, int e, char** nm, char** tx, int* n) {
    for (int u = t + 1; u < e; u++)
        if (is_Fn(u)) { int ue = skip_type(u); reg_sigs(u, ue, nm, tx, n); u = ue - 1; }
    char* name = sig_name(t, e);
    for (int i = 0; i < *n; i++) if (!strcmp(nm[i], name)) return;
    for (int i = 0; i < NDECL; i++) if (!strcmp(DECLARED[i], name)) return;
    if (*n >= MAXSIG || NDECL >= MAXSIG) die("%s: too many closure signatures", FILENAME);
    DECLARED[NDECL++] = name;
    int pts[MAXTP], pte[MAXTP], np, rts, rte;
    sig_parts(t, e, pts, pte, &np, &rts, &rte);
    size_t cap = 256, len = 0;
    char* b = xmalloc(cap);
    b[0] = 0;
    if (is_once(t)) {                     /* M6.4c6: the owning closure — an own struct that
                                           * drops through its finaliser, and its one call */
        apps(&b, &len, &cap, "#[drop(__fo_drop_");
        apps(&b, &len, &cap, name + 2);
        apps(&b, &len, &cap, ")]\nown ");
    }
    apps(&b, &len, &cap, "struct ");
    apps(&b, &len, &cap, name);
    apps(&b, &len, &cap, " {\n    env: addr,\n    call: fn(addr");
    for (int p = 0; p < np; p++) { apps(&b, &len, &cap, ", "); render_type(pts[p], pte[p], &b, &len, &cap); }
    apps(&b, &len, &cap, ")");
    if (rts >= 0) { apps(&b, &len, &cap, " -> "); render_type(rts, rte, &b, &len, &cap); }
    apps(&b, &len, &cap, ",\n");
    if (is_once(t)) {
        apps(&b, &len, &cap, "    fin: fn(addr),\n}\n\nfn __fo_drop_");
        apps(&b, &len, &cap, name + 2);
        apps(&b, &len, &cap, "(f: ");
        apps(&b, &len, &cap, name);
        apps(&b, &len, &cap, ") {\n    f.fin(f.env);\n}\n\nfn __call_once_");
        apps(&b, &len, &cap, name + 2);
        apps(&b, &len, &cap, "(f: ");
        apps(&b, &len, &cap, name);
        for (int p = 0; p < np; p++) {
            char pn[16];
            sprintf(pn, ", a%d: ", p);
            apps(&b, &len, &cap, pn);
            render_type(pts[p], pte[p], &b, &len, &cap);
        }
        apps(&b, &len, &cap, ")");
        if (rts >= 0) { apps(&b, &len, &cap, " -> "); render_type(rts, rte, &b, &len, &cap); }
        apps(&b, &len, &cap, rts >= 0 ? " {\n    r := f.call(f.env" : " {\n    f.call(f.env");
        for (int p = 0; p < np; p++) {
            char pn[16];
            sprintf(pn, ", a%d", p);
            apps(&b, &len, &cap, pn);
        }
        apps(&b, &len, &cap, ");\n    __fo_drop_");
        apps(&b, &len, &cap, name + 2);
        apps(&b, &len, &cap, rts >= 0 ? "(f);\n    r\n}\n\n" : "(f);\n}\n\n");
    } else
        apps(&b, &len, &cap, "}\n\n");
    nm[*n] = name; tx[*n] = b; (*n)++;
}

/* M6.4c5: a type span that is a pointer to a known struct (`*S`) — the
 * struct's table index, else -1. */
static int ptr_struct(int ts, int te) {
    if (te != ts + 2 || TOKS[ts].k != T_STAR || TOKS[ts + 1].k != T_IDENT) return -1;
    return stt_find(ts + 1);
}

/* The `;` ending the statement that starts at t (brackets balanced), or
 * -1 when the enclosing block closes first. */
static int stmt_end(int t) {
    for (int d = 0; TOKS[t].k != T_EOF; t++) {
        TK k = TOKS[t].k;
        if (k == T_LP || k == T_LB || k == T_LBRACK) d++;
        else if (k == T_RP || k == T_RBRACK) d--;
        else if (k == T_RB) { if (d == 0) return -1; d--; }
        else if (k == T_SEMI && d == 0) return t;
    }
    return -1;
}

/* Does the type span [ts, te) name one of item fi's type parameters?
 * (M6.4c6b: a FnOnce signature over a template parameter has no struct
 * name yet — its calls are rewritten by the closure pass that follows
 * the generic pass, on the instantiated, concrete signature.) */
static int span_names_tp(const Item* fi, int ts, int te) {
    for (int u = ts; u < te; u++)
        if (TOKS[u].k == T_IDENT)
            for (int q = 0; q < fi->ntp; q++) if (tokspan_eq(fi->tp[q], u)) return 1;
    return 0;
}

/* The closure pass (M6.4c1), after the lambdas are lifted: every `Fn(...)`
 * type slot becomes its struct name (the structs declared once, ahead of
 * the first struct or function), calls through closure-typed parameters,
 * locals and struct fields go through `.call(.env, ...)`, and a named
 * function passed where a closure is expected is wrapped in an adapter.
 * A program that never spells `Fn` is returned untouched. */
static char* closure_pass(const char* prog) {
    build_tables();
    static Edit ced[MAXI];
    int ne = 0;
    char* signm[MAXSIG]; char* sigtx[MAXSIG]; int nsig = 0;
    int firstitem = -1, firstprev = -1;   /* the first struct/fn/enum/impl, and the `}` before it */
    int lastfn = -1;                      /* the `}` of the last `__Fn_` struct an earlier pass declared */
    int after = -1;                       /* the `}` of the last struct or enum a signature names */
    int tdn[256], tde[256], ntd = 0;      /* the top-level struct/enum declarations: name, closing `}` */
    for (int t = 0, d = 0; t + 1 < NTOK; t++) {
        if (TOKS[t].k == T_LB) { d++; continue; }
        if (TOKS[t].k == T_RB) { d--; continue; }
        if (d == 0 && (TOKS[t].k == T_KW_STRUCT || TOKS[t].k == T_KW_ENUM) && TOKS[t + 1].k == T_IDENT && ntd < 256) {
            int b = t + 2;
            while (TOKS[b].k != T_LB && TOKS[b].k != T_EOF) b++;
            if (TOKS[b].k == T_LB) { tdn[ntd] = t + 1; tde[ntd] = match_brace(b); ntd++; }
        }
    }
    int depth = 0, itemfirst = 0, prevclose = -1;
    for (int t = 0; t + 1 < NTOK; t++) {
        TK k = TOKS[t].k;
        if (k == T_LB) { depth++; continue; }
        if (k == T_RB) { if (--depth == 0) { prevclose = t; itemfirst = t + 1; } continue; }
        if (depth == 0 && firstitem < 0 &&
            (k == T_KW_STRUCT || k == T_KW_FN || k == T_KW_ENUM || k == T_KW_IMPL)) {
            firstitem = itemfirst; firstprev = prevclose;
        }
        if (depth == 0 && k == T_KW_STRUCT && TOKS[t + 1].k == T_IDENT &&
            ((TOKS[t + 1].slen > 5 && !memcmp(TOKS[t + 1].s, "__Fn_", 5)) ||
             (TOKS[t + 1].slen > 9 && !memcmp(TOKS[t + 1].s, "__FnOnce_", 9)))) {
            int b = t + 2;
            while (TOKS[b].k != T_LB && TOKS[b].k != T_EOF) b++;
            if (TOKS[b].k == T_LB) lastfn = match_brace(b);
        }
        if (!is_Fn(t)) continue;
        int e = skip_type(t);
        if (e < 0) die("%s:%d: malformed Fn type", FILENAME, TOKS[t].line);
        /* Naming a type parameter of the enclosing template (M6.4c3): left
         * as written — the generic pass substitutes the parameter, and the
         * closure pass that follows it finishes the job. */
        int gname = generic_item_name(itemfirst), generic = 0;
        if (gname >= 0)
            for (int q = gname + 2; TOKS[q].k == T_IDENT && !generic; q += 2) {
                for (int u = t; u < e && !generic; u++) if (tokspan_eq(q, u)) generic = 1;
                if (TOKS[q + 1].k != T_COMMA) break;
            }
        if (is_once(t)) {                 /* M6.4c6: a FnOnce field would be an own value inside
                                           * its holder — a parameter, a local or a return only
                                           * (judged before the generic skip: a template's field
                                           * is refused whether or not it is ever instantiated) */
            int q = itemfirst;
            while (q < t && TOKS[q].k != T_KW_STRUCT && TOKS[q].k != T_KW_FN && TOKS[q].k != T_KW_ENUM &&
                   TOKS[q].k != T_KW_IMPL) q++;
            if (TOKS[q].k == T_KW_STRUCT)
                die("%s:%d: a FnOnce field needs an own struct holding it (N v0.25) — pass the closure as a parameter instead (M6.4c6)",
                    FILENAME, TOKS[t].line);
        }
        if (generic) { t = e - 1; continue; }
        reg_sigs(t, e, signm, sigtx, &nsig);
        /* A struct or enum the signature names by value must be laid out
         * first (N emits layouts in declaration order, and a struct field
         * of function type spells its types out): the block goes after it. */
        for (int u = t; u < e; u++)
            if (TOKS[u].k == T_IDENT)
                for (int d = 0; d < ntd; d++)
                    if (tokspan_eq(tdn[d], u) && tde[d] > after) after = tde[d];
        if (ne >= MAXI) die("%s: too many rewrites", FILENAME);
        ced[ne].start = TOKS[t].start;
        ced[ne].end = TOKS[e - 1].end;
        ced[ne].repl = sig_name(t, e);
        ne++;
        t = e - 1;
    }
    /* Calls through closure values, per function body: parameters of Fn
     * type, locals bound from a call returning Fn, and the Fn fields of a
     * struct-typed parameter or local (bound from a literal or a call). */
    for (int f = 0; f < NFNT; f++) {
        Item* fi = &FNT[f];
        int cl[128], ncl = 0, sn[128], sv[128], nsn = 0, pn[128], pv[128], npn = 0;
        char* cls[128];                   /* M6.4c6: a FnOnce name's signature, else NULL */
        for (int p = 0; p < fi->np; p++) {
            if (range_is_Fn(fi->pts[p], fi->pte[p])) {
                if (is_once(fi->pts[p]) && span_names_tp(fi, fi->pts[p], fi->pte[p])) continue;   /* M6.4c6b: pass 2 */
                if (ncl < 128) { cls[ncl] = is_once(fi->pts[p]) ? sig_name(fi->pts[p], fi->pte[p]) : NULL; cl[ncl++] = fi->pname[p]; }
            } else if (fi->pte[p] == fi->pts[p] + 1) {
                int s = stt_find(fi->pts[p]);
                if (s >= 0 && nsn < 128) { sn[nsn] = fi->pname[p]; sv[nsn] = s; nsn++; }
            } else {                      /* bs: *S (M6.4c5) */
                int s = ptr_struct(fi->pts[p], fi->pte[p]);
                if (s >= 0 && npn < 128) { pn[npn] = fi->pname[p]; pv[npn] = s; npn++; }
            }
        }
        for (int u = fi->body + 1; u + 2 < fi->end; u++) {
            if (TOKS[u].k != T_IDENT || TOKS[u + 1].k != T_WALRUS || TOKS[u + 2].k != T_IDENT) continue;
            int r = u + 2;
            int e = stmt_end(r);          /* x := ... as *S; (M6.4c5) */
            if (e >= r + 4 && TOKS[e - 3].k == T_KW_AS && TOKS[e - 2].k == T_STAR && TOKS[e - 1].k == T_IDENT) {
                int s = stt_find(e - 1);
                if (s >= 0 && npn < 128) { pn[npn] = u; pv[npn] = s; npn++; }
                continue;
            }
            if (TOKS[r + 1].k == T_DOT && TOKS[r + 2].k == T_IDENT && TOKS[r + 3].k == T_SEMI) {
                int s = -1;               /* x := o.f; — o struct-typed, f a *S field (M6.4c5) */
                for (int c = 0; c < nsn && s < 0; c++) if (tokspan_eq(sn[c], r)) s = sv[c];
                if (s < 0) continue;
                int fld = -1;
                for (int q = 0; q < STT[s].np && fld < 0; q++) if (tokspan_eq(STT[s].pname[q], r + 2)) fld = q;
                if (fld < 0) continue;
                int ps = ptr_struct(STT[s].pts[fld], STT[s].pte[fld]);
                if (ps >= 0 && npn < 128) { pn[npn] = u; pv[npn] = ps; npn++; }
                continue;
            }
            if (TOKS[r + 1].k == T_LB) {  /* x := S{ ... } */
                int s = stt_find(r);
                if (s >= 0 && nsn < 128) { sn[nsn] = u; sv[nsn] = s; nsn++; }
            } else {                      /* x := g(...) or x := g<A>(...) */
                int lp = -1;
                if (TOKS[r + 1].k == T_LP) lp = r + 1;
                else if (TOKS[r + 1].k == T_LT) {
                    int a = r + 2;
                    while (TOKS[a].k == T_IDENT || TOKS[a].k == T_COMMA) a++;
                    if (TOKS[a].k == T_GT && TOKS[a + 1].k == T_LP) lp = a + 1;
                }
                if (lp < 0) continue;
                int g = fnt_find(r);
                if (g < 0) continue;
                if (range_is_Fn(FNT[g].rts, FNT[g].rte)) {
                    if (is_once(FNT[g].rts) && span_names_tp(&FNT[g], FNT[g].rts, FNT[g].rte)) continue;   /* M6.4c6b: pass 2 */
                    if (ncl < 128) { cls[ncl] = is_once(FNT[g].rts) ? sig_name(FNT[g].rts, FNT[g].rte) : NULL; cl[ncl++] = u; }
                } else if (FNT[g].rts >= 0 && FNT[g].rte == FNT[g].rts + 1) {
                    int s = stt_find(FNT[g].rts);
                    if (s >= 0 && nsn < 128) { sn[nsn] = u; sv[nsn] = s; nsn++; }
                }
            }
        }
        if (!ncl && !nsn && !npn) continue;
        for (int u = fi->body + 1; u + 1 < fi->end; u++) {
            if (TOKS[u].k != T_IDENT || TOKS[u - 1].k == T_DOT || TOKS[u - 1].k == T_KW_FN) continue;
            if (TOKS[u + 1].k == T_LP) {  /* f(a) -> f.call(f.env, a) */
                int ci = -1;
                for (int c = 0; c < ncl && ci < 0; c++) if (tokspan_eq(cl[c], u)) ci = c;
                if (ci < 0) continue;
                char* r = xmalloc((size_t)TOKS[u].slen * 2 + 48 + (cls[ci] ? strlen(cls[ci]) : 0));
                if (cls[ci])              /* a FnOnce: the call consumes it (M6.4c6) */
                    sprintf(r, "__call_once_%s(%.*s%s", cls[ci] + 2, TOKS[u].slen, TOKS[u].s,
                            TOKS[u + 2].k == T_RP ? "" : ", ");
                else
                    sprintf(r, "%.*s.call(%.*s.env%s", TOKS[u].slen, TOKS[u].s, TOKS[u].slen, TOKS[u].s,
                            TOKS[u + 2].k == T_RP ? "" : ", ");
                if (ne >= MAXI) die("%s: too many rewrites", FILENAME);
                ced[ne].start = TOKS[u].start; ced[ne].end = TOKS[u + 1].end; ced[ne].repl = r; ne++;
                continue;
            }
            if (TOKS[u + 1].k == T_LBRACK) {  /* bs[i].run(a) -> bs[i].run.call(bs[i].run.env, a) (M6.4c5) */
                int s = -1;
                for (int c = 0; c < npn && s < 0; c++) if (tokspan_eq(pn[c], u)) s = pv[c];
                if (s < 0) continue;
                int v = u + 1, d = 0;     /* the matching `]` */
                for (; TOKS[v].k != T_EOF; v++) {
                    if (TOKS[v].k == T_LBRACK) d++;
                    else if (TOKS[v].k == T_RBRACK && --d == 0) break;
                }
                if (TOKS[v].k != T_RBRACK || TOKS[v + 1].k != T_DOT || TOKS[v + 2].k != T_IDENT || TOKS[v + 3].k != T_LP) continue;
                int fld = -1;
                for (int q = 0; q < STT[s].np && fld < 0; q++) if (tokspan_eq(STT[s].pname[q], v + 2)) fld = q;
                if (fld < 0 || !range_is_Fn(STT[s].pts[fld], STT[s].pte[fld])) continue;
                int pl = TOKS[v].end - TOKS[u].start;   /* `bs[i]`, verbatim from the source */
                const char* ps = prog + TOKS[u].start;
                char* r = xmalloc((size_t)(pl + TOKS[v + 2].slen) * 2 + 24);
                sprintf(r, "%.*s.%.*s.call(%.*s.%.*s.env%s", pl, ps, TOKS[v + 2].slen, TOKS[v + 2].s,
                        pl, ps, TOKS[v + 2].slen, TOKS[v + 2].s,
                        TOKS[v + 4].k == T_RP ? "" : ", ");
                if (ne >= MAXI) die("%s: too many rewrites", FILENAME);
                ced[ne].start = TOKS[u].start; ced[ne].end = TOKS[v + 3].end; ced[ne].repl = r; ne++;
                u = v + 3;
                continue;
            }
            if (TOKS[u + 1].k == T_DOT && TOKS[u + 2].k == T_IDENT && TOKS[u + 3].k == T_LP) {
                int s = -1;               /* o.run(a) -> o.run.call(o.run.env, a) */
                for (int c = 0; c < nsn && s < 0; c++) if (tokspan_eq(sn[c], u)) s = sv[c];
                if (s < 0) continue;
                int fld = -1;
                for (int q = 0; q < STT[s].np && fld < 0; q++) if (tokspan_eq(STT[s].pname[q], u + 2)) fld = q;
                if (fld < 0 || !range_is_Fn(STT[s].pts[fld], STT[s].pte[fld])) continue;
                char* r = xmalloc((size_t)(TOKS[u].slen + TOKS[u + 2].slen) * 2 + 24);
                sprintf(r, "%.*s.%.*s.call(%.*s.%.*s.env%s", TOKS[u].slen, TOKS[u].s, TOKS[u + 2].slen, TOKS[u + 2].s,
                        TOKS[u].slen, TOKS[u].s, TOKS[u + 2].slen, TOKS[u + 2].s,
                        TOKS[u + 4].k == T_RP ? "" : ", ");
                if (ne >= MAXI) die("%s: too many rewrites", FILENAME);
                ced[ne].start = TOKS[u].start; ced[ne].end = TOKS[u + 3].end; ced[ne].repl = r; ne++;
                u += 3;
            }
        }
    }
    /* A named function where a closure is expected: an adapter taking the
     * environment first, and the closure value in its place. */
    Acc ad = { -1, -1, xmalloc(256), 0, 256 };
    ad.acc[0] = 0;
    depth = 0; itemfirst = 0; prevclose = -1;
    for (int u = 0; u + 1 < NTOK; u++) {
        TK k = TOKS[u].k;
        if (k == T_LB) { depth++; continue; }
        if (k == T_RB) { if (--depth == 0) { prevclose = u; itemfirst = u + 1; } continue; }
        if (depth == 0 || k != T_IDENT || TOKS[u + 1].k != T_LP ||
            (u > 0 && (TOKS[u - 1].k == T_DOT || TOKS[u - 1].k == T_KW_FN))) continue;
        int g = fnt_find(u);
        if (g < 0) continue;
        int d = 0, ai = 0, as = u + 2;
        for (int v = u + 1; TOKS[v].k != T_EOF; v++) {
            TK q = TOKS[v].k;
            int close = 0;
            if (q == T_LP || q == T_LB || q == T_LBRACK) { d++; continue; }
            if (q == T_RP || q == T_RB || q == T_RBRACK) { d--; if (d > 0) continue; close = 1; }
            else if (!(q == T_COMMA && d == 1)) continue;
            /* one argument [as, v): a bare name of a function, into an Fn parameter */
            if (v == as + 1 && TOKS[as].k == T_IDENT && ai < FNT[g].np &&
                range_is_Fn(FNT[g].pts[ai], FNT[g].pte[ai]) && fnt_find(as) >= 0) {
                int ts = FNT[g].pts[ai], te = FNT[g].pte[ai];
                int pts[MAXTP], pte[MAXTP], np, rts, rte;
                sig_parts(ts, te, pts, pte, &np, &rts, &rte);
                char* name = xmalloc(24);
                sprintf(name, "__c_%d", NLAMBDA++);
                size_t cap = 256, len = 0;
                char* b = xmalloc(cap);
                b[0] = 0;
                apps(&b, &len, &cap, "fn ");
                apps(&b, &len, &cap, name);
                apps(&b, &len, &cap, "(_env: addr");
                for (int p = 0; p < np; p++) {
                    char pn[16];
                    sprintf(pn, ", a%d: ", p);
                    apps(&b, &len, &cap, pn);
                    render_type(pts[p], pte[p], &b, &len, &cap);
                }
                apps(&b, &len, &cap, ")");
                if (rts >= 0) { apps(&b, &len, &cap, " -> "); render_type(rts, rte, &b, &len, &cap); }
                apps(&b, &len, &cap, " { ");
                app(&b, &len, &cap, TOKS[as].s, (size_t)TOKS[as].slen);
                apps(&b, &len, &cap, "(");
                for (int p = 0; p < np; p++) {
                    char pn[16];
                    sprintf(pn, "%sa%d", p ? ", " : "", p);
                    apps(&b, &len, &cap, pn);
                }
                apps(&b, &len, &cap, rts >= 0 ? ") }" : "); }");
                if (is_once(ts)) {        /* M6.4c6: nothing to finalise */
                    apps(&b, &len, &cap, "\n\nfn __nop_fin_");
                    apps(&b, &len, &cap, name + 4);
                    apps(&b, &len, &cap, "(_env: addr) { }");
                }
                acc_add(&ad, itemfirst, prevclose, b, ced, &ne);
                if (ne >= MAXI) die("%s: too many rewrites", FILENAME);
                ced[ne].start = TOKS[as].start; ced[ne].end = TOKS[as].end;
                if (is_once(ts)) {
                    char fin[32];
                    sprintf(fin, "__nop_fin_%s", name + 4);
                    ced[ne].repl = once_value(sig_name(ts, te), "0", name, fin);
                } else
                    ced[ne].repl = closure_value(sig_name(ts, te), "0", name);
                ne++;
            }
            if (close) break;
            ai++; as = v + 1;
        }
    }
    acc_flush(&ad, ced, &ne);
    /* The signature structs, once, ahead of the first struct or function —
     * after the ones an earlier pass declared, whose layouts they may use,
     * and after the last struct or enum a signature names. */
    if (nsig > 0) {
        size_t cap = 256, len = 0;
        char* b = xmalloc(cap);
        b[0] = 0;
        for (int i = 0; i < nsig; i++) apps(&b, &len, &cap, sigtx[i]);
        if (ne >= MAXI) die("%s: too many rewrites", FILENAME);
        int at = lastfn > after ? lastfn : after;
        if (at < 0) at = firstprev;
        if (at >= 0) {
            char* r = xmalloc(len + 3);
            sprintf(r, "\n\n%s", b);
            r[len] = 0;                   /* drop the block's own trailing blank line */
            ced[ne].start = ced[ne].end = TOKS[at].end;
            ced[ne].repl = r;
        } else {
            ced[ne].start = ced[ne].end = firstitem >= 0 ? TOKS[firstitem].start : 0;
            ced[ne].repl = b;
        }
        ne++;
    }
    if (ne == 0) return (char*)prog;
    qsort(ced, (size_t)ne, sizeof(Edit), edit_cmp);
    size_t cap = strlen(prog) + (size_t)ne * 64 + 1, n = 0;
    char* out = xmalloc(cap);
    int prev = 0;
    for (int e = 0; e < ne; e++) {
        if (ced[e].start < prev) die("%s: overlapping closure rewrites", FILENAME);
        app(&out, &n, &cap, prog + prev, (size_t)(ced[e].start - prev));
        app(&out, &n, &cap, ced[e].repl, strlen(ced[e].repl));
        prev = ced[e].end;
    }
    app(&out, &n, &cap, prog + prev, strlen(prog) - (size_t)prev);
    return out;
}

/* ------------------------------------------------------------------ */
/* driver                                                             */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    if (argc != 4 || strcmp(argv[2], "-o") != 0)
        die("usage: nppc <file.npp> -o <out.n>");
    FILENAME = argv[1];

    long sz;
    char* source_path = normalize_path(FILENAME);
    char* buf = read_file(source_path, &sz);
    if (!buf) die("nppc: cannot open '%s'", FILENAME);

    /* Modules first (M6.5): the program is the main file with every `use`
     * inlined. A file with no directives resolves to itself byte-for-byte. */
    char* prog = resolve_file(source_path, buf, sz);
    FILENAME = argv[1];

    /* The read side is real: the whole program lexes as the N dialect (a
     * file that fails here would fail identically under ncc), and the token
     * buffer with source spans is what the generic pass rewrites over. */
    lex_text(prog, (long)strlen(prog));

    /* Visibility + private-item renaming (M6.5b), then re-lex the result so
     * the generic pass sees the mangled names as ordinary ones. */
    char* vis = modules_pass(prog);
    if (vis != prog) { prog = vis; lex_text(prog, (long)strlen(prog)); }

    /* Lambdas (M6.4a): lifted to top-level functions by a text splice, to
     * a fixpoint (innermost first), then re-lexed so the generic pass sees
     * plain functions. */
    for (;;) {
        char* lam = lambda_pass(prog);
        if (lam == prog) break;
        prog = lam;
        lex_text(prog, (long)strlen(prog));
    }
    /* Closures (M6.4c1): Fn types to their structs, calls through closure
     * values, adapters for named functions — then re-lex. */
    {
        char* cl = closure_pass(prog);
        if (cl != prog) { prog = cl; lex_text(prog, (long)strlen(prog)); }
    }
    collect_generic_decls();
    collect_generic_fns();
    collect_generic_enums();
    guard_templates();
    collect_instantiations();
    collect_inferred_calls();
    collect_inferred_constructions();
    expand_nested();

    /* Build the edit list: each generic declaration is replaced by the
     * concrete structs its distinct instantiations require (in first-use
     * order), and each use site is replaced by its mangled name. */
    for (int g = 0; g < NGS; g++) {
        char* body = xmalloc(1);
        body[0] = 0;
        int blen = 0, emitted = 0;
        for (int i = 0; i < NINST; i++) {
            if (INSTS[i].gi != g) continue;
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (INSTS[j].gi == g && args_same(&INSTS[i], &INSTS[j])) { dup = 1; break; }
            if (dup) continue;
            char* cs = concrete_item(&INSTS[i]);
            int add = (int)strlen(cs) + 2;
            body = realloc(body, (size_t)blen + (size_t)add + 1);
            if (!body) die("nppc: out of memory");
            blen += sprintf(body + blen, "%s%s", emitted ? "\n\n" : "", cs);
            emitted = 1;
        }
        EDITS[NEDIT].start = GS[g].declstart;
        EDITS[NEDIT].end = GS[g].declend;
        EDITS[NEDIT].repl = body;         /* empty if the generic is never used */
        NEDIT++;
    }
    for (int i = 0; i < NINST; i++) {
        if (INSTS[i].start < 0) continue;  /* a nested instantiation: rewritten by its template's emit */
        EDITS[NEDIT].start = INSTS[i].start;
        EDITS[NEDIT].end = INSTS[i].end;
        EDITS[NEDIT].repl = mangle(&INSTS[i]);
        NEDIT++;
    }
    strip_pub();
    qsort(EDITS, (size_t)NEDIT, sizeof(Edit), edit_cmp);

    /* Splice: verbatim between edits, replacement at each. With no generics
     * there are no edits and the output equals the input byte-for-byte. */
    size_t fcap = (size_t)LEN + 1024, fn = 0;
    char* final = xmalloc(fcap);
    final[0] = 0;
    int prev = 0;
    for (int e = 0; e < NEDIT; e++) {
        if (EDITS[e].start < prev)
            die("%s: overlapping generic rewrites", FILENAME);
        app(&final, &fn, &fcap, SRC + prev, (size_t)(EDITS[e].start - prev));
        app(&final, &fn, &fcap, EDITS[e].repl, strlen(EDITS[e].repl));
        prev = EDITS[e].end;
    }
    app(&final, &fn, &fcap, SRC + prev, (size_t)(LEN - prev));

    /* Closures, second pass (M6.4c3): a closure type that named a type
     * parameter is concrete in every instantiation now — its struct is
     * declared, its slots and calls rewritten. Programs without one come
     * back untouched. */
    lex_text(final, (long)strlen(final));
    {
        char* cl = closure_pass(final);
        if (cl != final) final = cl;
    }

    FILE* out = fopen(argv[3], "wb");
    if (!out) die("nppc: cannot open '%s' for writing", argv[3]);
    fwrite(final, 1, strlen(final), out);
    fclose(out);

    fprintf(stderr, "nppc: OK — %d token(s), %d generic(s), %d instantiation(s), %d lambda(s) -> %s\n",
            NTOK - 1, NGS, NINST, NLAMBDA, argv[3]);
    return 0;
}
