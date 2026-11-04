#pragma once
// One lock for every place this process starts a child (#327).
//
// A child inherits every descriptor (POSIX) or inheritable handle (Windows)
// that is open in the parent at the instant it is started, and this process
// starts children from several threads at once: the fetch worker, the measure
// worker, the browse worker and the UI's own peer, a reader's Python. Pipes are
// born inheritable and only become close-on-exec / non-inheritable a call
// later, so a child started by thread B in that gap carries thread A's pipe
// ends for its whole life. The end that hurts is spawn's exec-error pipe: its
// parent reads it to EOF, and EOF never comes while B's child - a peer that
// lives as long as its Session - holds a copy of the write end. A pair of
// workers connecting at once could each hold the other's, and neither read
// returns until the process is killed.
//
// Held from the first inheritable thing a spawner creates until the child has
// been started and the parent has closed the child's ends - never across a
// wait for the child itself. inline + a function-local static: one mutex in
// the whole program, in whichever binary includes this.
#include <mutex>

inline std::mutex& processSpawnMutex() {
    static std::mutex m;
    return m;
}
