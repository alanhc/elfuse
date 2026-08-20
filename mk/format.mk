# Source formatting

.PHONY: check-format indent

# Tracked source-like files only. Avoid editor/agent worktrees and other
# untracked mirrors under dot-directories.
C_FORMAT_FILES := $(shell git ls-files --cached --others --exclude-standard \
                           -- 'src/**/*.[ch]' 'src/*.[ch]' \
                           'tests/*.c' 'tests/*.h' \
                           'frama-c-stubs/**/*.h' 'frama-c-stubs/*.h')
SHELL_SCRIPTS := $(shell git ls-files --cached --others --exclude-standard \
                         -- '*.sh')
PYTHON_FORMAT_FILES := $(shell git ls-files --cached --others \
                               --exclude-standard -- '*.py')

# Comment reflow. clang-format breaks an over-long comment line but never
# refills a short-wrapped one, so commentflow settles comment width and runs
# first; clang-format then normalizes the indentation it produced. Required
# rather than best-effort, unlike shfmt and black below: a run that skips it
# formats to a different standard than the last one did.
#
# .ci/check-commentflow.sh holds the file list for both this target and the
# gate, so the set rewritten is the set checked.
COMMENTFLOW ?= commentflow

## Check formatting: comments (commentflow) + C (clang-format --dry-run) + shell (shellcheck)
check-format: check-syscall-dispatch
	$(Q)COMMENTFLOW=$(COMMENTFLOW) bash .ci/check-commentflow.sh
	@echo "  FMT     src/ tests/ (check)"
	$(Q)$(CLANG_FORMAT) --dry-run --Werror $(C_FORMAT_FILES)
	@echo "  MATRIX  skip lists"
	$(Q)bash .ci/check-matrix-lists.sh
	$(call require-tool,shellcheck,brew install shellcheck)
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
	$(Q)COMMENTFLOW=$(COMMENTFLOW) bash .ci/check-commentflow.sh --write
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
