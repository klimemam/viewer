#!/bin/sh
# Run the peer bootstrap's six shell states through a REAL SHELL.
#
#     tools/test_bootstrap_states.sh <bootstrap.sh> <forced-update.sh> <protocol> <probe.sh>
#
# #268 was a decision made in POSIX sh on somebody else's machine: the install
# early-out answered "already installed" about a file that merely existed, so an
# update that replaced nothing reported success and the same dead peer kept
# running. The fix reworded that decision into four lines of `sh`, and
# --settings-selftest L8 pins the TEXT of those lines - which is the smaller
# half. Whether `case` / `-ge` / `${v##* }` actually DECIDE correctly is a
# question about sh, and a C++ test cannot answer it.
#
# So: plant a fake viewer-serve in a throwaway HOME, six times, and run the real
# generated script against each. The scripts are not retyped here - they are the
# strings --settings-selftest dumps (core/selftest/settings.inc, group L8),
# which is what keeps this from drifting from what the viewer sends over ssh.
#
# HOW "IT DECIDED TO INSTALL" IS OBSERVED, with no network and no side effects:
# a fake `git` goes on the front of PATH. It prints one token and fails, so
# reaching it is proof the early-out did not fire, and nothing is ever cloned.
# The real git is checked to be shadowed BEFORE anything runs - a test that
# silently fell through to a real `git clone` would be worse than no test.
set -e

script=$1
forced=$2
proto=$3
probe=$4
if [ ! -f "$script" ] || [ ! -f "$forced" ] || [ -z "$proto" ] || [ ! -f "$probe" ]; then
  echo "usage: $0 <bootstrap script> <forced update script> <protocol> <probe script>" >&2
  exit 2
fi
case "$proto" in
  ''|*[!0-9]*) echo "protocol version must be a number, got '$proto'" >&2; exit 2 ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/viewer-bootstrap-states.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

bin=$work/bin
mkdir -p "$bin"
cat > "$bin/git" <<'EOF'
#!/bin/sh
echo "FAKE_GIT_CALLED: $*"
exit 1
EOF
chmod 755 "$bin/git"

# Refuse to run rather than clone for real. `sh -c command -v` is asked with the
# same PATH the states will see, because that is the lookup that matters.
found=$(PATH="$bin:$PATH" sh -c 'command -v git' 2>/dev/null || true)
if [ "$found" != "$bin/git" ]; then
  echo "the fake git is not what sh finds (got '$found') - refusing to run" >&2
  exit 1
fi
echo "git is shadowed by $found"
echo "this viewer speaks protocol $proto"

# Does this filesystem HONOUR the executable bit? State 2 below is "the peer is
# there and mode 644", and on a platform where the bit is advisory (MSYS / Git
# for Windows, where this script is also run by hand) a 644 script still runs -
# so that state would assert the wrong answer about the shell instead of about
# the script. Probed rather than assumed, and skipped out loud rather than
# quietly: a state silently dropped is a state nobody notices losing.
mkdir -p "$work/probe"
printf '#!/bin/sh\necho ran\n' > "$work/probe/p"
chmod 644 "$work/probe/p"
exec_bit_real=yes
if "$work/probe/p" >/dev/null 2>&1; then exec_bit_real=no; fi
if [ "$exec_bit_real" = no ]; then
  echo "NOTE: this platform runs a mode-644 file anyway, so state 2 is skipped here"
  echo "      (CI runs this on Linux, where it is not)"
fi
echo

home=$work/home
fail=0

plant() {                        # $1 = mode; the body comes on stdin
  mkdir -p "$home/.viewer"
  { echo '#!/bin/sh'; cat; } > "$home/.viewer/viewer-serve"
  chmod "$1" "$home/.viewer/viewer-serve"
}

# $1 = state name, $2 = INSTALL|KEEP, $3 = which script to run
expect() {
  name=$1; want=$2; use=$3
  out=$(HOME="$home" PATH="$bin:$PATH" sh "$use" 2>&1 || true)
  first=$(printf '%s\n' "$out" | sed -n '/./{p;q;}')
  verdict='neither'
  case "$out" in
    *FAKE_GIT_CALLED*)              verdict=INSTALL ;;
    *"no git on the server"*)       verdict=INSTALL ;;
    *VIEWER_SERVE_OK*)              verdict=KEEP ;;
  esac
  if [ "$verdict" = "$want" ]; then
    echo "  $name  -> $verdict"
    echo "      sh said: $first"
  else
    fail=1
    echo "  $name  -> $verdict, WANTED $want   FAILED"
    printf '%s\n' "$out" | sed 's/^/      | /'
  fi
}

echo "the six states, each one a real run of $(command -v sh):"

# 1. NOT THERE AT ALL - the plain missing-peer case.
rm -rf "$home"; mkdir -p "$home/.viewer"
expect "no peer at all            " INSTALL "$script"

# 2. PRESENT AND NOT EXECUTABLE. A copy that lost the bit, or a $HOME mounted
#    noexec. The viewer cannot start it either, so it is not installed.
if [ "$exec_bit_real" = yes ]; then
  rm -rf "$home"; plant 644 <<EOF
echo "viewer-serve protocol $proto"
EOF
  expect "there, but mode 644       " INSTALL "$script"
else
  echo "  there, but mode 644         -> SKIPPED (the exec bit is advisory here)"
fi

# 3. RUNS AND IS OLD. The pre-#268 script had no notion of this state: a peer
#    that answered at all was "installed", whatever it answered.
rm -rf "$home"; plant 755 <<'EOF'
echo "viewer-serve protocol 1"
EOF
expect "runs, protocol 1 (older)  " INSTALL "$script"

# 4. STARTS AND DIES ON GLIBC. THIS IS #268. The old early-out saw a file, said
#    VIEWER_SERVE_OK, and the user got an update that changed nothing.
rm -rf "$home"; plant 755 <<'EOF'
echo "$0: /lib64/libm.so.6: version GLIBC_2.29 not found (required by $0)" >&2
exit 1
EOF
expect "starts and dies on glibc  " INSTALL "$script"

# 5. GARBAGE. A download that stopped halfway: ELF magic and nothing behind it.
#    Whatever the shell makes of that, the answer must fall to the safe side.
rm -rf "$home"; mkdir -p "$home/.viewer"
printf '\177ELF\002\001\001\000 truncated-not-a-real-binary\n' > "$home/.viewer/viewer-serve"
chmod 755 "$home/.viewer/viewer-serve"
expect "garbage where it should be" INSTALL "$script"

# 6. RUNS AND IS CURRENT. The fast path has to survive, or every connect that
#    reaches this script re-clones the binaries branch.
rm -rf "$home"; plant 755 <<EOF
echo "viewer-serve protocol $proto"
EOF
expect "runs, protocol $proto (current)" KEEP "$script"

echo
# ...and the seventh, which is the other half of the same bug: `File > Update
# remote peer` must install OVER a peer that is already current. That form used
# to be made by deleting the early-out from the finished text by searching for
# it - surgery the rewording in states 1-6 would have broken silently. It is a
# parameter now, and this runs that parameter instead of reading it.
echo "and the forced form (File > Update remote peer), over that same current peer:"
expect "current peer, forced      " INSTALL "$forced"

echo
# ---- and the OTHER decision made in sh: what the probe BLAMES ---------------
#
# peerProbeScript used to print "viewer-serve needs GLIBC_x but this host has y"
# whenever `--version` failed and the binary contained a GLIBC_ string - which
# every glibc binary does. So a noexec $HOME, a mode-644 copy, an x86_64 build
# on an aarch64 box and a truncated download all reported a glibc mismatch, and
# docs tell the user to report that line. Wrong line, wrong next bug report.
#
# The fakes below therefore carry GLIBC_ strings in their bytes and fail for
# reasons that are NOT glibc: that is the trap, and it is the only way to see
# the gate work. The one that IS a glibc failure must still get the sentence,
# or the gate has merely turned the feature off.
echo "and the probe's diagnosis gate, on a fake peer whose bytes DO contain GLIBC_:"
probe_expect() {                 # $1 = state name, $2 = needs|no-needs
  out=$(HOME="$home" PATH="$bin:$PATH" sh "$probe" 2>&1 || true)
  got=no-needs
  case "$out" in *"needs GLIBC_"*) got=needs ;; esac
  if [ "$got" = "$2" ]; then
    echo "  $1  -> $got"
    printf '%s\n' "$out" | sed 's/^/      | /'
  else
    fail=1
    echo "  $1  -> $got, WANTED $2   FAILED"
    printf '%s\n' "$out" | sed 's/^/      | /'
  fi
}
glibc_strings() {                # what makes the naive diagnosis fire
  printf 'GLIBC_2.2.5\nGLIBC_2.17\nGLIBC_2.29\n' >> "$home/.viewer/viewer-serve"
}

rm -rf "$home"; plant 755 <<'EOF'
echo "$0: /lib64/libm.so.6: version GLIBC_2.29 not found (required by $0)" >&2
exit 1
EOF
glibc_strings
probe_expect "the loader really did say GLIBC_" needs

rm -rf "$home"; plant 755 <<'EOF'
echo "$0: cannot execute binary file: Exec format error" >&2
exit 126
EOF
glibc_strings
probe_expect "wrong architecture              " no-needs

rm -rf "$home"; plant 755 <<'EOF'
echo "$0: Permission denied" >&2
exit 126
EOF
glibc_strings
probe_expect "noexec \$HOME / mode 644         " no-needs

# ...and with no peer at all the marker deployPeer reads must be there, because
# step 1 tells "missing" from "broken" by finding it.
rm -rf "$home"; mkdir -p "$home/.viewer"
out=$(HOME="$home" PATH="$bin:$PATH" sh "$probe" 2>&1 || true)
case "$out" in
  *NO_PEER*) echo "  no peer at all                  -> NO_PEER (the marker deployPeer reads)" ;;
  *) fail=1; echo "  no peer at all                  -> NO MARKER   FAILED"
     printf '%s\n' "$out" | sed 's/^/      | /' ;;
esac

echo
if [ "$fail" -ne 0 ]; then
  echo "a decision the peer install makes in sh is wrong (#268)" >&2
  exit 1
fi
echo "all eleven decisions are as specified"
