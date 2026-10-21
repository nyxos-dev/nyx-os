# Mnemosyne: text files, editing and I/O limits

Mnemosyne is the built-in text editor. Its development-version 6.5.194 I/O repair
preserves the bytes of files within the contract below and rejects files that it
cannot represent before replacing the current document. Host and target GUI checks
pass. A bounded save/reopen/cold-boot check also passes on 6.5.226; broader
persistence acceptance remains open. This page
does not declare the editor or the wider desktop ready for daily use.

Implementation: [editor_win.c](../kernel/gui/apps/editor_win.c) and
[editor_win.h](../kernel/gui/apps/editor_win.h). The broader acceptance plan is
[DAILY_USE.md](DAILY_USE.md).

## Closing a document with unsaved changes (6.5.227)

The title-bar close button and Alt+F4 ask how to handle a modified Mnemosyne
document. A clean document closes without a prompt.

| Choice | Input | Result |
|---|---|---|
| Save | Button, S or Ctrl+S | Save to the established path and close only after a complete successful write. |
| Discard | Button or D | Close without writing the pending edits. |
| Cancel | Button or Escape | Return to the same document, selection and undo history. |

While the prompt is open, ordinary typing and text clicks do not edit the
document. A failed Save keeps the prompt and dirty document open; the status
message retains the write/path/allocation error. Cancel allows continued editing
or another save attempt. The prompt switches to a vertical layout in narrow
windows and fits the minimum 120-by-80 client area.

The PS/2 driver delivers Escape as byte 27 on press, independently of the US/ES
layout and modifiers, and emits no character on release. The first GUI trial
exposed the previous omission: its scancode mapped to zero, so a host-only
editor test could cancel while the real key could not.

The compositor queues user close requests and resolves them after input
callbacks return. This keeps the window context alive through both click and
held-press callbacks. `window_destroy` remains the forced cleanup operation;
the close confirmation does not cover logout, shutdown or forced destruction.
Open / Ctrl+O has a separate pending reload decision since 6.5.228 (below).
Save remains non-atomic and new unnamed documents retain the
existing `untitled.txt` default; this does not add Save As or crash recovery.

## Reloading a modified document (6.5.228)

Toolbar Open and Ctrl+O ask Save/Discard/Cancel before replacing modified text.
Clean documents reload directly. Cancel retains the text, filename, selection
and undo history. Save must complete before the reload proceeds; failed writes
or allocation leave the prompt and dirty document intact. Discard reloads
without saving, but an unavailable or invalid target still leaves the edited
buffer and undo intact. After a failed reload, closing the window asks again.

The pending target is captured when the prompt opens. In an unnamed document,
Open keeps its existing fallback search (`/home/test.txt`, `/test.txt`,
`/shell.txt`, `/README.txt`). Save writes the unnamed document to the usual
`untitled.txt` path before loading that captured fallback; it does not redirect
the reload to the new save path. Ctrl+O still requires an established filename.
This is not a file picker or Save As.

The prompt reads `Reload file?`. While it is open, X or Alt+F4 keeps the pending
reload decision instead of changing its action. Cancel the reload before
requesting Close. A completed reload keeps the same window alive through the
remaining input callbacks; it never queues window destruction.

The existing `tools/test_editor_close.py` now also runs **304 reload checks**
under ASan/UBSan with the real editor callbacks, real deferred-close functions,
simulated VFS and guarded allocator. It covers named keyboard/button routes,
unnamed fallback capture, all choices, write/allocation failure, disappearing
and malformed input, clean reload, selection/undo retention and competing close
requests. The 6.5.227 editor fails the dirty-reload confirmation assertion.
The 72 close, 76 I/O, 53 clipboard, 961 compositor-close and 43 presentation
checks still pass. GCC 14 builds without warnings; QEMU passes 230 kernel tests
and reaches login with four CPUs.

A four-CPU, 5 GiB QEMU GUI run exercises physical Ctrl+O and toolbar Open.
Five extracted EXT2 outputs match exactly: Cancel retains a 12-byte CRLF edit;
Discard reload followed by one appended byte produces 13 bytes; Save/reload
followed by an appended byte produces 14 bytes. A denied RAM-file Save blocks
reload and retains edits until write permission is restored. Removing the
reload target while its prompt is open also retains edited text after Discard.
Both recovered RAM files are copied to EXT2 and match their 12-byte expected
contents. Screenshots confirm the prompt, decisions and failed-save/open status;
there are no kernel fault markers and the VM is closed. Unnamed fallback and
malformed-input combinations have host coverage only. This is not a new reboot,
power-loss, hardware or long-session qualification.

## Cold-boot verification on 6.5.226

Two separate QEMU sessions used four CPUs, 5 GiB RAM and a private 16 MiB
EXT2 disk. Through the actual GUI, Mnemosyne replaced a CRLF document with
`persisted`, CRLF, and `second line`, with no final separator. Save and reopen
preserved all 22 bytes. A fresh VM opened the first session's extracted disk
and recovered the same document.

After Open in each session, the terminal replaced the backing file with a
sentinel. Ctrl+S then restored the loaded editor buffer, so an unchanged fixture
or missed save could not pass. Both extracted outputs had SHA-256
`922cfabfa6819b1553d09ad809f9372d52b0c28890ccac03ce19cdf2fadf7651`.
The saved/reopened screenshots were inspected and both VMs closed. The ISO hash
was `816bfccefb6be450f74d3483cb67da9aeeaf09dab7c307b05c5f2201cec4dece`,
built from `accad1ceeb13f3584e6b83d55f1260f746693708`.

Initial automation attempts lost focus or minimized the terminal; the final
harness activated exposed terminal content and confirmed the US keyboard layout.
This verification does not claim an input repair. One file through one cold boot
does not establish power-loss safety, large-file reboot coverage, hardware
durability or long-session stability. The broader daily-use gate remains open.

## File representation

The document stores at most **512 rows**, each with at most **255 content bytes**.
A line separator creates another row, including an empty row after a final
separator. Separators do not consume the content bytes of a row.

| Input | Rows stored | Accepted |
|---|---:|---|
| Empty file | 1 | Yes |
| 511 lines, no final separator | 511 | Yes |
| 511 lines, final separator present | 512 | Yes |
| 512 lines, no final separator | 512 | Yes |
| 512 lines, final separator present | 513 | No |
| 513 lines, either EOF form | At least 513 | No |

Files may use LF or uniform CRLF. Open and Save preserve that choice, empty files,
and the presence or absence of a final separator. Save writes separators only
between rows; it does not add a final LF. A file with no separator uses LF when
the user inserts a new line. Edits, selection replacement and undo retain the
document's separator style. The maximum representable image is 131,582 bytes:
512 rows of 255 bytes with CRLF between them.

Open rejects a row or document beyond these limits, embedded NUL, isolated CR,
and mixed LF/CRLF. Other non-NUL content bytes are preserved without transcoding.
This is a byte-oriented model: columns count bytes, keyboard insertion supports
printable ASCII, and the bitmap renderer does not provide Unicode-aware editing.
The Spanish keyboard layout is not a claim of UTF-8 support.

## Open, paths and Save

The file manager can open a selected file in Mnemosyne. The shell command
`open /mnt/example.txt` also opens an editor window. Ctrl+O reloads the current
filename. It is not a file-selection dialog.

The normalized absolute filename must fit in **63 bytes**. Paths too long for
the VFS's intermediate path buffer are rejected before normalization, even when
removing `..` could produce a shorter result. A supported relative name becomes
an absolute name so a later change of working directory cannot redirect Save.
VFS symbolic links in any path component are rejected on Open and Save: relative
symlink targets currently depend on the VFS working directory. `/proc` and `/dev`
are unsupported because their generated or stream data cannot be checked through
the regular-file image interface.

Open validates the full file image before changing text, filename, cursor,
selection, scroll position, modified state or undo history. A failed open changes
only the status message. Continuing to edit and save afterwards operates on the
previous document; rejection does not merely suppress one Save operation.

For mounted files, the editor compares the materialized image with independent
backing-file size metadata. It rejects a prefix or an empty image caused by a
nonempty file's failed read/allocation. It also rejects directories, failed
descriptors, unavailable metadata callbacks and nonempty images without data.

Ctrl+S writes the current filename. A new unnamed document defaults to
`untitled.txt` in the login home, or `/home` when no login home is set. The name
must pass the same path checks before any write. Whether that home is persistent
depends on the mounted storage; a RAM-filesystem write does not survive reboot.
There is currently no Save As dialog.

Save serializes the complete model into a heap buffer. Allocation failure makes
no write and reports an error. A short or negative write reports failure and keeps
the buffer marked modified; it never reports `Saved`. **Save is not an atomic
filesystem replacement:** a write failure may already have changed on-disk bytes.
The present VFS cannot guarantee retention of the last good disk version on every
allocation, media or power-loss failure.

Backing size callbacks have no distinct error result for every metadata failure;
zero can also mean an empty file. These checks do not detect a same-size concurrent
replacement, exclude concurrent writers, or compensate for a filesystem reporting
success after a physical I/O failure.

## Selection, undo and keyboard routing

Mouse dragging selects text and Ctrl+A selects the whole document. Printable input
replaces a selection; Backspace or Delete removes it. A replacement is one undo
step. The existing undo stack retains up to 16 snapshots and restores selection
and line-ending style along with the text. Ordinary typing still proceeds without
a new undo entry when allocating a snapshot fails. The Cut and Paste operations
introduced in development version **6.5.199** instead require an undo snapshot:
allocation failure cancels the operation and preserves the document and selection.

Ctrl+C and Ctrl+Z reach the focused client when no user process is registered as
the global foreground process. Mnemosyne uses them for Copy and Undo. When a
foreground process exists, the keyboard driver sends SIGINT or SIGTSTP to that
process and consumes the key, even if the editor is focused. This remains a global
foreground rule, not per-window process routing. In an idle terminal, Ctrl+C
cancels the current input line; Ctrl+X cuts that line and Ctrl+V pastes supported
clipboard characters. Terminal output selection is not added by this change.

## Shared clipboard

Ctrl+C copies the selection, Ctrl+X cuts it, and Ctrl+V inserts clipboard text or
replaces the selection. Cut and Paste each form one undo step. Copy and Cut reject
selections exceeding the shared clipboard's **4,096-byte** capacity, including
line separators, without changing the clipboard or document. A Cut that would
join a row beyond 255 bytes is rejected as well.

Paste validates and stages its complete result before replacing the selection.
It rejects NUL, bare CR, rows over 255 bytes and documents over 512 rows. LF and
CRLF clipboard separators, including a mixture, become separators in the current
document's existing style. This deliberate paste normalization differs from Open,
which rejects mixed separators to avoid silently changing a loaded file. Other
bytes remain opaque; pasting does not add Unicode-aware cursor movement or fonts.
An empty clipboard and Copy/Cut without a selection leave the document unchanged.
Clipboard shortcuts do not edit the document while a Find, Replace or Go To prompt
is active.

Paste temporarily allocates up to 131,072 bytes for staged rows, a 4,096-byte
clipboard buffer and an undo snapshot of the existing used rows. Cut allocates
the selected bytes and an undo snapshot. Allocation failure preserves the text,
selection and undo history; failed Cut also preserves the clipboard. The existing
shared clipboard is global: per-login clearing, ownership and synchronization
between concurrent writers remain separate work.

## Evidence and acceptance

### Close decisions, development version 6.5.227

`python3 tools/test_editor_close.py --compare-ref accad1ce` passes 72 sanitized
checks against the real editor and extracted compositor close functions. The
preceding immediate-close route fails the deferred-lifetime assertion. The
checks cover Save/Discard/Cancel, allocation and write failures, unchanged text
and undo history, narrow button geometry, clean documents and forced cleanup.
The existing 76 I/O, 53 clipboard, 961 compositor-close and 43 presentation
checks pass. US/ES IRQ, polling and mixed input tests include Escape press and
release with Ctrl, Shift and AltGr; this regression fails on the previous driver.

The final GCC 14 build has no compiler warnings. QEMU passes 230 kernel tests
with one CPU and reaches login with four CPUs. These host and boot checks do not
establish concurrent window lifetime safety or power-loss durability.

A four-CPU, 5 GiB QEMU GUI run verifies X/Alt+F4, mouse choices, physical Escape,
blocked typing in the prompt, clean close and the minimum-size vertical layout.
Extracted files match the 13-byte CRLF saved edit, the 11-byte LF file after
Discard, and the original 10 bytes of a RAM file copied out after a denied Save.
The failure caption and subsequent successful 12-byte Save are visible, and
terminal commands and dragging still work. The recovery-export command in this
run had a stray character before `cp`; its failed byte assertion is retained,
and that export is excluded from these results.

A separate recovery-only GUI run clears the terminal input before each command,
confirms the permission changes and reads back both states. Disk extraction
matches the original 10 bytes after denied Save and the edited 12 bytes after
write permission is restored. Screenshots show the failed-save prompt, retained
text and successful Save. All VMs are closed, with no kernel fault markers.
Earlier attempts used unsupported mounted `chmod` or lost focus and were
discarded; these checks do not establish a general input/focus timing repair.

### Clipboard, development version 6.5.199

On Linux or WSL:

```sh
python3 tools/test_editor_clipboard.py
python3 tools/test_editor_io.py
python3 tools/test_keyboard_control.py
```

The clipboard runner passes **53 checks** against the real editor key handler and
clipboard functions, with AddressSanitizer, UndefinedBehaviorSanitizer and
allocation fault injection. It covers reversed selections, exact LF/CRLF copies,
paste normalization, Copy/Cut/Paste bounds, failed allocation preservation, one-step
undo and eviction from the full undo stack. The preceding editor fails the first
copy assertion. All 76 existing text I/O cases and the US/ES IRQ/poll keyboard
checks also pass. A warning-free GCC 14 build passes 228 kernel tests in QEMU and
reaches login with four CPUs.

In a one-CPU, 512 MiB QEMU GUI session with a disposable EXT2 disk, actual PS/2
Ctrl+A/C/X/V/Z/S events copied LF text into a CRLF document, cut it, undid Cut and
undid Paste. Disk extraction verified all saved intermediate files exactly.
Terminal-to-editor text matched its 13-byte source; editor-to-terminal paste ran
the expected command. Idle terminal Ctrl+C discarded the pending command and the
next command ran successfully.

A separate GUI session copied 252 generated bytes from Oneiros into Mnemosyne,
saved them and matched the extracted file against the serial clipboard output,
excluding the single LF added by `clip` for display. The first attempt incorrectly
used `clip > file`: that built-in copies its arguments instead of performing shell
redirection, so that attempted reference capture was rejected and repeated using
console output. Both sessions ended with responsive desktops, no kernel fault
markers and closed VMs. These are bounded integration checks, not a clipboard
concurrency, memory-pressure, reboot or long-running daily-use test.

The tested 6.5.199 kernel SHA-256 is
`8d15e2d300075e654b5b1c75eacb1c10b71f3e0ffe1f52c7cece34fa1c1e2fa8`;
the ISO SHA-256 is
`e7e4b03f448660a235c9301dcffa9834ba5e75b514540a4c8fc066a8dc2d3f4e`.

### Text I/O, development version 6.5.194

On Linux or WSL:

```sh
python3 tools/test_editor_io.py
```

The runner includes the real editor functions and real VFS lexical path normalizer,
with a simulated VFS and fault-injecting allocator. Its **76 cases pass** against
the repaired editor. The same cases fail against the preceding implementation.
Fixtures and output files are compared byte for byte; allocation guard bytes and
descriptor/allocation counts are checked after each run.

Coverage includes empty files, both separator styles and EOF forms, 254/255/256-byte
rows, 511/512/513-line boundaries, maximum-size files, malformed input, partial and
failed file images, paths and symlinks, editing after a rejected open, selection,
undo, edit limits, allocation failures and short/error writes. These are host
results; they do not prove physical-key delivery, compositor behavior or storage
durability.

A four-CPU QEMU VM with 5 GiB configured RAM and a disposable EXT2 disk exercised
the real Terminal `open` command and Mnemosyne. The test replaced backing files
with a sentinel after loading them, then used Ctrl+S and extracted the disk after
closing the VM. Exact output matched LF, CRLF, empty files, both styles without a
final separator and the maximum 131,582-byte image. This checks actual Save, not
merely an unchanged input file.

Two rejected reloads visibly reported the 255-byte row and 512-row limits while
preserving the edited document. Ctrl+A, selection replacement and two PS/2 Ctrl+Z
events restored the original CRLF bytes. Saved intermediate selection/undo states
were also compared exactly. The desktop remained responsive; no panic, page fault
or general-protection fault appeared in the serial log. All test VMs were closed.

The first run lost focus/input in a scripted interaction; its four completed
outputs were verified and only the remaining cases were repeated with more widely
spaced input. This does not establish a fix for general input/focus timing. Mixed
LF/CRLF, bare CR and NUL rejection have host evidence only. Disk extraction after
the session does not establish reboot behavior, power-loss safety or a long-running
daily-use session; those acceptance checks remain open.

The tested kernel SHA-256 is
`61217372e8491b18f97f3419133c86ea33747368c23cb8c9240d31e451c105ea`;
the isolated ISO SHA-256 is
`e1ce956295c08caa8eed358d63e0d153db21ba98d29ac1de11d82c1f2e9d7886`.
