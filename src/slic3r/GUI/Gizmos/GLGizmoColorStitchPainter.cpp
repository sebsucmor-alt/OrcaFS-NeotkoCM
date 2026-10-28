// NEOTKO_PROFILE_TAG_START
#include <chrono>
#include "GLGizmoColorStitchPainter.hpp"
#include "GizmoNeotkoStyle.hpp"   // s318 — el lenguaje de panel compartido (Zonas de Soporte, s287)

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp" // NEOTKO_MIXEDFIL_SANDWICH_TAG — Print::mixed_filament_sandwich_profile_id
#include "libslic3r/ColorStitch.hpp" // NEOTKO_PROFILE_TAG — NeoDebug PROFILE channel
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/Utils.hpp" // s173 — resources_dir() para cargar los iconos de la toolbar
#include "libslic3r/ColorSci/StackFlatten.hpp" // NEOTKO_COLORSTITCH_TAG — sandwich_colour_stacked (pro mode live preview)
#include "libslic3r/ClipperUtils.hpp"      // NEOTKO_STICKER_TAG — union_ex para el overlay de edición
#include "libslic3r/Tesselate.hpp"         // NEOTKO_STICKER_TAG — triangulate_expolygons_2f para el relleno del overlay

#include "GLGizmoMeasure.hpp"             // s232 — TransformHelper::world_to_clip (chapas del realce)

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"          // s111 — render caja-marca
#include "slic3r/GUI/GLShader.hpp"        // s111 — GLShaderProgram (flat)
#include "libslic3r/Geometry.hpp"         // s111 — translation/scale_transform
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"          // s235 F5a — GUI::format para el aviso de solape MMU
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp" // NEOTKO_NEOTOWER_TAG — auto-promote tower type from the painter
#include "slic3r/GUI/ColorStitchPaintPreview.hpp"
#include "slic3r/GUI/ColorStitchStickerPreview.hpp" // s348 — stickers siempre visibles // s233 — colores de slot compartidos con la vista 3D normal
#include "slic3r/GUI/ColorStitchPatternLauncher.hpp" // NEOTKO_COLORSTITCH_TAG — botón ADV → editor de patrón
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>
#include <wx/filedlg.h> // NEOTKO_STICKER_TAG — Load SVG... file picker
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <fstream>   // NEOTKO_STICKER_TAG — read the chosen SVG file into memory
#include <sstream>   // NEOTKO_STICKER_TAG — ostringstream slurp
#include <boost/nowide/convert.hpp>
#include <boost/filesystem/path.hpp> // NEOTKO_STICKER_TAG — stem() for the sticker's display name
#ifdef NEOTKO_PRIVATE_TOWER
#include "libslic3r/neotko_private/StickerExt.hpp"
#endif

namespace Slic3r::GUI {

// NEOTKO_COLORSTITCH_TAG — s137: palette groups (SIMPLE / disposable, will be
// reworked with MixedFilament TD). A profile's group is encoded as an invisible
// suffix in its `name`: '\x1f' + 'g' + digit. This touches NO schema and NO 3mf
// (name already serializes), and old projects (no suffix) fall into Group 1.
// Strip for display, re-append on save/rename. Group 1 = no suffix (clean + back-compat).
namespace {
constexpr char CS_GROUP_SEP = '\x1f';   // unit separator — group marker
constexpr int  CS_MAX_GROUPS = GLGizmoColorStitchPainter::MAX_GROUPS;

int cs_parse_group(const std::string& name)
{
    const auto pos = name.rfind(CS_GROUP_SEP);
    if (pos == std::string::npos || pos + 2 >= name.size() || name[pos + 1] != 'g')
        return 1;
    const int g = std::atoi(name.c_str() + pos + 2);
    return (g >= 1 && g <= CS_MAX_GROUPS) ? g : 1;
}

std::string cs_strip_group(const std::string& name)
{
    const auto pos = name.rfind(CS_GROUP_SEP);
    return pos == std::string::npos ? name : name.substr(0, pos);
}

std::string cs_with_group(const std::string& name, int g)
{
    const std::string base = cs_strip_group(name);
    if (g <= 1) return base;                 // Group 1 = no suffix
    return base + CS_GROUP_SEP + 'g' + std::to_string(g);
}
} // namespace

// ----------------------------------------------------------------------------
// Boilerplate (mirror of FuzzySkin)
// ----------------------------------------------------------------------------

GLGizmoColorStitchPainter::GLGizmoColorStitchPainter(GLCanvas3D& parent,
                                               const std::string& icon_filename,
                                               unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
    , m_current_tool(ImGui::FillButtonIcon) // Smart fill — primary use case for ColorStitch Painter
{
}

std::string GLGizmoColorStitchPainter::on_get_name() const
{
    return _u8L("ColorStitch Painter") /*NEOTKO_COLORSTITCH_TAG*/;
}

// FFF only, no minimum-filament requirement — single-filament prints can still
// benefit from per-surface ColorStitch profiles.
bool GLGizmoColorStitchPainter::on_is_selectable() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
}

bool GLGizmoColorStitchPainter::on_is_activable() const
{
    // s111: se puede abrir con la selección VACÍA — el modo "Select" deja elegir
    // los objetos a pintar clicándolos en escena (igual que Align & Stack).
    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF)
        return false;
    // Cualquier estado de selección vale (vacía, uno, o varios): en modo Select
    // el usuario elige los objetos clicándolos.
    return true;
}

// s111 — quitamos InstancesHider de los requisitos: el painter ya no oculta los
// demás objetos (rompía el flujo "elegir objetos" del modo Select y hacía
// "desaparecer" todo lo no seleccionado). El resto de datos comunes se mantienen.
CommonGizmosDataID GLGizmoColorStitchPainter::on_get_requirements() const
{
    return CommonGizmosDataID(
                int(CommonGizmosDataID::SelectionInfo)
              | int(CommonGizmosDataID::Raycaster)
              | int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoColorStitchPainter::on_shutdown()
{
    garbage_collect_auto_profiles();   // PR.3: recoge colores de trabajo no usados
    m_select_mode = false;             // s111: salir de Select y limpiar marcas
    m_pick_mode   = false;             // s118: salir del eyedropper
    m_sticker_mode = false;            // NEOTKO_STICKER_TAG
    m_editing_sticker_idx = -1;        // NEOTKO_STICKER_TAG
    m_marked_objects.clear();
    m_hl_parts.clear();                // s232 — suelta los VBOs del realce al cerrar
    m_auto_parts.clear();              // s280d — ídem los del marcador de ángulo auto
    m_hover_slot = m_hover_slot_next = 0;
    m_hl_dirty   = true;
    m_auto_dirty = true;   // s280d
    m_parent.enable_picking(true);     // base lo hace también; explícito por claridad
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

// s111 — herramienta activa (Select / Bucket / Eraser, mutuamente excluyentes).
// El picking de la escena se mantiene SIEMPRE encendido (lo enciende on_set_state)
// porque hasta en modo pintar necesitamos saber qué objeto hay bajo el cursor
// para auto-activarlo y poder pintar en cualquier objeto del set marcado.
// NEOTKO_COLORSTITCH_TAG — s231 F6: setter ÚNICO de herramienta. Antes cada botón
// apagaba las otras cuatro por su cuenta (5 copias de la misma exclusión repartidas
// por el fichero), y ya se coló el bug "Paint y Sticker encendidos a la vez". Aquí
// la exclusión es estructural: se apaga TODO y se enciende una.
void GLGizmoColorStitchPainter::set_tool(Tool t)
{
    m_select_mode  = (t == TOOL_SELECT);
    m_erase_mode   = (t == TOOL_ERASER);
    m_pick_mode    = (t == TOOL_PICK);
    m_sticker_mode = (t == TOOL_STICKER);
    // Salir de la edición de sticker en cuanto la herramienta deja de ser Sticker.
    if (t != TOOL_STICKER) m_editing_sticker_idx = -1;
    if (t == TOOL_SELECT) {
        const int active_oid = m_parent.get_selection().get_object_idx();
        if (active_oid >= 0) m_marked_objects.insert(active_oid);
    }
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

GLGizmoColorStitchPainter::Tool GLGizmoColorStitchPainter::current_tool() const
{
    if (m_select_mode)  return TOOL_SELECT;
    if (m_erase_mode)   return TOOL_ERASER;
    if (m_pick_mode)    return TOOL_PICK;
    if (m_sticker_mode) return TOOL_STICKER;
    return TOOL_PAINT;   // catch-all: "nada más activo" = pincel
}

void GLGizmoColorStitchPainter::set_tool_mode(bool select, bool erase)
{
    set_tool(select ? TOOL_SELECT : (erase ? TOOL_ERASER : TOOL_PAINT));
}

// s111 — al abrir el gizmo: el set marcado (objetos pintables) se siembra desde la
// selección previa; si había varios objetos, se elige uno como ACTIVO (la base solo
// maneja un objeto a la vez) y los demás quedan marcados. Picking ON todo el tiempo.
void GLGizmoColorStitchPainter::on_set_state()
{
    const bool turning_on = (m_state == On && !m_prev_on);
    GLGizmoPainterBase::on_set_state();   // (en On apaga picking; lo re-encendemos)
    m_prev_on = (m_state == On);
    // NEOTKO_COLORSTITCH_TAG — s280d: ya no hay interruptor global de parpadeo que
    // apagar al cerrar. El aviso de ángulo auto es `render_auto_angle_marker()`, que es
    // un render DEL GIZMO: se apaga solo al cerrarlo, sin estado global que sincronizar.
    // Sí hay que invalidar el marcador, porque al abrir cambia el objeto activo.
    m_auto_dirty = true;
    if (!turning_on)
        return;

    m_marked_objects.clear();
    // s348 — la herramienta se reinicia al abrir: no dejar la pestaña Stickers huérfana de ella.
    if (m_department == DEP_STICKERS) m_department = DEP_PALETTE;
    m_tool_before_stickers = -1;
    m_select_mode = false;                // (se decide abajo, ya con el set sembrado)
    m_erase_mode  = false;

    const Selection& sel       = m_parent.get_selection();
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    for (unsigned int vi : sel.get_volume_idxs())
        if (vi < volumes.size() && volumes[vi])
            m_marked_objects.insert(volumes[vi]->object_idx());

    // Selección múltiple → colapsar a un único objeto activo (el primero marcado),
    // para que el panel completo abra y la maquinaria base tenga un model_object.
    // (NO llamamos update_data aquí: el manager lo hace justo tras activar el gizmo,
    //  ya con esta selección colapsada → evita reentrancia prematura.)
    if (!m_marked_objects.empty() && sel.get_object_idx() < 0)
        m_parent.get_selection().add_object((unsigned int)*m_marked_objects.begin(), true);

    // s169 F3 — si el objeto activo ya tiene MixedFilament Object en ON, abrir
    // directamente el departamento Object (evita el "¿dónde está mi objeto?").
    // m_c->selection_info() aún no está listo aquí (lo puebla el manager justo
    // después de activar el gizmo) — resolvemos el objeto activo directamente
    // desde `sel`/`sel.get_model()`, igual que el bloque de arriba.
    {
        const int active_oid = sel.get_object_idx();
        const Model* model = sel.get_model();
        if (model && active_oid >= 0 && active_oid < (int)model->objects.size()) {
            const ModelObject* mo = model->objects[active_oid];
            const auto* opt = dynamic_cast<const ConfigOptionBool*>(
                mo->config.option("mixed_filament_sandwich_mode"));
            if (opt && opt->value)
                m_department = DEP_OBJECT;
        }
    }

    // NEOTKO_COLORSTITCH_TAG — s231 F2: abrir el gizmo SIN selección era un callejón
    // sin salida. Arrancaba en modo pintar, y en modo pintar on_mouse sólo pre-activa
    // objetos MARCADOS (con el set vacío, ninguno) y encima CONSUME el LeftDown sobre
    // objetos no marcados, así que el canvas tampoco los seleccionaba: el panel pedía
    // "click an object in the scene" y clicar no hacía absolutamente nada. Salía de
    // ahí sólo quien adivinaba que debía pulsar Select. Sin objetos que pintar, la
    // herramienta correcta ES Select.
    if (m_marked_objects.empty())
        set_tool(TOOL_SELECT);

    m_preview_dirty = true;
    m_parent.enable_picking(true);
}

// s111 — activar un objeto como ACTIVO de pintura PRESERVANDO el color elegido.
// Poner el objeto como selección single-full-instance hace que el painter base
// reconstruya sus triangle_selectors (update_from_model_object), pero ESE reset
// pone m_selected_profile_id=0 y m_active_slot=0 → se perdía el color al cambiar
// de objeto (el trazo pintaba NONE = borrar). Aquí guardamos la intención de color
// antes y la re-materializamos en el NUEVO objeto después.
void GLGizmoColorStitchPainter::switch_active_object(int object_idx)
{
    // Preservar la INTENCIÓN de color (update_from_model_object la resetea); el
    // slot concreto se materializa POR objeto al pintar (get_left_button_state_type),
    // NO aquí — así pre-activar por hover no ensucia slots en objetos que solo cruzas.
    const int  saved_pid    = m_selected_profile_id;
    const bool saved_recipe = m_has_active_recipe;

    m_marked_objects.insert(object_idx);
    m_parent.get_selection().add_object((unsigned int)object_idx, true);
    m_preview_dirty = true;
    m_parent.get_gizmos_manager().update_data();   // SelectionInfo + selectors + raycaster
    // El cache de raycast (m_rr) está keyed por posición de ratón: al cambiar de
    // objeto sin mover el ratón devolvería el hit del objeto anterior. Invalidar.
    invalidate_raycast_cache();

    m_selected_profile_id = saved_pid;
    m_has_active_recipe   = saved_recipe;
    m_active_resolved     = false;   // recipe se re-materializa al pintar
    m_active_slot         = 0;       // saved-profile se re-materializa al pintar
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// s111 — marcar/activar un objeto (o desmarcarlo con shift). En select gestiona el
// set; el objeto marcado se vuelve el ACTIVO de pintura conservando el color.
void GLGizmoColorStitchPainter::toggle_mark(int object_idx, bool unmark)
{
    if (unmark) {
        Selection& sel = m_parent.get_selection();
        const bool was_active = (sel.get_object_idx() == object_idx);
        m_marked_objects.erase(object_idx);
        m_preview_dirty = true;
        if (was_active) {
            if (!m_marked_objects.empty())
                switch_active_object(*m_marked_objects.begin());
            else {
                sel.clear();
                m_parent.get_gizmos_manager().update_data();
            }
        }
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    } else {
        switch_active_object(object_idx);
    }
}

// NEOTKO_STICKER_TAG — `rr_mesh_id()` indexa m_triangle_selectors, que se
// construye 1:1 en orden con los model_part volumes de mo->volumes (ver
// update_from_model_object). Este helper hace ese mismo recorrido para
// resolver el ModelVolume real bajo el raycast — usado tanto al colocar un
// sticker nuevo como al arrastrar uno ya colocado.
static const ModelVolume* volume_for_mesh_id(const ModelObject* mo, int mesh_id)
{
    if (!mo || mesh_id < 0) return nullptr;
    int vidx = 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        if (vidx == mesh_id) return mv;
        ++vidx;
    }
    return nullptr;
}

// s342 (Stickers v2, Fase 0b) — raycast contra el volumen bajo el cursor, SIN activar
// su objeto. El volumen lo decide el picking de escena del canvas (el mismo que da
// get_first_hover_volume_idx, ya con el plano de corte aplicado); aquí solo se busca
// la faceta con un MeshRaycaster propio del ModelVolume (cacheado).
bool GLGizmoColorStitchPainter::raycast_under_cursor(CursorHit& out)
{
    const int hovered = m_parent.get_first_hover_volume_idx();
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    if (hovered < 0 || hovered >= (int)volumes.size() || !volumes[hovered])
        return false;
    const GLVolume* glv   = volumes[hovered];
    const Model*    model = m_parent.get_selection().get_model();
    const int oid = glv->object_idx(), vid = glv->volume_idx();
    if (!model || oid < 0 || oid >= (int)model->objects.size())
        return false;
    const ModelObject* mo = model->objects[oid];
    if (vid < 0 || vid >= (int)mo->volumes.size() || !mo->volumes[vid]->is_model_part())
        return false;
    const ModelVolume* mv = mo->volumes[vid];

    int mesh_id = 0;   // ordinal entre model_parts, el mismo orden que m_triangle_selectors
    for (int i = 0; i < vid; ++i)
        if (mo->volumes[i]->is_model_part()) ++mesh_id;

    CursorRaycaster& c = m_cursor_rc[mv->id().id];
    if (!c.rc || c.mesh != mv->mesh_ptr()) {
        c.mesh = mv->mesh_ptr();
        c.rc   = std::make_unique<MeshRaycaster>(c.mesh);
    }
    const ClippingPlane* clp = m_c->object_clipper() ? m_c->object_clipper()->get_clipping_plane() : nullptr;
    Vec3f  hit, normal;
    size_t facet = 0;
    if (!c.rc->unproject_on_mesh(m_parent.get_local_mouse_position(), glv->world_matrix(),
                                 wxGetApp().plater()->get_camera(), hit, normal, clp, &facet))
        return false;

    out.obj_idx = oid;
    out.mv      = mv;
    out.mesh_id = mesh_id;
    out.hit     = hit;
    out.normal  = normal;
    out.facet   = int(facet);
    return true;
}

bool GLGizmoColorStitchPainter::on_mouse(const wxMouseEvent& mouse_event)
{
    // NEOTKO_COLORSTITCH_TAG — s118: el botón derecho es SOLO cámara aquí (no pinta,
    // get_right_button_state_type()==-1). Tras un pan de cámara, el canvas
    // (GLCanvas3D ~:4558, RightUp) FUERZA la selección del volumen bajo el cursor —
    // sin guard de dragging — y cambia el objeto activo del painter: se veía como
    // swap de visibilidad del overlay (+ recarga del objeto no marcado). La base
    // sólo consume el RightUp si NO hubo arrastre, así que tras panear lo deja pasar.
    // Consumirlo aquí hace que el canvas salte el force-select (su mouse_up_cleanup()
    // se ejecuta igual en la rama "gizmo consumió", GLCanvas3D:4153-4154).
    if (mouse_event.RightUp())
        return true;

    // NEOTKO_COLORSTITCH_TAG — s118: eyedropper. Lee el slot de la FACETA bajo el
    // cursor y enlaza su receta. s342 (Stickers v2, Fase 0b): YA NO pre-activa en
    // hover. Antes cada objeto que cruzaba el ratón se marcaba para siempre y robaba
    // la selección (ObjectList, barra lateral, reconstrucción de selectores); ahora
    // el cuentagotas LEE con raycast_under_cursor() y no toca ni selección ni marcas.
    if (m_pick_mode) {
        if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
            CursorHit ch;
            if (!raycast_under_cursor(ch))
                return true;   // vacío: consumir
            const int active_oid = m_parent.get_selection().get_object_idx();
            int picked_slot = 0;   // 0 = sin pintar; 1..MAX-1 = slot
            if (ch.obj_idx == active_oid && ch.mesh_id < (int)m_triangle_selectors.size()
                && m_triangle_selectors[ch.mesh_id]) {
                // Activo: sus selectores vivos (pintura aún sin volcar al modelo).
                picked_slot = (int)m_triangle_selectors[ch.mesh_id]->get_state_at(ch.hit, ch.facet);
            } else {
                // No activo: selector de SOLO LECTURA desde la pintura guardada.
                TriangleSelector ts(ch.mv->mesh());
                ts.deserialize(ch.mv->colorstitch_paint_facets.get_data(), false,
                               static_cast<EnforcerBlockerType>(MAX_SLOTS - 1));
                picked_slot = (int)ts.get_state_at(ch.hit, ch.facet);
            }
            NEOTKO_LOG(PROFILE, "PICK_NOACTIVATE obj_idx=" << ch.obj_idx << " active=" << active_oid
                << " mesh_id=" << ch.mesh_id << " facet=" << ch.facet << " slot=" << picked_slot);
            // s232 — se pasa TAMBIÉN el volumen del raycast: el slot es por volumen.
            pick_recipe_from_object(ch.obj_idx, picked_slot, ch.mesh_id);
            return true;   // consumir: el pick no debe pintar
        }
        return false;   // resto de eventos: cámara/hover normales
    }

    // NEOTKO_STICKER_TAG — herramienta Sticker, dos sub-modos:
    //  (a) edición de un sticker YA colocado (m_editing_sticker_idx>=0, entrado
    //      vía el botón Edit de su capa, pestaña Stickers): arrastrar mueve su
    //      posición (raycast por frame, sin realinear a la normal — v1.5, ver
    //      nota del header) y consume TODO para no pintar/seleccionar mientras
    //      se edita. La rotación Z se controla con un slider en el panel, no
    //      aquí (ver render_sticker_section).
    //  (b) colocar un sticker NUEVO (sin cambios de comportamiento): mismo
    //      patrón de pre-activación en hover que el eyedropper, click coloca
    //      el SVG pendiente tangente al punto de impacto.
    if (m_sticker_mode) {
        if (m_editing_sticker_idx >= 0) {
            // Solo consumimos los eventos del botón IZQUIERDO que arrancan/continúan
            // el arrastre — el resto (Moving suelto, drag con botón derecho/medio
            // para orbitar cámara, rueda) pasa sin tocar, igual que el resto de
            // modos de esta clase, para no dejar la cámara bloqueada mientras se
            // coloca un sticker.
            bool consumed = false;
            if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
                // s348 — un arrastre entero = un paso de undo. s349 (auditoría #9): el snapshot
                // se toma al MOVER de verdad (abajo), no aquí: un clic fuera del objeto dejaba
                // un paso vacío.
                m_sticker_dragging = true;
                m_sticker_drag_snapshotted = false;
                consumed = true;
            } else if (m_sticker_dragging && mouse_event.Dragging()) {
                consumed = true;
            } else if (mouse_event.LeftUp() && m_sticker_dragging) {
                m_sticker_dragging = false;
                m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
                consumed = true;
            }

            if (consumed && (mouse_event.LeftDown() || mouse_event.Dragging())) {
                ModelObject* mo_c = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
                const int mid = rr_mesh_id();
                if (mo_c && m_editing_sticker_idx < (int)mo_c->colorstitch_stickers.size()) {
                    const ModelVolume* hit_mv = volume_for_mesh_id(mo_c, mid);
                    if (hit_mv) {
                        const Vec3d hit_obj = hit_mv->get_matrix() * rr_hit().cast<double>();
                        if (!m_sticker_drag_snapshotted) {
                            Plater::TakeSnapshot snap(wxGetApp().plater(), _u8L("Move sticker"),
                                                      UndoRedo::SnapshotType::GizmoAction);
                            m_sticker_drag_snapshotted = true;
                        }
                        ColorStitchSticker& st = mo_c->colorstitch_stickers[m_editing_sticker_idx];
                        // s350 — se desliza por la cara de SU lado (proyección), conservando giro,
                        // tamaño, lado y espejo (antes se reconstruía sin volteo ni espejo).
                        ColorStitch::StickerPose pose = ColorStitch::sticker_decompose(st.transform);
                        Vec3d proj;
                        if (sticker_project(mo_c, hit_obj, pose.bottom, proj)) {
                            pose.pos      = proj;
                            pose.spin_rad = double(m_editing_spin_deg) * M_PI / 180.0;
                            pose.scale    = double(m_editing_scale);
                            st.transform  = ColorStitch::sticker_compose(pose, sticker_obj_scale(mo_c));
                        }
                        m_parent.set_as_dirty();
                        m_parent.request_extra_frame();
                    }
                }
            }
            return consumed;
        }

        // s342 (Fase 0b): colocar tampoco pre-activa en hover. El click RAYCASTEA el
        // objeto bajo el cursor; si no es el activo, lo activa (gesto deliberado) y
        // coloca en el MISMO click, con el ModelObject resuelto a mano (no depende de
        // que selection_info() ya se haya refrescado).
        if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
            CursorHit ch;
            if (!raycast_under_cursor(ch))
                return true;   // vacío: consumir
            if (m_pending_sticker_svg.empty()) {
                // s349 (auditoría #7): sin SVG cargado el clic se tragaba y no había forma
                // de cambiar de objeto. Ahora selecciona el objeto clicado; no coloca nada.
                if (ch.obj_idx != m_parent.get_selection().get_object_idx())
                    switch_active_object(ch.obj_idx);
                return true;
            }
            Model* model = m_parent.get_selection().get_model();
            if (!model || ch.obj_idx >= (int)model->objects.size())
                return true;
            ModelObject* mo_hit = model->objects[ch.obj_idx];
            if (ch.obj_idx != m_parent.get_selection().get_object_idx())
                switch_active_object(ch.obj_idx);
            place_sticker_at(mo_hit, ch.mv, ch.hit);
            return true;   // consumir: colocar no debe además pintar
        }
        return false;   // resto de eventos: cámara/hover normales
    }

    if (m_select_mode) {
        if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
            const int hovered = m_parent.get_first_hover_volume_idx();
            if (hovered < 0)
                return true;   // espacio vacío: consumir, no pintar ni deseleccionar
            const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
            if (hovered >= (int)volumes.size() || volumes[hovered] == nullptr)
                return true;
            const int obj_idx = volumes[hovered]->object_idx();
            const Model* model = m_parent.get_selection().get_model();
            if (!model || obj_idx < 0 || obj_idx >= (int)model->objects.size())
                return true;
            // Shift = desmarcar; click normal = marcar + activar.
            toggle_mark(obj_idx, mouse_event.ShiftDown());
            return true;   // consume: mantenemos nosotros la selección
        }
        // Resto de eventos (Moving, rueda, arrastre de cámara): dejar pasar para
        // que la cámara y el hover funcionen normalmente.
        return false;
    }

    // Modo pintar/borrar. La base solo tiene UN objeto cargado a la vez y su
    // raycaster/cache (m_rr) solo está listo para ese objeto un frame DESPUÉS de
    // cambiarlo → si activáramos en el LeftDown, el primer click no pintaría
    // (de ahí el "doble click"). Solución: PRE-ACTIVAR al pasar el ratón (Moving):
    // cuando el cursor entra en otro objeto del SET marcado, lo cargamos ya; al
    // hacer click su raycaster lleva ≥1 frame listo y pinta a la primera. Solo se
    // pre-activan objetos MARCADOS → hover/click sobre no-marcados no hace nada
    // (no se añaden ni se pintan: la pintura queda restringida al set).
    if ((mouse_event.Moving() || mouse_event.LeftDown()) && !mouse_event.Dragging()) {
        const int hovered = m_parent.get_first_hover_volume_idx();
        int obj_idx = -1;
        if (hovered >= 0) {
            const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
            if (hovered < (int)volumes.size() && volumes[hovered])
                obj_idx = volumes[hovered]->object_idx();
        }
        const bool on_marked = (obj_idx >= 0 && m_marked_objects.count(obj_idx) > 0);
        if (on_marked && obj_idx != m_parent.get_selection().get_object_idx())
            switch_active_object(obj_idx);   // carga el objeto marcado bajo el cursor

        // NEOTKO_COLORSTITCH_TAG — s231 F2: click sobre un objeto NO-marcado. Antes se
        // consumía en silencio: ni se pintaba, ni se seleccionaba, ni había pista de
        // por qué ("he clicado y no pasa nada"). Ahora ese click lo MARCA y lo activa
        // — que es lo que el usuario está pidiendo al clicarlo — sin pintar todavía:
        // el raycaster de la base necesita ≥1 frame para estar listo (ver la nota de
        // pre-activación de arriba), así que este click adopta el objeto y el
        // siguiente ya pinta. Sigue siendo un gesto deliberado (LeftDown, no drag),
        // así que no puede "contagiarse" a un vecino mientras se pincela.
        if (mouse_event.LeftDown() && obj_idx >= 0 && !on_marked) {
            switch_active_object(obj_idx);   // inserta en m_marked_objects + activa
            return true;
        }
    }

    // NEOTKO_MIXEDFIL_SANDWICH_TAG — s231 F1: con "MixedFilament Object" ON el motor
    // BYPASEA el pintado por-cara de este objeto. La UI ya lo deshabilitaba, pero
    // `disabled_begin` sólo apaga los widgets del panel: el pincel del CANVAS seguía
    // vivo, así que se podía pintar el 3D quemando slots, creando perfiles auto y
    // agendando re-slices cuyo resultado el motor descarta — trabajo perdido y en
    // silencio. Mismo patrón que el guard "sin destino de pintura" de más abajo:
    // consumir el LeftDown corta el trazo en seco (m_button_down se queda en None, así
    // que el Dragging posterior tampoco pinta). Select/Pick siguen pasando: cambiar de
    // objeto o leer una receta son operaciones legítimas con MixedFilament activo.
    if (painting_blocked() && (mouse_event.LeftDown() || mouse_event.Dragging()))
        return true;

    // Sin objeto activo no hay nada que pintar — NO llamar a la base (deref nulo en
    // selection_info()->model_object()).
    if (!m_c->selection_info() || !m_c->selection_info()->model_object())
        return false;

    // NEOTKO_COLORSTITCH_TAG — s118: sin destino de pintura, un click izquierdo NO
    // debe borrar. get_left_button_state_type() cae a NONE (=borrar) cuando no hay
    // recipe/profile/slot activo, y la base NO tiene guard -1 para el izquierdo (sí
    // para el derecho) → un click en bucket sin color seleccionado borraba el slot.
    // Consumir el LeftDown aquí corta el trazo en seco (m_button_down se queda en
    // None, así que el Dragging posterior tampoco pinta). El modo Eraser explícito
    // sí pasa (borrar es su función).
    if (mouse_event.LeftDown() && !mouse_event.Dragging() && !m_erase_mode) {
        const bool has_paint_target =
            m_has_active_recipe ||
            m_selected_profile_id != 0 ||
            (m_active_slot >= 1 && m_active_slot < MAX_SLOTS);
        if (!has_paint_target)
            return true;   // consumir sin pintar/borrar
    }

    // NEOTKO_NEOTOWER_TAG — al terminar un trazo de pintura (LeftUp en modo pintar),
    // promociona el tipo de torre a NeoTower si seguía en Classic, para que la UI muestre
    // el planificador que realmente se usará (one-shot; el slice ya auto-promociona internamente).
    const bool finished_paint_stroke = mouse_event.LeftUp() && !m_select_mode && !m_pick_mode;
    const bool ret = GLGizmoPainterBase::on_mouse(mouse_event);
    if (finished_paint_stroke)
        ensure_neotower_tower_type();
    return ret;
}

// NEOTKO_NEOTOWER_TAG — painting a zone always needs the NeoTower planner (per-layer
// variable-height purges). The slicer already auto-promotes internally and Print::validate
// blocks Classic, but the Tower type combo stayed on "Classic", which is confusing. When the
// user paints, flip the print preset's tower type to NeoTower so the UI matches reality. The
// guard makes this a one-shot: once on NeoTower it does nothing on subsequent strokes.
void GLGizmoColorStitchPainter::ensure_neotower_tower_type()
{
    auto& cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto* t = cfg.option<ConfigOptionEnum<NeoTowerType>>("neotko_tower_type");
    if (!t || t->value == nttNeoTower || t->value == nttPrivate)   // both run on the NeoTower planner
        return;
    t->value = nttNeoTower;
    // Reflect in the Print Settings tab (refresh the combo + mark the preset modified).
    if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT)) {
        tab->reload_config();
        tab->update_dirty();
    }
    if (wxGetApp().plater())
        wxGetApp().plater()->set_plater_dirty(true);
}

PainterGizmoType GLGizmoColorStitchPainter::get_painter_type() const
{
    // No dedicated enum value (the field is informational only — never switched on).
    return PainterGizmoType::MM_SEGMENTATION;
}

wxString GLGizmoColorStitchPainter::handle_snapshot_action_name(bool shift_down,
                                                             GLGizmoPainterBase::Button /*button_down*/) const
{
    return (shift_down || m_erase_mode) ? _L("Erase ColorStitch paint") : _L("ColorStitch paint");
}

bool GLGizmoColorStitchPainter::on_init()
{
    m_shortcut_key = WXK_CONTROL_M; // tentative; collisions reviewed in B-polish

    const wxString ctrl  = _L("Ctrl+");
    const wxString alt   = _L("Alt+");
    const wxString shift = _L("Shift+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]         = _L("Section view");
    m_desc["reset_direction"]          = _L("Reset direction");
    m_desc["paint_caption"]            = _L("Left mouse button");
    m_desc["paint"]                    = _L("Paint with selected profile");
    m_desc["erase_caption"]            = shift + _L("Left mouse button");
    m_desc["erase"]                    = _L("Erase paint");
    m_desc["erase_mode"]               = _L("Erase mode");
    m_desc["remove_all"]               = _L("Erase all painting");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]         = _L("Smart fill angle");
    m_desc["profiles"]                 = _L("Profiles");
    m_desc["no_profiles"]              = _L("No profiles saved yet. Use 'Save as profile' in the Sandwich Editor.");
    m_desc["slots_full"]               = _L("All paint slots are used on this object. Erase a profile to free one.");

    // Smart fill default — coplanar bias for top surfaces (option 4 design choice).
    m_smart_fill_angle = 1.5f;

    return true;
}

void GLGizmoColorStitchPainter::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // El objeto ACTIVO se dibuja con la maquinaria base (pintura en vivo). Los
    // DEMÁS objetos marcados se dibujan con su pintura mediante selectors de
    // preview (render_marked_paint). Con selección vacía no hay activo → solo
    // preview. Guard para evitar derefs a selection_info().
    if (m_c->selection_info() && m_c->selection_info()->model_object()) {
        render_triangles(selection);
        m_c->object_clipper()->render_cut();
        if (m_c->instances_hider())          // s111: ya no es un requisito → puede faltar
            m_c->instances_hider()->render_cut();
        if (!m_select_mode)                   // sin cursor de pincel en modo Select
            render_cursor();
    }

    render_marked_paint();
    // s348 — Fase A: TODOS los stickers de la escena, menos el que está en edición (ese lo
    // dibuja render_sticker_edit_overlay con su contorno, justo después).
    if (const Model* model = selection.get_model())
        ColorStitchStickerPreview::render(*model, wxGetApp().plater()->get_camera(),
                                          m_editing_sticker_idx >= 0 ? selection.get_object_idx() : -1,
                                          m_editing_sticker_idx);
    render_sticker_edit_overlay();   // NEOTKO_STICKER_TAG — no-op si no hay sticker en edición
    render_slot_highlight();         // s232 — realce del slot activo / bajo el cursor
    // s280d — el marcador de auto va DESPUÉS del realce: si una zona es a la vez la
    // seleccionada y la que no tiene ángulo, el aviso tiene que quedar por encima.
    render_auto_angle_marker();

    glsafe(::glDisable(GL_BLEND));

    // La leyenda es ImGui (ForegroundDrawList), no GL: fuera del bloque de estado
    // de arriba para no dejarle el blend/depth tocado a quien venga detrás.
    render_stitch_legend();
}

// s111 — (re)construye los selectors de preview para los objetos marcados que NO
// son el activo, leyendo su pintura guardada (colorstitch_paint_facets). Lazy: solo
// cuando cambian las marcas / la pintura / el objeto activo (m_preview_dirty).
void GLGizmoColorStitchPainter::rebuild_preview_selectors_if_dirty()
{
    if (!m_preview_dirty)
        return;
    m_preview_dirty = false;
    m_hl_dirty      = true;   // s232 — cambió el set marcado / el objeto activo
    m_auto_dirty    = true;   // s280d — ídem para el marcador de ángulo auto
    m_preview_sel.clear();

    const Model* model = m_parent.get_selection().get_model();
    if (!model)
        return;
    const int active_oid = m_parent.get_selection().get_object_idx();
    const EnforcerBlockerType max_ebt = static_cast<EnforcerBlockerType>(MAX_SLOTS - 1);

    for (int oid : m_marked_objects) {
        if (oid == active_oid || oid < 0 || oid >= (int)model->objects.size())
            continue;
        const ModelObject* mo = model->objects[oid];
        auto& vec = m_preview_sel[oid];
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            auto sel = std::make_unique<TriangleSelectorPatch>(
                mv->mesh(), build_ebt_colors_for_volume(mv, mo));
            sel->deserialize(mv->colorstitch_paint_facets.get_data(), false, max_ebt);
            // NEOTKO_COLORSTITCH_TAG — s231b (glitch reportado por el usuario): estos
            // selectores recibían color pero NUNCA weave, así que un objeto marcado que
            // dejaba de ser el activo caía a su color PLANO compuesto. Al deslizar el
            // ratón de un objeto a otro eso se veía como un parpadeo: el patrón
            // ColorStitch/PathBlend desaparecía un momento y volvía, dejando a la vista
            // una composición color↔TD que además NO representa lo que hace el efecto
            // (un patrón plano no tiene un TD equivalente; esa vía se abandonó a favor
            // de RealColor, que sí modela cómo se ve el color de verdad).
            // El arreglo es darles el MISMO tejido que al objeto activo, no quitarles el
            // color: así el preview es estable pase el foco por donde pase.
            // DESPUÉS de deserialize, como en update_from_model_object: el weave necesita
            // medir el área realmente pintada (get_facets).
            sel->set_ebt_weave(build_ebt_weave_for_volume(mv, sel.get(), mo));
            {
                std::unordered_map<int,int> fwi;
                std::vector<TriangleSelectorPatch::WeaveParams> wl;
                build_ebt_weave_islands_for_volume(mv, sel.get(), fwi, wl, mo);
                sel->set_ebt_weave_islands(std::move(fwi), std::move(wl));
            }
            sel->request_update_render_data();
            vec.push_back(std::move(sel));
        }
    }
}

// s111 — dibuja la pintura de los objetos marcados NO-activos (el activo lo pinta
// render_triangles de la base). Replica el setup de uniforms de
// GLGizmoPainterBase::render_triangles, con la malla + trafo de instancia de cada
// objeto. NEOTKO_COLORSTITCH_TAG.
void GLGizmoColorStitchPainter::render_marked_paint()
{
    rebuild_preview_selectors_if_dirty();
    if (m_preview_sel.empty())
        return;

    const Model* model = m_parent.get_selection().get_model();
    if (!model)
        return;
    GLShaderProgram* shader = wxGetApp().get_shader("mm_gouraud");
    if (shader == nullptr)
        return;

    shader->start_using();
    const ClippingPlaneDataWrapper clp = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp.clp_dataf);
    shader->set_uniform("z_range", clp.z_range);
    glsafe(::glDisable(GL_CULL_FACE));

    const Camera&      camera = wxGetApp().plater()->get_camera();
    const Transform3d& view   = camera.get_view_matrix();

    for (auto& kv : m_preview_sel) {
        const int oid = kv.first;
        if (oid < 0 || oid >= (int)model->objects.size()) continue;
        const ModelObject* mo = model->objects[oid];
        if (mo->instances.empty()) continue;
        const ModelInstance* mi = mo->instances.front();

        int mesh_id = -1;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            ++mesh_id;
            if (mesh_id >= (int)kv.second.size() || !kv.second[mesh_id]) continue;

            const Transform3d trafo = mi->get_transformation().get_matrix() * mv->get_matrix();
            const bool is_left_handed = trafo.matrix().determinant() < 0.;
            if (is_left_handed) glsafe(::glFrontFace(GL_CW));

            shader->set_uniform("view_model_matrix", view * trafo);
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            const Matrix3d view_normal_matrix = view.matrix().block(0, 0, 3, 3)
                * trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
            shader->set_uniform("view_normal_matrix", view_normal_matrix);
            shader->set_uniform("volume_world_matrix", trafo);
            shader->set_uniform("volume_mirrored", is_left_handed);
            const Matrix3f normal_matrix = static_cast<Matrix3f>(
                trafo.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());
            shader->set_uniform("slope.actived", false);
            shader->set_uniform("slope.volume_world_normal_matrix", normal_matrix);
            shader->set_uniform("slope.normal_z", -1.0f);

            kv.second[mesh_id]->render(m_imgui, trafo);

            if (is_left_handed) glsafe(::glFrontFace(GL_CCW));
        }
    }
    shader->stop_using();
}

// s232 — fwd-decl: los colores por zona se definen junto al editor de zonas del Pro
// (allí es donde nacieron las chapas), pero el realce del viewport los necesita aquí.
// Una sola definición para los dos consumidores — ver la nota de `cs_zone_rgb`.
// NEOTKO_COLORSTITCH_TAG — s280d: violeta del marcador de ángulo AUTO.
// Deliberadamente FUERA de la familia de las zonas: Top es verde (0.24,0.75,0.44),
// Penu verde oscuro y Bottom naranja (0.94,0.58,0.20) — y el ámbar del tag "auto
// angle" del panel (0.95,0.75,0.20) es casi ese mismo naranja, así que reutilizarlo
// en el viewport haría ilegible "es de la zona de abajo" contra "no tiene ángulo".
static const ColorRGBA CS_AUTO_ANGLE_RGBA{0.78f, 0.36f, 0.96f, 1.f};

static ColorRGBA cs_zone_rgba(int zone, float alpha);
static ImU32     cs_zone_u32(int zone, int alpha);

// ============================================================================
// s232 — REALCE EN VIEWPORT DEL SLOT ACTIVO
//
// El pendiente que dejó s231: el panel sabe qué color está activo, pero el
// viewport no lo decía en ninguna parte, así que "¿dónde está aplicado este
// color?" sólo se podía contestar mirando la malla a ojo (y con dos colores
// parecidos, ni eso). Se resuelve con el MISMO lenguaje visual del AID de
// Align&Stack (s227) — geometría suelta con el shader `flat` + chapas
// billboard dibujadas directo en el ForegroundDrawList — en vez de tocar el
// render del patch: TriangleSelectorPatch::render y mm_gouraud los comparte
// todo el painter (y en macOS el contexto es Legacy GL 2.1, ver s229), así
// que un realce ahí sería caro y con blast radius. Aquí el realce es una
// capa independiente que se puede apagar sin riesgo.
// ============================================================================

// Slot a resaltar: manda el hover del panel (pregunta explícita "¿dónde está
// ESTE?"), y si no hay, el slot activo (respuesta continua a "¿dónde estoy
// pintando?"). 0 = ninguno.
int GLGizmoColorStitchPainter::highlight_slot() const
{
    if (m_hover_slot >= 1 && m_hover_slot < MAX_SLOTS)
        return m_hover_slot;
    if (m_active_slot >= 1 && m_active_slot < MAX_SLOTS)
        return m_active_slot;
    // m_active_slot es 0 mientras el color activo no esté MATERIALIZADO en este
    // objeto (s231 F0 lo invalida a propósito al cambiar de objeto o de perfil, y
    // sólo se re-materializa al pintar). Pero si el perfil seleccionado YA ocupa un
    // slot aquí, el realce puede contestar igual: búsqueda de sólo lectura, sin
    // asignar nada (asignar desde un render sería quemar un slot por mirar).
    if (m_selected_profile_id != 0) {
        const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (mo)
            for (const ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                for (int s = 1; s < MAX_SLOTS; ++s)
                    if (mv->colorstitch_slot_to_profile_id[s] == m_selected_profile_id)
                        return s;
            }
    }
    return 0;
}

// NEOTKO_COLORSTITCH_TAG — s280d: extraído del lambda `add_part` de
// rebuild_slot_highlight_if_dirty SIN cambiar una línea de su lógica, para que el
// marcador de ángulo AUTO reutilice exactamente el mismo contorno (aristas de borde +
// islas + zona por orientación) en vez de una segunda copia que se desincronice.
// `out`/`facets` son lo único que antes iba capturado (m_hl_parts / m_hl_facets).
void GLGizmoColorStitchPainter::build_outline_part(const TriangleSelectorGUI* sel,
                                                  const ModelVolume*         mv,
                                                  const ModelObject*         owner,
                                                  const Transform3d&         trafo,
                                                  int                        vol_slot,
                                                  std::vector<SlotHighlightPart>& out,
                                                  int&                       facets) const
{
    if (!sel || !mv || !owner || vol_slot <= 0)
        return;
    const indexed_triangle_set its = sel->get_facets(static_cast<EnforcerBlockerType>(vol_slot));
    if (its.indices.empty())
        return;
    const int nt = int(its.indices.size());

    // -- aristas: las que aparecen en UNA sola cara son el borde de la región
    // pintada. Dibujar la malla entera del slot taparía justo lo que el usuario
    // está mirando (el tejido del preview); el contorno lo enmarca sin taparlo.
    // El mismo recorrido sirve para unir caras por arista compartida (islas),
    // idéntico criterio que build_ebt_weave_islands_for_volume: dos zonas que
    // sólo se tocan en una ESQUINA son dos islas.
    // -- orientación por cara, en MUNDO (el signo depende del determinante de la
    // trafo: un objeto espejado tiene las caras invertidas). Se calcula ANTES que
    // las aristas porque el contorno se parte por zona: cada arista de borde
    // pertenece a UNA cara, y su zona es la de esa cara. Umbral 0.30 = el mismo de
    // `discard_non_zone_facing` y del preview del tejido.
    const Matrix3d nrm_mat = trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
    std::vector<char> tri_down(nt, 0);
    for (int k = 0; k < nt; ++k) {
        const auto& tri = its.indices[k];
        const Vec3d n = nrm_mat * (its.vertices[tri[1]] - its.vertices[tri[0]])
                                      .cast<double>()
                                      .cross((its.vertices[tri[2]] - its.vertices[tri[0]]).cast<double>());
        tri_down[k] = (n.norm() > 1e-12 && n.z() / n.norm() <= -0.30) ? 1 : 0;
    }

    std::map<std::pair<int, int>, std::pair<int, int>> edges;   // (v0,v1) → (nº usos, 1ª cara)
    std::vector<int> parent(nt);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&parent](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) parent[a] = b; };
    for (int k = 0; k < nt; ++k) {
        const auto& tri = its.indices[k];
        for (int c = 0; c < 3; ++c) {
            int a = tri[c], b = tri[(c + 1) % 3];
            if (a > b) std::swap(a, b);
            auto it = edges.find({a, b});
            if (it == edges.end()) edges.emplace(std::make_pair(a, b), std::make_pair(1, k));
            else { ++it->second.first; unite(k, it->second.second); }
        }
    }

    // -- normal por vértice, para separar el contorno de la superficie y que no
    // haga z-fighting con la propia cara pintada.
    std::vector<Vec3f> vnrm(its.vertices.size(), Vec3f::Zero());
    for (const auto& tri : its.indices) {
        const Vec3f n = (its.vertices[tri[1]] - its.vertices[tri[0]])
                            .cross(its.vertices[tri[2]] - its.vertices[tri[0]]);
        for (int c = 0; c < 3; ++c) vnrm[tri[c]] += n;
    }
    // El desplazamiento se pide en mm de MUNDO, así que se divide por la escala
    // de la trafo (un objeto escalado ×0.1 necesita 10× más en local para
    // separarse lo mismo en pantalla).
    double sc = 0.0;
    for (int c = 0; c < 3; ++c) sc += trafo.matrix().block(0, 0, 3, 3).col(c).norm();
    sc = std::max(sc / 3.0, 1e-6);
    const float eps = float(0.08 / sc);
    auto lifted = [&](int vi) -> Vec3f {
        Vec3f n = vnrm[vi];
        const float len = n.norm();
        if (len < 1e-12f) return its.vertices[vi];
        return its.vertices[vi] + (n / len) * eps;
    };

    SlotHighlightPart part;
    part.trafo = trafo;

    // Un modelo por zona (ver la nota del .hpp): el color es un uniform, así que
    // "verde arriba / naranja abajo" obliga a dos buffers.
    GLModel::Geometry geo_top, geo_bot;
    for (GLModel::Geometry* g : { &geo_top, &geo_bot })
        g->format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    unsigned int idx_top = 0, idx_bot = 0;
    for (const auto& e : edges) {
        if (e.second.first != 1) continue;   // interior: no es borde
        const bool down = tri_down[e.second.second] != 0;
        GLModel::Geometry& g = down ? geo_bot : geo_top;
        unsigned int&      n_idx = down ? idx_bot : idx_top;
        g.add_vertex(lifted(e.first.first));
        g.add_vertex(lifted(e.first.second));
        g.add_index(n_idx++);
        g.add_index(n_idx++);
    }
    if (idx_top == 0 && idx_bot == 0)
        return;
    if (idx_top > 0) {
        part.edges_top = std::make_unique<GLModel>();
        part.edges_top->init_from(std::move(geo_top));
    }
    if (idx_bot > 0) {
        part.edges_bottom = std::make_unique<GLModel>();
        part.edges_bottom->init_from(std::move(geo_bot));
    }

    // -- AABB en MUNDO por isla (+ su recuento de caras, para quedarse con las
    // que de verdad merecen chapa) + a qué zona mira.
    std::map<int, int> root_to_island;
    std::vector<int>   island_down;   // votos "mira hacia abajo" por isla
    for (int k = 0; k < nt; ++k) {
        const int root = find(k);
        auto it = root_to_island.find(root);
        int isl;
        if (it == root_to_island.end()) {
            isl = int(part.islands.size());
            root_to_island.emplace(root, isl);
            part.islands.emplace_back();
            part.island_facets.push_back(0);
            island_down.push_back(0);
        } else isl = it->second;
        ++part.island_facets[isl];
        const auto& tri = its.indices[k];
        if (tri_down[k]) ++island_down[isl];
        for (int c = 0; c < 3; ++c)
            part.islands[isl].merge(trafo * its.vertices[tri[c]].cast<double>());
    }
    // Mayoría simple: una isla es "de bottom" si la mayor parte de sus caras
    // miran hacia abajo. El umbral 0.30 es el mismo que usan la pintura
    // (`discard_non_zone_facing`) y el preview del tejido — si divergiera, el
    // realce clasificaría al revés que el motor.
    part.island_bottom.resize(part.islands.size(), 0);
    for (size_t i = 0; i < part.islands.size(); ++i)
        part.island_bottom[i] = (island_down[i] * 2 > part.island_facets[i]) ? 1 : 0;
    facets += nt;
    out.push_back(std::move(part));
}

void GLGizmoColorStitchPainter::rebuild_slot_highlight_if_dirty()
{
    // ANTES de mirar el flag: un rebuild de los selectores de preview (marcas /
    // objeto activo) ensucia el realce, y hacerlo al revés lo dejaría un frame
    // desfasado — o, si se llamara desde dentro, ensuciándose a sí mismo cada
    // frame y reconstruyendo la geometría sin parar.
    rebuild_preview_selectors_if_dirty();

    const int slot = highlight_slot();
    if (!m_hl_dirty && slot == m_hl_slot_built)
        return;
    m_hl_dirty      = false;
    m_hl_slot_built = slot;
    m_hl_parts.clear();
    m_hl_facets = 0;
    if (slot <= 0)
        return;

    const Model* model = m_parent.get_selection().get_model();
    if (!model)
        return;
    const int          active_oid = m_parent.get_selection().get_object_idx();
    const ModelObject* active_mo  = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;

    // s232 — el número de slot es POR VOLUMEN: en un objeto ensamblado el mismo color
    // ocupa el slot 1 en un cubo y el 6 u 11 en otro. Usar el número del objeto activo
    // para todos los volúmenes (lo que hacía este realce) ilumina el slot equivocado —
    // o nada, si ese número está libre ahí. Lo que se conserva entre volúmenes es el
    // PERFIL, así que se resuelve el slot de cada volumen por su pid.
    auto slot_in_volume = [&](const ModelVolume* mv, int pid) -> int {
        if (!mv || pid == 0) return 0;
        for (int s = 1; s < MAX_SLOTS; ++s)
            if (mv->colorstitch_slot_to_profile_id[s] == pid) return s;
        return 0;
    };

    // Construye el contorno + las islas de UN volumen ya deserializado. `vol_slot` es
    // el slot DE ESE VOLUMEN (ver la nota de arriba).
    auto add_part = [&](const TriangleSelectorGUI* sel, const ModelVolume* mv,
                        const ModelObject* owner, const Transform3d& trafo, int vol_slot) {
        build_outline_part(sel, mv, owner, trafo, vol_slot, m_hl_parts, m_hl_facets);
    };

    // El PERFIL es lo que identifica el color entre volúmenes y entre objetos.
    // ⚠️ s232 — y sale del ENLACE ACTIVO (`m_selected_profile_id`), no de la tabla de
    // slots. Derivarlo de "el primer volumen que tenga algo en ese número" fue la
    // regresión que apagó el realce: en un ensamblado ese primer volumen puede llevar
    // OTRO color en ese número, y con el pid equivocado el slot por volumen salía 0 en
    // todas partes. El número de slot sólo se usa como último recurso, para un color
    // recién materializado que todavía no tiene id enlazado.
    int want_pid = m_selected_profile_id;
    if (want_pid == 0 && active_mo)
        for (const ModelVolume* mv : active_mo->volumes)
            if (mv->is_model_part() && mv->colorstitch_slot_to_profile_id[slot] != 0) {
                want_pid = mv->colorstitch_slot_to_profile_id[slot];
                break;
            }

    // Objeto ACTIVO: se usan sus selectores vivos, para que el realce siga el
    // trazo que se está pintando ahora mismo y no una copia serializada.
    if (active_mo && !active_mo->instances.empty()) {
        const Transform3d inst = active_mo->instances.front()->get_transformation().get_matrix();
        int vol_idx = -1;
        for (const ModelVolume* mv : active_mo->volumes) {
            if (!mv->is_model_part()) continue;
            ++vol_idx;
            if (vol_idx >= (int)m_triangle_selectors.size()) break;
            // s232 — cada volumen con SU slot para este perfil (en un ensamblado el
            // mismo color es el slot 1 en un cubo y el 11 en otro). El fallback al
            // número del activo cubre el caso de un color aún sin perfil enlazado.
            const int vs = want_pid ? slot_in_volume(mv, want_pid) : slot;
            add_part(m_triangle_selectors[vol_idx].get(), mv, active_mo,
                     inst * mv->get_matrix(), vs);
        }
        // Color del slot = el MISMO compuesto con el que se dibuja la malla
        // (build_ebt_colors_for_volume), no un color de UI inventado: la chapa y
        // el contorno tienen que leerse como "este color de ahí".
        for (const ModelVolume* mv : active_mo->volumes) {
            if (!mv->is_model_part()) continue;
            const int vs = want_pid ? slot_in_volume(mv, want_pid) : slot;
            if (vs <= 0 || mv->colorstitch_slot_to_profile_id[vs] == 0) continue;
            const std::vector<ColorRGBA> cols = build_ebt_colors_for_volume(mv, active_mo);
            if (vs < (int)cols.size()) m_hl_color = cols[vs];
            break;
        }
    }

    if (want_pid != 0) {
        for (auto& kv : m_preview_sel) {
            const int oid = kv.first;
            if (oid == active_oid || oid < 0 || oid >= (int)model->objects.size()) continue;
            const ModelObject* mo = model->objects[oid];
            if (mo->instances.empty()) continue;
            const Transform3d inst = mo->instances.front()->get_transformation().get_matrix();
            int vol_idx = -1;
            for (const ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                ++vol_idx;
                if (vol_idx >= (int)kv.second.size()) break;
                // s232 — por perfil, no por número de slot (ídem que arriba).
                const int vs = slot_in_volume(mv, want_pid);
                if (vs <= 0) continue;
                add_part(kv.second[vol_idx].get(), mv, mo, inst * mv->get_matrix(), vs);
            }
        }
    }
}

void GLGizmoColorStitchPainter::render_slot_highlight()
{
    if (!m_slot_highlight)
        return;
    rebuild_slot_highlight_if_dirty();
    if (m_hl_parts.empty())
        return;

    // El latido necesita frames: el canvas sólo repinta por eventos, así que sin
    // esto el realce se quedaría congelado en la opacidad del último repintado.
    m_parent.request_extra_frame();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    // Caja unitaria (aristas) reusada para todas las AABB de isla: se construye
    // una vez y se coloca con la matriz, igual que el resto de gizmos que dibujan
    // cajas — nada de reconstruir geometría por frame.
    if (!m_hl_island_box.is_initialized()) {
        const Vec3f c[8] = {
            {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}, {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1},
        };
        static const int e[12][2] = {
            {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7},
        };
        GLModel::Geometry geo;
        geo.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        for (const auto& k : e) { geo.add_vertex(c[k[0]]); geo.add_vertex(c[k[1]]); }
        for (unsigned int i = 0; i < 24; ++i) geo.add_index(i);
        m_hl_island_box.init_from(std::move(geo));
    }

    const Camera&      camera = wxGetApp().plater()->get_camera();
    const Transform3d& view   = camera.get_view_matrix();

    // Latido lento: el realce tiene que distinguirse del propio color pintado sin
    // parpadear ni cansar. Sólo modula la opacidad del contorno.
    const float t     = float(ImGui::GetTime());
    const float pulse = 0.72f + 0.28f * std::sin(t * 2.6f);

    // s232 — el contorno va con el color de la ZONA, no con el del slot. Dos razones:
    // sobre su propia pintura un contorno del color del slot sería invisible, y así el
    // realce enseña lo mismo que las chapas del panel Pro (verde = Top, naranja =
    // Bottom) — ver `cs_zone_rgba`, que es la fuente única de los dos sitios.
    const ColorRGBA zone_top_col = cs_zone_rgba(0, 1.f);
    const ColorRGBA zone_bot_col = cs_zone_rgba(2, 1.f);

    // El grosor de línea lo clampa el driver (en macOS Legacy GL el máximo suele
    // ser 10): pedir 12 y que se quede en 1 sería peor que pedir el máximo real.
    // Se consulta una vez y se reparte el presupuesto entre las tres pasadas.
    static float max_lw = 0.f;
    if (max_lw <= 0.f) {
        GLfloat range[2] = { 1.f, 1.f };
        glsafe(::glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range));
        max_lw = std::max(1.f, (float)range[1]);
    }
    auto lw = [&](float want) { glsafe(::glLineWidth(std::min(want, max_lw))); };

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // Pasada 1 — sin test de profundidad: el contorno de las zonas que quedan
    // detrás del objeto se ve en fantasma (es justo el caso que hoy obliga a
    // orbitar a ciegas: "lo pinté, pero ¿dónde?").
    glsafe(::glDisable(GL_DEPTH_TEST));
    lw(6.4f);
    // Un pase por zona: mismo grosor y alfa, distinto color.
    auto draw_edges = [&](float alpha) {
        for (SlotHighlightPart& p : m_hl_parts) {
            shader->set_uniform("view_model_matrix", view * p.trafo);
            if (p.edges_top) {
                ColorRGBA c = zone_top_col; c[3] = alpha;
                p.edges_top->set_color(c);
                p.edges_top->render();
            }
            if (p.edges_bottom) {
                ColorRGBA c = zone_bot_col; c[3] = alpha;
                p.edges_bottom->set_color(c);
                p.edges_bottom->render();
            }
        }
    };
    draw_edges(0.45f * pulse);

    // Pasada 2 — con profundidad: el contorno real, sobre la superficie. Es LA
    // línea que tiene que cazar la vista, así que se lleva todo el grosor que dé
    // el driver y opacidad plena en el pico del latido.
    glsafe(::glEnable(GL_DEPTH_TEST));
    lw(12.0f);
    draw_edges(std::min(1.f, 1.25f * pulse));

    // Cajas AABB por isla: el "de un vistazo" a distancia, cuando el contorno ya
    // no se distingue. Tenues, con el color de la zona de SU isla, y sólo en las
    // islas que llevan chapa.
    lw(3.0f);
    for (const SlotHighlightPart& p : m_hl_parts) {
        for (size_t i = 0; i < p.islands.size(); ++i) {
            const BoundingBoxf3& bb = p.islands[i];
            if (!bb.defined || p.island_facets[i] < 4) continue;
            ColorRGBA box_col = p.island_bottom[i] ? zone_bot_col : zone_top_col;
            box_col[3] = 0.45f;
            // Las islas planas (una cara superior) tienen espesor 0 en Z y la caja
            // degeneraría a un rectángulo con una arista invisible: se le da un
            // mínimo para que se lea como volumen.
            Vec3d sz = bb.size();
            for (int c = 0; c < 3; ++c) sz[c] = std::max(sz[c], 0.4);
            const Transform3d m = Geometry::translation_transform(bb.min) * Geometry::scale_transform(sz);
            shader->set_uniform("view_model_matrix", view * m);
            m_hl_island_box.set_color(box_col);
            m_hl_island_box.render();
        }
    }

    glsafe(::glLineWidth(1.0f));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();

    // --- Chapas: disco del color del slot con su número, encima de cada isla.
    // Mismo canal y espacio de coordenadas que las chapas numeradas de
    // Align&Stack (ForegroundDrawList, sin ventanas ImGui). Se ordenan por
    // tamaño y se queda con las mayores: una chapa por cada isla de 3 caras
    // sería confeti sobre un mesh denso.
    struct Badge { double area; Vec3d anchor; bool bottom; };
    std::vector<Badge> badges;
    for (const SlotHighlightPart& p : m_hl_parts) {
        for (size_t i = 0; i < p.islands.size(); ++i) {
            const BoundingBoxf3& bb = p.islands[i];
            if (!bb.defined || p.island_facets[i] < 4) continue;
            const bool bottom = p.island_bottom[i] != 0;
            Vec3d a = bb.center();
            // La chapa de una zona bottom va POR DEBAJO de su isla: además de no
            // solaparse con la del top de la misma pieza, sale donde el usuario tiene
            // que mirar (orbitar por debajo) para ver esa cara.
            a.z() = bottom ? bb.min.z() - 1.5 : bb.max.z() + 1.5;
            const Vec3d s = bb.size();
            badges.push_back({ std::max(s.x() * s.y(), 1e-6), a, bottom });
        }
    }
    std::sort(badges.begin(), badges.end(), [](const Badge& a, const Badge& b) { return a.area > b.area; });
    if (badges.size() > 8) badges.resize(8);
    if (badges.empty())
        return;

    const Matrix4d              proj_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4>&   viewport  = camera.get_viewport();
    ImDrawList*                 fg        = ImGui::GetForegroundDrawList();
    const std::string           label     = "s" + std::to_string(m_hl_slot_built);
    const ImU32 disc = IM_COL32(int(m_hl_color[0] * 255), int(m_hl_color[1] * 255), int(m_hl_color[2] * 255), 235);
    // Texto negro o blanco según la luminancia del propio color del slot (un slot
    // casi negro con número negro no se leería).
    const float lum = 0.299f * m_hl_color[0] + 0.587f * m_hl_color[1] + 0.114f * m_hl_color[2];
    const ImU32 txt = lum > 0.55f ? IM_COL32(20, 20, 20, 255) : IM_COL32(240, 240, 240, 255);

    for (const Badge& b : badges) {
        const Vec4d clip = TransformHelper::world_to_clip(b.anchor, proj_view);
        if (clip.w() <= 1e-6) continue;                       // detrás de la cámara
        const Vec3d ndc = TransformHelper::clip_to_ndc(clip);
        if (std::abs(ndc.x()) > 1.2 || std::abs(ndc.y()) > 1.2) continue;
        const Vec2d  ss = TransformHelper::ndc_to_ss(ndc, viewport);
        const ImVec2 c((float)ss.x(), (float)(viewport[3] - ss.y()));

        // s232 — disco = color del SLOT (qué color es), aro y cuña = color de la ZONA
        // (dónde se aplica). Las dos preguntas en una chapa, y el aro repite el mismo
        // código de color que la chapa del panel Pro.
        const ImU32 zcol = cs_zone_u32(b.bottom ? 2 : 0, 255);
        const float r = 13.0f;
        fg->AddCircleFilled(c, r, disc, 32);
        fg->AddCircle(c, r, zcol, 32, 3.0f);
        ImFont*      font  = ImGui::GetFont();
        const float  fsize = 15.0f;
        const ImVec2 tsz   = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, label.c_str());
        fg->AddText(font, fsize, ImVec2(c.x - tsz.x * 0.5f, c.y - tsz.y * 0.5f), txt, label.c_str());
        // Zona: cuña hacia abajo en las islas de bottom, hacia arriba en las de top.
        // Con una receta que lleva las dos zonas, esto es lo que dice "el color está
        // aplicado arriba Y abajo" de un vistazo. Triángulo dibujado a mano, no un
        // glifo: la fuente por defecto de ImGui no trae flechas unicode.
        {
            const float w = 6.5f, h = 8.0f, gap = r + 3.0f;
            const float sgn = b.bottom ? 1.f : -1.f;   // +Y de pantalla = hacia abajo
            fg->AddTriangleFilled(ImVec2(c.x - w, c.y + sgn * gap),
                                  ImVec2(c.x + w, c.y + sgn * gap),
                                  ImVec2(c.x,     c.y + sgn * (gap + h)),
                                  zcol);
        }
    }
}

// ============================================================================
// NEOTKO_COLORSTITCH_TAG — s280d: MARCADOR DE ÁNGULO AUTO (sustituye al parpadeo)
//
// s280b/s280c hacían parpadear el tejido de las bandas en auto entre sus dos
// direcciones reales. El mensaje era correcto, pero (a) cansa la vista y (b) obliga
// al canvas —que es de repintado BAJO DEMANDA— a repintar de continuo mientras haya
// una zona en auto delante. Aquí el aviso se muda al CONTORNO: violeta, latiendo
// hacia fuera y hacia dentro, sobre la misma geometría de borde que ya usa el realce
// de slot. El tejido se queda quieto en su ángulo base.
//
// PathBlend entra igual que ColorStitch, y era el caso importante: allí el ángulo NO
// es cosmético (`_t_of()` reparte la rampa por el centroide Y de cada línea), así que
// necesita el aviso más que nadie — y ahora lo recibe SIN que le toquemos el degradado.
// ============================================================================

// Misma lógica exacta que el preview (ver `weave_islands_for_volume`): PathBlend decide
// por `fill_angle < 0`; ColorStitch por la clave del kv vía `colorstitch_weave_theta`.
// Se delega en esos helpers a propósito — si un día cambia el criterio, cambia en un sitio.
bool GLGizmoColorStitchPainter::profile_angle_is_auto(const SurfaceEffectProfile& prof)
{
    namespace P = ColorStitchPaintPreview;
    PathBlendPassConfig pbc;
    if (P::pathblend_top_config(prof, pbc))
        return pbc.fill_angle < 0;
    const std::map<std::string, std::string> kv = P::colorstitch_top_kv(prof);
    if (kv.empty())
        return false;
    bool is_auto = false;
    P::colorstitch_weave_theta(kv, is_auto);
    return is_auto;
}

// Tope de slots procesados por reconstrucción. `get_facets` por slot sobre una malla
// grande no es gratis y esto se ensucia con cada trazo de pincel: sin tope, un plato
// como el BIGTEST (decenas de recetas) daría tirones al pintar. Los que no entran se
// cuentan igual y la leyenda lo dice, para que el tope nunca mienta por omisión.
static constexpr int CS_AUTO_MARKER_MAX_SLOTS = 24;

void GLGizmoColorStitchPainter::rebuild_auto_angle_marker_if_dirty()
{
    if (!m_auto_dirty)
        return;
    m_auto_dirty  = false;
    m_auto_parts.clear();
    m_auto_facets = 0;
    m_auto_slots  = 0;

    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || mo->instances.empty())
        return;

    // Objeto ACTIVO y sus selectores VIVOS: el aviso tiene que seguir el trazo que se
    // está pintando ahora, no una copia serializada. (Limitación conocida, la misma que
    // el realce de s232: multi-instancia → sólo `instances.front()`.)
    const Transform3d      inst = mo->instances.front()->get_transformation().get_matrix();
    const auto&            mgr  = SurfaceEffectProfileManager::get();
    int                    done = 0;
    int                    vol_idx = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++vol_idx;
        if (vol_idx >= (int)m_triangle_selectors.size()) break;
        for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT; ++s) {
            const int pid = mv->colorstitch_slot_to_profile_id[s];
            if (pid == 0) continue;
            const SurfaceEffectProfile* prof = mgr.find(pid);
            if (!prof || !profile_angle_is_auto(*prof)) continue;
            ++m_auto_slots;
            if (done >= CS_AUTO_MARKER_MAX_SLOTS) continue;
            ++done;
            build_outline_part(m_triangle_selectors[vol_idx].get(), mv, mo,
                               inst * mv->get_matrix(), s, m_auto_parts, m_auto_facets);
        }
    }
}

void GLGizmoColorStitchPainter::render_auto_angle_marker()
{
    if (!m_auto_marker)
        return;
    rebuild_auto_angle_marker_if_dirty();
    if (m_auto_parts.empty())
        return;

    // Igual que el realce: el canvas sólo repinta por eventos, así que el latido
    // necesita pedir su fotograma. A diferencia del parpadeo del tejido, esto vive
    // sólo con el painter abierto (es un render del gizmo), que era la mitad del
    // problema que tenía el parpadeo.
    m_parent.request_extra_frame();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera&      camera = wxGetApp().plater()->get_camera();
    const Transform3d& view   = camera.get_view_matrix();

    // "Grow out, go back in": el latido escala el contorno respecto al centroide de su
    // propia parte. Va en la MATRIZ, no en la geometría — reconstruir vértices por
    // fotograma para animar sería tirar la caché de VBOs 60 veces por segundo.
    const float t     = float(ImGui::GetTime());
    const float phase = 0.5f - 0.5f * std::cos(t * 2.2f);   // 0..1 suave, sin tirón
    const double grow = 1.0 + 0.055 * double(phase);
    const float  alpha = 0.55f + 0.45f * phase;

    static float max_lw = 0.f;
    if (max_lw <= 0.f) {
        GLfloat range[2] = { 1.f, 1.f };
        glsafe(::glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range));
        max_lw = std::max(1.f, (float)range[1]);
    }

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    auto draw = [&](float a, float lw_want) {
        glsafe(::glLineWidth(std::min(lw_want, max_lw)));
        for (SlotHighlightPart& p : m_auto_parts) {
            // Centroide en MUNDO de esta parte, de la unión de sus AABB de isla. Escalar
            // respecto al origen del objeto haría que el contorno se fuera de paseo en vez
            // de respirar en su sitio.
            BoundingBoxf3 bb;
            for (const BoundingBoxf3& ib : p.islands)
                if (ib.defined) bb.merge(ib);
            Transform3d m = p.trafo;
            if (bb.defined) {
                const Vec3d c = bb.center();
                m = Geometry::translation_transform(c)
                  * Geometry::scale_transform(grow)
                  * Geometry::translation_transform(-c) * p.trafo;
            }
            shader->set_uniform("view_model_matrix", view * m);
            ColorRGBA col = CS_AUTO_ANGLE_RGBA; col[3] = a;
            for (GLModel* g : { p.edges_top.get(), p.edges_bottom.get() }) {
                if (!g) continue;
                g->set_color(col);
                g->render();
            }
        }
    };

    // Fantasma sin profundidad (se ve a través del objeto: si la zona en auto está
    // detrás, el aviso sigue llegando) + trazo sólido encima.
    glsafe(::glDisable(GL_DEPTH_TEST));
    draw(0.30f * alpha, 5.0f);
    glsafe(::glEnable(GL_DEPTH_TEST));
    draw(alpha, 9.0f);

    shader->stop_using();
    glsafe(::glDisable(GL_BLEND));
}

// Leyenda: dos filas, abajo a la izquierda del canvas. Sólo aparece cuando hay algo
// que explicar — una leyenda permanente que casi siempre sobra se vuelve invisible.
void GLGizmoColorStitchPainter::render_stitch_legend()
{
    const bool show_sel  = m_slot_highlight && !m_hl_parts.empty();
    const bool show_auto = m_auto_marker    && !m_auto_parts.empty();
    if (!show_sel && !show_auto)
        return;

    ImDrawList*  fg   = ImGui::GetForegroundDrawList();
    ImFont*      font = ImGui::GetFont();
    const float  fs   = ImGui::GetFontSize();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    const float pad = 10.f, sw = 13.f, gap = 8.f, lh = fs + 7.f;
    const int   rows = int(show_sel) + int(show_auto);

    std::string s_sel  = _u8L("Selected colour");
    std::string s_auto = _u8L("Sandwich without angle applied");
    if (show_auto && m_auto_slots > CS_AUTO_MARKER_MAX_SLOTS)
        s_auto += " (" + std::to_string(CS_AUTO_MARKER_MAX_SLOTS) + "/"
                + std::to_string(m_auto_slots) + ")";

    float wmax = 0.f;
    if (show_sel)  wmax = std::max(wmax, font->CalcTextSizeA(fs, FLT_MAX, 0.f, s_sel.c_str()).x);
    if (show_auto) wmax = std::max(wmax, font->CalcTextSizeA(fs, FLT_MAX, 0.f, s_auto.c_str()).x);

    const float w  = pad * 2.f + sw + gap + wmax;
    const float h  = pad * 1.4f + rows * lh;
    const float x0 = 16.f;
    const float y0 = disp.y - 16.f - h;

    fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(18, 18, 20, 205), 6.f);
    fg->AddRect      (ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(255, 255, 255, 40), 6.f);

    float y = y0 + pad * 0.7f;
    auto row = [&](ImU32 col, const std::string& label, bool pulsing) {
        const float cy = y + lh * 0.5f;
        // La muestra del marcador late igual que el contorno del viewport, para que
        // la leyenda se reconozca por el MOVIMIENTO y no sólo por el color.
        float r = sw * 0.5f;
        if (pulsing) {
            const float ph = 0.5f - 0.5f * std::cos(float(ImGui::GetTime()) * 2.2f);
            r *= 0.80f + 0.30f * ph;
        }
        fg->AddRectFilled(ImVec2(x0 + pad, cy - r), ImVec2(x0 + pad + r * 2.f, cy + r), col, 2.5f);
        fg->AddText(font, fs, ImVec2(x0 + pad + sw + gap, cy - fs * 0.5f),
                    IM_COL32(232, 232, 236, 255), label.c_str());
        y += lh;
    };
    if (show_sel)  row(cs_zone_u32(0, 255), s_sel, false);
    if (show_auto) row(IM_COL32(199, 92, 245, 255), s_auto, true);
}


EnforcerBlockerType GLGizmoColorStitchPainter::get_left_button_state_type() const
{
    if (m_erase_mode)
        return EnforcerBlockerType::NONE;
    // NEOTKO_COLORSTITCH_TAG — s118: perfil ENLAZADO por id primero (guardado, o auto
    // ya materializado). Antes ganaba m_has_active_recipe → ensure_active_slot dedup
    // por json, que con dos perfiles del mismo json podía pintar con el otro. Con el
    // binding live el id es la fuente: pintar usa el perfil seleccionado tal cual.
    // Materializa su slot en el objeto ACTIVO justo al pintar (tras cambiar de objeto
    // m_active_slot=0, así que se asigna aquí).
    if (m_selected_profile_id != 0) {
        const int slot = const_cast<GLGizmoColorStitchPainter*>(this)
                             ->slot_for_selected_profile(/*assign_if_missing=*/true);
        if (slot >= 1 && slot < MAX_SLOTS) return static_cast<EnforcerBlockerType>(slot);
        return EnforcerBlockerType::NONE;
    }
    // Color de trabajo nuevo sin enlazar (swatch predict / Pro custom): materializa
    // el slot AUTO al pintar por dedup (ensure_active_slot ENLAZA m_selected_profile_id,
    // así que los siguientes trazos caen por la rama de arriba).
    if (m_has_active_recipe) {
        const int slot = const_cast<GLGizmoColorStitchPainter*>(this)->ensure_active_slot();
        if (slot >= 1 && slot < MAX_SLOTS) return static_cast<EnforcerBlockerType>(slot);
        return EnforcerBlockerType::NONE;
    }
    if (m_active_slot >= 1 && m_active_slot < MAX_SLOTS)
        return static_cast<EnforcerBlockerType>(m_active_slot);
    return EnforcerBlockerType::NONE;
}

// ----------------------------------------------------------------------------
// Slot / profile resolution
// ----------------------------------------------------------------------------

// s233 — los cuerpos de fallback_color_for_id / color_for_profile se movieron a
// slic3r/GUI/ColorStitchPaintPreview.{hpp,cpp} para que la vista 3D normal (sin gizmo)
// pueda calcular el mismo color. Aquí sólo se importan los nombres, así que los
// call-sites de este fichero siguen igual.
// (fallback_color_for_id ya sólo lo usa color_for_profile, allí dentro.)
using Slic3r::GUI::ColorStitchPaintPreview::color_for_profile;

// ----------------------------------------------------------------------------
// Mini-sandwich preview (NEOTKO_PROFILE_TAG — Fase 6).
// Renders a profile's resolved SurfacePassStack (Top + Penu) inside the row
// using ImGui draw-list primitives — same visual language as the SandwichDialog
// (paint_chip / paint_preview in Tab.cpp): dark 45/45/45 background, real
// filament colours per tool, ColorStitch as vertical tool stripes, Solid as a
// flat block with T#, PathBlend as ramp/cap halves, None as a hatch.
// ----------------------------------------------------------------------------

// Real filament colour for a 0-based tool index, as ImU32. Grey fallback.
static ImU32 tool_col_u32(const std::vector<std::string>& fcolors, int tool0)
{
    if (tool0 >= 0 && tool0 < (int)fcolors.size() && !fcolors[tool0].empty()) {
        std::string s = fcolors[tool0];
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() >= 6) {
            const unsigned long rgb = std::strtoul(s.substr(0, 6).c_str(), nullptr, 16);
            return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
        }
    }
    return IM_COL32(128, 128, 128, 255);
}

// Participating ColorStitch tools for a pass (reads the pass's per-zone override
// kv; the injected payload at save time guarantees these keys are present).
static std::vector<int> cm_tools_from_pass(const SurfacePass& p, bool penu)
{
    const std::string pre = penu ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    std::vector<int> out;
    for (const char* s : { "tool_a", "tool_b", "tool_c", "tool_d" }) {
        const auto it = p.colorstitch.kv.find(pre + s);
        if (it != p.colorstitch.kv.end()) {
            try { const int v = std::stoi(it->second); if (v >= 0) out.push_back(v); }
            catch (...) {}
        }
    }
    if (out.empty()) out = { 0, 1 };
    return out;
}

// NEOTKO_COLORSTITCH_TAG — single source of truth for the ColorStitch per-line tool
// sequence used by ALL previews (3D weave + pro-tray strip), built with the SAME
// engine builders the slicer uses (ColorStitch.cpp:1326+): modes 1-3 →
// build_dithered_tools_2color/_3color / build_custom_bands over n_lines; mode 0 →
// pattern string (digit → 0-based physical tool). penu=false → top-role keys.
// Returns physical tool indices (length n_lines for modes 1-3; pattern length for
// mode 0 → caller tiles). Empty kv ⇒ empty result.
// s233 F3 — cuerpo en ColorStitchPaintPreview (lo comparte la vista 3D normal).
using ColorStitchPaintPreview::colorstitch_tool_sequence;

// Draw one pass inside rect [a,b].
static void draw_pass(ImDrawList* dl, ImVec2 a, ImVec2 b,
                      const SurfacePass& p, bool penu,
                      const std::vector<std::string>& fcolors)
{
    dl->AddRectFilled(a, b, IM_COL32(45, 45, 45, 255));
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    switch (p.kind) {
    case SurfacePassKind::Solid: {
        dl->AddRectFilled(a, b, tool_col_u32(fcolors, p.solid_tool));
        if (w > 14.f) {
            char t[8]; std::snprintf(t, sizeof(t), "T%d", p.solid_tool + 1);
            dl->AddText(ImVec2(a.x + 2.f, a.y), IM_COL32(255, 255, 255, 255), t);
        }
        break;
    }
    case SurfacePassKind::ColorStitch: {
        const std::vector<int> tools = cm_tools_from_pass(p, penu);
        const int n = (int)tools.size();
        for (int i = 0; i < n; ++i) {
            const float x0 = a.x + w * float(i)     / float(n);
            const float x1 = a.x + w * float(i + 1) / float(n);
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y),
                              tool_col_u32(fcolors, tools[i]));
        }
        break;
    }
    case SurfacePassKind::PathBlend: {
        PathBlendPassConfig pbc;
        const auto it = p.pathblend.kv.find("blob");
        if (it != p.pathblend.kv.end() && !it->second.empty())
            pbc = PathBlendPassConfig::from_blob_json(it->second);
        const float midy = a.y + h * 0.5f;
        // Bottom half = ramp tool (always present).
        dl->AddRectFilled(ImVec2(a.x, midy), b, tool_col_u32(fcolors, pbc.tool_bottom));
        // Top half = cap tool (Full only).
        if (pbc.mode == PathBlendPassConfig::Mode::Full)
            dl->AddRectFilled(a, ImVec2(b.x, midy), tool_col_u32(fcolors, pbc.tool_top));
        break;
    }
    default: // None — diagonal hatch
        for (float x = a.x - h; x < b.x; x += 6.f)
            dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), IM_COL32(110, 110, 110, 255));
        break;
    }
    dl->AddRect(a, b, IM_COL32(20, 20, 20, 255));
}

// Draw a zone's stack (bottom->top) stacked vertically inside rect [a,b],
// weighted by pass ratio (equal split when ratios are unset).
static void draw_zone(ImDrawList* dl, ImVec2 a, ImVec2 b,
                      const SurfacePassStack& st, bool penu,
                      const std::vector<std::string>& fcolors)
{
    const int n = (int)st.passes.size();
    if (n == 0) {
        dl->AddRectFilled(a, b, IM_COL32(60, 60, 60, 255));
        dl->AddRect(a, b, IM_COL32(20, 20, 20, 255));
        return;
    }
    const float h = b.y - a.y;
    double total = 0.0;
    for (const auto& p : st.passes) total += std::max(0.0, p.ratio);
    const bool weighted = total > 1e-6;
    float y = b.y; // start at the bottom (pass[0] is the bottom-most pass)
    for (int i = 0; i < n; ++i) {
        const double frac = weighted ? (std::max(0.0, st.passes[i].ratio) / total)
                                     : (1.0 / double(n));
        const float band = (float)(frac * h);
        const float y1 = y;
        const float y0 = (i == n - 1) ? a.y : (y - band);
        draw_pass(dl, ImVec2(a.x, y0), ImVec2(b.x, y1), st.passes[i], penu, fcolors);
        y = y0;
    }
}

// One-line textual description of a zone: "+" -joined per-pass tokens.
static std::string zone_desc(const SurfacePassStack& st)
{
    if (st.passes.empty()) return "—"; // em dash
    std::string s;
    for (const auto& p : st.passes) {
        if (!s.empty()) s += "+";
        switch (p.kind) {
        case SurfacePassKind::Solid:     s += "T" + std::to_string(p.solid_tool + 1); break;
        case SurfacePassKind::ColorStitch:  s += "CS"; break;   // s280f: era "CM", resto del rebrand
        case SurfacePassKind::PathBlend: s += "PB"; break;
        default:                         s += "·"; break; // middle dot
        }
    }
    return s;
}

// NEOTKO_COLORSTITCH_TAG — s280f: ángulo de relleno de una zona, para el tooltip de la
// paleta. Sin esto el ángulo de una receta sólo se podía ver entrando en Pro, así que al
// repartir ángulos distintos entre recetas no había forma de saber cuál llevaba cuál.
//
// Devuelve false si la zona no tiene ningún efecto con ángulo (Solid / vacía).
// `out_deg` < 0 ⇒ AUTO (el mismo -1 que marca el contorno violeta en el viewport).
//
// Misma resolución que el aviso "auto angle" del panel ADV, a propósito: si divergieran,
// el tooltip y la etiqueta del panel dirían cosas distintas del mismo perfil. Clave
// ausente ⇒ el default (45), NO auto — ver la nota de s280b sobre por qué el fallback -1
// encendía el aviso en todas las bandas.
static bool cs_zone_angle(const SurfacePassStack& st, bool penu, int& out_deg)
{
    namespace P = ColorStitchPaintPreview;
    Slic3r::PathBlendPassConfig pbc;
    if (P::pathblend_from_stack(st, pbc)) { out_deg = pbc.fill_angle; return true; }
    const std::map<std::string, std::string> kv = P::colorstitch_kv_from_stack(st);
    if (kv.empty()) return false;
    auto it = kv.find(penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle");
    if (it == kv.end() && penu) it = kv.find("interlayer_colormix_angle");
    out_deg = COLORSTITCH_DEFAULT_ANGLE_DEG;
    if (it != kv.end()) { try { out_deg = std::stoi(it->second); } catch (...) {} }
    return true;
}

// NEOTKO_COLORSTITCH_TAG_START — PR.2 render + PR.3 click. Sección colapsable
// con una tira de swatches scroll-horizontal; hover muestra el mini-sandwich
// (draw_zone) + desc. Devuelve el índice de swatch clicado (-1 si ninguno) —
// el caller lo fija como color activo (set_active_recipe).
// s231 F5 — `sel_idx`: qué swatch de ESTA tira es el color activo. Antes la tira sólo
// conocía hover, así que elegir un color en el Generator no dejaba marca en ninguna
// parte (set_active_recipe desenlaza el perfil, así que la rejilla de guardados
// tampoco resaltaba nada) y el usuario perdía de vista con qué estaba pintando.
static int draw_palette_strip(const char* id,
                              const std::vector<Slic3r::ColorSci::ColorRecipe>& pal,
                              const std::vector<std::string>& fcolors,
                              float strip_w, float /*strip_h*/,
                              int sel_idx = -1)
{
    // s318 — de tira con scroll horizontal a REJILLA de 8 por fila en un pozo Canvas: las tres tiras
    // del Generator (y Flat color, que era la que hacía scroll) miden igual y se ven enteras.
    // Mismo contrato: devuelve el índice pulsado, tooltip con la cajita sandwich.
    if (pal.empty()) {
        ImGui::TextDisabled("—");
        return -1;
    }
    int clicked = -1;
    const int    per  = 8;
    const float  pad  = 5.f, gap = 4.f;
    const float  w    = (strip_w > 0.f) ? strip_w : ImGui::GetContentRegionAvail().x;
    const float  sw   = std::max(8.f, std::floor((w - 2.f * pad - gap * float(per - 1)) / float(per)));
    const int    rows = (int(pal.size()) + per - 1) / per;
    const float  h    = 2.f * pad + float(rows) * sw + float(rows - 1) * gap;
    ImDrawList*  dl   = ImGui::GetWindowDrawList();
    const ImVec2 p0   = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), neo_col_u32(NeoCol::Canvas), 5.f);
    ImGui::PushID(id);
    for (int i = 0; i < (int)pal.size(); ++i) {
        ImGui::PushID(i);
        const ImVec2 p(p0.x + pad + float(i % per) * (sw + gap), p0.y + pad + float(i / per) * (sw + gap));
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw))) clicked = i;
        const bool hov = ImGui::IsItemHovered();
        const auto& c = pal[i].rgb;
        const ImU32 col = IM_COL32((int)std::min(255.f, c[0] * 255.f),
                                   (int)std::min(255.f, c[1] * 255.f),
                                   (int)std::min(255.f, c[2] * 255.f), 255);
        dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw), col, 3.f);
        if (i == sel_idx)
            neo_sel_ring(dl, p, ImVec2(p.x + sw, p.y + sw), 3.f);
        else if (hov)
            dl->AddRect(p, ImVec2(p.x + sw, p.y + sw), neo_col_u32(NeoCol::TextDim), 3.f, 0, 1.f);
        if (hov) {
            ImGui::BeginTooltip();
            const ImVec2 tp = ImGui::GetCursorScreenPos();
            const float zw = 64.f, zh = 44.f;
            ImGui::Dummy(ImVec2(zw, zh));
            ImDrawList* tdl = ImGui::GetWindowDrawList();
            const Slic3r::SurfacePassStack& top  = pal[i].top;
            const Slic3r::SurfacePassStack& penu = pal[i].penu;
            if (penu.passes.empty()) {
                draw_zone(tdl, tp, ImVec2(tp.x + zw, tp.y + zh), top, false, fcolors);
            } else {
                const float midy = tp.y + zh * 0.5f;
                draw_zone(tdl, tp, ImVec2(tp.x + zw, midy - 1.f), top,  false, fcolors);
                draw_zone(tdl, ImVec2(tp.x, midy + 1.f), ImVec2(tp.x + zw, tp.y + zh), penu, true, fcolors);
            }
            if (!pal[i].desc.empty()) ImGui::TextUnformatted(pal[i].desc.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
    ImGui::Dummy(ImVec2(w, 0.f));
    return clicked;
}



// ---------------------------------------------------------------------------
// Pro mode v2 (s108) — pass rows that mirror the SandwichDialog row anatomy:
// [x] · #N · chips · kind badge · kind selector · per-kind inline fields ·
// hatched preview bar · Z box on the right edge. All three kinds (Solid /
// ColorStitch / PathBlend Half|Full) are editable inline. The produced stack
// is SELF-CONTAINED — ColorStitch uses the long region keys in pass.colorstitch.kv
// (mirror of ColorPredict.cpp penu_dither, the engine-validated shape) and
// PathBlend the v=2 blob in pass.pathblend.kv["blob"] — so a profile saved
// from the tray slices exactly like one authored in the Sandwich Editor.
// ---------------------------------------------------------------------------


// PB blob round-trip on a pass. Self-contained (kv only) — unlike the dialog's
// read_pb_blob/write_pb_blob there is no live region config to mirror here.
// s233 F3 — cuerpo en ColorStitchPaintPreview.
using ColorStitchPaintPreview::pro_pb_read;

static void pro_pb_write(SurfacePass& p, PathBlendPassConfig pbc, double layer_h)
{
    pbc.apply_constraints(layer_h);   // shared rule set (engine, s108)
    pbc.sync_legacy_view();
    p.pathblend.present    = true;
    p.pathblend.kv["blob"] = pbc.to_blob_json();
}

// NEOTKO_PATHBLEND_TAG — s190 profile (Img 2/3). True when the ramp uses a
// non-linear profile (start/end zone moved away from 0/1).
static bool pro_pb_is_profiled(const PathBlendPassConfig& pbc)
{
    return !(pbc.in_t <= 0.001f && pbc.out_t >= 0.999f);
}

// NEOTKO_PATHBLEND_TAG — s191. Unified visual editor: a layer cross-section
// (X = surface position t 0..1, Y = height 0..H mm). Two draggable 2D handles:
//   • low (blue)   = (in_t, floor)     → X sets the start zone, Y the floor
//   • high (orange)= (out_t, ramp_end) → X sets the end zone,   Y the ramp top
// The ramp region (bottom tool) is shaded blue, the cap (top tool, Full only)
// orange. When the high handle is dragged to the top the cap vanishes (a
// "techo"). Half locks ramp_end = H (Y not draggable). Returns true on change.
static bool pro_pb_profile_editor(PathBlendPassConfig& pbc, double H, bool is_half)
{
    bool changed = false;
    const float W = 244.f, Hc = 168.f, pad = 14.f;
    ImDrawList*  dl  = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 p0(org.x + pad, org.y + pad);
    const ImVec2 p1(org.x + W - pad, org.y + Hc - pad);
    const float  pw = p1.x - p0.x, ph = p1.y - p0.y;
    ImGui::InvisibleButton("##pb_prof_canvas", ImVec2(W, Hc));

    const double Hd = std::max(0.04, H);
    auto sx = [&](double t) { return p0.x + float(std::clamp(t, 0.0, 1.0)) * pw; };
    auto sy = [&](double z) { return p1.y - float(std::clamp(z / Hd, 0.0, 1.0)) * ph; };

    // Resolve geometry (auto sentinel <0 → display value).
    double a  = std::clamp((double)pbc.in_t,  0.0, 1.0);
    double b  = std::clamp((double)pbc.out_t, 0.0, 1.0);
    if (b < a + 0.02) b = std::min(1.0, a + 0.02);
    double fl = std::clamp((double)std::max(0.01f, pbc.floor_mm), 0.01, Hd - 0.001);
    double re = (pbc.mid_end_mm < 0.f) ? (is_half ? Hd : Hd - 0.04) : (double)pbc.mid_end_mm;
    if (is_half) re = Hd;
    re = std::clamp(re, fl + 0.001, Hd);

    dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 32, 255), 3.f);

    const ImU32 col_ramp = IM_COL32( 90, 170, 255, 70);   // bottom tool
    const ImU32 col_cap  = IM_COL32(255, 170,  90, 55);   // top tool (Full)
    // Ramp region (below the curve).
    dl->AddRectFilled(ImVec2(sx(0), sy(fl)), ImVec2(sx(a), sy(0)), col_ramp);
    dl->AddQuadFilled(ImVec2(sx(a), sy(0)), ImVec2(sx(b), sy(0)),
                      ImVec2(sx(b), sy(re)), ImVec2(sx(a), sy(fl)), col_ramp);
    dl->AddRectFilled(ImVec2(sx(b), sy(re)), ImVec2(sx(1), sy(0)), col_ramp);
    // Cap region (above the curve up to H) — Full only.
    if (!is_half) {
        dl->AddRectFilled(ImVec2(sx(0), sy(Hd)), ImVec2(sx(a), sy(fl)), col_cap);
        dl->AddQuadFilled(ImVec2(sx(a), sy(fl)), ImVec2(sx(b), sy(re)),
                          ImVec2(sx(b), sy(Hd)), ImVec2(sx(a), sy(Hd)), col_cap);
        dl->AddRectFilled(ImVec2(sx(b), sy(Hd)), ImVec2(sx(1), sy(re)), col_cap);
    }
    dl->AddRect(p0, p1, IM_COL32(90, 90, 100, 255), 3.f);

    const ImU32 line = IM_COL32(120, 200, 255, 255);
    dl->AddLine(ImVec2(sx(0), sy(fl)), ImVec2(sx(a), sy(fl)), line, 2.f);
    dl->AddLine(ImVec2(sx(a), sy(fl)), ImVec2(sx(b), sy(re)), line, 2.f);
    dl->AddLine(ImVec2(sx(b), sy(re)), ImVec2(sx(1), sy(re)), line, 2.f);

    const ImVec2 h_lo(sx(a), sy(fl)), h_hi(sx(b), sy(re));
    const ImVec2 m = ImGui::GetIO().MousePos;
    static int drag = -1;                          // one popup open at a time
    if (ImGui::IsItemActivated()) {
        const float dlo = (m.x-h_lo.x)*(m.x-h_lo.x) + (m.y-h_lo.y)*(m.y-h_lo.y);
        const float dhi = (m.x-h_hi.x)*(m.x-h_hi.x) + (m.y-h_hi.y)*(m.y-h_hi.y);
        drag = (dlo <= dhi) ? 0 : 1;
    }
    if (ImGui::IsItemActive()) {
        const double mt = std::clamp((double)(m.x - p0.x) / pw, 0.0, 1.0);
        const double mz = std::clamp((double)(p1.y - m.y) / ph, 0.0, 1.0) * Hd;
        if (drag == 0) {
            pbc.in_t     = (float)std::clamp(mt, 0.0, b - 0.02);
            pbc.floor_mm = (float)std::clamp(mz, 0.01, re - 0.001);
        } else {
            pbc.out_t    = (float)std::clamp(mt, a + 0.02, 1.0);
            if (!is_half) pbc.mid_end_mm = (float)std::clamp(mz, fl + 0.001, Hd);
        }
        changed = true;
    }
    dl->AddCircleFilled(h_lo, 6.f, IM_COL32( 90, 170, 255, 255));
    dl->AddCircleFilled(h_hi, 6.f, IM_COL32(255, 170,  90, 255));
    dl->AddCircle      (h_lo, 6.f, IM_COL32(255, 255, 255, 255));
    dl->AddCircle      (h_hi, 6.f, IM_COL32(255, 255, 255, 255));

    ImGui::PushItemWidth(60.f);
    { float v = pbc.in_t;
      if (ImGui::DragFloat("start##pbi", &v, 0.005f, 0.f, 0.98f, "%.2f")) {
          pbc.in_t = std::clamp(v, 0.f, pbc.out_t - 0.02f); changed = true; } }
    ImGui::SameLine();
    { float v = pbc.out_t;
      if (ImGui::DragFloat("end##pbo", &v, 0.005f, 0.02f, 1.f, "%.2f")) {
          pbc.out_t = std::clamp(v, pbc.in_t + 0.02f, 1.f); changed = true; } }
    { float v = (float)fl;
      if (ImGui::DragFloat("floor##pbf", &v, 0.005f, 0.01f, (float)Hd, "%.2f")) {
          pbc.floor_mm = std::clamp(v, 0.01f, (float)re - 0.001f); changed = true; } }
    ImGui::SameLine();
    if (is_half) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("ramp %.2f", Hd);
    } else {
        float v = (float)re;
        if (ImGui::DragFloat("ramp##pbr", &v, 0.005f, (float)fl + 0.001f, (float)Hd, "%.2f")) {
            pbc.mid_end_mm = std::clamp(v, (float)fl + 0.001f, (float)Hd); changed = true; }
    }
    ImGui::PopItemWidth();
    return changed;
}

// ColorStitch kv — SAME canonical self-contained shape the Mixed palette emits
// (ColorPredict.cpp penu_dither): zone pattern long key + tool_a / tool_b.
static const char* pro_cm_pattern_key(bool penu)
{
    return penu ? "interlayer_colormix_pattern_penultimate"
                : "interlayer_colormix_pattern_top";
}
static std::string pro_cm_tool_key(bool penu, char ab)
{
    return std::string(penu ? "interlayer_colormix_penu_tool_"
                            : "interlayer_colormix_tool_") + ab;
}
static std::string pro_cm_pattern(const SurfacePass& p, bool penu)
{
    auto it = p.colorstitch.kv.find(pro_cm_pattern_key(penu));
    if (it != p.colorstitch.kv.end() && !it->second.empty()) return it->second;
    it = p.colorstitch.kv.find("pattern");   // short per-pass editor key
    if (it != p.colorstitch.kv.end()) return it->second;
    return std::string();
}
static void pro_cm_read(const SurfacePass& p, bool penu, int& a, int& b, int& pct_b)
{
    auto rd = [&](const std::string& k, int def) {
        const auto it = p.colorstitch.kv.find(k);
        if (it != p.colorstitch.kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return def;
    };
    a = std::clamp(rd(pro_cm_tool_key(penu, 'a'), 0), 0, 3);
    b = std::clamp(rd(pro_cm_tool_key(penu, 'b'), 1), 0, 3);
    pct_b = 50;
    const std::string pat = pro_cm_pattern(p, penu);
    if (!pat.empty()) {
        int nb = 0, tot = 0;
        for (char c : pat) {
            const int t = c - '1';
            if (t >= 0 && t < 4) { ++tot; if (t == b) ++nb; }
        }
        if (tot > 0) pct_b = (int)std::lround(100.0 * nb / tot);
    }
}
// Mix% → evenly interleaved digit pattern (Bresenham), length 8 → 12.5% steps.
static void pro_cm_write(SurfacePass& p, bool penu, int a, int b, int pct_b)
{
    constexpr int L = 8;
    const int nb = std::clamp((int)std::lround(pct_b * L / 100.0), 0, L);
    std::string pat; pat.reserve(L);
    int acc = 0;
    for (int i = 0; i < L; ++i) {
        acc += nb;
        if (acc >= L) { acc -= L; pat += char('1' + b); }
        else          {           pat += char('1' + a); }
    }
    p.colorstitch.present = true;
    p.colorstitch.kv[pro_cm_pattern_key(penu)]   = pat;
    p.colorstitch.kv[pro_cm_tool_key(penu, 'a')] = std::to_string(a);
    p.colorstitch.kv[pro_cm_tool_key(penu, 'b')] = std::to_string(b);
    p.solid_tool = a;   // chip/desc fallback — mirrors ColorPredict penu_dither
}

// s171 — "ColorStitch Pattern Color": reemplaza al generador "Mixed (ColorStitch)"
// (nukeado — venía de predict_mixed_palette, una ruta que llevaba desde s120
// "muerta" en el painter y que siempre predecía amarillo en el preview TD).
// Genera degradados usando SOLO ColorStitch: 2 pasadas (Top + Penu, cada una a
// ratio 1.0 = sobrepuestas, sin split de Z), los MISMOS 2 tools y el MISMO
// ángulo (-1 = auto en ambas) en las dos, barriendo el mix ColorStitch (pct_b)
// de 0% (puro A) a 100% (puro B) entre swatches. Reusa pro_cm_write — el mismo
// kv self-contained que ya usa (y valida al slicear) todo el Pro mode.
static std::vector<Slic3r::ColorSci::ColorRecipe> build_colorstitch_gradient_palette(
        const Slic3r::ColorSci::Material mats[4],
        const Slic3r::ColorSci::PredictOptions& opt,
        int tool_a, int tool_b, int steps)
{
    using namespace Slic3r;
    std::vector<ColorSci::ColorRecipe> out;
    steps = std::max(2, steps);
    out.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const int pct_b = (int)std::lround(100.0 * i / (steps - 1));

        SurfacePass top_pass;
        top_pass.kind  = SurfacePassKind::ColorStitch;
        top_pass.ratio = 1.0;
        pro_cm_write(top_pass, /*penu=*/false, tool_a, tool_b, pct_b);

        SurfacePass penu_pass;
        penu_pass.kind  = SurfacePassKind::ColorStitch;
        penu_pass.ratio = 1.0;
        pro_cm_write(penu_pass, /*penu=*/true, tool_a, tool_b, pct_b);

        SurfacePassStack top, penu;
        top.enabled  = true; top.passes  = { top_pass };
        penu.enabled = true; penu.passes = { penu_pass };

        ColorSci::ColorRecipe r;
        r.top  = top;
        r.penu = penu;
        ColorSci::sandwich_colour_stacked(top, penu, mats, opt.bg_rgb, opt.layer_height, r.rgb.data());
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d%%", pct_b);
        r.desc = buf;
        out.push_back(std::move(r));
    }
    return out;
}

// NEOTKO_COLORSTITCH_TAG — s118: un pase ColorStitch DEBE llevar su payload (pattern/A/B)
// o el motor lo degrada a Solid (síntoma: penu pintado "sin efecto" al slicear). Las
// recetas que llegan de tiras predict o de perfiles cargados pueden traer kind=ColorStitch
// con kv VACÍO; como pro_cm_write sólo se llamaba al CAMBIAR A/B, el payload se guardaba
// vacío. Rellena cada pase ColorStitch sin payload con sus valores efectivos (pro_cm_read
// da defaults si está vacío) → pase autocontenido que el motor sí pinta como ColorStitch.
static void pro_backfill_cm(Slic3r::SurfacePassStack& st, bool penu)
{
    for (Slic3r::SurfacePass& p : st.passes) {
        if (p.kind != Slic3r::SurfacePassKind::ColorStitch) continue;
        if (!pro_cm_pattern(p, penu).empty()) continue;   // ya tiene payload
        int a, b, cp; pro_cm_read(p, penu, a, b, cp);
        // Honrar el tool ya fijado (solid_tool) como A; B distinto para que la mezcla
        // sea visible. Mejor que los defaults 0/1 cuando el pase traía solid_tool.
        if (p.solid_tool >= 0 && p.solid_tool <= 3) a = p.solid_tool;
        if (b == a) b = (a + 1) & 3;
        pro_cm_write(p, penu, a, b, cp);
    }
}

// NEOTKO_COLORSTITCH_TAG — s231 F4: traduce el payload ColorStitch de una zona a las
// claves de OTRA al copiarla. Top/Bottom usan `interlayer_colormix_*` y el Penúltimo
// `interlayer_colormix_penu_*`: copiar el stack tal cual dejaría los pases sin patrón
// legible para la zona destino y el motor los degradaría a Solid (mismo fallo que s118
// documentó para los ColorStitch con kv vacío). Se conservan tools, mezcla y ángulo.
// 🔧 s317 — antes hacía kv.clear() y reescribía SÓLO A/B/mezcla/ángulo: el modo (bandas o
// degradado), los anchos en mm, gradient_span_mm, easing, invert, repeticiones, tools C/D… se
// perdían y la copia salía como un dither A/B de 8 dígitos. Ahora TRADUCE cada clave de receta
// del rol origen al destino (mismo juego que colorstitch_keys() por rol) y deja intactas las que
// no son de rol. Sólo un pase SIN receta cae al camino viejo (defaults vía pro_cm_read/write).
static void pro_retarget_cm(Slic3r::SurfacePassStack& st, bool from_penu, bool to_penu)
{
    if (from_penu == to_penu) return;   // Top ↔ Bottom comparten claves: nada que hacer
    static const char* const kRoleSuffixes[] = {
        "mode", "pct_a", "pct_b", "easing", "gamma", "min_surface_lines", "overlap", "invert",
        "repetitions", "band_count_a", "band_count_b", "band_count_c", "band_count_d",
        "band_mm_a", "band_mm_b", "band_mm_c", "band_mm_d", "gradient_span_mm",
        "tool_a", "tool_b", "tool_c", "tool_d", "angle" };
    const std::string fp = from_penu ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    const std::string tp = to_penu   ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    const std::string fpat = pro_cm_pattern_key(from_penu), tpat = pro_cm_pattern_key(to_penu);
    auto is_role_key = [&](const std::string& k, const std::string& pre, const std::string& pat) {
        if (k == pat) return true;
        for (const char* s : kRoleSuffixes) if (k == pre + s) return true;
        return false;
    };
    for (Slic3r::SurfacePass& p : st.passes) {
        if (p.kind != Slic3r::SurfacePassKind::ColorStitch) continue;
        if (p.colorstitch.kv.empty()) {           // sin receta: el camino de siempre (defaults)
            int a, b, pct; pro_cm_read(p, from_penu, a, b, pct);
            pro_cm_write(p, to_penu, a, b, pct);
            p.colorstitch.kv[tp + "angle"] = std::to_string(p.angle);
            continue;
        }
        std::map<std::string, std::string> out;
        // 1) lo que no es de rol (flags globales, "pattern" corto del editor por pase) tal cual;
        //    las claves de rol de AMBAS zonas fuera, para que no se mezclen dos recetas.
        for (const auto& [k, v] : p.colorstitch.kv)
            if (!is_role_key(k, fp, fpat) && !is_role_key(k, tp, tpat)) out[k] = v;
        // 2) las del rol origen, renombradas al destino.
        for (const auto& [k, v] : p.colorstitch.kv) {
            if (k == fpat) { out[tpat] = v; continue; }
            for (const char* s : kRoleSuffixes)
                if (k == fp + s) { out[tp + s] = v; break; }
        }
        p.colorstitch.kv = std::move(out);
        p.colorstitch.present = true;
        const auto it = p.colorstitch.kv.find(tp + "angle");
        if (it != p.colorstitch.kv.end()) { try { p.angle = std::stoi(it->second); } catch (...) {} }
        else p.colorstitch.kv[tp + "angle"] = std::to_string(p.angle);
    }
}

// s169 F3 — una línea de texto por pase para el "Live recipe" del departamento
// Object: "#N KIND detalle · X.XX mm" (solo lectura). Composición estilo
// zone_desc + el ratio convertido a mm real (p.ratio*lh) — el mismo dato que ya
// muestra el Z-box de draw_zone_editor, aquí en una sola línea.
static std::string pass_desc_line(int n, const Slic3r::SurfacePass& p, bool penu, double lh)
{
    using K = Slic3r::SurfacePassKind;
    std::string kind_detail;
    switch (p.kind) {
    case K::Solid:
        kind_detail = "SOLID T" + std::to_string(p.solid_tool + 1);
        break;
    case K::ColorStitch: {
        int a, b, pct; pro_cm_read(p, penu, a, b, pct);
        kind_detail = "CM T" + std::to_string(a + 1) + "/T" + std::to_string(b + 1);
        break;
    }
    case K::PathBlend: {
        const PathBlendPassConfig pbc = pro_pb_read(p);
        const bool is_half = pbc.mode == PathBlendPassConfig::Mode::Half;
        kind_detail = "PB T" + std::to_string(std::max(0, pbc.tool_bottom) + 1);
        if (!is_half)
            kind_detail += "→T" + std::to_string(std::max(0, pbc.tool_top) + 1);
        kind_detail += is_half ? " (half)" : " (full)";
        break;
    }
    default:
        kind_detail = "·";
        break;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", p.ratio * lh);
    return "#" + std::to_string(n) + " " + kind_detail + " · " + buf + " mm";
}

// Small filament chip; click opens a popup with the nfil tool chips.
// Returns true when `tool` changed.
static bool pro_tool_chip(const char* id, const std::vector<std::string>& fcolors,
                          int nfil, int& tool, const char* tip)
{
    // s318 — la ficha numerada del lenguaje nuevo (número dentro, tinta por luminancia) y, al
    // pulsarla, la tira de fichas de GizmoNeotkoStyle en el popup. Mismo contrato que antes.
    bool changed = false;
    ImGui::PushID(id);
    const float  u  = neo_u();
    const float  sw = 1.25f * u;
    const float  fh = ImGui::GetFrameHeight();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##chip", ImVec2(sw, fh)))
        ImGui::OpenPopup("##pick");
    const bool hov = ImGui::IsItemHovered();
    ImDrawList*  dl  = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + (fh - sw) * 0.5f), b(a.x + sw, a.y + sw);
    const ImU32  col = tool_col_u32(fcolors, tool);
    dl->AddRectFilled(a, b, col, 3.f);
    char num[4];
    std::snprintf(num, sizeof(num), "%d", tool + 1);
    const float  fs = 0.7f * u;
    const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
    dl->AddText(ImGui::GetFont(), fs, ImVec2((a.x + b.x - ns.x) * 0.5f, (a.y + b.y - ns.y) * 0.5f), ink_on(col), num);
    if (hov)
        dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 3.f, 0, 1.4f);
    if (hov && tip) ImGui::SetTooltip("%s (T%d)", tip, tool + 1);
    if (ImGui::BeginPopup("##pick")) {
        ImU32 cols[4];
        const int nn = std::max(1, std::min(nfil, 4));
        for (int t = 0; t < nn; ++t) cols[t] = tool_col_u32(fcolors, t);
        const int c = neo_tool_chips("##pk", cols, nn, tool);
        if (c >= 0) { tool = c; changed = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// s318 — ficha mini de SÓLO LECTURA (los dos filamentos de un ColorStitch en su fila: el editor
// de patrón es quien los cambia). Mismo dibujo que las fichas, sin botón.
static void cs_mini_tool(const std::vector<std::string>& fcolors, int t)
{
    const float  u  = neo_u();
    const float  s  = 1.05f * u;
    const float  fh = ImGui::GetFrameHeight();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(s, fh));
    ImDrawList*  dl  = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + (fh - s) * 0.5f), b(a.x + s, a.y + s);
    const ImU32  col = tool_col_u32(fcolors, t < 0 ? 0 : t);
    dl->AddRectFilled(a, b, col, 3.f);
    char num[4];
    std::snprintf(num, sizeof(num), "%d", (t < 0 ? 0 : t) + 1);
    const float  fs = 0.62f * u;
    const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
    dl->AddText(ImGui::GetFont(), fs, ImVec2((a.x + b.x - ns.x) * 0.5f, (a.y + b.y - ns.y) * 0.5f), ink_on(col), num);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("T%d", (t < 0 ? 0 : t) + 1);
}


// Preview bar for one pass — Solid: fill-angle hatch (wheel rotates);
// ColorStitch: interleaved tool bands straight from the pattern digits (same
// data flatten_stack consumes → zero divergence with the Result swatch);
// PathBlend: SAME Z-geometry as Tab.cpp paint_preview's Kind::PathBlend branch
// (dark bg = whole layer, ramp = eased curve from floor to ramp-end, cap =
// flat band above ramp-end in Full mode) — this is how the SandwichDialog
// shows "how the layer will be built up", not a cosmetic gradient.
static void pro_pass_preview(ImDrawList* dl, ImVec2 a, ImVec2 b,
                             const SurfacePass& p, bool penu,
                             const std::vector<std::string>& fcolors,
                             double layer_h)
{
    dl->AddRectFilled(a, b, neo_col_u32(NeoCol::Canvas));   // s318 — token, no gris a pelo
    switch (p.kind) {
    case SurfacePassKind::Solid: {
        const ImU32 col = tool_col_u32(fcolors, p.solid_tool);
        const float deg = (p.angle < 0) ? 45.f : (float)p.angle;
        const float ang = deg * 3.14159265f / 180.f;
        const float dx = std::cos(ang), dy = -std::sin(ang);   // screen Y down
        const float nx = -dy, ny = dx;
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float ext = 0.5f * (float)std::hypot(b.x - a.x, b.y - a.y) + 4.f;
        dl->PushClipRect(a, b, true);
        const int nlines = (int)(ext / 5.f) + 1;
        for (int i = -nlines; i <= nlines; ++i) {
            const float ox = c.x + nx * 5.f * (float)i;
            const float oy = c.y + ny * 5.f * (float)i;
            dl->AddLine(ImVec2(ox - dx * ext, oy - dy * ext),
                        ImVec2(ox + dx * ext, oy + dy * ext), col, 1.5f);
        }
        dl->PopClipRect();
        break;
    }
    case SurfacePassKind::ColorStitch: {
        // Unified with the 3D weave: the SAME per-line tool sequence the slicer
        // produces (dither / gradient / bands / pattern), not the raw digits.
        // NEOTKO_COLORSTITCH_TAG — bands drawn at the REAL fill angle so the preview
        // rotates with the wheel (mirrors the Solid hatch + the 3D weave).
        const float bw     = 4.f;
        const int   nbands = (int)(std::hypot(b.x - a.x, b.y - a.y) / bw) + 2;
        // NEOTKO_COLORSTITCH_TAG — s314: en modo 4 (bandas en mm) la barra pasa a tener
        // ESCALA. Cada celda de bw px representa una porción fija de milímetros, así que la
        // barra siempre enseña la misma ventana física (kBarMM) y una banda de 4 mm se ve
        // justo la mitad que una de 8 mm. Sin esto la barra dibujaba una celda por línea
        // impresa y el diseño no cabía. Los modos 0-3 pasan 0 y no se enteran.
        double bar_spacing_mm = 0.0;
        {
            const auto it = p.colorstitch.kv.find(penu ? "interlayer_colormix_penu_mode"
                                                       : "interlayer_colormix_mode");
            int m4 = 0;
            if (it != p.colorstitch.kv.end()) { try { m4 = std::stoi(it->second); } catch (...) {} }
            if (m4 == 4) {
                constexpr double kBarMM = 40.0;
                bar_spacing_mm = kBarMM / std::max(2, nbands);
            }
        }
        std::vector<int> seq = colorstitch_tool_sequence(p.colorstitch.kv, penu,
                                                         std::max(2, nbands), bar_spacing_mm);
        if (seq.empty()) seq.push_back(0);
        // angle from the pass kv (auto=-1 → display at 45°). Bands run ALONG the fill
        // lines, so band boundaries step across the perpendicular axis.
        int adeg = -1;
        { const auto it = p.colorstitch.kv.find(penu ? "interlayer_colormix_penu_angle"
                                                  : "interlayer_colormix_angle");
          if (it != p.colorstitch.kv.end()) { try { adeg = std::stoi(it->second); } catch (...) {} } }
        const float deg = (adeg < 0) ? 45.f : (float)adeg;     // = cm_angle (matches slice + weave)
        const float ang = deg * 3.14159265f / 180.f;
        const float dx = std::cos(ang), dy = -std::sin(ang);   // stripe dir (screen Y down)
        const float nx = -dy, ny = dx;                         // across the lines
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float ext = 0.5f * (float)std::hypot(b.x - a.x, b.y - a.y) + 4.f;
        const int   ns  = (int)seq.size();
        dl->PushClipRect(a, b, true);
        const int half = nbands / 2 + 1;
        for (int i = -half; i <= half; ++i) {
            // s319 — la secuencia se lee EN ORDEN de un extremo al otro de la barra (i + half). Antes
            // era seq[i % ns] con i de −half a +half: la mitad izquierda salía del FINAL del patrón
            // y la derecha del PRINCIPIO, así que un degradado se veía partido por la mitad (el
            // diálogo Pattern y el slice, que leen en orden, salían bien). En un patrón que se repite
            // sólo cambia la fase.
            // s319b — y de DERECHA a izquierda: leída al derecho la barra salía con los colores al
            // revés que el 3D y el slice (probado con el dedo, el ángulo sí coincidía).
            const int t = seq[(ns - 1) - ((((i + half) % ns) + ns) % ns)];
            const ImU32 col = tool_col_u32(fcolors, t < 0 ? 0 : t);
            const float o0 = bw * (float)i, o1 = bw * (float)(i + 1);
            const ImVec2 p0(c.x + nx * o0 - dx * ext, c.y + ny * o0 - dy * ext);
            const ImVec2 p1(c.x + nx * o0 + dx * ext, c.y + ny * o0 + dy * ext);
            const ImVec2 p2(c.x + nx * o1 + dx * ext, c.y + ny * o1 + dy * ext);
            const ImVec2 p3(c.x + nx * o1 - dx * ext, c.y + ny * o1 - dy * ext);
            dl->AddQuadFilled(p0, p1, p2, p3, col);
        }
        dl->PopClipRect();
        break;
    }
    case SurfacePassKind::PathBlend: {
        const PathBlendPassConfig pbc = pro_pb_read(p);
        const bool is_full = (pbc.mode == PathBlendPassConfig::Mode::Full);
        const ImU32 col_bottom = tool_col_u32(fcolors, std::max(0, pbc.tool_bottom));
        const ImU32 col_top    = tool_col_u32(fcolors, std::max(0, pbc.tool_top));
        const double H = std::max(0.04, layer_h);
        const double floor_frac   = std::clamp((double)pbc.floor_mm   / H, 0.0, 1.0);
        const double mid_end_frac = std::clamp((double)pbc.mid_end_mm / H, 0.0, 1.0);
        // y=a.y (top of bar) = nominal_z (frac=1), y=b.y (bottom of bar) = bottom_z
        // (frac=0) — same convention as Tab.cpp's y_from_frac.
        auto y_from_frac = [&](double f) -> float {
            return a.y + (float)((1.0 - f) * (double)(b.y - a.y));
        };
        // Cap (Full only): flat band from ramp-end up to the top of the layer.
        if (is_full) {
            const float y_mid = y_from_frac(mid_end_frac);
            if (y_mid > a.y) dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(b.x, y_mid), col_top);
        }
        // Ramp: filled area under the eased curve from (t=0, floor) to (t=1, ramp-end),
        // built as a strip of quads so the fill is correct regardless of curve convexity.
        const int steps = 16;
        ImVec2 prev_pt(a.x, y_from_frac(floor_frac));
        for (int i = 1; i <= steps; ++i) {
            const double t_raw = (double)i / (double)steps;
            double t = t_raw;
            switch (pbc.ease_mode) {
                case 1: t = t * t;                       break; // EaseIn
                case 2: t = 1.0 - (1.0 - t) * (1.0 - t); break; // EaseOut
                case 3: t = t * t * (3.0 - 2.0 * t);     break; // EaseInOut
                default: break;                                  // Linear
            }
            const double z_frac = floor_frac + t * (mid_end_frac - floor_frac);
            const ImVec2 pt(a.x + (float)(t_raw * (double)(b.x - a.x)), y_from_frac(z_frac));
            dl->AddQuadFilled(ImVec2(prev_pt.x, b.y), prev_pt, pt, ImVec2(pt.x, b.y), col_bottom);
            prev_pt = pt;
        }
        // Floor / ramp-end indicator lines (dialog mirror, values already shown
        // as numeric fields in the row above — no text here to avoid clutter).
        dl->AddLine(ImVec2(a.x, y_from_frac(floor_frac)), ImVec2(b.x, y_from_frac(floor_frac)),
                    IM_COL32(200, 200, 200, 140));
        if (is_full)
            dl->AddLine(ImVec2(a.x, y_from_frac(mid_end_frac)), ImVec2(b.x, y_from_frac(mid_end_frac)),
                        IM_COL32(200, 200, 200, 140));
        // s172 — angle indicator overlay: same hatch technique as the Solid
        // preview (line family at the resolved fill_angle), so the fill_angle
        // (auto=45° or override) is VISIBLE here exactly like ColorStitch's.
        {
            const float deg = (pbc.fill_angle < 0)
                                ? float(COLORSTITCH_DEFAULT_ANGLE_DEG)   // s280b: sin 45 clavado
                                : (float)pbc.fill_angle;
            const float ang = deg * 3.14159265f / 180.f;
            const float dx = std::cos(ang), dy = -std::sin(ang);
            const float nx = -dy, ny = dx;
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float ext = 0.5f * (float)std::hypot(b.x - a.x, b.y - a.y) + 4.f;
            dl->PushClipRect(a, b, true);
            const int nlines = (int)(ext / 5.f) + 1;
            for (int i = -nlines; i <= nlines; ++i) {
                const float ox = c.x + nx * 5.f * (float)i;
                const float oy = c.y + ny * 5.f * (float)i;
                dl->AddLine(ImVec2(ox - dx * ext, oy - dy * ext),
                            ImVec2(ox + dx * ext, oy + dy * ext),
                            IM_COL32(255, 255, 255, 70), 1.f);
            }
            dl->PopClipRect();
        }
        break;
    }
    default:
        break;
    }
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 110), 3.f);
}

// Editor de una zona (Top / Penu) del pro mode — v2 s108: filas estilo
// SandwichDialog con los tres kinds inline y cajita Z editable (auto-rescale).
// ---------------------------------------------------------------------------
// s232 — IDENTIDAD DE ZONA. Las tres zonas viven seguidas en un solo sitio desde
// s230 (más intuitivo de autorar), pero siguen siendo entidades distintas del
// motor, y el título en texto plano no lo decía: "Top / Penultimate / Bottom"
// se leían como etiquetas sueltas. Un color por zona, usado en DOS sitios —
// la chapa del título del editor y el realce del viewport (contorno + chapas
// de isla) — para que "verde = arriba, naranja = abajo" se aprenda una vez.
// Fuente ÚNICA: si alguien cambia un tono aquí, cambian los dos sitios.
//   0 = Top (verde) · 1 = Penultimate (azul) · 2 = Bottom (naranja)
// ---------------------------------------------------------------------------
static void cs_zone_rgb(int zone, float& r, float& g, float& b)
{
    switch (zone) {
        // s232 (feedback usuario) — el Penúltimo comparte FAMILIA con el Top: no es
        // otra cosa, es la capa de debajo de la misma superficie superior (el propio
        // motor lo cuenta como top-facing, ver `slot_wants_top`). Verde más oscuro:
        // "lo mismo, una capa más adentro", en vez de un tercer color que lo presentaba
        // como una entidad ajena.
        case 1:  r = 0.15f; g = 0.50f; b = 0.31f; break;   // Penultimate — verde oscuro
        case 2:  r = 0.94f; g = 0.58f; b = 0.20f; break;   // Bottom — naranja
        default: r = 0.24f; g = 0.75f; b = 0.44f; break;   // Top — verde
    }
}
static ImU32 cs_zone_u32(int zone, int alpha)
{
    float r, g, b; cs_zone_rgb(zone, r, g, b);
    return IM_COL32(int(r * 255), int(g * 255), int(b * 255), alpha);
}
static ColorRGBA cs_zone_rgba(int zone, float alpha)
{
    float r, g, b; cs_zone_rgb(zone, r, g, b);
    return ColorRGBA(r, g, b, alpha);
}


static std::string cs_strip_warn(std::string s);   // s318 — definida junto a la toolbar

// s318 — EDITOR DE UNA ZONA, rehecho con el lenguaje de panel (GizmoNeotkoStyle.hpp). Lo que se ve:
//   · una TARJETA biselada con filo teal: glifo de la zona (el bloque con su capa encendida),
//     nombre, "n/3 · 0.20 mm", y a la derecha copiar (menú con los dos destinos) y vaciar.
//     Zona vacía = contorno a trazos con un solo botón "Add…".
//   · la barra de mm a la izquierda (misma matemática de arrastre, s169/s232) y cada pase como
//     un pozo en tres líneas fijas: [#N · tipo en 4 glifos · nombre · ▲ ▼ 🗑] · [filamentos ·
//     ángulo] · [tejido]. PathBlend añade floor / ramp end / modo / perfil.
// 🔑 Lo que NO cambia: la lógica de cada control está copiada tal cual de la versión anterior
// (caps del Bottom, colapso a PB, payloads, diferidos, rueda, contextual, "+ layer"). Sólo cambian
// el dibujo, el orden y el sitio. El badge SOLID/COLORSTITCH y el combo "##kind" se fueron: el tipo
// lo dicen los 4 glifos y el nombre en teal, una sola vez.
// 🚨 Dos tarjetas anidadas (zona y pase) ⇒ dos ImDrawListSplitter DISTINTOS: ImGui no admite
// ChannelsSplit dentro de otro ChannelsSplit sobre la misma lista.
static void draw_zone_editor(const char* id, const char* label,
                             Slic3r::SurfacePassStack& st, bool allow_disable,
                             bool penu, const std::vector<std::string>& fcolors,
                             int nfil, double layer_h, float row_avail_w,
                             bool bottom_caps = false,
                             const char* add_label = nullptr,
                             const char* clear_label = nullptr,
                             const char* copy_label1 = nullptr,
                             const char* copy_label2 = nullptr,
                             int* copy_dest = nullptr,
                             std::vector<PainterTrayMsg>* tray = nullptr)
{
    // NEOTKO_BOTTOM_TAG — Fase 1 §5.5 (caps estrictos, sólo Bottom): máx 2 Solid · máx 1
    // ColorStitch · PathBlend SIEMPRE Full (PB Half no se ofrece abajo).
    using namespace Slic3r;
    using K = SurfacePassKind;
    ImGui::PushID(id);

    const float  u        = neo_u();
    const float  fh       = ImGui::GetFrameHeight();
    const float  gb       = 1.4f * u;
    ImDrawList*  dl       = ImGui::GetWindowDrawList();
    const int    zone_idx = penu ? 1 : (bottom_caps ? 2 : 0);
    const Glyph  zg       = (zone_idx == 1) ? Glyph::ZonePenu : (zone_idx == 2) ? Glyph::ZoneBottom : Glyph::ZoneTop;
    const float  card_w   = row_avail_w;
    // "+ Add…" / "x Clear…" llevan el signo delante porque eran botones de texto; con glifo sobra.
    auto strip_sign = [](const std::string& t) {
        return (t.size() > 2 && (t[0] == '+' || t[0] == 'x') && t[1] == ' ') ? t.substr(2) : t;
    };
    auto zone_glyph = [&](Glyph g, ImU32 col) {
        const ImVec2 gp = ImGui::GetCursorScreenPos();
        const float  gs = 1.35f * u;
        draw_glyph(dl, ImVec2(gp.x, gp.y + (fh - gs) * 0.5f), gs, g, col);
        ImGui::Dummy(ImVec2(gs, fh));
    };

    // NEOTKO_SANDWICH_TAG s119 (EMPTY model): no hay "Enabled"; una zona está vacía o no. Top es
    // el color mismo (siempre presente); Penu y Bottom se añaden y se vacían explícitamente.
    if (allow_disable && !st.any_effect()) {
        st.passes.clear();            // canonical Empty = no passes
        st.enabled = false;
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        const float  h  = fh + 0.9f * u;
        ImGui::SetCursorScreenPos(ImVec2(c0.x + 0.7f * u, c0.y + 0.45f * u));
        zone_glyph(zg, neo_col_u32(NeoCol::TextDim));
        ImGui::SameLine(0.f, 0.45f * u);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        const std::string add = strip_sign(add_label ? std::string(add_label) : _u8L("+ Add penultimate"));
        const float bw = ImGui::CalcTextSize(add.c_str()).x + 2.5f * u;
        neo_same_line_at(c0.x + card_w - 0.5f * u - bw);
        if (neo_text_button((add + "##add").c_str(), NeoBtn::Ghost, true, Glyph::Plus)) {
                SurfacePass sp; sp.kind = K::ColorStitch; sp.ratio = 1.0;
                st.passes.push_back(sp);
                pro_cm_write(st.passes.back(), penu, 0, 1, 50);  // default A/B/mix
                st.enabled = true;
        }
        neo_card_dashed(dl, c0, ImVec2(c0.x + card_w, c0.y + h));
        ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + h));
        ImGui::Dummy(ImVec2(card_w, 0.f));
        ImGui::PopID();
        return;                       // Empty → nothing else to render
    }
    st.enabled = true;
    if (!allow_disable && st.passes.empty()) {
        SurfacePass sp; sp.ratio = 1.0;
        st.passes.push_back(sp);
    }

    // Ratio sanity — equal split when degenerate (covers stacks seeded with the
    // default ratio 0.0 by the s107 MVP, which would otherwise predict black).
    {
        double tot = 0.0;
        for (const auto& p : st.passes) tot += std::max(0.0, p.ratio);
        if (tot < 1e-6 && !st.passes.empty())
            for (auto& p : st.passes) p.ratio = 1.0 / (double)st.passes.size();
    }

    const int n = (int)st.passes.size();

    // Deferred mutations (PR.4 lesson: never mutate the vector mid-iteration).
    int remove_idx       = -1;
    int pb_collapse_src  = -1;   // pass index that switched to PB
    int pb_collapse_mode = -1;   // 0 = Half, 1 = Full
    // s231 F4 — duplicar / reordenar un pase, desde el contextual de su barra de
    // preview. Montar a mano un pase igual que el de al lado (mismo kind, mismos
    // tools, mismo patrón ColorStitch, mismo perfil PB) era el trabajo más repetitivo
    // del Pro, y no había forma de reordenar sin borrar y rehacer.
    int dup_idx          = -1;
    int move_idx         = -1;
    int move_dir         = 0;    // +1 = hacia la superficie (arriba), -1 = hacia dentro

    const std::string ease_names[4] = {
        _u8L("Mode: Linear"), _u8L("Mode: Ease In"),
        _u8L("Mode: Ease Out"), _u8L("Mode: Ease In/Out")
    };

    bool clear_req = false;

    ImDrawListSplitter card_split;
    card_split.Split(dl, 2);
    card_split.SetCurrentChannel(dl, 1);     // contenido; el fondo va al canal 0 al final
    const ImVec2 c0 = ImGui::GetCursorScreenPos();

    // ---- cabecera de la tarjeta ----
    ImGui::SetCursorScreenPos(ImVec2(c0.x + 0.7f * u, c0.y + 0.45f * u));
    zone_glyph(zg, neo_col_u32(NeoCol::AccentBright));
    ImGui::SameLine(0.f, 0.45f * u);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.f, 0.5f * u);
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::Text("%d/%d \xc2\xb7 %.2f mm", n, SurfacePassStack::kMaxPasses, layer_h);
    ImGui::PopStyleColor();
    {
        const int nbtn = (copy_dest ? 1 : 0) + (allow_disable ? 1 : 0);
        if (nbtn > 0) {
            neo_same_line_at(c0.x + card_w - 0.5f * u - float(nbtn) * gb - float(nbtn - 1) * 2.f);
            if (copy_dest) {
                // s231 F4 — copia ZONA→ZONA. s318: en vez de una fila "copy to:" siempre visible,
                // un glifo que abre los dos destinos. La copia la hace el caller (do_copy), con la
                // normalización de caps del Bottom intacta.
                if (neo_glyph_button("##copy", gb, Glyph::Copy, false, _u8L("Copy this zone to…").c_str()))
                    ImGui::OpenPopup("##copy_to");
                if (ImGui::BeginPopup("##copy_to")) {
                    ImGui::TextDisabled("%s", _u8L("copy to:").c_str());
                    ImGui::Separator();
                    if (copy_label1 && ImGui::MenuItem(copy_label1)) *copy_dest = 0;
                    if (copy_label2 && ImGui::MenuItem(copy_label2)) *copy_dest = 1;
                    ImGui::EndPopup();
                }
                if (allow_disable) ImGui::SameLine(0.f, 2.f);
            }
            if (allow_disable) {
                const std::string clr = strip_sign(clear_label ? std::string(clear_label) : _u8L("x Clear penultimate"));
                if (neo_glyph_button("##clear", gb, Glyph::Trash, true, clr.c_str()))
                    clear_req = true;     // se aplica al final: la tarjeta de este frame se cierra entera
            }
        }
    }

    // ---- cuerpo: barra de mm + columna de pases ----
    const float  body_y = ImGui::GetCursorScreenPos().y + 0.15f * u;
    const float  bar_w  = 2.6f * u;
    const float  col_dx = 0.7f * u + bar_w + 0.45f * u;
    const float  pass_w = std::max(8.f * u, card_w - col_dx - 0.5f * u);
    const float  pin    = 0.4f * u;
    const ImVec2 bar_p0(c0.x + 0.7f * u, body_y);
    ImGui::SetCursorScreenPos(ImVec2(c0.x + col_dx, body_y));
    ImGui::Indent(col_dx);
    float bar_bottom = body_y;

    // Visual order = physical order: #1 = el pase más cercano a la superficie.
    for (int i = n - 1; i >= 0; --i) {
        SurfacePass& p = st.passes[i];
        ImGui::PushID(i);
        ImDrawListSplitter ps;
        ps.Split(dl, 2);
        ps.SetCurrentChannel(dl, 1);
        const ImVec2 pr0    = ImGui::GetCursorScreenPos();
        const float  left_w = pass_w - 2.f * pin;   // ancho útil dentro del pase (lo usa la barra de tejido)

        const PathBlendPassConfig pbc =
            (p.kind == K::PathBlend) ? pro_pb_read(p) : PathBlendPassConfig{};
        const bool is_pb_half =
            p.kind == K::PathBlend && pbc.mode == PathBlendPassConfig::Mode::Half;

        ImGui::SetCursorScreenPos(ImVec2(pr0.x + pin, pr0.y + pin));
        const float line_y = pr0.y + pin;
        ImGui::BeginGroup();

        // ---- línea 1: #N · tipo · nombre · ▲ ▼ 🗑 ----
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("#%d", n - i);
        ImGui::SameLine(0.f, 0.4f * u);
        int sel = 0;
        if (p.kind == K::ColorStitch)    sel = 1;
        else if (p.kind == K::PathBlend) sel = is_pb_half ? 2 : 3;
        const std::string kind_names[4] = { _u8L("Solid"), _u8L("ColorStitch"), _u8L("PB Half"), _u8L("PB Full") };
        {
            // NEOTKO_BOTTOM_TAG — §5.5: los tipos de los OTROS pases, para que cambiar ESTE a un tipo
            // con tope respete los caps (el tipo nuevo sustituye al suyo, así que i no cuenta).
            int _n_solid_other = 0, _n_cs_other = 0;
            if (bottom_caps)
                for (int j = 0; j < n; ++j) if (j != i) {
                    if (st.passes[j].kind == K::Solid)            ++_n_solid_other;
                    else if (st.passes[j].kind == K::ColorStitch) ++_n_cs_other;
                }
            const Glyph  kg[4] = { Glyph::KSolid, Glyph::KStitch, Glyph::KPbHalf, Glyph::KPbFull };
            const float  kb    = 1.5f * u;
            const int    nk    = bottom_caps ? 3 : 4;   // PB Half nunca abajo (dejaría una capa vacía)
            const ImVec2 wp    = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(ImVec2(wp.x - 2.f, line_y + (fh - kb) * 0.5f - 2.f),
                              ImVec2(wp.x + float(nk) * kb + float(nk - 1) * 2.f + 2.f, line_y + (fh + kb) * 0.5f + 2.f),
                              neo_fade(NeoCol::Surface, 0.7f), 5.f);
            ImGui::SetCursorScreenPos(ImVec2(wp.x, line_y + (fh - kb) * 0.5f));
            bool first = true;
            for (int k = 0; k < 4; ++k) {
                if (bottom_caps && k == 2) continue;
                if (!first) ImGui::SameLine(0.f, 2.f);
                first = false;
                const bool _capped = bottom_caps &&
                    ((k == 0 && _n_solid_other >= 2) ||   // max 2 Solid
                     (k == 1 && _n_cs_other    >= 1));    // max 1 ColorStitch
                const std::string tip = !_capped ? kind_names[k]
                                      : (k == 0 ? _u8L("Bottom: max 2 Solid passes")
                                                : _u8L("Bottom: max 1 ColorStitch pass"));
                char kid[8];
                std::snprintf(kid, sizeof(kid), "##k%d", k);
                // (Sin BeginDisabled en esta ImGui: se atenúa con Alpha — neo_col_u32 lo respeta —
                //  y el clic se veta con `!_capped`, igual que hacía el combo.)
                if (_capped) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
                const bool hit = neo_glyph_toggle(kid, kb, k == sel, kg[k], tip.c_str());
                if (_capped) ImGui::PopStyleVar();
                if (hit && k != sel && !_capped) {
                    if (k == 0) {
                        // s232 — RAÍZ del "la receta dice ColorStitch y sale plana":
                        // pasar un pase a Solid dejaba su payload de efecto DENTRO
                        // (`colorstitch.kv` / `pathblend.kv` con present=true). En el json
                        // queda un `kind:1` (Solid) con un ColorStitch completo colgando:
                        // el motor lo slicea como Solid (correcto, el kind manda) pero el
                        // perfil PARECE ColorStitch por su nombre y su payload, y el
                        // extractor del preview no encuentra pase ColorStitch → sin tejido →
                        // caras planas. Un Solid no tiene efecto: se limpia al cambiar.
                        p.kind = K::Solid;
                        p.colorstitch  = {};
                        p.pathblend = {};
                    } else if (k == 1) {
                        p.kind = K::ColorStitch;
                        if (pro_cm_pattern(p, penu).empty())
                            pro_cm_write(p, penu, 0, 1, 50);
                    } else {
                        // PB collapses the zone to ONE whole-layer PB pass —
                        // same rule as the SandwichDialog (on_kind_change).
                        pb_collapse_src  = i;
                        pb_collapse_mode = (k == 2) ? 0 : 1;
                    }
                }
            }
        }
        ImGui::SameLine(0.f, 0.5f * u);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, line_y));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
        ImGui::TextUnformatted(kind_names[sel].c_str());
        ImGui::PopStyleColor();

        if (p.kind == K::Solid && (p.colorstitch.present || p.pathblend.present)) {
            ImGui::SameLine(0.f, 0.4f * neo_u());
            const bool orphan_pb = p.pathblend.present;
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));   // s318: es un aviso real
            const bool _restore = ImGui::SmallButton(orphan_pb ? "!PB" : "!CS");
            ImGui::PopStyleColor();
            if (_restore) {
                p.kind = orphan_pb ? K::PathBlend : K::ColorStitch;
                if (!orphan_pb) p.colorstitch.present = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("This pass is Solid but still carries a "
                    "leftover effect payload (a pass degraded by an older build). "
                    "Click to restore it.").c_str());
        }

        if (n > 1) {
            // s232 — subir / bajar / quitar cierran la fila por la derecha, con la papelera
            // separada de las flechas (anti-missclick) y los huecos reservados en los extremos para
            // que la papelera caiga en la misma X en todas las filas.
            neo_same_line_at(pr0.x + pin + left_w - (3.f * gb + 0.6f * u));
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, line_y + (fh - gb) * 0.5f));
            {
                // s319 — un pozo detrás de los tres: sueltos sobre el fondo del pase no se veían.
                const ImVec2 w0 = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(ImVec2(w0.x - 2.f, w0.y - 2.f),
                                  ImVec2(w0.x + 3.f * gb + 2.f + 0.6f * u + 2.f, w0.y + gb + 2.f),
                                  neo_fade(NeoCol::Surface, 0.7f), 5.f);
            }
            if (i < n - 1) {
                if (neo_glyph_button("##up", gb, Glyph::Up, false, _u8L("Move this pass up (towards the surface)").c_str())) {
                    move_idx = i; move_dir = +1;
                }
            } else ImGui::Dummy(ImVec2(gb, gb));
            ImGui::SameLine(0.f, 2.f);
            if (i > 0) {
                if (neo_glyph_button("##dn", gb, Glyph::Down, false, _u8L("Move this pass down (into the part)").c_str())) {
                    move_idx = i; move_dir = -1;
                }
            } else ImGui::Dummy(ImVec2(gb, gb));
            ImGui::SameLine(0.f, 0.6f * u);
            if (neo_glyph_button("##del", gb, Glyph::Trash, true, _u8L("Remove this pass").c_str()))
                remove_idx = i;
        }

        // ---- línea 2: filamentos (+ patrón) · ángulo a la derecha ----
        if (p.kind == K::Solid) {
            ImU32 cols[4];
            const int nn = std::max(1, std::min(nfil, 4));
            for (int t = 0; t < nn; ++t) cols[t] = tool_col_u32(fcolors, t);
            const int c = neo_tool_chips("##sol", cols, nn, p.solid_tool);
            if (c >= 0) p.solid_tool = c;
        } else if (p.kind == K::ColorStitch) {
            // Backfill defensivo: un pase sin payload no debe degradar a Solid.
            if (pro_cm_pattern(p, penu).empty()) {
                int a, b, cp; pro_cm_read(p, penu, a, b, cp);
                pro_cm_write(p, penu, a, b, cp);
            }
            int ca, cb, cp; pro_cm_read(p, penu, ca, cb, cp);
            cs_mini_tool(fcolors, ca);
            ImGui::SameLine(0.f, 0.18f * u);
            cs_mini_tool(fcolors, cb);
            ImGui::SameLine(0.f, 0.5f * u);
            if (neo_text_button((_u8L("Pattern…") + "##cs_pat").c_str(), NeoBtn::Ghost, true, Glyph::Adv)) {
                auto* cfg = const_cast<DynamicPrintConfig*>(
                    &wxGetApp().preset_bundle->prints.get_edited_preset().config);
                std::map<std::string, std::string> out_kv;
                // El diálogo devuelve el PAYLOAD COMPLETO del pase (string + knobs
                // del gradiente). Sembrarlo con el kv actual permite re-editar. Así
                // cada pase lleva su PROPIO diseño → el motor (Fill.cpp FASE2) los
                // sliccea distintos; antes solo se capturaba el string y todos los
                // pases caían al gradiente compartido de la región (bug "salen todos
                // iguales", tanto en preview como en slice).
                if (open_colorstitch_pattern_dialog(
                        wxGetApp().plater(), fcolors, penu, cfg, p.colorstitch.kv, out_kv)) {
                    p.colorstitch.present = true;
                    p.colorstitch.kv      = std::move(out_kv);
                    // Sync chip/preview/wheel desde el payload nuevo.
                    int a, b, cp; pro_cm_read(p, penu, a, b, cp);
                    p.solid_tool = a;
                    const auto it = p.colorstitch.kv.find(
                        penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle");
                    if (it != p.colorstitch.kv.end()) {
                        try { p.angle = std::stoi(it->second); } catch (...) {}
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Open the ColorStitch pattern editor").c_str());
        } else if (p.kind == K::PathBlend) {
            PathBlendPassConfig pbe = pbc;
            bool pb_changed = false;
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("ramp").c_str());
            ImGui::SameLine(0.f, 0.3f * u);
            int tb = std::max(0, pbe.tool_bottom);
            if (pro_tool_chip("##pbb", fcolors, nfil, tb, _u8L("Ramp tool (bottom)").c_str())) {
                pbe.tool_bottom = tb; pb_changed = true;
            }
            if (!is_pb_half) {
                ImGui::SameLine(0.f, 0.5f * u);
                ImGui::TextDisabled("%s", _u8L("cap").c_str());
                ImGui::SameLine(0.f, 0.3f * u);
                int tt = std::max(0, pbe.tool_top);
                if (pro_tool_chip("##pbt", fcolors, nfil, tt, _u8L("Cap tool (top)").c_str())) {
                    pbe.tool_top = tt; pb_changed = true;
                }
            }
            if (pb_changed) pro_pb_write(p, pbe, layer_h);
        }
        {
            // El ángulo, en el mismo sitio para los tres tipos (s318: el de PathBlend subía desde la
            // línea del modo). −1 = auto, que se pinta "auto" en el violeta del contorno que pulsa en
            // el visor — el mismo aviso que antes daba el "auto angle" ámbar, sin gastar una línea.
            const char* akey = penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle";
            int av = -1;
            if (p.kind == K::Solid) {
                av = p.angle;
            } else if (p.kind == K::ColorStitch) {
                // NEOTKO_COLORSTITCH_TAG — clave ausente: el TOP cae al default (45), no a auto; el
                // PENU se queda en -1 a propósito (ahí -1 = heredar el ángulo del top).
                av = penu ? COLORSTITCH_ANGLE_AUTO : COLORSTITCH_DEFAULT_ANGLE_DEG;
                const auto it = p.colorstitch.kv.find(akey);
                if (it != p.colorstitch.kv.end()) { try { av = std::stoi(it->second); } catch (...) {} }
            } else {
                av = pbc.fill_angle;
            }
            const float aw = 4.0f * u;
            neo_same_line_at(pr0.x + pin + left_w - aw);
            {
                const ImVec2 gp = ImGui::GetCursorScreenPos();
                draw_glyph(dl, ImVec2(gp.x, gp.y + (fh - 0.9f * u) * 0.5f), 0.9f * u, Glyph::Angle,
                           neo_col_u32(NeoCol::TextDim));
                ImGui::Dummy(ImVec2(0.9f * u, fh));
            }
            ImGui::SameLine(0.f, 0.25f * u);
            ImGui::PushItemWidth(aw - 1.15f * u);
            const bool is_auto = av < 0;
            if (is_auto) ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Slope));
            const bool ach = ImGui::DragInt("##ang", &av, 1.f, -1, 359, is_auto ? "auto" : "%d\xc2\xb0");
            if (is_auto) ImGui::PopStyleColor();
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                if (p.kind == K::Solid)
                    ImGui::SetTooltip("%s", _u8L("-1 = auto (follow fill angle). Wheel over the bar rotates.").c_str());
                else if (p.kind == K::ColorStitch)
                    ImGui::SetTooltip("%s", _u8L("Fill line direction, 0-359. Default 45.\n"
                                                 "-1 = auto: follows Orca's infill rotation, which flips 90 degrees "
                                                 "every other layer, so the stripes change direction inside one band "
                                                 "and cannot be previewed - the zone is outlined in pulsing violet to show it.\n"
                                                 "Wheel over the bar rotates.").c_str());
                else
                    ImGui::SetTooltip("%s", _u8L("Fill line direction, 0-359. Default 45.\n"
                                                 "-1 = auto: flips 90 degrees every other layer. In "
                                                 "PathBlend that is not just surface finish - each "
                                                 "line takes its ramp height from its Y centroid, so "
                                                 "the angle shapes the gradient itself and auto makes "
                                                 "it differ layer to layer. The zone is outlined in pulsing violet to show it.").c_str());
            }
            if (ach) {
                if (p.kind == K::Solid) {
                    if (av < 0) av = -1; else if (av > 359) av = 359;
                    p.angle = av;
                } else if (p.kind == K::ColorStitch) {
                    // Escribe el kv del pase: persiste en el stack del perfil y el slice lo honra
                    // (Fill.cpp painted_colorstitch_angle_for_slot).
                    if (av < -1) av = -1; else if (av > 359) av = 359;
                    p.colorstitch.present = true;
                    p.colorstitch.kv[akey] = std::to_string(av);
                    p.angle = av;
                } else {
                    if (av < 0) av = -1; else av %= 360;
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    pbe.fill_angle = av;
                    pro_pb_write(p, pbe, layer_h);
                }
            }
        }

        // ---- línea 3: el tejido (rueda = ajustar, botón derecho = duplicar / reordenar / borrar) ----
        // ---- preview bar (hover wheel: Solid angle / PB Full mid) — line 2 for
        // Solid/ColorStitch, line 3 for PathBlend (tiene su línea propia arriba) ----
        {
            const float bar_h = ImGui::GetTextLineHeight() * 1.1f;
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##prev", ImVec2(left_w, bar_h));
            // s231 F4 — contextual del pase (botón derecho sobre su barra). El
            // InvisibleButton ya es un item ImGui válido, así que BeginPopupContextItem
            // engancha aquí sin tocar el layout de la fila.
            if (ImGui::BeginPopupContextItem("##pass_ctx")) {
                // NEOTKO_BOTTOM_TAG — §5.5: duplicar tampoco puede saltarse los caps del
                // Bottom (máx 2 Solid / 1 ColorStitch), igual que el selector de kind.
                bool cap_ok = true;
                if (bottom_caps) {
                    int ns = 0, nc = 0;
                    for (const auto& pp : st.passes) {
                        if (pp.kind == K::Solid)         ++ns;
                        else if (pp.kind == K::ColorStitch) ++nc;
                    }
                    if (p.kind == K::Solid)         cap_ok = (ns < 2);
                    else if (p.kind == K::ColorStitch) cap_ok = (nc < 1);
                }
                const bool can_dup = (int)st.passes.size() < SurfacePassStack::kMaxPasses
                                     && p.kind != K::PathBlend   // PB ocupa la zona entera
                                     && cap_ok;
                if (can_dup && ImGui::MenuItem(_u8L("Duplicate pass").c_str()))
                    dup_idx = i;
                if (!can_dup && p.kind == K::PathBlend)
                    ImGui::TextDisabled("%s", _u8L("PathBlend fills the whole zone").c_str());
                if (n > 1) {
                    ImGui::Separator();
                    if (i < n - 1 && ImGui::MenuItem(_u8L("Move up (towards the surface)").c_str())) {
                        move_idx = i; move_dir = +1;
                    }
                    if (i > 0 && ImGui::MenuItem(_u8L("Move down (into the part)").c_str())) {
                        move_idx = i; move_dir = -1;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(_u8L("Delete pass").c_str()))
                        remove_idx = i;
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                // s231 F4 — el contextual del pase no se descubre solo: decirlo aquí,
                // que es donde el usuario ya tiene el ratón cuando gira la rueda.
                // (la rueda hace cosas distintas por kind — ángulo en Solid/ColorStitch,
                //  ramp end en PB Full — así que el texto es deliberadamente genérico)
                ImGui::SetTooltip("%s", _u8L("Wheel: adjust this pass · Right-click: "
                                             "duplicate, reorder or delete it").c_str());
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.f) {
                    if (p.kind == K::Solid) {
                        // port of the dialog's on_angle_wheel (-1 = auto)
                        const int step = (wheel > 0.f) ? 5 : -5;
                        int a2 = p.angle;
                        if (a2 < 0) a2 = (step > 0) ? 0 : -1;
                        else { a2 += step; if (a2 < 0) a2 = -1; else a2 %= 360; }
                        p.angle = a2;
                    } else if (p.kind == K::ColorStitch) {
                        // NEOTKO_COLORSTITCH_TAG — wheel rotates the ColorStitch fill angle;
                        // write the pass kv so it persists to the slice (same as the DragInt).
                        const char* akey = penu ? "interlayer_colormix_penu_angle"
                                                : "interlayer_colormix_angle";
                        // NEOTKO_COLORSTITCH_TAG — mismo criterio que el DragInt: ausente =
                        // default 45 en el top, -1 (heredar) en el penu.
                        int a2 = penu ? COLORSTITCH_ANGLE_AUTO : COLORSTITCH_DEFAULT_ANGLE_DEG;
                        { const auto it = p.colorstitch.kv.find(akey);
                          if (it != p.colorstitch.kv.end()) { try { a2 = std::stoi(it->second); } catch (...) {} } }
                        const int step = (wheel > 0.f) ? 5 : -5;
                        if (a2 < 0) a2 = (step > 0) ? 0 : -1;
                        else { a2 += step; if (a2 < 0) a2 = -1; else a2 %= 360; }
                        p.colorstitch.present = true;
                        p.colorstitch.kv[akey] = std::to_string(a2);
                        p.angle = a2;
                    } else if (p.kind == K::PathBlend && !is_pb_half) {
                        PathBlendPassConfig pbe = pro_pb_read(p);
                        pbe.mid_end_mm += (wheel > 0.f) ? 0.01f : -0.01f;
                        pro_pb_write(p, pbe, layer_h);
                    }
                }
            }
            pro_pass_preview(dl, q, ImVec2(q.x + left_w, q.y + bar_h), p, penu, fcolors, layer_h);
        }

        // ---- línea 4 (PathBlend): floor / ramp end, y modo + perfil ----
        if (p.kind == K::PathBlend) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("floor:").c_str());
            ImGui::SameLine();
            {
                float fv = pbc.floor_mm;
                ImGui::PushItemWidth(3.6f * neo_u());
                if (ImGui::DragFloat("##floor", &fv, 0.005f, 0.01f, (float)layer_h, "%.2f")) {
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    pbe.floor_mm = fv;
                    // keep the ramp alive when floor crosses mid (dialog mirror)
                    if (pbe.mid_end_mm <= pbe.floor_mm) pbe.mid_end_mm = pbe.floor_mm + 0.001f;
                    pro_pb_write(p, pbe, layer_h);
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Ramp floor (mm)").c_str());
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", _u8L("ramp end:").c_str());
            ImGui::SameLine();
            if (is_pb_half) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%.2f", layer_h);   // Half: mid forced to layer top
            } else {
                float mv = pbc.mid_end_mm;
                ImGui::PushItemWidth(3.6f * neo_u());
                if (ImGui::DragFloat("##mid", &mv, 0.005f, 0.f, (float)layer_h, "%.3f")) {
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    pbe.mid_end_mm = mv;
                    pro_pb_write(p, pbe, layer_h);
                }
                ImGui::PopItemWidth();
            }
            // NEOTKO_PATHBLEND_TAG — s316 F2b: ESCALA de la rampa, en mm.
            // Tres estados en un float, los mismos que el degradado de ColorStitch:
            // -1 legacy · 0 ajustar a la superficie · >0 periodo físico. El default
            // es -1, así que una receta que ya existía no cambia de sitio ni de gcode.
            // En su PROPIA línea (s316b): en la misma que floor/ramp end no cabía y se
            // salía por la derecha del panel.
            // 🔒 s316d — OCULTO por decisión del usuario: PathBlend usa siempre span 0.
            // El control se conserva para el futuro editor de degradados con helper
            // gráfico de distancia; poner esto a true lo devuelve tal cual.
            constexpr bool kPathBlendSpanUI = false;
            if (kPathBlendSpanUI) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("span:").c_str());
            ImGui::SameLine();
            {
                float sv = pbc.span_mm;
                ImGui::PushItemWidth(3.6f * neo_u());
                // El step arranca en -1 y el mínimo ES -1: no hay nada por debajo,
                // y dejar bajar más daría tres valores distintos con el mismo efecto.
                if (ImGui::DragFloat("##pbspan", &sv, 0.25f, -1.f, 500.f, "%.2f")) {
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    pbe.span_mm = (sv < -0.5f) ? -1.f : std::max(0.f, sv);
                    pro_pb_write(p, pbe, layer_h);
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L(
                        "Ramp span (mm).\n"
                        "-1 = legacy: the ramp always stretches to fit the surface, so the "
                        "same recipe measures something different on every object.\n"
                        "0 = fit the surface, sampled on the real line grid: a hole no longer "
                        "splits one line into two heights.\n"
                        "> 0 = physical period in mm: the ramp measures the same everywhere, "
                        "whatever the surface size.").c_str());
            }
            } // kPathBlendSpanUI
            const int em = std::clamp(pbc.ease_mode, 0, 3);
            if (neo_text_button((ease_names[em] + "##ease").c_str(), NeoBtn::Ghost, true, Glyph::Ease)) {
                PathBlendPassConfig pbe = pro_pb_read(p);
                pbe.ease_mode = (em + 1) % 4;
                pro_pb_write(p, pbe, layer_h);
            }
            {
                // NEOTKO_PATHBLEND_TAG — s190. El editor del perfil de rampa (start/end zone). Si la
                // rampa está perfilada, un punto teal en la esquina del botón (antes un "*" ámbar).
                const std::string prof = _u8L("Profile…");
                const float pw = ImGui::CalcTextSize(prof.c_str()).x + 2.5f * u;
                neo_same_line_at(pr0.x + pin + left_w - pw);
                if (neo_text_button((prof + "##pb_prof").c_str(), NeoBtn::Ghost, true, Glyph::Adv))
                    ImGui::OpenPopup("##pb_profile_pop");
                if (pro_pb_is_profiled(pbc))
                    dl->AddCircleFilled(ImVec2(ImGui::GetItemRectMax().x - 3.f, ImGui::GetItemRectMin().y + 3.f),
                                        3.f, neo_col_u32(NeoCol::AccentBright), 10);
                if (ImGui::BeginPopup("##pb_profile_pop")) {
                    ImGui::TextUnformatted(_u8L("Ramp profile — floor / ramp end / start / end").c_str());
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    if (pro_pb_profile_editor(pbe, layer_h, is_pb_half))
                        pro_pb_write(p, pbe, layer_h);
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::EndGroup();

        // ---- el pozo del pase (detrás) ----
        const ImVec2 gmax = ImGui::GetItemRectMax();
        const ImVec2 pr1(pr0.x + pass_w, gmax.y + pin);
        // thin-pass warning (Solid/ColorStitch sub-bands; PB has its own gate)
        const double pass_mm = p.ratio * layer_h;
        const bool   thin    = (p.kind != K::PathBlend) && (pass_mm < 0.04 - 1e-9);
        ps.SetCurrentChannel(dl, 0);
        dl->AddRectFilled(pr0, pr1, neo_col_u32(NeoCol::Canvas), 5.f);
        if (thin)
            dl->AddRectFilled(pr0, ImVec2(pr0.x + 2.f, pr1.y), neo_col_u32(NeoCol::Warn));
        ps.Merge(dl);
        if (thin && tray)
            tray->push_back(PainterTrayMsg{ cs_strip_warn(_u8L("⚠ < 0.04 mm — pass is dropped at slice time")), {}, true });
        bar_bottom = pr1.y;
        ImGui::SetCursorScreenPos(ImVec2(pr0.x, pr1.y + 0.4f * u));
        ImGui::PopID();
    }

    // ---- "+ pase" (antes "+ layer"), oculto con PathBlend (ocupa la capa entera) o con 3 pases ----
    bool has_pb_now = false;
    for (const auto& pp : st.passes) has_pb_now |= (pp.kind == K::PathBlend);
    bool add_clicked = false;
    if (!has_pb_now && (int)st.passes.size() < SurfacePassStack::kMaxPasses)
        add_clicked = neo_dashed_button((_u8L("Add pass") + "##add_pass").c_str(), pass_w);
    const float col_bottom = ImGui::GetCursorScreenPos().y;
    ImGui::Unindent(col_dx);

    // s169 — pintar la ratio-bar: bandas (draw_zone, ya escala por ratio) +
    // etiqueta mm por banda (SIEMPRE, incluso con 1 solo pase / PathBlend —
    // desde que el Z-box de la derecha se retiró, la barra es la ÚNICA fuente
    // de "cuánto mide este pase") + divisor blanco 2px + divisores
    // arrastrables (solo si hay 2+ pases — nada que arrastrar con 1 solo).
    {
        const float bar_x  = bar_p0.x;
        const float bar_y0 = bar_p0.y;
        const float bar_y1 = bar_bottom;   // s318 — el borde de abajo del ÚLTIMO pase, medido en el bucle
        const float bar_h  = std::max(1.f, bar_y1 - bar_y0);
        ImDrawList* bdl = ImGui::GetWindowDrawList();

        draw_zone(bdl, ImVec2(bar_x, bar_y0), ImVec2(bar_x + bar_w, bar_y1), st, penu, fcolors);

        double bsum = 0.0;
        for (const auto& pp : st.passes) bsum += std::max(0.0, pp.ratio);
        if (bsum < 1e-6) bsum = 1.0;

        double acc = 0.0;
        for (int dp = 0; dp < n; ++dp) {                 // dp 0 = top of stack
            const int idx = n - 1 - dp;
            const double fr = std::max(0.0, st.passes[idx].ratio) / bsum;
            const float y0f = bar_y0 + (float)(acc * bar_h);
            acc += fr;
            const float y1f = (dp == n - 1) ? bar_y1 : (bar_y0 + (float)(acc * bar_h));

            // s232 — la etiqueta de mm iba pegada al borde IZQUIERDO, justo donde
            // `draw_zone` escribe su propia marca de tool ("T1") dentro de la misma
            // banda: los dos textos se solapaban y salía un churro ("0.110"). Ahora va
            // alineada a la DERECHA de la barra, con una cortinilla oscura detrás para
            // que se lea igual sobre una banda clara.
            char mmbuf[16];
            std::snprintf(mmbuf, sizeof(mmbuf), "%.2f", fr * layer_h);
            const ImVec2 mmsz = ImGui::CalcTextSize(mmbuf);
            const ImVec2 mmp(bar_x + bar_w - mmsz.x - 3.f, y0f + 2.f);
            bdl->AddRectFilled(ImVec2(mmp.x - 2.f, mmp.y - 1.f),
                               ImVec2(mmp.x + mmsz.x + 2.f, mmp.y + mmsz.y + 1.f),
                               IM_COL32(0, 0, 0, 130), 2.f);
            bdl->AddText(mmp, IM_COL32(255, 255, 255, 255), mmbuf);

            if (dp != n - 1) {   // divider — only reached when n >= 2
                // s318 — el divisor: un corte del color del pozo con una muesca teal (el asa), en vez
                // de la línea blanca de 2 px. La zona agarrable y la matemática no cambian.
                bdl->AddRectFilled(ImVec2(bar_x, y1f - 1.5f), ImVec2(bar_x + bar_w, y1f + 1.5f),
                                   neo_col_u32(NeoCol::Canvas));
                bdl->AddRectFilled(ImVec2(bar_x + bar_w * 0.3f, y1f - 1.f), ImVec2(bar_x + bar_w * 0.7f, y1f + 1.f),
                                   neo_col_u32(NeoCol::AccentBright), 1.f);

                // s232 — la zona agarrable eran 8 px (±4) y con 3 pases el divisor de
                // enmedio era casi imposible de pillar (feedback usuario). Se ensancha a
                // ±10 px, pero acotada a un TERCIO de la banda más corta que toca: si dos
                // divisores están juntos, solaparse haría que el de arriba se tragara los
                // clics del de abajo (ImGui da el item al primero que se dibuja) y el
                // segundo dejaría de responder — peor que fino.
                const float band_up = y1f - y0f;
                const float band_dn = ((dp + 1 == n - 1) ? bar_y1
                                       : (bar_y0 + (float)((acc + std::max(0.0, st.passes[n - 2 - dp].ratio) / bsum) * bar_h))) - y1f;
                // Cada divisor se come grab_h/2 hacia cada lado, así que dos divisores
                // que compartan banda ocupan grab_h en total: acotar a la banda más
                // corta garantiza que no se pisen.
                const float grab_h = std::max(8.f, std::min(20.f, std::min(band_up, band_dn) * 0.9f));
                ImGui::PushID(3000 + dp);
                ImGui::SetCursorScreenPos(ImVec2(bar_x, y1f - grab_h * 0.5f));
                ImGui::InvisibleButton("##div", ImVec2(bar_w, grab_h));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                // Realce del divisor bajo el cursor: sin esto, una zona agarrable más
                // ancha que la línea que se ve es adivinar dónde empieza.
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    bdl->AddLine(ImVec2(bar_x, y1f), ImVec2(bar_x + bar_w, y1f),
                                 neo_col_u32(NeoCol::AccentBright), 4.f);
                if (ImGui::IsItemActive()) {
                    // s169 F4 — matemática EXACTA de Tab.cpp ratio_bar_motion: solo
                    // tocamos los DOS pases adyacentes a este divisor, Σ intacta.
                    // Sin estado persistente: IsItemActive() ya retiene el drag
                    // mientras el botón siga pulsado aunque el cursor salga del
                    // InvisibleButton (equivalente a CaptureMouse() en wx).
                    const int k    = dp;
                    const int idxA = n - 1 - k;          // pass above the divider
                    const int idxB = n - 2 - k;          // pass below
                    double topAcc = 0.0;
                    for (int kk = 0; kk < k; ++kk)
                        topAcc += std::max(0.0, st.passes[n - 1 - kk].ratio) / bsum;
                    const double comb = (std::max(0.0, st.passes[idxA].ratio)
                                       + std::max(0.0, st.passes[idxB].ratio)) / bsum;
                    const double minF = std::min(0.45, 0.04 / layer_h);
                    double aFrac = (double)(ImGui::GetMousePos().y - bar_y0) / bar_h - topAcc;
                    aFrac = std::clamp(aFrac, minF, std::max(minF, comb - minF));
                    st.passes[idxA].ratio = aFrac * bsum;
                    st.passes[idxB].ratio = (comb - aFrac) * bsum;
                }
                ImGui::PopID();
            }
        }

    }

    // s231 F4 — duplicar / mover (deferido, fuera de la iteración: insertar en el
    // vector invalidaría las referencias `SurfacePass& p` del bucle).
    if (dup_idx >= 0 && dup_idx < (int)st.passes.size()
        && (int)st.passes.size() < SurfacePassStack::kMaxPasses) {
        SurfacePass copy = st.passes[dup_idx];   // kind + tools + kv + blob + ángulo
        // El clon se reparte el grosor del original a medias, así que la zona conserva
        // su altura total y ningún otro pase se mueve — duplicar no debe reescalar el
        // sándwich entero por debajo del usuario.
        const double half = std::max(0.0, st.passes[dup_idx].ratio) * 0.5;
        st.passes[dup_idx].ratio = half;
        copy.ratio               = half;
        st.passes.insert(st.passes.begin() + dup_idx + 1, copy);   // justo encima
    } else if (move_idx >= 0 && move_dir != 0) {
        const int j = move_idx + move_dir;
        if (j >= 0 && j < (int)st.passes.size())
            std::swap(st.passes[move_idx], st.passes[j]);   // el pase viaja con su grosor
    }

    // deferred delete / PB collapse (outside the iteration)
    if (remove_idx >= 0 && remove_idx < (int)st.passes.size()) {
        st.passes.erase(st.passes.begin() + remove_idx);
        double tot = 0.0;
        for (const auto& p : st.passes) tot += std::max(0.0, p.ratio);
        if (tot > 1e-6)
            for (auto& p : st.passes) p.ratio = std::max(0.0, p.ratio) / tot;
        else if (!st.passes.empty())
            for (auto& p : st.passes) p.ratio = 1.0 / (double)st.passes.size();
    } else if (pb_collapse_mode >= 0) {
        // keep the previous blob fields when the source pass already was PB
        PathBlendPassConfig pbc;
        if (pb_collapse_src >= 0 && pb_collapse_src < (int)st.passes.size()
            && st.passes[pb_collapse_src].kind == K::PathBlend)
            pbc = pro_pb_read(st.passes[pb_collapse_src]);
        pbc.mode = (pb_collapse_mode == 0) ? PathBlendPassConfig::Mode::Half
                                           : PathBlendPassConfig::Mode::Full;
        SurfacePass pb;
        pb.kind  = K::PathBlend;
        pb.ratio = 1.0;
        pro_pb_write(pb, pbc, layer_h);
        st.passes.assign(1, pb);
    }

    // + layer — la lógica de siempre (reparto de ratios y caps del Bottom), sobre el stack YA mutado
    bool has_pb = false;
    for (const auto& p : st.passes) has_pb |= (p.kind == K::PathBlend);
    if (add_clicked && !has_pb && (int)st.passes.size() < SurfacePassStack::kMaxPasses) {
            const double newR = 1.0 / (double)(st.passes.size() + 1);
            double tot = 0.0;
            for (const auto& p : st.passes) tot += std::max(0.0, p.ratio);
            if (tot > 1e-6)
                for (auto& p : st.passes)
                    p.ratio = std::max(0.0, p.ratio) / tot * (1.0 - newR);
            SurfacePass np;
            np.ratio = newR;
            // NEOTKO_BOTTOM_TAG — §5.5: a new bottom pass defaults to a kind that fits
            // the caps. Solid by default; once 2 Solids exist, default to ColorStitch
            // (2 Solid + 1 ColorStitch = kMaxPasses, so a slot always fits here).
            if (bottom_caps) {
                int ns = 0, nc = 0;
                for (const auto& p : st.passes) {
                    if (p.kind == K::Solid)         ++ns;
                    else if (p.kind == K::ColorStitch) ++nc;
                }
                if (ns >= 2 && nc < 1) {
                    np.kind = K::ColorStitch;
                    pro_cm_write(np, penu, 0, 1, 50);   // default A/B/mix
                }
            }
            st.passes.push_back(np);    // becomes the new TOPMOST pass (#1)
    }
    if (clear_req) {
        st.passes.clear();
        st.enabled = false;
    }

    // ---- el fondo de la tarjeta (canal 0, detrás de todo) ----
    const float bottom = std::max(col_bottom, bar_bottom + 0.4f * u) + 0.1f * u;
    card_split.SetCurrentChannel(dl, 0);
    neo_card_bg(dl, c0, ImVec2(c0.x + card_w, bottom), 0.9f * u, true, neo_fade(NeoCol::Surface, 0.38f));
    card_split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(c0.x, bottom));
    ImGui::Dummy(ImVec2(card_w, 0.f));
    ImGui::PopID();
}
// NEOTKO_COLORSTITCH_TAG_END

int GLGizmoColorStitchPainter::slot_for_selected_profile(bool assign_if_missing)
{
    if (m_selected_profile_id == 0) return 0;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return 0;

    // First look for the profile id in any existing slot of any model_part volume.
    int existing_slot = 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            if (mv->colorstitch_slot_to_profile_id[s] == m_selected_profile_id) {
                existing_slot = s; break;
            }
        }
        if (existing_slot) break;
    }
    if (existing_slot) {
        // ⚠️ NEOTKO_COLORSTITCH_TAG — s232, RAÍZ del "en un objeto ensamblado se pinta,
        // el slice sale bien, pero el painter lo enseña GRIS".
        //
        // Esta rama devolvía el número de slot en cuanto lo encontraba en UN volumen y
        // se iba SIN escribirlo en los demás. Con un solo volumen es equivalente; en un
        // objeto multivolumen (lo que deja un Assemble) el pincel pinta las caras del
        // volumen que toques con ese número, pero su tabla slot→perfil se queda a CERO:
        //   VOL_SLOTS vol=0 s2(pid=40,faces=4)    ← el que ya lo tenía
        //   VOL_SLOTS vol=1 s2(pid=0 ,faces=4)    ← pintado, sin perfil
        // Y todo lector del preview empieza por `if (pid == 0) continue;`, así que ese
        // volumen no tiene color ni tejido → gris. El slice, que resuelve el perfil por
        // objeto, sí lo encuentra: de ahí "el gcode es perfecto y la vista no".
        // El mismo agujero dejaba a la pipeta sin receta (leía pid 0 → sin bottom) y al
        // realce sin nada que iluminar.
        //
        // Se rellenan los volúmenes que tengan ESE slot libre. Si alguno lo tiene
        // ocupado por OTRO perfil no se pisa — sobrescribirlo le cambiaría el color a
        // caras ya pintadas de otra receta; se deja constancia en el log porque esa
        // colisión sí necesita un remap de estados, no un parche aquí.
        // Sólo cuando esto es una petición REAL de pintura (`assign_if_missing`): con
        // false esta función es una consulta, y `render_header` la llama CADA FRAME para
        // resincronizar el slot activo — rellenar ahí mutaría el modelo desde el render,
        // sin snapshot de undo y en bucle.
        for (ModelVolume* mv : mo->volumes) {
            if (!assign_if_missing) break;
            if (!mv->is_model_part()) continue;
            int& cell = mv->colorstitch_slot_to_profile_id[existing_slot];
            if (cell == m_selected_profile_id) continue;
            if (cell == 0) {
                cell = m_selected_profile_id;
                NEOTKO_LOG(PROFILE, "PAINT slot_backfill vol='" << mv->name
                    << "' slot=" << existing_slot << " ← pid=" << m_selected_profile_id);
            } else {
                NEOTKO_LOG(PROFILE, "PAINT slot_COLLISION vol='" << mv->name
                    << "' slot=" << existing_slot << " has pid=" << cell
                    << " want pid=" << m_selected_profile_id);
            }
        }
        NEOTKO_LOG(PROFILE, "PAINT slot_lookup profile_id=" << m_selected_profile_id
            << " → existing slot=" << existing_slot);
        return existing_slot;
    }
    if (!assign_if_missing) return 0;

    // Find the lowest slot index that is free across ALL model_part volumes.
    for (int s = 1; s < MAX_SLOTS; ++s) {
        bool free_everywhere = true;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            if (mv->colorstitch_slot_to_profile_id[s] != 0) { free_everywhere = false; break; }
        }
        if (free_everywhere) {
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                mv->colorstitch_slot_to_profile_id[s] = m_selected_profile_id;
            }
            NEOTKO_LOG(PROFILE, "PAINT slot_assign profile_id=" << m_selected_profile_id
                << " → new slot=" << s);
            return s;
        }
    }
    NEOTKO_LOG(PROFILE, "PAINT slot_assign FAILED — all paint slots used");
    return 0; // all slots taken — caller shows the "full" warning.
}

// s233 — el cuerpo vive ahora en ColorStitchPaintPreview::slot_colors(mv, owner) (misma
// función, con el objeto dueño explícito). Este método se queda como wrapper fino que
// rellena el default "el objeto ACTIVO del gizmo", que es lo que asumen sus call-sites.
std::vector<ColorRGBA>
GLGizmoColorStitchPainter::build_ebt_colors_for_volume(const ModelVolume* mv,
                                                    const ModelObject* owner) const
{
    static_assert(MAX_SLOTS == ModelVolume::COLORSTITCH_SLOT_COUNT,
                  "gizmo MAX_SLOTS must match ModelVolume::COLORMIX_SLOT_COUNT (3mf slot table)");
    const ModelObject* mo = owner
        ? owner
        : (m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr);
    return ColorStitchPaintPreview::slot_colors(mv, mo);
}

// NEOTKO_COLORSTITCH_TAG — weave preview helpers.
// s233 F3 — todos estos cuerpos (tool_col_rgba, los extractores colorstitch_*/
// pathblend_*, los dos *_make_weave y el ancho de línea / altura de capa resueltos)
// se mudaron a slic3r/GUI/ColorStitchPaintPreview.{hpp,cpp}: la vista 3D normal teje
// exactamente igual sin gizmo. Se importan por nombre para no tocar sus call-sites.
using ColorStitchPaintPreview::tool_col_rgba;
using ColorStitchPaintPreview::colorstitch_kv_from_stack;
using ColorStitchPaintPreview::pathblend_from_stack;
using ColorStitchPaintPreview::colorstitch_top_kv;
using ColorStitchPaintPreview::colorstitch_weave_theta;
using ColorStitchPaintPreview::colorstitch_make_weave;
using ColorStitchPaintPreview::pathblend_top_config;
using ColorStitchPaintPreview::pathblend_make_weave;
// NEOTKO_COLORSTITCH_TAG — s314: devuelve el SPACING real entre líneas, no el ancho. El
// nombre se conserva porque es lo que consumen los call-sites, pero lo que hace falta para
// contar líneas es la separación (ancho − altura·(1−π/4)); ver weave_top_line_spacing().
static double GLGizmoColorStitchPainter_top_line_width() { return ColorStitchPaintPreview::weave_top_line_spacing(); }
static double GLGizmoColorStitchPainter_layer_height()   { return ColorStitchPaintPreview::weave_layer_height(); }
#ifdef NEOTKO_PRIVATE_TOWER
static void recompute_depth_recipes(ColorStitchSticker& st, const Slic3r::ColorSci::Material mats[4], int n_fil,
                                    const float bg[3], double layer_mm);
#endif
std::vector<TriangleSelectorPatch::WeaveParams>
GLGizmoColorStitchPainter::build_ebt_weave_for_volume(const ModelVolume* mv,
                                                   const TriangleSelectorPatch* sel,
                                                   const ModelObject* owner) const
{
    std::vector<TriangleSelectorPatch::WeaveParams> out(MAX_SLOTS);
    m_weave_any_auto_angle = false;
    if (!mv || !m_weave_preview) return out;   // toggle off → all flat

    Slic3r::ColorSci::Material mats[4];
    std::vector<std::string>   fcolors;
    gizmo_materials(mats, fcolors);
    // NEOTKO_SANDWICH_TAG — Fase 3.2 (s167 plan): real bg for the PathBlend
    // ramp+cap Beer-Lambert blend below (same resolution as the Pro mode
    // "Result" swatch — Fase 2). ColorStitch's weave doesn't need this (it
    // looks up flat tool colours, no transmission math).
    float bg_rgb[3] = {0.f, 0.f, 0.f};
    resolve_object_base_bg(mats, bg_rgb, owner);

    const Slic3r::BoundingBoxf3 bb = mv->mesh().bounding_box();   // object-local (fallback)
    // Real top-surface line width (resolved from config, no slice). Drives the line COUNT
    // so stripe/gradient scale matches what the slicer lays on the top.
    const float line_w = (float) GLGizmoColorStitchPainter_top_line_width();   // s314: spacing
    const double lh = GLGizmoColorStitchPainter_layer_height();

    const auto& mgr = SurfaceEffectProfileManager::get();
    for (int s = 1; s < MAX_SLOTS; ++s) {
        const int pid = mv->colorstitch_slot_to_profile_id[s];
        if (pid == 0) continue;
        const SurfaceEffectProfile* p = mgr.find(pid);
        if (!p) continue;
        const std::map<std::string, std::string> kv = colorstitch_top_kv(*p);
        PathBlendPassConfig pbc;
        const bool is_pathblend = kv.empty() && pathblend_top_config(*p, pbc);
        // s318 F3 — opción A, igual que ColorStitchPaintPreview::weave_islands_for_volume.
        const ModelObject* fr_owner = owner
            ? owner
            : (m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr);
        const auto zr = ColorStitchPaintPreview::resolve_top_zone(*p, mv, fr_owner, mats);
        if (!zr && kv.empty() && !is_pathblend) continue;   // neither ColorStitch nor PathBlend → flat

        // PathBlend's axis is fixed at 0 (see pathblend_make_weave doc comment);
        // ColorStitch follows its own configured band angle.
        bool is_auto = false;
        // NEOTKO_PATHBLEND_TAG — s280e: el ángulo de PathBlend YA se dibuja.
        // Estaba clavado a 0.f, y eso era medio correcto por accidente: con theta=0 el eje
        // de color del shader sale `proj = model_pos.y`, que coincidía con el eje Y al que
        // el motor tenía clavado el degradado. O sea el degradado salía bien y la DIRECCIÓN
        // DE LAS LÍNEAS mal — poner 45 y no ver ni rastro de 45. Con el motor arreglado
        // (s280e: el eje se mide perpendicular a las líneas reales) el eje del preview es
        // simplemente el ángulo, misma convención que ColorStitch: sin +90, verificada
        // contra el slice por el ojo del usuario en s146. Y ya no hay caso degenerado que
        // avisar: a cualquier ángulo el degradado existe y va perpendicular a las líneas.
        // Auto (-1) → base real del relleno, igual que ColorStitch; el aviso lo da el
        // contorno violeta pulsante.
        float theta;
        if (!is_pathblend)
            theta = ColorStitchPaintPreview::colorstitch_weave_theta(kv, is_auto);
        else if (pbc.fill_angle >= 0)
            theta = float(pbc.fill_angle) * float(M_PI) / 180.f;
        else
            theta = ColorStitchPaintPreview::weave_solid_infill_dir_rad();
        // NEOTKO_PATHBLEND_TAG — s280b/s280d/s280e: PathBlend en ángulo AUTO también se marca.
        // No es cosmético: `_t_of()` (Fill.cpp) saca la altura de cada línea de su posición
        // sobre el eje del degradado, y ese eje es perpendicular a las líneas ⇒ el ángulo
        // decide el degradado, no sólo el acabado. Con -1 el ángulo alterna 90° por paridad
        // de capa, así que el degradado GIRA de una capa a la siguiente y no hay uno único
        // que previsualizar. Por eso el aviso sigue haciendo falta justo aquí.
        // 📌 s280e — ya NO es "centroide Y": el eje estaba clavado a Y y ése era el bug que
        // colapsaba el efecto a 90°. Ahora se mide sobre las líneas reales.
        if (is_pathblend && pbc.fill_angle < 0) is_auto = true;
        if (zr) is_auto = ColorStitchPaintPreview::top_zone_auto(*zr);
        if (is_auto) m_weave_any_auto_angle = true;
        // s318 F3 — mismo marco que el motor (ColorStitchPaintPreview::weave_frame). Semiplano
        // cuando el motor MIDE el eje: ColorStitch en auto y PathBlend siempre.
        const ColorStitchPaintPreview::WeaveFrame fr = zr
            ? ColorStitchPaintPreview::top_zone_frame(*zr, false)
            : ColorStitchPaintPreview::weave_frame(mv, fr_owner, theta,
                                                   /*canon*/ is_pathblend || is_auto);
        const ColorStitchPaintPreview::WeaveFrame fr2 = zr
            ? ColorStitchPaintPreview::top_zone_frame(*zr, true) : fr;

        // -- surface span along the cross-axis: union of the slot's painted facets (this is
        // the PER-SLOT fallback used by fragment patches; the per-ISLAND split happens in
        // build_ebt_weave_islands_for_volume). Falls back to the object AABB when no facets.
        float pmin = 1e9f, pmax = -1e9f;
        float qmin = 1e9f, qmax = -1e9f;   // s318 F3 — sobre el eje del pase de arriba
        bool  have_painted = false;
        if (sel) {
            const indexed_triangle_set its = sel->get_facets(static_cast<EnforcerBlockerType>(s));
            for (const stl_vertex& v : its.vertices) {
                const float pr = fr.proj(v.x(), v.y(), v.z());   // s318 F3
                pmin = std::min(pmin, pr); pmax = std::max(pmax, pr);
                const float pr2 = fr2.proj(v.x(), v.y(), v.z());
                qmin = std::min(qmin, pr2); qmax = std::max(qmax, pr2);
                have_painted = true;
            }
        }
        if (!have_painted) {
            for (double X : { bb.min.x(), bb.max.x() })
                for (double Y : { bb.min.y(), bb.max.y() })
                    for (double Z : { bb.min.z(), bb.max.z() }) {   // s318 F3: el eje puede tener Z
                        const float pr = fr.proj(float(X), float(Y), float(Z));
                        pmin = std::min(pmin, pr); pmax = std::max(pmax, pr);
                        const float pr2 = fr2.proj(float(X), float(Y), float(Z));
                        qmin = std::min(qmin, pr2); qmax = std::max(qmax, pr2);
                    }
        }
        TriangleSelectorPatch::WeaveParams w;
        if (zr) {
            w = ColorStitchPaintPreview::make_zone_weave(*zr, fcolors, mats, bg_rgb, line_w, lh,
                                                         pmin, pmax, qmin, qmax);   // s318 F3 opción A
        } else {
            w = is_pathblend
                ? pathblend_make_weave(pbc, mats, bg_rgb, lh, theta, pmin, pmax, line_w, fr.anchor_proj)
                : colorstitch_make_weave(kv, fcolors, theta, pmin, pmax, line_w, fr.anchor_proj);
            std::copy(fr.axis, fr.axis + 3, w.axis);   // s318 F3
        }
        // NEOTKO_COLORSTITCH_TAG — marca la banda: la lee el marcador de ángulo auto.
        w.auto_angle = is_auto;
        if (w.on) out[s] = std::move(w);
    }
    return out;
}

// s233 F3 — el cuerpo se mudó a ColorStitchPaintPreview::weave_islands_for_volume (mismo
// código, con el selector y el objeto por parámetro). Aquí queda el wrapper que aporta
// el gate m_weave_preview, el default "objeto activo" y el flag de ángulo auto.
void GLGizmoColorStitchPainter::build_ebt_weave_islands_for_volume(
        const ModelVolume* mv, const TriangleSelectorPatch* sel,
        std::unordered_map<int,int>& facet_weave_idx,
        std::vector<TriangleSelectorPatch::WeaveParams>& weave_list,
        const ModelObject* owner) const
{
    facet_weave_idx.clear();
    weave_list.clear();
    if (!m_weave_preview) return;
    const ModelObject* mo = owner
        ? owner
        : (m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr);
    bool any_auto = false;
    ColorStitchPaintPreview::weave_islands_for_volume(mv, sel, mo, facet_weave_idx,
                                                   weave_list, &any_auto);
    if (any_auto) m_weave_any_auto_angle = true;
}

void GLGizmoColorStitchPainter::refresh_selector_palettes()
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
    // s232 DEBUG — el dato que falta para cerrar el caso del ensamblado: qué slots
    // tienen CARAS de verdad en CADA volumen, y con qué perfil. Es lo que distingue
    // "el slot está en la tabla pero sin pintar" (herencia del Assemble, inofensiva) de
    // "hay pintura pero el preview la busca con el número de slot de otro volumen".
    {
        int _v = -1;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            ++_v;
            if (_v >= (int)m_triangle_selectors.size() || !m_triangle_selectors[_v]) continue;
            std::ostringstream os;
            os << "VOL_SLOTS vol=" << _v << " name='" << mv->name << "' ";
            for (int s = 1; s < MAX_SLOTS; ++s) {
                const int pid = mv->colorstitch_slot_to_profile_id[s];
                const int nf  = m_triangle_selectors[_v]->num_facets(
                                    static_cast<EnforcerBlockerType>(s));
                if (pid == 0 && nf == 0) continue;
                os << "s" << s << "(pid=" << pid << ",faces=" << nf << ") ";
            }
            NeoDebug::write(NeoDebug::BOTTOM, os.str());
        }
    }
    int idx = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        if (idx >= int(m_triangle_selectors.size())) break;
        auto* tsp = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[idx].get());
        if (tsp) {
            tsp->set_ebt_colors(build_ebt_colors_for_volume(mv));
            tsp->set_ebt_weave(build_ebt_weave_for_volume(mv, tsp));   // per-slot fallback
            // per-ISLAND weave: each painted zone (stair step) scaled to its own extent.
            {
                std::unordered_map<int,int> fwi;
                std::vector<TriangleSelectorPatch::WeaveParams> wl;
                build_ebt_weave_islands_for_volume(mv, tsp, fwi, wl);
                tsp->set_ebt_weave_islands(std::move(fwi), std::move(wl));
            }
            tsp->request_update_render_data();
        }
    }
}

// NEOTKO_COLORSTITCH_TAG_START — PR.2 palette panel (style strips in the gizmo)
//
// s233 — los cuerpos de estos dos (materiales del contexto + fondo real del objeto) se
// movieron a slic3r/GUI/ColorStitchPaintPreview.{hpp,cpp}, porque la vista 3D normal los
// necesita sin gizmo. Aquí quedan como wrappers finos: lo único que aportan es el
// default "el objeto ACTIVO del gizmo" que asumen sus call-sites.
void GLGizmoColorStitchPainter::gizmo_materials(Slic3r::ColorSci::Material out[4],
                                             std::vector<std::string>& fcolors_out) const
{
    ColorStitchPaintPreview::materials(out, fcolors_out);
}

bool GLGizmoColorStitchPainter::resolve_object_base_bg(const Slic3r::ColorSci::Material mats[4],
                                                     float bg_rgb[3],
                                                     const ModelObject* mo_override) const
{
    const ModelObject* mo = mo_override
        ? mo_override
        : (m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr);
    return ColorStitchPaintPreview::object_base_bg(mats, mo, bg_rgb);
}

// s169 F3 — ¿el objeto activo tiene "MixedFilament Object" en ON? Mismo patrón
// de lectura que object_has_mixed_filament (config option en el ModelObject).
bool GLGizmoColorStitchPainter::active_object_mixed_filament_mode() const
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return false;
    const auto* opt = dynamic_cast<const ConfigOptionBool*>(mo->config.option("mixed_filament_sandwich_mode"));
    return opt && opt->value;
}

// NEOTKO_MIXEDFIL_SANDWICH_TAG — s231 F1: gate compartido UI ↔ canvas. Ver la nota
// en on_mouse: hasta ahora sólo lo consultaba el panel, y el pincel del 3D no.
bool GLGizmoColorStitchPainter::painting_blocked() const
{
    return active_object_mixed_filament_mode();
}


// Regenera las tres paletas SOLO cuando cambia el contexto. La firma incluye
// filamentos + TD + layer height + tools del gradient.
void GLGizmoColorStitchPainter::rebuild_palettes_if_stale()
{
    namespace CS = Slic3r::ColorSci;
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    const double lh = GLGizmoColorStitchPainter_layer_height();

    // NEOTKO_SANDWICH_TAG — Fase 2 (s167 plan): fondo real del objeto activo
    // (antes negro hardcodeado). Se calcula antes de la firma de caché para
    // que un cambio de objeto (color base distinto) invalide la paleta igual
    // que un cambio de TD/filamento — si no, cambiar de objeto sin tocar el
    // resto del contexto dejaría paletas coherentes con el objeto anterior.
    float bg_rgb[3] = { 0.f, 0.f, 0.f };
    const bool has_bg = resolve_object_base_bg(mats, bg_rgb);

    auto* ac = wxGetApp().app_config;
    std::string key;
    for (int t = 0; t < 4; ++t) {
        key += fcolors[t] + "|";
        key += (ac ? ac->get("neotko_td_" + std::to_string(t + 1)) : "") + "|";
    }
    key += std::to_string(lh) + "|" + std::to_string(m_grad_tool_a)
         + "|" + std::to_string(m_grad_tool_b) + "|"
         + std::to_string(m_cs_tool_a) + "|" + std::to_string(m_cs_tool_b) + "|"
         + std::to_string(bg_rgb[0]) + "," + std::to_string(bg_rgb[1]) + "," + std::to_string(bg_rgb[2]);
    if (key == m_pal_key) return;   // contexto intacto → caché válida
    m_pal_key = key;

    CS::PredictOptions o;
    o.layer_height = lh;
    if (has_bg) { o.bg_rgb[0] = bg_rgb[0]; o.bg_rgb[1] = bg_rgb[1]; o.bg_rgb[2] = bg_rgb[2]; }
    m_pal_flat  = CS::build_palette(CS::PaletteKind::Flat,  mats, o);
    // s171 — "Mixed (ColorStitch)" NUKEADO (venía de predict_mixed_palette, ruta
    // muerta desde s120 que siempre predecía amarillo en el preview TD).
    // Reemplazado por "ColorStitch Pattern Color": solo ColorStitch, 2 pasadas
    // (Top+Penu) sobrepuestas, mismos tools/ángulo — ver build_colorstitch_gradient_palette.
    m_pal_cs_gradient = build_colorstitch_gradient_palette(mats, o, m_cs_tool_a, m_cs_tool_b, 8);

    CS::GradientSpec gs;
    gs.tool_a       = m_grad_tool_a;
    gs.tool_b       = m_grad_tool_b;
    gs.layer_height = lh;
    const CS::GradientSpec gsc = CS::sanitize(gs);
    m_pal_gradient = CS::build_palette(CS::PaletteKind::GradientRamp, mats, o, &gsc);
}

void GLGizmoColorStitchPainter::render_palette_panel(float window_width)
{
    rebuild_palettes_if_stale();

    std::vector<std::string> fcolors;
    if (auto* op = wxGetApp().preset_bundle->project_config
                       .option<ConfigOptionStrings>("filament_colour"))
        fcolors = op->values;
    // nfil = nº real de filamentos (antes de rellenar a 4) → no ofrecer chips
    // A/B placeholder grises para tools que el usuario no tiene cargados.
    const int nfil = std::max(1, std::min(4, (int)fcolors.size()));
    while (fcolors.size() < 4) fcolors.push_back("#808080");

    // s318 — el Generator en TRES TARJETAS iguales (glifo · nombre · dato), siempre abiertas: los
    // CollapsingHeader naranjas de Orca se fueron. A y B en una sola línea de fichas numeradas.
    // El comportamiento es el de siempre: cambiar A/B basta (rebuild_palettes_if_stale lo detecta en
    // la firma de caché) y elegir un swatch fija m_active_pal_kind/idx (s231 F5).
    (void)window_width;
    const float u = neo_u();
    ImU32 cols[4];
    for (int t = 0; t < 4; ++t) cols[t] = tool_col_u32(fcolors, t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto ab_row = [&](const char* id, int& ta, int& tb) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("A");
        ImGui::SameLine(0.f, 0.35f * u);
        const int ca = neo_tool_chips((std::string(id) + "a").c_str(), cols, nfil, ta);
        if (ca >= 0) ta = ca;
        ImGui::SameLine(0.f, 0.35f * u);
        {
            const ImVec2 gp = ImGui::GetCursorScreenPos();
            const float  fh = ImGui::GetFrameHeight();
            draw_glyph(dl, ImVec2(gp.x, gp.y + (fh - 0.8f * u) * 0.5f), 0.8f * u, Glyph::ChevR, neo_col_u32(NeoCol::TextDim));
            ImGui::Dummy(ImVec2(0.8f * u, fh));
        }
        ImGui::SameLine(0.f, 0.35f * u);
        ImGui::TextDisabled("B");
        ImGui::SameLine(0.f, 0.35f * u);
        const int cb = neo_tool_chips((std::string(id) + "b").c_str(), cols, nfil, tb);
        if (cb >= 0) tb = cb;
    };
    // Una tarjeta: fondo biselado detrás (splitter propio), cabecera con glifo y dato, y el cuerpo.
    auto card = [&](Glyph g, const std::string& title, const std::string& aside, const std::function<void(float)>& body) {
        ImDrawListSplitter sp;
        sp.Split(dl, 2);
        sp.SetCurrentChannel(dl, 1);
        const ImVec2 c0  = ImGui::GetCursorScreenPos();
        const float  cw  = ImGui::GetContentRegionAvail().x;
        const float  pad = 0.55f * u;
        ImGui::SetCursorScreenPos(ImVec2(c0.x + pad, c0.y + pad));
        ImGui::BeginGroup();
        ImGui::PushItemWidth(cw - 2.f * pad);
        {
            const ImVec2 gp = ImGui::GetCursorScreenPos();
            const float  gs = 1.1f * u;
            draw_glyph(dl, ImVec2(gp.x, gp.y + (ImGui::GetTextLineHeight() - gs) * 0.5f), gs, g, neo_col_u32(NeoCol::AccentBright));
            ImGui::Dummy(ImVec2(gs, ImGui::GetTextLineHeight()));
            ImGui::SameLine(0.f, 0.4f * u);
            ImGui::TextUnformatted(title.c_str());
            if (!aside.empty()) {
                const float aw = ImGui::CalcTextSize(aside.c_str()).x;
                neo_same_line_at(c0.x + cw - pad - aw);
                ImGui::TextDisabled("%s", aside.c_str());
            }
        }
        ImGui::Dummy(ImVec2(0.f, 0.15f * u));
        body(cw - 2.f * pad);   // el ancho útil, con el padding de los dos lados ya descontado
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        const float bottom = ImGui::GetItemRectMax().y + pad;
        sp.SetCurrentChannel(dl, 0);
        neo_card_bg(dl, c0, ImVec2(c0.x + cw, bottom), 0.7f * u, false, neo_fade(NeoCol::Surface, 0.45f));
        sp.Merge(dl);
        ImGui::SetCursorScreenPos(ImVec2(c0.x, bottom));
        ImGui::Dummy(ImVec2(cw, 0.45f * u));
    };
    int ci;
    card(Glyph::TabGen, _u8L("Gradient ramp"), _u8L("Top only"), [&](float bw) {
        ab_row("##grad_", m_grad_tool_a, m_grad_tool_b);
        ci = draw_palette_strip("##pal_gradient", m_pal_gradient, fcolors, bw, 0.f,
                                m_active_pal_kind == 1 ? m_active_pal_idx : -1);
        // s231 F5 — set_active_recipe limpia el enlace: anotar aquí qué swatch se está usando.
        if (ci >= 0) { set_active_recipe(m_pal_gradient[ci], _u8L("Gradient"));
                       m_active_pal_kind = 1; m_active_pal_idx = ci; }
    });
    card(Glyph::KSolid, _u8L("Flat color"), std::to_string(m_pal_flat.size()), [&](float bw) {
        ci = draw_palette_strip("##pal_flat", m_pal_flat, fcolors, bw, 0.f,
                                m_active_pal_kind == 2 ? m_active_pal_idx : -1);
        if (ci >= 0) { set_active_recipe(m_pal_flat[ci], _u8L("Flat"));
                       m_active_pal_kind = 2; m_active_pal_idx = ci; }
    });
    // s171 — "ColorStitch Pattern Color": la tira sale de build_colorstitch_gradient_palette (sólo
    // ColorStitch, 2 pasadas Top+Penu sobrepuestas, mismos tools/ángulo).
    card(Glyph::KStitch, _u8L("ColorStitch Pattern Color"), "Top + Penu", [&](float bw) {
        ab_row("##cs_", m_cs_tool_a, m_cs_tool_b);
        ci = draw_palette_strip("##pal_cs_gradient", m_pal_cs_gradient, fcolors, bw, 0.f,
                                m_active_pal_kind == 3 ? m_active_pal_idx : -1);
        if (ci >= 0) { set_active_recipe(m_pal_cs_gradient[ci], _u8L("ColorStitch"));
                       m_active_pal_kind = 3; m_active_pal_idx = ci; }
    });
}

// NEOTKO_COLORSTITCH_TAG — s118: fwd-decl (recipe_argb se define más abajo, tras
// render_pro_mode_panel, que ahora lo usa para el preview del write-back live).
static uint32_t recipe_argb(const Slic3r::ColorSci::ColorRecipe& r);

// NEOTKO_MIXEDFIL_SANDWICH_TAG — s169 F0: extraído del bloque inline de
// render_pro_mode_panel para que el departamento Object (F3) pueda reusar el
// mismo criterio ("¿este objeto tiene un MixedFilament asignado?").
static bool object_has_mixed_filament(const ModelObject* mo)
{
    if (!mo) return false;
    std::vector<std::string> fcolors;
    if (auto* o = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
        fcolors = o->values;
    const size_t num_physical = fcolors.size();
    int extruder_id = 0;
    for (const ModelVolume* mv : mo->volumes)
        if (mv && mv->is_model_part()) { extruder_id = mv->extruder_id(); break; }
    return extruder_id > (int)num_physical;
}

// s350 — LA RECETA DE CERCA (fase C): cada píxel apila, de abajo arriba (Penu y luego Top), la
// línea de cada pasada que le cae encima, con su filamento y su TD (Beer-Lambert por canal). Cada
// pasada con su ángulo (el suyo, o auto: Top 45°, Penu 135°, y las siguientes de la misma zona
// giradas 90°) y su patrón ColorStitch REAL, así que Top y Penu se CRUZAN como en la pieza.
// Es el mismo modelo que sandwich_colour_stacked (que es la MEDIA de esto sobre la superficie).
static void draw_recipe_texture(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
                                const Slic3r::SurfacePassStack& top, const Slic3r::SurfacePassStack& penu,
                                const Slic3r::ColorSci::Material mats[4], const float bg[3])
{
    namespace CS = Slic3r::ColorSci;
    using Slic3r::SurfacePassKind;
    struct PL { SurfacePassKind kind; std::vector<int> seq; int tool = 0; float r = 0.f; float ca = 1.f, sa = 0.f;
                int pbb = 0, pbt = -1, ease = 0; };
    std::vector<PL> pls;
    // 21-oct-2026: grosor en mm (el TD va en mm) = fracción de capa × altura de capa.
    const float lmm = float(std::max(0.01, ColorStitchPaintPreview::weave_layer_height()));
    auto add_zone = [&](const Slic3r::SurfacePassStack& st, bool is_penu) {
        if (!st.enabled) return;
        int k = 0;
        for (const Slic3r::SurfacePass& p : st.passes) {
            PL l;
            l.kind = p.kind;
            l.r    = std::max(0.f, float(p.ratio)) * lmm;
            if (l.r < 1e-6f || p.kind == SurfacePassKind::None) continue;
            const float deg = p.angle >= 0 ? float(p.angle) : (is_penu ? 135.f : 45.f) + 90.f * float(k);
            l.ca = std::cos(deg * float(M_PI) / 180.f);
            l.sa = std::sin(deg * float(M_PI) / 180.f);
            l.tool = std::clamp(p.solid_tool, 0, 3);
            if (p.kind == SurfacePassKind::ColorStitch) {
                for (char ch : CS::pass_pattern_of(p, is_penu)) { const int t = int(ch) - '1'; if (t >= 0 && t < 4) l.seq.push_back(t); }
                if (l.seq.empty()) continue;
            } else if (p.kind == SurfacePassKind::PathBlend) {
                Slic3r::PathBlendPassConfig pb;
                const auto it = p.pathblend.kv.find("blob");
                if (it != p.pathblend.kv.end() && !it->second.empty()) pb = Slic3r::PathBlendPassConfig::from_blob_json(it->second);
                l.pbb  = std::clamp(pb.tool_bottom, 0, 3);
                l.pbt  = (pb.mode == Slic3r::PathBlendPassConfig::Mode::Full && pb.tool_top >= 0) ? std::clamp(pb.tool_top, 0, 3) : -1;
                l.ease = pb.ease_mode;
            }
            pls.push_back(std::move(l));
            ++k;
        }
    };
    add_zone(penu, true);
    add_zone(top, false);
    const float W = b.x - a.x, H = b.y - a.y;
    if (W < 2.f || H < 2.f) return;
    const float lw   = std::max(3.f, std::min(W, H) / 11.f);   // ancho de línea en pantalla
    const float cell = std::max(1.5f, std::min(W, H) / 40.f);
    const float cx = 0.5f * (a.x + b.x), cy = 0.5f * (a.y + b.y), span = std::max(W, H);
    auto lay = [&](float acc[3], int tool, float r) {   // una capa maciza encima de acc (lineal)
        const CS::Material& m = mats[std::clamp(tool, 0, 3)];
        for (int c = 0; c < 3; ++c) {
            const float lc = CS::srgb_to_linear(m.rgb[c]);
            if (r <= 0.f) continue;
            if (m.td[c] < 1e-6f) { acc[c] = lc; continue; }
            const float t = std::pow(0.1f, r / m.td[c]);
            acc[c] = lc * (1.f - t) + acc[c] * t;
        }
    };
    auto ease = [](int mode, float t) {
        switch (mode) { case 1: return t * t; case 2: return 1.f - (1.f - t) * (1.f - t);
                        case 3: return t < 0.5f ? 2.f * t * t : 1.f - 2.f * (1.f - t) * (1.f - t); default: return t; }
    };
    for (float y = a.y; y < b.y; y += cell)
        for (float x = a.x; x < b.x; x += cell) {
            const float px = x + 0.5f * cell - cx, py = y + 0.5f * cell - cy;
            float acc[3];
            for (int c = 0; c < 3; ++c) acc[c] = CS::srgb_to_linear(bg[c]);
            for (const PL& l : pls) {
                const float s = px * l.ca + py * l.sa;   // a lo ancho de las líneas
                switch (l.kind) {
                case SurfacePassKind::Solid: lay(acc, l.tool, l.r); break;
                case SurfacePassKind::ColorStitch: {
                    const int n = int(l.seq.size());
                    const int i = ((int(std::floor(s / lw)) % n) + n) % n;
                    lay(acc, l.seq[size_t(i)], l.r);
                    break;
                }
                case SurfacePassKind::PathBlend: {
                    const float t = std::clamp(s / span + 0.5f, 0.f, 1.f);
                    const float h = l.r * ease(l.ease, t);
                    lay(acc, l.pbb, h);
                    if (l.pbt >= 0) lay(acc, l.pbt, l.r - h);
                    break;
                }
                default: break;
                }
            }
            const ImU32 col = IM_COL32(int(std::clamp(CS::linear_to_srgb(acc[0]), 0.f, 1.f) * 255.f),
                                       int(std::clamp(CS::linear_to_srgb(acc[1]), 0.f, 1.f) * 255.f),
                                       int(std::clamp(CS::linear_to_srgb(acc[2]), 0.f, 1.f) * 255.f), 255);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, b.x), std::min(y + cell, b.y)), col);
        }
}

// s318 — LA CABECERA DEL PRO. Un corte de la pieza: Top, Penultimate, un trozo rayado que es la pieza
// y Bottom, cada zona con su banda de pases (draw_zone, la misma que usaban Recipe y el tooltip de
// la rejilla) y, a la derecha, el Result grande con halo. Es un ESQUEMA y lo dice en su cara (cada
// zona = una capa; los grosores sí van a escala dentro de la zona). Bottom se atenúa porque no
// forma parte del color.
static void draw_pro_hero(const Slic3r::SurfacePassStack& top, const Slic3r::SurfacePassStack& penu,
                          const Slic3r::SurfacePassStack& bottom, const float out[3],
                          const std::vector<std::string>& fcolors, double layer_h, float width = -1.f,
                          const Slic3r::ColorSci::Material* tex_mats = nullptr, const float* tex_bg = nullptr)
{
    const float  u       = neo_u();
    // s350 — ancho opcional: el panel de colores del sticker lo dibuja en una columna fija (con el
    // ancho disponible, una ventana AlwaysAutoResize se ensancha sola sin fin).
    const float  w       = width > 0.f ? width : ImGui::GetContentRegionAvail().x;
    const float  pad     = 0.6f * u;
    const float  bh      = 1.35f * u;
    const float  ph      = 1.0f * u;
    const float  gap     = 0.3f * u;
    const float  fs      = 0.7f * u;
    const float  rs      = 5.f * u;
    const float  stamp_h = 0.9f * u;
    const float  h       = 2.f * pad + 3.f * bh + ph + 3.f * gap + stamp_h;
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    ImFont*      font    = ImGui::GetFont();
    const ImVec2 p0      = ImGui::GetCursorScreenPos();
    neo_draw_canvas(dl, p0, w, h);

    const std::string lt = _u8L("Top"), lp = _u8L("Penultimate"), lb = _u8L("Bottom");
    float lw = 0.f;
    for (const std::string* t : { &lt, &lp, &lb })
        lw = std::max(lw, font->CalcTextSizeA(fs, FLT_MAX, 0.f, t->c_str()).x);
    lw = std::min(lw, w * 0.24f);
    const float gsz    = 1.0f * u;
    const float x_lab  = p0.x + pad;
    const float x_band = x_lab + lw + 0.3f * u + gsz + 0.4f * u;
    const float x_res  = p0.x + w - pad - rs;
    const float band_w = std::max(2.f * u, x_res - 0.8f * u - x_band);

    auto row = [&](float y, float hh, const std::string& lbl, Glyph g) {
        const std::string t  = fit_text(lbl, lw / 0.7f);
        const ImVec2      ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, t.c_str());
        dl->AddText(font, fs, ImVec2(x_lab + lw - ts.x, y + (hh - ts.y) * 0.5f), neo_col_u32(NeoCol::TextDim), t.c_str());
        draw_glyph(dl, ImVec2(x_lab + lw + 0.3f * u, y + (hh - gsz) * 0.5f), gsz, g, neo_col_u32(NeoCol::TextDim));
    };
    auto band = [&](float y, const Slic3r::SurfacePassStack& st, bool is_penu, bool dim) {
        const ImVec2 a(x_band, y), b(x_band + band_w, y + bh);
        if (st.passes.empty()) { neo_card_dashed(dl, a, b); return; }
        // s319 — cada pase con SU tejido (el mismo pro_pass_preview de las filas de abajo: rayas de
        // ColorStitch a su ángulo, rampa de PathBlend, líneas del Solid), apilados de arriba abajo
        // en el orden físico y con el alto proporcional a su grosor. Antes sólo salían los colores.
        double tot = 0.0;
        for (const Slic3r::SurfacePass& sp : st.passes) tot += std::max(0.0, sp.ratio);
        if (tot < 1e-6) tot = 1.0;
        float yy = a.y;
        const int np = (int)st.passes.size();
        for (int k = np - 1; k >= 0; --k) {
            const float hk = (k == 0) ? (b.y - yy) : bh * float(std::max(0.0, st.passes[k].ratio) / tot);
            if (hk > 0.5f)
                pro_pass_preview(dl, ImVec2(a.x, yy), ImVec2(b.x, yy + hk), st.passes[k], is_penu, fcolors, layer_h);
            yy += hk;
        }
        dl->AddRect(a, b, IM_COL32(0, 0, 0, 110), 2.f);
        if (dim) dl->AddRectFilled(a, b, neo_fade(NeoCol::Canvas, 0.45f), 2.f);
    };
    float y = p0.y + pad;
    row(y, bh, lt, Glyph::ZoneTop);    band(y, top,  false, false); y += bh + gap;
    row(y, bh, lp, Glyph::ZonePenu);   band(y, penu, true,  false); y += bh + gap;
    hatch_rect(dl, ImVec2(x_band, y), ImVec2(x_band + band_w, y + ph), neo_fade(NeoCol::TextDim, 0.25f), 6.f);
    dashed_line(dl, ImVec2(x_band, y), ImVec2(x_band, y + ph), neo_col_u32(NeoCol::GridMajor), 1.f, 3.f, 3.f);
    dashed_line(dl, ImVec2(x_band + band_w, y), ImVec2(x_band + band_w, y + ph), neo_col_u32(NeoCol::GridMajor), 1.f, 3.f, 3.f);
    y += ph + gap;
    row(y, bh, lb, Glyph::ZoneBottom); band(y, bottom, false, true); y += bh + gap;
    {
        const float sfs = 0.62f * u;
        const std::string stamp = fit_text(_u8L("schematic · each zone = 1 layer"), (x_res - x_lab - 0.5f * u) / 0.62f);
        dl->AddText(font, sfs, ImVec2(x_lab, y + 0.1f * u), neo_col_u32(NeoCol::TextDim), stamp.c_str());
    }
    {
        const float  ry = p0.y + (h - rs) * 0.5f - 0.3f * u;
        const ImVec2 r0(x_res, ry), r1(x_res + rs, ry + rs);
        const ImU32 flat = IM_COL32((int)std::min(255.f, out[0] * 255.f),
                                    (int)std::min(255.f, out[1] * 255.f),
                                    (int)std::min(255.f, out[2] * 255.f), 255);
        if (tex_mats && tex_bg) {
            // s350 — arriba la receta DE CERCA (líneas cruzadas, TD), abajo la franja DE LEJOS.
            const float split = r0.y + rs * 0.72f;
            dl->PushClipRect(r0, r1, true);
            const bool bot_only = top.passes.empty() && penu.passes.empty() && !bottom.passes.empty();   // s350
            draw_recipe_texture(dl, r0, ImVec2(r1.x, split), bot_only ? Slic3r::ColorSci::reversed_stack(bottom) : top,
                                bot_only ? Slic3r::SurfacePassStack{} : penu, tex_mats, tex_bg);
            dl->AddRectFilled(ImVec2(r0.x, split), r1, flat);
            dl->PopClipRect();
            if (ImGui::IsMouseHoveringRect(r0, r1))
                ImGui::SetTooltip("%s", _u8L("Top: up close, the lines as they print\n"
                                             "(Top over Penultimate, crossing, each filament with its TD).\n"
                                             "Bottom strip: the colour you see from a distance.").c_str());
        } else
            dl->AddRectFilled(r0, r1, flat, 5.f);
        neo_halo(dl, r0, r1, 5.f);
        const float lfs = 0.66f * u;
        const std::string res = _u8L("Result");
        const ImVec2 rt = font->CalcTextSizeA(lfs, FLT_MAX, 0.f, res.c_str());
        dl->AddText(font, lfs, ImVec2(x_res + (rs - rt.x) * 0.5f, r0.y - rt.y - 0.2f * u), neo_col_u32(NeoCol::TextDim), res.c_str());
        const char* tp = "Top + Penu";
        const ImVec2 tt = font->CalcTextSizeA(lfs, FLT_MAX, 0.f, tp);
        dl->AddText(font, lfs, ImVec2(x_res + (rs - tt.x) * 0.5f, r1.y + 0.25f * u), neo_col_u32(NeoCol::TextDim), tp);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// s350 — LA TARJETA DE UNA RECETA (boceto de Neotko, WIP/PALETTE_HOVER_TOOLTIP_MOCKUP_s349.png):
// izquierda, columna con las zonas en miniatura (Top, Penu, la pieza, Bottom) con el patrón REAL de
// cada pasada (el mismo dibujo que el Pro); derecha, el cuadro grande con la receta de cerca (líneas
// cruzadas, TD) y una franja con el color de lejos; el ángulo, encima, abajo a la izquierda. Dueño
// único: tooltip de Palette y tramos/colores del panel de stickers. Se llama DENTRO de un tooltip.
void GLGizmoColorStitchPainter::draw_recipe_card(const SurfacePassStack& st_top, const SurfacePassStack& st_penu,
                                                 const SurfacePassStack& st_bot, bool auto_generated)
{
    std::vector<std::string> fcolors;
    { Slic3r::ColorSci::Material mm[4]; gizmo_materials(mm, fcolors); }

    namespace CS = Slic3r::ColorSci;
    const float u  = neo_u();
    const float big = 5.2f * u, gl = 0.85f * u, colw = 2.6f * u, gap = 0.25f * u;
    const float zh = (big - 3.f * gap) / 4.f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    
    const double tlh = GLGizmoColorStitchPainter_layer_height();
    auto zone = [&](float y, const SurfacePassStack& st, bool is_penu, Glyph g, bool dim) {
        draw_glyph(tdl, ImVec2(o.x, y + (zh - gl) * 0.5f), gl, g, neo_col_u32(NeoCol::TextDim));
        const ImVec2 za(o.x + gl + 0.2f * u, y), zb(o.x + gl + 0.2f * u + colw, y + zh);
        if (st.passes.empty()) { neo_card_dashed(tdl, za, zb); return; }
        double tot = 0.0;
        for (const SurfacePass& sp2 : st.passes) tot += std::max(0.0, sp2.ratio);
        if (tot < 1e-6) tot = 1.0;
        float yy = za.y;
        for (int k = int(st.passes.size()) - 1; k >= 0; --k) {
            const float hk = (k == 0) ? (zb.y - yy) : zh * float(std::max(0.0, st.passes[size_t(k)].ratio) / tot);
            if (hk > 0.5f) pro_pass_preview(tdl, ImVec2(za.x, yy), ImVec2(zb.x, yy + hk), st.passes[size_t(k)], is_penu, fcolors, tlh);
            yy += hk;
        }
        tdl->AddRect(za, zb, IM_COL32(0, 0, 0, 110), 2.f);
        if (dim) tdl->AddRectFilled(za, zb, neo_fade(NeoCol::Canvas, 0.45f), 2.f);
    };
    float y = o.y;
    zone(y, st_top,  false, Glyph::ZoneTop,  false); y += zh + gap;
    zone(y, st_penu, true,  Glyph::ZonePenu, false); y += zh + gap;
    hatch_rect(tdl, ImVec2(o.x + gl + 0.2f * u, y), ImVec2(o.x + gl + 0.2f * u + colw, y + zh),
               neo_fade(NeoCol::TextDim, 0.25f), 6.f);
    y += zh + gap;
    zone(y, st_bot,  false, Glyph::ZoneBottom, true);
    // El cuadro grande: de cerca arriba, de lejos en la franja de abajo.
    const ImVec2 r0(o.x + gl + 0.2f * u + colw + 0.35f * u, o.y), r1(r0.x + big, r0.y + big);
    CS::Material tm[4];
    std::vector<std::string> tfc;
    gizmo_materials(tm, tfc);
    float tbg[3] = { 0.f, 0.f, 0.f };
    resolve_object_base_bg(tm, tbg);
    const float split = r0.y + big * 0.78f;
    tdl->PushClipRect(r0, r1, true);
    // s350 — receta sólo Bottom: la textura y el color, vistos desde la cama.
    const bool bot_only = st_top.passes.empty() && st_penu.passes.empty() && !st_bot.passes.empty();
    draw_recipe_texture(tdl, r0, ImVec2(r1.x, split), bot_only ? CS::reversed_stack(st_bot) : st_top,
                        bot_only ? SurfacePassStack{} : st_penu, tm, tbg);
    uint32_t argb = predict_argb_for(st_top, st_penu);
    if (bot_only) {
        float bc[3] = { 0.f, 0.f, 0.f };
        CS::sandwich_colour_bottom(st_bot, tm, tbg, ColorStitchPaintPreview::weave_layer_height(), bc);
        CS::ColorRecipe br; br.rgb = { bc[0], bc[1], bc[2] };
        argb = recipe_argb(br);
    }
    tdl->AddRectFilled(ImVec2(r0.x, split), r1, IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
    tdl->PopClipRect();
    tdl->AddRect(r0, r1, neo_col_u32(NeoCol::TextDim), 3.f, 0, 1.f);
    // Ángulo encima, abajo a la izquierda del cuadro de cerca.
    {
        int ang_t = 0, ang_p = 0;
        const bool has_t = cs_zone_angle(st_top,  false, ang_t);
        const bool has_p = !st_penu.passes.empty() && cs_zone_angle(st_penu, true, ang_p);
        std::string at;
        auto one = [&](const char* tag, int deg) {
            if (!at.empty()) at += " ";
            at += tag + (deg < 0 ? _u8L("auto") : std::to_string(deg) + "\xc2\xb0");
        };
        if (has_t) one("T", ang_t);
        if (has_p) one("P", ang_p);
        if (!at.empty()) {
            const float fs = 0.62f * u;
            ImFont* f = ImGui::GetFont();
            const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, at.c_str());
            const ImVec2 ta(r0.x + 2.f, split - ts.y - 3.f);
            tdl->AddRectFilled(ImVec2(ta.x - 1.f, ta.y - 1.f), ImVec2(ta.x + ts.x + 3.f, ta.y + ts.y + 1.f),
                               IM_COL32(0, 0, 0, 170), 2.f);
            tdl->AddText(f, fs, ImVec2(ta.x + 1.f, ta.y), IM_COL32(235, 235, 235, 255), at.c_str());
        }
    }
    if (auto_generated) {   // color de trabajo: la muesca de la muestra, también aquí
        const float k = 0.7f * u;
        tdl->AddTriangleFilled(r0, ImVec2(r0.x + k, r0.y), ImVec2(r0.x, r0.y + k), neo_fade(NeoCol::Ink, 0.85f));
    }
    ImGui::Dummy(ImVec2(r1.x - o.x, big));
}


// NEOTKO_COLORSTITCH_TAG — Bandeja "pro mode": compone Top/Penu + TD y muestra
// el color resultante en vivo (sandwich_colour_stacked, mismo motor que el
// Sandwich Editor). Produce un ColorRecipe → color activo de pintura (sin pasar
// por el preset). Opción B del revamp; v2 s108: filas estilo SandwichDialog con
// los tres kinds inline (Solid / ColorStitch / PathBlend Half|Full), cajita Z
// editable con auto-rescale y TD en rejilla de 2 columnas al final.
void GLGizmoColorStitchPainter::render_pro_mode_panel()
{
    namespace CS = Slic3r::ColorSci;
    // s169 F1 — el CollapsingHeader "Pro mode" se retiró: el departamento Pro DEL
    // selector segmentado ya hace de gate (solo se llama a esta función cuando
    // m_department==2), así que su contenido se dibuja siempre que se invoque.

    // s364 — se llegó con «Edit in Pro» desde el editor de un sticker: decirlo y dar la vuelta.
    // El sticker sigue en edición (y su color late en 3D); el color y el tramo elegidos se conservan.
    if (m_pro_from_sticker) {
        const ModelObject* smo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (!smo || m_editing_sticker_idx < 0 || m_editing_sticker_idx >= (int)smo->colorstitch_stickers.size())
            m_pro_from_sticker = false;
        else {
            const std::string& nm = smo->colorstitch_stickers[size_t(m_editing_sticker_idx)].name;
            const std::string back = _u8L("Back to sticker");
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const float bw    = ImGui::CalcTextSize(back.c_str()).x + 2.5f * neo_u();
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
            ImGui::TextUnformatted(fit_text(_u8L("Editing a recipe of sticker:") + " " + (nm.empty() ? std::string("?") : nm),
                                            std::max(4.f * neo_u(), right - ImGui::GetCursorScreenPos().x - bw - neo_u())).c_str());
            ImGui::PopStyleColor();
            neo_same_line_at(right - bw);
            if (neo_text_button((back + "##pro_back_stk").c_str(), NeoBtn::Normal, true, Glyph::Sticker)) {
                m_pro_from_sticker = false;
                m_department       = DEP_STICKERS;
                m_stkc_block_open  = true;
                if (smo->colorstitch_stickers[size_t(m_editing_sticker_idx)].is_multicolor()) m_stkc_tab = 1;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Return to the Stickers tab, on the same colour.\n"
                                             "What you change here is already on the sticker.").c_str());
            ImGui::Separator();
        }
    }

    std::vector<std::string> fcolors;
    CS::Material mats[4];
    gizmo_materials(mats, fcolors);          // fcolors viene rellenado a >=4
    int nfil = 4;
    if (auto* op = wxGetApp().preset_bundle->project_config
                       .option<ConfigOptionStrings>("filament_colour"))
        nfil = std::max(1, std::min(4, (int)op->values.size()));

    const double lh = GLGizmoColorStitchPainter_layer_height();

    if (!m_pro_seeded) {
        m_pro_top.enabled = true;
        if (m_pro_top.passes.empty()) {
            Slic3r::SurfacePass sp;
            sp.ratio = 1.0;    // s108: full-layer seed (ratio 0.0 predicted black)
            m_pro_top.passes.push_back(sp);
        }
        m_pro_seeded = true;
    }

    // --- Editores de zona (v2 s108: filas estilo SandwichDialog, arriba) ---
    // NEOTKO_COLORSTITCH_TAG — s139: medir el ancho disponible UNA sola vez aquí y
    // pasarlo a ambas zonas, para que Top y Penu anclen su Z-box (tamaño de capa) y
    // la banda al MISMO X (antes cada fila lo remedía y se desalineaban entre sí).
    const float pro_row_w = ImGui::GetContentRegionAvail().x;

    // s169 F3 — el toggle "MixedFilament Object" + swatch se movieron al
    // departamento Object (mismo código, ver render_pro_mode_panel history).
    // El gate "el motor bypassea per-face painting mientras esté ON" ahora lo
    // hace el CALLER a nivel de departamento (on_render_input_window envuelve
    // TODO el contenido de Paint/Palette/Pro en disabled_begin/end + banner
    // cuando active_object_mixed_filament_mode() — así que ya no hace falta
    // repetirlo aquí dentro).

    // Bottom stack re-loads from the selected profile (so editing profile A never
    // leaks into B). Done regardless of the active mode so it's fresh when shown.
    if (m_pro_bottom_loaded_id != m_selected_profile_id) {
        const auto* bp = SurfaceEffectProfileManager::get().find(m_selected_profile_id);
        m_pro_bottom = (bp && !bp->stack_bottom_json.empty())
            ? Slic3r::SurfacePassStack::from_json(bp->stack_bottom_json)
            : Slic3r::SurfacePassStack{};
        m_pro_bottom.enabled = m_pro_bottom.any_effect();
        m_pro_bottom_loaded_id = m_selected_profile_id;
    }

    // --- Color resultante en vivo (Top+Penu — el color ACTIVO de pintura; Bottom no
    // forma parte de la receta de color, ver NEOTKO_BOTTOM_TAG) ---
    // NEOTKO_SANDWICH_TAG — Fase 2 (s167): fondo real del objeto activo en vez de negro
    // hardcodeado (falls back to black cuando no se puede resolver).
    float out[3] = {0.f, 0.f, 0.f};
    float bg[3]  = {0.f, 0.f, 0.f};
    resolve_object_base_bg(mats, bg);
    CS::sandwich_colour_stacked(m_pro_top, m_pro_penu, mats, bg, ColorStitchPaintPreview::weave_layer_height(), out);

    // s318 — la CABECERA del Pro: las tres zonas apiladas como están en la pieza (Top, Penu, la
    // pieza, Bottom) y el Result grande con halo. Sustituye a las cajitas Recipe | Result de s230.
    // Mismos datos: out[] es el sandwich_colour_stacked de arriba; Bottom se ve atenuado porque NO
    // entra en el color (NEOTKO_BOTTOM_TAG).
    draw_pro_hero(m_pro_top, m_pro_penu, m_pro_bottom, out, fcolors, lh, -1.f, mats, bg);   // s350: + textura
    ImGui::Dummy(ImVec2(0.f, 0.3f * neo_u()));

    // s230 — las TRES zonas seguidas en un solo sitio (antes Bottom vivía tras un
    // discriminador Top/Bottom). Orden visual = orden físico de arriba abajo:
    // Top → Penultimate → Bottom.
    // s231 F4 — copia ZONA→ZONA. Reproducir a mano en Bottom lo que acabas de componer
    // en Top (pase a pase, tools, patrón, ángulo) es el gesto más repetido desde que
    // las tres zonas conviven en el mismo sitio (s230). `pro_copy_zone` reescribe el
    // payload ColorStitch con las claves de la zona DESTINO — copiar un pase de Top a
    // Penu sin traducir las claves lo dejaría sin patrón (el motor lo degradaría a
    // Solid, el mismo fallo que s118 documentó para los pases sin payload).
    auto do_copy = [&](const Slic3r::SurfacePassStack& src, bool src_penu,
                       Slic3r::SurfacePassStack* dst, bool dst_penu) {
        *dst = src;
        pro_retarget_cm(*dst, src_penu, dst_penu);
        // NEOTKO_BOTTOM_TAG — §5.5: el Bottom tiene reglas propias (máx 2 Solid,
        // máx 1 ColorStitch, PB SIEMPRE Full — un PB Half abajo dejaría una capa
        // vacía). Una copia no puede colar por la puerta de atrás lo que el editor
        // no deja autorar, así que se normaliza al aterrizar.
        if (dst == &m_pro_bottom) {
            int ns = 0, nc = 0;
            std::vector<Slic3r::SurfacePass> keep;
            for (Slic3r::SurfacePass& pp : dst->passes) {
                if (pp.kind == Slic3r::SurfacePassKind::PathBlend) {
                    PathBlendPassConfig pbc = pro_pb_read(pp);
                    if (pbc.mode == PathBlendPassConfig::Mode::Half) {
                        pbc.mode = PathBlendPassConfig::Mode::Full;
                        pro_pb_write(pp, pbc, lh);
                    }
                    keep.assign(1, pp);   // PB ocupa la zona entera
                    break;
                }
                if (pp.kind == Slic3r::SurfacePassKind::Solid    && ++ns > 2) continue;
                if (pp.kind == Slic3r::SurfacePassKind::ColorStitch && ++nc > 1) continue;
                keep.push_back(pp);
            }
            dst->passes = std::move(keep);
            // Re-normalizar ratios si se descartó algún pase por los caps.
            double tot = 0.0;
            for (const auto& pp : dst->passes) tot += std::max(0.0, pp.ratio);
            if (tot > 1e-6)
                for (auto& pp : dst->passes) pp.ratio = std::max(0.0, pp.ratio) / tot;
            else
                for (auto& pp : dst->passes) pp.ratio = 1.0 / double(dst->passes.size());
        }
        dst->enabled = dst->any_effect();
    };
    // s318 — cada zona pide su copia desde el glifo de su cabecera (menú con los dos destinos) y
    // aquí se ejecuta. Mismos destinos y mismo orden que las filas "copy to:" de s231.
    const std::string l_top = _u8L("Top"), l_penu = _u8L("Penultimate"), l_bot = _u8L("Bottom");
    int cp = -1;
    draw_zone_editor("##pro_top",  l_top.c_str(), m_pro_top,
                     /*allow_disable=*/false, /*penu=*/false, fcolors, nfil, lh, pro_row_w,
                     false, nullptr, nullptr, (l_penu + "##cp1").c_str(), (l_bot + "##cp2").c_str(), &cp, &m_tray);
    if (cp == 0) do_copy(m_pro_top, false, &m_pro_penu, true);
    else if (cp == 1) do_copy(m_pro_top, false, &m_pro_bottom, false);
    ImGui::Dummy(ImVec2(0.f, 0.45f * neo_u()));
    cp = -1;
    draw_zone_editor("##pro_penu", l_penu.c_str(), m_pro_penu,
                     /*allow_disable=*/true,  /*penu=*/true,  fcolors, nfil, lh, pro_row_w,
                     false, nullptr, nullptr, (l_top + "##cp3").c_str(), (l_bot + "##cp4").c_str(), &cp, &m_tray);
    if (cp == 0) do_copy(m_pro_penu, true, &m_pro_top, false);
    else if (cp == 1) do_copy(m_pro_penu, true, &m_pro_bottom, false);
    ImGui::Dummy(ImVec2(0.f, 0.45f * neo_u()));
    cp = -1;
    // Bottom zone — same authoring widget, §5.5 caps, "+ Add Bottom Paint" wording.
    draw_zone_editor("##pro_bottom", l_bot.c_str(), m_pro_bottom,
                     /*allow_disable=*/true,  /*penu=*/false, fcolors, nfil, lh, pro_row_w,
                     /*bottom_caps=*/true,
                     _u8L("+ Add Bottom Paint").c_str(),
                     _u8L("x Clear Bottom Paint").c_str(),
                     (l_top + "##cp5").c_str(), (l_penu + "##cp6").c_str(), &cp, &m_tray);
    if (cp == 0) do_copy(m_pro_bottom, false, &m_pro_top, false);
    else if (cp == 1) do_copy(m_pro_bottom, false, &m_pro_penu, true);

    // NEOTKO_PATHBLEND_TAG — s230: PathBlend no sobrevive a un puente real (su escalera
    // ramp/cap subdivide la altura de capa y sobre aire eso deja hilos de sección
    // ridícula). El motor lo DEGRADA a MultiPass con los mismos tools (Fill.cpp,
    // PB_BRIDGE_DEGRADE); aquí solo se avisa, en amarillo, y únicamente si la zona
    // Bottom tiene de verdad algún pase PathBlend autorado.
    // s230 — el aviso solo sale si thick_bridges está ACTIVO: es esa opción, no el hecho de
    // ser puente, la que rompe PathBlend (con thick_bridges=false el bridge es una lámina
    // normal y PathBlend funciona). Mismo gate que Fill.cpp (_pb_bridge_degrade); si divergen,
    // el painter avisaría de una degradación que no ocurre, o callaría una que sí.
    // Valor efectivo = override del objeto si existe, si no el del preset de print activo.
    {
        bool _has_pb = false;
        for (const Slic3r::SurfacePass& _p : m_pro_bottom.passes)
            if (_p.kind == Slic3r::SurfacePassKind::PathBlend) { _has_pb = true; break; }

        bool _thick = false;
        {
            const ModelObject* _mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
            const ConfigOptionBool* _tb = _mo
                ? dynamic_cast<const ConfigOptionBool*>(_mo->config.option("thick_bridges")) : nullptr;
            if (_tb) _thick = _tb->value;
            else if (const auto* _po = wxGetApp().preset_bundle->prints.get_edited_preset()
                                            .config.option<ConfigOptionBool>("thick_bridges"))
                _thick = _po->value;
        }

        if (_has_pb && _thick) {
            tray(_u8L("PathBlend won't work on Bridges, will use MultiPass with same colors"),
                 _u8L("thick_bridges is on for this object: over a real bridge the engine prints "
                      "PathBlend as MultiPass with the same tools."));
        }
    }

    // s230 — el checkbox "Supported bottom — control" se retiró y el clamp de pasadas
    // también: el Bottom usa la pila completa en todas partes, puentes incluidos. Lo que
    // queda es AVISAR, no prohibir — el painter no puede saber hasta el slice si una zona
    // acabará siendo puente, así que el texto tiene que llegar antes que el resultado.
    // s230 — el Bottom usa la pila completa también en puentes: se AVISA, no se prohíbe. s318: es
    // información (gris), no un problema, y va a la bandeja con el porqué entero en el tooltip.
    tray(_u8L("On bridges, tune before you stack"),
         _u8L("Bottom uses the full stack everywhere — supported faces and "
                                     "real bridges alike. Pass #1 always keeps bridge flow, speed "
                                     "and fan; passes above it print as controlled solid.\n\n"
                                     "Over open air the extrusion is shared between the passes, so "
                                     "the strand crossing the gap is thinner than a single-pass "
                                     "bridge would be. Bridges are calibration territory (speed, "
                                     "temperature, flow) — test a stacked bridge before trusting it "
                                     "on a real print.\n\n"
                                     "The fill angle is honoured on bridges too. The default bridge "
                                     "angle is planned to cross perpendicular to its anchors, so "
                                     "rotating it can leave line ends unsupported."), /*amber=*/false);

    // NEOTKO_COLORSTITCH_TAG — s118: Perimeter override ÚNICO para el color (no
    // per-zona). El motor lo lee por-zona (Fill.cpp mp_stack.perimeter_override), así
    // que un solo checkbox replica el flag a top y penu. Misma semántica que el
    // SandwichDialog tras unificarlo allí.
    {
        neo_section_g(_u8L("Options").c_str(), Glyph::Adv);
        bool perim = m_pro_top.perimeter_override || m_pro_penu.perimeter_override;
        if (neo_glyph_toggle_row("##perim", Glyph::Square, _u8L("Perimeter override").c_str(), &perim,
                                 _u8L("Clone the walls into every Solid pass "
                                      "(MultiPass perimeter override).").c_str())) {
            m_pro_top.perimeter_override  = perim;
            m_pro_penu.perimeter_override = perim;
        }
        // NEOTKO_MMU_COEXIST_TAG s234 — the walls are NOT clipped by the painted
        // footprint: cloning them paints perimeter loops that run all the way
        // around the region, so the effect shows up outside the area the user
        // actually painted (and across an MMU zone, which since s234 the fill
        // itself no longer touches). Nothing breaks — it just reads as confusing
        // in the preview, so the trade-off is stated where the switch lives.
        if (perim)
            tray(cs_strip_warn(_u8L("⚠ Will go beyond Normal Paint Areas")),
                 _u8L("The walls are NOT clipped by the painted footprint: the cloned perimeter "
                      "loops run all the way around the region."));
    }

    ImGui::Spacing();

    // s230 — la rejilla (TD) se RETIRÓ de Pro: vive ahora SOLO en el departamento
    // "Object & TD" (render_object_department), siempre visible, para liberar el
    // espacio que necesitaban las tres zonas unificadas + Recipe/Result arriba.
    // El bloque de Recipe/Result que estaba aquí se movió arriba (ver s230 allí).

    // --- El Pro ES el color activo (s118 binding live, puntos 4+6) ---
    // Sin botones "Use as paint colour"/"Save as palette": editar el Pro actualiza
    // el color seleccionado auto-mágicamente. Si hay un perfil enlazado
    // (m_selected_profile_id: auto recién pintado o guardado), se reescribe EN SITIO
    // → cambia el objeto y TODOS los que usen ese perfil (decisión usuario s118).
    // "Pin to palette" promueve el color de trabajo a la biblioteca guardada.
    // s118: garantizar payload de los pases ColorStitch antes de persistir (panel
    // plegado o pase cargado sin tocar) → el motor no los degrada a Solid.
    pro_backfill_cm(m_pro_top,  /*penu=*/false);
    pro_backfill_cm(m_pro_penu, /*penu=*/true);
    pro_backfill_cm(m_pro_bottom, /*penu=*/false);   // NEOTKO_BOTTOM_TAG — Fase 0 (WIP)

    // s350 — RECALCULAR aquí: `out` se calculó ARRIBA, antes de los mandos de este fotograma, y
    // guardaba el color de ANTES del cambio (preview_argb siempre un cambio por detrás: la vista 3D
    // y el panel de colores del sticker enseñaban el color viejo; el Pro no, porque recalcula).
    CS::sandwich_colour_stacked(m_pro_top, m_pro_penu, mats, bg, ColorStitchPaintPreview::weave_layer_height(), out);
    CS::ColorRecipe recipe;
    recipe.top  = m_pro_top;
    recipe.penu = m_pro_penu;                 // disabled/empty → to_json() == "" (sin penu)
    recipe.rgb  = { out[0], out[1], out[2] };
    recipe.desc = "Top " + zone_desc(recipe.top);
    if (m_pro_penu.enabled && !recipe.penu.passes.empty())
        recipe.desc += " / Penu " + zone_desc(recipe.penu);

    m_active_recipe     = recipe;
    m_has_active_recipe = true;
    if (m_active_style.empty()) m_active_style = _u8L("Custom");

    auto& mgr = SurfaceEffectProfileManager::get();
    if (m_selected_profile_id != 0) {
        if (SurfaceEffectProfile* p = mgr.find_mut(m_selected_profile_id)) {
            const std::string tj = recipe.top.to_json();
            const std::string pj = recipe.penu.to_json();
            // NEOTKO_BOTTOM_TAG — Fase 0 (WIP): persist the Bottom zone alongside top/penu.
            const std::string bj = m_pro_bottom.to_json();
            if (p->stack_top_json != tj || p->stack_penu_json != pj || p->stack_bottom_json != bj) {
                // Reescritura live por id (NO dedup-crear): conserva el id y el
                // nombre; refresca payload del motor desde los stacks editados.
                p->stack_top_json = tj;
                p->stack_penu_json = pj;
                p->stack_bottom_json = bj;   // NEOTKO_BOTTOM_TAG — Fase 0 (WIP)
                p->preview_argb    = recipe_argb(recipe);
                p->colorstitch  = {};   // limpiar payload viejo (payload_from_stacks solo añade)
                p->pathblend = {};
                SurfaceEffectProfileManager::payload_from_stacks(recipe.top, recipe.penu, *p);
                // NEOTKO_COLORSTITCH_TAG — s231 F3: refresh_selector_palettes reconstruye
                // el weave por ISLA (union-find sobre TODAS las facetas pintadas del
                // objeto). Hacerlo mientras se arrastra una ratio-bar o un DragFloat era
                // pagarlo entero en CADA frame del arrastre — la causa principal de que
                // el Pro se sintiera pesado. Durante el arrastre basta con el Result /
                // Recipe del panel (que se recalculan igual cada frame, son baratos); la
                // malla se pone al día al soltar.
                if (!ImGui::IsAnyItemActive()) {
                    refresh_selector_palettes();   // overlay + swatches al día en vivo
                    m_preview_dirty = true;
                }
                // NEOTKO_COLORSTITCH_TAG — editing a ColorStitch profile changes the saved
                // project content, so mark it unsaved (the re-slice alone didn't flag the doc
                // dirty → the Save/file option stayed inactive). No snapshot = no undo spam.
                wxGetApp().plater()->set_plater_dirty(true);
                // NEOTKO_COLORSTITCH_TAG — editar el contenido del perfil no toca las
                // facetas ni el mapeo de slots. Agendamos el background process; la
                // invalidación REAL la decide Print::apply, que recalcula la huella de
                // contenido del perfil desde el manager (model_colorstitch_paint_data_changed,
                // Model.cpp) → re-slice sin depender de este camino concreto.
                // s231 F3 — pero NO en mitad de un arrastre: el json cambia cada frame,
                // así que esto agendaba un re-slice por frame. Se acumula y se dispara
                // una sola vez al soltar (mismo criterio que el TD desde s167/s168, ver
                // el td_committed de render_td_grid).
                m_pro_reslice_pending = true;
                NEOTKO_LOG(PROFILE, "PRO live-rewrite RESLICE pending"
                    << " profile_id=" << p->id);
                NEOTKO_LOG(PROFILE, "PRO live-rewrite profile_id=" << p->id
                    << " name='" << p->name << "' top_empty=" << tj.empty()
                    << " penu_empty=" << pj.empty());
            }
        }
    }

    // s231 F3 — flush del re-slice diferido: en cuanto no queda ningún widget en
    // arrastre, se dispara UNA vez el trabajo pesado que se fue acumulando. Cubre por
    // igual el drag de la ratio-bar, los DragFloat/DragInt y el editor de perfil PB.
    if (m_pro_reslice_pending && !ImGui::IsAnyItemActive()) {
        m_pro_reslice_pending = false;
        refresh_selector_palettes();
        m_preview_dirty = true;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        NEOTKO_LOG(PROFILE, "PRO live-rewrite RESLICE posted SCHEDULE_BG (commit)");
    }

    // s169 F2 — el nombre editable + "Pin to palette" se movieron al header
    // persistente (render_header): mismo código, solo cambia dónde se dibuja
    // (visible en todos los departamentos, no solo en Pro).
}

// s169 F0 — rejilla (TD) por filamento, extraída tal cual de render_pro_mode_panel
// (mismo par td_changed/td_committed → save()+SCHEDULE_BACKGROUND_PROCESS de la
// Fase 1 de s167/s168, sin regresionar) para que Create/Object (F1/F3) la reusen
// bajo su propio wrapper. Self-contained: relee fcolors/nfil por su cuenta (mismo
// patrón de padding que render_palette_panel).
void GLGizmoColorStitchPainter::render_td_grid()
{
    // s318 — una FILA por filamento (ficha numerada + deslizador a todo el ancho con el valor dentro)
    // en vez de la rejilla 2×2 de cuadros sueltos. Mismo guardado: td_changed marca la caché de
    // paletas cada frame del arrastre; td_committed guarda a disco y re-slicea UNA vez, al soltar.
    std::vector<std::string> fcolors;
    if (auto* o = wxGetApp().preset_bundle->project_config
                      .option<ConfigOptionStrings>("filament_colour"))
        fcolors = o->values;
    const int nfil = std::max(1, std::min(4, (int)fcolors.size()));
    while (fcolors.size() < 4) fcolors.push_back("#808080");

    auto* ac = wxGetApp().app_config;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float u  = neo_u();
    const float fh = ImGui::GetFrameHeight();
    for (int t = 0; t < nfil; ++t) {
        ImGui::PushID(2000 + t);
        float td = 1.f;
        if (ac) {
            const std::string v = ac->get("neotko_td_" + std::to_string(t + 1));
            try { if (!v.empty()) td = std::stof(v); } catch (...) {}
        }
        {
            const float  sq  = 1.45f * u;
            const ImVec2 p   = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(sq, fh));
            const ImVec2 a(p.x, p.y + (fh - sq) * 0.5f), z(a.x + sq, a.y + sq);
            const ImU32  col = tool_col_u32(fcolors, t);
            dl->AddRectFilled(a, z, col, 4.f);
            char num[4];
            std::snprintf(num, sizeof(num), "%d", t + 1);
            const float  fs = 0.78f * u;
            const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
            dl->AddText(ImGui::GetFont(), fs, ImVec2((a.x + z.x - ns.x) * 0.5f, (a.y + z.y - ns.y) * 0.5f), ink_on(col), num);
        }
        ImGui::SameLine(0.f, 0.5f * u);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        const bool td_changed = ImGui::SliderFloat("##td", &td, 0.01f, 10.f, "%.2f");
        // NEOTKO_SANDWICH_TAG — Fase 1 (s167 plan): IsItemDeactivatedAfterEdit
        // fires once, on mouse release — save()+reslice here instead of inside
        // the td_changed block above (that fires every dragged frame; hitting
        // disk/scheduling a background process per-frame would be wasteful).
        const bool td_committed = ImGui::IsItemDeactivatedAfterEdit();
        if (td_changed) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.3f", td);
            if (ac) ac->set("neotko_td_" + std::to_string(t + 1), buf);
            m_pal_key.clear();   // fuerza rebuild_palettes_if_stale en el panel de paletas
        }
        if (td_committed) {
            // AppConfig::set() only marks the in-memory store dirty (never
            // writes to disk on its own, see AppConfig.hpp) — save() here so
            // the value survives a non-clean app exit, not just clean shutdown.
            if (ac) ac->save();
            // s231 F5 — el color de la malla ya se DERIVA del TD (ver
            // build_ebt_colors_for_volume), así que al soltar el slider hay que
            // repintar el overlay o la mancha del modelo se quedaría con el TD viejo.
            refresh_selector_palettes();
            m_preview_dirty = true;
            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Transmission distance").c_str());
        ImGui::PopID();
    }

#ifdef NEOTKO_PRIVATE_TOWER
    // 23-oct-2026 — las recetas por profundidad se resuelven con los TD de ese momento y se guardan en el 3mf; al
    // cambiar un TD no se rehacen solas. Este botón las rehace todas con los TD de ahora.
    if (ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        mo && std::any_of(mo->colorstitch_stickers.begin(), mo->colorstitch_stickers.end(),
                          [](const ColorStitchSticker& st) { return st.depth_mode; })) {
        if (neo_text_button((_u8L("Recompute sticker TD") + "##td_recompute").c_str(), NeoBtn::Normal)) {
            Plater::TakeSnapshot snap(wxGetApp().plater(), _u8L("Recompute sticker TD"), UndoRedo::SnapshotType::GizmoAction);
            Slic3r::ColorSci::Material mats[4];
            std::vector<std::string> fc;
            gizmo_materials(mats, fc);
            float bg[3] = { 1.f, 1.f, 1.f };
            resolve_object_base_bg(mats, bg);
            for (ColorStitchSticker& st : mo->colorstitch_stickers)
                if (st.depth_mode) {
                    recompute_depth_recipes(st, mats, int(fc.size()), bg, GLGizmoColorStitchPainter_layer_height());
                    sticker_colours_changed(st);
                }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Solve the Depth recipe of every sticker again\n"
                                         "with the TD and filament colours set now.\n"
                                         "Use it after changing a TD or a filament.").c_str());
    }
#endif
}

// preview_argb (0xAARRGGBB) desde el rgb predicho de una receta.
static uint32_t recipe_argb(const Slic3r::ColorSci::ColorRecipe& r)
{
    return 0xFF000000u
         | ((uint32_t)std::min(255.f, r.rgb[0] * 255.f) << 16)
         | ((uint32_t)std::min(255.f, r.rgb[1] * 255.f) <<  8)
         |  (uint32_t)std::min(255.f, r.rgb[2] * 255.f);
}

static std::string recipe_name(const Slic3r::ColorSci::ColorRecipe& r, const std::string& style)
{
    std::string name = style + " " + zone_desc(r.top);
    if (!r.penu.passes.empty()) name += "/" + zone_desc(r.penu);
    return name;
}

// NEOTKO_COLORSTITCH_TAG — s118 (binding live por id): reconstruye un ColorRecipe
// desde un perfil guardado/auto, para cargarlo en los editores Pro (m_pro_*) y que
// el panel refleje EXACTAMENTE lo que tiene el perfil. El rgb se toma del
// preview_argb cacheado (el swatch lo re-predice en vivo igualmente).
static Slic3r::ColorSci::ColorRecipe recipe_from_profile(const Slic3r::SurfaceEffectProfile& p)
{
    Slic3r::ColorSci::ColorRecipe r;
    r.top  = Slic3r::SurfacePassStack::from_json(p.stack_top_json);
    r.penu = Slic3r::SurfacePassStack::from_json(p.stack_penu_json);
    const uint32_t a = p.preview_argb;
    r.rgb = { ((a >> 16) & 0xFF) / 255.f,
              ((a >>  8) & 0xFF) / 255.f,
              ( a        & 0xFF) / 255.f };
    r.desc = p.name;
    return r;
}

// NEOTKO_COLORSTITCH_TAG — s231 F0: punto ÚNICO de invalidación del estado activo.
// Ver la nota larga en el .hpp: los 4 campos del "color activo" se sincronizaban a
// mano en 8 sitios y cada uno olvidaba uno distinto (bug s209). Cualquier camino que
// deje obsoleto el enlace debe pasar por aquí, no resetear campos sueltos.
void GLGizmoColorStitchPainter::invalidate_active_binding(bool keep_recipe)
{
    m_selected_profile_id = 0;
    m_active_slot         = 0;
    // La clave del bug: SIEMPRE a false. Con resolved=true y slot=0, ensure_active_slot
    // devuelve 0 sin intentar nada y el pincel queda mudo pero con aspecto de listo.
    m_active_resolved     = false;
    if (!keep_recipe) {
        m_has_active_recipe = false;
        m_active_recipe     = {};
        m_active_pal_kind   = 0;
        m_active_pal_idx    = -1;
    }
    // NOTA: el stack Bottom NO se toca aquí a propósito. La recarga ya la gobierna la
    // comparación `m_pro_bottom_loaded_id != m_selected_profile_id` del panel Pro, que
    // dispara cuando el enlace pasa a OTRO perfil. Forzar la relectura desde aquí
    // borraría el Bottom que se esté componiendo sobre un color de trabajo todavía sin
    // perfil (pid ya era 0): perder trabajo del usuario por una invalidación de
    // bookkeeping sería peor que el estado obsoleto que se pretendía evitar.
}

// Click en swatch: SOLO fija el color activo. No crea profile ni asigna slot —
// navegar la paleta no contamina nada. El slot se materializa al pintar.
void GLGizmoColorStitchPainter::set_active_recipe(
        const Slic3r::ColorSci::ColorRecipe& r, const std::string& style)
{
    m_active_recipe     = r;
    // NEOTKO_COLORSTITCH_TAG — s118: normalizar enabled desde passes. Las recetas
    // predict (Mixed/Flat con penu) pueden venir con penu.enabled=false aunque
    // tengan passes; ensure_active_slot serializa m_active_recipe.to_json() y to_json()
    // corta a "" si !enabled → stack_penu_json vacío → el motor SUPRIME el penu y el
    // main editor lo muestra desactivado (bug "colormix penu no slicea"). load_recipe_
    // into_pro ya lo hacía para m_pro_*, faltaba para m_active_recipe.
    m_active_recipe.top.enabled  = !m_active_recipe.top.passes.empty();
    m_active_recipe.penu.enabled = !m_active_recipe.penu.passes.empty();
    m_active_style      = style;
    // NEOTKO_COLORSTITCH_TAG — s118: un swatch de las tiras predict (Mixed/Gradient/
    // Flat) es un color de trabajo NUEVO, aún SIN enlazar a un perfil. Desenlazar
    // aquí evita que el write-back live del Pro reescriba el perfil que estuviera
    // enlazado antes; se materializa/enlaza por dedup al primer trazo (ensure_active_slot).
    // s231 F0: vía el invalidador único (antes reseteaba 3 campos a mano y se olvidaba
    // de dejar el estado coherente para ensure_active_slot).
    invalidate_active_binding(/*keep_recipe=*/true);
    m_has_active_recipe = true;
    // s231 F5 — por defecto el color activo NO viene de una tira generada; los tres
    // callers del Generator vuelven a marcarlo justo después de esta llamada. Así un
    // "+ New" o una receta de otro origen no deja encendido un swatch que ya no es.
    m_active_pal_kind   = 0;
    m_active_pal_idx    = -1;
    // s111 — elegir un color implica querer pintar: salir de Select a modo bucket.
    set_tool(TOOL_PAINT);
    // s112 — Pro mode dual: que el panel Pro refleje el color recién elegido.
    load_recipe_into_pro(r);
}

// s112 — vuelca una receta a los editores de zona Pro. El Result se recalcula
// solo cada frame en render_pro_mode_panel (sandwich_colour_stacked), así que
// basta con fijar los stacks + marcar sembrado.
void GLGizmoColorStitchPainter::load_recipe_into_pro(const Slic3r::ColorSci::ColorRecipe& r)
{
    m_pro_top    = r.top;
    m_pro_penu   = r.penu;
    // NEOTKO_SANDWICH_TAG s119 (EMPTY model): "enabled" derives from whether the
    // zone carries a real effect, NOT merely from having passes. A [None] zone
    // (explicit Empty passthrough, e.g. authored in the SandwichDialog) has one
    // pass but no effect → it must load DISABLED, or the painter would render an
    // enabled zone whose kind the row selector cannot show.
    m_pro_top.enabled  = m_pro_top.any_effect();
    m_pro_penu.enabled = m_pro_penu.any_effect();
    // NEOTKO_BOTTOM_TAG — s232, RAÍZ del "bottom espejismo": esta función cargaba
    // top y penu y NO TOCABA `m_pro_bottom`, así que al elegir otro color el Pro se
    // quedaba con el bottom del perfil ANTERIOR. Consecuencias observadas: un patrón
    // recién sacado del Generator aparecía ya con bottom "automático" (nunca se
    // decidió que fuera así), el eyedropper devolvía un bottom que era de OTRO objeto,
    // y al cambiar el bottom a un pase sólido la cara inferior se dibujaba con ese
    // color plano (de ahí el "se pinta el patrón y al soltar queda gris": durante el
    // arrastre las caras nuevas caen al tejido por slot, que es el del TOP, y al
    // soltar el rebuild por isla les da la receta bottom que el perfil sí tenía).
    // `ColorRecipe` no lleva bottom, así que aquí se VACÍA y se invalida el id de
    // carga: el propio panel Pro lo re-siembra desde `stack_bottom_json` del perfil
    // seleccionado en su siguiente frame (única fuente de verdad del bottom).
    m_pro_bottom = Slic3r::SurfacePassStack{};
    m_pro_bottom_loaded_id = -1;
    m_pro_seeded = true;   // no re-sembrar el Solid default encima
}

// Materializa el color activo como slot pintable la primera vez que se pinta.
// Dedup por (top_json,penu_json): si ya existe un profile (auto o guardado) con
// esos blobs se reusa; si no, se crea uno AUTO. Idempotente vía m_active_resolved.
int GLGizmoColorStitchPainter::ensure_active_slot()
{
    if (!m_has_active_recipe) return m_active_slot;
    if (m_active_resolved)    return m_active_slot;

    // s118: backfill de payloads ColorStitch también en el camino de materialización
    // directa (predict swatch pintado sin abrir el Pro) → nunca se persiste un
    // ColorStitch vacío que el motor degradaría a Solid.
    pro_backfill_cm(m_active_recipe.top,  /*penu=*/false);
    pro_backfill_cm(m_active_recipe.penu, /*penu=*/true);

    const std::string top_json  = m_active_recipe.top.to_json();
    const std::string penu_json = m_active_recipe.penu.to_json();
    // NEOTKO_BOTTOM_TAG — s232: el BOTTOM también entra aquí. `ColorRecipe` sólo lleva
    // top+penu, así que un color de trabajo se materializaba SIN `stack_bottom_json`
    // aunque el Pro tuviera la zona Bottom con efecto — y entonces
    // `slot_wants_bottom` era false y `discard_non_zone_facing` borraba las caras
    // inferiores al soltar el botón (pintaba y se quedaba gris). El bottom entra
    // además en el DEDUP: dos colores con el mismo top/penu y distinto bottom son
    // colores distintos, y compartir el perfil le robaría el bottom a uno de los dos.
    // ⚠️ s232 — SÓLO si ese bottom es de ESTE enlace. `m_pro_bottom` es un editor
    // persistente y su dueño lo marca `m_pro_bottom_loaded_id`; sin esta condición se
    // heredaba el bottom del perfil anterior (el bug del "bottom espejismo", arreglado
    // en load_recipe_into_pro) y se PERSISTÍA en cada color nuevo.
    const bool bottom_is_ours = (m_pro_bottom_loaded_id == m_selected_profile_id);
    if (bottom_is_ours) pro_backfill_cm(m_pro_bottom, /*penu=*/false);
    const std::string bottom_json = (bottom_is_ours && m_pro_bottom.any_effect())
        ? m_pro_bottom.to_json() : std::string();

    auto& mgr = SurfaceEffectProfileManager::get();
    int id = 0;
    for (const SurfaceEffectProfile& p : mgr.list())
        if (p.stack_top_json == top_json && p.stack_penu_json == penu_json
            && p.stack_bottom_json == bottom_json) { id = p.id; break; }

    // s232 DEBUG — "¿qué pasa exactamente cuando hago click?": la materialización es el
    // punto donde el color de trabajo se convierte en perfil+slot, y donde se decidía
    // (mal) qué bottom se lleva. Incondicional, canal BOTTOM.
    {
        std::ostringstream os;
        os << "CLICK_MATERIALIZE dedup_hit=" << id
           << " top_len=" << top_json.size() << " penu_len=" << penu_json.size()
           << " bottom_len=" << bottom_json.size()
           << " bottom_is_ours=" << (bottom_is_ours ? 1 : 0)
           << " pro_bottom_loaded_id=" << m_pro_bottom_loaded_id
           << " selected_pid=" << m_selected_profile_id
           << " pro_bottom_effect=" << (m_pro_bottom.any_effect() ? 1 : 0);
        NeoDebug::write(NeoDebug::BOTTOM, os.str());
    }

    if (id == 0) {
        SurfaceEffectProfile p;
        p.name              = recipe_name(m_active_recipe, m_active_style);
        p.stack_top_json    = top_json;
        p.stack_penu_json   = penu_json;
        p.stack_bottom_json = bottom_json;
        p.preview_argb      = recipe_argb(m_active_recipe);
        p.auto_generated  = true;       // capa "auto" — oculta de la lista + GC-able
        // NEOTKO_COLORSTITCH_TAG — s112 fix: derive the engine payload from the
        // recipe stacks so the painted surface actually slices (the gate reads
        // p.colorstitch.present, not the visual stack json).
        SurfaceEffectProfileManager::payload_from_stacks(
            m_active_recipe.top, m_active_recipe.penu, p);
        id = mgr.add(std::move(p));
    } else if (SurfaceEffectProfile* ex = mgr.find_mut(id);
               ex && !ex->colorstitch.present && !ex->pathblend.present) {
        // Reused a pre-s112 auto profile that only has the visual stacks → backfill
        // the payload in place so it slices too.
        SurfaceEffectProfileManager::payload_from_stacks(
            m_active_recipe.top, m_active_recipe.penu, *ex);
    }

    m_selected_profile_id = id;
    m_active_slot         = slot_for_selected_profile(/*assign_if_missing=*/true);
    // Si todos los slots están llenos, slot=0: deja resolved=false para reintentar
    // cuando se libere uno (el "slots full" warning ya avisa). Sin duplicar: el
    // dedup de arriba reencuentra este profile en el siguiente intento.
    m_active_resolved     = (m_active_slot >= 1 && m_active_slot < MAX_SLOTS);
    refresh_selector_palettes();
    return m_active_slot;
}

// Borra profiles AUTO que ningún slot de ningún volumen referencia. Las paletas
// guardadas (auto_generated=false) nunca se tocan. Limpia también punteros de
// slot que apuntasen a ids ya borrados.
void GLGizmoColorStitchPainter::garbage_collect_auto_profiles()
{
    // NEOTKO_COLORSTITCH_TAG — s139: el set de referencias debe cubrir TODO el
    // proyecto, no solo el objeto activo. Antes barría sólo
    // selection_info()->model_object() → al pintar A, cambiar a B y disparar gc
    // (on_shutdown / Remove all) se borraba el auto-profile de A aunque su slot lo
    // siguiera referenciando → slot→pid colgando = caja gris fantasma. Cierra de
    // RAÍZ el caso (b) documentado en ColorStitch.cpp (s138 lo hizo inerte al
    // slicear; aquí evitamos crearlo). El gc nunca borra de más: sólo deja de
    // borrar autos referenciados por objetos no-activos.
    const Model* model = m_parent.get_selection().get_model();
    if (!model) return;

    std::set<int> referenced;
    for (const ModelObject* mo : model->objects) {
        if (!mo) continue;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            for (int s = 1; s < MAX_SLOTS; ++s)
                if (mv->colorstitch_slot_to_profile_id[s] != 0)
                    referenced.insert(mv->colorstitch_slot_to_profile_id[s]);
        }
        // s348 — los stickers también referencian perfiles (y uno por color si son multicolor).
        for (const ColorStitchSticker& st : mo->colorstitch_stickers) {
            if (st.profile_id) referenced.insert(st.profile_id);
            for (const ColorStitchStickerColor& c : st.colors) {
                if (c.profile_id) referenced.insert(c.profile_id);
                for (const int bp : c.band_profiles) if (bp) referenced.insert(bp);   // s349 — bandas
            }
        }
    }

    auto& mgr = SurfaceEffectProfileManager::get();
    std::vector<int> to_remove;
    for (const SurfaceEffectProfile& p : mgr.list())
        if (p.auto_generated && referenced.find(p.id) == referenced.end())
            to_remove.push_back(p.id);

    for (int id : to_remove) {
        mgr.remove(id);
        if (m_selected_profile_id == id) { m_selected_profile_id = 0; m_active_slot = 0; }
    }
    if (!to_remove.empty()) {
        NEOTKO_LOG(PROFILE, "PAINT gc removed " << to_remove.size() << " auto profiles");
        refresh_selector_palettes();
    }
}

// Promueve el color activo a paleta GUARDADA: si ya hay un profile con esos
// blobs, le quita el flag auto (aparece en la lista, deja de ser GC-able);
// si no, crea uno guardado. Útil para "guardar la paleta" sin que la lista
// se llene sola.
void GLGizmoColorStitchPainter::save_active_as_palette()
{
    if (!m_has_active_recipe) return;
    const std::string top_json  = m_active_recipe.top.to_json();
    const std::string penu_json = m_active_recipe.penu.to_json();
    // NEOTKO_BOTTOM_TAG — s232: "Save" IGNORABA el bottom. `ColorRecipe` sólo lleva
    // top+penu, así que guardar una receta con Bottom creaba un perfil SIN
    // `stack_bottom_json`: el bottom se perdía en silencio justo en el gesto con el que
    // el usuario cree estar conservando su trabajo. Misma regla de propiedad que
    // ensure_active_slot: el editor sólo viaja si está cargado para ESTE enlace.
    const std::string bottom_json =
        (m_pro_bottom_loaded_id == m_selected_profile_id && m_pro_bottom.any_effect())
            ? m_pro_bottom.to_json() : std::string();

    auto& mgr = SurfaceEffectProfileManager::get();
    for (const SurfaceEffectProfile& p : mgr.list())
        // El DEDUP también mira el bottom: dos recetas con el mismo top/penu y distinto
        // bottom son colores distintos, y quedarse con la primera es cómo se acaba
        // teniendo dos perfiles gemelos ('ColorStitch CM/CM' pid 40 y 45) de los que uno
        // lleva el bottom y el otro no.
        if (p.stack_top_json == top_json && p.stack_penu_json == penu_json
            && p.stack_bottom_json == bottom_json) {
            if (SurfaceEffectProfile* mp = mgr.find_mut(p.id)) {
                // NEOTKO_COLORSTITCH_TAG — s137: promoting a working colour (auto) to
                // the library files it into the active group. An already-saved profile
                // keeps its group (re-pinning must not silently move it).
                if (mp->auto_generated)
                    mp->name = cs_with_group(mp->name, m_active_group);
                mp->auto_generated = false;
                // s232 — …pero si ese perfil vive en OTRO grupo, la rejilla lo filtra y
                // "Save" parecía no hacer nada (síntoma reportado: "doy guardar y no se
                // añade a la vista"). En vez de moverlo de grupo (eso sí sería silencioso
                // y destructivo), se salta la vista a SU grupo: el color aparece donde
                // está de verdad.
                const int g = cs_parse_group(mp->name);
                if (g != m_active_group) {
                    m_active_group   = g;
                    m_max_group_hint = std::max(m_max_group_hint, g);
                }
                // NEOTKO_COLORSTITCH_TAG — s112: backfill payload if missing.
                if (!mp->colorstitch.present && !mp->pathblend.present)
                    SurfaceEffectProfileManager::payload_from_stacks(
                        m_active_recipe.top, m_active_recipe.penu, *mp);
            }
            m_selected_profile_id = p.id;
            return;
        }

    SurfaceEffectProfile p;
    // NEOTKO_COLORSTITCH_TAG — s137: file the new palette into the active group.
    p.name              = cs_with_group(recipe_name(m_active_recipe, m_active_style), m_active_group);
    p.stack_top_json    = top_json;
    p.stack_penu_json   = penu_json;
    p.stack_bottom_json = bottom_json;   // s232 — el bottom viaja con la receta
    p.preview_argb      = recipe_argb(m_active_recipe);
    p.auto_generated    = false;
    // NEOTKO_COLORSTITCH_TAG — s112 fix: payload so the saved palette slices.
    SurfaceEffectProfileManager::payload_from_stacks(
        m_active_recipe.top, m_active_recipe.penu, p);
    m_selected_profile_id = mgr.add(std::move(p));
}

// NEOTKO_COLORSTITCH_TAG — s231 F4: DUPLICAR un perfil de la biblioteca. Clona los
// tres stacks + el payload del motor en un perfil GUARDADO nuevo del grupo activo.
// Devuelve el id nuevo (0 si el original no existe). No toca el original ni los slots:
// duplicar es una operación de biblioteca, no de pintura — el nuevo color no ocupa
// slot hasta que se pinta con él, igual que cualquier otro.
int GLGizmoColorStitchPainter::duplicate_profile(int pid)
{
    auto& mgr = SurfaceEffectProfileManager::get();
    const SurfaceEffectProfile* src = mgr.find(pid);
    if (!src) return 0;

    SurfaceEffectProfile p = *src;          // copia: stacks + payload + preview_argb
    p.id             = 0;                   // el manager asigna uno nuevo en add()
    p.auto_generated = false;               // una copia explícita nace GUARDADA
    // El sufijo de grupo va SIEMPRE al final del nombre (cs_with_group), así que hay
    // que componer sobre el nombre limpio o el " copy" quedaría detrás del marcador.
    p.name = cs_with_group(cs_strip_group(src->name) + " " + _u8L("copy"), m_active_group);
    const int new_id = mgr.add(std::move(p));
    NEOTKO_LOG(PROFILE, "PALETTE duplicate src=" << pid << " → new=" << new_id
        << " group=" << m_active_group);
    return new_id;
}

// NEOTKO_COLORSTITCH_TAG — s231 F4: duplicar el COLOR ACTIVO. Éste es el que arregla
// el agujero de verdad: como el Pro reescribe EN VIVO el perfil enlazado (s118),
// "cojo este color guardado y lo varío un poco" destruía el original para todos los
// objetos que lo usaran, y la única forma de derivar sin romper era un rodeo que nadie
// descubre solo (tocar un swatch del Generator para desenlazar y volver a Pro).
// Ahora: copia → enlazada → Pro. Lo que edites cae sobre la copia.
void GLGizmoColorStitchPainter::duplicate_active_as_new()
{
    auto& mgr = SurfaceEffectProfileManager::get();

    // Si hay perfil enlazado, la copia sale de ÉL (conserva el bottom y el payload
    // exacto). Si el color activo es de trabajo (tira generada / Pro sin guardar), se
    // construye desde la receta activa — mismo camino que save_active_as_palette.
    int new_id = 0;
    if (m_selected_profile_id != 0) {
        new_id = duplicate_profile(m_selected_profile_id);
    } else if (m_has_active_recipe) {
        pro_backfill_cm(m_active_recipe.top,  /*penu=*/false);
        pro_backfill_cm(m_active_recipe.penu, /*penu=*/true);
        SurfaceEffectProfile p;
        p.name = cs_with_group(recipe_name(m_active_recipe, m_active_style) + " "
                               + _u8L("copy"), m_active_group);
        p.stack_top_json    = m_active_recipe.top.to_json();
        p.stack_penu_json   = m_active_recipe.penu.to_json();
        // s232 — igual que en ensure_active_slot: el bottom sólo viaja si el editor
        // está cargado para ESTE enlace (si no, se clonaba el bottom de otro perfil).
        p.stack_bottom_json = (m_pro_bottom_loaded_id == m_selected_profile_id)
                                  ? m_pro_bottom.to_json() : std::string();
        p.preview_argb      = recipe_argb(m_active_recipe);
        p.auto_generated    = false;
        SurfaceEffectProfileManager::payload_from_stacks(
            m_active_recipe.top, m_active_recipe.penu, p);
        new_id = mgr.add(std::move(p));
    }
    if (new_id == 0) return;

    // Enlazar la copia y abrirla en Pro. El slot NO se asigna aquí: se materializa al
    // primer trazo, como cualquier color (duplicar no debe quemar un slot del objeto).
    if (const SurfaceEffectProfile* np = mgr.find(new_id)) {
        const Slic3r::ColorSci::ColorRecipe r = recipe_from_profile(*np);
        invalidate_active_binding(/*keep_recipe=*/true);
        m_selected_profile_id = new_id;
        m_active_recipe       = r;
        m_active_style        = cs_strip_group(np->name);
        m_has_active_recipe   = true;
        load_recipe_into_pro(r);
        set_tool(TOOL_PAINT);
        m_department = DEP_PRO;      // Pro: la copia se crea para editarla
        refresh_selector_palettes();
    }
}

// NEOTKO_COLORSTITCH_TAG — s140: ¿queda algún color de trabajo sin guardar?
// "Sin guardar" = profile auto_generated (efímero, oculto de la lista, GC-able).
bool GLGizmoColorStitchPainter::has_unsaved_palettes() const
{
    for (const SurfaceEffectProfile& p : SurfaceEffectProfileManager::get().list())
        if (p.auto_generated) return true;
    return false;
}

// NEOTKO_COLORSTITCH_TAG — s140: SAVE ALL. Promueve TODOS los auto_generated a
// paleta guardada (auto_generated=false) filándolos en el grupo activo, igual que
// save_active_as_palette hace con uno solo. Tras esto no queda nada efímero, así
// "Remove all" puede borrar sin perder trabajo. No crea ni borra profiles: solo
// cambia el flag (+ nombre de grupo) en sitio. Devuelve cuántos promovió.
int GLGizmoColorStitchPainter::save_all_palettes()
{
    auto& mgr = SurfaceEffectProfileManager::get();
    // Snapshot de ids: promover muta flags, no el tamaño de la lista, pero iteramos
    // sobre una copia por seguridad ante reordenaciones internas del manager.
    std::vector<int> ids;
    for (const SurfaceEffectProfile& p : mgr.list())
        if (p.auto_generated) ids.push_back(p.id);

    for (int id : ids) {
        SurfaceEffectProfile* mp = mgr.find_mut(id);
        if (!mp) continue;
        mp->name           = cs_with_group(mp->name, m_active_group);
        mp->auto_generated = false;
        // Backfill defensivo del payload (perfiles pre-s112 que solo traían el
        // stack visual): reconstruir desde los blobs json para que sliceen.
        if (!mp->colorstitch.present && !mp->pathblend.present) {
            SurfacePassStack t  = SurfacePassStack::from_json(mp->stack_top_json);
            SurfacePassStack pe = SurfacePassStack::from_json(mp->stack_penu_json);
            SurfaceEffectProfileManager::payload_from_stacks(t, pe, *mp);
        }
    }
    if (!ids.empty()) {
        NEOTKO_LOG(PROFILE, "PAINT save_all promoted " << ids.size() << " profiles to group " << m_active_group);
        refresh_selector_palettes();
    }
    return (int)ids.size();
}
// NEOTKO_COLORSTITCH_TAG_END

// ----------------------------------------------------------------------------
// Sidebar UI
// ----------------------------------------------------------------------------

void GLGizmoColorStitchPainter::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 15.f;

    const float  scale       = m_parent.get_scale();
    const ImVec2 button_size(25 * scale, 25 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString& caption, const wxString& text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };
        for (const auto& t : { "paint", "erase", "smart_fill_angle", "clipping_of_view" })
            draw_text_with_caption(m_desc.at(std::string(t) + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

// s318 — PESTAÑAS de departamento: glifo arriba, palabra abajo, la activa sobre Surface con un
// subrayado AccentBright. Sustituye a cs_segmented_bar (s174), que era sólo texto sobre un teal
// sólido. Mismo contrato: `active` se actualiza en sitio y cada pestaña lleva su tooltip.
static void cs_department_tabs(const char* const* labels, const char* const* tips, const Glyph* glyphs,
                               int n, int& active)
{
    const float  u     = neo_u();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  pad   = 3.f;
    const float  h     = 2.55f * u;
    const float  seg_w = (avail - 2.f * pad - float(n - 1) * pad) / float(n);
    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const ImVec2 p0    = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + h + 2.f * pad), neo_col_u32(NeoCol::Canvas), 6.f);
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const ImVec2 a(p0.x + pad + float(i) * (seg_w + pad), p0.y + pad);
        const ImVec2 b(a.x + seg_w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##tab", ImVec2(seg_w, h))) active = i;
        const bool hov = ImGui::IsItemHovered();
        const bool on  = (i == active);
        if (on || hov)
            dl->AddRectFilled(a, b, neo_col_u32(NeoCol::Surface), 4.f);
        const float gs = 1.2f * u;
        draw_glyph(dl, ImVec2(a.x + (seg_w - gs) * 0.5f, a.y + 0.28f * u), gs, glyphs[i],
                   on ? neo_col_u32(NeoCol::AccentBright) : neo_col_u32(hov ? NeoCol::Ink : NeoCol::TextDim));
        const float fs = 0.8f * u;
        const std::string lbl = fit_text(labels[i], (seg_w - 0.4f * u) / 0.8f);
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, lbl.c_str());
        dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + (seg_w - ts.x) * 0.5f, a.y + 0.28f * u + gs + 0.12f * u),
                    neo_col_u32((on || hov) ? NeoCol::Ink : NeoCol::TextDim), lbl.c_str());
        if (on)
            dl->AddRectFilled(ImVec2(a.x + seg_w * 0.18f, b.y - 2.f), ImVec2(b.x - seg_w * 0.18f, b.y),
                              neo_col_u32(NeoCol::AccentBright), 1.f);
        if (hov && tips[i] && *tips[i]) ImGui::SetTooltip("%s", tips[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 2.f * pad));
    ImGui::Dummy(ImVec2(avail, 0.f));
}

// s318 — la columna de etiquetas de ESTE panel, medida sobre sus textos traducidos (GizmoNeotkoStyle
// neo_label_col_for). neo_row_slider la pide al pintar cada fila.
static float cs_label_col()
{
    return neo_label_col_for({ _u8L("Smart fill angle"), _u8L("Section view"), _u8L("Rotate"), _u8L("Scale"), _u8L("Size") });
}

// s318 — el "⚠ " con que empiezan algunas cadenas traducidas sobra en la bandeja: allí el triángulo
// ya lo pinta neo_warn_row. Sólo se quita si va al principio; la cadena (y su traducción) no cambia.
static std::string cs_strip_warn(std::string s)
{
    static const char k[] = "\xE2\x9A\xA0";   // U+26A0
    if (s.compare(0, 3, k) == 0) {
        s.erase(0, 3);
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    }
    return s;
}

// s111 — fila de herramientas (toggles mutuamente excluyentes, set_tool es el único dueño de la
// exclusión desde s231 F6). s318: cinco glifos vectoriales dentro de un pozo Canvas, en vez de los
// SVG de s173 + un "Sticker" de texto que no tenía icono. Deja el cursor DEBAJO del pozo; quien
// quiera seguir en la misma fila (el ? y Erase all) hace su propio SameLine().
void GLGizmoColorStitchPainter::render_tool_row()
{
    const float  u   = neo_u();
    const float  bs  = 2.1f * u;
    const float  pad = 3.f, gap = 3.f;
    ImDrawList*  dl  = ImGui::GetWindowDrawList();
    const ImVec2 p0  = ImGui::GetCursorScreenPos();
    const float  w   = 5.f * bs + 4.f * gap + 2.f * pad;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + bs + 2.f * pad), neo_col_u32(NeoCol::Canvas), 6.f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));

    if (neo_glyph_toggle("##t_select", bs, m_select_mode, Glyph::Select,
                         _u8L("Select objects to paint — click them in the scene "
                              "(Shift-click to unmark)").c_str()))
        set_tool_mode(/*select=*/true, /*erase=*/false);
    ImGui::SameLine(0.f, gap);
    // NEOTKO_STICKER_TAG — Paint es el catch-all "nada más activo" (bug de s170: se veía
    // encendido a la vez que Sticker).
    if (neo_glyph_toggle("##t_paint", bs, !m_select_mode && !m_erase_mode && !m_pick_mode && !m_sticker_mode,
                         Glyph::Paint, _u8L("Paint (smart fill)").c_str()))
        set_tool_mode(/*select=*/false, /*erase=*/false);
    ImGui::SameLine(0.f, gap);
    if (neo_glyph_toggle("##t_erase", bs, !m_select_mode && m_erase_mode, Glyph::Erase,
                         _u8L("Eraser — smart-fill removes paint").c_str()))
        set_tool_mode(/*select=*/false, /*erase=*/true);
    ImGui::SameLine(0.f, gap);
    // NEOTKO_COLORSTITCH_TAG — s118: eyedropper.
    if (neo_glyph_toggle("##t_pick", bs, m_pick_mode, Glyph::Pick,
                         _u8L("Eyedropper — click a painted object to load its colour "
                              "(and dump what it has painted / its base).").c_str()))
        set_tool(TOOL_PICK);
    ImGui::SameLine(0.f, gap);
    if (neo_glyph_toggle("##t_sticker", bs, m_sticker_mode, Glyph::Sticker,
                         _u8L("Sticker — load an SVG with the button that appears\n"
                              "next to it, then click the object to place it").c_str())) {
        set_tool(TOOL_STICKER);
        m_department = DEP_STICKERS;   // s348 — la herramienta vive en su pestaña
    }
    // s350 — «Load SVG…» al lado de la herramienta Sticker, sólo con ella activa (antes estaba
    // abajo, en la pestaña: así esa zona queda corta y cabe el editor del sticker, s364).
    if (m_sticker_mode) {
        ImGui::SameLine(0.f, pad + 0.6f * u);   // fuera del marco de las herramientas
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + 0.5f * (bs - ImGui::GetFrameHeight())));
        if (neo_text_button((_u8L("Load SVG...") + "##stk_load").c_str(), NeoBtn::Ghost, true, Glyph::Load))
            load_sticker_svg_dialog();
    }

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, bs + 2.f * pad));
}

// NEOTKO_COLORSTITCH_TAG — s118: eyedropper + debug read. Lee la receta pintada de
// un objeto, vuelca a PROFILE TODO lo que tiene (slots pintados + base sandwich del
// preset) y la enlaza como color activo (mismo camino que un swatch guardado).
void GLGizmoColorStitchPainter::pick_recipe_from_object(int object_idx, int picked_slot,
                                                     int picked_mesh_id)
{
    namespace CS = Slic3r::ColorSci;
    const Model* model = m_parent.get_selection().get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size()) return;
    const ModelObject* mo = model->objects[object_idx];
    if (!mo) return;
    auto& mgr = SurfaceEffectProfileManager::get();

    // Base sandwich del preset de impresión (lo que gobierna el main UX): si el penu
    // del preset NO está activo, el pintado del penu puede no honrarse → este dump lo
    // hace visible junto a lo pintado.
    {
        const auto& pc = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        auto gb = [&](const char* k)->int {
            auto* o = pc.option<ConfigOptionBool>(k); return o ? (o->value ? 1 : 0) : -1; };
        auto gi = [&](const char* k)->int {
            auto* o = pc.option<ConfigOptionInt>(k);  return o ? o->value : -1; };
        NEOTKO_LOG(PROFILE, "PICK obj='" << mo->name << "' mo=" << (const void*)mo
            << " obj_idx=" << object_idx << " BASE"
            << " colormix_en=" << gb("interlayer_colormix_enabled")
            << " colormix_surface=" << gi("interlayer_colormix_surface")
            << " multipass_en=" << gb("multipass_enabled")
            << " penu_multipass_en=" << gb("penultimate_multipass_enabled")
            << " perim_override=" << gb("multipass_perimeter_override"));
    }

    // s232 — RAÍZ del "tras Assemble el colorpick se queda raro y luego sale gris":
    // este bucle recorría TODOS los volúmenes y se quedaba con el primero que tuviera
    // el número de slot clicado (`s == picked_slot`). Con un solo volumen da igual,
    // pero un objeto ensamblado tiene un volumen por cubo, cada uno con su propia
    // tabla slot→perfil: el slot 1 del primer volumen no es el mismo color que el slot
    // 1 del segundo. Resultado: el eyedropper enlazaba la receta de OTRO cubo (la que
    // se veía en Pro con 3 Solid en vez de la ColorStitch), y al guardar y aplicar esa
    // receta ajena las caras salían con su color plano. El volumen del raycast es el
    // que manda; el barrido global queda sólo como respaldo cuando no se conoce.
    const ModelVolume* picked_mv = volume_for_mesh_id(mo, picked_mesh_id);

    int first_pid_vol = 0;   // primer slot pintado DEL volumen clicado
    int first_pid_any = 0;   // primer slot pintado del objeto (último recurso)
    int picked_pid    = 0;   // perfil del slot REAL bajo el cursor
    if (picked_mv && picked_slot >= 1 && picked_slot < MAX_SLOTS)
        picked_pid = picked_mv->colorstitch_slot_to_profile_id[picked_slot];
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int pid = mv->colorstitch_slot_to_profile_id[s];
            if (pid == 0) continue;
            const SurfaceEffectProfile* p = mgr.find(pid);
            const SurfacePassStack st_top  = p ? SurfacePassStack::from_json(p->stack_top_json)  : SurfacePassStack{};
            const SurfacePassStack st_penu = p ? SurfacePassStack::from_json(p->stack_penu_json) : SurfacePassStack{};
            // NEOTKO_COLORSTITCH_TAG — s118 dbg: resumen de KINDS por zona (S=Solid,
            // C=ColorStitch, H=PB-Half, F=PB-Full) → ver si el penu lleva el EFECTO o
            // sólo Solid plano (causa de "penu slicea pero sin efecto").
            auto kinds = [](const SurfacePassStack& st) {
                std::string s; using K = SurfacePassKind;
                for (const auto& pp : st.passes)
                    s += (pp.kind == K::Solid ? 'S' : pp.kind == K::ColorStitch ? 'C'
                        : pp.kind == K::PathBlend ? 'P' : '?');
                return s.empty() ? std::string("-") : s;
            };
            NEOTKO_LOG(PROFILE, "PICK obj='" << mo->name << "' vol='" << mv->name
                << "' slot=" << s << " pid=" << pid
                // s232 — la marca ahora exige VOLUMEN + slot: antes marcaba como "bajo
                // el cursor" el mismo número de slot en todos los volúmenes, que es el
                // propio bug escrito en el log.
                << ((s == picked_slot && (!picked_mv || mv == picked_mv))
                        ? " <== UNDER CURSOR" : "")
                << " name='" << (p ? p->name : std::string("?")) << "'"
                << " cm=" << (p && p->colorstitch.present)
                << " pb=" << (p && p->pathblend.present)
                << " top[" << kinds(st_top) << "] penu[" << kinds(st_penu) << "]");
            // s232 — el respaldo "primer slot pintado" también prefiere el volumen
            // clicado: caer al de otro cubo del ensamblado es el mismo error por la
            // puerta de atrás. Se acumulan los dos candidatos por separado porque el
            // volumen clicado puede venir DESPUÉS en el recorrido; la elección se hace
            // al salir del bucle.
            if (picked_mv && mv == picked_mv) { if (first_pid_vol == 0) first_pid_vol = pid; }
            if (first_pid_any == 0) first_pid_any = pid;
            if (s == picked_slot && picked_pid == 0 && !picked_mv) picked_pid = pid;
        }
    }

    // Preferir el perfil de la faceta clicada; si la cara no estaba pintada (slot 0)
    // o no se resolvió, caer al primer slot pintado del MISMO volumen y, sólo si ese
    // volumen está limpio, a cualquiera del objeto (s232).
    const int sel_pid = (picked_pid != 0) ? picked_pid
                      : (first_pid_vol != 0) ? first_pid_vol : first_pid_any;
    if (sel_pid == 0) {
        NEOTKO_LOG(PROFILE, "PICK obj='" << mo->name << "' picked_slot=" << picked_slot
            << " mesh_id=" << picked_mesh_id << " → no painted slots");
        return;
    }

    // Enlazar la receta pintada como color activo (camino del swatch guardado).
    const SurfaceEffectProfile* p = mgr.find(sel_pid);
    if (!p) return;
    const CS::ColorRecipe r = recipe_from_profile(*p);
    set_tool(TOOL_PAINT);          // s231 F6: exclusión de herramientas en un solo sitio
    m_active_pal_kind     = 0;     // s231 F5: el color no viene de una tira generada
    m_active_pal_idx      = -1;
    m_selected_profile_id = sel_pid;
    m_active_resolved     = false;
    m_active_slot         = slot_for_selected_profile(/*assign_if_missing=*/true);
    m_active_recipe       = r;
    m_active_style        = cs_strip_group(p->name);   // s137: sin sufijo de grupo
    m_has_active_recipe   = true;
    load_recipe_into_pro(r);
    refresh_selector_palettes();
    NEOTKO_LOG(PROFILE, "PICK selected pid=" << sel_pid << " name='" << p->name
        << "' (picked_slot=" << picked_slot << (picked_pid ? " exact" : " fallback-first") << ")");
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// NEOTKO_STICKER_TAG — file picker para el SVG de la pegatina. Diálogo propio
// (no reusa `choose_svg_file()` de GLGizmoSVG.cpp: esa función es file-local a
// ese .cpp, sin declaración en su .hpp — replicar aquí ~10 líneas es más barato
// y seguro que exponerla, y mantiene el painter sin blast radius sobre el gizmo
// SVG compartido, mismo criterio que los iconos privados de s173).
// s349 — aplanar (fondo, transparencias) y sacar colores + recetas del SVG pendiente.
void GLGizmoColorStitchPainter::prepare_pending_sticker()
{
    m_pending_sticker_svg = ColorStitch::svg_flatten_for_sticker(
        m_pending_sticker_svg_raw, m_sticker_keep_background, &m_pending_flatten);
    m_pending_gradients = ColorStitch::sticker_svg_gradients(m_pending_sticker_svg);   // s349
    detect_pending_sticker_colors();   // s348
    solve_pending_sticker_recipes();
}

bool GLGizmoColorStitchPainter::load_sticker_svg_dialog()
{
    wxWindow* parent = nullptr;
    wxFileDialog dlg(parent, _L("Select SVG for sticker"), wxEmptyString, wxEmptyString,
                      file_wildcards(FT_SVG), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return false;

    const std::string path = into_u8(dlg.GetPath());
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        NEOTKO_LOG(PROFILE, "STICKER_LOAD failed to open path='" << path << "'");
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ss.str().empty()) return false;

    m_pending_sticker_svg_raw = ColorStitch::svg_inline_css_classes(ss.str());   // s349: colores por clase CSS
    m_pending_sticker_name    = boost::filesystem::path(path).stem().string();
    prepare_pending_sticker();
    NEOTKO_LOG(PROFILE, "STICKER_LOAD name='" << m_pending_sticker_name
        << "' bytes=" << m_pending_sticker_svg.size() << " colors=" << m_pending_svg_colors.size());
    return true;
}

// s350 — ámbito «recetas para la zona Bottom» (m_solve_bottom), restaurado al salir.
struct CsBottomSolve {
    bool& f; bool old;
    CsBottomSolve(bool& flag, bool v) : f(flag), old(flag) { f = v; }
    ~CsBottomSolve() { f = old; }
};

// NEOTKO_STICKER_TAG — coloca el SVG pendiente como nueva pegatina: plana en el
// plano XY del frame-objeto (v1, sin rotación), en la posición del click. `hit_local`
// vive en el frame LOCAL del mesh del volumen impactado (rr_hit()); `mv->get_matrix()`
// lo lleva al frame-objeto compartido — el MISMO frame que `sticker_footprint_slice_frame`
// espera (ver comentario en Model.hpp: ColorStitchSticker::transform). Se apila al FINAL
// (back() = tope de la pila = ocluye a las demás, convención fijada en ColorStitch.cpp).
void GLGizmoColorStitchPainter::place_sticker_at(ModelObject* mo, const ModelVolume* mv, const Vec3f& hit_local)
{
    if (!mo || !mv || m_pending_sticker_svg.empty()) return;
    // s231 F1 — un sticker es pintado por-cara: si MixedFilament gobierna el objeto, el
    // motor lo ignora igual que al resto. No colocar en vez de colocar algo inerte.
    if (painting_blocked()) return;

    const Vec3d hit_obj = mv->get_matrix() * hit_local.cast<double>();

    // s348 — undo antes de mutar el objeto.
    Plater::TakeSnapshot snap(wxGetApp().plater(), _u8L("Place sticker"), UndoRedo::SnapshotType::GizmoAction);
    // s350 — a la cara plana de su LADO en esa vertical (ver sticker_project).
    Vec3d place_pos;
    if (!sticker_project(mo, hit_obj, m_place_bottom, place_pos)) {
        m_sticker_place_msg = m_place_bottom ? _u8L("No flat face facing down here.")
                                             : _u8L("No flat face facing up here.");
        return;
    }
    m_sticker_place_msg.clear();
    ColorStitchSticker st;
    st.name       = m_pending_sticker_name;
    st.svg_data   = m_pending_sticker_svg;
    st.profile_id = m_selected_profile_id;   // misma fuente que "con qué color pinto ahora"
    // s348 — multicolor: un perfil por color detectado (dedup: colocar copias no duplica perfiles).
    if (m_import_multicolor && !m_pending_svg_colors.empty()
        && m_pending_recipes.size() == m_pending_svg_colors.size()) {
        CsBottomSolve bottom_scope(m_solve_bottom, m_place_bottom);   // s350: recetas de su lado
        for (size_t j = 0; j < m_pending_svg_colors.size(); ++j) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%06X", (unsigned)m_pending_svg_colors[j]);
            ColorStitchStickerColor c;
            c.rgb        = m_pending_svg_colors[j];
            c.zone       = int(j);   // s350 — m_pending_svg_colors ES la lista de zonas
            const bool is_grad = j < m_pending_svg_grad.size() && m_pending_svg_grad[j];
            if (m_import_grad_bands && is_grad)   // s349 — degradado del SVG ⇒ bandas planas
                c.band_profiles = make_band_profiles(c.rgb, m_pending_gradients, m_import_color_mode,
                                                     m_pending_sticker_name + " " + hex);
            c.profile_id = c.band_profiles.empty()
                ? profile_from_sticker_recipe(m_place_bottom ? solve_sticker_color(c.rgb, m_import_color_mode, nullptr)
                                                             : m_pending_recipes[j],
                                              m_pending_sticker_name + " " + hex)
                : c.band_profiles.front();
            st.colors.push_back(c);
        }
        st.color_mode = m_import_color_mode;
        st.profile_id = st.colors.front().profile_id;   // respaldo (lista / lectores viejos)
    }
    {
        ColorStitch::StickerPose pose;
        pose.pos    = place_pos;
        pose.bottom = m_place_bottom;
        st.transform = ColorStitch::sticker_compose(pose, sticker_obj_scale(mo));
    }
    mo->colorstitch_stickers.push_back(std::move(st));

    m_preview_dirty = true;
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    NEOTKO_LOG(PROFILE, "STICKER_PLACE obj='" << mo->name << "' name='" << m_pending_sticker_name
        << "' profile_id=" << mo->colorstitch_stickers.back().profile_id << " colors=" << mo->colorstitch_stickers.back().colors.size()
        << " pos=(" << hit_obj.x() << "," << hit_obj.y() << "," << hit_obj.z() << ")"
        << " pile_size=" << mo->colorstitch_stickers.size());
}

// NEOTKO_STICKER_TAG — entra en modo edición (mover/rotar) para el sticker en
// `idx`. Fuerza la herramienta Sticker activa (mutex con Select/Paint/Eraser/
// Pick) y decodifica el ángulo Z actual de su transform para el slider — ver
// nota del header sobre por qué esa decodificación es EXACTA (invariante
// Translation*RotationZ que nosotros mismos garantizamos).
void GLGizmoColorStitchPainter::enter_sticker_edit(int idx)
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || idx < 0 || idx >= (int)mo->colorstitch_stickers.size()) return;

    set_tool(TOOL_STICKER);        // s231 F6 — exclusión en un solo sitio
    m_editing_sticker_idx = idx;   // (set_tool sólo lo limpia al SALIR de Sticker)
    m_sticker_dragging    = false;

    const ColorStitchSticker& st = mo->colorstitch_stickers[idx];
    {   // s350 — de la pose (con volteo y espejo), no de la columna 0 a pelo.
        const ColorStitch::StickerPose pose = ColorStitch::sticker_decompose(st.transform);
        m_editing_spin_deg = float(pose.spin_rad * 180.0 / M_PI);
        m_editing_scale    = float(pose.scale);
    }
    m_sticker_overlay_built_for = -1;   // fuerza reconstruir el overlay para ESTE sticker
    // s364 — editar un sticker (cualquiera) despliega su editor en la pestaña Stickers, por la
    // pestaña Place, y la cámara mira su lado. Al pegarlo, no: primero se ve qué sale.
    reset_sticker_editor(mo, idx);
    m_stkc_tab         = 0;
    m_stkc_block_open  = true;
    m_pro_from_sticker = false;
    if (ColorStitch::sticker_faces_down(st)) sticker_view_side(true);
    {   // s348 — tamaño del SVG a escala 1, para el "% del objeto"
        const Polygons rings = ColorStitch::sticker_rings_in_transform(st, Transform3d::Identity());
        const BoundingBox bb = get_extents(rings);
        m_edit_svg_w = bb.defined ? unscale<double>(bb.size().x()) : 0.0;
        m_edit_svg_h = bb.defined ? unscale<double>(bb.size().y()) : 0.0;
    }

    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// s348 — escala a la que el SVG (girado `spin_rad`) cabe en la caja XY del objeto con un 10 % de
// margen. Caja en frame OBJETO (raw_mesh_bounding_box): la instancia escala objeto y sticker por
// igual, así que "cabe" sigue siendo verdad al escalar el objeto. 0 si no se puede calcular.
double GLGizmoColorStitchPainter::sticker_fit_scale(const ModelObject* mo, double spin_rad) const
{
    if (!mo || m_edit_svg_w <= 0.0 || m_edit_svg_h <= 0.0) return 0.0;
    const BoundingBoxf3 bb = mo->raw_mesh_bounding_box();
    if (!bb.defined) return 0.0;
    const double c = std::abs(std::cos(spin_rad)), s = std::abs(std::sin(spin_rad));
    const double W = c * m_edit_svg_w + s * m_edit_svg_h;
    const double H = s * m_edit_svg_w + c * m_edit_svg_h;
    const Vec3d sz = bb.size();
    if (W <= 0.0 || H <= 0.0 || sz.x() <= 0.0 || sz.y() <= 0.0) return 0.0;
    return 0.9 * std::min(sz.x() / W, sz.y() / H);
}

// s350 — escala de la instancia en los ejes del objeto (la que deforma al sticker si no es uniforme).
Vec3d GLGizmoColorStitchPainter::sticker_obj_scale(const ModelObject* mo)
{
    if (!mo || mo->instances.empty()) return Vec3d::Ones();
    return mo->instances.front()->get_scaling_factor().cwiseAbs();
}

// s350 — PROYECCIÓN: el punto `p_obj` (frame objeto) se lleva a la cara PLANA del lado pedido que
// hay en su misma vertical. Bottom: la cara que mira abajo más alta que quede por debajo de p (la
// de debajo del clic); top: la que mira arriba más baja que quede por encima. Si no hay en ese
// sentido, la más cercana. Plana = |nz| ≥ 0.98 (≈ 11°): en una cara inclinada no se coloca.
bool GLGizmoColorStitchPainter::sticker_project(const ModelObject* mo, const Vec3d& p, bool bottom, Vec3d& out) const
{
    if (!mo) return false;
    constexpr double kFlat = 0.98, kEps = 1e-3;
    bool   have_side = false, have_any = false;
    double z_side = 0.0, z_any = 0.0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        const Transform3d m = mv->get_matrix();
        const bool flip = m.linear().determinant() < 0.0;
        const indexed_triangle_set& its = mv->mesh().its;
        for (const stl_triangle_vertex_indices& f : its.indices) {
            const Vec3d a = m * its.vertices[f[0]].cast<double>();
            const Vec3d b = m * its.vertices[f[1]].cast<double>();
            const Vec3d c = m * its.vertices[f[2]].cast<double>();
            Vec3d n = (b - a).cross(c - a);
            const double len = n.norm();
            if (len < 1e-12) continue;
            n /= len;
            if (flip) n = -n;
            if (bottom ? (n.z() > -kFlat) : (n.z() < kFlat)) continue;
            // ¿(p.x, p.y) dentro del triángulo en planta? Baricéntricas 2D.
            const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (std::abs(d) < 1e-14) continue;
            const double l1 = ((b.y() - c.y()) * (p.x() - c.x()) + (c.x() - b.x()) * (p.y() - c.y())) / d;
            const double l2 = ((c.y() - a.y()) * (p.x() - c.x()) + (a.x() - c.x()) * (p.y() - c.y())) / d;
            const double l3 = 1.0 - l1 - l2;
            if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9) continue;
            const double z = l1 * a.z() + l2 * b.z() + l3 * c.z();
            const bool on_side = bottom ? (z <= p.z() + kEps) : (z >= p.z() - kEps);
            if (on_side && (!have_side || (bottom ? z > z_side : z < z_side))) { z_side = z; have_side = true; }
            if (!have_any || std::abs(z - p.z()) < std::abs(z_any - p.z())) { z_any = z; have_any = true; }
        }
    }
    if (!have_side && !have_any) return false;
    out = Vec3d(p.x(), p.y(), have_side ? z_side : z_any);
    return true;
}

// s350 — la cámara mira la cara del lado (bottom: desde abajo).
void GLGizmoColorStitchPainter::sticker_view_side(bool bottom)
{
    m_parent.select_view(bottom ? "bottom" : "top");
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoColorStitchPainter::exit_sticker_edit()
{
    m_editing_sticker_idx = -1;
    m_pro_from_sticker    = false;   // s364
    m_sticker_dragging    = false;
    m_parent.set_as_dirty();
}

// NEOTKO_STICKER_TAG — construye (una vez por sticker en edición) la geometría
// GL del overlay en frame LOCAL (z=0, sin colocación): contorno (líneas, vía
// `sticker_rings_in_transform` + `GLModel::init_from(Polygons, z)`, soporta
// huecos/cóncavo tal cual) + relleno (triángulos, vía `triangulate_expolygons_2f`
// — el mismo tesselador glu que usa el resto de Orca para generar sólidos desde
// ExPolygons, así que letras/logos con huecos salen correctos, no una
// aproximación). Reusar `ColorStitch::sticker_rings_in_transform` evita
// duplicar el parseo NSVG que ya vive en el motor de slice.
void GLGizmoColorStitchPainter::ensure_sticker_overlay_built(const ColorStitchSticker& sticker)
{
    if (m_sticker_overlay_built_for == m_editing_sticker_idx) return;
    m_sticker_overlay_built_for = m_editing_sticker_idx;

    m_sticker_overlay_fill.reset();
    m_sticker_overlay_outline.reset();
    m_sticker_overlay_color_fills.clear();
    m_sticker_overlay_color_idx.clear();

    // s348 — sonda: si la conversión se cuelga, el log ya dice QUÉ sticker era (antes no había nada).
    NEOTKO_LOG(PROFILE, "STICKER_OVERLAY begin idx=" << m_editing_sticker_idx << " name='" << sticker.name
        << "' bytes=" << sticker.svg_data.size());
    const auto t0 = std::chrono::steady_clock::now();
    const Polygons rings = ColorStitch::sticker_rings_in_transform(sticker, Transform3d::Identity());
    size_t npts = 0;
    for (const Polygon& r : rings) npts += r.points.size();
    const double ov_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    NEOTKO_LOG(PROFILE, "STICKER_OVERLAY end rings=" << rings.size() << " pts=" << npts << " ms=" << ov_ms);
    if (rings.empty()) return;

    m_sticker_overlay_outline.init_from(rings, 0.f);
    // s349 — los RELLENOS ya no se construyen aquí: los dibuja ColorStitchStickerPreview::render_one,
    // recortados a la cara del objeto y rehechos al mover (aquí sólo se construían una vez).
}

// NEOTKO_STICKER_TAG — dibuja el sticker en edición con SU color asignado (no
// solo contorno), para que mover/rotar se haga sabiendo cómo va a quedar contra
// lo que ya hay debajo/alrededor — pedido explícito del usuario. Mismo patrón
// que GLGizmoFlatten::on_render (shader "flat", GLModel construido en frame
// LOCAL, transform vía uniform view_model_matrix, pequeño alzado en Z mundo
// para evitar z-fighting con la superficie real).
void GLGizmoColorStitchPainter::render_sticker_edit_overlay()
{
    if (m_editing_sticker_idx < 0) return;
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || m_editing_sticker_idx >= (int)mo->colorstitch_stickers.size()) { exit_sticker_edit(); return; }
    if (mo->instances.empty()) return;

    const ColorStitchSticker& st = mo->colorstitch_stickers[m_editing_sticker_idx];
    ensure_sticker_overlay_built(st);
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (!shader) return;

    const ModelInstance* mi = mo->instances.front();
    const Camera& camera = wxGetApp().plater()->get_camera();
    // Alzado en Z MUNDO (no local) para evitar z-fighting con la superficie real
    // — mismo truco que GLGizmoFlatten::update_planes ("Raise a bit above the
    // object surface to avoid flickering"), aplicado tras el resto de la cadena
    // para que sea robusto a la orientación de la instancia.
    const Transform3d lift = Geometry::translation_transform(
        Vec3d(0.0, 0.0, ColorStitchStickerPreview::sticker_lift_mm(st, m_editing_sticker_idx)));   // s348 — escalonado por pila; s350: con el lado
    const Transform3d model_matrix = lift * mi->get_transformation().get_matrix() * st.transform;
    const Transform3d view_model_matrix = camera.get_view_matrix() * model_matrix;

    // s349 — rellenos: el mismo camino que la vista normal (recortados a la cara del objeto, lo
    // que ves es lo que sale). Opacidad 0.8: se ve el color Y lo que hay debajo.
    // s350 — con los colores de ESTE sticker a la vista, el color (o tramo) elegido late y el resto
    // se apaga: «estás cambiando esta zona». Latido de ~1.2 s; sólo entonces se pide repintado
    // continuo (el canvas de Orca repinta bajo demanda). s364: «a la vista» = la pestaña Colours
    // del editor se dibujó en el último fotograma, o se está retocando su receta en Pro.
    int   focus = -1;
    float pulse = 0.f;
    if (m_stkc_obj_id == mo->id().id && st.is_multicolor()
        && (m_stkc_colours_shown || (m_pro_from_sticker && m_department == DEP_PRO))) {
        focus = m_stkc_cur;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        pulse = 0.5f + 0.5f * float(std::sin(t * 2.0 * M_PI / 1.2));
        m_parent.schedule_animation_frame(40);   // s350: el temporizador SÍ repinta
    }
    ColorStitchStickerPreview::render_one(*mo, m_editing_sticker_idx, mi->get_transformation().get_matrix(),
                                          camera, st.is_multicolor() ? 0.8f : 0.55f,
                                          focus, focus >= 0 ? m_stkc_focus_b0 : -1, focus >= 0 ? m_stkc_focus_b1 : -1, pulse);
    if (!m_sticker_overlay_outline.is_initialized()) return;

    shader->start_using();
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_DEPTH_TEST));

    if (m_sticker_overlay_outline.is_initialized()) {
        m_sticker_overlay_outline.set_color(ColorRGBA(1.f, 1.f, 1.f, 0.9f));
        m_sticker_overlay_outline.render();
    }

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// ---------------------------------------------------------------------------
// s348 — SVG MULTICOLOR → una receta por color.
// ---------------------------------------------------------------------------
static ImU32 cs_rgb_u32(uint32_t rgb)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

// Colores del SVG pendiente = sus ZONAS (s350, ColorStitch::sticker_svg_zones): los casi iguales
// (ΔE2000 < 2) ya vienen juntos, y un degradado es su propia zona aunque su media se parezca a un
// plano. La entrada j del sticker guarda zone = j, así el motor no tiene que adivinar por RGB.
void GLGizmoColorStitchPainter::detect_pending_sticker_colors()
{
    m_pending_svg_colors.clear();
    m_pending_svg_grad.clear();
    m_pending_recipes.clear();
    for (const ColorStitch::StickerZone& z : ColorStitch::sticker_svg_zones(m_pending_sticker_svg)) {
        m_pending_svg_colors.push_back(z.rgb);
        m_pending_svg_grad.push_back(z.grad);
    }
}

static std::string cs_hex(uint32_t rgb);   // s350 (definida con el panel de colores)
// s350 — receta BOTTOM: vista desde la cama, la pasada 0 (la primera impresa) delante. Plano: las
// mismas parejas de 1-2 Solid que la paleta plana, puestas al revés (la visible primero). ColorStitch:
// una pasada de dither. Cumple los límites de la zona Bottom (2 Solid + 1 ColorStitch).
Slic3r::ColorSci::ColorRecipe GLGizmoColorStitchPainter::solve_sticker_color_bottom(uint32_t rgb, int mode) const
{
    namespace CS = Slic3r::ColorSci;
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    CS::PredictOptions o;
    o.layer_height = GLGizmoColorStitchPainter_layer_height();
    float bg[3] = { 0.f, 0.f, 0.f };
    if (resolve_object_base_bg(mats, bg)) { o.bg_rgb[0] = bg[0]; o.bg_rgb[1] = bg[1]; o.bg_rgb[2] = bg[2]; }
    const float target[3] = { ((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f };
    const CS::Lab tlab = CS::rgb_to_lab(target);
    CS::ColorRecipe best;
    best.delta_e = 1e9f;
    auto consider = [&](SurfacePassStack bot) {
        if (bot.passes.empty()) return;
        bot.enabled = true;
        float c[3] = { 0.f, 0.f, 0.f };
        CS::sandwich_colour_bottom(bot, mats, o.bg_rgb, o.layer_height, c);
        const float de = CS::delta_e2000(tlab, CS::rgb_to_lab(c));
        if (de < best.delta_e) { best.bottom = std::move(bot); best.rgb = { c[0], c[1], c[2] }; best.delta_e = de; }
    };
    if (mode == 1) {
        float de = -1.f;
        consider(CS::suggest_dither_single(target, mats, o, &de));
    } else {
        CS::PredictOptions wide = o;
        wide.max_entries = 1 << 30;
        wide.dedup_de    = 0.f;
        wide.ratio_steps = std::max(o.ratio_steps, 12);
        for (const CS::ColorRecipe& r : CS::predict_flat_palette(mats, wide))
            consider(CS::reversed_stack(r.top));   // la pasada visible arriba pasa a ser la 0
    }
    best.desc = "bottom";
    NEOTKO_LOG(PROFILE, "STICKER_BOTTOM_SOLVE rgb=" << cs_hex(rgb) << " mode=" << mode
        << " passes=" << best.bottom.passes.size() << " dE=" << best.delta_e);
    return best;
}

Slic3r::ColorSci::ColorRecipe GLGizmoColorStitchPainter::solve_sticker_color(uint32_t rgb, int mode,
    const std::map<uint32_t, ColorStitch::StickerGradient>* grads) const
{
    namespace CS = Slic3r::ColorSci;
    if (m_solve_bottom) return solve_sticker_color_bottom(rgb, mode);   // s350 (sin rampa suave en bottom)
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    CS::PredictOptions o;
    o.layer_height = GLGizmoColorStitchPainter_layer_height();
    float bg_rgb[3] = { 0.f, 0.f, 0.f };
    if (resolve_object_base_bg(mats, bg_rgb)) { o.bg_rgb[0] = bg_rgb[0]; o.bg_rgb[1] = bg_rgb[1]; o.bg_rgb[2] = bg_rgb[2]; }
    const float target[3] = { ((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f };
    // s349b — DEGRADADO LINEAL del SVG → sandwich de DOS sub-pasadas en la capa Top (la misma
    // estructura que el solucionador plano: abajo gruesa, arriba fina y visible, con la MISMA
    // proporción de alturas en todo el sticker), y CADA sub-pasada con su propio degradado
    // ColorStitch entre dos filamentos (o sólida si no cambia). Así un rojo claro → rojo oscuro,
    // imposible entre dos filamentos, sale como «rojo fino encima de un fondo que va de blanco a
    // negro». Se buscan los filamentos por muestra (t = 0, ½, 1) para cada proporción, se
    // predice el color de la receta CONSTRUIDA en las tres muestras y gana la de menor error
    // máximo. Rampa = easing t² (EaseIn) al 50 %: con el lineal el Bresenham reparte a densidad
    // constante (mezcla uniforme). Eje y recorrido: los pone el motor desde el SVG.
    if (grads) {
        const auto git = grads->find(rgb);
        if (git != grads->end() && git->second.nstops == 3) {
            const ColorStitch::StickerGradient& g = git->second;
            CS::Lab tl[3];
            for (int k = 0; k < 3; ++k) {
                const float tg[3] = { ((g.stops[k] >> 16) & 0xFF) / 255.f, ((g.stops[k] >> 8) & 0xFF) / 255.f,
                                      (g.stops[k] & 0xFF) / 255.f };
                tl[k] = CS::rgb_to_lab(tg);
            }
            const SurfacePassStack no_penu;
            auto predict = [&](const SurfacePassStack& top) {
                float c[3] = { 0.f, 0.f, 0.f };
                CS::sandwich_colour_stacked(top, no_penu, mats, o.bg_rgb, o.layer_height, c);
                return CS::rgb_to_lab(c);
            };
            // Pasada sólida, o mezcla a pct_b (sólo para PREDECIR el medio de una rampa).
            auto pass_of = [](int a, int b, int pct_b, double ratio) {
                SurfacePass p;
                p.ratio = ratio; p.solid_tool = a; p.angle = -1; p.fan = -1; p.speed_pct = 100;
                if (a == b || pct_b <= 0) { p.kind = SurfacePassKind::Solid; return p; }
                if (pct_b >= 100) { p.kind = SurfacePassKind::Solid; p.solid_tool = b; return p; }
                p.kind = SurfacePassKind::ColorStitch;
                p.colorstitch.present = true;
                constexpr int L = 8;
                const int nb = std::clamp((int)std::lround(pct_b * L / 100.0), 0, L);
                std::string pat; int acc = 0;
                for (int i = 0; i < L; ++i) { acc += nb; if (acc >= L) { acc -= L; pat += char('1' + b); } else pat += char('1' + a); }
                p.colorstitch.kv["interlayer_colormix_pattern_top"] = pat;
                p.colorstitch.kv["interlayer_colormix_tool_a"] = std::to_string(a);
                p.colorstitch.kv["interlayer_colormix_tool_b"] = std::to_string(b);
                return p;
            };
            // Pasada FINAL de la receta: rampa from→to (sólida si no cambia).
            auto ramp_pass = [](int from, int to, double ratio) {
                SurfacePass p;
                p.ratio = ratio; p.solid_tool = from; p.angle = -1; p.fan = -1; p.speed_pct = 100;
                if (from == to) { p.kind = SurfacePassKind::Solid; return p; }
                p.kind = SurfacePassKind::ColorStitch;
                p.colorstitch.present = true;
                auto& kv = p.colorstitch.kv;
                kv["interlayer_colormix_pattern_top"]       = std::string(1, char('1' + from)) + char('1' + to);
                kv["interlayer_colormix_mode"]              = "1";
                kv["interlayer_colormix_tool_a"]            = std::to_string(from);
                kv["interlayer_colormix_tool_b"]            = std::to_string(to);
                kv["interlayer_colormix_pct_a"]             = "50";
                kv["interlayer_colormix_easing"]            = "1";   // EaseIn (t²) = rampa de densidad
                kv["interlayer_colormix_gradient_span_mm"]  = "0";
                kv["interlayer_colormix_repetitions"]       = "1";
                kv["interlayer_colormix_invert"]            = "0";
                kv["interlayer_colormix_min_surface_lines"] = "0";
                kv["interlayer_colormix_angle"]             = "-1";
                return p;
            };
            const int steps = std::max(2, o.ratio_steps);
            const double ra_lo = o.min_top_visible_mm / o.layer_height;
            const double ra_hi = 1.0 - o.min_pass_mm / o.layer_height;
            // s349c — criterio: error MEDIO en las 3 muestras (con el máximo, un extremo inalcanzable
            // empataba todo en «malo» y ganaba lo que no cambia). Y se compara con la mejor receta
            // PLANA en esas mismas muestras: el degradado gana si se parece más al SVG, sin tope
            // absoluto («acercarse con lo que hay», pedido suyo).
            float best_err = 1e9f, best_e[3] = { 0, 0, 0 }, flat_err = 1e9f;
            double best_ra = 1.0; int bt[2] = { 0, 0 }, bb[2] = { 0, 0 };
            // ra = 1 → una sola pasada (sin fondo); el resto, fondo + visible con la proporción ra.
            for (int s = -1; s <= steps; ++s) {
                const bool single = (s < 0);
                if (!single && !(ra_hi > ra_lo)) break;
                const double ra = single ? 1.0 : ra_lo + (ra_hi - ra_lo) * double(s) / steps;
                // Para cada extremo, el mejor (arriba, abajo) con ESA proporción.
                int ta[2], tb[2];
                for (int e = 0; e < 2; ++e) {
                    const int k = e ? 2 : 0;
                    float be = 1e9f;
                    for (int a = 0; a < 4; ++a)
                        for (int b = 0; b < (single ? 1 : 4); ++b) {
                            if (!single && a == b) continue;
                            SurfacePassStack st; st.enabled = true;
                            if (!single) st.passes.push_back(pass_of(b, b, 0, 1.0 - ra));
                            st.passes.push_back(pass_of(a, a, 0, ra));
                            const float d = CS::delta_e2000(tl[k], predict(st));
                            if (d < be) { be = d; ta[e] = a; tb[e] = single ? a : b; }
                        }
                }
                // Error de la receta construida en t = 0, ½ y 1 (el medio = mezcla al 50 % en cada pasada).
                float err[3];
                for (int k = 0; k < 3; ++k) {
                    const int pct = k * 50;
                    SurfacePassStack st; st.enabled = true;
                    if (!single) st.passes.push_back(pass_of(tb[0], tb[1], pct, 1.0 - ra));
                    st.passes.push_back(pass_of(ta[0], ta[1], pct, ra));
                    err[k] = CS::delta_e2000(tl[k], predict(st));
                }
                const float mean = (err[0] + err[1] + err[2]) / 3.f;
                // Referencia plana con esta proporción: los filamentos que mejor hacen el MEDIO, fijos.
                for (int a = 0; a < 4; ++a)
                    for (int b = 0; b < (single ? 1 : 4); ++b) {
                        if (!single && a == b) continue;
                        SurfacePassStack st; st.enabled = true;
                        if (!single) st.passes.push_back(pass_of(b, b, 0, 1.0 - ra));
                        st.passes.push_back(pass_of(a, a, 0, ra));
                        const CS::Lab pl = predict(st);
                        const float fm = (CS::delta_e2000(tl[0], pl) + CS::delta_e2000(tl[1], pl)
                                        + CS::delta_e2000(tl[2], pl)) / 3.f;
                        flat_err = std::min(flat_err, fm);
                    }
                const bool chg = (ta[0] != ta[1]) || (!single && tb[0] != tb[1]);
                if (chg && mean < best_err) {
                    best_err = mean; best_ra = ra;
                    bt[0] = ta[0]; bt[1] = ta[1]; bb[0] = tb[0]; bb[1] = tb[1];
                    for (int k = 0; k < 3; ++k) best_e[k] = err[k];
                }
            }
            const bool changes = best_err < 1e8f;
            NEOTKO_LOG(PROFILE, "STICKER_GRAD_SOLVE rgb=" << std::hex << rgb << std::dec
                << " ra=" << best_ra << " top=F" << bt[0] + 1 << "->F" << bt[1] + 1   // s350: F1..F4 (F1 = T0)
                << " fondo=F" << bb[0] + 1 << "->F" << bb[1] + 1
                << " dE=[" << best_e[0] << "," << best_e[1] << "," << best_e[2] << "]"
                << " medio=" << best_err << " plano=" << flat_err
                << " → " << (!changes ? "plano (ninguna rampa)" : best_err < flat_err ? "degradado" : "plano (se parece más)"));
            if (changes && best_err < flat_err) {
                CS::ColorRecipe r;
                r.top.enabled = true;
                if (best_ra < 1.0) r.top.passes.push_back(ramp_pass(bb[0], bb[1], 1.0 - best_ra));
                r.top.passes.push_back(ramp_pass(bt[0], bt[1], best_ra));
                r.delta_e = best_err;
                // s350 — color predicho de la rampa (su media, como la imprime), no el del SVG.
                CS::sandwich_colour_stacked(r.top, SurfacePassStack{}, mats, o.bg_rgb, o.layer_height, r.rgb.data());
                r.desc = "gradient";
                return r;
            }
        }
    }
    if (mode == 1) {
        // ColorStitch: el color se resuelve en XY dentro de UNA pasada (dos tools alternados).
        CS::ColorRecipe r;
        float de = -1.f;
        r.top     = CS::suggest_dither_single(target, mats, o, &de);
        r.delta_e = de;
        // s350 — el color PREDICHO de la receta, no el objetivo: antes se guardaba el color del SVG
        // («el dither no devuelve su rgb») y la vista 3D enseñaba colores imposibles (azules sin
        // filamento azul) y las candidatas decían dE 0.
        CS::sandwich_colour_stacked(r.top, SurfacePassStack{}, mats, o.bg_rgb, o.layer_height, r.rgb.data());
        r.delta_e = CS::delta_e2000(CS::rgb_to_lab(target), CS::rgb_to_lab(r.rgb.data()));
        return r;
    }
    return CS::suggest_sandwich(target, mats, o);   // s349: con Penultimate sólida si ayuda
}

std::vector<int> GLGizmoColorStitchPainter::make_band_profiles(uint32_t rgb,
    const std::map<uint32_t, ColorStitch::StickerGradient>& grads, int mode, const std::string& base_name)
{
    std::vector<int> out;
    const auto git = grads.find(rgb);
    if (git == grads.end() || git->second.ramp.size() < 2) return out;
    const std::vector<uint32_t>& ramp = git->second.ramp;
    const int N = std::clamp(m_grad_band_count, 2, 32);
    namespace CS = Slic3r::ColorSci;
    // Mismos materiales, opciones y fondo que solve_sticker_color, para PREDECIR cómo queda una
    // receta en el color de otra banda.
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    CS::PredictOptions o;
    o.layer_height = GLGizmoColorStitchPainter_layer_height();
    { float bg[3] = { 0.f, 0.f, 0.f };
      if (resolve_object_base_bg(mats, bg)) { o.bg_rgb[0] = bg[0]; o.bg_rgb[1] = bg[1]; o.bg_rgb[2] = bg[2]; } }
    auto lab_of = [](uint32_t c) {
        const float f[3] = { ((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f };
        return CS::rgb_to_lab(f);
    };

    // 1) Cada banda propone su mejor receta ⇒ candidatas (sin repetir).
    std::vector<uint32_t>         target(static_cast<size_t>(N));
    std::vector<CS::ColorRecipe>  cand;
    std::vector<std::string>      cand_key;
    for (int b = 0; b < N; ++b) {
        const double t = (double(b) + 0.5) / N;
        target[size_t(b)] = ramp[size_t(std::lround(t * double(ramp.size() - 1)))];
        CS::ColorRecipe r = solve_sticker_color(target[size_t(b)], mode, nullptr);   // plana: sin rama de degradado
        const std::string key = r.top.to_json() + "|" + r.penu.to_json() + "|" + r.bottom.to_json();   // s350: + bottom
        if (std::find(cand_key.begin(), cand_key.end(), key) == cand_key.end()) {
            cand_key.push_back(key);
            cand.push_back(std::move(r));
        }
    }
    // 2) s349 — SUAVIZADO: error de cada candidata en cada banda, y se elige la secuencia que
    //    minimiza Σ dE + λ·(cambios de receta). Sin esto, dos recetas casi empatadas se alternaban
    //    banda a banda (amarillo/naranja/amarillo…). λ en unidades de dE: un cambio debe ganar al
    //    menos eso para merecer una costura.
    constexpr float kSwitch = 2.5f;
    const int K = int(cand.size());
    std::vector<CS::Lab> pred(static_cast<size_t>(K));
    for (int k = 0; k < K; ++k) {
        float c[3] = { 0.f, 0.f, 0.f };
        if (m_solve_bottom) CS::sandwich_colour_bottom(cand[size_t(k)].bottom, mats, o.bg_rgb, o.layer_height, c);   // s350
        else                CS::sandwich_colour_stacked(cand[size_t(k)].top, cand[size_t(k)].penu, mats, o.bg_rgb, o.layer_height, c);
        pred[size_t(k)] = CS::rgb_to_lab(c);
    }
    std::vector<std::vector<float>> cost(size_t(N), std::vector<float>(size_t(K), 0.f));
    std::vector<std::vector<int>>   from(size_t(N), std::vector<int>(size_t(K), -1));
    for (int b = 0; b < N; ++b) {
        const CS::Lab tl = lab_of(target[size_t(b)]);
        for (int k = 0; k < K; ++k) {
            const float e = CS::delta_e2000(tl, pred[size_t(k)]);
            if (b == 0) { cost[0][size_t(k)] = e; continue; }
            float best = 1e30f; int arg = 0;
            for (int q = 0; q < K; ++q) {
                const float v = cost[size_t(b - 1)][size_t(q)] + (q == k ? 0.f : kSwitch);
                if (v < best) { best = v; arg = q; }
            }
            cost[size_t(b)][size_t(k)] = best + e;
            from[size_t(b)][size_t(k)] = arg;
        }
    }
    std::vector<int> pick(static_cast<size_t>(N), 0);
    { int k = 0; float best = 1e30f;
      for (int q = 0; q < K; ++q) if (cost[size_t(N - 1)][size_t(q)] < best) { best = cost[size_t(N - 1)][size_t(q)]; k = q; }
      for (int b = N - 1; b >= 0; --b) { pick[size_t(b)] = k; if (b > 0) k = from[size_t(b)][size_t(k)]; } }

    // 3) Perfiles sólo para las candidatas que quedan en uso.
    std::map<int, int> pid_of;
    for (int b = 0; b < N; ++b) {
        const int k = pick[size_t(b)];
        auto it = pid_of.find(k);
        if (it == pid_of.end()) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%06X", unsigned(target[size_t(b)]));
            it = pid_of.emplace(k, profile_from_sticker_recipe(cand[size_t(k)], base_name + " b" + std::to_string(b + 1) + " " + hex)).first;
        }
        out.push_back(it->second);
    }
    int switches = 0;
    for (int b = 1; b < N; ++b) switches += (pick[size_t(b)] != pick[size_t(b - 1)]);
    std::map<int, int>& by_json = pid_of;   // (para el log de abajo: recetas distintas en uso)
    NEOTKO_LOG(PROFILE, "STICKER_BANDS_SMOOTH candidatas=" << K << " cambios=" << switches);
    NEOTKO_LOG(PROFILE, "STICKER_BANDS rgb=" << std::hex << rgb << std::dec << " N=" << N
        << " recetas_distintas=" << by_json.size());
    return out;
}

const std::map<uint32_t, ColorStitch::StickerGradient>& GLGizmoColorStitchPainter::sticker_gradients_cached(const std::string& svg)
{
    const size_t key = std::hash<std::string>{}(svg);
    if (key != m_grad_cache_key) {
        m_grad_cache     = ColorStitch::sticker_svg_gradients(svg);
        m_grad_cache_key = key;
    }
    return m_grad_cache;
}

void GLGizmoColorStitchPainter::solve_pending_sticker_recipes()
{
    m_pending_recipes.clear();
    for (size_t j = 0; j < m_pending_svg_colors.size(); ++j)   // s350: degradado sólo si la zona lo es
        m_pending_recipes.push_back(solve_sticker_color(m_pending_svg_colors[j], m_import_color_mode,
            (j < m_pending_svg_grad.size() && m_pending_svg_grad[j]) ? &m_pending_gradients : nullptr));
}

// Receta → perfil GUARDADO del grupo activo (no auto: el GC no lo toca). Dedup por pilas, igual
static uint32_t    cs_f3_rgb(const float c[3]);   // s350 (definidas con el panel de colores)
static std::string cs_hex(uint32_t rgb);

// que save_active_as_palette: el mismo color resuelto igual reutiliza su perfil.
int GLGizmoColorStitchPainter::profile_from_sticker_recipe(const Slic3r::ColorSci::ColorRecipe& r,
                                                          const std::string& name)
{
    if (m_solve_bottom) {   // s350 — receta de la zona BOTTOM (sin top/penu)
        if (r.bottom.passes.empty()) return 0;
        const std::string bot_json = r.bottom.to_json();
        const std::string bname = name + " (bottom)";
        auto& bmgr = SurfaceEffectProfileManager::get();
        for (const SurfaceEffectProfile& p : bmgr.list())
            if (p.stack_bottom_json == bot_json && p.stack_top_json.empty() && cs_strip_group(p.name) == bname) {
                if (SurfaceEffectProfile* mp = bmgr.find_mut(p.id)) mp->auto_generated = false;
                return p.id;
            }
        SurfaceEffectProfile p;
        p.name              = cs_with_group(bname, m_active_group);
        p.stack_bottom_json = bot_json;
        p.preview_argb      = recipe_argb(r);
        p.auto_generated    = false;
        const int id = bmgr.add(std::move(p));
        refresh_selector_palettes();
        return id;
    }
    if (r.top.passes.empty()) return 0;
    const std::string top_json  = r.top.to_json();
    const std::string penu_json = r.penu.to_json();
    auto& mgr = SurfaceEffectProfileManager::get();
    for (const SurfaceEffectProfile& p : mgr.list())
        // Dedup SOLO con el mismo nombre (= mismo color del SVG): dos colores distintos que caen
        // en la misma receta quedan como perfiles separados, si no el azul salía llamado "#FF0000".
        if (p.stack_top_json == top_json && p.stack_penu_json == penu_json && p.stack_bottom_json.empty()
            && cs_strip_group(p.name) == name) {
            if (SurfaceEffectProfile* mp = mgr.find_mut(p.id)) mp->auto_generated = false;
            return p.id;
        }
    SurfaceEffectProfile p;
    p.name            = cs_with_group(name, m_active_group);
    p.stack_top_json  = top_json;
    p.stack_penu_json = penu_json;
    p.preview_argb    = recipe_argb(r);
    p.auto_generated  = false;
    SurfaceEffectProfileManager::payload_from_stacks(r.top, r.penu, p);
    const int id = mgr.add(std::move(p));
    refresh_selector_palettes();
    return id;
}

// ---------------------------------------------------------------------------
// s350 — colores del SVG (un color por zona). s364: dentro del editor de la pestaña Stickers.
// ---------------------------------------------------------------------------
// s364 — el siguiente botón de texto sigue en la misma línea sólo si cabe hasta `right` (x de
// pantalla); si no, baja. El panel es AlwaysAutoResize: un botón que se sale lo ensancha (s319).
static void cs_same_line_if_fits(const std::string& label, bool has_glyph, float right)
{
    const float u = neo_u();
    const float w = ImGui::CalcTextSize(label.c_str(), neo_label_end(label.c_str())).x
                  + (has_glyph ? 1.3f * u : 0.f) + 1.2f * u;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= right) ImGui::SameLine();
}

static float cs_de_rgb(uint32_t a, const float b[3])
{
    namespace CS = Slic3r::ColorSci;
    const float fa[3] = { ((a >> 16) & 0xFF) / 255.f, ((a >> 8) & 0xFF) / 255.f, (a & 0xFF) / 255.f };
    return CS::delta_e2000(CS::rgb_to_lab(fa), CS::rgb_to_lab(b));
}

static uint32_t cs_f3_rgb(const float c[3])
{
    auto q = [](float v) { return uint32_t(std::clamp(std::lround(v * 255.f), 0L, 255L)); };
    return (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
}

// s350 — materiales y fondo del fotograma, para que el panel de colores calcule el color de cada
// receta DESDE SUS CAPAS (como el Pro) y no se fíe de preview_argb, que puede estar viejo.
static bool                          g_stkc_live = false;
static Slic3r::ColorSci::Material    g_stkc_mats[4];
static float                         g_stkc_bg[3] = { 0.f, 0.f, 0.f };

static bool cs_profile_rgb(int pid, float out[3])
{
    const SurfaceEffectProfile* p = pid ? SurfaceEffectProfileManager::get().find(pid) : nullptr;
    if (!p) return false;
    if (g_stkc_live && p->stack_top_json.empty() && !p->stack_bottom_json.empty()) {   // s350: sólo Bottom
        Slic3r::ColorSci::sandwich_colour_bottom(SurfacePassStack::from_json(p->stack_bottom_json), g_stkc_mats, g_stkc_bg, ColorStitchPaintPreview::weave_layer_height(), out);
        return true;
    }
    if (g_stkc_live && !p->stack_top_json.empty()) {
        Slic3r::ColorSci::sandwich_colour_stacked(SurfacePassStack::from_json(p->stack_top_json),
                                                  SurfacePassStack::from_json(p->stack_penu_json),
                                                  g_stkc_mats, g_stkc_bg, ColorStitchPaintPreview::weave_layer_height(), out);
        return true;
    }
    const ColorRGBA c = color_for_profile(*p);
    out[0] = c.r(); out[1] = c.g(); out[2] = c.b();
    return true;
}

static std::string cs_hex(uint32_t rgb)
{
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%06X", unsigned(rgb & 0xFFFFFF));
    return hex;
}

void GLGizmoColorStitchPainter::reset_sticker_editor(const ModelObject* mo, int sticker_idx)
{
    if (!mo || sticker_idx < 0 || sticker_idx >= (int)mo->colorstitch_stickers.size()) return;
    m_stkc_obj_id     = mo->id().id;
    m_stkc_idx        = sticker_idx;
    m_stkc_cur        = 0;
    m_stkc_band       = -1;
    m_stkc_sel.assign(mo->colorstitch_stickers[sticker_idx].colors.size(), 0);
    m_stkc_cand_color = -1;
    m_stkc_cand_band  = -2;
    m_stkc_cands.clear();
}

void GLGizmoColorStitchPainter::edit_profile_in_pro(int pid)
{
    const SurfaceEffectProfile* p = pid ? SurfaceEffectProfileManager::get().find(pid) : nullptr;
    if (!p) return;
    // Mismo enlace que el cuentagotas: el Pro reescribe EN VIVO el perfil seleccionado, así que el
    // sticker (y sus copias, que comparten perfil) cambia según se edita.
    const Slic3r::ColorSci::ColorRecipe r = recipe_from_profile(*p);
    const std::string top_before  = p->stack_top_json, penu_before = p->stack_penu_json;
    invalidate_active_binding(/*keep_recipe=*/true);
    m_selected_profile_id = pid;
    m_active_recipe       = r;
    m_active_style        = cs_strip_group(p->name);
    m_has_active_recipe   = true;
    load_recipe_into_pro(r);
    {   // s350 — recetas guardadas con el color viejo (el Pro lo guardaba un cambio por detrás):
        // al abrirla se corrige su preview_argb desde sus capas.
        namespace CS = Slic3r::ColorSci;
        CS::Material mats[4]; std::vector<std::string> fc; gizmo_materials(mats, fc);
        float bg[3] = { 0.f, 0.f, 0.f }; resolve_object_base_bg(mats, bg);
        float rj[3] = { 0.f, 0.f, 0.f };
        CS::sandwich_colour_stacked(SurfacePassStack::from_json(top_before), SurfacePassStack::from_json(penu_before), mats, bg, ColorStitchPaintPreview::weave_layer_height(), rj);
        // Recetas guardadas con el color viejo (el Pro lo guardaba un cambio por detrás): se corrige.
        if (SurfaceEffectProfile* mp = SurfaceEffectProfileManager::get().find_mut(pid)) {
            const uint32_t fixed = 0xFF000000u | cs_f3_rgb(rj);
            if (mp->preview_argb != fixed) { mp->preview_argb = fixed; m_sticker_overlay_built_for = -1; }
        }
    }
    // s364 — viniendo del editor de un sticker, Pro enseña «Back to sticker» (la edición sigue viva).
    m_pro_from_sticker = m_editing_sticker_idx >= 0;
    m_department = DEP_PRO;
    refresh_selector_palettes();
}

void GLGizmoColorStitchPainter::sticker_colours_changed(ColorStitchSticker& st)
{
    if (!st.colors.empty()) st.profile_id = st.colors.front().profile_id;
    m_sticker_overlay_built_for = -1;
    m_stkc_cand_color = -1;   // las alternativas dependen de la receta actual
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

// ¿La entrada c es un degradado? Con zona (s350) lo dice la zona; sin ella, si su RGB es la media
// de algún degradado del SVG (lo de antes).
static bool cs_color_is_grad(const ColorStitchStickerColor& c, const std::vector<ColorStitch::StickerZone>& zones,
                             const std::map<uint32_t, ColorStitch::StickerGradient>& grads)
{
    if (c.zone >= 0 && c.zone < (int)zones.size()) return zones[size_t(c.zone)].grad;
    return grads.count(c.rgb) > 0;
}

// TRAMOS: bandas seguidas con la misma receta.
struct CsRun { int b0 = 0, b1 = 0, pid = 0; };
static std::vector<CsRun> cs_runs(const std::vector<int>& bands)
{
    std::vector<CsRun> out;
    for (int b = 0; b < (int)bands.size(); ++b) {
        if (!out.empty() && out.back().pid == bands[size_t(b)]) out.back().b1 = b;
        else out.push_back({ b, b, bands[size_t(b)] });
    }
    return out;
}
static int cs_run_of(const std::vector<CsRun>& runs, int band)
{
    for (int r = 0; r < (int)runs.size(); ++r)
        if (band >= runs[size_t(r)].b0 && band <= runs[size_t(r)].b1) return r;
    return runs.empty() ? -1 : 0;
}
// Color objetivo de un tramo [b0, b1] de N bandas: la rampa en su centro.
static uint32_t cs_span_target(const std::vector<uint32_t>& ramp, int b0, int b1, int N)
{
    if (ramp.empty() || N <= 0) return 0;
    const double t = (double(b0) + double(b1) + 1.0) * 0.5 / double(N);
    const int k = int(std::lround(t * double(ramp.size() - 1)));
    return ramp[size_t(std::clamp(k, 0, int(ramp.size()) - 1))];
}
static std::string cs_run_letter(int r)
{
    std::string s;
    do { s.insert(s.begin(), char('A' + r % 26)); r = r / 26 - 1; } while (r >= 0);
    return s;
}

void GLGizmoColorStitchPainter::build_sticker_candidates(const ColorStitchSticker& st, int color, int band)
{
    m_stkc_cands.clear();
    m_stkc_cand_color = color;
    m_stkc_cand_band  = band;
    if (color < 0 || color >= (int)st.colors.size()) return;
    const ColorStitchStickerColor& c = st.colors[size_t(color)];
    const auto& grads = sticker_gradients_cached(st.svg_data);
    const auto zones  = ColorStitch::sticker_svg_zones(st.svg_data);
    const bool grad   = cs_color_is_grad(c, zones, grads);
    uint32_t target = c.rgb;
    if (band >= 0 && !c.band_profiles.empty()) {
        const auto it = grads.find(c.rgb);
        const std::vector<CsRun> runs = cs_runs(c.band_profiles);
        const int r = cs_run_of(runs, band);
        if (it != grads.end() && r >= 0)
            target = cs_span_target(it->second.ramp, runs[size_t(r)].b0, runs[size_t(r)].b1, int(c.band_profiles.size()));
    }
    // Un tramo es un color plano; el color entero con degradado se resuelve como rampa.
    const std::map<uint32_t, ColorStitch::StickerGradient>* g = (band < 0 && grad) ? &grads : nullptr;
    for (int mode = 0; mode <= 1; ++mode) {
        StkCand k;
        k.recipe      = solve_sticker_color(target, mode, g);
        if ((m_solve_bottom ? k.recipe.bottom : k.recipe.top).passes.empty()) continue;   // s350
        k.from_recipe = true;
        k.label       = mode == 0 ? _u8L("Auto: flat") : _u8L("Auto: ColorStitch");
        k.rgb         = cs_f3_rgb(k.recipe.rgb.data());
        k.de          = cs_de_rgb(target, k.recipe.rgb.data());
        m_stkc_cands.push_back(std::move(k));
    }
    // Colores guardados de la paleta, por cercanía.
    std::vector<StkCand> saved;
    for (const SurfaceEffectProfile& p : SurfaceEffectProfileManager::get().list()) {
        if (p.auto_generated) continue;
        // s350 — un sticker bottom sólo puede usar recetas con zona Bottom (y sin Top: su color
        // mostrado es el de la cara de abajo); uno top, recetas con Top.
        if (m_solve_bottom ? (!p.stack_top_json.empty() || !SurfacePassStack::from_json(p.stack_bottom_json).any_effect())
                           : p.stack_top_json.empty()) continue;
        float rgb[3];
        if (!cs_profile_rgb(p.id, rgb)) continue;
        StkCand k;
        k.pid   = p.id;
        k.label = cs_strip_group(p.name);
        k.rgb   = cs_f3_rgb(rgb);
        k.de    = cs_de_rgb(target, rgb);
        saved.push_back(std::move(k));
    }
    std::sort(saved.begin(), saved.end(), [](const StkCand& a, const StkCand& b) { return a.de < b.de; });
    // s350 — sin repetidos: muchas recetas guardadas dan el MISMO color (las de otros stickers);
    // de cada color se queda la primera. Las automáticas tampoco se repiten en la lista.
    {
        std::vector<StkCand> uniq;
        auto same = [](uint32_t a, uint32_t b) {
            const float fb[3] = { ((b >> 16) & 0xFF) / 255.f, ((b >> 8) & 0xFF) / 255.f, (b & 0xFF) / 255.f };
            return cs_de_rgb(a, fb) < 1.f;
        };
        for (StkCand& k : saved) {
            bool dup = false;
            for (const StkCand& q : m_stkc_cands) dup |= same(k.rgb, q.rgb);
            for (const StkCand& q : uniq)         dup |= same(k.rgb, q.rgb);
            if (!dup) uniq.push_back(std::move(k));
        }
        saved = std::move(uniq);
    }
    if (saved.size() > 8) saved.resize(8);
    for (StkCand& k : saved) m_stkc_cands.push_back(std::move(k));
}

// s350 — recalcular TODAS las recetas de un sticker multicolor en `mode` (0 plano, 1 ColorStitch),
// para el lado al que mira (bottom ⇒ recetas de la zona Bottom). Los colores en bandas rehacen sus
// bandas; el resto, una receta (rampa suave si es un degradado, sólo en top).
void GLGizmoColorStitchPainter::recompute_sticker_colours(ColorStitchSticker& st, int mode)
{
    CsBottomSolve bottom_scope(m_solve_bottom, ColorStitch::sticker_faces_down(st));
    const auto grads = sticker_gradients_cached(st.svg_data);   // copia: el bucle crea perfiles
    const auto zones = ColorStitch::sticker_svg_zones(st.svg_data);
    for (ColorStitchStickerColor& c : st.colors) {
        const bool grad = cs_color_is_grad(c, zones, grads);
        if (grad && !c.band_profiles.empty()) {
            std::vector<int> bp = make_band_profiles(c.rgb, grads, mode, st.name + " " + cs_hex(c.rgb));
            if (!bp.empty()) { c.band_profiles = std::move(bp); c.profile_id = c.band_profiles.front(); }
            continue;
        }
        const int pid = profile_from_sticker_recipe(solve_sticker_color(c.rgb, mode, grad ? &grads : nullptr),
                                                    st.name + " " + cs_hex(c.rgb));
        if (pid) { c.profile_id = pid; c.band_profiles.clear(); }
    }
    st.color_mode = mode;
    if (!st.colors.empty()) st.profile_id = st.colors.front().profile_id;
}

// s350 — pestaña PLACE del editor del sticker: lo que antes iba en la fila de la capa (nombre, giro,
// tamaño, Fit) + LADO, ESPEJO y PROPORCIÓN. Todo reconstruye el transform con sticker_compose.
void GLGizmoColorStitchPainter::render_sticker_place_body(ModelObject* mo, ColorStitchSticker& st, int idx)
{
    const float u = neo_u();
    const float row_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;   // s364
    auto snapshot = [](const std::string& what) {
        Plater::TakeSnapshot snap(wxGetApp().plater(), what, UndoRedo::SnapshotType::GizmoAction);
    };
    auto set_pose = [&](const ColorStitch::StickerPose& pose) {
        st.transform = ColorStitch::sticker_compose(pose, sticker_obj_scale(mo));
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    };
    auto reslice = [&]() { m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS)); };
    ColorStitch::StickerPose pose = ColorStitch::sticker_decompose(st.transform);
    // Si no estamos editando ESTE sticker, los deslizadores salen de la pose.
    if (idx != m_editing_sticker_idx) {
        m_editing_spin_deg = float(pose.spin_rad * 180.0 / M_PI);
        m_editing_scale    = float(pose.scale);
    }

    // ---- Nombre ----
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", st.name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputText("##stk_name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && st.name != buf) {
            snapshot(_u8L("Rename sticker"));
            st.name = buf;
        }
    }

    // ---- Lado ----
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_u8L("Side").c_str());
    ImGui::SameLine(cs_label_col());
    int side = pose.bottom ? 1 : 0;
    for (int k = 0; k < 2; ++k) {
        if (k) ImGui::SameLine();
        const std::string lbl = (k ? _u8L("Bottom") : _u8L("Top")) + (k ? "##stk_sideb" : "##stk_sidet");
        if (neo_text_button(lbl.c_str(), side == k ? NeoBtn::Accent : NeoBtn::Ghost, true, k ? Glyph::ZoneBottom : Glyph::ZoneTop)
            && side != k) {
            Vec3d proj;
            if (sticker_project(mo, pose.pos, k == 1, proj)) {
                snapshot(_u8L("Sticker side"));
                pose.pos    = proj;
                pose.bottom = (k == 1);
                set_pose(pose);
                // Las recetas de un lado no valen en el otro (zona Top/Penu ⇄ zona Bottom).
                if (st.is_multicolor()) recompute_sticker_colours(st, st.color_mode);
                sticker_view_side(pose.bottom);
                reslice();
                m_sticker_place_msg.clear();
            } else
                m_sticker_place_msg = k ? _u8L("No flat face facing down under it.") : _u8L("No flat face facing up over it.");
        }
    }
    ImGui::SameLine();
    if (neo_glyph_button("##stk_look", 1.3f * u, Glyph::Eye, false, _u8L("Look at this side").c_str()))
        sticker_view_side(pose.bottom);
    if (!m_sticker_place_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
        ImGui::TextUnformatted(m_sticker_place_msg.c_str());
        ImGui::PopStyleColor();
    }

    // ---- Girar ----
    const std::string spin_fmt = std::string("%.0f") + I18N::translate_utf8("°", "deg");
    if (neo_row_slider("##stk_spin", _u8L("Rotate").c_str(), &m_editing_spin_deg, -180.f, 180.f,
                       spin_fmt.c_str(), _u8L("Rotate it on its face").c_str())) {
        pose.spin_rad = double(m_editing_spin_deg) * M_PI / 180.0;
        set_pose(pose);
    }
    if (ImGui::IsItemActivated()) snapshot(_u8L("Rotate sticker"));
    if (ImGui::IsItemDeactivatedAfterEdit()) reslice();

    // ---- Tamaño (% del objeto) ----
    const double fit = sticker_fit_scale(mo, pose.spin_rad);
    auto apply_scale = [&](double sc) { m_editing_scale = float(sc); pose.scale = sc; set_pose(pose); };
    if (fit > 0.0) {
        float pct = float(100.0 * double(m_editing_scale) / fit);
        if (neo_row_slider("##stk_size", _u8L("Size").c_str(), &pct, 2.f, 400.f, "%.0f%%",
                           _u8L("Size relative to the object:\n"
                                "100% = fits on its face (with a small margin).").c_str(),
                           -1.f, ImGuiSliderFlags_Logarithmic))
            apply_scale(fit * double(std::max(0.5f, pct)) / 100.0);
    } else if (neo_row_slider("##stk_scale", _u8L("Scale").c_str(), &m_editing_scale, 0.01f, 10.0f, "%.3fx",
                              _u8L("Resize the sticker (uniform)").c_str(), -1.f, ImGuiSliderFlags_Logarithmic))
        apply_scale(double(std::max(0.001f, m_editing_scale)));
    if (ImGui::IsItemActivated()) snapshot(_u8L("Scale sticker"));
    if (ImGui::IsItemDeactivatedAfterEdit()) reslice();

    // ---- Botones: Fit · espejo · proporción ----
    ImGui::Dummy(ImVec2(cs_label_col(), 0.f));
    ImGui::SameLine(cs_label_col());
    if (fit > 0.0) {
        if (neo_text_button((_u8L("Fit to object") + "##stk_fit").c_str(), NeoBtn::Ghost)) {
            snapshot(_u8L("Fit sticker"));
            const BoundingBoxf3 obb = mo->raw_mesh_bounding_box();
            Vec3d proj;
            if (sticker_project(mo, Vec3d(obb.center().x(), obb.center().y(), pose.pos.z()), pose.bottom, proj))
                pose.pos = proj;
            apply_scale(fit);
            reslice();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Centre it on the object and size it to 100%\n(fits on its face).").c_str());
        cs_same_line_if_fits(_u8L("Mirror") + " \xe2\x86\x94", false, row_right);
    }
    if (neo_text_button((_u8L("Mirror") + " \xe2\x86\x94##stk_mh").c_str(), pose.mirror ? NeoBtn::Accent : NeoBtn::Ghost)) {
        snapshot(_u8L("Mirror sticker"));
        pose.mirror = !pose.mirror;
        set_pose(pose);
        reslice();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _u8L("Flip left-right").c_str());
    cs_same_line_if_fits(_u8L("Mirror") + " \xe2\x86\x95", false, row_right);
    if (neo_text_button((_u8L("Mirror") + " \xe2\x86\x95##stk_mv").c_str(), NeoBtn::Ghost)) {
        // Arriba-abajo = espejo izquierda-derecha + media vuelta.
        snapshot(_u8L("Mirror sticker"));
        pose.mirror   = !pose.mirror;
        pose.spin_rad = std::remainder(pose.spin_rad + M_PI, 2.0 * M_PI);
        m_editing_spin_deg = float(pose.spin_rad * 180.0 / M_PI);
        set_pose(pose);
        reslice();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _u8L("Flip top-bottom").c_str());

    // ---- Proporción: ¿está deformado en la pieza (escala del objeto)? ----
    {
        const Transform3d world = (mo->instances.empty() ? Transform3d::Identity()
                                                          : mo->instances.front()->get_transformation().get_matrix())
                                * st.transform;
        const double ax = world.linear().col(0).norm(), ay = world.linear().col(1).norm();
        const double dev = std::abs(ax - ay) / std::max(1e-9, 0.5 * (ax + ay));
        if (dev > 0.01) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextUnformatted((_u8L("Stretched") + " " + std::to_string(int(std::lround(dev * 100.0))) + "%").c_str());
            ImGui::PopStyleColor();
            cs_same_line_if_fits(_u8L("Restore proportion"), false, row_right);
            if (neo_text_button((_u8L("Restore proportion") + "##stk_prop").c_str(), NeoBtn::Normal)) {
                snapshot(_u8L("Restore sticker proportion"));
                set_pose(pose);   // sticker_compose compensa la escala del objeto
                reslice();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("The object was scaled unevenly after placing the sticker,\n"
                                             "so the sticker got stretched with it. This gives it back\n"
                                             "its own shape (same average size).").c_str());
        }
    }

    // ---- Color (sticker de 1 color) ----
    if (!st.is_multicolor()) {
        ImGui::Separator();
        auto& mgr = SurfaceEffectProfileManager::get();
        const SurfaceEffectProfile* p = st.profile_id ? mgr.find(st.profile_id) : nullptr;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Colour").c_str());
        ImGui::SameLine(cs_label_col());
        ImGui::TextDisabled("%s", fit_text(p ? cs_strip_group(p->name) : _u8L("(no profile)"),
                                           std::max(2.f * u, row_right - ImGui::GetCursorScreenPos().x)).c_str());
        ImGui::Dummy(ImVec2(cs_label_col(), 0.f));   // el botón, en su línea y en la columna de los mandos
        ImGui::SameLine(cs_label_col());
        if (neo_text_button((_u8L("Use Palette colour") + "##stk_asg1").c_str(), NeoBtn::Ghost, true, Glyph::Target)
            && m_selected_profile_id != 0 && st.profile_id != m_selected_profile_id) {
            snapshot(_u8L("Assign sticker colour"));
            st.profile_id = m_selected_profile_id;
            reslice();
        }
        // s350 — en la cara de abajo sólo cuenta la zona Bottom de la receta.
        if (pose.bottom && p && !SurfacePassStack::from_json(p->stack_bottom_json).any_effect()) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextWrapped("%s", _u8L("This recipe has no Bottom zone: it won't print here.").c_str());
            ImGui::PopStyleColor();
            if (neo_text_button((_u8L("Make a Bottom version") + "##stk_mkbot").c_str(), NeoBtn::Normal)) {
                // El color que da la receta en top, resuelto para la cara de abajo.
                float rgb[3] = { 0.5f, 0.5f, 0.5f };
                const ColorRGBA pc = color_for_profile(*p);
                rgb[0] = pc.r(); rgb[1] = pc.g(); rgb[2] = pc.b();
                CsBottomSolve bottom_scope(m_solve_bottom, true);
                const uint32_t tgt = cs_f3_rgb(rgb);
                const int pid = profile_from_sticker_recipe(solve_sticker_color(tgt, 0, nullptr), cs_strip_group(p->name));
                if (pid) {
                    snapshot(_u8L("Sticker bottom recipe"));
                    st.profile_id = pid;
                    reslice();
                }
            }
        }
    }

    // ---- Aviso: el muro no se recolorea en stickers ----
    {
        bool has_ov = false;
        for (const int pid : ColorStitch::sticker_live_profiles(st))
            if (const SurfaceEffectProfile* pp = SurfaceEffectProfileManager::get().find(pid))
                has_ov |= SurfacePassStack::from_json(pp->stack_top_json).perimeter_override
                       || SurfacePassStack::from_json(pp->stack_penu_json).perimeter_override;
        if (has_ov) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(_u8L("A recipe here recolours the wall. Stickers "
                                        "don't do that yet: the wall keeps its colour.").c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }
    if (idx == m_editing_sticker_idx) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("Drag on the model to move it (it slides on its side's face).").c_str());
        ImGui::PopStyleColor();
    }
}

// s364 — EDITOR DEL STICKER, dentro de la pestaña Stickers y bajo la lista de capas (antes, s350,
// una ventana flotante aparte). Bloque plegable «Sticker: nombre» con las pestañas Place (lado,
// giro, tamaño, espejo, proporción) y Colours (sólo multicolor). Existe mientras hay un sticker
// en edición; se cierra con el mismo botón Edit de su capa.
void GLGizmoColorStitchPainter::render_sticker_editor()
{
    const float u = neo_u();
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (m_editing_sticker_idx < 0) {
        if (mo && !mo->colorstitch_stickers.empty()) {
            ImGui::Dummy(ImVec2(0.f, 0.2f * u));
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextWrapped("%s", _u8L("Press Edit on a layer to move it or change its colours.").c_str());
            ImGui::PopStyleColor();
        }
        return;
    }
    // Otro objeto activo, o la capa ya no existe: se acabó la edición.
    if (!mo || m_editing_sticker_idx >= (int)mo->colorstitch_stickers.size() || m_stkc_obj_id != mo->id().id) {
        exit_sticker_edit();
        return;
    }
    m_stkc_idx = m_editing_sticker_idx;   // reordenar capas mueve el índice en edición
    ColorStitchSticker& st = mo->colorstitch_stickers[size_t(m_stkc_idx)];
    if (!st.is_multicolor()) m_stkc_tab = 0;   // 1 color: sólo Place
    if (m_stkc_sel.size() != st.colors.size()) m_stkc_sel.assign(st.colors.size(), 0);
    m_stkc_cur = std::clamp(m_stkc_cur, 0, std::max(0, int(st.colors.size()) - 1));

    ImGui::Dummy(ImVec2(0.f, 0.2f * u));
    const std::string title = fit_text(_u8L("Sticker") + ": " + (st.name.empty() ? std::string("?") : st.name),
                                       std::max(4.f * u, ImGui::GetContentRegionAvail().x - 4.f * u));
    if (!neo_section_toggle("##stk_editor", title.c_str(), Glyph::Sticker, &m_stkc_block_open,
                            _u8L("The sticker being edited. Click to fold or unfold;\n"
                                 "the Edit button of its layer closes it.").c_str()))
        return;
    // Pestañas: Place y Colours (multicolor).
    if (neo_text_button((_u8L("Place") + "##stkc_tab0").c_str(), m_stkc_tab == 0 ? NeoBtn::Accent : NeoBtn::Ghost))
        m_stkc_tab = 0;
    if (st.is_multicolor()) {
        ImGui::SameLine();
        if (neo_text_button((_u8L("Colours") + "##stkc_tab1").c_str(), m_stkc_tab == 1 ? NeoBtn::Accent : NeoBtn::Ghost))
            m_stkc_tab = 1;
    }
    ImGui::Dummy(ImVec2(0.f, 0.15f * u));
    if (m_stkc_tab == 1 && st.is_multicolor()) {
        m_stkc_colours_shown = true;
        render_sticker_colours_body(st);
    } else
        render_sticker_place_body(mo, st, m_stkc_idx);
}

#ifdef NEOTKO_PRIVATE_TOWER
// 22-oct-2026 — recetas por profundidad de todos los colores del sticker (extensión opcional). Base = el
// filamento cuyo color es el del objeto; los demás filamentos cargados son los de color.
static void recompute_depth_recipes(ColorStitchSticker& st, const Slic3r::ColorSci::Material mats[4], int n_fil,
                                    const float bg[3], double layer_mm)
{
    namespace DR = Slic3r::StickerExt;
    auto close = [](const std::array<float, 3>& a, const float b[3]) {
        return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 0.02f;
    };
    DR::Filament base;
    base.rgb = { bg[0], bg[1], bg[2] };
    std::vector<DR::Filament> colours;
    bool base_found = false;
    for (int i = 0; i < std::min(n_fil, 4); ++i) {
        if (!base_found && close(mats[i].rgb, bg)) { base.tool = i; base.td_mm = mats[i].td[0]; base_found = true; continue; }
        colours.push_back({ i, mats[i].rgb, mats[i].td[0] });
    }
    DR::Options o;
    o.layer_mm     = float(layer_mm);
    o.max_depth_mm = st.depth_max_mm;
    o.fixed_order  = false;   // la torre no preocupa (Sebas, 22-oct-2026): gana el color
    auto solve_into = [&](uint32_t rgb, std::vector<ColorStitchDepthBlock>& out, float* de) {
        const std::array<float, 3> t { ((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f };
        const DR::Result r = DR::solve(t, colours, base, o);
        out.clear();
        for (const DR::Block& b : r.blocks)
            out.push_back({ b.tool, float(b.layers) * o.layer_mm });
        if (de) *de = r.delta_e;
        std::ostringstream os;
        for (const DR::Block& b : r.blocks) os << " T" << b.tool << "x" << b.layers;
        NEOTKO_LOG(PROFILE, "DEPTH_RECIPE sticker='" << st.name << "' rgb=" << std::hex << rgb << std::dec
            << " dE=" << r.delta_e << " mm=" << r.depth_mm << " base=T" << base.tool
            << " colours=" << colours.size() << (r.warning.empty() ? "" : " WARN=" + r.warning)
            << " blocks(abajo->arriba):" << os.str());
    };
    if (st.colors.empty())
        st.depth_blocks.clear();   // sticker de 1 color: sin RGB guardado, de momento sin receta
    for (ColorStitchStickerColor& c : st.colors)
        solve_into(c.rgb, c.depth_blocks, &c.depth_de);
}
#endif

void GLGizmoColorStitchPainter::render_sticker_colours_body(ColorStitchSticker& st)
{
    CsBottomSolve bottom_scope(m_solve_bottom, ColorStitch::sticker_faces_down(st));   // s350
    namespace CSP = Slic3r::GUI::ColorStitchStickerPreview;
    const float u  = neo_u();
    const float gb = 1.3f * u;
    const float fh = ImGui::GetFrameHeight();
    auto& mgr = SurfaceEffectProfileManager::get();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto snapshot = [](const std::string& what) {
        Plater::TakeSnapshot snap(wxGetApp().plater(), what, UndoRedo::SnapshotType::GizmoAction);
    };
    const auto& grads = sticker_gradients_cached(st.svg_data);
    const std::vector<ColorStitch::StickerZone> zones = ColorStitch::sticker_svg_zones(st.svg_data);
    auto ramp_of = [&](const ColorStitchStickerColor& c) -> const std::vector<uint32_t>* {
        if (!cs_color_is_grad(c, zones, grads)) return nullptr;
        const auto it = grads.find(c.rgb);
        return (it == grads.end() || it->second.ramp.size() < 2) ? nullptr : &it->second.ramp;
    };
    auto pid_rgb_u32 = [](int pid) {
        float rgb[3];
        return cs_profile_rgb(pid, rgb) ? cs_rgb_u32(cs_f3_rgb(rgb)) : IM_COL32(128, 128, 128, 255);
    };
    std::vector<std::string> fcolors;
    gizmo_materials(g_stkc_mats, fcolors);
    g_stkc_bg[0] = g_stkc_bg[1] = g_stkc_bg[2] = 0.f;
    resolve_object_base_bg(g_stkc_mats, g_stkc_bg);
    g_stkc_live = true;   // hasta el final del cuerpo: cs_profile_rgb calcula desde las capas
    struct LiveOff { ~LiveOff() { g_stkc_live = false; } } live_off;
    const double lh = GLGizmoColorStitchPainter_layer_height();
    // La tarjeta de la receta (la misma que Palette) dentro de un tooltip, más unas líneas de texto.
    auto card_tip = [&](int pid, const std::string& text) {
        ImGui::BeginTooltip();
        if (const SurfaceEffectProfile* cp = pid ? mgr.find(pid) : nullptr)
            draw_recipe_card(SurfacePassStack::from_json(cp->stack_top_json), SurfacePassStack::from_json(cp->stack_penu_json),
                             SurfacePassStack::from_json(cp->stack_bottom_json), cp->auto_generated);
        if (!text.empty()) ImGui::TextUnformatted(text.c_str());
        ImGui::EndTooltip();
    };
    bool changed = false;
    // s364 — UNA columna, al ancho del panel (antes: lista a la izquierda y detalle a la derecha en
    // una ventana de 46 em). Todo ancho sale de full_w: el panel es AlwaysAutoResize (s319).
    const float full_w    = ImGui::GetContentRegionAvail().x;
    const float gap       = 0.6f * u;
    const float det_w     = full_w;
    const float row_right = ImGui::GetCursorScreenPos().x + full_w;
    // dE de un color entero (media de sus bandas si va en tramos; -1 si es una rampa suave) y nº de tramos.
    auto color_de = [&](const ColorStitchStickerColor& c, int& nruns) {
        nruns = 0;
        const std::vector<uint32_t>* ramp = ramp_of(c);
        float rgb[3];
        if (ramp && !c.band_profiles.empty()) {
            const int N = int(c.band_profiles.size());
            const std::vector<CsRun> runs = cs_runs(c.band_profiles);
            nruns = int(runs.size());
            float sum = 0.f; int nde = 0;
            for (const CsRun& r : runs)
                if (cs_profile_rgb(r.pid, rgb))
                    for (int b = r.b0; b <= r.b1; ++b) { sum += cs_de_rgb(cs_span_target(*ramp, b, b, N), rgb); ++nde; }
            return nde ? sum / float(nde) : -1.f;
        }
        return (!ramp && cs_profile_rgb(c.profile_id, rgb)) ? cs_de_rgb(c.rgb, rgb) : -1.f;
    };
    auto de_text = [&](float de, int nruns, bool ramp, bool banded) {
        std::string t = de >= 0.f ? ("dE " + std::to_string(int(std::lround(de)))) : std::string("dE ~");
        t += banded ? ("  " + std::to_string(nruns) + " " + (nruns == 1 ? _u8L("part") : _u8L("parts")))
                    : (ramp ? "  " + _u8L("smooth") : std::string());
        return t;
    };

    // ======== Miniaturas: lo que pide / lo que sale (clic = elegir color y tramo) ========
    {
        const CSP::Flat2D& f = CSP::flat_2d(st);
        size_t nv = 0;
        for (const auto& t : f.tris) nv += t.size();
        const Vec2f ext = f.bb_max - f.bb_min;
        if (nv >= 3 && ext.x() > 1e-3f && ext.y() > 1e-3f && nv < 24000) {
            const float bw = 0.5f * (full_w - gap) - 4.f, bh = 6.5f * u;
            const float sc = std::min(bw / ext.x(), bh / ext.y());
            const ImVec2 sz(ext.x() * sc, ext.y() * sc);
            const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
            auto dim = [](ImU32 c) {   // los colores no seleccionados, apagados
                const ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
                return ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f * v.x + 0.2f, 0.35f * v.y + 0.2f, 0.35f * v.z + 0.2f, 1.f));
            };
            auto split = [](int cidx, int& ci, int& bi) {
                ci = cidx >= ColorStitch::kBandStride ? cidx % ColorStitch::kBandStride : cidx;
                bi = cidx >= ColorStitch::kBandStride ? cidx / ColorStitch::kBandStride - 1 : -1;
            };
            auto draw = [&](const ImVec2& o, bool want) {
                dl->AddRectFilled(ImVec2(o.x - 2.f, o.y - 2.f), ImVec2(o.x + bw + 2.f, o.y + bh + 2.f), neo_col_u32(NeoCol::Canvas), 3.f);
                const ImVec2 oo(o.x + 0.5f * (bw - sz.x), o.y + 0.5f * (bh - sz.y));
                for (size_t k = 0; k < f.tris.size(); ++k) {
                    int ci, bi; split(f.color_idx[k], ci, bi);
                    if (ci < 0 || ci >= (int)st.colors.size()) continue;
                    const ColorStitchStickerColor& c = st.colors[size_t(ci)];
                    const std::vector<uint32_t>* ramp = want ? ramp_of(c) : nullptr;
                    ImU32 flat = want ? cs_rgb_u32(ramp && bi >= 0 ? cs_span_target(*ramp, bi, bi, int(c.band_profiles.size())) : c.rgb)
                                      : pid_rgb_u32(ColorStitch::sticker_profile_for(st, f.color_idx[k]));
                    const bool off = ci != m_stkc_cur;
                    const bool smooth = want && ramp && bi < 0 && f.grad[k];
                    const Vec2d g0 = f.g0[k], gd = f.g1[k] - f.g0[k];
                    const double gl2 = gd.squaredNorm();
                    const std::vector<Vec2f>& tr = f.tris[k];
                    const int nn = int(tr.size() / 3 * 3);
                    dl->PrimReserve(nn, nn);
                    for (int i = 0; i < nn; ++i) {
                        const Vec2f& p = tr[size_t(i)];
                        ImU32 col = flat;
                        if (smooth && gl2 > 1e-12) {
                            const double t = std::clamp((Vec2d(p.x(), p.y()) - g0).dot(gd) / gl2, 0.0, 1.0);
                            col = cs_rgb_u32((*ramp)[size_t(std::lround(t * double(ramp->size() - 1)))]);
                        }
                        if (off) col = dim(col);
                        dl->PrimVtx(ImVec2(oo.x + (p.x() - f.bb_min.x()) * sc, oo.y + (f.bb_max.y() - p.y()) * sc), uv, col);
                    }
                }
                return oo;
            };
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::TextDisabled("%s", _u8L("The SVG asks for").c_str());
            ImGui::SameLine(bw + gap + 4.f);
            ImGui::TextDisabled("%s", _u8L("Your filaments give").c_str());
            const float ly = ImGui::GetCursorScreenPos().y + 2.f;
            const ImVec2 oa = draw(ImVec2(p0.x + 2.f, ly), true);
            const ImVec2 ob = draw(ImVec2(p0.x + bw + gap + 4.f, ly), false);
            ImGui::InvisibleButton("##stkc_thumbs", ImVec2(full_w, bh + 4.f));
            if (ImGui::IsItemHovered()) {
                // ¿Qué pieza hay bajo el ratón? (en cualquiera de las dos)
                const ImVec2 m = ImGui::GetIO().MousePos;
                const ImVec2 o = (m.x >= ob.x - 2.f) ? ob : oa;
                const Vec2f q(f.bb_min.x() + (m.x - o.x) / sc, f.bb_max.y() - (m.y - o.y) / sc);
                int hit = -1;
                for (size_t k = 0; k < f.tris.size() && hit < 0; ++k) {
                    const std::vector<Vec2f>& tr = f.tris[k];
                    for (size_t i = 0; i + 2 < tr.size(); i += 3) {
                        auto cr = [](const Vec2f& a, const Vec2f& b, const Vec2f& c) {
                            return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()); };
                        const float d1 = cr(tr[i], tr[i + 1], q), d2 = cr(tr[i + 1], tr[i + 2], q), d3 = cr(tr[i + 2], tr[i], q);
                        if (!(((d1 < 0) || (d2 < 0) || (d3 < 0)) && ((d1 > 0) || (d2 > 0) || (d3 > 0)))) { hit = int(k); break; }
                    }
                }
                if (hit >= 0) {
                    int ci, bi; split(f.color_idx[size_t(hit)], ci, bi);
                    if (ci >= 0 && ci < (int)st.colors.size()) {
                        ImGui::SetTooltip("%s", (cs_hex(st.colors[size_t(ci)].rgb) + "\n" + _u8L("Click: show this colour")).c_str());
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { m_stkc_cur = ci; m_stkc_band = bi; }
                    }
                }
            }
        } else if (nv >= 24000) {
            ImGui::TextDisabled("%s", _u8L("Too detailed to draw here (the print is not affected).").c_str());
        }
    }
    ImGui::Separator();

    // ======== Los colores: una fila de cuadritos (arriba lo que pide, abajo lo que sale) ========
    {
        const float cw = 2.4f * u, ch = fh, cg = 0.3f * u;
        const float x_left = ImGui::GetCursorScreenPos().x;
        float x = x_left;
        for (size_t j = 0; j < st.colors.size(); ++j) {
            ColorStitchStickerColor& c = st.colors[j];
            ImGui::PushID(int(3000 + j));
            if (j > 0) {
                if (x + cg + cw <= x_left + full_w) { ImGui::SameLine(0.f, cg); x += cg; }
                else                                x = x_left;
            }
            x += cw;
            const std::vector<uint32_t>* ramp = ramp_of(c);
            const bool banded = ramp && !c.band_profiles.empty();
            const bool is_cur = int(j) == m_stkc_cur, marked = m_stkc_sel[j] != 0;
            const ImVec2 r0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##chip", ImVec2(cw, ch))) {
                if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) m_stkc_sel[j] = marked ? 0 : 1;
                else { m_stkc_cur = int(j); m_stkc_band = -1; }
            }
            const bool hv = ImGui::IsItemHovered();
            const ImVec2 a(r0.x + 2.f, r0.y + 2.f), z(r0.x + cw - 2.f, r0.y + ch - 2.f);
            const float  sw = z.x - a.x, my = 0.5f * (a.y + z.y);
            if (ramp) {
                const int M = 12;
                for (int k = 0; k < M; ++k)
                    dl->AddRectFilled(ImVec2(a.x + sw * k / M, a.y), ImVec2(a.x + sw * (k + 1) / M, my),
                                      cs_rgb_u32((*ramp)[size_t(k * (ramp->size() - 1) / (M - 1))]));
            } else
                dl->AddRectFilled(a, ImVec2(z.x, my), cs_rgb_u32(c.rgb));
            if (banded) {
                const int N = int(c.band_profiles.size());
                for (const CsRun& r : cs_runs(c.band_profiles))
                    dl->AddRectFilled(ImVec2(a.x + sw * r.b0 / N, my), ImVec2(a.x + sw * (r.b1 + 1) / N, z.y), pid_rgb_u32(r.pid));
            } else
                dl->AddRectFilled(ImVec2(a.x, my), z, pid_rgb_u32(c.profile_id));
            if (c.hidden) dl->AddRectFilled(a, z, IM_COL32(0, 0, 0, 150));
            int nruns = 0;
            const float de = color_de(c, nruns);
            if (de > 10.f) {   // se aleja mucho de lo pedido
                dl->AddCircleFilled(ImVec2(z.x - 0.25f * u, a.y + 0.25f * u), 0.24f * u, IM_COL32(0, 0, 0, 200));
                dl->AddCircleFilled(ImVec2(z.x - 0.25f * u, a.y + 0.25f * u), 0.17f * u, neo_col_u32(NeoCol::Warn));
            }
            if (is_cur)      dl->AddRect(r0, ImVec2(r0.x + cw, r0.y + ch), neo_col_u32(NeoCol::Ink), 3.f, 0, 2.f);
            else if (marked) dl->AddRect(r0, ImVec2(r0.x + cw, r0.y + ch), neo_col_u32(NeoCol::AccentBright), 3.f, 0, 2.f);
            else if (hv)     dl->AddRect(r0, ImVec2(r0.x + cw, r0.y + ch), neo_fade(NeoCol::Ink, 0.5f), 3.f);
            if (hv) {
                const std::string t = cs_hex(c.rgb) + "   " + de_text(de, nruns, ramp != nullptr, banded) + (c.hidden ? "   (" + _u8L("hidden") + ")" : std::string())
                                    + "\n" + _u8L("Top: what the SVG asks for. Bottom: what prints.\n"
                                                  "dE: how far apart (under 3 hard to tell, over 10 clearly different).\n"
                                                  "Ctrl/Shift-click: mark several");
                if (banded) ImGui::SetTooltip("%s", t.c_str());   // varias recetas: se ven por tramo, abajo
                else        card_tip(c.profile_id, t);
            }
            ImGui::PopID();
        }
    }
    ImGui::Dummy(ImVec2(0.f, 0.1f * u));

    // ======== El detalle del color elegido ========
    {
        ColorStitchStickerColor& c = st.colors[size_t(m_stkc_cur)];
        const std::vector<uint32_t>* ramp = ramp_of(c);
        const bool banded = ramp && !c.band_profiles.empty();
        const int  N      = int(c.band_profiles.size());
        std::vector<CsRun> runs = banded ? cs_runs(c.band_profiles) : std::vector<CsRun>{};
        if (banded && (m_stkc_band < 0 || m_stkc_band >= N)) m_stkc_band = 0;
        const int  ri     = banded ? cs_run_of(runs, m_stkc_band) : -1;
        const int  sel_band = banded ? m_stkc_band : -1;
        m_stkc_focus_b0 = (banded && ri >= 0) ? runs[size_t(ri)].b0 : -1;   // s350: resaltar en 3D
        m_stkc_focus_b1 = (banded && ri >= 0) ? runs[size_t(ri)].b1 : -1;

        // Cabecera: ojo · #hex · qué es · dE
        if (neo_glyph_button("##stkc_eye", gb, Glyph::Eye, false,
                             c.hidden ? _u8L("Hidden: this colour is not printed,\nwhat is underneath shows").c_str()
                                      : _u8L("Visible: click to hide this colour").c_str(),
                             NeoCol::AccentBright, !c.hidden)) {
            snapshot(_u8L("Toggle sticker colour"));
            c.hidden = !c.hidden;
            changed = true;
        }
        ImGui::SameLine(0.f, 0.4f * u);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cs_hex(c.rgb).c_str());
        ImGui::SameLine();
        {
            int   nr = 0;
            const float de = color_de(c, nr);
            const std::string what = (ramp ? (banded ? _u8L("gradient in flat parts") : _u8L("gradient, smooth ColorStitch ramp"))
                                           : _u8L("flat colour"))
                                   + "   " + de_text(de, nr, false, false);
            ImGui::PushStyleColor(ImGuiCol_Text, de > 10.f ? neo_col(NeoCol::Warn) : neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(fit_text(what, std::max(2.f * u, row_right - ImGui::GetCursorScreenPos().x)).c_str());
            ImGui::PopStyleColor();
        }
        if (ramp) {
            // Dos tiras: la rampa que pide el SVG y, debajo, los TRAMOS (clic = elegir tramo).
            const float lw = 3.5f * u, tw = det_w - lw, th = 0.9f * u;
            const ImVec2 q0 = ImGui::GetCursorScreenPos();
            dl->AddText(ImVec2(q0.x, q0.y), neo_col_u32(NeoCol::TextDim), _u8L("Asks").c_str());
            const int M = 48;
            for (int k = 0; k < M; ++k)
                dl->AddRectFilled(ImVec2(q0.x + lw + tw * k / M, q0.y), ImVec2(q0.x + lw + tw * (k + 1) / M, q0.y + th),
                                  cs_rgb_u32((*ramp)[size_t(k * (ramp->size() - 1) / (M - 1))]));
            const float y1 = q0.y + th + 2.f;
            dl->AddText(ImVec2(q0.x, y1 + 0.2f * u), neo_col_u32(NeoCol::TextDim), _u8L("Gives").c_str());
            if (banded) {
                for (int r = 0; r < (int)runs.size(); ++r) {
                    const CsRun& rr = runs[size_t(r)];
                    const ImVec2 b0(q0.x + lw + tw * rr.b0 / N, y1), b1(q0.x + lw + tw * (rr.b1 + 1) / N, y1 + 1.4f * th);
                    dl->AddRectFilled(b0, b1, pid_rgb_u32(rr.pid));
                    const std::string L = cs_run_letter(r);
                    const ImVec2 ts = ImGui::CalcTextSize(L.c_str());
                    if (b1.x - b0.x > ts.x + 4.f) {
                        dl->AddText(ImVec2(0.5f * (b0.x + b1.x - ts.x) + 1.f, 0.5f * (b0.y + b1.y - ts.y) + 1.f), IM_COL32(0, 0, 0, 160), L.c_str());
                        dl->AddText(ImVec2(0.5f * (b0.x + b1.x - ts.x), 0.5f * (b0.y + b1.y - ts.y)), IM_COL32(255, 255, 255, 230), L.c_str());
                    }
                    if (r == ri) dl->AddRect(ImVec2(b0.x - 1.f, b0.y - 1.f), ImVec2(b1.x + 1.f, b1.y + 1.f), IM_COL32(255, 255, 255, 255), 2.f, 0, 2.f);
                    if (ImGui::IsMouseHoveringRect(b0, b1)) {
                        dl->AddRect(b0, b1, IM_COL32(255, 255, 255, 140));
                        float rgb[3];
                        std::string tip = _u8L("Part") + " " + L + "  (" + _u8L("bands") + " " + std::to_string(rr.b0 + 1)
                                        + "-" + std::to_string(rr.b1 + 1) + ")";
                        if (cs_profile_rgb(rr.pid, rgb))
                            tip += "  dE " + std::to_string(int(std::lround(cs_de_rgb(cs_span_target(*ramp, rr.b0, rr.b1, N), rgb))));
                        card_tip(rr.pid, tip + "\n" + _u8L("Click: show this part"));
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_stkc_band = rr.b0;
                    }
                }
            } else {
                dl->AddRectFilled(ImVec2(q0.x + lw, y1), ImVec2(q0.x + lw + tw, y1 + 1.4f * th), pid_rgb_u32(c.profile_id));
            }
            ImGui::Dummy(ImVec2(det_w, th + 2.f + 1.4f * th + 2.f));
            // Mandos del degradado.
            if (neo_text_button((banded ? _u8L("Make it a smooth ramp") : _u8L("Make it flat parts")).append("##stkc_mode").c_str(), NeoBtn::Ghost)) {
                snapshot(_u8L("Sticker gradient mode"));
                if (banded) {
                    const int pid = profile_from_sticker_recipe(solve_sticker_color(c.rgb, 1, &grads), st.name + " " + cs_hex(c.rgb));
                    if (pid) { c.band_profiles.clear(); c.profile_id = pid; }
                } else {
                    std::vector<int> bp = make_band_profiles(c.rgb, grads, st.color_mode, st.name + " " + cs_hex(c.rgb));
                    if (!bp.empty()) { c.band_profiles = std::move(bp); c.profile_id = c.band_profiles.front(); }
                }
                m_stkc_band = -1;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", (banded ? _u8L("One ColorStitch ramp between two filaments, no steps.")
                                                : _u8L("Cut into bands, each with the closest recipe;\n"
                                                       "neighbours with the same recipe make one part.")).c_str());
            if (banded && ri >= 0) {
                // (Sin disabled_begin: el departamento ya tiene uno abierto y no se anida; se atenúa
                // con Alpha y se veta el clic.)
                const CsRun rr = runs[size_t(ri)];
                cs_same_line_if_fits(_u8L("Split"), false, row_right);
                const bool can_split = rr.b1 > rr.b0;
                if (!can_split) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
                const bool split_clicked = neo_text_button((_u8L("Split") + "##stkc_split").c_str(), NeoBtn::Ghost);
                if (!can_split) ImGui::PopStyleVar();
                if (split_clicked && can_split) {
                    // La 2.ª mitad se resuelve aparte, con su propio color objetivo.
                    const int mid = (rr.b0 + rr.b1 + 1) / 2;
                    const uint32_t tgt = cs_span_target(*ramp, mid, rr.b1, N);
                    int pid = 0;
                    for (int mode : { st.color_mode, 1 - st.color_mode }) {
                        pid = profile_from_sticker_recipe(solve_sticker_color(tgt, mode, nullptr),
                                                          st.name + " " + cs_hex(c.rgb) + " b" + std::to_string(mid + 1) + " " + cs_hex(tgt));
                        if (pid && pid != rr.pid) break;
                    }
                    NEOTKO_LOG(PROFILE, "STICKER_PART split color=" << m_stkc_cur << " bands=" << rr.b0 << "-" << rr.b1
                        << " mid=" << mid << " pid=" << rr.pid << "->" << pid);
                    if (pid && pid != rr.pid) {
                        snapshot(_u8L("Split sticker part"));
                        for (int b = mid; b <= rr.b1; ++b) c.band_profiles[size_t(b)] = pid;
                        c.profile_id = c.band_profiles.front();
                        m_stkc_band = mid;
                        changed = true;
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Cut this part in two: the second half gets\n"
                                                 "its own recipe for its own colour.").c_str());
                cs_same_line_if_fits(_u8L("Join with next"), false, row_right);
                const bool can_join = ri + 1 < (int)runs.size();
                if (!can_join) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
                const bool join_clicked = neo_text_button((_u8L("Join with next") + "##stkc_join").c_str(), NeoBtn::Ghost);
                if (!can_join) ImGui::PopStyleVar();
                if (join_clicked && can_join) {
                    snapshot(_u8L("Join sticker parts"));
                    const CsRun nx = runs[size_t(ri + 1)];
                    for (int b = nx.b0; b <= nx.b1; ++b) c.band_profiles[size_t(b)] = rr.pid;
                    c.profile_id = c.band_profiles.front();
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("The next part takes this part's recipe\n(one recipe less).").c_str());
            }
        }

        // ---- La receta de la selección, como en el Pro ----
        const int sel_pid = (banded && ri >= 0) ? runs[size_t(ri)].pid : c.profile_id;
        ImGui::Dummy(ImVec2(0.f, 0.2f * u));
        ImGui::TextDisabled("%s", (banded && ri >= 0 ? (_u8L("Recipe of part") + " " + cs_run_letter(ri)) : _u8L("Recipe")).c_str());
        const SurfaceEffectProfile* sp = sel_pid ? mgr.find(sel_pid) : nullptr;
        if (sp) {
            const SurfacePassStack top  = SurfacePassStack::from_json(sp->stack_top_json);
            const SurfacePassStack penu = SurfacePassStack::from_json(sp->stack_penu_json);
            const SurfacePassStack bot  = SurfacePassStack::from_json(sp->stack_bottom_json);
            float out[3] = { 0.5f, 0.5f, 0.5f };
            cs_profile_rgb(sel_pid, out);   // desde las capas, como el Pro
            draw_pro_hero(top, penu, bot, out, fcolors, lh, det_w, g_stkc_mats, g_stkc_bg);
            std::string desc = cs_strip_group(sp->name) + ":  Top " + zone_desc(top);
            if (!penu.passes.empty()) desc += " / Penu " + zone_desc(penu);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + det_w);
            ImGui::TextDisabled("%s", desc.c_str());
            ImGui::PopTextWrapPos();
        } else
            ImGui::TextDisabled("%s", _u8L("(no recipe: this colour is not printed)").c_str());

        if (neo_text_button((_u8L("Edit in Pro") + "##stkc_pro").c_str(), NeoBtn::Normal, true, Glyph::Edit) && sp)
            edit_profile_in_pro(sel_pid);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Open this recipe in Pro. The sticker changes as you edit\n"
                                         "(copies of this sticker share it).").c_str());
        int nmark = 0;
        for (const char s : m_stkc_sel) nmark += s ? 1 : 0;
        const bool can_use = m_selected_profile_id != 0;
        const std::string use_lbl = (nmark > 0 ? _u8L("Use Palette colour on marked") + " (" + std::to_string(nmark) + ")"
                                               : _u8L("Use Palette colour")) + "##stkc_use";
        cs_same_line_if_fits(use_lbl, true, row_right);
        if (!can_use) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
        const bool use_clicked = neo_text_button(use_lbl.c_str(), NeoBtn::Ghost, true, Glyph::Target);
        if (!can_use) ImGui::PopStyleVar();
        if (use_clicked && can_use) {
            snapshot(_u8L("Assign sticker colour"));
            if (nmark > 0) {
                for (size_t j = 0; j < st.colors.size(); ++j)
                    if (m_stkc_sel[j]) { st.colors[j].profile_id = m_selected_profile_id; st.colors[j].band_profiles.clear(); }
            } else if (banded && ri >= 0) {
                for (int b = runs[size_t(ri)].b0; b <= runs[size_t(ri)].b1; ++b) c.band_profiles[size_t(b)] = m_selected_profile_id;
                c.profile_id = c.band_profiles.front();
            } else {
                c.profile_id = m_selected_profile_id;
                c.band_profiles.clear();
            }
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("The colour selected in Palette goes to this part\n"
                                         "(or to the marked colours, Ctrl/Shift-click in the list).").c_str());

        // ---- Alternativas (plegadas de entrada: alargan el panel; al pasar por encima se ve su receta).
        // Sólo se calculan con la sección abierta.
        const bool alts_open = neo_section_toggle("##stkc_alts", _u8L("Other recipes, closest first").c_str(),
                                                  Glyph::TabPalette, &m_stkc_alts_open);
        if (alts_open && (m_stkc_cand_color != m_stkc_cur || m_stkc_cand_band != sel_band))
            build_sticker_candidates(st, m_stkc_cur, sel_band);
        const float sq = 0.9f * u;
        for (size_t k = 0; alts_open && k < m_stkc_cands.size(); ++k) {
            const StkCand& cand = m_stkc_cands[k];
            if (k > 0 && !cand.from_recipe && m_stkc_cands[k - 1].from_recipe) ImGui::Separator();
            ImGui::PushID(int(k));
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const bool now = !cand.from_recipe && cand.pid == sel_pid;
            const std::string lbl = "      " + fit_text(cand.label, det_w - 6.f * u) + "  dE " + std::to_string(int(std::lround(cand.de)))
                                  + (now ? "  (" + _u8L("now") + ")" : std::string());
            if (ImGui::Selectable(lbl.c_str(), now, 0, ImVec2(det_w, 0.f))) {
                const uint32_t tgt = cand.rgb;
                const int pid = cand.from_recipe
                    ? profile_from_sticker_recipe(cand.recipe, st.name + " " + cs_hex(c.rgb)
                                                  + (banded && ri >= 0 ? " b" + std::to_string(runs[size_t(ri)].b0 + 1) + " " + cs_hex(tgt) : std::string()))
                    : cand.pid;
                if (pid && pid != sel_pid) {
                    snapshot(_u8L("Sticker colour recipe"));
                    if (banded && ri >= 0) {
                        for (int b = runs[size_t(ri)].b0; b <= runs[size_t(ri)].b1; ++b) c.band_profiles[size_t(b)] = pid;
                        c.profile_id = c.band_profiles.front();
                    } else {
                        c.band_profiles.clear();
                        c.profile_id = pid;
                    }
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                if (cand.from_recipe) draw_recipe_card(cand.recipe.top, cand.recipe.penu, cand.recipe.bottom, false);   // s350: + bottom
                else if (const SurfaceEffectProfile* cp = mgr.find(cand.pid))
                    draw_recipe_card(SurfacePassStack::from_json(cp->stack_top_json), SurfacePassStack::from_json(cp->stack_penu_json),
                                     SurfacePassStack::from_json(cp->stack_bottom_json), cp->auto_generated);
                ImGui::EndTooltip();
            }
            dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(q.x + 2.f, q.y + 1.f), ImVec2(q.x + 2.f + sq, q.y + 1.f + sq), cs_rgb_u32(cand.rgb), 2.f);
            ImGui::PopID();
        }
    }

    // ======== Abajo: recuento, presupuesto, regenerar ========
    ImGui::Separator();
    std::map<int, int> uses;
    for (const ColorStitchStickerColor& c : st.colors) {
        if (c.hidden) continue;
        if (c.band_profiles.empty()) { if (c.profile_id) ++uses[c.profile_id]; }
        else for (const int bp : c.band_profiles) if (bp) ++uses[bp];
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, (int)uses.size() > m_stkc_budget ? neo_col(NeoCol::Warn) : neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted((std::to_string(uses.size()) + " " + _u8L("recipes in use")).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Each recipe is its own set of passes on the top layers:\n"
                                     "more recipes = more tool changes and more purge.").c_str());
    ImGui::SameLine(0.f, u);
    ImGui::SetNextItemWidth(5.f * u);
    ImGui::SliderInt("##stkc_budget", &m_stkc_budget, 1, 24, "max %d");
    ImGui::SameLine();
    const bool can_reduce = (int)uses.size() > m_stkc_budget;
    if (!can_reduce) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
    const bool reduce_clicked = neo_text_button((_u8L("Reduce") + "##stkc_reduce").c_str(), NeoBtn::Ghost);
    if (!can_reduce) ImGui::PopStyleVar();
    if (reduce_clicked && can_reduce) {
        // Juntar, de dos en dos, las dos recetas más parecidas (ΔE entre lo que imprimen); se queda
        // la que más se usa. Hasta caber en el máximo.
        snapshot(_u8L("Reduce sticker recipes"));
        std::map<int, int> u2 = uses;
        int merged = 0;
        while ((int)u2.size() > m_stkc_budget) {
            int ka = 0, kb = 0; float best = std::numeric_limits<float>::max();
            for (auto ia = u2.begin(); ia != u2.end(); ++ia) {
                float ra[3];
                if (!cs_profile_rgb(ia->first, ra)) continue;
                for (auto ib = std::next(ia); ib != u2.end(); ++ib) {
                    float rb[3];
                    if (!cs_profile_rgb(ib->first, rb)) continue;
                    const float d = cs_de_rgb(cs_f3_rgb(ra), rb);
                    if (d < best) { best = d; ka = ia->first; kb = ib->first; }
                }
            }
            if (ka == 0) break;
            const int keep = u2[ka] >= u2[kb] ? ka : kb, drop = keep == ka ? kb : ka;
            for (ColorStitchStickerColor& c : st.colors) {
                if (c.profile_id == drop) c.profile_id = keep;
                for (int& bp : c.band_profiles) if (bp == drop) bp = keep;
            }
            u2[keep] += u2[drop];
            u2.erase(drop);
            ++merged;
        }
        NEOTKO_LOG(PROFILE, "STICKER_BUDGET name='" << st.name << "' before=" << uses.size()
            << " after=" << u2.size() << " merged=" << merged);
        changed = merged > 0;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Merge the two most alike recipes, again and again,\n"
                                     "until there are no more than the max.").c_str());
    // (segunda línea, s364: en una sola no cabe en el panel)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", _u8L("Recompute all:").c_str());
    ImGui::SameLine();
    int mode = -1;
    bool depth_recompute_all = false;   // 23-oct-2026: Recompute all también rehace las recetas por profundidad
    if (neo_text_button((_u8L("Flat") + "##stkc_flat").c_str(), st.color_mode == 0 ? NeoBtn::Normal : NeoBtn::Ghost)) mode = 0;
    ImGui::SameLine();
    if (neo_text_button((_u8L("ColorStitch") + "##stkc_cs").c_str(), st.color_mode == 1 ? NeoBtn::Normal : NeoBtn::Ghost)) mode = 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(5.f * u);
    ImGui::SliderInt("##stkc_gbn", &m_grad_band_count, 4, 32, "%d bands");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Bands per gradient when recomputing\n(neighbours with the same recipe join into one part)").c_str());
    if (mode >= 0)
        depth_recompute_all = true;
    if (mode >= 0) {
        snapshot(_u8L("Recompute sticker colours"));
        recompute_sticker_colours(st, mode);
        m_stkc_band = -1;
        changed = true;
    }

#ifdef NEOTKO_PRIVATE_TOWER
    // 22-oct-2026 — modo profundidad (extensión opcional del build): capas enteras bajo la cara vista.
    {
        bool dm = st.depth_mode;
        auto recompute = [&]() {
            Slic3r::ColorSci::Material dm_mats[4];
            std::vector<std::string> dm_fc;
            gizmo_materials(dm_mats, dm_fc);
            float dm_bg[3] = { 1.f, 1.f, 1.f };
            resolve_object_base_bg(dm_mats, dm_bg);
            recompute_depth_recipes(st, dm_mats, int(dm_fc.size()), dm_bg, GLGizmoColorStitchPainter_layer_height());
        };
        if (st.depth_mode && depth_recompute_all) {
            recompute();
            changed = true;
        }
        if (ImGui::Checkbox((_u8L("Depth recipe") + "##stk_depth").c_str(), &dm)) {
            snapshot(_u8L("Sticker depth recipe"));
            st.depth_mode = dm;
            if (dm) recompute();
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Build each colour from whole layers behind the sticker,\n"
                                         "as deep as it needs (up to Max depth).\n"
                                         "The infill behind the sticker turns solid to that depth:\n"
                                         "downwards for a sticker on top, upwards for one on the bottom.").c_str());
        if (st.depth_mode) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(6.f * u);
            float mx = st.depth_max_mm;
            if (ImGui::InputFloat("##stk_depth_max", &mx, 0.2f, 1.f, "%.1f mm")) {
                mx = std::clamp(mx, 0.2f, 10.f);
                if (mx != st.depth_max_mm) {
                    snapshot(_u8L("Sticker max depth"));
                    st.depth_max_mm = mx;
                    recompute();
                    changed = true;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Max depth: the deepest the recipe may go.\n"
                                             "Never deeper than the material behind the sticker.").c_str());
        }
        if (st.depth_mode && !st.colors.empty()) {
            // Por color: mm usados y ΔE predicho (modelo RealView Ray sin difusión).
            std::ostringstream os;
            os.setf(std::ios::fixed);
            os.precision(1);
            for (size_t i = 0; i < st.colors.size(); ++i) {
                float mm = 0.f;
                for (const ColorStitchDepthBlock& b : st.colors[i].depth_blocks) mm += b.mm;
                os << (i ? "  " : "") << cs_hex(st.colors[i].rgb) << " " << mm << "mm dE" << st.colors[i].depth_de;
            }
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("%s", os.str().c_str());
            ImGui::PopTextWrapPos();
        }
    }
#endif

    if (changed) sticker_colours_changed(st);
}

// NEOTKO_STICKER_TAG — s348: pestaña Stickers (antes, sección plegable al final de Palette).
// Arriba "Importar" (cargar SVG + qué color va a llevar), abajo las CAPAS del objeto activo
// (orden = pila, back()=tope, mostradas TOP-DOWN: la primera fila es la que OCLUYE a las de abajo).
// Toda mutación toma snapshot de undo ANTES de cambiar (antes no había ninguno).
void GLGizmoColorStitchPainter::render_stickers_department()
{
    const float u = neo_u();

    // ---- Importar ---- (s350: el botón «Load SVG…» vive junto a la herramienta Sticker; aquí sólo
    // las opciones del SVG cargado, y sólo mientras hay uno)
    if (!m_pending_sticker_svg.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
        ImGui::TextUnformatted((_u8L("Loaded:") + " " + m_pending_sticker_name).c_str());
        ImGui::PopStyleColor();
        // s349 — soltar el SVG cargado: sin esto se quedaba "pegado" y cada clic ponía otra copia.
        ImGui::SameLine(0.f, 0.4f * u);
        if (neo_glyph_button("##stk_unload", 1.3f * u, Glyph::Trash, false,
                             _u8L("Unload this SVG (stickers already placed stay)").c_str())) {
            m_pending_sticker_svg.clear();
            m_pending_sticker_svg_raw.clear();
            m_pending_flatten = {};
            m_pending_sticker_name.clear();
            m_pending_svg_colors.clear();
            m_pending_svg_grad.clear();
            m_pending_recipes.clear();
        }
        // s350 — LADO del sticker que se va a pegar. Cambia la cámara a ese lado; el clic se proyecta
        // a la cara de ese lado en la misma vertical (se puede pinchar el objeto desde donde sea).
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(_u8L("Side").c_str());
        ImGui::SameLine();
        for (int k = 0; k < 2; ++k) {
            if (k) ImGui::SameLine();
            const std::string lbl = (k ? _u8L("Bottom") : _u8L("Top")) + (k ? "##stk_pb" : "##stk_pt");
            if (neo_text_button(lbl.c_str(), int(m_place_bottom) == k ? NeoBtn::Accent : NeoBtn::Ghost, true,
                                k ? Glyph::ZoneBottom : Glyph::ZoneTop)
                && int(m_place_bottom) != k) {
                m_place_bottom = (k == 1);
                m_sticker_place_msg.clear();
                sticker_view_side(m_place_bottom);
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", (m_sticker_mode
            ? (m_place_bottom ? _u8L("Click the object: the sticker goes on the face underneath at that spot. Each click places a copy.")
                              : _u8L("Click the object: the sticker goes on the top face at that spot. Each click places a copy."))
            : _u8L("Pick the Sticker tool, then click the object to place it.")).c_str());
        ImGui::PopStyleColor();
        if (!m_sticker_place_msg.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextWrapped("%s", m_sticker_place_msg.c_str());
            ImGui::PopStyleColor();
        }
        // s348 — modo de importación: colores del SVG (una receta por color) o 1 color (el activo).
        // s349 — lo que hizo el aplanado, y la opción de conservar el fondo.
        if (m_pending_flatten.background_dropped || m_sticker_keep_background) {
            if (ImGui::Checkbox((_u8L("Keep background") + "##stk_bg").c_str(), &m_sticker_keep_background))
                prepare_pending_sticker();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("This SVG has a shape covering the whole drawing\n"
                                             "(a background). Off: it is removed, so the sticker\n"
                                             "is only the drawing. On: it is printed too.").c_str());
        }
        if (m_pending_flatten.translucent > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextWrapped("%s", (std::to_string(m_pending_flatten.translucent) + " "
                + _u8L("see-through shapes were merged into solid colours (a print can't be transparent).")).c_str());
            ImGui::PopStyleColor();
        }
        if (!m_pending_gradients.empty()) {   // s349 — cómo van los degradados del SVG
            ImGui::Checkbox((_u8L("Gradients as bands") + "##stk_gb").c_str(), &m_import_grad_bands);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("On: each gradient is cut into flat bands, each one with\n"
                                             "the closest recipe your filaments can make.\n"
                                             "Off: one smooth ColorStitch ramp between filaments.").c_str());
            if (m_import_grad_bands) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(6.f * u);
                ImGui::SliderInt("##stk_gbn", &m_grad_band_count, 4, 32, "%d bands");
            }
        }
        if (ImGui::Checkbox((_u8L("Use the SVG colours") + "##stk_mc").c_str(), &m_import_multicolor)) {}
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("On: every colour of the SVG gets its own recipe,\n"
                                         "made from your 4 filaments.\n"
                                         "Off: the whole SVG is one colour (the one selected in Palette).").c_str());
        if (m_import_multicolor) {
            ImGui::SameLine(0.f, u);
            int mode = m_import_color_mode;
            ImGui::RadioButton((_u8L("Flat") + "##stk_m0").c_str(), &mode, 0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Solid passes stacked in Z (1-2 colours seen\n"
                                             "through each other). Best match, top faces only.").c_str());
            ImGui::SameLine();
            ImGui::RadioButton((_u8L("ColorStitch") + "##stk_m1").c_str(), &mode, 1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Two filaments alternated line by line\n"
                                             "in ONE pass (dither). Also works on bridges.").c_str());
            if (mode != m_import_color_mode) { m_import_color_mode = mode; solve_pending_sticker_recipes(); }

            // s350 — el listado de colores se quitó de aquí: vive en la pestaña Colours del editor
            // (al editar el sticker), con la receta completa y para afinarla.
            if (m_pending_svg_colors.empty())
                tray(_u8L("No fill colours found in this SVG."));
        } else if (m_selected_profile_id == 0) {
            // s231 F6 — un sticker de 1 color hereda el color ACTIVO al colocarse.
            tray(_u8L("No colour selected: the sticker will be placed without a recipe. "
                      "Pick a saved colour in Palette first (or assign one afterwards with "
                      "\"Assign active\")."));
        }
    }

    ImGui::Dummy(ImVec2(0.f, 0.3f * u));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.f, 0.2f * u));

    // ---- Capas ----
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(_u8L("Layers (top covers the ones below)").c_str());
    ImGui::PopStyleColor();
    render_sticker_section();
    render_sticker_editor();   // s364 — los mandos de la capa en edición, aquí debajo (antes, ventana aparte)
}

// NEOTKO_STICKER_TAG — lista de capas del objeto activo (s170, movida a la pestaña Stickers en s348).
void GLGizmoColorStitchPainter::render_sticker_section()
{
    const float u  = neo_u();
    const float gb = 1.4f * u;

    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || mo->colorstitch_stickers.empty()) {
        m_imgui->text_colored(neo_col(NeoCol::TextDim), _u8L("No stickers on this object yet.").c_str());
        return;
    }

    // s348 — undo: snapshot ANTES de mutar. Los deslizadores lo toman al EMPEZAR a moverse
    // (IsItemActivated), así un arrastre entero es un único paso de undo.
    auto snapshot = [](const std::string& what) {
        Plater::TakeSnapshot snap(wxGetApp().plater(), what, UndoRedo::SnapshotType::GizmoAction);
    };

    ImGui::Dummy(ImVec2(0.f, 0.2f * u));
    auto& mgr = SurfaceEffectProfileManager::get();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    const size_t n = mo->colorstitch_stickers.size();
    int move_from = -1, move_to = -1;
    int remove_idx = -1, dup_idx = -1;
    for (size_t disp = 0; disp < n; ++disp) {
        const size_t i = n - 1 - disp;   // top-down: back() (tope) primero
        ColorStitchSticker& st = mo->colorstitch_stickers[i];
        ImGui::PushID((int)i);

        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float  rw = ImGui::GetContentRegionAvail().x;
        const SurfaceEffectProfile* p = st.profile_id ? mgr.find(st.profile_id) : nullptr;
        const ColorRGBA sw_col = p ? color_for_profile(*p) : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
        const bool  editing = ((int)i == m_editing_sticker_idx);
        if (editing)
            dl->AddRectFilled(ImVec2(r0.x - 2.f, r0.y - 1.f), ImVec2(r0.x + rw, r0.y + fh + 1.f),
                              neo_col_u32(NeoCol::Surface), 4.f);
        {
            const float  sq = 1.2f * u;
            const ImVec2 a(r0.x, r0.y + (fh - sq) * 0.5f), z(a.x + sq, a.y + sq);
            if (st.is_multicolor()) {
                // s348 — una franja por color (el de su receta).
                const size_t nc = st.colors.size();
                for (size_t j = 0; j < nc; ++j) {
                    const SurfaceEffectProfile* cp = st.colors[j].profile_id ? mgr.find(st.colors[j].profile_id) : nullptr;
                    const ColorRGBA cc = cp ? color_for_profile(*cp) : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
                    const float x0 = a.x + sq * float(j) / float(nc), x1 = a.x + sq * float(j + 1) / float(nc);
                    dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, z.y),
                                      ImGui::ColorConvertFloat4ToU32(ImVec4(cc.r(), cc.g(), cc.b(), 1.f)));
                }
            } else
                dl->AddRectFilled(a, z, ImGui::ColorConvertFloat4ToU32(ImVec4(sw_col.r(), sw_col.g(), sw_col.b(), 1.f)), 3.f);
            if (st.hidden)
                dl->AddRectFilled(a, z, IM_COL32(0, 0, 0, 160), 3.f);
            ImGui::Dummy(ImVec2(sq, fh));
        }
        const int   nbtn    = 6;   // s350: asignar color pasó al panel (Place)
        const float btns_w  = float(nbtn) * gb + float(nbtn - 1) * 2.f + 0.5f * u;
        ImGui::SameLine(0.f, 0.3f * u);
        {   // s350 — el lado del sticker
            const ImVec2 gp = ImGui::GetCursorScreenPos();
            const float  gs = 0.9f * u;
            draw_glyph(dl, ImVec2(gp.x, gp.y + (fh - gs) * 0.5f), gs,
                       ColorStitch::sticker_faces_down(st) ? Glyph::ZoneBottom : Glyph::ZoneTop, neo_col_u32(NeoCol::TextDim));
            ImGui::Dummy(ImVec2(gs, fh));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", (ColorStitch::sticker_faces_down(st) ? _u8L("Bottom face") : _u8L("Top face")).c_str());
        }
        ImGui::SameLine(0.f, 0.3f * u);
        {
            const std::string nm   = st.name.empty() ? "?" : st.name;
            const std::string prof = st.is_multicolor()
                ? (std::to_string(st.colors.size()) + " " + _u8L("colours"))
                : (p ? cs_strip_group(p->name) : _u8L("(no profile)"));
            const float room = std::max(2.f * u, r0.x + rw - btns_w - ImGui::GetCursorScreenPos().x - 0.3f * u);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(fit_text(nm, room * 0.55f).c_str());
            ImGui::SameLine(0.f, 0.4f * u);
            ImGui::TextDisabled("%s", fit_text(prof, room * 0.45f).c_str());
        }
        neo_same_line_at(r0.x + rw - btns_w + 0.5f * u);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, r0.y + (fh - gb) * 0.5f));
        // s348 — ojo de la capa: oculta = no se corta (se ve lo de debajo).
        if (neo_glyph_button("##stk_eye", gb, Glyph::Eye, false,
                             st.hidden ? _u8L("Hidden: not printed").c_str() : _u8L("Visible: click to hide").c_str(),
                             NeoCol::AccentBright, !st.hidden)) {
            snapshot(_u8L("Toggle sticker"));
            st.hidden = !st.hidden;
            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        }
        ImGui::SameLine(0.f, 2.f);
        // (Sin disabled_begin: aquí dentro ya hay uno abierto por el departamento y no se anida.)
        const bool can_up = disp != 0, can_dn = disp != n - 1;
        if (!can_up) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
        if (neo_glyph_button("##stk_up", gb, Glyph::Up, false, _u8L("Move up (covers more)").c_str()) && can_up) { move_from = (int)i; move_to = (int)i + 1; }
        if (!can_up) ImGui::PopStyleVar();
        ImGui::SameLine(0.f, 2.f);
        if (!can_dn) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.3f);
        if (neo_glyph_button("##stk_dn", gb, Glyph::Down, false, _u8L("Move down").c_str()) && can_dn) { move_from = (int)i; move_to = (int)i - 1; }
        if (!can_dn) ImGui::PopStyleVar();
        ImGui::SameLine(0.f, 2.f);
        // NEOTKO_STICKER_TAG — mover/rotar/escalar: "Edit placement" entra en el modo de on_mouse
        // (arrastrar mueve); los deslizadores rotan Z y escalan. Encendido = teal, como el editar de Zonas.
        if (neo_glyph_button("##stk_edit", gb, Glyph::Edit, false,
                             editing ? _u8L("Done").c_str()
                                     : _u8L("Edit: shows its controls below (side, rotate,\n"
                                            "size, mirror, colours) and lets you drag it\n"
                                            "on the model to move it.").c_str(),
                             NeoCol::AccentBright, editing)) {
            if (editing) exit_sticker_edit();
            else         enter_sticker_edit((int)i);
        }
        ImGui::SameLine(0.f, 2.f);
        if (neo_glyph_button("##stk_dup", gb, Glyph::Copy, false,
                             _u8L("Duplicate (the copy goes on top, same place)").c_str()))
            dup_idx = (int)i;
        ImGui::SameLine(0.f, 2.f);
        if (neo_glyph_button("##stk_rm", gb, Glyph::Trash, true, _u8L("Remove").c_str()))
            remove_idx = (int)i;

        ImGui::PopID();
    }

    if (move_from >= 0 && move_to >= 0 && move_to < (int)n) {
        snapshot(_u8L("Reorder stickers"));
        std::swap(mo->colorstitch_stickers[move_from], mo->colorstitch_stickers[move_to]);
        if (m_editing_sticker_idx == move_from) m_editing_sticker_idx = move_to;
        else if (m_editing_sticker_idx == move_to) m_editing_sticker_idx = move_from;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
    if (dup_idx >= 0 && dup_idx < (int)mo->colorstitch_stickers.size()) {
        snapshot(_u8L("Duplicate sticker"));
        ColorStitchSticker copy = mo->colorstitch_stickers[dup_idx];
        mo->colorstitch_stickers.push_back(std::move(copy));
        enter_sticker_edit((int)mo->colorstitch_stickers.size() - 1);   // la copia, lista para moverla
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
    if (remove_idx >= 0 && remove_idx < (int)mo->colorstitch_stickers.size()) {
        snapshot(_u8L("Remove sticker"));
        mo->colorstitch_stickers.erase(mo->colorstitch_stickers.begin() + remove_idx);
        if (remove_idx == m_editing_sticker_idx) exit_sticker_edit();
        else if (remove_idx < m_editing_sticker_idx) --m_editing_sticker_idx;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

// s111 — carril izquierdo del panel (mockup del usuario): swatch del color
// Activo arriba + la biblioteca de paletas guardadas como columna VERTICAL con
// scroll e indicador de overflow. El contenido es el antiguo bloque "Profile
// list" horizontal reorientado; el comportamiento se conserva intacto (click
// selecciona, click-derecho borra, hover muestra la cajita sandwich).
// NEOTKO_COLORSTITCH_TAG
// NEOTKO_COLORSTITCH_TAG — s137b: selector de grupo de paleta como fila full-width
// (antes vivía dentro del carril estrecho de render_left_rail y clipaba el botón "+").
// Lista GLOBAL (project-level, independiente del objeto); el grupo es solo navegación
// y NO particiona los slots por volumen. Grupo activo manda dónde cae el Pin/Save.
void GLGizmoColorStitchPainter::render_group_selector()
{
    auto& mgr = SurfaceEffectProfileManager::get();

    // s231 F6 — el techo de grupos se derivaba SÓLO de los nombres existentes, así que
    // un grupo recién creado y todavía vacío desaparecía ("he creado un grupo y no está").
    // El hint lo mantiene vivo durante la sesión.
    int max_group = std::max(m_active_group, m_max_group_hint);
    for (const SurfaceEffectProfile& gp : mgr.list())
        max_group = std::max(max_group, cs_parse_group(gp.name));
    m_max_group_hint = max_group;
    auto group_count = [&](int g) {
        int c = 0;
        for (const SurfaceEffectProfile& gp : mgr.list())
            if (!gp.auto_generated && cs_parse_group(gp.name) == g) ++c;
        return c;
    };

    // s318 — cabecera de la biblioteca: sección "Library" con el contador del grupo activo, y debajo
    // los grupos como FICHAS (antes un combo que había que abrir para ver cuántos había), el "+", la
    // papelera y "Save all" — que antes vivía suelto debajo, fuera de su sitio.
    const float u  = neo_u();
    const float gb = 1.4f * u;
    {
        const std::string cnt = std::to_string(group_count(m_active_group));
        neo_section_g(_u8L("Library").c_str(), Glyph::Group, cnt.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Swatches are re-predicted live using the TD values of Object & TD").c_str());
    }
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h  = 1.45f * u;
    bool first = true;
    for (int g = 1; g <= max_group; ++g) {
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%s %d", _u8L("Group").c_str(), g);
        const std::string num = std::to_string(group_count(g));
        const float w = ImGui::CalcTextSize(lbl).x + ImGui::CalcTextSize(num.c_str()).x + 1.4f * u;
        if (!first) {
            // Fichas que no caben pasan a la línea siguiente (hasta MAX_GROUPS = 10 grupos).
            if (ImGui::GetItemRectMax().x + 3.f + w <= right) ImGui::SameLine(0.f, 3.f);
        }
        first = false;
        ImGui::PushID(g);
        if (ImGui::InvisibleButton("##grp", ImVec2(w, h))) m_active_group = g;
        const bool   hv = ImGui::IsItemHovered();
        const bool   on = (g == m_active_group);
        const ImVec2 ga = ImGui::GetItemRectMin(), gz = ImGui::GetItemRectMax();
        if (on)      dl->AddRectFilled(ga, gz, neo_fade(NeoCol::Accent, 0.85f), 4.f);
        else if (hv) dl->AddRectFilled(ga, gz, neo_col_u32(NeoCol::AccentGhost), 4.f);
        if (!on)     dl->AddRect(ga, gz, neo_col_u32(hv ? NeoCol::AccentBright : NeoCol::SurfaceHi), 4.f, 0, 1.f);
        const float ty = ga.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(ga.x + 0.5f * u, ty), neo_col_u32(on ? NeoCol::Ink : NeoCol::TextDim), lbl);
        dl->AddText(ImVec2(gz.x - 0.5f * u - ImGui::CalcTextSize(num.c_str()).x, ty),
                    on ? neo_fade(NeoCol::Ink, 0.7f) : neo_col_u32(NeoCol::TextDim), num.c_str());
        if (hv) ImGui::SetTooltip("%s %d  (%s)", _u8L("Group").c_str(), g, num.c_str());
        ImGui::PopID();
    }

    // (Sin BeginDisabled en esta ImGui: atenuar con Alpha — neo_col_u32 lo respeta — y vetar el clic.)
    const bool can_add = max_group < GLGizmoColorStitchPainter::MAX_GROUPS;
    ImGui::SameLine(0.f, 3.f);
    if (!can_add) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
    if (neo_glyph_button("##grp_new", gb, Glyph::Plus, false, _u8L("Create a new palette group").c_str()) && can_add)
        m_active_group = std::min(max_group + 1, GLGizmoColorStitchPainter::MAX_GROUPS);
    if (!can_add) ImGui::PopStyleVar();

    // A la derecha: papelera del grupo y "Save all" (sólo si hay colores de trabajo sin guardar).
    const bool        can_del  = m_active_group > 1;
    const bool        unsaved  = has_unsaved_palettes();
    const std::string save_lbl = _u8L("Save all");
    const float       save_w   = unsaved ? ImGui::CalcTextSize(save_lbl.c_str()).x + 2.5f * u + 3.f : 0.f;
    const float       block_w  = gb + save_w;
    if (ImGui::GetItemRectMax().x + 0.8f * u + block_w <= right)
        neo_same_line_at(right - block_w);
    else
        ImGui::SetCursorScreenPos(ImVec2(right - block_w, ImGui::GetCursorScreenPos().y));
    if (!can_del) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
    if (neo_glyph_button("##grp_del", gb, Glyph::Trash, true,
                         _u8L("Delete group (its colours move to Group 1)").c_str()) && can_del) {
        // Borrar grupo = mover sus colores al Grupo 1 (sin perder trabajo).
        Plater::TakeSnapshot snap(wxGetApp().plater(), _u8L("Delete ColorStitch group"),
                                  UndoRedo::SnapshotType::GizmoAction);
        std::vector<int> ids;
        for (const SurfaceEffectProfile& gp : mgr.list())
            if (cs_parse_group(gp.name) == m_active_group) ids.push_back(gp.id);
        for (int id : ids)
            if (const SurfaceEffectProfile* gp = mgr.find(id))
                mgr.rename(id, cs_strip_group(gp->name));   // → Grupo 1
        m_active_group   = 1;
        // s231 F6 — soltar el hint: el techo vuelve a derivarse de los nombres reales,
        // así que el grupo borrado no se queda de fantasma en el combo.
        m_max_group_hint = 1;
        refresh_selector_palettes();
    }
    if (!can_del) ImGui::PopStyleVar();
    if (unsaved) {
        ImGui::SameLine(0.f, 3.f);
        if (neo_text_button((save_lbl + "##save_all").c_str(), NeoBtn::Accent, true, Glyph::Save))
            save_all_palettes();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Save every unsaved working colour into the active palette group").c_str());
    }

    // Guía SUAVE (no bloquea): el grupo activo pasa de 30 colores. A la bandeja.
    const int active_cnt = group_count(m_active_group);
    if (active_cnt > 30)
        tray(std::to_string(active_cnt) + "/30 " + _u8L("colours in this group"),
             _u8L("A group this full is hard to browse. Consider splitting it into another group."));
    ImGui::Dummy(ImVec2(0.f, 0.15f * u));
}

// s169 F0 — color-resultado predicho en vivo (top+penu compuestos contra el fondo
// real del objeto activo), extraído tal cual del lambda local `predict_argb` que
// vivía dentro de render_left_rail — para que el header/Object (F2/F3) puedan
// predecir un swatch sin duplicar la composición. Self-contained: recalcula
// mats/bg por su cuenta (mismo coste que ya pagaba cada swatch de la lista).
uint32_t GLGizmoColorStitchPainter::predict_argb_for(const Slic3r::SurfacePassStack& top,
                                                  const Slic3r::SurfacePassStack& penu) const
{
    namespace CS = Slic3r::ColorSci;
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    float bg[3] = {0.f, 0.f, 0.f};
    resolve_object_base_bg(mats, bg);
    float out[3] = {0.f, 0.f, 0.f};
    CS::sandwich_colour_stacked(top, penu, mats, bg, ColorStitchPaintPreview::weave_layer_height(), out);
    return 0xFF000000u
         | ((uint32_t)std::min(255.f, out[0] * 255.f) << 16)
         | ((uint32_t)std::min(255.f, out[1] * 255.f) <<  8)
         |  (uint32_t)std::min(255.f, out[2] * 255.f);
}

// s169 F1 — header persistente sobre el selector de departamentos: por ahora
// solo el swatch Active (reemplaza el bloque "Active" de render_left_rail,
// retirado junto al layout de 2 columnas). F2 añade aquí mismo "+ New" +
// nombre editable + Pin (movidos desde el final de Pro).
void GLGizmoColorStitchPainter::render_header()
{
    auto& mgr = SurfaceEffectProfileManager::get();

    // Drop selection if its profile was deleted under us.
    // s231 F0 — por el invalidador único: dejar `m_active_resolved` en true con el
    // slot a 0 es exactamente el estado que producía el bug s209 (pincel mudo con
    // aspecto de listo), y este camino lo reproducía igual que el de recarga.
    if (m_selected_profile_id != 0 && mgr.find(m_selected_profile_id) == nullptr)
        invalidate_active_binding(/*keep_recipe=*/true);
    // Resync active slot every frame (cheap; no mutation when slot already exists).
    if (m_selected_profile_id != 0)
        m_active_slot = slot_for_selected_profile(/*assign_if_missing=*/false);

    // s318 — TARJETA del color activo (biselada, como las zonas del Pro): swatch grande, nombre, el
    // estado honesto de s231 F0 como píldora, la receta en una línea, y New / Save / Duplicate como
    // glifos a la derecha. Mismas acciones, mismos tooltips y la misma predicción en vivo de antes.
    bool        have = false;
    uint32_t    argb = 0;
    std::string desc;
    if (m_has_active_recipe) {
        // Re-predecir en vivo con los TD actuales (no usar m_active_recipe.rgb, congelado al elegir).
        argb = predict_argb_for(m_active_recipe.top, m_active_recipe.penu);
        have = true;
        desc = "Top " + zone_desc(m_active_recipe.top);
        if (!m_active_recipe.penu.passes.empty())
            desc += " · Penu " + zone_desc(m_active_recipe.penu);
    } else if (const SurfaceEffectProfile* sp_sel = mgr.find(m_selected_profile_id)) {
        const SurfacePassStack st_top  = SurfacePassStack::from_json(sp_sel->stack_top_json);
        const SurfacePassStack st_penu = SurfacePassStack::from_json(sp_sel->stack_penu_json);
        argb = predict_argb_for(st_top, st_penu);
        have = true;
        desc = "Top " + zone_desc(st_top);
        if (!st_penu.passes.empty())
            desc += " · Penu " + zone_desc(st_penu);
    }

    // s231 F0 — el estado del color activo, con el vocabulario del sistema:
    //   slot N (teal) = ya materializado aquí · ready (gris) = se creará al pintar ·
    //   no colour (ámbar) = el clic no va a pintar. Es el único ámbar de la cabecera, y lo es porque
    //   de verdad algo no va a funcionar.
    std::string chip_txt, chip_tip;
    ImU32       chip_col;
    if (m_active_slot >= 1 && m_active_slot < MAX_SLOTS) {
        chip_txt = _u8L("slot") + " " + std::to_string(m_active_slot);
        chip_col = neo_col_u32(NeoCol::AccentBright);
        chip_tip = _u8L("This colour already has a paint slot on the active "
                        "object — painting applies it directly.");
    } else if (m_has_active_recipe || m_selected_profile_id != 0) {
        chip_txt = _u8L("ready");
        chip_col = neo_col_u32(NeoCol::TextDim);
        chip_tip = _u8L("Colour selected. It takes a paint slot on this object "
                        "the first time you paint with it.");
    } else {
        chip_txt = _u8L("no colour");
        chip_col = neo_col_u32(NeoCol::Warn);
        chip_tip = _u8L("No colour selected — clicking the model will not paint. "
                        "Pick one from the palette, the generator, or the "
                        "eyedropper.");
    }

    const float  u   = neo_u();
    const float  pad = 0.45f * u;
    const float  sw  = 2.9f * u;
    const float  bs  = 1.75f * u;
    ImDrawList*  dl  = ImGui::GetWindowDrawList();
    const ImVec2 c0  = ImGui::GetCursorScreenPos();
    const float  cw  = ImGui::GetContentRegionAvail().x;
    const ImVec2 c1(c0.x + cw, c0.y + sw + 2.f * pad);
    neo_card_bg(dl, c0, c1, 0.9f * u, false, neo_col_u32(NeoCol::CanvasTop));

    const ImVec2 s0(c0.x + pad, c0.y + pad), s1(s0.x + sw, s0.y + sw);
    if (have) {
        dl->AddRectFilled(s0, s1, IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255), 4.f);
        dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 46), 4.f, 0, 1.f);
    } else {
        // Sin color: rayado de "aquí no hay nada", no un cuadro negro que parece un color.
        dl->AddRectFilled(s0, s1, neo_col_u32(NeoCol::Canvas), 4.f);
        hatch_rect(dl, s0, s1, neo_fade(NeoCol::SurfaceHi, 0.7f), 5.f);
        dl->AddRect(s0, s1, neo_col_u32(NeoCol::SurfaceHi), 4.f, 0, 1.f);
    }

    const float bx0 = c1.x - pad - (3.f * bs + 4.f);
    const float tx  = s1.x + 0.6f * u;
    const std::string name   = have ? (m_active_style.empty() ? _u8L("Custom") : m_active_style) : std::string("—");
    const float       chip_w = ImGui::CalcTextSize(chip_txt.c_str()).x + 0.95f * u;
    ImGui::SetCursorScreenPos(ImVec2(tx, s0.y + 0.12f * u));
    ImGui::TextUnformatted(fit_text(name, std::max(2.f * u, bx0 - tx - chip_w - 0.9f * u)).c_str());
    ImGui::SameLine(0.f, 0.45f * u);
    neo_status_chip(chip_txt.c_str(), chip_col, false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", chip_tip.c_str());
    if (!desc.empty()) {
        const float fs = 0.74f * u;
        const std::string d = fit_text(desc, (bx0 - tx - 0.4f * u) / 0.74f);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(tx, s1.y - fs - 0.15f * u), neo_col_u32(NeoCol::TextDim), d.c_str());
    }

    ImGui::SetCursorScreenPos(ImVec2(bx0, c0.y + (c1.y - c0.y - bs) * 0.5f));
    // s169 F2 — "+ New": receta en blanco (1 pase Solid T1, penu vacío) y salta a Pro.
    if (neo_glyph_button("##cs_new", bs, Glyph::Plus, false,
                         _u8L("Start a new colour from scratch in Pro mode").c_str())) {
        Slic3r::ColorSci::ColorRecipe recipe;
        Slic3r::SurfacePass sp;
        sp.kind       = Slic3r::SurfacePassKind::Solid;
        sp.solid_tool = 0;
        sp.ratio      = 1.0;
        recipe.top.passes.push_back(sp);
        recipe.top.enabled = true;
        set_active_recipe(recipe, _u8L("Custom"));
        m_department = DEP_PRO;   // saltar a Pro para editar la receta recién creada
    }
    ImGui::SameLine(0.f, 2.f);
    // s174 — "Save" (= save_active_as_palette, antes "Pin to palette").
    if (neo_glyph_button("##cs_save", bs, Glyph::Save, false,
                         _u8L("Keep this colour in the saved palette library "
                              "(otherwise it is a temporary working colour).").c_str()))
        save_active_as_palette();
    ImGui::SameLine(0.f, 2.f);
    // s231 F4 — "Duplicate": copia independiente, enlazada, para variarla sin tocar el original.
    if (neo_glyph_button("##cs_dup", bs, Glyph::Copy, false,
                         _u8L("Make an independent copy of this colour and edit "
                              "the copy in Pro — the original stays untouched "
                              "everywhere it is already painted.").c_str()))
        duplicate_active_as_new();

    ImGui::SetCursorScreenPos(ImVec2(c0.x, c1.y));
    ImGui::Dummy(ImVec2(cw, 0.f));

    // NEOTKO_MMU_COEXIST_TAG s235 F5a — el aviso que faltaba: donde el objeto ya tiene
    // pintura de MMU manda el MMU (precedencia del motor desde s234), así que ESA parte de
    // lo que se pinta aquí no llevará efecto sandwich. Antes de esto sólo se descubría en
    // el gcode. Es el aviso INVERSO al de Perimeter override (que avisa de que el efecto se
    // sale hacia fuera de lo pintado); los dos pueden estar activos a la vez y dicen cosas
    // distintas. El gemelo, con el mismo texto desde el otro lado, vive en el gizmo de MMU
    // (GLGizmoMmuSegmentation::render_coexist_warning).
    //
    // Porcentaje, no mm²: ver la nota de ColorStitchPaintPreview::CoexistOverlap — el área es
    // una cota superior en malla local, y el % es lo que de verdad se quiere saber.
    {
        const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (mo) {
            const int      oid = m_parent.get_selection().get_object_idx();
            const uint64_t key = ColorStitchPaintPreview::overlap_key(mo);
            if (oid != m_coexist_oid || key != m_coexist_key) {
                m_coexist_oid = oid;
                m_coexist_key = key;
                m_coexist     = ColorStitchPaintPreview::mmu_sandwich_overlap(mo);
            }
            if (m_coexist.any() && m_coexist.sandwich_mm2 > 0.) {
                const int pct = std::max(1, std::min(100,
                    (int) (100. * m_coexist.area_mm2 / m_coexist.sandwich_mm2 + 0.5)));
                const std::string msg = into_u8(GUI::format(
                    _L("⚠ ~%1%%% of this paint is under MMU paint — flat there, no effect"), pct));
                // s318 — a la bandeja (R7): aquí arriba empujaba toda la columna.
                tray(cs_strip_warn(msg), _u8L("Where MMU paint and Sandwich paint overlap, MMU "
                                              "wins: that area prints flat in its own filament, "
                                              "with no Sandwich effect. Move one of the two "
                                              "paints if you want the effect there."));
            }
        }
    }
}

// s169 F1 — rejilla full-width de paletas guardadas (reemplaza la columna
// vertical estrecha de render_left_rail — mismo contenido/comportamiento,
// solo cambia el layout: N swatches por fila en vez de 1). Conservados
// INTACTOS: click (carga+enlaza), right-click Delete diferido, borde ámbar
// de autos, tooltip con draw_zone, aviso slots_full.
void GLGizmoColorStitchPainter::render_paint_palette_grid()
{
    namespace CS = Slic3r::ColorSci;
    auto& mgr = SurfaceEffectProfileManager::get();

    std::vector<std::string> fcolors;   // solo para el draw_zone del tooltip
    { CS::Material mats[4]; gizmo_materials(mats, fcolors); }

    // s318 — el título lo pone la sección "Library" (render_group_selector).

    const ModelObject* mo = m_c->selection_info()->model_object();

    // Helper: which slot (if any) does profile P occupy in this object?
    // s231 — miraba SÓLO el primer volumen model_part, mientras que quien asigna
    // (slot_for_selected_profile) recorre TODOS: en un objeto multivolumen el "(sN)"
    // del tooltip y el filtro "ocupa slot" podían mentir (y con ellos la decisión de
    // mostrar/ocultar un color de trabajo en la rejilla).
    // s232 — "la paleta cambia al mover el ratón entre objetos" (feedback usuario): un
    // color de TRABAJO sólo se lista si ocupa slot, y esto miraba SÓLO el objeto activo
    // — que en modo pintar/pipeta cambia con el simple hover (pre-activación, s118). O
    // sea: la rejilla se reordenaba al pasear el ratón, y con ella lo que "Save" parecía
    // hacer o no hacer. Ahora cuenta el objeto activo Y todos los marcados: el set
    // pintable es el mismo mientras no cambies la selección, así que la lista se queda
    // quieta. El número de slot devuelto sigue siendo el del objeto activo cuando lo
    // tiene (es el que enseña el tooltip "(sN)").
    auto slot_in = [&](const ModelObject* o, int pid) -> int {
        if (!o) return 0;
        for (const ModelVolume* mv : o->volumes) {
            if (!mv->is_model_part()) continue;
            for (int s = 1; s < MAX_SLOTS; ++s)
                if (mv->colorstitch_slot_to_profile_id[s] == pid) return s;
        }
        return 0;
    };
    const Model* _model = m_parent.get_selection().get_model();
    auto slot_of = [&](int pid) -> int {
        if (const int s = slot_in(mo, pid)) return s;
        if (!_model) return 0;
        for (int oid : m_marked_objects)
            if (oid >= 0 && oid < (int)_model->objects.size())
                if (const int s = slot_in(_model->objects[oid], pid)) return s;
        return 0;
    };

    int profile_to_delete = 0;   // diferido: no mutar mgr.list() durante la iteración
    int profile_to_duplicate = 0;   // s231 F4 — idem (add() invalida la iteración)
    int profile_to_save      = 0;   // s231 F4 — promover un color de trabajo concreto
    // s318 — el lado del swatch se REPARTE en el ancho (≈2.3u, nunca menos de 4 por fila), así la
    // rejilla llena su pozo de borde a borde en vez de dejar un hueco a la derecha.
    const float u       = neo_u();
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float gap     = 4.f;
    const float inner_w = avail_w - 8.f - ImGui::GetStyle().ScrollbarSize;   // padding 4+4 y la barra siempre visible
    const int   per_row = std::max(4, (int)std::floor((inner_w + gap) / (2.3f * u + gap)));
    const float sw      = std::max(1.2f * u, std::floor((inner_w - gap * float(per_row - 1)) / float(per_row)) - 0.5f);
    // NEOTKO_STICKER_TAG — feedback usuario: la rejilla se comía media pantalla
    // y dejaba la sección "Stickers (SVG)" (debajo) sin aire. Limitada a ~2
    // filas de swatches (calculado desde `sw`, no un alto fijo, para que siga
    // siendo robusto a DPI/escala); el resto hace scroll como siempre.
    const float grid_h  = 2.f * (sw + 4.f) + 8.f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);          // s318 — pozo, sin borde
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, neo_col(NeoCol::Canvas));
    // AlwaysVerticalScrollbar: reservar SIEMPRE el ancho de la scrollbar (evita
    // el "fliqueo" de s137/s138 — su aparición/desaparición cambiaría el ancho
    // disponible → cambiaría per_row/tamaño → oscilación cada frame).
    ImGui::BeginChild("##cmp_profile_grid", ImVec2(avail_w, grid_h), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (mgr.size() == 0) {
        ImGui::PushTextWrapPos(0.f);
        m_imgui->text_colored(neo_col(NeoCol::TextDim), m_desc["no_profiles"]);
        ImGui::PopTextWrapPos();
    } else {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        int shown = 0;

        for (const SurfaceEffectProfile& p : mgr.list()) {
            // PR.3: la lista es la biblioteca de paletas guardadas. NEOTKO_COLORSTITCH_TAG
            // s118: además mostramos los colores de TRABAJO (auto) que ocupan un slot del
            // objeto actual — así se pueden BORRAR con right-click→Delete y limpiar lo
            // acumulado (los slots se llenaban sin forma de vaciarlos salvo el main UX).
            const bool occupies_slot = (slot_of(p.id) != 0);
            if (p.auto_generated && !occupies_slot) continue;
            // NEOTKO_COLORSTITCH_TAG — s137: filtrar por grupo activo. Excepción: un
            // color que ocupa slot en el objeto actual se muestra SIEMPRE (para poder
            // borrarlo aunque sea de otro grupo) — mismo criterio que los auto.
            if (cs_parse_group(p.name) != m_active_group && !occupies_slot) continue;
            ImGui::PushID(p.id);

            const ImVec2 sp = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##cmp_sw", ImVec2(sw, sw))) {
                // NEOTKO_COLORSTITCH_TAG — s118 (punto 3): seleccionar una paleta
                // guardada AHORA la carga en el editor Pro y la deja enlazada por id,
                // para poder editarla en vivo (antes hacía m_has_active_recipe=false y
                // el Pro no la reflejaba → no se podían editar guardados). El binding
                // por id hace que el write-back del Pro reescriba ESTE perfil.
                m_select_mode       = false;   // s111: elegir paleta → modo pintar
                m_erase_mode        = false;
                if (m_selected_profile_id != p.id) {
                    const CS::ColorRecipe r = recipe_from_profile(p);
                    m_active_pal_kind     = 0;   // s231 F5: ya no viene del Generator
                    m_active_pal_idx      = -1;
                    m_selected_profile_id = p.id;
                    m_active_resolved     = false;   // re-materializa slot al pintar
                    m_active_slot = slot_for_selected_profile(/*assign_if_missing=*/true);
                    // Estado activo completo (pinta por id; el Pro lo edita en vivo).
                    m_active_recipe     = r;
                    m_active_style      = cs_strip_group(p.name);   // s137: sin sufijo de grupo
                    m_has_active_recipe = true;
                    load_recipe_into_pro(r);
                    refresh_selector_palettes();
                }
            }
            const bool hov = ImGui::IsItemHovered();
            const bool sel = (m_selected_profile_id == p.id);

            // Click derecho sobre el swatch → menú contextual.
            // s231 F4 — hasta ahora tenía UNA sola entrada (Delete), y duplicar era
            // justo lo que faltaba para poder trabajar sobre variantes sin destruir el
            // original (ver duplicate_active_as_new). "Save" aparece sólo en los
            // colores de trabajo (auto), que son los que se pueden perder por GC.
            if (ImGui::BeginPopupContextItem(nullptr)) {
                if (ImGui::MenuItem(_u8L("Duplicate").c_str()))
                    profile_to_duplicate = p.id;
                if (p.auto_generated && ImGui::MenuItem(_u8L("Save to palette").c_str()))
                    profile_to_save = p.id;
                ImGui::Separator();
                if (ImGui::MenuItem(_u8L("Delete").c_str()))
                    profile_to_delete = p.id;
                ImGui::EndPopup();
            }

            const SurfacePassStack st_top  = SurfacePassStack::from_json(p.stack_top_json);
            const SurfacePassStack st_penu = SurfacePassStack::from_json(p.stack_penu_json);

            // Swatch = color-resultado RE-PREDICHO en vivo a través de los TD
            // actuales desde el stack guardado (antes usaba el preview_argb
            // cacheado al guardar, que no reaccionaba a cambios de TD). s111.
            {
                const uint32_t argb = predict_argb_for(st_top, st_penu);
                ldl->AddRectFilled(sp, ImVec2(sp.x + sw, sp.y + sw),
                                   IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255), 3.f);
            }
            // s318 — marcas del swatch, todas leídas del propio perfil:
            //   · elegido = anillo teal (antes borde blanco de 2.5 px);
            //   · color de TRABAJO (auto, no guardado) = muesca en la esquina. Antes era un borde
            //     ámbar, pero el ámbar es de avisos y un color sin guardar no es un error;
            //   · tipo de efecto (rayas = ColorStitch, rampa = PathBlend) en la esquina de abajo;
            //   · punto teal = este color ocupa slot en el objeto / los marcados.
            if (sel)
                neo_sel_ring(ldl, sp, ImVec2(sp.x + sw, sp.y + sw), 3.f);
            else
                ldl->AddRect(sp, ImVec2(sp.x + sw, sp.y + sw),
                             hov ? neo_col_u32(NeoCol::TextDim) : IM_COL32(0, 0, 0, 90), 3.f, 0, 1.f);
            if (p.auto_generated) {
                const float k = sw * 0.34f;
                ldl->AddTriangleFilled(sp, ImVec2(sp.x + k, sp.y), ImVec2(sp.x, sp.y + k), neo_fade(NeoCol::Ink, 0.85f));
            }
            {
                bool has_pb = false, has_cs = false;
                for (const SurfacePassStack* zs : { &st_top, &st_penu })
                    for (const SurfacePass& pp : zs->passes) {
                        has_pb |= (pp.kind == SurfacePassKind::PathBlend);
                        has_cs |= (pp.kind == SurfacePassKind::ColorStitch);
                    }
                if (has_pb || has_cs) {
                    const float  m  = sw * 0.44f;
                    const ImVec2 m0(sp.x + sw - m - 1.f, sp.y + sw - m - 1.f), m1(sp.x + sw - 1.f, sp.y + sw - 1.f);
                    ldl->AddRectFilled(m0, m1, neo_fade(NeoCol::Canvas, 0.78f), 2.f);
                    draw_glyph(ldl, ImVec2(m0.x + m * 0.08f, m0.y + m * 0.08f), m * 0.84f,
                               has_pb ? Glyph::KPbFull : Glyph::KStitch, neo_col_u32(NeoCol::Ink));
                }
            }
            if (occupies_slot) {
                const ImVec2 dc(sp.x + sw - 4.5f, sp.y + 4.5f);
                ldl->AddCircleFilled(dc, 3.2f, neo_col_u32(NeoCol::Canvas), 10);
                ldl->AddCircleFilled(dc, 2.2f, neo_col_u32(NeoCol::AccentBright), 10);
            }

            // Hover → cajita sandwich (Top sobre Penu) como preview de
            // verificación + nombre / zonas / slot.
            if (hov) {
                // s232 — pasar el ratón por un swatch resalta en el viewport dónde
                // está aplicado ese color (si ocupa slot en este objeto). Es la
                // consulta que antes no tenía respuesta: el tooltip decía "(s3)"
                // pero no dónde está el s3.
                hover_slot(slot_of(p.id));
                // s350 — la tarjeta del boceto de Neotko (WIP/PALETTE_HOVER_TOOLTIP_MOCKUP_s349.png):
                // izquierda, columna con las zonas en miniatura (Top, Penu, la pieza, Bottom) con el
                // patrón REAL de cada pasada (el mismo dibujo que el Pro); derecha, el cuadro grande
                // con la receta de cerca (líneas cruzadas, TD) y una franja con el color de lejos; el
                // ángulo, encima, abajo a la izquierda. Los textos largos, fuera.
                ImGui::BeginTooltip();
                draw_recipe_card(st_top, st_penu, SurfacePassStack::from_json(p.stack_bottom_json), p.auto_generated);
                ImGui::EndTooltip();
            }

            ImGui::PopID();
            ++shown;
            if (shown % per_row != 0) ImGui::SameLine(0.f, 4.f);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);   // ChildBorderSize + WindowPadding + ItemSpacing de la rejilla

    // s318 — la leyenda de las marcas, una línea pequeña: sin ella la muesca y la esquina se adivinan.
    {
        ImDrawList*  ldl = ImGui::GetWindowDrawList();
        const float  fs  = 0.7f * u;
        const float  gs  = 0.8f * u;
        ImFont*      fnt = ImGui::GetFont();
        ImVec2       q   = ImGui::GetCursorScreenPos();
        q.y += 0.15f * u;
        const ImU32  dim = neo_col_u32(NeoCol::TextDim);
        auto item = [&](int kind, const std::string& txt) {
            if (kind == 0) {
                ldl->AddRectFilled(ImVec2(q.x, q.y + 0.1f * u), ImVec2(q.x + gs, q.y + 0.1f * u + gs), neo_col_u32(NeoCol::SurfaceHi), 2.f);
                ldl->AddTriangleFilled(ImVec2(q.x, q.y + 0.1f * u), ImVec2(q.x + gs * 0.5f, q.y + 0.1f * u),
                                       ImVec2(q.x, q.y + 0.1f * u + gs * 0.5f), neo_col_u32(NeoCol::Ink));
            } else if (kind == 1) {
                ldl->AddCircleFilled(ImVec2(q.x + gs * 0.5f, q.y + 0.1f * u + gs * 0.5f), 2.5f, neo_col_u32(NeoCol::AccentBright), 10);
            } else {
                draw_glyph(ldl, ImVec2(q.x, q.y + 0.1f * u), gs, kind == 2 ? Glyph::KStitch : Glyph::KPbFull, dim);
            }
            ldl->AddText(fnt, fs, ImVec2(q.x + gs + 0.25f * u, q.y + 0.1f * u), dim, txt.c_str());
            q.x += gs + 0.25f * u + fnt->CalcTextSizeA(fs, FLT_MAX, 0.f, txt.c_str()).x + 0.8f * u;
        };
        item(0, _u8L("not saved"));
        item(1, _u8L("on this object"));
        item(2, _u8L("ColorStitch"));
        item(3, _u8L("PathBlend"));
        ImGui::Dummy(ImVec2(avail_w, gs + 0.35f * u));
    }

    // s231 F4 — acciones diferidas del contextual, fuera de la iteración de mgr.list()
    // (add/rename reordenan o invalidan el contenedor).
    if (profile_to_duplicate != 0) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Duplicate ColorStitch profile"),
                                      UndoRedo::SnapshotType::GizmoAction);
        const int new_id = duplicate_profile(profile_to_duplicate);
        if (new_id != 0) {
            // Dejar la copia SELECCIONADA y abierta en Pro: duplicar se hace para
            // editar la copia, no para mirarla.
            if (const SurfaceEffectProfile* np = SurfaceEffectProfileManager::get().find(new_id)) {
                const CS::ColorRecipe r = recipe_from_profile(*np);
                invalidate_active_binding(/*keep_recipe=*/true);
                m_selected_profile_id = new_id;
                m_active_recipe       = r;
                m_active_style        = cs_strip_group(np->name);
                m_has_active_recipe   = true;
                load_recipe_into_pro(r);
                m_department = DEP_PRO;
            }
            refresh_selector_palettes();
            m_parent.set_as_dirty();
        }
    }
    if (profile_to_save != 0) {
        if (SurfaceEffectProfile* mp = SurfaceEffectProfileManager::get().find_mut(profile_to_save)) {
            mp->name           = cs_with_group(mp->name, m_active_group);
            mp->auto_generated = false;   // deja de ser GC-able → ya no se puede perder
        }
    }

    // Borrado diferido de un profile guardado (click derecho → Delete). Fuera del
    // child y de la iteración de mgr.list(). Limpia los slots que lo referencian en
    // todos los volúmenes para que las caras pintadas no adopten otro profile luego.
    if (profile_to_delete != 0) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete ColorStitch profile"),
                                      UndoRedo::SnapshotType::GizmoAction);
        // s231 — la limpieza de slots barría SÓLO el objeto activo, así que borrar un
        // perfil usado por otros objetos les dejaba el slot apuntando a un id muerto
        // (la caja gris fantasma que ya documentó s139 para el GC de autos, por la
        // misma causa: barrer un objeto en vez del proyecto). Igual que
        // garbage_collect_auto_profiles desde s139, esto recorre TODO el modelo.
        if (const Model* model_all = m_parent.get_selection().get_model())
            for (ModelObject* mo_it : model_all->objects) {
                if (!mo_it) continue;
                for (ModelVolume* mv : mo_it->volumes) {
                    if (!mv->is_model_part()) continue;
                    for (int s = 1; s < MAX_SLOTS; ++s)
                        if (mv->colorstitch_slot_to_profile_id[s] == profile_to_delete)
                            mv->colorstitch_slot_to_profile_id[s] = 0;
                }
            }
        if (m_selected_profile_id == profile_to_delete) {
            m_selected_profile_id = 0;
            m_active_slot         = 0;
        }
        SurfaceEffectProfileManager::get().remove(profile_to_delete);
        update_model_object();
        refresh_selector_palettes();
        m_parent.set_as_dirty();
    }

    // Capacity warning — SOLO si de verdad no queda ningún slot libre. Un profile
    // recién guardado (o seleccionado) todavía no quema slot hasta que se pinta,
    // así que m_active_slot==0 NO implica "lleno": comprobar que existe hueco.
    if (m_selected_profile_id != 0 && m_active_slot == 0) {
        bool any_free = false;
        for (int s = 1; s < MAX_SLOTS && !any_free; ++s) {
            bool free_everywhere = true;
            if (mo) for (const ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                if (mv->colorstitch_slot_to_profile_id[s] != 0) { free_everywhere = false; break; }
            }
            if (free_everywhere) any_free = true;
        }
        if (!any_free)
            tray(into_u8(m_desc["slots_full"]));   // s318 — a la bandeja
    }
}
// NEOTKO_COLORSTITCH_TAG_END

// NEOTKO_COLORSTITCH_TAG — s231 F6: inventario "EN USO EN ESTE OBJETO". Los slots son
// el recurso escaso real del sistema y hasta ahora no se veían por ningún lado: sólo
// existía el aviso "slots full" cuando ya era tarde, el "(sN)" escondido en el tooltip
// de un swatch, y el volcado del eyedropper... AL LOG (un fichero que el usuario no
// tiene por qué abrir nunca). Aquí, una fila por slot ocupado con su color real, su
// nombre, cuántas caras pinta, y las dos acciones que faltaban: recuperarlo como color
// activo y liberarlo.
void GLGizmoColorStitchPainter::render_slots_in_use()
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
    auto& mgr = SurfaceEffectProfileManager::get();

    // slot → (pid, nº de caras pintadas sumando todos los volúmenes model_part)
    std::map<int, std::pair<int, int>> in_use;
    int vol_idx = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++vol_idx;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int pid = mv->colorstitch_slot_to_profile_id[s];
            if (pid == 0) continue;
            auto& e = in_use[s];
            e.first = pid;
            // num_facets sale del selector VIVO (el estado que el usuario está viendo),
            // no del blob serializado: así el recuento cuadra con la pantalla incluso
            // antes de que update_model_object haya persistido el último trazo.
            if (vol_idx < (int)m_triangle_selectors.size() && m_triangle_selectors[vol_idx])
                e.second += m_triangle_selectors[vol_idx]->num_facets(static_cast<EnforcerBlockerType>(s));
        }
    }

    // s318 — sección siempre visible (antes plegable) con el contador de slots a la derecha.
    {
        const std::string aside = std::to_string(in_use.size()) + " " + _u8L("slots");
        neo_section_g(_u8L("In use on this object").c_str(), Glyph::Target, aside.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Every paint slot this object is spending, with the colour "
                                         "it holds. Freeing a slot removes that colour's paint from "
                                         "this object only.").c_str());
    }
    if (in_use.empty()) {
        m_imgui->text_colored(neo_col(NeoCol::TextDim), _u8L("Nothing painted yet.").c_str());
        return;
    }

    int slot_to_free = 0, pid_to_use = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float u  = neo_u();
    const float fh = ImGui::GetFrameHeight();
    const float gb = 1.4f * u;
    const float sq = 1.2f * u;
    int max_faces = 1;
    for (const auto& kv : in_use) max_faces = std::max(max_faces, kv.second.second);
    for (const auto& kv : in_use) {
        const int slot = kv.first, pid = kv.second.first, faces = kv.second.second;
        const SurfaceEffectProfile* p = mgr.find(pid);
        ImGui::PushID(4000 + slot);
        ImGui::BeginGroup();   // s232 — la fila entera como zona de hover del realce

        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const float  rw = ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("s%d", slot);
        neo_same_line_at(r0.x + 2.2f * u);

        uint32_t argb = 0xFF808080u;
        if (p) {
            const SurfacePassStack st  = SurfacePassStack::from_json(p->stack_top_json);
            const SurfacePassStack stp = SurfacePassStack::from_json(p->stack_penu_json);
            argb = predict_argb_for(st, stp);
        }
        {
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(sq, fh));
            const ImVec2 a(q.x, q.y + (fh - sq) * 0.5f), z(a.x + sq, a.y + sq);
            dl->AddRectFilled(a, z, IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255), 3.f);
            if (pid == m_selected_profile_id) neo_sel_ring(dl, a, z, 3.f);
        }
        ImGui::SameLine(0.f, 0.45f * u);

        // nombre + medidor de caras (relativo al slot que más gasta en este objeto)
        char fbuf[24];
        std::snprintf(fbuf, sizeof(fbuf), "%d", faces);
        const float fw     = faces > 0 ? ImGui::CalcTextSize(fbuf).x + 0.5f * u : 0.f;
        const float btns_w = 2.f * gb + 2.f;
        const ImVec2 np    = ImGui::GetCursorScreenPos();
        const float name_w = std::max(2.f * u, r0.x + rw - btns_w - fw - 0.5f * u - np.x);
        const std::string name = p ? cs_strip_group(p->name) : std::string("?");
        dl->AddText(ImVec2(np.x, np.y + 0.1f * u), neo_col_u32(NeoCol::Ink), fit_text(name, name_w).c_str());
        {
            const float my = np.y + fh - 0.3f * u;
            dl->AddLine(ImVec2(np.x, my), ImVec2(np.x + name_w, my), neo_col_u32(NeoCol::SurfaceHi), 3.f);
            if (faces > 0)
                dl->AddLine(ImVec2(np.x, my), ImVec2(np.x + name_w * float(faces) / float(max_faces), my),
                            neo_col_u32(NeoCol::Accent), 3.f);
        }
        ImGui::Dummy(ImVec2(name_w, fh));
        if (faces > 0) {
            ImGui::SameLine(0.f, 0.4f * u);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", fbuf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Painted facets on this object").c_str());
        }
        neo_same_line_at(r0.x + rw - btns_w);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, r0.y + (fh - gb) * 0.5f));
        if (neo_glyph_button("##use", gb, Glyph::Target, false, _u8L("Make this the active paint colour").c_str()))
            pid_to_use = pid;
        ImGui::SameLine(0.f, 2.f);
        if (neo_glyph_button("##free", gb, Glyph::Trash, true,
                             _u8L("Erase this colour from this object and release its slot").c_str()))
            slot_to_free = slot;
        ImGui::EndGroup();
        // s232 — hover en la fila = "enséñame dónde está este slot" en el visor.
        if (ImGui::IsItemHovered()) {
            hover_slot(slot);
            dl->AddRectFilled(ImVec2(ImGui::GetItemRectMin().x - 3.f, ImGui::GetItemRectMin().y - 1.f),
                              ImVec2(ImGui::GetItemRectMax().x + 3.f, ImGui::GetItemRectMax().y + 1.f),
                              neo_col_u32(NeoCol::AccentGhost), 4.f);
        }
        ImGui::PopID();
    }

    // --- acciones diferidas (mutan modelo/selectores: fuera del bucle) -------------
    if (pid_to_use != 0) {
        if (const SurfaceEffectProfile* p = mgr.find(pid_to_use)) {
            const Slic3r::ColorSci::ColorRecipe r = recipe_from_profile(*p);
            invalidate_active_binding(/*keep_recipe=*/true);
            m_selected_profile_id = pid_to_use;
            m_active_recipe       = r;
            m_active_style        = cs_strip_group(p->name);
            m_has_active_recipe   = true;
            m_active_slot         = slot_for_selected_profile(/*assign_if_missing=*/false);
            load_recipe_into_pro(r);
            set_tool(TOOL_PAINT);
            refresh_selector_palettes();
        }
    }
    if (slot_to_free != 0) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Free ColorStitch slot"),
                                      UndoRedo::SnapshotType::GizmoAction);
        // Las caras pintadas con ese slot vuelven a "sin pintar" (remap slot → NONE);
        // el puntero slot→perfil se libera. El perfil NO se borra: sigue en la
        // biblioteca (y si era de trabajo, el GC lo recogerá si ya no lo usa nadie).
        ModelObject* mo_mut = m_c->selection_info()->model_object();
        int idx = -1;
        for (ModelVolume* mv : mo_mut->volumes) {
            if (!mv->is_model_part()) continue;
            ++idx;
            if (idx < (int)m_triangle_selectors.size() && m_triangle_selectors[idx]) {
                EnforcerBlockerStateMap map;
                for (size_t k = 0; k < map.size(); ++k) map[k] = static_cast<EnforcerBlockerType>(k);
                map[(size_t)slot_to_free] = EnforcerBlockerType::NONE;
                m_triangle_selectors[idx]->remap_triangle_state(map);
                m_triangle_selectors[idx]->request_update_render_data(true);
            }
            mv->colorstitch_slot_to_profile_id[slot_to_free] = 0;
        }
        if (m_active_slot == slot_to_free) m_active_slot = 0;
        update_model_object();
        // NEOTKO_PROFILE_TAG — s238: mismo caso que "Erase all" (ver el comentario
        // largo allí). Acabamos de poner a 0 el puntero slot→perfil bajo un snapshot
        // de undo; el gc aquí borraría el auto-profile que ese puntero acaba de
        // soltar, y el undo devolvería el puntero pero no la receta. El gc de
        // `on_shutdown` lo recogerá igual si de verdad ya no lo usa nadie.
        refresh_selector_palettes();
        m_parent.set_as_dirty();
    }
}

// s169 F3 — departamento Object: toggle "MixedFilament Object" + swatch
// (código movido tal cual desde render_pro_mode_panel, ver su historial) +
// tarjeta "Live recipe" (nueva): lee el perfil resuelto por el último apply
// (Print::mixed_filament_sandwich_profile_id) y lo muestra apilado + descrito
// pase a pase + el color Result en vivo. Cierra pendientes s162 #1 y #2.
void GLGizmoColorStitchPainter::render_object_department()
{
    namespace CS = Slic3r::ColorSci;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const bool mf_has_mixed_filament = object_has_mixed_filament(mo);

    // s230 — la rejilla (TD) sale de Pro y pasa a vivir SOLO aquí, y ARRIBA DEL TODO:
    // antes se dibujaba al final de esta función, detrás de los dos `return` tempranos
    // (sin MixedFilament asignado / con el modo apagado), así que era inaccesible en el
    // caso más común. TD es project-wide (neotko_td_mirror), no depende del objeto ni
    // del modo, así que no tiene por qué estar gateada por ellos. De ahí el nombre
    // nuevo del departamento: "Object & TD".
    // Editarla dispara el mismo save()+SCHEDULE_BACKGROUND_PROCESS de la Fase 1 de
    // s167/s168 → apply → resolve_mixed_filament_sandwich_profiles() recalcula → el
    // live recipe de abajo se refresca solo (próximo frame post-reslice).
    // s318 — sección con nombre en vez de "(TD)" suelto. TD vive SOLO aquí (decisión del usuario).
    neo_section_g(_u8L("Transmission distance").c_str(), Glyph::Spot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Swatches below are re-predicted live using these TD values").c_str());
    render_td_grid();

    bool mf_mode_on = false;
    if (mo) {
        if (const auto* mf_opt = dynamic_cast<const ConfigOptionBool*>(mo->config.option("mixed_filament_sandwich_mode")))
            mf_mode_on = mf_opt->value;
    }

    neo_section_g(_u8L("MixedFilament Object").c_str(), Glyph::Mix);
    m_imgui->disabled_begin(!mf_has_mixed_filament);
    if (neo_glyph_toggle_row("##mf_obj", Glyph::Cube, _u8L("MixedFilament Object").c_str(), &mf_mode_on)
        && mo && mf_has_mixed_filament) {
        wxGetApp().plater()->take_snapshot("Toggle MixedFilament Object");
        mo->config.set_key_value("mixed_filament_sandwich_mode", new ConfigOptionBool(mf_mode_on));
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", mf_has_mixed_filament
            ? _u8L("Replace this object's top surface and penultimate infill with an "
                   "auto-generated sandwich approximating this MixedFilament's colour "
                   "(Perimeter Override forced on). Disables painting/patterns for this "
                   "object.").c_str()
            : _u8L("Assign a MixedFilament to this object's extruder first.").c_str());

    if (mf_has_mixed_filament) {
        // s318 — el swatch va en su propia línea: la fila del interruptor llega hasta el borde.
        // Swatch: resolved profile from the last apply (best-effort — may be one
        // apply-cycle stale right after editing the MixedFilament itself).
        uint32_t mf_argb = 0;
        if (mo) {
            const Print& print = wxGetApp().plater()->fff_print();
            for (const PrintObject* po : print.objects()) {
                if (po->model_object() != mo) continue;
                if (const int pid = print.mixed_filament_sandwich_profile_id(po); pid)
                    if (const auto* p = SurfaceEffectProfileManager::get().find(pid))
                        mf_argb = p->preview_argb;
                break;
            }
        }
        const float sw = ImGui::GetTextLineHeight();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (mf_argb) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = IM_COL32((mf_argb >> 16) & 0xFF, (mf_argb >> 8) & 0xFF, mf_argb & 0xFF, 255);
            dl->AddRectFilled(p0, ImVec2(p0.x + sw, p0.y + sw), col, 3.f);
            dl->AddRect(p0, ImVec2(p0.x + sw, p0.y + sw), IM_COL32(0, 0, 0, 80), 3.f);
        }
        ImGui::Dummy(ImVec2(sw, sw));
    }
    ImGui::Spacing();

    if (!mf_has_mixed_filament) {
        ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
        m_imgui->text_colored(neo_col(NeoCol::TextDim),
            _u8L("Assign a MixedFilament to this object's extruder to use this department."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (!mf_mode_on) {
        ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
        m_imgui->text_colored(neo_col(NeoCol::TextDim),
            _u8L("Turn on \"MixedFilament Object\" above to replace this object's painting "
                 "with an auto-generated sandwich approximating its MixedFilament colour."));
        ImGui::PopTextWrapPos();
        return;
    }

    // ---- Live recipe --------------------------------------------------------
    int pid = 0;
    {
        const Print& print = wxGetApp().plater()->fff_print();
        for (const PrintObject* po : print.objects()) {
            if (po->model_object() != mo) continue;
            pid = print.mixed_filament_sandwich_profile_id(po);
            break;
        }
    }

    neo_section_g(_u8L("Live recipe").c_str(), Glyph::TabPro);
    if (pid == 0) {
        m_imgui->text_colored(neo_col(NeoCol::TextDim),
            _u8L("No recipe resolved yet — slice once to resolve"));
    } else if (const SurfaceEffectProfile* p = SurfaceEffectProfileManager::get().find(pid)) {
        const SurfacePassStack st_top  = SurfacePassStack::from_json(p->stack_top_json);
        const SurfacePassStack st_penu = SurfacePassStack::from_json(p->stack_penu_json);
        const double lh = GLGizmoColorStitchPainter_layer_height();

        std::vector<std::string> fcolors;
        CS::Material mats[4];
        gizmo_materials(mats, fcolors);

        // a) caja apilada Top sobre Penu, a escala real de ratios (mismo dibujo
        // que el tooltip de la rejilla de paletas — draw_zone).
        {
            const ImVec2 tp = ImGui::GetCursorScreenPos();
            const float  zw = std::max(80.f, ImGui::GetContentRegionAvail().x * 0.5f);
            const float  zh = ImGui::GetTextLineHeight() * 4.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (st_penu.passes.empty()) {
                draw_zone(dl, tp, ImVec2(tp.x + zw, tp.y + zh), st_top, false, fcolors);
            } else {
                const float midy = tp.y + zh * 0.5f;
                draw_zone(dl, tp, ImVec2(tp.x + zw, midy - 1.f), st_top,  false, fcolors);
                draw_zone(dl, ImVec2(tp.x, midy + 1.f), ImVec2(tp.x + zw, tp.y + zh), st_penu, true, fcolors);
            }
            ImGui::Dummy(ImVec2(zw, zh));
        }

        // b) una línea de texto por pase (orden visual = orden físico, como en
        // draw_zone_editor: #1 = pase más superficial de la zona).
        ImGui::TextUnformatted(_u8L("Top:").c_str());
        {
            const int n = (int)st_top.passes.size();
            for (int i = n - 1; i >= 0; --i)
                ImGui::BulletText("%s", pass_desc_line(n - i, st_top.passes[i], false, lh).c_str());
        }
        if (!st_penu.passes.empty()) {
            ImGui::TextUnformatted(_u8L("Penultimate:").c_str());
            const int n = (int)st_penu.passes.size();
            for (int i = n - 1; i >= 0; --i)
                ImGui::BulletText("%s", pass_desc_line(n - i, st_penu.passes[i], true, lh).c_str());
        }

        // c) swatch Result en vivo (mismo motor que Pro: sandwich_colour_stacked
        // + resolve_object_base_bg — NO el preview_argb cacheado del perfil).
        {
            float bg[3] = {0.f, 0.f, 0.f};
            resolve_object_base_bg(mats, bg);
            float out[3] = {0.f, 0.f, 0.f};
            CS::sandwich_colour_stacked(st_top, st_penu, mats, bg, ColorStitchPaintPreview::weave_layer_height(), out);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const float  rh = ImGui::GetTextLineHeight() * 1.6f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(rp, ImVec2(rp.x + rh, rp.y + rh),
                              IM_COL32((int)std::min(255.f, out[0] * 255.f),
                                       (int)std::min(255.f, out[1] * 255.f),
                                       (int)std::min(255.f, out[2] * 255.f), 255));
            neo_halo(dl, rp, ImVec2(rp.x + rh, rp.y + rh), 3.f);
            ImGui::Dummy(ImVec2(rh, rh));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_u8L("Result"));
        }
    }

    // s230 — la rejilla (TD) que se repetía aquí subió al principio de la función
    // (siempre visible, fuera de los `return` tempranos). Ver comentario allí.

    ImGui::Spacing();
    ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
    m_imgui->text_colored(neo_col(NeoCol::TextDim),
        _u8L("Passes shown come from the last slice apply — may lag one cycle "
             "right after editing the MixedFilament itself."));
    ImGui::PopTextWrapPos();
}

// NEOTKO_COLORSTITCH_TAG — s231 F6: parámetros del PINCEL y de la VISTA. Extraídos de
// dentro del departamento Palette (donde eran inalcanzables desde Pro/Object aunque el
// pincel siguiera pintando) para dibujarse una sola vez, fuera del switch. Mismo
// contenido y mismos anchos de slider que tenían; sólo cambia dónde se dibujan.
void GLGizmoColorStitchPainter::render_brush_and_view()
{
    // s318 — sección plegable del lenguaje nuevo (antes CollapsingHeader de Orca), y los dos
    // deslizadores SIN la cajita numérica de al lado (decisión del usuario): el valor va dentro
    // del deslizador y se teclea con Ctrl+clic, que es nativo de ImGui.
    if (!neo_section_toggle("##cmp_brush_view", _u8L("Brush & view").c_str(), Glyph::Paint, &m_brush_open))
        return;

    const std::string fmt = std::string("%.1f") + I18N::translate_utf8("°", "deg");
    if (neo_row_slider("##cmp_smart_fill_angle", into_u8(m_desc.at("smart_fill_angle")).c_str(),
                       &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, fmt.c_str(), nullptr))
        for (auto& sel : m_triangle_selectors) {
            sel->seed_fill_unselect_all_triangles();
            sel->request_update_render_data();
        }

    // ---- Clipping plane ----
    {
        auto clp = float(m_c->object_clipper()->get_position());
        if (neo_row_slider("##cmp_clp", into_u8(m_desc.at("clipping_of_view")).c_str(), &clp, 0.f, 1.f, "%.2f", nullptr))
            m_c->object_clipper()->set_position_by_ratio(clp, true);
        if (m_c->object_clipper()->get_position() != 0.f) {
            // El deslizador ocupa la fila entera: el reset va debajo, alineado con él.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cs_label_col());
            if (neo_text_button(into_u8(m_desc.at("reset_direction")).c_str(), NeoBtn::Ghost))
                wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
        }
    }
    ImGui::Dummy(ImVec2(0.f, 0.2f * neo_u()));

    // s232 — realce del slot en el viewport (una AYUDA: tiene que poder apagarse). El contador que
    // antes iba al lado entra en la etiqueta; "not painted here" es un aviso y va a la bandeja.
    {
        std::string lbl = _u8L("Highlight active colour");
        if (m_slot_highlight && m_hl_slot_built > 0) {
            if (m_hl_facets > 0)
                lbl += "  · s" + std::to_string(m_hl_slot_built) + " — " + std::to_string(m_hl_facets);
            else
                tray("s" + std::to_string(m_hl_slot_built) + " — " + _u8L("not painted here"));
        }
        if (neo_glyph_toggle_row("##cmp_hl", Glyph::Spot, lbl.c_str(), &m_slot_highlight,
                                 _u8L("Outline in the 3D view the faces painted with the active "
                                      "colour — or with the one under the cursor while hovering a "
                                      "swatch or a row of \"In use on this object\".").c_str())) {
            m_hl_dirty   = true;
            m_auto_dirty = true;   // s280d
            m_parent.set_as_dirty();
        }
    }
    // s280d — marcador de ángulo AUTO (contorno violeta que pulsa).
    {
        std::string lbl = _u8L("Mark bands with no angle");
        if (m_auto_marker && m_auto_slots > 0)
            lbl += "  · " + std::to_string(m_auto_slots);
        if (neo_glyph_toggle_row("##cmp_auto", Glyph::NoAngle, lbl.c_str(), &m_auto_marker,
                                 _u8L("Outline in violet, with a slow pulse, the zones whose fill "
                                      "angle is left on auto (-1). There the slicer flips the angle "
                                      "by 90 degrees every other layer, so no single direction can "
                                      "be previewed. Applies to ColorStitch and to PathBlend — in "
                                      "PathBlend the angle also decides the gradient, not just the "
                                      "line direction. Set a fixed angle (wheel over the bar) to "
                                      "lock it.").c_str())) {
            m_auto_dirty = true;
            m_parent.set_as_dirty();
        }
    }
    // s233 — la pintura visible con el gizmo CERRADO (global, no por objeto).
    {
        auto* ac = wxGetApp().app_config;
        bool show_outside = ColorStitchPaintPreview::show_outside_gizmo();
        if (neo_glyph_toggle_row("##cmp_outside", Glyph::Eye, _u8L("Keep paint visible outside this gizmo").c_str(),
                                 &show_outside,
                                 _u8L("Show the painted zones in the normal 3D view, with this "
                                      "gizmo closed. Flat colour per slot — the woven/gradient "
                                      "preview only exists inside the painter.").c_str())) {
            if (ac) ac->set("neotko_show_paint_outside_gizmo", show_outside ? "1" : "0");
            m_parent.set_as_dirty();
        }
    }
}

void GLGizmoColorStitchPainter::on_render_input_window(float x, float y, float bottom_limit)
{
    // s232 — el hover que alimenta el realce del viewport se acumula durante ESTE
    // render (hover_slot() desde los swatches y el inventario) y se publica al
    // final. Se reinicia aquí, así que sacar el ratón del panel apaga el realce
    // de consulta y devuelve el mando al slot activo.
    m_hover_slot_next = 0;

    // s111 — sin objeto activo (recién abierto / selección vacía): panel mínimo
    // que invita a elegir objetos. Se fuerza el modo Select para que el clic en
    // escena marque (con picking encendido). El panel completo aparece en cuanto
    // hay un objeto activo.
    if (!m_c->selection_info() || !m_c->selection_info()->model_object()) {
        const float ah = m_imgui->scaled(22.f);
        y = std::min(y, bottom_limit - ah);
#if BBS_TOOLBAR_ON_TOP
        GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
        GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
        neo_push_window_style();   // s318 — antes del Begin (GizmoNeotkoStyle.hpp explica por qué)
        GizmoImguiBegin(get_name(),
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
                      | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        neo_push_panel_style();
        // s317 fase E: fuera el "(WIP Beta)".
        ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
        // s231 F2 — el texto prometía algo que el código impedía (ver la nota de
        // on_set_state): en modo pintar el click sobre un objeto no marcado se
        // consumía y no pasaba nada. Ahora el gizmo arranca en Select con la selección
        // vacía, así que el mensaje ya es cierto — y lo dice explícitamente.
        m_imgui->text(_u8L("Select tool is active: click the objects you want to paint "
                           "(Shift-click removes one). Then pick a colour and paint — "
                           "you can paint several objects in one go."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        render_tool_row();
        neo_pop_panel_style();
        GizmoImguiEnd();
        neo_pop_window_style();
        return;
    }

    const float approx_height = m_imgui->scaled(22.f);
    y = std::min(y, bottom_limit - approx_height);
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    neo_push_window_style();   // s318 — antes del Begin (GizmoNeotkoStyle.hpp explica por qué)
    // s319 — RED DE SEGURIDAD: la ventana es AlwaysAutoResize, y cualquier item que caiga fuera del
    // borde la ensancha, lo que mueve el borde, lo que… (el panel creció hasta tapar el 3D). Con un
    // tope de ancho, un descuido futuro se ve como algo recortado, no como el panel comiéndose la vista.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(34.f * ImGui::GetFontSize(), FLT_MAX));
    GizmoImguiBegin(get_name(),
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
                  | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    // 🚨 s318 — el estilo del panel se empuja y se saca AQUÍ, y el cuerpo vive en su propia función:
    // un PushStyleColor cuyo Pop se salta un `return` contamina TODAS las ventanas ImGui de Orca.
    neo_push_panel_style();
    neo_set_label_col_fn(&cs_label_col);
    render_panel_body(x, y);
    neo_pop_panel_style();
    GizmoImguiEnd();
    neo_pop_window_style();
}

// s318 — el cuerpo del panel (antes, en línea dentro de on_render_input_window). Orden de arriba
// abajo: tarjeta del color activo · herramientas · pestañas · departamento · Brush & view ·
// BANDEJA de avisos. Todo lo que avisa durante el frame escribe en m_tray y se pinta al final.
void GLGizmoColorStitchPainter::render_panel_body(float x, float y)
{
    m_tray.clear();
    m_stkc_colours_shown = false;   // s364 — lo enciende render_sticker_editor si dibuja Colours
    {   // s350 — el color GUARDADO de cada receta (preview_argb: vista 3D, stickers) al día con el
        // modelo línea a línea y con los filamentos/TD/fondo actuales. Palette ya lo recalcula en
        // vivo; esto es para lo que lee el guardado. Sólo cuando algo cambia (clave).
        namespace CS = Slic3r::ColorSci;
        CS::Material mats[4];
        std::vector<std::string> fc;
        gizmo_materials(mats, fc);
        float bg[3] = { 0.f, 0.f, 0.f };
        resolve_object_base_bg(mats, bg);
        auto& mgr = SurfaceEffectProfileManager::get();
        size_t key = mgr.list().size();
        auto mix = [&key](float v) { key ^= std::hash<long>{}(std::lround(v * 10000.f)) + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2); };
        for (const CS::Material& m : mats) for (int c = 0; c < 3; ++c) { mix(m.rgb[c]); mix(m.td[c]); }
        for (int c = 0; c < 3; ++c) mix(bg[c]);
        static size_t s_argb_key = 0;
        if (key != s_argb_key) {
            s_argb_key = key;
            std::vector<int> ids;
            for (const SurfaceEffectProfile& p : mgr.list())
                if (!p.stack_top_json.empty() || !p.stack_bottom_json.empty()) ids.push_back(p.id);
            int changed = 0;
            for (const int id : ids)
                if (SurfaceEffectProfile* p = mgr.find_mut(id)) {
                    float out[3] = { 0.f, 0.f, 0.f };
                    if (p->stack_top_json.empty())   // s350: receta sólo Bottom → vista desde la cama
                        CS::sandwich_colour_bottom(SurfacePassStack::from_json(p->stack_bottom_json), mats, bg, ColorStitchPaintPreview::weave_layer_height(), out);
                    else
                        CS::sandwich_colour_stacked(SurfacePassStack::from_json(p->stack_top_json),
                                                    SurfacePassStack::from_json(p->stack_penu_json), mats, bg, ColorStitchPaintPreview::weave_layer_height(), out);
                    CS::ColorRecipe r;
                    r.rgb = { out[0], out[1], out[2] };
                    const uint32_t argb = recipe_argb(r);
                    if (p->preview_argb != argb) { p->preview_argb = argb; ++changed; }
                }
            if (changed) { m_preview_dirty = true; m_sticker_overlay_built_for = -1; }
            NEOTKO_LOG(PROFILE, "PREVIEW_ARGB_REFRESH profiles=" << ids.size() << " changed=" << changed);
        }
    }
    const float u = neo_u();

    float caption_max = 0.f;
    for (const auto& t : { "paint", "erase", "smart_fill_angle", "clipping_of_view" })
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[std::string(t) + "_caption"]).x);
    caption_max += m_imgui->scaled(1.f);

    // s169 F1 — ancho estable entre departamentos (la ventana es AlwaysAutoResize). s318: en neo_u.
    ImGui::Dummy(ImVec2(std::max(m_imgui->scaled(24.f), 28.f * u), 0.f));
    // 🚨 Mismo tope de ajuste que Zonas (s286c): sin él una frase larga ESTIRA la ventana.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);

    render_header();

    // NEOTKO_PROFILE_TAG — Smart-Fill es la única herramienta, fijada sea cual sea el departamento.
    m_current_tool = ImGui::FillButtonIcon;
    m_cursor_type  = TriangleSelector::CursorType::POINTER;
    m_tool_type    = ToolType::SMART_FILL;

    // s169 F3 — MixedFilament Object gobierna el objeto: Palette/Generator/Pro se deshabilitan.
    const bool mf_mode_on = active_object_mixed_filament_mode();

    // ---- Herramientas (globales desde s169) + ? + Erase all, en una fila ----
    ImGui::Dummy(ImVec2(0.f, 0.3f * u));
    m_imgui->disabled_begin(mf_mode_on);
    {
        const float bs     = 2.1f * u;
        const float well_h = bs + 6.f;                  // = render_tool_row: bs + 2·pad
        const float help_h = 25.f * m_parent.get_scale() + 2.f * ImGui::GetStyle().FramePadding.y;
        const float help_w = 25.f * m_parent.get_scale() + 2.f * ImGui::GetStyle().FramePadding.x;
        const float row_y  = ImGui::GetCursorPosY();
        render_tool_row();
        // s319 — alineado a la derecha con el ancho MEDIDO en el frame anterior (el del botón ? de
        // Orca no se puede calcular de antemano). Estimarlo corto empujaba la ventana cada frame.
        const float right   = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float group_w = (m_tool_right_w > 0.f) ? m_tool_right_w : (help_w + 0.9f * u + 1.f + bs);
        neo_same_line_at(right - group_w);
        const float group_x0 = ImGui::GetCursorScreenPos().x;
        ImGui::SetCursorPosY(row_y + std::max(0.f, (well_h - help_h) * 0.5f));
        const float cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
        show_tooltip_information(caption_max, x, cur_y);
        ImGui::SameLine(0.f, 0.45f * u);
        ImGui::SetCursorPosY(row_y + (well_h - bs) * 0.5f);
        {
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(sp.x, sp.y + 0.25f * u), ImVec2(sp.x, sp.y + bs - 0.25f * u),
                        neo_col_u32(NeoCol::SurfaceHi), 1.f);
            ImGui::Dummy(ImVec2(1.f, bs));
        }
        ImGui::SameLine(0.f, 0.45f * u);
        ImGui::SetCursorPosY(row_y + (well_h - bs) * 0.5f);
        // s173/s231 F6 — la ÚNICA acción irreversible de la fila: glifo de peligro (gris que se pone
        // ámbar al pasar) y confirmación con el número de objetos afectados.
        const bool _remove_all_clicked = neo_glyph_button("##cs_erase_all", bs, Glyph::EraseAll,
                                                          /*danger=*/true, _u8L("Erase all painting").c_str());
        m_tool_right_w = ImGui::GetItemRectMax().x - group_x0;
        if (_remove_all_clicked)
            ImGui::OpenPopup("##cs_erase_all_confirm");
        bool _do_erase_all = false;
        if (ImGui::BeginPopup("##cs_erase_all_confirm")) {
            const int _n_obj = (int)m_marked_objects.size()
                + ((m_parent.get_selection().get_object_idx() >= 0
                    && !m_marked_objects.count(m_parent.get_selection().get_object_idx())) ? 1 : 0);
            ImGui::TextUnformatted(_u8L("Erase all painting?").c_str());
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "%s: %d",
                               _u8L("Objects affected").c_str(), std::max(1, _n_obj));
            ImGui::Separator();
            if (m_imgui->button(_L("Erase"))) { _do_erase_all = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (m_imgui->button(_L("Cancel"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (_do_erase_all) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset ColorStitch paint"),
                                      UndoRedo::SnapshotType::GizmoAction);
        // s111: borra la pintura de TODOS los objetos marcados, no solo el activo.
        ModelObject* active_mo = m_c->selection_info()->model_object();
        const int    active_oid = m_parent.get_selection().get_object_idx();
        const Model* model = m_parent.get_selection().get_model();

        // Conjunto a limpiar = marcados ∪ activo.
        std::set<int> to_clear = m_marked_objects;
        if (active_oid >= 0) to_clear.insert(active_oid);

        for (int oid : to_clear) {
            if (!model || oid < 0 || oid >= (int)model->objects.size()) continue;
            ModelObject* mo = model->objects[oid];
            if (oid == active_oid && active_mo) {
                // Activo: resetear sus triangle_selectors en vivo.
                int idx = -1;
                for (ModelVolume* mv : active_mo->volumes) {
                    if (!mv->is_model_part()) continue;
                    ++idx;
                    if (idx < (int)m_triangle_selectors.size()) {
                        m_triangle_selectors[idx]->reset();
                        m_triangle_selectors[idx]->request_update_render_data(true);
                    }
                    std::fill(std::begin(mv->colorstitch_slot_to_profile_id),
                              std::end  (mv->colorstitch_slot_to_profile_id), 0);
                }
            } else {
                // No-activos: limpiar la pintura guardada directamente en el modelo.
                for (ModelVolume* mv : mo->volumes) {
                    if (!mv->is_model_part()) continue;
                    mv->colorstitch_paint_facets.reset();
                    std::fill(std::begin(mv->colorstitch_slot_to_profile_id),
                              std::end  (mv->colorstitch_slot_to_profile_id), 0);
                }
            }
        }
        // s231 F0 — vía el invalidador único (aquí sí se tira también la receta: no
        // queda pintura ni color de trabajo al que referirse).
        invalidate_active_binding(/*keep_recipe=*/false);
        m_preview_dirty       = true;
        // NEOTKO_PROFILE_TAG — s238: AQUÍ NO SE RECOGE BASURA. Este bloque acaba de
        // poner a cero `colorstitch_slot_to_profile_id` de todo lo borrado; llamar al gc
        // justo después es preguntar "¿quién referencia algo?" un instante después de
        // haber borrado la única respuesta posible. No es una carrera de hilos: es una
        // carrera lógica que se cumple SIEMPRE, y se lleva por delante el auto-profile
        // de la pintura recién borrada.
        //
        // Por qué eso rompe datos: el snapshot de undo de arriba restaura la PINTURA
        // (facetas + tabla de slots, que son del ModelVolume), pero el
        // SurfaceEffectProfileManager es un singleton global que NO está en el undo
        // stack. Secuencia real del bug: pintar → "Erase" → Ctrl+Z → la pintura vuelve,
        // la receta no → se guarda un 3mf con slots huérfanos que no rebana en ninguna
        // máquina. Reproducido con el 3mf de un usuario (68 facetas en slot 1 → id 1,
        // cero recetas en el fichero).
        //
        // Quitarlo no filtra nada: si al final el auto-profile sigue sin referencias,
        // el gc de `on_shutdown` lo recoge igual al cerrar el gizmo — pero para
        // entonces el undo ya ha tenido su oportunidad de devolver las referencias.
        update_model_object();
        refresh_selector_palettes();
        m_parent.set_as_dirty();
        }
    }
    m_imgui->disabled_end();
    ImGui::Dummy(ImVec2(0.f, 0.25f * u));

    // ---- Departamentos (s169; nombres s169; s318 con glifo) ----
    {
        const std::string lbl0 = _u8L("Palette"),   lbl1 = _u8L("Generator"),
                           lbl2 = _u8L("Pro"),       lbl3 = _u8L("Object & TD"),
                           lbl4 = _u8L("Stickers");
        const std::string tip0 = _u8L("Paint / erase / pick colours on the model");
        const std::string tip1 = _u8L("Generated palettes: gradient ramp / flat colour");
        const std::string tip2 = _u8L("Compose Top / Penultimate / Bottom");
        const std::string tip3 = _u8L("MixedFilament Object — object-wide sandwich + Transmission Distance");
        const std::string tip4 = _u8L("SVG stickers on the model:\n"
                                      "load, place, move, rotate, scale\n"
                                      "and order them as layers (top one covers the rest)");
        const char* labels[5] = { lbl0.c_str(), lbl1.c_str(), lbl2.c_str(), lbl3.c_str(), lbl4.c_str() };
        const char* tips[5]   = { tip0.c_str(), tip1.c_str(), tip2.c_str(), tip3.c_str(), tip4.c_str() };
        const Glyph glyphs[5] = { Glyph::TabPalette, Glyph::TabGen, Glyph::TabPro, Glyph::Cube, Glyph::Sticker };
        const int dep_before = m_department;
        cs_department_tabs(labels, tips, glyphs, 5, m_department);
        // s348 — entrar en Stickers enciende la herramienta Sticker; salir devuelve la que había
        // (si seguía siendo Sticker: si el usuario cambió de herramienta dentro, se respeta).
        // s364 — «Edit in Pro» deja la edición del sticker viva estando en Pro: salir de Pro a mano
        // cuenta como salir de Stickers (antes la herramienta se quedaba en Sticker, huérfana).
        if (dep_before != m_department) {
            const bool was_sticker_ctx = dep_before == DEP_STICKERS || (dep_before == DEP_PRO && m_pro_from_sticker);
            m_pro_from_sticker = false;
            if (m_department == DEP_STICKERS) {
                if (!was_sticker_ctx) {
                    m_tool_before_stickers = (int)current_tool();
                    set_tool(TOOL_STICKER);
                }
            } else if (was_sticker_ctx) {
                if (m_editing_sticker_idx >= 0) exit_sticker_edit();
                if (current_tool() == TOOL_STICKER)
                    set_tool(m_tool_before_stickers >= 0 && m_tool_before_stickers != TOOL_STICKER
                                 ? (Tool)m_tool_before_stickers : TOOL_PAINT);
                m_tool_before_stickers = -1;
            }
        }
    }
    ImGui::Dummy(ImVec2(0.f, 0.2f * u));

    // s169 F3 — el banner de MixedFilament: a la bandeja (s318), una vez, en los tres departamentos
    // que deshabilita. Object & TD es el que gestiona el interruptor y nunca se deshabilita.
    if (mf_mode_on && m_department != DEP_OBJECT)
        tray(_u8L("MixedFilament Object governs this object — painting is disabled"));

    switch (m_department) {
    case DEP_PALETTE: { // Palette — biblioteca (grupo + rejilla) y slots en uso. Stickers: pestaña propia (s348).
        // s318 — TD ya NO vive aquí: sólo en Object & TD (decisión del usuario, s318). La rejilla
        // se sigue prediciendo con esos TD; lo dice el tooltip de la sección Library.
        m_imgui->disabled_begin(mf_mode_on);
        render_group_selector();          // s318 — incluye "Save all" (antes, suelto aquí)
        render_paint_palette_grid();
        render_slots_in_use();            // s231 F6
        m_imgui->disabled_end();
        break;
    }
    case DEP_GENERATOR: { // Generator — elige entre paletas GENERADAS (no crea nada).
        m_imgui->disabled_begin(mf_mode_on);
        render_palette_panel(ImGui::GetContentRegionAvail().x);
        m_imgui->disabled_end();
        break;
    }
    case DEP_PRO: { // Pro
        m_imgui->disabled_begin(mf_mode_on);
        render_pro_mode_panel();
        m_imgui->disabled_end();
        break;
    }
    case DEP_STICKERS: { // s348 — Stickers (SVG): cargar + capas
        m_imgui->disabled_begin(mf_mode_on);
        render_stickers_department();
        m_imgui->disabled_end();
        break;
    }
    default: { // Object & TD — MixedFilament Object + TD
        render_object_department();
        break;
    }
    }

    // s231 F6 — pincel + vista, comunes a todos los departamentos.
    m_imgui->disabled_begin(mf_mode_on);
    render_brush_and_view();
    m_imgui->disabled_end();

    // s231 F3 — red de seguridad del re-slice diferido: el commit normal vive al final
    // de render_pro_mode_panel, pero si el usuario suelta el arrastre y cambia de
    // departamento en el mismo gesto, ese código ya no se ejecuta y el trabajo pendiente
    // se quedaría sin agendar. Aquí se dispara pase lo que pase.
    if (m_pro_reslice_pending && !ImGui::IsAnyItemActive()) {
        m_pro_reslice_pending = false;
        refresh_selector_palettes();
        m_preview_dirty = true;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }

    // s232 — publica el hover acumulado. El panel se dibuja DESPUÉS del gizmo en
    // el frame, así que el realce de consulta entra en el siguiente: de ahí el
    // set_as_dirty + extra frame cuando cambia (si no, pasar el ratón por un
    // swatch sin mover la cámara no repintaría nada).
    if (m_hover_slot != m_hover_slot_next) {
        m_hover_slot = m_hover_slot_next;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }

    // ---- s318 — LA BANDEJA: todos los avisos del frame, juntos y al final (R7 de s287) ----
    if (!m_tray.empty()) {
        ImGui::Dummy(ImVec2(0.f, 0.3f * u));
        for (size_t i = 0; i < m_tray.size(); ++i) {
            ImGui::PushID(int(i));
            neo_warn_row("##tray", m_tray[i].text.c_str(),
                         m_tray[i].why.empty() ? nullptr : m_tray[i].why.c_str(), m_tray[i].amber);
            ImGui::PopID();
        }
    }
    ImGui::PopTextWrapPos();
}

// ----------------------------------------------------------------------------
// Model ↔ TriangleSelector sync
// ----------------------------------------------------------------------------

void GLGizmoColorStitchPainter::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
    m_preview_dirty = true;   // s111: la pintura cambió → refrescar preview de marcados
    m_hl_dirty      = true;   // s232: ídem para el contorno del realce
    m_auto_dirty    = true;   // s280d
    const ModelInstance* mi = mo->instances.empty() ? nullptr : mo->instances.front();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        if (idx >= (int)m_triangle_selectors.size() || !m_triangle_selectors[idx]) continue;
        // NEOTKO_COLORSTITCH_TAG — (a) ColorStitch sólo actúa en superficies TOP, así
        // que descartamos la pintura que cayó en paredes laterales/inferiores (el
        // pincel pinta "a través"). Así lo pintado = lo que imprime y el weave no
        // sangra por los lados. Se hace ANTES de serializar para que persista.
        // NEOTKO_BOTTOM_TAG — Fase 1 (§4.0): el descarte ahora es por ZONA del slot.
        // Cada slot apunta a un perfil cuyas zonas activas (top/penu/bottom) dictan
        // qué cara se conserva: top/penu → arriba, bottom → abajo, ambas → ambas;
        // laterales SIEMPRE fuera (mantiene la garantía anti-bleed de s145).
        // Además, para el SLOT que se está pintando (m_active_slot) se aplica la
        // "active zone mode" del plan: si en el Pro tray la zona Bottom WIP está en
        // edición (m_pro_bottom con efecto) conservamos la cara inferior AUNQUE el
        // perfil todavía no la haya persistido — evita que la primera pincelada en la
        // cara inferior se borre antes de que stack_bottom_json exista (trampa de
        // autoría). Sin zona Bottom WIP en ningún perfil ni en edición → bottom todo
        // false → comportamiento idéntico al top-only de s145.
        if (mi) {
            const Transform3d trafo = mi->get_transformation().get_matrix() * mv->get_matrix();
            std::vector<bool> slot_wants_top(MAX_SLOTS, false), slot_wants_bottom(MAX_SLOTS, false);
            {
                const auto& mgr = SurfaceEffectProfileManager::get();
                for (int s = 1; s < MAX_SLOTS; ++s) {
                    const int pid = mv->colorstitch_slot_to_profile_id[s];
                    if (pid == 0) continue;
                    if (const SurfaceEffectProfile* p = mgr.find(pid)) {
                        // penu es superficie del lado TOP → cuenta como top-facing.
                        slot_wants_top[s]    = !p->stack_top_json.empty() || !p->stack_penu_json.empty();
                        slot_wants_bottom[s] = !p->stack_bottom_json.empty();
                    }
                }
                // Active-zone override sobre el slot del pincel (autoría en vivo).
                if (m_active_slot >= 1 && m_active_slot < MAX_SLOTS) {
                    slot_wants_top[m_active_slot]    = slot_wants_top[m_active_slot]
                        || m_pro_top.any_effect() || (m_pro_penu.enabled && m_pro_penu.any_effect());
                    // s232 — `enabled` sólo se sincroniza mientras se DIBUJA el panel Pro
                    // (`m_pro_bottom.enabled = any_effect()` al cargar un perfil), así que
                    // pintando desde el departamento Paint podía estar en false con una
                    // receta Bottom cargada y la red de seguridad no saltaba: el trazo se
                    // veía durante el arrastre y desaparecía al soltar. Basta con que la
                    // zona TENGA efecto.
                    slot_wants_bottom[m_active_slot] = slot_wants_bottom[m_active_slot]
                        || (m_pro_bottom_loaded_id == m_selected_profile_id
                            && m_pro_bottom.any_effect());
                }
            }
            const int cleared = m_triangle_selectors[idx]->discard_non_zone_facing(
                trafo, 0.30f, slot_wants_top, slot_wants_bottom);
            if (NeoDebug::enabled(NeoDebug::BOTTOM)) {
                int n_bottom_slots = 0;
                for (int s = 1; s < MAX_SLOTS; ++s) if (slot_wants_bottom[s]) ++n_bottom_slots;
                std::ostringstream os;
                os << "PAINT_DISCARD vol_idx=" << idx << " cleared=" << cleared
                   << " bottom_slots=" << n_bottom_slots;
                NeoDebug::write(NeoDebug::BOTTOM, os.str());
            }
            if (cleared > 0) {
                if (auto* g = dynamic_cast<TriangleSelectorGUI*>(m_triangle_selectors[idx].get()))
                    g->request_update_render_data(true);
            }
        }
        updated |= mv->colorstitch_paint_facets.set(*m_triangle_selectors[idx]);
    }
    if (updated) {
        if (NeoDebug::enabled(NeoDebug::PROFILE)) {
            int painted_volumes = 0, painted_slots_total = 0;
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                ++painted_volumes;
                for (int s = 1; s < MAX_SLOTS; ++s)
                    if (mv->colorstitch_slot_to_profile_id[s] != 0) ++painted_slots_total;
            }
            std::ostringstream os;
            os << "PAINT update_model_object: volumes=" << painted_volumes
               << " slot_assignments=" << painted_slots_total
               << " selected_profile=" << m_selected_profile_id
               << " active_slot=" << m_active_slot;
            NeoDebug::write(NeoDebug::PROFILE, os.str());
        }
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        refresh_selector_palettes();   // sincroniza m_ebt_colors: el slot recién pintado puede no estar "horneado" todavía
        // El re-slice por paint ya lo dispara el cambio de facetas/slot (timestamp); la
        // huella de contenido la recalcula Print::apply. Solo agendamos el proceso.
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoColorStitchPainter::update_from_model_object(bool /*first_update*/)
{
    // s111: sin wxBusyCursor — ahora se reconstruye también al pre-activar objetos
    // en hover (multi-objeto), y el cursor "ocupado" parpadearía en cada cruce.
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    m_triangle_selectors.clear();
    // s232 — la geometría del realce se construyó desde los selectores que acaban
    // de morir: tirarla aquí evita dibujar el contorno del objeto anterior.
    m_hl_parts.clear();
    m_hl_dirty   = true;
    m_auto_dirty = true;   // s280d
    // Selection is a per-gizmo field; clear it when the active object changes
    // (or when a fresh 3mf is loaded) so the "slots full" warning doesn't fire
    // against a stale profile id whose slot table has been wiped.
    //
    // NEOTKO_COLORSTITCH_TAG — s231 F0, RAÍZ del bug s209 ("el color activo deja de
    // pintar tras Slice o cambio de tab; el swatch sigue igual"). Este bloque ponía
    // id+slot a 0 pero dejaba `m_active_resolved` a TRUE, y ensure_active_slot sale
    // por su puerta rápida `if (m_active_resolved) return m_active_slot;` → devolvía
    // 0 → get_left_button_state_type caía a NONE. Y el guard "sin destino no pintes"
    // (on_mouse) no consumía el click porque m_has_active_recipe seguía siendo true:
    // el resultado exacto que reportó el usuario, click que no hace NADA hasta
    // re-elegir en la paleta (que es lo único que reponía resolved=false).
    // Ahora la invalidación es un único punto que deja el estado COHERENTE: se
    // conserva la intención de color (lo que el swatch enseña) y se tira el enlace,
    // que se re-materializa solo en el primer trazo.
    invalidate_active_binding(/*keep_recipe=*/true);
    if (!mo) return;

    // NEOTKO_COLORSTITCH_TAG — s232: REPARACIÓN de los objetos ya dañados por el agujero
    // de `slot_for_selected_profile` (ver su nota): volúmenes con CARAS pintadas en un
    // slot cuya celda slot→perfil quedó a 0. Aquí no hay nada que adivinar — el número
    // de slot es el mismo para todo el objeto, así que el perfil correcto es el que
    // tiene ese mismo slot en cualquier volumen hermano; sin él, esas caras son pintura
    // huérfana que el preview no puede colorear y que la pipeta lee como "nada".
    // Se hace una vez al (re)cargar el objeto, ANTES de construir los selectores, para
    // que nazcan ya con su paleta y su tejido.
    if (ModelObject* mo_mut = m_c->selection_info()->model_object()) {
        // Perfil de referencia por slot = el primero que lo tenga mapeado.
        std::vector<int> pid_of_slot(MAX_SLOTS, 0);
        for (const ModelVolume* mv : mo_mut->volumes) {
            if (!mv->is_model_part()) continue;
            for (int s = 1; s < MAX_SLOTS; ++s)
                if (pid_of_slot[s] == 0 && mv->colorstitch_slot_to_profile_id[s] != 0)
                    pid_of_slot[s] = mv->colorstitch_slot_to_profile_id[s];
        }
        for (ModelVolume* mv : mo_mut->volumes) {
            if (!mv->is_model_part()) continue;
            // Las caras se cuentan sobre un selector temporal: los definitivos se
            // construyen justo debajo y necesitan la tabla YA arreglada.
            TriangleSelector probe(mv->mesh());
            probe.deserialize(mv->colorstitch_paint_facets.get_data(), false,
                              static_cast<EnforcerBlockerType>(MAX_SLOTS - 1));
            for (int s = 1; s < MAX_SLOTS; ++s) {
                if (mv->colorstitch_slot_to_profile_id[s] != 0 || pid_of_slot[s] == 0) continue;
                if (probe.num_facets(static_cast<EnforcerBlockerType>(s)) == 0) continue;
                mv->colorstitch_slot_to_profile_id[s] = pid_of_slot[s];
                NEOTKO_LOG(PROFILE, "PAINT slot_repair vol='" << mv->name << "' slot=" << s
                    << " ← pid=" << pid_of_slot[s] << " (pintura huérfana, s232)");
            }
        }
    }

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(
            *mesh, build_ebt_colors_for_volume(mv)));
        const EnforcerBlockerType max_ebt = static_cast<EnforcerBlockerType>(MAX_SLOTS - 1);
        m_triangle_selectors.back()->deserialize(mv->colorstitch_paint_facets.get_data(), false, max_ebt);
        // (b) NEOTKO_COLORSTITCH_TAG — sembrar el weave al (re)construir el selector
        // (entrar al gizmo / cambiar de objeto), no solo en refresh_selector_palettes;
        // sin esto, al volver al gizmo la zona pintada no recuperaba su tejido/ángulo.
        // DESPUÉS de deserialize: así el weave puede medir el área pintada real (get_facets).
        if (auto* tsp = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors.back().get())) {
            tsp->set_ebt_weave(build_ebt_weave_for_volume(mv, tsp));   // per-slot fallback
            std::unordered_map<int,int> fwi;
            std::vector<TriangleSelectorPatch::WeaveParams> wl;
            build_ebt_weave_islands_for_volume(mv, tsp, fwi, wl);
            tsp->set_ebt_weave_islands(std::move(fwi), std::move(wl));
        }
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}

} // namespace Slic3r::GUI
// NEOTKO_PROFILE_TAG_END
