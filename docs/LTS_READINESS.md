# Next LTS: stabilization and release acceptance

The next LTS is in preparation. It is not yet qualified for release. The proposed
release name is `v6.6.0-LTS`; development builds keep their own `6.5.x` versions
until the release scope and acceptance checks close. The published LTS remains
`v6.5.0-LTS`.

Stabilization takes priority over new features. A successful build or a clean
static analysis does not establish that an interactive work session, a saved file
or a supported hardware configuration is reliable. See [DAILY_USE.md](DAILY_USE.md)
for the broader roadmap and [TESTING.md](TESTING.md) for existing checks.

## Release gates

| Gate | Acceptance requirement | Current disposition |
|---|---|---|
| Build and security analysis | Clean rebuild, matching runtime version, successful CI on the candidate revision, and no unresolved CodeQL alerts in shipped code, including the integrated compiler. | Open until the candidate analysis completes. |
| Process lifetime | Diagnostics own process names/data after dropping the scheduler lock; spawn, signals, exit and collection preserve surviving tasks and reclaim resources. | Open: remaining raw process-table readers require correction and regression checks. |
| Persistence and recovery | Save, close, reopen and reboot preserve exact bytes; failed saves retain a usable document and report failure. Interrupted replacement must have a documented recovery path. | Open: EXT2 is not journalled; atomic replacement, concurrent writers and power-loss recovery are not qualified. |
| Interactive desktop | Keyboard, pointer, focus, workspace switching, resizing, dialogs and logout remain usable throughout representative foreground workloads. | Open: foreground command execution and shared capture/CWD ownership still need work. |
| Memory pressure | Allocation failures are bounded, resources recover after repeated workloads, and reported managed RAM matches the allocator. | Open: managed memory is capped at 1 GiB and the fixed kernel heap at 16 MiB; larger VM RAM is not an expansion of these limits. |
| Startup and sessions | Boot reaches login reliably; persistent settings reload; process/session responsibilities and recovery behaviour are documented. | Open: the kernel drives boot and login; the userspace init demonstration is not a service supervisor. |
| Applications and tools | Editor/file workflows, shell/pipes, package recovery, C compilation, N/N++ and Oneiros pass their documented contracts on installed binaries. | Existing focused checks must be rerun on the release candidate; unsupported features remain explicit. |
| Networking | Supported device, local HTTP/TLS fixtures, failure timeouts and certificate checks behave as documented. | Open: restricted trust/PKIX support and shared descriptor concurrency limit the supported scope. |
| Release images | Test the exact BIOS and UEFI images, not a separately built kernel; verify normal boot, self-tests, installation, reboot and checksums. | Open: release packaging and the complete firmware/CPU/RAM matrix remain to be qualified. |

## Qualification matrix

The reference environment is QEMU with a disposable EXT2 disk for destructive
tests and a separate persistent disk for normal work. Record the emulator and
firmware versions, CPU model, one/four CPUs, RAM, storage controller, framebuffer,
input devices and NIC with each run. Cover BIOS and UEFI/OVMF; include memory
settings below and above the allocator ceiling. Measure idle and interactive CPU
cost using the same setup before and after a change, and distinguish host QEMU
consumption from guest counters.

Physical hardware support is limited to configurations actually exercised with
the candidate images and the checklist in [REAL_HARDWARE.md](REAL_HARDWARE.md).
Unexecuted cases stay open. Passing emulation does not qualify physical devices.

## Candidate and publication procedure

1. Close reproducible crash, corruption, data-loss and security blockers. Recheck
   the remaining limitations against the intended release scope.
2. Build from a clean source tree using the existing toolchain and snapshot rules.
   Preserve local user configuration separately from the candidate image inputs.
3. Run the focused regressions, complete CI, the kernel self-tests and the target
   interaction/persistence matrix on the same candidate. Repeat affected checks
   whenever a fix changes that candidate.
4. Produce BIOS/UEFI images and checksums, then test those exact images. Prepare
   concise release notes describing supported workflows, installation/recovery
   procedures and remaining limits.
5. Publish the LTS only after the gates close and release publication is authorized.
   Resume feature development after the LTS; retain regressions for later fixes.

Test logs, screenshots and publication records stay in the private work area.
The wiki documents current usage and behaviour, with release history limited to
published releases. It is not a per-commit evidence ledger.
