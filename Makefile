# Makefile for NexsOS1 (AMD64 / AArch64)
.DEFAULT_GOAL := all
# Cross-compilation for bare-metal kernel
# ==============================================================================
# Configuration
# ==============================================================================

NEXSOS_SHARED_MAKE := 1
include system/config/toolchain-versions.env
HOST_OS ?= $(shell uname -s)
ifeq ($(HOST_OS),Linux)
    include Makefile.linux
else ifeq ($(HOST_OS),Darwin)
    include Makefile.macos
else
    $(error Unsupported host OS "$(HOST_OS)"; add a host compatibility file)
endif

# Target Architecture (default to aarch64)
ARCH ?= aarch64
HOST_CC ?= cc
# Build flavour for userland size control.
#   make BUILD=debug   → full symbols, no strip (default)
#   make BUILD=release → --gc-sections + strip
BUILD ?= debug
# Opt-in boot-time userland suite, separate from the normal debug symbols.
MDEBUG ?= 0

# Release Versioning (default V9.9.9)
VERSION ?= V0.0.6.4
RELEASE_BASE = release/$(VERSION)
RELEASE_DIR = $(RELEASE_BASE)/$(ARCH)



# Normalize ARCH typos
ifeq ($(findstring aarch64,$(ARCH)),aarch64)
    override ARCH := aarch64
else ifeq ($(findstring amd64,$(ARCH)),amd64)
    override ARCH := amd64
endif

# Common Compiler Flags
# ---------------------------------------------------------------------------
# CONTRACT GATE AND MAINTENANCE DIAGNOSTICS
#
# All ordinary first-party userland builds enable their warning diagnostics;
# warnings currently under correction remain visible rather than silently
# disabled. The independent NX-CORE and NX-HAL compiler profiles preserve
# fatal baseline diagnostics by default and require an explicit opt-out.
# Conversion diagnostics stay enabled in both domains; the separate
# NX_CONVERSION_AS_ERROR=1 profile makes their remaining debt build-blocking.
#
# No NX_DISCARD bypass is permitted. Fallible results require explicit handling.
# Power12 retains complete findings and source-digest evidence on every new
# audited input set. A cached audit is reused only for unchanged inputs.
# Release and check-strict retain the hard Power12 gate.
# ---------------------------------------------------------------------------

COMMON_FLAGS = -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
               -Wmissing-prototypes -Wstrict-prototypes \
               -Wconversion -Wsign-conversion \
               -ffreestanding -fno-builtin -nostdlib -nostartfiles \
               -fno-common -fstack-protector-strong \
               -fno-pic -fno-pie \
               -fno-omit-frame-pointer \
               -O2 -g

# First-party kernel, SDK and userland warnings are blocking by default.
# External vendor translation units use their dedicated non-blocking wrapper.
NX_WARNINGS_AS_ERRORS ?= 1
ifeq ($(NX_WARNINGS_AS_ERRORS),1)
COMMON_FLAGS += -Werror
endif

# Enforce the contract annotations on every ordinary build.  Both release
# architectures have been rebuilt successfully with NX_STRICT=1; keep the
# explicit 0 override only for temporary compiler/porting experiments.
NX_STRICT ?= 1
ifeq ($(NX_STRICT),1)
COMMON_FLAGS += -DNX_STRICT
endif

ifeq ($(ARCH), amd64)
    CROSS_COMPILE ?= $(AMD64_CROSS_COMPILE)
    ARCH_CFLAGS = -DARCH_AMD64 -mno-red-zone -mcmodel=large -Wa,--noexecstack
    ASFLAGS = -g $(if $(filter 1,$(NX_WARNINGS_AS_ERRORS)),--fatal-warnings)
    QEMU = qemu-system-x86_64
else
    CROSS_COMPILE ?= $(AARCH64_CROSS_COMPILE)
    ARCH_CFLAGS = -DARCH_AARCH64 -mcpu=cortex-a57 $(AARCH64_ARCH_CFLAGS_EXTRA)
    ASFLAGS = -mcpu=cortex-a57 -g $(if $(filter 1,$(NX_WARNINGS_AS_ERRORS)),--fatal-warnings)
    QEMU = qemu-system-aarch64
endif
KERNEL_DIR = kernel
NXSDK_DIR = system

# NX-SDK owns the public API and its versioned kernel-contract snapshot.
# The legacy shared root include/abi directory is intentionally absent.
USER_INCLUDE = -I$(NXSDK_DIR)/include/contract -I$(NXSDK_DIR)/include/api $(NXSDK_EXTENSION_INCLUDE_FLAGS) $(NXSDK_USERKIT_INCLUDE_FLAGS) $(USER_PROJECT_LIB_INCLUDE_FLAGS) -isystem $(NXSDK_DIR)/external/stb -isystem $(NEXS_LIBEXTERNAL_DIR_stb) -isystem $(NEXS_LIBEXTERNAL_DIR_pl_mpeg)
# Compatibility variable consumed by pinned upstream build fragments (e.g. DOOM).
INCLUDE = $(USER_INCLUDE)

# Userland: enable per-function/data sections so the linker can GC unused
# code from the whole-archive runtime objects (USR-BLOAT-01/02).
USER_CFLAGS = $(COMMON_FLAGS) $(ARCH_CFLAGS) $(USER_INCLUDE) -ffunction-sections -fdata-sections
# CFLAGS is the userland-facing convention used by upstream build fragments.
CFLAGS = $(USER_CFLAGS)

# Tools
CC      = $(CROSS_COMPILE)gcc
AS      = $(CROSS_COMPILE)as
LD      = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
NM = $(CROSS_COMPILE)nm
AR = $(CROSS_COMPILE)ar
MKDISK  = tools/mkdisk
# Rootfs inode layout: --extents (mkfs.ext4-like, default) or --legacy
# (direct/indirect pointer blocks, the pre-B1 format kept for regression).
MKDISK_LAYOUT ?= --extents

# Directories
BUILD_ROOT = build
BUILD_DIR  = $(BUILD_ROOT)/$(ARCH)

# Consume the independently buildable NX-SDK manifest.  The object namespace
# remains build/<arch>/system so extraction does not alter link inputs.
NXSDK_ROOT := $(NXSDK_DIR)
include $(NXSDK_DIR)/sdk.mk

# Optional GRUB ISO media for BOOT_MODE=iso and nxadb/nxrun. AArch64 EFI
# embeds kernel Image + generated DTB; AMD64 boots the Multiboot2 ELF.
HOST_CLANG ?= $(shell command -v clang 2>/dev/null)
LLD_LINK ?= $(shell command -v lld-link 2>/dev/null || { for p in /usr/local/opt/llvm/bin/lld-link /opt/homebrew/opt/llvm/bin/lld-link; do if test -x "$$p"; then echo "$$p"; exit 0; fi; done; if command -v brew >/dev/null 2>&1; then p="$$(brew --prefix llvm 2>/dev/null)/bin/lld-link"; test -x "$$p" && echo "$$p"; fi; })
AMD64_GRUB_ISO = $(BUILD_DIR)/grub-bios.iso
AMD64_UEFI_ISO = $(BUILD_DIR)/grub-uefi/nexsos-amd64-uefi.iso
AARCH64_UEFI_ISO = $(BUILD_DIR)/grub-efi/nexsos-aarch64-uefi.iso
ifeq ($(ARCH),aarch64)
BOOT_ISO = $(AARCH64_UEFI_ISO)
else
BOOT_ISO = $(AMD64_GRUB_ISO)
endif
USER_PROJECT ?= NexsOS1-main
USER_DIR := user/$(USER_PROJECT)
USER_SYS_DIR := $(USER_DIR)/sys
USERKIT_DIR := user/USERKIT
LIBEXTERNAL_DIR := user/LIBEXTERNAL
include $(LIBEXTERNAL_DIR)/catalog.mk
BINEXTERNAL_DIR := user/BINEXTERNAL
include $(BINEXTERNAL_DIR)/catalog.mk
USERKIT_BUILD_ROOT := $(abspath $(BUILD_ROOT))/userkit
USERKIT_BUILD_DIR := $(USERKIT_BUILD_ROOT)/$(ARCH)/userkit
include $(USERKIT_DIR)/catalog.mk
USER_LIB_DIR = $(NXSDK_DIR)/lib
USER_PROJECT_LIB_DIR := $(USER_DIR)/lib
include $(USER_PROJECT_LIB_DIR)/catalog.mk
USER_PROJECT_LIB_BUILD_ROOT := $(abspath $(BUILD_ROOT))/project-lib
USER_PROJECT_LIB_BUILD_DIR := $(USER_PROJECT_LIB_BUILD_ROOT)/$(ARCH)/nexsos1-lib
USER_PROJECT_LIB_ARCHIVE := $(USER_PROJECT_LIB_BUILD_DIR)/$(NEXSOS1_LIB_LINK_ARCHIVE)
USER_PROJECT_LIB_GLOBAL_FILES := $(addprefix $(USER_PROJECT_LIB_BUILD_DIR)/,$(NEXSOS1_LIB_GLOBAL_ARCHIVES))
USER_PROJECT_LIB_INCLUDE_FLAGS := $(addprefix -I$(USER_PROJECT_LIB_DIR)/,$(NEXSOS1_LIB_INCLUDE_REL))
USER_BIN_DIR = $(USER_SYS_DIR)/bin
USER_ARCH_DIR = $(NXSDK_DIR)/arch/$(ARCH)

# Output files
BOOTLOADER_ELF = $(BUILD_DIR)/bootloader.elf
BOOTLOADER_BIN = $(BUILD_DIR)/bootloader.bin
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
USER_ELF   = $(BUILD_DIR)/nxinit.elf
DISK_IMG   = $(BUILD_DIR)/disk.img
USER_IMAGE_DIR := $(BUILD_DIR)/user-images
MAIN_USER_IMAGE := $(USER_IMAGE_DIR)/NexsOS1-main.img
DEMO1_USER_IMAGE := $(USER_IMAGE_DIR)/demo1.img
DEMO1_BUILD_ROOT := $(abspath $(BUILD_ROOT))/demo1
DEMO1_ELF := $(DEMO1_BUILD_ROOT)/$(ARCH)/demo1/demo1shell.elf
DEMO1_ROOTFS := $(BUILD_DIR)/rootfs-demo1
DEMO1_ROOTFS_STAMP := $(BUILD_DIR)/rootfs-demo1-ready.stamp
# AArch64 only: pre-generated QEMU virt DTB used to pass a valid x0 to the kernel.
# QEMU does not set x0 for bare-metal ELF kernels (no Linux image magic), so we
# generate the DTB for the exact machine config and pass it via -dtb.  QEMU then
# loads it into RAM and sets x0 = dtb_load_address in the aarch64 boot stub.
VIRT_DTB   = $(BUILD_DIR)/virt.dtb

# ==============================================================================
# Source Files
# ==============================================================================

# ==============================================================================

.PHONY: all clean run run-direct debug disasm check check-deps check-external-catalog setup-deps dirs bootloader kernel sdk user disk rootfs release test-release help
# 'make debug' is the explicit MDEBUG boot profile; normal 'run' stays quiet.
debug: MDEBUG=1

# Bootstrap exact third-party revisions recorded as Git submodule gitlinks.
setup-deps:
	@tools/setup-dependencies.sh --init

check-deps:
	@tools/setup-dependencies.sh --check

check-external-catalog:
	@set -eu; \
	$(foreach p,$(NEXS_BINEXTERNAL_PROJECTS),test -d "$(NEXS_BINEXTERNAL_DIR_$(p))" || { echo "[catalog] missing BINEXTERNAL project: $(p)" >&2; exit 1; }; test "$(NEXS_BINEXTERNAL_VFS_DIR_$(p))" = "/bin" || { echo "[catalog] $(p) must install under /bin" >&2; exit 1; };) \
	$(foreach p,$(NEXS_BINEXTERNAL_IMAGE_PROJECTS),test "$(NEXS_BINEXTERNAL_COMPAT_$(p))" = "integrated" || { echo "[catalog] $(p) is selected for the image but not integrated" >&2; exit 1; }; test -f "$(NEXS_BINEXTERNAL_PROVIDER_$(p))" || { echo "[catalog] missing provider for $(p)" >&2; exit 1; }; test -n "$(NEXS_BINEXTERNAL_INTERNAL_LIBS_$(p))" || { echo "[catalog] $(p) has no internal-library declaration" >&2; exit 1; };) \
	$(foreach p,$(NEXS_BINEXTERNAL_IMAGE_PROJECTS),$(foreach l,$(filter-out none,$(NEXS_BINEXTERNAL_EXTERNAL_LIBS_$(p))),$(if $(filter $(l),$(NEXS_LIBEXTERNAL_PROJECTS)),,echo "[catalog] $(p) references unknown external library $(l)" >&2; exit 1;))) \
	true
	@echo "[catalog] BINEXTERNAL providers and library dependencies: PASS"

# Default target
all: power12 dirs bootloader kernel user $(MKDISK) disk
	@echo ""
	@echo "Build complete for $(ARCH)!"
	@echo "  Disk Image: $(DISK_IMG)"
ifeq ($(ARCH), amd64)
	@echo "  Kernel:     $(KERNEL_ELF)"
else
	@echo "  Kernel:     $(KERNEL_BIN) (AArch64 Raw Binary)"
endif

# Create build directories
dirs:
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/lib
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/sys/lib
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/sys/bin
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/sbin
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/bin
	@mkdir -p $(BUILD_DIR)/$(USER_ARCH_DIR)
	@mkdir -p $(BUILD_DIR)/lua
	@mkdir -p $(BUILD_DIR)/$(USER_DIR)/sys/lib/portability/lua

# NX-CORE owns the kernel and bootloader build. The superproject passes only
# the shared output root and target architecture, then consumes the artifacts.
.PHONY: kernel bootloader
kernel: power12
	+$(MAKE) -C $(KERNEL_DIR) BUILD_ROOT="$(abspath $(BUILD_ROOT))" ARCH=$(ARCH) MDEBUG=$(MDEBUG) kernel

bootloader: power12
	+$(MAKE) -C $(KERNEL_DIR) BUILD_ROOT="$(abspath $(BUILD_ROOT))" ARCH=$(ARCH) bootloader

$(KERNEL_BIN) $(KERNEL_ELF): | kernel
	@test -f "$@" || { echo "NX-CORE did not produce $@" >&2; exit 1; }

$(BOOTLOADER_BIN) $(BOOTLOADER_ELF): | bootloader
	@test -f "$@" || { echo "NX-CORE did not produce $@" >&2; exit 1; }

# NX-SDK builds its own complete object set. The integration layer supplies
# the two external media-header locations explicitly; the SDK never reaches
# into ../user by convention or hidden relative paths.
sdk:
	+$(MAKE) -C $(NXSDK_DIR) BUILD_ROOT="$(abspath $(BUILD_ROOT))" ARCH=$(ARCH) \
		NX_WARNINGS_AS_ERRORS=$(NX_WARNINGS_AS_ERRORS) NX_STRICT=$(NX_STRICT) \
		STB_INCLUDE="$(abspath $(NEXS_LIBEXTERNAL_DIR_stb))" \
		PL_MPEG_INCLUDE="$(abspath $(NEXS_LIBEXTERNAL_DIR_pl_mpeg))" module

$(NXSDK_MODULE_OBJECTS): | sdk
	@test -f "$@" || { echo "NX-SDK did not produce $@" >&2; exit 1; }

# Userland consumes the NX-SDK artifact manifest instead of duplicating its
# source/object list in the superproject.
USER_SYSCALL_O := $(NXSDK_SYSCALL_OBJECT)
USER_LIB_O := $(NXSDK_LIB_OBJECTS)
USER_MALLOC_O := $(NXSDK_MALLOC_OBJECT)

# ==============================================================================
# SDL2 static library (graphics-port: SDL cross-build over the OS1 libc)
# ==============================================================================
# The SDL submodule compiles UNPATCHED: SDL_config_nexsos.h is force-included
# (-include) so the tree's own SDL_config.h is skipped, and the DUMMY
# bootstrap slot is renamed to the NexsOS driver from the portability overlay
# (see user/sys/lib/portability/sdl2/overlay.mk).  Defined before the user-ELF
# link rules because prerequisite lists expand immediately.
AR = $(CROSS_COMPILE)ar

SDL2_DIR := $(NEXS_LIBEXTERNAL_DIR_sdl)
include system/lib/portability/sdl2/overlay.mk

SDL2_SRCS := \
  $(wildcard $(SDL2_DIR)/src/*.c) \
  $(wildcard $(SDL2_DIR)/src/atomic/*.c) \
  $(wildcard $(SDL2_DIR)/src/audio/*.c) \
  $(wildcard $(SDL2_DIR)/src/audio/dummy/*.c) \
  $(wildcard $(SDL2_DIR)/src/cpuinfo/*.c) \
  $(wildcard $(SDL2_DIR)/src/dynapi/*.c) \
  $(wildcard $(SDL2_DIR)/src/events/*.c) \
  $(wildcard $(SDL2_DIR)/src/file/*.c) \
  $(wildcard $(SDL2_DIR)/src/filesystem/dummy/*.c) \
  $(wildcard $(SDL2_DIR)/src/haptic/*.c) \
  $(wildcard $(SDL2_DIR)/src/joystick/*.c) \
  $(wildcard $(SDL2_DIR)/src/libm/*.c) \
  $(wildcard $(SDL2_DIR)/src/locale/*.c) \
  $(wildcard $(SDL2_DIR)/src/locale/dummy/*.c) \
  $(wildcard $(SDL2_DIR)/src/misc/*.c) \
  $(wildcard $(SDL2_DIR)/src/misc/dummy/*.c) \
  $(wildcard $(SDL2_DIR)/src/power/*.c) \
  $(wildcard $(SDL2_DIR)/src/render/*.c) \
  $(wildcard $(SDL2_DIR)/src/render/software/*.c) \
  $(wildcard $(SDL2_DIR)/src/sensor/*.c) \
  $(wildcard $(SDL2_DIR)/src/sensor/dummy/*.c) \
  $(wildcard $(SDL2_DIR)/src/stdlib/*.c) \
  $(wildcard $(SDL2_DIR)/src/thread/*.c) \
  $(wildcard $(SDL2_DIR)/src/thread/generic/*.c) \
  $(wildcard $(SDL2_DIR)/src/timer/*.c) \
  $(wildcard $(SDL2_DIR)/src/timer/dummy/*.c) \
  $(wildcard $(SDL2_DIR)/src/video/*.c) \
  $(wildcard $(SDL2_DIR)/src/video/yuv2rgb/*.c) \
  $(SDL_NEXSOS_OVERLAY_SRCS)

SDL2_OBJ_DIR := $(BUILD_DIR)/sdl2
SDL2_OBJS := $(patsubst %.c,$(SDL2_OBJ_DIR)/%.o,$(SDL2_SRCS))
SDL2_LIB  := $(BUILD_DIR)/libSDL2.a

# Upstream SDL is external: warnings remain enabled but do not inherit
# first-party Werror. NexsOS-owned SDL portability sources use USER_CFLAGS and
# therefore remain strict. Successful vendor diagnostics are captured per
# object and summarized once after the complete user module is built.
SDL2_VENDOR_CFLAGS = $(ARCH_CFLAGS) -O2 -g -Wall -Wextra \
              -ffreestanding -fno-builtin -nostdlib -nostartfiles \
              -fno-common -fstack-protector-strong -fno-pic -fno-pie \
              -fno-omit-frame-pointer -DSDL_NEXSOS_VENDOR \
              $(USER_INCLUDE) -isystem $(SDL2_DIR)/include \
              $(SDL_NEXSOS_OVERLAY_CPPFLAGS)
SDL2_PORT_CFLAGS = $(USER_CFLAGS) -isystem $(SDL2_DIR)/include \
              $(SDL_NEXSOS_OVERLAY_CPPFLAGS)

$(SDL2_OBJ_DIR)/system/lib/portability/sdl2/%.o: system/lib/portability/sdl2/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(SDL2_PORT_CFLAGS) -c $< -o $@

$(SDL2_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- \
	  $(CC) $(SDL2_VENDOR_CFLAGS) -c $< -o $@

$(SDL2_LIB): $(SDL2_OBJS)
	@echo "  [AR]     $@"
	@$(AR) rcs $@ $^

libsdl2: $(SDL2_LIB)
.PHONY: libsdl2

# ==============================================================================
# Lua static library and runtime (lua)
# ==============================================================================
LUA_DIR := $(NEXS_LIBEXTERNAL_DIR_lua)

LUA_CORE_SRCS := \
  $(LUA_DIR)/lapi.c \
  $(LUA_DIR)/lauxlib.c \
  $(LUA_DIR)/lbaselib.c \
  $(LUA_DIR)/lcode.c \
  $(LUA_DIR)/lcorolib.c \
  $(LUA_DIR)/lctype.c \
  $(LUA_DIR)/ldblib.c \
  $(LUA_DIR)/ldebug.c \
  $(LUA_DIR)/ldo.c \
  $(LUA_DIR)/ldump.c \
  $(LUA_DIR)/lfunc.c \
  $(LUA_DIR)/lgc.c \
  $(LUA_DIR)/linit.c \
  $(LUA_DIR)/liolib.c \
  $(LUA_DIR)/llex.c \
  $(LUA_DIR)/lmathlib.c \
  $(LUA_DIR)/lmem.c \
  $(LUA_DIR)/loadlib.c \
  $(LUA_DIR)/lobject.c \
  $(LUA_DIR)/lopcodes.c \
  $(LUA_DIR)/loslib.c \
  $(LUA_DIR)/lparser.c \
  $(LUA_DIR)/lstate.c \
  $(LUA_DIR)/lstring.c \
  $(LUA_DIR)/lstrlib.c \
  $(LUA_DIR)/ltable.c \
  $(LUA_DIR)/ltablib.c \
  $(LUA_DIR)/ltm.c \
  $(LUA_DIR)/lundump.c \
  $(LUA_DIR)/lutf8lib.c \
  $(LUA_DIR)/lvm.c \
  $(LUA_DIR)/lzio.c

LUA_PORT_SRCS := $(USER_LIB_DIR)/portability/lua/lua_portability.c
LUA_OS1_SRCS  := $(USER_LIB_DIR)/os1LUA_lib.c
# lua_portability.h is FORCE-INCLUDED into every Lua translation unit
# (-include, LUA_CFLAGS below), so it is a real prerequisite of all of them.
# Without it listed here, editing that header silently leaves STALE objects:
# the .c files it overrides (lua.c's lua_stdin_is_tty / lua_readline) keep the
# upstream fallbacks while lua_portability.o alone rebuilds — which is exactly
# how the "echo ... | lua still prints the REPL banner" bug survived a rebuild.
LUA_PORT_HDR  := $(USER_LIB_DIR)/portability/lua/lua_portability.h

LUA_OBJ_DIR := $(BUILD_DIR)/lua
LUA_CORE_OBJS := $(patsubst $(LUA_DIR)/%.c,$(LUA_OBJ_DIR)/%.o,$(LUA_CORE_SRCS))
LUA_PORT_OBJ  := $(BUILD_DIR)/$(USER_LIB_DIR)/portability/lua/lua_portability.o
LUA_OS1_OBJ   := $(BUILD_DIR)/$(USER_LIB_DIR)/os1LUA_lib.o

LUA_LIB     := $(BUILD_DIR)/lua.a
LUA_OS1_LIB := $(BUILD_DIR)/libos1lua.a

# The Lua module search path (LUA_ROOT/LDIR/CDIR/LUA_PATH_DEFAULT) is owned
# SOLELY by the force-included lua_portability.h — a C header, so the ';'
# path separators are inside a real string literal.  It must NOT be passed
# as -DLUA_PATH_DEFAULT=\"...;...\" here: Make forwards the \" verbatim, the
# shell reads it as a literal quote (not a delimiter), and the unquoted ';'
# then split the gcc line into pieces ("no input files" + bogus commands).
LUA_VENDOR_CFLAGS = $(ARCH_CFLAGS) -O2 -g -Wall -Wextra \
             -ffreestanding -fno-builtin -nostdlib -nostartfiles \
             -fno-common -fstack-protector-strong -fno-pic -fno-pie \
             -fno-omit-frame-pointer -DLUA_USE_NEXSOS -DLUA_NEXSOS_VENDOR \
             $(USER_INCLUDE) -I$(LUA_DIR) \
             -I$(USER_LIB_DIR)/portability/lua -include lua_portability.h
LUA_PORT_CFLAGS = $(USER_CFLAGS) -DLUA_USE_NEXSOS \
             -I$(LUA_DIR) -I$(USER_LIB_DIR)/portability/lua \
             -include lua_portability.h

$(LUA_OBJ_DIR)/%.o: $(LUA_DIR)/%.c $(LUA_PORT_HDR)
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- \
	  $(CC) $(LUA_VENDOR_CFLAGS) -c $< -o $@

$(LUA_PORT_OBJ): $(LUA_PORT_SRCS) $(LUA_PORT_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(LUA_PORT_CFLAGS) -c $< -o $@

$(LUA_OS1_OBJ): $(LUA_OS1_SRCS) $(LUA_PORT_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(LUA_PORT_CFLAGS) -c $< -o $@

$(LUA_LIB): $(LUA_CORE_OBJS) $(LUA_PORT_OBJ)
	@echo "  [AR]     $@"
	@$(AR) rcs $@ $^

$(LUA_OS1_LIB): $(LUA_OS1_OBJ)
	@echo "  [AR]     $@"
	@$(AR) rcs $@ $^

liblua: $(LUA_LIB) $(LUA_OS1_LIB)
.PHONY: liblua

$(LUA_OBJ_DIR)/lua.o: $(LUA_DIR)/lua.c $(LUA_PORT_HDR)
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- \
	  $(CC) $(LUA_VENDOR_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lua.elf: $(LUA_OBJ_DIR)/lua.o $(LUA_OS1_LIB) $(LUA_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ \
		$(LUA_OBJ_DIR)/lua.o \
		-Wl,--start-group $(LUA_LIB) $(LUA_OS1_LIB) -Wl,--end-group \
		$(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O) $(USER_RUNTIME_LIBS)

# ==============================================================================
# Gnulib static library (libgnulib)
# ==============================================================================
# NX-SDK owns the portability implementation; the superproject supplies only
# the pinned upstream source location from the LIBEXTERNAL catalog.
GNULIB_SOURCE := $(NEXS_LIBEXTERNAL_DIR_gnulib)
include $(NEXS_LIBEXTERNAL_PROVIDER_gnulib)

GNULIB_OBJ_DIR := $(BUILD_DIR)/gnulib
GNULIB_OBJS := $(patsubst %.c,$(GNULIB_OBJ_DIR)/%.o,$(GNULIB_OVERLAY_SRCS))
GNULIB_LIB  := $(BUILD_DIR)/libgnulib.a

$(GNULIB_OBJS): $(GNULIB_GENERATED_HEADERS)

# Upstream Gnulib sources keep their vendor warning policy. NexsOS
# portability sources are first-party and compile under USER_CFLAGS/Werror.
GNULIB_VENDOR_CFLAGS = $(ARCH_CFLAGS) -O2 -g -Wall -Wextra \
                -ffreestanding -fno-builtin -nostdlib -nostartfiles \
                -fno-common -fstack-protector-strong -fno-pic -fno-pie \
                -fno-omit-frame-pointer -DNEXSOS_EXTERNAL_VENDOR \
                $(USER_INCLUDE) \
                $(GNULIB_OVERLAY_CPPFLAGS)
GNULIB_PORT_CFLAGS = $(USER_CFLAGS) $(GNULIB_OVERLAY_CPPFLAGS)

$(GNULIB_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@tools/external-compile.sh "$@.external-warnings" -- \
	  $(CC) $(GNULIB_VENDOR_CFLAGS) -c $< -o $@

$(GNULIB_OBJ_DIR)/system/lib/portability/gnulib/%.o: system/lib/portability/gnulib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(GNULIB_PORT_CFLAGS) -c $< -o $@

# glue.c must define _GNULIB_OS1_GLUE_IMPL before the config header.
$(GNULIB_OBJ_DIR)/system/lib/portability/gnulib/gnulib_os1_glue.o: system/lib/portability/gnulib/gnulib_os1_glue.c
	@mkdir -p $(dir $@)
	@$(CC) $(GNULIB_PORT_CFLAGS) -D_GNULIB_OS1_GLUE_IMPL -c $< -o $@

$(GNULIB_LIB): $(GNULIB_OBJS) $(NEXS_LIBEXTERNAL_PROVIDER_gnulib) Makefile
	@echo "  [AR]     $@"
	@rm -f $@.tmp
	@$(AR) rcs $@.tmp $(GNULIB_OBJS)
	@mv -f $@.tmp $@

libgnulib: $(GNULIB_LIB)
.PHONY: libgnulib

# ==============================================================================
# Coreutils compatibility overlay
# ==============================================================================
include $(NEXS_BINEXTERNAL_PROVIDER_coreutils)
include $(NEXS_BINEXTERNAL_PROVIDER_kilo)




# The NexsOS userland owns the canonical first-party manifest.  The integration
# repository supplies SDK object paths and adds external applications below.
OBJ_DIR := $(BUILD_DIR)/$(USER_DIR)
NEXS_USERLAND_SDK_LINK_INPUTS := $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O) $(USER_PROJECT_LIB_ARCHIVE)
NEXS_USERLAND_SDK_SIMD_OBJECT := $(BUILD_DIR)/$(USER_ARCH_DIR)/simd_probe.o
include $(USER_DIR)/userland.mk

NX_MACHINE_EXTENSION_ELFS := $(BUILD_DIR)/nxmanageDD.elf
SYS_ELFS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(NEXS_USERLAND_SYS_PROGRAMS) $(NEXS_USERLAND_COMPLEX_PROGRAMS) $(NEXS_USERLAND_MEDIA_PROGRAMS) $(NEXS_USERLAND_MEDIA_COMPLEX_PROGRAMS)))
NEXS_USERLAND_BIN_ELFS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(NEXS_USERLAND_BIN_PROGRAMS) $(NEXS_USERLAND_ALIAS_PROGRAMS)))
EXTERNAL_BIN_ELFS := $(BUILD_DIR)/sdltest.elf $(BUILD_DIR)/raptor.elf \
  $(BUILD_DIR)/lua.elf $(BUILD_DIR)/nxempire.elf $(BUILD_DIR)/doom.elf \
  $(BUILD_DIR)/gnulibtest.elf $(BUILD_DIR)/luatest.elf $(BUILD_DIR)/kilo.elf \
  $(COREUTILS_ELFS)
BIN_ELFS := $(NEXS_USERLAND_BIN_ELFS) $(EXTERNAL_BIN_ELFS)
USER_ELFS := $(NX_MACHINE_EXTENSION_ELFS) $(SYS_ELFS) $(BIN_ELFS)
USERKIT_MAIN_INIT_ELF := $(USERKIT_BUILD_DIR)/sbin/nxinit.elf
USERKIT_DEMO_INIT_ELF := $(USERKIT_BUILD_DIR)/sbin/nxdemo1init.elf
USERKIT_SBIN_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/sbin/,$(addsuffix .elf,$(NEXS_USERKIT_SBIN_PROJECTS)))
USERKIT_SERVICE_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/services/,$(addsuffix .elf,$(NEXS_USERKIT_SERVICE_PROJECTS)))
USERKIT_EXAMPLE_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/services/,$(addsuffix .elf,$(NEXS_USERKIT_EXAMPLE_PROJECTS)))
USERKIT_RUNTIME_TEST_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/tests/test_,$(addsuffix .elf,$(NEXS_USERKIT_SERVICE_PROJECTS)))
USERKIT_EXAMPLE_TEST_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/tests/test_,$(addsuffix .elf,$(NEXS_USERKIT_EXAMPLE_PROJECTS)))
DEMO1_USERKIT_SERVICE_PROJECTS := nxnotify_registry $(filter-out nxntfy_srv,$(NEXS_USERKIT_SERVICE_PROJECTS)) nxexample_echo
DEMO1_USERKIT_SERVICE_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/services/,$(addsuffix .elf,$(DEMO1_USERKIT_SERVICE_PROJECTS)))
DEMO1_USERKIT_TEST_ELFS := $(addprefix $(USERKIT_BUILD_DIR)/tests/test_,$(addsuffix .elf,$(DEMO1_USERKIT_SERVICE_PROJECTS)))
USERKIT_ELFS := $(USERKIT_SBIN_ELFS) $(USERKIT_SERVICE_ELFS) $(USERKIT_EXAMPLE_ELFS) $(USERKIT_RUNTIME_TEST_ELFS) $(USERKIT_EXAMPLE_TEST_ELFS)

.PHONY: userkit project-libs
project-libs: sdk
	+$(MAKE) -C "$(USER_PROJECT_LIB_DIR)" ARCH=$(ARCH) BUILD_ROOT="$(USER_PROJECT_LIB_BUILD_ROOT)" SDK_ROOT="$(abspath $(NXSDK_DIR))" check
	@test -s "$(USER_PROJECT_LIB_ARCHIVE)"

$(USER_PROJECT_LIB_ARCHIVE) $(USER_PROJECT_LIB_GLOBAL_FILES): | project-libs
	@test -s "$@"

userkit: sdk project-libs
	+$(MAKE) -C $(USERKIT_DIR) ARCH=$(ARCH) BUILD_ROOT="$(USERKIT_BUILD_ROOT)" SDK_ROOT="$(abspath $(NXSDK_DIR))" \
	  $(foreach provider,$(NEXSOS1_LIB_PROVIDER_KEYS),USERKIT_GLOBAL_LIBRARY_$(provider)="$(USER_PROJECT_LIB_BUILD_DIR)/$(NEXSOS1_LIB_PROVIDER_ARCHIVE_$(provider))") \
	  check

$(USERKIT_ELFS): | userkit
	@test -f "$@" || { echo "NX-USERKIT did not produce $@" >&2; exit 1; }


user: power12 sdk userkit __nexsos_user_module
.PHONY: __nexsos_user_module __user
__nexsos_user_module: $(USER_ELFS)
	@python3 tools/summarize_external_warnings.py "$(BUILD_DIR)"
__user: $(USER_ELFS)
	@python3 tools/summarize_external_warnings.py "$(BUILD_DIR)"

# NX-CORE owns and builds the MACHINE user-mode extension.  The root target
# must carry the source/protocol prerequisites too: otherwise an existing ELF
# suppresses the kernel sub-make and bypasses its generated dependency files.
$(BUILD_DIR)/nxmanageDD.elf: $(KERNEL_DIR)/user/extension/nxmanageDD/main.c \
                             $(NXSDK_DIR)/lib/extensions/USERKIT/manager/include/nxmanage.h
	+$(MAKE) -C $(KERNEL_DIR) BUILD_ROOT="$(abspath $(BUILD_ROOT))" ARCH=$(ARCH) \
	  NXSDK_ROOT="$(abspath $(NXSDK_DIR))" machine-extension
	@test -f "$@" || { echo "NX-CORE did not produce $@" >&2; exit 1; }

# User Compilations
$(BUILD_DIR)/$(USER_DIR)/lib/%.o: $(USER_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/sys/lib/%.o: $(USER_DIR)/sys/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/bin/%.o: $(USER_DIR)/bin/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/$(USER_DIR)/sys/bin/%.o: $(USER_DIR)/sys/bin/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

# sdltest: first SDL2 client; the object needs the SDL headers + injected
# NexsOS config, the ELF links the cross-built archive before the OS1 libc
# objects (which satisfy the archive's malloc/os1_video_* undefineds).
$(BUILD_DIR)/$(USER_DIR)/bin/sdltest.o: $(USER_DIR)/bin/sdltest.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -isystem $(SDL2_DIR)/include $(SDL_NEXSOS_OVERLAY_CPPFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/sdltest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/sdltest.o $(SDL2_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/$(USER_DIR)/bin/raptor.o: $(USER_DIR)/bin/raptor.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -isystem $(SDL2_DIR)/include $(SDL_NEXSOS_OVERLAY_CPPFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/raptor.elf: $(BUILD_DIR)/$(USER_DIR)/bin/raptor.o $(SDL2_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/$(USER_DIR)/bin/nxempire.o: $(USER_DIR)/bin/nxempire.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -isystem $(SDL2_DIR)/include $(SDL_NEXSOS_OVERLAY_CPPFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/nxempire.elf: $(BUILD_DIR)/$(USER_DIR)/bin/nxempire.o $(SDL2_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/$(USER_DIR)/bin/gnulibtest.o: $(USER_DIR)/bin/gnulibtest.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -I$(GNULIB_PORT_DIR) -isystem $(GNULIB_DIR)/lib $(GNULIB_OVERLAY_CPPFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/gnulibtest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/gnulibtest.o $(GNULIB_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)

$(BUILD_DIR)/$(USER_DIR)/bin/luatest.o: $(USER_DIR)/bin/luatest.c $(LUA_PORT_HDR)
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -isystem $(LUA_DIR) -I$(USER_LIB_DIR)/portability/lua -include lua_portability.h -MMD -MP -c $< -o $@

$(BUILD_DIR)/luatest.elf: $(BUILD_DIR)/$(USER_DIR)/bin/luatest.o $(LUA_OS1_LIB) $(LUA_LIB) $(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O)
	@echo "  [LD]     $@"
	@$(CC) $(CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ \
		$< \
		-Wl,--start-group $(LUA_LIB) $(LUA_OS1_LIB) -Wl,--end-group \
		$(USER_LIB_O) $(USER_SYSCALL_O) $(USER_MALLOC_O) $(USER_RUNTIME_LIBS)

# Userland link flags.
# --gc-sections drops unused functions/data from the whole-linked lib.o
# (the main source of the ~50-70 KB bloat measured in counter.elf).
# In release builds we also strip symbols; debug keeps everything.
USER_LINK_FLAGS ?= -Wl,--gc-sections
# -nostdlib also omits the compiler ABI helpers. Resolve only needed members
# of the matching cross compiler's runtime, after all user objects/archives.
USER_RUNTIME_LIBS = -lgcc
ifeq ($(BUILD),release)
USER_LINK_FLAGS += -Wl,-s
endif

# Linking rule for user ELFs
$(BUILD_DIR)/%.elf:
	@$(CC) $(USER_CFLAGS) $(USER_LINK_FLAGS) -Wl,-Ttext=0x80000000 -e _start -o $@ $^ $(USER_RUNTIME_LIBS)


# NX-SDK owns its C/assembly compilation rules, including the scalar alpha
# profile and architecture startup veneers. The superproject compiles only
# userland/integration-owned translation units below.

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c -o $@ $<

# The superproject owns dependency files only for translation units it
# compiles itself under user/<project>. NX-SDK and NX-CORE generate .d files
# from different working directories; including those here makes relative
# prerequisites such as "lib/lib.c" resolve against the superproject and can
# break an otherwise valid incremental build. Each nested repository consumes
# its own dependency files in its own Makefile.
DEPS := $(shell find "$(BUILD_DIR)/$(USER_DIR)" -type f -name '*.d' 2>/dev/null)
-include $(DEPS)

# Disk Generation
# Publishing rootfs is a real artifact dependency, not an unconditional phony
# directory deletion on every make run / nxadb invocation. The marker sits
# outside the guest image and is created only after all copies succeed.
# Track programs, libraries, the packaging recipe and resource directories;
# directory mtimes also catch addition/removal of an asset.
ROOTFS_STAMP = $(BUILD_DIR)/rootfs-ready.stamp
# MDEBUG is USERLAND image policy, never a kernel compile-time branch. The
# value file updates only on profile changes and invalidates the rootfs.
NX_BOOT_PROFILE = $(BUILD_DIR)/nx-boot-profile.value
.PHONY: nx-boot-profile-force
nx-boot-profile-force:
$(NX_BOOT_PROFILE): nx-boot-profile-force
	@mkdir -p $(dir $@)
	@printf '%s\n' "$(MDEBUG)" > "$@.tmp"; \
	  if test -r "$@" && cmp -s "$@.tmp" "$@"; then rm "$@.tmp"; \
	  else mv "$@.tmp" "$@"; fi
ROOTFS_ASSET_DIRS = user/NexsOS1-main/home/Settings user/NexsOS1-main/home/Pictures user/NexsOS1-main/home/Video \
                    user/NexsOS1-main/sys/bin/nxfont/fonts user/NexsOS1-main/sys/bin/background
# Escape whitespace inside asset paths so fonts such as "Orkney Light.ttf"
# remain real Make prerequisites instead of silently skipped input files.
ROOTFS_ASSETS = $(shell find $(ROOTFS_ASSET_DIRS) \
                  \( -type f -o -type d -o -type l \) 2>/dev/null | \
                  sed 's/ /\\ /g') \
                user/NexsOS1-main/sys/boot/manager.conf \
                user/NexsOS1-main/sys/boot/services.conf user/NexsOS1-main/sys/boot/project \
                $(wildcard $(NEXS_BINEXTERNAL_DIR_doom)/freedoom*.wad)
rootfs: user libsdl2 liblua libgnulib coreutils $(ROOTFS_STAMP)
	@test -d "$(BUILD_DIR)/rootfs" || { \
	  echo "ERROR: rootfs missing while $(ROOTFS_STAMP) exists" >&2; exit 1; }
$(ROOTFS_STAMP): $(USER_ELFS) $(USERKIT_ELFS) $(SDL2_LIB) $(LUA_LIB) $(LUA_OS1_LIB) \
                $(GNULIB_LIB) $(USER_PROJECT_LIB_GLOBAL_FILES) $(ROOTFS_ASSETS) $(NX_BOOT_PROFILE) Makefile
	@rm -rf $(BUILD_DIR)/rootfs
	@mkdir -p $(BUILD_DIR)/rootfs/sbin
	@mkdir -p $(BUILD_DIR)/rootfs/krnl/extension
	@mkdir -p $(BUILD_DIR)/rootfs/sys/bin
	@mkdir -p $(BUILD_DIR)/rootfs/sys/services
	@mkdir -p $(BUILD_DIR)/rootfs/bin
	@mkdir -p $(BUILD_DIR)/rootfs/etc
	@mkdir -p $(BUILD_DIR)/rootfs/system/boot
	@mkdir -p $(BUILD_DIR)/rootfs/tmp
	@mkdir -p $(BUILD_DIR)/rootfs/var/tmp
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib
	@mkdir -p $(BUILD_DIR)/rootfs/home/Pictures/background
	@mkdir -p $(BUILD_DIR)/rootfs/home/Pictures/icon/dark
	@mkdir -p $(BUILD_DIR)/rootfs/home/Pictures/icon/light
	@mkdir -p $(BUILD_DIR)/rootfs/lib
	@# Lua module search root (lua_portability.h LUA_LDIR/LUA_CDIR): system
	@# .lua/.so modules live here — read-for-all, root-write-only (unlike
	@# /home). ext4 has no runtime dir creation, so pre-create the tree.
	@mkdir -p $(BUILD_DIR)/rootfs/lib/lua/5.4/lib
	@set -eu; for f in $(USER_PROJECT_LIB_GLOBAL_FILES); do \
	  name="$$(basename "$$f")"; cp "$$f" "$(BUILD_DIR)/rootfs/lib/$$name"; \
	done
	@mkdir -p  $(BUILD_DIR)/rootfs/sys/lib/include
	@# /home: the ONLY user-writable tree (vfs_write_allowed tree ACL —
	@# /sys/bin machine-only, every other tree root-only, guests confined to
	@# /home/shared).  Pre-created because ext4 has no directory creation yet:
	@# Documents/doom hosts doom's config+savegames (replaces the hardcoded
	@mkdir -p $(BUILD_DIR)/rootfs/home/Documents/doom
	@mkdir -p $(BUILD_DIR)/rootfs/home/Video
	@mkdir -p $(BUILD_DIR)/rootfs/home/shared
	@# Two independent real RW ext4 fixtures for the USER-level fd/writes tests.
	@# Their contents are on-disk files, not a test double or a runtime alias.
	@printf 'FDTEST00FDTEST01: real file descriptor and seek fixture.\n' > $(BUILD_DIR)/rootfs/home/shared/fdtest.fixture
	@printf '# writetest seed block\n' > $(BUILD_DIR)/rootfs/home/shared/writetest.fixture
	@printf 'GNULIB-FIXTURE: real read-write ext4 file.\n' > $(BUILD_DIR)/rootfs/home/shared/gnulib.fixture
	@mkdir -p $(BUILD_DIR)/rootfs/home/Settings
	@-cp -r user/NexsOS1-main/home/Settings/. $(BUILD_DIR)/rootfs/home/Settings/ 2>/dev/null || true
	@cp $(USERKIT_MAIN_INIT_ELF) $(BUILD_DIR)/rootfs/sbin/nxinit
	@cp $(NX_MACHINE_EXTENSION_ELFS) $(BUILD_DIR)/rootfs/krnl/extension/
	@cp $(SYS_ELFS) $(BUILD_DIR)/rootfs/sys/bin/
	@cp $(USERKIT_SERVICE_ELFS) $(BUILD_DIR)/rootfs/sys/services/
	@set -eu; for f in $(USERKIT_RUNTIME_TEST_ELFS); do \
	  name="$$(basename "$$f" .elf)"; name="$${name#test_}"; \
	  cp "$$f" "$(BUILD_DIR)/rootfs/sys/bin/nxtest_userkit_$$name"; \
	done
	@cp $(BIN_ELFS) $(BUILD_DIR)/rootfs/bin/
	@cp user/NexsOS1-main/sys/boot/manager.conf $(BUILD_DIR)/rootfs/system/boot/manager.conf
	@cp user/NexsOS1-main/sys/boot/services.conf $(BUILD_DIR)/rootfs/system/boot/services.conf
	@cp user/NexsOS1-main/sys/boot/project $(BUILD_DIR)/rootfs/system/boot/project
	@cp $(NX_BOOT_PROFILE) $(BUILD_DIR)/rootfs/system/boot/profile
	@# Pre-create a 16KB scratch file for the /bin/stress file lane: the FS has no
	@# runtime file creation (O_CREAT -> -EINVAL), and file_write only extends an
	@# EXISTING file, so ship one (perf §6 stress harness).
	@dd if=/dev/zero of=$(BUILD_DIR)/rootfs/etc/stress.tmp bs=1024 count=16 2>/dev/null
	@# Copy essential WAD files to the root and /bin for engine detection
	@-cp $(NEXS_BINEXTERNAL_DIR_doom)/freedoom1.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp $(NEXS_BINEXTERNAL_DIR_doom)/freedoom2.wad $(BUILD_DIR)/rootfs/bin/ 2>/dev/null || true
	@-cp user/NexsOS1-main/home/Pictures/icon/dark/*.png $(BUILD_DIR)/rootfs/home/Pictures/icon/dark/ 2>/dev/null || true
	@-cp user/NexsOS1-main/sys/bin/background/nxduck.png $(BUILD_DIR)/rootfs/sys/bin/background/ 2>/dev/null || true
	@# Dock/launcher tile icons (nxicon.h): both theme sets, so a runtime
	@# theme flip never needs a rebuilt image — see nxicon.h's header comment.
	@-cp user/NexsOS1-main/home/Pictures/globe.png $(BUILD_DIR)/rootfs/home/Pictures/ 2>/dev/null || true
	@-cp user/NexsOS1-main/home/Pictures/background/nxduck.png $(BUILD_DIR)/rootfs/home/Pictures/background/ 2>/dev/null || true
	@-cp user/NexsOS1-main/home/Pictures/icon/light/*.png $(BUILD_DIR)/rootfs/home/Pictures/icon/light/ 2>/dev/null || true
	@-cp user/NexsOS1-main/home/Video/*.mpeg $(BUILD_DIR)/rootfs/home/Video/ 2>/dev/null || true
	@# doom savegames are runtime-created in /home/Documents/doom now; the
	@mkdir -p $(BUILD_DIR)/rootfs/fonts
	@-cp user/NexsOS1-main/sys/bin/nxfont/fonts/*.ttf $(BUILD_DIR)/rootfs/fonts/ 2>/dev/null || true
	@-cp user/NexsOS1-main/sys/bin/nxfont/fonts/*.off $(BUILD_DIR)/rootfs/fonts/ 2>/dev/null || true
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib/include	
	@# Ship the system libraries into /sys/lib. These are HARD copies (no
	@# 2>/dev/null||true mask): rootfs now depends on libsdl2+liblua, so a
	@# missing archive is a real build error, not a silently-empty /sys/lib.
	@cp $(SDL2_LIB) $(BUILD_DIR)/rootfs/sys/lib/libSDL2.a
	@cp $(LUA_LIB) $(BUILD_DIR)/rootfs/sys/lib/liblua.a
	@cp $(LUA_OS1_LIB) $(BUILD_DIR)/rootfs/sys/lib/libos1lua.a
	@cp $(GNULIB_LIB) $(BUILD_DIR)/rootfs/sys/lib/libgnulib.a
	@# The include tree was created empty above: do not delete it mid-build.
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib/include/SDL2
	@cp -r $(SDL2_DIR)/include/. $(BUILD_DIR)/rootfs/sys/lib/include/SDL2/
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib/include/lua
	@cp $(LUA_DIR)/*.h $(BUILD_DIR)/rootfs/sys/lib/include/lua/
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib/include/api
	@cp -r system/include/api/. $(BUILD_DIR)/rootfs/sys/lib/include/api/
	@mkdir -p $(BUILD_DIR)/rootfs/sys/lib/include/contract
	@cp -r system/include/contract/. $(BUILD_DIR)/rootfs/sys/lib/include/contract/
	@# Copy Lua's own test suite next to nxlua, for on-device testing
	@mkdir -p $(BUILD_DIR)/rootfs/home/LUA/luatest
	@-cp -r $(LUA_DIR)/testes/. $(BUILD_DIR)/rootfs/home/LUA/luatest/ 2>/dev/null || true
	@# Remove .elf extensions in rootfs
	@for f in $(BUILD_DIR)/rootfs/sys/bin/*.elf; do [ -e "$$f" ] || continue; mv "$$f" "$${f%.elf}"; done
	@for f in $(BUILD_DIR)/rootfs/sys/services/*.elf; do [ -e "$$f" ] || continue; mv "$$f" "$${f%.elf}"; done
	@for f in $(BUILD_DIR)/rootfs/bin/*.elf; do [ -e "$$f" ] || continue; mv "$$f" "$${f%.elf}"; done
	@for f in $(BUILD_DIR)/rootfs/sbin/*.elf; do [ -e "$$f" ] || continue; mv "$$f" "$${f%.elf}"; done
	@for f in $(BUILD_DIR)/rootfs/krnl/extension/*.elf; do [ -e "$$f" ] || continue; mv "$$f" "$${f%.elf}"; done
	@# Rinomina i file cu_* togliendo il prefisso (senza duplicare)
	@for cu in $(COREUTILS_NAMES); do \
		if [ -f $(BUILD_DIR)/rootfs/bin/cu_$$cu ]; then \
			mv $(BUILD_DIR)/rootfs/bin/cu_$$cu $(BUILD_DIR)/rootfs/bin/$$cu; \
		fi; \
	done

	@# Keep a system-level env mirror in /sys/bin for direct invocation and PATH
	@# resolution, matching the default runtime PATH configured by nxinit.
	@-cp $(BUILD_DIR)/rootfs/bin/env $(BUILD_DIR)/rootfs/sys/bin/env 2>/dev/null || true
	@touch "$(ROOTFS_STAMP)"


disk: $(MKDISK) kernel rootfs bootloader
	@mkdir -p $(BUILD_DIR)
	@./$(MKDISK) $(DISK_IMG) $(BOOTLOADER_BIN) $(KERNEL_BIN) $(BUILD_DIR)/rootfs $(MKDISK_LAYOUT)

.PHONY: demo1-build demo1-rootfs main-user-image demo1-user-image user-images

demo1-build: sdk
	+$(MAKE) -C user/demo1 ARCH=$(ARCH) BUILD_ROOT="$(DEMO1_BUILD_ROOT)" \
	  SDK_ROOT="$(abspath $(NXSDK_DIR))" check
	@test -s "$(DEMO1_ELF)"

$(DEMO1_ROOTFS_STAMP): demo1-build userkit $(NX_MACHINE_EXTENSION_ELFS) $(NX_BOOT_PROFILE) \
                      user/demo1/sys/boot/manager.conf user/demo1/sys/boot/services.conf \
                      user/demo1/sys/boot/project \
                      $(USER_PROJECT_LIB_GLOBAL_FILES) user/demo1/assets/fonts/Rewir-Light.off Makefile
	@rm -rf "$(DEMO1_ROOTFS)"
	@mkdir -p "$(DEMO1_ROOTFS)/sbin" "$(DEMO1_ROOTFS)/krnl/extension"
	@mkdir -p "$(DEMO1_ROOTFS)/sys/bin" "$(DEMO1_ROOTFS)/sys/services"
	@mkdir -p "$(DEMO1_ROOTFS)/system/boot" "$(DEMO1_ROOTFS)/lib"
	@mkdir -p "$(DEMO1_ROOTFS)/fonts" "$(DEMO1_ROOTFS)/home" "$(DEMO1_ROOTFS)/tmp" "$(DEMO1_ROOTFS)/var/tmp"
	@cp "$(USERKIT_DEMO_INIT_ELF)" "$(DEMO1_ROOTFS)/sbin/nxdemo1init"
	@cp "$(NX_MACHINE_EXTENSION_ELFS)" "$(DEMO1_ROOTFS)/krnl/extension/nxmanageDD"
	@cp "$(DEMO1_ELF)" "$(DEMO1_ROOTFS)/sys/bin/demo1shell"
	@set -eu; for f in $(USER_PROJECT_LIB_GLOBAL_FILES); do \
	  name="$$(basename "$$f")"; cp "$$f" "$(DEMO1_ROOTFS)/lib/$$name"; \
	done
	@set -eu; for f in $(DEMO1_USERKIT_SERVICE_ELFS); do \
	  name="$$(basename "$$f" .elf)"; cp "$$f" "$(DEMO1_ROOTFS)/sys/services/$$name"; \
	done
	@set -eu; for f in $(DEMO1_USERKIT_TEST_ELFS); do \
	  name="$$(basename "$$f" .elf)"; name="$${name#test_}"; \
	  cp "$$f" "$(DEMO1_ROOTFS)/sys/bin/nxtest_userkit_$$name"; \
	done
	@cp user/demo1/sys/boot/manager.conf "$(DEMO1_ROOTFS)/system/boot/manager.conf"
	@cp user/demo1/sys/boot/services.conf "$(DEMO1_ROOTFS)/system/boot/services.conf"
	@cp user/demo1/sys/boot/project "$(DEMO1_ROOTFS)/system/boot/project"
	@cp "$(NX_BOOT_PROFILE)" "$(DEMO1_ROOTFS)/system/boot/profile"
	@cp user/demo1/assets/fonts/Rewir-Light.off "$(DEMO1_ROOTFS)/fonts/Rewir-Light.off"
	@touch "$@"

demo1-rootfs: $(DEMO1_ROOTFS_STAMP)
	@test -d "$(DEMO1_ROOTFS)"

main-user-image: disk
	@mkdir -p "$(USER_IMAGE_DIR)"
	@cp "$(DISK_IMG)" "$(MAIN_USER_IMAGE)"

demo1-user-image: $(MKDISK) kernel bootloader demo1-rootfs
	@mkdir -p "$(USER_IMAGE_DIR)"
	@./$(MKDISK) "$(DEMO1_USER_IMAGE)" "$(BOOTLOADER_BIN)" "$(KERNEL_BIN)" \
	  "$(DEMO1_ROOTFS)" $(MKDISK_LAYOUT)

user-images: main-user-image demo1-user-image
	@python3 kernel/tools/user-project-catalog.py --catalog kernel/config/user-projects.conf list >/dev/null
	@test -s "$(MAIN_USER_IMAGE)" -a -s "$(DEMO1_USER_IMAGE)"
	@echo "User-project images ready: $(MAIN_USER_IMAGE) $(DEMO1_USER_IMAGE)"

$(MKDISK): tools/mkdisk.c
	@mkdir -p tools
	@gcc -o $(MKDISK) tools/mkdisk.c

# =============================================================================
# QEMU Flags
# =============================================================================

# QEMU runtime policy lives in tools/nxqemu.py for both GRUB and direct
# boots. QEMU_RUN remains only for the bounded QEMU DTB-generation command.
QEMU_RUN = env -u GTK_PATH -u GTK_EXE_PREFIX -u GTK_MODULES -u GTK_IM_MODULE_FILE \
               -u GIO_MODULE_DIR -u GSETTINGS_SCHEMA_DIR -u XDG_DATA_DIRS \
               -u XDG_DATA_HOME -u LOCPATH $(QEMU)

.PHONY: grub-i386-pc-tools grub-x86_64-efi-tools grub-arm64-efi-tools
.PHONY: grub-tools grub-prebuilt grub-prebuilt-target grub-prebuilt-install
.PHONY: boot-iso boot-uefi

grub-i386-pc-tools:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" kernel/tools/build-grub-i386-pc.sh

grub-x86_64-efi-tools:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" kernel/tools/build-grub-x86_64.sh

grub-arm64-efi-tools:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" kernel/tools/build-grub-arm64.sh

grub-tools: grub-i386-pc-tools grub-x86_64-efi-tools grub-arm64-efi-tools

GRUB_TARGET ?= all
GRUB_ARCHIVE ?=

grub-prebuilt:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" kernel/tools/package-grub-target.sh all

grub-prebuilt-target:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" kernel/tools/package-grub-target.sh "$(GRUB_TARGET)"

grub-prebuilt-install:
	@test -n "$(GRUB_ARCHIVE)" || { echo "GRUB_ARCHIVE=<archive.tar.gz> is required" >&2; exit 2; }
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" kernel/tools/install-grub-prebuilt.sh "$(GRUB_ARCHIVE)"

ifeq ($(ARCH),amd64)
boot-iso: all user-images
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" \
	  NEXS_USER_IMAGE_DIR="$(abspath $(USER_IMAGE_DIR))" \
	  tools/build-amd64-grub-iso.sh

boot-uefi: all user-images
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" \
	  NEXS_USER_IMAGE_DIR="$(abspath $(USER_IMAGE_DIR))" \
	  kernel/tools/build-amd64-uefi-image.sh
else
boot-iso: all user-images $(VIRT_DTB)
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" \
	  NEXS_DTB_PATH="$(abspath $(VIRT_DTB))" \
	  NEXS_USER_IMAGE_DIR="$(abspath $(USER_IMAGE_DIR))" \
	  tools/build-aarch64-uefi-image.sh

boot-uefi: boot-iso
endif

# =============================================================================
# Release Generation
# =============================================================================

release: power12-strict
	@echo "========================================"
	@echo "  Creating Full Release $(VERSION)"
	@echo "========================================"
	@$(MAKE) ARCH=amd64 release-arch VERSION=$(VERSION)
	@$(MAKE) ARCH=aarch64 release-arch VERSION=$(VERSION)
	@echo "========================================"
	@echo "✓ Full release $(VERSION) complete!"
	@echo "  Location: $(RELEASE_BASE)"
	@echo "========================================"

# Keep boot media and root filesystem as separate artifacts. AMD64 releases
# carry both BIOS and UEFI GRUB media; AArch64 release media is UEFI-only.
ifeq ($(ARCH),amd64)
RELEASE_BOOT_TARGETS = boot-iso boot-uefi
else
RELEASE_BOOT_TARGETS = boot-iso
endif

release-arch: power12-strict all $(RELEASE_BOOT_TARGETS)
	@echo "--> Packaging $(ARCH) release..."
	@rm -rf $(RELEASE_DIR)
	@mkdir -p $(RELEASE_DIR)
	@cp $(DISK_IMG) $(RELEASE_DIR)/disk.img
	@if [ "$(ARCH)" = "amd64" ]; then \
		cp $(AMD64_GRUB_ISO) $(RELEASE_DIR)/NexsOS1-amd64-$(VERSION)-bios.iso; \
		cp $(AMD64_UEFI_ISO) $(RELEASE_DIR)/NexsOS1-amd64-$(VERSION)-uefi.iso; \
		cp $(KERNEL_ELF) $(RELEASE_DIR)/kernel.elf; \
		printf 'architecture=amd64\nversion=%s\nbootloader=GRUB\nbios_iso=%s\nuefi_iso=%s\nrootfs=disk.img\n' \
			'$(VERSION)' 'NexsOS1-amd64-$(VERSION)-bios.iso' \
			'NexsOS1-amd64-$(VERSION)-uefi.iso' > $(RELEASE_DIR)/manifest.txt; \
	else \
		cp $(BOOT_ISO) $(RELEASE_DIR)/NexsOS1-aarch64-$(VERSION)-uefi.iso; \
		cp $(KERNEL_BIN) $(RELEASE_DIR)/kernel.img; \
		cp $(VIRT_DTB) $(RELEASE_DIR)/virt.dtb; \
		cp $(BUILD_DIR)/grub-efi/manifest.txt $(RELEASE_DIR)/boot-manifest.txt; \
		printf 'architecture=aarch64\nversion=%s\nbootloader=GRUB\nuefi_iso=%s\nrootfs=disk.img\n' \
			'$(VERSION)' 'NexsOS1-aarch64-$(VERSION)-uefi.iso' > $(RELEASE_DIR)/manifest.txt; \
	fi
	@echo "Release ready: $(RELEASE_DIR)"

ifeq ($(ARCH),amd64)
test-release: release-arch
	@echo "Starting AMD64 BIOS GRUB release test ($(VERSION))..."
	@python3 tools/nxqemu.py --arch amd64 --boot-mode iso --firmware bios \
		--iso "$(RELEASE_DIR)/NexsOS1-amd64-$(VERSION)-bios.iso" \
		--disk "$(RELEASE_DIR)/disk.img" --serial stdio

test-release-uefi: release-arch
	@echo "Starting AMD64 UEFI GRUB release test ($(VERSION))..."
	@python3 tools/nxqemu.py --arch amd64 --boot-mode iso --firmware uefi \
		--iso "$(RELEASE_DIR)/NexsOS1-amd64-$(VERSION)-uefi.iso" \
		--disk "$(RELEASE_DIR)/disk.img" --serial stdio
else
test-release: release-arch
	@echo "Starting AArch64 UEFI GRUB release test ($(VERSION))..."
	@python3 tools/nxqemu.py --arch aarch64 --boot-mode iso --firmware uefi \
		--iso "$(RELEASE_DIR)/NexsOS1-aarch64-$(VERSION)-uefi.iso" \
		--disk "$(RELEASE_DIR)/disk.img" --serial stdio

test-release-uefi: test-release
endif

# =============================================================================
# Development Execution
# =============================================================================

# The QEMU virt DTB must match the AArch64 emulated hardware profile. It is
# embedded into the project-owned EFI loader and passed to the kernel in x0.
ifeq ($(ARCH),aarch64)
# The DTB target may be the first consumer after a clean build; define its
# directory as a real Make target instead of relying on the side effect of
# dirs (which is not a prerequisite of this standalone target).
$(BUILD_DIR):
	@mkdir -p $@

$(VIRT_DTB): disk | $(BUILD_DIR)
	@echo "  [DTB]    Generating QEMU virt device tree -> $@"
	@ERRLOG="$(BUILD_DIR)/qemu-dtb.err"; rm -f "$$ERRLOG"; \
	if ! $(QEMU_RUN) -M virt -cpu cortex-a57 -m 5G -smp 4 \
	         -device virtio-gpu-device \
	         -device virtio-keyboard-device -device virtio-mouse-device \
	         -drive if=none,file=$(DISK_IMG),id=hd0,format=raw \
	         -device virtio-blk-device,drive=hd0 \
	         -machine dumpdtb=$@ 2>"$$ERRLOG"; then \
	    cat "$$ERRLOG" >&2; \
	    echo "  [DTB]    QEMU DTB generation failed; see $$ERRLOG" >&2; \
	    exit 1; \
	fi; \
	rm -f "$$ERRLOG"
	@echo "  [DTB]    Done ($$($(STAT_SIZE) $@) bytes)"
endif

# ISO mode exercises the GRUB chain used by nxadb/nxrun and release media.
# Direct mode is the explicit legacy test path: AArch64 receives the generated
# DTB through the ARM64 Image handoff (x0=DTB, x1=0); AMD64 receives QEMU's PVH
# start-info rather than GRUB's Multiboot2 information structure.
BOOT_MODE ?= iso
RUN_FIRMWARE ?= auto
RUN_DISPLAY ?= default

ifeq ($(BOOT_MODE),direct)
ifneq ($(RUN_FIRMWARE),auto)
$(error RUN_FIRMWARE applies only to ISO boot; direct mode requires RUN_FIRMWARE=auto)
endif
endif
RUN_SERIAL ?= stdio
RUN_QMP ?=
RUN_QMP_ARG := $(if $(strip $(RUN_QMP)),--qmp "$(RUN_QMP)",)

ifeq ($(ARCH),amd64)
ifeq ($(RUN_FIRMWARE),auto)
RUN_ISO_TARGET = boot-iso
RUN_ISO_IMAGE = $(AMD64_GRUB_ISO)
RUN_ISO_FIRMWARE = bios
else ifeq ($(RUN_FIRMWARE),bios)
RUN_ISO_TARGET = boot-iso
RUN_ISO_IMAGE = $(AMD64_GRUB_ISO)
RUN_ISO_FIRMWARE = bios
else ifeq ($(RUN_FIRMWARE),uefi)
RUN_ISO_TARGET = boot-uefi
RUN_ISO_IMAGE = $(AMD64_UEFI_ISO)
RUN_ISO_FIRMWARE = uefi
else
$(error RUN_FIRMWARE must be 'auto', 'bios' or 'uefi'; got '$(RUN_FIRMWARE)')
endif
else
ifeq ($(RUN_FIRMWARE),bios)
$(error AArch64 has no BIOS lane; use RUN_FIRMWARE=auto or uefi)
endif
RUN_ISO_TARGET = boot-uefi
RUN_ISO_IMAGE = $(AARCH64_UEFI_ISO)
RUN_ISO_FIRMWARE = uefi
endif

.PHONY: direct-artifacts
ifeq ($(ARCH),aarch64)
DIRECT_KERNEL = $(KERNEL_BIN)
DIRECT_DTB_ARG = --dtb "$(VIRT_DTB)"
DIRECT_PREREQS = all $(VIRT_DTB)
direct-artifacts: all $(VIRT_DTB)
else
DIRECT_KERNEL = $(KERNEL_ELF)
DIRECT_DTB_ARG =
DIRECT_PREREQS = all
direct-artifacts: all
endif

# Headless is nxadb's own default and prevents host keyboard input contaminating
# the guest command/health transaction during automated test-all runs.
TEST_DISPLAY ?= none
# Exercise process termination across a timed nanosleep in every QEMU test.
TEST_TIMER_SECONDS ?= 10
ifeq ($(BOOT_MODE),iso)
run: all $(RUN_ISO_TARGET)
	@python3 tools/nxqemu.py --arch $(ARCH) --boot-mode iso --firmware $(RUN_ISO_FIRMWARE) 		--iso "$(RUN_ISO_IMAGE)" --disk "$(DISK_IMG)" --serial "$(RUN_SERIAL)" --display "$(RUN_DISPLAY)" $(RUN_QMP_ARG)
debug: all $(RUN_ISO_TARGET)
	@python3 tools/nxqemu.py --arch $(ARCH) --boot-mode iso --firmware $(RUN_ISO_FIRMWARE) 		--iso "$(RUN_ISO_IMAGE)" --disk "$(DISK_IMG)" --serial "$(RUN_SERIAL)" --display "$(RUN_DISPLAY)" $(RUN_QMP_ARG) --debug
else ifeq ($(BOOT_MODE),direct)
run: $(DIRECT_PREREQS)
	@python3 tools/nxqemu.py --arch $(ARCH) --boot-mode direct --kernel "$(DIRECT_KERNEL)" $(DIRECT_DTB_ARG) --disk "$(DISK_IMG)" --serial "$(RUN_SERIAL)" --display "$(RUN_DISPLAY)" $(RUN_QMP_ARG)
debug: $(DIRECT_PREREQS)
	@python3 tools/nxqemu.py --arch $(ARCH) --boot-mode direct --kernel "$(DIRECT_KERNEL)" $(DIRECT_DTB_ARG) --disk "$(DISK_IMG)" --serial "$(RUN_SERIAL)" --display "$(RUN_DISPLAY)" $(RUN_QMP_ARG) --debug
else
$(error BOOT_MODE must be 'direct' or 'iso'; got '$(BOOT_MODE)')
endif

# Backwards-compatible names delegate to the single direct nxqemu path.
run-direct:
	@$(MAKE) ARCH=$(ARCH) BOOT_MODE=direct RUN_FIRMWARE=auto RUN_DISPLAY="$(RUN_DISPLAY)" RUN_SERIAL="$(RUN_SERIAL)" run

debug-direct:
	@$(MAKE) ARCH=$(ARCH) BOOT_MODE=direct RUN_FIRMWARE=auto RUN_DISPLAY="$(RUN_DISPLAY)" RUN_SERIAL="$(RUN_SERIAL)" debug

# lockcheck — the two memory/lock rules as a build gate (scripts/check-lockrules.py).
# Not yet wired into `all`: the transitive closure is deliberately over-eager
# and the current output needs a triage pass before it can block a build.
# Run it before touching the scheduler or any teardown path.
lockcheck:
	@python3 scripts/check-lockrules.py

check-kernel-contracts:
	@python3 tools/generate-surface-contract.py --check
	@python3 tools/generate-time-contract.py --check
	@python3 tools/check-time-contract-layout.py
	@test "$$(cd system && pwd)" = "$$(make -s -C kernel print-sdk-root)"
	@kernel/tools/check-contract-snapshot.sh
	@python3 system/tools/check-contract-snapshot.py
	@python3 tools/check-cross-repo-contracts.py

.PHONY: check-buffer-source
check-buffer-source:
	@python3 tools/check-buffer-source.py

disasm: $(KERNEL_ELF) $(BOOTLOADER_ELF)
	$(OBJDUMP) -d $(KERNEL_ELF) > $(BUILD_DIR)/kernel.disasm
	$(OBJDUMP) -d $(BOOTLOADER_ELF) > $(BUILD_DIR)/bootloader.disasm
	$(OBJDUMP) -d $(USER_ELF) > $(BUILD_DIR)/init.disasm

EXPECTED_GCC_VERSION = $(if $(filter amd64,$(ARCH)),$(NEXS_X86_64_GCC_VERSION),$(NEXS_AARCH64_GCC_VERSION))
EXPECTED_BINUTILS_VERSION = $(if $(filter amd64,$(ARCH)),$(NEXS_X86_64_BINUTILS_VERSION),$(NEXS_AARCH64_BINUTILS_VERSION))

.PHONY: test-nxpax-core test-nxpax-stress test-nxpax-ast power12 power12-changed power12-strict check-strict
test-nxpax-core:
	@mkdir -p $(BUILD_ROOT)/tests/nxpax
	@clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wpedantic \
	  -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Ikernel/HAL/include -Ikernel/include \
	  kernel/test/nxpax_core_test.c kernel/core/nxpax.c \
	  kernel/HAL/core/nxpax.c -o $(BUILD_ROOT)/tests/nxpax/core_test
	@$(BUILD_ROOT)/tests/nxpax/core_test

test-nxpax-stress:
	@mkdir -p $(BUILD_ROOT)/tests/nxpax
	@clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wpedantic \
	  -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
	  -Ikernel/HAL/include kernel/HAL/tests/unit/nxpax_stress_test.c \
	  kernel/HAL/core/nxpax.c -o $(BUILD_ROOT)/tests/nxpax/stress_test
	@$(BUILD_ROOT)/tests/nxpax/stress_test

# Preserve NXPAX-specific Clang-AST mutation tests in addition to the
# project-wide mutation suite; they are non-mocking security regression tests.
test-nxpax-ast:
	@python3 tools/test_power12_nxpax.py

# Audit every development build and preserve failure reports; failed findings
# are warnings for maintenance, not successful compliance results.
power12:
	@python3 tools/power12/run.py --warn-only --incremental
	@$(MAKE) --no-print-directory power12-changed

power12-changed:
	@python3 tools/power12/test_changed_gate.py
	@python3 tools/power12/changed_gate.py

# Qualification is never downgraded: strict mode fails on real findings.
power12-strict:
	@python3 tools/power12/run.py

check-strict: check power12-strict

check: check-kernel-contracts power12 test-nxpax-core test-nxpax-stress test-nxpax-ast
	@set -eu; for tool in "$(CC)" "$(AS)" "$(LD)" "$(OBJCOPY)" "$(OBJDUMP)" "$(NM)" "$(AR)" "$(QEMU)" python3 xorriso gawk mformat mmd mcopy; do \
		command -v "$$tool" >/dev/null 2>&1 || { echo "ERROR: required tool not found: $$tool" >&2; exit 1; }; \
	done
	@target="$$($(CC) -dumpmachine)"; \
	case "$(ARCH):$$target" in \
		amd64:x86_64-elf|aarch64:aarch64-none-elf) echo "✓ Target: $$target ($(ARCH))" ;; \
		*) echo "ERROR: $(CC) targets '$$target', expected freestanding target for $(ARCH)" >&2; exit 1 ;; \
	esac
	@cc_version="$$($(CC) -dumpfullversion -dumpversion)"; \
	[ "$$cc_version" = "$(EXPECTED_GCC_VERSION)" ] || { echo "ERROR: $(CC) is $$cc_version; expected $(EXPECTED_GCC_VERSION)" >&2; exit 1; }
	@set -eu; for tool in "$(AS)" "$(LD)" "$(OBJCOPY)" "$(OBJDUMP)" "$(NM)" "$(AR)"; do \
		tool_version="$$($$tool --version | sed -E -n '1s/.* ([0-9]+([.][0-9]+)+)$$/\1/p')"; \
		[ "$$tool_version" = "$(EXPECTED_BINUTILS_VERSION)" ] || { echo "ERROR: $$tool is $$tool_version; expected Binutils $(EXPECTED_BINUTILS_VERSION)" >&2; exit 1; }; \
	done
	@echo "✓ CC: $$($(CC) --version | head -1)"
	@echo "✓ LD: $$($(LD) --version | head -1)"
	@echo "✓ QEMU: $$($(QEMU) --version | head -1)"
	@expected_grub="$$(. kernel/config/grub.env; printf '%s' "$$NEXS_GRUB_COMMIT")"; \
	actual_grub="$$(git -C kernel/external/grub rev-parse HEAD 2>/dev/null || true)"; \
	[ "$$actual_grub" = "$$expected_grub" ] || { \
		echo "ERROR: GRUB submodule is $$actual_grub; expected $$expected_grub" >&2; exit 1; \
	}; \
	echo "✓ GRUB source pin: $$actual_grub"
ifeq ($(ARCH),amd64)
	@set -eu; for tool in i686-elf-gcc i686-elf-objcopy; do \
		command -v "$$tool" >/dev/null 2>&1 || { echo "ERROR: $$tool is required for i386-pc GRUB" >&2; exit 1; }; \
	done
	@echo "✓ GRUB targets: i386-pc + x86_64-efi are project-built"
else
	@command -v "$(HOST_CLANG)" >/dev/null 2>&1 || { echo "ERROR: clang is required for the AArch64 EFI loader" >&2; exit 1; }
	@command -v "$(LLD_LINK)" >/dev/null 2>&1 || { echo "ERROR: lld-link is required for the AArch64 EFI loader" >&2; exit 1; }
	@python3 -c 'import sys; sys.path.insert(0,"tools"); from nxqemu import find_qemu_efi_firmware; print("✓ AArch64 UEFI firmware:", find_qemu_efi_firmware())'
	@echo "✓ GRUB arm64-efi prerequisites ready (grub-mkimage is built by boot-iso)."
endif
clean:
	-@rm -rf $(BUILD_ROOT)/aarch64 $(BUILD_ROOT)/amd64 2>/dev/null || true
	-@rm -rf $(BUILD_ROOT)/grub 2>/dev/null || true
	-@rm -f $(BUILD_ROOT)/*.img $(BUILD_ROOT)/*.iso $(BUILD_ROOT)/*.map 2>/dev/null || true
	-@rm -rf release 2>/dev/null || true
	-@rm -f $(MKDISK) 2>/dev/null || true
	@echo "Build outputs removed; preserving build/cache and diagnostic/runtime logs."

help:
	@echo "$(ARCH) Kernel Build System"
	@echo "Targets:"
	@echo "  all          - Build bootloader, kernel, and disk image"
	@echo "  release      - Build production files (Use: make release VERSION=0.4.1)"
	@echo "  test-release - Test the architecture default release firmware in QEMU"
	@echo "  test-release-uefi - Test the UEFI release lane explicitly"
	@echo "  boot-iso     - Build default GRUB media (AMD64 BIOS, AArch64 UEFI)"
	@echo "  boot-uefi    - Build GRUB UEFI media for the selected architecture"
	@echo "  grub-tools   - Build/publish i386-pc, x86_64-efi and arm64-efi prebuilt stages"
	@echo "  grub-prebuilt - Package all host-specific GRUB stages under release/grub"
	@echo "  grub-prebuilt-target - Package GRUB_TARGET=<target>"
	@echo "  grub-prebuilt-install - Install GRUB_ARCHIVE=<archive.tar.gz> into persistent cache"
	@echo "  run          - GRUB ISO boot; AMD64 RUN_FIRMWARE=bios|uefi, BOOT_MODE=direct for raw kernel"
	@echo "  run-direct   - Raw direct-kernel boot (diagnostic compatibility target)"
	@echo "  debug        - GRUB ISO debug boot (default); BOOT_MODE=direct selects raw-kernel diagnostics"
	@echo "  clean        - Remove generated build/release artifacts; preserve build/cache"
	@echo "  setup-deps   - Initialize pinned external repositories"
	@echo "  check-deps   - Verify external repository revisions"
	@echo "  check        - Verify cross-toolchain and emulator"
	@echo "  test-fdt     - Run bounded FDT parser host tests and QEMU DTB validation"

# DOOM is a pinned external binary project. NexsOS-specific build policy lives
# in the first-party provider selected by the BINEXTERNAL catalog, so the
# vendor tree remains independent of NexsOS1-main.
ifneq ($(wildcard $(NEXS_BINEXTERNAL_DIR_doom)/doomgeneric.c),)
include $(NEXS_BINEXTERNAL_PROVIDER_doom)
else
$(warning DOOM submodule is unavailable; run 'make setup-deps' before building the complete userland)
endif

# Host-side bounded FDT parser tests. The QEMU AArch64 DTB is a mandatory input.
.PHONY: test-fdt
test-fdt:
	@$(MAKE) ARCH=aarch64 build/aarch64/virt.dtb
	@mkdir -p build/host
	@$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
		-fsanitize=address,undefined -Ikernel/include \
		kernel/test/fdt_parser_test.c kernel/lib/fdt_parser.c \
		-o build/host/fdt_parser_test
	@./build/host/fdt_parser_test build/aarch64/virt.dtb

.PHONY: test-gfx-chrome
test-gfx-chrome:
	@mkdir -p build/host
	@$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
		-fsanitize=address,undefined -idirafter system/include/api -idirafter system/include/contract \
		system/lib/tests/gfx_chrome_test.c system/lib/gfx/chrome.c \
		system/lib/gfx/decoration.c system/lib/gfx/raster.c system/lib/gfx/alpha.c \
		-o build/host/gfx_chrome_test
	@./build/host/gfx_chrome_test

# Real QEMU hardening run for the selected architecture. Captures guest screens,
# checks KTEST/NXWINS, supervises the guest for 30 seconds and runs the timed
# process-kill stress lane (TEST_TIMER_SECONDS, default 10).
# TEST_DISPLAY defaults to nxadb's headless mode; set it to default for visual runs.
.PHONY: test test-all
test: all boot-iso check
	@$(MAKE) test-fdt
	@python3 tools/test_nxadb_capture.py
	@python3 tools/nxadb.py --arch $(ARCH) test --soak-seconds 30 --display $(TEST_DISPLAY) \
		--cmd "/bin/stress --dur $(TEST_TIMER_SECONDS) --seed 0xA11CE --interval 1 --fault 0 --timer-kill" \
		--expect stress=1

# Release-architecture gate. Keep only one guest alive at a time. The EXIT
# trap also stops both sessions if either architecture test fails.
test-all:
	@set -e; \
	trap 'python3 tools/nxadb.py --arch amd64 stop >/dev/null 2>&1 || true; python3 tools/nxadb.py --arch aarch64 stop >/dev/null 2>&1 || true' EXIT; \
	$(MAKE) ARCH=amd64 test; \
	python3 tools/nxadb.py --arch amd64 stop; \
	$(MAKE) ARCH=aarch64 test; \
	python3 tools/nxadb.py --arch aarch64 stop

.PHONY: test-gfx-alpha
test-gfx-alpha:
	@mkdir -p build/host
	@$(HOST_CC) -std=c11 -O2 -g -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
		-fsanitize=address,undefined -idirafter system/include/api -idirafter system/include/contract \
		user/NexsOS1-main/bin/gfxalphatest.c system/lib/gfx/alpha.c -o build/host/gfx_alpha_test
	@./build/host/gfx_alpha_test

.PHONY: test-mkdisk-groups
test-mkdisk-groups:
	@python3 tools/test_mkdisk_groups.py

.PHONY: test-timerdiag-sampling
test-timerdiag-sampling:
	@python3 tools/test_nxtimerdiag_sampling.py

.PHONY: test-spawn-owned test-entropy-policy
test-spawn-owned:
	@python3 tools/test_spawn_owned.py

test-entropy-policy:
	@python3 tools/test_entropy_policy.py

.PHONY: test-scheduler-quantum
test-scheduler-quantum:
	@python3 tools/test_scheduler_quantum.py

.PHONY: test-pmm-free-publish test-file-length test-getcwd-contract
test-pmm-free-publish:
	@python3 tools/test_pmm_free_publish.py

test-file-length:
	@python3 tools/test_file_length.py

test-getcwd-contract:
	@python3 tools/test_getcwd_contract.py

.PHONY: test-open-nonblock test-gnulib-archive
test-open-nonblock:
	@python3 tools/test_open_nonblock.py

test-gnulib-archive:
	@python3 tools/test_gnulib_archive.py

.PHONY: test-keyboard-layout test-tty-model
test-keyboard-layout:
	@mkdir -p $(BUILD_ROOT)/host
	@$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-fsanitize=address,undefined -Isystem/lib/extensions/input/include \
		system/lib/extensions/input/tests/keyboard_layout_test.c \
		system/lib/extensions/input/nxkeyboard_layout.c \
		-o $(BUILD_ROOT)/host/keyboard_layout_test
	@$(BUILD_ROOT)/host/keyboard_layout_test

test-tty-model:
	@mkdir -p $(BUILD_ROOT)/host
	@$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-fsanitize=address,undefined -idirafter system/include/api -idirafter system/include/contract \
		system/lib/tests/tty_model_test.c system/lib/extensions/USERKIT/nxttyd/tty_model.c \
		-o $(BUILD_ROOT)/host/tty_model_test
	@$(BUILD_ROOT)/host/tty_model_test
