#include "../core/kernel.h"

// ============================================================
// Anonymous pipes — a unidirectional byte stream between processes.
// Each pipe is a fixed ring buffer with a read end and a write end, each
// reference-counted by the number of fds pointing at it (fork() bumps them,
// close()/exit drop them). A read on an empty pipe BLOCKS until a writer writes
// (or all writers close → EOF); a writer wakes all blocked readers. Blocking is safe
// mid-syscall thanks to per-process syscall stacks (v5.8.2): see pipe_read.
// ============================================================

#define PIPE_BUF_SIZE 4096
#define MAX_PIPES     16

typedef struct {
    int used;
    uint64_t generation;
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head;               // read cursor
    uint32_t count;              // bytes currently buffered
    int read_refs;               // # of fds on the read end
    int write_refs;              // # of fds on the write end
} pipe_t;

static pipe_t pipes[MAX_PIPES];

// Allocate a fresh pipe (one read ref + one write ref). Returns its id or -1.
int pipe_new(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            uint64_t generation = pipes[i].generation + 1;
            memset_asm(&pipes[i], 0, sizeof(pipe_t));
            pipes[i].generation = generation;
            pipes[i].used = 1;
            pipes[i].read_refs = 1;
            pipes[i].write_refs = 1;
            return i;
        }
    }
    return -1;
}

// Add a reference to one end of a pipe (fork() inheriting a pipe fd).
void pipe_incref(int id, int is_write) {
    if (id < 0 || id >= MAX_PIPES || !pipes[id].used) return;
    if (is_write) pipes[id].write_refs++;
    else          pipes[id].read_refs++;
}

// Process-owned wait markers avoid retaining pointers after direct kill/reap.
// Like pipe read/write, this scan relies on the current BSP-only user execution.
static void pipe_wake_readers(int id) {
    for (int i = 0; i < process_count; i++) {
        process_t* reader = process_table[i];
        if (!reader || reader->pipe_wait_id != (uint32_t)(id + 1)) continue;
        reader->pipe_wait_id = 0;
        if (reader->state == PROC_BLOCKED) reader->state = PROC_RUN;
    }
}

// Drop one reference from an end. Closing the last writer wakes all blocked readers
// so they observe EOF; when both ends reach zero the pipe is freed.
void pipe_close_end(int id, int is_write) {
    if (id < 0 || id >= MAX_PIPES || !pipes[id].used) return;
    pipe_t* p = &pipes[id];
    if (is_write) {
        if (p->write_refs > 0) p->write_refs--;
        if (p->write_refs == 0) pipe_wake_readers(id);
    } else {
        if (p->read_refs > 0) p->read_refs--;
    }
    if (p->read_refs == 0 && p->write_refs == 0) p->used = 0;
}

// Non-blocking write: copy as much of src (up to n) as fits, wake blocked
// readers. Returns bytes written, or -1 if the read end is gone (broken pipe).
// Runs in a syscall (interrupts masked), so it's atomic against readers.
int pipe_write(int id, const char* src, int n) {
    if (id < 0 || id >= MAX_PIPES || !pipes[id].used || n < 0) return -1;
    pipe_t* p = &pipes[id];
    if (p->read_refs == 0) return -1;                 // EPIPE: nobody will read
    int written = 0;
    while (written < n && p->count < PIPE_BUF_SIZE) {
        uint32_t tail = (p->head + p->count) % PIPE_BUF_SIZE;
        p->buf[tail] = (uint8_t)src[written++];
        p->count++;
    }
    if (written > 0) pipe_wake_readers(id);
    return written;
}

// Blocking read into kbuf (up to n). Blocks while the pipe is empty and at least
// one writer is still open; returns 0 (EOF) once it's empty with no writers, or
// the number of bytes drained. Mirrors do_waitpid's mid-syscall blocking rules:
//  - blocked_in_kernel makes the scheduler resume us on the KERNEL CR3 (we sleep
//    in ring 0, whose -mcmodel=large code is only mapped there);
//  - other processes' syscalls clobber the shared user_cr3/user_rsp globals while
//    we sleep, so we save them on entry and restore them before returning so the
//    caller's copy_to_user + the asm return path target this process.
// poll(): 1 if pipe_read(id) would return immediately — buffered data, or EOF
// because all writers have closed. Called with interrupts masked (a syscall).
int pipe_readable(int id) {
    if (id < 0 || id >= MAX_PIPES || !pipes[id].used) return 1;   // invalid -> read errors
    return pipes[id].count > 0 || pipes[id].write_refs == 0;
}

int pipe_read(int id, char* kbuf, int n) {
    if (id < 0 || id >= MAX_PIPES || !pipes[id].used || n <= 0) return -1;
    pipe_t* p = &pipes[id];
    uint64_t generation = p->generation;
    process_t* self = get_current_process();
    uint64_t saved_cr3 = user_cr3, saved_ursp = user_rsp;
    int got = 0;
    for (;;) {
        __asm__ volatile("cli");
        if (self) self->blocked_in_kernel = 0;        // past any block (interrupts off)
        /* Signal wakeups bypass the pipe scan; detach before stop or return. */
        if (self) self->pipe_wait_id = 0;
        /* Another thread can close both shared fds and reuse this slot while the read sleeps. */
        if (!p->used || p->generation != generation) { got = -1; break; }
        if (p->count > 0) {                           // data available — drain up to n
            while (got < n && p->count > 0) {
                kbuf[got++] = (char)p->buf[p->head];
                p->head = (p->head + 1) % PIPE_BUF_SIZE;
                p->count--;
            }
            break;
        }
        if (p->write_refs == 0) { got = 0; break; }   // empty + no writers => EOF
        if (!self) break;                             // no process context: don't block
        if (signal_pending(self)) {
            if (signal_check_stop(self)) continue;
            got = -EINTR;
            break;
        }
        // Empty but writers remain — park until one writes (or closes).
        self->blocked_in_kernel = 1;                  // resume us on the kernel CR3
        self->state = PROC_BLOCKED;
        self->pipe_wait_id = (uint32_t)(id + 1);
        __asm__ volatile("sti; hlt");
        // Resumed on the kernel CR3 — loop, clear the flag, re-check (interrupts off).
    }
    user_cr3 = saved_cr3;
    user_rsp = saved_ursp;
    return got;
}

// KAT (`pipe`): the ring-buffer read/write path behind every shell pipeline. Nothing
// else pins the `% PIPE_BUF_SIZE` wraparound, the fill-to-full cap, or the EOF/EPIPE
// edges, and a regression there would silently corrupt piped data. Non-blocking by
// construction: every read drains only what is buffered (never empty-with-writers),
// EOF is checked after closing the write end, EPIPE after closing the read end.
// Buffers are static (the 4 KB pattern is far larger than the 4 KB kernel stack).
int pipe_selftest(void) {
    static char buf[PIPE_BUF_SIZE + 8];
    static char big[PIPE_BUF_SIZE + 100];
    int id = pipe_new();
    if (id < 0) return 1;

    // 1. round-trip
    if (pipe_write(id, "hello", 5) != 5) return 2;
    if (pipe_read(id, buf, 5) != 5) return 3;
    for (int i = 0; i < 5; i++) if (buf[i] != "hello"[i]) return 4;

    // 2. partial reads: write 10, read 3 then 7
    if (pipe_write(id, "0123456789", 10) != 10) return 5;
    if (pipe_read(id, buf, 3) != 3 || buf[0] != '0' || buf[2] != '2') return 6;
    if (pipe_read(id, buf, 7) != 7 || buf[0] != '3' || buf[6] != '9') return 7;

    // 3. fill-to-full + WRAPAROUND: head is now mid-buffer, so writing PIPE_BUF_SIZE
    //    bytes wraps the tail; the excess is dropped (buffer is full); reading it all
    //    back must reproduce the pattern in order across the wrap.
    for (int i = 0; i < PIPE_BUF_SIZE + 100; i++) big[i] = (char)(i & 0xFF);
    if (pipe_write(id, big, PIPE_BUF_SIZE + 100) != PIPE_BUF_SIZE) return 8;
    if (pipe_write(id, "x", 1) != 0) return 9;                 // full -> 0 written
    if (pipe_read(id, buf, PIPE_BUF_SIZE) != PIPE_BUF_SIZE) return 10;
    for (int i = 0; i < PIPE_BUF_SIZE; i++)
        if ((uint8_t)buf[i] != (uint8_t)(i & 0xFF)) return 11;

    // 4. EOF: empty pipe with the write end closed reads 0, not a block
    pipe_close_end(id, 1);
    if (pipe_read(id, buf, 8) != 0) return 12;
    pipe_close_end(id, 0);                                     // frees the pipe

    // 5. EPIPE: writing to a pipe whose read end is gone returns -1
    int id2 = pipe_new();
    if (id2 < 0) return 13;
    pipe_close_end(id2, 0);
    if (pipe_write(id2, "x", 1) != -1) { pipe_close_end(id2, 1); return 14; }
    pipe_close_end(id2, 1);
    return 0;
}
