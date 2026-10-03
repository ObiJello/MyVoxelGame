#!/usr/bin/env bash
# File: tools/claude_snapshot.sh
#
# Hidden git snapshots of the working tree, taken after every Claude Code
# edit (a PostToolUse hook in .claude/settings.local.json) so an agent's
# mistake is always recoverable — independent of commits and of CLion's
# Local History.
#
# Each snapshot is a commit of the whole working tree (tracked + untracked,
# .gitignore respected) built with a PRIVATE index file, chained under one
# ref per day: refs/claude-snapshots/YYYY-MM-DD. Your branch, your staged
# changes (the real index), your working tree and `git status` are never
# touched, and nothing is pushed. A snapshot whose tree equals the previous
# one is skipped (a read-only tool call costs one stat pass). Refs older than KEEP_DAYS are deleted on the first snapshot
# of a day (git gc then reclaims them).
#
# Usage:
#   tools/claude_snapshot.sh                    take a snapshot (what the hook runs)
#   tools/claude_snapshot.sh list [N]           newest N snapshots (default 30)
#   tools/claude_snapshot.sh log <path> [N]     snapshots in which <path> changed
#   tools/claude_snapshot.sh show <rev> <path>  print <path> as of snapshot <rev>
#   tools/claude_snapshot.sh diff <rev> <path>  diff snapshot <rev>'s <path> against the working tree
#   tools/claude_snapshot.sh restore <rev> <path>
#                                               overwrite <path> in the working tree with snapshot <rev>'s copy
#                                               (snapshots the current state first, so it is undoable)
set -u

KEEP_DAYS=14
REF_PREFIX=refs/claude-snapshots

repo="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$repo" 2>/dev/null || exit 0
git rev-parse --git-dir >/dev/null 2>&1 || exit 0
gitdir="$(git rev-parse --absolute-git-dir)"
snap_index="$gitdir/claude-snapshot-index"
lockdir="$gitdir/claude-snapshot.lock"

# All snapshot refs, newest commit first.
all_refs() { git for-each-ref --sort=-committerdate --format='%(refname)' "$REF_PREFIX/"; }

take_snapshot() {
    local reason="${1:-manual}"
    # One snapshot at a time (hooks run async and can overlap). Wait up to
    # ~20 s, then take the lock over from a crashed holder.
    local waited=0
    until mkdir "$lockdir" 2>/dev/null; do
        sleep 0.2; waited=$((waited + 1))
        if [ "$waited" -ge 100 ]; then rm -rf "$lockdir"; fi
    done
    trap 'rmdir "$lockdir" 2>/dev/null' EXIT

    # The private index persists between runs, so `add -A` only re-hashes
    # files whose stat changed. Seed it from HEAD the first time.
    if [ ! -f "$snap_index" ]; then
        GIT_INDEX_FILE="$snap_index" git read-tree HEAD 2>/dev/null || true
    fi
    GIT_INDEX_FILE="$snap_index" git add -A . 2>/dev/null || return 0
    local tree; tree="$(GIT_INDEX_FILE="$snap_index" git write-tree 2>/dev/null)" || return 0

    local day; day="$(date +%Y-%m-%d)"
    local ref="$REF_PREFIX/$day"
    local parent; parent="$(git rev-parse -q --verify "$ref^{commit}" 2>/dev/null)"
    if [ -z "$parent" ]; then
        # First snapshot today: chain off yesterday's last one if it has the
        # same tree, else off HEAD; and prune old days.
        prune_old
        parent="$(git rev-parse -q --verify HEAD 2>/dev/null)"
    else
        [ "$(git rev-parse "$parent^{tree}")" = "$tree" ] && return 0   # nothing changed
    fi

    local msg; msg="snapshot $(date '+%Y-%m-%d %H:%M:%S') ($reason)"
    local commit
    if [ -n "$parent" ]; then
        commit="$(git commit-tree "$tree" -p "$parent" -m "$msg" 2>/dev/null)" || return 0
    else
        commit="$(git commit-tree "$tree" -m "$msg" 2>/dev/null)" || return 0
    fi
    git update-ref "$ref" "$commit" 2>/dev/null
}

prune_old() {
    local cutoff; cutoff="$(date -v-"${KEEP_DAYS}"d +%Y-%m-%d 2>/dev/null || date -d "-${KEEP_DAYS} days" +%Y-%m-%d)"
    local r
    for r in $(git for-each-ref --format='%(refname)' "$REF_PREFIX/"); do
        local d="${r##*/}"
        [[ "$d" < "$cutoff" ]] && git update-ref -d "$r"
    done
}

# Every snapshot commit across all day refs, newest first. Args: count,
# then an optional path to keep only the snapshots that changed it.
snapshot_log() {
    local count="$1"; shift
    local refs; refs="$(all_refs)"
    [ -z "$refs" ] && { echo "no snapshots yet"; return 1; }
    # Stop at HEAD's history so only snapshot commits are listed.
    # shellcheck disable=SC2086
    git log --format='%h  %cd  %s' --date=format:'%Y-%m-%d %H:%M:%S' -n "$count" \
        $refs --not HEAD -- "$@"
}

cmd="${1:-snapshot}"
case "$cmd" in
    snapshot|hook)
        # The hook passes the tool payload on stdin; name the tool in the message.
        reason="manual"
        if [ ! -t 0 ]; then
            payload="$(cat)"
            tool="$(printf '%s' "$payload" | sed -n 's/.*"tool_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)"
            [ -n "$tool" ] && reason="$tool"
        fi
        take_snapshot "$reason"
        ;;
    list)
        snapshot_log "${2:-30}"
        ;;
    log)
        [ $# -ge 2 ] || { echo "usage: $0 log <path> [N]"; exit 1; }
        snapshot_log "${3:-30}" "$2"
        ;;
    show)
        [ $# -ge 3 ] || { echo "usage: $0 show <rev> <path>"; exit 1; }
        git show "$2:$3"
        ;;
    diff)
        [ $# -ge 3 ] || { echo "usage: $0 diff <rev> <path>"; exit 1; }
        git diff "$2" -- "$3"
        ;;
    restore)
        [ $# -ge 3 ] || { echo "usage: $0 restore <rev> <path>"; exit 1; }
        git cat-file -e "$2:$3" 2>/dev/null || { echo "$3 does not exist in snapshot $2"; exit 1; }
        take_snapshot "before restore of $3"
        git show "$2:$3" > "$3" && echo "restored $3 from snapshot $2"
        ;;
    *)
        sed -n '2,26p' "$0"; exit 1
        ;;
esac
exit 0
