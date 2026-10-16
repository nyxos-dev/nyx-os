#include "../core/kernel.h"
#include "ext2.h"
#include "../core/spinlock.h"

#define MAX_INODES    512   // total node pool (256->512: headroom for sustained in-OS
                            // compile sessions — mitigation for the #66 exhaustion. Was
                            // 128, then 256; root alone holds ~58. See vfs_pool_report.)
#define MAX_NAME      64
#define MAX_CHILDREN  128   // per-directory child cap (was 64 — root `/` needs headroom)
#define VFS_SYMLINK   2     // vfs_node_t.type: 0=file, 1=dir, 2=symlink (target string in ->data)
#define SYMLINK_MAX   8     // max symlink hops before giving up (loop / too-deep guard)
#define BLOCK_SIZE    512

// Mount table
static mount_entry_t mount_table[MAX_MOUNT_POINTS];
static int mount_count = 0;

// SMP: mount_table is append-only — vfs_mount only ever fills the next free slot
// and bumps mount_count; nothing removes or rewrites an entry (there is no
// vfs_unmount), so a mount_entry_t* returned by vfs_find_mount stays valid for the
// life of the system. This lock serializes the writer against two hazards once
// smpbalance spreads work across cores: a second concurrent vfs_mount clobbering
// the same slot, and a reader iterating [0, mount_count) while the count is bumped
// past a half-filled entry. The pointer handed back is then used UNLOCKED by
// callers — safe precisely because of the append-only + immutable invariant above.
// Leaf lock: vfs_mount releases it before ensure_mount_stub (which takes
// node_pool_lock), so the two VFS locks are never nested. (T1 track, v5.9.22.)
static spinlock_t mount_table_lock = SPINLOCK_INIT;

#define VFS_MOUNT_DIRENTS 64   // max entries loaded per mounted directory open

typedef struct vfs_node {
    char name[MAX_NAME];
    uint32_t type;         // 0=file, 1=dir
    uint32_t size;
    uint8_t* data;
    uint32_t node_id;
    struct vfs_node* parent;
    struct vfs_node* children[MAX_CHILDREN];
    uint32_t child_count;
    uint32_t readdir_idx;  // per-directory readdir cursor
    uint8_t  mount_backed; // 1 = transient node mirroring a mounted-FS file
    /* How many live fds point at this node. A VFS "fd" IS this pointer (see
     * vfs_open), so the pool must not recycle a node while anyone still holds
     * one — the next alloc_node() would hand the same memory to a different
     * file. This was tracked only for mount mirrors aliased by dup(); /proc
     * nodes and unlinked ramdisk files were recycled or wiped out from under
     * their holders. One count, every node kind: 1 after open, +1 per dup,
     * -1 per close, released at zero. */
    uint32_t open_refs;
    uint8_t  orphaned;     // unlinked from the tree while still open; free at last close
    uint8_t  on_free_list; // already queued in free_nodes[] — double-free guard
    uint8_t  dirty;        // mount file has unflushed writes; flushed on last close (v6.4.10).
                           // Was flush-on-every-write, which re-wrote the WHOLE file to ext2
                           // per 4 KB write() — O(n^2) I/O that made a ~496 KB write (the in-OS
                           // tcc self-host object) take minutes. Now writes just mark dirty.
    char     mpath[MAX_NAME]; // path within the mount (e.g. "/foo.txt")
    void*    mount_ent;    // mount_entry_t* to flush writes through
    uint32_t dev_type;    // 0 = regular file; else a /dev special (DEV_* below)
    uint32_t proc_type;   // 0 = not /proc; else a PROC_* generated node (below)
    uint32_t proc_pid;    // for PROC_PID_* nodes: which process this reflects
    uint16_t mode;        // rwx permission bits (chmod); default 0644 file / 0755 dir. A
                          // cleared owner-write bit (0200) makes vfs_write_file refuse (v6.4.352)
} vfs_node_t;

/* Special device nodes under /dev. read/write of these bypass ino->data. */
#define DEV_NULL    1     // reads -> EOF, writes discarded
#define DEV_ZERO    2     // reads -> endless zero bytes, writes discarded
#define DEV_RANDOM  3     // reads -> pseudo-random bytes, writes discarded

/* Generated /proc nodes. Their content is synthesized on read (vfs_pread) from
 * live kernel state — nothing is stored in ino->data. The per-pid dirs are
 * created/removed on the fly by proc_sync() to track the process table. */
#define PROC_MEMINFO     1   // MemTotal/MemUsed/MemFree
#define PROC_UPTIME      2   // seconds since boot
#define PROC_VERSION     3   // kernel version banner
#define PROC_CPUINFO     4   // arch / cpu summary
#define PROC_PID_DIR     5   // /proc/<pid> directory (proc_pid set)
#define PROC_PID_STATUS  6   // /proc/<pid>/status  (proc_pid set)
#define PROC_PID_CMDLINE 7   // /proc/<pid>/cmdline (proc_pid set)
#define PROC_PID_MAPS    8   // /proc/<pid>/maps    (mapped memory regions)
#define PROC_MOUNTS      9   // /proc/mounts        (mounted filesystems)
#define PROC_STAT        10  // /proc/stat          (ctxt / processes / procs_running / btime)
#define PROC_CMDLINE     11  // /proc/cmdline       (retained multiboot boot command line)
#define PROC_FILESYSTEMS 12  // /proc/filesystems   (the fs types the kernel supports)

/* /dev/random and /dev/urandom are served by the kernel CSPRNG (an HMAC-DRBG seeded from
 * RDSEED/RDRAND hardware entropy) — the same source uuid/mktemp/shuf/encrypt already use.
 * This replaced a tick-seeded xorshift64 PRNG: xorshift output is predictable (its 64-bit
 * state can be recovered from a handful of samples, then every future byte is known) and a
 * tick seed carries little entropy — both unacceptable for a device programs read to make
 * keys, nonces, and tokens. csprng_bytes lazily seeds itself, so it is safe to call here. */
extern void csprng_bytes(uint8_t* out, uint32_t n);

static vfs_node_t nodes[MAX_INODES];
static uint32_t node_count = 0;
static vfs_node_t* current_dir = NULL;

// Free list so transient mount-backed nodes (allocated per open, freed on close)
// don't exhaust the fixed pool. Ramdisk tree nodes are never freed.
static vfs_node_t* free_nodes[MAX_INODES];
static int free_node_count = 0;

// The node pool + free-list are shared across cores now: with smpbalance on, a
// process on any CPU can open/close a file and reach alloc_node/free_node. This is
// a real lock, not the old preempt_disable() — that stops a context switch on THIS
// core but means nothing to another core, which could read the same free_node_count
// and hand the same node to two files (the exact race free_node's comment warns of).
// It's a leaf lock (no other lock or allocator is taken while it's held), and
// irqsave also covers the single-core reentrancy preempt_disable used to: cli
// blocks the preempting timer tick. (T1 SMP-locking track, v5.9.21.)
static spinlock_t node_pool_lock = SPINLOCK_INIT;

static vfs_node_t* alloc_node(void) {
    uint64_t fl = spin_lock_irqsave(&node_pool_lock);
    vfs_node_t* node = NULL;
    if (free_node_count > 0) {
        node = free_nodes[--free_node_count];
    } else if (node_count < MAX_INODES) {
        node = &nodes[node_count++];
    }
    if (node) {
        memset_asm(node, 0, sizeof(vfs_node_t));
        node->node_id = (uint32_t)(node - nodes);
        node->mode = 0644;   // default: owner-writable, world-readable (so writes work unless chmod'd)
    }
    spin_unlock_irqrestore(&node_pool_lock, fl);
    return node;
}

// A VFS fd handed to ring 3 (and to the ufd layer in syscall.c) is the node's
// 1-based index in the static nodes[] pool, NOT the node pointer. Returning the
// pointer as an int truncated it to 32 bits, so any node placed above 4 GiB
// produced a corrupt handle and a wild pointer on the next read/write (issue
// #55). An index is small (<= MAX_INODES), stable for the node's whole life, and
// 64-bit-safe. handle_to_node maps back; a stale/out-of-range handle yields NULL,
// which every consumer already treats as a bad fd (same as the old NULL guard).
static inline int node_to_handle(vfs_node_t* n) {
    return n ? (int)(n - nodes) + 1 : -1;
}
static inline vfs_node_t* handle_to_node(int fd) {
    if (fd < 1 || fd > MAX_INODES) return NULL;
    return &nodes[fd - 1];
}

/* Queue a node for reuse. The `on_free_list` check is not defensive padding:
 * pushing the same node twice puts it in free_nodes[] twice, and the next two
 * alloc_node() calls then hand ONE node to two different files. Several paths
 * could reach this twice for the same node (a close racing proc_sync, an unlink
 * of a still-open file), so the guard belongs here rather than at each caller. */
static void free_node(vfs_node_t* n) {
    uint64_t fl = spin_lock_irqsave(&node_pool_lock);
    if (n && !n->on_free_list && free_node_count < MAX_INODES) {
        n->on_free_list = 1;
        free_nodes[free_node_count++] = n;
    }
    spin_unlock_irqrestore(&node_pool_lock, fl);
}

/* Release a node's storage and return it to the pool — but only once nobody
 * holds an fd onto it. While it is still open we merely mark it orphaned: it is
 * already out of the directory tree, so it is invisible to lookups, and the last
 * vfs_close finishes the job. That is the unlink-while-open behaviour callers
 * already assume, and it is what stops the pool recycling live handles. */
static void release_node(vfs_node_t* n) {
    if (!n) return;
    if (n->open_refs > 0) { n->orphaned = 1; return; }
    if (n->data) { kfree(n->data); n->data = NULL; }
    n->size = 0;
    n->child_count = 0;
    free_node(n);
}

// Read-only census of the node pool, for diagnosing #66-style exhaustion (`vfsstat`).
// Splits the transient mount-backed (ext2 mirror) nodes into those still held by an
// open fd (open_refs>0) versus idle-but-not-freed (open_refs==0, off the free list) —
// which distinguishes an fd leak (held keeps climbing) from a free-list drop (idle).
void vfs_pool_report(uint32_t* total, uint32_t* high_water, uint32_t* freelist,
                     uint32_t* mount_held, uint32_t* mount_idle) {
    uint64_t fl = spin_lock_irqsave(&node_pool_lock);
    if (total)      *total = MAX_INODES;
    if (high_water) *high_water = node_count;
    if (freelist)   *freelist = (uint32_t)free_node_count;
    uint32_t held = 0, idle = 0;
    for (uint32_t i = 0; i < node_count; i++) {
        if (!nodes[i].mount_backed) continue;
        if (nodes[i].open_refs > 0)      held++;
        else if (!nodes[i].on_free_list) idle++;
    }
    if (mount_held) *mount_held = held;
    if (mount_idle) *mount_idle = idle;
    spin_unlock_irqrestore(&node_pool_lock, fl);
}

// Which census bucket a live node falls in. Pure (reads only the node's own tag
// fields, no pool state) so a KAT can pin the rule. The order matters: an ext2
// mirror is never a ramdisk/proc leak, so mount_backed wins; a /proc or /dev node
// is tagged even though it is also type dir/file. 0=mount 1=proc 2=dev 3=ramdir
// 4=ramfile — mirrors the labels vfsstat prints.
static int node_census_kind(const vfs_node_t* n) {
    if (n->mount_backed) return 0;
    if (n->proc_type)    return 1;
    if (n->dev_type)     return 2;
    return (n->type == 1) ? 3 : 4;
}

// Break the LIVE non-mount nodes down by kind, for #66. vfs_pool_report could only
// report "ramdisk + /proc" as a single lump (live minus the mount-backed count), so
// which kind actually grows across a compile session was an inference across 15
// census comments, never a measurement. This counts each category directly, so the
// leaking bucket is visible. Read-only; same lock/pass shape as vfs_pool_report.
// Invariant: live == (mount held+idle) + proc + dev + ram_dir + ram_file.
void vfs_pool_kinds(uint32_t* proc_n, uint32_t* dev_n,
                    uint32_t* ram_file, uint32_t* ram_dir) {
    uint32_t pc = 0, dv = 0, rf = 0, rd = 0;
    uint64_t fl = spin_lock_irqsave(&node_pool_lock);
    for (uint32_t i = 0; i < node_count; i++) {
        vfs_node_t* n = &nodes[i];
        if (n->on_free_list) continue;              // freed slot: not live, tags are stale
        switch (node_census_kind(n)) {
            case 1: pc++; break;
            case 2: dv++; break;
            case 3: rd++; break;
            case 4: rf++; break;
            default: break;                          // 0 = mount-backed, counted by vfs_pool_report
        }
    }
    spin_unlock_irqrestore(&node_pool_lock, fl);
    if (proc_n)   *proc_n   = pc;
    if (dev_n)    *dev_n    = dv;
    if (ram_file) *ram_file = rf;
    if (ram_dir)  *ram_dir  = rd;
}

// KAT for the #66 census classifier: each tag combination must land in the right
// bucket, with the documented priority (mount > proc > dev > dir/file). Returns 0
// on pass, else the failing case number.
int vfs_poolkind_selftest(void) {
    vfs_node_t n;
    // mount-backed wins over every other tag (an ext2 mirror is never a leak here)
    memset_asm(&n, 0, sizeof n); n.mount_backed = 1; n.proc_type = PROC_PID_DIR; n.type = 1;
    if (node_census_kind(&n) != 0) return 1;
    // /proc generated node
    memset_asm(&n, 0, sizeof n); n.proc_type = PROC_MEMINFO;
    if (node_census_kind(&n) != 1) return 2;
    // /dev special node
    memset_asm(&n, 0, sizeof n); n.dev_type = DEV_NULL;
    if (node_census_kind(&n) != 2) return 3;
    // plain ramdisk directory
    memset_asm(&n, 0, sizeof n); n.type = 1;
    if (node_census_kind(&n) != 3) return 4;
    // plain ramdisk file
    memset_asm(&n, 0, sizeof n); n.type = 0;
    if (node_census_kind(&n) != 4) return 5;
    // proc is checked before dev: a node tagged both counts as proc
    memset_asm(&n, 0, sizeof n); n.proc_type = PROC_UPTIME; n.dev_type = DEV_ZERO;
    if (node_census_kind(&n) != 1) return 6;
    return 0;
}

// Append `child` to directory `parent`; returns 0, or -1 if `parent` already holds
// MAX_CHILDREN entries. EVERY insert into a node's fixed children[] array must go
// through here (or an equivalent `child_count >= MAX_CHILDREN` guard) — a raw
// `children[child_count++] = x` with no bound check writes past the array and
// corrupts the adjacent inode in the nodes[] pool. Callers that alloc_node()'d the
// child first should free_node() it on failure.
static int vfs_append_child(vfs_node_t* parent, vfs_node_t* child) {
    if (!parent || parent->child_count >= MAX_CHILDREN) return -1;
    parent->children[parent->child_count++] = child;
    return 0;
}

static vfs_node_t* find_child(vfs_node_t* dir, const char* name) {
    for (uint32_t i = 0; i < dir->child_count; i++) {
        if (strcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    }
    return NULL;
}

static char* path_token(char* path, char* token) {
    if (!path || !*path) return NULL;
    while (*path == '/') path++;
    if (!*path) return NULL;
    int i = 0;
    while (*path && *path != '/' && i < MAX_NAME-1) {
        token[i++] = *path++;
    }
    token[i] = '\0';
    return path;
}

static vfs_node_t* resolve_path_d(const char* path, int depth);

// Follow a symlink node to its target node, bounded by `depth` hops (loop/too-deep guard).
// A non-symlink passes straight through. The target string in ->data resolves as a path
// (absolute from root, else relative to the cwd). Additive: only ever called on type==2
// nodes, so ordinary file/dir resolution is byte-for-byte unchanged.
static vfs_node_t* deref_link(vfs_node_t* n, int depth) {
    while (n && n->type == VFS_SYMLINK) {
        if (depth-- <= 0 || !n->data) return NULL;
        n = resolve_path_d((const char*)n->data, depth);
    }
    return n;
}

static vfs_node_t* resolve_path_d(const char* path, int depth) {
    if (!path || !*path) return current_dir;
    char buf[256];
    strncpy(buf, path, 255);
    buf[255] = '\0';
    char* p = buf;

    vfs_node_t* dir;
    if (buf[0] == '/') {
        dir = &nodes[0];
        p++;
    } else {
        dir = current_dir;
    }

    char token[MAX_NAME];
    vfs_node_t* result = dir;
    // Walk directory components until the last one
    while (1) {
        char* next = path_token(p, token);
        if (!next || !*token) break;
        // Check if there are more components (i.e., this is a directory)
        if (*next) {
            result = find_child(dir, token);
            if (result && result->type == VFS_SYMLINK) result = deref_link(result, depth);  // follow intermediate link
            if (!result || result->type != 1) return NULL;
            dir = result;
            p = next;
        } else {
            // Last component - can be file, directory, or a symlink to follow
            result = find_child(dir, token);
            if (result && result->type == VFS_SYMLINK) result = deref_link(result, depth);
            return result;
        }
    }
    return result;
}

static vfs_node_t* resolve_path(const char* path) { return resolve_path_d(path, SYMLINK_MAX); }

// Resolve `path` to the directory that should CONTAIN its final component, and copy
// that final component into `child_name`. Every INTERMEDIATE component must already
// exist and be a directory: if one is missing (or is a file), the requested parent
// genuinely does not exist, so this fails with NULL. The old code returned early with
// whatever directory it had reached the moment it hit a missing component, so a
// create/unlink of "/a/b/c" whose "/a/b" was absent silently operated on "/a" instead
// of failing (issue #56). The final component is handed back unresolved — it may or
// may not exist yet, which is the caller's decision (create vs. remove vs. rename).
static vfs_node_t* resolve_parent(const char* path, char* child_name) {
    if (!path || !*path) return NULL;
    char buf[256];
    strncpy(buf, path, 255);
    buf[255] = '\0';
    char* p = buf;
    if (buf[0] == '/') p++;

    vfs_node_t* dir = (buf[0] == '/') ? &nodes[0] : current_dir;

    char token[MAX_NAME];
    char* next = path_token(p, token);
    if (!next || !*token) return NULL;              // no component at all (e.g. "/")
    for (;;) {
        // Peek at what follows `token`. If nothing real remains, `token` is the final
        // component and `dir` is its parent — done.
        char peek[MAX_NAME];
        char* after = path_token(next, peek);
        if (!after || !*peek) {
            if (child_name) strncpy(child_name, token, MAX_NAME-1);
            return dir;
        }
        // `token` is an intermediate: it MUST already resolve to a directory, else the
        // parent the caller asked for does not exist.
        vfs_node_t* child = find_child(dir, token);
        if (!child || child->type != 1) return NULL;
        dir = child;
        next = after;
        strncpy(token, peek, MAX_NAME-1);
        token[MAX_NAME-1] = '\0';
    }
}

// Create a symbolic link `linkpath` pointing at `target` (an arbitrary path string, stored
// verbatim in the node's ->data and followed by resolve_path). Returns 0, or -1 if the parent
// is missing/not-a-dir, the name already exists, the dir is full, or allocation fails.
int vfs_symlink(const char* linkpath, const char* target) {
    if (!linkpath || !target) return -1;
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(linkpath, child_name);
    if (!parent || parent->type != 1) return -1;
    if (find_child(parent, child_name)) return -1;             // name in use
    if (parent->child_count >= MAX_CHILDREN) return -1;
    int tlen = (int)strlen(target);
    vfs_node_t* n = alloc_node();
    if (!n) return -1;
    n->data = (uint8_t*)kmalloc((uint32_t)tlen + 1);
    if (!n->data) { free_node(n); return -1; }
    memcpy(n->data, target, (uint32_t)tlen + 1);
    n->type = VFS_SYMLINK;
    n->size = (uint32_t)tlen;
    strncpy(n->name, child_name, MAX_NAME - 1);
    n->name[MAX_NAME - 1] = '\0';
    n->parent = parent;
    parent->children[parent->child_count++] = n;
    return 0;
}

// Read a symlink's target into `buf` (NUL-terminated, up to `cap`) WITHOUT following it.
// Returns the target length, or -1 if `path` is not a symlink.
int vfs_readlink(const char* path, char* buf, int cap) {
    if (!buf || cap <= 0) return -1;
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(path, child_name);
    if (!parent) return -1;
    vfs_node_t* n = find_child(parent, child_name);
    if (!n || n->type != VFS_SYMLINK || !n->data) return -1;
    int len = (int)strlen((const char*)n->data);
    if (len >= cap) len = cap - 1;
    memcpy(buf, n->data, (uint32_t)len);
    buf[len] = '\0';
    return len;
}

// ==================== /proc (generated filesystem) ====================
// /proc exposes live kernel state as readable text, synthesized on demand — the
// static files (meminfo/uptime/version/cpuinfo) are created once at boot, while
// the per-process /proc/<pid> dirs (+ status/cmdline) are reconciled with the
// process table by proc_sync() at the VFS entry points ring 3 uses. No storage:
// content is generated in vfs_pread, so nothing here occupies ino->data.

static vfs_node_t* proc_node = NULL;   // the /proc directory node (cached at boot)

// Create a child node of `parent` tagged as a /proc node. Returns it, or NULL if
// the pool or the parent's child array is full (a partial /proc is acceptable).
static vfs_node_t* proc_make(vfs_node_t* parent, const char* name, uint32_t type,
                             uint32_t ptype, uint32_t pid) {
    if (!parent || parent->child_count >= MAX_CHILDREN) return NULL;
    vfs_node_t* n = alloc_node();
    if (!n) return NULL;
    strncpy(n->name, name, MAX_NAME - 1);
    n->type = type;
    n->proc_type = ptype;
    n->proc_pid = pid;
    n->parent = parent;
    parent->children[parent->child_count++] = n;
    return n;
}

// Unsigned base-10 into buf (NUL-terminated); returns the digit count.
static int proc_utoa(uint32_t v, char* buf) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

// Synthesize a /proc node's full text into buf; returns the byte length.
static int proc_generate(vfs_node_t* ino, char* buf, int bufsz) {
    buf[0] = '\0';
    switch (ino->proc_type) {
        case PROC_MEMINFO: {
            extern void mem_pool_kb(uint32_t*, uint32_t*, uint32_t*);
            uint32_t used, freeb, tot;                 // managed pool — one honest source (see mem_pool_kb)
            mem_pool_kb(&used, &freeb, &tot);
            snprintf(buf, bufsz,
                     "MemTotal: %u kB\nMemUsed:  %u kB\nMemFree:  %u kB\nMemDetected: %llu kB\n",
                     tot, used, freeb, memory_total / 1024);
            break;
        }
        case PROC_UPTIME: {
            extern uint32_t get_uptime_seconds(void);   // wall-clock (RTC), not tick_count
            snprintf(buf, bufsz, "%u.00\n", get_uptime_seconds());
            break;
        }
        case PROC_VERSION:
            snprintf(buf, bufsz, "NyxOS version %s (x86_64)\n", KERNEL_VERSION);
            break;
        case PROC_CMDLINE: {
            extern char g_boot_cmdline[];               // the boot command line the kernel retained
            snprintf(buf, bufsz, "%s\n", g_boot_cmdline);
            break;
        }
        case PROC_FILESYSTEMS:
            // The fs types the kernel actually supports, Linux /proc/filesystems layout:
            // "nodev\t<name>" for a pseudo/in-memory fs, "\t<name>" for a block-backed one.
            // NyxOS = nyxfs (the in-memory rootfs, no device) + ext2 (the only mountable type).
            snprintf(buf, bufsz, "nodev\tnyxfs\n\text2\n");
            break;
        case PROC_CPUINFO:
            snprintf(buf, bufsz, "arch\t: x86_64\nvendor\t: NyxOS\n");
            break;
        case PROC_STAT: {
            // Aggregate scheduler/system counters, Linux /proc/stat layout (the fields we
            // can report HONESTLY): ctxt = context switches since boot (the new sched_target
            // counter), processes = procs ever created (next_pid-1), procs_running = live
            // RUN-state tasks right now, btime = boot wall-clock (RTC now minus uptime).
            extern volatile uint64_t g_ctx_switches;
            extern uint64_t total_procs_created(void);
            extern int64_t rtc_epoch_now(void);
            extern uint32_t get_uptime_seconds(void);
            extern process_t* process_table[MAX_PROCESSES];
            extern int process_count;
            uint32_t running = 0;
            for (int i = 0; i < process_count; i++)
                if (process_table[i] && process_table[i]->state == PROC_RUN) running++;
            int64_t btime = rtc_epoch_now() - (int64_t)get_uptime_seconds();
            if (btime < 0) btime = 0;
            snprintf(buf, bufsz,
                     "ctxt %llu\nprocesses %llu\nprocs_running %u\nbtime %u\n",
                     (unsigned long long)g_ctx_switches,
                     (unsigned long long)total_procs_created(),
                     running, (uint32_t)btime);
            break;
        }
        case PROC_PID_STATUS: {
            process_t* p = find_process(ino->proc_pid);
            if (p) {
                const char* st = p->state == 1 ? "R (running)" :
                                 p->state == 2 ? "Z (zombie)"  :
                                 p->state == 3 ? "S (sleeping)" : "P (parked)";
                snprintf(buf, bufsz, "Name:\t%s\nPid:\t%u\nPPid:\t%u\nState:\t%s\n",
                         p->comm, p->pid, p->ppid, st);
            }
            break;
        }
        case PROC_PID_CMDLINE: {
            process_t* p = find_process(ino->proc_pid);
            if (p) snprintf(buf, bufsz, "%s\n", p->cmdline[0] ? p->cmdline : p->comm);
            break;
        }
        case PROC_PID_MAPS: {
            // Mapped memory regions of the process — the address ranges present in
            // its page tables, coalesced by permission, labelled by VA (program /
            // shared libc / dlopen'd lib / mmap / stack). Shows what dlopen mapped.
            process_t* p = find_process(ino->proc_pid);
            if (!p || !p->page_directory) break;
            static vm_region_t regs[24];      // static: this runs preempt-safe under vfs_pread
            int nr = vm_collect_regions((uint64_t*)p->page_directory, regs, 24);
            int off = 0;
            for (int r = 0; r < nr && off < bufsz - 64; r++) {
                const char* label =
                    regs[r].start >= 0x7FFF00000000ULL          ? "[stack]"  :
                    regs[r].start >= MMAP_BASE                  ? "[mmap]"   :
                    regs[r].start >= 0x31000000ULL              ? "[dynlib]" :
                    regs[r].start >= SHARED_LIBC_BASE - 0x1000  ? "[libc]"   :
                    regs[r].start <  0x100000ULL                ? "[program]": "[heap]";
                off += snprintf(buf + off, bufsz - off, "%012llx-%012llx r%c%c  %s\n",
                                (unsigned long long)regs[r].start, (unsigned long long)regs[r].end,
                                regs[r].writable ? 'w' : '-',
                                regs[r].exec ? 'x' : '-', label);
            }
            break;
        }
        case PROC_MOUNTS: {
            // The mounted filesystems, in the Linux /proc/mounts layout:
            //   device  mountpoint  fstype  options  dump  pass
            // First the in-memory root VFS, then every entry of the append-only
            // mount_table (only /mnt ext2 at boot). The lockless [0,mount_count)
            // read is safe by mount_table's design: an entry is fully written
            // before mount_count is published. NyxOS keeps no backing-device path,
            // so column 1 is a descriptive source label, not a /dev node.
            int off = snprintf(buf, bufsz, "rootfs / nyxfs rw 0 0\n");
            for (int i = 0; i < mount_count && off < bufsz - 80; i++) {
                const char* fstype = mount_table[i].type == FS_TYPE_EXT2 ? "ext2" : "none";
                const char* dev    = mount_table[i].type == FS_TYPE_EXT2 ? "ext2img" : "none";
                off += snprintf(buf + off, bufsz - off, "%s %s %s rw 0 0\n",
                                dev, mount_table[i].mount_point, fstype);
            }
            break;
        }
    }
    return (int)strlen(buf);
}

// Serializes proc_sync against itself across CPUs. preempt_disable() only stops a
// context switch on the LOCAL core, so on SMP two cores could reconcile /proc at
// once — their swap-removes and proc_make() appends into the SAME proc_node->children[]
// then interleave and corrupt the array (or double-release a node). A real spinlock,
// like node_pool_lock/mount_table_lock already use, is the actual fix (issue #57).
// Lock order is proc_lock -> node_pool_lock (release_node/proc_make take the latter);
// no caller holds node_pool_lock when entering here, so the order never inverts.
static spinlock_t proc_lock = SPINLOCK_INIT;

// Whether a process should be represented by a /proc/<pid> dir. Excludes the idle
// task (pid 0) and -- the #66 fix -- a process that has exited but is still parked
// in the table as a zombie awaiting reap: proc_sync() runs on every vfs_open, so a
// lingering zombie otherwise keeps its 4-node /proc subtree pinned, and across a
// sustained exec/compile session those drain the fixed node pool until every path
// lookup fails. A zombie's /proc content is dead state anyway. Pure (reads only p's
// own fields) so proc_expose_selftest can pin the rule.
static inline int proc_should_expose(const process_t* p) {
    return p && p->pid != 0 && p->state != PROC_ZOMBIE;
}

// Reconcile /proc/<pid> dirs with the live process table: drop dead ones, add
// new ones (each with status + cmdline children). Idempotent and cheap; called
// at vfs_open / vfs_isdir so an open/getdents/chdir sees the current pids. The
// node-pool churn is guarded — alloc/free plus the children[] edits run under
// proc_lock so a concurrent reconcile on another core can't race it.
static void proc_sync(void) {
    if (!proc_node) return;
    extern process_t* process_table[];
    extern int process_count;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    // 1. Remove pid dirs whose process is gone (free their children first).
    //
    // release_node, not free_node: a VFS fd IS the node pointer, and this runs
    // from vfs_open/vfs_isdir — so `cat /proc/<pid>/status` on a process that
    // exits mid-read used to have its node handed straight back to the pool and
    // reissued by the very next open. The reader then read, and could write
    // through, whatever file got the slot. Held nodes are unlinked from /proc
    // here (so they stop resolving) and freed by their last close instead.
    for (uint32_t i = 0; i < proc_node->child_count; ) {
        vfs_node_t* c = proc_node->children[i];
        // Drop the dir when its process is gone from the table OR still there but a
        // zombie (exited) -- proc_should_expose captures both (#66). The predicate
        // is only evaluated for pid dirs, so the static /proc files are untouched.
        if (c->proc_type == PROC_PID_DIR && !proc_should_expose(find_process(c->proc_pid))) {
            for (uint32_t j = 0; j < c->child_count; j++) release_node(c->children[j]);
            c->child_count = 0;
            release_node(c);
            proc_node->children[i] = proc_node->children[--proc_node->child_count];
        } else i++;
    }
    // 2. Add a dir for each live process that lacks one.
    for (int i = 0; i < process_count; i++) {
        process_t* p = process_table[i];
        if (!proc_should_expose(p)) continue;   // skip idle (pid 0) and exited zombies (#66)
        int have = 0;
        for (uint32_t k = 0; k < proc_node->child_count; k++) {
            vfs_node_t* c = proc_node->children[k];
            if (c->proc_type == PROC_PID_DIR && c->proc_pid == p->pid) { have = 1; break; }
        }
        if (have) continue;
        char name[12];
        proc_utoa(p->pid, name);
        vfs_node_t* d = proc_make(proc_node, name, 1, PROC_PID_DIR, p->pid);
        if (!d) continue;                        // pool full — skip this pid
        proc_make(d, "status",  0, PROC_PID_STATUS,  p->pid);
        proc_make(d, "cmdline", 0, PROC_PID_CMDLINE, p->pid);
        proc_make(d, "maps",    0, PROC_PID_MAPS,    p->pid);
    }
    spin_unlock_irqrestore(&proc_lock, fl);
}

// #66 KAT: the /proc exposure rule. proc_sync() must not create or keep a
// /proc/<pid> dir for the idle task or for an exited process still parked as a
// zombie -- those lingering dirs are exactly what drained the fixed node pool
// under sustained exec/compile activity. Pure: reads only each probe's own fields.
int proc_expose_selftest(void) {
    process_t t;
    t.pid = 4242; t.state = PROC_RUN;     if (!proc_should_expose(&t)) return 1;
    t.pid = 4242; t.state = PROC_ZOMBIE;  if ( proc_should_expose(&t)) return 2;  // the fix
    t.pid = 0;    t.state = PROC_RUN;      if ( proc_should_expose(&t)) return 3;
    if (proc_should_expose(NULL)) return 4;
    return 0;
}

void init_vfs(void) {
    node_count = 0;
    vfs_node_t* root = alloc_node();
    strcpy(root->name, "/");
    root->type = 1;
    root->parent = root;
    current_dir = root;

    // Default directories:
    vfs_mkdir("/home", 0755);
    vfs_mkdir("/home/user", 0755);
    vfs_mkdir("/tmp", 0755);
    vfs_mkdir("/etc", 0755);
    vfs_mkdir("/var", 0755);
    vfs_mkdir("/opt", 0755);
    vfs_mkdir("/dev", 0755);
    vfs_mkdir("/bin", 0755);
    vfs_mkdir("/proc", 0755);
    vfs_mkdir("/sys", 0755);

    // Create a welcome file. ONE vfs_write: it replaces the whole file (no per-fd position), so
    // the previous two-write version dropped the first line, leaving only "Type 'help'...".
    int fd = vfs_open("/home/user/welcome.txt", 1, 0644);
    vfs_write(fd, "Welcome to NyxOS v1.0.0\nType 'help' for commands.\n", 50);
    vfs_close(fd);

    // Default desktop config — the rice lives here. The compositor reads this at desktop
    // start (apply_nyx_config) to pick the wallpaper scene + accent color by name.
    static const char* nyxconf =
        "# NyxOS desktop config -- rice it here.\n"
        "# scheme: one-word colorscheme preset (nightfall|aurora|ember|nebula|abyss|forest|mono);\n"
        "#         when set it overrides the wallpaper + accent below. Uncomment to use one.\n"
        "# scheme = aurora\n"
        "# wallpaper: Limpio Nightfall Plano Estrellas Meteoros Aurora Nebula Luces Ondas Astral Lluvia Cordillera\n"
        "wallpaper = Nightfall\n"
        "# accent tints the wallpaper AND the whole UI (title bars, taskbar, menus, pop-ups)\n"
        "# accent: Morado Azul Turquesa Verde Lima Oro Naranja Rojo Rosa Pizarra Carbon — or a #RRGGBB hex for any color\n"
        "accent = Morado\n"
        "# border: focused-window outline color (accent | any accent color name); default follows the accent\n"
        "# border = Turquesa\n"
        "# panel_tint: how much the taskbar tints toward the wallpaper color, 0-100 (0 = off)\n"
        "# panel_tint = 60\n"
        "# widget: a live CPU/RAM graph panel on the desktop (on|off)\n"
        "widget = on\n"
        "# widget_pos: which corner the panel hugs (bottom-right|bottom-left|top-right|top-left)\n"
        "widget_pos = bottom-right\n"
        "# clock: the taskbar clock face (24h|12h)\n"
        "clock = 24h\n"
        "# icons: show the desktop app icons (Files/Terminal/.../Selene) on|off; default off (clean desktop)\n"
        "# icons = on\n"
        "# gaps: px inset around + between snapped/maximized tiles (0 = classic, off)\n"
        "gaps = 0\n"
        "# rounding: window + start-menu corner radius in px, 0-14 (0 = square windows)\n"
        "# rounding = 0\n"
        "# shadow: window + start-menu drop shadows (on|off); off = a flat, shadowless desktop\n"
        "# shadow = off\n"
        "# title_align: window title-bar text alignment (left|center); default left\n"
        "# title_align = center\n"
        "# wallpaper_dim: darken the wallpaper 0-80% so windows/icons pop (0 = off)\n"
        "# wallpaper_dim = 40\n";
    int cfd = vfs_open("/etc/nyx.conf", 1, 0644);
    vfs_write(cfd, nyxconf, strlen(nyxconf));
    vfs_close(cfd);

    // Special device files under /dev — regular-looking nodes whose read/write are
    // intercepted by dev_type in vfs_pread/vfs_pwrite. urandom is an alias of random.
    static const struct { const char* path; uint32_t dt; } devs[] = {
        {"/dev/null", DEV_NULL}, {"/dev/zero", DEV_ZERO},
        {"/dev/random", DEV_RANDOM}, {"/dev/urandom", DEV_RANDOM},
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        int dfd = vfs_open(devs[i].path, 1, 0);           // O_CREAT
        vfs_node_t* dn = handle_to_node(dfd);
        if (dn) dn->dev_type = devs[i].dt;
    }

    // /proc static files — generated on read (proc_generate); per-pid dirs are
    // added later by proc_sync(). /proc was created by vfs_mkdir above.
    proc_node = resolve_path("/proc");
    static const struct { const char* name; uint32_t pt; } procf[] = {
        {"meminfo", PROC_MEMINFO}, {"uptime", PROC_UPTIME},
        {"version", PROC_VERSION}, {"cpuinfo", PROC_CPUINFO},
        {"mounts",  PROC_MOUNTS},  {"stat",   PROC_STAT},
        {"cmdline", PROC_CMDLINE}, {"filesystems", PROC_FILESYSTEMS},
    };
    for (unsigned i = 0; i < sizeof(procf) / sizeof(procf[0]); i++)
        proc_make(proc_node, procf[i].name, 0, procf[i].pt, 0);
}

// Canonicalize a path for the mount-aware VFS entry points (defined below, near
// join_mount_relative). Forward-declared here so the earlier file operations can use
// it: a relative path in a mount-backed CWD (the persistent home) is joined to an
// absolute mount path so the op hits the disk; everything else is returned unchanged.
static const char* vfs_abs(const char* path, char* buf, int n);

int vfs_open(const char* path, int flags, mode_t mode) {
    (void)mode;
    char _absbuf[256]; path = vfs_abs(path, _absbuf, sizeof(_absbuf));

    // Refresh /proc's per-pid dirs so an open/getdents under /proc sees the
    // current process table (cheap no-op when nothing changed).
    proc_sync();

    // Mount paths (e.g. /mnt/...) are backed by a real filesystem, not the
    // ramdisk tree. Create a transient node that mirrors the FS file so that
    // fd-based read/write/close work and writes flush back to disk.
    mount_entry_t* me = vfs_find_mount(path);
    if (me) {
        const char* sub = path + strlen(me->mount_point);
        if (!*sub) sub = "/";

        // Directory? readdir returns -1 for non-dirs, so probe by loading its
        // entries once into the node; vfs_readdir then serves them by index.
        dirent_t* dents = (dirent_t*)kmalloc(sizeof(dirent_t) * VFS_MOUNT_DIRENTS);
        int nd = (me->readdir && dents) ? me->readdir(sub, dents, VFS_MOUNT_DIRENTS) : -1;
        if (nd >= 0) {
            vfs_node_t* dn = alloc_node();
            if (!dn) { if (dents) kfree(dents); return -1; }
            dn->type = 1; dn->mount_backed = 1; dn->mount_ent = me;
            strncpy(dn->mpath, sub, MAX_NAME - 1);
            dn->data = (uint8_t*)dents; dn->size = (uint32_t)nd;
            dn->open_refs = 1;
            return node_to_handle(dn);
        }
        if (dents) kfree(dents);

        // Otherwise a regular file.
        uint32_t exists = me->resolve ? me->resolve(sub) : 0;
        if (!exists && !(flags & 1)) return -1;        // read of a missing file
        vfs_node_t* n = alloc_node();
        if (!n) return -1;
        n->type = 0;
        n->mount_backed = 1;
        n->mount_ent = me;
        strncpy(n->mpath, sub, MAX_NAME - 1);
        if (!exists && (flags & 1)) {
            if (me->write_file) me->write_file(sub, "", 0);   // create empty
        } else if (exists && me->read_file) {
            uint32_t sz = me->get_size ? me->get_size(sub) : 0;
            if (sz > 0) {
                n->data = (uint8_t*)kmalloc(sz);
                if (n->data) {
                    int r = me->read_file(sub, n->data, sz);
                    n->size = (r > 0) ? (uint32_t)r : 0;
                }
            }
        }
        n->open_refs = 1;
        return node_to_handle(n);
    }

    vfs_node_t* ino = resolve_path(path);

    if (flags & 1) { // O_CREAT — create a file if it does not exist
        if (!ino) {
            char child_name[MAX_NAME];
            vfs_node_t* parent = resolve_parent(path, child_name);
            if (!parent || parent->type != 1) return -1;
            ino = alloc_node();
            if (!ino) return -1;
            strncpy(ino->name, child_name, MAX_NAME-1);
            ino->type = 0;
            ino->parent = parent;
            if (vfs_append_child(parent, ino) != 0) { free_node(ino); return -1; }
        }
        if (flags & 2) {                 // O_TRUNC — reset an existing file to empty
            if (ino->data) { kfree(ino->data); ino->data = 0; }
            ino->size = 0;
        }
        ino->open_refs++;
        return node_to_handle(ino);
    }

    if (!ino) return -1;
    ino->readdir_idx = 0;  // reset readdir cursor on open
    ino->open_refs++;
    return node_to_handle(ino);
}

// STATELESS whole-file-head read: always copies from offset 0, so it NEVER reaches
// EOF — a `while (vfs_read(...) > 0)` streaming loop re-reads the head forever. For
// streaming/EOF use vfs_pread with a caller-tracked offset (as cmd_cat does).
int vfs_read(int fd, void* buf, size_t count) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino || ino->type != 0) return -1;
    if (count > ino->size) count = ino->size;
    if (ino->data) memcpy(buf, ino->data, count);
    return count;
}

// Flush a mount-backed node's full contents back to its filesystem, then mark it clean.
static void flush_mount_node(vfs_node_t* ino) {
    if (!ino->mount_backed || !ino->mount_ent) return;
    mount_entry_t* me = (mount_entry_t*)ino->mount_ent;
    if (me->write_file) me->write_file(ino->mpath, ino->data ? ino->data : (uint8_t*)"", ino->size);
    ino->dirty = 0;
}

// WHOLE-FILE write: sets the file to exactly these `count` bytes (offset 0, size = count). There
// is NO per-fd position, so calling this MORE THAN ONCE on a handle does not stream/append — each
// call replaces the whole file and only the last survives. For incremental/append writes use
// vfs_pwrite (offset-aware, grows + preserves). (Two bugs came from ignoring this: cmd_echo's
// redirect and the welcome.txt setup — both now build one buffer and write once.)
int vfs_write(int fd, const void* buf, size_t count) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino || ino->type != 0) return -1;
    if (!ino->data || ino->size < count) {
        uint8_t* new_data = (uint8_t*)kmalloc(count + BLOCK_SIZE);
        if (!new_data) return -1;
        if (ino->data) {
            memcpy(new_data, ino->data, ino->size);
            kfree(ino->data);
        }
        ino->data = new_data;
    }
    memcpy(ino->data, buf, count);
    ino->size = count;
    flush_mount_node(ino);            // persist to disk if this is a mount file
    return count;
}

// Offset-aware read: copy up to `count` bytes starting at `offset`. Returns the
// number of bytes read (0 at/after EOF), or -1 on a bad handle.
int vfs_pread(int fd, void* buf, uint32_t count, uint32_t offset) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino || ino->type != 0) return -1;
    if (ino->dev_type) {                       // /dev special: content is generated
        switch (ino->dev_type) {
            case DEV_NULL:   return 0;         // always EOF
            case DEV_ZERO:   memset_asm(buf, 0, count); return (int)count;
            case DEV_RANDOM: csprng_bytes((uint8_t*)buf, count); return (int)count;
        }
    }
    if (ino->proc_type) {                      // /proc: content synthesized on read
        char gbuf[1024];               // maps can list many regions
        int len = proc_generate(ino, gbuf, sizeof(gbuf));
        if (offset >= (uint32_t)len) return 0;
        uint32_t avail = (uint32_t)len - offset;
        if (count > avail) count = avail;
        if (count) memcpy(buf, gbuf + offset, count);
        return (int)count;
    }
    if (offset >= ino->size) return 0;
    uint32_t avail = ino->size - offset;
    if (count > avail) count = avail;
    if (ino->data && count) memcpy(buf, ino->data + offset, count);
    return (int)count;
}

// Offset-aware write: write `count` bytes at `offset`, growing the file (and
// zero-filling any gap) as needed without discarding existing content.
int vfs_pwrite(int fd, const void* buf, uint32_t count, uint32_t offset) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino || ino->type != 0) return -1;
    if (ino->dev_type) return (int)count;      // /dev/null etc: accept + discard
    uint32_t end = offset + count;
    if (end < offset) return -1;                 // overflow
    // Grow ino->data only when the write runs past the CURRENT allocation, and then grow
    // GEOMETRICALLY (>= double). The old code reallocated the whole buffer on EVERY write
    // that extended the file, so a large file written in small chunks (a tcc object built
    // in-OS: ~4 KB per write() call) reallocated O(n) times and fragmented the kernel heap
    // until kmalloc failed and the write truncated (seen at ~274 KB). ksize() reads the
    // block's real capacity from its allocation header, so no separate capacity field is
    // needed and the reallocs drop to O(log n).
    if (!ino->data || end > ksize(ino->data)) {
        uint32_t oldcap = ksize(ino->data);      // 0 when ino->data is NULL
        uint32_t newcap = end + BLOCK_SIZE;
        if (newcap < oldcap * 2) newcap = oldcap * 2;
        uint8_t* nd = (uint8_t*)kmalloc(newcap);
        if (!nd) return -1;
        memset_asm(nd, 0, newcap);               // zero-fill gaps beyond old data
        if (ino->data) { memcpy(nd, ino->data, ino->size); kfree(ino->data); }
        ino->data = nd;
    } else if (offset > ino->size) {
        memset_asm(ino->data + ino->size, 0, offset - ino->size);  // zero a sparse gap in place
    }
    if (count) memcpy(ino->data + offset, buf, count);
    if (end > ino->size) ino->size = end;
    ino->dirty = 1;                   // defer persistence to close (see the dirty field)
    return (int)count;
}

// KAT: vfs_pwrite is the offset-aware write behind SYS_PWRITE, the shell `>>` append, and
// the chunked in-OS `cc`/tcc object writes. It grows the buffer geometrically and — the
// security-critical part — a write PAST EOF must ZERO the skipped gap so a file can never
// read back stale kernel-heap bytes. It was unpinned until now. Drives a throwaway pool
// node directly (an fd is just the node's 1-based index in nodes[], see node_to_handle),
// so nothing touches the directory tree or disk.
int vfs_pwrite_selftest(void) {
    vfs_node_t* n = alloc_node();
    if (!n) return 1;
    n->type = 0;                       // a regular file (alloc_node zeroed data/size/dev_type)
    int fd = node_to_handle(n);
    uint8_t rb[4];
    int rc = 0;
    // 1) plain write at offset 0
    if (vfs_pwrite(fd, "hello", 5, 0) != 5 || n->size != 5) rc = 10;
    else if (vfs_pread(fd, rb, 4, 0) != 4 || rb[0]!='h'||rb[1]!='e'||rb[2]!='l'||rb[3]!='l') rc = 11;
    // 2) append exactly at EOF
    else if (vfs_pwrite(fd, "!!", 2, 5) != 2 || n->size != 7) rc = 12;
    else if (vfs_pread(fd, rb, 2, 5) != 2 || rb[0]!='!' || rb[1]!='!') rc = 13;
    // 3) overwrite inside the file: size unchanged, only those bytes change
    else if (vfs_pwrite(fd, "H", 1, 0) != 1 || n->size != 7) rc = 14;
    else if (vfs_pread(fd, rb, 1, 0) != 1 || rb[0] != 'H') rc = 15;
    // 4) SPARSE write past EOF, in place (within the current allocation): the gap MUST be zero
    else if (vfs_pwrite(fd, "XY", 2, 20) != 2 || n->size != 22) rc = 16;
    else {
        for (int i = 7; i < 20 && !rc; i++)
            if (vfs_pread(fd, rb, 1, (uint32_t)i) != 1 || rb[0] != 0) rc = 17;   // stale-data leak
        if (!rc && (vfs_pread(fd, rb, 2, 20) != 2 || rb[0]!='X' || rb[1]!='Y')) rc = 18;
    }
    // 5) a big sparse write that FORCES a realloc-grow: old data preserved, new gap still zero
    if (!rc) {
        if (vfs_pwrite(fd, "PQ", 2, 8000) != 2 || n->size != 8002) rc = 20;
        else if (vfs_pread(fd, rb, 1, 0)    != 1 || rb[0] != 'H') rc = 21;   // preserved across realloc
        else if (vfs_pread(fd, rb, 1, 4000) != 1 || rb[0] != 0)   rc = 22;   // gap zeroed
        else if (vfs_pread(fd, rb, 2, 8000) != 2 || rb[0]!='P' || rb[1]!='Q') rc = 23;
    }
    // 6) offset+count overflow is rejected without disturbing the file
    if (!rc) {
        uint32_t before = n->size;
        if (vfs_pwrite(fd, "z", 2, 0xFFFFFFFFu) != -1 || n->size != before) rc = 24;
    }
    // 7) a bad fd is refused
    if (!rc && vfs_pwrite(0, "z", 1, 0) != -1) rc = 25;
    if (n->data) { kfree(n->data); n->data = NULL; }
    free_node(n);
    return rc;
}

/* dup(oldfd) aliases a handle, and for a VFS handle "the handle" is literally
 * this node pointer. That was documented as safe because vfs_close is a no-op
 * for the ramdisk nodes ring 3 normally opens — true, but NOT for a
 * mount-backed mirror, which vfs_close frees. Two fds onto one /mnt file
 * therefore produced a double kfree of ->data and a node returned to the pool
 * twice, which then hands the same node out to two different opens.
 *
 * A count of the aliases is enough: only the last close releases. The count is
 * kept for EVERY node kind, not just mount mirrors — a dup'd /proc handle is
 * just as capable of outliving the process whose directory it names. */
void vfs_dup(int fd) {
    if (fd <= 0) return;
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino) return;
    ino->open_refs++;
}

int vfs_close(int fd) {
    if (fd <= 0) return 0;
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino) return 0;
    if (ino->open_refs > 0) ino->open_refs--;
    if (ino->open_refs > 0) return 0;              // another fd still holds it

    // Persist a mount file's deferred writes at the last close (v6.4.10 write-back).
    // Skip an orphaned (unlinked-while-open) file: POSIX drops its data at last close,
    // so flushing it back to its path would wrongly resurrect it. flush_mount_node is a
    // no-op for non-mount nodes and clears the dirty flag.
    if (ino->dirty && !ino->orphaned) flush_mount_node(ino);

    /* At zero refs, two kinds of node go back to the pool: a transient
     * mount mirror (which only ever existed for this fd), and one that was
     * unlinked or reaped from the tree while this fd was open. Everything else
     * is a live tree node and simply stays where it is. */
    if (ino->mount_backed || ino->orphaned) {
        if (ino->data) { kfree(ino->data); ino->data = NULL; }
        ino->size = 0;
        free_node(ino);
    }
    return 0;
}

// 1 if `path` names a directory in the ramdisk tree (used by chdir to reject a
// file). Mount points are directories too.
int vfs_isdir(const char* path) {
    mount_entry_t* me = vfs_find_mount(path);
    if (me) {
        // The mount point itself is a directory; for a path INSIDE the mount, ask the
        // backing FS whether it is really a directory rather than assuming so. readdir
        // returns >=0 only for a directory (-1 for a regular file / missing), the same
        // probe vfs_open uses. Without this, every mounted path (incl. plain files)
        // reported as a directory — which made vfs_stat short-circuit to size 0 and
        // let `cd` descend into a file.
        const char* sub = path + strlen(me->mount_point);
        if (!*sub || (sub[0] == '/' && !sub[1])) return 1;   // the mount root
        dirent_t* d = (dirent_t*)kmalloc(sizeof(dirent_t) * VFS_MOUNT_DIRENTS);
        int nd = (me->readdir && d) ? me->readdir(sub, d, VFS_MOUNT_DIRENTS) : -1;
        if (d) kfree(d);
        return (nd >= 0) ? 1 : 0;
    }
    proc_sync();                       // so `cd /proc/<pid>` resolves a live pid
    vfs_node_t* n = resolve_path(path);
    return (n && n->type == 1) ? 1 : 0;
}

// stat() core: report a path's size + whether it's a directory. Handles ramdisk,
// /proc, /dev and mounted (/mnt) files by leaning on vfs_isdir + vfs_open/vfs_fsize
// (which already resolve mounts). Returns 0 on success, -1 if the path doesn't exist.
int vfs_stat(const char* path, uint32_t* size, int* is_dir) {
    if (vfs_isdir(path)) { if (is_dir) *is_dir = 1; if (size) *size = 0; return 0; }
    int fd = vfs_open(path, 0, 0);     // O_RDONLY, no create — fails if absent
    if (fd < 0) return -1;
    int sz = vfs_fsize(fd);
    vfs_close(fd);
    if (is_dir) *is_dir = 0;
    if (size) *size = (sz > 0) ? (uint32_t)sz : 0;
    return 0;
}

// fstat() core: size + type from an already-open internal vfs fd (a vfs_node_t*).
int vfs_fstat(int fd, uint32_t* size, int* is_dir) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino) return -1;
    if (is_dir) *is_dir = (ino->type == 1);
    int sz = vfs_fsize(fd);
    if (size) *size = (sz > 0) ? (uint32_t)sz : 0;
    return 0;
}

int vfs_create_from_mem(const char* path, uint8_t* data, uint32_t size) {
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent((char*)path, child_name);
    if (!parent || parent->type != 1) return -1;
    vfs_node_t* ino = alloc_node();
    if (!ino) return -1;
    strncpy(ino->name, child_name, MAX_NAME-1);
    ino->type = 0;
    ino->size = size;
    ino->data = data;
    ino->parent = parent;
    if (vfs_append_child(parent, ino) != 0) { free_node(ino); return -1; }
    return node_to_handle(ino);
}

uint32_t vfs_fsize(int fd) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino || ino->type != 0) return 0;
    return ino->size;
}

uint8_t* vfs_fdata(int fd) {
    vfs_node_t* ino = handle_to_node(fd);
    if (!ino || ino->type != 0) return NULL;
    return ino->data;
}

int vfs_mkdir(const char* path, mode_t mode) {
    (void)mode;
    char _absbuf[256]; path = vfs_abs(path, _absbuf, sizeof(_absbuf));
    mount_entry_t* me = vfs_find_mount(path);
    if (me && me->mkdir) {
        const char* subpath = path + strlen(me->mount_point);
        return me->mkdir(subpath);
    }
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(path, child_name);
    if (!parent || parent->type != 1) return -1;
    if (find_child(parent, child_name)) return -1;

    vfs_node_t* dir = alloc_node();
    if (!dir) return -1;
    strncpy(dir->name, child_name, MAX_NAME-1);
    dir->type = 1;
    dir->mode = 0755;
    dir->parent = parent;
    if (vfs_append_child(parent, dir) != 0) { free_node(dir); return -1; }
    return 0;
}

int vfs_unlink(const char* path) {
    char _absbuf[256]; path = vfs_abs(path, _absbuf, sizeof(_absbuf));
    mount_entry_t* me = vfs_find_mount(path);
    if (me && me->unlink) {
        const char* subpath = path + strlen(me->mount_point);
        return me->unlink(subpath);
    }
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(path, child_name);
    if (!parent || parent->type != 1) return -1;
    for (uint32_t i = 0; i < parent->child_count; i++) {
        vfs_node_t* victim = parent->children[i];
        if (strcmp(victim->name, child_name) != 0) continue;

        /* Removing a directory that still has children used to drop the whole
         * subtree on the floor: the directory node went away and every node
         * under it stayed allocated but unreachable, gone from the tree and
         * never returned to the pool. Refuse instead — same rule the ext2 side
         * applies, and it stops `rm` from quietly destroying a populated
         * directory. */
        if (victim->type == 1 && victim->child_count > 0) return -1;

        /* This was `memset_asm(victim, 0, sizeof(vfs_node_t))` and nothing else:
         * the node was wiped in place but never handed back, so every delete
         * permanently burned one of the MAX_INODES slots (256 total, root alone
         * holds ~58) until file creation simply stopped working. The wipe was
         * also done with no regard for open fds — and an fd here is a pointer
         * straight at this struct. release_node does both parts properly. */
        for (uint32_t j = i; j < parent->child_count - 1; j++)
            parent->children[j] = parent->children[j+1];
        parent->child_count--;
        victim->parent = NULL;
        release_node(victim);
        return 0;
    }
    return -1;
}

// Post-order recursive removal of a node and its whole subtree — the engine behind
// `rm -r`. Children are released before their parent, so no subtree is ever left
// orphaned (the leak vfs_unlink's refuse-nonempty rule guards against). A symlink
// (type 2) or file (type 0) is a LEAF — released without following its target, so
// `rm -r` on a symlink-to-dir removes the link, not the target's contents. Depth is
// bounded (pathological nesting is refused, not stack-overflowed). 0 ok, -1 refused.
static int rmtree_node(vfs_node_t* n, int depth) {
    if (!n) return -1;
    if (depth > 64) return -1;                       // pathological nesting; refuse cleanly
    if (n->type == 1) {                              // directory: remove children first
        while (n->child_count > 0)
            if (rmtree_node(n->children[n->child_count - 1], depth + 1) != 0) return -1;
    }
    vfs_node_t* p = n->parent;                       // detach from parent
    if (p) {
        for (uint32_t i = 0; i < p->child_count; i++)
            if (p->children[i] == n) {
                for (uint32_t j = i; j + 1 < p->child_count; j++) p->children[j] = p->children[j + 1];
                p->child_count--;
                break;
            }
    }
    n->parent = NULL;
    release_node(n);                                 // honours open_refs (orphans if held)
    return 0;
}

// Recursively remove `path` and, if it is a directory, its entire subtree. RAM tree
// only; a mount-backed path falls back to a single unlink (errors on a non-empty dir,
// as before). Returns 0, or -1 (not found / mount non-empty / too deep).
int vfs_rmtree(const char* path) {
    char _absbuf[256]; path = vfs_abs(path, _absbuf, sizeof(_absbuf));
    mount_entry_t* me = vfs_find_mount(path);
    if (me && me->unlink) {
        const char* subpath = path + strlen(me->mount_point);
        return me->unlink(subpath);
    }
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(path, child_name);
    if (!parent || parent->type != 1) return -1;
    vfs_node_t* victim = find_child(parent, child_name);
    if (!victim) return -1;
    return rmtree_node(victim, 0);
}

void hide_file(const char* path) {
    (void)path;
}

void vfs_rename(const char* old, const char* new) {
    char _ao[256], _an[256];
    old = vfs_abs(old, _ao, sizeof(_ao));   // so `mv a b` in the mounted home
    new = vfs_abs(new, _an, sizeof(_an));   // resolves both endpoints onto the disk
    // If either endpoint is on a mounted FS, the ramdisk tree walk below can't
    // find or place it. Fall back to copy-then-delete, which works for files
    // across any mix of ramdisk and mounted FS (vfs_cp/vfs_unlink are both
    // mount-aware). Directories on a mount aren't moved — that would need a
    // recursive copy — and vfs_cp fails cleanly on them, so `old` is preserved.
    if (vfs_find_mount(old) || vfs_find_mount(new)) {
        if (vfs_cp(old, new) == 0)
            vfs_unlink(old);
        return;
    }
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(old, child_name);
    if (!parent) return;
    vfs_node_t* ino = find_child(parent, child_name);
    if (!ino) return;
    char new_name[MAX_NAME];
    vfs_node_t* new_parent = resolve_parent(new, new_name);
    if (!new_parent) { strncpy(ino->name, new, MAX_NAME-1); return; }
    if (new_parent != parent) {
        // Append to the destination FIRST — if it's at MAX_CHILDREN, abort the move
        // and leave `ino` in its original parent rather than orphaning it (or writing
        // past the destination's children[]). The rename below only runs once the move
        // has committed, so a dest-full failure leaves the source FULLY unchanged (it
        // used to rename ino here first, so a failed cross-dir move left "/a/x" as "/a/y").
        if (vfs_append_child(new_parent, ino) != 0) return;
        for (uint32_t i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == ino) {
                for (uint32_t j = i; j < parent->child_count - 1; j++)
                    parent->children[j] = parent->children[j+1];
                parent->child_count--;
                break;
            }
        }
        ino->parent = new_parent;
    }
    strncpy(ino->name, new_name, MAX_NAME-1);   // rename only after any cross-dir move succeeded
}

dirent_t* vfs_readdir(int fd) {
    vfs_node_t* dir = handle_to_node(fd);
    if (!dir || dir->type != 1) return NULL;
    if (dir->mount_backed) {               // mounted-FS dir: entries loaded at open
        dirent_t* ents = (dirent_t*)dir->data;
        if (!ents || dir->readdir_idx >= dir->size) return NULL;
        return &ents[dir->readdir_idx++];
    }
    if (dir->readdir_idx >= dir->child_count) return NULL;
    vfs_node_t* child = dir->children[dir->readdir_idx];
    dir->readdir_idx++;
    static dirent_t entry;
    strncpy(entry.name, child->name, MAX_FILENAME - 1);
    entry.name[MAX_FILENAME - 1] = '\0';
    entry.type = child->type;
    entry.ino = child->node_id;
    return &entry;
}

// ==================== Mount table ====================

// --- FS re-entrancy guard ---------------------------------------------------
// EXT2 shares global scratch buffers (`ext2_bufs[5]` in ext2.c) across an entire
// operation, so the driver is non-reentrant. preempt_disable() here used to be a
// coarse single-core "FS lock" — it kept the local thread from switching mid-op,
// but meant nothing to another core, which under smpbalance enters the driver at
// the same instant and tramples the same buffers. ext2_lock (v5.9.25) is the real
// cross-core lock; auth.c's passwd helpers take it too, so a login credential check
// and a /mnt file op serialize against each other. These thin wrappers still sit on
// the mount function-pointers, so ext2.c needs no internal changes.
static uint32_t fs_resolve(const char* p) {
    uint64_t fl = ext2_lock_acquire(); uint32_t r = ext2_resolve(p);   ext2_lock_release(fl); return r;
}
static uint32_t fs_get_size(const char* p) {
    uint64_t fl = ext2_lock_acquire(); uint32_t r = ext2_get_size(p);  ext2_lock_release(fl); return r;
}
static int fs_read_file(const char* p, void* b, uint32_t n) {
    uint64_t fl = ext2_lock_acquire(); int r = ext2_read_file(p, b, n); ext2_lock_release(fl); return r;
}
static int fs_write_file(const char* p, const void* b, uint32_t n) {
    uint64_t fl = ext2_lock_acquire(); int r = ext2_write_file(p, b, n); ext2_lock_release(fl); return r;
}
static int fs_readdir(const char* p, dirent_t* e, uint32_t m) {
    uint64_t fl = ext2_lock_acquire(); int r = ext2_readdir(p, e, m);  ext2_lock_release(fl); return r;
}
static int fs_mkdir(const char* p) {
    uint64_t fl = ext2_lock_acquire(); int r = ext2_mkdir(p);          ext2_lock_release(fl); return r;
}
static int fs_unlink(const char* p) {
    uint64_t fl = ext2_lock_acquire(); int r = ext2_unlink(p);         ext2_lock_release(fl); return r;
}

// Make a mount point visible in the ramdisk tree, so directory listings that
// walk children (the GUI file manager, `ls` of the parent) show it. vfs_open on
// this path still routes to the mounted FS — vfs_find_mount takes precedence —
// so the stub only supplies the parent's entry; it is never opened itself.
static void ensure_mount_stub(const char* mount_point) {
    if (resolve_path(mount_point)) return;      // already present
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(mount_point, child_name);
    if (!parent || parent->type != 1) return;
    if (parent->child_count >= MAX_CHILDREN) return;   // full: mount still works, just not listed
    if (find_child(parent, child_name)) return;
    vfs_node_t* dir = alloc_node();
    if (!dir) return;
    strncpy(dir->name, child_name, MAX_NAME - 1);
    dir->type = 1;
    dir->mode = 0755;
    dir->parent = parent;
    parent->children[parent->child_count++] = dir;
}

int vfs_mount(const char* mount_point, int fs_type, void* fs_data) {
    (void)fs_data;
    uint64_t fl = spin_lock_irqsave(&mount_table_lock);
    if (mount_count >= MAX_MOUNT_POINTS) {
        spin_unlock_irqrestore(&mount_table_lock, fl);
        return -1;
    }
    mount_entry_t* me = &mount_table[mount_count];
    strncpy(me->mount_point, mount_point, MAX_PATH - 1);
    me->type = fs_type;
    me->resolve = NULL;
    me->get_size = NULL;
    me->read_file = NULL;
    me->readdir = NULL;

    if (fs_type == FS_TYPE_EXT2) {
        // Point at the preempt-guarded wrappers, not ext2_* directly (see above).
        me->resolve   = fs_resolve;
        me->get_size  = fs_get_size;
        me->read_file = fs_read_file;
        me->write_file = fs_write_file;
        me->readdir    = fs_readdir;
        me->mkdir      = fs_mkdir;
        me->unlink     = fs_unlink;
    }

    mount_count++;   // publish: the entry is fully written before it's counted
    spin_unlock_irqrestore(&mount_table_lock, fl);

    // Ramdisk-tree side effect only (touches no mount_table state); done outside the
    // lock so node_pool_lock is never nested under mount_table_lock.
    ensure_mount_stub(mount_point);   // so the mount shows up when browsing "/"
    return 0;
}

mount_entry_t* vfs_find_mount(const char* path) {
    if (!path) return NULL;
    mount_entry_t* found = NULL;
    uint64_t fl = spin_lock_irqsave(&mount_table_lock);
    for (int i = 0; i < mount_count; i++) {
        int len = strlen(mount_table[i].mount_point);
        if (strncmp(path, mount_table[i].mount_point, len) == 0) {
            if (path[len] == '\0' || path[len] == '/') { found = &mount_table[i]; break; }
        }
    }
    spin_unlock_irqrestore(&mount_table_lock, fl);
    return found;   // entry is immutable once counted, so it's safe to use unlocked
}

// ==================== New helper functions ====================

const char* vfs_getcwd(void) {
    // Build the full absolute path by walking parent pointers up to the root.
    static char path[256];
    vfs_node_t* d = current_dir;
    if (!d || d == &nodes[0]) return "/";
    const char* parts[32];
    int n = 0;
    while (d && d != &nodes[0] && n < 32) { parts[n++] = d->name; d = d->parent; }
    int pos = 0;
    for (int i = n - 1; i >= 0 && pos < 250; i--) {
        path[pos++] = '/';
        for (int j = 0; parts[i][j] && pos < 255; j++) path[pos++] = parts[i][j];
    }
    path[pos] = '\0';
    return path;
}

// Per-shell CWD: the current directory is no longer a single global — each
// Terminal window owns its own directory node and swaps it in around command
// execution (via these accessors), so `cd` in one shell doesn't move another.
void* vfs_getcwd_node(void)  { return current_dir; }
void  vfs_setcwd_node(void* n) { if (n) current_dir = (vfs_node_t*)n; }
void* vfs_root_node(void)    { return &nodes[0]; }

// Materialize a mount-backed directory path (e.g. /mnt/home/<user>) as a chain of
// ramdisk "stub" directory nodes and return the leaf. The node-based CWD model (each
// Terminal owns a directory node) can't otherwise hold a path that lives on a mounted
// FS, because such a path has no ramdisk node: vfs_getcwd() rebuilds the path string
// by walking these stubs' parent pointers, while directory *contents* keep coming from
// the backing FS by that path string (vfs_readdir / vfs_list_dir). Idempotent — an
// existing node (a real one, or a stub from an earlier visit) is reused. Returns NULL
// on a malformed path or if a non-directory node shadows a path component.
static vfs_node_t* mount_dir_stub(const char* abspath) {
    if (!abspath || abspath[0] != '/') return NULL;
    vfs_node_t* cur = &nodes[0];                       // root
    const char* p = abspath;
    while (*p == '/') p++;
    char comp[MAX_NAME];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < MAX_NAME - 1) comp[i++] = *p++;
        comp[i] = '\0';
        while (*p == '/') p++;
        if (!comp[0]) continue;
        vfs_node_t* child = find_child(cur, comp);
        if (!child) {
            child = alloc_node();
            if (!child) return NULL;
            strncpy(child->name, comp, MAX_NAME - 1);
            child->name[MAX_NAME - 1] = '\0';
            child->type = 1;
            child->parent = cur;
            if (vfs_append_child(cur, child) != 0) { free_node(child); return NULL; }
        } else if (child->type != 1) {
            return NULL;                               // a file shadows this component
        }
        cur = child;
    }
    return (cur == &nodes[0]) ? NULL : cur;
}

// Join a relative chdir target `rel` onto the absolute base directory `base`,
// resolving "." and ".." into `out` (a normalized absolute path). Lets `cd <subdir>`
// and `cd ..` work while the CWD is a mount-backed directory. `base` is absolute;
// `rel` has no leading '/'.
static void join_mount_relative(const char* base, const char* rel, char* out, int outsz) {
    char work[256];
    int w = 0;
    for (const char* s = base; *s && w < (int)sizeof(work) - 1; s++) work[w++] = *s;
    if (w == 0) work[w++] = '/';
    work[w] = '\0';
    const char* p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);
        if (len == 1 && start[0] == '.') continue;                // "."
        if (len == 2 && start[0] == '.' && start[1] == '.') {     // ".." -> pop a component
            while (w > 0 && work[w - 1] != '/') w--;              // drop the trailing name
            if (w > 1) w--;                                       // drop its '/', but keep root "/"
            work[w] = '\0';
            continue;
        }
        if (w == 0 || work[w - 1] != '/') { if (w < (int)sizeof(work) - 1) work[w++] = '/'; }
        for (int j = 0; j < len && w < (int)sizeof(work) - 1; j++) work[w++] = start[j];
        work[w] = '\0';
    }
    if (w == 0) { work[w++] = '/'; work[w] = '\0'; }
    int o = 0;
    for (int i = 0; i < w && o < outsz - 1; i++) out[o++] = work[i];
    out[o] = '\0';
}

// KAT (`pathnorm` in the self-test battery) for join_mount_relative — the canonicaliser
// behind vfs_abs (mount-aware relative-path resolution for touch/mkdir/rm/mv/cp/Editor
// saves issued from a mounted CWD). Verifies "." is dropped, ".." pops exactly one
// component, names that only LOOK like dot-dirs ("...", "..foo", ".bashrc") stay literal,
// and — the containment property — that stacked ".." can never walk ABOVE root. Pure
// string logic, runs offline. Returns 0 on pass, else the failing case number.
static int pathnorm_eq(const char* base, const char* rel, const char* want) {
    char out[256];
    join_mount_relative(base, rel, out, (int)sizeof(out));
    return strcmp(out, want) == 0;
}
int vfs_pathnorm_selftest(void) {
    if (!pathnorm_eq("/mnt/home/nyx", "..",             "/mnt/home"))          return 1;
    if (!pathnorm_eq("/mnt/home/nyx", "../../foo",      "/mnt/foo"))           return 2;
    if (!pathnorm_eq("/mnt/home/nyx", "a/b/../c",       "/mnt/home/nyx/a/c"))  return 3;
    if (!pathnorm_eq("/mnt/home/nyx", "././.",          "/mnt/home/nyx"))      return 4;
    if (!pathnorm_eq("/mnt",          "a//b",           "/mnt/a/b"))           return 5;
    if (!pathnorm_eq("/mnt",          "",               "/mnt"))               return 6;
    // ".." containment at/above root — must never escape "/":
    if (!pathnorm_eq("/",             "..",             "/"))                  return 7;
    if (!pathnorm_eq("/a",            "..",             "/"))                  return 8;
    if (!pathnorm_eq("/mnt",          "../..",          "/"))                  return 9;
    if (!pathnorm_eq("/mnt/home/nyx", "../../../../..", "/"))                  return 10;
    if (!pathnorm_eq("/mnt",          "a/../..",        "/"))                  return 11;
    // look-alike names that are NOT dot-dirs stay literal:
    if (!pathnorm_eq("/mnt",          "...",            "/mnt/..."))           return 12;
    if (!pathnorm_eq("/mnt",          "..foo",          "/mnt/..foo"))         return 13;
    if (!pathnorm_eq("/mnt",          ".bashrc",        "/mnt/.bashrc"))       return 14;
    if (!pathnorm_eq("/",             "foo/bar",        "/foo/bar"))           return 15;
    return 0;
}

// Canonicalize `path` for the mount-aware file operations. An absolute path is returned
// unchanged. A RELATIVE path is joined onto the current directory ONLY when the CWD is on
// a mounted FS -- so `touch foo`, `mkdir bar`, `rm baz`, `mv`/`cp`, and Editor saves issued
// from the persistent home (/mnt/home/<user>) hit the ext2 disk instead of creating an
// invisible, ephemeral ramdisk node under the mount stub. A relative path with a ramdisk
// CWD is returned unchanged, so ramdisk-relative behaviour is preserved exactly. `buf` must
// hold >= 2 bytes; callers pass a 256-byte scratch buffer.
static const char* vfs_abs(const char* path, char* buf, int n) {
    if (!path || path[0] == '/') return path;      // already absolute (or null)
    const char* cwd = vfs_getcwd();
    if (!vfs_find_mount(cwd)) return path;          // ramdisk CWD: leave relative as-is
    join_mount_relative(cwd, path, buf, n);
    return buf;
}

// Canonicalise `path` into `out` as a normalised absolute path: make it absolute
// (a relative path is taken against the CWD), then collapse "." and ".." purely
// lexically through join_mount_relative — the same canonicaliser vfs_abs/cd use,
// pinned by the `pathnorm` KAT. Feeding an already-absolute path as the relative
// part over base "/" normalises it too (vfs_abs alone leaves absolute paths as-is).
// NyxOS has no symlinks, so this is a complete realpath; existence is the caller's
// concern (the `realpath -e` flag checks it via vfs_stat).
const char* vfs_realpath(const char* path, char* out, int outsz) {
    if (outsz <= 0) return out;
    if (!path || !*path) path = ".";
    const char* base = (path[0] == '/') ? "/" : vfs_getcwd();
    join_mount_relative(base, path, out, outsz);
    return out;
}

// Resolve `path` to its directory node (opaque handle, like vfs_root_node), or
// NULL if it doesn't exist or isn't a directory. Lets a new Terminal start in the
// logged-in user's home directory.
void* vfs_path_node(const char* path) {
    vfs_node_t* n = resolve_path(path);
    if (n) return (n->type == 1) ? n : NULL;
    // Not in the ramdisk tree: the path may be a directory on a mounted FS (the user's
    // home now lives at /mnt/home/<user>). Confirm via the mount, then represent it as
    // a stub chain so the node-based CWD can start there.
    if (vfs_find_mount(path) && vfs_isdir(path))
        return mount_dir_stub(path);
    return NULL;
}

int vfs_chdir(const char* path) {
    if (!path || !*path) return -1;

    // Resolve a relative target against the current directory when the CWD is on a
    // mounted FS (the user's home is now /mnt/home/<user>), so `cd <subdir>` and
    // `cd ..` work there. A ramdisk-rooted CWD is left untouched (its getcwd isn't
    // under a mount), so ramdisk `cd` behaviour is exactly as before.
    char abs[256];
    const char* target = path;
    if (path[0] != '/') {
        const char* cwd = vfs_getcwd();
        if (vfs_find_mount(cwd)) {
            join_mount_relative(cwd, path, abs, sizeof(abs));
            target = abs;
        }
    }

    mount_entry_t* me = vfs_find_mount(target);
    if (me) {
        // A directory on the mounted FS: reject files (vfs_isdir probes the backing
        // FS), then represent the path as a ramdisk stub chain so getcwd() and `cd ..`
        // track it. This also lets a deep `cd /mnt/a/b/c` succeed in one step (the old
        // code only stubbed a single level and failed on missing intermediates).
        if (!vfs_isdir(target)) return -1;
        vfs_node_t* dir = mount_dir_stub(target);
        if (!dir) return -1;
        current_dir = dir;
        return 0;
    }

    vfs_node_t* dir = resolve_path(target);
    if (!dir || dir->type != 1) return -1;
    current_dir = dir;
    return 0;
}

void vfs_list_dir(const char* path) {
    mount_entry_t* me = vfs_find_mount(path);
    if (me && me->readdir) {
        int mlen = strlen(me->mount_point);
        const char* subpath = path + mlen;
        if (subpath[0] == '\0') subpath = "/";
        static dirent_t entries[64];   // off-stack: ~8.7 KB would overflow the 4 KB kernel task
        int n = me->readdir(subpath, entries, 64);   // stack; `ls` is single-threaded terminal I/O
        if (n < 0) {
            printf("ls: %s: error reading directory\n", path ? path : "");
            return;
        }
        for (int i = 0; i < n; i++) {
            if (entries[i].type == 1) {
                set_terminal_color(vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
                printf("%s/\n", entries[i].name);
            } else {
                set_terminal_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
                printf("%s\n", entries[i].name);
            }
        }
        set_terminal_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
        return;
    }

    vfs_node_t* dir = path ? resolve_path(path) : current_dir;
    if (!dir || dir->type != 1) {
        printf("ls: %s: No such directory\n", path ? path : "");
        return;
    }
    for (uint32_t i = 0; i < dir->child_count; i++) {
        vfs_node_t* child = dir->children[i];
        if (child->type == 1) {
            set_terminal_color(vga_entry_color(VGA_LIGHT_BLUE, VGA_BLACK));
            printf("%s/\n", child->name);
        } else {
            set_terminal_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
            printf("%s  (%d bytes)\n", child->name, child->size);
        }
    }
    set_terminal_color(vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK));
}

// Print a file's entire contents verbatim. Streams via vfs_open + vfs_pread so it reads the
// WHOLE file uniformly across RAM, mount-backed (ext2 /mnt), /proc and /dev. The old mount
// fast-path did a SINGLE 511-byte me->read_file() and silently truncated any mounted file
// past 511 bytes (e.g. `cat /mnt/home/<user>/notes.txt`); the -n/-b path already streamed.
void vfs_cat_file(const char* path) {
    int fd = vfs_open(path, 0, 0);
    if (fd < 0) { printf("cat: %s: No such file\n", path); return; }
    char buf[512]; int n; uint32_t off = 0;
    while ((n = vfs_pread(fd, buf, sizeof buf, off)) > 0) {
        for (int i = 0; i < n; i++) putchar(buf[i]);
        off += (uint32_t)n;
    }
    vfs_close(fd);
}

int vfs_touch(const char* path) {
    char _absbuf[256]; path = vfs_abs(path, _absbuf, sizeof(_absbuf));
    mount_entry_t* me = vfs_find_mount(path);
    if (me && me->write_file) {
        int mlen = strlen(me->mount_point);
        const char* subpath = path + mlen;
        if (subpath[0] == '\0') subpath = "/";
        // Create empty file via write with len=0
        return me->write_file(subpath, "", 0) >= 0 ? 0 : -1;
    }
    vfs_node_t* ino = resolve_path(path);
    if (ino) return 0; // exists
    char child_name[MAX_NAME];
    vfs_node_t* parent = resolve_parent(path, child_name);
    if (!parent || parent->type != 1) return -1;
    ino = alloc_node();
    if (!ino) return -1;
    strncpy(ino->name, child_name, MAX_NAME-1);
    ino->type = 0;
    ino->parent = parent;
    if (vfs_append_child(parent, ino) != 0) { free_node(ino); return -1; }
    return 0;
}

int vfs_write_file(const char* path, const void* buf, uint32_t len) {
    char _absbuf[256]; path = vfs_abs(path, _absbuf, sizeof(_absbuf));
    mount_entry_t* me = vfs_find_mount(path);
    if (me && me->write_file) {
        int mlen = strlen(me->mount_point);
        const char* subpath = path + mlen;
        if (subpath[0] == '\0') subpath = "/";
        return me->write_file(subpath, buf, len);
    }
    // Fallback to RAM VFS — auto-create file if it doesn't exist
    vfs_node_t* ino = resolve_path(path);
    if (!ino) {
        char child_name[MAX_NAME];
        vfs_node_t* parent = resolve_parent(path, child_name);
        if (!parent || parent->type != 1) return -1;
        ino = alloc_node();
        if (!ino) return -1;
        strncpy(ino->name, child_name, MAX_NAME - 1);
        ino->type = 0;
        ino->parent = parent;
        if (vfs_append_child(parent, ino) != 0) { free_node(ino); return -1; }
    }
    if (ino->type != 0) return -1;
    if (!(ino->mode & 0200)) return -1;         // read-only (owner-write bit cleared by chmod): refuse
    if (ino->data) kfree(ino->data);
    ino->data = (uint8_t*)kmalloc(len);
    if (!ino->data && len > 0) return -1;
    if (len > 0) memcpy(ino->data, buf, len);
    ino->size = len;
    return len;
}

// Change a RAM-VFS node's permission bits (chmod). Mount-backed (/mnt) files don't carry
// their own mode here yet, so this covers the ramdisk tree. Returns 0, or -1 if not found.
int vfs_chmod(const char* path, uint16_t mode) {
    vfs_node_t* n = resolve_path(path);
    if (!n) return -1;
    n->mode = (uint16_t)(mode & 07777);
    return 0;
}

// Read a node's mode bits (for stat / ls -l). Returns -1 if the path doesn't resolve.
int vfs_getmode(const char* path) {
    vfs_node_t* n = resolve_path(path);
    return n ? (int)n->mode : -1;
}

int vfs_cp(const char* src, const char* dst) {
    char _acs[256], _acd[256];
    src = vfs_abs(src, _acs, sizeof(_acs));   // `cp a b` in the mounted home resolves
    dst = vfs_abs(dst, _acd, sizeof(_acd));   // both endpoints onto the disk
    // --- Load the source bytes into a buffer, whether it lives in the ramdisk
    // tree or on a mounted FS (e.g. /mnt). Only regular files are copied. ---
    uint8_t* sbuf = NULL;
    uint32_t ssize = 0;
    mount_entry_t* src_me = vfs_find_mount(src);
    if (src_me && src_me->read_file) {
        const char* sub = src + strlen(src_me->mount_point);
        if (!*sub) sub = "/";
        // Refuse to "copy" a directory (that would need recursion); a mounted-FS
        // readdir succeeding (>=0) means the path is a directory.
        if (src_me->readdir) {
            dirent_t probe;
            if (src_me->readdir(sub, &probe, 1) >= 0) return -1;
        }
        if (src_me->resolve && !src_me->resolve(sub)) return -1;   // missing
        ssize = src_me->get_size ? src_me->get_size(sub) : 0;
        sbuf = (uint8_t*)kmalloc(ssize ? ssize : 1);
        if (!sbuf) return -1;
        if (ssize) {
            int r = src_me->read_file(sub, sbuf, ssize);
            if (r < 0) { kfree(sbuf); return -1; }
            ssize = (r > 0) ? (uint32_t)r : 0;
        }
    } else {
        vfs_node_t* src_ino = resolve_path(src);
        if (!src_ino || src_ino->type != 0) return -1;
        ssize = src_ino->size;
        sbuf = (uint8_t*)kmalloc(ssize ? ssize : 1);
        if (!sbuf) return -1;
        if (ssize && src_ino->data) memcpy(sbuf, src_ino->data, ssize);
    }

    // --- Write to the destination, again ramdisk- or mount-backed. ---
    int rc = -1;
    mount_entry_t* dst_me = vfs_find_mount(dst);
    if (dst_me && dst_me->write_file) {
        const char* sub = dst + strlen(dst_me->mount_point);
        if (!*sub) sub = "/";
        rc = dst_me->write_file(sub, sbuf, ssize) >= 0 ? 0 : -1;
    } else {
        char child_name[MAX_NAME];
        vfs_node_t* dst_parent = resolve_parent(dst, child_name);
        if (dst_parent && dst_parent->type == 1) {
            vfs_node_t* existing = find_child(dst_parent, child_name);
            if (existing) {
                // Overwrite an existing regular file in place — the documented `cp`
                // behaviour ("replacing <dst> if it already exists"). Previously this
                // path only created a NEW node, so `cp a b` failed when b already
                // existed. A directory destination is refused (leaves rc = -1).
                if (existing->type == 0) {
                    uint8_t* nd = ssize ? (uint8_t*)kmalloc(ssize) : NULL;
                    if (!ssize || nd) {
                        if (existing->data) kfree(existing->data);
                        existing->data = nd;
                        if (ssize) memcpy(existing->data, sbuf, ssize);
                        existing->size = ssize;
                        rc = 0;
                    }
                }
            } else {
                vfs_node_t* dst_ino = alloc_node();
                if (dst_ino) {
                    strncpy(dst_ino->name, child_name, MAX_NAME-1);
                    dst_ino->type = 0;
                    dst_ino->parent = dst_parent;
                    dst_ino->size = ssize;
                    if (ssize) {
                        dst_ino->data = (uint8_t*)kmalloc(ssize);
                        if (dst_ino->data) memcpy(dst_ino->data, sbuf, ssize);
                        else dst_ino->size = 0;
                    }
                    if (vfs_append_child(dst_parent, dst_ino) == 0) {
                        rc = 0;
                    } else {
                        if (dst_ino->data) kfree(dst_ino->data);
                        free_node(dst_ino);
                    }
                }
            }
        }
    }
    kfree(sbuf);
    return rc;
}

/* VFS ramdisk open/create/trunc/read/write KAT (v6.5.72). Nails down the exact tmpfs
 * open-flag behaviour the shell pipe stresses (create /tmp/pipe, write, O_TRUNC-reopen,
 * rewrite, shrink) — a subsystem that had NO self-test. Asserts: O_CREAT makes a missing
 * file; O_CREAT|O_TRUNC empties an existing one; a write REPLACES the content and sets the
 * size (a shorter write shrinks it, leaving no stale tail); a read returns exactly `size`
 * bytes from the head; and opening a missing file WITHOUT O_CREAT fails. Returns 0 on
 * success, else the failing case number. */
int vfs_selftest(void) {
    const char* p = "/tmp/__vfskat";
    char buf[16];
    int fd, n;
    vfs_mkdir("/tmp", 0755);                        /* ensure the parent dir exists (idempotent) */

    /* 1. O_CREAT makes a new file; write 5 bytes into it. */
    fd = vfs_open(p, O_CREAT, 0);
    if (fd < 0) return 1;
    if (vfs_write(fd, "hello", 5) != 5) { vfs_close(fd); return 2; }
    vfs_close(fd);

    /* 2. Reopen read-only; the 5 bytes read back from the head. */
    fd = vfs_open(p, O_RDONLY, 0);
    if (fd < 0) return 3;
    n = vfs_read(fd, buf, sizeof buf);
    vfs_close(fd);
    if (n != 5) return 4;
    for (int i = 0; i < 5; i++) if (buf[i] != "hello"[i]) return 5;

    /* 3. O_CREAT|O_TRUNC empties the existing file. */
    fd = vfs_open(p, O_CREAT | O_TRUNC, 0);
    if (fd < 0) return 6;
    vfs_close(fd);
    fd = vfs_open(p, O_RDONLY, 0);
    if (fd < 0) return 7;
    n = vfs_read(fd, buf, sizeof buf);
    vfs_close(fd);
    if (n != 0) return 8;

    /* 4. A rewrite REPLACES the content (7 bytes). */
    fd = vfs_open(p, O_CREAT | O_TRUNC, 0);
    if (fd < 0) return 9;
    if (vfs_write(fd, "world!!", 7) != 7) { vfs_close(fd); return 10; }
    vfs_close(fd);
    fd = vfs_open(p, O_RDONLY, 0);
    n = vfs_read(fd, buf, sizeof buf);
    vfs_close(fd);
    if (n != 7) return 11;
    for (int i = 0; i < 7; i++) if (buf[i] != "world!!"[i]) return 12;

    /* 5. A SHORTER rewrite shrinks the size — no stale tail from the longer content. */
    fd = vfs_open(p, O_CREAT | O_TRUNC, 0);
    if (fd < 0) return 13;
    if (vfs_write(fd, "ab", 2) != 2) { vfs_close(fd); return 14; }
    vfs_close(fd);
    fd = vfs_open(p, O_RDONLY, 0);
    n = vfs_read(fd, buf, sizeof buf);
    vfs_close(fd);
    if (n != 2) return 15;
    if (buf[0] != 'a' || buf[1] != 'b') return 16;

    /* 6. Opening a missing file WITHOUT O_CREAT fails. */
    if (vfs_open("/tmp/__vfskat_absent", O_RDONLY, 0) >= 0) return 17;

    vfs_unlink(p);                                  /* cleanup */
    return 0;
}
