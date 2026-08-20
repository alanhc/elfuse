#!/usr/bin/env bash

# Run cppcheck static analysis on elfuse host source files (src/ only). Tests
# use raw-syscall stubs and are excluded.
#
# CI mode: --max-configs=1 + --enable=warning for speed. Generated headers under
# build/ that the source #includes must exist before invocation:
#   build/dispatch.h    -- via scripts/gen-syscall-dispatch.py
#   build/version.h     -- one-line #define
#   build/shim_blob.h   -- empty stub (the byte array contents are opaque
#                          to cppcheck and produce no useful findings)
#
# Generating real dispatch.h instead of stubbing it keeps cppcheck honest about
# the syscall dispatch layer. Stubbing shim_blob.h is acceptable because the
# file is just a byte array with no callable surface.

set -e -u -o pipefail

CPPCHECK="${CPPCHECK:-cppcheck}"

# Exit codes: 0 clean, 1 findings, 2 the check could not run, anything else the
# analyzer's own failure. 1 is spoken for by --error-exitcode=1 below, so a
# missing binary must not borrow it: an environment problem would then read as a
# defect in the code.
if ! command -v "$CPPCHECK" > /dev/null 2>&1; then
    echo "Error: $CPPCHECK not found" >&2
    exit 2
fi

# Not version-pinned, so report the version: a finding nobody else can reproduce
# is then traceable to the checker rather than to the code.
"$CPPCHECK" --version

SOURCES=()
while IFS= read -rd '' f; do
    SOURCES+=("$f")
done < <(git ls-files -z -- 'src/*.c' 'src/**/*.c')

if [ ${#SOURCES[@]} -eq 0 ]; then
    echo "No tracked C source files found."
    exit 0
fi

BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

python3 scripts/gen-syscall-dispatch.py --output "$BUILD_DIR/dispatch.h"
printf '#define ELFUSE_VERSION "ci"\n' > "$BUILD_DIR/version.h"

# shim_blob.h declares an opaque byte array and its length; cppcheck gains
# nothing from the real contents, so a minimal stub suffices.
cat > "$BUILD_DIR/shim_blob.h" << 'EOF'
static const unsigned char shim_bin[1] = {0};
static const unsigned int shim_bin_len = 1;
EOF

# A hang guard, not a budget. The job it runs in is capped at 10 minutes total
# (see lint.yml), so a value above that would be killed at the job level first
# and the 124 branch below could never report. The sweep measures about 50s on a
# CI runner with -j.
#
# -j is safe here because unusedFunction, the one check cppcheck refuses to run
# in parallel, is not among the enabled ones.
status=0
timeout 300 "$CPPCHECK" \
    -j "$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)" \
    -I. -Isrc -I"$BUILD_DIR" \
    --platform=unix64 \
    --enable=warning \
    --max-configs=1 --error-exitcode=1 --inline-suppr \
    --suppress=checkersReport --suppress=unmatchedSuppression \
    --suppress=missingIncludeSystem --suppress=noValidConfiguration \
    --suppress=normalCheckLevelMaxBranches \
    --suppress=preprocessorErrorDirective \
    --suppress=missingInclude \
    -D_GNU_SOURCE -D__APPLE__ -D__aarch64__ \
    "${SOURCES[@]}" || status=$?

case "$status" in
    0)
        echo "cppcheck clean across ${#SOURCES[@]} source file(s)."
        ;;
    1)
        echo "cppcheck reported findings; see the diagnostics above." >&2
        echo "Suppress a false positive inline, with a comment saying why." >&2
        ;;
    124)
        echo "Error: cppcheck timed out after 300s; this is not a code finding." >&2
        ;;
    *)
        echo "Error: cppcheck exited $status without analyzing cleanly." >&2
        ;;
esac

exit "$status"
