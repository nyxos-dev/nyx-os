# PIT timing and desktop idle cost

Development version 6.5.265 programs PIT channel 0 in mode 2 (rate generator),
using the low byte followed by the high byte and a binary count. The control
word is `0x34`. The previous `0x36` selected mode 3 (square wave), despite the
mode-2 comment. The requested frequency remains 1000 Hz and the divisor remains
1193. This does not lower the requested scheduler rate or the AP timer rate.

## Verification

`python3 tools/test_pit_programming.py` executes the actual initialization code
with captured port writes. It checks the channel, byte access order, mode,
binary count and divisor. The 6.5.264 source compiles but fails the mode check.
This host test verifies programming; it does not emulate a physical timer.

Target qualification uses QEMU 10.2.1 TCG, the default PC machine, four CPUs,
5 GiB RAM, RTL8139 networking and a disposable EXT2 overlay. The terminal is
idle during measurements. Each phase has three eight-second host CPU samples,
followed by three timer-counter intervals. The host measure is total QEMU
process CPU time divided by elapsed time, expressed as a percentage of one host
core. It includes emulation and every virtual CPU. The desktop indicator samples
the BSP and is a different measurement.

The A-B-A sequence returns to the original 6.5.264 image for its final control:

| Image | Median QEMU CPU, one host core | BSP ticks per second |
|---|---:|---:|
| 6.5.264, initial control | 36.60% | 1988-1994 |
| 6.5.265, mode 2 | 27.60% | 999.5-999.8 |
| 6.5.264, repeated control | 36.34% | 1989-1994 |

The candidate uses about 24% less QEMU CPU in this idle workload than the mean
of the two control medians. AP rates remain approximately 1000 Hz. The guest BSP
indicator remains near zero and does not measure the host saving. A separate
single-CPU trial measures 999.6-999.8 BSP ticks per second; it does not establish
a paired CPU saving for that configuration.

The warning-free build passes 230 kernel tests and a four-CPU startup check.
In the candidate's GUI session, both `mtdemo` threads finish three heartbeats,
all three AP workers execute with zero BSP worker operations, and a second SMP
allocator stress run balances its page count with zero integrity failures.
The terminal accepts another command and its window can be dragged afterwards.
Each A-B-A VM closes normally; two control files per VM remain byte-identical
under independent offline EXT2 inspection.

The full File Manager keyboard workflow also passes on the candidate image:
captured Ctrl search and selection, repeated copy, refusal of an occupied move
destination, successful retry with the retained cut selection, and protection
of rename input from file shortcuts. Six search toggles leave the terminal
responsive. Offline inspection verifies eleven expected file contents and the
absence of the moved source after the VM closes normally.

## Scope and remaining limits

The control word now agrees with the intended rate-generator contract. The
measured effect is specific to this emulator configuration. Both PIT modes can
provide periodic interrupts on hardware; no hardware-wide doubling claim is
made. QEMU's [PIT output and transition model](https://github.com/qemu/qemu/blob/v10.2.1/hw/timer/i8254_common.c)
and [I/O APIC delivery implementation](https://github.com/qemu/qemu/blob/v10.2.1/hw/intc/ioapic.c)
provide context for the observed mode-dependent delivery. Measurements of NyxOS
remain the acceptance evidence.

The AP reload remains uncalibrated. Its measured rate is approximately 1000 Hz
in this VM; it is not inferred from the BSP tick ratio. Hardware APIC frequency,
lost interrupts under load, long sessions, active animation and power management
remain open. Tick-based waits still depend on serviced interrupts; wall-clock
uptime uses the RTC. Interrupt tracing is performed separately because logging
perturbs delivery and cannot serve as a CPU benchmark.

See [DAILY_USE.md](DAILY_USE.md) for the wider acceptance criteria and
[DESKTOP_CONFIG.md](DESKTOP_CONFIG.md) for display settings and earlier evidence.
