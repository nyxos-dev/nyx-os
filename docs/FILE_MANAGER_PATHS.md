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

## Verification of 6.5.262

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

Longer displayed names and broader concurrent filesystem semantics remain separate
work. Cooked shortcut support is described below. See [DAILY_USE.md](DAILY_USE.md).

## Keyboard actions (6.5.263)

File Manager uses the modifier snapshot attached to each dispatched key. Ctrl+C,
Ctrl+X and Ctrl+V accept the cooked control bytes emitted by the keyboard driver,
including when Ctrl has already been released before the application runs. They
call the same checked copy, cut and paste operations as the context menu. A failed
move retains the cut clipboard; a copy selects a free bounded destination name.
Ctrl+Alt combinations do not invoke these actions. Pointer Ctrl-drag continues to
sample physical Ctrl state at the start of the gesture.

Ctrl+F toggles the existing case-insensitive search. Ctrl+A selects the first
visible result, including under a filter; File Manager still has a single
selection, not a multi-selection model. Copy and cut act on that selected entry.
With no visible result they leave the clipboard unchanged. Escape closes search.
While a create or rename field is active, cooked file-action shortcuts leave the
field and file clipboard unchanged; ordinary printable input still edits the name.

The caller regression executes the real key callback, search mapping and clipboard
helpers under ASan/UBSan. Its 589 checks include delayed modifiers, cooked and
letter forms, filtered selection, invalid selection, input modes, path rejection,
copy collisions and move failure/success. The new copy assertion fails against
6.5.262. Keyboard IRQ/polling and captured compositor-modifier suites also pass.

A paired real-PS/2 QEMU check confirms Ctrl+F fails on the 6.5.262 image and
opens search on 6.5.263. The new four-CPU, 5 GiB TCG session verifies filtered
selection/copy, two non-overwriting pastes, cut/paste collision refusal followed
by a successful move, rename-field protection and six search toggles. Eleven
files match their expected bytes after offline EXT2 inspection, including the
new copies, collision target and moved file; the old source is absent. The
terminal remains responsive after closing File Manager. The VM closes normally.
The warning-free build also passes 230 kernel tests and four-CPU login.

The kernel still owns a global foreground-job signal route. This increment does
not add per-window job ownership, multi-selection, Unicode input or long-session
qualification. Serial bytes carry no captured Ctrl modifier and do not trigger
these File Manager shortcuts.

## Unavailable sizes and directory summaries (6.5.264)

A file size is known only after a successful `vfs_stat` for a complete bounded
path that still identifies a file. An explicit lookup failure, a path that does
not fit or a changed directory type leaves the size unknown. The list and
Properties show `Unknown`; a successfully measured empty file still shows `0 B`.
Properties retain the metadata snapshot from the listing, rather than claiming
live refresh while another process changes the file.

Size sorting places unknown files after known files in either direction, with
name ordering for ties. Name sorting and directories-first ordering remain.
Parallel name, type, size and validity fields move together during sorting.
Directory summaries use 64-bit accumulation over the listed entries, and mark
the byte total `known` when some listed file sizes are unavailable. Opening a
folder unsuccessfully clears its old selection and totals. Excluded long names,
entries beyond the list capacity and directory descendants are not in that sum.

Formatting uses widened arithmetic so a size near the 32-bit file-size limit
does not wrap while rounding. The actual 6.5.263 formatter reports `0 MB` for
4,294,967,295 bytes; the corrected formatter reports `4096 MB`. This is a display
repair, not support for larger files. The underlying file-size API stays 32-bit.

`tools/test_fileman_metadata.py` executes the actual refresh, sort, properties and
label helpers with the kernel's formatter under ASan/UBSan. Its 539 checks cover
zero/error/type-change metadata, both sort directions, all directory lengths
from 1 to 255, failed-refresh cleanup and the sum of 512 maximum-size files.
The 589 move/copy/shortcut caller checks also pass with the expanded state.

A four-CPU, 5 GiB QEMU/TCG session distinguishes a real empty file from an
unavailable deep-path size in both the list and Properties. It preserves a known
5-byte neighbour, labels the partial total `5 B known`, and keeps the unknown
entry last when sorting by size in either direction. Nine disk fixtures remain
byte-identical after closing the VM. The complete 6.5.263 shortcut workflow also
passes again on this image, including eleven offline file checks and source
absence after movement. The warning-free build passes 230 kernel tests and
four-CPU login. These GUI and offline checks are separate from host fault tests.

Backend callbacks that return success with a misleading size cannot be detected
by this UI. Directory-read errors that look like end-of-list, concurrent changes,
whole-folder accounting and hardware qualification remain separate work.

## Properties input isolation (6.5.270)

Properties consumes client keyboard input while open. Clipboard shortcuts,
search, navigation and pending filename edits cannot operate on the listing
behind it. Esc and Enter close the dialog without forwarding the key to an
underlying edit or file activation. A client click also closes it without
activating the covered toolbar or list item. Global window-management shortcuts
retain their existing behaviour.

Opening Properties cancels a pending file drag. Pointer callbacks cannot start
or finish a file move while it is open. A mouse press used to dismiss it stays
consumed until the buttons are released, so holding and moving that same press
cannot start a hidden drag. `props_wait_release` records that gesture boundary;
it does not poll, add a timer or allocate memory. Normal clipboard and pointer
operations resume after dismissal and release.

Hemera 3.35 delivers stationary button edges to File Manager's pointer callback
so a release is observed even without pointer movement. Other applications
retain their existing movement-only callback dispatch. The actual compositor
branch passes host checks for press, release, unchanged held state, ordinary
movement and clients without a callback. Idle/damage regressions also pass;
this is not a measured CPU optimization.

The preceding implementation allows captured Ctrl-V to move a cut file behind
the open dialog and allows a pending drag release to move it too. Controlled
host calls reproduce both using the actual action bodies and simulated VFS
operations. The expanded `tools/test_move_callers.py` passes 992 ASan/UBSan
checks, including preserved clipboard, search and filename-edit state, four
modifier combinations, both keyboard dismissals and pointer release recovery.
The click test observes the actual modal dispatch prefix; the remaining click
workflow is qualified separately in QEMU. Existing metadata and queued-modifier
tests also pass. This correction does not fix long property text overflowing
the fixed panel, add Unicode, make file moves atomic or provide async terminal
execution.

A four-CPU, 5 GiB QEMU/TCG run uses real PS/2 input to cut a file, enter a
destination directory and open Properties on another file. Clipboard, search
and navigation attempts leave the dialog, listing and pending cut unchanged.
Esc closes it; a subsequent paste moves the file and clears the cut. Enter
closes without activating a file, a click over Delete only dismisses, and a
held dismissing press cannot start a drag. Releasing the button with a stationary
pointer clears the gesture gate. Offline inspection verifies nine expected file
contents, including the moved six-byte file, and absence of its original source.
Installed `libctest`, `stackcheck`, a later terminal command and window drag pass.
The disposable VM closes. The build is warning-free, with 230 kernel tests and
four-CPU startup passing.

An initial candidate fails the stationary-release check because the compositor
only dispatches movement; the corrected pointer branch passes a complete fresh
VM run. This target check is distinct from the host model of hidden VFS actions.

## Bounded Properties layout (6.5.271)

The Properties panel fits inside the client area with a four-pixel margin and
a maximum width of 520 pixels. Its fields wrap using the existing 8-by-16 font.
When there is insufficient height, the final visible row ends with an ellipsis
and the footer asks for more space. Very small windows use a compact
`Resize/Esc` hint. The existing click, Esc and Enter dismissal rules remain.

Rendering replaces control and non-ASCII bytes with `?` in the temporary row;
the stored name, path, metadata and clipboard remain unchanged. This is a
bounded byte-oriented display, not Unicode support. The panel allocates no
heap memory, adds no periodic work and does not alter global framebuffer clips.
Other File Manager controls retain their existing layout limitations.

`tools/test_fileman_properties.py` exercises the actual drawing helpers with
recording graphics functions and the kernel formatter under ASan/UBSan. It
checks panel and text bounds across client sizes from zero through 800 by 600,
snapshot preservation, complete long fields at normal size, explicit overflow
at smaller sizes, directory/unknown sizes and control-byte rendering. These
are host geometry checks, separate from framebuffer and interactive VM tests.

Shrinking the client height to 80 pixels also exposed a pre-existing scrollbar
failure: the remaining track height became negative, was converted to an unsigned
dimension, and overflowed the framebuffer filler's `y + height` boundary test.
With window clipping active, drawing could then iterate billions of rows while
the desktop stopped responding. The scrollbar now skips non-positive tracks and
fits its thumb even when fewer than eight pixels remain. The rectangle filler
checks dimensions against the remaining framebuffer width and height using
subtraction, so wrapped dimensions cannot bypass the bounds in either pixel format.

The host suite covers 72,257 panel/scrollbar checks and 64,001 framebuffer cases
under ASan/UBSan. The framebuffer reference uses widened arithmetic, verifies the
entire buffer and guard area, and includes zero sizes, off-screen origins, wrapped
dimensions, 8/32-bit pixels and clipping combinations. It retains the existing
limitation that the 8-bit path does not apply window/region clips. The earlier
implementation fails the bounds suite with a sanitizer buffer overflow; the GUI
reproduction stops in `fb_fill_rect` while shrinking to the minimum height.

A fresh four-CPU, 5-GiB QEMU/TCG session exercises real PS/2 input with a 63-byte
name and a 256-byte property path. Normal, 300-by-180, minimum 120-by-80 and
quarter-tiled client geometry preserve the open snapshot; restoring the window
recovers the full fields. The long path is reached by navigating from its parent
because the complete launch command exceeds the terminal's input limit. Its size
remains `Unknown` when the existing metadata path limit is exceeded.
Screenshots separately verify wrapping, the ellipsis and the compact hint.

The same session checks blocked hidden actions, Esc/Enter/click dismissal,
consumed held presses, stationary release, an acknowledged move after dismissal,
terminal execution and dragging. Stack-canary checking and the userspace libc test
pass. After VM shutdown, nine expected file payloads match exactly, including the
six-byte moved file; its old source is absent. This does not establish atomic
moves, full Unicode layout, general rendering safety or hardware support.
