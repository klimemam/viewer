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
// WHICH RANGE: each frame's EFFECTIVE range, which is what effBlack/effWhite
// answer. Per stack and Linked hand every frame the same numbers and need
// nothing here. "Auto per frame" is the one mode that has to ACT: stepping
// onto a frame re-fits it to its own min..max (selectImage, sequence.inc), and
// an export that skipped that would write frames the user has never seen. The
// same call, in the same condition, is made here per frame.
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
    app.videoAskOpen = true;
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
        if (d->w != w || d->h != h)
            return "this stack's frames are not all one size (" +
                   std::to_string(w) + "x" + std::to_string(h) + " and " +
                   std::to_string(d->w) + "x" + std::to_string(d->h) + ") - a video "
                   "has one frame size, so there is nothing this could honestly "
                   "write. Frames land at two resolution levels while a remote "
                   "stack streams: wait for the full frames, then export.";
    }
    const std::string ext = videoExtOf(path);
    if (ext == ".mkv") {
        if (videowrite::ffmpegPath().empty()) return videowrite::ffmpegMissingRefusal();
    } else if (ext == ".avi") {
        std::string big = videowrite::aviSizeRefusal(w, h, (int)fr.size());
        if (!big.empty()) return big;
    } else {
        return "this export writes .avi (uncompressed RGB24, no dependency) and "
               ".mkv (FFV1, lossless compression, needs ffmpeg on PATH)" +
               std::string(ext.empty() ? " - the file name has no extension"
                                       : " - not \"" + ext + "\"") +
               ". Name the file with one of those two.";
    }
    if (framesOut) *framesOut = fr;
    return {};
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
        toast("wrote " + name + tail + " (" + kind + ")" +
              (hl.empty() ? "" : "  -  " + hl));
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
    if (d->w != J.w || d->h != J.h) {
        abandonVideoExport("frame " + std::to_string(J.at + 1) + " changed size mid-write");
        return false;
    }
    // "Auto per frame" re-fits the frame to its OWN min..max on the way in -
    // the same line selectImage runs on a step, in the same condition, so the
    // video shows what stepping through the stack would have shown. Linked and
    // per-stack need nothing: effBlack/effWhite already answer for the stack.
    if (app.rangeScope == 0 && !app.linkRange) { autoRange(*d); d->texDirty = true; }
    static std::vector<uint8_t> rgba;    // one scratch buffer, as rebuildTexture keeps
    static std::vector<uint8_t> rgb;
    renderDocRGBA(*d, rgba);
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
