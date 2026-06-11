// NEOTKO_PROFILE_TAG_START — Opción 4 Fase B (3D Painter for SurfaceEffectProfile)
// B2: standalone painter subclass of GLGizmoPainterBase (templated on FuzzySkin
// rather than MMU — simpler base, no extruder coupling). Paints slot 0..15 onto
// ModelVolume::color_mix_paint_facets; slot→profile mapping lives in
// ModelVolume::colormix_slot_to_profile_id[].
#ifndef slic3r_GLGizmoColorMixPainter_hpp_
#define slic3r_GLGizmoColorMixPainter_hpp_

#include "GLGizmoPainterBase.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/ColorSci/ColorPredict.hpp"   // NEOTKO_COLORSTITCH_TAG — PR.2 palette strips
#include <map>
#include <string>
#include <vector>

namespace Slic3r::GUI {

class GLGizmoColorMixPainter : public GLGizmoPainterBase
{
public:
    GLGizmoColorMixPainter(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

    // Max slots per painted volume — slot 0 is "unpainted", 1..15 are profile slots.
    static constexpr int MAX_SLOTS = 16;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name()       const override;
    bool        on_is_selectable()  const override;
    bool        on_is_activable()   const override;

    void show_tooltip_information(float caption_max, float x, float y);

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering ColorStitch Painter"); }
    std::string get_gizmo_leaving_text()  const override { return _u8L("Leaving ColorStitch Painter"); }
    std::string get_action_snapshot_name() const override { return _u8L("ColorStitch painting editing"); }

    // Painter state encoding: slot index 1..15 maps directly to EnforcerBlockerType(N).
    EnforcerBlockerType get_left_button_state_type()  const override;
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }

    wchar_t m_current_tool = 0;

private:
    bool on_init() override;

    void update_model_object()                       override;
    void update_from_model_object(bool first_update) override;

    void             on_opening()  override {}
    void             on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    // Resolve the slot index in the currently selected ModelObject for the chosen
    // profile id. Returns 0 if no profile is selected or if all 15 slots are taken
    // by other profiles. When `assign_if_missing` is true and a free slot exists,
    // assigns the profile to it (mutates the model) and returns the new slot.
    int slot_for_selected_profile(bool assign_if_missing);

    // Build per-volume ebt color palette from the slot→profile table + manager.
    std::vector<ColorRGBA> build_ebt_colors_for_volume(const ModelVolume* mv) const;

    // NEOTKO_COLORSTITCH_TAG_START — PR.2 (COLORSTITCH_PAINTER_REVAMP_PLAN.md):
    // estilo-paletas generadas por el dispatcher ColorSci::build_palette y
    // cacheadas. Se regeneran SOLO cuando cambia el contexto (filamentos / TD /
    // layer height / tools del gradient), no cada frame.
    void gizmo_materials(Slic3r::ColorSci::Material out[4],
                         std::vector<std::string>& fcolors_out) const;
    void rebuild_palettes_if_stale();
    void render_palette_panel(float window_width);

    // PR.3 — modelo de dos capas (auto vs guardadas):
    //  · set_active_recipe: click en swatch = SOLO fija el color activo (no crea
    //    profile ni quema slot — navegar la paleta no contamina nada).
    //  · ensure_active_slot: materializa el slot+profile AUTO (dedup) la primera
    //    vez que se pinta de verdad con el color activo. Llamado desde
    //    get_left_button_state_type.
    //  · garbage_collect_auto_profiles: borra profiles auto sin slot que los use.
    //  · save_active_as_palette: promueve el color activo a paleta GUARDADA
    //    (auto_generated=false → aparece en la lista, ya no se hace GC).
    void set_active_recipe(const Slic3r::ColorSci::ColorRecipe& r,
                           const std::string& style);
    int  ensure_active_slot();
    void garbage_collect_auto_profiles();
    void save_active_as_palette();

    std::vector<Slic3r::ColorSci::ColorRecipe> m_pal_flat;
    std::vector<Slic3r::ColorSci::ColorRecipe> m_pal_mixed;
    std::vector<Slic3r::ColorSci::ColorRecipe> m_pal_gradient;
    std::string m_pal_key;          // firma del contexto con el que se generaron
    int  m_grad_tool_a = 0;         // A/B del gradient (selección UI: fase posterior)
    int  m_grad_tool_b = 1;

    // Color activo de pintura (capa "auto"). Sin slot hasta que se pinta.
    Slic3r::ColorSci::ColorRecipe m_active_recipe;
    std::string                   m_active_style;       // para el nombre al materializar/guardar
    bool                          m_has_active_recipe = false;
    bool                          m_active_resolved   = false;  // slot ya materializado
    // NEOTKO_COLORSTITCH_TAG_END
    // Refresh all triangle-selector palettes after profile/slot table changes.
    void refresh_selector_palettes();

    // Currently chosen SurfaceEffectProfile id from the manager (0 = none).
    int m_selected_profile_id = 0;
    // Resolved slot 1..15 for the selected profile (0 if unresolved / no profile).
    int m_active_slot         = 0;
    // When true, left-click erases instead of painting (UI checkbox in panel).
    bool m_erase_mode         = false;

    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoColorMixPainter_hpp_
// NEOTKO_PROFILE_TAG_END
