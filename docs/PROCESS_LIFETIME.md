# Process collection and CPU handoff

Development version 6.5.257 delays collection until the outgoing CPU has
installed another stack and address space. It also serializes collector claims
and scheduler selection. Userspace init and general SMP resource ownership
remain separate work.

## Reproduced failures

Version 6.5.256 retires a collected child's per-CPU FPU references, closing a
stale-owner path from 6.5.255. Its AP-parent test still loses the kernel-shell
prompt after the parent exits. Inspecting the stopped VM shows CPU 0 halted on
its child's already-freed kernel stack, with its process-table cursor beyond
the remaining entries. Clearing a pointer does not prove that its CPU has
stopped using the underlying resources.

The exit path marks a task zombie before the next timer interrupt switches it
out. A parent on another core can observe that state immediately. Even after
the scheduler selects a replacement, its C function still executes on the
outgoing stack until IRQ assembly installs the new RSP and CR3.

## Lifetime contract

| Field or operation | Contract |
|---|---|
| `sched_cur` | Selected task; published under `sched_lock`, before the physical switch |
| `sched_active` | Outgoing task until the IRQ stub installs replacement RSP and CR3; boot-to-compositor identity transfer explicitly initializes it too |
| `reap_claimed` | One collector claims an inactive zombie under `sched_lock`; another collector cannot release the same task |
| `reap_claim_locked` | Rejects collection while any CPU selects or physically occupies the task |
| `sched_forget` | Clears selected/FPU references under the lock before freeing the process object; never acknowledges physical handoff |
| `current_idx` | BSP scan cursor; current-task identity comes from the CPU pointer after bootstrap |
| `publish_process` | Rechecks capacity and appends under the same lock used by selection and removal |

The BSP and AP schedulers retain the lock through target/FPU publication.
The BSP switches to idle when the outgoing task is no longer runnable and no
ordinary task can run. Boot explicitly adopts the compositor identity and
retires the initial placeholder's saved boot frame, so two entries cannot
resume that context.

Waiters publish their waiting condition under the same lock as exit wakeups.
If a child has exited but its CPU has not completed handoff, `do_waitpid`
remains runnable, halts until an interrupt and checks again. It does not rely
on a fixed grace period or a second exit notification. `WNOHANG` reports zero
while collection is unsafe. Exit codes, stop reporting, null status pointers
and syscall return context retain their contracts.

The claimed task stays in the table during cleanup. Heap, file and page-table
cleanup occur outside `sched_lock`; final compaction and FPU retirement occur
inside it. Allocation failure after a full-table recheck unwinds the
unpublished task's resources. PID allocation and balanced-placement counters
use atomic increments.

## Verification

| Layer | Evidence on 6.5.257 |
|---|---|
| Host collection | 802 sanitizer checks; preceding source compiles and runs the regression with 28 failures |
| Host scheduling | 43 checks of actual BSP/AP selection, stale cursor, blocked/zombie idle fallback, boot adoption and full-table publication; preceding source fails seven checks |
| Kernel | Warning-free build, 230 kernel self-tests and four-CPU login |
| Serial QEMU | Immediate child collection from parents on CPUs 0, 1, 2 and 3 returns status 73, followed by the kernel prompt and a successful command; no retained child FPU owner |
| Exception lifecycle | Seven existing `threads --faults` cases pass, then normal threads and a responsive shell; five deliberate exception records are expected |
| GUI | Two thread runs preserve shared counter, heap and descriptors; 20 fork/exec/pipe rounds report no faults; socket aliases, scheduler heartbeats, window movement and a later command pass |
| Disk controls | Unrelated 10-byte and 24-byte files remain exact |

Serial tests use four qemu64 CPUs, 512 MiB and disposable 32 MiB EXT2 disks.
GUI tests use four CPUs, 5 GiB and a separate disposable disk. All test VMs
close. Host tests mock interrupts, CPU state and resource cleanup. They model
specific interleavings, not arbitrary concurrent execution; target tests cover
the actual assembly handoff.

## Desktop CPU comparison

A sequential 6.5.256/6.5.257 control uses the same four-CPU, 5 GiB QEMU/TCG
configuration and fresh disposable disks. Each phase has three eight-second
samples after settling. Host values are the median QEMU process CPU time
normalized to one host core; guest values are the BSP indicator range. These
are different quantities and neither describes all hardware workloads.

| Phase | Host 6.5.256 | Host 6.5.257 | Guest BSP 6.5.257 |
|---|---|---|---|
| terminal-idle | 39.0% | 35.8% | 0–0% |
| start-menu-idle | 34.9% | 35.5% | 0–1% |
| context-menu-idle | 34.7% | 36.1% | 0–0% |
| stationary-button-held | 35.0% | 36.0% | 0–0% |

This correction does not claim a CPU optimization. The controls retain modest
host variation and a near-zero guest idle indicator; they do not reproduce a
sustained 100% host-core load. Future desktop effects still require their own
paired measurements.

## PID signal posting (6.5.267)

`do_kill` now retains `sched_lock` from PID lookup through signal posting.
An absent or collector-claimed target returns -1, including for signal zero.
A zero signal succeeds without changing an existing, unclaimed process;
an unclaimed zombie still exists until collected. Posting does not resurrect
a zombie. Invalid signal numbers are rejected before acquiring the lock.

Keyboard foreground delivery uses the same operation. A zero foreground PID
does nothing. The keyboard releases its input lock before calling the signal
path, and the scheduler lock saves and restores the caller's interrupt state.
No allocation, user-memory access or cleanup occurs during this operation.
After unlocking, the operation retains no process pointer.

The lower-level `signal_raise` deliberately does not acquire the lock. Its
caller must own the process lifetime or already hold `sched_lock`, as the
scheduler's alarm path does. Stopped tasks resume for SIGCONT or SIGKILL;
blocked tasks retain their existing wakeup behaviour.

`tools/test_signal_lifetime.py` runs the actual lookup and posting bodies with
controlled collector events. It passes 283 checks under ASan/UBSan, covering
invalid signals, all 31 pending bits, existence probes, claimed targets,
blocked/stopped/zombie states, foreground routing and the already-locked
alarm caller. The collector can free the object immediately after unlock;
the caller must not access it again. The same test on 6.5.266 reproduces a
heap-use-after-free when collection wins after an unprotected lookup. This
is a host interleaving model, not a newly reproduced guest crash.

The warning-free candidate passes 230 kernel tests and four-CPU startup.
In QEMU 10.2.1 TCG with four CPUs, 5 GiB and default placement, a ring-3 probe
checks existence while running, as an uncollected zombie and after collection
for 32 children. Installed `keyevtest` accepts one PS/2 Ctrl-C delivery and
passes nine canonical/raw/readkey signal-interruption, stop/continue and
stopped-kill workflows. Installed `alarmdemo` catches its scheduler alarm.
Both `mtdemo` tasks finish three heartbeats; three AP workers run, allocator
stress reports no integrity errors and balances on the second run, and a
later command and terminal drag succeed. Offline inspection preserves two
control files byte-for-byte, and the disposable VM closes. These guest checks
do not force the host model's collector interleaving or qualify AP-balanced
fork/exec churn. The pipe/sleep/futex and keyboard host regressions also pass.

This protects object lifetime for PID-based posting. It does not make every
signal field atomic, serialize all delivery-state transitions, implement
process groups or change foreground ownership. Other raw kernel shell readers
still require separate audits. Development 6.5.268 protects Monitor and kernel
`ps` with owned records; see [PROCESS_SNAPSHOT.md](PROCESS_SNAPSHOT.md).

## Stack-canary scan lifetime (6.5.269)

`stackcheck` reads task canaries while holding `sched_lock`, excluding
collector-claimed entries and tasks without a kernel stack. Excluding a claimed
entry is necessary even under the lock: its collector frees the stack before
reacquiring the lock to remove the process-table entry.

The scan reserves a 2 KiB buffer for up to 512 damaged-stack PID values before
locking. It releases the lock before formatting warnings or freeing that buffer;
reporting retains no process or stack pointer. If allocation fails, the command
prints `stackcheck: out of memory; scan not performed`. It does not report a
successful inspection. The canary word, allocation stamping and overflow
detection remain unchanged.

`tools/test_stack_lifetime.py` passes 1,624 ASan/UBSan checks of the actual scan
and shell reporting bodies. Cases cover intact and damaged stacks, null entries,
missing stacks, collector-claimed freed stacks, all 512 entries, full-width PID
reporting, allocation failure and collection immediately after unlock or during
reporting. Controlled tests of 6.5.268 reproduce two heap-use-after-free paths:
reading an already-freed claimed stack and reading the PID after its process
is collected. These are host interleaving models, not newly reproduced guest
crashes. The scan detects damage at inspection time; it does not prevent later
overflow, serialize stack writes or replace guard pages.

The warning-free kernel passes 230 self-tests and four-CPU startup. A four-CPU,
5 GiB QEMU/TCG desktop run uses actual PS/2 input for 12 repeated scans while
64 children exit at staggered times and their parent collects them. Completed
scans overlap collection milestones; scans before and after the workload also
pass. Both scheduler demo tasks finish their heartbeats, all three AP workers
run, and two allocator stress runs report zero integrity errors, with balanced
page counts on the second run. A later command and terminal drag succeed; two
offline disk controls stay byte-identical and the disposable VM closes. These
checks use default placement and do not force the host interleavings, qualify
all SMP workloads or establish a CPU performance improvement.

## Remaining limits

The previously reproduced AP-parent prompt failure passes the bounded target
cases above. This does not establish general SMP safety: process-table readers
outside `sched_lock`, shared descriptors, thread-group resource transfer,
concurrent address-space users and broad churn still need qualification.
The claim protects one task's collection, not every resource shared with it.

There is no physical-hardware or long-session qualification. Real userspace
service supervision and PID-1 ownership remain unfinished. Storage atomicity,
power-loss recovery and desktop visual work are independent of this correction.

Sources: `kernel/proc/process.c`, `kernel/core/isr_stubs.asm`,
`kernel/core/smp.c`, `kernel/core/smp.h`, `kernel/core/kernel.c`,
`tools/test_process_reap.py`, `tools/test_scheduler_handoff.py` and
`user/threads.c`.
