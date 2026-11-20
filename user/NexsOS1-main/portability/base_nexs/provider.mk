# Integrated external Base-NEXS interpreter provider for the NexsOS1 image.
# Its own Makefile and NX-SDK HAL build the program: do not copy or relink
# vendor implementation sources in the first-party userland.
NEXS_BASE_NEXS_DIR := $(NEXS_BINEXTERNAL_DIR_base_nexs)
NEXS_BASE_NEXS_ARCH := $(if $(filter aarch64,$(ARCH)),arm64,amd64)
NEXS_BASE_NEXS_ELF := $(NEXS_BASE_NEXS_DIR)/build/os1-$(NEXS_BASE_NEXS_ARCH)/nxscript.elf
NEXS_BASE_NEXS_SDK_ARCHIVE := $(NEXS_BASE_NEXS_DIR)/build/sdk-os1/$(ARCH)/libnxsdk-core.a

# Track every production C, header and source-manifest input of the provider.
# Its example/service data are separate image prerequisites below, not
# triggers for an unnecessary native compiler/linker invocation.
NEXS_BASE_NEXS_BUILD_INPUTS := $(shell find     $(addprefix $(NEXS_BASE_NEXS_DIR)/,core registry lang runtime sys compiler hal kernel include)     -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)     $(NEXS_BASE_NEXS_DIR)/Makefile $(NEXS_BASE_NEXS_DIR)/config/sources.mk
NEXS_BASE_NEXS_DATA_INPUTS := $(shell find     $(NEXS_BASE_NEXS_DIR)/example $(NEXS_BASE_NEXS_DIR)/services     $(NEXS_BASE_NEXS_DIR)/modules     -type f 2>/dev/null | sed 's/ /\\ /g')

$(NEXS_BASE_NEXS_ELF): $(NEXS_BASE_NEXS_BUILD_INPUTS)                        $(NEXS_BINEXTERNAL_PROVIDER_base_nexs) $(USER_LIB_O)
	+$(MAKE) -C $(NEXS_BASE_NEXS_DIR) os1-$(NEXS_BASE_NEXS_ARCH)
	@test -s "$@" && test -s "$(NEXS_BASE_NEXS_SDK_ARCHIVE)"
