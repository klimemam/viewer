#!/bin/sh
#   ./update.sh                       - the published build, in place
#   ./update.sh --peer                - ...and into ~/.viewer/, which is the
#                                       peer the VIEWER actually starts
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

# Did the update leave behind a peer that can actually START here?
#
# #268 is the reason this exists, and the report was the whole of it: "remote
# の update ができない。glibc で怒られます." The update had SUCCEEDED - git
# replaced every file without complaint - and the binary it left could not run
# on that host. Nothing anywhere said WHICH glibc the binary wanted or which
# one the machine had, so the answer needed a round trip to ask, and the next
# report would have needed another one.
#
# viewer-serve is the one that gets probed because it is the one that HAS a
# flag for it: --version prints its protocol number and exits. `viewer` has no
# such flag - running it to find out would try to open a window - so the GUI
# binary's floor stays a documented number (docs/guides/startup.md) instead of
# a measured one.
#
# grep -a over the binary rather than objdump, and strings only as a fallback:
# the machine that needs this sentence is a compute box with no build
# environment, which is exactly the machine with no binutils. The versioned
# symbol names sit in .gnu.version_r as plain ASCII either way, and
# `sort -Vu | tail -1` is the same expression the CI assertion runs on
# objdump's output - so both halves of the project quote the same number.
peer_glibc() {                   # $1 = an ELF binary; prints e.g. GLIBC_2.29
  { LC_ALL=C grep -ao 'GLIBC_[0-9.]*' "$1" 2>/dev/null \
    || strings -a "$1" 2>/dev/null; } \
    | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1
}

peer_starts() {                  # $1 = path to a peer, $2 = what to call it
  [ "$DIR" = linux-x64 ] || return 0        # a glibc question; macOS has none
  [ -x "$1" ] || return 0
  if "$1" --version >/dev/null 2>&1; then return 0; fi
  need=$(peer_glibc "$1")
  have=$(ldd --version 2>&1 | head -1)
  [ -n "$have" ] || have="an unknown libc (no ldd on this host)"
  if [ -n "$need" ]; then
    echo "$2 needs $need but this host has $have" >&2
  else
    echo "$2 does not start on this host ($have):" >&2
    "$1" --version 2>&1 | sed 's/^/  /' >&2
  fi
  echo "  the files ARE updated; this build cannot run here. Report that line." >&2
}

# THE PEER THE VIEWER STARTS IS ~/.viewer/viewer-serve, NEVER THIS CHECKOUT'S
# COPY. core/app/remote_client.inc builds that path from REMOTE_HOME and
# remote.cpp execs exactly it; this script updates the CHECKOUT. On the machine
# that is both the data host and the place `update.sh` is run, those are two
# different files, and nothing here ever touched the second one - so the update
# genuinely succeeded, reported success, and the viewer went on starting the
# same old binary. Together with the viewer's own bootstrap, which used to
# early-out on the mere EXISTENCE of ~/.viewer/viewer-serve, that is #268: "the
# remote update does not work" about an update that worked.
install_peer() {
  src="$DIR/viewer-serve"
  [ -f "$src" ] || { echo "no $src in this checkout - nothing to install" >&2; return 1; }
  d="$HOME/.viewer"
  mkdir -p "$d"
  # .new + mv, never a write in place. A live peer has its file MAPPED (the
  # viewer keeps one per worker for the life of the app), and truncating it
  # under the mapping is a SIGBUS in that process. Unlinking is free: the
  # running peer keeps the inode, and therefore the code, it started with -
  # the same reason bootstrapScript does .new + mv on the far side.
  cp "$src" "$d/viewer-serve.new"
  chmod +x "$d/viewer-serve.new"
  mv "$d/viewer-serve.new" "$d/viewer-serve"
  echo "~/.viewer/viewer-serve <- $src"
  # The plugins travel with it: server-side MEASURE dlopens ~/.viewer/plugins,
  # so a peer updated without them analyses with the old analyzers.
  [ -d "$DIR/plugins" ] || return 0
  mkdir -p "$d/plugins"
  for f in "$DIR"/plugins/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    cp "$f" "$d/plugins/$b.new"
    mv "$d/plugins/$b.new" "$d/plugins/$b"
  done
  echo "~/.viewer/plugins/   <- $DIR/plugins/"
}

# Say the above out loud when it matters, and only then: this folder's peer and
# the one the viewer starts are different builds.
peer_note() {
  [ -f "$HOME/.viewer/viewer-serve" ] || return 0
  [ -f "$DIR/viewer-serve" ] || return 0
  if cmp -s "$DIR/viewer-serve" "$HOME/.viewer/viewer-serve"; then return 0; fi
  cat >&2 <<EOF

The viewer starts ~/.viewer/viewer-serve, not the copy in this folder, and that
one is a DIFFERENT build from what this update just placed here. To refresh it:
    ./update.sh --peer
EOF
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
  if [ -n "$busy" ]; then
    cat >&2 <<EOF

The files are updated, but a viewer started from this folder is still running
(pid ${busy% }). Replacing a program's file does not change the program that
is already running - it keeps the code it started with. Quit that window and
start ./$DIR/$BIN again to be on the new build.
EOF
  fi
  # ...and LAST, because these are the lines that say the update was not
  # enough. Both are silent on a normal run: the peer starts, and ~/.viewer
  # either does not exist or already holds this same build.
  peer_starts "$DIR/viewer-serve" "viewer-serve"
  peer_note
}

case "$1" in
  "")   git fetch origin binaries && reset_to origin/binaries; exit 0 ;;
  --peer)
        # Installs what is IN THE TREE RIGHT NOW, and fetches nothing. That is
        # what makes it composable: after the plain form the tree is the
        # published build, and after `--fetch binaries-pr64` it is that branch's
        # build - so `--peer` always means "the peer beside me becomes the peer
        # the viewer starts", with no second rule about which build that is and
        # nothing that can silently undo a --fetch.
        install_peer
        peer_starts "$HOME/.viewer/viewer-serve" "~/.viewer/viewer-serve"
        exit 0 ;;
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
  *)    echo "usage: ./update.sh [--peer | --fetch REF | --pr N | --commit SHA]" >&2
        echo "  --peer  also install this folder's viewer-serve into ~/.viewer/" >&2
        echo "          (that copy, not this one, is what the viewer starts)" >&2
        exit 2 ;;
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
