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
    // s231b — `owner`: objeto dueño del volumen. Por defecto (nullptr) es el objeto
    // ACTIVO, que es lo que asumía todo este bloque; los selectores de preview de los
    // objetos marcados NO activos necesitan pasar el suyo para que el fondo contra el
    // que se compone el color sea el de SU objeto y no el del que esté activo.
    std::vector<ColorRGBA> build_ebt_colors_for_volume(const ModelVolume* mv,
                                                       const ModelObject* owner = nullptr) const;

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
                               const TriangleSelectorPatch* sel = nullptr,
                               const ModelObject* owner = nullptr) const;

    // NEOTKO_COLORSTITCH_TAG — per-ISLAND weave. Splits each painted slot into connected
    // components (islands = the flat top zones / stair steps, since painting is top-only)
    // and builds one WeaveParams per island scaled to ITS own projected extent at the real
    // top line width — so each zone previews like the slice instead of one shared field.
    // Fills `facet_weave_idx` (facet → weave_list index) + `weave_list`. Needs the live `sel`.
    void build_ebt_weave_islands_for_volume(const ModelVolume* mv,
                                            const TriangleSelectorPatch* sel,
                                            std::unordered_map<int,int>& facet_weave_idx,
                                            std::vector<TriangleSelectorPatch::WeaveParams>& weave_list,
                                            const ModelObject* owner = nullptr) const;
    bool         m_weave_preview        = true;   // UI toggle "Preview weave"
    mutable bool m_weave_any_auto_angle = false;  // drives the pre-slice angle notice

    // NEOTKO_COLORSTITCH_TAG_START — PR.2 (COLORSTITCH_PAINTER_REVAMP_PLAN.md):
    // estilo-paletas generadas por el dispatcher ColorSci::build_palette y
    // cacheadas. Se regeneran SOLO cuando cambia el contexto (filamentos / TD /
    // layer height / tools del gradient), no cada frame.
    void gizmo_materials(Slic3r::ColorSci::Material out[4],
                         std::vector<std::string>& fcolors_out) const;
    // NEOTKO_SANDWICH_TAG — Fase 2 (s167 plan): active object's already-assigned
    // base colour (physical tool or MixedFilament approximation), for composing
    // previews against what the object will actually print in instead of a
    // hardcoded black background. Returns false (bg untouched) when there's no
    // selected object or its extruder can't be resolved — callers keep black.
    // s231b — `mo_override`: ver la nota de build_ebt_colors_for_volume (por defecto,
    // el objeto activo).
    bool resolve_object_base_bg(const Slic3r::ColorSci::Material mats[4],
                                float bg_rgb[3],
                                const ModelObject* mo_override = nullptr) const;
    // s169 F3 — ¿el objeto activo tiene "MixedFilament Object" en modo ON? Gate
    // compartido: on_render_input_window lo usa para deshabilitar+banner en
    // Paint/Palette/Pro, y on_set_state para auto-abrir el departamento Object.
    bool active_object_mixed_filament_mode() const;
    void rebuild_palettes_if_stale();
    void render_palette_panel(float window_width);
    // Opción B del revamp — bandeja "pro mode" inline: compone Top/Penu (passes
    // Solid) + TD y produce un ColorRecipe como color activo de pintura.
    void render_pro_mode_panel();
    // s169 F1 — header persistente sobre el selector de departamentos: swatch
    // Active re-predicho en vivo (F2 añade "+ New" + nombre editable + Pin).
    void render_header();
    // s169 F1 — rejilla full-width de paletas guardadas (reemplaza la columna
    // vertical de render_left_rail, retirada junto al layout de 2 columnas).
    void render_paint_palette_grid();
    // s169 F3 — departamento Object: toggle "MixedFilament Object" + swatch
    // (movido desde Pro) + live recipe (pases resueltos por el último apply).
    void render_object_department();
    void render_group_selector();                    // s137b: fila full-width del selector de grupo
    // s231 F6 — inventario "en uso en ESTE objeto": los slots son el recurso escaso y
    // hasta ahora sólo se veían en el tooltip de un swatch o en el volcado del
    // eyedropper AL LOG. Fila por slot ocupado: swatch · nombre · [Use] [Erase].
    void render_slots_in_use();
    // s231 F6 — el pincel es GLOBAL (se pinta con cualquier departamento abierto,
    // ver nota en on_render_input_window) pero sus dos parámetros — ángulo de
    // smart-fill y plano de corte — vivían SÓLO dentro de Palette. Extraídos aquí
    // para dibujarlos una vez, fuera del switch de departamentos.
    void render_brush_and_view(float sliders_left_width, float sliders_width,
                               float drag_left_width, float slider_icon_width);
    // s231 F1 — ¿el objeto activo tiene el pintado gobernado por MixedFilament? Gate
    // compartido por la UI (banner + disabled) y por el CANVAS (on_mouse): sin el
    // segundo, deshabilitar el panel sólo apagaba los botones y se seguía pintando en
    // el 3D un objeto cuyo pintado el motor ignora.
    bool painting_blocked() const;
    // s169 F0 — rejilla (TD) por filamento, extraída de render_pro_mode_panel para
    // que Create/Object (F1/F3) puedan reusarla bajo su propio wrapper (Collapsing
    // Header / card). Self-contained: lee fcolors/nfil por su cuenta.
    void render_td_grid();
    // s169 F0 — color-resultado predicho en vivo (top+penu compuestos contra el
    // fondo real del objeto activo), extraído del lambda local de render_left_rail
    // para que el header/Object (F2/F3) puedan predecir un swatch también.
    uint32_t predict_argb_for(const Slic3r::SurfacePassStack& top,
                              const Slic3r::SurfacePassStack& penu) const;

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

    // s231 F4 — DUPLICAR. El Pro reescribe EN VIVO el perfil enlazado (s118), así que
    // "cojo un color guardado y lo varío" destruía el original: no había forma de
    // derivar sin romper. Estas dos crean una COPIA independiente y la dejan enlazada,
    // de modo que la edición posterior en Pro cae sobre la copia.
    //  · duplicate_profile: clona un perfil de la biblioteca (guardado, mismo grupo)
    //    y devuelve el id nuevo (0 si no se pudo).
    //  · duplicate_active_as_new: clona el COLOR ACTIVO (venga de donde venga: tira
    //    generada, perfil guardado o edición Pro sin guardar) a un perfil guardado
    //    nuevo, lo enlaza y salta a Pro.
    int  duplicate_profile(int pid);
    void duplicate_active_as_new();
    // s231 F0 — punto ÚNICO de invalidación del estado de pintura. El estado activo
    // vive repartido en 4 campos (recipe / profile_id / slot / resolved) que hasta
    // ahora se sincronizaban a mano en 8 sitios, y cada uno olvidaba uno distinto —
    // de ahí el bug s209 ("el swatch sigue activo pero el click no pinta"): la
    // reconstrucción del selector reseteaba slot+id y dejaba `resolved` a true, así
    // que ensure_active_slot salía por la puerta rápida devolviendo 0 = NONE.
    // `keep_recipe` conserva la INTENCIÓN de color (lo que el usuario ve en el
    // swatch) y sólo tira el enlace/slot, que se re-materializan al pintar.
    void invalidate_active_binding(bool keep_recipe);

    std::vector<Slic3r::ColorSci::ColorRecipe> m_pal_flat;
    std::vector<Slic3r::ColorSci::ColorRecipe> m_pal_gradient;
    // s171 — reemplaza a "Mixed (ColorStitch)" (nukeado: penu-dither+top-solid,
    // siempre salía amarillo en el preview TD por venir de una ruta muerta desde
    // s120). "ColorStitch Pattern Color": 2 pasadas (Top + Penu, ambas a ratio
    // 1.0 = sobrepuestas, no split de Z) con los MISMOS 2 tools y el MISMO ángulo
    // (auto en ambas), barriendo el mix ColorStitch (pct_b) de 0% a 100% — solo
    // ColorStitch, cero Solid/PathBlend.
    std::vector<Slic3r::ColorSci::ColorRecipe> m_pal_cs_gradient;
    std::string m_pal_key;          // firma del contexto con el que se generaron
    int  m_grad_tool_a = 0;         // A/B del gradient (selección UI: fase posterior)
    int  m_grad_tool_b = 1;
    int  m_cs_tool_a    = 0;        // A/B del ColorStitch Pattern Color (s171)
    int  m_cs_tool_b    = 1;
    // s231 F5 — qué swatch de qué tira GENERADA está activo, para poder marcarlo
    // (draw_palette_strip sólo conocía hover: elegir un color del Generator no dejaba
    // ninguna marca en ninguna parte y el usuario perdía de vista qué estaba usando).
    // kind: 0 = ninguno, 1 = Gradient ramp, 2 = Flat, 3 = ColorStitch Pattern.
    int  m_active_pal_kind = 0;
    int  m_active_pal_idx  = -1;

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
    // s231 F3 — el write-back live del Pro agendaba un re-slice (y un rebuild completo
    // del weave, con union-find sobre las facetas pintadas) CADA FRAME mientras se
    // arrastraba una ratio-bar o un DragFloat: el json cambia en cada frame del drag.
    // Mismo criterio que ya usaba el TD (IsItemDeactivatedAfterEdit): mientras haya un
    // widget activo se acumula aquí y se dispara UNA vez al soltar.
    bool                          m_pro_reslice_pending = false;
    // NEOTKO_BOTTOM_TAG — s230: el discriminador de superficie Top/Bottom del panel Pro
    // se retiró (las tres zonas se editan seguidas en un solo sitio), así que
    // m_pro_surface_mode desapareció con él.
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
    // s231 F6 — un grupo se DERIVA del sufijo de los nombres (cs_parse_group), así que
    // un grupo recién creado y todavía vacío no existía para el combo: "+ New group" y
    // el grupo desaparecía al redibujar. Este hint mantiene vivo el techo durante la
    // sesión (no persiste: un grupo vacío sigue sin guardarse en el proyecto, que es lo
    // correcto — sólo deja de evaporarse mientras lo estás llenando).
    int  m_max_group_hint     = 1;

    // s111 — Select mode: set multi-objeto pintable. NEOTKO_COLORSTITCH_TAG.
    bool             m_prev_on     = false;          // estado On previo (m_old_state es privado en la base)
    bool             m_select_mode = false;          // herramienta Select activa
    std::set<int>    m_marked_objects;               // object_idx marcados (set pintable)
    // s231 F6 — las 5 herramientas son mutuamente excluyentes, pero la exclusión se
    // reconstruía A MANO en 5 sitios (cada botón apagaba los otros por su cuenta), lo
    // que ya produjo el bug "Paint y Sticker encendidos a la vez". Un enum + un setter
    // único: añadir una herramienta nueva ya no puede olvidarse de apagar otra.
    enum Tool { TOOL_PAINT = 0, TOOL_SELECT, TOOL_ERASER, TOOL_PICK, TOOL_STICKER };
    void set_tool(Tool t);
    Tool current_tool() const;
    void set_tool_mode(bool select, bool erase);     // wrapper legacy sobre set_tool
    void toggle_mark(int object_idx, bool unmark);   // marcar/activar (o desmarcar)
    void switch_active_object(int object_idx);       // activar objeto + RE-APLICAR el color
    void render_tool_row();                          // fila [Select][Paint][Eraser][Pick]

    // s173 — iconos reales (Fable) para la toolbar: 4 variantes por icono
    // (light/dark × normal/hover), cargadas UNA vez. Mapa PRIVADO de este painter
    // (no toca GLGizmosManager::MENU_ICON_NAME, compartido por todos los gizmos —
    // decisión del plan de beauty-up, más barato y sin blast radius ajeno).
    struct ToolIconSet {
        void* normal      = nullptr;
        void* normal_dark = nullptr;
        void* hover       = nullptr;
        void* hover_dark  = nullptr;
    };
    ToolIconSet m_icon_select, m_icon_paint, m_icon_eraser, m_icon_pick, m_icon_erase_all;
    bool        m_tool_icons_loaded = false;
    void        ensure_tool_icons_loaded();
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
    // s232 — `picked_mesh_id` (índice de volumen model_part del raycast, -1 = no se
    // sabe): los slots son POR VOLUMEN, así que en un objeto multivolumen (lo que deja
    // un Assemble) el número de slot solo no identifica un perfil — el slot 1 del
    // volumen A y el del B son colores distintos. Sin este dato el eyedropper cogía el
    // primer volumen que tuviera ese número.
    void pick_recipe_from_object(int object_idx, int picked_slot, int picked_mesh_id = -1);

    // NEOTKO_STICKER_TAG — herramienta Sticker: coloca un SVG de 1 color como
    // pegatina plana sobre una cara (sin deformar el mesh), con una receta
    // Sandwich propia (ver ColorMixSticker en Model.hpp + Fill.cpp Fase 6c v2).
    // v1: sin arrastre/rotación/escala en viewport — la pegatina queda plana
    // en el plano XY del frame-objeto, en la posición del click; ajustar
    // tamaño/rotación desde el propio SVG de origen. Pulido pendiente.
    bool        m_sticker_mode = false;
    std::string m_pending_sticker_svg;    // SVG cargado, aún sin colocar
    std::string m_pending_sticker_name;   // nombre a mostrar (stem del fichero)
    bool load_sticker_svg_dialog();       // abre file picker, rellena m_pending_sticker_*
    void place_sticker_at(const ModelVolume* mv, const Vec3f& hit_local);
    void render_sticker_section();        // sección "Stickers (SVG)" en Palette

    // NEOTKO_STICKER_TAG — modo edición (mover/rotar) de un sticker YA colocado,
    // activado desde "Edit placement" en la lista de Palette. -1 = ninguno.
    // v1.5: solo traslación (arrastre por raycast, sin realinear a la normal —
    // el propio feature asume superficies top planas, ver plan §3.2) + rotación
    // Z vía slider (no anillo 3D: GLGizmoRotate está acoplado a Selection/
    // GLVolume, que un sticker no tiene — replicarlo suelto sería una pieza de
    // interacción nueva y grande, desproporcionada frente al valor que añade
    // sobre un slider ya WYSIWYG contra el overlay coloreado).
    int   m_editing_sticker_idx = -1;
    // Ángulo Z (grados) del sticker en edición, decodificado de su transform
    // (atan2 del bloque 2×2 superior-izquierdo) al entrar en edición — válido
    // porque nosotros mismos garantizamos que sticker.transform es SIEMPRE
    // Translation*RotationZ (nunca introducimos tilt), así que la decodificación
    // es exacta, no una aproximación.
    float m_editing_spin_deg    = 0.f;
    // Escala uniforme del sticker en edición, decodificada como la norma de la
    // primera columna del bloque lineal 3x3 (invariante al ángulo: R*S tiene
    // columnas de longitud `scale` porque R es rotación pura de columnas
    // unitarias) — igual de exacta que la decodificación del ángulo de arriba.
    // transform SIEMPRE se reconstruye como Translation*RotationZ*Scaling(s),
    // nunca escala no-uniforme ni negativa (el slider clampa a positivo).
    float m_editing_scale       = 1.f;
    bool  m_sticker_dragging    = false;
    void enter_sticker_edit(int idx);
    void exit_sticker_edit();

    // Overlay GL (contorno + relleno con el color del profile) del sticker en
    // edición. Geometría construida en frame LOCAL del sticker (z=0, sin
    // colocación) una vez al entrar en edición; mover/rotar solo cambia el
    // uniform view_model_matrix en cada frame — mismo patrón que
    // GLGizmoFlatten (plano coloreado sobre una cara, sin rehacer geometría).
    GLModel m_sticker_overlay_fill;
    GLModel m_sticker_overlay_outline;
    int     m_sticker_overlay_built_for = -1;   // idx para el que se construyó
    void ensure_sticker_overlay_built(const ColorMixSticker& sticker);
    void render_sticker_edit_overlay();

    // s111 — preview de pintura para los objetos marcados NO-activos: el painter
    // base solo dibuja el objeto activo, así que para ver TODOS los marcados con su
    // color construimos selectors de solo-lectura por objeto (cacheados) y los
    // renderizamos con la malla + trafo de cada uno.
    std::map<int, std::vector<std::unique_ptr<TriangleSelectorGUI>>> m_preview_sel;
    bool             m_preview_dirty = true;         // rebuild lazy al cambiar marcas/pintura/activo
    void rebuild_preview_selectors_if_dirty();
    void render_marked_paint();                      // dibuja la pintura de los marcados no-activos

    // ------------------------------------------------------------------
    // s232 — REALCE EN VIEWPORT DEL SLOT ACTIVO (AID visual, mismo lenguaje
    // que el de Align&Stack s227: geometría con el shader `flat` + chapas
    // billboard en el ForegroundDrawList de ImGui; CERO cambios en el render
    // del patch ni en los shaders — la lección de s229 es que tocar shaders
    // aquí cuesta compilaciones enteras).
    //
    // Responde a "¿qué color estoy usando y DÓNDE está aplicado?": dibuja el
    // CONTORNO de las caras del slot resaltado (borde de la región pintada,
    // no su malla entera) + la caja AABB de cada isla + una chapa con el
    // número de slot. El slot resaltado es el que esté bajo el cursor en el
    // panel (hover de un swatch / de una fila del inventario) y, si no hay
    // hover, el ACTIVO — así el realce es a la vez "dónde está esto" y
    // "dónde estoy pintando".
    // ------------------------------------------------------------------
    struct SlotHighlightPart {
        // Por puntero: GLModel libera sus VBOs en el destructor y no declara
        // copia/movimiento propios, así que un realojo del vector duplicaría los
        // ids y los liberaría dos veces.
        // s232 — DOS modelos, no uno: el contorno se pinta con el color de su ZONA
        // (verde arriba, naranja abajo — los mismos `cs_zone_*` que las chapas del
        // panel Pro), y eso exige separar las aristas por orientación de la cara que
        // las posee, porque el color es un uniform por render.
        std::unique_ptr<GLModel>   edges_top;                         // contorno (caras hacia arriba), frame LOCAL
        std::unique_ptr<GLModel>   edges_bottom;                      // contorno (caras hacia abajo)
        Transform3d                trafo{Transform3d::Identity()};    // local → mundo
        std::vector<BoundingBoxf3> islands;                           // AABB por isla, en MUNDO
        std::vector<int>           island_facets;                     // paralelo a islands
        // s232 — zona dominante de la isla: una receta con Top y Bottom pinta las dos
        // caras, y sin distinguirlas el realce no dice que el color está aplicado
        // ARRIBA y ABAJO (chapa por encima vs por debajo + marca).
        std::vector<char>          island_bottom;                     // paralelo a islands
    };
    std::vector<SlotHighlightPart> m_hl_parts;
    GLModel     m_hl_island_box;                   // cubo unitario reusado para las AABB
    ColorRGBA   m_hl_color{1.f, 1.f, 1.f, 1.f};    // color compuesto del slot resaltado
    bool        m_slot_highlight = true;           // toggle de UI (Brush & view)
    bool        m_hl_dirty       = true;
    int         m_hl_slot_built  = -1;             // slot con el que se construyó m_hl_parts
    int         m_hl_facets      = 0;              // caras totales resaltadas
    // Hover del panel: se acumula durante on_render_input_window (m_hover_slot_next)
    // y se publica al final del mismo, porque el panel se dibuja DESPUÉS del gizmo
    // en el frame — el realce entra en el frame siguiente (con set_as_dirty).
    int         m_hover_slot      = 0;
    int         m_hover_slot_next = 0;
    // NEOTKO_MMU_COEXIST_TAG s235 F5a — solape con la pintura de MMU del objeto activo,
    // cacheado: recalcularlo es un deserialize de las dos pinturas y el panel se dibuja cada
    // frame. La clave son los timestamps de las dos pinturas (ColorMixPaintPreview::overlap_key).
    ColorMixPaintPreview::CoexistOverlap m_coexist{};
    uint64_t    m_coexist_key     = 0;
    int         m_coexist_oid     = -2;
    void hover_slot(int slot) { if (slot > 0) m_hover_slot_next = slot; }
    int  highlight_slot() const;
    void rebuild_slot_highlight_if_dirty();
    void render_slot_highlight();

public:
    // Consultas para el render del canvas (GLCanvas3D::_render_objects):
    bool object_is_marked(int object_idx) const { return m_marked_objects.count(object_idx) > 0; }
    bool select_tool_active()             const { return m_select_mode; }
    bool has_marked()                     const { return !m_marked_objects.empty(); }
private:
    // s169 F1 — departamento activo del panel (0=Paint, 1=Create, 2=Pro, 3=Object).
    // Sustituye el layout de 2 columnas (rail+cuerpo) por un selector segmentado;
    // m_panel_col_h (altura medida del frame anterior para las 2 columnas) muere
    // con él — F1 ya no lo necesita.
    int m_department          = 0;

    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoColorMixPainter_hpp_
// NEOTKO_PROFILE_TAG_END
