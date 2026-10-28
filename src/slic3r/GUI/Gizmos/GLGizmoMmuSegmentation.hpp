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
    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5: shape mask preview fill in the selected filament colour.
    ColorRGBA get_shape_fill_color() const override;
    // NEOTKO_SVGDECAL_TAG — F2: colour of a filament id, and the filament closest to an SVG colour.
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

    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4, Sesion A/B: rectangle/polygon mask toggles.
    // Mutually exclusive (like the existing Vertical/Horizontal checkboxes below). Override
    // m_tool_type regardless of which toolbar icon is selected (mirrors how F1/F2/F3 live as Pro
    // Mode toggles rather than new toolbar buttons - a new toolbar icon needs a new glyph in the
    // ImGui icon font, out of scope here).
    bool                              m_rectangle_mask_active = false;
    bool                              m_polygon_mask_active   = false;
    // NEOTKO_PAINTERPRO_TAG — when true, "Extra walls" / "Surface depth" edit the per-color
    // override of the currently selected paint color instead of the global value.
    bool                              m_per_color_values      = false;

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

    // NEOTKO_PAINTERPRO_TAG — Pro Mode section: brush precision (F3), paint-perimeters-only
    // (F1), extra walls on painted regions (F2), rectangle/polygon masks (F4).
    // See docs/FUTURE/PAINTER_PROMODE_PLAN.md.
    void render_pro_mode_section(float sliders_left_width, float sliders_width, float slider_icon_width);
    // NEOTKO_SVGDECAL_TAG — the panel has two modes. Brush: Orca's tools (brushes, fill, height
    // range) and the rectangle mask. Vector: shapes drawn on screen and SVG decals. Keys B / V.
    enum class PaintMode { Brush, Vector };
    PaintMode m_paint_mode     = PaintMode::Brush;
    bool shape_draw_needs_arming() const override { return m_paint_mode == PaintMode::Vector; }
    bool m_show_decals_in_brush = false;
    int  selected_decal_index() const override { return m_selected_decal; }
    PaintDecal m_decal_ts_start;          // F4b: the decal when a Turn / Size slider was grabbed
    float      m_decal_turn_ui = 0.f;     // relative to that start, reset on release
    float      m_decal_size_ui = 100.f;
    bool show_decal_cards_while_brushing() const override { return m_paint_mode == PaintMode::Brush && m_show_decals_in_brush; }
    int       m_selected_decal = -1;   // decal row picked in the Vector panel's layer list
    float     m_colour_row_w   = 0.f;  // measured width of one SVG colour row (0 until drawn once)
    void set_paint_mode(PaintMode mode);
    void render_rectangle_mask_option();
    void render_vector_panel();
    // One SVG colour row: its colour, the filament it paints with (combo) and its eye.
    // Returns true when the filament or the eye changed.
    bool render_svg_colour_row(int id, uint32_t rgb, bool grad, int &state, bool &hidden);
    // NEOTKO_SVGDECAL_TAG — parts of the SVG on screen: pick on the canvas, the selection's filament
    // and eye, and the list. True when the paint groups changed (masks to rebuild).
    bool render_svg_parts();
    void render_mirror_centre_controls();
    // Filament combo with a first entry "Colour" (state -1 = follow the colour of the part).
    bool render_part_filament_combo(const char *id, int &state);
    int  m_svg_parts_filter = 0;   // list shows: 0 = changed and selected, 1 = all

    // NEOTKO_PROFILE_TAG_START — s235 F5b: el sandwich VISIBLE dentro del gizmo de MMU.
    // docs/FUTURE/MMU_SANDWICH_COEXISTENCE_PLAN.md §3 F5b. El bloqueo que documentaba el plan
    // (GLCanvas3D apaga el GLVolume con `is_active=false` cuando el gizmo es MmSegmentation,
    // así que el preview de s233 que vive en GLVolume::render no se dibuja) NO se toca: en vez
    // de reencender el volumen — que además reintroduciría la malla que el gizmo ya dibuja —
    // el propio gizmo dibuja el preview, con el MISMO patrón que GLGizmoColorStitchPainter::
    // render_marked_paint(): TriangleSelectorPatch + mm_gouraud. Eso resuelve de paso el
    // riesgo (1) del plan (¿qué shader dibuja?): mm_gouraud es donde nacieron las uniforms
    // u_weave_*, así que el tejido se ve sin portar nada. El picking (riesgo 2) no se toca:
    // esto sólo vive en el render.
    void rebuild_sandwich_preview_if_dirty();
    bool render_sandwich_preview();          // devuelve true si dibujó algo
    std::vector<std::unique_ptr<TriangleSelectorPatch>> m_sw_preview_sel;
    uint64_t m_sw_paint_key   = 0;           // ColorStitchPaintPreview::overlap_key del objeto
    uint64_t m_sw_color_key   = 0;           // ...::context_key (color/TD/perfiles)
    int      m_sw_built_oid    = -2;         // objeto para el que se construyó (-2 = nada)
    bool     m_sw_show         = true;       // toggle de UI (app_config neotko_mmu_show_sandwich)

    // s235 F5a — el aviso inverso al de s234: DENTRO de lo pintado de sandwich hay una zona
    // que no llevará efecto porque ahí manda el MMU.
    void render_coexist_warning();
    ColorStitchPaintPreview::CoexistOverlap m_coexist{};
    uint64_t m_coexist_key    = 0;
    int      m_coexist_oid    = -2;
    // NEOTKO_PROFILE_TAG_END

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r


#endif // slic3r_GLGizmoMmuSegmentation_hpp_
