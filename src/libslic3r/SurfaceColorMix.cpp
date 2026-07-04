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
#include "ClipperUtils.hpp"           // NEOTKO_PROFILE_TAG — Fase 6c: union_ex for footprint mask
#include "SurfaceEffectProfile.hpp"   // NEOTKO_PROFILE_TAG — painted-profile lookup
#include "NSVGUtils.hpp"              // NEOTKO_STICKER_TAG — SVG → ExPolygons (sticker masks)
#include "GCodeWriter.hpp"  // NEOTKO_NEOWEAVING_TAG — must be outside namespace Slic3r
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <limits>
#include <set>      // NEOTKO_COLORMIX: unique-tool check in build_tool_list_from_pattern
#include <mutex>    // NEOTKO_DEBUG: NeoDebug::write thread safety
#include <atomic>   // NEOTKO_DEBUG s79h: session-banner monotonic counter
#include <ctime>    // NEOTKO_DEBUG s79h: localtime + strftime for banner timestamp
#include <sstream>  // NEOTKO_DEBUG s79h: banner formatting
#include <numeric>  // NEOTKO_COLORMIX s58: std::iota for lane_mode sort indices
#include <nlohmann/json.hpp> // NEOTKO_PATHBLEND_TAG s69: miniblob JSON round-trip

namespace Slic3r {

// NEOTKO_DEBUG_TAG_START
// NeoDebug — centralised debug channel implementation.
// One log file per channel, guarded by its env var or ORCA_DEBUG_ALL.
// Thread-safe writes via a single global mutex (debug only, no perf concern).
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
// NEOTKO_COLORSTITCH_TAG — split_monotonic_path_into_runs
// ColorStitch on the continuous Monotonic pattern. FillMonotonic fuses adjacent
// scanlines into ONE ExtrusionPath via perimeter connector arcs (unlike Monotonic
// Line, which keeps lines separate). To colour each visual line independently we
// split a fused path into per-scanline "runs":
//   - run.scan = the colourable scanline (clean geometry → drives lane/slot maths)
//   - run.tail = the connector arc that FOLLOWS that scanline, kept (never dropped)
//                and re-appended at emission so it prints in the OUTGOING colour.
// Classification is by LANE CROSSING, not by angle, against the SURFACE fill axis
// (ax,ay) passed in by the caller — NOT a per-path axis. A per-path axis is unstable:
// on a fragmented penu path (short scanlines + long traversals) the dominant per-path
// direction can flip 90°, turning connectors into "scans" → a long horizontal run that
// then hijacks LaneQuant's global fill_dir (confirmed: penu ref=0° vs surface dom=90°).
// One axis for the whole surface keeps every path consistent. A segment is a scanline if
// it STAYS in its lane (perpendicular displacement < ½ line spacing) and a connector if it
// CROSSES toward the next line (Δ⊥ ≥ ½ spacing) — robust at any connector steepness. This
// is the same metric LaneQuant uses to assign colour, so the split agrees with the colouring.
// A new run begins at each connector→scanline transition, so each run is [scanline][trailing
// arc]. Entirely post-hoc — does NOT touch FillMonotonic/connect_infill.
// ---------------------------------------------------------------------------
struct MonotonicRun { Polyline scan; Polyline tail; };

static std::vector<MonotonicRun> split_monotonic_path_into_runs(const ExtrusionPath& path,
                                                                double ax, double ay)
{
    const Points& pts = path.polyline.points;
    std::vector<MonotonicRun> runs;
    const size_t np = pts.size();
    if (np < 2) return runs;
    const size_t nseg = np - 1;

    // Perpendicular (lane) axis of the SURFACE fill direction.
    const double px = -ay, py = ax;
    // Line spacing in scaled coords ≈ extrusion width (top/penu solid infill is adjacent).
    // Half-spacing is the lane-crossing threshold; guard against a zero/garbage width.
    const double lane_thresh = 0.5 * std::max(1.0, double(path.width) * 1e6);

    // Classify each segment by perpendicular (lane) displacement vs the surface axis:
    //   scanline → stays in its lane (|Δ⊥| < ½ spacing)
    //   connector → crosses toward an adjacent line (|Δ⊥| ≥ ½ spacing), at any steepness.
    std::vector<bool> is_scan(nseg, false);
    for (size_t i = 0; i < nseg; ++i) {
        const double dx = double(pts[i + 1].x() - pts[i].x());
        const double dy = double(pts[i + 1].y() - pts[i].y());
        if (dx * dx + dy * dy < 1e-12) { is_scan[i] = (i > 0) ? is_scan[i - 1] : true; continue; }
        const double dperp = std::abs(dx * px + dy * py);
        is_scan[i] = (dperp < lane_thresh);
    }

    // Run starts: point 0, then every connector→scanline transition once the
    // current run already owns a scanline (so a trailing connector stays attached).
    std::vector<size_t> run_starts;
    run_starts.push_back(0);
    bool run_has_scan = false;
    for (size_t i = 0; i < nseg; ++i) {
        if (i > 0 && is_scan[i] && !is_scan[i - 1] && run_has_scan) {
            run_starts.push_back(i);
            run_has_scan = false;
        }
        if (is_scan[i]) run_has_scan = true;
    }

    // Materialise each run as [scan | tail].
    for (size_t k = 0; k < run_starts.size(); ++k) {
        const size_t s = run_starts[k];
        const size_t e = (k + 1 < run_starts.size()) ? run_starts[k + 1] : (np - 1);
        if (e <= s) continue;
        // Scanline = leading run of scanline segments; tail = the rest (connector).
        size_t scan_split = s;
        while (scan_split < e && is_scan[scan_split]) ++scan_split;
        MonotonicRun r;
        if (scan_split == s) {
            // Defensive (run begins with a connector, e.g. a path that opens on a
            // perimeter hook): keep the whole run as scan so no geometry is lost.
            for (size_t i = s; i <= e; ++i) r.scan.points.push_back(pts[i]);
        } else {
            for (size_t i = s; i <= scan_split; ++i) r.scan.points.push_back(pts[i]);
            if (scan_split < e)
                for (size_t i = scan_split; i <= e; ++i) r.tail.points.push_back(pts[i]);
        }
        if (r.scan.points.size() >= 2) runs.push_back(std::move(r));
    }
    return runs;
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
//   slice_frame = po->trafo_centered() * mv->get_matrix() * mesh_vertex
// before comparing Z and clipping in XY. NEOTKO_COLORSTITCH_TAG s161 — this
// MUST be trafo_centered() (not trafo()): the slice/fill ExPolygons these masks
// are intersected against are centered by -center_offset, so bare trafo() left
// the paint mask off-surface for assembled/off-center objects (see call sites).
//
// NOTE on `used_states`: we deliberately do NOT gate the scan on
// `data.used_states[slot]` — `set_triangle_from_string` (the 3mf load path)
// does not populate that array, so a freshly loaded .3mf would always read
// "all states unused" and the override would never fire. Iterating all 15
// slots via get_facets() is robust to both fresh-paint and load-from-3mf.
// ---------------------------------------------------------------------------
int SurfaceColorMix::dominant_painted_slot_in_z_range(const PrintObject* po,
                                                       double z_min, double z_max,
                                                       bool downward)
{
    if (!po) return 0;
    const ModelObject* mo = po->model_object();
    if (!mo) return 0;

    // NEOTKO_COLORSTITCH_TAG s161 — must match the slice frame. Slicing uses
    // trafo_centered() (trafo minus the XY center_offset); the fill/slice
    // ExPolygons this mask is compared against live in that centered frame.
    // Using bare trafo() shifts the paint mask by +center_offset in XY —
    // ≈0 for a single object modeled at its origin (works by accident) but
    // = the assembly bbox-center for an ASSEMBLED object → mask lands off the
    // surface → 0 intersection → painted slot silently dropped to "natural".
    const Transform3d trafo = po->trafo_centered();
    const double z_tol = 0.02; // fp slack around the layer extent

    int counts[ModelVolume::COLORMIX_SLOT_COUNT] = {0};   // NEOTKO_COLORSTITCH_TAG s112: 16→31
    bool any_painted = false;
    // NEOTKO_COLORSTITCH_TAG — s112 diagnóstico: extent Z real de las facetas
    // pintadas (hacia arriba) vs la banda consultada.
    int    _dbg_up_tris = 0, _dbg_in_band = 0;
    double _dbg_minz = 1e30, _dbg_maxz = -1e30;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        const Transform3d vt = trafo * mv->get_matrix();
        for (int slot = 1; slot < ModelVolume::COLORMIX_SLOT_COUNT; ++slot) {   // NEOTKO_COLORSTITCH_TAG s112
            // NEOTKO_COLORSTITCH_TAG s137b — IGNORAR slots pintados que NO mapean a
            // ningún profile (slot→profile_id==0). Son "pintura fantasma": facetas
            // que quedaron con un índice de slot huérfano (p.ej. una zona pintada y
            // luego desemparejada / sin profile), visibles como una caja gris sin
            // color. Si tienen MÁS triángulos que el slot bueno, robaban el dominante
            // → profile_id_for_slot()=0 → top_profile=<none> → SCM_MODE SKIP → el
            // ColorMix de esa capa NO se generaba. Fill.cpp (FOOTPRINT_CLIP) ya los
            // ignora; aquí alineamos el escaneo para que ambos coincidan.
            // Cubrimos DOS casos de huérfano: (a) tabla a 0 (sin profile), y
            // (b) pid colgado = apunta a un profile ya borrado (p.ej. tras
            // garbage_collect_auto_profiles) → find()==null. Ambos = "fantasma".
            {
                const int _pid = mv->colormix_slot_to_profile_id[slot];
                if (_pid == 0 ||
                    Slic3r::SurfaceEffectProfileManager::get().find(_pid) == nullptr)
                    continue;
            }
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
                // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): downward picks the underside
                // (n.z<0, mesh-bottom = min_z); default upward keeps the legacy
                // top scan (n.z>0, mesh-top = max_z) byte-identical.
                if (downward ? (n.z() >= 0.0) : (n.z() <= 0.0)) continue;
                const double pick_z = downward
                    ? std::min({v0.z(), v1.z(), v2.z()})
                    : std::max({v0.z(), v1.z(), v2.z()});
                ++_dbg_up_tris;
                _dbg_minz = std::min(_dbg_minz, pick_z);
                _dbg_maxz = std::max(_dbg_maxz, pick_z);
                if (pick_z >= z_min - z_tol && pick_z <= z_max + z_tol) {
                    counts[slot]++;
                    any_painted = true;
                    ++_dbg_in_band;
                }
            }
        }
    }

    int best_slot = 0, best_count = 0;
    for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s)   // NEOTKO_COLORSTITCH_TAG s112
        if (counts[s] > best_count) { best_count = counts[s]; best_slot = s; }

    // NEOTKO_COLORSTITCH_TAG — s139 dbg: desglose POR-SLOT de los conteos en banda.
    // El "slot=1(pre)" agregado no permitía distinguir empate (1-vs-1, contaminación
    // entre cajas que comparten esquina) de mayoría real (3-vs-1, repintado). Esto
    // imprime counts[s] de cada slot pintado + qué slot gana, para ver en qué banda
    // un ColorStitch real (slot 2) pierde contra otro slot por conteo de facetas.
    std::string _per_slot;
    for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s)
        if (counts[s] > 0)
            _per_slot += (_per_slot.empty() ? "" : ",") + std::to_string(s)
                       + ":" + std::to_string(counts[s]);

    NEOTKO_LOG(PROFILE, "DOM_SLOT band=[" << z_min << "," << z_max << "]"
        << " up_tris=" << _dbg_up_tris << " in_band=" << _dbg_in_band
        << " facet_maxz=[" << (_dbg_up_tris ? _dbg_minz : 0) << ","
        << (_dbg_up_tris ? _dbg_maxz : 0) << "]"
        << " counts=[" << _per_slot << "]"
        << " → slot=" << best_slot << (any_painted ? "" : "(none)"));

    if (!any_painted) return 0;
    return best_slot;
}

// NEOTKO_PROFILE_TAG — Fase 6c: XY footprint of a slot's painted triangles in a
// Z band. Mirrors dominant_painted_slot_in_z_range's scan/frame, but instead of
// counting it projects each qualifying upward triangle to the XY plane (dropping
// Z), scales to clipper coords, and unions them. The result is the mask FASE 2
// uses to clip a painted surface so the sandwich applies ONLY where the user
// painted — the painted shape is preserved instead of flooding the whole top.
ExPolygons SurfaceColorMix::painted_footprint_in_z_range(const PrintObject* po, int slot,
                                                         double z_min, double z_max,
                                                         bool downward)
{
    ExPolygons out;
    if (!po || slot <= 0 || slot >= ModelVolume::COLORMIX_SLOT_COUNT) return out;   // NEOTKO_COLORSTITCH_TAG s112
    const ModelObject* mo = po->model_object();
    if (!mo) return out;

    const Transform3d trafo = po->trafo_centered(); // NEOTKO_COLORSTITCH_TAG s161 — slice frame (see dominant_painted_slot_in_z_range)
    const double z_tol = 0.02; // same fp slack as the dominant-slot scan
    Polygons tris;             // one CCW triangle polygon per qualifying facet

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        const Transform3d vt = trafo * mv->get_matrix();
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
            // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): mirror to the underside on demand.
            if (downward ? (n.z() >= 0.0) : (n.z() <= 0.0)) continue;
            const double pick_z = downward
                ? std::min({v0.z(), v1.z(), v2.z()})
                : std::max({v0.z(), v1.z(), v2.z()});
            if (pick_z < z_min - z_tol || pick_z > z_max + z_tol) continue;
            Polygon p;
            p.points = { Point(scale_(v0.x()), scale_(v0.y())),
                         Point(scale_(v1.x()), scale_(v1.y())),
                         Point(scale_(v2.x()), scale_(v2.y())) };
            if (std::abs(p.area()) < SCALED_EPSILON) continue; // degenerate
            p.make_counter_clockwise();
            tris.push_back(std::move(p));
        }
    }
    if (tris.empty()) return out;
    // Union the overlapping/adjacent triangle projections into clean regions.
    out = union_ex(tris);
    return out;
}

// NEOTKO_PROFILE_TAG — Fase 6c v2: enumerate every painted slot present in the
// Z band. Mirrors dominant_painted_slot_in_z_range's scan but returns the FULL
// set instead of just the winner. Used to handle multiple painted profiles at
// the same Z (twin islands with different profiles each get their own mask).
std::vector<int> SurfaceColorMix::enumerate_painted_slots_in_z_range(
    const PrintObject* po, double z_min, double z_max, bool downward)
{
    std::vector<int> out;
    if (!po) return out;
    const ModelObject* mo = po->model_object();
    if (!mo) return out;

    const Transform3d trafo = po->trafo_centered(); // NEOTKO_COLORSTITCH_TAG s161 — slice frame (see dominant_painted_slot_in_z_range)
    const double z_tol = 0.02;
    bool present[ModelVolume::COLORMIX_SLOT_COUNT] = {false};   // NEOTKO_COLORSTITCH_TAG s112: 16→31

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        const Transform3d vt = trafo * mv->get_matrix();
        for (int slot = 1; slot < ModelVolume::COLORMIX_SLOT_COUNT; ++slot) {   // NEOTKO_COLORSTITCH_TAG s112
            if (present[slot]) continue; // already known
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
                // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): underside scan on demand.
                if (downward ? (n.z() >= 0.0) : (n.z() <= 0.0)) continue;
                const double pick_z = downward
                    ? std::min({v0.z(), v1.z(), v2.z()})
                    : std::max({v0.z(), v1.z(), v2.z()});
                if (pick_z >= z_min - z_tol && pick_z <= z_max + z_tol) {
                    present[slot] = true;
                    break;
                }
            }
        }
    }
    for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s)   // NEOTKO_COLORSTITCH_TAG s112
        if (present[s]) out.push_back(s);
    return out;
}

// ---------------------------------------------------------------------------
// NEOTKO_STICKER_TAG — Sandwich Sticker helpers.
//
// A sticker (ModelObject::colormix_stickers) is a 1-colour SVG shape placed on
// a flat top face, carrying a SurfaceEffectProfile — a VECTOR paint mask, no
// facets. These helpers mirror the painted-slot scan trio above (has-any /
// enumerate-in-band / footprint) so Fill.cpp's FASE 6c v2 pre-split can consume
// stickers through the same machinery. Frames: sticker.transform maps
// sticker-local mm (SVG plane, z=0, shape centered by NSVGUtils) into the
// OBJECT frame; composing po->trafo_centered() (NEVER trafo(), s161) yields the
// slice frame the fill ExPolygons live in.
// ---------------------------------------------------------------------------

bool SurfaceColorMix::object_has_any_colormix_stickers(const ModelObject* mo)
{
    if (!mo) return false;
    for (const ColorMixSticker& st : mo->colormix_stickers) {
        // Ghost filter (mirrors the s137b slot filter): a sticker without svg,
        // without profile, or pointing at a deleted profile must not flip the
        // object into painter mode on its own.
        if (st.svg_data.empty() || st.profile_id == 0) continue;
        if (Slic3r::SurfaceEffectProfileManager::get().find(st.profile_id) == nullptr) continue;
        return true;
    }
    return false;
}

std::vector<size_t> SurfaceColorMix::enumerate_stickers_in_z_range(
    const PrintObject* po, double z_min, double z_max)
{
    std::vector<size_t> out;
    if (!po) return out;
    const ModelObject* mo = po->model_object();
    if (!mo || mo->colormix_stickers.empty()) return out;

    const Transform3d trafo = po->trafo_centered(); // NEOTKO_STICKER_TAG — slice frame (s161, see dominant_painted_slot_in_z_range)
    const double z_tol = 0.02; // same fp slack as the painted scans
    // TOP-DOWN: back() of the pile is the topmost sticker → first in the result,
    // so the Fill.cpp consumer can peel the remaining area in occlusion order.
    for (size_t k = mo->colormix_stickers.size(); k-- > 0; ) {
        const ColorMixSticker& st = mo->colormix_stickers[k];
        if (st.svg_data.empty() || st.profile_id == 0) continue;
        if (Slic3r::SurfaceEffectProfileManager::get().find(st.profile_id) == nullptr) continue;
        const double anchor_z = (trafo * st.transform * Vec3d(0.0, 0.0, 0.0)).z();
        if (anchor_z >= z_min - z_tol && anchor_z <= z_max + z_tol)
            out.push_back(k);
    }
    if (!out.empty())
        NEOTKO_LOG(PROFILE, "STICKER_ENUM band=[" << z_min << "," << z_max << "]"
            << " pile=" << mo->colormix_stickers.size()
            << " in_band=" << out.size() << " (top-down)");
    return out;
}

Polygons SurfaceColorMix::sticker_rings_in_transform(const ColorMixSticker& sticker,
                                                     const Transform3d& to_target)
{
    Polygons rings;
    if (sticker.svg_data.empty()) return rings;

    // nanosvg parses a private copy of the string (see NSVGUtils::nsvgParse) —
    // safe under the parallel make_fills / GUI render thread. Top/penu layers
    // are few per object (and GUI overlays only rebuild on edit), so the
    // per-call parse is acceptable (revisit with a cache if a pile of many
    // stickers ever shows up in a profile trace).
    NSVGimage_ptr image = nsvgParse(sticker.svg_data, "mm", 96.0f);
    if (image == nullptr || image->shapes == nullptr) {
        NEOTKO_LOG(PROFILE, "STICKER_MASK parse FAILED name='" << sticker.name << "'");
        return rings;
    }
    // Same tesselation as GLGizmoSVG::select_shape: 0.1 mm tolerance, expressed
    // in scaled^2 units (see get_tesselation_tolerance, GLGizmoSVG.cpp:87).
    const double tol_mm = 0.1;
    NSVGLineParams params{ (tol_mm * tol_mm) / SCALING_FACTOR / SCALING_FACTOR };
    const ExPolygonsWithIds shapes = create_shape_with_ids(*image, params);
    if (shapes.empty()) return rings;

    // An XY mirror (negative determinant of the 2D linear part) flips ring
    // orientation; reverse the rings so contour/hole winding stays consistent
    // for a NonZero union downstream (whichever caller does it).
    const double det_xy = to_target(0, 0) * to_target(1, 1) - to_target(0, 1) * to_target(1, 0);

    auto push_ring = [&](const Polygon& src) {
        Polygon p;
        p.points.reserve(src.points.size());
        for (const Point& q : src.points) {
            const Vec3d v = to_target * Vec3d(unscale<double>(q.x()), unscale<double>(q.y()), 0.0);
            p.points.emplace_back(coord_t(scale_(v.x())), coord_t(scale_(v.y())));
        }
        if (det_xy < 0.0) std::reverse(p.points.begin(), p.points.end());
        if (std::abs(p.area()) < SCALED_EPSILON) return; // degenerate
        rings.push_back(std::move(p));
    };
    for (const ExPolygonsWithId& sh : shapes)
        for (const ExPolygon& ep : sh.expoly) {
            push_ring(ep.contour);
            for (const Polygon& h : ep.holes) push_ring(h);
        }
    return rings;
}

ExPolygons SurfaceColorMix::sticker_footprint_slice_frame(const ColorMixSticker& sticker,
                                                          const PrintObject* po)
{
    ExPolygons out;
    if (!po) return out;
    const Transform3d to_slice = po->trafo_centered() * sticker.transform;
    const Polygons rings = sticker_rings_in_transform(sticker, to_slice);
    if (rings.empty()) return out;
    out = union_ex(rings); // pftNonZero → CW holes subtract, letters keep their holes
    NEOTKO_LOG(PROFILE, "STICKER_MASK name='" << sticker.name << "'"
        << " rings=" << rings.size() << " regions=" << out.size());
    return out;
}

// NEOTKO_PAINT_COEXIST_TAG s91 v1.2 — MMU footprint for sandwich-top suppression.
//
// v1 ORIGINAL: included ALL facet orientations (lateral + bottom + top) so the
// predicate caught any MMU paint whose Z range overlapped the band. INTENT:
// "if any MMU paint is in this z column, MMU owns it."
//
// v1.2 FIX (real-world test exposed): lateral/bottom MMU paint produces
// TRIANGLES whose Z spans the full cube height. Overlapping with a sandwich
// band [9.88, 10.08] is satisfied even when the paint is on the FRONT FACE.
// Worse, projecting the lateral triangle to XY paints almost the entire cube
// footprint (the triangle covers the full face area), so the intersection
// with the top surface is huge → predicate returns GOV → sandwich on TOP gets
// suppressed even though MMU lives on the side.
//
// Fix: mirror the colormix `painted_footprint_in_z_range` filter — keep ONLY
// upward-facing facets (n.z() > 0) whose `max_z` falls in the band. This
// captures MMU paint that actually lives ON the top surface at this layer,
// and ignores lateral MMU. Trade-off: MMU paint on a sloped surface that
// happens to graze the top band may be missed (acceptable — sandwich vs MMU
// arbitration on slopes is a separate problem not in s91 scope).
ExPolygons SurfaceColorMix::mmu_painted_footprint_in_z_range(
    const PrintObject* po, double z_min, double z_max)
{
    ExPolygons out;
    if (!po) return out;
    const ModelObject* mo = po->model_object();
    if (!mo) return out;
    bool any_mm = false;
    for (const ModelVolume* mv : mo->volumes)
        if (mv && mv->is_model_part() && !mv->mmu_segmentation_facets.empty()) {
            any_mm = true; break;
        }
    if (!any_mm) return out;

    const Transform3d trafo = po->trafo_centered(); // NEOTKO_COLORSTITCH_TAG s161 — slice frame (MMU mirror; see dominant_painted_slot_in_z_range)
    const double      z_tol = 0.02;
    Polygons          tris;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        if (mv->mmu_segmentation_facets.empty()) continue;
        const Transform3d vt = trafo * mv->get_matrix();
        for (int slot = 1; slot < 16; ++slot) {
            const indexed_triangle_set its = mv->mmu_segmentation_facets.get_facets(
                *mv, static_cast<EnforcerBlockerType>(slot));
            if (its.indices.empty()) continue;
            for (const auto& tri : its.indices) {
                const Vec3f& v0f = its.vertices[tri[0]];
                const Vec3f& v1f = its.vertices[tri[1]];
                const Vec3f& v2f = its.vertices[tri[2]];
                const Vec3d  v0  = vt * Vec3d(v0f.x(), v0f.y(), v0f.z());
                const Vec3d  v1  = vt * Vec3d(v1f.x(), v1f.y(), v1f.z());
                const Vec3d  v2  = vt * Vec3d(v2f.x(), v2f.y(), v2f.z());
                // s91 v1.2: upward-facing only (drop lateral/bottom MMU).
                const Vec3d e1 = v1 - v0, e2 = v2 - v0;
                const Vec3d n  = e1.cross(e2);
                if (n.z() <= 0.0) continue;
                // s91 v1.2: max_z in band (top-edge facet lives here), not
                // just overlap. A vertical facet spanning the cube has
                // max_z at the cube top — but its n.z() is 0, so it's
                // already rejected above. A roof facet has its max_z in
                // the band where it physically prints.
                const double max_z = std::max({v0.z(), v1.z(), v2.z()});
                if (max_z < z_min - z_tol || max_z > z_max + z_tol) continue;
                Polygon p;
                p.points = { Point(scale_(v0.x()), scale_(v0.y())),
                             Point(scale_(v1.x()), scale_(v1.y())),
                             Point(scale_(v2.x()), scale_(v2.y())) };
                if (std::abs(p.area()) < SCALED_EPSILON) continue;
                p.make_counter_clockwise();
                tris.push_back(std::move(p));
            }
        }
    }
    if (tris.empty()) return out;
    out = union_ex(tris);
    return out;
}

// NEOTKO_PAINT_COEXIST_TAG s91 — single source of truth predicate.
// Consulted identically from SurfaceColorMix::assign_and_group_tools (per-EEC),
// Fill.cpp surface_fill loop (per surface_fill piece), and
// ToolOrdering::collect_extruders (per LayerRegion top/penu surfaces).
// Any divergence between these 3 callsites = wipe-tower divergence = crash.
bool SurfaceColorMix::mmu_governs_xy(
    const PrintObject* po,
    const ExPolygons& surface_xy,
    double z_min, double z_max)
{
    if (!po || surface_xy.empty()) return false;

    // NEOTKO_PAINT_COEXIST_TAG s136 — PRECEDENCE DECISION (user, 2026-06-22):
    // the Sandwich/ColorStitch painter ALWAYS governs the surfaces it paints; MMU
    // never suppresses sandwich on a sandwich surface. Rationale: reconciling MMU
    // top-layer ownership with sandwich per-pass profiles is near-impossible, and
    // the sandwich painter delimits flat top surfaces better than MMU. MMU still
    // applies everywhere sandwich does NOT (walls, non-top regions, layers with no
    // sandwich profile). Returning false here keeps all 3 callsites (assign_and_
    // group_tools, Fill surface_fill, ToolOrdering) consistent → no wipe-tower
    // divergence. The original s91 coverage-fraction arbitration is preserved
    // below (now unreachable) so a gated MMU-governs mode can be reintroduced.
    {
        static const bool _sandwich_always_governs = true; // flip to false to restore s91 arbitration
        if (_sandwich_always_governs) {
            NEOTKO_LOG(COLORMIX, "s91/coexist mmu_governs_xy z=[" << z_min << ","
                << z_max << "] → SANDWICH_GOVERNS (s136 precedence: sandwich wins over MMU)");
            return false;
        }
    }

    const ModelObject* mo = po->model_object();
    if (!mo) return false;
    bool any_mm = false;
    for (const ModelVolume* mv : mo->volumes)
        if (mv && mv->is_model_part() && !mv->mmu_segmentation_facets.empty()) {
            any_mm = true; break;
        }
    if (!any_mm) return false;

    const ExPolygons fp = mmu_painted_footprint_in_z_range(po, z_min, z_max);
    if (fp.empty()) {
        NEOTKO_LOG(COLORMIX, "s91/coexist mmu_governs_xy z=[" << z_min << ","
            << z_max << "] fp=empty → false");
        return false;
    }
    ExPolygons inter = intersection_ex(surface_xy, fp);
    if (inter.empty()) {
        NEOTKO_LOG(COLORMIX, "s91/coexist mmu_governs_xy z=[" << z_min << ","
            << z_max << "] inter=empty → false");
        return false;
    }
    // s91 v1.3 — coverage-fraction threshold (not absolute area).
    //
    // WHY: when MMU paint maps to the SAME extruder as the natural one
    // (e.g. user paints slot=1 on a cube whose default is also T0), Slic3r's
    // MMU partitioning DOES NOT split the LayerRegion — there's no extruder
    // change to honor. The full top remains ONE region; FILL sees one piece
    // covering the entire top. A small MMU "paint" patch (e.g. 7.9 mm² of
    // 100 mm²) would falsely register as governance under an absolute
    // threshold and suppress sandwich on the whole top — even though MMU
    // physically owns nothing (same extruder = no real partition).
    //
    // When MMU paint maps to a DIFFERENT extruder, partitioning splits the
    // region into MMU-owned and natural sub-regions; the natural region's
    // piece XY is disjoint from MMU XY (~0 intersection → FREE), and the
    // MMU region's piece is fully covered by MMU XY (~100% → GOV).
    //
    // Coverage > 50% strikes the balance: small MMU patches on shared XY
    // don't fight sandwich; true MMU partitions still get fully suppressed.
    double inter_a = 0.0;
    for (const auto& e : inter) inter_a += std::abs(e.area());
    double piece_a = 0.0;
    for (const auto& e : surface_xy) piece_a += std::abs(e.area());
    // Trivial-area guard (still reject sub-mm² slivers as a sanity floor).
    const double trivial_thr = scaled<double>(0.01) * scaled<double>(0.01);
    if (inter_a <= trivial_thr) {
        NEOTKO_LOG(COLORMIX, "s91/coexist mmu_governs_xy z=[" << z_min << ","
            << z_max << "] inter_area=" << inter_a
            << " ≤ trivial_thr=" << trivial_thr << " → no");
        return false;
    }
    const double frac = (piece_a > 0.0) ? inter_a / piece_a : 0.0;
    const double cov_thr = 0.5; // require MMU to own >50% of the piece
    const bool gov = frac > cov_thr;
    NEOTKO_LOG(COLORMIX, "s91/coexist mmu_governs_xy z=[" << z_min << ","
        << z_max << "] inter=" << inter_a << " piece=" << piece_a
        << " frac=" << frac << " cov_thr=" << cov_thr
        << " → " << (gov ? "GOV" : "no"));
    return gov;
}

bool SurfaceColorMix::mmu_governs_surface(
    const PrintObject* po, const Surface& surface,
    double z_min, double z_max)
{
    if (surface.expolygon.empty()) return false;
    ExPolygons one; one.push_back(surface.expolygon);
    return mmu_governs_xy(po, one, z_min, z_max);
}

// Resolve the SurfaceEffectProfile id for a painted slot on the print's model.
// Slot tables live per-ModelVolume; we pick the first model_part with a
// non-zero entry at that slot (the gizmo keeps them consistent across volumes
// of the same object).
int SurfaceColorMix::profile_id_for_slot(const PrintObject* po, int slot)
{
    if (!po || slot <= 0 || slot >= ModelVolume::COLORMIX_SLOT_COUNT) return 0;   // NEOTKO_COLORSTITCH_TAG s112
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
        // NEOTKO_SANDWICH_TAG s119 — painter mode requires ACTUAL painted triangles,
        // not merely a populated slot→profile table. The eraser removes the painted
        // facets but does NOT clear colormix_slot_to_profile_id (slots stay mapped),
        // so an object painted-then-fully-erased kept reporting "painted" here →
        // painter takeover with no geometry → the preset SandwichDialog settings were
        // ignored AND nothing was painted → vanilla. Gating on real facets makes a
        // fully-erased object fall back to PRESET mode, as expected.
        if (mv->color_mix_paint_facets.empty())
            continue;
        for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s)   // NEOTKO_COLORSTITCH_TAG s112
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
        for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s) {   // NEOTKO_COLORSTITCH_TAG s112
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid <= 0) continue;
            if (!profile_ids_seen.insert(pid).second) continue;
            const SurfaceEffectProfile* p = mgr.find(pid);
            if (!p) continue;
            // NEOTKO_COLORSTITCH_TAG — s118: fuente de verdad = el STACK de penu del
            // perfil. Si el painter compuso una zona Penu (stack_penu_json con passes),
            // el color DECLARA penu y debemos forzar la estructura penu, aunque el
            // preset no tenga penultimate activo (el painter manda también para CREAR
            // la capa penu, no sólo para el color). El enum legacy interlayer_colormix_
            // surface sólo se ponía según los pases ColorMix → se perdían penu Solid/PB
            // u objetos cuyo enum salía "top". Ver bug "penu pintado no slicea sin
            // penultimate activo en el main UX".
            {
                const SurfacePassStack penu = SurfacePassStack::from_json(p->stack_penu_json);
                // NEOTKO_SANDWICH_TAG s119 (EMPTY model): the painter declares penu
                // iff the zone carries a NON-None pass. An authored-but-empty penu
                // ([None] passthrough) must NOT force the penu layer into existence.
                if (penu.any_effect())
                    return true;
            }
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

// NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): Bottom is painter-only (no legacy preset
// keys). The object declares bottom iff some painted profile carries a Bottom
// WIP stack with a real effect. Source of truth = stack_bottom_json, mirroring
// the s118/s119 content-driven model of object_painter_wants_penu.
bool SurfaceColorMix::object_painter_wants_bottom(const ModelObject* mo)
{
    if (!mo) return false;
    auto& mgr = Slic3r::SurfaceEffectProfileManager::get();
    std::set<int> profile_ids_seen;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s) {
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid <= 0) continue;
            if (!profile_ids_seen.insert(pid).second) continue;
            const SurfaceEffectProfile* p = mgr.find(pid);
            if (!p) continue;
            const SurfacePassStack bottom = SurfacePassStack::from_json(p->stack_bottom_json);
            if (bottom.any_effect())
                return true;
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
    double layer_height,
    bool config_has_pass_override
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

    // NEOTKO_PAINT_COEXIST_TAG s91 — per-surface MMU governance (camino 3).
    // Replaces the global is_mm_painted() short-circuit. Each fill EEC checks
    // MMU footprint at its bbox below; only EECs whose XY is governed by MMU
    // paint at this layer's slab are skipped. Unpainted surfaces of a
    // partially-MMU object now get sandwich/ColorMix normally.
    const bool _s91_obj_has_mmu_paint =
        (model_object && model_object->is_mm_painted());
    const double _s91_z_layer_min = (layer_height > 0.0)
        ? layer_print_z - layer_height : layer_print_z;
    const double _s91_z_layer_max = layer_print_z;
    if (_s91_obj_has_mmu_paint)
        NEOTKO_LOG(COLORMIX, "s91/coexist SCM ENTRY layer=" << layer_idx
            << " obj_has_mmu=1 z=[" << _s91_z_layer_min << "," << _s91_z_layer_max
            << "] — per-EEC governance active (was global skip pre-s91)");

    // NEOTKO_STICKER_TAG — a sticker-only object (no painted facets) must also
    // take painter mode here: this gate mirrors the one in Fill.cpp's
    // _mp_painter_mode (which is what routed this call's config_has_pass_override
    // in the first place); divergence between the two would leave the sticker's
    // per-piece override fighting the PRESET gate instead of being honoured.
    const bool painter_mode_obj = object_has_any_colormix_paint(model_object)
        || object_has_any_colormix_stickers(model_object);
    // NEOTKO_COLORSTITCH_TAG — s112 diagnóstico: ¿el objeto entra en painter-mode?
    // Recontamos los slots pintados aquí mismo (igual que object_has_any) + el
    // puntero del mo para cruzarlo con el PAINT_SLOT de PrintObjectSlice.
    {
        int _nvol = 0, _npaint = 0, _nfacets = 0;
        if (model_object)
            for (const ModelVolume* _mv : model_object->volumes) {
                if (!_mv || !_mv->is_model_part()) continue;
                ++_nvol;
                if (!_mv->color_mix_paint_facets.empty()) ++_nfacets;
                for (int _s = 1; _s < ModelVolume::COLORMIX_SLOT_COUNT; ++_s)
                    if (_mv->colormix_slot_to_profile_id[_s] != 0) ++_npaint;
            }
        // _nfacets>0 con _npaint=0 = facetas pintadas pero tabla slot vacía (bug copia).
        NEOTKO_LOG(PROFILE, "SCM_GATE layer=" << layer_idx
            << " painter_mode=" << (painter_mode_obj ? "1" : "0")
            << " preset_enabled=" << config.interlayer_colormix_enabled.value
            << " mo=" << (const void*)model_object
            << " parts=" << _nvol << " painted_slots=" << _npaint
            << " facet_vols=" << _nfacets
            << " obj='" << (model_object ? model_object->name : "<null>") << "'");
    }
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
        // NEOTKO_COLORSTITCH_TAG — s112 fix: extender el límite superior para
        // capturar la pintura del tope de malla que queda entre capas (hasta ~1
        // altura de capa por encima de la última rebanada). Top +1 capa; penu +2
        // (ancla 1 capa más abajo). Ver Fill.cpp mismo fix.
        const double z_top_min  = layer_print_z - layer_height;
        const double z_top_max  = layer_print_z + layer_height;
        const double z_penu_min = layer_print_z;
        const double z_penu_max = layer_print_z + 2.0 * layer_height;
        const int top_slot  = dominant_painted_slot_in_z_range(print_object, z_top_min,  z_top_max);
        const int penu_slot = dominant_painted_slot_in_z_range(print_object, z_penu_min, z_penu_max);

        // NEOTKO_COLORSTITCH_TAG — s139 dbg: el SCM colapsa a UN solo slot dominante
        // por rol/capa, pero una capa puede tener VARIOS slots pintados (cajas que
        // comparten capa/esquina). Si el dominante es un slot SIN colormix (p.ej. un
        // gradiente solid), un ColorStitch real presente en la MISMA capa se pierde
        // (SCM SKIP global). Esto enumera todos los slots del top band con su
        // colormix.present para ver si hay un slot-stitch eclipsado por el dominante.
        if (NeoDebug::enabled(NeoDebug::PROFILE)) {
            std::string _present;
            for (int _s : SurfaceColorMix::enumerate_painted_slots_in_z_range(
                              print_object, z_top_min, z_top_max)) {
                const int _pid = profile_id_for_slot(print_object, _s);
                const auto* _p = SurfaceEffectProfileManager::get().find(_pid);
                _present += (_present.empty() ? "" : " ") + std::to_string(_s) + "{"
                          + (_p ? _p->name : "<null>") + ",cm="
                          + (_p && _p->colormix.present ? "1" : "0") + "}";
            }
            NEOTKO_LOG(PROFILE, "TOP_SLOTS layer=" << layer_idx << " z=" << layer_print_z
                << " dominant=" << top_slot << " present=[" << _present << "]");
        }

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

        // NEOTKO_PAINT_COEXIST_TAG s91 — per-EEC MMU governance.
        // If this object has MMU paint anywhere, test the EEC's bbox against
        // the MMU footprint at this layer's slab. If MMU governs the XY, this
        // EEC is owned by MMU segmentation — skip ColorMix for it. Other EECs
        // in this fills collection (non-MMU regions of the same object) keep
        // going normally. Bbox is a conservative first-pass; if false positives
        // show up in concave geometries we can upgrade to real polyline proj.
        if (_s91_obj_has_mmu_paint) {
            Points _eec_pts;
            sub->collect_points(_eec_pts);
            if (!_eec_pts.empty()) {
                BoundingBox bb(_eec_pts);
                ExPolygons _eec_xy;
                Polygon poly;
                poly.points = {
                    Point(bb.min.x(), bb.min.y()),
                    Point(bb.max.x(), bb.min.y()),
                    Point(bb.max.x(), bb.max.y()),
                    Point(bb.min.x(), bb.max.y()),
                };
                ExPolygon ep; ep.contour = std::move(poly);
                _eec_xy.emplace_back(std::move(ep));
                if (mmu_governs_xy(print_object, _eec_xy,
                                   _s91_z_layer_min, _s91_z_layer_max)) {
                    NEOTKO_LOG(COLORMIX, "s91/coexist MMU_GOVERNS site=SCM"
                        << " layer=" << layer_idx
                        << " bb=[" << bb.min.x() << "," << bb.min.y() << ".."
                        << bb.max.x() << "," << bb.max.y() << "]"
                        << " → skip EEC (MMU owns this XY)");
                    continue;
                }
            }
        }

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
        // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): bottom roles (erBottomSurface, erBridgeInfill)
        // are authored with the SAME widget as Top (draw_zone_editor penu=false), so their
        // pass kv uses the top key names. Treat them as "top-keyed" here so the gv loader
        // reads the right interlayer_colormix_* keys (not the empty _penu_ set).
        const bool gv_is_top_role = (first_path->role() == erTopSolidInfill ||
                                     first_path->role() == erBottomSurface ||
                                     first_path->role() == erBridgeInfill);
        const SurfaceEffectProfile* eff_profile =
            gv_is_top_role ? painted_top_profile : painted_penu_profile;
        // NEOTKO_COLORSTITCH_TAG — s139 fix B (per-pieza): cuando Fill.cpp resolvió un
        // override de pase para ESTA pieza (config_has_pass_override), el perfil de la
        // pieza ya viaja en `config` (interlayer_colormix_enabled=true en cm_cfg_override,
        // Fill.cpp:2196) — NO en el slot dominante. El gate confiaba sólo en el dominante
        // (painted_top_profile), que colapsa la capa a UN slot: cuando dos cajas comparten
        // capa y el dominante cae en un slot sin colormix (p.ej. un gradiente solid), el
        // ColorStitch real de la OTRA caja se perdía (SKIP global). Confiar en el config
        // per-pieza cierra esa clase de bugs ("cajas que comparten capa/esquina").
        const bool painted_override =
            (config_has_pass_override && config.interlayer_colormix_enabled.value)
            || (eff_profile && eff_profile->colormix.present);

        if (painter_mode_obj) {
            // Painter mode: only process top/penu roles AND only if this layer
            // has a painted profile for the role. The painter decides where —
            // preset's surface/zone/filter gates do not apply.
            const ExtrusionRole r = first_path->role();
            // NEOTKO_SANDWICH_TAG s119 — SCM_MODE canary. Two bugs this session lived
            // exactly here (painter penu blocked by preset surface gate; erased object
            // wrongly in painter mode). This logs WHICH branch ran and WHICH gate cut,
            // so "no colormix on this role" is greppable in one line, not reconstructed
            // from bucket counts.
            // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): admit bottom roles too, else the bottom
            // ColorStitch EEC is never tool-encoded → eec_to_tool_buckets returns 1 bucket
            // on the natural tool (T0) → no toolchange/color/wipetower, and the suppressed
            // bridge fill leaves only the (overhang) wall visible.
            const bool _cut_role = (r != erTopSolidInfill && r != erPenultimateInfill &&
                                    r != erBottomSurface && r != erBridgeInfill);
            const bool _cut_paint = !painted_override;
            NEOTKO_LOG(PROFILE, "SCM_MODE layer=" << layer_idx << " role=" << (int)r
                << " mode=painter gate=" << (_cut_role ? "SKIP(non-top/penu)"
                    : _cut_paint ? "SKIP(no painted profile for role)" : "PASS"));
            if (_cut_role) continue;
            if (_cut_paint) continue;
        } else {
            // Preset mode (original gates).
            // NEOTKO_SANDWICH_TAG s119 — SCM_MODE canary (preset branch). surface is
            // the preset interlayer_colormix_surface; should_process_role gating the
            // PENU here is what the SandwichDialog "Enabled" used to (wrongly) drive.
            const bool _cut_surf = !should_process_role(first_path->role(), surface);
            NEOTKO_LOG(PROFILE, "SCM_MODE layer=" << layer_idx
                << " role=" << (int)first_path->role()
                << " mode=preset surface=" << surface
                << " gate=" << (_cut_surf ? "SKIP(should_process_role)" : "PASS"));
            if (_cut_surf) continue;
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
            int    repetitions;  // NEOTKO_COLORMIX_TAG — s80: repeat the gradient N times
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
            gv.repetitions = config.interlayer_colormix_repetitions.value;
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
            gv.repetitions = config.interlayer_colormix_penu_repetitions.value;
        }

        // NEOTKO_PROFILE_TAG — Fase D: painted-profile override.
        // The profile's ColorMix payload was snapshot-taken with the full key
        // set (top + _penu_). We pick the matching subset for this role and
        // overlay its values on top of the preset-loaded gv.
        // NEOTKO_COLORSTITCH_TAG — solo aplicar el payload COLAPSADO del profile cuando
        // el `config` NO trae ya el override per-pase. En la ruta FASE2 (band-loop) el
        // config ES cm_eff = región + pass.colormix.kv (per-pase, fuente de verdad);
        // re-aplicar aquí el colapsado pisaba los tools de la lámina con los del último
        // pase del rol (payload_from_stacks). gv ya quedó cargado desde config arriba.
        if (painted_override && !config_has_pass_override) {
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
            gv.repetitions = get_int ("repetitions",     gv.repetitions);
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
        // NEOTKO_COLORSTITCH_TAG — la lista de tools también debe salir del config
        // per-pase cuando lo hay (FASE2). painted_profile_tools_1based lee el payload
        // COLAPSADO del profile (tool_a del último pase del rol gana), que era el
        // segundo punto donde el pase de abajo heredaba los tools del de encima
        // (easing salía bien pero tools no). Con override per-pase, caer a la rama
        // cm_mode → gv.tool_a/b, que ya viene del config per-pase.
        if (painted_override && !config_has_pass_override) {
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
        // NEOTKO_COLORSTITCH_TAG — `tail` carries the connector arc to re-append at emission
        // (continuous-Monotonic split). Empty for every other path, so behaviour is unchanged.
        struct RawLine { Polyline pl; ExtrusionRole role; double mm3; float width; float height; Polyline tail{}; };
        std::vector<RawLine> raw_lines;

        if (n_paths > 1) {
            // NEOTKO_COLORSTITCH_TAG — "ColorStitch on Monotonic (continuous)" gate.
            // OFF (default): one ExtrusionPath == one colourable line. True for Monotonic Line
            // (anchor_length_max=0 → connect_infill does NOT fuse), and unchanged here.
            // ON: the continuous Monotonic pattern fuses several scanlines + perimeter connector
            // arcs into one path. Split each fused path into per-scanline runs so every visual line
            // gets its own tool; the connector arc rides with its OUTGOING scanline (run.tail), kept
            // (never dropped) and re-appended at emission. run.scan (clean) drives lane/slot geometry.
            // Post-hoc only — FillMonotonic / connect_infill are untouched.
            const bool split_monotonic = config.colorstitch_monotonic_split.value;
            NEOTKO_LOG(COLORMIX, "MONOTONIC_MODE layer=" << layer_idx
                << " n_paths=" << n_paths << " split=" << (split_monotonic ? 1 : 0));

            // NEOTKO_COLORSTITCH_TAG — SURFACE-level dominant fill axis (length-weighted
            // doubled-angle over ALL paths). One axis for the whole surface so the per-path
            // split is consistent; a per-path axis flips 90° on fragmented penu paths and
            // produces a horizontal "scan" that hijacks LaneQuant's global fill_dir.
            double surf_ax = 1.0, surf_ay = 0.0;
            if (split_monotonic) {
                double sa_x = 0.0, sa_y = 0.0;
                for (auto* e : sub->entities) {
                    const auto* p = dynamic_cast<const ExtrusionPath*>(e);
                    if (!p) continue;
                    const Points& pp = p->polyline.points;
                    for (size_t i = 1; i < pp.size(); ++i) {
                        const double dx = double(pp[i].x() - pp[i - 1].x());
                        const double dy = double(pp[i].y() - pp[i - 1].y());
                        const double l  = std::sqrt(dx * dx + dy * dy);
                        if (l < 1e-6) continue;
                        const double ux = dx / l, uy = dy / l;
                        sa_x += (ux * ux - uy * uy) * l;   // length-weighted doubled-angle
                        sa_y += (2.0 * ux * uy) * l;
                    }
                }
                const double sang = 0.5 * std::atan2(sa_y, sa_x);
                surf_ax = std::cos(sang);
                surf_ay = std::sin(sang);
                NEOTKO_LOG(COLORMIX, "  MONO_SURF_AXIS layer=" << layer_idx
                    << " axis=" << int(std::round(std::atan2(surf_ay, surf_ax) * 180.0 / M_PI)) << "deg");
            }

            for (auto* e : sub->entities) {
                auto* p = dynamic_cast<ExtrusionPath*>(e);
                if (!p) continue;
                if (split_monotonic) {
                    auto mono_runs = split_monotonic_path_into_runs(*p, surf_ax, surf_ay);
                    for (auto& mr : mono_runs) {
                        double len_mm = static_cast<double>(mr.scan.length()) / 1e6;
                        if (len_mm < min_length_mm) continue;
                        RawLine rl;
                        rl.pl     = std::move(mr.scan);
                        rl.role   = p->role();
                        rl.mm3    = p->mm3_per_mm;
                        rl.width  = p->width;
                        rl.height = p->height;
                        rl.tail   = std::move(mr.tail);
                        raw_lines.push_back(std::move(rl));
                    }
                } else {
                    double len_mm = static_cast<double>(p->polyline.length()) / 1e6;
                    if (len_mm < min_length_mm) continue;
                    raw_lines.push_back({ p->polyline, p->role(), p->mm3_per_mm, p->width, p->height });
                }
            }
            if (split_monotonic)
                NEOTKO_LOG(COLORMIX, "  MONO_SPLIT layer=" << layer_idx
                    << " fused_paths=" << n_paths << " → scan_runs=" << raw_lines.size());
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
            // NEOTKO_COLORMIX_TAG — s80: gradient repetitions. Build the dither
            // over a 1/reps slice of the lines (build_n), then tile it `reps`
            // times to fill all n lines → `reps` identical repeated gradients.
            // Surface analysis (line count, lane mode) is unchanged: the lane
            // mapping below still spreads the tiled sequence along the geometry.
            const int reps      = std::max(1, gv.repetitions);
            const int build_n   = (reps > 1) ? std::max(2, (n + reps - 1) / reps) : n;
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
                tools = build_dithered_tools_2color(build_n, t_a, t_b, pct_a, easing, gamma);
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
                tools = build_dithered_tools_3color(build_n, t_a, t_b, t_c, pct_a, pct_b,
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
                tools = build_custom_bands(build_n,
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

            // NEOTKO_COLORMIX_TAG — s80: tile the built period to fill all lines.
            // build_n == n when reps == 1 (period == full → tiling is a no-op).
            if (reps > 1 && !tools.empty() && (int)tools.size() < n) {
                const int period = static_cast<int>(tools.size());
                std::vector<int> tiled;
                tiled.reserve(n);
                for (int i = 0; i < n; ++i) tiled.push_back(tools[i % period]);
                tools.swap(tiled);
                NEOTKO_LOG(COLORMIX, "GRADIENT_REPEAT layer=" << layer_idx
                    << " role=" << (gv_is_top_role ? "Top" : "Penu")
                    << " reps=" << reps << " period=" << period
                    << " total=" << tools.size());
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

        // NEOTKO_COLORSTITCH_TAG — DEBUG-ONLY axis diagnostic (no behaviour change).
        // Penu+Monotonic shows ~2x the lane span of Top (same part, both 90°) → the global
        // fill_dir that LaneQuant builds from a SINGLE longest line's endpoints is being skewed
        // by one contaminated scan run. This block dumps, for monotonic surfaces:
        //   - the fill_dir the code actually uses (longest line, endpoint-based)
        //   - the length-weighted DOMINANT axis (the robust alternative)
        //   - their delta (a large delta == the bug is live on this surface)
        //   - the reference run, and the worst angular OUTLIER runs (suspected connector merged
        //     into a scan → diagonal endpoints), sorted by length.
        if (NeoDebug::enabled(NeoDebug::COLORMIX) && n_paths > 1 && raw_lines.size() >= 2) {
            auto ang_norm_pi = [](double a) { while (a < 0.0) a += M_PI; while (a >= M_PI) a -= M_PI; return a; };
            auto ang_dist_pi = [](double a, double b) { double d = std::abs(a - b); d = std::fmod(d, M_PI); if (d > M_PI / 2.0) d = M_PI - d; return d; };

            // fill_dir the template uses: longest line, endpoint direction.
            const size_t   ref      = lane_pick_reference(raw_lines);
            const LaneVec2 fdir     = lane_direction(raw_lines[ref].pl);
            const double   fdir_ang = ang_norm_pi(std::atan2(fdir.y, fdir.x));

            // Length-weighted dominant axis (doubled-angle), endpoint direction per run.
            double acc_x = 0.0, acc_y = 0.0;
            for (const auto& rlx : raw_lines) {
                const LaneVec2 d = lane_direction(rlx.pl);
                const double   a = std::atan2(d.y, d.x);
                const double   w = rlx.pl.length();
                acc_x += std::cos(2.0 * a) * w;
                acc_y += std::sin(2.0 * a) * w;
            }
            const double dom_ang = ang_norm_pi(0.5 * std::atan2(acc_y, acc_x));

            NEOTKO_LOG(COLORMIX, "MONO_AXIS_DIAG layer=" << layer_idx
                << " role=" << ExtrusionEntity::role_to_string(first_path->role())
                << " n=" << raw_lines.size()
                << " fill_dir(ref)=" << int(std::round(fdir_ang * 180.0 / M_PI)) << "deg"
                << " dominant=" << int(std::round(dom_ang * 180.0 / M_PI)) << "deg"
                << " delta=" << int(std::round(ang_dist_pi(fdir_ang, dom_ang) * 180.0 / M_PI)) << "deg"
                << " ref_idx=" << ref
                << " ref_len_mm=" << (raw_lines[ref].pl.length() / 1e6)
                << " ref_ang=" << int(std::round(ang_norm_pi(std::atan2(lane_direction(raw_lines[ref].pl).y, lane_direction(raw_lines[ref].pl).x)) * 180.0 / M_PI)) << "deg");

            // Worst outliers vs the dominant axis (the runs dragging fill_dir off-axis).
            std::vector<std::pair<double,int>> outliers; // (len, idx) for runs >20deg off-axis
            for (int i = 0; i < (int)raw_lines.size(); ++i) {
                const LaneVec2 d = lane_direction(raw_lines[i].pl);
                const double   a = ang_norm_pi(std::atan2(d.y, d.x));
                if (ang_dist_pi(a, dom_ang) > (20.0 * M_PI / 180.0))
                    outliers.emplace_back(raw_lines[i].pl.length(), i);
            }
            std::sort(outliers.begin(), outliers.end(), [](auto& l, auto& r){ return l.first > r.first; });
            NEOTKO_LOG(COLORMIX, "  MONO_AXIS_OUTLIERS layer=" << layer_idx
                << " count=" << outliers.size() << "/" << raw_lines.size() << " (>20deg off dominant)");
            const size_t cap = std::min<size_t>(8, outliers.size());
            for (size_t k = 0; k < cap; ++k) {
                const int i = outliers[k].second;
                const LaneVec2 d = lane_direction(raw_lines[i].pl);
                const double   a = ang_norm_pi(std::atan2(d.y, d.x));
                NEOTKO_LOG(COLORMIX, "    OUTLIER idx=" << i
                    << " len_mm=" << (raw_lines[i].pl.length() / 1e6)
                    << " pts=" << raw_lines[i].pl.points.size()
                    << " ang=" << int(std::round(a * 180.0 / M_PI)) << "deg"
                    << " dev=" << int(std::round(ang_dist_pi(a, dom_ang) * 180.0 / M_PI)) << "deg");
            }
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
            // NEOTKO_COLORSTITCH_TAG — re-attach the connector arc (continuous-Monotonic split) so it
            // prints in the OUTGOING colour. Empty tail for every other path → no-op (byte-identical).
            if (!rl.tail.points.empty()) {
                auto& tp = rl.tail.points;
                size_t k0 = (!new_path->polyline.points.empty() &&
                             new_path->polyline.points.back() == tp.front()) ? 1 : 0;
                for (size_t k = k0; k < tp.size(); ++k)
                    new_path->polyline.points.push_back(tp[k]);
            }
            encode_tool_in_path(new_path, tool_idx);
            tool_blocks[tool_idx].push_back(new_path);
        }

        if (tool_blocks.empty()) continue;

        // NEOTKO_COLORMIX_TAG s99 — single-tool short-circuit.
        // If after slot assignment every line ended up in the same tool bucket,
        // the ColorMix split produced ZERO multi-tool benefit but would still
        // break the original zig-zag's natural U-turn continuity (separate
        // ExtrusionPath* entries + no_sort=true → GCode emits travel+retract
        // between each pair). Free the throwaway paths we created in the loop
        // above and leave the original `sub` intact.
        if (unique_tool_order.size() == 1) {
            for (auto& kv : tool_blocks)
                for (auto* p : kv.second) delete p;
            NEOTKO_LOG(COLORMIX, "SHORT_CIRCUIT layer=" << layer_idx
                << " role=" << ExtrusionEntity::role_to_string(first_path->role())
                << " n_paths_before=" << n_paths
                << " single_tool=T" << unique_tool_order[0]
                << " (preserved original zig-zag, no split mutation)");
            continue;
        }

        // Nearest-neighbor travel optimization within each tool's block.
        // Minimizes travel moves and allows endpoint flipping per line.
        for (int t : unique_tool_order)
            optimize_tool_block_travel(tool_blocks[t]);

        // NEOTKO_COLORMIX_TAG s99 — stitch-back REVERTED.
        // The fusion broke the per-line discontinuity that ColorMix patterns
        // (GeoSort/LaneQuant/DirCluster) rely on to render visually uniform
        // gradients. Path separation is intentional. The retract/wipe burst
        // between adjacent monotonic lines is addressed at GCode-emit time
        // (suppress retract+wipe+hop when inter-path travel is below a small
        // threshold), not by re-stitching the paths here.

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
        // NEOTKO_SANDWICH_TAG s119 — per-tool extrusion width trace. Each ColorMix
        // tool should inherit the SAME source width (the role's flow, e.g. internal
        // solid infill). A divergence here (e.g. T0=0.3 / T1=0.4) is the "blue thin /
        // violet thick" bug: the per-tool paths are NOT sharing one width.
        float w_min = 1e9f, w_max = 0.f;
        for (const auto* p : pair.second) {
            total_mm += static_cast<double>(p->polyline.length()) / 1e6;
            const float w = p->width;   // ExtrusionPath::width is in mm
            w_min = std::min(w_min, w);
            w_max = std::max(w_max, w);
        }
        _s << "\n  T" << pair.first << ": " << pair.second.size()
           << " paths, " << total_mm << " mm"
           << " width=[" << w_min << ".." << w_max << "]mm";
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
    // NEOTKO_COLORSTITCH_TAG — s118 (B, unificación): leer el perimeter_override de
    // la MISMA fuente que Fill.cpp (línea ~1559): el SurfacePassStack del blob del
    // perfil (stack_top_json/penu_json). El painter (s118 A) ahora escribe ese flag
    // en el stack; payload_from_stacks NO lo vuelca a p->multipass.kv, así que el
    // check legacy de abajo nunca lo veía → el prepass de ToolOrdering divergía de
    // Fill. Una sola fuente: el stack. p->multipass.kv queda como fallback para
    // perfiles pintados legacy (Fase G) anteriores a la unificación de stacks.
    const std::pair<int, bool> zones[2] = { {top_slot, false}, {penu_slot, true} };
    for (const auto& [slot, is_penu] : zones) {
        if (slot <= 0) continue;
        const int pid = profile_id_for_slot(po, slot);
        if (!pid) continue;
        const SurfaceEffectProfile* p = mgr.find(pid);
        if (!p) continue;
        const std::string& js = is_penu ? p->stack_penu_json : p->stack_top_json;
        const SurfacePassStack st = SurfacePassStack::from_json(js);
        // NEOTKO_SANDWICH_TAG s119 (EMPTY model): content-driven, not `enabled`.
        if (st.any_effect() && st.perimeter_override)
            return true;
        // Fallback legacy (perfiles Fase G sin stack json).
        if (p->multipass.present && painted_perim_override_from_profile(p->multipass))
            return true;
    }
    // NEOTKO_STICKER_TAG — a sticker's stack can carry perimeter_override too;
    // this feeds the SAME ToolOrdering prepass (fill_wipe_tower_partitions-adjacent
    // MixedFilament pre-block) that the painted scan above feeds, so a sticker-only
    // object must be scanned here as well (else that prepass silently misses it —
    // same class of Plan/Emisión desync the s118 fix above closed for paint).
    const ModelObject* mo = po->model_object();
    if (mo && !mo->colormix_stickers.empty()) {
        for (const bool is_penu : { false, true }) {
            const double z_lo = is_penu ? print_z : (print_z - height);
            const double z_hi = is_penu ? (print_z + height) : print_z;
            for (size_t si : enumerate_stickers_in_z_range(po, z_lo, z_hi)) {
                const ColorMixSticker& stk = mo->colormix_stickers[si];
                const SurfaceEffectProfile* p = mgr.find(stk.profile_id);
                if (!p) continue;
                const std::string& js = is_penu ? p->stack_penu_json : p->stack_top_json;
                const SurfacePassStack st = SurfacePassStack::from_json(js);
                if (st.any_effect() && st.perimeter_override)
                    return true;
            }
        }
    }
    return false;
}

// NEOTKO_COLORSTITCH_TAG — read the painted profile's ColorStitch fill-angle (deg, >=0)
// or -1 (auto). Same source as the GUI weave preview (colorstitch_top_kv): prefer the
// resolved stack's ColorMix pass kv, fall back to the raw colormix payload.
int SurfaceColorMix::painted_colormix_angle_for_slot(const PrintObject* po, int slot, bool penu)
{
    if (po == nullptr || slot <= 0) return -1;
    const int pid = profile_id_for_slot(po, slot);
    return colormix_angle_for_profile_id(pid, penu);
}

// NEOTKO_STICKER_TAG — factored out of painted_colormix_angle_for_slot so a
// sticker (which has a profile id but no slot) can share the same lookup.
int SurfaceColorMix::colormix_angle_for_profile_id(int profile_id, bool penu)
{
    if (!profile_id) return -1;
    const SurfaceEffectProfile* p = Slic3r::SurfaceEffectProfileManager::get().find(profile_id);
    if (!p) return -1;
    auto read = [penu](const std::map<std::string, std::string>& kv) -> int {
        // Penu passes carry the penu-prefixed key; fall back to the plain key.
        auto it = kv.find(penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle");
        if (it == kv.end() && penu) it = kv.find("interlayer_colormix_angle");
        if (it != kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return -2;   // key absent
    };
    const std::string& js = penu ? p->stack_penu_json : p->stack_top_json;
    if (!js.empty()) {
        const SurfacePassStack st = SurfacePassStack::from_json(js);
        for (const SurfacePass& sp : st.passes)
            if (sp.kind == SurfacePassKind::ColorMix && !sp.colormix.kv.empty()) {
                const int a = read(sp.colormix.kv);
                if (a != -2) return a;
            }
    }
    if (p->colormix.present && !p->colormix.kv.empty()) {
        const int a = read(p->colormix.kv);
        if (a != -2) return a;
    }
    return -1;
}
// NEOTKO_PROFILE_TAG_END



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

// NEOTKO_COLORSTITCH_TAG s108 — physical sanity clamp on (floor_mm, mid_end_mm).
// Verbatim port of the SandwichDialog's pb_apply_constraints (Tab.cpp), promoted
// here so the painter pro-mode tray and the dialog share one rule set.
//   Half: mid_end_mm is the layer top; there is no cap. Forced mid = H.
//   Full: cap = top 0.04 mm of flow → mid_end ≤ H − 0.04. Ramp must exist
//         (mid > floor strictly); equal values would mean two flat layers,
//         which is the MultiPass use-case, not PathBlend.
void PathBlendPassConfig::apply_constraints(double layer_height_mm)
{
    constexpr float kRampGap = 0.001f;  // strict mid > floor (anti-equality)
    constexpr float kCapMin  = 0.04f;   // Full: cap flow minimum on top
    const double H = std::max(0.04, layer_height_mm);
    if (mode == Mode::Full) {
        const float floor_max = std::max(0.01f, (float)H - kCapMin - kRampGap);
        floor_mm   = std::clamp(floor_mm, 0.01f, floor_max);
        const float mid_min = floor_mm + kRampGap;
        const float mid_max = std::max(mid_min, (float)H - kCapMin);
        // Sentinel <0 ⇒ auto default: ramp the whole layer (tallest legal ramp).
        if (mid_end_mm < 0.f) mid_end_mm = mid_max;
        mid_end_mm = std::clamp(mid_end_mm, mid_min, mid_max);
    } else {  // Half
        const float floor_max = std::max(0.01f, (float)H - kRampGap);
        floor_mm   = std::clamp(floor_mm, 0.01f, floor_max);
        mid_end_mm = (float)H;  // ramp goes to layer top; no cap
    }
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
    PathBlendPassConfig c;  // defaults: Full, floor=0.01, mid_end=auto (<0 ⇒ H-0.04)
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
            c.mid_end_mm  = static_cast<float>(gf("mid_end_mm", -1.0));  // <0 ⇒ auto
            c.tool_bottom = gi("tool_bottom", 0);
            c.tool_top    = gi("tool_top",    1);
            c.ease_mode   = std::clamp(gi("ease_mode", 0), 0, 3);
            c.fill_angle  = gi("fill_angle", -1);
            // Hard constraint: floor >= 0.01.
            if (c.floor_mm   < 0.01f)         c.floor_mm   = 0.01f;
            // Keep the auto sentinel (<0) intact; only clamp real values.
            if (c.mid_end_mm >= 0.f && c.mid_end_mm < c.floor_mm)
                c.mid_end_mm = c.floor_mm;
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

// NEOTKO_SANDWICH_TAG s119 (EMPTY model) — "does this zone do anything?".
// A kind None pass is an explicit passthrough (no effect); a stack whose passes
// are all None is GCode-equivalent to a vanilla surface. This is the single
// authority used to gate effect emission, replacing the legacy `enabled` flag.
bool SurfacePassStack::any_effect() const
{
    for (const SurfacePass& p : passes)
        if (p.kind != SurfacePassKind::None) return true;
    return false;
}

std::string SurfacePassStack::to_json() const
{
    // NEOTKO_SANDWICH_TAG s119 (EMPTY model). "No effect on this zone" is encoded
    // exactly ONE way: an explicit passthrough = a single None pass. Three states:
    //   * passes.empty()                → "" (UNAUTHORED → synthesize_from_legacy
    //                                     stays authoritative for untouched presets;
    //                                     preserves the s70 universal-vanilla rule).
    //   * authored but no effect        → canonical [None] blob (a legacy
    //     (!enabled || !any_effect())     enabled=false stack, or an all-None stack).
    //                                     It SURVIVES the round-trip, so resolve()
    //                                     never re-synthesizes and the preset never
    //                                     captures the zone (root of s114→s118).
    //   * authored with effect          → full serialization.
    // The old `if (!enabled || passes.empty()) return "";` cut collapsed the middle
    // state into "" — that ambiguity WAS the penu-capture / disable-sticky bug.
    if (passes.empty())
        return std::string();

    if (!enabled || !any_effect()) {
        nlohmann::json root;
        root["v"]                  = 1;
        root["enabled"]            = false;
        root["perimeter_override"] = perimeter_override;
        root["bottom_supported_control"] = bottom_supported_control;
        nlohmann::json arr = nlohmann::json::array();
        nlohmann::json e;
        e["kind"]  = static_cast<int>(SurfacePassKind::None);
        e["ratio"] = 1.0;
        arr.push_back(std::move(e));
        root["passes"] = std::move(arr);
        return root.dump();
    }

    nlohmann::json root;
    root["v"]                  = 1;
    root["enabled"]            = enabled;
    root["perimeter_override"] = perimeter_override;
    root["bottom_supported_control"] = bottom_supported_control;
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
    // NEOTKO_BOTTOM_TAG — Fase 1 §5.3: per-zone supported-bottom control opt-in.
    if (root.contains("bottom_supported_control") && root["bottom_supported_control"].is_boolean())
        st.bottom_supported_control = root["bottom_supported_control"].get<bool>();

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

    // NEOTKO_SANDWICH_TAG s119 (EMPTY model) — migration A (lazy upcast, logged).
    // A legacy "disabled but populated" stack (enabled=false + real effect passes)
    // meant "no effect on this zone". Normalize it to the canonical explicit
    // passthrough so it can never re-trigger synthesize_from_legacy / preset
    // capture downstream. An already-canonical [None] passthrough
    // (any_effect()==false) is left untouched → no log spam on every load.
    if (!st.enabled && st.any_effect()) {
        NEOTKO_LOG(MULTIPASS, "SANDWICH from_json s119 upcast: enabled=false + "
            << st.passes.size() << " effect pass(es) → explicit [None] passthrough");
        st.passes.clear();
        SurfacePass none_pass;
        none_pass.kind  = SurfacePassKind::None;
        none_pass.ratio = 1.0;
        st.passes.push_back(std::move(none_pass));
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
    // Sentinel <0 ⇒ auto: resolve to the tallest legal ramp before clamping, so
    // a default (untouched) PathBlend ramps the full layer instead of collapsing
    // to floor (a flat, non-planar pass).
    const float mid_pref   = (pb.mid_end_mm < 0.f)
        ? ((pb.mode == PathBlendPassConfig::Mode::Full)
               ? static_cast<float>(H - 0.04) : static_cast<float>(H))
        : pb.mid_end_mm;
    const float mid_end_mm = std::max(floor_mm,
        (pb.mode == PathBlendPassConfig::Mode::Full)
            ? std::min(mid_pref, static_cast<float>(H - 0.04))
            : std::min(mid_pref, static_cast<float>(H)));

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
    // NEOTKO_PATHBLEND_TAG — s87 GEOM diagnostic: the s86 gap_boost quickfix was
    // retired (mathematically ill-conditioned: multiplied a flow that was already
    // tiny where the gap is widest). The real fix is the "B-bands" model: split
    // the ramp into K real micro-layers each with its own Flow.with_height() and
    // recomputed spacing, plus a band-by-band cap. This block stays as the
    // legacy K==1 fallback until the bands path is wired.
    NEOTKO_LOG(MULTIPASS,
        "PATHBLEND_GEOM"
        << " nominal_z=" << nominal_z
        << " H=" << H
        << " t=" << t
        << " ramp_thickness=" << ramp_thickness_clamped
        << " spacing_nominal_for_H=" << (path.width - H * (1.0 - 0.25 * 3.14159265358979))
        << " spacing_would_be_at_h_ramp=" << (path.width - ramp_thickness_clamped * (1.0 - 0.25 * 3.14159265358979))
        << " pass=" << pass_idx
        << " flow=" << flow);
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

// NEOTKO_PATHBLEND_TAG_START — s87 B-bands model implementation.
// Discretize the ramp into K real micro-layers. Strategy:
//   1. Clamp floor/mid_end against H and min_band_h (a band thinner than
//      min_band_h is unprintable — reabsorb upward).
//   2. K_ramp = floor((mid_end - floor) / min_band_h). If K_ramp < 1 → return
//      empty (caller uses legacy K==1 path).
//   3. Distribute the ramp range [floor, mid_end] in K_ramp equal-height steps.
//      Band k: t in [k/K, (k+1)/K], h_step = (mid_end - floor) / K_ramp,
//      z_top = bottom_z + floor + (k+1) * h_step.
//   4. If want_cap: emit a matching set of K_cap == K_ramp cap-bands, each at
//      Z = bottom_z + H, h_cap = H - ramp_at_t_mid (the residual hole height).
//      Cap-bands print bottom-to-top of nominal-z but at the SAME Z; their
//      h_step varies so the regenerated Flow gives a width/spacing matched to
//      the residual hole each band covers.
std::vector<PBBand> compute_pb_bands(
    const PathBlendPassConfig& pb,
    double                     bottom_z,
    double                     H,
    double                     min_band_h,
    bool                       want_cap,
    int                        target_k)
{
    std::vector<PBBand> out;
    if (H <= 0.0 || min_band_h <= 0.0) return out;

    // Clamp config values against physical limits. The PathBlend UX already
    // enforces floor >= 0.01 and (Full) mid_end <= H - 0.04; min_band_h is a
    // last-resort safety floor on per-band h_step.
    const float floor_mm   = std::max(static_cast<float>(min_band_h), std::max(0.01f, pb.floor_mm));
    const float mid_end_mm = (pb.mode == PathBlendPassConfig::Mode::Full)
        ? std::min(pb.mid_end_mm, static_cast<float>(H - min_band_h))
        : std::min(pb.mid_end_mm, static_cast<float>(H));
    if (mid_end_mm <= floor_mm) return out;             // degenerate → legacy path
    const double range_mm = double(mid_end_mm) - double(floor_mm);

    // How many bands? Natural K from range/min_band_h, or forced via target_k.
    // When forced, we still cap by what min_band_h permits so an over-ambitious
    // target_k doesn't produce unprintable sub-min_band_h slices.
    const int K_natural = std::max(1, static_cast<int>(std::floor(range_mm / min_band_h)));
    const int K_ramp    = (target_k > 0) ? std::min(target_k, K_natural) : K_natural;
    if (K_ramp < 1) return out;

    const double h_step  = range_mm / double(K_ramp);
    const double t_step  = 1.0 / double(K_ramp);

    // Ramp bands — ascending Z, single tool (pb.tool_bottom assumed by caller).
    for (int k = 0; k < K_ramp; ++k) {
        PBBand b;
        b.is_cap = false;
        b.t_lo   = static_cast<float>(k * t_step);
        b.t_hi   = static_cast<float>((k + 1) * t_step);
        b.t_mid  = 0.5f * (b.t_lo + b.t_hi);
        // ramp(t_mid) ≈ floor + t_mid * (mid_end - floor); the top of band k is
        // at z = bottom_z + floor + (k+1) * h_step (equivalent to the analytic
        // ramp value at t = t_hi when range/K equals h_step exactly).
        b.z_top  = static_cast<float>(bottom_z + double(floor_mm) + double(k + 1) * h_step);
        b.h_step = static_cast<float>(h_step);
        out.push_back(b);
    }

    if (!want_cap) return out;

    // Cap bands — Full mode only. All bands at Z = bottom_z + H but each with
    // its own h_cap matching the residual hole over its ramp counterpart.
    for (int k = 0; k < K_ramp; ++k) {
        const PBBand& r = out[k];
        const double ramp_at_mid = double(floor_mm) + double(r.t_mid) * range_mm;
        const double h_cap_mm    = std::max(min_band_h, H - ramp_at_mid);
        PBBand c;
        c.is_cap = true;
        c.t_lo   = r.t_lo;
        c.t_hi   = r.t_hi;
        c.t_mid  = r.t_mid;
        c.z_top  = static_cast<float>(bottom_z + H);
        c.h_step = static_cast<float>(h_cap_mm);
        out.push_back(c);
    }
    return out;
}
// NEOTKO_PATHBLEND_TAG_END


} // namespace Slic3r
