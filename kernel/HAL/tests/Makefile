SHELL := /bin/sh
CC := clang
PYTHON ?= python3
CLANG_TIDY ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy) clang-tidy)
CPPCHECK ?= cppcheck
ROOT := $(abspath ..)
BUILD := $(CURDIR)/build
UNIT := $(ROOT)/tests/unit
CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror -Wpedantic
CROSS_CFLAGS := -std=c17 -ffreestanding -fno-stack-protector -fno-builtin \
	-Wall -Wextra -Werror -Wpedantic
COMMON := \
	$(ROOT)/common/boot_memory_record.c \
	$(ROOT)/common/hcd_crc64.c \
	$(ROOT)/common/hcd_parser.c \
	$(ROOT)/common/hcd_semantics.c \
	$(ROOT)/common/hcd_section_catalog.c \
	$(ROOT)/common/hcd_builder.c \
	$(ROOT)/common/memory_plan.c
CAPABILITY := \
	$(ROOT)/capability/isa.c \
	$(ROOT)/capability/mmu.c \
	$(ROOT)/capability/hw_access_policy.c \
	$(ROOT)/capability/uefi_exit.c \
	$(ROOT)/capability/uefi_memory_map.c \
	$(ROOT)/capability/multiboot2_info.c \
	$(ROOT)/capability/multiboot2_efi_map.c \
	$(ROOT)/capability/multiboot2_memory_map.c

.PHONY: test test-host test-cross test-sanitize test-static clean
test: test-host test-cross test-sanitize test-static

$(BUILD):
	@mkdir -p $@

define host_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(1) $(UNIT)/$(2).c -o $(BUILD)/$(2)
	$(BUILD)/$(2)
endef

test-host: | $(BUILD)
	$(call host_test,$(ROOT)/common/boot_memory_record.c,boot_memory_record_test)
	$(call host_test,$(CAPABILITY),isa_snapshot_test)
	$(call host_test,$(CAPABILITY),isa_mmu_test)
	$(call host_test,$(CAPABILITY),hw_access_policy_test)
	$(call host_test,$(ROOT)/capability/uefi_exit.c,uefi_exit_test)
	$(call host_test,$(CAPABILITY),uefi_memory_map_test)
	$(call host_test,$(CAPABILITY),multiboot2_memory_map_test)
	$(call host_test,$(CAPABILITY),multiboot2_efi_map_test)
	$(call host_test,$(ROOT)/common/hcd_crc64.c,hcd_crc64_test)
	$(call host_test,$(COMMON),hcd_parser_test)
	$(call host_test,$(COMMON),hcd_core_profile_test)
	$(call host_test,$(COMMON),hcd_builder_test)
	$(call host_test,$(COMMON),memory_plan_test)
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/fault_protocol.c $(ROOT)/core/nxpax.c $(UNIT)/fault_protocol_test.c -o $(BUILD)/fault_protocol_test
	$(BUILD)/fault_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT)/include $(ROOT)/core/nxpax.c $(UNIT)/nxpax_test.c -o $(BUILD)/nxpax_test
	$(BUILD)/nxpax_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/diagnostic_protocol.c $(UNIT)/diagnostic_protocol_test.c -o $(BUILD)/diagnostic_protocol_test
	$(BUILD)/diagnostic_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/timer_protocol.c $(UNIT)/timer_protocol_test.c -o $(BUILD)/timer_protocol_test
	$(BUILD)/timer_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/smp_protocol.c $(UNIT)/smp_protocol_test.c -o $(BUILD)/smp_protocol_test
	$(BUILD)/smp_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/device_protocol.c $(UNIT)/device_protocol_test.c -o $(BUILD)/device_protocol_test
	$(BUILD)/device_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/irq.c $(UNIT)/irq_protocol_test.c -o $(BUILD)/irq_protocol_test
	$(BUILD)/irq_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/irq_mask_stats.c $(UNIT)/irq_mask_stats_test.c -o $(BUILD)/irq_mask_stats_test
	$(BUILD)/irq_mask_stats_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/fatal_protocol.c $(UNIT)/fatal_protocol_test.c -o $(BUILD)/fatal_protocol_test
	$(BUILD)/fatal_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/memory_protocol.c $(UNIT)/memory_protocol_test.c -o $(BUILD)/memory_protocol_test
	$(BUILD)/memory_protocol_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/time.c $(UNIT)/time_test.c -o $(BUILD)/time_test
	$(BUILD)/time_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/entropy.c $(UNIT)/entropy_test.c -o $(BUILD)/entropy_test
	$(BUILD)/entropy_test
	$(CC) $(CFLAGS) -I$(ROOT)/include $(ROOT)/core/resource.c $(ROOT)/base/checked.c $(UNIT)/resource_test.c -o $(BUILD)/resource_test
	$(BUILD)/resource_test
	$(CC) $(CFLAGS) -I$(ROOT)/include $(UNIT)/resource_abi_test.c -o $(BUILD)/resource_abi_test
	$(BUILD)/resource_abi_test
	$(CC) $(CFLAGS) -I$(ROOT)/include $(ROOT)/base/checked.c $(UNIT)/checked_test.c -o $(BUILD)/checked_test
	$(BUILD)/checked_test
	@set -eu; for isa in amd64 aarch64; do \
	  $(CC) $(CFLAGS) -I$(ROOT)/include -I$(ROOT)/../include -I$(ROOT)/../include/abi -I$(ROOT)/../include/api \
	    -I$(ROOT)/arch/$$isa/include $(ROOT)/arch/$$isa/cpu/frame.c \
	    $(UNIT)/frame_$${isa}_test.c -o $(BUILD)/frame_$${isa}_test; \
	  $(BUILD)/frame_$${isa}_test; \
	done
	$(CC) $(CFLAGS) -I$(ROOT)/include $(ROOT)/core/resource.c $(ROOT)/base/checked.c $(UNIT)/resource_mutation_test.c -o $(BUILD)/resource_mutation_test
	$(BUILD)/resource_mutation_test
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/pci.c $(UNIT)/pci_test.c -o $(BUILD)/pci_test
	$(BUILD)/pci_test
	$(CC) $(CFLAGS) -I$(ROOT)/include $(ROOT)/core/device_name.c $(UNIT)/device_name_test.c -o $(BUILD)/device_name_test
	$(BUILD)/device_name_test
	$(CC) $(CFLAGS) -I$(ROOT) $(UNIT)/hcd_layout_test.c -o $(BUILD)/hcd_layout_test
	$(BUILD)/hcd_layout_test
	@printf '%s\n' "PASS: NX-HAL integrated NXEC host tests"

test-cross: | $(BUILD)
	$(PYTHON) $(ROOT)/tools/gen-hcd-crc-table.py | cmp - $(ROOT)/common/hcd_crc64_table.inc
	@set -eu; \
	for target in x86_64-elf aarch64-none-elf; do \
	  objects=""; \
	  n=0; \
	  for source in $(COMMON) $(CAPABILITY); do \
	    n=$$((n+1)); object="$(BUILD)/protocol-$$n-$$target.o"; \
	    $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT) -I$(ROOT)/include -c "$$source" -o "$$object"; \
	    objects="$$objects $$object"; \
	  done; \
	  case "$$target" in \
	    x86_64-elf) adapter=$(ROOT)/capability/amd64/isa.c; adapter_include=$(ROOT)/arch/amd64/include; linker=x86_64-elf-ld ;; \
	    aarch64-none-elf) adapter=$(ROOT)/capability/arm64/isa.c; adapter_include=$(ROOT)/arch/aarch64/include; linker=aarch64-none-elf-ld ;; \
	  esac; \
	  adapter_object="$(BUILD)/isa-adapter-$$target.o"; \
	  $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT) -I"$$adapter_include" -c "$$adapter" -o "$$adapter_object"; \
	  objects="$$objects $$adapter_object"; \
	  "$$linker" -r $$objects -o "$(BUILD)/nxhal-protocol-$$target.o"; \
	  unresolved=$$(nm -u "$(BUILD)/nxhal-protocol-$$target.o"); \
	  if [ -n "$$unresolved" ]; then \
	    printf 'UNRESOLVED NX-HAL PROTOCOL SYMBOLS (%s):\n%s\n' "$$target" "$$unresolved" >&2; exit 1; \
	  fi; \
	  $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT) -c $(UNIT)/hcd_layout_test.c \
	    -o "$(BUILD)/hcd-layout-$$target.o"; \
	  $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT)/include -c $(ROOT)/core/resource.c \
	    -o "$(BUILD)/resource-core-$$target.o"; \
  $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT)/include -c $(ROOT)/base/checked.c \
	    -o "$(BUILD)/resource-checked-$$target.o"; \
	  $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT)/include -c $(UNIT)/resource_abi_test.c \
	    -o "$(BUILD)/resource-abi-$$target.o"; \
	  $(CC) $(CROSS_CFLAGS) -target "$$target" -I$(ROOT) -I$(ROOT)/include -c $(ROOT)/core/pci.c \
	    -o "$(BUILD)/pci-core-$$target.o"; \
	  "$$linker" -r "$(BUILD)/resource-core-$$target.o" "$(BUILD)/resource-abi-$$target.o" "$(BUILD)/resource-checked-$$target.o" \
	    -o "$(BUILD)/nxhal-resource-$$target.o"; \
	  unresolved=$$(nm -u "$(BUILD)/nxhal-resource-$$target.o"); \
	  if [ -n "$$unresolved" ]; then \
	    printf 'UNRESOLVED NX-HAL RESOURCE SYMBOLS (%s):\n%s\n' "$$target" "$$unresolved" >&2; exit 1; \
	  fi; \
	  printf 'PASS: NX-HAL protocol/resource freestanding cross-link %s\n' "$$target"; \
	done

test-sanitize: | $(BUILD)
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include \
	  $(ROOT)/common/boot_memory_record.c $(UNIT)/boot_memory_record_test.c \
	  -o $(BUILD)/boot_memory_record_test-sanitize
	$(BUILD)/boot_memory_record_test-sanitize
	@set -eu; \
	for test in isa_snapshot_test isa_mmu_test hw_access_policy_test \
	            uefi_memory_map_test multiboot2_memory_map_test \
	            multiboot2_efi_map_test; do \
	  $(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(CAPABILITY) \
	    "$(UNIT)/$$test.c" -o "$(BUILD)/$$test-sanitize"; \
	  "$(BUILD)/$$test-sanitize"; \
	done
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/capability/uefi_exit.c \
	  $(UNIT)/uefi_exit_test.c -o $(BUILD)/uefi_exit_test-sanitize
	$(BUILD)/uefi_exit_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/fault_protocol.c $(ROOT)/core/nxpax.c \
	  $(UNIT)/fault_protocol_test.c -o $(BUILD)/fault_protocol_test-sanitize
	$(BUILD)/fault_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT)/include \
	  $(ROOT)/core/nxpax.c $(UNIT)/nxpax_test.c -o $(BUILD)/nxpax_test-sanitize
	$(BUILD)/nxpax_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/diagnostic_protocol.c \
	  $(UNIT)/diagnostic_protocol_test.c -o $(BUILD)/diagnostic_protocol_test-sanitize
	$(BUILD)/diagnostic_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/timer_protocol.c \
	  $(UNIT)/timer_protocol_test.c -o $(BUILD)/timer_protocol_test-sanitize
	$(BUILD)/timer_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/smp_protocol.c \
	  $(UNIT)/smp_protocol_test.c -o $(BUILD)/smp_protocol_test-sanitize
	$(BUILD)/smp_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/device_protocol.c \
	  $(UNIT)/device_protocol_test.c -o $(BUILD)/device_protocol_test-sanitize
	$(BUILD)/device_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/irq.c \
	  $(UNIT)/irq_protocol_test.c -o $(BUILD)/irq_protocol_test-sanitize
	$(BUILD)/irq_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/irq_mask_stats.c \
	  $(UNIT)/irq_mask_stats_test.c -o $(BUILD)/irq_mask_stats_test-sanitize
	$(BUILD)/irq_mask_stats_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/fatal_protocol.c \
	  $(UNIT)/fatal_protocol_test.c -o $(BUILD)/fatal_protocol_test-sanitize
	$(BUILD)/fatal_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/memory_protocol.c \
	  $(UNIT)/memory_protocol_test.c -o $(BUILD)/memory_protocol_test-sanitize
	$(BUILD)/memory_protocol_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/time.c \
	  $(UNIT)/time_test.c -o $(BUILD)/time_test-sanitize
	$(BUILD)/time_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/entropy.c \
	  $(UNIT)/entropy_test.c -o $(BUILD)/entropy_test-sanitize
	$(BUILD)/entropy_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT)/include $(ROOT)/core/resource.c \
	  $(ROOT)/base/checked.c $(UNIT)/resource_test.c -o $(BUILD)/resource_test-sanitize
	$(BUILD)/resource_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT)/include $(ROOT)/core/resource.c \
	  $(ROOT)/base/checked.c $(UNIT)/resource_mutation_test.c -o $(BUILD)/resource_mutation_test-sanitize
	$(BUILD)/resource_mutation_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT)/include $(ROOT)/base/checked.c \
	  $(UNIT)/checked_test.c -o $(BUILD)/checked_test-sanitize
	$(BUILD)/checked_test-sanitize
	@set -eu; for isa in amd64 aarch64; do \
	  $(CC) -std=c17 -O1 -g -fsanitize=address,undefined \
	    -fno-omit-frame-pointer -Wall -Wextra -Werror -Wpedantic \
	    -I$(ROOT)/include -I$(ROOT)/../include -I$(ROOT)/../include/abi -I$(ROOT)/../include/api \
	    -I$(ROOT)/arch/$$isa/include $(ROOT)/arch/$$isa/cpu/frame.c \
	    $(UNIT)/frame_$${isa}_test.c -o $(BUILD)/frame_$${isa}_test-sanitize; \
	  $(BUILD)/frame_$${isa}_test-sanitize; \
	done
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(ROOT)/core/pci.c \
	  $(UNIT)/pci_test.c -o $(BUILD)/pci_test-sanitize
	$(BUILD)/pci_test-sanitize
	$(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Werror -Wpedantic -I$(ROOT)/include $(ROOT)/core/device_name.c \
	  $(UNIT)/device_name_test.c -o $(BUILD)/device_name_test-sanitize
	$(BUILD)/device_name_test-sanitize
	@set -eu; \
	for test in memory_plan_test hcd_builder_test hcd_core_profile_test hcd_parser_test; do \
	  $(CC) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Wall -Wextra -Werror -Wpedantic -I$(ROOT) -I$(ROOT)/include $(COMMON) \
	    "$(UNIT)/$$test.c" -o "$(BUILD)/$$test-sanitize"; \
	  "$(BUILD)/$$test-sanitize"; \
	done
	@printf '%s\n' "PASS: NX-HAL integrated ASan/UBSan"

test-static:
	$(CLANG_TIDY) $(COMMON) $(CAPABILITY) $(ROOT)/core/fault_protocol.c $(ROOT)/core/diagnostic_protocol.c $(ROOT)/core/timer_protocol.c $(ROOT)/core/smp_protocol.c $(ROOT)/core/device_protocol.c $(ROOT)/core/irq.c $(ROOT)/core/fatal_protocol.c $(ROOT)/core/memory_protocol.c $(ROOT)/core/time.c $(ROOT)/core/entropy.c $(ROOT)/base/checked.c $(ROOT)/core/resource.c $(ROOT)/core/pci.c $(ROOT)/core/device_name.c \
	  --checks='clang-analyzer-*,bugprone-*,cert-*,misc-*,performance-*,portability-*,-bugprone-easily-swappable-parameters' \
	  --warnings-as-errors='*' -- $(CROSS_CFLAGS) -I$(ROOT) -I$(ROOT)/include
	$(CLANG_TIDY) $(ROOT)/capability/amd64/isa.c --extra-arg=--target=x86_64-elf \
	  --checks='clang-analyzer-*,bugprone-*,cert-*,misc-*,performance-*,portability-*,-bugprone-easily-swappable-parameters,-portability-no-assembler' \
	  --warnings-as-errors='*' -- $(CROSS_CFLAGS) -I$(ROOT) -I$(ROOT)/arch/amd64/include
	$(CLANG_TIDY) $(ROOT)/capability/arm64/isa.c --extra-arg=--target=aarch64-none-elf \
	  --checks='clang-analyzer-*,bugprone-*,cert-*,misc-*,performance-*,portability-*,-bugprone-easily-swappable-parameters,-portability-no-assembler' \
	  --warnings-as-errors='*' -- $(CROSS_CFLAGS) -I$(ROOT) -I$(ROOT)/arch/aarch64/include
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 \
	  -I$(ROOT) -I$(ROOT)/include $(COMMON) $(CAPABILITY) $(ROOT)/core/fault_protocol.c $(ROOT)/core/diagnostic_protocol.c $(ROOT)/core/timer_protocol.c $(ROOT)/core/smp_protocol.c $(ROOT)/core/device_protocol.c $(ROOT)/core/irq.c $(ROOT)/core/fatal_protocol.c $(ROOT)/core/memory_protocol.c $(ROOT)/core/time.c $(ROOT)/core/entropy.c $(ROOT)/base/checked.c $(ROOT)/core/resource.c $(ROOT)/core/pci.c $(ROOT)/core/device_name.c \
	  $(UNIT)/*.c
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 \
	  -I$(ROOT) -I$(ROOT)/arch/amd64/include $(ROOT)/capability/amd64/isa.c
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 \
	  -I$(ROOT) -I$(ROOT)/arch/aarch64/include $(ROOT)/capability/arm64/isa.c

clean:
	rm -rf $(BUILD)
