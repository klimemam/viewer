// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- open dispatch
// #111: openRemote routes a local:// file the peer cannot serve back to this
// door, and it is defined further down. The two are deliberately not merged -
// openPath is "what does this machine do with a path", openRemote is "what does
// a peer do with a url", and the fix is that the second one now knows when the
// answer is the first one.
static void openPath(const std::string& path);

// `knownSize` > 0: the bytes are not on this disk and the caller already has
// the count (a peer's listing carries it - MSG_LIST has since protocol 3). The
// rest of this function is filename hints and byte-count arithmetic, neither of
// which needs the file open, so ONE dialog serves both disks.
static void openRawDialogFor(const std::string& path, uint64_t knownSize) {
    size_t sz = (size_t)knownSize;
    if (!knownSize) {
        std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
        if (!f) { toast("cannot open: " + baseName(path), true); return; }
        sz = (size_t)f.tellg();
    }
    rawDlg.open = true;
    rawDlg.path = path;
    rawDlg.fileSize = sz;
    rawDlg.replaceIdx = -1;           // fresh open (reinterpret sets this explicitly after)
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

static std::string bootstrapScript() {
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
        "if [ -x \"$d/viewer-serve\" ]; then echo VIEWER_SERVE_OK; exit 0; fi\n"
        "if ! command -v git >/dev/null 2>&1; then\n"
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

// Update an already-installed peer (menu action): same script, forced refresh.
static std::string updateScript() {
    std::string s = bootstrapScript();
    size_t p = s.find("if [ -x \"$d/viewer-serve\" ]");
    if (p != std::string::npos) s.erase(p, s.find('\n', p) + 1 - p);   // skip the early-out
    return s;
}

// Install the peer, best method first. Returns a human-readable log either way.
bool deployPeer(const std::string& host, int port, bool force, std::string& log) {
    std::string out, err;
    // 1. what machine is over there, and is a WORKING peer already in place?
    //    (--help must actually run: a peer that was copied but dies on a glibc
    //    mismatch reported only "connection lost" before this check existed)
    if (!remote::runSshScript(host, port,
            "uname -s; uname -m\n"
            "\"$HOME/.viewer/viewer-serve\" --version 2>&1 || echo NO_PEER\n",
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
                remote::runSshScript(host, port,
                    "\"$HOME/.viewer/viewer-serve\" --version 2>&1 || true\n",
                    vout, verr3, 15.0);
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
    log = "the server installed its own peer";
    return true;
}


// The UI thread's OWN connection to a host, used only by openRemote's meta+tile.
// It deliberately does NOT install the peer: deployPeer is minutes of ssh and
// the browse worker already owns that job (rbWorker's connect path). The most
// this can cost the window is one ssh handshake, bounded by ConnectTimeout=10.
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
// read on the thread that owns it (§3's "one Session, one owning thread"). The
// window does not repaint while it reads, exactly as an open does not - see
// UiThreadStall, and §16.6 for why that is the manual gesture's price and not
// the automatic one's.
using ReloadPeer = std::function<remote::Session*(const std::string& host, int port,
                                                  std::string& errOut)>;
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
// peer's own "stat it and read it again" - META then TILE, on the connection
// the open used - so a remote-backed source reloads, and fails only when the
// link does.
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
static bool reloadSource(const std::shared_ptr<FrameSource>& src, std::string& errOut,
                         std::string* noteOut = nullptr,
                         const ReloadPeer& peer = ReloadPeer()) {
    if (!src) { errOut = "no source"; return false; }
    // local:// is LOCAL: a Browse-panel local open carries the on-disk path
    // inside its url - the pixels are on this disk and reload reads that file,
    // which is strictly more than a peer can tell us about it (§13.7). A real
    // remote scheme goes through the peer's door, the FIRST of the four below.
    const std::string onDisk = localDiskPathOfUrl(src->remoteUrl);
    const bool peerUrl = isPeerUrl(src->remoteUrl);
    const std::string fpath = peerUrl || src->remoteUrl.empty() ? src->path : onDisk;
    if (fpath.empty()) { errOut = "no file behind these pixels"; return false; }
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
    if (src->member.rfind("__pixels_", 0) == 0) {
        errOut = "container/reader members reload by re-opening the file";
        return false;
    }
    std::unique_ptr<ImageDoc> nd;
    std::string err;
    if (peerUrl) {
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
        // the source. A reload is "the same frame, new values" over the link
        // exactly as it is off the disk, and re-deriving any of the three would
        // make it "a different frame, silently".
        //
        // It BLOCKS this thread, as an open of the same frame does (openRemote's
        // UiThreadStall). That is the price of a manual gesture and it is stated
        // where the gesture is; §16.6 is why it is not also the price of an
        // automatic one.
        std::string host, rpath;
        int port = 0;
        if (!remote::parseUrl(src->remoteUrl, host, rpath, &port)) {
            errOut = "not a remote url: " + src->remoteUrl;
            return false;
        }
        remote::Session* ses = reloadPeerFor(peer, host, port, errOut);
        if (!ses) {
            if (errOut.empty()) errOut = "the peer did not answer";
            return false;
        }
        remote::Meta m;
        if (!ses->meta(rpath, m, errOut, src->npyRead)) return false;
        if (src->remoteFrame >= (int)m.frames) {
            // The same sentence and the same reasoning as the .npy frame axis
            // below: there is no "same frame" left to give new values to.
            errOut = "file now has " + std::to_string(m.frames) + " frame(s); frame " +
                     std::to_string(src->remoteFrame) + " no longer exists";
            return false;
        }
        const int step = std::max(1, src->remoteStep);
        std::vector<float> px;
        int tw = 0, th = 0, tch = 0;
        std::string dt;
        rp::F32Loss tloss;
        if (!ses->tile(rpath, src->remoteFrame, 0, 0, m.w, m.h, step,
                       px, tw, th, tch, dt, errOut, src->npyRead, &tloss))
            return false;
        if (tw < 1 || th < 1 || tch < 1 || tch > 4) {
            errOut = "the peer returned a degenerate frame";
            return false;
        }
        nd = std::make_unique<ImageDoc>();
        FrameSource& P = *nd->src;
        P.path = src->path;                    // the url IS the provenance here
        P.remoteUrl = src->remoteUrl;
        P.remoteFrame = src->remoteFrame;
        P.remoteStep = step;
        P.npyShape = m.shape;                  // the PEER's disk, as it declares
                                               // it now (§3.3, the same field the
                                               // local branch takes off the file)
        P.npyRead = src->npyRead;              // ...replayed, never re-chosen
        P.remoteProto = ses->peerVersion();
        P.dtype = dt;
        P.w = tw; P.h = th; P.ch = tch;
        P.data = std::move(px);
        P.f32loss = tloss;                     // measured on THESE samples
        nd->syncMirrors();
        // a crop made after the open re-scoped this frame; the reload keeps that
        // scope, exactly as the two branches below do
        if (src->srcW > 0 &&
            (src->cropX != 0 || src->cropY != 0 || src->w != src->srcW ||
             src->h != src->srcH))
            cropInPlace(*nd, src->cropX, src->cropY, src->w, src->h);
    } else if (src->rawDtype >= 0) {
        RawDialog d;                       // the recipe, replayed against the file
        d.path = fpath;
        d.dtype = src->rawDtype; d.interp = src->rawInterp;
        d.offset = src->rawOffset; d.littleEndian = src->rawLE;
        d.w = src->srcW > 0 ? src->srcW : src->w;
        d.h = src->srcH > 0 ? src->srcH : src->h;
        if (src->cropX != 0 || src->cropY != 0 || src->w != d.w || src->h != d.h) {
            d.cropOn = true;
            d.cropX = src->cropX; d.cropY = src->cropY;
            d.cropW = src->w; d.cropH = src->h;
        }
        nd = decodeRawFrame(d, err);       // stats BEFORE reading, like every loader
    } else if (const imagefile::Backend* fmt = imagefile::forPath(fpath)) {
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
        // The whole file is decoded to recover one picture, which is what the
        // open door already does and says so (loadImageFile) - and what the
        // .npy frame-axis branch below has always done. Reloading a 100-frame
        // y4m frame by frame therefore reads it 100 times; that is the
        // frame-axis cost this walk has carried since it was written, not a
        // new one, and it is per-SOURCE, so reloadStackFromDisk pays it once
        // per distinct source rather than once per membership.
        FrameSource st;                    // identity BEFORE the bytes
        st.path = fpath;
        statSourceFile(st);
        std::vector<uint8_t> bytes;
        if (!readFileBytes(fpath, bytes)) { errOut = "cannot read file"; return false; }
        std::vector<imagefile::Image> imgs;
        if (!imagefile::decode(fpath, bytes, imgs, err)) { errOut = err; return false; }
        // Which picture of the file these pixels ARE - the provenance the open
        // recorded, replayed, never re-chosen. The two kinds are the two the
        // seam already distinguishes (core/imagefile.h): a NAMED part is found
        // by its name, an unnamed one by its position.
        imagefile::Image* pick = nullptr;
        if (!src->member.empty()) {
            for (auto& g : imgs) if (g.member == src->member) { pick = &g; break; }
            if (!pick) {          // ...and a part that is gone is refused BY NAME,
                                  // the way a vanished .npz member is
                errOut = std::string(fmt->partWord ? fmt->partWord : "part") + " \"" +
                         src->member + "\" is no longer in the file";
                return false;
            }
        } else {
            if (src->fileFrame >= (int)imgs.size()) {
                // the same sentence, and the same reasoning, as the .npy frame
                // axis: there is no "same frame" left to give new values to
                errOut = "file now has " + std::to_string(imgs.size()) + " frame(s); frame " +
                         std::to_string(src->fileFrame) + " no longer exists";
                return false;
            }
            pick = &imgs[(size_t)src->fileFrame];
        }
        nd = imageDocFromPicture(*pick, fpath, src->fileFrame, baseName(fpath), st);
        // a crop made after load re-scoped this frame; the reload keeps that
        // scope, exactly as the .npy branch does
        if (nd && src->srcW > 0 &&
            (src->cropX != 0 || src->cropY != 0 || src->w != src->srcW || src->h != src->srcH))
            cropInPlace(*nd, src->cropX, src->cropY, src->w, src->h);
    } else {
        // identity BEFORE the bytes: an overwrite completing during this read
        // binds the OLD tuple to the OLD pixels - the safe direction.
        FrameSource st;
        st.path = fpath;
        statSourceFile(st);                // local:// too: its embedded path IS this
                                           // disk, and the reloaded tuple must move
                                           // with it (an unmoved key would let a
                                           // later open adopt pre-reload pixels)
        std::vector<uint8_t> npy;          // the .npy bytes, plain or extracted
        if (!src->member.empty()) {
            std::vector<uint8_t> zip;
            if (!readFileBytes(fpath, zip)) { errOut = "cannot read file"; return false; }
            std::vector<NpzEntry> entries;
            if (!npzList(zip, entries, err)) { errOut = err; return false; }
            bool found = false;
            for (const auto& e : entries)
                if (e.name == src->member + ".npy") {
                    if (!npzExtract(zip, e, npy, err)) { errOut = err; return false; }
                    found = true;
                    break;
                }
            if (!found) {
                errOut = "array \"" + src->member + "\" is no longer in the file";
                return false;
            }
        } else if (!readFileBytes(fpath, npy)) {
            errOut = "cannot read file";
            return false;
        }
        int frames = 1;
        int64_t fs = 0;
        // re-decode under the RECORDED reading declaration (npyRead, §3.3),
        // never a guess made today: these pixels were born under that
        // declaration, and `frames` below is the file's frame count under it -
        // so the refusal message states a true count. (A declaration the new
        // shape cannot mean falls back to native and SAYS so - npyLayout.)
        nd = decodeNpyBuffer(npy, fpath, "", err, 0, frames, fs, src->npyRead);
        if (nd && src->fileFrame > 0) {    // frame-axis member: decode ITS frame
            if (src->fileFrame >= frames) {
                errOut = "file now has " + std::to_string(frames) + " frame(s); frame " +
                         std::to_string(src->fileFrame) + " no longer exists";
                return false;
            }
            nd = decodeNpyBuffer(npy, fpath, "", err, src->fileFrame, frames, fs,
                                 src->npyRead);
        }
        if (nd) { nd->src->mtime = st.mtime; nd->src->fsize = st.fsize; }
        // a crop made after load re-scoped this frame; the reload keeps that scope
        if (nd && src->srcW > 0 &&
            (src->cropX != 0 || src->cropY != 0 || src->w != src->srcW || src->h != src->srcH))
            cropInPlace(*nd, src->cropX, src->cropY, src->w, src->h);
    }
    if (!nd) { errOut = err.empty() ? "decode failed" : err; return false; }
    computeMinMax(*nd);
    const FrameSource& N = *nd->src;
    if (noteOut && (N.w != src->w || N.h != src->h || N.ch != src->ch || N.dtype != src->dtype)) {
        char b[96];
        snprintf(b, sizeof b, "%dx%d %dch %s -> %dx%d %dch %s", src->w, src->h, src->ch,
                 src->dtype.c_str(), N.w, N.h, N.ch, N.dtype.c_str());
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
        // no thread can see a half-swapped source. nd is PRIVATE here - its
        // cropInPlace above took and released the lock on the fresh decode -
        // so this scope acquires exactly one lock (see the note at g_srcRegMtx).
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
    // and the amber line that said otherwise has to stop being true. The watcher
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
static std::unique_ptr<ImageDoc> decodeJoiningMember(const ImageDoc& ref,
                                                     const std::string& path,
                                                     std::string& errOut,
                                                     const ReloadPeer& peer = ReloadPeer()) {
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
        remote::Meta m;
        if (!ses->meta(rpath, m, errOut, R.npyRead)) return nullptr;
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
                       R.npyRead, &tloss))
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
static std::string reloadStackFromDisk(int seqId, int* failedOut = nullptr,
                                       const ReloadPeer& peer = ReloadPeer()) {
    std::vector<const FrameSource*> done;
    int ok = 0, failed = 0, dims = 0, sharedN = 0;
    std::string firstErr, firstDims;
    std::vector<std::string> sharedNames;  // distinct holders, first one is named
    // ---- §6 step 1-2a: the member list, BEFORE a byte is read ---------------
    // The departures happen first, and that is not an implementation detail: a
    // member whose file is GONE would otherwise reach reloadSource, be refused
    // ("cannot read file"), and be counted as a REFUSAL - which would mark the
    // stack as holding frames of two moments (#56) when what actually happened
    // is that it now holds four frames of one moment. A frame that is gone is
    // not a frame that failed to read, and the difference is the whole of what
    // the mark means.
    const int hadBefore = (int)framesOfSeq(seqId).size();
    watch::Rebuild plan = planStackMembership(seqId, peer);
    if ((int)plan.dropped.size() >= hadBefore) plan = watch::Rebuild{};
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
    for (int idx : framesOfSeq(seqId)) {
        const std::shared_ptr<FrameSource>& s = app.images[idx]->src;
        if (std::find(done.begin(), done.end(), s.get()) != done.end()) continue;
        done.push_back(s.get());
        std::string err, note;
        if (reloadSource(s, err, &note, peer)) {
            ok++;
            if (!note.empty()) {
                dims++;
                if (firstDims.empty())
                    firstDims = (s->path.empty() ? app.images[idx]->name
                                                 : baseName(s->path)) + ": " + note;
            }
            // the walk just refreshed every membership of this source OUTSIDE
            // this stack too (other stacks, loose frames)
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
        } else {
            failed++;
            if (firstErr.empty())
                firstErr = (s->path.empty() ? app.images[idx]->name
                                            : baseName(s->path)) + ": " + err;
        }
    }
    // ---- §6 step 2b-3: the arrivals, then the numbers -----------------------
    // AFTER the re-read, so a joining frame is measured against the reference
    // frame's CURRENT shape rather than the shape it had a moment ago.
    int joinFailed = 0;
    std::string firstJoinErr;
    if (!plan.added.empty()) {
        const std::vector<int> fr = framesOfSeq(seqId);
        const ImageDoc* ref = fr.empty() ? nullptr : app.images[fr.front()].get();
        std::string dir;
        if (const App::SeqInfo* si = seqInfo(seqId)) dir = si->ruleDir;
        for (const std::string& n : plan.added) {
            if (!ref || dir.empty()) break;
            std::string err;
            std::unique_ptr<ImageDoc> nd =
                decodeJoiningMember(*ref, dir + "/" + n, err, peer);
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
    if (failedOut) *failedOut = failed;
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
    // un-says it. The pointer is taken after the last reloadSource: nothing in
    // the walk adds or removes a stack, but seqInfo()'s rule is that the
    // pointer is not held ACROSS anything, so it is not.
    if (App::SeqInfo* si = seqInfo(seqId)) {
        si->reloadFailed = failed;
        si->reloadOk = failed ? ok : 0;
        si->reloadFirstErr = failed ? firstErr : std::string();
        if (failed) {
            char ts[32] = "";
            time_t now = time(nullptr);
            if (struct tm* lt = localtime(&now)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", lt);
            si->reloadWhen = ts;
        } else {
            si->reloadWhen.clear();
        }
    }
    char buf[256];
    snprintf(buf, sizeof buf, "reloaded %d frame(s), %d failed", ok, failed);
    std::string msg = buf;
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
    if (failed)
        msg += " - " + firstErr + " - this stack is marked " +
               SEQ_RELOAD_FAIL_TOKEN + " in Files until every frame re-reads";
    if (dims) {
        snprintf(buf, sizeof buf, " - %d changed dimensions (%s)", dims, firstDims.c_str());
        msg += buf;
    }
    if (sharedN) {
        snprintf(buf, sizeof buf, " - also refreshed %d shared frame(s) in \"%s\"",
                 sharedN, sharedNames.empty() ? "" : sharedNames[0].c_str());
        msg += buf;
        if (sharedNames.size() > 1) {
            snprintf(buf, sizeof buf, " (+%d more)", (int)sharedNames.size() - 1);
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
// A headerless row on a PEER, double-clicked. The panel has the byte count and
// nothing else it needs; every decision below is the viewer's.
//
// The order is #166's, unchanged, because it is the same question asked about a
// different disk: has this SESSION already been told how to read a file of
// exactly this many bytes? If yes it applies and says so; if no it asks. What
// is NOT here is a guess - a headerless file is the one shape that does not
// defend itself (4096x3000 and 3000x4096 are the same byte count), so the only
// thing allowed to apply without being picked is a choice the operator made
// minutes ago and still remembers.
static void openRemoteRawRow(const std::string& url, uint64_t sizeBytes) {
    std::string host, rpath;
    int port = 0;
    remote::parseUrl(url, host, rpath, &port);
    // Before anything else: a peer that cannot carry a declaration. Said from
    // the number it announced, not discovered by sending and being refused -
    // and it has to be said HERE too, or the dialog would collect six fields
    // and then throw them away (rp::rawTooOldText).
    std::string cerr;
    if (!ensureUiSession(host, cerr, port)) { toast("remote: " + cerr, true); return; }
    if (app.uiSession.peerVersion() < 11) {
        toast(rp::rawTooOldText(app.uiSession.peerVersion(), baseName(rpath)), true);
        return;
    }
    if (const RawRecipe* r = rawRecipeForSize((size_t)sizeBytes)) {
        RawDialog d;
        rawRecipeIntoDialog(*r, d);
        d.path = url;
        d.fileSize = (size_t)sizeBytes;
        const rp::RawWire rw = rawWireFrom(d);
        if (openRemote(url, false, 0, 0, &rw))
            toast(rawRecipeAppliedLine(*r, baseName(rpath), (size_t)sizeBytes));
        return;
    }
    openRawDialogFor(url, sizeBytes);
}

// The 4-argument shape core/browse/host.h binds. See the note at that binding.
static bool openRemoteNpz(const std::string& url, const std::string& onlyMember);  // fwd

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
    const size_t before = app.images.size();
    const std::string err = openRemoteWithReader(url, spec);
    if (err.empty()) opened = app.images.size() > before;
    else toast(baseName(url) + " via " + spec + ": " + err, true);
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
    // docs/remote-headerless-design.md). peerServesName is still the predicate
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
            openPath(rpath);              // the File > Open door, which SAYS what
            return app.images.size() > before;   // it did - including refusals
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
    std::string err;
    // app.uiSession, never the browse worker's: two threads framing requests
    // into one ssh stdin desynchronise the stream permanently, and the worker's
    // stop() would free the pipe under this thread's blocking read.
    if (!ensureUiSession(host, err, port)) { toast("remote: " + err, true); return false; }
    // The DECLARED reading (§3.3), and it goes to META as well as to TILE: a
    // reading changes the geometry, so asking META natively and re-deriving
    // w/h/ch/frames here would put the layout rule on both sides of the link
    // with nothing checking they agree - which is what #71 cost a release.
    // Session::meta refuses a declaration a v8 peer cannot serve BEFORE sending.
    remote::Meta m;
    if (!app.uiSession.meta(rpath, m, err, npyRead, rw, kd)) { toast("remote: " + err, true); return false; }
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
    if (tw < 1 || th < 1 || tch < 1 || tch > 4) {   // a 0ch doc would crash later
        toast("remote: peer returned a degenerate frame", true);
        return false;
    }
    auto doc = std::make_unique<ImageDoc>();
    doc->name = baseName(rpath) + (member.empty() ? std::string() : ":" + member) +
                (frame > 0 ? " #" + std::to_string(frame) : "") +
                (step > 1 ? "  (1/" + std::to_string(step) + ")" : "");
    FrameSource& S = *doc->src;
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
    //   M1 (docs/settings-inventory.md §4.1). A headerless read is nothing but
    //   measuring-class settings, so it declares itself in the note - and the
    //   note has to be the SAME sentence the local door writes, or one file
    //   would describe itself two ways depending on which end opened it.
    if (rw) {
        S.rawDtype = (int)rw->dtype;
        S.rawInterp = (int)rw->interp;
        S.rawOffset = (int)rw->offset;
        S.rawLE = (rw->flags & rp::RW_LITTLE_ENDIAN) != 0;
        S.srcW = (int)rw->w; S.srcH = (int)rw->h;
    }
    // The Inspector prints a non-empty note as a LINE OF ITS OWN, directly above
    // the Interpret combo. A note the head frame carries and its siblings do not
    // makes that line appear and vanish as the user steps the stack, and every
    // control under it moves - so the note has to be stack-constant, exactly as
    // derived stacks decided it (materializeDerivedFrame: "the SAME note on every
    // frame"). The sibling frames are minted by pumpRemoteFetch, so this string
    // travels to them through RFetchJob::note.
    // Two things are deliberately NOT in it. The SIZE: the row immediately above
    // already prints "%dx%d %dch %s", and docs/todo-open.md item 11 asked for the
    // duplicate to go. The HOST when there is none: a local:// peer serves files
    // off this disk and was not fetched "from" anywhere, which is the same call
    // the "opened X" toast at the end of this function makes.
    doc->note = "remote";
    if (!host.empty()) doc->note += " " + host;
    // ...and, for a headerless file, the DECLARATION the numbers were read
    // under (M1, docs/settings-inventory.md §4.1). The same sentence the local
    // door writes, from the same helper: a size, a header offset and a byte
    // order each change every number on the screen, and a file that described
    // itself one way from disk and another over a link would be the "two
    // answers for one file" defect one level below where #148 removed it.
    if (rw) {
        RawDialog rd;
        rd.dtype = (int)rw->dtype; rd.interp = (int)rw->interp;
        rd.w = (int)rw->w; rd.h = (int)rw->h; rd.offset = (int)rw->offset;
        rd.littleEndian = (rw->flags & rp::RW_LITTLE_ENDIAN) != 0;
        const int cfa = rd.interp == RI_QUAD ? 2 : rd.interp == RI_BAYER ? 1 : 0;
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
    computeMinMax(*doc);
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
    return true;
}

// ---- a reader that runs on the PEER (issue #180, docs/remote-reader-design.md)
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
// Shared by both things that produce a key (docs/remote-reader-design.md
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
};

static void remoteTreeRefine(const std::string& url, const std::vector<VnzNode>& nodes,
                             int n, const std::string& key, int kind,
                             const std::map<int, VnzWire>& wireOf);   // fwd (below)

static bool remoteKeyedArray(const std::string& url, const remote::KeyedRef& ref,
                             const std::vector<int64_t>& shape,
                             std::vector<uint8_t>& out, int& stepOut, std::string& e) {
    remote::Meta m;
    if (!app.uiSession.meta(url, m, e, 0, nullptr, &ref)) return false;
    const size_t rank = shape.size();
    int iF = -1, iH = -1, iW = -1, iC = -1;
    if (rank == 2) { iH = 0; iW = 1; }
    else if (!rp::npyReadAxes(rank, rp::npyNativeRead(shape), iF, iH, iW, iC)) {
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
        if (rank < 3 || rp::npyNativeRead(dec) == rp::npyNativeRead(shape)) break;
        step = 1;
    }
    stepOut = step;
    const uint32_t dt = rp::dtypeFromName(m.dtype.c_str());
    const char* npyDt = rp::dtypeNpy(dt);
    if (!*npyDt) { e = "the peer returned dtype " + m.dtype; return false; }
    const size_t esz = rp::dtypeSize(dt);
    const size_t per = (size_t)dec[(size_t)iH] * (size_t)dec[(size_t)iW] *
                       (size_t)(iC >= 0 ? dec[(size_t)iC] : 1) * esz;
    const size_t pre = writeNpyHeaderFor(out, npyDt, dec);
    out.resize(pre + per * (size_t)wantF);
    for (int64_t f = 0; f < wantF; f++) {
        std::vector<uint8_t> raw;
        int tw = 0, th = 0, tch = 0;
        uint32_t rdt = 0;
        if (!app.uiSession.tileBytes(url, (int)f, 0, 0, m.w, m.h, step, raw,
                                     tw, th, tch, rdt, e, 0, nullptr, &ref))
            return false;
        if (rdt != dt || raw.size() != per) {
            e = "the peer returned " + std::to_string(raw.size()) +
                " bytes for frame " + std::to_string(f) + " and this viewer sized " +
                std::to_string(per);
            return false;
        }
        memcpy(out.data() + pre + per * (size_t)f, raw.data(), per);
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
        int step = 1;
        if (!remoteKeyedArray(url, ref, blobs[v.pixelEntry].shape, out, step, e)) return false;
        // A reader's address IS the tree index - the .vstream declares its
        // nodes with these very numbers - so the two halves of VnzWire are
        // equal here and differ for a container. Recorded either way, at the
        // one place that made the request, rather than assumed later.
        wireOf[i] = { i, step };
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
            S.remoteProto = app.uiSession.peerVersion();
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
// stack or a frame (docs/npz-design.md §2). Nothing about that is new here -
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
        int step = 1;
        if (!remoteKeyedArray(url, ref, H.shape, out, step, e)) return false;
        wireOf[i] = { (int)f.entry, step };
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
    // vnzBuild fills docFirst/docCount on the nodes, which is what the refine
    // pass below needs - so the vector has to outlive the call, and
    // loadViewerNpz owns it. It is handed back through the two out-parameters
    // rather than rebuilt here, for the reason everything else in this file is
    // shared: a second reader of a declaration is a second tree.
    const std::string err = loadViewerNpz(mem, byName, sc.containerVersion,
                                          std::string(), url, originStat, pixels,
                                          &nodes, &n);
    if (!err.empty()) return err;
    remoteTreeRefine(url, nodes, n, sc.key, remote::KeyedRef::FromNpz, wireOf);
    return {};
}

// The door. `onlyMember` restores a single array (sessions record which one):
// no picker and no batch of its own, because the session owns both - the same
// rule loadNpz follows, shared rather than restated.
static bool openRemoteNpz(const std::string& url, const std::string& onlyMember) {
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
        if (!onlyMember.empty())
            for (const auto& d : app.images)
                if (d->src->path == url && d->src->member.compare(0, 9, "__pixels_") == 0)
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

// A remote folder of numbered .npy files, opened as one stack: the first file
// shows immediately, the rest arrive in the background and slot in as ordinary
// local frames. Processing never moves - the pixels do, once each.
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name, int port, int token) {
    if (files.empty()) return;
    size_t before = app.images.size();
    openRemote(makeRemoteUrl(host, files[0], port));
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
    if (sl == std::string::npos) { err = url + ": no folder to list"; return false; }
    const std::string dir = sl == 0 ? std::string("/") : rpath.substr(0, sl);
    const std::string base = rpath.substr(sl + 1);
    if (!ensureUiSession(host, err, port)) return false;
    std::vector<remote::Entry> entries;
    if (!app.uiSession.list(dir, entries, err)) return false;
    for (const auto& e : entries) {
        if (!e.group || e.members.size() < 2) continue;
        bool mine = false;
        for (const auto& m : e.members) if (m == base) { mine = true; break; }
        if (!mine) continue;
        for (const auto& m : e.members)
            filesOut.push_back(dir == "/" ? "/" + m : dir + "/" + m);   // RbRow::join
        nameOut = stackNameFor(dir, e.name);
        return true;
    }
    err = base + ": " + (host.empty() ? std::string(PEER_HERE) : host) +
          " lists no sibling group for it in " + dir;
    return false;
}

// fwd: needs computeStackStats, defined below. The fold defaults to mean -
// every existing door means "average", and the sum is asked for by name.
static void requestStackAverage(int seqId, int fold = FOLD_MEAN);
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
    openRemoteStack(host, files, name, port);
    int got = 0;
    for (const auto& s : app.seqs)
        if (s.id >= firstNewSeq) { got = s.id; break; }
    // ...or an existing stack was reused: openRemote builds the SeqInfo itself
    // for a frame-axis file, and takes an early return through openRemoteStack.
    if (!got && !app.images.empty()) got = app.images.back()->seqId;
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
            rfEnqueue(std::move(j));
            q++;
        }
    }
    app.imagesRev++;
    toast(d->name + ": opened");
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
    {
        std::string spec = readerFor(path);
        if (!spec.empty()) {
            std::string err = openWithReader(path, spec);
            if (err.empty()) return;
            toast(baseName(path) + " via " + spec + ": " + err, true);
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
    if (ends(".npy")) {
        std::string err = loadNpy(path);
        // §3.2's third line, attached: the refusal that named the shape is the
        // same place the way out is offered. Nothing runs yet - the picker is a
        // question, and only what the user then chooses is ever executed.
        if (!err.empty()) { toast(baseName(path) + ": " + err, true); openReaderPicker(path, err); }
        else { toast("loaded " + baseName(path)); maybeOfferSequence(app.current); }
    } else if (ends(".npz")) {
        // loadNpz says what it did (or raises the member picker, which says it
        // after): a blanket "loaded x.npz" over a dialog would be a lie about
        // both what happened and when
        std::string err = loadNpz(path);
        if (!err.empty()) { toast(baseName(path) + ": " + err, true); openReaderPicker(path, err); }
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
        const size_t before = app.images.size();
        std::string err = loadImageFile(path);
        if (!err.empty()) { toast(baseName(path) + ": " + err, true); openReaderPicker(path, err); }
        // A file whose parts are NAMED, that produced several of them, has
        // already said WHICH ones inside loadImageFile. A blanket "loaded
        // x.exr" on top would be a second and vaguer sentence about the same
        // event. Everything else - one picture, or the pages of a stack - is
        // announced here exactly as it always was.
        else if (app.images.size() - before <= 1 || !imagefile::forPath(path)->partWord)
            toast("loaded " + baseName(path));
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
