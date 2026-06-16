// NEOTKO_ALIGNSTACK_TAG_START
#ifndef slic3r_GLGizmoAlignStack_hpp_
#define slic3r_GLGizmoAlignStack_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/Color.hpp"

#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace Slic3r { namespace GUI {

// Align & Stack gizmo v2: two visual controls.
//  - "Place / Align": six face buttons drawn as isometric mini-cubes (the
//    highlighted face + incoming plane tells the story) plus three center
//    buttons. Touch mode chains objects against the anchor face; Flush mode
//    aligns same-side faces Illustrator-style.
//  - "Stack on face": pick a real face on A in the viewport, drop B onto it.
// Order A/B/C is assigned by clicking objects in the scene while the gizmo
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
    // --- Order tracking (A, B, C, ... assigned by in-gizmo clicks) ---------
    // Object indices (into Model::objects) in user click order. Seeded from
    // the selection when the gizmo opens; afterwards owned by the gizmo.
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

    // --- Scene highlight: original GLVolume colors we tinted ---------------
    // Keyed by a stable (object_idx, volume_idx, instance_idx) id so we can
    // restore by matching live volumes — safe across selection changes AND
    // volume rebuilds (no dangling pointers).
    std::map<std::tuple<int, int, int>, ColorRGBA> m_saved_colors;

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

    // Touch: chain B against A's face on `axis` toward `dir` (+1/-1),
    // C against B, etc. Z+ uses epsilon gap; lateral contact is exact.
    void apply_touch(int axis, int dir);
    // Flush: same-side faces of B, C... coplanar with A's (dir>0: max, else min).
    void apply_flush(int axis, int dir);
    // Center every non-anchor object on A along axis.
    void apply_center(int axis);
    // Every ordered object to min_z = 0.
    void apply_all_on_bed();

    void apply_place_on_picked_face();
    void clear_face_pick();
    void ensure_face_raycaster_for_A();
};

}} // namespace Slic3r::GUI

#endif
// NEOTKO_ALIGNSTACK_TAG_END
