.DEFAULT_GOAL := all
ARCH ?= aarch64
BUILD_ROOT ?= build
SDK_ROOT ?= $(NEXS_SDK_ROOT)
ifeq ($(strip $(SDK_ROOT)),)
  $(error SDK_ROOT or NEXS_SDK_ROOT must name NX-SDK)
endif
NXSDK_ROOT := $(abspath $(SDK_ROOT))
include $(NXSDK_ROOT)/config/toolchain-versions.env
include $(NXSDK_ROOT)/config/extension-paths.mk
include $(NXSDK_ROOT)/config/userkit-paths.mk
include catalog.mk
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
AR := $(CROSS_COMPILE)ar
HOST_CC ?= cc
BUILD_DIR := $(abspath $(BUILD_ROOT))/$(ARCH)/nexsos1-lib
OBJ_DIR := $(BUILD_DIR)/obj
INCLUDE_FLAGS := $(addprefix -I$(CURDIR)/,$(NEXSOS1_LIB_INCLUDE_REL)) \
  -I$(NXSDK_ROOT)/include/contract -I$(NXSDK_ROOT)/include/api \
  $(NXSDK_EXTENSION_INCLUDE_FLAGS) $(NXSDK_USERKIT_INCLUDE_FLAGS)
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -Wconversion -Wsign-conversion \
  -ffreestanding -fno-builtin -nostdlib -nostartfiles -fno-common \
  -fstack-protector-strong -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g \
  $(ARCH_CFLAGS) $(INCLUDE_FLAGS)
KEYBOARD_OBJ := $(OBJ_DIR)/keyboard-layout/nxkeyboard_layout.o
KEYBOARD_ARCHIVE := $(BUILD_DIR)/libnexsos1-keyboard-layout.a
AGGREGATE_ARCHIVE := $(BUILD_DIR)/libnexsos1.a
.PHONY: all check clean info
all: $(KEYBOARD_ARCHIVE) $(AGGREGATE_ARCHIVE)
$(KEYBOARD_OBJ): keyboard-layout/src/nxkeyboard_layout.c
	@mkdir -p "$(dir $@)"
	@$(CC) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
$(KEYBOARD_ARCHIVE): $(KEYBOARD_OBJ)
	@mkdir -p "$(dir $@)"; rm -f "$@.tmp"; $(AR) rcs "$@.tmp" $(KEYBOARD_OBJ); mv "$@.tmp" "$@"
$(AGGREGATE_ARCHIVE): $(KEYBOARD_OBJ)
	@mkdir -p "$(dir $@)"; rm -f "$@.tmp"; $(AR) rcs "$@.tmp" $(KEYBOARD_OBJ); mv "$@.tmp" "$@"
check: all
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror \
	  -Ikeyboard-layout/include -I$(NXSDK_ROOT)/lib/extensions/input/include \
	  keyboard-layout/src/nxkeyboard_layout.c keyboard-layout/tests/keyboard_layout_test.c \
	  -o "$(BUILD_DIR)/host-tests/keyboard_layout_test"
	@"$(BUILD_DIR)/host-tests/keyboard_layout_test"
	@for header in look/include/nxres.h icon/include/nxicon.h exec-policy/include/nxexec.h; do test -s "$$header" || exit 1; done
	@echo "nexsos1-lib $(ARCH) check passed"
info:
	@printf 'archive=%s\nkeyboard_archive=%s\n' '$(AGGREGATE_ARCHIVE)' '$(KEYBOARD_ARCHIVE)'
DEPS := $(shell find "$(OBJ_DIR)" -type f -name '*.d' 2>/dev/null)
-include $(DEPS)
clean:
	@rm -rf "$(BUILD_ROOT)"
