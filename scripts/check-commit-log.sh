#!/usr/bin/env bash

# Validate the commit messages whose object IDs arrive on standard input.
#
# The shared body of the pre-push hook and the CI commit-log gate, so a
# contributor with hooks installed and one without are judged by the same script
# rather than by two lists that drift.

set -uo pipefail

script_dir=$(cd -P "$(dirname "$0")" && pwd)
message_hook="$script_dir/git-commit-msg.sh"
failed=0

while read -r commit; do
    [ -n "$commit" ] || continue
    if ! git show -s --format=%B "$commit" | "$message_hook" -; then

        # Positional, not "-- $commit": after a double dash git reads the
        # argument as a pathspec and prints nothing, which left every rejection
        # naming an empty commit. Through cat -v: the subject is
        # attacker-supplied text on its way to a terminal or a CI log, and an
        # escape sequence in one can rewrite what the reader sees around it.
        printf 'Invalid commit: %s\n' \
            "$(git show -s --format='%h %s' "$commit" | cat -v)" >&2
        failed=1
    fi
done

exit "$failed"
