# ============================================================
# kernel/Makefile - Compilación del kernel NyxOS x86_64
# ============================================================

CC = gcc
AS = nasm
LD = ld

CFLAGS = -std=gnu99 -ffreestanding -Os -Wall -Wextra -Wshadow \
         -nostdlib -nostartfiles -nodefaultlibs \
         -fno-stack-protector -fno-pie -m64 \
         -mno-red-zone -mno-sse -mno-mmx -mno-sse2 \
         -mno-3dnow -mcmodel=large \
         -ffunction-sections -fdata-sections \
         -MMD -MP \
         -I.



ASFLAGS = -f elf64

LDFLAGS = -T linker.ld -nostdlib -m elf_x86_64

# Kernel sources live in subfolders (core/ mm/ net/ crypto/ gui/… ) but the .o objects
# stay flat in kernel/: VPATH lets make find each .c/.h/.asm in its folder while the
# recipes still write foo.o here. Quoted #includes are relative to the source file's own
# folder (e.g. gui/apps/selene_win.c -> "../../core/kernel.h"), so no -I per folder needed.
SRCDIRS = core mm proc fs drivers/video drivers/input drivers/net drivers/audio drivers/misc \
          net crypto crypto/tls image gui/core gui/apps gui/games auth
vpath %.c $(SRCDIRS)
vpath %.h $(SRCDIRS)
vpath %.asm core

OBJS_KERNEL = boot.o kernel.o datefmt.o ini.o factor.o semver.o comm.o seq.o urlcodec.o paste.o tac.o wc.o expand.o unexpand.o fold.o csv.o tsort.o base58.o cut.o uniq.o od.o join.o calc.o json.o fletcher.o murmur3.o bech32.o gdt.o idt.o isr.o isr_stubs.o irq.o \
        memory.o heap.o slab.o process.o syscall.o signal.o mmap.o pipe.o vfs.o ext2.o filetype.o tar.o ata.o pci.o nvme.o blockdev.o gpt.o fat.o net.o udp.o \
        ip.o arp.o ethernet.o rtl8139.o icmp.o timer.o keyboard.o screen.o \
        serial.o umoddi3.o paging.o switch.o stubs.o dhcp.o \
        vbe.o fb.o tri.o mat4.o mouse.o gui.o font.o compositor.o userwin.o speaker.o tcp.o \
        sb16.o ac97.o terminal_win.o fileman_win.o paint_win.o taskman_win.o \
        editor_win.o imageview_win.o soundtest_win.o calc_win.o wallpaper_win.o minesweeper_win.o pong_win.o voxel_win.o fire_win.o matrix_win.o lava_win.o mandel_win.o julia_win.o particles_win.o rotor_win.o fill_win.o life_win.o blobs_win.o snake_win.o tetris_win.o games_win.o selene_win.o oneiros_win.o \
        elf.o initramfs.o rtc.o apic.o dns.o ipaddr.o ipcalc.o numparse.o namecheck.o urlparse.o fnmatch.o leb128.o crc16.o ansi.o caldate.o http.o tls.o curve25519.o ed25519.o tls_prf.o aes_gcm.o csprng.o der.o base64.o base32.o base16.o base85.o fnv.o utf8.o p256.o p384.o rsa.o sha512.o x509.o inflate.o deflate.o png.o bmp.o gif.o jpeg.o smp.o trampoline.o bootsplash.o auth.o login.o sha256.o sha1.o md5.o totp.o hkdf.o chacha20.o siphash.o poly1305.o chacha20poly1305.o blake2s.o aes_cmac.o aes_ctr.o sha3.o ct.o aes_cbc.o aes_kw.o shared_libc.o

OBJS = $(OBJS_KERNEL)

# Pin the default goal to `all` BEFORE -include pulls in the .d files below. Each
# auto-generated .d holds a rule like `kernel.o: kernel.c ...`; since -include sits
# above the `all:` target, on any incremental build (where .d files already exist)
# the first included rule would otherwise HIJACK the default goal to kernel.o — so a
# bare `make` (and build.ps1, which runs `make -C kernel` with no target) built only
# kernel.o and never relinked nyx-kernel.bin, silently shipping a stale kernel.
# `make clean` deletes the .d files, which is the only reason clean builds worked.
.DEFAULT_GOAL := all

# Auto-generated per-object header dependencies (from -MMD): every kernel .o now
# depends on EVERY header it #includes, so editing e.g. tcp.h's TCP_MAX_CONNS
# rebuilds tcp.o/net.o. Before this, only kernel.h was a tracked prerequisite, so
# changing a macro in any other header silently left STALE objects — a bump that
# "didn't take effect" until an unrelated .c edit forced a recompile (this masked a
# TCP_MAX_CONNS capacity fix as a phantom "Heisenbug"). `-include` ignores the .d
# files that don't exist yet (first build) or belong to .asm objects.
-include $(OBJS_KERNEL:.o=.d)

all: $(USER_ELFS) nyx-kernel.bin

# linker.ld is a prerequisite so editing the SECTIONS layout (e.g. capturing the
# -mcmodel=large .lbss/.ldata into .bss/.data) forces a relink. It is NOT passed to
# ld as an input object — the recipe uses $(OBJS), not $^, and LDFLAGS has -T linker.ld.
# (Before this, a linker.ld-only change left nyx-kernel.bin stale on incremental builds,
# because every .o was unchanged — the same stale-build class as the tcp.h/default-goal bugs.)
nyx-kernel.bin: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Header prerequisites are tracked automatically now (-MMD + the -include above), so
# a change to ANY included header — kernel.h's KERNEL_VERSION, tcp.h's TCP_MAX_CONNS,
# … — rebuilds the objects that include it. (kernel.h stays listed as a belt-and-
# braces prereq for the very first build, before any .d files exist.)
%.o: %.c core/kernel.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# User-space programs (x86_64 native)
USER_DIR = ../user
USER_ELFS = $(USER_DIR)/hello.elf $(USER_DIR)/init.elf $(USER_DIR)/spin.elf $(USER_DIR)/fdleak.elf $(USER_DIR)/hello_nyx.elf $(USER_DIR)/args.elf \
            $(USER_DIR)/sh.elf $(USER_DIR)/echo.elf $(USER_DIR)/upper.elf $(USER_DIR)/cat.elf $(USER_DIR)/wc.elf $(USER_DIR)/ls.elf \
            $(USER_DIR)/grep.elf $(USER_DIR)/head.elf $(USER_DIR)/tail.elf \
            $(USER_DIR)/mkdir.elf $(USER_DIR)/rm.elf $(USER_DIR)/touch.elf $(USER_DIR)/sort.elf $(USER_DIR)/uniq.elf $(USER_DIR)/cut.elf $(USER_DIR)/tr.elf $(USER_DIR)/fold.elf $(USER_DIR)/cksum.elf $(USER_DIR)/nl.elf $(USER_DIR)/xxd.elf $(USER_DIR)/base64.elf $(USER_DIR)/base32.elf $(USER_DIR)/tac.elf $(USER_DIR)/seq.elf $(USER_DIR)/diff.elf $(USER_DIR)/comm.elf $(USER_DIR)/tee.elf $(USER_DIR)/paste.elf $(USER_DIR)/factor.elf $(USER_DIR)/expand.elf $(USER_DIR)/od.elf $(USER_DIR)/basename.elf $(USER_DIR)/dirname.elf $(USER_DIR)/unexpand.elf $(USER_DIR)/sha256sum.elf $(USER_DIR)/md5sum.elf $(USER_DIR)/rev.elf $(USER_DIR)/split.elf $(USER_DIR)/sha512sum.elf $(USER_DIR)/join.elf $(USER_DIR)/cmp.elf $(USER_DIR)/sum.elf $(USER_DIR)/column.elf $(USER_DIR)/printf.elf $(USER_DIR)/dd.elf $(USER_DIR)/find.elf $(USER_DIR)/test.elf $(USER_DIR)/true.elf $(USER_DIR)/false.elf \
             $(USER_DIR)/ps.elf $(USER_DIR)/kill.elf $(USER_DIR)/top.elf $(USER_DIR)/edit.elf $(USER_DIR)/dltest.elf $(USER_DIR)/yes.elf $(USER_DIR)/arch.elf \
            $(USER_DIR)/pmap.elf $(USER_DIR)/free.elf $(USER_DIR)/env.elf $(USER_DIR)/date.elf $(USER_DIR)/cal.elf $(USER_DIR)/sleep.elf \
            $(USER_DIR)/cp.elf $(USER_DIR)/mv.elf $(USER_DIR)/uname.elf $(USER_DIR)/whoami.elf $(USER_DIR)/which.elf $(USER_DIR)/clear.elf \
            $(USER_DIR)/fault.elf $(USER_DIR)/less.elf $(USER_DIR)/more.elf \
            $(USER_DIR)/pstorm.elf $(USER_DIR)/bigread.elf $(USER_DIR)/segv.elf \
            $(USER_DIR)/sockdemo.elf $(USER_DIR)/srvdemo.elf $(USER_DIR)/udpdemo.elf \
            $(USER_DIR)/nc.elf $(USER_DIR)/sigrec.elf $(USER_DIR)/alarmdemo.elf \
            $(USER_DIR)/polldemo.elf $(USER_DIR)/wget.elf $(USER_DIR)/netstorm.elf $(USER_DIR)/stat.elf \
            $(USER_DIR)/vfsfill.elf $(USER_DIR)/fsx.elf $(USER_DIR)/stacktest.elf $(USER_DIR)/threads.elf $(USER_DIR)/vmtest.elf $(USER_DIR)/munmaptest.elf $(USER_DIR)/mprotecttest.elf $(USER_DIR)/fbtest.elf $(USER_DIR)/keyevtest.elf $(USER_DIR)/wintest.elf $(USER_DIR)/libctest.elf \
            $(USER_DIR)/doom.elf $(USER_DIR)/tcc.elf

# Flags for Nyx C output (GCC: statement-exprs, __auto_type). -mno-red-zone is mandatory.
NYXCFLAGS = -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR)

$(USER_DIR)/hello.elf: $(USER_DIR)/hello.asm
	nasm -f elf64 $< -o $(USER_DIR)/hello.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/hello.o

$(USER_DIR)/spin.elf: $(USER_DIR)/spin.asm
	nasm -f elf64 $< -o $(USER_DIR)/spin.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/spin.o

$(USER_DIR)/fdleak.elf: $(USER_DIR)/fdleak.asm
	nasm -f elf64 $< -o $(USER_DIR)/fdleak.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/fdleak.o

$(USER_DIR)/init.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/init.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/init.o

$(USER_DIR)/args.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/args.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/args.o

$(USER_DIR)/args.o: $(USER_DIR)/args.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Toolchain self-test (Phase 2): exercises the libc primitives the in-OS compiler needs.
$(USER_DIR)/libctest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/libctest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/libctest.o

$(USER_DIR)/libctest.o: $(USER_DIR)/libctest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# ---- TinyCC port (Phase 2 in-OS C compiler) --------------------------------
# Vendored TinyCC 0.9.27 (user/tcc/, LGPL) cross-compiled with host GCC against
# the NyxOS libc via the shim headers in user/tcc/nyxshim. ONE_SOURCE: compiling
# tcc.c pulls in the whole compiler + the x86_64/ELF backend; nyxshim.c supplies
# the POSIX OS calls tcc expects. NOT in USER_ELFS yet (M0: it links; it gets
# embedded once tcc runs in-OS). Build it directly: make ../user/tcc.elf
TCC_DIR    = $(USER_DIR)/tcc
TCC_CFLAGS = -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -w \
             -I$(TCC_DIR)/nyxshim -I$(TCC_DIR) -I$(USER_DIR)
$(TCC_DIR)/tcc_main.o: $(TCC_DIR)/tcc.c
	$(CC) $(TCC_CFLAGS) -c $< -o $@
$(TCC_DIR)/nyxshim.o: $(TCC_DIR)/nyxshim/nyxshim.c
	$(CC) $(TCC_CFLAGS) -c $< -o $@
# libtcc1: tcc's own runtime library (float<->64-bit-int + long-double helpers its
# codegen emits, e.g. __floatundixf/__fixunsxfdi). Header-free; built as a NyxOS-target
# object and shipped in the initramfs as /libtcc1.o, so the in-OS `cc` can LINK the
# tcc-built tcc_self.o into a runnable tcc_self.elf (v6.4.11).
$(TCC_DIR)/libtcc1.o: $(TCC_DIR)/lib/libtcc1.c
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(TCC_DIR)/include -c $< -o $@
$(USER_DIR)/tcc.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(TCC_DIR)/tcc_main.o $(TCC_DIR)/nyxshim.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(TCC_DIR)/tcc_main.o $(TCC_DIR)/nyxshim.o

# A deliberately-crashing program (NULL deref) to test ring-3 fault -> SIGSEGV.
$(USER_DIR)/fault.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/fault.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/fault.o

$(USER_DIR)/fault.o: $(USER_DIR)/fault.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Catchable-SIGSEGV demo (installs a handler, derefs NULL, recovers via longjmp).
$(USER_DIR)/segv.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/segv.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/segv.o
$(USER_DIR)/segv.o: $(USER_DIR)/segv.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Userspace TCP sockets demo (connects to the kernel loopback echo on 127.0.0.1:7).
$(USER_DIR)/sockdemo.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sockdemo.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sockdemo.o
$(USER_DIR)/sockdemo.o: $(USER_DIR)/sockdemo.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Userspace TCP server demo (fork: parent bind/listen/accepts, child connects).
$(USER_DIR)/srvdemo.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/srvdemo.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/srvdemo.o
$(USER_DIR)/srvdemo.o: $(USER_DIR)/srvdemo.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Userspace UDP sockets demo (SOCK_DGRAM sendto/recvfrom over loopback).
$(USER_DIR)/udpdemo.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/udpdemo.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/udpdemo.o
$(USER_DIR)/udpdemo.o: $(USER_DIR)/udpdemo.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# nc — netcat: bridge stdin/stdout to a TCP or UDP socket.
$(USER_DIR)/nc.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/nc.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/nc.o
$(USER_DIR)/nc.o: $(USER_DIR)/nc.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# wget — userspace HTTP/1.0 client (TCP sockets + a DNS-over-UDP resolver).
$(USER_DIR)/wget.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/wget.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/wget.o
$(USER_DIR)/wget.o: $(USER_DIR)/wget.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# netstorm — concurrent multi-process TCP stress repro (fork + loopback echo).
$(USER_DIR)/netstorm.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/netstorm.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/netstorm.o
$(USER_DIR)/netstorm.o: $(USER_DIR)/netstorm.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# stat — demo of the v5.8.80 POSIX syscalls (stat/fstat/lseek/getppid).
$(USER_DIR)/stat.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/stat.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/stat.o
$(USER_DIR)/stat.o: $(USER_DIR)/stat.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# vfsfill — VFS directory-child-cap hardening test (v5.8.81)
$(USER_DIR)/vfsfill.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/vfsfill.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/vfsfill.o
$(USER_DIR)/vfsfill.o: $(USER_DIR)/vfsfill.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# fsx — dup / rename / per-process cwd demo (v5.8.82)
$(USER_DIR)/fsx.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/fsx.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/fsx.o
$(USER_DIR)/fsx.o: $(USER_DIR)/fsx.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# vmtest — proves the user/kernel address boundary (user_v2p hardening)
$(USER_DIR)/vmtest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/vmtest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/vmtest.o
$(USER_DIR)/vmtest.o: $(USER_DIR)/vmtest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
# stacktest — demand-grown user stack + guard-page demo (v5.8.85)
$(USER_DIR)/stacktest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/stacktest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/stacktest.o
$(USER_DIR)/stacktest.o: $(USER_DIR)/stacktest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# threads — clone(CLONE_VM) + futex mutex demo (v5.8.87)
$(USER_DIR)/threads.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/threads.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/threads.o
$(USER_DIR)/threads.o: $(USER_DIR)/threads.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# sigrec — multi-fault recovery demo (sigsetjmp/siglongjmp catch repeated SIGSEGV).
$(USER_DIR)/sigrec.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sigrec.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sigrec.o
$(USER_DIR)/sigrec.o: $(USER_DIR)/sigrec.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# alarmdemo — timer-driven SIGALRM via alarm().
$(USER_DIR)/alarmdemo.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/alarmdemo.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/alarmdemo.o
$(USER_DIR)/alarmdemo.o: $(USER_DIR)/alarmdemo.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# polldemo — poll() I/O multiplexing over a pipe + a socket.
$(USER_DIR)/polldemo.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/polldemo.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/polldemo.o
$(USER_DIR)/polldemo.o: $(USER_DIR)/polldemo.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# The pager. less.elf and more.elf are the same program (full-screen pager).
$(USER_DIR)/less.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/less.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/less.o

$(USER_DIR)/more.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/less.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/less.o

$(USER_DIR)/less.o: $(USER_DIR)/less.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# pstorm + bigread: deterministic fork+pipe+execve churn reproducer (debug-only).
$(USER_DIR)/pstorm.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/pstorm.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/pstorm.o
$(USER_DIR)/pstorm.o: $(USER_DIR)/pstorm.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/bigread.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/bigread.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/bigread.o
$(USER_DIR)/bigread.o: $(USER_DIR)/bigread.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# The userspace shell + its coreutils (crt0 + libc + program)
$(USER_DIR)/sh.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sh.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sh.o

$(USER_DIR)/echo.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/echo.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/echo.o

$(USER_DIR)/upper.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/upper.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/upper.o

$(USER_DIR)/cat.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/cat.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/cat.o

$(USER_DIR)/wc.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/wc.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/wc.o

$(USER_DIR)/ls.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/ls.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/ls.o

$(USER_DIR)/grep.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/grep.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/grep.o

$(USER_DIR)/mkdir.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/mkdir.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/mkdir.o

$(USER_DIR)/rm.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/rm.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/rm.o

$(USER_DIR)/touch.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/touch.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/touch.o

$(USER_DIR)/sort.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sort.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sort.o

$(USER_DIR)/find.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/find.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/find.o

$(USER_DIR)/head.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/head.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/head.o

$(USER_DIR)/tail.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/tail.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/tail.o

$(USER_DIR)/ps.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/ps.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/ps.o

$(USER_DIR)/kill.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/kill.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/kill.o

$(USER_DIR)/top.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/top.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/top.o

$(USER_DIR)/edit.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/edit.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/edit.o

# dltest links against libc (--just-symbols) but NOT libdemo — it loads that at
# runtime with dlopen()/dlsym().
$(USER_DIR)/dltest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/dltest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/dltest.o

# libdemo.so: a demo shared library prelinked at 0x31000000 (above libc), loaded
# on demand by dlopen. Self-contained, so a plain link with no libc.
$(USER_DIR)/libdemo.so: $(USER_DIR)/libdemo.o
	$(LD) -nostdlib -m elf_x86_64 -e 0 -Ttext 0x31000000 -o $@ $(USER_DIR)/libdemo.o

$(USER_DIR)/pmap.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/pmap.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/pmap.o

$(USER_DIR)/free.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/free.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/free.o

$(USER_DIR)/env.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/env.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/env.o

$(USER_DIR)/date.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/date.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/date.o

$(USER_DIR)/cal.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/cal.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/cal.o
$(USER_DIR)/cal.o: $(USER_DIR)/cal.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/xxd.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/xxd.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/xxd.o
$(USER_DIR)/xxd.o: $(USER_DIR)/xxd.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/tr.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/tr.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/tr.o
$(USER_DIR)/tr.o: $(USER_DIR)/tr.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/fold.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/fold.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/fold.o
$(USER_DIR)/fold.o: $(USER_DIR)/fold.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/cksum.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/cksum.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/cksum.o
$(USER_DIR)/cksum.o: $(USER_DIR)/cksum.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/base64.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/base64.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/base64.o
$(USER_DIR)/base64.o: $(USER_DIR)/base64.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/base32.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/base32.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/base32.o
$(USER_DIR)/base32.o: $(USER_DIR)/base32.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/comm.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/comm.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/comm.o
$(USER_DIR)/comm.o: $(USER_DIR)/comm.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/join.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/join.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/join.o
$(USER_DIR)/join.o: $(USER_DIR)/join.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/cmp.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/cmp.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/cmp.o
$(USER_DIR)/cmp.o: $(USER_DIR)/cmp.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/sum.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sum.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sum.o
$(USER_DIR)/sum.o: $(USER_DIR)/sum.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/column.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/column.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/column.o
$(USER_DIR)/column.o: $(USER_DIR)/column.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/printf.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/printf.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/printf.o
$(USER_DIR)/printf.o: $(USER_DIR)/printf.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/dd.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/dd.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/dd.o
$(USER_DIR)/dd.o: $(USER_DIR)/dd.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/tee.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/tee.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/tee.o
$(USER_DIR)/tee.o: $(USER_DIR)/tee.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/paste.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/paste.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/paste.o
$(USER_DIR)/paste.o: $(USER_DIR)/paste.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/factor.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/factor.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/factor.o
$(USER_DIR)/factor.o: $(USER_DIR)/factor.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/expand.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/expand.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/expand.o
$(USER_DIR)/expand.o: $(USER_DIR)/expand.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/od.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/od.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/od.o
$(USER_DIR)/od.o: $(USER_DIR)/od.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/basename.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/basename.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/basename.o
$(USER_DIR)/basename.o: $(USER_DIR)/basename.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/dirname.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/dirname.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/dirname.o
$(USER_DIR)/dirname.o: $(USER_DIR)/dirname.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/unexpand.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/unexpand.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/unexpand.o
$(USER_DIR)/unexpand.o: $(USER_DIR)/unexpand.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/sha256sum.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sha256sum.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sha256sum.o
$(USER_DIR)/sha256sum.o: $(USER_DIR)/sha256sum.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/md5sum.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/md5sum.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/md5sum.o
$(USER_DIR)/md5sum.o: $(USER_DIR)/md5sum.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/rev.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/rev.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/rev.o
$(USER_DIR)/rev.o: $(USER_DIR)/rev.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/split.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/split.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/split.o
$(USER_DIR)/split.o: $(USER_DIR)/split.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/sha512sum.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sha512sum.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sha512sum.o
$(USER_DIR)/sha512sum.o: $(USER_DIR)/sha512sum.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/sleep.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/sleep.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/sleep.o

$(USER_DIR)/cp.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/cp.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/cp.o

$(USER_DIR)/mv.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/mv.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/mv.o

$(USER_DIR)/uname.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/uname.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/uname.o

$(USER_DIR)/whoami.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/whoami.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/whoami.o

$(USER_DIR)/which.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/which.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/which.o

$(USER_DIR)/clear.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/clear.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/clear.o

$(USER_DIR)/sh.o: $(USER_DIR)/sh.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/echo.o: $(USER_DIR)/echo.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/upper.o: $(USER_DIR)/upper.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/cat.o: $(USER_DIR)/cat.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/wc.o: $(USER_DIR)/wc.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/ls.o: $(USER_DIR)/ls.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/grep.o: $(USER_DIR)/grep.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/head.o: $(USER_DIR)/head.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/uniq.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/uniq.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/uniq.o
$(USER_DIR)/uniq.o: $(USER_DIR)/uniq.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/cut.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/cut.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/cut.o
$(USER_DIR)/cut.o: $(USER_DIR)/cut.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/nl.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/nl.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/nl.o
$(USER_DIR)/nl.o: $(USER_DIR)/nl.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/tac.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/tac.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/tac.o
$(USER_DIR)/tac.o: $(USER_DIR)/tac.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/seq.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/seq.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/seq.o
$(USER_DIR)/seq.o: $(USER_DIR)/seq.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/diff.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/diff.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/diff.o
$(USER_DIR)/diff.o: $(USER_DIR)/diff.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/tail.o: $(USER_DIR)/tail.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/mkdir.o: $(USER_DIR)/mkdir.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/rm.o: $(USER_DIR)/rm.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/touch.o: $(USER_DIR)/touch.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/sort.o: $(USER_DIR)/sort.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/find.o: $(USER_DIR)/find.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/ps.o: $(USER_DIR)/ps.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/kill.o: $(USER_DIR)/kill.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/top.o: $(USER_DIR)/top.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/edit.o: $(USER_DIR)/edit.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/dltest.o: $(USER_DIR)/dltest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/libdemo.o: $(USER_DIR)/libdemo.c
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/pmap.o: $(USER_DIR)/pmap.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/free.o: $(USER_DIR)/free.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/env.o: $(USER_DIR)/env.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/date.o: $(USER_DIR)/date.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/sleep.o: $(USER_DIR)/sleep.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/cp.o: $(USER_DIR)/cp.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/mv.o: $(USER_DIR)/mv.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/uname.o: $(USER_DIR)/uname.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/whoami.o: $(USER_DIR)/whoami.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/which.o: $(USER_DIR)/which.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/clear.o: $(USER_DIR)/clear.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

$(USER_DIR)/crt0.o: $(USER_DIR)/crt0.asm
	nasm -f elf64 $< -o $@

# Nyx C "hello": crt0 + runtime + transpiled output. Reuses the existing crt0
# (it calls main / exit). hello_nyx.c is nyxc's C output from hello.nyx.
$(USER_DIR)/hello_nyx.elf: $(USER_DIR)/crt0.o $(USER_DIR)/nyxrt.o $(USER_DIR)/hello_nyx.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ \
	    $(USER_DIR)/crt0.o $(USER_DIR)/nyxrt.o $(USER_DIR)/hello_nyx.o

$(USER_DIR)/nyxrt.o: $(USER_DIR)/nyxrt.c $(USER_DIR)/nyxrt.h
	$(CC) $(NYXCFLAGS) -c $< -o $@

$(USER_DIR)/hello_nyx.o: $(USER_DIR)/hello_nyx.c $(USER_DIR)/nyxrt.h
	$(CC) $(NYXCFLAGS) -c $< -o $@

$(USER_DIR)/libc.o: $(USER_DIR)/libc.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Shared libc: libc.o prelinked at the fixed VA SHARED_LIBC_BASE (0x30000000). The
# kernel loads this at boot and maps it into every process; programs link against
# it with --just-symbols (above) instead of bundling libc.o. (-e 0 silences the
# missing-_start warning; libc.so has no entry point — it's never executed as a
# program.) It ships in the initramfs (see mkinitramfs) as /libc.so.
$(USER_DIR)/libc.so: $(USER_DIR)/libc.o
	$(LD) -nostdlib -m elf_x86_64 -e 0 -Ttext 0x30000000 -o $@ $(USER_DIR)/libc.o

# TinyCC's va_list runtime (__va_start/__va_arg), compiled from the vendored
# user/tcc/lib/va_list.c into a small object shipped in the initramfs. `cc` links it so
# tcc-compiled code that DEFINES a varargs function (tcc's <stdarg.h> lowers va_start/
# va_arg to these) can link. It references only memset + abort, both in our libc.o. (v6.3.5)
$(USER_DIR)/va_list.o: $(TCC_DIR)/lib/va_list.c
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -c $< -o $@

$(USER_DIR)/init.o: $(USER_DIR)/init.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# Generate initramfs C header.
# FORCE + content-compare: the recipe runs on EVERY make (FORCE is a phony that is
# always "out of date"), so a freshly-built user/*.elf can never be missed. But we
# regenerate into a temp and only replace initramfs_data.h when the bytes actually
# differ, so initramfs.o relinks only on a real change — not every build. This is
# the canonical regenerate-only-if-changed idiom, and here it also defends against
# the WSL /mnt/c (9p) mtime skew that once let a stale initramfs — missing a
# just-built .elf — ship silently despite the .h being newer on disk. Companion to
# the -MMD/-MP auto-deps that track the hand-written headers.
MKINITRAMFS = ../tools/mkinitramfs.py
fs/initramfs_data.h: FORCE $(MKINITRAMFS) $(USER_ELFS) $(USER_DIR)/libc.so $(USER_DIR)/libdemo.so $(USER_DIR)/va_list.o $(TCC_DIR)/nyxshim.o $(TCC_DIR)/libtcc1.o
	@python3 $(MKINITRAMFS) $@.tmp c >/dev/null
	@if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv $@.tmp $@; echo "  [initramfs] regenerated $@"; fi

initramfs.o: initramfs.c fs/initramfs_data.h

.PHONY: FORCE
FORCE:

# SMP trampoline — flat binary embedded as ELF object
trampoline.bin: trampoline.asm
	$(AS) -f bin -o $@ $<

trampoline.o: trampoline.bin
	$(LD) -r -b binary -m elf_x86_64 -o $@ $<

# Persistent storage: create a fresh empty ext2 disk for /mnt. Boot NyxOS with
# `-hda ext2-test.img` and files written under /mnt survive reboots (it auto-mounts at
# boot). Needs mkfs.ext2 (e2fsprogs); NyxOS's ext2 driver uses classic 1024-byte
# blocks / 128-byte inodes. `make clean` never removes it (it's your data).
disk: ../ext2-test.img
../ext2-test.img:
	dd if=/dev/zero of=$@ bs=1M count=16
	mkfs.ext2 -q -b 1024 -I 128 -O ^resize_inode,^dir_index,^ext_attr $@
	@echo "Created $@ (16 MB ext2). Boot: qemu-system-x86_64 -cdrom NyxOS.iso -hda ext2-test.img"
.PHONY: disk

clean:
	rm -f *.o *.d *.bin fs/initramfs_data.h fs/initramfs_data.h.tmp trampoline.bin $(USER_DIR)/*.o $(USER_DIR)/*.elf $(USER_DIR)/hello.o
	rm -rf $(USER_DIR)/doom_obj

# doom.elf: the recovered doomgeneric engine (kernel/doom_src) built into a userspace
# ELF by an out-of-makefile recipe — it compiles ~80 engine TUs + the NyxOS platform
# layer + doom_userlibc (the syscall-backed libc) with system gcc and links crt0 with
# NO libc.so. See user/build_doom.sh; it self-locates, so `make` triggers it here and
# mkinitramfs then globs the resulting user/doom.elf into the image as `/doom.elf`.
$(USER_DIR)/doom.elf: $(USER_DIR)/doomgeneric_nyxos.c $(USER_DIR)/doom_userlibc.c $(USER_DIR)/doom_nullsound.c $(USER_DIR)/build_doom.sh $(USER_DIR)/crt0.o
	bash $(USER_DIR)/build_doom.sh

# munmaptest: partial-munmap VMA split/trim test (v5.9.27)
$(USER_DIR)/munmaptest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/munmaptest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/munmaptest.o
$(USER_DIR)/munmaptest.o: $(USER_DIR)/munmaptest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# mprotecttest: partial-mprotect VMA split test (v5.9.28)
$(USER_DIR)/mprotecttest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/mprotecttest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/mprotecttest.o
$(USER_DIR)/mprotecttest.o: $(USER_DIR)/mprotecttest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# fbtest: fullscreen framebuffer syscalls test (v5.9.29 DOOM enabler)
$(USER_DIR)/fbtest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/fbtest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/fbtest.o
$(USER_DIR)/fbtest.o: $(USER_DIR)/fbtest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# keyevtest: raw key-event path test (v5.9.30 DOOM DG_GetKey enabler)
$(USER_DIR)/keyevtest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/keyevtest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/keyevtest.o
$(USER_DIR)/keyevtest.o: $(USER_DIR)/keyevtest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# wintest: user-space window syscalls test (v6.4.354, #77) — opens a real desktop window
$(USER_DIR)/wintest.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/wintest.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/wintest.o
$(USER_DIR)/wintest.o: $(USER_DIR)/wintest.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h $(USER_DIR)/uwin.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@

# test / [ (POSIX conditional) + true + false: shell-scripting primitives
$(USER_DIR)/test.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/test.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/test.o
$(USER_DIR)/test.o: $(USER_DIR)/test.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/true.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/true.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/true.o
$(USER_DIR)/true.o: $(USER_DIR)/true.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/false.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/false.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/false.o
$(USER_DIR)/false.o: $(USER_DIR)/false.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/yes.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/yes.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/yes.o
$(USER_DIR)/yes.o: $(USER_DIR)/yes.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
$(USER_DIR)/arch.elf: $(USER_DIR)/crt0.o $(USER_DIR)/libc.so $(USER_DIR)/arch.o
	$(LD) -nostdlib -m elf_x86_64 -e _start -Ttext 0x10000 -o $@ $(USER_DIR)/crt0.o --just-symbols=$(USER_DIR)/libc.so $(USER_DIR)/arch.o
$(USER_DIR)/arch.o: $(USER_DIR)/arch.c $(USER_DIR)/libc.h $(USER_DIR)/syscall.h
	$(CC) -std=gnu99 -Os -ffreestanding -nostdlib -m64 -mno-red-zone -I$(USER_DIR) -c $< -o $@
