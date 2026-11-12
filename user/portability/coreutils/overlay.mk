# user/portability/coreutils/overlay.mk
# Coreutils build integration for NexsOS1


COREUTILS_PORT_DIR := user/portability/coreutils
COREUTILS_DIR      := user/bin/coreutils
COREUTILS_SRC_DIR  := $(COREUTILS_DIR)/src


COREUTILS_CONFIG   := $(COREUTILS_PORT_DIR)/coreutils_config_nexsos.h
COREUTILS_CFLAGS   := $(ARCH_CFLAGS) -O2 -g -Wall -Wextra -Wpedantic \
                      -ffreestanding -fno-builtin -nostdlib -nostartfiles \
                      -fno-common -fstack-protector-strong -fno-pic -fno-pie \
                      -fno-omit-frame-pointer \
                      -Isystem/include/contract \
                      -Isystem/include/api \
                      -I$(COREUTILS_PORT_DIR) \
                      -I$(GNULIB_PORT_DIR) \
                      -I$(GNULIB_DIR)/lib \
                      -I$(COREUTILS_DIR)/src \
                      -I$(COREUTILS_DIR)/lib \
                      -I$(COREUTILS_DIR)/gl/lib \
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

$(COREUTILS_GNULIB_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@

$(COREUTILS_GL_LIB): $(COREUTILS_GNULIB_OBJS)
	@echo "  [AR]     $@"
	@$(AR) rcs $@ $^




# ---------------------------------------------------------------------------
# Generate primes.h (required by factor.c)
# Uses the HOST compiler (cc), not the cross-compiler.
# ---------------------------------------------------------------------------
$(COREUTILS_SRC_DIR)/primes.h: $(COREUTILS_SRC_DIR)/make-prime-list.c
	@echo "  [GEN]    $@"
	@set -eu; \
	  mkdir -p $(BUILD_DIR); \
	  generator="$(BUILD_DIR)/make-prime-list"; \
	  output="$@.tmp"; \
	  trap 'rm -f "$$generator" "$$output"' EXIT; \
	  cc -O2 -o "$$generator" "$<"; \
	  "$$generator" 5000 > "$$output"; \
	  chmod a-w "$$output"; \
	  mv -f "$$output" "$@"


# Make factor.o depend on the generated header
$(COREUTILS_OBJ_DIR)/factor.o: $(COREUTILS_SRC_DIR)/primes.h


# Pattern rule for Coreutils port glue objects
$(COREUTILS_OBJ_DIR)/%.o: $(COREUTILS_PORT_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


# Pattern rule for Coreutils upstream objects
$(COREUTILS_OBJ_DIR)/%.o: $(COREUTILS_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


# Special multi-part / aliased applets
$(COREUTILS_OBJ_DIR)/true-true.o: $(COREUTILS_SRC_DIR)/true.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -DA_TRUE -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/true-mode-true.o:
	@mkdir -p $(dir $@)
	@echo "int true_mode = 0;" | $(CC) $(COREUTILS_CFLAGS) -x c -c - -o $@


$(COREUTILS_OBJ_DIR)/true-mode-false.o:
	@mkdir -p $(dir $@)
	@echo "int true_mode = 1;" | $(CC) $(COREUTILS_CFLAGS) -x c -c - -o $@


$(COREUTILS_OBJ_DIR)/uname-mode.o: $(COREUTILS_SRC_DIR)/uname.h
	@mkdir -p $(dir $@)
	@printf '#include "uname.h"\nenum uname_modes uname_mode = UNAME_UNAME;\n' | $(CC) $(COREUTILS_CFLAGS) -x c -c - -o $@


$(COREUTILS_OBJ_DIR)/uname-uname.o: $(COREUTILS_SRC_DIR)/uname.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


$(BUILD_DIR)/cu_true.elf: $(COREUTILS_OBJ_DIR)/true.o $(COREUTILS_OBJ_DIR)/true-mode-true.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_false.elf: $(COREUTILS_OBJ_DIR)/true.o $(COREUTILS_OBJ_DIR)/true-mode-false.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(BUILD_DIR)/cu_uname.elf: $(COREUTILS_OBJ_DIR)/uname.o $(COREUTILS_OBJ_DIR)/uname-mode.o $(COREUTILS_COMMON_OBJS) $(COREUTILS_GL_LIB) $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


$(COREUTILS_OBJ_DIR)/set-fields.o: $(COREUTILS_SRC_DIR)/set-fields.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/chown-core.o: $(COREUTILS_SRC_DIR)/chown-core.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/chown-chown.o: $(COREUTILS_SRC_DIR)/chown-chown.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


$(COREUTILS_OBJ_DIR)/chown-chgrp.o: $(COREUTILS_SRC_DIR)/chown-chgrp.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@


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

# Preserve all warnings by default; escalate only on explicit qualification.
ifeq ($(NX_WARNINGS_AS_ERRORS),1)
COREUTILS_CFLAGS += -Werror
endif
