// NEOTKO_ALIGNSTACK_TAG_START
#ifndef slic3r_GLGizmoAlignStack_hpp_
#define slic3r_GLGizmoAlignStack_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

#include <memory>
#include <vector>

namespace Slic3r { namespace GUI {

// Align & Stack gizmo: Illustrator-style align (X/Y/Z) plus ordered Z-stack
// with epsilon gap. Targets ModelInstances (top-level objects only in v1).
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
    // --- Order tracking (A, B, C, ... by click-add order) -----------------
    // Object indices (into Model::objects) in the order the user added them
    // to the selection while the gizmo is open. We do not patch Selection;
    // we diff against the current selection on each render to stay in sync.
    std::vector<int> m_ordered_object_idxs;

    // --- Stack Z params ---------------------------------------------------
    float m_epsilon_mm        = 0.01f;  // gap between B.min_z and A.max_z
    bool  m_place_a_on_bed    = true;   // before chained stack

    // --- Align anchor -----------------------------------------------------
    // 0 = first (A), 1 = last, 2 = selection bbox center, 3 = bed center
    int m_align_anchor = 0;

    // --- Face-pick state --------------------------------------------------
    bool   m_face_pick_mode    = false;
    bool   m_has_picked_face   = false;
    double m_picked_face_world_z = 0.0;
    Vec3d  m_picked_face_world_pos { Vec3d::Zero() };
    Vec3d  m_picked_face_world_normal { Vec3d::UnitZ() };
    // Raycaster built on demand from object A's raw_mesh (instance-level).
    std::unique_ptr<MeshRaycaster> m_face_raycaster;
    int                             m_face_raycaster_obj_idx = -1;
    Transform3d                     m_face_raycaster_world_trafo { Transform3d::Identity() };

    // --- Helpers ----------------------------------------------------------
    void sync_order_with_selection();
    void render_badges();

    // Returns the object index of the i-th entry in m_ordered_object_idxs,
    // or -1 if out of range.
    int  ordered_obj(size_t i) const {
        return i < m_ordered_object_idxs.size() ? m_ordered_object_idxs[i] : -1;
    }

    // World-space AABB of all instances of a given object index.
    BoundingBoxf3 world_bbox_of_object(int object_idx) const;

    // Apply a Z translation so that the object's min_z reaches target_z.
    // Iterates all instances of the object. Caller wraps in TakeSnapshot
    // and calls do_move afterwards.
    void move_object_min_z_to(int object_idx, double target_z);

    // Align along an axis (0=X, 1=Y, 2=Z), mode: 0=min, 1=center, 2=max.
    void apply_align(int axis, int mode);

    // Stack A→B→C: each next.min_z = prev.max_z + epsilon (XY preserved).
    void apply_stack_ordered();

    // Place B (ordered[1]) so that B.min_z = picked_face_world_z + eps.
    void apply_place_on_picked_face();

    // Reset face-pick state and release raycaster.
    void clear_face_pick();

    // Build raycaster for object A if needed (instance 0's transform).
    void ensure_face_raycaster_for_A();
};

}} // namespace Slic3r::GUI

#endif
// NEOTKO_ALIGNSTACK_TAG_END
