// NEOTKO_PRECISIONALH_TAG_START
#ifndef slic3r_GLGizmoPrecisionALH_hpp_
#define slic3r_GLGizmoPrecisionALH_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/ColorSci/ColorHeightEnvelope.hpp"
#include "libslic3r/ColorSci/ColorTopZoneProxy.hpp"

#include <vector>

namespace Slic3r { namespace GUI {

// Precision Adaptive Layer Height: point-based curve editor for
// ModelObject::layer_height_profile. Replaces the free-hand brush of the
// stock "Layers editing" tool (GLCanvas3D::LayersEditing, untouched by this
// gizmo) with exact (Z, height) control points and a per-segment tension
// (blend between a straight line and a Fritsch-Carlson monotone cubic, so it
// never overshoots past the two endpoint heights of a segment).
//
// LibreMode-gated (Tier B), single condition, same pattern as
// GLGizmoAlignStack::on_is_activable() — see
// docs/WIP/PRECISION_ADAPTIVE_LAYER_HEIGHT_PLAN.md §3.2.
class GLGizmoPrecisionALH : public GLGizmoBase
{
public:
    GLGizmoPrecisionALH(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    struct ALHPoint
    {
        double z_mm;
        double height_mm;
        double tension; // tension of the segment starting at this point, [0,1]; unused on the last point
    };

    // Session state: which object this in-progress edit belongs to, and the
    // control points themselves. Sorted by z; front().z_mm == 0,
    // back().z_mm == object height. Not reloaded from the model on every
    // toggle of the gizmo over the same object (in-progress edits survive a
    // close/reopen); reset when the selected object changes.
    bool                   m_have_session = false;
    int                    m_object_idx   = -1;
    double                 m_object_height = 0.0;       // == SlicingParameters::object_print_z_uncompensated_height()
    double                 m_first_layer_height = 0.2;  // the fixed first layer; the profile MUST start at this height
    SlicingParameters      m_slicing_params;
    std::vector<ALHPoint>  m_points;

    // NEOTKO_ALHCOLOR_TAG — Fase 1+2 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md §5).
    // When on: render_curve_editor() shades the forbidden
    // [nozzle_min,h_min)/(h_max,nozzle_max] bands and the h_opt line (Fase 1),
    // AND clamps drag / click-to-add / build_profile_vector() to the active
    // envelope so the profile that reaches the slicer always respects color
    // (Fase 2, plan §4.b/4.d). Persists as a viewing preference — not reset
    // on object switch or close.
    bool m_adapt_to_color = false;

    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md,
    // Frente 1). Cached in ensure_session() (mesh-only, no slicing) — see
    // ColorTopZoneProxy.hpp for what this approximates. Scopes the TD/fidelity-floor term of
    // the color envelope to plausible Sandwich zones instead of the whole object.
    std::vector<Slic3r::ColorSci::TopZoneBand> m_top_zone_bands;

    // NEOTKO_ALHCOLOR_TAG — Fase 5.1 (Frente 2, plan "sequential-yawning-stream"). Slope
    // bands from the SAME cached facet pass as m_top_zone_bands (one scan, two outputs —
    // compute_object_zone_scan()). Each band stores its steepest tan_alpha; the exposed
    // ledge d = h(z) * tan_alpha is evaluated per frame against the LIVE curve height, so
    // the shading tracks the drag in real time without rescanning the mesh. Visualization
    // only in this phase — no clamping, no color decisions (those are Fase 5.2+).
    std::vector<Slic3r::ColorSci::SlopeZoneBand> m_slope_bands;
    double m_perimeter_width_mm = 0.45; // frPerimeter flow width, pre-slice (config-only)
    int    m_wall_loops         = 2;

    // NEOTKO_ALHCOLOR_TAG — Fase 5.2bis (Frente 2). The pattern-resolution ceiling passed
    // to compute_color_height_envelope(). Fase 0 hardcoded its 0.16 default without knowing
    // it was borrowing mixed_filament_height_upper_bound (the Local-Z / "Subdivide Mix
    // Layer" system's own upper bound, PrintConfig.cpp) — now read live from config in
    // ensure_session() so a user-tuned bound actually reaches the envelope. Session-level
    // staleness, same class as m_perimeter_width_mm/m_wall_loops above.
    double m_mix_band_upper_mm  = 0.16;

    // Steepest tan_alpha among slope bands covering z_mm; 0.0 = no slope band there.
    double slope_tan_at(double z_mm) const;

    // NEOTKO_ALHCOLOR_TAG — Fase 5.3 (Frente 2). Opt-in for PERSISTING the slope-recolor
    // plan: while off (default) everything slope-related stays pure visualization and
    // commit() actively erases any stored blob; while on, every commit() rewrites
    // neotko_slope_perimeter_recolor in ModelObject::config from the current profile
    // (inside the same undo snapshot as the profile itself — plan §5.3). The engine
    // ignores the blob entirely until Fase 5.4 lands, so this toggle is inert data-wise
    // today. Session preference like m_adapt_to_color, not persisted itself.
    bool m_slope_recolor_enabled = false;

    // Serializes the per-band recolor plan as a JSON array
    // [{"z_lo":mm,"z_hi":mm,"tools":[t0,...]},...] (object-relative Z, 0-based tools,
    // outer ring first). Empty string when there is nothing to store (no pattern, no
    // exposing band at the committed heights). Band height sampled at the band's Z
    // midpoint from the emitted profile — same approximation tier as the scan itself.
    std::string build_slope_recolor_blob() const;

    // NEOTKO_ALHCOLOR_TAG — Fase 5.1+5.2 (Frente 2). Info-panel block for the focused
    // point: how many interior rings its slope exposes at its current height (5.1), and —
    // when the object prints a MixedFilament pattern — the read-only suggested ring-color
    // assignment from ColorSci::resolve_perimeter_colors() with its achieved dE2000 (5.2).
    // The target color is the recipe's own answer for the OUTER ring at that Z
    // (MixedFilamentManager::resolve_perimeter — the main fork's recipe is consumed, never
    // recalculated or approximated), candidates are the recipe's physical palette
    // (component_a/b + gradient components). Preview only: nothing is stored (5.3) and the
    // engine never sees it (5.4). Draws nothing when the Z has no slope band, the height
    // exposes no ring, or the object has no mixed-filament pattern.
    void render_slope_recolor_preview(double z_mm, double point_height_mm);

    // NEOTKO_ALHCOLOR_TAG — s222. The selected object's virtual mixed-filament ID (its own
    // or a part volume's extruder pointing at a mixed entry), 0 = none. Re-fetches the
    // ModelObject like every other method here.
    unsigned int object_mixed_filament_id() const;
    // The recipe's physical palette for that mixed ID as 0-based tools (component_a/b +
    // gradient components), deduplicated, capped to tools 0..3. Empty for mixed_id == 0.
    std::vector<unsigned int> pattern_component_tools(unsigned int mixed_id) const;

    // NEOTKO_ALHCOLOR_TAG — s222 fix ("slope zone turned solid"). The recolor TARGET is the
    // recipe's intended MIX color — what the pattern tries to look like from outside
    // (ratio_a:ratio_b weighted blend of the component colors; gradient rows with 3+
    // components weigh them equally) — NOT the solid tool of any single layer. The old
    // target (resolve_perimeter at the band's mid z) returned whichever component that one
    // layer happened to cycle to, so the solver's "perfect" answer was all-rings-that-tool
    // and the whole band collapsed to a solid color. Returns false when mixed_id has no
    // resolvable recipe.
    bool recipe_target_rgb(unsigned int mixed_id, const Slic3r::ColorSci::Material mats[4],
                           float out_rgb[3]) const;

    // Drag-in-progress state (local mutation, not yet committed/undo-snapshotted).
    int m_dragging_point = -1; // index into m_points, -1 = none
    int m_hover_point    = -1; // index of the point under the cursor in the 2D band, -1 = none
                                // (drives both the numeric label and the 3D Z-band highlight)

    // 3D Z-band highlight on the object (built in world coords, flat shader) —
    // rebuilt only when the affected point/Z changes (m_band_key).
    GLModel m_band_model;
    double  m_band_key = -1.0;

    // The bottom control point (index 0, z=0) is pinned to the fixed first
    // layer height and is not editable — see load/seed and build_profile_vector.
    bool point_is_locked(int idx) const { return idx == 0; }

    void ensure_session();
    void seed_flat_profile();
    // Rebuilds m_points from an existing ModelObject::layer_height_profile (one
    // control point per stored (z,h) pair, straight segments) so reopening the
    // gizmo — or a fresh session after a project reload — doesn't discard a
    // profile that was already committed. Falls back to seed_flat_profile() for
    // an empty profile, or one too dense to be a reasonable set of control
    // points (i.e. written by the stock brush, not this gizmo).
    void load_points_from_profile(const std::vector<coordf_t>& profile);

    // Fritsch-Carlson monotone tangents for the current m_points (one per point).
    std::vector<double> compute_tangents() const;
    // Height at parametric t in [0,1] along segment [seg_idx, seg_idx+1], blending
    // linear (tension=0) and the monotone cubic Hermite curve (tension=1).
    // NEOTKO_ALHCOLOR_TAG — Fase 2: optional color_env narrows the valid range
    // from [min_layer_height,max_layer_height] to the active color envelope, so
    // the live-drawn curve tracks the envelope every frame (not just after the
    // next drag/commit — e.g. right after a TD/paint edit). nullptr or
    // passthrough == old nozzle-only behavior, unchanged.
    double blended_height(size_t seg_idx, double t, const std::vector<double>& tangents,
                           const Slic3r::ColorSci::ColorHeightEnvelope* color_env = nullptr) const;
    // Final [z0,h0,z1,h1,...] vector consumed as-is by generate_object_layers().
    std::vector<coordf_t> build_profile_vector() const;

    // Takes an undo snapshot, writes the profile into ModelObject::layer_height_profile
    // and schedules a re-slice — same commit path as the stock pincel/dialog.
    void commit(const std::string& snapshot_name);

    // Builds a translucent horizontal slab (world coords) at the object's Z of
    // control point `idx`, spanning its XY footprint — the "affected zone".
    void build_band_model(int idx);

    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope, Frente 1. Worst-case across
    // the 4 configured filament slots (Nivel 0, unchanged — still needed for
    // h_max/the pattern-resolution ceiling, which IS object-wide, real physics
    // outside Sandwich too). But the TD/opacity fidelity-floor term is now
    // scoped to `z_mm` against m_top_zone_bands: outside a plausible Sandwich
    // zone, ctx.mats[].td[] is zeroed so fidelity_floor collapses to 0 there —
    // TD/opacity is only ever real inside the Sandwich recipe, never for
    // MixedFilament's normal pattern coloring (confirmed by the project owner,
    // s220). Returns false only if there is no valid object to inspect;
    // otherwise fills `out` (out.passthrough == true when the object has no
    // ColorStitch paint/stickers AND Mixed Filament Object mode is off).
    // Re-resolves the ModelObject fresh each call —
    // no cached ModelObject*, matching every other method in this class.
    // Cheap (app_config string reads + facet-emptiness checks + an interval
    // lookup against the already-cached m_top_zone_bands, no slicing) — safe
    // to call every frame per plan §6 R3.
    bool compute_active_color_envelope(Slic3r::ColorSci::ColorHeightEnvelope& out, double z_mm) const;

    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope, Frente 1. Split out of
    // compute_active_color_envelope() so render_curve_editor()'s per-sample
    // shading loop (up to ~kDrawSamples * segment-count calls per FRAME, not
    // per commit) doesn't redo the has_color/app_config/material_from_hex
    // resolution on every sample — that part doesn't depend on Z at all.
    // resolve_color_context() does the Z-independent work once; the cheap,
    // genuinely-O(1) per-Z work (the Sandwich-zone gate + the envelope math
    // itself) lives in color_envelope_for_z(), fed a copy of the resolved ctx.
    bool resolve_color_context(Slic3r::ColorSci::ColorHeightContext& ctx) const;
    Slic3r::ColorSci::ColorHeightEnvelope color_envelope_for_z(Slic3r::ColorSci::ColorHeightContext ctx, double z_mm) const;

    void render_curve_editor(float width, float height);
};

}} // namespace Slic3r::GUI

#endif
// NEOTKO_PRECISIONALH_TAG_END
