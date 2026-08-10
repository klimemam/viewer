// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- sequences (連番)
// A sequence is one "stack": frames from numbered files in a folder that share a
// decode recipe. It is the unit temporal analysis operates on.
// How much RAM the loader may fill with frames. A hardcoded 6 GB meant a machine
// with 128 GB stopped 300-frame folders at 60 frames, with no way to say "use the
// memory I have" - and 6 GB is at the same time too much for a small laptop.
static size_t physicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) return (size_t)ms.ullTotalPhys;
#elif defined(__APPLE__)
    uint64_t v = 0; size_t len = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &len, nullptr, 0) == 0) return (size_t)v;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) return (size_t)pages * (size_t)page;
#endif
    return (size_t)8 << 30;                       // unknown: assume a modest box
}
static size_t seqMemBudget() {
    if (app.memBudgetGB > 0) return (size_t)(app.memBudgetGB * 1073741824.0);
    // default: most of the machine, but never the whole of it
    size_t phys = physicalMemoryBytes();
    return std::max((size_t)2 << 30, (size_t)(phys * 0.6));
}
// frames already resident, so the budget covers everything open and not just the
// stack being loaded right now
static size_t residentImageBytes() {
    // the sum over unique SOURCES: two docs sharing one source (stage 2+) hold
    // one copy of the pixels. While nothing is shared this equals the old
    // per-doc sum exactly.
    size_t n = 0;
    std::unordered_set<uint64_t> seen;
    seen.reserve(app.images.size());
    for (const auto& d : app.images)
        if (seen.insert(d->src->srcId).second) n += d->px().size() * sizeof(float);
    return n;
}
// What the budget has to reckon with: what has landed PLUS what has been
// committed and is still on the wire. Sizing a prefetch against residency
// alone lets N overlapping opens each claim the whole budget.
static size_t claimedImageBytes() {
    return residentImageBytes() + app.rfBytesInFlight.load();
}

// ---- the source map: which documents are holding which pixels ---------------
// Sharing is invisible from outside this process. srcId is read in exactly one
// place (residentImageBytes' dedupe, just above), use_count() in exactly one
// (the crop CoW), rev in none at all, and none of the three has a printing
// path - so "these two stacks are one source" cannot be asserted from a CLI,
// which is why docs/verify-functional.md D-1/D-2/D-7 stand at 要probe and A-12
// could be settled only by reading the code. This writes down what the
// documents ACTUALLY point at, one line per membership.
//
// Two decisions, both of them about being readable back:
//   - the source is named by a DENSE ordinal, not by srcId. srcId is a global
//     counter, so its value depends on how many frames were decoded before
//     this one and differs between runs; nothing can assert on it. "Shared"
//     means two documents print the SAME ordinal, and that is stable.
//   - the rows are sorted by (stack, position, uid). app.images is in arrival
//     order, and a background loader does not promise one.
// Like g_tilePanesDrawn and g_filesBadgeProbe, this records what IS, never
// what ought to be: refs is use_count() as it stands, and mtime/fsize are
// whatever statSourceFile left there, 0 included.
struct SrcMapRow {
    uint64_t uid = 0;
    int seqId = 0, seqIndex = 0;
    int src = 0;              // dense source ordinal: equal ordinal = shared
    long refs = 0;            // src.use_count()
    int rev = 0, dataRev = 0;
    int64_t mtime = 0;
    uint64_t fsize = 0;
    size_t bytes = 0;
    int w = 0, h = 0, ch = 1;
    bool preview = false;
    std::string name, member, path;
};
static std::vector<SrcMapRow> srcMapRows() {
    std::vector<const ImageDoc*> ds;
    ds.reserve(app.images.size());
    for (const auto& d : app.images) if (d) ds.push_back(d.get());
    std::stable_sort(ds.begin(), ds.end(), [](const ImageDoc* a, const ImageDoc* b) {
        if (a->seqId != b->seqId) return a->seqId < b->seqId;
        if (a->seqIndex != b->seqIndex) return a->seqIndex < b->seqIndex;
        return a->uid < b->uid;
    });
    std::vector<SrcMapRow> rows;
    std::unordered_map<uint64_t, int> dense;
    for (const ImageDoc* d : ds) {
        const FrameSource& S = *d->src;
        auto it = dense.find(S.srcId);
        if (it == dense.end()) it = dense.emplace(S.srcId, (int)dense.size()).first;
        SrcMapRow r;
        r.uid = d->uid; r.seqId = d->seqId; r.seqIndex = d->seqIndex;
        r.src = it->second;
        r.refs = d->src.use_count();
        r.rev = S.rev; r.dataRev = d->dataRev;
        r.mtime = S.mtime; r.fsize = S.fsize;
        r.bytes = S.data.size() * sizeof(float);
        r.w = S.w; r.h = S.h; r.ch = S.ch;
        r.preview = d->preview;
        r.name = d->name; r.member = S.member; r.path = S.path;
        rows.push_back(std::move(r));
    }
    return rows;
}
// How many distinct sources the open documents hold between them.
static int srcMapSources() {
    std::unordered_set<uint64_t> seen;
    for (const auto& d : app.images) if (d) seen.insert(d->src->srcId);
    return (int)seen.size();
}
// The map as stderr lines. <tag> names the moment, because the interesting
// question is always "the same map before and after WHAT".
static void srcMapDump(const std::string& tag) {
    std::vector<SrcMapRow> rows = srcMapRows();
    for (const SrcMapRow& r : rows) {
        std::string tail = " name='" + r.name + "'";
        if (!r.member.empty()) tail += " member='" + r.member + "'";
        if (!r.path.empty())   tail += " path='" + r.path + "'";
        fprintf(stderr, "srcmap: %s uid=%llu seq=%d idx=%d src=#%d refs=%ld rev=%d "
                        "datarev=%d %dx%dx%d bytes=%zu mtime=%lld fsize=%llu%s%s\n",
                tag.c_str(), (unsigned long long)r.uid, r.seqId, r.seqIndex, r.src,
                r.refs, r.rev, r.dataRev, r.w, r.h, r.ch, r.bytes,
                (long long)r.mtime, (unsigned long long)r.fsize,
                r.preview ? " preview" : "", tail.c_str());
    }
    fprintf(stderr, "srcmap: %s %zu doc(s), %d source(s), %zu resident byte(s)\n",
            tag.c_str(), rows.size(), srcMapSources(), residentImageBytes());
    fflush(stderr);
}

// split a stem into alternating text / digit segments: "flat_0007_640x480" ->
// ["flat_"]["0007"]["_"]["640"]["x"]["480"]
struct NameSeg { bool digit; std::string s; };
static std::vector<NameSeg> segmentName(const std::string& stem) {
    std::vector<NameSeg> segs;
    size_t i = 0;
    while (i < stem.size()) {
        bool d = isdigit((unsigned char)stem[i]) != 0;
        size_t j = i;
        while (j < stem.size() && (isdigit((unsigned char)stem[j]) != 0) == d) j++;
        segs.push_back({ d, stem.substr(i, j - i) });
        i = j;
    }
    return segs;
}

// The '?' pattern of a set of file NAMES already in hand (no filesystem, no
// peer): the same segment rule findSequenceSiblings and the peer's grouper use,
// applied to an arbitrary selection. Empty when the names do not share a
// structure - a caller that cannot name a stack must say so, not invent one.
std::string patternOfNames(std::vector<std::string> names) {
    if (names.size() < 2) return {};
    std::sort(names.begin(), names.end());
    std::string ext;
    {
        size_t d = names[0].find_last_of('.');
        if (d != std::string::npos) ext = names[0].substr(d);
    }
    std::vector<std::vector<NameSeg>> segs;
    for (const auto& n : names) {
        size_t d = n.find_last_of('.');
        if ((d == std::string::npos ? std::string() : n.substr(d)) != ext) return {};
        segs.push_back(segmentName(d == std::string::npos ? n : n.substr(0, d)));
        if (segs.back().size() != segs[0].size()) return {};
        for (size_t k = 0; k < segs[0].size(); k++) {
            if (segs.back()[k].digit != segs[0][k].digit) return {};
            if (!segs[0][k].digit && segs.back()[k].s != segs[0][k].s) return {};
        }
    }
    // the frame axis is the LAST digit field that varies (capture software puts
    // the counter last) - the rule findSequenceSiblings and serve.cpp both use
    int best = -1;
    for (size_t k = 0; k < segs[0].size(); k++) {
        if (!segs[0][k].digit) continue;
        for (size_t i = 1; i < segs.size(); i++)
            if (segs[i][k].s != segs[0][k].s) { best = (int)k; break; }
    }
    if (best < 0) return {};
    // every OTHER digit field has to be constant, or this is not one stack
    for (size_t k = 0; k < segs[0].size(); k++) {
        if (!segs[0][k].digit || (int)k == best) continue;
        for (size_t i = 1; i < segs.size(); i++)
            if (segs[i][k].s != segs[0][k].s) return {};
    }
    std::string pat;
    for (size_t k = 0; k < segs[0].size(); k++)
        pat += (int)k == best ? std::string(segs[0][k].s.size(), '?') : segs[0][k].s;
    pat += ext;
    return rp::patternWithExtent(pat, names);
}

// The canon's default stack name: `フォルダ/パターン` (docs/terminology.md).
// Seven stacks all called frame_000.npy would be indistinguishable in every
// panel, AND extractLevelFromName only strips a frame-axis extent run from a
// name that HAS a folder part - so a bare member name parses its first digit
// run as a level, which is how a lit stack ends up proposed at 0.
std::string stackNameFor(const std::string& dir, const std::string& pattern) {
    if (pattern.empty()) return {};
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    size_t s = d.find_last_of('/');
    std::string leaf = s == std::string::npos ? d : d.substr(s + 1);
    if (leaf == "~" || leaf == "." || leaf.empty()) return pattern;
    return leaf + "/" + pattern;
}

// Siblings = files whose names differ ONLY in one digit field. The field is
// chosen by which one actually varies in the folder, so "shot_0007_1920x1080.raw"
// groups on 0007 and not on the resolution.
//
// THE RULE, over names already in hand: no filesystem, no directory_iterator.
// findSequenceSiblings below is this function plus one listing, and the split
// exists because Watch re-derives a stack's membership every few seconds
// (docs/watch-design.md §1: "the member list is taken again with the SAME
// function the open used") over a listing it has ALREADY taken - §2 wants one
// listing per directory per poll however many stacks live in it, and calling
// the IO version per stack would take one apiece. Splitting it also puts the
// grouping rule itself under test with no fixture directory at all.
//
// `headName` and `names` are BASENAMES; the result is the subset of `names`
// that belongs to the head's stack, ordered by the numeric field.
static std::vector<std::string> siblingNamesAmong(const std::string& headName,
                                                  const std::vector<std::string>& names,
                                                  std::string& patternOut) {
    std::vector<std::string> out;
    const std::filesystem::path hp = pathFromUtf8(headName);
    const std::string stem = hp.stem().u8string(), ext = hp.extension().u8string();
    std::vector<NameSeg> segs = segmentName(stem);

    // candidate files: same extension, same segment structure, all text segments equal
    struct Cand { std::vector<NameSeg> segs; std::string name; };
    std::vector<Cand> cands;
    for (const std::string& n : names) {
        const std::filesystem::path np = pathFromUtf8(n);
        if (np.extension().u8string() != ext) continue;
        std::vector<NameSeg> s2 = segmentName(np.stem().u8string());
        if (s2.size() != segs.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < segs.size() && ok; k++) {
            if (segs[k].digit != s2[k].digit) ok = false;
            else if (!segs[k].digit && segs[k].s != s2[k].s) ok = false;
        }
        if (ok) cands.push_back({ std::move(s2), n });
    }
    if (cands.size() < 2) return out;

    // The frame axis is the LAST digit field that varies among the siblings -
    // capture software puts the counter last (frame_001, IMG_0001, lv000_f02).
    // Picking the field with the most distinct values instead made a folder of
    // 10 illuminances x 3 frames group by ILLUMINANCE, so each "stack" spanned
    // ten levels and its sigma_t meant nothing. The peer uses the same rule.
    int best = -1;
    for (size_t k = 0; k < segs.size(); k++) {
        if (!segs[k].digit) continue;
        std::vector<std::string> vals;
        bool othersMatch = true;
        for (const auto& c : cands) {
            bool same = true;
            for (size_t m = 0; m < segs.size(); m++)
                if (m != k && segs[m].digit && c.segs[m].s != segs[m].s) { same = false; break; }
            if (!same) continue;
            bool dup = false;
            for (const auto& v : vals) if (v == c.segs[k].s) { dup = true; break; }
            if (!dup) vals.push_back(c.segs[k].s);
        }
        (void)othersMatch;
        if (vals.size() >= 2) best = (int)k;     // keep overwriting: the last wins
    }
    if (best < 0) return out;

    std::vector<std::pair<long long, std::string>> found;
    for (const auto& c : cands) {
        bool same = true;
        for (size_t m = 0; m < segs.size(); m++)
            if ((int)m != best && segs[m].digit && c.segs[m].s != segs[m].s) { same = false; break; }
        if (!same) continue;
        found.emplace_back(atoll(c.segs[best].s.c_str()), c.name);
    }
    if (found.size() < 2) return out;
    std::sort(found.begin(), found.end());
    patternOut.clear();
    for (size_t k = 0; k < segs.size(); k++)
        // '?' and not '#': ImGui truncates any label at "##", which blanked
        // every all-digit sequence name in the picker ('?' also reads as glob)
        patternOut += ((int)k == best) ? std::string(segs[k].s.size(), '?') : segs[k].s;
    patternOut += ext;
    out.reserve(found.size());
    for (auto& f : found) out.push_back(f.second);
    // ...then say what the '?' run actually spans. Same function the peer runs
    // (rp::patternWithExtent): a folder opened locally and the same folder
    // listed over ssh must produce the identical stack name.
    patternOut = rp::patternWithExtent(patternOut, out);
    return out;
}

// ...and the same rule with the listing taken here. The paths it returns are
// built exactly as directory_iterator built them (dir / name), so what callers
// see is byte-identical to what this function returned before the rule was
// split out above.
static std::vector<std::string> findSequenceSiblings(const std::string& path,
                                                     std::string& patternOut) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path p = pathFromUtf8(path);
    std::filesystem::path dir = p.parent_path();
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        names.push_back(e.path().filename().u8string());
    }
    for (const std::string& n : siblingNamesAmong(p.filename().u8string(), names, patternOut))
        out.push_back((dir / pathFromUtf8(n)).u8string());
    return out;
}

static void stopSequenceLoader() {
    app.seqCancel = true;
    if (app.seqThread.joinable()) app.seqThread.join();
    app.seqRunning = false;
    app.seqCancel = false;
    std::lock_guard<std::mutex> lk(app.seqMtx);
    app.seqReady.clear();
}

// (frameShapeText / stackShapeRefusal moved to core/app/util.inc: the remote
// fetcher refuses the same disagreement in the same words, and util.inc is
// above both transports. The rule is one rule, so it has one definition.)

// Spawn the worker. The thread only decodes (no app state, no GL) and pushes
// finished frames into seqReady; the UI thread integrates them in pumpSequence().
static void startSequenceLoad(int imageIdx, const std::vector<std::string>& files,
                              const std::string& pattern) {
    if (imageIdx < 0 || imageIdx >= (int)app.images.size()) return;
    stopSequenceLoader();
    ImageDoc* ref = app.images[imageIdx].get();
    App::SeqInfo info;
    info.id = app.nextSeqId++;
    info.name = pattern.empty() ? ref->name : pattern;
    // docs/terminology.md: "部分ロードされた stack の集計は「何枚中何枚か」を
    // 必ず併記する". expectedFrames was a REMOTE-only field, so a mid-load local
    // stack could not state the ratio even in principle - the A/B header
    // printed "n=5/5" while the loader was still decoding frames 6..24.
    info.expectedFrames = (int)files.size();
    info.lastImageIdx = imageIdx;
    {   // watch-design §6: IS this file list the folder's whole sibling group?
        // Only a stack of which that is true may have the rule re-applied by a
        // Reload and admit files that appeared (§11.4: a picker can hand this
        // function three of a folder's five, and re-admitting the other two
        // would undo the user's own choice).
        //
        // Asked HERE, once per stack, and answered by RUNNING the rule rather
        // than by a flag threaded through the six call sites - being wrong at
        // one of those is a stack that silently re-admits excluded frames, and
        // the cost is one directory listing per stack opened (the same listing
        // this open already took once, on a folder it has in cache).
        const std::filesystem::path p0 = pathFromUtf8(files[0]);
        std::string pat2;
        std::vector<std::string> sib = findSequenceSiblings(files[0], pat2);
        std::vector<std::string> a, b;
        for (const std::string& f : files) a.push_back(baseName(f));
        for (const std::string& f : sib)   b.push_back(baseName(f));
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (!b.empty() && a == b && !p0.parent_path().empty()) {
            info.ruleDir = p0.parent_path().u8string();
            info.ruleHead = p0.filename().u8string();
        }
    }
    app.seqs.push_back(info);
    ref->seqId = info.id;
    // position of the already-open frame inside the file list
    int selfIdx = 0;
    for (int i = 0; i < (int)files.size(); i++)
        if (files[i] == ref->src->path) { selfIdx = i; break; }
    ref->seqIndex = selfIdx;

    // capture everything the worker needs BY VALUE
    struct Job { std::string path; int index; };
    std::vector<Job> jobs;
    for (int i = 0; i < (int)files.size(); i++)
        if (i != selfIdx) jobs.push_back({ files[i], i });
    bool isRaw = ref->src->rawDtype >= 0;
    // ...and which door the SIBLINGS go through, decided by the reference
    // frame's own path: whatever read frame 1 reads the rest (docs/§6.2 - one
    // stack is one kind of thing). A picture format decodes through the seam on
    // this thread exactly as .npy does through decodeNpy.
    const bool isPicture = !isRaw && imagefile::forPath(ref->src->path) != nullptr;
    RawDialog recipe;
    if (isRaw) {
        recipe.dtype = ref->src->rawDtype;
        recipe.interp = ref->src->rawInterp;
        if (RAW_INTERP_CH[recipe.interp] == 1)
            recipe.interp = ref->cfa == 2 ? RI_QUAD : ref->cfa == 1 ? RI_BAYER : RI_GRAY;
        recipe.w = ref->src->srcW > 0 ? ref->src->srcW : ref->w;
        recipe.h = ref->src->srcH > 0 ? ref->src->srcH : ref->h;
        recipe.offset = ref->src->rawOffset;
        recipe.littleEndian = ref->src->rawLE;
        recipe.cfaPattern = ref->cfaPattern & 3;
        recipe.cropOn = isCropped(*ref);
        recipe.cropX = ref->src->cropX; recipe.cropY = ref->src->cropY;
        recipe.cropW = ref->w; recipe.cropH = ref->h;
        recipe.replaceIdx = -1;
    }
    app.seqLoadingId = info.id;
    app.seqBytes = ref->px().size() * sizeof(float);
    app.seqDone = 0;
    app.seqTotal = (int)jobs.size();
    app.seqCancel = false;
    app.seqRunning = true;
    {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        app.seqErr.clear();
    }
    app.seqNote.clear();
    fprintf(stderr, "stack: %s - %d files (%s)\n", info.name.c_str(),
            (int)files.size(), isRaw ? "raw recipe" : isPicture ? "picture file" : "npy");
    const size_t startBytes = residentImageBytes();
    const size_t budget = seqMemBudget();
    const std::string refNote = ref->note;   // the reference frame's recipe, which
                                             // every frame that JOINS this stack
                                             // shares - see refW/refH/refCh below,
                                             // which is where "shares" stopped
                                             // being an assumption
    // docs/terminology.md: a stack is frames "同一条件で撮られた" with a time
    // axis, and sigma_t / FPN / the per-frame table are only quantities if every
    // frame is the same picture measured again. The grouper works on NAMES - it
    // has to, because .raw and .bin declare nothing and peeking every header of
    // a 300-file run would be a scan that reads the folder twice - so the shapes
    // are not known until the frames are decoded, and THIS is where they first
    // are. A sibling that is not this shape is refused by name, exactly as a
    // multi-page TIFF refuses a page that is not page 1's shape (core/tiffread.cpp).
    const int refW = ref->w, refH = ref->h, refCh = ref->ch;
    const std::string refName = baseName(ref->src->path);
    app.seqThread = std::thread([jobs, isRaw, isPicture, recipe, startBytes, budget, refNote,
                                 refW, refH, refCh, refName]() {
        size_t bytes = startBytes;
        int failures = 0;
        double lastPost = 0;
        for (const auto& j : jobs) {
            if (app.seqCancel) break;
            std::string err;
            std::unique_ptr<ImageDoc> doc;
            bool adopted = false;
            if (!recipe.cropOn) {   // §6.2: consult the registry BEFORE decoding.
                // The same file already resident (the same folder opened twice)
                // is shared, not decoded again - and stacks 0 new bytes on this
                // load (§7): the pixels were counted by the stack that decoded
                // them, and residentImageBytes dedupes by srcId.
                FrameSource probe;
                probe.path = j.path;
                if (isRaw) {
                    probe.rawDtype = recipe.dtype;   probe.rawInterp = recipe.interp;
                    probe.rawOffset = recipe.offset; probe.rawLE = recipe.littleEndian;
                    probe.srcW = recipe.w;           probe.srcH = recipe.h;
                } else {
                    // folder members decode natively: npyRead stays NR_NATIVE
                    // (0), which is what npyKeyRead maps every native decode
                    // to, shape known or not. A DECLARED-reading resident keys
                    // nr!=0, misses here and self-heals at the
                    // adopt-on-collision add below.
                }
                statSourceFile(probe);
                if (probe.mtime || probe.fsize) {
                    const std::string probeKey = srcIdentityKey(probe);   // IO:
                                                                          // before
                                                                          // the lock
                    // ONE continuous hold across find + syncMirrors: the reload
                    // swap and crop hold the same lock for their whole mutation,
                    // so these mirrors can never capture a half-swapped source
                    std::lock_guard<std::mutex> lk(g_srcRegMtx);
                    if (std::shared_ptr<FrameSource> hit =
                            srcRegistryFindLocked(probeKey)) {
                        doc = std::make_unique<ImageDoc>();
                        doc->src = std::move(hit);
                        doc->name = baseName(j.path);
                        doc->note = refNote;
                        doc->syncMirrors();   // vmin/vmax: already measured
                        adopted = true;
                    }
                }
            }
            if (!doc) {
                if (isRaw) {
                    RawDialog d = recipe;
                    d.path = j.path;
                    doc = decodeRawFrame(d, err);
                } else if (isPicture) {
                    doc = decodeImageFileFrame(j.path, err);
                } else {
                    doc = decodeNpy(j.path, err);
                }
            }
            // ...and here it is: the shape the file turned out to hold, against
            // the shape this stack IS. Turned into an ordinary decode failure
            // rather than a special case, so it travels the one path that already
            // says "n of N" - a stack that quietly took four different pictures
            // is worse than one that says which file it left out and why.
            if (doc) {
                std::string bad = stackShapeRefusal(doc->w, doc->h, doc->ch,
                                                    refW, refH, refCh, refName);
                if (!bad.empty()) { err = bad; doc.reset(); }
            }
            if (!doc) {
                if (++failures <= 3) {
                    std::lock_guard<std::mutex> lk(app.seqMtx);
                    app.seqErr += baseName(j.path) + ": " + err + "\n";
                }
                app.seqDone++;
                continue;
            }
            if (!adopted) {
                computeMinMax(*doc);  // pure: keep this off the UI thread (and
                                      // off shared sources: theirs is done) -
                                      // and BEFORE the add: no source in the
                                      // registry ever shows the default 0/1
                // the UI thread may have registered this very tuple between the
                // probe miss and now: the add hands back the live occupant and
                // this decode is discarded for it - both stacks of one tuple
                // end up sharing. One hold across add + syncMirrors, as above.
                const std::string addKey = srcIdentityKey(*doc->src);   // IO: before
                                                                        // the lock
                std::lock_guard<std::mutex> lk(g_srcRegMtx);
                if (std::shared_ptr<FrameSource> live = srcRegistryAddLocked(doc->src, addKey)) {
                    doc->src = std::move(live);
                    doc->syncMirrors();
                    adopted = true;   // 0 new bytes (§7): the pixels were
                                      // counted by the stack that decoded them
                } else {
                    bytes += doc->px().size() * sizeof(float);
                }
            }
            {
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.emplace_back(j.index, std::move(doc));
            }
            app.seqDone++;
            // wake the UI at most ~10 Hz: one post per decoded frame would defeat
            // the idle throttle on a fast disk
            double now = nowSec();
            if (now - lastPost > 0.1) { lastPost = now; glfwPostEmptyEvent(); }
            if (bytes > budget) {
                // say what stopped it, with the numbers: silently loading 60 of
                // 300 frames is how a measurement quietly becomes wrong
                char m[192];
                snprintf(m, sizeof m,
                         "memory budget %.1f GB reached - stopped after %d of %d frames\n"
                         "(File > Stack loading > Memory budget)\n",
                         budget / 1073741824.0, app.seqDone.load(), (int)jobs.size());
                fputs(m, stderr);
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqErr += m;
                break;
            }
        }
        app.seqRunning = false;
    });
}

// UI-thread integration of decoded frames (also owns GL/texture lifetime).
static void pumpSequence() {
    std::vector<std::pair<int, std::unique_ptr<ImageDoc>>> batch;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        // cap per UI frame: a fast decoder could otherwise hand us dozens of
        // 48 MB frames at once and stall the frame
        const size_t MAX_PER_FRAME = 4;
        if (app.seqReady.size() <= MAX_PER_FRAME) {
            batch.swap(app.seqReady);
        } else {
            batch.insert(batch.end(), std::make_move_iterator(app.seqReady.begin()),
                         std::make_move_iterator(app.seqReady.begin() + MAX_PER_FRAME));
            app.seqReady.erase(app.seqReady.begin(), app.seqReady.begin() + MAX_PER_FRAME);
        }
        err.swap(app.seqErr);
    }
    // A toast expires; "60 of 300 frames" must not. Keep it in the Files panel
    // until the next load, because a partially loaded stack silently produces
    // wrong temporal statistics.
    if (!err.empty()) { toast(err, true); app.seqNote = err; }
    if (batch.empty()) {
        if (!app.seqRunning && app.seqThread.joinable()) app.seqThread.join();
        return;
    }
    // inherit display settings from the sequence reference frame so frames are
    // directly comparable (same range, same colormap, same CFA interpretation)
    const ImageDoc* ref = nullptr;
    for (const auto& d : app.images)
        if (d->seqId == app.seqLoadingId) { ref = d.get(); break; }
    for (auto& pr : batch) {
        auto& doc = pr.second;
        doc->seqId = app.seqLoadingId;
        doc->seqIndex = pr.first;
        doc->syncMirrors();           // a reload striking while this frame sat
                                      // in seqReady moved the source under it,
                                      // and the reload walk covers only
                                      // app.images - the landing itself adopts
                                      // the source's CURRENT geometry
        if (doc->vmax <= doc->vmin) computeMinMax(*doc);   // worker normally did this
        if (ref) {
            doc->black = ref->black; doc->white = ref->white;
            doc->cfa = ref->cfa; doc->cfaPattern = ref->cfaPattern;
            doc->cfaColorize = ref->cfaColorize;
            doc->displayLut = ref->displayLut;
            // frame ⊂ stack ⊂ batch: every frame carries its stack's batch, or
            // "close batch" / "move to batch" sees only the head frame
            doc->batchId = ref->batchId;
        } else {
            defaultRange(*doc);
        }
        doc->texDirty = true;
        doc->uid = app.nextUid++;
        app.imagesRev++;
        app.images.push_back(std::move(doc));
    }
    // invalidating per pump made this O(frames^2) over a load; refresh at ~2 Hz
    static double lastTemporalInvalidate = 0;
    double nowT = nowSec();
    if (!app.seqRunning || nowT - lastTemporalInvalidate > 0.5) {
        lastTemporalInvalidate = nowT;
        app.temporal[0].seqId = app.temporal[1].seqId = -1;
    }
    bool drained;
    {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        drained = app.seqReady.empty();
    }
    if (!app.seqRunning && app.seqThread.joinable() && drained) {
        // count only after the queue drained: adoption makes the worker finish
        // while frames still sit in seqReady, and "loaded 5" of 6 is a lie
        app.seqThread.join();
        int n = 0;
        for (const auto& d : app.images) if (d->seqId == app.seqLoadingId) n++;
        fprintf(stderr, "stack: loaded %d frames\n", n);
    }
}

static void startNextQueuedGroup();   // defined with the folder-open code below

// Frames decoded but not yet integrated. They are stamped with seqLoadingId when
// they land, so the next stack must NOT start while any are still waiting - it
// would move seqLoadingId and file the tail of one stack under another.
static bool seqReadyPending() {
    std::lock_guard<std::mutex> lk(app.seqMtx);
    return !app.seqReady.empty();
}

// A queued group has produced its stack: remember WHICH, under the token the
// group was stamped with. Recorded only while a sweep is waiting - that is the
// only consumer, and it bounds the table to the groups of the Opens still in
// flight (256 apiece), which resolvePendingSeries then clears.
// seqId 0 = the load made a lone frame, not a stack: nothing to record, and the
// resolver counts that member as unmatched, which is the truth.
static void noteGroupStack(int token, int seqId) {
    if (!token || !seqId || app.seriesPending.empty()) return;
    for (auto& p : app.groupStacks)
        if (p.first == token) { p.second = seqId; return; }
    app.groupStacks.emplace_back(token, seqId);
}

// The stack a session's series member names, found by the path of its FIRST
// frame. 0 = that frame is not open (or is not part of a stack).
//
// preferBatch disambiguates the case the canon explicitly blesses: reopening
// the same folder makes a NEW batch, so one file is legitimately resident in two
// stacks at once. Taking "the first one" then hands every member of the second
// copy's series a stack in the wrong batch, strict containment rejects them all,
// and the whole series is lost - permanently, at the next autosave.
//
// taken excludes the stacks earlier members already claimed. preferBatch only
// separates stacks in DIFFERENT batches, and the canon recommends reaching the
// other arrangement too ("メンバを同じ batch へ移してから series を作る"): two
// same-path stacks inside ONE batch are told apart by nothing but this list.
//
// `member` is the other half of §6.2's identity: two image members of one .npz
// share a first-frame path and nothing else can tell them apart, so a member
// line that named one used to resolve onto whichever came first in list order.
// Empty means unconstrained - a session written before that key resolves by
// path exactly as it did then, and every non-container caller passes nothing.
static int seqIdOfFirstFramePath(const std::string& path, int preferBatch,
                                 const std::vector<int>* taken,
                                 const std::string& member) {
    auto claimed = [&](int sid) {
        return taken && std::find(taken->begin(), taken->end(), sid) != taken->end();
    };
    int any = 0;
    for (const auto& d : app.images) {
        if (d->seqId == 0 || d->src->path != path || claimed(d->seqId)) continue;
        if (!member.empty() && d->src->member != member) continue;
        if (preferBatch && d->batchId == preferBatch) return d->seqId;
        if (!any) any = d->seqId;
    }
    return any;
}

// The frame twin: the STANDALONE frame a "seriesframe" member names, by its
// own path. Same disambiguation shape as seqIdOfFirstFramePath - prefer the
// series' batch, never a doc an earlier member already claimed.
static uint64_t uidOfFramePath(const std::string& path, int preferBatch,
                               const std::vector<uint64_t>& taken,
                               const std::string& member = std::string()) {
    uint64_t any = 0;
    for (const auto& d : app.images) {
        if (d->seqId != 0 || d->preview || d->src->path != path) continue;
        if (!member.empty() && d->src->member != member) continue;   // ...and WHICH member
        if (std::find(taken.begin(), taken.end(), d->uid) != taken.end()) continue;
        if (preferBatch && d->batchId == preferBatch) return d->uid;
        if (!any) any = d->uid;
    }
    return any;
}

// Turn what the session file said into real series. Runs once, when every
// stack the file asked for exists - a member cannot be looked up before then.
// Members that cannot be found are COUNTED and reported: silently dropping
// points out of a measurement is the one thing this must not do.
static void resolveSeriesRestore() {
    int made = 0, lost = 0, lostSeries = 0, badValues = 0;
    // ONE claim list, across every member of every series in the file. Two
    // stacks legitimately hold the same first-frame path (open the same folder
    // twice, then Move one into the other batch - both moves the canon
    // recommends), and without this the second same-path member resolved onto
    // the stack the first had already taken. Inside one series that was a bare
    // `continue`: a member and its hand-typed value gone under a full-success
    // toast, made permanent by the next autosave. Across two series it was
    // worse - the later one STOLE the earlier one's stacks, pruneEmptySeries
    // deleted what was left of it, and `made` had already counted it.
    std::vector<int> taken;
    std::vector<uint64_t> takenU;   // frame members claim docs the same way
    for (const auto& R : app.seriesRestore) {
        badValues += R.badValues;
        lost += R.truncated;        // cut before its path: never became a member
        int bid = 0;
        for (const auto& b : app.batches) if (b.name == R.batchName) bid = b.id;
        if (!bid) { lostSeries++; lost += (int)R.members.size(); continue; }
        std::vector<App::Series::Member> ms;
        for (const auto& m : R.members) {
            if (m.kind == 2) {      // a standalone frame member (§3.3)
                uint64_t u = uidOfFramePath(m.path, bid, takenU, m.member);
                const ImageDoc* fd = u ? frameMemberDoc(u) : nullptr;
                // strict containment survives the round trip too, or not at all
                if (!fd || fd->batchId != bid) { lost++; continue; }
                takenU.push_back(u);
                App::Series::Member mm;
                mm.frameUid = u;
                mm.value = m.value;
                mm.include = m.include;
                ms.push_back(mm);
                continue;
            }
            int sid = seqIdOfFirstFramePath(m.path, bid, &taken, m.member);
            if (!sid || batchOfStack(sid) != bid) { lost++; continue; }
            taken.push_back(sid);
            ms.push_back({ sid, m.value, m.include });
            // the declared fold comes back as declared: a sum reloaded as a
            // mean would be a different quantity under the same member line
            if (m.kind == 1) ms.back().fold = FOLD_SUM;
        }
        if (ms.empty()) { lostSeries++; continue; }
        int id = newSeries(bid, R.name);
        // At most one series per stack. With `taken` spanning the whole file no
        // series restored here can hold another's stack, so this can now only
        // fire for a series that existed BEFORE the restore - which loadSession
        // makes impossible (closeAll) and which nothing else in the program
        // arranges.
        for (const auto& m : ms) {
            if (m.seqId != 0) {
                App::Series* other = seriesOfStack(m.seqId);
                if (other && other->id != id) removeFromSeries(m.seqId);
            } else {
                App::Series* other = seriesOfFrame(m.frameUid);
                if (other && other->id != id) removeFrameFromSeries(m.frameUid);
            }
        }
        if (App::Series* S = seriesById(id)) {
            S->paramName = R.paramName;
            snprintf(S->unit, sizeof S->unit, "%s", R.unit.c_str());
            S->kind = std::clamp(R.kind, 0, 3);
            S->members = std::move(ms);
            made++;
        }
    }
    app.seriesRestore.clear();
    pruneEmptySeries();
    if (!app.curSeriesId && !app.series.empty()) app.curSeriesId = app.series.front().id;
    if (made || lost || lostSeries || badValues) {
        std::string msg = "restored " + std::to_string(made) + " series";
        if (lost || lostSeries)
            msg += "; " + std::to_string(lost) + " member(s) and " +
                   std::to_string(lostSeries) + " series could not be resolved";
        // a value the file states but this program cannot read is left UNSET,
        // and that is a fit point fewer than the file claims - say so
        if (badValues)
            msg += "; " + std::to_string(badValues) +
                   " unreadable value(s) left unset (never 0)";
        toast(msg, lost > 0 || lostSeries > 0 || badValues > 0);
        fprintf(stderr, "series: %s\n", msg.c_str());
    }
}

// Old sessions that carried a per-stack "seqlevel": one series per BATCH that
// actually has two or more levelled stacks, named "<batch> 掃引", unit = the
// prefill, kind = linearity. A batch with fewer gets NOTHING - guessing a sweep
// out of the folder structure is precisely what the canon forbids.
static void migrateLegacyLevels() {
    std::vector<std::pair<int, double>> lvl;      // seqId -> level
    for (const auto& p : app.seqLevelLegacy) {
        int sid = seqIdOfFirstFramePath(p.first);
        if (!sid || seriesOfStack(sid)) continue;   // an explicit series wins
        bool dup = false;
        for (const auto& q : lvl) if (q.first == sid) dup = true;
        if (!dup) lvl.emplace_back(sid, p.second);
    }
    app.seqLevelLegacy.clear();
    std::vector<int> batches;
    for (const auto& q : lvl) {
        int b = batchOfStack(q.first);
        if (!b) continue;
        bool seen = false;
        for (int x : batches) if (x == b) seen = true;
        if (!seen) batches.push_back(b);
    }
    int made = 0;
    for (int b : batches) {
        int n = 0;
        for (const auto& q : lvl) if (batchOfStack(q.first) == b) n++;
        if (n < 2) continue;                        // not a sweep, do not invent one
        int id = newSeries(b, "");
        if (App::Series* S = seriesById(id)) {
            S->paramName = "level";
            snprintf(S->unit, sizeof S->unit, "%s", app.lin.unit);
            S->kind = App::Series::KLinearity;
        }
        for (const auto& q : lvl)
            if (batchOfStack(q.first) == b) addToSeries(id, q.first, q.second);
        made++;
    }
    pruneEmptySeries();
    if (!app.curSeriesId && !app.series.empty()) app.curSeriesId = app.series.front().id;
    if (made) {
        toast("migrated " + std::to_string(made) +
              " series from an old session's stack levels");
        fprintf(stderr, "series: migrated %d from seqlevel\n", made);
    }
}

// ---- AnalysisSet: the binds that had to wait (docs/reader-analysisset.md) ----
// A folder Ref (or a session's stack role) opens its head at once and its
// siblings through the queued rescan; the BIND lands here, at the same drain
// gate seriesRestore resolves at - and a bind that still cannot land becomes
// an unbound role with the reason, never a silent drop.
static void resolveASetBindPending() {
    std::vector<App::ASetBind> q;
    q.swap(app.aSetBindPending);
    int bound = 0, failed = 0;
    for (const auto& b : q) {
        App::ASet* S = aSetById(b.setId);
        if (!S || b.roleIdx < 0 || b.roleIdx >= (int)S->roles.size())
            continue;                        // the set was closed while forming
        App::ASetRole& r = S->roles[(size_t)b.roleIdx];
        if (b.kind == 1) {
            // a Ref deferred past the session replay: full §2.2 order, now
            // that everything the session loads is loaded. May queue a kind-0
            // bind of its own (a folder group) - the gate refires for it.
            r.reason.clear();
            resolveSetRef(r, b.setId, b.roleIdx, b.batchId, b.origin);
            if (r.bound()) {
                bound++;
            } else {
                // a folder group may have re-queued itself (kind 0): still
                // forming is not a failure yet - the gate refires for it
                const bool forming = !app.aSetBindPending.empty() &&
                                     app.aSetBindPending.back().setId == b.setId &&
                                     app.aSetBindPending.back().roleIdx == b.roleIdx;
                if (!forming) failed++;
            }
            continue;
        }
        int sid = seqIdOfFirstFramePath(b.headPath, b.batchId);
        if (sid) {
            r.seqId = sid;
            r.reason.clear();
            bound++;
        } else {
            r.reason = b.what + " did not open as a stack";
            failed++;
        }
    }
    if (bound || failed) {
        app.imagesRev++;
        if (failed)
            toast(std::to_string(failed) + " set binding(s) could not be made - "
                  "the roles say why", true);
    }
}

// The session's analysisset blocks, once every stack the file asked for
// exists. §5.2: a reader re-run has usually rebuilt the same set already -
// the block ADOPTS it on (setbatch, name) rather than making a second node
// (the same-name prohibition of §1.4 is what makes that key valid). Roles are
// the re-run's truth; differences are counted and said. A block whose name
// matches nothing, but whose reader origin left exactly ONE unclaimed set in
// that batch, is the same set RENAMED - the block owns the rename (the
// seqname-on-SeqRestore precedent). Only when the reader could not speak at
// all does the block rebuild the set from its own records, each role resolved
// in §2.2 order and the unresolvable ones left unbound with the reason.
static void resolveASetRestore() {
    int made = 0, adopted = 0, renamed = 0, roleDiff = 0;
    int lostSets = 0, unbound = 0, truncated = 0;
    std::vector<int> claimed;
    auto isClaimed = [&](int id) {
        return std::find(claimed.begin(), claimed.end(), id) != claimed.end();
    };
    std::vector<App::ASetRestore> blocks;
    blocks.swap(app.aSetRestore);
    for (const auto& R : blocks) {
        truncated += R.truncated;
        int bid = 0;
        for (const auto& b : app.batches) if (b.name == R.batchName) bid = b.id;
        if (!bid) { lostSets++; continue; }
        App::ASet* hit = nullptr;
        for (auto& S : app.analysisSets)
            if (S.batchId == bid && S.name == R.name && !isClaimed(S.id)) {
                hit = &S;
                break;
            }
        if (!hit && !R.origin.empty()) {
            App::ASet* cand = nullptr;
            int nc = 0;
            for (auto& S : app.analysisSets)
                if (S.batchId == bid && S.origin == R.origin && !isClaimed(S.id)) {
                    cand = &S;
                    nc++;
                }
            if (nc == 1) { hit = cand; hit->name = R.name; renamed++; }
        }
        if (hit) {
            claimed.push_back(hit->id);
            adopted++;
            // truth is the re-run's; a role the block remembers that the
            // re-run no longer has (the reader was edited) is COUNTED
            for (const auto& rr : R.roles) {
                bool found = false;
                for (const auto& sr : hit->roles)
                    if (sr.role == rr.role) { found = true; break; }
                if (!found) roleDiff++;
            }
            continue;
        }
        // the block alone (§5.2 last bullet): every role from the records,
        // resolved in §2.2 order
        App::ASet S;
        S.id = app.nextASetId++;
        S.batchId = bid;
        S.readerSpec = R.readerSpec;
        S.origin = R.origin;
        for (const auto& rr : R.roles) {
            App::ASetRole r;
            r.role = rr.role;
            if (rr.kind == "unbound") {
                r.reason = rr.reason.empty() ? std::string("unbound when saved")
                                             : rr.reason;
                unbound++;
            } else {
                int mbid = 0;      // the member's own batch, named by the block:
                for (const auto& b : app.batches)   // bindings cross batches (§1.4)
                    if (b.name == rr.batchName) mbid = b.id;
                if (mbid == 0) mbid = bid;
                if (rr.kind == "stack") {
                    aSetBindStackByHead(r, rr.key, S.id, (int)S.roles.size(), mbid);
                } else if (rr.kind == "series") {
                    for (const auto& s : app.series)
                        if (s.batchId == mbid && s.name == rr.key) r.seriesId = s.id;
                    if (r.seriesId == 0)
                        r.reason = "series \"" + rr.key + "\" not found in batch \"" +
                                   rr.batchName + "\"";
                } else {           // frame: a file path, §2.2 whole
                    r.refPath = rr.key;
                    resolveSetRef(r, S.id, (int)S.roles.size(), mbid, std::string());
                    r.refPath.clear();   // the record was the session's, not a Ref's
                }
                if (!r.bound() && r.reason.empty()) r.reason = rr.key + " not found";
            }
            S.roles.push_back(std::move(r));
        }
        S.name = aSetUniqueName(bid, R.name);
        claimed.push_back(S.id);
        app.analysisSets.push_back(std::move(S));
        made++;
        app.imagesRev++;
    }
    if (made || adopted || lostSets || roleDiff || truncated) {
        std::string msg = "restored " + std::to_string(made + adopted) + " set(s)";
        if (adopted)
            msg += " (" + std::to_string(adopted) + " adopted from the reader's re-run" +
                   (renamed ? ", " + std::to_string(renamed) + " renamed by the session"
                            : std::string()) + ")";
        if (roleDiff)
            msg += "; " + std::to_string(roleDiff) +
                   " recorded role(s) differ - the reader's are kept";
        if (lostSets)
            msg += "; " + std::to_string(lostSets) + " set(s) had no batch to restore into";
        if (truncated)
            msg += "; " + std::to_string(truncated) + " role line(s) were cut short";
        toast(msg, lostSets > 0 || truncated > 0);
        fprintf(stderr, "analysisset: %s\n", msg.c_str());
    }
    (void)unbound;
}

// The picker's "open as a sweep", once its stacks exist. Matching is by
// IDENTITY: each ticked group was stamped with a token, and the stack that
// group created was recorded under it at creation (noteGroupStack). A display
// name is not identity - Files renames a stack with F2 the moment its head
// frame lands, and a rename to another group's name used to hand that group's
// LEVEL to the wrong stack: a silently misplaced point on the axis the fit is
// taken against, which is worse than the loss it was reported as.
//
// The name is still accepted as a fallback for a tokenless entry (a session
// written by an older build, a group queued outside the picker).
//
// Nothing here invents anything: the values were read from the group names and
// shown in the picker before Load was pressed, and a group whose stack never
// opened is COUNTED and said out loud rather than dropped.
static void resolveOnePendingSeries(const App::SeriesPending& P) {
    std::vector<int> used;
    int id = 0, made = 0, lost = 0;
    auto free4 = [&](int sid) {
        return sid && seqInfo(sid) && batchOfStack(sid) == P.batchId &&
               std::find(used.begin(), used.end(), sid) == used.end();
    };
    for (const auto& e : P.members) {
        int seqId = 0;
        if (e.token)
            for (const auto& gs : app.groupStacks)
                if (gs.first == e.token && free4(gs.second)) { seqId = gs.second; break; }
        for (const auto& si : app.seqs) {
            if (seqId) break;
            if (!free4(si.id)) continue;
            // names no longer carry " [remote xN]" (C2), but sessions saved
            // before that change do - accept both directions of the mismatch
            auto bare = [](const std::string& n) {
                size_t t = n.rfind(" [remote x");
                return t == std::string::npos ? n : n.substr(0, t);
            };
            bool same = bare(si.name) == bare(e.name);
            if (same) { seqId = si.id; break; }
        }
        if (!seqId) { lost++; continue; }
        used.push_back(seqId);
        if (!id) {                       // the first member that resolves makes it
            id = newSeries(P.batchId, P.name);
            if (App::Series* S = seriesById(id)) {
                S->paramName = P.paramName;
                snprintf(S->unit, sizeof S->unit, "%s", P.unit.c_str());
                S->kind = P.kind;
            }
        }
        if (addToSeries(id, seqId, e.value)) made++;
        else lost++;
    }
    // In VALUE order, not the order the folders happened to sort in: the groups
    // arrive as 0,10,160,20,320,40,80 (lexicographic), and a sweep is a
    // parameter axis - reading it out of order is reading it wrong. Unset
    // values keep their relative order and go last (they are not "before 0").
    // The modal's arrows and "Sort by value" still own the order after this.
    if (App::Series* S = seriesById(id))
        std::stable_sort(S->members.begin(), S->members.end(),
                         [](const App::Series::Member& a, const App::Series::Member& b) {
                             bool fa = std::isfinite(a.value), fb = std::isfinite(b.value);
                             if (fa != fb) return fa;          // set before unset
                             return fa && a.value < b.value;
                         });
    pruneEmptySeries();
    if (id && seriesById(id)) app.curSeriesId = id;
    const App::Series* S = seriesById(id);
    std::string msg = "sweep: " + std::to_string(made) + " stack(s) in series \"" +
                      (S ? S->name : std::string("(none)")) + "\"";
    if (lost) msg += "; " + std::to_string(lost) + " could not be matched";
    toast(msg, lost > 0);
    fprintf(stderr, "series: %s\n", msg.c_str());
}
// Every sweep the picker was told to open, in the order it was told. They all
// wait for the same drain (a member cannot be looked up before its stack
// exists), so they all resolve here - none of them is thrown away because a
// second Open happened while the first was still loading.
static void resolvePendingSeries() {
    std::vector<App::SeriesPending> queue;
    queue.swap(app.seriesPending);
    for (const auto& P : queue) {
        // The batch a sweep was accepted into can be gone by now. closeBatch
        // purges its notes, but a raw dialog cancelled out of the queue, and
        // anything else that reaches pruneEmptyBatches, does not - and a note
        // about a batch that no longer exists has nothing to say to anybody.
        bool live = false;
        for (const auto& b : app.batches) if (b.id == P.batchId) live = true;
        if (live) resolveOnePendingSeries(P);
    }
    app.groupStacks.clear();          // the only thing that reads them is done
}

// called once per frame: integrate decoded frames, then chain the next stack
static void pumpStackAverages();   // fwd: it needs computeStackStats, defined below
static void pumpDetrendProducts(); // fwd: same, and it rides the same gate
static void pumpSequenceAndQueue() {
    pumpSequence();
    // one restore at a time, matched by uid: indices shift as frames land
    if (!app.seqRestore.empty() && !app.seqRunning && !seqReadyPending()) {
        App::SeqRestore r = std::move(app.seqRestore.front());
        app.seqRestore.erase(app.seqRestore.begin());
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->uid == r.uid && app.images[i]->seqId == 0) {
                startSequenceLoad(i, r.files, r.pattern);
                // the session's seqname (and per-frame x axis), applied now
                // that the stack exists
                if (app.images[i]->seqId != 0)
                    if (App::SeqInfo* si2 = seqInfo(app.images[i]->seqId)) {
                        if (!r.name.empty()) si2->name = r.name;
                        if (!r.axisVals.empty()) {
                            si2->axisName = r.axisName;
                            si2->axisUnit = r.axisUnit;
                            si2->axisVals = std::move(r.axisVals);
                        }
                    }
                break;
            }
        return;                 // let it start before chaining anything else
    }
    if (!app.seqRunning && !app.seqQueue.empty() && !seqReadyPending()) startNextQueuedGroup();
    // Series LAST, and only once everything a member could name is open: the
    // stacks arrive over many frames, and a lookup one frame too early would
    // report perfectly good members as missing.
    if ((!app.seriesRestore.empty() || !app.seqLevelLegacy.empty() ||
         !app.seriesPending.empty() || !app.aSetRestore.empty() ||
         !app.aSetBindPending.empty()) &&
        !app.seqRunning && app.seqQueue.empty() && app.seqRestore.empty() &&
        app.rbOpenQueue.empty() && !seqReadyPending()) {
        if (!app.seriesRestore.empty()) resolveSeriesRestore();
        if (!app.seqLevelLegacy.empty()) migrateLegacyLevels();
        if (!app.seriesPending.empty()) resolvePendingSeries();
        // Binds BEFORE blocks: a bind waiting here belongs to an earlier
        // drain, and its stack exists now. Blocks may queue NEW rescans and
        // new binds - those wait for the NEXT drain, and this gate refires
        // because aSetBindPending is nonempty (ras §5.2 / §2.2).
        if (!app.aSetBindPending.empty()) resolveASetBindPending();
        if (!app.aSetRestore.empty()) resolveASetRestore();
    }
    // Parked frame averages last, and behind the same "everything has landed"
    // gate: a mean taken while the stack is still filling would be a mean of
    // however many frames happened to have arrived by then. It rides here so
    // every selftest's drain loop drives it too, without a second pump to keep
    // in step.
    pumpStackAverages();
    // ...and the preprocessor products a session asked for, behind the same
    // gate and for the same reason: a surface fitted mid-load is a surface of
    // whatever had arrived (docs/analysis-layers.md §5).
    pumpDetrendProducts();
}

// ---- stacks & navigation ------------------------------------------------------
// image indices of one sequence, ordered by frame number
static std::vector<int> framesOfSeq(int seqId) {
    std::vector<std::pair<int, int>> v;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->seqId == seqId) v.emplace_back(app.images[i]->seqIndex, i);
    std::sort(v.begin(), v.end());
    std::vector<int> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}
// every stack in list order: a sequence is one stack, a lone image is its own
static std::vector<std::vector<int>> stacksOf() {
    std::vector<std::vector<int>> out;
    std::vector<int> seen;
    for (int i = 0; i < (int)app.images.size(); i++) {
        int sid = app.images[i]->seqId;
        if (sid == 0) { out.push_back({ i }); continue; }
        bool dup = false;
        for (int s : seen) if (s == sid) { dup = true; break; }
        if (dup) continue;
        seen.push_back(sid);
        out.push_back(framesOfSeq(sid));
    }
    return out;
}
// Cached view of the above: rebuilding it per frame cost one allocation per
// image plus a sort per sequence, just to draw a handful of rows.
static const std::vector<std::vector<int>>& stacksCached() {
    static std::vector<std::vector<int>> cache;
    static uint64_t rev = 0;
    if (rev != app.imagesRev) { rev = app.imagesRev; cache = stacksOf(); }
    return cache;
}
// THE rule for the pointer this returns, and the only one:
//
//     NEVER hold it across anything that can open or close a stack.
//     Ask again by id instead. It costs a walk over a handful of entries.
//
// It points INTO App::seqs, whose every mutation reseats the storage on purpose
// (App::SeqTable in core/app/state.h says why), so an add or a remove leaves it
// pointing at freed memory - always, on every platform, not only where the
// allocator happened to grow. "Anything that can" is the operative phrase: the
// call in between does not have to look like an open or a close. closeStack,
// closeAll, closeBatch, closeSeries, applyDerivePlan, promotePreview,
// openRemote, attachRemoteStack, startSequenceLoad, loadNpyBuffer,
// loadImageFile and the ~60 functions that reach one of them all qualify, and
// the list moves with the code - which is why the rule is stated over the
// pointer rather than as a list to check against.
//
// Copy out what you need (`const std::string name = si->name;`) when the value
// has to outlive the call. If you must assert, App::seqs.holds(p) is the only
// defined question to ask of a pointer that may already be stale.
static App::SeqInfo* seqInfo(int id) {
    for (auto& s : app.seqs) if (s.id == id) return &s;
    return nullptr;
}
// Is this reduced frame (frame average / frame sum) still describing its
// stack? docs/reference-design.md §10.1: the doc recorded (seqId, stackRev) at
// compute time; the reload walk latches avgStale at the moment it bumps the
// key - at the EVENT, so a stack that is reloaded and then closed cannot take
// the fact with it. The live comparison below is the backstop for a key that
// moved without the walk (a future bump site, a selftest driving stackRev
// directly). Stale never clears by itself: only Recompute - the user's move -
// renews the record, because the picture is never redrawn behind the user.
static bool avgIsStale(const ImageDoc* d) {
    if (!d || d->avgOfPath.empty()) return false;
    if (d->avgStale) return true;
    if (!d->avgSeqId) return false;
    const App::SeqInfo* si = seqInfo(d->avgSeqId);
    return si && si->stackRev != d->avgStackRev;
}
// ---- the mark a partial reload leaves on the STACK (issue #56) --------------
// The neighbour of avgIsStale above, and deliberately NOT the same state.
// (stale) is about a DERIVED picture whose stack moved on under it, and one
// Recompute makes it true again. This is about the stack ITSELF: after a
// reload that refused a member, its frames are no longer of one moment, and no
// recompute can mend that - only a re-read that succeeds everywhere. Same
// mechanism though, which is what docs/reference-design.md §10.3 asked for:
// latched at the event, never cleared silently, and read in the same places a
// report quotes from (title / Files row / tooltip / Inspector / TSV export).
static bool seqReloadIncomplete(const App::SeqInfo* si) {
    return si && si->reloadFailed > 0;
}
// The TOKEN, for the two surfaces that have room for a word and not a
// sentence: the Files row label and the window title. Leading spaces like
// (stale)'s, so it sits off the name. "" = this stack is of one moment.
// (seqId 0 - a loose frame - is no stack and always answers "".)
static const char* SEQ_RELOAD_FAIL_TOKEN = "(reload failed)";
static const char* seqReloadMark(int seqId) {
    return seqReloadIncomplete(seqInfo(seqId)) ? "  (reload failed)" : "";
}
// ...and the SENTENCE, for the three that do: the Files tooltip, the Inspector
// and the export's provenance block. One line by construction - the export
// writes it after a "# " and a tooltip is one string - and it is THE guard, so
// what --reload-selftest asserts is the wording the panels draw rather than a
// second opinion beside it (f32LossNote's discipline). "" = nothing to say.
static std::string seqReloadNote(int seqId) {
    const App::SeqInfo* si = seqInfo(seqId);
    if (!seqReloadIncomplete(si)) return {};
    // std::string, not snprintf into a fixed buffer: the refusal it quotes
    // carries a file name of no bounded length, and a sentence that explains a
    // wrong number must not be the one thing that gets cut off.
    const std::string when = si->reloadWhen.empty() ? std::string()
                                                    : " at " + si->reloadWhen;
    std::string s = std::string(SEQ_RELOAD_FAIL_TOKEN) + " ";
    if (si->reloadOk > 0)
        // the dangerous half: two generations under one name
        s += std::to_string(si->reloadOk) + " of " +
             std::to_string(si->reloadOk + si->reloadFailed) +
             " frame(s) were re-read and " + std::to_string(si->reloadFailed) +
             " refused" + when + " - this stack's frames are no longer of one "
             "moment, so sigma_t and every other stack number mix two generations.";
    else
        // nothing was replaced: one moment still, but not the asked-for one
        s += "no frame could be re-read - all " + std::to_string(si->reloadFailed) +
             " refused" + when + ". These are still the pixels loaded earlier, "
             "NOT what the disk now holds.";
    s += " First refusal: " + si->reloadFirstErr +
         ". Only a reload of this stack in which EVERY frame succeeds clears this.";
    return s;
}

// "Same picture" between two reduced frames is a TUPLE comparison, never a
// pixel comparison - §10.2 reuses §6.2's identity vocabulary instead of
// inventing a second generation concept. Two results of the same recipe over
// the same recorded inputs are the same picture; the registry never dedups
// computed results, so this is how the program can still SAY it.
static bool avgSamePicture(const ImageDoc* a, const ImageDoc* b) {
    return a && b && a != b && !a->avgOfPath.empty() &&
           a->avgOfPath == b->avgOfPath && a->avgOfMember == b->avgOfMember &&
           a->avgFold == b->avgFold &&
           a->avgSrcDigest != 0 && a->avgSrcDigest == b->avgSrcDigest &&
           a->avgSrcCount == b->avgSrcCount;
}
static void selectImage(int idx) {
    if (idx < 0 || idx >= (int)app.images.size()) return;
    // Walking A onto the image pinned as B used to SWAP them, so that looking
    // at B stopped it from being B and rewrote the pin to whatever A had been.
    // B is something the user pinned deliberately; navigating must not rewrite
    // it. Nothing is needed here: the pin is a uid and this function does not
    // touch it. Standing on B is not a state that needs handling either - both
    // sides simply resolve to the one document and are drawn coinciding, which
    // is a comparison people set up ON PURPOSE (see resolveB). Shift+\ (or the
    // status bar's "swap A/B") is how you swap - on purpose.
    ImageDoc* prev = cur();
    if (prev && app.images[idx].get() != prev) app.prevImageUid = prev->uid;
    // Held-down frame stepping: B's caches cost real milliseconds per step
    // (measured, see the A/B stats commits), so a key repeat would drag the
    // whole UI down for as long as the key is held. Two switches inside 300 ms
    // means "still stepping"; the B slots then hold their last result and say
    // so, and refresh once the stepping stops. Same shape as annBusy.
    {
        static double lastSwitch = -1e9;
        double now = nowSec();
        if (now - lastSwitch < 0.30) app.abStepBusyUntil = now + 0.30;
        lastSwitch = now;
    }
    app.current = idx;
    ImageDoc* d = app.images[idx].get();
    // Value-range scope. The stack default (frames inherit the reference
    // frame's range so they compare directly) is only ONE of three sensible
    // policies, and it silently defeated Auto while stepping frames:
    //   0 per frame  - every frame shows its own min..max
    //   1 per stack  - the inherited behavior (default, unchanged)
    //   2 everything - the range follows you across stacks too
    if (prev && d != prev) {
        // linked mode overlays one range non-destructively (effBlack/effWhite),
        // so only auto-per-frame touches the image's own numbers here
        if (app.rangeScope == 0 && !app.linkRange) { defaultRange(*d); d->texDirty = true; }
    }
    if (d->pendingViewScale != 1.0f) {   // its preview->full swap happened off screen
        app.view.zoom = std::max(app.view.zoom / d->pendingViewScale, 1.0f / 512);
        app.view.center.x *= d->pendingViewScale;
        app.view.center.y *= d->pendingViewScale;
        d->pendingViewScale = 1;
    }
    if (App::SeqInfo* si = seqInfo(d->seqId)) si->lastImageIdx = idx;
}
// time axis: previous / next frame of the current stack
static void gotoFrame(int delta, bool absoluteEdge = false, bool toFirst = true) {
    ImageDoc* im = cur();
    if (!im) return;
    if (im->seqId == 0) return;                       // lone image: no time axis
    std::vector<int> f = framesOfSeq(im->seqId);
    if (f.empty()) return;
    int pos = 0;
    for (int i = 0; i < (int)f.size(); i++) if (f[i] == app.current) pos = i;
    int np = absoluteEdge ? (toFirst ? 0 : (int)f.size() - 1)
                          : std::clamp(pos + delta, 0, (int)f.size() - 1);
    selectImage(f[np]);
}
// stack axis: previous / next stack, resuming its last viewed frame
static void gotoStack(int delta) {
    const auto& st = stacksCached();
    if (st.empty()) return;
    int cs = 0;
    for (int i = 0; i < (int)st.size(); i++)
        for (int idx : st[i]) if (idx == app.current) cs = i;
    int ns = std::clamp(cs + delta, 0, (int)st.size() - 1);
    if (ns == cs) return;
    const std::vector<int>& target = st[ns];
    int pick = target.front();
    if (App::SeqInfo* si = seqInfo(app.images[target.front()]->seqId))
        if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
            app.images[si->lastImageIdx]->seqId == app.images[target.front()]->seqId)
            pick = si->lastImageIdx;
    selectImage(pick);
}

// ---- open a whole folder tree: every numbered group becomes its own stack ----
// The headerless ones are listed here by name because nothing else can say what
// they are. The picture formats are NOT listed: they are asked of
// core/imagefile.h, so a format gained or lost there is gained or lost here
// with it, and this list cannot come to disagree with the file dialog's.
static const char* SEQ_EXTS[] = { ".npy", ".npz", ".bin", ".raw", ".yuv", ".dat", ".rggb" };
static bool isLoadableExt(const std::string& extLower) {
    for (const char* e : SEQ_EXTS) if (extLower == e) return true;
    return imagefile::forPath("x" + extLower) != nullptr;
}

// Walk root recursively; each directory yields one group per numeric pattern.
static std::vector<App::PendingGroup> scanFolderGroups(const std::string& root) {
    std::vector<App::PendingGroup> groups;
    std::error_code ec;
    std::filesystem::path rootPath = pathFromUtf8(root);
    if (!std::filesystem::is_directory(rootPath, ec)) return groups;
    const size_t MAX_GROUPS = 256;    // a truncated scan is reported, never silent
    // collect loadable files per directory (depth-limited walk)
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> perDir;
    auto addFile = [&](const std::filesystem::path& p) {
        std::string ext = p.extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!isLoadableExt(ext)) return;
        std::filesystem::path dir = p.parent_path();
        for (auto& d : perDir)
            if (d.first == dir) { d.second.push_back(p.u8string()); return; }
        perDir.push_back({ dir, { p.u8string() } });
    };
    // Manual breadth-first walk: recursive_directory_iterator aborts the whole
    // scan when a single entry cannot be read (a permission error or a dangling
    // link would silently truncate the folder list).
    {
        std::vector<std::pair<std::filesystem::path, int>> todo{ { rootPath, 0 } };
        while (!todo.empty()) {
            auto [dir, depth] = todo.back();
            todo.pop_back();
            std::error_code dec;
            std::filesystem::directory_iterator dit(dir, dec), dend;
            if (dec) continue;                       // unreadable dir: skip just this one
            for (; dit != dend; dit.increment(dec)) {
                if (dec) { dec.clear(); break; }
                std::error_code fec;
                if (dit->is_directory(fec)) {
                    if (depth < 3) todo.push_back({ dit->path(), depth + 1 });
                } else if (dit->is_regular_file(fec)) {
                    addFile(dit->path());
                }
            }
        }
    }
    std::sort(perDir.begin(), perDir.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string rootStr = rootPath.u8string();
    for (auto& d : perDir) {
        std::vector<std::string> files = d.second;
        std::sort(files.begin(), files.end());
        std::vector<bool> used(files.size(), false);
        // name the stack by its folder relative to the opened root
        std::string rel = d.first.u8string();
        if (rel.size() > rootStr.size()) rel = rel.substr(rootStr.size() + 1);
        else rel.clear();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        auto push = [&](std::vector<std::string> fs, std::string pattern) {
            if (groups.size() >= MAX_GROUPS) return;
            App::PendingGroup g;
            g.files = std::move(fs);
            g.name = rel.empty() ? pattern : rel + "/" + pattern;
            std::string ext = std::filesystem::u8path(g.files[0]).extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            // "raw" here means HEADERLESS: the user must state the geometry
            // before anything can be decoded. A .npy carries its own shape and
            // so does every picture format behind core/imagefile.h, so neither
            // may be sent to the RAW dialog - a folder of .exr would otherwise
            // open a width/height/dtype prompt for files that already say all
            // three.
            g.isRaw = ext != ".npy" && !imagefile::forPath(g.files[0]);
            groups.push_back(std::move(g));
        };
        std::vector<std::string> leftover;
        for (size_t i = 0; i < files.size(); i++) {
            if (used[i]) continue;
            std::string pattern;
            std::vector<std::string> sibs = findSequenceSiblings(files[i], pattern);
            if (sibs.size() >= 2) {
                for (const auto& s : sibs)
                    for (size_t k = 0; k < files.size(); k++)
                        if (files[k] == s) used[k] = true;
                push(std::move(sibs), pattern);
            } else {
                used[i] = true;
                leftover.push_back(files[i]);
            }
        }
        // Two or more unnumbered leftovers = ONE stack in natural order, same
        // fold the remote SCAN does: capture sets are not always numbered, and
        // N single-frame stacks from one folder is never what Open Folder
        // meant. Mixed extensions stay apart (raw needs one recipe per group).
        std::map<std::string, std::vector<std::string>> byExt;
        for (auto& f : leftover) {
            std::string ext = std::filesystem::u8path(f).extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            byExt[ext].push_back(std::move(f));
        }
        for (auto& [ext, fs] : byExt) {
            if (fs.size() >= 2) {
                std::vector<std::string> sorted = fs;
                sortFramesNumerically(sorted);
                push(std::move(sorted), "*" + ext);
            } else {
                push({ fs[0] }, baseName(fs[0]));
            }
        }
    }
    return groups;
}

// Start the next queued stack once the loader is idle. Raw stacks need format
// settings: the dialog is shown once and the recipe is reused for the rest.
static void startNextQueuedGroup() {
    // rawDlg.open now means "the RAW dialog is alive", and forQueue can only
    // be true while it is - so a lost dialog can no longer strand the queue
    if (app.seqQueue.empty() || app.seqRunning || rawDlg.open ||
        (rawDlg.forQueue && rawDlg.open) || app.folderPickOpen) return;
    App::PendingGroup g = app.seqQueue.front();
    // The recipe belongs to the OPEN that answered for it. Two raw folders with
    // different width/height/dtype/offset cannot both be right, and the queue
    // spans Opens: applying a latched recipe to another Open's files decodes
    // them as garbage, silently, whenever those files are big enough.
    const bool haveRecipe = app.folderRecipeValid && app.folderRecipeOpenId == g.openId;
    if (g.isRaw && !haveRecipe) {
        openRawDialogFor(g.files[0]);          // ask once per Open
        if (rawDlg.open) {
            rawDlg.forQueue = true;
            rawDlg.queueCount = 0;             // ...for THIS Open's stacks
            for (const auto& q : app.seqQueue) if (q.openId == g.openId) rawDlg.queueCount++;
        } else {
            app.seqQueue.erase(app.seqQueue.begin());   // unreadable: skip
        }
        return;
    }
    app.seqQueue.erase(app.seqQueue.begin());
    std::string err;
    g_quietLoad = true;               // keep the user's current image selected
    app.loadBatchId = g.batchId;      // the head frame lands in the group's batch
    if (g.isRaw) {
        RawDialog d = g_folderRecipe;
        d.path = g.files[0];
        d.replaceIdx = -1;
        err = loadRaw(d);
    } else if (imagefile::forPath(g.files[0])) {
        err = loadImageFile(g.files[0]);
    } else {
        err = loadNpy(g.files[0]);
    }
    g_quietLoad = false;
    app.loadBatchId = 0;
    if (!err.empty()) { toast(baseName(g.files[0]) + ": " + err, true); return; }
    // reference the image we just appended, not app.current (which may be elsewhere)
    int idx = (int)app.images.size() - 1;
    if (idx < 0) return;
    if (g.files.size() >= 2) {
        startSequenceLoad(idx, g.files, g.name);
        noteGroupStack(g.token, app.images[idx]->seqId);
        return;
    }
    // ONE file that carries a frame axis is a stack too, and loadNpy could only
    // name it after the file it came out of ("capture.npy  (8 frames)"). The
    // canon's default stack name is フォルダ/パターン and the group name IS that:
    // apply it here, where the stack is minted, exactly as startSequenceLoad
    // does for the multi-file case. Without this a sweep laid out as one
    // capture.npy per level opens as N stacks of ONE name, indistinguishable in
    // Files, and the only number the value proposal can read out of that name is
    // the frame count (seven levels all "8").
    if (App::SeqInfo* si = seqInfo(app.images[idx]->seqId)) si->name = g.name;
    noteGroupStack(g.token, app.images[idx]->seqId);
}

static void enqueueGroups(std::vector<App::PendingGroup> groups) {
    if (groups.empty()) return;
    // One Open, one openId. This used to be `app.folderRecipeValid = false`,
    // which is the wrong scope in both directions once the queue spans Opens:
    // opening raw folder B while raw folder A drained CANCELLED the recipe the
    // user had already given for A (so the dialog came back showing an A file
    // to someone who had just opened B), and the answer to that dialog was then
    // latched for every remaining group of BOTH folders.
    int oid = app.nextOpenId++;
    for (auto& g : groups) g.openId = oid;
    // APPEND. This used to be `app.seqQueue = std::move(groups)`, so a second
    // Open while the first was still loading silently CANCELLED every stack the
    // first Open had not started yet - the second Open's folder came up looking
    // complete while most of the first one was simply gone. startNextQueuedGroup
    // already refuses to start while one is running, so the queue just gets
    // longer. (Found while reproducing the series pending-slot finding: fixing
    // the slot alone still left the first sweep with one stack out of seven.)
    int added = (int)groups.size(), frames = 0;
    for (const auto& g : groups) frames += (int)g.files.size();
    app.seqQueue.insert(app.seqQueue.end(), std::make_move_iterator(groups.begin()),
                        std::make_move_iterator(groups.end()));
    toast("opening " + std::to_string(added) + " stack(s), " +
          std::to_string(frames) + " files");
    startNextQueuedGroup();
}

// Minimal glob: * (any run), ? (one char), case-insensitive, no character classes.
static bool globMatch(const char* pat, const char* str) {
    const char *star = nullptr, *ss = str;
    while (*str) {
        char p = *pat, s = *str;
        if (p == '?' || (p && tolower((unsigned char)p) == tolower((unsigned char)s))) { pat++; str++; continue; }
        if (p == '*') { star = pat++; ss = str; continue; }
        if (star) { pat = star + 1; str = ++ss; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}
// Comma-separated patterns; a bare word is treated as a substring (*word*).
// Used by the remote listing filter (the picker has its own live filter).
bool globListMatch(const char* list, const std::string& subject) {
    if (!list || !*list) return false;
    std::string cur;
    bool any = false;
    auto test = [&](std::string p) {
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
        while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
        if (p.empty()) return;
        if (p.find('*') == std::string::npos && p.find('?') == std::string::npos)
            p = "*" + p + "*";
        if (globMatch(p.c_str(), subject.c_str())) any = true;
    };
    for (const char* c = list; ; c++) {
        if (*c == ',' || *c == 0) { test(cur); cur.clear(); if (!*c) break; }
        else cur.push_back(*c);
    }
    return any;
}

// Read just enough of a .npy header for "24x1200x1600 u16": the picker shows it
// per group and the merge warning compares it. Never touches pixel data; any
// oddity returns "" (rendered as nothing, the pre-metadata behavior).
static std::string peekNpyShape(const std::string& path) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return {};
    uint8_t h[12] = {};
    f.read((char*)h, 12);
    if (f.gcount() < 10 || h[0] != 0x93 || memcmp(h + 1, "NUMPY", 5) != 0) return {};
    size_t hlen, hoff;
    if (h[6] >= 2) { uint32_t v; memcpy(&v, h + 8, 4); hlen = v; hoff = 12; }
    else           { uint16_t v; memcpy(&v, h + 8, 2); hlen = v; hoff = 10; }
    if (hlen == 0 || hlen > 65536) return {};
    std::string hdr(hlen, '\0');
    f.seekg((std::streamoff)hoff);
    f.read(&hdr[0], (std::streamsize)hlen);
    if ((size_t)f.gcount() != hlen) return {};
    size_t k = hdr.find("'descr'");
    if (k == std::string::npos) return {};
    size_t q1 = hdr.find('\'', hdr.find(':', k));
    if (q1 == std::string::npos) return {};
    size_t q2 = hdr.find('\'', q1 + 1);
    if (q2 == std::string::npos) return {};
    std::string code = hdr.substr(q1 + 1, q2 - q1 - 1);
    if (!code.empty() && (code[0] == '<' || code[0] == '>' || code[0] == '|' || code[0] == '='))
        code = code.substr(1);
    static const struct { const char* np; const char* name; } DT[] = {
        { "u1", "u8" }, { "i1", "i8" }, { "b1", "bool" }, { "u2", "u16" },
        { "i2", "i16" }, { "u4", "u32" }, { "i4", "i32" }, { "f4", "f32" }, { "f8", "f64" },
    };
    for (const auto& d : DT) if (code == d.np) { code = d.name; break; }
    size_t sp = hdr.find("'shape'");
    size_t p1 = hdr.find('(', sp), p2 = hdr.find(')', sp);
    if (sp == std::string::npos || p1 == std::string::npos || p2 == std::string::npos) return {};
    std::string dims;
    for (size_t i = p1 + 1; i < p2; i++) {
        char c = hdr[i];
        if (c >= '0' && c <= '9') dims.push_back(c);
        else if (c == ',' && !dims.empty() && dims.back() != 'x') dims.push_back('x');
    }
    while (!dims.empty() && dims.back() == 'x') dims.pop_back();
    if (dims.empty()) return {};
    return dims + " " + code;
}

// The picker's ONE live filter, applied as the user types. Space-separated
// terms AND together; a term matches if it appears ANYWHERE in a file's
// "folder/name" relative to the scanned root (case-insensitive, * and ?
// wildcards allowed); a '!' prefix turns a term into an exclusion. This cuts
// each group's FILE list - accepted groups load only the surviving files - and
// never touches the checkboxes: the filter narrows what is visible, the
// checkboxes choose which groups open.
static void applyPickFilter() {
    std::vector<std::string> inc, exc;
    {
        std::string t;
        auto flush = [&]() {
            if (t.empty()) return;
            if (t[0] == '!') { if (t.size() > 1) exc.push_back(t.substr(1)); }
            else inc.push_back(t);
            t.clear();
        };
        for (const char* c = app.pickFilter; ; c++) {
            if (*c == 0 || *c == ' ' || *c == '\t') { flush(); if (!*c) break; }
            else t.push_back(*c);
        }
    }
    auto hit = [](const std::string& term, const std::string& subj) {
        std::string p = "*" + term + "*";      // match anywhere: "frame_0??" must
        return globMatch(p.c_str(), subj.c_str());   // not need to cover ".npy"
    };
    for (auto& e : app.folderPick) {
        e.match.assign(e.g.files.size(), 1);
        e.nMatch = 0;
        for (size_t i = 0; i < e.rel.size() && i < e.match.size(); i++) {
            bool ok = true;
            for (const auto& t : inc) if (!hit(t, e.rel[i])) { ok = false; break; }
            if (ok) for (const auto& t : exc) if (hit(t, e.rel[i])) { ok = false; break; }
            e.match[i] = ok ? 1 : 0;
            if (ok) e.nMatch++;
        }
    }
}

// Turn scan results into an open picker: per-file relative paths for the
// filter, npy shapes for the merge warning, fresh filter and load mode.
// stripRoot is the scanned directory (local path or remote dir); displayRoot
// is what the dialog's first line shows (local path or ssh:// url).
static void openPickerWith(std::vector<App::PendingGroup> groups,
                           const std::string& displayRoot, const std::string& stripRoot,
                           bool remoteMode, const std::string& host, int port = 0) {
    std::string rootN = stripRoot;
    std::replace(rootN.begin(), rootN.end(), '\\', '/');
    while (!rootN.empty() && rootN.back() == '/') rootN.pop_back();
    auto ieq = [](const std::string& a, const std::string& b, size_t n) {
        for (size_t i = 0; i < n; i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    };
    app.folderPick.clear();
    for (auto& g : groups) {
        App::FolderPick e;
        e.g = std::move(g);
        if (e.g.shape.empty() && !e.g.isRaw && !remoteMode &&
            e.g.files[0].size() > 4 &&
            e.g.files[0].compare(e.g.files[0].size() - 4, 4, ".npy") == 0)
            e.g.shape = peekNpyShape(e.g.files[0]);
        e.rel.reserve(e.g.files.size());
        for (const auto& f : e.g.files) {
            std::string r = f;
            std::replace(r.begin(), r.end(), '\\', '/');
            if (r.size() > rootN.size() + 1 && r[rootN.size()] == '/' &&
                ieq(r, rootN, rootN.size()))
                r = r.substr(rootN.size() + 1);
            else
                r = baseName(r);
            e.rel.push_back(std::move(r));
        }
        app.folderPick.push_back(std::move(e));
    }
    app.folderPickRoot = displayRoot;
    app.folderPickRemote = remoteMode;     // a stale remote flag would misroute these
    app.folderPickHost = host;
    app.folderPickPort = port;
    app.pickFilter[0] = 0;                 // a leftover filter would silently cut
    app.pickMerge = 0;                     // the new scan - start every scan clean
    app.pickBatchMode = 0;                 // batch layout too: one batch is the canon
    // ...and the sweep, which is the one that could invent a measurement: a
    // parameter name and a unit left over from the last Open would be applied to
    // a folder that has nothing to do with it, and "単位を仮定しない" means not
    // assuming the PREVIOUS one either.
    app.pickSweep = false;
    app.pickSweepParam[0] = '\0';
    app.pickSweepUnit[0] = '\0';
    {   // batch names stem from the scanned root's leaf ("batchset", "scanroot")
        std::string leaf = rootN;
        size_t sl = leaf.find_last_of('/');
        if (sl != std::string::npos && sl + 1 < leaf.size()) leaf = leaf.substr(sl + 1);
        app.folderPickBatchBase = leaf.empty() ? "opened" : leaf;
    }
    applyPickFilter();
    app.folderPickOpen = true;
}

static void openFolder(const std::string& path) {
    std::vector<App::PendingGroup> groups = scanFolderGroups(path);
    if (groups.empty()) { toast("no loadable files under " + baseName(path), true); return; }
    if (groups.size() >= 256)
        toast("scan stopped at 256 stacks - narrow the folder or use the filter", true);
    // ALWAYS ask, one group included - same rule the remote scan follows. The
    // picker is where the file filter lives (UC3: "open only the *_dark*
    // frames of this folder" needs the dialog even when the folder groups into
    // a single sequence), and "Always load folder" no longer mutes it: that
    // setting keeps its original job - loading a single FILE's numbered
    // siblings without asking. Headless callers auto-accept via pickerAccept().
    // Batches are created at ACCEPT time (pickerAccept), not here: creating one
    // per scan left an empty batch behind on Cancel, and the picker's batch
    // mode (one / per top folder) is only known once the user answers.
    openPickerWith(std::move(groups), path, path, false, "");
}

// Stack name for a merged load: the common prefix of the merged group names,
// trimmed of separator/placeholder debris; the root folder's name when the
// prefix says nothing (three characters or less).
static std::string mergedStackName(const std::vector<App::PendingGroup>& sel) {
    std::string p = sel.empty() ? std::string() : sel[0].name;
    for (const auto& g : sel) {
        size_t n = 0;
        while (n < p.size() && n < g.name.size() &&
               tolower((unsigned char)p[n]) == tolower((unsigned char)g.name[n])) n++;
        p.resize(n);
    }
    while (!p.empty() && strchr("?*_-/. ", p.back())) p.pop_back();
    if (p.size() <= 3) p = baseName(app.folderPickRoot);
    return p + " (merged)";
}

// The picker's answer: selected groups with their file lists cut to the live
// filter (UC3), folded into ONE group when merge mode is on (UC2). Shared by
// the Load button and every headless auto-accept, so the selftests exercise
// exactly what the button does. Returns empty and sets err for a merge the
// loader could not honor (raw and npy need different decode paths).
static std::vector<App::PendingGroup> pickerSelection(std::string* errOut = nullptr) {
    std::vector<App::PendingGroup> sel;
    for (const auto& e : app.folderPick) {
        if (!e.selected || e.nMatch == 0) continue;
        App::PendingGroup g;
        g.name = e.g.name; g.isRaw = e.g.isRaw; g.batchId = e.g.batchId; g.shape = e.g.shape;
        for (size_t i = 0; i < e.g.files.size(); i++)
            if (i < e.match.size() && e.match[i]) g.files.push_back(e.g.files[i]);
        sel.push_back(std::move(g));
    }
    if (app.pickMerge == 1 && !sel.empty()) {
        bool anyRaw = false, anyNpy = false;
        for (const auto& g : sel) (g.isRaw ? anyRaw : anyNpy) = true;
        if (anyRaw && anyNpy) {
            if (errOut) *errOut = "cannot merge raw and npy files into one stack";
            return {};
        }
        App::PendingGroup m;
        m.isRaw = anyRaw;
        m.batchId = sel[0].batchId;
        m.shape = sel[0].shape;
        m.name = mergedStackName(sel);
        for (auto& g : sel)
            m.files.insert(m.files.end(), std::make_move_iterator(g.files.begin()),
                           std::make_move_iterator(g.files.end()));
        sortFramesNumerically(m.files);    // natural order across the union
        sel.clear();
        sel.push_back(std::move(m));
    }
    return sel;
}

// Selected groups that will not become a STACK, and so cannot become a series
// member: ONE file holding ONE 2-D array, or one raw frame. loadNpy calls
// addImage for those with no SeqInfo at all, and resolveOnePendingSeries walks
// app.seqs - a lone frame is structurally invisible to it. The picker previews a
// value for such a row like any other and the user used to find out at the far
// end of the load, as "N could not be matched".
//
// Shape comes from the npy header peek (local) or the v3 listing (remote); when
// it is unknown nothing is claimed. nMatch is the live filter's cut, which is
// what actually loads - a numbered group filtered down to one file lands here
// too, and that was the wider half of the hole.
static int pickLoneImageGroups() {
    // Remote is different: openRemoteStack mints a real SeqInfo for a single
    // remote frame ("<group> [remote x1]"), so a lone remote row DOES become a
    // stack and DOES resolve into the sweep. Warning about it would be wrong.
    if (app.folderPickRemote) return 0;
    int n = 0;
    for (const auto& e : app.folderPick) {
        if (!e.selected || e.nMatch != 1) continue;
        if (e.g.isRaw) { n++; continue; }        // a raw file is one frame, always
        int dims = e.g.shape.empty() ? 0 : 1;    // "8x32x32 f32" -> 3, "480x640 f32" -> 2
        for (char c : e.g.shape) {
            if (c == ' ') break;
            if (c == 'x') dims++;
        }
        if (dims == 2) n++;
    }
    return n;
}

// Close the picker and open its selection through the route the scan came from.
// No ImGui calls: the headless selftests and the auto-accept blocks in main()
// go through this exact function.
static void pickerAccept() {
    std::string err;
    std::vector<App::PendingGroup> sel = pickerSelection(&err);
    bool remote = app.folderPickRemote;
    std::string host = app.folderPickHost;
    // "as a sweep" is exclusive with batch-per-top-folder HERE, not only in the
    // dialog: a series lives in one batch, and that has to hold for every
    // caller, headless ones included.
    bool sweep = app.pickSweep && app.pickMerge == 0;
    int batchMode = (app.pickMerge == 1 || sweep) ? 0 : app.pickBatchMode;
    std::string swParam = app.pickSweepParam, swUnit = app.pickSweepUnit;
    app.pickSweep = false;            // a per-Open choice, never sticky
    std::string batchBase = app.folderPickBatchBase;
    app.folderPick.clear();
    app.folderPickOpen = false;
    if (sel.empty()) { toast(err.empty() ? "nothing selected" : err, true); return; }
    // Batches are created HERE, on accept - Cancel used to leave an empty one
    // behind. Mode 0 (default): the whole Open is one batch named for the root.
    // Mode 1: one batch per TOP folder of the group name, "root/00" style, so
    // per-condition folders open as ready-made analysis groupings.
    if (batchMode == 1) {
        std::vector<std::pair<std::string, int>> made;   // top folder -> batchId
        for (auto& g : sel) {
            size_t sl = g.name.find('/');
            std::string top = sl == std::string::npos ? std::string() : g.name.substr(0, sl);
            int id = 0;
            for (const auto& m : made) if (m.first == top) id = m.second;
            if (!id) {
                id = newBatch(uniqueBatchName(top.empty() ? batchBase
                                                          : batchBase + "/" + top));
                made.emplace_back(top, id);
            }
            g.batchId = id;
        }
    } else {
        int id = newBatch(uniqueBatchName(batchBase));
        for (auto& g : sel) g.batchId = id;
    }
    // The sweep is only a NOTE here - the stacks do not exist yet, and half of
    // them will arrive over the next several seconds. Built before sel is moved
    // away below; two groups minimum, because one point is not a sweep.
    if (sweep && sel.size() >= 2) {
        App::SeriesPending P;
        P.batchId = sel.front().batchId;
        P.paramName = swParam;
        P.unit = swUnit;                       // may be empty: then no fit, and
        P.kind = App::Series::KLinearity;      // the panel says exactly that
        // Stamp each ticked group with an identity NOW - before it is queued -
        // and carry it on the pending member. The stack does not exist yet; the
        // token is the handle to the one this group will make, whatever it ends
        // up being called by the time everything has drained.
        for (auto& g : sel) {
            g.token = app.nextGroupToken++;
            P.members.push_back({ g.token, g.name, extractLevelFromName(g.name) });
        }
        app.seriesPending.push_back(std::move(P));   // QUEUED, never overwritten
    }
    if (remote) {
        // remote groups: through the open queue, one stack at a time, so the
        // memory budget is applied against reality
        int frames = 0;
        for (const auto& g : sel) frames += (int)g.files.size();
        toast("opening " + std::to_string(sel.size()) + " remote stack(s), " +
              std::to_string(frames) + " frames");
        for (auto& g : sel) {
            App::RemoteOpen ro;
            ro.host = host; ro.files = std::move(g.files); ro.name = g.name;
            ro.batchId = g.batchId; ro.port = app.folderPickPort; ro.token = g.token;
            app.rbOpenQueue.push_back(std::move(ro));
        }
    } else {
        enqueueGroups(std::move(sel));
    }
}

// ---- the .npz member picker's answer (docs/npz-design.md §2.3) --------------
// The same shape as pickerAccept above, one layer down: no ImGui, so the
// headless selftests and every auto-accept go through this exact function.
//
// The file is the BATCH (frame ⊂ stack ⊂ series ⊂ batch: an .npz is "what was
// opened together", which is precisely a batch and nothing more). Members of
// different shapes coexist inside it as separate stacks - that is the whole
// point of a container.
static bool applyFrameAxisVals(int seqId, const std::string& name, const std::string& unit,
                               std::vector<double> vals, std::string& err);   // fwd
static std::string trimAxisLabel(const std::string& s);                       // fwd
// The next .npz that was dropped while this picker was up. Draining here and on
// Cancel is what makes the queue a queue rather than a place files go to die.
static void npzPickNext() {
    if (app.npzPickOpen || app.npzPickQueue.empty()) return;
    std::string next = app.npzPickQueue.front();
    app.npzPickQueue.erase(app.npzPickQueue.begin());
    std::string err = loadNpz(next);
    if (!err.empty()) toast(baseName(next) + ": " + err, true);
}
static void npzPickAccept() {
    std::vector<NpzMember> mem = std::move(app.npzPick);
    std::string path = app.npzPickPath;
    // trimmed HERE too, so the message says the unit that was actually stored
    std::string axName = trimAxisLabel(app.npzAxisName), axUnit = trimAxisLabel(app.npzAxisUnit);
    app.npzPick.clear();
    app.npzPickOpen = false;
    app.npzPickPath.clear();
    if (path.empty() || mem.empty()) return;
    // identity BEFORE the bytes (§6.2), same as loadNpz: the dialog may have
    // stood open across an overwrite, and the members below bind to THIS stat
    FrameSource zipStat;
    zipStat.path = path;
    statSourceFile(zipStat);
    std::vector<uint8_t> zip;
    std::vector<NpzEntry> entries;
    std::string err;
    if (!readFileBytes(path, zip) || !npzList(zip, entries, err)) {
        toast(baseName(path) + ": " + (err.empty() ? std::string("cannot read file") : err), true);
        return;
    }
    npzRememberMeta(path, mem);
    int prevBatch = app.loadBatchId;
    app.loadBatchId = newBatch(uniqueBatchName(baseName(path)));
    std::vector<std::string> openedNames;
    std::vector<int> openedSeqs;
    int stacks = 0, stackFrames = 0, loneFrames = 0;
    for (const auto& m : mem) {
        if (!m.selected || m.role != NpzMember::RImage) continue;
        // by NAME, not by the index the scan recorded: the file could have been
        // rewritten while the dialog stood open, and opening whatever now sits
        // at that index under the old member's name is the quiet lie again
        size_t at = entries.size();
        for (size_t k = 0; k < entries.size(); k++)
            if (entries[k].name == m.name + ".npy") { at = k; break; }
        if (at == entries.size()) {
            toast(baseName(path) + ": " + m.name + " is no longer in this file", true);
            continue;
        }
        size_t before = app.images.size();
        std::string oErr = npzOpenMember(path, zip, entries[at], m.name,
                                         0 /*NR_NATIVE*/, zipStat);
        if (!oErr.empty()) { toast(baseName(path) + ":" + m.name + ": " + oErr, true); continue; }
        openedNames.push_back(m.name);
        int sid = 0;
        for (size_t k = before; k < app.images.size(); k++)
            if (app.images[k]->seqId) sid = app.images[k]->seqId;
        if (sid) { stacks++; stackFrames += (int)framesOfSeq(sid).size(); openedSeqs.push_back(sid); }
        else loneFrames += (int)(app.images.size() - before);
    }
    app.loadBatchId = prevBatch;
    // ...and the axis, through the SAME call the Temporal panel's pasted axis
    // makes. The values came out of the file at full precision; the NAME and the
    // UNIT came from the human, which is the only way either of them is allowed
    // to be set (a key called "exposure_ms" does not prove milliseconds).
    std::string axisNote;
    for (auto& m : mem) {
        if (!m.selected || m.role != NpzMember::RAxis) continue;
        int applied = 0;
        std::string lastErr;
        for (int sid : openedSeqs) {
            if (framesOfSeq(sid).size() != m.vals.size()) continue;
            std::string aerr;
            if (applyFrameAxisVals(sid, axName.empty() ? m.name : axName, axUnit,
                                   m.vals, aerr)) applied++;
            else lastErr = aerr;
        }
        if (applied) {
            axisNote = "x axis " + (axName.empty() ? m.name : axName) + " [" + axUnit +
                       "] from " + m.name + " on " + std::to_string(applied) + " stack(s)";
            // it is still not an image, and the summary still names it - but it
            // says what the member BECAME rather than what it was declined for
            m.becomes = "x axis [" + axUnit + "]";
        } else {
            axisNote = "x axis " + m.name + " NOT applied: " +
                       (lastErr.empty() ? "no opened stack has " +
                                          std::to_string(m.vals.size()) + " frames" : lastErr);
        }
        break;                     // one axis per Open: one name, one unit
    }
    // The batch is minted before it is known whether anything will open, so a
    // file whose every member failed left an empty heading standing in Files -
    // exactly the "lie about what is open" that made batches be created at
    // ACCEPT time in the first place.
    if (openedNames.empty()) pruneEmptyBatches();
    std::string say = npzSummary(path, mem, openedNames, stacks, stackFrames,
                                 loneFrames, axisNote);
    fprintf(stderr, "npz: %s\n", say.c_str());
    toast(say, stacks == 0 && loneFrames == 0);
    npzPickNext();                 // whatever was dropped alongside this file
}

// Tree of what the scan found. One live filter narrows FILES as you type;
// checkboxes choose groups; the footer picks between "one stack per group"
// and "everything as ONE stack". Group count is capped at 256 by the scans, so
// the tree needs no clipper - the per-group FILE lists (unbounded) get one.
static void drawFolderPickModal() {
    // Not just "my popup is not open" but "no popup is open": two root-level
    // modals that each reopen themselves every frame evict each other inside a
    // SINGLE frame (drag-drop a numbered .npy while the picker is up), and both
    // windows are then submitted on top of one another every frame. Whoever
    // loses simply opens when the winner closes.
    if (app.folderPickOpen && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        // "remote=1" was true of the code path and false of the world: a local
        // browse takes it too. Name the SOURCE instead - that is the thing a
        // reader of this log is trying to establish.
        fprintf(stderr, "picker: OpenPopup (source=%s, %d groups)\n",
                !app.folderPickRemote ? "disk"
                : app.folderPickHost.empty() ? "peer on " PEER_HERE
                                             : app.folderPickHost.c_str(),
                (int)app.folderPick.size());
        ImGui::OpenPopup("Select stacks");
    }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * app.uiScale, 520 * app.uiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, ImGui::GetFontSize() * 18),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal("Select stacks", nullptr)) return;

    ImGui::TextDisabled("%s", dispPath(app.folderPickRoot).c_str());
    int totalFiles = 0, matchFiles = 0, selGroups = 0, selFiles = 0;
    for (const auto& e : app.folderPick) {
        totalFiles += (int)e.g.files.size();
        matchFiles += e.nMatch;
        if (e.nMatch == 0) continue;               // fully filtered out: not loadable
        if (e.selected) { selGroups++; selFiles += e.nMatch; }
    }

    // ONE live filter, no Apply button: each keystroke re-cuts the tree below.
    {
        float countW = ImGui::CalcTextSize("00000 / 00000 files match").x;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - countW -
                                         ImGui::GetStyle().ItemSpacing.x,
                                         ImGui::GetFontSize() * 8));
        if (ImGui::InputTextWithHint("##pickfilter", "filter files - e.g. dark  !ng  *_A.npy",
                                     app.pickFilter, sizeof app.pickFilter))
            applyPickFilter();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "Narrows WHICH FILES will load, as you type.\n"
                "Each word is looked for anywhere in a file's \"folder/filename\"\n"
                "(shown when you expand a stack below). All words must match.\n"
                "    dark          keep files whose folder or name contains \"dark\"\n"
                "    !ng           drop files matching \"ng\"\n"
                "    10lx *_A.npy  words combine; * and ? wildcards work anywhere\n"
                "Stacks show \"kept / total\"; only the kept files are loaded.");
        ImGui::SameLine();
        if (app.pickFilter[0]) ImGui::Text("%d / %d files match", matchFiles, totalFiles);
        else ImGui::TextDisabled("%d stack(s), %d files", (int)app.folderPick.size(), totalFiles);
    }
    if (ImGui::SmallButton("All")) { for (auto& e : app.folderPick) if (e.nMatch) e.selected = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) { for (auto& e : app.folderPick) if (e.nMatch) e.selected = false; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Invert")) { for (auto& e : app.folderPick) if (e.nMatch) e.selected = !e.selected; }
    ImGui::Separator();

    // footer: mode row + status/warning line(s) + button row, measured not guessed
    std::vector<std::string> shapes;               // distinct known shapes in selection
    bool mixedRawNpy = false;
    {
        bool anyRaw = false, anyNpy = false;
        for (const auto& e : app.folderPick) {
            if (!e.selected || e.nMatch == 0) continue;
            (e.g.isRaw ? anyRaw : anyNpy) = true;
            if (e.g.shape.empty()) continue;
            bool dup = false;
            for (const auto& s : shapes) if (s == e.g.shape) { dup = true; break; }
            if (!dup) shapes.push_back(e.g.shape);
        }
        mixedRawNpy = anyRaw && anyNpy;
    }
    bool mergeWarn = app.pickMerge == 1 && (mixedRawNpy || shapes.size() > 1);
    // The sweep row exists only where a sweep could: separate stacks, two or
    // more of them. Hidden, the box is FORCED OFF - a control nobody can see
    // must not be able to create a series (docs/terminology.md: never auto).
    const bool sweepRow = app.pickMerge == 0 && selGroups >= 2;
    if (!sweepRow) app.pickSweep = false;
    float footer = ImGui::GetFrameHeightWithSpacing() * (sweepRow ? 4 : 3) +
                   ImGui::GetTextLineHeightWithSpacing() *
                       ((mergeWarn ? 2 : 1) + (app.pickSweep ? 2 : 0));
    ImGui::BeginChild("picktree", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    // group the flat list by the folder part of "folder/pattern"
    std::vector<std::string> folders;
    auto folderOf = [](const App::FolderPick& e) {
        size_t s = e.g.name.find_last_of('/');
        return s == std::string::npos ? std::string("(root)") : e.g.name.substr(0, s);
    };
    for (const auto& e : app.folderPick) {
        if (e.nMatch == 0) continue;               // the filter emptied it: hide
        std::string f = folderOf(e);
        bool dup = false;
        for (const auto& x : folders) if (x == f) { dup = true; break; }
        if (!dup) folders.push_back(f);
    }
    if (folders.empty())
        ImGui::TextDisabled("no files match \"%s\"", app.pickFilter);
    for (const auto& f : folders) {
        ImGui::PushID(f.c_str());
        bool all = true, any = false;
        int files = 0;
        for (const auto& e : app.folderPick) {
            if (e.nMatch == 0 || folderOf(e) != f) continue;
            files += e.nMatch;
            e.selected ? (any = true) : (all = false);
        }
        bool parent = all;
        if (ImGui::Checkbox("##folder", &parent)) {
            for (auto& e : app.folderPick)
                if (e.nMatch > 0 && folderOf(e) == f) e.selected = parent;
        }
        ImGui::SameLine();
        if (!all && any) ImGui::TextDisabled("~");     // partial selection marker
        else ImGui::TextDisabled(" ");
        ImGui::SameLine();
        char hdr[320];
        snprintf(hdr, sizeof hdr, "%s   (%d files)", f.c_str(), files);
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (auto& e : app.folderPick) {
                if (e.nMatch == 0 || folderOf(e) != f) continue;
                size_t s = e.g.name.find_last_of('/');
                std::string leaf = s == std::string::npos ? e.g.name : e.g.name.substr(s + 1);
                ImGui::PushID(&e);
                ImGui::Checkbox("##sel", &e.selected);
                ImGui::SameLine();
                // expandable: the files behind the pattern, so "what does the
                // filter keep?" has a visible answer. Label text goes through
                // TextUnformatted ('#' and '?' in names stay visible).
                bool open = ImGui::TreeNodeEx("##files", ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGui::SameLine();
                char lb[352];
                if (e.nMatch == (int)e.g.files.size())
                    snprintf(lb, sizeof lb, "%s   %d file(s)%s%s%s", leaf.c_str(), e.nMatch,
                             e.g.isRaw ? "  [raw]" : "",
                             e.g.shape.empty() ? "" : "  ", e.g.shape.c_str());
                else
                    snprintf(lb, sizeof lb, "%s   %d / %d files%s%s%s", leaf.c_str(), e.nMatch,
                             (int)e.g.files.size(), e.g.isRaw ? "  [raw]" : "",
                             e.g.shape.empty() ? "" : "  ", e.g.shape.c_str());
                ImGui::TextUnformatted(lb);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.g.files.front().c_str());
                // the sweep preview: the value this group's NAME suggests, on
                // the row it was read from, BEFORE Load is pressed. A proposal
                // nobody looked at is not a confirmation.
                if (app.pickSweep && e.selected) {
                    std::string from;
                    double v = extractLevelFromName(e.g.name, &from);
                    ImGui::SameLine();
                    if (std::isfinite(v))   // a number per NOTHING says so, here too
                        ImGui::TextDisabled("-> %.6g %s", v,
                                            app.pickSweepUnit[0] ? app.pickSweepUnit
                                                                 : "[unit not set]");
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "-> no value in the name");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(std::isfinite(v)
                            ? "read from \"%s\" - correct it later in Edit series..."
                            : "nothing numeric in \"%s\": this stack joins UNSET,\n"
                              "and an unset value never enters a fit as 0.",
                            from.empty() ? e.g.name.c_str() : from.c_str());
                }
                if (open) {
                    std::vector<int> vis;          // files the filter keeps
                    vis.reserve(e.nMatch);
                    for (int i = 0; i < (int)e.g.files.size(); i++)
                        if (i < (int)e.match.size() && e.match[i]) vis.push_back(i);
                    ImGuiListClipper cl;           // thousands of frames stay cheap
                    cl.Begin((int)vis.size());
                    while (cl.Step())
                        for (int row = cl.DisplayStart; row < cl.DisplayEnd; row++) {
                            ImGui::TextDisabled("%s", e.rel[vis[row]].c_str());
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", e.g.files[vis[row]].c_str());
                        }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // load mode: UC1 (one stack per sequence) vs UC2/UC3 (everything as ONE stack)
    {
        char m0[64], m1[64];
        snprintf(m0, sizeof m0, "%d separate stack(s)###mode0", selGroups);
        snprintf(m1, sizeof m1, "ONE stack of %d file(s)###mode1", selFiles);
        ImGui::RadioButton(m0, &app.pickMerge, 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("each checked group of numbered files becomes its own stack");
        ImGui::SameLine();
        ImGui::RadioButton(m1, &app.pickMerge, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ALL checked files merge into a single stack,\n"
                              "frames in natural (numeric) order - for a capture\n"
                              "split across folders, or a filtered subset");
    }
    // batch row: how the accepted selection is GROUPED in the Files panel.
    // One batch per Open is the canon's default; per-top-folder splits a root
    // of condition folders into ready-made analysis groupings. Merge wins over
    // this (a single merged stack is one batch by construction).
    {
        int topFolders = 0;
        {
            std::vector<std::string> tops;
            for (const auto& e : app.folderPick) {
                if (!e.selected || e.nMatch == 0) continue;
                size_t sl = e.g.name.find('/');
                std::string top = sl == std::string::npos ? std::string()
                                                          : e.g.name.substr(0, sl);
                bool dup = false;
                for (const auto& t : tops) if (t == top) { dup = true; break; }
                if (!dup) tops.push_back(top);
            }
            topFolders = (int)tops.size();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("batch:");
        ImGui::SameLine();
        ImGui::BeginDisabled(app.pickMerge == 1 || app.pickSweep);
        ImGui::RadioButton("one batch###batch0", &app.pickBatchMode, 0);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("everything from this Open lands under ONE Files heading");
        ImGui::SameLine();
        char b1[80];
        snprintf(b1, sizeof b1, "%d batches (one per top folder)###batch1", topFolders);
        ImGui::RadioButton(b1, &app.pickBatchMode, 1);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(app.pickSweep
                ? "not while \"open as a sweep\" is on: a series lives in ONE\n"
                  "batch, so one batch is the only grouping that can hold it"
                : "each top-level folder of the scanned root becomes\n"
                  "its own batch (its own Files heading)");
        ImGui::EndDisabled();
    }
    // ---- row 3: open as a sweep (docs/series-plan.md §5) --------------------
    // The one place in the program that creates a series without the modal, and
    // it does it because a human ticked a box that says so. Unticked, NOTHING
    // is created - a sweep guessed from a folder tree is a lie that fits.
    if (sweepRow) {
        ImGui::Checkbox("open as a sweep (creates a series)", &app.pickSweep);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The %d selected stacks become one SERIES: each stack keeps\n"
                              "the value read from its name, and linearity / PTC measure the\n"
                              "series as a whole.\n"
                              "Nothing is created unless this is ticked.", selGroups);
        if (app.pickSweep) {
            app.pickBatchMode = 0;              // series ⊂ batch, strictly
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
            ImGui::InputTextWithHint("##swparam", "parameter", app.pickSweepParam,
                                     sizeof app.pickSweepParam);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("what was swept: illuminance, exposure, temperature ...");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
            ImGui::InputTextWithHint("##swunit", "unit", app.pickSweepUnit,
                                     sizeof app.pickSweepUnit);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("lx, ms, photons/px ... LEAVE IT EMPTY if you do not\n"
                                  "know yet: an unset unit means no fit, which is the\n"
                                  "honest answer, and Edit series... sets it later.");
        }
    }
    if (app.pickSweep) {
        int valued = 0;
        for (const auto& e : app.folderPick)
            if (e.selected && e.nMatch && std::isfinite(extractLevelFromName(e.g.name)))
                valued++;
        if (valued < selGroups)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1),
                               "%d of %d have a value in their name; the rest join UNSET "
                               "(never 0)", valued, selGroups);
        else if (!app.pickSweepUnit[0])
            ImGui::TextDisabled("all %d have a value. No unit yet - the series opens, "
                                "the fit waits for one.", valued);
        else
            ImGui::TextDisabled("all %d have a value, in %s", valued, app.pickSweepUnit);
        // A row that cannot become a stack cannot become a member. Said HERE,
        // while the box can still be unticked and the selection changed, rather
        // than at the far end of the load as "N could not be matched". The line
        // is always drawn (blank when there are none) so the footer's reserved
        // height does not move as the filter is typed.
        int lone = pickLoneImageGroups();
        if (lone)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1),
                               "%d of them is ONE 2-D frame: a lone frame is not a stack, "
                               "so it cannot join the series", lone);
        else
            ImGui::NewLine();
    }
    bool loadable = selGroups > 0 && !(app.pickMerge == 1 && mixedRawNpy);
    if (app.pickMerge == 1 && mixedRawNpy)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "cannot merge raw and npy files into one stack");
    else if (app.pickMerge == 1 && shapes.size() > 1)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "shapes differ (%s vs %s) - mismatched frames are skipped at load",
                           shapes[0].c_str(), shapes[1].c_str());
    else
        ImGui::Text("selected: %d stack(s), %d files", selGroups, selFiles);
    if (mergeWarn && app.pickMerge == 1 && shapes.size() > 1 && !mixedRawNpy)
        ImGui::TextDisabled("check the shape column above to see which stacks differ");
    ImGui::BeginDisabled(!loadable);
    bool load = ImGui::Button(app.pickMerge == 1 ? "Load as ONE stack"
                              : app.pickSweep   ? "Load as a sweep"
                                                : "Load selected",
                              ImVec2(150 * app.uiScale, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) {
        app.folderPick.clear();
        app.folderPickOpen = false;
        app.pickSweep = false;         // Cancel decides nothing, here least of all
        ImGui::CloseCurrentPopup();
    }
    if (load) {
        ImGui::CloseCurrentPopup();
        pickerAccept();                // same call the headless selftests make
    }
    ImGui::EndPopup();
}

// ---- the Reader panel (docs/input-adapters.md §4.13.0) -----------------------
// Three entrances, one destination, and IT DOES NOT CLOSE. Writing an adapter is
// a loop - write, load, read the failure, fix, load again - and the first design
// here was a modal, which cut that loop every time the author went to their
// editor. A panel that stays put lets the loop run: press Load, alt-tab, fix the
// line the traceback named, press Load again.
static void openReaderPicker(const std::string& path, const std::string& why) {
    app.readerPickPath = path;
    app.readerPickWhy = why;
    app.readerPanelOpen = true;
    app.readerPanelRaise = true;
    // The previous run belonged to the previous file: showing its traceback
    // beside a new path would blame this file for that file's problem.
    app.readerRan = false;
    app.readerLastOut.clear();
    std::string spec = readerFor(path);
    if (!spec.empty()) {
        snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s", readerFileOf(spec).c_str());
        snprintf(app.readerPickFunc, sizeof app.readerPickFunc, "%s", readerFuncOf(spec).c_str());
    } else if (app.readerPickFile[0] == '\0' && !app.readerMemo.empty()) {
        // Nothing chosen for this path yet: start from the one most recently
        // used. Applying a reader to several files in a row is the common case,
        // and retyping the same path each time is the friction that shape has.
        snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s",
                 readerFileOf(app.readerMemo.front().spec).c_str());
        snprintf(app.readerPickFunc, sizeof app.readerPickFunc, "%s",
                 readerFuncOf(app.readerMemo.front().spec).c_str());
    }
}

// (The adapters that ship with the viewer are no longer a list of their own:
//  they are the last place on the search path, and readerSearchPath() in
//  session.inc is the one thing that knows where they are - §4.13.2.)

// Write the starting template to `dest` and open it. Every failure here used to
// come out as "could not write the template to <dest>", which names the
// destination even when the destination is fine and the SOURCE is what is
// missing - the case that actually happens, because a packaged build ships no
// tools/import at all. Say which of the three things went wrong.
static void newReaderFrom(const std::string& dest) {
    if (dest.empty()) return;
    std::string script = runAdapterScript();
    if (script.empty()) {
        toast("no reader harness found: this build has no tools/import/run_adapter.py "
              "beside it, so readers cannot run at all", true);
        return;
    }
    std::filesystem::path tmpl = pathFromUtf8(script).parent_path() / "adapters" / "template.py";
    std::error_code ec;
    if (!std::filesystem::exists(tmpl, ec)) {
        toast("the template is missing: " + tmpl.u8string(), true);
        return;
    }
    if (!std::filesystem::copy_file(tmpl, pathFromUtf8(dest),
                                    std::filesystem::copy_options::overwrite_existing, ec)) {
        toast("could not write " + dest + ": " +
              (ec ? ec.message() : std::string("copy refused without saying why")), true);
        return;
    }
    snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s", dest.c_str());
    snprintf(app.readerPickFunc, sizeof app.readerPickFunc, "load");
    std::string how;
    if (adapter::openInEditor(dest, how)) toast("opened in " + how + ": " + baseName(dest));
    else toast(how, true);
}

static void drawReaderPanel() {
    if (!app.readerPanelOpen) return;
    if (app.readerPanelRaise) {                 // an entrance asked for it
        ImGui::SetNextWindowFocus();
        app.readerPanelRaise = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 42, ImGui::GetFontSize() * 30),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Reader", &app.readerPanelOpen)) { ImGui::End(); return; }
    if (app.readerPickPath.empty()) {
        ImGui::TextDisabled("No file yet. Open one that the viewer cannot read, or use\n"
                            "File > Open With a Reader...");
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", app.readerPickPath.c_str());
    std::error_code dec;
    if (std::filesystem::is_directory(pathFromUtf8(app.readerPickPath), dec))
        ImGui::TextDisabled("this is a folder - a reader may take one (§4.1)");
    if (!app.readerPickWhy.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 38);
        ImGui::TextColored(ImVec4(1, 0.65f, 0.4f, 1), "%s", app.readerPickWhy.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    ImGui::TextDisabled("A reader is a Python function you write. It is handed this "
                        "path and returns pixels, or Frame / Stack / Series / Batch.");

    // §4.13.2: the places, in search order, and what is in each. ONE list,
    // because "where does the viewer look?" and "what can I pick?" are the same
    // question - they used to be two half-answers (a shipped tree and one local
    // folder), which is how a person with readers in two places ended up with
    // nowhere to say so.
    const std::vector<ReaderPlaceView>& places = readerPlacesForUi();
    // First-match-wins, made visible: the winner of a name is the first place
    // that has it, so anything later carrying that name is marked as shadowed
    // rather than quietly losing.
    std::map<std::string, std::string> firstOf;
    for (const auto& v : places)
        for (const auto& f : v.readers)
            firstOf.emplace(baseName(f), f);
    int nreg = (int)app.readerPlaces.size();
    if (ImGui::TreeNodeEx("##places", ImGuiTreeNodeFlags_SpanAvailWidth |
                                      ImGuiTreeNodeFlags_DefaultOpen,
                          "where readers are looked for (%d registered)", nreg)) {
        if (ImGui::SmallButton("Add folder...") && !app.rdFolderDlg) {
            app.rdFolderMode = 0;
            app.rdFolderDlg = std::make_unique<pfd::select_folder>("Folder of readers", ".");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add file...") && !app.rdOpenDlg) {
            app.rdOpenMode = 2;
            app.rdOpenDlg = std::make_unique<pfd::open_file>(
                "Reader to register (.py)", ".", std::vector<std::string>{ "Python", "*.py" });
        }
        ImGui::SameLine();
        ImGui::TextDisabled("searched in this order; the first match wins");
        int drop = -1, up = -1, down = -1;
        for (size_t i = 0; i < places.size(); i++) {
            const ReaderPlace& p = places[i].place;
            ImGui::PushID((int)i);
            if (!p.shipped) {
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton("^")) up = (int)i;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 >= (size_t)nreg);
                if (ImGui::SmallButton("v")) down = (int)i;
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) drop = (int)i;
                ImGui::SameLine();
            }
            // A place that has gone away is SAID, in the colour the refusals
            // use, not dropped: the user put it there, and a share that is
            // offline this morning is back this afternoon.
            char lbl[64];
            if (!p.exists)    snprintf(lbl, sizeof lbl, "MISSING");
            else if (p.isDir) snprintf(lbl, sizeof lbl, "folder, %d reader(s)", p.readers);
            else              snprintf(lbl, sizeof lbl, "file");
            if (ImGui::TreeNodeEx("##pl", ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%s%s)",
                                  dispPath(p.path).c_str(), lbl,
                                  p.shipped ? ", ships with the viewer" : "")) {
                if (!p.exists)
                    ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1),
                                       "registered, but not there now");
                for (const auto& f : places[i].readers) {
                    auto w = firstOf.find(baseName(f));
                    bool shadowed = w != firstOf.end() && w->second != f;
                    if (ImGui::Selectable(baseName(f).c_str()))
                        snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s", f.c_str());
                    if (shadowed) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(shadowed by %s)", dispPath(w->second).c_str());
                    }
                }
                if (p.exists && p.isDir && p.readers == 0)
                    ImGui::TextDisabled("no .py files in that folder");
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (places.empty())
            ImGui::TextDisabled("nowhere yet - register the folder your readers are in");
        if (up >= 0)   moveReaderPlace((size_t)up, -1);
        if (down >= 0) moveReaderPlace((size_t)down, +1);
        if (drop >= 0) unregisterReaderPlace((size_t)drop);
        ImGui::TreePop();
    }
    // What has been used before: applying one reader to a run of files is the
    // common case, so the last choices are one click away rather than retyped.
    if (!app.readerMemo.empty() &&
        ImGui::TreeNodeEx("##used", ImGuiTreeNodeFlags_SpanAvailWidth,
                          "used before (%d)", (int)app.readerMemo.size())) {
        std::vector<std::string> seen;
        for (const auto& m : app.readerMemo) {
            bool dup = false;
            for (const auto& s : seen) if (s == m.spec) dup = true;
            if (dup) continue;
            seen.push_back(m.spec);
            if (ImGui::Selectable(m.spec.c_str())) {
                snprintf(app.readerPickFile, sizeof app.readerPickFile, "%s",
                         readerFileOf(m.spec).c_str());
                snprintf(app.readerPickFunc, sizeof app.readerPickFunc, "%s",
                         readerFuncOf(m.spec).c_str());
            }
        }
        ImGui::TreePop();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
    ImGui::InputText("file", app.readerPickFile, sizeof app.readerPickFile);
    ImGui::SameLine();
    if (ImGui::Button("Browse...") && !app.rdOpenDlg) {
        app.rdOpenMode = 0;
        app.rdOpenDlg = std::make_unique<pfd::open_file>(
            "Reader (.py)", ".", std::vector<std::string>{ "Python", "*.py" });
    }
    // The functions the file actually defines. Read out of the text, so the list
    // is what is there rather than what someone hoped was there.
    std::vector<std::string> funcs = adapter::moduleFunctions(app.readerPickFile);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputText("function", app.readerPickFunc, sizeof app.readerPickFunc);
    if (!funcs.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##funcs", "defined", ImGuiComboFlags_NoPreview |
                                                    ImGuiComboFlags_PopupAlignLeft)) {
            for (const auto& f : funcs)
                if (ImGui::Selectable(f.c_str()))
                    snprintf(app.readerPickFunc, sizeof app.readerPickFunc, "%s", f.c_str());
            ImGui::EndCombo();
        }
    }

    // §4.13: no built-in editor. New writes the template out and opens it; Edit
    // opens the one named above. $EDITOR, then `code -g`, then the OS.
    if (ImGui::Button("New reader...") && !app.rdNewDlg) {
        app.rdNewDlg = std::make_unique<pfd::save_file>(
            "New reader", "reader.py", std::vector<std::string>{ "Python", "*.py" });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(app.readerPickFile[0] == '\0');
    if (ImGui::Button("Edit reader")) {
        std::string how;
        if (adapter::openInEditor(app.readerPickFile, how))
            toast("opened in " + how + ": " + baseName(app.readerPickFile));
        else toast(how, true);
    }
    ImGui::EndDisabled();

    std::string spec = std::string(app.readerPickFile) + ":" + app.readerPickFunc;
    bool haveFile = app.readerPickFile[0] != '\0' && app.readerPickFunc[0] != '\0';
    ImGui::Separator();
    // §4.13: show the EXACT command before running it, the first time this
    // reader is used. Not a paraphrase - an external program is about to start.
    if (haveFile && readerNeedsConfirming(spec)) {
        ImGui::TextDisabled("this will run:");
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 38);
        ImGui::TextWrapped("%s", readerCommandLine(app.readerPickPath, spec).c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!haveFile || app.rdJob != nullptr);
    // §4.13.0: pressable as many times as you like. The cache key carries the
    // adapter's mtime, so editing the file and pressing this again ALWAYS
    // re-runs it - a cache that defeated the edit loop would kill the feature.
    if (ImGui::Button("Load", ImVec2(ImGui::GetFontSize() * 8, 0))) {
        readerConfirmed(spec);
        // Started, not waited for. The panel stays open - applying a reader to
        // several files in a row is the common case, and this is also where the
        // traceback lands - and the window keeps answering while Python runs.
        startReader(app.readerPickPath, spec);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (app.rdJob) {
        // Something turning, how long it has been turning, and a way out. A
        // reader takes as long as the user's data takes, and a window that only
        // stops answering does not say whether it is working or wedged.
        double el = nowSec() - app.rdJob->startedAt;
        const char* spin = "|/-\\";
        ImGui::Text("%c  running  %.1f s", spin[(int)(nowSec() * 8) & 3], el);
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) {
            app.rdJob->cancel.store(true);
            toast("stopping the reader...");
        }
    } else {
        ImGui::TextDisabled("re-runs whenever the reader file has changed");
    }

    // ---- what the last run said, WHOLE -------------------------------------
    // The only debugging surface an adapter author has. A traceback folded into
    // "could not open" would make this feature useless to the people it is for,
    // so it is shown in full, selectable, and scrolls rather than truncating.
    ImGui::Separator();
    if (!app.readerRan) {
        ImGui::TextDisabled("no run yet");
    } else {
        if (app.readerLastOk)
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "%s read it",
                               app.readerLastSpec.c_str());
        else
            ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s refused it",
                               app.readerLastSpec.c_str());
        ImGui::SameLine();
        // Say it copied. Without this a Copy that worked and a Copy that had
        // nothing to copy look identical, and the user reports the button as
        // broken - which is how the empty-panel defect above was found.
        ImGui::BeginDisabled(app.readerLastOut.empty());
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(app.readerLastOut.c_str());
            toast("copied");
        }
        ImGui::EndDisabled();
        ImGui::BeginChild("##readerout", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        // Unwrapped and monospaced-ish: a traceback's indentation is how you
        // read which frame is which, and wrapping destroys it.
        ImGui::TextUnformatted(app.readerLastOut.c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}

// §4.12: visible and removable. "Why does this one file open differently?" has
// to have an answer that does not involve reading prefs.txt in a text editor.
static void drawReaderList() {
    if (!app.readerListOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, ImGui::GetFontSize() * 20),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Readers", &app.readerListOpen)) {
        ImGui::TextDisabled("Files this viewer opens with a reader instead of natively. "
                            "%d of %d remembered (oldest is dropped first).",
                            (int)app.readerMemo.size(), (int)READER_MEMO_MAX);
        std::string why;
        std::string py = adapter::findPython(app.pythonExe, why);
        ImGui::TextDisabled("python: %s", py.empty() ? why.c_str() : py.c_str());
        // §4.13.2: the same list the refusal prints. "Where does it look?" is
        // asked here as often as "which file opens differently", and answering
        // it only inside an error message means you have to fail to find out.
        if (ImGui::TreeNodeEx("##where", ImGuiTreeNodeFlags_SpanAvailWidth,
                              "where readers are looked for (%d registered)",
                              (int)app.readerPlaces.size())) {
            // The snapshot, not a live walk: this is drawn every frame while the
            // node is open, and one of these places can be a share whose stat()
            // blocks for seconds. Same rule as the Reader panel.
            std::vector<ReaderPlace> pv;
            for (const auto& v : readerPlacesForUi()) pv.push_back(v.place);
            ImGui::TextUnformatted(readerPlacesReport(pv).c_str());
            ImGui::TextDisabled("add and reorder them in the Reader panel");
            ImGui::TreePop();
        }
        ImGui::Separator();
        if (app.readerMemo.empty()) {
            ImGui::TextDisabled("nothing remembered yet");
        }
        int remove = -1;
        for (size_t i = 0; i < app.readerMemo.size(); i++) {
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Forget")) remove = (int)i;
            ImGui::SameLine();
            ImGui::Text("%s", app.readerMemo[i].spec.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", app.readerMemo[i].path.c_str());
            ImGui::PopID();
        }
        if (remove >= 0) {
            app.readerMemo.erase(app.readerMemo.begin() + remove);
            savePrefs();
        }
    }
    ImGui::End();
}

static void drawNpzPickModal() {
    if (app.npzPickOpen && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        fprintf(stderr, "npz picker: OpenPopup (%s, %d members)\n",
                baseName(app.npzPickPath).c_str(), (int)app.npzPick.size());
        ImGui::OpenPopup("Select arrays");
    }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * app.uiScale, 420 * app.uiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, ImGui::GetFontSize() * 14),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal("Select arrays", nullptr)) return;

    ImGui::TextDisabled("%s", dispPath(app.npzPickPath).c_str());
    int selImages = 0, nImages = 0, selFrames = 0, axisRow = -1;
    for (size_t i = 0; i < app.npzPick.size(); i++) {
        const NpzMember& m = app.npzPick[i];
        if (m.role == NpzMember::RImage) {
            nImages++;
            if (m.selected) { selImages++; selFrames += m.frames; }
        } else if (m.role == NpzMember::RAxis && m.selected) axisRow = (int)i;
    }
    // does the ticked axis have a ticked stack of its length to belong to?
    // the unit as it will actually be STORED: " " is not a unit
    std::string axUnitNow = trimAxisLabel(app.npzAxisUnit);
    bool axisFits = false;
    if (axisRow >= 0)
        for (const auto& m : app.npzPick)
            if (m.role == NpzMember::RImage && m.selected && m.frames > 1 &&
                (size_t)m.frames == app.npzPick[axisRow].vals.size()) axisFits = true;
    ImGui::TextDisabled("%d member(s), %d of them image-shaped",
                        (int)app.npzPick.size(), nImages);
    if (ImGui::SmallButton("All")) {
        for (auto& m : app.npzPick) if (m.role == NpzMember::RImage) m.selected = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) {
        for (auto& m : app.npzPick) if (m.role == NpzMember::RImage) m.selected = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Invert")) {
        for (auto& m : app.npzPick) if (m.role == NpzMember::RImage) m.selected = !m.selected;
    }
    ImGui::Separator();

    float footer = ImGui::GetFrameHeightWithSpacing() * (axisRow >= 0 ? 3 : 2) +
                   ImGui::GetTextLineHeightWithSpacing() * 2;
    ImGui::BeginChild("npztree", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("npzmembers", 4,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("array", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("shape", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("dtype", ImGuiTableColumnFlags_WidthStretch, 0.12f);
        ImGui::TableSetupColumn("becomes", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < app.npzPick.size(); i++) {
            NpzMember& m = app.npzPick[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableSetColumnIndex(0);
            bool pickable = m.role == NpzMember::RImage || m.role == NpzMember::RAxis;
            ImGui::BeginDisabled(!pickable);
            bool wasSel = m.selected;
            if (ImGui::Checkbox("##sel", &m.selected) && m.role == NpzMember::RAxis) {
                // ONE axis per Open: one name, one unit. Ticking a second one
                // would silently relabel the first with the second's name.
                if (!wasSel) {
                    for (size_t k = 0; k < app.npzPick.size(); k++)
                        if (k != i && app.npzPick[k].role == NpzMember::RAxis)
                            app.npzPick[k].selected = false;
                    snprintf(app.npzAxisName, sizeof app.npzAxisName, "%s", m.name.c_str());
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(m.shapeText.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", m.dtype.c_str());
            ImGui::TableSetColumnIndex(3);
            if (m.role == NpzMember::RImage) {
                ImGui::TextUnformatted(m.becomes.c_str());
            } else if (m.role == NpzMember::RAxis) {
                bool fits = false;
                for (const auto& o : app.npzPick)
                    if (o.role == NpzMember::RImage && o.selected && o.frames > 1 &&
                        (size_t)o.frames == m.vals.size()) fits = true;
                if (!fits)
                    ImGui::TextDisabled("metadata (no %d-frame stack ticked)", (int)m.vals.size());
                else if (m.selected && axUnitNow.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "x axis - needs a unit");
                else if (m.selected)
                    ImGui::Text("x axis in %s", axUnitNow.c_str());
                else
                    ImGui::TextDisabled("could be the x axis");
            } else {
                ImGui::TextDisabled("%s", m.becomes.c_str());
            }
            if (!m.why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", m.why.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (axisRow >= 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("x axis:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        ImGui::InputTextWithHint("##npzaxname", "name", app.npzAxisName, sizeof app.npzAxisName);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("what frame i physically IS: exposure, illuminance,\n"
                              "elapsed time ... the key's name is only the default");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::InputTextWithHint("##npzaxunit", "unit", app.npzAxisUnit, sizeof app.npzAxisUnit);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ms, lx, C ... NOT read from the key name: \"exposure_ms\"\n"
                              "is a name somebody chose, not a proof of milliseconds.\n"
                              "No unit, no axis - which is the honest answer.");
    }
    // one honesty line, always drawn so the footer height does not jump
    if (axisRow >= 0 && !axisFits)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1),
                           "no ticked stack has %d frames - the axis will not be applied",
                           (int)app.npzPick[axisRow].vals.size());
    else if (axisRow >= 0 && axUnitNow.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1),
                           "the axis needs a unit - without one it is not applied "
                           "(a unit is never assumed)");
    else if (axisRow >= 0)
        ImGui::TextDisabled("%d value(s) become the x axis of a %d-frame stack, exactly "
                            "as pasted values do",
                            (int)app.npzPick[axisRow].vals.size(),
                            (int)app.npzPick[axisRow].vals.size());
    else
        ImGui::NewLine();
    ImGui::Text("selected: %d array(s), %d frame(s)", selImages, selFrames);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("batch:");
    ImGui::SameLine();
    ImGui::TextUnformatted(baseName(app.npzPickPath).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("the file is the batch: everything opened from it lands\n"
                          "under ONE heading in Files, named after the file");
    ImGui::SameLine();
    ImGui::BeginDisabled(selImages == 0);
    bool open = ImGui::Button("Load selected", ImVec2(150 * app.uiScale, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) {
        app.npzPick.clear();
        app.npzPickOpen = false;
        app.npzPickPath.clear();
        ImGui::CloseCurrentPopup();
        npzPickNext();             // cancelling THIS file does not cancel the rest
    }
    if (open) {
        ImGui::CloseCurrentPopup();
        npzPickAccept();               // same call the headless selftests make
    }
    ImGui::EndPopup();
}

// After a single file is opened: offer (or silently start) loading its siblings.
static void maybeOfferSequence(int imageIdx) {
    if (app.seqLoadMode == 2) return;                 // never
    if (imageIdx < 0 || imageIdx >= (int)app.images.size()) return;
    ImageDoc* im = app.images[imageIdx].get();
    if (im->seqId != 0 || im->src->path.empty()) return;
    std::string pattern;
    std::vector<std::string> files = findSequenceSiblings(im->src->path, pattern);
    if ((int)files.size() < 2) return;
    if (app.seqLoadMode == 1) {                       // always
        startSequenceLoad(imageIdx, files, pattern);
        toast("loading stack: " + pattern + " (" + std::to_string(files.size()) + " files)");
        return;
    }
    app.seqAskImage = imageIdx;                       // ask
    app.seqAskFiles = files;
    app.seqAskPattern = pattern;
}
