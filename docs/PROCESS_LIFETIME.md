# Waited-child CPU references

Development version 6.5.256 retires per-CPU references when `do_waitpid`
collects a zombie child. This is a bounded lifetime correction, not a complete
SMP process-management redesign or a userspace init implementation.

## Failure and correction

Each CPU retains a `sched_cur` pointer and an `fpu_owner` pointer. Switching
from a user task to a kernel task can leave the outgoing user's FPU state live;
the next user-task switch saves it through `fpu_owner`. That pointer must not
outlive its `process_t` allocation.

The background reaper already calls `sched_forget`, but `do_waitpid` omitted
it. A parent on CPU 1 can collect a child that ran on CPU 0 while CPU 0 still
names the child as its FPU owner. The next FPU save could then write into a
freed allocation.

The waited-child path now calls `sched_forget` before freeing the process
object, and holds `sched_lock` around table compaction and reference retirement.
This matches the existing background-reaper cleanup. The lock protects that
table mutation against AP table scans; it is not a general lifetime lock for
all process fields or scheduler operations.

Exit status, shared-address-space handling, thread-group resource handoff,
`WNOHANG`, stop reporting and syscall return state retain their previous
contracts. Interrupts remain masked on return from `do_waitpid`.

## Verification

| Layer | Evidence on 6.5.256 |
|---|---|
| Host | 308 sanitizer checks of the actual wait and reference-retirement bodies; the previous source reaches 228 checks with 64 failures |
| Host controls | No child, running child, stopped child, repeated stop reporting, exact/any child, null status, shared address space, resource handoff, moved table cursor and unrelated CPU owners |
| Kernel | Warning-free build, 230 kernel self-tests and four-CPU login |
| Serial QEMU | Paired CPU-0 and CPU-1 parents reap CPU-0 children with status 73; physical-memory inspection confirms the collected child is no longer a CPU FPU owner |
| Exception lifecycle | Seven existing `threads --faults` cases pass, followed by normal threads and a responsive shell; five deliberate unhandled exception records are expected |
| GUI | Two normal thread runs preserve shared counter, heap and descriptors; 20 fork/exec/pipe rounds report no faults; socket alias/replacement checks, scheduler heartbeats, window movement and a later command pass |
| Disk controls | The GUI run preserves its 10-byte and 24-byte unrelated files exactly |

The serial tests use four qemu64 CPUs, 512 MiB and a disposable 32 MiB EXT2
disk. The GUI uses four CPUs, 5 GiB and a separate disposable disk. All test
VMs close. The host harness mocks interrupt control, resource cleanup and
per-CPU state; it does not simulate concurrent execution.

## Remaining limits

The opt-in `smpbalance on` serial case still fails to return the kernel-shell
prompt within 30 seconds after its AP parent reports exit. Clearing the stale
FPU owner does not resolve that separate failure. Default CPU-0 placement and
the graphical workloads above pass their bounded checks.

Remote-task quiescence, concurrent reapers, wait/wakeup races, process-table
readers outside `sched_lock` and broad SMP churn remain separate work. These
checks do not qualify physical hardware, long sessions or general memory
safety. Real userspace service supervision and PID-1 ownership also remain
unfinished.

Sources: `kernel/proc/process.c` (`do_waitpid`, `sched_forget`, `fpu_switch`),
`kernel/core/smp.c` (`ap_scheduler_tick`), `tools/test_process_reap.py` and
`user/threads.c`.
