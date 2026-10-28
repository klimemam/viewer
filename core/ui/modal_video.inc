// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// ---- the two questions a stack-to-video export has to settle first (#253) ---
// Frame rate and format, in front of the save dialog rather than inside it,
// because the OS dialog has no room for either and because the FORMAT decides
// the extension the dialog then defaults to - and the extension is what the
// export reads back as the user's intent (export_video.inc).
//
// MKV is offered only when ffmpeg is on PATH, and when it is not the row is
// still DRAWN, disabled, with the reason under it. A capability that is simply
// absent from the dialog teaches the user that this build cannot do it; a
// disabled row with "no ffmpeg on PATH" beside it teaches them what to install.
// The same two questions for the COMPARE VIEW of A's stack (#308): one modal,
// titled for what it exports, sized by the composite rather than the frame.
static void drawVideoExportModal() {
    const bool compare = app.videoAskCompare;
    const char* title = compare ? "Export compare view as video" : "Export stack as video";
    if (app.videoAskOpen && !ImGui::IsPopupOpen(title))
        ImGui::OpenPopup(title);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    int seqId = app.videoAskSeqId;
    std::vector<int> fr;
    int w = 0, h = 0;
    std::string planWhy;
    if (compare) {
        // A is the cursor: the stack is whichever A stands in NOW
        planWhy = compareVideoPlan(fr, w, h);
        if (cur()) seqId = cur()->seqId;
    } else {
        fr = framesOfSeq(seqId);
        if (!fr.empty()) { w = app.images[fr.front()]->w; h = app.images[fr.front()]->h; }
    }
    if (fr.empty()) {          // the stack closed while this was up
        app.videoAskOpen = false;
        if (compare && !planWhy.empty()) toast(planWhy, true);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const int n = (int)fr.size();
    {
        App::SeqInfo* si = seqInfo(seqId);
        if (compare) ImGui::Text("compare view, A stepping through %s", si ? si->name.c_str() : "its stack");
        else ImGui::Text("%s", si ? si->name.c_str() : "stack");
    }
    if (compare && !planWhy.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::TextWrapped("%s", planWhy.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled(compare ? "%d frames, %d x %d composite, at the display levels on screen"
                                    : "%d frames, %d x %d, at the display levels on screen", n, w, h);
    }
    if (compare) ImGui::Checkbox("letters (A / B / C) top-left of each side", &app.cmpExportLetters);
    ImGui::Separator();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    float fps = app.videoFps;
    if (ImGui::InputFloat("frames per second", &fps, 0.0f, 0.0f, "%.6g"))
        if (fps > 0.0f && std::isfinite(fps)) app.videoFps = fps;
    ImGui::TextDisabled("%.6g s of video", n / (double)app.videoFps);
    ImGui::Separator();
    const std::string ff = videowrite::ffmpegPath();
    if (ImGui::RadioButton("AVI - uncompressed RGB24", app.videoFormat == 0))
        app.videoFormat = 0;
    {   // the size is the whole argument for the other row, so it is a NUMBER
        const double bytes = (double)videowrite::aviFrameBytes(w, h) * n;
        ImGui::TextDisabled("   about %.1f GB, no dependency, plays anywhere",
                            bytes / 1073741824.0);
        std::string big = videowrite::aviSizeRefusal(w, h, n);
        if (!big.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
            ImGui::TextWrapped("   over AVI 1.0's 4 GB limit - this stack cannot be "
                               "written as AVI");
            ImGui::PopStyleColor();
        }
    }
    ImGui::BeginDisabled(ff.empty());
    if (ImGui::RadioButton("MKV - FFV1, lossless compression", app.videoFormat == 1))
        app.videoFormat = 1;
    ImGui::EndDisabled();
    if (ff.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::TextWrapped("   needs ffmpeg on PATH, and none was found. The same "
                           "pixels either way - FFV1 is lossless.");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("   %s", ff.c_str());
    }
    if (ff.empty() && app.videoFormat == 1) app.videoFormat = 0;
    ImGui::Separator();
    const bool canAvi = videowrite::aviSizeRefusal(w, h, n).empty();
    const bool can = (app.videoFormat == 1 ? !ff.empty() : canAvi) && planWhy.empty();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Choose file...", ImVec2(150 * app.uiScale, 0))) {
        app.videoAskOpen = false;
        if (!pfd::settings::available()) {
            toast("no file-dialog backend found (install zenity or kdialog)", true);
        } else {
            app.videoDlgSeqId = seqId;
            app.videoDlgCompare = compare;
            const std::string ext = videoExtFor(app.videoFormat);
            std::string name = videoDefaultName(seqId, app.videoFormat);
            if (compare) name.insert(name.size() - ext.size(), "_compare");
            app.videoDlg = std::make_unique<pfd::save_file>(title, name,
                std::vector<std::string>{
                    app.videoFormat == 1 ? "Matroska (*.mkv)" : "AVI (*.avi)",
                    "*" + ext });
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(150 * app.uiScale, 0))) {
        app.videoAskOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
