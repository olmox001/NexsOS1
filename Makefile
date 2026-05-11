# Makefile for AArch64 Production Kernel
# Cross-compilation for ARM 64-bit bare-metal

# ==============================================================================
# Configuration
# ==============================================================================

# Target Architecture (default to aarch64)
ARCH ?= aarch64

# Normalize ARCH typos (e.g. aaarch64 -> aarch64)
ifeq ($(findstring aarch64,$(ARCH)),aarch64)
    override ARCH := aarch64
else ifeq ($(findstring amd64,$(ARCH)),amd64)
    override ARCH := amd64
endif

# ==============================================================================
# Configuration
# ==============================================================================

ifeq ($(ARCH), amd64)
# Cross-compiler prefix for AMD64
CROSS_COMPILE ?= x86_64-elf-
KERNEL_DIR = kernel
BOOT_DIR   = boot/amd64
ARCH_DIR   = $(KERNEL_DIR)/arch/amd64
CFLAGS += -DARCH_AMD64 -mno-red-zone -mcmodel=large
# Assembler flags
ASFLAGS = -g --fatal-warnings
# Linker flags
LDFLAGS_BOOT = -nostdlib -static
LDFLAGS_KERN = -nostdlib -static -T $(ARCH_DIR)/kernel.ld -Map build/kernel.map

else
# Cross-compiler prefix for AArch64
CROSS_COMPILE ?= aarch64-none-elf-
KERNEL_DIR = kernel
BOOT_DIR   = boot/aarch64
ARCH_DIR   = $(KERNEL_DIR)/arch/aarch64
CFLAGS += -DARCH_AARCH64 -mcpu=cortex-a57
# Assembler flags
ASFLAGS = -mcpu=cortex-a57 -g --fatal-warnings
# Linker flags
LDFLAGS_BOOT = -nostdlib -static -T $(BOOT_DIR)/linker.ld
LDFLAGS_KERN = -nostdlib -static -T $(ARCH_DIR)/kernel.ld -Map build/kernel.map
endif

# Tools
CC      = $(CROSS_COMPILE)gcc
CXX     = $(CROSS_COMPILE)g++
AS      = $(CROSS_COMPILE)as
LD      = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump

# Directories
BUILD_ROOT = build
BUILD_DIR  = $(BUILD_ROOT)/$(ARCH)
USER_DIR   = user
USER_LIB_DIR = $(USER_DIR)/lib
USER_BIN_DIR = $(USER_DIR)/bin
USER_ARCH_DIR = $(USER_DIR)/arch/$(ARCH)
INCLUDE    = -I$(KERNEL_DIR)/include -I$(ARCH_DIR)/include -Iinclude/api

# Output files
BOOTLOADER_ELF = $(BUILD_DIR)/bootloader.elf
BOOTLOADER_BIN = $(BUILD_DIR)/bootloader.bin
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
USER_ELF   = $(BUILD_DIR)/init.elf
DISK_IMG   = $(BUILD_ROOT)/disk.img


# Compiler flags (common)
CFLAGS += -Wall -Wextra -Werror -Wpedantic -Wshadow -Wwrite-strings \
         -Wmissing-prototypes -Wstrict-prototypes \
         -ffreestanding -fno-builtin -nostdlib -nostartfiles \
         -fno-common -fstack-protector-strong \
         -fno-pic -fno-pie \
         -fno-omit-frame-pointer \
         -O2 -g \
         $(INCLUDE)

CXXFLAGS = -Wall -Wextra -Werror -Wpedantic -Wshadow \
           -ffreestanding -fno-builtin -nostdlib -nostartfiles \
           -fno-common -fstack-protector-strong \
           -fno-pic -fno-pie \
           -fno-omit-frame-pointer \
           -fno-exceptions -fno-rtti \
           -O2 -g \
           $(INCLUDE)


# ==============================================================================
# Source Files
# ==============================================================================

# Kernel assembly sources
ifeq ($(ARCH), amd64)
# No separate bootloader for AMD64, GRUB does it.
BOOT_SOURCES = 

KERN_ASM_SOURCES = \
    $(BOOT_DIR)/boot.S \
    $(ARCH_DIR)/cpu/isr_stubs.S \
    $(ARCH_DIR)/cpu/syscall.S \
    $(ARCH_DIR)/cpu/context.S

KERN_C_SOURCES = \
    $(ARCH_DIR)/cpu/cpu.c \
    $(ARCH_DIR)/cpu/idt.c \
    $(ARCH_DIR)/cpu/gdt.c \
    $(ARCH_DIR)/cpu/msr.c \
    $(ARCH_DIR)/cpu/syscall.c \
    $(ARCH_DIR)/mm/mmu.c \
    $(ARCH_DIR)/mm/uaccess.c \
    $(ARCH_DIR)/drivers/uart_16550.c \
    $(ARCH_DIR)/drivers/pic_pit.c \
    $(ARCH_DIR)/platform/platform.c \
    $(KERNEL_DIR)/drivers/pci/pci.c
else
# Bootloader sources
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
    $(ARCH_DIR)/platform.c \
    $(KERNEL_DIR)/drivers/uart/pl011.c \
    $(KERNEL_DIR)/drivers/gic/gic.c \
    $(KERNEL_DIR)/drivers/timer/timer.c
endif

KERN_C_SOURCES += \
    $(KERNEL_DIR)/core/syscall_dispatch.c \
    $(KERNEL_DIR)/core/timer.c \
    $(KERNEL_DIR)/drivers/console.c \
    $(KERNEL_DIR)/drivers/irq_ctrl.c \
    $(KERNEL_DIR)/drivers/sys_timer.c \
    $(KERNEL_DIR)/drivers/virtio/virtio_blk.c \
    $(KERNEL_DIR)/drivers/virtio/virtio_input.c \
    $(KERNEL_DIR)/drivers/gpu/virtio_gpu.c \
    $(KERNEL_DIR)/drivers/gpu/gpu_core.c \
    $(KERNEL_DIR)/drivers/keyboard/keyboard.c \
    $(KERNEL_DIR)/fs/gpt.c \
    $(KERNEL_DIR)/fs/ext4.c \
    $(KERNEL_DIR)/mm/pmm.c \
    $(KERNEL_DIR)/mm/vmm.c \
    $(KERNEL_DIR)/mm/buffer.c \
    $(KERNEL_DIR)/lib/string.c \
    $(KERNEL_DIR)/lib/printk.c \
    $(KERNEL_DIR)/lib/stack_protector.c \
    $(KERNEL_DIR)/lib/math.c \
    $(KERNEL_DIR)/lib/kmalloc.c \
    $(KERNEL_DIR)/lib/registry.c \
    $(KERNEL_DIR)/lib/ktest.c \
    $(KERNEL_DIR)/lib/ktest_samples.c \
    $(KERNEL_DIR)/cpu.c \
    $(KERNEL_DIR)/sched/process.c \
    $(KERNEL_DIR)/sched/elf.c \
    $(KERNEL_DIR)/graphics/graphics.c \
    $(KERNEL_DIR)/graphics/region.c \
    $(KERNEL_DIR)/graphics/gl.c \
    $(KERNEL_DIR)/graphics/font.c \
    $(KERNEL_DIR)/graphics/compositor.c \
    $(KERNEL_DIR)/irq/irq.c \
    $(KERNEL_DIR)/main.c

KERN_CPP_SOURCES = \
    $(KERNEL_DIR)/drivers/cpp_test.cpp


# Object files
BOOT_OBJECTS = $(BOOT_SOURCES:%.S=$(BUILD_DIR)/%.o)
KERN_ASM_OBJECTS = $(KERN_ASM_SOURCES:%.S=$(BUILD_DIR)/%.o)
KERN_C_OBJECTS = $(KERN_C_SOURCES:%.c=$(BUILD_DIR)/%.o)
# KERN_CPP_OBJECTS = $(KERN_CPP_SOURCES:%.cpp=$(BUILD_DIR)/%.o)

KERN_OBJECTS = $(KERN_ASM_OBJECTS) $(KERN_C_OBJECTS)

# Tools
MKDISK = tools/mkdisk

# Dependency files
DEPS = $(KERN_C_OBJECTS:.o=.d)

# ==============================================================================
# Build Rules
# ==============================================================================

.PHONY: all clean run run-direct debug disasm check dirs bootloader kernel iso

ifeq ($(ARCH), amd64)
# Default target - builds everything including ISO
all: dirs kernel user $(MKDISK) disk
	@echo ""
	@echo "Build complete!"
	@echo "  Disk Image: build/disk.img"
	@echo "  Kernel:     $(KERNEL_ELF)"
	@echo "  ISO Image:  build/os1test.iso"
	@echo ""
	@echo "Run with: make run"
	@echo ""
else
# Default target - builds everything including ISO
all: dirs bootloader kernel user $(MKDISK) disk
	@echo ""
	@echo "Build complete!"
	@echo "  Disk Image: $(BUILD_DIR)/disk.img"
	@echo "  Kernel:     $(KERNEL_ELF)"
	@echo ""
	@echo "Run with: make run ARCH=$(ARCH)"
	@echo ""
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
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/bin
	@mkdir -p $(BUILD_DIR)/$(USER_ARCH_DIR)


# ==============================================================================
# Bootloader
# ==============================================================================

ifeq ($(ARCH), amd64)
bootloader:
	@echo "Skipping bootloader for AMD64 (using GRUB)"
else
bootloader: $(BOOTLOADER_BIN)

$(BOOTLOADER_ELF): $(BOOT_OBJECTS)
	@echo "Linking bootloader..."
	$(LD) $(LDFLAGS_BOOT) -o $@ $^
	@echo "Bootloader size: $$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes"

$(BOOTLOADER_BIN): $(BOOTLOADER_ELF)
	@echo "Creating bootloader binary..."
	$(OBJCOPY) -O binary $< $@
endif

# ==============================================================================
# Kernel
# ==============================================================================

kernel: $(KERNEL_BIN)

$(KERNEL_ELF): $(KERN_OBJECTS)
	@echo "Linking kernel..."
	$(LD) $(LDFLAGS_KERN) -o $@ $^
	@echo "Kernel size: $$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes"

$(KERNEL_BIN): $(KERNEL_ELF)
	@echo "Creating kernel binary..."
	$(OBJCOPY) -O binary $< $@

# ==============================================================================
# Userland
# ==============================================================================

USER_SYSCALL_O = $(BUILD_DIR)/$(USER_ARCH_DIR)/syscall.o

USER_ELFS = $(BUILD_DIR)/init.elf $(BUILD_DIR)/counter.elf $(BUILD_DIR)/shell.elf \
            $(BUILD_DIR)/demo3d.elf $(BUILD_DIR)/ipc_send.elf $(BUILD_DIR)/ipc_recv.elf \
            $(BUILD_DIR)/notify_srv.elf $(BUILD_DIR)/crash.elf $(BUILD_DIR)/regedit.elf \
            $(BUILD_DIR)/writetest.elf

user: $(USER_ELFS)

# User Init
USER_SRC = $(USER_BIN_DIR)/init.c $(USER_LIB_DIR)/lib.c
USER_OBJ = $(USER_SRC:%.c=$(BUILD_DIR)/%.o)

# User Counter
COUNTER_SRC = $(USER_BIN_DIR)/counter.c $(USER_LIB_DIR)/lib.c
COUNTER_OBJ = $(COUNTER_SRC:%.c=$(BUILD_DIR)/%.o)

# User Shell
SHELL_SRC = $(USER_BIN_DIR)/shell.c $(USER_LIB_DIR)/lib.c $(USER_BIN_DIR)/proce.c
SHELL_OBJ = $(SHELL_SRC:%.c=$(BUILD_DIR)/%.o)

# User Demo3D
DEMO3D_SRC = $(USER_BIN_DIR)/demo3d.c $(USER_LIB_DIR)/lib.c
DEMO3D_OBJ = $(DEMO3D_SRC:%.c=$(BUILD_DIR)/%.o)

$(BUILD_DIR)/$(USER_DIR)/lib/%.o: $(USER_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/bin/%.o: $(USER_DIR)/bin/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(USER_ELF): $(USER_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^
	@echo "Userland init size: $$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes"

$(BUILD_DIR)/counter.elf: $(COUNTER_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^
	@echo "Userland counter size: $$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes"

$(BUILD_DIR)/shell.elf: $(SHELL_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^
	@echo "Userland shell size: $$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes"

$(BUILD_DIR)/demo3d.elf: $(DEMO3D_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^
	@echo "Userland demo3d size: $$(stat -f%z $@ 2>/dev/null || stat -c%s $@ 2>/dev/null) bytes"

# IPC Send
IPC_SEND_SRC = $(USER_BIN_DIR)/ipc_send.c $(USER_LIB_DIR)/lib.c
IPC_SEND_OBJ = $(IPC_SEND_SRC:%.c=$(BUILD_DIR)/%.o)
$(BUILD_DIR)/ipc_send.elf: $(IPC_SEND_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# IPC Recv
IPC_RECV_SRC = $(USER_BIN_DIR)/ipc_recv.c $(USER_LIB_DIR)/lib.c
IPC_RECV_OBJ = $(IPC_RECV_SRC:%.c=$(BUILD_DIR)/%.o)
$(BUILD_DIR)/ipc_recv.elf: $(IPC_RECV_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# Notification Server
NOTIFY_SRC = $(USER_BIN_DIR)/notification_server.c $(USER_LIB_DIR)/lib.c
NOTIFY_OBJ = $(NOTIFY_SRC:%.c=$(BUILD_DIR)/%.o)
$(BUILD_DIR)/notify_srv.elf: $(NOTIFY_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^
# Crash Test
CRASH_SRC = $(USER_BIN_DIR)/crash.c $(USER_LIB_DIR)/lib.c
CRASH_OBJ = $(CRASH_SRC:%.c=$(BUILD_DIR)/%.o)
$(BUILD_DIR)/crash.elf: $(CRASH_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# Registry Editor
REGEDIT_SRC = $(USER_BIN_DIR)/regedit.c $(USER_LIB_DIR)/lib.c
REGEDIT_OBJ = $(REGEDIT_SRC:%.c=$(BUILD_DIR)/%.o)
$(BUILD_DIR)/regedit.elf: $(REGEDIT_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# Write Test
WRITETEST_SRC = $(USER_BIN_DIR)/writetest.c $(USER_LIB_DIR)/lib.c
WRITETEST_OBJ = $(WRITETEST_SRC:%.c=$(BUILD_DIR)/%.o)
$(BUILD_DIR)/writetest.elf: $(WRITETEST_OBJ) $(USER_SYSCALL_O)
	$(CC) $(CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# ==============================================================================
# Common compilation rules

# ==============================================================================

# Compile assembly files
$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "AS  $<"
	@$(AS) $(ASFLAGS) -o $@ $<

# Compile C files
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Compile C++ files
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "CXX $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# Include dependencies
-include $(DEPS)

# ==============================================================================
# Run and Debug
# ==============================================================================

ifeq ($(ARCH), amd64)
QEMU = qemu-system-x86_64
QEMU_FLAGS = -m 1G -smp 4 -serial mon:stdio \
             -display default,show-cursor=on \
             -device virtio-gpu-pci \
             -device virtio-keyboard-pci -device virtio-mouse-pci \
             -drive if=none,file=$(BUILD_DIR)/disk.img,id=hd0,format=raw -device virtio-blk-pci,drive=hd0
else
QEMU = qemu-system-aarch64
QEMU_FLAGS = -M virt -cpu cortex-a57 -m 1G -smp 4 -serial mon:stdio \
             -display default,show-cursor=on \
             -device virtio-gpu-device \
             -device virtio-keyboard-device -device virtio-mouse-device \
             -drive if=none,file=$(BUILD_DIR)/disk.img,id=hd0,format=raw -device virtio-blk-device,drive=hd0
endif

# ==============================================================================
# Build rules

# Debug with QEMU and GDB
debug: all
	@echo "Starting QEMU in debug mode (waiting for GDB on :1234)..."
ifeq ($(ARCH), amd64)
	$(QEMU) $(QEMU_FLAGS) -cdrom build/os1test.iso -s -S
else
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -s -S
endif

# ==============================================================================
# Analysis Tools
# ==============================================================================

disasm: $(KERNEL_ELF) $(BOOTLOADER_ELF)
	$(OBJDUMP) -d $(KERNEL_ELF) > $(BUILD_DIR)/kernel.disasm
	$(OBJDUMP) -d $(BOOTLOADER_ELF) > $(BUILD_DIR)/bootloader.disasm
	$(OBJDUMP) -d $(USER_ELF) > $(BUILD_DIR)/init.disasm
	@echo "Disassembly saved to $(BUILD_DIR)/"

check:
	@echo "Checking toolchain..."
	@which $(CC) > /dev/null && echo "✓ CC: $(CC)" || echo "✗ CC not found"
	@which $(AS) > /dev/null && echo "✓ AS: $(AS)" || echo "✗ AS not found"
	@which $(LD) > /dev/null && echo "✓ LD: $(LD)" || echo "✗ LD not found"
	@which $(QEMU) > /dev/null && echo "✓ QEMU: $(QEMU)" || echo "✗ QEMU not found"

# ==============================================================================
# Clean
# ==============================================================================

rootfs: user
	@echo "Preparing RootFS directory..."
	@mkdir -p $(BUILD_DIR)/rootfs/bin
	@mkdir -p $(BUILD_DIR)/rootfs/etc
	@cp $(USER_ELFS) $(BUILD_DIR)/rootfs/bin/
	@cp user/bin/init.cfg $(BUILD_DIR)/rootfs/etc/
	@# Remove .elf extension from binaries in rootfs/bin for cleaner paths
	@for f in $(BUILD_DIR)/rootfs/bin/*.elf; do mv "$$f" "$${f%.elf}"; done

disk: $(MKDISK) kernel rootfs bootloader
	@echo "Assembling final disk image (GPT + Ext4)..."
	@mkdir -p $(BUILD_DIR)
ifeq ($(ARCH), amd64)
	@./$(MKDISK) $(BUILD_DIR)/disk.img none $(KERNEL_BIN) $(BUILD_DIR)/rootfs
else
	@./$(MKDISK) $(BUILD_DIR)/disk.img $(BOOTLOADER_BIN) $(KERNEL_BIN) $(BUILD_DIR)/rootfs
endif

$(MKDISK): tools/mkdisk.c
	@mkdir -p tools
	@echo "CC  tools/mkdisk.c"
	@gcc -o $(MKDISK) tools/mkdisk.c

clean:
	@echo "Cleaning up build artifacts..."
	-@rm -rf build/aarch64 2>/dev/null || true
	-@rm -rf build/amd64 2>/dev/null || true
	-@rm -rf build/*.img build/*.iso 2>/dev/null || true
	-@rm -rf $(BUILD_ROOT) 2>/dev/null || true
	@echo "Clean complete."

# ISO generation for AMD64
iso: kernel
ifeq ($(ARCH), amd64)
	@echo "Creating GRUB ISO..."
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	@cp $(KERNEL_ELF) $(BUILD_DIR)/iso/boot/kernel.elf
	@echo 'set timeout=0' > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo 'set default=0' >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo 'menuentry "os1test" {' >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo '  multiboot2 /boot/kernel.elf' >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo '  boot' >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo '}' >> $(BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $(BUILD_DIR)/os1test.iso $(BUILD_DIR)/iso 2>/dev/null || \
		echo "Warning: grub-mkrescue failed. Install xorriso and grub-common to create ISO."
	@echo "ISO Created: $(BUILD_DIR)/os1test.iso"
endif

run: all iso
	@echo ""
	@echo "========================================"
	@echo "  $(ARCH) Microkernel Boot"
	@echo "========================================"
	@echo ""
ifeq ($(ARCH), aarch64)
	@echo "Starting QEMU (aarch64 virt)..."
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)
else
	@if [ -f $(BUILD_DIR)/os1test.iso ]; then \
		echo "Starting QEMU with ISO image..."; \
		$(QEMU) $(QEMU_FLAGS) -cdrom $(BUILD_DIR)/os1test.iso; \
	else \
		echo "ISO not found (grub-mkrescue failed)."; \
		echo "Creating compatible 32-bit ELF for direct boot..."; \
		$(OBJCOPY) -I elf64-x86-64 -O elf32-i386 $(KERNEL_ELF) $(BUILD_DIR)/kernel32.elf; \
		echo "Starting QEMU via Multiboot..."; \
		$(QEMU) $(QEMU_FLAGS) -kernel $(BUILD_DIR)/kernel32.elf; \
	fi
endif

run-direct: all
	@echo "Starting QEMU (direct kernel boot)..."
	@echo "Press Ctrl+C to exit"
	@echo ""
ifeq ($(ARCH), aarch64)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)
else
	$(OBJCOPY) -I elf64-x86-64 -O elf32-i386 $(KERNEL_ELF) $(BUILD_DIR)/kernel32.elf
	$(QEMU) $(QEMU_FLAGS) -kernel $(BUILD_DIR)/kernel32.elf
endif

help:
	@echo "$(ARCH) Kernel Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build bootloader and kernel (default)"
	@echo "  bootloader - Build only bootloader"
	@echo "  kernel     - Build only kernel"
	@echo "  run        - Build and run with ISO boot"
	@echo "  run-direct - Build and run kernel directly"
	@echo "  debug      - Run with GDB debug"
	@echo "  disasm     - Generate disassembly"
	@echo "  clean      - Remove build artifacts"
	@echo "  check      - Verify toolchain"
	@echo "  help       - Show this help"

.PHONY: all kernel clean run run-direct iso help
