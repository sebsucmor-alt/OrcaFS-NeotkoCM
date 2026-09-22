#ifndef slic3r_ColorStitch_hpp_
#define slic3r_ColorStitch_hpp_

// NEOTKO_COLORSTITCH_TAG_START
// Neotko Surface ColorStitch Feature
// Multi-tool distribution for top/penultimate surface layers
// Author: Neotko
// NEOTKO_COLORSTITCH_TAG_END

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
#include <iomanip>    // NEOTKO_COLORSTITCH s235: setprecision en el volcado del eje (axis_note)
#include <functional> // NEOTKO_NEOWEAVING: std::function for point_to_gcode callback
#include <algorithm>  // NEOTKO_COLORSTITCH s58: std::sort/min/max in lane mode helpers
#include <cmath>      // NEOTKO_COLORSTITCH s58: std::atan2, std::sqrt, std::abs
#include <limits>     // NEOTKO_COLORSTITCH s58: std::numeric_limits
#include <cstdlib>    // NEOTKO_PATHBLEND s282: std::getenv en pb_min_printable_h()

namespace Slic3r {

// NEOTKO_DEBUG_TAG_START
// Centralised debug infrastructure for all Neotko features.
// Env vars (set before launching the slicer):
//   ORCA_DEBUG_COLORSTITCH     — Surface ColorStitch assign/group logic
//   ORCA_DEBUG_MULTIPASS    — MultiPass CAMINO 1/2 fill generation
//   ORCA_DEBUG_PENULTIMATE  — Penultimate surface classification pipeline
//   ORCA_DEBUG_TOOLORDER    — ToolOrdering ColorStitch/MultiPass extruder registration
//   ORCA_DEBUG_ZBLEND       — ZBlend sub-layer computation
//   ORCA_DEBUG_PROFILE      — Surface Effect Profile / 3D Painter pipeline
//                             (manager add/remove, 3mf I/O, painter UI,
//                              Fase D painted-slot resolution + gv override)
//   ORCA_DEBUG_ALL          — Enable every channel at once
// Log files: /tmp/neotko_{colorstitch|multipass|penultimate|toolorder|zblend|wipetower|profile}.log
// NEOTKO_DEBUG_TAG_END

class PrintRegionConfig;
class ExtrusionEntityCollection;
class PrintObject;            // NEOTKO_PROFILE_TAG
class Surface;                // NEOTKO_PAINT_COEXIST_TAG s91 — mmu_governs_surface overload
class ModelObject;            // NEOTKO_PROFILE_TAG
struct ColorStitchSticker;       // NEOTKO_STICKER_TAG — sticker helpers (defined in Model.hpp)
struct SurfaceEffectProfile;  // NEOTKO_PROFILE_TAG
struct SurfaceEffectPayload;  // NEOTKO_PROFILE_TAG — Fase F
struct MultiPassConfig;       // NEOTKO_PROFILE_TAG — Fase F (defined below)
struct PathBlendPassConfig;   // NEOTKO_PROFILE_TAG — Fase G (defined below)

// NEOTKO_COLORSTITCH_TAG_START
// Represents one selectable option in the ColorStitch pattern picker UI.
struct ColorStitchOption {
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
static constexpr int COLORSTITCH_FLAG_MODIFIED      = 1;
static constexpr int COLORSTITCH_FLAG_UNSPLITTABLE  = 2;
// NEOTKO_COLORSTITCH_TAG_END

class ColorStitch {
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
        // painted via the ColorStitch Painter at layer Z (top role) or one layer
        // up (penu role); if a dominant slot is found, its profile overrides
        // the preset gradient view for this layer's fills.
        const PrintObject*          print_object  = nullptr,
        double                      layer_print_z = 0.0,
        double                      layer_height  = 0.0,
        // NEOTKO_COLORSTITCH_TAG — cuando el llamador (FASE2 band-loop) YA fusionó el
        // override per-pase (pass.colorstitch.kv) dentro de `config`, ese config es la
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
    // `object_has_any_colorstitch_paint`: returns true if any model_part volume of
    // the object has a non-zero slot in its colorstitch_slot_to_profile_id table.
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
    static bool object_has_any_colorstitch_paint(const ModelObject* mo);
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

    // NEOTKO_XVOL_CLIP_TAG (s210) — index (into mo->volumes) of the first
    // model_part volume that contributes a qualifying triangle for `slot`
    // within the Z band, or -1 if none. Used ONLY to order painted zones from
    // DIFFERENT ModelVolumes by merge/assemble priority when two boolean-
    // overlapping pieces (Assemble, neotko_assemble_boolean=true) have painted
    // recipes whose footprints collide in the same band: the volume with the
    // HIGHER index (later in the merge) wins, mirroring the exact criterion
    // PrintObjectSlice.cpp's clip_multipart_objects already uses for plain
    // geometry ("Clip every non-zero region preceding it" — later volume
    // survives). Same scan/frame as dominant_painted_slot_in_z_range, stops at
    // the first match instead of counting.
    static int  painted_slot_owner_volume_index_in_z_range(const PrintObject* po, int slot,
                                                             double z_min, double z_max,
                                                             bool downward = false);

    // NEOTKO_XVOL_CLIP_TAG (s212, bug #4) — volume-aware profile_id_for_slot.
    // Slot tables (colorstitch_slot_to_profile_id) are per-ModelVolume: a merged
    // object where two pieces each number slot N to a DIFFERENT recipe makes the
    // first-volume-wins scan of profile_id_for_slot() return the wrong recipe for
    // whichever piece isn't first. This resolves the profile from the SPECIFIC
    // volume that owns the painted footprint here (vol_idx, typically from
    // painted_slot_owner_volume_index_in_z_range). vol_idx < 0 (owner unknown, or
    // natural remainder) falls back to profile_id_for_slot — byte-identical for
    // single-volume objects, where the owner IS the only model_part.
    static int  profile_id_for_slot_in_volume(const PrintObject* po, int slot, int vol_idx);

    // NEOTKO_STICKER_TAG — Sandwich Sticker helpers (SVG masks, no facets).
    //
    // `object_has_any_colorstitch_stickers`: true if the object carries at least
    // one sticker whose profile still exists in the manager (ghost stickers —
    // pid 0 or dangling — are ignored, mirroring the s137b slot filter). ORed
    // with object_has_any_colorstitch_paint at every painter-mode gate.
    //
    // `enumerate_stickers_in_z_range`: indices into mo->colorstitch_stickers whose
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
    static bool object_has_any_colorstitch_stickers(const ModelObject* mo);
    static std::vector<size_t> enumerate_stickers_in_z_range(const PrintObject* po,
                                                             double z_min, double z_max);
    static ExPolygons sticker_footprint_slice_frame(const ColorStitchSticker& sticker,
                                                    const PrintObject* po);

    // NEOTKO_STICKER_TAG — shared core: parses `sticker.svg_data` (nanosvg on a
    // private copy) and projects every ring (contours CCW + holes CW) through
    // `to_target`, returning them as scaled Polygons (mm → clipper int), NOT
    // unioned. `sticker_footprint_slice_frame` composes `to_target =
    // po->trafo_centered() * sticker.transform` and unions the result into the
    // slice-mask ExPolygons; the GUI edit-mode overlay (GLGizmoColorStitchPainter)
    // calls this directly with a world/GL transform instead, so the SVG
    // parsing + projection logic lives in exactly one place. Empty on parse
    // failure. A degenerate/mirrored `to_target` (negative XY determinant)
    // reverses ring winding so orientation stays consistent either way.
    static Polygons sticker_rings_in_transform(const ColorStitchSticker& sticker,
                                               const Transform3d& to_target);

    // NEOTKO_PAINT_COEXIST_TAG s91 — MMU governance helpers.
    //
    // `mmu_painted_footprint_in_z_range`: XY footprint of MMU-painted triangles
    // (any extruder slot != NONE) whose Z range OVERLAPS [z_min, z_max].
    // Unlike painted_footprint_in_z_range (which filters upward facets), this
    // includes ALL facet orientations so lateral MMU paint also occludes the
    // sandwich at the XY column it covers. Empty if no MMU paint in the band.
    // NEOTKO_MMU_COEXIST_TAG s234 F2a — `downward` mirrors the underside, same
    // convention as painted_footprint_in_z_range (normal test flipped, min_z
    // instead of max_z). Needed for bottom-band coexistence: without it the
    // bottom always reports "no MMU" whatever the user painted there. Default
    // false = pre-s234 behaviour for mmu_governs_xy.
    static ExPolygons mmu_painted_footprint_in_z_range(
        const PrintObject* po, double z_min, double z_max,
        bool downward = false);

    // NEOTKO_MMU_COEXIST_TAG s234 F1 — SINGLE ENTRY POINT for the two footprints
    // (sandwich + MMU) of one Z band, both resolved in the SAME frame
    // (trafo_centered — ⚠️ NUNCA trafo(), lección s161) and the SAME band.
    //
    // The plan's hard invariant (docs/FUTURE/MMU_SANDWICH_COEXISTENCE_PLAN.md §2)
    // is that the two subtractions to come (F2: sandwich cedes the MMU zone;
    // F3: MMU cedes the sandwich zone) MUST use the same polygon. Both will read
    // it from here, so there is exactly one place where the band and the frame
    // are decided. F1 only observes — nothing consumes the result yet.
    struct CoexistBandMasks {
        ExPolygons sandwich;   // union of every useful painted-slot footprint
        ExPolygons mmu;        // MMU-painted footprint (upward facets, s91 v1.2)
        ExPolygons overlap;    // sandwich ∩ mmu — the disputed zone
        double     sandwich_area = 0.0;  // mm², unscaled
        double     mmu_area      = 0.0;
        double     overlap_area  = 0.0;
        size_t     overlap_segments = 0; // total contour points of `overlap`
        bool       has_conflict  = false; // overlap above the trivial floor
    };
    // Builds the two masks for [z_min, z_max]. `downward` mirrors BOTH scans to
    // the underside (bottom surfaces) — since F2a the MMU side honours it too,
    // so a bottom band reports the MMU paint that is really there instead of a
    // structural zero.
    static CoexistBandMasks coexist_masks_in_z_range(const PrintObject* po,
                                                     double z_min, double z_max,
                                                     bool downward = false);

    // Emits one COLORSTITCH line per band with both areas, the intersection area
    // and the segment count of the resulting border (a border with very few
    // segments where there IS overlap = suspicious clip). `tag` identifies the
    // callsite. No-op cost when the object has neither paint nor MMU.
    static void log_coexist_band(const PrintObject* po, double z_min, double z_max,
                                 bool downward, const char* tag);

    // `mmu_governs_xy`: returns true if `surface_xy` intersects the MMU paint
    // footprint at this layer's vertical slab. SINGLE source of truth consulted
    // identically by SLICE (ColorStitch, Fill) and ToolOrdering — any
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
    // `painted_perim_override_from_profile`: returns the profile's
    // `multipass_perimeter_override` value (top-role key), defaulting to
    // false if absent. Used by ToolOrdering to decide whether to register
    // mp_perim_override_active in painter mode.
    //
    // s280 — el hermano `multipass_from_profile_payload` se retiró por huérfano.
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
    static int              painted_colorstitch_angle_for_slot(
        const PrintObject* po, int slot, bool penu);

    // NEOTKO_STICKER_TAG — same lookup as `painted_colorstitch_angle_for_slot`,
    // but by profile id directly (a sticker has no slot). Both share this
    // implementation.
    static int              colorstitch_angle_for_profile_id(int profile_id, bool penu);

    // NEOTKO_PROFILE_TAG — Fase G painter-mode PathBlend override.
    // Reads pathblend_* keys directly from the kv map. PathBlendPassConfig
    // defaults are used for absent keys (struct defaults match PrintConfig
    // defaults). s280 — el espejo del que venía, `multipass_from_profile_payload`,
    // se retiró por huérfano.
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
    //   - colorstitch.present AND interlayer_colormix_surface ∈ {0, 2}, OR
    //   - pathblend.present AND pathblend_surface ∈ {0, 2}.
    static bool             object_painter_wants_penu(const ModelObject* mo);
    // NEOTKO_BOTTOM_TAG — Fase 1 (§4.3): true if any painted profile on the object
    // declares a non-empty Bottom WIP zone (stack_bottom_json with a real effect).
    // Gates the bottom-surface sandwich so untouched objects stay byte-identical.
    static bool             object_painter_wants_bottom(const ModelObject* mo);

    // Encode tool index in mm3_per_mm: original + (tool_idx + 1) * 10.0
    // Decode in GCode.cpp: tool = floor(mm3_per_mm / 10.0) - 1
    static void encode_tool_in_path(ExtrusionPath* path, int tool_idx);

    // NEOTKO_SANDWICH_TAG — Fase 2: decode a ColorStitch-encoded EEC into per-tool
    // buckets. `encoded` is the output of assign_and_group_tools() (tool stored
    // in mm3_per_mm via the +(tool+1)*10 trick). Each path is cloned, its real
    // mm3_per_mm restored, and routed to its tool's bucket. Unencoded paths
    // (mm3_per_mm < 10) go to `default_tool`. Buckets are returned in
    // first-appearance (spatial) order. The caller owns the cloned entities.
    static std::vector<std::pair<int, ExtrusionEntityCollection>>
    eec_to_tool_buckets(const ExtrusionEntityCollection& encoded, int default_tool);

    // NEOTKO_COLORSTITCH_TAG — s60 numeric gradient.
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
    static double colorstitch_easing_apply(double t, int easing, double gamma = 1.0);

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

    // NEOTKO_COLORSTITCH_TAG_START — s315 F1: dither evaluado en la POSICIÓN REAL.
    //
    // 🔑 EL HALLAZGO QUE JUSTIFICA ESTAS DOS FUNCIONES: de los modos de reparto que había,
    // NINGUNO daba un degradado correcto, y fallaban por lados opuestos.
    //   · GeoSort (1) ordena las líneas por posición y les da la secuencia en ese orden. El
    //     dither se conserva ENTERO (biyectivo), pero el degradado avanza por RANGO de línea,
    //     no por milímetro: donde un agujero o un texto en relieve parte líneas en dos, el
    //     degradado se comprime justo ahí.
    //   · LaneQuant / DirCluster (2/3) sí colocan por geometría, pero indexan el patrón con
    //     una posición normalizada, así que muestrean la secuencia de forma NO uniforme:
    //     entradas duplicadas y entradas saltadas. La geometría sale bien y el dither sale
    //     roto (en el caso extremo de s235 se perdía un color entero).
    //
    // Estas dos se quedan con lo bueno de los dos. El recorrido Bresenham se mantiene TAL
    // CUAL —línea a línea, con su contador acumulado, que es lo que garantiza que la
    // proporción global salga exacta— pero la curva objetivo se evalúa en la `t` REAL de cada
    // línea en vez de en `rango/(n-1)`. Resultado: proporción exacta (como GeoSort) Y
    // degradado que sigue al milímetro (como LaneQuant), a la vez.
    //
    // `t_asc` = la `t` de cada línea, ASCENDENTE, tal como la produce
    // compute_field_groups() una vez ordenada. El llamador conserva el mapa
    // rango→índice-de-línea para devolver cada tool a su sitio. Salida: un tool por entrada
    // de `t_asc`, en el mismo orden.
    //
    // ⚠️ Con `t_asc` = {0, 1/(n-1), …, 1} estas funciones devuelven EXACTAMENTE lo mismo que
    // sus gemelas de arriba. Es la prueba barata de que la refactorización no cambió el
    // dither: si divergen con esa entrada, el port está mal.
    static std::vector<int> build_dithered_tools_2color_at(
        const std::vector<double>& t_asc, int tool_a, int tool_b, int pct_a,
        int easing = kColormixEasing_Linear, double gamma = 1.0);

    static std::vector<int> build_dithered_tools_3color_at(
        const std::vector<double>& t_asc, int tool_a, int tool_b, int tool_c,
        int pct_a, int pct_b,
        int easing = kColormixEasing_Linear, double gamma = 1.0,
        double overlap = 0.6);
    // NEOTKO_COLORSTITCH_TAG_END — s315 F1

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

    // NEOTKO_COLORSTITCH_TAG_START - MixedFilament UI helpers
    static std::vector<ColorStitchOption> get_mix_options(
        const std::string&              mixed_defs,
        const std::vector<std::string>& filament_colours);

    static std::string mixed_filament_to_pattern(const MixedFilament& mf);

    // Returns the normalized blend weights for a virtual MixedFilament recipe.
    // Key: 0-based physical tool index.  Value: fraction [0..1] of total blend.
    // Used by MultiPass "Normalize to MixedColor %" to set layer_ratio per pass.
    static std::map<int,float> extract_recipe_weights(
        const MixedFilament& mf, size_t num_physical);
    // NEOTKO_COLORSTITCH_TAG_END

private:
    static void debug_log(
        int layer_idx,
        const std::vector<int>& tools,
        const std::map<int, std::vector<ExtrusionPath*>>& grouped
    );
};
// NEOTKO_COLORSTITCH_TAG_END

// NEOTKO_MULTIPASS_TAG_START
// Neotko MultiPass Blend Feature
// Re-prints top/penultimate surface N times with different tools + reduced line width.
// Runs BEFORE ColorStitch in Fill.cpp::make_fills().
//
// CAMINO 1 (current — no combination with ColorStitch):
//   MultiPass encodes tool in mm3_per_mm (same trick as ColorStitch).
//   ColorStitch automatically skips already-encoded paths (mm3_per_mm >= 10.0 guard).
//   Result: MultiPass and ColorStitch are mutually exclusive per surface.
//
// CAMINO 2 (future — full combination):
//   MultiPass clones paths WITHOUT tool encoding (only applies width_ratio).
//   ColorStitch then runs on each cloned pass and assigns tools per-line within it.

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
// at load-time so existing iterating callers (GCode COLORSTITCH_HOOK,
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
    // NEOTKO_PATHBLEND_TAG — s280b: default 45, y el comentario viejo ("-1 = follow top
    // surface angle") era FALSO: Fill.cpp cae al base_angle ALTERNANTE, no al ángulo del
    // top. -1 sigue valiendo y significa auto (alterna 45/135 por paridad de capa), pero
    // en PathBlend eso no es acabado: `_t_of()` reparte la rampa por el centroide Y de
    // cada línea, así que el ángulo decide el degradado y alternarlo lo cambia capa a capa.
    // Sólo cambia el default del struct: los payloads YA guardados sin la clave siguen
    // leyéndose como -1 (ver from_blob_json) y el painter los marca parpadeando.
    int     fill_angle      = COLORSTITCH_DEFAULT_ANGLE_DEG;  // -1 = auto (alterna)

    // NEOTKO_PATHBLEND_TAG — s316 F2b: ESCALA de la rampa. Espejo exacto de
    // interlayer_colormix_gradient_span_mm (s315), tres estados en un float:
    //   −1  LEGACY  — `t` sale del bbox proyectado de la superficie (`_t_of`, s280e).
    //                 Ruta intacta byte a byte. Es el DEFAULT: un 3mf viejo no cambia.
    //    0  CAMPO ajustado a la superficie — misma forma que legacy, pero agrupando
    //                 por CARRIL antes de repartir, así que los trozos de una misma
    //                 línea partida por un agujero comparten altura.
    //   >0  CAMPO con PERIODO FÍSICO en mm — la rampa mide lo mismo en todos los
    //                 objetos, midan lo que midan. Es lo que PathBlend nunca tuvo.
    // 🚨 Vive en el BLOB, no en una clave plana de PrintConfig: el blob ya es por pase
    // y por zona, que es justo el aislamiento que este cambio necesita.
    // 🔒 s316d — DECISIÓN DEL USUARIO: en PathBlend el campo es la ÚNICA ruta. Default 0
    // (ajustar a la superficie por carriles) y un −1 leído de un blob se sube a 0 en
    // from_blob_json: la ruta legacy queda retirada para todo, nuevo y viejo.
    // El periodo (>0) se CONSERVA en el motor pero NO tiene control en la UI (ver
    // kPathBlendSpanUI en GLGizmoColorStitchPainter.cpp). Reservado para un futuro
    // editor de degradados "tipo Illustrator" con un helper gráfico de distancia.
    float   span_mm         = 0.0f;

    // NEOTKO_PATHBLEND_TAG — s190 profile (Img 2/3): start/end zone of the ramp.
    // The ramp stays flat-low (at floor) until in_t, rises linearly, and stays
    // flat-high (at mid_end) from out_t on. in_t=0,out_t=1 (default) ⇒ profile_u
    // is the identity ⇒ the s88 linear staircase is byte-identical (untouched).
    // Orthogonal to Mode (Half/Full). See docs/WIP/PATHBLEND_PROFILE_PLAN.md.
    float   in_t            = 0.0f;   // t where the ramp starts to rise  [0,1)
    float   out_t           = 1.0f;   // t where the ramp reaches mid_end (0,1]

    // Shared remap called by BOTH the ramp and the cap (and legacy apply_path)
    // so volume conservation ramp(u)+cap(1-u)=H stays exact per Y. Monotonic
    // non-decreasing when in_t<out_t (guaranteed by apply_constraints), so the
    // staircase sort/scheduling stay valid. Degenerate span ⇒ falls back to t.
    double  profile_u(double t) const;

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
// path: a ColorStitch-only surface is a stack of 1 ColorStitch pass.
//
// Storage (Q1): 2 coString JSON keys per region — neotko_surface_passes_top /
// neotko_surface_passes_penu. Legacy multipass_* / interlayer_colormix_* /
// pathblend_* keys (incl. the s69 pathblend_top/penu miniblob) stay read-only;
// synthesize_from_legacy() rebuilds a stack when the blob is empty so old 3mf /
// presets keep working.
//
// Slots model (user decision s69): a stack holds 1..3 passes.
//   1 slot    → ColorStitch or PathBlend only (a lone Solid pass is meaningless).
//   2-3 slots → any kind per pass (Solid / ColorStitch / PathBlend).
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

    // --- ColorStitch / PathBlend parameters ---
    // Serialized config-key -> value maps (same shape as SurfaceEffectPayload).
    //  - colorstitch.kv : interlayer_colormix_* keys. Empty kv + present=true →
    //                  the engine falls back to the region preset config.
    //  - pathblend.kv: one entry "blob" holding PathBlendPassConfig::to_blob_json()
    //                  (the s69 miniblob schema, reused verbatim).
    SurfaceEffectPayload colorstitch;
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

// NEOTKO_NEOWEAVING_PORT_TAG — WAVESUPPORT_PLAN.md Fase 1: ported from FULLSPECTRUM095
// (legacy) ColorStitch.hpp:597-639, where this class was fully implemented and print-tested
// (Linear mode) but never carried over to the SNAPOFFICIAL canonical port (Tier B keys were
// dropped, see PrintConfig.hpp/CMakeLists.txt comments). Wave mode was disabled in the legacy
// engine due to a known OOM crash (unbounded std::string growth in apply_path()'s micro-segment
// loop on complex top surfaces, 10k+ lines) — the fix (pre-reserve the gcode buffer) is applied
// in this port's apply_path() (ColorStitch.cpp), so Wave mode is enabled here from the start.
// WaveSupport (docs/FUTURE/WAVESUPPORT_PLAN.md) Mecanismo 2 depends on this engine's Wave mode to
// create the contact-layer microgaps described there — Fase 5, not part of this port.
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
    //   contact_mode      — WAVESUPPORT_PLAN.md Fase 5 (Mecanismo 2). When true: force WAVE mode
    //                       and apply to ANY role (bypassing the role gate), using the
    //                       interlayer_neoweave_* parameter set. The oscillation is UPWARD-ONLY
    //                       (z ∈ [nominal_z, nominal_z + amplitude], rectified sine) so the contact
    //                       layer's valleys touch the support roof at nominal_z and its crests lift
    //                       into air — never dipping below nominal_z. This structurally guarantees
    //                       the "amplitude ≤ layer_height/2 → no penetration of the previous layer"
    //                       NEVER-do (§4): the nozzle cannot penetrate the roof regardless of A.
    //                       (This supersedes the plan's literal "negative amplitude / valley-down"
    //                       wording, which a symmetric ±A sine would violate.) Default false =
    //                       byte-identical legacy behaviour for top/penu/infill neoweaving.
    static std::string apply_path(
        const ExtrusionPath&                       path,
        const PrintRegionConfig&                   cfg,
        GCodeWriter&                               writer,
        int                                        layer_index,
        double                                     nominal_z,
        double                                     F,
        double                                     e_per_mm,
        bool                                       is_force_no_extr,
        const std::function<Vec2d(const Point&)>&  point_to_gcode,
        bool                                       contact_mode = false,
        // Contact mode only: explicit wave params (the Support-section keys). <0 = read from cfg.
        double                                     contact_amplitude = -1.0,
        double                                     contact_period = -1.0
    );

    // Restore the nozzle to nominal_z after a weaving path.
    // Linear mode: emits a G1 Z move at path speed F (NOT travel speed).
    // Wave mode:   emits travel_to_z (speed already capped via weave_F).
    static std::string restore_z(
        const PrintRegionConfig& cfg,
        GCodeWriter&             writer,
        double                   nominal_z,
        double                   F,
        bool                     surface_weave_active, // true=top/penultimate, false=infill
        bool                     contact_mode = false  // Fase 5: force the Wave restore branch
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
// PathBlend is one of the sandwich ingredients (alongside ColorStitch and the
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
// the volume that band must deposit. This composes cleanly with ColorStitch bucket
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
// NEOTKO_PATHBLEND_TAG s282 — THE PathBlend physical floor. Single owner; every
// site that sizes a PB bead reads this and nothing hardcodes a number next to it.
//
// s191 removed the old 0.04 "cap reserve" (see the `ceiling = H` comments in
// Fill.cpp), which let the cap thin toward zero. PathBlend-Angle.gcode is what
// that costs: 0.4 nozzle, 0.20 layer, top surface at F4200 = 70 mm/s, bead
// 0.352 mm wide, so caudal = 24.7 * H mm³/s.
//   H=0.013 -> 0.32 mm³/s ( 6% of nominal)  starved, matte, no sheen
//   H=0.018 -> 0.44 mm³/s ( 9%)  the knee: the wedge tip stops being one path and
//                                breaks into islands, each paying a full retract +
//                                wipe + lift + 1.5 mm prime to lay 0.0002-0.014 mm
//                                of filament. That is the stringing.
//   H=0.020 -> 0.49 mm³/s (10%)  hard minimum
//   H=0.035 -> 0.86 mm³/s (17%)  <- chosen, margin over the knee
//   H=0.040 -> 0.99 mm³/s (20%)  the Fill.cpp Sandwich min-band limit
// Line spacing was 0.360 mm against a 0.352 mm bead at EVERY height, so the infill
// geometry was never at fault. Flow alone explains the defect.
// s282 wipe-tower regression: this constant moves the RAMP'S TOP Z, because
// mid_end is clamped to H - this. On the repro layer the ramp top went 1.45 ->
// 1.415, and 1.45 is exactly where the cap sits. The scheduler windows sublayers
// by z_actual within EPSILON (1e-4), so while the ramp reached H the top ramp
// sublayers and the caps shared one window and got grouped by tool; with any
// ceiling they fall into separate windows and the tool alternates. That makes
// this constant a live suspect for the toolchange explosion.
//
// ORCA_PB_MIN_H overrides it at RUNTIME so the whole range is testable from ONE
// build. Measure the suspect, do not revert the patch to bisect it.
//   ORCA_PB_MIN_H=0      → NO ceiling at all, mid_end = H. The EXACT pre-s282
//                          geometry. This is the only value that reproduces the
//                          old behaviour; 0.01 still leaves a ceiling 10 microns
//                          lower and does NOT settle the question (s282 mistake:
//                          0.01 was read as "pre-patch" and it is not).
//   ORCA_PB_MIN_H=0.035  → shipped default
//   unset                → 0.035
inline double pb_min_printable_h()
{
    static const double v = [] {
        if (const char* e = std::getenv("ORCA_PB_MIN_H")) {
            try {
                const double d = std::stod(e);
                if (d >= 0.0 && d < 0.5) return d;   // 0 allowed on purpose: "no ceiling"
            } catch (...) {}
        }
        return 0.035;
    }();
    return v;
}

// compute_pb_bands returns the ordered list of bands for one PB pass.
//   bottom_z      — base Z (top of layer below)
//   H             — full layer height
//   min_band_h    — QUANTISATION step only: how finely the ramp range is diced
//                   (K_natural = range / min_band_h). Nothing to do with what the
//                   nozzle can physically lay down.
//   want_cap      — true for Full mode (emit cap bands), false for Half
//   min_printable_h — NEOTKO_PATHBLEND_TAG s282. PHYSICAL floor (mm) on any bead
//                   this pass emits: the ramp floor, the mid_end clamp and the cap
//                   height are all held at or above it. 0 = "reuse min_band_h"
//                   (pre-s282 behaviour). These two were a single knob until s282,
//                   which forced a trade between a fine gradient and a printable
//                   bead. See the kMinPrintableH block in Fill.cpp for the gcode
//                   measurements behind the 0.035 default.
// Empty result means "fall back to legacy K==1 path" (the band model degenerates
// to the current variable-Z apply_path behaviour).
std::vector<PBBand> compute_pb_bands(
    const PathBlendPassConfig& pb,
    double                     bottom_z,
    double                     H,
    double                     min_band_h,
    bool                       want_cap,
    int                        target_k = 0,  // 0 = natural (range / min_band_h). >0 = force K (clamped by min_band_h floor; safety still applies).
    double                     min_printable_h = 0.0); // 0 = reuse min_band_h
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
// NEOTKO_COLORSTITCH_TAG — s58 lane distribution helpers (modes 1/2/3).
// Shared between ColorStitch (ColorStitch) and GCode.cpp (PathBlend) so both
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

// NEOTKO_COLORSTITCH_TAG_START — s314: eje del efecto, DUEÑO ÚNICO.
// Extraído LITERAL de compute_slot_per_line() (s235/s235b) para que el modo 4 (bandas en
// mm) no pueda construir el eje de otra forma. La razón de que esto sea una función y no
// código copiado está contada entera en la nota de compute_slot_per_line: mapear un EJE
// (sin sentido, mod π) a una DIRECCIÓN tiene un envoltorio en 0/180 que ya provocó una vez
// que dos objetos casi paralelos salieran con el degradado espejado el uno del otro.
// Se cierra por los dos lados, igual que allí:
//   (1) si hay ángulo AUTORADO (>= 0) manda ése, nunca uno medido de la geometría — es el
//       mismo valor que usa el preview (ColorStitchPaintPreview::colorstitch_weave_theta),
//       así que motor y preview coinciden POR CONSTRUCCIÓN;
//   (2) el eje se canoniza a un semiplano fijo (perp.y > 0, desempate por perp.x > 0) para
//       cubrir el caso auto (-1), donde no hay valor autorado del que tirar.
// `out_ref_ang` y `out_flipped` salen sólo para los logs.
//
// 🚨 `half_plane_when_authored` — s315b. La canonización de semiplano existe para que dos
// superficies casi paralelas cuyo eje se MIDE de la geometría no reciban perpendiculares
// opuestas (el envoltorio 0/180 de s235b). Con el ángulo AUTORADO ese problema no existe:
// todas las superficies del plato comparten el mismo valor, así que ya son deterministas sin
// canonizar. El propio comentario de s235b lo dice — "(2) ... Cubre el caso auto (ángulo =
// -1), donde no hay valor autorado del que tirar" — pero el código la aplicaba SIEMPRE.
// Consecuencia medida en s315: con ángulo 90 el eje natural es (-1, 0) y el semiplano lo
// dejaba en (+1, 0) ⇒ el degradado salía al revés de lo que enseñaba el preview. Y 90° cae
// JUSTO en la frontera (cos 90 ≈ 6e-17, por debajo del epsilon, desempata por el signo de X),
// así que a 89° no volteaba y a 90° sí.
//   true  (default) = comportamiento histórico. Lo usan las rutas LEGACY, que no se tocan.
//   false           = respeta el eje autorado tal cual. Lo usan las rutas de CAMPO.
template <class RawLineT>
inline LaneVec2 lane_perp_axis(const std::vector<RawLineT>& raw_lines,
                               int     authored_angle_deg,
                               double* out_ref_ang = nullptr,
                               bool*   out_flipped = nullptr,
                               bool    half_plane_when_authored = true)
{
    const size_t ref = lane_pick_reference(raw_lines);
    const bool   authored = (authored_angle_deg >= 0);
    const double ref_ang = authored
        ? std::fmod(static_cast<double>(authored_angle_deg) * M_PI / 180.0, M_PI)
        : lane_angle_mod_pi(raw_lines[ref].pl);
    LaneVec2 perp{-std::sin(ref_ang), std::cos(ref_ang)};
    const bool apply_half_plane = !authored || half_plane_when_authored;
    const bool flipped = apply_half_plane
                      && ((perp.y < -1e-12) || (std::abs(perp.y) <= 1e-12 && perp.x < 0.0));
    if (flipped) { perp.x = -perp.x; perp.y = -perp.y; }
    if (out_ref_ang) *out_ref_ang = ref_ang;
    if (out_flipped) *out_flipped = flipped;
    return perp;
}

// NEOTKO_COLORSTITCH_TAG — s314: Pattern mode 4, "bandas en MM" (el modo campo).
//
// 🔑 LEER ESTO ANTES DE TOCAR NADA. Este modo NO es una variante del modo 3 con otras
// unidades: es un modelo distinto, y por eso no comparte ni una línea con
// compute_slot_per_line().
//
// Los modos 0-3 REPARTEN UN RECUENTO: construyen una secuencia con una entrada por línea y
// luego deciden qué línea se lleva qué entrada. Medido en el plato de llaveros del usuario
// (s314), eso hace que un "31 líneas" tecleado salga impreso como 27,7 líneas = 7,15 mm
// cuando el usuario esperaba 31 x 0,30 = 9,33 mm, y con un error DISTINTO en cada objeto.
// Tres factores multiplicativos, todos invisibles desde la UI:
//   1. el ancho de línea es POR OBJETO (0,30 / 0,33 / 0,36 / 0,42 en el mismo plato);
//   2. las líneas no se separan por su ancho sino por el SPACING del Flow,
//      width - height*(1 - PI/4)  (Flow.cpp:183) — un 10-17% menos;
//   3. un agujero o un texto en relieve parte una línea en DOS raw_lines, así que la
//      secuencia se alarga sin que el span crezca y el patrón se comprime. Medido:
//      n_lineas/n_carriles = 1,118 / 1,142 / 1,150 / 1,191 en los cuatro objetos.
//
// Este modo INVIERTE la pregunta. El diseño existe en milímetros sobre la superficie y cada
// línea pregunta "¿de qué color es el sitio donde caigo?". No se cuenta nada, así que los
// tres factores desaparecen a la vez:
//   · el ancho de línea deja de definir el diseño y pasa a ser sólo la finura del muestreo;
//   · el spacing deja de importar por el mismo motivo;
//   · dos segmentos del mismo carril tienen la MISMA proyección ⇒ el mismo color, luego un
//     agujero deja de poder mover una banda.
// Y como el ancla va en el marco del OBJETO (no en el mínimo observado de cada capa), el
// Top y el Penultimate comparten campo por construcción: dejan de necesitar que
// surface_color_mix_lane_mode los pelee para que coincidan.
//
// ⚠️ `surface_color_mix_lane_mode` NO se aplica aquí, y es correcto que no se aplique:
// GeoSort/LaneQuant/DirCluster existen para mapear un RECUENTO sobre posiciones, y aquí no
// hay recuento que mapear. La clave sigue viva y sirviendo a los modos 0-3.
//
// Contrato de salida: slot_per_line[i] indexa `band_mm` (y por tanto el vector `tools` del
// llamador, que trae UNA entrada por banda activa en el mismo orden). Ojo: aquí
// n_slots == número de bandas (2..4), no == número de líneas como en los modos 0-3.
//
// Cuantización: una frontera de banda casi nunca cae justo entre dos líneas, así que una
// banda de 8,0 mm con spacing 0,258 sale de 31 líneas = 7,998 mm. Es inevitable. Lo que sí
// se evita es la DERIVA: cada línea se compara contra la frontera exacta en mm
// (`fmod` sobre el periodo), en vez de redondear el paso a un número entero de líneas y
// multiplicar, que acumularía el error banda tras banda a lo largo de la superficie.
//
//   anchor_x_mm / anchor_y_mm : origen del diseño en el marco de rebanado (mm). El llamador
//     pasa el origen del OBJETO (po->trafo_centered() * (0,0,0)) para que la fase viaje con
//     la pieza y sea la misma en Top y Penu. Con print_object nulo (modo preset, sin
//     painter) pasa (0,0) = origen de la bandeja, que es igual de estable.
template <class RawLineT>
inline std::vector<int> compute_slot_per_line_band_mm(
    const std::vector<RawLineT>& raw_lines,
    const std::vector<double>&   band_mm,
    int          authored_angle_deg,
    double       anchor_x_mm,
    double       anchor_y_mm,
    std::string* debug_summary = nullptr)
{
    const int n = static_cast<int>(raw_lines.size());
    std::vector<int> slot_per_line(n, 0);
    const int n_bands = static_cast<int>(band_mm.size());
    if (n <= 0 || n_bands <= 0) return slot_per_line;

    double period = 0.0;
    for (double b : band_mm) period += std::max(0.0, b);
    if (period <= 1e-6) {                      // todo a cero → banda A para todo
        if (debug_summary) *debug_summary = "BandMM period=0 -> all slot 0";
        return slot_per_line;
    }

    double ref_ang = 0.0; bool flipped = false;
    const LaneVec2 perp = lane_perp_axis(raw_lines, authored_angle_deg, &ref_ang, &flipped,
                                         /*half_plane_when_authored*/ false);   // s315b

    // Fase: proyección del ancla sobre el mismo eje, en mm. Las coordenadas de raw_lines
    // están en unidades internas escaladas (1 mm == 1e6), de ahí el divisor.
    const double anchor_proj_mm = anchor_x_mm * perp.x + anchor_y_mm * perp.y;

    int    hist[4] = {0, 0, 0, 0};
    double proj_lo =  1e30, proj_hi = -1e30;
    for (int i = 0; i < n; ++i) {
        const LaneVec2 c = lane_centroid(raw_lines[i].pl);
        const double proj_mm = (c.x * perp.x + c.y * perp.y) / 1e6 - anchor_proj_mm;
        proj_lo = std::min(proj_lo, proj_mm);
        proj_hi = std::max(proj_hi, proj_mm);
        double r = std::fmod(proj_mm, period);
        if (r < 0.0) r += period;
        int slot = n_bands - 1;                // por si un fp de borde se pasa del último
        double acc = 0.0;
        for (int k = 0; k < n_bands; ++k) {
            acc += std::max(0.0, band_mm[k]);
            if (r < acc) { slot = k; break; }
        }
        slot_per_line[i] = slot;
        if (slot >= 0 && slot < 4) ++hist[slot];
    }

    if (debug_summary) {
        std::ostringstream o;
        o << "axis=" << int(std::round(ref_ang * 180.0 / M_PI)) << "deg"
          << " src=" << (authored_angle_deg >= 0 ? "cfg" : "geo")
          << " perp=(" << std::fixed << std::setprecision(2) << perp.x << "," << perp.y << ")"
          << std::defaultfloat << " cano=" << (flipped ? 1 : 0)
          << " BandMM period=" << period << "mm"
          << " span=" << (proj_hi - proj_lo) << "mm"
          << " bands=[";
        for (int k = 0; k < n_bands; ++k) { if (k) o << ","; o << band_mm[k] << "mm"; }
        o << "] lines_per_band=[";
        for (int k = 0; k < n_bands && k < 4; ++k) { if (k) o << ","; o << hist[k]; }
        o << "]";
        *debug_summary = o.str();
    }
    return slot_per_line;
}
// NEOTKO_COLORSTITCH_TAG_END — s314

// NEOTKO_COLORSTITCH_TAG_START — s315 F0: campo posicional agrupado por CARRIL.
//
// Primitivo compartido (degradados de ColorStitch, y PathBlend cuando le toque). Es la
// generalización de compute_slot_per_line_band_mm() (s314) a un valor continuo.
//
// 🚨 POR QUÉ DEVUELVE GRUPOS Y NO UNA `t` POR LÍNEA — bug encontrado en s315 con el llavero.
// La primera versión devolvía una `t` por línea, el llamador ordenaba y le daba a cada línea
// un RANGO, y el dither se recorría rango a rango. Parece correcto y no lo es: al lado de un
// agujero, una línea sale del relleno partida en DOS segmentos. Los dos proyectan al mismo
// sitio y reciben la misma `t` —la inmunidad a la fragmentación sí funcionaba— pero al
// ordenarlos reciben rangos CONSECUTIVOS, y el Bresenham avanza en cada rango: los dos trozos
// de una misma línea acaban con COLORES DISTINTOS. En el objeto se ve como un costurón justo
// donde está el agujero.
// Por eso aquí se agrupa ANTES de repartir: todos los segmentos que caen en el mismo carril
// son UN grupo, con una sola entrada del dither, y comparten color pase lo que pase.
//
// 🔑 El eje sale de lane_perp_axis(), o sea del ÁNGULO AUTORADO cuando lo hay.
//   ⚠️ s316: PathBlend NO entra por aquí con ese eje. El suyo se MIDE (PCA de los
//   centroides, `_t_of()` en Fill.cpp, s280e) y debe seguir midiéndose: su ángulo
//   autorado es `pb.fill_angle`, que alimenta `f->angle`, y `f->angle` no es la
//   dirección de las líneas impresas (_infill_direction le suma la paridad y +90°).
//   · nada depende del RECUENTO de líneas. La legacy en lane_mode 0 (el DEFAULT) hace
//     `t = i/(n-1)`, puro índice de emisión.
//
// DOS SEMÁNTICAS, elegidas por `period_mm`:
//   · <= 0 → AJUSTAR A LA SUPERFICIE. Los carriles se numeran de un extremo al otro y `t`
//     recorre 0..1 entre ellos. Es lo que se espera de un degradado y no tiene tamaño físico.
//   · >  0 → PERIODO FÍSICO. El carril se pliega con `fmod` sobre el periodo, así que dos
//     sitios de la pieza con la MISMA fase comparten grupo y color. La misma receta mide lo
//     mismo en todos los objetos. Deja obsoleto a `repetitions`.
//
// `spacing_mm` = separación REAL entre líneas (NO el ancho: ver
// weave_top_line_spacing / Flow.cpp:183). Es el tamaño del carril con el que se agrupa.
struct FieldGroups {
    std::vector<int>    group_of_line;   // por línea → índice de grupo en [0, m)
    std::vector<double> t_of_group;      // por grupo, `t` en [0,1], ASCENDENTE
};

// NEOTKO_COLORSTITCH_TAG — s316 F2b: el muestreador del campo. DUEÑO ÚNICO de la
// aritmética "posición en mm → carril → t". Lo usan los dos consumidores:
//   · ColorStitch, a través de field_groups_from_projections() (agrupa y reparte el dither);
//   · PathBlend, path a path, porque su `t` es continua y no necesita grupos.
// 🚨 Si esto se duplica en el llamador, rampa y degradado se separan en silencio. No hay
// aviso posible: los dos siguen produciendo un número plausible en [0,1].
struct FieldSampler {
    double    sp       = 1.0;    // separación real entre carriles, mm
    double    period   = 0.0;    // >0 ⇒ periodo físico; <=0 ⇒ ajustar a lo observado
    double    anchor   = 0.0;    // ancla ya proyectada sobre el eje, mm
    bool      invert   = false;
    long long k0       = 0;      // carril mínimo observado (sólo modo ajustado)
    long long k1       = 0;      // carril máximo observado (sólo modo ajustado)

    bool periodic() const { return period > 1e-4; }

    long long lane_of(double proj_mm) const
    {
        double pos_mm = proj_mm - anchor;
        if (periodic()) {
            pos_mm = std::fmod(pos_mm, period);
            if (pos_mm < 0.0) pos_mm += period;
        }
        return std::llround(pos_mm / std::max(1e-3, sp));
    }

    double t_of_lane(long long k) const
    {
        double t;
        if (periodic()) {
            const double lanes_per_period = std::max(1.0, period / std::max(1e-3, sp));
            t = std::clamp(double(k) / lanes_per_period, 0.0, 1.0);
        } else if (k1 <= k0) {
            t = 0.5;                                   // un solo carril: no hay recorrido
        } else {
            t = double(k - k0) / std::max(1.0, double(k1 - k0));
        }
        return invert ? (1.0 - t) : t;
    }

    double t_of(double proj_mm) const { return t_of_lane(lane_of(proj_mm)); }
};

// NEOTKO_COLORSTITCH_TAG — s316 F2b: el NÚCLEO del campo, sobre proyecciones ya hechas.
//
// Se extrae de compute_field_groups() SIN cambiarle una línea de aritmética, para que
// PathBlend pueda entrar con SU eje sin duplicar el modelo. Aquí no hay geometría: sólo
// una posición en mm por elemento, medida sobre el eje que haya elegido el llamador.
//
// 🚨 POR QUÉ EL EJE ES DEL LLAMADOR Y NO DE AQUÍ. Los dos consumidores lo eligen distinto
// y los dos tienen razón:
//   · ColorStitch lo saca de lane_perp_axis(), o sea del ÁNGULO AUTORADO cuando lo hay;
//   · PathBlend lo MIDE (PCA de los centroides, Fill.cpp `_t_of`, s280e). No puede
//     deducirlo: su ángulo autorado es `pb.fill_angle`, que alimenta `f->angle`, y
//     `f->angle` NO es la dirección de las líneas impresas — `_infill_direction()`
//     (FillBase.cpp) le suma el flip de paridad y un +90° incondicional. Deducirlo movería
//     el colapso del degradado al ángulo contrario, que es justo lo que s280e arregló.
// Meter el eje aquí dentro obligaría a uno de los dos a mentir. Se queda fuera.
//
// `spacing_mm` = separación REAL entre carriles, ya resuelta (NO el ancho: ver
// weave_top_line_spacing / Flow.cpp:183). `anchor_proj_mm` = el ancla, ya proyectada.
inline FieldGroups field_groups_from_projections(
    const std::vector<double>& proj_mm,
    double       period_mm,
    double       spacing_mm,
    double       anchor_proj_mm,
    bool         invert,
    int*         out_lanes = nullptr)
{
    FieldGroups out;
    const int n = static_cast<int>(proj_mm.size());
    out.group_of_line.assign(std::max(0, n), 0);
    if (n <= 0) return out;

    FieldSampler smp;
    smp.sp     = std::max(1e-3, spacing_mm);
    smp.period = period_mm;
    smp.anchor = anchor_proj_mm;
    smp.invert = false;          // el volteo de GRUPOS se hace abajo, no en la `t`

    // Carril (o fase) entero por elemento. Es la MISMA cuantización que usa el modo bandas.
    std::vector<long long> key(n, 0);
    for (int i = 0; i < n; ++i) key[i] = smp.lane_of(proj_mm[i]);

    // Grupos = claves distintas, ordenadas. Un map ordenado deja los grupos ya en orden
    // ascendente, que es justo lo que los builders `_at` esperan.
    std::map<long long, int> group_of_key;
    for (int i = 0; i < n; ++i) group_of_key.emplace(key[i], 0);
    int m = 0;
    for (auto& kv : group_of_key) kv.second = m++;
    for (int i = 0; i < n; ++i) out.group_of_line[i] = group_of_key[key[i]];
    if (out_lanes) *out_lanes = m;

    // `t` de cada grupo. Con periodo, la fase dentro del ciclo (el diente de sierra completo
    // existe aunque la superficie sólo cubra parte de él). Sin periodo, la posición relativa
    // entre los carriles observados.
    smp.k0 = group_of_key.begin()->first;
    smp.k1 = group_of_key.rbegin()->first;
    out.t_of_group.assign(static_cast<size_t>(m), 0.0);
    if (m == 1) {
        out.t_of_group[0] = 0.5;
    } else {
        int g = 0;
        for (const auto& kv : group_of_key) out.t_of_group[g++] = smp.t_of_lane(kv.first);
    }

    if (invert) {
        // Voltear el CAMPO, no la lista: se invierte la `t` y se da la vuelta al orden de los
        // grupos, de modo que `t_of_group` siga siendo ascendente (contrato de los builders).
        for (double& t : out.t_of_group) t = 1.0 - t;
        std::reverse(out.t_of_group.begin(), out.t_of_group.end());
        const int last = m - 1;
        for (int i = 0; i < n; ++i) out.group_of_line[i] = last - out.group_of_line[i];
    }
    return out;
}

template <class RawLineT>
inline FieldGroups compute_field_groups(
    const std::vector<RawLineT>& raw_lines,
    int          authored_angle_deg,
    double       period_mm,
    double       spacing_mm,
    double       anchor_x_mm,
    double       anchor_y_mm,
    bool         invert,
    std::string* debug_summary = nullptr)
{
    FieldGroups out;
    const int n = static_cast<int>(raw_lines.size());
    out.group_of_line.assign(std::max(0, n), 0);
    if (n <= 0) return out;

    double ref_ang = 0.0; bool flipped = false;
    const LaneVec2 perp = lane_perp_axis(raw_lines, authored_angle_deg, &ref_ang, &flipped,
                                         /*half_plane_when_authored*/ false);   // s315b
    const double anchor_proj_mm = anchor_x_mm * perp.x + anchor_y_mm * perp.y;
    // Sin un spacing utilizable no se puede hablar de carriles; el ancho de la primera línea
    // es el mejor sustituto disponible y sigue agrupando los segmentos partidos, que es lo
    // que de verdad importa aquí.
    const double sp = (spacing_mm > 1e-4)
                    ? spacing_mm
                    : std::max(1e-3, static_cast<double>(raw_lines[0].width));

    std::vector<double> proj_mm(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const LaneVec2 c = lane_centroid(raw_lines[i].pl);
        proj_mm[i] = (c.x * perp.x + c.y * perp.y) / 1e6;
    }

    int m = 0;
    out = field_groups_from_projections(proj_mm, period_mm, sp, anchor_proj_mm, invert, &m);

    if (debug_summary) {
        const bool periodic = (period_mm > 1e-4);
        std::ostringstream o;
        o << "axis=" << int(std::round(ref_ang * 180.0 / M_PI)) << "deg"
          << " src=" << (authored_angle_deg >= 0 ? "cfg" : "geo")
          << " perp=(" << std::fixed << std::setprecision(2) << perp.x << "," << perp.y << ")"
          << std::defaultfloat << " cano=" << (flipped ? 1 : 0)
          << " Field " << (periodic ? "periodic" : "fit-surface")
          << " lines=" << n << " lanes=" << m
          << " spacing=" << sp << "mm";
        if (periodic) o << " period=" << period_mm << "mm";
        o << " inv=" << (invert ? 1 : 0);
        *debug_summary = o.str();
    }
    return out;
}
// NEOTKO_COLORSTITCH_TAG_END — s315 F0

// NEOTKO_COLORSTITCH_TAG — s316 fase C: aquí vivía compute_slot_per_line() con los cuatro
// `Line distribution mode` (Default, GeoSort, LaneQuant, DirCluster). BORRADA con la retirada del
// legacy: repartía un RECUENTO de líneas sobre posiciones, y un recuento no es una medida. Su
// historia (bug #14 de LaneQuant, s235) sigue en docs/BUG_HUNTING_QUEUE.md. Las constantes
// kLaneMode_* siguen en PrintConfig.hpp sólo porque la clave se sigue LEYENDO de ficheros viejos.

// NEOTKO_PATHBLEND_TAG — s316: aquí vivía compute_t_per_line() (s58), la gemela
// continua de compute_slot_per_line. Se BORRA porque no la llamaba nadie: cero
// referencias en src/ y en tests/, verificado antes de quitarla.
// 🚨 No re-crearla "para PathBlend". PathBlend NO saca su `t` de esta familia:
//   · ruta viva  = la ESCALERA, `_t_of()` en Fill.cpp, eje MEDIDO por PCA (s280e);
//   · ruta legacy = single-Fill ramp/cap, `surface_t` por bbox Y en GCode.cpp,
//     y sólo se alcanza con una config degenerada (mid_end <= floor).
// El plan de F2 daba por hecho que esta función alimentaba PathBlend. No era cierto.

// NEOTKO_COLORSTITCH_TAG_START — s316 fase B: RETIRADA DEL LEGACY al cargar.
// Decisión del usuario (s316): los repartos "de recuento" de ColorStitch son incorrectos visual y
// matemáticamente, y mueren. Un fichero viejo se CONVIERTE al abrirlo y se avisa (el aviso nombra
// la 2.4.5, la última con el sistema viejo). Qué se convierte:
//   · degradado (mode 1/2) con gradient_span_mm ausente o < 0 → 0 (campo, ajustar a la
//     superficie). Misma forma, construida por carriles.
//   · bandas en LÍNEAS (mode 3) → bandas en MM (mode 4), band_mm_x = band_count_x × paso.
//     ⚠️ NO es fiel y se sabe (§4 del plan de retirada): el paso sale del ancho de línea por
//     defecto del top, y un perfil pintado se comparte entre objetos con anchos distintos.
//     Decisión del usuario: UN valor para todos.
// Sitios que llaman aquí, uno por almacén: PrintConfigDef::handle_legacy_composite (toda config que
// se carga: presets, .ini, proyecto) y SurfaceEffectProfileManager::migrate_legacy_colorstitch
// (perfiles pintados y sus tres pilas, desde el loader del 3mf). El motor además sube a 0 cualquier
// gradient_span_mm < 0 que lea, por si algo se escapa.
// 🔑 Sólo cuenta como migración un cambio REAL: si no, cualquier perfil sin la clave dispararía el
// aviso sin motivo.
namespace ColorStitchLegacyMigration {
    // Paso real nominal del TOP en mm, con la MISMA fórmula que el preview
    // (weave_top_line_spacing): top_surface_line_width → line_width → nozzle×1.125, y luego
    // ancho − capa·(1−π/4) (Flow::rounded_rectangle_extrusion_spacing).
    double nominal_top_line_spacing_mm(const ConfigBase& cfg, double nozzle_mm);
    // Payload clave→valor (perfil pintado o pase de una pila). true si cambió algo.
    // `changed_roles` (opcional) recibe qué cambió, como máscara de kRole* de abajo.
    // `count = false` NO toca el informe: un mismo degradado vive a la vez en el payload del perfil
    // y en su pila de pases, y contarlo en los dos almacenes daba el DOBLE en el aviso (s316: 50
    // "degradados" para 25 perfiles). Quien migra varios almacenes junta las máscaras y llama a note().
    enum : unsigned { kRoleTopGradient = 1u, kRolePenuGradient = 2u, kRoleTopBands = 4u, kRolePenuBands = 8u };
    bool   migrate_kv(std::map<std::string, std::string>& kv, double sp_mm,
                      unsigned* changed_roles = nullptr, bool count = true);
    // Suma al informe a mano (para quien migró con count = false).
    void   note(int gradients, int bands);
    // Config (preset / proyecto / objeto). true si cambió algo.
    // `sp_mm` <= 0 → el paso sale de la propia cfg. s317: la config de un OBJETO es un delta sin
    // anchos de línea; quien la migra pasa el paso de la config del proyecto.
    bool   migrate_config(DynamicPrintConfig& cfg, double sp_mm = -1.0);
    // Informe para el aviso. Plater lo pone a cero antes de cargar y lo lee después: los presets
    // que se migran al ARRANCAR la app no deben disparar el aviso del primer proyecto.
    // s317: `sandwich_sources` = proyecto/objetos cuya receta del Sandwich Editor se movió;
    // `sandwich_profiles` = perfiles NUEVOS en la paleta (menos si había uno igual).
    struct Report { int gradients = 0; int bands = 0; int sandwich_sources = 0; int sandwich_profiles = 0; };
    void   reset_report();
    void   note_sandwich(int sources, int profiles);
    Report take_report();

    // NEOTKO_SANDWICH_TAG — s317 fase D: el Sandwich Editor DESAPARECE. Su receta (la que el preset
    // aplicaba sola a toda superficie sin pintar) pasa a ser un perfil más de la paleta y se repinta
    // a mano. Decisión del usuario: no hay "paint all".
    // 🚨 Y SE APAGA en la config: si no, resolve() la seguiría aplicando en silencio a lo no pintado.
    // "Sandwich activo" = lo que resolve() daría: blobs neotko_surface_passes_top/penu y, si están
    // sin autorar, los cuatro interruptores legacy (multipass_enabled, penultimate_multipass_enabled,
    // interlayer_colormix_enabled, multipass_path_gradient). Apagar = los dos blobs a "" y los
    // cuatro a false (también los leen la torre, GCode.cpp y la puerta del modo preset).
    // Estas cuatro son LIGERAS (sin ColorStitch.cpp): Preset.cpp las llama y el validador lo enlaza.
    bool   sandwich_active(const ConfigBase& cfg);
    bool   switch_off_sandwich(DynamicPrintConfig& cfg);
    // Preset de usuario cargado del disco: no tiene paleta donde dejar la receta. Decisión del
    // usuario (s317): se apaga y se avisa al arrancar (GUI_App::post_init). Sólo en memoria.
    bool   switch_off_sandwich_preset(DynamicPrintConfig& cfg, const std::string& preset_name);
    std::vector<std::string> take_sandwich_presets();
}
// NEOTKO_COLORSTITCH_TAG_END — s316 fase B

} // namespace Slic3r

// NEOTKO_DEBUG_TAG_START
// NEOTKO_LOG(CHANNEL, stream_expr) — write a debug line to a channel's log file.
// Usage (from any function inside namespace Slic3r):
//   NEOTKO_LOG(COLORSTITCH,    "layer=" << layer_idx << " fills=" << n);
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

#endif // slic3r_ColorStitch_hpp_
