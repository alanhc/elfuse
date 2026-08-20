#!/usr/bin/env bash

# Verify test-config.sh distinguishes execution from sourcing.
#
# Copyright 2026 elfuse contributors
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="$SCRIPT_DIR/test-config.sh"
HEADER="$SCRIPT_DIR/../src/elfuse-limits.h"

expected="$(awk '
    $1 == "#define" && $2 == "FD_TABLE_SIZE" { fd = $3 }
    $1 == "#define" && $2 == "HOST_FD_RESERVE" { reserve = $3 }
    END {
        if (fd == "" || reserve == "")
            exit 1
        print fd + reserve
    }
' "$HEADER")"

direct="$(env -u ELFUSE_HOST_NOFILE_MIN bash "$CONFIG" --host-nofile)"
if [ "$direct" != "$expected" ]; then
    printf 'test-config: direct CLI returned %s (want %s)\n' \
        "$direct" "$expected" >&2
    exit 1
fi

# A sourced helper must not interpret the caller's positional parameters as its
# own. This reproduces the collision that previously printed an extra line when
# the caller's first argument was --host-nofile.
sourced="$(CONFIG_PATH="$CONFIG" env -u ELFUSE_HOST_NOFILE_MIN bash -c '
    set -e
    set -- --host-nofile
    source "$CONFIG_PATH"
    printf "%s\n" sourced
')"
if [ "$sourced" != "sourced" ]; then
    printf 'test-config: sourcing leaked CLI output: %s\n' "$sourced" >&2
    exit 1
fi

printf 'test-config CLI/source guard: PASS\n'
