// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// ---------------------------------------------------- a stack, as a video ----
// Issue #253「Stack を lossless 動画で、表示しているレベルで出力する」.
//
// WHAT LEAVES: every frame of one stack, in seqIndex order, through
// renderDocRGBA - the SAME mapping the screen and the PNG export use (range,
// gamma, colormap, CFA colorize, value highlight), at the frame's own pixel
// size and not at the canvas zoom. So a video and a PNG of the same frame are
// the same picture, and that is asserted rather than asserted-to (see
// --video-selftest, V6).
//
// WHICH RANGE: each frame is drawn at the range the SCREEN gives it in the
// view it is on screen in (#304, exportFrameViewFor). For A's own stack that
// is A stepped onto frame k, B and the slots following to THEIR frame k:
// under the A/B union the union of A_k, B_k, C_k... (a side with no frame k is
// its pinned frame, as on screen, and the announcement says "B held at frame
// 1"); under each side fits itself A_k's own fit; under A's range A_k's own
// black-white. A stack held by B or a slot is drawn in that role, in the view
// follow-frame pairs it in. An export that drew every frame in the screen's
// view instead would draw only the frame on screen as a compared side and
// every other at its own range - two ranges in one video. "Auto per frame"
// additionally has to ACT: stepping A onto a frame re-fits it (selectImage,
// sequence.inc), and the same call, in the same condition, is made here on
// the frame A would be stepped onto. The range is named where the export is
// announced (exportRangeNote), never baked into the file.
//
// WHY IT IS SLICED: renderDocRGBA is CPU-only but not thread-safe - it holds a
// static gamma LUT - so it cannot go to a worker. It runs on the UI thread,
// ONE frame per UI frame, which is pumpSequence's MAX_PER_FRAME shape and for
// #232's reason: a 300-frame stack must not freeze the window for the length
// of the write. Progress and Stop sit in the Files panel where stack loading
// already reports, and Stop DELETES the partial file (videowrite.h says why).
//
// WHY NOT libavcodec: docs/features/media/video-support.md §4, unchanged. See core/videowrite.h
// for what writing - as opposed to reading - does to that argument.

// ".avi" / ".mkv" for the two formats the export dialog offers.
static const char* videoExtFor(int format) { return format == 1 ? ".mkv" : ".avi"; }

// Lower-cased extension of a path, including the dot ("" when there is none).
static std::string videoExtOf(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = path.substr(dot);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e;
}

// The name the save dialog comes up with: the stack's own, plus the extension
// the chosen format implies.
static std::string videoDefaultName(int seqId, int format) {
    std::string base = "stack";
    if (App::SeqInfo* si = seqInfo(seqId)) if (!si->name.empty()) base = si->name;
    // a stack named after a file pattern carries an extension of its own
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    for (char& c : base) if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                             c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    return base + videoExtFor(format);
}

// Is this document a frame of a stack we could export? The menu, the modal and
// the export itself all ask THIS, so they cannot come to disagree - the same
// arrangement exportRefusalFor keeps for a single picture.
static bool canExportStackVideo(const ImageDoc* im) {
    return im && im->seqId > 0 && !framesOfSeq(im->seqId).empty();
}

// ONE verb, wherever it is offered (the File menu, the canvas export block
// beside "Save image as PNG...", the Files panel's stack row): open the
// fps/format question for this stack. Nothing else may start an export, so the
// two settings cannot be skipped by one entry point and not another.
static void askStackVideoExport(int seqId) {
    if (seqId <= 0) return;
    app.videoAskSeqId = seqId;
    app.videoAskCompare = false;
    app.videoAskOpen = true;
}

// "" when a w x h, n-frame video can be written to `path`: the extension
// names the format, and each format's own limit is asked before a byte is
// written. One answer for the stack video and the compare video.
static std::string videoPathRefusal(const std::string& path, int w, int h, int n) {
    const std::string ext = videoExtOf(path);
    if (ext == ".mkv") {
        if (videowrite::ffmpegPath().empty()) return videowrite::ffmpegMissingRefusal();
    } else if (ext == ".avi") {
        std::string big = videowrite::aviSizeRefusal(w, h, n);
        if (!big.empty()) return big;
    } else {
        return "this export writes .avi (uncompressed RGB24, no dependency) and "
               ".mkv (FFV1, lossless compression, needs ffmpeg on PATH)" +
               std::string(ext.empty() ? " - the file name has no extension"
                                       : " - not \"" + ext + "\"") +
               ". Name the file with one of those two.";
    }
    return {};
}

// Open the sink the extension names for a job whose size is set, and mark it
// running: "" on success; on failure the job is cleared and the error returned.
static std::string openVideoSink(App::VideoExport& J, const std::string& path, double fps) {
    std::string err;
    if (videoExtOf(path) == ".mkv") {
        J.sink = videowrite::openFfv1(path, J.w, J.h, fps, err);
        J.sinkName = "FFV1/MKV";
    } else {
        J.sink = videowrite::openAvi(path, J.w, J.h, fps, err);
        J.sinkName = "uncompressed AVI";
    }
    if (!J.sink) { J = App::VideoExport(); return err; }
    J.active = true;
    return {};
}

// "" when this stack can be written to `path`; the refusal otherwise. Every
// answer here is available BEFORE a byte is written, which is the whole point:
// a 4 GB limit discovered at frame 900, or an absent ffmpeg discovered after
// the pixels have been rendered, is a refusal that already cost something.
static std::string stackVideoRefusal(int seqId, const std::string& path,
                                     double fps, std::vector<int>* framesOut = nullptr) {
    if (app.videoExport.active)
        return "a video export is already running - stop it first (Files panel)";
    if (!(fps > 0.0) || !std::isfinite(fps))
        return "the frame rate must be a positive number";
    std::vector<int> fr = framesOfSeq(seqId);
    if (fr.empty()) return "that stack is not open any more";
    const ImageDoc* first = app.images[fr.front()].get();
    const int w = first->w, h = first->h;
    for (size_t i = 0; i < fr.size(); i++) {
        const ImageDoc* d = app.images[fr[i]].get();
        // A DECIMATED PREVIEW is not an export - exportRefusalFor is the one
        // predicate that says so, and a stack is refused if ANY of its frames
        // is one. The alternative is a video whose frame 7 is a sixteenth of
        // the picture, which no file can afterwards explain about itself.
        std::string why = exportRefusalFor(d);
        if (!why.empty())
            return "frame " + std::to_string(d->seqIndex + 1) + " of " +
                   std::to_string(fr.size()) + ": " + why;
        // ...and every side the frame's range is taken OVER (the A/B union
        // reads B's and the slots' frame k): the same "not yet" for them.
        // Only the union reads them, so only the union builds the view.
        if (rangeReadsOtherSides()) {
            ImageDoc* stepTo = nullptr;
            ImageDoc* fi = app.images[fr[i]].get();
            why = viewRefusalFor(exportFrameViewFor(fi, stepTo), fi);
            if (!why.empty())
                return "frame " + std::to_string(d->seqIndex + 1) + " of " +
                       std::to_string(fr.size()) + ": " + why;
        }
        if (d->w != w || d->h != h)
            return "this stack's frames are not all one size (" +
                   std::to_string(w) + "x" + std::to_string(h) + " and " +
                   std::to_string(d->w) + "x" + std::to_string(d->h) + ") - a video "
                   "has one frame size, so there is nothing this could honestly "
                   "write. Frames land at two resolution levels while a remote "
                   "stack streams: wait for the full frames, then export.";
    }
    const std::string why = videoPathRefusal(path, w, h, (int)fr.size());
    if (!why.empty()) return why;
    if (framesOut) *framesOut = fr;
    return {};
}

// ---- the compare view, as a video (#308) ------------------------------------
// Frame k is composeCompareRGBA of the view A's frame k is on screen in -
// viewSidesFor(A_k), i.e. A stepped onto frame k and B and the slots following
// it (exportFrameViewFor's rule for A's own stack) - so frame k of the file is
// the PNG "Save compare view as PNG..." writes with A on frame k. The stack
// walked is the stack of the A the export was ASKED under (CompareSpec::aUid),
// not whatever is current when the OS dialog closes, and every frame is drawn
// by that spec. Same job, same pump, same sinks as a stack video; only what
// one frame IS differs (App::VideoExport::compare).

// The A a spec was taken under, or null when it has closed since.
static ImageDoc* compareSpecA(const App::CompareSpec& S) {
    for (const auto& d : app.images) if (d->uid == S.aUid) return d.get();
    return nullptr;
}

// "" with the frames, and the composite size, when the compare view of A's
// stack can be written under `S`; the refusal otherwise. Every frame's layout
// is asked for up front, without drawing: a side that is a preview at frame
// 40, or a composite that changes size there, is refused before a byte is
// written.
static std::string compareVideoPlan(const App::CompareSpec& S, std::vector<int>& fr, int& w, int& h) {
    fr.clear(); w = h = 0;
    if (S.mode == App::CmpOff || app.compareMode == App::CmpOff)
        return "compare is off - there is no compare view to export";
    ImageDoc* a = compareSpecA(S);
    if (!a) return S.aUid ? "the A this export was asked for has been closed" : "nothing to export";
    if (!canExportStackVideo(a))
        return "A is not a frame of a stack - a compare video steps A through its stack; "
               "for one picture use Save compare view as PNG";
    fr = framesOfSeq(a->seqId);
    for (size_t i = 0; i < fr.size(); i++) {
        ImageDoc* fi = app.images[fr[i]].get();
        CompareLayout L;
        const std::string why = compareLayoutFor(viewSidesFor(fi), L, S);
        if (!why.empty())
            return "frame " + std::to_string(fi->seqIndex + 1) + " of " +
                   std::to_string(fr.size()) + ": " + why;
        if (i == 0) { w = L.w; h = L.h; }
        else if (L.w != w || L.h != h)
            return "the compare view is " + std::to_string(w) + "x" + std::to_string(h) +
                   " at frame 1 and " + std::to_string(L.w) + "x" + std::to_string(L.h) +
                   " at frame " + std::to_string(fi->seqIndex + 1) + " - a video has one "
                   "frame size. Pin sides whose stacks keep one size, then export.";
    }
    return {};
}
static std::string compareVideoRefusal(const std::string& path, double fps,
                                       const App::CompareSpec& S,
                                       std::vector<int>* framesOut = nullptr,
                                       int* wOut = nullptr, int* hOut = nullptr) {
    if (app.videoExport.active)
        return "a video export is already running - stop it first (Files panel)";
    if (!(fps > 0.0) || !std::isfinite(fps))
        return "the frame rate must be a positive number";
    std::vector<int> fr; int w = 0, h = 0;
    std::string why = compareVideoPlan(S, fr, w, h);
    if (!why.empty()) return why;
    why = videoPathRefusal(path, w, h, (int)fr.size());
    if (!why.empty()) return why;
    if (framesOut) *framesOut = fr;
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
    return {};
}
// File > Export compare view as video...: the fps / format modal first, as
// for a stack, refused up front when there is nothing it could write. The
// spec - A, the wipe column, the letters, the gain - is taken HERE, when the
// export is asked for; the modal and the save dialog that follow it, and the
// job itself, use it (open_dispatch.inc hands it to startCompareVideoExport).
static void askCompareVideoExport() {
    app.videoDlgCmpSpec = compareSpecNow();
    std::vector<int> fr; int w = 0, h = 0;
    const std::string why = compareVideoPlan(app.videoDlgCmpSpec, fr, w, h);
    if (!why.empty()) { toast(why, true); return; }
    app.videoAskSeqId = compareSpecA(app.videoDlgCmpSpec)->seqId;
    app.videoAskCompare = true;
    app.videoAskOpen = true;
}
// Start the compare job under `spec` (null = compareSpecNow()): "" when it is
// running, the refusal otherwise.
static std::string startCompareVideoExport(const std::string& path, double fps,
                                           const App::CompareSpec* spec = nullptr) {
    const App::CompareSpec S = spec ? *spec : compareSpecNow();
    std::vector<int> fr; int w = 0, h = 0;
    std::string why = compareVideoRefusal(path, fps, S, &fr, &w, &h);
    if (!why.empty()) return why;
    App::VideoExport& J = app.videoExport;
    J = App::VideoExport();
    J.compare = true;
    J.cmp = S;
    J.seqId = compareSpecA(S)->seqId;
    J.path = path;
    J.fps = fps;
    J.w = w;
    J.h = h;
    for (int i : fr) J.uids.push_back(app.images[i]->uid);
    {
        const ViewSides v0 = viewSidesFor(app.images[fr.front()].get());
        CompareLayout L;
        compareLayoutFor(v0, L, S);
        J.rangeNote = compareExportNote(v0, L, true, 0.0f, S);
    }
    return openVideoSink(J, path, fps);
}

// Start the job. "" when it is running; the refusal otherwise (and then
// nothing has been created on disk). `uid`-keyed, because the frames must be
// able to CLOSE under this and be noticed doing it.
static std::string startStackVideoExport(int seqId, const std::string& path, double fps) {
    std::vector<int> fr;
    std::string why = stackVideoRefusal(seqId, path, fps, &fr);
    if (!why.empty()) return why;
    App::VideoExport& J = app.videoExport;
    J = App::VideoExport();
    J.seqId = seqId;
    J.path = path;
    J.fps = fps;
    J.w = app.images[fr.front()]->w;
    J.h = app.images[fr.front()]->h;
    for (int i : fr) J.uids.push_back(app.images[i]->uid);
    {
        ImageDoc* first = app.images[fr.front()].get();
        ImageDoc* stepTo = nullptr;
        J.rangeNote = exportRangeNote(*first, exportFrameViewFor(first, stepTo), true);
    }
    return openVideoSink(J, path, fps);
}

// What the Files panel shows while this runs ("" when nothing is running).
static std::string videoExportProgress() {
    const App::VideoExport& J = app.videoExport;
    if (!J.active) return {};
    return std::to_string(J.at) + " / " + std::to_string(J.uids.size()) + " frames";
}

// Close the file, delete it, and say why. One place, so a stop and a failure
// leave the disk in the same state - which is: nothing.
static void abandonVideoExport(const std::string& why) {
    App::VideoExport& J = app.videoExport;
    const std::string name = baseName(J.path);
    if (J.sink) J.sink->abort();
    J = App::VideoExport();
    toast(why + " - " + name + " deleted", true);
}

// ONE frame per call, from the UI thread. Returns true while the job is still
// running, so a selftest can drive it to completion with `while (pump()) {}`
// and the frame loop can call it once and get on with the frame.
static bool pumpVideoExport() {
    App::VideoExport& J = app.videoExport;
    if (!J.active) return false;
    if (J.stop) { abandonVideoExport("video export stopped"); return false; }
    if (J.at >= J.uids.size()) {                     // done
        std::string err;
        const bool ok = J.sink->finish(err);
        const int n = J.sink->framesWritten();
        const std::string name = baseName(J.path), kind = J.sinkName;
        const int w = J.w, h = J.h;
        const double fps = J.fps;
        std::string rNote = J.rangeNote;
        for (const std::string& h : J.held) rNote += "; " + h;
        // A finish that failed leaves a file with no index and a header still
        // claiming 0 frames - unplayable, and saying nothing about why. It goes
        // the same way a Stop goes, for the same reason: nothing left behind.
        if (!ok) { abandonVideoExport("video export failed: " + err); return false; }
        J = App::VideoExport();
        char tail[96];
        snprintf(tail, sizeof tail, "  %d frames  %dx%d  @%.6g fps", n, w, h, fps);
        // The highlight note goes where the export is ANNOUNCED, exactly as it
        // does for a PNG: the magenta is IN the file (renderDocRGBA is the one
        // mapping) and the badge that explains it is not.
        const std::string hl = exportHighlightNote(cur());
        toast("wrote " + name + tail + " (" + kind + ")  -  " + rNote +
              (hl.empty() ? "" : "  -  " + hl));
        fprintf(stderr, "export: video %s\n", rNote.c_str());
        if (!hl.empty()) fprintf(stderr, "export: video %s\n", hl.c_str());
        return false;
    }
    ImageDoc* d = nullptr;
    for (const auto& p : app.images) if (p->uid == J.uids[J.at]) { d = p.get(); break; }
    if (!d) {
        abandonVideoExport("frame " + std::to_string(J.at + 1) + " of " +
                           std::to_string(J.uids.size()) +
                           " was closed while the video was being written");
        return false;
    }
    // The same two refusals the start already made, asked again per frame: a
    // full frame can be REPLACED by a preview while this runs (a reload, a
    // remote re-fetch), and a stack whose frames changed size mid-write would
    // otherwise be written as garbage rows.
    std::string why = exportRefusalFor(d);
    if (!why.empty()) {
        abandonVideoExport("frame " + std::to_string(J.at + 1) + ": " + why);
        return false;
    }
    // The view this frame is on screen in (#304): for A's stack, A on it and B
    // and the slots followed to theirs. Its sides can turn into previews
    // mid-write too, and a side held at its pinned frame is said at the end.
    ImageDoc* stepTo = nullptr;
    // a compare video's frame is A_k's own view (A is the spec's, not cur())
    const ViewSides view = J.compare ? viewSidesFor(d) : exportFrameViewFor(d, stepTo);
    if (J.compare) stepTo = d;
    why = viewRefusalFor(view, d);
    if (!why.empty()) {
        abandonVideoExport("frame " + std::to_string(J.at + 1) + ": " + why);
        return false;
    }
    for (const std::string& h : viewHeldSides(view))
        if (std::find(J.held.begin(), J.held.end(), h) == J.held.end()) J.held.push_back(h);
    if (!J.compare && (d->w != J.w || d->h != J.h)) {
        abandonVideoExport("frame " + std::to_string(J.at + 1) + " changed size mid-write");
        return false;
    }
    // A compare video draws by the spec it was started under (mode, wipe
    // column, letters, gain), so a toggle or a drag mid-write changes nothing.
    // What the VIEWS resolve from cannot be snapshotted that way - B, the
    // slots, follow-frame - nor can |A-B| (renderDiffRGBA reads the flag): a
    // change to any of them would change the picture halfway through the
    // file, so it stops the job and the partial file goes.
    if (J.compare && (app.compareBUid != J.cmp.bUid || app.cmpExtra != J.cmp.slots ||
                      app.compareFollowFrame != J.cmp.follow || app.diffAbs != J.cmp.diffAbs)) {
        abandonVideoExport("frame " + std::to_string(J.at + 1) + ": B, a compare slot, "
                           "follow-frame or the difference's |A-B| changed mid-write");
        return false;
    }
    // "Auto per frame" re-fits the frame A steps onto - the same call
    // selectImage makes on a step, in the same condition, so the video shows
    // what stepping through the stack would have shown. For A's own stack that
    // is the frame itself; for a stack drawn as B it is A's paired frame (B is
    // never re-fitted by a step), and with A staying put it is nothing. WHICH fit
    // (min..max, the 0.1-99.9% cut, median->target) is app.autoFlavor's
    // answer and refitByFlavor's alone (#252): a min..max hard-wired here would
    // be a second answer to the one question. Linked and per-stack need
    // nothing: displayRangeFor already answers for the stack.
    if (app.rangeScope == 0 && !app.linkRange && stepTo) { refitByFlavor(*stepTo); stepTo->texDirty = true; }
    static std::vector<uint8_t> rgba;    // one scratch buffer, as rebuildTexture keeps
    static std::vector<uint8_t> rgb;
    if (J.compare) {
        // the compare view this frame is on screen in, by the one function
        // that composes it - the PNG of that view is these bytes (#308)
        int cw = 0, chh = 0;
        std::string note;
        if (!composeCompareRGBA(view, rgba, cw, chh, note, &J.cmp)) {
            abandonVideoExport("frame " + std::to_string(J.at + 1) + ": " + note);
            return false;
        }
        if (cw != J.w || chh != J.h) {
            abandonVideoExport("frame " + std::to_string(J.at + 1) +
                               ": the compare view changed size mid-write");
            return false;
        }
    } else {
        renderDocRGBA(*d, rgba, &view);
    }
    const size_t px = (size_t)J.w * J.h;
    rgb.resize(px * 3);
    for (size_t p = 0; p < px; p++) {
        rgb[p * 3 + 0] = rgba[p * 4 + 0];
        rgb[p * 3 + 1] = rgba[p * 4 + 1];
        rgb[p * 3 + 2] = rgba[p * 4 + 2];
    }
    std::string err;
    if (!J.sink->writeFrame(rgb.data(), err)) {
        abandonVideoExport("video export failed: " + err);
        return false;
    }
    J.at++;
    return true;
}
