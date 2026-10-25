#include "../core/kernel.h"
#include "auth.h"
#include "../fs/ext2.h"
#include "../crypto/sha256.h"
#include "../drivers/misc/rtc.h"

#define XENC_KEY     "NyxOS_AUTH_v5.3"
#define XENC_KEY_LEN 15
#define MAX_FALLBACK_USERS 8
#define AUTH_MIN_PASS 4        /* minimum password length enforced by useradd */
#define SALT_HEX_LEN 16

/* ------------------------------------------------------------------ */
/*  PBKDF2-HMAC-SHA256 wrapper                                        */
/* ------------------------------------------------------------------ */
static void hash_password(const char* password, const char* salt_hex,
                          uint32_t iterations, uint8_t out[SHA256_DIGEST_SIZE]) {
    uint8_t salt[8];
    for (int i = 0; i < 8; i++) {
        char c = salt_hex[i * 2];
        uint8_t nib = (c >= '0' && c <= '9') ? (c - '0') :
                      (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10);
        salt[i] = nib << 4;
        c = salt_hex[i * 2 + 1];
        nib = (c >= '0' && c <= '9') ? (c - '0') :
              (c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10);
        salt[i] |= nib;
    }
    pbkdf2_hmac_sha256((const uint8_t*)password, strlen(password),
                       salt, 8, iterations, out);
}

/* Mix entropy from RDTSC (the CPU cycle counter — its low bits are essentially
 * unpredictable), the 1000 Hz timer tick, and the RTC clock into a splitmix64
 * stream, and emit a random 8-byte salt as hex. This replaces a salt that used
 * to be derived DETERMINISTICALLY from the username with a compile-time secret:
 * that made identical username/password pairs hash identically on every NyxOS
 * install (precomputable / rainbow-table-able). A random salt stored per
 * /etc/passwd entry — which the verifier already reads back — is the standard
 * fix, so identical passwords now yield different hashes. */
static uint64_t salt_rng = 0x243F6A8885A308D3ULL;   /* pi fractional bits */

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t salt_next(void) {
    uint64_t z = (salt_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void gen_random_salt(char salt_hex[SALT_HEX_LEN + 1]) {
    extern volatile uint32_t tick_count;
    salt_rng ^= rdtsc64();
    salt_rng ^= (uint64_t)tick_count << 20;
    salt_rng ^= (uint64_t)rtc_read_register(RTC_SECONDS) << 8;
    salt_rng ^= (uint64_t)rtc_read_register(RTC_MINUTES) << 40;
    salt_rng ^= rdtsc64() << 1;            /* a second sample adds timing jitter */
    static const char hexdig[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(salt_next() >> 24);
        salt_hex[i * 2]     = hexdig[(b >> 4) & 0xF];
        salt_hex[i * 2 + 1] = hexdig[b & 0xF];
    }
    salt_hex[SALT_HEX_LEN] = '\0';
}

/* ------------------------------------------------------------------ */
/*  XOR obfuscation for passwd file on disk                           */
/* ------------------------------------------------------------------ */
static void xor_buf(char* buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        buf[i] ^= XENC_KEY[i % XENC_KEY_LEN];
}

/* ------------------------------------------------------------------ */
/*  Password file entry format:  user:salt_hex:iterations:hex_hash:avatar\n
 *  The trailing avatar id (0..AVATAR_COUNT-1, a profile picture chosen at
 *  sign-up) is a 5th field appended after the hash; parse_entry stops at the
 *  hash and ignores it, so old 4-field entries still verify unchanged.        */
/* ------------------------------------------------------------------ */
static void format_entry(char* buf, uint32_t bufsz,
                         const char* user, const char* salt_hex,
                         uint32_t iterations, const uint8_t hash[SHA256_DIGEST_SIZE],
                         uint32_t avatar) {
    char hex[SHA256_DIGEST_SIZE * 2 + 1];
    sha256_to_hex(hash, hex);
    snprintf(buf, bufsz, "%s:%s:%u:%s:%u\n", user, salt_hex, iterations, hex, avatar);
}

static int parse_entry(const char* buf,
                       char* user, uint32_t user_sz,
                       char* salt_hex, uint32_t salt_sz,
                       uint32_t* iterations,
                       uint8_t hash[SHA256_DIGEST_SIZE]) {
    const char* p = buf;
    int i = 0;
    while (*p && *p != ':' && i < (int)user_sz - 1) user[i++] = *p++;
    if (*p != ':') return -1;
    user[i] = '\0'; p++;
    i = 0;
    while (*p && *p != ':' && i < (int)salt_sz - 1) salt_hex[i++] = *p++;
    if (*p != ':') return -1;
    salt_hex[i] = '\0'; p++;
    if (i != SALT_HEX_LEN) return -1;
    *iterations = 0;
    while (*p >= '0' && *p <= '9') { *iterations = *iterations * 10 + (*p - '0'); p++; }
    if (*p != ':') return -1;
    p++;
    for (int j = 0; j < SHA256_DIGEST_SIZE; j++) {
        uint8_t hi = 0, lo = 0;
        char c = *p++;
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;
        c = *p++;
        if (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;
        hash[j] = (hi << 4) | lo;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Fallback: in-memory user table when no EXT2                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char username[AUTH_MAX_USER];
    char salt_hex[SALT_HEX_LEN + 1];
    uint32_t iterations;
    uint8_t hash[SHA256_DIGEST_SIZE];
    int avatar;
} fallback_user_t;

static fallback_user_t fb_users[MAX_FALLBACK_USERS];
static int fb_count = 0;

static void add_fallback(const char* user, const char* pass, int avatar) {
    if (fb_count >= MAX_FALLBACK_USERS) return;
    fallback_user_t* u = &fb_users[fb_count++];
    strncpy(u->username, user, AUTH_MAX_USER - 1);
    u->username[AUTH_MAX_USER - 1] = '\0';
    gen_random_salt(u->salt_hex);
    u->iterations = PBKDF2_ITERATIONS;
    u->avatar = avatar;
    hash_password(pass, u->salt_hex, u->iterations, u->hash);
}

static int fallback_verify(const char* user, const char* pass) {
    for (int i = 0; i < fb_count; i++) {
        if (strcmp(fb_users[i].username, user) == 0) {
            uint8_t check[SHA256_DIGEST_SIZE];
            hash_password(pass, fb_users[i].salt_hex, fb_users[i].iterations, check);
            int ok = 1;
            for (int j = 0; j < SHA256_DIGEST_SIZE; j++)
                if (check[j] != fb_users[i].hash[j]) ok = 0;
            if (ok) return 1;
        }
    }
    return 0;
}

static int fallback_add(const char* user, const char* pass, int avatar) {
    if (fb_count >= MAX_FALLBACK_USERS) return -1;
    add_fallback(user, pass, avatar);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  EXT2 passwd file helpers (XOR-encrypted)                          */
/* ------------------------------------------------------------------ */
static int passwd_exists(void) {
    if (ext2_fs.block_size == 0) return 0;
    uint64_t fl = ext2_lock_acquire();     // shares the ext2 driver with /mnt ops
    int64_t ino = ext2_resolve(AUTH_PATH);
    int r = ino < 0 ? -1 : ino > 0;
    ext2_lock_release(fl);
    return r;
}

static int read_passwd(char* buf, uint32_t sz) {
    if (ext2_fs.block_size == 0) return -1;
    uint64_t fl = ext2_lock_acquire();
    int r = ext2_read_file(AUTH_PATH, buf, sz);
    ext2_lock_release(fl);                 // xor is local CPU work, no lock needed
    if (r > 0) xor_buf(buf, r);
    return r;
}

// The passwd buffers in write_passwd / auth_add_user / auth_set_avatar are `static` (in .bss)
// rather than on the stack: at 3-4 KB each they would overflow NyxOS's 4 KB kernel task stack,
// and auth_add_user/auth_set_avatar even nest a write_passwd call (~11 KB combined). Auth writes
// are serial in practice (login is modal; useradd/avatar changes are single-user), so sharing one
// .bss copy of each buffer is safe. See issue #68 (kernel stack-frame hardening).
static int write_passwd(const char* buf, uint32_t len) {
    if (ext2_fs.block_size == 0) return -1;
    static char tmp[4096];
    uint32_t cplen = len < sizeof(tmp) ? len : sizeof(tmp) - 1;
    memcpy(tmp, buf, cplen);
    xor_buf(tmp, cplen);
    // A freshly-mkfs'd ext2 disk has only lost+found, so ensure the parent dir
    // exists before creating the file. ext2_mkdir is a harmless no-op (returns
    // -1) if /etc already exists, and ext2_create_file no-ops if the file does.
    // The three ext2 ops run under one ext2_lock section so the passwd write is
    // atomic against a concurrent /mnt op on another core.
    uint64_t fl = ext2_lock_acquire();
    ext2_mkdir("/etc");
    ext2_create_file(AUTH_PATH);
    int r = ext2_write_file(AUTH_PATH, tmp, cplen);
    ext2_lock_release(fl);
    return r;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */
int auth_setup(void) {
    // No persistent disk at all → in-memory fallback users (do NOT persist).
    if (ext2_fs.block_size == 0) {
        add_fallback("nyx", "nyx", 0);
        add_fallback("root", "root", 1);
        add_fallback("admin", "admin", 2);
        printf("[AUTH] No EXT2 disk — %d in-memory fallback user(s) (PBKDF2-HMAC-SHA256, %u iterations)\n",
               fb_count, PBKDF2_ITERATIONS);
        return 0;
    }

    // Disk present with an existing user file → use it (persistent accounts).
    int exists = passwd_exists();
    if (exists < 0) {
        printf("[AUTH] Cannot read persistent account metadata\n");
        return -1;
    }
    if (exists) {
        printf("[AUTH] Persistent user file found at %s\n", AUTH_PATH);
        return 0;
    }

    // Disk present but no user file yet → create the default 'nyx' account and
    // write it to disk, so this account (and any added later with `useradd`)
    // survive reboots. This path used to be unreachable — the old guard treated
    // "no passwd file" the same as "no disk" and fell back without persisting.
    char salt_hex[SALT_HEX_LEN + 1];
    gen_random_salt(salt_hex);
    uint8_t hash[SHA256_DIGEST_SIZE];
    hash_password("nyx", salt_hex, PBKDF2_ITERATIONS, hash);
    char entry[128 + SHA256_DIGEST_SIZE * 2];
    format_entry(entry, sizeof(entry), "nyx", salt_hex, PBKDF2_ITERATIONS, hash, 0);
    if (write_passwd(entry, strlen(entry)) > 0) {
        printf("[AUTH] Created default (guest) user 'nyx' at %s (persistent, PBKDF2-HMAC-SHA256, %u iterations, XOR-encrypted)\n",
               AUTH_PATH, PBKDF2_ITERATIONS);
        return 0;
    }

    // Disk write failed → still allow login via an in-memory fallback account.
    add_fallback("nyx", "nyx", 0);
    printf("[AUTH] Could not write %s — using in-memory fallback user 'nyx'\n", AUTH_PATH);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Login brute-force lockout (rate-limiting)                         */
/* ------------------------------------------------------------------ */
/* After LOCKOUT_THRESHOLD consecutive failed logins the gate is closed for
 * LOCKOUT_COOLDOWN timer ticks: every attempt (even the correct password) is
 * refused until the window elapses, throttling an online password-guessing
 * attack to a few tries per cooldown. Any successful login clears the counter.
 * A single global gate (the login screen serves one user at a time); the
 * decision logic is pure + clock-injected so it is known-answer tested. */
#define LOCKOUT_THRESHOLD 5
#define LOCKOUT_COOLDOWN  1800u          /* ticks (~18 s at the 100 Hz PIT) */

typedef struct { int fails; uint32_t locked_until; } lockout_t;
static lockout_t g_lockout;

static int lockout_blocked(const lockout_t* L, uint32_t now) {
    return L->fails >= LOCKOUT_THRESHOLD && (int32_t)(L->locked_until - now) > 0;
}
static void lockout_record(lockout_t* L, int success, uint32_t now) {
    if (success) { L->fails = 0; L->locked_until = 0; return; }
    if (L->fails < 1000000) L->fails++;
    if (L->fails >= LOCKOUT_THRESHOLD) L->locked_until = now + LOCKOUT_COOLDOWN;
}

static int auth_verify_core(const char* username, const char* password) {
    if (!username || !password) return 0;

    int exists = passwd_exists();
    if (exists < 0) return 0;
    if (!exists)
        return fallback_verify(username, password);

    char buf[2048];
    int len = read_passwd(buf, sizeof(buf) - 1);
    if (len < 0) return 0;
    buf[len] = '\0';

    char line[300];
    int li = 0;
    for (int i = 0; i <= len; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            line[li] = '\0';
            if (li > 0) {
                char file_user[AUTH_MAX_USER];
                char file_salt[SALT_HEX_LEN + 1];
                uint32_t file_iter;
                uint8_t file_hash[SHA256_DIGEST_SIZE];
                if (parse_entry(line, file_user, sizeof(file_user),
                                file_salt, sizeof(file_salt),
                                &file_iter, file_hash) == 0) {
                    if (strcmp(file_user, username) == 0) {
                        uint8_t check[SHA256_DIGEST_SIZE];
                        hash_password(password, file_salt, file_iter, check);
                        int ok = 1;
                        for (int j = 0; j < SHA256_DIGEST_SIZE; j++)
                            if (check[j] != file_hash[j]) ok = 0;
                        if (ok) return 1;
                    }
                }
            }
            li = 0;
        } else {
            if (li < 299) line[li++] = buf[i];
        }
    }
    return fallback_verify(username, password);
}

/* Public entry: apply the brute-force lockout gate around the password check. */
int auth_verify(const char* username, const char* password) {
    uint32_t now = get_ticks();
    if (lockout_blocked(&g_lockout, now)) return 0;      /* locked out - refuse outright */
    int ok = auth_verify_core(username, password);
    lockout_record(&g_lockout, ok, now);
    return ok;
}

/* KAT: drive the lockout state machine with an injected clock (deterministic). */
int auth_lockout_selftest(void) {
    lockout_t L = {0, 0};
    uint32_t t = 1000;
    for (int i = 0; i < LOCKOUT_THRESHOLD - 1; i++) {
        lockout_record(&L, 0, t);
        if (lockout_blocked(&L, t)) return 1;                          /* no lock before threshold */
    }
    lockout_record(&L, 0, t);
    if (!lockout_blocked(&L, t)) return 2;                             /* threshold failure arms lock */
    if (!lockout_blocked(&L, t + LOCKOUT_COOLDOWN - 1)) return 3;      /* still locked in-window */
    if (lockout_blocked(&L, t + LOCKOUT_COOLDOWN + 1)) return 4;       /* unlocks after cooldown */
    lockout_record(&L, 1, t + LOCKOUT_COOLDOWN + 2);                   /* success resets the counter */
    if (L.fails != 0 || lockout_blocked(&L, t + LOCKOUT_COOLDOWN + 2)) return 5;
    for (int i = 0; i < LOCKOUT_THRESHOLD; i++) lockout_record(&L, 0, t + 10000);
    if (!lockout_blocked(&L, t + 10000)) return 6;                     /* re-locks after threshold */
    return 0;
}

void auth_add_user(const char* username, const char* password, int avatar) {
    if (!username || !password) return;
    if (avatar < 0 || avatar >= AVATAR_COUNT) avatar = 0;

    // Basic account policy: a non-empty username and a password of at least
    // AUTH_MIN_PASS characters (applies whether the account persists to disk or
    // lands in the in-memory fallback table).
    if (username[0] == '\0') { printf("[AUTH] username cannot be empty\n"); return; }
    if (strlen(password) < AUTH_MIN_PASS) {
        printf("[AUTH] password too short (minimum %d characters)\n", AUTH_MIN_PASS);
        return;
    }

    if (ext2_fs.block_size == 0 || !passwd_exists()) {
        if (fallback_add(username, password, avatar) == 0)
            printf("[AUTH] Added fallback user '%s' (PBKDF2-HMAC-SHA256, %d users)\n",
                   username, fb_count);
        return;
    }

    static char existing[3072];   // off-stack (see write_passwd note; #68)
    int exlen = read_passwd(existing, sizeof(existing) - 1);
    if (exlen < 0) {
        printf("[AUTH] Cannot read persistent accounts; user not added\n");
        return;
    }
    existing[exlen] = '\0';

    // Reject a duplicate username (a name at the start of a line, up to its ':').
    int ulen = strlen(username);
    for (int i = 0; i + ulen <= exlen; i++) {
        if ((i == 0 || existing[i - 1] == '\n') &&
            strncmp(existing + i, username, ulen) == 0 &&
            existing[i + ulen] == ':') {
            printf("[AUTH] User '%s' already exists\n", username);
            return;
        }
    }

    char salt_hex[SALT_HEX_LEN + 1];
    gen_random_salt(salt_hex);
    uint8_t hash[SHA256_DIGEST_SIZE];
    hash_password(password, salt_hex, PBKDF2_ITERATIONS, hash);

    char new_entry[128 + SHA256_DIGEST_SIZE * 2];
    format_entry(new_entry, sizeof(new_entry), username, salt_hex, PBKDF2_ITERATIONS, hash, avatar);

    static char combined[4096];   // off-stack (see write_passwd note; #68)
    snprintf(combined, sizeof(combined), "%s%s", existing, new_entry);
    if (write_passwd(combined, strlen(combined)) > 0)
        printf("[AUTH] Added user '%s' (persistent, PBKDF2-HMAC-SHA256, %u iterations)\n",
               username, PBKDF2_ITERATIONS);
    else
        printf("[AUTH] Failed to write %s — user '%s' not saved\n", AUTH_PATH, username);
}

// Print the account names, one per line: from the persistent /etc/passwd if a
// disk is present, else from the in-memory fallback table.
void auth_list_users(void) {
    if (ext2_fs.block_size == 0 || !passwd_exists()) {
        for (int i = 0; i < fb_count; i++) printf("%s\n", fb_users[i].username);
        return;
    }
    char buf[2048];
    int len = read_passwd(buf, sizeof(buf) - 1);
    if (len < 0) return;
    buf[len] = '\0';
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            if (i > start) {
                char name[AUTH_MAX_USER];
                int k = 0;
                for (int j = start; j < i && buf[j] != ':' && k < AUTH_MAX_USER - 1; j++)
                    name[k++] = buf[j];
                name[k] = '\0';
                if (name[0]) printf("%s\n", name);
            }
            start = i + 1;
        }
    }
}

// Return the profile-picture id (0..AVATAR_COUNT-1) chosen by `username`, read
// from the 5th field of its /etc/passwd entry (or the fallback table). Returns 0
// (the default avatar) for an unknown user or an old entry without the field.
int auth_get_avatar(const char* username) {
    if (!username) return 0;
    if (ext2_fs.block_size == 0 || !passwd_exists()) {
        for (int i = 0; i < fb_count; i++)
            if (strcmp(fb_users[i].username, username) == 0) return fb_users[i].avatar;
        return 0;
    }
    char buf[2048];
    int len = read_passwd(buf, sizeof(buf) - 1);
    if (len < 0) return 0;
    buf[len] = '\0';
    int ulen = strlen(username);
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            if (i - start > ulen && strncmp(buf + start, username, ulen) == 0 &&
                buf[start + ulen] == ':') {
                // Skip the 4 ':' separators (user, salt, iter, hash) to the avatar.
                int col = 0, j = start;
                while (j < i && col < 4) { if (buf[j] == ':') col++; j++; }
                int av = 0;
                if (col == 4) while (j < i && buf[j] >= '0' && buf[j] <= '9') av = av * 10 + (buf[j++] - '0');
                if (av < 0 || av >= AVATAR_COUNT) av = 0;
                return av;
            }
            start = i + 1;
        }
    }
    return 0;
}

// Change the stored profile picture for `username` (used by the desktop user
// menu). Rewrites only the avatar field of that user's /etc/passwd entry, keeping
// the salt + hash; updates the in-memory fallback table when there's no disk.
void auth_set_avatar(const char* username, int avatar) {
    if (!username) return;
    if (avatar < 0 || avatar >= AVATAR_COUNT) avatar = 0;

    if (ext2_fs.block_size == 0 || !passwd_exists()) {
        for (int i = 0; i < fb_count; i++)
            if (strcmp(fb_users[i].username, username) == 0) fb_users[i].avatar = avatar;
        return;
    }

    static char buf[3072];        // off-stack (see write_passwd note; #68)
    int len = read_passwd(buf, sizeof(buf) - 1);
    if (len < 0) return;
    buf[len] = '\0';

    static char out[3200];        // off-stack (see write_passwd note; #68)
    int oi = 0, ulen = strlen(username), start = 0;
    for (int i = 0; i <= len; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            if (i > start) {
                int is_user = (i - start > ulen &&
                               strncmp(buf + start, username, ulen) == 0 &&
                               buf[start + ulen] == ':');
                if (is_user) {
                    // Copy the first 4 fields (up to and incl. the 4th ':'), then
                    // write the new avatar digit, dropping any old 5th field.
                    int col = 0, j = start;
                    while (j < i && col < 4) { if (buf[j] == ':') col++; j++; }
                    for (int k = start; k < j && oi < (int)sizeof(out) - 8; k++) out[oi++] = buf[k];
                    if (col < 4 && oi < (int)sizeof(out) - 8) out[oi++] = ':';   // old 4-field entry
                    if (oi < (int)sizeof(out) - 2) out[oi++] = (char)('0' + avatar);  // avatar < 10
                } else {
                    for (int k = start; k < i && oi < (int)sizeof(out) - 2; k++) out[oi++] = buf[k];
                }
                if (oi < (int)sizeof(out) - 1) out[oi++] = '\n';
            }
            start = i + 1;
        }
    }
    out[oi] = '\0';
    write_passwd(out, oi);
}
