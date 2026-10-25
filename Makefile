 # Makefile - Tool to setup the GRUB "wrap" module.
 # 
 # Copyright (c) 2026 End0832_.
 # SPDX-License-Identifier: MIT (text in LICENSES/MIT.txt)
 # 
 # Workflow:
 # 1. Get the [grub source code](https://gitlab.freedesktop.org/gnu-grub/grub/) and compile it completely. (WARNING: GRUB compilation can be really long).
 # 2. Download and extract a version of grub-wrap.
 # 3. Run `make link DIR=/path/to/your/compiled/grub/directory`.
 # 4. If everything went right, run `make build DIR=/path/to/your/compiled/grub/directory`.
 # 5. The module has now been added as wrap. You can find the file `wrap.mod` in `grub-core/`.
 # 
 # If needed, you can unlink and delete the module from the grub installation with `make unlink DIR=/path/to/your/compiled/grub/directory`
 # 
 # WARNING: `wrap.mod` must be built from the same GRUB version, for the same platform, as the GRUB that loads it.
 # 
 # For more informations, please check the README.MD file.
 # 

MDIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
CONTRIB_DIR := $(MDIR)/grub-contrib
force ?= 0

.PHONY: link unlink build

link:
	@if [ -z "$(DIR)" ]; then \
		printf '%s\n' "ERROR: DIR is not set." >&2; \
		exit 1; \
	fi; \
	if [ -L "$(CONTRIB_DIR)/wrap" ] && \
	   [ -L "$(DIR)/contrib/wrap" ]; then \
		printf '%s\n' \
			"Wrap module is already linked." \
			"To start over, unlink and retrieve it from grub-contrib:" \
			"make unlink DIR=/path/to/directory" \
			"WARNING: If CONTRIB_DIR changes, update it here too."; \
		exit 0; \
	fi; \
	if [ "$(force)" != "1" ] && \
	   { [ -L "$(DIR)/contrib" ] || \
	     [ -L "$(DIR)/grub-core/contrib" ]; }; then \
		printf '%s\n' \
			"ERROR: An existing symlink was detected." \
			"Other modules may already be linked." \
			"Consider changing CONTRIB_DIR for compatibility." \
			"Use force=1 to continue."; \
		exit 1; \
	fi; \
	rm -rf $(DIR)/contrib $(DIR)/grub-core/contrib && \
	ln -s "$(MDIR)" "$(CONTRIB_DIR)/wrap" && \
	ln -s "$(CONTRIB_DIR)" "$(DIR)/contrib" && \
	ln -s ../contrib "$(DIR)/grub-core/contrib" && \
	cd $(DIR) && python3 gentpl.py grub-core/Makefile.core.def grub-core/Makefile.gcry.def contrib/wrap/Makefile.core.def > grub-core/Makefile.core.am && \
	cd $(MDIR) && rm -rf .deps-core .dirstamp wrap_module-wrap.o

unlink:
	cd $(DIR) && \
	rm -rf contrib grub-core/contrib $(CONTRIB_DIR)/wrap && \
	rm -rf grub-core/wrap.marker \
	    grub-core/wrap.mod \
	    grub-core/wrap.module && \
	python3 gentpl.py grub-core/Makefile.core.def grub-core/Makefile.gcry.def > grub-core/Makefile.core.am

build:
	cd $(DIR) && \
	make -C grub-core wrap.mod
