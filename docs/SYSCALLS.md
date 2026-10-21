# NyxOS System Call Reference

The kernel exposes a small POSIX-flavoured system-call ABI to ring-3 programs. This is the
contract the userland C library (`user/libc`) wraps and that any native NyxOS program — a
coreutil, a ported game, a compiler — is built on. There are **61 calls, numbered 0–60**;
the numbers are stable and defined in `kernel/core/kernel.h` (`SYS_*`).

## Calling convention

A program issues the `syscall` instruction with:

| Register | Meaning |
|----------|---------|
| `rax` | system-call number (`SYS_*`) |
| `rdi, rsi, rdx, r10, r8, r9` | arguments 1–6 (System V order, `r10` replacing `rcx`) |
| `rax` | return value on the way back (negative or `-1` on error) |

The kernel entry point is `syscall_handler(no, a1..a6)` in `kernel/core/syscall.c`, a switch
over `no`. `MSR_LSTAR`/`MSR_STAR`/`MSR_SF_MASK` are programmed per-CPU at boot so the
`syscall` fast path lands in ring 0 on this core's own kernel stack. Most programs never
issue `syscall` directly — libc provides thin wrappers (`open`, `read`, `fork`, `socket`, …).

## Process & scheduling

| # | Name | Sketch | Notes |
|---|------|--------|-------|
| 0 | `SYS_EXIT` | `exit(code)` | terminate the caller; status collected by `waitpid` |
| 6 | `SYS_GETPID` | `getpid()` | caller's pid |
| 47 | `SYS_GETPPID` | `getppid()` | parent's pid |
| 9 | `SYS_EXEC` | `exec(path)` | replace the image (simple form) |
| 13 | `SYS_EXECVE` | `execve(path, argv, envp)` | replace the image with args + environment |
| 10 | `SYS_FORK` | `fork()` | copy-on-write child; returns 0 in child, child pid in parent |
| 50 | `SYS_CLONE` | `clone(fn, stack, arg, flags)` | `CLONE_VM` thread sharing the address space |
| 11 | `SYS_WAITPID` | `waitpid(pid, *status, opts)` | block for a child to exit/stop, then reap |
| 16 | `SYS_KILL` | `kill(pid, sig)` | send a signal |
| 32 | `SYS_SLEEP` | `sleep(ms)` | block for whole milliseconds |
| 53 | `SYS_NANOSLEEP` | `nanosleep(req, rem)` | sub-second sleep |
| 7 | `SYS_SBRK` | `sbrk(delta)` | grow/shrink the heap break (lazy fault-in) |
| 27 | `SYS_GETPROCS` | `getprocs(buf, max)` | snapshot the process table (backs `ps`) |
| 33 | `SYS_SETFG` | `setfg(pid)` | set the foreground job (signal delivery target) |

### Foreground PID validation (6.5.230)

`setfg` accepts an existing process ID representable as an unsigned 32-bit
value, or exactly zero to clear the foreground target. Values outside that
range, including negative values passed through the 64-bit syscall argument,
return `-1` without changing the target. Unknown IDs also return `-1`.
The range check precedes narrowing: a high-bit value whose low bits name a
real process must not silently select that process.

The preceding 6.5.229 kernel accepts both `0x100000000 | self_pid` and
`0xffffffff00000000 | self_pid`, as reproduced by the actual ring-3 probe.
The correction validates the complete argument before assignment. It preserves
the existing zero-clear and userspace shell child/self handoff behavior.
This is numeric validation, not terminal ownership or permission enforcement;
foreground state remains global, and foreground GUI input responsiveness is
still separate work. Existing-process lookup is not a new lifetime guarantee.

The existing keyboard runner includes 48 ASan/UBSan checks of the actual syscall
case with process lookup stubs: high-bit aliases, negative encodings, exact
32-bit boundaries, missing IDs, zero clear and unchanged state on refusal.
The old syscall case fails the new assertion. Existing US/ES IRQ/poll control
and Escape checks, plus 369 timeout-command checks, continue to pass.

`exec /keyevtest.elf --foreground` runs the target PID-range probe.
`exec /keyevtest.elf --foreground-signal` additionally waits for Ctrl+C after
rejecting a high-bit alias of another existing PID; the signal must still reach
the caller. Without these options, the original raw make/break probe is unchanged.

The four-CPU QEMU GUI trial rejects all four invalid values, delivers one real
PS/2 Ctrl+C to the original caller, and returns to interactive shell input after
a child exits. The first script then stopped on an incorrect `mtdemo` output
marker; a focused continuation with the correct marker verifies increasing
thread counters, subsequent terminal input and window movement. Its two disk
fixtures remain byte-identical. Both VMs were closed. The packaged build has
zero GCC 14 warnings, passes 230 kernel self-tests and reaches four-CPU login.
These are bounded host and VM checks; no physical hardware trial is claimed.

## Files & directories (VFS)

| # | Name | Sketch | Notes |
|---|------|--------|-------|
| 3 | `SYS_OPEN` | `open(path, flags, mode)` | returns a per-process fd |
| 4 | `SYS_READ` | `read(fd, buf, n)` | bytes read, 0 at EOF |
| 1 | `SYS_WRITE` | `write(fd, buf, n)` | bytes written (fd 1/2 = console) |
| 5 | `SYS_CLOSE` | `close(fd)` | release an fd (last ref frees the node) |
| 8 | `SYS_FSIZE` | `fsize(fd)` | file size in bytes |
| 44 | `SYS_STAT` | `stat(path, *st)` | size + mode (dir/regular) |
| 45 | `SYS_FSTAT` | `fstat(fd, *st)` | as `stat` on an open fd |
| 46 | `SYS_LSEEK` | `lseek(fd, off, whence)` | `SEEK_SET`/`CUR`/`END` |
| 48 | `SYS_DUP` | `dup(oldfd)` | lowest free fd aliasing `oldfd` |
| 14 | `SYS_DUP2` | `dup2(oldfd, newfd)` | alias onto a chosen fd (pipe/redirect plumbing) |
| 12 | `SYS_PIPE` | `pipe(fds[2])` | a 4 KB blocking byte pipe |
| 15 | `SYS_GETDENTS` | `getdents(fd, buf, n)` | read directory entries |
| 21 | `SYS_CHDIR` | `chdir(path)` | set the per-process cwd |
| 22 | `SYS_GETCWD` | `getcwd(buf, n)` | absolute, normalised cwd |
| 23 | `SYS_MKDIR` | `mkdir(path)` | create a directory |
| 24 | `SYS_UNLINK` | `unlink(path)` | remove a file/dir link |
| 49 | `SYS_RENAME` | `rename(old, new)` | VFS move/rename |
| 25 | `SYS_TTYMODE` | `ttymode(raw)` | canonical vs raw single-byte stdin |

## Memory

| # | Name | Sketch | Notes |
|---|------|--------|-------|
| 19 | `SYS_MMAP` | `mmap(addr, len, prot, …)` | anonymous, demand-zero regions |
| 20 | `SYS_MUNMAP` | `munmap(addr, len)` | unmap a region |
| 26 | `SYS_MPROTECT` | `mprotect(addr, len, prot)` | change page protection |

## Signals

| # | Name | Sketch | Notes |
|---|------|--------|-------|
| 17 | `SYS_SIGNAL` | `signal(sig, handler)` | set a disposition (`SIG_DFL`/`SIG_IGN`/handler) |
| 18 | `SYS_SIGRETURN` | `sigreturn()` | return from a handler (issued by the libc trampoline) |
| 41 | `SYS_SIGPROCMASK` | `sigprocmask(how, set, old)` | block/unblock signals |
| 42 | `SYS_ALARM` | `alarm(secs)` | schedule a `SIGALRM` |

Signals are delivered at the return-to-ring-3 boundary. `SYS_KILL` (16) raises them.

## Networking (BSD sockets)

| # | Name | Sketch |
|---|------|--------|
| 34 | `SYS_SOCKET` | `socket(domain, type, proto)` — `SOCK_STREAM`/`SOCK_DGRAM` |
| 35 | `SYS_CONNECT` | `connect(fd, ip, port)` |
| 36 | `SYS_BIND` | `bind(fd, ip, port)` |
| 37 | `SYS_LISTEN` | `listen(fd, backlog)` |
| 38 | `SYS_ACCEPT` | `accept(fd)` |
| 39 | `SYS_SENDTO` | `sendto(fd, buf, n, ip, port)` |
| 40 | `SYS_RECVFROM` | `recvfrom(fd, buf, n, *ip, *port)` |
| 43 | `SYS_POLL` | `poll(fds, n, timeout)` |

Blocking socket calls yield to the scheduler between poll passes (they release `net_lock`
while waiting); the TCP receive window advertises real free space and caps the recv buffer
(see [NETWORK.md](NETWORK.md)).

## Time

| # | Name | Sketch |
|---|------|--------|
| 31 | `SYS_TIME` | `time()` — seconds since boot / epoch |
| 52 | `SYS_GETTIMEOFDAY` | `gettimeofday(*tv, tz)` — epoch seconds + microseconds (RTC, UTC) |

## Dynamic linking

| # | Name | Sketch |
|---|------|--------|
| 29 | `SYS_DLOPEN` | `dlopen(path)` — load a shared object |
| 30 | `SYS_DLSYM` | `dlsym(handle, name)` — resolve a symbol |

## Threads / synchronization

| # | Name | Sketch |
|---|------|--------|
| 50 | `SYS_CLONE` | `clone(fn, stack, arg, flags)` — a `CLONE_VM` thread (shares heap/VMAs via the thread-group leader) |
| 51 | `SYS_FUTEX` | `futex(uaddr, op, val)` — `FUTEX_WAIT` / `FUTEX_WAKE` for userland mutexes |

## Console, framebuffer & input

| # | Name | Sketch | Notes |
|---|------|--------|-------|
| 2 | `SYS_PRINT` | `print(str)` | convenience console write |
| 28 | `SYS_READKEY` | `readkey()` | next console key, independent of `ttymode` |
| 54 | `SYS_FBINFO` | `fbinfo(out[3])` | screen width/height/bpp for a fullscreen app |
| 55 | `SYS_FBPRESENT` | `fbpresent(buf, w, h)` | blit a 32bpp buffer to the screen, scaled |
| 56 | `SYS_GETKEYEVENT` | `getkeyevent()` | next raw key event `(pressed<<8)|scancode`, or −1 |
| 57 | `SYS_WIN_CREATE` | `win_create(w, h, title)` | open a `w×h` composited desktop window; returns an opaque handle (≥0) or −1; handles are not slot indices and are not reused within a boot |
| 58 | `SYS_WIN_DESTROY` | `win_destroy(id)` | close the window (idempotent — 0 even if already gone) |
| 59 | `SYS_WIN_PRESENT` | `win_present(id, buf, w, h)` | blit a `w×h` XRGB (`0x00RRGGBB`) buffer as the whole client area |
| 60 | `SYS_WIN_POLL_EVENT` | `win_poll_event(id, ev)` | pop one input event, non-blocking: `1` got / `0` none / −1 |
| 61 | `SYS_WIN_PRESENT_RECT` | `win_present_rect(id, buf, x, y, w, h)` | blit a `w×h` XRGB buffer into just the `(x,y)` sub-rectangle of the client area (damage-rect update) |

The framebuffer + raw-key-event trio (54–56) lets a fullscreen ring-3 program (the DOOM
port) own the whole screen and input. The windowing quartet (57–60) is the *windowed*
counterpart: a program calls `win_create` for a composited desktop window, blits its client
area with `win_present` (a whole-window XRGB buffer, capped at 2048×2048), pumps input with
the non-blocking `win_poll_event` (each event is four `int64` — a `kind` plus three payload
words), and closes it with `win_destroy`. `user/wintest.c` is the end-to-end example. The
kernel copies the title and pixel buffer across the boundary (bounds-checked before use), so
a handler never dereferences a user pointer.

As of development 6.5.226, both presentation syscalls check the live handle and
window dimensions before growing their scratch buffer or copying pixels. Whole
presentation requires the exact client size; a damage rectangle must fit inside
it. Invalid/stale handles and mismatched or out-of-bounds dimensions return `-1`
without that allocation or copy. The registry checks again at presentation time.
This preflight does not pin the window or add process ownership or synchronization
for concurrent resize/close. Valid presentations still retain reusable scratch
buffers; a later allocation or copy failure can still return `-1`.

`tools/test_userwin_present.py` runs the actual syscall bodies and registry with
sanitizers, checks rejected requests before and after valid drawing, and injects
allocation/copy failures. Target heap comparisons must let background child
reaping complete: the kernel shell advances one background task per iteration,
and an immediate sample may include an exited process awaiting cleanup.

---

## Raw stdin boundaries (6.5.234)

For an unredirected fd 0 in `TTY_RAW`, arrows and Home/End produce three-byte
ANSI escapes. The reader stops before fetching another key once the requested
byte count is filled. A per-process three-byte pending buffer preserves an
escape that does not fit. Requests of at least three bytes keep a newly encoded
escape whole, returning the preceding bytes first if necessary. Requests of one
or two bytes may split an escape; subsequent reads receive its remaining bytes.

`poll` reports fd 0 readable while raw bytes remain pending. Changing tty mode
discards pending bytes; setting the same mode again preserves them. Invalid modes
do not change state. Exec resets the pending count with the canonical discipline;
new processes and threads start with zeroed state. `readkey` and raw make/break
events retain their separate interfaces. Redirected descriptors still use their
file, pipe or socket implementation.

The existing `keyevtest --raw-reads` diagnostic waits six seconds after each
ready message, then checks the following buffered PS/2 sequences. Type no Enter
between the listed keys; `z` is the final ordinary character in each case.

| Read capacity | Keys after the ready message |
|---|---|
| 1 | Up, z |
| 2 | Down, z |
| 3 | Right, z |
| 4 | Left, a, z |
| 16 | Up, Down, Right, Left, Home, End, z |

The host canonical/raw harness checks capacities 1 through 6, guard bytes,
pending readiness, process isolation, mode changes and syscall scratch-state
restoration under sanitizers. This does not give each terminal an input queue.
The separate keyboard ordering defect found during that trial is corrected in
6.5.235: one cooked PS/2 queue holds bytes and navigation/function keys in arrival
order. IRQ reads and polling fallback hold the same lock through decode and enqueue.
Buffered navigation also counts as input for `poll` and the compositor's idle check.

Byte-only readers (including login and canonical stdin) consume and ignore navigation
keys; they cannot leave an old arrow queued for a later GUI client. Internal byte tags
preserve Spanish high-bit layout bytes in this path. The public `readkey` integer
encoding is unchanged and is not a Unicode interface. Physical make/break events
remain independent; this change does not add polling-fallback events to that API.

`exec /keyevtest.elf --key-order` checks mixed sequences at capacities 1, 2, 3, 4
and 16. After each ready message enter respectively: `a, Up, z`; `a, Down, z`;
`a, Right, z`; `a, Left, z`; then `a, Up, b, Down, c, Right, d, Left, e, Home,
f, End, z`. Do not type Enter. The six-second pause lets the keys queue first.

The cooked queue holds 255 entries shared by bytes and navigation; new entries
are dropped when it is full. There is no lossless-input guarantee or per-terminal
input ownership. Serial remains a fallback after queued PS/2 input; no global
arrival order between the two devices is promised.

*Cross-references: [ARCHITECTURE.md](ARCHITECTURE.md) (privilege model), [PROCESS.md](PROCESS.md)
(scheduling, fork/exec, signals), [FILESYSTEM.md](FILESYSTEM.md) (the VFS), [NETWORK.md](NETWORK.md)
(the socket stack). Numbers are authoritative in `kernel/core/kernel.h`; behaviour lives in
`kernel/core/syscall.c`.*
