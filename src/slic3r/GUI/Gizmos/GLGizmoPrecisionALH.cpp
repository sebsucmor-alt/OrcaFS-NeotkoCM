// NEOTKO_PRECISIONALH_TAG_START
#include "GLGizmoPrecisionALH.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/ColorSci/ColorHeightEnvelope.hpp"
#include "libslic3r/ColorSci/SlopePerimeterRecolor.hpp"
#include "libslic3r/SurfaceColorMix.hpp"

#include <imgui/imgui.h>
#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace GUI {

GLGizmoPrecisionALH::GLGizmoPrecisionALH(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoPrecisionALH::on_init()
{
    m_shortcut_key = 0; // no global shortcut
    return true;
}

std::string GLGizmoPrecisionALH::on_get_name() const
{
    return _u8L("Precision Adaptive Layer Height");
}

bool GLGizmoPrecisionALH::on_is_activable() const
{
    // NEOTKO_PRECISIONALH_TAG: LibreMode feature — a single gate, same pattern as
    // GLGizmoAlignStack::on_is_activable() (no NeoDebug channel, no on_is_selectable
    // override — the icon stays visible, only the enabled state toggles). The
    // object-selection check below is an ordinary precondition (the profile is
    // per-ModelObject), not a second gate.
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled")
        && m_parent.get_selection().get_object_idx() >= 0;
}

void GLGizmoPrecisionALH::on_set_state()
{
    if (get_state() == Off) {
        m_have_session    = false;
        m_dragging_point  = -1;
    }
}

void GLGizmoPrecisionALH::ensure_session()
{
    const Selection& sel = m_parent.get_selection();
    const int obj_idx = sel.get_object_idx();
    const Model* model = sel.get_model();
    if (obj_idx < 0 || model == nullptr || obj_idx >= (int)model->objects.size()) {
        m_have_session = false;
        return;
    }
    if (m_have_session && obj_idx == m_object_idx)
        return; // same object, keep in-progress edits across a panel redraw

    m_object_idx = obj_idx;
    const ModelObject* mo = model->objects[obj_idx];

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    m_slicing_params = PrintObject::slicing_parameters(full_config, *mo, (float)mo->max_z(), Vec3d::Ones());

    // Use the exact values the engine uses, NOT mo->max_z()/layer_height:
    //  - object_print_z_uncompensated_height() is what the profile's last Z is
    //    checked against (PrintObject::update_layer_height_profile), and what
    //    layer_height_profile_from_ranges emits as the top.
    //  - first_object_layer_height is the fixed first layer; the profile MUST
    //    start at [0, first_object_layer_height] or the slicer discards the
    //    whole profile (that check is why edits never reached the slice).
    m_object_height      = m_slicing_params.object_print_z_uncompensated_height();
    m_first_layer_height = m_slicing_params.first_object_layer_height;
    if (m_first_layer_height <= 0.0)
        m_first_layer_height = m_slicing_params.layer_height > 0.0 ? m_slicing_params.layer_height : 0.2;

    load_points_from_profile(mo->layer_height_profile.get());

    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope, Frente 1. Cached once per object switch —
    // the mesh can't change while this gizmo is the active tool, stricter than plan §6 R3's
    // "no slicing per frame" bar. band_depth_mm reflects the plan §5bis point 4 nuance: a pure
    // mixed_filament_sandwich_mode object with no manual paint and penultimate_top_layers==0
    // never gets a real penu band (object_painter_wants_penu() only looks at manual paint).
    {
        const bool has_penu_override = mo->config.has("penultimate_top_layers");
        const int  penu_layers = has_penu_override ? mo->config.opt_int("penultimate_top_layers")
                                                     : full_config.opt_int("penultimate_top_layers");
        const bool wants_penu = Slic3r::SurfaceColorMix::object_painter_wants_penu(mo) || penu_layers > 0;

        const bool has_tst_override = mo->config.has("top_shell_thickness");
        const double top_shell_thickness = has_tst_override ? mo->config.opt_float("top_shell_thickness")
                                                              : full_config.opt_float("top_shell_thickness");
        const double nominal_layer_height = m_slicing_params.layer_height > 0.0 ? m_slicing_params.layer_height : 0.2;
        const double band_depth_mm = wants_penu ? std::max(top_shell_thickness, nominal_layer_height)
                                                 : nominal_layer_height;

        // NEOTKO_ALHCOLOR_TAG — Fase 5.1 (Frente 2). Pre-slice perimeter width + wall count,
        // config-only — the exact route the plan cites: PrintRegion::flow()'s own inner
        // logic is Flow::new_from_config_width(config_width, nozzle_diameter, layer_height),
        // which needs no sliced LayerRegion. Same object-override pattern as the penu block
        // above. Width option falls back inner_wall_line_width -> line_width -> Flow's own
        // auto default, mirroring PrintRegion::flow(frPerimeter).
        {
            auto opt_fop = [&](const char* key) -> ConfigOptionFloatOrPercent {
                // ModelConfig::option() is untyped (no template overload) — same
                // dynamic_cast pattern as the mixed_filament_sandwich_mode read above.
                if (auto* o = dynamic_cast<const ConfigOptionFloatOrPercent*>(mo->config.option(key)))
                    return *o;
                if (auto* o = full_config.option<ConfigOptionFloatOrPercent>(key))
                    return *o;
                return ConfigOptionFloatOrPercent(0., false);
            };
            ConfigOptionFloatOrPercent width = opt_fop("inner_wall_line_width");
            if (!width.percent && width.value <= 0.)
                width = opt_fop("line_width");
            const auto* nd = full_config.option<ConfigOptionFloats>("nozzle_diameter");
            const float nozzle = nd != nullptr && !nd->values.empty() ? float(nd->values.front()) : 0.4f;
            m_perimeter_width_mm = Flow::new_from_config_width(frPerimeter, width, nozzle,
                                                               float(nominal_layer_height)).width();

            m_wall_loops = mo->config.has("wall_loops") ? mo->config.opt_int("wall_loops")
                                                        : full_config.opt_int("wall_loops");
            m_wall_loops = std::max(m_wall_loops, 1);

            // NEOTKO_ALHCOLOR_TAG — Fase 5.2bis. Live-read of the pattern ceiling — see
            // the member's comment. Guarded: falls back to the historical 0.16 default if
            // the key is somehow absent.
            m_mix_band_upper_mm = full_config.has("mixed_filament_height_upper_bound")
                ? std::max(0.0, full_config.opt_float("mixed_filament_height_upper_bound"))
                : 0.16;
        }

        // One facet pass, two outputs (Frente 1 top bands + Fase 5.1 slope bands). Slopes
        // shallower than min_tan_alpha can't expose an interior ring even at the max layer
        // height (d = h*tan stays under one perimeter width), so the scan skips them.
        const double max_h = m_slicing_params.max_layer_height > 0.0 ? m_slicing_params.max_layer_height : 0.3;
        const double min_tan_alpha = m_perimeter_width_mm / max_h;
        const auto scan = Slic3r::ColorSci::compute_object_zone_scan(*mo, band_depth_mm, min_tan_alpha);
        m_top_zone_bands = scan.top_bands;
        m_slope_bands    = scan.slope_bands;
    }

    m_have_session   = true;
    m_dragging_point = -1;
    m_hover_point    = -1;
    m_band_key       = -1.0;
}

void GLGizmoPrecisionALH::seed_flat_profile()
{
    double h = m_slicing_params.layer_height;
    if (h <= 0.0)
        h = m_slicing_params.max_layer_height > 0.0 ? m_slicing_params.max_layer_height : 0.2;

    // Bottom point is pinned to the fixed first layer height (see header); the
    // top point defaults to the regular layer height. When the two are equal
    // (the usual case) this reads as a flat line.
    m_points.clear();
    m_points.push_back(ALHPoint{ 0.0, m_first_layer_height, 0.0 });
    m_points.push_back(ALHPoint{ m_object_height, h, 0.0 });
}

void GLGizmoPrecisionALH::load_points_from_profile(const std::vector<coordf_t>& profile)
{
    // Cap: a profile denser than this is almost certainly from the stock brush
    // (per-pixel samples), not a reasonable set of draggable control points —
    // fall back to a flat seed rather than flooding the band with circles.
    constexpr size_t kMaxPointsToImport = 80;

    if (profile.size() >= 4 && profile.size() <= kMaxPointsToImport * 2 && profile.size() % 2 == 0) {
        std::vector<ALHPoint> pts;
        pts.reserve(profile.size() / 2);
        for (size_t i = 0; i + 1 < profile.size(); i += 2)
            pts.push_back(ALHPoint{ profile[i], profile[i + 1], 0.0 });
        // Sanity check: must actually span from the base to the object height,
        // otherwise generate_object_layers()'s hard constraint (z=0 .. object
        // height) would be violated on the next commit.
        if (pts.size() >= 2 && pts.front().z_mm <= 1e-6 && pts.back().z_mm >= m_object_height - 1e-3) {
            // Pin the endpoints to the exact canonical values so a round-trip
            // (load → commit) never drifts and never fails the slicer's checks.
            pts.front().z_mm     = 0.0;
            pts.front().height_mm = m_first_layer_height;
            pts.back().z_mm      = m_object_height;
            m_points = std::move(pts);
            return;
        }
    }
    seed_flat_profile();
}

std::vector<double> GLGizmoPrecisionALH::compute_tangents() const
{
    const size_t n = m_points.size();
    std::vector<double> m(n, 0.0);
    if (n < 2)
        return m;

    std::vector<double> d(n - 1, 0.0);
    for (size_t i = 0; i + 1 < n; ++i) {
        const double dz = m_points[i + 1].z_mm - m_points[i].z_mm;
        d[i] = (dz > 1e-9) ? (m_points[i + 1].height_mm - m_points[i].height_mm) / dz : 0.0;
    }
    m[0]     = d.front();
    m[n - 1] = d.back();
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = 0.5 * (d[i - 1] + d[i]);

    // Fritsch-Carlson correction: guarantees the cubic never overshoots past
    // the two endpoint heights of a segment.
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0.0) { m[i] = 0.0; m[i + 1] = 0.0; continue; }
        double alpha = m[i] / d[i];
        double beta  = m[i + 1] / d[i];
        if (alpha < 0.0) { m[i]     = 0.0; alpha = 0.0; }
        if (beta  < 0.0) { m[i + 1] = 0.0; beta  = 0.0; }
        const double a2b2 = alpha * alpha + beta * beta;
        if (a2b2 > 9.0) {
            const double tau = 3.0 / std::sqrt(a2b2);
            m[i]     = tau * alpha * d[i];
            m[i + 1] = tau * beta  * d[i];
        }
    }
    return m;
}

double GLGizmoPrecisionALH::blended_height(size_t seg_idx, double t, const std::vector<double>& tangents,
                                            const Slic3r::ColorSci::ColorHeightEnvelope* color_env) const
{
    const ALHPoint& p0 = m_points[seg_idx];
    const ALHPoint& p1 = m_points[seg_idx + 1];
    const double linear = p0.height_mm + t * (p1.height_mm - p0.height_mm);

    const double tension = std::clamp(p0.tension, 0.0, 1.0);

    // NEOTKO_ALHCOLOR_TAG — Fase 2 (plan §4.b). Narrows lo/hi to the active
    // color envelope; nullptr/passthrough leaves lo/hi at the plain nozzle
    // bounds, i.e. byte-identical behavior to before this phase.
    const bool   use_color = color_env != nullptr && !color_env->passthrough;
    const double lo = use_color ? std::max(m_slicing_params.min_layer_height, color_env->h_min) : m_slicing_params.min_layer_height;
    const double hi = use_color ? std::min(m_slicing_params.max_layer_height, color_env->h_max) : m_slicing_params.max_layer_height;

    if (tension <= 1e-6)
        return use_color ? std::clamp(linear, lo, hi) : linear;

    const double dz = p1.z_mm - p0.z_mm;
    const double t2 = t * t, t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    const double cubic = h00 * p0.height_mm + h10 * dz * tangents[seg_idx]
                        + h01 * p1.height_mm + h11 * dz * tangents[seg_idx + 1];

    const double blended = (1.0 - tension) * linear + tension * cubic;
    // Belt-and-braces clamp: the blend of two monotone functions sharing the
    // same endpoints should already stay within range, this just guards fp
    // noise (lo/hi narrow to the color envelope above when active).
    return std::clamp(blended, lo, hi);
}

std::vector<coordf_t> GLGizmoPrecisionALH::build_profile_vector() const
{
    std::vector<coordf_t> out;
    if (m_points.size() < 2)
        return out;

    // NEOTKO_ALHCOLOR_TAG — Fase 2 + replanteo TD-vs-slope (plan §4.d, the
    // actual "pre-proceso" that sanitizes the profile before it reaches
    // generate_object_layers()). Recomputed fresh PER EMITTED Z (not once
    // globally, and not threaded in from the caller) — the envelope is now
    // per-zone (m_top_zone_bands), so a straight segment crossing a Sandwich-
    // zone boundary must clamp differently on each side, and re-resolving live
    // color state per call still catches TD/paint changes since the points
    // were set. Cheap per plan §6 R3 — no slicing, just app_config reads +
    // facet-emptiness checks + an O(bands) interval lookup.
    auto clamp_for_z = [this](double z_mm, double h) -> double {
        Slic3r::ColorSci::ColorHeightEnvelope color_env;
        const bool use_color = m_adapt_to_color && compute_active_color_envelope(color_env, z_mm) && !color_env.passthrough;
        const double lo = use_color ? std::max(m_slicing_params.min_layer_height, color_env.h_min) : m_slicing_params.min_layer_height;
        const double hi = use_color ? std::min(m_slicing_params.max_layer_height, color_env.h_max) : m_slicing_params.max_layer_height;
        return std::clamp(h, lo, hi);
    };

    const std::vector<double> tangents = compute_tangents();
    // First pair MUST be exactly [0, first_object_layer_height] or the slicer
    // discards the whole profile (PrintObject::update_layer_height_profile).
    // Exempt from the color envelope on purpose — the fixed first layer is
    // never renegotiated for color (plan §6 R4).
    out.push_back(0.0);
    out.push_back(m_first_layer_height);

    constexpr int kSamples = 16;
    for (size_t i = 0; i + 1 < m_points.size(); ++i) {
        const ALHPoint& p0 = m_points[i];
        const ALHPoint& p1 = m_points[i + 1];
        if (p0.tension <= 1e-6) {
            // Straight segment: the motor already lerps linearly between
            // consecutive pairs, no need to densify (generate_object_layers(),
            // Slicing.cpp).
            out.push_back(p1.z_mm);
            out.push_back(m_adapt_to_color ? clamp_for_z(p1.z_mm, p1.height_mm) : p1.height_mm);
            continue;
        }
        for (int s = 1; s <= kSamples; ++s) {
            const double t = double(s) / double(kSamples);
            const double z = p0.z_mm + t * (p1.z_mm - p0.z_mm);
            Slic3r::ColorSci::ColorHeightEnvelope color_env;
            const bool have_env = m_adapt_to_color && compute_active_color_envelope(color_env, z);
            out.push_back(z);
            out.push_back(blended_height(i, t, tangents, have_env ? &color_env : nullptr));
        }
    }
    // Last Z MUST equal object_print_z_uncompensated_height() exactly, else the
    // slicer's validity check (PrintObject::update_layer_height_profile) fails
    // and the profile is discarded.
    if (out.size() >= 2)
        out[out.size() - 2] = m_object_height;
    return out;
}

void GLGizmoPrecisionALH::commit(const std::string& snapshot_name)
{
    if (!m_have_session || m_object_idx < 0)
        return;
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx >= (int)model->objects.size())
        return;

    wxGetApp().plater()->take_snapshot(snapshot_name);
    ModelObject* mo = const_cast<ModelObject*>(model->objects[m_object_idx]);

    // NEOTKO_ALHCOLOR_TAG — Fase 5.3. Blob written/erased INSIDE the same snapshot as the
    // profile (plan §5.3: atomic from the user's undo perspective). Opt-in: toggle off
    // actively erases so stale plans never linger on the object.
    if (m_slope_recolor_enabled) {
        const std::string blob = build_slope_recolor_blob();
        if (blob.empty())
            mo->config.erase("neotko_slope_perimeter_recolor");
        else
            mo->config.set_key_value("neotko_slope_perimeter_recolor", new ConfigOptionString(blob));
    } else {
        mo->config.erase("neotko_slope_perimeter_recolor");
    }

    mo->layer_height_profile.set(build_profile_vector());
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(m_object_idx);
}

void GLGizmoPrecisionALH::build_band_model(int idx)
{
    m_band_model.reset();

    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[m_object_idx];

    BoundingBoxf3 bb;
    for (size_t i = 0; i < mo->instances.size(); ++i)
        bb.merge(mo->instance_bounding_box(i));
    if (!bb.defined)
        return;

    // A thin slab (box) at the control point's world Z, spanning the object's
    // XY footprint plus a small margin so it reads clearly around the object.
    const double zc  = bb.min.z() + m_points[idx].z_mm;
    const double pad = 1.5;                                   // mm, XY margin
    const double ht  = std::max(0.15, 0.5 * m_slicing_params.layer_height); // mm, half slab thickness
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
        {0,1,2},{0,2,3}, {4,6,5},{4,7,6},              // bottom, top
        {0,4,5},{0,5,1}, {1,5,6},{1,6,2},              // sides
        {2,6,7},{2,7,3}, {3,7,4},{3,4,0},
    };
    its.indices.reserve(12);
    for (const auto& t : box_tris)
        its.indices.emplace_back(t[0], t[1], t[2]);
    m_band_model.init_from(its);
}

void GLGizmoPrecisionALH::on_render()
{
    // Highlight, on the object itself, the Z-band of whichever control point is
    // being dragged (or hovered in the 2D panel) — same intent as the stock
    // "Layers editing" overlay: show which slice of the object you're affecting.
    // Rendered in WORLD space with the flat shader (like
    // GLGizmoAlignStack::render_face_highlights) — the previous screen-space
    // ImGui projection was unstable under perspective/retina and smeared across
    // the whole viewport.
    const int idx = (m_dragging_point >= 0) ? m_dragging_point : m_hover_point;
    if (!m_have_session || idx < 0 || idx >= (int)m_points.size())
        return;

    // Rebuild only when the affected point/Z changes.
    const double key = idx * 1e6 + m_points[idx].z_mm;
    if (key != m_band_key) {
        build_band_model(idx);
        m_band_key = key;
    }
    if (!m_band_model.is_initialized())
        return;

    const bool dragging = (m_dragging_point >= 0);
    m_band_model.set_color(dragging ? ColorRGBA(1.00f, 0.75f, 0.16f, 0.42f)   // amber while dragging
                                    : ColorRGBA(0.00f, 0.59f, 0.53f, 0.30f)); // teal on hover

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix()); // vertices are world-space
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    m_band_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope (Frente 1, s220). Z-INDEPENDENT
// half of the envelope resolution: has_color gate + worst-case materials
// across all 4 configured filament slots — same spirit/cost as
// gizmo_materials() in GLGizmoColorMixPainter.cpp. Split out of
// compute_active_color_envelope() so render_curve_editor()'s per-sample
// shading loop can call this ONCE per frame and reuse the result across
// every sample, instead of re-reading app_config/materials up to
// kDrawSamples*segment-count times per frame. Re-resolves the ModelObject
// fresh, no cached pointer, matching every other method in this class.
bool GLGizmoPrecisionALH::resolve_color_context(Slic3r::ColorSci::ColorHeightContext& ctx) const
{
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= (int)model->objects.size())
        return false;
    const ModelObject* mo = model->objects[m_object_idx];

    // Same guarded nozzle bounds render_curve_editor() uses for its band —
    // kept in sync so the shaded zones can never draw outside the visible
    // band (see that function's own h_min/h_max locals).
    const double nozzle_lo = m_slicing_params.min_layer_height > 0.0 ? m_slicing_params.min_layer_height : 0.01;
    const double nozzle_hi = std::max(m_slicing_params.max_layer_height, nozzle_lo + 1e-6);

    ctx.nozzle_min_height_mm = nozzle_lo;
    ctx.nozzle_max_height_mm = nozzle_hi;
    ctx.td_reference_height_mm = m_slicing_params.layer_height > 0.0 ? m_slicing_params.layer_height : 0.2;
    // ctx.sandwich_passes left at its default (0) — top-surface reservation
    // is Fase 3bis, not this phase.

    // Gate on the engine's most-complete "painter mode" check — Fill.cpp's
    // _mp_painter_mode (paint OR sticker OR Mixed Filament Object mode,
    // Fill.cpp:1663-1673) is the only one of the engine's several copies of
    // this check that includes all three sources; SurfaceColorMix.cpp
    // (assign_and_group_tools) and ToolOrdering.cpp are missing the
    // mixed_filament_sandwich_mode term despite a comment claiming parity —
    // a pre-existing engine inconsistency, not something to replicate here.
    // NOT ModelObject::is_mm_painted() — that only reflects the legacy
    // mmu_segmentation_facets store and is never populated by the ColorMix
    // painter, so it would never trigger for this feature's actual target.
    const auto* mf_opt = dynamic_cast<const ConfigOptionBool*>(mo->config.option("mixed_filament_sandwich_mode"));
    const bool manual_color = Slic3r::SurfaceColorMix::object_has_any_colormix_paint(mo)
                             || Slic3r::SurfaceColorMix::object_has_any_colormix_stickers(mo);
    // NEOTKO_ALHCOLOR_TAG — 5.4b (s222, user-reported: Cycle objects showed no envelope, no
    // slope preview, and never wrote a recolor blob). A MixedFilament PATTERN — the object's
    // extruder pointing at a mixed row (Cycle/gradient/manual) — IS a color signal: the
    // pattern-resolution ceiling applies to it by definition. The original three-source gate
    // (paint/stickers/sandwich toggle) only detected SANDWICH-related color, leaving pure
    // pattern objects invisible to the whole feature.
    const bool sandwich_signal = manual_color || (mf_opt != nullptr && mf_opt->value);
    const unsigned int pattern_id = object_mixed_filament_id();
    const bool has_color = sandwich_signal || pattern_id != 0;

    if (has_color) {
        // NEOTKO_ALHCOLOR_TAG — s222, user-reported ("snap keeps picking max layer height
        // — extraño"): the worst-case-across-4-slots material set let an unused slot's high
        // TD (e.g. td_3=3.76) drive the fidelity floor into permanent conflict (0.32-0.32)
        // for objects that never print that slot. When the object's only color signal is a
        // MixedFilament pattern, its palette is KNOWN — the recipe's own components (same
        // resolver Fase 5.2 uses) — so scope the envelope to exactly those tools. Manual
        // paint/stickers still fall back to worst-case-4 (the "which tools are painted"
        // resolver for the colormix facet store doesn't exist — documented Nivel 0 gap).
        std::vector<unsigned int> comp;
        if (!manual_color)
            comp = pattern_component_tools(pattern_id);
        if (!comp.empty()) {
            ctx.painted_tools.assign(comp.begin(), comp.end());
        } else {
            // Same pattern as GLGizmoColorMixPainter::gizmo_materials(): worst
            // case across all 4 configured filament slots, not filtered to which
            // tool is actually painted where (Nivel 0 scope, plan §3).
            ctx.painted_tools = { 0, 1, 2, 3 };
        }
        std::vector<std::string> fcolors;
        if (auto* o = wxGetApp().preset_bundle->project_config
                          .option<ConfigOptionStrings>("filament_colour"))
            fcolors = o->values;
        while (fcolors.size() < 4) fcolors.push_back("#808080");

        auto* ac = wxGetApp().app_config;
        for (int t = 0; t < 4; ++t) {
            float td = 1.f;
            if (ac) {
                const std::string v = ac->get("neotko_td_" + std::to_string(t + 1));
                try { if (!v.empty()) td = std::stof(v); } catch (...) {}
            }
            td = std::max(0.01f, std::min(10.f, td));
            ctx.mats[t] = Slic3r::ColorSci::material_from_hex(fcolors[t], td);
        }

        // NEOTKO_ALHCOLOR_TAG — 5.4b. Pure pattern object (no paint/stickers/sandwich
        // toggle): there is NO Sandwich recipe anywhere on it, and TD physics is only real
        // inside that recipe (user rule, s220) — zero every TD so the fidelity floor stays
        // 0 at every Z, leaving only the pattern-resolution ceiling. Without this, a top
        // band from the mesh proxy would wrongly apply TD to a plain Cycle object.
        if (!sandwich_signal)
            for (int t = 0; t < 4; ++t)
                for (int c = 0; c < 3; ++c)
                    ctx.mats[t].td[c] = 0.f;
    }
    // has_color == false: ctx.painted_tools stays empty → compute_color_height_envelope()
    // returns passthrough (its own documented no-color behavior).
    return true;
}

// NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope, Frente 1. The genuinely
// per-Z, O(1) half: takes a COPY of an already-resolved ctx (so repeated
// calls across samples never mutate each other's td[]) and applies the
// Sandwich-zone gate before computing the envelope. TD/opacity physics
// (fidelity_floor, driven entirely by mats[].td[]) is only ever real inside
// a Sandwich zone (build_mixed_filament_recipe stacking passes on a real top
// surface) — never for MixedFilament's normal pattern coloring. Outside
// m_top_zone_bands, zero td[] so min_thickness_for_opacity_mm() returns 0
// (its own documented "TD≈0 → opaque → any thickness works" behavior) and
// fidelity_floor collapses to 0 there. Deliberately NOT clearing
// ctx.painted_tools: that would flip passthrough=true and lose h_max's
// pattern-resolution ceiling, which IS real object-wide physics outside
// Sandwich too (plan §2) — only fidelity_floor is Sandwich-only.
Slic3r::ColorSci::ColorHeightEnvelope GLGizmoPrecisionALH::color_envelope_for_z(Slic3r::ColorSci::ColorHeightContext ctx, double z_mm) const
{
    if (!ctx.painted_tools.empty()) {
        const bool in_sandwich_zone = std::any_of(m_top_zone_bands.begin(), m_top_zone_bands.end(),
            [z_mm](const Slic3r::ColorSci::TopZoneBand& b) { return z_mm >= b.z_lo_mm && z_mm <= b.z_hi_mm; });
        if (!in_sandwich_zone) {
            for (int t = 0; t < 4; ++t)
                for (int c = 0; c < 3; ++c)
                    ctx.mats[t].td[c] = 0.f;
        }
    }
    // NEOTKO_ALHCOLOR_TAG — Fase 5.2bis: the ceiling now tracks the config's
    // mixed_filament_height_upper_bound (read per session) instead of Fase 0's
    // hardcoded 0.16 default — they were always the same constant, just unlinked.
    return Slic3r::ColorSci::compute_color_height_envelope(
        ctx, Slic3r::ColorSci::kDefaultTargetOpacity, m_mix_band_upper_mm);
}

bool GLGizmoPrecisionALH::compute_active_color_envelope(Slic3r::ColorSci::ColorHeightEnvelope& out, double z_mm) const
{
    Slic3r::ColorSci::ColorHeightContext ctx;
    if (!resolve_color_context(ctx))
        return false;
    out = color_envelope_for_z(ctx, z_mm);
    return true;
}

// NEOTKO_ALHCOLOR_TAG — Fase 5.1 (Frente 2). O(m_slope_bands) interval lookup, same cost
// class as the Sandwich-zone gate in color_envelope_for_z() — safe per sample per frame.
double GLGizmoPrecisionALH::slope_tan_at(double z_mm) const
{
    double tan_alpha = 0.0;
    for (const Slic3r::ColorSci::SlopeZoneBand& b : m_slope_bands)
        if (z_mm >= b.z_lo_mm && z_mm <= b.z_hi_mm)
            tan_alpha = std::max(tan_alpha, b.tan_alpha_max);
    return tan_alpha;
}

// NEOTKO_ALHCOLOR_TAG — s222. See the .hpp comments. Factored out of the Fase 5.2 preview
// so resolve_color_context() can reuse the same recipe-palette resolution (user decision
// s222: an object whose only color signal is a MixedFilament pattern must not have unused
// slots' TD rule its envelope — see the comment at the use site).
unsigned int GLGizmoPrecisionALH::object_mixed_filament_id() const
{
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr || m_object_idx < 0 || m_object_idx >= (int)model->objects.size())
        return 0;
    const ModelObject* mo = model->objects[m_object_idx];
    const PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr)
        return 0;
    const size_t num_phys = size_t(std::max(wxGetApp().filaments_cnt(), 0));
    if (num_phys == 0)
        return 0;

    unsigned int mixed_id = 0;
    auto consider = [&](int id) {
        if (mixed_id == 0 && id >= 1 && pb->mixed_filaments.is_mixed((unsigned int)id, num_phys))
            mixed_id = (unsigned int)id;
    };
    consider(mo->config.has("extruder") ? mo->config.opt_int("extruder") : 0);
    for (const ModelVolume* mv : mo->volumes)
        if (mv->is_model_part() && mv->config.has("extruder"))
            consider(mv->config.opt_int("extruder"));
    return mixed_id;
}

// NEOTKO_ALHCOLOR_TAG — s222 fix. See the .hpp comment: target = the recipe's MIX color.
bool GLGizmoPrecisionALH::recipe_target_rgb(unsigned int mixed_id,
                                            const Slic3r::ColorSci::Material mats[4],
                                            float out_rgb[3]) const
{
    const PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr || mixed_id == 0)
        return false;
    const size_t num_phys = size_t(std::max(wxGetApp().filaments_cnt(), 0));
    const MixedFilament* mf = pb->mixed_filaments.mixed_filament_from_id(mixed_id, num_phys);
    if (mf == nullptr)
        return false;

    std::vector<std::pair<unsigned int, float>> parts; // (0-based tool, weight)
    auto add_part = [&](unsigned int phys_1based, float w) {
        if (phys_1based >= 1 && phys_1based <= std::min<size_t>(num_phys, 4) && w > 0.f)
            parts.emplace_back(phys_1based - 1, w);
    };
    const std::vector<unsigned int> gradient_ids =
        MixedFilamentManager::decode_gradient_component_ids(mf->gradient_component_ids, num_phys);
    if (gradient_ids.size() >= 3) {
        for (unsigned int id : gradient_ids)
            add_part(id, 1.f); // multi-component gradient: equal weights (v1 approximation)
    } else {
        add_part(mf->component_a, float(std::max(1, mf->ratio_a)));
        add_part(mf->component_b, float(std::max(1, mf->ratio_b)));
    }
    if (parts.empty())
        return false;

    float acc[3] = { 0.f, 0.f, 0.f };
    float wsum = 0.f;
    for (const auto& [tool, w] : parts) {
        for (int c = 0; c < 3; ++c)
            acc[c] += mats[tool].rgb[c] * w;
        wsum += w;
    }
    for (int c = 0; c < 3; ++c)
        out_rgb[c] = acc[c] / wsum;
    return true;
}

std::vector<unsigned int> GLGizmoPrecisionALH::pattern_component_tools(unsigned int mixed_id) const
{
    std::vector<unsigned int> tools; // 0-based
    if (mixed_id == 0)
        return tools;
    const PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr)
        return tools;
    const size_t num_phys = size_t(std::max(wxGetApp().filaments_cnt(), 0));
    auto add = [&](unsigned int phys_1based) {
        if (phys_1based >= 1 && phys_1based <= std::min<size_t>(num_phys, 4)
            && std::find(tools.begin(), tools.end(), phys_1based - 1) == tools.end())
            tools.push_back(phys_1based - 1);
    };
    if (const MixedFilament* mf = pb->mixed_filaments.mixed_filament_from_id(mixed_id, num_phys)) {
        add(mf->component_a);
        add(mf->component_b);
        for (unsigned int id : MixedFilamentManager::decode_gradient_component_ids(mf->gradient_component_ids, num_phys))
            add(id);
    }
    return tools;
}

// NEOTKO_ALHCOLOR_TAG — Fase 5.3 (Frente 2). See the .hpp comment for the format contract.
std::string GLGizmoPrecisionALH::build_slope_recolor_blob() const
{
    const unsigned int mixed_id = object_mixed_filament_id();
    if (mixed_id == 0)
        return {};
    const std::vector<unsigned int> candidates = pattern_component_tools(mixed_id);
    if (candidates.empty())
        return {};
    Slic3r::ColorSci::ColorHeightContext ctx;
    if (!resolve_color_context(ctx) || ctx.painted_tools.empty())
        return {};

    // Target = the recipe's intended MIX color, once for the whole object (s222 fix — the
    // old per-band resolve_perimeter target picked whichever solid tool the band's mid
    // layer cycled to, collapsing the band to that solid; see recipe_target_rgb's comment).
    float target_rgb[3];
    if (!recipe_target_rgb(mixed_id, ctx.mats, target_rgb))
        return {};
    const Slic3r::ColorSci::Lab target = Slic3r::ColorSci::rgb_to_lab(target_rgb);

    // Heights come from the EMITTED profile (already envelope-sanitized), interpolated
    // linearly between its (z, h) pairs — the same data generate_object_layers consumes,
    // so the stored plan matches what will actually slice.
    const std::vector<coordf_t> profile = build_profile_vector();
    auto height_at = [&profile](double z) -> double {
        if (profile.size() < 4)
            return profile.size() >= 2 ? profile[1] : 0.2;
        for (size_t i = 0; i + 3 < profile.size(); i += 2) {
            const double z0 = profile[i], h0 = profile[i + 1];
            const double z1 = profile[i + 2], h1 = profile[i + 3];
            if (z <= z1 || i + 4 >= profile.size()) {
                if (z1 <= z0 + 1e-9)
                    return h1;
                const double t = std::clamp((z - z0) / (z1 - z0), 0.0, 1.0);
                return h0 + t * (h1 - h0);
            }
        }
        return profile.back();
    };

    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(4);
    ss << "[";
    bool first = true;
    for (const Slic3r::ColorSci::SlopeZoneBand& b : m_slope_bands) {
        const double z_mid = 0.5 * (b.z_lo_mm + b.z_hi_mm);
        const double h     = height_at(z_mid);
        const double d_mm  = h * b.tan_alpha_max;
        if (d_mm <= m_perimeter_width_mm)
            continue; // clean at the committed height — nothing to recolor in this band

        const Slic3r::ColorSci::PerimeterColorPlan plan = Slic3r::ColorSci::resolve_perimeter_colors(
            d_mm, m_perimeter_width_mm, m_wall_loops, target, candidates, ctx.mats);
        if (plan.tool_per_perimeter.empty())
            continue;

        if (!first)
            ss << ",";
        first = false;
        ss << "{\"z_lo\":" << b.z_lo_mm << ",\"z_hi\":" << b.z_hi_mm << ",\"tools\":[";
        for (size_t k = 0; k < plan.tool_per_perimeter.size(); ++k) {
            if (k > 0)
                ss << ",";
            ss << plan.tool_per_perimeter[k];
        }
        ss << "]}";
    }
    ss << "]";
    return first ? std::string() : ss.str();
}

// NEOTKO_ALHCOLOR_TAG — Fase 5.1+5.2 (Frente 2). See the .hpp comment for the contract.
// Runs only for the single focused point (hover/drag) per frame — the exhaustive search in
// resolve_perimeter_colors() is bounded by candidates^rings <= 4^(wall_loops+1), trivial.
void GLGizmoPrecisionALH::render_slope_recolor_preview(double z_mm, double point_height_mm)
{
    const double tan_alpha = slope_tan_at(z_mm);
    if (tan_alpha <= 0.0)
        return;
    const int n_exposed = Slic3r::ColorSci::compute_exposed_perimeter_count(
        point_height_mm, tan_alpha, 1.0, m_perimeter_width_mm, m_wall_loops);
    if (n_exposed <= 0)
        return; // clean at this height — the violet shading already shows where that changes

    // Fase 5.1 — the numeric readout of the violet shading. TextWrapped (not TextColored)
    // so the notice folds inside the fixed window width instead of clipping.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.5f, 0.95f, 1.0f));
    ImGui::TextWrapped("%s",
        (_u8L("Slope here exposes") + " " + std::to_string(n_exposed) + " "
         + _u8L("inner perimeter(s) at this height — pattern will show them.")).c_str());
    ImGui::PopStyleColor();

    // NEOTKO_ALHCOLOR_TAG — s222, "solución barata" agreed with the user: where a Sandwich
    // top band and a slope band overlap, the two regimes want opposite heights (thick for
    // the in-layer recipe, thin for the staircase) and TODAY thick-top wins by
    // construction. Distributing the recipe across penu+top layers ("Sandwich 2.0", auto)
    // is a future engine subsystem — this notice maps where it would matter on real
    // objects, at zero engine cost.
    const bool in_top_band = std::any_of(m_top_zone_bands.begin(), m_top_zone_bands.end(),
        [z_mm](const Slic3r::ColorSci::TopZoneBand& b) { return z_mm >= b.z_lo_mm && z_mm <= b.z_hi_mm; });
    if (in_top_band) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.15f, 1.0f));
        ImGui::TextWrapped("%s",
            _u8L("Sandwich + slope zone: fine layers here would need the recipe spread "
                 "across top layers (not available yet) — thick top wins for now.").c_str());
        ImGui::PopStyleColor();
    }

    // ---- Fase 5.2 — read-only recolor suggestion --------------------------------------
    // (recipe access — mixed_filament_from_id, components, ratios — happens inside
    // object_mixed_filament_id()/pattern_component_tools()/recipe_target_rgb(), all against
    // the always-alive GUI-side PresetBundle::mixed_filaments instance the plan verified.)
    const unsigned int mixed_id = object_mixed_filament_id();
    if (mixed_id == 0)
        return; // no MixedFilament pattern on this object — nothing to recolor for

    Slic3r::ColorSci::ColorHeightContext ctx;
    if (!resolve_color_context(ctx) || ctx.painted_tools.empty())
        return; // mats[] come from the same resolution Frente 1 already does

    // Target color = the recipe's intended MIX (s222 fix — see recipe_target_rgb's .hpp
    // comment for why the old per-layer solid-tool target collapsed the band to a solid).
    float target_rgb[3];
    if (!recipe_target_rgb(mixed_id, ctx.mats, target_rgb))
        return;
    const Slic3r::ColorSci::Lab target = Slic3r::ColorSci::rgb_to_lab(target_rgb);

    // Candidates = the recipe's physical palette (§7bis.b: "del palette de la receta").
    const std::vector<unsigned int> candidates = pattern_component_tools(mixed_id);
    if (candidates.empty())
        return;

    const double d_mm = point_height_mm * tan_alpha;
    const Slic3r::ColorSci::PerimeterColorPlan plan = Slic3r::ColorSci::resolve_perimeter_colors(
        d_mm, m_perimeter_width_mm, m_wall_loops, target, candidates, ctx.mats);
    if (plan.tool_per_perimeter.empty())
        return;

    // One swatch per recolored INTERIOR ring (the external ring is never overridden — it
    // keeps the pattern's own per-layer alternation, s222 fix), plus the achieved dE2000.
    ImGui::Text("%s", (_u8L("Inner ring recolor (outer keeps pattern)") + ":").c_str());
    ImGui::SameLine();
    for (size_t k = 1; k < plan.tool_per_perimeter.size(); ++k) {
        const auto& rgb = ctx.mats[std::min<unsigned int>(plan.tool_per_perimeter[k], 3)].rgb;
        ImGui::ColorButton(("##alh_ring" + std::to_string(k)).c_str(),
                           ImVec4(rgb[0], rgb[1], rgb[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(16.0f, 16.0f));
        ImGui::SameLine();
        ImGui::Text("F%u", plan.tool_per_perimeter[k] + 1);
        if (k + 1 < plan.tool_per_perimeter.size())
            ImGui::SameLine();
    }
    ImGui::Text("%s", ("dE2000 " + format((float)plan.delta_e, 1)).c_str());
}

void GLGizmoPrecisionALH::render_curve_editor(float width, float height)
{
    ImGuiIO&    io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##alh_band", ImVec2(width, height));
    const bool hovered         = ImGui::IsItemHovered();
    // IsItemDeactivated() is driven by ImGui's own active-id state machine, not
    // by catching the single-frame MouseReleased flag on the exact frame it
    // fires — robust even if the canvas doesn't repaint on that exact frame
    // (which is what silently dropped commits before this fix).
    const bool band_deactivated = ImGui::IsItemDeactivated();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const double z_max = std::max(m_object_height, 1e-6);
    const double h_min = m_slicing_params.min_layer_height > 0.0 ? m_slicing_params.min_layer_height : 0.01;
    const double h_max = std::max(m_slicing_params.max_layer_height, h_min + 1e-6);

    auto to_screen = [&](double z, double h) -> ImVec2 {
        const float u = float((h - h_min) / (h_max - h_min)); // 0..1 across width
        const float v = float(1.0 - z / z_max);                 // 0..1, top..bottom
        return ImVec2(p0.x + u * width, p0.y + v * height);
    };
    auto from_screen = [&](const ImVec2& s) -> std::pair<double, double> {
        const double u = std::clamp(double(s.x - p0.x) / double(width), 0.0, 1.0);
        const double v = std::clamp(double(s.y - p0.y) / double(height), 0.0, 1.0);
        return { h_min + u * (h_max - h_min), z_max * (1.0 - v) }; // { height, z }
    };

    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope, Frente 1. Resolved ONCE per
    // frame (Z-independent half only — has_color/app_config/materials), then
    // reused for every per-Z envelope query below via color_envelope_for_z(),
    // which is cheap (O(m_top_zone_bands) + arithmetic, no app_config reads).
    // Default-constructed base_ctx has empty painted_tools, so envelope_at()
    // is always safe to call even when the toggle is off — it just returns
    // passthrough, same as before.
    Slic3r::ColorSci::ColorHeightContext base_ctx;
    if (m_adapt_to_color)
        resolve_color_context(base_ctx);
    auto envelope_at = [this, &base_ctx](double z) -> Slic3r::ColorSci::ColorHeightEnvelope {
        return color_envelope_for_z(base_ctx, z);
    };
    // NEOTKO_ALHCOLOR_TAG — Fase 2 clamp REVISED (s222, user-reported): the original hard
    // per-Z drag clamp made points fight the cursor once the envelope became per-Z (Frente
    // 1) — crossing a Sandwich band edge mid-drag snapped the allowed range from free to
    // [0.32, 0.32] (a single value under conflict), i.e. flipping/immovable points. This
    // answers plan §6 P1 (soft vs hard) with SOFT at interaction time: dragging and
    // click-to-add move freely within the nozzle bounds, the envelope stays visible as
    // shading, an out-of-envelope point is tinted the conflict orange, and the HARD
    // guarantee lives where it always did — build_profile_vector() re-clamps the emitted
    // profile against a fresh envelope on every commit, so the slicer never sees a
    // color-unsafe height.
    auto outside_envelope = [&](double z, double h) -> bool {
        const auto env = envelope_at(z);
        return !env.passthrough && (h < env.h_min - 1e-9 || h > env.h_max + 1e-9);
    };

    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(24, 28, 33, 255), 3.0f);
    dl->AddRect(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(70, 78, 88, 255), 3.0f);

    constexpr ImU32 kForbidCol   = IM_COL32(200, 40, 40, 70);    // translucent red
    constexpr ImU32 kOptCol      = IM_COL32(50, 210, 90, 220);   // green — optimal
    constexpr ImU32 kConflictCol = IM_COL32(255, 140, 0, 230);  // orange — conflict override
    constexpr ImU32 kSlopeCol    = IM_COL32(150, 80, 220, 45);   // translucent violet — slope exposure (informational)

    // Curve — per-segment sampled polyline, straight or blended (§blended_height).
    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope: the forbidden-zone shading
    // and the h_opt line are now sampled together with the curve (same Z
    // samples, one envelope_at() call each) instead of one full-height band
    // from a single global env — the envelope varies per Z now
    // (m_top_zone_bands), so the shading must too. to_screen(z,h).x depends
    // ONLY on h and .y depends ONLY on z, so a rect per [z_prev,z] sample
    // interval reads directly off the same to_screen() used for the curve.
    // compute_color_height_envelope() guarantees nozzle_lo <= h_min <= h_max
    // <= nozzle_hi, so these rects can never invert or spill outside
    // [p0.x, p0.x+width].
    const std::vector<double> tangents = compute_tangents();
    constexpr int kDrawSamples = 24;
    ImVec2 opt_prev{};
    bool   have_opt_prev = false;
    for (size_t i = 0; i + 1 < m_points.size(); ++i) {
        ImVec2 prev = to_screen(m_points[i].z_mm, m_points[i].height_mm);
        double z_prev_sample = m_points[i].z_mm;
        for (int s = 1; s <= kDrawSamples; ++s) {
            const double t = double(s) / double(kDrawSamples);
            const double z = m_points[i].z_mm + t * (m_points[i + 1].z_mm - m_points[i].z_mm);

            const auto env = envelope_at(z);
            if (!env.passthrough) {
                const float y_top = to_screen(z, 0.0).y;
                const float y_bot = to_screen(z_prev_sample, 0.0).y;
                if (env.h_min > h_min) {                                    // fidelity floor
                    const float x_hi = to_screen(0.0, env.h_min).x;
                    dl->AddRectFilled(ImVec2(p0.x, y_top), ImVec2(x_hi, y_bot), kForbidCol);
                }
                if (env.h_max < h_max) {                                    // pattern-resolution ceiling
                    const float x_lo = to_screen(0.0, env.h_max).x;
                    dl->AddRectFilled(ImVec2(x_lo, y_top), ImVec2(p0.x + width, y_bot), kForbidCol);
                }
                // NEOTKO_ALHCOLOR_TAG — Fase 5.1 (Frente 2). Informational only, NOT a
                // clamp: heights above h_expose = w / tan_alpha make the staircase ledge at
                // this Z wider than one perimeter (d = h*tan > w), exposing interior rings
                // and visually breaking the MixedFilament pattern — until Fase 5.4 recolors
                // them, this shading is the honest "here the pattern degrades" signal, and
                // it doubles as the live A-vs-B regime view: drag the curve below the violet
                // edge and adaptive height (regime A) covers the slope instead.
                if (const double tan_alpha = slope_tan_at(z); tan_alpha > 0.0) {
                    const double h_expose = m_perimeter_width_mm / tan_alpha;
                    if (h_expose < h_max) {
                        const float x_lo = to_screen(0.0, std::max(h_expose, h_min)).x;
                        dl->AddRectFilled(ImVec2(x_lo, y_top), ImVec2(p0.x + width, y_bot), kSlopeCol);
                    }
                }
                const ImVec2 opt_pt = to_screen(z, env.h_opt);
                if (have_opt_prev)
                    dl->AddLine(opt_prev, opt_pt, env.conflict ? kConflictCol : kOptCol, 2.0f);
                opt_prev      = opt_pt;
                have_opt_prev = true;
            } else {
                have_opt_prev = false; // break the h_opt polyline across passthrough gaps
            }

            const double h = blended_height(i, t, tangents, env.passthrough ? nullptr : &env);
            const ImVec2 cur = to_screen(z, h);
            dl->AddLine(prev, cur, IM_COL32(0, 150, 136, 255), 2.0f);
            prev = cur;
            z_prev_sample = z;
        }
    }

    // Points: bottom (index 0) is the fixed first layer — drawn gray/locked;
    // top endpoint amber (height-movable); interior points teal (fully movable).
    int hovered_point = -1;
    for (size_t i = 0; i < m_points.size(); ++i) {
        const ImVec2 c = to_screen(m_points[i].z_mm, m_points[i].height_mm);
        const bool   locked = point_is_locked((int)i);
        const bool   is_top = (i + 1 == m_points.size());
        const float  r = ((int)i == m_dragging_point) ? 6.5f : 5.0f;
        if (!locked && hovered && std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) <= 8.0f)
            hovered_point = (int)i;
        ImU32 col = locked ? IM_COL32(150, 150, 150, 255)
                           : (is_top ? IM_COL32(230, 180, 40, 255) : IM_COL32(38, 198, 182, 255));
        // NEOTKO_ALHCOLOR_TAG — s222 soft-clamp revision: a point sitting outside its Z's
        // color envelope shows the conflict orange — commit will pull the emitted profile
        // back inside (build_profile_vector), this is the heads-up.
        if (!locked && outside_envelope(m_points[i].z_mm, m_points[i].height_mm))
            col = IM_COL32(255, 140, 0, 255);
        dl->AddCircleFilled(c, r, col);
        dl->AddCircle(c, r, IM_COL32(20, 20, 20, 255), 16, 1.2f);
    }

    m_hover_point = hovered ? hovered_point : -1;

    // Drag lifecycle: start on click-on-point, live-move while held, commit on
    // release. The release is detected via band_deactivated (see declaration
    // above), NOT a raw mouse-up flag — that was the root cause of edits never
    // sticking (the exact release frame could be missed if nothing else forced
    // a repaint right then, so commit() was simply never reached).
    // Point 0 (z=0) is the fixed first layer — locked, not draggable.
    if (m_dragging_point < 0 && hovered_point > 0 && io.MouseClicked[0]) {
        m_dragging_point = hovered_point;
    } else if (hovered && hovered_point < 0 && m_dragging_point < 0 && io.MouseClicked[0]) {
        // Click on empty band area: add a point, sorted into place by z.
        const auto [h, z] = from_screen(io.MousePos);
        size_t insert_at = m_points.size();
        for (size_t i = 0; i < m_points.size(); ++i)
            if (z < m_points[i].z_mm) { insert_at = i; break; }
        if (insert_at > 0 && insert_at < m_points.size()) {
            const double z_lo = m_points[insert_at - 1].z_mm + 0.02;
            const double z_hi = m_points[insert_at].z_mm - 0.02;
            if (z_hi > z_lo) {
                const double z_clamped = std::clamp(z, z_lo, z_hi);
                m_points.insert(m_points.begin() + insert_at,
                                 ALHPoint{ z_clamped, std::clamp(h, h_min, h_max), 0.0 });
                commit("Precision layer height - Add point");
            }
        }
    }

    if (m_dragging_point >= 0 && ImGui::IsItemActive()) {
        const auto [h, z] = from_screen(io.MousePos);
        ALHPoint&  pt = m_points[(size_t)m_dragging_point];
        const bool is_end = (m_dragging_point == 0 || (size_t)(m_dragging_point + 1) == m_points.size());
        if (!is_end) {
            const double z_lo = m_points[m_dragging_point - 1].z_mm + 0.02;
            const double z_hi = m_points[m_dragging_point + 1].z_mm - 0.02;
            if (z_hi > z_lo)
                pt.z_mm = std::clamp(z, z_lo, z_hi);
        }
        pt.height_mm = std::clamp(h, h_min, h_max);
        m_hover_point = m_dragging_point;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame(); // keep repainting every frame while held, not just on mouse-move
    }
    if (m_dragging_point >= 0 && band_deactivated) {
        commit("Precision layer height - Move point");
        m_dragging_point = -1;
    }

    // Delete: right-click a point (the two endpoints can't be deleted).
    if (hovered && hovered_point > 0 && (size_t)hovered_point + 1 < m_points.size() && io.MouseClicked[1]) {
        m_points.erase(m_points.begin() + hovered_point);
        commit("Precision layer height - Delete point");
    }

    // Numeric feedback for the point being moved or hovered — the resolution
    // (layer height) and Z it's currently set to.
    const int shown_idx = (m_dragging_point >= 0) ? m_dragging_point : m_hover_point;
    if (shown_idx >= 0) {
        const ImVec2      c     = to_screen(m_points[shown_idx].z_mm, m_points[shown_idx].height_mm);
        const std::string label = "Z " + format((float)m_points[shown_idx].z_mm, 2)
                                 + " mm   H " + format((float)m_points[shown_idx].height_mm, 3) + " mm";
        const ImVec2 tsz = ImGui::CalcTextSize(label.c_str());
        const ImVec2 lp(std::clamp(c.x - tsz.x * 0.5f, p0.x, p0.x + width - tsz.x), c.y - tsz.y - 10.0f);
        dl->AddRectFilled(ImVec2(lp.x - 4.0f, lp.y - 2.0f), ImVec2(lp.x + tsz.x + 4.0f, lp.y + tsz.y + 2.0f),
                          IM_COL32(20, 20, 20, 220), 3.0f);
        dl->AddText(lp, IM_COL32(255, 255, 255, 255), label.c_str());
    }
}

void GLGizmoPrecisionALH::on_render_input_window(float x, float y, float bottom_limit)
{
    ensure_session();

    const float win_w = 380.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);

    if (!m_have_session) {
        ImGui::TextWrapped("%s", _u8L("Select a single object to edit its layer height curve.").c_str());
        GizmoImguiEnd();
        return;
    }

    ImGui::TextWrapped("%s", _u8L("Click to add a point, drag to move it, right-click to delete it. "
                                   "The bottom point is the fixed first layer and the top point can "
                                   "only move in height.").c_str());
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    render_curve_editor(win_w - 16.0f, 220.0f);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Text("%s", (_u8L("Min layer height") + ": " + format((float)m_slicing_params.min_layer_height, 3) + " mm").c_str());
    ImGui::Text("%s", (_u8L("Max layer height") + ": " + format((float)m_slicing_params.max_layer_height, 3) + " mm").c_str());

    ImGui::Separator();
    if (ImGui::Checkbox(_u8L("Adapt to Color").c_str(), &m_adapt_to_color))
        m_parent.set_as_dirty();
    // NEOTKO_ALHCOLOR_TAG — Fase 5.3 opt-in. Toggling commits immediately so the stored
    // blob appears/disappears in the same user action (and lands on the undo stack).
    // Inert until Fase 5.4 — the engine doesn't read the blob yet.
    ImGui::SameLine();
    if (ImGui::Checkbox(_u8L("Slope recolor").c_str(), &m_slope_recolor_enabled))
        commit(m_slope_recolor_enabled ? "Precision layer height - Slope recolor on"
                                       : "Precision layer height - Slope recolor off");

    // NEOTKO_ALHCOLOR_TAG — replanteo TD-vs-slope, Frente 1. The envelope is
    // per-Z now — show the range for whichever point is being dragged/
    // hovered (the Z the user is actually looking at), or Z=0 (the fixed
    // first layer, essentially never inside a Sandwich zone) when nothing is
    // focused. passthrough/has_color is Z-independent (driven only by
    // whether the object has any color at all — see resolve_color_context()),
    // so this choice of Z never changes whether the info block shows at all,
    // only the specific numbers it reports.
    const int    info_idx = (m_dragging_point >= 0) ? m_dragging_point : m_hover_point;
    const double info_z   = (info_idx >= 0 && (size_t)info_idx < m_points.size()) ? m_points[info_idx].z_mm : 0.0;
    Slic3r::ColorSci::ColorHeightEnvelope color_env;
    const bool have_color_env = m_adapt_to_color && compute_active_color_envelope(color_env, info_z);

    if (m_adapt_to_color && have_color_env) {
        // NEOTKO_ALHCOLOR_TAG — Fase 5.1/5.2 UX fix (s222, user-reported): this whole block
        // changes content on HOVER (conflict line, slope readout, recolor preview appear/
        // disappear per focused point). Inside an AlwaysAutoResize window that made the
        // window grow/shrink every time the cursor touched a point, which moved the curve
        // band under the cursor, which lost the hover — an unusable feedback loop. Fixed-
        // height child = the window's size no longer depends on what's hovered; unused
        // space just stays empty. Long notices are TextWrapped (they were clipped at the
        // fixed window width before).
        const float info_h = ImGui::GetTextLineHeightWithSpacing() * 11.0f;
        ImGui::BeginChild("##alh_color_info", ImVec2(win_w - 16.0f, info_h), false,
                          ImGuiWindowFlags_NoScrollbar);
        if (color_env.passthrough) {
            ImGui::TextWrapped("%s", _u8L("No color painted on this object — "
                                           "layer height is unrestricted.").c_str());
        } else {
            if (color_env.conflict) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.15f, 1.0f));
                ImGui::TextWrapped("%s",
                    _u8L("Color fidelity and pattern resolution don't overlap here "
                         "— prioritizing color fidelity.").c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Text("%s", (_u8L("Color-safe height range") + ": "
                + format((float)color_env.h_min, 3) + " - " + format((float)color_env.h_max, 3)
                + " mm (" + _u8L("optimal") + " " + format((float)color_env.h_opt, 3) + " mm)").c_str());
            // NEOTKO_ALHCOLOR_TAG — Fase 2 (plan §4.c) + replanteo TD-vs-slope
            // (Frente 1). One click rewrites every editable point's height to
            // ITS OWN Z's h_opt (the envelope is per-Z now, not one shared
            // value) and commits — a ready-to-slice, color-optimal profile.
            // Skips the locked first point on purpose (plan §6 R4).
            if (ImGui::Button(_u8L("Snap to optimal").c_str())) {
                Slic3r::ColorSci::ColorHeightContext base_ctx;
                resolve_color_context(base_ctx);
                for (size_t i = 0; i < m_points.size(); ++i)
                    if (!point_is_locked((int)i))
                        m_points[i].height_mm = color_envelope_for_z(base_ctx, m_points[i].z_mm).h_opt;
                commit("Precision layer height - Snap to optimal");
            }

            // NEOTKO_ALHCOLOR_TAG — Fase 5.1+5.2 (Frente 2). Slope-exposure readout +
            // read-only recolor suggestion for the focused point — see the method's own
            // header comment for the full contract.
            if (info_idx >= 0 && (size_t)info_idx < m_points.size())
                render_slope_recolor_preview(info_z, m_points[info_idx].height_mm);
        }
        ImGui::EndChild();
    }

    if (m_points.size() > 2) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", _u8L("Tension per segment (0 = straight, 1 = smooth curve):").c_str());
        for (size_t i = 0; i + 1 < m_points.size(); ++i) {
            float             t     = (float)m_points[i].tension;
            const std::string label = format((float)m_points[i].z_mm, 1) + " - "
                                     + format((float)m_points[i + 1].z_mm, 1) + " mm##tension" + std::to_string(i);
            if (ImGui::SliderFloat(label.c_str(), &t, 0.0f, 1.0f)) {
                m_points[i].tension = std::clamp((double)t, 0.0, 1.0);
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                commit("Precision layer height - Tension");
        }
    }

    ImGui::Separator();
    if (ImGui::Button(_u8L("Reset").c_str())) {
        seed_flat_profile();
        commit("Precision layer height - Reset");
    }

    GizmoImguiEnd();
}

}} // namespace Slic3r::GUI
// NEOTKO_PRECISIONALH_TAG_END
