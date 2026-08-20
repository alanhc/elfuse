#!/usr/bin/env bash

# Security checks for elfuse host source files (src/ only). Tests are excluded
# -- they exercise unsafe patterns deliberately.
#
# 1. Banned functions -- unsafe libc calls with safer alternatives.
# 2. Credential / secret patterns -- catch accidental key leaks.
# 3. Dangerous preprocessor -- detect disabled security features.
# 4. Agent scratch markers -- labels a tool emits to flag a deliberate
#    simplification. The rationale behind one is often worth keeping; the
#    label is not, because it reads as a machine's note rather than the
#    author's, and nothing else in the tree notices when one survives.

set -u -o pipefail

failed=0

# Patterns
banned='(^|[^[:alnum:]_])(gets|sprintf|vsprintf|strcpy|stpcpy|strcat|atoi|atol|atoll|atof|mktemp|tmpnam|tempnam)[[:space:]]*\('
secrets='(password|secret|api_key|private_key|token)[[:space:]]*=[[:space:]]*"[^"]+'
dangerous_pp='#[[:space:]]*(undef|define)[[:space:]]+((_FORTIFY_SOURCE[[:space:]]+0)|(__SSP__))'
comment_only='^[[:space:]]*(//|/\*|\*|\*/)'

# Matched against comments too, unlike the three above, since that is the only
# place this appears.
scratch_marker='(^|[^[:alnum:]_])ponytail:'

# Only scan elfuse host source, not tests/ or assembly shim. Named paths win
# over the tracked set so the pre-commit hook can point this at an extracted
# index and get the same patterns applied to staged content.
list_files()
{
    if [ "$#" -gt 0 ]; then
        printf '%s\0' "$@"
    else
        git ls-files -z -- 'src/*.c' 'src/*.h' 'src/**/*.c' 'src/**/*.h'
    fi
}

# Each match uses process substitution rather than a shell pipeline: under
# `pipefail`, an early `grep -q` exit closes its stdin, the upstream filter
# receives SIGPIPE, and the pipeline returns non-zero even when the pattern
# matched -- silently dropping real findings. Process substitution puts the
# filter in a separate process whose exit status doesn't feed back into the
# matcher.
while IFS= read -r -d '' f; do

    # A path that cannot be read matches nothing, and nothing matching is what
    # this script reports as clean. A named path is supplied by a caller, so a
    # stale or mistyped one would turn a security gate into a success message.
    if [ ! -r "$f" ]; then
        echo "Cannot read $f"
        failed=1
        continue
    fi
    if grep -qE "$banned" < <(grep -vE "$comment_only" -- "$f"); then
        echo "Banned function in $f:"
        grep -nE "$banned" -- "$f" | grep -vE "$comment_only" || true
        failed=1
    fi
    if grep -iqE "$secrets" < <(grep -vE "$comment_only" -- "$f"); then
        echo "Possible hardcoded secret in $f:"
        grep -inE "$secrets" -- "$f" | grep -vE "$comment_only" || true
        failed=1
    fi
    if grep -qE "$dangerous_pp" < <(grep -vE "$comment_only" -- "$f"); then
        echo "Dangerous preprocessor directive in $f:"
        grep -nE "$dangerous_pp" -- "$f" | grep -vE "$comment_only" || true
        failed=1
    fi
    if grep -qE "$scratch_marker" -- "$f"; then
        echo "Agent scratch marker in $f (keep the reasoning, drop the label):"
        grep -nE "$scratch_marker" -- "$f" || true
        failed=1
    fi
done < <(list_files "$@")

if [ $failed -eq 0 ]; then
    echo "Security checks passed."
fi

exit $failed
