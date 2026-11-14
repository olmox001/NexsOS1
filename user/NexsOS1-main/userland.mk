# Canonical NexsOS first-party userland build manifest.
# External application/library projects are intentionally not listed here.
NEXS_USERLAND_SBIN_PROGRAMS :=
NEXS_USERLAND_SYS_PROGRAMS := \
  nxshell nxres nxreg nxkeymap nxtop nxpower nxbar nxproc nxinfo nxperm \
  nxmemstat nxsettings nxwins nxnotify nxexec nxenvinit nxtimerdiag \
  nxpax nxtestrunner nxroleprobe
NEXS_USERLAND_SERVICE_PROGRAMS :=
NEXS_USERLAND_BIN_PROGRAMS := \
  counter demo3d ipc_send ipc_recv crash writetest input_test nxtest fdtest \
  forkbomb captest notifytest porttest libctest buffertest gfxbuffertest \
  gfxalphatest simdtest simdchild capipc capipc_child portidle_child capreg \
  capkill capkill_child sandboxtest sandboxchild hello stress restest env \
  coreutils_test
NEXS_USERLAND_COMPLEX_PROGRAMS := nxfont
NEXS_USERLAND_MEDIA_COMPLEX_PROGRAMS := nxfilem
NEXS_USERLAND_ALIAS_PROGRAMS := coreutilstest
NEXS_USERLAND_MEDIA_PROGRAMS := nximage nxui nxlauncher

NEXS_USERLAND_CORE_PROGRAMS := $(NEXS_USERLAND_SBIN_PROGRAMS) $(NEXS_USERLAND_SYS_PROGRAMS) \
  $(NEXS_USERLAND_SERVICE_PROGRAMS) $(NEXS_USERLAND_BIN_PROGRAMS) $(NEXS_USERLAND_COMPLEX_PROGRAMS) $(NEXS_USERLAND_ALIAS_PROGRAMS)
NEXS_USERLAND_ALL_PROGRAMS := $(NEXS_USERLAND_CORE_PROGRAMS) $(NEXS_USERLAND_MEDIA_PROGRAMS) $(NEXS_USERLAND_MEDIA_COMPLEX_PROGRAMS)
NEXS_USERLAND_CORE_ELFS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(NEXS_USERLAND_CORE_PROGRAMS)))
NEXS_USERLAND_ALL_ELFS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(NEXS_USERLAND_ALL_PROGRAMS)))

# Single-source first-party programs.
define NEXS_USERLAND_DECLARE
$(BUILD_DIR)/$(1).elf: $(OBJ_DIR)/$(2).o $(NEXS_USERLAND_SDK_LINK_INPUTS)
endef
$(foreach p,$(NEXS_USERLAND_SYS_PROGRAMS),$(eval $(call NEXS_USERLAND_DECLARE,$(p),sys/bin/$(p))))
$(foreach p,$(filter-out simdtest simdchild,$(NEXS_USERLAND_BIN_PROGRAMS)),$(eval $(call NEXS_USERLAND_DECLARE,$(p),bin/$(p))))
$(foreach p,$(NEXS_USERLAND_MEDIA_PROGRAMS),$(eval $(call NEXS_USERLAND_DECLARE,$(p),sys/bin/$(p))))
$(BUILD_DIR)/coreutilstest.elf: $(OBJ_DIR)/bin/coreutils_test.o $(NEXS_USERLAND_SDK_LINK_INPUTS)

$(BUILD_DIR)/simdtest.elf: $(OBJ_DIR)/bin/simdtest.o $(NEXS_USERLAND_SDK_SIMD_OBJECT) $(NEXS_USERLAND_SDK_LINK_INPUTS)
$(BUILD_DIR)/simdchild.elf: $(OBJ_DIR)/bin/simdchild.o $(NEXS_USERLAND_SDK_SIMD_OBJECT) $(NEXS_USERLAND_SDK_LINK_INPUTS)
$(BUILD_DIR)/nxfont.elf: $(OBJ_DIR)/sys/bin/nxfont/nxfont.o $(NEXS_USERLAND_SDK_LINK_INPUTS)
$(BUILD_DIR)/nxfilem.elf: \
  $(OBJ_DIR)/sys/bin/nxfilem/main.o $(OBJ_DIR)/sys/bin/nxfilem/state.o \
  $(OBJ_DIR)/sys/bin/nxfilem/sort.o $(OBJ_DIR)/sys/bin/nxfilem/ui.o \
  $(OBJ_DIR)/sys/bin/nxfilem/draw.o $(OBJ_DIR)/sys/bin/nxfilem/events.o \
  $(OBJ_DIR)/sys/bin/nxfilem/fileops.o $(NEXS_USERLAND_SDK_LINK_INPUTS)
