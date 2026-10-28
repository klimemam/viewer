// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: リモート
// ---- --sshfail-selftest: an ssh that failed says so ----------------------------
//
// Issue #324. ssh could not log in (BatchMode=yes, no usable key: exit 255,
// "Permission denied (publickey)." on stderr, nothing on stdout), and the
// viewer said "no prebuilt peer for this machine:" over an EMPTY uname -
// runSshCommand read neither the exit status nor stderr, so "stdout came back
// empty" was a command that ran, and deployPeer named a cause from nothing.
//
// The server is this binary: `<viewer> --fake-ssh` stands in for ssh
// (remote::setSshProgramForTest) and the HOST names the outcome - the table is
// at runFakeSsh in core/remote.cpp. Asserted:
//
//   F1  ssh's own failure: runSshCommand returns false, exit 255, sshFailed,
//       ssh's stderr kept, and the sentence names the host, the status and
//       ssh's words (CRs gone), with the BatchMode=yes remedy
//   F2  deployPeer over that failure: "could not run commands on <host> over
//       ssh (exit 255): ..." with the remedy (and the port in it), never
//       "no prebuilt peer"
//   F3  exit 0 with NOTHING on stdout: still "could not run commands", never
//       "no prebuilt peer" - an empty uname is not an unsupported machine
//   F4  uname answered with a machine there is no peer for: the old sentence,
//       naming that machine
//   F5  256 KB of stderr before a line of stdout comes back promptly (both
//       pipes are drained together), with the stdout, the status, and the
//       LAST of stderr
//   F6  a script that explains itself on stdout and exits 1: false, the
//       words kept, not an ssh failure
//   F7  the protocol Session over a failed ssh: no "is it installed there?";
//       with the extra round trip opted in, ssh's own words
//   F8  an ssh failure that is not about keys gets its words and no key remedy
//   F9  the gist drops the host-key notice and the post-quantum banner
//
// NOGL; it starts child processes of this binary and nothing else. The ssh
// program is put back before it returns.
if (g_sshFailSelftest) {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "sshfailselftest: %-72s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto has = [](const std::string& s, const char* needle) {
        return s.find(needle) != std::string::npos;
    };
    auto starts = [](const std::string& s, const char* head) {
        return s.compare(0, strlen(head), head) == 0;
    };
    remote::setSshProgramForTest({ app.exePath, "--fake-ssh" });

    {   // F1
        std::string out, err;
        remote::SshRun run;
        const bool got = remote::runSshScript("auth", 0, "uname -s", out, err, 20.0, nullptr, &run);
        fprintf(stderr, "sshfailselftest: F1 exit %d err '%s'\n", run.exitCode, err.c_str());
        check(!got && run.exitCode == 255 && run.sshFailed,
              "F1 ssh's exit 255 is a failure, and an ssh failure");
        check(has(run.stderrText, "Permission denied (publickey)."),
              "F1 ...ssh's stderr is kept");
        check(starts(err, "could not run commands on auth over ssh (exit 255): "
                          "hish@auth: Permission denied (publickey).") &&
              !has(err, "\r"),
              "F1 ...and the sentence names host, status and ssh's words");
        check(has(err, "BatchMode=yes") && has(err, "`ssh -o BatchMode=yes auth uname`"),
              "F1 ...with the BatchMode=yes remedy");
    }
    {   // F2
        std::string log;
        const bool got = deployPeer("auth", 0, false, log);
        fprintf(stderr, "sshfailselftest: F2 log '%s'\n", log.c_str());
        check(!got && starts(log, "could not run commands on auth over ssh (exit 255): "),
              "F2 deployPeer over a failed login says the commands did not run");
        check(has(log, "Permission denied (publickey).") && has(log, "BatchMode=yes"),
              "F2 ...with ssh's words and the remedy");
        check(!has(log, "no prebuilt peer"), "F2 ...and never 'no prebuilt peer'");
        std::string logP;
        deployPeer("auth", 2222, false, logP);
        check(has(logP, "`ssh -o BatchMode=yes -p 2222 auth uname`"),
              "F2 the remedy carries the port");
    }
    {   // F3
        std::string log;
        const bool got = deployPeer("empty", 0, false, log);
        fprintf(stderr, "sshfailselftest: F3 log '%s'\n", log.c_str());
        check(!got && log == "could not run commands on empty over ssh (exit 0): "
                             "nothing came back, not even uname's answer",
              "F3 exit 0 with an empty answer: the commands did not run");
        check(!has(log, "no prebuilt peer"), "F3 ...and never 'no prebuilt peer'");
    }
    {   // F4
        std::string log;
        const bool got = deployPeer("sunos", 0, false, log);
        fprintf(stderr, "sshfailselftest: F4 log '%s'\n", log.c_str());
        check(!got && starts(log, "no prebuilt peer for this machine:\nSunOS sparc64\n"
                                  "(binaries exist for Linux x86_64 and macOS arm64"),
              "F4 a machine uname named and we have no peer for: 'no prebuilt peer'");
    }
    {   // F5
        std::string out, err;
        remote::SshRun run;
        auto w0 = std::chrono::steady_clock::now();
        const bool got = remote::runSshCommand("chatty", 0, "true", "", out, err, 20.0,
                                               nullptr, &run);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        fprintf(stderr, "sshfailselftest: F5 %.2fs exit %d stderr %zu bytes err '%.80s'\n",
                secs, run.exitCode, run.stderrText.size(), err.c_str());
        check(!got && run.exitCode == 3 && !has(err, "timed out") && secs < 15.0,
              "F5 a chatty stderr does not stall the child: it exits, status 3");
        check(out == "stdout after the noise\n", "F5 ...its stdout arrives whole");
        const std::string tail = "the last thing it said\n";
        check(run.stderrText.size() <= remote::kSshStderrCap &&
              run.stderrText.size() >= tail.size() &&
              run.stderrText.compare(run.stderrText.size() - tail.size(), tail.size(), tail) == 0,
              "F5 ...stderr keeps its LAST 64 KB");
        check(has(err, "the command on chatty exited with status 3:") &&
              has(err, "the last thing it said") && !run.sshFailed,
              "F5 ...and the sentence has the status and the last of stderr");
    }
    {   // F6
        std::string out, err;
        remote::SshRun run;
        const bool got = remote::runSshScript("explains", 0, "exit 1", out, err, 20.0,
                                              nullptr, &run);
        check(!got && run.exitCode == 1 && !run.sshFailed && has(out, "git clone failed"),
              "F6 a script that exits 1 is a failure, and keeps what it said");
    }
    {   // F7
        remote::Session s;
        std::string err;
        const bool up = s.startOn("auth", 0, "~/.viewer/viewer-serve", err);
        fprintf(stderr, "sshfailselftest: F7 plain '%s'\n", err.c_str());
        check(!up && starts(err, "could not run commands on auth over ssh (exit 255)") &&
              !has(err, "is it installed there"),
              "F7 a Session over a failed ssh does not ask 'is it installed there?'");
        remote::Session s2;
        s2.setExplainFailure(true);
        std::string err2;
        s2.startOn("auth", 0, "~/.viewer/viewer-serve", err2);
        fprintf(stderr, "sshfailselftest: F7 explained '%s'\n", err2.c_str());
        check(starts(err2, "could not run commands on auth over ssh (exit 255): "
                           "hish@auth: Permission denied (publickey)."),
              "F7 ...and with the extra round trip, ssh's own words");
    }
    {   // F8
        std::string out, err;
        remote::SshRun run;
        remote::runSshScript("nosuch", 0, "uname -s", out, err, 20.0, nullptr, &run);
        fprintf(stderr, "sshfailselftest: F8 err '%s'\n", err.c_str());
        check(run.sshFailed && has(err, "Could not resolve hostname nosuch") &&
              !has(err, "BatchMode"),
              "F8 a DNS failure gets ssh's words and no key remedy");
    }
    {   // F9
        const std::string g = remote::sshStderrGist(
            "Warning: Permanently added 'h' (ED25519) to the list of known hosts.\r\n"
            "** WARNING: connection is not using a post-quantum key exchange algorithm.\r\n"
            "\r\n"
            "hish@h: Permission denied (publickey).\r\n");
        check(g == "hish@h: Permission denied (publickey).",
              "F9 the gist keeps the cause and drops ssh's good-day noise");
    }

    remote::setSshProgramForTest({});
    fprintf(stderr, "sshfailselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
