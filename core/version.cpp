#include "version.h"
#include "viewer_version_generated.h"   // written at build time by cmake/gitversion.cmake

const char* viewerVersion() { return VIEWER_VERSION_STR; }
