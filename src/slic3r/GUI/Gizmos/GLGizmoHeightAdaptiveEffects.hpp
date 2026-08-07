// NEOTKO_HAE_TAG_START — Height Adaptive Effects gizmo (s247).
// docs/FUTURE/HEIGHT_ADAPTIVE_EFFECTS_PLAN.md §7.
#ifndef slic3r_GLGizmoHeightAdaptiveEffects_hpp_
#define slic3r_GLGizmoHeightAdaptiveEffects_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Feature/HeightAdaptive/HeightCurve.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Curve editor for the Height Adaptive Effects: several slicer parameters, each driven by its
// own Z → value curve, edited OVER THE OBJECT'S REAL LAYER BANDS. That last part is the whole
// point of the gizmo (plan §7): Orca's stock height-range modifiers already vary settings by
// height, but you type a Z blind and find out where the transition actually landed only after
// slicing. Here you see the layer you are affecting while you affect it.
//
// Structure cloned from GLGizmoPrecisionALH: same session-per-object model, same
// click-to-add / drag / right-click-to-delete, same commit-into-an-undo-snapshot path. The
// differences that matter:
//  - N curves share ONE Z axis, drawn as side-by-side columns over the same layer bands.
//  - Each curve carries its own interpolation mode, chosen among the ones its effect allows
//    (a s247 revision of plan §2 — see EffectDef::default_interp for why the original blanket
//    ban on smooth infill-width curves was replaced by a measured warning).
//  - The curves live in ModelObject config (coString), not in a member of ModelObject, so
//    undo/redo and 3mf round-trip come for free — see the plan §4 and the s238 orphan-recipe
//    lesson.
//
// LibreMode-gated, single condition, same pattern as GLGizmoPrecisionALH::on_is_activable().
class GLGizmoHeightAdaptiveEffects : public GLGizmoBase
{
public:
    GLGizmoHeightAdaptiveEffects(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    // ---------------------------------------------------------------- effect registry
    // One row per curve. Static, tiny, and deliberately data-driven: adding the third effect
    // of plan §11 (fuzzy that fades out, gradient infill) should be one entry here plus its
    // engine hook, not another gizmo.
    struct EffectDef
    {
        const char*            config_key;
        const char*            label;      // translated at use site
        const char*            units;
        // s247 REVISION of plan §2 ("the effect fixes the mode, not the user"). The mode is now
        // a property of each CURVE, chosen by the user among the modes the effect allows. The
        // reason: whether a smooth sparse-infill-width ramp is safe depends on how steep that
        // particular curve is, not on the parameter it drives — a slow ramp shifts consecutive
        // layers by a fraction of a micron. So instead of forbidding it we measure it and show
        // the number (misalignment_per_layer_mm). `default_interp` is what a new curve starts
        // as, and what a curve saved before the mode token existed is read as.
        HeightAdaptive::Interp default_interp;
        bool                   mode_selectable;
        double                 range_lo;   // editor axis, effect units
        double                 range_hi;
        double                 soft_lo;    // outside [soft_lo, soft_hi] the value is warned about,
        double                 soft_hi;    // never blocked — plan §7 "information, not permission"
        bool                   env_gated;  // NEOTKO_HAE_EXPANSION_TAG — hidden unless the env var is set
        bool                   is_expansion; // drives the overhang-angle readout
        bool                   is_fuzzy;     // drives the "the curve scales it, it does not switch it on" notice
    };

    static const std::vector<EffectDef>& effects();

    // NEOTKO_HAE_EXPANSION_TAG — plan §1/§3. Read ONCE into a static const: the XY Expansion
    // rows are not drawn unless ORCA_DEBUG_XYPROFILE is exported. This is a UI gate only —
    // the engine always honours a curve it finds in the config, otherwise a 3mf carrying one
    // would silently print differently on a machine without the variable, which is a far worse
    // failure than an undocumented feature being visible in the source.
    static bool expansion_effect_enabled();

    struct Node
    {
        double z_mm    = 0.;
        double value   = 0.;
        double tension = 0.5; // Smooth only
    };

    struct EffectState
    {
        std::vector<Node>      nodes;   // empty = effect off
        HeightAdaptive::Interp interp = HeightAdaptive::Interp::Stepped;
        // 2.0 — the effect is IN THIS OBJECT'S LIST. Distinct from "has a curve": an effect the
        // user just added has no points yet and must still hold its row, otherwise the "+" does
        // nothing visible. Not persisted on purpose: what a 3mf stores is curves, and an added
        // effect with fewer than two points is not a curve — commit() erases its key. So on
        // reload the list is exactly "the effects this object actually drives", which is the
        // honest reconstruction. See load_curves_from_object().
        bool                   added = false;
    };

    HeightAdaptive::Interp interp_of(size_t effect_idx) const;
    bool                   effect_in_use(size_t effect_idx) const;

    // Selectable at all on this machine — the env gate of the Expansion rows, in one place so
    // the "+" menu and the list cannot disagree about it.
    bool effect_available(size_t effect_idx) const;

    // The rows the panel draws: the effects added to THIS object, in registry order. 2.0 §10 of
    // ADAPTIVE_EFFECTOR_PLAN.md — the panel must stop growing by one row every time a candidate
    // gets wired, so it lists what is used, not what exists.
    std::vector<size_t> listed_effects() const;

    // Deletes the effector: drops the row, wipes the curve and erases the config key (commit()
    // does the erasing). This is what "Clear" never was — Clear empties the curve but the effect
    // stays on screen forever.
    void remove_effect(size_t effect_idx);

    // ONE graph at a time — the active effect, full width.
    //
    // This retires the side-by-side columns of plan §7 ("varios efectos, un solo eje Z
    // compartido"). Two reasons, both from using it: at three columns the value-axis labels
    // already collided into unreadable mush (-2.00.002.00), and the plan's own escape hatch
    // ("if they don't visually fit together, there are too many") triggers well before the six
    // effects this is heading for. The Z axis is still shared — it is the same axis, and the
    // same real layer bands, whichever effect you switch to; you just read them one at a time.
    std::vector<size_t>    visible_columns() const;

    // ---------------------------------------------------------------- session
    bool              m_have_session  = false;
    int               m_object_idx    = -1;
    double            m_object_height = 0.;
    double            m_first_layer_height = 0.2;
    SlicingParameters m_slicing_params;
    // Top Z of every real layer of the object, object-relative and raft-excluded — the same
    // coordinate system as Layer::slice_z, which is what the engine evaluates the curves at.
    // Adaptive layer height included: these are the bands the user actually gets.
    std::vector<double> m_layer_tops;
    std::vector<EffectState> m_states; // parallel to effects()

    int  m_active_effect = -1;   // which column takes the full-contrast curve and the info panel
    bool m_snap_to_layer = true; // forced on for Stepped effects

    // Interaction state (local, not yet committed).
    int m_drag_effect = -1;
    int m_drag_node   = -1;
    int m_hover_effect = -1;
    int m_hover_node   = -1;

    // 3D Z-band highlight on the object, same mechanism as GLGizmoPrecisionALH.
    GLModel m_band_model;
    double  m_band_key = -1.;

    void ensure_session();
    void rebuild_layer_bands();
    void load_curves_from_object();

    // Nearest real layer boundary to z (top of a layer). Returns z unchanged when there are
    // no bands yet. Mandatory for Stepped effects (plan §6.2): a step that falls mid-layer is
    // a step the user cannot see and cannot predict.
    double snap_z(double z) const;

    // Number of real layers strictly inside [z_lo, z_hi) — drives the "band shorter than 4
    // layers" warning of plan §6.2.
    int layers_between(double z_lo, double z_hi) const;

    HeightAdaptive::HeightCurve curve_of(size_t effect_idx) const;

    // Writes every effect's curve into the ModelObject config inside ONE undo snapshot, then
    // schedules a re-slice. An empty curve ERASES its key rather than storing "" so an object
    // that never had a curve stays byte-identical to one whose curve was deleted.
    void commit(const std::string& snapshot_name);

    void build_band_model(double z_mm);

    // Local slope of an Expansion curve at z, expressed as the wall angle from vertical in
    // degrees. Purely informational (plan §7): >45° is reported, never blocked.
    double overhang_angle_deg(size_t effect_idx, double z_mm) const;

    // Seeds an effect from one of the presets of plan §5/§8-F5.
    void apply_preset(size_t effect_idx, const char* preset_id);

    // Rewrites a smooth curve as a staircase of `bands` steps that follows its shape, with every
    // step edge snapped to a real layer boundary. This is how you get "looks like a ramp" without
    // giving up the per-layer alignment sparse infill depends on: inside each band the spacing is
    // constant, so the lines still stack.
    void quantize_to_steps(size_t effect_idx, int bands);
    int  m_quantize_bands = 6;
    // The numeric point table is exact but dense; off by default, opened on demand.
    bool m_show_points    = false;
    // The "+" candidate chooser, drawn inline under the list while it is open.
    bool m_show_add       = false;

    // Estimated XY shift between two CONSECUTIVE layers caused by the width ramp, at the far
    // edge of the object — the number that decides whether a smooth curve is safe here. Returns
    // 0 for stepped curves and for effects that don't drive infill spacing.
    // out_pct: the same shift as a percentage of the line width at that Z.
    double misalignment_per_layer_mm(size_t effect_idx, double* out_pct) const;

    // Draws the shared-Z multi-column editor and runs all the interaction.
    void render_editor(float width, float height);

    // The whole panel below GizmoImguiBegin(). Split out of on_render_input_window() in s248 so
    // the style push/pop around it stays symmetric: this function has three early exits, and a
    // `return` past a PopStyleColor leaks the gizmo's colours into every other ImGui window.
    void render_panel_body(float win_w);

    // s248 — the panel never shrinks WITHIN a session, only grows. Effects differ in how many
    // optional rows they show (mode radio, shift warning, tension sliders), and with an
    // AlwaysAutoResize window that made the panel jump size every time you switched effect. The
    // content is unchanged; it is padded up to the tallest it has been for this object, and reset
    // when the session changes.
    float m_body_max_h = 0.f;
};

}} // namespace Slic3r::GUI

#endif
// NEOTKO_HAE_TAG_END
