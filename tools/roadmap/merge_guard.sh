#!/usr/bin/env bash
# Self-merge guard for the autonomous execution loop.
#
# The operating loop merges ONLY through this script; raw `gh pr merge` is denied to
# loop agents in .claude/settings.json. It enforces the ONE safety property GitHub
# branch protection cannot (there is no required reviewer): an INDEPENDENT-REVIEW
# artifact must exist before merge. It also re-checks the required "CI gate".
#
# Hard backstops remain on the GitHub side (verified): branch protection requires the
# "CI gate" context, enforce_admins is ON (so `--admin` cannot bypass it), and
# allow_force_pushes is OFF. This guard is the loop-side complement, not a substitute.
#
# The `independent-reviewed` label MUST be applied by a DIFFERENT agent than the
# implementer, reviewing in a fresh context (see operating-loop.md). Usage:
#   bash tools/roadmap/merge_guard.sh <pr-number-or-url>
set -euo pipefail

PR="${1:-}"
[ -n "$PR" ] || { echo "usage: merge_guard.sh <pr-number-or-url>" >&2; exit 2; }
REVIEW_LABEL="${OLR_REVIEW_LABEL:-independent-reviewed}"

# (a) Required "CI gate" must be green. (GitHub also enforces this at merge time.)
checks="$(gh pr checks "$PR" 2>/dev/null || true)"
if ! printf '%s\n' "$checks" | grep -iE '(^|[[:space:]])CI gate([[:space:]]).*(pass|success)' -q; then
    echo "[merge-guard] BLOCK: required 'CI gate' is not green for PR $PR." >&2
    printf '%s\n' "$checks" | grep -i 'CI gate' >&2 || true
    exit 1
fi

# (b) Independent-review artifact must be present.
labels="$(gh pr view "$PR" --json labels --jq '.labels[].name' 2>/dev/null || true)"
if ! printf '%s\n' "$labels" | grep -qx "$REVIEW_LABEL"; then
    echo "[merge-guard] BLOCK: PR $PR lacks the '$REVIEW_LABEL' label." >&2
    echo "[merge-guard] An independent reviewer (a DIFFERENT agent, fresh context) must apply it" >&2
    echo "[merge-guard] after a review with zero unresolved Critical/Important findings." >&2
    exit 1
fi

echo "[merge-guard] CI gate green + '$REVIEW_LABEL' present -> merging PR $PR (merge commit)."
exec gh pr merge "$PR" --merge --delete-branch
