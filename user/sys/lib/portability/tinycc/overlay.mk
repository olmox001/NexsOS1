# user/sys/lib/portability/tinycc/overlay.mk
# TinyCC build integration for NexsOS1.
#
# Two distinct outputs, don't confuse them:
#   1. $(TINYCC_ELF)   — the tcc binary itself, /sys/bin/tcc. Built with the
#      REAL cross-compiler ($(CC) = $(CROSS_COMPILE)gcc), same as every other
#      user ELF in this tree. This is a bootstrap build, not self-hosting.
#   2. $(TINYCC_RT_DIR) — crt1.o/crti.o/crtn.o/libc.a/include/, the runtime
#      tcc(1) will itself link PROGRAMS THE USER COMPILES against, once it's
#      running live on NexsOS1. Installed to /sys/lib/tcc (must match
#      CONFIG_TCCDIR / CONFIG_TCC_CRTPREFIX in tinycc_config_nexsos.h).

TINYCC_PORT_DIR := user/sys/lib/portability/tinycc
TINYCC_DIR      := user/bin/tinycc

TINYCC_CONFIG   := $(TINYCC_PORT_DIR)/tinycc_config_nexsos.h
TINYCC_CFLAGS   := $(ARCH_CFLAGS) -O2 -g -Wall \
                    -Wno-error \
                    -ffreestanding -fno-builtin -nostdlib -nostartfiles \
                    -fno-common -fstack-protector-strong -fno-pic -fno-pie \
                    -fno-omit-frame-pointer \
                    -Iinclude/abi \
                    -Iinclude/api \
                    -I$(TINYCC_DIR) \
                    -include $(TINYCC_CONFIG)

TINYCC_OBJ_DIR  := $(BUILD_DIR)/tinycc
# Named tcc.elf (not tinycc.elf) on purpose: rootfs's ".elf"-stripping loop
# (root Makefile, rootfs: target) turns this into /sys/bin/tcc, the
# conventional Unix compiler-driver name.
TINYCC_ELF      := $(BUILD_DIR)/tcc.elf

# ---------------------------------------------------------------------------
# 1. tcc.elf itself
# ---------------------------------------------------------------------------
$(TINYCC_OBJ_DIR)/%.o: $(TINYCC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(TINYCC_CFLAGS) -MMD -MP -c $< -o $@

$(TINYCC_ELF): $(TINYCC_OBJ_DIR)/tinycc.o \
               $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# ---------------------------------------------------------------------------
# 2. tcc's own runtime (what CONFIG_TCC_CRTPREFIX / CONFIG_TCC_LIBPATHS /
#    CONFIG_TCCDIR point at) — installed under /sys/lib/tcc on the rootfs.
# ---------------------------------------------------------------------------
TINYCC_RT_DIR := $(BUILD_DIR)/tinycc-runtime
AR ?= $(CROSS_COMPILE)ar

# crt1.o *is* the _start every other NexsOS1 binary already links against —
# user/arch/$(ARCH)/syscall.S: `_start: call main; call _sys_exit`. tcc only
# needs a file literally named crt1.o under CONFIG_TCC_CRTPREFIX (see
# tcc_add_crt(s, "crt1.o") in tinycc.h); nothing NexsOS1-specific needs to be
# added on top of what that object already does.
$(TINYCC_RT_DIR)/crt1.o: $(USER_SYSCALL_O)
	@mkdir -p $(dir $@)
	@cp $< $@

# GNU convention splits the .init/.fini prologue/epilogue across crti.o and
# crtn.o. NexsOS1 binaries have no .init_array/.fini_array startup phase
# (nothing here calls into one), so both are harmless empty objects — tcc
# only needs the files to exist and link cleanly.
$(TINYCC_RT_DIR)/crti.o:
	@mkdir -p $(dir $@)
	@printf '' | $(CC) $(TINYCC_CFLAGS) -x c -c - -o $@

$(TINYCC_RT_DIR)/crtn.o:
	@mkdir -p $(dir $@)
	@printf '' | $(CC) $(TINYCC_CFLAGS) -x c -c - -o $@

# libc.a — the same lib.o/malloc.o object set already linked into every
# cu_*.elf (coreutils), archived so tcc's own -lc default pulls it in.
$(TINYCC_RT_DIR)/libc.a: $(USER_LIB_O) $(USER_MALLOC_O)
	@mkdir -p $(dir $@)
	@rm -f $@
	@$(AR) rcs $@ $^

$(TINYCC_RT_DIR)/include: $(wildcard $(TINYCC_DIR)/include/*.h)
	@mkdir -p $(dir $@)
	@rm -rf $@
	@cp -r $(TINYCC_DIR)/include $@

tinycc-runtime: $(TINYCC_RT_DIR)/crt1.o $(TINYCC_RT_DIR)/crti.o \
                 $(TINYCC_RT_DIR)/crtn.o $(TINYCC_RT_DIR)/libc.a \
                 $(TINYCC_RT_DIR)/include

tinycc: $(TINYCC_ELF) tinycc-runtime
.PHONY: tinycc tinycc-runtime
