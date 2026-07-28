// NEOTKO_ALIGNSTACK_TAG_START
#ifndef slic3r_GLGizmoAlignStack_hpp_
#define slic3r_GLGizmoAlignStack_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <array>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace Slic3r { namespace GUI {

// Align & Stack gizmo v3: relates exactly two objects, #1 (anchor) and #2
// (the one that moves) — a third click swaps out #2 rather than growing a
// chain nobody used.
//  - "Place / Align": five face buttons drawn as isometric mini-cubes (the
//    highlighted face + incoming plane tells the story) plus three center
//    buttons. Touch mode moves #2 against the anchor face; Flush mode aligns
//    same-side faces Illustrator-style.
//  - Viewport AABB aid: wireframes of #1/#2's bbox plus one translucent ghost
//    wireframe per placement op showing exactly where #2 would land, each
//    with its own clickable mini-cube icon at the predicted position — so the
//    operation can be read directly off the scene instead of decoded from an
//    abstract X-/X+ icon.
//  - "Stack on face": pick a real face on #1 in the viewport, drop #2 onto it.
// Order #1/#2 is assigned by clicking objects in the scene while the gizmo
// is open (selection is mirrored so Selection::translate keeps working).
class GLGizmoAlignStack : public GLGizmoBase
{
public:
    GLGizmoAlignStack(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent& mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    // --- Order tracking (#1 anchor, #2 the object that moves) --------------
    // Object indices (into Model::objects) in user click order, capped to two
    // (kMaxOrdered): this gizmo only ever relates one object against another.
    // Seeded from the selection when the gizmo opens; afterwards owned by it.
    std::vector<int> m_ordered_object_idxs;

    // --- Params -------------------------------------------------------------
    float m_epsilon_mm     = 0.01f;  // Z gap when touching/stacking
    bool  m_place_a_on_bed = true;   // drop A to bed before a Z+ chain
    bool  m_flush_mode     = false;  // false = Touch (contact), true = Flush

    // --- Face-pick state ----------------------------------------------------
    bool   m_face_pick_mode    = false;
    bool   m_has_picked_face   = false;
    double m_picked_face_world_z = 0.0;
    Vec3d  m_picked_face_world_pos { Vec3d::Zero() };
    Vec3d  m_picked_face_world_normal { Vec3d::UnitZ() };
    std::unique_ptr<MeshRaycaster> m_face_raycaster;
    int                             m_face_raycaster_obj_idx = -1;
    Transform3d                     m_face_raycaster_world_trafo { Transform3d::Identity() };
    // Kept alongside the raycaster so we can look up the hit facet's vertices
    // and draw the highlight region (raw_mesh() = object-space). Normals and
    // face adjacency are cached so the coplanar flood-fill is cheap on hover.
    TriangleMesh                    m_face_mesh;
    std::vector<Vec3f>              m_face_normals;
    std::vector<Vec3i32>            m_face_neighbors;

    // --- Face highlight (hover + picked) feedback --------------------------
    // Last mouse position seen while in pick mode (canvas coords), used by
    // on_render to raycast and light up the triangle under the cursor.
    Vec2d  m_hover_mouse_pos { Vec2d::Zero() };
    bool   m_have_hover_pos  = false;
    int    m_hover_facet_idx = -1;   // facet currently under the cursor (-1 none)
    int    m_picked_facet_idx = -1;  // facet chosen by the last click (-1 none)
    GLModel m_hover_face_model;      // teal overlay, rebuilt when hover changes
    int     m_hover_model_facet = -1;// facet the hover model was built for
    GLModel m_picked_face_model;     // green overlay for the confirmed face
    int     m_picked_model_facet = -1;

    // --- Scene highlight: original GLVolume colors we tinted ---------------
    // Keyed by a stable (object_idx, volume_idx, instance_idx) id so we can
    // restore by matching live volumes — safe across selection changes AND
    // volume rebuilds (no dangling pointers).
    std::map<std::tuple<int, int, int>, ColorRGBA> m_saved_colors;

    // --- Zone / ghost placement preview (viewport AABB aid) -----------------
    // Wireframe of #1's bbox ("the zone") and #2's current bbox, plus one
    // wireframe ghost per placement operation showing where #2 would land if
    // that operation were applied right now. Each GLModel is rebuilt only
    // when its source bbox actually changes (see rebuild_wire_box_if_needed).
    GLModel       m_zone_box_a;
    BoundingBoxf3 m_zone_box_a_bbox;
    GLModel       m_zone_box_b;
    BoundingBoxf3 m_zone_box_b_bbox;
    std::array<GLModel, 5>       m_ghost_face_models;   // Z+, X-, X+, Y-, Y+
    std::array<BoundingBoxf3, 5> m_ghost_face_bboxes;
    std::array<GLModel, 3>       m_ghost_center_models; // center X, Y, Z
    std::array<BoundingBoxf3, 3> m_ghost_center_bboxes;

    // Screen-space rects (ImGui coords) of the ghost icons drawn by the last
    // render pass; on_mouse hit-tests clicks against them to run the matching
    // operation. dir is ignored when is_center.
    struct GhostIconRect { float x0, y0, x1, y1; int axis; int dir; bool is_center; };
    std::vector<GhostIconRect> m_ghost_icon_rects;

    // --- Helpers ------------------------------------------------------------
    void seed_order_from_selection();
    void prune_dead_objects();
    void toggle_object_order(int object_idx);
    void render_badges();

    // Tint every GLVolume of each ordered object with its order color, after
    // restoring any previous tint. Requests a redraw.
    void apply_highlight();
    void restore_highlight();
    // Re-apply highlight and ask the canvas to repaint next frame.
    void refresh_highlight();

    int  ordered_obj(size_t i) const {
        return i < m_ordered_object_idxs.size() ? m_ordered_object_idxs[i] : -1;
    }

    BoundingBoxf3 world_bbox_of_object(int object_idx) const;

    // Translate every instance of the object by delta (selection-backed).
    void translate_object(int object_idx, const Vec3d& delta);

    // Touch: move #2 against #1's face on `axis` toward `dir` (+1/-1).
    // Z+ uses the epsilon gap; lateral contact is exact.
    void apply_touch(int axis, int dir);
    // Flush: #2's same-side face becomes coplanar with #1's (dir>0: max, else min).
    void apply_flush(int axis, int dir);
    // Center #2 on #1 along axis.
    void apply_center(int axis);
    // Every ordered object to min_z = 0.
    void apply_all_on_bed();

    // Pure math shared by apply_touch/apply_flush/apply_center AND by the
    // ghost preview (so the ghost never lies about what a click will do).
    Vec3d compute_place_delta(int axis, int dir, bool flush,
                              const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const;
    Vec3d compute_center_delta(int axis, const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const;

    // Rebuilds `model` (a 12-edge line box) only if `bb` differs from
    // `cached_bbox`, and updates `cached_bbox`. Resets `model` if bb is empty.
    void rebuild_wire_box_if_needed(GLModel& model, BoundingBoxf3& cached_bbox, const BoundingBoxf3& bb) const;
    void reset_ghost_geometry();
    // Draws #1/#2's bbox wireframes plus one translucent ghost wireframe per
    // placement op, and a floating clickable mini-cube icon at each ghost
    // (same icon as the panel) so the user can click the actual predicted
    // position instead of decoding an abstract X-/X+ button.
    void render_zone_and_ghosts();

    void apply_place_on_picked_face();
    void clear_face_pick();
    void ensure_face_raycaster_for_A();

    // Raycast under `mouse_pos` against #1 and update the hovered facet.
    void update_hover_face(const Vec2d& mouse_pos);
    // Build a GLModel for the connected coplanar region grown from `facet_idx`
    // (object space, each triangle lifted a hair along its normal to avoid
    // z-fighting) tinted with `col`. On a flat face this lights up the whole
    // face; on curved surfaces it stays a small patch around the cursor.
    void build_face_model(GLModel& model, int facet_idx, const ColorRGBA& col);
    // Render the hover/picked face overlays (flat shader, depth-tested).
    void render_face_highlights();
};

}} // namespace Slic3r::GUI

#endif
// NEOTKO_ALIGNSTACK_TAG_END
