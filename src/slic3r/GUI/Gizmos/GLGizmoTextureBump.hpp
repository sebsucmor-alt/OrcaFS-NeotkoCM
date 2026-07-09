#ifndef slic3r_GLGizmoTextureBump_hpp_
#define slic3r_GLGizmoTextureBump_hpp_

#include "GLGizmoPainterBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "libslic3r/PerimeterGenerator.hpp" // TextureBumpConfig
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Feature/TextureBump/TextureBumpZone.hpp"
#include "libslic3r/Feature/ZBump/ZBump.hpp" // NEOTKO_ZBUMP_TAG -- ZBumpConfig, own domain
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

// NEOTKO_TEXTUREBUMP_TAG — unification pass (2026-07-07, docs/ATTRIBUTION_TEXTURE_BUMP.md §6):
// merges what used to be 2 separate gizmos (GLGizmoTextureBump "All" mode + the deleted
// GLGizmoTextureBumpPainter, now OLDNOUSE) into ONE, with `m_mode` picking between them. Having 2
// live gizmos editing overlapping state caused a real mix-up in practice (a working fix landed in
// the wrong one), which is the direct reason this merge exists now rather than staying "future
// work, not urgent" as originally planned.
//
// Base class is GLGizmoPainterBase (needed for Painter mode's brush pipeline), not GLGizmoBase.
// This means on_render() is sealed to a no-op by GLGizmoPainterBase -- ALL drawing (both modes)
// happens in render_painter_gizmo() instead, dispatched on m_mode. Same dispatch pattern in
// on_mouse(): All mode calls use_grabbers() (GLGizmoBase's grabber pipeline) directly, bypassing
// GLGizmoPainterBase::on_mouse()'s brush pipeline entirely; Painter mode calls the brush pipeline.
// This is mechanical, not a redesign: GLGizmoTextureBump::on_mouse() was ALREADY just
// `return use_grabbers(mouse_event);` before this merge, and GLGizmoPainterBase::on_mouse() never
// touched grabbers, so the two paths don't fight over anything.
//
// GLGizmoPainterBase requires on_opening()/on_shutdown()/update_model_object()/
// update_from_model_object()/get_painter_type()/handle_snapshot_action_name() as pure virtual --
// implemented here (moved from the old Painter almost verbatim) regardless of mode; they're cheap
// infrastructure (triangle-selector sync, transparency, picking) that the framework's own
// on_set_state() invokes, not something either mode opts out of.

namespace Slic3r {

namespace GUI {

class GLGizmoTextureBump : public GLGizmoPainterBase
{
public:
    GLGizmoTextureBump(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

    // Max slots per painted volume -- slot 0 is "unpainted", 1..MAX_SLOTS-1 are zone slots. Must
    // match ModelVolume::COLORMIX_SLOT_COUNT (static_assert in .cpp) -- same FacetsAnnotation
    // encoding capacity ColorMix reuses, own canvas (texture_bump_paint_facets).
    static constexpr int MAX_SLOTS = 255;

    // NEOTKO_TEXTUREBUMP_TAG -- which section of the panel is showing. All = the object-wide
    // config (3D V/U/yaw/pivot handles + read-only value display); Painter = paint zones, each
    // with its own TextureBumpConfig (numeric-only transform fields, no 3D handles -- see
    // TextureBumpConfigUI.hpp's show_plane_handles_hint for why).
    // NEOTKO_ZBUMP_TAG -- Top = a third, unrelated domain (Feature/ZBump/, top-surface Z relief,
    // own ZBumpConfig struct, no shared code with Texture Bump's engine) reusing only this
    // gizmo's shell/mode-bar widget. Single global config per object, no 3D handles, no paint.
    enum class Mode { All, Painter, Top };

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    bool        on_is_selectable() const override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    bool        on_mouse(const wxMouseEvent& mouse_event) override;
    void        on_start_dragging() override;
    void        on_dragging(const UpdateData& data) override;
    void        on_stop_dragging() override;
    void        on_register_raycasters_for_picking() override;
    void        on_unregister_raycasters_for_picking() override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Bump Mapping Editor"); }
    std::string get_gizmo_leaving_text()  const override { return _u8L("Leaving Bump Mapping Editor"); }
    std::string get_action_snapshot_name() const override { return _u8L("Bump Mapping Editor"); }

    // Painter state encoding: slot index 1..MAX_SLOTS-1 maps directly to EnforcerBlockerType(N).
    EnforcerBlockerType get_left_button_state_type()  const override;
    // Right button never paints/erases here (same sentinel ColorMixPainter uses so it falls
    // through to camera pan instead of being treated as an erase stroke by the base).
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

private:
    void             update_model_object()                       override;
    void             update_from_model_object(bool first_update) override;
    void             on_opening()  override;
    void             on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    ModelObject* get_model_object() const;
    // NEOTKO_TEXTUREBUMP_TAG -- requested explicitly: the real (opaque) object blocks the view of
    // what's being projected onto it, especially bad with Spherical (the object occludes the far
    // side of the overlay from most angles). Dims the selected object's own volumes while this
    // gizmo is on; restores the ORIGINAL alpha (not hardcoded to 1.0f) when turned off.
    // NEOTKO_TEXTUREBUMP_TAG -- ⚠️ REPORTED BROKEN, unification only merged the 2 duplicate
    // implementations into 1 -- did not fix it (see Task 4 / docs/ATTRIBUTION_TEXTURE_BUMP.md §6).
    void set_object_transparent(bool transparent);
    std::vector<std::pair<unsigned int, float>> m_dimmed_volume_original_alpha;
    // Rebuilds m_overlay_model + repositions m_grabbers[0..3] from the current
    // mode/axis/scale/repeat/bounding box. Cheap GL-only work, safe to call every drag frame.
    void update_overlay_and_grabbers();
    // Shared by update_overlay_and_grabbers() (resting position) and on_start_dragging() (drag
    // sensitivity) so the two never disagree: mm of drag distance representing +1 repeat_u,
    // proportional to the wrap loop's circumference (or one face's width, for Cubic) -- NOT a
    // fixed constant, so the feel doesn't depend on absolute object size (see plan notes).
    double compute_repeat_step_mm(const BoundingBoxf3& box) const;
    // Verbatim port of GLGizmoMove3D::calc_projection() (GLGizmoMove.cpp) -- not reused directly
    // since that method is private to GLGizmoMove3D; projects the mouse ray onto the direction
    // (m_drag_starting_handle_pos - m_drag_starting_box_center) and returns a signed mm delta.
    double calc_projection(const UpdateData& data) const;

    Mode m_mode{ Mode::All };

    // NEOTKO_TEXTUREBUMP_TAG -- single shadow copy of the object's texture_bump_* config, built by
    // resolve_base_texture_bump_config() (TextureBumpConfigUI.hpp) in on_opening() -- the same
    // shape TextureBumpZoneProfile::config uses per-zone, so both modes share
    // render_texture_bump_config_sections(). Mutated locally during 3D-handle drags
    // (scale/repeat_u/plane_transform) and by the shared ImGui form (everything else); committed
    // to the object's config only on release/interaction end (see commit_config_field()) -- these
    // are PrintRegionConfig options that trigger a real re-slice when written.
    TextureBumpConfig m_config{
        TextureBumpType::None, scaled<coord_t>(0.2), scaled<coord_t>(0.3), false,
        TextureProjectionMode::Planar, TextureProjectionAxis::Z, 20.0, 1, 45.0 * M_PI / 180.0, 1.0, std::string()};

    // Writes a single changed field of m_config back onto the object's config (or
    // ModelObject::texture_bump_plane_transform for plane_yaw/plane_pivot/plane_reset, not a
    // ConfigOption) -- the commit callback passed to render_texture_bump_config_sections() in All mode.
    void commit_config_field(ModelObject* model_object, const char* field);

    // NEOTKO_ZBUMP_TAG -- Mode::Top's own shadow config, same "local copy, committed on
    // interaction end" pattern as m_config above, but a totally separate struct/domain (own
    // engine, Feature/ZBump/, no shared storage with Texture Bump's m_config).
    Feature::ZBump::ZBumpConfig m_zbump_config;
    // Writes a single changed field of m_zbump_config back onto the object's zbump_* config --
    // the commit callback passed to render_zbump_config_section() in Top mode.
    void commit_zbump_config_field(ModelObject* model_object, const char* field);

    // NEOTKO_ZBUMP_TAG -- Top mode's own overlay + pan-handle state, mirrors the All-mode fields
    // above 1:1 in spirit (own overlay model/transform/texture, own dirty flag, own drag-start
    // scratch) but kept fully separate -- Top is always a flat Z-axis projection (no axis/
    // projection-mode choice), so its overlay geometry and single 2D pan grabber (index 4) are
    // simpler than All mode's 4 handles, and nothing here shares rebuild/dirty state with them.
    void          update_zbump_overlay_and_grabbers();
    bool          m_zbump_overlay_dirty{ true };
    BoundingBoxf3 m_zbump_cached_bbox;
    GLModel       m_zbump_overlay_model;
    Transform3d   m_zbump_overlay_transform{ Transform3d::Identity() };
    GLTexture     m_zbump_preview_texture;
    std::string   m_zbump_preview_texture_path;
    // Starting value for handle 4's "did it actually change" check in on_stop_dragging() only --
    // the drag math itself (on_dragging()) needs no starting reference, see its own comment.
    double m_zbump_drag_starting_offset_x{ 0.0 };
    double m_zbump_drag_starting_offset_y{ 0.0 };

    // Per-handle drag reference point (mirrors GLGizmoMove3D's use of a fixed box-center anchor,
    // but per-handle here): the V handle's resting XY position is offset away from the object's
    // center to avoid sitting inside the object/overlay's solid volume (see update_overlay_and_
    // grabbers()), so its projection direction can no longer be derived from the true bbox center
    // without becoming diagonal. Storing an explicit per-handle anchor (same XY as the handle, at
    // base height for V; the true box center for U) keeps calc_projection()'s direction exactly
    // aligned with the intended drag axis regardless of where the handle visually rests.
    std::array<Vec3d, 2> m_grabber_ref_point{ Vec3d::Zero(), Vec3d::Zero() };

    // Drag-only scratch state (mirrors GLGizmoMove3D's m_starting_* fields). Deliberately no
    // separate "reference direction" field: exactly like GLGizmoMove3D::calc_projection(), the
    // projection direction is DERIVED from (m_drag_starting_handle_pos - m_drag_starting_box_center)
    // -- this falls out correctly for free because update_overlay_and_grabbers() already places
    // each grabber along the direction it should drag along (V along world Z, U along the
    // wrap-plane's reference direction), so no separate axis bookkeeping is needed here.
    // Captured from m_hover_id in on_start_dragging() rather than re-read from m_hover_id in
    // on_dragging()/on_stop_dragging() -- GLGizmoBase::do_stop_dragging() can invoke
    // mouse_up_cleanup() (which resets m_hover_id) BEFORE calling on_stop_dragging() when the
    // mouse leaves the viewport mid-drag, so relying on m_hover_id there would silently drop the
    // commit in that edge case.
    int    m_dragging_handle_id{ -1 };
    double m_drag_starting_scale_mm{ 0.0 };
    int    m_drag_starting_repeat_u{ 1 };
    double m_drag_repeat_step_mm{ 10.0 };
    Vec3d  m_drag_starting_handle_pos{ Vec3d::Zero() };
    Vec3d  m_drag_starting_box_center{ Vec3d::Zero() };
    // Handles 2 (yaw)/3 (pivot) don't use calc_projection()'s ray-onto-line math (that only ever
    // yields a 1D delta) -- they intersect the mouse ray against the wrap-plane itself (see
    // TextureBumpPlaneHandles.hpp), which needs the full starting transform, not just a scalar.
    Transform3d m_drag_starting_plane_transform{ Transform3d::Identity() };

    // All-mode overlay geometry (the object's own base config), rebuilt only when dirty
    // (mode/axis/bbox/scale/repeat changed since last build).
    GLModel       m_overlay_model;
    Transform3d   m_overlay_transform{ Transform3d::Identity() };
    bool          m_overlay_dirty{ true };
    BoundingBoxf3 m_cached_bbox;

    // The actual PNG rendered onto the All-mode overlay, so the user can confirm visually what
    // loaded and how it will be oriented/scaled/repeated, instead of only ever seeing a solid
    // color. Deliberately a SEPARATE cache from libslic3r's own load_texture_image()
    // (TextureBump.cpp) -- that one decodes into a png::ImageGreyscale for the slicing engine and
    // lives in libslic3r, which slic3r/GUI can't (and shouldn't) reach into; this is a plain
    // GLTexture, GUI-only, reloaded only when the path changes.
    GLTexture   m_preview_texture;
    std::string m_preview_texture_path;

    // ------------------------------------------------------------------------------------------
    // Painter mode (absorbed from the deleted GLGizmoTextureBumpPainter, OLDNOUSE)
    // ------------------------------------------------------------------------------------------

    // Resolve the slot index in the currently selected ModelObject for the chosen zone. Returns 0
    // if no zone is selected or if every slot is already taken by another zone. When
    // `assign_if_missing` is true and a free slot exists, assigns the zone to it (mutates the
    // model) and returns the new slot.
    int slot_for_selected_zone(bool assign_if_missing);

    // Build per-volume EBT color palette from the slot->zone table + manager (index 0 = neutral
    // base, same layout GLGizmoColorMixPainter::build_ebt_colors_for_volume uses).
    std::vector<ColorRGBA> build_ebt_colors_for_volume(const ModelVolume* mv) const;
    // NEOTKO_TEXTUREBUMP_TAG -- bug fix (reported: freshly painted/newly-assigned slots show gray
    // until the gizmo is closed and reopened): each TriangleSelectorPatch's EBT color array is a
    // SNAPSHOT taken once in update_from_model_object() -- it never re-reads
    // texture_bump_slot_to_zone_id afterwards. slot_for_selected_zone(assign_if_missing=true) can
    // assign a slot DURING an active paint stroke (get_left_button_state_type()), and "Delete zone"
    // clears a slot mapping -- both need this called right after so already-existing
    // TriangleSelectorPatch instances (built earlier, mid-session) show the correct color
    // immediately instead of only after a full close/reopen. Uses TriangleSelectorPatch's own
    // set_ebt_colors() (no rebuild of the selector/facet data, just repaints with fresh colors).
    void refresh_ebt_colors();

    void render_zone_list();
    // Small thumbnail for a zone's own texture image, shown next to its color swatch in
    // render_zone_list() so zones are recognizable by image, not just name. Lazily
    // loaded/reloaded per zone id, keyed off image_path -- separate cache from
    // m_zone_overlay_texture (that one is only ever built for the currently SELECTED zone's big 3D
    // overlay preview). Returns nullptr if the zone has no image set or the load failed.
    GLTexture* zone_thumbnail(const TextureBumpZoneProfile& zone);
    std::map<int, GLTexture>   m_zone_thumbnails;
    std::map<int, std::string> m_zone_thumbnail_paths;
    void render_zone_editor(TextureBumpZoneProfile& zone);
    // Overlay preview for the SELECTED zone, same shared mesh builder the All mode uses
    // (TextureBumpOverlayMesh.hpp) so both modes never disagree on what the engine will actually
    // project. Rebuilt only when the zone's relevant fields (or the selection bbox) actually
    // change since the last render -- render_painter_gizmo() runs every frame, and reloading a PNG
    // from disk every frame would not be free.
    void render_zone_overlay();
    // NEOTKO_TEXTUREBUMP_TAG -- Bug fix (s181): plater()->update() cascades into reload_scene() ->
    // GLGizmosManager::update_data() -> data_changed() -> update_from_model_object(), which
    // unconditionally zeroes m_selected_zone_id/m_active_slot. Every zone-editor commit but
    // "Delete zone" wants the reslice WITHOUT losing the current selection, so save/restore
    // around the call.
    void commit_and_reslice();
    // Creates a new zone in TextureBumpZoneManager seeded from the currently selected object's
    // OWN base texture_bump_* config -- an explicit "duplicate the object's current settings"
    // action, NOT what plain "New zone" does (see create_new_blank_zone()).
    void create_new_zone_from_base_config();
    // "New zone" means genuinely new: the same hardcoded, object-independent defaults
    // resolve_base_texture_bump_config(nullptr) already falls back to when there's no object to
    // read from (AllWalls/Planar/Z/20mm/no image). Seeding from the object's current settings is a
    // SEPARATE, explicitly-labeled action (create_new_zone_from_base_config(), "Duplicate object
    // settings" button) so the user always knows which one they clicked.
    void create_new_blank_zone();

    // Currently chosen TextureBumpZoneManager zone id (0 = none selected).
    int  m_selected_zone_id = 0;
    // Resolved slot 1..MAX_SLOTS-1 for the selected zone on the active object (0 if unresolved).
    int  m_active_slot      = 0;
    // When true, left-click erases (resets the slot to 0) instead of painting.
    bool m_erase_mode       = false;

    std::map<std::string, wxString> m_desc;

    // Zone overlay cache -- last-built-from values, compared each render_painter_gizmo() call to
    // decide whether to rebuild m_zone_overlay_model/reload m_zone_overlay_texture. Named
    // separately from the All-mode m_overlay_* members above (own overlay, own purpose) now that
    // both modes live in one class. -1/empty sentinel values guarantee the first render after a
    // zone is selected always (re)builds.
    int                    m_zone_overlay_zone_id{ 0 };
    TextureProjectionMode  m_zone_overlay_cached_mode{ TextureProjectionMode::Planar };
    TextureProjectionAxis  m_zone_overlay_cached_axis{ TextureProjectionAxis::Z };
    double                 m_zone_overlay_cached_scale{ -1.0 };
    int                    m_zone_overlay_cached_repeat_u{ -1 };
    Transform3d            m_zone_overlay_cached_plane_transform{ Transform3d::Identity() };
    BoundingBoxf3          m_zone_overlay_cached_bbox;
    GLModel                m_zone_overlay_model;
    Transform3d            m_zone_overlay_transform{ Transform3d::Identity() };
    GLTexture              m_zone_overlay_texture;
    std::string            m_zone_overlay_texture_path;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoTextureBump_hpp_
