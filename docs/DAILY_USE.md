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
   6.5.194; host and target GUI checks pass, while reboot acceptance remains open. The host regression
   passes 76 byte-exact, boundary and failure cases against the actual editor
   functions. Empty files, final LF present/absent and uniform CRLF round-trip;
   unrepresentable files and incomplete mounted images are rejected before replacing
   the document. The model allows 255 bytes per row and 512 rows, counting a final
   empty row. See [EDITOR.md](EDITOR.md) for the complete contract and remaining
   limits. A four-CPU QEMU session verified exact saved bytes, rejection with the
   edited document preserved, Ctrl+A/replacement and PS/2 Ctrl+Z. Close the broader
   persistence gate only after save/reopen/reboot checks; disk extraction after a
   single session does not establish that gate. Failed writes are not atomic.
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

- The physical allocator in `kernel/mm/memory.c` has a 512 MiB managed-frame ceiling.
  Reporting detected RAM above that limit does not remove it. The paging code and frame
  metadata must be considered together before increasing it; adding swap is a separate
  design decision, not a correction to a misleading memory counter.
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
- TLS certificate verification currently defaults to non-strict mode. Loading an
  HTTPS page therefore does not establish authenticated transport under a daily-use
  trust policy; stricter behavior needs compatibility and negative-certificate tests.
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
