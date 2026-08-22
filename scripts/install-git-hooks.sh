#!/usr/bin/env bash

# Link the repository's Git hooks into the hooks directory, or unlink them.
#
# Symlinks, so a hook edited on a branch takes effect without reinstalling. They
# point into the main worktree when it has the hook: hooks live in the shared
# git dir and apply to every worktree. A newer linked worktree supplies a hook
# that the main worktree does not have yet.
#
# A hook that is already there is never replaced. It belongs to whoever put it
# there, and silently overwriting it is how a tool loses the right to be run
# automatically.

set -uo pipefail

mode=install
quiet=0
for arg in "$@"; do
    case "$arg" in
        --uninstall) mode=uninstall ;;
        --quiet) quiet=1 ;;
        *)
            echo "usage: ${0##*/} [--uninstall] [--quiet]" >&2
            exit 2
            ;;
    esac
done

# A tarball export has no git dir and nothing to install into. Not an error: the
# build has no business failing over a convenience.
git rev-parse --git-dir > /dev/null 2>&1 || exit 0

root=$(git worktree list --porcelain | sed -n '1s/^worktree //p')
hooks=$(git rev-parse --path-format=absolute --git-path hooks)
[ -n "$root" ] && [ -n "$hooks" ] || exit 0
script_dir=$(cd -P "$(dirname "$0")" && pwd)

# say <tag> <text>
say()
{
    [ "$quiet" -eq 1 ] || printf '  %-7s %s\n' "$1" "$2"
}

# "git rev-parse --git-path hooks" answers with core.hooksPath when that is set,
# which is where git will actually look. Worth saying only when that lands
# somewhere other than this repository's own hooks directory, since it is
# frequently a directory shared with other repositories. Pointing it at the
# default is not news, and neither is repeating it on a build that changed
# nothing, so the quiet path holds it until something is actually installed.
default_hooks="$(git rev-parse --path-format=absolute --git-common-dir)/hooks"
hooks_moved=0
if [ "$hooks" != "$default_hooks" ]; then
    hooks_moved=1
fi

installed=0
failed=0

if [ "$mode" = uninstall ]; then

    # Enumerate what is installed, not what scripts/ holds today: a branch that
    # deletes a hook script would otherwise strand its link, because the name is
    # only known from the script that is now gone. A link counts as ours when it
    # points at the matching git-<name>.sh inside the scripts directory of some
    # worktree of this repository, which leaves a hand-written hook, a regular
    # file, and a link into anywhere else alone.
    for target in "$hooks"/*; do
        [ -L "$target" ] || continue
        name=${target##*/}
        link=$(readlink "$target")
        while IFS= read -r wt; do
            [ -n "$wt" ] || continue
            if [ "$link" = "$wt/scripts/git-$name.sh" ]; then
                rm -f "$target"
                say RM "$name"
                break
            fi
        done << EOF
$(git worktree list --porcelain | sed -n 's/^worktree //p')
EOF
    done
    exit 0
fi

for hook in "$script_dir"/git-*.sh; do
    [ -e "$hook" ] || continue
    name=${hook##*/git-}
    name=${name%.sh}
    target="$hooks/$name"
    source="$root/scripts/${hook##*/}"
    [ -e "$source" ] || source=$hook

    mkdir -p "$hooks" || {
        failed=1
        continue
    }
    if [ -e "$target" ] || [ -L "$target" ]; then
        say KEEP "$target"
    elif ln -s "$source" "$target" 2> /dev/null; then
        say HOOK "$name"
        installed=$((installed + 1))
    else

        # Not silent: the caller stamps this run as done only when it reports
        # success, so a hooks directory that was briefly unavailable is retried
        # rather than remembered as installed.
        echo "install-git-hooks: cannot link $target" >&2
        failed=1
    fi
done

# The quiet path is the one make runs on its own, and it stays silent unless it
# actually did something. A line per already-linked hook on every build is how
# output stops being read.
if [ "$quiet" -eq 0 ] || [ "$installed" -gt 0 ]; then
    if [ "$hooks_moved" -eq 1 ]; then
        printf '  NOTE    core.hooksPath sends hooks to %s\n' "$hooks"
    fi
fi

if [ "$quiet" -eq 1 ] && [ "$installed" -gt 0 ]; then
    printf "  HOOKS   installed %d git hook(s); make uninstall-hooks removes them\n" \
        "$installed"
fi

exit "$failed"
