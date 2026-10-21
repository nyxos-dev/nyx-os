# Shell command entry and exec arguments

Development version 6.5.212 preserves `sh -c` command status and rejects
oversized command input before parsing. It also prevents the kernel's `execve`
argument copy from silently shortening the command before the shell receives it.

## Contract

| Entry | Behaviour |
|---|---|
| `/sh.elf -c COMMAND` | Returns the last executed command's existing `last_status`, including the last pipeline stage. Skipped `&&`/`\|\|` segments preserve that status. |
| Empty command | Exits 0. |
| Bare `exit` in a command list | Stops the list and preserves the preceding status; later commands are not executed. |
| Missing `-c` argument | Exits 2 instead of entering the interactive shell. |
| Command longer than 127 bytes | Exits 2 before parsing or command substitution, when delivered intact to the shell. |
| `execve` arguments | Up to 32 strings of 255 bytes each, followed by a null pointer. Earlier capacity was 63 bytes with silent truncation. |
| `execve` environment | Up to 16 strings of 159 bytes each, followed by a null pointer. |
| `execve` executable path | Up to 127 bytes. |
| Excess string/vector length or inaccessible input | `execve` returns -1 before opening/loading the replacement image. It does not execute a shortened prefix. |

The shell checks the command-list status, not its separate stop/continue flag.
`exit N` is not added. Additional `-c` arguments retain their earlier handling;
this change does not implement positional shell parameters.

The `execve` path, argument and environment checks use the existing user-memory
translation boundary. String copying must reach an actual terminator within the
destination capacity. Full vectors must have an accessible null terminator at
their maximum count. The maximum argument/environment frame fits within the
existing four initially committed user-stack pages. No allocator or scheduler
policy changes are introduced.

## Verification

- `python3 tools/test_sh_entry.py --compare-ref 2f5e974` exercises the actual
  command-entry branch with a recording parser stub: 23 ASan/UBSan cases pass;
  the earlier source fails 15. This host test executes no shell commands.
- `python3 tools/test_exec_input.py` exercises the actual kernel string/vector
  helpers with mapped-memory mocks: 5,365 ASan/UBSan checks cover exact limits,
  overflows, inaccessible bytes and maximum-vector terminators.
- The rebuilt kernel has no compiler warnings, passes 230 kernel autotests,
  and reaches login with four CPUs.
- An isolated one-CPU, 512-MiB QEMU guest runs the actual shell and `execve`:
  26 shell cases and nine exec cases are repeated in RAM and on a private EXT2
  image. They cover selected command statuses, pipelines, missing arguments,
  127-byte accepted commands, rejected 128-byte commands and substitutions,
  255-byte intact arguments, excessive vectors/strings and invalid pointers.
  Maximum accepted argv/envp contents are checked by the replacement program.
  The extracted EXT2 `keep` file remains exactly `KEEP`; `boundary` is empty.
- A separate graphical session on the same kernel runs `mtdemo` with three
  heartbeats from each task, then `echo`, `mem` and `stackcheck`. Kernel-stack
  canaries remain intact, the terminal remains interactive, and a 100-by-60-pixel
  drag moves the window. All test VMs are closed.

## Checked external argument expansion (6.5.214)

The external-command path now preserves literal arguments up to 127 bytes,
instead of copying only their first 95 bytes. Longer words, too many words
or too many wildcard results stop that child with status 2 before file
redirections or `execve`. A wildcard directory prefix is no longer shortened
to 78 bytes, and each constructed result is checked before copying.

| Boundary | Behaviour |
|---|---|
| Raw words in a pipeline stage | At most 11, including redirection syntax; excess exits 2 |
| Expanded argv | At most 32 entries, including the program name |
| Expanded word or wildcard result | At most 127 bytes; excess exits 2 |
| Directory listing | A full 64-entry page rejects, because more names may have been omitted |
| No wildcard match in a non-full listing | Retains the literal pattern as before |

The 127-byte word boundary matches the existing raw filesystem syscall input
buffer. The larger 255-byte `execve` transport limit from 6.5.212 remains
available to other callers. The shell's static word storage grows by 1 KiB;
it adds no dynamic allocation. A full page is rejected conservatively even
if the directory contains exactly 64 entries. This is not paginated globbing.

`python3 tools/test_sh_expansion.py --compare-ref 6f5fb9b` extracts the actual
splitter and glob code with synthetic directory input: 886 ASan/UBSan checks
pass, while the preceding source fails 529 assertions. The entry regression
still passes 23 cases. The warning-free GCC 14 build passes the RTC/ELF
contracts, 230 kernel self-tests and four-CPU login.

An actual packaged shell in a one-CPU, 512-MiB QEMU guest repeats the following
checks in RAM and on disposable EXT2:

- Removing a requested 100-byte path preserves an existing 95-byte prefix file.
- Too many raw words, 33 expanded arguments and a full directory page return
  2 while preserving an existing redirection target.
- Exactly 32 argv entries reach `echo` intact, producing the expected 124 bytes.
- Local variable expansion produces a preserved 127-byte word; 128 bytes fail
  before redirection. An initial harness used imported environment variables,
  which the shell does not support; the final test uses its local assignments.
- A wildcard beneath a 94-byte directory path produces its full matching name.

Offline EXT2 extraction checks 364 exact bytes across the preserved prefix,
survivor and four output files. Both test modes pass and the VM is closed.
The earlier isolated candidate trial is separate from this packaged-kernel
result. This increment adds no GUI, hardware or long-duration qualification.

## Kernel-shell pipe failures (6.5.232)

The kernel shell used by the GUI terminal supports a single buffered pipe. It
captures the left command, writes that output to `/tmp/pipe`, and appends the
temporary path as the right command's file argument. This is separate from the
descriptor-based pipelines implemented by `/sh.elf`.

The capture holds at most 4096 bytes. Exactly 4096 bytes and an empty capture
remain valid. Any additional eligible byte marks the capture as failed;
`pipe_stop()` returns -1 and disables capture. The kernel shell then reports
`pipe: output exceeds 4096 bytes; right command not run`, without opening the
temporary file or running the consumer. A new capture clears the error state.
Unrelated background output, already excluded by 6.5.231, cannot overflow it.

Opening the temporary file must succeed, and a nonempty capture must be written
completely in one existing VFS write. A negative, zero or short write aborts the
consumer after closing the handle. These failures therefore do not open or
truncate a redirection destination belonging to the right command. Right-hand
command text longer than 245 bytes is rejected before running the producer,
leaving room for the appended temporary path and terminator.

The existing terminal host runner checks exact capacity, overflow and reset in
the actual capture implementation, and exercises the actual kernel pipeline
dispatcher with injected open/write failures. The existing userspace `cat`
streaming tests remain applicable. Source references:
`kernel/drivers/video/screen.c` and `kernel/core/kernel.c`.

This does not implement streaming pipes, transactional commands or crash-safe
temporary files. The left command has already run when capture or I/O fails;
the temporary file may have been truncated or partly written. Later semicolon
segments still run. The kernel command entry remains void rather than exposing
a new shell exit-status contract. Physical storage callbacks may not report
every hardware failure, and concurrent reuse of `/tmp/pipe` remains outside this
bounded fix. The existing top-level command length limit is unchanged.

## Remaining limits

The first target trial exposed the earlier 63-byte `execve` truncation and failed
its preservation check. The final trial includes both repairs; it must not be
confused with evidence for the shell-only change.

This is not a complete shell grammar or argument-expansion repair. Variable
storage and intermediate expansion buffers, stage/list limits, eager command
substitution, and pipe/fork/dup/wait failure handling need separate work.
Other pipeline stages or earlier substitutions may already have run when one
child rejects its arguments; the complete pipeline is not a transaction.
The existing wildcard dispatch still requires `*` or `?`; a bracket-only
pattern and quote-protected directory prefixes need separate compatibility work.
Inherited environment import is not implemented; local values remain bounded.
Executable PATH-search buffers still need their own complete-input checks.
Version 6.5.215 adds checked filesystem syscall input and normalization;
see [SYSCALL_PATHS.md](SYSCALL_PATHS.md) for its scope and remaining limits.
The interactive shell's input limit and exit behaviour remain unchanged. Other
uses of the legacy string copier retain their earlier semantics. Generic syscall errors
remain -1 rather than a complete errno contract. No power-loss, hardware or
long-running process-isolation claim is made.

See [RM.md](RM.md) for utility option semantics and [DAILY_USE.md](DAILY_USE.md)
for the broader acceptance plan.
