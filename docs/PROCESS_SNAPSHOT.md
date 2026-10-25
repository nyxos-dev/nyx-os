# Process enumeration ownership

Development version 6.5.266 makes `SYS_GETPROCS` copy an owned snapshot rather
than walking process pointers while transferring records to userspace. The
48-byte record remains ABI-compatible with `nyx_procinfo_t` in `user/syscall.h`:
four 32-bit values (`pid`, `ppid`, `state`, `cpu_time`) and a 32-byte name.

The kernel zeroes each complete record before filling it. A name contains at
most 31 bytes followed by a terminator and zero-filled unused bytes. The old
implementation terminated the name but copied the remaining uninitialized
stack bytes too; a shorter name could also retain a suffix from the preceding
record. Callers must not infer anything from those unused bytes.

`process_snapshot` holds `sched_lock` while reading table membership and copying
scalar fields. Null slots, PID zero and processes already claimed for collection
are skipped. No process pointer escapes into the copyout phase. Removing or
compacting the table after the snapshot cannot invalidate its records. Individual
state, CPU and name values remain observations; their writers are not all
serialized by this lock, so this is not a transactional snapshot of all task
activity.

The syscall validates the user range, allocates at most 512 records (24 KiB),
takes the snapshot, releases the lock and then copies one record at a time.
Allocation, user-memory access and freeing occur outside `sched_lock`.
Allocation failure returns -1 without writing records. Copyout failure returns
the number of preceding complete records, retaining the existing prefix-return
contract; it does not promise rollback of a partially touched failing record.
The temporary allocation is freed on every copyout outcome.

## Verification

`python3 tools/test_process_snapshot.py` executes the real snapshot and syscall
bodies with mocked allocation, user-memory access and lock boundaries. It
checks zero-filled name tails, bounded names, skipped entries, retirement of
the entire process table during copyout, allocation failure, partial copyout,
empty enumeration, invalid capacities and the 512-record limit. The original
source compiles but fails the name-tail and retirement cases. The host model
does not emulate hardware scheduling or establish general SMP safety.

The host snapshot test passes 836 checks under ASan/UBSan. The process collector
and assembly handoff regressions pass 802 and 43 checks respectively:
`tools/test_process_reap.py` and `tools/test_scheduler_handoff.py`.

A warning-free kernel build passes 230 kernel tests and four-CPU startup. A
ring-3 probe in QEMU 10.2.1 TCG (four CPUs, 5 GiB RAM, default placement) checks
289 enumerations interleaved with 32 forked-child collections. It verifies
zero-filled name tails, termination, unique nonzero PIDs, presence of its own
PID, buffer boundaries and the unchanged ABI. The identical probe on 6.5.265
reports a nonzero byte after a name terminator. The installed `/ps.elf` also
lists processes and returns successfully on the candidate.

Both `mtdemo` threads complete three heartbeats. All three AP workers run with
zero BSP worker operations, and the second allocator stress run balances its
page count with zero integrity failures. A subsequent command and terminal
window drag succeed. Both disposable VMs close normally; independent offline
inspection confirms two control files per VM are byte-identical. The host's
controlled concurrent retirement case remains distinct from this guest test;
these runs do not qualify heavy fork/exec churn with AP balancing enabled.

## Monitor and kernel diagnostics (6.5.268)

Nyx Monitor uses a bounded snapshot of the visible rows. Its context owns up
to 64 records; no temporary allocation occurs during drawing. Each internal
record contains the existing process fields plus a 64-bit heap byte count.
This internal representation does not change the 48-byte userspace ABI.
Names remain terminated and zero-filled. The table count and visible range
exclude null, PID-zero and collector-claimed entries.

`process_monitor_snapshot` holds `sched_lock` while counting and copying rows.
It clamps the requested offset to the last full page, including after the
process count shrinks. Drawing and font calls happen after unlocking. Input
uses the most recently displayed count and the next draw clamps it again.
Heap usage remains an observation of two scalar fields, not a page-table
walk or a transactional resource measurement. Name text is bounded to its
column, and numeric buffers retain full PID and heap values.

The kernel `ps` command allocates space for at most 512 existing ABI records,
takes a snapshot and prints after unlocking. Allocation failure reports
`ps: out of memory`; successful and empty output free the temporary array.
The user `ps`/`top` interfaces retain their existing enumeration contract.

`tools/test_process_monitor.py` executes the real copy, draw, key and kernel
`ps` functions with modelled rendering and collector events. It checks
retirement during rendering/output, allocation failure, scrolling after
collection, skipped entries, bounded output, long names, 64-bit heap sizes,
an inverted heap interval and a full 512-entry table. The same GUI test on
6.5.267 reproduces a heap-use-after-free when the table is retired at the
first row draw. This is a controlled host event, not a reproduced guest crash.

The monitor regression passes 311 sanitizer checks; the userspace snapshot
regression retains its 836 passing checks. Memory-reporting and PID-signal
regressions pass. The kernel builds without warnings, passes 230 kernel
tests and reaches four-CPU login.

A four-CPU, 5 GiB QEMU 10.2.1 TCG run places Terminal and Nyx Monitor side by
side using the existing tiling shortcuts. A background ring-3 probe creates
and collects 64 children. Real PS/2 input scrolls the populated monitor; after
collection its displayed count shrinks and its offset returns to zero.
Kernel and user process listings, monitor close, window restoration and drag
all succeed. Scheduler heartbeats, three AP workers and two allocator stress
runs pass, with zero integrity errors and a balanced second run. The VM
closes and two offline control files remain byte-identical. This is default
process placement, not qualification of AP-balanced fork/exec churn.

The workload uses existing `spawn`. Synchronous `exec` still holds the GUI
command path: it repaints other windows but does not service normal focus
changes. This separate responsiveness limitation remains open. The snapshot
change makes no host-CPU performance claim.

## Remaining limits

These changes cover `SYS_GETPROCS`, Nyx Monitor and the kernel `ps` command.
Other kernel diagnostic table walks and raw process readers still require
separate lifetime audits. They do not add a userspace
init supervisor, process groups, application isolation or resource ownership
for shared address spaces. See [PROCESS.md](PROCESS.md) and
[DAILY_USE.md](DAILY_USE.md).
