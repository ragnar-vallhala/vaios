#!/usr/bin/env bash
# Fails if a change adds a todo/room message number: lines added by the diff
# BASE..HEAD, or commit messages in that range, matching "<word> #<digits>"
# for word in todo, room, stack, ashu. Those numbers only mean something
# inside the room they came from; write the reason itself instead. Links to
# this repo's own GitHub issues and PRs are fine and do not match.
#
# usage: tools/check_no_room_refs.sh BASE [HEAD]    (HEAD defaults to HEAD)
set -u
base=${1:?usage: $0 BASE [HEAD]}
head=${2:-HEAD}
# '[#]' keeps this file from matching its own pattern.
pat='(todo|room|stack|ashu) [#][0-9]+'
rc=0

added=$(git diff --unified=0 --no-color "$base...$head" -- . ':!extern' \
        | grep -E '^\+[^+]|^\+$' | grep -iE "$pat")
if [ -n "$added" ]; then
    echo "FAIL: added lines contain a todo/room number:"
    echo "$added"
    rc=1
fi

msgs=$(git log --format='%h %s%n%b' "$base..$head" | grep -iE "$pat")
if [ -n "$msgs" ]; then
    echo "FAIL: commit messages contain a todo/room number:"
    echo "$msgs"
    rc=1
fi

[ $rc -eq 0 ] && echo "ok: no todo/room numbers added"
exit $rc
