MODULES := vmx_smoke vmx_transition
obj-m := $(addsuffix .o,$(MODULES))
SHELL := /bin/bash

REPO_ROOT := $(abspath ../../..)
KDIR ?= $(REPO_ROOT)/resources/linux-5.4.49
RESOURCE_KERNEL ?= $(REPO_ROOT)/resources/x86-linux-kernel-5.4.49-1.0.0
KERNEL_CC ?= gcc-13
KERNEL_JOBS ?= $(shell nproc)
KERNEL_WERROR ?= 0

.PHONY: all clean prepare

all: prepare
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(KERNEL_CC) \
		HOSTCC=$(KERNEL_CC) LOCALVERSION= CONFIG_UNWINDER_ORC= \
		SKIP_STACK_VALIDATION=1 WERROR=$(KERNEL_WERROR) modules
	@set -euo pipefail; \
	test -f $(RESOURCE_KERNEL); \
	for module in $(MODULES); do \
		sed -n 's/^[[:space:]]*{ 0x[0-9a-fA-F]*, "\([A-Za-z0-9_]*\)" },$$/\1/p' \
			$(CURDIR)/$$module.mod.c | while read symbol; do \
			crc=$$(nm -n $(RESOURCE_KERNEL) | awk \
				-v target="__crc_$$symbol" \
				'$$3 == target { printf "0x%s", \
				substr($$1, length($$1) - 7) }'); \
			if [ -z "$$crc" ]; then \
				echo "ERROR: $(RESOURCE_KERNEL) has no" \
					"__crc_$$symbol; '$$symbol' is not an" \
					"exported symbol of that exact kernel" \
					"build (e.g. a KASLR-dependent symbol" \
					"that only exists as a variable, not an" \
					"EXPORT_SYMBOL, in some configs)." >&2; \
				exit 1; \
			fi; \
			sed -i -E \
				"s/\\{ 0x[0-9a-fA-F]+, \"$$symbol\" \\}/\\{ $$crc, \"$$symbol\" \\}/" \
				$(CURDIR)/$$module.mod.c; \
			grep -q "{ $$crc, \"$$symbol\" }" \
				$(CURDIR)/$$module.mod.c; \
		done; \
		compile_cmd=$$(sed -n 's/^cmd_.* := //p' \
			$(CURDIR)/.$$module.mod.o.cmd); \
		test -n "$$compile_cmd"; \
		(cd $(KDIR) && eval "$$compile_cmd"); \
		(cd $(KDIR) && ld -r -m elf_x86_64 \
			-z max-page-size=0x200000 --build-id \
			-T ./scripts/module-common.lds \
			-o $(CURDIR)/$$module.ko \
			$(CURDIR)/$$module.o $(CURDIR)/$$module.mod.o); \
	done

prepare:
	@if [ ! -f $(KDIR)/Module.symvers ]; then \
			$(KDIR)/scripts/config --file $(KDIR)/.config \
				--disable UNWINDER_ORC --enable UNWINDER_FRAME_POINTER; \
			$(MAKE) -C $(KDIR) CC=$(KERNEL_CC) HOSTCC=$(KERNEL_CC) \
				LOCALVERSION= SKIP_STACK_VALIDATION=1 \
				WERROR=$(KERNEL_WERROR) olddefconfig; \
			$(MAKE) -C $(KDIR) CC=$(KERNEL_CC) HOSTCC=$(KERNEL_CC) \
				LOCALVERSION= SKIP_STACK_VALIDATION=1 \
				WERROR=$(KERNEL_WERROR) -j$(KERNEL_JOBS); \
		fi
	@$(KDIR)/scripts/config --file $(KDIR)/.config \
		--enable UNWINDER_ORC --disable UNWINDER_FRAME_POINTER
	@$(MAKE) -C $(KDIR) CC=$(KERNEL_CC) HOSTCC=$(KERNEL_CC) \
		LOCALVERSION= CONFIG_UNWINDER_ORC= SKIP_STACK_VALIDATION=1 \
		WERROR=$(KERNEL_WERROR) olddefconfig
	$(MAKE) -C $(KDIR) CC=$(KERNEL_CC) HOSTCC=$(KERNEL_CC) \
		LOCALVERSION= CONFIG_UNWINDER_ORC= SKIP_STACK_VALIDATION=1 \
		WERROR=$(KERNEL_WERROR) modules_prepare
	@set -eu; \
	test -f $(RESOURCE_KERNEL); \
	crc=$$(nm -n $(RESOURCE_KERNEL) | awk \
		'$$3 == "__crc_module_layout" { \
		printf "0x%s", substr($$1, length($$1) - 7) }'); \
	test -n "$$crc"; \
	sed -i -E \
		"s/^0x[0-9a-fA-F]+([[:space:]]+module_layout)/$$crc\\1/" \
		$(KDIR)/Module.symvers

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
