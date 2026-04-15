#ifndef slic3r_SurfaceColorMix_hpp_
#define slic3r_SurfaceColorMix_hpp_

// NEOTKO_COLORMIX_TAG_START
// Neotko Surface ColorMix Feature
// Multi-tool distribution for top/penultimate surface layers
// Author: Neotko
// NEOTKO_COLORMIX_TAG_END

#include "libslic3r.h"
#include "ExtrusionEntity.hpp"
#include "PrintConfig.hpp"
#include "MixedFilament.hpp"
#include <vector>
#include <map>
#include <string>
#include <sstream>    // NEOTKO_DEBUG: NEOTKO_LOG macro uses std::ostringstream
#include <functional> // NEOTKO_NEOWEAVING: std::function for point_to_gcode callback

namespace Slic3r {

// NEOTKO_DEBUG_TAG_START
// Centralised debug infrastructure for all Neotko features.
// Env vars (set before launching the slicer):
//   ORCA_DEBUG_COLORMIX     — Surface ColorMix assign/group logic
//   ORCA_DEBUG_MULTIPASS    — MultiPass CAMINO 1/2 fill generation
//   ORCA_DEBUG_PENULTIMATE  — Penultimate surface classification pipeline
//   ORCA_DEBUG_TOOLORDER    — ToolOrdering ColorMix/MultiPass extruder registration
//   ORCA_DEBUG_ZBLEND       — ZBlend sub-layer computation
//   ORCA_DEBUG_ALL          — Enable every channel at once
// Log files: /tmp/neotko_{colormix|multipass|penultimate|toolorder|zblend}.log
namespace NeoDebug {
    enum Channel : int {
        COLORMIX    = 0,
        MULTIPASS   = 1,
        PENULTIMATE = 2,
        TOOLORDER   = 3,
        ZBLEND      = 4,
        CH_COUNT    = 5
    };
    // Returns true if the channel is active (env var set, or ORCA_DEBUG_ALL set).
    // Cheap after first call (static flag per channel).
    bool enabled(Channel c);
    // Append msg + newline to the channel's log file (thread-safe).
    void write(Channel c, const std::string& msg);
} // namespace NeoDebug
// NEOTKO_DEBUG_TAG_END

class PrintRegionConfig;
class ExtrusionEntityCollection;

// NEOTKO_COLORMIX_TAG_START
// Represents one selectable option in the ColorMix pattern picker UI.
struct ColorMixOption {
    std::string label;          // "Mixed (F3+F4)"  or  "F1"
    std::string pattern;        // "12", "1221", "123" etc.
    std::string display_color;  // "#RRGGBB" blended or filament color
    bool        is_physical = false;
};

// assign_and_group_tools return flags
// Bit 0: at least one path was split and tool-encoded.
// Bit 1: at least one fill could not be split (monotonic pattern — not splittable).
static constexpr int COLORMIX_FLAG_MODIFIED      = 1;
static constexpr int COLORMIX_FLAG_UNSPLITTABLE  = 2;
// NEOTKO_COLORMIX_TAG_END

class SurfaceColorMix {
public:
    // Main entry point. Called from Fill.cpp::make_fills() after surface fill generation.
    // Splits top/penultimate surface paths into individual lines and groups them by tool
    // according to the pattern string (interlayer_colormix_pattern_top / _penultimate).
    // allow_top / allow_penu: zone filter from Fill.cpp call site — false skips that role.
    // Returns int flags: bit 0 = any path modified, bit 1 = unsplittable fill found.
    static int assign_and_group_tools(
        ExtrusionEntityCollection& fills,
        const PrintRegionConfig&   config,
        ExtrusionRole              role,
        int                        layer_idx,
        bool                       allow_top  = true,
        bool                       allow_penu = true
    );

    // Check if role matches the surface filter setting.
    // surface: 0=Both, 1=Top only, 2=Penultimate only (kColormixSurface_* constants)
    static bool should_process_role(ExtrusionRole role, int surface);

    // Encode tool index in mm3_per_mm: original + (tool_idx + 1) * 10.0
    // Decode in GCode.cpp: tool = floor(mm3_per_mm / 10.0) - 1
    static void encode_tool_in_path(ExtrusionPath* path, int tool_idx);

    // NEOTKO_COLORMIX_TAG_START - MixedFilament UI helpers
    static std::vector<ColorMixOption> get_mix_options(
        const std::string&              mixed_defs,
        const std::vector<std::string>& filament_colours);

    static std::string mixed_filament_to_pattern(const MixedFilament& mf);
    // NEOTKO_COLORMIX_TAG_END

private:
    static void debug_log(
        int layer_idx,
        const std::vector<int>& tools,
        const std::map<int, std::vector<ExtrusionPath*>>& grouped
    );
};
// NEOTKO_COLORMIX_TAG_END

// NEOTKO_MULTIPASS_TAG_START
// Neotko MultiPass Blend Feature
// Re-prints top/penultimate surface N times with different tools + reduced line width.
// Runs BEFORE SurfaceColorMix in Fill.cpp::make_fills().
//
// CAMINO 1 (current — no combination with ColorMix):
//   MultiPass encodes tool in mm3_per_mm (same trick as ColorMix).
//   ColorMix automatically skips already-encoded paths (mm3_per_mm >= 10.0 guard).
//   Result: MultiPass and ColorMix are mutually exclusive per surface.
//
// CAMINO 2 (future — full combination):
//   MultiPass clones paths WITHOUT tool encoding (only applies width_ratio).
//   ColorMix then runs on each cloned pass and assigns tools per-line within it.

struct MultiPassConfig {
    bool        enabled        = false;
    int         surface        = 0;             // 0=Both, 1=Top only, 2=Penultimate only
    int         num_passes     = 2;
    int         tool[3]        = {0, 1, -1};    // -1 = pass disabled
    double      width_ratio[3] = {0.50, 0.50, 0.34};
    bool        vary_pattern   = false;
    int         angle[3]       = {-1, -1, -1};  // -1 = auto (follow fill angle), 0-359 = custom
    // Per-pass GCode injection
    int         fan[3]         = {-1, -1, -1};       // 0-255, -1=no change
    int         speed_pct[3]   = {100, 100, 100};     // 1-200 via M220
    std::string gcode_start[3] = {"", "", ""};
    std::string gcode_end[3]   = {"", "", ""};
    static MultiPassConfig from_region_config(const PrintRegionConfig& cfg);
};

// NEOTKO_PATHBLEND_TAG_START — MultiPathBlend: independent gradient blend system
struct PathBlendPassConfig {
    bool    enabled        = false;
    int     surface        = 0;      // 0=both, 1=top, 2=penultimate
    int     num_passes     = 2;
    int     tool[4]        = {0, 1, 2, 3};
    float   layer_ratio[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // ratio at the active extreme of each pass
    float   min_ratio      = 0.05f;  // minimum extrusion ratio for the leading pass at t=0
    bool    invert_gradient = true;  // invert t for z calc → ascending z during print (safe)
    int     fill_angle      = -1;    // -1 = follow top surface angle; 0..359 = override

    // Compute the extrusion ratio for pass `p` at normalized surface position `t` in [0,1].
    // 2 passes: r0(t)=1-t, r1(t)=t
    // 3 passes: r0=max(0,1-2t), r1=1-|2t-1|, r2=max(0,2t-1)
    // 4 passes: linear-hat per tramo, peaks at t=0, 0.333, 0.667, 1.0
    // min_ratio: applied to pass 0 (dominant at t=0) so it never goes below min_ratio.
    double ratio_at(int p, double t) const;

    // Build from PrintRegionConfig (reads pathblend_* keys).
    static PathBlendPassConfig from_region_config(const PrintRegionConfig& cfg);
};
// NEOTKO_PATHBLEND_TAG_END

class SurfaceMultiPass {
public:
    // Called from Fill.cpp::make_fills() BEFORE SurfaceColorMix::assign_and_group_tools().
    // Clones all matching surface paths N times, each with width_ratio[i] and tool[i] encoded.
    // ColorMix skips these paths automatically (mm3_per_mm >= 10.0 guard — CAMINO 1).
    // allow_top / allow_penu: zone filter from Fill.cpp call site — false skips that role.
    static bool apply(
        ExtrusionEntityCollection& fills,
        const PrintRegionConfig&   config,
        int                        layer_idx,
        bool                       allow_top  = true,
        bool                       allow_penu = true
    );
};
// NEOTKO_MULTIPASS_TAG_END

// NEOTKO_NEOWEAVING_TAG_START
// Neotko Neoweaving — Z-axis interdigitation during extrusion.
// Invented by Neotko (creator of Ironing / Neosanding).
//
// Two modes:
//   Wave   — sinusoidal Z oscillation per micro-segment along each line.
//   Linear — alternating flat Z per full line (+A / 0 on alternate lines/layers).
//
// Roles processed:
//   erTopSolidInfill     — always (if surface filter matches)
//   erPenultimateInfill  — always (top-derived)
//   erSolidInfill        — only in Linear mode when neoweave_filter == All
//   erInternalInfill     — only via infill_neoweave_enabled override
//
// Called from GCode.cpp _extrude() via NeoweaveEngine::needs_weave() and ::apply_path().
// Point-to-gcode conversion is delegated back to GCode.cpp via the point_to_gcode callback
// so this class never depends on GCode's coordinate system directly.

// Forward declarations (avoid pulling GCodeWriter.hpp into the public header)
class GCodeWriter;
struct ExtrusionPath;

class NeoweaveEngine {
public:
    // Returns true if neoweaving should apply to this path.
    // When true, the caller MUST skip arc-fitting and use G1 extrusion.
    static bool needs_weave(const ExtrusionPath& path, const PrintRegionConfig& cfg);

    // Apply neowave to a complete ExtrusionPath (all lines in its polyline).
    // Appends to gcode_out. Both Wave and Linear modes handled.
    // Does NOT include the final Z-restore after the path; call restore_z() after.
    //
    // Parameters:
    //   path              — path to extrude (polyline + role + width)
    //   cfg               — region config (mode, amplitude, period, etc.)
    //   writer            — GCodeWriter for emit helpers (extrude_to_xy/xyz, get_position)
    //   layer_index       — m_layer_index (parity used for Linear mode)
    //   nominal_z         — m_nominal_z (layer base Z)
    //   F                 — current print speed (mm/min)
    //   e_per_mm          — extrusion per mm for this path
    //   is_force_no_extr  — pass-through path flag
    //   point_to_gcode    — converts Slic3r Point → Vec2d GCode coords (lambda from GCode.cpp)
    static std::string apply_path(
        const ExtrusionPath&                       path,
        const PrintRegionConfig&                   cfg,
        GCodeWriter&                               writer,
        int                                        layer_index,
        double                                     nominal_z,
        double                                     F,
        double                                     e_per_mm,
        bool                                       is_force_no_extr,
        const std::function<Vec2d(const Point&)>&  point_to_gcode
    );

    // Restore the nozzle to nominal_z after a weaving path.
    // Linear mode: emits a G1 Z move at path speed F (NOT travel speed).
    // Wave mode:   emits travel_to_z (speed already capped via weave_F).
    static std::string restore_z(
        const PrintRegionConfig& cfg,
        GCodeWriter&             writer,
        double                   nominal_z,
        double                   F,
        bool                     surface_weave_active  // true=top/penultimate, false=infill
    );
};
// NEOTKO_NEOWEAVING_TAG_END

// NEOTKO_MULTIPASS_TAG_START — PathBlend: Z+flow gradient intra-path
class PathBlendEngine {
public:
    // Returns true if PathBlend should apply to this path.
    // Requires multipass_path_gradient + multipass_enabled + top/solid role.
    // When true, caller MUST skip arc-fitting and use PathBlendEngine::apply_path().
    static bool needs_blend(const ExtrusionPath& path, const PrintRegionConfig& cfg);

    // Emit a PathBlend path.
    //   nominal_z    — m_nominal_z (top of current layer)
    //   layer_height — m_layer->height
    //   F            — current print speed (mm/min) — used for Z step moves
    //   pass_idx     — 0 = T0 pass (Z steps down, flow = surface_t)
    //                  1+ = T1 pass (Z stays at nominal, complementary flow)
    //   surface_t    — position of this path within the surface [0..1]
    //                  0 = first path (T1 dominates), 1 = last path (T0 dominates)
    //                  computed geometrically by caller from path centroid / layer bbox
    // Pass 0 restores Z to nominal_z before returning.
    static std::string apply_path(
        const ExtrusionPath&                      path,
        const PrintRegionConfig&                  cfg,
        GCodeWriter&                              writer,
        double                                    nominal_z,
        double                                    layer_height,
        double                                    F,
        double                                    e_per_mm,
        int                                       pass_idx,
        double                                    surface_t,
        const std::function<Vec2d(const Point&)>& point_to_gcode
    );
};
// NEOTKO_MULTIPASS_TAG_END

} // namespace Slic3r

// NEOTKO_DEBUG_TAG_START
// NEOTKO_LOG(CHANNEL, stream_expr) — write a debug line to a channel's log file.
// Usage (from any function inside namespace Slic3r):
//   NEOTKO_LOG(COLORMIX,    "layer=" << layer_idx << " fills=" << n);
//   NEOTKO_LOG(MULTIPASS,   "pass" << i << " tool=T" << t << " ratio=" << r);
//   NEOTKO_LOG(PENULTIMATE, "layer=" << idx << " pen_polys=" << n);
//   NEOTKO_LOG(TOOLORDER,   "extruder " << e << " added for colormix");
//   NEOTKO_LOG(ZBLEND,      "sublayer z=" << z << " height=" << h);
// For multi-line blocks: if (NeoDebug::enabled(NeoDebug::CHANNEL)) { oss; NeoDebug::write(...); }
#define NEOTKO_LOG(channel, body)                               \
    do {                                                        \
        if (NeoDebug::enabled(NeoDebug::channel)) {             \
            std::ostringstream _ndbg_;                          \
            _ndbg_ << body;                                     \
            NeoDebug::write(NeoDebug::channel, _ndbg_.str());   \
        }                                                       \
    } while (0)
// NEOTKO_DEBUG_TAG_END

#endif // slic3r_SurfaceColorMix_hpp_
