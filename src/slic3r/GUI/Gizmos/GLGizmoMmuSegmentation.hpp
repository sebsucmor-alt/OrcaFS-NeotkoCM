#ifndef slic3r_GLGizmoMmuSegmentation_hpp_
#define slic3r_GLGizmoMmuSegmentation_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/MixedFilament.hpp"

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
    // Shape mask preview fill in the selected filament colour.
    ColorRGBA get_shape_fill_color() const override;
    // Colour of a filament id, and the filament closest to an SVG colour.
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

    bool                              m_rectangle_mask_active = false;
    bool                              m_polygon_mask_active = false;

    // The panel has two modes. Brush: Orca's tools (brushes, fill, height range) and the
    // rectangle mask. Vector: shapes drawn on screen and SVG decals. Keys B / V.
    enum class PaintMode { Brush, Vector };
    PaintMode m_paint_mode     = PaintMode::Brush;
    bool shape_draw_needs_arming() const override { return m_paint_mode == PaintMode::Vector; }
    bool m_show_decals_in_brush = false;
    int  selected_decal_index() const override { return m_selected_decal; }
    PaintDecal m_decal_ts_start;          // the decal when a Turn / Size slider was grabbed
    float      m_decal_turn_ui = 0.f;     // relative to that start, reset on release
    float      m_decal_size_ui = 100.f;
    bool show_decal_cards_while_brushing() const override { return m_paint_mode == PaintMode::Brush && m_show_decals_in_brush; }
    int       m_selected_decal = -1;   // decal row picked in the Vector panel's layer list
    float     m_colour_row_w   = 0.f;  // measured width of one SVG colour row (0 until drawn once)
    void set_paint_mode(PaintMode mode);
    void render_vector_panel();
    // One SVG colour row: its colour, the filament it paints with (combo) and its eye.
    // Returns true when the filament or the eye changed.
    bool render_svg_colour_row(int id, uint32_t rgb, bool grad, int &state, bool &hidden);
    // Parts of the SVG on screen: pick on the canvas, the selection's filament and eye, and the
    // list. True when the paint groups changed (masks to rebuild).
    bool render_svg_parts();
    void render_mirror_centre_controls();
    // Filament combo with a first entry "Colour" (state -1 = follow the colour of the part).
    bool render_part_filament_combo(const char *id, int &state);
    int  m_svg_parts_filter = 0;   // list shows: 0 = changed and selected, 1 = all

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

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r


#endif // slic3r_GLGizmoMmuSegmentation_hpp_
