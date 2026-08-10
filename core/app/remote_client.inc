// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: remote client
// S3 の前方宣言ブロック(rb/remote 混在)は P7 でほぼ空になった: browse 側の
// 宣言は browse.h、browse が名前で呼ぶ viewer 側ヘルパは host.h が持つ(両方とも
// 背骨がこの断片より前に include する)。残るのは背骨内で完結する static だけ。
// instances.inc もここから出て行った — browse の TU (nav.cpp) が含む。
// ---- remote viewing: declared here, defined further down ----
// REMOTE_HOME sheds static: rbEnqueue (nav.cpp) builds the default peer path
// from it, and a TU-local copy cannot back an extern declaration (host.h).
const char* REMOTE_HOME = "~/.viewer";
// Plain ASCII: the bundled font has no icon set. TWO tags, because there are
// two states. An empty host means the peer runs HERE - the same protocol over a
// pipe instead of ssh - and a browse of this disk that flies an [ssh] flag,
// says "local peer" in the title bar and prints "local://" in a dialog reads as
// a connection to a machine called "local". Nothing is open. Say so.
#define ICON_SSH   "[ssh]"
#define ICON_LOCAL "[local]"
// (PEER_HERE - what an empty host is CALLED on screen - moved to
// core/browse/host.h: nav.cpp's connect toast says it too.)
static const char* peerTag(const std::string& host) {
    return host.empty() ? ICON_LOCAL : ICON_SSH;
}
// non-static: the Browse panel's disconnect tooltip names its host through this
std::string peerLabel(const std::string& host) {
    return host.empty() ? std::string(PEER_HERE) : host;
}
static bool openRemote(const std::string& url, bool asPreview = false, int frame = 0,
                       int npyRead = 0 /*rp::NR_NATIVE*/);
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name = std::string(), int port = 0,
                            int token = 0);
static bool ensureUiSession(const std::string& host, std::string& err, int port = 0);
static std::string bootstrapScript();
static std::string updateScript();

#include "compare.inc"
#include "export.inc"

static void ensureSession(remote::Session& ses, std::string& sesHost, int& sesPort,
                          const std::string& host, int port, const std::string& exe,
                          std::string& errOut) {
    std::string err;
    // startOn, not start(): start() hardwires port 0, i.e. plain
    // `ssh host` - a DIFFERENT machine when the user named a port
    if (!ses.alive() || sesHost != host || sesPort != port) {
        if (!ses.startOn(host, port, exe, err)) {
            errOut = err;
        } else {
            sesHost = host;
            sesPort = port;
        }
    }
}

// ---- background full-resolution fetch (remote frames) -------------------------
// The preview is already on screen; the real pixels arrive here on their own ssh
// connection and are swapped in by the UI thread.
static void rfWorker() {
    remote::Session ses;
    ses.setAbort(&app.rfStop);        // a blocked read gives up when Quit asks
    std::string sesHost = "\n";                  // impossible: force first connect
    int sesPort = -1;
    while (!app.rfStop) {
        App::RFetchJob job;
        {
            std::unique_lock<std::mutex> lk(app.rfMtx);
            app.rfCv.wait(lk, []{ return app.rfStop || !app.rfQueue.empty(); });
            // Stop wins over pending work, as it did when this was a poll: the
            // old `while (!app.rfStop)` was checked BEFORE any dequeue. Asking
            // for the queue to be empty as well lets a worker that was notified
            // with a job and stopped before it could reacquire the mutex run one
            // more fetch INSIDE stopRemoteFetcher's join(), with the UI thread
            // waiting on it. The predicate above already guarantees the queue is
            // non-empty whenever the flag is false, so there is nothing to lose.
            if (app.rfStop) break;
            job = std::move(app.rfQueue.front());
            app.rfQueue.erase(app.rfQueue.begin());
            app.rfBusyUid = job.uid;   // inside the lock: the dedupe reads the
                                       // queue and this uid under the same mtx
        }
        App::RFetchDone d;
        d.uid = job.uid;
        d.url = job.url; d.name = job.name; d.note = job.note;
        d.frame = job.frame; d.seqId = job.seqId; d.seqIndex = job.seqIndex;
        d.gen = job.gen;
        d.bytes = job.bytes;
        d.mtime = job.mtime; d.fsize = job.fsize;
        d.npyRead = job.npyRead; d.remoteProto = job.remoteProto;
        d.npyShape = job.npyShape;
        std::string host, rpath, err;
        int port = 0;
        if (!remote::parseUrl(job.url, host, rpath, &port)) {
            d.err = "bad remote url";
        } else {
            ensureSession(ses, sesHost, sesPort, host, port, job.exe, d.err);
            if (d.err.empty()) {
                int w = 0, h = 0, ch = 0;
                // the server clamps the rect, so "huge" means "the whole frame"
                // ...under the reading the OPEN was made with: a full-resolution
                // swap that fetched the native reading of a document declared
                // (F,H,W) would replace the preview with a different array.
                if (!ses.tile(rpath, job.frame, 0, 0, 1 << 30, 1 << 30, 1,
                              d.data, w, h, ch, d.dtype, err, job.npyRead, &d.f32loss)) {
                    d.err = err;
                } else {
                    d.w = w; d.h = h; d.ch = ch;
                    float mn = FLT_MAX, mx = -FLT_MAX;
                    for (float v : d.data)
                        if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
                    if (mn > mx) { mn = 0; mx = 1; }
                    if (mn == mx) mx = mn + 1;
                    d.vmin = mn; d.vmax = mx;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(app.rfMtx);
            app.rfDone.push_back(std::move(d));
            app.rfBusyUid = 0;         // done is QUEUED before busy clears, so a
                                       // dedupe that misses both still lands
                                       // behind the result it would duplicate
        }
        glfwPostEmptyEvent();                    // wake the UI to swap it in
    }
}

static void rfEnqueue(App::RFetchJob job) {
    job.gen = app.rfGen;
    // how the peer is invoked, frozen NOW: the UI thread edits remoteExe freely
    std::string host, rpath;
    remote::parseUrl(job.url, host, rpath);
    job.exe = app.remoteExe.empty() ? (host.empty() ? app.exePath
                                                : std::string(REMOTE_HOME) + "/viewer-serve")
                               : app.remoteExe;
    {
        // counters BEFORE the push, under the lock: the worker could finish job 1
        // while jobs 2..N are still being enqueued, and the pending==0 reset would
        // restart the progress display mid-stack
        std::lock_guard<std::mutex> lk(app.rfMtx);
        app.rfPending++;
        app.rfTotal++;
        app.rfBytesInFlight += job.bytes;
        // an unpromoted preview is only ALLOWED the link when nothing a user
        // registered is waiting: normal jobs enter ahead of every low one
        if (job.low) {
            app.rfQueue.push_back(std::move(job));
        } else {
            auto it = app.rfQueue.begin();
            while (it != app.rfQueue.end() && !it->low) ++it;
            app.rfQueue.insert(it, std::move(job));
        }
    }
    app.rfCv.notify_one();
    if (!app.rfThread.joinable()) app.rfThread = std::thread(rfWorker);
}

// The §3.3 facts a sibling frame inherits from the head of its stack (issue
// #124): the declared shape, the reading, and the peer's protocol. One helper
// rather than a copy at each of the three places a stack is built, because a
// stack whose frames disagreed about the reading would not be one array read N
// times - it would be two arrays in one Files row.
static void rfInheritRead(App::RFetchJob& j, const ImageDoc* head) {
    if (!head || !head->src) return;
    j.npyRead = head->src->npyRead;
    j.remoteProto = head->src->remoteProto;
    j.npyShape = head->src->npyShape;
}

static void requestFullRemote(const ImageDoc* d, bool low = false) {
    if (d->src->remoteUrl.empty() || d->src->remoteStep <= 1) return;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        // the job the worker is running RIGHT NOW left the queue but is still
        // this uid's fetch: enqueueing another would transfer the frame twice
        // and land a second in-place swap on a by-then-registered source (the
        // landing guard drops it, but the transfer is real bytes)
        if (app.rfBusyUid == d->uid) return;
        for (auto it = app.rfQueue.begin(); it != app.rfQueue.end(); ++it)
            if (it->uid == d->uid) {
                if (it->low && !low) {         // promotion: same job, real priority
                    App::RFetchJob j = std::move(*it);
                    j.low = false;
                    app.rfQueue.erase(it);
                    auto at = app.rfQueue.begin();
                    while (at != app.rfQueue.end() && !at->low) ++at;
                    app.rfQueue.insert(at, std::move(j));
                }
                return;
            }
    }
    App::RFetchJob j;
    j.url = d->src->remoteUrl; j.frame = d->src->remoteFrame; j.uid = d->uid;
    j.low = low;
    j.npyRead = d->src->npyRead;          // the declaration the preview was made
    j.remoteProto = d->src->remoteProto;  // under: the full frame is the SAME array
    j.npyShape = d->src->npyShape;
    // the full frame REPLACES the decimated one, so only the growth is new
    {
        size_t have = d->px().size() * sizeof(float);
        size_t full = have * (size_t)d->src->remoteStep * d->src->remoteStep;
        j.bytes = full > have ? full - have : 0;
    }
    rfEnqueue(std::move(j));
}

// UI thread: swap arrived frames in. After this an ex-preview is a local image.
// fwd: closed-stack results are dropped. What it returns is never held across
// an open or a close - the rule is written over the definition in sequence.inc.
static App::SeqInfo* seqInfo(int id);
static void pumpRemoteFetch() {
    std::vector<App::RFetchDone> batch;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        batch.swap(app.rfDone);
    }
    for (auto& d : batch) {
        if (d.gen != app.rfGen) continue;   // outlived a Close All: drop entirely
        app.rfPending--;
        app.rfBytesInFlight -= std::min(app.rfBytesInFlight.load(), d.bytes);
        app.rfFetched++;
        if (app.rfPending <= 0) {
            app.rfBytesInFlight = 0;        // nothing outstanding: no drift
            if (app.rfFetched > 1)
                fprintf(stderr, "remote: fetch complete (%d items)\n", app.rfFetched.load());
            app.rfTotal = 0;
            app.rfFetched = 0;
        }
        if (d.uid == 0) {                        // a new frame of a remote stack
            // Closed while this fetch was in flight: closeStack swept the QUEUE,
            // but the one job the worker was already running could not be
            // removed there - its result lands here and must be dropped, or a
            // closed stack regrows one orphan frame at a time.
            App::SeqInfo* si = d.seqId != 0 ? seqInfo(d.seqId) : nullptr;
            if (d.seqId != 0 && !si) continue;
            // ...and the shape this frame turned out to hold, against the shape
            // the stack IS. The local loader has refused a disagreeing numbered
            // sibling since PR #116; the frames that arrive over the LINK went
            // straight in, so the very same folder became one stack of four
            // different pictures when it was reached through a peer instead of
            // off this disk. Same rule, same sentence (stackShapeRefusal), and
            // ONE FILE is dropped rather than the stack - the frame simply never
            // lands, so the stack stays short of SeqInfo::expectedFrames and the
            // "n of N" the Files row and the canvas footer already print says
            // so, which is the remote half of "it travels the path that already
            // says n of N".
            //
            // Here and not in rfWorker: the reference is a property of the STACK
            // (it can be closed, and it is the UI thread's), and the shape is in
            // the reply either way - a TILE answers with the frame's real
            // dimensions, so nothing is fetched twice to learn it. What IS paid
            // is the transfer of a frame that is then refused; the peer's META
            // per sibling would be a round trip apiece to save it, on every
            // stack, for the folders that do agree.
            if (d.err.empty() && si && si->refW > 0) {
                std::string bad = stackShapeRefusal(d.w, d.h, d.ch, si->refW, si->refH,
                                                    si->refCh, si->refName);
                if (!bad.empty()) d.err = baseName(d.name) + ": " + bad;
            }
            if (!d.err.empty()) {
                // ONE note reaches the Files panel, so what goes in it is a
                // choice. "The first error explains the gap" is true of a
                // transport error - one dead link, reported once per frame -
                // and false of a refusal, which names a different file and a
                // different reason each time: a stack short two frames would
                // have said why one of them was missing. So: distinct messages,
                // capped at the local loader's three (sequence.inc's
                // `if (++failures <= 3)`), and a repeat still collapses to the
                // single line it always did.
                if (app.seqNote.find(d.err) == std::string::npos &&
                    std::count(app.seqNote.begin(), app.seqNote.end(), '\n') < 3)
                    app.seqNote += "remote: " + d.err + "\n";
                continue;
            }
            auto doc = std::make_unique<ImageDoc>();
            doc->name = d.name;
            doc->note = d.note;                  // the head frame's note, verbatim: the
                                                 // Inspector's source row is a property
                                                 // of the STACK, so it never appears or
                                                 // vanishes as the user steps frames
            FrameSource& S = *doc->src;
            S.mtime = d.mtime;                   // the enqueue-time baseline: every
            S.fsize = d.fsize;                   // frame of one open, one stat epoch
                                                 // (0/0 for ssh://)
            S.path = d.url;
            S.remoteUrl = d.url;
            S.remoteFrame = d.frame;
            // §3.3's three facts, the same on every frame of the stack: the
            // array's declared shape, the reading it is being read under, and
            // whether the peer can serve another one. The Inspector's "read as"
            // line is a property of the FILE, so it must not appear and vanish
            // as the user steps frames - the same rule `note` follows two lines
            // above, for the same reason.
            S.npyShape = d.npyShape;
            S.npyRead = d.npyRead;
            S.remoteProto = d.remoteProto;
            S.dtype = d.dtype;
            S.w = d.w; S.h = d.h; S.ch = d.ch;
            S.data = std::move(d.data);
            S.vmin = d.vmin; S.vmax = d.vmax;
            S.f32loss = d.f32loss;    // measured on the worker; see RFetchDone
            doc->syncMirrors();
            // §6.2: the same url+frame may already be resident (the same remote
            // folder opened twice) - the arrival grafts as a second membership
            // on the resident source instead of keeping a second copy. The
            // fetch itself already happened; what is deduplicated is residency.
            shareOrRegisterSource(*doc);
            doc->seqId = d.seqId; doc->seqIndex = d.seqIndex;
            doc->uid = app.nextUid++;
            doc->texDirty = true;
            // frames of one stack share the display range (like the local
            // loader) and its batch (frame ⊂ stack ⊂ batch)
            bool ranged = false;
            for (const auto& q : app.images)
                if (q->seqId == d.seqId) {
                    doc->black = q->black; doc->white = q->white;
                    doc->batchId = q->batchId;
                    ranged = true;
                    break;
                }
            if (!ranged) defaultRange(*doc);
            app.images.push_back(std::move(doc));  // quiet: never steals selection
            app.imagesRev++;
            continue;
        }
        ImageDoc* im = nullptr;
        for (auto& q : app.images) if (q->uid == d.uid) { im = q.get(); break; }
        if (!im) continue;                       // closed while fetching
        // A DUPLICATE full-res landing (a promote raced the in-flight fetch:
        // requestFullRemote's dedupe reads the queue, and a job the worker had
        // already dequeued was invisible to it). The first landing set step=1
        // and REGISTERED the source - it may be shared by now - so a second
        // in-place swap here would mutate a registered source outside the
        // g_srcRegMtx contract and outside any walk. Drop it, error included
        // (remoteErr on a registered source would un-share it silently). If
        // the peer's file changed between the two fetches, that is Watch's to
        // notice, the same answer as everywhere else.
        if (im->src->remoteStep <= 1) continue;
        if (!d.err.empty()) {
            im->src->remoteErr = d.err;
            toast("remote: " + d.err, true);
            continue;
        }
        int stepBefore = im->src->remoteStep;
        FrameSource& S = *im->src;
        S.data = std::move(d.data);
        S.w = d.w; S.h = d.h; S.ch = d.ch;
        S.dtype = d.dtype;
        S.remoteStep = 1;
        S.remoteErr.clear();
        S.vmin = d.vmin; S.vmax = d.vmax;        // measured on the worker
        S.f32loss = d.f32loss;    // ...and so was this: the preview's census was
                                  // over the decimated samples, and these are
                                  // different pixels. Replaced wholesale with
                                  // the rest of them, never merged - the two
                                  // counts overlap, and a sum of them would be
                                  // a number no array ever had.
        S.rev++;      // in-place pixel replacement (§2.2's "reload" family):
                      // stage 1's only rev writer; the invalidation walk is stage 5
        // §6.2: remoteStep=1 just made this source shareable, and it was
        // guaranteed unregistered until now (step>1 sources never pass
        // srcShareable, previews never register at all). Register it - or
        // ADOPT a resident of the same url+frame that appeared meanwhile:
        // this doc is the SOLE owner of an ex-preview source, the resident
        // holds exactly the full-resolution bytes this fetch delivered, and
        // the invalidation just below (mirrors, dataRev, forgetImage) is the
        // same walk an adoption needs - so adopting changes no pixels and
        // drops "same url opened twice" back to 1x resident bytes. Previews
        // stay out of the registry entirely; promotion re-runs this.
        if (!im->preview) shareOrRegisterSource(*im);
        im->syncMirrors();
        size_t p = im->name.find("  (1/");       // drop the preview marker
        if (p != std::string::npos) im->name.erase(p);
        im->dataRev++;
        im->texDirty = true;
        forgetImage(im);                         // caches hold the preview's numbers
        if (stepBefore > 1) {
            if (im == cur()) {
                // same pixels on screen, now through finer data: rescale the
                // shared view so nothing appears to move at the swap
                app.view.zoom = std::max(app.view.zoom / stepBefore, 1.0f / 512);
                app.view.center.x *= stepBefore;
                app.view.center.y *= stepBefore;
            } else {
                // not on screen: remember, and rescale when it is selected
                im->pendingViewScale = (float)stepBefore;
            }
        }
        app.imagesRev++;
        fprintf(stderr, "remote: full resolution %dx%d %dch for %s\n",
                d.w, d.h, d.ch, im->name.c_str());
    }
}

static void stopRemoteFetcher() {
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        app.rfStop = true;
    }
    app.rfCv.notify_all();
    if (app.rfThread.joinable()) app.rfThread.join();
}

// ---- background MEASURE (server-side analysis) --------------------------------
// Its own ssh connection: a 300-frame aggregate can hold the pipe for seconds,
// and that must not stall the tile fetches on the other worker.
static void mWorker() {
    remote::Session ses;
    ses.setAbort(&app.mStop);         // a blocked read gives up when Quit asks
    std::string sesHost = "\n";
    int sesPort = -1;
    while (!app.mStop) {
        App::MJob job;
        {
            std::unique_lock<std::mutex> lk(app.mMtx);
            app.mCv.wait(lk, []{ return app.mStop || !app.mQueue.empty(); });
            // Stop wins over pending work - the third time this exact line has
            // arrived, after rbWorker and rfWorker (3a5f724). The loop it
            // replaces checked the flag BEFORE dequeuing, so a stop abandoned
            // the queue; asking for the queue to be empty as well lets a worker
            // that was notified with a job and stopped before it could reacquire
            // the mutex run one more measurement inside stopMeasureWorker's
            // join(), with the UI thread waiting on it. The predicate already
            // guarantees the queue is non-empty whenever the flag is false.
            if (app.mStop) break;
            job = std::move(app.mQueue.front());
            app.mQueue.erase(app.mQueue.begin());
        }
        App::MDone d;
        d.token = job.token;
        std::string host, rpath, err;
        int port = 0;
        if (!remote::parseUrl(job.url, host, rpath, &port)) { d.err = "bad remote url"; }
        else {
            d.host = host;
            ensureSession(ses, sesHost, sesPort, host, port, job.exe, d.err);
            if (d.err.empty()) {
                remote::MeasureReq q;
                q.op = job.op;
                q.cfaType = job.cfaType; q.cfaPattern = job.cfaPattern;
                q.black = job.black; q.white = job.white;
                if (job.files.empty()) q.paths = { rpath };
                else                   q.paths = job.files;   // one file per frame
                if (job.rw > 0 && job.rh > 0)
                    q.rois.push_back({ job.rx, job.ry, job.rw, job.rh });
                if (!ses.measure(q, d.res, err)) d.err = err;
                else d.ok = true;
            }
        }
        {
            std::lock_guard<std::mutex> lk(app.mMtx);
            app.mDone.push_back(std::move(d));
        }
        glfwPostEmptyEvent();
    }
}

static void mEnqueue(App::MJob job) {
    if (job.exe.empty()) {
        std::string h, rp2;
        remote::parseUrl(job.url, h, rp2);
        job.exe = app.remoteExe.empty()
            ? (h.empty() ? app.exePath : std::string(REMOTE_HOME) + "/viewer-serve")
            : app.remoteExe;
    }
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        app.mPending++;
        app.mQueue.push_back(std::move(job));
    }
    app.mCv.notify_one();
    if (!app.mThread.joinable()) app.mThread = std::thread(mWorker);
}

static void stopMeasureWorker() {
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        app.mStop = true;
    }
    app.mCv.notify_all();
    if (app.mThread.joinable()) app.mThread.join();
}

// Pull one scalar / series out of a server MeasureResult column 0 by key.
static double mFindNum(const remote::MeasureResult& r, const char* key, double dflt = 0) {
    if (r.cols.empty()) return dflt;
    for (const auto& it : r.cols[0]) if (it.kind == 0 && it.key == key) return it.num;
    return dflt;
}
static const remote::MeasureSeries* mFindSeries(const remote::MeasureResult& r, const char* name) {
    for (const auto& s : r.series) if (s.name == name) return &s;
    return nullptr;
}

static void pumpMeasure() {
    std::vector<App::MDone> batch;
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        batch.swap(app.mDone);
    }
    for (auto& d : batch) {
        app.mPending--;
        // the A slot or the explicitly-measured B slot, by token
        App::ServerTemporal* Sp = nullptr;
        if (app.srvTemporal.token && d.token == app.srvTemporal.token) Sp = &app.srvTemporal;
        else if (app.srvTemporalB.token && d.token == app.srvTemporalB.token)
            Sp = &app.srvTemporalB;
        if (!Sp) continue;                                // superseded request
        App::ServerTemporal& S = *Sp;
        S.pending = false;
        S.host = d.host;
        if (!d.ok) { S.valid = false; S.err = d.err; continue; }
        S.err.clear();
        S.valid = true;
        S.frames = d.res.framesUsed;
        // The peer answers per CFA plane when it was told the mosaic (keys
        // suffixed " R" / " Gr" / " Gb" / " B", serve.cpp planeKey) and with
        // un-suffixed keys when it was not. Take whichever shape arrived and
        // record which it was - collapsing a four-plane reply onto its R plane
        // and calling it "the overall figure" is worse than the pooled number.
        auto has = [&](const char* k) {
            if (d.res.cols.empty()) return false;
            for (const auto& it : d.res.cols[0]) if (it.kind == 0 && it.key == k) return true;
            return false;
        };
        S.nPl = has("sigma_t [DN] R") ? 4 : 1;
        for (int p = 0; p < S.nPl; p++) {
            std::string sfx = S.nPl > 1 ? std::string(" ") + CFA_CH_NAMES[p] : std::string();
            S.mean[p] = mFindNum(d.res, ("mean [DN]" + sfx).c_str());
            S.tempNoise[p] = mFindNum(d.res, ("sigma_t [DN]" + sfx).c_str());
            S.fixedPattern[p] = mFindNum(d.res, ("sigma_fpn [DN]" + sfx).c_str());
            S.totalNoise[p] = mFindNum(d.res, ("sigma_tot [DN]" + sfx).c_str());
            // #57 judgment 2: sigma_fpn arrives CORRECTED, so the two numbers
            // that make it readable arrive with it - how much was subtracted,
            // and whether the subtraction hit the clamp. Which is why rp::VERSION
            // went to 6: a v5 peer sends the uncorrected upper bound under the
            // same key and neither of these, and the panel would then declare a
            // correction that peer never applied.
            S.fpnCorr[p] = mFindNum(d.res, ("fpn_corr [DN]" + sfx).c_str());
            S.fpnClamped[p] = mFindNum(d.res, ("fpn_clamped" + sfx).c_str()) > 0.5;
        }
        if (const auto* fm = mFindSeries(d.res, "frame mean")) { S.idx = fm->xs; S.frameMean = fm->ys; }
        if (const auto* fs = mFindSeries(d.res, "frame std"))  { S.frameStd = fs->ys; }
        fprintf(stderr, "remote: server temporal over %d frames, %d plane(s) - sigma_t "
                        "%.4g, sigma_fpn %.4g [%s]\n", S.frames, S.nPl, S.tempNoise[0],
                S.fixedPattern[0], d.res.serverLoc ? "gpu" : "cpu");
    }
}
