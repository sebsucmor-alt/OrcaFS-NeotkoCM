// NEOTKO_PRECISIONALH_TAG_START
#ifndef slic3r_GLGizmoPrecisionALH_hpp_
#define slic3r_GLGizmoPrecisionALH_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/Slicing.hpp"

#include <vector>

namespace Slic3r { namespace GUI {

// Precision Adaptive Layer Height: point-based curve editor for
// ModelObject::layer_height_profile. Replaces the free-hand brush of the
// stock "Layers editing" tool (GLCanvas3D::LayersEditing, untouched by this
// gizmo) with exact (Z, height) control points and a per-segment tension
// (blend between a straight line and a Fritsch-Carlson monotone cubic, so it
// never overshoots past the two endpoint heights of a segment).
//
// LibreMode-gated (Tier B), single condition, same pattern as
// GLGizmoAlignStack::on_is_activable() — see
// docs/WIP/PRECISION_ADAPTIVE_LAYER_HEIGHT_PLAN.md §3.2.
class GLGizmoPrecisionALH : public GLGizmoBase
{
public:
    GLGizmoPrecisionALH(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    void        on_set_state() override;

private:
    struct ALHPoint
    {
        double z_mm;
        double height_mm;
        double tension; // tension of the segment starting at this point, [0,1]; unused on the last point
    };

    // Session state: which object this in-progress edit belongs to, and the
    // control points themselves. Sorted by z; front().z_mm == 0,
    // back().z_mm == object height. Not reloaded from the model on every
    // toggle of the gizmo over the same object (in-progress edits survive a
    // close/reopen); reset when the selected object changes.
    bool                   m_have_session = false;
    int                    m_object_idx   = -1;
    double                 m_object_height = 0.0;       // == SlicingParameters::object_print_z_uncompensated_height()
    double                 m_first_layer_height = 0.2;  // the fixed first layer; the profile MUST start at this height
    SlicingParameters      m_slicing_params;
    std::vector<ALHPoint>  m_points;

    // Drag-in-progress state (local mutation, not yet committed/undo-snapshotted).
    int m_dragging_point = -1; // index into m_points, -1 = none
    int m_hover_point    = -1; // index of the point under the cursor in the 2D band, -1 = none
                                // (drives both the numeric label and the 3D Z-band highlight)

    // 3D Z-band highlight on the object (built in world coords, flat shader) —
    // rebuilt only when the affected point/Z changes (m_band_key).
    GLModel m_band_model;
    double  m_band_key = -1.0;

    // The bottom control point (index 0, z=0) is pinned to the fixed first
    // layer height and is not editable — see load/seed and build_profile_vector.
    bool point_is_locked(int idx) const { return idx == 0; }

    void ensure_session();
    void seed_flat_profile();
    // Rebuilds m_points from an existing ModelObject::layer_height_profile (one
    // control point per stored (z,h) pair, straight segments) so reopening the
    // gizmo — or a fresh session after a project reload — doesn't discard a
    // profile that was already committed. Falls back to seed_flat_profile() for
    // an empty profile, or one too dense to be a reasonable set of control
    // points (i.e. written by the stock brush, not this gizmo).
    void load_points_from_profile(const std::vector<coordf_t>& profile);

    // Fritsch-Carlson monotone tangents for the current m_points (one per point).
    std::vector<double> compute_tangents() const;
    // Height at parametric t in [0,1] along segment [seg_idx, seg_idx+1], blending
    // linear (tension=0) and the monotone cubic Hermite curve (tension=1).
    double blended_height(size_t seg_idx, double t, const std::vector<double>& tangents) const;
    // Final [z0,h0,z1,h1,...] vector consumed as-is by generate_object_layers().
    std::vector<coordf_t> build_profile_vector() const;

    // Takes an undo snapshot, writes the profile into ModelObject::layer_height_profile
    // and schedules a re-slice — same commit path as the stock pincel/dialog.
    void commit(const std::string& snapshot_name);

    // Builds a translucent horizontal slab (world coords) at the object's Z of
    // control point `idx`, spanning its XY footprint — the "affected zone".
    void build_band_model(int idx);

    void render_curve_editor(float width, float height);
};

}} // namespace Slic3r::GUI

#endif
// NEOTKO_PRECISIONALH_TAG_END
