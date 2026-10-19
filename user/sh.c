#include "libc.h"

/* sh — the NyxOS userspace shell. Runs entirely in ring 3 on the Unix process
 * toolkit the kernel grew in v5.7.25..v5.8.6: fork() + execve(argv) + waitpid()
 * + pipe() + dup2(). A command line is a list of pipelines joined by ';', '&&' and
 * '||'; each pipeline is up to MAX_STAGES programs joined by '|', and each stage runs
 * as a fork+execve'd child with its stdin/stdout wired to the neighbouring pipes, and
 * the shell waitpid()s them all. '&&'/'||' gate the next pipeline on the last one's $?.
 * `$(command)` (command substitution) runs `command` in a forked subshell and splices
 * its captured stdout into the line before it is parsed. Words may be single- ('...',
 * fully literal) or double-quoted ("...", but $VAR / $? / $(...) still expand).
 *
 * Modes: `sh -c "line"` runs one line; with no arguments it is INTERACTIVE —
 * read(0, …) is a blocking, echoing line read from the keyboard (kernel
 * canonical mode, v5.8.8), so you type commands at a live `sh$ ` prompt.
 * Builtins: `exit` leaves the shell, `demo` runs the canned script. */

#define MAX_STAGES 4
#define MAX_ARGS   12

/* Trim leading/trailing spaces in place. */
static char* trim(char* s) {
    while (*s == ' ') s++;
    char* e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = '\0';
    return s;
}

/* Split a stage ("prog arg1 arg2") into an argv[]; modifies s. Returns argc. */
static int split_args(char* s, char** av, int max) {
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ') *s++ = '\0';
        if (!*s) break;
        av[n++] = s;
        while (*s && *s != ' ') s++;
    }
    av[n] = 0;
    return n;
}

/* ---- quoting: '...' and "..." ---------------------------------------------- */
/* Shells must let a quoted argument contain characters that are otherwise special
 * to the parser — spaces (which would word-split), the operators ; & | < > and the
 * glob/expansion chars * ? ~ $ ( ). Rather than teach every splitter about quotes,
 * preprocess_quotes() strips the quote marks and rewrites each protected character
 * to a control-byte PLACEHOLDER that none of the splitters/expanders look for; the
 * real byte is restored (q_unprotect) only when the final argv / redirect filename
 * is built. Single quotes protect everything (fully literal); double quotes leave
 * $ ( ) alone so $VAR / $? / $(...) still expand inside them. */
#define Q_SP 0x01
#define Q_TAB 0x11
#define Q_SEMI 0x02
#define Q_AMP 0x03
#define Q_PIPE 0x04
#define Q_LT 0x05
#define Q_GT 0x06
#define Q_STAR 0x07
#define Q_QUES 0x0B
#define Q_TILDE 0x0C
#define Q_DOLLAR 0x0E
#define Q_LP 0x0F
#define Q_RP 0x10

/* Map a special char to its placeholder. `single`=1 also protects $ ( ) so nothing
 * expands inside single quotes; in double quotes those pass through untouched. */
static char q_protect(char c, int single) {
    switch (c) {
        case ' ':  return Q_SP;
        case '\t': return Q_TAB;
        case ';':  return Q_SEMI;
        case '&':  return Q_AMP;
        case '|':  return Q_PIPE;
        case '<':  return Q_LT;
        case '>':  return Q_GT;
        case '*':  return Q_STAR;
        case '?':  return Q_QUES;
        case '~':  return Q_TILDE;
    }
    if (single) switch (c) {
        case '$': return Q_DOLLAR;
        case '(': return Q_LP;
        case ')': return Q_RP;
    }
    return c;
}

/* Restore placeholder bytes back to their real characters, in place. Called on the
 * argv words and redirect filenames just before they are used (exec / open / cd). */
static void q_unprotect(char* s) {
    for (; *s; s++) switch ((unsigned char)*s) {
        case Q_SP:     *s = ' ';  break;
        case Q_TAB:    *s = '\t'; break;
        case Q_SEMI:   *s = ';';  break;
        case Q_AMP:    *s = '&';  break;
        case Q_PIPE:   *s = '|';  break;
        case Q_LT:     *s = '<';  break;
        case Q_GT:     *s = '>';  break;
        case Q_STAR:   *s = '*';  break;
        case Q_QUES:   *s = '?';  break;
        case Q_TILDE:  *s = '~';  break;
        case Q_DOLLAR: *s = '$';  break;
        case Q_LP:     *s = '(';  break;
        case Q_RP:     *s = ')';  break;
    }
}

/* Strip quotes from `in` -> `out`, protecting quoted specials with placeholders.
 * A `$(...)` substitution (outside single quotes) is copied VERBATIM — the command
 * inside keeps its real spaces/quotes, and the subshell re-parses them when it runs
 * (so `"$(echo hi)"` runs `echo hi`, not one mangled word). */
static void preprocess_quotes(const char* in, char* out, int outsz) {
    int o = 0, state = 0;            /* 0 = normal, 1 = single '', 2 = double "" */
    for (int i = 0; in[i] && o < outsz - 1; ) {
        char c = in[i];
        if (state != 1 && c == '$' && in[i + 1] == '(') {   /* copy $(...) verbatim */
            int depth = 0;
            while (in[i] && o < outsz - 1) {
                char d = in[i];
                if (d == '(') depth++;
                else if (d == ')') depth--;
                out[o++] = d; i++;
                if (depth == 0 && d == ')') break;
            }
            continue;
        }
        if (state == 0) {
            if (c == '\'') state = 1;
            else if (c == '"') state = 2;
            else out[o++] = c;
        } else if (state == 1) {                            /* single: fully literal */
            if (c == '\'') state = 0;
            else out[o++] = q_protect(c, 1);
        } else {                                            /* double: keep $ ( ) live */
            if (c == '"') state = 0;
            else out[o++] = q_protect(c, 0);
        }
        i++;
    }
    out[o] = '\0';
}

/* Pull I/O redirections out of an argv (in place): the tokens `<`, `>`, `>>`, `2>`,
 * `2>>` (each followed by a filename) and their attached forms `<f` / `>f` / `>>f` /
 * `2>f` / `2>>f`. Sets *infile / *outfile / *append (stdout) and *errfile / *errappend
 * (stderr, fd 2) and compacts argv to the command words only. */
static void parse_redir(char** av, int* pac, char** infile, char** outfile, int* append,
                        char** errfile, int* errappend) {
    *infile = 0; *outfile = 0; *append = 0; *errfile = 0; *errappend = 0;
    int n = *pac, w = 0;
    for (int i = 0; i < n; i++) {
        char* t = av[i];
        if (t[0] == '<') {
            *infile = t[1] ? t + 1 : (i + 1 < n ? av[++i] : 0);
        } else if (t[0] == '>' && t[1] == '>') {
            *append = 1;
            *outfile = t[2] ? t + 2 : (i + 1 < n ? av[++i] : 0);
        } else if (t[0] == '>') {
            *append = 0;
            *outfile = t[1] ? t + 1 : (i + 1 < n ? av[++i] : 0);
        } else if (t[0] == '2' && t[1] == '>' && t[2] == '>') {
            *errappend = 1;                                        /* 2>>file: append stderr */
            *errfile = t[3] ? t + 3 : (i + 1 < n ? av[++i] : 0);
        } else if (t[0] == '2' && t[1] == '>') {
            *errappend = 0;                                        /* 2>file: truncate stderr */
            *errfile = t[2] ? t + 2 : (i + 1 < n ? av[++i] : 0);
        } else {
            av[w++] = t;
        }
    }
    av[w] = 0;
    *pac = w;
}

static const char* env_get(const char* name);   /* forward: defined with the env table below */

/* True if `path` names an openable file — used to probe $PATH candidates. */
static int file_exists(const char* path) {
    long fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    close((int)fd);
    return 1;
}

/* Resolve a command name to a runnable path. A name containing '/' is an explicit
 * path (absolute or cwd-relative) and used verbatim — never $PATH-searched. Otherwise
 * each ':'-separated directory in $PATH is tried in order, "<dir>/<name>" then
 * "<dir>/<name>.elf" (NyxOS executables carry the .elf suffix), and the first that
 * exists wins — so $PATH is authoritative (a command not on it is "not found",
 * signalled by an empty `out`). Only if $PATH is unset/empty does it fall back to the
 * legacy "/<name>.elf" so the shell still works with no environment. */
static void resolve(const char* name, char* out) {
    if (strchr(name, '/')) { strcpy(out, name); return; }   /* explicit path: no search */
    const char* path = env_get("PATH");
    if (path && *path) {
        for (const char* p = path; *p; ) {
            char dir[96]; int di = 0;
            while (*p && *p != ':' && di < (int)sizeof(dir) - 1) dir[di++] = *p++;
            dir[di] = '\0';
            while (*p == ':') p++;                           /* skip separator(s) */
            if (di == 0) continue;                           /* empty entry */
            const char* sep = (dir[di - 1] == '/') ? "" : "/";
            char cand[160];
            snprintf(cand, sizeof(cand), "%s%s%s", dir, sep, name);
            if (file_exists(cand)) { strcpy(out, cand); return; }
            snprintf(cand, sizeof(cand), "%s%s%s.elf", dir, sep, name);
            if (file_exists(cand)) { strcpy(out, cand); return; }
        }
        out[0] = '\0';                                       /* searched $PATH, not found */
        return;
    }
    out[0] = '/'; strcpy(out + 1, name); strcat(out, ".elf");   /* no $PATH: legacy fallback */
}

/* Shell-local environment (execve doesn't carry envp yet, so `$VAR` lives here) and
 * the last command's exit status (`$?`). */
#define MAX_ENV 16
static char env_name[MAX_ENV][32];
static char env_val[MAX_ENV][96];
static int  env_count;
static int  last_status;

static const char* env_get(const char* name) {
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_name[i], name) == 0) return env_val[i];
    return 0;
}
static void env_set(const char* name, const char* val) {
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_name[i], name) == 0) {
            strncpy(env_val[i], val, sizeof(env_val[i]) - 1);
            env_val[i][sizeof(env_val[i]) - 1] = '\0';
            return;
        }
    if (env_count >= MAX_ENV) return;
    strncpy(env_name[env_count], name, 31); env_name[env_count][31] = '\0';
    strncpy(env_val[env_count], val, 95);   env_val[env_count][95] = '\0';
    env_count++;
}

/* Flatten the shell env into a NULL-terminated "NAME=VALUE" vector for execve, so
 * children inherit it (and `env` can print it). Static storage: the caller is a
 * forked child about to execve, and the kernel copies the strings before the new
 * image is built. */
static char** env_build(void) {
    static char store[MAX_ENV][160];
    static char* envp[MAX_ENV + 1];
    int n = 0;
    for (int i = 0; i < env_count && n < MAX_ENV; i++) {
        snprintf(store[n], sizeof(store[n]), "%s=%s", env_name[i], env_val[i]);
        envp[n] = store[n];
        n++;
    }
    envp[n] = 0;
    return envp;
}

static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
/* A variable name's FIRST char: a letter or '_', never a digit (so `1x=…` is not a name). */
static int is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* Substitute `$NAME` (from the env) and `$?` (last exit status) in `in` -> `out`.
 * An unknown variable expands to nothing, like a real shell. */
static void expand_vars(const char* in, char* out, int outsz) {
    int o = 0;
    for (int i = 0; in[i] && o < outsz - 1; ) {
        if (in[i] == '$' && in[i + 1] == '?') {
            char num[16]; snprintf(num, sizeof(num), "%d", last_status);
            for (int k = 0; num[k] && o < outsz - 1; k++) out[o++] = num[k];
            i += 2;
        } else if (in[i] == '$' && is_name_char(in[i + 1])) {
            char name[32]; int ni = 0;
            i++;
            while (is_name_char(in[i]) && ni < 31) name[ni++] = in[i++];
            name[ni] = '\0';
            const char* v = env_get(name);
            if (v) for (int k = 0; v[k] && o < outsz - 1; k++) out[o++] = v[k];
        } else if (in[i] == '~' && (i == 0 || in[i - 1] == ' ') &&
                   (in[i + 1] == '/' || in[i + 1] == ' ' || in[i + 1] == '\0')) {
            const char* h = env_get("HOME");            /* ~ / ~/... -> $HOME */
            if (!h || !*h) h = "/home/user";
            for (int k = 0; h[k] && o < outsz - 1; k++) out[o++] = h[k];
            i++;                                         /* consume the ~ */
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* Builtins that must run IN the shell process (they change its cwd/env — a forked
 * child couldn't). cd/pwd use the chdir/getcwd syscalls; export updates the env. */
static void builtin_cd(char* arg) {
    if (arg) q_unprotect(arg);                   /* a quoted dir may hold protected bytes */
    const char* dir = (arg && *arg) ? arg : "/home/user";
    if (chdir(dir) != 0) { printf("cd: %s: no such directory\n", dir); last_status = 1; }
}
static void builtin_pwd(void) {
    char buf[128];
    if (getcwd(buf, sizeof(buf)) >= 0) { write(1, buf, strlen(buf)); write(1, "\n", 1); }
}
static void builtin_export(char* rest) {
    q_unprotect(rest);                           /* export NAME="value with spaces" */
    char* eq = strchr(rest, '=');
    if (!eq) { printf("usage: export NAME=value\n"); return; }
    *eq = '\0';
    env_set(trim(rest), trim(eq + 1));
}

/* A bare `NAME=value` assignment (bash-style, no `export` keyword). Recognised only when
 * the segment starts with a name char and every char up to the first `=` is a name char,
 * so `x=5` assigns but `echo a=b`, `a b=1`, `=5` and `1x=…` do not. This shell keeps ONE
 * name table shared with the environment (no separate unexported-variable scope), so the
 * value is stored just like `export` minus the keyword — usable in later `$NAME` expansion
 * and inherited by children. Returns 1 if it was an assignment (and stored it). */
static int try_assignment(char* line) {
    if (!is_name_start(line[0])) return 0;
    int i = 1;
    while (is_name_char(line[i])) i++;
    if (line[i] != '=') return 0;                /* no `=` right after the name -> a command */
    builtin_export(line);                        /* same store path (unprotect + trim both sides) */
    return 1;
}

/* Job-control builtins (defined with the jobs table below). */
static void builtin_jobs(void);
static void builtin_fg(char* arg);
static void builtin_bg(char* arg);

/* Run a cd/pwd/export/jobs/fg/bg/true/false builtin if `line` is one; returns 1 if
 * handled. `true`/`false` exist purely to set `$?` for && / || testing (bash has them
 * as builtins too); they can't be exec'd but are always available at the prompt. */
static int try_builtin(char* line) {
    if (try_assignment(line)) return 1;          /* NAME=value (before command lookup, like bash) */
    if (strcmp(line, "pwd") == 0) { builtin_pwd(); return 1; }
    if (strcmp(line, "true") == 0) { last_status = 0; return 1; }
    if (strcmp(line, "false") == 0) { last_status = 1; return 1; }
    if (strcmp(line, "jobs") == 0) { builtin_jobs(); return 1; }
    if (strncmp(line, "cd", 2) == 0 && (line[2] == ' ' || line[2] == '\0')) {
        builtin_cd(line[2] ? trim(line + 3) : "");
        return 1;
    }
    if (strncmp(line, "fg", 2) == 0 && (line[2] == ' ' || line[2] == '\0')) {
        builtin_fg(line[2] ? trim(line + 3) : ""); return 1;
    }
    if (strncmp(line, "bg", 2) == 0 && (line[2] == ' ' || line[2] == '\0')) {
        builtin_bg(line[2] ? trim(line + 3) : ""); return 1;
    }
    if (strncmp(line, "export ", 7) == 0) { builtin_export(line + 7); return 1; }
    return 0;
}

/* Job control. Each `&` background pipeline and each Ctrl-Z-stopped foreground job
 * gets a slot here; `jobs` lists them, `fg`/`bg` resume them. The shell reaps its own
 * children — the kernel's reap_zombies only frees procs whose parent has already gone,
 * so an alive shell owns its zombies. */
#define MAX_JOBS 16
typedef struct {
    long pid;          /* the job's (last-stage) pid; 0 = free slot */
    int  stopped;      /* 1 = stopped (Ctrl-Z), 0 = running in the background */
    char cmd[64];      /* the command line, for the `jobs` listing */
} job_t;
static job_t jobs[MAX_JOBS];

/* Record a job; returns its 1-based number, or -1 if the table is full. */
static int job_add(long pid, int stopped, const char* cmd) {
    if (pid <= 0) return -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid; jobs[i].stopped = stopped;
            strncpy(jobs[i].cmd, cmd, sizeof(jobs[i].cmd) - 1);
            jobs[i].cmd[sizeof(jobs[i].cmd) - 1] = '\0';
            return i + 1;
        }
    }
    return -1;
}

/* Look up a job by 1-based number, or the most recent one when n <= 0. NULL if none. */
static job_t* job_find(int n) {
    if (n > 0 && n <= MAX_JOBS && jobs[n - 1].pid) return &jobs[n - 1];
    if (n <= 0)
        for (int i = MAX_JOBS - 1; i >= 0; i--) if (jobs[i].pid) return &jobs[i];
    return 0;
}

/* Non-blocking sweep of finished background jobs. Called before each prompt. Stopped
 * jobs aren't running, so they're skipped (they only resume via fg/bg). A final
 * waitpid(-1) mop-up reaps any other finished children (non-leader pipeline stages). */
static void reap_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0 || jobs[i].stopped) continue;
        int st = 0;
        long r = waitpid3((int)jobs[i].pid, &st, WNOHANG);
        if (r == jobs[i].pid) { printf("[%d]  Done       %s\n", i + 1, jobs[i].cmd); jobs[i].pid = 0; }
        else if (r < 0)       { jobs[i].pid = 0; }
    }
    int st;
    while (waitpid3(-1, &st, WNOHANG) > 0) { }   /* mop up other finished children */
}

/* Parse a job spec: "%2"/"2" -> 2, empty -> 0 (most recent). */
static int job_num(const char* arg) {
    if (!arg || !*arg) return 0;
    if (arg[0] == '%') arg++;
    return atoi(arg);
}

static void builtin_jobs(void) {
    int any = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) continue;
        printf("[%d]  %s  %s\n", i + 1, jobs[i].stopped ? "Stopped" : "Running", jobs[i].cmd);
        any = 1;
    }
    if (!any) printf("jobs: no active jobs\n");
}

/* fg [%n] — resume job n (or the most recent) in the FOREGROUND: SIGCONT it if
 * stopped, claim the terminal, and wait (WUNTRACED, so another Ctrl-Z re-stops it). */
static void builtin_fg(char* arg) {
    job_t* j = job_find(job_num(arg));
    if (!j) { printf("fg: no such job\n"); return; }
    long pid = j->pid;
    printf("%s\n", j->cmd);                       /* real shells echo the resumed cmd */
    if (j->stopped) { kill((int)pid, SIGCONT); j->stopped = 0; }
    setfg(pid);
    int st = 0;
    waitpid3((int)pid, &st, WUNTRACED);
    setfg((int)getpid());
    if (WIFSTOPPED(st)) { j->stopped = 1; printf("\nStopped    %s\n", j->cmd); }
    else { last_status = st; j->pid = 0; }        /* exited -> free the slot */
}

/* bg [%n] — resume a stopped job in the BACKGROUND (SIGCONT, keep running). */
static void builtin_bg(char* arg) {
    job_t* j = job_find(job_num(arg));
    if (!j) { printf("bg: no such job\n"); return; }
    if (!j->stopped) { printf("bg: job already running\n"); return; }
    kill((int)j->pid, SIGCONT);
    j->stopped = 0;
    printf("[bg] %s &\n", j->cmd);
}

/* ---- filename globbing (* ? [..]) ----------------------------------------- */
#define MAX_GLOB 32
static nyx_dirent_t glob_ents[64];       /* .bss: getdents can't fault lazy pages */
static char  glob_store[MAX_GLOB][96];
static char* glob_av[MAX_GLOB + 1];

/* Match a '[...]' class at *pp (pointing at '[') against char c; returns 1/0 and
 * advances *pp past ']', or -1 for a malformed class (no ']' before the NUL),
 * leaving *pp untouched so the caller treats '[' as a literal. */
static int glob_match_class(const char** pp, char c) {
    const char* p = *pp + 1;                             /* skip '[' */
    int negate = 0;
    if (*p == '!' || *p == '^') { negate = 1; p++; }
    int matched = 0;
    int first = 1;                                       /* ']' in slot 0 is a literal */
    while (*p && (*p != ']' || first)) {
        first = 0;
        char lo;
        if (*p == '\\' && p[1]) { lo = p[1]; p += 2; }   /* escaped member */
        else                   { lo = *p;   p += 1; }
        if (*p == '-' && p[1] && p[1] != ']') {          /* range  lo-hi */
            const char* q = p + 1;
            char hi;
            if (*q == '\\' && q[1]) { hi = q[1]; p = q + 2; }
            else                   { hi = *q;   p = q + 1; }
            if ((unsigned char)c >= (unsigned char)lo &&
                (unsigned char)c <= (unsigned char)hi) matched = 1;
        } else {
            if ((unsigned char)c == (unsigned char)lo) matched = 1;
        }
    }
    if (*p != ']') return -1;                            /* unterminated -> literal '[' */
    *pp = p + 1;
    return negate ? !matched : matched;
}

/* Shell wildcard match — a byte-for-byte copy of the kernel's hardened matcher
 * (kernel/core/fnmatch.c, KAT-verified as `glob`): iterative, with a single '*'
 * backtrack point, so it runs in O(pat*str) and can neither blow the ring-3 stack
 * nor backtrack catastrophically on a pattern like "*a*a*a*b" the way the old
 * recursive matcher could. '*' any run (incl. empty), '?' one char, '[abc]'/
 * '[a-z]'/'[!..]' a character class, '\' escapes the following metacharacter. */
static int glob_match(const char* pat, const char* str) {
    const char* p = pat;
    const char* s = str;
    const char* star_p = 0;                              /* pattern after the last '*' */
    const char* star_s = 0;                              /* text pos when it was seen */

    while (*s) {
        int adv = 0;                                     /* consumed *s this token? */
        if (*p == '*') {
            star_p = ++p;                                /* record the one backtrack... */
            star_s = s;                                  /* ...try '*' == empty first */
            continue;
        } else if (*p == '?') {
            p++; adv = 1;
        } else if (*p == '[') {
            int r = glob_match_class(&p, *s);
            if (r < 0) { if (*p == *s) { p++; adv = 1; } }  /* malformed -> literal '[' */
            else if (r) { adv = 1; }                        /* matched; p past ']' */
        } else if (*p == '\\' && p[1]) {                 /* escaped literal */
            if (p[1] == *s) { p += 2; adv = 1; }
        } else {                                         /* ordinary literal / p at NUL */
            if (*p == *s) { p++; adv = 1; }
        }

        if (adv) {
            s++;
        } else if (star_p) {                             /* no match: '*' eats one more */
            p = star_p;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;                                /* trailing '*'s match empty */
    return *p == '\0';
}

/* Expand any wildcard args in av[0..*pac) against the filesystem, returning the new
 * argv (and count via *pac). A plain arg passes through; an arg with '*'/'?'/'[' is
 * replaced by the sorted-ish directory entries it matches — leading-dot names are
 * skipped unless the pattern itself starts with '.'. A wildcard with no match stays
 * literal (nullglob off, like bash's default). Runs in the forked child before exec. */
static char** expand_globs(char** av, int* pac) {
    int out = 0;
    for (int a = 0; a < *pac && out < MAX_GLOB; a++) {
        char* arg = av[a];
        if (!strchr(arg, '*') && !strchr(arg, '?')) {    /* no wildcard: copy through */
            strncpy(glob_store[out], arg, sizeof(glob_store[0]) - 1);
            glob_store[out][sizeof(glob_store[0]) - 1] = '\0';
            glob_av[out] = glob_store[out]; out++;
            continue;
        }
        /* Split off a directory prefix (everything up to the last '/'). */
        char dir[80]; int has_dir = 0;
        const char* pat = arg;
        char* slash = 0;
        for (char* q = arg; *q; q++) if (*q == '/') slash = q;
        if (slash) {
            int dl = (int)(slash - arg);
            if (dl == 0) { dir[0] = '/'; dir[1] = '\0'; } /* "/pat" -> dir "/" */
            else { if (dl > 78) dl = 78; memcpy(dir, arg, dl); dir[dl] = '\0'; }
            pat = slash + 1;
            has_dir = 1;
        }
        long n = getdents(has_dir ? dir : ".", glob_ents, 64);
        int matched = 0;
        for (long e = 0; e < n && out < MAX_GLOB; e++) {
            const char* nm = glob_ents[e].name;
            if (nm[0] == '.' && pat[0] != '.') continue; /* skip hidden / . / .. */
            if (!glob_match(pat, nm)) continue;
            if (has_dir) {
                const char* sep = (dir[strlen(dir) - 1] == '/') ? "" : "/";
                snprintf(glob_store[out], sizeof(glob_store[0]), "%s%s%s", dir, sep, nm);
            } else {
                strncpy(glob_store[out], nm, sizeof(glob_store[0]) - 1);
                glob_store[out][sizeof(glob_store[0]) - 1] = '\0';
            }
            glob_av[out] = glob_store[out]; out++;
            matched = 1;
        }
        if (!matched && out < MAX_GLOB) {                /* no match: keep the literal */
            strncpy(glob_store[out], arg, sizeof(glob_store[0]) - 1);
            glob_store[out][sizeof(glob_store[0]) - 1] = '\0';
            glob_av[out] = glob_store[out]; out++;
        }
    }
    glob_av[out] = 0;
    *pac = out;
    return glob_av;
}

/* Run one command line (destroys it). Stages split on '|', each fork+execve'd
 * with pipes wired via dup2. A trailing '&' backgrounds the whole pipeline (the
 * shell parks the pids and returns immediately); otherwise it reaps every stage
 * with waitpid. */
static void run_line(char* line) {
    /* Trailing '&' (ignoring spaces) -> background the pipeline. */
    int background = 0;
    {
        char* e = line + strlen(line);
        while (e > line && e[-1] == ' ') e--;
        if (e > line && e[-1] == '&') { background = 1; *--e = '\0'; }
    }

    char jobcmd[64];                             /* snapshot the command for the jobs list */
    strncpy(jobcmd, line, sizeof(jobcmd) - 1);
    jobcmd[sizeof(jobcmd) - 1] = '\0';

    char* stages[MAX_STAGES];
    int nstages = 0;
    char* p = line;
    stages[nstages++] = p;
    while ((p = strchr(p, '|')) != 0 && nstages < MAX_STAGES) {
        *p++ = '\0';
        stages[nstages++] = p;
    }

    long pids[MAX_STAGES];
    int prev_rd = -1;                          /* read end of the previous pipe */
    for (int s = 0; s < nstages; s++) {
        int has_next = (s < nstages - 1);
        int pfd[2] = { -1, -1 };
        if (has_next && pipe(pfd) != 0) { printf("sh: pipe failed\n"); return; }

        long pid = fork();
        if (pid == 0) {
            /* Child: stdin from the previous pipe, stdout into the next one,
             * then drop the raw fds (the dup took a reference) and become the
             * program. If execve returns, the program wasn't found. */
            if (prev_rd >= 0) { dup2(prev_rd, 0); close(prev_rd); }
            if (has_next)     { close(pfd[0]); dup2(pfd[1], 1); close(pfd[1]); }
            char* av[MAX_ARGS];
            int ac = split_args(trim(stages[s]), av, MAX_ARGS);
            char *infile, *outfile, *errfile; int append, errappend;
            parse_redir(av, &ac, &infile, &outfile, &append, &errfile, &errappend);
            if (ac == 0) exit(0);
            char** rav = expand_globs(av, &ac);   /* wildcard (* ?) expansion */
            /* Restore any quote-protected bytes now that all splitting/globbing is
             * done, so exec and open() see the real spaces / specials. */
            for (int qi = 0; qi < ac; qi++) q_unprotect(rav[qi]);
            if (infile)  q_unprotect(infile);
            if (outfile) q_unprotect(outfile);
            if (errfile) q_unprotect(errfile);
            /* File redirections override the pipe wiring for the affected fd. */
            if (infile) {
                long fd = open(infile, O_RDONLY, 0);
                if (fd < 0) { printf("sh: %s: cannot open\n", infile); exit(1); }
                dup2((int)fd, 0); close((int)fd);
            }
            if (outfile) {
                long fd = open(outfile, O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
                if (fd < 0) { printf("sh: %s: cannot create\n", outfile); exit(1); }
                dup2((int)fd, 1); close((int)fd);
            }
            if (errfile) {                          /* 2> / 2>>: route stderr (fd 2) to a file */
                long fd = open(errfile, O_CREAT | (errappend ? O_APPEND : O_TRUNC), 0644);
                if (fd < 0) { printf("sh: %s: cannot create\n", errfile); exit(1); }
                dup2((int)fd, 2); close((int)fd);
            }
            char path[160];
            resolve(rav[0], path);
            if (!path[0]) { printf("sh: %s: not found\n", rav[0]); exit(127); }  /* not on $PATH */
            execve(path, rav, env_build());   /* child inherits the shell environment */
            printf("sh: %s: not found\n", path);
            exit(127);
        } else if (pid < 0) {
            printf("sh: fork failed\n");
            return;
        }
        pids[s] = pid;
        /* Parent: close the ends now owned by the children, keep the new pipe's
         * read end for the next stage. Closing promptly is what makes EOF fire
         * once the last writer exits. */
        if (prev_rd >= 0) close(prev_rd);
        if (has_next) { close(pfd[1]); prev_rd = pfd[0]; }
    }

    if (background) {                        /* don't wait — record a running job */
        int jn = job_add(pids[nstages - 1], 0, jobcmd);
        printf("[%d]  %ld  %s\n", jn, pids[nstages - 1], jobcmd);
        return;
    }

    /* Foreground: claim the terminal for the job (so Ctrl-C / Ctrl-Z reach IT, not the
     * shell), wait with WUNTRACED so a Ctrl-Z stop parks the job instead of killing it,
     * then reclaim the terminal for the shell. */
    setfg(pids[nstages - 1]);
    for (int s = 0; s < nstages; s++) {
        int st = 0;
        waitpid3((int)pids[s], &st, WUNTRACED);
        if (WIFSTOPPED(st)) {               /* Ctrl-Z: park it as a stopped job */
            int jn = job_add(pids[s], 1, jobcmd);
            printf("\n[%d]  Stopped    %s\n", jn, jobcmd);
            break;                          /* leave any other pipeline stages running */
        }
        last_status = st;                   /* pipeline status = last stage ($?) */
        if (st != 0)
            printf("sh: [pid %d] exited with status %d\n", (int)pids[s], st);
    }
    setfg((int)getpid());                   /* shell reclaims the terminal */
}

/* ---- command lists: pipelines joined by ; && || --------------------------- */
/* A command line is a list of pipelines separated by operators: `;` (sequence,
 * always run the next), `&&` (run next only if the last succeeded, $?==0) and `||`
 * (run next only if the last failed). This sits ABOVE run_line's pipeline/'|'/'&'
 * handling. */
#define MAX_LIST 16
enum { OP_SEMI, OP_AND, OP_OR };

/* Split `line` (in place) into pipeline segments on the top-level operators, filling
 * seg[] and the operator that PRECEDES each segment into op[] (op[0] is unused). We
 * must not mistake `&&` for a background `&` or `||` for a pipe `|`: only the doubled
 * forms are list operators; a lone `&`/`|` is left inside the segment for run_line's
 * background / pipeline splitter to handle. Returns the segment count. */
static int split_list(char* line, char** seg, int* op, int max) {
    int n = 0;
    seg[n] = line; op[n] = OP_SEMI; n++;
    for (char* s = line; *s && n < max; ) {
        if (s[0] == '&' && s[1] == '&') {
            *s = '\0'; s += 2; seg[n] = s; op[n] = OP_AND; n++;
        } else if (s[0] == '|' && s[1] == '|') {
            *s = '\0'; s += 2; seg[n] = s; op[n] = OP_OR; n++;
        } else if (s[0] == ';') {
            *s = '\0'; s += 1; seg[n] = s; op[n] = OP_SEMI; n++;
        } else {
            s++;
        }
    }
    return n;
}

static void run_demo(void);              /* forward: the demo runs command lists too */
static int run_command_list(char* line); /* forward: capture_command runs a subshell */

/* ---- command substitution $(...) ------------------------------------------ */
/* Run `cmd` in a forked subshell and capture its stdout into `out`. The child is a
 * fork of the shell, so it can run builtins / pipelines / operators via
 * run_command_list; we redirect its fd 1 to a pipe and drain it in the parent. The
 * captured text has trailing whitespace trimmed and its internal newlines/tabs turned
 * into spaces — the unquoted word-splitting a real shell does for `$(...)`. */
static void capture_command(char* cmd, char* out, int outsz) {
    out[0] = '\0';
    int pfd[2];
    if (pipe(pfd) != 0) return;
    long pid = fork();
    if (pid == 0) {                          /* subshell: stdout -> pipe, run, exit */
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        run_command_list(cmd);
        exit(0);
    }
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return; }
    close(pfd[1]);                           /* parent only reads */
    int total = 0;
    for (;;) {
        char buf[128];
        long n = read(pfd[0], buf, sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n && total < outsz - 1; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';   /* word-split on IFS */
            out[total++] = c;
        }
    }
    out[total] = '\0';
    close(pfd[0]);
    int st;
    waitpid3((int)pid, &st, 0);
    while (total > 0 && out[total - 1] == ' ') out[--total] = '\0';   /* trim trailing */
}

/* Expand every `$(command)` in `in` -> `out`, replacing it with the command's captured
 * output. The closing paren is matched by depth (so parens inside the command don't end
 * it early), and nested `$(...)` work for free because capture_command runs the inner
 * text back through run_command_list, which expands substitutions again. */
static void expand_cmdsubst(const char* in, char* out, int outsz) {
    int o = 0;
    for (int i = 0; in[i] && o < outsz - 1; ) {
        if (in[i] == '$' && in[i + 1] == '(') {
            int depth = 1, j = i + 2;
            while (in[j] && depth > 0) {
                if (in[j] == '(') depth++;
                else if (in[j] == ')') { depth--; if (depth == 0) break; }
                j++;
            }
            char inner[256];
            int il = 0;
            for (int k = i + 2; k < j && il < (int)sizeof(inner) - 1; k++) inner[il++] = in[k];
            inner[il] = '\0';
            char captured[512];
            capture_command(inner, captured, sizeof(captured));
            for (int k = 0; captured[k] && o < outsz - 1; k++) out[o++] = captured[k];
            i = (in[j] == ')') ? j + 1 : j;      /* skip past the closing ')' */
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* Run a full command line: the list of pipelines joined by ; && ||. Each segment is
 * var-expanded on its own (so `$?` sees the PREVIOUS segment's status), then run as a
 * builtin or a pipeline. Short-circuiting works because a skipped segment leaves
 * `last_status` untouched — exactly bash's behaviour for `a && b || c` chains.
 * Returns 1 if an `exit` builtin ran (the REPL should stop), else 0. */
static int run_command_list(char* line) {
    char quoted[512];                            /* strip quotes, protect quoted specials */
    preprocess_quotes(line, quoted, sizeof(quoted));
    char subst[512];                             /* then expand $(...) on the whole line */
    expand_cmdsubst(quoted, subst, sizeof(subst));
    char* seg[MAX_LIST];
    int   op[MAX_LIST];
    int   n = split_list(subst, seg, op, MAX_LIST);
    for (int i = 0; i < n; i++) {
        if (i > 0) {                                 /* honour the joining operator */
            if (op[i] == OP_AND && last_status != 0) continue;   /* && after failure */
            if (op[i] == OP_OR  && last_status == 0) continue;   /* || after success */
        }
        char* s = trim(seg[i]);
        if (!*s) continue;
        char xbuf[512];                              /* per-segment $VAR / $? / ~ expand */
        expand_vars(s, xbuf, sizeof(xbuf));
        char* x = trim(xbuf);
        if (!*x) continue;
        if (strcmp(x, "exit") == 0) return 1;
        if (strcmp(x, "demo") == 0) { run_demo(); last_status = 0; continue; }
        last_status = 0;                             /* builtins default to success... */
        if (try_builtin(x)) continue;                /* ...cd/false override on failure */
        run_line(x);                                 /* pipeline: sets last_status to $? */
    }
    return 0;
}

/* The canned demo — plain commands, a pipeline, redirection, and the new
 * && / || / ; operators. Every line exercises fork/execve/waitpid; the pipeline
 * adds pipe+dup2. Run via the `demo` builtin. */
static void run_demo(void) {
    static const char* script[] = {
        "pwd",
        "cd /home/user",
        "pwd",
        "cat welcome.txt",                 /* relative path -> resolves against cwd */
        "export NAME=NyxOS",
        "echo hello $NAME",                /* $VAR expansion */
        "greeting=hi",                     /* bare NAME=value assignment (no export) */
        "echo $greeting $NAME",            /* both expand */
        "ls / | grep elf",
        "echo redirected > /tmp/sh.txt",
        "cat /tmp/sh.txt | wc",
        "mkdir /tmp/proj",                 /* file tools: mkdir + touch + find */
        "touch /tmp/proj/b.txt /tmp/proj/a.txt",
        "find /tmp",
        "ls /tmp/proj | sort",             /* insertion order b,a -> sorted a,b */
        "rm /tmp/proj/a.txt /tmp/proj/b.txt",
        "ls /dev",                         /* device nodes */
        "echo swallowed by the void > /dev/null",
        "true && echo AND: runs after success",   /* && / || / ; operators */
        "false || echo OR: runs after failure",
        "false && echo skipped ; echo semicolon: always runs",
        "false; echo exit status was $?",  /* $? sees the previous segment */
        "echo working dir is $(pwd)",      /* command substitution $(...) */
        "echo nested: $(echo $(echo deep))",
        "echo \"double quoted: a; b | c\"",   /* quoting: specials stay literal */
        "echo 'single quoted: $HOME stays literal'",
        "echo \"double quoted expands: $NAME\"",   /* $VAR expands inside "" */
        "echo running in the background | upper &",
    };
    for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
        char buf[160];
        strncpy(buf, script[i], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        printf("sh$ %s\n", script[i]);
        run_command_list(buf);             /* handles builtins, pipelines, and ; && || */
    }
    printf("sh: demo done.\n");
}

/* SIGINT (Ctrl-C): abandon the current input line and drop to a fresh prompt, like
 * a real shell. We print ^C here; the interrupted read(0) returns EINTR (<0), so the
 * REPL loop just continues and reprints the prompt. */
static void on_sigint(int sig) {
    (void)sig;
    write(1, "^C\n", 3);
}

/* ---- line editor: raw-mode readline with history + cursor editing ---------- */
/* Runs on TTY_RAW (no kernel echo): every keystroke arrives as a byte, arrows as
 * ESC [ A/B/C/D (H/F = home/end), and we render the line ourselves. Redraws use
 * '\r' + full reprint + trailing-erase + backspaces to park the cursor — both the
 * GUI terminal capture (which rebuilds its pending line on '\r'/'\b') and a real
 * serial terminal render that correctly. */
#define HIST_MAX 16
static char hist[HIST_MAX][128];
static int  hist_count;

static void redraw(const char* prompt, const char* buf, int len, int pos, int drawn) {
    write(1, "\r", 1);
    write(1, prompt, strlen(prompt));
    if (len > 0) write(1, buf, len);
    for (int i = len; i < drawn; i++) write(1, " ", 1);   /* erase leftovers */
    int end = (drawn > len) ? drawn : len;
    for (int i = end; i > pos; i--) write(1, "\b", 1);    /* park cursor at pos */
}

/* ---- tab completion (pure userspace, over getdents) ------------------------ */
static nyx_dirent_t cmp_ents[64];      /* .bss: getdents can't fault lazy pages   */
static char    cmp_cand[48][64];       /* candidate names                         */
static unsigned char cmp_isdir[48];

static int str_prefix(const char* s, const char* pfx) {
    while (*pfx) if (*s++ != *pfx++) return 0;
    return 1;
}

/* Insert `s` into buf at *ppos, shifting the tail right; caps at buf[128]. */
static void insert_at(char* buf, int* plen, int* ppos, const char* s) {
    int len = *plen, pos = *ppos, sl = (int)strlen(s);
    if (len + sl > 126) sl = 126 - len;
    if (sl <= 0) return;
    for (int j = len - 1; j >= pos; j--) buf[j + sl] = buf[j];
    for (int j = 0; j < sl; j++) buf[pos + j] = s[j];
    *plen = len + sl; *ppos = pos + sl;
}

/* Complete the token ending at the cursor. Command position (line start, or right
 * after '|') completes shell builtins + /*.elf names; otherwise a filesystem path
 * via getdents on the token's directory. A single match is inserted (+ ' ' or '/');
 * several share their longest common prefix, or are listed. Returns 1 if it printed
 * a candidate list (so the caller redraws on a fresh line), else 0. */
static int do_complete(char* buf, int* plen, int* ppos) {
    int pos = *ppos, ts = pos;
    while (ts > 0 && buf[ts - 1] != ' ') ts--;
    int k = ts - 1;                              /* command position? */
    while (k >= 0 && buf[k] == ' ') k--;
    int is_cmd = (k < 0) || buf[k] == '|';

    char token[128]; int tlen = pos - ts;
    if (tlen > 127) tlen = 127;
    for (int i = 0; i < tlen; i++) token[i] = buf[ts + i];
    token[tlen] = '\0';

    const char* base = token;
    int nc = 0;

    if (is_cmd) {
        static const char* bi[] = { "cd", "pwd", "export", "exit", "demo" };
        for (int i = 0; i < 5 && nc < 48; i++)
            if (str_prefix(bi[i], token)) {
                strncpy(cmp_cand[nc], bi[i], 63); cmp_cand[nc][63] = 0; cmp_isdir[nc] = 0; nc++;
            }
        long n = getdents("/", cmp_ents, 64);
        for (long i = 0; i < n && nc < 48; i++) {
            char* nm = cmp_ents[i].name;
            int l = (int)strlen(nm);
            if (l > 4 && strcmp(nm + l - 4, ".elf") == 0) {
                char bare[64]; int bl = l - 4; if (bl > 63) bl = 63;
                for (int j = 0; j < bl; j++) bare[j] = nm[j];
                bare[bl] = 0;
                if (str_prefix(bare, token)) {
                    strncpy(cmp_cand[nc], bare, 63); cmp_cand[nc][63] = 0; cmp_isdir[nc] = 0; nc++;
                }
            }
        }
    } else {
        char dir[128]; int sl = -1;
        for (int i = 0; i < tlen; i++) if (token[i] == '/') sl = i;
        if (sl < 0) { strcpy(dir, "."); base = token; }
        else {
            int dl = (sl == 0) ? 1 : sl;         /* "/x" -> dir "/" */
            for (int i = 0; i < dl; i++) dir[i] = token[i];
            dir[dl] = 0; base = token + sl + 1;
        }
        long n = getdents(dir, cmp_ents, 64);
        for (long i = 0; i < n && nc < 48; i++)
            if (str_prefix(cmp_ents[i].name, base)) {
                strncpy(cmp_cand[nc], cmp_ents[i].name, 63); cmp_cand[nc][63] = 0;
                cmp_isdir[nc] = (cmp_ents[i].type == 1);
                nc++;
            }
    }

    int blen = (int)strlen(base);
    if (nc == 0) return 0;
    if (nc == 1) {                               /* unique: complete + suffix */
        char ins[80]; int il = 0;
        for (const char* r = cmp_cand[0] + blen; *r && il < 70; ) ins[il++] = *r++;
        ins[il++] = cmp_isdir[0] ? '/' : ' ';
        ins[il] = 0;
        insert_at(buf, plen, ppos, ins);
        return 0;
    }
    int lcp = (int)strlen(cmp_cand[0]);          /* longest common prefix */
    for (int i = 1; i < nc; i++) {
        int j = 0;
        while (j < lcp && cmp_cand[i][j] == cmp_cand[0][j]) j++;
        lcp = j;
    }
    if (lcp > blen) {                            /* extend to the shared prefix */
        char ins[80]; int il = 0;
        for (int j = blen; j < lcp && il < 70; j++) ins[il++] = cmp_cand[0][j];
        ins[il] = 0;
        insert_at(buf, plen, ppos, ins);
        return 0;
    }
    write(1, "\n", 1);                           /* ambiguous: list candidates */
    for (int i = 0; i < nc; i++) {
        write(1, cmp_cand[i], strlen(cmp_cand[i]));
        if (cmp_isdir[i]) write(1, "/", 1);
        write(1, "  ", 2);
    }
    write(1, "\n", 1);
    return 1;
}

/* Read one edited line into out (NUL-terminated, no '\n'). Returns its length,
 * or -1 if the read was interrupted (Ctrl-C). Restores canonical mode on exit. */
static int readline(const char* prompt, char* out, int outsz) {
    char buf[128], saved[128];
    int len = 0, pos = 0, drawn = 0, saved_len = 0;
    int hview = hist_count;                  /* one past the newest entry */
    ttymode(TTY_RAW);
    write(1, prompt, strlen(prompt));
    for (;;) {
        char kb[16];
        long n = read(0, kb, sizeof(kb));
        if (n < 0) { ttymode(TTY_CANON); return -1; }    /* EINTR: Ctrl-C */
        for (long i = 0; i < n; i++) {
            char c = kb[i];
            if (c == 0x1B) {                 /* ESC [ x — emitted atomically */
                if (i + 2 >= n || kb[i + 1] != '[') continue;   /* lone ESC: ignore */
                char x = kb[i + 2];
                i += 2;
                if (x == 'A' || x == 'B') {  /* history up / down */
                    if (x == 'A') {
                        if (hview == 0) continue;
                        if (hview == hist_count) {       /* stash the in-progress line */
                            for (int j = 0; j < len; j++) saved[j] = buf[j];
                            saved_len = len;
                        }
                        hview--;
                        strncpy(buf, hist[hview], sizeof(buf) - 1);
                    } else {
                        if (hview >= hist_count) continue;
                        hview++;
                        if (hview == hist_count) {       /* back to the stashed line */
                            for (int j = 0; j < saved_len; j++) buf[j] = saved[j];
                            buf[saved_len] = '\0';
                        } else {
                            strncpy(buf, hist[hview], sizeof(buf) - 1);
                        }
                    }
                    buf[sizeof(buf) - 1] = '\0';
                    len = pos = (int)strlen(buf);
                } else if (x == 'D') { if (pos > 0) pos--; }         /* left  */
                else if (x == 'C') { if (pos < len) pos++; }         /* right */
                else if (x == 'H') { pos = 0; }                      /* home  */
                else if (x == 'F') { pos = len; }                    /* end   */
                redraw(prompt, buf, len, pos, drawn); drawn = len;
                continue;
            }
            if (c == '\n') {                 /* finish: cursor to end, real newline */
                redraw(prompt, buf, len, len, drawn);
                write(1, "\n", 1);
                buf[len] = '\0';
                int cn = (len < outsz - 1) ? len : outsz - 1;
                for (int j = 0; j < cn; j++) out[j] = buf[j];
                out[cn] = '\0';
                if (cn > 0 && (hist_count == 0 || strcmp(hist[hist_count - 1], out) != 0)) {
                    if (hist_count == HIST_MAX) {        /* drop the oldest */
                        for (int j = 0; j < HIST_MAX - 1; j++) strcpy(hist[j], hist[j + 1]);
                        hist_count--;
                    }
                    strncpy(hist[hist_count], out, sizeof(hist[0]) - 1);
                    hist[hist_count][sizeof(hist[0]) - 1] = '\0';
                    hist_count++;
                }
                ttymode(TTY_CANON);
                return cn;
            }
            if (c == '\b' || c == 0x7F) {    /* delete before the cursor */
                if (pos > 0) {
                    for (int j = pos - 1; j < len - 1; j++) buf[j] = buf[j + 1];
                    pos--; len--;
                    redraw(prompt, buf, len, pos, drawn); drawn = len;
                }
                continue;
            }
            if (c == '\t') {                 /* tab completion */
                do_complete(buf, &len, &pos);
                redraw(prompt, buf, len, pos, len); drawn = len;
                continue;
            }
            if (c >= 0x20 && c < 0x7F && len < (int)sizeof(buf) - 1) {  /* insert */
                for (int j = len; j > pos; j--) buf[j] = buf[j - 1];
                buf[pos] = c;
                len++; pos++;
                redraw(prompt, buf, len, pos, drawn); drawn = len;
            }
        }
    }
}

int main(int argc, char** argv) {
    /* Seed a default environment so children (and `env`) see something out of the
     * box; `export NAME=value` adds to it, and every command inherits it. */
    env_set("USER", "nyx");
    env_set("HOME", "/home/user");
    env_set("SHELL", "/sh.elf");
    env_set("PATH", "/bin:/usr/bin:/");
    env_set("TERM", "nyx");

    /* sh -c "one command line" — may itself be a ; && || list */
    if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
        char buf[128];
        if (argc < 3) {
            printf("sh: -c requires a command\n");
            return 2;
        }
        if (strlen(argv[2]) >= sizeof(buf)) {
            printf("sh: command exceeds %u bytes\n", (unsigned)(sizeof(buf) - 1));
            return 2;
        }
        strcpy(buf, argv[2]);
        run_command_list(buf);
        return last_status;
    }

    /* Interactive REPL: read(0) blocks in the kernel's canonical line
     * discipline (echo + backspace handled there), so this is a live shell. */
    signal(SIGINT, on_sigint);           /* Ctrl-C -> fresh prompt instead of dying */
    signal(SIGTSTP, SIG_IGN);            /* Ctrl-Z at the prompt is a no-op (only jobs stop) */
    printf("NyxOS sh v0.13 — history/edit, Tab, globs (*?), ~, $(cmd), $VAR, NAME=val, quotes, jobs/fg/bg, '|', '&', '&&', '||', ';', 'exit'\n");
    for (;;) {
        reap_jobs();                     /* report finished background jobs */
        char line[128];
        int n = readline("sh$ ", line, sizeof(line));
        if (n < 0) continue;             /* interrupted (Ctrl-C -> SIGINT): fresh prompt */
        char* t = trim(line);
        if (!*t) continue;
        /* run_command_list splits on ; && || and expands $VARs per segment (so $?
         * reflects the previous segment); it returns 1 when an `exit` builtin runs. */
        if (run_command_list(t)) break;
    }
    printf("sh: bye\n");
    return 0;
}
