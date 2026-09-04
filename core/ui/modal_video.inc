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
static void drawVideoExportModal() {
    if (app.videoAskOpen && !ImGui::IsPopupOpen("Export stack as video"))
        ImGui::OpenPopup("Export stack as video");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Export stack as video", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const int seqId = app.videoAskSeqId;
    std::vector<int> fr = framesOfSeq(seqId);
    if (fr.empty()) {          // the stack closed while this was up
        app.videoAskOpen = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const ImageDoc* f0 = app.images[fr.front()].get();
    const int w = f0->w, h = f0->h, n = (int)fr.size();
    {
        App::SeqInfo* si = seqInfo(seqId);
        ImGui::Text("%s", si ? si->name.c_str() : "stack");
    }
    ImGui::TextDisabled("%d frames, %d x %d, at the display levels on screen", n, w, h);
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
    const bool can = app.videoFormat == 1 ? !ff.empty() : canAvi;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Choose file...", ImVec2(150 * app.uiScale, 0))) {
        app.videoAskOpen = false;
        if (!pfd::settings::available()) {
            toast("no file-dialog backend found (install zenity or kdialog)", true);
        } else {
            app.videoDlgSeqId = seqId;
            const std::string ext = videoExtFor(app.videoFormat);
            app.videoDlg = std::make_unique<pfd::save_file>(
                "Export stack as video", videoDefaultName(seqId, app.videoFormat),
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
