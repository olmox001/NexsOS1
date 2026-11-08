# NX-CORE canonical source manifest and build rules.
# Included by the integration Makefile and NX-CORE standalone Makefile.

ifeq ($(ARCH), amd64)
BOOT_SOURCES = \
    $(BOOT_DIR)/header.S \
    $(BOOT_DIR)/stage1.S \
    $(BOOT_DIR)/stage2.S

KERN_ASM_SOURCES = \
    $(ARCH_DIR)/kinit/start.S \
    $(ARCH_DIR)/cpu/isr_stubs.S \
    $(ARCH_DIR)/cpu/syscall.S \
    $(ARCH_DIR)/cpu/context.S \
    $(ARCH_DIR)/kinit/trampoline.S

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
    $(ARCH_DIR)/kinit/start.S \
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
    $(KERNEL_DIR)/core/smp.c \
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
    $(KERNEL_DIR)/fs/procfs.c \
    $(KERNEL_DIR)/mm/pmm.c \
    $(KERNEL_DIR)/mm/vmm.c \
    $(KERNEL_DIR)/mm/buffer.c \
    $(KERNEL_DIR)/lib/string.c \
    $(KERNEL_DIR)/lib/crc32.c \
    $(KERNEL_DIR)/lib/sha256.c \
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
    $(KERNEL_DIR)/graphics/surface.c \
    $(KERNEL_DIR)/graphics/chrome.c \
    $(KERNEL_DIR)/graphics/region.c \
    $(KERNEL_DIR)/graphics/gl.c \
    $(KERNEL_DIR)/graphics/font.c \
    $(KERNEL_DIR)/graphics/term.c \
    $(KERNEL_DIR)/graphics/compositor_style.c \
    $(KERNEL_DIR)/graphics/compositor.c \
    $(KERNEL_DIR)/irq/irq.c \
    $(KERNEL_DIR)/lib/fdt.c \
    $(KERNEL_DIR)/lib/fdt_parser.c \
    $(KERNEL_DIR)/main.c \
    $(KERNEL_DIR)/drivers/ps2/ps2.c

# Object files
BOOT_OBJECTS = $(patsubst %.S,$(BUILD_DIR)/%.o,$(BOOT_SOURCES))
KERN_ASM_OBJECTS = $(KERN_ASM_SOURCES:%.S=$(BUILD_DIR)/%.o)
KERN_C_OBJECTS = $(KERN_C_SOURCES:%.c=$(BUILD_DIR)/%.o)
KERN_OBJECTS = $(KERN_ASM_OBJECTS) $(KERN_C_OBJECTS)

# Dependency files
# Every %.o: %.c rule (kernel AND user, see "Common compilation rules" below)
# passes -MMD -MP, so a per-TU .d file listing every header it pulled in
# lands next to each .o.  DEPS used to be JUST the kernel ones
# (KERN_C_OBJECTS:.o=.d) — userland .o files (nxres.o, nxsettings.o, ...)
# never got their .d included, so `make` had no idea a userland .c depended
# on a header like nxres.h/style_names.h: editing ONLY the header left every
# .o that #included it stale and un-rebuilt, silently running old code with
# no compile error to flag it.  Globbing every .d already on disk (kernel
# and user alike) closes that gap; empty on a clean tree (no .d files yet),
# which is fine — -include tolerates a missing/empty list.
DEPS = $(shell find $(BUILD_DIR) -type f -name '*.d' 2>/dev/null)


# NX-CORE-owned kernel and bootloader build rules.
.PHONY: kernel bootloader
kernel: $(KERNEL_BIN)
bootloader: $(BOOTLOADER_BIN)

$(BOOTLOADER_ELF): $(BOOT_OBJECTS) $(BOOT_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS_BOOT) -o $@ $(BOOT_OBJECTS)
$(BOOTLOADER_BIN): $(BOOTLOADER_ELF)
	@$(OBJCOPY) -O binary $< $@

NM = $(CROSS_COMPILE)nm
KERNEL_PRE = $(BUILD_DIR)/kernel.pre.elf
KSYMS_S = $(BUILD_DIR)/ksyms.S
KSYMS_O = $(BUILD_DIR)/ksyms.o
$(KERNEL_PRE): $(KERN_OBJECTS)
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS_KERN) -o $@ $^
$(KSYMS_O): $(KERNEL_PRE) $(KERNEL_DIR)/tools/gen_ksyms.sh
	@$(KERNEL_DIR)/tools/gen_ksyms.sh "$(NM)" $(KERNEL_PRE) $(KSYMS_S)
	@$(CC) $(KERNEL_CFLAGS) -c -o $@ $(KSYMS_S)
$(KERNEL_ELF): $(KERN_OBJECTS) $(KSYMS_O)
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS_KERN) -o $@ $^
$(KERNEL_BIN): $(KERNEL_ELF)
	@$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(BOOT_DIR)/%.o: $(BOOT_DIR)/%.S
	@mkdir -p $(dir $@)
	@$(CC) $(KERNEL_CFLAGS) -c -o $@ $<
$(BUILD_DIR)/$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.S
	@mkdir -p $(dir $@)
	@$(CC) $(KERNEL_CFLAGS) -c -o $@ $<
$(BUILD_DIR)/$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(KERNEL_CFLAGS) -MMD -MP -c -o $@ $<
