# NX-HAL standalone migration build.
.DEFAULT_GOAL := all

ARCH ?= aarch64
BUILD_ROOT ?= build
HOST_OS ?= $(shell uname -s)
KERNEL_ROOT ?= ..
HAL_ROOT := .

ifeq ($(HOST_OS),Darwin)
  AMD64_CROSS_COMPILE ?= x86_64-elf-
  AARCH64_CROSS_COMPILE ?= aarch64-none-elf-
else ifeq ($(HOST_OS),Linux)
  AMD64_CROSS_COMPILE ?= x86_64-elf-
  AARCH64_CROSS_COMPILE ?= aarch64-none-elf-
else
  $(error Unsupported host OS $(HOST_OS))
endif
include $(KERNEL_ROOT)/config/toolchain-versions.env

ifeq ($(ARCH),amd64)
  CROSS_COMPILE ?= $(AMD64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AMD64 -mno-red-zone -mcmodel=large
else ifeq ($(ARCH),aarch64)
  CROSS_COMPILE ?= $(AARCH64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AARCH64 -mcpu=cortex-a57
else
  $(error ARCH must be amd64 or aarch64)
endif

CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
COMMON_FLAGS := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -ffreestanding -fno-builtin \
  -nostdlib -nostartfiles -fno-common -fstack-protector-strong \
  -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g -DNX_STRICT
HAL_INCLUDE := -I$(HAL_ROOT) -I$(HAL_ROOT)/include -I$(KERNEL_ROOT)/include -I$(HAL_ROOT)/arch/$(ARCH)/include \
  -I$(KERNEL_ROOT)/include/abi -I$(KERNEL_ROOT)/include/api
HAL_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) $(HAL_INCLUDE) \
  -Wundef -Wformat=2 -Wcast-align -Wpointer-arith -Wswitch-enum \
  -Wdouble-promotion -Wvla -Wmissing-declarations -DKERNEL

include sources.mk
HAL_REL_C := $(patsubst $(HAL_ROOT)/%,%,$(HAL_C_SOURCES))
HAL_REL_ASM := $(patsubst $(HAL_ROOT)/%,%,$(HAL_ASM_SOURCES))
HAL_C_OBJECTS := $(addprefix $(BUILD_DIR)/,$(HAL_REL_C:.c=.o))
HAL_ASM_OBJECTS := $(addprefix $(BUILD_DIR)/,$(HAL_REL_ASM:.S=.o))
HAL_OBJECTS := $(HAL_ASM_OBJECTS) $(HAL_C_OBJECTS)
HAL_LIB := $(BUILD_DIR)/libnxhal.a
DEPS := $(HAL_C_OBJECTS:.o=.d)

.PHONY: all clean check test test-host test-cross test-sanitize test-static
all: $(HAL_LIB)

$(HAL_LIB): $(HAL_OBJECTS)
	@mkdir -p $(dir $@)
	@$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(HAL_ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(HAL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/%.o: $(HAL_ROOT)/%.S
	@mkdir -p $(dir $@)
	@$(CC) $(HAL_CFLAGS) -c -o $@ $<

test:
	$(MAKE) -C tests test

test-host:
	$(MAKE) -C tests test-host

test-cross:
	$(MAKE) -C tests test-cross

test-sanitize:
	$(MAKE) -C tests test-sanitize

test-static:
	$(MAKE) -C tests test-static

check:
	@command -v "$(CC)" >/dev/null
	@command -v "$(AR)" >/dev/null
	@echo "NX-HAL $(ARCH): $$($(CC) --version | head -1)"

clean:
	rm -rf "$(BUILD_DIR)"

-include $(DEPS)
