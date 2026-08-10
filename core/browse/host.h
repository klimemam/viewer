// core/browse/host.h — BrowseHost: browse → viewer の継ぎ目。担当: Browse
// P7 (docs/split-plan.md §3): 「一覧して選ぶ」までが Browse、「開いて何をする
// か」が viewer。S3 が panel.inc / nav.inc に置いた [BrowseHost] 注記 55 箇所が
// この構造体経由になった。browse の実 TU が viewer に対して見るのはこのヘッダ
// (と remote_proto.h / state.h の共有状態)だけで、逆向きの宣言は browse.h。
#pragma once

#include "../app/state.h"            // App / extern app / ImageDoc / PendingGroup

// The seam, as data: every viewer VERB the Browse panels invoke, bundled into
// one struct of plain function pointers. The targets stay `static` in the
// spine TU (core/main.cpp fills this after cli.inc, when it has seen them
// all), so P7 adds no linkage to the viewer's own glue - the pointer crosses
// the TU boundary, the names do not. Plain pointers, not std::function: the
// table is constant-initialized, so no static-init-order question can arise,
// and rbWorker calls wakeUi from the worker thread where a capturing wrapper
// would only add a place to be wrong.
struct BrowseHost {
    // ---- say / persist / wake ----------------------------------------------
    void (*toast)(const std::string& msg, bool err);
    void (*savePrefs)();
    // was glfwPostEmptyEvent at the end of rbWorker's loop: the one GLFW call
    // the browse side had. Behind the seam the browse TUs touch no window API.
    void (*wakeUi)();
    // ---- registered opens (the viewer's open dispatch) ----------------------
    // npyRead is rp::NpyRead and is 0 from every browse call site: Browse opens
    // a file, and "read it another way" is a thing said to a document that is
    // already open (docs/input-adapters.md §3.3, Inspector). The parameter is
    // here only because the pointer has to have the function's type.
    bool (*openRemote)(const std::string& url, bool asPreview, int frame, int npyRead);
    void (*openRemoteStack)(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name, int port, int token);
    void (*openStackForAverage)(const std::string& host, const std::vector<std::string>& files,
                                const std::string& name, int port);
    void (*requestBrowseTemporal)(const std::string& host, std::vector<std::string> files,
                                  const std::string& label, int port);
    void (*openPickerWith)(std::vector<App::PendingGroup> groups, const std::string& displayRoot,
                           const std::string& stripRoot, bool remoteMode,
                           const std::string& host, int port);
    void (*openReaderPicker)(const std::string& path, const std::string& why);
    // File > Browse Folder (Local)...'s dialog, shared by the empty state's
    // button. Not one of S3's 55 marks, but the same class of call - a viewer
    // door the panel merely knocks on - so it goes through the same seam.
    void (*browseFolderDialog)();
    // ---- the preview slot and the registered list ---------------------------
    void (*selectImage)(int idx);
    void (*promotePreview)(ImageDoc* d);
    void (*dropPreview)();
    void (*stepPreviewFrame)(int delta);
};
// Filled once, statically, in core/main.cpp - the spine is the only TU that
// can see all fourteen targets.
extern const BrowseHost g_browseHost;

// ---- viewer-side helpers the browse TUs call BY NAME -------------------------
// NOT callbacks, on purpose. These are pure string/path/name helpers plus the
// remote-client infrastructure (deployPeer and its log) - things a carved-out
// Browse would take a copy of or receive from its remote layer, not decisions
// only the viewer can make. Routing them through BrowseHost would dress ~50
// working call sites as seam traffic; a plain extern declaration leaves every
// one of them textually untouched. Definitions stay where they live today
// (util.inc / sequence.inc / open_dispatch.inc / remote_client.inc /
// canvas.inc / panel_projection.inc / menus.inc), shorn of `static`.
struct PathSeg { std::string label, path; };   // moved from util.inc: the decl
                                               // below returns it, so the type
                                               // must live where the decl does
std::vector<PathSeg> pathSegments(const std::string& d);
std::string pathRootOf(const std::string& d);
std::string baseName(const std::string& p);
// #111: the two questions the one predicate `isNpyName` used to fold into one.
// Definitions and the full argument live in core/ui/menus.inc.
bool viewerReadsName(const std::string& n);          // ...this build can open
bool peerServesName(const std::string& n);           // ...core/serve.cpp can serve
std::string peerRefusalFor(const std::string& n);    // "" when the peer serves it
std::string viewerRefusalFor(const std::string& n);  // "" when this build reads it
void sortFramesNumerically(std::vector<std::string>& files);
bool globListMatch(const char* list, const std::string& subject);
std::string patternOfNames(std::vector<std::string> names);
std::string stackNameFor(const std::string& dir, const std::string& pattern);
std::string makeRemoteUrl(const std::string& host, const std::string& path, int port = 0);
bool deployPeer(const std::string& host, int port, bool force, std::string& log);
std::string peerLabel(const std::string& host);
extern std::string g_bootstrapLog;   // owned by the UI thread; the worker only
                                     // carries text home in RbResult::info
extern const char* REMOTE_HOME;      // where the peer self-installs
// what an empty host is CALLED on screen (the url form stays "local://" - it
// is storage, not language). Was in remote_client.inc; nav.inc says
// "browsing " PEER_HERE, so the macro has to be visible on both sides.
#define PEER_HERE  "this machine"
extern const char* AVG_TIP;          // the frame-average tooltip, shared with
                                     // the projection panel (one wording)
extern const ImVec4 AB_AMBER;        // the house warning colour (canvas.inc)
