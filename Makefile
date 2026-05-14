# Makefile for OS1Test (AMD64 / AArch64)
# Cross-compilation for bare-metal kernel
# ==============================================================================
# Configuration
# ==============================================================================

# Target Architecture (default to aarch64)
ARCH ?= aarch64

# Release Versioning (default V9.9.9)
VERSION ?= V9.9.9
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

# Resolve grub-mkrescue
GRUB_MKRESCUE := $(shell command -v i686-elf-grub-mkrescue 2>/dev/null || command -v grub-mkrescue 2>/dev/null || echo "")

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
DISK_IMG   = $(BUILD_DIR)/disk.img

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
    $(ARCH_DIR)/mm/uaccess.c \
    $(KERNEL_DIR)/drivers/uart/16550.c \
    $(KERNEL_DIR)/drivers/timer/pic_pit.c \
    $(ARCH_DIR)/platform/platform.c \
    $(ARCH_DIR)/hal.c \
    $(ARCH_DIR)/virtio.c \
    $(KERNEL_DIR)/drivers/pci/pci.c
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
    $(KERNEL_DIR)/fs/vfs.c \
    $(KERNEL_DIR)/mm/pmm.c \
    $(KERNEL_DIR)/mm/vmm.c \
    $(KERNEL_DIR)/mm/buffer.c \
    $(KERNEL_DIR)/lib/string.c \
    $(KERNEL_DIR)/lib/crc32.c \
    $(KERNEL_DIR)/lib/vsnprintf.c \
    $(KERNEL_DIR)/lib/printk.c \
    $(KERNEL_DIR)/lib/stack_protector.c \
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
    $(KERNEL_DIR)/graphics/compositor.c \
    $(KERNEL_DIR)/irq/irq.c \
    $(KERNEL_DIR)/lib/fdt.c \
    $(KERNEL_DIR)/main.c

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
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/bin
	@mkdir -p $(BUILD_DIR)/$(USER_ARCH_DIR)
	@mkdir -p $(BUILD_DIR)/$(USER_ARCH_DIR)

# Bootloader
bootloader: $(BOOTLOADER_BIN)

$(BOOTLOADER_ELF): $(BOOT_OBJECTS)
	@$(LD) $(LDFLAGS_BOOT) -o $@ $^

$(BOOTLOADER_BIN): $(BOOTLOADER_ELF)
	@$(OBJCOPY) -O binary $< $@

# Kernel
kernel: $(KERNEL_BIN)

$(KERNEL_ELF): $(KERN_OBJECTS)
	@$(LD) $(LDFLAGS_KERN) -o $@ $^

$(KERNEL_BIN): $(KERNEL_ELF)
	@$(OBJCOPY) -O binary $< $@

# Userland
USER_SYSCALL_O = $(BUILD_DIR)/$(USER_ARCH_DIR)/syscall.o
USER_LIB_O     = $(BUILD_DIR)/$(USER_DIR)/lib/lib.o
USER_MALLOC_O  = $(BUILD_DIR)/$(USER_DIR)/lib/malloc.o

USER_ELFS = $(BUILD_DIR)/init.elf $(BUILD_DIR)/counter.elf $(BUILD_DIR)/shell.elf \
            $(BUILD_DIR)/demo3d.elf $(BUILD_DIR)/ipc_send.elf $(BUILD_DIR)/ipc_recv.elf \
            $(BUILD_DIR)/notify_srv.elf $(BUILD_DIR)/crash.elf $(BUILD_DIR)/regedit.elf \
            $(BUILD_DIR)/writetest.elf $(BUILD_DIR)/doom.elf $(BUILD_DIR)/input_test.elf \
            $(BUILD_DIR)/fontman.elf
USER_TARGETS = $(USER_DIR)/bin/shell.elf $(USER_DIR)/bin/ls.elf $(USER_DIR)/bin/cat.elf \
               $(USER_DIR)/bin/mkdir.elf $(USER_DIR)/bin/ps.elf $(USER_DIR)/bin/demo.elf \
               $(USER_DIR)/bin/demo3d.elf $(USER_DIR)/bin/doom.elf $(USER_DIR)/bin/input_test.elf \
               $(USER_DIR)/bin/fontman.elf

user: $(USER_ELFS)

# User Compilations
$(BUILD_DIR)/$(USER_DIR)/lib/%.o: $(USER_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/bin/%.o: $(USER_DIR)/bin/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Explicit dependencies for each user ELF
$(BUILD_DIR)/init.elf: $(BUILD_DIR)/$(USER_DIR)/bin/init.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/counter.elf: $(BUILD_DIR)/$(USER_DIR)/bin/counter.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/shell.elf: $(BUILD_DIR)/$(USER_DIR)/bin/shell.o $(BUILD_DIR)/$(USER_DIR)/bin/proce.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/demo3d.elf: $(BUILD_DIR)/$(USER_DIR)/bin/demo3d.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/ipc_send.elf: $(BUILD_DIR)/$(USER_DIR)/bin/ipc_send.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/ipc_recv.elf: $(BUILD_DIR)/$(USER_DIR)/bin/ipc_recv.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/notify_srv.elf: $(BUILD_DIR)/$(USER_DIR)/bin/notification_server.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/crash.elf: $(BUILD_DIR)/$(USER_DIR)/bin/crash.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/regedit.elf: $(BUILD_DIR)/$(USER_DIR)/bin/regedit.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/writetest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/writetest.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/input_test.elf: $(BUILD_DIR)/$(USER_DIR)/bin/input_test.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
$(BUILD_DIR)/fontman.elf: $(BUILD_DIR)/$(USER_DIR)/bin/fontman/fontman.o $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/$(USER_DIR)/bin/fontman/%.o: $(USER_DIR)/bin/fontman/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@


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
	@mkdir -p $(BUILD_DIR)/rootfs/bin
	@mkdir -p $(BUILD_DIR)/rootfs/etc
	@cp $(USER_ELFS) $(BUILD_DIR)/rootfs/bin/
	@cp user/bin/init.cfg $(BUILD_DIR)/rootfs/etc/
	@# Copy essential WAD files to the root and /bin for engine detection
	@-cp user/bin/doomgeneric-master/DOOM_wads-master/doom.wad $(BUILD_DIR)/rootfs/ 2>/dev/null || true
	@-cp user/bin/doomgeneric-master/DOOM_wads-master/doom1.wad $(BUILD_DIR)/rootfs/ 2>/dev/null || true
	@-cp user/bin/doomgeneric-master/DOOM_wads-master/doom2.wad $(BUILD_DIR)/rootfs/ 2>/dev/null || true
	@-cp user/bin/doomgeneric-master/DOOM_wads-master/doom.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/doomgeneric-master/DOOM_wads-master/doom1.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/bin/doomgeneric-master/DOOM_wads-master/doom2.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/rootfs/fonts
	@-cp user/bin/fontman/fonts/*.ttf $(BUILD_DIR)/rootfs/fonts/ 2>/dev/null || true
	@-cp user/bin/fontman/fonts/*.off $(BUILD_DIR)/rootfs/fonts/ 2>/dev/null || true
	@for f in $(BUILD_DIR)/rootfs/bin/*.elf; do mv "$$f" "$${f%.elf}"; done

disk: $(MKDISK) kernel rootfs bootloader
	@mkdir -p $(BUILD_DIR)
	@./$(MKDISK) $(DISK_IMG) $(BOOTLOADER_BIN) $(KERNEL_BIN) $(BUILD_DIR)/rootfs

$(MKDISK): tools/mkdisk.c
	@mkdir -p tools
	@gcc -o $(MKDISK) tools/mkdisk.c

# ==============================================================================
# QEMU Flags
# ==============================================================================

ifeq ($(ARCH), amd64)
QEMU_FLAGS = -m 3G -smp 4 -serial mon:stdio \
             -display default,show-cursor=on \
             -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
             -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
             -device virtio-mouse-pci,disable-legacy=on,disable-modern=off \
             -drive if=none,file=$(DISK_IMG),id=hd0,format=raw \
             -device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off

QEMU_RELEASE_FLAGS = -m 3G -smp 4 -serial mon:stdio \
                     -display default,show-cursor=on \
                     -device virtio-gpu-pci,disable-legacy=on,disable-modern=off \
                     -device virtio-keyboard-pci,disable-legacy=on,disable-modern=off \
                     -device virtio-mouse-pci,disable-legacy=on,disable-modern=off
else
QEMU_FLAGS = -M virt -cpu cortex-a57 -m 3G -smp 4 -serial mon:stdio \
             -display default,show-cursor=on \
             -device virtio-gpu-device \
             -device virtio-keyboard-device -device virtio-mouse-device \
             -drive if=none,file=$(DISK_IMG),id=hd0,format=raw -device virtio-blk-device,drive=hd0

QEMU_RELEASE_FLAGS = -M virt -cpu cortex-a57 -m 3G -smp 4 -serial mon:stdio \
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
ifeq ($(ARCH), amd64)
	@rm -rf $(RELEASE_DIR)/iso
	@mkdir -p $(RELEASE_DIR)/iso/boot/grub
	
	@cp $(KERNEL_ELF) $(RELEASE_DIR)/iso/boot/kernel.elf
	
	@echo 'set timeout=0' > $(RELEASE_DIR)/iso/boot/grub/grub.cfg
	@echo 'set default=0' >> $(RELEASE_DIR)/iso/boot/grub/grub.cfg
	@echo 'menuentry "OS1Test" {' >> $(RELEASE_DIR)/iso/boot/grub/grub.cfg
	@echo '  multiboot2 /boot/kernel.elf' >> $(RELEASE_DIR)/iso/boot/grub/grub.cfg
	@echo '  boot' >> $(RELEASE_DIR)/iso/boot/grub/grub.cfg
	@echo '}' >> $(RELEASE_DIR)/iso/boot/grub/grub.cfg
	
	@dd if=$(DISK_IMG) of=$(BUILD_DIR)/userland.img bs=512 skip=34850 status=none
	
	$(GRUB_MKRESCUE) -o $(RELEASE_DIR)/os1test-amd64-$(VERSION).iso $(RELEASE_DIR)/iso \
		-- -append_partition 2 0x83 $(BUILD_DIR)/userland.img
	
	@echo "✓ AMD64 Hybrid ISO: $(RELEASE_DIR)/os1test-amd64-$(VERSION).iso"
else
	@cp $(KERNEL_BIN) $(RELEASE_DIR)/kernel.img
	@cp $(DISK_IMG) $(RELEASE_DIR)/disk.img
	@echo "✓ AArch64 release files: kernel.img, disk.img"
endif

test-release: release-arch
	@echo "Starting QEMU Release Test for $(ARCH) (Version: $(VERSION))..."
ifeq ($(ARCH), amd64)
	$(QEMU) $(QEMU_RELEASE_FLAGS) \
		-drive if=none,file=$(RELEASE_DIR)/os1test-amd64-$(VERSION).iso,id=hd0,format=raw \
		-device virtio-blk-pci,drive=hd0,disable-legacy=on,disable-modern=off
else
	$(QEMU) $(QEMU_RELEASE_FLAGS) -kernel $(RELEASE_DIR)/kernel.img
endif

# ==============================================================================
# Development Execution
# ==============================================================================

run: all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

run-direct: run

debug: all
ifeq ($(ARCH), amd64)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF) -s -S
else
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
	@echo "  release      - Build production files (Use: make release VERSION=0.1.2)"
	@echo "  test-release - Build release and test it in QEMU (Use: make test-release VERSION=0.1.2)"
	@echo "  run          - Build and run kernel directly"
	@echo "  clean        - Remove build artifacts"

DOOM_DIR = user/bin/doomgeneric-master/doomgeneric
DOOM_SRC = am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c dummy.c
DOOM_OBJS = $(addprefix $(BUILD_DIR)/$(DOOM_DIR)/, $(DOOM_SRC:.c=.o))
DOOM_PLATFORM_OBJ = $(BUILD_DIR)/user/bin/doomgeneric_os1.o

# Doom code is legacy, relax strict warnings
DOOM_CFLAGS = -w -ffreestanding -fno-builtin -nostdlib -nostartfiles -fno-common -O2 -g $(ARCH_CFLAGS) $(INCLUDE)
DOOM_CFLAGS += -I$(DOOM_DIR) -DNORMALUNIX

$(BUILD_DIR)/doom.elf: $(DOOM_OBJS) $(DOOM_PLATFORM_OBJ) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	echo "[Linking Doom]"
	$(CC) $(DOOM_CFLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

$(DOOM_PLATFORM_OBJ): user/bin/doomgeneric_os1.c
	mkdir -p $(dir $@)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(DOOM_OBJS): $(BUILD_DIR)/$(DOOM_DIR)/%.o: $(DOOM_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(DOOM_CFLAGS) -I$(DOOM_DIR) -DNORMALUNIX -c $< -o $@