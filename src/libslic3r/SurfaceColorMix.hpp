#ifndef slic3r_SurfaceColorMix_hpp_
#define slic3r_SurfaceColorMix_hpp_

// NEOTKO_COLORMIX_TAG_START
// Neotko Surface ColorMix Feature
// Multi-tool distribution for top/penultimate surface layers
// Author: Neotko
// NEOTKO_COLORMIX_TAG_END

#include "libslic3r.h"
#include "ExPolygon.hpp"  // NEOTKO_PROFILE_TAG — Fase 6c: painted_footprint_in_z_range returns ExPolygons
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp" // NEOTKO_SANDWICH_TAG — eec_to_tool_buckets() returns EEC by value
#include "PrintConfig.hpp"
#include "MixedFilament.hpp"
#include "SurfaceEffectProfile.hpp"  // NEOTKO_SANDWICH_TAG — SurfaceEffectPayload by value in SurfacePass
#include "SurfacePassKind.hpp"   // NEOTKO_SANDWICH_TAG — extracted enum (was inline)
#include "NeoDebug.hpp"           // NEOTKO_DEBUG_TAG — extracted NeoDebug namespace
#include "PathBlendRuntime.hpp"   // NEOTKO_PATHBLEND_TAG — extracted scheduler/dispatcher runtimes
#include <utility>  // NEOTKO_SANDWICH_TAG — std::pair in eec_to_tool_buckets() return type
#include <vector>
#include <map>
#include <string>
#include <sstream>    // NEOTKO_DEBUG: NEOTKO_LOG macro uses std::ostringstream
#include <functional> // NEOTKO_NEOWEAVING: std::function for point_to_gcode callback
#include <algorithm>  // NEOTKO_COLORMIX s58: std::sort/min/max in lane mode helpers
#include <cmath>      // NEOTKO_COLORMIX s58: std::atan2, std::sqrt, std::abs
#include <limits>     // NEOTKO_COLORMIX s58: std::numeric_limits

namespace Slic3r {

// NEOTKO_DEBUG_TAG_START
// Centralised debug infrastructure for all Neotko features.
// Env vars (set before launching the slicer):
//   ORCA_DEBUG_COLORMIX     — Surface ColorMix assign/group logic
//   ORCA_DEBUG_MULTIPASS    — MultiPass CAMINO 1/2 fill generation
//   ORCA_DEBUG_PENULTIMATE  — Penultimate surface classification pipeline
//   ORCA_DEBUG_TOOLORDER    — ToolOrdering ColorMix/MultiPass extruder registration
//   ORCA_DEBUG_ZBLEND       — ZBlend sub-layer computation
//   ORCA_DEBUG_PROFILE      — Surface Effect Profile / 3D Painter pipeline
//                             (manager add/remove, 3mf I/O, painter UI,
//                              Fase D painted-slot resolution + gv override)
//   ORCA_DEBUG_ALL          — Enable every channel at once
// Log files: /tmp/neotko_{colormix|multipass|penultimate|toolorder|zblend|wipetower|profile}.log
// NEOTKO_DEBUG_TAG_END

class PrintRegionConfig;
class ExtrusionEntityCollection;
class PrintObject;            // NEOTKO_PROFILE_TAG
class Surface;                // NEOTKO_PAINT_COEXIST_TAG s91 — mmu_governs_surface overload
class ModelObject;            // NEOTKO_PROFILE_TAG
struct ColorMixSticker;       // NEOTKO_STICKER_TAG — sticker helpers (defined in Model.hpp)
struct SurfaceEffectProfile;  // NEOTKO_PROFILE_TAG
struct SurfaceEffectPayload;  // NEOTKO_PROFILE_TAG — Fase F
struct MultiPassConfig;       // NEOTKO_PROFILE_TAG — Fase F (defined below)
struct PathBlendPassConfig;   // NEOTKO_PROFILE_TAG — Fase G (defined below)

// NEOTKO_COLORMIX_TAG_START
// Represents one selectable option in the ColorMix pattern picker UI.
struct ColorMixOption {
    std::string label;          // "Mixed (F3+F4)"  or  "F1"
    std::string pattern;        // "12", "1221", "123" etc.
    std::string display_color;  // "#RRGGBB" blended or filament color
    bool        is_physical = false;
    int         filament_id = 0; // 1-based: 1..N = physical, N+1.. = virtual mixed
    // tool_weights: 0-based physical tool index → normalized weight [0..1].
    // Only populated for virtual (is_physical=false) options.
    // Used by MultiPass "Normalize to MixedColor %" to set layer_ratio per pass.
    std::map<int,float> tool_weights;
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
    // mgr / num_physical: optional MixedFilament manager for virtual-digit recipe expansion.
    //   When mgr != nullptr and use_virtual is ON, digits '5'-'9' expand to physical tools
    //   (component_a + component_b of the named virtual filament). Physical indices are
    //   encoded directly — GCode decode needs no per-layer virtual resolution.
    // Returns int flags: bit 0 = any path modified, bit 1 = unsplittable fill found.
    static int assign_and_group_tools(
        ExtrusionEntityCollection&  fills,
        const PrintRegionConfig&    config,
        ExtrusionRole               role,
        int                         layer_idx,
        bool                        allow_top     = true,
        bool                        allow_penu    = true,
        const MixedFilamentManager* mgr           = nullptr,
        size_t                      num_physical  = 0,
        // NEOTKO_PROFILE_TAG — Fase D: per-layer painted-profile override.
        // When `print_object` is non-null, the function checks for triangles
        // painted via the ColorMix Painter at layer Z (top role) or one layer
        // up (penu role); if a dominant slot is found, its profile overrides
        // the preset gradient view for this layer's fills.
        const PrintObject*          print_object  = nullptr,
        double                      layer_print_z = 0.0,
        double                      layer_height  = 0.0,
        // NEOTKO_COLORSTITCH_TAG — cuando el llamador (FASE2 band-loop) YA fusionó el
        // override per-pase (pass.colormix.kv) dentro de `config`, ese config es la
        // fuente de verdad de ESTA lámina. En painter-mode NO se debe re-aplicar
        // encima el payload COLAPSADO del profile (payload_from_stacks funde todos los
        // pases por rol → tool_a del último gana), que pisaba los tools per-pase.
        bool                        config_has_pass_override = false
    );

    // Check if role matches the surface filter setting.
    // surface: 0=Both, 1=Top only, 2=Penultimate only (kColormixSurface_* constants)
    static bool should_process_role(ExtrusionRole role, int surface);

    // NEOTKO_PROFILE_TAG — Fase D painter-mode helpers (shared with ToolOrdering).
    //
    // `object_has_any_colormix_paint`: returns true if any model_part volume of
    // the object has a non-zero slot in its colormix_slot_to_profile_id table.
    // This flips the slicer into "painter mode": preset SCM settings are
    // ignored, only painted profiles drive the effect.
    //
    // `dominant_painted_slot_in_z_range`: scans painted facets and returns the
    // most-painted slot whose upward-facing triangles have max_z inside the
    // provided z range. Returns 0 if none.
    //
    // `profile_id_for_slot`: maps slot index (1..15) → SurfaceEffectProfile id
    // by reading the first model_part's slot table. 0 if unmapped.
    //
    // `painted_profile_tools_1based`: produces the 1-based tool list that the
    // SLICE pipeline would assign for a given profile + role. ToolOrdering
    // calls this to register the same tools the SLICE will use, keeping the
    // wipe-tower plan in sync.
    static bool object_has_any_colormix_paint(const ModelObject* mo);
    // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): `downward` mirrors the scan to the
    // underside. Default false → upward-facing (max_z), byte-identical to the
    // top/penu callers. true → downward-facing (min_z), for bottom surfaces.
    static int  dominant_painted_slot_in_z_range(const PrintObject* po,
                                                  double z_min, double z_max,
                                                  bool downward = false);
    static int  profile_id_for_slot(const PrintObject* po, int slot);

    // NEOTKO_PROFILE_TAG — Fase 6c: XY footprint mask of the painted triangles
    // for a given slot within a Z band, projected to the print-frame XY plane
    // (scaled coords) and unioned. Used by FASE 2 to clip a painted surface so
    // the sandwich applies ONLY where the user painted (the rest prints natural),
    // preserving the painted shape instead of flooding the whole top surface.
    // Same scan/frame as dominant_painted_slot_in_z_range. Empty if no paint.
    static ExPolygons painted_footprint_in_z_range(const PrintObject* po, int slot,
                                                    double z_min, double z_max,
                                                    bool downward = false);

    // NEOTKO_PROFILE_TAG — Fase 6c v2: every painted slot whose upward-facing
    // triangles fall in the Z band, with at least one triangle. Used to handle
    // twin/multi islands at the same Z painted with DIFFERENT profiles: instead
    // of picking only the dominant slot (v1), each painted slot in the band gets
    // its own footprint mask and its own sandwich; the rest prints natural.
    // Returned in ascending slot order (stable).
    static std::vector<int> enumerate_painted_slots_in_z_range(const PrintObject* po,
                                                                double z_min, double z_max,
                                                                bool downward = false);

    // NEOTKO_STICKER_TAG — Sandwich Sticker helpers (SVG masks, no facets).
    //
    // `object_has_any_colormix_stickers`: true if the object carries at least
    // one sticker whose profile still exists in the manager (ghost stickers —
    // pid 0 or dangling — are ignored, mirroring the s137b slot filter). ORed
    // with object_has_any_colormix_paint at every painter-mode gate.
    //
    // `enumerate_stickers_in_z_range`: indices into mo->colormix_stickers whose
    // anchor point (sticker-local origin, composed through trafo_centered — the
    // s161 lesson applies identically here) lands inside [z_min, z_max] with
    // the same 0.02 fp slack as the painted scans. Returned TOP-DOWN (pile back
    // first): the first entry is the topmost sticker, which occludes the rest.
    //
    // `sticker_footprint_slice_frame`: the sticker's SVG outline as ExPolygons
    // in the slice frame (scaled coords), ready to intersect against
    // surface_fill.expolygons. Parses svg_data per call (nanosvg on a private
    // copy — thread-safe; top layers are few, so no cache needed yet). Empty on
    // parse failure or degenerate transform.
    static bool object_has_any_colormix_stickers(const ModelObject* mo);
    static std::vector<size_t> enumerate_stickers_in_z_range(const PrintObject* po,
                                                             double z_min, double z_max);
    static ExPolygons sticker_footprint_slice_frame(const ColorMixSticker& sticker,
                                                    const PrintObject* po);

    // NEOTKO_STICKER_TAG — shared core: parses `sticker.svg_data` (nanosvg on a
    // private copy) and projects every ring (contours CCW + holes CW) through
    // `to_target`, returning them as scaled Polygons (mm → clipper int), NOT
    // unioned. `sticker_footprint_slice_frame` composes `to_target =
    // po->trafo_centered() * sticker.transform` and unions the result into the
    // slice-mask ExPolygons; the GUI edit-mode overlay (GLGizmoColorMixPainter)
    // calls this directly with a world/GL transform instead, so the SVG
    // parsing + projection logic lives in exactly one place. Empty on parse
    // failure. A degenerate/mirrored `to_target` (negative XY determinant)
    // reverses ring winding so orientation stays consistent either way.
    static Polygons sticker_rings_in_transform(const ColorMixSticker& sticker,
                                               const Transform3d& to_target);

    // NEOTKO_PAINT_COEXIST_TAG s91 — MMU governance helpers.
    //
    // `mmu_painted_footprint_in_z_range`: XY footprint of MMU-painted triangles
    // (any extruder slot != NONE) whose Z range OVERLAPS [z_min, z_max].
    // Unlike painted_footprint_in_z_range (which filters upward facets), this
    // includes ALL facet orientations so lateral MMU paint also occludes the
    // sandwich at the XY column it covers. Empty if no MMU paint in the band.
    static ExPolygons mmu_painted_footprint_in_z_range(
        const PrintObject* po, double z_min, double z_max);

    // `mmu_governs_xy`: returns true if `surface_xy` intersects the MMU paint
    // footprint at this layer's vertical slab. SINGLE source of truth consulted
    // identically by SLICE (SurfaceColorMix, Fill) and ToolOrdering — any
    // divergence triggers wipe-tower "append_tcr unexpected" crashes.
    // Empty surface_xy / no MMU paint → false (fast path).
    static bool mmu_governs_xy(
        const PrintObject* po,
        const ExPolygons& surface_xy,
        double z_min, double z_max);

    // Convenience overload taking a single Surface (uses surface.expolygon).
    static bool mmu_governs_surface(
        const PrintObject* po,
        const Surface& surface,
        double z_min, double z_max);

    static std::vector<unsigned int> painted_profile_tools_1based(
        const SurfaceEffectProfile& p, bool top_role);

    // NEOTKO_PROFILE_TAG — Fase F painter-mode MultiPass override.
    //
    // `multipass_from_profile_payload`: handwritten kv → MultiPassConfig
    // builder (avoids a per-region-per-layer PrintRegionConfig copy on the
    // hot path). `role == erPenultimateInfill` reads `penultimate_multipass_*`
    // keys; any other role reads top `multipass_*` keys.
    //
    // `painted_perim_override_from_profile`: returns the profile's
    // `multipass_perimeter_override` value (top-role key), defaulting to
    // false if absent. Used by ToolOrdering to decide whether to register
    // mp_perim_override_active in painter mode.
    static MultiPassConfig multipass_from_profile_payload(
        const SurfaceEffectPayload& payload, ExtrusionRole role);
    static bool             painted_perim_override_from_profile(
        const SurfaceEffectPayload& payload);

    // NEOTKO_PROFILE_TAG — Fase F: returns true if any painted profile that
    // covers this layer's top/penu Z range carries
    // `multipass_perimeter_override=true`. ToolOrdering uses this to set
    // `mp_perim_override_active` in painter mode without falling back to the
    // preset region config (which is suppressed under painter mode).
    static bool             any_painted_profile_has_perim_override(
        const PrintObject* po, double print_z, double height);

    // NEOTKO_COLORSTITCH_TAG — fill-angle override (degrees, >=0) carried by the
    // PAINTED profile at `slot`, or -1 (auto). In painter mode the angle lives in the
    // profile's stack (not the region preset), so Fill.cpp must read it from here to
    // honour a fixed ColorStitch angle instead of falling back to the alternating
    // base_angle. `penu` picks the penultimate stack/zone.
    static int              painted_colormix_angle_for_slot(
        const PrintObject* po, int slot, bool penu);

    // NEOTKO_STICKER_TAG — same lookup as `painted_colormix_angle_for_slot`,
    // but by profile id directly (a sticker has no slot). Both share this
    // implementation.
    static int              colormix_angle_for_profile_id(int profile_id, bool penu);

    // NEOTKO_PROFILE_TAG — Fase G painter-mode PathBlend override.
    // Mirror of `multipass_from_profile_payload`. Reads pathblend_* keys
    // from the kv map. PathBlendPassConfig defaults are used for absent
    // keys (struct defaults match PrintConfig defaults).
    static PathBlendPassConfig pathblend_from_profile_payload(
        const SurfaceEffectPayload& payload);

    // NEOTKO_PROFILE_TAG — Penu role autonomy (s66 polish):
    // returns true if ANY painted profile on the object has penultimate
    // activity declared in its payloads. Used by PrintObject's
    // vertical-shells discovery to force-classify penultimate solid
    // surfaces when the preset's `penultimate_top_layers` is 0 but the
    // painter wants them.
    //
    // Activity detection:
    //   - multipass.kv has `penultimate_multipass_enabled` == "1", OR
    //   - colormix.present AND interlayer_colormix_surface ∈ {0, 2}, OR
    //   - pathblend.present AND pathblend_surface ∈ {0, 2}.
    static bool             object_painter_wants_penu(const ModelObject* mo);
    // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): true if any painted profile on the object
    // declares a non-empty Bottom WIP zone (stack_bottom_json with a real effect).
    // Gates the bottom-surface sandwich so untouched objects stay byte-identical.
    static bool             object_painter_wants_bottom(const ModelObject* mo);

    // Encode tool index in mm3_per_mm: original + (tool_idx + 1) * 10.0
    // Decode in GCode.cpp: tool = floor(mm3_per_mm / 10.0) - 1
    static void encode_tool_in_path(ExtrusionPath* path, int tool_idx);

    // NEOTKO_SANDWICH_TAG — Fase 2: decode a ColorMix-encoded EEC into per-tool
    // buckets. `encoded` is the output of assign_and_group_tools() (tool stored
    // in mm3_per_mm via the +(tool+1)*10 trick). Each path is cloned, its real
    // mm3_per_mm restored, and routed to its tool's bucket. Unencoded paths
    // (mm3_per_mm < 10) go to `default_tool`. Buckets are returned in
    // first-appearance (spatial) order. The caller owns the cloned entities.
    static std::vector<std::pair<int, ExtrusionEntityCollection>>
    eec_to_tool_buckets(const ExtrusionEntityCollection& encoded, int default_tool);

    // NEOTKO_COLORMIX_TAG — s60 numeric gradient.
    //
    // Easing curves applied to the position fraction t ∈ [0,1] BEFORE the dither
    // decision. The curve shapes WHERE in the gradient the colour transitions
    // happen, not WHETHER they happen (the per-window frequency is preserved
    // globally — easing redistributes locally).
    enum ColormixEasing : int {
        kColormixEasing_Linear      = 0,
        kColormixEasing_EaseIn      = 1,
        kColormixEasing_EaseOut     = 2,
        kColormixEasing_EaseInOut   = 3,
        kColormixEasing_Gamma       = 4,
        kColormixEasing_HardBand    = 5,
    };

    // Apply easing curve to a linear t ∈ [0,1].
    // For Gamma mode, `gamma` is the exponent (1.0 = linear).
    static double colormix_easing_apply(double t, int easing, double gamma = 1.0);

    // Bresenham-style dithered tool sequence for "Linear 2-color" mode.
    //   n_lines : total number of lines to place tools onto (the actual line
    //             count of the surface being processed)
    //   tool_a  : 0-based physical tool index for the majority/start side
    //   tool_b  : 0-based physical tool index for the minority/end side
    //   pct_a   : 0-100 — fraction of lines assigned to tool A
    //   easing  : ColormixEasing enum (default Linear)
    //   gamma   : exponent for kColormixEasing_Gamma (else ignored)
    // Returns a vector<int> of length n_lines.
    static std::vector<int> build_dithered_tools_2color(
        int n_lines, int tool_a, int tool_b, int pct_a,
        int easing = kColormixEasing_Linear, double gamma = 1.0);

    // Bresenham-style dithered sequence for "Linear 3-color" mode.
    // pct_a + pct_b + pct_c = 100 (pct_c = 100 - pct_a - pct_b, clamped >= 0).
    // The gradient morphs A → B → C across the surface; B is concentrated in
    // the middle of the sequence with proportional density.
    //   overlap : 0.0..1.0 — how much each colour bleeds into its neighbour's
    //             zone. 0 = hard 3-band split; 1 = strong overlap (every
    //             colour sprinkles throughout the sequence). Default 0.6 keeps
    //             the gradient direction visible while softening the bands.
    static std::vector<int> build_dithered_tools_3color(
        int n_lines, int tool_a, int tool_b, int tool_c,
        int pct_a, int pct_b,
        int easing = kColormixEasing_Linear, double gamma = 1.0,
        double overlap = 0.6);

    // Custom hard-band sequence: emits `cnt_a` of tool_a, then `cnt_b` of tool_b,
    // then `cnt_c` of tool_c, then `cnt_d` of tool_d, cycling until n_lines is
    // reached. Skips bands with count == 0. No dither — clean blocks.
    static std::vector<int> build_custom_bands(
        int n_lines,
        int tool_a, int cnt_a,
        int tool_b, int cnt_b,
        int tool_c, int cnt_c,
        int tool_d, int cnt_d);

    // Geometric estimate of fill-line count for a surface.
    //   area_mm2          : surface area in mm²
    //   line_width_mm     : actual extrusion line width (top_solid_infill_line_width)
    //   overlap_fraction  : infill_overlap (0..1) — fraction of width that overlaps
    //   pattern_factor    : 1.0 for rectilinear/monotonic (default), 0.85 for
    //                       concentric / archimedean (paths follow contours).
    // Returns an integer estimate. ±~15% on irregular shapes — good enough for
    // UI feedback "≈ N lines". Cost: O(1).
    static int estimate_surface_line_count(
        double area_mm2,
        double line_width_mm,
        double overlap_fraction = 0.0,
        double pattern_factor   = 1.0);

    // NEOTKO_COLORMIX_TAG_START - MixedFilament UI helpers
    static std::vector<ColorMixOption> get_mix_options(
        const std::string&              mixed_defs,
        const std::vector<std::string>& filament_colours);

    static std::string mixed_filament_to_pattern(const MixedFilament& mf);

    // Returns the normalized blend weights for a virtual MixedFilament recipe.
    // Key: 0-based physical tool index.  Value: fraction [0..1] of total blend.
    // Used by MultiPass "Normalize to MixedColor %" to set layer_ratio per pass.
    static std::map<int,float> extract_recipe_weights(
        const MixedFilament& mf, size_t num_physical);
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
    // role: erTopSolidInfill → reads multipass_* keys (top surface config)
    //       erPenultimateInfill → reads penultimate_multipass_* keys
    static MultiPassConfig from_region_config(const PrintRegionConfig& cfg,
                                              ExtrusionRole role = erTopSolidInfill);
};

// NEOTKO_PATHBLEND_TAG_START — MultiPathBlend: independent gradient blend system
// NEOTKO_SANDWICH_TAG — Fase 5 (s72): geometry-driven PathBlend.
//
// The blend is governed by explicit Z heights (mm) instead of the old
// min_ratio/max_ratio fractions. Two variants live in the SandwichDialog
// row selector: PathBlend Half (no cap, semi-filled layer) and PathBlend
// Full (ramp + flat cap on top, fully filled).
//
// Geometry (relative to bottom_z, in mm):
//   - floor_mm   : Z of the ramp at t=0 (low end). Min 0.01 mm.
//   - mid_end_mm : Z of the ramp at t=1 (high end). Must be >= floor_mm.
//                  Full also requires mid_end_mm <= H - 0.04 (cap >= 0.04).
//                  Sentinel < 0 ⇒ "auto": resolves to the tallest legal ramp
//                  (H - 0.04 for Full, H for Half). This is the default so a
//                  fresh PathBlend ramps the full layer instead of staying flat.
//   - Full       : flat cap at nominal_z covering [mid_end, nominal_z].
//   - Half       : no cap; area above mid_end is empty (authorized semi-fill).
//
// Legacy view (num_passes, tool[]) is derived from mode/tool_bottom/tool_top
// at load-time so existing iterating callers (GCode COLORMIX_HOOK,
// ToolOrdering, Fill.cpp PB block) keep compiling unchanged.
struct PathBlendPassConfig {
    enum class Mode : int { Half = 0, Full = 1 };

    bool    enabled         = false;
    int     surface         = 0;      // 0=both, 1=top, 2=penultimate

    // --- New geometry model (Fase 5) ---
    Mode    mode            = Mode::Full;
    float   floor_mm        = 0.01f;  // Z of ramp at t=0 (low end), >= 0.01
    float   mid_end_mm      = -1.0f;  // Z of ramp at t=1; <0 ⇒ auto = H-0.04 (Full) / H (Half)
    int     tool_bottom     = 0;      // ramp tool (0-based)
    int     tool_top        = 1;      // cap tool (Full only; -1 if Half)
    int     ease_mode       = 0;      // 0=Linear, 1=EaseIn (t²), 2=EaseOut, 3=EaseInOut
    int     fill_angle      = -1;     // -1 = follow top surface angle; 0..359 = override

    // --- Legacy view (derived from the new fields by from_*) ---
    // num_passes == 1 for Half, 2 for Full. tool[0]=tool_bottom,
    // tool[1]=tool_top (Full) or -1 (Half), tool[2..3] always -1.
    int     num_passes      = 2;
    int     tool[4]         = {0, 1, -1, -1};

    // Recompute num_passes/tool[] from mode/tool_bottom/tool_top.
    // Call this after writing to the new fields (called automatically by
    // from_region_config / from_blob_json).
    void    sync_legacy_view();

    // NEOTKO_COLORSTITCH_TAG s108 — physical sanity clamp on (floor_mm,
    // mid_end_mm) for a given layer height H (mm). Promoted from the
    // SandwichDialog (Tab.cpp pb_apply_constraints) so the ColorStitch
    // Painter pro-mode tray shares the exact same rules:
    //   Half: mid_end_mm is the layer top; no cap. Forced mid = H.
    //   Full: cap = top 0.04 mm of flow → mid_end ≤ H − 0.04. Ramp must
    //         exist (mid > floor strictly).
    void    apply_constraints(double layer_height_mm);

    // Build from PrintRegionConfig.  NEOTKO_PATHBLEND_TAG — s69 miniblob: when
    // the per-zone blob key (pathblend_top / pathblend_penu, selected by `role`)
    // is non-empty it is parsed; if it's the new v=2 schema (Fase 5) the
    // geometry fields are read directly. Otherwise (v=1 miniblob or absent →
    // flat pathblend_* keys legacy) the values are converted to the new model.
    // enable + surface are always the shared scope keys
    // (multipass_path_gradient / pathblend_surface).
    static PathBlendPassConfig from_region_config(const PrintRegionConfig& cfg,
                                                  ExtrusionRole role = erTopSolidInfill);

    // NEOTKO_PATHBLEND_TAG — JSON round-trip for the per-zone blob.
    // to_blob_json() emits the v=2 schema (Fase 5 geometry).
    // from_blob_json() reads v=2 directly and converts v=1 (legacy s69 schema)
    // to the new model. An empty or invalid blob yields a default-constructed
    // config (Full, floor=0.01, mid_end=0.05).
    std::string                to_blob_json() const;
    static PathBlendPassConfig from_blob_json(const std::string& blob);
};
// NEOTKO_PATHBLEND_TAG_END

// NEOTKO_SANDWICH_TAG_START
// ===========================================================================
// Sandwich revamp — MultiPass as the universal layer cutter.
//
// A Top surface (and an independent Penultimate surface) becomes a *stack* of
// passes — a "sandwich of effects". Each pass owns a Z fraction (`ratio`, the
// draggable "50%") and an effect kind. MultiPass virtual sublayers are the only
// path: a ColorMix-only surface is a stack of 1 ColorMix pass.
//
// Storage (Q1): 2 coString JSON keys per region — neotko_surface_passes_top /
// neotko_surface_passes_penu. Legacy multipass_* / interlayer_colormix_* /
// pathblend_* keys (incl. the s69 pathblend_top/penu miniblob) stay read-only;
// synthesize_from_legacy() rebuilds a stack when the blob is empty so old 3mf /
// presets keep working.
//
// Slots model (user decision s69): a stack holds 1..3 passes.
//   1 slot    → ColorMix or PathBlend only (a lone Solid pass is meaningless).
//   2-3 slots → any kind per pass (Solid / ColorMix / PathBlend).
//
// NOTE: the enum is `SurfacePassKind`, NOT `SurfaceEffectKind` — the latter
// already exists in SurfaceEffectProfile.hpp with different members.

struct SurfacePass {
    SurfacePassKind kind  = SurfacePassKind::Solid;
    double          ratio = 0.0;          // fraction of layer Z height; Σ over stack ≈ 1.0

    // --- Solid pass parameters (kind == Solid) ---
    int             solid_tool = 0;       // 0-based physical extruder
    int             angle      = -1;      // -1 = auto (follow fill angle), 0-359 custom
    int             fan        = -1;      // 0-255, -1 = no change
    int             speed_pct  = 100;     // M220 Sxx override (100 = no change)
    std::string     gcode_start;          // injected before the pass fills
    std::string     gcode_end;            // injected after the pass fills

    // --- ColorMix / PathBlend parameters ---
    // Serialized config-key -> value maps (same shape as SurfaceEffectPayload).
    //  - colormix.kv : interlayer_colormix_* keys. Empty kv + present=true →
    //                  the engine falls back to the region preset config.
    //  - pathblend.kv: one entry "blob" holding PathBlendPassConfig::to_blob_json()
    //                  (the s69 miniblob schema, reused verbatim).
    SurfaceEffectPayload colormix;
    SurfaceEffectPayload pathblend;
};

struct SurfacePassStack {
    static constexpr int kMaxPasses = 3;  // slots model — hard cap

    bool                     enabled            = false;
    bool                     perimeter_override = false;
    // NEOTKO_BOTTOM_TAG — Fase 1 §5.3 (s152 OVERLAY): per-zone "this bottom is
    // SUPPORTED, control it" opt-in. OFF (default) = bottom overlay is clamped to a
    // single full-height pass (paint-only; a real bridge stays a bridge by
    // construction). ON = up to kMaxPasses Z-stacked passes, where pass 0 keeps the
    // base role and passes ≥1 take a solid role (erSolidInfill) so they print solid,
    // not bridge. Only meaningful on the bottom zone (stack_bottom_json); harmless on
    // top/penu stacks. Authored via the painter's Bottom WIP checkbox; round-trips in
    // to_json/from_json like enabled/perimeter_override.
    bool                     bottom_supported_control = false;
    std::vector<SurfacePass> passes;            // bottom -> top, 1..kMaxPasses

    bool empty() const { return passes.empty(); }

    // True if every pass is Solid (or the stack is empty). An all-Solid stack
    // is GCode-equivalent to a classic MultiPass run.
    bool all_solid() const;

    // NEOTKO_SANDWICH_TAG s119 (EMPTY model) — kind None is the first-class
    // "empty zone authored on purpose" (explicit passthrough: natural surface
    // tool, no gap), distinct from an unauthored (passes.empty()) stack.
    // any_effect() is the single authority for "this zone does something",
    // retiring the legacy `enabled` flag as a parallel encoding of "no effect".
    bool any_effect() const;   // true iff any pass has kind != None

    // JSON round-trip. to_json() of a disabled/empty stack returns "" so the
    // config key stays at its empty default (→ synthesize_from_legacy kicks in).
    std::string             to_json() const;
    static SurfacePassStack from_json(const std::string& text);

    // Rebuild a stack from a region's legacy keys when the blob is empty.
    //   role == erPenultimateInfill → reads penu legacy keys + pathblend_penu
    //   any other role             → reads top legacy keys  + pathblend_top
    static SurfacePassStack synthesize_from_legacy(const PrintRegionConfig& cfg,
                                                   ExtrusionRole role);

    // Read the right blob key for `role`, parse it; fall back to
    // synthesize_from_legacy() when the blob is empty. Single entry point for
    // the engine (Fill.cpp FASE 2) and the wipe-tower mirror (ToolOrdering).
    static SurfacePassStack resolve(const PrintRegionConfig& cfg,
                                    ExtrusionRole role);

    // Fase 3 UX helper — same as resolve() but for the SandwichDialog, which
    // works with a DynamicPrintConfig (not a typed PrintRegionConfig). Copies
    // the overlapping region keys into a PrintRegionConfig and delegates.
    // `penu == true` resolves the Penultimate zone, false the Top zone.
    static SurfacePassStack resolve_for_zone(const DynamicPrintConfig& cfg,
                                             bool penu);

    // Build a legacy MultiPassConfig view of the stack so the existing FASE 2
    // sublayer loop can consume it. Non-Solid passes get tool = -1.
    MultiPassConfig to_multipass_config(ExtrusionRole role) const;

    // Inverse: build an all-Solid stack from a MultiPassConfig. Used by the
    // FASE 2 painter branch (legacy bridge) so the loop always iterates a stack.
    static SurfacePassStack from_multipass_config(const MultiPassConfig& mp);
};
// NEOTKO_SANDWICH_TAG_END


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
        // NEOTKO_PATHBLEND_TAG — s68: explicit role. erPenultimateInfill reads
        // the penultimate_multipass_* keys; any other role reads multipass_*.
        // Without this the MULTIPASS-mode branch always used the TOP stack,
        // breaking the PB+MP combo on the penultimate surface.
        ExtrusionRole                             role,
        GCodeWriter&                              writer,
        double                                    nominal_z,
        double                                    layer_height,
        double                                    F,
        double                                    e_per_mm,
        int                                       pass_idx,
        double                                    surface_t,
        const std::function<Vec2d(const Point&)>& point_to_gcode,
        // NEOTKO_PATHBLEND_TAG — s58 Bug 2 safety: optional out-param tracking
        // the max z reached so far per pass_idx within the current layer.  When
        // provided, this function clamps z_pass to max(z_pass, (*max_z_per_pass)[pass_idx])
        // and updates the map.  Effect: nozzle never descends within a pass —
        // only ascends or stays flat.  Prevents the dangerous "start high z +
        // low flow, end low z + high flow" pattern that risks drag/lifts.
        // Caller must reset the map at the start of each real layer.
        std::map<int, double>*                    max_z_per_pass = nullptr
    );

    // NEOTKO_PATHBLEND_TAG — Fase 5 s77 migración: overload taking the resolved
    // PathBlendPassConfig directly. The cfg-version above derives `pb` from
    // from_region_config(cfg, role) and forwards here. The MultiPass-sublayer
    // dispatch (GCode.cpp) decodes the sublayer's stored blob into a pb and calls
    // this directly — it has no PrintRegionConfig in scope and must not depend on
    // the m_layer/m_config global state that the legacy extrude_path branch used.
    static std::string apply_path(
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
        std::map<int, double>*                    max_z_per_pass = nullptr
    );
};

// ===========================================================================
// === INGREDIENT: PathBlend ================================================
// ===========================================================================
// PathBlend is one of the sandwich ingredients (alongside ColorMix and the
// MultiPass passes). It owns its own data structures, runtime toggles and
// helper math. Code below this banner is PathBlend-specific — keep it
// self-contained so future ingredients can be added in their own banner
// blocks without polluting this one.
//
// Sections inside the PathBlend ingredient:
//   1. PBBand / compute_pb_bands        — band geometry helper (legacy + cap)
//   2. PathBlendSchedulerRuntime        — scheduler-side toggle (atomic chain)
//   3. PathBlendDispatcherRuntime       — dispatcher-side toggles (continuous
//                                          chain, XY threshold)
// ===========================================================================

// --- 1. PathBlend band geometry ------------------------------------------
// NEOTKO_PATHBLEND_TAG_START — s87 B-bands model.
// A PB pass discretized into K real micro-layers along the t axis. Each band
// is a self-contained printable slice with its own Z, height and t-range; the
// caller masks the surface to that t-range, builds a Flow.with_height(h_step)
// and emits a regular single-tool sublayer (no apply_path flow scaling needed).
//
// For Full mode, K_ramp bands cover the ascending wedge, and a matching set of
// K_cap cap-bands cover the residual hole between each ramp step and Z=nominal.
// All cap-bands print at Z=nominal_z but each has its own h_cap (= H - ramp_at_t_mid)
// so the regenerated Flow.with_height(h_cap) gives spacing/width consistent with
// the volume that band must deposit. This composes cleanly with ColorMix bucket
// splitting (each band can itself be sub-split into N tool-buckets without
// touching the band math).
struct PBBand {
    bool   is_cap;       // false = ramp band, true = cap band
    float  t_lo;         // [0,1] — t-range covered by this band (XY mask range)
    float  t_hi;
    float  t_mid;        // midpoint used for ramp height eval
    float  z_top;        // absolute Z at which the nozzle prints this band
    float  h_step;       // physical layer height for the new Flow (rounded-rect)
};
// compute_pb_bands returns the ordered list of bands for one PB pass.
//   bottom_z      — base Z (top of layer below)
//   H             — full layer height
//   min_band_h    — minimum printable height; bands thinner are merged upward
//   want_cap      — true for Full mode (emit cap bands), false for Half
// Empty result means "fall back to legacy K==1 path" (the band model degenerates
// to the current variable-Z apply_path behaviour).
std::vector<PBBand> compute_pb_bands(
    const PathBlendPassConfig& pb,
    double                     bottom_z,
    double                     H,
    double                     min_band_h,
    bool                       want_cap,
    int                        target_k = 0); // 0 = natural (range / min_band_h). >0 = force K (clamped by min_band_h floor; safety still applies).
// NEOTKO_PATHBLEND_TAG_END

// --- 2. PathBlend SCHEDULER runtime --------------------------------------
// NEOTKO_PATHBLEND_TAG — s88. Toggle consumed by MultiPassScheduler and
// NeoTower. When chain_atomic is true, the cross-object scheduler drains
// every consecutive same-tool sublayer of one chain before moving on to
// the next chain. Eliminates cross-object micro-travels between PB
// scanlines of different objects (the multi-cube preview bug).
// Lives at libslic3r level so both backend and GUI can read/write through
// a single singleton without pulling GUI headers into libslic3r.

// --- 3. PathBlend DISPATCHER runtime -------------------------------------
// NEOTKO_PATHBLEND_TAG — s88. Toggles consumed by GCode.cpp dispatcher.
//   chain_continuous: when true, suppress retract+wipe+lift between two
//     same-tool PB sublayers within chain_max_xy_mm of each other →
//     continuous extrusion across adjacent scanlines.
//   chain_max_xy_mm:  XY threshold (mm) below which two PB sublayers are
//     considered part of the same chain. Beyond it (disconnected
//     islands) the normal lift cycle returns.

// NEOTKO_MULTIPASS_TAG_END

// ===========================================================================
// NEOTKO_COLORMIX_TAG — s58 lane distribution helpers (modes 1/2/3).
// Shared between SurfaceColorMix (ColorMix) and GCode.cpp (PathBlend) so both
// engines respect the same `surface_color_mix_lane_mode` config key.
// Header-defined as templates so the caller can pass any RawLine-like type
// exposing `.pl` (Polyline) and `.width` (float).  See PrintConfig.hpp for
// the kLaneMode_* constants and full mode description.
// ===========================================================================
struct LaneVec2 { double x = 0.0, y = 0.0; };

inline LaneVec2 lane_centroid(const Polyline& pl) {
    LaneVec2 c{0.0, 0.0};
    if (pl.points.empty()) return c;
    for (const auto& pt : pl.points) {
        c.x += static_cast<double>(pt.x());
        c.y += static_cast<double>(pt.y());
    }
    c.x /= static_cast<double>(pl.points.size());
    c.y /= static_cast<double>(pl.points.size());
    return c;
}

inline LaneVec2 lane_direction(const Polyline& pl) {
    if (pl.points.size() < 2) return {1.0, 0.0};
    double dx = static_cast<double>(pl.points.back().x() - pl.points.front().x());
    double dy = static_cast<double>(pl.points.back().y() - pl.points.front().y());
    double n = std::sqrt(dx*dx + dy*dy);
    if (n < 1e-6) return {1.0, 0.0};
    return {dx/n, dy/n};
}

inline double lane_angle_mod_pi(const Polyline& pl) {
    auto d = lane_direction(pl);
    double a = std::atan2(d.y, d.x);
    while (a <  0.0) a += M_PI;
    while (a >= M_PI) a -= M_PI;
    return a;
}

template <class RawLineT>
inline size_t lane_pick_reference(const std::vector<RawLineT>& raw_lines) {
    size_t best = 0;
    double best_len = -1.0;
    for (size_t i = 0; i < raw_lines.size(); ++i) {
        double len = raw_lines[i].pl.length();
        if (len > best_len) { best_len = len; best = i; }
    }
    return best;
}

// Compute slot_per_line[i] in [0, n_slots) for all lines, according to lane_mode.
// debug_summary (optional) receives a short human-readable label for logs.
template <class RawLineT>
inline std::vector<int> compute_slot_per_line(
    const std::vector<RawLineT>& raw_lines,
    int n_slots,
    int lane_mode,
    std::string* debug_summary = nullptr)
{
    const int n = static_cast<int>(raw_lines.size());
    std::vector<int> slot_per_line(n, 0);
    if (n_slots <= 0 || n <= 0) return slot_per_line;

    if (lane_mode == kLaneMode_Default) {
        for (int i = 0; i < n; ++i) slot_per_line[i] = i % n_slots;
        if (debug_summary) *debug_summary = "Default";
        return slot_per_line;
    }

    const size_t ref = lane_pick_reference(raw_lines);
    const LaneVec2 fill_dir = lane_direction(raw_lines[ref].pl);
    const LaneVec2 perp{-fill_dir.y, fill_dir.x};
    const double width_mm = static_cast<double>(raw_lines[0].width);
    const double spacing_scaled = std::max(1.0, width_mm * 1e6);

    auto proj_of = [&](size_t i) -> double {
        LaneVec2 c = lane_centroid(raw_lines[i].pl);
        return c.x * perp.x + c.y * perp.y;
    };

    if (lane_mode == kLaneMode_GeoSort) {
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return proj_of(a) < proj_of(b); });
        for (int rank = 0; rank < n; ++rank)
            slot_per_line[order[rank]] = rank % n_slots;
        if (debug_summary) *debug_summary = "GeoSort";
        return slot_per_line;
    }

    if (lane_mode == kLaneMode_LaneQuant) {
        std::vector<int> lane(n, 0);
        int min_lane = std::numeric_limits<int>::max();
        for (int i = 0; i < n; ++i) {
            lane[i] = static_cast<int>(std::llround(proj_of(i) / spacing_scaled));
            min_lane = std::min(min_lane, lane[i]);
        }
        for (int i = 0; i < n; ++i) {
            const int rel = lane[i] - min_lane;
            slot_per_line[i] = ((rel % n_slots) + n_slots) % n_slots;
        }
        if (debug_summary) {
            int max_lane = std::numeric_limits<int>::min();
            for (int i = 0; i < n; ++i) max_lane = std::max(max_lane, lane[i]);
            std::ostringstream s;
            s << "LaneQuant lanes=" << (max_lane - min_lane + 1)
              << " spacing_mm=" << width_mm;
            *debug_summary = s.str();
        }
        return slot_per_line;
    }

    if (lane_mode == kLaneMode_DirCluster) {
        constexpr double kAngleThresh = M_PI / 12.0; // 15°
        auto angle_dist = [](double a, double b) -> double {
            double d = std::abs(a - b);
            if (d > M_PI / 2.0) d = M_PI - d;
            return d;
        };
        std::vector<double> angle_per_line(n);
        for (int i = 0; i < n; ++i)
            angle_per_line[i] = lane_angle_mod_pi(raw_lines[i].pl);

        std::vector<double> cluster_angles;
        std::vector<int>    cluster_of(n, 0);
        for (int i = 0; i < n; ++i) {
            int best = -1; double best_d = 1e9;
            for (size_t c = 0; c < cluster_angles.size(); ++c) {
                double d = angle_dist(angle_per_line[i], cluster_angles[c]);
                if (d < kAngleThresh && d < best_d) { best_d = d; best = static_cast<int>(c); }
            }
            if (best < 0) {
                cluster_angles.push_back(angle_per_line[i]);
                best = static_cast<int>(cluster_angles.size()) - 1;
            }
            cluster_of[i] = best;
        }

        const int K = static_cast<int>(cluster_angles.size());
        std::vector<int> lane_per_line(n, 0);
        std::vector<int> min_lane_per_cluster(K, std::numeric_limits<int>::max());
        for (int i = 0; i < n; ++i) {
            const double ang = cluster_angles[cluster_of[i]];
            const LaneVec2 cperp{-std::sin(ang), std::cos(ang)};
            LaneVec2 c = lane_centroid(raw_lines[i].pl);
            const double proj = c.x * cperp.x + c.y * cperp.y;
            const int lane = static_cast<int>(std::llround(proj / spacing_scaled));
            lane_per_line[i] = lane;
            min_lane_per_cluster[cluster_of[i]] =
                std::min(min_lane_per_cluster[cluster_of[i]], lane);
        }
        for (int i = 0; i < n; ++i) {
            const int rel = lane_per_line[i] - min_lane_per_cluster[cluster_of[i]];
            slot_per_line[i] = ((rel % n_slots) + n_slots) % n_slots;
        }
        if (debug_summary) {
            std::ostringstream s;
            s << "DirCluster K=" << K << " angles=[";
            for (int c = 0; c < K; ++c) {
                if (c) s << ",";
                s << int(std::round(cluster_angles[c] * 180.0 / M_PI)) << "deg";
            }
            s << "]";
            *debug_summary = s.str();
        }
        return slot_per_line;
    }

    // Unknown mode → safe fallback.
    for (int i = 0; i < n; ++i) slot_per_line[i] = i % n_slots;
    if (debug_summary) *debug_summary = "UnknownMode->Default";
    return slot_per_line;
}

// NEOTKO_COLORMIX_TAG — s58 Bug 1 fix: continuous t [0,1] per line.
// Same family as compute_slot_per_line but returns a fractional t in [0, 1]
// normalised against the actual lane range observed, NOT against a fixed slot
// count.  PathBlend uses this t directly as `surface_t` so it MUST cover the
// full [0, 1] range — otherwise pass N-1 paths near t=0 get flow=0 and are
// skipped by the apply_path guard (`if (flow < 1e-9) return "";`), producing
// visible gaps in the second pass.
//
// Modes:
//   0 Default     — t = i / (n - 1)                       (no geometry)
//   1 GeoSort     — t = rank(⊥proj) / (n - 1)             (sort-based)
//   2 LaneQuant   — t = (lane - min_lane) / (max - min)   (geometric, global)
//   3 DirCluster  — t = (lane_c - min_c) / (max_c - min_c) per cluster
//                   (intra-cluster gradient; clusters are independent)
template <class RawLineT>
inline std::vector<double> compute_t_per_line(
    const std::vector<RawLineT>& raw_lines,
    int lane_mode,
    std::string* debug_summary = nullptr)
{
    const int n = static_cast<int>(raw_lines.size());
    std::vector<double> t_per_line(n, 0.0);
    if (n <= 0) return t_per_line;
    if (n == 1) { t_per_line[0] = 0.5; return t_per_line; }

    if (lane_mode == kLaneMode_Default) {
        const double denom = static_cast<double>(n - 1);
        for (int i = 0; i < n; ++i) t_per_line[i] = static_cast<double>(i) / denom;
        if (debug_summary) *debug_summary = "Default";
        return t_per_line;
    }

    const size_t ref = lane_pick_reference(raw_lines);
    const LaneVec2 fill_dir = lane_direction(raw_lines[ref].pl);
    const LaneVec2 perp{-fill_dir.y, fill_dir.x};
    const double width_mm = static_cast<double>(raw_lines[0].width);
    const double spacing_scaled = std::max(1.0, width_mm * 1e6);

    auto proj_of = [&](size_t i) -> double {
        LaneVec2 c = lane_centroid(raw_lines[i].pl);
        return c.x * perp.x + c.y * perp.y;
    };

    if (lane_mode == kLaneMode_GeoSort) {
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return proj_of(a) < proj_of(b); });
        const double denom = static_cast<double>(n - 1);
        for (int rank = 0; rank < n; ++rank)
            t_per_line[order[rank]] = static_cast<double>(rank) / denom;
        if (debug_summary) *debug_summary = "GeoSort";
        return t_per_line;
    }

    if (lane_mode == kLaneMode_LaneQuant) {
        std::vector<int> lane(n, 0);
        int min_l = std::numeric_limits<int>::max();
        int max_l = std::numeric_limits<int>::min();
        for (int i = 0; i < n; ++i) {
            lane[i] = static_cast<int>(std::llround(proj_of(i) / spacing_scaled));
            min_l = std::min(min_l, lane[i]);
            max_l = std::max(max_l, lane[i]);
        }
        const double range = std::max(1.0, static_cast<double>(max_l - min_l));
        for (int i = 0; i < n; ++i)
            t_per_line[i] = static_cast<double>(lane[i] - min_l) / range;
        if (debug_summary) {
            std::ostringstream s;
            s << "LaneQuant lanes=" << (max_l - min_l + 1)
              << " spacing_mm=" << width_mm;
            *debug_summary = s.str();
        }
        return t_per_line;
    }

    if (lane_mode == kLaneMode_DirCluster) {
        constexpr double kAngleThresh = M_PI / 12.0; // 15°
        auto angle_dist = [](double a, double b) -> double {
            double d = std::abs(a - b);
            if (d > M_PI / 2.0) d = M_PI - d;
            return d;
        };
        std::vector<double> angle_per_line(n);
        for (int i = 0; i < n; ++i)
            angle_per_line[i] = lane_angle_mod_pi(raw_lines[i].pl);

        std::vector<double> cluster_angles;
        std::vector<int>    cluster_of(n, 0);
        for (int i = 0; i < n; ++i) {
            int best = -1; double best_d = 1e9;
            for (size_t c = 0; c < cluster_angles.size(); ++c) {
                double d = angle_dist(angle_per_line[i], cluster_angles[c]);
                if (d < kAngleThresh && d < best_d) { best_d = d; best = static_cast<int>(c); }
            }
            if (best < 0) {
                cluster_angles.push_back(angle_per_line[i]);
                best = static_cast<int>(cluster_angles.size()) - 1;
            }
            cluster_of[i] = best;
        }

        const int K = static_cast<int>(cluster_angles.size());
        std::vector<int> lane_per_line(n, 0);
        std::vector<int> min_lane_per_c(K, std::numeric_limits<int>::max());
        std::vector<int> max_lane_per_c(K, std::numeric_limits<int>::min());
        for (int i = 0; i < n; ++i) {
            const double ang = cluster_angles[cluster_of[i]];
            const LaneVec2 cperp{-std::sin(ang), std::cos(ang)};
            LaneVec2 c = lane_centroid(raw_lines[i].pl);
            const double proj = c.x * cperp.x + c.y * cperp.y;
            const int lane = static_cast<int>(std::llround(proj / spacing_scaled));
            lane_per_line[i] = lane;
            min_lane_per_c[cluster_of[i]] = std::min(min_lane_per_c[cluster_of[i]], lane);
            max_lane_per_c[cluster_of[i]] = std::max(max_lane_per_c[cluster_of[i]], lane);
        }
        for (int i = 0; i < n; ++i) {
            const int c = cluster_of[i];
            const double range = std::max(1.0,
                static_cast<double>(max_lane_per_c[c] - min_lane_per_c[c]));
            t_per_line[i] = static_cast<double>(lane_per_line[i] - min_lane_per_c[c]) / range;
        }
        if (debug_summary) {
            std::ostringstream s;
            s << "DirCluster K=" << K << " angles=[";
            for (int c = 0; c < K; ++c) {
                if (c) s << ",";
                s << int(std::round(cluster_angles[c] * 180.0 / M_PI)) << "deg";
            }
            s << "]";
            *debug_summary = s.str();
        }
        return t_per_line;
    }

    // Unknown mode → safe fallback.
    const double denom = static_cast<double>(n - 1);
    for (int i = 0; i < n; ++i) t_per_line[i] = static_cast<double>(i) / denom;
    if (debug_summary) *debug_summary = "UnknownMode->Default";
    return t_per_line;
}

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
