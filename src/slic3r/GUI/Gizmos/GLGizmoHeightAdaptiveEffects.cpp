// NEOTKO_HAE_TAG_START — see GLGizmoHeightAdaptiveEffects.hpp.
#include "GLGizmoHeightAdaptiveEffects.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "GizmoNeotkoStyle.hpp" // NEOTKO_GIZMOSTYLE_TAG — shared palette, s248

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/NeoDebug.hpp" // NEOTKO_HAE_EXPANSION_TAG — ORCA_DEBUG_PROFILE gates the Expansion rows

#include <imgui/imgui.h>
#include <GL/glew.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdlib>

namespace Slic3r { namespace GUI {

// NEOTKO_HAE_EXPANSION_TAG — s247: the discretion gate of plan §1/§3 hangs off the EXISTING
// ORCA_DEBUG_PROFILE channel instead of a private variable of its own. Decision of the project
// owner, and the better engineering too: that is the channel already exported day to day, so
// the rows appear as part of the debug session that is running anyway, and there is one less
// magic string to go dig out of the source. ORCA_DEBUG_ALL opens it for free.
//
// Unlike NeoDebug::render_panels_enabled() (s229), being covered by ORCA_DEBUG_ALL is fine
// here: the cost is two extra rows inside a gizmo the user deliberately opened, not floating
// windows landing on top of the model.
//
// ⚠️ UI ONLY. The engine has never been gated and must not be: a 3mf carrying an Expansion
// curve prints identically on every machine, with or without the variable. Hiding a row is
// discretion; hiding the effect would be a file that prints differently depending on someone's
// shell, which is a far worse failure than an undocumented feature.
bool GLGizmoHeightAdaptiveEffects::expansion_effect_enabled()
{
    return NeoDebug::enabled(NeoDebug::PROFILE);
}

const std::vector<GLGizmoHeightAdaptiveEffects::EffectDef>& GLGizmoHeightAdaptiveEffects::effects()
{
    // Order matters only for the on-screen column order. Ranges are the EDITOR axis, not a
    // hard constraint on the engine: the soft_* pair is what gets warned about.
    static const std::vector<EffectDef> defs = {
        // NEOTKO_HAE_EXPANSION_TAG — the two undocumented rows (plan §1). Smooth: the whole
        // point of the effect is the continuous bullnose that S3D could only staircase.
        // Range is ±2 mm, and 2 is a MEASURED ceiling, not a round number: past it the sparse
        // infill regions fail to be generated (project owner, from an older fork). Below it the
        // engine copes fine, so the axis covers the whole usable domain and stops exactly where
        // it stops working. No soft-warning band: the real risk of a big offset is fusing
        // details or losing small islands, which depends on the geometry and not on a fixed
        // number — that one is caught after slicing by the islands-lost notification.
        { "neotko_hae_xy_contour", L("XY contour"), "mm", HeightAdaptive::Interp::Smooth,
          /*mode_selectable*/ true, -2.0, 2.0, -2.0, 2.0, /*env_gated*/ true,  /*is_expansion*/ true, /*is_fuzzy*/ false },
        { "neotko_hae_xy_hole",    L("XY hole"),    "mm", HeightAdaptive::Interp::Smooth,
          /*mode_selectable*/ true, -2.0, 2.0, -2.0, 2.0, /*env_gated*/ true,  /*is_expansion*/ true, /*is_fuzzy*/ false },
        // Stepped by DEFAULT, not by decree — the s247 revision of plan §6.2. Sparse infill
        // spacing is held constant across layers on purpose so the lines stack; a width that
        // drifts moves them. But what breaks the stacking is the drift BETWEEN CONSECUTIVE
        // LAYERS, and that is proportional to how steep the curve is, not to the fact that it is
        // a curve at all. A ramp of 0.05 mm over 40 layers shifts each layer by a fraction of a
        // micron. So: smooth is allowed, and misalignment_per_layer_mm() puts the actual number
        // on screen — information, not permission, like everything else here.
        { "neotko_hae_infill_width", L("Sparse infill width"), "mm", HeightAdaptive::Interp::Stepped,
          /*mode_selectable*/ true, 0.2, 0.8, 0.3, 0.7, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ false },
        // Effect C — fuzzy skin that is born and dies with the height (plan §11). Ramp by
        // default and no step-alignment worry at all: this is surface texture, there is nothing
        // stacking layer on layer to keep aligned. Range [0, 1] mm is the bound the underlying
        // fuzzy_skin_thickness key already declares (PrintConfig.cpp), not a new invention;
        // the soft limit is that tooltip's own advice — stay below the outer wall line width.
        { "neotko_hae_fuzzy_thickness", L("Fuzzy skin thickness"), "mm", HeightAdaptive::Interp::Smooth,
          /*mode_selectable*/ true, 0.0, 1.0, 0.0, 0.6, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ true },

        // ---- LEVEL 0 (s248, ADAPTIVE_EFFECTOR_PLAN.md §4) ----
        // No new engine work: these ride the two hooks the effects above already installed. They
        // only became worth adding once the panel listed what you USE instead of what exists,
        // which is why the add/remove list came first.
        //
        // ⚠️ THREE widths only, and that is a decision, not an oversight (project owner, s248).
        // Internal solid infill and top surface were wired, looked at, and taken back out: a ramp
        // over them varies the width of the surfaces the eye reads as flat, which is not something
        // any print wants. LayerRegion::flow() still resolves all five roles, so re-adding one is
        // a case in its switch plus a key.
        //
        // Stepped by default for exactly the reason sparse infill width is — anything that stacks
        // layer on layer wants a constant spacing inside each band. Ranges mirror the sane span of
        // the keys they drive; they are the editor axis, not a clamp.
        { "neotko_hae_outer_wall_width", L("Outer wall width"), "mm", HeightAdaptive::Interp::Stepped,
          /*mode_selectable*/ true, 0.2, 0.8, 0.3, 0.7, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ false },
        { "neotko_hae_inner_wall_width", L("Inner wall width"), "mm", HeightAdaptive::Interp::Stepped,
          /*mode_selectable*/ true, 0.2, 0.8, 0.3, 0.7, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ false },

        // The four extra fuzzy parameters. Smooth, and no alignment worry at all: this is surface
        // texture, nothing stacks on it. is_fuzzy is set so they inherit the "fuzzy skin is off for
        // this object, so this curve does nothing" notice, which is just as true for them.
        // ⚠️ is_fuzzy does NOT mean "reaching 0 switches fuzzy off" — that is only the thickness,
        // and the notice is keyed on the config key, not on this flag (see the panel).
        { "neotko_hae_fuzzy_point_distance", L("Fuzzy skin point distance"), "mm", HeightAdaptive::Interp::Smooth,
          /*mode_selectable*/ true, 0.05, 2.0, 0.1, 1.5, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ true },
        { "neotko_hae_fuzzy_scale", L("Fuzzy skin noise scale"), "mm", HeightAdaptive::Interp::Smooth,
          /*mode_selectable*/ true, 0.1, 10.0, 0.1, 10.0, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ true },
        // Integer key: a curve over it is a staircase whatever the mode says, so it does not get
        // to pretend otherwise. The engine rounds; the editor says steps.
        { "neotko_hae_fuzzy_octaves", L("Fuzzy skin noise octaves"), "", HeightAdaptive::Interp::Stepped,
          /*mode_selectable*/ false, 1.0, 8.0, 1.0, 8.0, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ true },
        { "neotko_hae_fuzzy_persistence", L("Fuzzy skin noise persistence"), "", HeightAdaptive::Interp::Smooth,
          /*mode_selectable*/ true, 0.0, 1.0, 0.0, 1.0, /*env_gated*/ false, /*is_expansion*/ false, /*is_fuzzy*/ true },
    };
    return defs;
}

GLGizmoHeightAdaptiveEffects::GLGizmoHeightAdaptiveEffects(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoHeightAdaptiveEffects::on_init()
{
    m_shortcut_key = 0; // no global shortcut
    return true;
}

std::string GLGizmoHeightAdaptiveEffects::on_get_name() const
{
    return _u8L("Height Adaptive Effects");
}

bool GLGizmoHeightAdaptiveEffects::on_is_activable() const
{
    // Single LibreMode gate, same pattern as GLGizmoPrecisionALH. The object-selection check
    // is an ordinary precondition (the curves are per-ModelObject), not a second gate.
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled")
        && m_parent.get_selection().get_object_idx() >= 0;
}

void GLGizmoHeightAdaptiveEffects::on_set_state()
{
    if (get_state() == Off) {
        m_have_session = false;
        m_drag_effect  = -1;
        m_drag_node    = -1;
        m_show_add     = false;
    }
}

// ------------------------------------------------------------------------------- session

void GLGizmoHeightAdaptiveEffects::ensure_session()
{
    const Selection& sel = m_parent.get_selection();
    const int   obj_idx = sel.get_object_idx();
    const Model* model  = sel.get_model();
    if (obj_idx < 0 || model == nullptr || obj_idx >= (int)model->objects.size()) {
        m_have_session = false;
        return;
    }
    if (m_have_session && obj_idx == m_object_idx)
        return; // same object: keep in-progress edits across a panel redraw

    m_object_idx = obj_idx;
    const ModelObject* mo = model->objects[obj_idx];

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    m_slicing_params = PrintObject::slicing_parameters(full_config, *mo, (float)mo->max_z(), Vec3d::Ones());

    // Same two engine-exact values GLGizmoPrecisionALH uses: object_print_z_uncompensated_height()
    // is the Z the profile is validated against, and first_object_layer_height is the fixed
    // first layer. The gizmo's Z axis is object-relative and raft-excluded — the SAME coordinate
    // system as Layer::slice_z, which is what the engine evaluates every curve at
    // (PrintObjectSlice.cpp new_layers(): slice_z = mid-layer, zmin added only to print_z).
    m_object_height      = m_slicing_params.object_print_z_uncompensated_height();
    m_first_layer_height = m_slicing_params.first_object_layer_height;
    if (m_first_layer_height <= 0.)
        m_first_layer_height = m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2;

    rebuild_layer_bands();
    load_curves_from_object();

    if (m_states.size() != effects().size())
        m_states.resize(effects().size());
    // The previous object's active row means nothing here: this object has its own list, which
    // may well be empty. Land on its first row, or on nothing at all.
    {
        const std::vector<size_t> listed = listed_effects();
        m_active_effect = listed.empty() ? -1 : (int)listed.front();
    }
    m_body_max_h   = 0.f; // a new object starts from its own natural height, not the last one's
    m_have_session = true;
}

void GLGizmoHeightAdaptiveEffects::rebuild_layer_bands()
{
    // 🔑 Plan §7: these must be the REAL layers, adaptive height included — that is the entire
    // reason this gizmo exists instead of typing a Z into a height-range modifier. So we ask the
    // same function the slicer asks, fed the object's own layer height profile when it has one
    // (i.e. when Precision ALH has been used on it).
    m_layer_tops.clear();
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[m_object_idx];
    if (!m_slicing_params.valid || m_object_height <= 0.)
        return;

    std::vector<coordf_t> profile = mo->layer_height_profile.get();
    // The profile is only usable if it spans exactly 0 .. object height — that is the same
    // hard constraint PrintObject::update_layer_height_profile enforces before the slicer will
    // touch it, and generate_object_layers() below assumes it holds. A profile that fails it is
    // one the slicer would discard too, so falling back to flat shows the user the bands they
    // will actually get rather than bands derived from a profile nobody will honour.
    const bool profile_usable = profile.size() >= 4 && profile.size() % 2 == 0
        && profile.front() <= 1e-6
        && std::abs(profile[profile.size() - 2] - m_object_height) < 1e-3;
    if (!profile_usable) {
        // No adaptive profile on this object: a flat one, in the [z0,h0,z1,h1] form
        // generate_object_layers() consumes.
        double h = m_slicing_params.layer_height;
        if (h <= 0.)
            h = m_slicing_params.max_layer_height > 0. ? m_slicing_params.max_layer_height : 0.2;
        profile = { 0., m_first_layer_height, m_object_height, h };
    }

    const bool precise_z = wxGetApp().preset_bundle->full_config().opt_bool("precise_z_height");
    // Pairs of (bottom, top) per layer, object-relative, raft excluded.
    const std::vector<coordf_t> bounds = generate_object_layers(m_slicing_params, profile, precise_z);
    m_layer_tops.reserve(bounds.size() / 2);
    for (size_t i = 1; i < bounds.size(); i += 2)
        m_layer_tops.emplace_back(double(bounds[i]));
}

HeightAdaptive::Interp GLGizmoHeightAdaptiveEffects::interp_of(size_t effect_idx) const
{
    return effect_idx < m_states.size() ? m_states[effect_idx].interp : effects()[effect_idx].default_interp;
}

bool GLGizmoHeightAdaptiveEffects::effect_in_use(size_t effect_idx) const
{
    // Two nodes is the threshold a curve needs to be committed at all (commit() erases anything
    // shorter), so it is also the honest definition of "this effect is in use".
    return effect_idx < m_states.size() && m_states[effect_idx].nodes.size() >= 2;
}

bool GLGizmoHeightAdaptiveEffects::effect_available(size_t effect_idx) const
{
    return effect_idx < effects().size()
        && (!effects()[effect_idx].env_gated || expansion_effect_enabled());
}

std::vector<size_t> GLGizmoHeightAdaptiveEffects::listed_effects() const
{
    std::vector<size_t> out;
    for (size_t e = 0; e < effects().size() && e < m_states.size(); ++ e)
        if (m_states[e].added && effect_available(e))
            out.push_back(e);
    return out;
}

void GLGizmoHeightAdaptiveEffects::remove_effect(size_t effect_idx)
{
    if (effect_idx >= m_states.size())
        return;
    m_states[effect_idx].nodes.clear();
    m_states[effect_idx].added  = false;
    m_states[effect_idx].interp = effects()[effect_idx].default_interp;
    // Any interaction in flight belonged to a row that no longer exists.
    if (m_drag_effect == (int)effect_idx)  { m_drag_effect = -1; m_drag_node = -1; }
    if (m_hover_effect == (int)effect_idx) { m_hover_effect = -1; m_hover_node = -1; }
    if (m_active_effect == (int)effect_idx) {
        const std::vector<size_t> rest = listed_effects();
        m_active_effect = rest.empty() ? -1 : (int)rest.front();
    }
    // commit() erases the key of every effect whose curve is shorter than two nodes, so this is
    // all it takes for the object to become byte-identical to one that never had the effect.
    commit(_u8L("Height adaptive effects - Remove effect"));
}

std::vector<size_t> GLGizmoHeightAdaptiveEffects::visible_columns() const
{
    // Just the active one. Kept as a vector so the editor keeps its column loop and adding a
    // side-by-side comparison mode later stays a one-line change here.
    std::vector<size_t> cols;
    const size_t e = size_t(std::max(0, m_active_effect));
    if (m_active_effect >= 0 && e < m_states.size() && m_states[e].added && effect_available(e))
        cols.push_back(e);
    return cols;
}

void GLGizmoHeightAdaptiveEffects::load_curves_from_object()
{
    const Model* model = m_parent.get_selection().get_model();
    m_states.assign(effects().size(), EffectState{});
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[m_object_idx];

    for (size_t e = 0; e < effects().size(); ++ e) {
        const EffectDef& def = effects()[e];
        m_states[e].interp = def.default_interp;
        if (!mo->config.has(def.config_key))
            continue;
        const auto* opt = dynamic_cast<const ConfigOptionString*>(mo->config.option(def.config_key));
        if (opt == nullptr || opt->value.empty())
            continue;
        const auto curve = HeightAdaptive::HeightCurve::parse(opt->value, def.default_interp);
        // The stored curve carries its own mode; a curve written before the mode token existed
        // parses back as the effect's default, which is exactly what it was evaluated as.
        m_states[e].interp = curve.interp();
        for (const HeightAdaptive::Node& n : curve.nodes())
            m_states[e].nodes.push_back(Node{ n.z, n.value, n.tension });
        // 2.0 — the list is reconstructed from the curves the object really carries. A key with
        // fewer than two nodes cannot exist in a committed object (commit() erases it), so this
        // is not a heuristic: an effect is listed iff it drives something.
        m_states[e].added = m_states[e].nodes.size() >= 2;
    }
}

double GLGizmoHeightAdaptiveEffects::snap_z(double z) const
{
    if (m_layer_tops.empty())
        return z;
    auto it = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z);
    if (it == m_layer_tops.begin())
        return *it;
    if (it == m_layer_tops.end())
        return m_layer_tops.back();
    const double hi = *it, lo = *(it - 1);
    return (z - lo <= hi - z) ? lo : hi;
}

int GLGizmoHeightAdaptiveEffects::layers_between(double z_lo, double z_hi) const
{
    if (m_layer_tops.empty() || z_hi <= z_lo)
        return 0;
    const auto lo = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z_lo);
    const auto hi = std::lower_bound(m_layer_tops.begin(), m_layer_tops.end(), z_hi);
    return int(std::distance(lo, hi));
}

HeightAdaptive::HeightCurve GLGizmoHeightAdaptiveEffects::curve_of(size_t effect_idx) const
{
    HeightAdaptive::HeightCurve curve(interp_of(effect_idx));
    if (effect_idx >= m_states.size())
        return curve;
    std::vector<HeightAdaptive::Node> nodes;
    nodes.reserve(m_states[effect_idx].nodes.size());
    for (const Node& n : m_states[effect_idx].nodes)
        nodes.push_back(HeightAdaptive::Node{ n.z_mm, n.value, n.tension });
    curve.set_nodes(std::move(nodes));
    return curve;
}

void GLGizmoHeightAdaptiveEffects::commit(const std::string& snapshot_name)
{
    if (!m_have_session || m_object_idx < 0)
        return;
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx >= (int)model->objects.size())
        return;

    wxGetApp().plater()->take_snapshot(snapshot_name);
    ModelObject* mo = const_cast<ModelObject*>(model->objects[m_object_idx]);

    for (size_t e = 0; e < effects().size(); ++ e) {
        const EffectDef& def = effects()[e];
        // A curve needs at least two nodes to mean anything as a curve; a single node is a
        // constant, which is a job for a plain per-object override, not for this gizmo.
        const std::string blob = m_states[e].nodes.size() >= 2 ? curve_of(e).serialize() : std::string();
        if (blob.empty())
            // ERASE, don't store "": an object that never had a curve and one whose curve was
            // deleted must be indistinguishable, both in the 3mf and in the preset diff.
            mo->config.erase(def.config_key);
        else
            mo->config.set_key_value(def.config_key, new ConfigOptionString(blob));
    }

    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(m_object_idx);
}

// ------------------------------------------------------------------------------- 3D band

void GLGizmoHeightAdaptiveEffects::build_band_model(double z_mm)
{
    m_band_model.reset();

    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[m_object_idx];

    BoundingBoxf3 bb;
    for (size_t i = 0; i < mo->instances.size(); ++ i)
        bb.merge(mo->instance_bounding_box(i));
    if (!bb.defined)
        return;

    const double zc  = bb.min.z() + z_mm;
    const double pad = 1.5;
    const double ht  = std::max(0.15, 0.5 * m_slicing_params.layer_height);
    const double x0 = bb.min.x() - pad, x1 = bb.max.x() + pad;
    const double y0 = bb.min.y() - pad, y1 = bb.max.y() + pad;
    const double z0 = zc - ht,          z1 = zc + ht;

    indexed_triangle_set its;
    its.vertices = {
        Vec3f((float)x0, (float)y0, (float)z0), Vec3f((float)x1, (float)y0, (float)z0),
        Vec3f((float)x1, (float)y1, (float)z0), Vec3f((float)x0, (float)y1, (float)z0),
        Vec3f((float)x0, (float)y0, (float)z1), Vec3f((float)x1, (float)y0, (float)z1),
        Vec3f((float)x1, (float)y1, (float)z1), Vec3f((float)x0, (float)y1, (float)z1),
    };
    const int box_tris[12][3] = {
        {0,1,2},{0,2,3}, {4,6,5},{4,7,6},
        {0,4,5},{0,5,1}, {1,5,6},{1,6,2},
        {2,6,7},{2,7,3}, {3,7,4},{3,4,0},
    };
    its.indices.reserve(12);
    for (const auto& t : box_tris)
        its.indices.emplace_back(t[0], t[1], t[2]);
    m_band_model.init_from(its);
}

void GLGizmoHeightAdaptiveEffects::on_render()
{
    // Same intent as the ALH gizmo's band: show, on the object itself, which slice of it the
    // focused node affects. World-space + flat shader (a screen-space ImGui projection was
    // unstable under perspective/retina there, and would be here too).
    const int eff  = (m_drag_effect >= 0) ? m_drag_effect : m_hover_effect;
    const int node = (m_drag_effect >= 0) ? m_drag_node   : m_hover_node;
    if (!m_have_session || eff < 0 || eff >= (int)m_states.size())
        return;
    if (node < 0 || node >= (int)m_states[eff].nodes.size())
        return;

    const double z_mm = m_states[eff].nodes[node].z_mm;
    const double key  = eff * 1e6 + z_mm;
    if (key != m_band_key) {
        build_band_model(z_mm);
        m_band_key = key;
    }
    if (!m_band_model.is_initialized())
        return;

    const bool dragging = (m_drag_effect >= 0);
    m_band_model.set_color(dragging ? ColorRGBA(1.00f, 0.75f, 0.16f, 0.42f)
                                    : ColorRGBA(0.00f, 0.59f, 0.53f, 0.30f));

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    m_band_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// ------------------------------------------------------------------------------- info

double GLGizmoHeightAdaptiveEffects::overhang_angle_deg(size_t effect_idx, double z_mm) const
{
    // Plan §7, "information, not permission": an Expansion curve that grows fast with Z is a
    // wall leaning outwards. The local slope d(offset)/dz IS the tangent of the wall angle
    // measured from vertical, so we can report the overhang the user is drawing, live, without
    // slicing anything — and without blocking them.
    const auto curve = curve_of(effect_idx);
    if (curve.empty())
        return 0.;
    const double dz = std::max(1e-3, 0.02 * std::max(m_object_height, 1.));
    const double v0 = curve.at(std::max(0., z_mm - dz), 0.);
    const double v1 = curve.at(z_mm + dz, 0.);
    return std::atan(std::abs(v1 - v0) / (2. * dz)) * 180. / M_PI;
}

void GLGizmoHeightAdaptiveEffects::apply_preset(size_t effect_idx, const char* preset_id)
{
    if (effect_idx >= m_states.size() || m_object_height <= 0.)
        return;
    const double h = m_object_height;
    std::vector<Node>& n = m_states[effect_idx].nodes;
    n.clear();

    const std::string id = preset_id;
    if (id == "bullnose") {
        // The §5.1 reference shape, normalized to this object's height: widest at the equator,
        // closing towards both ends.
        n.push_back(Node{ 0.,        0.0,  0.5 });
        n.push_back(Node{ 0.45 * h,  0.6,  0.5 });
        n.push_back(Node{ h,         0.0,  0.5 });
    } else if (id == "chamfer") {
        // Straight taper: tension 0 makes the segment exactly linear, which is what a chamfer
        // means — no easing, no surprises.
        n.push_back(Node{ 0.,        0.5,  0.0 });
        n.push_back(Node{ 0.30 * h,  0.0,  0.0 });
        n.push_back(Node{ h,         0.0,  0.0 });
    } else if (id == "barrel") {
        n.push_back(Node{ 0.,       -0.2,  1.0 });
        n.push_back(Node{ 0.50 * h,  0.4,  1.0 });
        n.push_back(Node{ h,        -0.2,  1.0 });
    } else if (id == "fine_bottom") {
        // Effect B (§6.1): thin lines low down where the sparse infill has to support the
        // solid above it, coarse lines in the deep interior where nothing rests on it.
        n.push_back(Node{ 0.,        0.30, 0.0 });
        n.push_back(Node{ 0.35 * h,  0.55, 0.0 });
    } else if (id == "fine_top") {
        n.push_back(Node{ 0.,        0.55, 0.0 });
        n.push_back(Node{ 0.65 * h,  0.30, 0.0 });
    } else if (id == "fade_in") {
        // Smooth bottom that grows texture as it rises.
        n.push_back(Node{ 0.,        0.0,  0.7 });
        n.push_back(Node{ h,         0.35, 0.7 });
    } else if (id == "fade_out") {
        n.push_back(Node{ 0.,        0.35, 0.7 });
        n.push_back(Node{ h,         0.0,  0.7 });
    } else if (id == "fade_band") {
        // Texture that is born and dies: smooth, rough in the middle, smooth again.
        n.push_back(Node{ 0.,        0.0,  0.8 });
        n.push_back(Node{ 0.5 * h,   0.35, 0.8 });
        n.push_back(Node{ h,         0.0,  0.8 });
    }

    // The shape a preset describes implies its mode: the expansion presets are continuous
    // profiles (a bullnose drawn as steps is not a bullnose), the infill ones are bands.
    m_states[effect_idx].interp = effects()[effect_idx].is_expansion || effects()[effect_idx].is_fuzzy
                                      ? HeightAdaptive::Interp::Smooth : HeightAdaptive::Interp::Stepped;
    if (interp_of(effect_idx) == HeightAdaptive::Interp::Stepped || m_snap_to_layer)
        for (Node& node : n)
            node.z_mm = snap_z(node.z_mm);
}

void GLGizmoHeightAdaptiveEffects::quantize_to_steps(size_t effect_idx, int bands)
{
    // "I want a ramp, not a jump" without giving up per-layer alignment: sample the smooth curve
    // at `bands` equally spaced heights and emit a stepped curve through those samples, every
    // edge snapped to a real layer boundary. Inside each band the spacing is constant, so the
    // infill lines still stack; across the object it reads as a gradient instead of one cliff.
    if (effect_idx >= m_states.size() || m_object_height <= 0. || bands < 2)
        return;
    const auto curve = curve_of(effect_idx);
    if (curve.empty())
        return;

    std::vector<Node> out;
    out.reserve(size_t(bands));
    for (int i = 0; i < bands; ++ i) {
        // Sample at the MIDDLE of each band, not at its edge: the value a band gets should be
        // representative of the whole band, and an edge sample biases every band one way.
        const double z_lo = m_object_height * double(i)       / double(bands);
        const double z_hi = m_object_height * double(i + 1)   / double(bands);
        Node n;
        n.z_mm  = (i == 0) ? 0. : snap_z(z_lo);
        n.value = curve.at(0.5 * (z_lo + z_hi), 0.);
        n.tension = 0.;
        // snap_z can collapse two bands onto the same layer on a short object; keep the first.
        if (!out.empty() && n.z_mm <= out.back().z_mm + 1e-9)
            continue;
        out.push_back(n);
    }
    if (out.size() >= 2) {
        m_states[effect_idx].nodes  = std::move(out);
        m_states[effect_idx].interp = HeightAdaptive::Interp::Stepped;
    }
}

double GLGizmoHeightAdaptiveEffects::misalignment_per_layer_mm(size_t effect_idx, double* out_pct) const
{
    if (out_pct != nullptr)
        *out_pct = 0.;
    // Only meaningful for the ONE effect that drives infill spacing — matched by key rather than
    // by "not expansion", so adding a fourth effect cannot silently inherit a number that means
    // nothing for it (fuzzy skin thickness is surface texture: nothing stacks, nothing to align).
    if (effect_idx >= m_states.size()
        || std::string(effects()[effect_idx].config_key) != "neotko_hae_infill_width")
        return 0.;
    if (interp_of(effect_idx) != HeightAdaptive::Interp::Smooth)
        return 0.;
    const auto curve = curve_of(effect_idx);
    if (curve.empty() || m_object_height <= 0.)
        return 0.;

    // Distance from the infill pattern's origin at which we report the shift. The pattern is
    // laid out across the object's footprint, so the worst case is the far edge — half the XY
    // diagonal is an honest stand-in for "the far side of this object".
    double far_dist = 0.;
    const Model* model = m_parent.get_selection().get_model();
    if (model != nullptr && m_object_idx >= 0 && m_object_idx < (int)model->objects.size()) {
        BoundingBoxf3 bb;
        for (size_t i = 0; i < model->objects[m_object_idx]->instances.size(); ++ i)
            bb.merge(model->objects[m_object_idx]->instance_bounding_box(i));
        if (bb.defined)
            far_dist = 0.5 * std::hypot(bb.size().x(), bb.size().y());
    }
    if (far_dist <= 0.)
        return 0.;

    const double layer_h = m_slicing_params.layer_height > 0. ? m_slicing_params.layer_height : 0.2;
    double worst = 0., worst_pct = 0.;
    constexpr int kSamples = 64;
    for (int i = 0; i < kSamples; ++ i) {
        const double z  = m_object_height * double(i) / double(kSamples - 1);
        const double dz = std::max(1e-3, 0.5 * layer_h);
        const double w0 = curve.at(std::max(0., z - dz), 0.);
        const double w1 = curve.at(std::min(m_object_height, z + dz), 0.);
        const double w  = curve.at(z, 0.);
        if (w <= 1e-6)
            continue;
        // Width change over one layer, as a fraction of the width: that fraction is exactly how
        // much the Nth line of the pattern moves per unit distance from the origin.
        const double rel_per_layer = std::abs(w1 - w0) / (2. * dz) * layer_h / w;
        const double shift         = far_dist * rel_per_layer;
        if (shift > worst) {
            worst     = shift;
            worst_pct = 100. * shift / w;
        }
    }
    if (out_pct != nullptr)
        *out_pct = worst_pct;
    return worst;
}

// ------------------------------------------------------------------------------- editor

void GLGizmoHeightAdaptiveEffects::render_editor(float width, float height)
{
    ImGuiIO&     io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hae_band", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    // IsItemDeactivated(), not a raw mouse-up flag: the exact release frame can be missed if
    // nothing forces a repaint right then, which is what silently dropped commits in the ALH
    // gizmo before its fix. Same trap, same answer.
    const bool deactivated = ImGui::IsItemDeactivated();
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    const std::vector<size_t> cols = visible_columns();
    if (cols.empty())
        return;

    const double z_max = std::max(m_object_height, 1e-6);
    const float  gap   = 8.0f;
    const float  col_w = (width - gap * float(cols.size() - 1)) / float(cols.size());

    auto col_x0 = [&](size_t ci) { return p0.x + float(ci) * (col_w + gap); };
    auto z_to_y  = [&](double z) { return p0.y + float(1.0 - z / z_max) * height; };
    auto y_to_z  = [&](float y)  { return z_max * (1.0 - std::clamp(double(y - p0.y) / double(height), 0.0, 1.0)); };

    // --------------------------------------------------------------- shared layer bands
    // 🔑 The real layers, drawn ONCE across the full widget: that is what makes the Z axis
    // genuinely shared between the columns. Skipped when there are so many layers that the
    // lines would merge into a solid block and tell the user nothing.
    const bool draw_bands = !m_layer_tops.empty() && m_layer_tops.size() <= size_t(height * 0.75f);
    // Rounded base first, then the vertical gradient inset by a pixel: AddRectFilledMultiColor has
    // no rounding parameter, so this is how you get both. The gradient is what stops the graph
    // reading as a flat black hole punched in the panel.
    neo_draw_canvas(dl, p0, width, height);
    if (draw_bands) {
        for (size_t i = 0; i < m_layer_tops.size(); ++ i) {
            const float y = z_to_y(m_layer_tops[i]);
            dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p0.x + width - 1.f, y),
                        (i % 5 == 4) ? neo_fade(NeoCol::GridMajor, 0.55f) : neo_col_u32(NeoCol::Grid), 1.0f);
        }
    }
    // The fixed first layer: it cannot be renegotiated by anything, so it gets its own marker.
    if (!m_layer_tops.empty()) {
        const float y = z_to_y(m_layer_tops.front());
        dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p0.x + width - 1.f, y), neo_col_u32(NeoCol::GridMajor), 1.5f);
    }
    // ---- Z scale down the left edge ----
    // New in s248: the graph had a labelled VALUE axis and no labelled Z axis at all, so the one
    // thing the gizmo exists to show — which height you are at — had to be read by hovering. Five
    // ticks, drawn under everything else, with the same dark strip trick the value labels use so
    // they survive a curve passing behind them.
    for (int t = 0; t <= 4; ++ t) {
        const double z = z_max * double(t) / 4.;
        const float  y = std::clamp(z_to_y(z), p0.y + 1.f, p0.y + height - 1.f);
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%.1f", z);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        const float  ly  = std::clamp(y - tsz.y * 0.5f, p0.y + 2.f, p0.y + height - tsz.y - 2.f);
        dl->AddRectFilled(ImVec2(p0.x + 2.f, ly - 1.f), ImVec2(p0.x + 6.f + tsz.x, ly + tsz.y + 1.f),
                          neo_fade(NeoCol::Canvas, 0.72f), 3.f);
        dl->AddText(ImVec2(p0.x + 4.f, ly), neo_fade(NeoCol::TextDim, 0.75f), lbl);
    }
    // Plan §7: the raft offset is not part of this Z axis (the engine evaluates the curves at
    // Layer::slice_z, which excludes the raft) — say so instead of leaving the user to guess.

    // --------------------------------------------------------------- per-column curves
    int new_hover_effect = -1, new_hover_node = -1;

    for (size_t ci = 0; ci < cols.size(); ++ ci) {
        const size_t     e   = cols[ci];
        const EffectDef& def = effects()[e];
        const float      x0  = col_x0(ci);
        const float      x1  = x0 + col_w;
        const bool       is_active = (m_active_effect == (int)e);
        const bool       stepped   = interp_of(e) == HeightAdaptive::Interp::Stepped;

        auto v_to_x = [&](double v) {
            return x0 + float((v - def.range_lo) / (def.range_hi - def.range_lo)) * col_w;
        };
        auto x_to_v = [&](float x) {
            return def.range_lo + std::clamp(double(x - x0) / double(col_w), 0.0, 1.0) * (def.range_hi - def.range_lo);
        };

        dl->AddRect(ImVec2(x0, p0.y), ImVec2(x1, p0.y + height),
                    is_active ? neo_fade(NeoCol::Accent, 0.75f) : neo_col_u32(NeoCol::Surface),
                    6.0f, 0, is_active ? 1.5f : 1.0f);

        // Value axis. Without this you can see the SHAPE of the curve but not what it is worth,
        // which was the single most confusing thing about the first version: five ticks with the
        // two ends and the middle labelled, so a glance tells you both where and how much.
        {
            constexpr int kTicks = 8; // gridlines; only some of them get a number
            const float   lbl_h  = ImGui::GetTextLineHeight();
            // A strip behind the numbers: they sit inside the plot, and over a curve or the
            // soft-limit shading they were unreadable.
            dl->AddRectFilled(ImVec2(x0 + 1.f, p0.y + 1.f), ImVec2(x1 - 1.f, p0.y + lbl_h + 3.f),
                              neo_fade(NeoCol::Canvas, 0.82f), 5.f, ImDrawCornerFlags_Top);
            for (int t = 0; t <= kTicks; ++ t) {
                const double v  = def.range_lo + (def.range_hi - def.range_lo) * double(t) / double(kTicks);
                const float  xv = v_to_x(v);
                const bool   edge = (t == 0 || t == kTicks);
                dl->AddLine(ImVec2(xv, p0.y + lbl_h + 3.f), ImVec2(xv, p0.y + height - 1.f),
                            edge ? neo_fade(NeoCol::GridMajor, 0.6f) : neo_col_u32(NeoCol::Grid), 1.0f);
                // Label the ends and the quarters. Ends are ALIGNED to their edge rather than
                // centred on it: centring is what made "-2.00" and "0.00" run into each other
                // in a narrow column.
                if (t != 0 && t != kTicks && (t * 4) % kTicks != 0)
                    continue;
                char lbl[24];
                std::snprintf(lbl, sizeof(lbl), "%.2f", v);
                const ImVec2 tsz = ImGui::CalcTextSize(lbl);
                float lx = (t == 0)      ? x0 + 3.f
                         : (t == kTicks) ? x1 - tsz.x - 3.f
                                          : xv - tsz.x * 0.5f;
                lx = std::clamp(lx, x0 + 3.f, x1 - tsz.x - 3.f);
                dl->AddText(ImVec2(lx, p0.y + 2.f), neo_fade(NeoCol::TextDim, 0.9f), lbl);
            }
        }

        // Zero / neutral reference line, when it falls inside the axis.
        if (def.range_lo < 0. && def.range_hi > 0.) {
            const float xz = v_to_x(0.);
            dl->AddLine(ImVec2(xz, p0.y), ImVec2(xz, p0.y + height), neo_fade(NeoCol::GridMajor, 0.7f), 1.0f);
        }

        // Soft-limit shading — a warning surface, not a wall (plan §7). Now a GRADIENT that fades
        // in towards the edge instead of a flat slab: the old solid red blocks read as "forbidden",
        // which is the opposite of what this is supposed to say.
        const ImU32 warn_edge = neo_fade(NeoCol::Warn, 0.16f);
        const ImU32 warn_none = neo_fade(NeoCol::Warn, 0.f);
        if (def.soft_lo > def.range_lo)
            dl->AddRectFilledMultiColor(ImVec2(x0 + 1.f, p0.y + 1.f), ImVec2(v_to_x(def.soft_lo), p0.y + height - 1.f),
                                        warn_edge, warn_none, warn_none, warn_edge);
        if (def.soft_hi < def.range_hi)
            dl->AddRectFilledMultiColor(ImVec2(v_to_x(def.soft_hi), p0.y + 1.f), ImVec2(x1 - 1.f, p0.y + height - 1.f),
                                        warn_none, warn_edge, warn_edge, warn_none);

        const auto  curve  = curve_of(e);
        const ImU32 curve_col = is_active ? neo_col_u32(NeoCol::AccentBright) : neo_fade(NeoCol::Accent, 0.45f);
        const ImU32 glow_col  = neo_fade(NeoCol::AccentBright, is_active ? 0.20f : 0.08f);
        const ImU32 fill_col  = is_active ? neo_col_u32(NeoCol::AccentGhost) : neo_fade(NeoCol::AccentBright, 0.06f);
        // The curve is drawn value-on-X, so "under the curve" means between it and the LEFT edge of
        // the axis. One convex quad per sample: AddConvexPolyFilled cannot take the whole ribbon
        // (it is not convex), but each trapezoid between two consecutive samples is.
        const float fill_x0 = x0 + 1.f;
        auto ribbon = [&](float xa, float ya, float xb, float yb) {
            dl->AddQuadFilled(ImVec2(fill_x0, ya), ImVec2(xa, ya), ImVec2(xb, yb), ImVec2(fill_x0, yb), fill_col);
        };

        if (!curve.empty() && m_states[e].nodes.size() >= 2) {
            if (stepped) {
                // Draw the steps as steps. A stepped curve rendered as a ramp would be a lie
                // about the only thing that matters for this effect (plan §6.2).
                const auto& ns = curve.nodes();
                for (size_t i = 0; i < ns.size(); ++ i) {
                    const float xv   = v_to_x(ns[i].value);
                    const float ytop = z_to_y(i + 1 < ns.size() ? ns[i + 1].z : z_max);
                    const float ybot = z_to_y(i == 0 ? 0. : ns[i].z);
                    // Each band is a plain rectangle of value — say so, it is the honest picture.
                    dl->AddRectFilled(ImVec2(fill_x0, ytop), ImVec2(xv, ybot), fill_col);
                    dl->AddLine(ImVec2(xv, ybot), ImVec2(xv, ytop), glow_col, 5.0f);
                    dl->AddLine(ImVec2(xv, ybot), ImVec2(xv, ytop), curve_col, 2.0f);
                    if (i + 1 < ns.size()) {
                        // The transition layer — exactly one, and the user should see which.
                        const float xv2 = v_to_x(ns[i + 1].value);
                        dl->AddLine(ImVec2(xv, ytop), ImVec2(xv2, ytop), curve_col, 2.0f);
                        dl->AddLine(ImVec2(x0 + 1.f, ytop), ImVec2(x1 - 1.f, ytop),
                                    neo_fade(NeoCol::Warn, 0.35f), 1.0f);
                    }
                }
            } else {
                constexpr int kSamples = 120;
                // Collected first, then drawn as ONE polyline: AddPolyline joins the segments, so
                // the stroke has no notches at the sample points the way 120 separate AddLine
                // calls did at any real thickness.
                ImVec2 pts[kSamples + 1];
                for (int s = 0; s <= kSamples; ++ s) {
                    const double z = z_max * double(s) / double(kSamples);
                    pts[s] = ImVec2(v_to_x(curve.at(z, 0.)), z_to_y(z));
                    if (s > 0)
                        ribbon(pts[s - 1].x, pts[s - 1].y, pts[s].x, pts[s].y);
                }
                dl->AddPolyline(pts, kSamples + 1, glow_col,  ImDrawFlags_None, 6.0f);
                dl->AddPolyline(pts, kSamples + 1, curve_col, ImDrawFlags_None, 2.2f);
            }
        }

        // Nodes.
        for (size_t i = 0; i < m_states[e].nodes.size(); ++ i) {
            const Node&  n = m_states[e].nodes[i];
            const ImVec2 c(v_to_x(n.value), z_to_y(n.z_mm));
            const bool   soft_bad = n.value < def.soft_lo - 1e-9 || n.value > def.soft_hi + 1e-9;
            const bool   dragged  = (m_drag_effect == (int)e && m_drag_node == (int)i);
            if (is_active && hovered && std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) <= 8.0f) {
                new_hover_effect = (int)e;
                new_hover_node   = (int)i;
            }
            const bool  lit = dragged || (new_hover_effect == (int)e && new_hover_node == (int)i);
            const float r   = dragged ? 6.5f : (lit ? 6.0f : 4.5f);
            const ImU32 col = soft_bad ? neo_col_u32(NeoCol::Warn)
                                       : (is_active ? neo_col_u32(NeoCol::AccentBright)
                                                    : neo_fade(NeoCol::AccentBright, 0.45f));
            // Halo when live: the old version only grew the dot by 1.5 px, which on a retina
            // panel is not a state change you can see while dragging.
            neo_draw_node(dl, c, r, col, lit);
        }

        // ⚠️ A DRAG IN PROGRESS IS HANDLED FIRST, AND WITHOUT ASKING ABOUT HOVER. Once the
        // mouse is down the cursor routinely leaves the widget, and `hovered` goes false while
        // the drag is still perfectly alive — gating the drag/release on hover would drop the
        // release frame and with it the commit. That is exactly the bug that made ALH edits
        // silently not stick; the answer there and here is ImGui's own active-id state
        // (IsItemActive / IsItemDeactivated), never the cursor's position.
        if (m_drag_effect == (int)e) {
            if (ImGui::IsItemActive() && m_drag_node >= 0 && m_drag_node < (int)m_states[e].nodes.size()) {
                Node& n = m_states[e].nodes[(size_t)m_drag_node];
                n.value = x_to_v(io.MousePos.x);
                // Free Z while the button is held, snapped only on release: snapping every
                // frame makes the node stutter between bands and fight the cursor (the s222
                // lesson from ALH's hard clamp, applied before it bites).
                n.z_mm  = std::clamp(y_to_z(io.MousePos.y), 0., z_max);
                new_hover_effect = (int)e;
                new_hover_node   = m_drag_node;
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
            }
            if (deactivated) {
                if (m_drag_node >= 0 && m_drag_node < (int)m_states[e].nodes.size()) {
                    Node& n = m_states[e].nodes[(size_t)m_drag_node];
                    if (stepped || m_snap_to_layer)
                        n.z_mm = snap_z(n.z_mm);
                    std::stable_sort(m_states[e].nodes.begin(), m_states[e].nodes.end(),
                                     [](const Node& a, const Node& b) { return a.z_mm < b.z_mm; });
                    commit(_u8L("Height adaptive effects - Move point"));
                }
                m_drag_effect = -1;
                m_drag_node   = -1;
            }
            continue;
        }

        // Everything below STARTS an interaction, so it does need the cursor. Only the active
        // column takes input: with three columns in one widget, "whichever column the cursor
        // happens to be over" makes a mis-click on a neighbour trivially easy, and each of
        // those is an undo entry and a re-slice.
        if (!is_active || !hovered || m_drag_effect >= 0)
            continue;

        const bool in_col = io.MousePos.x >= x0 && io.MousePos.x <= x1;

        if (new_hover_node >= 0 && io.MouseClicked[1]) {
            m_states[e].nodes.erase(m_states[e].nodes.begin() + new_hover_node);
            new_hover_effect = -1;
            new_hover_node   = -1;
            commit(_u8L("Height adaptive effects - Delete point"));
        } else if (new_hover_node >= 0 && io.MouseClicked[0]) {
            m_drag_effect = (int)e;
            m_drag_node   = new_hover_node;
        } else if (new_hover_node < 0 && in_col && io.MouseClicked[0]) {
            Node n;
            n.z_mm  = y_to_z(io.MousePos.y);
            n.value = x_to_v(io.MousePos.x);
            if (stepped || m_snap_to_layer)
                n.z_mm = snap_z(n.z_mm);
            auto& ns = m_states[e].nodes;
            size_t at = ns.size();
            for (size_t i = 0; i < ns.size(); ++ i)
                if (n.z_mm < ns[i].z_mm) { at = i; break; }
            ns.insert(ns.begin() + at, n);
            commit(_u8L("Height adaptive effects - Add point"));
        }
    }

    m_hover_effect = new_hover_effect;
    m_hover_node   = new_hover_node;

    // Live crosshair readout: hovering ANY point of the active column tells you the Z you are on
    // and what the curve is worth there. Before this you only got a number by grabbing a node,
    // so "how much am I actually setting" had no answer while just looking.
    if (hovered && m_drag_effect < 0 && new_hover_node < 0 && m_active_effect >= 0) {
        for (size_t ci = 0; ci < cols.size(); ++ ci) {
            if ((int)cols[ci] != m_active_effect)
                continue;
            const EffectDef& def = effects()[cols[ci]];
            const float      x0  = col_x0(ci), x1 = x0 + col_w;
            if (io.MousePos.x < x0 || io.MousePos.x > x1)
                break;
            const double z    = y_to_z(io.MousePos.y);
            const auto   cur  = curve_of(cols[ci]);
            const double v_at = cur.empty() ? 0. : cur.at(z, 0.);
            dl->AddLine(ImVec2(x0 + 1.f, io.MousePos.y), ImVec2(x1 - 1.f, io.MousePos.y),
                        neo_fade(NeoCol::Ink, 0.22f), 1.0f);
            char buf[96];
            if (cur.empty())
                std::snprintf(buf, sizeof(buf), "Z %.2f mm", z);
            else
                std::snprintf(buf, sizeof(buf), "Z %.2f mm  →  %.3f %s", z, v_at, def.units);
            const ImVec2 tsz = ImGui::CalcTextSize(buf);
            const ImVec2 lp(std::clamp(io.MousePos.x + 10.f, p0.x, p0.x + width - tsz.x),
                            std::clamp(io.MousePos.y + 8.f, p0.y, p0.y + height - tsz.y));
            neo_draw_pill(dl, lp, buf, neo_fade(NeoCol::Accent, 0.5f));
            break;
        }
    }

    // --------------------------------------------------------------- readout
    const int reff = (m_drag_effect >= 0) ? m_drag_effect : m_hover_effect;
    const int rnod = (m_drag_effect >= 0) ? m_drag_node   : m_hover_node;
    if (reff >= 0 && rnod >= 0 && rnod < (int)m_states[reff].nodes.size()) {
        const Node& n = m_states[reff].nodes[(size_t)rnod];
        std::string label = "Z " + format((float)n.z_mm, 2) + " mm   "
                          + format((float)n.value, 3) + " " + effects()[reff].units;
        if (effects()[reff].is_expansion) {
            const double a = overhang_angle_deg((size_t)reff, n.z_mm);
            if (a > 0.5)
                label += "   " + _u8L("wall") + " " + format((float)a, 0) + "\xc2\xb0";
        }
        const ImVec2 tsz = ImGui::CalcTextSize(label.c_str());
        const ImVec2 lp(std::clamp(p0.x + width * 0.5f - tsz.x * 0.5f, p0.x, p0.x + width - tsz.x),
                        z_to_y(n.z_mm) - tsz.y - 10.0f);
        neo_draw_pill(dl, lp, label.c_str(), neo_col_u32(NeoCol::Accent));
    }

    // Column captions, drawn straight onto the draw list right under each column — laying
    // them out with SameLine()+SetCursorScreenPos() fights ImGui's own cursor and drifts as
    // soon as the label widths differ. Underlined with a short accent rule: it ties the caption
    // to its column and gives the block a baseline to sit on.
    for (size_t ci = 0; ci < cols.size(); ++ ci) {
        const EffectDef&  def   = effects()[cols[ci]];
        const bool        act   = (m_active_effect == (int)cols[ci]);
        const std::string label = _u8L(def.label);
        const ImVec2      tsz   = ImGui::CalcTextSize(label.c_str());
        const float       lx    = col_x0(ci) + std::max(0.f, (col_w - tsz.x) * 0.5f);
        dl->AddText(ImVec2(lx, p0.y + height + 5.f),
                    act ? neo_col_u32(NeoCol::AccentBright) : neo_col_u32(NeoCol::TextDim), label.c_str());
        if (act)
            dl->AddLine(ImVec2(lx, p0.y + height + tsz.y + 7.f), ImVec2(lx + tsz.x, p0.y + height + tsz.y + 7.f),
                        neo_fade(NeoCol::Accent, 0.6f), 1.5f);
    }
    // Reserve the row the captions were drawn into, so the widgets below don't overlap them.
    ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight() + 10.f));
}

// ------------------------------------------------------------------------------- panel

// Short explanatory text as a hover marker instead of a paragraph: useful the first time,
// clutter every time after (plan §11).
// ⚠️ ONLY EVER PUT THIS AFTER A SHORT, SINGLE-LINE LABEL. SameLine() lands it wherever the
// previous item ended, so behind a text that wrapped it starts at the right margin with no room
// left — and since the panel-wide wrap position applies to "(?)" as well, it then wraps ITSELF
// into three stacked lines. The -1 below stops that last part from ever happening again, but the
// real rule is the placement: long explanations go INSIDE the tooltip, and the marker hangs off
// the control's own short name.
static void help_marker(const std::string& text)
{
    ImGui::SameLine();
    ImGui::PushTextWrapPos(-1.f); // negative = no wrapping at all
    ImGui::TextDisabled("(?)");
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320.f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void GLGizmoHeightAdaptiveEffects::on_render_input_window(float x, float y, float bottom_limit)
{
    ensure_session();

    const float win_w = 540.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);

    // 🔑 Plan §11 — THE reason the panel used to grow sideways without limit. The window is
    // AlwaysAutoResize, so SetWindowSize() is only a request: any single item wider than win_w
    // wins, and the longest items here are the notice texts. TextWrapped alone does not fix it
    // either — with auto-resize, "wrap at the window width" is circular, because the window
    // width is whatever the text asks for. Pinning the wrap column to an ABSOLUTE x breaks the
    // loop, and it also makes the plain Text*/TextDisabled calls wrap, which they otherwise
    // never do (ImGui::TextEx honours DC.TextWrapPos for all of them).
    ImGui::PushTextWrapPos(win_w - 16.f);

    // s248 — the gizmo's own look, pushed here and popped below. This is deliberately the ONLY
    // place the style is touched: the body has three early exits, and a push scattered inside it
    // is a leak into every other ImGui window the moment one of them is taken. That is also why
    // the body is a separate function — it can `return` freely and the pops still happen.
    neo_push_panel_style();

    // s248 — the panel only ever GROWS within a session. Effects differ in how many optional rows
    // they show (mode radio, shift warning, tension sliders, points table), and with an
    // AlwaysAutoResize window that made the whole thing jump size on every effect switch. Measuring
    // the body and padding up to the tallest it has been for this object costs one Dummy and holds
    // the panel still. It is reset in ensure_session(), so a different object starts from its own
    // natural height rather than inheriting the previous one's.
    const float body_y0 = ImGui::GetCursorPosY();
    render_panel_body(win_w);
    const float body_h = ImGui::GetCursorPosY() - body_y0;
    m_body_max_h = std::max(m_body_max_h, body_h);
    if (m_have_session && body_h < m_body_max_h - 1.f)
        ImGui::Dummy(ImVec2(1.f, m_body_max_h - body_h));

    neo_pop_panel_style();
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
}

void GLGizmoHeightAdaptiveEffects::render_panel_body(float win_w)
{
    if (!m_have_session) {
        ImGui::TextWrapped("%s", _u8L("Select a single object to edit its height adaptive effects.").c_str());
        return;
    }

    // ---- the object's effect list (2.0) ----
    // The registry is no longer the list. The panel shows the effects ADDED TO THIS OBJECT, so
    // wiring a new candidate costs one entry in the "+" menu and zero pixels for everyone who
    // does not use it — which is the whole point: with a fixed registry every new effect made
    // the panel worse for every object that did not want it.
    const std::vector<size_t> listed = listed_effects();

    // Candidates = available on this machine and not already in the list.
    std::vector<size_t> candidates;
    for (size_t e = 0; e < effects().size(); ++ e)
        if (effect_available(e) && !(e < m_states.size() && m_states[e].added))
            candidates.push_back(e);

    ImGui::TextUnformatted(_u8L("Effects on this object").c_str());
    help_marker(_u8L("Click in the graph to add a point, drag to move it, right-click to delete it. "
                     "The horizontal lines are this object's real layers."));
    // The candidate chooser is an INLINE list, not an ImGui popup: no other gizmo in the tree
    // opens a popup from inside a GizmoImguiBegin window, and a popup is its own window with its
    // own lifetime rules — not worth finding out the hard way for a four-item menu.
    if (!candidates.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(((m_show_add ? _u8L("Cancel") : _u8L("Add")) + "##hae_add_btn").c_str()))
            m_show_add = !m_show_add;
    } else {
        m_show_add = false;
    }
    if (m_show_add) {
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        // Capped: the registry is now twelve entries and still growing, and a chooser taller than
        // the graph it is choosing for helps nobody. Past the cap it scrolls.
        const int shown = std::min<int>(int(candidates.size()), 6);
        ImGui::BeginChild("##hae_add_list",
                          ImVec2(win_w - 16.f, row_h * float(shown) + 2.f * ImGui::GetStyle().WindowPadding.y + 2.f),
                          true);
        for (size_t e : candidates) {
            if (ImGui::Selectable((_u8L(effects()[e].label) + "##hae_add_" + std::to_string(e)).c_str())) {
                m_states[e].added  = true;
                m_states[e].nodes.clear();
                m_states[e].interp = effects()[e].default_interp;
                m_active_effect    = (int)e;
                m_show_add         = false;
                // No commit: an effect with no points writes no key, so there is nothing to put
                // in an undo snapshot yet. The first point the user drops commits it.
            }
        }
        ImGui::EndChild();
    }

    if (listed.empty()) {
        ImGui::Dummy(ImVec2(0.f, 4.f));
        ImGui::TextWrapped("%s", _u8L("No effect on this object yet. Press Add to drive a setting with a "
                                       "curve along the object's height.").c_str());
        return;
    }

    if (m_active_effect < 0 || !(size_t(m_active_effect) < m_states.size() && m_states[m_active_effect].added))
        m_active_effect = (int)listed.front();

    // Each row states what that effect is worth right now — points, mode — because "which of
    // these am I actually using" was not answerable at a glance before. The trash button is per
    // row and deletes the effector itself, which "Clear" never did.
    int remove_at = -1;
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const int   shown = std::min<int>(int(listed.size()), 6);
        // The child's own frame eats WindowPadding top and bottom. Not accounting for it is what
        // put a scrollbar on a two-row list that fits perfectly well.
        const float child_h = row_h * float(shown) + 2.f * style.WindowPadding.y + 2.f;
        ImGui::BeginChild("##hae_effect_list", ImVec2(win_w - 16.f, child_h), true);
        // Reserved on the right for the delete button, measured once so every row lines up even
        // if a scrollbar appears mid-list.
        const float btn_w   = ImGui::CalcTextSize("x").x + 2.f * style.FramePadding.x;
        const float row_w   = ImGui::GetContentRegionAvail().x;
        for (size_t e : listed) {
            const EffectDef& def = effects()[e];
            // The mode word is gone from the row: the sparkline on the right shows a staircase or a
            // ramp, and that is a faster read than the word plus it leaves room for long labels.
            std::string row = _u8L(def.label);
            if (effect_in_use(e))
                row += "   " + std::to_string(m_states[e].nodes.size()) + " " + _u8L("pts");
            else
                row += "   " + _u8L("(no points yet)");
            // 🔑 AllowItemOverlap, in BOTH halves — this is why the delete button did nothing at
            // first. The Selectable spans the whole row, so the button sits INSIDE its rectangle,
            // and ImGui hands hover to whoever claimed the area first unless the first item
            // explicitly stands aside. The flag lets the Selectable be pushed off the hover, and
            // SetItemAllowOverlap() lets the item submitted AFTER it take that hover.
            if (ImGui::Selectable((row + "##hae_sel" + std::to_string(e)).c_str(), m_active_effect == (int)e,
                                  ImGuiSelectableFlags_AllowItemOverlap))
                m_active_effect = (int)e;
            ImGui::SetItemAllowOverlap();
            {
                // s248 — a per-row SPARKLINE of that effect's curve, plus an accent bar on the
                // selected row. Both are drawn over the Selectable's own rectangle, which is why
                // they cost no layout at all. The sparkline is the one thing that makes a list of
                // similarly-named effects readable at a glance: you recognise the SHAPE you drew
                // long before you finish reading "Fuzzy skin noise persistence".
                ImDrawList*  rdl = ImGui::GetWindowDrawList();
                const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                if (m_active_effect == (int)e)
                    rdl->AddRectFilled(ImVec2(rmin.x, rmin.y + 1.f), ImVec2(rmin.x + 2.5f, rmax.y - 1.f),
                                       neo_col_u32(NeoCol::AccentBright), 1.f);
                const auto  cur = curve_of(e);
                const float sw  = 46.f, pad = 4.f;
                const float sx1 = rmax.x - btn_w - 6.f, sx0 = sx1 - sw;
                const float sy0 = rmin.y + pad,        sy1 = rmax.y - pad;
                if (!cur.empty() && m_states[e].nodes.size() >= 2 && sx0 > rmin.x + 40.f && sy1 > sy0) {
                    const EffectDef& d   = effects()[e];
                    const double     span = std::max(1e-9, d.range_hi - d.range_lo);
                    // The sparkline is drawn the way a chart is (Z left→right, value bottom→top),
                    // NOT the way the big editor is (value on X). They answer different questions:
                    // this one is "which shape is this", the editor is "where exactly is this node".
                    constexpr int kS = 24;
                    ImVec2 sp[kS + 1];
                    for (int s = 0; s <= kS; ++ s) {
                        const double z = m_object_height * double(s) / double(kS);
                        const double v = std::clamp((cur.at(z, 0.) - d.range_lo) / span, 0.0, 1.0);
                        sp[s] = ImVec2(sx0 + sw * float(s) / float(kS), sy1 - float(v) * (sy1 - sy0));
                    }
                    rdl->AddPolyline(sp, kS + 1, m_active_effect == (int)e ? neo_col_u32(NeoCol::AccentBright)
                                                                           : neo_fade(NeoCol::AccentBright, 0.5f),
                                     ImDrawFlags_None, 1.6f);
                }
            }
            ImGui::SameLine(row_w - btn_w);
            ImGui::PushID(int(e));
            if (ImGui::SmallButton("x"))
                // Recorded, applied after the loop: removing here would mutate `listed` while
                // iterating it and commit() mid-list.
                remove_at = int(e);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Remove this effect from the object").c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (remove_at >= 0) {
        remove_effect(size_t(remove_at));
        return; // the rest of the panel described a row that is gone; redraw next frame
    }

    ImGui::Dummy(ImVec2(0.f, 4.f));
    render_editor(win_w - 16.0f, 260.0f);
    ImGui::Dummy(ImVec2(0.f, 6.f));

    if (m_active_effect >= 0 && m_active_effect < (int)effects().size()) {
        const size_t     e   = (size_t)m_active_effect;
        const EffectDef& def = effects()[e];
        // Not const: the mode radio right below can flip it, and everything after must reflect
        // the choice in THIS frame — otherwise the panel shows one frame of the old mode.
        bool             stepped = interp_of(e) == HeightAdaptive::Interp::Stepped;

        ImGui::Separator();
        ImGui::Text("%s", (_u8L("Range") + ": " + format((float)def.range_lo, 2) + " - "
                           + format((float)def.range_hi, 2) + " " + def.units).c_str());
        if (def.is_expansion)
            // Why the axis stops where it stops — a measured limit, worth stating so nobody
            // widens it later thinking it was an arbitrary round number. Tooltip rather than a
            // line of text: it answers a question about the range, right next to the range.
            help_marker(_u8L("Past 2 mm the sparse infill regions fail to generate. The limit is measured, "
                             "not a round number."));

        // ---- interpolation mode (s247 revision of plan §2: the curve owns the mode) ----
        if (def.mode_selectable) {
            int mode = stepped ? 0 : 1;
            ImGui::TextUnformatted(_u8L("Transition").c_str());
            // The explanation of the two modes belongs HERE, on the control that chooses between
            // them — not as a paragraph underneath. That paragraph is what wrapped and dragged
            // its own help marker off the edge of the panel.
            help_marker(_u8L("Steps: the value is constant inside each band and changes in one jump at a "
                             "layer boundary, and every point is snapped to a real layer. Safest for sparse "
                             "infill width, because the lines of every layer keep stacking on the ones "
                             "below.\n\nRamp: the value changes continuously with height."));
            ImGui::SameLine();
            bool changed = false;
            if (ImGui::RadioButton((_u8L("Steps") + "##hae_mode_step").c_str(), mode == 0)) {
                m_states[e].interp = HeightAdaptive::Interp::Stepped;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton((_u8L("Ramp") + "##hae_mode_smooth").c_str(), mode == 1)) {
                m_states[e].interp = HeightAdaptive::Interp::Smooth;
                changed = true;
            }
            if (changed) {
                // Switching to steps re-snaps every node: a step edge that falls mid-layer is a
                // step the user can neither see nor predict.
                if (m_states[e].interp == HeightAdaptive::Interp::Stepped)
                    for (Node& n : m_states[e].nodes)
                        n.z_mm = snap_z(n.z_mm);
                commit(_u8L("Height adaptive effects - Transition mode"));
            }
            stepped = interp_of(e) == HeightAdaptive::Interp::Stepped;
        }

        // Only Ramp has a choice to offer: Stepped snaps unconditionally (a step edge falling
        // mid-layer is a step the user can neither see nor predict), and that is now said in the
        // Transition tooltip rather than costing two lines of the panel forever.
        if (!stepped)
            ImGui::Checkbox(_u8L("Snap points to layer boundaries").c_str(), &m_snap_to_layer);

        // ---- the measured risk of a ramp, instead of a blanket ban ----
        // Gated on the same key check as misalignment_per_layer_mm() itself: the Staircase tool
        // and the shift warning are both about infill line alignment and mean nothing elsewhere.
        if (!stepped && std::string(def.config_key) == "neotko_hae_infill_width") {
            double pct = 0.;
            const double shift = misalignment_per_layer_mm(e, &pct);
            if (shift > 0.) {
                // One line, not a paragraph: the NUMBER is the message, the reasoning belongs in
                // the tooltip. Six lines of orange prose was the bulk of the panel's height and
                // stopped being read after the first time.
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s: %.3f mm (%.0f%%)",
                              _u8L("Line shift per layer").c_str(), shift, pct);
                const bool risky = pct > 25.;
                if (risky) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.15f, 1.f));
                ImGui::TextUnformatted(buf);
                if (risky) ImGui::PopStyleColor();
                help_marker(_u8L("How far the infill lines of one layer land from the ones below, at the far "
                                 "edge of the object, as a percentage of the line width. Above roughly 25% "
                                 "the sparse infill stops stacking. Flatten the curve, or press Staircase to "
                                 "keep the shape with aligned bands."));
            }

            // ---- ramp → aligned staircase ----
            ImGui::SetNextItemWidth(110.f);
            ImGui::SliderInt((_u8L("Bands") + "##hae_bands").c_str(), &m_quantize_bands, 2, 20);
            ImGui::SameLine();
            if (ImGui::Button((_u8L("Staircase") + "##hae_quantize").c_str())) {
                quantize_to_steps(e, m_quantize_bands);
                commit(_u8L("Height adaptive effects - Staircase"));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Turns this ramp into that many aligned steps.").c_str());
        }

        // Band-length warning (plan §6.2: a band shorter than ~4 layers isn't worth the jump).
        if (stepped && e < m_states.size() && m_states[e].nodes.size() >= 2) {
            const auto& ns = m_states[e].nodes;
            int shortest = INT_MAX;
            for (size_t i = 0; i + 1 < ns.size(); ++ i)
                shortest = std::min(shortest, layers_between(ns[i].z_mm, ns[i + 1].z_mm));
            if (shortest != INT_MAX && shortest < 4) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.15f, 1.f));
                ImGui::TextWrapped("%s", (_u8L("Shortest band is") + " " + std::to_string(shortest) + " "
                                          + _u8L("layers — under about 4 layers the jump costs more than it gives.")).c_str());
                ImGui::PopStyleColor();
            }
        }

        // The curve SCALES fuzzy skin, it does not switch it on — exactly as the Expansion curve
        // inherits being cancelled by MMU painting. Saying it here beats sliced-and-nothing-changed.
        if (def.is_fuzzy) {
            const DynamicPrintConfig& full_cfg = wxGetApp().preset_bundle->full_config();
            const Model* model = m_parent.get_selection().get_model();
            const ModelObject* mo = (model != nullptr && m_object_idx >= 0 && m_object_idx < (int)model->objects.size())
                                    ? model->objects[m_object_idx] : nullptr;
            // The object may override the preset; check its own value first.
            std::string fz;
            if (mo != nullptr && mo->config.has("fuzzy_skin")) {
                if (const auto* o = mo->config.option("fuzzy_skin"))
                    fz = o->serialize();
            } else if (const auto* o = full_cfg.option("fuzzy_skin")) {
                fz = o->serialize();
            }
            if (fz == "none" || fz.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.15f, 1.f));
                ImGui::TextWrapped("%s", _u8L("Fuzzy skin is off for this object, so this curve does nothing. "
                                               "It scales the fuzzy skin thickness; it does not turn the "
                                               "effect on.").c_str());
                ImGui::PopStyleColor();
            } else if (std::string(def.config_key) == "neotko_hae_fuzzy_thickness") {
                // Keyed on the config key, NOT on is_fuzzy: the four noise parameters added in
                // s248 are fuzzy effects too, but only the THICKNESS switches the feature off at
                // zero. Saying it on a persistence curve would simply be false.
                ImGui::TextDisabled("%s", _u8L("Reaching 0 switches fuzzy skin off.").c_str());
                help_marker(_u8L("Where the curve reaches 0 the fuzzy skin is switched off entirely for "
                                 "that layer, not just flattened — otherwise a zero-amplitude fuzzy would "
                                 "still resample every perimeter and bloat the gcode."));
            }
        }

        // NEOTKO_HAE_EXPANSION_TAG — the honest conflict notice of plan §9 #1/#2: the engine
        // drops XY compensation entirely on an MMU-painted or fuzzy-skin-painted object, and
        // the curve inherits that. Better said out loud here than discovered in the gcode.
        if (def.is_expansion) {
            const Model* model = m_parent.get_selection().get_model();
            const ModelObject* mo = (model != nullptr && m_object_idx >= 0 && m_object_idx < (int)model->objects.size())
                                    ? model->objects[m_object_idx] : nullptr;
            if (mo != nullptr) {
                const bool mm_painted = std::any_of(mo->volumes.begin(), mo->volumes.end(),
                                                    [](const ModelVolume* v) { return !v->mmu_segmentation_facets.empty(); });
                if (mm_painted || mo->is_fuzzy_skin_painted()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.3f, 1.f));
                    ImGui::TextWrapped("%s", _u8L("This object is color- or fuzzy-skin-painted. The slicer drops XY "
                                                   "size compensation on painted objects, so this curve will have no "
                                                   "effect.").c_str());
                    ImGui::PopStyleColor();
                }
            }
        }

        // Per-segment tension, Smooth effects only (a step has no tension to give).
        if (!stepped && e < m_states.size() && m_states[e].nodes.size() > 1) {
            ImGui::Separator();
            ImGui::TextUnformatted(_u8L("Tension per segment").c_str());
            help_marker(_u8L("0 = straight segment, 1 = smooth curve."));
            auto& ns = m_states[e].nodes;
            // The Z range goes on the LEFT as plain text, and the slider gets an explicit width.
            // With SliderFloat's own label the range was drawn to the RIGHT of a slider already
            // sized at the default fraction of the window, so the pair always overflowed — and in
            // an AlwaysAutoResize window an overflowing item is what widens the panel.
            //
            // The column is MEASURED over every row, not guessed: a fixed width truncates as soon
            // as the object is tall enough for two-digit millimetres ("0.2 - 18.5 mm"), which is
            // most objects.
            std::vector<std::string> range_lbl(ns.size() - 1);
            float range_w = 0.f;
            for (size_t i = 0; i + 1 < ns.size(); ++ i) {
                range_lbl[i] = format((float)ns[i].z_mm, 1) + " - " + format((float)ns[i + 1].z_mm, 1) + " mm";
                range_w = std::max(range_w, ImGui::CalcTextSize(range_lbl[i].c_str()).x);
            }
            range_w += ImGui::GetStyle().ItemSpacing.x;
            const float slider_w = win_w - 16.f - range_w - 2.f * ImGui::GetStyle().ItemSpacing.x;
            for (size_t i = 0; i + 1 < ns.size(); ++ i) {
                float t = (float)ns[i].tension;
                ImGui::TextDisabled("%s", range_lbl[i].c_str());
                ImGui::SameLine(range_w);
                ImGui::SetNextItemWidth(slider_w);
                const std::string label = "##hae_tension" + std::to_string(i);
                if (ImGui::SliderFloat(label.c_str(), &t, 0.f, 1.f)) {
                    ns[i].tension = std::clamp((double)t, 0., 1.);
                    m_parent.set_as_dirty();
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                    commit(_u8L("Height adaptive effects - Tension"));
            }
        }

        // ---- the numbers, plainly ----
        // A curve you can only set by dragging is a curve you cannot set exactly. Every node is
        // editable here, which doubles as the answer to "how much am I putting in".
        if (e < m_states.size() && !m_states[e].nodes.empty()) {
            ImGui::Separator();
            // Collapsed by default: exact numbers are what you want when you want them, and
            // wall-of-figures the rest of the time. The curve and the hover readout carry the
            // everyday case; this is for typing a value in precisely.
            if (ImGui::Button(((m_show_points ? _u8L("Close Z points") : _u8L("Open Z points"))
                               + "##hae_points_toggle").c_str()))
                m_show_points = !m_show_points;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", (std::to_string(m_states[e].nodes.size()) + " "
                                       + _u8L("points")).c_str());
        }
        if (m_show_points && e < m_states.size() && !m_states[e].nodes.empty()) {
            ImGui::TextUnformatted((_u8L("Z") + std::string(" / ") + def.units).c_str());
            auto& ns = m_states[e].nodes;
            // Nothing that changes the node list runs INSIDE the loop: an erase would invalidate
            // the iteration and a sort would move nodes under the cursor mid-frame. Both are
            // recorded and applied once, after.
            int  erase_at   = -1;
            bool edit_ended = false;
            for (size_t i = 0; i < ns.size(); ++ i) {
                ImGui::PushID(int(i));
                float z = (float)ns[i].z_mm;
                float v = (float)ns[i].value;

                ImGui::SetNextItemWidth(78.f);
                const bool z_edited = ImGui::DragFloat("##hae_z", &z, 0.05f, 0.f, (float)m_object_height, "%.2f");
                // Ask right after ITS OWN widget: IsItemDeactivatedAfterEdit() always refers to
                // the last item submitted, so reading it after the second drag would miss the first.
                edit_ended = ImGui::IsItemDeactivatedAfterEdit() || edit_ended;

                ImGui::SameLine();
                ImGui::SetNextItemWidth(78.f);
                const bool v_edited = ImGui::DragFloat("##hae_v", &v, 0.005f, (float)def.range_lo,
                                                       (float)def.range_hi, "%.3f");
                edit_ended = ImGui::IsItemDeactivatedAfterEdit() || edit_ended;

                if (z_edited || v_edited) {
                    ns[i].z_mm  = std::clamp((double)z, 0., m_object_height);
                    ns[i].value = std::clamp((double)v, def.range_lo, def.range_hi);
                    m_parent.set_as_dirty();
                }

                // Which real layer this Z lands on — the whole point of the gizmo, in text form.
                ImGui::SameLine();
                const int layer_no = layers_between(0., ns[i].z_mm + 1e-9);
                ImGui::TextDisabled("%s", (_u8L("layer") + " " + std::to_string(std::max(1, layer_no))).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    erase_at = int(i);
                ImGui::PopID();
            }
            if (erase_at >= 0) {
                ns.erase(ns.begin() + erase_at);
                commit(_u8L("Height adaptive effects - Delete point"));
            } else if (edit_ended) {
                if (stepped || m_snap_to_layer)
                    for (Node& n : ns)
                        n.z_mm = snap_z(n.z_mm);
                std::stable_sort(ns.begin(), ns.end(),
                                 [](const Node& a, const Node& b) { return a.z_mm < b.z_mm; });
                commit(_u8L("Height adaptive effects - Edit point"));
            }
        }

        ImGui::Separator();
        auto preset_button = [&](const char* id, const std::string& label) {
            if (ImGui::Button((label + "##hae_preset_" + id).c_str())) {
                apply_preset(e, id);
                commit(_u8L("Height adaptive effects - Preset"));
            }
        };
        // Presets are per-EFFECT shapes with absolute values in that effect's units, so they are
        // matched by config key. The level-0 effects of s248 deliberately have none yet: a preset
        // that seeds 0.35 into a noise-scale curve whose axis runs to 10 would be a worse starting
        // point than the empty curve. Better no preset than a wrong one.
        const std::string key = def.config_key;
        const bool has_presets = def.is_expansion
                              || key == "neotko_hae_fuzzy_thickness"
                              || key == "neotko_hae_infill_width";
        if (has_presets) {
            ImGui::TextUnformatted(_u8L("Presets").c_str());
            if (def.is_expansion) {
                preset_button("bullnose", _u8L("Bullnose"));
                ImGui::SameLine(); preset_button("chamfer", _u8L("Chamfer"));
                ImGui::SameLine(); preset_button("barrel", _u8L("Barrel"));
            } else if (key == "neotko_hae_fuzzy_thickness") {
                preset_button("fade_in", _u8L("Fade in"));
                ImGui::SameLine(); preset_button("fade_out", _u8L("Fade out"));
                ImGui::SameLine(); preset_button("fade_band", _u8L("Rough middle"));
            } else {
                preset_button("fine_bottom", _u8L("Fine low, coarse high"));
                ImGui::SameLine(); preset_button("fine_top", _u8L("Coarse low, fine high"));
            }
            ImGui::SameLine();
        }
        if (ImGui::Button((_u8L("Clear") + "##hae_clear").c_str())) {
            m_states[e].nodes.clear();
            commit(_u8L("Height adaptive effects - Clear"));
        }
    }

    ImGui::Separator();
    ImGui::Text("%s", (_u8L("Object height") + ": " + format((float)m_object_height, 2) + " mm   "
                       + std::to_string(m_layer_tops.size()) + " " + _u8L("layers")).c_str());
    if (m_slicing_params.has_raft())
        ImGui::TextWrapped("%s", _u8L("Z is measured from the bottom of the object, not from the bed — "
                                       "the raft is not part of this axis.").c_str());
    if (ImGui::Button(_u8L("Refresh layers").c_str()))
        rebuild_layer_bands();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Re-reads the object's real layers. Needed after changing the adaptive "
                                      "layer height profile.").c_str());
}

}} // namespace Slic3r::GUI
// NEOTKO_HAE_TAG_END
