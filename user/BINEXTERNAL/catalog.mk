# Canonical catalog for external user programs.
#
# Source projects live outside NexsOS1-main so the first-party userland remains
# independent of vendor source trees. Image providers may select these projects,
# but any produced executable is installed in the user VFS namespace (/bin),
# never in /sys/bin.
NEXS_BINEXTERNAL_ROOT ?= user/BINEXTERNAL

NEXS_BINEXTERNAL_PROJECTS := base_nexs busybox coreutils doom kilo
NEXS_BINEXTERNAL_IMAGE_PROJECTS := coreutils doom kilo
NEXS_BINEXTERNAL_REFERENCE_PROJECTS := base_nexs busybox

NEXS_BINEXTERNAL_DIR_base_nexs := $(NEXS_BINEXTERNAL_ROOT)/base-nexs
NEXS_BINEXTERNAL_DIR_busybox := $(NEXS_BINEXTERNAL_ROOT)/busybox
NEXS_BINEXTERNAL_DIR_coreutils := $(NEXS_BINEXTERNAL_ROOT)/coreutils
NEXS_BINEXTERNAL_DIR_doom := $(NEXS_BINEXTERNAL_ROOT)/doom
NEXS_BINEXTERNAL_DIR_kilo := $(NEXS_BINEXTERNAL_ROOT)/kilo

# First-party provider manifests own NexsOS-specific build/portability policy.
# Vendor trees remain source providers and are not coupled to NexsOS1-main.
NEXS_BINEXTERNAL_PROVIDER_coreutils := user/NexsOS1-main/portability/coreutils/overlay.mk
NEXS_BINEXTERNAL_PROVIDER_doom := user/NexsOS1-main/portability/doom/doom.mk
NEXS_BINEXTERNAL_PROVIDER_kilo := user/NexsOS1-main/portability/kilo/provider.mk

NEXS_BINEXTERNAL_VFS_DIR_base_nexs := /bin
NEXS_BINEXTERNAL_VFS_DIR_busybox := /bin
NEXS_BINEXTERNAL_VFS_DIR_coreutils := /bin
NEXS_BINEXTERNAL_VFS_DIR_doom := /bin
NEXS_BINEXTERNAL_VFS_DIR_kilo := /bin

# Compatibility declarations. "nxsdk-runtime" means the canonical NX-SDK
# libc/runtime object manifest consumed by the superproject. External names
# must resolve through user/LIBEXTERNAL/catalog.mk.
NEXS_BINEXTERNAL_COMPAT_base_nexs := reference-only
NEXS_BINEXTERNAL_INTERNAL_LIBS_base_nexs := none
NEXS_BINEXTERNAL_EXTERNAL_LIBS_base_nexs := none

NEXS_BINEXTERNAL_COMPAT_busybox := reference-only
NEXS_BINEXTERNAL_INTERNAL_LIBS_busybox := none
NEXS_BINEXTERNAL_EXTERNAL_LIBS_busybox := none

NEXS_BINEXTERNAL_COMPAT_coreutils := integrated
NEXS_BINEXTERNAL_INTERNAL_LIBS_coreutils := nxsdk-runtime
NEXS_BINEXTERNAL_EXTERNAL_LIBS_coreutils := gnulib

NEXS_BINEXTERNAL_COMPAT_doom := integrated
NEXS_BINEXTERNAL_INTERNAL_LIBS_doom := nxsdk-runtime
NEXS_BINEXTERNAL_EXTERNAL_LIBS_doom := none

NEXS_BINEXTERNAL_COMPAT_kilo := integrated
NEXS_BINEXTERNAL_INTERNAL_LIBS_kilo := nxsdk-runtime
NEXS_BINEXTERNAL_EXTERNAL_LIBS_kilo := none
