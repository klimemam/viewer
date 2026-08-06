#pragma once
// What this binary was built from. The string is the short commit hash, with
// "+local" appended when the working tree had uncommitted changes, or
// "unknown" when there was no repository to ask.
//
// It lives in its own translation unit so that a new commit relinks one small
// file instead of recompiling main.cpp.
const char* viewerVersion();
