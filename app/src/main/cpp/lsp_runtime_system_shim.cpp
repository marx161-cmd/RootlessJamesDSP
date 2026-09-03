// lsp_runtime_system_shim.cpp — minimal Android/Bionic implementation of
// just lsp::system::get_time(), the one symbol from lsp-runtime's
// system.cpp actually needed here (by lsp-dsp-units/util/Randomizer.cpp,
// for RNG seeding — not audio-DSP logic).
//
// The real lsp-runtime/src/main/runtime/system.cpp is a full desktop OS
// abstraction layer (process spawning via posix_spawn, secure_getenv,
// getlogin_r, filesystem volume enumeration, etc.) — none of that compiles
// against Android's Bionic libc, and none of it is needed here. Pulling
// the whole file in was tried and was the wrong move (build errors on
// APIs Bionic doesn't have); this shim provides only the one function
// this build's dependency graph actually calls.
#include <lsp-plug.in/runtime/system.h>
#include <time.h>

namespace lsp
{
    namespace system
    {
        void get_time(time_t *time)
        {
            if (time == nullptr)
                return;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            time->seconds = static_cast<uint64_t>(ts.tv_sec);
            time->nanos   = static_cast<uint32_t>(ts.tv_nsec);
        }
    }
}
