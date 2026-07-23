// NEOTKO_ALHCOLOR_TAG_START — Fase 5.4 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md replanteo
// TD-vs-slope, Frente 2). Consumption of the slope-perimeter recolor plan the Precision ALH
// gizmo stores per-object (opt-in) in ModelObject::config["neotko_slope_perimeter_recolor"]
// — JSON array [{"z_lo":mm,"z_hi":mm,"tools":[t0,...]},...], object-relative Z (same frame
// as print_z / layer_height_profile), 0-based tools, outer ring first.
//
// These free functions are called from MixedFilamentManager::resolve_perimeter() and
// ::ordered_perimeter_extruders() so every consumer — GCode's perimeter split, ToolOrdering's
// per-layer planning — inherits the exact same answer from the exact same source:
// Plan == Emisión by construction. No new material is injected (unlike the Sandwich passes):
// only WHICH tool prints an already-existing perimeter changes, so the wipe tower learns
// about it through its normal planning path.
//
// Lives in its OWN translation unit (not MixedFilament.cpp) on purpose: it needs
// Model.hpp/Print.hpp, whose transitive Color.hpp defines `using RGB = std::array<float,3>`,
// which collides with MixedFilament.cpp's own RGB/RGBf pigment structs.
//
// Safe fallback everywhere: no object / no blob / unparseable blob / z outside every band /
// ring beyond the stored plan / tool out of range → 0 → the normal resolution runs
// unchanged. A corrupt blob can degrade to today's behavior, never crash or misroute.
#include "Model.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "NeoDebug.hpp" // unconditional write()s below — REALCOLOR reused as destination
                        // (feedback_debug_logging_use_neotko_channels: reusing an unrelated
                        // channel purely as a log file is allowed; the user can't find
                        // Orca's own log, /tmp/neotko_realcolor.log they can)

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace slope_recolor {

namespace {

struct Band
{
    double z_lo = 0.0, z_hi = 0.0;
    std::vector<unsigned int> tools_0based; // outer ring first
};

// Strict scanner for the gizmo's own writer format. Anything unexpected aborts to empty.
std::vector<Band> parse_blob(const std::string& blob)
{
    std::vector<Band> out;
    const char* p   = blob.c_str();
    const char* end = p + blob.size();
    auto skip_ws = [&]() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p; };
    auto expect = [&](char c) -> bool { skip_ws(); if (p < end && *p == c) { ++p; return true; } return false; };
    auto read_num = [&](double& v) -> bool {
        skip_ws();
        char* q = nullptr;
        v = std::strtod(p, &q);
        if (q == p) return false;
        p = q;
        return true;
    };
    auto read_key = [&](const char* key) -> bool {
        skip_ws();
        const size_t n = std::strlen(key);
        if (size_t(end - p) < n + 3 || *p != '"' || std::strncmp(p + 1, key, n) != 0 || p[n + 1] != '"')
            return false;
        p += n + 2;
        return expect(':');
    };

    if (!expect('['))
        return out;
    skip_ws();
    if (p < end && *p == ']')
        return out; // empty array
    while (true) {
        Band band;
        double v = 0.0;
        if (!expect('{') || !read_key("z_lo") || !read_num(band.z_lo) || !expect(',')
            || !read_key("z_hi") || !read_num(band.z_hi) || !expect(',')
            || !read_key("tools") || !expect('['))
            return {};
        skip_ws();
        if (p < end && *p == ']') {
            ++p;
        } else {
            while (true) {
                if (!read_num(v) || v < 0.0 || v > 3.0)
                    return {};
                band.tools_0based.push_back((unsigned int)(v));
                skip_ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (expect(']')) break;
                return {};
            }
        }
        if (!expect('}') || band.z_hi < band.z_lo)
            return {};
        out.push_back(std::move(band));
        skip_ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (expect(']')) break;
        return {};
    }
    return out;
}

// Parsed-blob cache keyed by ModelObject pointer, invalidated by comparing the stored raw
// blob string (ModelConfigObject::timestamp() is private, so string compare is the portable
// signal). Mutex-guarded: resolve_perimeter runs from both ToolOrdering and (potentially
// parallel) GCode paths.
std::mutex g_mutex;
struct CacheEntry { std::string raw; std::vector<Band> bands; bool hit_logged = false; };
std::map<const void*, CacheEntry> g_cache;

// Fetch the object's blob string (empty if none). Returns nullptr object-config = "".
const std::string* object_blob(const PrintObject* po)
{
    if (po == nullptr)
        return nullptr;
    const ModelObject* mo = po->model_object();
    if (mo == nullptr)
        return nullptr;
    const auto* opt = dynamic_cast<const ConfigOptionString*>(mo->config.option("neotko_slope_perimeter_recolor"));
    if (opt == nullptr || opt->value.empty())
        return nullptr;
    return &opt->value;
}

// Bands for this object, parsing (and caching) on first use or when the blob string changed.
// Caller must hold g_mutex. `mo` used only as the cache key.
const std::vector<Band>& bands_for(const void* key, const std::string& raw)
{
    if (g_cache.size() > 64)
        g_cache.clear(); // bounded; next lookups re-parse
    CacheEntry& entry = g_cache[key];
    if (entry.raw != raw) {
        entry.raw        = raw;
        entry.bands      = parse_blob(raw);
        entry.hit_logged = false;
        // One line per (re)parse — low volume, unconditional so the user never needs an
        // env var to confirm the blob actually reached the engine and parsed.
        NeoDebug::write(NeoDebug::REALCOLOR,
            "SLOPE_RECOLOR parse: blob_bytes=" + std::to_string(raw.size())
            + " bands=" + std::to_string(entry.bands.size())
            + (entry.bands.empty() ? " (PARSE FAILED or empty — plan will be ignored)" : ""));
        for (const Band& b : entry.bands) {
            std::string tools;
            for (unsigned int t : b.tools_0based)
                tools += (tools.empty() ? "" : ",") + std::to_string(t + 1);
            NeoDebug::write(NeoDebug::REALCOLOR,
                "SLOPE_RECOLOR band: z=[" + std::to_string(b.z_lo) + "," + std::to_string(b.z_hi)
                + "] tools(1based,outer-first)=" + tools);
        }
    }
    return entry.bands;
}

} // namespace

// 1-based physical tool override, or 0 = no override (fall through to normal resolution).
int override_1based(const PrintObject* po, float z, int perimeter_index, size_t num_physical)
{
    // NEOTKO_ALHCOLOR_TAG — s222 fix (user-reported: "the whole slope zone turns solid").
    // The EXTERNAL ring (0) is never overridden: its per-layer alternation IS the pattern's
    // visible texture (its average across layers equals the recipe mix), and forcing it to
    // one tool across a whole z band collapsed the banding to a solid color. Only the
    // INTERIOR rings the slope newly exposes get plan colors — they fill the ledge with a
    // mix-matched dither while the external keeps the pattern's own rhythm.
    if (perimeter_index == 0)
        return 0;
    if (z <= 0.f || perimeter_index < 0 || num_physical == 0)
        return 0;
    const std::string* raw = object_blob(po);
    if (raw == nullptr)
        return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    const std::vector<Band>& bands = bands_for(po->model_object(), *raw);
    for (const Band& b : bands) {
        if (double(z) < b.z_lo || double(z) > b.z_hi)
            continue;
        if (size_t(perimeter_index) < b.tools_0based.size()) {
            const unsigned int tool_1based = b.tools_0based[size_t(perimeter_index)] + 1;
            if (tool_1based >= 1 && tool_1based <= num_physical) {
                CacheEntry& entry = g_cache[po->model_object()];
                if (!entry.hit_logged) {
                    entry.hit_logged = true;
                    NeoDebug::write(NeoDebug::REALCOLOR,
                        "SLOPE_RECOLOR first override hit: z=" + std::to_string(z)
                        + " ring=" + std::to_string(perimeter_index)
                        + " -> tool F" + std::to_string(tool_1based));
                }
                return int(tool_1based);
            }
        }
        return 0; // ring beyond the stored plan → normal resolution
    }
    return 0;
}

// Ring count of the stored plan at this z (0 = no plan there). ToolOrdering's per-layer
// planning must enumerate AT LEAST this many rings — the emission side resolves per actual
// entity ring, so a plan covering more rings than the pattern has comma-groups would
// otherwise put tools on the plate that the planner never saw (the exact Plan != Emisión
// failure this phase is designed to make impossible).
size_t ring_count(const PrintObject* po, float z)
{
    if (z <= 0.f)
        return 0;
    const std::string* raw = object_blob(po);
    if (raw == nullptr)
        return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    const std::vector<Band>& bands = bands_for(po->model_object(), *raw);
    for (const Band& b : bands)
        if (double(z) >= b.z_lo && double(z) <= b.z_hi)
            return b.tools_0based.size();
    return 0;
}

} // namespace slope_recolor
} // namespace Slic3r
// NEOTKO_ALHCOLOR_TAG_END
