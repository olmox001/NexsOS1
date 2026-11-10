# NX-HAL canonical architecture/provider source manifest.
# Consumed by both kernel/build.mk and HAL/Makefile.
HAL_ROOT ?= HAL
HAL_ARCH_DIR := $(HAL_ROOT)/arch/$(ARCH)

ifeq ($(ARCH),amd64)
HAL_BOOT_SOURCES := \
    $(HAL_ARCH_DIR)/boot/header.S \
    $(HAL_ARCH_DIR)/boot/stage1.S \
    $(HAL_ARCH_DIR)/boot/stage2.S

HAL_ASM_SOURCES := \
    $(HAL_ARCH_DIR)/kinit/start.S \
    $(HAL_ARCH_DIR)/cpu/isr_stubs.S \
    $(HAL_ARCH_DIR)/cpu/syscall.S \
    $(HAL_ARCH_DIR)/cpu/context.S \
    $(HAL_ARCH_DIR)/kinit/trampoline.S

HAL_C_SOURCES := \
    $(HAL_ARCH_DIR)/cpu/cpu.c \
    $(HAL_ARCH_DIR)/cpu/idt.c \
    $(HAL_ARCH_DIR)/cpu/gdt.c \
    $(HAL_ARCH_DIR)/cpu/msr.c \
    $(HAL_ARCH_DIR)/cpu/syscall_hal.c \
    $(HAL_ARCH_DIR)/cpu/apic.c \
    $(HAL_ARCH_DIR)/mm/mmu.c \
    $(HAL_ARCH_DIR)/mm/tlb.c \
    $(HAL_ARCH_DIR)/platform/platform.c \
    $(HAL_ARCH_DIR)/boot_memory.c \
    $(HAL_ARCH_DIR)/hal.c \
    $(HAL_ARCH_DIR)/entropy.c \
    $(HAL_ARCH_DIR)/virtio.c \
    $(HAL_ARCH_DIR)/devices/uart_16550.c \
    $(HAL_ARCH_DIR)/devices/pic_pit.c
else ifeq ($(ARCH),aarch64)
HAL_BOOT_SOURCES := \
    $(HAL_ARCH_DIR)/boot/header.S \
    $(HAL_ARCH_DIR)/boot/stage1.S \
    $(HAL_ARCH_DIR)/boot/stage2.S

HAL_ASM_SOURCES := \
    $(HAL_ARCH_DIR)/kinit/start.S \
    $(HAL_ARCH_DIR)/cpu/exception.S

HAL_C_SOURCES := \
    $(HAL_ARCH_DIR)/cpu/cpu.c \
    $(HAL_ARCH_DIR)/cpu/syscall.c \
    $(HAL_ARCH_DIR)/mm/mmu.c \
    $(HAL_ARCH_DIR)/platform.c \
    $(HAL_ARCH_DIR)/boot_memory.c \
    $(HAL_ARCH_DIR)/hal.c \
    $(HAL_ARCH_DIR)/entropy.c \
    $(HAL_ARCH_DIR)/virtio.c \
    $(HAL_ARCH_DIR)/devices/uart_pl011.c \
    $(HAL_ARCH_DIR)/devices/gic.c \
    $(HAL_ARCH_DIR)/devices/arch_timer.c
else
$(error HAL: unsupported ARCH $(ARCH))
endif

HAL_C_SOURCES += $(HAL_ROOT)/common/boot_memory_record.c \
                 $(HAL_ROOT)/core/core.c \
                 $(HAL_ROOT)/core/time.c \
                 $(HAL_ROOT)/core/entropy.c \
                 $(HAL_ROOT)/core/frame.c \
                 $(HAL_ROOT)/core/core_protocol.c \
                 $(HAL_ROOT)/core/fault_protocol.c \
                 $(HAL_ROOT)/core/nxpax.c \
                 $(HAL_ROOT)/core/diagnostic_protocol.c \
                 $(HAL_ROOT)/core/irq_mask_stats.c \
                 $(HAL_ROOT)/core/timer_protocol.c \
                 $(HAL_ROOT)/core/smp_protocol.c \
                 $(HAL_ROOT)/core/device_protocol.c \
                 $(HAL_ROOT)/core/irq.c \
                 $(HAL_ROOT)/core/fatal_protocol.c \
                 $(HAL_ROOT)/core/cpu_protocol.c \
                 $(HAL_ROOT)/core/uaccess.c \
                 $(HAL_ROOT)/core/memory_protocol.c \
                 $(HAL_ROOT)/core/resource.c \
                 $(HAL_ROOT)/core/pci.c \
                 $(HAL_ROOT)/core/device_name.c

NXHAL_PROTOCOL_SOURCES := \
    $(HAL_ROOT)/common/hcd_crc64.c \
    $(HAL_ROOT)/common/hcd_parser.c \
    $(HAL_ROOT)/common/hcd_semantics.c \
    $(HAL_ROOT)/common/hcd_section_catalog.c \
    $(HAL_ROOT)/common/hcd_builder.c \
    $(HAL_ROOT)/common/memory_plan.c \
    $(HAL_ROOT)/capability/isa.c \
    $(HAL_ROOT)/capability/mmu.c \
    $(HAL_ROOT)/capability/hw_access_policy.c \
    $(HAL_ROOT)/capability/uefi_exit.c \
    $(HAL_ROOT)/capability/uefi_memory_map.c \
    $(HAL_ROOT)/capability/multiboot2_info.c \
    $(HAL_ROOT)/capability/multiboot2_efi_map.c \
    $(HAL_ROOT)/capability/multiboot2_memory_map.c

ifeq ($(ARCH),amd64)
NXHAL_ISA_DISCOVERY_SOURCE := $(HAL_ROOT)/capability/amd64/isa.c
else
NXHAL_ISA_DISCOVERY_SOURCE := $(HAL_ROOT)/capability/arm64/isa.c
endif

HAL_C_SOURCES += $(NXHAL_PROTOCOL_SOURCES) $(NXHAL_ISA_DISCOVERY_SOURCE)
