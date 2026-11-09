# NX-CORE standalone build entrypoint.
# This repository owns the kernel source manifest and rules in build.mk.
.DEFAULT_GOAL := all

ARCH ?= aarch64
BUILD ?= debug
BUILD_ROOT ?= build
HOST_OS ?= $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
  AMD64_CROSS_COMPILE ?= x86_64-elf-
  AARCH64_CROSS_COMPILE ?= aarch64-none-elf-
else ifeq ($(HOST_OS),Linux)
  AMD64_CROSS_COMPILE ?= x86_64-elf-
  AARCH64_CROSS_COMPILE ?= aarch64-none-elf-
else
  $(error Unsupported host OS $(HOST_OS))
endif
include config/toolchain-versions.env

ifeq ($(findstring amd64,$(ARCH)),amd64)
  override ARCH := amd64
  CROSS_COMPILE ?= $(AMD64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AMD64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large
  HAL_ARCH_DIR := HAL/arch/amd64
  QEMU := qemu-system-x86_64
else ifeq ($(findstring aarch64,$(ARCH)),aarch64)
  override ARCH := aarch64
  CROSS_COMPILE ?= $(AARCH64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AARCH64 -mcpu=cortex-a57
  HAL_ARCH_DIR := HAL/arch/aarch64
  QEMU := qemu-system-aarch64
else
  $(error ARCH must be amd64 or aarch64)
endif

KERNEL_DIR := .
HAL_ROOT := HAL
BOOT_DIR := $(HAL_ARCH_DIR)/boot
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

COMMON_FLAGS := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -ffreestanding -fno-builtin \
  -nostdlib -nostartfiles -fno-common -fstack-protector-strong \
  -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g -DNX_STRICT
KERNEL_INCLUDE := -I$(HAL_ROOT) -I$(KERNEL_DIR)/include -I$(HAL_ROOT)/include -I$(HAL_ARCH_DIR)/include \
  -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api
KERNEL_HARDEN_FLAGS := -Wundef -Wformat=2 -Wcast-align -Wpointer-arith \
  -Wswitch-enum -Wdouble-promotion -Wvla -Wmissing-declarations
KERNEL_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) $(KERNEL_INCLUDE) \
  $(KERNEL_HARDEN_FLAGS) -DKERNEL

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
BOOTLOADER_ELF := $(BUILD_DIR)/bootloader.elf
BOOTLOADER_BIN := $(BUILD_DIR)/bootloader.bin
LDFLAGS_BOOT := -nostdlib -static -z noexecstack -T $(BOOT_DIR)/linker.ld
LDFLAGS_KERN := -nostdlib -static -z noexecstack -T $(HAL_ARCH_DIR)/kernel.ld \
  -Map $(BUILD_DIR)/kernel.map

include build.mk
-include $(DEPS)

.PHONY: all hal check check-contracts setup-deps clean run boot help
all: kernel bootloader
hal: $(HAL_LIB)

check: check-contracts
	@set -eu; for tool in "$(CC)" "$(LD)" "$(OBJCOPY)" "$(CROSS_COMPILE)nm"; do \
	  command -v "$$tool" >/dev/null || { echo "missing tool: $$tool" >&2; exit 1; }; done
	@case "$(ARCH):$$($(CC) -dumpmachine)" in \
	  amd64:x86_64-elf|aarch64:aarch64-none-elf) ;; \
	  *) echo "compiler target mismatch for $(ARCH): $$($(CC) -dumpmachine)" >&2; exit 1;; esac
	@expected_gcc="$(if $(filter amd64,$(ARCH)),$(NEXS_X86_64_GCC_VERSION),$(NEXS_AARCH64_GCC_VERSION))"; actual_gcc="$$($(CC) -dumpfullversion -dumpversion)"; test "$$actual_gcc" = "$$expected_gcc" || { echo "GCC $$actual_gcc; expected $$expected_gcc" >&2; exit 1; }
	@expected_binutils="$(if $(filter amd64,$(ARCH)),$(NEXS_X86_64_BINUTILS_VERSION),$(NEXS_AARCH64_BINUTILS_VERSION))"; actual_binutils="$$($(LD) --version | awk 'NR==1 {print $$NF}')"; test "$$actual_binutils" = "$$expected_binutils" || { echo "binutils $$actual_binutils; expected $$expected_binutils" >&2; exit 1; }
	@echo "NX-CORE $(ARCH): $$($(CC) --version | head -1)"

setup-deps:
	@if [ "$(HOST_OS)" = "Darwin" ]; then tools/setup-toolchain-macos.sh; \
	elif [ "$(HOST_OS)" = "Linux" ]; then tools/setup-toolchain-linux.sh; \
	else echo "Unsupported host OS: $(HOST_OS)" >&2; exit 1; fi

check-contracts:
	@tools/check-contract-snapshot.sh

clean:
	rm -rf "$(BUILD_DIR)"

UEFI_CODE ?= $(firstword $(wildcard /usr/local/share/qemu/edk2-aarch64-code.fd /usr/local/Cellar/qemu/*/share/qemu/edk2-aarch64-code.fd /opt/homebrew/share/qemu/edk2-aarch64-code.fd /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI_CODE.fd))
UEFI_VARS_TEMPLATE ?= $(firstword $(wildcard /usr/local/share/qemu/edk2-arm-vars.fd /usr/local/Cellar/qemu/*/share/qemu/edk2-arm-vars.fd /opt/homebrew/share/qemu/edk2-arm-vars.fd /usr/share/AAVMF/AAVMF_VARS.fd /usr/share/qemu-efi-aarch64/QEMU_VARS.fd))
.PHONY: boot-iso run-direct grub-arm64-tools
grub-arm64-tools:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" tools/build-grub-arm64.sh

ifeq ($(ARCH),amd64)
boot-iso: kernel
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" tools/build-amd64-grub-iso.sh
run: boot-iso
	$(QEMU) -machine q35 -cpu max -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -cdrom $(BUILD_DIR)/grub-bios.iso
run-direct: kernel
	$(QEMU) -machine q35 -cpu max -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -kernel "$(KERNEL_ELF)"
else
$(BUILD_DIR)/virt.dtb:
	@mkdir -p $(dir $@)
	$(QEMU) -M virt -cpu cortex-a57 -m 5G -smp 4 \
	  -device virtio-gpu-device -device virtio-keyboard-device \
	  -device virtio-mouse-device -machine dumpdtb=$@
boot-iso: kernel $(BUILD_DIR)/virt.dtb
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" tools/build-aarch64-uefi-image.sh
run: boot-iso
	@test -f "$(UEFI_CODE)" || { echo "Missing AArch64 EDK2 code image; set UEFI_CODE" >&2; exit 1; }
	@test -f "$(UEFI_VARS_TEMPLATE)" || { echo "Missing AArch64 EDK2 vars template; set UEFI_VARS_TEMPLATE" >&2; exit 1; }
	@cp "$(UEFI_VARS_TEMPLATE)" "$(BUILD_DIR)/grub-efi/vars.fd"
	$(QEMU) -machine virt -cpu cortex-a57 -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -boot order=d \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file="$(UEFI_CODE)" \
	  -drive if=pflash,format=raw,unit=1,file="$(BUILD_DIR)/grub-efi/vars.fd" \
	  -device virtio-gpu-device,id=nxgpu0 -device virtio-keyboard-device,id=nxkbd0,display=nxgpu0 \
	  -device virtio-mouse-device,id=nxmouse0,display=nxgpu0 \
	  -device virtio-scsi-device,id=scsi0 \
	  -drive if=none,file=$(BUILD_DIR)/grub-efi/nexsos-aarch64-uefi.iso,id=cdrom,media=cdrom,format=raw \
	  -device scsi-cd,drive=cdrom
run-direct: kernel $(BUILD_DIR)/virt.dtb
	$(QEMU) -machine virt -cpu cortex-a57 -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -dtb $(BUILD_DIR)/virt.dtb -kernel "$(KERNEL_BIN)"
endif
boot: run

help:
	@echo "NX-CORE standalone: make [ARCH=amd64|aarch64] all"
	@echo "Targets: hal kernel bootloader check run/boot clean"
