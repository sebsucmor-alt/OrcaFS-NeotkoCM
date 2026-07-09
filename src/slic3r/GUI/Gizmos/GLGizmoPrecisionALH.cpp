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

#include <imgui/imgui.h>
#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <cmath>

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

double GLGizmoPrecisionALH::blended_height(size_t seg_idx, double t, const std::vector<double>& tangents) const
{
    const ALHPoint& p0 = m_points[seg_idx];
    const ALHPoint& p1 = m_points[seg_idx + 1];
    const double linear = p0.height_mm + t * (p1.height_mm - p0.height_mm);

    const double tension = std::clamp(p0.tension, 0.0, 1.0);
    if (tension <= 1e-6)
        return linear;

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
    // same endpoints should already stay within range, this just guards fp noise.
    return std::clamp(blended, m_slicing_params.min_layer_height, m_slicing_params.max_layer_height);
}

std::vector<coordf_t> GLGizmoPrecisionALH::build_profile_vector() const
{
    std::vector<coordf_t> out;
    if (m_points.size() < 2)
        return out;

    const std::vector<double> tangents = compute_tangents();
    // First pair MUST be exactly [0, first_object_layer_height] or the slicer
    // discards the whole profile (PrintObject::update_layer_height_profile).
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
            out.push_back(p1.height_mm);
            continue;
        }
        for (int s = 1; s <= kSamples; ++s) {
            const double t = double(s) / double(kSamples);
            out.push_back(p0.z_mm + t * (p1.z_mm - p0.z_mm));
            out.push_back(blended_height(i, t, tangents));
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
    const_cast<ModelObject*>(model->objects[m_object_idx])->layer_height_profile.set(build_profile_vector());
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

    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(24, 28, 33, 255), 3.0f);
    dl->AddRect(p0, ImVec2(p0.x + width, p0.y + height), IM_COL32(70, 78, 88, 255), 3.0f);

    // Curve — per-segment sampled polyline, straight or blended (§blended_height).
    const std::vector<double> tangents = compute_tangents();
    constexpr int kDrawSamples = 24;
    for (size_t i = 0; i + 1 < m_points.size(); ++i) {
        ImVec2 prev = to_screen(m_points[i].z_mm, m_points[i].height_mm);
        for (int s = 1; s <= kDrawSamples; ++s) {
            const double t = double(s) / double(kDrawSamples);
            const double z = m_points[i].z_mm + t * (m_points[i + 1].z_mm - m_points[i].z_mm);
            const double h = blended_height(i, t, tangents);
            const ImVec2 cur = to_screen(z, h);
            dl->AddLine(prev, cur, IM_COL32(0, 150, 136, 255), 2.0f);
            prev = cur;
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
        const ImU32 col = locked ? IM_COL32(150, 150, 150, 255)
                                 : (is_top ? IM_COL32(230, 180, 40, 255) : IM_COL32(38, 198, 182, 255));
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
                m_points.insert(m_points.begin() + insert_at,
                                 ALHPoint{ std::clamp(z, z_lo, z_hi), std::clamp(h, h_min, h_max), 0.0 });
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
