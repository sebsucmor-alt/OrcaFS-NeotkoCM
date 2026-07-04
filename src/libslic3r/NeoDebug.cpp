// NEOTKO_DEBUG_TAG_START
// NeoDebug — centralised debug channel implementation.
// One log file per channel, guarded by its env var or ORCA_DEBUG_ALL.
// Thread-safe writes via a single global mutex (debug only, no perf concern).
#include "NeoDebug.hpp"

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

namespace Slic3r {

namespace NeoDebug {
    struct ChanInfo { const char* env_var; const char* log_path; };
    static constexpr ChanInfo k_chans[CH_COUNT] = {
        { "ORCA_DEBUG_COLORMIX",    "/tmp/neotko_colormix.log"    },
        { "ORCA_DEBUG_MULTIPASS",   "/tmp/neotko_multipass.log"   },
        { "ORCA_DEBUG_PENULTIMATE", "/tmp/neotko_penultimate.log" },
        { "ORCA_DEBUG_TOOLORDER",   "/tmp/neotko_toolorder.log"   },
        { "ORCA_DEBUG_ZBLEND",      "/tmp/neotko_zblend.log"      },
        { "ORCA_DEBUG_WIPETOWER",   "/tmp/neotko_wipetower.log"   },
        { "ORCA_DEBUG_PROFILE",     "/tmp/neotko_profile.log"     }, // NEOTKO_PROFILE_TAG
        { "ORCA_DEBUG_DISPATCH",    "/tmp/neotko_dispatch.log"    }, // NEOTKO_NEOARACHNE_TAG s95
        { "ORCA_DEBUG_BOTTOM",      "/tmp/neotko_bottom.log"      }, // NEOTKO_BOTTOM_TAG — Fase 0 instrumentation
        { "ORCA_DEBUG_REALCOLOR",   "/tmp/neotko_realcolor.log"   }, // NEOTKO_REALCOLOR_TAG
        { "ORCA_DEBUG_TEXTUREBUMP", "/tmp/neotko_texturebump.log" }, // NEOTKO_TEXTUREBUMP_TAG
    };

    bool enabled(Channel c)
    {
        // Static arrays — safe: worst case is benign double-init from two threads.
        static bool s_checked[CH_COUNT] = {};
        static bool s_active [CH_COUNT] = {};
        const int idx = static_cast<int>(c);
        if (!s_checked[idx]) {
            s_active [idx] = (std::getenv(k_chans[idx].env_var) != nullptr)
                           || (std::getenv("ORCA_DEBUG_ALL")    != nullptr);
            s_checked[idx] = true;
        }
        return s_active[idx];
    }

    void write(Channel c, const std::string& msg)
    {
        static std::mutex s_mtx;
        std::lock_guard<std::mutex> lk(s_mtx);
        std::ofstream f(k_chans[static_cast<int>(c)].log_path, std::ios::app);
        if (f.is_open()) f << msg << "\n";
    }

    // NEOTKO_DEBUG_TAG s79h — session banner. Writes the same separator line to
    // every channel that is currently active. Process-wide monotonic counter +
    // wall-clock HH:MM:SS so the user can correlate a specific slice across all
    // /tmp/neotko_*.log files. Cheap; only fires once per call.
    void write_session_banner(const std::string& tag)
    {
        static std::atomic<int> s_slice_n{0};
        const int n = ++s_slice_n;

        // Format HH:MM:SS in local time.
        const std::time_t now = std::time(nullptr);
        std::tm tm_local{};
#ifdef _WIN32
        localtime_s(&tm_local, &now);
#else
        localtime_r(&now, &tm_local);
#endif
        char ts[16] = "??:??:??";
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_local);

        std::ostringstream oss;
        oss << "\n============= [" << ts << "] SLICE #" << n
            << "  " << tag << "  =============";
        const std::string banner = oss.str();

        // Write to every channel that is enabled. We bypass enabled() here only
        // to ensure the banner appears even if the channel cache hasn't been
        // queried yet — but still honour env-gating to avoid creating spurious
        // logs for channels the user didn't enable.
        for (int i = 0; i < CH_COUNT; ++i)
            if (enabled(static_cast<Channel>(i)))
                write(static_cast<Channel>(i), banner);
    }
} // namespace NeoDebug

} // namespace Slic3r
// NEOTKO_DEBUG_TAG_END
