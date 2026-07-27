// The peer that ssh starts on the machine holding the data, as its own binary.
//
// Why separate from `viewer`: the GUI binary links GLFW, OpenGL and X11, and a
// headless compute server usually has none of them - the dynamic loader would
// refuse to start it long before --serve could return. This target links nothing
// but the C++ runtime and miniz, so it runs on a bare machine.
#include "remote_proto.h"
#include <cstring>
#include <cstdio>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                    "viewer-serve: answer image requests on stdin/stdout.\n"
                    "Started for you by the viewer over ssh:\n"
                    "  viewer ssh://user@host/path.npy --remote-exe /path/to/viewer-serve\n");
            return 0;
        }
    }
    return rp::runServeMode();      // --serve is accepted and implied
}
