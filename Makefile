ifeq ($(shell uname -s),Linux)
%:
	@$(MAKE) -f Makefile.linux $@
all:
	@$(MAKE) -f Makefile.linux all

else

# Makefile for NexsOS1 (AMD64 / AArch64)
# Cross-compilation for bare-metal kernel
# ==============================================================================
# Configuration
# ==============================================================================

# Target Architecture (default to aarch64)
ARCH ?= aarch64

# Release Versioning (default V9.9.9)
VERSION ?= V0.0.4.1
RELEASE_BASE = release/$(VERSION)
RELEASE_DIR = $(RELEASE_BASE)/$(ARCH)

# Normalize ARCH typos
ifeq ($(findstring aarch64,$(ARCH)),aarch64)
    override ARCH := aarch64
else ifeq ($(findstring amd64,$(ARCH)),amd64)
    override ARCH := amd64
endif

# Common Compiler Flags
COMMON_FLAGS = -Wall -Wextra -Werror -Wpedantic -Wshadow -Wwrite-strings \
               -Wmissing-prototypes -Wstrict-prototypes \
               -ffreestanding -fno-builtin -nostdlib -nostartfiles \
               -fno-common -fstack-protector-strong \
               -fno-pic -fno-pie \
               -fno-omit-frame-pointer \
               -O2 -g

ifeq ($(ARCH), amd64)
    CROSS_COMPILE ?= x86_64-elf-
    KERNEL_DIR = kernel
    BOOT_DIR   = boot/amd64
    ARCH_DIR   = $(KERNEL_DIR)/arch/amd64
    
    ARCH_CFLAGS = -DARCH_AMD64 -mno-red-zone -mcmodel=large
    ASFLAGS = -g --fatal-warnings
    
    LDFLAGS_BOOT = -nostdlib -static -z noexecstack -T $(BOOT_DIR)/linker.ld
    LDFLAGS_KERN = -nostdlib -static -z noexecstack -T $(ARCH_DIR)/kernel.ld -Map build/kernel.map
    
    CFLAGS_BOOT = $(filter-out -mcmodel=large,$(COMMON_FLAGS)) $(INCLUDE) -m32
    QEMU = qemu-system-x86_64
else
    CROSS_COMPILE ?= aarch64-none-elf-
    KERNEL_DIR = kernel
    BOOT_DIR   = boot/aarch64
    ARCH_DIR   = $(KERNEL_DIR)/arch/aarch64
    
    ARCH_CFLAGS = -DARCH_AARCH64 -mcpu=cortex-a57
    ASFLAGS = -mcpu=cortex-a57 -g --fatal-warnings
    
    LDFLAGS_BOOT = -nostdlib -static -z noexecstack -T $(BOOT_DIR)/linker.ld
    LDFLAGS_KERN = -nostdlib -static -z noexecstack -T $(ARCH_DIR)/kernel.ld -Map build/kernel.map
    
    CFLAGS_BOOT = $(COMMON_FLAGS) $(INCLUDE)
    QEMU = qemu-system-aarch64
endif

CFLAGS = $(COMMON_FLAGS) $(ARCH_CFLAGS) $(INCLUDE)
CXXFLAGS = $(COMMON_FLAGS) $(ARCH_CFLAGS) $(INCLUDE) -fno-exceptions -fno-rtti

# Tools
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
AS      = $(CROSS_COMPILE)as
LD      = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
MKDISK  = tools/mkdisk
# Rootfs inode layout: --extents (mkfs.ext4-like, default) or --legacy
# (direct/indirect pointer blocks, the pre-B1 format kept for regression).
MKDISK_LAYOUT ?= --extents

# Resolve grub-mkrescue
GRUB_MKRESCUE := $(shell command -v i686-elf-grub-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null || echo "")

# Directories
BUILD_ROOT = build
BUILD_DIR  = $(BUILD_ROOT)/$(ARCH)
USER_DIR   = user
USER_SYS_DIR = $(USER_DIR)/sys
USER_LIB_DIR = $(USER_SYS_DIR)/lib
USER_BIN_DIR = $(USER_SYS_DIR)/bin
USER_ARCH_DIR = $(USER_DIR)/arch/$(ARCH)
INCLUDE    = -I$(KERNEL_DIR)/include -I$(ARCH_DIR)/include -Iinclude/api

# Output files
BOOTLOADER_ELF = $(BUILD_DIR)/bootloader.elf
BOOTLOADER_BIN = $(BUILD_DIR)/bootloader.bin
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
USER_ELF   = $(BUILD_DIR)/init.elf
DISK_IMG   = $(BUILD_DIR)/disk.img
# AArch64 only: pre-generated QEMU virt DTB used to pass a valid x0 to the kernel.
# QEMU does not set x0 for bare-metal ELF kernels (no Linux image magic), so we
# generate the DTB for the exact machine config and pass it via -dtb.  QEMU then
# loads it into RAM and sets x0 = dtb_load_address in the aarch64 boot stub.
VIRT_DTB   = $(BUILD_DIR)/virt.dtb

# ==============================================================================
# Source Files
# ==============================================================================

ifeq ($(ARCH), amd64)
BOOT_SOURCES = \
    $(BOOT_DIR)/header.S \
    $(BOOT_DIR)/stage1.S \
    $(BOOT_DIR)/stage2.S

KERN_ASM_SOURCES = \
    $(ARCH_DIR)/boot/start.S \
    $(ARCH_DIR)/cpu/isr_stubs.S \
    $(ARCH_DIR)/cpu/syscall.S \
    $(ARCH_DIR)/cpu/context.S \
    $(ARCH_DIR)/boot/trampoline.S

KERN_C_SOURCES = \
    $(ARCH_DIR)/cpu/cpu.c \
    $(ARCH_DIR)/cpu/idt.c \
    $(ARCH_DIR)/cpu/gdt.c \
    $(ARCH_DIR)/cpu/msr.c \
    $(ARCH_DIR)/cpu/syscall_hal.c \
    $(ARCH_DIR)/cpu/apic.c \
    $(ARCH_DIR)/mm/mmu.c \
    $(ARCH_DIR)/mm/tlb.c \
    $(ARCH_DIR)/mm/uaccess.c \
    $(KERNEL_DIR)/drivers/uart/16550.c \
    $(KERNEL_DIR)/drivers/timer/pic_pit.c \
    $(ARCH_DIR)/platform/platform.c \
    $(ARCH_DIR)/hal.c \
    $(ARCH_DIR)/virtio.c
else
BOOT_SOURCES = \
    $(BOOT_DIR)/header.S \
    $(BOOT_DIR)/stage1.S \
    $(BOOT_DIR)/stage2.S

KERN_ASM_SOURCES = \
    $(ARCH_DIR)/boot/start.S \
    $(ARCH_DIR)/cpu/exception.S

KERN_C_SOURCES = \
    $(ARCH_DIR)/cpu/cpu.c \
    $(ARCH_DIR)/cpu/syscall.c \
    $(ARCH_DIR)/mm/mmu.c \
    $(ARCH_DIR)/platform.c \
    $(ARCH_DIR)/hal.c \
    $(ARCH_DIR)/virtio.c \
    $(KERNEL_DIR)/drivers/uart/pl011.c \
    $(KERNEL_DIR)/drivers/gic/gic.c \
    $(KERNEL_DIR)/drivers/timer/timer.c
endif

KERN_C_SOURCES += \
    $(KERNEL_DIR)/core/hal_bus.c \
    $(KERNEL_DIR)/core/syscall_dispatch.c \
    $(KERNEL_DIR)/core/object.c \
    $(KERNEL_DIR)/core/fault.c \
    $(KERNEL_DIR)/core/timer.c \
    $(KERNEL_DIR)/drivers/console.c \
    $(KERNEL_DIR)/drivers/irq_ctrl.c \
    $(KERNEL_DIR)/drivers/sys_timer.c \
    $(KERNEL_DIR)/drivers/block/block.c \
    $(KERNEL_DIR)/drivers/block/ramdisk.c \
    $(KERNEL_DIR)/drivers/virtio/virtio_blk.c \
    $(KERNEL_DIR)/drivers/virtio/virtio_input.c \
    $(KERNEL_DIR)/drivers/gpu/virtio_gpu.c \
    $(KERNEL_DIR)/drivers/gpu/gpu_core.c \
    $(KERNEL_DIR)/drivers/pci/pci.c \
    $(KERNEL_DIR)/drivers/usb/usb_core.c \
    $(KERNEL_DIR)/drivers/usb/usb_hid.c \
    $(KERNEL_DIR)/drivers/usb/xhci.c \
    $(KERNEL_DIR)/drivers/usb/ehci.c \
    $(KERNEL_DIR)/drivers/usb/uhci.c \
    $(KERNEL_DIR)/drivers/keyboard/keyboard.c \
    $(KERNEL_DIR)/fs/gpt.c \
    $(KERNEL_DIR)/fs/ext4.c \
    $(KERNEL_DIR)/fs/vfs.c \
    $(KERNEL_DIR)/mm/pmm.c \
    $(KERNEL_DIR)/mm/vmm.c \
    $(KERNEL_DIR)/mm/buffer.c \
    $(KERNEL_DIR)/lib/string.c \
    $(KERNEL_DIR)/lib/crc32.c \
    $(KERNEL_DIR)/lib/vsnprintf.c \
    $(KERNEL_DIR)/lib/printk.c \
    $(KERNEL_DIR)/lib/fault_print.c \
    $(KERNEL_DIR)/lib/backtrace.c \
    $(KERNEL_DIR)/lib/stack_protector.c \
    $(KERNEL_DIR)/lib/entropy.c \
    $(KERNEL_DIR)/lib/math.c \
    $(KERNEL_DIR)/lib/kmalloc.c \
    $(KERNEL_DIR)/lib/registry.c \
    $(KERNEL_DIR)/lib/ktest.c \
    $(KERNEL_DIR)/lib/ktest_samples.c \
    $(KERNEL_DIR)/lib/utf8.c \
    $(KERNEL_DIR)/cpu.c \
    $(KERNEL_DIR)/sched/process.c \
    $(KERNEL_DIR)/sched/elf.c \
    $(KERNEL_DIR)/graphics/graphics.c \
    $(KERNEL_DIR)/graphics/region.c \
    $(KERNEL_DIR)/graphics/gl.c \
    $(KERNEL_DIR)/graphics/font.c \
    $(KERNEL_DIR)/graphics/term.c \
    $(KERNEL_DIR)/graphics/compositor_style.c \
    $(KERNEL_DIR)/graphics/compositor.c \
    $(KERNEL_DIR)/irq/irq.c \
    $(KERNEL_DIR)/lib/fdt.c \
    $(KERNEL_DIR)/main.c \
    $(KERNEL_DIR)/drivers/ps2/ps2.c

KERN_CPP_SOURCES = \
    $(KERNEL_DIR)/drivers/cpp_test.cpp

# Object files
BOOT_OBJECTS = $(patsubst %.S,$(BUILD_DIR)/%.o,$(BOOT_SOURCES))
KERN_ASM_OBJECTS = $(KERN_ASM_SOURCES:%.S=$(BUILD_DIR)/%.o)
KERN_C_OBJECTS = $(KERN_C_SOURCES:%.c=$(BUILD_DIR)/%.o)
KERN_OBJECTS = $(KERN_ASM_OBJECTS) $(KERN_C_OBJECTS)

# Dependency files
DEPS = $(KERN_C_OBJECTS:.o=.d)

# ==============================================================================
# Build Rules
# ==============================================================================

.PHONY: all clean run run-direct debug disasm check dirs bootloader kernel disk rootfs release test-release help

# Default target
all: dirs bootloader kernel user $(MKDISK) disk
	@echo ""
	@echo "Build complete for $(ARCH)!"
	@echo "  Disk Image: $(DISK_IMG)"
ifeq ($(ARCH), amd64)
	@echo "  Kernel:     $(KERNEL_ELF)"
else
	@echo "  Kernel:     $(KERNEL_BIN) (AArch64 Raw Binary)"
endif

# Create build directories
dirs:
	@mkdir -p $(BUILD_DIR)/$(BOOT_DIR)
	@mkdir -p $(BUILD_DIR)/$(ARCH_DIR)/boot
	@mkdir -p $(BUILD_DIR)/$(ARCH_DIR)/cpu
	@mkdir -p $(BUILD_DIR)/$(ARCH_DIR)/mm
	@mkdir -p $(BUILD_DIR)/$(ARCH_DIR)/platform
	@mkdir -p $(BUILD_DIR)/$(ARCH_DIR)/drivers
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/uart
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/gic
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/timer
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/virtio
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/gpu
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/keyboard
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/fs
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/mm
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/lib
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/sched
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/graphics
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/irq
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/pci
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/core
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/lib
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/sys/lib
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/sys/bin
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/bin
	@mkdir -p $(BUILD_DIR)/$(USER_ARCH_DIR)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/block
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/ps2
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/drivers/usb
# Bootloader
bootloader: $(BOOTLOADER_BIN)

$(BOOTLOADER_ELF): $(BOOT_OBJECTS)
	@$(LD) $(LDFLAGS_BOOT) -o $@ $^

$(BOOTLOADER_BIN): $(BOOTLOADER_ELF)
	@$(OBJCOPY) -O binary $< $@

# Kernel
kernel: $(KERNEL_BIN)

# Two-pass link for the in-kernel symbol table (Phase A, kernel/lib/backtrace.c):
# pass 1 links without .ksyms, gen_ksyms.sh derives the table from `nm -n` of
# pass 1, pass 2 relinks with it.  .ksyms sits after .text in both kernel.ld
# scripts, so text addresses are identical across the passes (the table stays
# valid).  If generation ever fails the empty-table fallback in backtrace.c
# keeps the kernel functional (raw addresses).
NM = $(CROSS_COMPILE)nm
KERNEL_PRE = $(BUILD_DIR)/kernel.pre.elf
KSYMS_S    = $(BUILD_DIR)/ksyms.S
KSYMS_O    = $(BUILD_DIR)/ksyms.o

$(KERNEL_PRE): $(KERN_OBJECTS)
	@$(LD) $(LDFLAGS_KERN) -o $@ $^

$(KSYMS_O): $(KERNEL_PRE) tools/gen_ksyms.sh
	@tools/gen_ksyms.sh "$(NM)" $(KERNEL_PRE) $(KSYMS_S)
	@$(CC) $(CFLAGS) -c -o $@ $(KSYMS_S)

$(KERNEL_ELF): $(KERN_OBJECTS) $(KSYMS_O)
	@$(LD) $(LDFLAGS_KERN) -o $@ $^

$(KERNEL_BIN): $(KERNEL_ELF)
	@$(OBJCOPY) -O binary $< $@

# Userland
USER_SYSCALL_O = $(BUILD_DIR)/$(USER_ARCH_DIR)/syscall.o
USER_LIB_O     = $(BUILD_DIR)/$(USER_SYS_DIR)/lib/lib.o
USER_MALLOC_O  = $(BUILD_DIR)/$(USER_SYS_DIR)/lib/malloc.o

# System ELFs (placed in /sys/bin)
SYS_ELFS = $(BUILD_DIR)/init.elf $(BUILD_DIR)/shell.elf $(BUILD_DIR)/notify_srv.elf $(BUILD_DIR)/nxres.elf \
           $(BUILD_DIR)/regedit.elf $(BUILD_DIR)/fontman.elf $(BUILD_DIR)/top.elf $(BUILD_DIR)/nexs-fm.elf \
           $(BUILD_DIR)/nxui.elf $(BUILD_DIR)/nxproc.elf $(BUILD_DIR)/nxinfo.elf \
           $(BUILD_DIR)/nxperm.elf $(BUILD_DIR)/nxmemstat.elf

# User ELFs (placed in /bin)
BIN_ELFS = $(BUILD_DIR)/counter.elf $(BUILD_DIR)/demo3d.elf $(BUILD_DIR)/ipc_send.elf \
           $(BUILD_DIR)/ipc_recv.elf $(BUILD_DIR)/crash.elf $(BUILD_DIR)/writetest.elf \
           $(BUILD_DIR)/doom.elf $(BUILD_DIR)/input_test.elf $(BUILD_DIR)/nxtest.elf \
           $(BUILD_DIR)/fdtest.elf $(BUILD_DIR)/forkbomb.elf \
           $(BUILD_DIR)/captest.elf \
           $(BUILD_DIR)/capipc.elf $(BUILD_DIR)/capipc_child.elf \
           $(BUILD_DIR)/capreg.elf \
           $(BUILD_DIR)/capkill.elf $(BUILD_DIR)/capkill_child.elf \
           $(BUILD_DIR)/sandboxtest.elf $(BUILD_DIR)/sandboxchild.elf \
           $(BUILD_DIR)/hello.elf \
           $(BUILD_DIR)/stress.elf \
		   $(BUILD_DIR)/kilo.elf

USER_ELFS = $(SYS_ELFS) $(BIN_ELFS)

user: $(USER_ELFS)

# User Compilations
$(BUILD_DIR)/$(USER_DIR)/lib/%.o: $(USER_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/sys/lib/%.o: $(USER_DIR)/sys/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/bin/%.o: $(USER_DIR)/bin/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/sys/bin/%.o: $(USER_DIR)/sys/bin/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Explicit dependencies for each user ELF
$(BUILD_DIR)/init.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/init.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/counter.elf: $(BUILD_DIR)/$(USER_DIR)/bin/counter.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/shell.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/shell.o $(BUILD_DIR)/$(USER_DIR)/sys/bin/proce.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/demo3d.elf: $(BUILD_DIR)/$(USER_DIR)/bin/demo3d.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/kilo.elf: $(BUILD_DIR)/$(USER_DIR)/bin/kilo/kilo.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/ipc_send.elf: $(BUILD_DIR)/$(USER_DIR)/bin/ipc_send.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/ipc_recv.elf: $(BUILD_DIR)/$(USER_DIR)/bin/ipc_recv.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/notify_srv.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/notification_server.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/crash.elf: $(BUILD_DIR)/$(USER_DIR)/bin/crash.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/regedit.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/regedit.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/top.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/top.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxproc.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nxproc.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxinfo.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nxinfo.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxperm.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nxperm.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxui.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nxui.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/writetest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/writetest.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/fdtest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/fdtest.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/captest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/captest.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/capipc.elf: $(BUILD_DIR)/$(USER_DIR)/bin/capipc.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/capipc_child.elf: $(BUILD_DIR)/$(USER_DIR)/bin/capipc_child.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/capreg.elf: $(BUILD_DIR)/$(USER_DIR)/bin/capreg.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/capkill.elf: $(BUILD_DIR)/$(USER_DIR)/bin/capkill.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/capkill_child.elf: $(BUILD_DIR)/$(USER_DIR)/bin/capkill_child.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/forkbomb.elf: $(BUILD_DIR)/$(USER_DIR)/bin/forkbomb.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/sandboxtest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/sandboxtest.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/sandboxchild.elf: $(BUILD_DIR)/$(USER_DIR)/bin/sandboxchild.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/hello.elf: $(BUILD_DIR)/$(USER_DIR)/bin/hello.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxmemstat.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nxmemstat.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/stress.elf: $(BUILD_DIR)/$(USER_DIR)/bin/stress.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxres.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nxres.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/nxtest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/nxtest.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/input_test.elf: $(BUILD_DIR)/$(USER_DIR)/bin/input_test.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/fontman.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/fontman/fontman.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/nexs-fm.elf: $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/main.o \
                          $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/state.o \
                          $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/sort.o \
                          $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/ui.o \
                          $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/draw.o \
                          $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/events.o \
                          $(BUILD_DIR)/$(USER_DIR)/sys/bin/nexs-fm/fileops.o \
                          $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/$(USER_DIR)/sys/bin/fontman/%.o: $(USER_DIR)/sys/bin/fontman/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# kilo is linked like every other user ELF (prereqs declared above, generic
# linking rule below). Its object is built via the $(USER_DIR)/bin/%.o pattern.

# Linking rule for user ELFs
$(BUILD_DIR)/%.elf:
	@$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^



# Common compilation rules
$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -DKERNEL -MMD -MP -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

# Disk Generation
rootfs: user
	@rm -rf $(BUILD_DIR)/rootfs
	@mkdir -p $(BUILD_DIR)/rootfs/sys/bin
	@mkdir -p $(BUILD_DIR)/rootfs/bin
	@mkdir -p $(BUILD_DIR)/rootfs/etc
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib
	@mkdir -p $(BUILD_DIR)/rootfs/lib
	@cp $(SYS_ELFS) $(BUILD_DIR)/rootfs/sys/bin/
	@cp $(BIN_ELFS) $(BUILD_DIR)/rootfs/bin/
	@cp user/sys/bin/init.cfg $(BUILD_DIR)/rootfs/etc/
	@# Pre-create a 16KB scratch file for the /bin/stress file lane: the FS has no
	@# runtime file creation (O_CREAT -> -EINVAL), and file_write only extends an
	@# EXISTING file, so ship one (perf §6 stress harness).
	@dd if=/dev/zero of=$(BUILD_DIR)/rootfs/etc/stress.tmp bs=1024 count=16 2>/dev/null
	@# Copy essential WAD files to the root and /bin for engine detection
	@-cp user/bin/doom/freedoom1.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/doom/freedoom2.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/globe.png $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/doom/doomsav0.dsg $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/doom/doomsav1.dsg $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/doom/doomsav2.dsg $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/rootfs/fonts
	@-cp user/sys/bin/fontman/fonts/*.ttf $(BUILD_DIR)/rootfs/fonts/ 2>/dev/null || true
	@-cp user/sys/bin/fontman/fonts/*.off $(BUILD_DIR)/rootfs/fonts/ 2>/dev/null || true
	@# Remove .elf extensions in rootfs
	@for f in $(BUILD_DIR)/rootfs/sys/bin/*.elf; do mv "$$f" "$${f%.elf}"; done
	@for f in $(BUILD_DIR)/rootfs/bin/*.elf; do mv "$$f" "$${f%.elf}"; done

disk: $(MKDISK) kernel rootfs bootloader
	@mkdir -p $(BUILD_DIR)
	@./$(MKDISK) $(DISK_IMG) $(BOOTLOADER_BIN) $(KERNEL_BIN) $(BUILD_DIR)/rootfs $(MKDISK_LAYOUT)

$(MKDISK): tools/mkdisk.c
	@mkdir -p tools
	@gcc -o $(MKDISK) tools/mkdisk.c

# ==============================================================================
# QEMU Flags
# ==============================================================================

ifeq ($(ARCH), amd64)
QEMU_FLAGS = -m 5G -smp 4 -serial mon:stdio \
             -display default,show-cursor=on \
             -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
             -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
             -device virtio-mouse-pci,disable-legacy=on,disable-modern=off \
             -drive if=none,file=$(DISK_IMG),id=hd0,format=raw \
             -device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off

QEMU_RELEASE_FLAGS = -m 5G -smp 4 -serial mon:stdio \
                     -display default,show-cursor=on \
                     -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
                     -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
                     -device virtio-mouse-pci,disable-legacy=on,disable-modern=off
else
# AArch64: pass -dtb $(VIRT_DTB) so QEMU loads the pre-generated device tree into RAM
# and sets x0 = dtb_load_address in its aarch64 boot stub.  Without -dtb, QEMU does not
# generate/load a DTB for bare-metal ELF kernels and x0 arrives as 0 at _start.
QEMU_FLAGS = -M virt -cpu cortex-a57 -m 5G -smp 4 -serial mon:stdio \
             -display default,show-cursor=on \
             -device virtio-gpu-device \
             -device virtio-keyboard-device -device virtio-mouse-device \
             -drive if=none,file=$(DISK_IMG),id=hd0,format=raw -device virtio-blk-device,drive=hd0 \
             -dtb $(VIRT_DTB)

QEMU_RELEASE_FLAGS = -M virt -cpu cortex-a57 -m 5G -smp 4 -serial mon:stdio \
                     -display default,show-cursor=on \
                     -device virtio-gpu-device \
                     -device virtio-keyboard-device -device virtio-mouse-device \
                     -drive if=none,file=$(RELEASE_DIR)/disk.img,id=hd0,format=raw -device virtio-blk-device,drive=hd0
endif

# ==============================================================================
# Release Generation
# ==============================================================================

release:
	@echo "========================================"
	@echo "  Creating Full Release $(VERSION)"
	@echo "========================================"
	@$(MAKE) ARCH=amd64 release-arch VERSION=$(VERSION)
	@$(MAKE) ARCH=aarch64 release-arch VERSION=$(VERSION)
	@echo "========================================"
	@echo "✓ Full release $(VERSION) complete!"
	@echo "  Location: $(RELEASE_BASE)"
	@echo "========================================"

release-arch: all
	@echo "--> Building $(ARCH) release..."
	@rm -rf $(RELEASE_DIR)
	@mkdir -p $(RELEASE_DIR)
	@if [ "$(ARCH)" = "amd64" ]; then \
		set -e; \
		mkdir -p $(RELEASE_DIR)/boot/grub; \
		cp $(KERNEL_ELF) $(RELEASE_DIR)/boot/kernel.elf; \
		cp $(DISK_IMG) $(RELEASE_DIR)/boot/disk.img; \
		echo 'set timeout=0' > $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo 'set default=0' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo 'menuentry "NexsOS1" {' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    insmod part_gpt' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    insmod ext2' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    insmod loopback' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    multiboot2 /boot/kernel.elf' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    loopback loop /boot/disk.img' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    ls (loop,gpt3)/' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    module2 /boot/disk.img diskimg' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '    boot' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo '}' >> $(RELEASE_DIR)/boot/grub/grub.cfg; \
		echo "AMD64 files ready: boot/kernel.elf, boot/disk.img"; \
		mkdir -p $(RELEASE_DIR)/iso/boot/grub; \
		cp -r $(RELEASE_DIR)/boot/* $(RELEASE_DIR)/iso/boot/; \
		GRUB_MKRESCUE=""; \
		for c in i686-elf-grub-mkrescue grub-mkrescue; do \
			if command -v $$c >/dev/null 2>&1; then \
				GRUB_MKRESCUE=$$c; break; \
			fi; \
		done; \
		if [ -z "$$GRUB_MKRESCUE" ]; then \
			echo "Errore: grub-mkrescue non trovato"; \
			exit 1; \
		fi; \
		echo "-> Creo ISO con $$GRUB_MKRESCUE..."; \
		$$GRUB_MKRESCUE -o $(RELEASE_DIR)/NexsOS1-amd64-$(VERSION).iso $(RELEASE_DIR)/iso; \
		echo "-> Rimuovo MBR hybrid per renderla leggibile da macOS..."; \
		dd if=/dev/zero of=$(RELEASE_DIR)/NexsOS1-amd64-$(VERSION).iso bs=512 count=1 conv=notrunc 2>/dev/null; \
		echo "ISO creata: $(RELEASE_DIR)/NexsOS1-amd64-$(VERSION).iso"; \
	else \
		cp $(KERNEL_BIN) $(RELEASE_DIR)/kernel.img; \
		cp $(DISK_IMG) $(RELEASE_DIR)/disk.img; \
		echo "AArch64 release: kernel.img + disk.img"; \
	fi

test-release: release-arch
	@echo "Starting QEMU Release Test for $(ARCH) (Version: $(VERSION))..."
ifeq ($(ARCH), amd64)
	$(QEMU) $(QEMU_RELEASE_FLAGS) \
		-drive if=none,file=$(RELEASE_DIR)/NexsOS1-amd64-$(VERSION).iso,id=hd0,format=raw \
		-device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off
else
	$(QEMU) $(QEMU_RELEASE_FLAGS) -kernel $(RELEASE_DIR)/kernel.img
endif

# ==============================================================================
# Development Execution
# ==============================================================================

# AArch64: generate the QEMU virt device tree for the exact machine configuration
# used in the run target.  The DTB must match the devices, memory size, and SMP
# count so that fdt_get_mem_regions() and fdt_count_cpus() see accurate data.
# dumpdtb exits immediately after writing the DTB; no kernel is required.
# amd64 does not use a DTB (discovery via multiboot2), so this target is aarch64-only.
ifeq ($(ARCH), aarch64)
$(VIRT_DTB): | $(BUILD_DIR)
	@echo "  [DTB]    Generating QEMU virt device tree -> $@"
	@$(QEMU) -M virt -cpu cortex-a57 -m 5G -smp 4 \
	         -device virtio-gpu-device \
	         -device virtio-keyboard-device -device virtio-mouse-device \
	         -drive if=none,file=$(DISK_IMG),id=hd0,format=raw \
	         -device virtio-blk-device,drive=hd0 \
	         -machine dumpdtb=$@ 2>/dev/null
	@echo "  [DTB]    Done ($$(stat -f%z $@ 2>/dev/null || stat -c%s $@) bytes)"

# AArch64 run: use kernel.bin (raw binary) so QEMU's aarch64 boot stub sets x0 = dtb_addr.
# A raw binary triggers QEMU's arm_load_kernel image-loading path; combined with -dtb,
# QEMU loads the DTB into RAM and passes its address in x0 before jumping to the entry.
# Using kernel.elf (ELF) without a Linux image header leaves x0=0 (no DTB handover).
run: all $(VIRT_DTB)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_BIN)

run-direct: run

debug: all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_BIN) -s -S
else
run: all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

run-direct: run

debug: all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -s -S
endif

disasm: $(KERNEL_ELF) $(BOOTLOADER_ELF)
	$(OBJDUMP) -d $(KERNEL_ELF) > $(BUILD_DIR)/kernel.disasm
	$(OBJDUMP) -d $(BOOTLOADER_ELF) > $(BUILD_DIR)/bootloader.disasm
	$(OBJDUMP) -d $(USER_ELF) > $(BUILD_DIR)/init.disasm

check:
	@which $(CC) > /dev/null && echo "✓ CC: $(CC)" || echo "✗ CC not found"
	@which $(AS) > /dev/null && echo "✓ AS: $(AS)" || echo "✗ AS not found"
	@which $(LD) > /dev/null && echo "✓ LD: $(LD)" || echo "✗ LD not found"
	@which $(QEMU) > /dev/null && echo "✓ QEMU: $(QEMU)" || echo "✗ QEMU not found"
	@if [ -n "$(GRUB_MKRESCUE)" ]; then echo "✓ GRUB: $(GRUB_MKRESCUE)"; else echo "✗ GRUB mancante"; fi

clean:
	-@rm -rf build/aarch64 2>/dev/null || true
	-@rm -rf build/amd64 2>/dev/null || true
	-@rm -rf build/*.img build/*.iso 2>/dev/null || true
	-@rm -rf $(BUILD_ROOT) 2>/dev/null || true
	-@rm -rf release 2>/dev/null || true
	-@rm $(MKDISK) 2>/dev/null || true

help:
	@echo "$(ARCH) Kernel Build System"
	@echo "Targets:"
	@echo "  all          - Build bootloader, kernel, and disk image"
	@echo "  release      - Build production files (Use: make release VERSION=0.4.1)"
	@echo "  test-release - Build release and test it in QEMU (Use: make test-release VERSION=0.4.1)"
	@echo "  run          - Build and run kernel directly"
	@echo "  clean        - Remove build artifacts"

# Include architecture-specific Doom makefiles
ifeq ($(ARCH), aarch64)
    include user/bin/doom/doom-nexs-aarch64.make
else ifeq ($(ARCH), amd64)
    include user/bin/doom/doom-nexs-amd64.make
endif
endif
