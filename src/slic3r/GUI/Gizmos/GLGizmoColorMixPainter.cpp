// NEOTKO_PROFILE_TAG_START
#include "GLGizmoColorMixPainter.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SurfaceColorMix.hpp" // NEOTKO_PROFILE_TAG — NeoDebug PROFILE channel
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/ColorSci/StackFlatten.hpp" // NEOTKO_COLORSTITCH_TAG — sandwich_colour_stacked (pro mode live preview)

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
#include "slic3r/GUI/ColorStitchPatternLauncher.hpp" // NEOTKO_COLORSTITCH_TAG — botón ADV → editor de patrón
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <boost/nowide/convert.hpp>

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
    m_select_mode = select;
    m_erase_mode  = erase;
    m_pick_mode   = false;   // s118: cualquier modo normal apaga el eyedropper
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

    return GLGizmoPainterBase::on_mouse(mouse_event);
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
// PathBlend: ramp→cap horizontal blend (Half fades into the background).
static void pro_pass_preview(ImDrawList* dl, ImVec2 a, ImVec2 b,
                             const SurfacePass& p, bool penu,
                             const std::vector<std::string>& fcolors)
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
        std::string pat = pro_cm_pattern(p, penu);
        if (pat.empty()) pat = "12";
        const float bw = 6.f;
        int i = 0;
        for (float x = a.x; x < b.x; x += bw, ++i) {
            const int t = std::clamp((int)pat[i % (int)pat.size()] - (int)'1', 0, 3);
            dl->AddRectFilled(ImVec2(x, a.y), ImVec2(std::min(x + bw, b.x), b.y),
                              tool_col_u32(fcolors, t));
        }
        break;
    }
    case SurfacePassKind::PathBlend: {
        const PathBlendPassConfig pbc = pro_pb_read(p);
        const ImU32 cb = tool_col_u32(fcolors, std::max(0, pbc.tool_bottom));
        const ImU32 ct = (pbc.mode == PathBlendPassConfig::Mode::Full)
                       ? tool_col_u32(fcolors, std::max(0, pbc.tool_top))
                       : IM_COL32(45, 45, 45, 255);
        dl->AddRectFilledMultiColor(a, b, cb, ct, ct, cb);
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
                             int nfil, double layer_h, float row_avail_w)
{
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
            if (ImGui::SmallButton(_u8L("+ Add penultimate").c_str())) {
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
        if (ImGui::SmallButton(_u8L("x Clear penultimate").c_str())) {
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

    const float zbox_w = 56.f;

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
        const float left_w = std::max(140.f, row_avail_w - zbox_w - 10.f);

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
        ImGui::PushItemWidth(100.f);   // s120: estrecha la banda de tipo (antes 118) — el Z-box de pass height ya no salta de línea; 100 aún cabe "ColorStitch"
        if (ImGui::BeginCombo("##kind", kind_items[sel].c_str())) {
            for (int k = 0; k < 4; ++k)
                if (ImGui::Selectable(kind_items[k].c_str(), k == sel) && k != sel) {
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
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();

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
        } else if (p.kind == K::PathBlend) {
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
            ImGui::SameLine();
            const int em = std::clamp(pbc.ease_mode, 0, 3);
            if (ImGui::SmallButton((ease_names[em] + "##ease").c_str())) {
                PathBlendPassConfig pbe = pro_pb_read(p);
                pbe.ease_mode = (em + 1) % 4;
                pro_pb_write(p, pbe, layer_h);
            }
        }

        // ---- line 2: preview bar (hover wheel: Solid angle / PB Full mid) ----
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
                    } else if (p.kind == K::PathBlend && !is_pb_half) {
                        PathBlendPassConfig pbe = pro_pb_read(p);
                        pbe.mid_end_mm += (wheel > 0.f) ? 0.01f : -0.01f;
                        pro_pb_write(p, pbe, layer_h);
                    }
                }
            }
            pro_pass_preview(dl, q, ImVec2(q.x + left_w, q.y + bar_h), p, penu, fcolors);
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
        dl->AddRectFilled(ImVec2(gmin.x - 3.f, gmin.y - 2.f),
                          ImVec2(gmin.x + left_w + 3.f, gmax.y + 2.f),
                          _box_col, 3.f);
        dl->ChannelsMerge();

        // ---- Z box (right edge): pass height mm / PB floor mm ----
        const float frame_h = ImGui::GetFrameHeight();
        const float block_h = gmax.y - gmin.y;
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(gmin.x + left_w + 8.f,
                                         gmin.y + std::max(0.f, (block_h - frame_h) * 0.5f)));
        ImGui::PushItemWidth(zbox_w - 6.f);
        if (p.kind == K::PathBlend) {
            float fv = pbc.floor_mm;
            if (ImGui::DragFloat("##floor", &fv, 0.005f, 0.01f, (float)layer_h, "%.2f")) {
                PathBlendPassConfig pbe = pro_pb_read(p);
                pbe.floor_mm = fv;
                // keep the ramp alive when floor crosses mid (dialog mirror)
                if (pbe.mid_end_mm <= pbe.floor_mm) pbe.mid_end_mm = pbe.floor_mm + 0.001f;
                pro_pb_write(p, pbe, layer_h);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Ramp floor (mm)").c_str());
        } else if (n == 1) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%.2f", layer_h);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("A single pass fills the whole layer").c_str());
        } else {
            float mm = (float)(p.ratio * layer_h);
            if (ImGui::DragFloat("##mm", &mm, 0.005f, 0.f, (float)layer_h, "%.2f")) {
                // port of the dialog's on_height_edit: set this pass, rescale
                // the siblings so the stack still fills the layer.
                const double newR = std::clamp((double)mm / layer_h, 0.0, 1.0);
                double others_old = 0.0;
                for (int j = 0; j < n; ++j)
                    if (j != i) others_old += std::max(0.0, st.passes[j].ratio);
                p.ratio = newR;
                const double target = 1.0 - newR;
                if (others_old > 1e-6) {
                    const double kf = target / others_old;
                    for (int j = 0; j < n; ++j)
                        if (j != i) st.passes[j].ratio = std::max(0.0, st.passes[j].ratio) * kf;
                } else {
                    const double ev = target / (double)(n - 1);
                    for (int j = 0; j < n; ++j) if (j != i) st.passes[j].ratio = ev;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Pass height (mm) — siblings rescale to fill the layer").c_str());
        }
        ImGui::PopItemWidth();

        // resume the layout below the block
        ImGui::SetCursorScreenPos(ImVec2(gmin.x, gmax.y + 5.f));
        if (thin)
            ImGui::TextColored(ImVec4(0.86f, 0.59f, 0.24f, 1.f), "%s",
                _u8L("⚠ < 0.04 mm — pass is dropped at slice time").c_str());
        ImGui::PopID();
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

static double GLGizmoColorMixPainter_layer_height()
{
    if (auto* o = wxGetApp().preset_bundle->prints.get_edited_preset()
                      .config.option<ConfigOptionFloat>("layer_height"))
        if (o->value > 0.001) return o->value;
    return 0.2;
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

    auto* ac = wxGetApp().app_config;
    std::string key;
    for (int t = 0; t < 4; ++t) {
        key += fcolors[t] + "|";
        key += (ac ? ac->get("neotko_td_" + std::to_string(t + 1)) : "") + "|";
    }
    key += std::to_string(lh) + "|" + std::to_string(m_grad_tool_a)
         + "|" + std::to_string(m_grad_tool_b);
    if (key == m_pal_key) return;   // contexto intacto → caché válida
    m_pal_key = key;

    CS::PredictOptions o;
    o.layer_height = lh;
    m_pal_flat  = CS::build_palette(CS::PaletteKind::Flat,  mats, o);
    // s120: "Mixed approximation" retirada del painter — sus recetas llevaban
    // penu ColorStitch (origen del bug del gradiente). Ya no se construye.

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

    // (s111: el swatch "Active colour" + Save palette viven ahora en el carril
    //  izquierdo — render_left_rail.)
    ImGui::Separator();
}

// NEOTKO_COLORSTITCH_TAG — s118: fwd-decl (recipe_argb se define más abajo, tras
// render_pro_mode_panel, que ahora lo usa para el preview del write-back live).
static uint32_t recipe_argb(const Slic3r::ColorSci::ColorRecipe& r);

// NEOTKO_COLORSTITCH_TAG — Bandeja "pro mode": compone Top/Penu + TD y muestra
// el color resultante en vivo (sandwich_colour_stacked, mismo motor que el
// Sandwich Editor). Produce un ColorRecipe → color activo de pintura (sin pasar
// por el preset). Opción B del revamp; v2 s108: filas estilo SandwichDialog con
// los tres kinds inline (Solid / ColorStitch / PathBlend Half|Full), cajita Z
// editable con auto-rescale y TD en rejilla de 2 columnas al final.
void GLGizmoColorMixPainter::render_pro_mode_panel()
{
    namespace CS = Slic3r::ColorSci;
    // s111 — label corto en el header; la descripción larga pasa a tooltip.
    const bool pro_open = ImGui::CollapsingHeader(_u8L("Pro mode").c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Compose Top / Penu + TD").c_str());
    if (!pro_open)
        return;

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
    draw_zone_editor("##pro_top",  _u8L("Top").c_str(),         m_pro_top,
                     /*allow_disable=*/false, /*penu=*/false, fcolors, nfil, lh, pro_row_w);
    ImGui::Spacing();
    draw_zone_editor("##pro_penu", _u8L("Penultimate").c_str(), m_pro_penu,
                     /*allow_disable=*/true,  /*penu=*/true,  fcolors, nfil, lh, pro_row_w);
    ImGui::Spacing();

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
    auto* ac = wxGetApp().app_config;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "(TD)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Transmission distance").c_str());

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
        if (ImGui::SliderFloat("##td", &td, 0.01f, 10.f, "%.2f")) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.3f", td);
            if (ac) ac->set("neotko_td_" + std::to_string(t + 1), buf);
            m_pal_key.clear();   // fuerza rebuild_palettes_if_stale en el panel de paletas
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
    };
    for (int r = 0; r < td_rows; ++r) {
        td_cell(r);
        const int t2 = td_rows + r;
        if (t2 < nfil) { ImGui::SameLine(col_w + 8.f); td_cell(t2); }
    }

    // --- Color resultante en vivo ---
    float out[3] = {0.f, 0.f, 0.f};
    const float bg[3] = {0.f, 0.f, 0.f};     // fondo negro (= default PredictOptions)
    CS::sandwich_colour_stacked(m_pro_top, m_pro_penu, mats, bg, out);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight() * 1.6f;
        dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h),
                          IM_COL32((int)std::min(255.f, out[0] * 255.f),
                                   (int)std::min(255.f, out[1] * 255.f),
                                   (int)std::min(255.f, out[2] * 255.f), 255));
        dl->AddRect(p, ImVec2(p.x + h, p.y + h), IM_COL32(255, 255, 255, 255));
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_u8L("Result"));
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
            if (p->stack_top_json != tj || p->stack_penu_json != pj) {
                // Reescritura live por id (NO dedup-crear): conserva el id y el
                // nombre; refresca payload del motor desde los stacks editados.
                p->stack_top_json = tj;
                p->stack_penu_json = pj;
                p->preview_argb    = recipe_argb(recipe);
                p->colormix  = {};   // limpiar payload viejo (payload_from_stacks solo añade)
                p->pathblend = {};
                SurfaceEffectProfileManager::payload_from_stacks(recipe.top, recipe.penu, *p);
                refresh_selector_palettes();   // overlay + swatches al día en vivo
                m_preview_dirty = true;
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

    // Nombre del color enlazado (editable) + Pin a la biblioteca.
    if (SurfaceEffectProfile* p = (m_selected_profile_id != 0)
                                      ? mgr.find_mut(m_selected_profile_id) : nullptr) {
        char namebuf[128];
        // NEOTKO_COLORSTITCH_TAG — s137: edit the bare name (no group suffix); on
        // change re-append the profile's OWN group so renaming never moves it.
        std::snprintf(namebuf, sizeof(namebuf), "%s", cs_strip_group(p->name).c_str());
        ImGui::PushItemWidth(160.f);
        if (ImGui::InputText("##cs_name", namebuf, sizeof(namebuf)))
            p->name = cs_with_group(namebuf, cs_parse_group(p->name));
        ImGui::PopItemWidth();
        ImGui::SameLine();
    }
    if (m_imgui->button(_L("Pin to palette")))
        save_active_as_palette();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Keep this colour in the saved palette library "
                                     "(otherwise it is a temporary working colour).").c_str());
    ImGui::Separator();
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

// s111 — fila de herramientas [Select][bucket][Eraser] (toggles mutuamente
// excluyentes). Select intercepta clics para marcar/activar objetos (re-activa el
// picking); bucket pinta; eraser despinta. NEOTKO_COLORSTITCH_TAG.
void GLGizmoColorMixPainter::render_tool_row()
{
    auto tool_toggle = [](const char* label, bool active, const char* tip) -> bool {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        // NEOTKO_COLORSTITCH_TAG — botón de fondo transparente: el texto debe seguir
        // al modo (blanco sobre oscuro / oscuro sobre claro), no forzarse a blanco
        // (era ilegible en light mode). Mismos valores que push_toolbar_style.
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGuiWrapper::is_dark_mode() ? ImVec4(1.f, 1.f, 1.f, 1.f)
                                         : ImVec4(50 / 255.f, 58 / 255.f, 61 / 255.f, 1.f));
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.59f, 0.53f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Border,        ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        }
        const bool clicked = ImGui::Button(label);
        if (active) { ImGui::PopStyleColor(3); ImGui::PopStyleVar(1); }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };

    if (tool_toggle(_u8L("Select").c_str(), m_select_mode,
                    _u8L("Select objects to paint — click them in the scene "
                         "(Shift-click to unmark)").c_str()))
        set_tool_mode(/*select=*/true, /*erase=*/false);
    ImGui::SameLine();
    const std::string bucket =
        boost::nowide::narrow(std::wstring(1, (wchar_t)ImGui::FillButtonIcon));
    if (tool_toggle(bucket.c_str(), !m_select_mode && !m_erase_mode,
                    _u8L("Paint (smart fill)").c_str()))
        set_tool_mode(/*select=*/false, /*erase=*/false);
    ImGui::SameLine();
    if (tool_toggle(_u8L("Eraser").c_str(), !m_select_mode && m_erase_mode,
                    _u8L("Eraser — smart-fill removes paint").c_str()))
        set_tool_mode(/*select=*/false, /*erase=*/true);
    ImGui::SameLine();
    // NEOTKO_COLORSTITCH_TAG — s118: eyedropper. Click sobre un objeto → lee su
    // receta pintada y la enlaza como color activo (+ dump de debug).
    if (tool_toggle(_u8L("Pick").c_str(), m_pick_mode,
                    _u8L("Eyedropper — click a painted object to load its colour "
                         "(and dump what it has painted / its base).").c_str())) {
        m_select_mode = false;
        m_erase_mode  = false;
        m_pick_mode   = true;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
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

void GLGizmoColorMixPainter::render_left_rail(float rail_w, float rail_h)
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

    // Materiales (color + TD) actuales: el swatch Activo y los guardados son
    // mezclas que deben RE-PREDECIRSE en vivo cuando cambian los TD del Pro mode
    // (igual que ya hacen las tiras de paleta), no quedarse con el rgb congelado
    // al seleccionar/guardar. NEOTKO_COLORSTITCH_TAG s111.
    namespace CS = Slic3r::ColorSci;
    CS::Material mats[4];
    std::vector<std::string> fcolors;             // rellenado a >=4 por gizmo_materials
    gizmo_materials(mats, fcolors);
    auto predict_argb = [&](const SurfacePassStack& top,
                            const SurfacePassStack& penu) -> uint32_t {
        float out[3] = {0.f, 0.f, 0.f};
        const float bg[3] = {0.f, 0.f, 0.f};      // fondo negro (= default)
        CS::sandwich_colour_stacked(top, penu, mats, bg, out);
        return 0xFF000000u
             | ((uint32_t)std::min(255.f, out[0] * 255.f) << 16)
             | ((uint32_t)std::min(255.f, out[1] * 255.f) <<  8)
             |  (uint32_t)std::min(255.f, out[2] * 255.f);
    };

    // Padding chico: el rail es estrecho, hay que aprovechar cada píxel de ancho
    // para que los swatches no queden minúsculos.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.f, 3.f));
    ImGui::BeginChild("##cmp_left_rail", ImVec2(rail_w, rail_h), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Lado del swatch Activo = ancho interior real del rail (no rail_w, que aún
    // no descuenta el padding del child).
    const float sw = ImGui::GetContentRegionAvail().x;

    // ---- Active ------------------------------------------------------------
    m_imgui->text(_u8L("Active"));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        bool have = false;
        if (m_has_active_recipe) {
            // Re-predecir en vivo con los TD actuales (no usar m_active_recipe.rgb,
            // que quedó congelado al seleccionar).
            const uint32_t argb = predict_argb(m_active_recipe.top, m_active_recipe.penu);
            dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw),
                              IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
            have = true;
        } else if (const SurfaceEffectProfile* sp_sel = mgr.find(m_selected_profile_id)) {
            // Paleta guardada seleccionada → re-predecir su color-resultado en vivo
            // desde el stack (los TD actuales mandan, no el preview_argb cacheado).
            const SurfacePassStack st_top  = SurfacePassStack::from_json(sp_sel->stack_top_json);
            const SurfacePassStack st_penu = SurfacePassStack::from_json(sp_sel->stack_penu_json);
            const uint32_t argb = predict_argb(st_top, st_penu);
            dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw),
                              IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
            have = true;
        }
        dl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                    have ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 120, 120, 255));
        ImGui::Dummy(ImVec2(sw, sw));
    }
    if (m_has_active_recipe) {
        if (m_imgui->button(_L("Save")))
            save_active_as_palette();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Save palette").c_str());
    }
    // s140 — SAVE ALL: visible solo si queda algún color de trabajo sin guardar.
    // Vuelca todos los efímeros al grupo activo de una vez → "Remove all" queda
    // libre de borrar lo no guardado. NEOTKO_COLORSTITCH_TAG.
    if (has_unsaved_palettes()) {
        if (m_imgui->button(_L("Save all")))
            save_all_palettes();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Save every unsaved working colour into the active palette group").c_str());
    }

    // ---- Profiles (biblioteca de paletas guardadas, columna vertical) ------
    ImGui::Spacing();
    m_imgui->text(m_desc["profiles"]);

    // NEOTKO_COLORSTITCH_TAG — s137b: el selector de grupo se renderiza ahora como
    // fila full-width ENCIMA de las dos columnas (render_group_selector), no aquí:
    // el carril es demasiado estrecho (~2.8 líneas) y clipaba el botón "+".

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
    // Reservar una línea al pie para el indicador de overflow "v".
    const float list_w = ImGui::GetContentRegionAvail().x;   // ancho interior real
    const float list_h = std::max(sw, ImGui::GetContentRegionAvail().y);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.f, 2.f));
    // AlwaysVerticalScrollbar: reservar SIEMPRE el ancho de la scrollbar. Si no,
    // su aparición/desaparición cambia el ancho disponible → los swatches
    // cuadrados cambian de tamaño → cambia la altura total → la scrollbar vuelve
    // a aparecer/desaparecer: oscilación ("fliqueo") cada frame.
    ImGui::BeginChild("##cmp_profile_list", ImVec2(list_w, list_h), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (mgr.size() == 0) {
        ImGui::PushTextWrapPos(0.f);
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), m_desc["no_profiles"]);
        ImGui::PopTextWrapPos();
    } else {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const float psw = ImGui::GetContentRegionAvail().x;   // lado del swatch

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
            if (ImGui::InvisibleButton("##cmp_sw", ImVec2(psw, psw))) {
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
                const uint32_t argb = predict_argb(st_top, st_penu);
                ldl->AddRectFilled(sp, ImVec2(sp.x + psw, sp.y + psw),
                                   IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255));
            }
            ldl->AddRect(sp, ImVec2(sp.x + psw, sp.y + psw),
                         sel ? IM_COL32(255, 255, 255, 255)
                             : (hov ? IM_COL32(210, 210, 210, 255) : IM_COL32(20, 20, 20, 255)),
                         0.f, 0, sel ? 2.5f : 1.f);
            // NEOTKO_COLORSTITCH_TAG — s118: marca ámbar = color de TRABAJO (auto, no
            // guardado) que ocupa un slot; distinguible de las paletas guardadas para
            // borrarlo sin miedo (right-click→Delete).
            if (p.auto_generated)
                ldl->AddRect(ImVec2(sp.x + 2.f, sp.y + 2.f),
                             ImVec2(sp.x + psw - 2.f, sp.y + psw - 2.f),
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
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);   // ChildBorderSize + WindowPadding del profile list
    // (overflow lo señala ya la scrollbar vertical permanente del child.)

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

    ImGui::EndChild();
    ImGui::PopStyleVar();   // WindowPadding del rail
}
// NEOTKO_COLORSTITCH_TAG_END

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

    // ---- Cuerpo en dos columnas (s111 UX) — NEOTKO_COLORSTITCH_TAG ----------
    // Carril izquierdo: swatch Activo arriba + biblioteca Profiles en columna
    // vertical con scroll. Columna derecha: paletas / tools / pro mode /
    // sliders. La altura de ambas se mide el frame anterior (m_panel_col_h).
    // s137b: selector de grupo full-width, encima de las dos columnas.
    render_group_selector();
    ImGui::Separator();

    // s140: la fila full-width del selector de grupo (render_group_selector) hace
    // que la ventana AlwaysAutoResize crezca hasta su ancho, pero el cuerpo
    // (##cmp_main) seguía clavado al viejo window_width estrecho → la zona de
    // grupo quedaba MÁS ANCHA que todo lo de abajo y el angle/Z-box se clipaba.
    // Medimos el ancho real del contenido (= ancho que impone la fila de grupo) y
    // estiramos el cuerpo para llenarlo, descontando el carril y su SameLine.
    const float full_content_w = ImGui::GetContentRegionAvail().x;
    // s140: 2.8→3.4 para que "Save all" quepa sin clipar (la lección de s138). El
    // resto del rail (swatch Active, lista Profiles) escala con sw=ancho interior.
    const float rail_w = ImGui::GetTextLineHeight() * 3.4f;
    const float col_h  = m_panel_col_h > 0.f ? m_panel_col_h : m_imgui->scaled(26.f);
    render_left_rail(rail_w, col_h);
    ImGui::SameLine();
    const float body_w = std::max(window_width + space_size,
                                  full_content_w - rail_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##cmp_main", ImVec2(body_w, col_h), false);

    // ---- Style palettes (PR.2 — ColorSci::build_palette, cached) -----------
    // Tira aditiva por estilo (Mixed/Gradient/Flat). NO sustituye la biblioteca
    // de paletas guardadas del carril izquierdo; el click pintable llega en PR.3.
    render_palette_panel(window_width);

    // (s111: la biblioteca de Profiles vive ahora en el carril izquierdo —
    //  render_left_rail —; aquí sigue el resto del cuerpo en su nuevo orden.)

    // ---- Tool row: select / paint bucket / eraser (toggle) ------------------
    render_tool_row();

    // ---- Pro mode tray (compose Top/Penu + TD inline) -----------------------
    render_pro_mode_panel();

    // ---- Smart-Fill only ----------------------------------------------------
    // NEOTKO_PROFILE_TAG — the ColorMix Painter only paints coplanar top
    // surfaces, so the inherited brush tools (Circle/Sphere/Triangle) were
    // removed. Smart-Fill is the sole tool; pinned unconditionally.
    m_current_tool = ImGui::FillButtonIcon;
    m_cursor_type  = TriangleSelector::CursorType::POINTER;
    m_tool_type    = ToolType::SMART_FILL;

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

    // ---- Clipping plane ----------------------------------------------------
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

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    const float cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, cur_y);
    const float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    ImGui::SameLine();

    if (m_imgui->button(m_desc.at("remove_all"))) {
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
    ImGui::PopStyleVar(2);

    // s111 — cierre de la columna derecha. Memorizar la altura real del
    // contenido para dimensionar ambas columnas el próximo frame (abrir/cerrar
    // Pro mode u otros headers converge en 1 frame).
    m_panel_col_h = ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::EndChild();

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
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
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
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}

} // namespace Slic3r::GUI
// NEOTKO_PROFILE_TAG_END
