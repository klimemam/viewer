// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    // The local-browse entry, verifiable without a human: the function behind
    // File > Browse Folder (Local)... must land the picked folder in the Browse
    // panel through the LOCAL peer - connected, empty host, entries listed.
    if (!g_localbrowseSelftest.empty()) {
        browseLocalFolder(g_localbrowseSelftest);   // exactly what the menu does
        double t0 = nowSec();
        while (nowSec() - t0 < 120.0) {
            pumpRemoteBrowse();
            if (rbMain().b.connected && !rbMain().b.entries.empty()) break;
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const App::RemoteBrowse& B = rbMain().b;
        bool ok = B.connected && B.host.empty() && !B.entries.empty() &&
                  app.showRemote;
        fprintf(stderr, "localbrowseselftest: connected=%d host='%s' dir=%s "
                        "entries=%d showBrowse=%d%s%s\n",
                B.connected ? 1 : 0, B.host.c_str(), B.dir.c_str(),
                (int)B.entries.size(), app.showRemote ? 1 : 0,
                B.err.empty() ? "" : " err=", B.err.c_str());
        // ...and what it SAYS, which is the half that was wrong: a browse of
        // this disk claimed a machine in the window title, an ssh session in
        // the status bar and a "local://" scheme in the picker's header. The
        // mechanism above is unchanged - only the words are checked here.
        {
            std::string title = windowTitleText();
            std::string tag = peerTag(B.host);
            std::string label = peerLabel(B.host);
            std::string pickHdr = B.host.empty() ? B.dir : makeRemoteUrl(B.host, B.dir);
            bool said = title.find('[') == std::string::npos &&
                        title.find("ssh") == std::string::npos &&
                        viewingHost().empty() &&
                        tag == ICON_LOCAL && label == PEER_HERE &&
                        pickHdr.find("local://") == std::string::npos;
            fprintf(stderr, "localbrowseselftest: title=\"%s\" tag=%s peer=\"%s\" "
                            "pickerheader=\"%s\" othermachine=%d: %s\n",
                    title.c_str(), tag.c_str(), label.c_str(), pickHdr.c_str(),
                    viewingHost().empty() ? 0 : 1, said ? "ok" : "FAIL");
            if (!said) ok = false;
        }
        {   // WHICH END DECIDES THE ORDER. The peer sends a LIST in plain
            // lexicographic order - serve.cpp merges directories, singles and
            // groups into one row list and sorts it with `<` on the name - and
            // this end sorts it AGAIN before drawing, with rbSortShown. That
            // is the reason a local listing and an ssh listing of the same
            // folder read identically, and the reason the natural default
            // needed no protocol change and no peer update: the bytes on the
            // wire are unchanged and the order is decided here.
            //
            // rb/unpadded is where the claim is falsifiable, because its names
            // are not zero-padded: the REPLY arrives lv1, lv10, lv2 and the
            // SCREEN must read lv1, lv2, lv10. If the client ever stopped
            // sorting and drew the reply's order, this is the assertion that
            // notices - on the local door, which is the same LIST path.
            std::string sub = B.dir + "/unpadded";
            remoteBrowseTo(rbMain(), sub);
            double t1 = nowSec();
            while (nowSec() - t1 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == sub && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<std::string> wire, screen;
            for (const auto& e : B.entries)
                if (e.dir && e.name.rfind("lv", 0) == 0) wire.push_back(e.name);
            rbMain().nameNatural = true;
            std::vector<RbRow> v = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
            std::vector<int> idx(v.size());
            for (int i = 0; i < (int)v.size(); i++) idx[i] = i;
            rbSortShown(rbMain(), v, idx);
            for (int i : idx)
                if (v[i].isDir() && !v[i].up && v[i].name().rfind("lv", 0) == 0)
                    screen.push_back(v[i].name());
            auto say = [](const std::vector<std::string>& s) {
                std::string t;
                for (const auto& n : s) t += (t.empty() ? "" : " ") + n;
                return t;
            };
            bool ordOk = wire == std::vector<std::string>{ "lv1", "lv10", "lv2" } &&
                         screen == std::vector<std::string>{ "lv1", "lv2", "lv10" };
            fprintf(stderr, "localbrowseselftest: order in %s: peer sent [%s], "
                            "panel lays out [%s]: %s\n",
                    sub.c_str(), say(wire).c_str(), say(screen).c_str(),
                    ordOk ? "ok" : "FAIL");
            if (!ordOk) ok = false;
        }
        fprintf(stderr, "localbrowseselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
