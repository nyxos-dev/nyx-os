#!/bin/bash
# mkuefi.sh — build a UEFI-bootable NyxOS ISO (GRUB x86_64-efi + multiboot2 kernel).
#
# NyxOS's normal build.ps1 makes a legacy-BIOS ISO (GRUB i386-pc). Modern machines
# (e.g. an Ice Lake laptop) boot UEFI-only, so this script emits a GRUB-EFI ISO that
# real firmware — and QEMU+OVMF — can boot. The kernel is unchanged; only the GRUB
# platform differs. See docs/REAL_HARDWARE.md for flashing + firmware notes.
#
# Usage:  tools/mkuefi.sh [output.iso]     (run from anywhere; paths are derived)
# Needs:  grub-mkrescue, xorriso, mtools (mformat). The x86_64-efi GRUB platform is
#         used from /usr/lib/grub if installed system-wide (apt: grub-efi-amd64-bin),
#         otherwise fetched WITHOUT root via `apt-get download` + `dpkg-deb -x`.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/NyxOS-uefi.iso}"
KERNEL="$REPO/kernel/nyx-kernel.bin"

[ ! -d "$OUT" ] || { echo "[mkuefi] output is a directory: $OUT" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "[mkuefi] kernel not built: $KERNEL — run build.ps1 (or make -C kernel) first"; exit 1; }
for t in grub-mkrescue xorriso mformat; do
  command -v "$t" >/dev/null || { echo "[mkuefi] missing '$t' — apt install grub-common grub-efi-amd64-bin xorriso mtools"; exit 1; }
done

# Locate the x86_64-efi GRUB platform (modinfo.sh + *.mod). Prefer a system install.
EFI="${NYX_GRUB_EFI_DIR:-/usr/lib/grub/x86_64-efi}"
if [ -n "${NYX_GRUB_EFI_DIR:-}" ] && [ ! -f "$EFI/modinfo.sh" ]; then
  echo "[mkuefi] invalid NYX_GRUB_EFI_DIR: missing modinfo.sh" >&2
  exit 1
fi
if [ ! -f "$EFI/modinfo.sh" ]; then
  echo "[mkuefi] x86_64-efi GRUB platform not installed system-wide; fetching (no root)..."
  CACHE="$HOME/.cache/nyx-grub-efi"
  if [ ! -f "$CACHE/x86_64-efi/modinfo.sh" ]; then
    rm -rf "$CACHE"; mkdir -p "$CACHE"
    ( cd "$CACHE" && apt-get download grub-efi-amd64-bin && dpkg-deb -x grub-efi-amd64-bin_*.deb x && cp -a x/usr/lib/grub/x86_64-efi . )
  fi
  EFI="$CACHE/x86_64-efi"
fi

STAGE="$(mktemp -d)"
PENDING=""
cleanup() {
  rm -rf -- "$STAGE"
  if [ -n "$PENDING" ]; then rm -f -- "$PENDING"; fi
}
trap cleanup EXIT
# Keep an existing image intact until the new one has been produced successfully.
# A sibling temporary file also keeps the final rename on the same filesystem.
PENDING="$(mktemp -- "${OUT}.pending.XXXXXX")"
mkdir -p "$STAGE/image/boot/grub"
cp "$KERNEL" "$STAGE/image/boot/nyx-kernel.bin"
cat > "$STAGE/image/boot/grub/grub.cfg" <<'CFG'
set timeout=10
set default=0
insmod all_video
# Default is landscape (correct for most PCs). Portrait/rotated panels (e.g. an 8" UMPC)
# scan out a landscape LFB, so an unrotated desktop looks sideways there — pick a "rotate"
# entry that comes up upright ("rotate=" is read by the kernel; 0/90/180/270 clockwise).
menuentry 'NyxOS (landscape)' {
    multiboot2 /boot/nyx-kernel.bin
    module2 /boot/nyx-kernel.bin nyxkernel.bin
    boot
}
menuentry 'NyxOS (rotate 90 - portrait)' {
    multiboot2 /boot/nyx-kernel.bin rotate=90
    module2 /boot/nyx-kernel.bin nyxkernel.bin
    boot
}
menuentry 'NyxOS (rotate 270 - portrait, other way)' {
    multiboot2 /boot/nyx-kernel.bin rotate=270
    module2 /boot/nyx-kernel.bin nyxkernel.bin
    boot
}
menuentry 'NyxOS (rotate 180)' {
    multiboot2 /boot/nyx-kernel.bin rotate=180
    module2 /boot/nyx-kernel.bin nyxkernel.bin
    boot
}
CFG

# grub-mkrescue's -d takes a single-platform module dir → a UEFI (El Torito 0xEF) ISO
# that is USB-bootable under UEFI. (Secure Boot must be OFF; the loader is unsigned.)
if ! grub-mkrescue -d "$EFI" -o "$PENDING" "$STAGE/image" > "$STAGE/build.log" 2>&1; then
  cat "$STAGE/build.log" >&2
  echo "[mkuefi] image creation failed; destination unchanged" >&2
  exit 1
fi
cat "$STAGE/build.log"
if [ ! -s "$PENDING" ]; then
  echo "[mkuefi] builder produced no image; destination unchanged" >&2
  exit 1
fi
mv -f -- "$PENDING" "$OUT"
PENDING=""
echo "[mkuefi] wrote $OUT ($(stat -c%s "$OUT") bytes) — UEFI-bootable, Secure Boot OFF"
