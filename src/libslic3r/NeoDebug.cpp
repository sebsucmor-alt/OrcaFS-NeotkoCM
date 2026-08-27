// NEOTKO_DEBUG_TAG_START
// NeoDebug — centralised debug channel implementation.
// One log file per channel, guarded by its env var or ORCA_DEBUG_ALL.
// Thread-safe writes via a single global mutex (debug only, no perf concern).
#include "NeoDebug.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <boost/filesystem.hpp>

namespace Slic3r {

namespace NeoDebug {
    // NEOTKO_NEODEBUG_CONSOLE_TAG s285 — the log path is no longer stored here. It used to be a
    // literal per row, which meant moving the logs anywhere was a 20-line edit; log_path() is now
    // the single owner of where a channel writes. What stays per channel is its env var and a
    // short name, used both for the file name (lowercased) and for the console's row label.
    struct ChanInfo { const char* env_var; const char* name; };
    static constexpr ChanInfo k_chans[CH_COUNT] = {
        { "ORCA_DEBUG_COLORSTITCH", "COLORSTITCH"  },
        { "ORCA_DEBUG_MULTIPASS",   "MULTIPASS"    },
        { "ORCA_DEBUG_PENULTIMATE", "PENULTIMATE"  },
        { "ORCA_DEBUG_TOOLORDER",   "TOOLORDER"    },
        { "ORCA_DEBUG_ZBLEND",      "ZBLEND"       },
        { "ORCA_DEBUG_WIPETOWER",   "WIPETOWER"    },
        { "ORCA_DEBUG_PROFILE",     "PROFILE"      }, // NEOTKO_PROFILE_TAG
        { "ORCA_DEBUG_DISPATCH",    "DISPATCH"     }, // NEOTKO_NEOARACHNE_TAG s95
        { "ORCA_DEBUG_BOTTOM",      "BOTTOM"       }, // NEOTKO_BOTTOM_TAG
        { "ORCA_DEBUG_REALCOLOR",   "REALCOLOR"    }, // NEOTKO_REALCOLOR_TAG
        { "ORCA_DEBUG_TEXTUREBUMP", "TEXTUREBUMP"  }, // NEOTKO_TEXTUREBUMP_TAG
        { "ORCA_DEBUG_ZBUMP",       "ZBUMP"        }, // NEOTKO_ZBUMP_TAG
        { "ORCA_DEBUG_WAVESUPPORT", "WAVESUPPORT"  }, // NEOTKO_WAVESUPPORT_TAG
        { "ORCA_DEBUG_WAVEROOF",    "WAVEROOF"     }, // NEOTKO_WAVESUPPORT_TAG
        { "ORCA_DEBUG_NEOSTITCH",   "NEOSTITCH"    }, // NEOTKO_NEOSTITCH_TAG
        { "ORCA_DEBUG_CONTACT",     "CONTACT"      }, // NEOTKO_CONTACT_TAG s224
        { "ORCA_DEBUG_XOBJ",        "XOBJ"         }, // NEOTKO_XOBJ_TAG s225
        { "ORCA_DEBUG_GRAVITY",     "GRAVITY"      }, // NEOTKO_GRAVITY_TAG s226
        { "ORCA_DEBUG_SHADING",     "SHADING"      }, // NEOTKO_SMOOTHNORMALS_TAG s229
        { "ORCA_DEBUG_FLUTTERDARK", "FLUTTERDARK"  }, // NEOTKO_FLUTTERDARK_TAG s252
        { "ORCA_DEBUG_INFILL",      "INFILL"       }, // NeotkoLIBRE_DBG s133 — folded in s285
        { "ORCA_DEBUG_SUPPORTZONES","SUPPORTZONES" }, // NEOTKO_SUPPORTZONES_TAG s289
    };

    const char* channel_name(Channel c) { return k_chans[static_cast<int>(c)].name; }

    // ---- where the logs live -------------------------------------------------------------------
    std::string log_dir() { return "/tmp/neotko_logs"; }

    std::string log_path(Channel c)
    {
        std::string n = k_chans[static_cast<int>(c)].name;
        for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return log_dir() + "/" + n + ".log";
    }

    // Created on demand, once. If it can't be created we simply fail to log — a debug facility
    // must never take the slicer down with it.
    static void ensure_dir()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            boost::system::error_code ec;
            boost::filesystem::create_directories(log_dir(), ec);
        });
    }

    // ---- live state ------------------------------------------------------------------------------
    // NEOTKO_NEODEBUG_CONSOLE_TAG s285 — the switch positions. Seeded once from the environment
    // and mutable from then on. The env vars keep their exact old meaning: they are the STARTING
    // position of each switch, not a latch. Nothing persists across runs, by decision: the user
    // launches with ORCA_DEBUG_ALL and turns the few noisy ones off when they get in the way.
    static std::atomic<bool> s_on[CH_COUNT];
    static std::atomic<bool> s_paused{false};

    static void init_state()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            const bool all = (std::getenv("ORCA_DEBUG_ALL") != nullptr);
            for (int i = 0; i < CH_COUNT; ++i) {
                const bool own = (std::getenv(k_chans[i].env_var) != nullptr);
                // NEOTKO_GRAVITY_TAG — GRAVITY fires from the Snap & Drag mouse-move handler and
                // can write many MB/s while dragging. It used to be nailed shut with an
                // unconditional `return false` in enabled(). Now it simply starts OFF even under
                // ORCA_DEBUG_ALL: asking for it explicitly still works, and the console can arm
                // it as a conscious decision instead of it being impossible.
                s_on[i].store((i == GRAVITY) ? own : (own || all), std::memory_order_relaxed);
            }
        });
    }

    bool is_enabled(Channel c)
    {
        init_state();
        return s_on[static_cast<int>(c)].load(std::memory_order_relaxed);
    }

    void set_enabled(Channel c, bool on)
    {
        init_state();
        s_on[static_cast<int>(c)].store(on, std::memory_order_relaxed);
    }

    bool paused() { return s_paused.load(std::memory_order_relaxed); }

    void set_paused(bool on) { s_paused.store(on, std::memory_order_relaxed); }

    long long log_size(Channel c)
    {
        boost::system::error_code ec;
        const auto sz = boost::filesystem::file_size(log_path(c), ec);
        return ec ? -1LL : static_cast<long long>(sz);
    }

    // NEOTKO_NEODEBUG_CONSOLE_TAG s285 — REMOVE, not truncate. Truncating left twenty-one 0-byte
    // files sitting there, so after a clear the console showed a wall of "0 B" and you could no
    // longer tell which channels had actually run — which is precisely the question the size
    // column exists to answer. Deleted, a channel only reappears once something writes to it.
    // This is why no site may hold a long-lived handle to a log (see the ofstreams in
    // NeoWipeTower.cpp / ToolOrdering.cpp, which were made per-call for exactly this reason):
    // a handle kept open across a delete would keep writing into an unlinked ghost file.
    void clear(Channel c)
    {
        boost::system::error_code ec;
        boost::filesystem::remove(log_path(c), ec);
    }

    void clear_all()
    {
        for (int i = 0; i < CH_COUNT; ++i)
            clear(static_cast<Channel>(i));
    }

    bool enabled(Channel c)
    {
        // Two relaxed atomic loads and nothing else — a channel that is off has to stay as cheap
        // as it was when the answer was a cached bool.
        return !paused() && is_enabled(c);
    }

    // NEOTKO_SMOOTHNORMALS_TAG s229 — see the header. Only ORCA_DEBUG_RENDER opens the render
    // tuning panels; ORCA_DEBUG_ALL is intentionally not consulted here.
    bool render_panels_enabled()
    {
        static bool s_checked = false;
        static bool s_active  = false;
        if (!s_checked) {
            s_active  = (std::getenv("ORCA_DEBUG_RENDER") != nullptr);
            s_checked = true;
        }
        return s_active;
    }

    // NEOTKO_NEODEBUG_CONSOLE_TAG s285 — see the header for why ORCA_DEBUG_ALL opens this one.
    bool console_enabled()
    {
        static bool s_checked = false;
        static bool s_active  = false;
        if (!s_checked) {
            s_active  = (std::getenv("ORCA_DEBUG_ALL")    != nullptr)
                     || (std::getenv("ORCA_DEBUG_RENDER") != nullptr);
            s_checked = true;
        }
        return s_active;
    }

    void write(Channel c, const std::string& msg)
    {
        ensure_dir();
        static std::mutex s_mtx;
        std::lock_guard<std::mutex> lk(s_mtx);
        std::ofstream f(log_path(c), std::ios::app);
        if (f.is_open()) f << msg << "\n";
    }

    // NEOTKO_DEBUG_TAG s79h — session banner. Writes the same separator line to
    // every channel that is currently active. Process-wide monotonic counter +
    // wall-clock HH:MM:SS so the user can correlate a specific slice across all
    // log files in /tmp/neotko_logs/. Cheap; only fires once per call.
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
