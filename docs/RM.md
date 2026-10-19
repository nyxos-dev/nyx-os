# Removing files

NyxOS has a kernel-shell `rm` and the ring-3 `/rm.elf` used by the userspace
shell. Development version 6.5.211 adds an explicit option boundary to both.
Their remaining capabilities differ.

| Command | Syntax | Behaviour |
|---|---|---|
| Kernel shell | `rm [-rRf] [--] PATH...` | Existing `r`/`R` selects recursive RAM-tree removal; `f` suppresses errors. `--` ends option parsing. |
| `/rm.elf` | `rm [--] PATH...` | Removes each operand using `unlink`; a leading unsupported option is rejected before any removal. |

Use `rm -- -draft` to remove a literal filename starting with a dash. `./-draft`
also names that file without looking like an option. A single `-` is an operand.
Option parsing ends at `--` or the first operand; subsequent words are filenames.
Thus `rm file -draft` treats both words as filenames. Neither implementation
reorders options after operands.

Previously `/rm.elf -f missing` attempted to unlink a file named `-f`. It now
reports an unsupported option and exits 2 without touching any operand. This
does not implement `-f`, `-r`, `-R` or long options in the ring-3 utility. The
kernel command retains its existing options, now allowing `rm -r -- -directory`.
An unknown leading kernel option is rejected before removal begins.

The ring-3 utility exits 0 when every removal succeeds, 1 for missing operands
or a failed removal, and 2 for an unsupported option. A failure on one operand
does not prevent attempts on later operands. Diagnostics retain their existing
stdout destination. Empty-directory removal follows the VFS's existing `unlink`
semantics; non-empty directories are refused by the ring-3 utility.

## Verification and limits

`python3 tools/test_rm_options.py` runs 37 ASan/UBSan scenarios against the actual
drivers with recording filesystem mocks; it never deletes host files. Comparing
against `89a1c24` detects 16 earlier failures. Cases cover unsupported/grouped
options, `--`, literal dash names, single `-`, spaces, multiple operands and
continued processing after failure.

The warning-free kernel passes 230 KATs and four-CPU login. A separate headless
QEMU trial runs the actual `/rm.elf` through fork/execve/waitpid on RAM and a
private EXT2 image. It checks refusal before mutation, explicit dash-name
removal, multi-operand failure status and preserved file bytes. Kernel-shell
`rm --` succeeds and an invalid grouped option preserves its target. Extracted
EXT2 fixtures retain the exact two four-byte `KEEP` contents; the VM is closed.

The same trial confirms file preservation and explicit removal through `sh -c`,
but also reproduces a separate unresolved shell bug: `sh -c` returns 0 even when
the child `rm.elf` exits 2. Direct utility status and shell status must not be
treated as equivalent at this baseline.

Kernel recursive removal remains limited to the RAM tree; mounted paths fall
back to a single unlink. Its force option still suppresses all removal errors.
The current unlink/stat syscall results do not distinguish every missing,
permission or I/O error. Full command parity, precise force semantics and
mounted recursive removal remain open; no POSIX-completeness claim is made.
