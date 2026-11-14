# NexsOS portability provider for the external Kilo editor.
#
# The imported BINEXTERNAL repository stays untouched. NexsOS hardening is
# applied to a build-tree copy through the patch stored in this portability
# layer, and the resulting executable is installed as a normal /bin program.
KILO_DIR := $(NEXS_BINEXTERNAL_DIR_kilo)
KILO_PORT_DIR := user/NexsOS1-main/portability/kilo
KILO_PATCH := $(KILO_PORT_DIR)/patches/kilo-hardening.patch
KILO_PATCHED_DIR := $(BUILD_DIR)/binexternal/kilo/src
KILO_PATCHED_SRC := $(KILO_PATCHED_DIR)/kilo.c
KILO_OBJ := $(BUILD_DIR)/binexternal/kilo/kilo.o

KILO_VENDOR_CFLAGS := $(ARCH_CFLAGS) -O2 -g -Wall -Wextra -Wpedantic \
  -Wconversion -Wsign-conversion \
  -ffreestanding -fno-builtin -nostdlib -nostartfiles -fno-common \
  -fstack-protector-strong -fno-pic -fno-pie -fno-omit-frame-pointer \
  -DOS1_EXTERNAL_COMPAT $(USER_INCLUDE)

$(KILO_PATCHED_SRC): $(KILO_DIR)/kilo.c $(KILO_PATCH)
	@mkdir -p $(KILO_PATCHED_DIR)
	@cp $(KILO_DIR)/kilo.c $@
	@patch -s -p1 -d $(KILO_PATCHED_DIR) < $(KILO_PATCH)

$(KILO_OBJ): $(KILO_PATCHED_SRC)
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- \
	  $(CC) $(KILO_VENDOR_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/kilo.elf: $(KILO_OBJ) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@tools/external-compile.sh "$@.external-warnings" -- \
	  $(CC) $(KILO_VENDOR_CFLAGS) $(USER_LINK_FLAGS) \
	  -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)
