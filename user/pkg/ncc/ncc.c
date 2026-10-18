/* ncc — bootstrap compiler for N, the native language of NyxOS.
 * Transpiles .n source to C targeting the nyxrt runtime (user/nyxrt.h).
 * Hosted tool (runs on the dev machine); single file, no dependencies.
 * See lang/docs/spec-n.md for the language specification this implements.
 *
 * Supported subset (currently N v0.22):
 *   - extern syscall { fn name(params) [-> T] = N }
 *   - fn decls with block bodies, params, return types
 *   - statements: let (:=, mut), assignment (= += -=), return, while,
 *     if/else, break, continue, expression statements, block tail-expr
 *   - expressions: int (dec/hex), bool, string literals, string
 *     interpolation "{expr}" (typed: str inserts text, integers decimal;
 *     "{expr:x}"/"{expr:X}" = hex, v0.20; ":wN"/":zN" = width / zero-pad,
 *     composable with x/X, v0.21 — all on integers),
 *     path, call, field access, unary - !, binary arith/cmp/logic/bit,
 *     `as` casts, parens
 *   - types: primitive names, *T, raw *T
 *   - minimal type inference (v0.2): `x := e` bindings get a concrete C
 *     type from the initializer (integer literals default to i64), `mut`
 *     is enforced, and the generated C is plain C99 (no __auto_type), so
 *     TinyCC — the in-OS compiler — can build it.
 *   - static checks (v0.3 + v0.4, the N++ P1 checker): undeclared variables,
 *     unknown callees, wrong call arity, argument/parameter mismatches,
 *     binary-operand type errors (str in operators, pointer arithmetic,
 *     int/pointer mixing), return-value vs declared return type, and
 *     assignment value vs target type — all compile errors with file:line
 *     diagnostics.
 *   - struct declarations (v0.5, the first N++ P2 construct): named field
 *     records with literal construction `Rect{ w: 1, h: 2 }`, typed field
 *     access/assignment, by-value passing/returning — fully checked
 *     (unknown structs/fields, missing literal fields, mut-through-field).
 *   - enum + match (v0.7): tagged-union sum types with named payload
 *     fields, variant construction `Shape.Circle{ r: 5 }` (payload-less:
 *     `Shape.Empty`), and an exhaustive `match` statement that binds
 *     payload fields positionally — the Result foundation.
 *   - impl methods (v0.8, closing N++ P2): `impl Type { fn m(self, ...) }`
 *     attaches functions to structs and enums; calls dispatch statically
 *     as `expr.m(args)` and lower to plain C functions Type_m(self, ...).
 *     The receiver is by value and immutable.
 *   - match as an expression (v0.9, the P3 groundwork): `match` may appear
 *     as the value of a binding, an assignment, or a return; arms yield
 *     single expressions (`Variant(binds) => expr`), all arms must agree
 *     on one result type, and exhaustiveness is enforced as in the
 *     statement form. This is the shape `Result`/`?` will lower through.
 *   - the `?` operator (v0.10, N++ P3): `x := e?;` / `x = e?;` / `e?;`
 *     over "result enums" — any enum shaped exactly { Ok, Err } with at
 *     most one payload field each. On Err the function returns the error
 *     (rewrapped if the return type is a different result enum, defers
 *     honored); on Ok the payload is bound/assigned/discarded.
 *   - counted `for` loops (v0.11): `for i in a..b { }` over the half-open
 *     range [a, b); bounds are integers evaluated once, the loop variable
 *     is a fresh immutable i64 scoped to the body; break/continue work.
 *   - #[user] checked pointers (v0.12, opening N++ P4): `#[user] *T` is a
 *     distinct pointer flavor that never converts implicitly to or from
 *     plain pointers (the byte-pointer wildcards don't cross it either);
 *     `expr as #[user] *T` is the audited crossing. Marking user-facing
 *     syscall params makes every user-pointer handoff visible in source.
 *     Erased at codegen — same C type, zero runtime cost.
 *   - pageflags W^X bitset (v0.13, N++ P4 rung 2): a builtin type whose
 *     values are |-compositions of the predeclared PROT_* constants (the
 *     kernel's own mmap/mprotect numbers), so every value's bit set is
 *     statically known and WRITABLE|EXEC is a TOTAL compile-time error.
 *     Casts out to integers only (syscall arguments); casts in refused.
 *   - capabilities (v0.14, closing N++ P4): `#[caps(syscall)]` marks an
 *     extern syscall block as a gated kernel crossing; direct calls to
 *     its bindings then require the CALLING fn to hold the capability
 *     (declare it with the same attribute). Opt-in per block; a holding
 *     wrapper is the audited boundary, like unsafe-fn. Methods hold none.
 *   - indexing (v0.15, the M5 enabler): `p[i]` reads element i of a
 *     pointer (type = one level down), `s[i]` reads byte i of a str as
 *     u8. Bounds are the programmer's contract, as in C.
 *   - index writes (v0.16): `p[i] = x` (and += -=) stores through a
 *     POINTER element — the binding is not mutated, so `mut` is not
 *     required on the pointer. Writing through a str index stays
 *     refused: str is an immutable view (often read-only storage).
 *     Unlocks real buffers and stacks — the M5 arc's motivating case.
 *   - own structs (v0.17, opening N++ P5): `own struct File { fd: i64 }`
 *     is move-not-copy and must-consume. A locally-born own value MUST
 *     be returned or passed on (leak = compile error); using a binding
 *     after its move is an error; field reads peek without moving; own
 *     params arrive held (the callee is the owner of record). Branch-
 *     aware moves (v0.18): if/else arms may consume, but both exits
 *     must agree (an arm ending in `return` is exempt); moves stay
 *     refused in loops (bodies AND while conditions — they re-run) and
 *     match arms; own values cannot nest in plain structs/enums (an
 *     own struct may hold own fields since v0.25), alias
 *     through pointers, cross into syscalls, ride match expressions,
 *     or appear in defers. Destructors (v0.19): #[drop(fn)] wires an
 *     ordinary consuming fn as the type's destructor — a LIVE value
 *     reaching its scope end auto-drops (defers first, then drops in
 *     reverse birth order) instead of erroring; held params never
 *     auto-drop (the sink rule, which also makes drop recursion
 *     impossible); no drop flags — branch agreement still required.
 *     Zero-cost: the C layout is a plain struct.
 *   - missing-return flow analysis (v0.22): a typed fn/method must
 *     GUARANTEE a value on every path (tail expr, return, both-arm if,
 *     all-arm match, or a diverging never call); loops never guarantee.
 *     Checked here, not delegated to the C compiler (in-OS tcc is silent).
 * Not yet (N++ territory): closures, generic Result<T,E>, modules/use.
 */
/* Platform includes. In-OS builds (milestone M2) compile this file with
 * NyxOS's ported TinyCC, whose libc surface is the single header libc.h
 * (stdio/stdlib/string/va_* over the fd syscalls) — the same pattern the
 * self-hosted coreutils use. tcc always defines __TINYC__, and NyxOS is the
 * only tcc environment we target, so that macro selects the in-OS build:
 *     cc /mnt/ncc.c -I/usr/src/nyx -o /mnt/bin/ncc
 * Hosted builds (any C99 compiler) take the standard-headers branch. */
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
    if (!p) die("ncc: out of memory");
    return p;
}
static char* xstrndup(const char* s, size_t n) {
    char* p = xmalloc(n + 1);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* lexer                                                              */
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
/* parser                                                             */
/* ------------------------------------------------------------------ */
static Tok CUR, AHEAD;
static int HAS_AHEAD;

static Tok padv(void) {
    Tok p = CUR;
    if (HAS_AHEAD) { CUR = AHEAD; HAS_AHEAD = 0; }
    else CUR = next_token();
    return p;
}
static Tok ppeek2(void) {
    if (!HAS_AHEAD) { AHEAD = next_token(); HAS_AHEAD = 1; }
    return AHEAD;
}
static int pchk(TK k) { return CUR.k == k; }
static int pacc(TK k) { if (pchk(k)) { padv(); return 1; } return 0; }

/* N identifiers lower into the generated C verbatim — readable output is a
 * design goal — so a name that is a C keyword would compile fine as N and
 * then break the C build with a confusing downstream error (found the day
 * an example declared `fn double`). Reject such names at the N level with
 * a real diagnostic instead. Checked in pexp so every declaration site
 * (functions, params, bindings, loop vars, types, fields, variants) gets
 * it for free; C keywords can never be declared, so they can never appear
 * at a use site either. */
static const char* const C_KEYWORDS[] = {
    "auto","case","char","const","default","do","double","float","goto",
    "inline","int","long","register","restrict","short","signed","sizeof",
    "static","switch","typedef","union","unsigned","void","volatile",
    NULL   /* N's own keywords (if, while, return, ...) are lexed as
            * keywords and can never reach an identifier slot */
};
static void check_cname(const char* name, int line, const char* what) {
    for (int i = 0; C_KEYWORDS[i]; i++)
        if (!strcmp(name, C_KEYWORDS[i]))
            die("%s:%d: %s '%s' is a C keyword — generated code uses names verbatim, pick another name",
                FILENAME, line, what, name);
}

static Tok pexp(TK k, const char* what) {
    if (!pchk(k)) die("%s:%d: expected %s", FILENAME, CUR.line, what);
    if (k == T_IDENT) check_cname(CUR.s, CUR.line, what);
    return padv();
}

/* --- AST --- */
/* name==NULL means "no type" (void). is_user (v0.12, N++ P4) marks a
 * #[user] pointer: a distinct compile-time flavor that never converts
 * implicitly to or from plain pointers — the `as` cast is the audited
 * crossing point. Erased at codegen (same C type, zero runtime cost). */
typedef struct { char* name; int ptrs; int is_user; } Ty;

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Block Block;

typedef enum { E_INT, E_BOOL, E_STR, E_INTERP, E_PATH, E_CALL, E_FIELD,
               E_UN, E_BIN, E_CAST, E_SLIT, E_ELIT,
               E_INDEX } EK;               /* v0.15: e[i] byte/element read */

typedef struct { char* text; int tlen; Expr* e; int fmt; int width; int zero; } Frag;
/* Frag.fmt (v0.20): 0 = default (str as text, integers decimal),
 * 1 = ":x" lowercase hex, 2 = ":X" uppercase hex.
 * Frag.width/zero (v0.21): ":wN" right-aligns in N columns with spaces,
 * ":zN" pads with zeros; either composes with a trailing x/X. A spec is
 * always ONE identifier token — that is why width starts with a letter:
 * N identifiers may contain digits after the first letter, so the lexer
 * hands the whole spec over as a single T_IDENT and needs no changes
 * (a bare ":08x" would lex as an integer that forgets its leading zero,
 * or worse, trip the 0x hex-literal prefix). */
#define FMT_DEC  0
#define FMT_HEXL 1
#define FMT_HEXU 2

struct Expr {
    EK k;
    int line;
    long long ival;
    char* s; int slen;
    Frag* frags; int nfrags; int iid;
    char* name;
    Expr* callee; Expr** args; int nargs;   /* E_SLIT/E_ELIT reuse args as values */
    char** snames;                          /* E_SLIT/E_ELIT: field name per value */
    Expr* base; char* field;                /* E_ELIT: name = enum, field = variant */
    const char* op; Expr* l; Expr* r;
    Ty cast_to;
};

typedef enum { S_LET, S_ASSIGN, S_RET, S_EXPR, S_WHILE, S_IF, S_BREAK, S_CONT,
               S_DEFER, S_MATCH, S_FOR } SK;

typedef struct {
    char* variant;
    char** binds; int nbinds;   /* positional: bind i <- payload field i */
    struct Block* body;         /* statement form: arm body is a block */
    Expr* val;                  /* expression form (v0.9): arm yields this value */
    int line;
} MatchArm;

/* Where a v0.9 match-expression delivers its value. MT_STMT is the plain
 * statement form (arm bodies are blocks, no value). The expression form is
 * deliberately restricted to these three positions — each has a single,
 * obvious lowering into strict C99 (see gen_stmt), which keeps the
 * generated C readable and needs no statement-expressions. */
typedef enum { MT_STMT, MT_LET, MT_ASSIGN, MT_RET } MTarget;
struct Stmt {
    SK k;
    int line;
    int is_mut; char* name; Expr* e;
    Expr* lhs; const char* aop;
    Expr* cond; Block* body; Block* els;
    MatchArm* arms; int narms;              /* S_MATCH (subject in e) */
    int mtarget;                            /* S_MATCH: an MTarget value */
    int is_try;                             /* v0.10: statement value ends in `?` */
    /* S_FOR (v0.11): name = loop variable, e = range start, cond = range
     * end (half-open [start, end)), body = loop body */
};
struct Block { Stmt** st; int n; Expr* tail; int line; };   /* line: its `{` — where an
                                                             * empty body reports (v0.25) */

/* caps (v0.14, closing N++ P4): bit 0 = the `syscall` capability. On an
 * XFn it means "calling me is a kernel crossing and needs the capability";
 * on an Fn it means "I hold it". Enforced at direct call sites only —
 * a capability-holding wrapper is the audited boundary, exactly like
 * #[user]'s `as` cast and unsafe-fn in other languages. */
typedef struct { char* name; Ty ty; } Param;
typedef struct { char* name; Param* ps; int np; Ty ret; long long num; int caps; } XFn;
typedef struct { char* name; Param* ps; int np; Ty ret; Block* body; int caps; } Fn;

static XFn XFNS[64]; static int NXFN;
static Fn  FNS[256]; static int NFN;   /* 256: the selfhost generator crossed 128 */

/* struct declarations (v0.5): named field records, C-struct layout.
 * is_own (v0.17, N++ P5): a move-not-copy, must-consume type — the
 * checker tracks each binding's life; the C layout is unchanged. */
typedef struct { char* name; Param* fs; int nf; int is_own; char* drop_fn; int seq; } StructDef;
static StructDef STRUCTS[32]; static int NSTRUCTS;
static int DECL_SEQ;      /* v0.24: struct/enum declaration order, for the C layouts */

static StructDef* struct_lookup(const char* name) {
    for (int i = 0; i < NSTRUCTS; i++)
        if (!strcmp(STRUCTS[i].name, name)) return &STRUCTS[i];
    return NULL;
}

/* enum declarations (v0.7): tagged unions. Each variant has an optional
 * named-field payload; the C lowering is { int tag; union { ... } u; }. */
typedef struct { char* name; Param* fs; int nf; } VariantDef;
typedef struct { char* name; VariantDef* vs; int nv; int seq; } EnumDef;
static EnumDef ENUMS[16]; static int NENUMS;

static EnumDef* enum_lookup(const char* name) {
    for (int i = 0; i < NENUMS; i++)
        if (!strcmp(ENUMS[i].name, name)) return &ENUMS[i];
    return NULL;
}
static int variant_index(EnumDef* ed, const char* v) {
    for (int i = 0; i < ed->nv; i++)
        if (!strcmp(ed->vs[i].name, v)) return i;
    return -1;
}
/* A "result enum" (v0.10, spec 5.9) is the structural shape `?` operates
 * on: exactly the two variants Ok and Err, each carrying zero or one
 * payload field. Any enum matching the shape qualifies — N has no
 * generics yet, so Result<T, E> is a convention, not a type former. */
static int enum_is_result(EnumDef* ed) {
    if (ed->nv != 2) return 0;
    if (variant_index(ed, "Ok") < 0 || variant_index(ed, "Err") < 0) return 0;
    return ed->vs[0].nf <= 1 && ed->vs[1].nf <= 1;
}
/* Is this expression a payload-less variant reference (`Shape.Empty`)?
 * Returns the enum and sets *idx, or NULL. */
static EnumDef* variant_ref(Expr* e, int* idx) {
    if (e->k != E_FIELD || e->base->k != E_PATH) return NULL;
    EnumDef* ed = enum_lookup(e->base->name);
    if (!ed) return NULL;
    *idx = variant_index(ed, e->field);
    return ed;
}

/* impl methods (v0.8): functions attached to a struct or enum. Dispatch is
 * static — the receiver's inferred type picks the method at compile time;
 * the lowering is a plain C function Type_name(Type self, ...). */
typedef struct { char* type; char* name; Param* ps; int np; Ty ret; Block* body; } Method;
static Method METHODS[64]; static int NMETHODS;

static Method* method_lookup(const char* type, const char* name) {
    for (int i = 0; i < NMETHODS; i++)
        if (!strcmp(METHODS[i].type, type) && !strcmp(METHODS[i].name, name))
            return &METHODS[i];
    return NULL;
}

/* Struct literals `Name{ ... }` are valid anywhere EXCEPT directly in an
 * if/while condition, where `{` must open the body block instead (the same
 * rule Rust uses). The parser sets this flag around condition parsing. */
static int NO_SLIT;

static Expr* newe(EK k) { Expr* e = xmalloc(sizeof(Expr)); e->k = k; e->line = CUR.line; return e; }
static Stmt* news(SK k) { Stmt* s = xmalloc(sizeof(Stmt)); s->k = k; s->line = CUR.line; return s; }

/* --- function types (v0.24) ------------------------------------------- */
/* `fn(A, B) -> R` is a type. Each distinct signature is interned once and
 * named `__nyx_fn<N>` — the C typedef of the matching function pointer —
 * so a fn type travels through Ty as an ordinary type name: two fn types
 * are the same type exactly when their signatures are identical, and the
 * places that render or compare types (emit_type, ty_compat) need no new
 * case. `pretty` is the N spelling, for diagnostics. */
typedef struct { char* cname; char* pretty; Ty* ps; int np; Ty ret; } FnTy;
static FnTy FNTYS[32];
static int NFNTYS;
static int EMITTED_FNTYS;                 /* typedefs already written to the C */

static int ty_same(Ty a, Ty b) {
    if (!a.name || !b.name) return !a.name && !b.name;
    return a.ptrs == b.ptrs && a.is_user == b.is_user && !strcmp(a.name, b.name);
}

/* A type inside a signature's spelling: a nested function type reads as
 * its own spelling, never as its typedef name. */
static const char* fnty_name_in(Ty t) {
    if (t.name && !t.ptrs)
        for (int i = 0; i < NFNTYS; i++)
            if (!strcmp(FNTYS[i].cname, t.name)) return FNTYS[i].pretty;
    return t.name;
}

/* The interned index of this signature, added on first sight. */
static int fnty_intern(Ty* ps, int np, Ty ret) {
    for (int i = 0; i < NFNTYS; i++) {
        if (FNTYS[i].np != np || !ty_same(FNTYS[i].ret, ret)) continue;
        int same = 1;
        for (int p = 0; p < np; p++)
            if (!ty_same(FNTYS[i].ps[p], ps[p])) { same = 0; break; }
        if (same) return i;
    }
    if (NFNTYS >= 32) die("%s: too many distinct function types", FILENAME);
    FnTy* f = &FNTYS[NFNTYS];
    f->np = np;
    f->ps = xmalloc(sizeof(Ty) * (size_t)(np ? np : 1));
    for (int p = 0; p < np; p++) f->ps[p] = ps[p];
    f->ret = ret;
    f->cname = xmalloc(24);
    sprintf(f->cname, "__nyx_fn%d", NFNTYS);
    char* b = xmalloc(1024);
    int n = sprintf(b, "fn(");
    for (int p = 0; p < np && n < 900; p++) {
        if (p) n += sprintf(b + n, ", ");
        if (ps[p].is_user) n += sprintf(b + n, "#[user] ");
        for (int k = 0; k < ps[p].ptrs; k++) b[n++] = '*';
        n += sprintf(b + n, "%s", fnty_name_in(ps[p]));
    }
    n += sprintf(b + n, ")");
    if (ret.name && n < 900) {
        n += sprintf(b + n, " -> ");
        if (ret.is_user) n += sprintf(b + n, "#[user] ");
        for (int k = 0; k < ret.ptrs; k++) b[n++] = '*';
        n += sprintf(b + n, "%s", fnty_name_in(ret));
    }
    b[n] = 0;
    f->pretty = b;
    return NFNTYS++;
}

/* The fn type a Ty names, or -1. */
static int fnty_of(Ty t) {
    if (!t.name || t.ptrs) return -1;
    for (int i = 0; i < NFNTYS; i++)
        if (!strcmp(FNTYS[i].cname, t.name)) return i;
    return -1;
}

static Ty fnty_ty(int i) { Ty t; t.name = FNTYS[i].cname; t.ptrs = 0; t.is_user = 0; return t; }

static Ty parse_type(void) {
    Ty t; t.ptrs = 0; t.name = NULL; t.is_user = 0;
    int line = CUR.line;
    t.is_user = pacc(T_ATTR_USER);        /* #[user] *T — checked pointer (v0.12) */
    int got_raw = pacc(T_KW_RAW);         /* raw *T and *T lower identically in C */
    if (t.is_user && got_raw)
        die("%s:%d: #[user] and raw are mutually exclusive — raw is the explicit opt-out",
            FILENAME, line);
    while (pacc(T_STAR)) t.ptrs++;
    if (pchk(T_KW_FN)) {                  /* fn(A, B) -> R: a function type (v0.24) */
        padv();
        if (t.ptrs || t.is_user || got_raw)
            die("%s:%d: a function type is a value of its own — no pointer, raw or #[user] on it (v0.24)",
                FILENAME, line);
        pexp(T_LP, "'(' after fn in a type");
        Ty ps[16]; int np = 0;
        if (!pchk(T_RP)) {
            do {
                if (np >= 16) die("%s:%d: too many parameters in a function type", FILENAME, line);
                ps[np++] = parse_type();
            } while (pacc(T_COMMA));
        }
        pexp(T_RP, "')' closing the function type's parameters");
        Ty ret; ret.name = NULL; ret.ptrs = 0; ret.is_user = 0;
        if (pacc(T_ARROW)) ret = parse_type();
        return fnty_ty(fnty_intern(ps, np, ret));
    }
    Tok id = pexp(T_IDENT, "type name");
    t.name = id.s;
    if (t.is_user && t.ptrs == 0)
        die("%s:%d: #[user] applies only to pointer types (got %s)",
            FILENAME, line, t.name);
    return t;
}

static Expr* parse_expr(void);

static Expr* parse_interp(void) {
    Expr* e = newe(E_INTERP);
    Frag* fr = xmalloc(sizeof(Frag) * 64);
    int nf = 0;
    Tok h = padv();                       /* T_STR_HEAD */
    fr[nf].text = h.s; fr[nf].tlen = h.slen; fr[nf].e = NULL;
    fr[nf].fmt = FMT_DEC; fr[nf].width = 0; fr[nf].zero = 0; nf++;
    for (;;) {
        if (nf + 2 >= 64) die("%s:%d: too many interpolations", FILENAME, e->line);
        fr[nf].text = NULL; fr[nf].tlen = 0;
        fr[nf].fmt = FMT_DEC; fr[nf].width = 0; fr[nf].zero = 0;
        fr[nf].e = parse_expr();
        if (pacc(T_COLON)) {              /* v0.20/v0.21: "{expr:spec}" —
                                           * spec := [wN|zN][x|X] | x | X   */
            Tok ft = pexp(T_IDENT, "format spec after ':'");
            const char* s = ft.s;
            int i = 0;
            if (s[0] == 'w' || s[0] == 'z') {
                fr[nf].zero = (s[0] == 'z');
                i = 1;
                int wd = 0;
                while (s[i] >= '0' && s[i] <= '9') { wd = wd * 10 + (s[i] - '0'); i++; }
                if (i == 1 || wd <= 0 || wd > 128) wd = -1;   /* no/absurd digits */
                fr[nf].width = wd;
            }
            if (s[i] == 'x')      { fr[nf].fmt = FMT_HEXL; i++; }
            else if (s[i] == 'X') { fr[nf].fmt = FMT_HEXU; i++; }
            if (i != ft.slen || fr[nf].width < 0)
                die("%s:%d: unknown format spec ':%s' — the format specs are :x, :X (hex), :wN (width), :zN (zero-pad), and wN/zN composed with a trailing x or X",
                    FILENAME, e->line, ft.s);
        }
        nf++;
        pexp(T_INTERP_R, "'}' closing interpolation");
        if (pchk(T_STR_MID)) {
            Tok m = padv();
            fr[nf].text = m.s; fr[nf].tlen = m.slen; fr[nf].e = NULL;
            fr[nf].fmt = FMT_DEC; fr[nf].width = 0; fr[nf].zero = 0; nf++;
            continue;
        }
        if (pchk(T_STR_TAIL)) {
            Tok t2 = padv();
            fr[nf].text = t2.s; fr[nf].tlen = t2.slen; fr[nf].e = NULL;
            fr[nf].fmt = FMT_DEC; fr[nf].width = 0; fr[nf].zero = 0; nf++;
            break;
        }
        die("%s:%d: expected string continuation after interpolation", FILENAME, CUR.line);
    }
    e->frags = fr; e->nfrags = nf;
    return e;
}

static Expr* parse_primary(void) {
    if (pchk(T_INT))  { Tok t = padv(); Expr* e = newe(E_INT);  e->ival = t.ival; return e; }
    if (pchk(T_KW_TRUE) || pchk(T_KW_FALSE)) {
        Expr* e = newe(E_BOOL);
        e->ival = pchk(T_KW_TRUE);
        padv();
        return e;
    }
    if (pchk(T_STR))  { Tok t = padv(); Expr* e = newe(E_STR);  e->s = t.s; e->slen = t.slen; return e; }
    if (pchk(T_STR_HEAD)) return parse_interp();
    if (pchk(T_LP)) {                 /* parens re-enable struct/enum literals
                                       * inside if/while/match headers (v0.9,
                                       * the same escape hatch Rust uses) */
        padv();
        int slit_save = NO_SLIT;
        NO_SLIT = 0;
        Expr* e = parse_expr();
        NO_SLIT = slit_save;
        pexp(T_RP, "')'");
        return e;
    }
    if (pchk(T_IDENT)) {
        Tok t = padv();
        if (pchk(T_LB) && !NO_SLIT) {           /* struct literal: Name{ f: e, ... } */
            padv();
            Expr* e = newe(E_SLIT);
            e->name = t.s;
            e->args = xmalloc(sizeof(Expr*) * 16);
            e->snames = xmalloc(sizeof(char*) * 16);
            e->nargs = 0;
            if (!pchk(T_RB)) {
                do {
                    if (pchk(T_RB)) break;      /* trailing comma */
                    if (e->nargs >= 16) die("%s:%d: too many fields in literal", FILENAME, CUR.line);
                    Tok f = pexp(T_IDENT, "field name");
                    pexp(T_COLON, "':' after field name");
                    e->snames[e->nargs] = f.s;
                    e->args[e->nargs] = parse_expr();
                    e->nargs++;
                } while (pacc(T_COMMA));
            }
            pexp(T_RB, "'}' closing struct literal");
            return e;
        }
        Expr* e = newe(E_PATH);
        e->name = t.s;
        return e;
    }
    die("%s:%d: expected an expression", FILENAME, CUR.line);
    return NULL;
}

static Expr* parse_postfix(void) {
    Expr* e = parse_primary();
    for (;;) {
        if (pchk(T_DOT)) {
            int dline = CUR.line;
            padv();
            Tok f = pexp(T_IDENT, "field name");
            if (pchk(T_LP)) {                   /* method call: recv.m(args) */
                padv();
                Expr* fe = newe(E_FIELD);
                fe->base = e; fe->field = f.s;
                Expr* c = newe(E_CALL);
                c->callee = fe;
                Expr** as = xmalloc(sizeof(Expr*) * 16);
                int na = 0;
                if (!pchk(T_RP)) {
                    do {
                        if (na >= 16) die("%s:%d: too many call arguments", FILENAME, CUR.line);
                        as[na++] = parse_expr();
                    } while (pacc(T_COMMA));
                }
                pexp(T_RP, "')'");
                c->args = as; c->nargs = na;
                e = c;
                continue;
            }
            if (pchk(T_LB) && !NO_SLIT && e->k == E_PATH) {
                /* enum-variant literal: Enum.Variant{ field: value, ... } */
                padv();
                Expr* v = newe(E_ELIT);
                v->name = e->name;              /* enum */
                v->field = f.s;                 /* variant */
                v->args = xmalloc(sizeof(Expr*) * 16);
                v->snames = xmalloc(sizeof(char*) * 16);
                v->nargs = 0;
                if (!pchk(T_RB)) {
                    do {
                        if (pchk(T_RB)) break;
                        if (v->nargs >= 16) die("%s:%d: too many fields", FILENAME, CUR.line);
                        Tok fn_ = pexp(T_IDENT, "field name");
                        pexp(T_COLON, "':' after field name");
                        v->snames[v->nargs] = fn_.s;
                        v->args[v->nargs] = parse_expr();
                        v->nargs++;
                    } while (pacc(T_COMMA));
                }
                pexp(T_RB, "'}' closing variant literal");
                e = v;
                continue;
            }
            Expr* fe = newe(E_FIELD);
            fe->base = e; fe->field = f.s;
            fe->line = dline;             /* the `.` line: where the access is written,
                                           * so a refused move of a block's tail is
                                           * reported there and not at the `}` after it */
            e = fe;
            continue;
        }
        if (pchk(T_LP)) {
            padv();
            Expr* c = newe(E_CALL);
            c->callee = e;
            Expr** as = xmalloc(sizeof(Expr*) * 16);
            int na = 0;
            if (!pchk(T_RP)) {
                do {
                    if (na >= 16) die("%s:%d: too many call arguments", FILENAME, CUR.line);
                    as[na++] = parse_expr();
                } while (pacc(T_COMMA));
            }
            pexp(T_RP, "')'");
            c->args = as; c->nargs = na;
            e = c;
            continue;
        }
        if (pchk(T_LBRACK)) {               /* v0.15: e[i] indexing (read) */
            padv();
            Expr* ix = newe(E_INDEX);
            ix->l = e;
            ix->r = parse_expr();
            pexp(T_RBRACK, "']' closing the index");
            e = ix;
            continue;
        }
        break;
    }
    return e;
}

static Expr* parse_unary(void) {
    if (pchk(T_MINUS) || pchk(T_NOT)) {
        const char* op = pchk(T_MINUS) ? "-" : "!";
        padv();
        Expr* e = newe(E_UN);
        e->op = op; e->r = parse_unary();
        return e;
    }
    return parse_postfix();
}

static Expr* parse_cast(void) {
    Expr* e = parse_unary();
    while (pacc(T_KW_AS)) {
        Expr* c = newe(E_CAST);
        c->l = e; c->cast_to = parse_type();
        e = c;
    }
    return e;
}

static Expr* mkbin(const char* op, Expr* l, Expr* r) {
    Expr* e = newe(E_BIN);
    e->op = op; e->l = l; e->r = r;
    return e;
}

static Expr* parse_mul(void) {
    Expr* l = parse_cast();
    for (;;) {
        const char* op = pchk(T_STAR) ? "*" : pchk(T_SLASH) ? "/" : pchk(T_PERCENT) ? "%" : NULL;
        if (!op) return l;
        padv();
        l = mkbin(op, l, parse_cast());
    }
}
static Expr* parse_add(void) {
    Expr* l = parse_mul();
    for (;;) {
        const char* op = pchk(T_PLUS) ? "+" : pchk(T_MINUS) ? "-" : NULL;
        if (!op) return l;
        padv();
        l = mkbin(op, l, parse_mul());
    }
}
static Expr* parse_shift(void) {
    Expr* l = parse_add();
    for (;;) {
        const char* op = pchk(T_SHL) ? "<<" : pchk(T_SHR) ? ">>" : NULL;
        if (!op) return l;
        padv();
        l = mkbin(op, l, parse_add());
    }
}
static Expr* parse_band(void) {
    Expr* l = parse_shift();
    while (pchk(T_AMP)) { padv(); l = mkbin("&", l, parse_shift()); }
    return l;
}
static Expr* parse_bxor(void) {
    Expr* l = parse_band();
    while (pchk(T_CARET)) { padv(); l = mkbin("^", l, parse_band()); }
    return l;
}
static Expr* parse_bor(void) {
    Expr* l = parse_bxor();
    while (pchk(T_PIPE)) { padv(); l = mkbin("|", l, parse_bxor()); }
    return l;
}
static Expr* parse_cmp(void) {
    Expr* l = parse_bor();
    for (;;) {
        const char* op = pchk(T_EQ) ? "==" : pchk(T_NE) ? "!=" :
                         pchk(T_LE) ? "<=" : pchk(T_GE) ? ">=" :
                         pchk(T_LT) ? "<"  : pchk(T_GT) ? ">"  : NULL;
        if (!op) return l;
        padv();
        l = mkbin(op, l, parse_bor());
    }
}
static Expr* parse_and(void) {
    Expr* l = parse_cmp();
    while (pchk(T_ANDAND)) { padv(); l = mkbin("&&", l, parse_cmp()); }
    return l;
}
static Expr* parse_or(void) {
    Expr* l = parse_and();
    while (pchk(T_OROR)) { padv(); l = mkbin("||", l, parse_and()); }
    return l;
}
static Expr* parse_expr(void) { return parse_or(); }

/* --- statements / blocks --- */
static Block* parse_block(void);

static Stmt* parse_if_stmt(void) {
    padv();                               /* 'if' */
    Stmt* s = news(S_IF);
    NO_SLIT = 1;                          /* `{` after the condition opens the body */
    s->cond = parse_expr();
    NO_SLIT = 0;
    s->body = parse_block();
    if (pacc(T_KW_ELSE)) {
        if (pchk(T_KW_IF)) {              /* else-if: wrap in a one-stmt block */
            Block* b = xmalloc(sizeof(Block));
            b->st = xmalloc(sizeof(Stmt*));
            b->st[0] = parse_if_stmt();
            b->n = 1; b->tail = NULL;
            s->els = b;
        } else {
            s->els = parse_block();
        }
    }
    return s;
}

/* Parse `match <subject> { arms }` with the `match` keyword already
 * consumed. Shared by the statement form (expr_arms == 0, arm bodies are
 * blocks) and the v0.9 expression form (expr_arms == 1, each arm yields a
 * single expression). The caller sets s->mtarget and the target fields. */
static Stmt* parse_match(int expr_arms) {
    Stmt* s = news(S_MATCH);
    NO_SLIT = 1;                  /* `{` after the subject opens the arms */
    s->e = parse_expr();
    NO_SLIT = 0;
    pexp(T_LB, "'{' opening match arms");
    MatchArm* arms = xmalloc(sizeof(MatchArm) * 16);
    int na = 0;
    while (!pchk(T_RB)) {
        if (pchk(T_EOF)) die("%s: unexpected EOF in match", FILENAME);
        if (na >= 16) die("%s:%d: too many match arms", FILENAME, CUR.line);
        MatchArm* a = &arms[na];
        a->line = CUR.line;
        Tok v = pexp(T_IDENT, "variant name");
        a->variant = v.s;
        a->binds = xmalloc(sizeof(char*) * 8);
        a->nbinds = 0;
        if (pacc(T_LP)) {
            do {
                if (a->nbinds >= 8) die("%s:%d: too many binds", FILENAME, CUR.line);
                a->binds[a->nbinds++] = pexp(T_IDENT, "binding name").s;
            } while (pacc(T_COMMA));
            pexp(T_RP, "')'");
        }
        pexp(T_FATARROW, "'=>' after the pattern");
        if (expr_arms) a->val = parse_expr();
        else           a->body = parse_block();
        na++;
        if (!pacc(T_COMMA)) break;
    }
    pexp(T_RB, "'}' closing match");
    s->arms = arms;
    s->narms = na;
    return s;
}

static Block* parse_block(void) {
    Tok lb = pexp(T_LB, "'{'");
    Block* b = xmalloc(sizeof(Block));
    b->line = lb.line;                    /* an empty block reports at its brace */
    Stmt** st = xmalloc(sizeof(Stmt*) * 256);
    int n = 0;
    while (!pchk(T_RB)) {
        if (pchk(T_EOF)) die("%s:%d: unexpected EOF inside block", FILENAME, CUR.line);
        if (n >= 256) die("%s:%d: too many statements in block", FILENAME, CUR.line);

        if (pchk(T_KW_MUT) || (pchk(T_IDENT) && ppeek2().k == T_WALRUS)) {
            int is_mut = pacc(T_KW_MUT);
            Tok id = pexp(T_IDENT, "variable name");
            pexp(T_WALRUS, "':='");
            if (pchk(T_KW_MATCH)) {       /* v0.9: x := match subj { ... }; */
                padv();
                Stmt* m = parse_match(1);
                m->mtarget = MT_LET;
                m->name = id.s;
                m->is_mut = is_mut;
                pexp(T_SEMI, "';'");
                st[n++] = m;
                continue;
            }
            Stmt* s = news(S_LET);
            s->is_mut = is_mut;
            s->name = id.s;
            s->e = parse_expr();
            s->is_try = pacc(T_QUESTION);     /* v0.10: x := e?; */
            pexp(T_SEMI, "';'");
            st[n++] = s;
            continue;
        }
        if (pacc(T_KW_RETURN)) {
            if (pchk(T_KW_MATCH)) {       /* v0.9: return match subj { ... }; */
                padv();
                Stmt* m = parse_match(1);
                m->mtarget = MT_RET;
                pexp(T_SEMI, "';'");
                st[n++] = m;
                continue;
            }
            Stmt* s = news(S_RET);
            if (!pchk(T_SEMI)) s->e = parse_expr();
            pexp(T_SEMI, "';'");
            st[n++] = s;
            continue;
        }
        if (pacc(T_KW_BREAK))    { pexp(T_SEMI, "';'"); st[n++] = news(S_BREAK); continue; }
        if (pacc(T_KW_CONTINUE)) { pexp(T_SEMI, "';'"); st[n++] = news(S_CONT);  continue; }
        if (pacc(T_KW_DEFER)) {
            Stmt* s = news(S_DEFER);
            s->e = parse_expr();
            pexp(T_SEMI, "';'");
            st[n++] = s;
            continue;
        }
        if (pchk(T_KW_WHILE)) {
            padv();
            Stmt* s = news(S_WHILE);
            NO_SLIT = 1;                  /* `{` after the condition opens the body */
            s->cond = parse_expr();
            NO_SLIT = 0;
            s->body = parse_block();
            st[n++] = s;
            continue;
        }
        if (pchk(T_KW_IF)) { st[n++] = parse_if_stmt(); continue; }
        if (pchk(T_KW_FOR)) {                 /* v0.11: for i in a..b { } */
            padv();
            Stmt* s = news(S_FOR);
            Tok id = pexp(T_IDENT, "loop variable name");
            s->name = id.s;
            pexp(T_KW_IN, "'in' after the loop variable");
            NO_SLIT = 1;                      /* `{` after the range opens the body */
            s->e = parse_expr();
            pexp(T_DOTDOT, "'..' in the range");
            s->cond = parse_expr();
            NO_SLIT = 0;
            s->body = parse_block();
            st[n++] = s;
            continue;
        }
        if (pchk(T_KW_MATCH)) {
            padv();
            st[n++] = parse_match(0);     /* statement form (mtarget = MT_STMT) */
            continue;
        }

        /* expression, assignment, or block tail */
        Expr* e = parse_expr();
        if (pchk(T_ASSIGN) || pchk(T_PLUSEQ) || pchk(T_MINUSEQ)) {
            const char* op = pchk(T_ASSIGN) ? "=" : pchk(T_PLUSEQ) ? "+=" : "-=";
            padv();
            if (pchk(T_KW_MATCH)) {       /* v0.9: x = match subj { ... }; */
                padv();
                Stmt* m = parse_match(1);
                m->mtarget = MT_ASSIGN;
                m->lhs = e;
                m->aop = op;
                pexp(T_SEMI, "';'");
                st[n++] = m;
                continue;
            }
            Stmt* s = news(S_ASSIGN);
            s->lhs = e; s->aop = op; s->e = parse_expr();
            s->is_try = pacc(T_QUESTION);     /* v0.10: x = e?; */
            pexp(T_SEMI, "';'");
            st[n++] = s;
            continue;
        }
        if (pacc(T_QUESTION)) {               /* v0.10: e?; — propagate only */
            Stmt* s = news(S_EXPR);
            s->e = e;
            s->is_try = 1;
            pexp(T_SEMI, "';'");
            st[n++] = s;
            continue;
        }
        if (pacc(T_SEMI)) {
            Stmt* s = news(S_EXPR);
            s->e = e;
            st[n++] = s;
            continue;
        }
        if (pchk(T_RB)) { b->tail = e; break; }
        die("%s:%d: expected ';' or '}' after expression", FILENAME, CUR.line);
    }
    pexp(T_RB, "'}'");
    b->st = st; b->n = n;
    return b;
}

/* --- items --- */
static Param* parse_params(int* out_n) {
    pexp(T_LP, "'('");
    Param* ps = xmalloc(sizeof(Param) * 16);
    int np = 0;
    if (!pchk(T_RP)) {
        do {
            if (np >= 16) die("%s:%d: too many parameters", FILENAME, CUR.line);
            Tok id = pexp(T_IDENT, "parameter name");
            pexp(T_COLON, "':'");
            ps[np].name = id.s;
            ps[np].ty = parse_type();
            np++;
        } while (pacc(T_COMMA));
    }
    pexp(T_RP, "')'");
    *out_n = np;
    return ps;
}

static void parse_extern_block(int caps) {
    pexp(T_KW_SYSCALL, "'syscall'");
    pexp(T_LB, "'{'");
    while (pacc(T_KW_FN)) {
        if (NXFN >= 64) die("too many extern syscalls");
        XFn* x = &XFNS[NXFN++];
        x->caps = caps;                   /* gated block: callers need the cap */
        Tok id = pexp(T_IDENT, "syscall name");
        x->name = id.s;
        x->ps = parse_params(&x->np);
        x->ret.name = NULL; x->ret.ptrs = 0;
        if (pacc(T_ARROW)) x->ret = parse_type();
        pexp(T_ASSIGN, "'=' before the syscall number");
        Tok num = pexp(T_INT, "syscall number");
        x->num = num.ival;
        pacc(T_SEMI);                     /* trailing ';' is optional here */
    }
    pexp(T_RB, "'}'");
}

static void parse_fn(int caps) {
    if (NFN >= 256) die("too many functions");
    Fn* f = &FNS[NFN++];
    f->caps = caps;                       /* fn declares the capabilities it holds */
    Tok id = pexp(T_IDENT, "function name");
    f->name = id.s;
    f->ps = parse_params(&f->np);
    f->ret.name = NULL; f->ret.ptrs = 0;
    if (pacc(T_ARROW)) f->ret = parse_type();
    f->body = parse_block();
}

static void parse_struct(int is_own) {
    if (NSTRUCTS >= 32) die("too many structs");
    StructDef* sd = &STRUCTS[NSTRUCTS++];
    sd->is_own = is_own;
    sd->seq = DECL_SEQ++;
    Tok id = pexp(T_IDENT, "struct name");
    sd->name = id.s;
    pexp(T_LB, "'{'");
    Param* fs = xmalloc(sizeof(Param) * 16);
    int nf = 0;
    while (!pchk(T_RB)) {
        if (pchk(T_EOF)) die("%s: unexpected EOF in struct '%s'", FILENAME, sd->name);
        if (nf >= 16) die("%s: too many fields in struct '%s'", FILENAME, sd->name);
        Tok f = pexp(T_IDENT, "field name");
        pexp(T_COLON, "':' after field name");
        fs[nf].name = f.s;
        fs[nf].ty = parse_type();
        nf++;
        if (!pacc(T_COMMA)) break;
    }
    pexp(T_RB, "'}' closing struct");
    sd->fs = fs;
    sd->nf = nf;
    if (nf == 0) die("%s: struct '%s' has no fields", FILENAME, sd->name);
}

static void parse_enum(void) {
    if (NENUMS >= 16) die("too many enums");
    EnumDef* ed = &ENUMS[NENUMS++];
    ed->seq = DECL_SEQ++;
    Tok id = pexp(T_IDENT, "enum name");
    ed->name = id.s;
    pexp(T_LB, "'{'");
    VariantDef* vs = xmalloc(sizeof(VariantDef) * 16);
    int nv = 0;
    while (!pchk(T_RB)) {
        if (pchk(T_EOF)) die("%s: unexpected EOF in enum '%s'", FILENAME, ed->name);
        if (nv >= 16) die("%s: too many variants in enum '%s'", FILENAME, ed->name);
        Tok v = pexp(T_IDENT, "variant name");
        vs[nv].name = v.s;
        vs[nv].fs = NULL;
        vs[nv].nf = 0;
        if (pacc(T_LP)) {                        /* optional named-field payload */
            Param* fs = xmalloc(sizeof(Param) * 8);
            int nf = 0;
            do {
                if (nf >= 8) die("%s: too many payload fields", FILENAME);
                Tok f = pexp(T_IDENT, "payload field name");
                pexp(T_COLON, "':'");
                fs[nf].name = f.s;
                fs[nf].ty = parse_type();
                nf++;
            } while (pacc(T_COMMA));
            pexp(T_RP, "')'");
            vs[nv].fs = fs;
            vs[nv].nf = nf;
        }
        nv++;
        if (!pacc(T_COMMA)) break;
    }
    pexp(T_RB, "'}' closing enum");
    ed->vs = vs;
    ed->nv = nv;
    if (nv == 0) die("%s: enum '%s' has no variants", FILENAME, ed->name);
}

static void parse_impl(void) {
    Tok id = pexp(T_IDENT, "type name after 'impl'");
    pexp(T_LB, "'{'");
    while (pacc(T_KW_FN)) {
        if (NMETHODS >= 64) die("too many methods");
        Method* m = &METHODS[NMETHODS++];
        m->type = id.s;
        Tok mn = pexp(T_IDENT, "method name");
        m->name = mn.s;
        pexp(T_LP, "'('");
        Tok self_ = pexp(T_IDENT, "'self' as the first parameter");
        if (strcmp(self_.s, "self") != 0)
            die("%s:%d: the first parameter of a method must be 'self'", FILENAME, self_.line);
        Param* ps = xmalloc(sizeof(Param) * 16);
        int np = 0;
        while (pacc(T_COMMA)) {
            if (np >= 16) die("%s: too many parameters in method '%s'", FILENAME, m->name);
            Tok pn = pexp(T_IDENT, "parameter name");
            pexp(T_COLON, "':'");
            ps[np].name = pn.s;
            ps[np].ty = parse_type();
            np++;
        }
        pexp(T_RP, "')'");
        m->ps = ps; m->np = np;
        m->ret.name = NULL; m->ret.ptrs = 0;
        if (pacc(T_ARROW)) m->ret = parse_type();
        m->body = parse_block();
    }
    pexp(T_RB, "'}' closing impl");
}

static void parse_program(void) {
    for (;;) {
        if (pchk(T_EOF)) return;
        int caps = 0;                     /* v0.14: #[caps(syscall)] item attr */
        if (pacc(T_ATTR_CAPS_SYSCALL)) caps = 1;
        if (pacc(T_KW_EXTERN)) { parse_extern_block(caps); continue; }
        if (pacc(T_KW_FN))     { parse_fn(caps); continue; }
        if (caps)
            die("%s:%d: #[caps(syscall)] applies to a fn or an extern syscall block",
                FILENAME, CUR.line);
        if (pchk(T_ATTR_DROP)) {          /* v0.19: #[drop(fn)] own struct */
            Tok dt = padv();
            pexp(T_KW_OWN, "'own' after #[drop(...)] — only an own struct has a destructor");
            pexp(T_KW_STRUCT, "'struct' after 'own'");
            parse_struct(1);
            STRUCTS[NSTRUCTS - 1].drop_fn = dt.s;
            continue;
        }
        if (pacc(T_KW_OWN)) {             /* v0.17: own struct — must-consume type */
            pexp(T_KW_STRUCT, "'struct' after 'own'");
            parse_struct(1);
            continue;
        }
        if (pacc(T_KW_STRUCT)) { parse_struct(0); continue; }
        if (pacc(T_KW_ENUM))   { parse_enum(); continue; }
        if (pacc(T_KW_IMPL))   { parse_impl(); continue; }
        die("%s:%d: expected 'extern', 'fn', 'struct', 'enum' or 'impl' at top level",
            FILENAME, CUR.line);
    }
}

/* ------------------------------------------------------------------ */
/* codegen                                                            */
/* ------------------------------------------------------------------ */
static FILE* OUT;
static int IID;                            /* per-function interp counter */
static Ty CUR_RET;

/* defer (v0.6): function-scoped, Go-style. Deferred expressions register in
 * textual order and run in LIFO order on EVERY exit path — each early
 * `return`, the body's tail expression, and (for void functions) falling
 * off the end. Restricted to the function's outermost block so that a
 * static lowering is exact (no runtime defer stack needed). */
static Expr* FN_DEFERS[16];
static int   NDEFERS;
static int   BLOCK_DEPTH;                  /* 1 = the function's own block */
static int   LOOP_DEPTH;                   /* v0.18: >0 = inside a loop (body OR
                                            * a while condition — both re-run) */
static int   MATCH_DEPTH;                  /* v0.18: >0 = inside a match arm body */

static const char* base_ctype(const char* n) {
    static const struct { const char* a; const char* b; } M[] = {
        {"i8", "nyx_i8"}, {"u8", "nyx_u8"}, {"i16", "nyx_i16"}, {"u16", "nyx_u16"},
        {"i32", "nyx_i32"}, {"u32", "nyx_u32"}, {"i64", "nyx_i64"}, {"u64", "nyx_u64"},
        {"isize", "nyx_isize"}, {"usize", "nyx_usize"}, {"addr", "nyx_addr"},
        {"bool", "nyx_bool"}, {"str", "nyx_str"}, {"never", "void"}, {"void", "void"},
        {"pageflags", "nyx_i64"},   /* W^X-checked at compile time; plain bits in C */
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (!strcmp(n, M[i].a)) return M[i].b;
    return n;                             /* user types pass through */
}
static void emit_type(Ty t) {
    if (!t.name) { fputs("void", OUT); return; }
    fputs(base_ctype(t.name), OUT);
    for (int i = 0; i < t.ptrs; i++) fputc('*', OUT);
}
static int is_never(Ty t) { return t.name && !strcmp(t.name, "never"); }

/* --- minimal type inference (v0.2) ------------------------------------ */
/* A flat per-function symbol table with block scoping via save/restore of
 * the counter (gen_block records NVARS on entry and truncates on exit).
 * This is deliberately not a full checker: it exists to give `:=` bindings
 * a concrete C type, to dispatch interpolation by type, and to enforce
 * `mut` — the three things codegen cannot do blind. The full checker is
 * N++ P1 (lang/docs/design-npp.md). */
/* pmask (v0.13): for a pageflags binding, the statically-known flag-bit
 * set — what the W^X check reasons about. -1 = unknown (parameters).
 * own_state (v0.17): OWN_NONE for ordinary bindings; a locally-born own
 * value is OWN_LIVE (it MUST be consumed), a consumed one is OWN_MOVED
 * (any further use is an error), and an own parameter is OWN_HELD —
 * usable and movable, but this function is already its owner-of-record,
 * so it carries no local consumption obligation. */
#define OWN_NONE  0
#define OWN_LIVE  1
#define OWN_MOVED 2
#define OWN_HELD  3
typedef struct { char* name; Ty ty; int is_mut; long long pmask; int own_state; } VarInfo;
static VarInfo VARS[256];
static int NVARS;

static void vars_add(char* name, Ty ty, int is_mut) {
    if (NVARS >= 256) die("ncc: too many locals in one function");
    VARS[NVARS].name = name;
    VARS[NVARS].ty = ty;
    VARS[NVARS].pmask = -1;
    VARS[NVARS].own_state = OWN_NONE;
    VARS[NVARS].is_mut = is_mut;
    NVARS++;
}
static VarInfo* vars_find(const char* name) {
    for (int i = NVARS - 1; i >= 0; i--)        /* innermost shadows */
        if (!strcmp(VARS[i].name, name)) return &VARS[i];
    return NULL;
}

/* pageflags (v0.13, N++ P4 rung 2): a W^X-checked page-permission bitset.
 * Values exist only as compositions of the predeclared PROT_* constants
 * with `|`, so the compiler always knows every value's exact bit set —
 * which makes the W^X rule (never WRITABLE and EXEC together) a TOTAL
 * compile-time check, not a lint. The bit values are the kernel's own
 * mmap/mprotect PROT_* numbers (kernel.h — identical to POSIX/Linux, so
 * one N source works against both the NyxOS kernel and the host shim).
 * Casting OUT to an integer (for a syscall argument) is allowed; casting
 * IN is not — an arbitrary integer would have unknowable bits. */
static const struct { const char* name; long long bits; } PF_CONSTS[] = {
    {"PROT_NONE", 0}, {"PROT_READ", 1}, {"PROT_WRITE", 2}, {"PROT_EXEC", 4},
};
static int pf_const(const char* n, long long* bits) {
    for (size_t i = 0; i < sizeof(PF_CONSTS) / sizeof(PF_CONSTS[0]); i++)
        if (!strcmp(n, PF_CONSTS[i].name)) { if (bits) *bits = PF_CONSTS[i].bits; return 1; }
    return 0;
}
/* The statically-known bit set of a pageflags expression; -1 = unknown
 * (a parameter, whose flags were fixed — and W^X-proven — at its call
 * sites). Only constants, |-compositions, and bindings can appear, so
 * this walk is exhaustive. */
static long long pf_mask(Expr* e) {
    long long b;
    if (e->k == E_PATH) {
        if (pf_const(e->name, &b)) return b;
        VarInfo* v = vars_find(e->name);
        return v ? v->pmask : -1;
    }
    if (e->k == E_BIN && !strcmp(e->op, "|")) {
        long long l = pf_mask(e->l), r = pf_mask(e->r);
        return (l < 0 || r < 0) ? -1 : (l | r);
    }
    return -1;
}

/* Is t a value of an `own struct` type? (v0.17) */
static int ty_is_own(Ty t) {
    if (!t.name || t.ptrs != 0) return 0;
    StructDef* sd = struct_lookup(t.name);
    return sd && sd->is_own;
}

/* v0.17 move point: expression e is being consumed as a VALUE (bound,
 * passed, returned, assigned). If it is a bare path naming an own
 * binding, ownership transfers here: the binding becomes OWN_MOVED and
 * its consumption obligation (if any) is discharged. Field reads are
 * NOT moves — `f.fd` peeks, `f` transfers.
 * v0.18 relaxes v0.17's outermost-block-only rule to BRANCH-AWARE
 * tracking: a move is fine anywhere the statement runs at most once on
 * any path — the outermost block, and `if`/`else` arms (S_IF reconciles
 * the two exit states below). Moves stay refused where a statement may
 * run zero or many times: loop bodies AND `while` conditions (the
 * condition re-evaluates every iteration — consuming there would use
 * the value again on the second test), and inside match arms (reserved;
 * consume conditionally with if/else in v0.18). */
static Ty infer_type(Expr* e);

/* v0.25: the PLACE a refused field move names — a binding, or a field
 * chain rooted in one, spelled as written (`o.p`, `__self.env`); anything
 * else (a call's result, a computed index) stays "an own value". `*root`
 * receives the binding the chain hangs from: the value to consume as a
 * whole. v0.26: a chain may hang from a POINTEE instead — `p[0]`, a name
 * indexed by a literal or a name, the cell an own value sits in behind a
 * cast-born pointer — spelled as written too; `*pointee` says so, and the
 * fix is then a take (`e := p[0]`), not a consume. */
static const char* expr_place(Expr* e, const char** root, int* pointee) {
    if (e->k == E_PATH) { *root = e->name; return e->name; }
    if (e->k == E_INDEX && e->l->k == E_PATH && (e->r->k == E_INT || e->r->k == E_PATH)) {
        char* s = xmalloc(strlen(e->l->name) + (e->r->k == E_PATH ? strlen(e->r->name) : 24) + 3);
        if (e->r->k == E_INT) sprintf(s, "%s[%ld]", e->l->name, (long)e->r->ival);
        else sprintf(s, "%s[%s]", e->l->name, e->r->name);
        *root = s;
        *pointee = 1;
        return s;
    }
    if (e->k != E_FIELD) return NULL;
    const char* b = expr_place(e->base, root, pointee);
    if (!b) return NULL;
    char* s = xmalloc(strlen(b) + strlen(e->field) + 2);
    sprintf(s, "%s.%s", b, e->field);
    return s;
}

static void own_move_expr(Expr* e, int line) {
    if (!e) return;
    if (e->k == E_FIELD) {                /* v0.25: a field never moves out alone —
                                           * the container is consumed as a whole */
        if (ty_is_own(infer_type(e))) {
            const char* root = "an own value";
            int pointee = 0;
            const char* base = expr_place(e->base, &root, &pointee);
            if (!base) base = root;
            if (pointee)                  /* v0.26: the container is a cell behind a
                                           * pointer — taken whole, then consumed */
                die("%s:%d: cannot move field '%s' out of the pointee '%s' — take it as a whole first, e := %s (v0.26)",
                    FILENAME, line, e->field, base, root);
            die("%s:%d: cannot move field '%s' out of own value '%s' — consume '%s' as a whole (v0.25)",
                FILENAME, line, e->field, base, root);
        }
        return;
    }
    if (e->k != E_PATH) return;
    VarInfo* v = vars_find(e->name);
    if (!v || v->own_state == OWN_NONE) return;
    if (v->own_state == OWN_MOVED)
        die("%s:%d: use of '%s' after move", FILENAME, line, e->name);
    if (LOOP_DEPTH > 0)
        die("%s:%d: own value '%s' cannot move inside a loop — the move could run zero or many times (v0.18)",
            FILENAME, line, e->name);
    if (MATCH_DEPTH > 0)
        die("%s:%d: own value '%s' cannot move inside a match arm — consume it with if/else instead (v0.18)",
            FILENAME, line, e->name);
    v->own_state = OWN_MOVED;
}

/* Any own binding at index >= from still OWN_LIVE here leaks — report
 * the first one. Return sites scan from 0 (the whole frame); block ends
 * scan their own scope, so a value born inside a block must be consumed
 * before that block closes. */
static void own_leak_scan(int from, int line, const char* where) {
    for (int i = from; i < NVARS; i++)
        if (VARS[i].own_state == OWN_LIVE)
            die("%s:%d: unconsumed own value '%s' at %s — return it, pass it on, or give the type a #[drop] destructor",
                FILENAME, line, VARS[i].name, where);
}

/* Does this expression mention an own binding anywhere? Used to keep own
 * values out of `defer` (v0.17): a deferred expression runs at exits,
 * after moves the flat tracker cannot see across. */
static int expr_has_own(Expr* e) {
    if (!e) return 0;
    switch (e->k) {
        case E_PATH: {
            VarInfo* v = vars_find(e->name);
            return v && v->own_state != OWN_NONE;
        }
        case E_FIELD: return expr_has_own(e->base);
        case E_CALL: {
            if (expr_has_own(e->callee)) return 1;
            for (int i = 0; i < e->nargs; i++)
                if (expr_has_own(e->args[i])) return 1;
            return 0;
        }
        case E_UN:    return expr_has_own(e->r);
        case E_BIN:   return expr_has_own(e->l) || expr_has_own(e->r);
        case E_CAST:  return expr_has_own(e->l);
        case E_INDEX: return expr_has_own(e->l) || expr_has_own(e->r);
        default:      return 0;
    }
}

static Ty ty_named(const char* n) { Ty t; t.name = (char*)n; t.ptrs = 0; t.is_user = 0; return t; }
static Ty ty_ptr_u8(void)         { Ty t; t.name = (char*)"u8"; t.ptrs = 1; t.is_user = 0; return t; }
static int ty_is(Ty t, const char* n) { return t.name && t.ptrs == 0 && !strcmp(t.name, n); }

/* Look up a named function's signature (extern syscalls and user fns are all
 * parsed before codegen runs, so forward references resolve). Returns 1 and
 * fills the out-params on success, 0 for an unknown name. */
/* Capability set of the function whose body is being generated (bit 0 =
 * syscall). Methods hold no capabilities in v0.14 — they call gated
 * wrappers like everyone else. */
static int CUR_FN_CAPS;

/* The capability an extern syscall demands of its direct callers, or 0. */
static int xfn_caps(const char* name) {
    for (int i = 0; i < NXFN; i++)
        if (!strcmp(XFNS[i].name, name)) return XFNS[i].caps;
    return 0;
}

static int fn_lookup(const char* name, Param** ps, int* np, Ty* ret) {
    for (int i = 0; i < NXFN; i++)
        if (!strcmp(XFNS[i].name, name)) {
            *ps = XFNS[i].ps; *np = XFNS[i].np; *ret = XFNS[i].ret;
            return 1;
        }
    for (int i = 0; i < NFN; i++)
        if (!strcmp(FNS[i].name, name)) {
            *ps = FNS[i].ps; *np = FNS[i].np; *ret = FNS[i].ret;
            return 1;
        }
    return 0;
}

static Ty ret_type_of(const char* name) {
    Param* ps; int np; Ty ret;
    if (fn_lookup(name, &ps, &np, &ret)) return ret;
    return ty_named("i64");    /* soft fallback; check_expr rejects unknowns */
}

/* v0.24 function values. A function declared with `fn` (never an extern
 * syscall) may be named without a call: its type is the fn type of its
 * signature. A struct field of fn type may be called through. */
static int ufn_lookup(const char* name, Param** ps, int* np, Ty* ret) {
    for (int i = 0; i < NFN; i++)
        if (!strcmp(FNS[i].name, name)) {
            *ps = FNS[i].ps; *np = FNS[i].np; *ret = FNS[i].ret;
            return 1;
        }
    return 0;
}

static Ty fn_value_ty(Param* ps, int np, Ty ret) {
    Ty tys[16];
    for (int p = 0; p < np && p < 16; p++) tys[p] = ps[p].ty;
    return fnty_ty(fnty_intern(tys, np, ret));
}

/* The fn type of struct field `field` on a value of type recv, or -1. */
static int field_fnty(Ty recv, const char* field) {
    if (!recv.name || recv.ptrs) return -1;
    StructDef* sd = struct_lookup(recv.name);
    if (!sd) return -1;
    for (int i = 0; i < sd->nf; i++)
        if (!strcmp(sd->fs[i].name, field)) return fnty_of(sd->fs[i].ty);
    return -1;
}

/* Infer the N type of an expression. Integer literals default to i64 (the
 * language rule); comparisons and logic yield bool; `as` is authoritative. */
static Ty infer_type(Expr* e) {
    switch (e->k) {
        case E_INT:    return ty_named("i64");
        case E_BOOL:   return ty_named("bool");
        case E_STR:
        case E_INTERP: return ty_named("str");
        case E_PATH: {
            if (pf_const(e->name, NULL)) return ty_named("pageflags");
            VarInfo* v = vars_find(e->name);
            if (v) return v->ty;
            {   /* v0.24: a function named without a call is a value */
                Param* ps; int np; Ty ret;
                if (ufn_lookup(e->name, &ps, &np, &ret)) return fn_value_ty(ps, np, ret);
            }
            return ty_named("i64");
        }
        case E_CALL:
            if (e->callee->k == E_PATH && !strcmp(e->callee->name, "arg_count"))
                return ty_named("i64");
            if (e->callee->k == E_PATH && !strcmp(e->callee->name, "arg"))
                return ty_named("str");
            if (e->callee->k == E_PATH) {
                VarInfo* v = vars_find(e->callee->name);   /* v0.24: through a fn value */
                if (v) {
                    int fi = fnty_of(v->ty);
                    if (fi >= 0) return FNTYS[fi].ret;
                }
                return ret_type_of(e->callee->name);
            }
            if (e->callee->k == E_FIELD) {      /* method: type from the receiver */
                Ty rt = infer_type(e->callee->base);
                if (rt.name && rt.ptrs == 0) {
                    Method* m = method_lookup(rt.name, e->callee->field);
                    if (m) return m->ret;
                    int fi = field_fnty(rt, e->callee->field);   /* v0.24: a fn field */
                    if (fi >= 0) return FNTYS[fi].ret;
                }
            }
            return ty_named("i64");
        case E_FIELD: {
            int vi;
            EnumDef* ed = variant_ref(e, &vi);  /* Shape.Empty: a variant, not a field */
            if (ed) return ty_named(ed->name);
            Ty b = infer_type(e->base);
            if (ty_is(b, "str")) {
                if (!strcmp(e->field, "ptr")) return ty_ptr_u8();
                if (!strcmp(e->field, "len")) return ty_named("u64");
            }
            if (b.name && b.ptrs == 0) {        /* struct field access */
                StructDef* sd = struct_lookup(b.name);
                if (sd)
                    for (int i = 0; i < sd->nf; i++)
                        if (!strcmp(sd->fs[i].name, e->field)) return sd->fs[i].ty;
            }
            return ty_named("i64");
        }
        case E_SLIT: return ty_named(e->name);
        case E_ELIT: return ty_named(e->name);
        case E_INDEX: {                     /* v0.15: p[i] -> element, s[i] -> u8 */
            Ty bt = infer_type(e->l);
            if (bt.ptrs > 0) {
                Ty el = bt;
                el.ptrs--;
                el.is_user = 0;             /* the element is a plain value */
                return el;
            }
            if (ty_is(bt, "str")) return ty_named("u8");
            return ty_named("i64");
        }
        case E_UN:
            if (e->op[0] == '!') return ty_named("bool");
            return infer_type(e->r);
        case E_BIN: {
            const char* o = e->op;
            if (!strcmp(o, "==") || !strcmp(o, "!=") || !strcmp(o, "<") ||
                !strcmp(o, "<=") || !strcmp(o, ">")  || !strcmp(o, ">=") ||
                !strcmp(o, "&&") || !strcmp(o, "||"))
                return ty_named("bool");
            return infer_type(e->l);            /* arithmetic: left operand rules */
        }
        case E_CAST:   return e->cast_to;
    }
    return ty_named("i64");
}

/* --- static checks (v0.3): the first N++ P1 slice ---------------------- */
/* Render a type for diagnostics: "i64", "*u8", "raw"-ness is not tracked. */
static const char* ty_str(Ty t) {
    static char buf[2][48];
    static int flip;
    int fi = fnty_of(t);                  /* v0.24: a fn type reads as its N spelling */
    if (fi >= 0) return FNTYS[fi].pretty;
    char* b = buf[flip ^= 1];
    int n = 0;
    if (t.is_user) { memcpy(b, "#[user] ", 8); n = 8; }
    for (int i = 0; i < t.ptrs && n < 16; i++) b[n++] = '*';
    const char* nm = t.name ? t.name : "void";
    while (*nm && n < 46) b[n++] = *nm++;
    b[n] = '\0';
    return b;
}

static int ty_is_int(Ty t) {
    if (!t.name || t.ptrs) return 0;
    static const char* I[] = { "i8", "u8", "i16", "u16", "i32", "u32",
                               "i64", "u64", "isize", "usize", "addr", "bool" };
    for (size_t i = 0; i < sizeof(I) / sizeof(I[0]); i++)
        if (!strcmp(t.name, I[i])) return 1;
    return 0;
}

/* Argument/parameter compatibility: integers inter-convert (C semantics);
 * `str` only matches `str`; pointers must agree in depth and base, except
 * that *u8 / *void act as byte pointers and match any base. `as` casts are
 * authoritative because they change the inferred type itself. */
static int ty_compat(Ty a, Ty p) {
    /* #[user] is a hard boundary (v0.12): a plain pointer never converts
     * to a #[user] one or back implicitly — the `as` cast is the audited
     * crossing. Checked before everything else so the byte-pointer
     * wildcards below cannot smuggle a pointer across the boundary. */
    if ((a.ptrs || p.ptrs) && a.is_user != p.is_user) return 0;
    if (a.name && p.name && a.ptrs == p.ptrs && !strcmp(a.name, p.name)) return 1;
    if (ty_is_int(a) && ty_is_int(p)) return 1;
    if (a.ptrs && p.ptrs && a.ptrs == p.ptrs) {
        if (!strcmp(a.name, "u8") || !strcmp(p.name, "u8")) return 1;
        if (!strcmp(a.name, "void") || !strcmp(p.name, "void")) return 1;
    }
    return 0;
}

/* Recursive static check of one expression tree, run against the live
 * symbol table right before the enclosing statement is emitted. Rejects
 * undeclared variables, unknown callees, arity errors, and argument type
 * mismatches — each with a file:line diagnostic. */
static void check_expr(Expr* e);

/* v0.24: a call through a function value — a local, parameter or field of
 * fn type — checked against the signature exactly like a direct call. */
static void check_fn_value_call(Expr* e, int fi, const char* what) {
    FnTy* f = &FNTYS[fi];
    if (e->nargs != f->np)
        die("%s:%d: '%s' takes %d argument(s), got %d", FILENAME, e->line, what, f->np, e->nargs);
    for (int i = 0; i < e->nargs; i++) {
        check_expr(e->args[i]);
        Ty at = infer_type(e->args[i]);
        if (!ty_compat(at, f->ps[i]))
            die("%s:%d: argument %d to '%s': expected %s, got %s",
                FILENAME, e->line, i + 1, what, ty_str(f->ps[i]), ty_str(at));
        own_move_expr(e->args[i], e->line);
    }
}

static void check_expr(Expr* e) {
    if (!e) return;
    switch (e->k) {
        case E_PATH: {
            if (pf_const(e->name, NULL)) break;   /* predeclared pageflags const */
            VarInfo* v = vars_find(e->name);
            if (!v) {
                Param* ps; int np; Ty ret;
                if (ufn_lookup(e->name, &ps, &np, &ret)) break;   /* v0.24: a fn value */
                if (fn_lookup(e->name, &ps, &np, &ret))
                    die("%s:%d: syscall '%s' is not a value — only functions declared with fn are (v0.24)",
                        FILENAME, e->line, e->name);
                die("%s:%d: undeclared variable '%s'", FILENAME, e->line, e->name);
            }
            if (v->own_state == OWN_MOVED)        /* v0.17: no life after move */
                die("%s:%d: use of '%s' after move", FILENAME, e->line, e->name);
            break;
        }
        case E_CALL: {
            if (e->callee->k == E_FIELD) {      /* method call: recv.m(args) */
                Expr* recv = e->callee->base;
                check_expr(recv);
                Ty rt = infer_type(recv);
                Method* m = (rt.name && rt.ptrs == 0)
                            ? method_lookup(rt.name, e->callee->field) : NULL;
                if (!m) {
                    int fi = field_fnty(rt, e->callee->field);   /* v0.24: a fn field */
                    if (fi >= 0) { check_fn_value_call(e, fi, e->callee->field); break; }
                    die("%s:%d: %s has no method '%s'",
                        FILENAME, e->line, ty_str(rt), e->callee->field);
                }
                if (e->nargs != m->np)
                    die("%s:%d: method '%s.%s' takes %d argument(s), got %d",
                        FILENAME, e->line, m->type, m->name, m->np, e->nargs);
                for (int i = 0; i < e->nargs; i++) {
                    check_expr(e->args[i]);
                    Ty at = infer_type(e->args[i]);
                    if (!ty_compat(at, m->ps[i].ty))
                        die("%s:%d: argument %d to '%s.%s': expected %s, got %s",
                            FILENAME, e->line, i + 1, m->type, m->name,
                            ty_str(m->ps[i].ty), ty_str(at));
                    own_move_expr(e->args[i], e->line);   /* v0.17 */
                }
                break;
            }
            if (e->callee->k != E_PATH)
                die("%s:%d: only named functions can be called", FILENAME, e->line);
            /* v0.23 builtins: program arguments, lowered to the nyxrt
             * accessors — not syscalls, so no capability is involved. */
            if (!strcmp(e->callee->name, "arg_count")) {
                if (e->nargs != 0)
                    die("%s:%d: 'arg_count' takes 0 arguments, got %d",
                        FILENAME, e->line, e->nargs);
                break;
            }
            if (!strcmp(e->callee->name, "arg")) {
                if (e->nargs != 1)
                    die("%s:%d: 'arg' takes 1 argument, got %d",
                        FILENAME, e->line, e->nargs);
                check_expr(e->args[0]);
                Ty at0 = infer_type(e->args[0]);
                if (!ty_compat(at0, ty_named("i64")))
                    die("%s:%d: 'arg' index must be an integer, got %s",
                        FILENAME, e->line, ty_str(at0));
                break;
            }
            {   /* v0.24: a local or parameter of fn type, called through */
                VarInfo* lv = vars_find(e->callee->name);
                if (lv) {
                    int fi = fnty_of(lv->ty);
                    if (fi < 0)
                        die("%s:%d: '%s' is not a function (it is %s)",
                            FILENAME, e->line, e->callee->name, ty_str(lv->ty));
                    if (lv->own_state == OWN_MOVED)
                        die("%s:%d: use of '%s' after move", FILENAME, e->line, e->callee->name);
                    check_fn_value_call(e, fi, e->callee->name);
                    break;
                }
            }
            Param* ps; int np; Ty ret;
            if (!fn_lookup(e->callee->name, &ps, &np, &ret))
                die("%s:%d: unknown function '%s'", FILENAME, e->line, e->callee->name);
            if ((xfn_caps(e->callee->name) & 1) && !(CUR_FN_CAPS & 1))
                die("%s:%d: '%s' requires the syscall capability — mark the calling function #[caps(syscall)]",
                    FILENAME, e->line, e->callee->name);
            if (e->nargs != np)
                die("%s:%d: '%s' takes %d argument(s), got %d",
                    FILENAME, e->line, e->callee->name, np, e->nargs);
            for (int i = 0; i < e->nargs; i++) {
                check_expr(e->args[i]);
                Ty at = infer_type(e->args[i]);
                if (!ty_compat(at, ps[i].ty))
                    die("%s:%d: argument %d to '%s': expected %s, got %s",
                        FILENAME, e->line, i + 1, e->callee->name,
                        ty_str(ps[i].ty), ty_str(at));
                own_move_expr(e->args[i], e->line);       /* v0.17 */
            }
            break;
        }
        case E_FIELD: {
            {   /* payload-less variant reference: Shape.Empty */
                int vi;
                EnumDef* ed = variant_ref(e, &vi);
                if (ed) {
                    if (vi < 0)
                        die("%s:%d: enum '%s' has no variant '%s'",
                            FILENAME, e->line, ed->name, e->field);
                    if (ed->vs[vi].nf != 0)
                        die("%s:%d: variant '%s.%s' carries a payload — construct it "
                            "with %s.%s{ ... }", FILENAME, e->line, ed->name, e->field,
                            ed->name, e->field);
                    break;
                }
            }
            check_expr(e->base);
            Ty b = infer_type(e->base);
            if (ty_is(b, "str")) {
                if (strcmp(e->field, "ptr") != 0 && strcmp(e->field, "len") != 0)
                    die("%s:%d: str has no field '%s' (only .ptr and .len)",
                        FILENAME, e->line, e->field);
            } else if (b.name && b.ptrs == 0 && struct_lookup(b.name)) {
                StructDef* sd = struct_lookup(b.name);
                int ok = 0;
                for (int i = 0; i < sd->nf; i++)
                    if (!strcmp(sd->fs[i].name, e->field)) ok = 1;
                if (!ok)
                    die("%s:%d: struct '%s' has no field '%s'",
                        FILENAME, e->line, b.name, e->field);
            } else {
                die("%s:%d: %s has no fields", FILENAME, e->line, ty_str(b));
            }
            break;
        }
        case E_SLIT: {
            StructDef* sd = struct_lookup(e->name);
            if (!sd)
                die("%s:%d: unknown struct '%s'", FILENAME, e->line, e->name);
            /* every declared field exactly once; every value type-compatible */
            for (int i = 0; i < sd->nf; i++) {
                int seen = 0;
                for (int j = 0; j < e->nargs; j++)
                    if (!strcmp(e->snames[j], sd->fs[i].name)) seen++;
                if (seen != 1)
                    die("%s:%d: literal for '%s' must initialize field '%s' exactly once",
                        FILENAME, e->line, e->name, sd->fs[i].name);
            }
            if (e->nargs != sd->nf)
                die("%s:%d: literal for '%s' names a field it does not have",
                    FILENAME, e->line, e->name);
            for (int j = 0; j < e->nargs; j++) {
                check_expr(e->args[j]);
                Ty ft = ty_named("i64");
                for (int i = 0; i < sd->nf; i++)
                    if (!strcmp(sd->fs[i].name, e->snames[j])) ft = sd->fs[i].ty;
                Ty at = infer_type(e->args[j]);
                if (!ty_compat(at, ft))
                    die("%s:%d: field '%s' of '%s': expected %s, got %s",
                        FILENAME, e->line, e->snames[j], e->name,
                        ty_str(ft), ty_str(at));
                if (ty_is_own(ft))                /* v0.25: construction moves the field in */
                    own_move_expr(e->args[j], e->line);
            }
            break;
        }
        case E_ELIT: {
            EnumDef* ed = enum_lookup(e->name);
            if (!ed)
                die("%s:%d: unknown enum '%s'", FILENAME, e->line, e->name);
            int vi = variant_index(ed, e->field);
            if (vi < 0)
                die("%s:%d: enum '%s' has no variant '%s'",
                    FILENAME, e->line, e->name, e->field);
            VariantDef* vd = &ed->vs[vi];
            for (int i = 0; i < vd->nf; i++) {
                int seen = 0;
                for (int j = 0; j < e->nargs; j++)
                    if (!strcmp(e->snames[j], vd->fs[i].name)) seen++;
                if (seen != 1)
                    die("%s:%d: literal for '%s.%s' must initialize field '%s' exactly once",
                        FILENAME, e->line, e->name, e->field, vd->fs[i].name);
            }
            if (e->nargs != vd->nf)
                die("%s:%d: literal for '%s.%s' names a field it does not have",
                    FILENAME, e->line, e->name, e->field);
            for (int j = 0; j < e->nargs; j++) {
                check_expr(e->args[j]);
                Ty ft = ty_named("i64");
                for (int i = 0; i < vd->nf; i++)
                    if (!strcmp(vd->fs[i].name, e->snames[j])) ft = vd->fs[i].ty;
                Ty at = infer_type(e->args[j]);
                if (!ty_compat(at, ft))
                    die("%s:%d: field '%s' of '%s.%s': expected %s, got %s",
                        FILENAME, e->line, e->snames[j], e->name, e->field,
                        ty_str(ft), ty_str(at));
            }
            break;
        }
        case E_UN: {
            check_expr(e->r);
            Ty rt = infer_type(e->r);
            if (!ty_is_int(rt))
                die("%s:%d: unary '%s' expects an integer operand (got %s)",
                    FILENAME, e->line, e->op, ty_str(rt));
            break;
        }
        case E_BIN: {
            check_expr(e->l);
            check_expr(e->r);
            Ty lt = infer_type(e->l);
            Ty rt = infer_type(e->r);
            const char* o = e->op;
            int is_cmp = !strcmp(o, "==") || !strcmp(o, "!=") || !strcmp(o, "<") ||
                         !strcmp(o, "<=") || !strcmp(o, ">")  || !strcmp(o, ">=");
            if (ty_is(lt, "pageflags") || ty_is(rt, "pageflags")) {
                /* v0.13: pageflags compose ONLY with `|` between pageflags
                 * values, and every composition's bit set must be statically
                 * known — that is what makes W^X a total proof. */
                if (!(ty_is(lt, "pageflags") && ty_is(rt, "pageflags")) || strcmp(o, "|") != 0)
                    die("%s:%d: pageflags compose only with '|' between pageflags values (got %s %s %s)",
                        FILENAME, e->line, ty_str(lt), o, ty_str(rt));
                long long m = pf_mask(e);
                if (m < 0)
                    die("%s:%d: compose pageflags from the PROT_* constants — a parameter's flags are opaque here (fixed at its call sites)",
                        FILENAME, e->line);
                if ((m & 2) && (m & 4))
                    die("%s:%d: W^X violation: a mapping cannot be both writable and executable",
                        FILENAME, e->line);
                break;
            }
            if (ty_is(lt, "str") || ty_is(rt, "str"))
                die("%s:%d: operator '%s' cannot be applied to str values "
                    "(build strings with interpolation)", FILENAME, e->line, o);
            if (lt.ptrs || rt.ptrs) {
                /* Pointers compare; they do not do arithmetic. Cast to `addr`
                 * to compute on an address explicitly. */
                if (!(is_cmp && lt.ptrs && rt.ptrs && ty_compat(lt, rt)))
                    die("%s:%d: operator '%s': pointers only support comparison "
                        "against a compatible pointer (cast to addr for arithmetic)",
                        FILENAME, e->line, o);
            } else if (!(ty_is_int(lt) && ty_is_int(rt))) {
                die("%s:%d: operator '%s' expects integer operands (got %s and %s)",
                    FILENAME, e->line, o, ty_str(lt), ty_str(rt));
            }
            break;
        }
        case E_CAST: {
            check_expr(e->l);
            Ty st = infer_type(e->l);
            if (ty_is(e->cast_to, "pageflags"))
                die("%s:%d: cannot cast into pageflags — build it from the PROT_* constants (the W^X proof needs static flag sets)",
                    FILENAME, e->line);
            if (ty_is(st, "pageflags") && !ty_is_int(e->cast_to))
                die("%s:%d: pageflags may only be cast to an integer type (for a syscall argument), got %s",
                    FILENAME, e->line, ty_str(e->cast_to));
            break;
        }
        case E_INDEX: {
            check_expr(e->l);
            check_expr(e->r);
            Ty bt = infer_type(e->l);
            if (bt.ptrs == 0 && !ty_is(bt, "str"))
                die("%s:%d: cannot index a %s value (only pointers and str)",
                    FILENAME, e->line, ty_str(bt));
            Ty it = infer_type(e->r);
            if (!ty_is_int(it))
                die("%s:%d: index must be an integer (got %s)",
                    FILENAME, e->line, ty_str(it));
            break;
        }
        case E_INTERP:
            for (int i = 0; i < e->nfrags; i++)
                if (e->frags[i].e) {
                    check_expr(e->frags[i].e);
                    Ty ft = infer_type(e->frags[i].e);
                    if (!ty_is(ft, "str") && !ty_is_int(ft))
                        die("%s:%d: cannot interpolate a %s value (only str and integers)",
                            FILENAME, e->line, ty_str(ft));
                    if ((e->frags[i].fmt != FMT_DEC || e->frags[i].width > 0) && !ty_is_int(ft))
                        die("%s:%d: format specs apply to integers — a str interpolates as text",
                            FILENAME, e->line);
                }
            break;
        default: break;
    }
}
/* ---------------------------------------------------------------------- */

static void indentf(int ind) { for (int i = 0; i < ind; i++) fputs("    ", OUT); }

/* A C string literal from the decoded bytes of an N literal: the named
 * escapes for \n \t \r \" \\, three-digit octal for every other control
 * or non-ASCII byte (NUL included). Never \xNN: C reads a hex escape as
 * far as the hex digits go, so "\xc3\xa9" + "1" would fold into one byte;
 * an octal escape stops after three digits whatever follows. */
static void emit_cstr(const char* s, int n) {
    fputc('"', OUT);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\n': fputs("\\n", OUT); break;
            case '\t': fputs("\\t", OUT); break;
            case '\r': fputs("\\r", OUT); break;
            case '"':  fputs("\\\"", OUT); break;
            case '\\': fputs("\\\\", OUT); break;
            default:
                if (c < 32 || c > 126) fprintf(OUT, "\\%03o", c);
                else fputc(c, OUT);
        }
    }
    fputc('"', OUT);
}

static void gen_expr(Expr* e) {
    switch (e->k) {
        /* %ld, not %lld: both targets are LP64 (long == 64-bit) and the
         * NyxOS libc formatter implements %l[dux] but not %ll. */
        case E_INT:  fprintf(OUT, "%ld", (long)e->ival); break;
        case E_BOOL: fprintf(OUT, "%d", (int)e->ival); break;
        case E_STR:
            fputs("((nyx_str){", OUT);
            emit_cstr(e->s, e->slen);
            fprintf(OUT, ", %d})", e->slen);
            break;
        case E_INTERP: fprintf(OUT, "__s%d", e->iid); break;
        case E_PATH: {
            long long pb;
            if (pf_const(e->name, &pb)) {         /* pageflags constant: its bits */
                fprintf(OUT, "%ld", (long)pb);
                break;
            }
            fputs(e->name, OUT);
            break;
        }
        case E_INDEX:
            if (ty_is(infer_type(e->l), "str")) { /* byte of the backing text */
                fputs("((nyx_u8)", OUT);
                gen_expr(e->l);
                fputs(".ptr[", OUT);
                gen_expr(e->r);
                fputs("])", OUT);
            } else {                              /* pointer element read */
                gen_expr(e->l);
                fputs("[", OUT);
                gen_expr(e->r);
                fputs("]", OUT);
            }
            break;
        case E_FIELD: {
            int vi;
            EnumDef* ed = variant_ref(e, &vi);
            if (ed) {                   /* Shape.Empty -> tag-only value */
                fprintf(OUT, "((%s){ .tag = %d })", ed->name, vi);
                break;
            }
            if (!strcmp(e->field, "ptr") && ty_is(infer_type(e->base), "str")) {
                /* The language says str.ptr : *u8; make the C agree (the
                 * backing storage is const char*, so cast at the read). */
                fputs("((nyx_u8*)", OUT);
                gen_expr(e->base);
                fputs(".ptr)", OUT);
                break;
            }
            gen_expr(e->base);
            fprintf(OUT, ".%s", e->field);
            break;
        }
        case E_ELIT: {                  /* Shape.Circle{ r: 5 } */
            EnumDef* ed = enum_lookup(e->name);
            int vi = ed ? variant_index(ed, e->field) : 0;
            fprintf(OUT, "((%s){ .tag = %d", e->name, vi);
            if (e->nargs) {
                fprintf(OUT, ", .u = { .%s = { ", e->field);
                for (int i = 0; i < e->nargs; i++) {
                    if (i) fputs(", ", OUT);
                    fprintf(OUT, ".%s = ", e->snames[i]);
                    gen_expr(e->args[i]);
                }
                fputs(" } }", OUT);
            }
            fputs(" })", OUT);
            break;
        }
        case E_CALL:
            if (e->callee->k == E_FIELD &&      /* v0.24: recv.f(a) through a fn field */
                field_fnty(infer_type(e->callee->base), e->callee->field) >= 0) {
                gen_expr(e->callee);
                fputc('(', OUT);
                for (int i = 0; i < e->nargs; i++) {
                    if (i) fputs(", ", OUT);
                    gen_expr(e->args[i]);
                }
                fputc(')', OUT);
                break;
            }
            if (e->callee->k == E_FIELD) {      /* recv.m(a) -> Type_m(recv, a) */
                Ty rt = infer_type(e->callee->base);
                fprintf(OUT, "%s_%s(", rt.name, e->callee->field);
                gen_expr(e->callee->base);
                for (int i = 0; i < e->nargs; i++) {
                    fputs(", ", OUT);
                    gen_expr(e->args[i]);
                }
                fputc(')', OUT);
                break;
            }
            if (e->callee->k == E_PATH && !strcmp(e->callee->name, "arg_count")) {
                fputs("nyx_arg_count()", OUT);
                break;
            }
            if (e->callee->k == E_PATH && !strcmp(e->callee->name, "arg")) {
                fputs("nyx_arg(", OUT);
                gen_expr(e->args[0]);
                fputc(')', OUT);
                break;
            }
            gen_expr(e->callee);
            fputc('(', OUT);
            for (int i = 0; i < e->nargs; i++) {
                if (i) fputs(", ", OUT);
                gen_expr(e->args[i]);
            }
            fputc(')', OUT);
            break;
        case E_UN:
            fprintf(OUT, "%s(", e->op);
            gen_expr(e->r);
            fputc(')', OUT);
            break;
        case E_BIN:
            fputc('(', OUT);
            gen_expr(e->l);
            fprintf(OUT, " %s ", e->op);
            gen_expr(e->r);
            fputc(')', OUT);
            break;
        case E_CAST:
            fputc('(', OUT);
            emit_type(e->cast_to);
            fputs(")(", OUT);
            gen_expr(e->l);
            fputc(')', OUT);
            break;
        case E_SLIT:                    /* C99 compound literal, designated */
            fprintf(OUT, "((%s){", e->name);
            for (int i = 0; i < e->nargs; i++) {
                if (i) fputs(", ", OUT);
                fprintf(OUT, ".%s = ", e->snames[i]);
                gen_expr(e->args[i]);
            }
            fputs("})", OUT);
            break;
    }
}

/* Lower every string interpolation in the expr tree to prelude statements:
 * hoisted buffer + nyx_str built with __nyx_fmt_*. The expression slot then
 * references __sN. Interpolated exprs are formatted as i64 in N v0.1. */
static void gen_preludes(Expr* e, int ind) {
    if (!e) return;
    switch (e->k) {
        case E_INTERP: {
            for (int i = 0; i < e->nfrags; i++)
                if (e->frags[i].e) gen_preludes(e->frags[i].e, ind);
            e->iid = IID++;
            indentf(ind); fprintf(OUT, "char __b%d[256];\n", e->iid);
            indentf(ind); fprintf(OUT, "nyx_str __s%d = __nyx_fmt_begin(__b%d, 256);\n", e->iid, e->iid);
            for (int i = 0; i < e->nfrags; i++) {
                Frag* f = &e->frags[i];
                if (f->e) {
                    indentf(ind);
                    if (ty_is(infer_type(f->e), "str")) {   /* str: insert as text */
                        fprintf(OUT, "__nyx_fmt_str(&__s%d, __b%d, 256, ", e->iid, e->iid);
                        gen_expr(f->e);
                        fputs(");\n", OUT);
                    } else if (f->width > 0) {              /* v0.21 width/zero-pad,
                                                             * decimal or hex */
                        fprintf(OUT, "__nyx_fmt_num(&__s%d, __b%d, 256, (nyx_i64)(", e->iid, e->iid);
                        gen_expr(f->e);
                        fprintf(OUT, "), %d, %d, %d, %d);\n",
                                f->fmt != FMT_DEC ? 1 : 0,
                                f->fmt == FMT_HEXU ? 1 : 0,
                                f->width, f->zero);
                    } else if (f->fmt != FMT_DEC) {         /* v0.20 ":x"/":X" — hex
                                                             * shows the BIT PATTERN:
                                                             * the value crosses as u64 */
                        fprintf(OUT, "__nyx_fmt_hex(&__s%d, __b%d, 256, (nyx_u64)(", e->iid, e->iid);
                        gen_expr(f->e);
                        fprintf(OUT, "), %d);\n", f->fmt == FMT_HEXU ? 1 : 0);
                    } else {                                /* everything else: i64 */
                        fprintf(OUT, "__nyx_fmt_i64(&__s%d, __b%d, 256, (nyx_i64)(", e->iid, e->iid);
                        gen_expr(f->e);
                        fputs("));\n", OUT);
                    }
                } else if (f->tlen > 0) {
                    indentf(ind);
                    fprintf(OUT, "__nyx_fmt_str(&__s%d, __b%d, 256, (nyx_str){", e->iid, e->iid);
                    emit_cstr(f->text, f->tlen);
                    fprintf(OUT, ", %d});\n", f->tlen);
                }
            }
            break;
        }
        case E_CALL:
            gen_preludes(e->callee, ind);
            for (int i = 0; i < e->nargs; i++) gen_preludes(e->args[i], ind);
            break;
        case E_FIELD: gen_preludes(e->base, ind); break;
        case E_INDEX: gen_preludes(e->l, ind); gen_preludes(e->r, ind); break;
        case E_UN:    gen_preludes(e->r, ind); break;
        case E_BIN:   gen_preludes(e->l, ind); gen_preludes(e->r, ind); break;
        case E_CAST:  gen_preludes(e->l, ind); break;
        default: break;
    }
}

static void gen_block(Block* b, int ind, int fn_tail);
static void gen_try_cleanup(int ind, int line, const int* err_states);

/* Emit every pending defer, most recently registered first. */
static void gen_defers(int ind) {
    for (int i = NDEFERS - 1; i >= 0; i--) {
        gen_preludes(FN_DEFERS[i], ind);
        indentf(ind);
        gen_expr(FN_DEFERS[i]);
        fputs(";\n", OUT);
    }
}

/* Emit a zero initializer for a declaration of type t. Used by the v0.9
 * match-expression lowering: the switch that follows is exhaustive (the
 * checker proved it), but a C compiler cannot see that, so the slot is
 * zeroed to keep the generated C warning-free. Aggregates (str, structs,
 * enums) take {0}; scalars and pointers take 0. */
static void emit_zero(Ty t) {
    if (t.ptrs == 0 && t.name &&
        (ty_is(t, "str") || struct_lookup(t.name) || enum_lookup(t.name)))
        fputs("{0}", OUT);
    else
        fputs("0", OUT);
}

/* v0.10 error propagation — lower `x := e?;`, `x = e?;`, and `e?;`.
 * The operand and the enclosing function's return type must both be
 * result enums (enum_is_result). On Err the function returns at once:
 * the same value when the two types are identical, otherwise the Err
 * payload rewrapped into the return type — with defers and ownership
 * cleanup exactly like any other return. On Ok the
 * statement consumes the payload (bind/assign) or discards it (`e?;`). */
static void gen_try(Stmt* s, int ind) {
    check_expr(s->e);
    Ty ot = infer_type(s->e);
    EnumDef* oe = (ot.name && ot.ptrs == 0) ? enum_lookup(ot.name) : NULL;
    if (!oe || !enum_is_result(oe))
        die("%s:%d: operand of '?' must be a result enum (exactly the variants Ok and Err), got %s",
            FILENAME, s->line, ty_str(ot));
    EnumDef* re = (CUR_RET.name && CUR_RET.ptrs == 0) ? enum_lookup(CUR_RET.name) : NULL;
    if (!re || !enum_is_result(re))
        die("%s:%d: '?' propagates by returning, so the function must return a result enum (it returns %s)",
            FILENAME, s->line, ty_str(CUR_RET));
    VariantDef* ov_ok  = &oe->vs[variant_index(oe, "Ok")];
    VariantDef* ov_err = &oe->vs[variant_index(oe, "Err")];
    VariantDef* rv_err = &re->vs[variant_index(re, "Err")];
    int same = !strcmp(oe->name, re->name);
    if (!same) {
        if (ov_err->nf != rv_err->nf)
            die("%s:%d: cannot propagate: %s.Err and %s.Err disagree on carrying a payload",
                FILENAME, s->line, oe->name, re->name);
        if (ov_err->nf == 1 && !ty_compat(ov_err->fs[0].ty, rv_err->fs[0].ty))
            die("%s:%d: cannot propagate %s.Err (%s) as %s.Err (%s)",
                FILENAME, s->line, oe->name, ty_str(ov_err->fs[0].ty),
                re->name, ty_str(rv_err->fs[0].ty));
    }
    if (s->k != S_EXPR && ov_ok->nf != 1)
        die("%s:%d: %s.Ok carries no payload to bind — use the statement form `expr?;`",
            FILENAME, s->line, oe->name);
    Ty okty; okty.name = NULL; okty.ptrs = 0; okty.is_user = 0;
    if (ov_ok->nf == 1) okty = ov_ok->fs[0].ty;
    /* The lhs is evaluated only on Ok. Checking it below may consume
     * owners, but those owners are still live on the Err exit. */
    int err_states[256];
    for (int i = 0; i < NVARS; i++) err_states[i] = VARS[i].own_state;
    if (s->k == S_ASSIGN) {                  /* same checks as plain S_ASSIGN */
        check_expr(s->lhs);
        Expr* root = s->lhs;
        while (root->k == E_FIELD) root = root->base;
        if (root->k == E_INDEX) {
            /* v0.16: element writes through a POINTER are allowed — the
             * pointer binding itself is not mutated, so `mut` is not
             * required on it. Writing through a str index stays refused:
             * str is an immutable view of its backing text (often a
             * literal in read-only storage). */
            Ty ibt = infer_type(root->l);
            if (ty_is(ibt, "str"))
                die("%s:%d: cannot write through a str index — str is an immutable view of its text",
                    FILENAME, s->line);
        }
        if (root->k == E_PATH) {
            VarInfo* v = vars_find(root->name);
            if (v && !v->is_mut)
                die("%s:%d: cannot assign to immutable '%s' (declare it with 'mut')",
                    FILENAME, s->line, root->name);
        }
        Ty lt = infer_type(s->lhs);
        if (!ty_compat(okty, lt))
            die("%s:%d: cannot assign %s to a %s target",
                FILENAME, s->line, ty_str(okty), ty_str(lt));
        if (strcmp(s->aop, "=") != 0 && !ty_is_int(lt))
            die("%s:%d: '%s' requires an integer target (got %s)",
                FILENAME, s->line, s->aop, ty_str(lt));
    }
    int tid = IID++;
    if (s->k == S_LET) {                     /* zero init: dead store, the Err
                                              * branch below always returns */
        indentf(ind);
        emit_type(okty);
        fprintf(OUT, " __ok%d = ", tid);
        emit_zero(okty);
        fputs(";\n", OUT);
    }
    indentf(ind); fputs("{\n", OUT);
    gen_preludes(s->e, ind + 1);
    indentf(ind + 1);
    fprintf(OUT, "%s __t%d = ", oe->name, tid);
    gen_expr(s->e);
    fputs(";\n", OUT);
    indentf(ind + 1);
    fprintf(OUT, "if (__t%d.tag == %d) {\n", tid, variant_index(oe, "Err"));
    if (same) {
        gen_try_cleanup(ind + 2, s->line, err_states);
        indentf(ind + 2); fprintf(OUT, "return __t%d;\n", tid);
    } else {
        indentf(ind + 2);
        fprintf(OUT, "%s __e%d = {0};\n", re->name, tid);
        indentf(ind + 2);
        fprintf(OUT, "__e%d.tag = %d;\n", tid, variant_index(re, "Err"));
        if (rv_err->nf == 1) {
            indentf(ind + 2);
            fprintf(OUT, "__e%d.u.Err.%s = __t%d.u.Err.%s;\n",
                    tid, rv_err->fs[0].name, tid, ov_err->fs[0].name);
        }
        gen_try_cleanup(ind + 2, s->line, err_states);
        indentf(ind + 2); fprintf(OUT, "return __e%d;\n", tid);
    }
    indentf(ind + 1); fputs("}\n", OUT);
    if (s->k == S_LET) {
        indentf(ind + 1);
        fprintf(OUT, "__ok%d = __t%d.u.Ok.%s;\n", tid, tid, ov_ok->fs[0].name);
    } else if (s->k == S_ASSIGN) {
        indentf(ind + 1);
        gen_expr(s->lhs);
        fprintf(OUT, " %s __t%d.u.Ok.%s;\n", s->aop, tid, ov_ok->fs[0].name);
    }
    indentf(ind); fputs("}\n", OUT);
    if (s->k == S_LET) {
        /* Bind only after Err cleanup: the new name may shadow an outer
         * owner which that cleanup still has to refer to. */
        indentf(ind);
        emit_type(okty);
        fprintf(OUT, " %s = __ok%d;\n", s->name, tid);
        vars_add(s->name, okty, s->is_mut);
    }
}

/* Does this block's last statement return? (v0.18: an if arm that ends
 * with `return` never reaches the code after the if, so the own-state
 * merge takes the OTHER arm's states unchallenged.) */
static int block_returns(Block* b) {
    return b && b->n > 0 && b->st[b->n - 1]->k == S_RET;
}

/* v0.22 missing-return flow analysis: does this block GUARANTEE a value
 * on every path through it? A path is covered by a tail expression, a
 * `return`, an `if` whose BOTH arms guarantee, an exhaustive `match`
 * statement whose every arm guarantees, or a call to a `never` function
 * (the path diverges — exit does not come back). Loops never guarantee:
 * a `while` may run zero times, and `while true` is not special-cased
 * (§9). Before this check, "does every path return?" was delegated to
 * the C compiler on the generated file — and the in-OS TinyCC does not
 * even warn, so a missed path was silent garbage on target. */
static int block_guarantees(Block* b) {
    if (!b) return 0;
    if (b->tail) return 1;
    for (int i = 0; i < b->n; i++) {
        Stmt* s = b->st[i];
        if (s->k == S_RET) return 1;
        if (s->k == S_MATCH && s->mtarget == MT_RET) return 1;  /* return match */
        if (s->k == S_IF && s->els &&
            block_guarantees(s->body) && block_guarantees(s->els)) return 1;
        if (s->k == S_MATCH && s->mtarget == MT_STMT && s->narms > 0) {
            int all = 1;
            for (int a = 0; a < s->narms; a++)
                if (!block_guarantees(s->arms[a].body)) { all = 0; break; }
            if (all) return 1;
        }
        if (s->k == S_EXPR && s->e && s->e->k == E_CALL &&
            s->e->callee->k == E_PATH) {
            Param* ps; int np; Ty rt;
            if (fn_lookup(s->e->callee->name, &ps, &np, &rt) && is_never(rt))
                return 1;
        }
    }
    return 0;
}

/* v0.19: the destructor wired to t's struct via #[drop(fn)], or NULL. */
static const char* own_drop_fn(Ty t) {
    if (!t.name || t.ptrs != 0) return NULL;
    StructDef* sd = struct_lookup(t.name);
    return (sd && sd->is_own && sd->drop_fn) ? sd->drop_fn : NULL;
}

/* v0.19 auto-drop: every binding at index >= from still OWN_LIVE whose
 * type carries a #[drop] gets its destructor call emitted here — in
 * REVERSE birth order (last born, first dropped, mirroring defer's
 * LIFO) — and becomes OWN_MOVED, so the leak scan that follows only
 * reports values with no destructor. Held params never auto-drop: a
 * callee owns its own parameters manually (the sink pattern), which is
 * also why the drop function cannot recurse — its parameter arrives
 * held. */
/* v0.25: can a value of type t end on its own — through its #[drop], or,
 * for an own container without one, through its own fields' drops, every
 * one of which must exist? */
static int own_has_drop(Ty t) {
    if (!t.name || t.ptrs != 0) return 0;
    StructDef* sd = struct_lookup(t.name);
    if (!sd || !sd->is_own) return 0;
    if (sd->drop_fn) return 1;
    int any = 0;
    for (int f = 0; f < sd->nf; f++)
        if (ty_is_own(sd->fs[f].ty)) {
            if (!own_has_drop(sd->fs[f].ty)) return 0;
            any = 1;
        }
    return any;
}

/* v0.25: drop the own fields of the container value spelled `path`, in
 * REVERSE declaration order, each through its type's #[drop] — or, for a
 * field that is itself a container without one, through its fields. */
static void own_field_drops_emit(Ty t, const char* path, int ind) {
    StructDef* sd = struct_lookup(t.name);
    for (int f = sd->nf - 1; f >= 0; f--) {
        if (!ty_is_own(sd->fs[f].ty)) continue;
        char fp[512];
        snprintf(fp, sizeof fp, "%s.%s", path, sd->fs[f].name);
        const char* d = own_drop_fn(sd->fs[f].ty);
        if (d) { indentf(ind); fprintf(OUT, "%s(%s);\n", d, fp); }
        else own_field_drops_emit(sd->fs[f].ty, fp, ind);
    }
}

/* v0.19 auto-drop: every binding at index >= from still OWN_LIVE whose
 * type carries a #[drop] gets its destructor call emitted here — in
 * REVERSE birth order (last born, first dropped, mirroring defer's
 * LIFO) — and becomes OWN_MOVED, so the leak scan that follows only
 * reports values with no destructor. Held params never auto-drop: a
 * callee owns its own parameters manually (the sink pattern), which is
 * also why the drop function cannot recurse — its parameter arrives
 * held. v0.25: a live own container without a #[drop] of its own drops
 * through its fields when every own field can drop. */
static void own_drops_emit(int from, int ind) {
    for (int i = NVARS - 1; i >= from; i--) {
        if (VARS[i].own_state != OWN_LIVE) continue;
        const char* d = own_drop_fn(VARS[i].ty);
        if (d) {
            indentf(ind);
            fprintf(OUT, "%s(%s);\n", d, VARS[i].name);
        } else if (own_has_drop(VARS[i].ty)) {
            own_field_drops_emit(VARS[i].ty, VARS[i].name, ind);
        } else continue;
        VARS[i].own_state = OWN_MOVED;
    }
}

/* v0.25: a HELD own container ending its body here — a sink, the
 * container's own #[drop] function included — drops its own fields, in
 * reverse declaration order, after the body's last statement. The
 * container's own destructor never re-runs (the sink rule, unchanged),
 * and a field whose type cannot drop makes this end a leak: the
 * container should have been moved on instead. Emitted only at function
 * exits (returns and the function's end), never at inner block ends. */
static void own_held_drops_emit(int ind, int line) {
    for (int i = 0; i < NVARS; i++) {
        if (VARS[i].own_state != OWN_HELD) continue;
        StructDef* sd = struct_lookup(VARS[i].ty.name);
        if (!sd) continue;
        for (int f = 0; f < sd->nf; f++)
            if (ty_is_own(sd->fs[f].ty) && !own_has_drop(sd->fs[f].ty))
                die("%s:%d: held own value '%s' ends here with field '%s' unconsumed — '%s' has no #[drop] destructor; move '%s' on instead (v0.25)",
                    FILENAME, line, VARS[i].name, sd->fs[f].name, sd->fs[f].ty.name, VARS[i].name);
        own_field_drops_emit(VARS[i].ty, VARS[i].name, ind);
    }
}

/* The Err arm of ? exits, but the Ok arm keeps the ownership state
 * after evaluating the operand. Emit the same cleanup/leak check as a
 * return without letting Err's auto-drops consume Ok's live bindings. */
static void gen_try_cleanup(int ind, int line, const int* err_states) {
    int states[256];
    for (int i = 0; i < NVARS; i++) {
        states[i] = VARS[i].own_state;
        VARS[i].own_state = err_states[i];
    }
    gen_defers(ind);
    own_drops_emit(0, ind);
    own_held_drops_emit(ind, line);
    own_leak_scan(0, line, "this return");
    for (int i = 0; i < NVARS; i++) VARS[i].own_state = states[i];
}

static int own_held_pending(void) {
    for (int i = 0; i < NVARS; i++) {
        if (VARS[i].own_state != OWN_HELD) continue;
        StructDef* sd = struct_lookup(VARS[i].ty.name);
        if (!sd) continue;
        for (int f = 0; f < sd->nf; f++)
            if (ty_is_own(sd->fs[f].ty)) return 1;
    }
    return 0;
}

/* Is any live binding waiting on an auto-drop? (Decides whether an exit
 * path needs the braced __ret form so the calls have somewhere to go.)
 * A function exit (from == 0) also counts a held container's field drops. */
static int own_drops_pending(int from) {
    for (int i = from; i < NVARS; i++)
        if (VARS[i].own_state == OWN_LIVE && own_has_drop(VARS[i].ty)) return 1;
    return from == 0 && own_held_pending();
}

/* v0.19: #[drop(fn)] wires an ORDINARY function as an own struct's
 * destructor. Checked once all declarations are known: the function
 * must exist, take exactly one parameter of exactly that own type, and
 * return nothing — i.e. it has the same shape a manual close-function
 * already has (and it stays callable by hand: explicit consumption and
 * auto-drop share one implementation). */
static void validate_drops(void) {
    for (int i = 0; i < NSTRUCTS; i++) {
        StructDef* sd = &STRUCTS[i];
        if (!sd->drop_fn) continue;
        Param* ps; int np; Ty ret;
        if (!fn_lookup(sd->drop_fn, &ps, &np, &ret))
            die("%s: #[drop(%s)] on '%s': unknown function '%s'",
                FILENAME, sd->drop_fn, sd->name, sd->drop_fn);
        if (np != 1 || !ps[0].ty.name || ps[0].ty.ptrs != 0 ||
            strcmp(ps[0].ty.name, sd->name) != 0)
            die("%s: drop function '%s' must take exactly one %s parameter",
                FILENAME, sd->drop_fn, sd->name);
        if (ret.name && !is_never(ret))
            die("%s: drop function '%s' must not return a value — the value ends there",
                FILENAME, sd->drop_fn);
    }
}

static void gen_stmt(Stmt* s, int ind) {
    if (s->is_try) { gen_try(s, ind); return; }
    switch (s->k) {
        case S_LET: {
            check_expr(s->e);
            Ty t = infer_type(s->e);
            if (!t.name || is_never(t))
                die("%s:%d: cannot bind '%s' to an expression with no value",
                    FILENAME, s->line, s->name);
            if (fnty_of(t) >= EMITTED_FNTYS)  /* v0.24: no typedef exists for it */
                die("%s:%d: cannot bind '%s': the function type %s is declared nowhere in this "
                    "program — pass the function as an argument, or store it in a field, of that type",
                    FILENAME, s->line, s->name, ty_str(t));
            own_move_expr(s->e, s->line);         /* v0.17: init consumes its source */
            if (ty_is_own(t) && s->is_mut)
                die("%s:%d: own bindings are immutable — ownership transfers by move (v0.17)",
                    FILENAME, s->line);
            gen_preludes(s->e, ind);
            indentf(ind);
            emit_type(t);
            fprintf(OUT, " %s = ", s->name);
            gen_expr(s->e);
            fputs(";\n", OUT);
            vars_add(s->name, t, s->is_mut);
            if (ty_is(t, "pageflags"))            /* track the static flag set */
                VARS[NVARS - 1].pmask = pf_mask(s->e);
            if (ty_is_own(t))                     /* v0.17: birth — must be consumed */
                VARS[NVARS - 1].own_state = OWN_LIVE;
            break;
        }
        case S_ASSIGN: {
            check_expr(s->lhs);
            check_expr(s->e);
            {                                   /* mut enforcement (spec 5.1) —
                                                 * field writes mutate the root binding */
                Expr* root = s->lhs;
                while (root->k == E_FIELD) root = root->base;
                if (root->k == E_INDEX) {
                    /* v0.16: element writes through a POINTER are allowed — the
                     * pointer binding itself is not mutated, so `mut` is not
                     * required on it. Writing through a str index stays refused:
                     * str is an immutable view of its backing text (often a
                     * literal in read-only storage). */
                    Ty ibt = infer_type(root->l);
                    if (ty_is(ibt, "str"))
                        die("%s:%d: cannot write through a str index — str is an immutable view of its text",
                            FILENAME, s->line);
                }
                if (root->k == E_PATH) {
                    VarInfo* v = vars_find(root->name);
                    if (v && !v->is_mut)
                        die("%s:%d: cannot assign to immutable '%s' (declare it with 'mut')",
                            FILENAME, s->line, root->name);
                }
            }
            {                                    /* value/target type check (v0.4) */
                Ty lt = infer_type(s->lhs);
                Ty rt = infer_type(s->e);
                if (!ty_compat(rt, lt))
                    die("%s:%d: cannot assign %s to a %s target",
                        FILENAME, s->line, ty_str(rt), ty_str(lt));
                if (strcmp(s->aop, "=") != 0 && !ty_is_int(lt))
                    die("%s:%d: '%s' requires an integer target (got %s)",
                        FILENAME, s->line, s->aop, ty_str(lt));
            }
            own_move_expr(s->e, s->line);         /* v0.17: assignment consumes */
            if (s->lhs->k == E_PATH) {            /* keep a reassigned pageflags
                                                   * binding's static set current */
                VarInfo* v = vars_find(s->lhs->name);
                if (v && ty_is(v->ty, "pageflags")) v->pmask = pf_mask(s->e);
            }
            gen_preludes(s->e, ind);
            indentf(ind);
            gen_expr(s->lhs);
            fprintf(OUT, " %s ", s->aop);
            gen_expr(s->e);
            fputs(";\n", OUT);
            break;
        }
        case S_RET:
            check_expr(s->e);
            if (s->e) {
                if (!CUR_RET.name || is_never(CUR_RET))
                    die("%s:%d: 'return' with a value in a function with no return type",
                        FILENAME, s->line);
                Ty vt = infer_type(s->e);
                if (!ty_compat(vt, CUR_RET))
                    die("%s:%d: return type mismatch: expected %s, got %s",
                        FILENAME, s->line, ty_str(CUR_RET), ty_str(vt));
            } else if (CUR_RET.name && !is_never(CUR_RET)) {
                die("%s:%d: 'return' without a value in a function returning %s",
                    FILENAME, s->line, ty_str(CUR_RET));
            }
            own_move_expr(s->e, s->line);         /* v0.17: returning consumes */
            {
                int pend = own_drops_pending(0);  /* v0.19: drops need the braced form */
                if ((NDEFERS || pend) && s->e) {
                    /* Semantics: the return value is computed BEFORE the defers
                     * run, so a defer cannot change what gets returned; auto-
                     * drops run after the defers (v0.19: defer keeps its v0.6
                     * timing — adding a #[drop] never reorders existing code). */
                    indentf(ind); fputs("{\n", OUT);
                    gen_preludes(s->e, ind + 1);
                    indentf(ind + 1);
                    emit_type(infer_type(s->e));
                    fputs(" __ret = ", OUT);
                    gen_expr(s->e);
                    fputs(";\n", OUT);
                    gen_defers(ind + 1);
                    own_drops_emit(0, ind + 1);
                    own_held_drops_emit(ind + 1, s->line);   /* v0.25 */
                    own_leak_scan(0, s->line, "this return");
                    indentf(ind + 1); fputs("return __ret;\n", OUT);
                    indentf(ind); fputs("}\n", OUT);
                    break;
                }
                if (NDEFERS || pend) {              /* void return */
                    gen_defers(ind);
                    own_drops_emit(0, ind);
                    own_held_drops_emit(ind, s->line);       /* v0.25 */
                    own_leak_scan(0, s->line, "this return");
                    indentf(ind); fputs("return;\n", OUT);
                    break;
                }
            }
            own_leak_scan(0, s->line, "this return");
            gen_preludes(s->e, ind);
            indentf(ind);
            if (s->e) { fputs("return ", OUT); gen_expr(s->e); fputs(";\n", OUT); }
            else fputs("return;\n", OUT);
            break;
        case S_EXPR:
            check_expr(s->e);
            if (ty_is_own(infer_type(s->e)))      /* v0.17: nobody owns this now */
                die("%s:%d: own result discarded — bind it so someone owns it",
                    FILENAME, s->line);
            gen_preludes(s->e, ind);
            indentf(ind);
            gen_expr(s->e);
            fputs(";\n", OUT);
            break;
        case S_BREAK: indentf(ind); fputs("break;\n", OUT); break;
        case S_CONT:  indentf(ind); fputs("continue;\n", OUT); break;
        case S_DEFER:
            if (BLOCK_DEPTH != 1)
                die("%s:%d: defer is only allowed in the function's outermost block",
                    FILENAME, s->line);
            if (NDEFERS >= 16)
                die("%s:%d: too many defers in one function", FILENAME, s->line);
            check_expr(s->e);           /* names resolve at registration point */
            if (expr_has_own(s->e))     /* v0.17: defers run after moves the
                                         * flat tracker cannot see across */
                die("%s:%d: own values may not appear in defer expressions (v0.17)",
                    FILENAME, s->line);
            FN_DEFERS[NDEFERS++] = s->e;
            break;
        case S_MATCH: {
            check_expr(s->e);
            Ty st_ = infer_type(s->e);
            EnumDef* ed = (st_.name && st_.ptrs == 0) ? enum_lookup(st_.name) : NULL;
            if (!ed)
                die("%s:%d: match subject must be an enum value (got %s)",
                    FILENAME, s->line, ty_str(st_));
            /* exhaustive, no duplicates, binds match the payload */
            for (int v = 0; v < ed->nv; v++) {
                int seen = 0;
                for (int a = 0; a < s->narms; a++)
                    if (!strcmp(s->arms[a].variant, ed->vs[v].name)) seen++;
                if (seen != 1)
                    die("%s:%d: match must cover variant '%s' exactly once",
                        FILENAME, s->line, ed->vs[v].name);
            }
            if (s->narms != ed->nv)
                die("%s:%d: match names a variant '%s' does not have",
                    FILENAME, s->line, ed->name);
            for (int a = 0; a < s->narms; a++) {
                int vi = variant_index(ed, s->arms[a].variant);
                if (s->arms[a].nbinds != ed->vs[vi].nf)
                    die("%s:%d: arm '%s' binds %d name(s), but the payload has %d field(s)",
                        FILENAME, s->arms[a].line, s->arms[a].variant,
                        s->arms[a].nbinds, ed->vs[vi].nf);
            }
            if (s->mtarget != MT_STMT) {
                /* v0.9 expression form: every arm yields a value. Each arm
                 * is typed with its binds in scope; the first arm fixes the
                 * result type and every later arm must agree with it. */
                Ty res; res.name = NULL; res.ptrs = 0; res.is_user = 0;
                for (int a = 0; a < s->narms; a++) {
                    int vi = variant_index(ed, s->arms[a].variant);
                    int vsave = NVARS;
                    for (int b2 = 0; b2 < s->arms[a].nbinds; b2++)
                        vars_add(s->arms[a].binds[b2], ed->vs[vi].fs[b2].ty, 0);
                    check_expr(s->arms[a].val);
                    Ty at = infer_type(s->arms[a].val);
                    NVARS = vsave;
                    if (!at.name || is_never(at))
                        die("%s:%d: match arm '%s' must yield a value",
                            FILENAME, s->arms[a].line, s->arms[a].variant);
                    if (a == 0) res = at;
                    else if (!ty_compat(at, res))
                        die("%s:%d: match arms disagree: arm '%s' yields %s, expected %s",
                            FILENAME, s->arms[a].line, s->arms[a].variant,
                            ty_str(at), ty_str(res));
                }
                if (ty_is_own(res))     /* v0.17: only one arm runs — the flat
                                         * tracker cannot follow that */
                    die("%s:%d: own values cannot flow through a match expression (v0.17)",
                        FILENAME, s->line);
                if (s->mtarget == MT_ASSIGN) {   /* same checks as S_ASSIGN */
                    check_expr(s->lhs);
                    Expr* root = s->lhs;
                    while (root->k == E_FIELD) root = root->base;
                    if (root->k == E_INDEX) {
                        /* v0.16: element writes through a POINTER are allowed — the
                         * pointer binding itself is not mutated, so `mut` is not
                         * required on it. Writing through a str index stays refused:
                         * str is an immutable view of its backing text (often a
                         * literal in read-only storage). */
                        Ty ibt = infer_type(root->l);
                        if (ty_is(ibt, "str"))
                            die("%s:%d: cannot write through a str index — str is an immutable view of its text",
                                FILENAME, s->line);
                    }
                    if (root->k == E_PATH) {
                        VarInfo* v = vars_find(root->name);
                        if (v && !v->is_mut)
                            die("%s:%d: cannot assign to immutable '%s' (declare it with 'mut')",
                                FILENAME, s->line, root->name);
                    }
                    Ty lt = infer_type(s->lhs);
                    if (!ty_compat(res, lt))
                        die("%s:%d: cannot assign %s to a %s target",
                            FILENAME, s->line, ty_str(res), ty_str(lt));
                    if (strcmp(s->aop, "=") != 0 && !ty_is_int(lt))
                        die("%s:%d: '%s' requires an integer target (got %s)",
                            FILENAME, s->line, s->aop, ty_str(lt));
                }
                if (s->mtarget == MT_RET) {      /* same checks as S_RET */
                    if (!CUR_RET.name || is_never(CUR_RET))
                        die("%s:%d: 'return' with a value in a function with no return type",
                            FILENAME, s->line);
                    if (!ty_compat(res, CUR_RET))
                        die("%s:%d: return type mismatch: expected %s, got %s",
                            FILENAME, s->line, ty_str(CUR_RET), ty_str(res));
                }
                /* Lowering: the selected arm assigns its value into a
                 * __mres temp; the target is consumed AFTER the switch, so
                 * an arm bind may freely shadow the target's name. For
                 * `return match` the value is computed before the defers
                 * run, exactly like a plain `return` (spec 5.7). */
                int mid = IID++;
                Ty rty = (s->mtarget == MT_RET) ? CUR_RET : res;
                if (s->mtarget == MT_LET) {
                    indentf(ind);
                    emit_type(res);
                    fprintf(OUT, " %s = ", s->name);
                    emit_zero(res);
                    fputs(";\n", OUT);
                }
                indentf(ind); fputs("{\n", OUT);
                gen_preludes(s->e, ind + 1);
                indentf(ind + 1);
                fprintf(OUT, "%s __m%d = ", ed->name, mid);
                gen_expr(s->e);
                fputs(";\n", OUT);
                indentf(ind + 1);
                emit_type(rty);
                fprintf(OUT, " __mres%d = ", mid);
                emit_zero(rty);                  /* dead store: the switch is exhaustive */
                fputs(";\n", OUT);
                indentf(ind + 1); fprintf(OUT, "switch (__m%d.tag) {\n", mid);
                for (int a = 0; a < s->narms; a++) {
                    int vi = variant_index(ed, s->arms[a].variant);
                    indentf(ind + 1);
                    fprintf(OUT, "case %d: {\n", vi);
                    int vsave = NVARS;
                    for (int b2 = 0; b2 < s->arms[a].nbinds; b2++) {
                        Param* pf = &ed->vs[vi].fs[b2];
                        indentf(ind + 2);
                        emit_type(pf->ty);
                        fprintf(OUT, " %s = __m%d.u.%s.%s;\n",
                                s->arms[a].binds[b2], mid, s->arms[a].variant, pf->name);
                        vars_add(s->arms[a].binds[b2], pf->ty, 0);
                    }
                    gen_preludes(s->arms[a].val, ind + 2);
                    indentf(ind + 2);
                    fprintf(OUT, "__mres%d = ", mid);
                    gen_expr(s->arms[a].val);
                    fputs(";\n", OUT);
                    indentf(ind + 2); fputs("break;\n", OUT);
                    indentf(ind + 1); fputs("}\n", OUT);
                    NVARS = vsave;
                }
                indentf(ind + 1); fputs("}\n", OUT);
                if (s->mtarget == MT_LET) {
                    indentf(ind + 1);
                    fprintf(OUT, "%s = __mres%d;\n", s->name, mid);
                } else if (s->mtarget == MT_ASSIGN) {
                    indentf(ind + 1);
                    gen_expr(s->lhs);
                    fprintf(OUT, " %s __mres%d;\n", s->aop, mid);
                } else {
                    gen_defers(ind + 1);
                    indentf(ind + 1); fprintf(OUT, "return __mres%d;\n", mid);
                }
                indentf(ind); fputs("}\n", OUT);
                if (s->mtarget == MT_LET)
                    vars_add(s->name, res, s->is_mut);
                break;
            }
            /* lowering: subject once into a temp, switch on the tag, binds
             * become typed locals initialized from the payload fields */
            int mid = IID++;
            indentf(ind); fputs("{\n", OUT);
            gen_preludes(s->e, ind + 1);
            indentf(ind + 1);
            fprintf(OUT, "%s __m%d = ", ed->name, mid);
            gen_expr(s->e);
            fputs(";\n", OUT);
            indentf(ind + 1); fprintf(OUT, "switch (__m%d.tag) {\n", mid);
            MATCH_DEPTH++;     /* v0.18: own moves stay out of match arms
                                * (see own_move_expr) — if/else got the
                                * branch merge first */
            for (int a = 0; a < s->narms; a++) {
                int vi = variant_index(ed, s->arms[a].variant);
                indentf(ind + 1);
                fprintf(OUT, "case %d: {\n", vi);
                int vsave = NVARS;
                for (int b2 = 0; b2 < s->arms[a].nbinds; b2++) {
                    Param* pf = &ed->vs[vi].fs[b2];
                    indentf(ind + 2);
                    emit_type(pf->ty);
                    fprintf(OUT, " %s = __m%d.u.%s.%s;\n",
                            s->arms[a].binds[b2], mid, s->arms[a].variant, pf->name);
                    vars_add(s->arms[a].binds[b2], pf->ty, 0);
                }
                indentf(ind + 2);
                gen_block(s->arms[a].body, ind + 2, 0);
                fputs("\n", OUT);
                indentf(ind + 2); fputs("break;\n", OUT);
                indentf(ind + 1); fputs("}\n", OUT);
                NVARS = vsave;
            }
            MATCH_DEPTH--;
            indentf(ind + 1); fputs("}\n", OUT);
            indentf(ind); fputs("}\n", OUT);
            break;
        }
        case S_WHILE:
            LOOP_DEPTH++;      /* v0.18: covers the CONDITION too — it
                                * re-evaluates every iteration, so an own
                                * move there would consume twice */
            check_expr(s->cond);
            gen_preludes(s->cond, ind);
            indentf(ind);
            fputs("while (", OUT);
            gen_expr(s->cond);
            fputs(") ", OUT);
            gen_block(s->body, ind, 0);
            LOOP_DEPTH--;
            fputs("\n", OUT);
            break;
        case S_FOR: {
            /* v0.11 counted loop: both bounds are evaluated ONCE, before
             * the loop; the loop variable is a fresh immutable i64 scoped
             * to the body, counting over the half-open range [start, end). */
            check_expr(s->e);
            check_expr(s->cond);
            Ty at = infer_type(s->e);
            Ty bt = infer_type(s->cond);
            if (!ty_is_int(at) || !ty_is_int(bt))
                die("%s:%d: for range bounds must be integers (got %s .. %s)",
                    FILENAME, s->line, ty_str(at), ty_str(bt));
            int fid = IID++;
            indentf(ind); fputs("{\n", OUT);
            gen_preludes(s->e, ind + 1);
            gen_preludes(s->cond, ind + 1);
            indentf(ind + 1);
            fprintf(OUT, "nyx_i64 __fs%d = ", fid);
            gen_expr(s->e);
            fputs(";\n", OUT);
            indentf(ind + 1);
            fprintf(OUT, "nyx_i64 __fe%d = ", fid);
            gen_expr(s->cond);
            fputs(";\n", OUT);
            indentf(ind + 1);
            fprintf(OUT, "for (nyx_i64 %s = __fs%d; %s < __fe%d; %s++) ",
                    s->name, fid, s->name, fid, s->name);
            {
                int vsave = NVARS;
                vars_add(s->name, ty_named("i64"), 0);
                LOOP_DEPTH++;  /* v0.18: the body may run 0..N times; the
                                * BOUNDS stay outside the guard — they are
                                * hoisted and evaluated exactly once */
                gen_block(s->body, ind + 1, 0);
                LOOP_DEPTH--;
                NVARS = vsave;
            }
            fputs("\n", OUT);
            indentf(ind); fputs("}\n", OUT);
            break;
        }
        case S_IF: {
            check_expr(s->cond);
            gen_preludes(s->cond, ind);
            indentf(ind);
            fputs("if (", OUT);
            gen_expr(s->cond);
            fputs(") ", OUT);
            /* v0.18 branch-aware own tracking: each arm is checked from
             * the same pre-if states, and at the merge point the two
             * exits must AGREE on every own binding — consumed in both
             * arms or in neither. An arm whose last statement is
             * `return` never reaches the merge point, so it is exempt
             * (the return's own leak scan already policed that path). */
            int n0 = NVARS;
            int pre[256], thn[256];
            for (int i = 0; i < n0; i++) pre[i] = VARS[i].own_state;
            gen_block(s->body, ind, 0);
            for (int i = 0; i < n0; i++) { thn[i] = VARS[i].own_state;
                                           VARS[i].own_state = pre[i]; }
            if (s->els) { fputs(" else ", OUT); gen_block(s->els, ind, 0); }
            int t_ret = block_returns(s->body);
            int e_ret = s->els ? block_returns(s->els) : 0;
            for (int i = 0; i < n0; i++) {
                /* VARS now holds the else-exit state (= pre-if when the
                 * if has no else: falling past it changes nothing). */
                if (t_ret)       { if (!e_ret) continue;          /* else exit flows on */
                                   VARS[i].own_state = thn[i]; }  /* neither flows: dead */
                else if (e_ret)    VARS[i].own_state = thn[i];    /* then exit flows on */
                else {
                    if (thn[i] != VARS[i].own_state)
                        die("%s:%d: own value '%s' is consumed in only one branch of this if — both branches must agree (v0.18)",
                            FILENAME, s->line, VARS[i].name);
                }
            }
            fputs("\n", OUT);
            break;
        }
    }
}

static int STASH_ARGS;   /* v0.23: set for main's body — its first act is
                          * handing the SysV frame to the runtime */

static void gen_block(Block* b, int ind, int fn_tail) {
    int vsave = NVARS;                  /* block scope: locals die with the block */
    int tail_returned = 0;              /* v0.25: a returning tail drained the held drops */
    BLOCK_DEPTH++;
    fputs("{\n", OUT);
    if (STASH_ARGS) {
        fputs("    __nyx_args_set(__argc, __argv);\n", OUT);
        STASH_ARGS = 0;
    }
    for (int i = 0; i < b->n; i++) gen_stmt(b->st[i], ind + 1);
    if (b->tail) {
        check_expr(b->tail);
        own_move_expr(b->tail, b->tail->line);    /* v0.17: the tail consumes — reported
                                                   * at the tail's own line */
        int returns = fn_tail && CUR_RET.name && !is_never(CUR_RET);
        if (returns) {
            Ty vt = infer_type(b->tail);         /* tail value = return value */
            if (!ty_compat(vt, CUR_RET))
                die("%s: tail expression type mismatch: expected %s, got %s",
                    FILENAME, ty_str(CUR_RET), ty_str(vt));
        }
        if (returns && (NDEFERS || own_drops_pending(0))) {
            /* tail value, then defers, then auto-drops, then return */
            gen_preludes(b->tail, ind + 1);
            indentf(ind + 1);
            emit_type(infer_type(b->tail));
            fputs(" __ret = ", OUT);
            gen_expr(b->tail);
            fputs(";\n", OUT);
            gen_defers(ind + 1);
            own_drops_emit(0, ind + 1);
            own_held_drops_emit(ind + 1, b->n ? b->st[b->n - 1]->line : b->line);   /* v0.25 */
            tail_returned = 1;
            indentf(ind + 1); fputs("return __ret;\n", OUT);
        } else {
            gen_preludes(b->tail, ind + 1);
            indentf(ind + 1);
            if (returns) fputs("return ", OUT);
            gen_expr(b->tail);
            fputs(";\n", OUT);
            if (fn_tail && !returns && NDEFERS) gen_defers(ind + 1);
        }
    } else if (fn_tail && NDEFERS && (!CUR_RET.name || is_never(CUR_RET))) {
        gen_defers(ind + 1);                     /* void fn falling off the end */
    }
    /* v0.19 auto-drops at the close of this scope, after any defers: the
     * whole frame when the function ends here, this block's own births
     * otherwise. (Exit paths that already returned drained their drops
     * above, so this is a no-op for them.) */
    own_drops_emit(fn_tail ? 0 : vsave, ind + 1);
    if (fn_tail && !tail_returned)                /* v0.25: a held container's fields
                                                   * (a returning tail drained them above) */
        own_held_drops_emit(ind + 1, b->n ? b->st[b->n - 1]->line : b->line);
    indentf(ind);
    fputs("}", OUT);
    own_leak_scan(vsave, b->n ? b->st[b->n - 1]->line : b->line,   /* v0.17: nothing
        * may leak out of any scope — undropped, unconsumed = error */
        BLOCK_DEPTH == 1 ? "the end of the function" : "the end of this block");
    BLOCK_DEPTH--;
    NVARS = vsave;
}

static void gen_extern_fns(void) {
    for (int i = 0; i < NXFN; i++) {
        XFn* x = &XFNS[i];
        fputs("static inline ", OUT);
        if (is_never(x->ret)) fputs("void", OUT);
        else emit_type(x->ret);
        fprintf(OUT, " %s(", x->name);
        if (!x->np) fputs("void", OUT);
        for (int k = 0; k < x->np; k++) {
            if (k) fputs(", ", OUT);
            emit_type(x->ps[k].ty);
            fprintf(OUT, " %s", x->ps[k].name);
        }
        fputs(") {\n", OUT);
        if (is_never(x->ret) || !x->ret.name) {
            fprintf(OUT, "    (void)__nyx_syscall6(%ld", (long)x->num);
            for (int k = 0; k < 6; k++) {
                if (k < x->np) fprintf(OUT, ", (nyx_i64)%s", x->ps[k].name);
                else fputs(", 0", OUT);
            }
            fputs(");\n", OUT);
            if (is_never(x->ret)) fputs("    for (;;) {}\n", OUT);
        } else {
            fputs("    return (", OUT);
            emit_type(x->ret);
            fprintf(OUT, ")__nyx_syscall6(%ld", (long)x->num);
            for (int k = 0; k < 6; k++) {
                if (k < x->np) fprintf(OUT, ", (nyx_i64)%s", x->ps[k].name);
                else fputs(", 0", OUT);
            }
            fputs(");\n", OUT);
        }
        fputs("}\n", OUT);
    }
    if (NXFN) fputs("\n", OUT);
}

static void gen_fn_sig(Fn* f) {
    if (!f->ret.name || is_never(f->ret)) fputs("void", OUT);
    else emit_type(f->ret);
    fprintf(OUT, " %s(", f->name);
    if (!strcmp(f->name, "main") && !f->np) {   /* v0.23: main receives the SysV frame */
        fputs("nyx_i64 __argc, nyx_u8** __argv)", OUT);
        return;
    }
    if (!f->np) fputs("void", OUT);
    for (int i = 0; i < f->np; i++) {
        if (i) fputs(", ", OUT);
        emit_type(f->ps[i].ty);
        fprintf(OUT, " %s", f->ps[i].name);
    }
    fputc(')', OUT);
}

/* Every non-primitive type name in a signature or struct field must be a
 * declared struct — catches typos before they become C errors. */
static void validate_ty(Ty t, const char* where) {
    if (!t.name) return;
    if (t.ptrs > 0 && ty_is_own(ty_named(t.name)))   /* v0.17: no aliasing an owner */
        die("%s: pointers to own type '%s' are not allowed (in %s)",
            FILENAME, t.name, where);
    int fi = fnty_of(t);                  /* v0.24: a fn type — check its signature */
    if (fi >= 0) {
        for (int p = 0; p < FNTYS[fi].np; p++) validate_ty(FNTYS[fi].ps[p], where);
        validate_ty(FNTYS[fi].ret, where);
        return;
    }
    if (strcmp(base_ctype(t.name), t.name) != 0) return;   /* mapped: primitive */
    if (!struct_lookup(t.name) && !enum_lookup(t.name))
        die("%s: unknown type '%s' in %s", FILENAME, t.name, where);
}

/* v0.24: `R (*name)(A, B)` — the C declarator of a fn type around a name.
 * Struct fields spell it out (their layout precedes the typedefs, which
 * may mention the structs); everything else uses the typedef name. */
static void emit_fn_declarator(int fi, const char* name) {
    FnTy* f = &FNTYS[fi];
    if (!f->ret.name || is_never(f->ret)) fputs("void", OUT);
    else emit_type(f->ret);
    fprintf(OUT, " (*%s)(", name);
    if (!f->np) fputs("void", OUT);
    for (int p = 0; p < f->np; p++) {
        if (p) fputs(", ", OUT);
        emit_type(f->ps[p]);
    }
    fputc(')', OUT);
}

/* One struct's C layout: a typedef with the fields in order. */
static void gen_struct_layout(int i) {
    fprintf(OUT, "typedef struct {\n");
    for (int f = 0; f < STRUCTS[i].nf; f++) {
        fputs("    ", OUT);
        int fi = fnty_of(STRUCTS[i].fs[f].ty);
        if (fi >= 0) {                    /* v0.24: a fn field, as a declarator */
            emit_fn_declarator(fi, STRUCTS[i].fs[f].name);
            fputs(";\n", OUT);
            continue;
        }
        emit_type(STRUCTS[i].fs[f].ty);
        fprintf(OUT, " %s;\n", STRUCTS[i].fs[f].name);
    }
    fprintf(OUT, "} %s;\n\n", STRUCTS[i].name);
}

/* One enum's C layout: a tagged union — int tag + a union of payload
 * structs; an empty union is not valid C, so payload-less enums get the
 * tag alone. The tag comment names every variant's number. */
static void gen_enum_layout(int i) {
    EnumDef* ed = &ENUMS[i];
    int has_payload = 0;
    for (int v = 0; v < ed->nv; v++)
        if (ed->vs[v].nf) has_payload = 1;
    fprintf(OUT, "typedef struct {\n    int tag;    /*");
    for (int v = 0; v < ed->nv; v++)
        fprintf(OUT, " %d=%s", v, ed->vs[v].name);
    fputs(" */\n", OUT);
    if (has_payload) {
        fputs("    union {\n", OUT);
        for (int v = 0; v < ed->nv; v++) {
            if (!ed->vs[v].nf) continue;
            fputs("        struct { ", OUT);
            for (int f = 0; f < ed->vs[v].nf; f++) {
                emit_type(ed->vs[v].fs[f].ty);
                fprintf(OUT, " %s; ", ed->vs[v].fs[f].name);
            }
            fprintf(OUT, "} %s;\n", ed->vs[v].name);
        }
        fputs("    } u;\n", OUT);
    }
    fprintf(OUT, "} %s;\n\n", ed->name);
}

static void gen_program(const char* srcname) {
    /* v0.17 own containment: an own value lives in exactly one binding at
     * a time, so it cannot hide inside other aggregates or cross the
     * kernel boundary — only plain bindings, fn params and fn returns. */
    for (int i = 0; i < NSTRUCTS; i++)
        for (int f = 0; f < STRUCTS[i].nf; f++) {
            validate_ty(STRUCTS[i].fs[f].ty, STRUCTS[i].name);
            /* v0.25: an OWN container may hold own fields — it is the one
             * binding they live in, and its own discipline carries them. */
            if (ty_is_own(STRUCTS[i].fs[f].ty) && !STRUCTS[i].is_own)
                die("%s: own type in field '%s.%s' — own values cannot nest in other types (v0.17)",
                    FILENAME, STRUCTS[i].name, STRUCTS[i].fs[f].name);
        }
    for (int i = 0; i < NXFN; i++) {
        validate_ty(XFNS[i].ret, XFNS[i].name);
        for (int p = 0; p < XFNS[i].np; p++) {
            validate_ty(XFNS[i].ps[p].ty, XFNS[i].name);
            if (ty_is_own(XFNS[i].ps[p].ty))
                die("%s: own type in syscall '%s' — the kernel is not an owner (v0.17)",
                    FILENAME, XFNS[i].name);
        }
    }
    for (int i = 0; i < NFN; i++) {
        validate_ty(FNS[i].ret, FNS[i].name);
        for (int p = 0; p < FNS[i].np; p++) validate_ty(FNS[i].ps[p].ty, FNS[i].name);
    }

    for (int i = 0; i < NENUMS; i++)
        for (int v = 0; v < ENUMS[i].nv; v++)
            for (int f = 0; f < ENUMS[i].vs[v].nf; f++) {
                validate_ty(ENUMS[i].vs[v].fs[f].ty, ENUMS[i].name);
                if (ty_is_own(ENUMS[i].vs[v].fs[f].ty))
                    die("%s: own type in variant '%s.%s' — own values cannot nest in other types (v0.17)",
                        FILENAME, ENUMS[i].name, ENUMS[i].vs[v].name);
            }
    for (int i = 0; i < NMETHODS; i++) {
        Method* m = &METHODS[i];
        if (!struct_lookup(m->type) && !enum_lookup(m->type))
            die("%s: impl for unknown type '%s'", FILENAME, m->type);
        if (struct_lookup(m->type) && struct_lookup(m->type)->is_own)
            die("%s: impl for own type '%s' — by-value self would move the receiver (v0.17)",
                FILENAME, m->type);
        validate_ty(m->ret, m->name);
        for (int p = 0; p < m->np; p++) validate_ty(m->ps[p].ty, m->name);
    }

    fprintf(OUT, "/* Generated by ncc from %s */\n#include \"nyxrt.h\"\n\n", srcname);
    /* Type layouts in declaration order (v0.24): a struct may hold an enum
     * by value when the enum is declared first — as C requires. The two
     * tables are each in source order, so this is a merge on seq. */
    {
        int si = 0, ei = 0;
        while (si < NSTRUCTS || ei < NENUMS) {
            if (ei >= NENUMS || (si < NSTRUCTS && STRUCTS[si].seq < ENUMS[ei].seq))
                gen_struct_layout(si++);
            else
                gen_enum_layout(ei++);
        }
    }
    /* Function-pointer typedefs for the fn types the program declares
     * (v0.24) — after the layouts they may mention, before anything that
     * names them. A signature interned later (a bare fn value with no
     * declared type anywhere) has no typedef, and binding one is refused. */
    for (int i = 0; i < NFNTYS; i++) {
        fputs("typedef ", OUT);
        emit_fn_declarator(i, FNTYS[i].cname);
        fputs(";\n", OUT);
    }
    if (NFNTYS) fputc('\n', OUT);
    EMITTED_FNTYS = NFNTYS;
    gen_extern_fns();
    for (int i = 0; i < NFN; i++) { gen_fn_sig(&FNS[i]); fputs(";\n", OUT); }
    for (int i = 0; i < NMETHODS; i++) {        /* method prototypes */
        Method* m = &METHODS[i];
        fputs("static ", OUT);
        if (!m->ret.name || is_never(m->ret)) fputs("void", OUT);
        else emit_type(m->ret);
        fprintf(OUT, " %s_%s(%s self", m->type, m->name, m->type);
        for (int p = 0; p < m->np; p++) {
            fputs(", ", OUT);
            emit_type(m->ps[p].ty);
            fprintf(OUT, " %s", m->ps[p].name);
        }
        fputs(");\n", OUT);
    }
    if (NFN || NMETHODS) fputs("\n", OUT);
    for (int i = 0; i < NMETHODS; i++) {        /* method bodies */
        Method* m = &METHODS[i];
        IID = 0;
        CUR_RET = m->ret;
        CUR_FN_CAPS = 0;                  /* methods hold no capabilities (v0.14) */
        NVARS = 0;
        NDEFERS = 0;
        BLOCK_DEPTH = 0;
        LOOP_DEPTH = MATCH_DEPTH = 0;
        vars_add((char*)"self", ty_named(m->type), 0);   /* receiver: immutable */
        for (int p = 0; p < m->np; p++) vars_add(m->ps[p].name, m->ps[p].ty, 0);
        if (m->ret.name && !is_never(m->ret) && !block_guarantees(m->body))
            die("%s: not every path through method '%s.%s' returns a %s — add a tail expression or a return",
                FILENAME, m->type, m->name, ty_str(m->ret));
        fputs("static ", OUT);
        if (!m->ret.name || is_never(m->ret)) fputs("void", OUT);
        else emit_type(m->ret);
        fprintf(OUT, " %s_%s(%s self", m->type, m->name, m->type);
        for (int p = 0; p < m->np; p++) {
            fputs(", ", OUT);
            emit_type(m->ps[p].ty);
            fprintf(OUT, " %s", m->ps[p].name);
        }
        fputs(") ", OUT);
        gen_block(m->body, 0, 1);
        fputs("\n\n", OUT);
    }
    for (int i = 0; i < NFN; i++) {
        Fn* f = &FNS[i];
        IID = 0;
        CUR_RET = f->ret;
        CUR_FN_CAPS = f->caps;
        NVARS = 0;                      /* fresh symbol table per function */
        NDEFERS = 0;                    /* defers are function-scoped */
        BLOCK_DEPTH = 0;
        LOOP_DEPTH = MATCH_DEPTH = 0;
        for (int p = 0; p < f->np; p++) {   /* parameters are immutable bindings */
            vars_add(f->ps[p].name, f->ps[p].ty, 0);
            if (ty_is_own(f->ps[p].ty))     /* v0.17: this fn is the owner now */
                VARS[NVARS - 1].own_state = OWN_HELD;
        }
        if (f->ret.name && !is_never(f->ret) && !block_guarantees(f->body))
            die("%s: not every path through '%s' returns a %s — add a tail expression or a return",
                FILENAME, f->name, ty_str(f->ret));
        STASH_ARGS = !strcmp(f->name, "main") && !f->np;
        gen_fn_sig(f);
        fputc(' ', OUT);
        gen_block(f->body, 0, 1);
        fputs("\n\n", OUT);
    }
}

/* ------------------------------------------------------------------ */
/* driver                                                             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* --ast (the parser's differential anchor): a postorder dump of the
 * finished tree, one line per node, children before parents — the
 * shape a parser rewritten in N will be held to, the way --tokens
 * holds the lexer. Absent optional slots print a bare "nil" line so
 * the walk order stays deterministic; types render as
 * ptrs:is_user:name; string payloads keep the --tokens rule (byte
 * counts, not bodies). Format spec: lang/docs/selfhost.md. */
static void ast_expr(const Expr* e);
static void ast_block(const Block* b);

static void ast_ty(Ty t) {
    fprintf(stdout, " %d:%d:%s", t.ptrs, t.is_user, t.name ? t.name : "-");
}

static void ast_expr(const Expr* e) {
    if (!e) { fprintf(stdout, "nil\n"); return; }
    switch (e->k) {
        case E_INT:  fprintf(stdout, "E 0 %ld\n", (long)e->ival); return;
        case E_BOOL: fprintf(stdout, "E 1 %ld\n", (long)e->ival); return;
        case E_STR:  fprintf(stdout, "E 2 #%d\n", e->slen); return;
        case E_INTERP:
            for (int i = 0; i < e->nfrags; i++)
                if (e->frags[i].e) ast_expr(e->frags[i].e);
            fprintf(stdout, "E 3 %d", e->nfrags);
            for (int i = 0; i < e->nfrags; i++) {
                if (e->frags[i].e)
                    fprintf(stdout, " @%d.%d.%d", e->frags[i].fmt, e->frags[i].width, e->frags[i].zero);
                else
                    fprintf(stdout, " #%d", e->frags[i].tlen);
            }
            fprintf(stdout, "\n");
            return;
        case E_PATH: fprintf(stdout, "E 4 %s\n", e->name); return;
        case E_CALL:
            ast_expr(e->callee);
            for (int i = 0; i < e->nargs; i++) ast_expr(e->args[i]);
            fprintf(stdout, "E 5 %d\n", e->nargs);
            return;
        case E_FIELD: ast_expr(e->base); fprintf(stdout, "E 6 %s\n", e->field); return;
        case E_UN: ast_expr(e->r); fprintf(stdout, "E 7 %s\n", e->op); return;
        case E_BIN: ast_expr(e->l); ast_expr(e->r); fprintf(stdout, "E 8 %s\n", e->op); return;
        case E_CAST: ast_expr(e->l); fprintf(stdout, "E 9"); ast_ty(e->cast_to); fprintf(stdout, "\n"); return;
        case E_SLIT:
            for (int i = 0; i < e->nargs; i++) ast_expr(e->args[i]);
            fprintf(stdout, "E 10 %d %s", e->nargs, e->name);
            for (int i = 0; i < e->nargs; i++) fprintf(stdout, " %s", e->snames[i]);
            fprintf(stdout, "\n");
            return;
        case E_ELIT:
            for (int i = 0; i < e->nargs; i++) ast_expr(e->args[i]);
            fprintf(stdout, "E 11 %d %s %s", e->nargs, e->name, e->field);
            for (int i = 0; i < e->nargs; i++) fprintf(stdout, " %s", e->snames ? e->snames[i] : "-");
            fprintf(stdout, "\n");
            return;
        case E_INDEX: ast_expr(e->l); ast_expr(e->r); fprintf(stdout, "E 12\n"); return;
    }
}

static void ast_stmt(const Stmt* s) {
    switch (s->k) {
        case S_LET: ast_expr(s->e); fprintf(stdout, "S 0 %d %d %s\n", s->is_mut, s->is_try, s->name); return;
        case S_ASSIGN: ast_expr(s->lhs); ast_expr(s->e); fprintf(stdout, "S 1 %d %s\n", s->is_try, s->aop ? s->aop : "="); return;
        case S_RET: ast_expr(s->e); fprintf(stdout, "S 2 %d\n", s->is_try); return;
        case S_EXPR: ast_expr(s->e); fprintf(stdout, "S 3 %d\n", s->is_try); return;
        case S_WHILE: ast_expr(s->cond); ast_block(s->body); fprintf(stdout, "S 4\n"); return;
        case S_IF:
            ast_expr(s->cond); ast_block(s->body);
            if (s->els) ast_block(s->els);
            fprintf(stdout, "S 5 %d\n", s->els ? 1 : 0);
            return;
        case S_BREAK: fprintf(stdout, "S 6\n"); return;
        case S_CONT: fprintf(stdout, "S 7\n"); return;
        case S_DEFER: ast_expr(s->e); fprintf(stdout, "S 8\n"); return;
        case S_MATCH:
            if (s->mtarget == MT_ASSIGN) ast_expr(s->lhs);
            ast_expr(s->e);
            for (int i = 0; i < s->narms; i++) {
                if (s->arms[i].body) ast_block(s->arms[i].body);
                else ast_expr(s->arms[i].val);
                fprintf(stdout, "A %s %d", s->arms[i].variant, s->arms[i].nbinds);
                for (int j = 0; j < s->arms[i].nbinds; j++) fprintf(stdout, " %s", s->arms[i].binds[j]);
                fprintf(stdout, " %d\n", s->arms[i].body ? 0 : 1);
            }
            fprintf(stdout, "S 9 %d %d %s %s\n", s->narms, s->mtarget,
                   s->name ? s->name : "-", s->aop ? s->aop : "-");
            return;
        case S_FOR: ast_expr(s->e); ast_expr(s->cond); ast_block(s->body); fprintf(stdout, "S 10 %s\n", s->name); return;
    }
}

static void ast_block(const Block* b) {
    if (!b) { fprintf(stdout, "nil\n"); return; }
    for (int i = 0; i < b->n; i++) ast_stmt(b->st[i]);
    if (b->tail) ast_expr(b->tail);
    fprintf(stdout, "B %d %d\n", b->n, b->tail ? 1 : 0);
}

static void ast_program(void) {
    for (int i = 0; i < NSTRUCTS; i++) {
        fprintf(stdout, "D %s %d %s %d", STRUCTS[i].name, STRUCTS[i].is_own,
               STRUCTS[i].drop_fn ? STRUCTS[i].drop_fn : "-", STRUCTS[i].nf);
        for (int j = 0; j < STRUCTS[i].nf; j++) {
            fprintf(stdout, " %s", STRUCTS[i].fs[j].name);
            ast_ty(STRUCTS[i].fs[j].ty);
        }
        fprintf(stdout, "\n");
    }
    for (int i = 0; i < NENUMS; i++) {
        for (int j = 0; j < ENUMS[i].nv; j++) {
            fprintf(stdout, "V %s %d", ENUMS[i].vs[j].name, ENUMS[i].vs[j].nf);
            for (int q = 0; q < ENUMS[i].vs[j].nf; q++) {
                fprintf(stdout, " %s", ENUMS[i].vs[j].fs[q].name);
                ast_ty(ENUMS[i].vs[j].fs[q].ty);
            }
            fprintf(stdout, "\n");
        }
        fprintf(stdout, "U %s %d\n", ENUMS[i].name, ENUMS[i].nv);
    }
    for (int i = 0; i < NXFN; i++) {
        fprintf(stdout, "X %s %ld %d %d", XFNS[i].name, (long)XFNS[i].num, XFNS[i].caps, XFNS[i].np);
        for (int j = 0; j < XFNS[i].np; j++) {
            fprintf(stdout, " %s", XFNS[i].ps[j].name);
            ast_ty(XFNS[i].ps[j].ty);
        }
        ast_ty(XFNS[i].ret);
        fprintf(stdout, "\n");
    }
    for (int i = 0; i < NFN; i++) {
        ast_block(FNS[i].body);
        fprintf(stdout, "F %s %d %d", FNS[i].name, FNS[i].caps, FNS[i].np);
        for (int j = 0; j < FNS[i].np; j++) {
            fprintf(stdout, " %s", FNS[i].ps[j].name);
            ast_ty(FNS[i].ps[j].ty);
        }
        ast_ty(FNS[i].ret);
        fprintf(stdout, "\n");
    }
    for (int i = 0; i < NMETHODS; i++) {
        ast_block(METHODS[i].body);
        fprintf(stdout, "M %s %s %d", METHODS[i].type, METHODS[i].name, METHODS[i].np);
        for (int j = 0; j < METHODS[i].np; j++) {
            fprintf(stdout, " %s", METHODS[i].ps[j].name);
            ast_ty(METHODS[i].ps[j].ty);
        }
        ast_ty(METHODS[i].ret);
        fprintf(stdout, "\n");
    }
    fprintf(stdout, ".\n");
}

static void finish_output(FILE* stream, const char* path) {
    int failed = ferror(stream);
    // Preserve earlier buffered-write failures even if close/flush succeeds.
    int rc = path ? fclose(stream) : fflush(stream);
    if (rc != 0) failed = 1;
    if (!path && ferror(stream)) failed = 1;
    if (failed) die("ncc: write error on '%s'", path ? path : "stdout");
}

int main(int argc, char** argv) {
    const char* in = NULL;
    const char* out = NULL;
    int tokens_mode = 0;                  /* --tokens: dump the token stream */
    int ast_mode = 0;                     /* --ast: dump the parsed tree */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--tokens")) tokens_mode = 1;
        else if (!strcmp(argv[i], "--ast")) ast_mode = 1;
        else if (argv[i][0] == '-') die("ncc: unknown option '%s'", argv[i]);
        else if (!in) in = argv[i];
        else die("ncc: multiple input files");
    }
    if (!in) die("usage: ncc <input.n> [-o output.c | --tokens | --ast]");

    FILE* f = fopen(in, "rb");
    if (!f) die("ncc: cannot open '%s'", in);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("ncc: read error on '%s'", in);
    fclose(f);

    FILENAME = in;
    SRC = buf;
    LEN = (int)sz;
    CUR = next_token();
    if (tokens_mode) {                    /* the differential-test anchor: one
                                           * line per token — kind + source
                                           * line, plus the lexeme where one
                                           * exists: idents and #[drop] names
                                           * verbatim, ints as their PARSED
                                           * value (hex and '_' normalized),
                                           * string segments as their
                                           * processed byte COUNT. A lexer
                                           * rewritten in N is held to
                                           * exactly this stream. */
        for (;;) {
            if (CUR.k == T_IDENT || CUR.k == T_ATTR_DROP)
                fprintf(stdout, "%d %d %s\n", (int)CUR.k, CUR.line, CUR.s);
            else if (CUR.k == T_INT)
                fprintf(stdout, "%d %d %ld\n", (int)CUR.k, CUR.line, (long)CUR.ival);
            else if (CUR.k >= T_STR && CUR.k <= T_STR_TAIL)
                fprintf(stdout, "%d %d #%d\n", (int)CUR.k, CUR.line, CUR.slen);
            else
                fprintf(stdout, "%d %d\n", (int)CUR.k, CUR.line);
            if (CUR.k == T_EOF) break;
            CUR = next_token();
        }
        finish_output(stdout, NULL);
        return 0;
    }
    parse_program();
    if (ast_mode) { ast_program(); finish_output(stdout, NULL); return 0; }
    validate_drops();                     /* v0.19: destructor wiring */

    OUT = out ? fopen(out, "w") : stdout;
    if (!OUT) die("ncc: cannot open output '%s'", out);
    gen_program(in);
    finish_output(OUT, out);

    fprintf(stderr, "ncc: OK — %d extern syscall(s), %d function(s)%s%s\n",
            NXFN, NFN, out ? " -> " : "", out ? out : "");
    return 0;
}
