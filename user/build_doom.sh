#!/bin/bash
# build_doom.sh (v5.9.33) — build the recovered doomgeneric engine into a NyxOS
# userspace ELF (user/doom.elf). Engine sources: kernel/doom_src (freestanding, own
# libc shim headers). NyxOS platform layer: user/doomgeneric_nyxos.c. Userspace libc
# backing (stdio over syscalls, sbrk malloc, etc.): user/doom_userlibc.c — this
# replaces the kernel-coupled doom_utils.c, so that TU is excluded. No libc.so is
# linked: doom_userlibc.c IS the libc. Run from the repo root (WSL).
#   -fcommon                       : old code relies on tentative-definition merging
#   -fno-builtin ...loop-distribute : keep our memcpy/memset from recursing
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/kernel/doom_src"
OUT="$ROOT/user/doom_obj"; mkdir -p "$OUT"
CC=${CC:-gcc}; LD=${LD:-ld}
CF="-std=gnu99 -Os -ffreestanding -nostdlib -m64 -march=x86-64 -mtune=generic -mno-red-zone -fcommon -fno-builtin \
  -fno-tree-loop-distribute-patterns -w -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
  -DNORMALUNIX -I$SRC -I$ROOT/user"
# Exclude the other platforms' backends and the SDL/allegro/GUS sound; doom_utils is
# replaced by user/doom_userlibc.c.
EX='doomgeneric_sdl|doomgeneric_win|doomgeneric_xlib|doomgeneric_allegro|doomgeneric_emscripten|doomgeneric_linuxvt|doomgeneric_soso|doomgeneric_sosox|i_sdlsound|i_sdlmusic|i_allegrosound|i_allegromusic|gusconf|doom_utils'
for f in "$SRC"/*.c; do
  b=$(basename "$f" .c); echo "$b" | grep -qE "^($EX)\$" && continue
  $CC $CF -c "$f" -o "$OUT/$b.o"
done
$CC $CF -c "$ROOT/user/doomgeneric_nyxos.c" -o "$OUT/doomgeneric_nyxos.o"
$CC $CF -c "$ROOT/user/doom_userlibc.c"     -o "$OUT/doom_userlibc.o"
$CC $CF -c "$ROOT/user/doom_nullsound.c"    -o "$OUT/doom_nullsound.o"
$LD -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o "$ROOT/user/doom.elf" \
  "$ROOT/user/crt0.o" "$OUT"/*.o
echo "built user/doom.elf ($(stat -c%s "$ROOT/user/doom.elf" 2>/dev/null || echo ?) bytes)"
