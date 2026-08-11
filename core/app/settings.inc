// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 設定ファイル
//
// settings.jsonc - THE FILE THE USER WRITES BY HAND (issue #50 stage 1).
// docs/settings-inventory.md is the spec; the numbers below are its 判断 list.
//
// The whole point of this file is a split (判断3): settings.jsonc is written by
// a HUMAN and never written back by the app, because savePrefs() rewrites
// prefs.txt whole from 17 product-code call sites and a JSONC file kept under
// that discipline loses the user's comments on the first toggle. So there is no
// writer here. There is a reader, a refusal, and a template generator that
// prints to stdout - nothing in this file opens a file for writing.
//
// WHERE IT SITS IN THE ORDER (asserted by --settings-selftest):
//
//     built-in defaults  <  prefs.txt  <  settings.jsonc  <  command line
//
// The right-hand end is 判断13 ("a flag is a temporary override; there must not
// be two truths"). The middle is 判断18 as rewritten on 2026-08-11: precedence
// is not "env vs file", it is SPECIFIC BEATS GENERAL, and a file the user wrote
// for THIS application is more specific than what the application happened to
// be in last time it quit. The same rule is why readers.editor beats $EDITOR -
// $EDITOR is a generic variable, and git's own order (GIT_EDITOR > core.editor
// > VISUAL > EDITOR) is the model.
//
// The consequence, stated because it is a real one and hiding it would be the
// #50 symptom again: a key WRITTEN IN THIS FILE is sticky. Toggling it in the
// GUI changes it for the session and prefs.txt records it, but the next start
// the file wins again - so the app SAYS SO, by name, once, when it overrides a
// value prefs.txt held differently. Deleting the key hands the toggle back.
//
// THE DIALECT is JSONC exactly as VS Code defines it: JSON, plus `//` and
// `/* */` comments, plus trailing commas. It is deliberately NOT JSON5. Every
// JSON5 extra (single quotes, unquoted keys, hex, +Infinity, leading `.`) is
// one more way for a file to mean something different to this parser than to
// the editor the user is looking at it in, and the point of picking a dialect
// with an editor mode is that the editor and the app agree.

// ---- the value tree ---------------------------------------------------------
// Every node carries WHERE IT CAME FROM. That is not decoration: §5.3 requires
// "line, column and reason, by name" for a whole-file refusal and a per-key one,
// and a position that is recovered after the fact is a position that is wrong
// for the one case that matters (a key nested three deep on a long line).
struct SjVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj };
    Kind kind = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<SjVal> arr;
    // insertion order, and DUPLICATES ARE KEPT: "compact" written twice is a
    // fact about the file the reader has to be able to name, and a std::map
    // would have eaten one of them before anybody could.
    std::vector<std::pair<std::string, SjVal>> obj;
    int line = 0, col = 0;
};

static const char* sjKindName(SjVal::Kind k) {
    switch (k) {
        case SjVal::Null: return "null";
        case SjVal::Bool: return "true or false";
        case SjVal::Num:  return "a number";
        case SjVal::Str:  return "a string";
        case SjVal::Arr:  return "an array";
        case SjVal::Obj:  return "an object";
    }
    return "?";
}

// ---- the parser -------------------------------------------------------------
// Hand-written, and not a vendored JSON library (判断2). The reason is not size:
// it is the SHAPE OF THE REFUSAL. §5.3 asks for two different granularities -
// the whole file refused with a position, and one key refused with the rest of
// the file still alive - and a general parser's exception is neither.
struct SjParser {
    const std::string& src;
    size_t i = 0;
    int line = 1, col = 1;
    std::string err;                 // non-empty = the file is refused
    int errLine = 0, errCol = 0;
    int depth = 0;
    // A guard, not a schema rule. The schema is two levels deep (section.key)
    // plus one for loading.raw, but a file of 100000 open brackets must refuse
    // rather than recurse until the stack goes - and it must refuse BY NAME.
    static const int MAX_DEPTH = 16;

    explicit SjParser(const std::string& s) : src(s) {}

    bool bad() const { return !err.empty(); }
    void fail(const std::string& why) {
        if (bad()) return;                       // the FIRST error is the news
        err = why; errLine = line; errCol = col;
    }
    void failAt(int l, int c, const std::string& why) {
        if (bad()) return;
        err = why; errLine = l; errCol = c;
    }
    int peek() const { return i < src.size() ? (unsigned char)src[i] : -1; }
    int at(size_t k) const { return i + k < src.size() ? (unsigned char)src[i + k] : -1; }
    void bump() {
        if (i >= src.size()) return;
        // \r\n counts as ONE line break, so the numbers this prints are the
        // numbers the user's editor shows on a file saved on Windows.
        if (src[i] == '\n') { line++; col = 1; }
        else if (src[i] != '\r') col++;
        i++;
    }

    void skipTrivia() {
        for (;;) {
            int c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { bump(); continue; }
            if (c == '/' && at(1) == '/') {
                while (peek() != -1 && peek() != '\n') bump();
                continue;
            }
            if (c == '/' && at(1) == '*') {
                int sl = line, sc = col;
                bump(); bump();
                for (;;) {
                    if (peek() == -1) {
                        failAt(sl, sc, "this /* comment is never closed");
                        return;
                    }
                    if (peek() == '*' && at(1) == '/') { bump(); bump(); break; }
                    bump();
                }
                continue;
            }
            // A lone '/' is the everyday typo of a half-typed comment, and
            // saying "unexpected character" for it would be true and useless.
            if (c == '/') { fail("a lone '/' - a comment is // or /* */"); return; }
            return;
        }
    }

    void utf8Append(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        out = 0;
        for (int k = 0; k < 4; k++) {
            int c = peek();
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            out = out * 16 + (unsigned)d;
            bump();
        }
        return true;
    }

    bool parseString(std::string& out) {
        out.clear();
        if (peek() != '"') {
            // The two JSON5-isms a user actually types. Naming the dialect here
            // is worth more than "unexpected character": the fix is mechanical
            // once you know which JSON this is.
            if (peek() == '\'')
                fail("a single-quoted string - this is JSONC (JSON + comments + "
                     "trailing commas), and JSON strings use double quotes");
            else
                fail("a string was expected here (JSONC keys and text are "
                     "double-quoted)");
            return false;
        }
        bump();
        for (;;) {
            int c = peek();
            if (c == -1) { fail("this string is never closed"); return false; }
            if (c == '"') { bump(); return true; }
            if (c == '\n') { fail("a string may not run past the end of its line"); return false; }
            if (c < 0x20) { fail("a control character in a string must be escaped"); return false; }
            if (c != '\\') { out += (char)c; bump(); continue; }
            bump();
            int e = peek();
            switch (e) {
                case '"':  out += '"';  bump(); break;
                case '\\': out += '\\'; bump(); break;
                case '/':  out += '/';  bump(); break;
                case 'b':  out += '\b'; bump(); break;
                case 'f':  out += '\f'; bump(); break;
                case 'n':  out += '\n'; bump(); break;
                case 'r':  out += '\r'; bump(); break;
                case 't':  out += '\t'; bump(); break;
                case 'u': {
                    bump();
                    unsigned cp = 0;
                    if (!hex4(cp)) { fail("\\u needs exactly four hex digits"); return false; }
                    // A surrogate PAIR is one character written as two escapes:
                    // a path with an emoji in it arrives this way from an editor
                    // that escapes non-BMP text, and joining them here is the
                    // difference between the path and two replacement squares.
                    if (cp >= 0xD800 && cp <= 0xDBFF && peek() == '\\' && at(1) == 'u') {
                        size_t save = i; int sl = line, sc = col;
                        bump(); bump();
                        unsigned lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else { i = save; line = sl; col = sc; }
                    }
                    utf8Append(out, cp);
                    break;
                }
                default:
                    fail(std::string("\\") + (e == -1 ? '?' : (char)e) +
                         " is not an escape JSON knows (\" \\ / b f n r t uXXXX)");
                    return false;
            }
        }
    }

    // JSON's number grammar, strictly. Rejecting +1, .5, 01, 0x10, NaN and
    // Infinity is not pedantry - it is the promise that this file means the
    // same thing here as it does in the editor's JSONC mode.
    bool parseNumber(SjVal& v) {
        size_t start = i;
        if (peek() == '-') bump();
        if (peek() == '0') {
            bump();
            if (peek() >= '0' && peek() <= '9') {
                fail("a number may not have a leading zero");
                return false;
            }
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') bump();
        } else {
            fail("a number was expected here");
            return false;
        }
        if (peek() == '.') {
            bump();
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("a decimal point needs a digit after it");
                return false;
            }
            while (peek() >= '0' && peek() <= '9') bump();
        }
        if (peek() == 'e' || peek() == 'E') {
            bump();
            if (peek() == '+' || peek() == '-') bump();
            if (!(peek() >= '0' && peek() <= '9')) {
                fail("an exponent needs at least one digit");
                return false;
            }
            while (peek() >= '0' && peek() <= '9') bump();
        }
        // 0x10 lands here: the '0' parsed, and 'x' is still sitting there.
        int c = peek();
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '.' || c == '_') {
            fail(std::string("'") + (char)c + "' cannot follow a number "
                 "(JSON has no hex, no suffix and no 'Infinity')");
            return false;
        }
        v.kind = SjVal::Num;
        v.num = strtod(src.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    bool word(const char* w) {
        size_t n = strlen(w);
        if (src.compare(i, n, w) != 0) return false;
        int nx = at(n);
        if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z') ||
            (nx >= '0' && nx <= '9') || nx == '_') return false;
        for (size_t k = 0; k < n; k++) bump();
        return true;
    }

    bool parseValue(SjVal& v) {
        skipTrivia();
        if (bad()) return false;
        v.line = line; v.col = col;
        int c = peek();
        if (c == -1) { fail("the file ends where a value was expected"); return false; }
        if (c == '{') {
            if (++depth > MAX_DEPTH) { fail("nested too deeply to be a settings file"); return false; }
            bump();
            v.kind = SjVal::Obj;
            for (;;) {
                skipTrivia();
                if (bad()) return false;
                if (peek() == '}') { bump(); depth--; return true; }
                int kl = line, kc = col;
                // The two everyday JSON5-isms, each named as itself. "unexpected
                // character" here would be true and useless: the fix is
                // mechanical once you know which JSON this is.
                if (peek() != '"') {
                    failAt(kl, kc, peek() == '\''
                        ? "a key must be a DOUBLE-quoted string - this is JSONC "
                          "(JSON + comments + trailing commas) and has no single quotes"
                        : "a key must be a double-quoted string (JSONC has no "
                          "unquoted keys)");
                    return false;
                }
                std::string key;
                if (!parseString(key)) return false;
                skipTrivia();
                if (peek() != ':') { fail("':' was expected after the key"); return false; }
                bump();
                SjVal child;
                if (!parseValue(child)) return false;
                // The position carried onward is the KEY's, not the value's:
                // that is the line the user has to go and edit, and for a
                // multi-line array the value's position is the wrong one.
                child.line = kl; child.col = kc;
                v.obj.push_back({ key, child });
                skipTrivia();
                if (peek() == ',') { bump(); continue; }   // trailing comma: the
                if (peek() == '}') { bump(); depth--; return true; }  // loop top takes it
                fail("',' or '}' was expected here");
                return false;
            }
        }
        if (c == '[') {
            if (++depth > MAX_DEPTH) { fail("nested too deeply to be a settings file"); return false; }
            bump();
            v.kind = SjVal::Arr;
            for (;;) {
                skipTrivia();
                if (bad()) return false;
                if (peek() == ']') { bump(); depth--; return true; }
                SjVal child;
                if (!parseValue(child)) return false;
                v.arr.push_back(child);
                skipTrivia();
                if (peek() == ',') { bump(); continue; }
                if (peek() == ']') { bump(); depth--; return true; }
                fail("',' or ']' was expected here");
                return false;
            }
        }
        if (c == '"' || c == '\'') {
            std::string s;
            if (!parseString(s)) return false;
            v.kind = SjVal::Str; v.str = s;
            return true;
        }
        if (word("true"))  { v.kind = SjVal::Bool; v.b = true;  return true; }
        if (word("false")) { v.kind = SjVal::Bool; v.b = false; return true; }
        // null PARSES and is then refused as a value (判断9). It has to parse,
        // or the file would be refused whole for one key - which is exactly the
        // punishment §5.3 says not to hand out.
        if (word("null"))  { v.kind = SjVal::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(v);
        fail(std::string("'") + (char)c + "' cannot start a value");
        return false;
    }
};

// Parse a whole settings document. false = the file is refused (判断7), with
// `whyOut` carrying "line:col: reason" - the caller prefixes the file name.
static bool sjParseDocument(const std::string& text, SjVal& out, std::string& whyOut) {
    // A UTF-8 BOM is what Notepad puts in front of a file it saved as UTF-8.
    // Refusing a settings file because of three bytes the user cannot see is
    // the worst possible first experience of hand-editing one.
    size_t start = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) start = 3;
    std::string body = text.substr(start);
    SjParser p(body);
    if (!p.parseValue(out)) {
        whyOut = std::to_string(p.errLine) + ":" + std::to_string(p.errCol) + ": " + p.err;
        return false;
    }
    p.skipTrivia();
    if (p.bad()) {
        whyOut = std::to_string(p.errLine) + ":" + std::to_string(p.errCol) + ": " + p.err;
        return false;
    }
    if (p.peek() != -1) {
        whyOut = std::to_string(p.line) + ":" + std::to_string(p.col) +
                 ": there is more text after the closing '}' - a settings file "
                 "holds exactly one object";
        return false;
    }
    if (out.kind != SjVal::Obj) {
        whyOut = std::to_string(out.line) + ":" + std::to_string(out.col) +
                 ": the settings file must be an object - the whole file goes "
                 "inside { }";
        return false;
    }
    return true;
}

// ---- the schema -------------------------------------------------------------
// ONE table. The applier switches on the id, the spelling suggester searches
// the paths, and the template generator walks the same rows - so a key cannot
// exist for one of the three and not the others. --settings-selftest S1 walks
// this table and feeds every readable row its own sample, which is what stops
// a row being added here and nowhere else.
enum SKey {
    SK_None = 0,
    SK_theme, SK_accent, SK_compact, SK_titleBar, SK_showFps, SK_pixelGrid, SK_gamma,
    SK_dragPans, SK_wheelZoom, SK_fitOnSwitch,
    SK_grouped, SK_tree, SK_naturalSort,
    SK_stackPrompt,
    SK_peerExe, SK_policy, SK_lowBandwidth, SK_repoUrl,
    SK_pythonExe, SK_searchPath, SK_editor,
    SK_defaultUnit
};

enum SStatus {
    SS_Read,        // this version reads it and it takes effect
    SS_Later,       // a real key, named in the inventory, NOT read yet
    SS_NotHere      // deliberately not a setting in this file, with the reason
};

struct SKeyDef {
    SKey id;
    const char* path;
    SStatus status;
    const char* note;        // SS_Later / SS_NotHere: the sentence the user gets
    const char* sample;      // SS_Read: a valid literal, for the schema walk
};

static const SKeyDef SETTING_KEYS[] = {
    { SK_theme,       "appearance.theme",        SS_Read, nullptr, "\"light\"" },
    { SK_accent,      "appearance.accent",       SS_Read, nullptr, "1" },
    { SK_compact,     "appearance.compact",      SS_Read, nullptr, "false" },
    { SK_titleBar,    "appearance.titleBar",     SS_Read, nullptr, "\"system\"" },
    { SK_showFps,     "appearance.showFps",      SS_Read, nullptr, "true" },
    { SK_pixelGrid,   "appearance.pixelGrid",    SS_Read, nullptr, "true" },
    { SK_gamma,       "appearance.displayGamma", SS_Read, nullptr, "2.2" },

    { SK_dragPans,    "input.dragPans",          SS_Read, nullptr, "true" },
    { SK_wheelZoom,   "input.wheelZoomPlain",    SS_Read, nullptr, "true" },
    { SK_fitOnSwitch, "input.fitOnSwitch",       SS_Read, nullptr, "true" },
    // 判断14: keybindings are not in v1. The PLACE is held open here rather
    // than in a comment alone, so that someone who writes the key gets the
    // answer "not yet" instead of "no such key" - those are different facts.
    { SK_None,        "input.keyBindings",       SS_Later,
      "keybindings do not exist anywhere in core/ yet (settings-inventory 3.8, "
      "judgment 14) - this is the place they will go", nullptr },

    { SK_grouped,     "browse.grouped",          SS_Read, nullptr, "false" },
    { SK_tree,        "browse.tree",             SS_Read, nullptr, "true" },
    { SK_naturalSort, "browse.naturalSort",      SS_Read, nullptr, "false" },
    { SK_None,        "browse.folderActivate",   SS_Later,
      "single/double click on a folder is fixed behaviour today (board row 136)"
      " - there is no mechanism for this to switch yet", nullptr },
    { SK_None,        "browse.sortColumn",       SS_Later,
      "the Browse sort column is not persisted anywhere yet "
      "(settings-inventory 3.7)", nullptr },
    { SK_None,        "browse.sortDescending",   SS_Later,
      "the Browse sort direction is not persisted anywhere yet "
      "(settings-inventory 3.7)", nullptr },

    { SK_stackPrompt, "loading.stackPrompt",     SS_Read, nullptr, "\"never\"" },
    { SK_None,        "loading.raw",             SS_Later,
      "the raw defaults are the measuring class and move in their own change "
      "(settings-inventory 5.5, judgment 10)", nullptr },

    { SK_peerExe,     "remote.peerExecutable",   SS_Read, nullptr, "\"/opt/viewer-serve\"" },
    { SK_policy,      "remote.policy",           SS_Read, nullptr, "\"local-fetch\"" },
    { SK_lowBandwidth,"remote.lowBandwidth",     SS_Read, nullptr, "true" },
    { SK_repoUrl,     "remote.repoUrl",          SS_Read, nullptr, "\"https://example.invalid/f.git\"" },
    // 判断18 as rewritten: these two are not "an env var that wins", they are
    // NOT ON THIS MACHINE. core/serve.cpp reads them inside the peer process,
    // which runs on the far side of an ssh connection; a file here cannot
    // reach it. The reason a setting is refused is worth more than the refusal.
    { SK_None,        "remote.pluginPath",       SS_NotHere,
      "VIEWER_SERVE_PLUGINS is read by core/serve.cpp INSIDE THE PEER, on the "
      "other machine - a file on this one cannot reach it (judgment 18)", nullptr },
    { SK_None,        "remote.pinBudgetMB",      SS_NotHere,
      "VIEWER_SERVE_PIN_BUDGET_MB is read by core/serve.cpp INSIDE THE PEER, on "
      "the other machine - a file on this one cannot reach it (judgment 18)", nullptr },

    { SK_pythonExe,   "readers.pythonExecutable",SS_Read, nullptr, "\"/usr/bin/python3\"" },
    { SK_searchPath,  "readers.searchPath",      SS_Read, nullptr, "[\"/tmp/r\"]" },
    { SK_editor,      "readers.editor",          SS_Read, nullptr, "\"micro\"" },

    { SK_defaultUnit, "series.defaultUnit",      SS_Read, nullptr, "\"cd/m2\"" },

    // 判断10/11: the measuring class gets its own section, carries no defaults
    // there, and the session beats it. None of that is built yet, so the key is
    // named as "later" rather than read - a measuring setting that is read but
    // does not declare itself on the result would break M1 on its first day.
    { SK_None,        "measuring.memoryBudgetGB", SS_Later,
      "the measuring section moves in its own change: it has to declare itself "
      "on the results it changed (judgment 10/11, M1)", nullptr },
};
static const size_t SETTING_KEY_COUNT = sizeof SETTING_KEYS / sizeof SETTING_KEYS[0];

// Sections the file may carry. A name that is not here is a mistyped SECTION,
// and saying that once beats saying "unknown key" seven times under it.
static const char* SETTING_SECTIONS[] = {
    "appearance", "input", "browse", "loading", "panels", "remote", "readers",
    "series", "measuring"
};
static const size_t SETTING_SECTION_COUNT =
    sizeof SETTING_SECTIONS / sizeof SETTING_SECTIONS[0];

// panels is a whole section this version does not read yet: it needs the
// "clean start" question settled against layout.ini first (settings-inventory
// 5.5). Registered as a section so its keys are not each called unknown.
static bool sectionIsLater(const std::string& s) { return s == "panels"; }

static size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i2 = 1; i2 <= a.size(); i2++) {
        cur[0] = i2;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t sub = prev[j - 1] + (tolower((unsigned char)a[i2 - 1]) ==
                                        tolower((unsigned char)b[j - 1]) ? 0 : 1);
            cur[j] = std::min(sub, std::min(prev[j] + 1, cur[j - 1] + 1));
        }
        prev = cur;
    }
    return prev[b.size()];
}

// The nearest READABLE key path, or "" when nothing is near enough. §5.2: a
// settings file's unknown key is a typo as often as not, and a suggestion is
// what turns "skipped" into something the user can act on.
static std::string nearestSettingKey(const std::string& path) {
    std::string best;
    size_t bestD = (size_t)-1;
    for (size_t k = 0; k < SETTING_KEY_COUNT; k++) {
        size_t d = editDistance(path, SETTING_KEYS[k].path);
        if (d < bestD) { bestD = d; best = SETTING_KEYS[k].path; }
    }
    // 3 edits on a dotted path is generous for a typo and tight enough that
    // "browse.zzz" is not told it meant "browse.tree".
    return bestD <= 3 ? best : std::string();
}

// ---- the report -------------------------------------------------------------
struct SettingsReport {
    bool fileExists = false;
    bool parsed = false;              // false with fileExists = 判断7 refusal
    std::string parseError;           // "settings.jsonc:14:3: ..."
    std::vector<std::string> notes;   // every named line, in file order
    std::vector<std::string> overrode;// keys whose value prefs.txt held otherwise
    int applied = 0, rejected = 0, unknown = 0, later = 0, notHere = 0;
};

static SettingsReport g_settings;     // what the last load said
// The one sentence that has to reach THE SCREEN (§5.3), held until a screen
// exists. loadSettings() runs before ImGui does - it has to, because the
// settings decide the title bar and the title bar is chosen while the window is
// being created - and toast() guards itself against a null context (it must:
// the windowless selftests would segfault otherwise). That guard turns an early
// toast into one whose deadline is already past, i.e. a message that lands in
// the log and is never drawn. The log alone is not "on screen": nobody opens
// Messages to find out why the file they just wrote did nothing.
static std::string g_settingsToast;
// remote.repoUrl (判断17): the build-time VIEWER_REPO_URL stays the default,
// and a fork's user has no reason to rebuild to change where the peer comes
// from. Empty = not set, which is 判断9 in one variable.
static std::string g_settingsRepoUrl;
static const char* repoUrlInUse() {
    return g_settingsRepoUrl.empty() ? VIEWER_REPO_URL : g_settingsRepoUrl.c_str();
}
// readers.editor (判断18). Empty = not set, and the $EDITOR -> `code -g` -> OS
// chain in core/adapter.cpp is then exactly what it always was. Handed to
// adapter:: at the end of every load, because that TU is deliberately free of
// the viewer's state and must be TOLD rather than reach in.
static std::string g_settingsEditorCmd;

// ---- applying ---------------------------------------------------------------
namespace {

struct SjApply {
    SettingsReport* rep;
    std::string file;                 // display name, e.g. "settings.jsonc"

    std::string at(const SjVal& v) const {
        return file + ":" + std::to_string(v.line) + ":" + std::to_string(v.col) + ": ";
    }
    void note(const std::string& s) { rep->notes.push_back(s); }

    // 判断8: one key refused, the rest of the file alive. 判断9: null is a TYPE
    // ERROR and never "explicitly the default" - the way back to a default is
    // to delete the key, and a third state would need the three-valued bucket
    // terminology.md:105-108 already turned down.
    bool typeErr(const SjVal& v, const char* path, const char* want) {
        rep->rejected++;
        if (v.kind == SjVal::Null)
            note(at(v) + "\"" + path + "\" is null - null is not a value here; "
                 "delete the key to get the default back");
        else
            note(at(v) + "\"" + path + "\" wants " + want + ", not " +
                 sjKindName(v.kind) + " - the key is skipped and nothing else in "
                 "the file is affected");
        return false;
    }
    void rangeErr(const SjVal& v, const char* path, const std::string& want) {
        rep->rejected++;
        note(at(v) + "\"" + path + "\" wants " + want + " - the key is skipped");
    }

    bool wantBool(const SjVal& v, const char* path, bool& out) {
        if (v.kind != SjVal::Bool) return typeErr(v, path, "true or false");
        out = v.b; return true;
    }
    bool wantNum(const SjVal& v, const char* path, double& out) {
        if (v.kind != SjVal::Num) return typeErr(v, path, "a number");
        out = v.num; return true;
    }
    bool wantStr(const SjVal& v, const char* path, std::string& out) {
        if (v.kind != SjVal::Str) return typeErr(v, path, "a string");
        out = v.str; return true;
    }
    // The enums print their whole alphabet in the refusal: "wants one of" with
    // the list is the difference between a message and an instruction.
    bool wantEnum(const SjVal& v, const char* path,
                  const std::vector<const char*>& words, int& out) {
        std::string s;
        if (!wantStr(v, path, s)) return false;
        for (size_t k = 0; k < words.size(); k++)
            if (s == words[k]) { out = (int)k; return true; }
        std::string want = "one of";
        for (size_t k = 0; k < words.size(); k++)
            want += std::string(k ? ", " : " ") + "\"" + words[k] + "\"";
        rangeErr(v, path, want + " (got \"" + s + "\")");
        return false;
    }

    // Every setter goes through these so that "the file changed something
    // prefs.txt held otherwise" is recorded once, in one place, rather than
    // being a thing each key remembers to do.
    void setB(bool& field, bool nv, const char* path) {
        if (field != nv) rep->overrode.push_back(path);
        field = nv; rep->applied++;
    }
    void setI(int& field, int nv, const char* path) {
        if (field != nv) rep->overrode.push_back(path);
        field = nv; rep->applied++;
    }
    void setF(float& field, float nv, const char* path) {
        if (field != nv) rep->overrode.push_back(path);
        field = nv; rep->applied++;
    }
    void setS(std::string& field, const std::string& nv, const char* path) {
        if (field != nv) rep->overrode.push_back(path);
        field = nv; rep->applied++;
    }
};

}  // namespace

static void sjApplyKey(SjApply& A, const SKeyDef& d, const SjVal& v) {
    const char* path = d.path;
    switch (d.id) {
        case SK_theme: {
            int idx = 0;
            if (A.wantEnum(v, path, { "dark", "light" }, idx))
                A.setI(app.themeVariant, idx ? ui_theme::VariantLight : ui_theme::VariantDark, path);
            break;
        }
        case SK_accent: {
            double n = 0;
            if (!A.wantNum(v, path, n)) break;
            int hi = ui_theme::accentCount() - 1;
            if (n != floor(n) || n < 0 || n > hi) {
                A.rangeErr(v, path, "a whole number from 0 to " + std::to_string(hi));
                break;
            }
            A.setI(app.themeAccent, (int)n, path);
            break;
        }
        case SK_compact: { bool b; if (A.wantBool(v, path, b)) A.setB(app.compactUi, b, path); break; }
        case SK_titleBar: {
            int idx = 0;
            // "system" is 0 and "integrated" is 1, the same two numbers
            // prefs.txt has carried since the frame was made switchable.
            if (A.wantEnum(v, path, { "system", "integrated" }, idx))
                A.setI(app.frameMode, idx, path);
            break;
        }
        case SK_showFps:   { bool b; if (A.wantBool(v, path, b)) A.setB(app.showFps, b, path); break; }
        case SK_pixelGrid: { bool b; if (A.wantBool(v, path, b)) A.setB(app.showGrid, b, path); break; }
        case SK_gamma: {
            double n = 0;
            if (!A.wantNum(v, path, n)) break;
            // Two values, not a range: the display path has exactly these. A
            // 1.8 silently rounded to 2.2 is the "I set it and nothing
            // happened" shape this whole file exists to stop.
            if (n != 1.0 && n != 2.2) {
                A.rangeErr(v, path, "1.0 or 2.2 (the only two the display path has)");
                break;
            }
            A.setF(app.dispGamma, (float)n, path);
            break;
        }
        case SK_dragPans:    { bool b; if (A.wantBool(v, path, b)) A.setB(app.dragPans, b, path); break; }
        case SK_wheelZoom:   { bool b; if (A.wantBool(v, path, b)) A.setB(app.wheelZoomPlain, b, path); break; }
        case SK_fitOnSwitch: { bool b; if (A.wantBool(v, path, b)) A.setB(app.fitOnSwitch, b, path); break; }
        // "grouped" is the word on screen; rbFlat is the field, and it is the
        // NEGATION. Spelling the file after the UI rather than after the
        // variable is deliberate - the user is reading the panel, not the code.
        case SK_grouped:     { bool b; if (A.wantBool(v, path, b)) A.setB(app.rbFlat, !b, path); break; }
        case SK_tree:        { bool b; if (A.wantBool(v, path, b)) A.setB(app.rbTree, b, path); break; }
        case SK_naturalSort: { bool b; if (A.wantBool(v, path, b)) A.setB(app.rbNatural, b, path); break; }
        case SK_stackPrompt: {
            int idx = 0;
            if (A.wantEnum(v, path, { "ask", "always", "never" }, idx))
                A.setI(app.seqLoadMode, idx, path);
            break;
        }
        case SK_peerExe: { std::string s; if (A.wantStr(v, path, s)) A.setS(app.remoteExe, s, path); break; }
        case SK_policy: {
            int idx = 0;
            // PolServer (1) is retired, so the file offers the two that exist.
            if (A.wantEnum(v, path, { "auto", "local-fetch" }, idx))
                A.setI(app.procPolicy, idx ? App::PolLocalFetch : App::PolAuto, path);
            break;
        }
        case SK_lowBandwidth: { bool b; if (A.wantBool(v, path, b)) A.setB(app.lowBandwidth, b, path); break; }
        case SK_repoUrl: { std::string s; if (A.wantStr(v, path, s)) A.setS(g_settingsRepoUrl, s, path); break; }
        case SK_pythonExe: { std::string s; if (A.wantStr(v, path, s)) A.setS(app.pythonExe, s, path); break; }
        case SK_searchPath: {
            if (v.kind != SjVal::Arr) { A.typeErr(v, path, "an array of strings"); break; }
            std::vector<std::string> places;
            bool ok = true;
            for (const SjVal& e : v.arr) {
                if (e.kind != SjVal::Str) { A.typeErr(e, path, "an array of strings"); ok = false; break; }
                if (!e.str.empty()) places.push_back(e.str);
            }
            if (!ok) break;
            // The same 32 loadPrefs enforces on the way in. A cap silently
            // applied is a list the user cannot see the end of.
            if (places.size() > 32) {
                A.rangeErr(v, path, "at most 32 places (this one has " +
                                    std::to_string(places.size()) + ")");
                break;
            }
            if (app.readerPlaces != places) A.rep->overrode.push_back(path);
            app.readerPlaces = places;
            A.rep->applied++;
            break;
        }
        case SK_editor: {
            std::string s;
            if (!A.wantStr(v, path, s)) break;
            A.setS(g_settingsEditorCmd, s, path);
            break;
        }
        case SK_defaultUnit: {
            std::string s;
            if (!A.wantStr(v, path, s)) break;
            if (s.size() >= sizeof app.lin.unit) {
                A.rangeErr(v, path, "at most " +
                           std::to_string(sizeof app.lin.unit - 1) + " characters");
                break;
            }
            std::string was = app.lin.unit;
            if (was != s) A.rep->overrode.push_back(path);
            snprintf(app.lin.unit, sizeof app.lin.unit, "%s", s.c_str());
            A.rep->applied++;
            break;
        }
        case SK_None: break;   // SS_Later / SS_NotHere never reach here
    }
}

// Apply a parsed document. The document is walked in FILE ORDER, so the notes
// come out in the order the user would read them.
static void sjApplyDocument(const SjVal& doc, SettingsReport& rep,
                            const std::string& fileName) {
    SjApply A{ &rep, fileName };
    auto pos = [&](const SjVal& v) {
        return fileName + ":" + std::to_string(v.line) + ":" + std::to_string(v.col) + ": ";
    };
    // Duplicates, named. Two spellings of the same key in a hand-edited file is
    // the quietest way to "I set it and nothing happened": JSON's last-wins is
    // correct and invisible, so the invisibility is what gets fixed here.
    auto namedDuplicates = [&](const std::vector<std::pair<std::string, SjVal>>& members,
                               const std::string& prefix) {
        for (size_t a = 0; a < members.size(); a++)
            for (size_t b2 = a + 1; b2 < members.size(); b2++)
                if (members[a].first == members[b2].first) {
                    rep.notes.push_back(
                        pos(members[b2].second) + "\"" + prefix + members[a].first +
                        "\" is written twice (also at line " +
                        std::to_string(members[a].second.line) +
                        ") - the LAST one is the one that counts");
                    break;
                }
    };

    namedDuplicates(doc.obj, "");
    for (const auto& sec : doc.obj) {
        const std::string& sname = sec.first;
        bool known = false;
        for (size_t k = 0; k < SETTING_SECTION_COUNT; k++)
            if (sname == SETTING_SECTIONS[k]) known = true;
        if (!known) {
            // A scalar at the top level is almost always a section forgotten,
            // so look for a key whose LEAF is this name and point at it.
            if (sec.second.kind != SjVal::Obj) {
                std::string leafHit;
                for (size_t k = 0; k < SETTING_KEY_COUNT; k++) {
                    std::string p = SETTING_KEYS[k].path;
                    if (p.substr(p.find('.') + 1) == sname) leafHit = p;
                }
                rep.unknown++;
                rep.notes.push_back(pos(sec.second) + "\"" + sname +
                    "\" is not a top-level key" +
                    (leafHit.empty() ? std::string(" - every setting lives inside a section")
                                     : std::string(" - did you mean \"") + leafHit + "\"?"));
                continue;
            }
            rep.unknown++;
            std::string near2;
            size_t bestD = (size_t)-1;
            for (size_t k = 0; k < SETTING_SECTION_COUNT; k++) {
                size_t d = editDistance(sname, SETTING_SECTIONS[k]);
                if (d < bestD) { bestD = d; near2 = SETTING_SECTIONS[k]; }
            }
            rep.notes.push_back(pos(sec.second) + "\"" + sname +
                "\" is not a settings section" +
                (bestD <= 3 ? std::string(" - did you mean \"") + near2 + "\"?" : std::string()) +
                " (" + std::to_string(sec.second.obj.size()) + " key(s) under it were not read)");
            continue;
        }
        if (sec.second.kind != SjVal::Obj) {
            rep.rejected++;
            rep.notes.push_back(pos(sec.second) + "\"" + sname +
                "\" is a section and wants an object, not " +
                sjKindName(sec.second.kind) + " - nothing under it was read");
            continue;
        }
        if (sectionIsLater(sname)) {
            rep.later++;
            rep.notes.push_back(pos(sec.second) + "\"" + sname +
                "\" is a real section that this version does not read yet: the "
                "startup panel set has to be settled against layout.ini first "
                "(settings-inventory 5.5)");
            continue;
        }
        namedDuplicates(sec.second.obj, sname + ".");
        for (const auto& kv : sec.second.obj) {
            std::string path = sname + "." + kv.first;
            const SKeyDef* def = nullptr;
            for (size_t k = 0; k < SETTING_KEY_COUNT; k++)
                if (path == SETTING_KEYS[k].path) { def = &SETTING_KEYS[k]; break; }
            if (!def) {
                rep.unknown++;
                std::string near2 = nearestSettingKey(path);
                rep.notes.push_back(pos(kv.second) + "\"" + path +
                    "\" is not a key this version knows" +
                    (near2.empty() ? std::string() : " - did you mean \"" + near2 + "\"?") +
                    " (skipped)");
                continue;
            }
            if (def->status == SS_Later) {
                rep.later++;
                rep.notes.push_back(pos(kv.second) + "\"" + path +
                    "\" is a real setting this version does not read yet: " +
                    def->note);
                continue;
            }
            if (def->status == SS_NotHere) {
                rep.notHere++;
                rep.notes.push_back(pos(kv.second) + "\"" + path +
                    "\" is not a setting of this file: " + def->note);
                continue;
            }
            sjApplyKey(A, *def, kv.second);
        }
    }
}

// ---- reading the file -------------------------------------------------------
static std::string settingsPath() {
    // viewerConfigDir(), never a second resolution of its own: §3.4 already
    // carries a duplicate of the APPDATA/HOME rule (board row 185) and a third
    // copy is the same defect one worse.
    std::string dir = viewerConfigDir();
    return dir.empty() ? std::string() : dir + "/settings.jsonc";
}

// Read and apply ONE settings file. Returns the report; nothing here writes.
//
// 判断7 in one sentence: a file that cannot be parsed is refused BY NAME, no
// key from it is applied, and the file is not touched. What the run then has is
// not "the defaults" - it is exactly what prefs.txt and the built-ins gave,
// i.e. the run behaves as though the file were absent. Overwriting it with
// something parseable would destroy the very text the user has to fix.
static SettingsReport loadSettingsRead(const std::string& path) {
    SettingsReport rep;
    std::string name = path;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // The two values this file OWNS outright - they have no prefs.txt layer
    // under them - are cleared first. Every app.* field is deliberately not:
    // "absent" there means "keep what prefs gave", which is the whole shape of
    // the layering. Here "absent" means empty, and a value left over from a
    // previous read would be a setting you can enter and never leave.
    g_settingsRepoUrl.clear();
    g_settingsEditorCmd.clear();
    if (path.empty()) return rep;
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return rep;      // absent is not an error
    rep.fileExists = true;
    SjVal doc;
    std::string why;
    if (!sjParseDocument(std::string(buf.begin(), buf.end()), doc, why)) {
        rep.parseError = name + ":" + why;
        return rep;
    }
    rep.parsed = true;
    sjApplyDocument(doc, rep, name);
    return rep;
}

static SettingsReport loadSettingsFrom(const std::string& path) {
    SettingsReport r = loadSettingsRead(path);
    // adapter.cpp holds no viewer state on purpose, so it is TOLD rather than
    // reaching in. Told on every path through the read, the refusals included:
    // a run that refused the file must not keep an editor an earlier read set.
    adapter::setSettingsEditor(g_settingsEditorCmd);
    return r;
}

// The startup call. Order matters and is asserted by --settings-selftest L5:
// this runs AFTER loadPrefs() (the file beats what the app last was) and BEFORE
// parseCli() applies anything (判断13: a flag is a temporary override).
static void loadSettings() {
    g_settings = loadSettingsFrom(settingsPath());
    const SettingsReport& r = g_settings;
    if (!r.fileExists) return;                      // the common case: silence
    if (!r.parsed) {
        // Both channels, because this is the one failure the user cannot see
        // any other way: their file did nothing and the app looks normal.
        logMsg("settings NOT read: " + r.parseError, true);
        logMsg("settings.jsonc was left exactly as it is - nothing was written "
               "back over it. This run uses prefs.txt and the built-in defaults.",
               true);
        g_settingsToast = "settings.jsonc was NOT read: " + r.parseError;
        return;
    }
    for (const std::string& n : r.notes) logMsg(n, true);
    if (!r.overrode.empty()) {
        // The sticky consequence, said once and by name. Without this line a
        // user toggles something in the menu, quits, restarts, finds it back
        // and has no way at all to learn that their own file did it.
        std::string s = "settings.jsonc set " + std::to_string(r.overrode.size()) +
                        " preference(s) that were saved differently: ";
        for (size_t k = 0; k < r.overrode.size(); k++)
            s += (k ? ", " : "") + r.overrode[k];
        s += " - the file wins; delete a key there to hand it back to the menus";
        logMsg(s);
    }
    if (r.rejected || r.unknown || r.notHere)
        g_settingsToast = "settings.jsonc: " +
            std::to_string(r.rejected + r.unknown + r.notHere) +
            " line(s) were not used - see Messages";
}

// Raised once the context can carry it. Called from main() beside the CJK-font
// toast - the same kind of news at the same moment - and LAST of the two on
// purpose: a settings file that would not parse outranks a font substitution.
static void settingsToastIfAny() {
    if (g_settingsToast.empty()) return;
    toast(g_settingsToast, true);
    g_settingsToast.clear();
}

// ---- the template -----------------------------------------------------------
// THE MIGRATION, and the only thing in this file that produces JSONC.
//
// It is printed to stdout by `viewer --settings-template`, never written to
// disk by the app: 判断3 says the app does not write this file, and a file the
// app creates behind the user's back is a file the user did not write. What it
// contains is the user's OWN current values - i.e. what prefs.txt has - for
// exactly the keys that differ from the built-in default, which is 判断9 in
// executable form: a key at its default is not written, because "chose the
// default" and "never chose" have to stay different when a default changes.
static std::string sjQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\t': o += "\\t";  break;
            case '\r': o += "\\r";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", (unsigned)(unsigned char)c);
                    o += b;
                } else o += c;
        }
    }
    return o + "\"";
}

// One row per readable key: its path, and the value the app is in RIGHT NOW,
// or "" when that value is the built-in default (and so must not be written).
static std::string settingsCurrentValue(SKey id) {
    switch (id) {
        case SK_theme:  return app.themeVariant == ui_theme::VariantDark ? "" : "\"light\"";
        case SK_accent: return app.themeAccent == 0 ? "" : std::to_string(app.themeAccent);
        case SK_compact: return app.compactUi ? "" : "false";
        case SK_titleBar: return app.frameMode == 1 ? "" : "\"system\"";
        case SK_showFps: return app.showFps ? "true" : "";
        case SK_pixelGrid: return app.showGrid ? "true" : "";
        case SK_gamma: return app.dispGamma > 1.5f ? "2.2" : "";
        case SK_dragPans: return app.dragPans ? "true" : "";
        case SK_wheelZoom: return app.wheelZoomPlain ? "true" : "";
        case SK_fitOnSwitch: return app.fitOnSwitch ? "true" : "";
        case SK_grouped: return app.rbFlat ? "false" : "";
        case SK_tree: return app.rbTree ? "true" : "";
        case SK_naturalSort: return app.rbNatural ? "" : "false";
        case SK_stackPrompt:
            return app.seqLoadMode == 0 ? "" :
                   (app.seqLoadMode == 1 ? "\"always\"" : "\"never\"");
        case SK_peerExe: return app.remoteExe.empty() ? "" : sjQuote(app.remoteExe);
        case SK_policy: return app.procPolicy == App::PolLocalFetch ? "\"local-fetch\"" : "";
        case SK_lowBandwidth: return app.lowBandwidth ? "true" : "";
        case SK_repoUrl: return g_settingsRepoUrl.empty() ? "" : sjQuote(g_settingsRepoUrl);
        case SK_pythonExe: return app.pythonExe.empty() ? "" : sjQuote(app.pythonExe);
        case SK_searchPath: {
            if (app.readerPlaces.empty()) return "";
            std::string s = "[";
            for (size_t k = 0; k < app.readerPlaces.size(); k++)
                s += (k ? ", " : "") + sjQuote(app.readerPlaces[k]);
            return s + "]";
        }
        case SK_editor: return g_settingsEditorCmd.empty() ? "" : sjQuote(g_settingsEditorCmd);
        case SK_defaultUnit:
            return strcmp(app.lin.unit, "lx") == 0 ? "" : sjQuote(app.lin.unit);
        case SK_None: return "";
    }
    return "";
}

static std::string settingsTemplateText() {
    std::string o;
    o += "{\n";
    o += "  // viewer settings - %APPDATA%/viewer/settings.jsonc  (Windows)\n";
    o += "  //                   ~/.config/viewer/settings.jsonc  (POSIX)\n";
    o += "  //\n";
    o += "  // JSONC: JSON, plus // and /* */ comments, plus trailing commas.\n";
    o += "  // Not JSON5 - single quotes, unquoted keys and hex numbers are refused,\n";
    o += "  // so that this file means the same thing here as in your editor.\n";
    o += "  //\n";
    o += "  // A key that is NOT WRITTEN is not set. Writing a key at its default\n";
    o += "  // changes nothing today and changes something later: if the default ever\n";
    o += "  // moves, whoever wrote the value keeps it and whoever wrote nothing gets\n";
    o += "  // the new one. To go back to a default, DELETE THE KEY - null is refused.\n";
    o += "  //\n";
    o += "  // This file is yours. The app reads it and never writes it, so your\n";
    o += "  // comments survive. A key here beats the value the app saved in\n";
    o += "  // prefs.txt, and a command-line flag beats this file.\n";
    o += "  //\n";
    o += "  // A key it does not know is skipped AND NAMED in the Messages panel,\n";
    o += "  // with the nearest key it does know. Nothing is ignored in silence.\n";
    o += "  //\n";
    o += "  // Printed by `viewer --settings-template`: below are the settings that\n";
    o += "  // currently differ from the built-in defaults, i.e. yours, and every\n";
    o += "  // other key this version reads, commented out with its default.\n";
    std::string lastSection;
    for (size_t k = 0; k < SETTING_KEY_COUNT; k++) {
        const SKeyDef& d = SETTING_KEYS[k];
        std::string p = d.path;
        std::string sec = p.substr(0, p.find('.'));
        std::string leaf = p.substr(p.find('.') + 1);
        if (sec != lastSection) {
            if (!lastSection.empty()) o += "  },\n";
            o += "\n  " + sjQuote(sec) + ": {\n";
            lastSection = sec;
        }
        if (d.status != SS_Read) {
            // "not yet" and "not here" are different answers and only one of
            // them tells the reader to wait, so the template does not blur
            // them into one sentence either.
            o += "    // " + sjQuote(leaf) + ": " +
                 (d.status == SS_Later ? "not read by this version yet - "
                                       : "not a setting of this file - ") +
                 std::string(d.note) + "\n";
            continue;
        }
        std::string v = settingsCurrentValue(d.id);
        if (v.empty())
            o += "    // " + sjQuote(leaf) + ": <default>\n";
        else
            o += "    " + sjQuote(leaf) + ": " + v + ",\n";
    }
    if (!lastSection.empty()) o += "  },\n";
    o += "}\n";
    return o;
}
