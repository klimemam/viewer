// Running an input adapter: finding Python, starting it without a console
// flashing up, and opening a reader in the user's editor.
// docs/input-adapters.md §4.13. Everything here is free of the viewer's state so
// that the parts that touch the process table stay testable on their own.
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace adapter {

// One external command run to completion, with its output captured whole.
// `started` separates "the program ran and said no" from "the program never
// ran" - they need different sentences and the caller must not conflate them.
struct Run {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;   // the caller asked for it to stop; not a failure
    int exit = -1;
    std::string out;          // stdout
    std::string err;          // stderr: where the harness writes its summary
    std::string fail;         // why it did not start, when it did not
};

// argv[0] is the executable. No shell is involved, so nothing is word-split and
// no quoting rule of the user's applies to their own paths.
//
// `cancel`, when given, is polled while waiting: set it and the child is killed.
// The wait is therefore done in slices rather than in one uninterruptible call,
// which is also what lets the caller run this on a worker thread and still stop
// it - a reader takes as long as the user's data takes.
//
// `outFile` / `errFile`, when given, are where the child's stdout and stderr go.
// Supplying them is what lets the caller READ THEM WHILE THE CHILD IS STILL
// RUNNING - a reader that prints its progress is only useful if the progress
// arrives before the end. Left empty, run() picks temporaries of its own.
Run run(const std::vector<std::string>& argv, int timeoutMs = 300000,
        std::atomic<bool>* cancel = nullptr,
        const std::string& outFile = std::string(),
        const std::string& errFile = std::string());

// The interpreter to use, or "" with `why` naming what was tried. `configured`
// wins when it is set. Probed by RUNNING it: on Windows the bare python3 on PATH
// is a Microsoft Store stub that prints an advert and exits non-zero, and numpy
// is what the harness actually needs, so importing it IS the probe.
std::string findPython(const std::string& configured, std::string& why);
void forgetPython();                      // re-probe (the configured path changed)

// $EDITOR, then `code -g <file>`, then the OS association (§4.13). `how` names
// the one that answered, for a message that can be checked against reality.
bool openInEditor(const std::string& path, std::string& how);

// The command exactly as it will be run, for showing it BEFORE running it.
std::string showCommand(const std::vector<std::string>& argv);

// A module's VERSION, read out of the text rather than by importing it: the
// cache key needs it before deciding whether to start Python at all, and
// starting Python to find out would defeat the cache. "" when absent.
std::string moduleVersion(const std::string& pyFile);

// The functions a reader file defines, in file order - the candidates offered
// when a reader is chosen. Text again, for the same reason.
std::vector<std::string> moduleFunctions(const std::string& pyFile);

}  // namespace adapter
