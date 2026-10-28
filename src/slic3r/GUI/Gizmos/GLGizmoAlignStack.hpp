#ifndef slic3r_GLGizmoAlignStack_hpp_
#define slic3r_GLGizmoAlignStack_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ObjectID.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {
class ModelObject;
class ModelVolume;
namespace GUI {

// Align & Construct: relates two things, #1 (the anchor) and #2 (the one that moves). Each one is
// a whole object, or one part of an object made of several parts.
//  - Place #2 against #1: next to it, on top, centred, or with an edge lined up. Hovering a
//    button shows #2 as a ghost where it would land.
//  - Pick a face of #1: drop #2 on it, or extrude it into a new solid (any flat face, along its
//    own normal; optionally both ways, or just its holes).
//  - Boolean of #1 with #2, keeping the painting of both.
// #1/#2 are assigned by clicking in the scene while the gizmo is open (the selection is
// mirrored so Selection::translate keeps working).
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
    void        render_panel_body();   // the panel itself, apart so no early exit skips the Pops
    void        on_set_state() override;

private:
    // --- Order tracking (#1 anchor, #2 the one that moves) -----------------
    // each one is a whole object or ONE part of an object of several parts (an assembly):
    // the first click on an object takes it whole, a second click on it takes just the part
    // touched. Capped to two (kMaxOrdered). Seeded from the selection when the gizmo opens.
    struct Pick {
        int      obj    = -1;
        int      vol    = -1; // index into ModelObject::volumes, -1 = the whole object
        ObjectID vol_id;      // the part's id, so a part that changes index is followed
        bool is_part() const { return vol >= 0; }
        bool same(const Pick& o) const { return obj == o.obj && vol == o.vol; }
    };
    std::vector<Pick> m_ordered;

    // --- Params -------------------------------------------------------------
    float m_epsilon_mm     = 0.01f;  // Z gap when touching/stacking
    bool  m_flush_mode     = false;  // false = Touch (contact), true = Flush

    // --- Face-pick state ----------------------------------------------------
    bool   m_face_pick_mode    = false;
    bool   m_has_picked_face   = false;
    double m_picked_face_world_z = 0.0;
    Vec3d  m_picked_face_world_pos { Vec3d::Zero() };
    Vec3d  m_picked_face_world_normal { Vec3d::UnitZ() };
    std::unique_ptr<MeshRaycaster> m_face_raycaster;
    Pick                            m_face_raycaster_pick;  // what the raycaster was built for
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
    float   m_picked_model_cos   = 0.f;   // the grow angle the picked overlay was built with

    // --- Scene highlight: original GLVolume colors we tinted ---------------
    // Keyed by a stable (object_idx, volume_idx, instance_idx) id so we can
    // restore by matching live volumes — safe across selection changes AND
    // volume rebuilds (no dangling pointers).
    // also whether the volume drew its own colour (force_native_color), which is what lets a
    // PAINTED volume turn see-through (Orca's set_selected_visible() does the same).
    std::map<std::tuple<int, int, int>, std::pair<ColorRGBA, bool>> m_saved_colors;

    // A layer of the order colour drawn over each pick. The tint above does not show on
    // painted volumes (they draw with their paint colours), this does.
    GLModel m_pick_overlay[2];
    Pick    m_pick_overlay_pick[2];
    size_t  m_pick_overlay_volumes[2] = { 0, 0 };
    void render_pick_overlays();

    // --- Placement  ---------------------------------------------------
    // Every way of placing #2. The pad is the bed seen from above, fixed to the bed axes.
    enum class Op { None, XNeg, XPos, YNeg, YPos, CenterXY, OnTop, CenterZ, Bed, Face };
    Op   m_hover_op = Op::None;      // the placing button under the mouse (set by the panel)
    // The ghost: #2's own mesh (object space), drawn translucent where the click would leave it.
    GLModel m_ghost_mesh;
    Pick    m_ghost_mesh_pick;
    size_t  m_ghost_mesh_volumes = 0;
    bool    m_ghost_visible      = false;
    Vec3d   m_ghost_from { Vec3d::Zero() };   // centre of #2 now / after, for the on-screen mark
    Vec3d   m_ghost_to   { Vec3d::Zero() };
    // The move `op` would make, with the exact maths the click uses. False if it can't be done.
    bool op_delta(Op op, Vec3d& delta) const;
    void apply_op(Op op);
    void swap_order();
    void render_place_ghost();

    // --- Helpers ------------------------------------------------------------
    void seed_order_from_selection();
    void prune_dead_objects();
    // A click on part `vol_idx` of object `object_idx` (-1 = not on a part): see Pick.
    void click_object(int object_idx, int vol_idx);
    void remove_pick(size_t i);
    void clear_order();
    // The object of `p` stays selected while some pick still uses it.
    void sync_selection_of(int object_idx);
    void render_badges();

    // Tint every GLVolume of each ordered object with its order color, after
    // restoring any previous tint. Requests a redraw.
    void apply_highlight();
    void restore_highlight();
    // Re-apply highlight and ask the canvas to repaint next frame.
    void refresh_highlight();

    int  ordered_obj(size_t i) const {
        return i < m_ordered.size() ? m_ordered[i].obj : -1;
    }

    // What a pick is made of. nullptr / empty when the pick is stale.
    const ModelObject* pick_object(const Pick& p) const;
    const ModelVolume* pick_volume(const Pick& p) const;   // nullptr for a whole object
    Transform3d        pick_trafo(const Pick& p) const;    // pick_mesh() -> world (first instance)
    TriangleMesh       pick_mesh(const Pick& p) const;     // object raw mesh, or the part's mesh
    std::string        pick_name(const Pick& p) const;
    int                model_part_count(int object_idx) const;

    BoundingBoxf3 world_bbox_of(const Pick& p) const;

    // Translate the object (every instance), or just the part inside its object.
    void translate_pick(const Pick& p, const Vec3d& delta);

    // Pure maths behind op_delta().
    Vec3d compute_place_delta(int axis, int dir, bool flush,
                              const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const;
    Vec3d compute_center_delta(int axis, const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const;

    void clear_face_pick();
    void ensure_face_raycaster_for_A();

    // Raycast under `mouse_pos` against #1 and update the hovered facet.
    void update_hover_face(const Vec2d& mouse_pos);
    // Build a GLModel for the connected coplanar region grown from `facet_idx`
    // (object space, each triangle lifted a hair along its normal to avoid
    // z-fighting) tinted with `col`. On a flat face this lights up the whole
    // face; on curved surfaces it stays a small patch around the cursor.
    // cos_thresh: how far a neighbour's normal may drift from the seed's (hover is loose,
    // the picked face is strict so what is lit is exactly what Construct extrudes).
    void build_face_model(GLModel& model, int facet_idx, const ColorRGBA& col, float cos_thresh);
    // Render the hover/picked face overlays (flat shader, depth-tested).
    void render_face_highlights();
    float m_construct_height    = 5.0f;  // mm (was 0.05, 5 is a better start for everyone)
    float m_construct_offset    = 0.0f;  // mm, negative shrinks the outline
    bool  m_construct_both      = false; // the height is split half outwards, half inwards
    bool  m_construct_holes     = false; // extrude the holes of the face (filled) instead of the face
    bool  m_construct_as_object = false; // false = part of #1, true = separate object
    // Translucent preview of the sheet, in world coordinates. Rebuilt only when the
    // picked facet, the two values or #1's position change.
    GLModel     m_construct_ghost;
    int         m_construct_ghost_facet  = -2;
    float       m_construct_ghost_height = 0.f;
    float       m_construct_ghost_offset = 0.f;
    bool        m_construct_ghost_both   = false;
    bool        m_construct_ghost_holes  = false;
    Transform3d m_construct_ghost_trafo { Transform3d::Identity() };
    std::string m_construct_block_reason; // why "Create" is off (empty = it can create)

    // Builds the sheet (world coordinates) from the picked face. False + `why` when it can't.
    bool build_construct_mesh(indexed_triangle_set& out, std::string& why, bool preview,
                              size_t* n_facets = nullptr, double* face_z = nullptr);
    void update_construct_ghost();
    void render_construct_ghost();
    void apply_construct();

    // Boolean between #1 and #2, no "assemble" needed first.
    // op is the mcut name ("UNION", "A_NOT_B", "INTERSECTION"). two parts of the same object
    // give a part of that object, in place; anything else gives a new object.
    bool        m_bool_keep_inputs = false; // false = #1 and #2 are removed after the operation
    std::string m_bool_error;               // last failure, shown under the buttons
    void apply_boolean(const std::string& op, const std::string& suffix);

    // Remesh (voxels): #1 rebuilt as one closed solid (a repair), its surface moved by Offset,
    // optionally hollow with Wall/Smooth and open through the picked face. Result: Replace #1, a
    // New object, or (hollow only) the inside as a negative part of #1.
    enum class RemeshOut { Replace, NewObject, NegativePart };
    enum class RemeshOpening { Face, Angle, Circle };
    RemeshOpening m_remesh_opening    = RemeshOpening::Face;
    float       m_remesh_open_angle   = 20.f;   // deg, Angle opening
    float       m_remesh_hole_d       = 20.f;   // mm, Circle opening
    float       m_remesh_voxel        = 0.2f;   // mm
    float       m_remesh_offset       = 0.0f;   // mm, + out / - in
    bool        m_remesh_hollow       = false;
    bool        m_remesh_open_face    = true;   // only with a picked face
    float       m_hollow_wall         = 2.0f;   // mm
    float       m_hollow_smooth       = 3.0f;   // mm: the inside is shrunk this much more and grown back
    int         m_remesh_max_tris     = 500000;
    float       m_remesh_adaptive     = 0.1f;   // merges triangles where the surface is flat (0..1)
    bool        m_remesh_keep_paint   = true;
    bool        m_remesh_drop_bits    = true;   // delete tiny loose shells
    bool        m_remesh_fast_smooth  = true;   // the inside on coarser voxels
    bool        m_remesh_running      = false;  // the worker is busy (the progress window pumps events)
    RemeshOut   m_remesh_out          = RemeshOut::Replace;
    std::string m_remesh_error;                 // last failure, shown under the button
    // Surface of #1 (mm²), for the size estimate; recomputed when #1 changes.
    double      m_remesh_area         = 0.;
    const void* m_remesh_area_key     = nullptr;
    void apply_remesh();
};

}} // namespace Slic3r::GUI

#endif
