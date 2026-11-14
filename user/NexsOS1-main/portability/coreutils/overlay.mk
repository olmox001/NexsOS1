# user/NexsOS1-main/portability/coreutils/overlay.mk
# Coreutils portability layer over the imported BINEXTERNAL source.


COREUTILS_PORT_DIR := user/NexsOS1-main/portability/coreutils
COREUTILS_DIR      := $(NEXS_BINEXTERNAL_DIR_coreutils)
COREUTILS_SRC_DIR  := $(COREUTILS_DIR)/src
COREUTILS_GENERATED_DIR := $(BUILD_DIR)/coreutils-generated
COREUTILS_PRIMES_H := $(COREUTILS_GENERATED_DIR)/primes.h


COREUTILS_CONFIG   := $(COREUTILS_PORT_DIR)/coreutils_config_nexsos.h
# Coreutils itself is a vendor gitlink. Preserve its diagnostics without
# blocking the NexsOS build; our portability/glue layer remains Werror-strict.
COREUTILS_VENDOR_CFLAGS := $(ARCH_CFLAGS) -O2 -g -Wall -Wextra -Wpedantic \
                      -ffreestanding -fno-builtin -nostdlib -nostartfiles \
                      -fno-common -fstack-protector-strong -fno-pic -fno-pie \
                      -fno-omit-frame-pointer -DNEXSOS_EXTERNAL_VENDOR \
                      $(USER_INCLUDE) \
                      -I$(COREUTILS_GENERATED_DIR) \
                      -I$(GNULIB_GEN_INC_DIR) \
                      -I$(COREUTILS_PORT_DIR) \
                      -I$(GNULIB_PORT_DIR) \
                      -isystem $(GNULIB_DIR)/lib \
                      -isystem $(COREUTILS_DIR)/src \
                      -isystem $(COREUTILS_DIR)/lib \
                      -isystem $(COREUTILS_DIR)/gl/lib \
                      -include $(COREUTILS_CONFIG)
COREUTILS_PORT_CFLAGS := $(USER_CFLAGS) \
                      -I$(COREUTILS_GENERATED_DIR) \
                      -I$(GNULIB_GEN_INC_DIR) \
                      -I$(COREUTILS_PORT_DIR) \
                      -I$(GNULIB_PORT_DIR) \
                      -isystem $(GNULIB_DIR)/lib \
                      -isystem $(COREUTILS_DIR)/src \
                      -isystem $(COREUTILS_DIR)/lib \
                      -isystem $(COREUTILS_DIR)/gl/lib \
                      -include $(COREUTILS_CONFIG)


COREUTILS_OBJ_DIR  := $(BUILD_DIR)/coreutils
COREUTILS_COMMON_OBJS := $(COREUTILS_OBJ_DIR)/version.o $(COREUTILS_OBJ_DIR)/coreutils_os1_glue.o


COREUTILS_NAMES := \
  echo true false pwd uname cat yes sleep mkdir rmdir unlink sync basename dirname whoami printenv \
  head tail wc tee touch chmod chown chgrp cut tr fold paste nl seq shuf nproc readlink realpath tty hostid logname users who \
  expr factor fmt link mknod nice nohup pathchk truncate tsort uniq uptime


COREUTILS_ELFS := $(patsubst %,$(BUILD_DIR)/cu_%.elf,$(COREUTILS_NAMES))

# Coreutils-generated Gnulib compatibility sources are app-specific and must
# not be part of the generic Gnulib SDK archive.
COREUTILS_GNULIB_SRCS := \
  $(COREUTILS_DIR)/gl/lib/root-dev-ino.c \
  $(COREUTILS_DIR)/gl/lib/xfts.c \
  $(COREUTILS_DIR)/gl/lib/fadvise.c \
  $(COREUTILS_DIR)/gl/lib/cl-strtod.c \
  $(COREUTILS_DIR)/gl/lib/cl-strtold.c \
  $(COREUTILS_DIR)/gl/lib/rand-isaac.c \
  $(COREUTILS_DIR)/gl/lib/randint.c \
  $(COREUTILS_DIR)/gl/lib/randperm.c \
  $(COREUTILS_DIR)/gl/lib/randread.c \
  $(COREUTILS_DIR)/gl/lib/xdectoimax.c \
  $(COREUTILS_DIR)/gl/lib/xdectoumax.c \
  $(COREUTILS_DIR)/gl/lib/buffer-lcm.c \
  $(COREUTILS_DIR)/gl/lib/fd-reopen.c \
  $(COREUTILS_DIR)/gl/lib/strintcmp.c \
  $(COREUTILS_DIR)/gl/lib/strnumcmp.c \
  $(COREUTILS_SRC_DIR)/prog-fprintf.c

COREUTILS_GNULIB_OBJ_DIR := $(BUILD_DIR)/coreutils-gnulib
COREUTILS_GNULIB_OBJS := $(patsubst %.c,$(COREUTILS_GNULIB_OBJ_DIR)/%.o,$(COREUTILS_GNULIB_SRCS))
COREUTILS_GL_LIB := $(BUILD_DIR)/libcoreutils-gnulib.a
COREUTILS_PROGRAM_OBJS := $(patsubst %,$(COREUTILS_OBJ_DIR)/%.o,$(COREUTILS_NAMES)) \
  $(COREUTILS_COMMON_OBJS) $(COREUTILS_OBJ_DIR)/true-true.o \
  $(COREUTILS_OBJ_DIR)/true-mode-true.o $(COREUTILS_OBJ_DIR)/true-mode-false.o \
  $(COREUTILS_OBJ_DIR)/uname-mode.o $(COREUTILS_OBJ_DIR)/uname-uname.o \
  $(COREUTILS_OBJ_DIR)/set-fields.o $(COREUTILS_OBJ_DIR)/chown-core.o \
  $(COREUTILS_OBJ_DIR)/chown-chown.o $(COREUTILS_OBJ_DIR)/chown-chgrp.o

# The SDK Gnulib portability layer generates target headers (including fts_.h)
# into the build tree.  Every Coreutils translation unit consumes that single
# generated-header provider; no copy is kept in NexsOS1-main or the vendor tree.
$(COREUTILS_GNULIB_OBJS) $(COREUTILS_PROGRAM_OBJS): | $(GNULIB_GENERATED_HEADERS)

$(COREUTILS_GNULIB_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@

$(COREUTILS_GL_LIB): $(COREUTILS_GNULIB_OBJS)
	@echo "  [AR]     $@"
	@$(AR) rcs $@ $^




# ---------------------------------------------------------------------------
# Generate primes.h (required by factor.c) outside the external gitlink.
# The source submodule stays read-only; factor.c finds this build-owned header
# through COREUTILS_GENERATED_DIR before its generated dependency is compiled.
# Uses the HOST compiler (cc), not the cross-compiler.
# ---------------------------------------------------------------------------
$(COREUTILS_PRIMES_H): $(COREUTILS_SRC_DIR)/make-prime-list.c
	@echo "  [GEN]    $@"
	@set -eu; \
	  mkdir -p "$(COREUTILS_GENERATED_DIR)"; \
	  generator="$(BUILD_DIR)/make-prime-list"; \
	  output="$@.tmp"; \
	  trap 'rm -f "$$generator" "$$output"' EXIT; \
	  cc -O2 -o "$$generator" "$<"; \
	  "$$generator" 5000 > "$$output"; \
	  chmod a-w "$$output"; \
	  mv -f "$$output" "$@"

# Make factor.o depend on the build-owned generated header.
$(COREUTILS_OBJ_DIR)/factor.o: $(COREUTILS_PRIMES_H)


# Pattern rule for Coreutils port glue objects
$(COREUTILS_OBJ_DIR)/%.o: $(COREUTILS_PORT_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_PORT_CFLAGS) -MMD -MP -c $< -o $@


# Pattern rule for Coreutils upstream objects
$(COREUTILS_OBJ_DIR)/%.o: $(COREUTILS_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@


# Special multi-part / aliased applets
$(COREUTILS_OBJ_DIR)/true-true.o: $(COREUTILS_SRC_DIR)/true.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -DA_TRUE -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/true-mode-true.o:
	@mkdir -p $(dir $@)
	@echo "int true_mode = 0;" | $(CC) $(COREUTILS_PORT_CFLAGS) -x c -c - -o $@


$(COREUTILS_OBJ_DIR)/true-mode-false.o:
	@mkdir -p $(dir $@)
	@echo "int true_mode = 1;" | $(CC) $(COREUTILS_PORT_CFLAGS) -x c -c - -o $@


$(COREUTILS_OBJ_DIR)/uname-mode.o: $(COREUTILS_SRC_DIR)/uname.h
	@mkdir -p $(dir $@)
	@printf '#include "uname.h"\nenum uname_modes uname_mode = UNAME_UNAME;\n' | $(CC) $(COREUTILS_PORT_CFLAGS) -x c -c - -o $@


$(COREUTILS_OBJ_DIR)/uname-uname.o: $(COREUTILS_SRC_DIR)/uname.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@


$(BUILD_DIR)/cu_true.elf: $(COREUTILS_OBJ_DIR)/true.o $(COREUTILS_OBJ_DIR)/true-mode-true.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_false.elf: $(COREUTILS_OBJ_DIR)/true.o $(COREUTILS_OBJ_DIR)/true-mode-false.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_uname.elf: $(COREUTILS_OBJ_DIR)/uname.o $(COREUTILS_OBJ_DIR)/uname-mode.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(COREUTILS_OBJ_DIR)/set-fields.o: $(COREUTILS_SRC_DIR)/set-fields.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/chown-core.o: $(COREUTILS_SRC_DIR)/chown-core.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/chown-chown.o: $(COREUTILS_SRC_DIR)/chown-chown.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/chown-chgrp.o: $(COREUTILS_SRC_DIR)/chown-chgrp.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- $(CC) $(COREUTILS_VENDOR_CFLAGS) -MMD -MP -c $< -o $@


$(BUILD_DIR)/cu_chown.elf: $(COREUTILS_OBJ_DIR)/chown.o $(COREUTILS_OBJ_DIR)/chown-core.o $(COREUTILS_OBJ_DIR)/chown-chown.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_chgrp.elf: $(COREUTILS_OBJ_DIR)/chown.o $(COREUTILS_OBJ_DIR)/chown-core.o $(COREUTILS_OBJ_DIR)/chown-chgrp.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_cut.elf: $(COREUTILS_OBJ_DIR)/cut.o $(COREUTILS_OBJ_DIR)/set-fields.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_realpath.elf: $(COREUTILS_OBJ_DIR)/realpath.o $(COREUTILS_OBJ_DIR)/relpath.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_expand.elf: $(COREUTILS_OBJ_DIR)/expand.o $(COREUTILS_OBJ_DIR)/expand-common.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_unexpand.elf: $(COREUTILS_OBJ_DIR)/unexpand.o $(COREUTILS_OBJ_DIR)/expand-common.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


# Explicit rule generation for every single-file Coreutils applet
define CU_ELF_RULE
$$(BUILD_DIR)/cu_$(1).elf: $$(COREUTILS_OBJ_DIR)/$(1).o $$(COREUTILS_COMMON_OBJS) $$(COREUTILS_GL_LIB) $$(GNULIB_LIB) $$(USER_LIB_O) $$(USER_SYSCALL_O) $$(USER_MALLOC_O)
	@$$(CC) $$(CFLAGS) $$(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $$@ $$^ $$(USER_RUNTIME_LIBS)
endef


$(foreach app,$(filter-out true false uname chown chgrp cut realpath,$(COREUTILS_NAMES)),$(eval $(call CU_ELF_RULE,$(app))))


coreutils: $(COREUTILS_ELFS)
.PHONY: coreutils
