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
