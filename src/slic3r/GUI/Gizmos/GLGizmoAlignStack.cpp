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

#include <algorithm>
#include <array>
#include <set>

namespace Slic3r { namespace GUI {

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
    // Activable as soon as something is selected; the panel guards heavier ops.
    return !m_parent.get_selection().is_empty();
}

void GLGizmoAlignStack::on_set_state()
{
    if (get_state() == Off) {
        m_ordered_object_idxs.clear();
        clear_face_pick();
    }
}

void GLGizmoAlignStack::data_changed(bool /*is_serializing*/)
{
    // Selection changed externally — keep our ordered list in sync.
    sync_order_with_selection();
}

// -----------------------------------------------------------------------------
// Order tracking
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::sync_order_with_selection()
{
    const Selection& sel = m_parent.get_selection();

    // Collect currently selected object idxs (top-level instances only).
    std::set<int> cur_set;
    for (const auto& kv : sel.get_content())
        cur_set.insert(kv.first);

    // Drop entries no longer selected.
    m_ordered_object_idxs.erase(
        std::remove_if(m_ordered_object_idxs.begin(), m_ordered_object_idxs.end(),
                       [&](int idx) { return cur_set.find(idx) == cur_set.end(); }),
        m_ordered_object_idxs.end());

    // Append newly selected ones, preserving insertion order from the set
    // iterator (best effort — true click order is not retrievable from
    // std::set, but stable enough for typical shift+click flows).
    for (int idx : cur_set) {
        if (std::find(m_ordered_object_idxs.begin(), m_ordered_object_idxs.end(), idx)
            == m_ordered_object_idxs.end())
            m_ordered_object_idxs.push_back(idx);
    }
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
// Render — badges only, no 3D handles
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::on_render()
{
    sync_order_with_selection();
    render_badges();
}

void GLGizmoAlignStack::render_badges()
{
    if (m_ordered_object_idxs.empty())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Matrix4d proj_view = (camera.get_projection_matrix() * camera.get_view_matrix()).matrix();
    const std::array<int, 4>& viewport = camera.get_viewport();

    for (size_t i = 0; i < m_ordered_object_idxs.size(); ++i) {
        const int obj_idx = m_ordered_object_idxs[i];
        const BoundingBoxf3 bb = world_bbox_of_object(obj_idx);
        if (!bb.defined)
            continue;

        // Anchor the badge slightly above the top of the bbox.
        Vec3d anchor = bb.center();
        anchor.z()   = bb.max.z() + 2.0;

        const Vec2d ss = TransformHelper::world_to_ss(anchor, proj_view, viewport);

        // ImGui Y is inverted vs OpenGL viewport.
        ImGui::SetNextWindowPos(ImVec2((float)ss.x(),
                                       (float)(viewport[3] - ss.y())),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));

        const char label_letter = (char)('A' + (int)std::min<size_t>(i, 25));
        std::string win_id = std::string("##alignstack_badge_") + std::to_string(obj_idx);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.95f, 0.75f, 0.10f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        ImGui::Begin(win_id.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text("%c", label_letter);
        ImGui::End();
        ImGui::PopStyleColor(2);
    }
}

// -----------------------------------------------------------------------------
// Mouse — face-pick mode only
// -----------------------------------------------------------------------------

bool GLGizmoAlignStack::on_mouse(const wxMouseEvent& mouse_event)
{
    if (!m_face_pick_mode)
        return false;

    if (mouse_event.LeftDown()) {
        // We pick on object A (first ordered).
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
            // Stay in pick mode so user can re-pick; explicit toggle to exit.
            return true;
        }
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
    if (m_face_raycaster && m_face_raycaster_obj_idx == a_idx)
        return;

    const Model* model = m_parent.get_selection().get_model();
    if (!model || a_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[a_idx];
    if (mo->instances.empty())
        return;

    // raw_mesh is the merged mesh of all MODEL_PART volumes in object coords.
    TriangleMesh mesh = mo->raw_mesh();
    m_face_raycaster.reset(new MeshRaycaster(std::move(mesh)));
    m_face_raycaster_obj_idx     = a_idx;
    m_face_raycaster_world_trafo = mo->instances.front()->get_matrix();
}

void GLGizmoAlignStack::clear_face_pick()
{
    m_face_pick_mode         = false;
    m_has_picked_face        = false;
    m_face_raycaster.reset();
    m_face_raycaster_obj_idx = -1;
}

// -----------------------------------------------------------------------------
// Transform application
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::move_object_min_z_to(int object_idx, double target_z)
{
    const BoundingBoxf3 bb = world_bbox_of_object(object_idx);
    if (!bb.defined)
        return;
    const double dz = target_z - bb.min.z();
    if (std::abs(dz) < 1e-9)
        return;

    Selection& sel = m_parent.get_selection();
    const Model* model = sel.get_model();
    if (!model || object_idx >= (int)model->objects.size())
        return;
    const ModelObject* mo = model->objects[object_idx];
    for (size_t i = 0; i < mo->instances.size(); ++i)
        sel.translate((unsigned int)object_idx, (unsigned int)i, Vec3d(0.0, 0.0, dz));
}

void GLGizmoAlignStack::apply_align(int axis, int mode)
{
    if (m_ordered_object_idxs.size() < 2)
        return;

    // Compute anchor coordinate on `axis` from the chosen anchor.
    auto bbox_coord = [&](const BoundingBoxf3& bb)->double {
        if (mode == 0) return bb.min(axis);
        if (mode == 2) return bb.max(axis);
        return bb.center()(axis);
    };

    double target = 0.0;
    switch (m_align_anchor) {
    case 0: target = bbox_coord(world_bbox_of_object(m_ordered_object_idxs.front())); break;
    case 1: target = bbox_coord(world_bbox_of_object(m_ordered_object_idxs.back()));  break;
    case 2: {
        BoundingBoxf3 union_bb;
        for (int idx : m_ordered_object_idxs)
            union_bb.merge(world_bbox_of_object(idx));
        target = bbox_coord(union_bb);
        break;
    }
    case 3: {
        // Bed anchor: in v1, only meaningful on Z (target = 0). For X/Y fall
        // back to selection center to avoid coupling to bed-shape internals.
        if (axis == 2) {
            target = 0.0;
        } else {
            BoundingBoxf3 union_bb;
            for (int idx : m_ordered_object_idxs)
                union_bb.merge(world_bbox_of_object(idx));
            target = bbox_coord(union_bb);
        }
        break;
    }
    }

    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align objects");
    Selection& sel = m_parent.get_selection();
    const Model* model = sel.get_model();
    if (!model) return;

    for (int obj_idx : m_ordered_object_idxs) {
        if (obj_idx < 0 || obj_idx >= (int)model->objects.size()) continue;
        const ModelObject* mo = model->objects[obj_idx];
        const BoundingBoxf3 bb = world_bbox_of_object(obj_idx);
        if (!bb.defined) continue;

        double cur = bbox_coord(bb);
        double d   = target - cur;
        if (std::abs(d) < 1e-9) continue;

        Vec3d disp = Vec3d::Zero();
        disp(axis) = d;
        for (size_t i = 0; i < mo->instances.size(); ++i)
            sel.translate((unsigned int)obj_idx, (unsigned int)i, disp);
    }
    m_parent.do_move(""); // snapshot already taken
}

void GLGizmoAlignStack::apply_stack_ordered()
{
    if (m_ordered_object_idxs.size() < 2)
        return;

    Plater::TakeSnapshot snap(wxGetApp().plater(), "Stack objects");

    // Optionally place A on the bed first.
    if (m_place_a_on_bed)
        move_object_min_z_to(m_ordered_object_idxs.front(), 0.0);

    // Recompute bbox of A after the move.
    double prev_max_z = world_bbox_of_object(m_ordered_object_idxs.front()).max.z();
    for (size_t i = 1; i < m_ordered_object_idxs.size(); ++i) {
        const int idx = m_ordered_object_idxs[i];
        move_object_min_z_to(idx, prev_max_z + (double)m_epsilon_mm);
        prev_max_z = world_bbox_of_object(idx).max.z();
    }
    m_parent.do_move("");
}

void GLGizmoAlignStack::apply_place_on_picked_face()
{
    if (!m_has_picked_face || m_ordered_object_idxs.size() < 2)
        return;
    Plater::TakeSnapshot snap(wxGetApp().plater(), "Stack on face");
    move_object_min_z_to(m_ordered_object_idxs[1],
                         m_picked_face_world_z + (double)m_epsilon_mm);
    m_parent.do_move("");
}

// -----------------------------------------------------------------------------
// Input window (panel)
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::on_render_input_window(float x, float y, float bottom_limit)
{
    sync_order_with_selection();

    const float win_w = 320.0f;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin(on_get_name(),
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoCollapse);

    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);

    // --- Selection summary --------------------------------------------------
    const size_t n = m_ordered_object_idxs.size();
    ImGui::TextDisabled(_u8L("Selected: %zu object(s)").c_str(), n);
    if (n == 0)
        ImGui::TextWrapped("%s", _u8L("Shift+click objects in the scene to add them in order A, B, C…").c_str());

    ImGui::Separator();

    // --- Align XY/Z ---------------------------------------------------------
    ImGui::Text("%s", _u8L("Align").c_str());
    static const std::string s_anchor_first  = _u8L("First (A)");
    static const std::string s_anchor_last   = _u8L("Last");
    static const std::string s_anchor_center = _u8L("Selection center");
    static const std::string s_anchor_bed    = _u8L("Bed");
    const char* anchors[] = {
        s_anchor_first.c_str(),
        s_anchor_last.c_str(),
        s_anchor_center.c_str(),
        s_anchor_bed.c_str()
    };
    ImGui::SetNextItemWidth(160.f);
    ImGui::Combo("##alignstack_anchor", &m_align_anchor, anchors, IM_ARRAYSIZE(anchors));

    const bool can_align = n >= 2;
    m_imgui->disabled_begin(!can_align);

    const std::string s_min    = _u8L("Min");
    const std::string s_center = _u8L("Center");
    const std::string s_max    = _u8L("Max");
    auto align_row = [&](const char* axis_label, int axis) {
        ImGui::Text("%s", axis_label);
        ImGui::SameLine();
        if (ImGui::Button((s_min    + "##" + axis_label).c_str())) apply_align(axis, 0);
        ImGui::SameLine();
        if (ImGui::Button((s_center + "##" + axis_label).c_str())) apply_align(axis, 1);
        ImGui::SameLine();
        if (ImGui::Button((s_max    + "##" + axis_label).c_str())) apply_align(axis, 2);
    };
    align_row("X", 0);
    align_row("Y", 1);
    align_row("Z", 2);

    m_imgui->disabled_end();

    ImGui::Separator();

    // --- Stack Z -----------------------------------------------------------
    ImGui::Text("%s", _u8L("Stack Z").c_str());
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputFloat(_u8L("Epsilon (mm)").c_str(), &m_epsilon_mm, 0.001f, 0.01f, "%.4f");
    if (m_epsilon_mm < 0.f) m_epsilon_mm = 0.f;
    if (m_epsilon_mm > 0.1f) m_epsilon_mm = 0.1f;

    ImGui::Checkbox(_u8L("Place A on bed first").c_str(), &m_place_a_on_bed);

    m_imgui->disabled_begin(!can_align);
    if (ImGui::Button(_u8L("Stack ordered (A->B->C...)").c_str()))
        apply_stack_ordered();
    if (n == 2) {
        ImGui::SameLine();
        if (ImGui::Button(_u8L("B on top of A").c_str()))
            apply_stack_ordered();
    }
    m_imgui->disabled_end();

    ImGui::Separator();

    // --- Stack on Face -----------------------------------------------------
    ImGui::Text("%s", _u8L("Stack on Face").c_str());
    m_imgui->disabled_begin(!can_align);

    if (ImGui::Checkbox(_u8L("Pick face on A").c_str(), &m_face_pick_mode)) {
        if (m_face_pick_mode) {
            m_has_picked_face = false;
            ensure_face_raycaster_for_A();
        } else {
            clear_face_pick();
        }
    }
    if (m_has_picked_face) {
        ImGui::Text(_u8L("Picked Z = %.3f mm").c_str(), m_picked_face_world_z);
        if (ImGui::Button(_u8L("Place B on picked face").c_str()))
            apply_place_on_picked_face();
    } else if (m_face_pick_mode) {
        ImGui::TextDisabled("%s", _u8L("Click a face on object A...").c_str());
    }
    m_imgui->disabled_end();

    ImGui::Separator();

    // --- Utilities ---------------------------------------------------------
    if (ImGui::Button(_u8L("Place all on bed").c_str())) {
        Plater::TakeSnapshot snap(wxGetApp().plater(), "Place on bed");
        for (int idx : m_ordered_object_idxs)
            move_object_min_z_to(idx, 0.0);
        m_parent.do_move("");
    }
    ImGui::SameLine();
    if (ImGui::Button(_u8L("Clear order").c_str()))
        m_ordered_object_idxs.clear();

    GizmoImguiEnd();
}

}} // namespace Slic3r::GUI
// NEOTKO_ALIGNSTACK_TAG_END
