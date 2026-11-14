NEXS_DEMO1_PROGRAMS := demo1shell
NEXS_DEMO1_ELFS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(NEXS_DEMO1_PROGRAMS)))

$(BUILD_DIR)/demo1shell.elf: $(OBJ_DIR)/sys/bin/demo1shell.o $(DEMO_SDK_LINK_INPUTS)
