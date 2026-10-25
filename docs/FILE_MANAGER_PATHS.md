# File Manager paths and non-overwriting copies

Development version 6.5.262 checks the full operand before File Manager performs
an action. Path construction returns an error without changing the output buffer
when the complete path and terminator do not fit. Cut and copy therefore retain
an earlier clipboard when the newly selected path is too long. Open, delete,
rename, create, navigation, properties and drag/drop stop before using a truncated
operand. A rejected drop also clears the drag state.

## Copy destinations

`vfs_cp_new(src, dst)` returns `0` for a completed copy, `1` when the destination
is occupied, and `-1` for an invalid path, allocation failure or storage error.
It uses the checked normalizer and component limits from file moves. Mounted
lookup errors are distinct from a missing destination. RAM destinations require
a valid directory parent. The existing kernel `cp` overwrite policy is unchanged.

File Manager paste first tries the original filename, then `<name>_copy`,
`<name>_copy2` through `<name>_copy99`. Only an occupied destination advances the
sequence; other failures stop it. Every candidate must fit both the 63-byte name
limit and the full path buffer. Exhausting the 100 candidates reports failure.
Ctrl-drag copies use the same checked copy operation and refuse an occupied target.
The copy clipboard remains available after both success and failure.

## Names and paths

The File Manager entry array holds 63 filename bytes plus the terminator. Entries
whose names exceed that capacity are excluded from the actionable list, and the
initial refresh reports how many were hidden. They are not converted to shorter
aliases that could select a different file. Supporting longer displayed names is
still open. This limit counts bytes, not Unicode characters.

The path buffer holds 255 bytes plus the terminator. Root joins avoid a duplicate
separator. An overlong name, suffix or directory path produces a visible error;
no shortened operand is passed to a filesystem mutation. Status messages have
their own smaller display buffer and are not used as filesystem paths.

## Verification

`python3 tools/test_vfs_moves.py --mutations` runs 307 RAM/mounted copy and move
checks, including allocation and lookup/I/O faults. Five unsafe mutations are
rejected. Makefile-based frame checks include the new copy function (112 bytes
on the qualification compiler). `python3 tools/test_move_callers.py` runs 555
caller checks, including every directory length from 1 to 255, output-buffer
preservation, retained clipboard, repeated-name selection, exhaustion, suffix
bounds, long entry filtering and refused-drop cleanup. Both suites use
AddressSanitizer and UndefinedBehaviorSanitizer.

A four-CPU QEMU/TCG VM with 5 GiB RAM and a disposable EXT2 overlay verifies
repeated GUI paste into a directory with an existing `_copy`, the generated
`_copy2` and `_copy3`, suffix refusal for a 63-byte name, and long-name filtering.
In a 251-byte directory path, cut and delete refuse the longer `note` operand;
the neighbouring `not` file remains untouched. Independent offline inspection
checks ten files byte-for-byte, including both controls, the pre-existing copy,
the new copies, long-name files and both deep-path files. The VM closes normally.

The warning-free build passes 230 kernel tests and four-CPU login. The complete
installed libc suite, including the 32 mounted-move regression from 6.5.261,
also passes. Host mocks, guest execution, GUI state and offline disk inspection
are separate evidence; none qualifies physical hardware or power-loss recovery.

## Limits

The occupied-target check and copy are separate operations. They do not lock the
whole operation against concurrent namespace changes, and they do not provide
atomic replacement or power-loss recovery. A failed disk write can leave a
partial destination; inspect it before retrying. Source bytes and metadata have
the same existing copy limitations described in [VFS_MOVES.md](VFS_MOVES.md).

Cooked File Manager shortcuts, longer displayed names and broader concurrent
filesystem semantics remain separate work. See [DAILY_USE.md](DAILY_USE.md).
