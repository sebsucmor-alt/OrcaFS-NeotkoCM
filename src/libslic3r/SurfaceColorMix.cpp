// NEOTKO_COLORMIX_TAG_START
// Neotko Surface ColorMix + MultiPass Blend — Implementation
// Multi-tool distribution for top/penultimate surface layers
// Author: Neotko
// NEOTKO_COLORMIX_TAG_END

#include "SurfaceColorMix.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "PrintConfig.hpp"
#include "GCodeWriter.hpp"  // NEOTKO_NEOWEAVING_TAG — must be outside namespace Slic3r
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <limits>
#include <set>      // NEOTKO_COLORMIX: unique-tool check in build_tool_list_from_pattern
#include <mutex>    // NEOTKO_DEBUG: NeoDebug::write thread safety

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
            // Physical tool: 1-based digit → 0-based index.
            int idx = static_cast<int>(c - '1');
            if (std::find(tools.begin(), tools.end(), idx) == tools.end())
                tools.push_back(idx);
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
int SurfaceColorMix::assign_and_group_tools(
    ExtrusionEntityCollection& fills,
    const PrintRegionConfig& config,
    ExtrusionRole /*role — detected internally per path*/,
    int layer_idx,
    bool allow_top,
    bool allow_penu,
    const MixedFilamentManager* mgr,
    size_t num_physical
) {
    NEOTKO_LOG(COLORMIX, "ENTRY layer=" << layer_idx
        << " enabled=" << config.interlayer_colormix_enabled.value
        << " fills_size=" << fills.entities.size());

    if (!config.interlayer_colormix_enabled.value)
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

        // Surface filter
        if (!should_process_role(first_path->role(), surface)) continue;

        // Zone filter (allow_top / allow_penu from call site — interlayer_colormix_*_zone)
        if (!allow_top  && first_path->role() == erTopSolidInfill)    continue;
        if (!allow_penu && first_path->role() == erPenultimateInfill)  continue;

        // Resolve tool list from role-specific pattern (fallback to legacy slots if invalid)
        const std::string& pattern_str = (first_path->role() == erTopSolidInfill)
            ? config.interlayer_colormix_pattern_top.value
            : config.interlayer_colormix_pattern_penultimate.value;
        const std::vector<int> tools = build_tool_list_from_pattern(pattern_str, config, mgr, num_physical);
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
        // Distribute lines cyclically by pattern slot, then group by UNIQUE tool_id.
        // unique_tool_order tracks first-occurrence so first tool in pattern prints first.
        const int n_slots = static_cast<int>(tools.size());
        std::vector<int> unique_tool_order;
        std::map<int, std::vector<ExtrusionPath*>> tool_blocks;

        for (int path_idx = 0; path_idx < (int)raw_lines.size(); ++path_idx) {
            int slot     = path_idx % n_slots;
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
        c.vary_pattern   = false;
        c.angle[0]       = cfg.penultimate_multipass_angle_1.value;
        c.angle[1]       = cfg.penultimate_multipass_angle_2.value;
        c.angle[2]       = cfg.penultimate_multipass_angle_3.value;
        c.fan[0]         = -1; c.fan[1] = -1; c.fan[2] = -1;
        c.speed_pct[0]   = 100; c.speed_pct[1] = 100; c.speed_pct[2] = 100;
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

// ---------------------------------------------------------------------------
// SurfaceMultiPass::apply
//
// CAMINO 1c (current): perpendicular-offset tiling.
//   Each pass gets a narrower line width (ratio × LWTS) and its polyline
//   points are shifted perpendicularly so passes tile the surface without gaps.
//   offset_i = (Σ_{j<i} ratio_j + ratio_i/2 - Σ_all/2) × LWTS
//   For Σ ratios = 1.0 → adjacent strips exactly fill the original LWTS width.
//
// CAMINO 2 (future combination with ColorMix):
//   Clone paths N times WITHOUT encoding tool (only apply width_ratio).
//   Store clones in separate sub-collections. Let ColorMix iterate each sub-collection.
// ---------------------------------------------------------------------------
bool SurfaceMultiPass::apply(
    ExtrusionEntityCollection& fills,
    const PrintRegionConfig&   config,
    int                        layer_idx,
    bool                       allow_top,
    bool                       allow_penu)
{
    const auto mp = MultiPassConfig::from_region_config(config);
    if (!mp.enabled) return false;

    const int n = std::max(1, std::min(3, mp.num_passes));
    bool any_modified = false;

    if (NeoDebug::enabled(NeoDebug::MULTIPASS)) {
        std::ostringstream _s;
        _s << "=== MULTIPASS Layer " << layer_idx
           << " passes=" << n << " surface=" << mp.surface
           << " vary=" << mp.vary_pattern << " fills=" << fills.entities.size();
        for (int i = 0; i < n; ++i) {
            if (mp.tool[i] < 0) { _s << "\n  pass" << i << ": DISABLED"; continue; }
            _s << "\n  pass" << i << ": T" << mp.tool[i]
               << " ratio=" << mp.width_ratio[i]
               << (mp.vary_pattern && i%2==1 ? " REVERSED" : "");
        }
        NeoDebug::write(NeoDebug::MULTIPASS, _s.str());
    }

    for (auto* top_entity : fills.entities) {
        auto* sub = dynamic_cast<ExtrusionEntityCollection*>(top_entity);
        if (!sub || sub->entities.empty()) continue;

        ExtrusionPath* flat_path = nullptr;
        for (auto* e : sub->entities) {
            flat_path = dynamic_cast<ExtrusionPath*>(e);
            if (flat_path) break;
        }
        if (!flat_path) continue;

        // Surface filter
        if (!SurfaceColorMix::should_process_role(flat_path->role(), mp.surface)) continue;

        // Zone filter (allow_top / allow_penu from call site — interlayer_colormix_*_zone)
        if (!allow_top  && flat_path->role() == erTopSolidInfill)   continue;
        if (!allow_penu && flat_path->role() == erPenultimateInfill) continue;

        // FASE 2 guard: skip if paths were already generated + encoded inline in Fill.cpp.
        {
            bool already_encoded = false;
            for (auto* e : sub->entities)
                if (auto* p = dynamic_cast<ExtrusionPath*>(e))
                    if (p->mm3_per_mm >= 10.0) { already_encoded = true; break; }
            if (already_encoded) continue;
        }

        std::vector<ExtrusionPath*> originals;
        for (auto* e : sub->entities) {
            if (auto* p = dynamic_cast<ExtrusionPath*>(e))
                originals.push_back(p);
        }
        if (originals.empty()) continue;

        NEOTKO_LOG(MULTIPASS, "  SUB role=" << ExtrusionEntity::role_to_string(flat_path->role())
            << " originals=" << originals.size() << " mm3=" << flat_path->mm3_per_mm);

        // NEOTKO_MULTIPASS_TAG_START — Beer-Lambert height-based sub-layer model
        //
        // Each pass occupies a proportional fraction of the layer height (width_ratio[i]).
        // Passes are stacked vertically (same XY polyline, different sub-layer thickness)
        // rather than tiled side-by-side with XY perpendicular offsets.
        //
        // For width_ratio = {0.6, 0.4} and layer_height = 0.2 mm, W = 0.4 mm:
        //   Pass 0 (T1): H_sub=0.12 → mm3 = 0.12×(0.4−0.2146×0.12) ≈ stadium(0.4,0.12)
        //   Pass 1 (T2): H_sub=0.08 → mm3 = 0.08×(0.4−0.2146×0.08) ≈ stadium(0.4,0.08)
        //   Total: Σ A_sub_i ≈ A_orig + ε  (ε = stadium non-linearity overhead, ~6-8%)
        //   Each pass extrudes exactly what an independent thin layer of that height would.
        //
        // width is NEVER scaled — each pass covers the full line width so spacing and
        // collision detection in GCode.cpp remain correct.
        //
        // Z positioning within the layer (Beer-Lambert FASE 2):
        //   Pass 0 z = bottom_z + ratio[0] * layer_height
        //   Pass 1 z = nominal_z (top of layer)
        //   Implemented via PathBlend's apply_path() when multipass_path_gradient is ON.
        //   Without PathBlend, all passes print at nominal_z (stacked extrusion, correct
        //   total volume, visual mixing depends on filament diffusion).

        std::vector<ExtrusionPath*> all_pass_paths;
        all_pass_paths.reserve(originals.size() * n);

        for (int i = 0; i < n; ++i) {
            if (mp.tool[i] < 0) continue;
            const bool reverse = mp.vary_pattern && (i % 2 == 1);
            const double ratio = mp.width_ratio[i];

            for (auto* orig : originals) {
                auto* clone = new ExtrusionPath(
                    orig->role(),
                    orig->mm3_per_mm,
                    orig->width,
                    orig->height
                );
                clone->polyline = orig->polyline;

                if (reverse)
                    std::reverse(clone->polyline.points.begin(),
                                 clone->polyline.points.end());

                // NEOTKO_MULTIPASS_TAG_START
                // Each MultiPass sub-layer is a genuine independent thin layer at its own Z.
                // Conceptually equivalent to Variable Layer Height: slice the full layer into
                // N physical laminae of heights H×ratio[i] and stack them back.
                //
                // Flow::mm3_per_mm() uses the FDM stadium cross-section model:
                //   A(W, H) = H × (W − H × (1 − π/4))      [= H×(W−H×0.2146)]
                // This is NON-LINEAR in H: A_sub ≠ A_orig × ratio.
                //
                // WRONG (old code): clone->mm3_per_mm *= ratio
                //   Assumes rectangular cross-section (A ∝ H). Under-extrudes every pass:
                //   thin passes by ~10%, thick passes by ~5%.
                //
                // CORRECT: compute A_sub directly from H_sub using the same formula.
                //   Width stays fixed — the slicer's XY polylines are reused as-is.
                //
                // Note: Σ A_sub_i is slightly greater than A_orig (the rounded bead ends
                // are paid N times instead of 1). This is the physically correct behavior
                // when printing N independent thin laminae — identical to what Variable
                // Layer Height would produce if each slice were a real slicer layer.
                {
                    const double W     = static_cast<double>(orig->width);
                    const double H     = static_cast<double>(orig->height);
                    const double H_sub = H * ratio;
                    // k = 1 − π/4 ≈ 0.2146.  Stadium area: A = H × (W − k×H)
                    // Guard: requires W > H (normal FDM bead). If violated (bridge / bad
                    // config), fall back to naive linear scaling so we never go negative.
                    constexpr double k = 1.0 - 0.25 * M_PI;   // matches Flow.cpp exactly
                    const double A_orig = H     * (W - k * H);
                    const double A_sub  = H_sub * (W - k * H_sub);
                    if (A_orig > 1e-9 && W > H + 1e-6) {
                        clone->mm3_per_mm = static_cast<float>(
                            static_cast<double>(orig->mm3_per_mm) * (A_sub / A_orig));
                    } else {
                        // Degenerate (W ≤ H, bridge, or near-zero area): linear fallback.
                        clone->mm3_per_mm = static_cast<float>(
                            static_cast<double>(orig->mm3_per_mm) * ratio);
                    }
                    clone->height = static_cast<float>(H_sub);
                }
                // Width is intentionally NOT modified — XY polylines reused verbatim.
                // NEOTKO_MULTIPASS_TAG_END

                SurfaceColorMix::encode_tool_in_path(clone, mp.tool[i]);

                all_pass_paths.push_back(clone);
            }
        }
        // NEOTKO_MULTIPASS_TAG_END

        if (all_pass_paths.empty()) continue;

        for (auto* e : sub->entities) delete e;
        sub->entities.clear();
        sub->no_sort = true;
        for (auto* p : all_pass_paths)
            sub->entities.push_back(p);

        NEOTKO_LOG(MULTIPASS, "  HOOK FIRED: entities=" << sub->entities.size() << " no_sort=1");
        any_modified = true;
    }

    if (any_modified)
        NEOTKO_LOG(MULTIPASS, "");

    return any_modified;
}

// NEOTKO_MULTIPASS_TAG_END

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

double PathBlendPassConfig::ratio_at(int p, double t) const
{
    if (p < 0 || p >= num_passes) return 0.0;

    // Apply easing curve to t before any ratio calculation.
    // Pass 1 is always the exact complement of pass 0, so easing is mirrored automatically.
    // ease_mode: 0=Linear (no-op), 1=EaseIn (t²), 2=EaseOut (1-(1-t)²), 3=EaseInOut (smoothstep).
    switch (ease_mode) {
        case 1: t = t * t;                    break;  // Ease In
        case 2: t = 1.0 - (1.0-t)*(1.0-t);   break;  // Ease Out
        case 3: t = t * t * (3.0 - 2.0 * t); break;  // Ease In/Out (smoothstep)
        default: break;                                // Linear: t unchanged
    }

    const double mn = static_cast<double>(min_ratio);
    const double mx = static_cast<double>(max_ratio);  // cap for dominant pass at peak
    double r = 0.0;

    if (num_passes == 1) {
        // Single-pass: T2 fades IN from min_ratio (t=0, base shows through) to max_ratio (t=1).
        // Combined with Z staircase → visual blend from base color into T2 across the surface.
        r = mn + (mx - mn) * t;
    } else if (num_passes == 2) {
        // Pass 0 (T0, dominant at t=0): clamped to [min_ratio, max_ratio].
        // Pass 1 (T1): exact complement of actual pass 0 → flow_0 + flow_1 == 1.0 always.
        //
        // NEOTKO_FIX: pass 1 must mirror the *real* pass 0 output (after clamp),
        // not raw (1-t), to preserve the sum==1.0 invariant when min/max are active.
        if (p == 0) {
            r = std::clamp(1.0 - t, mn, mx);
        } else {
            r = 1.0 - std::clamp(1.0 - t, mn, mx);
        }
        return r;  // early-return: clamp already applied, don't double-apply below
    } else if (num_passes == 3) {
        if      (p == 0) r = std::max(0.0, 1.0 - 2.0 * t);
        else if (p == 1) r = 1.0 - std::abs(2.0 * t - 1.0);
        else             r = std::max(0.0, 2.0 * t - 1.0);
    } else { // 4 passes — linear-hat, peaks at t = 0, 0.333, 0.667, 1.0
        const double step   = 1.0 / 3.0;
        const double center = p * step;
        r = std::max(0.0, 1.0 - std::abs(t - center) / step);
    }
    // For 3/4-pass: floor pass 0 at min_ratio, cap last pass at max_ratio.
    // Not applied to 2-pass (handled above with early return).
    if (p == 0)              r = std::max(mn, r);
    if (p == num_passes - 1) r = std::min(mx, r);
    return r;
}

PathBlendPassConfig PathBlendPassConfig::from_region_config(const PrintRegionConfig& cfg)
{
    PathBlendPassConfig c;
    c.enabled        = cfg.multipass_path_gradient.value; // existing enable key; PathBlend now independent of multipass_enabled
    c.surface        = cfg.pathblend_surface.value;
    c.num_passes     = std::clamp(cfg.pathblend_num_passes.value, 1, 4);
    c.tool[0]        = cfg.pathblend_tool_1.value;
    c.tool[1]        = cfg.pathblend_tool_2.value;
    c.tool[2]        = cfg.pathblend_tool_3.value;
    c.tool[3]        = cfg.pathblend_tool_4.value;
    c.layer_ratio[0] = static_cast<float>(cfg.pathblend_layer_ratio_1.value);
    c.layer_ratio[1] = static_cast<float>(cfg.pathblend_layer_ratio_2.value);
    c.layer_ratio[2] = static_cast<float>(cfg.pathblend_layer_ratio_3.value);
    c.layer_ratio[3] = static_cast<float>(cfg.pathblend_layer_ratio_4.value);
    c.min_ratio        = static_cast<float>(
        std::clamp(cfg.pathblend_min_ratio.value, 0.01, 0.49));
    c.max_ratio        = static_cast<float>(
        std::clamp(cfg.pathblend_max_ratio.value, 0.51, 1.0));
    c.ease_mode        = std::clamp(cfg.pathblend_ease_mode.value, 0, 3);
    c.invert_gradient  = cfg.pathblend_invert_gradient.value;
    c.fill_angle       = cfg.pathblend_fill_angle.value;
    return c;
}
// NEOTKO_PATHBLEND_TAG_END

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
    GCodeWriter&                              writer,
    double                                    nominal_z,
    double                                    layer_height,
    double                                    F,
    double                                    e_per_mm,
    int                                       pass_idx,
    double                                    surface_t,
    const std::function<Vec2d(const Point&)>& point_to_gcode)
{
    // Gradient model (restored from path_blend_fixed.cpp):
    //   - surface_t [0..1]: position of this path within the surface (0=first, 1=last).
    //   - Pass 0 (T0): steps Z DOWN to bottom_z + surface_t * layer_height so each
    //     successive path sits slightly higher — this is the diagonal staircase that
    //     reads as a smooth gradient with 50+ paths.  Flow = ratio_at(0, surface_t).
    //   - Pass N-1 (TN): stays at nominal_z (pass never changes Z). Flow = ratio_at(N-1, t).
    //   - Intermediate passes (3/4-pass mode): Z evenly spaced between bottom and top,
    //     but anchored to surface_t so the "peak" of each pass tracks across the surface.
    //   - The Z for pass 0 is VARIABLE per path (surface_t-driven), NOT fixed per pass.
    //     The old "fixed Z per pass" formula produced a flat band per tool instead of a
    //     diagonal gradient — that was the visible bug.

    const PathBlendPassConfig pb = PathBlendPassConfig::from_region_config(cfg);

    if (pass_idx < 0 || pass_idx >= pb.num_passes) return "";

    const auto& pts = path.polyline.points;
    if (pts.size() < 2) return "";

    const double bottom_z = nominal_z - layer_height;

    // NEOTKO_FIX: Two separate behaviours depending on whether MultiPass is active.
    //
    // ── MULTIPASS MODE ──────────────────────────────────────────────────────────
    // Each pass clone was already created in SurfaceMultiPass::apply() with:
    //   clone->height     = orig->height * ratio[i]
    //   clone->mm3_per_mm = stadium(W, H*ratio[i]) / stadium(W, H) * orig->mm3_per_mm
    //                       (stadium cross-section correction — NOT naive ×ratio)
    // The extruded volume is correct for that sub-height bead geometry.
    // flow must be 1.0 here — any further scaling causes under/over-extrusion.
    //
    // Z must be the TOP surface of each sub-layer, stacked from bottom_z upward
    // using ACCUMULATED ratios.  The t_eff staircase must NOT be used here:
    //   Pass 0: z = bottom_z + ratio[0] * layer_height
    //   Pass 1: z = bottom_z + (ratio[0]+ratio[1]) * layer_height
    //   Pass k: z = bottom_z + sum(ratio[0..k]) * layer_height
    //   Last:   z = nominal_z  (guard against fp drift)
    //
    // Example — 3 passes at 33% each, layer_height=0.2 mm:
    //   Pass 0 (T0): z = bottom_z + 0.067   ← lowest physical sub-layer
    //   Pass 1 (T1): z = bottom_z + 0.133
    //   Pass 2 (T2): z = nominal_z = bottom_z + 0.200  ← topmost
    //
    // ── STANDALONE PATHBLEND MODE (multipass disabled) ───────────────────────────
    // No prior mm3 scaling.  Use the original t_eff staircase + ratio_at() flow
    // to blend two tools visually across the surface via a diagonal Z sweep.
    double flow;
    double z_pass;

    if (cfg.multipass_enabled.value) {
        // Multipass: volume already scaled, so flow = 1.0 and Z = stacked sub-layer top.
        flow = 1.0;

        const MultiPassConfig mp = MultiPassConfig::from_region_config(cfg);
        const int n_mp = std::max(1, std::min(3, mp.num_passes));
        const int clamped_idx = std::min(pass_idx, n_mp - 1);

        double z_accum = bottom_z;
        for (int i = 0; i <= clamped_idx; ++i)
            z_accum += mp.width_ratio[i] * layer_height;

        // Last pass snaps to nominal_z to avoid floating-point drift.
        z_pass = (clamped_idx >= n_mp - 1) ? nominal_z
                                            : std::clamp(z_accum, bottom_z, nominal_z);
    } else {
        // Standalone PathBlend: surface_t staircase + ratio_at() gradient flow.
        // invert_gradient flips t so the nozzle ascends during pass 0 (collision safety).
        const double t_eff = pb.invert_gradient ? (1.0 - surface_t) : surface_t;
        flow = pb.ratio_at(pass_idx, t_eff);
        if (flow < 1e-9) return "";  // pass contributes nothing at this t — skip

        const double z_bottom_anchor = bottom_z + t_eff * layer_height;
        if (pb.num_passes == 1) {
            z_pass = z_bottom_anchor;
        } else {
            const double frac = static_cast<double>(pass_idx)
                              / static_cast<double>(pb.num_passes - 1);
            z_pass = z_bottom_anchor + frac * (nominal_z - z_bottom_anchor);
        }
        z_pass = std::clamp(z_pass, bottom_z, nominal_z);
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
        << " mode="      << (cfg.multipass_enabled.value ? "multipass" : "standalone")
        << " pass="      << pass_idx << "/" << pb.num_passes
        << " t_raw="     << surface_t
        << " z_pass="    << z_pass
        << " flow="      << flow
        << " pts="       << pts.size());

    return gcode;
}
// NEOTKO_MULTIPASS_TAG_END

} // namespace Slic3r
