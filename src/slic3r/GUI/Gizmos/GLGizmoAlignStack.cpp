// NEOTKO_ALIGNSTACK_TAG_START
#include "GLGizmoAlignStack.hpp"
#include "GLGizmoMeasure.hpp" // for TransformHelper::world_to_ss

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/Model.hpp"

#include <imgui/imgui.h>

#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace Slic3r { namespace GUI {

// =============================================================================
// Visual language: per-letter colors for badges/chips, and procedurally drawn
// isometric mini-cube icon buttons (no SVG assets, no toolbar tint pipeline).
// =============================================================================

namespace {

const ImU32 kOrderColors[] = {
    IM_COL32(255, 140,   0, 235), // A orange
    IM_COL32(240, 200,   0, 235), // B yellow
    IM_COL32( 70, 180,  80, 235), // C green
    IM_COL32( 40, 180, 200, 235), // D cyan
    IM_COL32(180,  90, 230, 235), // E violet
    IM_COL32(230,  80, 120, 235), // F pink
};
constexpr size_t kOrderColorCount = sizeof(kOrderColors) / sizeof(kOrderColors[0]);

inline ImU32 order_color(size_t i) { return kOrderColors[i % kOrderColorCount]; }

// Same palette as ColorRGBA for tinting scene volumes (kept in sync above).
inline ColorRGBA order_color_rgba(size_t i)
{
    const ImU32 c = order_color(i);
    return ColorRGBA((float)((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                     (float)((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                     (float)((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                     1.0f);
}

enum class CubeIcon {
    TouchZPos,  // plane descends onto the top face (stack)
    TouchXPos,  // plane presses against the right (+X) face
    TouchXNeg,  // mirrored: plane presses against the left (-X) face
    TouchYNeg,  // plane presses against the front (-Y) face
    TouchYPos,  // mirrored: plane presses against the back (+Y) face
    Bed,        // cube drops onto the ground line
    CenterX,    // translucent mid slab perpendicular to X
    CenterY,    // mid slab perpendicular to Y
    CenterZ,    // mid slab perpendicular to Z
};

// Isometric mini-cube icon button. The highlighted face + incoming plane with
// arrows tells the user what the operation does. In flush mode the plane is
// drawn nearly coincident with the face (faces become coplanar, not stacked).
bool cube_icon_button(const char* id, CubeIcon icon, float size, bool flush_mode,
                      const char* axis_label)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();

    const bool mirror = (icon == CubeIcon::TouchXNeg || icon == CubeIcon::TouchYPos);
    auto P = [&](float u, float v) {
        if (mirror) u = 1.0f - u;
        return ImVec2(p0.x + u * size, p0.y + v * size);
    };

    // Colors
    const ImU32 col_line  = IM_COL32(43, 52, 62, 255);
    const ImU32 col_top   = IM_COL32(214, 214, 214, 255);
    const ImU32 col_left  = IM_COL32(168, 168, 168, 255);
    const ImU32 col_right = IM_COL32(128, 128, 128, 255);
    const ImU32 col_hl    = hovered ? IM_COL32(38, 198, 182, 255)
                                    : IM_COL32(0, 150, 136, 255); // Orca teal
    const ImU32 col_plane_fill = IM_COL32(255, 255, 255, 60);

    if (hovered)
        dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                          IM_COL32(255, 255, 255, 26), 4.0f);

    // Base cube geometry in [0,1] button space (slightly low, room for planes).
    // Iso axes on screen: +X = (0.28, 0.14), +Y = (0.28, -0.14), +Z = up.
    const float lift = (icon == CubeIcon::Bed) ? -0.10f : 0.0f;
    auto F = [&](std::initializer_list<std::pair<float, float>> uv, ImU32 fill) {
        ImVec2 pts[4]; int n = 0;
        for (auto& q : uv) pts[n++] = P(q.first, q.second + lift);
        dl->AddConvexPolyFilled(pts, n, fill);
        dl->AddPolyline(pts, n, col_line, ImDrawFlags_Closed, 1.2f);
    };

    const std::initializer_list<std::pair<float, float>> top_face =
        { {0.50f, 0.30f}, {0.78f, 0.44f}, {0.50f, 0.58f}, {0.22f, 0.44f} };
    const std::initializer_list<std::pair<float, float>> left_face =   // -Y
        { {0.22f, 0.44f}, {0.50f, 0.58f}, {0.50f, 0.88f}, {0.22f, 0.74f} };
    const std::initializer_list<std::pair<float, float>> right_face =  // +X
        { {0.50f, 0.58f}, {0.78f, 0.44f}, {0.78f, 0.74f}, {0.50f, 0.88f} };

    const bool hl_top   = (icon == CubeIcon::TouchZPos);
    const bool hl_right = (icon == CubeIcon::TouchXPos || icon == CubeIcon::TouchXNeg);
    const bool hl_left  = (icon == CubeIcon::TouchYNeg || icon == CubeIcon::TouchYPos);

    F(top_face,   hl_top   ? col_hl : col_top);
    F(left_face,  hl_left  ? col_hl : col_left);
    F(right_face, hl_right ? col_hl : col_right);

    // Incoming plane (offset copy of the target face) + two approach arrows.
    const float k = flush_mode ? 0.18f : 0.55f; // flush: almost touching
    auto plane = [&](std::initializer_list<std::pair<float, float>> face,
                     float dx, float dy) {
        ImVec2 pts[4]; int n = 0;
        for (auto& q : face) pts[n++] = P(q.first + dx * k, q.second + dy * k + lift);
        dl->AddConvexPolyFilled(pts, n, col_plane_fill);
        dl->AddPolyline(pts, n, col_line, ImDrawFlags_Closed, 1.2f);
    };
    auto arrow = [&](ImVec2 a, ImVec2 b) {
        dl->AddLine(a, b, col_line, 1.4f);
        ImVec2 d(b.x - a.x, b.y - a.y);
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1e-3f) return;
        d.x /= len; d.y /= len;
        const ImVec2 perp(-d.y, d.x);
        const float h = 0.08f * size;
        dl->AddTriangleFilled(b,
                              ImVec2(b.x - d.x * h + perp.x * h * 0.6f, b.y - d.y * h + perp.y * h * 0.6f),
                              ImVec2(b.x - d.x * h - perp.x * h * 0.6f, b.y - d.y * h - perp.y * h * 0.6f),
                              col_line);
    };

    switch (icon) {
    case CubeIcon::TouchZPos:
        plane(top_face, 0.0f, -0.42f);              // plane floating above
        arrow(P(0.12f, 0.18f), P(0.12f, 0.34f));    // flanking down arrows
        arrow(P(0.88f, 0.18f), P(0.88f, 0.34f));
        break;
    case CubeIcon::TouchXPos:
    case CubeIcon::TouchXNeg:
        plane(right_face, 0.28f, 0.14f);            // plane out along +X
        arrow(P(0.97f, 0.30f), P(0.85f, 0.24f));    // arrows pushing in (-X)
        arrow(P(0.97f, 0.95f), P(0.85f, 0.89f));
        break;
    case CubeIcon::TouchYNeg:
    case CubeIcon::TouchYPos:
        plane(left_face, -0.28f, 0.14f);            // plane out along -Y
        arrow(P(0.03f, 0.30f), P(0.15f, 0.24f));    // arrows pushing in (+Y)
        arrow(P(0.03f, 0.95f), P(0.15f, 0.89f));
        break;
    case CubeIcon::Bed: {
        dl->AddLine(P(0.08f, 0.92f), P(0.92f, 0.92f), col_hl, 2.2f);
        arrow(P(0.30f, 0.68f), P(0.30f, 0.88f));
        arrow(P(0.70f, 0.68f), P(0.70f, 0.88f));
        break;
    }
    case CubeIcon::CenterX: {
        // Mid slab perpendicular to X = right face pulled back to the center.
        ImVec2 pts[4]; int n = 0;
        for (auto& q : right_face) pts[n++] = P(q.first - 0.14f, q.second - 0.07f);
        dl->AddConvexPolyFilled(pts, n, (col_hl & 0x00FFFFFF) | (200u << 24));
        dl->AddPolyline(pts, n, col_line, ImDrawFlags_Closed, 1.2f);
        break;
    }
    case CubeIcon::CenterY: {
        ImVec2 pts[4]; int n = 0;
        for (auto& q : left_face) pts[n++] = P(q.first + 0.14f, q.second - 0.07f);
        dl->AddConvexPolyFilled(pts, n, (col_hl & 0x00FFFFFF) | (200u << 24));
        dl->AddPolyline(pts, n, col_line, ImDrawFlags_Closed, 1.2f);
        break;
    }
    case CubeIcon::CenterZ: {
        ImVec2 pts[4]; int n = 0;
        for (auto& q : top_face) pts[n++] = P(q.first, q.second + 0.145f);
        dl->AddConvexPolyFilled(pts, n, (col_hl & 0x00FFFFFF) | (200u << 24));
        dl->AddPolyline(pts, n, col_line, ImDrawFlags_Closed, 1.2f);
        break;
    }
    }

    // Tiny axis label so mirrored twins are unambiguous at a glance.
    if (axis_label != nullptr && axis_label[0] != '\0')
        dl->AddText(ImGui::GetFont(), size * 0.26f,
                    ImVec2(p0.x + 2.0f, p0.y + size - size * 0.26f - 1.0f),
                    col_line, axis_label);

    return pressed;
}

} // anonymous namespace

// =============================================================================
// Gizmo
// =============================================================================

GLGizmoAlignStack::GLGizmoAlignStack(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoAlignStack::on_init()
{
    m_shortcut_key = 0; // no global shortcut by default
    return true;
}

std::string GLGizmoAlignStack::on_get_name() const
{
    return _u8L("Align & Stack");
}

bool GLGizmoAlignStack::on_is_activable() const
{
    // NEOTKO_ALIGNSTACK_TAG: LibreMode feature — only offered when the LibreMode
    // master switch is enabled (Preferences). When on, it is usable even with an
    // empty scene/selection: the user builds the order by clicking objects.
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled");
}

void GLGizmoAlignStack::on_set_state()
{
    if (get_state() == On) {
        seed_order_from_selection();
        apply_highlight();
    } else if (get_state() == Off) {
        restore_highlight();
        m_ordered_object_idxs.clear();
        clear_face_pick();
    }
}

void GLGizmoAlignStack::data_changed(bool /*is_serializing*/)
{
    // Fires on selection changes and on volume rebuilds. apply_highlight()
    // restores via stable ids first (no dangling pointers), then re-tints —
    // so tints never accumulate and removed objects regain their color.
    prune_dead_objects();
    if (get_state() == On)
        apply_highlight();
}

// -----------------------------------------------------------------------------
// Order tracking — owned by the gizmo, selection is mirrored so that
// Selection::translate() keeps acting on every ordered object.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::seed_order_from_selection()
{
    m_ordered_object_idxs.clear();
    for (const auto& kv : m_parent.get_selection().get_content())
        m_ordered_object_idxs.push_back(kv.first);
}

void GLGizmoAlignStack::prune_dead_objects()
{
    const Model* model = m_parent.get_selection().get_model();
    const int n_objects = model ? (int)model->objects.size() : 0;
    m_ordered_object_idxs.erase(
        std::remove_if(m_ordered_object_idxs.begin(), m_ordered_object_idxs.end(),
                       [&](int idx) { return idx < 0 || idx >= n_objects; }),
        m_ordered_object_idxs.end());
}

void GLGizmoAlignStack::toggle_object_order(int object_idx)
{
    Selection& sel = m_parent.get_selection();
    auto it = std::find(m_ordered_object_idxs.begin(), m_ordered_object_idxs.end(), object_idx);
    if (it != m_ordered_object_idxs.end()) {
        m_ordered_object_idxs.erase(it);
        sel.remove_object((unsigned int)object_idx);
    } else {
        m_ordered_object_idxs.push_back(object_idx);
        sel.add_object((unsigned int)object_idx, false);
    }
    // Order changed → a previously picked face on the old anchor is stale.
    if (m_face_pick_mode || m_has_picked_face)
        clear_face_pick();
    refresh_highlight();
}

// -----------------------------------------------------------------------------
// Scene highlight — tint each ordered object's volumes with its order color
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::restore_highlight()
{
    if (m_saved_colors.empty())
        return;
    // Restore by matching live volumes against saved ids; stale ids (volumes
    // that no longer exist after a rebuild) simply don't match and are dropped.
    for (GLVolume* v : m_parent.get_volumes().volumes) {
        if (v == nullptr)
            continue;
        auto it = m_saved_colors.find(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()));
        if (it != m_saved_colors.end())
            v->set_color(it->second);
    }
    m_saved_colors.clear();
}

void GLGizmoAlignStack::apply_highlight()
{
    // Always restore first so we never save an already-tinted color as the
    // "original" (which would make tints accumulate/darken on each refresh).
    restore_highlight();

    // While picking a face, ghost the whole scene so the highlighted face pops.
    // (render_color picks up the lowered alpha → goes to the transparent pass.)
    const bool  ghost   = m_face_pick_mode;
    const float ghost_a = 0.30f; // dial: lower = fainter scene

    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    for (GLVolume* v : volumes) {
        if (v == nullptr || v->is_modifier || v->is_wipe_tower)
            continue;

        // Order index of this volume's object, if it is part of the order.
        int order = -1;
        for (size_t i = 0; i < m_ordered_object_idxs.size(); ++i)
            if (m_ordered_object_idxs[i] == v->object_idx()) { order = (int)i; break; }

        // #1 stays untinted while picking a face on it so the overlay reads
        // cleanly; every other ordered object keeps its order color.
        const bool do_tint = (order >= 0) && !(m_face_pick_mode && order == 0);
        if (!do_tint && !ghost)
            continue; // nothing to do for this volume

        const ColorRGBA orig = v->color;
        ColorRGBA out = orig;
        if (do_tint) {
            const ColorRGBA letter = order_color_rgba((size_t)order);
            // Blend 72% toward the letter color so geometry shading still reads.
            for (int k = 0; k < 3; ++k)
                out[k] = 0.28f * orig[k] + 0.72f * letter[k];
        }
        out[3] = ghost ? ghost_a : orig[3];

        m_saved_colors.emplace(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()), orig);
        v->set_color(out);
    }
}

void GLGizmoAlignStack::refresh_highlight()
{
    apply_highlight();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// -----------------------------------------------------------------------------
// World bounding box helper
// -----------------------------------------------------------------------------

BoundingBoxf3 GLGizmoAlignStack::world_bbox_of_object(int object_idx) const
{
    BoundingBoxf3 out;
    const Model* model = m_parent.get_selection().get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size())
        return out;
    const ModelObject* mo = model->objects[object_idx];
    for (size_t i = 0; i < mo->instances.size(); ++i)
        out.merge(mo->instance_bounding_box(i));
    return out;
}

// -----------------------------------------------------------------------------
// Render — colored letter badges, no 3D handles
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::on_render()
{
    prune_dead_objects();
    render_face_highlights();
    render_badges();
}

void GLGizmoAlignStack::render_badges()
{
    if (m_ordered_object_idxs.empty())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Matrix4d proj_view = (camera.get_projection_matrix() * camera.get_view_matrix()).matrix();
    const std::array<int, 4>& viewport = camera.get_viewport();

    ImDrawList* fg = ImGui::GetForegroundDrawList();

    for (size_t i = 0; i < m_ordered_object_idxs.size(); ++i) {
        const int obj_idx = m_ordered_object_idxs[i];
        const BoundingBoxf3 bb = world_bbox_of_object(obj_idx);
        if (!bb.defined)
            continue;

        // Billboard a big numbered disc just above the object's top.
        Vec3d anchor = bb.center();
        anchor.z()   = bb.max.z() + 2.0;
        const Vec2d ss = TransformHelper::world_to_ss(anchor, proj_view, viewport);
        const ImVec2 center((float)ss.x(), (float)(viewport[3] - ss.y()));

        const ImU32 col      = order_color(i);
        const float radius   = 17.0f;
        const bool  anchor_n = (i == 0); // #1 = the anchor

        // Disc + outline (anchor gets a thicker ring to read as "the base").
        fg->AddCircleFilled(center, radius, col, 32);
        fg->AddCircle(center, radius, IM_COL32(43, 52, 62, 255),
                      32, anchor_n ? 3.5f : 1.8f);

        // Big centered number.
        const std::string num = std::to_string((int)i + 1);
        ImFont* font = ImGui::GetFont();
        const float fsize = 26.0f;
        const ImVec2 tsz  = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, num.c_str());
        fg->AddText(font, fsize,
                    ImVec2(center.x - tsz.x * 0.5f, center.y - tsz.y * 0.5f),
                    IM_COL32(20, 20, 20, 255), num.c_str());
    }
}

// -----------------------------------------------------------------------------
// Mouse — click-to-order in the scene, plus face-pick mode
// -----------------------------------------------------------------------------

bool GLGizmoAlignStack::on_mouse(const wxMouseEvent& mouse_event)
{
    if (m_face_pick_mode) {
        // Hover feedback: remember where the cursor is so on_render can light up
        // the triangle under it. Don't consume Moving — camera/hover stay live.
        if (mouse_event.Moving()) {
            m_hover_mouse_pos = Vec2d(mouse_event.GetX(), mouse_event.GetY());
            m_have_hover_pos  = true;
            m_parent.set_as_dirty();
            m_parent.request_extra_frame();
            return false;
        }
        if (mouse_event.LeftDown()) {
            const int a_idx = ordered_obj(0);
            if (a_idx < 0)
                return false;

            ensure_face_raycaster_for_A();
            if (!m_face_raycaster)
                return false;

            const Camera& camera = wxGetApp().plater()->get_camera();
            const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());

            Vec3f  hit_local  { 0.f, 0.f, 0.f };
            Vec3f  hit_normal { 0.f, 0.f, 1.f };
            size_t facet_idx  = 0;

            if (m_face_raycaster->unproject_on_mesh(mouse_pos,
                                                    m_face_raycaster_world_trafo,
                                                    camera, hit_local, hit_normal,
                                                    nullptr, &facet_idx)) {
                const Vec3d hit_world =
                    m_face_raycaster_world_trafo * hit_local.cast<double>();
                m_picked_face_world_pos    = hit_world;
                m_picked_face_world_normal = (m_face_raycaster_world_trafo.linear()
                                               * hit_normal.cast<double>()).normalized();
                m_picked_face_world_z      = hit_world.z();
                m_has_picked_face          = true;
                m_picked_facet_idx         = (int)facet_idx; // confirmed-face overlay
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
                // Stay in pick mode so the user can re-pick.
                return true;
            }
        }
        return false;
    }

    // Click-to-order: clicking an object in the scene assigns the next letter;
    // clicking an already-lettered object removes it from the order.
    if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
        const int hovered = m_parent.get_first_hover_volume_idx();
        if (hovered < 0)
            return false; // empty space → camera keeps working as usual
        const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
        if (hovered >= (int)volumes.size() || volumes[hovered] == nullptr)
            return false;
        const int obj_idx = volumes[hovered]->object_idx();
        const Model* model = m_parent.get_selection().get_model();
        if (!model || obj_idx < 0 || obj_idx >= (int)model->objects.size())
            return false;
        toggle_object_order(obj_idx);
        return true; // consume: keep global selection stable while ordering
    }
    return false;
}

void GLGizmoAlignStack::ensure_face_raycaster_for_A()
{
    const int a_idx = ordered_obj(0);
    if (a_idx < 0) {
        clear_face_pick();
        return;
    }
    const Model* model = m_parent.get_selection().get_model();
    if (!model || a_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[a_idx];
    if (mo->instances.empty())
        return;

    // Refresh the world transform every call so a moved/rotated #1 never leaves
    // a stale raycaster (cheap); only rebuild the mesh + raycaster when #1 itself
    // changes (raw_mesh() is object-space, so we render with view * this trafo).
    m_face_raycaster_world_trafo = mo->instances.front()->get_matrix();
    if (m_face_raycaster && m_face_raycaster_obj_idx == a_idx)
        return;

    m_face_mesh = mo->raw_mesh();
    m_face_raycaster.reset(new MeshRaycaster(m_face_mesh)); // copies internally
    m_face_raycaster_obj_idx = a_idx;
    // Cache adjacency + normals for the coplanar flood-fill (computed once per #1).
    m_face_normals   = its_face_normals(m_face_mesh.its);
    m_face_neighbors = its_face_neighbors(m_face_mesh.its);
    m_hover_facet_idx = m_hover_model_facet = -1; // force highlight rebuild
}

void GLGizmoAlignStack::clear_face_pick()
{
    m_face_pick_mode         = false;
    m_has_picked_face        = false;
    m_face_raycaster.reset();
    m_face_raycaster_obj_idx = -1;
    m_have_hover_pos         = false;
    m_hover_facet_idx        = -1;
    m_hover_model_facet      = -1;
    m_picked_facet_idx       = -1;
    m_picked_model_facet     = -1;
    m_hover_face_model.reset();
    m_picked_face_model.reset();
}

// -----------------------------------------------------------------------------
// Face highlight — light up the triangle under the cursor (and the picked one)
// so the user gets feedback that a face is about to be / has been chosen.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::build_face_model(GLModel& model, int facet_idx, const ColorRGBA& col)
{
    model.reset();
    const int n_facets = (int)m_face_mesh.its.indices.size();
    if (facet_idx < 0 || facet_idx >= n_facets ||
        (int)m_face_normals.size()   != n_facets ||
        (int)m_face_neighbors.size() != n_facets)
        return;

    // Grow the connected region of facets whose normal stays within a small
    // angle of the seed: a flat face fills completely, a curved one yields a
    // patch around the cursor. Capped so a near-flat huge mesh can't explode.
    const float    cos_thresh = 0.94f; // ~20 degrees
    const size_t   max_facets = 40000;
    const Vec3f    seed_n     = m_face_normals[facet_idx].normalized();

    std::vector<int> region;
    region.reserve(256);
    std::vector<char> visited(n_facets, 0);
    std::vector<int>  stack { facet_idx };
    visited[facet_idx] = 1;
    while (!stack.empty() && region.size() < max_facets) {
        const int f = stack.back();
        stack.pop_back();
        region.push_back(f);
        for (int e = 0; e < 3; ++e) {
            const int nb = m_face_neighbors[f][e];
            if (nb < 0 || nb >= n_facets || visited[nb])
                continue;
            if (m_face_normals[nb].normalized().dot(seed_n) >= cos_thresh) {
                visited[nb] = 1;
                stack.push_back(nb);
            }
        }
    }

    const float lift = 0.10f; // along each facet's own normal, beats z-fighting
    indexed_triangle_set its;
    its.vertices.reserve(region.size() * 3);
    its.indices.reserve(region.size());
    int base = 0;
    for (int f : region) {
        const Vec3i32 tri = m_face_mesh.its.indices[f];
        Vec3f n = m_face_normals[f];
        const float nl = n.norm();
        n = (nl > 1e-6f) ? (n / nl) : seed_n;
        its.vertices.push_back(m_face_mesh.its.vertices[tri[0]] + n * lift);
        its.vertices.push_back(m_face_mesh.its.vertices[tri[1]] + n * lift);
        its.vertices.push_back(m_face_mesh.its.vertices[tri[2]] + n * lift);
        its.indices.emplace_back(base, base + 1, base + 2);
        base += 3;
    }

    if (its.indices.empty())
        return;
    model.init_from(its);
    model.set_color(col);
}

void GLGizmoAlignStack::update_hover_face(const Vec2d& mouse_pos)
{
    ensure_face_raycaster_for_A();
    if (!m_face_raycaster) {
        m_hover_facet_idx = -1;
        return;
    }

    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3f  hit_local  { 0.f, 0.f, 0.f };
    Vec3f  hit_normal { 0.f, 0.f, 1.f };
    size_t facet_idx  = 0;

    if (m_face_raycaster->unproject_on_mesh(mouse_pos, m_face_raycaster_world_trafo,
                                            camera, hit_local, hit_normal,
                                            nullptr, &facet_idx))
        m_hover_facet_idx = (int)facet_idx;
    else
        m_hover_facet_idx = -1;
}

void GLGizmoAlignStack::render_face_highlights()
{
    if (!m_face_pick_mode)
        return;

    // Refresh the hovered facet from the last known cursor position.
    if (m_have_hover_pos)
        update_hover_face(m_hover_mouse_pos);

    // (Re)build overlays only when their facet changed.
    if (m_hover_facet_idx != m_hover_model_facet) {
        if (m_hover_facet_idx >= 0)
            build_face_model(m_hover_face_model, m_hover_facet_idx,
                             ColorRGBA(0.10f, 0.80f, 0.74f, 0.55f)); // teal hover
        else
            m_hover_face_model.reset();
        m_hover_model_facet = m_hover_facet_idx;
    }
    if (m_picked_facet_idx != m_picked_model_facet) {
        if (m_picked_facet_idx >= 0)
            build_face_model(m_picked_face_model, m_picked_facet_idx,
                             ColorRGBA(0.16f, 0.85f, 0.30f, 0.60f)); // green picked
        else
            m_picked_face_model.reset();
        m_picked_model_facet = m_picked_facet_idx;
    }

    if (!m_hover_face_model.is_initialized() && !m_picked_face_model.is_initialized())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE)); // raw-mesh facet winding varies; show both sides
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_model_matrix = camera.get_view_matrix() * m_face_raycaster_world_trafo;
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // Picked face underneath, live hover on top.
    if (m_picked_face_model.is_initialized())
        m_picked_face_model.render();
    if (m_hover_face_model.is_initialized())
        m_hover_face_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// -----------------------------------------------------------------------------
// Transform application — A is always the anchor; B, C... move toward it.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::translate_object(int object_idx, const Vec3d& delta)
{
    if (delta.norm() < 1e-9)
        return;
    Selection& sel = m_parent.get_selection();
    const Model* model = sel.get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[object_idx];
    for (size_t i = 0; i < mo->instances.size(); ++i)
        sel.translate((unsigned int)object_idx, (unsigned int)i, delta);
}

void GLGizmoAlignStack::apply_touch(int axis, int dir)
{
    if (m_ordered_object_idxs.size() < 2)
        return;

    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Stack: place against");

    if (axis == 2 && dir > 0 && m_place_a_on_bed) {
        const BoundingBoxf3 abb = world_bbox_of_object(m_ordered_object_idxs.front());
        if (abb.defined)
            translate_object(m_ordered_object_idxs.front(), Vec3d(0.0, 0.0, -abb.min.z()));
    }

    // Z contact gets the epsilon gap (slicing sanity); lateral contact is exact.
    const double gap = (axis == 2) ? (double)m_epsilon_mm : 0.0;

    BoundingBoxf3 ref = world_bbox_of_object(m_ordered_object_idxs.front());
    for (size_t i = 1; i < m_ordered_object_idxs.size(); ++i) {
        const int idx = m_ordered_object_idxs[i];
        const BoundingBoxf3 bb = world_bbox_of_object(idx);
        if (!bb.defined || !ref.defined)
            continue;
        Vec3d delta = Vec3d::Zero();
        delta(axis) = (dir > 0) ? (ref.max(axis) + gap - bb.min(axis))
                                : (ref.min(axis) - gap - bb.max(axis));
        translate_object(idx, delta);
        ref = world_bbox_of_object(idx); // chain: C goes against B
    }
    m_parent.do_move("");
}

void GLGizmoAlignStack::apply_flush(int axis, int dir)
{
    if (m_ordered_object_idxs.size() < 2)
        return;

    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Stack: flush align");

    const BoundingBoxf3 a_bb = world_bbox_of_object(m_ordered_object_idxs.front());
    if (!a_bb.defined)
        return;
    const double target = (dir > 0) ? a_bb.max(axis) : a_bb.min(axis);

    for (size_t i = 1; i < m_ordered_object_idxs.size(); ++i) {
        const int idx = m_ordered_object_idxs[i];
        const BoundingBoxf3 bb = world_bbox_of_object(idx);
        if (!bb.defined)
            continue;
        Vec3d delta = Vec3d::Zero();
        delta(axis) = target - ((dir > 0) ? bb.max(axis) : bb.min(axis));
        translate_object(idx, delta);
    }
    m_parent.do_move("");
}

void GLGizmoAlignStack::apply_center(int axis)
{
    if (m_ordered_object_idxs.size() < 2)
        return;

    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Stack: center");

    const BoundingBoxf3 a_bb = world_bbox_of_object(m_ordered_object_idxs.front());
    if (!a_bb.defined)
        return;
    const double target = a_bb.center()(axis);

    for (size_t i = 1; i < m_ordered_object_idxs.size(); ++i) {
        const int idx = m_ordered_object_idxs[i];
        const BoundingBoxf3 bb = world_bbox_of_object(idx);
        if (!bb.defined)
            continue;
        Vec3d delta = Vec3d::Zero();
        delta(axis) = target - bb.center()(axis);
        translate_object(idx, delta);
    }
    m_parent.do_move("");
}

void GLGizmoAlignStack::apply_all_on_bed()
{
    if (m_ordered_object_idxs.empty())
        return;
    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Stack: drop to bed");
    for (int idx : m_ordered_object_idxs) {
        const BoundingBoxf3 bb = world_bbox_of_object(idx);
        if (bb.defined)
            translate_object(idx, Vec3d(0.0, 0.0, -bb.min.z()));
    }
    m_parent.do_move("");
}

void GLGizmoAlignStack::apply_place_on_picked_face()
{
    if (!m_has_picked_face || m_ordered_object_idxs.size() < 2)
        return;
    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Stack: stack on face");
    const int b_idx = m_ordered_object_idxs[1];
    const BoundingBoxf3 bb = world_bbox_of_object(b_idx);
    if (bb.defined)
        translate_object(b_idx, Vec3d(0.0, 0.0,
                         m_picked_face_world_z + (double)m_epsilon_mm - bb.min.z()));
    m_parent.do_move("");
}

// -----------------------------------------------------------------------------
// Input window (panel)
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::on_render_input_window(float x, float y, float bottom_limit)
{
    prune_dead_objects();

    const float win_w = 336.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(),
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoCollapse);

    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);

    const size_t n = m_ordered_object_idxs.size();
    const Model* model = m_parent.get_selection().get_model();

    // --- Order chips ---------------------------------------------------------
    if (n == 0)
        ImGui::TextWrapped("%s", _u8L("Click objects in the scene to add them in order. #1 becomes the anchor; the rest move toward it.").c_str());
    else
        ImGui::TextWrapped("%s", _u8L("Click more objects to extend the order. #1 is the anchor.").c_str());

    for (size_t i = 0; i < n; ++i) {
        const int obj_idx = m_ordered_object_idxs[i];
        const int number  = (int)i + 1;
        const ImVec4 col = ImGui::ColorConvertU32ToFloat4(order_color(i));

        // Wrap chips: keep at most 3 per row.
        if (i > 0 && i % 3 != 0) ImGui::SameLine(0.0f, 4.0f);

        std::string name = (model && obj_idx < (int)model->objects.size())
                               ? model->objects[obj_idx]->name : std::string("?");
        if (name.size() > 10) name = name.substr(0, 9) + "…";

        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
        const std::string chip = "#" + std::to_string(number) + " · " + name +
                                 "##chip" + std::to_string(obj_idx);
        if (ImGui::SmallButton(chip.c_str()))
            toggle_object_order(obj_idx); // click chip = remove from order
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Click to remove from the order").c_str());
    }
    if (n > 0) {
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::SmallButton(_u8L("Reset").c_str())) {
            while (!m_ordered_object_idxs.empty())
                toggle_object_order(m_ordered_object_idxs.back());
        }
    }

    ImGui::Separator();

    // --- Mode toggle ----------------------------------------------------------
    if (ImGui::RadioButton(_u8L("Place against").c_str(), !m_flush_mode))
        m_flush_mode = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Objects come to rest touching the chosen face of #1 (chained: #2 on #1, #3 on #2...)").c_str());
    ImGui::SameLine(0.0f, 14.0f);
    if (ImGui::RadioButton(_u8L("Align flush").c_str(), m_flush_mode))
        m_flush_mode = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Same-side faces become coplanar with #1 (Illustrator-style align)").c_str());

    // --- Face buttons -----------------------------------------------------------
    const bool can_apply = n >= 2;
    m_imgui->disabled_begin(!can_apply);

    const float bs = 46.0f;
    struct FaceBtn { const char* id; CubeIcon icon; const char* label; int axis; int dir; const char* tip_touch; const char* tip_flush; };
    const FaceBtn face_btns[] = {
        { "##as_zpos", CubeIcon::TouchZPos, "Z",  2, +1,
          "Stack on top of #1 (#2 on #1, #3 on #2...), with gap", "Top faces flush with #1" },
        { "##as_xneg", CubeIcon::TouchXNeg, "X-", 0, -1,
          "Place against #1's left side", "Left faces flush with #1" },
        { "##as_xpos", CubeIcon::TouchXPos, "X+", 0, +1,
          "Place against #1's right side", "Right faces flush with #1" },
        { "##as_yneg", CubeIcon::TouchYNeg, "Y-", 1, -1,
          "Place against #1's front side", "Front faces flush with #1" },
        { "##as_ypos", CubeIcon::TouchYPos, "Y+", 1, +1,
          "Place against #1's back side", "Back faces flush with #1" },
    };
    for (size_t i = 0; i < sizeof(face_btns) / sizeof(face_btns[0]); ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 6.0f);
        const FaceBtn& b = face_btns[i];
        if (cube_icon_button(b.id, b.icon, bs, m_flush_mode, b.label)) {
            if (m_flush_mode) apply_flush(b.axis, b.dir);
            else              apply_touch(b.axis, b.dir);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L(m_flush_mode ? b.tip_flush : b.tip_touch).c_str());
    }

    // --- Center row + bed --------------------------------------------------------
    const struct { const char* id; CubeIcon icon; const char* label; int axis; const char* tip; } center_btns[] = {
        { "##as_cx", CubeIcon::CenterX, "X", 0, "Center on #1 in X" },
        { "##as_cy", CubeIcon::CenterY, "Y", 1, "Center on #1 in Y" },
        { "##as_cz", CubeIcon::CenterZ, "Z", 2, "Center on #1 in Z" },
    };
    for (size_t i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 6.0f);
        if (cube_icon_button(center_btns[i].id, center_btns[i].icon, bs, false, center_btns[i].label))
            apply_center(center_btns[i].axis);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L(center_btns[i].tip).c_str());
    }
    m_imgui->disabled_end();

    ImGui::SameLine(0.0f, 6.0f);
    m_imgui->disabled_begin(n == 0);
    if (cube_icon_button("##as_bed", CubeIcon::Bed, bs, false, nullptr))
        apply_all_on_bed();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Drop every ordered object to the bed (Z = 0)").c_str());
    m_imgui->disabled_end();

    // --- Options ------------------------------------------------------------------
    ImGui::SetNextItemWidth(90.f);
    ImGui::InputFloat(_u8L("Z gap (mm)").c_str(), &m_epsilon_mm, 0.0f, 0.0f, "%.3f");
    if (m_epsilon_mm < 0.f)   m_epsilon_mm = 0.f;
    if (m_epsilon_mm > 0.1f)  m_epsilon_mm = 0.1f;
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::Checkbox(_u8L("#1 to bed first").c_str(), &m_place_a_on_bed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Drop #1 to the bed before stacking on top of it").c_str());

    ImGui::Separator();

    // --- Stack on face ---------------------------------------------------------------
    ImGui::Text("%s", _u8L("Stack on face").c_str());
    m_imgui->disabled_begin(!can_apply);

    if (ImGui::Checkbox(_u8L("Pick a face on #1").c_str(), &m_face_pick_mode)) {
        if (m_face_pick_mode) {
            m_has_picked_face = false;
            ensure_face_raycaster_for_A();
        } else {
            clear_face_pick();
        }
        // #1's tint is suppressed while picking — refresh so it shows real
        // color on entry and regains its order color on exit.
        refresh_highlight();
    }
    if (m_has_picked_face) {
        ImGui::SameLine();
        ImGui::Text(_u8L("Z = %.3f mm").c_str(), m_picked_face_world_z);
        if (ImGui::Button(_u8L("Place B on picked face").c_str()))
            apply_place_on_picked_face();
    } else if (m_face_pick_mode) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _u8L("Click a face on #1...").c_str());
    }
    m_imgui->disabled_end();

    GizmoImguiEnd();
}

}} // namespace Slic3r::GUI
// NEOTKO_ALIGNSTACK_TAG_END
