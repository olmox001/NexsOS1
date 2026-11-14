.DEFAULT_GOAL := all

ARCH ?= aarch64
BUILD_ROOT ?= build
BUILD_DIR := $(abspath $(BUILD_ROOT))/$(ARCH)/demo1
OBJ_DIR := $(BUILD_DIR)/obj
SDK_ROOT ?= $(NEXS_SDK_ROOT)
ifeq ($(strip $(SDK_ROOT)),)
  $(error SDK_ROOT or NEXS_SDK_ROOT must name an NX-SDK source/install root)
endif
HOST_OS ?= $(shell uname -s)

include $(SDK_ROOT)/config/toolchain-versions.env
NXSDK_ROOT := $(SDK_ROOT)
include $(SDK_ROOT)/config/extension-paths.mk
include $(SDK_ROOT)/config/userkit-paths.mk

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
COMMON_FLAGS := -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -Wconversion -Wsign-conversion \
  -ffreestanding -fno-builtin -nostdlib -nostartfiles -fno-common \
  -fstack-protector-strong -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g \
  -DNX_STRICT
DEMO_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) \
  -I$(SDK_ROOT)/include/contract -I$(SDK_ROOT)/include/api $(NXSDK_EXTENSION_INCLUDE_FLAGS) $(NXSDK_USERKIT_INCLUDE_FLAGS) \
  -ffunction-sections -fdata-sections
DEMO_LDFLAGS := -Wl,--gc-sections -Wl,-Ttext=0x80000000 -Wl,-e,_start

SDK_SOURCE_MODE := $(if $(wildcard $(SDK_ROOT)/lib/lib.c),1,0)
SDK_BUILD_ROOT := $(abspath $(BUILD_ROOT))/sdk
ifeq ($(SDK_SOURCE_MODE),1)
  NXSDK_ROOT := $(SDK_ROOT)
  NXSDK_BUILD_DIR := $(SDK_BUILD_ROOT)/$(ARCH)
  NXSDK_OUTPUT_NAMESPACE := system
  include $(SDK_ROOT)/sdk.mk
  DEMO_SDK_LINK_INPUTS := $(NXSDK_CORE_LIB_OBJECTS) $(NXSDK_MALLOC_OBJECT) $(NXSDK_SYSCALL_OBJECT)
  .PHONY: sdk
  sdk:
	+$(MAKE) -C $(SDK_ROOT) ARCH=$(ARCH) BUILD_ROOT="$(SDK_BUILD_ROOT)" core
else
  include $(SDK_ROOT)/runtime/$(ARCH)/nxsdk-config.mk
  DEMO_SDK_ARCHIVE := $(firstword $(wildcard $(NEXS_SDK_LIBDIR)/libnxsdk-core.a $(NEXS_SDK_LIBDIR)/libnxsdk.a))
  DEMO_SDK_LINK_INPUTS := $(DEMO_SDK_ARCHIVE) $(NEXS_SDK_CRTDIR)/syscall.o
  .PHONY: sdk
  sdk:
	@test -n "$(DEMO_SDK_ARCHIVE)" && test -s "$(DEMO_SDK_ARCHIVE)"
	@test -s "$(NEXS_SDK_CRTDIR)/syscall.o"
endif

include userland.mk

.PHONY: all check clean
all: sdk $(NEXS_DEMO1_ELFS)
	@echo "demo1 $(ARCH) built"

check: all
	@python3 tests/project_test.py
	@case "$(ARCH):$$($(CC) -dumpmachine)" in amd64:x86_64-elf|aarch64:aarch64-none-elf) ;; *) echo "compiler target mismatch" >&2; exit 1;; esac
	@echo "demo1 $(ARCH) check passed"

$(OBJ_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	@$(CC) $(DEMO_CFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/%.elf:
	@$(CC) $(DEMO_CFLAGS) $(DEMO_LDFLAGS) -o "$@" \
	  $(filter %.o,$^) -Wl,--start-group $(filter %.a,$^) -lgcc -Wl,--end-group

DEPS := $(shell find "$(OBJ_DIR)" -type f -name '*.d' 2>/dev/null)
-include $(DEPS)

clean:
	@rm -rf "$(BUILD_ROOT)"
