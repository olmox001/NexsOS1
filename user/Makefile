# NexsOS standalone first-party userland build.
.DEFAULT_GOAL := core
ARCH ?= aarch64
BUILD ?= debug
BUILD_ROOT ?= build
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
OBJ_DIR := $(BUILD_DIR)/obj
NXSDK_ROOT ?= $(shell tools/resolve-sdk.sh --print 2>/dev/null)
ifeq ($(strip $(NXSDK_ROOT)),)
$(error NX-SDK not found; set NEXS_SDK_ROOT/NEXS_SDK_INSTALL_PREFIX or run tools/resolve-sdk.sh --ensure)
endif
include $(NXSDK_ROOT)/config/toolchain-versions.env
ifeq ($(ARCH),amd64)
  CROSS_COMPILE ?= $(NEXS_X86_64_TARGET)-
  ARCH_CFLAGS := -DARCH_AMD64 -mno-red-zone -mcmodel=large
else ifeq ($(ARCH),aarch64)
  CROSS_COMPILE ?= $(NEXS_AARCH64_TARGET)-
  ARCH_CFLAGS := -DARCH_AARCH64 -mcpu=cortex-a57
else
  $(error ARCH must be amd64 or aarch64)
endif
CC := $(CROSS_COMPILE)gcc
STB_INCLUDE ?= $(CURDIR)/lib/stb
PL_MPEG_INCLUDE ?= $(CURDIR)/lib/pl_mpeg
NX_WARNINGS_AS_ERRORS ?= 0
NX_STRICT ?= 1
COMMON_FLAGS := -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -Wconversion -Wsign-conversion \
  -ffreestanding -fno-builtin -nostdlib -nostartfiles -fno-common \
  -fstack-protector-strong -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g
ifeq ($(NX_WARNINGS_AS_ERRORS),1)
COMMON_FLAGS += -Werror
endif
ifeq ($(NX_STRICT),1)
COMMON_FLAGS += -DNX_STRICT
endif
NXSDK_SOURCE_LAYOUT := $(wildcard $(NXSDK_ROOT)/lib/lib.c)
SDK_BUILD_ROOT ?= $(abspath $(BUILD_ROOT))/sdk
ifeq ($(strip $(NXSDK_SOURCE_LAYOUT)),)
  SDK_RUNTIME_ROOT := $(NXSDK_ROOT)/runtime/$(ARCH)
  SDK_LIB_DIR := $(SDK_RUNTIME_ROOT)/lib
  SDK_CRT_DIR := $(SDK_RUNTIME_ROOT)/crt
  SDK_FULL_ARCHIVE := $(SDK_LIB_DIR)/libnxsdk.a
  SDK_CORE_ARCHIVE := $(firstword $(wildcard $(SDK_LIB_DIR)/libnxsdk-core.a $(SDK_LIB_DIR)/libnxsdk.a))
else
  SDK_LIB_DIR := $(SDK_BUILD_ROOT)/$(ARCH)
  SDK_CRT_DIR := $(SDK_BUILD_ROOT)/$(ARCH)/system/arch/$(ARCH)
  SDK_FULL_ARCHIVE := $(SDK_LIB_DIR)/libnxsdk.a
  SDK_CORE_ARCHIVE := $(SDK_LIB_DIR)/libnxsdk-core.a
endif
SDK_SYSCALL_OBJECT := $(SDK_CRT_DIR)/syscall.o
NEXS_USERLAND_SDK_SIMD_OBJECT := $(SDK_CRT_DIR)/simd_probe.o
NEXS_USERLAND_SDK_LINK_INPUTS := $(SDK_SYSCALL_OBJECT) $(SDK_CORE_ARCHIVE)
USER_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) -I$(NXSDK_ROOT)/include/contract -I$(NXSDK_ROOT)/include/api -I$(NXSDK_ROOT)/tools/include \
  -ffunction-sections -fdata-sections
USER_LINK_FLAGS := -Wl,--gc-sections
ifeq ($(BUILD),release)
USER_LINK_FLAGS += -Wl,-s
endif
include userland.mk
.PHONY: core all check setup-sdk clean help require-sdk-core require-sdk-full sdk-core sdk-full
core: require-sdk-core $(NEXS_USERLAND_CORE_ELFS)
	@echo "NexsOS userland core built for $(ARCH): $(words $(NEXS_USERLAND_CORE_ELFS)) ELFs"
all: require-sdk-full
	@$(MAKE) --no-print-directory NEXS_USERLAND_SDK_LINK_INPUTS="$(SDK_SYSCALL_OBJECT) $(SDK_FULL_ARCHIVE)" $(NEXS_USERLAND_ALL_ELFS)
	@echo "NexsOS first-party userland built for $(ARCH): $(words $(NEXS_USERLAND_ALL_ELFS)) ELFs"
check: core
	@case "$(ARCH):$$($(CC) -dumpmachine)" in amd64:x86_64-elf|aarch64:aarch64-none-elf) ;; *) echo "compiler target mismatch" >&2; exit 1;; esac
	@echo "NexsOS userland $(ARCH) check passed"
setup-sdk:
	@tools/resolve-sdk.sh --ensure
sdk-core:
ifneq ($(strip $(NXSDK_SOURCE_LAYOUT)),)
	+$(MAKE) -C $(NXSDK_ROOT) ARCH=$(ARCH) BUILD_ROOT="$(SDK_BUILD_ROOT)" core-archive
endif

sdk-full:
ifneq ($(strip $(NXSDK_SOURCE_LAYOUT)),)
	@test -f "$(STB_INCLUDE)/stb_image.h" || { echo "NexsOS userland: STB_INCLUDE must provide stb_image.h for full SDK" >&2; exit 1; }
	@test -f "$(PL_MPEG_INCLUDE)/pl_mpeg.h" || { echo "NexsOS userland: PL_MPEG_INCLUDE must provide pl_mpeg.h for full SDK" >&2; exit 1; }
	+$(MAKE) -C $(NXSDK_ROOT) ARCH=$(ARCH) BUILD_ROOT="$(SDK_BUILD_ROOT)" \
	  STB_INCLUDE="$(abspath $(STB_INCLUDE))" PL_MPEG_INCLUDE="$(abspath $(PL_MPEG_INCLUDE))" archive
endif

require-sdk-core: sdk-core
	@test -s "$(SDK_SYSCALL_OBJECT)" || { echo "missing SDK startup object: $(SDK_SYSCALL_OBJECT)" >&2; exit 1; }
	@test -n "$(SDK_CORE_ARCHIVE)" && test -s "$(SDK_CORE_ARCHIVE)" || { echo "missing SDK core/full runtime under $(SDK_LIB_DIR)" >&2; exit 1; }
require-sdk-full: sdk-full
	@test -s "$(SDK_SYSCALL_OBJECT)" || { echo "missing SDK startup object: $(SDK_SYSCALL_OBJECT)" >&2; exit 1; }
	@test -s "$(SDK_FULL_ARCHIVE)" || { echo "missing full SDK runtime: $(SDK_FULL_ARCHIVE)" >&2; exit 1; }
	@test -s "$(NEXS_USERLAND_SDK_SIMD_OBJECT)" || { echo "missing SDK SIMD probe: $(NEXS_USERLAND_SDK_SIMD_OBJECT)" >&2; exit 1; }
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/%.elf:
	@$(CC) $(USER_CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ \
	  $(filter %.o,$^) -Wl,--start-group $(filter %.a,$^) -lgcc -Wl,--end-group
DEPS := $(shell find "$(OBJ_DIR)" -type f -name '*.d' 2>/dev/null)
-include $(DEPS)
clean:
	@rm -rf "$(BUILD_ROOT)"
help:
	@echo "NexsOS userland standalone: core (SDK core only), all (full SDK), check, clean"
