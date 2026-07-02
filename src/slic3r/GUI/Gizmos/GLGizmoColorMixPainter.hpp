// NEOTKO_PROFILE_TAG_START — Opción 4 Fase B (3D Painter for SurfaceEffectProfile)
// B2: standalone painter subclass of GLGizmoPainterBase (templated on FuzzySkin
// rather than MMU — simpler base, no extruder coupling). Paints slot 0..15 onto
// ModelVolume::color_mix_paint_facets; slot→profile mapping lives in
// ModelVolume::colormix_slot_to_profile_id[].
#ifndef slic3r_GLGizmoColorMixPainter_hpp_
#define slic3r_GLGizmoColorMixPainter_hpp_

#include "GLGizmoPainterBase.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GLModel.hpp"                 // NEOTKO_COLORSTITCH_TAG s111 — caja-marca ×1.15
#include "libslic3r/ColorSci/ColorPredict.hpp"   // NEOTKO_COLORSTITCH_TAG — PR.2 palette strips
#include <map>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::GUI {

class GLGizmoColorMixPainter : public GLGizmoPainterBase
{
public:
    GLGizmoColorMixPainter(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

    // Max slots per painted volume — slot 0 is "unpainted", 1..MAX_SLOTS-1 are
    // profile slots. Must match ModelVolume::COLORMIX_SLOT_COUNT (static_assert in .cpp).
    // NEOTKO_COLORSTITCH_TAG — s137: 255 (EnforcerBlockerType::ExtruderMax sentinel),
    // so up to 254 distinct profiles can be painted on one volume. Encoding already
    // supports it (2-bit prefix + base-15 nibbles); this was a deliberate UI cap.
    static constexpr int MAX_SLOTS = 255;
    // NEOTKO_COLORSTITCH_TAG — s137: palette groups. Project-level library (mgr) is
    // bucketed into 1..MAX_GROUPS via a name suffix (see cs_*_group helpers in .cpp).
    static constexpr int MAX_GROUPS = 10;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name()       const override;
    bool        on_is_selectable()  const override;
    bool        on_is_activable()   const override;
    // s111 — sin InstancesHider: el painter ya NO aísla (oculta) los demás
    // objetos; el modo Select necesita verlos y clicarlos.
    CommonGizmosDataID on_get_requirements() const override;

    void show_tooltip_information(float caption_max, float x, float y);

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering ColorStitch Painter"); }
    std::string get_gizmo_leaving_text()  const override { return _u8L("Leaving ColorStitch Painter"); }
    std::string get_action_snapshot_name() const override { return _u8L("ColorStitch painting editing"); }

    // Painter state encoding: slot index 1..15 maps directly to EnforcerBlockerType(N).
    EnforcerBlockerType get_left_button_state_type()  const override;
    // NEOTKO_COLORSTITCH_TAG — s118: el botón derecho NO pinta/borra aquí (el
    // borrado es Shift+Izquierdo / modo Eraser). Devolver NONE hacía que la base
    // (GLGizmoPainterBase::gizmo_event :669) lo tratara como trazo de borrado, así
    // que panear la cámara con el derecho borraba el slot del objeto bajo el cursor.
    // El centinela EnforcerBlockerType(-1) es el "este botón no hace nada" que la
    // base reconoce (665/669) → el derecho cae al canvas = pan/rotación de cámara.
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

    // s111 — modo "Select": intercepta clics para marcar/activar objetos en vez
    // de pintar. Solo en este modo se re-activa el picking de la escena.
    bool on_mouse(const wxMouseEvent& mouse_event) override;

    wchar_t m_current_tool = 0;

private:
    bool on_init() override;

    void update_model_object()                       override;
    void update_from_model_object(bool first_update) override;

    void             on_opening()  override {}
    void             on_shutdown() override;
    // s111 — al abrir: sembrar el set marcado desde la selección previa + activo.
    void             on_set_state() override;
    PainterGizmoType get_painter_type() const override;

    // Resolve the slot index in the currently selected ModelObject for the chosen
    // profile id. Returns 0 if no profile is selected or if all 15 slots are taken
    // by other profiles. When `assign_if_missing` is true and a free slot exists,
    // assigns the profile to it (mutates the model) and returns the new slot.
    int slot_for_selected_profile(bool assign_if_missing);

    // Build per-volume ebt color palette from the slot→profile table + manager.
    std::vector<ColorRGBA> build_ebt_colors_for_volume(const ModelVolume* mv) const;

    // NEOTKO_COLORSTITCH_TAG — per-slot weave preview data (parallel to the ebt
    // colours). For each slot whose profile carries a ColorStitch Top pass, builds
    // the stripe sequence (tool colours) + angle so the painted patch shows the
    // woven effect. Off (flat) when m_weave_preview is disabled or the profile is
    // not ColorStitch. Sets m_weave_any_auto_angle when a slot uses an auto angle.
    // `sel` (the live painted selector for this volume) lets the weave span the
    // PAINTED AREA per slot (projected extent of that slot's facets), not the whole
    // object AABB — so the effect/pattern fits the selection. Null/empty ⇒ AABB fallback.
    std::vector<TriangleSelectorPatch::WeaveParams>
    build_ebt_weave_for_volume(const ModelVolume* mv,
                               const TriangleSelectorPatch* sel = nullptr) const;

    // NEOTKO_COLORSTITCH_TAG — per-ISLAND weave. Splits each painted slot into connected
    // components (islands = the flat top zones / stair steps, since painting is top-only)
    // and builds one WeaveParams per island scaled to ITS own projected extent at the real
    // top line width — so each zone previews like the slice instead of one shared field.
    // Fills `facet_weave_idx` (facet → weave_list index) + `weave_list`. Needs the live `sel`.
    void build_ebt_weave_islands_for_volume(const ModelVolume* mv,
                                            const TriangleSelectorPatch* sel,
                                            std::unordered_map<int,int>& facet_weave_idx,
                                            std::vector<TriangleSelectorPatch::WeaveParams>& weave_list) const;
    bool         m_weave_preview        = true;   // UI toggle "Preview weave"
    mutable bool m_weave_any_auto_angle = false;  // drives the pre-slice angle notice

    // NEOTKO_COLORSTITCH_TAG_START — PR.2 (COLORSTITCH_PAINTER_REVAMP_PLAN.md):
    // estilo-paletas generadas por el dispatcher ColorSci::build_palette y
    // cacheadas. Se regeneran SOLO cuando cambia el contexto (filamentos / TD /
    // layer height / tools del gradient), no cada frame.
    void gizmo_materials(Slic3r::ColorSci::Material out[4],
                         std::vector<std::string>& fcolors_out) const;
    void rebuild_palettes_if_stale();
    void render_palette_panel(float window_width);
    // Opción B del revamp — bandeja "pro mode" inline: compone Top/Penu (passes
    // Solid) + TD y produce un ColorRecipe como color activo de pintura.
    void render_pro_mode_panel();
    // s111 — carril izquierdo del panel: swatch del color Activo + biblioteca
    // de paletas guardadas como columna vertical con scroll.
    void render_left_rail(float rail_w, float rail_h);
    void render_group_selector();                    // s137b: fila full-width del selector de grupo

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
    // s112 — Pro mode dual: cargar una receta (paleta, perfil o sandwich base del
    // objeto) en los editores de zona Pro (m_pro_top/penu) para que el panel Pro
    // SIEMPRE refleje el color activo, no sea solo un creador de custom.
    void load_recipe_into_pro(const Slic3r::ColorSci::ColorRecipe& r);
    int  ensure_active_slot();
    void garbage_collect_auto_profiles();
    void save_active_as_palette();
    // s140 — SAVE ALL: promueve TODOS los colores de trabajo no guardados
    // (auto_generated) al grupo activo de golpe. Deja "Remove all" libre para
    // borrar lo efímero sin perder trabajo. Devuelve cuántos promovió.
    int  save_all_palettes();
    // ¿Hay algún color de trabajo sin guardar? (gobierna la visibilidad del botón.)
    bool has_unsaved_palettes() const;

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

    // Pro mode — estado editable de la bandeja inline (passes Solid + TD). El
    // resultado se compone con sandwich_colour_stacked y se vuelca a la receta
    // activa vía set_active_recipe. ColorMix/PathBlend quedan en el editor completo.
    Slic3r::SurfacePassStack      m_pro_top;
    Slic3r::SurfacePassStack      m_pro_penu;
    bool                          m_pro_seeded = false;   // sembrado lazy (1 pass Solid en Top)
    // NEOTKO_COLORSTITCH_TAG_END
    // NEOTKO_BOTTOM_TAG — Fase 0 (WIP): "Bottom WIP" zone of the Pro tray. Mirrors
    // m_pro_top/penu but persists to SurfaceEffectProfile::stack_bottom_json. The
    // slice engine does NOT consume it yet (Fase 0 = graphic control + instrumentation
    // only). m_pro_bottom_loaded_id guards re-loading the stack when the selected
    // profile changes (so editing one profile doesn't bleed into another).
    Slic3r::SurfacePassStack      m_pro_bottom;
    int                           m_pro_bottom_loaded_id = -1;
    // NEOTKO_BOTTOM_TAG — Pro mode surface discriminator: 0 = Top (shows Top+Penu),
    // 1 = Bottom (shows Bottom + supported control). VIEW toggle only — all three
    // stacks persist regardless, so switching never discards authored work. Opens Top.
    int                           m_pro_surface_mode = 0;
    // Refresh all triangle-selector palettes after profile/slot table changes.
    void refresh_selector_palettes();

    // Currently chosen SurfaceEffectProfile id from the manager (0 = none).
    int m_selected_profile_id = 0;
    // Resolved slot 1..15 for the selected profile (0 if unresolved / no profile).
    int m_active_slot         = 0;
    // When true, left-click erases instead of painting (UI checkbox in panel).
    bool m_erase_mode         = false;
    // NEOTKO_COLORSTITCH_TAG — s137: active palette group (1..MAX_GROUPS). Pin to
    // palette / Save tags the new profile with this group; the Profiles list filters
    // by it. Project-level navigation only — does NOT partition the per-volume slots.
    int  m_active_group       = 1;

    // s111 — Select mode: set multi-objeto pintable. NEOTKO_COLORSTITCH_TAG.
    bool             m_prev_on     = false;          // estado On previo (m_old_state es privado en la base)
    bool             m_select_mode = false;          // herramienta Select activa
    std::set<int>    m_marked_objects;               // object_idx marcados (set pintable)
    void set_tool_mode(bool select, bool erase);     // mutuamente excluyentes
    void toggle_mark(int object_idx, bool unmark);   // marcar/activar (o desmarcar)
    void switch_active_object(int object_idx);       // activar objeto + RE-APLICAR el color
    void render_tool_row();                          // fila [Select][bucket][Eraser][Pick]
    // NEOTKO_NEOTOWER_TAG — al pintar, promociona el tipo de torre a NeoTower en el preset
    // de impresión (one-shot, no-op si ya está) para que la UI refleje el planificador real.
    void ensure_neotower_tower_type();

    // NEOTKO_COLORSTITCH_TAG — s118 (eyedropper): leer la receta de un objeto y
    // ENLAZARLA (mismo camino que un swatch guardado: bind id + load Pro). Además
    // vuelca a PROFILE un dump de TODO lo que tiene el objeto: slots pintados
    // (pid/nombre/cm/pb/top-penu-empty) + la base sandwich del preset (penu/colormix
    // enabled) → debug del "penu pintado no se honra sin penu activo en el main UX".
    bool m_pick_mode = false;                         // herramienta eyedropper
    // picked_slot = slot real de la faceta bajo el cursor (0 si sin pintar) → se
    // enlaza ese perfil; si 0/ inválido, cae al primer slot pintado del objeto.
    void pick_recipe_from_object(int object_idx, int picked_slot);

    // s111 — preview de pintura para los objetos marcados NO-activos: el painter
    // base solo dibuja el objeto activo, así que para ver TODOS los marcados con su
    // color construimos selectors de solo-lectura por objeto (cacheados) y los
    // renderizamos con la malla + trafo de cada uno.
    std::map<int, std::vector<std::unique_ptr<TriangleSelectorGUI>>> m_preview_sel;
    bool             m_preview_dirty = true;         // rebuild lazy al cambiar marcas/pintura/activo
    void rebuild_preview_selectors_if_dirty();
    void render_marked_paint();                      // dibuja la pintura de los marcados no-activos

public:
    // Consultas para el render del canvas (GLCanvas3D::_render_objects):
    bool object_is_marked(int object_idx) const { return m_marked_objects.count(object_idx) > 0; }
    bool select_tool_active()             const { return m_select_mode; }
    bool has_marked()                     const { return !m_marked_objects.empty(); }
private:
    // s111 — altura real del cuerpo del panel medida el frame anterior (los
    // children de ImGui 1.8x no auto-redimensionan en Y; converge en 1 frame).
    float m_panel_col_h       = 0.f;

    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoColorMixPainter_hpp_
// NEOTKO_PROFILE_TAG_END
