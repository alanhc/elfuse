# Static analysis and formatting

.PHONY: lint analyze check-format indent verify verify-elf

CLANG_TIDY ?= clang-tidy

# Tracked source-like files only. Avoid editor/agent worktrees and other
# untracked mirrors under dot-directories.
C_FORMAT_FILES := $(shell git ls-files --cached --others --exclude-standard \
                           -- 'src/**/*.[ch]' 'src/*.[ch]' \
                           'tests/*.c' 'tests/*.h')
SHELL_SCRIPTS := $(shell git ls-files --cached --others --exclude-standard \
                         -- '*.sh')
PYTHON_FORMAT_FILES := $(shell git ls-files --cached --others \
                               --exclude-standard -- '*.py')

## Run clang-tidy on all source files
lint: $(BUILD_DIR)/shim_blob.h $(BUILD_DIR)/version.h
	@echo "  TIDY    src/"
	$(Q)$(CLANG_TIDY) $(SRCS) -- $(CFLAGS) -Isrc -I$(BUILD_DIR)

# Frama-C proof of the ELF parsing core. ELF headers come from untrusted
# binaries, so every offset and extent computed from them is discharged as a
# machine-checked proof rather than reviewed by eye. -wp-rte adds the implicit
# runtime-error goals (overflow, out-of-bounds, invalid dereference) that the
# ACSL contracts alone would not cover.
#
# Model: caveat. The default typed model cannot follow the byte-addressed
# program header buffer, whose entry stride (e_phentsize) is attacker chosen
# and need not match the struct alignment. caveat assumes formal pointer
# parameters do not alias, which the contracts state explicitly via \separated.
# The callers are not in -wp-fct, so nothing checks that they honor it: today
# they pass a malloc'd ph_buf plus distinct stack locals, but a future
# elf_segment_extent(..., &x, &x) would invalidate the proof with no diagnostic.
#
# Install: opam install frama-c, then why3 config detect (without the latter WP
# aborts with "Prover not found" instead of reporting unproved goals).
#
# Data model: Frama-C's -machdep names a C DATA MODEL (type widths, alignment,
# endianness, char signedness), not a code generation target. Nothing here is
# proved "for x86_64" and no x86_64 code is involved; the flag only tells the
# prover how wide a size_t is.
#
# Frama-C 31 ships avr_16, avr_8, gcc_x86_16, gcc_x86_32, gcc_x86_64,
# msvc_x86_64, ppc_32, x86_16, x86_32, x86_64 -- no aarch64 entry at all. Of
# those, gcc_x86_64 is the only one that matches arm64 macOS on the properties
# these proofs rest on:
#
#   property               arm64 macOS   gcc_x86_64   used by the proof?
#   pointer / long / size_t  64-bit        64-bit       yes
#   byte order               little        little       yes
#   uint64_t alignment       8             8            yes
#   plain char signedness    unsigned      signed       see below
#
# Plain-char signedness is the one mismatch, and no proved function here reads a
# plain char: the ELF parser works on uint8_t and fixed-width fields. That is
# the invariant to preserve. Prove a function that reads a plain char without an
# explicit (unsigned char) or (uint8_t) cast first, and supply a custom machdep
# rather than extending this list.
FRAMAC ?= frama-c
FRAMAC_DATA_MODEL ?= gcc_x86_64
FRAMAC_TIMEOUT ?= 30

# The analyzer parses against Frama-C's own modeled libc headers, never the
# host's. -print-share-path runs at recipe time rather than through $(shell) so
# a make invocation with no frama-c installed does not pay for it.
FRAMAC_CPP_ARGS = -nostdinc \
    -isystem $$($(FRAMAC) -print-share-path)/libc -Isrc -I$(BUILD_DIR)

# One proof per attacker-facing parser. Each is declared by a single
# verify_target call below; the recipe itself lives in one place.
#
# MIN_GOALS is a floor on obligations GENERATED. "N of N discharged" is not
# evidence on its own: an emptied function body or a dropped contract proves
# 0 of 0. Raise it when adding proved functions; it is a tripwire, not a target.

VERIFY_ELF_SRC   := src/core/elf.c
VERIFY_ELF_FCTS  := elf_add_no_wrap elf_phdr_gpa_in_segment \
                    elf_phdr_table_bytes elf_phdr_fetch elf_segment_extent
VERIFY_ELF_MIN_GOALS ?= 64
VERIFY_ELF_MODEL := caveat

# Includes utils.h and elf.h: elf.c includes both, and utils.h already carries
# contracts, so scanning only the .c would let one added there become an
# unchecked axiom for this proof.
VERIFY_ELF_SCAN  := src/core/elf.c src/core/elf.h src/utils.h
VERIFY_ELF_CLAIM := for ANY byte sequence an untrusted ELF can supply
VERIFY_ELF_UNPROVED := the pread/malloc I/O around them stays test-covered

# -wp-fct wants one comma-separated argument; the lists stay space-separated so
# the recipe can iterate them for the banner.
verify_empty :=
verify_space := $(verify_empty) $(verify_empty)
verify_comma := ,
commafy = $(subst $(verify_space),$(verify_comma),$(strip $(1)))

# Per-proof values, consumed by the single recipe below through target-specific
# variables. Adding a proof means adding a VERIFY_<NAME>_* block, one line of
# assignments here, and the target name to the shared rule; the recipe itself is
# written once.

## Prove the ELF parser cannot be driven out of bounds by a crafted binary
verify-elf: NAME := elf
verify-elf: SRC := $(VERIFY_ELF_SRC)
verify-elf: FCTS := $(VERIFY_ELF_FCTS)
verify-elf: FCT_ARG := $(call commafy,$(VERIFY_ELF_FCTS))
verify-elf: MIN_GOALS := $(VERIFY_ELF_MIN_GOALS)
verify-elf: MODEL := $(VERIFY_ELF_MODEL)
verify-elf: SCAN := $(VERIFY_ELF_SCAN)
verify-elf: CLAIM := $(VERIFY_ELF_CLAIM)
verify-elf: UNPROVED := $(VERIFY_ELF_UNPROVED)

# One recipe, shared by every verify-* target above. Listing several targets on
# one rule gives each of them this recipe; the target-specific variables select
# what gets proved, so a second proof is a variable block plus a target name.
#
# The recipe only runs the prover. Deciding whether the run counts as a proof
# lives in scripts/check-wp-result.py: as a shell recipe it needed every $
# doubled and every line continued, which put the gate that matters out of
# reach of any test.
verify-elf: | $(BUILD_DIR)
	@command -v $(FRAMAC) >/dev/null 2>&1 || { \
		printf "$(RED)frama-c not found$(RESET) "; \
		printf "(set FRAMAC=, or eval \$$(opam env --switch=<switch>))\n"; \
		exit 1; \
	}
	@python3 scripts/check-acsl-coverage.py --target verify-$(NAME) \
	    --fcts "$(FCTS)" $(SCAN)
	@echo "  PROVE   $(SRC) (Frama-C WP: weakest-precondition prover)"
	@echo "          claim: $(CLAIM),"
	@echo "                 these compute no out-of-bounds access and no overflow"
	@for f in $(FCTS); do echo "                 - $$f"; done
	@echo "          memory model: $(MODEL);  data model: $(FRAMAC_DATA_MODEL)"
	@echo "          (data model is type widths only; Frama-C 31"
	@echo "           has no aarch64 machdep, so this is the LP64 stand-in)"
	@$(FRAMAC) -machdep $(FRAMAC_DATA_MODEL) \
	    -cpp-extra-args="$(FRAMAC_CPP_ARGS)" \
	    $(SRC) -wp -wp-rte -wp-model $(MODEL) \
	    -wp-fct $(FCT_ARG) \
	    -wp-prover alt-ergo,z3 -wp-timeout $(FRAMAC_TIMEOUT) \
	    > $(BUILD_DIR)/verify-$(NAME).log 2>&1; \
	python3 scripts/check-wp-result.py --status $$? \
	    --log $(BUILD_DIR)/verify-$(NAME).log --min-goals $(MIN_GOALS) \
	    --src $(SRC) --unproved "$(UNPROVED)"

## Run every Frama-C proof
verify: verify-elf

## Run clang static analyzer (scan-build)
analyze:
	@echo "  SCAN    elfuse"
	$(Q)scan-build --use-cc=$(CC) $(MAKE) -B elfuse

## Check formatting: C (clang-format --dry-run) + shell (shellcheck)
check-format: check-syscall-dispatch
	@echo "  FMT     src/ tests/ (check)"
	$(Q)$(CLANG_FORMAT) --dry-run --Werror $(C_FORMAT_FILES)
	@echo "  MATRIX  skip lists"
	$(Q)bash .ci/check-matrix-lists.sh
	@printf "  SHCHK   %d scripts\n" $(words $(SHELL_SCRIPTS))
	@fail=0; \
	for f in $(SHELL_SCRIPTS); do \
		if shellcheck --severity=warning "$$f" 2>&1; then \
			printf "  $(GREEN)OK$(RESET) %s\n" "$$f"; \
		else \
			printf "  $(RED)FAIL$(RESET) %s\n" "$$f"; \
			fail=$$((fail + 1)); \
		fi; \
	done; \
	if [ "$$fail" -eq 0 ]; then \
		printf "$(GREEN)All %d scripts pass$(RESET)\n" $(words $(SHELL_SCRIPTS)); \
	else \
		printf "$(RED)%d script(s) have warnings$(RESET)\n" "$$fail"; \
		exit 1; \
	fi

## Indent all C, shell, and Python files in-place
indent: gen-syscall-dispatch
	@echo "  FMT     src/ tests/"
	$(Q)$(CLANG_FORMAT) -i $(C_FORMAT_FILES)
	@if command -v shfmt >/dev/null 2>&1; then \
		printf "  SHFMT   %d scripts\n" $(words $(SHELL_SCRIPTS)); \
		shfmt -w -ln=bash -i 4 -ci -bn -fn -sr $(SHELL_SCRIPTS); \
	fi
	@if command -v black >/dev/null 2>&1 && [ -n "$(PYTHON_FORMAT_FILES)" ]; then \
		printf "  BLACK   %d files\n" $(words $(PYTHON_FORMAT_FILES)); \
		black --quiet $(PYTHON_FORMAT_FILES); \
	fi
