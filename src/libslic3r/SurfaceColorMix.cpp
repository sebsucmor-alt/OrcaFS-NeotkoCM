// NEOTKO_COLORMIX_TAG_START
// Neotko Surface ColorMix + MultiPass Blend — Implementation
// Multi-tool distribution for top/penultimate surface layers
// Author: Neotko
// NEOTKO_COLORMIX_TAG_END

#include "SurfaceColorMix.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "PrintConfig.hpp"
#include "Print.hpp"                  // NEOTKO_PROFILE_TAG — PrintObject access
#include "Model.hpp"                  // NEOTKO_PROFILE_TAG — ModelObject/Volume access
#include "SurfaceEffectProfile.hpp"   // NEOTKO_PROFILE_TAG — painted-profile lookup
#include "GCodeWriter.hpp"  // NEOTKO_NEOWEAVING_TAG — must be outside namespace Slic3r
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <limits>
#include <set>      // NEOTKO_COLORMIX: unique-tool check in build_tool_list_from_pattern
#include <mutex>    // NEOTKO_DEBUG: NeoDebug::write thread safety
#include <numeric>  // NEOTKO_COLORMIX s58: std::iota for lane_mode sort indices
#include <nlohmann/json.hpp> // NEOTKO_PATHBLEND_TAG s69: miniblob JSON round-trip

namespace Slic3r {

// NEOTKO_DEBUG_TAG_START
// NeoDebug — centralised debug channel implementation.
// One log file per channel, guarded by its env var or ORCA_DEBUG_ALL.
// Thread-safe writes via a single global mutex (debug only, no perf concern).
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
} // namespace NeoDebug
// NEOTKO_DEBUG_TAG_END

// NEOTKO_COLORMIX_TAG_START

// ---------------------------------------------------------------------------
// optimize_tool_block_travel
// Nearest-neighbor sort within a single tool's path block.
// Considers both endpoints of each path so the polyline can be flipped to
// reduce travel. O(n²) — acceptable for typical surface line counts (~5-200).
// ---------------------------------------------------------------------------
static void optimize_tool_block_travel(std::vector<ExtrusionPath*>& paths)
{
    const size_t n = paths.size();
    if (n <= 1) return;

    std::vector<bool> used(n, false);
    std::vector<ExtrusionPath*> sorted;
    sorted.reserve(n);

    // Anchor to the first path in generation order (no prior knowledge of head pos).
    sorted.push_back(paths[0]);
    used[0] = true;
    Point cur = paths[0]->polyline.last_point();

    while (sorted.size() < n) {
        int    best_i    = -1;
        bool   best_flip = false;
        double best_d2   = std::numeric_limits<double>::max();

        for (size_t i = 0; i < n; ++i) {
            if (used[i]) continue;
            auto& pl = paths[i]->polyline;
            {
                double dx = (double)cur.x() - (double)pl.first_point().x();
                double dy = (double)cur.y() - (double)pl.first_point().y();
                double d2 = dx*dx + dy*dy;
                if (d2 < best_d2) { best_d2 = d2; best_i = (int)i; best_flip = false; }
            }
            {
                double dx = (double)cur.x() - (double)pl.last_point().x();
                double dy = (double)cur.y() - (double)pl.last_point().y();
                double d2 = dx*dx + dy*dy;
                if (d2 < best_d2) { best_d2 = d2; best_i = (int)i; best_flip = true; }
            }
        }
        if (best_i < 0) break;

        used[best_i] = true;
        if (best_flip)
            paths[best_i]->polyline.reverse();
        cur = paths[best_i]->polyline.last_point();
        sorted.push_back(paths[best_i]);
    }

    paths = std::move(sorted);
}

// ---------------------------------------------------------------------------
// split_path_into_lines
// Splits a flat zig-zag ExtrusionPath into individual lines by detecting
// direction changes (dot product < 0.94 ≈ 20° threshold).
// ---------------------------------------------------------------------------
static std::vector<Polyline> split_path_into_lines(const ExtrusionPath& path)
{
    const Points& pts = path.polyline.points;
    std::vector<Polyline> lines;
    if (pts.size() < 2) return lines;

    Polyline current;
    current.points.push_back(pts[0]);
    double prev_dx = 0, prev_dy = 0;
    bool has_prev = false;

    for (size_t i = 1; i < pts.size(); ++i) {
        double dx = static_cast<double>(pts[i].x() - pts[i-1].x());
        double dy = static_cast<double>(pts[i].y() - pts[i-1].y());
        double len = std::sqrt(dx*dx + dy*dy);
        if (len < 1.0) { current.points.push_back(pts[i]); continue; }
        double nx = dx/len, ny = dy/len;

        if (has_prev && (prev_dx*nx + prev_dy*ny) < 0.94) {
            current.points.push_back(pts[i]);
            if (current.points.size() >= 2) lines.push_back(std::move(current));
            current = Polyline();
            current.points.push_back(pts[i]);
            has_prev = false;
            continue;
        }
        current.points.push_back(pts[i]);
        prev_dx = nx; prev_dy = ny;
        has_prev = true;
    }
    if (current.points.size() >= 2) lines.push_back(std::move(current));
    return lines;
}

// ---------------------------------------------------------------------------
// Build active tool list from the 4 explicit slots (A, B, C, D).
// C and D are optional: -1 means disabled.
// ---------------------------------------------------------------------------
static std::vector<int> build_tool_list(const PrintRegionConfig& config)
{
    std::vector<int> tools;
    tools.push_back(config.interlayer_colormix_tool_a.value);
    tools.push_back(config.interlayer_colormix_tool_b.value);
    if (config.interlayer_colormix_tool_c.value >= 0)
        tools.push_back(config.interlayer_colormix_tool_c.value);
    if (config.interlayer_colormix_tool_d.value >= 0)
        tools.push_back(config.interlayer_colormix_tool_d.value);
    return tools;
}

// ---------------------------------------------------------------------------
// GCD helper for building minimum-length weighted sequences.
static int recipe_gcd(int a, int b) { return b == 0 ? a : recipe_gcd(b, a % b); }

// ---------------------------------------------------------------------------
// Bresenham-style dithering: distributes tool indices as uniformly as possible
// across a sequence of 'n' slots given integer weights for each tool.
//
// Each step: accumulate all weights into per-tool error counters, pick the tool
// with the highest counter, emit it, subtract total from that counter.
// This is identical to the greedy dithering algorithm in MixedFilamentManager
// for building gradient layer sequences — we apply it within a surface layer.
//
// Example: tool_weights = [(T0,37),(T3,11)], n=16
//   → T0,T0,T0,T0,T3,T0,T0,T0,T3,T0,T0,T0,T0,T3,T0,T0
//   Instead of the naive [T0×12,T3×4] which creates visible bands.
// ---------------------------------------------------------------------------
static std::vector<int> dither_sequence(
    const std::vector<std::pair<int,int>>& tool_weights, // (0-based idx, weight)
    int n)
{
    std::vector<int> seq;
    if (tool_weights.empty() || n <= 0) return seq;
    int total = 0;
    for (auto& tw : tool_weights) total += std::max(0, tw.second);
    if (total <= 0) return seq;
    seq.reserve(n);
    std::vector<int> err(tool_weights.size(), 0);
    for (int step = 0; step < n; ++step) {
        for (size_t i = 0; i < tool_weights.size(); ++i)
            err[i] += tool_weights[i].second;
        size_t best = 0;
        for (size_t i = 1; i < tool_weights.size(); ++i)
            if (err[i] > err[best]) best = i;
        seq.push_back(tool_weights[best].first);
        err[best] -= total;
    }
    return seq;
}

// ---------------------------------------------------------------------------
// Extract the FULL ordered physical tool sequence from a MixedFilament recipe.
// Returns 0-based tool indices with repetitions/distribution preserved so that
// ColorMix cycles through this list to reproduce the correct blend ratio.
//
// Priority order (mirrors MixedFilamentManager::resolve priority):
//   1. manual_pattern  — token sequence decoded verbatim; repetitions preserved.
//                        '1'→component_a, '2'→component_b, '3'-'9'→direct 1-based ID.
//                        User controls the exact interleave — no dithering applied.
//   2. gradient_component_ids + gradient_component_weights — Bresenham-dithered
//      sequence; "50/25/25" reduced by GCD ([2,1,1]) then dithered to length ≤ cap.
//   3. Ratio fallback  — component_a/component_b Bresenham-dithered with
//      ratio_a/ratio_b as weights, GCD-reduced.
//
// Cases 2 and 3 use dithering instead of naive concatenation so that tools are
// uniformly spread across the surface (avoids T0-band then T3-band visual artifacts).
// Max sequence length capped at 48.
// ---------------------------------------------------------------------------
static std::vector<int> extract_recipe_tools(const MixedFilament& mf, size_t num_physical)
{
    static constexpr int kMaxSeqLen = 48;

    // Helper: validate and convert 1-based physical ID to 0-based, or -1 if invalid.
    auto to_idx = [&](unsigned int phys_1based) -> int {
        return (phys_1based >= 1 && phys_1based <= (unsigned)num_physical)
            ? static_cast<int>(phys_1based) - 1 : -1;
    };

    if (!mf.manual_pattern.empty()) {
        // Manual pattern — decode tokens verbatim (explicit interleave, no dithering).
        std::vector<int> seq;
        for (char c : mf.manual_pattern) {
            if (c == ',') continue;
            int idx = -1;
            if      (c == '1') idx = to_idx(mf.component_a);
            else if (c == '2') idx = to_idx(mf.component_b);
            else if (c >= '3' && c <= '9') idx = to_idx(static_cast<unsigned int>(c - '0'));
            if (idx >= 0 && (int)seq.size() < kMaxSeqLen)
                seq.push_back(idx);
        }
        return seq;
    }

    if (!mf.gradient_component_ids.empty()) {
        // Gradient — parse IDs and weights, reduce by GCD, then Bresenham-dither.
        std::vector<unsigned int> ids;
        for (char c : mf.gradient_component_ids)
            if (c >= '1' && c <= '9') ids.push_back(static_cast<unsigned int>(c - '0'));

        std::vector<int> wts;
        if (!mf.gradient_component_weights.empty()) {
            std::istringstream ss(mf.gradient_component_weights);
            std::string tok;
            while (std::getline(ss, tok, '/'))
                try { wts.push_back(std::stoi(tok)); } catch (...) {}
        }
        while ((int)wts.size() < (int)ids.size())
            wts.push_back(ids.empty() ? 1 : (100 / (int)ids.size()));

        if (!ids.empty()) {
            int g = wts[0];
            for (int w : wts) if (w > 0) g = recipe_gcd(g, w);
            if (g <= 0) g = 1;
            int reduced_total = 0;
            std::vector<std::pair<int,int>> tw;
            for (size_t i = 0; i < ids.size() && i < wts.size(); ++i) {
                int idx = to_idx(ids[i]);
                int w   = (wts[i] > 0) ? (wts[i] / g) : 1;
                if (idx >= 0) { tw.push_back({idx, w}); reduced_total += w; }
            }
            return dither_sequence(tw, std::min(reduced_total, kMaxSeqLen));
        }
        return {};
    }

    // Simple ratio — Bresenham-dither component_a/b using ratio_a/ratio_b as weights.
    int ra = std::max(1, mf.ratio_a);
    int rb = std::max(1, mf.ratio_b);
    int g  = recipe_gcd(ra, rb);
    ra /= g; rb /= g;
    int idx_a = to_idx(mf.component_a);
    int idx_b = to_idx(mf.component_b);
    if (idx_a < 0 || idx_b < 0) return {};
    return dither_sequence({{idx_a, ra}, {idx_b, rb}}, std::min(ra + rb, kMaxSeqLen));
}

// ---------------------------------------------------------------------------
// NEOTKO_COLORMIX_TAG — s58: lane distribution helpers (modes 1/2/3) moved to
// SurfaceColorMix.hpp so GCode.cpp can reuse them for PathBlend. See the header
// for the `compute_slot_per_line()` template and its companion helpers.

// Build tool list from a pattern string (e.g. "12", "1221", "123").
// Digits '1'-'4': user 1-based → internal 0-based physical tool index.
// Digits '5'-'9' (use_virtual ON + mgr provided): RECIPE EXPANSION.
//   The digit identifies a virtual MixedFilament by ID; its full recipe is
//   extracted via extract_recipe_tools() and all unique physical tools are
//   appended to the list.  Only ONE virtual digit should be used per pattern —
//   the virtual filament IS the pattern (its recipe defines the tool sequence).
//   No virtual index is ever stored in mm3_per_mm; GCode decode needs no
//   per-layer virtual resolution.
// If fewer than 2 distinct physical tools result → fallback to build_tool_list().
// ---------------------------------------------------------------------------
static std::vector<int> build_tool_list_from_pattern(
    const std::string& pattern,
    const PrintRegionConfig& config,
    const MixedFilamentManager* mgr    = nullptr,
    size_t                      num_physical = 0)
{
    std::vector<int> tools;
    const bool use_virtual = config.interlayer_colormix_use_virtual.value
                             && mgr != nullptr
                             && num_physical > 0;
    for (char c : pattern) {
        if (c >= '1' && c <= '4') {
            // NEOTKO_COLORMIX_TAG — s58: preserve repetitions for physical tools.
            // Previously this branch deduped via std::find, collapsing
            //   "1111111122222222" → [T0,T1] (cycle 2) → fine T0/T1 alternation.
            // Now repetitions widen bands ("1122" = 2-band; "112" ≠ "12"), matching
            // the existing behaviour for virtual MixedFilament digits below
            // ("Append without dedup — repetitions carry the weighting").
            // This is the foundation for upcoming numeric-gradient pattern support.
            // The fallback `unique_tools.size() < 2` check still uses std::set, so it
            // correctly counts uniques regardless of repetitions.
            tools.push_back(static_cast<int>(c - '1'));
        } else if (c >= '5' && c <= '9' && use_virtual) {
            // Virtual MixedFilament: append full recipe sequence (repetitions preserved).
            // The sequence encodes blend ratios: "112233" → [T0,T0,T1,T1,T2,T2] cycles
            // at 33%/33%/33%. ColorMix cycles through the list, so the recipe IS the pattern.
            unsigned int virtual_id = static_cast<unsigned int>(c - '0'); // 1-based
            const MixedFilament* mf = mgr->mixed_filament_from_id(virtual_id, num_physical);
            if (mf) {
                auto recipe = extract_recipe_tools(*mf, num_physical);
                if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
                    std::set<int> uniq(recipe.begin(), recipe.end());
                    std::ostringstream _s;
                    _s << "RECIPE_EXPAND digit='" << c << "' virtual_id=" << virtual_id
                       << " seq_len=" << recipe.size() << " unique=" << uniq.size() << " → [";
                    for (size_t i = 0; i < recipe.size(); ++i) _s << (i?",":"") << "T" << recipe[i];
                    _s << "]";
                    NeoDebug::write(NeoDebug::COLORMIX, _s.str());
                }
                // Append without dedup — repetitions carry the weighting.
                for (int t : recipe) tools.push_back(t);
            }
        }
    }
    // Fallback check: need at least 2 UNIQUE physical tools (repetitions don't count).
    {
        std::set<int> unique_tools(tools.begin(), tools.end());
        if (unique_tools.size() < 2) {
            NEOTKO_LOG(COLORMIX, "PATTERN_FALLBACK pattern=\"" << pattern
                << "\" (unique=" << unique_tools.size() << ") → using legacy tool_a/b/c/d");
            return build_tool_list(config);
        }
    }
    if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
        std::ostringstream _s;
        _s << "PATTERN_USE pattern=\"" << pattern << "\" seq_len=" << tools.size() << " → [";
        for (size_t i = 0; i < tools.size(); ++i) _s << (i?",":"") << "T" << tools[i];
        _s << "]";
        NeoDebug::write(NeoDebug::COLORMIX, _s.str());
    }
    return tools;
}

// ---------------------------------------------------------------------------
// Check if a given extrusion role should be processed given the surface filter.
// surface int: 0=Both, 1=Top only, 2=Penultimate only (kColormixSurface_* constants)
// ---------------------------------------------------------------------------
bool SurfaceColorMix::should_process_role(ExtrusionRole role, int surface)
{
    switch (surface) {
    case kColormixSurface_Top:
        return role == erTopSolidInfill;
    case kColormixSurface_Penultimate:
        return role == erPenultimateInfill;
    case kColormixSurface_Both:
    default:
        return role == erTopSolidInfill || role == erPenultimateInfill;
    }
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// NEOTKO_PROFILE_TAG — Fase D: painted-profile slot resolution.
//
// `dominant_painted_slot_in_z_range` counts, per slot 1..15, the upward-facing
// painted triangles whose max_z lies inside [z_min, z_max] (with a tiny fp
// tolerance). Returns the slot with the highest count, or 0 if none.
//
// Caller passes the layer's *vertical extent* in print-frame coords, NOT a
// (target, tol) pair — this is critical because the mesh's natural top of a
// step typically sits BELOW the layer's print_z (which marks the layer's top
// boundary). e.g. a step ending at mesh z=2.0 belongs to the layer at
// print_z=2.05; querying around 2.05 with a small tolerance misses 2.0.
// By querying the whole layer span [layer_print_z - layer_height, layer_print_z]
// we catch the mesh-top wherever it falls within that slab.
//
// Frames: mesh vertices live in volume-local space; we compose
//   print_frame = po->trafo() * mv->get_matrix() * mesh_vertex
// before comparing Z.
//
// NOTE on `used_states`: we deliberately do NOT gate the scan on
// `data.used_states[slot]` — `set_triangle_from_string` (the 3mf load path)
// does not populate that array, so a freshly loaded .3mf would always read
// "all states unused" and the override would never fire. Iterating all 15
// slots via get_facets() is robust to both fresh-paint and load-from-3mf.
// ---------------------------------------------------------------------------
int SurfaceColorMix::dominant_painted_slot_in_z_range(const PrintObject* po,
                                                       double z_min, double z_max)
{
    if (!po) return 0;
    const ModelObject* mo = po->model_object();
    if (!mo) return 0;

    const Transform3d trafo = po->trafo();
    const double z_tol = 0.02; // fp slack around the layer extent

    int counts[16] = {0};
    bool any_painted = false;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        const Transform3d vt = trafo * mv->get_matrix();
        for (int slot = 1; slot < 16; ++slot) {
            const indexed_triangle_set its = mv->color_mix_paint_facets.get_facets(
                *mv, static_cast<EnforcerBlockerType>(slot));
            if (its.indices.empty()) continue;
            for (const auto& tri : its.indices) {
                const Vec3f& v0f = its.vertices[tri[0]];
                const Vec3f& v1f = its.vertices[tri[1]];
                const Vec3f& v2f = its.vertices[tri[2]];
                const Vec3d v0 = vt * Vec3d(v0f.x(), v0f.y(), v0f.z());
                const Vec3d v1 = vt * Vec3d(v1f.x(), v1f.y(), v1f.z());
                const Vec3d v2 = vt * Vec3d(v2f.x(), v2f.y(), v2f.z());
                const Vec3d e1 = v1 - v0, e2 = v2 - v0;
                const Vec3d n  = e1.cross(e2);
                if (n.z() <= 0.0) continue; // upward-facing only
                const double max_z = std::max({v0.z(), v1.z(), v2.z()});
                if (max_z >= z_min - z_tol && max_z <= z_max + z_tol) {
                    counts[slot]++;
                    any_painted = true;
                }
            }
        }
    }

    if (!any_painted) return 0;
    int best_slot = 0, best_count = 0;
    for (int s = 1; s < 16; ++s)
        if (counts[s] > best_count) { best_count = counts[s]; best_slot = s; }
    return best_slot;
}

// Resolve the SurfaceEffectProfile id for a painted slot on the print's model.
// Slot tables live per-ModelVolume; we pick the first model_part with a
// non-zero entry at that slot (the gizmo keeps them consistent across volumes
// of the same object).
int SurfaceColorMix::profile_id_for_slot(const PrintObject* po, int slot)
{
    if (!po || slot <= 0 || slot >= 16) return 0;
    const ModelObject* mo = po->model_object();
    if (!mo) return 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        if (mv->colormix_slot_to_profile_id[slot] != 0)
            return mv->colormix_slot_to_profile_id[slot];
    }
    return 0;
}

// NEOTKO_PROFILE_TAG — true if any model_part volume has at least one slot
// (1..15) mapped to a profile id. Flips the slicer into painter mode.
bool SurfaceColorMix::object_has_any_colormix_paint(const ModelObject* mo)
{
    if (!mo) return false;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        for (int s = 1; s < 16; ++s)
            if (mv->colormix_slot_to_profile_id[s] != 0)
                return true;
    }
    return false;
}

// NEOTKO_PROFILE_TAG — Penu role autonomy detection (s66 polish).
bool SurfaceColorMix::object_painter_wants_penu(const ModelObject* mo)
{
    if (!mo) return false;
    auto& mgr = Slic3r::SurfaceEffectProfileManager::get();
    std::set<int> profile_ids_seen;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        for (int s = 1; s < 16; ++s) {
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid <= 0) continue;
            if (!profile_ids_seen.insert(pid).second) continue;
            const SurfaceEffectProfile* p = mgr.find(pid);
            if (!p) continue;
            // MultiPass — penu enabled?
            if (p->multipass.present) {
                auto it = p->multipass.kv.find("penultimate_multipass_enabled");
                if (it != p->multipass.kv.end() &&
                    (it->second == "1" || it->second == "true"))
                    return true;
            }
            // ColorMix — surface 0=Both / 2=Penu only → wants penu.
            if (p->colormix.present) {
                auto it = p->colormix.kv.find("interlayer_colormix_surface");
                if (it != p->colormix.kv.end()) {
                    const int v = std::atoi(it->second.c_str());
                    if (v == 0 || v == 2) return true;
                }
            }
            // PathBlend — same surface enum semantics.
            if (p->pathblend.present) {
                auto it = p->pathblend.kv.find("pathblend_surface");
                if (it != p->pathblend.kv.end()) {
                    const int v = std::atoi(it->second.c_str());
                    if (v == 0 || v == 2) return true;
                }
            }
        }
    }
    return false;
}

// NEOTKO_PROFILE_TAG — derive the 1-based tool list for a given painted
// profile + role. This MUST stay in sync with the gv-build path in
// `assign_and_group_tools` so that ToolOrdering registers exactly the tools
// the SLICE will end up assigning (otherwise the wipe-tower plan diverges
// from runtime and we get the "unexpected toolchange" mismatch error).
//
// Source of truth — payload keys per role:
//   * mode / tool_a..d / band_count_a..d: prefixed (`` or `_penu_`)
//   * pattern strings: NOT prefixed — `pattern_top` / `pattern_penultimate`
//
// Behavior mirrors SurfaceColorMix.cpp gv consumption + ToolOrdering.cpp
// `tools_for_role` + `parse_colormix_pattern_1based`.
std::vector<unsigned int> SurfaceColorMix::painted_profile_tools_1based(
    const SurfaceEffectProfile& p, bool top_role)
{
    std::vector<unsigned int> out;
    if (!p.colormix.present) return out;

    const auto& kv = p.colormix.kv;
    const std::string prefix = top_role
        ? std::string("interlayer_colormix_")
        : std::string("interlayer_colormix_penu_");

    auto kv_get = [&](const std::string& full_key) -> std::string {
        auto it = kv.find(full_key);
        return it == kv.end() ? std::string() : it->second;
    };
    auto get_int = [&](const char* base, int dflt) -> int {
        const std::string s = kv_get(prefix + base);
        if (s.empty()) return dflt;
        try { return std::stoi(s); } catch (...) { return dflt; }
    };

    const int mode    = get_int("mode",            0);
    const int tool_a  = get_int("tool_a",          0);
    const int tool_b  = get_int("tool_b",          1);
    const int tool_c  = get_int("tool_c",         -1);
    const int tool_d  = get_int("tool_d",         -1);
    const int band_a  = get_int("band_count_a",    0);
    const int band_b  = get_int("band_count_b",    0);
    const int band_c  = get_int("band_count_c",    0);
    const int band_d  = get_int("band_count_d",    0);

    auto add_phys = [&](int t) {
        if (t < 0) return;
        unsigned int u = static_cast<unsigned int>(t + 1);
        if (std::find(out.begin(), out.end(), u) == out.end())
            out.push_back(u);
    };

    if (mode == 1) {                     // Linear 2-color
        if (tool_a >= 0 && tool_b >= 0 && tool_a != tool_b) {
            add_phys(tool_a); add_phys(tool_b);
            return out;
        }
        // else fall through to legacy
    } else if (mode == 2) {              // Linear 3-color
        if (tool_a >= 0 && tool_b >= 0 && tool_c >= 0
            && (tool_a != tool_b || tool_b != tool_c)) {
            add_phys(tool_a); add_phys(tool_b); add_phys(tool_c);
            return out;
        }
    } else if (mode == 3) {              // Custom bands
        if (band_a > 0 && tool_a >= 0) add_phys(tool_a);
        if (band_b > 0 && tool_b >= 0) add_phys(tool_b);
        if (band_c > 0 && tool_c >= 0) add_phys(tool_c);
        if (band_d > 0 && tool_d >= 0) add_phys(tool_d);
        if (out.size() >= 2) return out;
        out.clear();
        // fall through to legacy
    }

    // mode == 0 (or fallback): parse the pattern string for this role, then
    // fall back to tool_a/b/c/d if fewer than 2 distinct physical tools.
    const std::string pattern = kv_get(top_role
        ? std::string("interlayer_colormix_pattern_top")
        : std::string("interlayer_colormix_pattern_penultimate"));
    for (char c : pattern)
        if (c >= '1' && c <= '4')
            add_phys(static_cast<int>(c - '1'));
    if (out.size() >= 2) return out;

    // Legacy A/B/C/D fallback.
    out.clear();
    add_phys(tool_a);
    add_phys(tool_b);
    if (tool_c >= 0) add_phys(tool_c);
    if (tool_d >= 0) add_phys(tool_d);
    return out;
}

int SurfaceColorMix::assign_and_group_tools(
    ExtrusionEntityCollection& fills,
    const PrintRegionConfig& config,
    ExtrusionRole /*role — detected internally per path*/,
    int layer_idx,
    bool allow_top,
    bool allow_penu,
    const MixedFilamentManager* mgr,
    size_t num_physical,
    const PrintObject* print_object,
    double layer_print_z,
    double layer_height
) {
    NEOTKO_LOG(COLORMIX, "ENTRY layer=" << layer_idx
        << " enabled=" << config.interlayer_colormix_enabled.value
        << " fills_size=" << fills.entities.size());

    // NEOTKO_PROFILE_TAG — Fase D (final design): painter-mode takeover.
    //
    // If the object has ANY painted slot (any volume's colormix_slot_to_profile_id
    // is non-zero), we enter PAINTER MODE for this object:
    //   * preset's interlayer_colormix_enabled/surface/zone/filament_filter
    //     are IGNORED — the painter decides everything
    //   * a layer/role only gets colormix if a painted profile resolves there
    //   * the profile's own values (mode, pct, easing, tools, pattern, …) drive
    //     the effect for that fill
    //
    // Otherwise (no paint anywhere on the object) we run PRESET MODE, which is
    // the original pipeline — the preset controls everything as before.
    //
    // ToolOrdering mirrors the same painter_mode_obj logic so its tool list
    // registration matches the SLICE output. They MUST stay in sync; any
    // divergence triggers a wipe-tower "unexpected toolchange" crash.
    const ModelObject* model_object =
        print_object ? print_object->model_object() : nullptr;

    // NEOTKO_PROFILE_TAG — MMU exclusion. A multi-material-painted object is
    // controlled entirely by MMU segmentation; re-encoding its top/penu fills
    // here (preset OR painter mode) clobbers the per-region extruder assignment
    // MMU produced. MMU owns the surface — ColorMix steps aside completely.
    if (model_object && model_object->is_mm_painted()) {
        NEOTKO_LOG(COLORMIX, "MMU_SKIP ColorMix layer=" << layer_idx
            << " — object is MMU-painted, ColorMix suppressed (MMU owns surfaces)");
        return 0;
    }

    const bool painter_mode_obj = object_has_any_colormix_paint(model_object);
    const SurfaceEffectProfile* painted_top_profile  = nullptr;
    const SurfaceEffectProfile* painted_penu_profile = nullptr;
    if (print_object && layer_height > 0.0) {
        // NEOTKO_PROFILE_TAG — Fase D: query the layer's vertical EXTENT, not
        // a single Z plane. The slicer treats the layer whose print_z is the
        // first ≥ mesh_top as the "top surface" layer — so for a step ending
        // at mesh z=2.0 inside layer L with print_z=2.05 and height=0.2, the
        // painted triangle's max_z=2.0 lives inside L's slab [1.85, 2.05].
        //   top role  at L: paint inside this layer's slab
        //                   → [print_z - height, print_z]
        //   penu role at L: paint inside the slab ONE LAYER ABOVE
        //                   → [print_z, print_z + height]
        const double z_top_min  = layer_print_z - layer_height;
        const double z_top_max  = layer_print_z;
        const double z_penu_min = layer_print_z;
        const double z_penu_max = layer_print_z + layer_height;
        const int top_slot  = dominant_painted_slot_in_z_range(print_object, z_top_min,  z_top_max);
        const int penu_slot = dominant_painted_slot_in_z_range(print_object, z_penu_min, z_penu_max);
        if (const int pid = profile_id_for_slot(print_object, top_slot); pid)
            painted_top_profile = SurfaceEffectProfileManager::get().find(pid);
        if (const int pid = profile_id_for_slot(print_object, penu_slot); pid)
            painted_penu_profile = SurfaceEffectProfileManager::get().find(pid);
        if (top_slot || penu_slot)
            NEOTKO_LOG(PROFILE, "SLICE layer=" << layer_idx
                << " z=" << layer_print_z << " h=" << layer_height
                << " painter_mode=" << (painter_mode_obj ? "1" : "0")
                << " top_range=[" << z_top_min << "," << z_top_max << "]"
                << " top_slot=" << top_slot << " penu_slot=" << penu_slot
                << " top_profile=" << (painted_top_profile ? painted_top_profile->name : "<none>")
                << " penu_profile=" << (painted_penu_profile ? painted_penu_profile->name : "<none>"));
    }

    // NEOTKO_PROFILE_TAG — Fase D: painter-mode bypasses preset enable gate.
    // In painter mode we still proceed even with enabled=false; the per-sub
    // loop below filters out layers/roles that don't have a painted profile.
    // In preset mode the original gate is preserved.
    if (!painter_mode_obj && !config.interlayer_colormix_enabled.value)
        return 0;

    const double min_length_mm = config.interlayer_colormix_min_length.value;
    const int surface = config.interlayer_colormix_surface.value;

    bool any_modified    = false;
    bool any_unsplittable = false;

    for (auto* top_entity : fills.entities) {
        ExtrusionEntityCollection* sub = dynamic_cast<ExtrusionEntityCollection*>(top_entity);
        if (!sub || sub->entities.empty()) continue;

        // Skip if already colormix-encoded (mm3_per_mm >= 10.0)
        {
            bool already_encoded = false;
            for (const auto* e : sub->entities) {
                if (const auto* p = dynamic_cast<const ExtrusionPath*>(e))
                    if (p->mm3_per_mm >= 10.0) { already_encoded = true; break; }
            }
            if (already_encoded) continue;
        }

        // Find first ExtrusionPath in sub — used for role/attrs and single-path detection.
        const ExtrusionPath* first_path = nullptr;
        int n_paths = 0;
        for (auto* e : sub->entities) {
            if (auto* p = dynamic_cast<const ExtrusionPath*>(e)) {
                if (!first_path) first_path = p;
                ++n_paths;
            }
        }
        if (!first_path) continue;

        // NEOTKO_PROFILE_TAG — Fase D (final): split painter / preset modes.
        const bool gv_is_top_role = (first_path->role() == erTopSolidInfill);
        const SurfaceEffectProfile* eff_profile =
            gv_is_top_role ? painted_top_profile : painted_penu_profile;
        const bool painted_override =
            eff_profile && eff_profile->colormix.present;

        if (painter_mode_obj) {
            // Painter mode: only process top/penu roles AND only if this layer
            // has a painted profile for the role. The painter decides where —
            // preset's surface/zone/filter gates do not apply.
            const ExtrusionRole r = first_path->role();
            if (r != erTopSolidInfill && r != erPenultimateInfill) continue;
            if (!painted_override) continue;
        } else {
            // Preset mode (original gates).
            if (!should_process_role(first_path->role(), surface)) continue;
            if (!allow_top  && first_path->role() == erTopSolidInfill)    continue;
            if (!allow_penu && first_path->role() == erPenultimateInfill)  continue;
        }

        // NEOTKO_COLORMIX_TAG — s61: per-role gradient view.
        // Pick the top-role or penultimate-role config keys based on the actual
        // ExtrusionRole of this surface. The dialog edits each set
        // independently; out of the box the defaults match so old presets
        // behave the same on both roles.
        // (gv_is_top_role already declared above, before the role-filter gates.)
        struct GV {
            int    cm_mode, pct_a, pct_b, easing, min_lines;
            double gamma, overlap;
            bool   invert;
            int    band_a, band_b, band_c, band_d;
            int    tool_a, tool_b, tool_c, tool_d;
        };
        GV gv;
        if (gv_is_top_role) {
            gv.cm_mode   = config.interlayer_colormix_mode.value;
            gv.pct_a     = config.interlayer_colormix_pct_a.value;
            gv.pct_b     = config.interlayer_colormix_pct_b.value;
            gv.easing    = config.interlayer_colormix_easing.value;
            gv.gamma     = config.interlayer_colormix_gamma.value;
            gv.min_lines = config.interlayer_colormix_min_surface_lines.value;
            gv.overlap   = config.interlayer_colormix_overlap.value;
            gv.invert    = config.interlayer_colormix_invert.value;
            gv.band_a    = config.interlayer_colormix_band_count_a.value;
            gv.band_b    = config.interlayer_colormix_band_count_b.value;
            gv.band_c    = config.interlayer_colormix_band_count_c.value;
            gv.band_d    = config.interlayer_colormix_band_count_d.value;
            gv.tool_a    = config.interlayer_colormix_tool_a.value;
            gv.tool_b    = config.interlayer_colormix_tool_b.value;
            gv.tool_c    = config.interlayer_colormix_tool_c.value;
            gv.tool_d    = config.interlayer_colormix_tool_d.value;
        } else {
            gv.cm_mode   = config.interlayer_colormix_penu_mode.value;
            gv.pct_a     = config.interlayer_colormix_penu_pct_a.value;
            gv.pct_b     = config.interlayer_colormix_penu_pct_b.value;
            gv.easing    = config.interlayer_colormix_penu_easing.value;
            gv.gamma     = config.interlayer_colormix_penu_gamma.value;
            gv.min_lines = config.interlayer_colormix_penu_min_surface_lines.value;
            gv.overlap   = config.interlayer_colormix_penu_overlap.value;
            gv.invert    = config.interlayer_colormix_penu_invert.value;
            gv.band_a    = config.interlayer_colormix_penu_band_count_a.value;
            gv.band_b    = config.interlayer_colormix_penu_band_count_b.value;
            gv.band_c    = config.interlayer_colormix_penu_band_count_c.value;
            gv.band_d    = config.interlayer_colormix_penu_band_count_d.value;
            gv.tool_a    = config.interlayer_colormix_penu_tool_a.value;
            gv.tool_b    = config.interlayer_colormix_penu_tool_b.value;
            gv.tool_c    = config.interlayer_colormix_penu_tool_c.value;
            gv.tool_d    = config.interlayer_colormix_penu_tool_d.value;
        }

        // NEOTKO_PROFILE_TAG — Fase D: painted-profile override.
        // The profile's ColorMix payload was snapshot-taken with the full key
        // set (top + _penu_). We pick the matching subset for this role and
        // overlay its values on top of the preset-loaded gv.
        if (painted_override) {
            const auto& kv = eff_profile->colormix.kv;
            const std::string prefix = gv_is_top_role
                ? std::string("interlayer_colormix_")
                : std::string("interlayer_colormix_penu_");
            auto get_int = [&](const char* base, int dflt) -> int {
                auto it = kv.find(prefix + base);
                if (it == kv.end()) return dflt;
                try { return std::stoi(it->second); } catch (...) { return dflt; }
            };
            auto get_dbl = [&](const char* base, double dflt) -> double {
                auto it = kv.find(prefix + base);
                if (it == kv.end()) return dflt;
                try { return std::stod(it->second); } catch (...) { return dflt; }
            };
            auto get_bool = [&](const char* base, bool dflt) -> bool {
                auto it = kv.find(prefix + base);
                if (it == kv.end()) return dflt;
                return (it->second == "1" || it->second == "true");
            };
            gv.cm_mode   = get_int ("mode",              gv.cm_mode);
            gv.pct_a     = get_int ("pct_a",             gv.pct_a);
            gv.pct_b     = get_int ("pct_b",             gv.pct_b);
            gv.easing    = get_int ("easing",            gv.easing);
            gv.gamma     = get_dbl ("gamma",             gv.gamma);
            gv.min_lines = get_int ("min_surface_lines", gv.min_lines);
            gv.overlap   = get_dbl ("overlap",           gv.overlap);
            gv.invert    = get_bool("invert",            gv.invert);
            gv.band_a    = get_int ("band_count_a",      gv.band_a);
            gv.band_b    = get_int ("band_count_b",      gv.band_b);
            gv.band_c    = get_int ("band_count_c",      gv.band_c);
            gv.band_d    = get_int ("band_count_d",      gv.band_d);
            gv.tool_a    = get_int ("tool_a",            gv.tool_a);
            gv.tool_b    = get_int ("tool_b",            gv.tool_b);
            gv.tool_c    = get_int ("tool_c",            gv.tool_c);
            gv.tool_d    = get_int ("tool_d",            gv.tool_d);
            NEOTKO_LOG(PROFILE, "OVERRIDE layer=" << layer_idx
                << " role=" << (gv_is_top_role ? "Top" : "Penu")
                << " profile='" << eff_profile->name << "' (id=" << eff_profile->id << ")"
                << " mode=" << gv.cm_mode << " pct_a=" << gv.pct_a
                << " pct_b=" << gv.pct_b << " easing=" << gv.easing
                << " tools=[" << gv.tool_a << "," << gv.tool_b
                << "," << gv.tool_c << "," << gv.tool_d << "]"
                << " bands=[" << gv.band_a << "," << gv.band_b
                << "," << gv.band_c << "," << gv.band_d << "]");
        }

        // Resolve tool list from role-specific pattern (fallback to legacy slots if invalid)
        // NEOTKO_COLORMIX_TAG — s60: mode-aware tool resolution.
        // mode=0 (legacy)     → parse the pattern string as before.
        // mode=1 (Linear2)    → preflight {tool_a, tool_b}; dither post-raw_lines.
        // mode=2 (Linear3)    → preflight {tool_a, tool_b, tool_c}; dither post-raw_lines.
        // mode=3 (CustomBands)→ preflight the non-zero-count tools; bands post-raw_lines.
        // For modes 1-3 we only need a "preflight" tools[] (at least 2 entries)
        // to pass the `tools.size() < 2` guard. The per-line decisions are
        // computed AFTER raw_lines is known.
        std::vector<int> tools;
        const int cm_mode = gv.cm_mode;
        auto fallback_to_pattern_string = [&]() {
            const std::string& pattern_str = gv_is_top_role
                ? config.interlayer_colormix_pattern_top.value
                : config.interlayer_colormix_pattern_penultimate.value;
            tools = build_tool_list_from_pattern(pattern_str, config, mgr, num_physical);
        };

        // NEOTKO_PROFILE_TAG — Fase D (final): in painter mode the painted
        // profile is the single source of truth for the tool list. We must
        // use the SAME helper that ToolOrdering uses (`painted_profile_tools_1based`)
        // so the registered tools match what SLICE actually consumes — otherwise
        // the legacy fallback path (`build_tool_list(config)`) reads the
        // preset's tool_a/b/c/d which can be different from the profile's
        // (e.g., preset has [0,1,2,3] but profile has [0,1,-1,-1] → 4 vs 2
        // tools → wipe-tower MISMATCH crash).
        if (painted_override) {
            const auto tools_1b = painted_profile_tools_1based(*eff_profile, gv_is_top_role);
            for (auto t : tools_1b)
                if (t > 0) tools.push_back(int(t) - 1); // back to 0-based for gv path
        } else if (cm_mode == 1) {
            if (gv.tool_a >= 0 && gv.tool_b >= 0 && gv.tool_a != gv.tool_b) {
                tools.push_back(gv.tool_a);
                tools.push_back(gv.tool_b);
            } else fallback_to_pattern_string();
        } else if (cm_mode == 2) {
            // For Linear3 we need at least 2 distinct tools; tool_c may equal a/b
            // (effectively collapses to a 2-stop ramp at one end).
            if (gv.tool_a >= 0 && gv.tool_b >= 0 && gv.tool_c >= 0
                && (gv.tool_a != gv.tool_b || gv.tool_b != gv.tool_c)) {
                tools.push_back(gv.tool_a);
                tools.push_back(gv.tool_b);
                tools.push_back(gv.tool_c);
            } else fallback_to_pattern_string();
        } else if (cm_mode == 3) {
            // NEOTKO_COLORMIX_TAG — s60: tool_c/_d default to -1 ("off") in the
            // legacy config. Treat a negative tool index as "skip this slot"
            // even if a band count is configured for it — otherwise the slot
            // silently encoded T0 and the user saw "C/D didn't apply".
            if (gv.band_a > 0 && gv.tool_a >= 0) tools.push_back(gv.tool_a);
            if (gv.band_b > 0 && gv.tool_b >= 0) tools.push_back(gv.tool_b);
            if (gv.band_c > 0 && gv.tool_c >= 0) tools.push_back(gv.tool_c);
            if (gv.band_d > 0 && gv.tool_d >= 0) tools.push_back(gv.tool_d);
            if (tools.size() < 2) fallback_to_pattern_string();
        } else {
            fallback_to_pattern_string();
        }
        if (tools.size() < 2) continue;

        // Collect lines:
        //   n_paths > 1 → Monotonic/MonotonicLine: sub already has one ExtrusionPath per line.
        //   n_paths == 1 → Rectilinear: single zig-zag path, split at direction changes.
        // Each entry: (polyline, proto attributes source)
        struct RawLine { Polyline pl; ExtrusionRole role; double mm3; float width; float height; };
        std::vector<RawLine> raw_lines;

        if (n_paths > 1) {
            // Monotonic case: iterate all paths directly
            NEOTKO_LOG(COLORMIX, "MONOTONIC_MODE layer=" << layer_idx
                << " n_paths=" << n_paths);
            for (auto* e : sub->entities) {
                auto* p = dynamic_cast<ExtrusionPath*>(e);
                if (!p) continue;
                double len_mm = static_cast<double>(p->polyline.length()) / 1e6;
                if (len_mm < min_length_mm) continue;
                raw_lines.push_back({ p->polyline, p->role(), p->mm3_per_mm, p->width, p->height });
            }
        } else {
            // Rectilinear case: split the single zig-zag
            std::vector<Polyline> split = split_path_into_lines(*first_path);
            if (split.size() < 2) {
                any_unsplittable = true;
                continue;
            }
            for (auto& pl : split) {
                double len_mm = static_cast<double>(pl.length()) / 1e6;
                if (len_mm < min_length_mm) continue;
                raw_lines.push_back({ std::move(pl), first_path->role(),
                    first_path->mm3_per_mm, first_path->width, first_path->height });
            }
        }

        NEOTKO_LOG(COLORMIX, "  RAW_LINES layer=" << layer_idx
            << " collected=" << raw_lines.size() << "/" << n_paths
            << " min_len=" << min_length_mm << "mm");
        if (raw_lines.size() < 2) {
            any_unsplittable = true;
            continue;
        }

        // NEOTKO_COLORMIX_TAG — s60: build per-line tool sequence for modes 1-3.
        // We do this AFTER raw_lines is known so the sequence length matches
        // the actual line count of THIS surface. Multi-surface objects get
        // proportional gradients per island automatically — small surfaces
        // with few lines get a coarser but still balanced sequence.
        //
        // min_surface_lines guard: tiny surfaces fall back to single-tool (A)
        // rather than show a degenerate 1-of-3 split. Set to 0 to disable.
        //
        // Replacing `tools` with a length == raw_lines.size() vector means
        // slot_per_line[i] = i below — each line has its own decision. The
        // lane_mode (GeoSort/LaneQuant/DirCluster) still maps geometric line
        // index → position in the sequence → the gradient axis follows the
        // chosen geometry, not the emission order.
        if (cm_mode >= 1 && cm_mode <= 3) {
            // NEOTKO_COLORMIX_TAG — s61 BUG FIX: route ALL dither parameters
            // through the per-role `gv` view. Previously these were read with
            // `config.interlayer_colormix_*` directly which always returned
            // the TOP role values regardless of the actual surface role →
            // Penultimate surfaces silently used Top's pct, easing, gamma,
            // overlap, band counts. With this fix Top and Penu are truly
            // independent end-to-end.
            const int min_lines = gv.min_lines;
            const int n         = static_cast<int>(raw_lines.size());
            const int easing    = gv.easing;
            const double gamma  = gv.gamma;
            if (min_lines > 0 && n < min_lines) {
                // Fall back to single tool (Tool A) for tiny surfaces.
                tools.assign(static_cast<size_t>(n), gv.tool_a);
                NEOTKO_LOG(COLORMIX, "DITHER_MIN_LINES_FALLBACK layer=" << layer_idx
                    << " role=" << (gv_is_top_role ? "Top" : "Penu")
                    << " n=" << n << " < min=" << min_lines << " → all T" << gv.tool_a);
            } else if (cm_mode == 1 && tools.size() == 2) {
                const int t_a   = tools[0];
                const int t_b   = tools[1];
                const int pct_a = gv.pct_a;
                tools = build_dithered_tools_2color(n, t_a, t_b, pct_a, easing, gamma);
                if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
                    int count_a = 0, count_b = 0;
                    for (int t : tools) (t == t_a ? count_a : count_b)++;
                    NEOTKO_LOG(COLORMIX, "DITHER_2COLOR layer=" << layer_idx
                        << " role=" << (gv_is_top_role ? "Top" : "Penu")
                        << " n=" << tools.size()
                        << " T" << t_a << "=" << count_a
                        << " T" << t_b << "=" << count_b
                        << " pct_a=" << pct_a << "% easing=" << easing);
                }
            } else if (cm_mode == 2 && tools.size() == 3) {
                const int t_a   = tools[0];
                const int t_b   = tools[1];
                const int t_c   = tools[2];
                const int pct_a = gv.pct_a;
                const int pct_b = gv.pct_b;
                const double overlap = gv.overlap;
                tools = build_dithered_tools_3color(n, t_a, t_b, t_c, pct_a, pct_b,
                                                    easing, gamma, overlap);
                if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
                    int ca = 0, cb = 0, cc = 0;
                    for (int t : tools) { if (t == t_a) ca++; else if (t == t_b) cb++; else if (t == t_c) cc++; }
                    NEOTKO_LOG(COLORMIX, "DITHER_3COLOR layer=" << layer_idx
                        << " role=" << (gv_is_top_role ? "Top" : "Penu")
                        << " n=" << tools.size()
                        << " T" << t_a << "=" << ca
                        << " T" << t_b << "=" << cb
                        << " T" << t_c << "=" << cc
                        << " pct_a=" << pct_a << "% pct_b=" << pct_b
                        << "% easing=" << easing << " overlap=" << overlap);
                }
            } else if (cm_mode == 3) {
                // Custom bands: ignore easing (hard blocks by definition).
                tools = build_custom_bands(n,
                    gv.tool_a, gv.band_a,
                    gv.tool_b, gv.band_b,
                    gv.tool_c, gv.band_c,
                    gv.tool_d, gv.band_d);
                if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
                    NEOTKO_LOG(COLORMIX, "CUSTOM_BANDS layer=" << layer_idx
                        << " role=" << (gv_is_top_role ? "Top" : "Penu")
                        << " n=" << tools.size()
                        << " cycle=[T" << gv.tool_a << "x" << gv.band_a
                        << ", T" << gv.tool_b << "x" << gv.band_b
                        << ", T" << gv.tool_c << "x" << gv.band_c
                        << ", T" << gv.tool_d << "x" << gv.band_d << "]");
                }
            }

            // NEOTKO_COLORMIX_TAG — s60: invert applies AFTER dither/band gen.
            // We flip the order of the entire per-line sequence so that the
            // gradient runs in the opposite direction without the user having
            // to swap tool slots or pct values manually. Equivalent visual
            // result, single-checkbox UX.
            // NEOTKO_COLORMIX_TAG — s61 BUG FIX: was reading the TOP-only key
            // (`interlayer_colormix_invert`) regardless of role, so a
            // Penultimate surface with invert toggled in the Penu dialog was
            // ignored (and vice versa: a Top invert leaked into Penu). Now
            // uses `gv.invert` which was loaded from the role-correct key in
            // the per-role view struct at the top of the loop.
            if (gv.invert && tools.size() > 1) {
                std::reverse(tools.begin(), tools.end());
                if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
                    NEOTKO_LOG(COLORMIX, "INVERT_GRADIENT layer=" << layer_idx
                        << " role=" << (gv_is_top_role ? "Top" : "Penu")
                        << " n=" << tools.size() << " (reversed sequence)");
                }
            }
        }

        if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
            std::ostringstream _s;
            _s << "SPLIT layer=" << layer_idx
               << " mode=" << (n_paths > 1 ? "monotonic" : "rectilinear")
               << " role=" << ExtrusionEntity::role_to_string(first_path->role())
               << " total_lines=" << raw_lines.size()
               << " tools=[";
            for (size_t i = 0; i < tools.size(); ++i) _s << (i?",":"") << "T" << tools[i];
            _s << "] surface=" << int(surface);
            NeoDebug::write(NeoDebug::COLORMIX, _s.str());
        }

        // NEOTKO_COLORMIX_TAG_START - unique-tool block merge
        // Distribute lines according to surface_color_mix_lane_mode (s58):
        //   0 = Default     — slot = path_idx % n_slots         (legacy)
        //   1 = GeoSort     — sort by ⊥ projection              (Opt A)
        //   2 = LaneQuant   — quantized lane per midpoint       (Opt B)
        //   3 = DirCluster  — cluster by direction + LaneQuant  (Opt C)
        // Then group by UNIQUE tool_id; unique_tool_order = first-occurrence order
        // so the first tool in the pattern prints first.
        const int n_slots = static_cast<int>(tools.size());
        const int lane_mode = config.surface_color_mix_lane_mode.value;
        std::string lane_summary;
        std::vector<int> slot_per_line =
            compute_slot_per_line(raw_lines, n_slots, lane_mode,
                                  NeoDebug::enabled(NeoDebug::COLORMIX) ? &lane_summary : nullptr);

        if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
            NEOTKO_LOG(COLORMIX, "LANE_MODE mode=" << lane_mode
                << " (" << lane_summary << ") layer=" << layer_idx
                << " lines=" << raw_lines.size() << " slots=" << n_slots);
        }

        std::vector<int> unique_tool_order;
        std::map<int, std::vector<ExtrusionPath*>> tool_blocks;

        for (int path_idx = 0; path_idx < (int)raw_lines.size(); ++path_idx) {
            int slot     = slot_per_line[path_idx];
            int tool_idx = tools[slot];

            if (tool_blocks.find(tool_idx) == tool_blocks.end())
                unique_tool_order.push_back(tool_idx);

            auto& rl = raw_lines[path_idx];
            ExtrusionPath* new_path = new ExtrusionPath(rl.role, rl.mm3, rl.width, rl.height);
            new_path->polyline = std::move(rl.pl);
            encode_tool_in_path(new_path, tool_idx);
            tool_blocks[tool_idx].push_back(new_path);
        }

        if (tool_blocks.empty()) continue;

        // Nearest-neighbor travel optimization within each tool's block.
        // Minimizes travel moves and allows endpoint flipping per line.
        for (int t : unique_tool_order)
            optimize_tool_block_travel(tool_blocks[t]);

        // Replace sub-collection with unique-tool blocks in first-occurrence order.
        // no_sort=true prevents path reordering and breaking block grouping.
        for (auto* e : sub->entities) delete e;
        sub->entities.clear();
        sub->no_sort = true;
        for (int t : unique_tool_order)
            for (auto* p : tool_blocks[t])
                sub->entities.push_back(p);

        debug_log(layer_idx, unique_tool_order, tool_blocks); // NeoDebug guard is inside debug_log()

        any_modified = true;
        // NEOTKO_COLORMIX_TAG_END - unique-tool block merge
    }

    int flags = 0;
    if (any_modified)    flags |= COLORMIX_FLAG_MODIFIED;
    if (any_unsplittable) flags |= COLORMIX_FLAG_UNSPLITTABLE;
    return flags;
}

// ---------------------------------------------------------------------------
// NEOTKO_COLORMIX_TAG — s60 numeric gradient (Step 1 of UX plan).
// Bresenham-style dither for "Linear 2-color" mode.
//
// Mathematical core: we want exactly `count_b = round(n_lines * pct_b/100)`
// instances of tool B distributed amongst `n_lines - count_b` instances of
// tool A, with the local frequency of B at position i approximating
// `pct_b/100`.  Bresenham's line algorithm achieves this with a single
// integer accumulator — no random sampling, no quality knobs, perfectly
// deterministic, optimal distribution.
//
// Compared to a hard band split ("AAA…BBB…") this gives the eye a smooth
// transition: at any sub-window of length W, the fraction of B is within
// 1/W of the target ratio.  Visually reads as a continuous gradient.
//
// Edge cases:
//   pct_a = 0   → all tool_b
//   pct_a = 100 → all tool_a
//   n_lines <= 0 → empty vector
// ---------------------------------------------------------------------------
// Apply easing curve to a position t ∈ [0,1]. The returned value is also in
// [0,1] and acts as the "effective t" that the dither uses to decide which
// tool wins. Linear (default) is the identity; other curves bias the
// transition zone toward one end of the gradient.
double SurfaceColorMix::colormix_easing_apply(double t, int easing, double gamma)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (easing) {
    case kColormixEasing_EaseIn:    return t * t;
    case kColormixEasing_EaseOut:   return 1.0 - (1.0 - t) * (1.0 - t);
    case kColormixEasing_EaseInOut: return t * t * (3.0 - 2.0 * t); // smoothstep
    case kColormixEasing_Gamma:     {
        const double g = std::clamp(gamma, 0.1, 10.0);
        return std::pow(t, g);
    }
    case kColormixEasing_HardBand:
        // Step at t=0.5 — no transition zone, two clean halves.
        return (t < 0.5) ? 0.0 : 1.0;
    case kColormixEasing_Linear:
    default:                        return t;
    }
}

// Bresenham 2-color dither with easing.
//
// Algorithm: target_b_count(i) = (i+1) * pct_b / 100 in the LINEAR case.
// With easing, we replace the linear ramp by `easing(t) * pct_b * n / 100`,
// where t = i / (n-1). This way the cumulative count of B at position i is
// shaped by the easing curve — Bresenham still picks A or B based on whether
// the running count is ahead of or behind the target, giving optimally
// distributed emission for any monotonic ramp.
std::vector<int> SurfaceColorMix::build_dithered_tools_2color(
    int n_lines, int tool_a, int tool_b, int pct_a, int easing, double gamma)
{
    std::vector<int> out;
    if (n_lines <= 0) return out;
    out.reserve(static_cast<size_t>(n_lines));

    const int p_a = std::clamp(pct_a, 0, 100);
    const int p_b = 100 - p_a;
    if (p_b == 0) { out.assign(static_cast<size_t>(n_lines), tool_a); return out; }
    if (p_a == 0) { out.assign(static_cast<size_t>(n_lines), tool_b); return out; }

    // Total B emissions we want across the whole sequence.
    const double total_b_d = static_cast<double>(n_lines) * static_cast<double>(p_b) / 100.0;
    const int    total_b   = static_cast<int>(std::lround(total_b_d));

    // Walk the sequence, comparing emitted_b vs target_b(i). Easing reshapes
    // the target curve so emissions cluster where the easing function rises
    // fastest. Standard Bresenham fast-path when easing==Linear.
    int emitted_b = 0;
    const double denom = std::max(1.0, static_cast<double>(n_lines - 1));
    for (int i = 0; i < n_lines; ++i) {
        const double t_lin = (n_lines == 1) ? 0.5 : (static_cast<double>(i) / denom);
        const double t_eff = colormix_easing_apply(t_lin, easing, gamma);
        // target_b: how many B emissions we should have by position i (inclusive),
        // scaled so that target_b(n-1) == total_b. Bias by half so the first
        // and last picks aren't both A on symmetric ratios.
        const double target_b = t_eff * static_cast<double>(total_b)
                              + (t_eff - 0.5) * 0.0; // (centring already handled by ceil/lround)
        const int    need_b   = static_cast<int>(std::lround(target_b));
        if (emitted_b < need_b && emitted_b < total_b) {
            out.push_back(tool_b);
            ++emitted_b;
        } else {
            out.push_back(tool_a);
        }
    }
    return out;
}

// 3-color dither: morphs A → B → C across the sequence.
// Internally we run two independent Bresenham trackers (one for B, one for C)
// whose target curves are shaped so that:
//   - Tool A dominates at t=0
//   - Tool B peaks around the middle
//   - Tool C dominates at t=1
// The simplest reproducible way: split the sequence in two halves along the
// "A→B transition" and "B→C transition" markers using cumulative pct mass.
// At each step we pick whichever of {A, B, C} is most behind its expected
// share — a 3-way Bresenham. Optimal distribution; deterministic.
std::vector<int> SurfaceColorMix::build_dithered_tools_3color(
    int n_lines, int tool_a, int tool_b, int tool_c,
    int pct_a, int pct_b, int easing, double gamma, double overlap)
{
    std::vector<int> out;
    if (n_lines <= 0) return out;
    out.reserve(static_cast<size_t>(n_lines));

    int p_a = std::clamp(pct_a, 0, 100);
    int p_b = std::clamp(pct_b, 0, 100 - p_a);
    int p_c = 100 - p_a - p_b;

    // Target totals across the whole sequence.
    const double n = static_cast<double>(n_lines);
    const int target_a_total = static_cast<int>(std::lround(n * p_a / 100.0));
    const int target_b_total = static_cast<int>(std::lround(n * p_b / 100.0));
    const int target_c_total = n_lines - target_a_total - target_b_total;

    // Weight halfwidth controls how much each colour bleeds into its
    // neighbour's zone.
    //   ov=0.0  → halfwidth=0.5 → triangular weights → hard 3-band split
    //   ov=1.0  → halfwidth=1.0 → strong overlap, every colour appears in
    //                              every position (with locally varying odds)
    // Anchors: A @ t=0, B @ t=0.5, C @ t=1. We use a hat function for each:
    //   w_x(t) = max(0, halfwidth - |t - anchor_x|) / halfwidth
    // This is a generalised triangle whose support is [anchor - halfwidth,
    // anchor + halfwidth] clipped to [0,1]. The Bresenham deficit logic
    // below still drives the cumulative counts toward (p_a, p_b, p_c) so the
    // GLOBAL ratios are preserved exactly — only the LOCAL distribution
    // changes shape with overlap.
    const double halfwidth = 0.5 + std::clamp(overlap, 0.0, 1.0) * 0.5;

    // Pre-compute cumulative weight sums per tool across the whole sequence.
    // The expected emissions of tool x by position i is then
    //   expected_x(i) = cum_x[i] / area_x * target_x_total
    // which (a) starts at 0, (b) reaches target_x_total at i = n-1, (c) grows
    // monotonically and proportionally to where the tool's weight is non-zero.
    // This eliminates the "extra A band at the end" glitch that the previous
    // (cum_pos-based) target curves produced once a tool exhausted its quota.
    std::vector<double> cum_a(n_lines), cum_b(n_lines), cum_c(n_lines);
    double sum_a = 0.0, sum_b = 0.0, sum_c = 0.0;
    const double denom = std::max(1.0, static_cast<double>(n_lines - 1));
    for (int i = 0; i < n_lines; ++i) {
        const double t_lin = (n_lines == 1) ? 0.5 : (static_cast<double>(i) / denom);
        const double t_eff = colormix_easing_apply(t_lin, easing, gamma);
        auto hat = [&](double anchor) -> double {
            return std::max(0.0, halfwidth - std::abs(t_eff - anchor)) / halfwidth;
        };
        sum_a += hat(0.0);
        sum_b += hat(0.5);
        sum_c += hat(1.0);
        cum_a[i] = sum_a;
        cum_b[i] = sum_b;
        cum_c[i] = sum_c;
    }
    const double area_a = std::max(1e-9, sum_a);
    const double area_b = std::max(1e-9, sum_b);
    const double area_c = std::max(1e-9, sum_c);

    int emitted_a = 0, emitted_b = 0, emitted_c = 0;
    for (int i = 0; i < n_lines; ++i) {
        const double exp_a = (cum_a[i] / area_a) * static_cast<double>(target_a_total);
        const double exp_b = (cum_b[i] / area_b) * static_cast<double>(target_b_total);
        const double exp_c = (cum_c[i] / area_c) * static_cast<double>(target_c_total);

        // Tool with the largest deficit wins, sinking exhausted tools so the
        // tail-fill bias (A reappearing near t=1) cannot happen.
        const double def_a = (emitted_a < target_a_total) ? (exp_a - emitted_a) : -1e9;
        const double def_b = (emitted_b < target_b_total) ? (exp_b - emitted_b) : -1e9;
        const double def_c = (emitted_c < target_c_total) ? (exp_c - emitted_c) : -1e9;

        if (def_a >= def_b && def_a >= def_c)      { out.push_back(tool_a); ++emitted_a; }
        else if (def_b >= def_c)                   { out.push_back(tool_b); ++emitted_b; }
        else                                       { out.push_back(tool_c); ++emitted_c; }
    }
    return out;
}

// Custom hard bands. Cycles through up to 4 (tool, count) pairs (zero counts
// are skipped) until n_lines is reached. Useful when the user wants explicit
// banded gradients ("first 30 lines A, then 15 B, then 30 A again, repeat").
std::vector<int> SurfaceColorMix::build_custom_bands(
    int n_lines,
    int tool_a, int cnt_a,
    int tool_b, int cnt_b,
    int tool_c, int cnt_c,
    int tool_d, int cnt_d)
{
    std::vector<int> out;
    if (n_lines <= 0) return out;
    out.reserve(static_cast<size_t>(n_lines));

    struct Band { int tool; int count; };
    std::vector<Band> bands;
    // NEOTKO_COLORMIX_TAG — s60: skip slots with tool index < 0 ("off") in
    // addition to count == 0. Prevents the silent T0-fallback that happened
    // when the legacy config left tool_c/_d at their -1 default but the user
    // configured band counts for those slots.
    if (cnt_a > 0 && tool_a >= 0) bands.push_back({tool_a, cnt_a});
    if (cnt_b > 0 && tool_b >= 0) bands.push_back({tool_b, cnt_b});
    if (cnt_c > 0 && tool_c >= 0) bands.push_back({tool_c, cnt_c});
    if (cnt_d > 0 && tool_d >= 0) bands.push_back({tool_d, cnt_d});

    if (bands.empty()) {
        const int t = (tool_a >= 0) ? tool_a : 0;
        out.assign(static_cast<size_t>(n_lines), t);
        return out;
    }

    int produced = 0;
    while (produced < n_lines) {
        for (const Band& b : bands) {
            for (int j = 0; j < b.count && produced < n_lines; ++j, ++produced)
                out.push_back(b.tool);
            if (produced >= n_lines) break;
        }
    }
    return out;
}

// Geometric estimate of line count for a surface.
//   line_spacing = line_width * (1 - overlap)
//   For rectilinear at "best case" angle the line count ~ √area / spacing.
//   For irregular shapes we apply a pattern_factor and a √2 correction so
//   the estimate matches typical slicer output within ±15%.
int SurfaceColorMix::estimate_surface_line_count(
    double area_mm2,
    double line_width_mm,
    double overlap_fraction,
    double pattern_factor)
{
    if (area_mm2 <= 0.0 || line_width_mm <= 0.0) return 0;
    const double overlap = std::clamp(overlap_fraction, 0.0, 0.99);
    const double spacing = line_width_mm * (1.0 - overlap);
    if (spacing <= 1e-6) return 0;
    // sqrt(area) / spacing × √2 ≈ diagonal traversal of a square of area=area_mm2.
    const double side = std::sqrt(std::max(0.0, area_mm2));
    const double n = side * std::sqrt(2.0) / spacing * std::clamp(pattern_factor, 0.5, 1.5);
    if (n < 0.0) return 0;
    if (n > 1e6) return 1000000; // sanity cap
    return static_cast<int>(std::lround(n));
}

// ---------------------------------------------------------------------------
// Encode tool index in mm3_per_mm:  original + (tool_idx + 1) * 10.0
// GCode.cpp detects via mm3_per_mm >= 10.0, decodes: tool = floor/10 - 1
// ---------------------------------------------------------------------------
void SurfaceColorMix::encode_tool_in_path(ExtrusionPath* path, int tool_idx)
{
    // tool_idx is always a 0-based physical extruder index at this point.
    // Virtual MixedFilament digits are expanded to physical components before encoding
    // (in build_tool_list_from_pattern) so GCode.cpp decode needs no virtual resolution.
    // Clamp to 14 to support up to 15 physical extruders without wrapping.
    int safe_tool = std::max(0, std::min(tool_idx, 14));
    path->mm3_per_mm += static_cast<double>(safe_tool + 1) * 10.0;
}

// ---------------------------------------------------------------------------
// Debug logging
// ---------------------------------------------------------------------------
void SurfaceColorMix::debug_log(
    int layer_idx,
    const std::vector<int>& tools,
    const std::map<int, std::vector<ExtrusionPath*>>& grouped
) {
    if (!NeoDebug::enabled(NeoDebug::COLORMIX)) return;
    std::ostringstream _s;
    _s << "=== COLORMIX Layer " << layer_idx << " ===\n  Tools: [";
    for (size_t i = 0; i < tools.size(); ++i) _s << (i?",":"") << "T" << tools[i];
    _s << "]";
    for (const auto& pair : grouped) {
        double total_mm = 0.0;
        for (const auto* p : pair.second)
            total_mm += static_cast<double>(p->polyline.length()) / 1e6;
        _s << "\n  T" << pair.first << ": " << pair.second.size()
           << " paths, " << total_mm << " mm";
    }
    NeoDebug::write(NeoDebug::COLORMIX, _s.str());
}

// NEOTKO_COLORMIX_TAG_END

// NEOTKO_COLORMIX_TAG_START - MixedFilament UI helpers

// ---------------------------------------------------------------------------
// Translate a MixedFilament to a pattern string usable by ColorMix.
// Priority: manual_pattern > gradient_component_ids > ratio/component build.
// ---------------------------------------------------------------------------
std::string SurfaceColorMix::mixed_filament_to_pattern(const MixedFilament& mf)
{
    if (!mf.manual_pattern.empty())
        return mf.manual_pattern;
    if (!mf.gradient_component_ids.empty())
        return mf.gradient_component_ids;
    std::string pat;
    for (int i = 0; i < std::max(1, mf.ratio_a); i++)
        pat += std::to_string(mf.component_a);
    for (int i = 0; i < std::max(1, mf.ratio_b); i++)
        pat += std::to_string(mf.component_b);
    return pat;
}

// ---------------------------------------------------------------------------
// Parse mixed_filament_definitions string and build the ColorMixOption list.
// Enabled + non-deleted mixed entries come first, then physical filaments.
// ---------------------------------------------------------------------------
std::vector<ColorMixOption> SurfaceColorMix::get_mix_options(
    const std::string&              mixed_defs,
    const std::vector<std::string>& filament_colours)
{
    std::vector<ColorMixOption> result;

    const int num_physical = (int)filament_colours.size();

    if (!mixed_defs.empty()) {
        MixedFilamentManager mgr;
        // auto_generate must run before load_custom_entries so that auto rows
        // (custom=false) in the serialised data find their match in auto_rows_by_pair.
        // Without it every row is skipped ("auto row missing after regenerate").
        const bool was_auto = MixedFilamentManager::auto_generate_enabled();
        MixedFilamentManager::set_auto_generate_enabled(true);
        mgr.auto_generate(filament_colours);
        MixedFilamentManager::set_auto_generate_enabled(was_auto);
        mgr.load_custom_entries(mixed_defs, filament_colours);
        // Mirror PrintApply.cpp: compute ratio_a/ratio_b from mix_b_percent so that
        // mixed_filament_to_pattern() and extract_recipe_tools() dither correctly.
        // Mode 0 (LayerCycle/Simple) is the correct default — bounds unused in this mode.
        mgr.apply_gradient_settings(0, 0.0f, 1.0f, false);

        auto get_color = [&](unsigned int id) -> std::string {
            if (id >= 1 && id <= filament_colours.size())
                return filament_colours[id - 1];
            return "#888888";
        };

        int virtual_counter = 0;
        for (const auto& mf : mgr.mixed_filaments()) {
            if (mf.deleted || !mf.enabled) continue;

            ColorMixOption opt;
            opt.is_physical = false;
            // filament_id: 1-based, virtual slots start at num_physical+1.
            // Matches the digit encoding in build_tool_list_from_pattern():
            //   digit '5' (for 4-physical setup) → tool_idx 4 → virtual ID 5.
            opt.filament_id = num_physical + virtual_counter + 1;
            opt.pattern      = mixed_filament_to_pattern(mf);
            {
                const int pct_b = std::max(0, std::min(100, mf.mix_b_percent));
                const int pct_a = 100 - pct_b;
                opt.label = "Mixed " + std::to_string(virtual_counter + 1)
                          + ": F" + std::to_string(mf.component_a)
                          + "+F" + std::to_string(mf.component_b)
                          + " (" + std::to_string(pct_a) + "%/"
                          + std::to_string(pct_b) + "%)";
            }
            opt.tool_weights = SurfaceColorMix::extract_recipe_weights(mf, (size_t)num_physical);

            if (!mf.display_color.empty()) {
                opt.display_color = mf.display_color;
            } else {
                // Use mix_b_percent directly — ratio_a/ratio_b may be stale (not serialized).
                const int ra = std::max(0, 100 - mf.mix_b_percent);
                const int rb = mf.mix_b_percent;
                opt.display_color = MixedFilamentManager::blend_color(
                    get_color(mf.component_a),
                    get_color(mf.component_b),
                    ra, rb);
            }
            result.push_back(std::move(opt));
            ++virtual_counter;
        }
    }

    for (int i = 0; i < num_physical; i++) {
        ColorMixOption opt;
        opt.is_physical   = true;
        opt.filament_id   = i + 1;   // 1-based physical ID
        opt.label         = "F" + std::to_string(i + 1);
        opt.pattern       = std::to_string(i + 1);
        opt.display_color = filament_colours[i];
        result.push_back(std::move(opt));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Returns the normalized blend weight per physical tool for a MixedFilament recipe.
// Calls extract_recipe_tools() to get the dithered full sequence, then counts
// tool frequencies → proportions.  0-based tool index → weight [0..1].
// Used by MultiPass "Normalize to MixedColor %" (Tab.cpp) to set layer_ratio[].
// ---------------------------------------------------------------------------
std::map<int,float> SurfaceColorMix::extract_recipe_weights(
    const MixedFilament& mf, size_t num_physical)
{
    const auto seq = extract_recipe_tools(mf, num_physical);
    if (seq.empty()) return {};

    std::map<int,int> counts;
    for (int t : seq) counts[t]++;

    const float total = static_cast<float>(seq.size());
    std::map<int,float> weights;
    for (auto& kv : counts)
        weights[kv.first] = static_cast<float>(kv.second) / total;
    return weights;
}
// NEOTKO_COLORMIX_TAG_END

// NEOTKO_MULTIPASS_TAG_START

// ---------------------------------------------------------------------------
// MultiPassConfig::from_region_config
// ---------------------------------------------------------------------------
// role == erPenultimateInfill  → reads penultimate_multipass_* keys (independent config)
// any other role               → reads multipass_* keys (top surface config, legacy default)
MultiPassConfig MultiPassConfig::from_region_config(const PrintRegionConfig& cfg, ExtrusionRole role)
{
    MultiPassConfig c;
    if (role == erPenultimateInfill) {
        c.enabled        = cfg.penultimate_multipass_enabled.value;
        c.surface        = 2; // penultimate only — no surface filter needed
        c.num_passes     = cfg.penultimate_multipass_num_passes.value;
        c.tool[0]        = cfg.penultimate_multipass_tool_1.value;
        c.tool[1]        = cfg.penultimate_multipass_tool_2.value;
        c.tool[2]        = cfg.penultimate_multipass_tool_3.value;
        c.width_ratio[0] = cfg.penultimate_multipass_width_ratio_1.value;
        c.width_ratio[1] = cfg.penultimate_multipass_width_ratio_2.value;
        c.width_ratio[2] = cfg.penultimate_multipass_width_ratio_3.value;
        c.vary_pattern   = cfg.penultimate_multipass_vary_pattern.value;
        c.angle[0]       = cfg.penultimate_multipass_angle_1.value;
        c.angle[1]       = cfg.penultimate_multipass_angle_2.value;
        c.angle[2]       = cfg.penultimate_multipass_angle_3.value;
        c.fan[0]         = cfg.penultimate_multipass_fan_1.value;
        c.fan[1]         = cfg.penultimate_multipass_fan_2.value;
        c.fan[2]         = cfg.penultimate_multipass_fan_3.value;
        c.speed_pct[0]   = cfg.penultimate_multipass_speed_pct_1.value;
        c.speed_pct[1]   = cfg.penultimate_multipass_speed_pct_2.value;
        c.speed_pct[2]   = cfg.penultimate_multipass_speed_pct_3.value;
        c.gcode_start[0] = cfg.penultimate_multipass_gcode_start_1.value;
        c.gcode_start[1] = cfg.penultimate_multipass_gcode_start_2.value;
        c.gcode_start[2] = cfg.penultimate_multipass_gcode_start_3.value;
        c.gcode_end[0]   = cfg.penultimate_multipass_gcode_end_1.value;
        c.gcode_end[1]   = cfg.penultimate_multipass_gcode_end_2.value;
        c.gcode_end[2]   = cfg.penultimate_multipass_gcode_end_3.value;
    } else {
        c.enabled        = cfg.multipass_enabled.value;
        c.surface        = cfg.multipass_surface.value;
        c.num_passes     = cfg.multipass_num_passes.value;
        c.tool[0]        = cfg.multipass_tool_1.value;
        c.tool[1]        = cfg.multipass_tool_2.value;
        c.tool[2]        = cfg.multipass_tool_3.value;
        c.width_ratio[0] = cfg.multipass_width_ratio_1.value;
        c.width_ratio[1] = cfg.multipass_width_ratio_2.value;
        c.width_ratio[2] = cfg.multipass_width_ratio_3.value;
        c.vary_pattern   = cfg.multipass_vary_pattern.value;
        c.angle[0]       = cfg.multipass_angle_1.value;
        c.angle[1]       = cfg.multipass_angle_2.value;
        c.angle[2]       = cfg.multipass_angle_3.value;
        c.fan[0]         = cfg.multipass_fan_1.value;
        c.fan[1]         = cfg.multipass_fan_2.value;
        c.fan[2]         = cfg.multipass_fan_3.value;
        c.speed_pct[0]   = cfg.multipass_speed_pct_1.value;
        c.speed_pct[1]   = cfg.multipass_speed_pct_2.value;
        c.speed_pct[2]   = cfg.multipass_speed_pct_3.value;
        c.gcode_start[0] = cfg.multipass_gcode_start_1.value;
        c.gcode_start[1] = cfg.multipass_gcode_start_2.value;
        c.gcode_start[2] = cfg.multipass_gcode_start_3.value;
        c.gcode_end[0]   = cfg.multipass_gcode_end_1.value;
        c.gcode_end[1]   = cfg.multipass_gcode_end_2.value;
        c.gcode_end[2]   = cfg.multipass_gcode_end_3.value;
    }
    return c;
}


// NEOTKO_PROFILE_TAG_START — Fase F painter-mode MultiPass override.
//
// Build a MultiPassConfig from a profile's `multipass` payload kv map
// without going through a temporary PrintRegionConfig. Symmetric with
// `painted_profile_tools_1based` — keeps allocation off the per-region hot
// path. `role == erPenultimateInfill` reads `penultimate_multipass_*` keys;
// any other role reads top `multipass_*` keys. Missing keys → struct
// defaults.
MultiPassConfig SurfaceColorMix::multipass_from_profile_payload(
    const SurfaceEffectPayload& payload, ExtrusionRole role)
{
    MultiPassConfig c;
    auto gi = [&](const char* k, int def) {
        auto it = payload.kv.find(k);
        return (it == payload.kv.end()) ? def : std::atoi(it->second.c_str());
    };
    auto gf = [&](const char* k, double def) {
        auto it = payload.kv.find(k);
        return (it == payload.kv.end()) ? def : std::atof(it->second.c_str());
    };
    auto gb = [&](const char* k, bool def) {
        auto it = payload.kv.find(k);
        if (it == payload.kv.end()) return def;
        // ConfigOptionBool::serialize emits "1" / "0".
        return it->second == "1" || it->second == "true";
    };
    auto gs = [&](const char* k) -> std::string {
        auto it = payload.kv.find(k);
        return (it == payload.kv.end()) ? std::string() : it->second;
    };

    if (role == erPenultimateInfill) {
        c.enabled        = gb("penultimate_multipass_enabled", false);
        c.surface        = 2; // penu-only; matches from_region_config
        c.num_passes     = gi("penultimate_multipass_num_passes", 2);
        c.tool[0]        = gi("penultimate_multipass_tool_1",  0);
        c.tool[1]        = gi("penultimate_multipass_tool_2",  1);
        c.tool[2]        = gi("penultimate_multipass_tool_3", -1);
        c.width_ratio[0] = gf("penultimate_multipass_width_ratio_1", 0.50);
        c.width_ratio[1] = gf("penultimate_multipass_width_ratio_2", 0.50);
        c.width_ratio[2] = gf("penultimate_multipass_width_ratio_3", 0.34);
        c.vary_pattern   = gb("penultimate_multipass_vary_pattern", false);
        c.angle[0]       = gi("penultimate_multipass_angle_1", -1);
        c.angle[1]       = gi("penultimate_multipass_angle_2", -1);
        c.angle[2]       = gi("penultimate_multipass_angle_3", -1);
        c.fan[0]         = gi("penultimate_multipass_fan_1", -1);
        c.fan[1]         = gi("penultimate_multipass_fan_2", -1);
        c.fan[2]         = gi("penultimate_multipass_fan_3", -1);
        c.speed_pct[0]   = gi("penultimate_multipass_speed_pct_1", 100);
        c.speed_pct[1]   = gi("penultimate_multipass_speed_pct_2", 100);
        c.speed_pct[2]   = gi("penultimate_multipass_speed_pct_3", 100);
        c.gcode_start[0] = gs("penultimate_multipass_gcode_start_1");
        c.gcode_start[1] = gs("penultimate_multipass_gcode_start_2");
        c.gcode_start[2] = gs("penultimate_multipass_gcode_start_3");
        c.gcode_end[0]   = gs("penultimate_multipass_gcode_end_1");
        c.gcode_end[1]   = gs("penultimate_multipass_gcode_end_2");
        c.gcode_end[2]   = gs("penultimate_multipass_gcode_end_3");
    } else {
        c.enabled        = gb("multipass_enabled", false);
        c.surface        = gi("multipass_surface", 0);
        c.num_passes     = gi("multipass_num_passes", 2);
        c.tool[0]        = gi("multipass_tool_1",  0);
        c.tool[1]        = gi("multipass_tool_2",  1);
        c.tool[2]        = gi("multipass_tool_3", -1);
        c.width_ratio[0] = gf("multipass_width_ratio_1", 0.50);
        c.width_ratio[1] = gf("multipass_width_ratio_2", 0.50);
        c.width_ratio[2] = gf("multipass_width_ratio_3", 0.34);
        c.vary_pattern   = gb("multipass_vary_pattern", false);
        c.angle[0]       = gi("multipass_angle_1", -1);
        c.angle[1]       = gi("multipass_angle_2", -1);
        c.angle[2]       = gi("multipass_angle_3", -1);
        c.fan[0]         = gi("multipass_fan_1", -1);
        c.fan[1]         = gi("multipass_fan_2", -1);
        c.fan[2]         = gi("multipass_fan_3", -1);
        c.speed_pct[0]   = gi("multipass_speed_pct_1", 100);
        c.speed_pct[1]   = gi("multipass_speed_pct_2", 100);
        c.speed_pct[2]   = gi("multipass_speed_pct_3", 100);
        c.gcode_start[0] = gs("multipass_gcode_start_1");
        c.gcode_start[1] = gs("multipass_gcode_start_2");
        c.gcode_start[2] = gs("multipass_gcode_start_3");
        c.gcode_end[0]   = gs("multipass_gcode_end_1");
        c.gcode_end[1]   = gs("multipass_gcode_end_2");
        c.gcode_end[2]   = gs("multipass_gcode_end_3");
    }
    return c;
}

bool SurfaceColorMix::painted_perim_override_from_profile(
    const SurfaceEffectPayload& payload)
{
    auto it = payload.kv.find("multipass_perimeter_override");
    if (it == payload.kv.end()) return false;
    return it->second == "1" || it->second == "true";
}

// NEOTKO_SANDWICH_TAG — Fase 5 (s72): forward declaration of the legacy→v=2
// converter used below. The full definition lives next to the blob round-trip
// helpers (PathBlendPassConfig::from_blob_json).
static PathBlendPassConfig pathblend_convert_legacy_to_v2(int   legacy_num_passes,
                                                          int   legacy_tool_first,
                                                          int   legacy_tool_last,
                                                          float legacy_min_ratio,
                                                          float legacy_max_ratio,
                                                          int   legacy_ease_mode,
                                                          int   legacy_fill_angle);

PathBlendPassConfig SurfaceColorMix::pathblend_from_profile_payload(
    const SurfaceEffectPayload& payload)
{
    PathBlendPassConfig c;
    auto gi = [&](const char* k, int def) {
        auto it = payload.kv.find(k);
        return (it == payload.kv.end()) ? def : std::atoi(it->second.c_str());
    };
    auto gf = [&](const char* k, double def) {
        auto it = payload.kv.find(k);
        return (it == payload.kv.end()) ? def : std::atof(it->second.c_str());
    };
    auto gb = [&](const char* k, bool def) {
        auto it = payload.kv.find(k);
        if (it == payload.kv.end()) return def;
        return it->second == "1" || it->second == "true";
    };

    // NEOTKO_SANDWICH_TAG — Fase 5 (s72): convert the painter profile's legacy
    // PathBlend payload (`pathblend_min_ratio` / `pathblend_max_ratio` / etc.)
    // to the new geometry model. The painter (Fase G) still writes the legacy
    // keys into `SurfaceEffectPayload.kv` so painted profiles saved before s72
    // keep working; the read side normalises everything to the v=2 in-memory
    // representation here.
    const int legacy_np = std::clamp(gi("pathblend_num_passes", 2), 1, 4);
    const int legacy_t0 = gi("pathblend_tool_1", 0);
    const int legacy_tlast = (legacy_np >= 4) ? gi("pathblend_tool_4", 3)
                          : (legacy_np == 3) ? gi("pathblend_tool_3", 2)
                          : (legacy_np == 2) ? gi("pathblend_tool_2", 1)
                                              : legacy_t0;
    c = pathblend_convert_legacy_to_v2(
        legacy_np, legacy_t0, legacy_tlast,
        static_cast<float>(std::clamp(gf("pathblend_min_ratio", 0.05), 0.01, 0.49)),
        static_cast<float>(std::clamp(gf("pathblend_max_ratio", 1.0),  0.51, 1.0)),
        std::clamp(gi("pathblend_ease_mode", 0), 0, 3),
        gi("pathblend_fill_angle", -1));
    c.enabled = gb("multipass_path_gradient", false);
    c.surface = gi("pathblend_surface", 0);
    c.sync_legacy_view();
    return c;
}

bool SurfaceColorMix::any_painted_profile_has_perim_override(
    const PrintObject* po, double print_z, double height)
{
    if (po == nullptr) return false;
    const int top_slot  = dominant_painted_slot_in_z_range(po, print_z - height, print_z);
    const int penu_slot = dominant_painted_slot_in_z_range(po, print_z, print_z + height);
    auto& mgr = Slic3r::SurfaceEffectProfileManager::get();
    for (int slot : {top_slot, penu_slot}) {
        if (slot <= 0) continue;
        const int pid = profile_id_for_slot(po, slot);
        if (!pid) continue;
        const SurfaceEffectProfile* p = mgr.find(pid);
        if (p && p->multipass.present && painted_perim_override_from_profile(p->multipass))
            return true;
    }
    return false;
}
// NEOTKO_PROFILE_TAG_END


// NEOTKO_NEOWEAVING_TAG_START
// NeoweaveEngine — Z-axis interdigitation logic extracted from GCode.cpp.
// All Z-motion computation lives here; GCode.cpp calls needs_weave() + apply_path() + restore_z().
// (GCodeWriter.hpp + ExtrusionEntity.hpp included at file top — outside namespace Slic3r)

// Keep M_PI available without relying on platform extensions
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool NeoweaveEngine::needs_weave(const ExtrusionPath& path, const PrintRegionConfig& cfg)
{
    if (!cfg.interlayer_neoweave_enabled.value)
        return (cfg.infill_neoweave_enabled.value == InfillNeoweaveOverride::Enable
                && path.role() == erInternalInfill);

    // NEOTKO_NEOWEAVING_TAG_START — Wave mode disabled (known crash)
    // Root cause: Wave subdivides every line into ≥8 micro-segments, producing O(lines×segs)
    // std::string::append calls. On a complex top surface (10k+ lines) this exhausts RAM
    // (8 GB Mac mini M2 with Orca already at 5.5 GB) → OS page-in stall → freeze + crash.
    // The Wave code is intentionally kept for future work (streaming / pre-reserved buffer).
    // TODO: fix by pre-reserving gcode string capacity or streaming directly to output.
    if (cfg.interlayer_neoweave_mode.value == NeoweaveMode::Wave)
        return false;
    // NEOTKO_NEOWEAVING_TAG_END

    const bool linear = (cfg.interlayer_neoweave_mode.value == NeoweaveMode::Linear);
    const NeoweaveFilter filter = cfg.neoweave_filter.value;

    // Surface/penultimate roles always qualify
    if (path.role() == erTopSolidInfill)
        return true;
    // Penultimate: respect neoweave_penultimate_layers (0 = disabled)
    if (path.role() == erPenultimateInfill)
        return cfg.neoweave_penultimate_layers.value > 0;
    // Solid infill in Linear+All mode for interlayer angle-lock synergy
    if (linear && path.role() == erSolidInfill && filter == NeoweaveFilter::All)
        return true;
    // Infill override (tristate)
    if (cfg.infill_neoweave_enabled.value == InfillNeoweaveOverride::Enable
            && path.role() == erInternalInfill)
        return true;
    return false;
}

std::string NeoweaveEngine::apply_path(
    const ExtrusionPath&                       path,
    const PrintRegionConfig&                   cfg,
    GCodeWriter&                               writer,
    int                                        layer_index,
    double                                     nominal_z,
    double                                     F,
    double                                     e_per_mm,
    bool                                       is_force_no_extr,
    const std::function<Vec2d(const Point&)>&  point_to_gcode)
{
    std::string gcode;

    // ── Resolve which weave mode applies ────────────────────────────────────────
    const bool surface_enabled = cfg.interlayer_neoweave_enabled.value;
    const bool linear          = surface_enabled && (cfg.interlayer_neoweave_mode.value == NeoweaveMode::Linear);
    const NeoweaveFilter filter = cfg.neoweave_filter.value;

    const bool surface_weave =
        surface_enabled &&
        (path.role() == erTopSolidInfill
         || path.role() == erPenultimateInfill
         || (linear && path.role() == erSolidInfill && filter == NeoweaveFilter::All));

    const bool infill_weave =
        (cfg.infill_neoweave_enabled.value == InfillNeoweaveOverride::Enable)
        && path.role() == erInternalInfill;

    const bool any_weave = surface_weave || infill_weave;
    if (!any_weave)
        return gcode; // caller handles normal extrusion

    // ── Resolve parameter set ────────────────────────────────────────────────────
    double weave_period = surface_weave ? cfg.interlayer_neoweave_period.value
                                        : cfg.infill_neoweave_period.value;
    if (weave_period < 1e-9) {
        weave_period = unscale<double>(path.width);
        if (weave_period < 1e-9) weave_period = 0.4;
    }
    const double weave_amplitude = surface_weave ? cfg.interlayer_neoweave_amplitude.value
                                                 : cfg.infill_neoweave_amplitude.value;
    const double weave_max_z_speed = surface_weave ? cfg.interlayer_neoweave_max_z_speed.value
                                                   : cfg.infill_neoweave_max_z_speed.value;
    const double weave_min_length  = cfg.interlayer_neoweave_min_length.value;

    if (weave_amplitude < 1e-9)
        return gcode; // degenerate — nothing to emit; caller falls through to normal

    // ── Wave mode: cap XY speed globally ────────────────────────────────────────
    double weave_F = F;
    if (!linear && weave_period > 1e-9) {
        const double xy_speed_max = weave_max_z_speed * weave_period / (2.0 * M_PI * weave_amplitude);
        const double xy_F_max     = xy_speed_max * 60.0; // mm/min
        weave_F = std::min(F, xy_F_max);
        if (std::abs(weave_F - F) > 1e-9)
            gcode += writer.set_speed(weave_F, "", "");
    }

    // ── Speed override (Linear mode) ────────────────────────────────────────────
    // neoweave_speed_pct scales the G1 F on the Z move; firmware inherits it for
    // the following extrude_to_xy (no explicit F emitted there). restore_z resets
    // to the original F via its own G1 Z move.
    const int speed_pct = std::max(1, std::min(200, cfg.neoweave_speed_pct.value));
    const double weave_line_F = F * speed_pct / 100.0;

    // ── Per-line state ────────────────────────────────────────────────────────────
    double weave_dist    = 0.0;
    int    weave_line_idx = 0;

    for (const Line& line : path.polyline.lines()) {
        const double line_length = line.length() * SCALING_FACTOR;
        if (line_length < 1e-9) continue;
        const double dE = e_per_mm * line_length;

        if (linear && surface_weave) {
            // ── Linear mode ───────────────────────────────────────────────────────
            // Auto-minimum: max(user_min_length, 2×line_width) filters connector segments.
            const double auto_min     = std::max(weave_min_length,
                                                  2.0 * unscale<double>(path.width));
            const bool   line_too_short = line_length < auto_min;

            if (line_too_short) {
                // Connector — extrude at current Z, do not count toward line index
                gcode += writer.extrude_to_xy(point_to_gcode(line.b), dE, "", is_force_no_extr);
            } else {
                // Alternate per-line: 0/+A with layer parity flip for interlayer nesting.
                // 0/+A (never below nominal) avoids moiré interference between objects.
                const bool layer_flip   = (layer_index % 2 != 0);
                const bool line_is_even = (weave_line_idx % 2 == 0);
                const bool elevated     = (line_is_even == layer_flip); // XOR → elevated
                const double target_z   = nominal_z + (elevated ? weave_amplitude : 0.0);

                // G1 Z move at neoweave speed (NOT travel speed — §7.6 NeoweaveF bug).
                // weave_line_F = F * neoweave_speed_pct/100; firmware inherits it for
                // the following extrude_to_xy. restore_z resets back to F.
                {
                    GCodeG1Formatter w;
                    w.emit_z(target_z);
                    w.emit_f(weave_line_F);
                    w.emit_comment(GCodeWriter::full_gcode_comment,
                        elevated ? "Neoweaving: line Z +A" : "Neoweaving: line Z nominal");
                    gcode += w.string();
                    writer.get_position().z() = target_z;
                }
                gcode += writer.extrude_to_xy(point_to_gcode(line.b), dE, "", is_force_no_extr);
                ++weave_line_idx;
            }
            weave_dist += line_length;

        } else if (!linear && weave_period > 1e-9) {
            // ── Wave mode ─────────────────────────────────────────────────────────
            // Subdivide line into ≥8 micro-segments per period for smooth sinusoid.
            const int    n_per_period = 8;
            const double seg_target   = weave_period / double(n_per_period);
            const int    n_segs       = std::max(1, (int)std::ceil(line_length / seg_target));
            const double seg_len      = line_length / double(n_segs);
            const double dE_seg       = dE / double(n_segs);

            const Vec2d pt_a = point_to_gcode(line.a);
            const Vec2d pt_b = point_to_gcode(line.b);

            for (int si = 0; si < n_segs; ++si) {
                const double t     = double(si + 1) / double(n_segs);
                const Vec2d  pt    = pt_a + t * (pt_b - pt_a);
                const double d     = weave_dist + seg_len * double(si + 1);
                const double phase = (d / weave_period) * 2.0 * M_PI;
                const double z_off = weave_amplitude * std::sin(phase);
                const Vec3d  dest3d(pt(0), pt(1), nominal_z + z_off);
                gcode += writer.extrude_to_xyz(dest3d, dE_seg, "", is_force_no_extr);
            }
            weave_dist += line_length;

        } else {
            // Degenerate (wave with zero period) — plain extrusion
            gcode += writer.extrude_to_xy(point_to_gcode(line.b), dE, "", is_force_no_extr);
        }
    }

    return gcode;
}

std::string NeoweaveEngine::restore_z(
    const PrintRegionConfig& cfg,
    GCodeWriter&             writer,
    double                   nominal_z,
    double                   F,
    bool                     surface_weave_active)
{
    std::string gcode;
    const bool linear = cfg.interlayer_neoweave_enabled.value
                     && (cfg.interlayer_neoweave_mode.value == NeoweaveMode::Linear);

    const char* comment = surface_weave_active
        ? "Neotko Neoweaving: restore layer Z"
        : "Neotko infill neoweaving: restore layer Z";

    if (linear) {
        // G1 Z move at print speed to avoid travel_to_z emitting travel F (§7.6 NeoweaveF fix)
        GCodeG1Formatter w;
        w.emit_z(nominal_z);
        w.emit_f(F);
        w.emit_comment(GCodeWriter::full_gcode_comment, comment);
        gcode += w.string();
        writer.get_position().z() = nominal_z;
    } else {
        gcode += writer.travel_to_z(nominal_z, comment);
    }
    return gcode;
}
// NEOTKO_NEOWEAVING_TAG_END

// NEOTKO_PATHBLEND_TAG_START — PathBlendPassConfig implementation

// NEOTKO_SANDWICH_TAG — Fase 5 (s72): sync the legacy view (num_passes/tool[])
// from the geometry fields (mode/tool_bottom/tool_top). Called automatically
// by from_region_config / from_blob_json after writing the geometry fields,
// so existing iterating callers (GCode COLORMIX_HOOK pre-seed, ToolOrdering,
// Fill.cpp PB block) keep reading the right tools without per-call awareness
// of Half vs Full.
void PathBlendPassConfig::sync_legacy_view()
{
    if (mode == Mode::Full) {
        num_passes = 2;
        tool[0] = tool_bottom;
        tool[1] = tool_top;
    } else { // Half
        num_passes = 1;
        tool[0] = tool_bottom;
        tool[1] = -1;
    }
    tool[2] = -1;
    tool[3] = -1;
}

// NEOTKO_PATHBLEND_TAG — s69 miniblob: JSON round-trip for the per-zone blob.
// The blob carries only the per-zone settings; enable + surface are the shared
// scope keys, applied by from_region_config() from the live config.
// NEOTKO_SANDWICH_TAG — Fase 5 (s72): blob v=2 schema. The Sandwich UX edits
// the new geometry fields directly (mode/floor_mm/mid_end_mm/tool_bottom/
// tool_top); the legacy view is synced for downstream iterators.
std::string PathBlendPassConfig::to_blob_json() const
{
    nlohmann::json j;
    j["v"]           = 2;
    j["mode"]        = (mode == Mode::Full) ? "full" : "half";
    j["floor_mm"]    = floor_mm;
    j["mid_end_mm"]  = mid_end_mm;
    j["tool_bottom"] = tool_bottom;
    j["tool_top"]    = tool_top;
    j["ease_mode"]   = ease_mode;
    j["fill_angle"]  = fill_angle;
    return j.dump();
}

// Helper — convert a legacy (v=1 / flat-keys) representation to the new
// geometry model. Lossy: 3/4-pass PathBlend collapses to Full keeping only
// tool[0] and tool[num_passes-1]; min_ratio/max_ratio map to floor/mid_end
// fractions of the unit interval and the dialog will clamp once layer_height
// is known. Pre-release, accepted by the user as a one-way conversion.
static PathBlendPassConfig pathblend_convert_legacy_to_v2(int   legacy_num_passes,
                                                          int   legacy_tool_first,
                                                          int   legacy_tool_last,
                                                          float legacy_min_ratio,
                                                          float legacy_max_ratio,
                                                          int   legacy_ease_mode,
                                                          int   legacy_fill_angle)
{
    PathBlendPassConfig c;
    c.mode        = (legacy_num_passes <= 1)
                  ? PathBlendPassConfig::Mode::Half
                  : PathBlendPassConfig::Mode::Full;
    // min_ratio / max_ratio are fractions of layer_height in the legacy model.
    // The dialog re-clamps once H is known; here we just translate the numbers
    // so a 0.20mm-layer preset comes through with sensible defaults.
    const double H_default = 0.20; // assumed when no layer context yet
    c.floor_mm    = static_cast<float>(std::clamp(double(legacy_min_ratio) * H_default,
                                                  0.01, H_default - 0.04));
    c.mid_end_mm  = static_cast<float>(std::clamp(double(legacy_max_ratio) * H_default,
                                                  double(c.floor_mm), H_default - 0.04));
    if (c.mid_end_mm < c.floor_mm) c.mid_end_mm = c.floor_mm;
    c.tool_bottom = legacy_tool_first;
    c.tool_top    = (c.mode == PathBlendPassConfig::Mode::Full) ? legacy_tool_last : -1;
    c.ease_mode   = std::clamp(legacy_ease_mode, 0, 3);
    c.fill_angle  = legacy_fill_angle;
    c.sync_legacy_view();
    return c;
}

PathBlendPassConfig PathBlendPassConfig::from_blob_json(const std::string& blob)
{
    PathBlendPassConfig c;  // defaults: Full, floor=0.01, mid_end=0.05
    c.sync_legacy_view();
    if (blob.empty()) return c;
    try {
        nlohmann::json j = nlohmann::json::parse(blob);
        if (!j.is_object()) return c;

        auto gi = [&](const char* k, int def) {
            return (j.contains(k) && j[k].is_number()) ? j[k].get<int>() : def; };
        auto gf = [&](const char* k, double def) {
            return (j.contains(k) && j[k].is_number()) ? j[k].get<double>() : def; };

        const int  version  = gi("v", 1); // missing "v" → legacy v=1
        const bool is_v2 = (version >= 2)
                        || j.contains("mode")
                        || j.contains("floor_mm")
                        || j.contains("mid_end_mm");

        if (is_v2) {
            // v=2: read geometry fields directly.
            std::string mode_str = (j.contains("mode") && j["mode"].is_string())
                                   ? j["mode"].get<std::string>() : "full";
            c.mode        = (mode_str == "half") ? Mode::Half : Mode::Full;
            c.floor_mm    = static_cast<float>(gf("floor_mm",   0.01));
            c.mid_end_mm  = static_cast<float>(gf("mid_end_mm", 0.05));
            c.tool_bottom = gi("tool_bottom", 0);
            c.tool_top    = gi("tool_top",    1);
            c.ease_mode   = std::clamp(gi("ease_mode", 0), 0, 3);
            c.fill_angle  = gi("fill_angle", -1);
            // Hard constraint: floor >= 0.01.
            if (c.floor_mm   < 0.01f)         c.floor_mm   = 0.01f;
            if (c.mid_end_mm < c.floor_mm)    c.mid_end_mm = c.floor_mm;
        } else {
            // v=1 legacy: num_passes / tool[] / layer_ratio / min/max / invert.
            int   legacy_np   = std::clamp(gi("num_passes", 2), 1, 4);
            int   legacy_t0   = 0;
            int   legacy_tlast = 1;
            if (j.contains("tool") && j["tool"].is_array()) {
                if ((int)j["tool"].size() > 0 && j["tool"][0].is_number())
                    legacy_t0 = j["tool"][0].get<int>();
                if ((int)j["tool"].size() > std::max(0, legacy_np - 1) &&
                    j["tool"][legacy_np - 1].is_number())
                    legacy_tlast = j["tool"][legacy_np - 1].get<int>();
            }
            const float legacy_min   = static_cast<float>(
                std::clamp(gf("min_ratio", 0.05), 0.01, 0.49));
            const float legacy_max   = static_cast<float>(
                std::clamp(gf("max_ratio", 1.0),  0.51, 1.0));
            const int   legacy_ease  = std::clamp(gi("ease_mode", 0), 0, 3);
            const int   legacy_angle = gi("fill_angle", -1);
            c = pathblend_convert_legacy_to_v2(legacy_np, legacy_t0, legacy_tlast,
                                               legacy_min, legacy_max,
                                               legacy_ease, legacy_angle);
            // (sync already done inside the helper)
            return c;
        }
    } catch (const std::exception& e) {
        NEOTKO_LOG(MULTIPASS, "PB_BLOB parse error: " << e.what()
            << " — using PathBlend defaults");
    }
    c.sync_legacy_view();
    return c;
}

PathBlendPassConfig PathBlendPassConfig::from_region_config(const PrintRegionConfig& cfg,
                                                            ExtrusionRole role)
{
    // NEOTKO_PATHBLEND_TAG — s69 miniblob: a non-empty per-zone JSON blob
    // overrides the flat pathblend_* keys, so Top and Penultimate PathBlend
    // hold independent settings.
    //
    // NEOTKO_SANDWICH_TAG — Fase 5 (s72): from_blob_json transparently handles
    // both v=2 (new Fase 5 schema) and v=1 (s69 legacy) blobs. When the blob
    // is empty, the flat pathblend_* keys are read once and converted to v=2.
    const std::string& blob = (role == erPenultimateInfill)
        ? cfg.pathblend_penu.value
        : cfg.pathblend_top.value;
    PathBlendPassConfig c;
    if (!blob.empty()) {
        c = from_blob_json(blob);
    } else {
        // Legacy fallback: flat pathblend_* keys → convert to v=2 once.
        c = pathblend_convert_legacy_to_v2(
            std::clamp(cfg.pathblend_num_passes.value, 1, 4),
            cfg.pathblend_tool_1.value,
            // Pick tool[num_passes-1] as the cap tool.
            (cfg.pathblend_num_passes.value >= 4) ? cfg.pathblend_tool_4.value :
            (cfg.pathblend_num_passes.value == 3) ? cfg.pathblend_tool_3.value :
            (cfg.pathblend_num_passes.value == 2) ? cfg.pathblend_tool_2.value :
                                                    cfg.pathblend_tool_1.value,
            static_cast<float>(std::clamp(cfg.pathblend_min_ratio.value, 0.01, 0.49)),
            static_cast<float>(std::clamp(cfg.pathblend_max_ratio.value, 0.51, 1.0)),
            std::clamp(cfg.pathblend_ease_mode.value, 0, 3),
            cfg.pathblend_fill_angle.value);
    }
    // enable + surface are shared scope keys — never stored in the per-zone blob.
    c.enabled = cfg.multipass_path_gradient.value; // PathBlend independent of multipass_enabled
    c.surface = cfg.pathblend_surface.value;
    c.sync_legacy_view();
    return c;
}
// NEOTKO_PATHBLEND_TAG_END

// NEOTKO_SANDWICH_TAG_START
// ===========================================================================
// SurfacePassStack — JSON serialization + legacy synthesis (Fase 0).
// ===========================================================================
namespace {
    nlohmann::json sandwich_payload_to_json(const SurfaceEffectPayload& p) {
        nlohmann::json j;
        j["present"] = p.present;
        j["kv"]      = p.kv;
        return j;
    }
    SurfaceEffectPayload sandwich_payload_from_json(const nlohmann::json& j) {
        SurfaceEffectPayload out;
        if (j.is_object()) {
            if (j.contains("present") && j["present"].is_boolean())
                out.present = j["present"].get<bool>();
            if (j.contains("kv") && j["kv"].is_object())
                for (auto it = j["kv"].begin(); it != j["kv"].end(); ++it)
                    if (it.value().is_string())
                        out.kv[it.key()] = it.value().get<std::string>();
        }
        return out;
    }
} // anon namespace

bool SurfacePassStack::all_solid() const
{
    for (const SurfacePass& p : passes)
        if (p.kind != SurfacePassKind::Solid) return false;
    return true;
}

std::string SurfacePassStack::to_json() const
{
    // Empty/disabled stack → "" so the config key stays at its empty default
    // and synthesize_from_legacy() remains authoritative for untouched presets.
    if (!enabled || passes.empty())
        return std::string();

    nlohmann::json root;
    root["v"]                  = 1;
    root["enabled"]            = enabled;
    root["perimeter_override"] = perimeter_override;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : passes) {
        nlohmann::json e;
        e["kind"]        = static_cast<int>(p.kind);
        e["ratio"]       = p.ratio;
        e["solid_tool"]  = p.solid_tool;
        e["angle"]       = p.angle;
        e["fan"]         = p.fan;
        e["speed_pct"]   = p.speed_pct;
        e["gcode_start"] = p.gcode_start;
        e["gcode_end"]   = p.gcode_end;
        e["colormix"]    = sandwich_payload_to_json(p.colormix);
        e["pathblend"]   = sandwich_payload_to_json(p.pathblend);
        arr.push_back(std::move(e));
    }
    root["passes"] = std::move(arr);
    return root.dump();
}

SurfacePassStack SurfacePassStack::from_json(const std::string& text)
{
    SurfacePassStack st;
    if (text.empty()) return st;

    nlohmann::json root;
    try { root = nlohmann::json::parse(text); }
    catch (const std::exception& e) {
        NEOTKO_LOG(MULTIPASS, "SANDWICH from_json parse error: " << e.what());
        return st;
    }
    if (!root.is_object() || !root.contains("passes") || !root["passes"].is_array())
        return st;

    if (root.contains("enabled") && root["enabled"].is_boolean())
        st.enabled = root["enabled"].get<bool>();
    if (root.contains("perimeter_override") && root["perimeter_override"].is_boolean())
        st.perimeter_override = root["perimeter_override"].get<bool>();

    for (const auto& e : root["passes"]) {
        if (!e.is_object()) continue;
        if (static_cast<int>(st.passes.size()) >= kMaxPasses) break; // slots cap
        SurfacePass p;
        if (e.contains("kind") && e["kind"].is_number_integer()) {
            const int k = e["kind"].get<int>();
            if (k >= 0 && k <= 3) p.kind = static_cast<SurfacePassKind>(k);
        }
        if (e.contains("ratio")       && e["ratio"].is_number())             p.ratio      = e["ratio"].get<double>();
        if (e.contains("solid_tool")  && e["solid_tool"].is_number_integer()) p.solid_tool = e["solid_tool"].get<int>();
        if (e.contains("angle")       && e["angle"].is_number_integer())     p.angle      = e["angle"].get<int>();
        if (e.contains("fan")         && e["fan"].is_number_integer())       p.fan        = e["fan"].get<int>();
        if (e.contains("speed_pct")   && e["speed_pct"].is_number_integer()) p.speed_pct  = e["speed_pct"].get<int>();
        if (e.contains("gcode_start") && e["gcode_start"].is_string())       p.gcode_start = e["gcode_start"].get<std::string>();
        if (e.contains("gcode_end")   && e["gcode_end"].is_string())         p.gcode_end   = e["gcode_end"].get<std::string>();
        if (e.contains("colormix"))   p.colormix  = sandwich_payload_from_json(e["colormix"]);
        if (e.contains("pathblend"))  p.pathblend = sandwich_payload_from_json(e["pathblend"]);
        st.passes.push_back(std::move(p));
    }
    return st;
}

SurfacePassStack SurfacePassStack::synthesize_from_legacy(const PrintRegionConfig& cfg,
                                                          ExtrusionRole role)
{
    SurfacePassStack st;
    const bool is_penu = (role == erPenultimateInfill);

    // Legacy effects are mutually exclusive per surface in practice; priority
    // MultiPass > ColorMix > PathBlend mirrors the FASE 2 dispatch order.
    const bool mp_on = is_penu ? cfg.penultimate_multipass_enabled.value
                               : cfg.multipass_enabled.value;
    if (mp_on) {
        const MultiPassConfig mp = MultiPassConfig::from_region_config(cfg, role);
        if (SurfaceColorMix::should_process_role(role, mp.surface)) {
            // Keep ALL n passes including tool<0 (disabled pass): the FASE 2
            // loop's `if (tool<0) continue` still skips them, but their ratio
            // stays in the cumulative Z sum — byte-equivalent to classic MultiPass.
            const int n = std::max(1, std::min(SurfacePassStack::kMaxPasses, mp.num_passes));
            for (int i = 0; i < n; ++i) {
                SurfacePass p;
                p.kind        = SurfacePassKind::Solid;
                p.ratio       = mp.width_ratio[i];
                p.solid_tool  = mp.tool[i];
                p.angle       = mp.angle[i];
                p.fan         = mp.fan[i];
                p.speed_pct   = mp.speed_pct[i];
                p.gcode_start = mp.gcode_start[i];
                p.gcode_end   = mp.gcode_end[i];
                st.passes.push_back(std::move(p));
            }
            st.enabled            = true;
            st.perimeter_override = cfg.multipass_perimeter_override.value;
            return st;
        }
    }

    // ColorMix legacy → single full-height ColorMix pass. Empty kv → the engine
    // reads the region preset config directly (legacy assign_and_group_tools).
    if (cfg.interlayer_colormix_enabled.value) {
        const int surf = cfg.interlayer_colormix_surface.value; // 0 both,1 top,2 penu
        const bool want = (surf == 0) || (is_penu ? surf == 2 : surf == 1);
        if (want) {
            SurfacePass p;
            p.kind  = SurfacePassKind::ColorMix;
            p.ratio = 1.0;
            p.colormix.present = true;     // kv empty → preset-config fallback
            st.passes.push_back(std::move(p));
            st.enabled = true;
            return st;
        }
    }

    // PathBlend legacy → single full-height PathBlend pass.
    // from_region_config(cfg, role) ABSORBS the s69 pathblend_top/penu miniblob:
    // a non-empty per-zone JSON blob overrides the flat pathblend_* keys.
    if (cfg.multipass_path_gradient.value) {
        const int surf = cfg.pathblend_surface.value;
        const bool want = (surf == 0) || (is_penu ? surf == 2 : surf == 1);
        if (want) {
            const PathBlendPassConfig pb =
                PathBlendPassConfig::from_region_config(cfg, role);
            SurfacePass p;
            p.kind  = SurfacePassKind::PathBlend;
            p.ratio = 1.0;
            p.pathblend.present   = true;
            p.pathblend.kv["blob"] = pb.to_blob_json(); // s69 miniblob schema
            st.passes.push_back(std::move(p));
            st.enabled = true;
            return st;
        }
    }

    return st; // disabled, empty
}

SurfacePassStack SurfacePassStack::resolve(const PrintRegionConfig& cfg,
                                           ExtrusionRole role)
{
    const std::string& blob = (role == erPenultimateInfill)
        ? cfg.neotko_surface_passes_penu.value
        : cfg.neotko_surface_passes_top.value;
    SurfacePassStack st = SurfacePassStack::from_json(blob);
    if (st.passes.empty())
        st = SurfacePassStack::synthesize_from_legacy(cfg, role);
    return st;
}

// NEOTKO_SANDWICH_TAG — Fase 3 UX: DynamicPrintConfig overload of resolve().
// The SandwichDialog edits a DynamicPrintConfig; synthesize_from_legacy() needs
// a typed PrintRegionConfig. Copy the overlapping keys across and delegate —
// no engine-logic change, purely additive.
SurfacePassStack SurfacePassStack::resolve_for_zone(const DynamicPrintConfig& cfg,
                                                    bool penu)
{
    PrintRegionConfig rc;
    rc.apply(cfg, true /* ignore keys absent from PrintRegionConfig */);
    return SurfacePassStack::resolve(rc, penu ? erPenultimateInfill
                                              : erTopSolidInfill);
}

MultiPassConfig SurfacePassStack::to_multipass_config(ExtrusionRole role) const
{
    MultiPassConfig c;
    c.enabled      = enabled && !passes.empty();
    c.surface      = (role == erPenultimateInfill) ? 2 : 0;
    c.num_passes   = std::min<int>(SurfacePassStack::kMaxPasses,
                                   static_cast<int>(passes.size()));
    c.vary_pattern = false;
    for (int i = 0; i < c.num_passes; ++i) {
        const SurfacePass& p = passes[i];
        // Non-Solid passes encode tool = -1 → the FASE 2 loop skips them.
        c.tool[i]        = (p.kind == SurfacePassKind::Solid) ? p.solid_tool : -1;
        c.width_ratio[i] = p.ratio;
        c.angle[i]       = p.angle;
        c.fan[i]         = p.fan;
        c.speed_pct[i]   = p.speed_pct;
        c.gcode_start[i] = p.gcode_start;
        c.gcode_end[i]   = p.gcode_end;
    }
    return c;
}

SurfacePassStack SurfacePassStack::from_multipass_config(const MultiPassConfig& mp)
{
    SurfacePassStack st;
    st.enabled = mp.enabled;
    const int n = std::max(1, std::min(SurfacePassStack::kMaxPasses, mp.num_passes));
    for (int i = 0; i < n; ++i) {
        SurfacePass p;
        p.kind        = SurfacePassKind::Solid;
        p.ratio       = mp.width_ratio[i];
        p.solid_tool  = mp.tool[i];
        p.angle       = mp.angle[i];
        p.fan         = mp.fan[i];
        p.speed_pct   = mp.speed_pct[i];
        p.gcode_start = mp.gcode_start[i];
        p.gcode_end   = mp.gcode_end[i];
        st.passes.push_back(std::move(p));
    }
    return st;
}

// Recursive walk: clone each leaf entity, decode its mm3 tool, route to a bucket.
static void sandwich_bucket_visit(
    const ExtrusionEntity* e, int default_tool,
    std::vector<std::pair<int, ExtrusionEntityCollection>>& buckets)
{
    if (!e) return;
    if (const auto* coll = dynamic_cast<const ExtrusionEntityCollection*>(e)) {
        for (const ExtrusionEntity* c : coll->entities)
            sandwich_bucket_visit(c, default_tool, buckets);
        return;
    }
    ExtrusionEntity* cl = e->clone();
    int tool = default_tool;
    if (auto* p = dynamic_cast<ExtrusionPath*>(cl)) {
        if (p->mm3_per_mm >= 10.0) {
            const int t = static_cast<int>(std::floor(p->mm3_per_mm / 10.0)) - 1;
            if (t >= 0) {
                tool = t;
                p->mm3_per_mm -= static_cast<double>(t + 1) * 10.0;
            }
        }
    }
    ExtrusionEntityCollection* dst = nullptr;
    for (auto& b : buckets)
        if (b.first == tool) { dst = &b.second; break; }
    if (!dst) {
        buckets.emplace_back(tool, ExtrusionEntityCollection());
        dst = &buckets.back().second;
    }
    dst->entities.push_back(cl);
}

std::vector<std::pair<int, ExtrusionEntityCollection>>
SurfaceColorMix::eec_to_tool_buckets(const ExtrusionEntityCollection& encoded,
                                     int default_tool)
{
    std::vector<std::pair<int, ExtrusionEntityCollection>> buckets;
    for (const ExtrusionEntity* e : encoded.entities)
        sandwich_bucket_visit(e, default_tool, buckets);
    return buckets;
}
// NEOTKO_SANDWICH_TAG_END

// NEOTKO_MULTIPASS_TAG_START — PathBlend engine implementation
bool PathBlendEngine::needs_blend(const ExtrusionPath& path,
                                   const PrintRegionConfig& cfg)
{
    // PathBlend is now independent of MultiPass — only check its own enable flag.
    if (!cfg.multipass_path_gradient.value)
        return false;
    // Surface filter from pathblend_surface (0=both, 1=top only, 2=penultimate only).
    const int  surface   = cfg.pathblend_surface.value;
    const bool want_top  = (surface == 0 || surface == 1);
    const bool want_penu = (surface == 0 || surface == 2);
    if (want_top  && path.role() == erTopSolidInfill)    return true;
    if (want_penu && path.role() == erPenultimateInfill) return true;
    return false;
}

std::string PathBlendEngine::apply_path(
    const ExtrusionPath&                      path,
    const PrintRegionConfig&                  cfg,
    ExtrusionRole                             role,
    GCodeWriter&                              writer,
    double                                    nominal_z,
    double                                    layer_height,
    double                                    F,
    double                                    e_per_mm,
    int                                       pass_idx,
    double                                    surface_t,
    const std::function<Vec2d(const Point&)>& point_to_gcode,
    std::map<int, double>*                    max_z_per_pass)
{
    // NEOTKO_SANDWICH_TAG — Fase 5 (s72): geometry-driven PathBlend.
    //
    // Gradient model:
    //   - surface_t [0..1]: position of this path within the surface (0=t-low, 1=t-high).
    //   - Pass 0 (RAMPA, tool_bottom): diagonal Z from `floor` (t=0) to `mid_end` (t=1).
    //     Flow at each path is the ramp's local thickness fraction of the layer
    //     height — `(floor_mm + t*(mid_end_mm - floor_mm)) / H`.
    //   - Pass 1 (TAPA, tool_top, Full only): flat at nominal_z. Flow = complement of
    //     ramp = `1 - ramp_thickness / H` so total deposited volume ≈ 1.0×.
    //   - Half mode (no cap): pass_idx >= 1 paths never reach here (sync_legacy_view
    //     sets num_passes=1 so the FASE 2 clone loop skips pi >= 1). Total flow < 1.0
    //     is intentional — the authorised semi-fill exception.
    //   - PathBlend lives alone in the Sandwich (Fase 5 UX) → no MultiPass coexistence.
    //     The old "_mp_on" branch is unreachable here; removed.

    const PathBlendPassConfig pb = PathBlendPassConfig::from_region_config(cfg, role);
    // NEOTKO_PATHBLEND_TAG — Fase 5 s77 migración: delegate to the pb-overload so
    // the geometry lives in a single place. The MultiPass-sublayer dispatch calls
    // the overload directly with a pb decoded from the sublayer's stored blob.
    return apply_path(path, pb, role, writer, nominal_z, layer_height,
                      F, e_per_mm, pass_idx, surface_t, point_to_gcode, max_z_per_pass);
}

std::string PathBlendEngine::apply_path(
    const ExtrusionPath&                      path,
    const PathBlendPassConfig&                pb,
    ExtrusionRole                             role,
    GCodeWriter&                              writer,
    double                                    nominal_z,
    double                                    layer_height,
    double                                    F,
    double                                    e_per_mm,
    int                                       pass_idx,
    double                                    surface_t,
    const std::function<Vec2d(const Point&)>& point_to_gcode,
    std::map<int, double>*                    max_z_per_pass)
{
    if (pass_idx < 0 || pass_idx >= pb.num_passes) return "";

    const auto& pts = path.polyline.points;
    if (pts.size() < 2) return "";

    const double bottom_z = nominal_z - layer_height;
    const double H        = layer_height;

    // Apply easing to surface_t before deriving geometry. Easing shapes how the
    // ramp's Z and the flow split distribute across the surface; pass 0 and pass 1
    // share the same t so the cap's complement stays exact.
    double t = std::clamp(surface_t, 0.0, 1.0);
    switch (pb.ease_mode) {
        case 1: t = t * t;                    break;  // EaseIn
        case 2: t = 1.0 - (1.0 - t) * (1.0 - t); break; // EaseOut
        case 3: t = t * t * (3.0 - 2.0 * t); break;  // EaseInOut (smoothstep)
        default: break;                                 // Linear
    }

    // Ramp Z geometry (relative to bottom_z): floor at t=0, mid_end at t=1.
    // The dialog enforces floor >= 0.01 and (Full only) mid_end <= H - 0.04, but
    // a stale or hand-edited preset could violate the invariants — clamp here
    // so the engine never reads invalid geometry.
    const float floor_mm   = std::max(0.01f, pb.floor_mm);
    const float mid_end_mm = std::max(floor_mm,
        (pb.mode == PathBlendPassConfig::Mode::Full)
            ? std::min(pb.mid_end_mm, static_cast<float>(H - 0.04))
            : std::min(pb.mid_end_mm, static_cast<float>(H)));

    const double ramp_thickness = double(floor_mm) + t * double(mid_end_mm - floor_mm);
    // Clamp the ramp's local thickness to [0.01, H] so cap_flow stays non-negative
    // even when a preset asked for a ramp larger than the current layer.
    const double ramp_thickness_clamped = std::clamp(ramp_thickness, 0.01, H);

    double z_pass;
    double flow;

    if (pass_idx == 0) {
        // RAMPA: nozzle Z varies with t. The bead's top surface IS the ramp.
        z_pass = bottom_z + ramp_thickness_clamped;
        flow   = ramp_thickness_clamped / H;
    } else {
        // TAPA (pass_idx == 1, Full only): flat at nominal_z. Flow = 1 - ramp.
        z_pass = nominal_z;
        flow   = std::max(0.0, 1.0 - (ramp_thickness_clamped / H));
    }

    // NEOTKO_PATHBLEND_TAG — internal `min_flow` safety floor (engine logic,
    // NOT a UX control). Mirrors MultiPass MinLayer Rule 1: a bead thinner than
    // 0.04 mm is at the edge of what a 0.4 mm nozzle can extrude reliably.
    // Without this floor, the cap at t=0 (ramp at floor=0.01, cap thickness =
    // 0.19/0.20 ≈ 0.95) and similar edges would still extrude fine, but the
    // floor protects against degenerate edge cases (e.g. Full preset with
    // mid_end pushed to H − ε by float drift → cap_thickness near zero).
    const double min_flow_04mm = std::max(0.05, 0.04 / H);
    if (pass_idx == 1) {
        // Only floor the cap — the ramp is allowed to be thinner (the authorised
        // semi-fill in Half, the 0.01 mm minimum in Full).
        flow = std::max(flow, min_flow_04mm);
    }
    if (flow < 1e-9) return "";  // last-resort safety

    // NEOTKO_PATHBLEND_TAG — s58 Bug 2 SAFETY: enforce monotonic Z ascent per pass.
    // If max_z_per_pass is provided, never let z_pass drop below the maximum z
    // already reached for this pass_idx within the current layer.
    if (max_z_per_pass != nullptr) {
        auto it_mz = max_z_per_pass->find(pass_idx);
        if (it_mz != max_z_per_pass->end() && it_mz->second > z_pass + 1e-5) {
            NEOTKO_LOG(MULTIPASS, "PATHBLEND_SAFETY z-clamp pass=" << pass_idx
                << " requested=" << z_pass
                << " max_reached=" << it_mz->second
                << " (preventing dangerous descent)");
            z_pass = it_mz->second;
        }
        (*max_z_per_pass)[pass_idx] = std::max(
            (it_mz != max_z_per_pass->end() ? it_mz->second : z_pass), z_pass);
    }

    std::string gcode;

    // Step Z to this pass's level using print speed (smooth, avoids Z-axis ringing).
    if (std::abs(writer.get_position().z() - z_pass) > 1e-5) {
        GCodeG1Formatter w;
        w.emit_z(z_pass);
        w.emit_f(F);
        w.emit_comment(GCodeWriter::full_gcode_comment,
                       "pb" + std::to_string(pass_idx) + " step");
        gcode += w.string();
        writer.get_position().z() = z_pass;
    }

    // Extrude all segments at constant z_pass and constant flow.
    for (size_t i = 1; i < pts.size(); ++i) {
        const double seg_l = (pts[i] - pts[i-1]).cast<double>().norm() * SCALING_FACTOR;
        if (seg_l < 1e-6) continue;
        gcode += writer.extrude_to_xy(
            point_to_gcode(pts[i]),
            e_per_mm * seg_l * flow,
            "pb" + std::to_string(pass_idx));
    }

    NEOTKO_LOG(MULTIPASS,
        "PathBlend"
        << " layer="     << (int)(nominal_z * 1000) << "um"
        << " mode="      << (pb.mode == PathBlendPassConfig::Mode::Full ? "full" : "half")
        << " role="      << (role == erPenultimateInfill ? "penu" : "top")
        << " pass="      << pass_idx << "/" << pb.num_passes
        << " t_raw="     << surface_t
        << " ease_t="    << t
        << " floor_mm="  << floor_mm
        << " mid_end_mm=" << mid_end_mm
        << " z_pass="    << z_pass
        << " flow="      << flow
        << " pts="       << pts.size());

    return gcode;
}
// NEOTKO_MULTIPASS_TAG_END

} // namespace Slic3r
