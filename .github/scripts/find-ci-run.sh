#!/usr/bin/env bash
# find-ci-run.sh WORKFLOW COMMIT ARTIFACT
#
# Prints the id of a successful branch-push run of WORKFLOW (a file in .github/workflows) for COMMIT whose ARTIFACT
# hasn't expired, so the release workflow can take its package rather than building it again. A run still going is
# waited for. Prints nothing when there's no such run: the commit had [skip ci], or its run failed or was cancelled.
# Needs GH_TOKEN, and GITHUB_REPOSITORY as owner/name.
set -euo pipefail

workflow="$1"
commit="$2"
artifact="$3"
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"

runs() {
    gh api "repos/$repo/actions/workflows/$workflow/runs?head_sha=$commit&event=push&per_page=20" \
        --jq '.workflow_runs[] | "\(.id) \(.status) \(.conclusion)"'
}

has_artifact() {
    [ -n "$(gh api "repos/$repo/actions/runs/$1/artifacts" \
        --jq ".artifacts[] | select(.name == \"$artifact\" and (.expired | not)) | .id")" ]
}

while :; do
    pending=""
    # Newest first, so the newest successful run is taken.
    while read -r id status conclusion; do
        [ -n "$id" ] || continue
        if [ "$status" = completed ]; then
            if [ "$conclusion" = success ] && has_artifact "$id"; then
                echo "$id"
                exit 0
            fi
        elif [ -z "$pending" ]; then
            pending="$id"
        fi
    done < <(runs)

    [ -n "$pending" ] || exit 0
    echo "Waiting for run $pending of $workflow to finish" >&2
    gh run watch "$pending" --repo "$repo" --interval 60 > /dev/null 2>&1 || true
done
