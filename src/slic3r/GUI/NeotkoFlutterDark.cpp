#include "NeotkoFlutterDark.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace Slic3r { namespace GUI {

// NEOTKO_FLUTTERDARK_TAG s252 — see the header for what this is and why it exists.

namespace {

// The bundle is the only file we touch. Everything else the server hands out (their
// index.html, images, fonts, the assets folder) goes through untouched.
const char* const kBundleName = "main.dart.js";

// Their service worker, and the one we hand out in its place. Tested in the real page in s252:
// the bundle asks for it, it installs, wipes Cache Storage, unregisters, and the page loads
// once — no reload loop, because it never calls clients.claim().
const char* const kServiceWorkerName = "flutter_service_worker.js";

const char* const kTombstoneServiceWorker =
    "// NEOTKO_FLUTTERDARK_TAG s252 - replaced by Neotko FullSpectrum.\n"
    "// Snapmaker's service worker cached the app and answered from that cache, which kept\n"
    "// serving the previous theme's colors. Nothing is offline here: the files are local.\n"
    "self.addEventListener('install', function (e) { self.skipWaiting(); });\n"
    "self.addEventListener('activate', function (e) {\n"
    "  e.waitUntil((async function () {\n"
    "    try {\n"
    "      var keys = await caches.keys();\n"
    "      await Promise.all(keys.map(function (k) { return caches.delete(k); }));\n"
    "    } catch (err) {}\n"
    "    try { await self.registration.unregister(); } catch (err) {}\n"
    "  })());\n"
    "});\n";

// The palette literal, as dart2js emits it: B.<obfuscated>=new A.F(4294967295).
// Matched by value, never by name — the identifiers are obfuscated and change on every
// build of theirs, the integer for white does not.
const char* const kNeedle    = "new A.F(";
const size_t      kNeedleLen = 8;

// A recognized bundle carries a couple hundred of these (224 at the time of writing).
// Anything far below that means the format moved under us; then we leave it alone.
const int kMinPaletteSize = 50;

bool ends_with(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void rgb_to_hsl(double r, double g, double b, double& h, double& s, double& l)
{
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    l              = (mx + mn) * 0.5;
    if (mx == mn) {
        h = s = 0.0; // grey: hue is meaningless
        return;
    }
    const double d = mx - mn;
    s              = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r)
        h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g)
        h = (b - r) / d + 2.0;
    else
        h = (r - g) / d + 4.0;
    h /= 6.0;
}

double hue_to_rgb(double p, double q, double t)
{
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5)       return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

void hsl_to_rgb(double h, double s, double l, double& r, double& g, double& b)
{
    if (s == 0.0) {
        r = g = b = l;
        return;
    }
    const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    const double p = 2.0 * l - q;
    r              = hue_to_rgb(p, q, h + 1.0 / 3.0);
    g              = hue_to_rgb(p, q, h);
    b              = hue_to_rgb(p, q, h - 1.0 / 3.0);
}

uint8_t to_byte(double v) { return (uint8_t) std::lround(std::min(1.0, std::max(0.0, v)) * 255.0); }

// The mapping itself, tuned against the real pages in s252.
//
// Only lightness is flipped; hue and saturation are left alone, so their blue stays their
// blue instead of turning orange the way an invert filter would.
//
// Greys (the backgrounds and the text) get their flipped lightness squeezed into 0.08..0.88
// rather than 0..1: pure white must not become pure black — a page of #000 with #FFF text
// is harsh and, worse, hides the panel borders that make their layout readable.
uint32_t darken_argb(uint32_t argb)
{
    const uint32_t a = (argb >> 24) & 0xFF;
    const double   r = ((argb >> 16) & 0xFF) / 255.0;
    const double   g = ((argb >> 8) & 0xFF) / 255.0;
    const double   b = (argb & 0xFF) / 255.0;

    double h, s, l;
    rgb_to_hsl(r, g, b, h, s, l);

    double nl = 1.0 - l;
    if (s < 0.12)
        nl = 0.08 + nl * 0.80;

    double nr, ng, nb;
    hsl_to_rgb(h, s, nl, nr, ng, nb);

    return (a << 24) | ((uint32_t) to_byte(nr) << 16) | ((uint32_t) to_byte(ng) << 8) | (uint32_t) to_byte(nb);
}

// Rewrites every palette literal. Returns the number of colors converted, or 0 if the
// palette does not look like one we know, in which case `out` must be discarded.
int build_dark_bundle(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size() + in.size() / 64);

    int    converted = 0;
    size_t pos       = 0;
    while (true) {
        const size_t hit = in.find(kNeedle, pos);
        if (hit == std::string::npos) {
            out.append(in, pos, std::string::npos);
            break;
        }
        // Copy everything up to and including "new A.F(", then read the integer.
        out.append(in, pos, hit - pos + kNeedleLen);
        size_t p = hit + kNeedleLen;

        uint64_t value  = 0;
        size_t   digits = 0;
        while (p < in.size() && in[p] >= '0' && in[p] <= '9' && digits < 10) {
            value = value * 10 + (uint64_t)(in[p] - '0');
            ++p;
            ++digits;
        }
        // Only a plain "new A.F(<digits>)" is a color constant. Anything else (a variable,
        // an expression, an overlong number) is copied through as-is.
        if (digits == 0 || p >= in.size() || in[p] != ')' || value > 0xFFFFFFFFull) {
            pos = hit + kNeedleLen;
            continue;
        }

        out += std::to_string(darken_argb((uint32_t) value));
        ++converted;
        pos = p; // the ')' is copied with the next chunk
    }

    return converted >= kMinPaletteSize ? converted : 0;
}

std::atomic<bool> g_dark{false};

// One cached dark bundle. Keyed by the source we derived it from, so a Snapmaker update
// (new size, new content) rebuilds instead of serving a stale one.
std::mutex  g_cache_mutex;
std::string g_cache_out;
size_t      g_cache_src_size  = 0;
size_t      g_cache_src_hash  = 0;
bool        g_cache_src_known = false;
bool        g_cache_usable    = false;

} // namespace

void NeotkoFlutterDark::set_dark(bool dark) { g_dark.store(dark, std::memory_order_relaxed); }

bool NeotkoFlutterDark::is_dark() { return g_dark.load(std::memory_order_relaxed); }

bool NeotkoFlutterDark::must_not_be_cached(const std::string& file_path)
{
    return ends_with(file_path, kBundleName) || ends_with(file_path, kServiceWorkerName);
}

bool NeotkoFlutterDark::neutralize_service_worker(const std::string& file_path, std::string& content)
{
    if (!ends_with(file_path, kServiceWorkerName))
        return false;
    content = kTombstoneServiceWorker;
    return true;
}

bool NeotkoFlutterDark::maybe_patch(const std::string& file_path, std::string& content)
{
    if (!is_dark() || !ends_with(file_path, kBundleName) || content.empty())
        return false;

    // Cheap identity for the source: its size plus a hash. Content is what we key on rather
    // than the file's timestamp, because an update rewrites the file in place and we would
    // rather rebuild once too often than serve the previous version's colors.
    const size_t src_size = content.size();
    const size_t src_hash = std::hash<std::string>{}(content);

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (!(g_cache_src_known && g_cache_src_size == src_size && g_cache_src_hash == src_hash)) {
        std::string patched;
        const int   converted = build_dark_bundle(content, patched);

        g_cache_src_size  = src_size;
        g_cache_src_hash  = src_hash;
        g_cache_src_known = true;
        g_cache_usable    = converted > 0;
        g_cache_out       = g_cache_usable ? std::move(patched) : std::string();
    }

    // Palette not recognized: hand back their file untouched and let the page render light.
    // Tinting is a nicety; breaking the only way to drive the printer is not an option.
    if (!g_cache_usable)
        return false;

    content = g_cache_out;
    return true;
}

}} // namespace Slic3r::GUI
