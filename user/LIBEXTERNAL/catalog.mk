# Canonical catalog for external user libraries.
#
# Vendor sources and first-party NexsOS providers are deliberately separated:
# vendor trees remain pinned dependencies, while providers own compatibility
# policy without coupling them to NexsOS1-main.
NEXS_LIBEXTERNAL_ROOT ?= user/LIBEXTERNAL

NEXS_LIBEXTERNAL_PROJECTS := direct3d gnulib lua musl opengl pl_mpeg sdl stb

NEXS_LIBEXTERNAL_DIR_direct3d := $(NEXS_LIBEXTERNAL_ROOT)/direct3d
NEXS_LIBEXTERNAL_DIR_gnulib := $(NEXS_LIBEXTERNAL_ROOT)/gnulib
NEXS_LIBEXTERNAL_DIR_lua := $(NEXS_LIBEXTERNAL_ROOT)/lua
NEXS_LIBEXTERNAL_DIR_musl := $(NEXS_LIBEXTERNAL_ROOT)/musl
NEXS_LIBEXTERNAL_DIR_opengl := $(NEXS_LIBEXTERNAL_ROOT)/opengl
NEXS_LIBEXTERNAL_DIR_pl_mpeg := $(NEXS_LIBEXTERNAL_ROOT)/pl_mpeg
NEXS_LIBEXTERNAL_DIR_sdl := $(NEXS_LIBEXTERNAL_ROOT)/sdl
NEXS_LIBEXTERNAL_DIR_stb := $(NEXS_LIBEXTERNAL_ROOT)/stb

NEXS_LIBEXTERNAL_PROVIDER_gnulib := system/lib/portability/gnulib/overlay.mk
