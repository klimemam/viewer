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

REPO=klimemam/viewer
# which published directory this machine runs out of, and what it is called
case "$(uname -s)" in
  Darwin) DIR=macos-arm64 ; ART=viewer-macos-arm64 ; BIN="viewer.app" ;;
  *)      DIR=linux-x64   ; ART=viewer-linux-x64   ; BIN="viewer"     ;;
esac

# Is a viewer from THIS directory running? Prints one pid per line.
#
# update.cmd refuses to update at all in this situation, and it is right to:
# Windows maps a running .exe and will not let git unlink it, so the reset
# stops halfway and leaves a mixture of two builds. Here the opposite holds,
# and it is worth being explicit rather than copying the Windows behaviour.
# Unlinking a file that is being executed is allowed, so git replaces every
# file, the update genuinely succeeds - and the process already running keeps
# the inode, and therefore the code, it started with. Nothing is broken; the
# surprise is that the window still open is the OLD build and stays the old
# build until it is restarted. So: update, then say so.
#
# The check is by FILE, not by name. /proc/<pid>/exe is a symlink to the exact
# file a process was started from, so comparing it with this directory's
# binary finds processes from THIS checkout and no others - `pgrep viewer`
# would also match a build under try/, a copy elsewhere on the disk, and any
# unrelated program that happens to be called viewer. Where there is no /proc
# (macOS), lsof asks the same question about the same path. If neither is
# available we say nothing at all rather than guess.
running_pids() {                 # $1 = path to a binary
  [ -e "$1" ] || return 0
  # -P: the PHYSICAL path. /proc/<pid>/exe is already resolved, so a logical
  # path picked up through a symlinked parent would never compare equal.
  abs=$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")
  if [ -r /proc/self/exe ]; then
    for e in /proc/[0-9]*/exe; do
      t=$(readlink "$e" 2>/dev/null) || continue
      # after an earlier update the old inode is unlinked: "<path> (deleted)"
      case "$t" in
        "$abs"|"$abs (deleted)") p=${e#/proc/}; echo "${p%/exe}" ;;
      esac
    done
  elif command -v lsof >/dev/null 2>&1; then
    lsof -t -- "$abs" 2>/dev/null || true
  fi
}

reset_to() {                     # $1 = ref to make the tree be
  # Only the binaries this reset actually REWRITES are worth asking about: a
  # viewer running out of a file the update does not touch is still the
  # current build, and saying otherwise would be noise on every run. Asked
  # BEFORE the reset - afterwards the path names a new inode and the running
  # process holds the old one, which no longer has a name.
  busy=$(for b in "$DIR/viewer" "$DIR/viewer-serve"; do
           git diff --quiet "$1" -- "$b" 2>/dev/null || running_pids "$b"
         done | sort -un | tr '\n' ' ')
  git reset --hard "$1"
  [ -n "$busy" ] || return 0
  cat >&2 <<EOF

The files are updated, but a viewer started from this folder is still running
(pid ${busy% }). Replacing a program's file does not change the program that
is already running - it keeps the code it started with. Quit that window and
start ./$DIR/$BIN again to be on the new build.
EOF
}

case "$1" in
  "")   git fetch origin binaries && reset_to origin/binaries; exit 0 ;;
  --fetch)
        [ -n "$2" ] || { echo "--fetch needs a ref: ./update.sh --fetch binaries-pr64" >&2; exit 2; }
        # In place, like the plain form - this branch is disposable by design,
        # so there is nothing here worth protecting from being replaced.
        git fetch origin "$2" || {
          echo "no such ref: $2" >&2
          echo "  CI publishes a branch as binaries-<branch>, e.g. binaries-dblclick-probe" >&2
          exit 1
        }
        reset_to FETCH_HEAD; exit 0 ;;
  --pr|--commit) ;;
  *)    echo "usage: ./update.sh [--fetch REF | --pr N | --commit SHA]" >&2; exit 2 ;;
esac

[ -n "$2" ] || { echo "$1 needs a value: ./update.sh $1 <value>" >&2; exit 2; }
command -v gh >/dev/null 2>&1 || {
  echo "GitHub CLI not found. Install from https://cli.github.com/ then: gh auth login" >&2
  exit 1
}

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
  RUN=$(gh run list --repo "$REPO" --commit "$FULL" --status success --limit 1 \
        --json databaseId --jq '.[0].databaseId' 2>/dev/null || true)
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
