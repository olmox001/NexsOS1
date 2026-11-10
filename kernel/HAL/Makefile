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
  ARCH_CFLAGS := -DARCH_AMD64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large
else ifeq ($(ARCH),aarch64)
  CROSS_COMPILE ?= $(AARCH64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AARCH64 -mcpu=cortex-a57
else
  $(error ARCH must be amd64 or aarch64)
endif

CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
COMMON_FLAGS := -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -Wconversion -Wsign-conversion \
  -ffreestanding -fno-builtin \
  -nostdlib -nostartfiles -fno-common -fstack-protector-strong \
  -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g -DNX_STRICT
# Fatal by default for the baseline correctness/hardening warning classes.
# Conversion diagnostics remain visible in every build while each narrowing
# site is proved and repaired. No -Wno-error or warning suppressions.
# NX_WARNINGS_AS_ERRORS=0 explicitly opts out of fatal baseline warnings;
# NX_CONVERSION_AS_ERROR=1 makes the conversion migration fail-fast as well.
NX_WARNINGS_AS_ERRORS ?= 1
NX_CONVERSION_AS_ERROR ?= 0
ifeq ($(NX_WARNINGS_AS_ERRORS),1)
COMMON_FLAGS += -Werror=all -Werror=extra -Werror=pedantic \
  -Werror=shadow -Werror=write-strings -Werror=missing-prototypes \
  -Werror=strict-prototypes -Werror=undef -Werror=format=2 -Werror=cast-align \
  -Werror=pointer-arith -Werror=switch-enum -Werror=double-promotion \
  -Werror=vla -Werror=missing-declarations
endif
ifeq ($(NX_CONVERSION_AS_ERROR),1)
COMMON_FLAGS += -Werror=conversion -Werror=sign-conversion
endif
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

.PHONY: all clean check boundary test test-host test-cross test-sanitize test-static
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

boundary: $(HAL_LIB)
	@tools/audit-boundary.sh $(ARCH)

check: boundary
	@command -v "$(CC)" >/dev/null
	@command -v "$(AR)" >/dev/null
	@echo "NX-HAL $(ARCH): $$($(CC) --version | head -1)"

clean:
	rm -rf "$(BUILD_DIR)"

-include $(DEPS)
