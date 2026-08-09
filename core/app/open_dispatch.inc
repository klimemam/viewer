// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- open dispatch
// #111: openRemote routes a local:// file the peer cannot serve back to this
// door, and it is defined further down. The two are deliberately not merged -
// openPath is "what does this machine do with a path", openRemote is "what does
// a peer do with a url", and the fix is that the second one now knows when the
// answer is the first one.
static void openPath(const std::string& path);

static void openRawDialogFor(const std::string& path) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) { toast("cannot open: " + baseName(path), true); return; }
    rawDlg.open = true;
    rawDlg.path = path;
    rawDlg.fileSize = (size_t)f.tellg();
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
        "  $T git clone --depth 1 -b binaries " VIEWER_REPO_URL " \"$d/bin\" 2>&1 || { echo \"git clone failed (private repo or no network on this server?)\"; exit 1; }\n"
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

// Does analysis of this stack run on the server? Yes for remote data unless the
// user chose local-fetch. (Single-frame policy nuance lives in the Analysis
// panel; this is the stack-aggregate decision.)
static bool serverComputes(const App::SeqInfo& si) {
    if (si.remoteUrl.empty() && si.remoteFiles.empty()) return false;   // local data
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
    if (!si->remoteFiles.empty()) {
        j.url = makeRemoteUrl(si->remoteHost, si->remoteFiles[0], si->remotePort);
        for (const auto& f : si->remoteFiles) j.files.push_back(f);
    } else {
        j.url = si->remoteUrl;        // already carries its port
    }
    mEnqueue(std::move(j));
}
static void maybeRequestServerTemporal(int seqId) {
    requestServerTemporal(seqId, app.srvTemporal);
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
// caller's notice can. What DOES refuse: no local file behind the pixels
// (remote-backed / montage / plugin output), an unreadable or undecodable file,
// a vanished npz member, and a frame-axis index past the file's new frame count
// (that frame no longer exists - there is no "same frame" to give new values
// to). On any failure the source is untouched: old pixels stay, never a
// half-swap.
//
// A cropped source re-applies its recorded crop rect to the fresh decode (the
// crop is part of what these pixels ARE - §2.1 keeps it on the source); the
// rect is clamped if the file shrank. CFA re-snapping is not attempted: the
// recorded rect was snapped when the crop was made, and the mosaic is a
// per-membership interpretation the source cannot see.
static bool reloadSource(const std::shared_ptr<FrameSource>& src, std::string& errOut,
                         std::string* noteOut = nullptr) {
    if (!src) { errOut = "no source"; return false; }
    // local:// is LOCAL: a Browse-panel local open carries the on-disk path
    // inside its url - the pixels are on this disk and reload reads that file.
    // Only real remote schemes (a peer holds the bytes) refuse.
    const bool localUrl = !src->remoteUrl.empty() &&
                          src->remoteUrl.rfind("local://", 0) == 0;
    if (!src->remoteUrl.empty() && !localUrl) { errOut = "remote-backed"; return false; }
    const std::string fpath = localUrl ? src->remoteUrl.substr(8) : src->path;
    if (fpath.empty()) { errOut = "no file behind these pixels"; return false; }
    // PNG/JPEG stay out of the reload walk for now, BY NAME: these sources do
    // not participate in the registry either (see the integration order in
    // #63), and without this line their bytes would fall through to the npy
    // decoder below and the toast would blame a "decode failure" that never
    // happened. Re-opening the file is the working path today.
    if (const imagefile::Backend* fmt = imagefile::forPath(fpath)) {
        errOut = std::string(fmt->format) + " reload is not wired yet: re-open the file";
        return false;
    }
    // Container/reader members (__pixels_*) rebuild as a UNIT: one __viewer
    // line restores the whole tree, and a reader file re-runs its adapter.
    // A per-member reload is not wired - and without this refusal a reader's
    // .dat origin would fall into the npz branch below, whose npzList would
    // then blame "not a zip file" for a failure that never happened.
    if (src->member.rfind("__pixels_", 0) == 0) {
        errOut = "container/reader members reload by re-opening the file";
        return false;
    }
    std::unique_ptr<ImageDoc> nd;
    std::string err;
    if (src->rawDtype >= 0) {
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
        srcRegistryAddLocked(src, newKey);   // the new tuple's slot. A live
                                      // occupant (the same tuple decoded fresh
                                      // meanwhile) wins: it already serves
                                      // future opens, and both hold identical
                                      // bytes.
    }
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
    for (int sid : seqIds) {
        if (App::SeqInfo* si = seqInfo(sid)) si->stackRev++;   // the temporal key
        // §10.1: reduced frames (frame average / sum) recorded this key at
        // compute time. The bump is the event that makes them describe pixels
        // that no longer exist, so the mark is LATCHED here, at the event -
        // a stack reloaded and then closed cannot take the fact with it. The
        // picture itself never recomputes behind the user: silent recompute
        // is silent overwrite, and Recompute is the user's move.
        for (auto& rd : app.images)
            if (!rd->avgOfPath.empty() && rd->avgSeqId == sid) rd->avgStale = true;
        // a server aggregate naming this stack now describes dead pixels:
        // re-fire it (same shape as seqMosaicChanged above)
        if (app.srvTemporal.seqId == sid) requestServerTemporal(sid, app.srvTemporal);
        if (app.srvTemporalB.seqId == sid) requestServerTemporal(sid, app.srvTemporalB);
        // the linearity rows measured over this stack describe the old pixels:
        // drop them - rows of untouched stacks keep their live numbers - and
        // the fit that included them goes too (closeStack's discipline)
        for (auto it = app.lin.rows.begin(); it != app.lin.rows.end();)
            if (it->seqId == sid) { it = app.lin.rows.erase(it); linHit = true; }
            else ++it;
    }
    if (linHit) linFitStale();
    return true;
}

// Files right-click on a stack header: every member's source, re-read from
// disk. A shared source is reloaded ONCE (the walk already updated every
// membership it has). The summary counts BOTH directions and is never silent:
// failures (with the first error), members whose dimensions changed (the
// single-frame path toasts this - the batch path must not lose the fact), and
// memberships OUTSIDE this stack that the walk refreshed through shared
// sources - numbers moving in a stack the user never touched need a stated
// cause.
static std::string reloadStackFromDisk(int seqId, int* failedOut = nullptr) {
    std::vector<const FrameSource*> done;
    int ok = 0, failed = 0, dims = 0, sharedN = 0;
    std::string firstErr, firstDims;
    std::vector<std::string> sharedNames;  // distinct holders, first one is named
    for (int idx : framesOfSeq(seqId)) {
        const std::shared_ptr<FrameSource>& s = app.images[idx]->src;
        if (std::find(done.begin(), done.end(), s.get()) != done.end()) continue;
        done.push_back(s.get());
        std::string err, note;
        if (reloadSource(s, err, &note)) {
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
    if (failedOut) *failedOut = failed;
    char buf[256];
    snprintf(buf, sizeof buf, "reloaded %d frame(s), %d failed", ok, failed);
    std::string msg = buf;
    if (failed) msg += " - " + firstErr;
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
static bool openRemote(const std::string& url, bool asPreview, int frame) {
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
    if (!peerServesName(rpath)) {
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
        // ssh://, or a name nothing here reads: refused BY NAME and before the
        // round trip, with the reason and the way out. The peer's own answer to
        // this file is "not a .npy file", which blames the file for a limit
        // that belongs to the link.
        toast(baseName(rpath) + ": " +
              (host.empty() ? viewerRefusalFor(rpath) : peerRefusalFor(rpath)), true);
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
    remote::Meta m;
    if (!app.uiSession.meta(rpath, m, err)) { toast("remote: " + err, true); return false; }
    frame = std::clamp(frame, 0, std::max(0, (int)m.frames - 1));

    // First view: the whole frame decimated to something a screen can show. A
    // zoom or a pan asks for a better tile; nothing else on screen costs a byte.
    int step = std::max(1, (int)ceilf(std::max(m.w, m.h) / 1600.0f));
    std::vector<float> px;
    int tw = 0, th = 0, tch = 0;
    std::string dt;
    if (!app.uiSession.tile(rpath, frame, 0, 0, m.w, m.h, step, px, tw, th, tch, dt, err)) {
        toast("remote: " + err, true);
        return false;
    }
    if (tw < 1 || th < 1 || tch < 1 || tch > 4) {   // a 0ch doc would crash later
        toast("remote: peer returned a degenerate frame", true);
        return false;
    }
    auto doc = std::make_unique<ImageDoc>();
    doc->name = baseName(rpath) + (frame > 0 ? " #" + std::to_string(frame) : "") +
                (step > 1 ? "  (1/" + std::to_string(step) + ")" : "");
    FrameSource& S = *doc->src;
    S.mtime = urlStat.mtime;                 // the PRE-read stat: 0/0 for ssh://,
    S.fsize = urlStat.fsize;                 // the disk baseline for local://
    S.path = url;
    S.dtype = dt;
    S.w = tw; S.h = th; S.ch = tch;
    S.data = std::move(px);
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
    if (m.frames > 1) doc->note += "  -  " + std::to_string(m.frames) + " frames";
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
        si.name = baseName(rpath) + " [remote]";
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
    // a file on this disk was not fetched "from" anywhere
    toast(host.empty() ? "opened " + baseName(rpath)
                       : "opened " + baseName(rpath) + " from " + host);
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
        si.name = baseName(rpath) + " [remote]";
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
            rfEnqueue(std::move(j));
            q++;
        }
    }
    app.imagesRev++;
    toast(d->name + ": opened");
}

static void openPath(const std::string& path) {
    if (path.compare(0, 6, "ssh://") == 0 || path.compare(0, 8, "local://") == 0) {
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
        openRawDialogFor(path);
    }
}

static void openFileDialog() {
    if (!pfd::settings::available()) {   // e.g. Linux without zenity/kdialog
        toast("no file-dialog backend found (install zenity or kdialog) - drag & drop files instead", true);
        return;
    }
    if (app.openDlg) return;             // one dialog at a time
    // The picture formats come from the format table rather than from a literal
    // here, so a format gained or lost in core/imagefile.cpp cannot leave the
    // dialog offering (or hiding) something the viewer no longer does.
    const std::string media = imagefile::dialogPattern();
    const std::string pat = "*.npy *.npz *.bin *.raw *.yuv *.dat " + media;
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
    if (app.pngDlg && app.pngDlg->ready(0)) {
        std::string p = app.pngDlg->result();
        if (!p.empty()) {
            if (p.size() < 4 || p.substr(p.size() - 4) != ".png") p += ".png";
            saveViewPng(p);
        }
        app.pngDlg.reset();
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
            if (mode == 1) openReaderPicker(sel[0], "");
            else snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s", sel[0].c_str());
        }
    }
    if (app.rdFolderDlg && app.rdFolderDlg->ready(0)) {
        std::string d = app.rdFolderDlg->result();
        int mode = app.rdFolderMode;
        app.rdFolderDlg.reset();
        if (!d.empty()) {
            if (mode == 1) openReaderPicker(d, "");
            else { snprintf(app.readerLocalDir, sizeof app.readerLocalDir, "%s", d.c_str());
                   savePrefs(); }
        }
    }
    if (app.rdNewDlg && app.rdNewDlg->ready(0)) {
        std::string dest = app.rdNewDlg->result();
        app.rdNewDlg.reset();
        newReaderFrom(dest);
    }
}

// (dynamic crop helpers are defined above the session code)
