# Booting NyxOS on real hardware (UEFI)

NyxOS normally boots inside QEMU as a legacy-BIOS ISO. Modern laptops are UEFI-only, so
this page covers building a UEFI image, putting it on a USB stick, and what to expect on a
physical machine. The kernel is identical — only the GRUB bootloader platform changes
(`i386-pc` → `x86_64-efi`), and NyxOS draws into the framebuffer GRUB sets up via UEFI GOP.

## Build the UEFI ISO

```
.\build-uefi.ps1
```

This compiles the kernel and wraps it into `NyxOS-uefi.iso` (a GRUB-EFI rescue image).
It needs WSL with `grub-mkrescue`, `xorriso`, and `mtools`. The `x86_64-efi` GRUB modules
come from a system install (`sudo apt install grub-efi-amd64-bin`) if present, otherwise
they are fetched without root via `apt-get download`. Under the hood it just runs
`tools/mkuefi.sh`, which you can call directly on Linux.

An unsuccessful image build returns an error and preserves any existing output.
The destination is replaced only after the builder succeeds with a nonempty
temporary image. `NYX_GRUB_EFI_DIR` can select an existing GRUB EFI module directory;
an explicit directory without `modinfo.sh` is rejected rather than downloaded.

## Flash to USB

Write `NyxOS-uefi.iso` to a USB stick with **Rufus** in **DD / Image mode** (not the
Windows "ISO" install mode). The image is a GRUB El-Torito UEFI rescue ISO; DD mode copies
it byte-for-byte so UEFI firmware finds `/EFI/BOOT/BOOTX64.EFI` and boots it.

## Firmware settings

- **Secure Boot: OFF.** NyxOS's GRUB and kernel are unsigned; Secure Boot will refuse them.
- **CSM / Legacy boot: not needed.** NyxOS boots native UEFI now.
- Boot the USB from the firmware boot menu (usually F12 / F9 / Esc at power-on).

## What works on real hardware today

NyxOS boots to its **full desktop on real metal** — proven on the reference i5-1035G7
laptop, not just under emulation (see [`docs/real-hardware.jpg`](real-hardware.jpg)):

- **Graphics.** GRUB sets a GOP framebuffer (typically 1024×768×32) and hands it to the
  kernel in the multiboot2 framebuffer tag; NyxOS maps it and renders the desktop on the
  real Iris Plus iGPU — correct BGRX colors, full-screen. This is the make-or-break path.
- **Login + desktop, RAM-only.** The whole system lives in the initramfs — no disk needed.
  You log in at the keyboard (nyx/nyx) and land on the compositor: wallpaper, taskbar,
  windows, the Terminal + `nyxfetch`.
- **Input.** The built-in keyboard works, and **MouseKeys** (hold **Alt** + **W/A/S/D** to
  move the pointer, **Alt+Space**/**Alt+Q** to click) drives the whole GUI from the keyboard
  alone — needed because the touchpad is USB-HID and NyxOS has no USB stack (see below).
- **Portrait panels.** A small UMPC panel that scans out sideways is corrected with the
  `rotate=90` (or `180`/`270`) boot cmdline; the UEFI GRUB menu offers the rotated entries.

The kernel prints, early in boot, the framebuffer geometry and pixel channel layout, e.g.:

```
[INIT] GRUB framebuffer 1024x768x32 pitch=4096 at 0x80000000
[INIT] GRUB fb pixel format: R@16/8 G@8/8 B@0/8
```

NyxOS renders BGRX (`R@16 G@8 B@0`). If a panel reports a different layout the kernel logs
`[WARN] fb is not BGRX...` — so if reds and blues look swapped on screen, that line is why.

## Known limitations on bare metal

These are driver gaps, not bugs:

- **USB input.** NyxOS speaks PS/2 (i8042) only, with no USB-HID stack. The reference
  laptop's **built-in** keyboard works because its firmware keeps legacy-USB i8042 emulation
  on; a separately-plugged **USB** keyboard/mouse is not picked up, and the touchpad (USB-HID)
  isn't driven. MouseKeys (above) covers pointing from the working keyboard.
- **Storage.** The **NVMe** driver now brings the SSD up on real hardware — IDENTIFY, reads,
  and writes all work on the target's Silicon Motion controller, and `nyxinstall --uefi
  <disk> confirm` writes a GPT + ESP + ext2 install to it. Booting *from* the installed disk
  is still being brought up, so the USB image remains the reliable path; a RAM-only boot
  needs no disk at all. ATA-PIO is also supported for older/emulated disks.
- **Network.** RTL8139 only; there is no 802.11 / Intel AX200 Wi-Fi stack. No networking.
- **Framebuffer must be 32bpp.** Universal on Intel GOP; other depths fall back to the
  QEMU-only VBE path and won't light up a real panel.

## Reference test machine

The physical target is an Ice Lake laptop: **Intel Core i5-1035G7 / Iris Plus Graphics
(Gen11) / 16 GB LPDDR4 / NVMe SSD / Intel Wi-Fi 6 AX200**. NyxOS has booted there to the full
desktop with keyboard login (the built-in keyboard rides the firmware's legacy-USB i8042
emulation), and its NVMe SSD is driven for reads, writes, and installs.

## Developer test under QEMU + OVMF

You don't need real hardware to exercise the UEFI path — OVMF is a real UEFI firmware:

```
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -cdrom NyxOS-uefi.iso -m 512
```

(Copy `/usr/share/OVMF/OVMF_VARS_4M.fd` to a writable `OVMF_VARS.fd` first.)
