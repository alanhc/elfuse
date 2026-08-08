# Static analysis and formatting

.PHONY: lint analyze check-format indent verify verify-elf verify-rsp \
        verify-gva verify-cmsg verify-fuse verify-stack verify-sockaddr \
        check-contracts infer-uninit

CLANG_TIDY ?= clang-tidy
INFER ?= infer

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
# That caveat is general, and it bites hardest for gva-math.h: guest.c cannot be
# given to Frama-C at all, so nothing here checks that its call sites honor the
# nine `requires` clauses there. check-acsl-coverage.py closes the other
# direction (a contract assumed because its function was left out of -wp-fct);
# it says nothing about preconditions at call sites.
#
# `make check-contracts` narrows that gap from the runtime side: it rebuilds
# with -DELFUSE_CONTRACT_ASSERT so the five expressible clauses are checked on
# every call the suite makes. The four pointer clauses (\valid x3, \separated)
# have no C expression and stay review-only. The checks call the *_args_ok
# predicates in gva-math.h, whose <==> contracts are proved here, so a check
# that drifted weaker than the clause it mirrors fails verify-gva.
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
# Plain-char signedness is the one mismatch, and the RSP proof DOES cover
# functions taking plain char (gdb_hex_pair, gdb_hex_decode, rsp_checksum).
# What keeps the result signedness-independent is not their parameter types but
# that every use of a char value goes through an explicit (unsigned char) or
# (uint8_t) cast before it is compared or accumulated. That is the invariant to
# preserve: no proved function may read a plain char without such a cast. Prove
# one that does, and supply a custom machdep rather than extending this list.
FRAMAC ?= frama-c
FRAMAC_DATA_MODEL ?= gcc_x86_64
FRAMAC_TIMEOUT ?= 30

# The analyzer parses against Frama-C's own modeled libc headers, never the
# host's. -print-share-path runs at recipe time rather than through $(shell) so
# a make invocation with no frama-c installed does not pay for it.

# Per-target preprocessor defines, empty for every target that does not set one.
CPP_DEFS :=

FRAMAC_CPP_ARGS = -nostdinc \
    -isystem $$($(FRAMAC) -print-share-path)/libc -Isrc -I$(BUILD_DIR) \
    $(CPP_DEFS)

# One proof per attacker-facing parser. Each is declared by a single
# verify_target call below; the recipe itself lives in one place.
#
# MIN_GOALS is a floor on obligations GENERATED. "N of N discharged" is not
# evidence on its own: an emptied function body or a dropped contract proves
# 0 of 0. Raise it when adding proved functions; it is a tripwire, not a target.

# Contracts in the shared src/utils.h. Every proof whose source includes that
# header must prove them too, not merely assume them, so this list is appended
# to each such proof's -wp-fct. Proving hex_nibble twice costs a second or two;
# assuming it once is how the RSP proof came to rest on an unchecked axiom.
VERIFY_UTILS_FCTS := hex_nibble

VERIFY_ELF_SRC   := src/core/elf.c
VERIFY_ELF_FCTS  := elf_add_no_wrap elf_phdr_gpa_in_segment \
                    elf_phdr_table_bytes elf_phdr_fetch elf_segment_extent \
                    $(VERIFY_UTILS_FCTS)
VERIFY_ELF_MIN_GOALS ?= 78
VERIFY_ELF_MODEL := caveat

# Includes utils.h and elf.h: elf.c includes both, and utils.h already carries
# contracts, so scanning only the .c would let one added there become an
# unchecked axiom for this proof.
VERIFY_ELF_SCAN  := src/core/elf.c src/core/elf.h src/utils.h
VERIFY_ELF_CLAIM := for ANY byte sequence an untrusted ELF can supply
VERIFY_ELF_UNPROVED := the pread/malloc I/O around them stays test-covered

VERIFY_GVA_SRC   := src/core/gva-math.h
VERIFY_GVA_FCTS  := gva_pt_table_offset gva_leaf_target gva_chunk_clamp \
                    gva_span_ok gva_leaf_target_args_ok \
                    gva_chunk_clamp_args_ok
VERIFY_GVA_MIN_GOALS ?= 66
VERIFY_GVA_MODEL := typed
VERIFY_GVA_SCAN  := src/core/gva-math.h
VERIFY_GVA_CLAIM := for ANY guest address, length, and page-table content
VERIFY_GVA_UNPROVED := the walk and copy loops around them stay test-covered

VERIFY_RSP_SRC   := src/debug/gdbstub-rsp.c
VERIFY_RSP_FCTS  := $(VERIFY_UTILS_FCTS) gdb_hex_pair gdb_hex_decode \
                    gdb_parse_hex rsp_checksum
VERIFY_RSP_MIN_GOALS ?= 98

# typed, not caveat: gdb_hex_decode assigns a pointer RANGE (dst[0 .. len-1]),
# which caveat's flat single-region memory cannot express ("Undefined
# array-size"). elf.c can use caveat because its proved functions assign only
# single locations. Nothing here reinterprets bytes at an attacker-chosen
# stride, which is the reason elf.c needed caveat in the first place.
VERIFY_RSP_MODEL := typed

# Includes utils.h: hex_nibble lives there and the whole RSP proof rests on it.
VERIFY_RSP_SCAN  := src/debug/gdbstub-rsp.c src/utils.h
VERIFY_RSP_CLAIM := for ANY packet bytes a GDB remote can send
VERIFY_RSP_UNPROVED := the socket I/O and framing loop stay test-covered

VERIFY_CMSG_SRC  := src/syscall/cmsg-math.h
VERIFY_CMSG_FCTS := cmsg_entry_bounds
VERIFY_CMSG_MIN_GOALS ?= 17
VERIFY_CMSG_MODEL := typed
VERIFY_CMSG_SCAN := src/syscall/cmsg-math.h
VERIFY_CMSG_CLAIM := for ANY control-message bytes a guest can supply
VERIFY_CMSG_UNPROVED := the walk loop and the host cmsg build stay test-covered

VERIFY_FUSE_SRC  := src/syscall/fuse-math.h
VERIFY_FUSE_FCTS := fuse_frame_count_ok fuse_reply_extent \
                    fuse_clamp_negotiated_write
VERIFY_FUSE_MIN_GOALS ?= 27
VERIFY_FUSE_MODEL := typed
VERIFY_FUSE_SCAN := src/syscall/fuse-math.h
VERIFY_FUSE_CLAIM := for ANY reply frame a guest FUSE daemon can write
VERIFY_FUSE_UNPROVED := the per-opcode payload extents stay test-covered

VERIFY_STACK_SRC  := src/core/stack-math.h
VERIFY_STACK_FCTS := stack_take stack_align_down stack_pushed_words \
                    stack_final_sp
VERIFY_STACK_MIN_GOALS ?= 36
VERIFY_STACK_MODEL := typed
VERIFY_STACK_SCAN := src/core/stack-math.h
VERIFY_STACK_CLAIM := for ANY argv, envp, and auxv set a guest can present
VERIFY_STACK_UNPROVED := the string writes and push loop stay test-covered

VERIFY_SOCKADDR_SRC  := src/syscall/sockaddr-math.h
VERIFY_SOCKADDR_FCTS := sockaddr_len_ok sockaddr_payload_len
VERIFY_SOCKADDR_MIN_GOALS ?= 11
VERIFY_SOCKADDR_MODEL := typed
VERIFY_SOCKADDR_SCAN := src/syscall/sockaddr-math.h
VERIFY_SOCKADDR_CLAIM := for ANY address length a guest or host can present
VERIFY_SOCKADDR_UNPROVED := the family translation and memcpy stay test-covered

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

## Prove guest address translation cannot compute an out-of-bounds window
# Prove with the call-site checks compiled IN. Without this the prover never
# sees the GVA_CONTRACT_ASSERT calls, and a check wired to the wrong predicate
# or handed permuted arguments would show up only as a spurious runtime abort
# under check-contracts. With it, WP must discharge each assert from the very
# requires clause it mirrors, so the wiring is machine-checked too.
verify-gva: CPP_DEFS := -DELFUSE_CONTRACT_ASSERT
verify-gva: NAME := gva
verify-gva: SRC := $(VERIFY_GVA_SRC)
verify-gva: FCTS := $(VERIFY_GVA_FCTS)
verify-gva: FCT_ARG := $(call commafy,$(VERIFY_GVA_FCTS))
verify-gva: MIN_GOALS := $(VERIFY_GVA_MIN_GOALS)
verify-gva: MODEL := $(VERIFY_GVA_MODEL)
verify-gva: SCAN := $(VERIFY_GVA_SCAN)
verify-gva: CLAIM := $(VERIFY_GVA_CLAIM)
verify-gva: UNPROVED := $(VERIFY_GVA_UNPROVED)

## Prove the GDB RSP parser cannot be driven out of bounds by a remote
verify-rsp: NAME := rsp
verify-rsp: SRC := $(VERIFY_RSP_SRC)
verify-rsp: FCTS := $(VERIFY_RSP_FCTS)
verify-rsp: FCT_ARG := $(call commafy,$(VERIFY_RSP_FCTS))
verify-rsp: MIN_GOALS := $(VERIFY_RSP_MIN_GOALS)
verify-rsp: MODEL := $(VERIFY_RSP_MODEL)
verify-rsp: SCAN := $(VERIFY_RSP_SCAN)
verify-rsp: CLAIM := $(VERIFY_RSP_CLAIM)
verify-rsp: UNPROVED := $(VERIFY_RSP_UNPROVED)

## Prove the control-message walk cannot be driven out of the control buffer
verify-cmsg: NAME := cmsg
verify-cmsg: SRC := $(VERIFY_CMSG_SRC)
verify-cmsg: FCTS := $(VERIFY_CMSG_FCTS)
verify-cmsg: FCT_ARG := $(call commafy,$(VERIFY_CMSG_FCTS))
verify-cmsg: MIN_GOALS := $(VERIFY_CMSG_MIN_GOALS)
verify-cmsg: MODEL := $(VERIFY_CMSG_MODEL)
verify-cmsg: SCAN := $(VERIFY_CMSG_SCAN)
verify-cmsg: CLAIM := $(VERIFY_CMSG_CLAIM)
verify-cmsg: UNPROVED := $(VERIFY_CMSG_UNPROVED)

## Prove a hostile FUSE daemon cannot drive a reply past its own frame
verify-fuse: NAME := fuse
verify-fuse: SRC := $(VERIFY_FUSE_SRC)
verify-fuse: FCTS := $(VERIFY_FUSE_FCTS)
verify-fuse: FCT_ARG := $(call commafy,$(VERIFY_FUSE_FCTS))
verify-fuse: MIN_GOALS := $(VERIFY_FUSE_MIN_GOALS)
verify-fuse: MODEL := $(VERIFY_FUSE_MODEL)
verify-fuse: SCAN := $(VERIFY_FUSE_SCAN)
verify-fuse: CLAIM := $(VERIFY_FUSE_CLAIM)
verify-fuse: UNPROVED := $(VERIFY_FUSE_UNPROVED)

## Prove the initial stack stays in its region and lands SP aligned on argc
verify-stack: NAME := stack
verify-stack: SRC := $(VERIFY_STACK_SRC)
verify-stack: FCTS := $(VERIFY_STACK_FCTS)
verify-stack: FCT_ARG := $(call commafy,$(VERIFY_STACK_FCTS))
verify-stack: MIN_GOALS := $(VERIFY_STACK_MIN_GOALS)
verify-stack: MODEL := $(VERIFY_STACK_MODEL)
verify-stack: SCAN := $(VERIFY_STACK_SCAN)
verify-stack: CLAIM := $(VERIFY_STACK_CLAIM)
verify-stack: UNPROVED := $(VERIFY_STACK_UNPROVED)

## Prove sockaddr reshaping cannot overrun either representation
verify-sockaddr: NAME := sockaddr
verify-sockaddr: SRC := $(VERIFY_SOCKADDR_SRC)
verify-sockaddr: FCTS := $(VERIFY_SOCKADDR_FCTS)
verify-sockaddr: FCT_ARG := $(call commafy,$(VERIFY_SOCKADDR_FCTS))
verify-sockaddr: MIN_GOALS := $(VERIFY_SOCKADDR_MIN_GOALS)
verify-sockaddr: MODEL := $(VERIFY_SOCKADDR_MODEL)
verify-sockaddr: SCAN := $(VERIFY_SOCKADDR_SCAN)
verify-sockaddr: CLAIM := $(VERIFY_SOCKADDR_CLAIM)
verify-sockaddr: UNPROVED := $(VERIFY_SOCKADDR_UNPROVED)

# One recipe, shared by every verify-* target above. Listing several targets on
# one rule gives each of them this recipe; the target-specific variables select
# what gets proved.
#
# The recipe only runs the prover. Deciding whether the run counts as a proof
# lives in scripts/check-wp-result.py: as a shell recipe it needed every $
# doubled and every line continued, which put the gate that matters out of
# reach of any test.
verify-elf verify-rsp verify-gva verify-cmsg verify-fuse verify-stack \
    verify-sockaddr: | $(BUILD_DIR)
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
verify: verify-elf verify-gva verify-rsp verify-cmsg verify-fuse verify-stack \
        verify-sockaddr

## Rebuild with the gva-math.h precondition checks live, then run the suite
#
# Separate from `make check` rather than folded into it: gva_leaf_target and
# gva_chunk_clamp sit on the guest_read / guest_write hot path, and the tree has
# no NDEBUG release split that would compile the checks out again.
#
# Builds into its own directory rather than rebuilding in place. Sharing
# $(BUILD_DIR) would leave instrumented objects behind, and a later `make
# elfuse` would silently relink them: a default-looking binary carrying the
# checks on its hot path. A separate tree also means no -B is needed, since it
# starts empty.
check-contracts:
	@echo "  CONTRACT gva-math.h call-site preconditions (5 of 9 clauses)"
	$(Q)$(MAKE) BUILD_DIR=$(BUILD_DIR)/contracts \
	    EXTRA_CFLAGS="-DELFUSE_CONTRACT_ASSERT $(EXTRA_CFLAGS)" check

## Re-run Infer with the uninitialized-value checker that .inferconfig disables
infer-uninit: | $(BUILD_DIR)
	@command -v $(INFER) >/dev/null 2>&1 || { \
		printf "  $(RED)infer not found$(RESET) (set INFER=)\n"; exit 1; \
	}
	@echo "  INFER   uninitialized-value checker (disabled in .inferconfig)"
	@echo "          A count of 0 means the suppression is no longer needed and"
	@echo "          .inferconfig should be deleted. Anything else is the known"
	@echo "          false-positive class: Pulse cannot prove guest_copy's"
	@echo "          chunked loop fills its destination."
	@status=0; \
	$(INFER) run --keep-going --enable-issue-type PULSE_UNINITIALIZED_VALUE \
	    --results-dir $(BUILD_DIR)/infer-uninit \
	    -- $(MAKE) -B elfuse > $(BUILD_DIR)/infer-uninit.log 2>&1 || status=$$?; \
	if [ "$$status" -ne 0 ] && [ "$$status" -ne 2 ]; then \
		printf "  $(RED)FAILED$(RESET)   infer exited %s; this is an analysis\n" "$$status"; \
		printf "           failure, not an audit result. See $(BUILD_DIR)/infer-uninit.log\n"; \
		exit 1; \
	fi; \
	if [ ! -s $(BUILD_DIR)/infer-uninit/report.json ]; then \
		printf "  $(RED)FAILED$(RESET)   infer produced no report\n"; exit 1; \
	fi; \
	python3 -c "import json,sys; \
	    d=json.load(open('$(BUILD_DIR)/infer-uninit/report.json')); \
	    u=[x for x in d if x['bug_type']=='PULSE_UNINITIALIZED_VALUE']; \
	    print('  %d PULSE_UNINITIALIZED_VALUE finding(s) across %d file(s)' \
	          % (len(u), len({x['file'] for x in u})))"

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
