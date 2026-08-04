#!/bin/sh
#   ./update.sh                       - the published build, in place
#   ./update.sh --fetch binaries-pr64 - that branch's build, in place, git only
#   ./update.sh --pr 64               - that pull request's build, into try/pr64/
#   ./update.sh --commit a1b2c3       - that commit's build, into try/a1b2c3/
#
# --fetch needs nothing but git. The other two need the GitHub CLI and a token,
# which the machine that most needs a build is the least likely to have - so CI
# publishes every branch as `binaries-<branch>` and --fetch takes it the same
# way the plain form takes main's.
#
# The two new forms never touch the published binaries. This branch's history is
# REPLACED on every publish, so anything unpacked over linux-x64/ would be
# destroyed by the next update without warning - and a build you are testing is
# exactly the thing you would not want quietly replaced. They go under try/, and
# you run them from there.
set -e

case "$1" in
  "")   git fetch origin binaries && exec git reset --hard origin/binaries ;;
  --fetch)
        [ -n "$2" ] || { echo "--fetch needs a ref: ./update.sh --fetch binaries-pr64" >&2; exit 2; }
        # In place, like the plain form - this branch is disposable by design,
        # so there is nothing here worth protecting from being replaced.
        git fetch origin "$2" || {
          echo "no such ref: $2" >&2
          echo "  CI publishes a branch as binaries-<branch>, e.g. binaries-dblclick-probe" >&2
          exit 1
        }
        exec git reset --hard FETCH_HEAD ;;
  --pr|--commit) ;;
  *)    echo "usage: ./update.sh [--fetch REF | --pr N | --commit SHA]" >&2; exit 2 ;;
esac

[ -n "$2" ] || { echo "$1 needs a value: ./update.sh $1 <value>" >&2; exit 2; }
command -v gh >/dev/null 2>&1 || {
  echo "GitHub CLI not found. Install from https://cli.github.com/ then: gh auth login" >&2
  exit 1
}

REPO=klimemam/viewer
case "$(uname -s)" in
  Darwin) ART=viewer-macos-arm64 ; BIN="viewer.app" ;;
  *)      ART=viewer-linux-x64   ; BIN="viewer"     ;;
esac

if [ "$1" = "--pr" ]; then
  TAG="pr$2"
  # A PR's build is the run for its HEAD BRANCH, not for the PR number
  REF=$(gh pr view "$2" --repo "$REPO" --json headRefName --jq .headRefName 2>/dev/null || true)
  [ -n "$REF" ] || { echo "PR $2 not found in $REPO" >&2; exit 1; }
  RUN=$(gh run list --repo "$REPO" --branch "$REF" --status success --limit 1 \
        --json databaseId --jq '.[0].databaseId' 2>/dev/null || true)
else
  TAG="$2"
  # --commit matches the full 40-character sha only, and nobody types those.
  # Expand through the API rather than git rev-parse: this clone has the
  # binaries branch, not main's history, so it cannot resolve main's shas.
  FULL=$(gh api "repos/$REPO/commits/$2" --jq .sha 2>/dev/null || true)
  [ -n "$FULL" ] || { echo "commit $2 not found in $REPO" >&2; exit 1; }
  RUN=$(gh run list --repo "$REPO" --commit "$FULL" --status success --limit 1 \n        --json databaseId --jq '.[0].databaseId' 2>/dev/null || true)
fi

[ -n "$RUN" ] && [ "$RUN" != "null" ] || {
  echo "no SUCCESSFUL build found for $1 $2" >&2
  echo "  a run still going, or one that failed, has no binaries to take" >&2
  exit 1
}

DEST="try/$TAG"
rm -rf "$DEST"
mkdir -p "$DEST"
echo "run $RUN -> $DEST"
gh run download "$RUN" --repo "$REPO" -n "$ART" -D "$DEST" || {
  echo "download failed (artifacts expire after 90 days)" >&2
  exit 1
}
chmod +x "$DEST"/viewer "$DEST"/viewer-serve 2>/dev/null || true

echo
echo "  $DEST/$BIN"
echo
echo "The published build is untouched. Delete try/ when you are done."
