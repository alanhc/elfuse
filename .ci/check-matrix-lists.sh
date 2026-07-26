#!/usr/bin/env bash

# Keep the test-matrix skip lists honest.
#
# tests/test-matrix.sh runs each test under two runners and carries a skip list
# per runner: QEMU_SKIP for tests the reference kernel cannot adjudicate,
# ELFUSE_SKIP for tests that need a writable byte-exact root the elfuse lane
# does not have. Both lists are matched against a test's label by string, and a
# string that matches nothing fails silently: the suite still reports success,
# having run one test fewer than the reader believes.
#
# Two ways that goes wrong, both of which cost coverage without costing a red
# build, and neither of which the suite itself can notice:
#
#   1. A label in a skip list that no longer names a registered test. Dead
#      config: it reads as deliberate coverage policy while guarding nothing,
#      and it hides the rename that orphaned it.
#   2. A label in both lists at once. The test is then skipped under every
#      runner the matrix has, so it never executes anywhere while still looking
#      registered.
#
# The pass counts themselves are not checked here. test-matrix.sh already holds
# each lane to its EXPECTED_BASELINES floor at runtime, which is a stronger
# check than anything static, and duplicating it would only add a second number
# to keep in step.

set -e -u -o pipefail

MATRIX="${1:-$(dirname "$0")/../tests/test-matrix.sh}"

if [ ! -r "$MATRIX" ]; then
    echo "check-matrix-lists: cannot read $MATRIX" >&2
    exit 2
fi

# Body of a NAME="..." block spanning lines, one entry per line.
list_entries()
{
    sed -n "/^$1=\"/,/^\"\$/p" "$MATRIX" | sed '1d;$d' | tr -s ' \t' '\n' \
        | grep -v '^$' || true
}

# Labels registered with the test_* wrappers: the argument after "$runner".
registered_labels()
{
    grep -oE '\btest_(check|rc|pipe) +"\$runner" +"[^"]+"' "$MATRIX" \
        | sed -E 's/.*"\$runner" +"([^"]+)"$/\1/' | sort -u
}

ret=0
registered="$(registered_labels)"

for list in QEMU_SKIP ELFUSE_SKIP; do
    while IFS= read -r label; do
        [ -n "$label" ] || continue
        if ! printf '%s\n' "$registered" | grep -qxF "$label"; then
            echo "Error: $list names '$label', which no test_* call registers" >&2
            ret=1
        fi
    done < <(list_entries "$list")
done

while IFS= read -r label; do
    [ -n "$label" ] || continue
    if list_entries QEMU_SKIP | grep -qxF "$label"; then
        echo "Error: '$label' is in both QEMU_SKIP and ELFUSE_SKIP, so it never runs" >&2
        ret=1
    fi
done < <(list_entries ELFUSE_SKIP)

exit $ret
