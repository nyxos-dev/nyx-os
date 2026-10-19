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

## Remaining limits

The first target trial exposed the earlier 63-byte `execve` truncation and failed
its preservation check. The final trial includes both repairs; it must not be
confused with evidence for the shell-only change.

This is not a complete shell grammar or argument-expansion repair. Expansion
buffers, 95-byte copied external arguments, argument/stage/list limits, eager
command substitution, and pipe/fork/dup/wait failure handling need separate work.
The interactive shell's input limit and exit behaviour remain unchanged. Other
syscalls still use their existing string-copy semantics. Generic syscall errors
remain -1 rather than a complete errno contract. No power-loss, hardware or
long-running process-isolation claim is made.

See [RM.md](RM.md) for utility option semantics and [DAILY_USE.md](DAILY_USE.md)
for the broader acceptance plan.
