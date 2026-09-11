# user/sys/lib/portability/coreutils/overlay.mk
# Coreutils build integration for NexsOS1

COREUTILS_PORT_DIR := user/sys/lib/portability/coreutils
COREUTILS_DIR      := user/bin/coreutils
COREUTILS_SRC_DIR  := $(COREUTILS_DIR)/src

COREUTILS_CONFIG   := $(COREUTILS_PORT_DIR)/coreutils_config_nexsos.h
COREUTILS_CFLAGS   := $(ARCH_CFLAGS) -O2 -g -Wall \
                      -Wno-error \
                      -ffreestanding -fno-builtin -nostdlib -nostartfiles \
                      -fno-common -fstack-protector-strong -fno-pic -fno-pie \
                      -fno-omit-frame-pointer \
                      -Iinclude/abi \
                      -Iinclude/api \
                      -I$(COREUTILS_PORT_DIR) \
                      -I$(GNULIB_PORT_DIR) \
                      -I$(GNULIB_DIR)/lib \
                      -I$(COREUTILS_DIR)/src \
                      -I$(COREUTILS_DIR)/lib \
                      -I$(COREUTILS_DIR)/gl/lib \
                      -include $(COREUTILS_CONFIG)

COREUTILS_OBJ_DIR  := $(BUILD_DIR)/coreutils
COREUTILS_COMMON_OBJS := $(COREUTILS_OBJ_DIR)/version.o \
                          $(COREUTILS_OBJ_DIR)/coreutils_os1_glue.o

COREUTILS_NAMES := \
  echo true false pwd uname cat yes sleep mkdir rmdir unlink sync basename dirname whoami printenv \
  head tail wc tee touch chmod chown chgrp cut tr fold paste nl seq shuf nproc readlink realpath tty hostid logname users who \
  expr factor fmt link mknod nice nohup pathchk truncate tsort uniq uptime

COREUTILS_ELFS := $(patsubst %,$(BUILD_DIR)/cu_%.elf,$(COREUTILS_NAMES))

# ---------------------------------------------------------------------------
# Generate primes.h (required by factor.c).  Host compiler.
# ---------------------------------------------------------------------------
$(COREUTILS_SRC_DIR)/primes.h: $(COREUTILS_SRC_DIR)/make-prime-list.c
	@echo "  [GEN]    $@"
	@mkdir -p $(BUILD_DIR)
	@cc -O2 -o $(BUILD_DIR)/make-prime-list $<
	@$(BUILD_DIR)/make-prime-list 5000 > $@
	@rm -f $(BUILD_DIR)/make-prime-list
	@chmod a-w $@

$(COREUTILS_OBJ_DIR)/factor.o: $(COREUTILS_SRC_DIR)/primes.h

# ---------------------------------------------------------------------------
# Pattern rules: port glue objects and upstream coreutils objects.
# ---------------------------------------------------------------------------
$(COREUTILS_OBJ_DIR)/%.o: $(COREUTILS_PORT_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@

$(COREUTILS_OBJ_DIR)/%.o: $(COREUTILS_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(COREUTILS_CFLAGS) -MMD -MP -c $< -o $@

# ---------------------------------------------------------------------------
# Multi-part / aliased applets.
#
# `true` and `false` share one source (true.c).  Upstream chooses at
# runtime by argv[0]; on this tree, symlinks are not exercised, so the
# port links a small `int X_mode = N;` companion object and true.c reads
# it.  The same pattern applies to uname (uname vs arch).
#
# true.o (pattern rule) + true-mode-true.o  => cu_true
# true.o (pattern rule) + true-mode-false.o => cu_false
# ---------------------------------------------------------------------------
$(COREUTILS_OBJ_DIR)/true-mode-true.o:
	@mkdir -p $(dir $@)
	@echo "int true_mode = 0;" | $(CC) $(COREUTILS_CFLAGS) -x c -c - -o $@

$(COREUTILS_OBJ_DIR)/true-mode-false.o:
	@mkdir -p $(dir $@)
	@echo "int true_mode = 1;" | $(CC) $(COREUTILS_CFLAGS) -x c -c - -o $@

$(COREUTILS_OBJ_DIR)/uname-mode.o:
	@mkdir -p $(dir $@)
	@echo "int uname_mode = 0;" | $(CC) $(COREUTILS_CFLAGS) -x c -c - -o $@
	
# ---------------------------------------------------------------------------
# Link rules for cu_*.elf (the porting-layer wrap is scoped here, see
# USER_WRAP_FLAGS in the top-level Makefile).
# ---------------------------------------------------------------------------
$(BUILD_DIR)/cu_true.elf: $(COREUTILS_OBJ_DIR)/true.o \
                          $(COREUTILS_OBJ_DIR)/true-mode-true.o \
                          $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                          $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

$(BUILD_DIR)/cu_false.elf: $(COREUTILS_OBJ_DIR)/true.o \
                           $(COREUTILS_OBJ_DIR)/true-mode-false.o \
                           $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                           $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

$(BUILD_DIR)/cu_uname.elf: $(COREUTILS_OBJ_DIR)/uname.o \
                           $(COREUTILS_OBJ_DIR)/uname-mode.o \
                           $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                           $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# Multi-file applets.
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

$(BUILD_DIR)/cu_chown.elf: $(COREUTILS_OBJ_DIR)/chown.o \
                           $(COREUTILS_OBJ_DIR)/chown-core.o \
                           $(COREUTILS_OBJ_DIR)/chown-chown.o \
                           $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                           $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

$(BUILD_DIR)/cu_chgrp.elf: $(COREUTILS_OBJ_DIR)/chown.o \
                           $(COREUTILS_OBJ_DIR)/chown-core.o \
                           $(COREUTILS_OBJ_DIR)/chown-chgrp.o \
                           $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                           $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

$(BUILD_DIR)/cu_cut.elf: $(COREUTILS_OBJ_DIR)/cut.o \
                         $(COREUTILS_OBJ_DIR)/set-fields.o \
                         $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                         $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

$(BUILD_DIR)/cu_realpath.elf: $(COREUTILS_OBJ_DIR)/realpath.o \
                              $(COREUTILS_OBJ_DIR)/relpath.o \
                              $(COREUTILS_COMMON_OBJS) $(GNULIB_LIB) \
                              $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $@ $^

# ---------------------------------------------------------------------------
# Single-file applets: one rule per name, all using the wrapped flags.
# ---------------------------------------------------------------------------
define CU_ELF_RULE
$$(BUILD_DIR)/cu_$(1).elf: $$(COREUTILS_OBJ_DIR)/$(1).o $$(COREUTILS_COMMON_OBJS) $$(GNULIB_LIB) $$(USER_LIB_O) $$(USER_SYSCALL_O) $$(USER_MALLOC_O)
	@$$(CC) $$(CFLAGS) $$(USER_LINK_FLAGS_PORT) -Wl,-Ttext=0x80000000 -e _start -o $$@ $$^
endef

$(foreach app,$(filter-out true false uname chown chgrp cut realpath,$(COREUTILS_NAMES)),$(eval $(call CU_ELF_RULE,$(app))))

coreutils: $(COREUTILS_ELFS)
.PHONY: coreutils