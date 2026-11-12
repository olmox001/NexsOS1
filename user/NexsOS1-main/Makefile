# NexsOS standalone first-party userland build.
.DEFAULT_GOAL := core
ARCH ?= aarch64
BUILD ?= debug
BUILD_ROOT ?= build
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
OBJ_DIR := $(BUILD_DIR)/obj
NXSDK_ROOT ?= $(NEXS_SDK_ROOT)
ifeq ($(strip $(NXSDK_ROOT)),)
$(error NXSDK_ROOT or NEXS_SDK_ROOT must name an NX-SDK source/install root)
endif
include $(NXSDK_ROOT)/config/toolchain-versions.env
include $(NXSDK_ROOT)/config/extension-paths.mk
include $(NXSDK_ROOT)/config/userkit-paths.mk
PROJECT_LIB_DIR := $(CURDIR)/lib
include $(PROJECT_LIB_DIR)/catalog.mk
PROJECT_LIB_BUILD_ROOT ?= $(abspath $(BUILD_ROOT))/project-lib
PROJECT_LIB_BUILD_DIR := $(PROJECT_LIB_BUILD_ROOT)/$(ARCH)/nexsos1-lib
PROJECT_LIB_ARCHIVE := $(PROJECT_LIB_BUILD_DIR)/$(NEXSOS1_LIB_LINK_ARCHIVE)
PROJECT_LIB_INCLUDE_FLAGS := $(addprefix -I$(PROJECT_LIB_DIR)/,$(NEXSOS1_LIB_INCLUDE_REL))
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
STB_INCLUDE ?= $(abspath $(CURDIR)/../LIBEXTERNAL/stb)
PL_MPEG_INCLUDE ?= $(abspath $(CURDIR)/../LIBEXTERNAL/pl_mpeg)
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
NEXS_USERLAND_SDK_LINK_INPUTS := $(SDK_SYSCALL_OBJECT) $(PROJECT_LIB_ARCHIVE) $(SDK_CORE_ARCHIVE)
USER_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) -I$(NXSDK_ROOT)/include/contract -I$(NXSDK_ROOT)/include/api $(NXSDK_EXTENSION_INCLUDE_FLAGS) $(NXSDK_USERKIT_INCLUDE_FLAGS) $(PROJECT_LIB_INCLUDE_FLAGS) -I$(NXSDK_ROOT)/tools/include \
  -ffunction-sections -fdata-sections
USER_LINK_FLAGS := -Wl,--gc-sections
ifeq ($(BUILD),release)
USER_LINK_FLAGS += -Wl,-s
endif
include userland.mk
.PHONY: core all check clean help require-sdk-core require-sdk-full sdk-core sdk-full project-libs
core: require-sdk-core project-libs $(NEXS_USERLAND_CORE_ELFS)
	@echo "NexsOS userland core built for $(ARCH): $(words $(NEXS_USERLAND_CORE_ELFS)) ELFs"
all: require-sdk-full project-libs
	@$(MAKE) --no-print-directory NEXS_USERLAND_SDK_LINK_INPUTS="$(SDK_SYSCALL_OBJECT) $(PROJECT_LIB_ARCHIVE) $(SDK_FULL_ARCHIVE)" $(NEXS_USERLAND_ALL_ELFS)
	@echo "NexsOS first-party userland built for $(ARCH): $(words $(NEXS_USERLAND_ALL_ELFS)) ELFs"
check: core
	@case "$(ARCH):$$($(CC) -dumpmachine)" in amd64:x86_64-elf|aarch64:aarch64-none-elf) ;; *) echo "compiler target mismatch" >&2; exit 1;; esac
	@echo "NexsOS userland $(ARCH) check passed"
project-libs:
	+$(MAKE) -C "$(PROJECT_LIB_DIR)" ARCH="$(ARCH)" BUILD_ROOT="$(PROJECT_LIB_BUILD_ROOT)" SDK_ROOT="$(NXSDK_ROOT)" check
	@test -s "$(PROJECT_LIB_ARCHIVE)"

$(PROJECT_LIB_ARCHIVE): | project-libs
	@test -s "$@"

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
