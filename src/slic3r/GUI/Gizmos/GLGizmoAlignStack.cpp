#include "GLGizmoAlignStack.hpp"
#include "GLGizmoMeasure.hpp" // for TransformHelper::world_to_ss

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/SurfaceExtrude.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/PaintTransfer.hpp"
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include <wx/busyinfo.h>
#include <wx/progdlg.h>
#include "slic3r/GUI/GUI_ObjectList.hpp"

#include <imgui/imgui.h>

#include <GL/glew.h>
#include <wx/glcanvas.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <cmath>
#include <set>
#include <sstream>

namespace Slic3r { namespace GUI {

// =============================================================================
// Per-order colours for the chips, badges and the overlay on the scene.
// =============================================================================

namespace {

// This gizmo only ever relates one object against another (#1 anchor, #2 the
// one that moves) — an ordered chain of 3+ was never actually used, just
// confusing. Picking a third object swaps out #2 instead of extending it.
constexpr size_t kMaxOrdered = 2;

// The hovered face lights up loosely (a patch on curved surfaces),
// the picked one strictly: only what is really coplanar, which is what Construct extrudes.
constexpr float kHoverFaceCos  = 0.94f;   // ~20 degrees
constexpr float kPickedFaceCos = 0.9994f; // ~2 degrees

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

std::string format_mm(double v) { char b[32]; std::snprintf(b, sizeof b, "%.3f mm", v); return b; }

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
    return _u8L("Align & Construct");
}

bool GLGizmoAlignStack::on_is_activable() const
{
    // Usable even with an empty scene/selection: the user builds the order by clicking objects.
    return true;
}

void GLGizmoAlignStack::on_set_state()
{
    if (get_state() == On) {
        seed_order_from_selection();
        apply_highlight();
    } else if (get_state() == Off) {
        restore_highlight();
        m_ordered.clear();
        clear_face_pick();
        m_ghost_mesh.reset(); m_ghost_mesh_pick = Pick();
        for (size_t i = 0; i < 2; ++i) { m_pick_overlay[i].reset(); m_pick_overlay_pick[i] = Pick(); }
        m_hover_op = Op::None; m_ghost_visible = false;
    }
}

void GLGizmoAlignStack::data_changed(bool /*is_serializing*/)
{
    // Fires on selection changes and on volume rebuilds. apply_highlight()
    // restores via stable ids first (no dangling pointers), then re-tints —
    // so tints never accumulate and removed objects regain their color.
    prune_dead_objects();
    m_ghost_mesh_pick = Pick();  // #2's mesh may have changed; the ghost rebuilds on next hover
    if (get_state() == On)
        apply_highlight();
}

// -----------------------------------------------------------------------------
// Order tracking — owned by the gizmo, selection is mirrored so that
// Selection::translate() keeps acting on every ordered object.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::seed_order_from_selection()
{
    m_ordered.clear();
    for (const auto& kv : m_parent.get_selection().get_content()) {
        if (m_ordered.size() >= kMaxOrdered)
            break;
        Pick p;
        p.obj = kv.first;
        m_ordered.push_back(p);
    }
}

void GLGizmoAlignStack::prune_dead_objects()
{
    const Model* model = m_parent.get_selection().get_model();
    const int n_objects = model ? (int)model->objects.size() : 0;
    std::vector<Pick> alive;
    for (Pick p : m_ordered) {
        if (p.obj < 0 || p.obj >= n_objects)
            continue;
        if (p.is_part()) {
            // Follow the part by its id: deleting another part shifts the indices.
            const ModelVolumePtrs& vols = model->objects[p.obj]->volumes;
            if (p.vol >= (int)vols.size() || vols[p.vol]->id() != p.vol_id) {
                p.vol = -1;
                for (size_t i = 0; i < vols.size(); ++i)
                    if (vols[i]->id() == p.vol_id) { p.vol = (int)i; break; }
                if (p.vol < 0)
                    continue; // the part is gone
            }
        }
        alive.push_back(p);
    }
    m_ordered.swap(alive);
}

void GLGizmoAlignStack::sync_selection_of(int object_idx)
{
    Selection& sel = m_parent.get_selection();
    const bool used = std::any_of(m_ordered.begin(), m_ordered.end(), [&](const Pick& p) { return p.obj == object_idx; });
    if (used)
        sel.add_object((unsigned int)object_idx, false);
    else
        sel.remove_object((unsigned int)object_idx);
}

void GLGizmoAlignStack::remove_pick(size_t i)
{
    if (i >= m_ordered.size())
        return;
    const int obj = m_ordered[i].obj;
    m_ordered.erase(m_ordered.begin() + i);
    sync_selection_of(obj);
    // The anchor may have changed → a previously picked face on it is stale.
    if (m_face_pick_mode || m_has_picked_face)
        clear_face_pick();
    m_ghost_mesh_pick = Pick();
    refresh_highlight();
}

void GLGizmoAlignStack::clear_order()
{
    while (!m_ordered.empty())
        remove_pick(m_ordered.size() - 1);
}

// The first click on an object takes it whole; another click on it takes just the part
// that was touched (only when it has several parts); a click on a picked part lets it go. A click
// on another part of an object whose part is already picked picks that part too (#1 and #2 may be
// two parts of the same object).
void GLGizmoAlignStack::click_object(int object_idx, int vol_idx)
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size())
        return;
    const ModelObjectPtrs& objs = model->objects;
    const bool can_be_part = vol_idx >= 0 && vol_idx < (int)objs[object_idx]->volumes.size() &&
                             objs[object_idx]->volumes[vol_idx]->is_model_part() &&
                             model_part_count(object_idx) > 1;

    for (size_t i = 0; i < m_ordered.size(); ++i) {
        Pick& p = m_ordered[i];
        if (p.obj != object_idx)
            continue;
        if (!p.is_part()) {
            if (!can_be_part) { remove_pick(i); return; }
            p.vol    = vol_idx;                       // whole object → just this part, same place
            p.vol_id = objs[object_idx]->volumes[vol_idx]->id();
            if (i == 0 && (m_face_pick_mode || m_has_picked_face))
                clear_face_pick();
            m_ghost_mesh_pick = Pick();
            refresh_highlight();
            return;
        }
        if (p.vol == vol_idx || !can_be_part) { remove_pick(i); return; }
    }

    Pick np;
    np.obj = object_idx;
    // A new object is taken whole; a part of an object that already has a part picked is a part.
    const bool has_part_here = std::any_of(m_ordered.begin(), m_ordered.end(),
                                           [&](const Pick& p) { return p.obj == object_idx && p.is_part(); });
    if (has_part_here && can_be_part) {
        np.vol    = vol_idx;
        np.vol_id = objs[object_idx]->volumes[vol_idx]->id();
    }
    // Only #1 (anchor) and #2 (the one that moves) ever matter here.
    // Clicking a third one swaps out #2 rather than growing a chain.
    if (m_ordered.size() >= kMaxOrdered) {
        const int old_obj = m_ordered.back().obj;
        m_ordered.pop_back();
        sync_selection_of(old_obj);
    }
    m_ordered.push_back(np);
    sync_selection_of(object_idx);
    if (m_ordered.size() == 1 && (m_face_pick_mode || m_has_picked_face))
        clear_face_pick();
    m_ghost_mesh_pick = Pick();
    refresh_highlight();
}

// -----------------------------------------------------------------------------
// what a pick is made of
// -----------------------------------------------------------------------------

const ModelObject* GLGizmoAlignStack::pick_object(const Pick& p) const
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || p.obj < 0 || p.obj >= (int)model->objects.size())
        return nullptr;
    const ModelObject* mo = model->objects[p.obj];
    if (mo->instances.empty() || (p.is_part() && p.vol >= (int)mo->volumes.size()))
        return nullptr;
    return mo;
}

const ModelVolume* GLGizmoAlignStack::pick_volume(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    return (mo && p.is_part()) ? mo->volumes[p.vol] : nullptr;
}

Transform3d GLGizmoAlignStack::pick_trafo(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return Transform3d::Identity();
    const Transform3d inst = mo->instances.front()->get_matrix();
    return p.is_part() ? Transform3d(inst * mo->volumes[p.vol]->get_matrix()) : inst;
}

TriangleMesh GLGizmoAlignStack::pick_mesh(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return TriangleMesh();
    return p.is_part() ? mo->volumes[p.vol]->mesh() : mo->raw_mesh();
}

std::string GLGizmoAlignStack::pick_name(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return "?";
    return p.is_part() ? mo->volumes[p.vol]->name : mo->name;
}

int GLGizmoAlignStack::model_part_count(int object_idx) const
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size())
        return 0;
    int n = 0;
    for (const ModelVolume* v : model->objects[object_idx]->volumes)
        if (v->is_model_part()) ++n;
    return n;
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
        if (it != m_saved_colors.end()) {
            v->set_color(it->second.first);
            v->force_native_color = it->second.second;
        }
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
    // An object with one of its parts picked: the rest of it turns see-through, so the
    // part stands out inside the assembly.
    const float glass_a = 0.25f;
    auto part_picked_here = [&](int obj) {
        return std::any_of(m_ordered.begin(), m_ordered.end(), [&](const Pick& p) { return p.obj == obj && p.is_part(); });
    };

    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    for (GLVolume* v : volumes) {
        if (v == nullptr || v->is_modifier || v->is_wipe_tower)
            continue;

        // Order index of this volume, if it is part of the order (its whole object, or this
        // very part).
        int order = -1;
        for (size_t i = 0; i < m_ordered.size(); ++i)
            if (m_ordered[i].obj == v->object_idx() &&
                (!m_ordered[i].is_part() || m_ordered[i].vol == v->volume_idx())) { order = (int)i; break; }

        // #1 stays untinted while picking a face on it so the overlay reads
        // cleanly; every other ordered object keeps its order color.
        const bool do_tint = (order >= 0) && !(m_face_pick_mode && order == 0);
        const bool glass   = order < 0 && part_picked_here(v->object_idx());
        if (!do_tint && !ghost && !glass)
            continue; // nothing to do for this volume

        const ColorRGBA orig = v->color;
        ColorRGBA out = orig;
        if (do_tint) {
            const ColorRGBA letter = order_color_rgba((size_t)order);
            // Blend 72% toward the letter color so geometry shading still reads.
            for (int k = 0; k < 3; ++k)
                out[k] = 0.28f * orig[k] + 0.72f * letter[k];
        }
        out[3] = glass ? glass_a : (ghost ? ghost_a : orig[3]);

        m_saved_colors.emplace(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()),
                               std::make_pair(orig, (bool)v->force_native_color));
        v->set_color(out);
        // A painted volume only takes the lowered alpha with its native colour forced.
        if (out[3] < 1.f)
            v->force_native_color = true;
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

BoundingBoxf3 GLGizmoAlignStack::world_bbox_of(const Pick& p) const
{
    BoundingBoxf3 out;
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return out;
    if (!p.is_part()) {
        for (size_t i = 0; i < mo->instances.size(); ++i)
            out.merge(mo->instance_bounding_box(i));
        return out;
    }
    // A part: its own mesh, placed with every instance of its object.
    const ModelVolume* mv = mo->volumes[p.vol];
    for (const ModelInstance* inst : mo->instances)
        out.merge(mv->mesh().transformed_bounding_box(inst->get_matrix() * mv->get_matrix()));
    return out;
}

// -----------------------------------------------------------------------------
// Render — colored letter badges, no 3D handles
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::on_render()
{
    prune_dead_objects();
    render_pick_overlays();
    render_face_highlights();
    render_construct_ghost();
    render_place_ghost();
    render_badges();
}

void GLGizmoAlignStack::render_pick_overlays()
{
    GLShaderProgram* shader = nullptr;
    for (size_t i = 0; i < 2; ++i) {
        const bool show = i < m_ordered.size() && !(i == 0 && m_face_pick_mode);
        const ModelObject* mo = show ? pick_object(m_ordered[i]) : nullptr;
        if (mo == nullptr) {
            if (m_pick_overlay[i].is_initialized())
                m_pick_overlay[i].reset();
            m_pick_overlay_pick[i] = Pick();
            continue;
        }
        const Pick& p = m_ordered[i];
        if (!m_pick_overlay[i].is_initialized() || !m_pick_overlay_pick[i].same(p) ||
            m_pick_overlay_volumes[i] != mo->volumes.size()) {
            m_pick_overlay[i].reset();
            const TriangleMesh mesh = pick_mesh(p);
            m_pick_overlay_pick[i]    = p;
            m_pick_overlay_volumes[i] = mo->volumes.size();
            if (mesh.empty())
                continue;
            m_pick_overlay[i].init_from(mesh.its);
            ColorRGBA c = order_color_rgba(i);
            c.a(0.45f);
            m_pick_overlay[i].set_color(c);
        }
        if (!m_pick_overlay[i].is_initialized())
            continue;

        if (shader == nullptr) {
            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;
            shader->start_using();
            glsafe(::glEnable(GL_DEPTH_TEST));
            glsafe(::glDepthFunc(GL_LEQUAL));
            glsafe(::glDepthMask(GL_FALSE));
            glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
            glsafe(::glPolygonOffset(-1.0f, -1.0f)); // wins over the volume's own, coplanar surface
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());
        }
        shader->set_uniform("view_model_matrix", wxGetApp().plater()->get_camera().get_view_matrix() * pick_trafo(p));
        m_pick_overlay[i].render();
    }
    if (shader != nullptr) {
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDepthFunc(GL_LESS));
        shader->stop_using();
    }
}

void GLGizmoAlignStack::render_badges()
{
    if (m_ordered.empty())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Matrix4d proj_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4>& viewport = camera.get_viewport();

    ImDrawList* fg = ImGui::GetForegroundDrawList();

    for (size_t i = 0; i < m_ordered.size(); ++i) {
        const BoundingBoxf3 bb = world_bbox_of(m_ordered[i]);
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

    // Where #2 will land: a dashed line from where it is to a target mark, both drawn at
    // a fixed size on screen, so the landing spot of a 3 mm part reads as well as a big one's.
    if (m_ghost_visible) {
        auto ss = [&](const Vec3d& w) {
            const Vec2d q = TransformHelper::world_to_ss(w, proj_view, viewport);
            return ImVec2((float)q.x(), (float)(viewport[3] - q.y()));
        };
        const ImVec2 a = ss(m_ghost_from), b = ss(m_ghost_to);
        const ImU32 ghost_col = IM_COL32(40, 200, 185, 255);
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        if (len > 1.f) {
            const float ux = (b.x - a.x) / len, uy = (b.y - a.y) / len;
            for (float d = 0.f; d < len; d += 12.f) {
                const float e = std::min(d + 7.f, len);
                fg->AddLine(ImVec2(a.x + ux * d, a.y + uy * d), ImVec2(a.x + ux * e, a.y + uy * e), ghost_col, 2.5f);
            }
        }
        fg->AddCircle(b, 11.f, IM_COL32(20, 20, 20, 255), 32, 4.5f);
        fg->AddCircle(b, 11.f, ghost_col, 32, 2.5f);
        fg->AddCircleFilled(b, 3.5f, ghost_col, 16);
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
        // A quick second click also arrives as a LeftDown; the DClick after it is swallowed.
        if (mouse_event.LeftDClick())
            return true;
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

    // Click-to-order: clicking an object in the scene assigns the next number; clicking it
    // again picks just the part touched, and once more lets it go (click_object()).
    // On macOS a quick second click arrives TWICE: as a LeftDown and then as a LeftDClick
    // (seen while testing). Only the LeftDown counts; the DClick is swallowed, or one click
    // would take the part and let it go at once. The press is consumed, so Orca never starts
    // dragging the object under it.
    if (mouse_event.LeftDClick()) {
        if (m_parent.get_first_hover_volume_idx() >= 0) {
            return true;
        }
        return false;
    }
    if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
        const int hovered = m_parent.get_first_hover_volume_idx();
        if (hovered < 0)
            return false; // empty space → camera keeps working as usual
        const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
        if (hovered >= (int)volumes.size() || volumes[hovered] == nullptr)
            return false;
        const GLVolume* gv     = volumes[hovered];
        const int       obj_idx = gv->object_idx();
        const Model* model = m_parent.get_selection().get_model();
        if (!model || obj_idx < 0 || obj_idx >= (int)model->objects.size())
            return false;
        click_object(obj_idx, gv->is_modifier ? -1 : gv->volume_idx());
        return true; // consume: keep global selection stable while ordering
    }
    return false;
}

void GLGizmoAlignStack::ensure_face_raycaster_for_A()
{
    if (m_ordered.empty() || pick_object(m_ordered[0]) == nullptr) {
        clear_face_pick();
        return;
    }
    const Pick a = m_ordered[0];

    // Refresh the world transform every call so a moved/rotated #1 never leaves
    // a stale raycaster (cheap); only rebuild the mesh + raycaster when #1 itself
    // changes (the mesh is object-space — or the part's own space — so we
    // render with view * this trafo).
    m_face_raycaster_world_trafo = pick_trafo(a);
    if (m_face_raycaster && m_face_raycaster_pick.same(a))
        return;

    m_face_mesh = pick_mesh(a);
    m_face_raycaster.reset(new MeshRaycaster(m_face_mesh)); // copies internally
    m_face_raycaster_pick = a;
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
    m_face_raycaster_pick    = Pick();
    m_have_hover_pos         = false;
    m_hover_facet_idx        = -1;
    m_hover_model_facet      = -1;
    m_picked_facet_idx       = -1;
    m_picked_model_facet     = -1;
    m_hover_face_model.reset();
    m_picked_face_model.reset();
    m_construct_ghost.reset();
    m_construct_ghost_facet = -2;
    m_construct_block_reason.clear();
}

// -----------------------------------------------------------------------------
// Face highlight — light up the triangle under the cursor (and the picked one)
// so the user gets feedback that a face is about to be / has been chosen.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::build_face_model(GLModel& model, int facet_idx, const ColorRGBA& col, float cos_thresh)
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
    const Vec3f            seed_n = m_face_normals[facet_idx].normalized();
    const std::vector<int> region = SurfaceExtrude::coplanar_region(m_face_normals, m_face_neighbors,
                                                                    facet_idx, cos_thresh);

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
                             ColorRGBA(0.10f, 0.80f, 0.74f, 0.55f), kHoverFaceCos); // teal hover
        else
            m_hover_face_model.reset();
        m_hover_model_facet = m_hover_facet_idx;
    }
    // With Hollow opening by Angle, the green shows the grown face.
    const float picked_cos = m_remesh_hollow && m_remesh_open_face && m_remesh_opening == RemeshOpening::Angle
                                 ? std::cos(float(m_remesh_open_angle) * float(M_PI) / 180.f)
                                 : kPickedFaceCos;
    if (m_picked_facet_idx != m_picked_model_facet || picked_cos != m_picked_model_cos) {
        if (m_picked_facet_idx >= 0)
            build_face_model(m_picked_face_model, m_picked_facet_idx,
                             ColorRGBA(0.16f, 0.85f, 0.30f, 0.60f), picked_cos); // green picked
        else
            m_picked_face_model.reset();
        m_picked_model_facet = m_picked_facet_idx;
        m_picked_model_cos   = picked_cos;
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
// Construct: extrude the picked flat face of #1 into a solid of the chosen height (outline
// optionally grown/shrunk), added as a part of #1's object or as a separate object. Any flat
// face works, along its own normal; optionally both ways (half out, half in) and optionally
// just its holes, filled.
// -----------------------------------------------------------------------------

bool GLGizmoAlignStack::build_construct_mesh(indexed_triangle_set& out, std::string& why, bool preview,
                                             size_t* n_facets_out, double* face_z_out)
{
    out.clear();
    why.clear();
    const int n_facets = (int)m_face_mesh.its.indices.size();
    if (!m_has_picked_face || m_picked_facet_idx < 0 || m_picked_facet_idx >= n_facets ||
        (int)m_face_normals.size() != n_facets || (int)m_face_neighbors.size() != n_facets) {
        why = _u8L("Pick a face on #1 first.");
        return false;
    }
    if (m_construct_height <= 0.f) {
        why = _u8L("Height must be above 0.");
        return false;
    }

    // Normal of the picked facet in world space (inverse transpose: survives scaling).
    const Matrix3d lin     = m_face_raycaster_world_trafo.linear();
    const Vec3d    world_n = (lin.inverse().transpose() *
                              m_face_normals[m_picked_facet_idx].cast<double>()).normalized();
    if (!world_n.allFinite()) {
        why = _u8L("That face has no usable outline.");
        return false;
    }

    // Any face: turn the world so the face looks straight up (+Z), extrude there exactly as
    // a top face, and turn the solid back. `to_flat` is a pure rotation, so nothing is distorted.
    const Eigen::Quaterniond q_flat = Eigen::Quaterniond::FromTwoVectors(world_n, Vec3d::UnitZ());
    Transform3d to_flat = Transform3d::Identity();
    to_flat.linear()    = q_flat.toRotationMatrix();

    const std::vector<int> region = SurfaceExtrude::coplanar_region(m_face_normals, m_face_neighbors,
                                                                    m_picked_facet_idx, kPickedFaceCos);
    double face_z = 0.;
    ExPolygons outline = SurfaceExtrude::region_outline(m_face_mesh.its, region,
                                                        to_flat * m_face_raycaster_world_trafo, &face_z);
    if (outline.empty()) {
        why = _u8L("That face has no usable outline.");
        return false;
    }
    if (m_construct_holes) {
        // The holes of the face, each one filled: e.g. the circle of a screw, ready to
        // carve a bigger hole with a boolean (Offset grows them).
        outline = SurfaceExtrude::outline_holes(outline);
        if (outline.empty()) {
            why = _u8L("That face has no holes.");
            return false;
        }
    }

    // Outwards from the face, or half outwards and half inwards.
    const double h  = (double)m_construct_height;
    const double z0 = m_construct_both ? face_z - 0.5 * h : face_z;
    out = SurfaceExtrude::extrude_outline(outline, (double)m_construct_offset, z0, h);
    if (out.indices.empty()) {
        why = _u8L("With that offset nothing is left of the shape.");
        return false;
    }
    its_transform(out, to_flat.inverse());
    if (n_facets_out) *n_facets_out = region.size();
    if (face_z_out)   *face_z_out   = face_z;
    return true;
}

void GLGizmoAlignStack::update_construct_ghost()
{
    if (!m_has_picked_face) {
        if (m_construct_ghost.is_initialized())
            m_construct_ghost.reset();
        m_construct_ghost_facet = -2;
        m_construct_block_reason.clear();
        return;
    }
    if (m_construct_ghost_facet == m_picked_facet_idx &&
        m_construct_ghost_height == m_construct_height &&
        m_construct_ghost_offset == m_construct_offset &&
        m_construct_ghost_both   == m_construct_both &&
        m_construct_ghost_holes  == m_construct_holes &&
        m_construct_ghost_trafo.isApprox(m_face_raycaster_world_trafo))
        return;

    m_construct_ghost_facet  = m_picked_facet_idx;
    m_construct_ghost_height = m_construct_height;
    m_construct_ghost_offset = m_construct_offset;
    m_construct_ghost_both   = m_construct_both;
    m_construct_ghost_holes  = m_construct_holes;
    m_construct_ghost_trafo  = m_face_raycaster_world_trafo;

    m_construct_ghost.reset();
    indexed_triangle_set its;
    if (build_construct_mesh(its, m_construct_block_reason, true)) {
        m_construct_ghost.init_from(its);
        m_construct_ghost.set_color(ColorRGBA(1.0f, 0.55f, 0.0f, 0.55f));
    }
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoAlignStack::render_construct_ghost()
{
    if (!m_has_picked_face)
        return;
    update_construct_ghost();
    if (!m_construct_ghost.is_initialized())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // The sheet is already in world coordinates.
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_construct_ghost.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoAlignStack::apply_construct()
{
    const int a_idx = ordered_obj(0);
    if (a_idx < 0)
        return;

    indexed_triangle_set its;
    std::string          why;
    if (!build_construct_mesh(its, why, false))
        return;

    const bool        as_object = m_construct_as_object;
    const std::string name      = _u8L("Extruded surface");
    {
        Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: extrude surface");
        TriangleMesh mesh(std::move(its));
        if (as_object)
            wxGetApp().obj_list()->load_mesh_object(mesh, from_u8(name), false);
        else
            wxGetApp().obj_list()->add_mesh_part_in_place(a_idx, std::move(mesh), name);
    }

    // #1's mesh may have just grown a part: the cached raycaster and the pick are stale.
    clear_face_pick();
    refresh_highlight();
}

// Boolean of #1 with #2. Orca's own boolean only works between the parts of ONE object, which
// forces an "assemble" first; here both meshes are taken where they sit in the scene. Two parts
// of the same object give a part of that object, in place; anything else gives a new object.
void GLGizmoAlignStack::apply_boolean(const std::string& op, const std::string& suffix)
{
    m_bool_error.clear();
    prune_dead_objects();
    if (m_ordered.size() < 2)
        return;
    const Pick pa = m_ordered[0];
    const Pick pb = m_ordered[1];
    Model& model = wxGetApp().plater()->model();
    const ModelObject* a = pick_object(pa);
    const ModelObject* b = pick_object(pb);
    if (a == nullptr || b == nullptr || pa.same(pb))
        return;
    // A whole object and one of its own parts overlap by definition: nothing sensible.
    if (pa.obj == pb.obj && (!pa.is_part() || !pb.is_part())) {
        m_bool_error = _u8L("One of them is inside the other. Pick two different parts, or two objects.");
        return;
    }
    // Two parts of the same object: the result stays a part of that object, in place.
    const bool in_place = pa.obj == pb.obj;

    TriangleMesh mesh_a = pick_mesh(pa);
    mesh_a.transform(pick_trafo(pa));
    TriangleMesh mesh_b = pick_mesh(pb);
    mesh_b.transform(pick_trafo(pb));

    std::vector<TriangleMesh> results;
    MeshBoolean::mcut::make_boolean(mesh_a, mesh_b, results, op);
    if (results.empty() || results.front().empty()) {
        m_bool_error = _u8L("The operation gave no result. The two objects may not overlap, or one of them is not a closed solid.");
        return;
    }

    const std::string name     = pick_name(pa) + " - " + suffix;
    const int         extruder = pa.is_part() ? PaintTransfer::effective_filament(*pick_volume(pa))
                                              : (a->config.has("extruder") ? a->config.opt_int("extruder") : 0);
    const bool        keep     = m_bool_keep_inputs;
    // Where the result sits before it is placed: load_mesh_object() centres the mesh on its
    // bounding box (and may drop the object to the bed afterwards).
    const Vec3d                        result_centre = results.front().bounding_box().center();
    std::vector<PaintTransfer::Source> paint_sources;
    // A whole object gives all its parts; a part gives just itself.
    auto add_sources = [&](const Pick& p, const ModelObject& mo, bool cutter) {
        if (!p.is_part()) {
            PaintTransfer::append_object_sources(mo, cutter, paint_sources);
            return;
        }
        PaintTransfer::Source src;
        src.volume   = mo.volumes[p.vol];
        src.to_world = pick_trafo(p);
        src.filament = PaintTransfer::effective_filament(*src.volume);
        src.cutter   = cutter;
        paint_sources.push_back(src);
    };
    add_sources(pa, *a, false);
    add_sources(pb, *b, op == "A_NOT_B"); // the cut walls stay unpainted
    // What to delete afterwards, by pointer: indices move while the model changes.
    const ModelObject* del_obj[2] = { a, b };
    const ModelVolume* del_vol[2] = { pick_volume(pa), pick_volume(pb) };
    PaintTransfer::Stats paint_stats;
    // Keeping the painting can take a few seconds on big meshes. Shown (and painted, with a yield)
    // before the model changes, so the yield never draws it half done.
    std::unique_ptr<wxBusyInfo> busy;
    if (PaintTransfer::worth_transferring(paint_sources)) {
        busy = std::make_unique<wxBusyInfo>(_L("Keeping the painting..."), m_parent.get_wxglcanvas());
        wxSafeYield(nullptr, true);
    }
    {
        Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: boolean");
        // Leave the order (and the mirrored selection) empty before the model changes.
        clear_order();

        ModelVolume* result     = nullptr;
        int          result_obj = -1;
        Transform3d  result_to_world = Transform3d::Identity();
        if (in_place) {
            result_obj = pa.obj;
            result     = wxGetApp().obj_list()->add_mesh_part_in_place(result_obj, results.front(), name);
            if (result != nullptr) {
                if (extruder > 0)
                    result->config.set_key_value("extruder", new ConfigOptionInt(extruder));
                result_to_world = model.objects[result_obj]->instances.front()->get_matrix() * result->get_matrix();
            }
        } else {
            wxGetApp().obj_list()->load_mesh_object(results.front(), from_u8(name), false);
            if (extruder > 0 && !model.objects.empty())
                model.objects.back()->config.set_key_value("extruder", new ConfigOptionInt(extruder));
            if (!model.objects.empty() && !model.objects.back()->volumes.empty()) {
                result_obj      = int(model.objects.size()) - 1;
                result          = model.objects.back()->volumes.front();
                result_to_world = Geometry::translation_transform(result_centre) * result->get_matrix();
            }
        }
        // The result keeps the painting of #1 and #2, and the filament each one had. Before
        // deleting the inputs: the sources point into them.
        if (result != nullptr) {
            paint_stats = PaintTransfer::transfer(paint_sources, *result, result_to_world,
                                                  PaintTransfer::effective_filament(*result));
            if (paint_stats.layers > 0)
                wxGetApp().obj_list()->update_info_items(size_t(result_obj));
        }

        if (!keep) {
            auto obj_index = [&](const ModelObject* mo) {
                for (size_t i = 0; i < model.objects.size(); ++i)
                    if (model.objects[i] == mo) return int(i);
                return -1;
            };
            // Parts first (deleting a part never moves an object), the higher index first so the
            // other keeps its place; then whole objects, the higher index first as well.
            std::vector<std::pair<int, int>> parts;
            for (int k = 0; k < 2; ++k) {
                if (del_vol[k] == nullptr)
                    continue;
                const int oi = obj_index(del_obj[k]);
                if (oi < 0)
                    continue;
                const ModelVolumePtrs& vols = model.objects[oi]->volumes;
                for (size_t vi = 0; vi < vols.size(); ++vi)
                    if (vols[vi] == del_vol[k]) { parts.emplace_back(oi, int(vi)); break; }
            }
            std::sort(parts.begin(), parts.end(), [](const auto& l, const auto& r) { return l.second > r.second; });
            for (const auto& pv : parts)
                wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itVolume, pv.first, pv.second);
            std::vector<int> objs;
            for (int k = 0; k < 2; ++k)
                if (del_vol[k] == nullptr) {
                    const int oi = obj_index(del_obj[k]);
                    if (oi >= 0) objs.push_back(oi);
                }
            std::sort(objs.rbegin(), objs.rend());
            for (int oi : objs)
                wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itObject, oi, -1);
        }
    }

    refresh_highlight();
}

// -----------------------------------------------------------------------------
// Transform application — #1 is always the anchor; #2 moves toward it.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::translate_pick(const Pick& p, const Vec3d& delta)
{
    if (delta.norm() < 1e-9)
        return;
    const ModelObject* mo = pick_object(p);
    if (mo == nullptr)
        return;
    Selection& sel = m_parent.get_selection();
    if (!p.is_part()) {
        for (size_t i = 0; i < mo->instances.size(); ++i)
            sel.translate((unsigned int)p.obj, (unsigned int)i, delta);
        m_parent.do_move("");
        return;
    }
    // A part moves inside its object: its offset lives in the instance's space, so the world
    // move is taken back through the first instance (one ModelVolume serves every instance). The
    // scene volumes are moved directly and do_move() in Volume mode writes them into the model,
    // exactly as when a part is moved with Orca's own Move.
    const Vec3d local = mo->instances.front()->get_matrix().linear().inverse() * delta;
    for (GLVolume* v : m_parent.get_volumes().volumes)
        if (v != nullptr && v->object_idx() == p.obj && v->volume_idx() == p.vol)
            v->set_volume_offset(v->get_volume_offset() + local);
    const Selection::EMode old_mode = sel.get_mode();
    sel.set_mode(Selection::Volume);
    m_parent.do_move("");
    sel.set_mode(old_mode);
}

Vec3d GLGizmoAlignStack::compute_place_delta(int axis, int dir, bool flush,
                                             const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const
{
    Vec3d delta = Vec3d::Zero();
    if (!a_bb.defined || !b_bb.defined)
        return delta;
    if (flush) {
        const double target = (dir > 0) ? a_bb.max(axis) : a_bb.min(axis);
        delta(axis) = target - ((dir > 0) ? b_bb.max(axis) : b_bb.min(axis));
    } else {
        // Z contact gets the epsilon gap (slicing sanity); lateral contact is exact.
        const double gap = (axis == 2) ? (double)m_epsilon_mm : 0.0;
        delta(axis) = (dir > 0) ? (a_bb.max(axis) + gap - b_bb.min(axis))
                                : (a_bb.min(axis) - gap - b_bb.max(axis));
    }
    return delta;
}

Vec3d GLGizmoAlignStack::compute_center_delta(int axis, const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const
{
    Vec3d delta = Vec3d::Zero();
    if (a_bb.defined && b_bb.defined)
        delta(axis) = a_bb.center()(axis) - b_bb.center()(axis);
    return delta;
}

// Every way of placing #2 goes through here, for the click AND for the ghost, so the
// ghost cannot lie about what the click will do.
bool GLGizmoAlignStack::op_delta(Op op, Vec3d& delta) const
{
    delta = Vec3d::Zero();
    if (op == Op::None || m_ordered.size() < 2)
        return false;
    const BoundingBoxf3 a = world_bbox_of(m_ordered[0]);
    const BoundingBoxf3 b = world_bbox_of(m_ordered[1]);
    if (!a.defined || !b.defined)
        return false;
    switch (op) {
    case Op::XNeg:     delta = compute_place_delta(0, -1, m_flush_mode, a, b); break;
    case Op::XPos:     delta = compute_place_delta(0, +1, m_flush_mode, a, b); break;
    case Op::YNeg:     delta = compute_place_delta(1, -1, m_flush_mode, a, b); break;
    case Op::YPos:     delta = compute_place_delta(1, +1, m_flush_mode, a, b); break;
    case Op::OnTop:    delta = compute_place_delta(2, +1, m_flush_mode, a, b); break;
    case Op::CenterXY: delta = compute_center_delta(0, a, b) + compute_center_delta(1, a, b); break;
    case Op::CenterZ:  delta = compute_center_delta(2, a, b); break;
    case Op::Bed:      delta = Vec3d(0.0, 0.0, -b.min.z()); break;
    case Op::Face:
        if (!m_has_picked_face) return false;
        delta = Vec3d(0.0, 0.0, m_picked_face_world_z + (double)m_epsilon_mm - b.min.z());
        break;
    default: return false;
    }
    return true;
}

void GLGizmoAlignStack::apply_op(Op op)
{
    Vec3d delta;
    if (!op_delta(op, delta))
        return;
    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: place");
    translate_pick(m_ordered[1], delta);
}

void GLGizmoAlignStack::swap_order()
{
    if (m_ordered.size() < 2)
        return;
    std::swap(m_ordered[0], m_ordered[1]);
    if (m_face_pick_mode || m_has_picked_face)
        clear_face_pick();          // the picked face belonged to the old #1
    m_ghost_mesh_pick = Pick();
    refresh_highlight();
}

// -----------------------------------------------------------------------------
// Placement ghost. Only ONE at a time: the one for the button under the mouse. It is
// #2's real mesh, translucent, drawn where the click would leave it.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::render_place_ghost()
{
    m_ghost_visible = false;
    Vec3d delta;
    if (!op_delta(m_hover_op, delta))
        return;
    const Pick         pb = m_ordered[1];
    const ModelObject* mo = pick_object(pb);
    if (mo == nullptr)
        return;

    if (!m_ghost_mesh.is_initialized() || !m_ghost_mesh_pick.same(pb) || m_ghost_mesh_volumes != mo->volumes.size()) {
        m_ghost_mesh.reset();
        const TriangleMesh mesh = pick_mesh(pb);          // object space, or the part's own
        if (mesh.empty())
            return;
        m_ghost_mesh.init_from(mesh.its);
        m_ghost_mesh.set_color(ColorRGBA(0.18f, 0.84f, 0.77f, 0.50f));   // the panel's teal
        m_ghost_mesh_pick    = pb;
        m_ghost_mesh_volumes = mo->volumes.size();
    }

    const BoundingBoxf3 bb = world_bbox_of(pb);
    m_ghost_from    = bb.center();
    m_ghost_to      = bb.center() + delta;
    m_ghost_visible = true;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d placed = Transform3d::Identity();
    placed.translate(delta);
    placed = placed * pick_trafo(pb);
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * placed);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_ghost_mesh.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoAlignStack::on_render_input_window(float x, float y, float bottom_limit)
{
    prune_dead_objects();

    const float win_w = 22.f * ImGui::GetFontSize();
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.f), ImVec2(win_w, FLT_MAX));
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::PushTextWrapPos(win_w - 16.f);
    render_panel_body(); // its own function: no early exit in there can skip the pops below
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
}

// Remesh (voxels). #1 is voxelized whole and rebuilt as one closed
// solid: holes, open shells and flipped faces are gone (Orca's repair is Windows only). Offset moves
// the surface; Hollow takes the inside away, Wall under the new surface, and with a picked face
// opens it through that face (a mouth: the face shrunk by the wall, from 2 walls deep to 1 mm out).
void GLGizmoAlignStack::apply_remesh()
{
    if (m_remesh_running)
        return;
    m_remesh_error.clear();
    prune_dead_objects();
    if (m_ordered.empty())
        return;
    const Pick pa = m_ordered[0];
    Model& model = wxGetApp().plater()->model();
    const ModelObject* a = pick_object(pa);
    if (a == nullptr)
        return;
    const bool   hollow = m_remesh_hollow;
    const double wall   = (double)m_hollow_wall;
    const double offset = (double)m_remesh_offset;
    const RemeshOut out = hollow ? m_remesh_out
                                 : (m_remesh_out == RemeshOut::NegativePart ? RemeshOut::Replace : m_remesh_out);
    if (hollow && wall <= offset) {
        m_remesh_error = _u8L("The wall must be thicker than the offset.");
        return;
    }

    // The mouth, built like Construct builds its sheet: face turned to +Z, prism, turned back.
    indexed_triangle_set mouth;     // deep, clipped to the solid Wall mm in
    indexed_triangle_set mouth_top; // short, from the face to just past one wall: goes through the face
    const int n_face = (int)m_face_mesh.its.indices.size();
    if (hollow && m_remesh_open_face && m_has_picked_face && m_picked_facet_idx >= 0 && m_picked_facet_idx < n_face &&
        (int)m_face_normals.size() == n_face && (int)m_face_neighbors.size() == n_face) {
        const Matrix3d lin     = m_face_raycaster_world_trafo.linear();
        const Vec3d    world_n = (lin.inverse().transpose() * m_face_normals[m_picked_facet_idx].cast<double>()).normalized();
        if (!world_n.allFinite()) {
            m_remesh_error = _u8L("That face has no usable outline.");
            return;
        }
        Transform3d to_flat = Transform3d::Identity();
        to_flat.linear()    = Eigen::Quaterniond::FromTwoVectors(world_n, Vec3d::UnitZ()).toRotationMatrix();
        // The opening's outline, flat: the picked face (Face), the face grown to the neighbours
        // within Angle (Angle, for curved skins), or a circle of Diameter round the click (Circle).
        // zlo/zhi: how far the skin under it goes down and up, so the plug starts above all of it.
        ExPolygons outline;
        double     zlo = 0., zhi = 0., inset = -wall;
        if (m_remesh_opening == RemeshOpening::Circle) {
            const Vec3d  c = to_flat * m_picked_face_world_pos;
            const double r = 0.5 * (double)m_remesh_hole_d;
            Polygon      circle;
            for (int i = 0; i < 64; ++i) {
                const double a = 2. * M_PI * i / 64.;
                circle.points.emplace_back(scale_(c.x() + r * std::cos(a)), scale_(c.y() + r * std::sin(a)));
            }
            outline.emplace_back(std::move(circle));
            // The skin round the click can curve away by up to the radius; the hole is the circle itself.
            zlo   = c.z() - r;
            zhi   = c.z() + r;
            inset = 0.;
        } else {
            const float cos_t = m_remesh_opening == RemeshOpening::Angle ? std::cos(float(m_remesh_open_angle) * float(M_PI) / 180.f)
                                                                         : kPickedFaceCos;
            const std::vector<int> region = SurfaceExtrude::coplanar_region(m_face_normals, m_face_neighbors,
                                                                            m_picked_facet_idx, cos_t);
            const Transform3d flat = to_flat * m_face_raycaster_world_trafo;
            double mean_z = 0.;
            outline = SurfaceExtrude::region_outline(m_face_mesh.its, region, flat, &mean_z);
            zlo = zhi = mean_z;
            for (int f : region)
                for (int k = 0; k < 3; ++k) {
                    const double z = (flat * m_face_mesh.its.vertices[m_face_mesh.its.indices[f][k]].cast<double>()).z();
                    zlo = std::min(zlo, z);
                    zhi = std::max(zhi, z);
                }
        }
        // Deep enough to reach the inside however much Smooth sinks it; the mouth is clipped to the
        // solid Wall mm in later, so going deep never thins a wall where the outside curves in.
        const double depth = 2. * wall + 2. * (double)m_hollow_smooth + std::abs(offset) + 1.;
        const double top   = wall + 2. * (double)m_remesh_voxel + 0.5 + std::abs(offset);
        if (!outline.empty()) {
            mouth     = SurfaceExtrude::extrude_outline(outline, inset, zlo - depth, zhi - zlo + depth + 1.);
            mouth_top = SurfaceExtrude::extrude_outline(outline, inset, zlo - top, zhi - zlo + top + 1.);
        }
        if (mouth.indices.empty()) {
            m_remesh_error = _u8L("The face is too small for that wall.");
            return;
        }
        its_transform(mouth, to_flat.inverse());
        its_transform(mouth_top, to_flat.inverse());
    }

    TriangleMesh mesh_a = pick_mesh(pa);
    mesh_a.transform(pick_trafo(pa));

    const size_t mouth_tris = mouth.indices.size();
    auto fail = [this](const std::string& msg) { m_remesh_error = msg; };

    // Hollow, opening and subtraction all happen on the voxels: no mesh boolean.
    sla::RemeshParams rp;
    rp.voxel      = (double)m_remesh_voxel;
    rp.offset     = offset;
    rp.hollow     = hollow;
    rp.wall       = wall;
    rp.smooth     = (double)m_hollow_smooth;
    rp.want_outer = out != RemeshOut::NegativePart;
    rp.adaptivity = (double)m_remesh_adaptive;
    rp.fast_smooth = m_remesh_fast_smooth;
    if (mouth_tris > 0) {
        rp.mouth = &mouth;
        rp.plug  = &mouth_top;
    }

    // The heavy part runs on its own thread; this one keeps a progress window with Cancel. One
    // OpenVDB step can't be stopped halfway, so Cancel takes effect at the next step.
    // The window lists every step of this run: done ones ticked, the current one marked, the
    // rest waiting, so it shows how much is left (the bar only says it is alive).
    struct Step { const char* key; std::string label; };
    std::vector<Step> steps { { "voxels", _u8L("Voxelizing") } };
    if (hollow) {
        steps.push_back({ "inside", _u8L("Shrinking the inside") });
        steps.push_back({ "smooth", _u8L("Smoothing the inside") });
        if (mouth_tris > 0)
            steps.push_back({ "opening", _u8L("Opening the face") });
    }
    steps.push_back({ "mesh", _u8L("Building the mesh") });
    steps.push_back({ "simplify", _u8L("Simplifying") });
    std::atomic<bool> cancel { false }, done { false };
    std::atomic<int>  cur_step { 0 };
    auto set_step = [&](const char* key) {
        for (int i = 0; i < int(steps.size()); ++i)
            if (std::strcmp(steps[i].key, key) == 0 && i > cur_step.load())
                cur_step = i;
    };
    rp.cancel = [&cancel]() { return cancel.load(); };
    rp.status = [&](int, const char* what) { set_step(what); };
    auto step_list = [&](int cur) {
        wxString msg;
        for (int i = 0; i < int(steps.size()); ++i) {
            msg += i < cur ? wxString::FromUTF8("\u2713  ") : i == cur ? wxString::FromUTF8("\u25B8  ") : wxString("     ");
            msg += from_u8(steps[i].label);
            if (i == cur)
                msg += cancel.load() ? wxString(" - ") + _L("stopping...") : wxString("...");
            msg += "\n";
        }
        return msg;
    };
    const uint32_t     max_tris  = uint32_t(std::max(1000, m_remesh_max_tris));
    const bool         drop_bits = m_remesh_drop_bits;
    const double       voxel3    = rp.voxel * rp.voxel * rp.voxel;
    sla::RemeshResult  rr;
    size_t             bits_dropped = 0;
    bool               worker_failed = false;
    m_remesh_running = true;
    std::thread worker([&]() {
        try {
            rr = sla::neo_remesh(mesh_a.its, rp);
            indexed_triangle_set& keep = rp.want_outer ? rr.outer : rr.cavity;
            if (!rr.cancelled && keep.indices.size() > max_tris) {
                // Simplified to Max triangles; flat zones go first, so the shape stays.
                set_step("simplify");
                its_quadric_edge_collapse(keep, max_tris, nullptr,
                                          [&cancel]() { if (cancel.load()) throw std::runtime_error("cancelled"); });
            }
            // Loose bits: shells far smaller than the biggest one (leftovers of hidden geometry).
            // The inside of a closed hollow is a big shell of its own and stays.
            if (!rr.cancelled && drop_bits && !keep.indices.empty()) {
                std::vector<indexed_triangle_set> shells = its_split(keep);
                if (shells.size() > 1) {
                    double biggest = 0.;
                    std::vector<double> vols(shells.size());
                    for (size_t i = 0; i < shells.size(); ++i)
                        biggest = std::max(biggest, vols[i] = std::abs(double(its_volume(shells[i]))));
                    const double min_vol = std::max(2e-3 * biggest, 8. * voxel3);
                    indexed_triangle_set kept;
                    for (size_t i = 0; i < shells.size(); ++i) {
                        if (vols[i] < min_vol) { ++bits_dropped; continue; }
                        its_merge(kept, shells[i]);
                    }
                    if (bits_dropped > 0 && !kept.indices.empty())
                        keep = std::move(kept);
                    else
                        bits_dropped = 0;
                }
            }
        } catch (const std::exception&) {
            if (cancel.load())
                rr.cancelled = true;
            else
                worker_failed = true;
        }
        done = true;
    });
    {
        // Built with the whole list so the window is tall enough for it. The bar pulses: it shows
        // the work is alive, the list shows what is left.
        wxProgressDialog dlg(_L("Remesh"), step_list(0), 100, m_parent.get_wxglcanvas()->GetParent(),
                             wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
        while (!done.load()) {
            if (!dlg.Pulse(step_list(cur_step.load())))
                cancel = true;
            wxMilliSleep(100);
        }
    }
    worker.join();
    m_remesh_running = false;
    if (rr.cancelled) {
        m_remesh_error = _u8L("Cancelled.");
        return;
    }
    if (worker_failed)
        return fail(_u8L("The remesh failed. Try a coarser quality."));
    if (hollow && !rr.has_cavity)
        return fail(_u8L("Nothing is left inside with that wall. Try a thinner wall."));
    if (rp.want_outer && rr.outer.indices.empty())
        return fail(_u8L("Nothing came out. Try a finer quality or a smaller negative offset."));
    if (!rp.want_outer && rr.cavity.indices.empty())
        return fail(_u8L("Nothing is left inside with that wall. Try a thinner wall."));

    // Negative part: #1 and its painting untouched, the slicer takes the inside away.
    if (out == RemeshOut::NegativePart) {
        const std::string neg_name = pick_name(pa) + " - " + _u8L("hollow");
        {
            Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: hollow (negative part)");
            const int obj = pa.obj;
            clear_face_pick();
            clear_order();
            wxGetApp().obj_list()->add_mesh_part_in_place(obj, TriangleMesh(std::move(rr.cavity)), neg_name, true);
        }
        refresh_highlight();
        return;
    }

    TriangleMesh result(std::move(rr.outer));

    // Into the scene, with #1's painting.
    const std::string name     = pick_name(pa) + " - " + (hollow ? _u8L("hollow") : _u8L("remesh"));
    const int         extruder = pa.is_part() ? PaintTransfer::effective_filament(*pick_volume(pa))
                                              : (a->config.has("extruder") ? a->config.opt_int("extruder") : 0);
    const Vec3d       result_centre = result.bounding_box().center();
    std::vector<PaintTransfer::Source> paint_sources;
    if (m_remesh_keep_paint) {
        if (pa.is_part()) {
            PaintTransfer::Source src;
            src.volume   = a->volumes[pa.vol];
            src.to_world = pick_trafo(pa);
            src.filament = PaintTransfer::effective_filament(*src.volume);
            paint_sources.push_back(src);
        } else
            PaintTransfer::append_object_sources(*a, false, paint_sources);
    }
    const bool         replace = out == RemeshOut::Replace;
    const ModelObject* del_obj = a;
    const ModelVolume* del_vol = pick_volume(pa);
    {
        Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: remesh");
        clear_face_pick();
        clear_order();

        ModelVolume* res_vol    = nullptr;
        int          result_obj = -1;
        Transform3d  result_to_world = Transform3d::Identity();
        if (pa.is_part() && replace) {
            result_obj = pa.obj;
            res_vol    = wxGetApp().obj_list()->add_mesh_part_in_place(result_obj, result, name);
            if (res_vol != nullptr) {
                if (extruder > 0)
                    res_vol->config.set_key_value("extruder", new ConfigOptionInt(extruder));
                result_to_world = model.objects[result_obj]->instances.front()->get_matrix() * res_vol->get_matrix();
            }
        } else {
            wxGetApp().obj_list()->load_mesh_object(result, from_u8(name), false);
            if (extruder > 0 && !model.objects.empty())
                model.objects.back()->config.set_key_value("extruder", new ConfigOptionInt(extruder));
            if (!model.objects.empty() && !model.objects.back()->volumes.empty()) {
                result_obj      = int(model.objects.size()) - 1;
                res_vol         = model.objects.back()->volumes.front();
                result_to_world = Geometry::translation_transform(result_centre) * res_vol->get_matrix();
            }
        }
        if (res_vol != nullptr && PaintTransfer::worth_transferring(paint_sources)) {
            // The rebuilt skin lies up to about a voxel off the old one (more with an offset).
            const float reach = 1.5f * m_remesh_voxel + std::abs(float(offset)) + 0.05f;
            const PaintTransfer::Stats paint_stats = PaintTransfer::transfer(paint_sources, *res_vol, result_to_world,
                                                                             PaintTransfer::effective_filament(*res_vol), reach);
            if (paint_stats.layers > 0)
                wxGetApp().obj_list()->update_info_items(size_t(result_obj));
        }

        // Replace: delete #1 (by pointer: indices may have moved).
        if (replace)
            for (size_t oi = 0; oi < model.objects.size(); ++oi) {
                if (model.objects[oi] != del_obj)
                    continue;
                if (del_vol == nullptr) {
                    wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itObject, int(oi), -1);
                } else {
                    const ModelVolumePtrs& vols = model.objects[oi]->volumes;
                    for (size_t vi = 0; vi < vols.size(); ++vi)
                        if (vols[vi] == del_vol) { wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itVolume, int(oi), int(vi)); break; }
                }
                break;
            }
    }

    refresh_highlight();
}

// The panel in plain ImGui, like Orca's own gizmos.
void GLGizmoAlignStack::render_panel_body()
{
    const size_t n   = m_ordered.size();
    const bool   two = n >= 2;
    Op hovered = Op::None;      // the placing button under the mouse this frame, if any

    auto tip  = [](const std::string& text) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str()); };
    auto hint = [](const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    };
    auto section = [](const std::string& text) {
        ImGui::Spacing();
        ImGui::TextUnformatted(text.c_str());
        ImGui::Separator();
    };
    // A placing button: applies `op` on click and shows #2's ghost while hovered.
    auto place_button = [&](const std::string& label, Op op, const std::string& text, float w) {
        if (ImGui::Button(label.c_str(), ImVec2(w, 0.f)))
            apply_op(op);
        if (ImGui::IsItemHovered()) {
            hovered = op;
            ImGui::SetTooltip("%s", text.c_str());
        }
    };

    // --- #1 and #2 ----------------------------------------------------------------
    for (size_t i = 0; i < n; ++i) {
        const Pick& pk = m_ordered[i];
        if (i > 0) ImGui::SameLine();
        std::string name = pick_name(pk);
        if (name.size() > 12) name = name.substr(0, 11) + "...";
        // A part shows a dot before its name, so it reads apart from a whole object.
        const std::string label = "#" + std::to_string((int)i + 1) + (pk.is_part() ? "  · " : "  ") + name + "##chip" + std::to_string(i);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(order_color(i)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.08f, 1.f));
        const bool pressed = ImGui::Button(label.c_str());
        ImGui::PopStyleColor(2);
        const std::string what = pk.is_part() ? _u8L("Just this part of its object.") : _u8L("The whole object.");
        tip((i == 0 ? _u8L("#1 stays where it is.") : _u8L("#2 is the one that moves.")) + "\n" + what + "\n" + _u8L("Click to let it go."));
        if (pressed) {
            remove_pick(i);
            break;
        }
    }
    if (two) {
        ImGui::SameLine();
        if (ImGui::Button(_u8L("Swap").c_str()))
            swap_order();
        tip(_u8L("#1 becomes #2 and #2 becomes #1."));
    }
    if (n > 0) {
        ImGui::SameLine();
        if (ImGui::Button(_u8L("Reset").c_str()))
            clear_order();
        tip(_u8L("Let both go and start again."));
    }
    if (n == 0)
        hint(_u8L("Click an object: it becomes #1, the one that stays. Then click another: #2, the one that moves. Click an object made of several parts again to take just the part you touch."));
    else if (n == 1)
        hint(_u8L("Click a second object to place it against #1, or pick a face of #1 below. Click #1 again to take just the part you touch."));

    // --- Place #2 ---------------------------------------------------------------------
    section(_u8L("Place #2"));
    m_imgui->disabled_begin(!two);
    {
        if (ImGui::RadioButton(_u8L("Next to").c_str(), !m_flush_mode)) m_flush_mode = false;
        tip(_u8L("#2 ends up beside #1, touching it,\non the side you choose."));
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Align edge").c_str(), m_flush_mode)) m_flush_mode = true;
        tip(_u8L("#2 moves until its edge on the side you choose\nlines up with the same edge of #1."));

        const std::string how = m_flush_mode ? _u8L("Lines up this edge of #2 with #1's.") : _u8L("Puts #2 on this side of #1, touching it.");
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float bw  = (ImGui::GetContentRegionAvail().x - 4.f * gap) / 5.f;
        place_button("X-", Op::XNeg, how, bw);                ImGui::SameLine();
        place_button("X+", Op::XPos, how, bw);                ImGui::SameLine();
        place_button("Y-", Op::YNeg, how, bw);                ImGui::SameLine();
        place_button("Y+", Op::YPos, how, bw);                ImGui::SameLine();
        place_button(_u8L("Centre") + "##as_cxy", Op::CenterXY, _u8L("Centres #2 on #1, seen from above."), bw);

        const float hw = (ImGui::GetContentRegionAvail().x - 2.f * gap) / 3.f;
        place_button(m_flush_mode ? _u8L("Align tops") : _u8L("On top"), Op::OnTop,
                     m_flush_mode ? _u8L("#2 moves up or down until its top\nis level with #1's.")
                                  : _u8L("#2 goes up until it rests on top of #1,\nleaving the Z gap."), hw);
        ImGui::SameLine();
        place_button(_u8L("Centre Z"), Op::CenterZ, _u8L("Centres #2 on #1 in height."), hw);
        ImGui::SameLine();
        place_button(_u8L("To bed"), Op::Bed, _u8L("#2 drops to the bed."), hw);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Z gap").c_str());
        tip(_u8L("Air left between the two when one\nis put on top of the other, in mm."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(5.f * ImGui::GetFontSize());
        ImGui::InputFloat("##as_zgap", &m_epsilon_mm, 0.0f, 0.0f, "%.3f mm");
        if (m_epsilon_mm < 0.f)   m_epsilon_mm = 0.f;
        if (m_epsilon_mm > 0.1f)  m_epsilon_mm = 0.1f;
    }
    m_imgui->disabled_end();

    // --- A face of #1: drop #2 on it, or extrude from it ------------------------------
    section(m_has_picked_face ? _u8L("Face of #1") + "   Z " + format_mm(m_picked_face_world_z) : _u8L("Face of #1"));
    m_imgui->disabled_begin(n < 1);
    bool pick_mode = m_face_pick_mode;
    if (ImGui::Checkbox(_u8L("Pick a face").c_str(), &pick_mode)) {
        m_face_pick_mode = pick_mode;
        if (m_face_pick_mode) {
            m_has_picked_face = false;
            ensure_face_raycaster_for_A();
        } else {
            clear_face_pick();
        }
        // #1's tint is suppressed while picking: refresh so it shows its real colour on entry
        // and gets its order colour back on exit.
        refresh_highlight();
    }
    tip(_u8L("Then click a face of #1 in the scene.\nA flat face lights up whole."));
    m_imgui->disabled_end();
    if (m_has_picked_face) {
        m_imgui->disabled_begin(!two);
        place_button(_u8L("Drop #2 on it"), Op::Face, _u8L("#2 goes up or down until it rests\nat the height of that face."), 0.f);
        m_imgui->disabled_end();
    } else if (m_face_pick_mode)
        hint(_u8L("Click a face of #1 in the scene."));

    // --- Extrude from that face -------------------------------------------------------
    section(_u8L("Extrude from the face"));
    if (!m_has_picked_face) {
        hint(_u8L("Pick a flat face first."));
    } else {
        // The face itself, or only its holes (filled): e.g. the circle of a screw, to carve a
        // bigger hole with a boolean.
        if (ImGui::RadioButton(_u8L("Face").c_str(), !m_construct_holes)) m_construct_holes = false;
        tip(_u8L("Extrudes the face as it is."));
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Holes").c_str(), m_construct_holes)) m_construct_holes = true;
        tip(_u8L("Extrudes only the holes of the face, filled.\nPick the flat face around a screw hole and\nraise Offset to make a bigger hole with\na Difference."));

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Height").c_str());
        tip(_u8L("How thick the new solid is, in mm.\nIt grows along the face's own direction."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(5.f * ImGui::GetFontSize());
        ImGui::InputFloat("##as_h", &m_construct_height, 0.0f, 0.0f, "%.3f");
        if (m_construct_height < 0.01f)  m_construct_height = 0.01f;
        if (m_construct_height > 100.f)  m_construct_height = 100.f;
        ImGui::SameLine();
        ImGui::TextUnformatted(_u8L("Offset").c_str());
        tip(_u8L("Grows the outline of the face, in mm.\nNegative values shrink it."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(5.f * ImGui::GetFontSize());
        ImGui::InputFloat("##as_off", &m_construct_offset, 0.0f, 0.0f, "%.3f");
        if (m_construct_offset < -100.f) m_construct_offset = -100.f;
        if (m_construct_offset > 100.f)  m_construct_offset = 100.f;

        // Both ways: half outwards, half into the object, so it goes through without moving it.
        ImGui::Checkbox(_u8L("Both directions").c_str(), &m_construct_both);
        tip(_u8L("Off: the solid grows out of the face.\nOn: half the height grows out and half\ngoes into the object, so it passes through\nthe face (handy for booleans)."));

        if (ImGui::RadioButton(_u8L("Part").c_str(), !m_construct_as_object)) m_construct_as_object = false;
        tip(_u8L("A new part of #1's object, in place.\nIt shares that object's layer height."));
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Object").c_str(), m_construct_as_object)) m_construct_as_object = true;
        tip(_u8L("A separate object, with its own layer height."));

        update_construct_ghost();
        const bool can_create = m_construct_block_reason.empty();
        ImGui::SameLine();
        m_imgui->disabled_begin(!can_create);
        if (ImGui::Button(_u8L("Create").c_str()))
            apply_construct();
        m_imgui->disabled_end();
        if (!can_create) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.2f, 1.f));
            ImGui::TextWrapped("%s", m_construct_block_reason.c_str());
            ImGui::PopStyleColor();
        }
    }

    // --- Remesh (voxels) ---------------------------------------------------------------
    section(_u8L("Remesh (voxels)"));
    if (n == 0) {
        hint(_u8L("Pick #1 first."));
    } else {
        const float fw   = 5.f * ImGui::GetFontSize();
        auto        warn = [](const std::string& text) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.2f, 1.f));
            ImGui::TextWrapped("%s", text.c_str());
            ImGui::PopStyleColor();
        };
        // Surface of #1, for the estimate (only when #1 changes).
        {
            const Pick& p0  = m_ordered[0];
            const void* key = pick_volume(p0) != nullptr ? (const void*)pick_volume(p0) : (const void*)pick_object(p0);
            if (key != m_remesh_area_key && key != nullptr) {
                TriangleMesh mw = pick_mesh(p0);
                mw.transform(pick_trafo(p0));
                double area = 0.;
                for (const stl_triangle_vertex_indices& f : mw.its.indices) {
                    const Vec3d v0 = mw.its.vertices[f[0]].cast<double>();
                    area += 0.5 * (mw.its.vertices[f[1]].cast<double>() - v0).cross(mw.its.vertices[f[2]].cast<double>() - v0).norm();
                }
                m_remesh_area     = area;
                m_remesh_area_key = key;
            }
        }

        // Quality: presets + the number.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Quality").c_str());
        tip(_u8L("Voxel size, in mm. Smaller keeps more detail\nbut takes longer and needs more memory.\nDetails thinner than one voxel are lost,\nsharp edges round off by half a voxel."));
        {
            struct Q { std::string label; float v; };
            const Q qs[] = { { _u8L("Draft"), 0.5f }, { _u8L("Normal"), 0.2f }, { _u8L("High"), 0.1f }, { _u8L("Extreme"), 0.05f } };
            for (int i = 0; i < 4; ++i) {
                ImGui::SameLine();
                if (ImGui::RadioButton(qs[i].label.c_str(), std::abs(m_remesh_voxel - qs[i].v) < 1e-4f))
                    m_remesh_voxel = qs[i].v;
                char mm[16];
                snprintf(mm, sizeof(mm), "%.2f mm", qs[i].v);
                tip(qs[i].label + ": " + mm + (i == 3 ? "\n" + _u8L("Heavy: big objects take minutes\nand gigabytes of memory.") : std::string()));
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Voxel").c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw);
        ImGui::InputFloat("##rq_voxel", &m_remesh_voxel, 0.0f, 0.0f, "%.3f mm");
        tip(_u8L("Any size you like, from 0.02 to 2 mm."));
        m_remesh_voxel = std::clamp(m_remesh_voxel, 0.02f, 2.f);
        {
            // Estimate: the surface in voxels, about 3 triangles each before simplifying; the
            // grid keeps about 6 voxels across the surface.
            const double vx2   = double(m_remesh_voxel) * double(m_remesh_voxel);
            const double cells = m_remesh_area / vx2;
            const double mb    = cells * 220. / (1024. * 1024.);
            char buf[160];
            snprintf(buf, sizeof(buf), "~%.1f M %s, ~%.0f MB", 3. * cells / 1e6, _u8L("triangles").c_str(), mb);
            ImGui::SameLine();
            hint(buf);
            if (mb > 1500.)
                warn(_u8L("Very heavy: this can take minutes and a lot of memory."));
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Offset").c_str());
        tip(_u8L("Moves the whole surface, in mm.\n+ makes it fatter (thin walls get printable),\n- makes it thinner (room for a fit).\n0 just repairs. Big offsets blur the painting."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw);
        ImGui::InputFloat("##rq_offset", &m_remesh_offset, 0.0f, 0.0f, "%.2f mm");
        m_remesh_offset = std::clamp(m_remesh_offset, -10.f, 10.f);

        ImGui::Checkbox(_u8L("Hollow").c_str(), &m_remesh_hollow);
        tip(_u8L("Takes the inside away, leaving walls\nof Wall mm under the new surface."));
        if (m_remesh_hollow) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("Wall").c_str());
            tip(_u8L("How thick the walls stay, in mm.\nThe inside comes out smoothed and the\nwall is approximate. Parts thinner than\ntwo walls stay solid."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(fw);
            ImGui::InputFloat("##as_wall", &m_hollow_wall, 0.0f, 0.0f, "%.2f mm");
            m_hollow_wall = std::clamp(m_hollow_wall, 0.4f, 50.f);
            ImGui::SameLine();
            ImGui::TextUnformatted(_u8L("Smooth").c_str());
            tip(_u8L("Smooths the inside, in mm.\n0 follows every bump of the outside.\nHigher fills bumps and thin spots,\nso the wall never gets thinner than Wall."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(fw);
            ImGui::InputFloat("##as_smooth", &m_hollow_smooth, 0.0f, 0.0f, "%.1f mm");
            m_hollow_smooth = std::clamp(m_hollow_smooth, 0.f, 30.f);

            m_imgui->disabled_begin(!m_has_picked_face);
            ImGui::Checkbox(_u8L("Open through the picked face").c_str(), &m_remesh_open_face);
            tip(_u8L("Opens the hollow through the picked face,\nleaving a rim of one wall.\nOff: a closed hollow inside."));
            if (m_has_picked_face && m_remesh_open_face) {
                if (ImGui::RadioButton(_u8L("Face").c_str(), m_remesh_opening == RemeshOpening::Face)) m_remesh_opening = RemeshOpening::Face;
                tip(_u8L("The flat face you picked, less one wall\nall round (a rim stays)."));
                ImGui::SameLine();
                if (ImGui::RadioButton(_u8L("Angle").c_str(), m_remesh_opening == RemeshOpening::Angle)) m_remesh_opening = RemeshOpening::Angle;
                tip(_u8L("The face grown to its neighbours while\nthey turn less than Angle: for curved skins.\nThe green shows what it takes.\nToo much may come out odd."));
                ImGui::SameLine();
                if (ImGui::RadioButton(_u8L("Circle").c_str(), m_remesh_opening == RemeshOpening::Circle)) m_remesh_opening = RemeshOpening::Circle;
                tip(_u8L("A round hole of Diameter where you clicked,\nstraight into the surface. Works anywhere,\nlike a drain hole."));
                if (m_remesh_opening == RemeshOpening::Angle) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(_u8L("Angle").c_str());
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(fw);
                    ImGui::InputFloat("##rq_ang", &m_remesh_open_angle, 0.0f, 0.0f, "%.0f deg");
                    tip(_u8L("How much the surface may turn and\nstill be part of the opening."));
                    m_remesh_open_angle = std::clamp(m_remesh_open_angle, 1.f, 80.f);
                } else if (m_remesh_opening == RemeshOpening::Circle) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(_u8L("Diameter").c_str());
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(fw);
                    ImGui::InputFloat("##rq_hole", &m_remesh_hole_d, 0.0f, 0.0f, "%.1f mm");
                    tip(_u8L("Size of the round hole, in mm."));
                    m_remesh_hole_d = std::clamp(m_remesh_hole_d, 1.f, 200.f);
                }
            }
            m_imgui->disabled_end();
            if (!m_has_picked_face || !m_remesh_open_face)
                hint(m_has_picked_face ? _u8L("Closed hollow: sealed inside, it looks the same from outside.")
                                       : _u8L("Closed hollow: sealed inside. Pick a flat face to open it there."));
            ImGui::Checkbox(_u8L("Fast smooth").c_str(), &m_remesh_fast_smooth);
            tip(_u8L("Builds the inside on coarser voxels\n(about Smooth / 10). It is smoothed anyway,\nso it looks the same, and a big Smooth\ntakes seconds instead of minutes.\nThe outside keeps the chosen Quality."));
        }

        ImGui::Checkbox(_u8L("Keep painting").c_str(), &m_remesh_keep_paint);
        tip(_u8L("Paints the new mesh like #1.\nBorders between colours follow the\nnew triangles."));

        // Fine tuning, folded: the defaults work.
        if (ImGui::TreeNode(_u8L("Advanced").c_str())) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("Max triangles").c_str());
            tip(_u8L("The result is simplified down to this.\nMore keeps finer detail, files get bigger."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(1.2f * fw);
            ImGui::InputInt("##rq_max", &m_remesh_max_tris, 0, 0);
            m_remesh_max_tris = std::clamp(m_remesh_max_tris, 1000, 20000000);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("Adaptive").c_str());
            tip(_u8L("Bigger triangles where the surface is flat,\nsmall ones only on curves and details.\n0 makes them all the voxel size.\nFewer triangles slice faster.\nToo high softens fine detail."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(fw);
            ImGui::InputFloat("##rq_adapt", &m_remesh_adaptive, 0.0f, 0.0f, "%.2f");
            m_remesh_adaptive = std::clamp(m_remesh_adaptive, 0.f, 1.f);
            ImGui::Checkbox(_u8L("Remove loose bits").c_str(), &m_remesh_drop_bits);
            tip(_u8L("Deletes tiny separate pieces left over\nfrom hidden geometry inside the model,\nso the result is one clean solid.\nThe inside of a closed hollow stays."));
            ImGui::TreePop();
        }

        if (!m_remesh_hollow && m_remesh_out == RemeshOut::NegativePart)
            m_remesh_out = RemeshOut::Replace;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Result").c_str());
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Replace").c_str(), m_remesh_out == RemeshOut::Replace)) m_remesh_out = RemeshOut::Replace;
        tip(_u8L("The new mesh takes #1's place."));
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("New object").c_str(), m_remesh_out == RemeshOut::NewObject)) m_remesh_out = RemeshOut::NewObject;
        tip(_u8L("The new mesh as a separate object.\n#1 stays as it was."));
        ImGui::SameLine();
        m_imgui->disabled_begin(!m_remesh_hollow);
        if (ImGui::RadioButton(_u8L("Neg. part").c_str(), m_remesh_out == RemeshOut::NegativePart)) m_remesh_out = RemeshOut::NegativePart;
        m_imgui->disabled_end();
        tip(_u8L("Hollow without touching #1: the inside\nbecomes a negative part of #1's object and\nthe slicer takes it away. Move it, cut it,\nor delete it to undo. Needs Hollow."));

        m_imgui->disabled_begin(m_remesh_running);
        if (ImGui::Button(_u8L("Remesh").c_str()))
            apply_remesh();
        m_imgui->disabled_end();
        tip(_u8L("Rebuilds #1 from voxels as one closed solid:\nholes, loose shells and flipped faces are gone.\nBig meshes take a while."));
        if (!m_remesh_error.empty())
            warn(m_remesh_error);
    }

    // --- Boolean -----------------------------------------------------------------------
    section(_u8L("Boolean: #1 with #2"));
    m_imgui->disabled_begin(!two);
    {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float bw  = (ImGui::GetContentRegionAvail().x - 2.f * gap) / 3.f;
        if (ImGui::Button(_u8L("Union").c_str(), ImVec2(bw, 0.f)))
            apply_boolean("UNION", "union");
        tip(_u8L("#1 and #2 fused into one solid."));
        ImGui::SameLine();
        if (ImGui::Button(_u8L("Difference").c_str(), ImVec2(bw, 0.f)))
            apply_boolean("A_NOT_B", "difference");
        tip(_u8L("#1 with the shape of #2 carved out of it."));
        ImGui::SameLine();
        if (ImGui::Button(_u8L("Intersection").c_str(), ImVec2(bw, 0.f)))
            apply_boolean("INTERSECTION", "intersection");
        tip(_u8L("Only the volume that #1 and #2 share."));
        ImGui::Checkbox(_u8L("Keep #1 and #2").c_str(), &m_bool_keep_inputs);
        tip(_u8L("Off: the result replaces both.\nOn: the result is added and both stay."));
    }
    m_imgui->disabled_end();
    if (!m_bool_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.2f, 1.f));
        ImGui::TextWrapped("%s", m_bool_error.c_str());
        ImGui::PopStyleColor();
    }

    // The ghost follows the button under the mouse: tell the scene when that changes.
    if (hovered != m_hover_op) {
        m_hover_op = hovered;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
}

}} // namespace Slic3r::GUI
