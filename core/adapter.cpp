// See adapter.h. The one rule that shapes this file: the viewer must never make
// a console window flash, and must never claim a program failed when it never
// started (docs/input-adapters.md §4.13 - "if Python is missing, say so").
#include "adapter.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <atomic>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#endif

namespace adapter {

namespace {

std::filesystem::path u8p(const std::string& s) { return std::filesystem::u8path(s); }

std::string readWhole(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // CRLF only matters because these strings are shown to a human and pasted
    // into reports; the bytes themselves are never parsed for structure.
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out += s[i];
    }
    return out;
}

// A scratch path nobody else holds. Not tempfile-secure and does not need to be:
// it is the viewer's own stdout sink, in the user's own temp directory.
std::filesystem::path scratch(const char* tag) {
    static std::atomic<unsigned> seq{ 0 };
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec);
#if defined(_WIN32)
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    return d / ("viewer_" + std::string(tag) + "_" + std::to_string(pid) + "_" +
                std::to_string(seq.fetch_add(1)));
}

#if defined(_WIN32)
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// CreateProcessW takes ONE string: quote an argument the way CommandLineToArgvW
// unquotes one. Same rule as main.cpp's quoteArgW - a reader lives under a path
// the user chose, and this user's profile directory has spaces and Japanese in it.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}
#endif

}  // namespace

std::string showCommand(const std::vector<std::string>& argv) {
    std::string s;
    for (const auto& a : argv) {
        if (!s.empty()) s += ' ';
        s += a.find_first_of(" \t\"") == std::string::npos ? a : "\"" + a + "\"";
    }
    return s;
}

// Output goes to FILES, not pipes. A pipe would need a drain loop running
// alongside the wait or a child writing more than the pipe buffer deadlocks
// forever; a traceback plus a numpy warning gets close enough to that to matter.
// Files cost one temp write and cannot deadlock at all.
Run run(const std::vector<std::string>& argv, int timeoutMs, std::atomic<bool>* cancel,
        const std::string& outFile, const std::string& errFile, bool captureStdout) {
    Run r;
    if (argv.empty()) { r.fail = "no command"; return r; }
    std::filesystem::path outPath = outFile.empty() ? scratch("out")
                                                    : std::filesystem::u8path(outFile);
    std::filesystem::path errPath = errFile.empty() ? scratch("err")
                                                    : std::filesystem::u8path(errFile);
    bool ownPaths = outFile.empty() && errFile.empty();
    std::error_code ec;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE hOut = CreateFileW(outPath.wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    HANDLE hErr = CreateFileW(errPath.wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        r.fail = "cannot create a temporary file for the command's output";
        return r;
    }
    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoteArg(wide(a));
    }
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);                       // CreateProcessW may write into it
    STARTUPINFOW si{};
    si.cb = sizeof si;
    // Belt and braces, exactly as core/remote.cpp spawns the peer: CREATE_NO_WINDOW
    // suppresses the console, SW_HIDE covers the case where the child asks for a
    // window itself. A window flashing up on every open would be unusable.
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nullptr;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    CloseHandle(hErr);
    if (!started) {
        DWORD e = GetLastError();
        r.fail = "cannot start " + argv[0] + " (windows error " + std::to_string((unsigned)e) + ")";
        std::filesystem::remove(outPath, ec);
        std::filesystem::remove(errPath, ec);
        return r;
    }
    r.started = true;
    if (!cancel && timeoutMs < 0) {
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        // Sliced, not one uninterruptible wait: a Stop has to be able to reach
        // the child, and the caller has to be able to say how long it has been
        // running. 25 ms is below anything a person perceives and costs nothing.
        int waited = 0;
        for (;;) {
            if (WaitForSingleObject(pi.hProcess, 25) != WAIT_TIMEOUT) break;
            if (cancel && cancel->load()) {
                r.cancelled = true;
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                break;
            }
            waited += 25;
            if (timeoutMs >= 0 && waited >= timeoutMs) {
                r.timedOut = true;
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                break;
            }
        }
    }
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    pid_t pid = fork();
    if (pid < 0) { r.fail = "cannot fork"; return r; }
    if (pid == 0) {
        int fo = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int fe = open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fo >= 0) dup2(fo, STDOUT_FILENO);
        if (fe >= 0) dup2(fe, STDERR_FILENO);
        int fi = open("/dev/null", O_RDONLY);
        if (fi >= 0) dup2(fi, STDIN_FILENO);
        std::vector<char*> av;
        for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    r.started = true;
    int status = 0;
    if (timeoutMs < 0 && !cancel) {
        waitpid(pid, &status, 0);
    } else {
        int waited = 0;
        for (;;) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) break;
            if (cancel && cancel->load()) {
                r.cancelled = true;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            if (timeoutMs >= 0 && waited >= timeoutMs) {
                r.timedOut = true;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            struct timespec ts { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
            waited += 10;
        }
    }
    r.exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    // execvp failing in the child looks like a normal exit here; 127 is the
    // agreed "never ran", and reporting it as a program that ran and said no
    // would send the user hunting through their adapter for a bug we caused.
    if (r.exit == 127) {
        r.started = false;
        r.fail = "cannot start " + argv[0] + " (not found or not executable)";
    }
#endif

    if (captureStdout) r.out = readWhole(outPath);
    r.err = readWhole(errPath);
    // Only clean up what we chose ourselves. Caller-supplied paths belong to the
    // caller, which is still holding them open to show what the reader printed.
    if (ownPaths) {
        std::filesystem::remove(outPath, ec);
        std::filesystem::remove(errPath, ec);
    }
    return r;
}

namespace {
std::string g_python;              // "" = not probed, or probed and not found
std::string g_pythonWhy;
bool g_probed = false;
}

void forgetPython() { g_probed = false; g_python.clear(); g_pythonWhy.clear(); }

std::string findPython(const std::string& configured, std::string& why) {
    if (g_probed) { why = g_pythonWhy; return g_python; }
    g_probed = true;
    g_python.clear();

    std::vector<std::string> tried;
    auto probe = [&](const std::string& exe) {
        // Import numpy, not just --version: the harness needs numpy, and on
        // Windows a "python3" App Execution Alias sits on PATH by default and is
        // a stub for the Microsoft Store. It answers, so looking it up says yes;
        // it exits non-zero, so running it says no. Running it is the truth.
        Run p = run({ exe, "-c", "import numpy" }, 30000);
        if (p.started && p.exit == 0) return true;
        std::string note = exe;
        if (!p.started) note += " (not found)";
        else if (p.exit != 0) {
            std::string e = p.err;
            if (e.find("numpy") != std::string::npos) note += " (no numpy)";
            else note += " (exit " + std::to_string(p.exit) + ")";
        }
        tried.push_back(note);
        return false;
    };

    if (!configured.empty()) {
        if (probe(configured)) {
            g_python = configured;
            g_pythonWhy = "configured: " + configured;
            why = g_pythonWhy;
            return g_python;
        }
    }
    for (const char* cand : { "python", "python3", "py" }) {
        if (probe(cand)) {
            g_python = cand;
            g_pythonWhy = std::string("found on PATH: ") + cand;
            why = g_pythonWhy;
            return g_python;
        }
    }
    g_pythonWhy = "no python with numpy: tried ";
    for (size_t i = 0; i < tried.size(); i++)
        g_pythonWhy += (i ? ", " : "") + tried[i];
    why = g_pythonWhy;
    return {};
}

bool openInEditor(const std::string& path, std::string& how) {
    // §4.13: $EDITOR, then `code -g`, then the OS association. No built-in
    // editor - writing Python in an ImGui text box is not a job this tool wants.
    const char* ed = std::getenv("EDITOR");
    if (ed && *ed) {
        // $EDITOR may carry flags ("code -w"); the first word is the program.
        std::istringstream ss(ed);
        std::vector<std::string> av;
        std::string tok;
        while (ss >> tok) av.push_back(tok);
        av.push_back(path);
        Run r = run(av, -1);
        if (r.started) { how = "$EDITOR (" + std::string(ed) + ")"; return true; }
    }
    {
        Run r = run({ "code", "-g", path }, 20000);
        if (r.started && r.exit == 0) { how = "code -g"; return true; }
    }
#if defined(_WIN32)
    HINSTANCE h = ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
    if ((INT_PTR)h > 32) { how = "the file's own program"; return true; }
#elif defined(__APPLE__)
    Run r = run({ "open", path }, 20000);
    if (r.started && r.exit == 0) { how = "open"; return true; }
#else
    Run r = run({ "xdg-open", path }, 20000);
    if (r.started && r.exit == 0) { how = "xdg-open"; return true; }
#endif
    how = "nothing would open it: set $EDITOR, or install the 'code' command";
    return false;
}

std::string moduleVersion(const std::string& pyFile) {
    std::string text = readWhole(u8p(pyFile));
    // Top level only: "VERSION" at the start of a line. A VERSION nested in a
    // class or a function is not the module's, and this is a cache key, so a
    // wrong answer is worse than no answer.
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.compare(0, 7, "VERSION") != 0) continue;
        size_t eq = line.find('=', 7);
        if (eq == std::string::npos) continue;
        if (line.find_first_not_of(" \t", 7) != eq) continue;   // "VERSIONS = ..."
        std::string v = line.substr(eq + 1);
        size_t a = v.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        size_t b = v.find_first_of("#\r", a);
        v = v.substr(a, b == std::string::npos ? std::string::npos : b - a);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return v;
    }
    return {};
}

std::vector<std::string> moduleFunctions(const std::string& pyFile) {
    std::string text = readWhole(u8p(pyFile));
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        // Top-level "def name(" only: a method is not something the viewer can
        // name as <file>:<func>, so offering one would be a broken choice.
        if (line.compare(0, 4, "def ") != 0) continue;
        size_t a = line.find_first_not_of(" \t", 4);
        if (a == std::string::npos) continue;
        size_t b = line.find('(', a);
        if (b == std::string::npos) continue;
        std::string name = line.substr(a, b - a);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        if (name.empty() || name[0] == '_') continue;
        bool ident = true;
        for (char c : name)
            if (!(isalnum((unsigned char)c) || c == '_')) ident = false;
        if (ident) out.push_back(name);
    }
    return out;
}

}  // namespace adapter
