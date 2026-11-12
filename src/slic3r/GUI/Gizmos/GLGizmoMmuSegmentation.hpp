#ifndef slic3r_GLGizmoMmuSegmentation_hpp_
#define slic3r_GLGizmoMmuSegmentation_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PaintClay.hpp"

#include <atomic>
#include <chrono>
#include <memory>

namespace Slic3r::GUI {

class GLMmSegmentationGizmo3DScene
{
public:
    GLMmSegmentationGizmo3DScene() = delete;

    explicit GLMmSegmentationGizmo3DScene(size_t triangle_indices_buffers_count)
    {
    }

    virtual ~GLMmSegmentationGizmo3DScene() { release_geometry(); }

    [[nodiscard]] inline bool has_VBOs(size_t triangle_indices_idx) const
    {
        assert(triangle_indices_idx < this->triangle_patches.size());
        return this->triangle_indices_VBO_ids[triangle_indices_idx] != 0;
    }

    // Release the geometry data, release OpenGL VBOs.
    void release_geometry();
    // Finalize the initialization of the geometry, upload the geometry to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_vertices();
    // Finalize the initialization of the indices, upload the indices to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_triangle_indices();

    void clear()
    {
        this->vertices.clear();
        // BBS
        this->triangle_indices_VBO_ids.clear();
        this->triangle_indices_sizes.clear();

        for (TrianglePatch& patch : this->triangle_patches)
            patch.triangle_indices.clear();
        this->triangle_patches.clear();
    }

    void render(size_t triangle_indices_idx) const;

    std::vector<float>            vertices;
    //std::vector<std::vector<int>> triangle_indices;

    // BBS
    std::vector<TrianglePatch>    triangle_patches;

    // When the triangle indices are loaded into the graphics card as Vertex Buffer Objects,
    // the above mentioned std::vectors are cleared and the following variables keep their original length.
    std::vector<size_t> triangle_indices_sizes;

    // IDs of the Vertex Array Objects, into which the geometry has been loaded.
    // Zero if the VBOs are not sent to GPU yet.
    unsigned int              vertices_VBO_id{0};
    std::vector<unsigned int> triangle_indices_VBO_ids;
};

class GLGizmoMmuSegmentation : public GLGizmoPainterBase
{
public:
    GLGizmoMmuSegmentation(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoMmuSegmentation() override = default;

    void render_painter_gizmo() override;

    void data_changed(bool is_serializing) override;

    // Keep this in sync with the shared triangle-selector state range.
    static const constexpr size_t EXTRUDERS_LIMIT = static_cast<size_t>(EnforcerBlockerType::ExtruderMax);

    const float get_cursor_radius_min() const override { return CursorRadiusMin; }

    // BBS
    bool on_number_key_down(int number);
    bool on_key_down_select_tool_type(int keyCode);

protected:
    // BBS
    ColorRGBA get_cursor_hover_color() const override;
    void on_set_state() override;

    EnforcerBlockerType get_left_button_state_type() const override
    {
        if (m_selected_extruder_idx < m_display_filament_ids.size())
            return EnforcerBlockerType(m_display_filament_ids[m_selected_extruder_idx]);
        return EnforcerBlockerType::Extruder1;
    }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    void show_tooltip_information(float caption_max, float x, float y);
    bool on_is_selectable() const override;
    bool on_is_activable() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;
    // Painter Pro Mode F5: shape mask preview fill in the selected filament colour.
    ColorRGBA get_shape_fill_color() const override;
    // F2: colour of a filament id, and the filament closest to an SVG colour.
    ColorRGBA get_state_color(int state) const override;
    int       svg_default_state(uint32_t rgb) const override;

    std::string get_gizmo_entering_text() const override { return "Entering color painting"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving color painting"; }
    std::string get_action_snapshot_name() const override { return "Color painting editing"; }

    // BBS
    size_t                            m_selected_extruder_idx = 0;
    std::vector<ColorRGBA>            m_extruders_colors;
    std::vector<unsigned int>         m_display_filament_ids;
    std::vector<int>                  m_volumes_extruder_idxs;

    // BBS
    wchar_t                           m_current_tool = 0;
    bool                              m_detect_geometry_edge = true;
    
    // Filament remap feature
    std::vector<size_t>               m_extruder_remap;      // index → target extruder index
    bool                              m_show_filament_remap_ui = false;

    // Minimal context for gradient rendering; only physical_colors is used
    MixedFilamentDisplayContext       m_mixed_display_context;

    // Painter Pro Mode F4, Sesion A/B: rectangle/polygon mask toggles.
    // Mutually exclusive (like the existing Vertical/Horizontal checkboxes below). Override
    // m_tool_type regardless of which toolbar icon is selected (mirrors how F1/F2/F3 live as Pro
    // Mode toggles rather than new toolbar buttons - a new toolbar icon needs a new glyph in the
    // ImGui icon font, out of scope here).
    bool                              m_rectangle_mask_active = false;
    bool                              m_polygon_mask_active   = false;
    // when true, "Extra walls" / "Surface depth" edit the per-color
    // override of the currently selected paint color instead of the global value.
    bool                              m_per_color_values      = false;
    const ModelObject                *m_per_color_mo          = nullptr; // object the checkbox was last derived for

    static const constexpr float      CursorRadiusMin = 0.1f; // cannot be zero

private:
    bool on_init() override;

    // BBS. remove const.
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update = false) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    void init_model_triangle_selectors();

    // BBS
    void update_triangle_selectors_colors();
    void init_extruders_data();
    void init_extruders_data(const std::vector<ColorRGBA> &extruder_colors);
    
    // Filament remapping methods
    void remap_filament_assignments();
    void render_filament_remap_ui(float window_width, float max_tooltip_width);

    // Pro Mode section: brush precision (F3), paint-perimeters-only
    // (F1), extra walls on painted regions (F2), rectangle/polygon masks (F4).
    void render_pro_mode_section(float sliders_left_width, float sliders_width, float slider_icon_width);
    // the panel has two modes. Brush: Orca's tools (brushes, fill, height
    // range) and the rectangle mask. Vector: shapes drawn on screen and SVG decals. Keys B / V.
    // 2_50: third mode, Clay. Decals raised over the surface as parts of their own.
    // 2_50: VectorBrush, free-hand strokes saved as decals (edited in Vector).
    enum class PaintMode { Brush, Vector, Clay, VectorBrush };
    PaintMode m_paint_mode     = PaintMode::Brush;
    bool shape_draw_needs_arming() const override { return m_paint_mode == PaintMode::Vector || m_paint_mode == PaintMode::Clay; }
    bool m_show_decals_in_brush = false;
    bool vector_brush_on() const override { return m_paint_mode == PaintMode::VectorBrush; }
    void render_vector_brush_panel();
    void render_surface_controls();   // Draw on surface / Wrap: width, status, Finish / Cancel
    std::string m_vb_wrap_msg;        // why Wrap could not be picked
    int  selected_decal_index() const override { return m_selected_decal; }
    PaintDecal m_decal_ts_start;          // F4b: the decal when a Turn / Size slider was grabbed
    float      m_decal_turn_ui = 0.f;     // relative to that start, reset on release
    float      m_decal_size_ui = 100.f;
    bool show_decal_cards_while_brushing() const override { return (m_paint_mode == PaintMode::Brush && m_show_decals_in_brush) || m_paint_mode == PaintMode::VectorBrush; }
    int       m_selected_decal = -1;   // decal row picked in the Vector panel's layer list
    // 2b: Shift + click picks more rows; Merge joins them into the topmost one.
    std::vector<int> m_decal_multi;
    std::string      m_decal_merge_msg;
    void decal_merge();
    float     m_colour_row_w   = 0.f;  // measured width of one SVG colour row (0 until drawn once)
    void set_paint_mode(PaintMode mode);
    void render_rectangle_mask_option();
    void render_vector_panel();
    // Clay panel: the decals with their Clay settings, Build / Remove.
    void render_clay_panel();
    void render_decal_layers(bool clay);
    void render_decal_overlay();
    // Builds (or rebuilds) the Clay parts of decal idx: removes its old ones, bakes, adds the new
    // ones as normal parts, and paints the decal under them when paint_under. Returns the parts added.
    // Build runs on a worker thread (clay_build starts it, clay_poll finishes it on the GUI thread):
    // the panel shows progress and Cancel instead of freezing for seconds.
    struct ClayJob {
        std::atomic<bool>           cancel { false };
        std::atomic<bool>           finished { false };
        std::atomic<int>            done { 0 }, total { 0 };
        std::vector<PaintClayPiece> pieces;   // written by the worker, read after `finished`
    };
    std::shared_ptr<ClayJob> m_clay_job;
    size_t                   m_clay_job_decal = 0;
    std::string              m_clay_job_name;   // decal name when started: the decal must still be there
    ObjectID                 m_clay_job_object;
    std::chrono::steady_clock::time_point m_clay_job_start;
    bool                     m_clay_job_rebuild = false;
    bool                     m_clay_show_decal    = false;   // Clay's own "Show decal"
    bool                     m_clay_saved_preview = true;    // Vector's "Preview colours" while in Clay
    // Decals with Clay parts are watched: moved or edited (in Vector, by undo...) = rebuilt once the
    // mouse is let go. Key = the decal without its Clay settings (those rebuild from the panel).
    std::map<std::string, PaintDecal> m_clay_watch;
    std::vector<std::string>          m_clay_queue;   // Rebuild all: decal names still to build, one job at a time
    // X-ray of the engrave parts (negative Clay volumes): red, see-through, drawn over the object so the
    // hole they will cut is seen inside it. The painter draws none of the object's other volumes.
    // The model behind a pointer: a moved-from GLModel frees the GPU buffers the moved-to one still
    // draws from.
    struct ClayXray { ObjectID volume; size_t tris = 0; std::unique_ptr<GLModel> model; };
    std::vector<ClayXray> m_clay_xray;
    bool                  m_clay_xray_on = true;
    bool                  m_clay_arrow_on = true;
    bool                  m_clay_repair   = false;  // option (off): voxel-rebuild each part after Build: no self-crossings left for the slicer
    void                  render_clay_xray();
    ObjectID                          m_clay_watch_object;
    void clay_watch_decals();
    void clay_build(size_t idx);
    void clay_cancel();
    void clay_poll();
    int  clay_finish(size_t idx, const std::vector<PaintClayPiece> &pieces);
    int  clay_remove_parts(size_t idx);   // removes the Clay parts of decal idx; returns how many
    int  clay_detach_parts(size_t idx);   // they become normal parts: Rebuild / Remove leave them alone
    bool clay_apply_engrave(size_t idx);  // cuts the engrave part out of the object (keeps the painting) and deletes it
    std::string m_clay_status;
    std::string m_clay_warning;   // folded faces of the last build
    // Groups of the decal shown in Clay, kept while its SVG / zones / parts do not change (parsing every frame is slow).
    std::string      m_clay_groups_key;
    PaintDecalGroups m_clay_groups;
    // One SVG colour row: its colour, the filament it paints with (combo) and its eye.
    // Returns true when the filament or the eye changed.
    bool render_svg_colour_row(int id, uint32_t rgb, bool grad, int &state, bool &hidden);
    // parts of the SVG on screen: pick on the canvas, the selection's filament
    // and eye, and the list. True when the paint groups changed (masks to rebuild).
    bool render_svg_parts();
    void render_mirror_centre_controls();
    // Filament combo with a first entry "Colour" (state -1 = follow the colour of the part).
    bool render_part_filament_combo(const char *id, int &state);
    int  m_svg_parts_filter = 0;   // list shows: 0 = changed and selected, 1 = all


    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r


#endif // slic3r_GLGizmoMmuSegmentation_hpp_
