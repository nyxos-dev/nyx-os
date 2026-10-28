# NyxOS Process & Scheduling Model

How NyxOS represents, runs, and retires processes. Every structure and constant
below is grounded in the source — the header offsets are in
[`kernel/core/kernel.h`](../kernel/core/kernel.h), the lifecycle logic in
[`kernel/proc/process.c`](../kernel/proc/process.c), and the system-call surface
in [`kernel/core/syscall.c`](../kernel/core/syscall.c).

This complements [ARCHITECTURE.md](ARCHITECTURE.md) (the whole-system view) and
[SECURITY.md](SECURITY.md) (the privilege boundary as a security property).

---

## 1. The process table

Every task — kernel thread or user process — is a `struct process` (`process_t`)
held in a fixed global array:

```c
#define MAX_PROCESSES 512
process_t* process_table[MAX_PROCESSES];   // dense: slots 0..process_count-1
```

The table is **dense**: exiting tasks are swap-removed and `process_count` shrinks,
so iterators walk `0..process_count-1` with no NULL gaps in the middle (the
`ps`, `pgrep`, and `jobs` commands all share this loop). A task is found by pid
with `find_process(pid)`.

Key `process_t` fields:

| Field | Meaning |
|-------|---------|
| `pid`, `ppid` | process id and parent id |
| `uid`/`euid`, `gid`/`egid` | real/effective user and group ids |
| `comm[32]` | short name (shown by `ps`; set from the program path) |
| `cmdline[256]` | full space-joined argv |
| `page_directory` | the process's PML4 (address space). **NULL ⇒ kernel thread** |
| `stack`, `kernel_stack` | ring-3 and ring-0 stacks |
| `state` | one of the `PROC_*` states below |
| `files[256]`, `ufd_*[32]` | open handles; the per-process fd table (`PROC_MAX_FDS = 32`) |
| `program_break`, `heap_start` | the lazy-`sbrk` heap window |
| `mmap_vmas[]`, `mmap_next` | anonymous `mmap` regions |
| `cwd[MAX_PATH]` | per-process current directory (relative paths resolve here) |
| `sched_managed`, `sched_weight`, `sched_quantum`, `sched_cpu` | scheduler bookkeeping (§5) |
| `tgid` | thread-group leader pid (§6) |
| `sig_pending`, `sig_mask`, `sig_handlers[NSIG]`, … | signal disposition (§7) |
| `exit_code` | status passed to `exit()`, collected by `waitpid()` |

---

## 2. Process states

```c
#define PROC_PARKED  0   // not runnable (a retired kernel thread); scheduler skips it
#define PROC_RUN     1   // runnable or running
#define PROC_ZOMBIE  2   // exited, awaiting reap_zombies() / waitpid()
#define PROC_BLOCKED 3   // blocked in kwait()/sleep(); scheduler skips it
#define PROC_STOPPED 4   // job-control stopped (SIGTSTP/SIGSTOP); parked until SIGCONT
```

The scheduler only considers `PROC_RUN` tasks; `PARKED`, `BLOCKED`, and `STOPPED`
are skipped, and `ZOMBIE` tasks are dead but not yet freed.

---

## 3. Privilege model: kernel threads vs user processes

The single distinction that matters for safety is **`page_directory`**:

- **Kernel tasks** (`page_directory == NULL`) — the PID 1 bootstrap record,
  `idle`, the per-core `apworker` tasks, and the `compositor` use the shared
  kernel address space. The PID 1 record named `init` has no runnable stack;
  the actual kernel threads execute in **ring 0**. They have no user stack to free.
- **User processes** (`page_directory != NULL`) run in **ring 3** with their own
  PML4. A fault in ring 3 kills only that process; the kernel keeps running.

This is why `kill` and `pkill` **refuse a task with `page_directory == NULL`**:
tearing a kernel thread's stack out from under the scheduler would crash the
system. Both commands share that exact guard, so neither can take down `init`,
`idle`, `compositor`, or an `apworker`.

---

## 4. Lifecycle: create → run → exit → reap

### Boot ownership and the current `init` limit

The present startup sequence is:

```text
Firmware → GRUB → assembly bootstrap → kernel_main
  → memory, paging, interrupts, devices and process manager
  → initramfs and shared libc
  → persistent filesystem mount
  → account setup and login → compositor desktop
```

`kernel_main` performs these steps. With no framebuffer, it enters the kernel
shell instead of the graphical login. PID 1 is a kernel bootstrap record named
`init`; it is not a userspace service supervisor. Background cleanup is performed
by the existing kernel `reap_zombies` callback, while live user parents collect
their children through `waitpid`.

`user/init.c`, packaged as `/init.elf`, is a manual demonstration of libc,
fork/wait, pipes, signals and other system calls. Run it using `exec /init.elf`.
It prints its results and exits; it does not launch login or the desktop. Before
development version 6.5.250, boot loaded a separate copy but never marked it
scheduler-managed. That process retained its address space and stack without
running. Boot now leaves the demonstration on disk until explicitly launched.

A real userspace init remains planned work: define service configuration and
startup order, adopt and reap orphans, bound restart attempts, expose failures,
and coordinate shutdown with filesystem flushing. Moving login or the compositor
under that supervisor requires an explicit handoff and failure recovery; the
current demonstration is not that handoff.

```
 spawn /prog.elf ──► elf_validate ──► elf_load_args ──► PROC_RUN ──► exit()
                                                              │
                                                    PROC_ZOMBIE (exit_code kept)
                                                              │
                          ┌───────────────────────────────────┴───────────────┐
                 parent is a user process                    parent is a kernel thread
                 → waitpid() collects it                     (a `spawn` background job)
                   (do_waitpid reaps)                        → reap_zombies() auto-frees
```

**Creation.** `spawn_user_path_args(path, argv, argc)` opens the ELF, copies it
off the VFS, runs it through `elf_validate` (see [SECURITY.md](SECURITY.md)),
loads it with `elf_load_args` (seeding argv on the new stack), names it after the
path, sets `sched_managed = 1`, records `ppid`, and enables the scheduler. Userland
ELFs live at root paths, so it is `spawn /spin.elf`, not `spawn spin` — `vfs_open`
has no search path. The `fork(2)`, `execve(2)`, and `clone(2)` system calls
(`SYS_FORK=10`, `SYS_EXECVE=13`, `SYS_CLONE=50`) build processes the same way from
inside ring 3.

**Exit and reap.** On `exit()` a process becomes a `PROC_ZOMBIE` holding its
`exit_code`, so a parent can still read the status. `reap_zombies()`
([process.c](../kernel/proc/process.c)) then frees the address space, kernel
stack, `process_t`, and table slot — but only for **orphans and background jobs**
(a `spawn` job whose parent is a kernel thread). A zombie whose parent is a live
user process is **left for that parent's `waitpid()`** to collect. Reaping runs in
the compositor's background slot (never in the IRQ path), because it calls
`kfree`/`free_page_directory`.

> A consequence worth knowing: a background job killed with `pkill`/`kill` stays a
> zombie in `ps`/`pgrep` output for a short window until the background reaper runs —
> exactly like a `<defunct>` process on Linux. A second `pkill` on it then hits the
> `page_directory == NULL` guard (its address space is already gone) and safely
> refuses, so nothing is double-freed.

---

## 5. The scheduler

NyxOS runs a preemptive scheduler, enabled by `sched_enable()` once user
processes exist. The BSP uses weighted round-robin; APs use round-robin.

- **Weighted round-robin.** Each task gets `sched_weight` ticks per turn
  (`sched_quantum` counts down the current turn). On the BSP, `nice`/`renice`
  set the weight (clamped to 1..64; higher = more CPU). Changes affect the next
  turn and leave the current quantum intact. This is what the `mtdemo` self-test
  demonstrates: a thread with 3× the weight counts ~3× faster.
- **Per-CPU pinning (SMP).** `sched_cpu` fixes which core may run a task: `0`
  (the default) means the BSP, `N > 0` means AP #N exclusively. Because no other
  core will select a task with a different pin, changing a pin requires holding
  the scheduler lock and excluding both selected and physically active CPU
  contexts and lazy FPU ownership. `taskset` refuses such a change while the
  process retains CPU state or is not yet scheduler-managed. FPU state can remain
  on a CPU after a task blocks, until another user task runs there and saves it.
  Kernel-thread pins are fixed. Processes sharing an address space cannot be
  moved individually. Requesting a user process's existing pin is a no-op.
  The `apworker1..N` kernel threads are the per-AP idle/work tasks.
- **`sched_managed`.** Only tasks with this flag are round-robined. A blocking
  `exec` and unstarted processes leave it `0` so the scheduler skips them.
- **Blocking.** `sleep()` sets `wake_tick`; `kwait()` sets `waiting_for` and the
  `PROC_BLOCKED` state; `FUTEX_WAIT` parks on a `futex_key`. Each is woken from
  the corresponding kernel path and returns to `PROC_RUN`.

---

## 6. Threads (CLONE_VM) and thread groups

`clone(fn, stack, arg, flags)` (`SYS_CLONE`) with `CLONE_VM` creates a **thread**
that shares its creator's address space. Shared state (heap via `sbrk`, `mmap`
regions) is owned by the **thread-group leader**, identified by `tgid` (the
leader's pid; `0` or `== pid` means the task *is* the leader). Any thread's
`sbrk`/`mmap` is seen by all of them because they defer to `tg_leader()`. When a
member is reaped, `tg_reassign_leader()` keeps the group's shared state owned by a
surviving member.

---

## 7. Signals and job control

Signals are delivered at **return-to-ring-3** (`signal_dispatch`, off the syscall
path), never asynchronously inside the kernel:

- `sig_pending` / `sig_mask` / `sig_active` are per-signal bitmasks; `sig_handlers[NSIG]`
  holds each disposition (`SIG_DFL`, `SIG_IGN`, or a ring-3 handler VA).
- On entering a handler the kernel saves the ring-3 context in `sig_saved[18]`;
  `SYS_SIGRETURN` restores it via the libc `sig_trampoline`.
- `alarm(2)` arms `alarm_tick`; the scheduler tick posts `SIGALRM` once it passes.
- **Job control:** `SIGTSTP`/`SIGSTOP` move a task to `PROC_STOPPED` (recorded in
  `stop_sig`); `SIGCONT` resumes it. `waitpid(..., WUNTRACED)` reports the stop.

`kill(pid, sig)` (`SYS_KILL`) posts a signal; `waitpid` (`SYS_WAITPID`, with the
`WNOHANG` option) collects children.

---

## 8. Per-process resources

- **File descriptors** — a private table of `PROC_MAX_FDS = 32` small-int fds, each
  mapping to an internal VFS handle + byte offset. Isolated per process and closed
  on reap, so fds neither leak nor cross processes.
- **Working directory** — `cwd` is per process, inherited on `fork`, kept across
  `execve`. Relative paths in `open`/`getdents` resolve against it (see
  [`vfs_realpath`](../kernel/fs/vfs.c) and the `realpath` command).
- **Heap** — `sbrk` moves `program_break`; the window `[heap_start, program_break)`
  is faulted in lazily, one zeroed page per first touch.
- **Anonymous mmap** — `mmap_vmas[]` regions fault in fresh zeroed pages at the
  mapping's protection.

---

## 9. The command surface

| Command | What it does |
|---------|--------------|
| `ps` | list every task: pid, ppid, state, name |
| `pgrep [-l] <pat>` | print pids whose name contains `<pat>` (`-l` adds names) |
| `pkill <pat>` | terminate matching processes (kernel threads refused) |
| `kill <pid>` | terminate one process by pid (kernel threads refused) |
| `jobs` | the shell's scheduler-managed background jobs |
| `wait [pid]` | block until a background job (or all) exits |
| `nice`, `renice` | set a task's scheduler weight (1..64) |
| `top` | live process/CPU view |
| `spawn /prog.elf` | run a user ELF in the background |
| `exec <file>` | run a user ELF to completion (foreground) |
| `free`, `pmap` | memory usage; a process's address-space map |

---

### Command output capture (6.5.231)

The kernel terminal hook and the kernel shell's 4096-byte pipe capture each have
an owner PID and a monotonically assigned capture identifier. User-process
creation, `fork` and `clone` inherit the creator's identifiers; `execve` keeps
them. A capture accepts its owner and descendants carrying its current identifier.
An older background task therefore cannot append to a later command's terminal
capture or kernel pipe. Identifier exhaustion does not recycle an old identifier:
only the owner remains eligible. These identifiers are routing state, not access
control credentials.

An IRQ-saving lock serializes capture changes, pipe appends and hook callbacks.
Removing a hook waits for an in-flight callback; the callback must not yield,
print recursively or call capture management functions. The existing terminal
callback meets that requirement. This does not serialize every framebuffer draw.

Output excluded from both captures still goes to serial/VGA. Since 6.5.233 it
also enters the originating terminal's session queue while that session remains
open. Input ownership and asynchronous foreground execution remain future work;
foreground jobs can still stall desktop input. The kernel pipe retains its
4096-byte capacity; since
6.5.232, overflow aborts the consumer instead of passing truncated output (see
[SHELL_ENTRY.md](SHELL_ENTRY.md)). Descriptor-based userspace pipes/redirection
retain their existing semantics.

The existing `threads` diagnostic has `--capture` for stdout, stderr, legacy print,
clone, fork and exec output. Its separate `--capture-wait` probe emits a marker
after observing a newer process named `sleep`, allowing a bounded two-terminal
contamination test without depending on host typing speed. The host terminal
runner checks expired identifiers, descendants, serial fallback, pipe bounds and
hook removal with a pthread mutex standing in for the IRQ-saving lock. That host
model is not a proof of hardware interrupt or general SMP correctness.

### Terminal output sessions (6.5.233)

Each Erebus window owns a numeric session. Process creation, fork and clone
inherit the session; exec retains it. Producers retain no window pointer. Closing
the window invalidates its session under the capture lock, so surviving children
cannot write into a later window reusing its slot. Output still reaches serial
after closure. Session identifiers never wrap or recycle; exhaustion rejects a
new terminal instead of reconnecting stale producers.

| Bound | Value | Behaviour |
|---|---|---|
| Concurrent sessions | 32 | Fixed slots in `screen.c`; allocation fails when full |
| Queue per session | 8192 bytes | Static storage; producers do not allocate or wait for a reader |
| One GUI drain | 512 bytes | Tick/draw consumes pending output when that terminal is not capturing |
| Omitted counter | Saturating 32-bit unsigned | Reported after the retained prefix drains |

When a queue fills, it preserves its prefix and discards subsequent bytes until
that prefix drains. Erebus then resets that window's ANSI parser and displays an
explicit omitted-byte warning. Serial retains the output. This is bounded output
loss, not producer backpressure or unlimited scrollback. Hidden windows and a
terminal running a synchronous foreground command can accumulate queued output.
Each window owns its ANSI parser and displays partial lines without modifying
its editable input. Descriptor redirection and pipe capture take precedence over
session routing; foreground capture keeps its existing synchronous callback.

### Foreground terminal repainting (6.5.253)

The kernel's synchronous `exec` and `timeout` waits still yield for 60 ms per
iteration. `compositor_foreground_tick` now uses the terminal capture revision
to avoid painting an unchanged terminal scene on every iteration. New captured
output is painted at the next pump; otherwise desktop indicators refresh after
500 ms. The static welcome card also qualifies. Visible nonterminal renderers,
animated wallpaper and notifications keep the previous unconditional path.
Explicit forced redraws, including process exit, remain available.

This reduces repeated software rendering while canonical or raw terminal reads
wait for input. It does not change input ownership, make foreground execution
asynchronous, or qualify the performance of other applications. Source:
`kernel/gui/core/compositor.c`, `kernel/gui/apps/terminal_win.c` and the two
foreground loops in `kernel/core/kernel.c`. `tools/test_foreground_redraw.py`
executes the gate and capture code with mocked time and rendering; actual CPU
and GUI evidence is scoped in [DAILY_USE.md](DAILY_USE.md).

## 10. Limits and non-goals

- Up to **512** tasks and **32** open fds per process — fixed arrays, no dynamic
  growth. These are generous for an experimental desktop OS, not tuned for server
  fan-out.
- The scheduler is a straightforward weighted round-robin with static per-CPU
  pinning — no priority inheritance, no load balancing across cores, no cgroups.
- `#40` (FPU/SSE `XMM` state is not saved across context switches) is a **known,
  latent** limitation — harmless today because the kernel is built `-mno-sse`; see
  [SECURITY.md](SECURITY.md) and the issue tracker.

NyxOS is an experimental system; this model favours clarity and a small, auditable
core over completeness.
