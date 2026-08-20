#!/usr/bin/env bash

# Put the rules in the editor buffer, where they are read, rather than leaving
# them to be discovered by rejection.

set -uo pipefail

message_file=$1
source=${2:-}

# A message that already exists came from -m, a template, a merge, a squash or
# an amend. Appending to any of those edits someone else's text.
case "$source" in
    message | template | merge | squash | commit) exit 0 ;;
esac

# git strips comment lines using core.commentChar, not "#". Writing the wrong
# one leaves this guidance in the message, where it becomes the subject and is
# then rejected for starting with punctuation. "auto" asks git to pick a
# character the message does not already use, which is only knowable once the
# message exists, so treat it as the default it picks for an empty buffer.
comment_char=$(git config --get core.commentChar) || comment_char='#'
case "$comment_char" in
    '' | auto) comment_char='#' ;;
esac

# "git commit -v" has already appended the diff below a scissors line by the
# time this runs. Everything from there down is git's, so it must not count as a
# message the author wrote, and the guidance has to go above it or git discards
# the two together. Read against the whole file the diff looked like an existing
# message and the rules were never offered to anyone committing with -v, which
# is the population that reads the buffer.
scissors='-\{8,\}[[:space:]]*>8[[:space:]]*-\{8,\}'
cut=$(grep -n -- "$scissors" "$message_file" | sed -n '1s/:.*//p')

if grep -qE "^[^[:space:]$comment_char]" \
    < <(sed "/$scissors/,\$d" "$message_file"); then
    exit 0
fi

# Exit 0 rather than 1 wherever this cannot proceed: the buffer is a
# convenience, and a hook that aborts the commit over its own scratch file has
# cost the author more than it ever offered them.
rules=$(mktemp) || exit 0
trap 'rm -f "$rules"' EXIT

{
    printf '\n%s Commit rules, from CONTRIBUTING.md:\n' "$comment_char"
    printf '%s  1. Separate the subject from the body with a blank line\n' "$comment_char"
    printf '%s  2. Limit the subject line to 50 characters\n' "$comment_char"
    printf '%s  3. Capitalize the subject line\n' "$comment_char"
    printf '%s  4. Do not end the subject line with a period\n' "$comment_char"
    printf '%s  5. Use the imperative mood in the subject line\n' "$comment_char"
    printf '%s  6. Wrap the body at 72 characters\n' "$comment_char"
    printf '%s  7. Use the body to explain what and why, not how\n' "$comment_char"
    printf '%s\n%s ASCII only. No backticks in the subject.\n' "$comment_char" "$comment_char"
    printf '%s\n%s Staged files:\n' "$comment_char" "$comment_char"
    git diff --cached --name-only | sed "s/^/$comment_char - /"
} > "$rules"

if [ -z "$cut" ]; then
    cat "$rules" >> "$message_file"
else
    spliced=$(mktemp) || exit 0
    {
        head -n "$((cut - 1))" "$message_file"
        cat "$rules"
        tail -n "+$cut" "$message_file"
    } > "$spliced" && cat "$spliced" > "$message_file"
    rm -f "$spliced"
fi
