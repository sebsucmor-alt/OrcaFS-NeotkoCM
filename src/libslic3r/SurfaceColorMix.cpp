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
// Build tool list from a pattern string (e.g. "12", "1221", "123").
// Digits 1-4 are valid: user 1-based → internal 0-based (digit - '1').
// If the resulting list has fewer than 2 entries, falls back to build_tool_list().
// ---------------------------------------------------------------------------
static std::vector<int> build_tool_list_from_pattern(
    const std::string& pattern,
    const PrintRegionConfig& config)
{
    std::vector<int> tools;
    for (char c : pattern) {
        if (c >= '1' && c <= '4')
            tools.push_back(static_cast<int>(c - '1')); // 1-based → 0-based
    }
    if (tools.size() < 2) {
        NEOTKO_LOG(COLORMIX, "PATTERN_FALLBACK pattern=\"" << pattern
            << "\" (parsed=" << tools.size() << " tools) → using legacy tool_a/b/c/d");
        return build_tool_list(config);
    }
    if (NeoDebug::enabled(NeoDebug::COLORMIX)) {
        std::ostringstream _s;
        _s << "PATTERN_USE pattern=\"" << pattern << "\" → tools=[";
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
    bool allow_penu
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
        const std::vector<int> tools = build_tool_list_from_pattern(pattern_str, config);
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
    int safe_tool = std::max(0, std::min(tool_idx, 3));
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

    if (!mixed_defs.empty()) {
        MixedFilamentManager mgr;
        mgr.load_custom_entries(mixed_defs, filament_colours);

        auto get_color = [&](unsigned int id) -> std::string {
            if (id >= 1 && id <= filament_colours.size())
                return filament_colours[id - 1];
            return "#888888";
        };

        for (const auto& mf : mgr.mixed_filaments()) {
            if (mf.deleted || !mf.enabled) continue;

            ColorMixOption opt;
            opt.is_physical = false;
            opt.pattern     = mixed_filament_to_pattern(mf);
            opt.label       = "Mixed (F" + std::to_string(mf.component_a)
                            + "+F" + std::to_string(mf.component_b) + ")";

            if (!mf.display_color.empty()) {
                opt.display_color = mf.display_color;
            } else {
                opt.display_color = MixedFilamentManager::blend_color(
                    get_color(mf.component_a),
                    get_color(mf.component_b),
                    mf.ratio_a, mf.ratio_b);
            }
            result.push_back(std::move(opt));
        }
    }

    for (int i = 0; i < (int)filament_colours.size(); i++) {
        ColorMixOption opt;
        opt.is_physical   = true;
        opt.label         = "F" + std::to_string(i + 1);
        opt.pattern       = std::to_string(i + 1);
        opt.display_color = filament_colours[i];
        result.push_back(std::move(opt));
    }

    return result;
}
// NEOTKO_COLORMIX_TAG_END

// NEOTKO_MULTIPASS_TAG_START

// ---------------------------------------------------------------------------
// MultiPassConfig::from_region_config
// ---------------------------------------------------------------------------
MultiPassConfig MultiPassConfig::from_region_config(const PrintRegionConfig& cfg)
{
    MultiPassConfig c;
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
        // For width_ratio = {0.6, 0.4} and layer_height = 0.2 mm:
        //   Pass 0 (T1): height = 0.12 mm → mm3_per_mm = orig * 0.6  (physically below T2)
        //   Pass 1 (T2): height = 0.08 mm → mm3_per_mm = orig * 0.4  (on top)
        //   Total: same XY area covered, sum of flows = 1.0, layer remains solid.
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

                // Height-based sub-layer: scale height (Z thickness) by ratio.
                // mm3_per_mm scales proportionally (rectangular cross-section, width fixed).
                // Width is intentionally NOT modified.
                clone->height     = static_cast<float>(static_cast<double>(orig->height) * ratio);
                clone->mm3_per_mm *= ratio;

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
    double r = 0.0;
    if (num_passes == 1) {
        // Single-pass: T2 fades IN from min_ratio (t=0, base shows through) to 1.0 (t=1, full coverage).
        // Combined with Z staircase → visual blend from base color into T2 across the surface.
        r = static_cast<double>(min_ratio) + (1.0 - static_cast<double>(min_ratio)) * t;
    } else if (num_passes == 2) {
        // Pass 0 (T0, dominant at t=0): apply min_ratio floor so it never
        // under-extrudes on very thin paths at the top of the surface.
        // Pass 1 (T1, dominant at t=1): ALWAYS pure geometry (t), independent
        // of min_ratio. Using (1 - flow_0) instead would propagate the min_ratio
        // boost into T1 causing over-extrusion when t < min_ratio. See original.
        if (p == 0) {
            r = std::max(static_cast<double>(min_ratio), 1.0 - t);
        } else {
            r = t;  // pure geometry — never touched by min_ratio
        }
        return r;  // early-return: min_ratio floor already applied above, don't double-apply below
    } else if (num_passes == 3) {
        if      (p == 0) r = std::max(0.0, 1.0 - 2.0 * t);
        else if (p == 1) r = 1.0 - std::abs(2.0 * t - 1.0);
        else             r = std::max(0.0, 2.0 * t - 1.0);
    } else { // 4 passes — linear-hat, peaks at t = 0, 0.333, 0.667, 1.0
        const double step   = 1.0 / 3.0;
        const double center = p * step;
        r = std::max(0.0, 1.0 - std::abs(t - center) / step);
    }
    // min_ratio floor only for pass 0 (the pass dominant at t=0).
    // Not applied to the 2-pass case (handled above with early return).
    if (p == 0) r = std::max(static_cast<double>(min_ratio), r);
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
        std::clamp(cfg.pathblend_min_ratio.value, 0.01, 0.5));
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

    // invert_gradient=true: invert surface_t so that the nozzle ASCENDS during pass 0.
    // When Orca prints high-Y paths first, raw surface_t starts near 1.0 and descends.
    // Without inversion z would descend too (collision risk). With inversion t_eff starts
    // near 0.0 → z starts at bottom_z and ascends → safe.
    const double t_eff = pb.invert_gradient ? (1.0 - surface_t) : surface_t;

    const double flow = pb.ratio_at(pass_idx, t_eff);
    if (flow < 1e-9) return "";  // this pass contributes nothing at this t — skip

    const auto& pts = path.polyline.points;
    if (pts.size() < 2) return "";

    const double bottom_z = nominal_z - layer_height;

    // Z calculation — t_eff-driven staircase for all pass counts.
    //   pass 0   → bottom_z + t_eff * layer_height  (diagonal, direction controlled by invert_gradient)
    //   pass N-1 → nominal_z
    //   passes 1..N-2 → linearly interpolated
    // For num_passes == 1 we use the same diagonal as pass 0 of a 2-pass setup.
    // With invert_gradient=true (default): low t_eff = high raw_t = first printed path → z starts
    // at bottom_z and ascends as printing progresses → safe, no collision.
    double z_pass;
    {
        const double z_bottom_anchor = bottom_z + t_eff * layer_height;
        if (pb.num_passes == 1) {
            z_pass = z_bottom_anchor;
        } else {
            const double frac = static_cast<double>(pass_idx) / static_cast<double>(pb.num_passes - 1);
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
        << " layer="  << (int)(nominal_z * 1000) << "um"
        << " pass="   << pass_idx << "/" << pb.num_passes
        << " t_raw="  << surface_t
        << " t_eff="  << t_eff
        << " z_pass=" << z_pass
        << " flow="   << flow
        << " pts="    << pts.size());

    return gcode;
}
// NEOTKO_MULTIPASS_TAG_END

} // namespace Slic3r
