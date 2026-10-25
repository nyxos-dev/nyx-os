# Daily-use roadmap: VM first, then physical hardware

NyxOS's next objective is a dependable daily workspace: open the desktop, edit and
save files, use the terminal, compile and run programs, install packages, and return
to the same work after reboot. The first supported environment is a documented VM
configuration. Physical hardware follows once those workflows are repeatable there.

This is an acceptance plan, not a declaration that NyxOS is ready for daily use.
The stages below have no calendar deadlines; each closes when its evidence satisfies
the stated criteria. A passing unit test, a successful boot, and a usable work session
prove different things and must be reported separately.

The architecture remains the repository's C and x86_64 Assembly kernel, its C runtime
and in-OS C toolchain, and the N/N++ language pipeline. Changes should extend those
interfaces in small reviewable steps. The subsystem map is in
[ARCHITECTURE.md](ARCHITECTURE.md), and the current test machinery is in
[TESTING.md](TESTING.md).

## 1. Establish the VM reference and evidence

Choose and record a reference QEMU configuration rather than treating every emulator
setting as supported. Record firmware, CPU model/count, RAM, storage controller and
disk geometry, framebuffer mode, input devices, and NIC. Use a disposable disk image
for failure injection and a separate persistent image for normal save/reboot checks.

The initial matrix should exercise BIOS and UEFI/OVMF, one and four CPUs, and RAM
configurations below and above the current allocator ceiling, including the reported
4–5 GiB case. Passing with more configured RAM must not be described as allocating all
of that RAM. Network checks need a declared NIC and test endpoint; a boot without a
supported NIC is a separate configuration.

Acceptance evidence for each supported configuration:

- The recorded commit builds without warnings; the compiler/tool versions, kernel
  and ISO hashes, full build log, and test commands are retained together.
- The self-test boot reaches `SELFTEST-END`, reports no failures, and contains no
  kernel fault. The authoritative test count comes from that build, not this page.
- A normal boot reaches login; keyboard login opens a responsive desktop. A screenshot
  and interaction transcript supplement the serial log.
- Repeated boot, logout/login, and shutdown/reboot trials record exact counts and any
  failures. The same procedure can be rerun from a clean checkout and disk fixture.

## 2. Make the VM a reliable work environment

Work by observable failure: reproduce a problem, agree on the behavior, repair its
owning subsystem, and keep a regression that exercises the real boundary. Changes in
memory, scheduling, filesystem lifetime, or input require a desktop workflow as well
as their focused tests.

| Responsibility | Work to close | Acceptance evidence |
|---|---|---|
| Kernel and process runtime | Reliable spawn/exit/wait, pipes, signals, scheduling, syscall failure paths, and resource cleanup under repeated use. | Run foreground/background programs while the desktop responds; exercise exits, faults, cancellation and allocation failures. Surviving processes continue; process/fd/frame counts settle after cleanup. Record the workload and iteration count. |
| Memory and paging | Keep detected RAM, allocator-managed RAM, used RAM and free RAM consistent. Expand the allocator only with the matching bitmap/refcount/reservation, mapping and address-width changes. | `/proc/meminfo`, `free` and Nyx Monitor agree for the same snapshot. Allocation pressure returns clear failures without corrupting a surviving process. A future RAM expansion must actually allocate, write and verify frames beyond the old ceiling. |
| Storage, VFS and packages | Preserve saved data and descriptor ownership; detect incomplete reads/writes. Treat compiler success, staged output, installed binary and its manifest as separate checks. | Save/read/reboot/hash round trips; short-write, full-disk and read-failure cases; failed package builds preserve the previous executable and manifest. Failed recovery retains usable recovery files and reports the failure. Test the real mounted filesystem in addition to mocks. |
| Input and clocks | Consistent IRQ/polling keyboard behavior, canonical input/EOF, pointer edges, button transitions, layout handling and advancing wall time. | Type, paste, edit, use Ctrl+D/C/Z, drag to every edge and back, and hold/release menu and taskbar buttons while work runs. Check the clock advances and no key/button remains latched after the interaction. |
| Hemera desktop | Stable focus, menus, stacking, resize, workspaces, window lifetime, redraw and logout. | Repeatedly open/close, move/resize and switch applications; exercise menu hover and held buttons. Preserve focus/input routing and usable interaction during a foreground task; retain screenshots plus serial/fault logs. |
| Erebus terminal and shell | Predictable builtins/userland dispatch, clear, redirection, pipelines, interactive input and child-status reporting. | Compare representative commands through graphical and serial entry points; stream and redirect `cat`, including EOF and failure paths; verify saved bytes and status rather than only displayed text. |
| Mnemosyne editor and file workflow | Reliable open/edit/select/undo/save/reopen, paths with spaces, large-file boundaries and errors. | Edit and save a known document, close/reopen it, and compare exact bytes before and after reboot. An unsuccessful save is reported; editing or an application failure must not silently replace the last good file. |
| Selene and network stack | Usable navigation, local/remote content handling, bounded parsing, timeouts and recoverable connection errors. | Load a fixed local corpus and controlled HTTP/TLS endpoints; navigate/back/reload, test malformed content and offline/time-out cases, then close/reopen. Record protocol and rendering limits instead of inferring general web compatibility. |
| Oneiros | A coherent model-loading contract in CLI and GUI, bounded generation, usable stop/copy controls and correct cleanup. | The shipped model generates text; Copy exports the displayed bytes; stop, close and reopen remain responsive. Missing, truncated, extra-length and non-finite checkpoints are rejected by the actual loaders. Host validation and target GUI evidence are identified separately. |
| N and N++ | Preserve the documented language contracts while extending the compiler, selfhost drivers and integration examples. | Keep compiler/checker differentials, diagnostic refusals, ownership cleanup traces and input-boundary tests. Exercise edit → compile → run inside NyxOS with exact output/status checks; identify any host-built lowering or tool still used. |
| Accounts and configuration | Persistent user homes and settings, permissions, login/logout and useful behavior without a writable disk. | Create a user, save a file and desktop settings, log out and reboot, then verify ownership and exact contents. Report unavailable storage and failed writes; do not describe a RAM-only save as persistence. |
| Text and accessibility | A stated encoding contract, shared clipboard behavior, keyboard navigation, readable fonts and visible focus. | Copy selected text between Oneiros, Mnemosyne and Erebus with exact byte checks, undo and size limits. Test supported accented characters separately from keyboard-layout selection; unsupported input must be explicit. |
| Network trust | Define certificate, hostname and clock-validation policy for normal HTTPS use. | Correct trusted connections work; wrong-host, expired and unknown-root certificates fail visibly. An explicitly chosen development override must not silently become the normal trust policy. |
| Audio and media | Qualify the existing AC97 and SB16 paths before building media controls on top of them. | Play a fixed WAV, exercise volume and stop, then repeat with no supported audio device. Keep the desktop responsive and report unavailable playback accurately. |
| QA and integration | Define supported configurations, retain reproducible fixtures, independently review changes and prevent regressions across subsystem boundaries. | Every closed defect has a reproducer or stated reason a permanent test is unsuitable. Each integration result identifies the exact artifact, workload, expected result, actual result and remaining limits. |

The end-to-end VM session is the stage gate: log in; create/edit/save/reopen a file;
use terminal input and redirection; compile and execute C and an N program; install a
package and exercise a failed replacement; use the browser against a controlled
endpoint; generate/copy text in Oneiros; close the applications; reboot and verify the
saved files and installed program. Run a recorded repetition and duration budget, with
resource measurements at comparable idle points. Specify that budget before the run;
do not relabel an unmeasured interactive check as a soak test.

The immediate data and desktop priorities are:

1. Mnemosyne's bounded text I/O repair is implemented for development version
   6.5.194; host and target GUI checks pass. A bounded 6.5.226 GUI check also
   recovers one 22-byte CRLF document through a fresh QEMU boot. The host regression
   passes 76 byte-exact, boundary and failure cases against the actual editor
   functions. Empty files, final LF present/absent and uniform CRLF round-trip;
   unrepresentable files and incomplete mounted images are rejected before replacing
   the document. The model allows 255 bytes per row and 512 rows, counting a final
   empty row. See [EDITOR.md](EDITOR.md) for the complete contract and remaining
   limits. A four-CPU QEMU session verified exact saved bytes, rejection with the
   edited document preserved, Ctrl+A/replacement and PS/2 Ctrl+Z. The newer
   save/reopen/cold-boot check covers one document; large-file reboot coverage,
   repeated daily-use sessions and power-loss safety remain open. Failed writes
   are not atomic.
2. Development version 6.5.196 connects desktop configuration to per-user storage,
   checks writes/readback and distinguishes disk, RAM and failure notifications.
   [DESKTOP_CONFIG.md](DESKTOP_CONFIG.md) records the load/save contract and evidence.
   Broader persistence, concurrent-writer and power-loss recovery remain separate gates.
3. Development version 6.5.197 preserves existing RAM file content when
   `vfs_write_file` cannot allocate its replacement, and cleans up failed new-file
   creation. See [VFS_REPLACEMENT.md](VFS_REPLACEMENT.md) for regression coverage,
   transient memory cost and the separate mounted-filesystem/concurrency limits.
4. Development version 6.5.198 rejects unrepresentable VFS writes, prevents growth
   capacity overflow and makes zero-byte positional writes leave the file unchanged.
   [VFS_WRITE_BOUNDS.md](VFS_WRITE_BOUNDS.md) distinguishes host fault injection,
   kernel tests and RAM/EXT2 syscall checks from remaining seek and storage limits.
5. Development version 6.5.199 adds bounded Copy/Cut/Paste and single-step undo to
   Mnemosyne through the shared clipboard. [EDITOR.md](EDITOR.md) records content
   limits, allocation-failure preservation and host/target evidence. Terminal
   output selection, per-login clipboard isolation and per-window job routing
   remain acceptance work.
6. Development version 6.5.200 checks `lseek` arithmetic before adding or narrowing
   offsets. Invalid results preserve the descriptor position; valid positions
   still use the existing unsigned 32-bit range. Host, kernel and ring-3 RAM/EXT2
   evidence is recorded in [VFS_WRITE_BOUNDS.md](VFS_WRITE_BOUNDS.md).
7. Development version 6.5.201 checks output progress in the four N-written
   compiler drivers and regenerates the installable emitters. Short writes are
   completed; failed or zero-progress writes terminate with error. The contract
   and evidence are in [selfhost/README.md](../lang/selfhost/README.md). Accepted
   output prefixes are not rolled back, and redirected files remain non-atomic.
8. Development version 6.5.202 adds the optional `nightfall` Erebus palette through
   the existing per-user configuration. ANSI cell attributes retain their meanings;
   saving other desktop settings preserves the choice and Reset restores `classic`.
   See [DESKTOP_CONFIG.md](DESKTOP_CONFIG.md) for configuration and verification.
9. Development version 6.5.203 defers Selene's page grids for blank tabs and
   reports required rendering allocation failures. [SELENE_MEMORY.md](SELENE_MEMORY.md)
   records the host allocation checks and measured QEMU heap savings. Synchronous
   networking, image budgets and the documented baseline HTML failures remain open.
10. Development version 6.5.204 reconciles the legacy `formtest` with the existing
    glyph and nested-table contracts, handles rendering allocation failure and
    adds the suite to CI. [SELENE_MEMORY.md](SELENE_MEMORY.md) distinguishes this
    test maintenance from renderer changes and documents mutation/failure checks.
11. Development version 6.5.205 checks allocation results in all four N-written
    drivers and regenerates the installed emitters. A rejected state or scratch
    allocation exits with an error instead of dereferencing the failed result.
    [selfhost/README.md](../lang/selfhost/README.md) records the checks and the
    remaining fixed-arena bounds and demand-paging limitations.
12. Development version 6.5.206 rejects the minimum signed BMP height without
    overflowing its magnitude calculation. Existing valid image row order and
    pixels are retained. The decoder section in [ARCHITECTURE.md](ARCHITECTURE.md)
    describes the host regression; browser memory budgets and blocking network
    operations remain separate work.
13. Development version 6.5.207 exposes the existing terminal palettes in the
    Wallpaper panel. Users can change and save the palette with immediate visual
    feedback. [DESKTOP_CONFIG.md](DESKTOP_CONFIG.md) documents per-user persistence,
    failure notices and Reset behaviour.

14. Development version 6.5.209 rejects N++ input seek/tell/read/close failures
    before opening the output. Main files and imported modules share the checked
    path; ordinary and empty regular-file lowering remain supported. The
    [N++ compiler reference](../lang/nppc/README.md) records per-file size bounds,
    cleanup, host fault injection, and the separate target Workbench evidence.

15. Development version 6.5.210 retains snapped/maximized arrangements across
    minimize and taskbar recovery, preserving the original floating rectangle.
    [DESKTOP_CONFIG.md](DESKTOP_CONFIG.md) records session semantics and the
    separate host/GUI evidence; this does not add saved window layouts.

16. Development version 6.5.211 rejects unsupported leading `rm.elf` options
    before removal and supports `--` in both rm implementations. [RM.md](RM.md)
    records operand rules, target file-preservation checks and the separate
    `sh -c` exit-status bug subsequently fixed in 6.5.212.

17. Development version 6.5.212 preserves `sh -c` status and rejects oversized
    commands before parsing; checked `execve` input prevents earlier truncation.
    [SHELL_ENTRY.md](SHELL_ENTRY.md) records string/vector bounds, host and target
    preservation tests, and remaining shell expansion/error-handling limits.

18. Development version 6.5.213 preserves complete mounted paths when dirty
    file mirrors flush, preventing writes to truncated prefix names.
    [MOUNT_PATHS.md](MOUNT_PATHS.md) records checked open bounds, the 96 KiB
    static memory cost, host regressions and exact RAM/EXT2 file checks.
    The separate shell argument truncation is addressed in 6.5.214 below;
    non-atomic mounted saves remain open.

19. Development version 6.5.214 preserves bounded external shell words and
    wildcard paths, rejecting excess words/results or incomplete directory
    pages before that child's redirections and exec. [SHELL_ENTRY.md](SHELL_ENTRY.md)
    records host regressions and exact packaged-shell RAM/EXT2 evidence.
    Variable storage, intermediate expansion and syscall path limits remain.

20. Development version 6.5.215 rejects truncated raw syscall paths and
    overflow in cwd joining, component stacking or normalized output before
    filesystem callbacks. [SYSCALL_PATHS.md](SYSCALL_PATHS.md) records the
    checked bounds and host/guest prefix-preservation evidence. Other VFS
    callers, shell PATH lookup and filesystem transaction limits remain.

21. Development version 6.5.216 evaluates an indexed N++ `Fn` receiver once,
    preventing duplicated index side effects during callback dispatch. The
    [compiler reference](../lang/nppc/README.md) records seven host differential
    regressions, seven target ELF runs with 184 exact EXT2 output bytes, and
    remaining bounds/inference limits. Lowering ran on the host.

22. Development version 6.5.217 checks desktop calculator digit/arithmetic
    overflow and division errors, preserves `ERR`, and allows recovery with
    `C` or a new digit. [CALCULATOR.md](CALCULATOR.md) documents its signed
    integer contract, host regression and QEMU GUI evidence. The application remains in
    kernel context.

23. Development version 6.5.218 keeps user windows headless when no framebuffer
    exists, avoiding a ring-0 divide-by-zero panic from text-mode `win_create`.
    [USERSPACE_GUI.md](USERSPACE_GUI.md) records the regression and remaining
    ownership, stale-handle and process-cleanup requirements for GUI migration.

24. Development version 6.5.219 separates user-window handles from reusable
    registry slots. Stale handles cannot address a replacement window; handle
    exhaustion fails creation instead of wrapping. [USERSPACE_GUI.md](USERSPACE_GUI.md)
    describes the contract and the remaining ownership/lifetime limitations.

25. Development version 6.5.220 detaches the static user-window registry pointer
    during close, preventing the compositor from passing it to `kfree` as if it
    were an allocated application context. [USERSPACE_GUI.md](USERSPACE_GUI.md)
    documents the real compositor teardown regression and its scope.

26. Development version 6.5.221 validates the complete `timeout` duration before
    launching a program. It accepts 1..4294967 whole seconds, with the existing
    numeric parser's optional plus sign and surrounding blanks, and rejects
    suffixes, fractions and values whose millisecond conversion would overflow.
    `tools/test_timeout.py` exercises the actual command and numeric parser with
    virtual ticks/processes under ASan/UBSan: 369 checks cover tick wrap, natural
    exit, single timeout delivery, reaping and failure paths. Both malformed and
    overflowing durations reproduce unwanted launches on 6.5.220. The QEMU GUI
    rejects `1x` and `4294968`, runs an immediately exiting child with `4294967`,
    and verifies a one-second timeout (124), normal exit (0), `mtdemo` progress
    and subsequent terminal commands. GCC14 builds without warnings, 230 kernel
    tests pass and four-CPU login succeeds. This does not change the
    foreground GUI input limitation or add subsecond durations/job control.

27. Development version 6.5.222 extends N++ callback receivers to
    `ui.buttons[index].on_click(...)` for a known struct's pointer field. It
    reuses the single-evaluation invocation helper and supports substituted
    generic holders/dispatch functions and known local holder types. The
    existing indexed-call regression gains direct-field and invalid-type cases;
    [nppc documentation](../lang/nppc/README.md) describes the scope. Bounds,
    arbitrary receiver inference and closure environment lifetime are unchanged.

28. Development version 6.5.223 transfers shared thread-group resources before
    closing descriptors when an unhandled ring-3 exception terminates the leader.
    Surviving `CLONE_VM` threads retain their shared descriptors; the last member
    still closes them. Run `exec /threads.elf --faults` for normal exit, three
    leader exceptions, a worker exception, last-member EOF and a handled exception.
    This uses the existing leader handoff; it does not add window ownership,
    automatic window cleanup or concurrent thread-group migration guarantees.

29. Development version 6.5.224 measures the desktop NET rate over the actual
    elapsed kernel ticks instead of assuming every repaint takes 500 ms. The first
    sample primes the counter; disabling the panel or resetting/loading desktop
    settings clears its history. Re-enabling starts a fresh measurement. Counter
    rollback re-primes the rate, and very large rates saturate without wrapping
    the graph. The existing 32-bit tick clock supports a wrap within an interval;
    intervals spanning an entire clock cycle (about 49.7 days) remain ambiguous.
    `python3 tools/test_desktop_config.py` covers sampling and configuration
    transitions; the existing `netrate` KAT also checks arithmetic boundaries.
    This corrects measurement after a delayed update; foreground shell commands
    can still block desktop event processing until they return.

30. Development version 6.5.225 lowers explicitly indexed N++ callback chains,
    such as `ui[i].buttons[j].on_click(...)`, with one evaluation per index.
    Each intermediate pointer field must refer to a known struct. Value roots,
    additional indexed levels and generic dispatch use existing type metadata.
    `python3 lang/nppc/tests/test_indexed_calls.py` checks 28 executable cases
    and ten invalid inputs. The eleven new cases also run as host-compiled ELFs
    in QEMU, with exact redirected EXT2 output. This provides building blocks
    for callback-based applications; it does not establish GUI event dispatch,
    bounds checking, implicit pointer field access or closure lifetime safety.

31. Development version 6.5.226 validates user-window presentation handles and
    dimensions before scratch allocation or user-buffer copying. Rejected full
    and rectangle updates preserve the existing pixels and do not grow scratch
    buffers. The 43-check sanitized syscall/registry regression includes stale
    handles, invalid extents, resize and allocation/copy failures. In one-CPU
    QEMU, 32 invalid-request cycles leave settled kernel heap use at 46 KiB;
    the same workload on 225 raises it to 8,238 KiB. Further invalid requests
    after valid full/rectangle/resize/close leave it at 113 KiB. A separate GUI
    check verifies full and partial drawing, close/exit, terminal commands and
    drag. Valid calls still retain scratch memory, and window ownership,
    exit cleanup and concurrent lifetime safety remain separate work.

32. Development version 6.5.227 asks Save/Discard/Cancel before closing a dirty
    Mnemosyne document with X or Alt+F4. Failed writes leave the document open;
    user-close destruction waits until input callbacks return. Physical Escape
    reaches the editor through the shared IRQ/poll decoder. There are 72 new
    sanitized close checks, existing I/O/clipboard/window regressions, 230 kernel
    tests and four-CPU login. GUI checks exercise the choices, retained edits,
    exact saved/discarded bytes, minimum-size prompt and terminal dragging.
    See [EDITOR.md](EDITOR.md) for scoped results and rejected capture attempts.
    In 227, forced cleanup, logout/shutdown and Open/Ctrl+O reload are unguarded;
    Save remains non-atomic and no Save As or crash recovery is added.

33. Development version 6.5.228 extends Save/Discard/Cancel to toolbar Open and
    Ctrl+O when the document is modified. Cancel preserves text/selection/undo;
    failed Save blocks reload; failed Open after Discard retains the dirty buffer.
    The pending target survives an unnamed document's default Save path. Close
    requests do not replace a pending reload. The existing host runner adds 304
    sanitized reload checks, and the 227 baseline fails the confirmation check.
    Existing editor/window regressions, 230 kernel tests and four-CPU login pass.
    A four-CPU GUI run exercises both reload inputs and compares five exact disk
    outputs, including recovery after denied Save and removal of the target.
    See [EDITOR.md](EDITOR.md) for behaviour and verification scope. Forced cleanup,
    logout/shutdown, atomic Save and crash recovery remain separate work.

34. Development version 6.5.229 adds explicit first-save destinations and Save As.
    New documents no longer automatically replace the same home `untitled.txt`.
    Next/Enter reviews the normalized destination; Save/Enter confirms it with
    a `May replace` notice. Cancel makes no write. A failed Save As retains the
    source filename and text; pending Close/Reload waits for successful saving.
    F2 and path scrolling remain usable in the minimum window. Host checks cover
    711 destination, 308 reload and 72 close cases; the 228 baseline fails the new
    destination assertion. Existing editor/window regressions, warning-free
    GCC 14, 230 kernel tests and four-CPU login pass. A four-CPU GUI run checks
    independent names, cancellation, denied writes, retry, close and minimum-size
    review, with eight exact disk outputs. See [EDITOR.md](EDITOR.md). Save is
    still non-atomic; exclusive creation, crash recovery and hardware qualification
    remain open.

35. Development version 6.5.230 rejects foreground process identifiers with bits
    outside the 32-bit PID range before changing the signal target. Previously,
    those bits were discarded, so an invalid value could select another process.
    The existing keyboard runner adds 48 sanitizer checks and reproduces the old
    failure. Keyboard and 369 timeout checks, warning-free GCC 14, 230 kernel
    tests and four-CPU login pass. Actual GUI input verifies four rejected values,
    Ctrl+C reaching the original caller, and shell input after a child exits.
    A focused GUI continuation verifies scheduler counters, another command and
    window movement after correcting a test-script output marker. See
    [SYSCALLS.md](SYSCALLS.md). Foreground ownership remains global; the separately
    reproduced desktop input stall during synchronous foreground execution is
    still open.

36. Development version 6.5.231 prevents older background tasks from writing into
    a later command's terminal capture or kernel pipe. Capture identifiers are
    inherited through user-process creation, fork and clone, and retained by exec;
    hook removal waits for an in-flight callback. The existing terminal runner
    passes 26 capture and 229 renderer checks under sanitizers; the old capture
    code fails the expired-child assertion. GCC 14 builds without warnings, 230
    kernel tests and four-CPU login pass. Actual GUI tests preserve foreground
    stdout/stderr, legacy print, clone, fork and exec output; a second terminal
    excludes the first one's delayed background output. Scheduler counters,
    subsequent commands and window movement pass. A synchronized four-CPU pipe
    comparison extracts the background marker from the old kernel's output file,
    while the new kernel sends that marker only to serial/VGA and keeps it out of
    the file. Redirected stdout/stderr match their expected 15 bytes each, and two
    disk fixtures remain unchanged. An earlier timed pipe test missed the overlap;
    a redirection retry used the direct ELF command supported by the kernel shell.
    All test VMs are closed. See [PROCESS.md](PROCESS.md). Background output is not
    delivered back to its original window; global input ownership, synchronous
    foreground stalls and the existing 4096-byte kernel pipe limit remain open.

37. Development version 6.5.232 stops a kernel-shell pipe before running its
    consumer when capture exceeds 4096 bytes or the temporary file cannot be
    opened/written completely. It also rejects a right-hand command that would
    be shortened when appending the temporary path. The existing terminal runner
    passes 30 capture, 16 pipeline-dispatch and 229 renderer sanitizer checks;
    injected open/short-write failures leave the consumer uncalled. Existing cat
    checks, warning-free GCC 14, RTC/ELF contracts, 230 kernel tests and four-CPU
    login pass. In actual GUI tests, the old kernel replaces a destination with
    a truncated 4096-byte prefix from a 4097-byte producer. The corrected kernel
    preserves the original 15-byte destination. A non-writable temporary path
    also skips the consumer and preserves its 11-byte destination; exact 4096-byte
    and empty captures remain usable. Six extracted files match, subsequent input
    and window dragging work, and both VMs close. Offline verification corrected
    the baseline script's mistaken expectation that cat would truncate its target
    after a source read error; that particular target was already preserved.
    See [SHELL_ENTRY.md](SHELL_ENTRY.md). The producer can already have side effects,
    the temporary file is not transactional, and later semicolon segments still
    execute. Per-terminal background routing follows in 6.5.233.

38. Development version 6.5.233 gives Erebus 1.4 numeric output sessions with
    bounded queues. A background child can write to its originating window while
    another terminal runs a foreground command. Closing the original window
    invalidates its session, so a newly opened terminal does not receive that
    child's output. Each terminal owns its ANSI parser; pending output leaves
    the editable input intact. Host sanitizers pass 15,312 capture/session,
    241 renderer/parser and 16 kernel-pipe checks, including slot reuse, queue
    wrap, overflow recovery and identifier exhaustion. GCC14 emits no warnings;
    230 kernel tests and four-CPU login pass. Actual PS/2 GUI checks show output
    in window A while B sleeps, serial-only output after A closes, then a new
    terminal, `mtdemo`, continued commands and window dragging. A second VM
    displays a 3073-byte omission warning after its 8192-byte queue fills;
    serial retains all 800 `QUEUE_PAYLOAD` lines. Both VMs preserve the two disk
    fixtures and close. The test readers report an ancillary socket reset during
    QEMU shutdown; both runners finish successfully. See [PROCESS.md](PROCESS.md).
    This provides neither lossless queues nor asynchronous foreground input:
    foreground jobs can still stall desktop input, and existing scrollback and
    line-length limits remain.

39. Development version 6.5.234 preserves raw stdin escapes across small reads.
    The old reader loses a whole arrow for a one-byte request and consumes the
    next key after an escape fills the buffer. A per-process pending buffer now
    retains the suffix; reads of at least three bytes keep newly encoded escapes
    whole. Readiness includes pending bytes, mode changes discard them and exec
    clears them. Host canonical/raw sanitizer tests cover capacities 1 through 6,
    guard bytes, process isolation and mode/readiness semantics. Keyboard controls
    and 48 foreground PID checks also pass. GCC14 builds without warnings;
    RTC/ELF contracts, 230 kernel tests and four-CPU login pass. In actual GUI
    input, the old kernel returns zero of four expected bytes on the first test;
    the final kernel passes capacities 1, 2, 3, 4 and 16 with exact byte streams.
    `mtdemo`, subsequent input and window dragging pass; two unchanged disk files
    match and both comparison VMs close. An initial candidate trial exposed the
    separate existing prioritisation of extended keys over buffered ASCII; that
    trial failed and is retained, and the final diagnostic distinguishes that
    ordering issue from byte-boundary loss. See [SYSCALLS.md](SYSCALLS.md).
    Input queues remain global; this does not make foreground execution async.

40. Development version 6.5.235 preserves buffered PS/2 letter/navigation order.
    A single cooked queue replaces the extended-before-ASCII priority. IRQ port
    reads, decode and enqueue share the polling lock; login uses the locked byte
    reader, and compositor/readiness checks include navigation. Byte-only readers
    consume unsupported navigation instead of leaving stale keys for the desktop.
    Host sanitizer tests cover US/ES bytes, mixed order, eight full-ring wraps,
    overflow, IRQ/polling, mouse-packet exclusion, serial fallback, independent
    make/break events and control signals after unlocking. Canonical/raw boundary
    tests and 48 foreground PID checks also pass. GCC14 emits no warnings;
    RTC/ELF contracts, 230 KATs and four-CPU login pass. In QEMU, 234 returns
    `ESC[Aaz` instead of `aESC[Az`; 235 passes mixed streams at capacities
    1/2/3/4/16 (5/5/5/5/25 exact bytes), then `mtdemo`, command input and window
    dragging. Two disk fixtures remain exact; both comparison VMs close.
    The shared queue retains 255 entries and drops new keys when full. Input
    ownership and foreground execution remain global/synchronous; this is not
    lossless input or a Unicode/readkey ABI change. See [SYSCALLS.md](SYSCALLS.md).

41. Development version 6.5.236 validates userspace shell pipelines before
    launching their stages. A leading/trailing pipe, an empty intervening stage
    or more than four stages returns status 2 without creating pipeline children
    or applying their redirections. For example, a trailing pipe in
    `echo damaged > /mnt/keep.txt |` no longer lets the first stage overwrite the
    existing file. Quoted pipe/space placeholders retain their existing handling.
    This gate applies to the pipeline after expansion: command substitutions
    still run earlier, and effects from earlier command-list segments remain.
    Host ASan/UBSan checks cover 23 command-entry and 31 preflight cases; the
    old parser fails 21 preflight cases. The existing 886 expansion checks pass.
    GCC14 builds without warnings; RTC/ELF checks, 230 kernel tests and four-CPU
    boot pass. In a separate 235 QEMU baseline, the trailing-pipe command exits 0
    and changes the 10-byte fixture to damaged LF XT. The initial harness expected
    an 8-byte replacement; offline inspection confirms the retained two-byte tail.
    On 236, four malformed pipelines return 2 and leave both disk fixtures exact.
    A real PS/2 four-stage cat/upper/cat/cat pipeline, EOF, six scheduler heartbeats
    and the subsequent command pass; screenshots are inspected and both VMs close.
    It does not provide rollback for pipe/fork allocation failures, job-wide
    signal delivery, input ownership or asynchronous GUI foreground execution.

42. Development version 6.5.237 makes empty pipe reads with a live writer
    respond to deliverable signals. The read detaches its waiter before stopping
    or returning `-EINTR`, preserving the caller's saved address-space and stack
    globals. Default stop/continue restarts the wait. The shared stop helper now
    follows the same signal order and mask as delivery, and leaves caught or
    ignored terminal-stop signals to the normal syscall-return path. Buffered
    data and EOF retain their existing priority over an interrupted empty wait.
    In the separate 236 QEMU baseline, SIGKILL leaves the reader unreaped across
    200 wait polls; closing the writer permits exit 137. The 237 host suite runs
    16 actual-function scenarios under ASan/UBSan with simulated scheduling; the
    baseline fails ten. The production build has no compiler/linker warnings,
    RTC/ELF checks pass, and QEMU passes 230 kernel tests plus four-CPU login.
    Eight target scenarios verify SIGKILL/SIGINT/SIGTERM, a masked signal,
    stop/continue with data or EOF, and caught/ignored SIGTSTP. A four-stage
    interactive pipeline accepts PS/2 input and EOF; six scheduler heartbeats,
    a terminal drag and the following command succeed. Screenshots are inspected,
    both disk fixtures remain exact and the VM closes. Caught or ignored
    SIGTSTP currently returns `-EINTR` from an empty pipe read; this does not claim
    POSIX restart semantics for ignored signals. Multiple-reader wake queues,
    SIGKILL while already stopped, job-wide cancellation, input ownership and
    asynchronous foreground execution remain outside this change.

43. Development version 6.5.238 wakes every blocked reader of a pipe after
    a write or last-writer close. A process-owned pipe-index marker replaces the
    single retained reader pointer. The wake scan uses the live process table,
    clears matching markers and only resumes blocked processes; stopped and
    zombie states stay unchanged. A reader rechecks the buffer and can block
    again if another reader consumed the available bytes. Fork/clone start with
    zeroed markers, and the existing signal/stop path clears its marker before
    returning or parking. The scan is bounded by the 512-process table; this
    does not add cross-core pipe locking or promise reader ordering/fairness.
    The existing host runner passes 23 ASan/UBSan scenarios, including two-reader
    data/EOF, partial-data reblocking, signal interruption, simulated direct
    teardown, unrelated pipes and the 512-reader bound. Baseline 237 fails six.
    Scheduling and teardown are simulated in those host tests, not a target
    lifecycle qualification. The warning-free build passes RTC/ELF checks,
    230 kernel tests and four-CPU login. In a separate four-CPU QEMU desktop run,
    both readers finish with status 0 before cleanup in the data and EOF trials;
    baseline 237 left one blocked in each. All eight earlier target signal
    cases pass, as do the four-stage PS/2 pipeline/EOF, six scheduler heartbeats,
    terminal drag and following command. Screenshots are inspected, the 10-byte
    and 24-byte disk fixtures remain exact, and the VM closes. The final kernel
    is byte-identical to the tested ISO payload. The harness reports an ancillary
    QMP reader reset during intentional shutdown; its main runner exits 0.
    Concurrent shared-descriptor close/reuse, userspace execution on other cores,
    SIGKILL while already stopped, job-wide cancellation and terminal ownership
    remain outside this verification.

44. Development version 6.5.239 prevents a suspended read from consuming bytes
    belonging to a replacement pipe. Each allocation advances a 64-bit slot
    generation. A read records its generation and, after waking, rejects a closed
    or replaced slot before inspecting its buffer. The return is the existing
    invalid-pipe value `-1`; it does not promise to retain a closed pipe object.
    In the separate 238 QEMU baseline, eight CLONE_VM trials close both shared
    descriptors during an empty read, allocate a new pipe and write X; every old
    read consumes X. The host model reproduces that defect on both 237 and 238.
    On 239, 25 ASan/UBSan scenarios pass, with 238 failing the two new lifetime
    cases. One covers a freed slot without replacement; the other also checks
    that the replacement's byte remains available. Scheduling remains simulated
    on the host. The warning-free build passes RTC/ELF checks, 230 kernel tests
    and four-CPU login. In the four-CPU target run, all eight old reads return -1
    without changing the caller's byte; subsequent reads recover all eight X
    bytes from the replacement pipes. Both multi-reader data/EOF trials and all
    eight prior signal cases pass. The four-stage PS/2 pipeline/EOF, six scheduler
    heartbeats, terminal drag and next command pass; screenshots are inspected,
    both disk fixtures remain exact and the VM closes. The final kernel matches
    the tested ISO payload. The harness's ancillary QMP reader resets during
    intentional shutdown; the main runner exits 0. This is pipe-instance checking,
    not cross-core synchronization, a general shared-descriptor lifetime model,
    job-wide cancellation or POSIX restart semantics.

45. Development version 6.5.240 delivers SIGKILL to an already-stopped process.
    Posting SIGKILL makes that process runnable so its kernel wait can unwind;
    the blocking syscall still restores its state and releases temporary resources
    before normal return-path delivery. SIGKILL takes precedence over caught
    lower-numbered signals. A process stopped inside signal dispatch checks for
    pending SIGKILL before returning to user space. In the separate 239 baseline,
    children stopped during a pipe read, sleep and self-stop remain unreaped for
    200 ten-millisecond polls after SIGKILL; SIGCONT cleanup is required, and the
    self-stopped child executes a user-space marker before termination.
    The host harness now extracts the actual dispatch and termination functions:
    30 ASan/UBSan cases pass, while 239 fails the five new wake, dispatch,
    precedence and blocking-cleanup cases. Scheduling, privileged instructions
    and user-memory copying remain simulated in those host tests. The build has
    no compiler/linker warnings and passes RTC/ELF contracts, 230 kernel tests
    and four-CPU login. In QEMU, all three stopped children are reaped with status
    137 without SIGCONT; no resumed-user marker appears. All eight pipe-reuse,
    two multiple-reader and eight earlier signal cases also pass. PS/2 pipeline
    input/EOF, six scheduler heartbeats, terminal dragging and the next command
    pass; screenshots are inspected, both disk fixtures remain exact and the VM
    closes. The final kernel matches the tested ISO payload byte-for-byte. The
    ancillary QMP reader resets during intentional shutdown; the main runner
    exits 0. This change does not add job groups, terminal input ownership, cross-core synchronization
    or signal delivery to arbitrary user code that never reaches a delivery path.

46. Development version 6.5.241 retains all stages in shell job records.
    A background pipeline is Done only after every stage is reaped. `fg` waits
    for all remaining members and preserves an already-collected final-stage
    status; `fg`/`bg` resume every observed stopped member. Foreground waits poll
    all members with a ten-millisecond sleep, so an upstream reader cannot hide
    a later stop. The table reserves capacity before launch, including foreground
    commands that might stop; at 16 occupied slots, external launches fail with
    status 1. Partial pipe/fork failure closes parent pipe ends, kills and reaps
    launched children. This cannot undo redirections already executed by a child.
    Command-substitution children clear inherited job records, and background
    reaping never uses an indiscriminate wait for any child.
    The first GUI trial exposed a sleep-return fault: interrupts were enabled
    after restoring per-CPU user CR3/RSP, allowing another task to overwrite
    those slots before return. Sleep now keeps interrupts masked through that
    return boundary. The failing image and fault log are retained separately.
    Host tests extract actual job and launch functions with simulated syscalls:
    17 ASan/UBSan lifecycle cases pass and baseline 240 fails 16. The existing
    23 entry and 31 pipeline preflight cases also pass. These include capacity
    and launch-failure injection; those failures were not injected in QEMU.
    Six actual sleep-function return scenarios also pass with simulated
    interrupts/scheduling; baseline 240 fails all six. The prior 30 pipe/signal
    host cases pass. The corrected target run repeats three stopped-process,
    eight pipe-reuse, two multiple-reader and eight earlier signal checks.
    The warning-free build passes RTC/ELF contracts, 230 kernel tests and four-CPU
    login. Actual PS/2 input verifies early and late stage exits against `ps`,
    retention of final status 1 through `fg`, two stopped members resumed by `bg`,
    cleanup, substitution, redirection and four-stage input/EOF. Six scheduler
    heartbeats, an 80-by-40 terminal drag and the next command pass; screenshots
    are inspected, two disk fixtures remain exact, and the VM closes. The final
    kernel matches the tested ISO. Kernel foreground signals still target one
    PID: job-wide Ctrl+C, terminal input ownership and externally initiated
    continue notifications remain open. A displayed Stopped job can contain
    other running members; this is not a process-group implementation.
    The ancillary QMP reader reports a reset during intentional shutdown;
    the main runner exits 0 after fixture verification.

47. Development version 6.5.242 keeps futex wait return context atomic.
    The changed-value path and the wake path now keep interrupts masked through
    cleanup and restoration of per-CPU user CR3/RSP. Resumption after the wait
    masks interrupts before changing the blocked flags. This closes the same
    return-boundary hazard corrected for sleep in 241.
    Eight actual-function ASan/UBSan cases cover changed values, real and
    spurious wakes, rejected input, wake-one limits and key isolation. Baseline
    241 fails the four return-boundary cases; the validation/wake controls pass.
    Interrupts, address translation and scheduling are simulated in those host
    tests. The previous 30 pipe/signal and six sleep host cases also pass.
    The local warning-free image passes 230 kernel tests and four-CPU login.
    Five executions of the existing threads program each report 8000 increments,
    intact separate heap blocks and shared descriptors. This does not measure
    futex contention or establish cross-core user scheduling. The first GUI
    attempt stopped because a successful kernel exit message split a counter
    line; the rerun normalizes only that known message and keeps the raw log.
    The final GUI run repeats the prior 21 signal/pipe checks, shell job lifecycle,
    substitution/redirection, four-stage terminal input/EOF, six scheduler
    heartbeats, terminal drag and next command. Screenshots are inspected,
    both disk fixtures remain exact, and the owned VM closes.
    These are local results: 241 CI separately exposed an unsupported SHLX
    instruction in its compiler-produced boot image. Publication of this change
    requires a verified build-baseline correction in a separate increment.

48. Development version 6.5.243 selects the baseline x86-64 instruction set
    explicitly for kernel and user C compilation, including TinyCC support
    objects and the standalone DOOM build. Relevant objects depend on the
    makefile, so an incremental build does not retain objects from the previous
    architecture settings. Custom host compiler selection still works.
    The 241 CI compile/host checks succeeded, but both boot jobs failed before
    self-tests/login: its artifact contains BMI2 SHLX at init_memory address
    0x12f542, unsupported by the documented qemu64 CPU. The exact downloaded CI
    image reproduces that fault locally. This is distinct from the sleep/futex
    return fixes and is retained as failed historical evidence.
    The build-contract test simulates a v3 compiler default, expands 267 actual
    kernel/user compilation recipes and captures four standalone DOOM commands
    in a temporary tree. GCC feature queries verify that their target flags
    disable post-baseline ISA extensions. The previous configuration fails.
    The corrected local build emits no compiler/linker warnings; RTC, nine
    assembly and 103 user-ELF metadata checks pass, along with 230 kernel tests
    and four-CPU login. On the rebuilt image, actual PS/2 input runs libctest
    with ALL PASS, two thread workloads with correct 8000 counters/heap/fds,
    and the previous 21 signal/pipe checks. Four-stage terminal input/EOF,
    six scheduler heartbeats, terminal drag and the next command pass.
    Screenshots are inspected, both disk fixtures remain exact, and the VM
    closes. These checks do not establish general hardware compatibility,
    AVX context support, all DOOM gameplay paths or long-session stability.
    Remote descendant CI is checked separately after publication.
    A retained external probe initially faulted because its absolute printf
    binding was 0x30001564 while the rebuilt libc exports 0x30001567. Relinking
    that unchanged probe against the new libc resolves the fixture mismatch.
    Bundled programs are relinked by make; persisted external binaries need
    rebuilding when this shared-libc layout changes. A stable external ABI or
    runtime symbol resolution remains separate work.

49. Development version 6.5.244 keeps interrupts masked on interrupted and
    stop/resume returns from canonical stdin, raw stdin and timed key reads.
    Their saved per-CPU user CR3/RSP is restored before the syscall can deliver
    the signal or return to userspace. The existing wait loop still enables
    interrupts while yielding; a resumed reader continues waiting for input.
    The actual-function host suite covers 21 cases under ASan/UBSan with
    simulated IRQs, signals and scheduling. Baseline 243 fails nine signal/stop
    return cases; the corrected source and 12 controls pass. Existing canonical
    EOF/edit/echo/capacity and raw escape/pending-byte checks also pass.
    The existing keyevtest gains --wait-return: run it from a Nyx terminal and
    type a at each of its three resumed prompts. It checks SIGUSR1 interruption,
    SIGSTOP/SIGCONT followed by a key, and SIGKILL while stopped for each input
    mode, including handler count, read result, stop status and final reaping.
    On the rebuilt image, two runs pass all 18 checks with real PS/2 keys.
    The warning-free build passes 230 kernel tests and four-CPU login. Two
    threads runs produce correct 8000 counters/heap/fds; libctest, the previous
    21 signal/pipe checks, four-stage input/EOF, six scheduler heartbeats,
    terminal drag and the next command pass. Screenshots are inspected,
    both disk fixtures remain exact, and the owned VM closes.
    These are bounded checks, not a measured race frequency, long-session
    stability, terminal ownership, job-wide signal routing or general syscall
    qualification. The before/after failure is a simulated host invariant;
    no deterministic pre-fix QEMU crash is claimed. Remote CI is recorded
    separately after publication.

50. Development version 6.5.245 removes the compositor's fixed 100,000-PAUSE
    spin per iteration. Its existing 5 ms idle wait now depends on queued keys,
    pointer/wheel changes and button transitions; an open menu or an unchanged
    held button does not disable that wait. Network polling, background tasks,
    application ticks and redraw processing retain their existing order.
    The actual idle decision passes 72 ASan/UBSan host cases; 244 fails 24.
    The 407 configuration checks and 230 QEMU kernel tests pass, with a
    warning-free build and four-CPU boot. A separate graphical run checks a
    four-stage interactive pipeline and EOF, scheduler heartbeats, terminal
    dragging, subsequent commands and opening a disk file in the editor.
    In paired QEMU TCG trials (four qemu64 CPUs, 5 GiB configured RAM, 1024x768,
    static wallpaper), each idle state has three eight-second samples. QEMU
    process CPU time divided by wall time is normalized to one host CPU:
    terminal idle falls from 102.0% to 33.8%; open Start menu 111.8% to 33.7%;
    context menu 111.8% to 34.2%; a stationary held button in terminal content
    111.8% to 34.1%. The sampled guest BSP indicator is 0-1% after the change;
    that counter is not host utilization or total multicore utilization.
    These are bounded emulator measurements, not physical power measurements
    or an idle-CPU guarantee. Active dragging, rendering effects, residual
    emulator overhead, screensaver pacing and long sessions remain separate
    performance work. Existing socket-return corruption is not fixed here.

51. Development version 6.5.246 preserves per-CPU user CR3/RSP across the
    blocking connect, accept, TCP read and UDP recvfrom calls. It masks interrupts
    before restoring those slots, before copyout, descriptor allocation or return.
    Socket polling, wait bounds and transport semantics are unchanged.
    The actual syscall bodies pass 51 ASan/UBSan host cases covering immediate
    and repeated waits, data/error/zero returns, allocation failure, descriptor
    rejection/exhaustion and copyout failure. Version 245 fails 28 of these cases.
    Scheduling and transport are mocked in that host test; disassembly separately
    confirms CLI precedes the restored slots in all four compiled paths.
    The corrected four-byte delayed UDP reproducer now returns exact data and
    reaps its child successfully in QEMU. Existing sockdemo and udpdemo gain
    `--wait-return` checks with separate child sockets and copy-on-write sentinels;
    delayed accept/read, UDP sender metadata and empty-queue timeout pass. The
    existing netstorm completes four children and 100 echo round trips without
    corruption, compared with four faulting children on the audited 244 baseline.
    Default TCP/UDP demos, poll multiplexing, four loopback pings, TCP loopback
    with and without a dropped packet, and RTL8139/slirp DHCP pass. A warning-free
    build, 230 kernel tests, four-CPU boot, scheduler heartbeats and graphical
    terminal interaction pass; two disk fixtures remain exact. These bounded
    checks do not establish general socket lifetime, fork descriptor inheritance,
    signal cancellation or physical-network stability. TCP receive still returns
    zero for both closure and timeout. Exact-source remote CI is recorded separately.

52. Development version 6.5.247 adds selectable flat window decoration in the
    existing wallpaper settings. Dark title bars, unfilled window controls,
    accent focus outlines and flat taskbar tabs use the existing palette. The
    `decoration = flat` or `classic` setting follows system/user configuration
    layering, saves with other desktop settings and resets to classic. The
    taskbar focus marker follows the selected accent in both styles. Existing
    tiling, floating windows, gaps, shadows and rounding remain independent.
    The host configuration/picker harness passes 428 checks; the idle-loop
    regression passes 72 cases. A warning-free build, 230 kernel tests and
    four-CPU QEMU boot pass. Real PS/2 input selects the flat style, tiles terminal
    and editor, restores a floating terminal and drags it before another command.
    A fresh boot restores flat decoration, eight-pixel gaps and Nightfall terminal
    colours; GUI reset restores classic defaults. Two disk fixtures remain exact.
    With QEMU TCG, four qemu64 CPUs, 5 GiB RAM, 1024x768 and a static wallpaper,
    three eight-second idle samples average 34.0% host CPU for classic and 33.5%
    for flat decoration, normalized to one host CPU. Guest BSP samples are 0% in
    both. This small difference does not establish a performance improvement;
    no idle cost increase was observed in this bounded comparison. Active
    rendering, animation, hardware acceleration and physical hardware are not
    covered. This is an optional decoration style, not a complete desktop redesign.
    Exact-source remote CI is recorded separately.

53. Development version 6.5.248 only requests drag/resize damage when the
    window geometry actually changes after clamping. A held stationary title bar,
    resize handle or desktop icon no longer forces repeated desktop repainting.
    Other pending redraws, window ticks, input and release handling are preserved.
    The actual drag/resize/icon blocks and geometry functions pass 87 sanitized
    host checks, covering eight resize directions, minimum and screen limits,
    growth/shrink damage and pre-existing redraw requests; version 247 fails 33.
    The 428 configuration checks and 72 idle decisions also pass. The build has
    no warnings; 230 kernel tests and four-CPU QEMU boot pass.
    Paired QEMU TCG measurements use four qemu64 CPUs, 5 GiB, 1024x768, a static
    wallpaper and three eight-second samples per state. Host process CPU time,
    normalised to one host CPU, averages 108.0% before versus 33.2% after while
    holding the stationary title bar, and 108.9% versus 32.7% while holding the
    stationary bottom-right resize corner. Ordinary terminal idle remains about
    33%. Guest BSP samples fall from 86-90% during these holds to 0%; that is a
    separate counter. Each hold is followed by actual movement, release and a
    successful terminal command; two disk fixtures remain exact. Screenshots
    confirm the moved and resized window. This measures stationary holds, not
    active dragging, animation, all applications or physical hardware. Exact-source
    remote CI is recorded separately.

54. Development version 6.5.249 expands the managed physical frame pool from
    512 MiB to 1 GiB, sharing its bound with the existing runtime identity map.
    Bitmap, reference and pin metadata cover the entire span; detected RAM above
    1 GiB remains outside the pool, and the separate kernel heap remains 16 MiB.
    Sanitized host tests exhaust and recover contiguous and reserved-hole pools
    with 2,586,634 metadata checks; they mock locks and do not touch physical RAM.
    The final QEMU artifact passes 230 kernel tests at 256 MiB/one CPU,
    1536 MiB/one CPU and 5 GiB/four CPUs. The latter two configurations write,
    verify, reference and release 256 frames above 512 MiB, with exact recovery;
    the 256 MiB configuration explicitly skips that upper-frame check. CI now
    requires the upper-frame pass with 1536 MiB. The build has no warnings and
    the separate four-CPU smoke boot reaches login.
    Real PS/2 terminal input runs the user/kernel pointer-boundary test and two
    600 MiB demand-zero/COW/unmap runs. Both ends of all 153,600 user pages are
    checked, the child writes three COW probes, and all parent markers survive.
    Each program exit restores the exact initial free-page count. Two four-CPU
    allocator stress runs report zero integrity failures; the second recovers
    its starting count after the first warms a slab cache. Memory reports agree
    on the 1 GiB pool; mtdemo, dragging and another command remain responsive.
    A 512 MiB VM refuses the oversized test with status 1 and no frame loss.
    The two persistent fixtures remain byte-exact. These are bounded VM checks,
    not full 5 GiB allocation, swap, physical-hardware or long-session validation.
    See [MEMORY.md](MEMORY.md) for the current layout and test contracts.

55. Development version 6.5.250 stops loading the manual `/init.elf`
    demonstration as an unmanaged boot process. It previously retained an address
    space and stack while the scheduler skipped it. The kernel still owns device
    initialization, mounting, account/login setup and desktop startup; PID 1 is
    a bootstrap record, not a userspace service supervisor. The demonstration
    remains available through `exec /init.elf`; [PROCESS.md](PROCESS.md) describes
    startup ownership and the planned supervisor responsibilities.
    Paired four-CPU, 5 GiB QEMU boots show the unmanaged user `init` disappearing,
    one fewer process and 20 additional free pages (80 KiB) at the same terminal
    checkpoint. Five manual demonstration runs exit successfully and leave no
    user processes behind. The first two runs retain three and one slab-cache
    pages respectively, identified by walking the real cache lists; the final
    three runs have exact page recovery, and all five preserve heap usage.
    mtdemo, dragging and a subsequent terminal command remain usable; the two
    persistent fixtures are byte-exact. The build has no warnings, 230 kernel
    tests pass, and the four-CPU smoke boot reaches login. This fixes an unstarted
    boot allocation; it does not implement service supervision or move the GUI
    into userspace. Exact-source remote CI is recorded separately.

56. Development version 6.5.251 enables the supported TLS authentication
    checks by default. An unanchored chain, unmatched hostname or invalid leaf
    dates now require the explicit session development override `tlsstrict off`;
    missing or empty Certificate messages are rejected in either mode. Diagnostics
    distinguish an encrypted connection from authenticated server identity.
    [CRYPTO.md](CRYPTO.md) records the small trust set and incomplete PKIX scope.
    Host tests run the real refusal blocks with mocked certificate verdicts and
    transport: 249 checks pass; the previous source fails 13. These tests establish
    policy and cleanup, not cryptographic verification. The warning-free build
    passes 230 kernel tests and a four-CPU login smoke boot.
    In a four-CPU, 5 GiB QEMU GUI session, a controlled self-signed server is refused
    by default, then returns the exact HTTP response only after the explicit
    override; the real RSA-PSS key-exchange signature and Finished are verified.
    A peer omitting Certificate is refused in both modes. Enforcement is restored;
    loopback ping, tcploop, DHCP and a later terminal command work. Two persistent
    fixtures remain byte-exact. A public-site probe fails to obtain a usable TLS
    exchange; the host probe also resets, so external-site compatibility is not
    established by this run. Exact-source CI is recorded separately.
    This check also reproduces an existing cold-neighbour TCP hang before TLS:
    ARP resolution re-enters network polling while the TCP lock is held. The TLS
    policy tests explicitly warm the neighbour cache to isolate authentication.
    That preparation is not a fix; nonblocking neighbour transmission remains a
    priority follow-up, as do full chain validation and broader negative inputs.

### Oneiros numerical failure handling (6.5.208)

The desktop and `nyxgen` check attention scores, hidden activations, output logits
and normalization totals before consuming their results. Non-finite scores or
invalid totals stop generation. The custom exponential also avoids converting
NaN to an integer. Finite checkpoint validation remains a separate format gate.

The window shows `numeric error; try another model` and preserves the already
produced text. Clear removes the error and output; Dream retries with the loaded
model. To use another checkpoint, replace the model file and reopen the window.
`nyxgen` writes `numerical inference failure` to stderr, frees its buffers and
exits with code 1. It may already have printed the seed or a valid prefix, so a
redirected output must not be treated as complete after that failure.

`python3 tools/test_oneiros_inference.py` runs 19 host scenarios under ASan/UBSan:
normal distributions, finite extreme models, output overflow, GUI stop/clear/retry,
prefix preservation and CLI failure/cleanup. With `--compare-ref 7c02874`, six
additional comparisons show the previous undefined conversion and byte-identical
valid probability vectors for the shipped and zero models. Existing CLI and
20 loader cases pass. The kernel uses the no-SSE ABI and heap-backed scratch;
its new `oneiros-inference` self-test covers refusal and recovery. The build has
zero GCC 14 warnings, passes 230 kernel tests and reaches four-CPU login.

The target trial compiles and installs `nyxgen` through `xbm` inside NyxOS.
The distributed checkpoint produces text and exit 0; the finite extreme model
produces the numerical error and exit 1; restoring the original checkpoint
returns to successful generation. In the GUI, the same extreme model shows the
error without appending an invalid token; Clear removes it and Dream retries.
Reopening the window after restoring the distributed checkpoint resumes text
generation and completes its 240-token run; window dragging remains responsive.
The initial automated reopening typed into the terminal instead of
launching the app; it was repeated through the visible Start menu.
This trial uses QEMU with four CPUs, 5 GiB configured RAM and a private EXT2 disk;
the existing allocator limits still apply.

This is bounded failure handling, not a proof of accurate inference for every
finite model. There is no general model-quality, long-running performance or
hardware qualification. Floating-point exception traps remain outside this work.

### Visual direction and saved personalization

The requested visual direction is a customized desktop in the style of the supplied
references: tiled and floating windows, compact dark panels, colored terminals,
legible monospace typography, a chosen wallpaper, deliberate spacing, thin borders,
and widgets for CPU, RAM, network, media and the clock. Preserve Nyx's identity and
make the choices coherent across Hemera, Erebus and the applications rather than
giving each window unrelated controls or styling.

This is planned work, with the VM as its first test environment. Theme, layout and
widget choices should be user-configurable, saved and restored after logout/reboot.
Hemera loads shared defaults from RAM-backed `/etc/nyx.conf`, then per-user
`nyx.conf` from the logged-in home. From 6.5.196, the picker saves all supported
settings in that home and resets every knob before each login. A mounted home
provides disk storage; a fallback RAM home still loses changes on reboot.
Widgets must use real subsystem measurements or show a clear unavailable state;
decorative or simulated values are not evidence of working resource accounting or
media integration. In particular, the RAM widget must retain the distinction between
detected and managed memory.

Transparency, blur and animation are optional effects. Establish and record a frame
time, CPU and memory budget on the reference VM before accepting them; measure the
desktop both idle and while applications work. Provide an opaque, reduced-motion
fallback that keeps text, focus, controls and keyboard navigation clear.

Acceptance includes paired screenshots of the chosen layouts, keyboard and pointer
interaction checks, persisted settings after reboot, and measurements before/after
effects are enabled. A wallpaper or theme screenshot alone does not establish the
usability or performance of the desktop.

## 3. Establish application isolation

The built-in GUI applications currently execute in kernel context. A fault in one can
therefore take down the desktop and the whole system. That limits what a successful VM
work session establishes about robustness.

Use the existing window syscall ABI and the phased
[user-space GUI plan](USERSPACE_GUI.md) to migrate suitable clients incrementally.
Keep Hemera's display/input responsibilities and the client interface explicit; avoid
rewriting all applications simultaneously.

Acceptance is fault containment: a deliberately failing client can be terminated and
restarted while other windows, input, the compositor and saved files remain usable.
Check buffer ownership, invalid handle behavior, event delivery, close/disconnect and
process-exit cleanup. A working ring-3 window demo is groundwork, not completion of
the application migration.

## 4. Qualify physical hardware after the VM workflows

Start from the documented [UEFI path and reference machine](REAL_HARDWARE.md), and
record the actual machine, firmware settings, PCI IDs and boot artifact for each new
result. Earlier hardware observations are useful starting evidence; they do not certify
a newly built kernel or a different machine.

Proceed in this order:

1. Boot from removable media into the RAM-based desktop. Verify framebuffer geometry,
   colors/rotation, clock, keyboard and a usable pointing method without relying on
   writes to an existing system disk.
2. Qualify required input and storage devices. Test identification and reads first;
   perform write/readback and installation tests on an explicitly designated test disk
   with known fixtures. Keep recovery boot media and the disk layout recorded.
3. Prove persistent boot from the installed disk, then repeat the VM work-session
   checklist and cold-boot/save/reopen trials on that machine.
4. Qualify the network and audio devices needed for that machine's intended use.
   Unsupported devices remain listed as unsupported; emulated RTL8139/SB16 results
   do not establish support for a physical Wi-Fi or audio controller.

Hardware support is accepted per configuration. USB-HID/touchpad, additional NICs,
graphics modes and power-management work should follow a concrete target and a testable
driver contract. Expanding the device list must not weaken the VM regression gate.

## Current limits and open investigations

- Since 6.5.249, the physical allocator in `kernel/mm/memory.c` has a 1 GiB
  managed-frame ceiling, verified with actual upper-frame access and a 600 MiB user
  mapping. RAM detected above that bound remains unavailable to the allocator.
  Further expansion requires page-directory, metadata and address-width work;
  swap is a separate design decision.
- The current identity mapping is bounded by one page directory (1 GiB), and
  `kernel/mm/heap.c` provides a fixed 16 MiB kernel heap. More configured VM RAM alone
  does not enlarge either limit. Measure the exhausted pool before changing its size.
- The built-in GUI remains in ring 0. The user-space migration is a planned robustness
  step with existing ABI groundwork, not a completed isolation boundary.
- Package backup/restore is an integrity improvement, not an atomic power-loss
  transaction. The current filesystem metadata callbacks cannot distinguish every
  physical I/O failure from absence/zero size, and whole publications do not exclude
  concurrent writers. Persistence and recovery claims must retain those limits.
- The reported DOOM launch crash remains an investigation until reproduced, diagnosed
  and verified on the affected configuration. A successful unrelated GUI test does
  not close it.
- The hardware guide currently documents PS/2 input without USB-HID support, RTL8139
  networking without the reference machine's AX200 Wi-Fi, a 32-bpp framebuffer
  requirement, and installed-disk UEFI boot work still in progress. Recheck these
  statements against a tested revision before claiming broader hardware support.
- N++ lowers through N and C. M6.6 remains open and `FnMut` is not implemented; a
  workbench test covers a bounded combination of implemented features. Distinguish
  host compilation, target ELF execution, and compilation performed inside NyxOS.
- Mnemosyne accepts ASCII keyboard input and bounded shared clipboard text. Its
  exact text I/O contract preserves supported bytes but does not add UTF-8 editing;
  the byte-oriented 8×16 font renderer and Spanish keymap do not establish Unicode
  support. [EDITOR.md](EDITOR.md) documents row/path limits, unsupported formats,
  failed-open preservation and the remaining non-atomic save limitation.
- TLS enforces its supported certificate checks by default since 6.5.251, but the
  small pinned trust set and incomplete PKIX validation limit compatibility and
  assurance. `tlsstrict off` explicitly disables that enforcement for the session;
  an encrypted response in that mode does not establish server identity. See
  [CRYPTO.md](CRYPTO.md) for the exact checks and remaining limits.
- Development 6.5.208 rejects invalid Oneiros inference scores and normalization
  totals explicitly. This is not a proof that every finite checkpoint is numerically
  stable, nor that generated text is correct code; see the bounded checks above.

## How work is accepted

Each increment should name the user-visible behavior, the owning subsystem and its
cross-subsystem dependencies, then carry only the necessary implementation and evidence.
An independent review checks error paths and the scope of the claims. CI, host tests,
target KATs, scripted VM integration, visual interaction and physical-machine trials
are complementary evidence; record which ones actually ran.

Before adding a command or port, check the kernel command table, `user/pkg/` and
standalone `user/*.c` tools for an existing implementation. Prefer repairing a
reproduced failure or completing a workflow over adding duplicate utilities. Historical
notes are leads to verify against the current revision, not proof that a subsystem is
complete or that an old failure still reproduces. A canceled CI run is not a passing
run: confirm that the replacement run covers a descendant containing the change.

Keep the roadmap and linked subsystem documents consistent with the evidence after
each accepted stage. Publish commits under the agreed delivery cadence; create version
releases only when explicitly requested. Progress is measured by closed acceptance
criteria and a smaller list of reproducible failures, not by version counts.
