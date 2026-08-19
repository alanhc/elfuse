# mk/common.mk -- Generic build rules
#
# Per-file compilation with automatic dependency tracking, verbosity
# control, and kernel-style build output.  Inspired by libiui's build
# infrastructure.

# Verbosity: make V=1 shows full commands
ifeq ($(V),1)
    Q :=
else
    Q := @
    MAKEFLAGS += --no-print-directory
endif

$(BUILD_DIR):
	@mkdir -p $@

# Automatic header dependency generation (-MMD -MP)
DEPFLAGS = -MMD -MP -MF $(BUILD_DIR)/$(subst /,_,$*).d

# Build flavor guard.
#
# Objects compiled with -fsanitize=... cannot be linked into a binary built
# without it, and the failure surfaces as a wall of undefined
# ___asan_version_mismatch symbols that names neither cause nor cure. The
# sanitizer targets clean before they build, but nothing cleans after them, so
# the next ordinary "make elfuse" walks into it. Record the flavor and wipe the
# tree when it changes: the rebuild was unavoidable either way, since every
# object is instrumented differently, so this costs nothing beyond making the
# reason legible.
#
# Evaluated while the makefile is read rather than as a target, because the
# sanitizer lanes reach $(ELFUSE_BIN) through their own prerequisites and never
# run a phony guard hung off the elfuse target. Getting that wrong is worse than
# no guard: the stamp stays stale, the link succeeds, and an instrumented binary
# is handed back to a caller who asked for a plain one.
BUILD_FLAVOR := $(strip $(CFLAGS))
BUILD_FLAVOR_STAMP := $(BUILD_DIR)/.build-flavor

# Goals that build nothing, so nothing needs the tree in a known flavor. A
# goal-less invocation is one of them: .DEFAULT_GOAL is help, while
# MAKECMDGOALS stays empty, so filtering the skip list out of the goals and
# testing what remains covers both that and a mixed "make help elfuse".
BUILD_FLAVOR_GOALS := $(filter-out clean distclean help,$(MAKECMDGOALS))

ifneq ($(BUILD_FLAVOR_GOALS),)
BUILD_FLAVOR_PREV := $(shell cat $(BUILD_FLAVOR_STAMP) 2>/dev/null)

# What matters is whether objects exist that this build cannot use, so the two
# ways that happens are separated. A stamp that disagrees is one. No stamp at
# all beside objects is the other, since a tree written before this guard
# existed records nothing and its objects carry unknown flags. An empty tree
# with no stamp is neither, and treating it as a mismatch is what deleted
# scan-build's report directory on every fresh checkout.
BUILD_FLAVOR_OBJS := $(wildcard $(BUILD_DIR)/*.o $(BUILD_DIR)/*/*.o)

ifneq ($(BUILD_FLAVOR_OBJS),)
ifeq ($(BUILD_FLAVOR_PREV),)
$(info   FLAVOR  build/ holds objects with no record of their CFLAGS, so they)
$(info           are being removed)
BUILD_FLAVOR_STALE := 1
else ifneq ($(BUILD_FLAVOR),$(BUILD_FLAVOR_PREV))
$(info   FLAVOR  build/ holds objects compiled with different CFLAGS, so they)
$(info           are being removed. Was: [$(BUILD_FLAVOR_PREV)])
$(info           Now: [$(BUILD_FLAVOR)])
BUILD_FLAVOR_STALE := 1
endif
endif

ifdef BUILD_FLAVOR_STALE
# Only the objects and their dependency files, not the directory. Other tools
# put their own output under build/ and are entitled to keep it: scan-build
# creates its report directory and then invokes make, so removing the tree here
# deletes the directory it is writing into and the analyzer dies on the next
# translation unit.
#
# What survived is the question, not how find reported it. Both of find's
# answers are to a different one: -delete descends into that same report
# directory, so an unreadable entry there exits non-zero with every removal
# having succeeded, and a removal that failed silently is invisible to stderr.
# Stopping is the point. Carrying on writes a stamp claiming a flavor the
# leftover objects do not have.
BUILD_FLAVOR_RM := $(shell find $(BUILD_DIR) \( -name '*.o' -o -name '*.d' \) -delete 2>/dev/null; \
    find $(BUILD_DIR) \( -name '*.o' -o -name '*.d' \) 2>/dev/null | head -3)
ifneq ($(BUILD_FLAVOR_RM),)
$(error FLAVOR: stale objects under $(BUILD_DIR) survived removal: $(BUILD_FLAVOR_RM))
endif
endif

$(shell mkdir -p $(BUILD_DIR) && printf '%s' '$(BUILD_FLAVOR)' > $(BUILD_FLAVOR_STAMP))
endif

# Pattern rules -- source to object.
# GENERATED_HEADERS are order-only prerequisites so clean builds have the
# build-generated includes available before compilation. .d files track the
# real header dependencies after the first compile. Generators whose output
# triggers a rebuild on input change (e.g., build/dispatch.h from
# src/syscall/dispatch.tbl) use explicit normal prerequisites where needed.

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) $(GENERATED_HEADERS)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) -I$(BUILD_DIR) -Isrc -c -o $@ $<

$(BUILD_DIR)/%.o: tests/%.c | $(BUILD_DIR) $(GENERATED_HEADERS)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) -I$(BUILD_DIR) -Isrc -c -o $@ $<

# Include generated dependency files (silently skip on first build)
-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: clean distclean

## Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)

## Remove build artifacts plus downloaded test fixtures (Alpine packages, kernel, initramfs, busybox)
distclean: clean
	rm -rf externals/test-fixtures
