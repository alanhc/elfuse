#!/usr/bin/env bash

# Validate every non-merge commit that a push would introduce.
#
# The last chance before a pull request exists. A rewrite is still free here;
# after the push it costs a force-push and whatever review was already based on
# the old hashes.

set -uo pipefail

remote=${1:-}

# Installed hooks are symlinks, so $0 names the link under .git/hooks and
# "dirname $0" gives that directory rather than the checkout the link points
# into. Walk the chain to find the checker beside the real script instead of
# under whichever worktree happens to be pushing.
#
# Unbounded on purpose. A cycle would spin here forever, but the chain has
# already been resolved once by whatever exec'd this script: a cyclic or
# overlong $0 fails with ELOOP before the first line runs, so this loop only
# ever walks a chain the kernel already walked to the end.
hook_path=$0
while [ -L "$hook_path" ]; do
    hook_dir=$(cd -P "$(dirname "$hook_path")" && pwd)
    hook_path=$(readlink "$hook_path")
    case "$hook_path" in
        /*) ;;
        *) hook_path="$hook_dir/$hook_path" ;;
    esac
done
hook_dir=$(cd -P "$(dirname "$hook_path")" && pwd)
check_script="$hook_dir/check-commit-log.sh"
zero_oid=0000000000000000000000000000000000000000
failed=0

# For a branch the remote has never seen there is no remote_sha to diff from, so
# the boundary is "everything not already published". Scope that to the remote
# being pushed to when it has refs to scope by: a commit sitting only in some
# other remote's namespace has not been published to this one, and excluding it
# would let a malformed message through.
#
# A remote with no tracking refs at all is the exception, and it has to be. A
# freshly added fork, or a push straight at a URL, excludes nothing, so the walk
# reaches the root commit and every message in the project's history is judged
# against today's rules. That produces hundreds of rejections for commits the
# author never wrote, and the only way out of it is --no-verify, which costs
# every other check too. Falling back to all remotes there means a commit
# already published somewhere is not this push's problem. CI checks the pull
# request either way.
published="--remotes"
if [ -n "$remote" ] && [ -n "$(git for-each-ref --count=1 \
    --format='%(refname)' "refs/remotes/$remote/")" ]; then
    published="--remotes=$remote"
fi

while read -r local_ref local_sha remote_ref remote_sha; do
    [ -n "${local_ref:-}" ] || continue
    [ "$local_sha" != "$zero_oid" ] || continue
    git cat-file -e "${local_sha}^{commit}" 2> /dev/null || continue

    if [ "$remote_sha" = "$zero_oid" ]; then
        if ! commits=$(git rev-list --no-merges "$local_sha" --not "$published"); then
            echo "Push rejected: cannot list commits for $local_ref -> $remote_ref." >&2
            failed=1
            continue
        fi
    else
        if ! commits=$(git rev-list --no-merges "${remote_sha}..${local_sha}"); then
            echo "Push rejected: cannot list commits for $local_ref -> $remote_ref." >&2
            failed=1
            continue
        fi
    fi

    if [ -n "$commits" ]; then
        if ! printf '%s\n' "$commits" | "$check_script"; then
            echo "Push rejected for $local_ref -> $remote_ref." >&2
            failed=1
        fi
    fi
done

exit "$failed"
