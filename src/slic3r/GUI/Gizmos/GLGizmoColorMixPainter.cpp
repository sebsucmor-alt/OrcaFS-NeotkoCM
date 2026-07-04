// NEOTKO_PROFILE_TAG_START
#include "GLGizmoColorMixPainter.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp" // NEOTKO_MIXEDFIL_SANDWICH_TAG — Print::mixed_filament_sandwich_profile_id
#include "libslic3r/SurfaceColorMix.hpp" // NEOTKO_PROFILE_TAG — NeoDebug PROFILE channel
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/Utils.hpp" // s173 — resources_dir() para cargar los iconos de la toolbar
#include "libslic3r/ColorSci/StackFlatten.hpp" // NEOTKO_COLORSTITCH_TAG — sandwich_colour_stacked (pro mode live preview)
#include "libslic3r/ClipperUtils.hpp"      // NEOTKO_STICKER_TAG — union_ex para el overlay de edición
#include "libslic3r/Tesselate.hpp"         // NEOTKO_STICKER_TAG — triangulate_expolygons_2f para el relleno del overlay

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"          // s111 — render caja-marca
#include "slic3r/GUI/GLShader.hpp"        // s111 — GLShaderProgram (flat)
#include "libslic3r/Geometry.hpp"         // s111 — translation/scale_transform
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp" // NEOTKO_NEOTOWER_TAG — auto-promote tower type from the painter
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

namespace Slic3r::GUI {

// NEOTKO_COLORSTITCH_TAG — s137: palette groups (SIMPLE / disposable, will be
// reworked with MixedFilament TD). A profile's group is encoded as an invisible
// suffix in its `name`: '\x1f' + 'g' + digit. This touches NO schema and NO 3mf
// (name already serializes), and old projects (no suffix) fall into Group 1.
// Strip for display, re-append on save/rename. Group 1 = no suffix (clean + back-compat).
namespace {
constexpr char CS_GROUP_SEP = '\x1f';   // unit separator — group marker
constexpr int  CS_MAX_GROUPS = GLGizmoColorMixPainter::MAX_GROUPS;

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

GLGizmoColorMixPainter::GLGizmoColorMixPainter(GLCanvas3D& parent,
                                               const std::string& icon_filename,
                                               unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
    , m_current_tool(ImGui::FillButtonIcon) // Smart fill — primary use case for ColorMix Painter
{
}

std::string GLGizmoColorMixPainter::on_get_name() const
{
    return _u8L("ColorStitch Painter") /*NEOTKO_COLORSTITCH_TAG*/;
}

// FFF only, no minimum-filament requirement — single-filament prints can still
// benefit from per-surface ColorMix profiles.
bool GLGizmoColorMixPainter::on_is_selectable() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
}

bool GLGizmoColorMixPainter::on_is_activable() const
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
CommonGizmosDataID GLGizmoColorMixPainter::on_get_requirements() const
{
    return CommonGizmosDataID(
                int(CommonGizmosDataID::SelectionInfo)
              | int(CommonGizmosDataID::Raycaster)
              | int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoColorMixPainter::on_shutdown()
{
    garbage_collect_auto_profiles();   // PR.3: recoge colores de trabajo no usados
    m_select_mode = false;             // s111: salir de Select y limpiar marcas
    m_pick_mode   = false;             // s118: salir del eyedropper
    m_sticker_mode = false;            // NEOTKO_STICKER_TAG
    m_editing_sticker_idx = -1;        // NEOTKO_STICKER_TAG
    m_marked_objects.clear();
    m_parent.enable_picking(true);     // base lo hace también; explícito por claridad
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

// s111 — herramienta activa (Select / Bucket / Eraser, mutuamente excluyentes).
// El picking de la escena se mantiene SIEMPRE encendido (lo enciende on_set_state)
// porque hasta en modo pintar necesitamos saber qué objeto hay bajo el cursor
// para auto-activarlo y poder pintar en cualquier objeto del set marcado.
void GLGizmoColorMixPainter::set_tool_mode(bool select, bool erase)
{
    m_select_mode  = select;
    m_erase_mode   = erase;
    m_pick_mode    = false;   // s118: cualquier modo normal apaga el eyedropper
    m_sticker_mode = false;   // NEOTKO_STICKER_TAG: idem para Sticker
    m_editing_sticker_idx = -1;   // NEOTKO_STICKER_TAG: salir de edición si estaba activa
    if (select) {
        const int active_oid = m_parent.get_selection().get_object_idx();
        if (active_oid >= 0) m_marked_objects.insert(active_oid);
    }
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// s111 — al abrir el gizmo: el set marcado (objetos pintables) se siembra desde la
// selección previa; si había varios objetos, se elige uno como ACTIVO (la base solo
// maneja un objeto a la vez) y los demás quedan marcados. Picking ON todo el tiempo.
void GLGizmoColorMixPainter::on_set_state()
{
    const bool turning_on = (m_state == On && !m_prev_on);
    GLGizmoPainterBase::on_set_state();   // (en On apaga picking; lo re-encendemos)
    m_prev_on = (m_state == On);
    if (!turning_on)
        return;

    m_marked_objects.clear();
    m_select_mode = false;                // arranca en modo pintar (bucket)
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
                m_department = 3;
        }
    }

    m_preview_dirty = true;
    m_parent.enable_picking(true);
}

// s111 — activar un objeto como ACTIVO de pintura PRESERVANDO el color elegido.
// Poner el objeto como selección single-full-instance hace que el painter base
// reconstruya sus triangle_selectors (update_from_model_object), pero ESE reset
// pone m_selected_profile_id=0 y m_active_slot=0 → se perdía el color al cambiar
// de objeto (el trazo pintaba NONE = borrar). Aquí guardamos la intención de color
// antes y la re-materializamos en el NUEVO objeto después.
void GLGizmoColorMixPainter::switch_active_object(int object_idx)
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
void GLGizmoColorMixPainter::toggle_mark(int object_idx, bool unmark)
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

bool GLGizmoColorMixPainter::on_mouse(const wxMouseEvent& mouse_event)
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
    // cursor (m_rr del raycast de la base) y enlaza su receta. Pre-activa el objeto
    // al pasar por encima (igual que el modo pintar) para (a) marcarlo/seleccionarlo
    // visualmente y (b) dejar su raycaster listo ≥1 frame antes del click.
    if (m_pick_mode) {
        if ((mouse_event.Moving() || mouse_event.LeftDown()) && !mouse_event.Dragging()) {
            const int hovered = m_parent.get_first_hover_volume_idx();
            int obj_idx = -1;
            if (hovered >= 0) {
                const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
                if (hovered < (int)volumes.size() && volumes[hovered])
                    obj_idx = volumes[hovered]->object_idx();
            }
            if (obj_idx >= 0 && obj_idx != m_parent.get_selection().get_object_idx())
                switch_active_object(obj_idx);   // marca + carga selectores/raycaster

            if (mouse_event.LeftDown()) {
                if (obj_idx < 0) return true;   // vacío: consumir
                // Estado (slot) de la faceta bajo el cursor en el objeto activo.
                int picked_slot = 0;
                const int mid = rr_mesh_id();
                if (mid >= 0 && mid < (int)m_triangle_selectors.size()
                    && m_triangle_selectors[mid]) {
                    const EnforcerBlockerType st = m_triangle_selectors[mid]
                        ->get_state_at(rr_hit(), rr_facet());
                    picked_slot = (int)st;   // 0 = sin pintar; 1..MAX-1 = slot
                }
                pick_recipe_from_object(obj_idx, picked_slot);
                return true;   // consumir: el pick no debe pintar
            }
        }
        return false;   // resto de eventos: cámara/hover normales
    }

    // NEOTKO_STICKER_TAG — herramienta Sticker, dos sub-modos:
    //  (a) edición de un sticker YA colocado (m_editing_sticker_idx>=0, entrado
    //      vía "Edit placement" en la lista de Palette): arrastrar mueve su
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
                m_sticker_dragging = true;
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
                if (mo_c && m_editing_sticker_idx < (int)mo_c->colormix_stickers.size()) {
                    const ModelVolume* hit_mv = volume_for_mesh_id(mo_c, mid);
                    if (hit_mv) {
                        const Vec3d hit_obj = hit_mv->get_matrix() * rr_hit().cast<double>();
                        ColorMixSticker& st = mo_c->colormix_stickers[m_editing_sticker_idx];
                        const double spin_rad = double(m_editing_spin_deg) * M_PI / 180.0;
                        st.transform = Eigen::Translation3d(hit_obj) * Eigen::AngleAxisd(spin_rad, Vec3d::UnitZ())
                                     * Eigen::Scaling(double(m_editing_scale));
                        m_parent.set_as_dirty();
                        m_parent.request_extra_frame();
                    }
                }
            }
            return consumed;
        }

        if ((mouse_event.Moving() || mouse_event.LeftDown()) && !mouse_event.Dragging()) {
            const int hovered = m_parent.get_first_hover_volume_idx();
            int obj_idx = -1;
            if (hovered >= 0) {
                const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
                if (hovered < (int)volumes.size() && volumes[hovered])
                    obj_idx = volumes[hovered]->object_idx();
            }
            if (obj_idx >= 0 && obj_idx != m_parent.get_selection().get_object_idx())
                switch_active_object(obj_idx);

            if (mouse_event.LeftDown()) {
                if (obj_idx < 0 || m_pending_sticker_svg.empty())
                    return true;   // vacío, o nada cargado para colocar: consumir sin pintar
                const int mid = rr_mesh_id();
                const ModelObject* mo_c = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
                if (mid >= 0 && mo_c) {
                    const ModelVolume* hit_mv = volume_for_mesh_id(mo_c, mid);
                    if (hit_mv) place_sticker_at(hit_mv, rr_hit());
                }
                return true;   // consumir: colocar no debe además pintar
            }
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

        // Click sobre un objeto NO-marcado: consumir para que el canvas no lo
        // seleccione/añada (la pintura queda restringida al set). El vacío NO se
        // consume → la cámara sigue rotando con arrastre desde el fondo.
        if (mouse_event.LeftDown() && obj_idx >= 0 && !on_marked)
            return true;
    }

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
void GLGizmoColorMixPainter::ensure_neotower_tower_type()
{
    auto& cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto* t = cfg.option<ConfigOptionEnum<NeoTowerType>>("neotko_tower_type");
    if (!t || t->value == nttNeoTower)
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

PainterGizmoType GLGizmoColorMixPainter::get_painter_type() const
{
    // No dedicated enum value (the field is informational only — never switched on).
    return PainterGizmoType::MM_SEGMENTATION;
}

wxString GLGizmoColorMixPainter::handle_snapshot_action_name(bool shift_down,
                                                             GLGizmoPainterBase::Button /*button_down*/) const
{
    return (shift_down || m_erase_mode) ? _L("Erase ColorStitch paint") : _L("ColorStitch paint");
}

bool GLGizmoColorMixPainter::on_init()
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

void GLGizmoColorMixPainter::render_painter_gizmo()
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
    render_sticker_edit_overlay();   // NEOTKO_STICKER_TAG — no-op si no hay sticker en edición

    glsafe(::glDisable(GL_BLEND));
}

// s111 — (re)construye los selectors de preview para los objetos marcados que NO
// son el activo, leyendo su pintura guardada (color_mix_paint_facets). Lazy: solo
// cuando cambian las marcas / la pintura / el objeto activo (m_preview_dirty).
void GLGizmoColorMixPainter::rebuild_preview_selectors_if_dirty()
{
    if (!m_preview_dirty)
        return;
    m_preview_dirty = false;
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
                mv->mesh(), build_ebt_colors_for_volume(mv));
            sel->deserialize(mv->color_mix_paint_facets.get_data(), false, max_ebt);
            sel->request_update_render_data();
            vec.push_back(std::move(sel));
        }
    }
}

// s111 — dibuja la pintura de los objetos marcados NO-activos (el activo lo pinta
// render_triangles de la base). Replica el setup de uniforms de
// GLGizmoPainterBase::render_triangles, con la malla + trafo de instancia de cada
// objeto. NEOTKO_COLORSTITCH_TAG.
void GLGizmoColorMixPainter::render_marked_paint()
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

EnforcerBlockerType GLGizmoColorMixPainter::get_left_button_state_type() const
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
        const int slot = const_cast<GLGizmoColorMixPainter*>(this)
                             ->slot_for_selected_profile(/*assign_if_missing=*/true);
        if (slot >= 1 && slot < MAX_SLOTS) return static_cast<EnforcerBlockerType>(slot);
        return EnforcerBlockerType::NONE;
    }
    // Color de trabajo nuevo sin enlazar (swatch predict / Pro custom): materializa
    // el slot AUTO al pintar por dedup (ensure_active_slot ENLAZA m_selected_profile_id,
    // así que los siguientes trazos caen por la rama de arriba).
    if (m_has_active_recipe) {
        const int slot = const_cast<GLGizmoColorMixPainter*>(this)->ensure_active_slot();
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

// Deterministic preview color when profile->preview_argb is unset.
static ColorRGBA fallback_color_for_id(int id)
{
    // Golden-ratio hue stepping → distinguishable hues for 15 ids.
    const float hue = std::fmod(0.61803398875f * float(id), 1.f);
    const float s = 0.55f, v = 0.85f;
    const float h6 = hue * 6.f;
    const int   i  = int(std::floor(h6)) % 6;
    const float f  = h6 - std::floor(h6);
    const float p = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r=0, g=0, b=0;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return ColorRGBA(r, g, b, 1.f);
}

static ColorRGBA color_for_profile(const SurfaceEffectProfile& p)
{
    if (p.preview_argb == 0u)
        return fallback_color_for_id(p.id);
    const uint8_t a = (p.preview_argb >> 24) & 0xFF;
    const uint8_t r = (p.preview_argb >> 16) & 0xFF;
    const uint8_t g = (p.preview_argb >>  8) & 0xFF;
    const uint8_t b = (p.preview_argb >>  0) & 0xFF;
    return ColorRGBA(r / 255.f, g / 255.f, b / 255.f, a == 0 ? 1.f : a / 255.f);
}

// ----------------------------------------------------------------------------
// Mini-sandwich preview (NEOTKO_PROFILE_TAG — Fase 6).
// Renders a profile's resolved SurfacePassStack (Top + Penu) inside the row
// using ImGui draw-list primitives — same visual language as the SandwichDialog
// (paint_chip / paint_preview in Tab.cpp): dark 45/45/45 background, real
// filament colours per tool, ColorMix as vertical tool stripes, Solid as a
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

// Participating ColorMix tools for a pass (reads the pass's per-zone override
// kv; the injected payload at save time guarantees these keys are present).
static std::vector<int> cm_tools_from_pass(const SurfacePass& p, bool penu)
{
    const std::string pre = penu ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    std::vector<int> out;
    for (const char* s : { "tool_a", "tool_b", "tool_c", "tool_d" }) {
        const auto it = p.colormix.kv.find(pre + s);
        if (it != p.colormix.kv.end()) {
            try { const int v = std::stoi(it->second); if (v >= 0) out.push_back(v); }
            catch (...) {}
        }
    }
    if (out.empty()) out = { 0, 1 };
    return out;
}

// NEOTKO_COLORSTITCH_TAG — single source of truth for the ColorStitch per-line tool
// sequence used by ALL previews (3D weave + pro-tray strip), built with the SAME
// engine builders the slicer uses (SurfaceColorMix.cpp:1326+): modes 1-3 →
// build_dithered_tools_2color/_3color / build_custom_bands over n_lines; mode 0 →
// pattern string (digit → 0-based physical tool). penu=false → top-role keys.
// Returns physical tool indices (length n_lines for modes 1-3; pattern length for
// mode 0 → caller tiles). Empty kv ⇒ empty result.
static std::vector<int> colorstitch_tool_sequence(
    const std::map<std::string, std::string>& kv, bool penu, int n_lines)
{
    if (kv.empty()) return {};
    n_lines = std::max(1, n_lines);
    const std::string pre = penu ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    auto gi = [&](const char* k, int d) {
        const auto it = kv.find(pre + k);
        if (it != kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return d; };
    auto gd = [&](const char* k, double d) {
        const auto it = kv.find(pre + k);
        if (it != kv.end()) { try { return std::stod(it->second); } catch (...) {} }
        return d; };
    auto gb = [&](const char* k, bool d) {
        const auto it = kv.find(pre + k);
        return it != kv.end() ? (it->second == "1" || it->second == "true") : d; };

    const int    mode = gi("mode", 0);
    const int    ta = gi("tool_a", 0), tb = gi("tool_b", 1), tc = gi("tool_c", 2), td = gi("tool_d", 3);
    const int    pa = gi("pct_a", 50), pb = gi("pct_b", 33);
    const int    ea = gi("easing", 0);
    const double ga = gd("gamma", 1.0), ov = gd("overlap", 0.6);
    const int    ba = gi("band_count_a", 0), bb = gi("band_count_b", 0);
    const int    bc = gi("band_count_c", 0), bd = gi("band_count_d", 0);
    const bool   inv = gb("invert", false);

    std::vector<int> seq;
    switch (mode) {
    case 1: seq = Slic3r::SurfaceColorMix::build_dithered_tools_2color(n_lines, ta, tb, pa, ea, ga); break;
    case 2: seq = Slic3r::SurfaceColorMix::build_dithered_tools_3color(n_lines, ta, tb, tc, pa, pb, ea, ga, ov); break;
    case 3: seq = Slic3r::SurfaceColorMix::build_custom_bands(n_lines, ta, ba, tb, bb, tc, bc, td, bd); break;
    default: {   // mode 0 — pattern string, digit → 0-based physical tool ('1' → 0)
        std::string pat;
        auto pit = kv.find(penu ? std::string("interlayer_colormix_pattern_penultimate")
                                : std::string("interlayer_colormix_pattern_top"));
        if (pit == kv.end() || pit->second.empty()) pit = kv.find("pattern");
        if (pit != kv.end()) pat = pit->second;
        for (char ch : pat) if (ch >= '1' && ch <= '9') seq.push_back(ch - '1');
        if (seq.empty()) seq.push_back(ta);
        break;
    }
    }
    if (inv && seq.size() > 1) std::reverse(seq.begin(), seq.end());
    return seq;
}

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
    case SurfacePassKind::ColorMix: {
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
        case SurfacePassKind::ColorMix:  s += "CM"; break;
        case SurfacePassKind::PathBlend: s += "PB"; break;
        default:                         s += "·"; break; // middle dot
        }
    }
    return s;
}

// NEOTKO_COLORSTITCH_TAG_START — PR.2 render + PR.3 click. Sección colapsable
// con una tira de swatches scroll-horizontal; hover muestra el mini-sandwich
// (draw_zone) + desc. Devuelve el índice de swatch clicado (-1 si ninguno) —
// el caller lo fija como color activo (set_active_recipe).
static int draw_palette_strip(const char* id,
                              const std::vector<Slic3r::ColorSci::ColorRecipe>& pal,
                              const std::vector<std::string>& fcolors,
                              float strip_w, float strip_h)
{
    if (pal.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "—");
        return -1;
    }
    int clicked = -1;
    ImGui::BeginChild(id, ImVec2(strip_w, strip_h), false, ImGuiWindowFlags_HorizontalScrollbar);
    const float gap = 3.f;
    const float sw  = std::max(10.f, strip_h - ImGui::GetStyle().ScrollbarSize - 2.f * gap);
    ImDrawList* dl  = ImGui::GetWindowDrawList();

    for (int i = 0; i < (int)pal.size(); ++i) {
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw))) clicked = i;
        const bool hov = ImGui::IsItemHovered();

        const auto& c = pal[i].rgb;
        const ImU32 col = IM_COL32((int)std::min(255.f, c[0] * 255.f),
                                   (int)std::min(255.f, c[1] * 255.f),
                                   (int)std::min(255.f, c[2] * 255.f), 255);
        dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw), col);
        dl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                    hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255));

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
        ImGui::SameLine(0.f, gap);
    }
    ImGui::NewLine();
    ImGui::EndChild();
    return clicked;
}

// Fila de chips de filamento para elegir el tool de inicio/fin (A/B) del
// gradient. Devuelve el tool clicado (-1 si ninguno); `cur` resalta el activo.
static int draw_tool_selector_row(const char* id, const char* label,
                                  const std::vector<std::string>& fcolors,
                                  int nfil, int cur)
{
    int clicked = -1;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::PushID(id);
    const float sw  = ImGui::GetTextLineHeight() * 1.2f;
    const float gap = 4.f;
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    for (int t = 0; t < nfil; ++t) {
        ImGui::PushID(t);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tool", ImVec2(sw, sw))) clicked = t;
        const bool hov    = ImGui::IsItemHovered();
        const bool active = (t == cur);
        dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw), tool_col_u32(fcolors, t));
        dl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                    active ? IM_COL32(255, 255, 255, 255)
                           : (hov ? IM_COL32(200, 200, 200, 255) : IM_COL32(20, 20, 20, 255)),
                    0.f, 0, active ? 2.f : 1.f);
        if (hov) ImGui::SetTooltip("T%d", t + 1);
        ImGui::PopID();
        ImGui::SameLine(0.f, gap);
    }
    ImGui::NewLine();
    ImGui::PopID();
    return clicked;
}

// Header colapsable + tira de swatches (Flat / Mixed). El Gradient se renderiza
// inline en render_palette_panel para alojar los selectores A/B en medio.
static int draw_palette_section(const char* id, const std::string& label,
                                const std::vector<Slic3r::ColorSci::ColorRecipe>& pal,
                                const std::vector<std::string>& fcolors,
                                float strip_w, float strip_h)
{
    if (!ImGui::CollapsingHeader(label.c_str())) return -1;
    return draw_palette_strip(id, pal, fcolors, strip_w, strip_h);
}

// ---------------------------------------------------------------------------
// Pro mode v2 (s108) — pass rows that mirror the SandwichDialog row anatomy:
// [x] · #N · chips · kind badge · kind selector · per-kind inline fields ·
// hatched preview bar · Z box on the right edge. All three kinds (Solid /
// ColorStitch / PathBlend Half|Full) are editable inline. The produced stack
// is SELF-CONTAINED — ColorMix uses the long region keys in pass.colormix.kv
// (mirror of ColorPredict.cpp penu_dither, the engine-validated shape) and
// PathBlend the v=2 blob in pass.pathblend.kv["blob"] — so a profile saved
// from the tray slices exactly like one authored in the Sandwich Editor.
// ---------------------------------------------------------------------------

// wx SandwichDialog kind_colour(), as ImU32.
static ImU32 pro_kind_colour(SurfacePassKind k)
{
    switch (k) {
    case SurfacePassKind::Solid:     return IM_COL32(214, 124,  48, 255);
    case SurfacePassKind::ColorMix:  return IM_COL32( 72, 110, 200, 255);
    case SurfacePassKind::PathBlend: return IM_COL32(150,  88, 178, 255);
    default:                         return IM_COL32(110, 110, 110, 255);
    }
}

// PB blob round-trip on a pass. Self-contained (kv only) — unlike the dialog's
// read_pb_blob/write_pb_blob there is no live region config to mirror here.
static PathBlendPassConfig pro_pb_read(const SurfacePass& p)
{
    const auto it = p.pathblend.kv.find("blob");
    return PathBlendPassConfig::from_blob_json(
        it != p.pathblend.kv.end() ? it->second : std::string());
}

static void pro_pb_write(SurfacePass& p, PathBlendPassConfig pbc, double layer_h)
{
    pbc.apply_constraints(layer_h);   // shared rule set (engine, s108)
    pbc.sync_legacy_view();
    p.pathblend.present    = true;
    p.pathblend.kv["blob"] = pbc.to_blob_json();
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
    auto it = p.colormix.kv.find(pro_cm_pattern_key(penu));
    if (it != p.colormix.kv.end() && !it->second.empty()) return it->second;
    it = p.colormix.kv.find("pattern");   // short per-pass editor key
    if (it != p.colormix.kv.end()) return it->second;
    return std::string();
}
static void pro_cm_read(const SurfacePass& p, bool penu, int& a, int& b, int& pct_b)
{
    auto rd = [&](const std::string& k, int def) {
        const auto it = p.colormix.kv.find(k);
        if (it != p.colormix.kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
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
    p.colormix.present = true;
    p.colormix.kv[pro_cm_pattern_key(penu)]   = pat;
    p.colormix.kv[pro_cm_tool_key(penu, 'a')] = std::to_string(a);
    p.colormix.kv[pro_cm_tool_key(penu, 'b')] = std::to_string(b);
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
        top_pass.kind  = SurfacePassKind::ColorMix;
        top_pass.ratio = 1.0;
        pro_cm_write(top_pass, /*penu=*/false, tool_a, tool_b, pct_b);

        SurfacePass penu_pass;
        penu_pass.kind  = SurfacePassKind::ColorMix;
        penu_pass.ratio = 1.0;
        pro_cm_write(penu_pass, /*penu=*/true, tool_a, tool_b, pct_b);

        SurfacePassStack top, penu;
        top.enabled  = true; top.passes  = { top_pass };
        penu.enabled = true; penu.passes = { penu_pass };

        ColorSci::ColorRecipe r;
        r.top  = top;
        r.penu = penu;
        ColorSci::sandwich_colour_stacked(top, penu, mats, opt.bg_rgb, r.rgb.data());
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d%%", pct_b);
        r.desc = buf;
        out.push_back(std::move(r));
    }
    return out;
}

// NEOTKO_COLORSTITCH_TAG — s118: un pase ColorMix DEBE llevar su payload (pattern/A/B)
// o el motor lo degrada a Solid (síntoma: penu pintado "sin efecto" al slicear). Las
// recetas que llegan de tiras predict o de perfiles cargados pueden traer kind=ColorMix
// con kv VACÍO; como pro_cm_write sólo se llamaba al CAMBIAR A/B, el payload se guardaba
// vacío. Rellena cada pase ColorMix sin payload con sus valores efectivos (pro_cm_read
// da defaults si está vacío) → pase autocontenido que el motor sí pinta como ColorMix.
static void pro_backfill_cm(Slic3r::SurfacePassStack& st, bool penu)
{
    for (Slic3r::SurfacePass& p : st.passes) {
        if (p.kind != Slic3r::SurfacePassKind::ColorMix) continue;
        if (!pro_cm_pattern(p, penu).empty()) continue;   // ya tiene payload
        int a, b, cp; pro_cm_read(p, penu, a, b, cp);
        // Honrar el tool ya fijado (solid_tool) como A; B distinto para que la mezcla
        // sea visible. Mejor que los defaults 0/1 cuando el pase traía solid_tool.
        if (p.solid_tool >= 0 && p.solid_tool <= 3) a = p.solid_tool;
        if (b == a) b = (a + 1) & 3;
        pro_cm_write(p, penu, a, b, cp);
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
    case K::ColorMix: {
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
    bool changed = false;
    ImGui::PushID(id);
    const float sw = ImGui::GetTextLineHeight() * 1.2f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##chip", ImVec2(sw, sw)))
        ImGui::OpenPopup("##pick");
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw), tool_col_u32(fcolors, tool));
    dl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255));
    if (hov && tip) ImGui::SetTooltip("%s (T%d)", tip, tool + 1);
    if (ImGui::BeginPopup("##pick")) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        for (int t = 0; t < nfil; ++t) {
            ImGui::PushID(t);
            const ImVec2 q = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##t", ImVec2(sw, sw))) {
                tool = t; changed = true;
                ImGui::CloseCurrentPopup();
            }
            pdl->AddRectFilled(q, ImVec2(q.x + sw, q.y + sw), tool_col_u32(fcolors, t));
            pdl->AddRect(q, ImVec2(q.x + sw, q.y + sw),
                         (t == tool) ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255),
                         0.f, 0, (t == tool) ? 2.f : 1.f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("T%d", t + 1);
            ImGui::PopID();
            if (t + 1 < nfil) ImGui::SameLine(0.f, 4.f);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Coloured kind badge (SOLID / COLORMIX / PB HALF / PB FULL), wx visual mirror.
static void pro_kind_badge(const char* text, ImU32 col)
{
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 sz(ts.x + 12.f, ImGui::GetTextLineHeight() + 4.f);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), col, 2.f);
    dl->AddText(ImVec2(p.x + 6.f, p.y + 2.f), IM_COL32(255, 255, 255, 255), text);
    ImGui::Dummy(sz);
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
    dl->AddRectFilled(a, b, IM_COL32(45, 45, 45, 255));
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
    case SurfacePassKind::ColorMix: {
        // Unified with the 3D weave: the SAME per-line tool sequence the slicer
        // produces (dither / gradient / bands / pattern), not the raw digits.
        // NEOTKO_COLORSTITCH_TAG — bands drawn at the REAL fill angle so the preview
        // rotates with the wheel (mirrors the Solid hatch + the 3D weave).
        const float bw     = 4.f;
        const int   nbands = (int)(std::hypot(b.x - a.x, b.y - a.y) / bw) + 2;
        std::vector<int> seq = colorstitch_tool_sequence(p.colormix.kv, penu, std::max(2, nbands));
        if (seq.empty()) seq.push_back(0);
        // angle from the pass kv (auto=-1 → display at 45°). Bands run ALONG the fill
        // lines, so band boundaries step across the perpendicular axis.
        int adeg = -1;
        { const auto it = p.colormix.kv.find(penu ? "interlayer_colormix_penu_angle"
                                                  : "interlayer_colormix_angle");
          if (it != p.colormix.kv.end()) { try { adeg = std::stoi(it->second); } catch (...) {} } }
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
            const int t = seq[((i % ns) + ns) % ns];
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
            const float deg = (pbc.fill_angle < 0) ? 45.f : (float)pbc.fill_angle;
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
    dl->AddRect(a, b, IM_COL32(20, 20, 20, 255));
}

// Editor de una zona (Top / Penu) del pro mode — v2 s108: filas estilo
// SandwichDialog con los tres kinds inline y cajita Z editable (auto-rescale).
static void draw_zone_editor(const char* id, const char* label,
                             Slic3r::SurfacePassStack& st, bool allow_disable,
                             bool penu, const std::vector<std::string>& fcolors,
                             int nfil, double layer_h, float row_avail_w,
                             bool bottom_caps = false,
                             const char* add_label = nullptr,
                             const char* clear_label = nullptr)
{
    // NEOTKO_BOTTOM_TAG — Fase 1 §5.5 (strict caps, bottom only). When bottom_caps:
    //   · max 2 Solid passes · max 1 ColorStitch pass · PathBlend ALWAYS Full
    //     (PB Half is hidden — a Half on the bottom would leave an empty layer and
    //      destabilize how the print is built up). Top/Penu pass false → untouched.
    // Caps are enforced at authoring (kind dropdown + "+ layer" default kind);
    // total passes are already bounded by kMaxPasses (2 Solid + 1 ColorStitch = 3).
    using namespace Slic3r;
    using K = SurfacePassKind;
    ImGui::PushID(id);

    // NEOTKO_SANDWICH_TAG s119 (EMPTY model): NO "Enabled" gate. A zone is simply
    // "Empty" (no effect) or "not Empty" (has painted content) — the content is the
    // only control. This kills the gate that made identically-painted regions
    // diverge (one passed the enable check, the other didn't). The Top zone is the
    // colour itself (always present); the Penultimate is Empty by default and is
    // added/cleared explicitly — no checkbox.
    ImGui::TextUnformatted(label);
    if (allow_disable) {
        if (!st.any_effect()) {
            st.passes.clear();            // canonical Empty = no passes
            st.enabled = false;
            ImGui::SameLine();
            if (ImGui::SmallButton(add_label ? add_label : _u8L("+ Add penultimate").c_str())) {
                SurfacePass sp; sp.kind = K::ColorMix; sp.ratio = 1.0;
                st.passes.push_back(sp);
                pro_cm_write(st.passes.back(), penu, 0, 1, 50);  // default A/B/mix
                st.enabled = true;
            }
            ImGui::PopID();
            return;                       // Empty → nothing else to render
        }
        st.enabled = true;                // has content → not Empty
        ImGui::SameLine();
        if (ImGui::SmallButton(clear_label ? clear_label : _u8L("x Clear penultimate").c_str())) {
            st.passes.clear();
            st.enabled = false;
            ImGui::PopID();
            return;
        }
    } else {
        st.enabled = true;
        if (st.passes.empty()) {
            SurfacePass sp; sp.ratio = 1.0;
            st.passes.push_back(sp);
        }
    }

    // NEOTKO_COLORSTITCH_TAG — s118: el "Perimeter override" YA NO es per-zona aquí.
    // Pasó a ser una opción ÚNICA del color (decisión usuario s118), pintada una sola
    // vez en render_pro_mode_panel y replicada a top+penu. Ver apply_perim_override_*.

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

    const std::string kind_items[4] = {
        _u8L("Solid"), _u8L("ColorStitch"),
        _u8L("PB Half"), _u8L("PB Full")
    };
    const std::string ease_names[4] = {
        _u8L("Mode: Linear"), _u8L("Mode: Ease In"),
        _u8L("Mode: Ease Out"), _u8L("Mode: Ease In/Out")
    };

    // s169 F4 — ratio-bar arrastrable (port de Tab.cpp paint_ratio_bar +
    // ratio_bar_motion): columna a la izquierda de TODAS las filas, mismo ancho
    // de arriba abajo. Se pinta DESPUÉS del loop (necesitamos y1 = borde inferior
    // de la última fila), pero el indent debe aplicarse ANTES para que las filas
    // se corran a la derecha y le hagan sitio.
    const float bar_w  = ImGui::GetTextLineHeight() * 1.2f;
    const ImVec2 bar_p0 = ImGui::GetCursorScreenPos();   // (bar_x, y0)
    ImGui::Indent(bar_w + 6.f);

    // Visual order = physical order: top of the layer first (#1 = topmost,
    // like the SandwichDialog draws its rows). Vector is bottom→top.
    for (int i = n - 1; i >= 0; --i) {
        SurfacePass& p = st.passes[i];
        ImGui::PushID(i);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);          // content above the bg rect

        // NEOTKO_COLORSTITCH_TAG — s139: ancho de fila ESTABLE pasado por el caller
        // (medido UNA vez antes de ambas zonas). Antes se recalculaba por-fila con
        // GetContentRegionAvail().x → variaba entre Top (arriba) y Penu (abajo) si
        // aparecía scrollbar o derivaba el cursor → el Z-box (tamaño de capa) y la
        // banda quedaban en X distinto entre zonas. Anclar a un único ancho los iguala.
        // s169 — el Z-box de la derecha se retiró (feedback usuario): las filas
        // ya no reservan su ancho, ganan sitio para chips/combo/angle.
        const float left_w = std::max(140.f, row_avail_w - (bar_w + 6.f));

        const PathBlendPassConfig pbc =
            (p.kind == K::PathBlend) ? pro_pb_read(p) : PathBlendPassConfig{};
        const bool is_pb_half =
            p.kind == K::PathBlend && pbc.mode == PathBlendPassConfig::Mode::Half;

        ImGui::BeginGroup();
        // ---- line 1: [x] · #N · chips · badge · kind · per-kind fields ----
        if (n > 1) {
            if (ImGui::SmallButton("x")) remove_idx = i;
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("#%d", n - i);
        ImGui::SameLine();

        if (p.kind == K::Solid) {
            int t = p.solid_tool;
            if (pro_tool_chip("##sol", fcolors, nfil, t, _u8L("Tool").c_str()))
                p.solid_tool = t;
            ImGui::SameLine();
        } else if (p.kind == K::ColorMix) {
            // display-only mini chips (the A/B pickers live on line 3)
            int ca, cb, cp; pro_cm_read(p, penu, ca, cb, cp);
            const float msw = ImGui::GetTextLineHeight() * 0.8f;
            for (int t : { ca, cb }) {
                const ImVec2 q = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(q, ImVec2(q.x + msw, q.y + msw), tool_col_u32(fcolors, t));
                dl->AddRect(q, ImVec2(q.x + msw, q.y + msw), IM_COL32(20, 20, 20, 255));
                ImGui::Dummy(ImVec2(msw, msw));
                ImGui::SameLine(0.f, 2.f);
            }
            ImGui::SameLine(0.f, 6.f);
        } else if (p.kind == K::PathBlend) {
            PathBlendPassConfig pbe = pbc;
            bool pb_changed = false;
            int tb = std::max(0, pbe.tool_bottom);
            if (pro_tool_chip("##pbb", fcolors, nfil, tb, _u8L("Ramp tool (bottom)").c_str())) {
                pbe.tool_bottom = tb; pb_changed = true;
            }
            if (!is_pb_half) {
                ImGui::SameLine(0.f, 2.f);
                int tt = std::max(0, pbe.tool_top);
                if (pro_tool_chip("##pbt", fcolors, nfil, tt, _u8L("Cap tool (top)").c_str())) {
                    pbe.tool_top = tt; pb_changed = true;
                }
            }
            if (pb_changed) pro_pb_write(p, pbe, layer_h);
            ImGui::SameLine();
        }

        const char* btxt = "SOLID";
        if (p.kind == K::ColorMix)       btxt = "COLORMIX";
        else if (p.kind == K::PathBlend) btxt = is_pb_half ? "PB HALF" : "PB FULL";
        pro_kind_badge(btxt, pro_kind_colour(p.kind));
        ImGui::SameLine();

        int sel = 0;
        if (p.kind == K::ColorMix)       sel = 1;
        else if (p.kind == K::PathBlend) sel = is_pb_half ? 2 : 3;
        // NEOTKO — light mode readability: the default ImGui popup theme is dark, so the open
        // dropdown was dark text on dark bg (user report). Force a light popup background + dark
        // text + translucent Orca header in light mode. Dark mode keeps the default (untouched).
        const bool _kind_light = !ImGuiWrapper::is_dark_mode();
        if (_kind_light) {
            ImGui::PushStyleColor(ImGuiCol_PopupBg,       ImGuiWrapper::COL_WINDOW_BG);
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.00f, 0.59f, 0.53f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.00f, 0.59f, 0.53f, 0.55f));
        }
        ImGui::PushItemWidth(110.f);   // s169 F4: 100→110 (más aire, ya no compite con el rail muerto en F1)
        if (ImGui::BeginCombo("##kind", kind_items[sel].c_str())) {
            // NEOTKO_BOTTOM_TAG — §5.5: count the OTHER passes' kinds so switching
            // THIS pass to a capped kind respects the bottom caps (the target kind
            // replaces this pass's current kind, so pass i is excluded from the count).
            int _n_solid_other = 0, _n_cs_other = 0;
            if (bottom_caps)
                for (int j = 0; j < n; ++j) if (j != i) {
                    if (st.passes[j].kind == K::Solid)         ++_n_solid_other;
                    else if (st.passes[j].kind == K::ColorMix) ++_n_cs_other;
                }
            for (int k = 0; k < 4; ++k) {
                // PB Half is never offered on the bottom (would leave an empty layer).
                if (bottom_caps && k == 2) continue;
                // Disable a capped kind when picking it would exceed its bottom cap.
                const bool _capped = bottom_caps &&
                    ((k == 0 && _n_solid_other >= 2) ||   // max 2 Solid
                     (k == 1 && _n_cs_other    >= 1));    // max 1 ColorStitch
                // (Sin BeginDisabled: esta versión de ImGui no lo trae → dim manual +
                //  guarda. El Selectable sigue clicable; el `&& !_capped` veta la acción.)
                if (_capped) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                if (ImGui::Selectable(kind_items[k].c_str(), k == sel) && k != sel && !_capped) {
                    if (k == 0) {
                        p.kind = K::Solid;
                    } else if (k == 1) {
                        p.kind = K::ColorMix;
                        if (pro_cm_pattern(p, penu).empty())
                            pro_cm_write(p, penu, 0, 1, 50);
                    } else {
                        // PB collapses the zone to ONE whole-layer PB pass —
                        // same rule as the SandwichDialog (on_kind_change).
                        pb_collapse_src  = i;
                        pb_collapse_mode = (k == 2) ? 0 : 1;
                    }
                }
                if (_capped) {
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", (k == 0)
                            ? _u8L("Bottom: max 2 Solid passes").c_str()
                            : _u8L("Bottom: max 1 ColorStitch pass").c_str());
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (_kind_light) ImGui::PopStyleColor(5);

        if (p.kind == K::Solid) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("angle:").c_str());
            ImGui::SameLine();
            int av = p.angle;
            ImGui::PushItemWidth(42.f);
            if (ImGui::DragInt("##ang", &av, 1.f, -1, 359)) {
                if (av < 0) av = -1; else if (av > 359) av = 359;
                p.angle = av;
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("-1 = auto (follow fill angle). Wheel over the bar rotates.").c_str());
        } else if (p.kind == K::ColorMix) {
            // NEOTKO_COLORSTITCH_TAG — inline fill-angle for ColorStitch (mirrors Solid).
            // Writes the pass kv so it persists into the profile stack → the slice honours
            // it (Fill.cpp painted_colormix_angle_for_slot). -1 = auto (follow fill angle).
            const char* akey = penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle";
            int cm_ang = -1;
            { const auto it = p.colormix.kv.find(akey);
              if (it != p.colormix.kv.end()) { try { cm_ang = std::stoi(it->second); } catch (...) {} } }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("angle:").c_str());
            ImGui::SameLine();
            ImGui::PushItemWidth(42.f);
            if (ImGui::DragInt("##cm_ang", &cm_ang, 1.f, -1, 359)) {
                if (cm_ang < -1) cm_ang = -1; else if (cm_ang > 359) cm_ang = 359;
                p.colormix.present = true;
                p.colormix.kv[akey] = std::to_string(cm_ang);
                p.angle = cm_ang;
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("-1 = auto (follow fill angle). Wheel over the bar rotates.").c_str());
        }

        // s169 F4 — PathBlend en su PROPIA línea (antes compartía la línea 1 con
        // chips+badge+combo, quedando apretado). SOLO recolocación: los mismos
        // widgets/helpers/clamps que antes (floor venía del Z-box; ahora vive
        // aquí, el Z-box de PB pasa a mostrar "full" más abajo).
        if (p.kind == K::PathBlend) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("floor:").c_str());
            ImGui::SameLine();
            {
                float fv = pbc.floor_mm;
                ImGui::PushItemWidth(52.f);
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
            ImGui::TextUnformatted(_u8L("ramp end:").c_str());
            ImGui::SameLine();
            if (is_pb_half) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%.2f", layer_h);   // Half: mid forced to layer top
            } else {
                float mv = pbc.mid_end_mm;
                ImGui::PushItemWidth(52.f);
                if (ImGui::DragFloat("##mid", &mv, 0.005f, 0.f, (float)layer_h, "%.3f")) {
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    pbe.mid_end_mm = mv;
                    pro_pb_write(p, pbe, layer_h);
                }
                ImGui::PopItemWidth();
            }
        }

        // ---- preview bar (hover wheel: Solid angle / PB Full mid) — line 2 for
        // Solid/ColorMix, line 3 for PathBlend (tiene su línea propia arriba) ----
        {
            const float bar_h = ImGui::GetTextLineHeight() * 1.1f;
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##prev", ImVec2(left_w, bar_h));
            if (ImGui::IsItemHovered()) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.f) {
                    if (p.kind == K::Solid) {
                        // port of the dialog's on_angle_wheel (-1 = auto)
                        const int step = (wheel > 0.f) ? 5 : -5;
                        int a2 = p.angle;
                        if (a2 < 0) a2 = (step > 0) ? 0 : -1;
                        else { a2 += step; if (a2 < 0) a2 = -1; else a2 %= 360; }
                        p.angle = a2;
                    } else if (p.kind == K::ColorMix) {
                        // NEOTKO_COLORSTITCH_TAG — wheel rotates the ColorStitch fill angle;
                        // write the pass kv so it persists to the slice (same as the DragInt).
                        const char* akey = penu ? "interlayer_colormix_penu_angle"
                                                : "interlayer_colormix_angle";
                        int a2 = -1;
                        { const auto it = p.colormix.kv.find(akey);
                          if (it != p.colormix.kv.end()) { try { a2 = std::stoi(it->second); } catch (...) {} } }
                        const int step = (wheel > 0.f) ? 5 : -5;
                        if (a2 < 0) a2 = (step > 0) ? 0 : -1;
                        else { a2 += step; if (a2 < 0) a2 = -1; else a2 %= 360; }
                        p.colormix.present = true;
                        p.colormix.kv[akey] = std::to_string(a2);
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

        // s169 F4 FIX — Mode (ease) button on its OWN line, BELOW the preview bar.
        // Mirrors Tab.cpp's build_row anatomy (row1 = kind fields, row2 = preview,
        // row3 = Mode/Advanced buttons): the previous port crammed this button onto
        // the floor/ramp-end line, where it competed for width and could overflow
        // the panel. Giving it its own line removes that pressure entirely.
        if (p.kind == K::PathBlend) {
            const int em = std::clamp(pbc.ease_mode, 0, 3);
            if (ImGui::SmallButton((ease_names[em] + "##ease").c_str())) {
                PathBlendPassConfig pbe = pro_pb_read(p);
                pbe.ease_mode = (em + 1) % 4;
                pro_pb_write(p, pbe, layer_h);
            }
            // s172 — fill_angle: existía en el motor (PathBlendPassConfig::fill_angle,
            // Fill.cpp lo lee ya) pero nunca se expuso en ninguna UI. Visible y
            // editable igual que ColorStitch (DragInt -1=auto/0-359), en esta línea
            // (con sitio de sobra) para no repetir el overflow de floor/ramp-end.
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(_u8L("angle:").c_str());
            ImGui::SameLine();
            {
                int pb_ang = pbc.fill_angle;
                ImGui::PushItemWidth(42.f);
                if (ImGui::DragInt("##pb_ang", &pb_ang, 1.f, -1, 359)) {
                    if (pb_ang < 0) pb_ang = -1; else pb_ang %= 360;
                    PathBlendPassConfig pbe = pro_pb_read(p);
                    pbe.fill_angle = pb_ang;
                    pro_pb_write(p, pbe, layer_h);
                }
                ImGui::PopItemWidth();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("-1 = auto (follow top surface angle).").c_str());
            }
        }

        // ---- line 3 (ColorStitch only): botón ADV → editor avanzado de patrón ----
        // NEOTKO_COLORSTITCH_TAG — el editor inline A/B + mix% (derivado del sistema
        // de predicción textil) se retiró: solo generaba un patrón Bresenham de 2
        // tools y no aportaba a la *creación* de color. Ahora un único botón abre el
        // ColorMixPatternDialog (4 tools, S-curve, overlap, ángulo…), que TOMA el
        // patrón actual y DEVUELVE el editado → se escribe en el pase. La preview del
        // patrón ya se dibuja en la línea 2 (pro_pass_preview), que sigue siendo el
        // feedback visual. Knobs de diseño NO round-trip per-pase (solo el patrón).
        if (p.kind == K::ColorMix) {
            // Backfill defensivo: un pase sin payload no debe degradar a Solid.
            if (pro_cm_pattern(p, penu).empty()) {
                int a, b, cp; pro_cm_read(p, penu, a, b, cp);
                pro_cm_write(p, penu, a, b, cp);
            }
            if (ImGui::Button(_u8L("ADV…").c_str())) {
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
                        wxGetApp().plater(), fcolors, penu, cfg, p.colormix.kv, out_kv)) {
                    p.colormix.present = true;
                    p.colormix.kv      = std::move(out_kv);
                    // Sync chip/preview/wheel desde el payload nuevo.
                    int a, b, cp; pro_cm_read(p, penu, a, b, cp);
                    p.solid_tool = a;
                    const auto it = p.colormix.kv.find(
                        penu ? "interlayer_colormix_penu_angle" : "interlayer_colormix_angle");
                    if (it != p.colormix.kv.end()) {
                        try { p.angle = std::stoi(it->second); } catch (...) {}
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Open the ColorStitch pattern editor").c_str());
            // NEOTKO_COLORSTITCH_TAG — auto-angle (-1) notice next to ADV. With -1 the slicer
            // alternates the fill angle per layer (uniform finish), so the print won't keep the
            // previewed orientation. Compact amber tag + tooltip; set a fixed angle to lock it.
            {
                int _adv_ang = -1;
                const auto it = p.colormix.kv.find(penu ? "interlayer_colormix_penu_angle"
                                                        : "interlayer_colormix_angle");
                if (it != p.colormix.kv.end()) { try { _adv_ang = std::stoi(it->second); } catch (...) {} }
                if (_adv_ang < 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.f), "%s", _u8L("auto angle").c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", _u8L("Angle -1 = auto: the slicer rotates the fill "
                                                     "angle every layer (uniform finish), so the print "
                                                     "won't match this orientation. Set a fixed angle "
                                                     "(wheel over the bar) to lock it.").c_str());
                }
            }
        }
        ImGui::EndGroup();

        const ImVec2 gmin = ImGui::GetItemRectMin();
        const ImVec2 gmax = ImGui::GetItemRectMax();

        // thin-pass warning (Solid/ColorStitch sub-bands; PB has its own gate)
        const double pass_mm = p.ratio * layer_h;
        const bool thin = (p.kind != K::PathBlend) && (pass_mm < 0.04 - 1e-9);

        dl->ChannelsSetCurrent(0);          // bg rect behind the block
        // NEOTKO_COLORSTITCH_TAG — la caja de la fila debe contrastar con el TEXTO,
        // que es claro en modo oscuro y oscuro en modo claro. Una caja gris-oscura
        // fija dejaba texto oscuro sobre fondo oscuro en light mode (ilegible). Caja
        // adaptativa: oscura en dark mode (como estaba), clara en light mode.
        const bool _dark = ImGuiWrapper::is_dark_mode();
        const ImU32 _box_col = thin
            ? (_dark ? IM_COL32(48, 40, 32, 255) : IM_COL32(245, 232, 205, 255))
            : (_dark ? IM_COL32(60, 60, 60, 255) : IM_COL32(214, 214, 214, 255));
        // s169 F4 — más aire (feedback usuario): padding de bloque 3→6 px (ya no
        // compite con el rail, que murió en F1).
        dl->AddRectFilled(ImVec2(gmin.x - 6.f, gmin.y - 2.f),
                          ImVec2(gmin.x + left_w + 6.f, gmax.y + 2.f),
                          _box_col, 3.f);
        dl->ChannelsMerge();

        // s169 (feedback usuario, tras ver F4 compilado) — el Z-box numérico de
        // la derecha (mm / "full") se RETIRA: quedaba descolocado, duplicando el
        // número que la ratio-bar ya muestra a la izquierda. La barra pasa a ser
        // la ÚNICA fuente de "cuánto mide este pase" (ver más abajo, tras el
        // loop) — ahora también etiqueta el caso de un solo pase / PathBlend
        // (antes solo etiquetaba con 2+ pases). Esto retira el "set esta altura,
        // reescala TODOS los hermanos proporcionalmente" del DragFloat viejo; el
        // drag de la barra sigue permitiendo mover cualquier frontera entre dos
        // pases adyacentes.

        // resume the layout below the block — s169 F4: separación entre filas
        // 5→8 px (más aire, feedback usuario).
        ImGui::SetCursorScreenPos(ImVec2(gmin.x, gmax.y + 8.f));
        if (thin)
            ImGui::TextColored(ImVec4(0.86f, 0.59f, 0.24f, 1.f), "%s",
                _u8L("⚠ < 0.04 mm — pass is dropped at slice time").c_str());
        ImGui::PopID();
    }
    ImGui::Unindent(bar_w + 6.f);

    // s169 — pintar la ratio-bar: bandas (draw_zone, ya escala por ratio) +
    // etiqueta mm por banda (SIEMPRE, incluso con 1 solo pase / PathBlend —
    // desde que el Z-box de la derecha se retiró, la barra es la ÚNICA fuente
    // de "cuánto mide este pase") + divisor blanco 2px + divisores
    // arrastrables (solo si hay 2+ pases — nada que arrastrar con 1 solo).
    {
        const float bar_x  = bar_p0.x;
        const float bar_y0 = bar_p0.y;
        const float bar_y1 = ImGui::GetCursorScreenPos().y;   // el loop nos dejó aquí
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

            char mmbuf[16];
            std::snprintf(mmbuf, sizeof(mmbuf), "%.2f", fr * layer_h);
            bdl->AddText(ImVec2(bar_x + 2.f, y0f + 2.f), IM_COL32(255, 255, 255, 255), mmbuf);

            if (dp != n - 1) {   // divider — only reached when n >= 2
                bdl->AddLine(ImVec2(bar_x, y1f), ImVec2(bar_x + bar_w, y1f),
                            IM_COL32(235, 235, 235, 255), 2.f);

                ImGui::PushID(3000 + dp);
                ImGui::SetCursorScreenPos(ImVec2(bar_x, y1f - 4.f));
                ImGui::InvisibleButton("##div", ImVec2(bar_w, 8.f));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
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

        ImGui::SetCursorScreenPos(ImVec2(bar_x, bar_y1));
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

    // + layer — hidden when the zone holds a PB pass (whole-layer by definition)
    bool has_pb = false;
    for (const auto& p : st.passes) has_pb |= (p.kind == K::PathBlend);
    if (!has_pb && (int)st.passes.size() < SurfacePassStack::kMaxPasses) {
        if (ImGui::SmallButton((std::string("+ layer##") + id).c_str())) {
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
                    else if (p.kind == K::ColorMix) ++nc;
                }
                if (ns >= 2 && nc < 1) {
                    np.kind = K::ColorMix;
                    pro_cm_write(np, penu, 0, 1, 50);   // default A/B/mix
                }
            }
            st.passes.push_back(np);    // becomes the new TOPMOST pass (#1)
        }
    }
    ImGui::PopID();
}
// NEOTKO_COLORSTITCH_TAG_END

int GLGizmoColorMixPainter::slot_for_selected_profile(bool assign_if_missing)
{
    if (m_selected_profile_id == 0) return 0;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return 0;

    // First look for the profile id in any existing slot of any model_part volume.
    int existing_slot = 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            if (mv->colormix_slot_to_profile_id[s] == m_selected_profile_id) {
                existing_slot = s; break;
            }
        }
        if (existing_slot) break;
    }
    if (existing_slot) {
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
            if (mv->colormix_slot_to_profile_id[s] != 0) { free_everywhere = false; break; }
        }
        if (free_everywhere) {
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                mv->colormix_slot_to_profile_id[s] = m_selected_profile_id;
            }
            NEOTKO_LOG(PROFILE, "PAINT slot_assign profile_id=" << m_selected_profile_id
                << " → new slot=" << s);
            return s;
        }
    }
    NEOTKO_LOG(PROFILE, "PAINT slot_assign FAILED — all paint slots used");
    return 0; // all slots taken — caller shows the "full" warning.
}

std::vector<ColorRGBA>
GLGizmoColorMixPainter::build_ebt_colors_for_volume(const ModelVolume* mv) const
{
    // Index 0 holds the volume's neutral base, indices 1..MAX_SLOTS-1 hold per-slot
    // colors. TriangleSelectorPatch internally prepends the volume base color, then
    // maps EnforcerBlockerType(N) → m_ebt_colors[N+1]. We mirror MMU's layout:
    //   ebt_colors[0] = volume base (unpainted)
    //   ebt_colors[1..MAX_SLOTS-1] = per-slot colors
    static_assert(MAX_SLOTS == ModelVolume::COLORMIX_SLOT_COUNT,
                  "gizmo MAX_SLOTS must match ModelVolume::COLORMIX_SLOT_COUNT (3mf slot table)");
    std::vector<ColorRGBA> ebt(MAX_SLOTS, ColorRGBA(0.6f, 0.6f, 0.6f, 1.f));
    ebt[0] = GLVolume::NEUTRAL_COLOR;
    int _slots_set = 0, _slots_resolved = 0;   // NEOTKO_COLORSTITCH_TAG — s118 dbg (punto 1)
    if (mv) {
        const auto& mgr = SurfaceEffectProfileManager::get();
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid == 0) continue;
            ++_slots_set;
            if (const SurfaceEffectProfile* p = mgr.find(pid)) {
                ebt[s] = color_for_profile(*p);
                ++_slots_resolved;
            }
        }
        // NEOTKO_COLORSTITCH_TAG — s118 (punto 1: al cargar no aparecen los
        // pintados). Discrimina las 3 hipótesis en el momento de construir los
        // colores del selector: slots_set=0 → tabla slot→perfil per-volumen NO
        // restaurada (parse/dedup shared-object); slots_set>0 & resolved=0 →
        // perfil no encontrado (manager vacío/timing o id mismatch); set==resolved
        // → colores OK aquí (el problema sería render/llamada).
        NEOTKO_LOG(PROFILE, "EBT_BUILD vol='" << (mv->name.empty() ? "?" : mv->name)
            << "' slots_set=" << _slots_set << " resolved=" << _slots_resolved
            << " mgr_size=" << mgr.size());
    }
    return ebt;
}

// NEOTKO_COLORSTITCH_TAG — weave preview helpers.
// Fwd-decl: resolved top-surface line width (defined below, after the palette helpers).
static double GLGizmoColorMixPainter_top_line_width();
// Fwd-decl: resolved layer height (defined below, after the palette helpers) —
// needed here by pathblend_make_weave's H (Fase 3.2, s167 plan).
static double GLGizmoColorMixPainter_layer_height();
// Real filament colour for a 0-based tool, as ColorRGBA (mirror of tool_col_u32).
static ColorRGBA tool_col_rgba(const std::vector<std::string>& fcolors, int tool0)
{
    const ImU32 c = tool_col_u32(fcolors, tool0);
    return ColorRGBA(float((c >>  0) & 0xFF) / 255.f,   // IM_COL32 packs R at bit 0
                     float((c >>  8) & 0xFF) / 255.f,
                     float((c >> 16) & 0xFF) / 255.f, 1.f);
}

// Extract the ColorStitch Top pass's config keys (interlayer_colormix_*). Prefers the
// resolved Top stack (same source as the mini-sandwich preview); falls back to the raw
// colormix payload (predict swatches with no resolved stack). Empty ⇒ not ColorStitch.
static std::map<std::string, std::string>
colorstitch_top_kv(const Slic3r::SurfaceEffectProfile& prof)
{
    if (!prof.stack_top_json.empty()) {
        const Slic3r::SurfacePassStack st = Slic3r::SurfacePassStack::from_json(prof.stack_top_json);
        for (const Slic3r::SurfacePass& p : st.passes)
            if (p.kind == Slic3r::SurfacePassKind::ColorMix && !p.colormix.kv.empty())
                return p.colormix.kv;
    }
    if (prof.colormix.present && !prof.colormix.kv.empty())
        return prof.colormix.kv;
    return {};
}

// NEOTKO_COLORSTITCH_TAG — band orientation for a ColorStitch slot = printed fill lines =
// cm_angle + 90°. `is_auto` set when the kv leaves the angle on auto (-1).
static float colorstitch_weave_theta(const std::map<std::string, std::string>& kv, bool& is_auto)
{
    int angle = -1;
    const auto it = kv.find("interlayer_colormix_angle");
    if (it != kv.end()) { try { angle = std::stoi(it->second); } catch (...) {} }
    float base_rad;
    if (angle >= 0) { base_rad = float(angle) * float(M_PI) / 180.f; is_auto = false; }
    else            { base_rad = float(M_PI) / 4.f;                  is_auto = true;  }
    // NEOTKO_COLORSTITCH_TAG — band direction = cm_angle (verified against the slice). The
    // per-layer alternation that used to scramble this is now locked out for a fixed angle via
    // f->is_using_template_angle in Fill.cpp, so weave == slice == pro-tray bar at cm_angle.
    return base_rad;
}

// NEOTKO_COLORSTITCH_TAG — one WeaveParams from a projected extent [pmin,pmax] along `theta`.
// N (line count) comes from the REAL line width; pitch = span/N so each stripe is one line
// wide (≈ line_w) until the LUT cap (64), beyond which it coarsens to still cover the span.
static TriangleSelectorPatch::WeaveParams
colorstitch_make_weave(const std::map<std::string, std::string>& kv,
                       const std::vector<std::string>& fcolors,
                       float theta, float pmin, float pmax, float line_w)
{
    TriangleSelectorPatch::WeaveParams w;
    const float span = pmax - pmin;
    if (span < 1e-3f) return w;   // w.on stays false
    const float lw = std::max(line_w, 0.05f);

    int mode = 0;
    { const auto it = kv.find("interlayer_colormix_mode");
      if (it != kv.end()) { try { mode = std::stoi(it->second); } catch (...) {} } }

    auto fill_cols = [&](const std::vector<int>& seq, int count) {
        w.cols.resize(count);
        for (int i = 0; i < count; ++i) {
            const int tool0 = seq[(size_t) i % seq.size()];
            w.cols[i] = (tool0 >= 0) ? tool_col_rgba(fcolors, tool0)
                                     : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
        }
    };

    if (mode == 0) {
        // PATTERN (periodic, e.g. "12"): tile ONE period at the real line width so each
        // stripe == one printed line, independent of island size (no 64-line stretch).
        const std::vector<int> base = colorstitch_tool_sequence(kv, /*penu*/false, 1);
        if (base.empty()) return w;
        const int n = std::min<int>((int) base.size(), 64);
        w.on = true; w.tile = true; w.angle_rad = theta; w.p0 = pmin; w.pitch = lw;
        fill_cols(base, n);
    } else {
        // GRADIENT / dither (modes 1-3): span the island once with N real-width lines,
        // capped at the 64-entry LUT (then it coarsens, but the ramp still reads right).
        const int N = std::clamp((int) std::lround(span / lw), 4, 64);
        const std::vector<int> base = colorstitch_tool_sequence(kv, /*penu*/false, N);
        if (base.empty()) return w;
        w.on = true; w.tile = false; w.angle_rad = theta; w.p0 = pmin; w.pitch = span / float(N);
        fill_cols(base, N);
    }
    return w;
}

// NEOTKO_SANDWICH_TAG — Fase 3.2 (s167 plan): extract a profile's Top PathBlend
// config, mirror of colorstitch_top_kv (empty kv -> "not this kind"). Returns
// false when Top has no PathBlend pass.
static bool pathblend_top_config(const Slic3r::SurfaceEffectProfile& prof, PathBlendPassConfig& out)
{
    if (prof.stack_top_json.empty())
        return false;
    const Slic3r::SurfacePassStack st = Slic3r::SurfacePassStack::from_json(prof.stack_top_json);
    for (const Slic3r::SurfacePass& p : st.passes)
        if (p.kind == Slic3r::SurfacePassKind::PathBlend) {
            out = pro_pb_read(p);
            return true;
        }
    return false;
}

// NEOTKO_SANDWICH_TAG — Fase 3.2: PathBlend on-mesh preview. Reuses the same
// WeaveParams/u_weave_cols infrastructure as ColorStitch (colorstitch_make_weave
// above) — NO shader changes — but the per-step colour comes from the REAL
// ramp+cap Beer-Lambert physics (pathblend_canonical_model.md, cross-checked
// against Fill.cpp's actual ramp block) instead of a flat tool-sequence lookup:
// PathBlend's "gradient" is a true Z-wedge composed by transmission (thin cap
// lets the ramp show through), not a dithered sequence of solid-colour lines.
// theta is fixed at 0 (pure object-local Y projection): Fill.cpp's `_t_of()`
// reads un-rotated slice-space Y regardless of fill_angle (fill_surface_extrusion
// rotates only to generate the lines, then rotates back before returning), so
// this axis matches the engine's — unlike ColorStitch's weave, which follows
// the configured cm_angle.
static TriangleSelectorPatch::WeaveParams
pathblend_make_weave(const PathBlendPassConfig& pbc,
                     const Slic3r::ColorSci::Material mats[4],
                     const float bg_rgb[3],
                     double layer_h_mm,
                     float pmin, float pmax, float line_w)
{
    TriangleSelectorPatch::WeaveParams w;
    const float span = pmax - pmin;
    if (span < 1e-3f || pbc.tool_bottom < 0) return w;   // w.on stays false

    // Same auto-resolution + clamp as the real ramp block (Fill.cpp): mid_end_mm
    // < 0 means "tallest legal ramp" (H-0.04 Full / H Half).
    const double H = std::max(0.01, layer_h_mm);
    const float floor_pb = std::max(0.01f, pbc.floor_mm);
    const bool  is_full  = (pbc.mode == PathBlendPassConfig::Mode::Full) && pbc.tool_top >= 0;
    const float mid_pref = (pbc.mid_end_mm < 0.f)
        ? (is_full ? float(H - 0.04) : float(H))
        : pbc.mid_end_mm;
    const float mid_end  = is_full ? std::min(mid_pref, float(H - 0.04))
                                   : std::min(mid_pref, float(H));
    const double range = double(mid_end) - double(floor_pb);

    const int N = std::clamp((int)std::lround(span / std::max(line_w, 0.05f)), 4, 64);
    w.cols.resize(N);
    const int tb = std::clamp(pbc.tool_bottom, 0, 3);
    const int tt = std::clamp(pbc.tool_top,    0, 3);
    for (int i = 0; i < N; ++i) {
        const double t      = (N > 1) ? double(i) / double(N - 1) : 0.5;
        const double h_ramp = floor_pb + t * range;
        const double h_cap  = H - h_ramp;

        std::vector<Slic3r::ColorSci::Layer> layers;
        Slic3r::ColorSci::Layer bottom;
        bottom.rgb   = mats[tb].rgb;
        bottom.td    = mats[tb].td;
        bottom.ratio = float(h_ramp / H);
        layers.push_back(bottom);
        // Half mode has no cap — the area above the ramp is genuinely unfilled
        // (authorized semi-fill, see PathBlendPassConfig comment), not a second
        // material. Letting the resolved real bg show through there (via the
        // single-layer Beer-Lambert blend below) is the closest honest preview:
        // it's the same "what's really behind this" bg Fase 2 already resolves.
        if (is_full) {
            Slic3r::ColorSci::Layer cap;
            cap.rgb   = mats[tt].rgb;
            cap.td    = mats[tt].td;
            cap.ratio = float(h_cap / H);
            layers.push_back(cap);
        }
        float rgb[3];
        Slic3r::ColorSci::blend_stacked(layers, bg_rgb, rgb);
        w.cols[i] = ColorRGBA(rgb[0], rgb[1], rgb[2], 1.f);
    }
    w.on = true; w.tile = false; w.angle_rad = 0.f; w.p0 = pmin; w.pitch = span / float(N);
    return w;
}

std::vector<TriangleSelectorPatch::WeaveParams>
GLGizmoColorMixPainter::build_ebt_weave_for_volume(const ModelVolume* mv,
                                                   const TriangleSelectorPatch* sel) const
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
    resolve_object_base_bg(mats, bg_rgb);

    const Slic3r::BoundingBoxf3 bb = mv->mesh().bounding_box();   // object-local (fallback)
    // Real top-surface line width (resolved from config, no slice). Drives the line COUNT
    // so stripe/gradient scale matches what the slicer lays on the top.
    const float line_w = (float) GLGizmoColorMixPainter_top_line_width();
    const double lh = GLGizmoColorMixPainter_layer_height();

    const auto& mgr = SurfaceEffectProfileManager::get();
    for (int s = 1; s < MAX_SLOTS; ++s) {
        const int pid = mv->colormix_slot_to_profile_id[s];
        if (pid == 0) continue;
        const SurfaceEffectProfile* p = mgr.find(pid);
        if (!p) continue;
        const std::map<std::string, std::string> kv = colorstitch_top_kv(*p);
        PathBlendPassConfig pbc;
        const bool is_pathblend = kv.empty() && pathblend_top_config(*p, pbc);
        if (kv.empty() && !is_pathblend) continue;   // neither ColorStitch nor PathBlend → flat

        // PathBlend's axis is fixed at 0 (see pathblend_make_weave doc comment);
        // ColorStitch follows its own configured band angle.
        bool is_auto = false;
        const float theta = is_pathblend ? 0.f : colorstitch_weave_theta(kv, is_auto);
        if (is_auto) m_weave_any_auto_angle = true;
        const float sN = std::sin(theta), cN = std::cos(theta);

        // -- surface span along the cross-axis: union of the slot's painted facets (this is
        // the PER-SLOT fallback used by fragment patches; the per-ISLAND split happens in
        // build_ebt_weave_islands_for_volume). Falls back to the object AABB when no facets.
        float pmin = 1e9f, pmax = -1e9f;
        bool  have_painted = false;
        if (sel) {
            const indexed_triangle_set its = sel->get_facets(static_cast<EnforcerBlockerType>(s));
            for (const stl_vertex& v : its.vertices) {
                const float pr = -v.x() * sN + v.y() * cN;
                pmin = std::min(pmin, pr); pmax = std::max(pmax, pr);
                have_painted = true;
            }
        }
        if (!have_painted) {
            for (double X : { bb.min.x(), bb.max.x() })
                for (double Y : { bb.min.y(), bb.max.y() }) {
                    const float pr = -float(X) * sN + float(Y) * cN;
                    pmin = std::min(pmin, pr); pmax = std::max(pmax, pr);
                }
        }
        TriangleSelectorPatch::WeaveParams w = is_pathblend
            ? pathblend_make_weave(pbc, mats, bg_rgb, lh, pmin, pmax, line_w)
            : colorstitch_make_weave(kv, fcolors, theta, pmin, pmax, line_w);
        if (w.on) out[s] = std::move(w);
    }
    return out;
}

void GLGizmoColorMixPainter::build_ebt_weave_islands_for_volume(
        const ModelVolume* mv, const TriangleSelectorPatch* sel,
        std::unordered_map<int,int>& facet_weave_idx,
        std::vector<TriangleSelectorPatch::WeaveParams>& weave_list) const
{
    facet_weave_idx.clear();
    weave_list.clear();
    if (!mv || !sel || !m_weave_preview) return;

    Slic3r::ColorSci::Material mats[4];
    std::vector<std::string>   fcolors;
    gizmo_materials(mats, fcolors);
    // NEOTKO_SANDWICH_TAG — Fase 3.2: real bg, see build_ebt_weave_for_volume.
    float bg_rgb[3] = {0.f, 0.f, 0.f};
    resolve_object_base_bg(mats, bg_rgb);
    const float line_w = (float) GLGizmoColorMixPainter_top_line_width();
    const double lh = GLGizmoColorMixPainter_layer_height();

    const auto& mgr = SurfaceEffectProfileManager::get();
    for (int s = 1; s < MAX_SLOTS; ++s) {
        const int pid = mv->colormix_slot_to_profile_id[s];
        if (pid == 0) continue;
        const SurfaceEffectProfile* p = mgr.find(pid);
        if (!p) continue;
        const std::map<std::string, std::string> kv = colorstitch_top_kv(*p);
        PathBlendPassConfig pbc;
        const bool is_pathblend = kv.empty() && pathblend_top_config(*p, pbc);
        if (kv.empty() && !is_pathblend) continue;

        bool is_auto = false;
        const float theta = is_pathblend ? 0.f : colorstitch_weave_theta(kv, is_auto);
        if (is_auto) m_weave_any_auto_angle = true;
        const float sN = std::sin(theta), cN = std::cos(theta);

        std::vector<int> src;   // parallel to its.indices → original facet index
        const indexed_triangle_set its = sel->get_facets(static_cast<EnforcerBlockerType>(s), src);
        if (its.indices.empty()) continue;

        // -- connected components (islands) via union-find over TRIANGLES, joined by a shared
        // EDGE (2 common vertices). Edge adjacency — not vertex adjacency — so coplanar zones
        // that only touch at a CORNER (1 shared vertex) stay separate islands (each flat zone
        // gets its own gradient instead of bleeding across the corner). Welded verts from
        // get_facets give stable shared indices.
        const int nt = int(its.indices.size());
        std::vector<int> parent(nt);
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&parent](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
        auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) parent[a] = b; };
        {
            std::map<std::pair<int,int>, int> edge_owner;   // sorted (v0,v1) → first triangle
            auto add_edge = [&](int a, int b, int tri) {
                if (a > b) std::swap(a, b);
                const std::pair<int,int> key{a, b};
                auto it = edge_owner.find(key);
                if (it == edge_owner.end()) edge_owner.emplace(key, tri);
                else                        unite(tri, it->second);
            };
            for (int k = 0; k < nt; ++k) {
                const auto& tri = its.indices[k];
                add_edge(tri[0], tri[1], k);
                add_edge(tri[1], tri[2], k);
                add_edge(tri[2], tri[0], k);
            }
        }

        // per-island projected extent + which island each emitted triangle belongs to
        std::map<int,int> root_to_island;
        std::vector<float> imin, imax;
        std::vector<int>   tri_island(its.indices.size());
        for (size_t k = 0; k < its.indices.size(); ++k) {
            const int root = find(int(k));
            auto rit = root_to_island.find(root);
            int isl;
            if (rit == root_to_island.end()) {
                isl = int(imin.size());
                root_to_island.emplace(root, isl);
                imin.push_back(1e9f); imax.push_back(-1e9f);
            } else isl = rit->second;
            tri_island[k] = isl;
            for (int c = 0; c < 3; ++c) {
                const stl_vertex& v = its.vertices[its.indices[k][c]];
                const float pr = -v.x() * sN + v.y() * cN;
                imin[isl] = std::min(imin[isl], pr);
                imax[isl] = std::max(imax[isl], pr);
            }
        }

        // build one WeaveParams per island; map its facets to the new weave_list entry
        std::vector<int> island_weave(imin.size(), -1);
        for (size_t isl = 0; isl < imin.size(); ++isl) {
            TriangleSelectorPatch::WeaveParams w = is_pathblend
                ? pathblend_make_weave(pbc, mats, bg_rgb, lh, imin[isl], imax[isl], line_w)
                : colorstitch_make_weave(kv, fcolors, theta, imin[isl], imax[isl], line_w);
            if (!w.on) continue;
            island_weave[isl] = int(weave_list.size());
            weave_list.push_back(std::move(w));
        }
        for (size_t k = 0; k < src.size(); ++k) {
            const int wi = island_weave[tri_island[k]];
            if (wi >= 0) facet_weave_idx[src[k]] = wi;
        }
    }
}

void GLGizmoColorMixPainter::refresh_selector_palettes()
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
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
// Materiales del contexto actual: color de filamento (project_config) + TD
// (app_config neotko_td_N). Mismo origen que el SandwichDialog.
void GLGizmoColorMixPainter::gizmo_materials(Slic3r::ColorSci::Material out[4],
                                             std::vector<std::string>& fcolors_out) const
{
    fcolors_out.clear();
    if (auto* o = wxGetApp().preset_bundle->project_config
                      .option<ConfigOptionStrings>("filament_colour"))
        fcolors_out = o->values;
    while (fcolors_out.size() < 4) fcolors_out.push_back("#808080");

    auto* ac = wxGetApp().app_config;
    for (int t = 0; t < 4; ++t) {
        float td = 1.f;
        if (ac) {
            const std::string v = ac->get("neotko_td_" + std::to_string(t + 1));
            try { if (!v.empty()) td = std::stof(v); } catch (...) {}
        }
        td = std::max(0.01f, std::min(10.f, td));
        out[t] = Slic3r::ColorSci::material_from_hex(fcolors_out[t], td);
    }
}

// NEOTKO_SANDWICH_TAG — Fase 2 (s167 plan): mirrors the tool-detection half of
// Print::resolve_mixed_filament_sandwich_profiles() (PrintApply.cpp) — same
// "first model_part volume's extruder_id()" pattern, GUI-side, so every
// preview swatch composes against the colour the object is actually assigned
// to instead of the black every ColorSci::sandwich_colour_stacked caller
// hardcoded before this.
bool GLGizmoColorMixPainter::resolve_object_base_bg(const Slic3r::ColorSci::Material mats[4],
                                                     float bg_rgb[3]) const
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo)
        return false;

    int extruder_id = 0;
    for (const ModelVolume* mv : mo->volumes)
        if (mv && mv->is_model_part()) { extruder_id = mv->extruder_id(); break; }
    // ModelVolume::extruder_id() returns 0 when neither the volume nor the
    // object has an explicit "extruder" option set (Model.cpp) — that's NOT
    // "unresolvable", the engine's own default-extruder convention treats an
    // unassigned object as T0. Bailing out here would wrongly fall back to
    // black for the (very common) freshly-imported, no-tool-assigned object.
    if (extruder_id <= 0)
        extruder_id = 1;

    // Same source as the "MixedFilament Object" toggle above (mf_num_physical) —
    // filament_colour, not filament_presets, so the physical/virtual boundary
    // matches what that block already treats as authoritative in this file.
    size_t num_physical = 0;
    if (auto* o = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
        num_physical = o->values.size();
    if ((size_t)extruder_id <= num_physical) {
        const int idx = std::clamp(extruder_id - 1, 0, 3);
        bg_rgb[0] = mats[idx].rgb[0];
        bg_rgb[1] = mats[idx].rgb[1];
        bg_rgb[2] = mats[idx].rgb[2];
        return true;
    }

    // Virtual MixedFilament id — approximate with the same TD-aware
    // side-by-side blend_parallel() build_mixed_filament_recipe() uses
    // (ColorPredict.cpp), NOT the naive RGB average the swatch-list display
    // color uses. Known approximation (the real print composes via the
    // engine's actual dither pattern), not a new gap — documented in
    // PAINTED_EFFECTS_PREVIEW_TD_PLAN.md §2.1.
    const MixedFilament* mf = wxGetApp().preset_bundle->mixed_filaments
                                   .mixed_filament_from_id((unsigned)extruder_id, num_physical);
    if (!mf)
        return false;
    const int mix_b = std::clamp(mf->mix_b_percent, 0, 100);
    const int a = std::clamp<int>((int)mf->component_a - 1, 0, 3);
    const int b = std::clamp<int>((int)mf->component_b - 1, 0, 3);
    std::vector<Slic3r::ColorSci::Slice> slices;
    slices.push_back({ a, (100 - mix_b) / 100.f });
    slices.push_back({ b, mix_b / 100.f });
    Slic3r::ColorSci::blend_parallel(slices, mats, bg_rgb);
    return true;
}

// s169 F3 — ¿el objeto activo tiene "MixedFilament Object" en ON? Mismo patrón
// de lectura que object_has_mixed_filament (config option en el ModelObject).
bool GLGizmoColorMixPainter::active_object_mixed_filament_mode() const
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return false;
    const auto* opt = dynamic_cast<const ConfigOptionBool*>(mo->config.option("mixed_filament_sandwich_mode"));
    return opt && opt->value;
}

static double GLGizmoColorMixPainter_layer_height()
{
    if (auto* o = wxGetApp().preset_bundle->prints.get_edited_preset()
                      .config.option<ConfigOptionFloat>("layer_height"))
        if (o->value > 0.001) return o->value;
    return 0.2;
}

// NEOTKO_COLORSTITCH_TAG — resolved TOP-surface line width (mm), "casi real": the
// weave runs on top-facing surfaces, so the stripe pitch should match what the slicer
// lays there. Resolved from config WITHOUT a slice (top_surface_line_width → line_width
// → nozzle), so a Calculate/pre-slice pass is not required for the pitch. See on-screen
// notice for the only thing slicing would add (per-layer auto-angle).
static double GLGizmoColorMixPainter_top_line_width()
{
    const auto& cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double nozzle = 0.4;
    if (auto* nd = wxGetApp().preset_bundle->printers.get_edited_preset()
                       .config.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!nd->values.empty() && nd->values.front() > 0.05) nozzle = nd->values.front();
    // base line width: 0/auto → Orca's ~nozzle default.
    double line_w = nozzle * 1.125;
    if (auto* lw = cfg.option<ConfigOptionFloatOrPercent>("line_width")) {
        const double v = lw->get_abs_value(nozzle);
        if (v > 0.05) line_w = v;
    }
    // top width is ratio_over line_width.
    double top = line_w;
    if (auto* tw = cfg.option<ConfigOptionFloatOrPercent>("top_surface_line_width")) {
        const double v = tw->get_abs_value(line_w);
        if (v > 0.05) top = v;
    }
    return (top > 0.05) ? top : 0.45;
}

// Regenera las tres paletas SOLO cuando cambia el contexto. La firma incluye
// filamentos + TD + layer height + tools del gradient.
void GLGizmoColorMixPainter::rebuild_palettes_if_stale()
{
    namespace CS = Slic3r::ColorSci;
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    const double lh = GLGizmoColorMixPainter_layer_height();

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

void GLGizmoColorMixPainter::render_palette_panel(float window_width)
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

    const float strip_h = ImGui::GetTextLineHeight() * 3.2f;
    int ci;
    // s120: banda "Mixed approximation (predict)" retirada — el painter ofrece
    // Gradient ramp (top-only) + Flat color. El penu lo añade el usuario aparte.
    // Gradient: header propio para alojar los selectores A/B de tool en medio.
    // Cambiar m_grad_tool_a/b basta — rebuild_palettes_if_stale lo detecta en la
    // firma de caché y regenera la tira sola.
    if (ImGui::CollapsingHeader(_u8L("Gradient ramp").c_str())) {
        const int ta = draw_tool_selector_row("##grad_a", _u8L("Start (A)").c_str(),
                                               fcolors, nfil, m_grad_tool_a);
        const int tb = draw_tool_selector_row("##grad_b", _u8L("End (B)").c_str(),
                                               fcolors, nfil, m_grad_tool_b);
        if (ta >= 0) m_grad_tool_a = ta;
        if (tb >= 0) m_grad_tool_b = tb;
        ci = draw_palette_strip("##pal_gradient", m_pal_gradient, fcolors,
                                window_width, strip_h);
        if (ci >= 0) set_active_recipe(m_pal_gradient[ci], _u8L("Gradient"));
    }
    ci = draw_palette_section("##pal_flat", _u8L("Flat color"),
                              m_pal_flat, fcolors, window_width, strip_h);
    if (ci >= 0) set_active_recipe(m_pal_flat[ci], _u8L("Flat"));

    // s171 — "Mixed (ColorStitch)" NUKEADO (predict_mixed_palette, ruta muerta
    // desde s120, siempre predecía amarillo). "ColorStitch Pattern Color": mismo
    // header propio que Gradient ramp (selectores A/B en medio), pero la tira
    // sale de build_colorstitch_gradient_palette — solo ColorStitch, 2 pasadas
    // (Top+Penu) sobrepuestas, mismos tools/ángulo.
    if (ImGui::CollapsingHeader(_u8L("ColorStitch Pattern Color").c_str())) {
        const int ta = draw_tool_selector_row("##cs_a", _u8L("Start (A)").c_str(),
                                               fcolors, nfil, m_cs_tool_a);
        const int tb = draw_tool_selector_row("##cs_b", _u8L("End (B)").c_str(),
                                               fcolors, nfil, m_cs_tool_b);
        if (ta >= 0) m_cs_tool_a = ta;
        if (tb >= 0) m_cs_tool_b = tb;
        ci = draw_palette_strip("##pal_cs_gradient", m_pal_cs_gradient, fcolors,
                                window_width, strip_h);
        if (ci >= 0) set_active_recipe(m_pal_cs_gradient[ci], _u8L("ColorStitch"));
    }

    // (s169 F1: el swatch "Active colour" vive ahora en el header persistente
    //  — render_header — y la biblioteca guardada en la rejilla de Paint —
    //  render_paint_palette_grid.)

    // NEOTKO_COLORSTITCH_TAG — the weave preview is now always on (it matches the slice);
    // the "Preview weave" toggle + the auto-angle (!) notice were retired. m_weave_preview
    // stays as an internal flag (default true) in case we need to disable it programmatically.
    ImGui::Separator();
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

// NEOTKO_COLORSTITCH_TAG — Bandeja "pro mode": compone Top/Penu + TD y muestra
// el color resultante en vivo (sandwich_colour_stacked, mismo motor que el
// Sandwich Editor). Produce un ColorRecipe → color activo de pintura (sin pasar
// por el preset). Opción B del revamp; v2 s108: filas estilo SandwichDialog con
// los tres kinds inline (Solid / ColorStitch / PathBlend Half|Full), cajita Z
// editable con auto-rescale y TD en rejilla de 2 columnas al final.
void GLGizmoColorMixPainter::render_pro_mode_panel()
{
    namespace CS = Slic3r::ColorSci;
    // s169 F1 — el CollapsingHeader "Pro mode" se retiró: el departamento Pro DEL
    // selector segmentado ya hace de gate (solo se llama a esta función cuando
    // m_department==2), así que su contenido se dibuja siempre que se invoque.

    std::vector<std::string> fcolors;
    CS::Material mats[4];
    gizmo_materials(mats, fcolors);          // fcolors viene rellenado a >=4
    int nfil = 4;
    if (auto* op = wxGetApp().preset_bundle->project_config
                       .option<ConfigOptionStrings>("filament_colour"))
        nfil = std::max(1, std::min(4, (int)op->values.size()));

    const double lh = GLGizmoColorMixPainter_layer_height();

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

    // NEOTKO_BOTTOM_TAG — surface discriminator. The gizmo is a COMPRESSED painter
    // (the full UX is the Surface Sandwich editor), so it shows ONE surface at a time:
    //   Top Surface    → Top + Penultimate zones
    //   Bottom Surface → Bottom zone + its supported-control switch
    // Pure VIEW toggle: all three stacks persist below regardless of the active mode,
    // so flipping it never discards authored passes (the user can't lose work). Opens
    // on Top by default.
    {
        const bool top_sel = (m_pro_surface_mode == 0);
        if (!top_sel) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
        if (ImGui::Button(_u8L("Top Surface").c_str()) && !top_sel) m_pro_surface_mode = 0;
        if (!top_sel) ImGui::PopStyleVar();
        ImGui::SameLine();
        const bool bot_sel = (m_pro_surface_mode == 1);
        if (!bot_sel) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
        if (ImGui::Button(_u8L("Bottom Surface").c_str()) && !bot_sel) m_pro_surface_mode = 1;
        if (!bot_sel) ImGui::PopStyleVar();
    }
    ImGui::Spacing();

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

    if (m_pro_surface_mode == 0) {
        draw_zone_editor("##pro_top",  _u8L("Top").c_str(),         m_pro_top,
                         /*allow_disable=*/false, /*penu=*/false, fcolors, nfil, lh, pro_row_w);
        ImGui::Spacing();
        draw_zone_editor("##pro_penu", _u8L("Penultimate").c_str(), m_pro_penu,
                         /*allow_disable=*/true,  /*penu=*/true,  fcolors, nfil, lh, pro_row_w);
        ImGui::Spacing();
    } else {
        // Bottom zone — same authoring widget, §5.5 caps, "+ Add Bottom Paint" wording.
        draw_zone_editor("##pro_bottom", "", m_pro_bottom,
                         /*allow_disable=*/true,  /*penu=*/false, fcolors, nfil, lh, pro_row_w,
                         /*bottom_caps=*/true,
                         _u8L("+ Add Bottom Paint").c_str(),
                         _u8L("x Clear Bottom Paint").c_str());
        ImGui::Spacing();

        // NEOTKO_BOTTOM_TAG — §5.3 supported-bottom control, scoped to the Bottom
        // surface only. OFF = single full-height pass (paint-only; a real bridge stays
        // a bridge). ON = up to 3 Z-stacked passes (pass 0 keeps the base treatment,
        // passes above print solid). Persists in stack_bottom_json (read at Fill.cpp).
        {
            bool sup = m_pro_bottom.bottom_supported_control;
            if (ImGui::Checkbox(_u8L("Supported bottom — control (stack passes)").c_str(), &sup))
                m_pro_bottom.bottom_supported_control = sup;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("ON: treat this painted bottom as SUPPORTED and control it "
                                             "(up to 3 stacked passes; pass 0 keeps the base treatment, "
                                             "passes above print solid). OFF: single full-height pass, "
                                             "paint only — leave OFF for real bridges (overhangs over air).").c_str());
        }
        ImGui::Spacing();
    }

    // NEOTKO_COLORSTITCH_TAG — s118: Perimeter override ÚNICO para el color (no
    // per-zona). El motor lo lee por-zona (Fill.cpp mp_stack.perimeter_override), así
    // que un solo checkbox replica el flag a top y penu. Misma semántica que el
    // SandwichDialog tras unificarlo allí.
    {
        bool perim = m_pro_top.perimeter_override || m_pro_penu.perimeter_override;
        if (ImGui::Checkbox(_u8L("Perimeter override").c_str(), &perim)) {
            m_pro_top.perimeter_override  = perim;
            m_pro_penu.perimeter_override = perim;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Clone the walls into every Solid pass "
                                         "(MultiPass perimeter override).").c_str());
    }

    ImGui::Spacing();

    // --- (TD) per filamento, rejilla de 2 columnas (live; invalida la caché de
    // paletas). TD = Transmission Distance — nomenclatura usuario s108, NO
    // "translucency". Column-major (T1/T2 izquierda, T3/T4 derecha); crece por
    // filas si nfil sube en el futuro.
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "(TD)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Transmission distance").c_str());
    render_td_grid();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // --- Color resultante en vivo (Top+Penu — el color ACTIVO de pintura; el
    // WIP de Bottom aún no forma parte de la receta, ver NEOTKO_BOTTOM_TAG) ---
    float out[3] = {0.f, 0.f, 0.f};
    // NEOTKO_SANDWICH_TAG — Fase 2 (s167 plan): fondo real del objeto activo
    // en vez de negro hardcodeado (falls back to black when unresolvable —
    // no object selected, or its extruder can't be read).
    float bg[3] = {0.f, 0.f, 0.f};
    resolve_object_base_bg(mats, bg);
    CS::sandwich_colour_stacked(m_pro_top, m_pro_penu, mats, bg, out);

    // s169 F4 — dual preview "Recipe | Result" (estilo Add Mix): reemplaza el
    // swatch único de antes (mismo cálculo de Result, solo reubicado) + una
    // caja "Recipe" nueva a la izquierda. Recipe muestra la zona que se está
    // EDITANDO ahora (Top+Penu apilados, o Bottom sola en modo Bottom); Result
    // siempre refleja Top+Penu (el color activo de pintura).
    {
        const float rw = m_imgui->scaled(6.f);
        const float rh = m_imgui->scaled(4.f);

        ImGui::BeginGroup();
        m_imgui->text(_u8L("Recipe"));
        {
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(rw, rh));
            if (m_pro_surface_mode == 1) {
                draw_zone(dl, rp, ImVec2(rp.x + rw, rp.y + rh), m_pro_bottom, false, fcolors);
            } else if (!m_pro_penu.enabled || m_pro_penu.passes.empty()) {
                draw_zone(dl, rp, ImVec2(rp.x + rw, rp.y + rh), m_pro_top, false, fcolors);
            } else {
                const float midy = rp.y + rh * 0.5f;
                draw_zone(dl, rp, ImVec2(rp.x + rw, midy - 1.f), m_pro_top,  false, fcolors);
                draw_zone(dl, ImVec2(rp.x, midy + 1.f), ImVec2(rp.x + rw, rp.y + rh), m_pro_penu, true, fcolors);
            }
        }
        ImGui::EndGroup();

        ImGui::SameLine(0.f, 20.f);

        ImGui::BeginGroup();
        m_imgui->text(_u8L("Result"));
        {
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(sp, ImVec2(sp.x + rw, sp.y + rh),
                              IM_COL32((int)std::min(255.f, out[0] * 255.f),
                                       (int)std::min(255.f, out[1] * 255.f),
                                       (int)std::min(255.f, out[2] * 255.f), 255));
            dl->AddRect(sp, ImVec2(sp.x + rw, sp.y + rh), IM_COL32(255, 255, 255, 255));
            ImGui::Dummy(ImVec2(rw, rh));
        }
        ImGui::EndGroup();
    }

    // --- El Pro ES el color activo (s118 binding live, puntos 4+6) ---
    // Sin botones "Use as paint colour"/"Save as palette": editar el Pro actualiza
    // el color seleccionado auto-mágicamente. Si hay un perfil enlazado
    // (m_selected_profile_id: auto recién pintado o guardado), se reescribe EN SITIO
    // → cambia el objeto y TODOS los que usen ese perfil (decisión usuario s118).
    // "Pin to palette" promueve el color de trabajo a la biblioteca guardada.
    // s118: garantizar payload de los pases ColorMix antes de persistir (panel
    // plegado o pase cargado sin tocar) → el motor no los degrada a Solid.
    pro_backfill_cm(m_pro_top,  /*penu=*/false);
    pro_backfill_cm(m_pro_penu, /*penu=*/true);
    pro_backfill_cm(m_pro_bottom, /*penu=*/false);   // NEOTKO_BOTTOM_TAG — Fase 0 (WIP)

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
                p->colormix  = {};   // limpiar payload viejo (payload_from_stacks solo añade)
                p->pathblend = {};
                SurfaceEffectProfileManager::payload_from_stacks(recipe.top, recipe.penu, *p);
                refresh_selector_palettes();   // overlay + swatches al día en vivo
                m_preview_dirty = true;
                // NEOTKO_COLORSTITCH_TAG — editing a ColorStitch profile changes the saved
                // project content, so mark it unsaved (the re-slice alone didn't flag the doc
                // dirty → the Save/file option stayed inactive). No snapshot = no undo spam.
                wxGetApp().plater()->set_plater_dirty(true);
                // NEOTKO_COLORSTITCH_TAG — editar el contenido del perfil no toca las
                // facetas ni el mapeo de slots. Agendamos el background process; la
                // invalidación REAL la decide Print::apply, que recalcula la huella de
                // contenido del perfil desde el manager (model_colormix_paint_data_changed,
                // Model.cpp) → re-slice sin depender de este camino concreto.
                m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
                NEOTKO_LOG(PROFILE, "PRO live-rewrite RESLICE posted SCHEDULE_BG"
                    << " profile_id=" << p->id);
                NEOTKO_LOG(PROFILE, "PRO live-rewrite profile_id=" << p->id
                    << " name='" << p->name << "' top_empty=" << tj.empty()
                    << " penu_empty=" << pj.empty());
            }
        }
    }

    // s169 F2 — el nombre editable + "Pin to palette" se movieron al header
    // persistente (render_header): mismo código, solo cambia dónde se dibuja
    // (visible en todos los departamentos, no solo en Pro).
    ImGui::Separator();
}

// s169 F0 — rejilla (TD) por filamento, extraída tal cual de render_pro_mode_panel
// (mismo par td_changed/td_committed → save()+SCHEDULE_BACKGROUND_PROCESS de la
// Fase 1 de s167/s168, sin regresionar) para que Create/Object (F1/F3) la reusen
// bajo su propio wrapper. Self-contained: relee fcolors/nfil por su cuenta (mismo
// patrón de padding que render_palette_panel).
void GLGizmoColorMixPainter::render_td_grid()
{
    std::vector<std::string> fcolors;
    if (auto* o = wxGetApp().preset_bundle->project_config
                      .option<ConfigOptionStrings>("filament_colour"))
        fcolors = o->values;
    const int nfil = std::max(1, std::min(4, (int)fcolors.size()));
    while (fcolors.size() < 4) fcolors.push_back("#808080");

    auto* ac = wxGetApp().app_config;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int   td_rows = (nfil + 1) / 2;
    const float col_w   = ImGui::GetContentRegionAvail().x * 0.5f;
    const float sq      = ImGui::GetTextLineHeight();
    auto td_cell = [&](int t) {
        ImGui::PushID(2000 + t);
        float td = 1.f;
        if (ac) {
            const std::string v = ac->get("neotko_td_" + std::to_string(t + 1));
            try { if (!v.empty()) td = std::stof(v); } catch (...) {}
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + sq, p.y + sq), tool_col_u32(fcolors, t));
        dl->AddRect(p, ImVec2(p.x + sq, p.y + sq), IM_COL32(20, 20, 20, 255));
        ImGui::Dummy(ImVec2(sq, sq));
        ImGui::SameLine();
        ImGui::PushItemWidth(std::max(70.f, col_w - sq - 18.f));
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
            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
    };
    for (int r = 0; r < td_rows; ++r) {
        td_cell(r);
        const int t2 = td_rows + r;
        if (t2 < nfil) { ImGui::SameLine(col_w + 8.f); td_cell(t2); }
    }
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

// Click en swatch: SOLO fija el color activo. No crea profile ni asigna slot —
// navegar la paleta no contamina nada. El slot se materializa al pintar.
void GLGizmoColorMixPainter::set_active_recipe(
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
    m_has_active_recipe = true;
    m_active_resolved   = false;   // se materializará al primer trazo
    m_active_slot       = 0;
    // NEOTKO_COLORSTITCH_TAG — s118: un swatch de las tiras predict (Mixed/Gradient/
    // Flat) es un color de trabajo NUEVO, aún SIN enlazar a un perfil. Desenlazar
    // aquí evita que el write-back live del Pro reescriba el perfil que estuviera
    // enlazado antes; se materializa/enlaza por dedup al primer trazo (ensure_active_slot).
    m_selected_profile_id = 0;
    // s111 — elegir un color implica querer pintar: salir de Select a modo bucket.
    m_select_mode       = false;
    m_erase_mode        = false;
    // s112 — Pro mode dual: que el panel Pro refleje el color recién elegido.
    load_recipe_into_pro(r);
}

// s112 — vuelca una receta a los editores de zona Pro. El Result se recalcula
// solo cada frame en render_pro_mode_panel (sandwich_colour_stacked), así que
// basta con fijar los stacks + marcar sembrado.
void GLGizmoColorMixPainter::load_recipe_into_pro(const Slic3r::ColorSci::ColorRecipe& r)
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
    m_pro_seeded = true;   // no re-sembrar el Solid default encima
}

// Materializa el color activo como slot pintable la primera vez que se pinta.
// Dedup por (top_json,penu_json): si ya existe un profile (auto o guardado) con
// esos blobs se reusa; si no, se crea uno AUTO. Idempotente vía m_active_resolved.
int GLGizmoColorMixPainter::ensure_active_slot()
{
    if (!m_has_active_recipe) return m_active_slot;
    if (m_active_resolved)    return m_active_slot;

    // s118: backfill de payloads ColorMix también en el camino de materialización
    // directa (predict swatch pintado sin abrir el Pro) → nunca se persiste un
    // ColorMix vacío que el motor degradaría a Solid.
    pro_backfill_cm(m_active_recipe.top,  /*penu=*/false);
    pro_backfill_cm(m_active_recipe.penu, /*penu=*/true);

    const std::string top_json  = m_active_recipe.top.to_json();
    const std::string penu_json = m_active_recipe.penu.to_json();

    auto& mgr = SurfaceEffectProfileManager::get();
    int id = 0;
    for (const SurfaceEffectProfile& p : mgr.list())
        if (p.stack_top_json == top_json && p.stack_penu_json == penu_json) { id = p.id; break; }

    if (id == 0) {
        SurfaceEffectProfile p;
        p.name            = recipe_name(m_active_recipe, m_active_style);
        p.stack_top_json  = top_json;
        p.stack_penu_json = penu_json;
        p.preview_argb    = recipe_argb(m_active_recipe);
        p.auto_generated  = true;       // capa "auto" — oculta de la lista + GC-able
        // NEOTKO_COLORSTITCH_TAG — s112 fix: derive the engine payload from the
        // recipe stacks so the painted surface actually slices (the gate reads
        // p.colormix.present, not the visual stack json).
        SurfaceEffectProfileManager::payload_from_stacks(
            m_active_recipe.top, m_active_recipe.penu, p);
        id = mgr.add(std::move(p));
    } else if (SurfaceEffectProfile* ex = mgr.find_mut(id);
               ex && !ex->colormix.present && !ex->pathblend.present) {
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
void GLGizmoColorMixPainter::garbage_collect_auto_profiles()
{
    // NEOTKO_COLORSTITCH_TAG — s139: el set de referencias debe cubrir TODO el
    // proyecto, no solo el objeto activo. Antes barría sólo
    // selection_info()->model_object() → al pintar A, cambiar a B y disparar gc
    // (on_shutdown / Remove all) se borraba el auto-profile de A aunque su slot lo
    // siguiera referenciando → slot→pid colgando = caja gris fantasma. Cierra de
    // RAÍZ el caso (b) documentado en SurfaceColorMix.cpp (s138 lo hizo inerte al
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
                if (mv->colormix_slot_to_profile_id[s] != 0)
                    referenced.insert(mv->colormix_slot_to_profile_id[s]);
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
void GLGizmoColorMixPainter::save_active_as_palette()
{
    if (!m_has_active_recipe) return;
    const std::string top_json  = m_active_recipe.top.to_json();
    const std::string penu_json = m_active_recipe.penu.to_json();

    auto& mgr = SurfaceEffectProfileManager::get();
    for (const SurfaceEffectProfile& p : mgr.list())
        if (p.stack_top_json == top_json && p.stack_penu_json == penu_json) {
            if (SurfaceEffectProfile* mp = mgr.find_mut(p.id)) {
                // NEOTKO_COLORSTITCH_TAG — s137: promoting a working colour (auto) to
                // the library files it into the active group. An already-saved profile
                // keeps its group (re-pinning must not silently move it).
                if (mp->auto_generated)
                    mp->name = cs_with_group(mp->name, m_active_group);
                mp->auto_generated = false;
                // NEOTKO_COLORSTITCH_TAG — s112: backfill payload if missing.
                if (!mp->colormix.present && !mp->pathblend.present)
                    SurfaceEffectProfileManager::payload_from_stacks(
                        m_active_recipe.top, m_active_recipe.penu, *mp);
            }
            m_selected_profile_id = p.id;
            return;
        }

    SurfaceEffectProfile p;
    // NEOTKO_COLORSTITCH_TAG — s137: file the new palette into the active group.
    p.name            = cs_with_group(recipe_name(m_active_recipe, m_active_style), m_active_group);
    p.stack_top_json  = top_json;
    p.stack_penu_json = penu_json;
    p.preview_argb    = recipe_argb(m_active_recipe);
    p.auto_generated  = false;
    // NEOTKO_COLORSTITCH_TAG — s112 fix: payload so the saved palette slices.
    SurfaceEffectProfileManager::payload_from_stacks(
        m_active_recipe.top, m_active_recipe.penu, p);
    m_selected_profile_id = mgr.add(std::move(p));
}

// NEOTKO_COLORSTITCH_TAG — s140: ¿queda algún color de trabajo sin guardar?
// "Sin guardar" = profile auto_generated (efímero, oculto de la lista, GC-able).
bool GLGizmoColorMixPainter::has_unsaved_palettes() const
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
int GLGizmoColorMixPainter::save_all_palettes()
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
        if (!mp->colormix.present && !mp->pathblend.present) {
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

void GLGizmoColorMixPainter::show_tooltip_information(float caption_max, float x, float y)
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

// s169 F0 — toggle "pill" transparente (teal cuando activo), extraído tal cual
// del lambda local `tool_toggle` de render_tool_row para que el futuro selector
// segmentado de departamentos (F1) pueda reusar el mismo idiom visual.
static bool cs_toggle_button(const char* label, bool active, const char* tip)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    // s173 — feedback usuario ("bonito y claro" en la propuesta de zonas): el
    // activo pasa de un tinte teal casi transparente (0.25) a un relleno SÓLIDO
    // (0.85) + texto blanco forzado, para que se lea como pill/tab de verdad en
    // vez de un simple resaltado de texto. Inactivo sigue igual (texto sigue al
    // modo, sin fondo).
    ImGui::PushStyleColor(ImGuiCol_Text,
        active ? ImVec4(1.f, 1.f, 1.f, 1.f)
        : (ImGuiWrapper::is_dark_mode() ? ImVec4(1.f, 1.f, 1.f, 1.f)
                                        : ImVec4(50 / 255.f, 58 / 255.f, 61 / 255.f, 1.f)));
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.59f, 0.53f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.64f, 0.58f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImGuiWrapper::COL_ORCA);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }
    const bool clicked = ImGui::Button(label);
    if (active) { ImGui::PopStyleColor(3); ImGui::PopStyleVar(1); }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// s169 F1 (helper listo desde F0) — "card" con caja redondeada adaptada al modo
// (Add-Mix style). begin/end envuelven contenido ImGui arbitrario; end() pinta la
// caja DETRÁS del contenido (canal 0, truco ChannelsSplit ya usado en
// draw_zone_editor) + el título encima, y deja el cursor bajo la caja con el hueco
// de separación entre cards. Sin llamadas aún (F0 = cero cambio visual).
static void cs_card_begin()
{
    ImGui::BeginGroup();
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
}

// s174 — `same_line_after`: cuando la card debe seguir compartiendo fila con lo
// que venga después (p.ej. header + toolbar + erase-all en una sola línea, pedido
// del usuario "recolocamos"), en vez de forzar el cursor abajo-izquierda (el
// comportamiento de apilado original, que se mantiene por defecto para cards
// futuras verticales) se deja la línea ABIERTA con un SameLine() propio.
static void cs_card_end(const char* title, bool same_line_after = false)
{
    ImGui::EndGroup();
    const ImVec2 gmin = ImGui::GetItemRectMin();
    const ImVec2 gmax = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    const bool  dark = ImGuiWrapper::is_dark_mode();
    const float pad  = 6.f;
    const ImVec2 b0(gmin.x - pad, gmin.y - pad);
    const ImVec2 b1(gmax.x + pad, gmax.y + pad);
    // s173 fix — el fill (52,52,52) era casi idéntico al fondo real del panel
    // (COL_WINDOW_BG_DARK = 45,45,49) y el borde iba a 47% de alpha: la card
    // resultaba invisible en la práctica (motivo real de "lo veo igual", no un
    // problema de rebuild). Delta de fill mayor + borde opaco, mismo criterio de
    // contraste que ya usa pro_pass_preview (borde opaco 20,20,20 contra fondo 45).
    dl->AddRectFilled(b0, b1, dark ? IM_COL32(64, 64, 68, 255) : IM_COL32(228, 228, 228, 255), 4.f);
    dl->AddRect(b0, b1, dark ? IM_COL32(20, 20, 20, 255) : IM_COL32(0, 0, 0, 90), 4.f);
    if (title && *title)
        dl->AddText(ImVec2(b0.x, b0.y - ImGui::GetTextLineHeight()),
                    IM_COL32(178, 178, 178, 255), title);   // gris (0.7,0.7,0.7,1)
    dl->ChannelsMerge();
    if (same_line_after) {
        ImGui::SameLine(0.f, pad * 2.f + 6.f);   // respeta el padding derecho de la card
    } else {
        ImGui::SetCursorScreenPos(ImVec2(gmin.x, b1.y));
        ImGui::Dummy(ImVec2(gmax.x - gmin.x, 6.f));   // separación vertical entre cards
    }
}

// s174 — selector de departamento como BARRA segmentada de verdad (pista con
// fondo propio + segmentos proporcionales al ancho + activo = relleno sólido
// que ocupa TODO su segmento), reemplazando los 4 cs_toggle_button sueltos que
// dejaban huecos de aire desigual — pedido del usuario: "la barra de selección
// bien hecha como tu mockup, no como sale ahora". `active` se actualiza in-place.
static void cs_segmented_bar(const char* const* labels, const char* const* tips,
                             int n, int& active)
{
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  h     = ImGui::GetFrameHeight();
    const float  seg_w = avail / (float)n;
    const bool   dark  = ImGuiWrapper::is_dark_mode();
    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const ImVec2 p0    = ImGui::GetCursorScreenPos();

    dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + h),
                      dark ? IM_COL32(32, 32, 35, 255) : IM_COL32(205, 205, 205, 255), 4.f);

    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const ImVec2 a(p0.x + seg_w * (float)i, p0.y);
        const ImVec2 b(a.x + seg_w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##seg", ImVec2(seg_w, h))) active = i;
        const bool hov      = ImGui::IsItemHovered();
        const bool is_active = (i == active);
        if (is_active)
            dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.f, 0.59f, 0.53f, 0.85f)), 4.f);
        else if (hov)
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 20), 4.f);
        const ImVec2 tsz  = ImGui::CalcTextSize(labels[i]);
        const ImU32  tcol = is_active ? IM_COL32(255, 255, 255, 255)
                          : (dark ? IM_COL32(220, 220, 220, 255) : IM_COL32(50, 58, 61, 255));
        dl->AddText(ImVec2(a.x + (seg_w - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f), tcol, labels[i]);
        if (hov && tips[i] && *tips[i]) ImGui::SetTooltip("%s", tips[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
    ImGui::Dummy(ImVec2(avail, 0.f));
}

// s173 — carga (UNA vez) los 5 iconos de la toolbar que Fable entregó como SVG en
// resources/images/ (cs_tool_*.svg, 4 variantes cada uno: light/dark × normal/
// hover). Mismo mecanismo que el botón (?) (IMTexture::load_from_svg_file →
// ImTextureID), pero en un mapa PRIVADO de este painter — ver nota en el .hpp.
void GLGizmoColorMixPainter::ensure_tool_icons_loaded()
{
    if (m_tool_icons_loaded) return;
    m_tool_icons_loaded = true;
    const std::string dir = Slic3r::resources_dir() + "/images/";
    auto load = [&](const char* filename, void*& out) {
        ImTextureID tid;
        if (IMTexture::load_from_svg_file(dir + filename, 25, 25, tid)) out = tid;
    };
    load("cs_tool_select.svg",             m_icon_select.normal);
    load("cs_tool_select_dark.svg",        m_icon_select.normal_dark);
    load("cs_tool_select_hover.svg",       m_icon_select.hover);
    load("cs_tool_select_hover_dark.svg",  m_icon_select.hover_dark);
    load("cs_tool_paint.svg",              m_icon_paint.normal);
    load("cs_tool_paint_dark.svg",         m_icon_paint.normal_dark);
    load("cs_tool_paint_hover.svg",        m_icon_paint.hover);
    load("cs_tool_paint_hover_dark.svg",   m_icon_paint.hover_dark);
    load("cs_tool_eraser.svg",             m_icon_eraser.normal);
    load("cs_tool_eraser_dark.svg",        m_icon_eraser.normal_dark);
    load("cs_tool_eraser_hover.svg",       m_icon_eraser.hover);
    load("cs_tool_eraser_hover_dark.svg",  m_icon_eraser.hover_dark);
    load("cs_tool_pick.svg",               m_icon_pick.normal);
    load("cs_tool_pick_dark.svg",          m_icon_pick.normal_dark);
    load("cs_tool_pick_hover.svg",         m_icon_pick.hover);
    load("cs_tool_pick_hover_dark.svg",    m_icon_pick.hover_dark);
    load("cs_tool_erase_all.svg",              m_icon_erase_all.normal);
    load("cs_tool_erase_all_dark.svg",         m_icon_erase_all.normal_dark);
    load("cs_tool_erase_all_hover.svg",        m_icon_erase_all.hover);
    load("cs_tool_erase_all_hover_dark.svg",   m_icon_erase_all.hover_dark);
}

// s173 — icono-toggle: mismo idioma visual que cs_toggle_button (fondo/borde
// teal sólido cuando activo, transparente cuando no) pero dibujando la textura
// del icono en vez de texto. ImageButton3 ya soporta bg_col + swap normal/hover
// nativo (ImageButtonEx3, imgui_widgets.cpp:1173) — el borde sale de
// ImGuiCol_Button/Hovered/Active (FrameBorderSize ya viene a 1.0 desde
// push_toolbar_style), así que basta con empujar esos 3 colores cuando active.
static bool cs_icon_toggle_button(void* normal_id, void* hover_id, bool active,
                                  float size_px, const char* tip)
{
    const ImVec2 sz(size_px, size_px);
    // Igual que cs_toggle_button: SIEMPRE transparente de base (idle = sin caja
    // visible, solo el icono) y, si active, se empuja el teal sólido ENCIMA —
    // si no se hiciera así, el borde heredado de push_toolbar_style (gris,
    // FrameBorderSize ya a 1.0) dejaría una cajita visible incluso en idle.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImVec4 bg(0.f, 0.f, 0.f, 0.f);
    if (active) {
        bg = ImVec4(0.f, 0.59f, 0.53f, 0.85f);
        ImGui::PushStyleColor(ImGuiCol_Button,        bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.64f, 0.58f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.f, 0.64f, 0.58f, 0.95f));
    }
    const bool clicked = ImGui::ImageButton3((ImTextureID)normal_id, (ImTextureID)hover_id,
                                              sz, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                                              -1, bg);
    if (active) ImGui::PopStyleColor(3);
    ImGui::PopStyleColor(1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// s111 — fila de herramientas [Select][Paint][Eraser][Pick] (toggles mutuamente
// excluyentes). Select intercepta clics para marcar/activar objetos (re-activa el
// picking); Paint pinta; Eraser despinta. NEOTKO_COLORSTITCH_TAG. s173: iconos
// reales (Fable) en vez de texto/glifo, mismo idioma de 2 zonas de color.
void GLGizmoColorMixPainter::render_tool_row()
{
    ensure_tool_icons_loaded();
    const bool  dark     = ImGuiWrapper::is_dark_mode();
    const float icon_px  = 20.f * m_parent.get_scale();

    // s173 — agrupados en su propia card (zona "modo de herramienta", teal):
    // feedback usuario, propuesta de 2 zonas de color aprobada — separa
    // visualmente el MODO de pintura de la acción destructiva "Erase all
    // painting" (que queda fuera de esta card, con su propio tinte de peligro).
    cs_card_begin();
    if (cs_icon_toggle_button(dark ? m_icon_select.normal_dark : m_icon_select.normal,
                              dark ? m_icon_select.hover_dark  : m_icon_select.hover,
                              m_select_mode, icon_px,
                              _u8L("Select objects to paint — click them in the scene "
                                   "(Shift-click to unmark)").c_str()))
        set_tool_mode(/*select=*/true, /*erase=*/false);
    ImGui::SameLine();
    if (cs_icon_toggle_button(dark ? m_icon_paint.normal_dark : m_icon_paint.normal,
                              dark ? m_icon_paint.hover_dark  : m_icon_paint.hover,
                              // NEOTKO_STICKER_TAG — Paint es el catch-all "nada más
                              // activo"; sin excluir pick/sticker aquí se mostraba
                              // encendido A LA VEZ que Sticker (bug reportado: "da la
                              // sensación de que puedes hacer las dos cosas").
                              !m_select_mode && !m_erase_mode && !m_pick_mode && !m_sticker_mode, icon_px,
                              _u8L("Paint (smart fill)").c_str()))
        set_tool_mode(/*select=*/false, /*erase=*/false);
    ImGui::SameLine();
    if (cs_icon_toggle_button(dark ? m_icon_eraser.normal_dark : m_icon_eraser.normal,
                              dark ? m_icon_eraser.hover_dark  : m_icon_eraser.hover,
                              !m_select_mode && m_erase_mode, icon_px,
                              _u8L("Eraser — smart-fill removes paint").c_str()))
        set_tool_mode(/*select=*/false, /*erase=*/true);
    ImGui::SameLine();
    // NEOTKO_COLORSTITCH_TAG — s118: eyedropper. Click sobre un objeto → lee su
    // receta pintada y la enlaza como color activo (+ dump de debug).
    if (cs_icon_toggle_button(dark ? m_icon_pick.normal_dark : m_icon_pick.normal,
                              dark ? m_icon_pick.hover_dark  : m_icon_pick.hover,
                              m_pick_mode, icon_px,
                              _u8L("Eyedropper — click a painted object to load its colour "
                                   "(and dump what it has painted / its base).").c_str())) {
        m_select_mode  = false;
        m_erase_mode   = false;
        m_pick_mode    = true;
        m_sticker_mode = false;        // NEOTKO_STICKER_TAG
        m_editing_sticker_idx = -1;    // NEOTKO_STICKER_TAG
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
    ImGui::SameLine();
    // NEOTKO_STICKER_TAG — sin icono propio todavía (los 5 SVG de Fable de s173
    // son Select/Paint/Eraser/Pick/EraseAll); texto plano hasta que haya un
    // sexto asset, mismo idioma visual que `cs_toggle_button` (pill teal).
    if (cs_toggle_button(_u8L("Sticker").c_str(), m_sticker_mode,
                         _u8L("Sticker — click a flat top face to place the loaded SVG "
                              "(load it below, in the Palette panel)").c_str())) {
        m_select_mode  = false;
        m_erase_mode   = false;
        m_pick_mode    = false;
        m_sticker_mode = true;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
    cs_card_end(nullptr, /*same_line_after=*/true);   // s174 — sigue en la misma fila (info + erase-all)
}

// NEOTKO_COLORSTITCH_TAG — s118: eyedropper + debug read. Lee la receta pintada de
// un objeto, vuelca a PROFILE TODO lo que tiene (slots pintados + base sandwich del
// preset) y la enlaza como color activo (mismo camino que un swatch guardado).
void GLGizmoColorMixPainter::pick_recipe_from_object(int object_idx, int picked_slot)
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

    int first_pid  = 0;
    int picked_pid = 0;   // perfil del slot REAL bajo el cursor
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid == 0) continue;
            const SurfaceEffectProfile* p = mgr.find(pid);
            const SurfacePassStack st_top  = p ? SurfacePassStack::from_json(p->stack_top_json)  : SurfacePassStack{};
            const SurfacePassStack st_penu = p ? SurfacePassStack::from_json(p->stack_penu_json) : SurfacePassStack{};
            // NEOTKO_COLORSTITCH_TAG — s118 dbg: resumen de KINDS por zona (S=Solid,
            // C=ColorMix, H=PB-Half, F=PB-Full) → ver si el penu lleva el EFECTO o
            // sólo Solid plano (causa de "penu slicea pero sin efecto").
            auto kinds = [](const SurfacePassStack& st) {
                std::string s; using K = SurfacePassKind;
                for (const auto& pp : st.passes)
                    s += (pp.kind == K::Solid ? 'S' : pp.kind == K::ColorMix ? 'C'
                        : pp.kind == K::PathBlend ? 'P' : '?');
                return s.empty() ? std::string("-") : s;
            };
            NEOTKO_LOG(PROFILE, "PICK obj='" << mo->name << "' vol='" << mv->name
                << "' slot=" << s << " pid=" << pid
                << (s == picked_slot ? " <== UNDER CURSOR" : "")
                << " name='" << (p ? p->name : std::string("?")) << "'"
                << " cm=" << (p && p->colormix.present)
                << " pb=" << (p && p->pathblend.present)
                << " top[" << kinds(st_top) << "] penu[" << kinds(st_penu) << "]");
            if (first_pid == 0) first_pid = pid;
            if (s == picked_slot && picked_pid == 0) picked_pid = pid;
        }
    }

    // Preferir el perfil de la faceta clicada; si la cara no estaba pintada (slot 0)
    // o no se resolvió, caer al primer slot pintado del objeto.
    const int sel_pid = (picked_pid != 0) ? picked_pid : first_pid;
    if (sel_pid == 0) {
        NEOTKO_LOG(PROFILE, "PICK obj='" << mo->name << "' picked_slot=" << picked_slot
            << " → no painted slots");
        return;
    }

    // Enlazar la receta pintada como color activo (camino del swatch guardado).
    const SurfaceEffectProfile* p = mgr.find(sel_pid);
    if (!p) return;
    const CS::ColorRecipe r = recipe_from_profile(*p);
    m_select_mode = false; m_erase_mode = false; m_pick_mode = false;
    m_sticker_mode = false;        // NEOTKO_STICKER_TAG
    m_editing_sticker_idx = -1;    // NEOTKO_STICKER_TAG
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
bool GLGizmoColorMixPainter::load_sticker_svg_dialog()
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

    m_pending_sticker_svg  = ss.str();
    m_pending_sticker_name = boost::filesystem::path(path).stem().string();
    NEOTKO_LOG(PROFILE, "STICKER_LOAD name='" << m_pending_sticker_name
        << "' bytes=" << m_pending_sticker_svg.size());
    return true;
}

// NEOTKO_STICKER_TAG — coloca el SVG pendiente como nueva pegatina: plana en el
// plano XY del frame-objeto (v1, sin rotación), en la posición del click. `hit_local`
// vive en el frame LOCAL del mesh del volumen impactado (rr_hit()); `mv->get_matrix()`
// lo lleva al frame-objeto compartido — el MISMO frame que `sticker_footprint_slice_frame`
// espera (ver comentario en Model.hpp: ColorMixSticker::transform). Se apila al FINAL
// (back() = tope de la pila = ocluye a las demás, convención fijada en SurfaceColorMix.cpp).
void GLGizmoColorMixPainter::place_sticker_at(const ModelVolume* mv, const Vec3f& hit_local)
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || !mv || m_pending_sticker_svg.empty()) return;

    const Vec3d hit_obj = mv->get_matrix() * hit_local.cast<double>();

    ColorMixSticker st;
    st.name       = m_pending_sticker_name;
    st.svg_data   = m_pending_sticker_svg;
    st.profile_id = m_selected_profile_id;   // misma fuente que "con qué color pinto ahora"
    st.transform  = Transform3d::Identity();
    st.transform.pretranslate(hit_obj);
    mo->colormix_stickers.push_back(std::move(st));

    m_preview_dirty = true;
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    NEOTKO_LOG(PROFILE, "STICKER_PLACE obj='" << mo->name << "' name='" << m_pending_sticker_name
        << "' profile_id=" << m_selected_profile_id
        << " pos=(" << hit_obj.x() << "," << hit_obj.y() << "," << hit_obj.z() << ")"
        << " pile_size=" << mo->colormix_stickers.size());
}

// NEOTKO_STICKER_TAG — entra en modo edición (mover/rotar) para el sticker en
// `idx`. Fuerza la herramienta Sticker activa (mutex con Select/Paint/Eraser/
// Pick) y decodifica el ángulo Z actual de su transform para el slider — ver
// nota del header sobre por qué esa decodificación es EXACTA (invariante
// Translation*RotationZ que nosotros mismos garantizamos).
void GLGizmoColorMixPainter::enter_sticker_edit(int idx)
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || idx < 0 || idx >= (int)mo->colormix_stickers.size()) return;

    m_select_mode  = false;
    m_erase_mode   = false;
    m_pick_mode    = false;
    m_sticker_mode = true;
    m_editing_sticker_idx = idx;
    m_sticker_dragging    = false;

    const ColorMixSticker& st = mo->colormix_stickers[idx];
    m_editing_spin_deg = float(std::atan2(st.transform(1, 0), st.transform(0, 0)) * 180.0 / M_PI);
    m_editing_scale    = float(st.transform.linear().col(0).norm());
    m_sticker_overlay_built_for = -1;   // fuerza reconstruir el overlay para ESTE sticker

    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoColorMixPainter::exit_sticker_edit()
{
    m_editing_sticker_idx = -1;
    m_sticker_dragging    = false;
    m_parent.set_as_dirty();
}

// NEOTKO_STICKER_TAG — construye (una vez por sticker en edición) la geometría
// GL del overlay en frame LOCAL (z=0, sin colocación): contorno (líneas, vía
// `sticker_rings_in_transform` + `GLModel::init_from(Polygons, z)`, soporta
// huecos/cóncavo tal cual) + relleno (triángulos, vía `triangulate_expolygons_2f`
// — el mismo tesselador glu que usa el resto de Orca para generar sólidos desde
// ExPolygons, así que letras/logos con huecos salen correctos, no una
// aproximación). Reusar `SurfaceColorMix::sticker_rings_in_transform` evita
// duplicar el parseo NSVG que ya vive en el motor de slice.
void GLGizmoColorMixPainter::ensure_sticker_overlay_built(const ColorMixSticker& sticker)
{
    if (m_sticker_overlay_built_for == m_editing_sticker_idx) return;
    m_sticker_overlay_built_for = m_editing_sticker_idx;

    m_sticker_overlay_fill.reset();
    m_sticker_overlay_outline.reset();

    const Polygons rings = SurfaceColorMix::sticker_rings_in_transform(sticker, Transform3d::Identity());
    if (rings.empty()) return;

    m_sticker_overlay_outline.init_from(rings, 0.f);

    const ExPolygons local_expolys = union_ex(rings);
    const std::vector<Vec2f> tris2d = triangulate_expolygons_2f(local_expolys);
    if (tris2d.size() >= 3) {
        indexed_triangle_set its;
        its.vertices.reserve(tris2d.size());
        for (const Vec2f& p : tris2d) its.vertices.emplace_back(p.x(), p.y(), 0.f);
        its.indices.reserve(tris2d.size() / 3);
        for (size_t i = 0; i + 2 < tris2d.size(); i += 3)
            its.indices.emplace_back(int(i), int(i + 1), int(i + 2));
        m_sticker_overlay_fill.init_from(its);
    }
}

// NEOTKO_STICKER_TAG — dibuja el sticker en edición con SU color asignado (no
// solo contorno), para que mover/rotar se haga sabiendo cómo va a quedar contra
// lo que ya hay debajo/alrededor — pedido explícito del usuario. Mismo patrón
// que GLGizmoFlatten::on_render (shader "flat", GLModel construido en frame
// LOCAL, transform vía uniform view_model_matrix, pequeño alzado en Z mundo
// para evitar z-fighting con la superficie real).
void GLGizmoColorMixPainter::render_sticker_edit_overlay()
{
    if (m_editing_sticker_idx < 0) return;
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || m_editing_sticker_idx >= (int)mo->colormix_stickers.size()) { exit_sticker_edit(); return; }
    if (mo->instances.empty()) return;

    const ColorMixSticker& st = mo->colormix_stickers[m_editing_sticker_idx];
    ensure_sticker_overlay_built(st);
    if (!m_sticker_overlay_outline.is_initialized() && !m_sticker_overlay_fill.is_initialized()) return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (!shader) return;

    const ModelInstance* mi = mo->instances.front();
    const Camera& camera = wxGetApp().plater()->get_camera();
    // Alzado en Z MUNDO (no local) para evitar z-fighting con la superficie real
    // — mismo truco que GLGizmoFlatten::update_planes ("Raise a bit above the
    // object surface to avoid flickering"), aplicado tras el resto de la cadena
    // para que sea robusto a la orientación de la instancia.
    const Transform3d lift = Geometry::translation_transform(Vec3d(0.0, 0.0, 0.05));
    const Transform3d model_matrix = lift * mi->get_transformation().get_matrix() * st.transform;
    const Transform3d view_model_matrix = camera.get_view_matrix() * model_matrix;

    shader->start_using();
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_DEPTH_TEST));

    const SurfaceEffectProfile* p = st.profile_id ? SurfaceEffectProfileManager::get().find(st.profile_id) : nullptr;
    ColorRGBA fill_col = p ? color_for_profile(*p) : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
    fill_col[3] = 0.55f;   // semi-transparente: se ve el color Y lo que hay debajo
    if (m_sticker_overlay_fill.is_initialized()) {
        m_sticker_overlay_fill.set_color(fill_col);
        m_sticker_overlay_fill.render();
    }
    if (m_sticker_overlay_outline.is_initialized()) {
        m_sticker_overlay_outline.set_color(ColorRGBA(1.f, 1.f, 1.f, 0.9f));
        m_sticker_overlay_outline.render();
    }

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// NEOTKO_STICKER_TAG — sección del departamento Palette: cargar SVG + lista de
// pegatinas del objeto activo (orden = pila, back()=tope). Mostrada TOP-DOWN
// (el primer row de la lista es el que OCLUYE a los de abajo) para que coincida
// con el modelo mental de "apilar pegatinas" del plan.
void GLGizmoColorMixPainter::render_sticker_section()
{
    const bool open = ImGui::CollapsingHeader(_u8L("Stickers (SVG)").c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Place a 1-colour SVG shape on a flat top face and give it "
                                     "its own Sandwich recipe. Stack several — the topmost "
                                     "occludes the ones below (no blending).").c_str());
    if (!open) return;

    if (m_imgui->button(_L("Load SVG...")))
        load_sticker_svg_dialog();
    if (!m_pending_sticker_svg.empty()) {
        ImGui::SameLine();
        m_imgui->text_colored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f),
            (_u8L("Loaded:") + " " + m_pending_sticker_name).c_str());
        ImGui::PushTextWrapPos(0.f);
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            _u8L("Pick the Sticker tool above, then click a flat top face to place it.").c_str());
        ImGui::PopTextWrapPos();
    }

    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo || mo->colormix_stickers.empty()) {
        m_imgui->text_colored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), _u8L("No stickers on this object yet.").c_str());
        return;
    }

    ImGui::Separator();
    auto& mgr = SurfaceEffectProfileManager::get();
    const size_t n = mo->colormix_stickers.size();
    int move_from = -1, move_to = -1;
    int remove_idx = -1;
    for (size_t disp = 0; disp < n; ++disp) {
        const size_t i = n - 1 - disp;   // top-down: back() (tope) primero
        ColorMixSticker& st = mo->colormix_stickers[i];
        ImGui::PushID((int)i);

        const SurfaceEffectProfile* p = st.profile_id ? mgr.find(st.profile_id) : nullptr;
        const ColorRGBA sw_col = p ? color_for_profile(*p) : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
        ImGui::ColorButton("##stk_sw", ImVec4(sw_col.r(), sw_col.g(), sw_col.b(), 1.f),
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                            ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::TextUnformatted(st.name.empty() ? "?" : st.name.c_str());
        ImGui::SameLine();
        m_imgui->text_colored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            (p ? cs_strip_group(p->name) : _u8L("(no profile)")).c_str());

        if (m_imgui->button(_L("Assign active")))
            st.profile_id = m_selected_profile_id;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Assign the currently selected palette profile "
                                         "(the one you'd paint with) to this sticker").c_str());
        ImGui::SameLine();
        m_imgui->disabled_begin(disp == 0);
        if (m_imgui->button("^")) { move_from = (int)i; move_to = (int)i + 1; }
        m_imgui->disabled_end();
        ImGui::SameLine();
        m_imgui->disabled_begin(disp == n - 1);
        if (m_imgui->button("v")) { move_from = (int)i; move_to = (int)i - 1; }
        m_imgui->disabled_end();
        ImGui::SameLine();
        if (m_imgui->button(_L("Remove")))
            remove_idx = (int)i;

        // NEOTKO_STICKER_TAG — mover/rotar/escalar: "Edit placement" entra en
        // el modo descrito en on_mouse (arrastrar mueve); estos sliders rotan
        // Z y escalan; "Done" sale. Cada slider reescribe transform EN VIVO
        // conservando lo demás (posición/ángulo/escala) y solo agenda re-slice
        // al soltar (IsItemDeactivatedAfterEdit), igual que el drag de
        // posición solo re-slicea en LeftUp — evita spamear el scheduler
        // mientras se ajusta.
        const bool is_editing_this = ((int)i == m_editing_sticker_idx);
        if (is_editing_this) {
            if (m_imgui->button(_L("Done")))
                exit_sticker_edit();
            ImGui::SameLine();
            ImGui::TextUnformatted(_u8L("Rotate").c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(m_imgui->scaled(6.f));
            const std::string spin_fmt = std::string("%.0f") + I18N::translate_utf8("°", "deg");
            if (ImGui::SliderFloat("##stk_spin", &m_editing_spin_deg, -180.f, 180.f, spin_fmt.c_str())) {
                const Vec3d pos = st.transform.translation();
                const double spin_rad = double(m_editing_spin_deg) * M_PI / 180.0;
                st.transform = Eigen::Translation3d(pos) * Eigen::AngleAxisd(spin_rad, Vec3d::UnitZ())
                             * Eigen::Scaling(double(m_editing_scale));
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Rotate around the vertical axis").c_str());
            if (ImGui::IsItemDeactivatedAfterEdit())
                m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));

            ImGui::TextUnformatted(_u8L("Scale").c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(m_imgui->scaled(6.f));
            if (ImGui::SliderFloat("##stk_scale", &m_editing_scale, 0.2f, 5.0f, "%.2fx")) {
                const Vec3d pos = st.transform.translation();
                const double spin_rad = double(m_editing_spin_deg) * M_PI / 180.0;
                st.transform = Eigen::Translation3d(pos) * Eigen::AngleAxisd(spin_rad, Vec3d::UnitZ())
                             * Eigen::Scaling(double(m_editing_scale));
                // Nota: NO hace falta invalidar m_sticker_overlay_built_for — la
                // geometría del overlay vive en frame LOCAL (1:1, sin escalar);
                // el tamaño en pantalla sale del uniform view_model_matrix
                // (que ya incluye Scaling(m_editing_scale) vía st.transform),
                // releído cada frame en render_sticker_edit_overlay().
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Resize the sticker (uniform)").c_str());
            if (ImGui::IsItemDeactivatedAfterEdit())
                m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        } else {
            ImGui::SameLine();
            if (m_imgui->button(_L("Edit placement")))
                enter_sticker_edit((int)i);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Drag on the model to move it, then use the "
                                             "slider to rotate — the colour preview shows "
                                             "how it will land against everything else.").c_str());
        }

        ImGui::PopID();
    }

    if (move_from >= 0 && move_to >= 0 && move_to < (int)n) {
        std::swap(mo->colormix_stickers[move_from], mo->colormix_stickers[move_to]);
        if (m_editing_sticker_idx == move_from) m_editing_sticker_idx = move_to;
        else if (m_editing_sticker_idx == move_to) m_editing_sticker_idx = move_from;
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
    if (remove_idx >= 0 && remove_idx < (int)mo->colormix_stickers.size()) {
        mo->colormix_stickers.erase(mo->colormix_stickers.begin() + remove_idx);
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
void GLGizmoColorMixPainter::render_group_selector()
{
    auto& mgr = SurfaceEffectProfileManager::get();

    int max_group = m_active_group;   // mostrar al menos hasta el activo (grupo "nuevo" vacío)
    for (const SurfaceEffectProfile& gp : mgr.list())
        max_group = std::max(max_group, cs_parse_group(gp.name));
    auto group_count = [&](int g) {
        int c = 0;
        for (const SurfaceEffectProfile& gp : mgr.list())
            if (!gp.auto_generated && cs_parse_group(gp.name) == g) ++c;
        return c;
    };

    m_imgui->text(_u8L("Palette group"));
    ImGui::SameLine();

    // NEOTKO — light mode readability for the dropdown popup (see ##kind combo above).
    const bool _grp_light = !ImGuiWrapper::is_dark_mode();
    if (_grp_light) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg,       ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.00f, 0.59f, 0.53f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.59f, 0.53f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.00f, 0.59f, 0.53f, 0.55f));
    }
    ImGui::PushItemWidth(ImGui::GetFontSize() * 9.f);
    char gid[40];
    std::snprintf(gid, sizeof(gid), "%s %d", _u8L("Group").c_str(), m_active_group);
    if (ImGui::BeginCombo("##cs_group", gid)) {
        for (int g = 1; g <= max_group; ++g) {
            char lbl[56];
            std::snprintf(lbl, sizeof(lbl), "%s %d  (%d)", _u8L("Group").c_str(), g, group_count(g));
            if (ImGui::Selectable(lbl, g == m_active_group)) m_active_group = g;
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    if (_grp_light) ImGui::PopStyleColor(5);

    // (Sin BeginDisabled: esta versión de ImGui no lo trae → dim manual + guarda.)
    const bool can_add = max_group < GLGizmoColorMixPainter::MAX_GROUPS;
    ImGui::SameLine();
    if (!can_add) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    if (m_imgui->button(_L("+ New group")) && can_add)
        m_active_group = std::min(max_group + 1, GLGizmoColorMixPainter::MAX_GROUPS);
    if (!can_add) ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _u8L("Create a new palette group").c_str());

    const bool can_del = m_active_group > 1;
    ImGui::SameLine();
    if (!can_del) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    if (m_imgui->button(_L("- Delete")) && can_del) {
        // Borrar grupo = mover sus colores al Grupo 1 (sin perder trabajo).
        Plater::TakeSnapshot snap(wxGetApp().plater(), _u8L("Delete ColorStitch group"),
                                  UndoRedo::SnapshotType::GizmoAction);
        std::vector<int> ids;
        for (const SurfaceEffectProfile& gp : mgr.list())
            if (cs_parse_group(gp.name) == m_active_group) ids.push_back(gp.id);
        for (int id : ids)
            if (const SurfaceEffectProfile* gp = mgr.find(id))
                mgr.rename(id, cs_strip_group(gp->name));   // → Grupo 1
        m_active_group = 1;
        refresh_selector_palettes();
    }
    if (!can_del) ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Delete group (its colours move to Group 1)").c_str());

    // Guía SUAVE (no bloquea): aviso si el grupo activo pasa de 30.
    const int active_cnt = group_count(m_active_group);
    if (active_cnt > 30) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.63f, 0.16f, 1.f), "%d/30", active_cnt);
    }
}

// s169 F0 — color-resultado predicho en vivo (top+penu compuestos contra el fondo
// real del objeto activo), extraído tal cual del lambda local `predict_argb` que
// vivía dentro de render_left_rail — para que el header/Object (F2/F3) puedan
// predecir un swatch sin duplicar la composición. Self-contained: recalcula
// mats/bg por su cuenta (mismo coste que ya pagaba cada swatch de la lista).
uint32_t GLGizmoColorMixPainter::predict_argb_for(const Slic3r::SurfacePassStack& top,
                                                  const Slic3r::SurfacePassStack& penu) const
{
    namespace CS = Slic3r::ColorSci;
    CS::Material mats[4];
    std::vector<std::string> fcolors;
    gizmo_materials(mats, fcolors);
    float bg[3] = {0.f, 0.f, 0.f};
    resolve_object_base_bg(mats, bg);
    float out[3] = {0.f, 0.f, 0.f};
    CS::sandwich_colour_stacked(top, penu, mats, bg, out);
    return 0xFF000000u
         | ((uint32_t)std::min(255.f, out[0] * 255.f) << 16)
         | ((uint32_t)std::min(255.f, out[1] * 255.f) <<  8)
         |  (uint32_t)std::min(255.f, out[2] * 255.f);
}

// s169 F1 — header persistente sobre el selector de departamentos: por ahora
// solo el swatch Active (reemplaza el bloque "Active" de render_left_rail,
// retirado junto al layout de 2 columnas). F2 añade aquí mismo "+ New" +
// nombre editable + Pin (movidos desde el final de Pro).
void GLGizmoColorMixPainter::render_header()
{
    auto& mgr = SurfaceEffectProfileManager::get();

    // Drop selection if its profile was deleted under us.
    if (m_selected_profile_id != 0 && mgr.find(m_selected_profile_id) == nullptr) {
        m_selected_profile_id = 0;
        m_active_slot         = 0;
    }
    // Resync active slot every frame (cheap; no mutation when slot already exists).
    if (m_selected_profile_id != 0)
        m_active_slot = slot_for_selected_profile(/*assign_if_missing=*/false);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hs = ImGui::GetTextLineHeight() * 1.6f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool have = false;
    if (m_has_active_recipe) {
        // Re-predecir en vivo con los TD actuales (no usar m_active_recipe.rgb,
        // que quedó congelado al seleccionar).
        const uint32_t argb = predict_argb_for(m_active_recipe.top, m_active_recipe.penu);
        dl->AddRectFilled(p, ImVec2(p.x + hs, p.y + hs),
                          IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
        have = true;
    } else if (const SurfaceEffectProfile* sp_sel = mgr.find(m_selected_profile_id)) {
        // Paleta guardada seleccionada → re-predecir su color-resultado en vivo
        // desde el stack (los TD actuales mandan, no el preview_argb cacheado).
        const SurfacePassStack st_top  = SurfacePassStack::from_json(sp_sel->stack_top_json);
        const SurfacePassStack st_penu = SurfacePassStack::from_json(sp_sel->stack_penu_json);
        const uint32_t argb = predict_argb_for(st_top, st_penu);
        dl->AddRectFilled(p, ImVec2(p.x + hs, p.y + hs),
                          IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
        have = true;
    }
    dl->AddRect(p, ImVec2(p.x + hs, p.y + hs),
                have ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 120, 120, 255));
    ImGui::Dummy(ImVec2(hs, hs));

    // s169 F2 — "+ New": receta en blanco (1 pase Solid T1, penu vacío) y salta a
    // Pro para editarla. set_active_recipe ya desenlaza el perfil anterior y
    // siembra m_pro_top/penu (load_recipe_into_pro) — no duplicar esa lógica aquí.
    ImGui::SameLine();
    if (m_imgui->button(_L("+ New"))) {
        Slic3r::ColorSci::ColorRecipe recipe;
        Slic3r::SurfacePass sp;
        sp.kind       = Slic3r::SurfacePassKind::Solid;
        sp.solid_tool = 0;
        sp.ratio      = 1.0;
        recipe.top.passes.push_back(sp);
        recipe.top.enabled = true;
        set_active_recipe(recipe, _u8L("Custom"));
        m_department = 2;   // saltar a Pro para editar la receta recién creada
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Start a new colour from scratch in Pro mode").c_str());

    // s174 — feedback usuario: el nombre editable no aportaba nada visible (el
    // color activo ya se ve en el swatch) y "Pin to palette" pasa a llamarse
    // simplemente "Save" — mismo save_active_as_palette() debajo, solo cambia
    // el label. Renombrar NO toca el nombre guardado del perfil (que se sigue
    // asignando internamente al materializar/promover).
    ImGui::SameLine();
    if (m_imgui->button(_L("Save")))
        save_active_as_palette();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Keep this colour in the saved palette library "
                                     "(otherwise it is a temporary working colour).").c_str());
}

// s169 F1 — rejilla full-width de paletas guardadas (reemplaza la columna
// vertical estrecha de render_left_rail — mismo contenido/comportamiento,
// solo cambia el layout: N swatches por fila en vez de 1). Conservados
// INTACTOS: click (carga+enlaza), right-click Delete diferido, borde ámbar
// de autos, tooltip con draw_zone, aviso slots_full.
void GLGizmoColorMixPainter::render_paint_palette_grid()
{
    namespace CS = Slic3r::ColorSci;
    auto& mgr = SurfaceEffectProfileManager::get();

    std::vector<std::string> fcolors;   // solo para el draw_zone del tooltip
    { CS::Material mats[4]; gizmo_materials(mats, fcolors); }

    m_imgui->text(m_desc["profiles"]);

    const ModelObject* mo = m_c->selection_info()->model_object();
    const ModelVolume* first_mv = nullptr;
    if (mo) for (const ModelVolume* mv : mo->volumes) if (mv->is_model_part()) { first_mv = mv; break; }

    // Helper: which slot (if any) does profile P occupy in this object?
    auto slot_of = [&](int pid) -> int {
        if (!first_mv) return 0;
        for (int s = 1; s < MAX_SLOTS; ++s)
            if (first_mv->colormix_slot_to_profile_id[s] == pid) return s;
        return 0;
    };

    int profile_to_delete = 0;   // diferido: no mutar mgr.list() durante la iteración
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float sw      = m_imgui->scaled(2.0f);                       // lado de cada swatch
    const int   per_row = std::max(1, (int)std::floor(avail_w / (sw + 4.f)));
    // NEOTKO_STICKER_TAG — feedback usuario: la rejilla se comía media pantalla
    // y dejaba la sección "Stickers (SVG)" (debajo) sin aire. Limitada a ~2
    // filas de swatches (calculado desde `sw`, no un alto fijo, para que siga
    // siendo robusto a DPI/escala); el resto hace scroll como siempre.
    const float grid_h  = 2.f * (sw + 4.f) + 8.f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.f, 2.f));
    // AlwaysVerticalScrollbar: reservar SIEMPRE el ancho de la scrollbar (evita
    // el "fliqueo" de s137/s138 — su aparición/desaparición cambiaría el ancho
    // disponible → cambiaría per_row/tamaño → oscilación cada frame).
    ImGui::BeginChild("##cmp_profile_grid", ImVec2(avail_w, grid_h), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (mgr.size() == 0) {
        ImGui::PushTextWrapPos(0.f);
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), m_desc["no_profiles"]);
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

            // Click derecho sobre el swatch → menú contextual con "Delete".
            if (ImGui::BeginPopupContextItem(nullptr)) {
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
                                   IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
            }
            ldl->AddRect(sp, ImVec2(sp.x + sw, sp.y + sw),
                         sel ? IM_COL32(255, 255, 255, 255)
                             : (hov ? IM_COL32(210, 210, 210, 255) : IM_COL32(20, 20, 20, 255)),
                         0.f, 0, sel ? 2.5f : 1.f);
            // NEOTKO_COLORSTITCH_TAG — s118: marca ámbar = color de TRABAJO (auto, no
            // guardado) que ocupa un slot; distinguible de las paletas guardadas para
            // borrarlo sin miedo (right-click→Delete).
            if (p.auto_generated)
                ldl->AddRect(ImVec2(sp.x + 2.f, sp.y + 2.f),
                             ImVec2(sp.x + sw - 2.f, sp.y + sw - 2.f),
                             IM_COL32(230, 160, 40, 220), 0.f, 0, 2.0f);

            // Hover → cajita sandwich (Top sobre Penu) como preview de
            // verificación + nombre / zonas / slot.
            if (hov) {
                ImGui::BeginTooltip();
                const ImVec2 tp = ImGui::GetCursorScreenPos();
                const float zw = 64.f, zh = 44.f;
                ImGui::Dummy(ImVec2(zw, zh));
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                if (st_penu.passes.empty()) {
                    draw_zone(tdl, tp, ImVec2(tp.x + zw, tp.y + zh), st_top, false, fcolors);
                } else {
                    const float midy = tp.y + zh * 0.5f;
                    draw_zone(tdl, tp, ImVec2(tp.x + zw, midy - 1.f), st_top,  false, fcolors);
                    draw_zone(tdl, ImVec2(tp.x, midy + 1.f), ImVec2(tp.x + zw, tp.y + zh), st_penu, true, fcolors);
                }
                ImGui::TextUnformatted(cs_strip_group(p.name).c_str());   // s137
                if (p.auto_generated)   // s118: aclara que es color de trabajo
                    ImGui::TextColored(ImVec4(0.9f, 0.63f, 0.16f, 1.f), "%s",
                                       _u8L("working colour (not saved) — right-click to delete").c_str());
                const int slot = slot_of(p.id);
                std::string d = "T:" + zone_desc(st_top);
                if (!st_penu.passes.empty()) d += "  P:" + zone_desc(st_penu);
                if (slot) d += "  (s" + std::to_string(slot) + ")";
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.f), "%s", d.c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.f), "%s",
                                   _u8L("Right-click: delete").c_str());
                ImGui::EndTooltip();
            }

            ImGui::PopID();
            ++shown;
            if (shown % per_row != 0) ImGui::SameLine(0.f, 4.f);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);   // ChildBorderSize + WindowPadding de la rejilla

    // Borrado diferido de un profile guardado (click derecho → Delete). Fuera del
    // child y de la iteración de mgr.list(). Limpia los slots que lo referencian en
    // todos los volúmenes para que las caras pintadas no adopten otro profile luego.
    if (profile_to_delete != 0) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete ColorStitch profile"),
                                      UndoRedo::SnapshotType::GizmoAction);
        ModelObject* mo_mut = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (mo_mut)
            for (ModelVolume* mv : mo_mut->volumes) {
                if (!mv->is_model_part()) continue;
                for (int s = 1; s < MAX_SLOTS; ++s)
                    if (mv->colormix_slot_to_profile_id[s] == profile_to_delete)
                        mv->colormix_slot_to_profile_id[s] = 0;
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
                if (mv->colormix_slot_to_profile_id[s] != 0) { free_everywhere = false; break; }
            }
            if (free_everywhere) any_free = true;
        }
        if (!any_free) {
            ImGui::PushTextWrapPos(0.f);
            m_imgui->text_colored(ImVec4(0.9f, 0.5f, 0.1f, 1.0f), m_desc["slots_full"]);
            ImGui::PopTextWrapPos();
        }
    }
}
// NEOTKO_COLORSTITCH_TAG_END

// s169 F3 — departamento Object: toggle "MixedFilament Object" + swatch
// (código movido tal cual desde render_pro_mode_panel, ver su historial) +
// tarjeta "Live recipe" (nueva): lee el perfil resuelto por el último apply
// (Print::mixed_filament_sandwich_profile_id) y lo muestra apilado + descrito
// pase a pase + el color Result en vivo. Cierra pendientes s162 #1 y #2.
void GLGizmoColorMixPainter::render_object_department()
{
    namespace CS = Slic3r::ColorSci;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const bool mf_has_mixed_filament = object_has_mixed_filament(mo);

    bool mf_mode_on = false;
    if (mo) {
        if (const auto* mf_opt = dynamic_cast<const ConfigOptionBool*>(mo->config.option("mixed_filament_sandwich_mode")))
            mf_mode_on = mf_opt->value;
    }

    m_imgui->disabled_begin(!mf_has_mixed_filament);
    if (ImGui::Checkbox(_u8L("MixedFilament Object").c_str(), &mf_mode_on) && mo)
        mo->config.set_key_value("mixed_filament_sandwich_mode", new ConfigOptionBool(mf_mode_on));
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", mf_has_mixed_filament
            ? _u8L("Replace this object's top surface and penultimate infill with an "
                   "auto-generated sandwich approximating this MixedFilament's colour "
                   "(Perimeter Override forced on). Disables painting/patterns for this "
                   "object.").c_str()
            : _u8L("Assign a MixedFilament to this object's extruder first.").c_str());

    if (mf_has_mixed_filament) {
        ImGui::SameLine();
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
            dl->AddRectFilled(p0, ImVec2(p0.x + sw, p0.y + sw), col);
            dl->AddRect(p0, ImVec2(p0.x + sw, p0.y + sw), IM_COL32(0, 0, 0, 80));
        }
        ImGui::Dummy(ImVec2(sw, sw));
    }
    ImGui::Spacing();

    if (!mf_has_mixed_filament) {
        ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.f),
            _u8L("Assign a MixedFilament to this object's extruder to use this department."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (!mf_mode_on) {
        ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.f),
            _u8L("Turn on \"MixedFilament Object\" above to replace this object's painting "
                 "with an auto-generated sandwich approximating its MixedFilament colour."));
        ImGui::PopTextWrapPos();
        return;
    }

    ImGui::Separator();

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

    m_imgui->text(_u8L("Live recipe"));
    if (pid == 0) {
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.f),
            _u8L("No recipe resolved yet — slice once to resolve"));
    } else if (const SurfaceEffectProfile* p = SurfaceEffectProfileManager::get().find(pid)) {
        const SurfacePassStack st_top  = SurfacePassStack::from_json(p->stack_top_json);
        const SurfacePassStack st_penu = SurfacePassStack::from_json(p->stack_penu_json);
        const double lh = GLGizmoColorMixPainter_layer_height();

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
            CS::sandwich_colour_stacked(st_top, st_penu, mats, bg, out);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const float  rh = ImGui::GetTextLineHeight() * 1.6f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(rp, ImVec2(rp.x + rh, rp.y + rh),
                              IM_COL32((int)std::min(255.f, out[0] * 255.f),
                                       (int)std::min(255.f, out[1] * 255.f),
                                       (int)std::min(255.f, out[2] * 255.f), 255));
            dl->AddRect(rp, ImVec2(rp.x + rh, rp.y + rh), IM_COL32(255, 255, 255, 255));
            ImGui::Dummy(ImVec2(rh, rh));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            m_imgui->text(_u8L("Result"));
        }
    }

    ImGui::Separator();

    // ---- (TD), repetida aquí: editarla dispara el mismo save()+SCHEDULE_BG de
    // siempre → apply → resolve_mixed_filament_sandwich_profiles() recalcula →
    // el live recipe de arriba se refresca solo (próximo frame post-reslice).
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "(TD)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Transmission distance").c_str());
    render_td_grid();

    ImGui::Spacing();
    ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
    m_imgui->text_colored(ImVec4(0.5f, 0.5f, 0.5f, 1.f),
        _u8L("Passes shown come from the last slice apply — may lag one cycle "
             "right after editing the MixedFilament itself."));
    ImGui::PopTextWrapPos();
}

void GLGizmoColorMixPainter::on_render_input_window(float x, float y, float bottom_limit)
{
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
        ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
        GizmoImguiBegin(get_name(),
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
                      | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        m_imgui->text_colored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "(WIP Beta)");
        ImGui::Separator();
        ImGui::PushTextWrapPos(m_imgui->scaled(16.f));
        m_imgui->text(_u8L("Click an object in the scene to start painting it. "
                           "Then pick a colour and paint; you can paint any object."));
        ImGui::PopTextWrapPos();
        render_tool_row();
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const float approx_height = m_imgui->scaled(22.f);
    y = std::min(y, bottom_limit - approx_height);
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(),
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
                  | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size            = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left  = std::max(
        m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f),
        m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f)
            + ImGui::GetStyle().FramePadding.x * 2);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x  + m_imgui->scaled(1.5f);
    const float sliders_left_width     = std::max(smart_fill_slider_left, clipping_slider_left);
    const float sliders_width          = m_imgui->scaled(7.0f);
    const float slider_icon_width      = m_imgui->get_slider_icon_size().x;
    const float drag_left_width        = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;
    const float window_width           = std::max(m_imgui->scaled(18.f),
                                            sliders_left_width + sliders_width + slider_icon_width);
    const float max_tooltip_width      = ImGui::GetFontSize() * 20.0f;

    float caption_max = 0.f;
    for (const auto& t : { "paint", "erase", "smart_fill_angle", "clipping_of_view" })
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[std::string(t) + "_caption"]).x);
    caption_max += m_imgui->scaled(1.f);

    // WIP beta banner — NEOTKO_COLORSTITCH_TAG (quick&dirty para la beta).
    m_imgui->text_colored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "(WIP Beta)");
    ImGui::Separator();

    // s169 F1 — revamp "departamentos" (estilo Add Mix): el layout de 2 columnas
    // (carril + cuerpo) muere; una sola columna con un selector segmentado
    // Paint/Create/Pro/Object que decide qué bloque se dibuja. Ancho estable
    // entre departamentos: esta línea impone un mínimo para que la ventana
    // AlwaysAutoResize no cambie de ancho al cambiar de pestaña (la altura sí
    // varía — eso ya pasaba con Pro antes de este revamp).
    ImGui::Dummy(ImVec2(m_imgui->scaled(24.f), 0.f));

    // ---- Header persistente: swatch Active + "+ New" + "Save" (s174 quita el
    // nombre editable, no aportaba nada; recolocado para compartir fila con la
    // toolbar de abajo — pedido del usuario) ------------------------------------
    render_header();
    ImGui::SameLine();

    // ---- Smart-Fill only ----------------------------------------------------
    // NEOTKO_PROFILE_TAG — the ColorMix Painter only paints coplanar top
    // surfaces, so the inherited brush tools (Circle/Sphere/Triangle) were
    // removed. Smart-Fill is the sole tool; pinned unconditionally regardless of
    // which department tab is visible — painting on the canvas must keep
    // working even while e.g. the Pro tab is open.
    m_current_tool = ImGui::FillButtonIcon;
    m_cursor_type  = TriangleSelector::CursorType::POINTER;
    m_tool_type    = ToolType::SMART_FILL;

    // s169 F3 — MixedFilament Object gobierna el objeto: Paint/Palette/Pro se
    // deshabilitan enteros (el motor bypassea el pintado por-cara para este
    // objeto igualmente) con un banner ámbar explicándolo. Object (el propio
    // departamento que gestiona el toggle) queda siempre operativo.
    const bool mf_mode_on = active_object_mixed_filament_mode();

    // s169 — Select/Paint/Eraser/Pick + "Erase all painting" pasan a ser
    // GLOBALES (antes solo vivían en Palette): feedback usuario — al editar
    // una receta en Pro a veces no sabes si sigue aplicada al mismo objeto (se
    // puede haber deseleccionado a pesar de estar en modo Select), y hacía
    // falta volver a Palette para limpiar/recoger de otro objeto/cambiar de
    // objeto. Solo reubicación por ahora — SIN iconos todavía (ver plan de
    // beauty-up para otra sesión, docs/WIP/PAINTER_TOOLBAR_ICONS_BEAUTYUP_PLAN.md).
    m_imgui->disabled_begin(mf_mode_on);
    render_tool_row();   // s174 — ya deja la línea abierta (cs_card_end same_line_after)

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    const float cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, cur_y);
    const float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    ImGui::SameLine();

    // s173 — zona "peligro" (coral): única acción irreversible de la fila, tinte
    // propio para que no se confunda visualmente con un cambio de modo (feedback
    // usuario, propuesta de 2 zonas de color aprobada). Mismos stops que el
    // mockup (coral 900/800 de la paleta de referencia) + icono real de Fable
    // (cs_tool_erase_all.svg — gota de pintura tachada) en vez de texto plano.
    ensure_tool_icons_loaded();
    {
        const bool  dark    = ImGuiWrapper::is_dark_mode();
        const float icon_px = 20.f * m_parent.get_scale();
        const ImVec4 coral_bg     (74  / 255.f, 27 / 255.f, 12 / 255.f, 1.f);
        const ImVec4 coral_bg_hov (113 / 255.f, 43 / 255.f, 19 / 255.f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Button,        coral_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, coral_bg_hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  coral_bg_hov);
        const bool _remove_all_clicked = ImGui::ImageButton3(
            (ImTextureID)(dark ? m_icon_erase_all.normal_dark : m_icon_erase_all.normal),
            (ImTextureID)(dark ? m_icon_erase_all.hover_dark  : m_icon_erase_all.hover),
            ImVec2(icon_px, icon_px), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), -1, coral_bg);
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Erase all painting").c_str());
        if (_remove_all_clicked) {
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
                    std::fill(std::begin(mv->colormix_slot_to_profile_id),
                              std::end  (mv->colormix_slot_to_profile_id), 0);
                }
            } else {
                // No-activos: limpiar la pintura guardada directamente en el modelo.
                for (ModelVolume* mv : mo->volumes) {
                    if (!mv->is_model_part()) continue;
                    mv->color_mix_paint_facets.reset();
                    std::fill(std::begin(mv->colormix_slot_to_profile_id),
                              std::end  (mv->colormix_slot_to_profile_id), 0);
                }
            }
        }
        m_selected_profile_id = 0;
        m_active_slot         = 0;
        m_has_active_recipe   = false;
        m_active_resolved     = false;
        m_preview_dirty       = true;
        garbage_collect_auto_profiles();   // PR.3: slots vaciados → recoge autos
        update_model_object();
        refresh_selector_palettes();
        m_parent.set_as_dirty();
        }
    }
    ImGui::PopStyleVar(2);
    m_imgui->disabled_end();
    ImGui::Separator();

    // ---- Selector de departamento — barra segmentada de verdad (s174) -------
    // Recolocado DEBAJO de header+toolbar (antes iba arriba) — pedido del
    // usuario, mismo orden que su mockup. Nombres (feedback s169): "Palette" =
    // pintar + biblioteca guardada; "Generator" = elegir entre paletas GENERADAS.
    {
        const std::string lbl0 = _u8L("Palette"),   lbl1 = _u8L("Generator"),
                           lbl2 = _u8L("Pro"),       lbl3 = _u8L("Object");
        const std::string tip0 = _u8L("Paint / erase / pick colours on the model");
        const std::string tip1 = _u8L("Generated palettes: gradient ramp / flat colour");
        const std::string tip2 = _u8L("Compose Top / Penu + TD");
        const std::string tip3 = _u8L("MixedFilament Object — object-wide sandwich");
        const char* labels[4] = { lbl0.c_str(), lbl1.c_str(), lbl2.c_str(), lbl3.c_str() };
        const char* tips[4]   = { tip0.c_str(), tip1.c_str(), tip2.c_str(), tip3.c_str() };
        cs_segmented_bar(labels, tips, 4, m_department);
    }
    ImGui::Separator();

    // ---- Contenido del departamento activo ----------------------------------
    switch (m_department) {
    case 0: { // Palette (renamed from "Paint" — feedback usuario s169)
        if (mf_mode_on) {
            m_imgui->text_colored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                _u8L("MixedFilament Object governs this object — painting is disabled"));
            ImGui::Separator();
        }
        m_imgui->disabled_begin(mf_mode_on);

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["smart_fill_angle"]);
        const std::string fmt = std::string("%.1f") + I18N::translate_utf8("°", "deg");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##cmp_smart_fill_angle", &m_smart_fill_angle,
                                            SmartFillAngleMin, SmartFillAngleMax, fmt.c_str(), 1.0f, true))
            for (auto& sel : m_triangle_selectors) {
                sel->seed_fill_unselect_all_triangles();
                sel->request_update_render_data();
            }
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        ImGui::BBLDragFloat("##cmp_smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.f, 0.f, "%.2f");

        ImGui::Separator();

        // s169 — (TD) vive AQUÍ, no en Generator (feedback usuario): refresca en
        // vivo la predicción de TODA la paleta guardada (rejilla de abajo) y del
        // swatch Active del header — Generator solo elige entre paletas ya
        // generadas, no es donde se "ve cómo queda" el resultado.
        {
            const bool td_open = ImGui::CollapsingHeader(_u8L("(TD)").c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Swatches below are re-predicted live using these TD values").c_str());
            if (td_open)
                render_td_grid();
        }
        ImGui::Spacing();

        // ---- Palette library: grupo activo + Save all + rejilla de guardados
        render_group_selector();
        if (has_unsaved_palettes()) {
            if (m_imgui->button(_L("Save all")))
                save_all_palettes();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Save every unsaved working colour into the active palette group").c_str());
        }
        ImGui::Spacing();
        render_paint_palette_grid();

        ImGui::Separator();

        // NEOTKO_STICKER_TAG — s170 confirmó Palette como departamento destino
        // para "pintar con SVG" (ver future_svg_sticker_sandwich.md). Card propia
        // para no mezclar visualmente con la rejilla de paletas de arriba.
        render_sticker_section();
        ImGui::Separator();

        // ---- Clipping plane ----
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        } else {
            if (m_imgui->button(m_desc.at("reset_direction")))
                wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
        }
        auto clp = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        const bool sl_clp = m_imgui->bbl_slider_float_style("##cmp_clp", &clp, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        const bool dr_clp = ImGui::BBLDragFloat("##cmp_clp_input", &clp, 0.05f, 0.f, 0.f, "%.2f");
        if (sl_clp || dr_clp) m_c->object_clipper()->set_position_by_ratio(clp, true);

        m_imgui->disabled_end();
        break;
    }
    case 1: { // Generator (elige entre paletas GENERADAS — no "crea" nada; (TD)
              // vive en Palette, no aquí — feedback usuario s169)
        if (mf_mode_on) {
            m_imgui->text_colored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                _u8L("MixedFilament Object governs this object — painting is disabled"));
            ImGui::Separator();
        }
        m_imgui->disabled_begin(mf_mode_on);
        // ---- Style palettes (PR.2 — ColorSci::build_palette, cached) -------
        render_palette_panel(window_width);
        m_imgui->disabled_end();
        break;
    }
    case 2: { // Pro
        if (mf_mode_on) {
            m_imgui->text_colored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                _u8L("MixedFilament Object governs this object — painting is disabled"));
            ImGui::Separator();
        }
        m_imgui->disabled_begin(mf_mode_on);
        render_pro_mode_panel();
        m_imgui->disabled_end();
        break;
    }
    default: { // Object — MixedFilament Object toggle + live recipe (F3)
        render_object_department();
        break;
    }
    }

    GizmoImguiEnd();

    ImGuiWrapper::pop_toolbar_style();
}

// ----------------------------------------------------------------------------
// Model ↔ TriangleSelector sync
// ----------------------------------------------------------------------------

void GLGizmoColorMixPainter::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
    m_preview_dirty = true;   // s111: la pintura cambió → refrescar preview de marcados
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
                    const int pid = mv->colormix_slot_to_profile_id[s];
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
                    slot_wants_bottom[m_active_slot] = slot_wants_bottom[m_active_slot]
                        || (m_pro_bottom.enabled && m_pro_bottom.any_effect());
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
        updated |= mv->color_mix_paint_facets.set(*m_triangle_selectors[idx]);
    }
    if (updated) {
        if (NeoDebug::enabled(NeoDebug::PROFILE)) {
            int painted_volumes = 0, painted_slots_total = 0;
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                ++painted_volumes;
                for (int s = 1; s < MAX_SLOTS; ++s)
                    if (mv->colormix_slot_to_profile_id[s] != 0) ++painted_slots_total;
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
        // El re-slice por paint ya lo dispara el cambio de facetas/slot (timestamp); la
        // huella de contenido la recalcula Print::apply. Solo agendamos el proceso.
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoColorMixPainter::update_from_model_object(bool /*first_update*/)
{
    // s111: sin wxBusyCursor — ahora se reconstruye también al pre-activar objetos
    // en hover (multi-objeto), y el cursor "ocupado" parpadearía en cada cruce.
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    m_triangle_selectors.clear();
    // Selection is a per-gizmo field; clear it when the active object changes
    // (or when a fresh 3mf is loaded) so the "slots full" warning doesn't fire
    // against a stale profile id whose slot table has been wiped.
    m_selected_profile_id = 0;
    m_active_slot         = 0;
    if (!mo) return;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(
            *mesh, build_ebt_colors_for_volume(mv)));
        const EnforcerBlockerType max_ebt = static_cast<EnforcerBlockerType>(MAX_SLOTS - 1);
        m_triangle_selectors.back()->deserialize(mv->color_mix_paint_facets.get_data(), false, max_ebt);
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
