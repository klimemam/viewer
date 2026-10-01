// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- open dispatch
// #111: openRemote routes a local:// file the peer cannot serve back to this
// door, and it is defined further down. The two are deliberately not merged -
// openPath is "what does this machine do with a path", openRemote is "what does
// a peer do with a url", and the fix is that the second one now knows when the
// answer is the first one.
static void openPath(const std::string& path);
// Defined with the stack door far below, declared here because the WATCH door
// (decodeJoiningMember) asks the same question of anything joining a headerless
// stack. See the note at the definition for why the question has a function.
static bool remoteRawSameSize(remote::Session& ses, int peerVersion, const std::string& dir,
                              const std::vector<std::string>& names,
                              uint64_t& sizeOut, std::string& why);
static bool remoteRawSameSizeIn(const std::vector<remote::Entry>& es, int peerVersion,
                                const std::string& dir,
                                const std::vector<std::string>& names,
                                uint64_t& sizeOut, std::string& why);

// `knownSize` > 0: the bytes are not on this disk and the caller already has
// the count (a peer's listing carries it - MSG_LIST has since protocol 3). The
// rest of this function is filename hints and byte-count arithmetic, neither of
// which needs the file open, so ONE dialog serves both disks.
// How many times a geometry has been ASKED for. Not decoration: "the recipe is
// asked once per Open" is a rule about a count, and a test that read rawDlg.open
// can only see whether a dialog is standing right now - it cannot tell one
// question from three answered in between. Monotonic, never reset.
static int g_rawDialogsRaised = 0;

// Returns whether the dialog is now standing. It can say no: an Open that is
// ALREADY waiting to be told how to read its frames owns this dialog, and a
// second question raised over it would take the slot and leave the first Open
// parked with nothing that can ever answer it - App::remoteRawStack alive
// forever and the remote Open queue stopped in silence. The first question
// keeps the dialog; the second is refused BY NAME (issue review #5/#15).
static bool openRawDialogFor(const std::string& path, uint64_t knownSize) {
    if (app.remoteRawStack.alive && rawDlg.open) {
        toast(baseName(path) + ": a geometry is already being asked for (" +
              std::to_string(app.remoteRawStack.files.size()) + " frame(s) of " +
              baseName(app.remoteRawStack.files.front()) +
              " are waiting)\n  answer that first, then open this", true);
        return false;
    }
    size_t sz = (size_t)knownSize;
    if (!knownSize) {
        // A URL IS NOT A PATH. With no count in hand, the ifstream below opens
        // `path` as a file on THIS machine - and for "ssh://host/x.raw" that is
        // a filename nothing here has, so the answer was "cannot open x.raw"
        // about a file the peer is serving perfectly well (review P3-5).
        //
        // A peer's byte count never comes from a stat here; it comes from a
        // listing (remoteRawFileSize) and is passed in. Arriving with 0 means
        // the listing itself said 0 - a row the peer could not stat, or a file
        // that really is empty - and neither of those is a geometry. Said from
        // what is actually known, which is that the count is the missing fact.
        if (hasUrlScheme(path)) {
            toast(baseName(path) + ": the peer reports 0 bytes for it, and a headerless "
                  "file's geometry is worked out from its byte count - so there is "
                  "nothing to ask about yet"
                  "\n  check the file is readable on the peer, then re-open the folder "
                  "from Browse", true);
            return false;
        }
        std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
        if (!f) { toast("cannot open: " + baseName(path), true); return false; }
        sz = (size_t)f.tellg();
    }
    rawDlg.open = true;
    g_rawDialogsRaised++;
    rawDlg.path = path;
    rawDlg.fileSize = sz;
    rawDlg.replaceIdx = -1;           // fresh open (reinterpret sets this explicitly after)
    rawDlg.forRemoteStack = false;    // a fresh ask is nobody's parked Open until it says so
    // guess dtype/interpretation/pattern from filename hints
    std::string n = baseName(path);
    {
        std::string nl = n;
        std::transform(nl.begin(), nl.end(), nl.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (int i = 0; i < 4; i++) {
            std::string pat = CFA_PATTERNS[i];
            std::transform(pat.begin(), pat.end(), pat.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (nl.find(pat) != std::string::npos) {
                rawDlg.interp = RI_BAYER; rawDlg.cfaPattern = i;
                if (rawDlg.dtype == RD_U8) rawDlg.dtype = RD_U16;   // bayer dumps are usually 16bit
            }
        }
        if (nl.find("bayer") != std::string::npos && rawDlg.interp != RI_BAYER && rawDlg.interp != RI_QUAD) {
            rawDlg.interp = RI_BAYER;
            if (rawDlg.dtype == RD_U8) rawDlg.dtype = RD_U16;
        }
        if (nl.find("quad") != std::string::npos &&
            (rawDlg.interp == RI_BAYER || nl.find("bayer") != std::string::npos))
            rawDlg.interp = RI_QUAD;
        if (nl.find("f32") != std::string::npos || nl.find("float") != std::string::npos)
            rawDlg.dtype = RD_F32;
        else if (nl.find("f64") != std::string::npos || nl.find("double") != std::string::npos)
            rawDlg.dtype = RD_F64;
    }
    for (size_t i = 0; i + 1 < n.size(); i++) {
        if ((n[i] == 'x' || n[i] == 'X') && isdigit((unsigned char)n[i + 1]) && i > 0 && isdigit((unsigned char)n[i - 1])) {
            size_t s = i; while (s > 0 && isdigit((unsigned char)n[s - 1])) s--;
            size_t e = i + 1; while (e < n.size() && isdigit((unsigned char)n[e])) e++;
            int W = atoi(n.substr(s, i - s).c_str());
            int H = atoi(n.substr(i + 1, e - i - 1).c_str());
            if (W >= 16 && H >= 16 && W <= 32768 && H <= 32768) { rawDlg.w = W; rawDlg.h = H; }
            break;
        }
    }
    rawGuessDims(rawDlg);
    return true;
}

// The size binding, asked at the ONE door a headerless file arrives at (#166).
// Returns true when the file was opened from a recipe THIS SESSION was told to
// use for files of exactly this byte count, in which case no dialog is raised.
//
// It says so, in one line, every time. That line is not politeness: this is the
// only thing in the whole viewer that decodes pixels a particular way without
// being asked on the spot, and the one property that makes that safe is that
// the user still remembers making the choice. Naming the recipe is what lets
// them check the memory against what is on screen.
//
// A binding that fails to decode is DROPPED and the dialog comes back. It
// cannot normally happen - a binding is only made by a decode that worked, and
// the key is the byte count, so the same geometry fits - but a file that is
// being written while it is read is real, and the honest answer to "the recipe
// no longer fits" is the dialog, not a refusal.
static bool openRawFromSessionRecipe(const std::string& path) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) return false;                        // openRawDialogFor says why
    const size_t sz = (size_t)f.tellg();
    const RawRecipe* r = rawRecipeForSize(sz);
    if (!r) return false;
    const RawRecipe use = *r;                    // by value: loadRaw can rebind the map
    RawDialog d;
    rawRecipeIntoDialog(use, d);
    d.path = path;
    d.fileSize = sz;
    d.replaceIdx = -1;
    std::string err = loadRaw(d);
    if (!err.empty()) {
        g_rawSizeRecipe.erase(sz);
        toast(baseName(path) + ": " + err + " - the remembered recipe no longer "
              "fits, so it is forgotten", true);
        return false;
    }
    toast(rawRecipeAppliedLine(use, path, sz));
    return true;
}

// ssh://user@host/path - the UI stays here, the pixels stay there. What arrives is
// the region being looked at, at the resolution it is being looked at.
// Numeric frame order: frame_1, frame_10, frame_100, frame_2 must not become the
// time axis. Shared by the browser and anything else that builds a stack.
// Frames need not be a strict counter pattern to stack in the order a human
// expects - rp::naturalLess (shared with the peer) is the contract.
//
// THIS ONE TAKES NO SETTING, AND MUST NOT GROW ONE. The Browse listing can be
// asked to show names in plain lexicographic order (rbNameCmp / the panel's
// "..." menu), and the temptation is to thread that flag through to here "for
// consistency". It is the opposite of consistent: for a STACK the order of the
// frames is a measurement property - sigma_t, the drift, the whole temporal
// axis are computed over the frames in the order they were stacked, so a
// different order is a different number - while for a LISTING it is a display
// preference, like which column you sorted by. Keeping the two comparators
// apart is what makes it structurally impossible for a display toggle to move
// a measurement: there is no code path from the menu item to this function.
// (user decision, 2026-08-05: "stack を積むときは natural のみ")
// non-static since P7: nav.cpp (pumpRemoteOpenQueue) and panel.cpp (the
// selection ctx menu) call it by name - declared in core/browse/host.h
void sortFramesNumerically(std::vector<std::string>& files) {
    std::sort(files.begin(), files.end(), rp::naturalLess);
}

// Put the peer on the server without the user copying anything: check
// ~/.viewer/viewer-serve, and if it is missing, have the SERVER pull the
// prebuilt binary from the repository's binaries branch (it has git and network;
// this machine may not even be the same OS). Everything lives under ~/.viewer,
// so uninstalling is one rm -rf.
std::string g_bootstrapLog;   // non-static since P7: the pump (nav.cpp) assigns it

// The Linux/macOS peer, found on THIS machine. A binaries-branch checkout has
// every platform side by side, and the source tree has the built one - so the
// client can hand the server its peer directly. That path works when the server
// has no internet and when the repository is private, which is the normal case
// for a lab compute box; server-side git is only the fallback.
static std::string findLocalPeer(const std::string& unameOut) {
    std::string plat = unameOut.find("Darwin") != std::string::npos ? "macos-arm64" : "linux-x64";
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::u8path(app.exePath);
    std::filesystem::path dir = exe.has_parent_path() ? exe.parent_path()
                                                      : std::filesystem::current_path(ec);
    std::vector<std::filesystem::path> cand;
    for (int up = 0; up < 4 && !dir.empty(); up++) {
        cand.push_back(dir / plat / "viewer-serve");              // binaries checkout
        cand.push_back(dir / "binaries" / plat / "viewer-serve");
        cand.push_back(dir / "viewer-serve");                     // same-dir build
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    for (const auto& c : cand)
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec))
            return c.u8string();
    return {};
}

static bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

static std::string bootstrapScript(bool keepCurrentPeer) {
    // POSIX sh, delivered over stdin so no quoting layer can mangle it.
    // Prints VIEWER_SERVE_OK on success; anything else is shown to the user.
    // 判断17: the build-time VIEWER_REPO_URL is the DEFAULT, not the answer.
    // Somebody running a fork has no reason to rebuild this program in order to
    // change where its peer is fetched from, so settings.jsonc remote.repoUrl
    // may say otherwise; repoUrlInUse() is empty-means-unset (判断9).
    const std::string repo = repoUrlInUse();
    return
        // Everything the script says reaches the user: runSshCommand wires the
        // child's stderr to the PARENT's, so cp's "Text file busy" went to the
        // viewer's own console and the log the user is shown contained git
        // output and no cause at all.
        "exec 2>&1\n"
        "set -e\n"
        "d=$HOME/.viewer\n"
        "mkdir -p \"$d\"\n"
      + (keepCurrentPeer ? std::string(
        // THE EARLY-OUT IS ABOUT A PEER THAT RUNS, not about a file that
        // exists. This line used to be `[ -x "$d/viewer-serve" ]` and that
        // was wrong in EVERY case that can reach this script: deployPeer only
        // gets here after its OWN probe has already decided the peer is
        // missing, dead or stale (see step 1), so an existence test answers
        // "already installed" about the very binary that just failed to
        // start. #268 is what that cost - `./update.sh` refreshed the
        // binaries checkout but not ~/.viewer/viewer-serve, the viewer then
        // found a file there, said VIEWER_SERVE_OK, and the user got an
        // update that reported success and changed nothing.
        //
        // So: ask it, and believe it only if it answers with a protocol
        // number at least as new as ours. `--version` prints
        // "viewer-serve protocol N", hence the last word.
        "if v=$(\"$d/viewer-serve\" --version 2>/dev/null); then\n"
        "  n=${v##* }\n"
        "  case \"$n\" in ''|*[!0-9]*) ;;\n"
        "    *) [ \"$n\" -ge " + std::to_string((int)rp::VERSION) + " ] && { echo VIEWER_SERVE_OK; exit 0; } ;;\n"
        "  esac\n"
        "fi\n") : std::string())
      + "if ! command -v git >/dev/null 2>&1; then\n"
        "  echo 'no git on the server: copy viewer-serve to ~/.viewer/ manually'; exit 1\n"
        "fi\n"
        // A private repo would sit forever on a credential prompt, and an
        // offline box would sit on a TCP connect. Both must fail, loudly.
        "export GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/echo SSH_ASKPASS=/bin/echo\n"
        "export GIT_HTTP_LOW_SPEED_LIMIT=1000 GIT_HTTP_LOW_SPEED_TIME=20\n"
        "T=''; command -v timeout >/dev/null 2>&1 && T='timeout 45'\n"
        "if [ -d \"$d/bin/.git\" ]; then\n"
        "  $T git -C \"$d/bin\" fetch --depth 1 origin binaries 2>&1 || { echo \"git fetch failed\"; exit 1; }\n"
        "  git -C \"$d/bin\" reset --hard origin/binaries >/dev/null 2>&1\n"
        "else\n"
        "  $T git clone --depth 1 -b binaries " + repo + " \"$d/bin\" 2>&1 || { echo \"git clone failed (private repo or no network on this server?)\"; exit 1; }\n"
        "fi\n"
        "u=$(uname -s)\n"
        "case \"$u\" in\n"
        "  Linux)  src=\"$d/bin/linux-x64\" ;;\n"
        "  Darwin) src=\"$d/bin/macos-arm64\" ;;\n"
        "  *)      echo \"unsupported server OS: $u\"; exit 1 ;;\n"
        "esac\n"
        "[ -f \"$src/viewer-serve\" ] || { echo 'binaries branch has no viewer-serve'; exit 1; }\n"
        // Replace the INODE, never the bytes: rfWorker and mWorker each hold a
        // ~/.viewer/viewer-serve process for the life of the app and only the
        // BROWSE session is stopped for an update, so a plain cp opens the
        // running binary O_WRONLY|O_TRUNC and fails with ETXTBSY - which
        // `set -e` then turns into an update that reports the wrong reason.
        // Step 2 of deployPeer already does .new + chmod + mv for this reason.
        "cp \"$src/viewer-serve\" \"$d/viewer-serve.new\"\n"
        "chmod +x \"$d/viewer-serve.new\"\n"
        "mv \"$d/viewer-serve.new\" \"$d/viewer-serve\"\n"
        // The plugins are already safe as they stand: rm -rf UNLINKS them, which
        // a live peer's mapping survives, and cp -r then creates new inodes.
        // Only an in-place TRUNCATE would be a SIGBUS in the peer - which is
        // what deployPeer's direct `cat >` did, and no longer does.
        "rm -rf \"$d/plugins\"; [ -d \"$src/plugins\" ] && cp -r \"$src/plugins\" \"$d/plugins\"\n"
        "echo VIEWER_SERVE_OK\n";
}

// Update an already-installed peer (menu action): the same script with the
// early-out switched off.
//
// It used to DELETE that line from the finished script by searching for its
// text - surgery that stops working SILENTLY the day the line is reworded, and
// #268 is the rewording that would have done it (one `[ -x ]` test became a
// four-line --version check). One parameter instead, so the two forms cannot
// drift apart.
static std::string updateScript() { return bootstrapScript(false); }

// Does the peer over there RUN, and if not, WHY NOT in one line.
//
// #268: the loader's own words name the version the binary wanted and never
// the version the host has ("version `GLIBC_2.29' not found"), so a report
// that quoted them cost a round trip to answer - and the next report would
// have cost another one. The missing half is on the SERVER, where both are
// readable, so the probe says it there: ONE sentence, and the same sentence
// tools/binaries/update.sh prints, produced by the same expression.
//
// grep -a over the binary rather than objdump, for update.sh's reason: the
// machine that needs this sentence is a compute box with no build environment,
// which is exactly the machine with no binutils. `strings` is the fallback,
// and sort's stderr is dropped so a macOS peer (no Mach-O GLIBC_ strings, no
// sort -V) says nothing instead of something confusing.
//
// THE DIAGNOSIS IS CHOSEN FROM WHAT HAPPENED, never assumed. The glibc
// sentence used to be printed whenever the binary contained a GLIBC_ string at
// all - which every build does - so a $HOME mounted noexec, a mode-644 copy, an
// x86_64 peer on an aarch64 box (Exec format error) and a half-finished
// download all reported "needs GLIBC_2.17 but this host has 2.35": a statement
// that is FALSE about a host whose glibc is newer than required. docs/guides
// call this line the machine's answer and tell the user to report it, so a
// wrong one redirects the next bug report instead of merely confusing.
//
// The failure text is therefore CAPTURED and echoed first (it is the only thing
// in the room that knows), and the glibc sentence is added only when that text
// is about glibc. tools/binaries/update.sh's peer_starts has the same gate and
// quotes the same expression, for the reason both exist: two diagnoses of one
// failure must not disagree.
//
// `orElse` is what the caller wants printed when the peer is not there at all
// - the marker the NO_PEER / "does not run" branches below read.
static std::string peerProbeScript(const std::string& orElse) {
    return
        "p=\"$HOME/.viewer/viewer-serve\"\n"
        "if why=$(\"$p\" --version 2>&1); then\n"
        "  echo \"$why\"\n"
        "else\n"
        "  echo \"$why\"\n"
        "  case \"$why\" in *GLIBC_*)\n"
        "    need=$({ LC_ALL=C grep -ao 'GLIBC_[0-9.]*' \"$p\" 2>/dev/null"
        " || strings -a \"$p\" 2>/dev/null; }"
        " | grep -o 'GLIBC_[0-9.]*' | sort -Vu 2>/dev/null | tail -1)\n"
        // `ldd --version 2>&1` read straight into the sentence made a host with
        // no ldd say "...but this host has sh: ldd: not found" - the redirect
        // turns the shell's complaint into something that LOOKS like an answer,
        // so the emptiness test could never fire. Ask if the tool exists.
        "    if command -v ldd >/dev/null 2>&1; then have=$(ldd --version 2>&1 | head -1);"
        " else have=''; fi\n"
        "    [ -n \"$have\" ] || have='an unknown libc (no ldd on this host)'\n"
        "    [ -n \"$need\" ] && echo \"viewer-serve needs $need but this host has $have\"\n"
        "  ;; esac\n"
        "  " + (orElse.empty() ? std::string(":") : orElse) + "\n"
        "fi\n";
}

// Install the peer, best method first. Returns a human-readable log either way.
bool deployPeer(const std::string& host, int port, bool force, std::string& log) {
    std::string out, err;
    // 1. what machine is over there, and is a WORKING peer already in place?
    //    (--help must actually run: a peer that was copied but dies on a glibc
    //    mismatch reported only "connection lost" before this check existed)
    if (!remote::runSshScript(host, port,
            "uname -s; uname -m\n" + peerProbeScript("echo NO_PEER"),
            out, err, 20.0)) {
        log = "cannot reach " + host + ": " + err;
        return false;
    }
    // "it runs" is not "it is current": an old peer answers --help happily and
    // then cannot serve MEASURE. Compare the protocol version it reports.
    int peerProto = 0;
    size_t vp = out.find("viewer-serve protocol ");
    if (vp != std::string::npos) peerProto = atoi(out.c_str() + vp + 22);
    if (!force && peerProto >= (int)rp::VERSION) {
        log = "peer already installed (protocol v" + std::to_string(peerProto) + ")";
        return true;
    }
    if (!force && peerProto == 0 && out.find("NO_PEER") == std::string::npos &&
        out.find("not found") == std::string::npos)
        log = "the installed peer is older than this viewer; replacing it\n";
    bool isLinux = out.find("Linux") != std::string::npos;
    bool isDarwin = out.find("Darwin") != std::string::npos;
    if ((isLinux && out.find("x86_64") == std::string::npos) ||
        (isDarwin && out.find("arm64") == std::string::npos) ||
        (!isLinux && !isDarwin)) {
        log = "no prebuilt peer for this machine:\n" + out +
              "(binaries exist for Linux x86_64 and macOS arm64; build viewer-serve "
              "from source on the server and place it at ~/.viewer/viewer-serve)";
        return false;
    }

    // 2. hand it our own copy - no network and no credentials needed on the far
    //    side, which is what a lab compute box usually has
    std::string local = findLocalPeer(out);
    if (!local.empty()) {
        std::string bytes;
        if (readWholeFile(local, bytes)) {
            std::string o2, e2;
            bool ok = remote::runSshCommand(host, port,
                "sh -c 'mkdir -p ~/.viewer && cat > ~/.viewer/viewer-serve.new && "
                "chmod +x ~/.viewer/viewer-serve.new && "
                "mv ~/.viewer/viewer-serve.new ~/.viewer/viewer-serve && echo VIEWER_SERVE_OK'",
                bytes, o2, e2, 120.0);
            if (ok && o2.find("VIEWER_SERVE_OK") != std::string::npos) {
                // PROVE it runs before calling this a success: a binary built on
                // a newer distro dies on glibc with nothing but "connection
                // lost" further down. Surface the loader's actual words.
                std::string vout, verr3;
                // The RETURN VALUE, not just the text. When the probe times out
                // or the link drops, `vout` is empty and the sentence below
                // read "but it does not run there:" with nothing after the
                // colon - an accusation against a peer that was never asked.
                // "we could not check" and "we checked and it is broken" are
                // different facts and the install must not report the second
                // when it means the first.
                const bool asked = remote::runSshScript(host, port, peerProbeScript(""),
                                                        vout, verr3, 15.0);
                if (!asked) {
                    log = "copied to " + host + ", but the check that it RUNS there did "
                          "not come back (" + (verr3.empty() ? "no reason given" : verr3) +
                          ") - the peer may well be fine; nothing here can say so. "
                          "Try again, or run ~/.viewer/viewer-serve --version over ssh "
                          "by hand.";
                    return false;
                }
                size_t vp2 = vout.find("viewer-serve protocol ");
                if (vp2 == std::string::npos) {
                    log = "copied to " + host + ", but it does not run there:\n" + vout +
                          "\n(usually an OS/glibc mismatch - run update.cmd in viewer-bin "
                          "to get the compat build, or build viewer-serve on the server: "
                          "cmake --build <dir> --target viewer-serve)";
                    return false;
                }
                // "it runs" is still not "it is current". The number is on the
                // wire and the parse is twenty lines up (peerProto); without
                // comparing it, a stale local viewer-serve - an out-of-date
                // viewer-bin checkout, which findLocalPeer PREFERS over a
                // same-dir build - was copied, toasted "remote peer updated",
                // reconnected, and then served pre-v4 grouping and pre-v5
                // pattern text for the rest of the session with nothing on
                // screen saying why (autoUpdateTried stops any retry).
                int newProto = atoi(vout.c_str() + vp2 + 22);
                if (newProto < (int)rp::VERSION) {
                    log = "copied to " + host + ", but it reports protocol " +
                          std::to_string(newProto) + " (this viewer speaks " +
                          std::to_string((int)rp::VERSION) + ") - the local peer binary "
                          "at " + local + " is stale: run update.cmd in viewer-bin";
                    return false;
                }
                // plugins too, so server-side MEASURE has the same analyzers
                std::filesystem::path pdir = std::filesystem::u8path(local).parent_path() / "plugins";
                std::error_code ec;
                if (std::filesystem::exists(pdir, ec)) {
                    for (auto& e : std::filesystem::directory_iterator(pdir, ec)) {
                        std::string pb;
                        if (!e.is_regular_file(ec) || !readWholeFile(e.path().u8string(), pb)) continue;
                        std::string o3, e3;
                        // .new + mv, like the binary two blocks up: `cat >`
                        // truncates the .so IN PLACE, and a live peer that has
                        // run one MEASURE has it mapped (serve.cpp dlopens
                        // exactly ~/.viewer/plugins) - truncating under the
                        // mapping is a SIGBUS in the peer.
                        std::string pn = e.path().filename().u8string();
                        remote::runSshCommand(host, port,
                            "sh -c 'mkdir -p ~/.viewer/plugins && cat > "
                            "~/.viewer/plugins/" + pn + ".new && "
                            "mv ~/.viewer/plugins/" + pn + ".new "
                            "~/.viewer/plugins/" + pn + "'",
                            pb, o3, e3, 60.0);
                    }
                }
                log = "sent the peer from this machine (" + local + ")";
                return true;
            }
            log = "could not copy the peer to " + host + ": " + (e2.empty() ? o2 : e2);
            // fall through and let the server try git
        }
    }

    // 3. last resort: the server fetches it itself (needs git + network + access)
    std::string o4, e4;
    if (!remote::runSshScript(host, port, force ? updateScript() : bootstrapScript(),
                              o4, e4, 90.0)) {
        log += (log.empty() ? "" : "\n") + std::string("server-side install: ") +
               (e4.empty() ? "failed" : e4);
        if (local.empty())
            log += "\nno local viewer-serve to send either - looked next to " + app.exePath;
        return false;
    }
    if (o4.find("VIEWER_SERVE_OK") == std::string::npos) {
        log += (log.empty() ? "" : "\n") + o4;
        return false;
    }
    // PROVE it runs, exactly as the send-our-own path above does. This path had
    // no such check: it cloned the binaries branch, saw its own `cp` succeed,
    // printed VIEWER_SERVE_OK and reported an install - so a peer that cannot
    // start on this host was installed and CALLED INSTALLED, and the only
    // symptom left was "no answer from the remote viewer" at the next connect.
    // Two ways to fail, and they are different sentences: it does not start
    // (the loader's words, plus which glibc is missing), or it starts and is
    // older than this viewer (the branch it cloned is behind). #268.
    std::string vout5, verr5;
    // ...and the return value here for the same reason as in step 2: a probe
    // that never landed leaves vout5 empty, and "it does not run there:" with
    // an empty line under it blames the peer for our own timeout.
    if (!remote::runSshScript(host, port, peerProbeScript(""), vout5, verr5, 15.0)) {
        log += (log.empty() ? "" : "\n") +
               std::string("the server installed its own peer, but the check that it "
               "RUNS there did not come back (") +
               (verr5.empty() ? "no reason given" : verr5) + ") - nothing here can say "
               "whether the install worked. Try again, or run "
               "~/.viewer/viewer-serve --version over ssh by hand.";
        return false;
    }
    size_t vp5 = vout5.find("viewer-serve protocol ");
    if (vp5 == std::string::npos) {
        log += (log.empty() ? "" : "\n") +
               std::string("the server installed its own peer, but it does not run there:\n") +
               vout5;
        return false;
    }
    int gotProto = atoi(vout5.c_str() + vp5 + 22);
    if (gotProto < (int)rp::VERSION) {
        log += (log.empty() ? "" : "\n") + std::string("the server installed its own peer, but it "
               "reports protocol ") + std::to_string(gotProto) + " (this viewer speaks " +
               std::to_string((int)rp::VERSION) + ") - the `binaries` branch it cloned is "
               "older than this viewer";
        return false;
    }
    log = "the server installed its own peer (protocol " + std::to_string(gotProto) + ")";
    return true;
}


// The UI thread's OWN connection to a host, used only by openRemote's meta+tile.
// It deliberately does NOT install the peer: deployPeer is minutes of ssh and
// the browse worker already owns that job (rbWorker's connect path). The most
// this can cost the window is one ssh handshake, bounded by ConnectTimeout=10 -
// and that number is UNCHANGED by the peer diagnosis added for #268, because
// this session never opts into it (remote.h, Session::setExplainFailure,
// default false). Turning it on here would make an unreachable host cost two
// handshakes plus the probe's own budget with the window not repainting, so
// the sentence above is a constraint on this function and not a description:
// whatever else ensureUiSession gains, it stays at one round trip.
static bool ensureUiSession(const std::string& host, std::string& err, int port) {
    if (app.uiSession.alive() && app.uiSession.host() == host &&
        app.uiSession.port() == port)
        return true;
    // the peer is the same binary: ~/.viewer/viewer-serve over ssh, or this very
    // executable when testing through local:// (--remote-exe overrides both,
    // which is how the standalone viewer-serve peer gets exercised)
    std::string exe = (host.empty() && app.remoteExe.empty()) ? app.exePath
                    : (app.remoteExe.empty() ? std::string(REMOTE_HOME) + "/viewer-serve"
                                             : app.remoteExe);
    // The window is not repainting while this session reads, so a dead link
    // must become an error in seconds rather than waiting out ssh's keepalive
    // (and a local:// peer has no keepalive at all). Progress resets the clock,
    // so a slow tile is not a timeout.
    app.uiSession.setIdleTimeout(30.0);
    if (app.uiSession.startOn(host, port, exe, err)) return true;
    if (!host.empty())
        err += "\n(the browse panel installs the peer; connect there first)";
    return false;
}

// ---- §16: HOW the remote half of a Reload reaches the peer -------------------
// A function the reload is HANDED rather than a Session it reaches for. This is
// watchPollRound's WatchLister one layer down, and it exists for that function's
// two reasons exactly:
//
//   THE ROUND TRIP HAS TO BE DRIVABLE. --rwatch-selftest runs against the
//   standalone `viewer-serve` over a pipe, and the local peer is reached with an
//   EMPTY host - which no remote url can spell (remote::parseUrl refuses
//   "ssh://" with no host, and makeRemoteUrl turns an empty host into local://,
//   which is LOCAL by §13.7 and reloads off this disk). Handing the session in
//   is what lets the branch below be exercised against the real peer instead of
//   a stub, which is the whole ethos of that file (R1).
//
//   ...and a resolver that ANSWERS NULL is how "the link is down" is tested
//   without unplugging anything. That is §13.4 one layer down: no answer is not
//   an empty directory, and the rebuild has to fail towards doing NOTHING.
//
// Empty = the program's own answer: app.uiSession, the connection the open used,
// read on the thread that owns it (§3's "one Session, one owning thread").
//
// ...and that last sentence is now exactly why the RELOAD WORKER hands one in
// (the 2026-09-28 report). A reload no longer runs on the UI thread, and
// app.uiSession belongs to the UI thread and to nobody else, so
// reloadJobWorker's resolver answers with a Session of its OWN - dialled once,
// never re-dialled after the link drops. The empty case therefore stopped being
// "what a reload does" and became "what a reload does when something is
// deliberately driving it synchronously" (a scripted run, --reload-selftest).
//
// The ALIAS lives in state.h, beside App::ReloadJob, which carries one. The
// decision record stays here, where the resolver is used.
static remote::Session* reloadPeerFor(const ReloadPeer& how, const std::string& host,
                                      int port, std::string& errOut) {
    if (how) return how(host, port, errOut);
    if (!ensureUiSession(host, errOut, port)) return nullptr;
    return &app.uiSession;
}

// The peer's directory holding `p`, with no trailing separator - and "" when it
// has none. A peer path is the SERVER's, so it is split as text: this machine's
// std::filesystem would apply this machine's rules to a name that never touches
// this disk (a Windows client reading a Linux peer's "/data/cap/f_000.npy").
// Both separators, because the peer may be either.
//
// It lives here rather than in watch.inc (where it was born) because the
// membership rebuild below needs it and this fragment is included first. ONE
// spelling of "which folder is this peer path in", for the reason every other
// pair in this feature is one function: two would differ on a trailing slash
// exactly once, in the one place nobody reads.
static std::string remoteDirOf(const std::string& p) {
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : p.substr(0, cut);
}

std::string makeRemoteUrl(const std::string& host, const std::string& path, int port) {
    if (host.empty()) return "local://" + path;
    // A home-relative path ("~/data/x.npy") pasted straight after the host would
    // read as part of the HOST ("ssh://trc2~/data/..."), and the mis-split host
    // then looks like a machine we have never connected to - which is what made
    // picking a file in the browser answer "could not install the peer".
    // git's ssh:// extension spells it /~/rel, and parseUrl unwraps that.
    // The PORT is part of the locator, not a side channel. It used to be
    // dropped here, so browsing ssh://host:2222/data worked (RbJob carries the
    // port) while every url minted afterwards - ImageDoc::path, RFetchJob::url,
    // MJob::url, the session file's npy3 line - said port 22. The fetch, the
    // server-side MEASURE and every session restore then connected to whatever
    // answered on 22, and the peer-install fallback would deploy there.
    std::string p = path;
    if (!p.empty() && p[0] != '/') p = "/" + p;
    return "ssh://" + host + (port > 0 ? ":" + std::to_string(port) : "") + p;
}

// This stack's subject when it is ONE ARRAY INSIDE A MATERIALISATION - a node
// of what a reader produced on the peer, or a member of a container it listed
// (protocol 12/13, measured since 14). It is read off the DOCUMENTS rather than
// off SeqInfo because that is where the four fields live and are kept in step
// (remoteTreeRefine writes them, rfInheritRead carries them to every sibling
// frame): a stack whose head says "key k, node n" IS that array, and a second
// copy of the answer on the stack row is a second thing to keep true.
//
// It is also what makes a reader's stack answerable at all. The documents a
// tree lands as are built by vnzBuild, so their SeqInfo carries no remoteUrl -
// nothing about the stack row says "these pixels are over there" - and without
// this the one thing the link exists for would be the one thing that stack
// could not do.
static const FrameSource* stackKeyedSubject(int seqId) {
    if (seqId == 0) return nullptr;
    for (const auto& d : app.images)
        if (d->seqId == seqId && d->src && !d->src->remoteKey.empty() &&
            !d->src->remoteUrl.empty())
            return d->src.get();
    return nullptr;
}

// Does analysis of this stack run on the server? Yes for remote data unless the
// user chose local-fetch. (Single-frame policy nuance lives in the Analysis
// panel; this is the stack-aggregate decision.)
static bool serverComputes(const App::SeqInfo& si) {
    if (si.remoteUrl.empty() && si.remoteFiles.empty() && !stackKeyedSubject(si.id))
        return false;                                                   // local data
    return app.procPolicy != App::PolLocalFetch;
}

static uint64_t g_measureToken = 1;

// Fire the server-side temporal aggregate for a remote stack, if policy allows.
// The result feeds the Temporal panel with a [server, N frames] tag, in seconds,
// without waiting for any frame transfer.
// `into` is the slot the answer lands in: app.srvTemporal for A (automatic),
// app.srvTemporalB for the compare side (button only - see the struct).
static void requestServerTemporal(int seqId, App::ServerTemporal& S) {
    App::SeqInfo* si = nullptr;
    for (auto& s : app.seqs) if (s.id == seqId) { si = &s; break; }
    if (!si || !serverComputes(*si)) return;
    S = App::ServerTemporal{};
    S.seqId = seqId;
    S.token = g_measureToken++;
    S.pending = true;
    // whole-frame: no ROI is put on the wire (mWorker only sends one when
    // rw > 0). Recorded so the labels can say so instead of borrowing the
    // local side's ROI state.
    S.rx = S.ry = S.rw = S.rh = 0;
    S.roiUsed = false;
    App::MJob j;
    j.token = S.token;
    j.op = rp::MOP_TEMPORAL_STATS;
    j.cfaType = si->cfaType; j.cfaPattern = si->cfaPattern;
    // The subject, in the order that decides it. A KEY wins over both path
    // forms and is not merely added to them: for a .npz member the url names
    // the CONTAINER, so a request that also carried the path would have the
    // peer measure the whole file - which is what it did before protocol 14,
    // and it came back as "not a .npy file" rather than as a wrong number only
    // because a container happens not to open as one array.
    if (const FrameSource* kd = stackKeyedSubject(seqId)) {
        j.url = kd->remoteUrl;        // the HOST to ask; no path goes on the wire
        j.key = kd->remoteKey;
        j.node = kd->remoteNode;
        j.keyKind = kd->remoteKeyKind;
        j.keyedRead = kd->remoteKeyRead;
        j.keyLayout = kd->remoteKeyLayout;
        j.keyRequires15 = kd->remoteKeyRequires15;
    } else if (!si->remoteFiles.empty()) {
        j.url = makeRemoteUrl(si->remoteHost, si->remoteFiles[0], si->remotePort);
        for (const auto& f : si->remoteFiles) j.files.push_back(f);
    } else {
        j.url = si->remoteUrl;        // already carries its port
    }
    // A headerless stack declares its geometry on every request, so the
    // measurement carries the SAME six numbers the open did - read off the
    // stack's own head document, which is where they were recorded (§6.2). A
    // measurement made under a different recipe than the picture on screen
    // would be a number about other pixels, wearing this stack's name.
    for (const auto& d : app.images)
        if (d->seqId == seqId && d->src->rawDtype >= 0) {
            RawDialog rd;
            rd.dtype = d->src->rawDtype; rd.interp = d->src->rawInterp;
            rd.w = d->src->srcW > 0 ? d->src->srcW : d->w;
            rd.h = d->src->srcH > 0 ? d->src->srcH : d->h;
            rd.offset = d->src->rawOffset; rd.littleEndian = d->src->rawLE;
            j.recipe = rawWireFrom(rd);
            j.hasRecipe = true;
            break;
        }
    mEnqueue(std::move(j));
}
static void maybeRequestServerTemporal(int seqId) {
    requestServerTemporal(seqId, app.srvTemporal);
}

// The A slot follows the SELECTION.
//
// Every door that mints a remote stack fires the aggregate for it at that
// moment - which answers the whole question while a stack is a stack. A TREE is
// not: one open lands several, and only one of them can hold the single A slot.
// So the panel asks on behalf of the stack it is drawing.
//
// THE SLOT IS THE RECORD, and there is no second one kept on the stack. A
// per-stack "has been asked" bit meant a stack asked once was never asked
// again, so looking B -> A -> B left the slot holding A: the panel drops the
// server row when it is not this stack's (S.seqId != si->id) and B fell back to
// computing locally - "remote when it can be remote", inverted by looking away
// and back. The slot answers the only question there is - is the number being
// held this stack's? - and it is already what every other re-fire here keys off
// (the mosaic change below, the reload walk, and the close that empties it).
//
// So a return trip costs one measurement, which is the price of the answer
// reaching the screen at all. Nothing storms: the request sets the slot's seqId
// synchronously, so the line above returns on every frame after this one.
static void ensureServerTemporalFor(int seqId) {
    if (app.srvTemporal.seqId == seqId) return;      // it is already this one's
    requestServerTemporal(seqId, app.srvTemporal);   // ...and it refuses non-server stacks
}

// The Inspector changed a frame's mosaic. The stack it belongs to carries that
// mosaic to the server (SeqInfo::cfaType is what requestServerTemporal sends),
// so the whole stack follows and any server aggregate is re-fired: a sigma
// computed with the wrong plane split is a wrong number, not a stale one.
static void seqMosaicChanged(const ImageDoc* im) {
    if (!im || im->seqId == 0) return;
    App::SeqInfo* si = nullptr;
    for (auto& s : app.seqs) if (s.id == im->seqId) { si = &s; break; }
    if (!si) return;
    if (si->cfaType == im->cfa && si->cfaPattern == im->cfaPattern) return;
    si->cfaType = im->cfa;
    si->cfaPattern = im->cfaPattern;
    for (auto& d : app.images)
        if (d->seqId == si->id && d.get() != im) {
            d->cfa = im->cfa; d->cfaPattern = im->cfaPattern; d->texDirty = true;
        }
    if (app.srvTemporal.seqId == si->id) requestServerTemporal(si->id, app.srvTemporal);
    if (app.srvTemporalB.seqId == si->id) requestServerTemporal(si->id, app.srvTemporalB);
}

// ---- reload from disk (docs/reference-design.md §3.2) --------------------------
// Why "Reload from disk" cannot serve THIS source - the sentence the Files
// right-click shows where the click would have been - or "" when it can.
//
// ONE predicate, and it exists because there were two. The menu item greys
// itself out, and it did so from its own transcription of reloadSource's
// refusals, sitting in another file: the two lists agreed only as long as
// nobody taught the walk something new. That is not a hypothetical - the
// picture formats were greyed out here for exactly as long as they were
// refused there, and the day the walk learned to reload a .tif this copy would
// still have been saying "not wired yet" over a working reload. The refusals
// are a fact about reloadSource, so they are stated once, next to it.
//
// Order matches reloadSource's own, so the reason a user is shown is the reason
// the function would have given.
static std::string reloadUnavailable(const FrameSource& S) {
    // local:// is LOCAL: a Browse-panel local open carries the on-disk path
    // inside its url, so those pixels ARE reloadable off this disk.
    const std::string onDisk = localDiskPathOfUrl(S.remoteUrl);
    // ...and the row that USED to be here - "remote-backed: these pixels come
    // from the peer, not from a local file" - is gone, because it stopped being
    // true (§16). reloadSource now has the peer's equivalent of "stat it and
    // read it again": META then TILE, the same two round trips the open made,
    // on the same connection. A refusal is a fact about reloadSource and this
    // function states reloadSource's facts, so removing the branch there
    // removes the row here - which is the whole reason there is one list.
    //
    // What is NOT decided here is whether the peer will ANSWER. That is a fact
    // about a link at a moment, and this predicate greys a menu item by
    // inspection, on the UI thread, every frame: asking the network would be a
    // round trip per repaint, and the answer would still be stale by the time
    // the click arrived. An unreachable peer is a reload that FAILS, with the
    // reason on the row and #56's mark saying nothing was re-read.
    // WHICH string is the file: a local:// url carries it inside itself, and
    // everything else - no url at all, or a peer's - keeps it in `path`. Not
    // "url empty ? path : onDisk": a peer source has a non-empty url AND a
    // non-empty path (the peer's), so that form refuses every peer by name.
    if ((isPeerUrl(S.remoteUrl) || S.remoteUrl.empty() ? S.path : onDisk).empty())
        return "no file behind these pixels - they were composed\n"
               "here (montage / plugin result).";
    if (S.member.rfind("__pixels_", 0) == 0)
        return "a container/reader member: the file rebuilds\n"
               "as a unit - re-open it to reload.";
    // (no row for the picture formats: they go through the walk now. The three
    // above are the whole list, and it is short on purpose - anything a re-read
    // of the file can answer belongs in reloadSource, not here.)
    return {};
}

// Everything computed FROM a stack that a change to that stack kills, in one
// place. Called once per affected stack by the reload walk below (a member's
// pixels were swapped) and once by the membership rebuild (the member LIST
// moved) - the two are the same event as far as anything derived from the stack
// is concerned, and a second copy of this list is how one of them would come to
// be missing a row. Returns true when a linearity row was dropped, so a whole
// walk can call linFitStale ONCE.
static bool invalidateStackDerived(int sid) {
    bool linHit = false;
    if (App::SeqInfo* si = seqInfo(sid)) si->stackRev++;   // the temporal key
    // §10.1: reduced frames (frame average / sum) recorded this key at compute
    // time. The bump is the event that makes them describe pixels that no longer
    // exist, so the mark is LATCHED here, at the event - a stack reloaded and
    // then closed cannot take the fact with it. The picture itself never
    // recomputes behind the user: silent recompute is silent overwrite, and
    // Recompute is the user's move.
    for (auto& rd : app.images)
        if (!rd->avgOfPath.empty() && rd->avgSeqId == sid) rd->avgStale = true;
    // ...and the same event for a preprocessor product, which is the same kind
    // of thing one layer up: a whole stack computed from this one, now
    // describing pixels the disk no longer holds. Latched, not recomputed -
    // remaking it is the user's move, from the source stack's own menu.
    for (auto& ps : app.seqs)
        if (ps.dt.made() && ps.dt.srcSeqId == sid) ps.dt.stale = true;
    // a server aggregate naming this stack now describes dead pixels: re-fire
    // it (same shape as seqMosaicChanged above)
    if (app.srvTemporal.seqId == sid) requestServerTemporal(sid, app.srvTemporal);
    if (app.srvTemporalB.seqId == sid) requestServerTemporal(sid, app.srvTemporalB);
    // the linearity rows measured over this stack describe the old pixels: drop
    // them - rows of untouched stacks keep their live numbers - and the fit that
    // included them goes too (closeStack's discipline)
    for (auto it = app.lin.rows.begin(); it != app.lin.rows.end();)
        if (it->seqId == sid) { it = app.lin.rows.erase(it); linHit = true; }
        else ++it;
    return linHit;
}

// THE one place pixels are replaced in place. Re-decodes a source from its own
// provenance (path + member + fileFrame + raw recipe), swaps the result into
// the SAME FrameSource, then walks every membership that shares it: dataRev,
// mirrors, forgetImage, texDirty per doc; stackRev, server-temporal re-fire and
// the linearity rows per affected stack. Numbers share the fate of the pixels
// they describe (5092c4b).
//
// Shape-change policy: reload means "same frame, new values". If the file now
// holds different w/h/ch/dtype the reload PROCEEDS - what is on disk is a fact,
// not an error - the mirrors resync and *noteOut says what changed, so the
// caller's notice can. What DOES refuse: no origin behind the pixels at all
// (a montage, a plugin output), an unreadable or undecodable file, a peer that
// does not answer, a vanished named part (an .npz member, an .exr layer), and a
// picture index past the file's new picture count - an .npy frame, a TIFF page,
// a y4m frame or a peer's frame that no longer exists, and there is no "same
// frame" to give new values to. On any failure the source is untouched: old
// pixels stay, never a half-swap.
//
// A PEER's pixels are NOT on that list (watch-design §16): the walk has the
// peer's own "stat it and read it again" - META then TILE, over a link of its
// own - so a remote-backed source reloads, and fails only when the link does.
//
// WHICH DECODER re-reads the file is decided by what the PIXELS are, in that
// order, and never by the file name alone: a reader/container member first (it
// rebuilds as a unit), then a declared raw recipe (the recipe is what those
// pixels are), then the picture seam, then .npy/.npz. A source that came in
// through one door must not go out through another - the name is the last
// thing consulted, and only for sources no earlier test claimed.
//
// A cropped source re-applies its recorded crop rect to the fresh decode (the
// crop is part of what these pixels ARE - §2.1 keeps it on the source); the
// rect is clamped if the file shrank. CFA re-snapping is not attempted: the
// recorded rect was snapped when the crop was made, and the mosaic is a
// per-membership interpretation the source cannot see.
//
// ...and it is SPLIT IN TWO, for the reason the whole of it used to be on the
// UI thread (the 2026-09-28 report 「reload を押すと応答なしになる」):
//
//   reloadDecodeGroup  the stat, the bytes, the decode, the crop and the
//                      measurement. Pure - no app.images, no publication, no
//                      GL - and therefore a worker's. It takes a GROUP of
//                      memberships served by ONE read, which is the second
//                      half of the same report: a frame-axis stack is one
//                      FrameSource per frame, so the old walk read a 480 MB
//                      file four times over and decoded eight frames to land
//                      four (V22f's pathology, fixed for the open in #232
//                      stage 2 and not for the reload).
//   reloadLand         the dims note, the new registry key, the swap under
//                      g_srcRegMtx, Watch's re-seed and the invalidation walk.
//                      Every one of those touches shared state, so every one
//                      of them is the UI thread's, and together they are what
//                      the old function did in its last hundred lines.
//
// reloadSource below is still THE DOOR and still synchronous: capture, decode,
// land. Every existing caller and all of core/selftest/reload.inc therefore go
// on passing unchanged, and the asynchronous door (startStackReload) is built
// out of the same three calls in the same order. openLocalLand with a null
// OpenPre is the precedent.

// WHAT these pixels are, by value, taken on the UI thread. See ReloadSpec.
static ReloadSpec reloadSpecOf(const FrameSource& s) {
    ReloadSpec r;
    r.path = s.path;
    r.remoteUrl = s.remoteUrl;
    r.member = s.member;
    r.fileFrame = s.fileFrame;
    r.remoteFrame = s.remoteFrame;
    r.remoteStep = s.remoteStep;
    r.npyRead = s.npyRead;
    r.rawDtype = s.rawDtype; r.rawInterp = s.rawInterp;
    r.rawOffset = s.rawOffset; r.rawLE = s.rawLE;
    r.w = s.w; r.h = s.h; r.ch = s.ch;
    r.srcW = s.srcW; r.srcH = s.srcH; r.cropX = s.cropX; r.cropY = s.cropY;
    r.dtype = s.dtype;
    return r;
}

// The FILE behind a spec. local:// is LOCAL - a Browse-panel local open carries
// the on-disk path inside its url, and the pixels are on this disk, which is
// strictly more than a peer can tell us about them (§13.7). A real remote
// scheme keeps its url and goes through the peer's door.
static std::string reloadSpecFile(const ReloadSpec& s) {
    const std::string onDisk = localDiskPathOfUrl(s.remoteUrl);
    return isPeerUrl(s.remoteUrl) || s.remoteUrl.empty() ? s.path : onDisk;
}

// Can one stat + one read + one decode serve this membership ALONGSIDE others?
// "" = no, this one is read on its own. The key is the READ's identity and
// nothing else: the file, the array or part inside it, and the §3.3 reading the
// pixels were born under (a second reading of one file is a second decode of
// the same bytes, so it is a second group and not a second read).
//
// The three doors that answer "" do so because there is nothing to share:
// a container/reader member is refused outright, a raw recipe is one file and
// one frame by construction, and a PEER's frame is its own META+TILE round
// trip - one file on the other machine is not one transfer.
static std::string reloadReadKey(const ReloadSpec& s) {
    if (s.member.rfind("__pixels_", 0) == 0) return {};
    if (isPeerUrl(s.remoteUrl)) return {};
    if (s.rawDtype >= 0) return {};
    const std::string f = reloadSpecFile(s);
    if (f.empty()) return {};
    return f + "\n" + s.member + "\n" + std::to_string(s.npyRead);
}

// Which frame of the file a spec IS, for the grouping above: two memberships of
// one group must not both be handed the same decode (a crop split one source
// into two, and the second would find the document already moved out). They go
// in separate groups instead, which costs one extra read in a case a stack has
// to have been cropped member-by-member to reach, and costs nothing at all in
// the case this whole change is about.
static int reloadPickIndex(const ReloadSpec& s) { return s.fileFrame; }

// ---- the DECODE half, for a group of memberships served by ONE read ---------
// `specs` all share reloadReadKey() and pick distinct frames, or there is
// exactly one of them (the ungroupable doors and the synchronous door). `pres`
// is parallel to it and is what the landing will be handed; a refusal is
// written into pre.err and the other specs of the group are unaffected.
//
// `cancel` is looked at between ITEMS and inside the decode loop, never inside
// one frame. `peer` resolves the link for the peer door - the worker's own
// Session, or the program's (§16).
static void reloadDecodeGroup(const std::vector<const ReloadSpec*>& specs,
                              const std::vector<ReloadPre*>& pres,
                              const std::atomic<bool>* cancel,
                              const ReloadPeer& peer) {
    if (specs.empty()) return;
    const ReloadSpec& s0 = *specs.front();
    const std::string fpath = reloadSpecFile(s0);
    auto refuseAll = [&](const std::string& why) {
        for (ReloadPre* p : pres) p->err = why;
    };
    // The crop a membership recorded, re-applied to ITS fresh decode. The crop
    // is part of what these pixels ARE (§2.1 keeps it on the source), the rect
    // is clamped if the file shrank, and the CFA snap is not re-attempted: the
    // rect was snapped when the crop was made, and the mosaic is a
    // per-membership interpretation the source cannot see. cropPixels, not
    // cropInPlace: nd is private to this thread, so there is no source to
    // clone, no registry slot to vacate and no stack revision to move - and no
    // lock to take on a worker (see cropPixels).
    auto recrop = [](const ReloadSpec& s, ImageDoc& nd) {
        // cropInPlace's OWN refusal, and it has to be the same predicate rather
        // than a second opinion (review P2-3). A source still showing a
        // DECIMATED remote tile refuses a crop by name: the full-resolution
        // fetch is in flight and its landing replaces data/w/h wholesale,
        // knowing nothing of crop bookkeeping - so a rect applied to the
        // preview grid is silently reverted by that swap and leaves srcW
        // stamped with preview dimensions. That is a phantom crop (isCropped
        // forever true), an unshareable source, and a session line carrying
        // garbage rectangles, and #272's R29 pins its absence. Dropping the
        // guard while extracting cropPixels would have re-created it on the one
        // path that re-applies a RECORDED rect.
        if (s.remoteStep > 1) return;
        if (s.srcW > 0 && (s.cropX != 0 || s.cropY != 0 || s.w != s.srcW || s.h != s.srcH))
            cropPixels(nd, s.cropX, s.cropY, s.w, s.h);
    };
    if (fpath.empty()) { refuseAll("no file behind these pixels"); return; }
    // Container/reader members (__pixels_*) rebuild as a UNIT: one __viewer
    // line restores the whole tree, and a reader file re-runs its adapter.
    // A per-member reload is not wired - and without this refusal a reader's
    // .dat origin would fall into the npz branch below, whose npzList would
    // then blame "not a zip file" for a failure that never happened.
    //
    // This test comes FIRST, before the recipe and the format table, because
    // its subject is where the pixels came from rather than what the file is
    // called: a reader's origin may perfectly well be named .tif, and reading
    // it as a TIFF would silently replace a reader's output with a decode of
    // the file it was run on.
    if (s0.member.rfind("__pixels_", 0) == 0) {
        refuseAll("container/reader members reload by re-opening the file");
        return;
    }
    if (isPeerUrl(s0.remoteUrl)) {
        // ---- §16: THE PEER's door - "stat it and read it again", over a link --
        //
        // The local doors below all say the same thing twice: statSourceFile
        // before the bytes, then a decode. The peer's two calls ARE that pair.
        // META is the stat (the shape the file declares NOW, and how many frames
        // it holds), TILE is the read, and they go out in that order for the
        // local doors' own reason - a file being rewritten under us binds the
        // OLD answer to the OLD pixels, which is the safe direction.
        //
        // Nothing about WHICH pixels these are is re-chosen: the frame index,
        // the §3.3 reading declaration and the decimation step are replayed off
        // the spec. A reload is "the same frame, new values" over the link
        // exactly as it is off the disk, and re-deriving any of the three would
        // make it "a different frame, silently".
        //
        // It no longer blocks the UI thread. It blocks the RELOAD WORKER, on
        // that worker's own Session, and that is what the whole of this file's
        // reload half was re-shaped for: openRemote's UiThreadStall was the
        // price of a manual gesture, and §16.6 argued it was not the automatic
        // one's - the answer taken in the end is that it is nobody's.
        ReloadPre& pre = *pres.front();
        const ReloadSpec& s = s0;
        std::string host, rpath;
        int port = 0;
        if (!remote::parseUrl(s.remoteUrl, host, rpath, &port)) {
            pre.err = "not a remote url: " + s.remoteUrl;
            return;
        }
        remote::Session* ses = reloadPeerFor(peer, host, port, pre.err);
        if (!ses) {
            if (pre.err.empty()) pre.err = "the peer did not answer";
            return;
        }
        // The DECLARED GEOMETRY, replayed off the spec exactly as npyRead and
        // the step are, and for the same reason one line down: re-deriving any
        // of the three would make a reload "a different frame, silently". A
        // headerless reload that dropped it was not a different frame - it was
        // the peer's refusal, on a document already on screen, every time the
        // button was pressed.
        rp::RawWire rrw{};
        // 0 for the CFA phase, and that is not a dropped declaration: the peer
        // ignores the phase, and reloadLand moves the PIXELS onto the documents
        // that are already open rather than replacing them - so the phase the
        // operator picked is never touched by a reload and there is nothing
        // here to carry. (`pre.nd` below is a carrier for samples only.)
        const bool haveRrw = rawWireOf(s.rawDtype, s.rawInterp,
                                       s.srcW > 0 ? s.srcW : s.w, s.srcH > 0 ? s.srcH : s.h,
                                       s.rawOffset, s.rawLE, 0, rrw);
        const rp::RawWire* rrwp = haveRrw ? &rrw : nullptr;
        remote::Meta m;
        if (!ses->meta(rpath, m, pre.err, s.npyRead, rrwp)) return;
        if (s.remoteFrame >= (int)m.frames) {
            // The same sentence and the same reasoning as the .npy frame axis
            // below: there is no "same frame" left to give new values to.
            pre.err = "file now has " + std::to_string(m.frames) + " frame(s); frame " +
                      std::to_string(s.remoteFrame) + " no longer exists";
            return;
        }
        const int step = std::max(1, s.remoteStep);
        std::vector<float> px;
        int tw = 0, th = 0, tch = 0;
        std::string dt;
        rp::F32Loss tloss;
        if (!ses->tile(rpath, s.remoteFrame, 0, 0, m.w, m.h, step,
                       px, tw, th, tch, dt, pre.err, s.npyRead, &tloss, rrwp))
            return;
        if (tw < 1 || th < 1 || tch < 1 || tch > 4) {
            pre.err = "the peer returned a degenerate frame";
            return;
        }
        pre.nd = std::make_unique<ImageDoc>();
        FrameSource& P = *pre.nd->src;
        P.path = s.path;                       // the url IS the provenance here
        P.remoteUrl = s.remoteUrl;
        P.remoteFrame = s.remoteFrame;
        P.remoteStep = step;
        P.npyShape = m.shape;                  // the PEER's disk, as it declares
                                               // it now (§3.3, the same field the
                                               // local branch takes off the file)
        P.npyRead = s.npyRead;                 // ...replayed, never re-chosen
        P.remoteProto = ses->peerVersion();
        // ...and the recipe, on the FRESH source too. Not decoration: the
        // landing re-keys the document off these fields (srcIdentityKey's raw
        // branch), so a reloaded headerless frame that dropped them would key
        // as an .npy tuple and could adopt someone else's pixels.
        if (haveRrw) {
            P.rawDtype = s.rawDtype; P.rawInterp = s.rawInterp;
            P.rawOffset = s.rawOffset; P.rawLE = s.rawLE;
            P.srcW = (int)rrw.w; P.srcH = (int)rrw.h;
            pre.nd->cfa = rp::rawInterpCfa(rrw.interp);
        }
        P.dtype = dt;
        P.w = tw; P.h = th; P.ch = tch;
        P.data = std::move(px);
        P.f32loss = tloss;                     // measured on THESE samples
        pre.nd->syncMirrors();
        recrop(s, *pre.nd);
        computeMinMax(*pre.nd);                // off the UI thread, as the open
        return;                                // measures its frames off it
    }
    if (s0.rawDtype >= 0) {
        ReloadPre& pre = *pres.front();
        const ReloadSpec& s = s0;
        RawDialog d;                       // the recipe, replayed against the file
        d.path = fpath;
        d.dtype = s.rawDtype; d.interp = s.rawInterp;
        d.offset = s.rawOffset; d.littleEndian = s.rawLE;
        d.w = s.srcW > 0 ? s.srcW : s.w;
        d.h = s.srcH > 0 ? s.srcH : s.h;
        if (s.cropX != 0 || s.cropY != 0 || s.w != d.w || s.h != d.h) {
            d.cropOn = true;
            d.cropX = s.cropX; d.cropY = s.cropY;
            d.cropW = s.w; d.cropH = s.h;
        }
        std::string err;
        pre.nd = decodeRawFrame(d, err);   // stats BEFORE reading, like every loader
        if (!pre.nd) { pre.err = err.empty() ? "decode failed" : err; return; }
        computeMinMax(*pre.nd);
        return;
    }
    // ---- the two LOCAL file doors, and the ONE read they share ---------------
    // identity BEFORE the bytes: an overwrite completing during this read binds
    // the OLD tuple to the OLD pixels - the safe direction. Once per GROUP,
    // which is once per file: a stat per membership of one file would be four
    // readings of one generation and would let two of them disagree.
    FrameSource st;
    st.path = fpath;
    statSourceFile(st);                    // local:// too: its embedded path IS
                                           // this disk, and the reloaded tuple
                                           // must move with it (an unmoved key
                                           // would let a later open adopt
                                           // pre-reload pixels)
    std::vector<uint8_t> bytes;
    if (!readFileBytes(fpath, bytes)) { refuseAll("cannot read file"); return; }
    if (cancel && cancel->load()) { refuseAll("stopped"); return; }
    if (const imagefile::Backend* fmt = imagefile::forPath(fpath)) {
        // A PICTURE FILE, back through the same seam it was opened through
        // (core/imagefile.h). The extension only chooses this door - what
        // decodes is decided by the bytes, exactly as on the way in, so a file
        // re-exported into a different encoding of the same format still
        // reloads and a .png that has become a JPEG still says so.
        //
        // AFTER the raw recipe on purpose: a .png the user chose to read
        // through the headerless dialog is those pixels because of the RECIPE,
        // and srcIdentityKey keys it by the recipe for the same reason. The
        // extension does not get to overrule a reading the user declared.
        //
        // The whole file still decodes to recover one picture - the container
        // is one object and picDecodeAll says why - but it decodes ONCE for
        // every membership of it, which is what a frame-axis stack of a
        // 100-page TIFF used to pay a hundred times over.
        std::vector<int> wanted;
        for (const ReloadSpec* s : specs) wanted.push_back(reloadPickIndex(*s));
        OpenDecoded dec;
        std::string err;
        err = picDecodeAll(fpath, bytes, st, s0.member, dec, cancel, &wanted);
        if (!err.empty()) { refuseAll(err); return; }
        for (size_t i = 0; i < specs.size(); i++) {
            const ReloadSpec& s = *specs[i];
            ReloadPre& pre = *pres[i];
            // Which picture of the file these pixels ARE - the provenance the
            // open recorded, replayed, never re-chosen. The two kinds are the
            // two the seam already distinguishes (core/imagefile.h): a NAMED
            // part is found by its name, an unnamed one by its position.
            size_t pick = (size_t)-1;
            if (!s.member.empty()) {
                for (size_t k = 0; k < dec.members.size(); k++)
                    if (dec.members[k] == s.member) { pick = k; break; }
                if (pick == (size_t)-1) {   // ...and a part that is gone is
                                            // refused BY NAME, the way a
                                            // vanished .npz member is
                    pre.err = std::string(fmt->partWord ? fmt->partWord : "part") +
                              " \"" + s.member + "\" is no longer in the file";
                    continue;
                }
            } else {
                if (s.fileFrame >= (int)dec.members.size()) {
                    // the same sentence, and the same reasoning, as the .npy
                    // frame axis: there is no "same frame" left to give new
                    // values to
                    pre.err = "file now has " + std::to_string(dec.members.size()) +
                              " frame(s); frame " + std::to_string(s.fileFrame) +
                              " no longer exists";
                    continue;
                }
                pick = (size_t)s.fileFrame;
            }
            if (!dec.docs[pick]) {
                pre.err = pick < dec.errs.size() && !dec.errs[pick].empty()
                              ? dec.errs[pick] : std::string("decode failed");
                continue;
            }
            pre.nd = std::move(dec.docs[pick]);
            recrop(s, *pre.nd);
        }
        return;
    }
    // .npy, plain or inside a container. The bytes of the ARRAY are extracted
    // once for the group, for the read's own reason: a stack of four
    // memberships of one .npz member used to inflate that member four times.
    std::vector<uint8_t> npy;
    if (!s0.member.empty()) {
        std::vector<NpzEntry> entries;
        std::string err;
        if (!npzList(bytes, entries, err)) { refuseAll(err); return; }
        bool found = false;
        for (const auto& e : entries)
            if (e.name == s0.member + ".npy") {
                if (!npzExtract(bytes, e, npy, err)) { refuseAll(err); return; }
                found = true;
                break;
            }
        if (!found) {
            refuseAll("array \"" + s0.member + "\" is no longer in the file");
            return;
        }
    } else {
        npy = std::move(bytes);
    }
    if (cancel && cancel->load()) { refuseAll("stopped"); return; }
    // re-decode under the RECORDED reading declaration (npyRead, §3.3), never a
    // guess made today: these pixels were born under that declaration, and the
    // frame count npyDecodeFrames reports is the file's count UNDER it - so the
    // refusal below states a true count. (A declaration the new shape cannot
    // mean falls back to native and SAYS so - npyLayout.)
    //
    // `wanted` is the other half of the report: the frames this group's
    // memberships actually are, and nothing else. Frame 0 is no longer decoded
    // on the way to frame 7 (npyDecodeFrames takes the count off the header),
    // and a three-frame subset of a 120-frame array decodes three.
    std::vector<int> wanted;
    for (const ReloadSpec* s : specs) wanted.push_back(reloadPickIndex(*s));
    OpenDecoded dec;
    {
        std::string err = npyDecodeFrames(npy, fpath, "", s0.npyRead, dec, cancel, &wanted);
        if (!err.empty()) { refuseAll(err); return; }
    }
    for (size_t i = 0; i < specs.size(); i++) {
        const ReloadSpec& s = *specs[i];
        ReloadPre& pre = *pres[i];
        if (s.fileFrame >= dec.frames) {
            pre.err = "file now has " + std::to_string(dec.frames) + " frame(s); frame " +
                      std::to_string(s.fileFrame) + " no longer exists";
            continue;
        }
        if (!dec.docs[(size_t)s.fileFrame]) {
            pre.err = !dec.errs[(size_t)s.fileFrame].empty()
                          ? dec.errs[(size_t)s.fileFrame] : std::string("decode failed");
            continue;
        }
        pre.nd = std::move(dec.docs[(size_t)s.fileFrame]);
        pre.nd->src->mtime = st.mtime;     // the Watch baseline, off the ONE stat
        pre.nd->src->fsize = st.fsize;
        recrop(s, *pre.nd);
    }
}

// One membership, on its own. The group of one, so there is no second copy of
// any decision above.
static void reloadDecode(const ReloadSpec& spec, ReloadPre& pre,
                         const std::atomic<bool>* cancel = nullptr,
                         const ReloadPeer& peer = ReloadPeer()) {
    reloadDecodeGroup({ &spec }, { &pre }, cancel, peer);
}

// ---- the LANDING half, and everything in it needs THIS thread ---------------
// The dims note, the new registry key, the swap under g_srcRegMtx, Watch's
// re-seed and the §3.2 invalidation walk. Nothing here reads a byte of the
// file: `pre` already holds a measured, cropped document, or the reason there
// is none.
static bool reloadLand(const std::shared_ptr<FrameSource>& src, const ReloadSpec& spec,
                       ReloadPre& pre, std::string& errOut, std::string* noteOut) {
    if (!src) { errOut = "no source"; return false; }
    if (!pre.nd) { errOut = pre.err.empty() ? "decode failed" : pre.err; return false; }
    std::unique_ptr<ImageDoc> nd = std::move(pre.nd);
    const FrameSource& N = *nd->src;
    if (noteOut && (N.w != spec.w || N.h != spec.h || N.ch != spec.ch ||
                    N.dtype != spec.dtype)) {
        char b[96];
        snprintf(b, sizeof b, "%dx%d %dch %s -> %dx%d %dch %s", spec.w, spec.h, spec.ch,
                 spec.dtype.c_str(), N.w, N.h, N.ch, N.dtype.c_str());
        *noteOut = b;
    }
    // The new key BEFORE the lock (it canonicalizes the path: filesystem IO -
    // see the g_srcRegMtx note), composed on an identity probe from values
    // already in hand - identity fields only move on this thread, so it
    // cannot go stale between here and the hold below. The OLD slot is erased
    // by src->regKey, the key it was actually created under: recomputing it
    // through today's filesystem could miss (see the note at regKey).
    std::string newKey;
    {
        FrameSource idNew;            // identity only - no pixels are copied
        idNew.path = src->path; idNew.remoteUrl = src->remoteUrl;
        idNew.member = src->member;
        idNew.fileFrame = src->fileFrame; idNew.remoteFrame = src->remoteFrame;
        idNew.rawDtype = src->rawDtype; idNew.rawInterp = src->rawInterp;
        idNew.rawOffset = src->rawOffset; idNew.rawLE = src->rawLE;
        idNew.srcW = N.srcW; idNew.srcH = N.srcH;
        // the fresh decode's shape with the declaration it replayed (N.npyRead
        // == src->npyRead: decodeNpyBuffer records what it was handed) - the
        // key normalizes them together (npyKeyRead), so a declaration the new
        // shape absorbed into native keys natively
        idNew.npyShape = N.npyShape; idNew.npyRead = N.npyRead;
        idNew.mtime = N.mtime; idNew.fsize = N.fsize;
        newKey = srcIdentityKey(idNew);
    }
    {   // the swap, in ONE hold of g_srcRegMtx: the worker reads a shared
        // source only inside that lock (find + syncMirrors in one section), so
        // no thread can see a half-swapped source. nd is PRIVATE here - the
        // crop it may have had was applied by cropPixels, which takes no lock
        // at all - so this scope acquires exactly one lock (see the note at
        // g_srcRegMtx).
        std::lock_guard<std::mutex> lk(g_srcRegMtx);
        // re-key: the tuple embeds mtime/fsize, so the reloaded source MOVES to
        // its new tuple's slot - a re-open of the changed file finds and shares
        // it, and a restore of the old bytes misses and re-decodes the disk.
        // Without this the stale row could hand post-reload pixels to a tuple
        // that never contained them.
        if (!src->regKey.empty()) {
            auto it = g_srcRegistry.find(src->regKey);
            if (it != g_srcRegistry.end() && it->second.lock() == src)
                g_srcRegistry.erase(it);
            src->regKey.clear();
        }
        // same srcId (identity), new pixels, rev +1, the PRE-read Watch baseline
        src->data = std::move(nd->src->data);
        src->w = N.w; src->h = N.h; src->ch = N.ch;
        src->dtype = N.dtype; src->vmin = N.vmin; src->vmax = N.vmax;
        src->srcW = N.srcW; src->srcH = N.srcH; src->cropX = N.cropX; src->cropY = N.cropY;
        src->npyShape = N.npyShape;   // the DISK's new shape: §3.3's menu must
                                      // offer what the file now permits.
                                      // npyRead stays: the declaration was
                                      // replayed, not re-chosen.
        src->rev++;
        src->mtime = N.mtime;
        src->fsize = N.fsize;
        src->f32loss = N.f32loss;     // what float32 cost THESE samples. It is a
                                      // census of the array being installed, so
                                      // it travels with vmin/vmax and not with
                                      // the identity fields above - the same
                                      // call pumpRemoteFetch makes when a full
                                      // frame replaces a preview's.
        srcRegistryAddLocked(src, newKey);   // the new tuple's slot. A live
                                      // occupant (the same tuple decoded fresh
                                      // meanwhile) wins: it already serves
                                      // future opens, and both hold identical
                                      // bytes.
    }
    // Watch's baseline just moved: these pixels ARE what the disk holds again,
    // and the mark that said otherwise has to stop being true. The watcher
    // learns it from the republished seed (App::watchTargetsDirty), not from a
    // message - which is why the flag is set HERE, in the one place a source's
    // mtime/fsize is ever rewritten, and not at each of the callers.
    app.watchTargetsDirty = true;
    // ---- the walk (§3.2): every membership sharing this source, in one step ----
    std::vector<int> seqIds;
    for (auto& d : app.images) {
        if (d->src != src) continue;
        d->dataRev++;                 // hist/proj/ROI/diff/auto-range keys unlatch
        d->syncMirrors();
        forgetImage(d.get());         // ana/hist/proj pointers + texLru + temporal
        d->texDirty = true;
        if (d->seqId != 0 && std::find(seqIds.begin(), seqIds.end(), d->seqId) == seqIds.end())
            seqIds.push_back(d->seqId);
    }
    bool linHit = false;
    for (int sid : seqIds) linHit |= invalidateStackDerived(sid);
    if (linHit) linFitStale();
    return true;
}

// THE DOOR, and it is the three calls above in their order. Synchronous, and
// that is what keeps every existing caller - the frame menu's single reload,
// reloadStackFromDisk's own loop, all of core/selftest/reload.inc - compiling
// and passing unchanged.
static bool reloadSource(const std::shared_ptr<FrameSource>& src, std::string& errOut,
                         std::string* noteOut = nullptr,
                         const ReloadPeer& peer = ReloadPeer()) {
    if (!src) { errOut = "no source"; return false; }
    const ReloadSpec spec = reloadSpecOf(*src);
    ReloadPre pre;
    reloadDecode(spec, pre, nullptr, peer);
    return reloadLand(src, spec, pre, errOut, noteOut);
}

// ---- §6: the membership a Reload rebuilds -----------------------------------
// One directory's file NAMES. The same listing watchScanDir takes, without the
// stat: what a rebuild wants from a folder is which names are IN it - whether
// their bytes moved is reloadSource's question, and it answers that by re-reading
// them.
static std::vector<std::string> stackDirNames(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        names.push_back(e.path().filename().u8string());
    }
    return names;
}

// §16: the PEER's answer to the same question, and to one more.
//
// `listed` is every name the peer's LIST reports in that folder - the members of
// every GROUP row and the name of every PLAIN row, because a folder that has
// dropped to a single file is no longer a group and its last survivor arrives as
// a plain row. Building `listed` out of group rows alone would report that
// survivor as VANISHED, which is the one file it demonstrably is not.
//
// `group` is the members of the group row that names `head` - the peer's own
// grouper, which IS the rule a remote folder stack was minted from
// (remoteSequenceSiblings opens one from exactly this row, and the peer shares
// rp::patternWithExtent with the local scan, so it is the same rule
// siblingNamesAmong applies here).
//
// FALSE means NO ANSWER (§13.4 one layer down): an unreachable peer, a dropped
// ssh link and a refused path all arrive this way, and folding them into an
// empty listing would have the rebuild conclude that every file of the stack is
// gone. The caller's only correct response is to do nothing at all.
static bool remoteFolderNames(const std::string& host, int port, const std::string& dir,
                              const std::string& head,
                              std::vector<std::string>& listed,
                              std::vector<std::string>& group,
                              const ReloadPeer& peer) {
    std::string err;
    remote::Session* ses = reloadPeerFor(peer, host, port, err);
    if (!ses) return false;
    std::vector<remote::Entry> rows;
    if (!ses->list(dir, rows, err)) return false;
    for (const remote::Entry& e : rows) {
        if (e.dir) continue;
        if (!e.group) { listed.push_back(e.name); continue; }
        bool mine = false;
        for (const std::string& m : e.members) {
            listed.push_back(m);
            if (m == head) mine = true;
        }
        if (mine) group = e.members;
    }
    return true;
}

// §12.2 over the link: IS this file list the peer's WHOLE sibling group?
//
// The local half asks this at the mint by RUNNING findSequenceSiblings and
// comparing (sequence.inc), and it does so for a reason that does not weaken
// over a link - it gets stronger. At Reload time "the folder gained a file" and
// "this stack always excluded two" are indistinguishable from a listing alone,
// and a group row cannot even be narrowed to the stack's own members (§13.5). So
// the answer is recorded at the moment the stack is minted, from the peer's own
// grouper, or it is not recorded at all and the stack can only ever LOSE members.
//
// WHICH rule is run is the same question planStackMembership asks later, and it
// has to get the same answer or the record describes a rule nobody re-applies:
//
//   AN EMPTY HOST IS THIS DISK. The peer runs here and serves this filesystem,
//   so makeRemoteUrl spelled those frames local:// and §13.7 keeps them on the
//   LOCAL half - the rebuild will re-apply siblingNamesAmong over a
//   directory_iterator. Recording it with the PEER's grouper instead would ask a
//   link for a grouping this machine is about to compute anyway, and would put
//   a round trip on EVERY Browse open of a local folder - the Browse selftests
//   alone open dozens. So the local rule is run locally.
//
//   A NAMED HOST is the peer's, and the rebuild will ask the peer's LIST, so
//   that is what is run here.
//
// Cost, stated for both because it lands on every open and not only on watched
// stacks: a local:// open pays ONE directory_iterator - the same listing §12.2
// already accepted for a File > Open folder stack - and a peer open pays ONE
// LIST, beside the META and TILE that open is already making. Returns whether a
// rule was recorded, so the caller can say so.
static bool recordRemoteRule(App::SeqInfo& si, const std::vector<std::string>& files,
                             int headIdx, const ReloadPeer& peer = ReloadPeer()) {
    si.ruleDir.clear();
    si.ruleHead.clear();
    if (files.empty() || headIdx < 0 || headIdx >= (int)files.size()) return false;
    // ONE directory, or there is no folder whose listing could be the rule -
    // watchTargetsNow's own refusal, for the same reason (a stack spanning two
    // folders has no single listing that speaks about it).
    const std::string dir = remoteDirOf(files[0]);
    if (dir.empty()) return false;
    for (const std::string& f : files)
        if (remoteDirOf(f) != dir) return false;
    const std::string head = baseName(files[headIdx]);
    std::vector<std::string> group;
    if (si.remoteHost.empty()) {
        std::string pat;
        group = siblingNamesAmong(head, stackDirNames(dir), pat);
    } else {
        std::vector<std::string> listed;
        if (!remoteFolderNames(si.remoteHost, si.remotePort, dir, head, listed, group, peer))
            return false;                    // no answer records no rule either
    }
    std::vector<std::string> a, b = group;
    for (const std::string& f : files) a.push_back(baseName(f));
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (b.empty() || a != b) return false;   // a subset, a hand-picked open
    si.ruleDir = dir;
    si.ruleHead = head;
    return true;
}

// What a Reload has to do to THIS stack's member list, decided before a byte is
// read. The three lists watch::planRebuild wants, assembled from the stack:
//
//   have    the members' basenames, in seqIndex order
//   listed  the ones whose file is still there, plus every name in the stack's
//           own directory when it HAS a rule (that is where an arrival can come
//           from at all)
//   want    the stack's own rule re-applied - and ONLY for a stack that
//           recorded one (SeqInfo::ruleHead, set at mint time when the stack was
//           its folder's whole sibling group). A derived subset records none,
//           gets an empty `want`, and therefore can lose a member whose file is
//           gone but can never be handed back the frames its rule excluded.
//
// Two stacks answer "no rebuild at all", and both are refusals of the same
// kind - the name rule has nothing to say about them:
//   - a stack whose members do not have one file each (a frame-axis .npy is F
//     memberships of ONE path): "which names are members" is not a question
//     about that stack's names;
//   - a stack with a member that has no file at all (a montage, a plugin
//     output): it never had one to lose.
//
// §16 added a THIRD list to build them from. Which folder answers "what is in
// it now" is decided by where the frames are, not by which panel opened them:
// a frame with a file on this disk (local://, §13.7) is answered by the disk,
// and a frame that is the PEER's is answered by the peer's LIST - whose group
// row carries the full member name list, which is exactly and only what these
// three lists are made of. A stack of BOTH origins has no one folder and is not
// rebuilt, which is the same refusal a two-directory stack already takes.
static watch::Rebuild planStackMembershipRemote(int seqId, const ReloadPeer& peer);
static watch::Rebuild planStackMembership(int seqId,
                                          const ReloadPeer& peer = ReloadPeer()) {
    watch::Rebuild none;
    std::vector<std::string> have;
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si) return none;
    const std::vector<int> fr = framesOfSeq(seqId);
    if (fr.empty()) return none;
    {   // WHOSE listing, asked of every frame before any of them is read
        bool anyPeer = false, allPeer = true;
        for (int idx : fr) {
            const FrameSource& s = *app.images[idx]->src;
            const bool p = isPeerUrl(s.remoteUrl);
            anyPeer |= p;
            allPeer &= p;
        }
        if (anyPeer && !allPeer) return none;   // two origins, no one folder
        if (anyPeer) return planStackMembershipRemote(seqId, peer);
    }
    std::vector<std::string> paths;
    for (int idx : fr) {
        const FrameSource& s = *app.images[idx]->src;
        const std::string onDisk = localDiskPathOfUrl(s.remoteUrl);
        const std::string p = isPeerUrl(s.remoteUrl) || s.remoteUrl.empty()
                                  ? s.path : onDisk;
        if (p.empty()) return none;                              // composed here
        paths.push_back(p);
        have.push_back(baseName(p));
    }
    {   // one file per frame, or the names are not the membership
        std::vector<std::string> u = have;
        std::sort(u.begin(), u.end());
        if (std::unique(u.begin(), u.end()) != u.end()) return none;
    }
    std::vector<std::string> listed;
    for (size_t k = 0; k < paths.size(); k++) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(pathFromUtf8(paths[k]), ec))
            listed.push_back(have[k]);
    }
    std::vector<std::string> want;
    if (!si->ruleHead.empty() && !si->ruleDir.empty()) {
        const std::vector<std::string> names = stackDirNames(si->ruleDir);
        std::string pat;
        want = siblingNamesAmong(si->ruleHead, names, pat);
        for (const std::string& n : names)
            if (std::find(listed.begin(), listed.end(), n) == listed.end() &&
                std::find(want.begin(), want.end(), n) != want.end())
                listed.push_back(n);
    }
    return watch::planRebuild(have, listed, want, rp::naturalLess);
}

// ...and the same three lists for a stack whose frames are the PEER's (§16).
//
// What the peer can answer is exactly what planRebuild needs, and §13 is what
// makes that a finding rather than a hope: a group row's (mtime, size) is the
// SET's and can never say which member moved, but its member NAME LIST is
// per-member truth, named as precisely as a local scan names it. `listed` and
// `want` are name lists. So no new protocol is needed here either - the same
// LIST the watcher already sends every fifteen seconds answers the rebuild.
//
// Three refusals, and each of them fails towards changing NOTHING:
//
//   NO ANSWER. §13.4 one layer down, and harder: detection that folds an
//   unreachable peer into an empty listing says a false sentence, a REBUILD
//   that does it takes every member out of the stack. §12.5's "never empties a
//   stack" would catch the extreme case afterwards, but a guard is not a
//   reading - the plan has to be empty because nothing was read.
//
//   RESIDENCY IS NOT MEMBERSHIP. A remote stack is allowed to be short of its
//   own file list (the memory budget fetches four of three hundred, and the
//   Files row says "n of N"). `have` is what is HERE; rebuilding from it would
//   re-issue expectedFrames and remoteFiles as the four and delete two hundred
//   and ninety-six of the peer's files from the stack without a word.
//
//   NO RULE, NO ARRIVALS. §12.2, recorded by recordRemoteRule at the mint.
static watch::Rebuild planStackMembershipRemote(int seqId, const ReloadPeer& peer) {
    watch::Rebuild none;
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si) return none;
    std::vector<std::string> have;
    std::string dir, host = si->remoteHost;
    const int port = si->remotePort;
    for (int idx : framesOfSeq(seqId)) {
        const FrameSource& s = *app.images[idx]->src;
        std::string h, rpath;
        int p = 0;
        if (!remote::parseUrl(s.remoteUrl, h, rpath, &p)) return none;
        const std::string d = remoteDirOf(rpath);
        if (d.empty()) return none;
        if (dir.empty()) dir = d;
        else if (d != dir) return none;      // one LIST speaks about one folder
        have.push_back(baseName(rpath));
    }
    {   // one file per frame, or the names are not the membership (a remote
        // frame-axis stack is F memberships of ONE url)
        std::vector<std::string> u = have;
        std::sort(u.begin(), u.end());
        if (std::unique(u.begin(), u.end()) != u.end()) return none;
    }
    {   // ...and every member of this stack's own list has to BE here
        std::vector<std::string> mine;
        for (const std::string& f : si->remoteFiles) mine.push_back(baseName(f));
        std::sort(mine.begin(), mine.end());
        std::vector<std::string> h2 = have;
        std::sort(h2.begin(), h2.end());
        if (mine != h2) return none;
    }
    std::vector<std::string> listed, group;
    if (!remoteFolderNames(host, port, dir, si->ruleHead, listed, group, peer))
        return none;                         // no answer is not an empty folder
    std::vector<std::string> want;
    if (!si->ruleHead.empty() && si->ruleDir == dir) want = group;
    return watch::planRebuild(have, listed, want, rp::naturalLess);
}

// A file JOINING an open stack, decoded the way that stack's own frames were
// (§6 step 2). WHICH DOOR is decided by the reference frame's provenance and
// never by the new file's name - one stack is one kind of thing (§6.2) - and the
// order of the tests is startSequenceLoad's worker's own, because it is the same
// decision made in the same order. Returns null and a reason; the caller counts
// it exactly as the loader counts a sibling that would not read.
// `folder` is the peer's listing of the directory the new member lives in, when
// the caller has one - the headerless size check reads it instead of asking for
// its own. ONE LIST PER DRAIN, not one per member: this runs on the UI thread,
// and a round trip per arriving frame is the freeze this file spent two PRs
// removing. Null (the default) means "fetch it", which is what the local doors
// and the tests want.
// The folder a PEER path lives in. One spelling, because the byte-count check
// put two hand-written copies of it in and both got the same case wrong: in
// "/cube.raw" the separator is at index 0, so `substr(0, cut)` is the EMPTY
// string and the MSG_LIST that follows asks the peer to list "" - a listing
// that comes back with nothing in it, so a root-level headerless file fails the
// same-size check with "the peer would not list" and never opens at all. The
// peer's root is "/", which is what remoteSequenceSiblings had already worked
// out for itself; this is that answer, available to everyone who needs it.
static std::string peerParentDir(const std::string& rpath) {
    const size_t cut = rpath.find_last_of("/\\");
    if (cut == std::string::npos) return ".";     // a bare name: the peer's cwd
    if (cut == 0) return "/";                     // the peer's ROOT, never ""
    return rpath.substr(0, cut);
}

static std::unique_ptr<ImageDoc> decodeJoiningMember(const ImageDoc& ref,
                                                     const std::string& path,
                                                     std::string& errOut,
                                                     const ReloadPeer& peer = ReloadPeer(),
                                                     const std::vector<remote::Entry>* folder
                                                         = nullptr) {
    const FrameSource& R = *ref.src;
    const bool isRaw = R.rawDtype >= 0;
    // ...and the PEER's door is one of them (§16). It is chosen by the same
    // question every other door here is chosen by - what the REFERENCE frame is
    // - so a stack whose frames came over a link takes its new member over that
    // link too, and `path` is read as the peer's path rather than this disk's.
    const bool refPeer = isPeerUrl(R.remoteUrl);
    std::unique_ptr<ImageDoc> doc;
    bool adopted = false;
    if (refPeer) {
        // The joining member's LOCATOR, built by replacing the file name in the
        // reference frame's own url. Not makeRemoteUrl(host, path, port): that
        // re-spells the locator from parsed pieces, and a url that came in as
        // one form and went out as another would key a different registry slot
        // for the same file. One name changes; everything that says WHERE stays
        // byte for byte what the stack is already using.
        const size_t cut = R.remoteUrl.find_last_of("/\\");
        if (cut == std::string::npos) { errOut = "no folder in " + R.remoteUrl; return nullptr; }
        const std::string url = R.remoteUrl.substr(0, cut + 1) + baseName(path);
        std::string host, rpath;
        int port = 0;
        if (!remote::parseUrl(url, host, rpath, &port)) {
            errOut = "not a remote url: " + url;
            return nullptr;
        }
        std::string perr;
        remote::Session* ses = reloadPeerFor(peer, host, port, perr);
        if (!ses) { errOut = perr.empty() ? "the peer did not answer" : perr; return nullptr; }
        // ...and the stack's own recipe, when its frames are headerless. Read
        // off the REFERENCE frame, never off the new file's name - one stack is
        // one kind of thing (6.2), and a member that asked natively would be
        // refused rather than joined.
        rp::RawWire jrw{};
        const bool haveJrw = rawWireOfSource(R, ref.cfaPattern, jrw);
        const rp::RawWire* jrwp = haveJrw ? &jrw : nullptr;
        // ...and BEFORE the bytes, the question a headerless stack has to ask
        // of anything joining it: is this file the same number of bytes as the
        // ones already in the stack? It is not the recipe's `need` - a file may
        // carry padding past the frame and still be right - it is agreement
        // with the siblings, because the geometry came from a person and one
        // recipe is being applied to all of them. A member of another length
        // decodes into a PLAUSIBLE picture of the wrong bytes, which is the
        // one outcome this whole feature is built not to produce.
        //
        // Asked with the same function the stack door asks it with, over the
        // folder this member and the reference both live in - one MSG_LIST,
        // whatever N is. This check did not exist: a Watch arrival went
        // straight to META/TILE with the reference frame's recipe.
        if (haveJrw) {
            const std::string mdir = peerParentDir(rpath);
            uint64_t sameSz = 0;
            std::string sameWhy;
            const std::vector<std::string> pair = { baseName(rpath), baseName(R.remoteUrl) };
            const bool same = folder
                ? remoteRawSameSizeIn(*folder, ses->peerVersion(), mdir, pair, sameSz, sameWhy)
                : remoteRawSameSize(*ses, ses->peerVersion(), mdir, pair, sameSz, sameWhy);
            if (!same) { errOut = sameWhy; return nullptr; }
        }
        remote::Meta m;
        if (!ses->meta(rpath, m, errOut, R.npyRead, jrwp)) return nullptr;
        // The SAME decimation the stack's own frames carry, so the new member is
        // the same kind of thing as the ones beside it - and so the shape check
        // below compares two readings of one geometry rather than a full frame
        // against a preview.
        const int step = std::max(1, R.remoteStep);
        std::vector<float> px;
        int tw = 0, th = 0, tch = 0;
        std::string dt;
        rp::F32Loss tloss;
        if (!ses->tile(rpath, 0, 0, 0, m.w, m.h, step, px, tw, th, tch, dt, errOut,
                       R.npyRead, &tloss, jrwp))
            return nullptr;
        if (tw < 1 || th < 1 || tch < 1 || tch > 4) {
            errOut = "the peer returned a degenerate frame";
            return nullptr;
        }
        doc = std::make_unique<ImageDoc>();
        doc->name = baseName(rpath);
        doc->note = ref.note;              // one note per stack, as every other
                                           // frame of it carries (openRemote)
        FrameSource& P = *doc->src;
        P.path = url;
        P.remoteUrl = url;
        P.remoteFrame = 0;                 // a folder stack: one file, one frame
        P.remoteStep = step;
        P.npyShape = m.shape;
        P.npyRead = R.npyRead;             // §3.3's three facts, inherited from
        P.remoteProto = ses->peerVersion();// the head exactly as rfInheritRead
        if (haveJrw) {                     // ...and the recipe, for identity and M1
            P.rawDtype = R.rawDtype; P.rawInterp = R.rawInterp;
            P.rawOffset = R.rawOffset; P.rawLE = R.rawLE;
            P.srcW = (int)jrw.w; P.srcH = (int)jrw.h;
            doc->cfa = rp::rawInterpCfa(jrw.interp);
            doc->cfaPattern = ref.cfaPattern & 3;   // one stack, one phase
        }
        P.dtype = dt;                      // gives them to a sibling at the open
        P.w = tw; P.h = th; P.ch = tch;
        P.data = std::move(px);
        P.f32loss = tloss;
        doc->syncMirrors();
    }
    if (!doc)
    {   // §6.2: the registry BEFORE the decode. This very file may already be
        // resident (the same folder opened twice, a derived stack beside its
        // source), and decoding it again would put two copies of identical
        // pixels in RAM under two srcIds.
        FrameSource probe;
        probe.path = path;
        if (isRaw) {
            probe.rawDtype = R.rawDtype; probe.rawInterp = R.rawInterp;
            probe.rawOffset = R.rawOffset; probe.rawLE = R.rawLE;
            probe.srcW = R.srcW > 0 ? R.srcW : R.w;
            probe.srcH = R.srcH > 0 ? R.srcH : R.h;
        }
        statSourceFile(probe);
        if (probe.mtime || probe.fsize) {
            const std::string key = srcIdentityKey(probe);     // IO: before the lock
            std::lock_guard<std::mutex> lk(g_srcRegMtx);
            if (std::shared_ptr<FrameSource> hit = srcRegistryFindLocked(key)) {
                doc = std::make_unique<ImageDoc>();
                doc->src = std::move(hit);
                doc->name = baseName(path);
                doc->note = ref.note;
                doc->syncMirrors();
                adopted = true;
            }
        }
    }
    std::string err;
    if (!doc) {
        if (isRaw) {
            RawDialog d;
            d.path = path;
            d.dtype = R.rawDtype; d.interp = R.rawInterp;
            d.offset = R.rawOffset; d.littleEndian = R.rawLE;
            d.w = R.srcW > 0 ? R.srcW : R.w;
            d.h = R.srcH > 0 ? R.srcH : R.h;
            d.cfaPattern = ref.cfaPattern & 3;
            d.replaceIdx = -1;
            doc = decodeRawFrame(d, err);
        } else if (imagefile::forPath(R.path)) {
            doc = decodeImageFileFrame(path, err);
        } else {
            doc = decodeNpy(path, err);
        }
    }
    // ...and the shape this stack IS. Same refusal, in the same words, as the
    // loader's: a stack whose frames are not the same picture measured again is
    // not a stack, and a member that joined on a Reload must clear the same bar
    // as one that joined at the open.
    if (doc) {
        std::string bad = stackShapeRefusal(doc->w, doc->h, doc->ch, ref.w, ref.h, ref.ch,
                                            baseName(R.path));
        if (!bad.empty()) { err = bad; doc.reset(); }
    }
    if (!doc) { errOut = err.empty() ? "decode failed" : err; return nullptr; }
    if (!adopted) {
        computeMinMax(*doc);
        const std::string addKey = srcIdentityKey(*doc->src);   // IO: before the lock
        std::lock_guard<std::mutex> lk(g_srcRegMtx);
        if (std::shared_ptr<FrameSource> live = srcRegistryAddLocked(doc->src, addKey)) {
            doc->src = std::move(live);
            doc->syncMirrors();
        }
    }
    return doc;
}

// Files right-click on a stack header: every member's source, re-read from
// disk. A shared source is reloaded ONCE (the walk already updated every
// membership it has). The summary counts BOTH directions and is never silent:
// failures (with the first error), members whose dimensions changed (the
// single-frame path toasts this - the batch path must not lose the fact), and
// memberships OUTSIDE this stack that the walk refreshed through shared
// sources - numbers moving in a stack the user never touched need a stated
// cause.
//
// ...and it too is in three parts now, for the reason reloadSource is in two:
// the middle part is N file reads and N decodes, and neither the plan in front
// of it nor the arithmetic behind it costs anything at all. The asynchronous
// door below (startStackReload) is these same three parts with a worker in the
// middle, so there is one plan, one summary and one set of §12/§13 decisions.

// ---- §6 step 1-2a: the member list, BEFORE a byte is read -------------------
// The departures happen first, and that is not an implementation detail: a
// member whose file is GONE would otherwise reach reloadSource, be refused
// ("cannot read file"), and be counted as a REFUSAL - which would mark the
// stack as holding frames of two moments (#56) when what actually happened is
// that it now holds four frames of one moment. A frame that is gone is not a
// frame that failed to read, and the difference is the whole of what the mark
// means.
//
// It stays on the UI THread in both doors, and that is a decision about cost:
// locally it is one directory_iterator, and over a link it is ONE listing - the
// per-member META+TILE that follows is where a remote reload spends its
// seconds, and that is what moved. closeImages is not movable at all.
static void reloadStackPlan(int seqId, const ReloadPeer& peer,
                            watch::Rebuild& planOut, int& hadBeforeOut) {
    hadBeforeOut = (int)framesOfSeq(seqId).size();
    watch::Rebuild plan = planStackMembership(seqId, peer);
    if ((int)plan.dropped.size() >= hadBeforeOut) plan = watch::Rebuild{};
                                          // ...and a rebuild NEVER empties a
                                          // stack. "Every file is gone" is not
                                          // something a click on Reload may
                                          // silently do to a measurement: the
                                          // frames stay, every member refuses
                                          // below, and the #56 mark says so.
    if (!plan.dropped.empty()) {
        std::vector<int> gone;
        for (int idx : framesOfSeq(seqId))
            if (std::find(plan.dropped.begin(), plan.dropped.end(),
                          baseName(app.images[idx]->src->path)) != plan.dropped.end())
                gone.push_back(idx);
        // closeImages, exactly as §6 says: the membership goes, the SOURCE only
        // dies if nothing else references it, and the pins/selection/series
        // pruning this stack's frames need is the pruning any close does.
        closeImages(gone);
    }
    planOut = std::move(plan);
}

// The walk just refreshed every membership of this source OUTSIDE this stack
// too (other stacks, loose frames): numbers moving in a stack the user never
// touched need a stated cause, so the summary earns a clause and this is what
// counts it. Called once per LANDED item, by both doors.
static void reloadSharedTally(const std::shared_ptr<FrameSource>& s, int seqId,
                              int& sharedN, std::vector<std::string>& sharedNames) {
    for (const auto& d2 : app.images)
        if (d2->src == s && d2->seqId != seqId) {
            sharedN++;
            std::string where = d2->name;
            if (d2->seqId != 0)
                if (const App::SeqInfo* si2 = seqInfo(d2->seqId)) where = si2->name;
            if (std::find(sharedNames.begin(), sharedNames.end(), where) ==
                sharedNames.end())
                sharedNames.push_back(where);
        }
}

// What one attempt's numbers ADD UP TO: the arrivals, the renumbering, #56's
// latch and the sentence. `stopped` is the asynchronous door's own term - a
// Stop that left n of N frames re-read - and it is said rather than counted as
// a refusal, because nothing refused.
struct ReloadTally {
    int ok = 0, failed = 0, dims = 0, sharedN = 0;
    std::string firstErr, firstDims;
    std::vector<std::string> sharedNames;
    int stoppedAfter = -1;            // -1 = ran to the end
    int stoppedOf = 0;
};
static std::string reloadStackTail(int seqId, const watch::Rebuild& plan, int hadBefore,
                                   const ReloadTally& t, int* failedOut,
                                   const ReloadPeer& peer) {
    // ---- §6 step 2b-3: the arrivals, then the numbers -----------------------
    // AFTER the re-read, so a joining frame is measured against the reference
    // frame's CURRENT shape rather than the shape it had a moment ago. That
    // ordering is also why this half stays on the UI thread in the
    // asynchronous door: the shape it measures against does not exist until
    // the last item has landed.
    int joinFailed = 0;
    std::string firstJoinErr;
    if (!plan.added.empty()) {
        const std::vector<int> fr = framesOfSeq(seqId);
        const ImageDoc* ref = fr.empty() ? nullptr : app.images[fr.front()].get();
        std::string dir;
        if (const App::SeqInfo* si = seqInfo(seqId)) dir = si->ruleDir;
        // ONE listing for the whole drain. A headerless member may only join a
        // stack when it is the same number of bytes as the ones already in it
        // (remoteRawSameSizeIn), and that question is answered out of the
        // peer's listing of this folder - so the folder is listed ONCE here and
        // the rows are handed to every arrival, rather than each of them asking
        // for its own on the UI thread. Fetched only for a headerless remote
        // stack, because nothing else asks the question.
        std::vector<remote::Entry> folderRows;
        const std::vector<remote::Entry>* folderPtr = nullptr;
        if (ref && ref->src && ref->src->rawDtype >= 0 && isPeerUrl(ref->src->remoteUrl) &&
            !dir.empty()) {
            std::string host, rp2, ferr;
            int fport = 0;
            if (remote::parseUrl(ref->src->remoteUrl, host, rp2, &fport)) {
                if (remote::Session* fs = reloadPeerFor(peer, host, fport, ferr))
                    if (fs->list(dir, folderRows, ferr)) folderPtr = &folderRows;
            }
            // A listing that did not come back is not an excuse to skip the
            // check: folderPtr stays null and each member asks for its own,
            // which is slow and correct rather than fast and blind.
        }
        for (const std::string& n : plan.added) {
            if (!ref || dir.empty()) break;
            std::string err;
            std::unique_ptr<ImageDoc> nd =
                decodeJoiningMember(*ref, dir + "/" + n, err, peer, folderPtr);
            if (!nd) {
                // NOT counted with the re-read refusals, and this is the same
                // distinction #56 rests on: a file the rule NAMES but cannot
                // decode (a sibling of another shape, a half-written frame) was
                // never a member, so it leaves no seam between two moments in
                // this stack and must not raise the (reload failed) mark. What
                // it does raise is the ratio the loader raises for exactly this
                // case: expectedFrames counts the names, the membership counts
                // what decoded, and the Files row says "4/5f" until it reads.
                // That statement outlives the toast, which is the point.
                joinFailed++;
                if (firstJoinErr.empty()) firstJoinErr = n + ": " + err;
                continue;
            }
            // the per-membership half (§2.1), inherited from the reference frame
            // exactly as pumpSequence inherits it for a frame that lands from
            // the loader: the same range, the same mosaic, the same batch, or
            // the new frame is not comparable with the ones beside it
            nd->name = baseName(n);
            nd->seqId = seqId;
            nd->batchId = ref->batchId;
            nd->black = ref->black; nd->white = ref->white;
            nd->cfa = ref->cfa; nd->cfaPattern = ref->cfaPattern;
            nd->cfaColorize = ref->cfaColorize;
            nd->displayLut = ref->displayLut;
            nd->texDirty = true;
            nd->uid = app.nextUid++;
            app.images.push_back(std::move(nd));
            app.imagesRev++;
        }
    }
    if (plan.any()) {
        // §6 step 3: seqIndex re-issued over the rebuilt list, and it is the
        // OPEN's own numbering - position in the natural-order member list, so a
        // frame that arrived in the middle is numbered in the middle and a gap
        // is left exactly where the open would leave one (a named member that
        // would not decode).
        for (int idx : framesOfSeq(seqId)) {
            const std::string n = baseName(app.images[idx]->src->path);
            auto it = std::find(plan.order.begin(), plan.order.end(), n);
            if (it != plan.order.end())
                app.images[idx]->seqIndex = (int)(it - plan.order.begin());
        }
        if (App::SeqInfo* si = seqInfo(seqId)) {
            si->expectedFrames = (int)plan.order.size();
            // ...and a REMOTE stack's own list of the peer's files is part of
            // its membership, not a note beside it: watchTargetsNow builds the
            // watched member list out of it (so the next poll would decide
            // "whole group" against a folder this stack no longer matches), the
            // server aggregates name those paths, and a session line writes
            // them. Re-issued from the rebuilt order, in the peer's spelling.
            // planStackMembershipRemote has already refused any stack whose
            // residency is not this list, so `order` names all of it.
            if (!si->remoteFiles.empty()) {
                const std::string rd = remoteDirOf(si->remoteFiles[0]);
                std::vector<std::string> rf;
                for (const std::string& n : plan.order)
                    rf.push_back(rd.empty() ? n : rd + "/" + n);
                si->remoteFiles = std::move(rf);
            }
        }
        // The member LIST moved, so everything keyed on the stack AS A SET has
        // to move with it - the sigma_t of four frames is not the sigma_t of
        // five even when not one member's pixels changed. Same call the pixel
        // walk makes, for the same reason (§3.2 / §10.1).
        if (invalidateStackDerived(seqId)) linFitStale();
        // ...and the watcher's target list is now a different set of files.
        app.watchTargetsDirty = true;
    }
    if (failedOut) *failedOut = t.failed;
    // ---- #56: what this attempt LEAVES ON the stack ------------------------
    // The summary below is a toast, and a toast scrolls away. A reload that
    // refused a member left frames of two moments under one name, and every
    // stack number computed afterwards averages across that seam - so the fact
    // is latched on the SeqInfo, here, at the event that caused it, exactly as
    // §10.1 latches avgStale on the reduced frames a few lines up.
    //
    // And the clean run is the one thing that CLEARS it: every member re-read
    // in one attempt is a stack of one moment again. Not a redraw, not a
    // single-frame reload - the operation that failed is the operation that
    // un-says it. The pointer is taken after the last landing: nothing in the
    // walk adds or removes a stack, but seqInfo()'s rule is that the pointer is
    // not held ACROSS anything, so it is not.
    //
    // A STOP is on the same side of this as a refusal, and for the same reason
    // rather than by analogy: the stack really does hold frames of two moments,
    // so #56's mark has to be up. What differs is the WORD - `firstErr` says
    // "stopped after n of N" - because nothing about the files was wrong.
    // A STOP's frames are NOT refusals, and they are not "of this moment"
    // either: they are the frames nobody re-read. So the LATCH counts them -
    // the stack really does hold two moments, which is the one thing #56's mark
    // is about - while `failed` above stays the count of members that actually
    // refused, because that is what the summary's first clause claims.
    int latchFailed = t.failed, latchOk = t.ok;
    std::string latchErr = t.firstErr;
    if (t.stoppedAfter >= 0 && t.stoppedAfter < t.stoppedOf) {
        latchFailed += t.stoppedOf - t.stoppedAfter;
        if (latchErr.empty())
            latchErr = "the reload was stopped, so " +
                       std::to_string(t.stoppedOf - t.stoppedAfter) +
                       " frame(s) were never re-read";
    }
    if (App::SeqInfo* si = seqInfo(seqId)) {
        si->reloadFailed = latchFailed;
        si->reloadOk = latchFailed ? latchOk : 0;
        si->reloadFirstErr = latchFailed ? latchErr : std::string();
        if (latchFailed) {
            char ts[32] = "";
            time_t now = time(nullptr);
            if (struct tm* lt = localtime(&now)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", lt);
            si->reloadWhen = ts;
        } else {
            si->reloadWhen.clear();
        }
    }
    char buf[256];
    snprintf(buf, sizeof buf, "reloaded %d frame(s), %d failed", t.ok, t.failed);
    std::string msg = buf;
    if (t.stoppedAfter >= 0) {
        snprintf(buf, sizeof buf, " - stopped after %d of %d", t.stoppedAfter, t.stoppedOf);
        msg += buf;
    }
    // §6: the member list moved, so the summary says so, with the names and
    // with both counts. A stack that quietly measures a different set of frames
    // than it did a second ago is the failure this sentence exists to prevent -
    // and the count alone would not prevent it, because one file gone and one
    // arrived leaves the count exactly where it was.
    if (plan.any()) {
        const App::SeqInfo* si = seqInfo(seqId);
        msg += " - " + watch::rebuildText(plan, hadBefore,
                                          (int)framesOfSeq(seqId).size(),
                                          si ? si->expectedFrames : 0);
        if (joinFailed) {
            snprintf(buf, sizeof buf, " - %d file(s) the rule names did not read (%s)",
                     joinFailed, firstJoinErr.c_str());
            msg += buf;
        }
    }
    // ...and the toast points at the mark it just left, because the mark is
    // the part that outlives the toast: a user who reads only this line must
    // still learn that the stack now carries the fact, and where.
    if (latchFailed)
        msg += " - " + latchErr + " - this stack is marked " +
               SEQ_RELOAD_FAIL_TOKEN + " in Files until every frame re-reads";
    if (t.dims) {
        snprintf(buf, sizeof buf, " - %d changed dimensions (%s)", t.dims,
                 t.firstDims.c_str());
        msg += buf;
    }
    if (t.sharedN) {
        snprintf(buf, sizeof buf, " - also refreshed %d shared frame(s) in \"%s\"",
                 t.sharedN, t.sharedNames.empty() ? "" : t.sharedNames[0].c_str());
        msg += buf;
        if (t.sharedNames.size() > 1) {
            snprintf(buf, sizeof buf, " (+%d more)", (int)t.sharedNames.size() - 1);
            msg += buf;
        }
    }
    // A frame average/sum of this stack now shows pixels the disk no longer
    // holds (§10.1). The walk latched the mark; the summary must SAY it, or
    // the one picture that ends up in reports goes quietly wrong.
    int staleAvg = 0;
    for (const auto& d : app.images)
        if (!d->avgOfPath.empty() && d->avgSeqId == seqId && d->avgStale) staleAvg++;
    if (staleAvg) {
        snprintf(buf, sizeof buf, " - %d computed frame(s) of this stack now stale "
                                  "(right-click > Recompute from stack)", staleAvg);
        msg += buf;
    }
    return msg;
}

static std::string reloadStackFromDisk(int seqId, int* failedOut = nullptr,
                                       const ReloadPeer& peer = ReloadPeer()) {
    std::vector<const FrameSource*> done;
    ReloadTally t;
    int hadBefore = 0;
    watch::Rebuild plan;
    reloadStackPlan(seqId, peer, plan, hadBefore);
    for (int idx : framesOfSeq(seqId)) {
        const std::shared_ptr<FrameSource>& s = app.images[idx]->src;
        if (std::find(done.begin(), done.end(), s.get()) != done.end()) continue;
        done.push_back(s.get());
        std::string err, note;
        if (reloadSource(s, err, &note, peer)) {
            t.ok++;
            if (!note.empty()) {
                t.dims++;
                if (t.firstDims.empty())
                    t.firstDims = (s->path.empty() ? app.images[idx]->name
                                                   : baseName(s->path)) + ": " + note;
            }
            reloadSharedTally(s, seqId, t.sharedN, t.sharedNames);
        } else {
            t.failed++;
            if (t.firstErr.empty())
                t.firstErr = (s->path.empty() ? app.images[idx]->name
                                              : baseName(s->path)) + ": " + err;
        }
    }
    return reloadStackTail(seqId, plan, hadBefore, t, failedOut, peer);
}

// ============================================================================
//  THE RELOAD, OFF THE UI THREAD (the 2026-09-28 report)
//  ============================================================================
//
// 「reload を押すと応答なしになる」. Measured on this repo before the change:
// with the freeze probe and the automatic Reload driving the gesture (median of
// 3, longest unresponsive; the table and the method are in watch-design §18.2b):
//
//   480 MB .npy, ONE file, TEN frames    3003 ms -> 0 ms
//   480 MB x 4 files, folder stack       5013 ms -> 1508 ms
//   48 MB x 4 files, folder stack         375 ms -> 0 ms   (the control)
//
// The ten-frame row is this walk's own pathology: a frame-axis stack is one
// FrameSource per frame, so the old walk asked each of them to read the whole
// file for itself - TEN whole-file reads and twenty decodes to land ten frames.
// That is #232 stage 2's pathology exactly, fixed for the open and not for the
// reload. (What is left on row 2 is not a read at all - see §18.2b: it is the
// GL upload of a ~1 GB frame, and it is the same before and after.)
//
// NO NEW MACHINERY. App::ReloadJob is App::OpenJob's shape (one at a time, a
// queue in front of it, a cancel flag, a start time that IS the progress), the
// split between decode and landing is reloadDecodeGroup/reloadLand above, the
// plan and the summary are reloadStackPlan/reloadStackTail above, and the row it
// draws is the row the open, the reader, the stack loader and the video export
// already draw.
//
// FOUR THINGS ARE THE RELOAD's OWN, and each is a decision rather than a detail:
//
//   IT LANDS ONE ITEM PER FRAME. A reload that swapped twelve frames in one
//   landing would have moved the freeze from the read to the swap - each
//   landing walks app.images, unlatches every cache keyed on the source and
//   marks a texture dirty. So the progress row carries a true fraction
//   ("3 / 12") where an open's carries a phase, and the window answers between
//   every frame of the stack. The worker runs ahead of the landing, so on real
//   data the cost is the decode and this costs nothing; on a stack of tiny
//   files it is one frame of the loop per member, which is a slower reload of
//   something that was never the complaint.
//
//   STOP LEAVES WHAT LANDED. The opposite of OpenJob's "Stop leaves nothing",
//   and for a reason rather than by accident: a landed frame is a complete,
//   valid document of what the disk holds now, and un-landing it would mean
//   re-reading the file to get the OLD pixels back. What a stop leaves is a
//   stack of two moments, which is precisely the state #56's mark exists for,
//   so the mark goes up and the summary says "stopped after n of N".
//
//   THE PEER IS THE WORKER's. app.uiSession belongs to the UI thread and to
//   nobody else (state.h's ownership note), so a job with peer items dials a
//   Session of its own - rfWorker's shape - and a link that drops fails every
//   remaining peer item WITHOUT re-dialling. §16.6 argued that the stall was
//   the manual gesture's price and not the automatic one's; the answer taken in
//   the end is that it is nobody's.
//
//   THE MEMBERSHIP REBUILD STAYS HERE. Both ends of it: the plan, because
//   locally it is one directory listing and remotely it is ONE request (the
//   per-member transfer is what moved), and closeImages, which is not movable.
//   The ARRIVALS stay too, and that one is forced: §6 measures a joining frame
//   against the reference frame's CURRENT shape, and that shape does not exist
//   until the last item has landed. A Reload of a folder that gained a file
//   therefore still decodes that ONE file on this thread. Stated rather than
//   hidden; the F-member re-read it used to sit behind is what the report was
//   about.

// A landed or refused item, as the summary names it: the file's base name, or
// the document's own name when there is no file behind it (a computed frame).
static std::string reloadItemName(const App::ReloadJob::Item& it) {
    if (!it.spec.path.empty()) return baseName(it.spec.path);
    for (const auto& d : app.images)
        if (d->src == it.src) return d->name;
    return std::string("this frame");
}

// What the Files row says while a reload runs. A true fraction and the seconds,
// and no phase: every item goes through the same three steps, so naming them
// would say the same word twelve times over. (openJobPhase is the twin, and its
// note says why an OPEN has no fraction to give.)
static std::string reloadJobRowText(const App::ReloadJob& j) {
    char b[192];
    // §12.6: in STAGE 2 the fraction is the ARRIVALS'. Going on printing
    // "3 / 3" while the new frames are being read would put a finished
    // fraction on the row for the whole second half of the gesture, and the
    // Stop beside it would look like a Stop for nothing. The verb stays
    // "reloading" - one gesture, and the row is not the place to teach
    // anybody about stages - and the noun says what the count counts.
    if (j.stage == 2)
        snprintf(b, sizeof b, "reloading %s: %d / %d new frame(s)  %.1f s",
                 j.name.empty() ? "frame" : j.name.c_str(), j.arrivalsLanded,
                 (int)j.arrivals.size(), nowSec() - j.startedAt);
    else
        snprintf(b, sizeof b, "reloading %s: %d / %d  %.1f s",
                 j.name.empty() ? "frame" : j.name.c_str(), j.landedN,
                 (int)j.items.size(), nowSec() - j.startedAt);
    return b;
}

// Everything the worker does, and nothing else. Every call in here is pure or
// is already called from the sequence loader's thread for the same reason.
static void reloadJobWorker(App::ReloadJob* j) {
    // WHICH LIST, and the answer is the STAGE (§12.6). Stage 1 re-reads the
    // resident memberships; stage 2 decodes the frames that are JOINING, which
    // have no FrameSource yet and are therefore items with a null `src` and a
    // spec built from the plan.
    //
    // `stage` is a plain int read here and written by the UI thread, and that
    // is not a race: it is written only while NO worker is running - after
    // th.join() and before the next std::thread(), whose constructor is the
    // synchronisation point that publishes it to this thread. The same argument
    // covers `arrivals`, `arrivalEnd` and the reset of doneCount/done.
    const bool second = j->stage == 2;
    std::vector<App::ReloadJob::Item>& items = second ? j->arrivals : j->items;
    const std::vector<int>& ends = second ? j->arrivalEnd : j->groupEnd;
    // The two probes are kept apart for the reason written at
    // g_reloadArrivalThread: an assert about the arrivals must not be
    // satisfiable by the re-read's thread id.
    if (second) j->arrivalThread = std::this_thread::get_id();
    else        j->decodeThread  = std::this_thread::get_id();
    // ---- the peer, dialled ONCE, and never again once it is gone -----------
    // A function-local Session, rfWorker's shape and its reason: a 300 s round
    // trip must not hold the UI's frame, and two threads framing requests into
    // one ssh stdin desynchronise the stream permanently. `cancel` is its abort,
    // so a Stop reaches a blocked read.
    //
    // A resolver a CALLER handed in wins (§16): --rwatch-selftest drives a real
    // local:// peer over a pipe and reaches it with an empty host, which no url
    // can spell. The test thread is inside waitForReloadJob while this runs, so
    // that session still has exactly one thread touching it at a time.
    remote::Session ses;
    ses.setAbort(&j->cancel);
    ses.setIdleTimeout(30.0);
    bool dialled = false, dead = false;
    std::string deadErr;
    ReloadPeer own = [&](const std::string& host, int port,
                         std::string& errOut) -> remote::Session* {
        // ONE LATCH, whichever side answers. A link that is down is down for
        // every remaining item of this job, and the alternative is a stack of
        // forty peer frames spending forty connect timeouts discovering it one
        // at a time - with a Stop button that cannot be pressed any faster than
        // the frame loop, which is the very thing this worker exists for.
        if (dead) { errOut = deadErr; return nullptr; }
        auto die = [&](const std::string& why) -> remote::Session* {
            dead = true;
            deadErr = why.empty() ? std::string("the peer did not answer") : why;
            errOut = deadErr;
            return nullptr;
        };
        if (j->peer) {
            remote::Session* s = j->peer(host, port, errOut);
            return s ? s : die(errOut);
        }
        if (!dialled) {
            dialled = true;
            // the peer is the same binary, resolved exactly as ensureUiSession
            // resolves it - one spelling of "which program answers on the other
            // side", or a reload would reach a different peer than the open did
            // - but off the values FROZEN at the start of the job (review P1-1).
            // app.remoteExe and app.exePath are std::strings the UI thread
            // writes (settings.inc SK_peerExe, session.inc's `remoteexe` line),
            // so reading them here is a data race on a std::string and not
            // merely a stale read. rfEnqueue freezes for exactly this reason and
            // says so. With this, the worker touches NO app state at all, which
            // is the whole discipline.
            const std::string exe =
                (host.empty() && j->remoteExe.empty())
                    ? j->exePath
                    : (j->remoteExe.empty() ? std::string(REMOTE_HOME) + "/viewer-serve"
                                            : j->remoteExe);
            std::string err;
            if (!ses.startOn(host, port, exe, err)) return die(err);
        }
        // The link went down under an earlier item: same latch, different
        // sentence, because "it never answered" and "it stopped answering" are
        // two different things to read on a row.
        if (!ses.alive()) return die("the connection to the peer was lost");
        return &ses;
    };
    // The groups were laid out by startStackReload, contiguously and in item
    // order, so `doneCount` is monotone in the order the landing lands: item k
    // is decoded when doneCount > k, whichever group it was in.
    size_t from = 0;
    for (int end : ends) {
        if (j->cancel.load()) break;       // between GROUPS, never inside one:
                                           // half a file read is not a read
        std::vector<const ReloadSpec*> specs;
        std::vector<ReloadPre*> pres;
        for (size_t i = from; i < (size_t)end; i++) {
            specs.push_back(&items[i].spec);
            pres.push_back(&items[i].pre);
        }
        reloadDecodeGroup(specs, pres, &j->cancel, own);
        j->doneCount.store(end);
        from = (size_t)end;
    }
    j->done.store(true);
}

// Build the items of a job out of the stack's distinct resident sources, group
// them so one file is read once, and lay the groups out contiguously. Returns
// how many items there are; 0 means there is nothing to reload.
static size_t reloadJobItemsForStack(App::ReloadJob* j, int seqId) {
    std::vector<const FrameSource*> done;
    for (int idx : framesOfSeq(seqId)) {
        const std::shared_ptr<FrameSource>& s = app.images[idx]->src;
        if (std::find(done.begin(), done.end(), s.get()) != done.end()) continue;
        done.push_back(s.get());
        App::ReloadJob::Item it;
        it.src = s;
        it.spec = reloadSpecOf(*s);
        j->items.push_back(std::move(it));
    }
    // ---- the grouping, and it is the second half of the report --------------
    // Memberships served by ONE read go together, and two memberships that want
    // the SAME frame of one file do not (reloadPickIndex says why). The groups
    // are laid out contiguously in item order so that the worker's progress
    // counter is monotone for the landing.
    std::vector<App::ReloadJob::Item> ordered;
    std::vector<int> ends;
    std::vector<bool> taken(j->items.size(), false);
    for (size_t i = 0; i < j->items.size(); i++) {
        if (taken[i]) continue;
        const std::string key = reloadReadKey(j->items[i].spec);
        std::vector<int> picks;
        taken[i] = true;
        picks.push_back(reloadPickIndex(j->items[i].spec));
        ordered.push_back(std::move(j->items[i]));
        if (!key.empty())
            for (size_t k = i + 1; k < j->items.size(); k++) {
                if (taken[k] || reloadReadKey(j->items[k].spec) != key) continue;
                const int pick = reloadPickIndex(j->items[k].spec);
                if (std::find(picks.begin(), picks.end(), pick) != picks.end()) continue;
                taken[k] = true;
                picks.push_back(pick);
                ordered.push_back(std::move(j->items[k]));
            }
        ends.push_back((int)ordered.size());
    }
    j->items = std::move(ordered);
    j->groupEnd = std::move(ends);
    return j->items.size();
}

// ---- §12.6 STAGE 2: the frame-axis ARRIVALS, as items of the SAME job -------
//
// Built on the UI thread at the one moment stage 1 is over, out of the PLAN and
// nothing else. Not a byte is read here: `path`, `fileFrame` and `npyRead` are
// all a joining frame is, and the shape it will be measured against is already
// in the record (watchFrameAxisHead wrote it on the line above the call).
//
// The items carry a NULL `src`, and that is what makes this possible at all: a
// joining frame has no FrameSource yet, and reloadDecodeGroup does not want one
// - it takes ReloadSpecs by value precisely so the worker cannot touch a live
// source. So the arrivals reach the very same decode the re-read reaches, and
// their landing is a JOIN instead of a swap (watchFrameAxisJoin) rather than a
// second decode path.
//
// ONE GROUP, always, and it is provable rather than hoped - but the proof has
// to cite the guards that EXIST. reloadReadKey answers "" (read this one on its
// own) for three kinds of spec, and watchFrameAxisPlan has already excluded all
// three before this function can be reached:
//
//   a CONTAINER/READER member (`__pixels_*`) - refused BY NAME: the plan
//     requires `s.member.empty()` of the head and of every member.
//   a PEER frame - refused by the PATH: the plan's `path` is
//     watchLocalPathOf(s0), which is "" for a real remote url, and an empty
//     path returns applies=false. (A `local://` url answers with the on-disk
//     path, and that one really is local, which is §13.7's own line.)
//   a RAW RECIPE - and this is the one the first version of this comment got
//     WRONG. watchFrameAxisPlan never looks at `rawDtype`; it does not have to,
//     and saying it did made this paragraph cite a guard that is not there.
//     What actually excludes a raw source is the shape of the stack: a raw open
//     yields ONE image per file (decodeRawFrame), so a raw stack is a FOLDER of
//     one-frame files and its second member fails
//     `watchLocalPathOf(s) != path`. A raw stack of exactly one member survives
//     that and is then refused by watchNpyFrames, which peeks for an .npy
//     header and answers -1.
//
// On top of that the arrivals share npyRead (the plan carries one, taken from
// the head and checked against every member), and reloadPickIndex is their
// fileFrame, which is distinct by construction. So N joining frames are ONE
// stat, ONE read and ONE decode pass - where the UI thread used to pay one
// whole-file read PER frame.
//
// `w/h/ch` are deliberately left at 0. A joining frame has no RECORDED shape to
// re-apply: it is new. Those fields are what reloadLand's dims note and
// reloadDecodeGroup's recrop compare against, and the arrivals reach neither -
// they are measured against the plan's record, in the join, once.
static size_t reloadArrivalItems(App::ReloadJob* j) {
    const WatchAxisPlan& p = j->axis;
    j->arrivals.clear();
    j->arrivalEnd.clear();
    j->arrivalsLanded = 0;
    if (!p.moved() || p.want <= p.have || p.path.empty()) return 0;
    for (int f = p.have; f < p.want; f++) {
        App::ReloadJob::Item it;
        it.spec.path = p.path;
        it.spec.fileFrame = f;
        it.spec.npyRead = p.npyRead;
        j->arrivals.push_back(std::move(it));
    }
    j->arrivalEnd.push_back((int)j->arrivals.size());
    return j->arrivals.size();
}

static bool startReloadJob(App::ReloadJob* j);
// watch-design 12's frame-axis membership, defined in watch.inc (included after
// this file). Declared here because THE PLAN BELONGS TO THE JOB that does the
// reading (review P1-2), so it is taken - and its departures applied - inside
// startStackReload below rather than at the gesture.
static WatchAxisPlan watchFrameAxisPlan(int seqId);
static void watchFrameAxisDrop(int seqId, int keep);
// ...and §12.6's two halves of the ARRIVALS, watch.inc's too. THE SHAPE is
// recorded once, at the one moment the re-read is over (watchFrameAxisHead),
// and every joining frame goes through ONE join against that record
// (watchFrameAxisJoin) whichever thread decoded it. Declared here because the
// job's stage 2 is what decodes them and pumpReloadJob is what lands them.
static bool watchFrameAxisHead(int seqId, WatchAxisPlan& p);
static bool watchFrameAxisJoin(int seqId, WatchAxisPlan& p, int f,
                               std::unique_ptr<ImageDoc> nd, std::string err);
// ...and the SYNCHRONOUS door's whole gesture, also watch.inc's: the plan, the
// re-read and the sentence, in that order. One spelling, so the queue's
// fall-back below cannot drift from watchReloadNow's own body.
static void reloadStackSync(int seqId, bool automatic);
// ...and what a finished stack reload SAYS about the finding it just answered
// (watch-design §9's record and its one toast), watch.inc's too: ONE
// post-processing, so a click and the automaton cannot come to do two different
// things.
static void watchReloadFinish(int seqId, bool automatic, const std::string& msg,
                              int failed, uint64_t pinWas, WatchAxisPlan& ax);

// Start a stack's reload without waiting for it. Returns whether the
// asynchronous door took it - false means the caller must do the reload itself,
// here and now (reloadStackFromDisk).
static bool startStackReload(int seqId, bool automatic,
                             const ReloadPeer& peer = ReloadPeer()) {
    // THE SYNCHRONOUS DOOR, and it is wide for startOpenJob's reason exactly:
    // --reload-selftest, --watch-selftest and --rwatch-selftest all call
    // reloadStackFromDisk (or watchReloadNow) and assert on app.images on the
    // line after it. --async-open is the lid, so the path a person takes is not
    // the one path the suite never runs.
    if (g_scriptedRun && !g_forceAsyncOpen) return false;
    if (!seqInfo(seqId)) return false;
    // Already reloading. The SAME stack is folded in - a double-click on
    // Reload, or §9's drain firing while the click's job runs, is one reload -
    // and a different stack waits. Never folded into a job that has been
    // STOPPED (startOpenJob's review P2-4): that job will land nothing more, so
    // answering "already reloading" about it would silently do nothing.
    if (app.reloadJob) {
        // FOLDED, and folding is NOT discarding (review P1-2). This request has
        // not been read: the job in flight started before the request was made,
        // so it cannot be an answer to it. `again` runs the stack once more when
        // this job has landed, with a NEW plan and a NEW stat - which is the
        // only statement that is actually true. (Overwriting the RUNNING job's
        // plan is not available: its decode began on the old member list, so a
        // grow would claim frames nobody read.)
        if (app.reloadJob->seqId == seqId && !app.reloadJob->cancel.load()) {
            App::ReloadJob* run = app.reloadJob.get();
            run->againAutomatic = run->again ? (run->againAutomatic && automatic)
                                             : automatic;
            run->again = true;
            return true;
        }
        for (auto& w : app.reloadQueue)
            if (w.seqId == seqId) {          // a request already WAITING needs no
                w.automatic = w.automatic && automatic;   // second entry, but a
                return true;                 // CLICK deserves a click's sentence
            }
        app.reloadQueue.push_back({ seqId, 0, automatic });
        return true;
    }
    std::unique_ptr<App::ReloadJob> j(new App::ReloadJob());
    j->seqId = seqId;
    j->automatic = automatic;
    j->peer = peer;
    // ...and HOW the peer is invoked, frozen now (review P1-1, rfEnqueue's rule)
    j->remoteExe = app.remoteExe;
    j->exePath = app.exePath;
    if (const App::SeqInfo* si = seqInfo(seqId)) j->name = si->name;
    // §12.5's other half, sampled ACROSS the whole reload: a pin on a frame
    // that VANISHES is dropped by closeImages, and the summary is about files.
    j->pinWas = app.compareBUid;
    // ---- watch-design §12's frame-axis plan, taken HERE (review P1-2) --------
    // Not at the gesture. A gesture folded into a running job does no reading,
    // so a plan taken there applied its departures against a stack whose
    // re-read was somebody else's and then lost its arrivals with the fold -
    // board 222's own defect, back through the folding path
    // (--asyncopen-selftest R9e). Taken where the READ begins, that path is
    // structurally gone: no job, no plan, no departures.
    //
    // The departures go FIRST, in §6's order and for §6's reason: a frame the file
    // no longer holds must not reach reloadSource and be counted as a refusal.
    // The arrivals are watchReloadFinish's, when this job lands. `pinWas` above
    // is sampled before them because they are a closeImages.
    j->axis = watchFrameAxisPlan(seqId);
    if (j->axis.moved() && j->axis.want < j->axis.have)
        watchFrameAxisDrop(seqId, j->axis.want);
    reloadStackPlan(seqId, j->peer, j->plan, j->hadBefore);
    if (!reloadJobItemsForStack(j.get(), seqId)) {
        // Nothing resident to re-read - a stack every member of which just
        // departed, which reloadStackPlan refuses to let happen, or one that
        // was emptied under us. The gesture still owes its WHOLE answer, and
        // that includes §12's arrivals and §5's clearing, so it goes through
        // watchReloadFinish rather than toasting on its own.
        ReloadTally t;
        int failed = 0;
        const std::string msg = reloadStackTail(seqId, j->plan, j->hadBefore, t,
                                                &failed, j->peer);
        watchReloadFinish(seqId, automatic, msg, failed, j->pinWas, j->axis);
        return true;
    }
    // ---- §12.6: THE ARRIVALS ARE THIS JOB'S, from here on -------------------
    // Set only now, on the line after the job is known to have items and a
    // worker: it means "watchReloadFinish must not read the file for these",
    // and the one path that could have made that a lie is the early return
    // above - a stack with nothing resident to re-read, which has no worker to
    // hand them to. (It cannot actually co-occur with a grow:
    // watchFrameAxisPlan returns applies=false for a stack with no frames, and
    // a stack with frames always yields at least one item. The flag is set
    // here anyway, so that the guarantee does not rest on that argument
    // holding for ever.)
    //
    // Stage 2 is dispatched from pumpReloadJob, when stage 1 has landed and
    // the head's shape is a fact. If it cannot run - a Stop, or the stack
    // emptied under us - the arrivals do not happen and the summary SAYS which
    // frames are not in the measurement; it does not quietly fall back to
    // reading 480 MB per frame on this thread, which is the board row itself.
    if (j->axis.moved() && j->axis.want > j->axis.have) j->axis.arrived = true;
    app.reloadJob = std::move(j);
    return startReloadJob(app.reloadJob.get());
}

// ...and ONE frame, from the frame menu or the Files row's own Reload. The same
// job with one item and no stack: the membership rebuild has no subject, and the
// summary is the single-frame one (reloadOneFrameText). It is here rather than
// left synchronous because the cost is the SAME cost: one whole-file read and
// one decode, which is what #232 stage 2 measured at 451 ms for an OPEN of a
// 480 MB .npy and moved off this thread for that reason.
static bool startFrameReload(int imgIdx) {
    if (g_scriptedRun && !g_forceAsyncOpen) return false;
    if (imgIdx < 0 || imgIdx >= (int)app.images.size()) return false;
    const uint64_t uid = app.images[(size_t)imgIdx]->uid;
    if (app.reloadJob) {
        // FOLDED, and not discarded - startStackReload's rule and its reason
        // (review P1-2), which is about WHEN the read started and so says the
        // same thing about one frame as about a stack.
        if (app.reloadJob->oneUid == uid && !app.reloadJob->cancel.load()) {
            app.reloadJob->again = true;
            return true;
        }
        for (const auto& w : app.reloadQueue)
            if (w.oneUid == uid) return true;
        app.reloadQueue.push_back({ 0, uid, false });
        return true;
    }
    std::unique_ptr<App::ReloadJob> j(new App::ReloadJob());
    j->oneUid = uid;
    j->remoteExe = app.remoteExe;      // frozen, for startStackReload's reason
    j->exePath = app.exePath;
    j->name = app.images[(size_t)imgIdx]->name;
    App::ReloadJob::Item it;
    it.src = app.images[(size_t)imgIdx]->src;
    it.spec = reloadSpecOf(*it.src);
    j->items.push_back(std::move(it));
    j->groupEnd.push_back(1);
    app.reloadJob = std::move(j);
    return startReloadJob(app.reloadJob.get());
}

static bool startReloadJob(App::ReloadJob* j) {
    g_reloadStarts++;
    // The two thread probes belong to THIS gesture and are cleared with it.
    // Without the clear an assert about the arrivals could be satisfied by the
    // id an EARLIER reload left behind, which is the anti-vacuity half of the
    // only statement a headless test can make about a freeze (see
    // g_reloadArrivalThread). Stage 2 does not come through this function -
    // one gesture is one start - so the clear happens once per gesture.
    g_reloadDecodeThread = std::thread::id();
    g_reloadArrivalThread = std::thread::id();
    j->startedAt = nowSec();
    j->th = std::thread(reloadJobWorker, j);
    return true;
}

// The single-frame summary, and the ONE place its sentences live: the
// synchronous door in the Files panel and the job's landing both reach it, so
// the wording cannot differ by which thread read the file. (openLocalLand's
// discipline, one feature over.)
static std::string reloadOneFrameText(const ImageDoc& d, const std::string& note) {
    const std::shared_ptr<FrameSource>& src = d.src;
    std::string msg = "reloaded " + (src->path.empty() ? d.name : baseName(src->path)) +
                      (note.empty() ? "" : " - dimensions changed: " + note);
    // the §3.2 walk refreshed EVERY membership of this source, not just this
    // row: numbers moving in a stack the user never touched need a stated cause
    // - the same clause reloadStackFromDisk earns
    int sharedN = 0;
    std::vector<std::string> sharedNames;
    for (const auto& q : app.images)
        if (q.get() != &d && q->src == src) {
            sharedN++;
            std::string where = q->name;
            if (q->seqId != 0)
                if (const App::SeqInfo* si2 = seqInfo(q->seqId)) where = si2->name;
            if (std::find(sharedNames.begin(), sharedNames.end(), where) ==
                sharedNames.end())
                sharedNames.push_back(where);
        }
    if (sharedN) {
        char b2[192];
        snprintf(b2, sizeof b2, " - also refreshed %d shared frame(s) in \"%s\"",
                 sharedN, sharedNames[0].c_str());
        msg += b2;
        if (sharedNames.size() > 1)
            msg += " (+" + std::to_string((int)sharedNames.size() - 1) + " more)";
    }
    // the walk latched staleness on every reduced frame of every stack this
    // source belongs to (§10.1) - the toast must say so too. The row is a loose
    // frame, but a shared source can sit in stacks too, so the criterion is the
    // walk's own: memberships of THIS source.
    int staleAvg = 0;
    for (const auto& q : app.images) {
        if (q->avgOfPath.empty() || !q->avgStale || !q->avgSeqId) continue;
        for (const auto& r : app.images)
            if (r->src == src && r->seqId == q->avgSeqId) { staleAvg++; break; }
    }
    if (staleAvg)
        msg += " - " + std::to_string(staleAvg) + " computed frame(s) now stale "
               "(right-click > Recompute from stack)";
    return msg;
}


static void startNextQueuedReload();

// Called once per frame. Lands at most ONE item - see the note at the top of
// this block - and finishes the job when the worker has stopped and there is
// nothing left to land.
static void pumpReloadJob() {
    if (!app.reloadJob) { startNextQueuedReload(); return; }
    App::ReloadJob* j = app.reloadJob.get();
    // The stack went away under us (Close stack, closeAll, a session restore).
    // Treated exactly as a Stop: nothing more lands, and what landed stays.
    //
    // ...and it CANCELS the worker too (review P3-11). Without that, closing a
    // stack half-way through its reload left the worker reading and decoding
    // nine more frames for a stack that no longer exists, with the Files row
    // still saying "reloading 3 / 10" and the next stack in the queue waiting
    // behind work nobody will take. closeAll already does this; Close stack
    // reaches the same state by a different door and owes the same answer.
    // ...and a ONE-FRAME job's subject is a document, not a stack, so it is
    // asked about by uid (review): with `seqId != 0` alone a frame closed under
    // its own reload went on being read for nobody, which is the very thing
    // this term exists to stop.
    bool gone = false;
    if (j->seqId != 0) {
        gone = !seqInfo(j->seqId);
    } else if (j->oneUid != 0) {
        gone = true;
        for (const auto& q : app.images) if (q->uid == j->oneUid) gone = false;
    }
    if (gone && !j->cancel.load()) j->cancel.store(true);
    const bool stop = j->cancel.load() || gone;
    // §12.6: which list this pump is landing out of, and which counter says how
    // far it has got. One a frame in either stage, for the reason stated at the
    // top of this block - twelve swaps in one landing would move the freeze
    // from the reading to the swapping and nothing else.
    const bool second = j->stage == 2;
    int& landed = second ? j->arrivalsLanded : j->landedN;
    if (!stop && landed < j->doneCount.load()) {
        App::ReloadJob::Item& it = second ? j->arrivals[(size_t)landed]
                                         : j->items[(size_t)landed];
        if (second) {
            // ---- an ARRIVAL: a JOIN, not a swap -------------------------
            // There is no source to swap into - this frame is new - so
            // reloadLand has no subject and watchFrameAxisJoin is the landing:
            // the shape check against the plan's RECORD, the membership
            // inherited from the head, the registry, the stack revision.
            //
            // Asked only while the growth is still CONTIGUOUS. A refusal ends
            // it (watchFrameAxisJoin's own note says why), and the frames
            // behind the refusal are counted and said rather than attempted:
            // a stack holding the file's frames 0,1,2,4 is not the file's
            // first N, which is the one shape watchFrameAxisPlan applies to,
            // so the NEXT Reload would refuse this stack outright.
            if (j->axis.firstRefusal.empty())
                it.landed = watchFrameAxisJoin(j->seqId, j->axis, it.spec.fileFrame,
                                               std::move(it.pre.nd), it.pre.err);
            // A frame behind a refusal is not landing, so its decode is
            // released here rather than held to the end of the job. It costs a
            // whole frame of pixels and nothing will ever read it - the
            // arrivals are decoded as ONE group, so every one of them is in
            // memory by the time the first one lands (see reloadArrivalItems).
            it.pre.nd.reset();
            landed++;
        } else {
            std::string err, note;
            const std::string who = reloadItemName(it);
            if (reloadLand(it.src, it.spec, it.pre, err, &note)) {
                j->ok++;
                if (!note.empty()) {
                    j->dims++;
                    if (j->firstDims.empty()) j->firstDims = who + ": " + note;
                }
                if (!note.empty()) j->oneNote = note;
                reloadSharedTally(it.src, j->seqId, j->sharedN, j->sharedNames);
                it.landed = true;
            } else {
                j->failed++;
                if (j->firstErr.empty()) j->firstErr = who + ": " + err;
            }
            landed++;
        }
    }
    if (!j->done.load()) { g_reloadPumps++; return; }
    if (!stop && landed < j->doneCount.load()) return;   // one a frame, still
    // ---- this STAGE is over ------------------------------------------------
    j->th.join();
    // ordered by the join above, and to the probe this stage belongs to
    if (second) g_reloadArrivalThread = j->arrivalThread;
    else        g_reloadDecodeThread  = j->decodeThread;
    // ---- STAGE 1 -> STAGE 2: §12.6's arrivals, and this thread reads nothing
    //
    // THE SHAPE IS RECORDED HERE, and here is the only place it can honestly be
    // recorded: the re-read has landed, so the head frame's shape has stopped
    // being a moving target, and every joining frame below is measured against
    // what watchFrameAxisHead writes on this line. That is board row 319's
    // ruling, and the version it replaces - each arrival looking at "the head's
    // current shape" as it landed - is why the arrivals had to be synchronous
    // at all: the shape did not exist until the last item was in.
    //
    // A Stop, or a stack that emptied under us, skips this. Nothing then joins,
    // and the summary SAYS which frames are not in the measurement
    // (watch::frameAxisJoinText) - it does not fall back to reading the file on
    // this thread, which is the defect being removed.
    if (!second && !stop && j->seqId != 0 && j->axis.moved() &&
        j->axis.want > j->axis.have) {
        if (watchFrameAxisHead(j->seqId, j->axis) && reloadArrivalItems(j)) {
            j->stage = 2;
            j->doneCount.store(0);      // safe: no worker is running right now
            j->done.store(false);
            g_reloadArrivalStages++;
            j->th = std::thread(reloadJobWorker, j);
            g_reloadPumps++;
            return;                     // ...the gesture is not over yet
        }
        // No head left to measure against, or nothing to build: fall through to
        // the tail, which states the count it actually ended up with.
    }
    // An offered frame that never reached the join at all still owes a REASON.
    // Without this the sentence would default to watch::frameAxisJoinText's
    // "decode failed" about a frame nothing ever tried to decode, which is the
    // kind of invented cause this project refuses to print. The three ways it
    // happens are the three ways stage 2 can end early.
    if (j->axis.arrived && j->axis.firstRefusal.empty() &&
        j->axis.joined < j->axis.offered())
        j->axis.firstRefusal =
            stop ? "the reload was stopped before this frame was read"
                 : (j->stage == 2 ? "the reload ended before this frame was read"
                                  : "the stack had no frame left to measure it "
                                    "against");
    // Taken out of App before anything is said. The tail decodes a joining
    // member, fires a toast and may reach seqInfo again, and everything that
    // asks "is a reload running?" must see the truth, which is no.
    std::unique_ptr<App::ReloadJob> held = std::move(app.reloadJob);
    j = held.get();
    ReloadTally t;
    t.ok = j->ok; t.failed = j->failed; t.dims = j->dims; t.sharedN = j->sharedN;
    t.firstErr = j->firstErr; t.firstDims = j->firstDims;
    t.sharedNames = j->sharedNames;
    if (stop && j->landedN < (int)j->items.size()) {
        t.stoppedAfter = j->landedN;
        t.stoppedOf = (int)j->items.size();
    }
    // ---- a gesture that arrived WHILE this job was reading (review P1-2) ----
    // It was folded, which is not the same as answered: the read that just
    // finished started before that request was made. So it goes to the FRONT of
    // the queue and runs once more, through the one door that takes a fresh plan
    // and a fresh stat (startNextQueuedReload -> startStackReload). A Stop is
    // the exception and needs none: Stop means stop, and it has cleared the
    // queue already.
    //
    // BEFORE the one-frame branch below, which returns: a re-run put after it
    // would be reached by stacks only, and the rule is about WHEN the read
    // started, which does not depend on how many items the job had.
    //
    // It is BOUNDED, with one frame's exception worth stating. A gesture that
    // arrives while the re-run WAITS folds into the queue entry, so one flight
    // adds at most one re-run - except in the single frame between this landing
    // and the next pump's startNextQueuedReload, where the queue entry exists
    // but no job does, so a gesture there starts a job of its own and the
    // re-run then folds into IT. One extra flight, once, on a one-frame window.
    // Accepted (review): it is finite, it re-reads the file the user asked
    // about, and the alternative is a second piece of bookkeeping about which
    // request is whose.
    if (j->again && !stop) {
        if (j->oneUid != 0) {
            bool alive = false;
            for (const auto& q : app.images) if (q->uid == j->oneUid) alive = true;
            if (alive)
                app.reloadQueue.insert(app.reloadQueue.begin(), { 0, j->oneUid, false });
        } else if (seqInfo(j->seqId)) {
            app.reloadQueue.insert(app.reloadQueue.begin(),
                                   { j->seqId, 0, j->againAutomatic });
        }
    }
    if (j->oneUid) {
        // ONE frame: no membership, no #56 latch (the mark is a STACK's), and
        // the single-frame sentence.
        ImageDoc* d = nullptr;
        for (const auto& q : app.images)
            if (q->uid == j->oneUid) { d = q.get(); break; }
        if (t.failed)                    toast("reload failed - " + t.firstErr, true);
        else if (t.stoppedAfter >= 0)    toast("stopped: " + j->name);
        else if (d && !j->items.empty() && j->items[0].landed)
            toast(reloadOneFrameText(*d, j->oneNote));
        return;
    }
    if (!seqInfo(j->seqId)) return;      // the stack is gone; nothing to say it to
    int failed = 0;
    const std::string msg = reloadStackTail(j->seqId, j->plan, j->hadBefore, t, &failed,
                                            j->peer);
    watchReloadFinish(j->seqId, j->automatic, msg, failed, j->pinWas, j->axis);
}

static void startNextQueuedReload() {
    if (app.reloadJob || app.reloadQueue.empty()) return;
    const App::ReloadWait w = app.reloadQueue.front();
    app.reloadQueue.erase(app.reloadQueue.begin());
    // The fall-back matters for startNextQueuedOpen's reason: a queue can
    // outlive the flag that filled it, and a queued reload that quietly
    // disappeared is the failure the queue exists to prevent.
    if (w.oneUid) {
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[(size_t)i]->uid == w.oneUid) {
                if (!startFrameReload(i)) {
                    std::string err, note;
                    ImageDoc* d = app.images[(size_t)i].get();
                    if (reloadSource(d->src, err, &note))
                        toast(reloadOneFrameText(*d, note));
                    else
                        toast("reload failed - " + baseName(d->src->path) + ": " + err, true);
                }
                return;
            }
        return;                          // the frame closed while it waited
    }
    if (!seqInfo(w.seqId)) return;
    if (!startStackReload(w.seqId, w.automatic)) reloadStackSync(w.seqId, w.automatic);
}

// Drive a started reload to the end on THIS thread. waitForOpenJob's twin and
// its reason: a run with no frame loop - a selftest, or a scripted gesture -
// would otherwise leave the job hanging and the files un-re-read. The waiting is
// the ONLY thing this puts back.
static void waitForReloadJob() {
    while (app.reloadJob || !app.reloadQueue.empty()) {
        pumpReloadJob();
        if (app.reloadJob && !app.reloadJob->done.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Temporal stats for ONE stack that is NOT opened: fired from the browser's
// group row, so the answer to "is this set worth transferring?" costs zero
// pixels. One stack per call, always - the multi-selection caller that used to
// share this door flattened N stacks into one file list and got a sigma_t over
// a time axis that never existed, and #107's ruling deleted it rather than
// teaching it to refuse (sigma_t IS a stack's attribute, docs/terminology.md).
// The frames of one group row are one stack by construction, which is why this
// function needs no gate of its own.
// Runs on the MEASURE worker - the browse worker serializes
// behind LIST/SCAN and shares the browsing ssh session, so a 300-frame
// aggregate there would freeze navigation. cfaType stays 0 on purpose:
// guessing a mosaic for a file nobody opened would silently split the planes
// wrong - the panel tag says plane=all instead.
static void requestBrowseTemporal(const std::string& host,
                                  std::vector<std::string> files,
                                  const std::string& label, int port = 0) {
    if (files.empty()) return;
    sortFramesNumerically(files);
    App::ServerTemporal& S = app.srvTemporal;
    S = App::ServerTemporal{};
    S.seqId = -2;                     // sentinel: no SeqInfo backs this result
    S.detached = true;
    S.label = label;
    S.host = host;
    S.files = files;
    S.token = g_measureToken++;
    S.pending = true;
    App::MJob j;
    j.token = S.token;
    j.op = rp::MOP_TEMPORAL_STATS;
    j.url = makeRemoteUrl(host, files[0], port);
    if (files.size() > 1)
        for (const auto& f : files) j.files.push_back(f);
    mEnqueue(std::move(j));
    app.showTemporal = true;          // the result lands in the Temporal panel
    app.focusTemporal = true;
}


// Close the transient preview, if one is still unpromoted.
static void dropPreview() {
    if (!app.previewUid) return;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->uid == app.previewUid) {
            if (!app.images[i]->preview) break;      // promoted: not ours anymore
            // through the one close path that re-points app.current BY UID.
            // Erasing by index and clamping only the tail silently re-targets A
            // at a different document whenever the preview sits below current:
            // every uid-keyed cache then recomputes for the wrong image, and on
            // openRemote's four failure returns nothing puts it back.
            closeImages({ i });
            break;
        }
    app.previewUid = 0;
    // the sequence it belonged to goes with it (stepPreviewFrame restores its
    // own copy across the openRemote inside a step)
    app.previewFiles.clear();
    app.previewFrames = 0;
    app.previewIndex = 0;
    app.previewLabel.clear();
    app.previewHost.clear();
    app.previewPort = 0;
}

// Walk the previewed sequence without opening it. Each step replaces the one
// preview slot, so browsing a 300-frame capture costs one frame of transfer at
// a time - the whole point of previewing rather than opening. A stack of
// numbered files steps across previewFiles; a frame-axis file steps the frame
// index inside the one file it previews (openRemote re-derives that state).
static void stepPreviewFrame(int delta) {
    if (app.previewFiles.size() >= 2) {
        int n = (int)app.previewFiles.size();
        int want = std::clamp(app.previewIndex + delta, 0, n - 1);
        if (want == app.previewIndex) return;
        // the machine the previewed paths live on rides WITH the preview
        // (previewHost/previewPort): the slot is global across N Browse
        // instances, so "the" browse panel's host is no longer a question
        // that has one answer
        std::string host = app.previewHost;
        int port = app.previewPort;
        std::vector<std::string> files = app.previewFiles;   // openRemote drops the preview
        std::string label = app.previewLabel;
        app.previewIndex = want;
        // a failed step must not restore the list over a preview that is now gone
        if (!openRemote(makeRemoteUrl(host, files[want], port), true)) return;
        app.previewFiles = std::move(files);
        app.previewIndex = want;
        app.previewLabel = std::move(label);
        app.previewHost = host;
        app.previewPort = port;
    } else if (app.previewFrames >= 2) {
        int want = std::clamp(app.previewIndex + delta, 0, app.previewFrames - 1);
        if (want == app.previewIndex) return;
        for (const auto& d : app.images)
            if (d->uid == app.previewUid && d->preview) {
                std::string url = d->src->remoteUrl;   // openRemote frees the doc
                // Same guard the file-stack branch above carries. openRemote
                // calls dropPreview() before it can fail, so a discarded return
                // leaves previewIndex and previewFrames describing a preview
                // that is no longer there - and the scrub bar keeps re-running
                // the failing open, one toast per press.
                if (!openRemote(url, true, want)) {
                    app.previewIndex = 0;
                    app.previewFrames = 0;
                }
                return;
            }
    }
}

// ---- TIME THE USER DID NOT GET BACK ---------------------------------------
// A remote open BLOCKS the UI thread: ensureUiSession may start a whole peer,
// and meta+tile are round trips read to completion inside the frame ("the
// window is not repainting while this session reads", ensureUiSession). Nothing
// is drawn and no input is processed for as long as that takes.
//
// ImGui decides whether a press is the second half of a double-click by
// comparing g.Time against the previous press - and g.Time advances by the REAL
// duration of every frame, blocked or not (UpdateMouseInputs: is_repeated_click
// needs g.Time - MouseClickedTime < MouseDoubleClickTime, 0.30 s by default).
// So a first click that fetches something spends, from its own frame, the whole
// window its second click needs to arrive in.
//
// That is the report「ダブルクリックすると一回目はダブルクリック判定されずに
// previewで表示されてしまう」. Click one previews (that is its job); click two
// is handed to ImGui a second later, counts as a FIRST click, so
// IsMouseDoubleClicked is false, rbOpenRow never runs, and what stays on screen
// is the preview - the Open silently missing. Measured on a peer answering
// meta/tile with 400 ms of latency: press one at t=0.794, press two seen at
// t=1.740, MouseClickedCount 1 - and the same gesture against the same peer
// with no latency reports count 2 and opens the file.
//
// The user pressed twice in their own time; the clock ran in ours. So the time
// the UI thread spends BLOCKED is given back to every button's click clock -
// the interval is measured in time the user could actually see and act in.
// MouseClickedPos is deliberately untouched: a click that moved still starts a
// new gesture, and the pointer is the half of the rule that stayed honest.
//
// It cannot manufacture a double-click out of two separate ones: after the
// credit the clock runs normally again, so a second click that really arrives
// seconds later is still seconds later.
struct UiThreadStall {
    double t0 = nowSec();
    static int depth;
    UiThreadStall() { depth++; }
    ~UiThreadStall() {
        double d = nowSec() - t0;
        if (--depth != 0 || d <= 0 || !ImGui::GetCurrentContext()) return;
        ImGuiIO& io = ImGui::GetIO();
        for (int i = 0; i < IM_ARRAYSIZE(io.MouseClickedTime); i++) {
            io.MouseClickedTime[i] += d;
            io.MouseReleasedTime[i] += d;
        }
    }
};
int UiThreadStall::depth = 0;

// Returns false when nothing was opened. It has four failure returns AFTER
// dropPreview() has already cleared the preview state, and its callers used to
// have no way to know: rbActivateRow repopulated previewFiles unconditionally,
// so a failed open left a scrub bar with 2+ entries and previewUid == 0 - a
// bar for a preview that does not exist, whose buttons and , / . keys each
// re-ran the failing open, one toast per press, forever.
// ---- THE ONE DOOR A HEADERLESS FILE ON A PEER GOES THROUGH -----------------
//
// A headerless row on a PEER, named by anything: a Browse double-click, a url
// on the command line, a drop, File > Open, a folder opened as a stack. All of
// them arrive here, and this is the difference between what stage 4 shipped and
// what a user found on 2026-09-29.
//
// WHAT WAS WRONG. This logic existed, and exactly one caller had it: the Browse
// row (openRemoteRawRow, bound into core/browse/host.h). Every other door
// called openRemote with a null recipe, which is unanswerable by construction,
// so the answer was Session::recipeServable's refusal - and that refusal still
// carried STAGE 1's sentence ("this link cannot yet carry that declaration"),
// six weeks after protocol 11 made it false. Two defects that read as one: the
// door that did not ask, and the sentence that blamed the wire for it.
//
// The order is #166's, unchanged, because it is the same question asked about a
// different disk: has this SESSION already been told how to read a file of
// exactly this many bytes? If yes it applies and says so; if no it asks. What
// is NOT here is a guess - a headerless file is the one shape that does not
// defend itself (4096x3000 and 3000x4096 are the same byte count), so the only
// thing allowed to apply without being picked is a choice the operator made
// minutes ago and still remembers.
//
// `sizeBytes == 0` means the caller has only a url. The bytes are then asked
// for with ONE MSG_LIST, which is docs/features/remote/remote-headerless-design.md
// 5.2's answer in full - the byte count has travelled in a listing entry since
// protocol 3, and handleList answers for a non-directory path with that one
// entry. No new wire, one round trip.
//
// TIME THE USER DOES NOT GET BACK: that LIST is a synchronous round trip on the
// UI thread, and so is the ensureUiSession above it - the same price every
// other synchronous remote door pays (see UiThreadStall). Measured shape: one
// request, one reply, a directory entry's worth of bytes; against a peer with
// 400 ms of latency it is one 400 ms stall before the dialog appears, and the
// alternative is a dialog that cannot fill in its own byte count.
enum RawDoorAnswer { RD_Opened, RD_Asked, RD_Refused };

// How many times this door has been ENTERED. It exists for the same reason
// g_rawDialogsRaised does, one layer out: the assertions that matter here are
// about WHICH door a url reaches, and a test that only drives the door directly
// cannot tell whether anything routes into it. The review found exactly that -
// the block in openRemote that sends a headerless peer name here could be
// deleted and every assertion still passed (review #17). Monotonic, never
// reset, and read only by the selftest.
static int g_rawDoorEntries = 0;

// The byte count the peer already knows, for ONE file. Empty err = `out` is it.
static bool remoteRawFileSize(const std::string& host, const std::string& rpath, int port,
                              uint64_t& out, std::string& err) {
    if (!ensureUiSession(host, err, port)) return false;
    std::vector<remote::Entry> es;
    if (!app.uiSession.list(rpath, es, err)) return false;
    // handleList returns the one entry for a path that is not a directory. The
    // NAME has to match: this count is about to key #166's size binding, and a
    // binding is the one thing in this viewer that decodes pixels a particular
    // way without being asked on the spot - a number taken from the wrong entry
    // could match a recipe the operator chose for some OTHER file of that size
    // and open this one as a plausible picture of the wrong bytes. A path that
    // is not there comes back as "no such path" from the call above; anything
    // else that does not name this file is said rather than guessed at.
    const std::string leaf = baseName(rpath);
    for (const remote::Entry& e : es)
        if (!e.dir && !e.group && e.name == leaf) { out = e.size; return true; }
    err = "the peer's listing does not name " + leaf + " as a file";
    return false;
}

// THE HALF OF A RECIPE THAT DOES NOT TRAVEL. A RawWire carries six numbers and
// a flags word; a CROP is not among them and cannot be, because the peer's job
// is to serve the bytes a reading asks for and a crop is a view of what came
// back (§3.2). So every door that applies a recipe to a peer's file has to
// finish it on this side, and until this review two of them did not: the file
// opened whole while the applied line read "... crop 10,10 100x100".
//
// The session restore worked this out first and wrote down the answer
// (session.inc, the raw3 branch): cropInPlace, and when it refuses because the
// frame on screen is still a decimated preview, park it and let the
// full-resolution arrival finish the job - G10 / PR #177 is the record of what
// ignoring that `false` cost. This is that answer, in one place, because two
// spellings of "finish the recipe" is how one of them ends up not doing it.
//
// Returns whether the crop is DONE: false means it is parked, which the
// caller's line has to say rather than claim a crop that is not there yet.
static bool applyRecipeCropHere(bool on, int x, int y, int w, int h, ImageDoc* d) {
    if (!on || w <= 0 || h <= 0 || !d) return true;
    if (cropInPlace(*d, x, y, w, h)) return true;
    parkRestoreWait(*d, x, y, w, h, false);     // false: the operator's, not a session's
    return false;
}

static RawDoorAnswer openRemoteRawDeclared(const std::string& url, uint64_t sizeBytes) {
    g_rawDoorEntries++;
    std::string host, rpath;
    int port = 0;
    remote::parseUrl(url, host, rpath, &port);
    // Before anything else: a peer that cannot carry a declaration. Said from
    // the number it announced, not discovered by sending and being refused -
    // and it has to be said HERE too, or the dialog would collect six fields
    // and then throw them away (rp::rawTooOldText).
    std::string cerr;
    if (!ensureUiSession(host, cerr, port)) { toast("remote: " + cerr, true); return RD_Refused; }
    if (app.uiSession.peerVersion() < 11) {
        toast(rp::rawTooOldText(app.uiSession.peerVersion(), baseName(rpath)), true);
        return RD_Refused;
    }
    // The byte count, from the listing when the caller did not have one. It is
    // asked for BEFORE the binding is consulted because the binding's key IS
    // the byte count: without a number there is no question to ask and no
    // dialog to raise (rawGuessDims works from nothing else), so the honest
    // answer is to say which fact is missing rather than to open a dialog with
    // a zero in it.
    if (sizeBytes == 0 && !remoteRawFileSize(host, rpath, port, sizeBytes, cerr)) {
        toast(baseName(rpath) + ": a headerless file's geometry is worked out from its "
              "byte count, and this peer would not give one - " + cerr +
              "\n  open it from the Browse panel (the listing carries the count), or "
              "state the geometry with --raw-size", true);
        return RD_Refused;
    }
    if (const RawRecipe* r = rawRecipeForSize((size_t)sizeBytes)) {
        RawDialog d;
        rawRecipeIntoDialog(*r, d);
        d.path = url;
        d.fileSize = (size_t)sizeBytes;
        const rp::RawWire rw = rawWireFrom(d);
        if (!openRemote(url, false, 0, 0, &rw)) return RD_Refused;
        std::string line = rawRecipeAppliedLine(*r, baseName(rpath), (size_t)sizeBytes);
        // The line names the recipe, crop and all, so the crop had better be
        // there - and when it is only parked the line says which part of
        // itself has not happened yet instead of over-claiming by a frame.
        if (!applyRecipeCropHere(r->cropOn, r->cropX, r->cropY, r->cropW, r->cropH,
                                 app.images.empty() ? nullptr : app.images.back().get()))
            line += "\n  the crop is applied when the full-resolution frame lands "
                    "(this view is decimated)";
        toast(line);
        return RD_Opened;
    }
    // RD_Asked only when the dialog is actually STANDING and is this url's.
    // openRawDialogFor refuses a second question over a live park and says so,
    // and reporting that as "asked" told the caller a geometry prompt was up
    // for a file it was not up for (review P3-12).
    if (!openRawDialogFor(url, sizeBytes)) return RD_Refused;
    return RD_Asked;
}

// The shape core/browse/host.h binds (see main.cpp's table). The panel has the
// byte count off its own listing row, so it passes one and no LIST is sent.
static void openRemoteRawRow(const std::string& url, uint64_t sizeBytes) {
    openRemoteRawDeclared(url, sizeBytes);
}

// The 4-argument shape core/browse/host.h binds. See the note at that binding.
static bool openRemoteNpz(const std::string& url, const std::string& onlyMember,
                          bool reuseExisting);  // fwd; default is declared in remote_client.inc

// §4.12 OVER A LINK, at the door rather than after it (#218 review).
//
// The memo is a RECORD OF A CHOICE - this user opened this url with this reader
// once - and the whole point of remembering one is that the choice is applied
// again BEFORE anything else is consulted. That is exactly what the two remote
// doors did not do: openPath asked about the url SCHEME first and openRemote
// asks the format table ("will the peer serve a file called this"), so a
// remembered url the peer happens to read natively came back as the peer's own
// array - the ORIGIN's pixels, under the reader's name - and one the peer does
// not read came back as a refusal, with the saved __pixels_* member nowhere.
// The local door has always asked the memo first (openPath's second block); this
// is the same question, asked one layer out.
//
// WHICH DOORS ASK. The ones that mean "open this url, fresh, as itself": File >
// Open, the command line, a drop, a desktop shortcut (openPath), and the Browse
// panel's registered open. The ones that do NOT are the ones that already name
// what they want, and each would be a different bug if it did:
//
//   a PREVIEW is a decimated tile of the peer's own array and one click may
//   never start Python on another machine (§10.4);
//   an explicit §3.3 READING is the user saying how to cut the peer's array,
//   which is a statement about that array and not a request for another one;
//   a headerless DECLARATION and a KEYED request are the same shape of thing -
//   an address into bytes the peer already has;
//   a SESSION line names a reader and never runs one (#179 ruling C). Its
//   reader branch consults the memo itself, and only when the LINE says the
//   pixels were a reader's (the __pixels_ member) - so a url saved as native
//   reopens as native even while the same url sits in the memo from another
//   day. That decision stays where the line is, and this door must not
//   pre-empt it: session restore reaches openRemote directly.
//
// Returns whether the memo answered at all. `opened` is whether pixels landed -
// a remembered reader that fails says so and the caller does NOT fall through
// to the native path, because "the reader you chose could not run" is the
// answer, and quietly handing back the origin's own array instead is the
// silence this whole door exists to prevent.
static bool openRemoteMemoFirst(const std::string& url, bool& opened) {
    opened = false;
    const std::string spec = readerFor(url);
    if (spec.empty()) return false;              // no record: not this door's business
    // STARTED, not waited for (issue #232 stage 2). This was the last door still
    // calling the synchronous round trip - stage 1 left it because a selftest
    // depended on it - and it is the same door the local memo replay was: the
    // ordinary reopen of a url a reader was once chosen for, which spent the
    // whole of a 300 s timeout's worth of possible wait on the UI thread.
    //
    // startReader has carried REMOTE jobs since #180: a session of its own on a
    // thread of its own (two threads framing requests into one ssh stdin
    // desynchronise the stream permanently), the Files row's timer, and a Stop.
    // There is nothing to add - only this call to redirect.
    //
    // `opened` now means THIS DOOR HAS IT, which is the only thing a door can
    // honestly report once the reading is another thread's: a job running or
    // waiting. It is false exactly where it was false before - when the link
    // could not even be prepared, which startReader has already said. Nothing
    // falls through to the native path either way: a url the user chose a reader
    // for is that reader's to refuse, and quietly handing back the origin's own
    // array instead is the silence this door exists to prevent.
    startReader(url, spec, baseName(url) + " via " + spec + ": ");
    opened = app.rdJob != nullptr || !app.rdQueue.empty();
    return true;
}

static bool browseOpenRemote(const std::string& url, bool asPreview, int frame, int npyRead) {
    // The registered open, and only it: see openRemoteMemoFirst for why each of
    // the three exclusions is one.
    if (!asPreview && frame == 0 && npyRead == NR_NATIVE) {
        bool opened = false;
        if (openRemoteMemoFirst(url, opened)) return opened;
    }
    return openRemote(url, asPreview, frame, npyRead, nullptr);
}

static bool openRemote(const std::string& url, bool asPreview, int frame, int npyRead,
                       const rp::RawWire* rw, const remote::KeyedRef* kd,
                       const std::string& member) {
    UiThreadStall stallCredit;      // see above: a blocked frame is not the user's
    std::string host, rpath;
    int port = 0;
    if (!remote::parseUrl(url, host, rpath, &port)) {
        toast("expected ssh://user@host/path/to/file.npy", true);
        return false;
    }
    // ---- #111: WHOSE READER --------------------------------------------------
    // Everything below this block is the peer's protocol - META then TILE - and
    // the peer reads .npy. That was fine while .npy was all the viewer read; it
    // stopped being fine when core/imagefile.h gained PNG, JPEG, TIFF, OpenEXR
    // and y4m, because a `local://` url names a file on THIS disk and this
    // process has a reader for it. Sending it round through a peer that does
    // not is the whole of the bug: five formats reachable from File > Open and
    // dead from the Browse panel listing the very folder they sit in.
    //
    // ONE place, not one per call site. Every Browse open - a double-click, a
    // search hit, Enter over a multi-selection, the row menu - mints a url with
    // makeRemoteUrl and comes through here, so routing at the door means none
    // of them had to learn the rule and none of them can drift from it.
    // A headerless file crosses the link only WITH a declaration (protocol 11,
    // docs/features/remote/remote-headerless-design.md). peerServesName is still the predicate
    // for "answerable with nothing else said" - it is what the listing and the
    // one-click preview ask - so the door adds the second half here rather than
    // widening that predicate underneath them.
    // A KEYED request is not about a name on the peer's disk (protocol 12/13):
    // it addresses one array inside something the peer already materialised, so
    // the format gate below - which asks "will the peer serve a file called
    // this" - has nothing to decide. It is skipped rather than widened: a .npz
    // is deliberately NOT in peerServes (its Browse row lives through
    // peerServesDeclared and its one-click preview stays off, §10.4), and
    // widening the predicate to let a member through would take the preview
    // with it.
    const bool headerlessOk = imagefile::isHeaderless(rpath) && rw != nullptr;
    if (!kd && !headerlessOk && !peerServesName(rpath)) {
        if (host.empty() && viewerReadsName(rpath)) {
            // A PREVIEW is a decimated tile and only the peer path makes one.
            // The Browse click does not ask for one on these rows (see
            // rbActivateRow); anything that does is told no rather than being
            // handed a registered open it did not want.
            if (asPreview) return false;
            const size_t before = app.images.size();
            const int starts0 = g_openStarts;
            openPath(rpath);              // the File > Open door, which SAYS what
            // ...it did - including refusals. An open that went to a WORKER has
            // landed nothing yet and has still been answered, so "did this door
            // take it" is either a document or a started job (issue #232 stage
            // 2): a door cannot report failure because the reading moved off
            // this thread.
            return app.images.size() > before || g_openStarts != starts0;
        }
        // A CONTAINER on the far side (issue #217). It comes after the local
        // fall-through above on purpose: a local:// url names a file on THIS
        // disk and the local door opens it with the picker, the metadata and
        // the identity a member deserves, which is #111's rule and §10.3's
        // record of it ("local:// は今日と同じく openPath へ回す"). What was
        // missing was the ssh:// road, and this is it.
        //
        // A PREVIEW is refused rather than answered: one click makes a
        // decimated tile of ONE geometry and a container has none (§10.4 -
        // .npz is in peerServesDeclared, so the row lives and opens, and it is
        // deliberately not in peerServes, so the preview gate stays where it
        // is).
        if (imagefile::isNpz(rpath)) {
            if (asPreview) return false;
            return openRemoteNpz(url, member);
        }
        // A HEADERLESS name on a peer, with no recipe in hand. It is not a
        // refusal - it is a question, and this is where it gets asked (the
        // user's report of 2026-09-29, PR #278). The Browse row has asked it
        // since #188; every other door - a url on the command line, a drop,
        // File > Open, a session line with no geometry - came here instead and
        // got a sentence blaming the wire. ONE place, not one per call site,
        // which is #111's own rule for this function three blocks up: every
        // door mints a url and comes through here, so routing at the door means
        // none of them had to learn the rule and none of them can drift.
        //
        // local:// never reaches this line: the #111 block above sends it to
        // openPath, where the LOCAL dialog asks the same six questions about
        // bytes on this disk (5.4, and F4e asserts it).
        //
        // A PREVIEW is told no and says why, exactly as it was before. One
        // click is selection; a geometry prompt raised by a single click would
        // break the listing's own rule (5.3's recorded judgment), and the
        // sentence is the only half of this that a preview gets.
        if (imagefile::isHeaderless(rpath)) {
            if (!asPreview) return openRemoteRawDeclared(url, 0) == RD_Opened;
            toast(baseName(rpath) + ": " + imagefile::peerRefusal(rpath), true);
            return false;
        }
        // ssh://, or a name nothing here reads: refused BY NAME and before the
        // round trip, with the reason and the way out. The peer's own answer to
        // this file is "not a .npy file", which blames the file for a limit
        // that belongs to the link.
        const std::string why = host.empty() ? viewerRefusalFor(rpath) : peerRefusalFor(rpath);
        toast(baseName(rpath) + ": " + why, true);
        // ...and the door the refusal now NAMES (issue #180 stage 2). Since a
        // reader runs on the peer, "choose a reader" is a true offer for a
        // remote row too, so the dim row leads to the same panel a local
        // unreadable file leads to - openPath's own openReaderPicker call, one
        // layer out. A refusal that names a mechanism and does not open it is
        // the half-feature this whole file refuses to ship; before stage 2
        // there was no mechanism to name, which is why the sentence said so.
        if (!asPreview) openReaderPicker(url, why);
        return false;
    }
    if (asPreview) dropPreview();          // ONE slot: the next look replaces it
    // identity BEFORE the bytes (§6.2), the same discipline as every local
    // loader: for a local:// url the peer reads THIS disk, so stat it now -
    // an overwrite completing during the tile reads below binds the OLD tuple
    // to the OLD pixels, and a later re-open stats fresh and re-decodes.
    // ssh:// stays 0/0 (a peer's disk cannot be statted from here).
    FrameSource urlStat;
    urlStat.remoteUrl = url;
    statLocalUrl(urlStat);
    // Board 305: WHICH of these phases is the 344 ms. Every one of them is on
    // this thread, and every one is a candidate for moving off it - which is why
    // the marks are here and not inside a profiler. `VIEWER_TIME_OPEN=1` only.
    OpenPhase phase("openRemote");
    std::string err;
    // app.uiSession, never the browse worker's: two threads framing requests
    // into one ssh stdin desynchronise the stream permanently, and the worker's
    // stop() would free the pipe under this thread's blocking read.
    if (!ensureUiSession(host, err, port)) { toast("remote: " + err, true); return false; }
    phase.lap("ensureUiSession");
    // The DECLARED reading (§3.3), and it goes to META as well as to TILE: a
    // reading changes the geometry, so asking META natively and re-deriving
    // w/h/ch/frames here would put the layout rule on both sides of the link
    // with nothing checking they agree - which is what #71 cost a release.
    // Session::meta refuses a declaration a v8 peer cannot serve BEFORE sending.
    remote::Meta m;
    const uint64_t rxBefore = app.uiSession.bytesReceived();
    if (!app.uiSession.meta(rpath, m, err, npyRead, rw, kd)) { toast("remote: " + err, true); return false; }
    phase.lap("META round trip");
    // The peer fell back to native because the declaration does not fit this
    // shape - a session line against a file whose rank changed under it. The
    // local decoder's answer is a note and nothing else: the DECLARATION is
    // still remembered on the source (a rank that changed once may change back,
    // and forgetting it would silently discard what the user said), and
    // srcIdentityKey normalises it through npyKeyRead so it does not key a
    // tuple it did not produce. Same sentence, same two decisions, here.
    std::string readNote;
    if (m.readFellBack)
        readNote = "re-read choice does not fit " + rp::npyShapeText(m.shape) +
                   "; read natively";
    frame = std::clamp(frame, 0, std::max(0, (int)m.frames - 1));

    // First view: the whole frame decimated to something a screen can show. A
    // zoom or a pan asks for a better tile; nothing else on screen costs a byte.
    int step = std::max(1, (int)ceilf(std::max(m.w, m.h) / 1600.0f));
    std::vector<float> px;
    int tw = 0, th = 0, tch = 0;
    std::string dt;
    rp::F32Loss tloss;                        // what float32 cost these pixels
    if (!app.uiSession.tile(rpath, frame, 0, 0, m.w, m.h, step, px, tw, th, tch, dt, err,
                            npyRead, &tloss, rw, kd)) {
        toast("remote: " + err, true);
        return false;
    }
    phase.lap("TILE + to float");
    if (tw < 1 || th < 1 || tch < 1 || tch > 4) {   // a 0ch doc would crash later
        toast("remote: peer returned a degenerate frame", true);
        return false;
    }
    auto doc = std::make_unique<ImageDoc>();
    doc->name = baseName(rpath) + (member.empty() ? std::string() : ":" + member) +
                (frame > 0 ? " #" + std::to_string(frame) : "") +
                (step > 1 ? "  (1/" + std::to_string(step) + ")" : "");
    FrameSource& S = *doc->src;
    if (kd) doc->materializedRunId = app.nextMaterializedRunId++;
    S.mtime = urlStat.mtime;                 // the PRE-read stat: 0/0 for ssh://,
    S.fsize = urlStat.fsize;                 // the disk baseline for local://
    S.path = url;
    S.dtype = dt;
    S.w = tw; S.h = th; S.ch = tch;
    S.data = std::move(px);
    // What float32 cost THESE samples. A decimated first view examines only the
    // samples it fetched, and says so by counting them - "3 of 40000" from a
    // preview and "3 of 4000000" from the full frame are both true statements
    // about the pixels on screen, which is the only array anyone can read a
    // number off. pumpRemoteFetch replaces this wholesale with the full frame's.
    S.f32loss = tloss;
    // §3.3 over the link (issue #124). npyShape is what the PEER's META said the
    // file declares - empty from a peer older than protocol 9, which is the
    // honest value and the one the Inspector distinguishes from "not a .npy".
    // npyRead is the declaration this open was made under; it is also part of
    // the identity tuple (srcIdentityKey -> npyKeyRead), so two readings of one
    // remote url are two residents and never silently share pixels, exactly as
    // two readings of one local file are.
    S.npyShape = m.shape;
    S.npyRead = npyRead;
    S.remoteProto = app.uiSession.peerVersion();
    // WHICH array of a materialisation these pixels are (protocol 12/13). It
    // rides on the source for the two reasons every other remote fact does: the
    // full-resolution swap and the sibling frames re-ask the peer and must ask
    // for the SAME array, and the identity tuple has to tell two members of one
    // container apart - which `member` already did for a local .npz and now
    // does for a remote one, through the very same field.
    if (kd) {
        S.remoteKey = kd->key;
        S.remoteNode = kd->node;
        S.remoteKeyKind = kd->kind;
        S.materializedIssuer = kd->kind;
        S.remoteKeyRead = npyRead;
        S.remoteKeyLayout = kd->layout;
        S.remoteKeyRequires15 = kd->requires15;
    }
    S.member = member;
    // The recipe this open was made under, recorded on the SOURCE. Two reasons
    // and both are load-bearing:
    //
    //   IDENTITY (§6.2). srcIdentityKey's raw branch keys on exactly these
    //   fields, so two recipes over one url are two residents and never
    //   silently share pixels - the same rule two readings of one .npy follow.
    //   The fields were already there for the local door; a remote raw doc
    //   simply fills the same ones, which is why this feature needed no new
    //   identity machinery at all.
    //
    //   M1 (docs/features/settings/settings-inventory.md §4.1). A headerless read is nothing but
    //   measuring-class settings, so it declares itself in the note - and the
    //   note has to be the SAME sentence the local door writes, or one file
    //   would describe itself two ways depending on which end opened it.
    if (rw) {
        S.rawDtype = (int)rw->dtype;
        S.rawInterp = (int)rw->interp;
        S.rawOffset = (int)rw->offset;
        S.rawLE = (rw->flags & rp::RW_LITTLE_ENDIAN) != 0;
        S.srcW = (int)rw->w; S.srcH = (int)rw->h;
        // ...and the CFA PHASE, on the DOC (it is display, not identity - 3.2
        // keeps it out of srcIdentityKey on purpose). Set here, above the note,
        // because the note quotes it: without this line a remote Bayer frame
        // demosaiced as RGGB whatever the dialog was told, the note said RGGB
        // about pixels that were not, and a session came back a different
        // colour from the one it saved.
        doc->cfaPattern = rp::rawWireCfaPattern(rw->flags);
    }
    // The Inspector prints a non-empty note as a LINE OF ITS OWN, directly above
    // the Interpret combo. A note the head frame carries and its siblings do not
    // makes that line appear and vanish as the user steps the stack, and every
    // control under it moves - so the note has to be stack-constant, exactly as
    // derived stacks decided it (materializeDerivedFrame: "the SAME note on every
    // frame"). The sibling frames are minted by pumpRemoteFetch, so this string
    // travels to them through RFetchJob::note.
    // Two things are deliberately NOT in it. The SIZE: the row immediately above
    // already prints "%dx%d %dch %s", and docs/background/project/todo-open.md item 11 asked for the
    // duplicate to go. The HOST when there is none: a local:// peer serves files
    // off this disk and was not fetched "from" anywhere, which is the same call
    // the "opened X" toast at the end of this function makes.
    doc->note = "remote";
    if (!host.empty()) doc->note += " " + host;
    // ...and, for a headerless file, the DECLARATION the numbers were read
    // under (M1, docs/features/settings/settings-inventory.md §4.1). The same sentence the local
    // door writes, from the same helper: a size, a header offset and a byte
    // order each change every number on the screen, and a file that described
    // itself one way from disk and another over a link would be the "two
    // answers for one file" defect one level below where #148 removed it.
    if (rw) {
        RawDialog rd;
        rd.dtype = (int)rw->dtype; rd.interp = (int)rw->interp;
        rd.w = (int)rw->w; rd.h = (int)rw->h; rd.offset = (int)rw->offset;
        rd.littleEndian = (rw->flags & rp::RW_LITTLE_ENDIAN) != 0;
        const int cfa = rp::rawInterpCfa(rw->interp);   // one spelling, both ends
        doc->cfa = cfa;
        doc->note += "  -  " + rawRecipeNote(rd, cfa, doc->cfaPattern);
    }
    if (m.frames > 1) doc->note += "  -  " + std::to_string(m.frames) + " frames";
    // ...and it is stack-constant like the rest of this line, which is why the
    // fallback notice goes HERE rather than into a toast: it describes the
    // FILE, and it travels to every sibling frame through RFetchJob::note.
    if (!readNote.empty()) doc->note += "  -  " + readNote;
    doc->uid = app.nextUid++;
    S.remoteUrl = url;
    S.remoteFrame = frame;
    S.remoteStep = step;
    doc->syncMirrors();
    doc->remoteFrames = (int)m.frames;
    // The peer's own answer for this file, kept whether or not the tile above
    // was decimated: w/h come from META (step never touches them) and the
    // channel count from the tile that just landed, which is the number the
    // full-resolution swap will also carry. A folder stack reads its reference
    // shape off this - see attachRemoteStack.
    doc->remoteFullW = (int)m.w; doc->remoteFullH = (int)m.h; doc->remoteFullCh = tch;
    doc->preview = asPreview;
    // --cfa applies to a remote 1ch frame exactly as it does to a local one:
    // openRemote never goes through addImage, so it had to be repeated here or
    // a mosaiced sensor dump arrived over ssh as plain gray
    if (app.forceCfa >= 0 && doc->ch == 1) {
        doc->cfa = app.forceCfa;
        doc->cfaPattern = app.forceCfaPattern & 3;
    }
    // remote opens never went through addImage, so the batch is assigned here:
    // previews sit in their own "preview" pseudo-batch until promoted
    if (asPreview) doc->batchId = batchReuse("preview");
    else if (app.loadBatchId) doc->batchId = app.loadBatchId;
    else {
        size_t sl = rpath.find_last_of('/');
        std::string dir = sl == std::string::npos ? std::string() : rpath.substr(0, sl);
        size_t s2 = dir.find_last_of('/');
        std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
        doc->batchId = batchReuse(leaf.empty() ? host : leaf, host + ":" + dir);
    }
    phase.lap("build the document");
    computeMinMax(*doc);
    phase.lap("computeMinMax");
    // §6.2: a full-resolution open (step == 1) of an already-resident url+frame
    // shares that source. A PREVIEW never registers OR adopts, whatever its
    // step - it is a throwaway the next Browse click replaces, so it must not
    // become anyone's counterpart in share marks or survival notices, and the
    // in-place preview->full swap relies on ex-preview sources being
    // sole-owned. Promotion runs this again for the doc that stays.
    // defaultRange AFTER the possible adoption: an ssh:// resident can hold an
    // OLDER fetch than the bytes this open just measured, and black/white must
    // describe the pixels the doc will actually show (the adoption resyncs the
    // mirrors; computeMinMax above measured the private decode BEFORE it could
    // be registered, per the no-unmeasured-source rule).
    if (!asPreview) shareOrRegisterSource(*doc);
    defaultRange(*doc);
    doc->texDirty = true;
    app.images.push_back(std::move(doc));
    app.imagesRev++;
    selectImage((int)app.images.size() - 1);
    app.fitRequested = true;
    phase.lap("register + select");
    if (asPreview) {
        app.previewUid = app.images.back()->uid;
        // A frame axis makes the file previewable as a sequence too: the same
        // scrub the browser gives a stack of numbered files, stepping the
        // frame index inside this one file instead of a member list. Without
        // it the frames past the first were unreachable short of opening -
        // and a frame-axis stack has no group row to flatten.
        if (m.frames > 1) {
            app.previewFrames = (int)m.frames;
            app.previewIndex = frame;
            app.previewLabel = baseName(rpath);
        }
    }
    // The full-resolution follow-up. Registered opens always get it. A preview
    // gets it too when the frame is cheap enough to just have (small file or
    // fast link - "problem-free" full fetch, verbatim), but only at LOW
    // priority: an unpromoted preview never delays anything registered.
    if (step > 1) {
        size_t estBytes = (size_t)m.w * m.h * tch * rp::dtypeSize(rp::dtypeFromName(dt.c_str()));
        if (!asPreview) requestFullRemote(app.images.back().get());
        else if (estBytes <= (size_t)48 << 20)
            requestFullRemote(app.images.back().get(), true);
    }
    // A frame axis makes this a stack: prefetch the rest in the background, and
    // they become ordinary local frames as they land - temporal analysis, frame
    // stepping, every analyzer, all unchanged. Processing stays local by design;
    // the remote side only ever ships pixels. A PREVIEW only ever holds ONE
    // frame (the scrub above replaces it in place): the rest of the stack is
    // exactly the buffering promotion defers.
    if (m.frames > 1 && !asPreview) {
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = baseName(rpath) + (member.empty() ? std::string() : ":" + member) +
                  " [remote]";
        si.remoteUrl = url;           // frame-axis: one file, N frames
        si.remoteHost = host;
        si.remotePort = port;
        si.expectedFrames = m.frames;
        // the mosaic travels with the stack, so the server splits the planes
        // the same way the local path does (it had NO writer before, so every
        // server aggregate was plane-blind whatever the Inspector said)
        si.cfaType = app.images.back()->cfa;
        si.cfaPattern = app.images.back()->cfaPattern;
        app.seqs.push_back(si);
        ImageDoc* first = app.images.back().get();
        first->seqId = si.id;
        first->seqIndex = 0;
        // server-side temporal stats fire NOW, regardless of transfer, when the
        // policy allows the server to compute (auto/server for remote data)
        maybeRequestServerTemporal(si.id);
        size_t perFrame = (size_t)m.w * m.h * m.ch * sizeof(float);
        size_t room = seqMemBudget() - std::min(seqMemBudget(), claimedImageBytes());
        int fit = (int)std::min<size_t>((size_t)m.frames - 1, perFrame ? room / perFrame : 0);
        for (int i = 1; i <= fit; i++) {
            App::RFetchJob j;
            j.url = url;
            j.name = baseName(rpath) + " #" + std::to_string(i);
            j.note = first->note;     // one note, every frame: see where it is built
            j.frame = i;
            j.seqId = si.id;
            j.seqIndex = i;
            j.bytes = perFrame;       // committed against the budget until it lands
            j.mtime = urlStat.mtime;  // the open's stat epoch rides to every frame
            j.fsize = urlStat.fsize;
            rfInheritRead(j, first);  // ...and so does the reading it is read under
            rfInheritRecipe(j, first); // ...and the declared geometry, if it has one
            if (!member.empty()) j.name = baseName(rpath) + ":" + member +
                                          " #" + std::to_string(i);
            rfEnqueue(std::move(j));
        }
        if (fit < m.frames - 1) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "memory budget: fetching %d of %d frames\n"
                     "(File > Stack loading > Memory budget)", fit + 1, m.frames);
            app.seqNote = msg;
        }
    }
    // a file on this disk was not fetched "from" anywhere.
    // g_quietLoad: a §3.3 re-read drives this open and says "read as ..." itself
    // when it finishes (pumpReRead), so an "opened X" beside it would be two
    // toasts for one gesture, the second one describing the wrong thing.
    if (!g_quietLoad) {
        const std::string what = baseName(rpath) +
                                 (member.empty() ? std::string() : ":" + member);
        toast(host.empty() ? "opened " + what : "opened " + what + " from " + host);
    }
    phase.lap("stack + toast");
    // What the two round trips above were FOR, so their milliseconds can be read
    // as a rate rather than as a mystery. Printed after the last lap and before
    // the phase lines (the destructor writes those), so this write is inside no
    // measured region - see OpenPhase on why that matters here.
    if (timeOpenPhases())
        fprintf(stderr, "time:   [tile %dx%d %dch step %d = %.2f MB of pixels, "
                        "%.2f MB received]\n",
                tw, th, tch, step, (double)tw * th * tch * 4 / (1 << 20),
                (double)(app.uiSession.bytesReceived() - rxBefore) / (1 << 20));
    return true;
}

// ---- a reader that runs on the PEER (issue #180, docs/features/remote/remote-reader-design.md)
//
// The local door is openWithReader (core/app/session.inc): find the reader,
// start Python here, read the .vstream it writes. This is the same door with
// the middle step on the other machine - and NOTHING ELSE moves, which is the
// point of the shape rather than a happy accident:
//
//   the CHOICE stays here. Only a picker or the memo names a reader (§4.13),
//   and the peer reads neither. What can make code run over there is one
//   product: an explicit RUN this client sent, times a flag the peer's own
//   launcher set.
//   the READER travels. Three texts, carried, never resolved by name on the
//   far side - so the bytes that ran are the bytes the memo names (§3.2).
//   the PIXELS do not. The materialisation stays in the peer's cache and
//   reaches this process through META and TILE, decimated to the screen, in
//   the source dtype. §4.13.1's "the converted result does not cross the
//   wire", implemented.
//   the DOCUMENT is built by the ONE tree builder. vnzBuild with a different
//   fetch (§5.2): the layer, the axes, the unit, the note, the cfa and the
//   range all go through the code a local reader open goes through, so there
//   is no room for one reader to make two different documents.

// " on <host>", or " on the local peer" - the machine a sentence is about.
// local:// is a real peer (its own process, its own python) and saying so is
// more honest than an empty phrase that reads like this machine.
static std::string readerHostWhere(const std::string& host) {
    return host.empty() ? std::string(" on the local peer") : " on " + host;
}

// The whole text of a file, or "" - the reader and the two harness files, as
// they will be written on the peer. Read as BYTES: a reader may be in any
// encoding Python accepts and re-encoding it here would change the file that
// runs from the file the author edits.
static bool readWholeTextFile(const std::string& path, std::string& out) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// A url this door owns. Narrow on purpose: makeRemoteUrl writes exactly these
// two schemes, and remote::parseUrl also accepts the bare `host:path` scp form,
// which a Windows path is one colon away from being mistaken for.
static bool isRemoteUrl(const std::string& s) {
    return s.compare(0, 8, "local://") == 0 || s.compare(0, 6, "ssh://") == 0;
}

// Everything BEFORE the far side is asked anything: which reader, and the three
// texts as bytes. Split out because the RUN itself belongs on a worker thread
// (§5: 300 s must not hold the UI's frame) while this half needs the reader
// search path and the settings, which live on the UI thread.
static std::string remoteReaderPrepare(const std::string& url, const std::string& specIn,
                                       std::string& spec, std::string& host,
                                       std::string& rpath, int& port,
                                       std::string files[3]) {
    port = 0;
    if (!remote::parseUrl(url, host, rpath, &port))
        return "expected ssh://user@host/path or local://path";
    // §4.13.2, unchanged over the link: the reader is located BEFORE anything
    // is started, so "there is no such reader" is a sentence naming every place
    // we looked instead of a traceback from a machine the user cannot see.
    ReaderLookup look = resolveReaderFile(specIn);
    if (look.file.empty()) return look.why;
    spec = look.file + ":" + readerFuncOf(specIn);
    if (!readWholeTextFile(look.file, files[0]))
        return "cannot read the reader: " + dispPath(look.file);
    const std::string script = runAdapterScript();
    if (script.empty())
        return "cannot find tools/import/run_adapter.py - the adapter harness ships "
               "with the viewer and this build cannot see it";
    const std::string vimport =
        (pathFromUtf8(script).parent_path() / "viewer_import.py").u8string();
    if (!readWholeTextFile(vimport, files[1]))
        return "cannot read " + dispPath(vimport) + " - it ships beside run_adapter.py";
    if (!readWholeTextFile(script, files[2]))
        return "cannot read " + dispPath(script);
    // §6's promise, kept on this side too: the command as it will be built over
    // there, before it is asked for. Same shape as the local door's
    // "reader: ..." line, so "what did it actually start?" is answerable after
    // the fact on either machine.
    logMsg("reader (" + (host.empty() ? std::string("local peer") : host) + "): python -u " +
           std::string(rp::READER_RUN_FILES[2]) + " " + std::string(rp::READER_RUN_FILES[0]) +
           ":" + readerFuncOf(spec) + " " + rpath + " --stream");
    return {};
}

// ONE ARRAY of something the peer materialised, as a .npy buffer this process
// can hand to the decoder every other array goes through.
//
// Shared by both things that produce a key (docs/features/remote/remote-reader-design.md
// §10.5): a reader's tree node and a .npz member. What it does is what stage 2
// wrote for the reader and #217 needed unchanged - ask META what the peer makes
// of this array, check that against the shape the DECLARATION says it has (a
// disagreement means the two doors have drifted and the one thing that must not
// follow is pixels under a label), and fetch it decimated to what a screen can
// show, IN THE SOURCE DTYPE, so the very same decoder sees the very same bytes.
// That last part is what makes the bit-identity assertions (R8, R14) something
// the code guarantees rather than something it hopes.
//
// `stepOut` is the decimation it settled on, which the caller needs in order to
// ask for the full resolution later.
// What one node of a landed tree needs in order to be asked for AGAIN - the
// full-resolution swap, a sibling frame, and (protocol 14) its statistics.
//
// TWO facts, in ONE record, written at the single place that knows both: the
// fetch that made the node's first request. It used to be a map of decimation
// steps beside an ASSUMPTION about the address - that the peer's number for a
// node is the tree's own index - and the assumption is true of a reader (whose
// .vstream node numbers ARE the tree's) and false of a container, whose address
// is the ZIP entry the SCAN issued. In tools/testdata's two-stack container the
// two are 2/3 and 14/18; where they happen to collide, a request would come
// back not as a refusal but as another member's pixels under this stack's name.
// §10.5's rule is that the node is "the peer's own address, quoted back - never
// a number computed here", and this is what makes that structural.
struct VnzWire {
    int node = 0;                 // what the PEER called this node's array
    int step = 1;                 // the decimation its first fetch settled on
    int wireRead = NR_NATIVE;     // reading put on later META/TILE/MEASURE wires
    std::string layout;           // local-only pre-v15 refusal evidence
    bool requires15 = false;      // also covers blank-layout narrow Stack
};

static void remoteTreeRefine(const std::string& url, const std::vector<VnzNode>& nodes,
                             int n, const std::string& key, int kind,
                             const std::map<int, VnzWire>& wireOf);   // fwd (below)

// TILE is one logical frame in HWC byte order.  Most arrays declare that same
// order (HW/HWC/FHW/FHWC), so their frame is already one contiguous span in
// the rebuilt C-order .npy and must stay the one-memcpy path this code had
// before typed layouts.  CHW/FCHW with multiple channels put C before H and
// need the sample scatter; C=1 is contiguous. Returning true makes that decision testable
// without a wall-clock threshold.
static bool scatterRemoteTileToDeclaration(
        std::vector<uint8_t>& out, size_t pre, const std::vector<uint8_t>& raw,
        size_t esz, int64_t frame, int tw, int th, int tch,
        int iF, int iH, int iW, int iC, const std::vector<size_t>& stride) {
    const size_t frameElems = (size_t)th * (size_t)tw * (size_t)tch;
    const bool sameOrder =
        stride[(size_t)iW] == (size_t)tch &&
        stride[(size_t)iH] == (size_t)tw * (size_t)tch &&
        (iC < 0 || tch == 1 || stride[(size_t)iC] == 1) &&
        (iF < 0 || stride[(size_t)iF] == frameElems);
    if (sameOrder) {
        const size_t dst = iF >= 0 ? (size_t)frame * stride[(size_t)iF] : 0;
        memcpy(out.data() + pre + dst * esz, raw.data(), raw.size());
        return true;
    }
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
            for (int c = 0; c < tch; c++) {
                size_t dst = (size_t)y * stride[(size_t)iH] +
                             (size_t)x * stride[(size_t)iW];
                if (iF >= 0) dst += (size_t)frame * stride[(size_t)iF];
                if (iC >= 0) dst += (size_t)c * stride[(size_t)iC];
                const size_t src = ((size_t)y * (size_t)tw + (size_t)x) *
                                   (size_t)tch + (size_t)c;
                memcpy(out.data() + pre + dst * esz, raw.data() + src * esz, esz);
            }
    return false;
}

static bool remoteKeyedArray(const std::string& url, const remote::KeyedRef& ref,
                             const std::vector<int64_t>& shape, int logicalRead, int wireRead,
                             std::vector<uint8_t>& out, int& stepOut, std::string& e) {
    remote::Meta m;
    if (!app.uiSession.meta(url, m, e, wireRead, nullptr, &ref)) return false;
    const size_t rank = shape.size();
    int iF = -1, iH = -1, iW = -1, iC = -1;
    if (rank == 2) { iH = 0; iW = 1; }
    else {
        int r = logicalRead == NR_NATIVE ? rp::npyNativeRead(shape) : logicalRead;
        if (!rp::npyReadAxes(rank, r, iF, iH, iW, iC)) {
            e = rp::npyNotThatWayText(shape);
            return false;
        }
    }
    if (iH < 0 || iW < 0) {
        e = rp::npyNotNativeText(shape);
        return false;
    }
    const int64_t wantF = iF >= 0 ? shape[iF] : 1;
    const int64_t wantC = iC >= 0 ? shape[iC] : 1;
    if (m.w != (int)shape[iW] || m.h != (int)shape[iH] ||
        m.frames != (int)wantF || m.ch != (int)wantC) {
        e = "the peer reads " + rp::npyShapeText(shape) + " as " +
            std::to_string(m.frames) + "x" + std::to_string(m.h) + "x" +
            std::to_string(m.w) + "x" + std::to_string(m.ch) + " and this viewer "
            "does not";
        return false;
    }
    // openRemote's first view, to the pixel: about one sample per screen pixel,
    // and the full resolution only when something asks for it.
    int step = std::max(1, (int)ceilf((float)std::max(m.w, m.h) / 1600.0f));
    std::vector<int64_t> dec = shape;
    for (int pass = 0; pass < 2; pass++) {
        dec = shape;
        dec[(size_t)iH] = (m.h + step - 1) / step;
        dec[(size_t)iW] = (m.w + step - 1) / step;
        // A decimated array must still READ the way the full one does. A
        // (F,H,W) whose W decimates to 4 or fewer would come back out of the
        // shape rule as (H,W,C) - the same samples under a different meaning,
        // which is the one thing decimation may never cost. So that case is
        // fetched whole instead.
        if (logicalRead != NR_NATIVE || rank < 3 ||
            rp::npyNativeRead(dec) == rp::npyNativeRead(shape)) break;
        step = 1;
    }
    stepOut = step;
    const uint32_t dt = rp::dtypeFromName(m.dtype.c_str());
    const char* npyDt = rp::dtypeNpy(dt);
    if (!*npyDt) { e = "the peer returned dtype " + m.dtype; return false; }
    const size_t esz = rp::dtypeSize(dt);
    const size_t per = (size_t)dec[(size_t)iH] * (size_t)dec[(size_t)iW] *
                       (size_t)(iC >= 0 ? dec[(size_t)iC] : 1) * esz;
    size_t elems = 1;
    for (int64_t d : dec) {
        if (d < 1 || (size_t)d > (size_t)-1 / elems) {
            e = "typed array is too large to materialise";
            return false;
        }
        elems *= (size_t)d;
    }
    if (esz && elems > ((size_t)-1 - 256) / esz) {
        e = "typed array is too large to materialise";
        return false;
    }
    const size_t pre = writeNpyHeaderFor(out, npyDt, dec);
    out.resize(pre + elems * esz);
    // C-order strides in the declaration's own order. TILE is HWC per logical
    // frame. Matching declarations (including C=1 CHW/FCHW) retain one memcpy
    // per frame; multi-channel CHW/FCHW is scattered into declaration order.
    std::vector<size_t> stride(rank, 1);
    for (size_t k = rank; k-- > 1; ) stride[k - 1] = stride[k] * (size_t)dec[k];
    for (int64_t f = 0; f < wantF; f++) {
        std::vector<uint8_t> raw;
        int tw = 0, th = 0, tch = 0;
        uint32_t rdt = 0;
        if (!app.uiSession.tileBytes(url, (int)f, 0, 0, m.w, m.h, step, raw,
                                     tw, th, tch, rdt, e, wireRead, nullptr, &ref))
            return false;
        if (rdt != dt || tw != (int)dec[(size_t)iW] ||
            th != (int)dec[(size_t)iH] || tch != (int)wantC || raw.size() != per) {
            e = "the peer returned " + std::to_string(raw.size()) +
                " bytes for frame " + std::to_string(f) + " and this viewer sized " +
                std::to_string(per);
            return false;
        }
        scatterRemoteTileToDeclaration(out, pre, raw, esz, f, tw, th, tch,
                                       iF, iH, iW, iC, stride);
    }
    return true;
}

// Everything AFTER it: the outcome as a sentence, or the documents. On the UI
// thread, because it touches app.images and app.uiSession.
static std::string remoteReaderLand(const std::string& url, const std::string& spec,
                                    const std::string& host,
                                    const remote::ReaderRun& run) {
    const std::string where = readerHostWhere(host);
    app.readerLastSpec = spec;
    // §6's table, in order. Each outcome is a DIFFERENT sentence because each
    // has a different fix, and two of the fixes are on the other machine.
    if (run.outcome != rp::RO_OK) {
        std::string msg;
        switch (run.outcome) {
            case rp::RO_GATE_CLOSED: msg = run.err; break;   // the peer's own words
            case rp::RO_NO_PYTHON:
                msg = readerNoPythonText(where, run.err,
                          "install numpy for it, or set VIEWER_SERVE_PYTHON where "
                          "viewer-serve runs");
                break;
            case rp::RO_NOT_STARTED: msg = readerNotStartedText(where, run.err); break;
            case rp::RO_TIMED_OUT:   msg = readerTimedOutText(where); break;
            case rp::RO_EXITED:
                // VERBATIM (§4.9 / §4.13.0). The traceback's line numbers are
                // this machine's file's, because that file is what ran.
                msg = run.stderrText.empty() ? run.err : run.stderrText;
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
                break;
            case rp::RO_UNREADABLE:  msg = readerUnreadableText(where, run.err); break;
            default:                 msg = run.err.empty() ? "the reader did not run" : run.err;
        }
        app.readerLastOut = msg;
        if (!run.stderrText.empty() && run.outcome != rp::RO_EXITED)
            app.readerLastOut += "\n\n" + run.stderrText;
        return msg;
    }

    // The header, parsed by the parser the cache file goes through. ONE reading
    // of what a reader declared, whichever machine wrote it.
    int ver = 0, n = -1;
    std::vector<VnzNode> nodes;
    std::vector<VnzBlob> blobs;
    std::vector<std::pair<std::string, std::string>> meta;
    std::istringstream hs(run.header);
    std::string herr = vnsParseHeader(hs, ver, nodes, n, blobs, meta);
    if (!herr.empty()) {
        app.readerLastOut = readerUnreadableText(where, herr);
        return herr;
    }
    // §7: a set does not cross the link in v1. Its Refs name paths on the PEER
    // and the roles are resolved against this machine's registry, which is a
    // design question ("what does a Ref mean across a link") and not a missing
    // field. Refused by name rather than half-built.
    for (int i = 0; i < n; i++)
        if (nodes[(size_t)i].layer == "analysisset") {
            const std::string m =
                "the reader returned an analysis set, and a set does not cross the "
                "link yet: its Refs name files" + where + " and the roles are bound "
                "here. Copy the data to this machine and run the reader locally, "
                "which does build the set.";
            app.readerLastOut = m;
            return m;
        }
    if (blobs.empty()) {
        const std::string m = readerUnreadableText(where, "the container declares no pixels");
        app.readerLastOut = m;
        return m;
    }
    // The tiles come through the UI thread's session, not the job thread's -
    // that one has already been closed, and the pixels a document goes on
    // needing belong to the session every other document fetches through.
    {
        std::string h2, p2;
        int port2 = 0;
        remote::parseUrl(url, h2, p2, &port2);
        std::string serr;
        if (!ensureUiSession(h2, serr, port2)) {
            app.readerLastOut = "remote: " + serr;
            return "remote: " + serr;
        }
    }
    // A real v12-v14 peer ignores named layout when it opens the cached blob.
    // This check deliberately follows ensureUiSession: asynchronous RUN uses a
    // different worker Session, and a previously connected UI session may name
    // another host/version. The session that will send META/TILE is now the one
    // whose HELLO we inspect. Decide before vnzBuild creates anything.
    if (app.uiSession.peerVersion() < 15) {
        for (const VnzBlob& b : blobs) {
            if (b.node < 0 || b.node >= n) continue;
            const VnzNode& v = nodes[(size_t)b.node];
            int logicalRead = NR_NATIVE;
            std::string aerr;
            if (!vnzPixelRead(v, b.shape, logicalRead, aerr)) return aerr;
            if (!v.layout.empty() || npyKeyRead(b.shape, logicalRead) != NR_NATIVE) {
                const std::string kind = !v.layout.empty() ? "Reader " + v.layout
                                                           : "Reader Stack/FHW";
                const std::string m = rp::typedAxesTooOldText(
                    app.uiSession.peerVersion(), baseName(url), kind);
                app.readerLastOut = m;
                return m;
            }
        }
    }
    // identity BEFORE the bytes (§6.2). For a local:// url the peer reads this
    // disk, so the tuple is real; ssh:// stays 0/0, exactly as openRemote
    // records it, because a peer's disk cannot be statted from here.
    FrameSource originStat;
    originStat.remoteUrl = url;
    statLocalUrl(originStat);
    originStat.path = url;

    // The fetch, and it is the ONLY thing that differs from a local reader
    // open (remoteKeyedArray, above).
    std::map<int, VnzWire> wireOf;
    auto fetch = [&](int i, const VnzNode& v, std::vector<uint8_t>& out,
                     std::string& e) -> bool {
        if (v.pixelEntry >= blobs.size()) { e = "that node declared no pixels"; return false; }
        remote::KeyedRef ref{ run.key, i, remote::KeyedRef::FromReader };
        ref.layout = v.layout;
        int logicalRead = NR_NATIVE;
        if (!vnzPixelRead(v, blobs[v.pixelEntry].shape, logicalRead, e)) return false;
        const bool requires15 = !v.layout.empty() ||
                                npyKeyRead(blobs[v.pixelEntry].shape, logicalRead) != NR_NATIVE;
        ref.requires15 = requires15;
        int step = 1;
        if (!remoteKeyedArray(url, ref, blobs[v.pixelEntry].shape,
                              logicalRead, NR_NATIVE, out, step, e)) return false;
        // A reader's address IS the tree index - the .vstream declares its
        // nodes with these very numbers - so the two halves of VnzWire are
        // equal here and differ for a container. Recorded either way, at the
        // one place that made the request, rather than assumed later.
        wireOf[i] = { i, step, NR_NATIVE, v.layout, requires15 };
        return true;
    };

    // Provenance, as metadata beside the frames: §4.3.1's rule is that the
    // viewer CARRIES what it is told and never rewrites it, and "which python,
    // on which machine" is exactly the kind of thing a number gets quoted
    // without later. vnzBuild adds the reader spec itself.
    meta.emplace_back("reader ran on", host.empty() ? std::string("local peer") : host);
    if (!run.provenance.empty()) meta.emplace_back("reader python", run.provenance);
    const std::string berr = vnzBuild(nodes, n, url, spec, fetch, meta, originStat, true);
    if (!berr.empty()) {
        app.readerLastOut = readerUnreadableText(where, berr);
        return berr;
    }
    remoteTreeRefine(url, nodes, n, run.key, remote::KeyedRef::FromReader, wireOf);
    // The memo is keyed on the WHOLE url (§4.12 over a link): readerFor is
    // asked with FrameSource::path, and a remote document's path IS its url, so
    // the Inspector's reader row and a later restore both find this without a
    // second lookup rule.
    rememberReader(url, spec);
    app.readerLastOk = true;
    std::string sum = run.stderrText;
    while (!sum.empty() && (sum.back() == '\n' || sum.back() == '\r')) sum.pop_back();
    app.readerLastOut = sum.empty() ? std::string("the reader wrote no summary") : sum;
    app.readerLastOut += "\n\nran" + where;
    if (!run.provenance.empty()) app.readerLastOut += " with " + run.provenance;
    return {};
}

// A tree the peer materialised lands as ORDINARY LOCAL DOCUMENTS: vnzBuild
// decodes the .npy buffers this process fetched, exactly as it does for a
// container on this disk, which is the whole point of there being one tree
// builder. The cost of that shape is that nothing on the documents says where
// the pixels came from - so the decimated first view (remoteKeyedArray picks
// step = ceil(max(w,h)/1600), the same rule openRemote uses) would have stayed
// decimated for the life of the session, with the Files row saying "1/2" and no
// way to ask for the rest.
//
// This is the wiring that was missing after stage 2 and the natural place for
// it is here, where the node numbers are: the fields openRemote fills on a
// source of its own are filled on these, so requestFullRemote and
// pumpRemoteFetch reach a reader's node and a .npz member through the code that
// already refines every other remote document. Nothing about the fetch is
// new - the full-resolution TILE is the same request with step 1.
static void remoteTreeRefine(const std::string& url, const std::vector<VnzNode>& nodes,
                             int n, const std::string& key, int kind,
                             const std::map<int, VnzWire>& wireOf) {
    std::vector<int> stacks;                     // refined, in declaration order
    for (int i = 0; i < n; i++) {
        const VnzNode& v = nodes[(size_t)i];
        if (v.docFirst < 0 || v.docCount <= 0) continue;
        // No recorded address means no request was ever made for this node's
        // pixels, and there is no number to invent - `i` is the reader's answer
        // and would be a DIFFERENT ARRAY for a container. Left un-refined: the
        // documents keep the pixels they have, and nothing addresses what it
        // cannot name.
        auto it = wireOf.find(i);
        if (it == wireOf.end()) continue;
        const int node = it->second.node;
        const int step = it->second.step;
        for (int k = 0; k < v.docCount; k++) {
            if ((size_t)v.docFirst + (size_t)k >= app.images.size()) break;
            ImageDoc* d = app.images[(size_t)v.docFirst + (size_t)k].get();
            FrameSource& S = *d->src;
            S.remoteUrl = url;
            S.remoteFrame = k;
            S.remoteStep = step;
            S.remoteKey = key;
            S.remoteNode = node;
            S.remoteKeyKind = kind;
            S.materializedIssuer = kind;
            S.remoteProto = app.uiSession.peerVersion();
            // Preserve vnzBuild's logical npyRead: it is identity, session and
            // UI truth. Later peer requests use a separate wire reading. A
            // reader owns its axes in its cached stream header (native wire),
            // while a typed .npz member sends CHW/FCHW explicitly. The named
            // layout rides locally so a new pre-15 Session refuses either one.
            S.remoteKeyRead = it->second.wireRead;
            S.remoteKeyLayout = it->second.layout;
            S.remoteKeyRequires15 = it->second.requires15;
            if (step > 1) requestFullRemote(d);
            // The STACK's own facts, from the node that declared them. vnzBuild
            // puts a container's `__cfa_i` on every ImageDoc and leaves SeqInfo
            // at its defaults, which nothing local ever read - but the wire does
            // (requestServerTemporal sends the STACK's mosaic), so a Bayer stack
            // would have been folded into one plane over there while this side
            // splits it into four. Two answers to one question, which is the
            // one thing a peer may not produce. expectedFrames rides along: the
            // panel prints it while a measurement is in flight.
            if (k == 0 && d->seqId) {
                for (auto& s : app.seqs)
                    if (s.id == d->seqId) {
                        s.cfaType = d->cfa;
                        s.cfaPattern = d->cfaPattern;
                        if (!s.expectedFrames) s.expectedFrames = v.docCount;
                        break;
                    }
                stacks.push_back(d->seqId);
            }
        }
    }
    // ...and now that the documents say where their pixels live and the stacks
    // say what they are, ask the peer for the aggregate - the same call
    // openRemote makes the moment IT mints a remote stack (#180 stage 5).
    //
    // ONE stack, because app.srvTemporal is one slot, and THE ONE THAT IS ON
    // SCREEN: a tree lands several, the last one added is normally the current
    // document, and the panel shows a server row only for the stack it is
    // looking at. Firing for the first node would leave the visible stack
    // computing locally while a measurement of another one sat unread. The
    // others ask for themselves when they are selected
    // (ensureServerTemporalFor, called by the panel).
    if (stacks.empty()) return;
    int want = cur() ? cur()->seqId : 0;
    if (std::find(stacks.begin(), stacks.end(), want) == stacks.end()) want = stacks.front();
    maybeRequestServerTemporal(want);
}

// The whole round trip, synchronously. The door the selftests and a session
// restore use; the PANEL uses the two halves above with the RUN on a thread of
// its own, because 300 s on the UI thread is the exact freeze §5 refuses.
static std::string openRemoteWithReader(const std::string& url, const std::string& specIn) {
    app.readerRan = true;
    app.readerLastOk = false;
    app.readerLastSpec = specIn;
    app.readerLastOut.clear();
    std::string spec, host, rpath, files[3];
    int port = 0;
    std::string perr = remoteReaderPrepare(url, specIn, spec, host, rpath, port, files);
    if (!perr.empty()) { app.readerLastOut = perr; return perr; }
    std::string err;
    if (!ensureUiSession(host, err, port)) return "remote: " + err;
    remote::ReaderRun run;
    if (!app.uiSession.readerRun(rpath, readerFuncOf(spec), files, run, err)) {
        app.readerLastOut = err;
        return err;
    }
    return remoteReaderLand(url, spec, host, run);
}

// ---- a CONTAINER that lives on the peer (issue #217, §10) -------------------
//
// A .npz is a zip of .npy arrays: the file is a batch and each member is a
// stack or a frame (docs/features/adapters/npz-design.md §2). Nothing about that is new here -
// what was missing was a way to ASK a peer what is inside one, and a way to
// address a member once it had. Both exist now and neither is npz-shaped: the
// SCAN returns facts, and a member is reached by the same [key][node] trailer a
// reader's node uses (§10.5).
//
// The three things that do NOT happen on the peer are the point of the shape:
//   it builds no TREE - a viewer container's declaration comes back verbatim
//   and this process's one tree builder reads it (§10.2);
//   it classifies NOTHING - npzClassify turns the facts into picker rows here,
//   so a row cannot read differently depending on which end listed the file;
//   it inflates no PIXELS until one is asked for, and then exactly one.
static bool openRemoteNpzMember(const std::string& url, const std::string& key,
                                const NpzMember& m, bool asPreview) {
    remote::KeyedRef ref{ key, (int)m.entry, remote::KeyedRef::FromNpz };
    return openRemote(url, asPreview, 0, 0 /*NR_NATIVE*/, nullptr, &ref, m.name);
}

// The container half: the declaration comes back as reserved members and the
// tree is built HERE, by the function a local .vnz goes through.
static std::string openRemoteViewerNpz(const std::string& url, const remote::NpzScan& sc) {
    int containerVersion = 0;
    const std::string markerErr = nz::containerVersion(sc.members, containerVersion);
    if (!markerErr.empty()) return markerErr;
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < sc.members.size(); i++) byName[sc.members[i].name] = i;
    auto mem = [&](const std::string& name, std::vector<uint8_t>& out,
                   std::string& e) -> bool {
        auto it = byName.find(name);
        if (it == byName.end()) { e = "missing member " + name; return false; }
        const nz::Fact& f = sc.members[it->second];
        if (!f.err.empty()) { e = f.err; return false; }
        // A reserved member the peer did not carry. It says so rather than
        // guessing: the peer's rule for what rides along is nz::wantsValues,
        // and a container whose declaration does not fit that rule is a
        // container this build cannot read - which is a sentence, not a shrug.
        if (!f.whole) {
            e = name + " is too large to carry across the link (a container's "
                       "declaration is words and short vectors; its pixels are "
                       "fetched a tile at a time)";
            return false;
        }
        out = f.bytes;
        return true;
    };
    std::map<int, VnzWire> wireOf;
    auto pixels = [&](int i, const VnzNode& v, std::vector<uint8_t>& out,
                      std::string& e) -> bool {
        if (v.pixelEntry >= sc.members.size()) { e = "that node declared no pixels"; return false; }
        const nz::Fact& f = sc.members[v.pixelEntry];
        NpyHead H;
        if (!npyPeekHeader(f.bytes, H, e)) { e = f.name + ": " + e; return false; }
        // The NODE is the peer's own address for this member, issued with the
        // key and quoted back - never a number computed here (§10.5). It is NOT
        // the tree index `i`: a container's stacks are nodes 2 and 3 of a
        // declaration whose `__pixels_` members are ZIP entries 14 and 18, and
        // every later request for this node - the full-resolution swap, a
        // sibling frame, its statistics - has to quote the same number this one
        // did or it addresses a different array.
        remote::KeyedRef ref{ sc.key, (int)f.entry, remote::KeyedRef::FromNpz };
        ref.layout = v.layout;
        int logicalRead = NR_NATIVE;
        if (!vnzPixelRead(v, H.shape, logicalRead, e)) return false;
        const int wireRead = npyKeyRead(H.shape, logicalRead);
        const bool requires15 = !v.layout.empty() || wireRead != NR_NATIVE;
        ref.requires15 = requires15;
        int step = 1;
        if (!remoteKeyedArray(url, ref, H.shape, logicalRead, wireRead,
                              out, step, e)) return false;
        wireOf[i] = { (int)f.entry, step, wireRead, v.layout, requires15 };
        return true;
    };
    auto heads = [&](int, const VnzNode& v, std::vector<int64_t>& shape,
                     std::string& e) -> bool {
        if (v.pixelEntry >= sc.members.size()) {
            e = "that node declared no pixels";
            return false;
        }
        const nz::Fact& f = sc.members[v.pixelEntry];
        if (!f.err.empty()) { e = f.err; return false; }
        NpyHead H;
        if (!npyPeekHeader(f.bytes, H, e)) { e = f.name + ": " + e; return false; }
        shape = H.shape;
        return true;
    };
    // identity BEFORE the bytes (§6.2). For a local:// url the peer reads this
    // disk, so the tuple is real; ssh:// stays 0/0, exactly as openRemote
    // records it, because a peer's disk cannot be statted from here.
    FrameSource originStat;
    originStat.remoteUrl = url;
    statLocalUrl(originStat);
    originStat.path = url;
    std::vector<VnzNode> nodes;
    int n = 0;
    auto preflight = [&](const std::vector<VnzNode>& parsed, int count) -> std::string {
        for (int i = 0; i < count; i++) {
            const VnzNode& v = parsed[(size_t)i];
            if (!v.hasPixels) continue;
            std::vector<int64_t> shape;
            std::string e;
            if (!heads(i, v, shape, e)) return "__pixels_" + std::to_string(i) + ": " + e;
            int logicalRead = NR_NATIVE;
            if (!vnzPixelRead(v, shape, logicalRead, e))
                return "__pixels_" + std::to_string(i) + ": " + e;
            const int wireRead = npyKeyRead(shape, logicalRead);
            if (app.uiSession.peerVersion() < 15 &&
                (!v.layout.empty() || wireRead != NR_NATIVE)) {
                const std::string kind = !v.layout.empty() ? "container " + v.layout
                                                           : "container Stack/FHW";
                return rp::typedAxesTooOldText(app.uiSession.peerVersion(),
                                               baseName(url), kind);
            }
        }
        return {};
    };
    // vnzBuild fills docFirst/docCount on the nodes, which is what the refine
    // pass below needs - so the vector has to outlive the call, and
    // loadViewerNpz owns it. It is handed back through the two out-parameters
    // rather than rebuilt here, for the reason everything else in this file is
    // shared: a second reader of a declaration is a second tree.
    const std::string err = loadViewerNpz(mem, byName, containerVersion,
                                          std::string(), url, originStat, pixels, heads,
                                          &nodes, &n, preflight);
    if (!err.empty()) return err;
    remoteTreeRefine(url, nodes, n, sc.key, remote::KeyedRef::FromNpz, wireOf);
    return {};
}

// The door. `onlyMember` restores a single array (sessions record which one):
// no picker and no batch of its own, because the session owns both - the same
// rule loadNpz follows, shared rather than restated.
static bool openRemoteNpz(const std::string& url, const std::string& onlyMember,
                          bool reuseExisting) {
    std::string host, rpath;
    int port = 0;
    if (!remote::parseUrl(url, host, rpath, &port)) {
        toast("expected ssh://user@host/path/to/file.npz", true);
        return false;
    }
    std::string err;
    if (!ensureUiSession(host, err, port)) { toast("remote: " + err, true); return false; }
    remote::NpzScan sc;
    if (!app.uiSession.npzScan(rpath, sc, err)) { toast("remote: " + err, true); return false; }
    if (sc.kind == rp::NK_CONTAINER) {
        // A container is ONE unit and rebuilds every doc at once, so a session's
        // second line for the same file adds nothing (loadNpz makes the same
        // call, with the same reason).
        if (!onlyMember.empty() && reuseExisting)
            for (const auto& d : app.images)
                if (d->src->path == url &&
                    !d->src->remoteKey.empty() &&
                    d->src->remoteKeyKind == remote::KeyedRef::FromNpz &&
                    d->src->member.compare(0, 9, "__pixels_") == 0)
                    return true;
        const size_t before = app.images.size();
        const std::string cerr = openRemoteViewerNpz(url, sc);
        if (!cerr.empty()) { toast(baseName(rpath) + ": " + cerr, true); return false; }
        return app.images.size() > before;
    }
    std::vector<NpzMember> mem = npzClassify(sc.members);
    if (mem.empty()) { toast(baseName(rpath) + ": no arrays in npz", true); return false; }
    if (!onlyMember.empty()) {
        for (const auto& m : mem) {
            if (m.name != onlyMember) continue;
            if (m.role != NpzMember::RImage) {
                toast(baseName(rpath) + ": member " + onlyMember + " " + m.shapeText +
                      " is not an image (" + m.why + ")", true);
                return false;
            }
            npzRememberMeta(url, mem);
            return openRemoteNpzMember(url, sc.key, m, false);
        }
        toast(baseName(rpath) + ": no member named " + onlyMember + " in this npz", true);
        return false;
    }
    int images = 0, axes = 0;
    for (const auto& m : mem) if (m.role == NpzMember::RImage) images++;
    for (const auto& m : mem) {
        if (m.role != NpzMember::RAxis || m.vals.empty()) continue;
        for (const auto& im : mem)
            if (im.role == NpzMember::RImage && im.frames > 1 &&
                (size_t)im.frames == m.vals.size()) { axes++; break; }
    }
    if (images == 0) {
        std::string what;
        for (const auto& m : mem)
            what += (what.empty() ? "" : ", ") + m.name + " " + m.shapeText + " " + m.dtype;
        npzRememberMeta(url, mem);
        toast(baseName(rpath) + ": no image-shaped arrays in this npz (" + what + ")", true);
        return false;
    }
    // ONE image and nothing to decide: open it, no dialog. The rule is
    // loadNpz's, word for word, because it is the same rule (§10.3) - a remote
    // .npz that asked a question a local one does not would be the "two answers
    // for one file" defect #148 removed, one container down.
    if (images == 1 && axes == 0) {
        npzRememberMeta(url, mem);
        for (const auto& m : mem) {
            if (m.role != NpzMember::RImage) continue;
            int prevBatch = app.loadBatchId;
            app.loadBatchId = newBatch(uniqueBatchName(baseName(rpath)));
            const bool ok = openRemoteNpzMember(url, sc.key, m, false);
            app.loadBatchId = prevBatch;
            if (!ok) { pruneEmptyBatches(); return false; }
            std::string say = npzSummary(url, mem, { m.name }, m.frames > 1 ? 1 : 0,
                                         m.frames > 1 ? m.frames : 0, m.frames > 1 ? 0 : 1,
                                         "");
            fprintf(stderr, "npz: %s\n", say.c_str());
            toast(say);
            return true;
        }
    }
    // More than one picture, or a 1-D key that could be this stack's axis: ask,
    // in the SAME modal, from the SAME rows. drawNpzPickModal never learns that
    // a listing can come from a peer - npzPickAccept re-scans by name when the
    // user says yes, which is the remote half of "the file could have been
    // rewritten while the dialog stood open".
    if (app.npzPickOpen) {
        app.npzPickQueue.push_back(url);
        toast(baseName(rpath) + ": waiting its turn - choose arrays for " +
              baseName(app.npzPickPath) + " first (" +
              std::to_string(app.npzPickQueue.size()) + " file(s) waiting)");
        return true;
    }
    app.npzPick = std::move(mem);
    app.npzPickPath = url;
    app.npzAxisName[0] = '\0';
    app.npzAxisUnit[0] = '\0';
    app.npzPickOpen = true;
    return true;
}

// The stack AROUND a remote frame that is already open: the SeqInfo, the
// membership, and the background fetch for every other file of the folder.
// Returns the seqId, 0 when there was nothing to make.
//
// Two callers, because there are two ways to arrive with the head frame open.
// openRemoteStack below opens files[0] and calls this. A SESSION RESTORE
// already has its head open when it reads `seqload` - and that head is the
// frame the stack was LEFT on, which is usually not files[0] - so it calls
// this directly rather than opening a second copy of frame 0. One place
// decides what a remote folder stack is; the head's position in the list is a
// parameter, not an assumption.
static int attachRemoteStack(ImageDoc* first, const std::string& host,
                             const std::vector<std::string>& files,
                             const std::string& name, int port, int token) {
    if (!first || files.empty()) return 0;
    int headIdx = 0;
    for (int i = 0; i < (int)files.size(); i++)
        if (makeRemoteUrl(host, files[i], port) == first->src->path) { headIdx = i; break; }
    App::SeqInfo si;
    si.id = app.nextSeqId++;
    // A scan names the stack by its folder ("10lx/frame_#.npy"): seven stacks
    // all called frame_000.npy would be indistinguishable in every panel, and
    // the linearity Auto-levels reads the level from exactly this name.
    si.name = !name.empty() ? name : baseName(files[0]);
    si.remoteHost = host;             // folder stack: one file per frame
    si.remotePort = port;
    si.expectedFrames = (int)files.size();
    // The shape the siblings have to agree with, and the file that sets it -
    // read from the PEER's META rather than from first->w/h, which are the
    // dimensions of whatever tile is on screen (a big frame opens decimated).
    // The frames land in pumpRemoteFetch, and that is where a disagreeing one
    // is refused, exactly as the local loader's worker refuses a disagreeing
    // numbered sibling. Falls back to the resident shape for a head that did
    // not come through openRemote.
    si.refW = first->remoteFullW > 0 ? first->remoteFullW : first->w;
    si.refH = first->remoteFullW > 0 ? first->remoteFullH : first->h;
    si.refCh = first->remoteFullW > 0 ? first->remoteFullCh : first->ch;
    si.refName = baseName(files[headIdx]);
    si.cfaType = first->cfa; si.cfaPattern = first->cfaPattern;   // planes travel too
    for (const auto& f : files) si.remoteFiles.push_back(f);
    // watch-design §12.2 over the link (§16): IS this file list the peer's whole
    // sibling group? Answered by RUNNING the peer's own grouper - the one that
    // produced the row Browse was clicked on - and comparing, exactly as
    // startSequenceLoad answers it for a local folder stack by running
    // findSequenceSiblings. A stack that records no rule can still LOSE a member
    // whose file is gone; it can never be handed back frames it did not open
    // with, which is the direction a wrong answer has to fail in.
    //
    // One LIST, once, at the mint. It cannot be re-derived at Reload time: a
    // folder that gained a file and a subset that excluded two are the same
    // listing from there, and a group row cannot even be narrowed (§13.5).
    recordRemoteRule(si, files, headIdx);
    // where the user is standing, so the next session line is written from the
    // frame the stack was left on (startSequenceLoad records the same fact for
    // a local folder stack; this path never did, so a restored remote stack
    // saved its FIRST frame's state next time round whatever was on screen)
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i].get() == first) { si.lastImageIdx = i; break; }
    app.seqs.push_back(si);
    first->seqId = si.id;
    first->seqIndex = headIdx;
    noteGroupStack(token, si.id);
    maybeRequestServerTemporal(si.id);
    size_t perFrame = (size_t)first->w * first->h * first->ch * sizeof(float);
    if (first->src->remoteStep > 1)               // preview dims: scale the estimate
        perFrame *= (size_t)first->src->remoteStep * first->src->remoteStep;
    size_t room = seqMemBudget() - std::min(seqMemBudget(), claimedImageBytes());
    int fit = (int)std::min<size_t>(files.size() - 1, perFrame ? room / perFrame : 0);
    int queued = 0;
    for (int i = 0; i < (int)files.size() && queued < fit; i++) {
        if (i == headIdx) continue;               // it is already here
        App::RFetchJob j;
        j.url = makeRemoteUrl(host, files[i], port);
        j.name = baseName(files[i]);
        j.note = first->note;         // every file of the folder came from the same
                                      // peer, so they say so identically or not at all
        j.seqId = si.id;
        j.seqIndex = i;
        j.bytes = perFrame;
        {   // per-file baseline: a folder stack's members are separate files,
            // so each job stats ITS file (identity before bytes; 0/0 for ssh://)
            FrameSource fs;
            fs.remoteUrl = j.url;
            statLocalUrl(fs);
            j.mtime = fs.mtime; j.fsize = fs.fsize;
        }
        rfInheritRead(j, first);
        rfInheritRecipe(j, first);    // a headerless folder: the OPEN's one recipe,
                                      // on every member's request (protocol 11)
        rfEnqueue(std::move(j));
        queued++;
    }
    if (queued < (int)files.size() - 1) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "memory budget: fetching %d of %d frames\n"
                 "(File > Stack loading > Memory budget)", queued + 1, (int)files.size());
        app.seqNote = msg;
    }
    return si.id;
}

// ---- A REMOTE FOLDER OF HEADERLESS FILES, OPENED AS ONE STACK --------------
//
// The parked Open lives in App::remoteRawStack (see the note there, which is
// where the slot and its lifetime rule are): a
// headerless folder cannot be opened in one call the way an .npy folder can -
// the recipe may have to be ASKED for, the dialog is modal and answered frames
// later, and the answer belongs to THIS Open and no other (sequence.inc's
// g_folderRecipe/openId discipline, "The recipe belongs to the OPEN that
// answered for it"). So the request waits there while the dialog is up, and the
// dialog's Load resumes it - the same shape rawDlg.forQueue has for a local
// folder queue, kept in its own slot because the two queues are different
// things and a shared one would let one Open's Cancel drop the other's stacks.

// Is every one of `names` in `dir` the same number of bytes? `sizeOut` is that
// number when it is.
//
// WHY THIS QUESTION HAS A FUNCTION. A headerless file does not defend itself:
// the geometry comes from a recipe, so 108 bytes and 110 bytes both decode as
// 8x6 u16 with 12 B skipped and the second one comes back as a PLAUSIBLE
// picture of the wrong bytes. Every door that opens more than one of them under
// ONE recipe therefore has to ask this before it reads anything, and there are
// THREE REMOTE doors:
//
//   a folder opened as a stack        openRemoteStack
//   a member Watch saw appear         decodeJoiningMember, via the drain's
//                                     one listing
//   a SESSION rebuilding a stack      remoteSequenceSiblings
//
// The third was missed twice - once when this feature shipped and once when
// this comment said there were two - and it is the one that matters most,
// because a restore runs with nobody watching it (review P1). Anything that
// adds a fourth adds a row here and a call there.
//
// AND THE LOCAL FOLDER HAS NO SUCH CHECK. Said here, in the function whose
// name is the check, because the sentence above reads like a rule about the
// program and it is a rule about the LINK: core/app/sequence.inc's stack
// worker calls decodeRawFrame once per member with the one recipe and never
// compares byte counts. A headerless member always decodes AT the recipe's
// dimensions, so stackShapeRefusal - the gate behind it that refuses a
// disagreeing sibling - cannot fire on this at all, and `silent`
// (108/112/104 read at offset 0, which needs 96) misreads two of three frames
// on this machine's own disk in silence.
//
// PR #278 does not close it, by ruling: the subject here is the remote door,
// this function is named for it, and the local door has been this way since
// before any of it. What the PR will not do is leave it unsaid - that shape
// ("decided, not built, and silent about it") is the exact defect this PR
// started from. The board carries a row; the design doc names it in §12.1.
//
// HOW IT IS ONE ROUND TRIP. The listing answers per-file for the rows it names
// individually, and per-GROUP for a numbered family - and since protocol 16 a
// group row carries the min and max over its members, which is exactly this
// question and nothing more. One MSG_LIST of the folder, whatever N is.
//
// WHAT THIS REPLACED, AND WHY. The first version of this compared the group's
// SUM against count x headSize and argued that a sum which matched could only
// hide a member that was too small, which the peer would then refuse by name.
// Both halves were wrong. `108 / 110 / 106` sums to exactly 3 x 108, so the sum
// matched with a member 2 bytes over AND one 2 bytes under; and the peer's own
// check is `offset + W*H*ch*elem > size`, which refuses a file too SMALL for
// the recipe - not one that merely disagrees with its siblings. With a recipe
// whose need is under the head's size (8x6 u16 at offset 0 needs 96 of 108),
// `108 / 112 / 104` passed the sum, passed the peer on every member, and
// finished as a stack of three frames two of which were read from the wrong
// offsets with nothing anywhere saying so. min == max is the check; the sum
// never was one.
// ...over a listing ALREADY IN HAND. Split from the fetching form below because
// two of the three doors already hold one: the Watch drain lists the folder once
// per drain and hands the same rows to every member joining from it, and the
// SESSION restore gets its member names out of a group row it just read. A LIST
// per member is a round trip per frame on the UI thread, which is the freeze two
// PRs were spent removing in the last two days; it does not come back here.
static bool remoteRawSameSizeIn(const std::vector<remote::Entry>& es, int peerVersion,
                                const std::string& dir,
                                const std::vector<std::string>& names,
                                uint64_t& sizeOut, std::string& why) {
    std::map<std::string, uint64_t> one;                   // name -> its own size
    std::map<std::string, const remote::Entry*> inGroup;   // name -> the group row holding it
    for (const remote::Entry& e : es) {
        if (e.dir) continue;
        if (e.group) { for (const std::string& m : e.members) inGroup[m] = &e; }
        else one[e.name] = e.size;
    }
    // The extremes over everything the listing told us about the names asked
    // for. Two sources, both exact: a single row IS its own size, and a group
    // row carries the min and max over its members.
    bool have = false;
    uint64_t lo = 0, hi = 0;
    std::string loName, hiName;                            // "" = a group's extreme
    auto note = [&](uint64_t v, const std::string& nm) {
        if (!have || v < lo) { lo = v; loName = nm; }
        if (!have || v > hi) { hi = v; hiName = nm; }
        have = true;
    };
    std::set<const remote::Entry*> seen;
    for (const std::string& f : names) {
        const std::string nm = baseName(f);
        auto o = one.find(nm);
        if (o != one.end()) { note(o->second, nm); continue; }
        auto g = inGroup.find(nm);
        if (g == inGroup.end()) {
            why = nm + " is not in the peer's listing of " + dir +
                  " any more\n  re-open the folder from Browse";
            return false;
        }
        const remote::Entry& grp = *g->second;
        // A pre-16 peer folds the family into a row whose only size is the SUM,
        // and the sum cannot answer this (see the note above). Refused BY NAME
        // and BY NUMBER rather than approximated: "probably all the same" is
        // how the bytes of one file get read as another's.
        if (!grp.hasMemberSizes) {
            // `peerVersion` here is the EFFECTIVE number - the smaller of what
            // the peer announced and what this client announced - so quoting it
            // as "this peer speaks" was a sentence that contradicted itself
            // whenever the two differed (review P3-7). What is true either way
            // is that the two ends, TOGETHER, are speaking a protocol that does
            // not carry the field.
            why = grp.name + ": this link is running protocol " +
                  std::to_string(peerVersion) + ", which does not report the size of "
                  "each member, so there is no way to check that one recipe fits them "
                  "all - and a headerless file has no header to disagree with"
                  "\n  open them one at a time (each asks for its own geometry), or "
                  "update viewer-serve (protocol 16 reports it)";
            return false;
        }
        if (!seen.insert(g->second).second) continue;
        note(grp.minMemberSize, std::string());
        note(grp.maxMemberSize, std::string());
    }
    if (!have) {
        why = "the peer's listing of " + dir + " named none of these files"
              "\n  re-open the folder from Browse";
        return false;
    }
    if (lo != hi) {
        // WHOSE numbers. A single row names itself; a group row carries only the
        // two extremes, so what can be named there is the FOLDER they are in -
        // "its smallest member" had no antecedent in a sentence that starts
        // "these files" (review P3-13).
        const std::string loWho = loName.empty()
            ? "the smallest file in " + dir : loName;
        const std::string hiWho = hiName.empty()
            ? "the largest file in " + dir : hiName;
        why = "these files are not all one size: " + loWho + " is " + std::to_string(lo) +
              " bytes and " + hiWho + " is " + std::to_string(hi) +
              " bytes - one recipe cannot be right for both, and a headerless file has "
              "no header to disagree with"
              "\n  open them separately (each asks for its own geometry), or leave the "
              "odd one out of the folder";
        return false;
    }
    if (lo == 0) {
        // NAMED when the listing allows it, for the reason the mismatch above
        // is named: a single row IS its own size, so the file with no bytes can
        // be pointed at. A group row carries only its extremes, so there the
        // folder is as far as the truth goes. 0 is also what the peer sends for
        // a row it could not stat (serve.cpp fileSizeOr0), which is why this
        // says "reports 0 bytes" rather than asserting the file is empty.
        why = (loName.empty() ? "a file in " + dir : loName) +
              " reports 0 bytes: there is no geometry a recipe could describe"
              "\n  check it is readable on the peer, then re-open the folder from Browse";
        return false;
    }
    sizeOut = lo;
    return true;
}

// ...and the form that fetches the listing itself, for the door that has none.
static bool remoteRawSameSize(remote::Session& ses, int peerVersion, const std::string& dir,
                              const std::vector<std::string>& names,
                              uint64_t& sizeOut, std::string& why) {
    std::string err;
    std::vector<remote::Entry> es;
    if (!ses.list(dir, es, err)) {
        why = "the peer would not list " + dir + " - " + err +
              "\n  a headerless stack is opened from a byte count, and that listing is "
              "where the count comes from";
        return false;
    }
    return remoteRawSameSizeIn(es, peerVersion, dir, names, sizeOut, why);
}

// The STACK door's form of the question: ensure the session, then ask.
static bool remoteRawStackOneSize(const std::string& host, const std::vector<std::string>& files,
                                  int port, uint64_t& headSize, std::string& why) {
    const std::string dir = peerParentDir(files.front());
    std::string err;
    if (!ensureUiSession(host, err, port)) {
        why = "remote: " + err;
        return false;
    }
    return remoteRawSameSize(app.uiSession, app.uiSession.peerVersion(), dir, files,
                             headSize, why);
}

// Resume: the head opens with `rw`, and attachRemoteStack's jobs inherit the
// same six numbers off the head's own source (rfInheritRecipe). Split out of
// openRemoteStack so the dialog's Load and this session's binding reach the
// SAME code - a second transcription of a stack open is a second place for the
// note, the batch and the memory budget to drift (board row 134's lesson at the
// Browse doors, applied one layer in).
static void openRemoteStackWith(const std::string& host, const std::vector<std::string>& files,
                                const std::string& name, int port, int token,
                                const rp::RawWire* rw) {
    if (files.empty()) return;
    size_t before = app.images.size();
    openRemote(makeRemoteUrl(host, files[0], port), false, 0, NR_NATIVE, rw);
    if (app.images.size() == before) return;      // first frame failed; toasted already
    ImageDoc* first = app.images.back().get();
    if (first->seqId != 0) {
        // A frame-axis file IS the stack already: openRemote built the SeqInfo
        // and, knowing only the url, could name it nothing better than
        // "capture.npy [remote]". The scan knows the FOLDER, so the canon's
        // フォルダ/パターン name is applied here - this early return used to skip
        // the naming below entirely, and a sweep of one capture.npy per level
        // came up as seven stacks with one name.
        if (!name.empty())
            if (App::SeqInfo* si = seqInfo(first->seqId))
                si->name = name;   // origin and count live in columns, not the name (C2)
        noteGroupStack(token, first->seqId);
        return;
    }
    attachRemoteStack(first, host, files, name, port, token);
}

// fwd: needs computeStackStats, defined below. The fold defaults to mean -
// every existing door means "average", and the sum is asked for by name.
static void requestStackAverage(int seqId, int fold = FOLD_MEAN);
// The dialog's Load, for a parked headerless stack. Called from drawRawModal
// after rawDialogAccept has opened the HEAD with these settings and bound the
// byte count - so what is left is the members, which is exactly what the
// forQueue branch beside it does for a local folder.
static void resumeRemoteRawStack() {
    App::PendingRemoteRawStack p = app.remoteRawStack;      // by value: attach may re-enter
    app.remoteRawStack = App::PendingRemoteRawStack();
    if (!p.alive || app.images.empty()) return;
    ImageDoc* first = app.images.back().get();
    // THE BATCH IS PUT ON THE HEAD, which is the only place it can be read
    // from. Setting app.loadBatchId around the call below - what this function
    // used to do - could not work, and the comment that said it did was
    // describing an effect the code does not have: the HEAD was opened by
    // rawDialogAccept before this function was entered (so it was stamped at
    // openRemote's batch line, outside any window here), attachRemoteStack
    // creates no documents at all, and every member frame takes
    // `nd->batchId = ref->batchId` off the head when it lands in
    // pumpRemoteFetch - frames later, with app.loadBatchId long restored.
    // Re-stamping the head is therefore the whole mechanism: the members
    // inherit it, and the Open that asked owns all of them.
    //
    // Only when the asking Open HAD a batch. A Browse double-click outside a
    // queue drain records 0, and 0 means "leave the batch openRemote chose".
    if (p.batchId) first->batchId = p.batchId;
    int seq = 0;
    if (first->seqId != 0) {
        if (!p.name.empty())
            if (App::SeqInfo* si = seqInfo(first->seqId)) si->name = p.name;
        noteGroupStack(p.token, first->seqId);
        seq = first->seqId;
    } else {
        seq = attachRemoteStack(first, p.host, p.files, p.name, p.port, p.token);
    }
    // The parked verb, honoured against the stack that came back. "Open as
    // frame average" that quietly produced "Open as stack" was the other half
    // of the same defect: a verb may be refused by name, never substituted.
    if (p.average) {
        if (seq != 0) requestStackAverage(seq);
        else toast("frame average: that did not open as a stack (a mean needs a time axis)", true);
    }
}

// ...and its Cancel. A folder's worth of stacks dropped in silence is what
// drawRawModal's forQueue branch learned not to do.
static void dropRemoteRawStack() {
    if (!app.remoteRawStack.alive) return;
    const size_t n = app.remoteRawStack.files.size();
    app.remoteRawStack = App::PendingRemoteRawStack();
    toast("cancelled: " + std::to_string(n) + " frame(s) not opened", true);
}

// A remote folder of numbered files, opened as one stack: the first file shows
// immediately, the rest arrive in the background and slot in as ordinary local
// frames. Processing never moves - the pixels do, once each.
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name, int port, int token) {
    if (files.empty()) return;
    // A HEADERLESS folder asks for a geometry, ONCE per Open (11 stage 4), and
    // it did not: this door called openRemote with a null recipe, so the head
    // frame was refused (or, over local://, silently rerouted to the local
    // dialog) and the members were refused one by one (PR #278).
    if (imagefile::isHeaderless(files[0])) {
        // A peer that cannot be TOLD a recipe, said from the number it
        // announced and before anything else - the same check, in the same
        // position, openRemoteRawDeclared makes. Without it this door listed
        // the folder, raised a dialog, collected six fields and threw them
        // away (review #4).
        std::string verr;
        if (!ensureUiSession(host, verr, port)) { toast("remote: " + verr, true); return; }
        if (app.uiSession.peerVersion() < 11) {
            toast(rp::rawTooOldText(app.uiSession.peerVersion(), baseName(files[0])), true);
            return;
        }
        uint64_t sz = 0;
        std::string why;
        if (!remoteRawStackOneSize(host, files, port, sz, why)) {
            toast(why, true);
            return;
        }
        if (const RawRecipe* r = rawRecipeForSize((size_t)sz)) {
            // ...but not a CROPPING one, and it is refused by name rather than
            // applied to half the stack. A crop is finished on this side, on a
            // frame that has landed (applyRecipeCropHere); a folder stack's
            // members land one at a time afterwards, so cropping the head
            // alone would leave it a different shape from every frame behind
            // it - and then the stack's own shape check refuses those frames
            // one by one, N refusals for one decision. Said once, here, with
            // the way out named.
            if (r->cropOn && r->cropW > 0 && r->cropH > 0) {
                toast(baseName(files[0]) + ": the recipe for " + std::to_string(sz) +
                      "-byte files crops, and a cropped recipe cannot open a remote "
                      "folder as ONE stack - the crop would apply to the first frame "
                      "and to none of the others"
                      "\n  open a frame on its own to use it, or clear the crop in the "
                      "recipe panel and open the folder again", true);
                return;
            }
            // #166 over a link, and once for the whole Open: the frames of one
            // stack are one array read N times, so one recipe answers for all
            // of them or the stack is not one thing.
            RawDialog d;
            rawRecipeIntoDialog(*r, d);
            d.path = makeRemoteUrl(host, files[0], port);
            d.fileSize = (size_t)sz;
            const rp::RawWire rw = rawWireFrom(d);
            const size_t before = app.images.size();
            openRemoteStackWith(host, files, name, port, token, &rw);
            if (app.images.size() > before)
                toast(rawRecipeAppliedLine(*r, baseName(files[0]), (size_t)sz));
            return;
        }
        // Nothing bound: ask, and park the Open until the answer arrives. One
        // at a time - pumpRemoteOpenQueue holds the queue while this slot is
        // live, and openRawDialogFor refuses a second question over a live one
        // rather than taking the slot from the Open already waiting.
        // openRawDialogFor decides whether a second question may be raised OVER
        // a live park, and says so by name. It is not a guard in both
        // directions: nothing here stops a park being taken while some OTHER
        // dialog is standing, because the two callers that could do it are
        // already held - pumpRemoteOpenQueue by `!rawDlg.open` at its call site
        // in main.cpp, and a Browse double-click by the modal owning the input.
        if (!openRawDialogFor(makeRemoteUrl(host, files[0], port), sz)) return;
        rawDlg.forRemoteStack = true;
        app.remoteRawStack.alive = true;
        app.remoteRawStack.host = host;
        app.remoteRawStack.files = files;
        app.remoteRawStack.name = name;
        app.remoteRawStack.port = port;
        app.remoteRawStack.token = token;
        // ...and WHICH BATCH the frames belong to. pumpRemoteOpenQueue sets
        // app.loadBatchId around this call and clears it on return, so by the
        // time the dialog is answered - frames later - the id is gone and the
        // stack landed in a batch of its own, named after a different Open
        // (review #6). The Open that asked is the Open that owns them.
        //
        // APPLIED by resumeRemoteRawStack, which stamps it on the HEAD. That
        // is the only place it can be put: the members read their batch off
        // the head when they land, and nothing in the resume path creates a
        // document for app.loadBatchId to reach.
        app.remoteRawStack.batchId = app.loadBatchId;
        return;
    }
    openRemoteStackWith(host, files, name, port, token, nullptr);
}

// The siblings of a remote frame, from the PEER's own listing of its folder:
// findSequenceSiblings' remote twin. It cannot be that function - that one
// walks this filesystem with parent_path/directory_iterator and returns
// nothing at all for an "ssh://..." or "local://..." url - and it must not be
// a second grouping rule either, so it asks the peer, whose grouper is the one
// the Browse panel's group rows already come from (and which shares
// rp::patternWithExtent with the local scan, so a folder opened either way
// produces the same stack name).
//
// Synchronous, on app.uiSession, exactly like the openRemote that just fetched
// this frame's pixels a few lines earlier in the same restore - one LIST after
// a META and a TILE.
static bool remoteSequenceSiblings(const std::string& url,
                                   std::vector<std::string>& filesOut,
                                   std::string& nameOut, std::string& err) {
    std::string host, rpath;
    int port = 0;
    if (!remote::parseUrl(url, host, rpath, &port)) { err = "not a remote url: " + url; return false; }
    size_t sl = rpath.find_last_of('/');
    // A restore's url is absolute or it is not a stack this door can rebuild,
    // so "no separator" stays a refusal here rather than becoming the peer's
    // cwd. The folder itself is peerParentDir's answer, as everywhere else.
    if (sl == std::string::npos) { err = url + ": no folder to list"; return false; }
    const std::string dir = peerParentDir(rpath);
    const std::string base = rpath.substr(sl + 1);
    if (!ensureUiSession(host, err, port)) return false;
    std::vector<remote::Entry> entries;
    if (!app.uiSession.list(dir, entries, err)) return false;
    for (const auto& e : entries) {
        if (!e.group || e.members.size() < 2) continue;
        bool mine = false;
        for (const auto& m : e.members) if (m == base) { mine = true; break; }
        if (!mine) continue;
        // THE THIRD DOOR. A headerless folder may only be opened under ONE
        // recipe when every member is the same number of bytes, and this
        // function is how a SESSION gets back a remote folder stack - so it is
        // one of the doors that has to ask, and it did not. The review's
        // `silent` case (108/112/104 read at offset 0, where the recipe needs
        // 96) went straight through: the sum matched, the peer refused nothing,
        // and the stack came back with the face of a complete one while two of
        // its three frames were read out of files of other lengths. Restore is
        // the WORST place for that, because nobody is watching it happen.
        //
        // Asked from the row already in hand - the listing above is the same
        // one the check wants, so this costs no round trip at all.
        if (imagefile::isHeaderless(base)) {
            uint64_t memberSz = 0;
            std::string sameWhy;
            if (!remoteRawSameSizeIn(entries, app.uiSession.peerVersion(), dir,
                                     e.members, memberSz, sameWhy)) {
                // Said in the RESTORE's own terms. The check's sentence is
                // about a folder; what the operator needs to know here is that
                // a stack their session recorded is not coming back, and why.
                err = stackNameFor(dir, e.name) +
                      ": the frames this stack was saved with cannot be shown to be "
                      "one size, so it is not rebuilt - " + sameWhy;
                return false;
            }
        }
        for (const auto& m : e.members)
            filesOut.push_back(dir == "/" ? "/" + m : dir + "/" + m);   // RbRow::join
        nameOut = stackNameFor(dir, e.name);
        return true;
    }
    err = base + ": " + (host.empty() ? std::string(PEER_HERE) : host) +
          " lists no sibling group for it in " + dir;
    return false;
}

// "Open as frame average" from Browse: open the stack exactly as "Open as
// stack" does, and park the mean until its frames are here. Browse opens are
// asynchronous by design (first frame now, the rest in the background), so
// there is no seqId to hand back at click time and nothing to average yet.
//
// The stack really is OPENED - this is not a way of computing a mean without
// paying for the frames. It cannot be: the mean is over the pixels, so the
// pixels have to arrive. What the user gets is both, which is also what makes
// the result checkable against its own source.
static void openStackForAverage(const std::string& host, const std::vector<std::string>& files,
                                const std::string& name, int port = 0) {
    if (files.empty()) return;
    const int firstNewSeq = app.nextSeqId;
    const size_t before = app.images.size();
    const bool parkWasLive = app.remoteRawStack.alive;
    openRemoteStack(host, files, name, port);
    // THE PARK IS PART OF THIS DOOR'S CONTRACT (App::PendingRemoteRawStack).
    // A headerless remote folder with no bound recipe does not open here at
    // all: openRemoteStack raises the dialog, parks the Open and returns, and
    // the frames exist only after the dialog is answered. Every line below
    // asks "what did that open?", and there is nothing yet - so the verb is
    // handed to the park and requested where the stack is born, instead of
    // being answered now with whatever happens to be on screen.
    //
    // `parkWasLive` is not decoration: openRawDialogFor refuses a SECOND
    // question over a live park, so a park that was already standing when this
    // call began belongs to a different Open. Marking that one `average` would
    // put this click's mean on somebody else's stack - the same wrong-subject
    // mistake one level up.
    if (!parkWasLive && app.remoteRawStack.alive) {
        app.remoteRawStack.average = true;
        return;
    }
    // Nothing was opened and nothing was parked: the open refused and said
    // why, in its own words, which are more specific than anything this
    // function could add. No verb was performed - which is the point: an
    // average that cannot be taken is not quietly turned into a stack.
    if (app.images.size() == before) return;
    int got = 0;
    for (const auto& s : app.seqs)
        if (s.id >= firstNewSeq) { got = s.id; break; }
    // ...or an existing stack was reused: openRemote builds the SeqInfo itself
    // for a frame-axis file, and takes an early return through openRemoteStack.
    // Reached only once something WAS appended by this call, so `back()` is a
    // document this click produced and never a bystander.
    if (!got) got = app.images.back()->seqId;
    if (!got) {
        toast("frame average: that did not open as a stack (a mean needs a time axis)", true);
        return;
    }
    requestStackAverage(got);
}

// A preview becomes a registered open the moment it is USED: double-click,
// Enter, being measured, being made compare-B, or a session save. The user
// never answers "did you mean to keep this?" - their actions already did.
static void promotePreview(ImageDoc* d) {
    if (!d || !d->preview) return;
    d->preview = false;
    if (app.previewUid == d->uid) {
        app.previewUid = 0;
        // the browse scrub belonged to the transient; a registered stack steps
        // frames through the sequence UI instead - a bar left behind here
        // walked previews next to the registered copy, one orphan per step
        app.previewFiles.clear();
        app.previewFrames = 0;
        app.previewIndex = 0;
        app.previewLabel.clear();
    }
    std::string host, rpath;
    int port = 0;
    remote::parseUrl(d->src->remoteUrl, host, rpath, &port);
    {   // out of the "preview" pseudo-batch, into a folder-named one
        size_t sl = rpath.find_last_of('/');
        std::string dir = sl == std::string::npos ? std::string() : rpath.substr(0, sl);
        size_t s2 = dir.find_last_of('/');
        std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
        d->batchId = batchReuse(leaf.empty() ? (host.empty() ? "opened" : host) : leaf,
                                host + ":" + dir);
    }
    // §6.2: promotion is what makes this a REGISTERED open, so this is where
    // an ex-preview source joins the registry (previews themselves never
    // touch it). A source still at step > 1 is refused by srcShareable and
    // registers later, when its full-resolution swap lands. A step-1 source
    // may ADOPT a resident here - and an ssh:// tuple carries no mtime/fsize
    // generation (a local:// one now does, but the stat's granularity is not
    // a proof of identical bytes), so the resident can hold an OLDER fetch of
    // the same url+frame than the preview just displayed. Adoption therefore
    // runs the same invalidation walk as the full-res swap (mirrors are
    // synced by the adoption itself): the screen and every cache re-read
    // through the adopted source instead of keeping the preview generation's
    // numbers.
    if (shareOrRegisterSource(*d)) {
        d->dataRev++;
        d->texDirty = true;
        forgetImage(d);
    }
    // the buffering the preview deferred, at REGISTERED priority now
    requestFullRemote(d);
    if (d->remoteFrames > 1 && d->seqId == 0) {
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = baseName(rpath) +
                  (d->src->member.empty() ? std::string() : ":" + d->src->member) +
                  " [remote]";
        si.remoteUrl = d->src->remoteUrl;
        si.remoteHost = host;
        si.remotePort = port;
        si.expectedFrames = d->remoteFrames;
        si.cfaType = d->cfa; si.cfaPattern = d->cfaPattern;       // planes travel too
        app.seqs.push_back(si);
        d->seqId = si.id;
        d->seqIndex = d->src->remoteFrame;   // the scrub may have parked it mid-stack
        maybeRequestServerTemporal(si.id);
        size_t perFrame = (size_t)d->w * d->h * d->ch * sizeof(float) *
                          (d->src->remoteStep > 1 ? (size_t)d->src->remoteStep * d->src->remoteStep : 1);
        size_t room = seqMemBudget() - std::min(seqMemBudget(), claimedImageBytes());
        int fit = (int)std::min<size_t>((size_t)d->remoteFrames - 1,
                                        perFrame ? room / perFrame : 0);
        for (int i = 0, q = 0; i < d->remoteFrames && q < fit; i++) {
            if (i == d->src->remoteFrame) continue;   // that frame is already resident
            App::RFetchJob j;
            j.url = d->src->remoteUrl;
            j.name = baseName(rpath) + " #" + std::to_string(i);
            j.note = d->note;         // promoting a preview mints the same stack the
                                      // other two paths do, and it says the same thing
            j.seqId = si.id;
            j.seqIndex = i;
            j.frame = i;
            j.bytes = perFrame;
            j.mtime = d->src->mtime;  // the promoted open's stat epoch (see above)
            j.fsize = d->src->fsize;
            rfInheritRead(j, d);
            rfInheritRecipe(j, d);
            rfEnqueue(std::move(j));
            q++;
        }
    }
    app.imagesRev++;
    toast(d->name + ": opened");
}

// ---- the ordinary open, off the UI thread (issue #232 stage 2) --------------
//
// A .npy, a .npz or a picture file used to be read and decoded HERE, inside
// openPath, on the UI thread, for as long as the file took. Measured on this
// repo: 451 ms of a window answering nothing for a 480 MB .npy and 593 ms for a
// stored 480 MB .npz, against 74 ms of actual disk time - the cost is the float
// conversion, not the I/O, and none of it needs this thread.
//
// No new machinery. App::OpenJob is App::ReaderJob's shape (one at a time, a
// queue in front of it, a cancel flag, a start time that IS the progress), the
// split between decode and landing is the one pumpSequence has had since the
// folder loader was written, and the row it draws is the row the reader, the
// stack loader and the video export already draw.
//
// WHAT THE WORKER MAY TOUCH is the whole discipline: the pre-stat, the bytes and
// the decode - every one of them pure, and every one of them already run on a
// thread by the sequence loader. What it may NOT touch is app.images, the source
// registry's publication, the batches or GL. That is the landing, below, and it
// is the same list pumpSequence keeps.
// (g_openStarts / g_openPumps live in state.h beside g_forceAsyncOpen: openRemote
// reads the first one far above this block. See the note there.)
//
// What the Files row says while a job runs: the elapsed seconds and the NAME OF
// THE PHASE, and no percentage. The read has a denominator - the file's size -
// and the decode has none, so one bar carrying both would be measuring two
// things with one number; and a bar that jumps from a true fraction to a made-up
// one is worse than no bar. So the phases are named instead and the seconds are
// the progress, exactly as they are for a reader and for a video export.
static std::string openJobPhase(const App::OpenJob& j) {
    switch (j.phase.load()) {
    case App::OpenJob::PhReading: {
        const uint64_t b = j.bytes.load();
        if (!b) return "reading";
        char n[48];
        if (b >= (uint64_t)1 << 30) snprintf(n, sizeof n, "%.1f GB", (double)b / 1073741824.0);
        else                        snprintf(n, sizeof n, "%.0f MB", (double)b / 1048576.0);
        return std::string("reading ") + n;
    }
    case App::OpenJob::PhDecoding: return "decoding";
    default:                       return "landing";
    }
}

// The landing, and the ONE place the three local doors' sentences live. openPath
// reaches it with nothing pre-decoded (the synchronous door) and pumpOpenJob
// reaches it with a worker's OpenPre, so the wording, the offers and the stack
// question cannot differ by which thread read the file.
static void openLocalLand(const std::string& path, int kind, int npyRead,
                          const std::string& onlyMember, OpenPre* pre) {
    if (kind == OK_Npy) {
        std::string err = loadNpy(path, npyRead, pre);
        // §3.2's third line, attached: the refusal that named the shape is the
        // same place the way out is offered. Nothing runs yet - the picker is a
        // question, and only what the user then chooses is ever executed.
        if (!err.empty()) { toast(baseName(path) + ": " + err, true); openReaderPicker(path, err); }
        else { toast("loaded " + baseName(path)); maybeOfferSequence(app.current); }
        return;
    }
    if (kind == OK_Npz) {
        // loadNpz says what it did (or raises the member picker, which says it
        // after): a blanket "loaded x.npz" over a dialog would be a lie about
        // both what happened and when
        std::string err = loadNpz(path, onlyMember, npyRead, std::string(), std::string(),
                                  true, pre);
        if (!err.empty()) { toast(baseName(path) + ": " + err, true); openReaderPicker(path, err); }
        return;
    }
    // PNG / JPEG / TIFF. The extension only chose this door; what decodes is
    // decided by the bytes behind it. A multi-page TIFF becomes a stack in
    // loadImageFile, so this door opens one file and may gain N documents.
    const size_t before = app.images.size();
    std::string err = loadImageFile(path, onlyMember, pre);
    if (!err.empty()) { toast(baseName(path) + ": " + err, true); openReaderPicker(path, err); }
    // A file whose parts are NAMED, that produced several of them, has already
    // said WHICH ones inside loadImageFile. A blanket "loaded x.exr" on top
    // would be a second and vaguer sentence about the same event. Everything
    // else - one picture, or the pages of a stack - is announced here exactly as
    // it always was.
    else if (app.images.size() - before <= 1 || !imagefile::forPath(path) ||
             !imagefile::forPath(path)->partWord)
        toast("loaded " + baseName(path));
}

// Everything the worker does, and nothing else. Every call in here is pure or is
// already called from the sequence loader's thread for the same reason.
static void openJobWorker(App::OpenJob* j) {
    OpenPre& pre = j->pre;
    // identity BEFORE the bytes (§6.2), and taken on THIS thread before THIS
    // thread's read: the tuple the landing binds its documents to is the stat of
    // the generation of the file whose bytes were actually read.
    pre.stat.path = j->path;
    statSourceFile(pre.stat);
    j->bytes.store(pre.stat.fsize);
    // Between the phases, never inside one (the design's promise: "it stops as
    // soon as the phase it is on is finished"). A cancelled job lands nothing,
    // which pumpOpenJob guarantees structurally rather than by being careful.
    if (j->cancel.load()) { j->done.store(true); return; }
    if (!readFileBytes(j->path, pre.bytes)) {
        j->err = "cannot read file";
        j->done.store(true);
        return;
    }
    if (j->cancel.load()) { j->done.store(true); return; }
    j->phase.store(App::OpenJob::PhDecoding);
    std::string derr;
    if (j->kind == OK_Npy) {
        derr = npyDecodeFrames(pre.bytes, j->path, "", j->npyRead, pre.dec, &j->cancel);
        if (derr.empty()) pre.decoded = true;
    } else if (j->kind == OK_Picture) {
        derr = picDecodeAll(j->path, pre.bytes, pre.stat, j->onlyMember, pre.dec, &j->cancel);
        if (derr.empty()) pre.decoded = true;
    } else if (j->kind == OK_Npz) {
        // The one member this container answers with NO QUESTION ASKED, found
        // with the very predicate the landing will use (npzPlanOf). Everything
        // else - the `__viewer` container reading, a member picker, a refusal -
        // is left to the landing with the bytes already in memory: a question is
        // the UI thread's to ask, and there is nothing to pre-decode for one.
        std::vector<NpzEntry> entries;
        std::string lerr;
        if (!npzList(pre.bytes, entries, lerr)) { j->err = lerr; j->done.store(true); return; }
        NpzPlan plan;
        npzPlanOf(pre.bytes, entries, j->onlyMember, plan);
        for (const auto& m : plan.mem) {
            if (plan.member.empty() || m.name != plan.member) continue;
            std::vector<uint8_t> member;
            std::string mErr;
            // WHICH MEMBER could not be read, in the landing's own spelling
            // (review P2-5). loadNpz's single-image branch answers
            // "<file>:<member>: <why>" and this door reports the worker's error
            // instead of calling loadNpz at all, so the prefix has to be put on
            // here or the member name is simply lost. Only for the branch that
            // wears it: a NAMED member (a session line, a §3.3 re-read) is
            // reported bare by loadNpz, and this must not differ from that
            // either.
            auto blameMember = [&](const std::string& why) {
                return j->onlyMember.empty() ? baseName(j->path) + ":" + m.name + ": " + why
                                             : why;
            };
            if (!npzExtract(pre.bytes, entries[m.entry], member, mErr)) {
                derr = blameMember(mErr);
                break;
            }
            // The reading, and the display name, exactly as npzOpenMember would
            // have asked for them: a named member carries the caller's npyRead
            // (a session line, a §3.3 re-read), the single-image case is native.
            derr = npyDecodeFrames(member, j->path, baseName(j->path) + ":" + m.name,
                                   j->onlyMember.empty() ? 0 /*NR_NATIVE*/ : j->npyRead,
                                   pre.dec, &j->cancel);
            if (derr.empty()) { pre.member = m.name; pre.decoded = true; }
            else if (derr != "stopped") derr = blameMember(derr);
            break;
        }
    }
    // "stopped" is the user's Stop and not the file's fault: it carries no
    // sentence, because nothing will be said about a job nobody is waiting for.
    if (!derr.empty() && derr != "stopped") j->err = derr;
    j->done.store(true);
}

// Start an open without waiting for it. Returns whether the asynchronous door
// took it - false means the caller must do the open itself, here and now.
static bool startOpenJob(const std::string& path, int kind, int npyRead = 0,
                         const std::string& onlyMember = std::string()) {
    if (kind == OK_None) return false;
    // THE SYNCHRONOUS DOOR, and it is wide on purpose. openPath is how nearly
    // every selftest gets a document on screen and a great many of them assert
    // on app.images on the line AFTER it - the same reasoning g_scriptedRun's
    // folder-scan branch is built on, and the same count (#236's 103 sites).
    // --async-open is the lid: a scripted run that asks for the worker and then
    // pumps, so the path a person takes is not the one path the suite never runs.
    if (g_scriptedRun && !g_forceAsyncOpen) return false;
    if (app.openJob) {
        // Already reading one. A drop of five files, or a multi-select, calls
        // openPath in a loop, and a loop that opened five files must still open
        // five: the rest WAIT rather than being refused. Asking for what is
        // already running or already waiting is folded in rather than queued
        // twice - a double-click that landed twice is one open. rdQueue's shape
        // and rdQueue's reason.
        // ...but never into a job that has been STOPPED (review P2-4). A
        // cancelled job will land nothing, so folding the same path into it
        // would answer "this is already opening" about an open that is on its
        // way to opening nothing - and re-dropping the file you just stopped,
        // before its phase finished, would silently do nothing at all.
        if (app.openJob->path == path && app.openJob->onlyMember == onlyMember &&
            !app.openJob->cancel.load())
            return true;
        for (const auto& w : app.openQueue)
            if (w.path == path && w.onlyMember == onlyMember) return true;
        app.openQueue.push_back({ path, kind, npyRead, onlyMember });
        return true;
    }
    g_openStarts++;
    app.openJob.reset(new App::OpenJob());
    App::OpenJob* j = app.openJob.get();
    j->path = path;
    j->kind = kind;
    j->npyRead = npyRead;
    j->onlyMember = onlyMember;
    j->startedAt = nowSec();
    j->th = std::thread(openJobWorker, j);
    return true;
}

// One local file, whichever door is open. The three branches of openPath that
// used to call loadNpy / loadNpz / loadImageFile go through here, so neither the
// worker nor the synchronous fall-back can be reached without the other.
static void openLocalFile(const std::string& path, int kind) {
    if (startOpenJob(path, kind)) return;      // a worker has it; pumpOpenJob lands it
    openLocalLand(path, kind, 0, std::string(), nullptr);
}

static void startNextQueuedOpen() {
    if (app.openJob || app.openQueue.empty()) return;
    const App::OpenWait w = app.openQueue.front();
    app.openQueue.erase(app.openQueue.begin());
    // The fall-back matters: a queue can outlive the flag that filled it, and a
    // queued file that quietly disappeared is the failure the queue exists to
    // prevent in the first place.
    if (!startOpenJob(w.path, w.kind, w.npyRead, w.onlyMember))
        openLocalLand(w.path, w.kind, w.npyRead, w.onlyMember, nullptr);
}

// Called once per frame. Until the worker is done this does NOTHING but count
// itself - there is no live output to tail, because the phase and the clock are
// the whole of what a read has to say. The landing happens here, on the UI
// thread, when the worker has finished.
static void pumpOpenJob() {
    if (!app.openJob) { startNextQueuedOpen(); return; }
    App::OpenJob* j = app.openJob.get();
    if (!j->done.load()) { g_openPumps++; return; }
    j->th.join();
    const bool cancelled = j->cancel.load();
    const std::string path = j->path;
    const int kind = j->kind, npyRead = j->npyRead;
    const std::string onlyMember = j->onlyMember;
    const std::string err = j->err;
    j->phase.store(App::OpenJob::PhLanding);
    // Taken out of App before anything lands. The landing can open the member
    // picker, offer a stack, or reach openPath again through a session recipe,
    // and every one of those asks "is an open running?" - it must see the truth,
    // which is no. The job stays alive in this frame only to own `pre`.
    std::unique_ptr<App::OpenJob> held = std::move(app.openJob);
    if (cancelled) {
        // STOP MEANS NOTHING LANDED, and it is structural rather than promised:
        // landing happens only here and only for a job that was not cancelled,
        // so a stopped open leaves no document, no batch and no source in the
        // registry. There is nothing to undo because nothing was done.
        toast("stopped: " + baseName(path));
        return;
    }
    if (!err.empty()) {
        toast(baseName(path) + ": " + err, true);
        openReaderPicker(path, err);
        return;
    }
    openLocalLand(path, kind, npyRead, onlyMember, &held->pre);
}

// Drive a started open to the end on THIS thread. The frame loop is the only
// other caller of pumpOpenJob, and a run with no frame loop - a selftest, or the
// command line before the window exists - would otherwise leave the job hanging
// and the file unopened. The waiting is the ONLY thing this puts back: the job,
// the cancel flag and the landing are the ones the window drives, so a test that
// calls this is testing the path the user takes. (waitForReader's twin.)
static void waitForOpenJob() {
    while (app.openJob || !app.openQueue.empty()) {
        pumpOpenJob();
        if (app.openJob && !app.openJob->done.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// --zoom / --center, applied when there is something to apply them TO. Called
// straight after parseCli - so every synchronous door (a raw file, a session, a
// scripted run) gets them at once, exactly as before - and then once per frame
// until they land, because the file they are about may be on the worker
// (issue #232 stage 2, review P2-3; App::pendingViewZoom says what went wrong).
static void applyPendingView() {
    if (!app.pendingViewZoom && !app.pendingViewCenter) return;
    // The ZOOM needs no document, so it is applied at once and `viewer --zoom 2`
    // with no file still opens zoomed. The CENTRE and "the explicit view wins
    // over fit-on-load" both need one: addImage raises fitRequested for the
    // first document, and running after that is the whole point of the wait.
    if (app.pendingViewZoom) app.view.zoom = app.pendingZoom;
    if (!cur()) {
        // ...and if nothing is coming either - no job, no queue, no stack
        // loading - then there is nothing left to wait for, and the flags are
        // dropped rather than kept to ambush whatever the user opens by hand.
        if (!app.openJob && app.openQueue.empty() && !app.seqRunning)
            app.pendingViewZoom = app.pendingViewCenter = false;
        return;
    }
    if (app.pendingViewCenter) app.view.center = app.pendingCenter;
    else app.view.center = ImVec2(cur()->w * 0.5f, cur()->h * 0.5f);
    app.fitRequested = false;      // explicit view from CLI wins over fit-on-load
    app.pendingViewZoom = app.pendingViewCenter = false;
}

static void openPath(const std::string& path) {
    if (hasUrlScheme(path)) {
        // A url naming a file opens that file. A url naming a host or a
        // directory means "start here": connect and browse it, the same thing
        // File > Start Remote does - which is what a desktop shortcut to a
        // machine passes, and what a half-remembered path pasted on the command
        // line usually is. startRemote keeps the ssh handshake off this thread.
        //
        // #111: "names a file" is the format table's question, not the peer's.
        // A local://.../shot.exr used to fall to the else and be BROWSED - the
        // listing of a file, which is empty - and openRemote now knows which
        // reader a url on this machine belongs to.
        //
        // ...but §4.12 comes FIRST, ahead of both answers this branch has
        // (#218 review): a url the user chose a reader for reopens through
        // that reader whether or not the format table would have called it
        // readable. Asked here rather than in the block below, because the
        // block below has already been passed by the time a url gets there.
        // See openRemoteMemoFirst.
        {
            bool opened = false;
            if (openRemoteMemoFirst(path, opened)) return;
        }
        if (viewerReadsName(path)) {
            openRemote(path);
        } else {
            App::BrowseInstance& I = rbInstanceFor(path);   // never take an open listing
            startRemote(I, path);
            rbShowInstance(I);
        }
        return;
    }
    // §4.12: a reader chosen for this path earlier is applied again, before the
    // extension is consulted at all - that is the point of remembering, and it
    // is what lets a .dat or a folder open at all. It is a RECORD OF A CHOICE,
    // never a discovery: nothing puts an entry here except the user picking one.
    //
    // STARTED, not waited for (issue #232). This is the ordinary open - the
    // command line, a double-click, every open after the first - and it used to
    // run Python here, on the UI thread, for as long as the user's data took.
    // Measured: 1.65 s of a window answering nothing on a 768 MB file, the worst
    // of every open path in this tool, and up to the five-minute timeout in
    // principle. There is no second machinery for this: startReader is the door
    // the Reader panel's Load has used since #180, and it brings its running
    // timer, its Stop and its live output along - all of which this path had
    // none of, because nothing can be drawn by a thread that is inside Python.
    //
    // The failure still arrives; it arrives from pollReader instead of from
    // here, wearing the same sentence (App::ReaderJob::blame). Nothing FALLS
    // THROUGH to the extension table on a failure, which is what the
    // synchronous form did too: a file the user chose a reader for is that
    // reader's to refuse, and guessing again from the extension afterwards
    // would open something they did not ask for.
    {
        std::string spec = readerFor(path);
        if (!spec.empty()) {
            startReader(path, spec, baseName(path) + " via " + spec + ": ");
            return;
        }
    }
    std::string low = path;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto ends = [&](const char* suf) {
        size_t n = strlen(suf);
        return low.size() >= n && low.compare(low.size() - n, n, suf) == 0;
    };
    // STARTED, not waited for (issue #232 stage 2). The bytes and the decode are
    // a worker's; only the landing is this thread's, and openLocalLand is where
    // both doors say what happened. See openLocalFile.
    if (ends(".npy")) {
        openLocalFile(path, OK_Npy);
    } else if (ends(".npz")) {
        openLocalFile(path, OK_Npz);
    } else if (ends(".vsession")) {
        std::string err = loadSession(path);
        if (!err.empty()) toast(baseName(path) + ": " + err, true);
        else toast("session restored: " + baseName(path));
    } else if (imagefile::forPath(path)) {
        // PNG / JPEG / TIFF. The extension only chooses this door; what decodes
        // is decided by the bytes behind it. A file this build will not read -
        // a format with no decoder, or a TIFF whose compression or layout this
        // reader refuses - refuses HERE, by name and with a reason, instead of
        // falling through to the raw dialog and asking a user who opened a .tif
        // to type in its width. A multi-page TIFF becomes a stack in
        // loadImageFile, so this door opens one file and may gain N documents.
        openLocalFile(path, OK_Picture);
    } else if (std::filesystem::is_directory(pathFromUtf8(path))) {
        openFolder(path);                     // dropping a folder loads every stack below it
    } else if (!imagefile::videoRefusal(path).empty()) {
        // A codec-bearing video container. Refused BY NAME, with the measured
        // reason and the conversion that would be honest - and refused HERE,
        // before the raw dialog, because that dialog would invite the operator
        // to type a width and a height for an H.264 bitstream and would then
        // show them a picture made of entropy-coded bytes.
        //
        // It is not a row in core/imagefile.h's table: see core/y4mread.h. The
        // short of it is that the table decides by BYTES and answers "what
        // would it take to read this", and these are refused by NAME for a
        // reason that no library would change.
        toast(baseName(path) + ": " + imagefile::videoRefusal(path), true);
        openReaderPicker(path, imagefile::videoRefusal(path));
    } else {
        // Headerless. This session's binding first (#166): a byte count the
        // user has already answered for opens with that answer and says which
        // one it was; everything else - the first file of a session included -
        // goes through the dialog exactly as it always did. The stack offer
        // belongs to the branch that actually opened something, the same way
        // the dialog's own Load makes it after a fresh open.
        if (openRawFromSessionRecipe(path)) maybeOfferSequence(app.current);
        else openRawDialogFor(path);
    }
}

// The "Images" filter of File > Open, as one string. Neither half is a literal:
// the pictures come from the format table and our own doors from the arrays at
// the top of sequence.inc, so a format gained or lost on either side cannot
// leave the dialog offering - or HIDING - something this build's other doors
// disagree about. A hidden name is the worse of the two: File > Open silently
// omitted .rggb while Open Folder bundled it, and the operator's way out was to
// know that "All files" existed (verify-matrix G8).
//
// A function so the selftest can read what ships, rather than a second copy of
// the same concatenation asserting against itself.
static std::string openImagesFilter() {
    return ownDoorPattern() + " " + imagefile::dialogPattern();
}

static void openFileDialog() {
    if (!pfd::settings::available()) {   // e.g. Linux without zenity/kdialog
        toast("no file-dialog backend found (install zenity or kdialog) - drag & drop files instead", true);
        return;
    }
    if (app.openDlg) return;             // one dialog at a time
    const std::string pat = openImagesFilter();
    app.openDlg = std::make_unique<pfd::open_file>("Open image / session", "",
        std::vector<std::string>{ "Images (" + pat + ")", pat,
          "Session (*.vsession)", "*.vsession",
          "All files", "*" },
        pfd::opt::multiselect);
}
static void openFolderDialog() {
    if (!pfd::settings::available()) {
        toast("no file-dialog backend found (install zenity or kdialog)", true);
        return;
    }
    if (app.folderDlg) return;
    app.folderDlgMode = 0;
    app.folderDlg = std::make_unique<pfd::select_folder>("Open folder (all stacks below it)");
}
// The dialog's mode-1 action, shared with --localbrowse-selftest: the picked
// folder opens in the Browse panel through the LOCAL peer - the same listing,
// grouping, filters and server stats a remote machine gets, on this disk.
static void browseLocalFolder(std::string p) {
    if (p.empty()) return;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    // Same rule as connecting to a machine: land in the active panel when it
    // has nothing to lose or is already here, and in its own panel otherwise.
    // With one panel open that is exactly the old behaviour.
    App::BrowseInstance& I = rbInstanceFor("local://" + p);
    startRemote(I, "local://" + p);
    rbShowInstance(I);
}
static void browseFolderDialog() {
    if (!pfd::settings::available()) {
        toast("no file-dialog backend found (install zenity or kdialog)", true);
        return;
    }
    if (app.folderDlg) return;
    app.folderDlgMode = 1;
    app.folderDlg = std::make_unique<pfd::select_folder>("Browse folder (local)");
}
static void saveSessionDialog() {
    if (app.images.empty()) { toast("nothing to save - no images loaded", true); return; }
    if (!pfd::settings::available()) { toast("no file-dialog backend found (install zenity or kdialog)", true); return; }
    if (app.saveDlg) return;
    app.saveDlg = std::make_unique<pfd::save_file>("Save session", "session.vsession",
        std::vector<std::string>{ "viewer session (*.vsession)", "*.vsession" });
}
static void openFolder(const std::string& path);   // defined with the sequence code

static void pollFileDialog() {           // called once per frame from the main loop
    if (app.openDlg && app.openDlg->ready(0)) {
        for (const std::string& p : app.openDlg->result()) openPath(p);
        app.openDlg.reset();
    }
    if (app.saveDlg && app.saveDlg->ready(0)) {
        std::string p = app.saveDlg->result();
        if (!p.empty()) saveSession(p);
        app.saveDlg.reset();
    }
    if (app.csvDlg && app.csvDlg->ready(0)) {
        std::string p = app.csvDlg->result();
        if (!p.empty()) {
            if (p.find('.') == std::string::npos) p += ".csv";
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            if (f) { f << app.pendingCsv; toast("curves exported: " + baseName(p)); }
            else toast("cannot write: " + baseName(p), true);
        }
        app.csvDlg.reset();
        app.pendingCsv.clear();
    }
    if (app.texportDlg && app.texportDlg->ready(0)) {
        std::string p = app.texportDlg->result();
        if (!p.empty()) {
            if (p.find('.') == std::string::npos) p += ".csv";
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            if (f) { f << app.pendingTexport; toast("temporal statistics exported: " + baseName(p)); }
            else toast("cannot write: " + baseName(p), true);
        }
        app.texportDlg.reset();
        app.pendingTexport.clear();
    }
    if (app.roiExportDlg && app.roiExportDlg->ready(0)) {
        std::string p = app.roiExportDlg->result();
        if (!p.empty()) {
            if (p.find('.') == std::string::npos) p += ".csv";
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            if (f) { f << app.pendingRoiExport; toast("ROI statistics exported: " + baseName(p)); }
            else toast("cannot write: " + baseName(p), true);
        }
        app.roiExportDlg.reset();
        app.pendingRoiExport.clear();
    }
    if (app.pngDlg && app.pngDlg->ready(0)) {
        std::string p = app.pngDlg->result();
        if (!p.empty()) {
            if (p.size() < 4 || p.substr(p.size() - 4) != ".png") p += ".png";
            saveViewPng(p, app.pngDlgUid);
        }
        app.pngDlg.reset();
        app.pngDlgUid = 0;
    }
    // ...and the same rule one layer up (#253): the dialog was opened on a
    // STACK, so the frames written are that stack's, named by seqId - not
    // whatever became current while the OS dialog was up. The extension is
    // completed rather than corrected: the user may have typed .mkv into an
    // AVI dialog, and export_video.inc reads the extension as the intent.
    if (app.videoDlg && app.videoDlg->ready(0)) {
        std::string p = app.videoDlg->result();
        const int seqId = app.videoDlgSeqId;
        app.videoDlg.reset();
        app.videoDlgSeqId = 0;
        if (!p.empty()) {
            if (videoExtOf(p).empty()) p += videoExtFor(app.videoFormat);
            std::string why = startStackVideoExport(seqId, p, app.videoFps);
            if (!why.empty()) toast(why, true);
        }
    }
    if (app.folderDlg && app.folderDlg->ready(0)) {
        std::string p = app.folderDlg->result();
        app.folderDlg.reset();
        if (!p.empty()) {
            if (app.folderDlgMode == 1) browseLocalFolder(p);   // Browse panel
            else openFolder(p);                                 // load stacks
        }
    }
    if (app.rdOpenDlg && app.rdOpenDlg->ready(0)) {
        std::vector<std::string> sel = app.rdOpenDlg->result();
        int mode = app.rdOpenMode;
        app.rdOpenDlg.reset();
        if (!sel.empty()) {
            std::string why;
            if (mode == 1) openReaderPicker(sel[0], "");
            // §4.13.2: a single FILE is a place too - the ruling said file OR
            // folder, and the odd reader in a scratch directory is the case that
            // has no folder worth registering.
            else if (mode == 2) {
                if (registerReaderPlace(sel[0], &why)) toast("registered: " + dispPath(sel[0]));
                else toast(why, true);
            }
            else snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s", sel[0].c_str());
        }
    }
    if (app.rdFolderDlg && app.rdFolderDlg->ready(0)) {
        std::string d = app.rdFolderDlg->result();
        int mode = app.rdFolderMode;
        app.rdFolderDlg.reset();
        if (!d.empty()) {
            std::string why;
            if (mode == 1) openReaderPicker(d, "");
            else if (registerReaderPlace(d, &why)) toast("registered: " + dispPath(d));
            else toast(why, true);
        }
    }
    if (app.rdNewDlg && app.rdNewDlg->ready(0)) {
        std::string dest = app.rdNewDlg->result();
        app.rdNewDlg.reset();
        newReaderFrom(dest);
    }
}

// (dynamic crop helpers are defined above the session code)
