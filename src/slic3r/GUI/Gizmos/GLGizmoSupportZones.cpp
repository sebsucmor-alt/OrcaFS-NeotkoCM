// NEOTKO_SUPPORTZONES_TAG_START — s286, F2.5 skeleton. See the header for the design notes.
#include "GLGizmoSupportZones.hpp"

#include "GizmoNeotkoStyle.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Triangulation.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Feature/SupportZones/SupportZoneProbe.hpp"
#include "libslic3r/NeoDebug.hpp"

#include <nlohmann/json.hpp>

#include <imgui/imgui.h>

#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <fstream>
#include <map>
#include <tuple>
#include <set>
#include <unordered_map>
#include <utility>

namespace Slic3r { namespace GUI {

namespace {

// The colours the two picks wear. Teal is the accent of the Neotko gizmos (GizmoNeotkoStyle.hpp)
// and here it means "this is the surface you are about to take"; green means "taken".
// Teal: about to take this one, and the slicer already calls it an overhang.
const ColorRGBA kHoverCol     (0.10f, 0.95f, 0.85f, 0.88f);
// Violet: about to take this one, but it is UNDER the support threshold, so the slicer would not
// have supported it on its own. You are overriding, not confirming. Violet because that is what
// GizmoNeotkoStyle already reserves for informational slope shading, and amber is reserved for
// "something is off" — this is a choice, not a fault.
const ColorRGBA kHoverBelowCol(0.72f, 0.48f, 1.00f, 0.88f);
const ColorRGBA kTargetCol    (0.16f, 0.90f, 0.32f, 0.82f); // taken
// The overhang map. s286b, seen in the hand: it used to wear the same teal as the hover, and with
// every downward face of the part painted in it the cursor had nothing left to say — moving onto a
// face changed almost nothing. So the map steps back to a desaturated slate wash and gives the
// accent up. Context is grey, the subject is teal, and hovering is now unmistakable.
const ColorRGBA kOverhangCol  (0.42f, 0.52f, 0.60f, 0.16f);
// El interruptor del relleno del mapa. En false: iluminar es del hover, una cara cada vez.
const bool kOverhangMapFill = false;
const ColorRGBA kReachCol     (0.35f, 0.62f, 0.95f, 0.22f);
const ColorRGBA kReachBadCol  (1.00f, 0.59f, 0.20f, 0.30f);
// La sombra del pie en el plano del aterrizaje. Mismo azul del preview del pilar para que se lean
// como la misma cosa, pero mucho más sólido: es el dato exacto, no el rango.
const ColorRGBA kFootCol      (0.30f, 0.55f, 0.95f, 0.70f);
// s289 — el LIENZO del pincel: hasta dónde se puede pintar. Flojo a propósito: es el fondo sobre
// el que se pinta, no una marca. Lo que se pinta encima va en kTargetCol y tiene que ganarle.

} // namespace

GLGizmoSupportZones::GLGizmoSupportZones(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoSupportZones::on_init()
{
    m_shortcut_key = 0; // no global shortcut yet
    return true;
}

std::string GLGizmoSupportZones::on_get_name() const
{
    return _u8L("Support Zones");
}

bool GLGizmoSupportZones::on_is_activable() const
{
    // LibreMode-gated like the rest of the Neotko gizmos, and it needs one object selected: every
    // zone is a ModelVolume of a specific object.
    if (wxGetApp().app_config == nullptr || ! wxGetApp().app_config->get_bool("neotko_libre_enabled"))
        return false;
    const Selection &sel = m_parent.get_selection();
    return sel.get_object_idx() >= 0;
}

CommonGizmosDataID GLGizmoSupportZones::on_get_requirements() const
{
    // NEOTKO_SUPPORTZONES_TAG s299d — EL CORTE VUELVE, Y AHORA SÍ ES EL INSTRUMENTO CORRECTO.
    //
    // 🚨 Estuvo aquí en la primera versión, salió, y vuelve por el camino contrario al que se fue.
    // Lo que salió mal entonces fue el FANTASMA —teñir la pieza de translúcida— y era malo por su
    // cuenta: bajo un bloque que ya es translúcido no se distingue qué está delante de qué. Se
    // quitó en s299c y con él se fue también, por error de bulto, la única forma de mirar dentro.
    //
    // 🔑 El corte no tiene ese problema: quita materia en vez de transparentarla, así que lo que
    // queda se ve sólido y sin ambigüedad. Es el mismo `ObjectClipper` que usan el painter de
    // soportes, el de costuras y el de fuzzy skin, o sea el que el usuario ya sabe usar — y es
    // barato, porque lo hace el shader y no un repintado de volúmenes.
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo)
                            | int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoSupportZones::on_set_state()
{
    if (get_state() == On) {
        m_zones_dirty = true;
        ensure_snug_style();
        if (m_show_gaps)
            m_parent.set_support_zone_gaps(true, overhang_normal_z_cut(), m_gap_step_mm);
        apply_see_through();
        apply_overhang_highlight();
    } else if (get_state() == Off) {
        // 🚨 s289 — devolver el arrastre de objetos SIEMPRE al salir, pase lo que pase. Es un flag
        // del canvas, no del gizmo: dejarlo apagado convierte la escena en intocable y el usuario
        // no tiene ni de qué tirar para saber por qué.
        m_parent.enable_moving(true);
        restore_overhang_highlight();
        restore_see_through();
        // ⛔ s299h — AQUÍ SE RESETEABA EL CORTE AL SALIR, Y ERA EL CRASH. La traza de lldb lo señala
        // sin intermediarios:
        //
        //   Plater::remove_selected → Selection::erase → delete_from_model_and_list
        //     → del_subobject_from_object → changed_object → reload_scene
        //     → refresh_on_off_state → activate_gizmo(Undefined) → set_state(Off)
        //     → ESTA LÍNEA → ObjectClipper::set_position_by_ratio → mo->instances[…] 💥
        //
        // `set_position_by_ratio()` lee `mo->instances[active_inst]` (GLGizmosCommon.cpp:384). Este
        // `on_set_state(Off)` se ejecuta EN MEDIO de un borrado, desde `reload_scene()`, cuando el
        // objeto ya se ha ido o se está yendo: el vector de instancias está vacío y el acceso cae en
        // 0x58. Nada que ver con la zona ni con la lista; era tocar el clipper en el peor momento
        // posible.
        //
        // 🔑 Y no hace falta reponer nada: el corte no sobrevive al gizmo por su cuenta. Al cerrarse,
        // `on_get_requirements()` deja de pedir `ObjectClipper` y el pool de datos comunes lo suelta,
        // que es justo el mecanismo para el que existe. Por eso los painters tampoco lo resetean
        // aquí: sólo lo tocan cuando el usuario pulsa «reset direction».
        //
        // La lección, que ya está pagada dos veces en esta sesión: dentro de `on_set_state(Off)` no
        // se puede dar por hecho que el modelo siga entero.
        clear_pick();
        m_forced_snug       = false;
        m_lean_angle_seeded = false;
        // El mapa es del gizmo: no se queda encendido detrás del usuario al salir.
        m_parent.set_support_zone_gaps(false, overhang_normal_z_cut(), m_gap_step_mm);
        m_zones.clear();
        m_selected_zone = -1;
        // 🚨 El índice de edición apunta a un volumen de ESTE objeto. Sobrevivir al cierre haría
        // que al reabrir sobre otro objeto se estuviera "editando" el volumen que ocupe ese hueco.
        m_editing_volume_idx = -1;
        m_edit_lost_surface  = false;
    }
}

void GLGizmoSupportZones::data_changed(bool /*is_serializing*/)
{
    // Fires on selection changes and on volume rebuilds. Everything cached off the model is
    // invalidated here and rebuilt lazily, never per frame.
    m_zones_dirty       = true;
    m_raycaster.reset();
    m_raycaster_obj_idx = -1;
    m_hover_model_facet = m_target_model_facet = -1;
    m_overhang_model_dirty = true;
    m_candidates.clear();
    // 🚨 s288 — y la edición en curso se cae aquí. Esto salta al cambiar de selección y al
    // reconstruirse la lista de volúmenes, o sea justo cuando `m_editing_volume_idx` puede pasar a
    // señalar a otro volumen o a ninguno. Una edición que sobreviva a eso escribiría la malla en el
    // sitio equivocado, que es el fallo más caro que esta feature puede tener.
    if (editing()) {
        m_editing_volume_idx = -1;
        clear_pick();
    }
    m_edit_lost_surface = false;

    // 🚨 s299g — Y TODO LO QUE SE CACHEÓ CONTRA LA MALLA, TAMBIÉN.
    //
    // Esto salta cuando el modelo cambia por debajo, y un undo es exactamente eso. Lo que se quedaba
    // vivo eran las cachés que s299 añadió: el lienzo (índices de TRIÁNGULOS de la malla anterior),
    // su mapa de alturas, la máscara y las marcas del pincel con su cara semilla. Ninguna de ellas
    // tiene forma de saber que la malla ya no es la misma — sus claves son el índice de la cara y la
    // matriz, y las dos pueden coincidir por casualidad con las de la malla nueva.
    //
    // ⚠️ No puedo afirmar que esto sea el crash del undo que él vio; no lo he reproducido. Lo que sí
    // es cierto es que un índice de triángulo cacheado contra una malla que ya no existe es la
    // familia de fallos a la que ese crash pertenece, y aquí es donde se cortan todos de una vez.
    m_region_cache_facet = -1;
    m_region_cache.clear();
    m_region_area_cache.clear();
    m_hm_nx = m_hm_ny = 0;
    m_hm_z.clear();
    m_hm_n.clear();
    invalidate_patch();
    m_mask_cache = ZoneMask();
    m_stamps.clear();
    clear_painted();
    m_brush_seed_facet = -1;
    m_cand_mouse       = Vec2d(-1e9, -1e9);

    // The volume list may have been rebuilt under us, so the ghost is re-applied against the live
    // volumes. apply_see_through() restores first, which is what keeps the saved colours honest.
    if (get_state() == On)
        apply_see_through();
}

// -----------------------------------------------------------------------------
// The zone list
// -----------------------------------------------------------------------------

int GLGizmoSupportZones::current_object_idx() const
{
    return m_parent.get_selection().get_object_idx();
}

const ModelObject *GLGizmoSupportZones::current_object() const
{
    const Model *model = m_parent.get_selection().get_model();
    const int    idx   = current_object_idx();
    if (model == nullptr || idx < 0 || idx >= int(model->objects.size()))
        return nullptr;
    return model->objects[idx];
}

void GLGizmoSupportZones::rebuild_zone_rows()
{
    m_zones.clear();
    m_zones_dirty = false;

    const ModelObject *mo = current_object();
    if (mo == nullptr)
        return;

    // 🔑 The probe's grid step is support_base_pattern_spacing, the pillar resolution that already
    // governs the printed support (F1). What you read here and what gets built come off the same
    // number, which is the whole reason the illumination is worth trusting.
    float step = 2.f;
    {
        const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (cfg.has("support_base_pattern_spacing"))
            step = float(cfg.opt_float("support_base_pattern_spacing"));
        // A per-object override wins, same as it does for the printed support.
        if (mo->config.has("support_base_pattern_spacing"))
            step = float(mo->config.opt_float("support_base_pattern_spacing"));
    }

    size_t priority = 0;
    for (int vi = 0; vi < int(mo->volumes.size()); ++ vi) {
        const ModelVolume *v = mo->volumes[vi];
        if (v == nullptr || ! v->is_support_enforcer())
            continue;
        ZoneRow row;
        row.volume_idx = vi;
        row.priority   = priority ++;
        row.name       = v->name.empty() ? std::string("?") : v->name;

        const SupportZones::ZoneProbe probe = SupportZones::probe_zone(*mo, *v, step);
        row.cells_in_zone = probe.cells_inside_zone;
        row.lit           = probe.lit.size();
        row.sterile       = probe.sterile();
        // s287 — para la tapa del pilar de la tarjeta. La config del volumen sigue siendo el único
        // dueño del dato; esto es una copia para dibujar.
        row.roof_filament = v->config.has("support_interface_filament") ? v->config.opt_int("support_interface_filament") : 0;

        // NEOTKO_SUPPORTZONES_TAG s288 — EL CANDADO, DERIVADO.
        //
        // 🔑 No hay que engancharse a los gizmos de mover, rotar y escalar, ni escuchar eventos, ni
        // marcar nada sucio. Una zona recién creada tiene rotación y escala NEUTRAS por
        // construcción (`write_pillar_into` pone `Transformation()` y luego el `offset`), así que
        // "¿sigue bloqueada?" es comparar. Cualquier cosa que la toque por fuera rompe una de las
        // condiciones y la zona pasa a ser una malla como cualquier otra, que es exactamente el
        // modelo que pidió el dueño: camino de una sola dirección.
        // 🚨 Se lee el `.value` CRUDO, nunca `opt_serialize()`: `ConfigOptionString::serialize()`
        // pasa por `escape_string_cstyle()`, así que devolvería el JSON con las comillas escapadas
        // y el parseo fallaría. El motor lee los blobs de Sandwich igual, por `.value`.
        if (const ConfigOption *opt = v->config.option("neotko_support_zone_gesture"); opt != nullptr)
            if (const auto *sopt = dynamic_cast<const ConfigOptionString *>(opt); sopt != nullptr)
                row.has_gesture = gesture_from_json(sopt->value, row.gesture);
        if (row.has_gesture) {
            const Vec3d rot   = v->get_rotation();
            const Vec3d scale = v->get_scaling_factor();
            const Vec3d mirr  = v->get_mirror();
            // 🚨 EL EPSILON NO ES PEREZA, ES OBLIGATORIO. El `offset` viaja al 3mf como TEXTO y
            // vuelve como texto. Un `==` exacto haría que guardar y reabrir desbloqueara todas las
            // zonas, y el fallo parecería aleatorio — que es el peor fallo posible de esta feature.
            const bool same_place = (v->get_offset() - row.gesture.lock_offset).norm() < 1e-4;
            const bool neutral    = rot.norm() < 1e-6 &&
                                    (scale - Vec3d::Ones()).norm() < 1e-6 &&
                                    (mirr  - Vec3d::Ones()).norm() < 1e-6;
            row.locked = same_place && neutral;
        }

        m_zones.push_back(std::move(row));
    }

    if (m_selected_zone >= int(m_zones.size()))
        m_selected_zone = -1;

    // Una selección pedida por índice de volumen (duplicar), resuelta ahora que las filas existen.
    if (m_select_volume_idx_pending >= 0) {
        for (int i = 0; i < int(m_zones.size()); ++ i)
            if (m_zones[i].volume_idx == m_select_volume_idx_pending) {
                m_selected_zone = i;
                break;
            }
        m_select_volume_idx_pending = -1;
    }

    // 🚨 La lista se reconstruye cuando el modelo cambia (crear, duplicar, borrar), y los índices
    // de volumen se mueven con ella. Repintar aquí es lo que evita que el resaltado se quede
    // señalando a la zona que ocupaba ese hueco ANTES — que es peor que no señalar nada.
    if (get_state() == On)
        apply_see_through();
}

// -----------------------------------------------------------------------------
// Pick #1 — the surface to hold up
// -----------------------------------------------------------------------------

void GLGizmoSupportZones::ensure_raycaster()
{
    const ModelObject *mo  = current_object();
    const int          idx = current_object_idx();
    if (mo == nullptr || mo->instances.empty()) {
        m_raycaster.reset();
        m_raycaster_obj_idx = -1;
        return;
    }

    // Refresh the world transform every call so a moved or rotated object never leaves a stale
    // raycaster (cheap); rebuild the mesh and the tree only when the object itself changes.
    m_world_trafo = mo->instances.front()->get_matrix();
    if (m_raycaster && m_raycaster_obj_idx == idx)
        return;

    m_mesh = mo->raw_mesh();
    m_raycaster.reset(new MeshRaycaster(m_mesh));
    m_raycaster_obj_idx = idx;
    m_face_normals      = its_face_normals(m_mesh.its);
    m_face_neighbors    = its_face_neighbors(m_mesh.its);
    // 🚨 s299 — la malla es OTRA, así que el lienzo cacheado (la región, su proyección y el mapa de
    // alturas) apunta a triángulos que ya no existen. Se tira aquí, que es el único sitio por el
    // que pasa un cambio de malla.
    m_region_cache_facet = -1;
    m_region_cache.clear();
    m_region_area_cache.clear();
    m_hm_nx = m_hm_ny = 0;
    m_hm_z.clear();
    m_hm_n.clear();
    // s300b — y el pintado, que va por índices de TRIÁNGULO de la malla anterior.
    m_painted.clear();
    m_painted_list.clear();
    m_painted_count = 0;
    m_visit_stamp.clear();
    ++ m_stamp_stamp;
    m_hover_model_facet = m_target_model_facet = -1; // force the overlays to rebuild
    m_overhang_model_dirty = true;
    m_candidates.clear();
}

void GLGizmoSupportZones::clear_pick()
{
    m_target_pick_mode  = false;
    m_landing_pick_mode = false;
    m_stump_pick_mode   = false;   // s301 — el tercer modo de picking se apaga con los otros dos
    m_has_target        = false;
    m_has_landing       = false;
    m_landing_locked    = false;
    m_mouse_down        = false;
    m_painting          = false;
    m_extra_stumps.clear();
    m_stamps.clear();
    clear_painted();
    m_brush_seed_facet  = -1;
    invalidate_patch();
    m_preview_dirty     = true;
    m_preview_model.reset();
    m_reach_model.reset();
    m_reach_model_r     = -1.;
    m_target_facet_idx  = -1;
    m_target_model_facet = -1;
    m_hover_model_facet = -1;
    m_have_hover_pos    = false;
    m_candidate_idx     = 0;
    m_candidates.clear();
    m_hover_model.reset();
    m_target_model.reset();
    m_raycaster.reset();
    m_raycaster_obj_idx = -1;
}

void GLGizmoSupportZones::update_candidates(const Vec2d &mouse_pos)
{
    ensure_raycaster();

    // 🔑 s299 — el ratón quieto no cuesta nada. Esta función se llama desde el render, así que sin
    // este corte es una consulta al árbol AABB por frame sólo por tener el cursor encima de la
    // pieza. Medio píxel es más fino que cualquier gesto humano.
    if ((mouse_pos - m_cand_mouse).squaredNorm() < 0.25 && m_cand_trafo.isApprox(m_world_trafo)
        && ! m_candidates.empty())
        return;
    m_cand_mouse = mouse_pos;
    m_cand_trafo = m_world_trafo;

    // Remember which surface the wheel had landed on, so a small mouse move does not silently
    // swap the live candidate under the user's finger.
    const int previous_facet = (m_candidate_idx >= 0 && m_candidate_idx < int(m_candidates.size()))
                                   ? m_candidates[m_candidate_idx].facet_idx
                                   : -1;
    m_candidates.clear();

    if (! m_raycaster)
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3d world_src, world_dir;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, world_src, world_dir);

    // The tree lives in mesh space, so the ray goes there too. The direction is NOT normalised
    // after the transform on purpose: hit_result::distance() is measured in units of `dir`, and
    // keeping the same scaling for every hit is all the ordering below needs.
    const Transform3d inv = m_world_trafo.inverse();
    const Vec3d local_src = inv * world_src;
    const Vec3d local_dir = inv.linear() * world_dir;

    const AABBMesh &tree = m_raycaster->get_aabb_mesh();
    std::vector<AABBMesh::hit_result> hits = tree.query_ray_hits(local_src, local_dir);

    // NEOTKO_SUPPORTZONES_TAG s299d — NO SE PINTA DENTRO DE LA PIEZA.
    //
    // 🚨 Éste era el fallo de raíz de "está pintando zonas de dentro", y se ve claro contándolo: el
    // filtro de abajo se queda con la primera cara que MIRA HACIA ABAJO. Si lo primero que hay bajo
    // el cursor es una pared vertical o una cara de arriba, esa se descarta y el candidato pasa a
    // ser la siguiente que sí mira hacia abajo — que está DETRÁS, o sea dentro de la pieza. En una
    // pieza hueca eso es la cara interior, y ahí es donde se pintaba sin querer.
    //
    // 🔑 La regla es la que usa cualquier painter y es la que el ojo espera: se pinta sobre lo que
    // SE VE. El primer impacto del rayo es la superficie visible; a partir del segundo ya estás
    // mirando a través de materia. Se conservan los candidatos apilados para la rueda, porque una
    // pieza fina tiene dos caras casi pegadas y elegir entre ellas es legítimo, pero sólo hasta
    // donde el rayo sigue dentro del MISMO grosor: `depth_limit` es el primer impacto más un
    // margen, y lo que caiga más allá es otra pared y no se ofrece.
    //
    // 🔑 s299f — FUERA O DENTRO, y lo elige el usuario. Idea suya: "tal vez tengamos que elegir
    // paint outside / paint inside, así no se escaparía el pintado dentro".
    //
    // Es la respuesta correcta porque las dos cosas son legítimas: normalmente pintas la piel que
    // ves, pero en una pieza hueca puede que quieras sujetar de verdad una superficie interior. Lo
    // que no vale es que el modo interior se cuele sin pedirlo, que es lo que pasaba.
    //
    // 🚨 El margen de 3 mm de la primera versión de este corte era justo el escape: una pared de 2
    // mm cabe dentro de él, así que la cara interior seguía siendo elegible. Ahora en modo FUERA no
    // hay margen ninguno — sólo la primera superficie del rayo — y en modo DENTRO no hay límite.
    // Entre esos dos no queda hueco por el que colarse.
    double depth_limit = std::numeric_limits<double>::max();
    if (! m_paint_inside)
        for (const AABBMesh::hit_result &h : hits)
            if (h.is_hit()) {
                // Sólo lo que esté a la misma profundidad que el primer impacto: dos triángulos
                // coplanares de la misma cara sí, la pared de enfrente no.
                depth_limit = h.distance() + 1e-4;
                break;
            }

    const int n_facets = int(m_mesh.its.indices.size());
    for (const AABBMesh::hit_result &h : hits) {
        if (! h.is_hit())
            continue;
        if (h.distance() > depth_limit)
            break;   // ya estamos mirando a través de la pieza: lo de aquí en adelante es interior
        const int f = h.face();
        if (f < 0 || f >= n_facets || f >= int(m_face_normals.size()))
            continue;

        // 🚨 The filter that makes the ambiguity go away: only surfaces looking DOWNWARD are
        // candidates, so the top face of the object cannot be picked even by accident, and a
        // vertical wall (which needs no support) cannot either.
        //
        // This decides a DESTINATION and nothing else. Whether support is needed there is
        // detect_overhangs()'s call, in the backend, and it must stay that way (§8).
        Vec3d world_n = (m_world_trafo.linear() * m_face_normals[f].cast<double>());
        const double nl = world_n.norm();
        if (nl < 1e-9)
            continue;
        world_n /= nl;
        if (world_n.z() > double(DOWN_FACING_MAX_NORMAL_Z))
            continue;

        Candidate c;
        c.facet_idx      = f;
        c.distance       = h.distance();
        c.world_pos      = m_world_trafo * h.position();
        c.world_normal   = world_n;
        c.past_threshold = (world_n.z() <= double(overhang_normal_z_cut()));
        m_candidates.push_back(c);
    }

    std::sort(m_candidates.begin(), m_candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });

    // Keep the wheel's choice if that surface is still under the cursor; otherwise start at the
    // nearest one, which is what the user is looking at.
    m_candidate_idx = 0;
    if (previous_facet >= 0)
        for (int i = 0; i < int(m_candidates.size()); ++ i)
            if (m_candidates[i].facet_idx == previous_facet) {
                m_candidate_idx = i;
                break;
            }
}

const GLGizmoSupportZones::Candidate *GLGizmoSupportZones::live_candidate() const
{
    if (m_candidate_idx < 0 || m_candidate_idx >= int(m_candidates.size()))
        return nullptr;
    return &m_candidates[m_candidate_idx];
}

// NEOTKO_SUPPORTZONES_TAG s289 — EL LIENZO DEL PINCEL
//
// 🔑 Dicho por él, y era el fallo de raíz: "lo máximo que me deja rellenar es lo mismo que la zona
// de selección de surface... entonces la zona elegida tiene que extenderse mucho más para poder
// pintar más allá y dar un feed visual de que eso es lo que ocurre".
//
// El pincel estaba metido dentro de `collect_region()`, que es un flood-fill COPLANAR a 20° del
// semilla. En un plano eso es toda la cara; en una curva es un parche minúsculo, así que pintabas
// y el trazo se cortaba solo. Y la culpa no era del pincel: el límite de 20° existe para que
// PINCHAR una cara te dé esa cara, que es lo correcto para parche/redondo/cuadrado.
//
// Así que el pincel tiene su propia región: conectada y MIRANDO HACIA ABAJO, sin límite de ángulo
// contra la semilla. En un toro eso es toda la panza hasta el ecuador, que es exactamente donde
// tiene sentido pintar. Sigue acotada por lo único que no es negociable: una pared vertical no se
// sujeta, así que no se pinta.
//
// 🚨 Frontera del §8 intacta: esto elige DESTINO (dónde puedes pintar), nunca decide que ahí haga
// falta soporte. Eso sigue siendo `detect_overhangs()`.
// ⚠️ s299b — YA NO ESTÁ EN NINGÚN CAMINO CALIENTE, y no debe volver a estarlo. Esto recorre TODA la
// superficie que mira hacia abajo: en una pieza de millones de triángulos son cientos de miles de
// caras y varios segundos, y unir su proyección en XY es peor todavía. Fue el error de la primera
// versión de s299 y se veía en la mano: el pincel se quedaba pensando y luego marcaba media pieza.
// El pincel ya no tiene región. Queda aquí porque el volcado de diagnóstico lo usa para decir
// cuánta superficie hay, y eso sólo ocurre con el canal encendido.
std::vector<int> GLGizmoSupportZones::collect_paint_region(int facet_idx) const
{
    std::vector<int> region;
    const int n_facets = int(m_mesh.its.indices.size());
    if (facet_idx < 0 || facet_idx >= n_facets ||
        int(m_face_normals.size()) != n_facets ||
        int(m_face_neighbors.size()) != n_facets)
        return region;

    const Matrix3d nrm = m_world_trafo.linear().inverse().transpose();
    auto faces_down = [&](int f) {
        Vec3d n = nrm * m_face_normals[f].cast<double>();
        const double l = n.norm();
        if (l < 1e-12)
            return false;
        n /= l;
        // El mismo número con el que se decide qué se puede PINCHAR, para que el lienzo y los
        // candidatos no puedan discrepar en pantalla.
        return n.z() <= double(DOWN_FACING_MAX_NORMAL_Z);
    };
    if (! faces_down(facet_idx))
        return region;

    const size_t max_facets = 200000;
    std::vector<char> visited(n_facets, 0);
    std::vector<int>  stack { facet_idx };
    region.reserve(4096);
    visited[facet_idx] = 1;
    while (! stack.empty() && region.size() < max_facets) {
        const int f = stack.back();
        stack.pop_back();
        region.push_back(f);
        for (int e = 0; e < 3; ++ e) {
            const int nb = m_face_neighbors[f][e];
            if (nb < 0 || nb >= n_facets || visited[nb])
                continue;
            if (! faces_down(nb))
                continue;
            visited[nb] = 1;
            stack.push_back(nb);
        }
    }
    return region;
}

std::vector<int> GLGizmoSupportZones::collect_region(int facet_idx) const
{
    std::vector<int> region;
    const int n_facets = int(m_mesh.its.indices.size());
    if (facet_idx < 0 || facet_idx >= n_facets ||
        int(m_face_normals.size()) != n_facets ||
        int(m_face_neighbors.size()) != n_facets)
        return region;

    // Grow the connected region of facets whose normal stays within a small angle of the seed: a
    // flat overhang fills completely, a curved one yields a patch around the cursor. This is the
    // fork's own flood-fill (same as GLGizmoAlignStack's), which is why this gizmo does not need
    // to be a painter to select a surface.
    const float  cos_thresh = 0.94f; // ~20 degrees
    const size_t max_facets = 40000;
    const Vec3f  seed_n     = m_face_normals[facet_idx].normalized();

    std::vector<char> visited(n_facets, 0);
    std::vector<int>  stack { facet_idx };
    region.reserve(256);
    visited[facet_idx] = 1;
    while (! stack.empty() && region.size() < max_facets) {
        const int f = stack.back();
        stack.pop_back();
        region.push_back(f);
        for (int e = 0; e < 3; ++ e) {
            const int nb = m_face_neighbors[f][e];
            if (nb < 0 || nb >= n_facets || visited[nb])
                continue;
            // 🔑 s289 — VALOR ABSOLUTO, y no es laxitud. El propio render de este gizmo apaga el
            // culling porque "raw-mesh winding varies": en una malla con winding mixto el vecino
            // coplanar tiene la normal AL REVÉS, así que el `>=` de antes lo dejaba fuera y el
            // parche salía con agujeros — agujeros que luego se convertían en bucles de borde y en
            // paredes por dentro del pilar. Se acepta: la proyección a XY de `region_for()` le da
            // la vuelta a los que hagan falta, así que el lienzo sale orientado a una sola cara.
            if (std::abs(m_face_normals[nb].normalized().dot(seed_n)) >= cos_thresh) {
                visited[nb] = 1;
                stack.push_back(nb);
            }
        }
    }
    return region;
}

void GLGizmoSupportZones::build_face_model(GLModel &model, int facet_idx, const Vec2d &centre_xy, const ColorRGBA &col)
{
    model.reset();
    // 🔑 Lo que se ENCIENDE es lo que se va a tomar, forma incluida. Si el resaltado enseñara el
    // parche entero y el pilar saliera recortado, el gizmo estaría mintiendo en el sitio donde más
    // se mira.
    // ✅ s299 — y ahora es literalmente la misma `ZoneMask` que va a ser el techo del sólido: la
    // misma área de Clipper y el mismo mapa de alturas. No hay forma de que discrepen porque no hay
    // dos geometrías, hay una.
    // 🔑 s300 — CON EL PINCEL SE ENSEÑAN LOS TRIÁNGULOS, no la huella proyectada.
    //
    // Es la diferencia que se ve en la mano: lo que marcas está SOBRE la superficie, así que
    // enseñarlo proyectado y vuelto a triangular era enseñar otra cosa —y en un arco que rodea un
    // agujero, algo que no se parecía en nada. Aquí se dibuja lo que de verdad está marcado.
    if (m_foot_shape == FootShape::Brush && ! m_painted_list.empty()) {
        const float lift = 0.10f;   // por la normal de cada cara, contra el z-fighting
        indexed_triangle_set its;
        its.vertices.reserve(m_painted_list.size() * 3);
        its.indices.reserve(m_painted_list.size());
        int base = 0;
        // s300b — por la LISTA de pintados, nunca por la malla: son cientos, y esto se rehace cada
        // vez que el trazo crece.
        for (int f : m_painted_list) {
            if (f < 0 || f >= int(m_mesh.its.indices.size()))
                continue;
            const Vec3i32 tri = m_mesh.its.indices[f];
            Vec3f n = m_face_normals[f];
            const float nl = n.norm();
            if (nl < 1e-6f)
                continue;
            n /= nl;
            its.vertices.push_back(m_mesh.its.vertices[tri[0]] + n * lift);
            its.vertices.push_back(m_mesh.its.vertices[tri[1]] + n * lift);
            its.vertices.push_back(m_mesh.its.vertices[tri[2]] + n * lift);
            its.indices.emplace_back(base, base + 1, base + 2);
            base += 3;
        }
        if (! its.indices.empty()) {
            model.init_from(its);
            model.set_color(col);
        }
        return;
    }

    const ZoneMask *mk = mask(facet_idx, centre_xy);
    if (mk == nullptr || mk->area.empty())
        return;

    // 🚨 La máscara está en MUNDO y estos overlays se dibujan con la matriz del objeto puesta en el
    // shader (`view_model_matrix * m_world_trafo`), así que hay que volver. Dibujarlos en mundo con
    // esa matriz puesta los colocaría el doble de lejos, que es el bug clásico de esta familia.
    const Transform3d inv  = m_world_trafo.inverse();
    const double      lift = 0.10;   // por la normal, contra el z-fighting

    indexed_triangle_set its;
    for (const ExPolygon &ep : mk->area) {
        const Points               pts = to_points(ep);
        const std::vector<Vec3i32> idx = Triangulation::triangulate(ep);
        if (idx.empty() || pts.empty())
            continue;
        const int base = int(its.vertices.size());
        its.vertices.reserve(its.vertices.size() + pts.size());
        for (const Point &q : pts) {
            const Vec2d  p2(unscaled<double>(q.x()), unscaled<double>(q.y()));
            const Vec3d  n = n_at(*mk, p2);
            const Vec3d  w = inv * (Vec3d(p2.x(), p2.y(), z_at(*mk, p2)) + n * lift);
            its.vertices.emplace_back(float(w.x()), float(w.y()), float(w.z()));
        }
        for (const Vec3i32 &t : idx) {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 ||
                t[0] >= int(pts.size()) || t[1] >= int(pts.size()) || t[2] >= int(pts.size()))
                continue;
            its.indices.emplace_back(base + t[0], base + t[1], base + t[2]);
        }
    }
    if (its.indices.empty())
        return;
    model.init_from(its);
    model.set_color(col);
}

void GLGizmoSupportZones::render_pick_overlays()
{
    // 🚨 The overhang map is drawn whenever the gizmo is open, not only while picking: it is the
    // thing that tells you WHERE to pick, so gating it behind having already picked would be
    // backwards.
    if (! m_target_pick_mode && ! m_has_target && ! m_show_overhangs)
        return;

    if (m_target_pick_mode && m_have_hover_pos)
        update_candidates(m_hover_mouse_pos);

    const Candidate *live       = m_target_pick_mode ? live_candidate() : nullptr;
    const int        hover_face = (live != nullptr) ? live->facet_idx : -1;

    const bool hover_past = (live != nullptr) && live->past_threshold;
    // 🚨 Con una huella recortada el resaltado depende de DÓNDE está el cursor, no sólo de qué cara
    // toca: moverse dentro de la misma cara mueve el recorte. Se reconstruye por distancia y no
    // por frame porque debajo hay un flood-fill de la malla, y medio milímetro es más fino que lo
    // que el ojo distingue a cualquier zoom útil.
    const Vec2d hover_c = (live != nullptr) ? Vec2d(live->world_pos.x(), live->world_pos.y()) : Vec2d(0., 0.);
    // Con el pincel el centro no manda (el recorte son las marcas), así que moverse por la cara no
    // tiene por qué rehacer nada.
    const bool  centre_moved = (m_foot_shape == FootShape::Round || m_foot_shape == FootShape::Square) &&
                               ((hover_c - m_hover_model_centre).norm() > 0.5);
    if (hover_face != m_hover_model_facet || hover_past != m_hover_model_past || centre_moved ||
        m_foot_shape != m_hover_model_shape || m_foot_size_mm != m_hover_model_size ||
        m_stamps.size() != m_hover_model_stamps) {
        m_hover_model_stamps = m_stamps.size();
        m_hover_model_centre = hover_c;
        m_hover_model_shape  = m_foot_shape;
        m_hover_model_size   = m_foot_size_mm;
        if (hover_face >= 0)
            build_face_model(m_hover_model, hover_face,
                             (live != nullptr) ? Vec2d(live->world_pos.x(), live->world_pos.y()) : Vec2d(0., 0.),
                             hover_past ? kHoverCol : kHoverBelowCol);
        else
            m_hover_model.reset();
        m_hover_model_facet = hover_face;
        m_hover_model_past  = hover_past;
    }
    if (m_target_facet_idx != m_target_model_facet ||
        m_foot_shape != m_target_model_shape || m_foot_size_mm != m_target_model_size ||
        m_stamps.size() != m_target_model_stamps) {
        m_target_model_stamps = m_stamps.size();
        m_target_model_shape = m_foot_shape;
        m_target_model_size  = m_foot_size_mm;
        if (m_target_facet_idx >= 0)
            build_face_model(m_target_model, m_target_facet_idx,
                             Vec2d(m_target_world_pos.x(), m_target_world_pos.y()), kTargetCol);
        else
            m_target_model.reset();
        m_target_model_facet = m_target_facet_idx;
    }

    // ⛔ s299b — AQUÍ SE DIBUJABA EL LIENZO DEL PINCEL, y se ha quitado, no apagado.
    //
    // El lienzo existía para enseñar hasta dónde te dejaba pintar el flood-fill. El pincel ya no
    // tiene flood-fill: lo que pintas es la huella, así que no hay límite que enseñar y el dibujo
    // se quedaba sin significado.
    //
    // Y de paso se lleva por delante lo que el dueño veía en pantalla: en una pieza de millones de
    // triángulos ese modelo era la mancha enorme que aparecía tras la espera, porque era la
    // superficie entera que miraba hacia abajo.

    if (m_overhang_model_dirty)
        build_overhang_model();

    if (! m_hover_model.is_initialized() && ! m_target_model.is_initialized()
        && ! m_overhang_model.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glDisable(GL_CULL_FACE)); // raw-mesh winding varies; show both sides
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * m_world_trafo);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // 🚨 X-ray, in two passes, and it is not decoration.
    // s286, first build in the hand: the marked surface lit up and could not be SEEN. The patch it
    // describes is an underside, so from any useful camera angle there is a wall of the part in
    // front of it — and the part writes depth even while translucent, so a plain depth-tested
    // overlay is thrown away exactly where it matters. From above it survived as a single line.
    //
    // This is the same lesson F1 paid for in s284 (a translucent volume still writes depth and hid
    // the very markers that explained it) arriving from the other side.
    //
    // Pass 1, depth OFF and faint: the patch is always there, wherever it is.
    // Pass 2, depth ON and solid: where it really is visible it reads bright.
    // Together they say both "this is the surface" and "it is behind something".
    auto draw_both = [this](float alpha_scale) {
        // El mapa de voladizos ya no se rellena (build_overhang_model), así que aquí no hay nada
        // debajo: el único sujeto es la cara bajo el cursor y la cara tomada.
        if (m_target_model.is_initialized()) {
            ColorRGBA c = kTargetCol; c[3] *= alpha_scale;
            m_target_model.set_color(c);
            m_target_model.render();
        }
        if (m_hover_model.is_initialized()) {
            ColorRGBA c = m_hover_model_past ? kHoverCol : kHoverBelowCol;
            c[3] *= alpha_scale;
            m_hover_model.set_color(c);
            m_hover_model.render();
        }
    };

    glsafe(::glDisable(GL_DEPTH_TEST));
    draw_both(0.45f);
    glsafe(::glEnable(GL_DEPTH_TEST));
    draw_both(1.0f);

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoSupportZones::on_render()
{
    // 🔑 s289, dicho por él: "en cuanto hago drag muevo el objeto, seguramente por eso el Painter
    // clásico de Orca bloquea todo el mundo". Exacto, y el fork ya tenía el interruptor: es el
    // mismo `enable_moving(false)` que usa SurfaceDrag para arrastrar un texto sobre una cara.
    //
    // 🚨 Y no basta con consumir el evento. `GLCanvas3D::on_mouse()` sólo sale antes de armar el
    // arrastre si el gizmo devuelve `true` en ESE LeftDown, y la pincelada devuelve `true` sólo
    // cuando el rayo encuentra superficie: un pixel de más en el borde de la pieza y el LeftDown se
    // le escapa al gizmo, arma el arrastre, y a partir de ahí el objeto se mueve durante todo el
    // trazo. Apagar el arrastre mientras hay un paso armado lo cierra por el lado del canvas, que
    // es donde de verdad se decide.
    //
    // Se hace por frame a propósito: los modos cambian desde el panel, desde el teclado y desde el
    // propio ratón, y un solo sitio que lo sincroniza no se puede olvidar de ninguno.
    m_parent.enable_moving(! (m_target_pick_mode || m_landing_pick_mode || m_painting));

    // Re-assert the shading the way GLGizmoFdmSupports does: anything else in the app may have
    // turned it off between frames, and it silently going away mid-session reads as a bug.
    if (m_show_overhangs && ! m_parent.is_using_slope()) {
        m_parent.set_slope_normal_angle(90.f - m_overhang_threshold_deg);
        m_parent.use_slope(true);
    }
    render_pick_overlays();
    render_reach();
    render_preview();

    // NEOTKO_SUPPORTZONES_TAG s299f — LA TAPA DEL CORTE.
    //
    // 🚨 Esto es lo que faltaba para que el corte se lea como un corte. El plano ya recortaba TODOS
    // los volúmenes —el canvas se lo pasa a `m_volumes` sin distinguir objeto de soporte— pero sin
    // esta llamada la pieza cortada se ve hueca por dentro, como una cáscara, y el soporte macizo
    // que hay detrás sí parece cortado. De ahí "sólo corta los soportes, no el objeto": los dos
    // estaban cortados, sólo que uno enseñaba su sección y el otro no.
    //
    // Es la misma línea que GLGizmoFdmSupports y GLGizmoSeam tienen en su on_render.
    if (m_c != nullptr && m_c->object_clipper() != nullptr)
        m_c->object_clipper()->render_cut();
}

bool GLGizmoSupportZones::resolve_landing(const Vec2d &mouse_pos, Vec3d &out_pos, bool &out_on_bed)
{
    ensure_raycaster();
    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3d world_src, world_dir;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, world_src, world_dir);

    // First look for a shelf of the part itself: an UPWARD-facing surface that sits below the
    // target. That is the ordinary "land it on the crossbar" case, and the backend already knows
    // what to do with it (support_bottom_z_distance, Slicing.cpp:120).
    if (m_raycaster) {
        const Transform3d inv  = m_world_trafo.inverse();
        const AABBMesh   &tree = m_raycaster->get_aabb_mesh();
        std::vector<AABBMesh::hit_result> hits = tree.query_ray_hits(inv * world_src, inv.linear() * world_dir);
        const int n_facets = int(m_mesh.its.indices.size());
        double best_dist = std::numeric_limits<double>::max();
        bool   found     = false;
        Vec3d  best_pos { Vec3d::Zero() };
        for (const AABBMesh::hit_result &h : hits) {
            if (! h.is_hit())
                continue;
            const int f = h.face();
            if (f < 0 || f >= n_facets || f >= int(m_face_normals.size()))
                continue;
            Vec3d wn = m_world_trafo.linear() * m_face_normals[f].cast<double>();
            const double nl = wn.norm();
            if (nl < 1e-9)
                continue;
            wn /= nl;
            if (wn.z() < - double(DOWN_FACING_MAX_NORMAL_Z))
                continue; // not facing up
            const Vec3d wp = m_world_trafo * h.position();
            if (wp.z() >= m_target_world_pos.z() - 0.2)
                continue; // not below the surface we are holding up
            if (h.distance() < best_dist) {
                best_dist = h.distance();
                best_pos  = wp;
                found     = true;
            }
        }
        if (found) {
            out_pos    = best_pos;
            out_on_bed = false;   // §4.2-bis Stop
            return true;
        }
    }

    // Nothing of the part under the cursor: the bed. Plain ray/plane at z = 0.
    if (std::abs(world_dir.z()) < 1e-9)
        return false;
    const double t = - world_src.z() / world_dir.z();
    if (t <= 0.)
        return false; // the bed is behind the camera
    out_pos    = world_src + t * world_dir;
    out_pos.z() = 0.;
    out_on_bed = true;            // §4.2-bis Straighten
    return true;
}

// NEOTKO_SUPPORTZONES_TAG s300g — EL ATERRIZAJE NACE A PLOMO.
//
// 🔑 Medido en s300g sobre el `.obj` del pilar recién construido, antes de rebanar nada: cuatro
// anillos, los dos de abajo con centro (139.3, 93.6) y el tercero en (129.9, 133.1). El pilar se iba
// 40.6 mm de lado en 32.9 mm de altura — razón 1.234, y `tan(51°) = 1.2349`, o sea el ángulo clavado.
// Ese viaje es el que atravesaba la pieza, y de ahí salían los "techos dentro del muro".
//
// 🔑 La causa no era el pincel ni el motor: era que el gesto EXIGE un segundo clic, y ese clic fija
// `shift = landing − c_top` (ver `build_pillar_mesh`). Pintabas una zona de 23 × 16 mm y el pilar se
// iba a donde hubieras dejado el ratón. Dicho por el dueño: "yo lo que quiero es pintar el área que
// se cree el soporte y que se genere bien".
//
// ⚠️ Y el mando del ángulo no ayudaba a entenderlo, porque no decide CUÁNTO se viaja —eso lo fija el
// aterrizaje— sino a qué altura empieza el viaje (`z_knee = lean_top - offset_d/tan_a`). Subirlo
// acerca la rodilla al techo y hace el salto MÁS brusco, que es lo contrario de lo que promete.
//
// Así que el aterrizaje de salida es el de debajo: `shift = 0` ⇒ sin rodilla ⇒ prisma recto ⇒ el
// pilar ES la huella pintada, extruida hacia abajo. Mover el aterrizaje sigue estando entero
// (`m_landing_pick_mode` y el botón "Move it again"), pero ahora es un acto deliberado y no un paso
// obligatorio del gesto. El §4-bis.1 del plan ya contaba con este caso: "con C1 == C0 degenera en
// prisma recto, v = 0".
bool GLGizmoSupportZones::resolve_landing_plumb(Vec3d &out_pos, bool &out_on_bed)
{
    ensure_raycaster();
    if (! m_has_target)
        return false;

    // El plomo se mide desde el MISMO centroide que usa el alcance, el aviso de travesía y la
    // construcción del sólido. Calcularlo aquí a mano sería tener dos centros que se separan sin
    // avisar, que es exactamente el fallo que este fichero ya documenta en `build_pillar_mesh`.
    const std::vector<Vec2d> foot = target_footprint_world();
    Vec2d c { 0., 0. };
    if (foot.size() >= 3) {
        for (const Vec2d &p : foot)
            c += p;
        c /= double(foot.size());
    } else {
        // Sin huella todavía (el sólido aún no se ha levantado): el punto señalado sirve igual.
        c = Vec2d(m_target_world_pos.x(), m_target_world_pos.y());
    }

    const Vec3d src(c.x(), c.y(), m_target_world_pos.z());
    const Vec3d dir(0., 0., -1.);

    // Una repisa de la propia pieza, igual que en `resolve_landing()`: mirando hacia arriba y por
    // debajo de la superficie que se sujeta. El backend ya sabe qué hacer con eso
    // (`support_bottom_z_distance`, Slicing.cpp:120).
    if (m_raycaster) {
        const Transform3d inv  = m_world_trafo.inverse();
        const AABBMesh   &tree = m_raycaster->get_aabb_mesh();
        const std::vector<AABBMesh::hit_result> hits = tree.query_ray_hits(inv * src, inv.linear() * dir);
        const int n_facets = int(m_mesh.its.indices.size());
        double best_dist = std::numeric_limits<double>::max();
        bool   found     = false;
        Vec3d  best_pos { Vec3d::Zero() };
        for (const AABBMesh::hit_result &h : hits) {
            if (! h.is_hit())
                continue;
            const int f = h.face();
            if (f < 0 || f >= n_facets || f >= int(m_face_normals.size()))
                continue;
            Vec3d wn = m_world_trafo.linear() * m_face_normals[f].cast<double>();
            const double nl = wn.norm();
            if (nl < 1e-9)
                continue;
            wn /= nl;
            if (wn.z() < - double(DOWN_FACING_MAX_NORMAL_Z))
                continue; // no mira hacia arriba
            const Vec3d wp = m_world_trafo * h.position();
            if (wp.z() >= m_target_world_pos.z() - 0.2)
                continue; // no está por debajo de lo que se sujeta
            if (h.distance() < best_dist) {
                best_dist = h.distance();
                best_pos  = wp;
                found     = true;
            }
        }
        if (found) {
            out_pos    = best_pos;
            out_on_bed = false;   // §4.2-bis Stop
            return true;
        }
    }

    // Nada debajo: la cama, en la misma vertical.
    out_pos    = Vec3d(c.x(), c.y(), 0.);
    out_on_bed = true;            // §4.2-bis Straighten
    return true;
}

bool GLGizmoSupportZones::on_mouse(const wxMouseEvent &mouse_event)
{
    const bool picking = m_target_pick_mode || (m_landing_pick_mode && m_has_target)
                      || (m_stump_pick_mode && m_has_target);
    if (! picking) {
        m_mouse_down = false;
        return false;
    }

    const Vec2d mpos(mouse_event.GetX(), mouse_event.GetY());

    // 🚨 The DOWN is only remembered, never consumed, so a left drag still orbits the camera.
    // Everything this gizmo does happens on the UP, and the UP is always consumed while picking —
    // which is what stops a click on empty space from reaching the canvas, deselecting the object
    // and closing the gizmo underneath the user.
    if (mouse_event.LeftDown()) {
        m_mouse_down_pos = mpos;
        m_mouse_down     = true;
        // s289 — el pincel es la ÚNICA excepción a la regla de arriba, y sólo cuando el arrastre
        // empieza SOBRE la superficie. Empezado en el vacío sigue siendo una órbita de cámara, que
        // es lo mismo que hacen los pintores de Orca y por eso no descoloca a nadie.
        if (m_target_pick_mode && m_foot_shape == FootShape::Brush) {
            m_paint_erase = mouse_event.ShiftDown();
            // Cada botón abajo abre un trazo nuevo: lo que se une con cápsulas es lo de DENTRO de
            // un trazo, nunca el salto de un sitio a otro con el botón levantado.
            if (! m_paint_erase)
            if (paint_at(mpos, m_paint_erase)) {
                m_painting = true;
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
                return true;   // consumido: esto es una pincelada, no una órbita
            }
        }
        return false;
    }

    // La pincelada en curso. Se sale por aquí antes que el "un arrastre es una órbita" de más
    // abajo, que es lo que hace que arrastrar pinte en vez de girar la cama.
    if (m_painting && (mouse_event.Dragging() || mouse_event.Moving())) {
        m_hover_mouse_pos = mpos;
        m_have_hover_pos  = true;
        paint_at(mpos, m_paint_erase);
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }
    if (m_painting && mouse_event.LeftUp()) {
        m_painting   = false;
        m_mouse_down = false;
        // s299 — el trazo ha terminado: AHORA se levanta el sólido, una sola vez.
        m_preview_dirty = true;
        // NEOTKO_SUPPORTZONES_TAG s300g — y el plomo se recentra sobre la huella ya terminada.
        //
        // Cada pincelada mueve el centroide, así que el aterrizaje sembrado con el primer toque se
        // queda corto en cuanto pintas de verdad. Sólo si el aterrizaje NO está echado el pestillo:
        // si el usuario lo puso a mano, mandar él.
        if (m_has_target && ! m_landing_locked) {
            Vec3d plumb_pos;
            bool  plumb_on_bed = true;
            if (resolve_landing_plumb(plumb_pos, plumb_on_bed)) {
                m_landing_world_pos = plumb_pos;
                m_landing_on_bed    = plumb_on_bed;
                m_has_landing       = true;
            }
        }
        // Con el canal encendido, cada trazo deja su radiografía. Es la respuesta a "empieza a
        // pintar y me marca zona verde, pero desaparece": el volcado dice cuántas marcas hay,
        // cuánto lienzo había y qué parche ha salido de las dos cosas.
        dump_geometry("brush stroke", nullptr);
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }

    if (mouse_event.Moving() || mouse_event.Dragging()) {
        m_hover_mouse_pos = mpos;
        m_have_hover_pos  = true;
        // Live landing: the pillar follows the cursor until the click latches it, so where it will
        // fall is something you SEE before you commit, not something you find out afterwards.
        if (m_landing_pick_mode && m_has_target && ! m_landing_locked && ! mouse_event.Dragging()) {
            Vec3d pos;
            bool  on_bed = true;
            if (resolve_landing(mpos, pos, on_bed)) {
                m_landing_world_pos = pos;
                m_landing_on_bed    = on_bed;
                m_has_landing       = true;
                m_preview_dirty     = true;
            }
        }
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return false;
    }

    if (mouse_event.LeftUp()) {
        const bool was_down = m_mouse_down;
        m_mouse_down = false;
        // A left drag is a camera orbit, not a pick. Let it through untouched.
        if (was_down && (mpos - m_mouse_down_pos).norm() > CLICK_SLOP_PX)
            return false;

        if (m_target_pick_mode) {
            update_candidates(mpos);
            const Candidate *live = live_candidate();
            if (live != nullptr) {
                m_has_target          = true;
                m_target_facet_idx    = live->facet_idx;
                m_target_world_pos    = live->world_pos;
                m_target_world_normal = live->world_normal;
                m_preview_dirty       = true;
                // El ángulo se siembra UNA vez por sesión del gizmo, no en cada parche: si el
                // usuario lo ha movido, hacer tres pilares seguidos no debe devolverlo al valor de
                // fábrica a sus espaldas. 45 es el que él esperaba; el techo suele estar más alto.
                if (! m_lean_angle_seeded) {
                    m_lean_angle_deg    = float(SupportZones::SUPPORT_ZONE_DEFAULT_LEAN_DEG);
                    m_lean_angle_seeded = true;
                }
                // NEOTKO_SUPPORTZONES_TAG s300g — EL PASO 2 DEJA DE SER OBLIGATORIO.
                //
                // Antes esto encendía `m_landing_pick_mode` y ponía `m_has_landing = false`: el
                // pilar no existía hasta que movías el ratón, y entonces el aterrizaje SEGUÍA al
                // cursor. Por eso acababa a 40 mm de lo señalado sin que nadie lo hubiera pedido.
                //
                // 🚨 Y no basta con sembrar el plomo dejando el modo encendido: mientras
                // `m_landing_pick_mode && ! m_landing_locked`, el `Moving()` de arriba lo vuelve a
                // llevar tras el cursor en el primer píxel que muevas, y el arreglo se deshace solo.
                // Así que el plomo entra con el pestillo ECHADO, igual que si lo hubieras puesto tú:
                // el panel enseña "Move it again", que es la puerta de siempre para moverlo.
                m_target_pick_mode    = false;
                Vec3d plumb_pos;
                bool  plumb_on_bed = true;
                m_has_landing = resolve_landing_plumb(plumb_pos, plumb_on_bed);
                if (m_has_landing) {
                    m_landing_world_pos = plumb_pos;
                    m_landing_on_bed    = plumb_on_bed;
                    m_landing_pick_mode = false;
                    m_landing_locked    = true;
                } else {
                    // Sin plomo resoluble (raro: siempre queda la cama) se cae al gesto de antes.
                    m_landing_pick_mode = true;
                    m_landing_locked    = false;
                }
            }
        } else if (m_stump_pick_mode && m_has_target) {
            // NEOTKO_SUPPORTZONES_TAG s301 — PLANTAR (O ARRANCAR) UN TOCÓN.
            //
            // 🔑 Se reutiliza `resolve_landing()` entero: ya sabe resolver «una repisa de la pieza
            // que mire hacia arriba, y si no la cama», que es exactamente dónde puede caer un tocón.
            // Un segundo resolvedor con la misma regla es un segundo sitio donde se separan.
            //
            // Un clic DENTRO de un tocón ya plantado lo arranca. Es el gesto que uno espera de una
            // herramienta de plantar, y ahorra un botón de borrar por tocón en el panel.
            Vec3d pos;
            bool  on_bed = true;
            if (resolve_landing(mpos, pos, on_bed)) {
                const double r = std::max(0.5, 0.5 * double(m_stump_size_mm));
                bool         removed = false;
                for (size_t i = 0; i < m_extra_stumps.size(); ++ i)
                    if ((Vec2d(m_extra_stumps[i].p.x(), m_extra_stumps[i].p.y())
                         - Vec2d(pos.x(), pos.y())).norm() < r) {
                        m_extra_stumps.erase(m_extra_stumps.begin() + i);
                        removed = true;
                        break;
                    }
                if (! removed)
                    m_extra_stumps.push_back(StumpSpot{ pos, on_bed });
                m_preview_dirty         = true;
                m_footprint_model_dirty = true;
            }
        } else if (m_landing_pick_mode && m_has_target) {
            Vec3d pos;
            bool  on_bed = true;
            if (resolve_landing(mpos, pos, on_bed)) {
                m_landing_world_pos = pos;
                m_landing_on_bed    = on_bed;
                m_has_landing       = true;
                m_landing_locked    = true;   // stop following the cursor
                m_preview_dirty     = true;
            }
        }
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;  // consumed whatever happened: never let this reach the canvas
    }

    // The wheel cycles the stacked downward surfaces under the cursor instead of zooming, but only
    // while there is actually something to cycle — otherwise it would steal the zoom for nothing.
    if (m_target_pick_mode && mouse_event.GetWheelRotation() != 0 && m_candidates.size() > 1) {
        const int n = int(m_candidates.size());
        m_candidate_idx += (mouse_event.GetWheelRotation() > 0) ? -1 : 1;
        m_candidate_idx = (m_candidate_idx % n + n) % n;
        m_hover_model_facet = -1; // force the overlay to rebuild for the new candidate
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Pick #2 and the pillar
// -----------------------------------------------------------------------------

// NEOTKO_SUPPORTZONES_TAG s299 — LA HUELLA, UNA SOLA VEZ Y EN 2D
// -----------------------------------------------------------------------------
// La huella tuvo primero DOS implementaciones que no podían coincidir (s286b), luego una sola pero
// atada a la teselación de la malla (s289), y ahora una sola que no depende de la malla en
// absoluto: `build_mask()` produce un `ExPolygons` de Clipper más un mapa de alturas para el techo.
// El contorno del panel es esa área, los anillos del sólido son offsets de esa área, y el resaltado
// es su triangulación. El porqué de cada pieza vive con la declaración de `ZoneMask` en la
// cabecera.

// 🚨 Nada de `M_PI`: no es estándar y en MSVC hace falta `_USE_MATH_DEFINES` antes de <cmath>.
// Es una de las trampas que ya costó una sesión en este árbol.
// 🚨 Y va a nivel de fichero, NO dentro de la función: MSVC exige capturar una constante local
// dentro de una lambda sin captura por defecto (C3493), mientras que clang/gcc la dejan pasar.
static constexpr double PI_D = 3.14159265358979323846;

// NEOTKO_SUPPORTZONES_TAG s299 — EL LIENZO, CACHEADO POR OBJETO
//
// 🔑 El flood-fill no depende de dónde esté el ratón: una vez elegida la semilla, la región es
// siempre la misma hasta que cambia la malla. Rehacerlo en cada marca del pincel —hasta 200.000
// caras— era una de las tres cosas que se comían la CPU. Ahora se hace una vez y se guarda, junto
// con su proyección en XY, que es el lienzo real donde puede caer la huella.
const std::vector<int> &GLGizmoSupportZones::region_for(int seed_facet) const
{
    if (m_region_cache_facet == seed_facet && m_region_cache_trafo.isApprox(m_world_trafo))
        return m_region_cache;

    // 🚨 s299b — SIEMPRE el flood-fill COPLANAR (20° contra la semilla), nunca el del pincel.
    // El de «toda la superficie que mira hacia abajo» (`collect_paint_region`) era el que se comía
    // la pieza grande, y ya no hace falta: el pincel no tiene región. Aquí sólo llega «tomar la
    // cara» y el recorte de círculo/cuadrado, y los dos quieren la cara que pinchaste, que es lo
    // que este flood-fill acota.
    m_region_cache       = collect_region(seed_facet);
    m_region_cache_facet = seed_facet;
    m_region_cache_trafo = m_world_trafo;
    m_region_area_cache.clear();

    if (m_region_cache.empty())
        return m_region_cache;

    // La proyección en XY, unida de una vez. El micro-offset cierra las costuras de un entero que
    // el redondeo deja entre triángulos vecinos y que sobrevivirían a la unión como astillas.
    Polygons tris;
    tris.reserve(m_region_cache.size());
    for (int f : m_region_cache) {
        const Vec3i32 t = m_mesh.its.indices[f];
        Points p;
        p.reserve(3);
        for (int k = 0; k < 3; ++ k) {
            const Vec3d w = m_world_trafo * m_mesh.its.vertices[t[k]].cast<double>();
            p.emplace_back(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
        }
        Polygon q(std::move(p));
        if (q.area() == 0.)
            continue;                       // de canto: en XY es una línea
        if (! q.is_counter_clockwise())
            q.reverse();
        tris.emplace_back(std::move(q));
    }
    if (! tris.empty())
        m_region_area_cache = union_ex(offset(tris, scaled<float>(0.002f)));
    return m_region_cache;
}

// NEOTKO_SUPPORTZONES_TAG s299b — EL TECHO, MUESTREADO SÓLO DEBAJO DE LO PINTADO
//
// 🚨 La primera versión de s299 rasterizaba el mapa recorriendo los triángulos de la región entera.
// Con una pieza de millones de polígonos eso es inaceptable, y además obligaba a tener región, que
// es justo lo que el pincel no debe necesitar.
//
// 🔑 Ahora el mapa cubre SÓLO la caja de lo que has pintado, con un margen, y se muestrea con un
// rayo vertical por nodo contra el árbol AABB que el gizmo ya tiene montado. Unos miles de rayos en
// vez de millones de triángulos, y el coste deja de depender del tamaño del objeto: depende del
// tamaño de la zona, que es lo que el usuario controla.
//
// La caja sólo crece, nunca encoge, así que seguir pintando sobre lo ya pintado no vuelve a
// muestrear nada. Cuando el trazo se sale, se rehace la caja nueva de una vez.
//
// 🚨 Frontera del §8: estos rayos eligen la ALTURA del techo del bloque que el usuario ha dibujado.
// No deciden dónde hace falta soporte — eso sigue siendo `detect_overhangs()` en el motor — ni
// proponen destinos. Es la misma clase de consulta que el picking del cursor.
// NEOTKO_SUPPORTZONES_TAG s300c — EL TECHO, RASTERIZADO DESDE LO PINTADO
//
// 🔑 La segunda mitad del híbrido, y la que faltaba: si la marca ya vive en la superficie, la altura
// del techo no hay que ir a buscarla — está en los triángulos que marcaste. Una pasada por ellos y
// listo, sin tocar el árbol de la malla ni una vez.
//
// Frente a los rayos: coste proporcional a lo PINTADO en vez de al área de la caja, exacto en vez de
// muestreado, y sin el riesgo de que un rayo se encuentre otra pared por el camino.
void GLGizmoSupportZones::heightmap_from_painted(const BoundingBox &area_bb) const
{
    m_hm_nx = m_hm_ny = 0;
    m_hm_z.clear();
    m_hm_n.clear();
    if (m_painted_list.empty())
        return;

    constexpr double MARGIN = 1.0;   // mm: el borde del área se interpola con nodos de fuera
    const double x0 = unscaled<double>(double(area_bb.min.x())) - MARGIN;
    const double y0 = unscaled<double>(double(area_bb.min.y())) - MARGIN;
    const double x1 = unscaled<double>(double(area_bb.max.x())) + MARGIN;
    const double y1 = unscaled<double>(double(area_bb.max.y())) + MARGIN;

    double step = 0.5;
    constexpr int MAX_CELLS = 250000;
    int nx = std::max(1, int(std::ceil((x1 - x0) / step)));
    int ny = std::max(1, int(std::ceil((y1 - y0) / step)));
    while (nx * ny > MAX_CELLS) {
        step *= 1.5;
        nx = std::max(1, int(std::ceil((x1 - x0) / step)));
        ny = std::max(1, int(std::ceil((y1 - y0) / step)));
    }
    m_hm_origin = Vec2d(x0, y0);
    m_hm_step   = step;
    m_hm_nx     = nx;
    m_hm_ny     = ny;
    m_hm_trafo  = m_world_trafo;
    const int w = nx + 1;
    m_hm_z.assign(size_t(w) * size_t(ny + 1), std::numeric_limits<float>::quiet_NaN());
    m_hm_n.assign(size_t(w) * size_t(ny + 1), Vec3f::Zero());

    for (int f : m_painted_list) {
        if (f < 0 || f >= int(m_mesh.its.indices.size()))
            continue;
        const Vec3i32 t = m_mesh.its.indices[f];
        const Vec3d A = m_world_trafo * m_mesh.its.vertices[t[0]].cast<double>();
        const Vec3d B = m_world_trafo * m_mesh.its.vertices[t[1]].cast<double>();
        const Vec3d C = m_world_trafo * m_mesh.its.vertices[t[2]].cast<double>();
        Vec3d nrm = (B - A).cross(C - A);
        const double nl = nrm.norm();
        if (nl < 1e-12)
            continue;
        nrm /= nl;
        if (nrm.z() > 0.)
            nrm = - nrm;                 // todo mirando hacia abajo, como el resto del gizmo
        if (std::abs(nrm.z()) < 1e-9)
            continue;                    // de canto: en XY es una línea y no tapa ningún nodo

        const int i_lo = std::max(0,  int(std::floor((std::min({ A.x(), B.x(), C.x() }) - x0) / step)));
        const int i_hi = std::min(nx, int(std::ceil ((std::max({ A.x(), B.x(), C.x() }) - x0) / step)));
        const int j_lo = std::max(0,  int(std::floor((std::min({ A.y(), B.y(), C.y() }) - y0) / step)));
        const int j_hi = std::min(ny, int(std::ceil ((std::max({ A.y(), B.y(), C.y() }) - y0) / step)));
        if (i_lo > i_hi || j_lo > j_hi)
            continue;

        const double d = (B.y() - C.y()) * (A.x() - C.x()) + (C.x() - B.x()) * (A.y() - C.y());
        if (std::abs(d) < 1e-12)
            continue;
        const double d_plane = nrm.dot(A);

        for (int j = j_lo; j <= j_hi; ++ j) {
            const double py = y0 + step * j;
            for (int i = i_lo; i <= i_hi; ++ i) {
                const double px = x0 + step * i;
                const double l1 = ((B.y() - C.y()) * (px - C.x()) + (C.x() - B.x()) * (py - C.y())) / d;
                const double l2 = ((C.y() - A.y()) * (px - C.x()) + (A.x() - C.x()) * (py - C.y())) / d;
                const double l3 = 1. - l1 - l2;
                // El épsilon cose los nodos que caen justo sobre una arista compartida.
                if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9)
                    continue;
                const double zw = (d_plane - nrm.x() * px - nrm.y() * py) / nrm.z();
                const size_t k  = size_t(j) * size_t(w) + size_t(i);
                // 🔑 Gana el MÁS ALTO, y aquí sí es lo correcto: todo lo que se mira está pintado, o
                // sea elegido por el usuario, y el techo del bloque tiene que llegar a lo más alto de
                // lo que sujeta. (Con los rayos había que elegir "el más cercano al clic" porque
                // entraban superficies que nadie había pedido.)
                if (std::isnan(m_hm_z[k]) || zw > double(m_hm_z[k])) {
                    m_hm_z[k] = float(zw);
                    m_hm_n[k] = nrm.cast<float>();
                }
            }
        }
    }
}

void GLGizmoSupportZones::ensure_heightmap(const BoundingBox &area_bb, double z_ref) const
{
    if (! m_raycaster)
        return;

    constexpr double MARGIN = 3.0;   // mm: el trazo siguiente casi siempre cae dentro
    double nx0 = unscaled<double>(double(area_bb.min.x())) - MARGIN;
    double ny0 = unscaled<double>(double(area_bb.min.y())) - MARGIN;
    double nx1 = unscaled<double>(double(area_bb.max.x())) + MARGIN;
    double ny1 = unscaled<double>(double(area_bb.max.y())) + MARGIN;

    const bool same_frame = m_hm_trafo.isApprox(m_world_trafo) && std::abs(m_hm_z_ref - z_ref) < 1e-6;
    if (m_hm_nx > 0 && m_hm_ny > 0 && same_frame) {
        const double ox1 = m_hm_origin.x() + m_hm_step * m_hm_nx;
        const double oy1 = m_hm_origin.y() + m_hm_step * m_hm_ny;
        if (nx0 >= m_hm_origin.x() && ny0 >= m_hm_origin.y() && nx1 <= ox1 && ny1 <= oy1)
            return;                     // lo que hay ya lo cubre
        // Crece englobando lo viejo, para que el mapa no se vaya rehaciendo a trocitos.
        nx0 = std::min(nx0, m_hm_origin.x());
        ny0 = std::min(ny0, m_hm_origin.y());
        nx1 = std::max(nx1, ox1);
        ny1 = std::max(ny1, oy1);
    }

    // Medio milímetro es más fino que la propia extrusión del soporte, y el techo de celdas evita
    // que una zona enorme se convierta en un problema: cede el paso antes que la interactividad.
    double step = 0.5;
    constexpr int MAX_CELLS = 90000;
    int nx = std::max(1, int(std::ceil((nx1 - nx0) / step)));
    int ny = std::max(1, int(std::ceil((ny1 - ny0) / step)));
    while (nx * ny > MAX_CELLS) {
        step *= 1.5;
        nx = std::max(1, int(std::ceil((nx1 - nx0) / step)));
        ny = std::max(1, int(std::ceil((ny1 - ny0) / step)));
    }

    m_hm_origin = Vec2d(nx0, ny0);
    m_hm_step   = step;
    m_hm_nx     = nx;
    m_hm_ny     = ny;
    m_hm_trafo  = m_world_trafo;
    m_hm_z_ref  = z_ref;
    const int w = nx + 1;
    m_hm_z.assign(size_t(w) * size_t(ny + 1), std::numeric_limits<float>::quiet_NaN());
    m_hm_n.assign(size_t(w) * size_t(ny + 1), Vec3f::Zero());

    // El árbol vive en espacio de malla, así que el rayo va allí. La dirección NO se normaliza tras
    // la transformación a propósito: sólo se usa para ordenar y para reconstruir el punto.
    const Transform3d inv  = m_world_trafo.inverse();
    const AABBMesh   &tree = m_raycaster->get_aabb_mesh();
    const Matrix3d    nrm  = m_world_trafo.linear().inverse().transpose();
    const Vec3d       dir_w { 0., 0., -1. };
    const Vec3d       ldir = inv.linear() * dir_w;

    std::vector<AABBMesh::hit_result> hits;
    for (int j = 0; j <= ny; ++ j) {
        for (int i = 0; i <= nx; ++ i) {
            const double px = nx0 + step * i;
            const double py = ny0 + step * j;
            const Vec3d  src_w { px, py, z_ref + 1000. };
            hits = tree.query_ray_hits(inv * src_w, ldir);
            double best  = 0.;
            Vec3f  best_n { Vec3f::Zero() };
            bool   found = false;
            for (const AABBMesh::hit_result &h : hits) {
                if (! h.is_hit())
                    continue;
                const int f = h.face();
                if (f < 0 || f >= int(m_face_normals.size()))
                    continue;
                Vec3d wn = nrm * m_face_normals[f].cast<double>();
                if (wn.norm() < 1e-12)
                    continue;
                wn.normalize();
                // 🚨 s299g — AQUÍ SE FILTRABA POR "MIRA HACIA ABAJO", Y ERA UN ERROR DE CONCEPTO.
                //
                // El filtro tiene todo el sentido para ELEGIR dónde se puede pintar: una cara que
                // mira hacia arriba no necesita que la sujeten. Pero este mapa no elige nada: dice
                // A QUÉ ALTURA está la pieza sobre cada punto, para que el techo del bloque la
                // toque. Y la pieza está donde está, mire su normal hacia donde mire.
                //
                // Lo que pasaba: en una superficie que se va poniendo vertical —el ecuador de un
                // toro, justo donde uno pinta— muchos nodos no pasaban el filtro y se quedaban en
                // NaN, así que su z caía al plano de la cara semilla. El techo se alejaba de la
                // pieza y el bloque no llegaba a tocarla: "tiene buena pinta, pero no toca y no
                // genera techo". La malla estaba perfecta; lo que estaba mal era su altura.
                const double zw = (m_world_trafo * h.position()).z();
                // La más cercana en z a la cara pinchada, no la primera: una pieza con varios pisos
                // tiene varias, y la que interesa es la que estás sujetando.
                if (! found || std::abs(zw - z_ref) < std::abs(best - z_ref)) {
                    best   = zw;
                    best_n = wn.cast<float>();
                    found  = true;
                }
            }
            if (found) {
                const size_t k = size_t(j) * size_t(w) + size_t(i);
                m_hm_z[k] = float(best);
                m_hm_n[k] = best_n;
            }
        }
    }
}

// El disco de las formas de clic (redondo). Sigue siendo 2D: esas formas SON una primitiva en XY,
// a diferencia del pincel, que ahora vive en la malla.
static Polygon zone_disc(const Vec2d &c, double r)
{
    Points pts;
    const int seg = 32;
    pts.reserve(seg);
    for (int i = 0; i < seg; ++ i) {
        const double a = 2.0 * PI_D * double(i) / double(seg);
        pts.emplace_back(scaled<coord_t>(c.x() + r * std::cos(a)), scaled<coord_t>(c.y() + r * std::sin(a)));
    }
    return Polygon(std::move(pts));
}

// NEOTKO_SUPPORTZONES_TAG s300 — PINTAR TRIÁNGULOS
//
// 🔑 Un recorrido por VECINDAD desde el triángulo que has tocado, aceptando los que caen dentro del
// radio. Y ahí está toda la diferencia con lo de antes: el radio se mide en el espacio, pero el
// camino se anda por la malla. Al otro lado de un agujero no se llega aunque esté a dos milímetros
// en línea recta, porque no hay triángulos que lleven hasta allí.
//
// Es la misma idea que el pincel esférico de los painters de Orca, y el motivo de que aquéllos sean
// instantáneos: marcar un byte por triángulo no cuesta nada.
//
// ⚠️ Se mide el CENTROIDE contra el punto tocado. Con triángulos mucho más grandes que el pincel el
// borde queda a saltos — es la escalera que Rhino no tiene porque allí el contorno es una curva.
// Se acepta: el contorno se suaviza después, al proyectar (`resample_ring` simplifica a 0.02 mm).
size_t GLGizmoSupportZones::paint_facets(int seed, const Vec3d &hit_world, double radius, bool erase)
{
    const int n_facets = int(m_mesh.its.indices.size());
    if (seed < 0 || seed >= n_facets || int(m_face_neighbors.size()) != n_facets)
        return 0;
    if (int(m_painted.size()) != n_facets) {
        m_painted.assign(n_facets, 0);
        m_painted_list.clear();
        m_painted_count = 0;
    }
    // 🚨 s300b — EL SELLO EN VEZ DE UN `visited` NUEVO. Reservar y limpiar un vector del tamaño de la
    // malla en CADA marca es, en una pieza de millones de triángulos, varios megas por movimiento
    // del ratón. Con un sello que sube, "no visitado" es "su número no es el de esta pincelada" y no
    // hay nada que limpiar.
    // ⚠️ El desbordamiento del contador se trata: al dar la vuelta, todo el mundo parecería visitado.
    if (int(m_visit_stamp.size()) != n_facets) {
        m_visit_stamp.assign(n_facets, 0);
        m_visit_epoch = 0;
    }
    if (++ m_visit_epoch == 0) {
        std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
        m_visit_epoch = 1;
    }

    const char   want = erase ? 0 : 1;
    const double r2   = radius * radius;

    // 🚨 s300d — EL TIPO DE RETORNO VA ESCRITO, Y ÉSTE ERA EL FALLO QUE PINTABA LA PIEZA ENTERA.
    //
    // Con `auto`, esta lambda NO devolvía un `Vec3d`: devolvía la EXPRESIÓN de Eigen que describe
    // "suma estos tres productos matriz-vector y divide por tres". Eigen no evalúa hasta que se
    // asigna a un tipo concreto, y esa expresión guarda REFERENCIAS a los temporales de dentro de la
    // lambda — que mueren al salir de ella. Lo que llegaba a la resta de fuera era basura.
    //
    // Por eso el log decía `visitas=200000 cambiados=200000` con un pincel de 4 mm: el test del
    // radio no filtraba nada porque la distancia se calculaba contra memoria muerta, y el recorrido
    // se comía la pieza hasta el tope. No era el radio, ni la vecindad, ni la matriz.
    //
    // Es la misma trampa que el fork ya tiene apuntada con `normalized()`: en Eigen, `auto` guarda
    // la receta, no el resultado. En este fichero se escribe el tipo, siempre.
    auto centroid = [&](int f) -> Vec3d {
        const Vec3i32 t = m_mesh.its.indices[f];
        return (m_world_trafo * m_mesh.its.vertices[t[0]].cast<double>()
              + m_world_trafo * m_mesh.its.vertices[t[1]].cast<double>()
              + m_world_trafo * m_mesh.its.vertices[t[2]].cast<double>()) / 3.;
    };

    size_t changed = 0;
    std::vector<int> stack { seed };
    m_visit_stamp[seed] = m_visit_epoch;
    // Tope de cordura para un pincel enorme sobre una malla densa.
    const size_t max_visit = 200000;
    size_t visits = 0;
    while (! stack.empty() && visits < max_visit) {
        const int f = stack.back();
        stack.pop_back();
        ++ visits;
        if ((centroid(f) - hit_world).squaredNorm() > r2)
            continue;                    // fuera del pincel: ni se pinta ni se sigue por aquí
        if (m_painted[f] != want) {
            m_painted[f] = want;
            if (want)
                m_painted_list.push_back(f);
            ++ changed;
        }
        for (int e = 0; e < 3; ++ e) {
            const int nb = m_face_neighbors[f][e];
            if (nb >= 0 && nb < n_facets && m_visit_stamp[nb] != m_visit_epoch) {
                m_visit_stamp[nb] = m_visit_epoch;
                stack.push_back(nb);
            }
        }
    }
    // 🔎 s300c — cuánto cuesta UNA pincelada, en el log. Es el número que faltaba: cuando él dijo
    // "sale la ruedita" no había forma de saber si el caro era el pincel, la huella o el techo, y
    // se fue a buscar al sitio equivocado. Ahora lo dice.
    if (NeoDebug::enabled(NeoDebug::SUPPORTZONES)) {
        char b[192];
        std::snprintf(b, sizeof(b), "brush     visitas=%d  cambiados=%d  pintados=%d  (radio %.1f mm)",
                      int(visits), int(changed), int(m_painted_list.size()), radius);
        NeoDebug::write(NeoDebug::SUPPORTZONES, b);
    }
    if (changed > 0) {
        // Al borrar, la lista se queda con índices que ya no están pintados: se compacta aquí, que
        // es barato porque la lista son los pintados, no la malla.
        if (erase)
            m_painted_list.erase(std::remove_if(m_painted_list.begin(), m_painted_list.end(),
                                                [this](int f) { return ! m_painted[f]; }),
                                 m_painted_list.end());
        m_painted_count = m_painted_list.size();
        ++ m_stamp_stamp;
    }
    return changed;
}

void GLGizmoSupportZones::clear_painted()
{
    // Sin `assign` sobre la malla entera: se apagan sólo los que estaban encendidos.
    for (int f : m_painted_list)
        if (f >= 0 && f < int(m_painted.size()))
            m_painted[f] = 0;
    m_painted_list.clear();
    m_painted_count = 0;
    ++ m_stamp_stamp;
}

// La proyección en XY de lo pintado. Es la frontera entre las dos mitades del híbrido: de aquí para
// abajo todo vuelve a ser 2D y Clipper, igual que antes.
//
// 🚨 s300b — CACHEADA. La piden el sólido, el contorno del panel, la huella y los avisos, varias
// veces por frame. Sin caché, cada una de esas llamadas volvía a unir todos los triángulos pintados
// con Clipper.
ExPolygons GLGizmoSupportZones::painted_area_world() const
{
    if (m_painted_area_stamp == m_stamp_stamp && m_painted_area_trafo.isApprox(m_world_trafo))
        return m_painted_area;

    ExPolygons out;
    if (! m_painted_list.empty()) {
        Polygons tris;
        tris.reserve(m_painted_list.size());
        for (int f : m_painted_list) {
            if (f < 0 || f >= int(m_mesh.its.indices.size()))
                continue;
            const Vec3i32 t = m_mesh.its.indices[f];
            Points p;
            p.reserve(3);
            for (int k = 0; k < 3; ++ k) {
                const Vec3d w = m_world_trafo * m_mesh.its.vertices[t[k]].cast<double>();
                p.emplace_back(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
            }
            Polygon q(std::move(p));
            if (q.area() == 0.)
                continue;                // de canto: en XY es una línea
            if (! q.is_counter_clockwise())
                q.reverse();
            tris.emplace_back(std::move(q));
        }
        // El micro-offset cierra las costuras de un entero que el redondeo deja entre triángulos
        // vecinos y que sobrevivirían a la unión como astillas.
        if (! tris.empty())
            out = union_ex(offset(tris, scaled<float>(0.002f)));
    }
    m_painted_area       = out;
    m_painted_area_stamp = m_stamp_stamp;
    m_painted_area_trafo = m_world_trafo;
    return out;
}

// Vuelve a pintar desde cero a partir de los toques guardados. Lo usan el borrado (que no se puede
// hacer sumando) y la reapertura de una zona guardada.
void GLGizmoSupportZones::repaint_from_stamps()
{
    clear_painted();
    if (! m_raycaster)
        return;
    for (const Stamp &st : m_stamps) {
        // El triángulo bajo el toque se reencuentra por posición, no por índice: un índice a la
        // malla no sobrevive a una reparación, y el gesto tiene que sobrevivir (misma razón que
        // documenta `begin_edit_zone`).
        const Vec3d local = m_world_trafo.inverse() * st.p;
        const int   f     = m_raycaster->get_closest_facet(local.cast<float>());
        if (f >= 0)
            paint_facets(f, st.p, st.r, false);
    }
}

// --------------------------------------------------------------------------------------------
// El mapa de alturas: leer
// --------------------------------------------------------------------------------------------
double GLGizmoSupportZones::z_at(const ZoneMask &mk, const Vec2d &p) const
{
    auto on_plane = [&]() {
        if (std::abs(mk.seed_n.z()) < 1e-6)
            return mk.seed_p.z();
        return (mk.seed_n.dot(mk.seed_p) - mk.seed_n.x() * p.x() - mk.seed_n.y() * p.y()) / mk.seed_n.z();
    };
    if (m_hm_nx < 1 || m_hm_ny < 1 || m_hm_step <= 0.)
        return on_plane();

    const double fx = (p.x() - m_hm_origin.x()) / m_hm_step;
    const double fy = (p.y() - m_hm_origin.y()) / m_hm_step;
    const int    i0 = int(std::floor(fx));
    const int    j0 = int(std::floor(fy));
    if (i0 < 0 || j0 < 0 || i0 + 1 > m_hm_nx || j0 + 1 > m_hm_ny)
        return on_plane();
    const double tx = fx - double(i0);
    const double ty = fy - double(j0);
    const int    w  = m_hm_nx + 1;
    const float  z00 = m_hm_z[size_t(j0)     * size_t(w) + size_t(i0)];
    const float  z10 = m_hm_z[size_t(j0)     * size_t(w) + size_t(i0 + 1)];
    const float  z01 = m_hm_z[size_t(j0 + 1) * size_t(w) + size_t(i0)];
    const float  z11 = m_hm_z[size_t(j0 + 1) * size_t(w) + size_t(i0 + 1)];
    // 🚨 Un solo nodo sin superficie invalida la celda entera: interpolar contra un NaN da NaN, e
    // inventarse el valor del vecino estiraría el techo hacia un sitio donde no hay pieza. Ahí
    // manda el plano, que es plano y está en el aire, que es lo honesto.
    if (std::isnan(z00) || std::isnan(z10) || std::isnan(z01) || std::isnan(z11))
        return on_plane();
    const double a = double(z00) + (double(z10) - double(z00)) * tx;
    const double b = double(z01) + (double(z11) - double(z01)) * tx;
    return a + (b - a) * ty;
}

Vec3d GLGizmoSupportZones::n_at(const ZoneMask &mk, const Vec2d &p) const
{
    if (m_hm_nx < 1 || m_hm_ny < 1 || m_hm_step <= 0.)
        return mk.seed_n;
    const int i = int(std::lround((p.x() - m_hm_origin.x()) / m_hm_step));
    const int j = int(std::lround((p.y() - m_hm_origin.y()) / m_hm_step));
    if (i < 0 || j < 0 || i > m_hm_nx || j > m_hm_ny)
        return mk.seed_n;
    const Vec3f &n = m_hm_n[size_t(j) * size_t(m_hm_nx + 1) + size_t(i)];
    if (n.squaredNorm() < 1e-12f)
        return mk.seed_n;
    return n.cast<double>();
}

// --------------------------------------------------------------------------------------------
// NEOTKO_SUPPORTZONES_TAG s299 — CONSTRUIR LA MÁSCARA
// --------------------------------------------------------------------------------------------
// Dos cosas y en este orden: el ÁREA (Clipper) y el TECHO (un mapa de alturas rasterizado desde
// los triángulos de la región). A partir de aquí nadie vuelve a mirar la malla: el sólido, el
// contorno del panel, el resaltado y los avisos salen todos de estas dos.
//
// 🔑 El techo no es plano y eso es deliberado: el dueño descartó el plano por ser el caso fácil.
// Donde hay pieza, la z sale de la superficie de verdad; donde no la hay, del plano de la cara que
// pinchaste, que es lo que uno espera de un bloque que sobresale del voladizo.
bool GLGizmoSupportZones::build_mask(int seed_facet, const Vec2d &centre_xy, ZoneMask &out) const
{
    out = ZoneMask();
    const int n_facets = int(m_mesh.its.indices.size());
    if (seed_facet < 0 || seed_facet >= n_facets || int(m_face_normals.size()) != n_facets)
        return false;

    // El plano de la cara pinchada, que es el respaldo del techo donde no hay pieza debajo.
    {
        const Vec3i32 t0 = m_mesh.its.indices[seed_facet];
        out.seed_p = (m_world_trafo * m_mesh.its.vertices[t0[0]].cast<double>()
                    + m_world_trafo * m_mesh.its.vertices[t0[1]].cast<double>()
                    + m_world_trafo * m_mesh.its.vertices[t0[2]].cast<double>()) / 3.;
        Vec3d n = m_world_trafo.linear().inverse().transpose() * m_face_normals[seed_facet].cast<double>();
        if (n.norm() > 1e-12) {
            n.normalize();
            if (n.z() > 0.)
                n = - n;
            out.seed_n = n;
        }
    }

    // ---- el área ------------------------------------------------------------------------------
    //
    // 🚨 s299b — EL PINCEL NO TIENE REGIÓN, Y ÉSTE ERA EL ERROR QUE SE VEÍA EN LA MANO.
    //
    // La primera versión de s299 seguía atando el pincel al flood-fill: recortaba lo pintado contra
    // la proyección en XY de TODA la superficie que mira hacia abajo. En una pieza de millones de
    // triángulos eso son dos cosas carísimas seguidas — el flood-fill de cientos de miles de caras
    // y una unión de Clipper con todos esos triángulos — y encima da un área enorme, que es el
    // bloque gigante que salía en pantalla.
    //
    // 🔑 Lo dijo el dueño desde el principio y no lo apliqué hasta el final: "pintar un área es
    // pintar un área", y "es un fake, no necesitamos una recreación perfecta, tenemos margen". Lo
    // que pintas ES la huella. Punto. Nadie le pregunta nada a la malla.
    //
    // La superficie sigue entrando, pero sólo donde importa: el TECHO se muestrea debajo de lo
    // pintado (`ensure_heightmap`), que son unos miles de puntos y no depende del tamaño del
    // objeto.
    ExPolygons shape;
    bool needs_region = false;
    switch (m_foot_shape) {
    case FootShape::Patch:
        // «Tomar la cara»: aquí la región SÍ es el gesto, y es el flood-fill coplanar a 20°, que
        // está acotado por construcción (una cara, no media pieza).
        needs_region = true;
        break;
    case FootShape::Round:
        if (m_foot_size_mm > 0.f)
            shape.emplace_back(zone_disc(centre_xy, 0.5 * double(m_foot_size_mm)));
        needs_region = ! m_shape_covers;
        break;
    case FootShape::Square: {
        if (m_foot_size_mm <= 0.f)
            break;
        const double h = 0.5 * double(m_foot_size_mm);
        Points pts { { scaled<coord_t>(centre_xy.x() - h), scaled<coord_t>(centre_xy.y() - h) },
                     { scaled<coord_t>(centre_xy.x() + h), scaled<coord_t>(centre_xy.y() - h) },
                     { scaled<coord_t>(centre_xy.x() + h), scaled<coord_t>(centre_xy.y() + h) },
                     { scaled<coord_t>(centre_xy.x() - h), scaled<coord_t>(centre_xy.y() + h) } };
        shape.emplace_back(Polygon(std::move(pts)));
        needs_region = ! m_shape_covers;
        break;
    }
    case FootShape::Brush:
        // s300 — la proyección de los TRIÁNGULOS pintados. Ni flood-fill ni recorte: lo que has
        // marcado sobre la superficie es la huella, y ya viene de la malla, así que no puede
        // contener nada que no hayas tocado.
        shape = painted_area_world();
        needs_region = false;
        break;
    }

    if (needs_region) {
        const std::vector<int> &region = region_for(seed_facet);
        if (region.empty() || m_region_area_cache.empty())
            return false;
        out.area = shape.empty() ? m_region_area_cache
                                 : intersection_ex(m_region_area_cache, shape);
    } else {
        if (shape.empty())
            return false;
        out.area = shape;
    }

    if (out.area.empty())
        return false;
    const BoundingBox abb = get_extents(out.area);
    // 🚨 s300c — CON EL PINCEL, EL TECHO SALE DE LOS TRIÁNGULOS PINTADOS, NO DE RAYOS. Éste era el
    // cuelgue, y no estaba en el pincel: estaba aquí.
    //
    // `ensure_heightmap()` dispara UN RAYO VERTICAL POR NODO contra el árbol de la malla. Con paso
    // de medio milímetro y un área pintada mediana eso son decenas de miles de rayos, y con la pieza
    // de 1,47 millones de triángulos cada rayo cuesta. Y como la máscara se invalida con cada marca
    // del pincel, ese barrido entero se repetía EN CADA MARCA. De ahí la ruedita y que lo único que
    // se refrescara de vez en cuando fuera un frame suelto.
    //
    // 🔑 Y era trabajo tirado además de caro: los triángulos de la superficie que queremos ya los
    // tenemos marcados. Rasterizarlos cuesta lo que cuesta recorrerlos —cientos— y da la altura
    // EXACTA de lo que pintaste, no la de lo que un rayo se encuentre por el camino.
    //
    // Las formas de clic (círculo, cara entera) siguen con los rayos: ahí no hay lista de triángulos
    // pintados, y su área está acotada por el tamaño del mando.
    if (m_foot_shape == FootShape::Brush && ! m_painted_list.empty())
        heightmap_from_painted(abb);
    else
        ensure_heightmap(abb, out.seed_p.z());

    // ---- el rango de alturas ------------------------------------------------------------------
    // Se mide sobre lo que de verdad va a acabar en el techo: los nodos con superficie y los puntos
    // del contorno del área, que son los que llevan la pared.
    out.z_low  = std::numeric_limits<double>::max();
    out.z_high = std::numeric_limits<double>::lowest();
    // 🚨 Sólo los nodos de la BBOX de la huella, no los del lienzo entero. El mapa de alturas cubre
    // toda la región —puede tener cientos de miles de nodos— y la huella de un pincel es un
    // pañuelo: recorrer el mapa completo con un `contains` por nodo era gratis en el papel y se
    // notaba en la mano, porque esto se recalcula con cada marca.
    int i0 = 0, i1 = -1, j0 = 0, j1 = -1;
    if (m_hm_nx > 0 && m_hm_ny > 0 && m_hm_step > 0.) {
        i0 = std::max(0, int(std::floor((unscaled<double>(double(abb.min.x())) - m_hm_origin.x()) / m_hm_step)));
        i1 = std::min(m_hm_nx, int(std::ceil((unscaled<double>(double(abb.max.x())) - m_hm_origin.x()) / m_hm_step)));
        j0 = std::max(0, int(std::floor((unscaled<double>(double(abb.min.y())) - m_hm_origin.y()) / m_hm_step)));
        j1 = std::min(m_hm_ny, int(std::ceil((unscaled<double>(double(abb.max.y())) - m_hm_origin.y()) / m_hm_step)));
    }
    for (int j = j0; j <= j1; ++ j)
        for (int i = i0; i <= i1; ++ i) {
            const float z = m_hm_z[size_t(j) * size_t(m_hm_nx + 1) + size_t(i)];
            if (std::isnan(z))
                continue;
            const Vec2d p(m_hm_origin.x() + m_hm_step * i, m_hm_origin.y() + m_hm_step * j);
            const Point q(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
            bool inside = false;
            for (const ExPolygon &ep : out.area)
                if (ep.contains(q)) { inside = true; break; }
            if (! inside)
                continue;
            out.z_low  = std::min(out.z_low,  double(z));
            out.z_high = std::max(out.z_high, double(z));
        }
    for (const ExPolygon &ep : out.area)
        for (const Point &q : ep.contour.points) {
            const double z = z_at(out, Vec2d(unscaled<double>(q.x()), unscaled<double>(q.y())));
            out.z_low  = std::min(out.z_low,  z);
            out.z_high = std::max(out.z_high, z);
        }
    if (out.z_low > out.z_high) {
        // Ni un nodo ni un punto de contorno: el área está entera fuera de la pieza. El plano de la
        // semilla es entonces todo lo que hay, y sigue siendo una respuesta válida.
        out.z_low = out.z_high = out.seed_p.z();
    }

    out.ok = true;
    return true;
}

void GLGizmoSupportZones::invalidate_patch()
{
    m_mask_cache_facet = -1;
    m_foot_cache_facet = -1;
}

const GLGizmoSupportZones::ZoneMask *GLGizmoSupportZones::mask(int facet_idx, Vec2d centre_xy) const
{
    // 🔑 s289 — con el pincel la semilla NO es la cara bajo el cursor: es la primera que se pintó.
    // Así el lienzo sigue siendo el flood-fill desde ella y pintar nunca se salta una esquina hacia
    // otra cara; y de paso todos los sitios que ya llamaban con la cara del cursor siguen valiendo.
    if (m_foot_shape == FootShape::Brush && m_brush_seed_facet >= 0 && ! m_stamps.empty()) {
        facet_idx = m_brush_seed_facet;
        centre_xy = Vec2d(m_stamps.front().p.x(), m_stamps.front().p.y());
    }
    // El modo cubrir va DENTRO de la clave de forma (+100): cambia la geometría entera y no tener
    // que añadir un campo a las dos cachés es lo que evita olvidarse de una.
    const int shape_key = int(m_foot_shape) + (m_shape_covers ? 100 : 0);
    if (m_mask_cache_facet == facet_idx && m_mask_cache_shape == shape_key
        && m_mask_cache_size == m_foot_size_mm && m_mask_cache_centre == centre_xy
        && m_mask_cache_stamp == m_stamp_stamp
        && m_mask_cache_trafo.isApprox(m_world_trafo))
        return m_mask_cache.ok ? &m_mask_cache : nullptr;

    build_mask(facet_idx, centre_xy, m_mask_cache);
    m_mask_cache_facet  = facet_idx;
    m_mask_cache_shape  = shape_key;
    m_mask_cache_size   = m_foot_size_mm;
    m_mask_cache_centre = centre_xy;
    m_mask_cache_stamp  = m_stamp_stamp;
    m_mask_cache_trafo  = m_world_trafo;
    return m_mask_cache.ok ? &m_mask_cache : nullptr;
}

// NEOTKO_SUPPORTZONES_TAG s299 — REMUESTREAR UN ANILLO
//
// 🔑 Es la pieza que hace posible dejar de mover vértices a mano. Cada anillo del pilar se calcula
// con su propio `offset_ex()`, así que ni comparten número de vértices ni orden, y sin eso no se
// pueden coser las paredes. Remuestrear los dos por LONGITUD DE ARCO, con el mismo número de
// puntos y empezando por el mismo sitio, los vuelve a hacer cosibles sin volver al método que se
// cruzaba consigo mismo.
//
// ⚠️ Se queda con la isla más grande. Un anillo partido en dos por un offset agresivo no es un
// prisma, y el bloque de soporte quiere ser uno.
std::vector<Vec2d> GLGizmoSupportZones::resample_ring(const ExPolygons &area, int n_pts)
{
    std::vector<Vec2d> out;
    if (area.empty() || n_pts < 3)
        return out;
    size_t best = 0;
    for (size_t i = 1; i < area.size(); ++ i)
        if (std::abs(area[i].contour.area()) > std::abs(area[best].contour.area()))
            best = i;
    Polygon c = area[best].contour;
    // ⚠️ Los agujeros se tiran a propósito: el pilar es un prisma macizo, y el techo que hay sobre
    // un agujero también quiere sujeción.
    if (! c.is_counter_clockwise())
        c.reverse();
    // 🚨 s299e — SIMPLIFICAR ANTES DE MUESTREAR, y no es cosmético.
    //
    // El contorno viene de Clipper con detalle de micras. Muestrearlo a paso fijo puede saltarse un
    // diente estrecho y devolver una poligonal que se cruza consigo misma; y una tapa triangulada
    // sobre un contorno que se cruza sale rota, que es de donde salían mallas no-manifold sin que
    // nada avisara. Quitando antes lo que es más fino que el propio muestreo, eso no puede pasar.
    c.douglas_peucker(scaled<double>(0.02));
    if (c.points.size() < 3)
        return out;

    std::vector<Vec2d> pts;
    pts.reserve(c.points.size());
    for (const Point &p : c.points)
        pts.emplace_back(unscaled<double>(p.x()), unscaled<double>(p.y()));

    // El punto de partida: el más cercano a la dirección +X desde el centroide. Es arbitrario, pero
    // es EL MISMO para todos los anillos, que es lo único que importa para que la costura no gire.
    Vec2d ctr { 0., 0. };
    for (const Vec2d &p : pts)
        ctr += p;
    ctr /= double(pts.size());
    size_t start = 0;
    double best_a = 1e9;
    for (size_t i = 0; i < pts.size(); ++ i) {
        const Vec2d d = pts[i] - ctr;
        const double a = std::abs(std::atan2(d.y(), d.x()));
        if (a < best_a) { best_a = a; start = i; }
    }

    std::vector<double> seg(pts.size(), 0.);
    double total = 0.;
    for (size_t i = 0; i < pts.size(); ++ i) {
        const Vec2d &a = pts[(start + i) % pts.size()];
        const Vec2d &b = pts[(start + i + 1) % pts.size()];
        seg[i] = (b - a).norm();
        total += seg[i];
    }
    if (total < 1e-9)
        return out;

    out.reserve(n_pts);
    size_t si = 0;
    double acc = 0.;
    for (int k = 0; k < n_pts; ++ k) {
        const double want = total * double(k) / double(n_pts);
        while (si + 1 < seg.size() && acc + seg[si] < want) {
            acc += seg[si];
            ++ si;
        }
        const Vec2d &a = pts[(start + si) % pts.size()];
        const Vec2d &b = pts[(start + si + 1) % pts.size()];
        const double t = (seg[si] > 1e-12) ? std::clamp((want - acc) / seg[si], 0., 1.) : 0.;
        out.push_back(a + (b - a) * t);
    }
    // 🚨 s299e — Y NINGUNO REPETIDO. Dos puntos de muestreo que caen en el mismo sitio producen un
    // par de triángulos de pared con área cero. No es un detalle estético: es de donde salía la
    // malla no-manifold, porque el filtro de degenerados que había al final los borraba y dejaba
    // sus tres aristas huérfanas. Se evita el degenerado en vez de borrarlo después.
    //
    // Devuelve vacío en vez de un anillo corto: el resto del sólido cuenta con que todos los
    // anillos tengan el mismo número de puntos, y un anillo distinto sería peor que ninguno.
    for (size_t i = 0; i < out.size(); ++ i)
        if ((out[(i + 1) % out.size()] - out[i]).squaredNorm() < 1e-12)
            return std::vector<Vec2d>();
    return out;
}

// El contorno de la huella metida `edge_mm` por su borde. Con `offset_ex` esto ES la sección del
// sólido a esa altura, no una aproximación de ella: el mismo Clipper que construye el anillo del
// pilar construye lo que dibuja el panel, así que no pueden discrepar.
//
// 🚨 s299 — y aquí es donde se arregla el bug de expandir dos veces. `offset_ex` con un delta
// negativo (crecer) resuelve las auto-intersecciones y funde las islas que se tocan; el
// desplazamiento por bisectriz que había antes no sabía hacer ninguna de las dos cosas, y por eso
// una zona cóncava —todo lo pintado con pincel lo es— salía con las paredes cruzadas.
std::vector<Vec2d> GLGizmoSupportZones::ring_outline_world(double edge_mm) const
{
    std::vector<Vec2d> out;
    if (! m_has_target)
        return out;
    const ZoneMask *mk = mask(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (mk == nullptr)
        return out;

    const ExPolygons ring = (std::abs(edge_mm) < 1e-6)
        ? mk->area
        : offset_ex(mk->area, scaled<float>(- edge_mm), ClipperLib::jtRound);
    if (ring.empty())
        return out;   // te has pasado metiendo: no hay forma, y eso es una respuesta

    size_t best = 0;
    for (size_t i = 1; i < ring.size(); ++ i)
        if (std::abs(ring[i].contour.area()) > std::abs(ring[best].contour.area()))
            best = i;
    Polygon outline = ring[best].contour;
    outline.douglas_peucker(scaled<double>(0.05));
    if (outline.points.size() < 3)
        return out;
    if (! outline.is_counter_clockwise())
        outline.reverse();
    out.reserve(outline.points.size());
    for (const Point &p : outline.points)
        out.emplace_back(unscaled<double>(p.x()), unscaled<double>(p.y()));
    return out;
}

std::vector<Vec2d> GLGizmoSupportZones::target_footprint_world() const
{
    const Vec2d centre = Vec2d(m_target_world_pos.x(), m_target_world_pos.y());
    std::vector<Vec2d> out;
    if (! m_has_target)
        return out;
    if (m_foot_cache_facet == m_target_facet_idx && m_foot_cache_shrink == m_footprint_shrink_mm
        && m_foot_cache_shape == int(m_foot_shape) + (m_shape_covers ? 100 : 0)
        && m_foot_cache_size == m_foot_size_mm
        && m_foot_cache_centre == centre && m_foot_cache_stamp == m_stamp_stamp
        && m_foot_cache_trafo.isApprox(m_world_trafo))
        return m_foot_cache;

    const ZoneMask *mk = mask(m_target_facet_idx, centre);
    if (mk == nullptr)
        return out;
    // El z más bajo de la huella (donde empieza la inclinación) sale del mismo sitio que todo lo
    // demás, así que no puede discrepar del sólido.
    m_foot_cache_z_low     = mk->z_low;
    out = ring_outline_world(double(m_footprint_shrink_mm));

    m_foot_cache        = out;
    m_foot_cache_facet  = m_target_facet_idx;
    m_foot_cache_shrink = m_footprint_shrink_mm;
    m_foot_cache_shape  = int(m_foot_shape) + (m_shape_covers ? 100 : 0);
    m_foot_cache_size   = m_foot_size_mm;
    m_foot_cache_centre = centre;
    m_foot_cache_stamp  = m_stamp_stamp;
    m_foot_cache_trafo  = m_world_trafo;
    return out;
}

// 🚨 s299 — el rango del mando, no un tope de colapso. Ver la nota de la declaración: con Clipper
// el colapso se NOTA (la forma sale vacía) en vez de predecirse, así que esto sólo dice hasta dónde
// tiene sentido ofrecer. La mitad del lado menor mete la huella hasta cerrarla y ni un mm más.
double GLGizmoSupportZones::max_inset_mm() const
{
    if (! m_has_target)
        return 0.;
    const ZoneMask *mk = mask(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (mk == nullptr || mk->area.empty())
        return 0.;
    const BoundingBox bb = get_extents(mk->area);
    const double side = unscaled<double>(double(std::min(bb.size().x(), bb.size().y())));
    return std::max(0., 0.5 * side);
}

double GLGizmoSupportZones::max_outset_mm() const
{
    if (! m_has_target)
        return 0.;
    const ZoneMask *mk = mask(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (mk == nullptr || mk->area.empty())
        return 0.;
    // Crecer no tiene tope geométrico —Clipper siempre sabe crecer— así que el tope es de cordura:
    // un palmo generoso, que es donde el dueño pedía sitio para agarrar un alero.
    const BoundingBox bb = get_extents(mk->area);
    const double span = unscaled<double>(double(std::max(bb.size().x(), bb.size().y())));
    return std::max(50.0, span);
}

// NEOTKO_SUPPORTZONES_TAG s289 — EL PINCEL
//
// 🔑 Idea suya: "marco una zona y la extruyo, eso está bien, pero con la misma, si la marca inicial
// es menor en tamaño fijo con slider, podría arrastrarla y marcar zonas en la superficie a
// extruir". Eso es exactamente esto, y encaja sin romper nada porque el recorte ya había dejado de
// ser una forma para ser un conjunto de polígonos: una marca redonda del tamaño del pincel por cada
// posición por la que pasas, todas unidas.
//
// 🚨 NO se sale del parche. La semilla es la primera cara pintada y la región sigue siendo su
// flood-fill coplanar, así que pintar hacia una esquina no se lleva la cara de al lado por delante.
// Las marcas que caen fuera de la región simplemente no recortan nada.
bool GLGizmoSupportZones::paint_at(const Vec2d &mouse_pos, bool erase)
{
    update_candidates(mouse_pos);
    const Candidate *live = live_candidate();
    if (live == nullptr)
        return false;
    const double r = std::max(0.2, 0.5 * double(m_foot_size_mm));

    // s300 — el toque va en 3D y la marca se hace sobre la malla. Ya no hay discos ni cápsulas: el
    // pincel recorre triángulos vecinos, así que el trazo es continuo porque la superficie lo es.
    if (erase) {
        const size_t before = m_stamps.size();
        m_stamps.erase(std::remove_if(m_stamps.begin(), m_stamps.end(),
                                      [&](const Stamp &st) { return (st.p - live->world_pos).norm() <= r; }),
                       m_stamps.end());
        // 🚨 Borrar NO es pintar del revés: quitar los triángulos del pincel se llevaría por delante
        // los que otro toque anterior también cubría. Se vuelve a pintar desde los toques que
        // quedan, que es la única cuenta que no miente. Cuesta un trazo entero y sólo pasa al
        // borrar, que es raro.
        const size_t painted_before = m_painted_count;
        repaint_from_stamps();
        if (m_stamps.size() == before && m_painted_count == painted_before)
            return false;
        if (m_stamps.empty()) {
            // Borrado del todo: se suelta también la semilla, para que la siguiente pincelada pueda
            // empezar en otra cara sin obligar a salir y volver a entrar en el paso 1.
            m_brush_seed_facet = -1;
            m_has_target       = false;
        }
    } else {
        // Una marca por cada 0.8 radios recorridos. Con el pincel sobre la malla la cadencia ya no
        // tiene que garantizar solape —lo garantiza la vecindad— así que es sólo para no guardar mil
        // toques idénticos en el gesto.
        if (! m_stamps.empty() && (m_stamps.back().p - live->world_pos).norm() < 0.8 * r
            && std::abs(m_stamps.back().r - r) < 1e-6)
            return false;
        if (m_stamps.empty()) {
            m_brush_seed_facet    = live->facet_idx;
            m_target_facet_idx    = live->facet_idx;
            m_target_world_pos    = live->world_pos;
            m_target_world_normal = live->world_normal;
            // La zona queda tomada con la primera pincelada, así que el paso 2 se enciende solo.
            // ⛔ Y NO se salta al paso 2 como hace el clic: con el pincel se sigue pintando hasta
            // que uno dice que ha terminado.
            m_has_target = true;
            if (! m_lean_angle_seeded) {
                m_lean_angle_deg    = float(SupportZones::SUPPORT_ZONE_DEFAULT_LEAN_DEG);
                m_lean_angle_seeded = true;
            }
            // NEOTKO_SUPPORTZONES_TAG s300g — y el pie a plomo desde la primera pincelada, para que
            // haya pilar que mirar mientras se pinta. Se refina al soltar, cuando la huella ya es la
            // definitiva (ver el `LeftUp` del pincel).
            Vec3d plumb_pos;
            bool  plumb_on_bed = true;
            if (! m_landing_locked && resolve_landing_plumb(plumb_pos, plumb_on_bed)) {
                m_landing_world_pos = plumb_pos;
                m_landing_on_bed    = plumb_on_bed;
                m_has_landing       = true;
            }
        }
        if (paint_facets(live->facet_idx, live->world_pos, r, false) == 0 && ! m_stamps.empty())
            return false;               // ni un triángulo nuevo: no hay nada que redibujar
        m_stamps.push_back({ live->world_pos, r });
    }

    invalidate_patch();
    m_preview_dirty         = true;
    m_footprint_model_dirty = true;
    m_hover_model_facet     = -1;   // que el resaltado se rehaga con lo recién pintado
    m_target_model_facet    = -1;
    return true;
}

// NEOTKO_SUPPORTZONES_TAG s299 — EL SÓLIDO, DESDE LA MÁSCARA
//
// 🔑 Sigue siendo un EXTRUDE DE SUPERFICIE, que es como el dueño lo pensó en s286b: el techo es la
// superficie de verdad, con su curva, y las paredes bajan de su contorno. Lo que cambia es de
// dónde sale cada anillo.
//
// Antes: un parche de triángulos de la malla, y cada anillo se obtenía moviendo CADA VÉRTICE por su
// bisectriz. Eso no es un offset — no sabe que el borde se cruce consigo mismo, ni fundir dos
// brazos que se tocan — y por eso expandir dos veces daba polígonos imposibles.
//
// Ahora: cada anillo es `offset_ex()` sobre la máscara. Clipper no puede devolver un contorno
// cruzado. A cambio, dos anillos ya no comparten vértices, así que se cosen remuestreándolos a un
// número fijo de puntos por longitud de arco (`resample_ring`).
//
// Los cuatro anillos siguen queriendo decir lo mismo que en s286b:
//   T  el techo, con la z de la superficie por punto, subido medio milímetro para que la superficie
//      quede DENTRO del bloque y nunca justo en una frontera de capa (con ALH esa frontera se
//      mueve, §4-bis.1);
//   M  la cota más baja del techo, en vertical bajo T: aquí acaba lo que ENVUELVE la superficie.
//      Inclinar ahí dentro dejaría la parte media de una banda fuera del bloque — el donut;
//   K  LA RODILLA: aquí se ha consumido TODO el desplazamiento, a ángulo constante desde M;
//   B  el pie, tras el tramo de bajada, que es vertical y por tanto no gasta corredor.
// NEOTKO_SUPPORTZONES_TAG s300h — el eje del pilar, calculado una vez para todo el que pregunte.
// El porqué vive en la declaración (GLGizmoSupportZones.hpp). En corto: el pilar tiene RODILLA, y
// quien lo sondee por la recta de extremo a extremo está mirando donde el pilar no está.
bool GLGizmoSupportZones::pillar_axis(PillarAxis &out) const
{
    if (! m_has_target || ! m_has_landing)
        return false;
    const Vec2d centre  = Vec2d(m_target_world_pos.x(), m_target_world_pos.y());
    const Vec3d landing = m_landing_world_pos;

    const ZoneMask *mk = mask(m_target_facet_idx, centre);
    if (mk == nullptr || mk->area.empty())
        return false;

    out.z_low  = mk->z_low;
    out.z_high = mk->z_high;

    // El desplazamiento se mide contra el MISMO centroide que usa el alcance y el aviso de fuera de
    // alcance (target_footprint_world). Calcularlo aquí a mano sería tener dos centros que se
    // separan sin avisar.
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.size() < 3)
        return false;
    Vec2d c_top { 0., 0. };
    for (const Vec2d &p : foot)
        c_top += p;
    c_top /= double(foot.size());
    out.c_top = c_top;

    const double z_bottom  = landing.z();
    out.z_bot_ext = m_landing_on_bed ? -0.5 : z_bottom;
    if (out.z_high - out.z_bot_ext < 0.2)
        return false;   // nothing worth building
    out.shift = Vec2d(landing.x(), landing.y()) - c_top;

    constexpr double ROOF_LIFT_MM = 0.5;
    out.wraps             = (out.z_low > out.z_bot_ext + 0.2);
    const double offset_d = out.shift.norm();
    constexpr double deg2rad = 0.01745329251994329576923690768489;
    const double tan_a    = std::tan(std::max(0.05, double(m_lean_angle_deg)) * deg2rad);
    const double lean_top = out.wraps ? out.z_low : (out.z_high + ROOF_LIFT_MM);
    // Lo que le cuesta al ángulo recorrer esa distancia. A plomo no cuesta nada y no hay rodilla.
    out.z_knee = (offset_d < 1e-6) ? out.z_bot_ext : (lean_top - offset_d / tan_a);
    // 🚨 La rodilla por debajo del pie es "fuera de alcance": ese ángulo no cubre esa distancia en
    // la altura que hay. Se rechaza aquí y no se dibuja nada, que es la promesa del §1.
    if (out.z_knee < out.z_bot_ext - 1e-6)
        return false;
    out.has_knee = (offset_d > 1e-6) && (out.z_knee > out.z_bot_ext + 0.2);

    out.rings.clear();
    out.rings.emplace_back(out.z_high + ROOF_LIFT_MM, Vec2d(0., 0.));    // T  (z real por punto)
    if (out.wraps)
        out.rings.emplace_back(out.z_low, Vec2d(0., 0.));                // M
    if (out.has_knee)
        out.rings.emplace_back(out.z_knee, out.shift);                   // K
    out.rings.emplace_back(out.z_bot_ext, out.shift);                    // B
    return true;
}

// NEOTKO_SUPPORTZONES_TAG s301 — EXTRUIR UN `ExPolygons` EN UN PRISMA, EXACTO Y SIN REMUESTREAR.
//
// 🔑 Ésta es la pieza que mata el problema medido en el §1 del estudio. El pilar lofteado tiene que
// remuestrear sus anillos (`resample_ring`) porque cose dos contornos que Clipper calculó por
// separado y que no comparten ni número de puntos ni orden. Un prisma no cose nada: arriba y abajo
// son EL MISMO contorno a dos alturas. Así que aquí no hay remuestreo, y con él se van las tres
// cosas que dolían — el contorno que pasaba de 94 a 292 puntos al expandir, los puntos repetidos que
// salían degenerados, y quedarse con «la isla más grande» perdiendo el resto de lo pintado.
//
// 🔑 El truco que lo hace corto: `Triangulation::triangulate(const ExPolygons&)` devuelve índices
// EN EL ESPACIO DE `to_points(area)` —contorno y luego agujeros, expolígono a expolígono
// (ExPolygon.hpp:196)—, así que una sola triangulación sirve para las dos tapas y los agujeros
// entran solos. Nada de un caso especial para las islas ni para los huecos.
static void zone_extrude_prism(const ExPolygons &area, double z_bot, double z_top,
                               indexed_triangle_set &its)
{
    if (area.empty() || z_top <= z_bot)
        return;
    const Points pts = to_points(area);
    if (pts.size() < 3)
        return;
    const std::vector<Vec3i32> cap = Triangulation::triangulate(area);
    if (cap.empty())
        return;

    const int n    = int(pts.size());
    const int up   = int(its.vertices.size());   // el anillo de ARRIBA
    const int dn   = up + n;                     // el de ABAJO, mismos XY
    its.vertices.reserve(its.vertices.size() + size_t(2 * n));
    for (const Point &p : pts)
        its.vertices.emplace_back(float(unscaled<double>(p.x())), float(unscaled<double>(p.y())), float(z_top));
    for (const Point &p : pts)
        its.vertices.emplace_back(float(unscaled<double>(p.x())), float(unscaled<double>(p.y())), float(z_bot));

    for (const Vec3i32 &t : cap) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= n || t[1] >= n || t[2] >= n)
            continue;
        // `Triangulation` devuelve el sentido de la ExPolygon (CCW en XY, normal +Z), que como techo
        // es lo correcto; el suelo va del revés.
        its.indices.emplace_back(up + t[0], up + t[1], up + t[2]);
        its.indices.emplace_back(dn + t[0], dn + t[2], dn + t[1]);
    }

    // 🚨 EL WINDING DE LAS PAREDES ES EL DE s299f, y ahí está escrito por qué: para un contorno
    // antihorario visto desde arriba, esta pareja da la normal EXTERIOR y recorre la arista común
    // con el techo en sentido opuesto, que es lo que hace que emparejen. La versión intuitiva deja
    // 960 aristas abiertas y una malla que parece cerrada.
    //
    // 🔑 Y vale igual para los agujeros sin un solo `if`: un agujero viene en horario, así que la
    // misma fórmula le da también su normal exterior, que mirando desde dentro del hueco es la
    // contraria. Escribir el caso del agujero a mano es escribir la ocasión de equivocarse.
    int off = 0;
    auto wall = [&](const Polygon &ring) {
        const int m = int(ring.points.size());
        for (int i = 0; i < m; ++ i) {
            const int a = off + i, b = off + (i + 1) % m;
            its.indices.emplace_back(up + a, dn + b, up + b);
            its.indices.emplace_back(up + a, dn + a, dn + b);
        }
        off += m;
    };
    for (const ExPolygon &ep : area) {
        wall(ep.contour);
        for (const Polygon &h : ep.holes)
            wall(h);
    }
}

// Todos los tocones en MUNDO, el primario incluido. El primario NO se guarda aparte: es el
// aterrizaje de siempre, sembrado a plomo por `resolve_landing_plumb()` (s300g) y movible con «Move
// it again». Tener una copia suya en `m_extra_stumps` sería exactamente la trampa de los dos
// centroides que este fichero ya documenta.
std::vector<GLGizmoSupportZones::StumpSpot> GLGizmoSupportZones::all_stumps() const
{
    std::vector<StumpSpot> out;
    if (m_has_landing)
        out.push_back(StumpSpot{ m_landing_world_pos, m_landing_on_bed });
    for (const StumpSpot &s : m_extra_stumps)
        out.push_back(s);
    return out;
}

// De dónde a dónde va el prisma de la cabeza. En una función porque lo piden dos sitios —el
// constructor y el aviso del hueco— y porque el `0.4` de abajo es una regla, no un detalle.
bool GLGizmoSupportZones::block_tree_head_z(double &z_bot, double &z_top) const
{
    if (! m_has_target)
        return false;
    const ZoneMask *mk = mask(m_target_facet_idx,
                              Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (mk == nullptr || mk->area.empty())
        return false;
    // El mismo medio milímetro de siempre: el techo del bloque se levanta por encima de la frontera
    // de capa para que la primera capa de contacto caiga dentro y no justo en el borde.
    z_top = mk->z_high + 0.5;
    z_bot = mk->z_low;
    // Un área pintada sobre una cara plana da `z_low == z_high` y el prisma saldría sin altura. Se
    // le da el mínimo para que exista y se pueda rebanar; el motor no necesita más de la cabeza que
    // saber dónde agarra.
    if (z_top - z_bot < 0.4)
        z_bot = z_top - 0.4;
    return true;
}

double GLGizmoSupportZones::block_tree_gap_mm() const
{
    double z_bot = 0., z_top = 0.;
    if (! block_tree_head_z(z_bot, z_top))
        return 0.;
    const std::vector<StumpSpot> stumps = all_stumps();
    if (stumps.empty())
        return 0.;
    double stump_top = - std::numeric_limits<double>::max();
    for (const StumpSpot &st : stumps)
        stump_top = std::max(stump_top, st.p.z() + STUMP_HEIGHT_MM);
    return z_bot - stump_top;
}

// NEOTKO_SUPPORTZONES_TAG s301 — LA CABEZA Y LOS TOCONES, Y NADA EN MEDIO.
//
// 🔑 La cabeza es un PRISMA RECTO (decisión suya en s300: prisma ahora, curva después sólo si el
// preview resulta demasiado tosco). Con eso desaparecen `z_at()`, `n_at()` y el mapa de alturas del
// constructor, y con ellos la proyección ambigua que producía los 42 picos: el techo pasa a ser
// plano, así que su desnivel es CERO por construcción y no «pequeño».
//
// 🔑 Y las dos partes salen SEPARADAS a propósito. Ese hueco no es un descuido de la malla: es el
// dato de entrada del motor. Al rebanar deja capas vacías entre la cabeza y el tocón, y eso es lo
// que `SupportMaterial.cpp` reconoce como árbol de bloques para bajar la columna hacia el tocón.
bool GLGizmoSupportZones::build_block_tree_mesh(TriangleMesh &out_world) const
{
    if (! m_has_target)
        return false;
    const ZoneMask *mk = mask(m_target_facet_idx,
                              Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (mk == nullptr || mk->area.empty())
        return false;
    const std::vector<StumpSpot> stumps = all_stumps();
    if (stumps.empty())
        return false;

    double head_bot = 0., head_top = 0.;
    if (! block_tree_head_z(head_bot, head_top))
        return false;

    // --- la cabeza ------------------------------------------------------------------------------
    // El mando `head` se conserva y sigue significando lo mismo: meter o sacar el borde de lo
    // pintado. Es un `offset_ex`, así que no puede devolver un contorno cruzado — y si te pasas
    // metiendo devuelve VACÍO, que es una respuesta y no un sólido imposible (s299).
    const double     e   = double(m_footprint_shrink_mm);
    const ExPolygons raw = (std::abs(e) < 1e-6)
        ? mk->area
        : offset_ex(mk->area, scaled<float>(- e), ClipperLib::jtRound);
    if (raw.empty())
        return false;

    // 🚨 s301b — Y SE SIMPLIFICA, QUE NO ES COSMÉTICA. Medido en el primer build: la cabeza salía
    // con 28.976 vértices y 57.944 triángulos, porque el contorno de lo pintado lleva una arista por
    // cada triángulo del borde de la malla. El pilar lofteado no lo notaba porque `resample_ring()`
    // le ponía un techo de 512 puntos; el prisma no remuestrea, que es justo su virtud, así que el
    // recuento entra crudo — y esa malla se rebana en cada slice.
    //
    // 🔑 `ExPolygon::simplify()` va por Clipper, así que no puede devolver un contorno cruzado (a
    // diferencia de un Douglas-Peucker a pelo, que sí). 0.05 mm es la misma tolerancia con la que
    // `ring_outline_world()` dibuja el contorno del panel: por debajo de la extrusión, o sea
    // invisible en la pieza y en el preview.
    const ExPolygons head = expolygons_simplify(raw, scaled<double>(0.05));
    if (head.empty())
        return false;

    indexed_triangle_set its;
    zone_extrude_prism(head, head_bot, head_top, its);
    if (its.indices.empty())
        return false;
    const size_t head_tris = its.indices.size();
    // 🔎 El desnivel del techo, MEDIDO y no afirmado. Es el número con el que se juzga si el
    // problema de raíz se fue: hoy, con el techo remuestreado contra el mapa de alturas, son 42 de
    // 512 puntos por encima de 1 mm y el peor salta 7.56 mm. Con el prisma tiene que salir 0.00
    // exacto, y sale de recorrer los vértices de verdad — escribir un cero a mano aquí no probaría
    // nada. Se mide ANTES de soldar, que es cuando el anillo del techo son los primeros n vértices.
    double roof_lo = 1e9, roof_hi = -1e9;
    for (size_t i = 0; i < its.vertices.size() / 2; ++ i) {
        roof_lo = std::min(roof_lo, double(its.vertices[i].z()));
        roof_hi = std::max(roof_hi, double(its.vertices[i].z()));
    }

    // --- los tocones ----------------------------------------------------------------------------
    // Redondos y todos del mismo tamaño: un tocón no es una forma que se dibuje, es un sitio donde
    // se planta el pie. El −0.5 en la cama es el mismo que usa el pilar lofteado, para que la
    // primera capa entre entera.
    size_t n_stumps = 0;
    for (const StumpSpot &st : stumps) {
        const double zb = st.on_bed ? -0.5 : st.p.z();
        const double zt = st.p.z() + STUMP_HEIGHT_MM;
        if (zt - zb < 0.2)
            continue;
        const ExPolygons one { ExPolygon(zone_disc(Vec2d(st.p.x(), st.p.y()),
                                                   0.5 * double(m_stump_size_mm))) };
        zone_extrude_prism(one, zb, zt, its);
        ++ n_stumps;
    }
    if (n_stumps == 0)
        return false;

    // Mismo remate que el pilar lofteado, y por el mismo motivo (s299e): los degenerados no se
    // borran —borrarlos deja aristas huérfanas— se sueldan.
    its_merge_vertices(its, true);
    its_remove_degenerate_faces(its, true);
    its_compactify_vertices(its, true);
    if (its.indices.empty())
        return false;
    const bool flipped = its_volume(its) < 0.f;
    if (flipped)
        its_flip_triangles(its);

    // 🔑 El hueco se lo pregunta al MISMO dueño al que se lo pregunta el panel. Restar aquí
    // `head_bot - stump_top_max` daría el mismo número hoy y dejaría abierta la puerta a que
    // mañana no lo diera: es la trampa de los dos centroides otra vez.
    const double gap = block_tree_gap_mm();

    if (NeoDebug::enabled(NeoDebug::SUPPORTZONES)) {
        const size_t open_edges = its_num_open_edges(its);
        double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
        for (const Vec3f &v : its.vertices) {
            bx0 = std::min(bx0, double(v.x())); bx1 = std::max(bx1, double(v.x()));
            by0 = std::min(by0, double(v.y())); by1 = std::max(by1, double(v.y()));
        }
        // 🔎 El `hueco=` es EL número de esta feature: es lo que separa la cabeza del tocón, y por
        // tanto lo que decide si el motor ve un tramo que guiar o un pilar corriente. Si sale
        // negativo, la cabeza y el tocón se tocan y no hay árbol — se dice aquí y en el panel.
        //
        // Y el `desnivel` tiene que ser 0.00 EXACTO. Con el techo plano ya no es «por debajo de
        // 1 mm»: cualquier otra cosa significa que alguien le devolvió el mapa de alturas.
        char b[320];
        std::snprintf(b, sizeof(b),
                      "cabeza    %s  verts=%d tris=%d (cabeza %d)  aristas_abiertas=%d"
                      "  huella=%.1fx%.1f mm  techo=%.2f..%.2f (desnivel %.2f)"
                      "  tocones=%d ø%.1f  hueco=%.2f mm%s",
                      open_edges == 0 ? "cerrada" : "🚨 MALLA ABIERTA",
                      int(its.vertices.size()), int(its.indices.size()), int(head_tris), int(open_edges),
                      bx1 - bx0, by1 - by0, roof_lo, roof_hi, roof_hi - roof_lo,
                      int(n_stumps), double(m_stump_size_mm), gap,
                      flipped ? "  🚨 el sólido salió del revés y se ha volteado" : "");
        NeoDebug::write(NeoDebug::SUPPORTZONES, b);
        if (gap <= 0.)
            NeoDebug::write(NeoDebug::SUPPORTZONES,
                            "cabeza    🚨 sin hueco entre la cabeza y el tocón: el motor no verá un árbol de "
                            "bloques, bajará como un pilar corriente");
    }

    out_world = TriangleMesh(std::move(its));
    return ! out_world.empty();
}

bool GLGizmoSupportZones::build_pillar_mesh(TriangleMesh &out_world) const
{
    // 🔑 s301 — el despacho, en la primera línea y en un solo sitio. Ver la nota de la cabecera.
    if (block_tree_mode())
        return build_block_tree_mesh(out_world);
    // NEOTKO_SUPPORTZONES_TAG s300h — la forma del eje sale del helper, no de aquí.
    PillarAxis ax;
    if (! pillar_axis(ax))
        return false;
    // El helper ya ha validado la máscara y la huella; aquí se recupera la máscara porque el techo
    // se remuestrea contra ella más abajo.
    const ZoneMask *mk = mask(m_target_facet_idx,
                              Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (mk == nullptr || mk->area.empty())
        return false;

    const double z_low     = ax.z_low;
    const double z_high    = ax.z_high;
    const Vec2d  c_top     = ax.c_top;
    const double z_bot_ext = ax.z_bot_ext;
    // El mismo medio milímetro que usa el helper para levantar el techo del bloque por encima de la
    // frontera de capa. Aquí lo necesitan el `edge_at()` y el remuestreo del techo, más abajo.
    constexpr double ROOF_LIFT_MM = 0.5;

    struct Ring { double z; Vec2d off; };
    std::vector<Ring> rings;
    rings.reserve(ax.rings.size());
    for (const auto &r : ax.rings)
        rings.push_back({ r.first, r.second });

    // 🔑 s286b, idea suya: el borde no tiene por qué valer lo mismo arriba que abajo. Al engordar
    // para agarrar un alero, engordaba también el pie, y ahí no hace ninguna falta — sólo material.
    // Con un valor arriba y otro en el pie, interpolados POR ALTURA, el pilar sale cónico: la rama
    // gorda donde sujeta y fina donde apoya.
    //
    // 🚨 ¿No rompe esto la sección constante del §4-bis.1? No, y el motivo importa: el corredor no
    // mide ÁREA, mide desplazamiento del CENTROIDE. Un crecimiento simétrico deja el centroide
    // donde estaba. Y un `offset_ex` es simétrico por definición, que es una razón más para que sea
    // él quien lo haga y no un desplazamiento por vértice.
    const double z_span = std::max(1e-6, (z_high + ROOF_LIFT_MM) - z_bot_ext);
    const double inset_cap  = max_inset_mm();
    const double outset_cap = max_outset_mm();
    auto edge_at = [&](double z) {
        const double t = std::clamp(((z - z_bot_ext) / z_span), 0., 1.);
        const double v = double(m_footprint_base_mm) + (double(m_footprint_shrink_mm) - double(m_footprint_base_mm)) * t;
        return std::clamp(v, - outset_cap, inset_cap);
    };

    // Cuántos puntos tiene cada anillo. El MISMO para todos, que es lo que permite coser dos
    // contornos que Clipper ha calculado por separado.
    //
    // 🚨 s299e — y adaptado al perímetro, no fijo en 128. Un número fijo es una cuerda de longitud
    // arbitraria: en una huella de 200 mm se salta el detalle, y en una de 3 mm pone puntos más
    // juntos que la precisión del entero y salen repetidos, que es de donde venían los degenerados.
    // Una cuerda de ~0.3 mm es más fina que la extrusión y sigue siendo barata.
    int N_RING = 128;
    {
        double perim = 0.;
        for (const ExPolygon &ep : mk->area) {
            const Points &p = ep.contour.points;
            for (size_t i = 0; i < p.size(); ++ i) {
                const Point &a = p[i], &b = p[(i + 1) % p.size()];
                perim += (Vec2d(unscaled<double>(b.x()), unscaled<double>(b.y())) -
                          Vec2d(unscaled<double>(a.x()), unscaled<double>(a.y()))).norm();
            }
        }
        N_RING = std::clamp(int(std::lround(perim / 0.3)), 24, 512);
    }

    std::vector<std::vector<Vec2d>> ring_pts;
    ring_pts.reserve(rings.size());
    for (size_t ri = 0; ri < rings.size(); ++ ri) {
        const Ring  &r = rings[ri];
        // El anillo del techo se mide a la altura del punto MÁS ALTO de la huella, no a la z que
        // lleva escrita: su z de verdad es una por punto y sale del mapa de alturas más abajo.
        const double e = edge_at((ri == 0) ? (z_high + ROOF_LIFT_MM) : r.z);
        const ExPolygons ring_area = (std::abs(e) < 1e-6)
            ? mk->area
            : offset_ex(mk->area, scaled<float>(- e), ClipperLib::jtRound);
        // 🚨 s299 — AQUÍ ES DONDE SE NOTA EL COLAPSO, y por eso ya no hay que predecirlo. Si te has
        // pasado metiendo, Clipper devuelve vacío y no se construye nada. La versión de antes
        // intentaba adivinar ese punto con una condición local que no veía chocar dos aristas
        // lejanas, y cuando se equivocaba dibujaba un sólido imposible en vez de no dibujar nada.
        std::vector<Vec2d> pts = resample_ring(ring_area, N_RING);
        if (pts.size() != size_t(N_RING))
            return false;
        for (Vec2d &p : pts)
            p += r.off;
        ring_pts.push_back(std::move(pts));
    }

    // La cota del anillo que va justo debajo del techo. El techo sigue la superficie y encima se
    // corrige en Z por el plano tangente, así que puede bajar; si baja por debajo del anillo de
    // abajo, esa pareja de triángulos de pared sale del revés. Aquí es donde se le pone el suelo.
    const double z_under_roof = (rings.size() > 1) ? rings[1].z : z_bot_ext;

    indexed_triangle_set its;
    const int nblocks = int(rings.size());
    its.vertices.reserve(size_t(nblocks) * size_t(N_RING) + size_t(N_RING));

    // --- los vértices de cada anillo ------------------------------------------------------------
    for (int r = 0; r < nblocks; ++ r) {
        for (int i = 0; i < N_RING; ++ i) {
            const Vec2d &p = ring_pts[r][i];
            double z = rings[r].z;
            if (r == 0) {
                // 🔑 Sólo el techo sigue la superficie. Su z sale del mapa de alturas, y la
                // corrección por plano tangente es la respuesta a lo que él vio en s286b:
                // "expande de manera muy lineal x/y y falla en las curvas a tocarlos; tendría que
                // crecer en Z en menor medida, pero sí, para compensar".
                //     dz = -(n.x·Δx + n.y·Δy) / n.z
                // que en una cara horizontal da cero y crece con la pendiente.
                //
                // ⚠️ El mapa se lee en el punto SIN desplazar: el desplazamiento del borde es
                // justamente lo que se está compensando, así que preguntar por la z en el sitio
                // nuevo sería contar el efecto dos veces.
                const Vec2d p_src = p - rings[r].off;
                z = z_at(*mk, p_src) + ROOF_LIFT_MM;
                const double e = edge_at(z_high + ROOF_LIFT_MM);
                if (std::abs(e) > 1e-9) {
                    const Vec3d n = n_at(*mk, p_src);
                    if (std::abs(n.z()) > 1e-3) {
                        // La dirección en la que se movió este punto, que es hacia dónde apunta el
                        // offset: del contorno sin offset al de con offset. Basta su componente
                        // radial respecto al centro, que es lo que el plano tangente necesita.
                        const Vec2d d = p_src - c_top;
                        const double dl = d.norm();
                        if (dl > 1e-9) {
                            const Vec2d u = (d / dl) * (- e);   // e>0 = meter ⇒ hacia dentro
                            z += - (n.x() * u.x() + n.y() * u.y()) / n.z();
                        }
                    }
                }
                z = std::max(z, z_under_roof + 0.05);
            }
            its.vertices.emplace_back(float(p.x()), float(p.y()), float(z));
        }
    }
    const int bot_base = (nblocks - 1) * N_RING;

    // --- las tapas ------------------------------------------------------------------------------
    // 🔑 Una sola triangulación, la del contorno del techo ya remuestreado, y sirve para las dos
    // tapas. Al usar los MISMOS puntos que lleva la pared, la tapa cierra sin costuras y sin tener
    // que soldar nada por posición, que era otro `std::map` por reconstrucción.
    {
        Points cap;
        cap.reserve(N_RING);
        for (int i = 0; i < N_RING; ++ i)
            cap.emplace_back(scaled<coord_t>(ring_pts[0][i].x()), scaled<coord_t>(ring_pts[0][i].y()));
        Polygon cap_poly(std::move(cap));
        // 🚨 s299e — SE EXIGE CCW, no se reordena. La versión de antes invertía el contorno para
        // triangular y luego recuperaba los índices leyéndolos al revés (`N_RING-1-t[k]`). Funciona
        // sólo mientras `reverse()` sea exactamente eso, y una tapa mal indexada no se ve: sale una
        // malla que se cruza por dentro y nadie sabe de dónde viene.
        //
        // `resample_ring()` ya devuelve el anillo en sentido antihorario, así que aquí no puede
        // llegar del revés. Si llegara, es un fallo de invariante y se dice, en vez de apañarlo.
        if (! cap_poly.is_counter_clockwise()) {
            NeoDebug::write(NeoDebug::SUPPORTZONES,
                            "pillar    🚨 el anillo del techo llego en sentido horario: resample_ring deberia darlo CCW");
            return false;
        }
        const std::vector<Vec3i32> idx = Triangulation::triangulate(ExPolygon(cap_poly));
        if (idx.empty())
            return false;
        for (const Vec3i32 &t : idx) {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 ||
                t[0] >= N_RING || t[1] >= N_RING || t[2] >= N_RING)
                continue;
            // `Triangulation` devuelve el sentido de la ExPolygon (CCW en XY, normal +Z), que como
            // techo del sólido es lo correcto; el suelo va del revés.
            its.indices.emplace_back(t[0], t[1], t[2]);                                       // techo
            its.indices.emplace_back(bot_base + t[0], bot_base + t[2], bot_base + t[1]);       // suelo
        }
    }

    // --- las paredes ----------------------------------------------------------------------------
    // 🚨 s299e — sin banda entre dos anillos que están en el mismo sitio. Una banda de altura y
    // desplazamiento cero son N pares de triángulos degenerados, que es justo lo que ya no queremos
    // ni crear ni borrar. Puede pasar de verdad: el anillo M está en `z_low` y el techo puede
    // acabar ahí mismo en una superficie plana.
    for (int blk = 0; blk + 1 < nblocks; ++ blk) {
        if (blk > 0
            && std::abs(rings[blk + 1].z - rings[blk].z) < 1e-6
            && (rings[blk + 1].off - rings[blk].off).squaredNorm() < 1e-12)
            continue;
        const int lo = blk * N_RING, hi = (blk + 1) * N_RING;
        for (int i = 0; i < N_RING; ++ i) {
            const int a = i, b = (i + 1) % N_RING;
            // 🚨 s299f — ESTE WINDING ESTABA DEL REVÉS, Y ERA LA CAUSA DE LAS 960 ARISTAS ABIERTAS.
            //
            // La cuenta lo señala sin margen de duda: el log daba `aristas_abiertas` = `verts` =
            // anillos × puntos, o sea TODAS las aristas de anillo. Eso no es un caso raro de una
            // geometría concreta, es un fallo de orientación en todas.
            //
            // El porqué, con los vectores en la mano. Para un contorno antihorario visto desde
            // arriba, la normal exterior de una arista `d` es `(d.y, -d.x)`. La pareja que había
            // —(lo+a, lo+b, hi+b)— da como normal `d × (0,0,-h) ∝ (-d.y, d.x)`, que es la INTERIOR.
            // Con las paredes mirando hacia dentro y las tapas hacia fuera, `its_volume()` salía
            // negativo y el volteo global de más abajo le daba la vuelta a todo, dejando la malla
            // consistente en apariencia y con las mismas aristas sin emparejar.
            //
            // 🔑 Y hay un segundo motivo, el que rompía el emparejamiento: dos caras vecinas con
            // normal exterior tienen que recorrer su arista común en sentidos OPUESTOS. El techo,
            // antihorario, recorre `a→b`; la pared de antes también `a→b`, así que no emparejaban
            // ni siendo ambas correctas. Intercambiando los dos últimos índices sale `b→a`.
            its.indices.emplace_back(lo + a, hi + b, lo + b);
            its.indices.emplace_back(lo + a, hi + a, hi + b);
        }
    }

    // 🚨 s299e — AQUÍ SE BORRABAN LOS TRIÁNGULOS DE ÁREA CERO, Y ERA LA CAUSA DE LAS MALLAS ROTAS.
    //
    // El razonamiento de entonces sonaba bien: "a un visor le dan igual, pero a un booleano no". El
    // fallo es lo que pasa AL BORRARLOS. Un triángulo degenerado sigue teniendo tres aristas, y esas
    // tres aristas emparejan con las de sus vecinos; quitarlo de la lista deja tres aristas
    // huérfanas, o sea tres agujeros. Por eso el pilar "siempre salía non-manifold": no era la
    // geometría, era la limpieza.
    //
    // 🔑 Ahora los degenerados no se borran: se EVITAN antes de existir (el remuestreo ya no
    // devuelve puntos repetidos, y dos anillos con la misma altura y el mismo offset no generan
    // banda), y lo que quede se resuelve soldando vértices, que es una operación que mantiene la
    // malla cerrada porque une en vez de quitar.
    its_merge_vertices(its, true);
    its_remove_degenerate_faces(its, true);
    its_compactify_vertices(its, true);
    if (its.indices.empty())
        return false;

    // Red de seguridad barata: si el sólido sale del revés se voltea entero, en vez de entregar una
    // malla con las normales hacia dentro.
    if (its_volume(its) < 0.f)
        its_flip_triangles(its);

    // 🔎 s299e — y se DICE si ha salido abierta, en vez de dejarlo para que lo descubra un booleano
    // tres sesiones después. Cero aristas abiertas es la definición de cerrada; cualquier otra cosa
    // es un pilar que se va a comportar raro y aquí es donde se ve.
    if (NeoDebug::enabled(NeoDebug::SUPPORTZONES)) {
        const size_t open_edges = its_num_open_edges(its);
        char b[256];
        // 🔎 s299g — y la HUELLA, que es lo que de verdad se quiere mirar cuando algo sale raro.
        //
        // El .obj de su último informe lo demostró: la malla decía `cerrada` y aun así el resultado
        // era una lámina absurda. Midiéndolo, la huella ocupaba 31 × 82 mm — una banda que cruzaba
        // el agujero del toro de lado a lado. El problema nunca estuvo en la construcción del
        // sólido, estuvo en lo que se le pidió construir, y ese número no estaba en ninguna parte.
        //
        // Con el ancho, el largo y el desnivel del techo aquí, "esto no es lo que pinté" se ve en
        // una línea en vez de exportando un .obj y midiéndolo a mano.
        double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
        for (const Vec3f &v : its.vertices) {
            bx0 = std::min(bx0, double(v.x())); bx1 = std::max(bx1, double(v.x()));
            by0 = std::min(by0, double(v.y())); by1 = std::max(by1, double(v.y()));
        }
        std::snprintf(b, sizeof(b),
                      "pillar    %s  verts=%d tris=%d anillos=%d pts/anillo=%d  aristas_abiertas=%d"
                      "  huella=%.1fx%.1f mm  techo=%.2f..%.2f (desnivel %.2f)",
                      open_edges == 0 ? "cerrada" : "🚨 MALLA ABIERTA",
                      int(its.vertices.size()), int(its.indices.size()), nblocks, N_RING, int(open_edges),
                      bx1 - bx0, by1 - by0, z_low, z_high, z_high - z_low);
        NeoDebug::write(NeoDebug::SUPPORTZONES, b);
    }

    out_world = TriangleMesh(std::move(its));
    return ! out_world.empty();
}

void GLGizmoSupportZones::update_preview()
{
    m_preview_dirty = false;
    m_preview_model.reset();
    TriangleMesh mesh;
    if (! build_pillar_mesh(mesh))
        return;
    m_preview_model.init_from(mesh.its);
    m_preview_model.set_color(ColorRGBA(0.30f, 0.55f, 0.95f, 0.45f));
}

void GLGizmoSupportZones::render_preview()
{
    // 🔑 s299 — MIENTRAS SE PINTA NO SE CONSTRUYE EL SÓLIDO, y es la mitad de la sensación de
    // fluidez que él pedía. Levantar el pilar es triangular, cuatro offsets y coser las paredes;
    // hacerlo en cada marca del pincel es pagarlo cincuenta veces por un trazo del que sólo importa
    // el final. Lo que sí se dibuja mientras tanto es la máscara sobre la superficie (el resaltado
    // de `build_face_model`), que es lo que uno mira cuando está pintando.
    //
    // El sólido aparece al soltar el botón, que es exactamente cuando uno quiere verlo.
    if (m_painting)
        return;
    if (m_preview_dirty)
        update_preview();
    if (! m_preview_model.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera &camera = wxGetApp().plater()->get_camera();
    // The preview mesh is already in world coordinates, so no model matrix of its own.
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_preview_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

double GLGizmoSupportZones::lean_top_z() const
{
    if (! m_has_target)
        return 0.;
    // Rellena la caché si hace falta; el z más bajo sale del mismo barrido que la huella.
    if (target_footprint_world().empty())
        return m_target_world_pos.z();
    // A patch that reports nothing is a patch with no region: fall back to the click, which is on
    // it by construction.
    return (m_foot_cache_z_low == std::numeric_limits<double>::max()) ? m_target_world_pos.z()
                                                                     : m_foot_cache_z_low;
}

double GLGizmoSupportZones::support_line_width_mm() const
{
    // support_line_width is a FloatOrPercent over nozzle_diameter, and 0 means "use the default",
    // which the engine resolves from the nozzle. Resolved the same way here so the strip is not
    // quietly computed from a different width than the brake.
    //
    // 🚨 nozzle_diameter lives in the PRINTER preset, not the print one. Reading it off the print
    // config would silently return nothing and leave the fallback in place, which is the kind of
    // wrong number that never announces itself.
    const DynamicPrintConfig &cfg  = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    double nozzle = 0.4;
    if (const auto *nd = pcfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr && ! nd->values.empty())
        nozzle = nd->values.front();
    double line_w = cfg.has("support_line_width") ? cfg.get_abs_value("support_line_width", nozzle) : 0.;
    if (line_w <= 0.)
        line_w = nozzle * 1.05;
    return line_w;
}

double GLGizmoSupportZones::worst_layer_height_mm() const
{
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double layer_h = cfg.has("layer_height") ? cfg.opt_float("layer_height") : 0.2;

    // 🚨 El tope del corredor es un desplazamiento POR CAPA, así que con altura variable la capa
    // que manda es la MÁS GRUESA: con el mismo ángulo pide más desplazamiento que la nominal.
    // Prometer un ángulo calculado sobre la capa fina y pasarse en la gorda es exactamente la
    // mentira que esta feature existe para no contar.
    const ModelObject *mo = current_object();
    if (mo != nullptr && mo->has_custom_layering()) {
        const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        if (const auto *mh = pcfg.option<ConfigOptionFloats>("max_layer_height"); mh != nullptr && ! mh->values.empty()) {
            // 0 significa "sin tope propio, usa el 75% de la boquilla", que es como lo resuelve el
            // backend; un 0 leído tal cual daría una capa de cero y con ella un ángulo infinito.
            double mx = mh->values.front();
            if (mx <= 0.) {
                double nozzle = 0.4;
                if (const auto *nd = pcfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr && ! nd->values.empty())
                    nozzle = nd->values.front();
                mx = 0.75 * nozzle;
            }
            layer_h = std::max(layer_h, mx);
        }
    }
    return layer_h;
}

double GLGizmoSupportZones::max_lean_angle_deg() const
{
    return SupportZones::corridor_max_angle_deg(worst_layer_height_mm(), support_line_width_mm());
}

double GLGizmoSupportZones::reach_radius_mm(double z_landing) const
{
    if (! m_has_target)
        return 0.;

    // 🔑 s286b — el alcance pasa a ser CONSECUENCIA del ángulo, no un aviso a posteriori. Antes
    // esto contaba pasos (`corridor_reach_mm`) y decía cuánto se podía llegar a desplazar en total;
    // ahora el pilar se inclina a un ángulo elegido, así que lo que se puede recorrer es pura
    // trigonometría sobre la altura disponible.
    //
    // 🔑 En el ángulo MÁXIMO las dos fórmulas dan el mismo número — `corridor_reach_mm` es
    // (altura/dz)·paso y `tan(max) = paso/dz` — así que el disco conserva su significado exacto y
    // ahora además encoge al bajar el ángulo. `corridor_reach_mm()` sigue siendo el dueño del techo
    // absoluto: si algún día los dos discrepan en el ángulo máximo, uno de los dos está mal.
    //
    // 🚨 Y la altura que cuenta es donde EMPIEZA la inclinación, no donde pinchaste: el sólido es
    // vertical desde el techo hasta el punto más bajo del parche (build_pillar_mesh). Medir desde
    // el clic regalaría un presupuesto que el tramo inclinado no tiene.
    const double h = lean_top_z() - z_landing;
    if (h <= 0.)
        return 0.;
    constexpr double deg2rad = 0.01745329251994329576923690768489;
    return h * std::tan(m_lean_angle_deg * deg2rad);
}

double GLGizmoSupportZones::landing_offset_mm() const
{
    if (! m_has_target || ! m_has_landing)
        return 0.;
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.empty())
        return 0.;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    c /= double(foot.size());
    return (Vec2d(m_landing_world_pos.x(), m_landing_world_pos.y()) - c).norm();
}

bool GLGizmoSupportZones::landing_out_of_reach() const
{
    if (! m_has_target || ! m_has_landing)
        return false;
    // s301 — con varios tocones la pregunta es «¿alguno queda fuera de mi presupuesto?». Misma
    // fórmula, un `for` por encima: el alcance de cada tocón se mide a SU altura, porque el techo
    // de desplazamiento sale de la altura que queda por bajar y ésa cambia con cada tocón.
    //
    // ⚠️ Es un aviso, no un veto: el motor recorta a `d_max` por capa y la isla que no llegue se
    // dice en `wavesupport.log` con su capa y su área («esta raíz no da para todo»). Aquí sólo se
    // adelanta antes de crear nada.
    if (block_tree_mode()) {
        const std::vector<Vec2d> foot = target_footprint_world();
        if (foot.empty())
            return false;
        Vec2d c { 0., 0. };
        for (const Vec2d &p : foot)
            c += p;
        c /= double(foot.size());
        for (const StumpSpot &st : all_stumps())
            if ((Vec2d(st.p.x(), st.p.y()) - c).norm() > reach_radius_mm(st.p.z()) + 1e-6)
                return true;
        return false;
    }
    return landing_offset_mm() > reach_radius_mm(m_landing_world_pos.z()) + 1e-6;
}

void GLGizmoSupportZones::bring_landing_into_reach()
{
    if (! landing_out_of_reach())
        return;
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.empty())
        return;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    c /= double(foot.size());

    Vec2d d = Vec2d(m_landing_world_pos.x(), m_landing_world_pos.y()) - c;
    const double n = d.norm();
    if (n < 1e-9)
        return;
    // Keep the direction the user chose and only shorten it. Snapping to somewhere else would be
    // the tool overruling a decision instead of correcting one.
    d *= reach_radius_mm(m_landing_world_pos.z()) / n;
    m_landing_world_pos.x() = c.x() + d.x();
    m_landing_world_pos.y() = c.y() + d.y();
    m_preview_dirty = true;
    m_parent.set_as_dirty();
}

void GLGizmoSupportZones::update_footprint_model()
{
    m_footprint_model_dirty = false;
    m_footprint_model.reset();
    if (! m_has_target || ! m_has_landing)
        return;

    // NEOTKO_SUPPORTZONES_TAG s301 — LA SOMBRA DE LOS TOCONES, Y LA LÍNEA QUE LOS UNE AL ÁREA.
    //
    // 🔑 Es la misma respuesta a la misma queja de s286b —«cuesta clavar dónde caerá y toca girar la
    // cama»— con varios pies en vez de uno. El disco dice dónde se planta cada tocón; la cinta dice
    // a qué trozo del área va, que es la pregunta nueva que trae tener más de uno.
    if (block_tree_mode()) {
        const std::vector<StumpSpot> stumps = all_stumps();
        const std::vector<Vec2d>     top    = target_footprint_world();
        if (stumps.empty() || top.empty())
            return;
        Vec2d c { 0., 0. };
        for (const Vec2d &p : top)
            c += p;
        c /= double(top.size());

        indexed_triangle_set its;
        const double r    = 0.5 * double(m_stump_size_mm);
        const int    segs = 32;
        for (const StumpSpot &st : stumps) {
            const double z = st.p.z() + 0.05;   // un pelo sobre la cama, no dentro de ella
            const int    b = int(its.vertices.size());
            its.vertices.emplace_back(float(st.p.x()), float(st.p.y()), float(z));
            for (int i = 0; i < segs; ++ i) {
                const double a = 2.0 * PI_D * double(i) / double(segs);
                its.vertices.emplace_back(float(st.p.x() + r * std::cos(a)),
                                          float(st.p.y() + r * std::sin(a)), float(z));
            }
            for (int i = 0; i < segs; ++ i)
                its.indices.emplace_back(b, b + 1 + i, b + 1 + (i + 1) % segs);

            // La cinta al centroide del área, estrecha a propósito: es una indicación de a dónde va
            // esa raíz, no una promesa de por dónde bajará. Por dónde baja lo decide el motor.
            const Vec2d d = c - Vec2d(st.p.x(), st.p.y());
            const double dl = d.norm();
            if (dl < 1e-6)
                continue;
            const Vec2d n(-d.y() / dl * 0.3, d.x() / dl * 0.3);
            const int   q = int(its.vertices.size());
            its.vertices.emplace_back(float(st.p.x() + n.x()), float(st.p.y() + n.y()), float(z));
            its.vertices.emplace_back(float(st.p.x() - n.x()), float(st.p.y() - n.y()), float(z));
            its.vertices.emplace_back(float(c.x() - n.x()),    float(c.y() - n.y()),    float(z));
            its.vertices.emplace_back(float(c.x() + n.x()),    float(c.y() + n.y()),    float(z));
            its.indices.emplace_back(q, q + 1, q + 2);
            its.indices.emplace_back(q, q + 2, q + 3);
        }
        if (! its.indices.empty())
            m_footprint_model.init_from(its);
        return;
    }

    // El contorno DEL PIE (no el de arriba: con el borde progresivo no son el mismo) llevado al
    // sitio donde aterriza.
    const std::vector<Vec2d> foot = foot_outline_world();
    if (foot.size() < 3)
        return;
    const std::vector<Vec2d> top = target_footprint_world();
    if (top.empty())
        return;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : top)
        c += p;
    c /= double(top.size());
    const Vec2d shift = Vec2d(m_landing_world_pos.x(), m_landing_world_pos.y()) - c;
    const double z = m_landing_world_pos.z() + 0.05;   // un pelo sobre la cama, no dentro de ella

    Points pts;
    pts.reserve(foot.size());
    for (const Vec2d &p : foot) {
        const Vec2d w = p + shift;
        pts.emplace_back(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
    }
    Polygon ring(std::move(pts));
    if (! ring.is_counter_clockwise())
        ring.reverse();
    const std::vector<Vec3i32> tri = Triangulation::triangulate(ring);
    if (tri.empty())
        return;

    indexed_triangle_set its;
    its.vertices.reserve(ring.points.size());
    for (const Point &p : ring.points)
        its.vertices.emplace_back(float(unscaled<double>(p.x())), float(unscaled<double>(p.y())), float(z));
    its.indices = tri;
    m_footprint_model.init_from(its);
}

void GLGizmoSupportZones::update_reach_model()
{
    m_reach_model.reset();
    m_reach_model_r = -1.;
    if (! m_has_target)
        return;

    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.empty())
        return;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    c /= double(foot.size());

    // The strip is drawn at the height the landing is currently at, so it answers the question the
    // user is actually asking ("can I put it HERE?") and not an abstract one.
    const double z = m_has_landing ? m_landing_world_pos.z() : 0.;
    const double r = reach_radius_mm(z);
    if (r <= 0.)
        return;

    m_reach_model_z = z;
    m_reach_model_r = r;
    m_reach_model_c = c;

    // A flat disc, one fan, lifted a hair so it does not fight the bed or the shelf it lies on.
    const int    segments = 96;
    const double lift     = 0.05;
    indexed_triangle_set its;
    its.vertices.reserve(segments + 1);
    its.vertices.emplace_back(float(c.x()), float(c.y()), float(z + lift));
    for (int i = 0; i < segments; ++ i) {
        const double a = Geometry::deg2rad(360.0 * double(i) / double(segments));
        its.vertices.emplace_back(float(c.x() + r * std::cos(a)), float(c.y() + r * std::sin(a)), float(z + lift));
    }
    for (int i = 0; i < segments; ++ i)
        its.indices.emplace_back(0, 1 + i, 1 + (i + 1) % segments);

    m_reach_model.init_from(its);
    m_reach_model.set_color(kReachCol);
}

void GLGizmoSupportZones::render_reach()
{
    if (! m_has_target)
        return;
    // Rebuilt only when the geometry it describes actually moved.
    const double z = m_has_landing ? m_landing_world_pos.z() : 0.;
    if (m_reach_model_r < 0. || std::abs(z - m_reach_model_z) > 1e-6 || m_preview_dirty)
        update_reach_model();
    if (! m_reach_model.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_reach_model.set_color(landing_out_of_reach() ? kReachBadCol : kReachCol);
    m_reach_model.render();

    // La silueta del pie, encima de la franja y bastante más sólida: la franja dice DÓNDE PUEDES,
    // la silueta dice DÓNDE CAE. Con las dos del mismo tono había que girar la cama para saberlo.
    if (m_preview_dirty || m_footprint_model_dirty)
        update_footprint_model();
    if (m_footprint_model.is_initialized()) {
        m_footprint_model.set_color(kFootCol);
        m_footprint_model.render();
    }

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

double GLGizmoSupportZones::min_footprint_width_mm() const
{
    const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    double nozzle = 0.4;
    if (const auto *nd = pcfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr && ! nd->values.empty())
        nozzle = nd->values.front();
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double line_w = cfg.has("support_line_width") ? cfg.get_abs_value("support_line_width", nozzle) : 0.;
    if (line_w <= 0.)
        line_w = nozzle * 1.05;
    // Two extrusions. One is a wall that will not hold anything and that the grid projection is
    // very likely to remove on the way down.
    return 2.0 * line_w;
}

std::vector<Vec2d> GLGizmoSupportZones::foot_outline_world() const
{
    // s301 — en el árbol de bloques el pie es el TOCÓN. Devolver aquí el anillo de abajo del pilar
    // sería enseñar en el alzado un pie que no se construye, que es justo el fallo que la nota de
    // s289 documenta un párrafo más abajo, cometido otra vez con otra forma.
    if (block_tree_mode()) {
        std::vector<Vec2d> out;
        if (! m_has_landing)
            return out;
        const double r    = 0.5 * double(m_stump_size_mm);
        const int    segs = 32;
        out.reserve(segs);
        for (int i = 0; i < segs; ++ i) {
            const double a = 2.0 * PI_D * double(i) / double(segs);
            out.emplace_back(m_landing_world_pos.x() + r * std::cos(a),
                             m_landing_world_pos.y() + r * std::sin(a));
        }
        return out;
    }
    // ✅ s289 — la silueta del anillo DE ABAJO, sacada del mismo parche que el sólido. Antes esto
    // era el contorno de arriba pasado otra vez por `offset()` de Clipper, y eso ya son dos
    // operaciones distintas sobre dos formas distintas: el pie que se comprobaba (¿cabe en la cama?
    // ¿es demasiado estrecho?) no era el pie que se imprimía.
    return ring_outline_world(double(m_footprint_base_mm));
}

bool GLGizmoSupportZones::footprint_too_narrow() const
{
    // s301 — en el árbol de bloques el pie es el TOCÓN, no el anillo de abajo del pilar. Y como el
    // tocón es un disco de un tamaño que se elige con un mando, la pregunta se contesta con el
    // número directamente: nada de rehacer un `offset` para medir el ancho de un círculo.
    if (block_tree_mode())
        return double(m_stump_size_mm) < min_footprint_width_mm();
    const std::vector<Vec2d> foot = foot_outline_world();
    if (foot.size() < 3)
        return true;
    Points pts;
    pts.reserve(foot.size());
    for (const Vec2d &p : foot)
        pts.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    const Polygon poly(std::move(pts));
    // Shrink by half the minimum width: if nothing survives, the shape was never that wide.
    // Cheaper and more honest than an inradius formula, and it is the same operation the engine
    // uses on these polygons anyway.
    const Polygons shrunk = offset(Polygons{ poly }, - scaled<float>(float(min_footprint_width_mm() * 0.5)));
    return shrunk.empty();
}

bool GLGizmoSupportZones::landing_off_bed() const
{
    // A landing on a shelf of the part stops there (§4.2-bis, `Stop`), so it never reaches the
    // plate and the plate has no say.
    //
    // 🚨 s301 — y con varios tocones esa condición ya no puede mirar SÓLO al primario: el primario
    // puede estar sobre una repisa y un tocón adicional caerse de la cama. La pregunta «¿alguno se
    // posa en la cama?» se resuelve abajo, tocón a tocón.
    if (! m_has_target || ! m_has_landing)
        return false;
    if (! block_tree_mode() && ! m_landing_on_bed)
        return false;

    const BuildVolume &bv = wxGetApp().plater()->build_volume();
    if (! bv.valid())
        return false;
    const Polygon &bed = bv.polygon();
    if (bed.points.size() < 3)
        return false;

    // s301 — en el árbol de bloques se pregunta por CADA tocón, en su sitio y con su radio. Aquí no
    // hay desplazamiento que aplicar —el tocón ya está donde está— y aplicarle el del pilar
    // lofteado movería un pie que no existe.
    if (block_tree_mode()) {
        const double r = 0.5 * double(m_stump_size_mm);
        for (const StumpSpot &st : all_stumps()) {
            if (! st.on_bed)
                continue;   // el que se posa en una repisa no llega nunca a la cama
            for (int i = 0; i < 16; ++ i) {
                const double a = 2.0 * PI_D * double(i) / 16.0;
                const Vec2d  w(st.p.x() + r * std::cos(a), st.p.y() + r * std::sin(a));
                if (! bed.contains(Point(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()))))
                    return true;
            }
        }
        return false;
    }

    // The foot is the same ring as the roof, translated (build_pillar_mesh) — same polygon top and
    // bottom is what makes the section constant. Asking about the ring rather than the click point
    // is the difference between "your cursor is on the plate" and "the pillar fits on the plate":
    // a wide foot can hang over the edge with its centre well inside.
    const std::vector<Vec2d> foot = foot_outline_world();
    if (foot.empty())
        return false;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    c /= double(foot.size());
    const Vec2d shift = Vec2d(m_landing_world_pos.x(), m_landing_world_pos.y()) - c;

    // 🚨 The build volume is in the same world frame the GLVolumes live in — it is the frame
    // GLCanvas3D::check_outside_state() compares against — so no conversion, and any conversion
    // invented here would be the bug.
    for (const Vec2d &p : foot) {
        const Vec2d w = p + shift;
        if (! bed.contains(Point(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()))))
            return true;
    }
    return false;
}

SupportType GLGizmoSupportZones::effective_support_type() const
{
    // Mismo criterio que `support_style_blocks_corridor()`: el override del OBJETO manda sobre el
    // preset, porque así es como `PrintObjectConfig` se construye abajo.
    if (const ModelObject *mo = current_object(); mo != nullptr && mo->config.has("support_type"))
        return mo->config.get().opt_enum<SupportType>("support_type");
    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (! cfg.has("support_type"))
        return stNormalAuto;
    return cfg.opt_enum<SupportType>("support_type");
}

bool GLGizmoSupportZones::support_style_blocks_corridor() const
{
    // 🚨 El objeto manda sobre el preset, igual que abajo: `PrintObjectConfig` se construye
    // aplicando el override del ModelObject encima del preset, así que preguntarle sólo al preset
    // haría que el panel siguiera avisando de un Grid que ya no existe — y peor, que dejara de
    // avisar de un Grid puesto a mano en el objeto.
    if (const ModelObject *mo = current_object(); mo != nullptr && mo->config.has("support_style"))
        return mo->config.get().opt_enum<SupportMaterialStyle>("support_style") != smsSnug;

    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (! cfg.has("support_style"))
        return false;
    return cfg.opt_enum<SupportMaterialStyle>("support_style") != smsSnug;
}

bool GLGizmoSupportZones::pillar_crosses_object() const
{
    // NEOTKO_SUPPORTZONES_TAG s301 — SE JUBILA EN EL ÁRBOL DE BLOQUES, Y NO POR COMODIDAD.
    //
    // 🔑 Esta función existe porque un pilar lofteado es un sólido dibujado de una vez, de arriba
    // abajo, y hay que interrogarlo con rayos para saber si atraviesa la pieza. El árbol de bloques
    // no se dibuja: el tramo del medio lo construye el motor capa a capa, y cada capa se recorta
    // contra `offset(layer.lslices, gap_xy)` antes de imprimirse. O sea que no PUEDE meterse en la
    // pieza — la pregunta deja de tener respuesta que dar porque deja de tener sentido.
    //
    // 🚨 Y esto es lo que el §4-bis.8 A1 del plan pedía desde s265, resuelto por construcción en vez
    // de con la sonda de 9×19 rayos que costó s300h y s300i.
    if (block_tree_mode())
        return false;
    if (! m_has_target || ! m_has_landing || ! m_raycaster)
        return false;
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.empty())
        return false;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    c /= double(foot.size());

    // NEOTKO_SUPPORTZONES_TAG s300h — SE SIGUE LA RODILLA, NO LA RECTA ENTRE LOS EXTREMOS.
    //
    // 🚨 Esto es lo que hacía saltar el aviso con un pilar que a la vista no tocaba nada. El pilar
    // baja a plomo del techo a M, se inclina de M a K, y vuelve a plomo de K al pie: una ele. La
    // recta que une el techo con el pie corta por DENTRO de esa ele, y por ese atajo sí que pasa la
    // pared de la pieza. Se estaba preguntando por sitios donde el pilar no está.
    //
    // Con el eje verdadero, un pilar que rodea la pared contesta que no cruza, que es lo que se ve.
    PillarAxis ax;
    if (! pillar_axis(ax) || ax.rings.size() < 2)
        return false;

    const Vec3d top(c.x() + ax.rings.front().second.x(),
                    c.y() + ax.rings.front().second.y(),
                    ax.rings.front().first);
    const Vec3d bot(c.x() + ax.rings.back().second.x(),
                    c.y() + ax.rings.back().second.y(),
                    ax.rings.back().first);

    // NEOTKO_SUPPORTZONES_TAG s300g — la respuesta de este frame, si nada ha cambiado.
    if (m_cross_cache_valid && m_cross_cache_npts == foot.size()
        && m_cross_cache_head == m_footprint_shrink_mm
        && m_cross_cache_foot == m_footprint_base_mm
        && (m_cross_cache_top - top).squaredNorm() < 1e-12
        && (m_cross_cache_bot - bot).squaredNorm() < 1e-12)
        return m_cross_cache_value;

    // NEOTKO_SUPPORTZONES_TAG s300g — SE MIDE EL CUERPO, NO SÓLO EL EJE.
    //
    // 🚨 Esto miraba únicamente la línea del centro, y con una sección de 23 × 16 mm un eje que pasa
    // rozando la pared por fuera contesta "no cruza" con medio pilar metido dentro. Medido en s300g:
    // el pilar atravesaba el toro de lado a lado y este aviso no saltó.
    //
    // 🔑 El arreglo es barato porque la huella ya está calculada: se llevan unos cuantos puntos de
    // su CONTORNO por la misma trayectoria, además del centroide. Sigue siendo el mismo test de
    // dentro/fuera por paridad, sólo que ahora sobre el bulto y no sobre una raya.
    //
    // ⚠️ Frontera de patente (§8 NEVER, PATENT_US9524357_ANALYSIS.md): esto NO cae en la
    // reivindicación 1 — no se calcula la normal en el punto de impacto ni se decide necesidad de
    // soporte por el ángulo rayo↔normal; es un contador de cruces para un aviso. Si algún día hace
    // falta más fino, el camino que pide el plan (§4-bis.8 A1) es la intersección 2D por capa, NO
    // más rayos.
    std::vector<Vec2d> probes;
    probes.reserve(9);
    probes.push_back(c);
    {
        // Ocho puntos repartidos por el contorno. Más no compra nada: lo que se busca es un túnel,
        // no un roce, y un túnel lo cruzan todos.
        const size_t n_ring = 8;
        const size_t step   = std::max<size_t>(1, foot.size() / n_ring);
        for (size_t i = 0; i < foot.size() && probes.size() <= n_ring; i += step)
            probes.push_back(foot[i]);
    }

    m_cross_cache_top   = top;
    m_cross_cache_bot   = bot;
    m_cross_cache_npts  = foot.size();
    m_cross_cache_head  = m_footprint_shrink_mm;
    m_cross_cache_foot  = m_footprint_base_mm;
    m_cross_cache_valid = true;
    m_cross_cache_value = false;

    // El eje como polilínea, de arriba abajo, y su longitud para poder recorrerlo a paso constante.
    std::vector<Vec3d> axis;
    axis.reserve(ax.rings.size());
    for (const auto &r : ax.rings)
        axis.emplace_back(c.x() + r.second.x(), c.y() + r.second.y(), r.first);
    double axis_len = 0.;
    for (size_t i = 1; i < axis.size(); ++ i)
        axis_len += (axis[i] - axis[i - 1]).norm();
    if (axis_len < 1e-6)
        return false;

    // 🔑 Both ends touch geometry ON PURPOSE — the top is under the surface being held up, the
    // bottom may rest on a shelf — so the ends are skipped and only the span in between is asked
    // about. Without that the answer would be "yes, always".
    const Transform3d inv  = m_world_trafo.inverse();
    const AABBMesh   &tree = m_raycaster->get_aabb_mesh();
    const int         n_samples = 24;
    size_t            probes_inside = 0;
    for (const Vec2d &probe : probes) {
        // Cada sonda recorre el MISMO eje quebrado, desplazada a su punto de la huella. La sección
        // del pilar es la misma en todos los anillos (los offsets del borde son simétricos y no
        // mueven el centroide), así que trasladar la huella a lo largo del eje describe el cuerpo.
        const Vec2d d = probe - c;
        int inside = 0;
        for (int i = 1; i < n_samples; ++ i) {
            const double t = double(i) / double(n_samples);
            if (t < 0.10 || t > 0.90)
                continue;
            // Punto a distancia t del recorrido, siguiendo los tramos de la polilínea.
            double want = t * axis_len;
            Vec3d  w    = axis.back();
            for (size_t k = 1; k < axis.size(); ++ k) {
                const double seg = (axis[k] - axis[k - 1]).norm();
                if (want <= seg || k + 1 == axis.size()) {
                    w = axis[k - 1] + (axis[k] - axis[k - 1]) * (seg > 1e-9 ? (want / seg) : 0.);
                    break;
                }
                want -= seg;
            }
            w += Vec3d(d.x(), d.y(), 0.);
            // Inside-ness by parity: shoot straight up and count crossings of the watertight shell.
            const std::vector<AABBMesh::hit_result> hits = tree.query_ray_hits(inv * w, inv.linear() * Vec3d(0., 0., 1.));
            int crossings = 0;
            for (const AABBMesh::hit_result &h : hits)
                if (h.is_hit() && h.distance() > 1e-6)
                    ++ crossings;
            if (crossings % 2 == 1)
                ++ inside;
        }
        // One stray sample can be a grazing pass along a wall; a run of them is a tunnel.
        if (inside >= 3)
            ++ probes_inside;
    }

    // 🚨 s300h — UNA SONDA NO BASTA PARA BLOQUEAR. Con nueve sondas repartidas por el contorno,
    // exigir sólo una era convertir cualquier roce de una esquina en "no puedes crear esto", y eso
    // es peor que no avisar: le quita al dueño una zona que a la vista está bien. Se pide un tercio
    // del cuerpo, que es lo que distingue un túnel de un rasguño.
    const size_t need = std::max<size_t>(2, probes.size() / 3);
    m_cross_cache_value = (probes_inside >= need);

    // 🔎 s300h — el dato que faltaba para no volver a discutir esto a ojo. Sólo cuando alguna sonda
    // entra: si nunca cruza, no hay nada que contar y el log se queda limpio.
    if (probes_inside > 0 && NeoDebug::enabled(NeoDebug::SUPPORTZONES))
        NeoDebug::write(NeoDebug::SUPPORTZONES,
                        "cross     sondas_dentro=" + std::to_string(probes_inside) +
                        " de " + std::to_string(probes.size()) +
                        "  hacen_falta=" + std::to_string(need) +
                        "  rodilla=" + std::string(ax.has_knee ? "si" : "no") +
                        "  veredicto=" + std::string(m_cross_cache_value ? "ATRAVIESA" : "roza"));

    return m_cross_cache_value;
}

// -----------------------------------------------------------------------------
// Zone management
// -----------------------------------------------------------------------------

void GLGizmoSupportZones::delete_selected_zone()
{
    // 🚨 s299h — se llega aquí por `CallAfter`, o sea un rato DESPUÉS del clic. En ese rato
    // el usuario ha podido cerrar el gizmo o cambiar de objeto, así que lo primero es
    // comprobar que la orden sigue teniendo sentido. Sin esto, diferir cambia un crash por
    // otro más raro de encontrar.
    if (get_state() != On)
        return;
    if (m_selected_zone < 0 || m_selected_zone >= int(m_zones.size()))
        return;
    Model *model = m_parent.get_selection().get_model();
    const int obj_idx = current_object_idx();
    if (model == nullptr || obj_idx < 0 || obj_idx >= int(model->objects.size()))
        return;
    ModelObject *mo = model->objects[obj_idx];
    const int vi = m_zones[m_selected_zone].volume_idx;
    if (mo == nullptr || vi < 0 || vi >= int(mo->volumes.size()))
        return;

    // 🚨 s299g — EL VOLUMEN SE BORRA POR LA LISTA DE OBJETOS, NO A MANO. Éste es el crash.
    //
    // Lo que había era `mo->delete_volume(vi)` directo. Borra el ModelVolume, sí, pero el árbol de
    // la lista de objetos NO se entera: se queda con un item que apunta a un volumen que ya no
    // existe. El siguiente refresco resuelve ese item contra un índice inválido y el proceso se cae
    // — que es exactamente lo que él vio, `Segmentation fault` justo al borrar, sin nada anterior
    // en el log.
    //
    // 🔑 `delete_from_model_and_list()` es el camino que usa el propio menú Borrar de la lista:
    // quita el volumen del modelo Y su item del árbol, en ese orden, y con sus reglas puestas — la
    // de "no puedes borrar la última pieza sólida", por ejemplo. Una zona nunca es una pieza sólida,
    // así que para nosotros esa regla no aplica; lo que importa es no volver a mantener a mano un
    // estado que la aplicación ya sabe mantener.
    //
    // ⛔ Y SIN snapshot propio: `delete_from_model_and_list()` toma el suyo. Anidar dos deja el
    // undo con un paso vacío, que es el bug con el que empieza el siguiente informe.
    if (wxGetApp().obj_list() == nullptr)
        return;
    m_selected_zone = -1;
    m_zones_dirty   = true;
    // El estado del editor se suelta ANTES de tocar el modelo: `data_changed()` va a llegar desde
    // dentro de la llamada, y todo lo que quede apuntando a este volumen habrá dejado de valer.
    if (editing())
        end_edit();
    clear_pick();
    wxGetApp().obj_list()->delete_from_model_and_list(itVolume, obj_idx, vi);
    m_parent.set_as_dirty();
}

// NEOTKO_SUPPORTZONES_TAG s288 — quitarle el candado a mano.
//
// ⚠️ Camino de una sola dirección, y a propósito: volver a bloquear exigiría volver a dar los dos
// clics, y eso ya es crear otra zona. Se ofrece porque el candado tiene un precio real — mientras
// está puesto, mover o rotar la zona con los gizmos de siempre la desbloquea sin avisar, y hay
// quien prefiere decidirlo él antes de tocar nada.
void GLGizmoSupportZones::unlock_selected_zone()
{
    // 🚨 s299h — se llega aquí por `CallAfter`, o sea un rato DESPUÉS del clic. En ese rato
    // el usuario ha podido cerrar el gizmo o cambiar de objeto, así que lo primero es
    // comprobar que la orden sigue teniendo sentido. Sin esto, diferir cambia un crash por
    // otro más raro de encontrar.
    if (get_state() != On)
        return;
    if (m_selected_zone < 0 || m_selected_zone >= int(m_zones.size()))
        return;
    const int obj_idx = current_object_idx();
    Model    &model   = wxGetApp().plater()->model();
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()))
        return;
    ModelObject *mo = model.objects[obj_idx];
    const int    vi = m_zones[m_selected_zone].volume_idx;
    if (mo == nullptr || vi < 0 || vi >= int(mo->volumes.size()))
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Unlock support zone"));
    mo->volumes[vi]->config.erase("neotko_support_zone_gesture");
    m_zones_dirty = true;
    // 🚨 Nada de relaminar: el gesto es descriptivo y la malla no se ha movido. La clave tiene su
    // propia rama de "no invalida nada" en PrintObject::invalidate_state_by_config_options().
    m_parent.set_as_dirty();
}

void GLGizmoSupportZones::duplicate_selected_zone()
{
    // 🚨 s299h — se llega aquí por `CallAfter`, o sea un rato DESPUÉS del clic. En ese rato
    // el usuario ha podido cerrar el gizmo o cambiar de objeto, así que lo primero es
    // comprobar que la orden sigue teniendo sentido. Sin esto, diferir cambia un crash por
    // otro más raro de encontrar.
    if (get_state() != On)
        return;
    if (m_selected_zone < 0 || m_selected_zone >= int(m_zones.size()))
        return;
    Model *model = m_parent.get_selection().get_model();
    const int obj_idx = current_object_idx();
    if (model == nullptr || obj_idx < 0 || obj_idx >= int(model->objects.size()))
        return;
    ModelObject *mo = model->objects[obj_idx];
    const int vi = m_zones[m_selected_zone].volume_idx;
    if (mo == nullptr || vi < 0 || vi >= int(mo->volumes.size()))
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Duplicate support zone"));
    ModelVolume *src = mo->volumes[vi];
    ModelVolume *dup = mo->add_volume(*src);
    if (dup == nullptr)
        return;
    dup->set_new_unique_id();
    // 🔑 The copy carries the source's config, which after T3 actually reaches the backend
    // (model_volume_list_copy_configs now includes SUPPORT_ENFORCER). That is what makes "set one
    // up the way you want and then repeat it" worth anything.
    //
    // Nudged sideways by its own footprint so it does not land exactly on top of the original,
    // which would look like nothing happened.
    const BoundingBoxf3 bb = dup->mesh().bounding_box();
    dup->set_offset(dup->get_offset() + Vec3d(bb.size().x() * 1.1, 0., 0.));

    // 🚨 s288 — EL DUPLICADO NACE SIN CANDADO, y no es una limitación: es la única de las tres
    // salidas que no miente.
    //   · copiar el gesto tal cual deja un gesto que apunta al parche del ORIGINAL, en un sitio
    //     donde este pilar ya no está;
    //   · recolocarlo y volver a buscar la cara es "repetir a lo largo del parche", que él probó y
    //     QUITÓ (SUPPORT_ZONES_PLAN §6) porque en una superficie curva las copias dejan de tocar.
    //     ⛔ No se reintroduce por la puerta de atrás.
    // Así que el duplicado es una malla: se coloca a mano, y si se quiere editable se dan dos
    // clics, que es barato.
    dup->config.erase("neotko_support_zone_gesture");

    // 🚨 s286b, visto por él: tras duplicar seguía seleccionado el ORIGINAL. Con el resaltado en
    // el 3D eso es peor que un detalle — te enseña encendida una zona que no es la que acabas de
    // crear, y el siguiente Duplicate vuelve a copiar el original en vez de encadenar.
    // La fila todavía no existe (la lista se reconstruye después), así que se apunta por índice de
    // VOLUMEN, que es lo único estable entre las dos, y rebuild_zone_rows() lo resuelve.
    m_select_volume_idx_pending = int(mo->volumes.size()) - 1;

    m_zones_dirty = true;
    // 🚨 s299g — MISMA LECCIÓN QUE EL BORRADO, del otro lado. Añadir un volumen a mano deja el árbol
    // de la lista sin la fila nueva, y a partir de ahí los índices del árbol y los del modelo van
    // desfasados: cualquier acción posterior sobre la lista opera sobre el volumen equivocado.
    // `add_volumes_to_object_in_list()` es lo que usa `create_pillar()`, y es lo que faltaba aquí.
    if (wxGetApp().obj_list() != nullptr) {
        wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(obj_idx));
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    }
    wxGetApp().plater()->update();
    m_parent.set_as_dirty();
}

// NEOTKO_SUPPORTZONES_TAG s288 — el gesto, de ida y de vuelta.
//
// JSON y no un formato propio: la clave viaja al 3mf como metadata escapada
// (`bbs_3mf.cpp`, `xml_escape`) y ya hay precedente de blobs JSON ahí dentro (los dos stacks de
// Sandwich). Un campo por dato y un número de versión delante.
std::string GLGizmoSupportZones::gesture_to_json(const ZoneGesture &g)
{
    nlohmann::json r;
    r["v"]   = g.version;
    r["tp"]  = { g.target_pos.x(),    g.target_pos.y(),    g.target_pos.z() };
    r["tn"]  = { g.target_normal.x(), g.target_normal.y(), g.target_normal.z() };
    r["lp"]  = { g.landing_pos.x(),   g.landing_pos.y(),   g.landing_pos.z() };
    r["bed"] = g.on_bed;
    r["fs"]  = int(g.foot_shape);
    r["fz"]  = g.foot_size_mm;
    if (g.covers)
        r["cv"] = true;
    // s289 — las marcas del pincel. Sólo se escriben si las hay, para que una zona de las de antes
    // siga produciendo exactamente el mismo JSON que producía.
    if (! g.stamps.empty()) {
        nlohmann::json st = nlohmann::json::array();
        for (const Vec4d &q : g.stamps)
            st.push_back({ q.x(), q.y(), q.z(), q.w() });
        r["st"] = st;
    }
    // s301 — los tocones ADICIONALES y su tamaño.
    //
    // 🚨 `tz` se escribe con el PINCEL, no con «hay tocones extra»: un árbol con un solo tocón —el
    // primario, que viaja en `lp`— no tiene extras, y sin esta distinción su tamaño no se guardaría
    // y la zona se reabriría con otro pie. Una zona de círculo o cuadrado sigue produciendo
    // exactamente el mismo JSON que producía.
    if (! g.stumps.empty()) {
        nlohmann::json tk = nlohmann::json::array();
        for (const StumpSpot &s : g.stumps)
            tk.push_back({ s.p.x(), s.p.y(), s.p.z(), s.on_bed ? 1. : 0. });
        r["tk"] = tk;
    }
    if (g.foot_shape == FootShape::Brush)
        r["tz"] = g.stump_size_mm;
    r["ep"]  = g.edge_patch_mm;
    r["ef"]  = g.edge_foot_mm;
    r["la"]  = g.lean_deg;
    r["lo"]  = { g.lock_offset.x(), g.lock_offset.y(), g.lock_offset.z() };
    return r.dump();
}

bool GLGizmoSupportZones::gesture_from_json(const std::string &text, ZoneGesture &out)
{
    if (text.empty())
        return false;
    // 🚨 Nada de dejar escapar una excepción de parseo hasta el render del gizmo: un 3mf de otra
    // versión, o tocado a mano, tiene que degradar a "esta zona ya no es editable" y no a un
    // diálogo de error encima del plato.
    try {
        const nlohmann::json r = nlohmann::json::parse(text);
        auto vec3 = [](const nlohmann::json &a, Vec3d &v) {
            if (! a.is_array() || a.size() != 3)
                return false;
            v = Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
            return true;
        };
        ZoneGesture g;
        g.version = r.value("v", 0);
        // 🔑 s289 — v2 añade las marcas del pincel y NADA más, así que un v1 se lee entero y se
        // queda sin marcas, que es exactamente lo que era. Rechazar v1 aquí habría dejado sin
        // candado todas las zonas ya creadas.
        // 🚨 s301 — AQUÍ FALTABA LA 3 Y LA ZONA PINTADA NO SE PODÍA REABRIR NUNCA.
        //
        // s300 subió el gesto a v3 al pasar los toques a 3D (`current_gesture`) y no tocó esta
        // puerta, que seguía aceptando 1 y 2. Resultado: toda zona con pincel se guardaba como v3,
        // volvía como "no parseable", y por tanto sin candado — o sea sin el botón de editar. El
        // `begin_edit_zone` de al lado da por hecho que un v3 llega entero, así que las dos mitades
        // estaban de acuerdo en todo menos en esta línea.
        if (g.version < 1 || g.version > 4)
            return false;   // formato futuro o corrupto: no se adivina
        if (! vec3(r["tp"], g.target_pos) || ! vec3(r["tn"], g.target_normal) ||
            ! vec3(r["lp"], g.landing_pos) || ! vec3(r["lo"], g.lock_offset))
            return false;
        g.on_bed        = r.value("bed", true);
        g.foot_shape    = FootShape(std::clamp(r.value("fs", 0), 0, 3));
        g.foot_size_mm  = r.value("fz", 8.f);
        g.covers        = r.value("cv", false);
        g.edge_patch_mm = r.value("ep", 0.f);
        g.edge_foot_mm  = r.value("ef", 0.f);
        g.lean_deg      = r.value("la", 45.f);
        if (r.contains("st") && r["st"].is_array())
            for (const auto &q : r["st"])
                if (q.is_array() && q.size() >= 3)
                    // El 4º es el trazo. Un gesto escrito antes de las cápsulas no lo lleva: todas
                    // sus marcas van al trazo 0, que las une en una sola banda. Es lo más parecido
                    // a lo que el usuario dibujó y no puede quedar peor que el rosario de antes.
                    g.stamps.emplace_back(q[0].get<double>(), q[1].get<double>(), q[2].get<double>(),
                                          q.size() > 3 ? q[3].get<double>() : 0.);
        // s301 — los tocones adicionales. Un gesto v1-v3 no los trae y se queda con la lista vacía,
        // que es exactamente lo que era: su único apoyo es el aterrizaje.
        g.stump_size_mm = r.value("tz", 8.f);
        if (r.contains("tk") && r["tk"].is_array())
            for (const auto &q : r["tk"])
                if (q.is_array() && q.size() >= 3)
                    g.stumps.push_back(StumpSpot{
                        Vec3d(q[0].get<double>(), q[1].get<double>(), q[2].get<double>()),
                        q.size() > 3 ? (q[3].get<double>() != 0.) : true });
        out = g;
        return true;
    } catch (...) {
        return false;
    }
}

GLGizmoSupportZones::ZoneGesture GLGizmoSupportZones::current_gesture(const Transform3d &inst) const
{
    ZoneGesture g;
    const Transform3d inv = inst.inverse();
    g.target_pos    = inv * m_target_world_pos;
    // 🚨 Una normal NO se transforma con la matriz. De objeto a mundo va con la inversa traspuesta
    // de la parte lineal, así que de mundo a objeto va con la TRASPUESTA a secas. Meterla por la
    // inversa la deja apuntando a otro sitio en cuanto hay una escala no uniforme, y la normal es
    // justo la guarda que confirma que la cara reencontrada es la misma.
    g.target_normal = (inst.linear().transpose() * m_target_world_normal).normalized();
    g.landing_pos   = inv * m_landing_world_pos;
    g.on_bed        = m_landing_on_bed;
    g.foot_shape    = m_foot_shape;
    g.foot_size_mm  = m_foot_size_mm;
    g.covers        = m_shape_covers;
    if (g.covers)
        g.version = 2;
    // Las marcas viajan en espacio del OBJETO, como todo lo demás del gesto: en mundo, Orca
    // renormaliza objeto e instancias sola y el trazo se descoloca sin que nadie lo toque.
    // 🚨 s300 — LOS TOQUES SON 3D, y por eso el gesto sube a la versión 3.
    //
    // En v2 se guardaba (x, y, radio, trazo): el toque era un disco en XY y el trazo servía para
    // saber qué marcas unir con una cápsula. Ahora el toque es un PUNTO SOBRE LA SUPERFICIE y lo
    // que se reproduce al reabrir es el recorrido por la malla, así que hace falta la z y sobra el
    // trazo. Los dos campos ocupan el mismo hueco, y el número de versión es lo que impide que un
    // gesto viejo se lea como si fuera nuevo.
    //
    // ⛔ Una zona pintada guardada con v2 ya NO se puede reabrir para editar: sus marcas describen
    // una geometría que este pincel no sabe reproducir. Se sigue imprimiendo igual —la malla está
    // en el volumen— y la tarjeta lo dice. Es la decisión que él tomó al empezar el rediseño.
    g.stamps.clear();
    g.stamps.reserve(m_stamps.size());
    for (const Stamp &st : m_stamps) {
        const Vec3d o = inv * st.p;
        g.stamps.emplace_back(o.x(), o.y(), o.z(), st.r);
    }
    if (! g.stamps.empty())
        g.version = 3;
    // NEOTKO_SUPPORTZONES_TAG s301 — LOS TOCONES, y con ellos la v4.
    //
    // 🚨 La versión sube con el PINCEL, no con «hay tocones extra». El modo se deduce de la forma
    // de la huella, así que un árbol con un solo tocón —lo normal— no tiene extras y aun así es un
    // gesto nuevo: su `tz` hace falta para reabrirlo con el mismo pie.
    g.stumps.clear();
    g.stumps.reserve(m_extra_stumps.size());
    for (const StumpSpot &s : m_extra_stumps)
        g.stumps.push_back(StumpSpot{ inv * s.p, s.on_bed });
    g.stump_size_mm = m_stump_size_mm;
    if (block_tree_mode())
        g.version = 4;
    g.edge_patch_mm = m_footprint_shrink_mm;
    g.edge_foot_mm  = m_footprint_base_mm;
    g.lean_deg      = m_lean_angle_deg;
    return g;
}

// La mitad de `create_pillar()` que no crea el volumen. Vive aparte porque editar una zona que ya
// existe es exactamente esto sobre un volumen que ya está (F2 del plan del candado).
void GLGizmoSupportZones::write_pillar_into(ModelVolume &v, TriangleMesh &&world_mesh,
                                            const Transform3d &inst, const ZoneGesture &g)
{
    // 🚨 s286b — ESTO ESTABA MAL Y SE NOTABA EN TRES SITIOS A LA VEZ.
    //
    // La malla se guardaba en coordenadas de MUNDO y se compensaba metiendo la inversa de la
    // instancia ENTERA — traslación incluida — en la matriz del volumen. Cuadra en pantalla y es
    // frágil en todo lo demás, porque un volumen no vive en mundo: vive en el espacio del OBJETO, y
    // su matriz debe ser sólo su colocación dentro de él. Las tres consecuencias, todas vistas por
    // él en la misma sesión:
    //   · la caja del volumen salía enorme, porque su malla estaba lejísimos de su propio origen
    //     — eso es "me marca una zona muy grande";
    //   · al guardar y cargar, Orca renormaliza objeto e instancias (`ensure_on_bed`,
    //     `center_around_origin`) y esa traslación escondida en la matriz del volumen se descoloca:
    //     "salen fuera y con la Z cambiada", y no siempre, que es lo propio de una renormalización;
    //   · y moverla o escalarla con los gizmos normales operaba sobre una matriz que ya llevaba la
    //     instancia dentro.
    //
    // Lo correcto es lo que hace `ObjectList::load_generic_subobject`: malla en espacio del objeto
    // y centrada en su propio origen, y la colocación en el `offset` del volumen. Entonces
    // `instancia · offset · malla` devuelve exactamente lo que dibujó el preview.
    TriangleMesh mesh = std::move(world_mesh);
    mesh.transform(inst.inverse());
    const Vec3d local_centre = mesh.bounding_box().center();
    mesh.translate(float(- local_centre.x()), float(- local_centre.y()), float(- local_centre.z()));

    v.set_mesh(std::move(mesh));
    v.calculate_convex_hull();
    v.invalidate_convex_hull_2d();
    // 🚨 s289 — ESTA LÍNEA ES EL BUG DEL EDITOR, y no se ve venir. `reload_scene()` empareja los
    // GLVolume de la escena con los ModelVolume POR ID, y reutiliza el que ya tiene cuando el id no
    // ha cambiado — reaplicándole la transformación nueva y quedándose con los TRIÁNGULOS VIEJOS.
    // Al crear no se notaba nunca (volumen nuevo, id nuevo); al editar era exactamente lo que él
    // describía: "al darle apply intenta cambiar la geometría en vez de usar el preview". El
    // preview era correcto porque lo dibuja el gizmo con la malla recién construida; lo que se
    // rompía era la copia de la escena.
    //
    // Es el mismo remate que hacen Simplify (GLGizmoSimplify.cpp) y Emboss (EmbossJob.cpp) después
    // de sustituir la malla de un volumen que ya existe. Se pone aquí, en el único sitio por el que
    // pasan crear y editar, para que no pueda volver a faltar en uno de los dos.
    v.set_new_unique_id();
    v.set_transformation(Geometry::Transformation());   // ni rotación ni escala propias
    v.set_offset(local_centre);
    v.config.set_key_value("extruder", new ConfigOptionInt(0));

    // 🔑 s288 — y el gesto, con el `offset` recién puesto como testigo del candado. Se escribe
    // DESPUÉS de colocar el volumen a propósito: el testigo tiene que ser lo que quedó, no lo que
    // se pretendía dejar.
    ZoneGesture stamped = g;
    stamped.lock_offset = local_centre;
    v.config.set_key_value("neotko_support_zone_gesture", new ConfigOptionString(gesture_to_json(stamped)));

    // NEOTKO_SUPPORTZONES_TAG s299 — el ángulo, otra vez y suelto.
    //
    // 🔑 Sí, está duplicado: vive dentro del JSON de aquí arriba y también en su propia clave. No
    // es descuido. El gesto es del editor y el motor no debe parsear JSON para nada; esta clave es
    // la frontera, y es la que `slice_support_enforcers_per_zone()` lee para saber a qué ángulo
    // baja la columna de esta zona. La otra diferencia importante es que ÉSTA invalida el soporte
    // y el gesto no.
    v.config.set_key_value("neotko_zone_lean_deg", new ConfigOptionFloat(double(g.lean_deg)));

    // NEOTKO_SUPPORTZONES_TAG s299c — y la zona nace sembrando SÓLO bajo el voladizo.
    //
    // 🔑 Es el default correcto para un pilar, y no para un enforcer cualquiera. Este bloque sube
    // desde la cama hasta la superficie que sujetas, así que con el contrato clásico del enforcer
    // ("soporta todo lo que caiga dentro, en todas las capas") iba dejando soporte pegado a la
    // pared del objeto en todo el recorrido. El dueño lo estaba tapando a mano con blockers.
    //
    // ⛔ Sólo se pone en las zonas que crea este gizmo. Un enforcer que traiga el usuario de fuera
    // conserva el comportamiento clásico, que es el que espera quien lo dibujó.
    // NEOTKO_SUPPORTZONES_TAG s300e — «agarra sólo en su techo», ENCENDIDA por defecto.
    //
    // 🚨 Es la misma clave que en s299e se apagó, con OTRO significado. Entonces quería decir "sólo
    // donde el diff 2D dice que hay superficie sin apoyo", y él la quitó con razón: dentro de una
    // pieza hueca la cara interior de una pared también es superficie sin apoyo, así que aquel
    // criterio seguía sembrando techos pasado el muro y encima recortaba de más.
    //
    // 🔑 Ahora quiere decir "el bloque agarra donde EMPIEZA, no por donde pasa", calculado como lo
    // que hay del bloque en una capa y no había en la de arriba. Por el tramo que atraviesa la
    // pieza el bloque ya venía de arriba, así que no hay techo y no siembra. No recorta el pilar:
    // sólo decide dónde se pega.
    // NEOTKO_SUPPORTZONES_TAG s301b — LAS DOS SE APAGAN, EN TODOS LOS MODOS.
    //
    // 🚨 Dicho por el dueño mirando el gcode del primer build: «desactiva grab only at its roof y
    // print the whole block, son la causa de que aparezcan techos dentro de la pieza; si no están
    // activos los soportes normales se crean casi bien». O sea que el veredicto ya no es una
    // sospecha: con las dos apagadas el pilar de círculo/cuadrado/parche sale bien, y con ellas
    // encendidas siembra interfaz contra las paredes por las que pasa.
    //
    // 🔑 Y encaja con lo que el §8 del estudio ya había razonado para el árbol de bloques: la zona
    // vuelve a ser un enforcer NORMAL, del camino clásico, que siembra donde corta superficie sin
    // apoyo. `roof_only` nació para evitar los techos dentro del muro y acabó causándolos porque
    // decide dónde AGARRA con un criterio (lo que hay aquí y no arriba) que en un bloque inclinado
    // o en un prisma recto no dice lo que parece.
    //
    // ⛔ Las claves NO se borran, pero NO es por compatibilidad: ninguna de las dos llegó nunca a un
    // build publicado (dicho por el dueño, s301b), así que no hay un solo 3mf ahí fuera que las
    // traiga puestas y no hay migración que hacer. Se quedan porque el motor las entiende y son la
    // palanca para probar a mano, sin recompilar, qué hace cada una — que es justo lo que hará
    // falta el día que se toquen los soportes que NO son de tocón.
    v.config.set_key_value("neotko_zone_roof_only", new ConfigOptionBool(false));

    // NEOTKO_SUPPORTZONES_TAG s299c — y MACIZA, que es la otra mitad de la misma promesa.
    //
    // 🔑 Las dos van juntas y hacen cosas distintas: `roof_only` decide dónde se AGARRA (sólo bajo
    // superficie sin apoyo, para no pegarse a la pared en todo el descenso) y `solid` decide qué se
    // IMPRIME (la sección del bloque, entera). Sin la segunda, lo que baja es la sección del
    // contacto y el pilar sale más flaco que el dibujo — "falta medio objeto en el slice".
    //
    // Encendida por defecto porque el §1 de esta herramienta es que lo que ves es lo que sale. Un
    // preview que enseña un pilar entero y un gcode que imprime una lámina es exactamente la
    // mentira que esto existe para no contar.
    //
    // s301b — apagada también, y por la misma medida del dueño. En el árbol de bloques además no
    // tendría sentido: `solid` aporta la sección del BLOQUE en cada capa, y en el tramo del medio
    // no hay bloque — lo que baja ahí es la columna, guiada hacia el tocón.
    v.config.set_key_value("neotko_zone_solid", new ConfigOptionBool(false));

    // NEOTKO_SUPPORTZONES_TAG s299d — y se posa SÓLO donde tú dijiste.
    //
    // 🔑 El gesto tiene un aterrizaje elegido con el segundo clic. Todo lo demás que la columna se
    // encuentre por el camino —una repisa interior, el borde de una cavidad— no lo ha pedido nadie,
    // y el generador se apoyaba en ello generando su interfaz de fondo allí mismo: los "techos
    // dentro de la pieza" que en realidad son suelos.
    v.config.set_key_value("neotko_zone_land_only", new ConfigOptionBool(false));
}

// NEOTKO_SUPPORTZONES_TAG s288 F2 — cargar un gesto guardado en el panel.
//
// 🔑 Aquí es donde se ve por qué el gesto se guarda en espacio del OBJETO: el punto va tal cual a
// `get_closest_facet()`, que trabaja en coordenadas de malla, y el raycaster de este gizmo está
// construido sobre `raw_mesh()`. Cero transformaciones intermedias donde equivocarse.
//
// ⛔ NO se guardó el índice de la cara y por eso hay que reencontrarla: un índice a la malla no
// sobrevive a una reparación. Con dos guardas, y si alguna falla se DEGRADA — se dice que la
// superficie ya no está y no se edita. Nunca se reconstruye el pilar en otro sitio.
bool GLGizmoSupportZones::begin_edit_zone(int row_idx)
{
    m_edit_lost_surface = false;
    if (row_idx < 0 || row_idx >= int(m_zones.size()) || ! m_zones[row_idx].locked)
        return false;
    const ModelObject *mo = current_object();
    if (mo == nullptr || mo->instances.empty())
        return false;
    ensure_raycaster();
    if (! m_raycaster)
        return false;

    const ZoneGesture &g = m_zones[row_idx].gesture;

    // Guarda 1: hay una cara ahí. Guarda 2: mira hacia donde miraba.
    const int facet = m_raycaster->get_closest_facet(g.target_pos.cast<float>());
    if (facet < 0 || facet >= int(m_face_normals.size())) {
        m_edit_lost_surface = true;
        return false;
    }
    const Vec3d n_now = m_face_normals[facet].cast<double>().normalized();
    if (n_now.dot(g.target_normal.normalized()) < 0.85) {
        m_edit_lost_surface = true;
        return false;
    }

    const Transform3d inst = mo->instances.front()->get_matrix();
    m_target_facet_idx    = facet;
    m_target_world_pos    = inst * g.target_pos;
    // De objeto a mundo, una normal va con la inversa traspuesta de la parte lineal.
    m_target_world_normal = (inst.linear().inverse().transpose() * g.target_normal).normalized();
    m_landing_world_pos   = inst * g.landing_pos;
    m_landing_on_bed      = g.on_bed;
    m_foot_shape          = g.foot_shape;
    m_foot_size_mm        = g.foot_size_mm;
    m_shape_covers        = g.covers;
    // s300 — los toques vuelven a mundo y se vuelve a pintar sobre la malla, para que editar una
    // zona pintada enseñe lo mismo que dejaste.
    //
    // 🚨 Sólo un gesto v3 trae toques 3D. Uno anterior los guardó como discos en XY, que describen
    // una geometría que este pincel ya no sabe reproducir: reproducirlos a ojo daría una zona
    // PARECIDA a la del usuario, y una zona parecida es peor que ninguna. Se degrada y se dice.
    m_stamps.clear();
    clear_painted();
    if (! g.stamps.empty() && g.version < 3) {
        m_edit_lost_surface = true;
        return false;
    }
    for (const Vec4d &q : g.stamps)
        m_stamps.push_back({ inst * Vec3d(q.x(), q.y(), q.z()), q.w() });
    repaint_from_stamps();
    // s301 — y los tocones vuelven a mundo. Un gesto anterior a la v4 llega sin ellos y se queda
    // con el aterrizaje como único apoyo, que es lo que era.
    m_extra_stumps.clear();
    m_extra_stumps.reserve(g.stumps.size());
    for (const StumpSpot &s : g.stumps)
        m_extra_stumps.push_back(StumpSpot{ inst * s.p, s.on_bed });
    m_stump_size_mm       = g.stump_size_mm;
    m_brush_seed_facet    = m_stamps.empty() ? -1 : facet;
    m_footprint_shrink_mm = g.edge_patch_mm;
    m_footprint_base_mm   = g.edge_foot_mm;
    m_lean_angle_deg      = g.lean_deg;

    // 🚨 s299 — AQUÍ SE RECORTABA EL ÁNGULO GUARDADO y ya no se recorta. El tope de antes salía de
    // milímetros por capa, así que reabrir una zona con otra altura de capa le cambiaba el ángulo a
    // sus espaldas. Ahora el motor recalcula su tope A PARTIR del ángulo, no al revés, así que el
    // valor guardado es válido siempre y lo único que cambia con la altura de capa es el color de
    // la recomendación. Se acota sólo contra el máximo duro del deslizador.
    m_lean_angle_deg = float(std::clamp(double(m_lean_angle_deg), 0.05,
                                        SupportZones::SUPPORT_ZONE_MAX_LEAN_DEG));

    m_has_target        = true;
    m_has_landing       = true;
    m_landing_locked    = true;   // el aterrizaje ya está decidido: no persigue al cursor
    m_target_pick_mode  = false;
    m_landing_pick_mode = false;
    m_stump_pick_mode   = false;
    m_have_hover_pos    = false;
    m_candidates.clear();
    m_painting           = false;
    invalidate_patch();           // la huella se recalcula para esta cara
    m_preview_dirty      = true;
    m_reach_model_r      = -1.;
    m_footprint_model_dirty = true;
    m_editing_volume_idx = m_zones[row_idx].volume_idx;
    m_parent.set_as_dirty();
    return true;
}

void GLGizmoSupportZones::end_edit()
{
    m_editing_volume_idx = -1;
    clear_pick();
    m_parent.set_as_dirty();
}

// Editar es escribir la MISMA malla que crearía el botón de crear, en un volumen que ya existe.
// Ni una línea de geometría nueva: `build_pillar_mesh()` y `write_pillar_into()` son los mismos de
// siempre, que es lo que garantiza que una zona editada y una recién creada sean idénticas.
void GLGizmoSupportZones::apply_edit()
{
    if (! editing())
        return;
    TriangleMesh mesh;
    if (! build_pillar_mesh(mesh))
        return;
    const int obj_idx = current_object_idx();
    Model    &model   = wxGetApp().plater()->model();
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()))
        return;
    ModelObject *mo = model.objects[obj_idx];
    if (mo == nullptr || mo->instances.empty() ||
        m_editing_volume_idx < 0 || m_editing_volume_idx >= int(mo->volumes.size()))
        return;
    ModelVolume *mv = mo->volumes[m_editing_volume_idx];
    if (mv == nullptr || ! mv->is_support_enforcer())
        return;

    // 🔑 UN solo snapshot por edición, y por eso se escribe con un botón y no mientras se arrastra
    // el deslizador. Escribir en vivo daría cuarenta snapshots y cuarenta invalidaciones para un
    // gesto de dos segundos. Mientras tanto lo que se ve es el preview del gizmo, que es la misma
    // malla, así que no se pierde nada por esperar.
    dump_geometry("apply_edit", &mesh);
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Edit support zone"));
    const Transform3d inst = mo->instances.front()->get_matrix();
    write_pillar_into(*mv, std::move(mesh), inst, current_gesture(inst));
    // La caja del objeto llevaba dentro la del pilar viejo. Sin esto el encuadre, el "fuera de
    // cama" y el propio `reload_scene` siguen midiendo contra la forma anterior.
    mo->invalidate_bounding_box();
    // El respaldo de BBS, igual que al añadir: la malla ha cambiado.
    Slic3r::save_object_mesh(*mo);

    m_zones_dirty = true;
    end_edit();
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    wxGetApp().plater()->update();
}

// NEOTKO_SUPPORTZONES_TAG s289 — EL VOLCADO DE GEOMETRÍA
//
// 🔑 Existe por una frase suya: "añade un debug para la geometría del soporte, así no hay que hacer
// export del 3mf". Escribe TODA la cadena de decisiones que dio forma al pilar —qué parche se
// tomó, cómo se recortó, dónde está el borde, cuánto admite, qué anillos salieron— y deja el
// sólido en un `.obj` que se abre en cualquier visor.
//
// 🚨 `NeoDebug::write()` a un canal propio, nunca `BOOST_LOG_TRIVIAL` ni el log de Orca.
void GLGizmoSupportZones::dump_obj(const TriangleMesh &m, const std::string &path)
{
    std::ofstream f(path, std::ios::trunc);
    if (! f.good())
        return;
    f << "# neotko support zone pillar\n";
    for (const Vec3f &v : m.its.vertices)
        f << "v " << v.x() << ' ' << v.y() << ' ' << v.z() << '\n';
    for (const Vec3i32 &t : m.its.indices)
        f << "f " << (t[0] + 1) << ' ' << (t[1] + 1) << ' ' << (t[2] + 1) << '\n';
}

void GLGizmoSupportZones::dump_geometry(const char *why, const TriangleMesh *solid) const
{
    if (! NeoDebug::enabled(NeoDebug::SUPPORTZONES))
        return;
    char b[512];
    auto L = [](const std::string &t) { NeoDebug::write(NeoDebug::SUPPORTZONES, t); };

    L("================ support zone geometry  [" + std::string(why) + "] ================");
    static const char *k_shape[4] = { "PATCH", "ROUND", "SQUARE", "BRUSH" };
    std::snprintf(b, sizeof(b), "gesture   facet=%d  shape=%s(%s)  size=%.2f mm  stamps=%d  edge patch=%.2f foot=%.2f  lean=%.1f deg",
                  m_target_facet_idx, k_shape[std::clamp(int(m_foot_shape), 0, 3)],
                  m_shape_covers ? "COVER" : "cut", double(m_foot_size_mm),
                  int(m_stamps.size()), double(m_footprint_shrink_mm), double(m_footprint_base_mm),
                  double(m_lean_angle_deg));
    L(b);
    std::snprintf(b, sizeof(b), "target    world (%.3f %.3f %.3f)  normal (%.3f %.3f %.3f)",
                  m_target_world_pos.x(), m_target_world_pos.y(), m_target_world_pos.z(),
                  m_target_world_normal.x(), m_target_world_normal.y(), m_target_world_normal.z());
    L(b);
    std::snprintf(b, sizeof(b), "landing   world (%.3f %.3f %.3f)  on_bed=%d  offset=%.3f mm  reach=%.3f mm  out_of_reach=%d",
                  m_landing_world_pos.x(), m_landing_world_pos.y(), m_landing_world_pos.z(),
                  int(m_landing_on_bed), landing_offset_mm(), reach_radius_mm(m_landing_world_pos.z()),
                  int(landing_out_of_reach()));
    L(b);

    if (m_foot_shape == FootShape::Brush) {
        // 🔑 El lienzo contra el parche coplanar: es EL número que explica un trazo cortado. Si el
        // lienzo es enorme y el parche coplanar es de 4 triángulos, lo que estabas viendo antes de
        // s289 era el segundo.
        const int seed = (m_brush_seed_facet >= 0) ? m_brush_seed_facet : m_target_facet_idx;
        std::snprintf(b, sizeof(b), "canvas    paint region=%d facets   (coplanar region would be %d)",
                      int(collect_paint_region(seed).size()), int(collect_region(seed).size()));
        L(b);
    }
    if (! m_stamps.empty()) {
        std::snprintf(b, sizeof(b), "brush     %d toques, %d triangulos pintados, cara semilla %d",
                      int(m_stamps.size()), int(m_painted_count), m_brush_seed_facet);
        L(b);
        for (size_t i = 0; i < m_stamps.size() && i < 200; ++ i) {
            std::snprintf(b, sizeof(b), "  toque %3d  (%.3f %.3f %.3f)  r=%.3f", int(i),
                          m_stamps[i].p.x(), m_stamps[i].p.y(), m_stamps[i].p.z(), m_stamps[i].r);
            L(b);
        }
    }

    // NEOTKO_SUPPORTZONES_TAG s299 — la sonda mira la MÁSCARA, que es lo que hay ahora.
    //
    // Las tres preguntas de antes (aristas no-manifold, bucles de borde, inglete máximo) eran
    // preguntas sobre un parche de triángulos, y ya no hay parche. Con Clipper esas tres patologías
    // no se pueden dar: un `ExPolygons` es válido por construcción. Lo que sí hay que poder mirar
    // es cuánta huella queda, cuántas islas, y qué dice el mapa de alturas.
    const ZoneMask *mk = m_has_target
        ? mask(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()))
        : nullptr;
    if (mk == nullptr || mk->area.empty()) {
        L("mask      NONE — no hay huella que extruir (revisa la forma: puede que no toque la region)");
    } else {
        double area_mm2 = 0.;
        size_t holes = 0, pts = 0;
        for (const ExPolygon &ep : mk->area) {
            area_mm2 += unscale<double>(unscale<double>(ep.area()));
            holes    += ep.holes.size();
            pts      += ep.contour.points.size();
        }
        std::snprintf(b, sizeof(b), "mask      islas=%d  agujeros=%d  pts=%d  area=%.3f mm2",
                      int(mk->area.size()), int(holes), int(pts), area_mm2);
        L(b);
        size_t hm_valid = 0;
        for (float z : m_hm_z)
            if (! std::isnan(z))
                ++ hm_valid;
        std::snprintf(b, sizeof(b), "mask      z_low=%.3f  z_high=%.3f  heightmap=%dx%d paso=%.2f mm  nodos con superficie=%d",
                      mk->z_low, mk->z_high, m_hm_nx, m_hm_ny, m_hm_step, int(hm_valid));
        L(b);
        // 🚨 s301 — este aviso SOLO vale para el pilar lofteado. La cabeza del árbol de bloques se
        // extruye con `zone_extrude_prism()`, que no remuestrea nada y por tanto conserva todas las
        // islas y todos los agujeros. Dejarlo dicho para los dos casos sería mentir en el log, que
        // es la clase de mentira que cuesta una sesión entera.
        if (mk->area.size() > 1 && ! block_tree_mode())
            L("mask      ⚠️ mas de una isla: el pilar se queda con la mas grande y descarta el resto");
        else if (mk->area.size() > 1)
            L("mask      islas multiples: la cabeza las extruye TODAS (el prisma no remuestrea)");
        if (hm_valid == 0)
            L("mask      ⚠️ el mapa de alturas esta vacio: el techo sera el plano de la cara semilla");
    }

    // NEOTKO_SUPPORTZONES_TAG s301 — los tocones, que en este modo son la mitad del gesto.
    if (block_tree_mode()) {
        const std::vector<StumpSpot> stumps = all_stumps();
        std::snprintf(b, sizeof(b), "arbol     tocones=%d  ø%.2f mm  alto=%.2f mm  hueco cabeza-tocón=%.2f mm%s",
                      int(stumps.size()), double(m_stump_size_mm), STUMP_HEIGHT_MM,
                      block_tree_gap_mm(),
                      block_tree_gap_mm() <= 0. ? "  🚨 SIN HUECO: el motor no verá un arbol" : "");
        L(b);
        for (size_t i = 0; i < stumps.size() && i < 64; ++ i) {
            std::snprintf(b, sizeof(b), "  tocón %2d  (%.3f %.3f %.3f)  en_cama=%d  alcance=%.2f mm",
                          int(i), stumps[i].p.x(), stumps[i].p.y(), stumps[i].p.z(),
                          int(stumps[i].on_bed), reach_radius_mm(stumps[i].p.z()));
            L(b);
        }
    }

    const std::vector<Vec2d> top  = target_footprint_world();
    const std::vector<Vec2d> foot = foot_outline_world();
    std::snprintf(b, sizeof(b), "outline   top pts=%d  foot pts=%d  too narrow=%d  off bed=%d  crosses object=%d",
                  int(top.size()), int(foot.size()), int(footprint_too_narrow()),
                  int(landing_off_bed()), int(pillar_crosses_object()));
    L(b);

    if (solid == nullptr) {
        L(block_tree_mode()
              ? "solid     NONE — no hay cabeza que extruir: revisa la huella y que haya al menos un tocón."
              : "solid     NONE — build_pillar_mesh() ha dicho que no. Mira la rodilla y el alcance de arriba.");
    } else {
        const BoundingBoxf3 bb = solid->bounding_box();
        std::snprintf(b, sizeof(b), "solid     verts=%d  tris=%d  volume=%.3f mm3",
                      int(solid->its.vertices.size()), int(solid->its.indices.size()), double(its_volume(solid->its)));
        L(b);
        std::snprintf(b, sizeof(b), "solid     bbox (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)",
                      bb.min.x(), bb.min.y(), bb.min.z(), bb.max.x(), bb.max.y(), bb.max.z());
        L(b);
        // Paridad de aristas: en un sólido cerrado cada arista aparece dos veces. Un número
        // distinto de 0 aquí es una malla abierta, y eso se lo come el motor sin decir nada.
        std::map<std::pair<int,int>, int> ec;
        for (const Vec3i32 &t : solid->its.indices)
            for (int k = 0; k < 3; ++ k)
                ++ ec[std::minmax(t[k], t[(k + 1) % 3])];
        int open_edges = 0, over = 0;
        for (const auto &e : ec) {
            if (e.second == 1) ++ open_edges;
            if (e.second > 2)  ++ over;
        }
        std::snprintf(b, sizeof(b), "solid     open edges=%d  over-shared edges=%d  %s",
                      open_edges, over,
                      (open_edges == 0 && over == 0) ? "(cerrado)" : "🚨 LA MALLA NO CIERRA");
        L(b);
        const std::string path = NeoDebug::log_dir() + "/supportzones_pillar.obj";
        dump_obj(*solid, path);
        L("solid     obj -> " + path);
    }
    L("");
}

void GLGizmoSupportZones::create_pillar()
{
    TriangleMesh mesh;
    if (! build_pillar_mesh(mesh))
        return;
    dump_geometry("create_pillar", &mesh);
    Model *model = m_parent.get_selection().get_model();
    const int obj_idx = current_object_idx();
    if (model == nullptr || obj_idx < 0 || obj_idx >= int(model->objects.size()))
        return;
    ModelObject *mo = model->objects[obj_idx];
    if (mo == nullptr || mo->instances.empty())
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add support zone"));

    // 🔑 s287 — la línea base de ajustes, DENTRO de este snapshot a propósito: el pilar y los
    // ajustes que lo hacen imprimible son una sola acción, así que se deshacen de una sola vez.
    // Sólo siembra lo que el objeto no tuviera ya dicho (ver seed_support_defaults).
    const bool seeded = seed_support_defaults(*mo);

    // 🚨 add_volume() CENTERS the mesh it is given (see the note in ObjectList::load_generic_subobject
    // and the workaround in EmbossJob's create_volume). So a placeholder goes in first and the real
    // mesh is set afterwards, which is the only way to keep coordinates we chose ourselves.
    ModelVolume *nv = mo->add_volume(make_cube(1., 1., 1.), ModelVolumeType::SUPPORT_ENFORCER);
    if (nv == nullptr)
        return;
    const Transform3d inst = mo->instances.front()->get_matrix();
    write_pillar_into(*nv, std::move(mesh), inst, current_gesture(inst));
    nv->name = into_u8(_L("Support zone"));
    nv->source.is_from_builtin_objects = true;
    // The pillar is placed, so the picks are spent. Keeping them would invite a second identical
    // zone on the next click.
    //
    // 🚨 s286b: esto se hacía a mano campo por campo y se dejaba DOS fuera — `m_candidates` y
    // `m_have_hover_pos`. `render_pick_overlays()` pinta el parche del candidato vivo sin mirar si
    // el modo de picking sigue activo, así que la lista de candidatos vieja mantenía la superficie
    // encendida después de crear el pilar. Ése era el "a veces se queda iluminada la zona": pasaba
    // sólo cuando quedaban candidatos, o sea según por dónde hubieras pasado el ratón.
    // `clear_pick()` ya sabe cuáles son TODOS los campos del gesto. Reimplementarlo a mano al lado
    // es la forma de que vuelva a pasar la próxima vez que se añada uno.
    // El backup de BBS, igual que hace load_generic_subobject al añadir un volumen. Sin esto la
    // malla nueva no entra en el respaldo del objeto.
    Slic3r::save_object_mesh(*mo);

    clear_pick();
    m_zones_dirty = true;

    // 🚨 Y la FILA en la lista de objetos. `update_info_items` refresca los contadores, NO crea el
    // nodo del volumen: por eso la zona "no salía como objeto" hasta que guardabas y recargabas,
    // que es cuando la lista se construye entera de cero.
    wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(obj_idx));
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    // Y si se sembró algo, el nodo de ajustes del objeto tiene que enseñarlo: un override que no se
    // ve en la lista es un ajuste que el usuario no puede ni revisar ni quitar.
    if (seeded)
        wxGetApp().obj_list()->update_and_show_object_settings_item();
    wxGetApp().plater()->update();
    m_parent.set_as_dirty();
}

// -----------------------------------------------------------------------------
// Overhang highlight
// -----------------------------------------------------------------------------

float GLGizmoSupportZones::overhang_normal_z_cut() const
{
    // GLCanvas3D::set_slope_normal_angle(a) stores -cos(deg2rad(90 - a)), and every caller passes
    // a = 90 - threshold. Composing the two gives -cos(threshold). Written out rather than
    // simplified so it can be checked against GLCanvas3D.hpp:463 by eye.
    return -::cos(Geometry::deg2rad(90.0f - (90.0f - m_overhang_threshold_deg)));
}

void GLGizmoSupportZones::apply_overhang_highlight()
{
    if (! m_threshold_seeded) {
        // Start from the number that governs the printed support, so the tool and the slicer are
        // talking about the same angle until the user says otherwise.
        const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (cfg.has("support_threshold_angle"))
            m_overhang_threshold_deg = float(cfg.opt_int("support_threshold_angle"));
        m_threshold_seeded = true;
    }

    m_overhang_model_dirty = true;
    if (m_show_overhangs && get_state() == On) {
        m_parent.set_slope_normal_angle(90.f - m_overhang_threshold_deg);
        if (! m_parent.is_using_slope())
            m_parent.use_slope(true);
    } else if (m_parent.is_using_slope()) {
        m_parent.use_slope(false);
    }
    m_parent.set_as_dirty();
}

void GLGizmoSupportZones::build_overhang_model()
{
    m_overhang_model_dirty = false;
    m_overhang_model.reset();
    // 🚨 s286b, dicho por él mirando el antes y el después: el mapa NO debe encenderse solo.
    // Pintar de una vez TODA cara que mire hacia abajo deja la pieza bañada y le roba el trabajo
    // al cursor — encender lo que ya está encendido no dice nada. Iluminar es del hover: una cara,
    // la que está bajo el ratón. El umbral sigue mandando (decide teal vs violeta en el hover y
    // alimenta el sombreado de pendiente), sólo se va el relleno.
    // El constructor de abajo se queda entero y sin llamar: si algún día el mapa vuelve, vuelve
    // como contorno, no como baño.
    if (! kOverhangMapFill)
        return;
    if (! m_show_overhangs)
        return;
    ensure_raycaster();
    const int n_facets = int(m_mesh.its.indices.size());
    if (n_facets == 0 || int(m_face_normals.size()) != n_facets || size_t(n_facets) > OVERHANG_MAP_MAX_FACETS)
        return;

    const double cut  = double(overhang_normal_z_cut());
    const float  lift = 0.06f; // along each facet's own normal, beats z-fighting

    indexed_triangle_set its;
    int base = 0;
    for (int f = 0; f < n_facets; ++ f) {
        Vec3d wn = m_world_trafo.linear() * m_face_normals[f].cast<double>();
        const double nl = wn.norm();
        if (nl < 1e-9)
            continue;
        wn /= nl;
        if (wn.z() > cut)
            continue;
        const Vec3i32 tri = m_mesh.its.indices[f];
        Vec3f n = m_face_normals[f];
        const float fl = n.norm();
        if (fl < 1e-6f)
            continue;
        n /= fl;
        its.vertices.push_back(m_mesh.its.vertices[tri[0]] + n * lift);
        its.vertices.push_back(m_mesh.its.vertices[tri[1]] + n * lift);
        its.vertices.push_back(m_mesh.its.vertices[tri[2]] + n * lift);
        its.indices.emplace_back(base, base + 1, base + 2);
        base += 3;
    }
    if (its.indices.empty())
        return;
    m_overhang_model.init_from(its);
    m_overhang_model.set_color(kOverhangCol);
}

void GLGizmoSupportZones::restore_overhang_highlight()
{
    // The FDM supports painter turns the shading off the same way when it closes; leaving it on
    // would follow the user out of the gizmo and look like a bug in the plater.
    if (m_parent.is_using_slope())
        m_parent.use_slope(false);
    m_threshold_seeded = false;
    m_parent.set_as_dirty();
}

// -----------------------------------------------------------------------------
// See-through
// -----------------------------------------------------------------------------

void GLGizmoSupportZones::restore_see_through()
{
    // 🚨 Before the early return, not after: this also runs on gizmo close, and leaving the switch
    // flipped would change how every translucent volume renders for the rest of the session.
    m_parent.get_volumes().set_transparent_depth_write(true);

    if (m_saved_colors.empty())
        return;
    // Restore by matching live volumes against saved ids; a stale id (a volume that no longer
    // exists after a rebuild) simply finds no match and is dropped. No dangling pointers.
    for (GLVolume *v : m_parent.get_volumes().volumes) {
        if (v == nullptr)
            continue;
        auto it = m_saved_colors.find(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()));
        if (it != m_saved_colors.end())
            v->set_color(it->second);
    }
    m_saved_colors.clear();
    m_parent.set_as_dirty();
}

// NEOTKO_SUPPORTZONES_TAG s287 — la línea base de ajustes de soporte.
//
// 🔑 Petición suya (s287) y con el aval de haberlos impreso: los ajustes con los que las zonas de
// soporte salen BIEN no son los de fábrica, y hasta ahora había que ponerlos a mano cada vez. Al
// crear el primer pilar de un objeto se le siembran, de modo que el punto de partida es decente en
// vez de ser el que Orca trae para soportes automáticos.
//
// Las tres reglas que hacen que esto sea sembrar y no secuestrar:
//
//  1. 🚨 SÓLO lo que el objeto no tenga ya dicho. Si una clave ya está en la config del objeto es
//     que alguien la decidió — él, un 3mf, o esta misma función en un pilar anterior — y no se
//     pisa. Sembrar es dar un punto de partida, nunca corregir al usuario.
//  2. 🚨 En la config DEL OBJETO, jamás en el preset. Aparece en la lista de objetos, se ve, se
//     borra de ahí, y entra en el snapshot de "Add support zone", así que un Ctrl+Z se lo lleva
//     junto con el pilar que lo trajo. Mismo criterio que `ensure_snug_style()`.
//  3. ⛔ `support_style` NO está en la tabla: lo pone `ensure_snug_style()` al ABRIR el gizmo,
//     porque el corredor lo necesita antes de que exista ningún pilar. Un solo dueño por clave.
//
// ⛔ Y `support_filament` tampoco está, aunque saliera marcado en su captura: el cuerpo del soporte
// en "como el objeto" es el vertedero de purga de Orca (`is_support_overriddable` exige filamento
// 0). Fijárselo aquí sería quitárselo a todo el mundo por defecto, que es justo lo contrario de un
// punto de partida decente.
bool GLGizmoSupportZones::seed_support_defaults(ModelObject &mo)
{
    bool touched = false;
    auto seed = [&mo, &touched](const char *key, ConfigOption *opt) {
        if (mo.config.has(key)) {
            delete opt;   // ya decidido: la opción nueva no llega a tener dueño
            return;
        }
        mo.config.set_key_value(key, opt);
        touched = true;
    };

    // Los valores son los suyos, medidos e impresos, no una elección de diseño de este fichero.
    seed("enable_support",               new ConfigOptionBool(true));
    // 🚨 s305 — 0.2, no 0.1. En la impresión verificada de s304 (jarrón, 2 extrusores, ALH de 0.32
    // a 0.08) el techo a 0.1 se pegaba a la pieza a ratos. No es del ancho de la zona: depende de
    // lo bien que salga el voladizo de esa capa, y cuando sale un poco alto el hueco desaparece.
    // 0.2 es además lo que trae el perfil por defecto, así que sembrar 0.1 era apartarse del perfil
    // para dejarlo peor. ⚠️ El hueco REAL sigue sin ser exactamente éste con altura de capa
    // variable, porque el techo del soporte no se lamina a la altura de la zona: cola de features.
    seed("support_top_z_distance",       new ConfigOptionFloat(0.2));
    seed("tree_support_wall_count",      new ConfigOptionInt(1));      // "Support wall loops"
    seed("support_interface_top_layers", new ConfigOptionInt(3));
    seed("support_interface_spacing",    new ConfigOptionFloat(0.));   // techo macizo
    seed("support_interface_pattern",
         new ConfigOptionEnum<SupportMaterialInterfacePattern>(smipRectilinear));
    seed("support_neoweave_enabled",     new ConfigOptionBool(false));

    // 🚨 s287-bis — EL TIPO, y sólo cuando es ÁRBOL.
    //
    // El perfil por defecto de Snapmaker viene en Tree, y con árbol esta feature entera es
    // decorativa: el generador de árbol no conoce el corredor, así que ni la inclinación ni la
    // rodilla ni el `Snug` que el gizmo fuerza al abrir significan nada. El pilar que dibujas y el
    // soporte que sale no son la misma cosa, que es justo lo que esta herramienta existe para no
    // hacer. Un aviso no basta cuando lo que se ofrece es un editor de forma.
    //
    // 🔑 Se conserva el auto/manual que el usuario tuviera: `tree(auto)` → `normal(auto)`,
    // `tree(manual)` → `normal(manual)`. Auto contra manual es una decisión de FLUJO — "¿quiero
    // además el soporte automático?" — y no es nuestra. Lo que sí es nuestro es que el generador
    // pueda cumplir lo que se dibuja.
    //
    // ⛔ Y si ya está en Normal no se toca NADA, ni siquiera para pasarlo a manual. Un usuario con
    // `normal(auto)` que añade una zona quiere las dos cosas, y quitarle el automático por haber
    // pinchado un techo sería secuestrarle el objeto.
    if (const SupportType st = effective_support_type(); is_tree(st)) {
        // Aquí NO vale el `seed()` de arriba: la clave puede existir ya en el objeto (puesta a
        // árbol a mano) y este caso es precisamente el que hay que corregir.
        mo.config.set_key_value("support_type",
                                new ConfigOptionEnum<SupportType>(is_auto(st) ? stNormalAuto : stNormal));
        touched = true;
    }

    return touched;
}

void GLGizmoSupportZones::ensure_snug_style()
{
    // 🔑 s286b, decisión suya después de verlo escalonado: al ENTRAR, lo primero es dejar el
    // estilo en Snug. El aviso A3 estaba bien pero llegaba tarde — un objeto recién creado nace
    // con el estilo por defecto, que abajo se resuelve a Grid, y entonces la rejilla se re-alinea
    // a la bbox de cada capa y una columna que debería deslizar sale a saltos de celda. Dibujar
    // bien encima de un ajuste que no puede cumplirlo es exactamente lo que esta herramienta
    // existe para no hacer.
    if (! support_style_blocks_corridor())
        return;

    const int obj_idx = current_object_idx();
    if (obj_idx < 0)
        return;
    Model &model = wxGetApp().plater()->model();
    if (obj_idx >= int(model.objects.size()))
        return;
    ModelObject *mo = model.objects[obj_idx];
    if (mo == nullptr)
        return;

    // 🚨 Se escribe en la config DEL OBJETO, nunca en el preset de impresión. Dos razones y las
    // dos pesan: el preset es del usuario y lo comparten todos los objetos del plato, y esto es
    // justo lo que son los ajustes por objeto — el override aparece en la lista, se ve, y se borra
    // desde ahí. Y va con snapshot, así que un Ctrl+Z lo deshace como cualquier otra cosa.
    wxGetApp().plater()->take_snapshot(_u8L("Support zones: support style set to Snug"));
    mo->config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsSnug));
    m_forced_snug = true;

    // Que se vea donde vive: el nodo de ajustes del objeto en la lista.
    if (wxGetApp().obj_list() != nullptr)
        wxGetApp().obj_list()->update_and_show_object_settings_item();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

// ⛔ s299c — AQUÍ VIVÍA EL «VER A TRAVÉS DE LA PIEZA», y se ha quitado por decisión del dueño:
// "cuanto más lo pienso menos sentido tiene". Y tenía razón por partida doble.
//
// Teñía de translúcido toda la geometría impresa para poder mirar dentro. El problema es que un
// bloque de soporte YA se dibuja translúcido, así que con la pieza también translúcida se
// superponen dos transparencias sin profundidad fiable y lo que sale no es "ver dentro", es un
// revoltijo donde no se distingue qué está delante de qué. Eso es lo que él veía como "muestra mal
// los objetos". Un instrumento que enseña algo distinto de la verdad, en la herramienta cuya
// promesa es que lo que dibujas es lo que sale, no vale la pena arreglarlo: sobra.
//
// Se conserva el resaltado de la zona seleccionada, que es lo ÚNICO de este bloque que hacía falta
// y que además nació aparte (s286b).
void GLGizmoSupportZones::apply_see_through()
{
    // 🚨 Restaurar SIEMPRE primero. Si no, la segunda llamada guardaría un color ya teñido como "el
    // original" y la escena se iría apagando en cada refresco hasta desaparecer.
    restore_see_through();
    if (get_state() != On)
        return;
    highlight_selected_zone();
    m_parent.set_as_dirty();
}

void GLGizmoSupportZones::highlight_selected_zone()
{
    // 🚨 s286b, dicho por él usando la herramienta: seleccionar una zona en la lista no se veía en
    // ninguna parte. Te enterabas de cuál habías cogido cuando cambiaban sus `hits` — o sea
    // demasiado tarde — y **Duplicate era un botón a ciegas**: no sabes qué se duplica hasta que
    // ya se ha duplicado. Un botón destructivo o creativo sobre una selección invisible es un
    // fallo de la herramienta, no una falta de costumbre.
    //
    // Va por el mismo camino que el fantasma y comparte `m_saved_colors`, así que restaurar sigue
    // siendo una sola operación y no hay dos sistemas peleándose por el color de un volumen.
    if (m_selected_zone < 0 || m_selected_zone >= int(m_zones.size()))
        return;
    const int obj_idx = current_object_idx();
    if (obj_idx < 0)
        return;
    const int vol_idx = m_zones[m_selected_zone].volume_idx;

    for (GLVolume *v : m_parent.get_volumes().volumes) {
        if (v == nullptr || v->object_idx() != obj_idx || v->volume_idx() != vol_idx)
            continue;
        // Sólo se guarda el original si no lo guardó ya el fantasma: si no, la segunda escritura
        // tomaría el color ya teñido por "el de verdad" y la restauración dejaría la zona teñida
        // para siempre. Es la misma trampa que el propio apply_see_through() documenta arriba.
        const auto key = std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx());
        if (m_saved_colors.find(key) == m_saved_colors.end())
            m_saved_colors.emplace(key, v->color);
        // Más brillante y más opaca que cualquier otro bloque: "ésta es la que tienes cogida".
        v->set_color(ColorRGBA(0.20f, 0.95f, 0.90f, 0.72f));
    }
}

// -----------------------------------------------------------------------------
// Panel
// -----------------------------------------------------------------------------

void GLGizmoSupportZones::on_render_input_window(float x, float y, float /*bottom_limit*/)
{
    // 🔑 s287-bis — el ancho se MIDE EN FUENTES, no en píxeles. Con la fuente de Orca los 360 px
    // fijos dejaban las etiquetas pegadas a los deslizadores; 22 alturas de fuente da la misma
    // holgura con cualquier fuente y cualquier DPI, y el suelo de 380 evita que una fuente diminuta
    // deje el alzado sin sitio para leerse.
    const float win_w = std::max(380.f, 22.0f * ImGui::GetFontSize());
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    // 🚨 s287-bis — EL PANEL SE ABRÍA ESTRECHÍSIMO Y LA CAUSA ES UNA PESCADILLA.
    //
    // `AlwaysAutoResize` significa "el ancho de la ventana lo dice su contenido". Desde el
    // rediseño, casi todo el contenido se mide con `GetContentRegionAvail()`, o sea "el ancho de la
    // ventana". Los dos se persiguen y el sistema tiene un punto fijo ESTABLE en el mínimo: al
    // abrir de cero no hay nada ancho que empuje, así que la ventana se queda pequeña y todo el
    // texto se parte en columnas de dos palabras. En cuanto aparecía algo con ancho propio (el
    // primer pilar) se desatascaba sola, que es justo el síntoma raro que se vio.
    //
    // 🔑 `SetWindowSize` DESPUÉS de `Begin` no lo arregla, y por eso estaba puesto y no servía: con
    // auto-resize, el `Begin` del frame siguiente recalcula el tamaño desde el contenido del
    // anterior y lo pisa. Lo que sí manda sobre el auto-fit es una RESTRICCIÓN, que se aplica
    // dentro de `Begin` (`CalcWindowSizeAfterConstraint`). Ancho fijado arriba y abajo, alto libre:
    // el alto sigue siendo del contenido, que es lo único que se le quería pedir al auto-resize.
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.f),
                                        ImVec2(win_w, std::numeric_limits<float>::max()));
    neo_push_window_style(); // 🚨 antes del Begin: WindowBg y el titulo se resuelven al abrir
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    // 🚨 The push/pop pair must not be crossed by an early return, or the style leaks into every
    // other ImGui window. That is exactly why the body lives in its own function
    // (GizmoNeotkoStyle.hpp says so, and it cost a session to learn).
    neo_push_panel_style();
    // 🚨 s286c — la ventana es `AlwaysAutoResize`, así que un `TextWrapped` sin tope de ajuste no
    // envuelve: ESTIRA la ventana hasta caber, y basta una frase larga para que el panel se vaya de
    // los 360 px y baile de ancho según lo que haya que decir. Fijando aquí la posición de ajuste,
    // todo el cuerpo respeta el ancho de contenido y ninguna cadena futura puede volver a romperlo.
    //
    // 🚨 s287: el rediseño saca casi todo el texto largo del panel y lo manda al tooltip, pero esto
    // NO sobra por eso. Los avisos siguen siendo frases, y una frase larga sin tope volvería a
    // estirar la ventana igual que antes. Si algún día el cuerpo cambia de contenedor, tiene que
    // seguir habiendo algo equivalente.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    render_panel_body();
    ImGui::PopTextWrapPos();
    neo_pop_panel_style();

    GizmoImguiEnd();
    neo_pop_window_style();
}

// =============================================================================
// s287 — EL REDISEÑO DEL PANEL
// =============================================================================
//
// El encargo (docs/WIP/SUPPORT_ZONES_s286_DEBUG_PLAN.md §5): el panel de s286 era una lista de
// frases apiladas. Todo lo que la herramienta sabe estaba escrito, nada estaba dibujado, y la lista
// de zonas eran filas de texto que no se distinguían entre sí.
//
// Las cuatro reglas del rediseño, y de dónde sale cada una:
//
//  1. ⛔ NO se sigue el canon de Orca. Que un panel nuestro se parezca a uno suyo no es un valor.
//     La referencia es Align & Stack (iconos vectoriales dibujados a mano en el draw list, no
//     texturas) con el color de GizmoNeotkoStyle.
//
//  2. 🔑 MENOS TEXTO, MÁS DIBUJO. Lo que se explicaba en un párrafo se ve. Los porqués largos se
//     conservan ENTEROS, pero en el tooltip: nada de lo que decía el panel de s286 se ha perdido,
//     se ha movido a donde no estorba. Las cadenas largas son las mismas de antes a propósito, para
//     no tirar sus traducciones.
//
//  3. 🔑 EL ALZADO manda. El corazón del panel es un dibujo de sección a ESCALA UNIFORME: 45° en el
//     deslizador salen 45° en pantalla. No es decoración — es el único sitio donde "la rodilla cae
//     bajo la cama" se ve en vez de leerse, y donde el alcance máximo del motor es una forma y no
//     un número que hay que creerse.
//
//  4. 🔑 Cada zona se reconoce por su FORMA, su COLOR y su ESTADO sin leer: una tarjeta con el
//     dibujo de su pilar, el color de su filamento de techo, y un medidor de lo que captura.
//
// 🚨 Todo lo que se dibuja aquí sale de los MISMOS números que construyen el pilar (lean_top_z,
// landing_offset_mm, reach_radius_mm, max_lean_angle_deg, las dos huellas). Ningún dibujo tiene
// geometría propia: si el dibujo y el sólido discreparan alguna vez, sería un bug de los de verdad.

namespace {

// El color de un slot de filamento tal y como lo guarda el proyecto. Mismo criterio que usa
// GLGizmoColorStitchPainter (tool_col_u32), para que un mismo filamento se vea del mismo color en
// los dos gizmos.
ImU32 zone_filament_col(const std::vector<std::string> &fcolors, int slot0)
{
    if (slot0 >= 0 && slot0 < int(fcolors.size()) && ! fcolors[slot0].empty()) {
        std::string h = fcolors[slot0];
        if (h[0] == '#')
            h = h.substr(1);
        if (h.size() >= 6) {
            const unsigned long rgb = std::strtoul(h.substr(0, 6).c_str(), nullptr, 16);
            return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
        }
    }
    return IM_COL32(128, 134, 142, 255);
}

// Los colores de un filamento se eligen a gusto del usuario, así que un texto encima puede caer
// sobre amarillo puro o sobre negro. La luminancia decide, como en cualquier etiqueta de color.
ImU32 ink_on(ImU32 bg)
{
    const float r = float((bg >> IM_COL32_R_SHIFT) & 0xFF);
    const float g = float((bg >> IM_COL32_G_SHIFT) & 0xFF);
    const float b = float((bg >> IM_COL32_B_SHIFT) & 0xFF);
    return (0.299f * r + 0.587f * g + 0.114f * b) > 140.f ? IM_COL32(18, 21, 26, 255)
                                                          : IM_COL32(236, 240, 244, 255);
}

// 🔑 s287-bis — LA UNIDAD DEL PANEL ES LA FUENTE, no el píxel.
//
// Primera versión del rediseño: todo en píxeles fijos. Con la fuente de Orca (18 px de base, y más
// con DPI alto) las etiquetas se comían la columna del deslizador y los iconos quedaban apretados
// contra el texto — lo que se vio en la captura. La cura no es subir los números a ojo: es que cada
// medida se exprese en ALTURAS DE FUENTE, así que el panel se estira solo cuando la fuente crece y
// no hay ninguna combinación de idioma y DPI que lo pueda apretar.
inline float neo_u() { return ImGui::GetFontSize(); }

// La columna donde empiezan los deslizadores. Se mide sobre las etiquetas DE VERDAD, ya traducidas,
// para que la tabla siga cuadrando en alemán o en chino. Se recalcula por frame: son ocho
// CalcTextSize, que al lado de un flood-fill de la huella no se nota.
float neo_label_col()
{
    const char *caps[] = { "overhangs", "map detail", "ghost", "grid marks", "across", "side",
                           "edge · patch", "edge · foot", "lean", "footprint" };
    float w = 0.f;
    for (const char *c : caps)
        w = std::max(w, ImGui::CalcTextSize(_u8L(c).c_str()).x);
    // El tope existe para que una traducción larguísima no deje el deslizador sin sitio: a partir
    // de ahí la etiqueta se recorta, que es mejor que un control de doce píxeles.
    return std::min(w + 0.7f * neo_u(), ImGui::GetContentRegionAvail().x * 0.46f);
}

// -----------------------------------------------------------------------------
// Los iconos
// -----------------------------------------------------------------------------
// Vectoriales y dibujados a mano, como en Align & Stack, y por la misma razón: escalan con el DPI
// sin pedirle nada al empaquetado de recursos, se recolorean solos según el estado, y un icono que
// es código se corrige en el sitio donde se usa.
//
// Todos se dibujan dentro del cuadrado [0,1] escalado por `s`, así que el mismo glifo sirve para un
// botón de 22 px y para un dibujo de 40.
enum class Glyph {
    Overhang,   // el sombreado de voladizos
    GapMap,     // el mapa de lo que nadie sujeta
    Xray,       // la pieza fantasma
    Patch,      // huella: el parche entero
    Round,      // huella: recorte redondo
    Square,     // huella: recorte cuadrado
    Brush,      // huella: pintada a mano (s289)
    Trash,
    Copy,
    Cube,       // "como el objeto"
    Warn,
    Target,     // el 1er clic: la superficie a sujetar
    Landing,    // el 2º clic: dónde aterriza
    Pillar,     // crear
    Lock,       // s288 — la zona nació del gesto y nadie la ha tocado por fuera
    Unlock,     // s288 — la acción de soltarla
    Edit,       // s288 — la acción de volver a editarla
};

void draw_glyph(ImDrawList *dl, const ImVec2 &p0, float s, Glyph g, ImU32 col)
{
    auto P = [&](float u, float v) { return ImVec2(p0.x + u * s, p0.y + v * s); };
    const float th = std::max(1.2f, s * 0.075f);

    switch (g) {
    case Glyph::Overhang: {
        // Un techo con su alero, y debajo la ristra de rayas que es lo que el sombreado pinta.
        const ImVec2 roof[4] = { P(0.12f, 0.18f), P(0.88f, 0.18f), P(0.88f, 0.34f), P(0.12f, 0.34f) };
        dl->AddConvexPolyFilled(roof, 4, col);
        const ImVec2 lean[3] = { P(0.12f, 0.34f), P(0.62f, 0.34f), P(0.12f, 0.72f) };
        dl->AddConvexPolyFilled(lean, 3, (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT));
        for (int i = 0; i < 3; ++ i) {
            const float u = 0.24f + 0.16f * float(i);
            dl->AddLine(P(u, 0.52f + 0.10f * float(i)), P(u, 0.88f), col, th * 0.7f);
        }
        break;
    }
    case Glyph::GapMap: {
        // La rejilla del mapa: unos puntos cogidos y otros no. Sin color propio — el estado del
        // botón lo pone, porque el rojo y el verde son del 3D, no del icono.
        for (int r = 0; r < 3; ++ r)
            for (int c = 0; c < 3; ++ c) {
                const ImVec2 q = P(0.22f + 0.28f * float(c), 0.22f + 0.28f * float(r));
                if ((r + c) % 2 == 0)
                    dl->AddCircleFilled(q, s * 0.085f, col, 10);
                else
                    dl->AddCircle(q, s * 0.085f, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT), 10, th * 0.6f);
            }
        break;
    }
    case Glyph::Xray: {
        // Una caja con la arista de detrás vista a través: es literalmente lo que hace el modo.
        const ImVec2 box[4] = { P(0.16f, 0.24f), P(0.84f, 0.24f), P(0.84f, 0.80f), P(0.16f, 0.80f) };
        dl->AddPolyline(box, 4, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.16f, 0.24f), P(0.84f, 0.80f), (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), th * 0.8f);
        dl->AddCircleFilled(P(0.50f, 0.52f), s * 0.13f, (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), 14);
        break;
    }
    case Glyph::Patch: {
        // Un parche con muescas: la forma que sale del flood-fill, que no es ni redonda ni cuadrada.
        const ImVec2 blob[7] = { P(0.14f, 0.34f), P(0.42f, 0.20f), P(0.74f, 0.26f), P(0.88f, 0.50f),
                                 P(0.68f, 0.78f), P(0.34f, 0.76f), P(0.10f, 0.58f) };
        dl->AddConvexPolyFilled(blob, 7, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
        dl->AddPolyline(blob, 7, col, ImDrawFlags_Closed, th);
        break;
    }
    case Glyph::Round:
        dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.30f, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), 24);
        dl->AddCircle(P(0.5f, 0.5f), s * 0.30f, col, 24, th);
        break;
    case Glyph::Square:
        dl->AddRectFilled(P(0.22f, 0.22f), P(0.78f, 0.78f), (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), s * 0.06f);
        dl->AddRect      (P(0.22f, 0.22f), P(0.78f, 0.78f), col, s * 0.06f, 0, th);
        break;
    case Glyph::Brush: {
        // Un rastro de marcas solapadas y el pincel encima: es exactamente lo que hace el modo.
        dl->AddCircleFilled(P(0.30f, 0.66f), s * 0.20f, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), 20);
        dl->AddCircleFilled(P(0.52f, 0.58f), s * 0.20f, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), 20);
        dl->AddCircle      (P(0.30f, 0.66f), s * 0.20f, col, 20, th * 0.8f);
        dl->AddCircle      (P(0.52f, 0.58f), s * 0.20f, col, 20, th * 0.8f);
        dl->AddLine(P(0.58f, 0.50f), P(0.86f, 0.18f), col, th);
        break;
    }
    case Glyph::Trash:
        dl->AddLine(P(0.18f, 0.28f), P(0.82f, 0.28f), col, th);
        dl->AddLine(P(0.40f, 0.20f), P(0.60f, 0.20f), col, th);
        {
            const ImVec2 body[4] = { P(0.26f, 0.32f), P(0.74f, 0.32f), P(0.66f, 0.84f), P(0.34f, 0.84f) };
            dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, th);
        }
        break;
    case Glyph::Copy:
        dl->AddRect(P(0.16f, 0.16f), P(0.66f, 0.66f), (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), s * 0.06f, 0, th);
        dl->AddRectFilled(P(0.34f, 0.34f), P(0.84f, 0.84f), (col & ~IM_COL32_A_MASK) | (60u << IM_COL32_A_SHIFT), s * 0.06f);
        dl->AddRect      (P(0.34f, 0.34f), P(0.84f, 0.84f), col, s * 0.06f, 0, th);
        break;
    case Glyph::Cube: {
        // El cubo isométrico de Align & Stack, en pequeño: "esto lo hereda del objeto".
        const ImVec2 top[4]   = { P(0.50f, 0.16f), P(0.86f, 0.36f), P(0.50f, 0.56f), P(0.14f, 0.36f) };
        const ImVec2 left[4]  = { P(0.14f, 0.36f), P(0.50f, 0.56f), P(0.50f, 0.88f), P(0.14f, 0.68f) };
        const ImVec2 right[4] = { P(0.50f, 0.56f), P(0.86f, 0.36f), P(0.86f, 0.68f), P(0.50f, 0.88f) };
        dl->AddConvexPolyFilled(top,   4, (col & ~IM_COL32_A_MASK) | (200u << IM_COL32_A_SHIFT));
        dl->AddConvexPolyFilled(left,  4, (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT));
        dl->AddConvexPolyFilled(right, 4, (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT));
        break;
    }
    case Glyph::Warn: {
        const ImVec2 tri[3] = { P(0.50f, 0.14f), P(0.94f, 0.84f), P(0.06f, 0.84f) };
        dl->AddConvexPolyFilled(tri, 3, (col & ~IM_COL32_A_MASK) | (55u << IM_COL32_A_SHIFT));
        dl->AddPolyline(tri, 3, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.50f, 0.38f), P(0.50f, 0.60f), col, th * 1.1f);
        dl->AddCircleFilled(P(0.50f, 0.72f), th * 0.75f, col, 8);
        break;
    }
    case Glyph::Target: {
        // La superficie de arriba y la flecha que la empuja hacia abajo: lo que hay que sujetar.
        dl->AddRectFilled(P(0.12f, 0.16f), P(0.88f, 0.30f), col, s * 0.05f);
        for (int i = 0; i < 2; ++ i) {
            const float u = 0.34f + 0.32f * float(i);
            dl->AddLine(P(u, 0.40f), P(u, 0.74f), col, th * 0.9f);
            const ImVec2 head[3] = { P(u, 0.86f), P(u - 0.10f, 0.68f), P(u + 0.10f, 0.68f) };
            dl->AddConvexPolyFilled(head, 3, col);
        }
        break;
    }
    case Glyph::Landing: {
        // La cama y el punto donde se apoya, con su marca de sitio.
        dl->AddLine(P(0.08f, 0.80f), P(0.92f, 0.80f), col, th * 1.3f);
        dl->AddCircleFilled(P(0.50f, 0.80f), s * 0.10f, col, 14);
        dl->AddLine(P(0.50f, 0.18f), P(0.50f, 0.68f), (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), th * 0.8f);
        for (int i = 0; i < 3; ++ i)
            dl->AddLine(P(0.14f + 0.28f * float(i), 0.86f), P(0.06f + 0.28f * float(i), 0.94f),
                        (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), th * 0.6f);
        break;
    }
    case Glyph::Lock:
    case Glyph::Unlock: {
        // 🚨 s288, dicho por él: el candado del ESTADO se leía como abierto. La culpa era del arco,
        // que estaba hecho con cuatro rectas y a 13 px no cerraba. Ahora es un arco de verdad
        // (`PathArcTo`), y cerrado quiere decir cerrado: las dos patas bajan RECTAS hasta el cuerpo.
        // La versión abierta levanta y gira una de las patas, que es como se dibuja un candado
        // abierto en todas partes.
        dl->AddRectFilled(P(0.22f, 0.48f), P(0.78f, 0.88f), col, s * 0.10f);
        const bool  open = (g == Glyph::Unlock);
        const ImVec2 c   = P(open ? 0.62f : 0.50f, 0.42f);
        const float  r   = s * 0.17f;
        dl->PathClear();
        // 🚨 Literal y no `IM_PI`: esa macro vive en `imgui_internal.h`, que este fichero no incluye
        // ni debe incluir. Media vuelta, de PI a 2·PI, o sea el arco de arriba.
        dl->PathArcTo(c, r, 3.14159265f, 6.28318531f, 14);
        dl->PathStroke(col, 0, th);
        // La pata que baja al cuerpo. Abierto: sólo una, y más corta.
        dl->AddLine(ImVec2(c.x + r, c.y), ImVec2(c.x + r, P(0.f, open ? 0.60f : 0.50f).y), col, th);
        if (! open)
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r, P(0.f, 0.50f).y), col, th);
        break;
    }
    case Glyph::Edit: {
        // Un lápiz en diagonal: el cuerpo, la punta y la línea de lo escrito.
        const ImVec2 body[4] = { P(0.28f, 0.70f), P(0.68f, 0.18f), P(0.82f, 0.30f), P(0.42f, 0.82f) };
        dl->AddConvexPolyFilled(body, 4, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
        dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, th);
        const ImVec2 tip[3] = { P(0.42f, 0.82f), P(0.28f, 0.70f), P(0.24f, 0.88f) };
        dl->AddConvexPolyFilled(tip, 3, col);
        break;
    }
    case Glyph::Pillar: {
        // El pilar con rodilla, que es exactamente lo que fabrica el botón.
        const ImVec2 col_pts[6] = { P(0.20f, 0.14f), P(0.44f, 0.14f), P(0.78f, 0.56f),
                                    P(0.78f, 0.86f), P(0.60f, 0.86f), P(0.60f, 0.60f) };
        dl->AddConvexPolyFilled(col_pts, 6, (col & ~IM_COL32_A_MASK) | (90u << IM_COL32_A_SHIFT));
        dl->AddPolyline(col_pts, 6, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.06f, 0.92f), P(0.94f, 0.92f), col, th);
        break;
    }
    }
}

// -----------------------------------------------------------------------------
// Primitivas de dibujo que el alzado y las tarjetas comparten
// -----------------------------------------------------------------------------

void dashed_line(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 col, float th,
                 float dash = 5.f, float gap = 4.f)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f)
        return;
    const float ux = dx / len, uy = dy / len;
    for (float t = 0.f; t < len; t += dash + gap) {
        const float t1 = std::min(t + dash, len);
        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t), ImVec2(a.x + ux * t1, a.y + uy * t1), col, th);
    }
}

// El rayado de "esto es la PIEZA". Diagonal, fino, y recortado al rectángulo que se le pide, que es
// lo que le permite seguir un contorno sin calcular intersecciones.
void hatch_rect(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 col, float spacing = 7.f)
{
    dl->PushClipRect(a, b, true);
    const float w = b.x - a.x, h = b.y - a.y;
    for (float t = -h; t < w; t += spacing)
        dl->AddLine(ImVec2(a.x + t, b.y), ImVec2(a.x + t + h, a.y), col, 1.0f);
    dl->PopClipRect();
}

// Una cota, con sus dos topes y su número flotando encima. Horizontal.
void dim_line_h(ImDrawList *dl, float x0, float x1, float y, ImU32 col, const char *label)
{
    if (x1 < x0) std::swap(x0, x1);
    dl->AddLine(ImVec2(x0, y - 4.f), ImVec2(x0, y + 4.f), col, 1.f);
    dl->AddLine(ImVec2(x1, y - 4.f), ImVec2(x1, y + 4.f), col, 1.f);
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col, 1.f);
    if (label != nullptr && label[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        neo_draw_pill(dl, ImVec2((x0 + x1) * 0.5f - ts.x * 0.5f, y - ts.y - 6.f), label, col);
    }
}

// El texto que no cabe se corta con puntos suspensivos en vez de estirar la ventana o desbordar la
// tarjeta. 🚨 Se recorta por BYTES sobre UTF-8, así que hay que retroceder hasta un principio de
// carácter o se escribe medio glifo y la fuente pinta basura.
std::string fit_text(const std::string &s, float max_w)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w)
        return s;
    std::string out = s;
    while (! out.empty()) {
        out.pop_back();
        while (! out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        if (ImGui::CalcTextSize((out + "…").c_str()).x <= max_w)
            break;
    }
    return out + "…";
}

// -----------------------------------------------------------------------------
// Los widgets del panel
// -----------------------------------------------------------------------------

// Un título de sección: la etiqueta y una regla que llega hasta el borde. Ocupa una línea y separa
// mejor que un Separator con un Text encima, que son dos.
void neo_section(const char *label)
{
    ImGui::Spacing();
    ImDrawList  *dl    = ImGui::GetWindowDrawList();
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts    = ImGui::CalcTextSize(label);
    dl->AddText(p, neo_fade(NeoCol::Ink, 0.90f), label);
    const float x0 = p.x + ts.x + 8.f, x1 = p.x + avail;
    if (x1 > x0)
        dl->AddLine(ImVec2(x0, p.y + ts.y * 0.55f), ImVec2(x1, p.y + ts.y * 0.55f),
                    neo_fade(NeoCol::SurfaceHi, 0.85f), 1.f);
    ImGui::Dummy(ImVec2(avail, ts.y + 3.f));
}

// Un interruptor con icono. Encendido = fondo de acento y el glifo en tinta; apagado = superficie y
// glifo apagado. Sin etiqueta: lo que hace lo cuenta el tooltip, que es donde cabe entero.
bool neo_glyph_toggle(const char *id, float size, bool active, Glyph g, const char *tip)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, active ? neo_fade(NeoCol::Accent, hv ? 1.0f : 0.85f)
                                   : neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface), 5.f);
    if (active)
        dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 5.f, 0, 1.2f);
    draw_glyph(dl, ImVec2(a.x + size * 0.16f, a.y + size * 0.16f), size * 0.68f, g,
               active ? neo_col_u32(NeoCol::Ink) : neo_fade(NeoCol::TextDim, hv ? 1.0f : 0.85f));
    if (hv && tip != nullptr)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

// Un botón de icono pequeño, para las acciones de una tarjeta. `danger` lo pinta de ámbar al pasar
// por encima, que es lo único que distingue borrar de duplicar antes de pulsar.
//
// 🔑 s288 — `tint` rompe esa regla a propósito para UN caso: editar. Editar y el candado son la
// misma idea vista dos veces —se puede editar PORQUE está bloqueada— así que van del mismo color y
// el botón lleva su fondo puesto siempre, no sólo al pasar por encima. Con el gris de las demás
// acciones había que saberse la relación; del mismo verde, se ve.
bool neo_glyph_button(const char *id, float size, Glyph g, bool danger, const char *tip,
                      NeoCol tint = NeoCol::TextDim, bool always_lit = false)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv = ImGui::IsItemHovered();
    const bool   own_tint = (tint != NeoCol::TextDim);
    if (hv || always_lit)
        dl->AddRectFilled(a, b, danger      ? neo_fade(NeoCol::Warn, hv ? 0.22f : 0.10f)
                              : own_tint    ? neo_fade(tint, hv ? 0.30f : 0.14f)
                                            : neo_fade(NeoCol::Accent, 0.28f), 4.f);
    draw_glyph(dl, ImVec2(a.x + size * 0.18f, a.y + size * 0.18f), size * 0.64f, g,
               own_tint ? neo_fade(tint, hv ? 1.0f : 0.85f)
                        : (hv ? (danger ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright))
                              : neo_fade(NeoCol::TextDim, 0.9f)));
    if (hv && tip != nullptr)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

// Una píldora de estado en la fila del cabecero. No es un control: no se pulsa, sólo cuenta algo.
void neo_status_chip(const char *text, ImU32 col, bool solid)
{
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float  w  = ts.x + 0.95f * neo_u(), h = ts.y + 0.36f * neo_u();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), solid ? col : ((col & ~IM_COL32_A_MASK) | (36u << IM_COL32_A_SHIFT)), h * 0.5f);
    if (! solid)
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), (col & ~IM_COL32_A_MASK) | (170u << IM_COL32_A_SHIFT), h * 0.5f, 0, 1.f);
    dl->AddText(ImVec2(p.x + 0.47f * neo_u(), p.y + 0.18f * neo_u()), solid ? ink_on(col) : col, text);
    ImGui::Dummy(ImVec2(w, h));
}

// Un deslizador con su etiqueta en una columna fija a la izquierda. La columna es lo que hace que
// seis deslizadores distintos se lean como una tabla y no como seis frases.
bool neo_row_slider(const char *id, const char *caption, float *v, float mn, float mx,
                    const char *fmt, const char *tip)
{
    const float col = neo_label_col();
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    // 🚨 Recortada al ancho de la columna: sin esto una etiqueta larga se mete DEBAJO del
    // deslizador, que es exactamente lo que pasaba con "overhangs" y "map detail".
    ImGui::TextUnformatted(fit_text(caption, col - 0.4f * neo_u()).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip != nullptr ? tip : caption);
    ImGui::SameLine(col);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::SliderFloat("##v", v, mn, mx, fmt);
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

// s299c — la misma fila, con un interruptor en vez de un deslizador. Misma columna de etiqueta que
// todo lo demás del panel, que es lo que mantiene la rejilla del rediseño.
bool neo_row_toggle(const char *id, const char *caption, bool *v, const char *tip)
{
    // 🚨 s299c — NO se recorta a la columna de etiqueta, y aquí está la diferencia con el
    // deslizador. Un deslizador necesita todo el ancho que queda, así que su etiqueta cede; una
    // casilla ocupa un cuadrado, así que hay sitio de sobra y recortar sólo servía para dejar
    // "only my zo…" en pantalla. Se recorta contra lo que queda de verdad, menos el hueco de la
    // casilla, y la casilla se coloca DESPUÉS del texto cuando éste se pasa de la columna.
    const float col = neo_label_col();
    const float box = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float room = std::max(col - 0.4f * neo_u(),
                                ImGui::GetContentRegionAvail().x - box - 0.4f * neo_u());
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    const std::string label = fit_text(caption, room);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip != nullptr ? tip : caption);
    // Si la etiqueta cabe en la columna, la casilla se alinea con el resto del panel y la rejilla
    // se mantiene. Si no cabe, va detrás del texto en vez de encima de él.
    if (ImGui::GetItemRectSize().x + 0.4f * neo_u() < col)
        ImGui::SameLine(col);
    else
        ImGui::SameLine(0.f, 0.4f * neo_u());
    const bool changed = ImGui::Checkbox("##v", v);
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

// Una baldosa de dato: rótulo pequeño arriba, número grande abajo, y una barra opcional que dice
// cuánto del presupuesto se ha gastado. Tres de estas cuentan de un vistazo lo que antes eran
// cuatro frases seguidas.
void neo_stat_tile(const char *id, float w, const char *caption, const char *value, ImU32 val_col,
                   float bar01, const char *tip)
{
    const float u = neo_u();
    const float h = 2.6f * u;
    const float pad = 0.42f * u;
    ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, neo_fade(NeoCol::Surface, 0.65f), 5.f);
    dl->AddText(ImGui::GetFont(), 0.64f * u, ImVec2(a.x + pad, a.y + 0.22f * u), neo_fade(NeoCol::TextDim, 0.95f), caption);
    dl->AddText(ImGui::GetFont(), 0.92f * u, ImVec2(a.x + pad, a.y + 0.98f * u), val_col, value);
    if (bar01 >= 0.f) {
        const float y = b.y - 0.24f * u;
        dl->AddLine(ImVec2(a.x + pad, y), ImVec2(b.x - pad, y), neo_fade(NeoCol::SurfaceHi, 0.9f), 2.f);
        const float t = std::min(1.f, std::max(0.f, bar01));
        if (t > 0.f)
            dl->AddLine(ImVec2(a.x + pad, y), ImVec2(a.x + pad + (b.x - a.x - 2.f * pad) * t, y), val_col, 2.f);
    }
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
}

// Un aviso: triángulo ámbar, la frase corta, y el porqué entero en el tooltip. 🚨 El ámbar sigue
// significando lo mismo que en GizmoNeotkoStyle — "algo va mal" — y nada más lo usa.
void neo_warn_row(const char *id, const char *text, const char *why, bool amber = true)
{
    const ImU32  col   = amber ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::TextDim);
    const float  u     = neo_u();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  gut   = 1.55f * u;   // el hueco del triángulo
    const float  wrap  = avail - gut - 0.3f * u;
    const ImVec2 ts    = ImGui::CalcTextSize(text, nullptr, false, wrap);
    const float  h     = std::max(1.3f * u, ts.y + 0.5f * u);
    ImGui::InvisibleButton(id, ImVec2(avail, h));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, (col & ~IM_COL32_A_MASK) | (24u << IM_COL32_A_SHIFT), 4.f);
    dl->AddRectFilled(a, ImVec2(a.x + 2.f, b.y), col, 1.f);
    draw_glyph(dl, ImVec2(a.x + 0.4f * u, a.y + (h - 0.8f * u) * 0.5f), 0.8f * u, Glyph::Warn, col);
    dl->AddText(nullptr, 0.f, ImVec2(a.x + gut, a.y + 0.25f * u), col, text, nullptr, wrap);
    if (why != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", why);
}

} // namespace

// -----------------------------------------------------------------------------
// El alzado
// -----------------------------------------------------------------------------
// 🔑 La pieza central del rediseño, y la razón por la que el panel dejó de ser una lista. Es una
// SECCIÓN por el plano de la inclinación, a ESCALA UNIFORME: un grado del deslizador es un grado en
// pantalla, y por eso se puede juzgar la forma mirándola en vez de leyendo tres números.
//
// Qué dibuja, y de dónde sale cada trazo:
//   · la cuña fantasma  = ±(z_low - z)·tan(ángulo RECOMENDADO), o el del pilar si es mayor.
//     🚨 s299 — antes esto era el tope del motor y lo de fuera no se seguía. Ya no: el motor
//     deriva su tope del ángulo que pediste, así que la cuña es la referencia de lo cómodo, y un
//     pilar que la desborda se imprime igual, sólo que con cuidado.
//   · el parche         = el contorno real de la huella, medido a lo largo de la dirección del
//                         aterrizaje (target_footprint_world).
//   · el pie            = foot_outline_world, que con el borde progresivo YA NO es el mismo.
//   · la rodilla        = z_low - desplazamiento/tan(ángulo), la misma cuenta que build_pillar_mesh.
//   · la cama           = z = 0.
//
// 🚨 Ni una sola de esas magnitudes se calcula aquí: todas se le piden a quien ya las tiene. Si el
// alzado y el sólido discreparan, sería un bug del motor y no del dibujo, que es exactamente lo que
// se quiere de un dibujo que sirve para decidir.
void GLGizmoSupportZones::render_elevation()
{
    constexpr double deg2rad = 0.01745329251994329576923690768489;

    const float  u  = neo_u();
    const float  w  = ImGui::GetContentRegionAvail().x;
    const float  h  = 9.6f * u;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##elevation", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    neo_draw_canvas(dl, p0, w, h);
    dl->PushClipRect(ImVec2(p0.x + 1.f, p0.y + 1.f), ImVec2(p0.x + w - 1.f, p0.y + h - 1.f), true);

    const ImU32 c_bed  = neo_col_u32(NeoCol::GridMajor);
    const ImU32 c_part = neo_fade(NeoCol::SurfaceHi, 0.85f);

    if (! m_has_target) {
        // El estado vacío no es un hueco: es el dibujo de lo que hay que hacer. Dos clics, en orden,
        // con la forma que van a producir en fantasma. Enseña el gesto sin gastar un párrafo.
        const float cx = p0.x + w * 0.5f, top = p0.y + 1.6f * u, bed = p0.y + h - 1.7f * u;
        dl->AddRectFilled(ImVec2(cx - 54.f, top - 16.f), ImVec2(cx + 54.f, top), c_part, 3.f);
        hatch_rect(dl, ImVec2(cx - 54.f, top - 16.f), ImVec2(cx + 54.f, top), neo_fade(NeoCol::Grid, 0.9f), 6.f);
        dl->AddLine(ImVec2(cx - 54.f, top), ImVec2(cx + 54.f, top), neo_fade(NeoCol::AccentBright, 0.85f), 2.2f);
        dashed_line(dl, ImVec2(cx - 16.f, top), ImVec2(cx + 30.f, bed), neo_fade(NeoCol::Accent, 0.55f), 1.6f);
        dashed_line(dl, ImVec2(cx + 16.f, top), ImVec2(cx + 62.f, bed), neo_fade(NeoCol::Accent, 0.55f), 1.6f);
        dl->AddLine(ImVec2(p0.x + 14.f, bed), ImVec2(p0.x + w - 14.f, bed), c_bed, 2.f);
        for (float t = p0.x + 14.f; t < p0.x + w - 14.f; t += 8.f)
            dl->AddLine(ImVec2(t, bed + 2.f), ImVec2(t - 5.f, bed + 8.f), neo_fade(NeoCol::Grid, 0.9f), 1.f);

        auto step_dot = [&](const ImVec2 &c, const char *n, bool live) {
            dl->AddCircleFilled(c, 0.52f * u, live ? neo_col_u32(NeoCol::Accent) : neo_col_u32(NeoCol::Surface), 18);
            dl->AddCircle(c, 0.52f * u, live ? neo_col_u32(NeoCol::AccentBright) : neo_fade(NeoCol::TextDim, 0.6f), 18, 1.4f);
            dl->AddText(ImGui::GetFont(), 0.68f * u, ImVec2(c.x - 0.17f * u, c.y - 0.40f * u),
                        live ? neo_col_u32(NeoCol::Ink) : neo_fade(NeoCol::TextDim, 0.9f), n);
        };
        step_dot(ImVec2(cx, top + 0.7f * u), "1", m_target_pick_mode);
        step_dot(ImVec2(cx + 2.6f * u, bed - 0.7f * u), "2", false);

        const std::string hint = _u8L("Pick the surface, then where it lands.");
        const ImVec2      ts   = ImGui::CalcTextSize(hint.c_str());
        dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + h - 1.15f * u), neo_fade(NeoCol::TextDim, 0.95f), hint.c_str());
        dl->PopClipRect();
        return;
    }

    // --- Los números, todos pedidos a quien los tiene ------------------------
    const std::vector<Vec2d> top_poly  = target_footprint_world();
    const std::vector<Vec2d> foot_poly = foot_outline_world();
    const double z_click = m_target_world_pos.z();
    const double z_low   = lean_top_z();
    const double z_land  = m_has_landing ? m_landing_world_pos.z() : 0.;
    const double off     = m_has_landing ? landing_offset_mm() : 0.;
    const double ang     = std::max(0.05, double(m_lean_angle_deg));
    const double ang_max = std::max(ang, max_lean_angle_deg());
    const double lean_h  = off / std::tan(ang * deg2rad);
    const double z_knee  = z_low - lean_h;

    // El eje de la sección: la dirección en la que se va el pie. Sin aterrizaje aún, el eje X del
    // mundo sirve igual de bien, porque no hay desplazamiento que enseñar.
    Vec2d centre { 0., 0. };
    for (const Vec2d &p : top_poly)
        centre += p;
    if (! top_poly.empty())
        centre /= double(top_poly.size());
    Vec2d dir(1., 0.);
    if (m_has_landing) {
        const Vec2d d = Vec2d(m_landing_world_pos.x(), m_landing_world_pos.y()) - centre;
        if (d.norm() > 1e-6)
            dir = d.normalized();
    }
    // La anchura de un contorno MEDIDA SOBRE EL EJE, no su diámetro: es lo que se vería al cortar.
    auto extent = [&dir](const std::vector<Vec2d> &poly, double &lo, double &hi) {
        lo = hi = 0.;
        if (poly.size() < 2)
            return;
        Vec2d c { 0., 0. };
        for (const Vec2d &p : poly)
            c += p;
        c /= double(poly.size());
        lo =  1e30;
        hi = -1e30;
        for (const Vec2d &p : poly) {
            const double t = (p - c).dot(dir);
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
    };
    double top_lo = 0., top_hi = 0., foot_lo = 0., foot_hi = 0.;
    extent(top_poly,  top_lo,  top_hi);
    extent(foot_poly, foot_lo, foot_hi);

    // --- La ventana del dibujo y su escala, que es ÚNICA para los dos ejes ---
    const double x_min = std::min({ top_lo, off + foot_lo, -0.5 });
    const double x_max = std::max({ top_hi, off + foot_hi,  0.5, off });
    const double z_hi  = std::max(z_click, z_low) + 0.2;
    const double z_lo  = std::min({ z_land, z_knee, 0. });
    const float  pad_x = 1.5f * u, pad_t = 0.9f * u, pad_b = 1.3f * u;
    const double span_x = std::max(1e-3, (x_max - x_min) * 1.10);
    const double span_z = std::max(1e-3, (z_hi - z_lo) * 1.06);
    const float  s = float(std::min((w - 2.f * pad_x) / span_x, (h - pad_t - pad_b) / span_z));

    const double x_mid = 0.5 * (x_min + x_max);
    const float  cx    = p0.x + w * 0.5f;
    const float  cy    = p0.y + pad_t + (h - pad_t - pad_b) * 0.5f;
    const double z_mid = 0.5 * (z_hi + z_lo);
    auto X = [&](double x) { return cx + float((x - x_mid) * s); };
    auto Y = [&](double z) { return cy - float((z - z_mid) * s); };

    // --- La cuña del motor ---------------------------------------------------
    // Lo que el corredor puede seguir, dibujado como forma. Un aterrizaje fuera de la cuña es un
    // aterrizaje que el motor no va a alcanzar, y así se ve sin leer un número.
    {
        const double reach_lo = (z_low - z_lo) * std::tan(ang_max * deg2rad);
        const ImVec2 wedge[3] = { ImVec2(X(0.), Y(z_low)),
                                  ImVec2(X(-reach_lo), Y(z_lo)),
                                  ImVec2(X(reach_lo), Y(z_lo)) };
        dl->AddConvexPolyFilled(wedge, 3, neo_col_u32(NeoCol::AccentGhost));
        dashed_line(dl, wedge[0], wedge[1], neo_fade(NeoCol::Accent, 0.45f), 1.2f);
        dashed_line(dl, wedge[0], wedge[2], neo_fade(NeoCol::Accent, 0.45f), 1.2f);
    }

    // --- La cama -------------------------------------------------------------
    if (0. >= z_lo - 1e-6 && 0. <= z_hi) {
        const float yb = Y(0.);
        dl->AddLine(ImVec2(p0.x + 6.f, yb), ImVec2(p0.x + w - 6.f, yb), c_bed, 1.8f);
        for (float t = p0.x + 8.f; t < p0.x + w - 6.f; t += 9.f)
            dl->AddLine(ImVec2(t, yb + 2.f), ImVec2(t - 5.f, yb + 8.f), neo_fade(NeoCol::Grid, 0.9f), 1.f);
    }

    // --- La pieza y el parche ------------------------------------------------
    {
        const float y_low = Y(z_low), y_top = std::min(y_low - 6.f, Y(z_hi));
        const float xa = X(top_lo), xb = X(top_hi);
        dl->AddRectFilled(ImVec2(xa, y_top), ImVec2(xb, y_low), c_part, 2.f);
        hatch_rect(dl, ImVec2(xa, y_top), ImVec2(xb, y_low), neo_fade(NeoCol::Grid, 0.95f), 6.f);
        // El parche tomado: la línea verde de "esto ya está cogido", el mismo verde que lleva en 3D.
        dl->AddLine(ImVec2(xa, y_low), ImVec2(xb, y_low), neo_col_u32(NeoCol::Optimal), 2.4f);
    }

    // --- El pilar ------------------------------------------------------------
    const bool bad = landing_out_of_reach() || (m_has_landing && pillar_crosses_object()) || footprint_too_narrow();
    const ImU32 c_line = bad ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright);
    const ImU32 c_fill = bad ? neo_fade(NeoCol::Warn, 0.16f) : neo_fade(NeoCol::Accent, 0.22f);
    if (m_has_landing) {
        // Tramo inclinado: del parche a la rodilla, con la sección abriéndose o cerrándose por el
        // borde progresivo. Dos trapecios y no un polígono de seis puntos, porque AddConvexPolyFilled
        // quiere convexidad y un pilar acampanado no la tiene garantizada.
        const ImVec2 a0(X(top_lo), Y(z_low)), a1(X(top_hi), Y(z_low));
        const ImVec2 b0(X(off + foot_lo), Y(z_knee)), b1(X(off + foot_hi), Y(z_knee));
        dl->AddQuadFilled(a0, a1, b1, b0, c_fill);
        dl->AddLine(a0, b0, c_line, 1.8f);
        dl->AddLine(a1, b1, c_line, 1.8f);

        if (z_knee - z_land > 1e-4) {
            const ImVec2 c0(X(off + foot_lo), Y(z_land)), c1(X(off + foot_hi), Y(z_land));
            dl->AddQuadFilled(b0, b1, c1, c0, c_fill);
            dl->AddLine(b0, c0, c_line, 1.8f);
            dl->AddLine(b1, c1, c_line, 1.8f);
            dl->AddLine(c0, c1, c_line, 1.8f);
            // La rodilla, marcada sólo cuando existe de verdad.
            neo_draw_node(dl, ImVec2(X(off), Y(z_knee)), 3.4f, c_line, true);
        } else {
            dl->AddLine(b0, b1, c_line, 1.8f);
        }

        // El ángulo, escrito donde se está midiendo.
        if (off > 0.05) {
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.0f°", double(m_lean_angle_deg));
            const ImVec2 mid((X(top_hi) + X(off + foot_hi)) * 0.5f + 8.f, (Y(z_low) + Y(z_knee)) * 0.5f - 7.f);
            neo_draw_pill(dl, mid, lab, c_line);
        }

        // Dónde apoya: la cama ya está dibujada, así que lo que hace falta marcar es la repisa.
        if (! m_landing_on_bed) {
            dl->AddRectFilled(ImVec2(X(off + foot_lo) - 12.f, Y(z_land)),
                              ImVec2(X(off + foot_hi) + 12.f, Y(z_land) + 7.f), c_part, 2.f);
            hatch_rect(dl, ImVec2(X(off + foot_lo) - 12.f, Y(z_land)),
                       ImVec2(X(off + foot_hi) + 12.f, Y(z_land) + 7.f), neo_fade(NeoCol::Grid, 0.95f), 6.f);
        }

        // La cota del desplazamiento, abajo del todo: el número que decide si esto es alcanzable.
        if (off > 0.05) {
            char lab[32];
            std::snprintf(lab, sizeof(lab), "%.1f mm", off);
            dim_line_h(dl, X(0.), X(off), p0.y + h - 0.55f * u, neo_fade(NeoCol::TextDim, 0.95f), lab);
            dashed_line(dl, ImVec2(X(0.), Y(z_low)), ImVec2(X(0.), p0.y + h - 0.55f * u), neo_fade(NeoCol::Grid, 1.f), 1.f, 3.f, 4.f);
        }
    } else {
        // Con parche y sin aterrizaje: la cuña ya dice hasta dónde se puede llegar. Lo que falta es
        // el segundo clic, y se dice donde se va a dar.
        const std::string hint = _u8L("Now click where it lands.");
        const ImVec2      ts   = ImGui::CalcTextSize(hint.c_str());
        dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + h - 1.15f * u), neo_fade(NeoCol::AccentBright, 0.95f), hint.c_str());
    }

    // La altura, sólo al pasar por encima: es un dato de consulta, no de decisión, y permanentemente
    // sería ruido sobre el dibujo.
    if (hovered) {
        char lab[48];
        std::snprintf(lab, sizeof(lab), "z %.2f → %.2f mm", z_low, z_land);
        neo_draw_pill(dl, ImVec2(p0.x + 8.f, p0.y + 6.f), lab, neo_fade(NeoCol::TextDim, 0.8f));
    }

    dl->PopClipRect();
}

// -----------------------------------------------------------------------------
// La lista de zonas
// -----------------------------------------------------------------------------
// 🔑 Tarjetas y no filas de texto, que era la queja concreta del dueño. Cada zona se reconoce por:
//   · su DIBUJO — un pilar en miniatura, hueco y a rayas si es estéril;
//   · su COLOR  — el del filamento de techo que le hayas puesto, en la tapa del pilar;
//   · su ESTADO — el medidor de capturas, ámbar y vacío cuando no coge nada.
// Y su número de prioridad grande, porque cuando dos zonas se solapan ese número es la respuesta.
void GLGizmoSupportZones::render_zone_cards()
{
    std::vector<std::string> fcolors;
    if (const auto *o = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
        fcolors = o->values;

    size_t max_lit = 1;
    for (const ZoneRow &z : m_zones)
        max_lit = std::max(max_lit, z.lit);

    const float u      = neo_u();
    const float avail  = ImGui::GetContentRegionAvail().x;
    const float card_h = 3.0f * u;      // dos líneas de texto con aire, sea cual sea la fuente
    const float icon   = 1.35f * u;     // los iconos de acción
    const float meter  = 3.6f * u;      // el medidor de capturas
    ImDrawList *dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < int(m_zones.size()); ++ i) {
        const ZoneRow &z = m_zones[i];
        const bool sel = (m_selected_zone == i);
        ImGui::PushID(z.volume_idx);

        const ImVec2 p = ImGui::GetCursorScreenPos();
        // 🚨 Los dos botones de acción se SOMETEN ANTES que la tarjeta, y no es un capricho de orden:
        // en ImGui el primero que reclama el hover se lo queda, así que sometiéndolos antes el clic
        // sobre el icono no llega también a la tarjeta de debajo. La comprobación manual de más abajo
        // es el segundo cinturón, por si alguna vez cambia esa regla.
        bool dup_hit = false, del_hit = false, unlock_hit = false, edit_hit = false;
        const int   n_icons = z.locked ? 4 : 2;
        const float icons_w = float(n_icons) * icon + float(n_icons - 1) * 0.25f * u;
        ImVec2 icon_a(p.x + avail - icons_w - 0.4f * u, p.y + (card_h - icon) * 0.5f);
        if (sel) {
            ImGui::SetCursorScreenPos(icon_a);
            dup_hit = neo_glyph_button("##dup", icon, Glyph::Copy, false, _u8L("Duplicate").c_str());
            ImGui::SetCursorScreenPos(ImVec2(icon_a.x + icon + 0.25f * u, icon_a.y));
            del_hit = neo_glyph_button("##del", icon, Glyph::Trash, true, _u8L("Delete").c_str());
            if (z.locked) {
                ImGui::SetCursorScreenPos(ImVec2(icon_a.x + 2.f * (icon + 0.25f * u), icon_a.y));
                // Verde `Optimal`, el mismo del candado de la izquierda: editar y estar bloqueada
                // son la misma cosa dicha dos veces.
                edit_hit = neo_glyph_button("##edit", icon, Glyph::Edit, false,
                                            _u8L("Edit this zone: the two clicks that built it are loaded back into the panel. It can be edited because it is locked.").c_str(),
                                            NeoCol::Optimal, true);
                ImGui::SetCursorScreenPos(ImVec2(icon_a.x + 3.f * (icon + 0.25f * u), icon_a.y));
                // Y el de soltar, del MISMO verde pero sin fondo: es la salida de ese estado, no
                // una acción destructiva de las de ámbar. Lo que quita es la edición, no la zona.
                unlock_hit = neo_glyph_button("##unlock", icon, Glyph::Unlock, false,
                                              _u8L("Unlock this zone: it stops being editable from here and becomes an ordinary mesh. There is no way back, but making another one is cheap.").c_str(),
                                              NeoCol::Optimal, false);
            }
        }

        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(avail, card_h));
        const bool hv      = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();

        // Fondo, riel de selección y borde.
        dl->AddRectFilled(a, b, sel ? neo_fade(NeoCol::AccentDim, 0.30f)
                                    : neo_fade(NeoCol::Surface, hv ? 0.85f : 0.45f), 6.f);
        if (sel) {
            dl->AddRect(a, b, neo_fade(NeoCol::Accent, 0.85f), 6.f, 0, 1.3f);
            dl->AddRectFilled(a, ImVec2(a.x + 3.f, b.y), neo_col_u32(NeoCol::AccentBright), 2.f);
        }
        // La que se está editando lleva su propio borde, y no el de selección: son dos cosas
        // distintas y hay que poder verlas a la vez.
        if (editing() && z.volume_idx == m_editing_volume_idx)
            dl->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f),
                        neo_col_u32(NeoCol::Optimal), 7.f, 0, 1.6f);

        // El pilar en miniatura. La tapa lleva el color del filamento de techo cuando lo tiene, que
        // es lo que convierte "esta zona lleva PVA" en algo que se ve desde el otro lado del panel.
        {
            const float gx = a.x + 0.6f * u, gy = a.y + 0.4f * u, gw = 1.05f * u, gh = card_h - 0.8f * u;
            const ImU32 body = z.sterile ? neo_col_u32(NeoCol::Warn)
                                         : (sel ? neo_col_u32(NeoCol::AccentBright) : neo_fade(NeoCol::Accent, 0.9f));
            const ImVec2 shape[4] = { ImVec2(gx + gw * 0.10f, gy + gh * 0.16f),
                                      ImVec2(gx + gw * 0.90f, gy + gh * 0.16f),
                                      ImVec2(gx + gw * 0.72f, gy + gh * 0.90f),
                                      ImVec2(gx + gw * 0.28f, gy + gh * 0.90f) };
            if (z.sterile) {
                // Estéril: hueco y a trazos. Un bloque que en pantalla parece lleno y no va a
                // producir un solo gramo no puede dibujarse macizo.
                for (int k = 0; k < 4; ++ k)
                    dashed_line(dl, shape[k], shape[(k + 1) % 4], body, 1.3f, 3.f, 3.f);
            } else {
                dl->AddConvexPolyFilled(shape, 4, (body & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
                dl->AddPolyline(shape, 4, body, ImDrawFlags_Closed, 1.4f);
            }
            // 🔑 s288 — el candado, en la esquina del pilar. Verde `Optimal`, que en este gizmo ya
            // significa "esto está tomado", y nunca ámbar: una zona bloqueada no es un problema.
            if (z.locked)
                draw_glyph(dl, ImVec2(gx + gw - 0.15f * u, gy + gh - 0.75f * u), 0.72f * u,
                           Glyph::Lock, neo_col_u32(NeoCol::Optimal));
            const ImU32 cap = z.roof_filament > 0 ? zone_filament_col(fcolors, z.roof_filament - 1) : body;
            dl->AddRectFilled(ImVec2(gx, gy + gh * 0.02f), ImVec2(gx + gw, gy + gh * 0.16f), cap, 2.f);
            dl->AddLine(ImVec2(gx - 1.f, gy + gh * 0.96f), ImVec2(gx + gw + 1.f, gy + gh * 0.96f),
                        neo_fade(NeoCol::TextDim, 0.7f), 1.4f);
        }

        // El número de prioridad, grande, y el nombre debajo en pequeño.
        {
            char num[16];
            std::snprintf(num, sizeof(num), "#%zu", z.priority + 1);
            const float tx = a.x + 2.2f * u;
            dl->AddText(ImGui::GetFont(), 1.02f * u, ImVec2(tx, a.y + 0.30f * u),
                        sel ? neo_col_u32(NeoCol::Ink) : neo_fade(NeoCol::Ink, 0.88f), num);
            const std::string nm = fit_text(z.name, avail - 2.2f * u - meter - 1.2f * u - (sel ? icons_w : 0.f));
            dl->AddText(ImGui::GetFont(), 0.70f * u, ImVec2(tx, a.y + 1.55f * u),
                        neo_fade(NeoCol::TextDim, 0.95f), nm.c_str());
        }

        // El medidor de capturas, a la derecha. Con la tarjeta elegida se corre hacia la izquierda
        // para dejarle sitio a los iconos: esconderlo haría que el dato desapareciera justo en la
        // zona que estás mirando, que es la única en la que de verdad importa.
        {
            const float mx1 = b.x - (sel ? icons_w + 0.9f * u : 0.7f * u), mx0 = mx1 - meter;
            if (z.sterile) {
                draw_glyph(dl, ImVec2(mx1 - 0.85f * u, a.y + 0.4f * u), 0.85f * u, Glyph::Warn, neo_col_u32(NeoCol::Warn));
                dl->AddText(ImGui::GetFont(), 0.64f * u, ImVec2(mx0, a.y + 1.55f * u), neo_col_u32(NeoCol::Warn),
                            fit_text(_u8L("catches nothing"), meter).c_str());
            } else {
                char cnt[32];
                std::snprintf(cnt, sizeof(cnt), "%zu", z.lit);
                const ImVec2 ts = ImGui::CalcTextSize(cnt);
                dl->AddText(ImGui::GetFont(), 0.78f * u, ImVec2(mx1 - ts.x, a.y + 0.45f * u),
                            neo_fade(NeoCol::Ink, 0.85f), cnt);
                const float y = a.y + card_h - 0.8f * u;
                dl->AddLine(ImVec2(mx0, y), ImVec2(mx1, y), neo_fade(NeoCol::SurfaceHi, 0.9f), 3.f);
                const float t = float(double(z.lit) / double(max_lit));
                dl->AddLine(ImVec2(mx0, y), ImVec2(mx0 + (mx1 - mx0) * std::min(1.f, t), y),
                            neo_col_u32(NeoCol::Accent), 3.f);
            }
        }

        if (hv)
            ImGui::SetTooltip("%s\n\n%s\n\n%s",
                              _u8L("Downward-facing surface samples found inside this block. One column of the grid can cross more than one surface, so this is a count and not a percentage.").c_str(),
                              _u8L("The number is the priority: when two zones overlap, the lower one keeps the shared area.").c_str(),
                              z.locked ? _u8L("Built by this gizmo and untouched since, so the two clicks that made it are still on record.").c_str()
                                       : (z.has_gesture
                                              ? _u8L("This zone was built here, but it has been moved, rotated or scaled since, so it is now an ordinary mesh.").c_str()
                                              : _u8L("An ordinary support enforcer volume: it was not built by this gizmo.").c_str()));

        ImGui::PopID();
        ImGui::Dummy(ImVec2(avail, 0.18f * u));

        // Las acciones se resuelven DESPUÉS de dibujar la tarjeta: borrar reconstruye la lista, y
        // hacerlo a mitad de un bucle que la está recorriendo es la manera clásica de leer basura.
        // 🚨 s299h — LAS TRES SE DIFIEREN, Y ÉSTE ES EL CRASH QUE SEGUÍA CAYENDO AL BORRAR.
        //
        // Borrar el volumen por el camino correcto (`delete_from_model_and_list`) arregló el árbol
        // de la lista, pero no el problema de FONDO: esa función llama a `select_item()`, que llama
        // a `part_selection_changed()`, que cambia la selección del canvas — y eso reentra en este
        // mismo gizmo (`data_changed()`, y en algunos casos `on_set_state`) MIENTRAS estamos dentro
        // de su frame de ImGui, recorriendo `m_zones`. Al volver, la mitad del estado que este
        // bucle está usando ya no es el mismo. Es la re-entrada de wx que el proyecto ya tiene
        // documentada, esta vez por la puerta de la lista de objetos.
        //
        // 🔑 `CallAfter` lo saca del render: la acción se ejecuta en el bucle de eventos, con el
        // frame de ImGui ya cerrado y sin nadie recorriendo nada. Es lo mismo que hace el reset del
        // plano de sección unas líneas más abajo, y lo que hacen los demás gizmos para tocar el
        // modelo desde un botón.
        //
        // ⛔ Y el `return` se queda: aunque la acción ya no ocurra aquí, este frame ha terminado de
        // dibujar una lista que en el siguiente será otra.
        if (del_hit) {
            wxGetApp().CallAfter([this]() { delete_selected_zone(); });
            return;
        }
        if (dup_hit) {
            wxGetApp().CallAfter([this]() { duplicate_selected_zone(); });
            return;
        }
        if (unlock_hit) {
            wxGetApp().CallAfter([this]() { unlock_selected_zone(); });
            return;
        }
        if (edit_hit) {
            begin_edit_zone(i);
            return;
        }
        if (clicked) {
            // 🚨 La comprobación manual del rectángulo de los iconos: segundo cinturón por si el
            // orden de sometido dejara de bastar.
            const ImVec2 m = ImGui::GetIO().MousePos;
            const bool over_icons = sel && m.x >= icon_a.x && m.x <= icon_a.x + icons_w &&
                                    m.y >= icon_a.y && m.y <= icon_a.y + icon;
            if (! over_icons) {
                m_selected_zone = sel ? -1 : i;
                apply_see_through();   // el bloque elegido se enciende y el anterior vuelve a su color
            }
        }
    }
}

// NEOTKO_SUPPORTZONES_TAG s286c F3 — los materiales de la zona seleccionada.
//
// 🔑 Vive AQUÍ y no en el panel de objeto de Orca, y es decisión del dueño con motivo escrito en el
// plan (§F3): el pestillo de `GUI_ObjectList.cpp:3810` sigue cerrado a propósito porque abrirlo
// ofrecería los cincuenta ajustes de soporte, y de esos sólo dos pueden cumplirse por zona sin
// regenerar el soporte. Aquí se elige exactamente qué se ofrece.
//
// 🚨 Se escribe en la config DEL VOLUMEN, que es lo que T3 ya enruta al backend y lo que el 3mf
// lleva solo (`Format/3mf.cpp:3164`). Ningún almacén propio: uno más que mantener y otro sitio del
// que se puede desincronizar.
//
// 🔑 s287 — el desplegable se fue y en su sitio hay FICHAS. Elegir filamento es elegir un COLOR: un
// menú que hay que abrir para ver los colores, y que además sólo enseña uno a la vez, es la forma
// más cara de hacer eso. Con fichas se ve la paleta entera y se elige en un clic en vez de tres.
void GLGizmoSupportZones::render_zone_materials()
{
    if (m_selected_zone < 0 || m_selected_zone >= int(m_zones.size()))
        return;
    const int obj_idx = current_object_idx();
    Model    &model   = wxGetApp().plater()->model();
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()))
        return;
    ModelObject *mo = model.objects[obj_idx];
    const int    vi = m_zones[m_selected_zone].volume_idx;
    if (mo == nullptr || vi < 0 || vi >= int(mo->volumes.size()))
        return;
    ModelVolume *mv = mo->volumes[vi];

    std::vector<std::string> fcolors;
    if (const auto *o = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
        fcolors = o->values;
    const int n_fil = int(fcolors.size());
    if (n_fil < 2)
        return;   // con un solo filamento no hay nada que elegir y el bloque sólo sería ruido

    // 🚨 s286c — SÓLO EL TECHO. Decisión suya tras probarlo, y con motivo de fondo, no un rodeo.
    //
    // `WipingExtrusions::is_support_overriddable()` (ToolOrdering.cpp) sólo deja PURGAR DENTRO del
    // soporte cuando su filamento es 0. Es decir: un cuerpo de soporte en "como el objeto" es el
    // vertedero de purga de Orca, y por eso al probarlo se veía alternar de color — está gastando
    // ahí el material que si no iría a la torre. Fijarle una herramienta al cuerpo renuncia a eso,
    // hace crecer la torre, y encima es la parte del soporte a la que menos le importa el material.
    //
    // El caso que motiva la feature sobrevive entero: masa de lo que sea, TECHO soluble. Que es
    // además el único sitio donde el material toca la pieza.
    int       roof        = mv->config.has("support_interface_filament") ? mv->config.opt_int("support_interface_filament") : 0;
    const int roof_before = roof;

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(_u8L("Roof filament").c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Only the roof of the zone takes its own filament: it is the part that touches the model. The body is left as the object has it, which also lets the slicer dump its purge in there instead of growing the wipe tower.").c_str());

    // Las fichas. La primera es "como el objeto" y lleva el cubo isométrico; las demás llevan el
    // color del slot y su número, con la tinta calculada sobre el propio color.
    const float chip = 1.6f * neo_u(), step = chip + 5.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int   per_row = std::max(1, int((avail + 5.f) / step));
    ImDrawList *dl = ImGui::GetWindowDrawList();

    for (int i = 0; i <= n_fil; ++ i) {
        if (i > 0 && (i % per_row) != 0)
            ImGui::SameLine(0.f, 5.f);
        ImGui::PushID(i);
        const bool on = (roof == i);
        ImGui::InvisibleButton("##chip", ImVec2(chip, chip));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool   hv = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            roof = i;

        if (i == 0) {
            dl->AddRectFilled(a, b, neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface), 5.f);
            draw_glyph(dl, ImVec2(a.x + 4.f, a.y + 4.f), chip - 8.f, Glyph::Cube,
                       neo_fade(NeoCol::TextDim, 0.95f));
        } else {
            const ImU32 c = zone_filament_col(fcolors, i - 1);
            dl->AddRectFilled(a, b, c, 5.f);
            char n[8];
            std::snprintf(n, sizeof(n), "%d", i);
            const ImVec2 ts = ImGui::CalcTextSize(n);
            dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), ink_on(c), n);
        }
        // El anillo de elegida, por FUERA del color: un borde que se comiera el color haría dudar
        // de qué filamento es el que está puesto.
        if (on)
            dl->AddRect(ImVec2(a.x - 2.f, a.y - 2.f), ImVec2(b.x + 2.f, b.y + 2.f),
                        neo_col_u32(NeoCol::AccentBright), 7.f, 0, 2.f);
        else if (hv)
            dl->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f),
                        neo_fade(NeoCol::Ink, 0.45f), 6.f, 0, 1.2f);
        if (hv)
            ImGui::SetTooltip("%s", i == 0 ? _u8L("Same as the object").c_str()
                                           : (_u8L("Filament") + " " + std::to_string(i)).c_str());
        ImGui::PopID();
    }

    if (roof > 0) {
        neo_warn_row("##roofwarn", _u8L("Two tools on the contact layers.").c_str(),
                     (_u8L("The roof and the body of this zone print with different filaments, so every layer that has both needs a tool change. That purge goes to the wipe tower.") + "\n\n" +
                      _u8L("Zones that touch and share their settings are printed as one column. Giving this one its own filament separates it from its neighbours, so the shape may change where they used to meet.")).c_str());
    }

    if (roof != roof_before) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Support zone material"));
        // 🚨 El cuerpo se escribe SIEMPRE a 0 ("como el objeto"). No es un descuido: es lo que lo
        // mantiene disponible como vertedero de purga (is_support_overriddable) y lo que evita
        // fijarle una herramienta a la parte del soporte a la que menos le importa.
        mv->config.set_key_value("support_filament",           new ConfigOptionInt(0));
        mv->config.set_key_value("support_interface_filament", new ConfigOptionInt(roof));
        // 🚨 REVISIÓN: aquí ponía `m_zones_dirty = true`, y eso vuelve a lanzar la sonda de F1 sobre
        // CADA zona — un árbol AABB por zona— en cada toque del control. Lo que capturan las zonas
        // no depende del material: la geometría no se ha movido. La lista no necesita reconstruirse,
        // sólo hay que relaminar.
        //
        // 🔑 s287: lo que SÍ hay que refrescar es el color de la tapa en la tarjeta, que se lee de
        // la config del volumen. Se escribe en la fila directamente, sin volver a sondar.
        m_zones[m_selected_zone].roof_filament = roof;
        if (wxGetApp().obj_list() != nullptr)
            wxGetApp().obj_list()->update_and_show_object_settings_item();
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        m_parent.set_as_dirty();
    }
}

// -----------------------------------------------------------------------------
// La tira de modos de vista
// -----------------------------------------------------------------------------
// Tres interruptores de icono en una fila, y debajo sólo los deslizadores de los que estén
// encendidos. Antes eran tres casillas con su etiqueta, tres deslizadores y tres frases: nueve
// líneas para tres cosas que se miran, no se leen.
void GLGizmoSupportZones::render_view_strip()
{
    const float sz = 1.9f * neo_u();

    if (neo_glyph_toggle("##vovh", sz, m_show_overhangs, Glyph::Overhang,
                         _u8L("Highlight overhangs").c_str())) {
        m_show_overhangs = ! m_show_overhangs;
        m_overhang_model_dirty = true;
        apply_overhang_highlight();
    }
    ImGui::SameLine(0.f, 6.f);
    if (neo_glyph_toggle("##vgap", sz, m_show_gaps, Glyph::GapMap,
                         // 🚨 §8: esto AVISA, no decide. "might need support" es deliberado — quien
                         // decide que hace falta soporte sigue siendo detect_overhangs() en el motor.
                         (_u8L("Show what still needs holding") + "\n\n" +
                          _u8L("Red marks surface that leans past the threshold and sits inside no zone: it might need support and nothing is holding it. Green is already caught by a zone.")).c_str())) {
        m_show_gaps = ! m_show_gaps;
        m_parent.set_support_zone_gaps(m_show_gaps && get_state() == On, overhang_normal_z_cut(), m_gap_step_mm);
    }

    // El estado de las zonas, en la misma fila: dos píldoras que dicen cuántas hay y cuántas no
    // cogen nada. Es lo primero que se quiere saber al abrir el gizmo.
    if (! m_zones.empty()) {
        size_t sterile = 0;
        for (const ZoneRow &z : m_zones)
            if (z.sterile)
                ++ sterile;
        char t[64];
        std::snprintf(t, sizeof(t), "%zu %s", m_zones.size(), _u8L("zones").c_str());
        ImGui::SameLine(0.f, 12.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (sz - ImGui::GetTextLineHeight() - 6.f) * 0.5f);
        neo_status_chip(t, neo_col_u32(NeoCol::Accent), false);
        if (sterile > 0) {
            std::snprintf(t, sizeof(t), "%zu %s", sterile, _u8L("sterile").c_str());
            ImGui::SameLine(0.f, 5.f);
            neo_status_chip(t, neo_col_u32(NeoCol::Warn), false);
        }
    }

    if (m_show_overhangs) {
        if (neo_row_slider("##ovhdeg", _u8L("overhangs").c_str(), &m_overhang_threshold_deg, 0.f, 90.f, "%.0f deg",
                           _u8L("Highlight overhangs").c_str())) {
            m_overhang_model_dirty = true;
            apply_overhang_highlight();
            // Mismo número para el sombreado y para el mapa: no pueden discrepar en pantalla.
            if (m_show_gaps)
                m_parent.set_support_zone_gaps(true, overhang_normal_z_cut(), m_gap_step_mm);
        }
    }
    if (m_show_gaps) {
        // El detalle del mapa es una preferencia de MIRAR, no un ajuste de impresión, y por eso no
        // está atado a support_base_pattern_spacing: si lo estuviera habría que cambiar cómo se
        // imprime el soporte para poder ver mejor.
        if (neo_row_slider("##gapstep", _u8L("map detail").c_str(), &m_gap_step_mm, 0.3f, 4.f, "%.1f mm",
                           _u8L("Red marks surface that leans past the threshold and sits inside no zone: it might need support and nothing is holding it. Green is already caught by a zone.").c_str()))
            m_parent.set_support_zone_gaps(true, overhang_normal_z_cut(), m_gap_step_mm);
    }
    // NEOTKO_SUPPORTZONES_TAG s299d — EL CORTE, en el mismo sitio donde estaba el fantasma.
    //
    // 🔑 Pedido por el dueño después de usar el painter de Orca: "la barra al lado de reset
    // direction permite ver el interior y se ve en rojo el interior del objeto que necesita
    // soportes". Es esa barra, el mismo `ObjectClipper` compartido, así que se comporta igual que
    // en los otros painters y el plano se puede reorientar desde ellos.
    //
    // El botón sólo aparece cuando hay corte: con la barra a cero no hay dirección que resetear, y
    // un botón que no hace nada es peor que no tenerlo. Mismo criterio que GLGizmoFdmSupports.
    if (m_c != nullptr && m_c->object_clipper() != nullptr) {
        float clp = float(m_c->object_clipper()->get_position());
        if (neo_row_slider("##clp", _u8L("section").c_str(), &clp, 0.f, 1.f, "%.2f",
                           _u8L("Cut the part away to look inside. It is the same section view the support painter has, so the plane keeps whatever direction you gave it there.").c_str()))
            m_c->object_clipper()->set_position_by_ratio(clp, true);
        if (clp != 0.f) {
            ImGui::SameLine();
            if (ImGui::SmallButton(_u8L("reset direction").c_str()))
                wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
        }
    }

    // ⛔ s299c — el deslizador del fantasma se va con el fantasma.
    //
    // 🔑 Y el de los marcadores se queda, que es la razón por la que en s287 se sacó de dentro del
    // bloque del fantasma: el fantasma era la PIEZA y esto son los MARCADORES. Aquella separación
    // es justo lo que hace que quitar uno no se lleve el otro por delante.
    if (m_show_gaps) {
        float marker_alpha = m_parent.get_support_zone_marker_alpha();
        if (neo_row_slider("##markera", _u8L("grid marks").c_str(), &marker_alpha, 0.10f, 1.0f, "%.2f",
                           _u8L("How strong the green grid and the red gap map are drawn. They are read through the part, so they do not depend on how ghosted the part is.").c_str())) {
            m_parent.set_support_zone_marker_alpha(marker_alpha);
            m_parent.set_as_dirty();
        }
    }
}

// -----------------------------------------------------------------------------
// Los avisos
// -----------------------------------------------------------------------------
// Todos juntos y en un solo sitio, en vez de repartidos por el panel donde cada uno cae. Un aviso
// que aparece a mitad de una columna de controles la desplaza entera y hace bailar el panel; aquí
// crecen hacia abajo y no mueven nada de lo que estabas usando.
// NEOTKO_SUPPORTZONES_TAG s304 — «SÓLO MIS ZONAS», EN UN SOLO SITIO.
//
// 🔑 El cuerpo era del interruptor de s299c y ahora lo comparten el interruptor y el aviso del
// cajón. Lo que hace por debajo ya existía en Orca —`support_type` en `normal(manual)`— pero está
// a tres menús de distancia y con un nombre que no dice esto.
//
// ⛔ No se toca la familia (árbol contra normal): eso lo decide `seed_support_defaults()` al crear,
// y cambiarlo aquí de rebote sería secuestrar un ajuste que el usuario no ha tocado. Sólo el
// auto/manual, que es exactamente lo que dice la etiqueta.
void GLGizmoSupportZones::apply_only_my_zones(bool only_mine)
{
    ModelObject *mo = const_cast<ModelObject *>(current_object());
    if (mo == nullptr)
        return;
    const bool tree = is_tree(effective_support_type());
    // Con snapshot, como el resto de overrides que pone este gizmo: un Ctrl+Z lo deshace.
    wxGetApp().plater()->take_snapshot(_u8L("Support zones: only my zones"));
    mo->config.set_key_value("support_type",
        new ConfigOptionEnum<SupportType>(only_mine ? (tree ? stTree     : stNormal)
                                                    : (tree ? stTreeAuto : stNormalAuto)));
    if (wxGetApp().obj_list() != nullptr)
        wxGetApp().obj_list()->update_and_show_object_settings_item();
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoSupportZones::render_issue_tray()
{
    // ⛔ s299 — aquí vivía el aviso de "el parche se pliega sobre sí mismo". Se ha QUITADO, no
    // movido: era un problema de la geometría de antes, donde el suelo del sólido era la
    // triangulación del parche aplastada y una banda que pasaba de la vertical se solapaba consigo
    // misma. La huella ya es un `ExPolygons` plano, así que ese solape no puede existir: Clipper lo
    // resolvió al unir. Un aviso que no puede dispararse es peor que ninguno.

    if (m_has_target && footprint_too_narrow())
        neo_warn_row("##w_narrow", _u8L("Too narrow to print").c_str(),
                     _u8L("Too narrow to print: this footprint is thinner than two extrusions and the support would be stripped away on the way down.").c_str());

    if (m_has_target && m_has_landing && pillar_crosses_object())
        neo_warn_row("##w_cross", _u8L("It goes through the part").c_str(),
                     _u8L("It goes through the part: this pillar would be printed inside the model and weld to it.").c_str());

    if (landing_off_bed())
        neo_warn_row("##w_offbed", _u8L("The foot hangs off the plate").c_str(),
                     _u8L("The foot hangs off the plate: part of this footprint is outside the printable area and would be cut away.").c_str());

    // A3, en dos voces. Si se pudo arreglar al entrar, esto sólo cuenta lo que se hizo y dónde
    // deshacerlo. Si el estilo SIGUE sin ser Snug con el gizmo abierto es que alguien lo cambió a
    // mano por debajo, y entonces vuelve a ser un aviso de verdad.
    if (support_style_blocks_corridor()) {
        if (m_has_target && m_has_landing && landing_offset_mm() > 0.5)
            neo_warn_row("##w_snug", _u8L("Support style is not Snug").c_str(),
                         _u8L("Support style is not Snug: the support grid re-aligns to every layer, so this lean will come out as steps instead of a slide.").c_str());
    } else if (m_forced_snug) {
        neo_warn_row("##w_snugset", _u8L("Support style set to Snug for this object.").c_str(),
                     _u8L("Support style set to Snug for this object, so a leaning column slides instead of stepping. It is an object setting: undo it, or remove it from the object list.").c_str(),
                     false);
    }

    // NEOTKO_SUPPORTZONES_TAG s301 — EL HUECO, QUE ES LA CONDICIÓN DE QUE ESTO SEA UN ÁRBOL.
    //
    // 🔑 Sin hueco entre la cabeza y el tocón no hay tramo que crecer: el motor ve un bloque
    // contiguo y baja como un pilar corriente. No es un fallo —sale una zona válida— pero es otra
    // cosa que la que el panel está prometiendo, así que se dice.
    if (block_tree_mode() && m_has_target && ! all_stumps().empty() && block_tree_gap_mm() <= 0.)
        neo_warn_row("##w_nogap", _u8L("The stump reaches the area").c_str(),
                     _u8L("The stump reaches the area: with nothing in between there is no stretch for the column to grow through, so this comes out as a plain block instead of a tree. Lower the stump, or aim at a surface higher up.").c_str());

    if (landing_out_of_reach()) {
        // Ámbar ganado: este enlace pide más inclinación de la que el motor puede seguir, y la
        // columna se quedaría detrás del dibujo. Decirlo aquí es la diferencia entre una herramienta
        // que informa y una que miente.
        // s301 — dos avisos con el mismo nombre y distinta cuenta: en el pilar lofteado la rodilla
        // saldría por debajo de la cama; en el árbol es una raíz a la que la columna no llega
        // gastando `d_max` en cada capa. Y ahí «Bring it into reach» no vale: sólo sabe mover el
        // tocón primario, así que con un tocón adicional lejos arreglaría el que no falla.
        if (block_tree_mode()) {
            neo_warn_row("##w_reach", _u8L("A stump is too far for this lean").c_str(),
                         _u8L("A stump is too far for this lean: the column cannot walk that far sideways in the height it has, so that root would be left behind. Steepen the lean, move the stump closer, or plant one nearer the area. The log names the layer and the island if it happens.").c_str());
        } else {
            neo_warn_row("##w_reach", _u8L("Too far for this lean").c_str(),
                         _u8L("Too far for this lean: the knee would come out below the bed. Steepen the lean, or bring the foot closer.").c_str());
            if (ImGui::SmallButton(_u8L("Bring it into reach").c_str()))
                bring_landing_into_reach();
        }
    }

    // El árbol, dicho ANTES de crear nada. Es un aviso con fecha de caducidad a propósito: crear el
    // pilar lo arregla, y el aviso cuenta que lo va a arreglar en vez de pedirle al usuario que
    // vaya a buscar el ajuste.
    if (is_tree(effective_support_type()))
        neo_warn_row("##w_tree", _u8L("Support type is Tree").c_str(),
                     _u8L("The tree generator does not know about the corridor, so the lean and the knee you draw here would not come out. Creating the pillar switches this object to Normal, keeping whether it was auto or manual. It is an object setting: undo it, or remove it from the object list.").c_str());

    // NEOTKO_SUPPORTZONES_TAG s304 — EL AUTOMÁTICO SIGUE ENCENDIDO, Y ES LA PREGUNTA QUE SE HACE
    // MIRANDO EL RESULTADO.
    //
    // 🔑 Dicho por el dueño con los tocones ya funcionando: «se generan soportes no deseados... la
    // solución para mí fue activar On Build Plate Only», y acto seguido cayó en que con
    // `normal(manual)` salía perfecto. El interruptor «only my zones» de s299c hace exactamente
    // eso y estaba a un dedo, arriba en esta misma ventana. O sea que no faltaba la función:
    // faltaba que el aviso llegara en el momento en el que uno se hace la pregunta.
    //
    // ⛔ Y NO se cambia el ajuste solo al plantar un tocón. Esa decisión ya está tomada por escrito
    // en `seed_support_defaults()` (s287-bis) y sigue valiendo: quien tiene `normal(auto)` y añade
    // una zona puede querer las dos cosas, y quitarle el automático por haber pintado un tocón
    // sería secuestrarle el objeto. Se avisa y se ofrece el clic; decide él.
    //
    // ⚠️ Sólo con zonas ya creadas. Con el gizmo abierto y nada dibujado, el automático no es un
    // problema de nadie y el aviso sería ruido.
    if (! m_zones.empty() && is_auto(effective_support_type())) {
        neo_warn_row("##w_auto", _u8L("Automatic support is also on").c_str(),
                     _u8L("Automatic support is also on: besides your zones, the slicer supports everything else it thinks needs holding, which is where the support you did not draw comes from. Turning it off leaves only what you painted.").c_str());
        if (ImGui::SmallButton(_u8L("Only my zones").c_str()))
            apply_only_my_zones(true);
    }

    if (m_has_target && m_has_landing && ! m_preview_model.is_initialized())
        // Ni un crash ni un silencio: la geometría salió degenerada.
        neo_warn_row("##w_degen", _u8L("Nothing to build there").c_str(),
                     _u8L("Nothing to build there: the landing is level with the surface, the footprint has been pulled in to nothing, or this lean cannot reach that far before hitting the bed.").c_str());
}

// -----------------------------------------------------------------------------
// El cuerpo del panel
// -----------------------------------------------------------------------------
void GLGizmoSupportZones::render_panel_body()
{
    if (m_zones_dirty)
        rebuild_zone_rows();

    constexpr double deg2rad = 0.01745329251994329576923690768489;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- Los modos de vista ---------------------------------------------------
    render_view_strip();

    // --- Las zonas que ya existen ---------------------------------------------
    neo_section(_u8L("Zones on this object").c_str());

    // NEOTKO_SUPPORTZONES_TAG s299c — SÓLO MIS ZONAS.
    //
    // 🔑 Pedido por el dueño con estas palabras: "los soportes se crean, pero a la vez crea todos
    // los demás soportes naturales que necesitaría el objeto, y debería ser sólo el creado por mí".
    //
    // Lo que hace por debajo ya existía en Orca —`support_type` en `normal(manual)`— pero estaba a
    // tres menús de distancia y con un nombre que no dice esto. Aquí es una línea, en el sitio
    // donde uno se hace la pregunta, y escribe el override EN EL OBJETO, así que se ve en la lista
    // de objetos y se puede quitar desde allí como cualquier otro.
    if (current_object() != nullptr) {
        bool only_mine = ! is_auto(effective_support_type());
        if (neo_row_toggle("##onlymine", _u8L("only my zones").c_str(), &only_mine,
                           (_u8L("Only the zones you drew get support. The automatic overhang detection is turned off for this object.") + "\n\n" +
                            _u8L("With it off, the slicer also supports everything else it thinks needs holding, and your zones are added on top.")).c_str()))
            // s304 — el cuerpo vive en `apply_only_my_zones()`: el aviso del cajón ofrece el mismo
            // cambio de un clic y los dos tienen que escribir exactamente lo mismo.
            apply_only_my_zones(only_mine);
    }

    if (m_zones.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("No support zones yet. A zone is a support enforcer volume: add one from the object list, or pick a surface below.").c_str());
        ImGui::PopStyleColor();
    } else {
        render_zone_cards();
        // ⛔ s301b — AQUÍ VIVÍAN «grab only at its roof» Y «print the whole block», Y SE HAN QUITADO.
        //
        // 🚨 Medido por el dueño en el gcode del primer build de s301: las dos son la causa de los
        // techos de soporte DENTRO de la pieza, y con las dos apagadas los pilares de círculo,
        // cuadrado y parche salen bien. Un interruptor cuyo único efecto medido es romper la pieza
        // no es una opción, es una trampa — y encima estaba encendido por defecto.
        //
        // 🔑 Ahora las dos nacen apagadas en `write_pillar_into()`, para todos los modos. Y no hay
        // migración que hacer: ninguna llegó a un build publicado, así que no existe un 3mf que las
        // traiga puestas. El motor las sigue entendiendo, que es lo que permitirá volver a medirlas
        // a mano cuando se toquen los soportes que no son de tocón.
        //
        // ⛔ s299e — y antes de éstos vivía aquí «land only at the end», fuera del panel por otra
        // razón: recorta dónde puede posarse el pilar y esta herramienta promete lo contrario.
        if (m_selected_zone >= 0)
            render_zone_materials();
    }

    // --- El pilar, nuevo o el que se está editando ----------------------------
    neo_section(editing() ? _u8L("Editing a zone").c_str() : _u8L("Build a pillar").c_str());

    // La barra de "estás editando la #N". 🔑 Un panel que hace dos cosas con los mismos controles
    // necesita que se vea CUÁL está haciendo — es la deuda que la auditoría del plan señalaba, y
    // ésta es la respuesta: un riel verde, el número, y la salida al lado.
    if (editing()) {
        size_t which = 0;
        for (const ZoneRow &z : m_zones)
            if (z.volume_idx == m_editing_volume_idx)
                which = z.priority + 1;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float bh = 1.5f * neo_u();
        ImGui::InvisibleButton("##editbar", ImVec2(avail * 0.68f, bh));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dl->AddRectFilled(a, b, neo_fade(NeoCol::Optimal, 0.16f), 5.f);
        dl->AddRectFilled(a, ImVec2(a.x + 2.f, b.y), neo_col_u32(NeoCol::Optimal), 1.f);
        draw_glyph(dl, ImVec2(a.x + 0.4f * neo_u(), a.y + (bh - 0.85f * neo_u()) * 0.5f), 0.85f * neo_u(),
                   Glyph::Edit, neo_col_u32(NeoCol::Optimal));
        char t[64];
        std::snprintf(t, sizeof(t), "%s #%zu", _u8L("Editing zone").c_str(), which);
        dl->AddText(ImVec2(a.x + 1.55f * neo_u(), a.y + (bh - ImGui::GetTextLineHeight()) * 0.5f),
                    neo_col_u32(NeoCol::Optimal), t);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("The controls below are editing this zone. Nothing is written until you apply, so leaving discards the changes.").c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(_u8L("Leave").c_str()))
            end_edit();
    }

    // Se dijo que la superficie del gesto ya no está. Es lo único que se puede decir con honradez:
    // reconstruir el pilar contra otra cara sería inventarse una zona que el usuario no dibujó.
    if (m_edit_lost_surface)
        neo_warn_row("##w_lostsurface", _u8L("That surface is not there any more").c_str(),
                     _u8L("The zone remembers the point and the normal of the surface it was built against, and neither matches the mesh now. The mesh was repaired, simplified or replaced. The zone is still printed as it is; it just cannot be edited from the gesture any more.").c_str());

    // Los dos pasos, como dos botones grandes con su icono. Un interruptor de icono en vez de una
    // casilla: es un MODO, y un modo activo tiene que verse desde el otro lado de la pantalla.
    {
        const float bh = 2.15f * neo_u();
        // s301 — con el pincel son TRES pasos: superficie, aterrizaje y tocones. Los otros modos
        // siguen teniendo dos, así que el ancho sale de cuántos hay y no de un número escrito.
        const int   n_steps = block_tree_mode() ? 3 : 2;
        const float bw = (avail - 6.f * float(n_steps - 1)) / float(n_steps);
        auto step_button = [&](const char *id, bool active, bool done, bool enabled, Glyph g,
                               const char *label, const char *tip) -> bool {
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(bw, bh));
            ImDrawList  *dl = ImGui::GetWindowDrawList();
            const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool   hv = ImGui::IsItemHovered() && enabled;
            const ImU32 bg = ! enabled ? neo_fade(NeoCol::Surface, 0.35f)
                                       : (active ? neo_fade(NeoCol::Accent, hv ? 1.0f : 0.88f)
                                                 : neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface));
            dl->AddRectFilled(a, b, bg, 6.f);
            if (done && ! active)
                dl->AddRect(a, b, neo_fade(NeoCol::Optimal, 0.8f), 6.f, 0, 1.3f);
            const ImU32 ink = ! enabled ? neo_fade(NeoCol::TextDim, 0.45f)
                                        : (active ? neo_col_u32(NeoCol::Ink)
                                                  : (done ? neo_col_u32(NeoCol::Optimal) : neo_fade(NeoCol::Ink, 0.85f)));
            const float gs = 1.15f * neo_u();
            draw_glyph(dl, ImVec2(a.x + 0.4f * neo_u(), a.y + (bh - gs) * 0.5f), gs, g, ink);
            dl->AddText(ImVec2(a.x + gs + 0.75f * neo_u(), a.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), ink,
                        fit_text(label, bw - gs - 1.1f * neo_u()).c_str());
            if (ImGui::IsItemHovered() && tip != nullptr)
                ImGui::SetTooltip("%s", tip);
            return pressed && enabled;
        };

        // 🚨 s288 — los dos pasos se apagan MIENTRAS SE EDITA, por la misma razón que "Move it
        // again": volver a dar los clics sobre una zona que ya existe rehace el gesto por un camino
        // que todavía no está resuelto y el pilar sale mal. Editar es ajustar los mandos; los dos
        // clics son para crear. Cuando ese camino se arregle, esto se enciende y ya está.
        if (step_button("##step1", m_target_pick_mode, m_has_target, ! editing(), Glyph::Target,
                        _u8L("1 · Surface").c_str(),
                        (_u8L("1 · Surface to hold up") + "\n\n" +
                         _u8L("Only surfaces that look downward can be picked, so the top of the part is never taken by mistake.") + "\n" +
                         _u8L("Several surfaces are stacked here. Use the wheel to go through them.")).c_str())) {
            m_target_pick_mode = ! m_target_pick_mode;
            if (! m_target_pick_mode) {
                m_have_hover_pos = false;
                m_candidates.clear();
            } else {
                m_landing_pick_mode = false;
                m_stump_pick_mode   = false;
            }
            m_parent.set_as_dirty();
        }
        ImGui::SameLine(0.f, 6.f);
        if (step_button("##step2", m_landing_pick_mode, m_has_landing, m_has_target && ! editing(), Glyph::Landing,
                        _u8L("2 · Landing").c_str(),
                        (editing() ? _u8L("While you are editing a zone the landing spot stays where it was. To move it, make a new zone: the two clicks are cheap.")
                                   : (_u8L("2 · Where it lands") + "\n\n" +
                                      _u8L("Click the bed, or a surface of the part below the one you picked."))).c_str())) {
            m_landing_pick_mode = ! m_landing_pick_mode;
            if (m_landing_pick_mode) {
                m_target_pick_mode = false;
                m_stump_pick_mode  = false;
            }
            m_parent.set_as_dirty();
        }

        // NEOTKO_SUPPORTZONES_TAG s301 — EL TERCER PASO: PLANTAR TOCONES.
        //
        // 🔑 El paso 2 sigue siendo el que hay: el tocón primario, sembrado a plomo y movible. Éste
        // añade los DEMÁS, que es lo que hace falta para un área grande — con un tocón a cada lado
        // del centro de un donut la columna se parte sola al bajar y cada mitad va a la suya.
        if (block_tree_mode()) {
            ImGui::SameLine(0.f, 6.f);
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), "%s (%d)", _u8L("3 · Stumps").c_str(),
                          int(all_stumps().size()));
            if (step_button("##step3", m_stump_pick_mode, ! m_extra_stumps.empty(),
                            m_has_target && ! editing(), Glyph::Landing, lbl,
                            (_u8L("3 · Plant the stumps") + "\n\n" +
                             // 🔑 s301b, dicho por él: «no me queda claro si realmente lo necesito o
                             // simplemente puedo». Un mando que no dice CUÁNDO hace falta es un mando
                             // que obliga a experimentar, así que la respuesta va primero y el gesto
                             // después.
                             _u8L("One is enough almost always: the first one is planted for you, plumb under what you painted.") + "\n" +
                             _u8L("Plant another only when the tray below says a root does not reach - a long or forked area needs a foot at each end, or the column would lean further than the angle allows.") + "\n\n" +
                             _u8L("Click the bed, or a shelf of the part, to plant one. Click a stump again to pull it out.") + "\n" +
                             _u8L("The column is born from the whole painted area and splits on the way down, each piece heading for its nearest stump.")).c_str())) {
                m_stump_pick_mode = ! m_stump_pick_mode;
                if (m_stump_pick_mode) {
                    m_target_pick_mode  = false;
                    m_landing_pick_mode = false;
                }
                m_parent.set_as_dirty();
            }
        }
    }

    // La línea viva del primer clic: qué hay bajo el cursor, en qué z, y de qué lado del umbral.
    // Una línea que cambia en vez de tres párrafos que aparecen y desaparecen.
    if (m_target_pick_mode) {
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        const int    n  = int(m_candidates.size());
        const Candidate *live = live_candidate();
        // 🚨 InvisibleButton y no Dummy: Dummy no tiene id, así que no es "hovereable" y el tooltip
        // de esta línea NUNCA se abriría. Costó verlo una vez; aquí queda escrito.
        ImGui::InvisibleButton("##livecand", ImVec2(avail, ImGui::GetTextLineHeight() + 6.f));
        const ImVec2 p = ImGui::GetItemRectMin();
        if (n == 0 || live == nullptr) {
            dl->AddText(ImVec2(p.x + 0.95f * neo_u(), p.y + 0.15f * neo_u()), neo_fade(NeoCol::TextDim, 0.9f),
                        _u8L("Nothing under the cursor.").c_str());
        } else {
            const ImU32 col = live->past_threshold ? neo_col_u32(NeoCol::AccentBright) : neo_col_u32(NeoCol::Slope);
            dl->AddCircleFilled(ImVec2(p.x + 0.35f * neo_u(), p.y + 0.52f * neo_u()), 0.22f * neo_u(), col, 12);
            char t[96];
            if (n > 1)
                std::snprintf(t, sizeof(t), "z = %.2f mm   ·   %d / %d", live->world_pos.z(), m_candidate_idx + 1, n);
            else
                std::snprintf(t, sizeof(t), "z = %.2f mm", live->world_pos.z());
            dl->AddText(ImVec2(p.x + 0.95f * neo_u(), p.y + 0.15f * neo_u()), col, t);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n\n%s",
                                  live->past_threshold
                                      ? _u8L("Past the threshold: the slicer already treats this as an overhang.").c_str()
                                      : _u8L("Under the threshold: the slicer would not support this on its own. You can still pick it.").c_str(),
                                  _u8L("Several surfaces are stacked here. Use the wheel to go through them.").c_str());
        }
    }

    // --- El alzado ------------------------------------------------------------
    render_elevation();

    // --- Las tres baldosas ----------------------------------------------------
    if (m_has_target) {
        const float tw = (avail - 12.f) / 3.f;   // tres baldosas con 6 px entre ellas
        // NEOTKO_SUPPORTZONES_TAG s299 — los tres tramos de la recomendación, en un solo sitio.
        //
        // 🔑 El color ya no dice "te has pasado del tope", porque tope no hay. Dice en qué terreno
        // estás: verde lo seguro, ámbar lo que hay que imprimir con cuidado, rojo lo que sólo sale
        // a velocidad baja. El número calculado (`a_adv`) sigue en el tooltip porque depende de la
        // altura de capa y de la boquilla, y esos sí cambian de una placa a otra.
        const double a_adv = max_lean_angle_deg();
        const double lean  = double(m_lean_angle_deg);
        char v[48], tip[320];

        std::snprintf(v, sizeof(v), "%.0f°", lean);
        std::snprintf(tip, sizeof(tip), "%s\n\n%s %.0f deg  ·  %s %.2f mm %s",
                      lean > 60. ? _u8L("Above 60 deg: only at low speed.").c_str()
                        : lean > 50. ? _u8L("Above 50 deg: needs enough width and top gap.").c_str()
                                     : _u8L("Safe range.").c_str(),
                      _u8L("Recommended up to").c_str(), a_adv,
                      _u8L("at this layer height a support line is").c_str(),
                      support_line_width_mm(),
                      _u8L("wide").c_str());
        neo_stat_tile("##t_lean", tw, _u8L("LEAN").c_str(), v,
                      lean > 60. ? neo_col_u32(NeoCol::Forbid)
                        : lean > 50. ? neo_col_u32(NeoCol::Warn)
                                     : neo_col_u32(NeoCol::Optimal),
                      float(lean / SupportZones::SUPPORT_ZONE_MAX_LEAN_DEG), tip);

        ImGui::SameLine(0.f, 6.f);
        if (block_tree_mode()) {
            // s301 — en el árbol de bloques la baldosa cambia de pregunta: no hay rodilla que
            // enseñar porque no hay tramo dibujado. Lo que sí importa es cuántas raíces tiene, que
            // es lo que decide si el área se reparte o se queda coja.
            char sv[32];
            std::snprintf(sv, sizeof(sv), "%d", int(all_stumps().size()));
            neo_stat_tile("##t_knee", tw, _u8L("STUMPS").c_str(), sv,
                          all_stumps().empty() ? neo_fade(NeoCol::TextDim, 0.9f)
                                               : neo_col_u32(NeoCol::AccentBright), -1.f,
                          (_u8L("No knee here: the stretch between the area and the stumps is not drawn, it grows.") + "\n\n" +
                           _u8L("The column is born from the whole painted area and contracts toward the nearest stump as it descends, spending at most one lean-step per layer.")).c_str());
        } else if (! m_has_landing) {
            neo_stat_tile("##t_knee", tw, _u8L("KNEE").c_str(), "·", neo_fade(NeoCol::TextDim, 0.9f), -1.f, nullptr);
        } else {
            const double offs   = landing_offset_mm();
            const double lean_h = offs / std::tan(std::max(0.05f, m_lean_angle_deg) * deg2rad);
            const double z_knee = lean_top_z() - lean_h;
            const double drop   = z_knee - m_landing_world_pos.z();
            if (offs < 0.05) {
                neo_stat_tile("##t_knee", tw, _u8L("KNEE").c_str(), "·", neo_fade(NeoCol::TextDim, 0.9f), -1.f,
                              _u8L("Straight down, no knee.").c_str());
            } else if (drop > 0.2) {
                std::snprintf(v, sizeof(v), "z %.1f", z_knee);
                std::snprintf(tip, sizeof(tip), "%s %.1f mm  ·  %s %.1f mm",
                              _u8L("Knee at z =").c_str(), z_knee, _u8L("then drops").c_str(), drop);
                neo_stat_tile("##t_knee", tw, _u8L("KNEE").c_str(), v, neo_col_u32(NeoCol::AccentBright), -1.f, tip);
            } else {
                // La rodilla existe, pero no queda tramo recto debajo. El número sigue siendo cierto
                // y lo que cambia es el porqué, así que cambia el tooltip y no el valor.
                std::snprintf(v, sizeof(v), "z %.1f", z_knee);
                neo_stat_tile("##t_knee", tw, _u8L("KNEE").c_str(), v, neo_col_u32(NeoCol::AccentBright), -1.f,
                              _u8L("The lean reaches all the way down: no drop left.").c_str());
            }
        }

        ImGui::SameLine(0.f, 6.f);
        if (! m_has_landing) {
            neo_stat_tile("##t_reach", tw, _u8L("REACH").c_str(), "·", neo_fade(NeoCol::TextDim, 0.9f), -1.f, nullptr);
        } else {
            const double asked = landing_offset_mm();
            const double av    = reach_radius_mm(m_landing_world_pos.z());
            std::snprintf(v, sizeof(v), "%.1f/%.1f", asked, av);
            std::snprintf(tip, sizeof(tip), "%.1f mm %s %.1f mm", asked, _u8L("of a reach of").c_str(), av);
            neo_stat_tile("##t_reach", tw, _u8L("REACH").c_str(), v,
                          landing_out_of_reach() ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright),
                          av > 1e-6 ? float(asked / av) : 1.f, tip);
        }
    }

    // --- Los mandos -----------------------------------------------------------
    // 🔑 s289, dicho por él: "al dar surface, como no está el botón de tipo, no se puede pintar
    // hasta que se crea, entonces no deja". Tenía razón y era un pez que se muerde la cola: la fila
    // de la forma sólo salía con un parche YA tomado, y con el pincel el parche se toma PINTANDO.
    // Así que la forma se elige también mientras el paso 1 está armado — que además es cuando uno
    // quiere elegirla, antes de tocar la pieza y no después.
    if (m_has_target || m_target_pick_mode) {
        // La forma de la huella, en cuatro iconos. 🚨 El seed fill es coplanar: en un techo plano
        // grande se lleva el techo ENTERO y el pilar sale de ese tamaño. Aquí es donde se decide
        // que no.
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(_u8L("footprint").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(neo_label_col());
        // 🔑 s299 — TRES, y el cuadrado deja de ser uno de ellos. Decisión del dueño: "círculo para
        // point/click, área agrandar/achicar, y brocha libre".
        //
        // El cuadrado no se va porque estorbe: se va porque era EL QUE MEJOR FUNCIONABA por el
        // motivo equivocado. Con el desplazamiento por vértice de antes, cuatro esquinas convexas
        // eran el único caso donde crecer daba una forma correcta, así que el cuadrado parecía el
        // bueno y las otras formas, rotas. Ahora crece Clipper y las tres van igual de bien, así
        // que sobra un modo que estaba ahí para tapar un bug.
        //
        // 🚨 s301c — Y AQUÍ ESTABA EL BUG QUE ÉL ARRASTRABA DESDE s299: «el cuadrado desaparece al
        // cambiar a pintar o círculo».
        //
        // La lista era `{ Patch, Round, Brush }` y el cuadrado se añadía SÓLO si era la forma
        // activa. La intención de arriba es buena —jubilarlo, porque era el que mejor funcionaba
        // por el motivo equivocado— pero «oculto salvo herencia» salió implementado como PUERTA DE
        // UN SOLO SENTIDO: al pulsar cualquier otra forma el botón se iba de la lista y no había
        // manera de volver salvo reabrir una zona ya guardada con él.
        //
        // 🔑 Un modo del que se puede salir y no entrar no es un modo jubilado, es un modo roto. Y
        // la razón para jubilarlo se cayó sola: con `offset_ex()` de Clipper las cuatro formas
        // crecen igual de bien, así que el cuadrado ya no tapa ningún bug — es una forma más, y hay
        // huellas que son cuadradas.
        std::vector<FootShape> shapes { FootShape::Patch, FootShape::Round, FootShape::Square, FootShape::Brush };
        auto glyph_of = [](FootShape f) {
            switch (f) {
            case FootShape::Round:  return Glyph::Round;
            case FootShape::Square: return Glyph::Square;
            case FootShape::Brush:  return Glyph::Brush;
            default:                return Glyph::Patch;
            }
        };
        auto title_of = [](FootShape f) {
            switch (f) {
            case FootShape::Round:  return _u8L("Round");
            case FootShape::Square: return _u8L("Square");
            case FootShape::Brush:  return _u8L("Paint");
            default:                return _u8L("Whole patch");
            }
        };
        const std::string cut_tip    = _u8L("Cut around the point you picked. The roof still follows the surface, so it stays flush on a curve.");
        const std::string brush_tip  = _u8L("Drag on the surface to mark the area you want held up. Shift-drag rubs it out. The mark is the size of the brush, so a small brush draws a narrow strip.");
        for (size_t i = 0; i < shapes.size(); ++ i) {
            if (i > 0)
                ImGui::SameLine(0.f, 5.f);
            char id[16];
            std::snprintf(id, sizeof(id), "##shape%d", int(shapes[i]));
            const std::string title = title_of(shapes[i]);
            if (neo_glyph_toggle(id, 1.6f * neo_u(), m_foot_shape == shapes[i], glyph_of(shapes[i]),
                                 shapes[i] == FootShape::Patch
                                     ? title.c_str()
                                     : (title + "\n\n" + (shapes[i] == FootShape::Brush ? brush_tip : cut_tip)).c_str())
                && m_foot_shape != shapes[i]) {
                m_foot_shape       = shapes[i];
                // Cambiar de forma no borra lo pintado: se puede ir y volver al pincel sin perder
                // el trazo, que es lo que uno espera de un modo y no de un botón destructivo.
                m_preview_dirty    = true;
                invalidate_patch();
                m_parent.set_as_dirty();
            }
        }

        // s299f — fuera o dentro. Va aquí, con la forma, porque decide DÓNDE cae lo que pintas y no
        // qué se hace con ello después.
        if (neo_row_toggle("##paintin", _u8L("paint inside").c_str(), &m_paint_inside,
                           (_u8L("Off: you paint the surface you can see, and only that one. It is what you want almost always.") + "\n\n" +
                            _u8L("On: the pick goes through the wall, so you can grab a surface inside a hollow part. Use the section view to see what you are doing.")).c_str())) {
            m_cand_mouse = Vec2d(-1e9, -1e9);   // la lista de candidatos se rehace con la regla nueva
            invalidate_patch();
            m_parent.set_as_dirty();
        }

        if (m_foot_shape != FootShape::Patch) {
            // 🚨 Una `std::string` temporal y un `c_str()` guardado es un puntero colgando en
            // cuanto acaba la expresión. La cadena se queda viva en una variable.
            // "expand" y no "side"/"across", decisión suya: el número no se lee como la medida de
            // un cuadrado, se lee como cuánto agrandas la zona. Que es para lo que se usa.
            const std::string lbl = (m_foot_shape == FootShape::Brush) ? _u8L("brush") : _u8L("expand");
            // 🚨 s289 — hasta 200 mm y no 60. El tope de antes se quedaba corto en cuanto la pieza
            // era mediana, y con «cubrir» la forma ya no depende del parche, así que crecer no
            // rompe nada: sólo ocupa más área.
            if (neo_row_slider("##footsize", lbl.c_str(), &m_foot_size_mm, 1.f, 200.f, "%.1f mm",
                               (m_foot_shape == FootShape::Brush ? brush_tip : cut_tip).c_str())) {
                m_preview_dirty = true;
                invalidate_patch();
                m_parent.set_as_dirty();
            }

            // RECORTAR / CUBRIR. Dos palabras y no un icono, porque la diferencia es de concepto y
            // un dibujo no la dice: la misma forma, una vez metida dentro del parche y otra vez
            // mandando ella.
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(_u8L("area").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(neo_label_col());
            const std::string mode_lbl[2] = { _u8L("cut"), _u8L("cover") };
            const std::string mode_tip[2] = {
                _u8L("Cut: the shape is trimmed to the surface you picked, so it can never be bigger than the patch. The roof is the surface itself."),
                _u8L("Cover: the shape IS the section of the pillar, so it can be bigger than the patch and grow as far as you want. The roof still follows the surface where there is one, and stays flat where there is not. It holds more area than it needs, which is the trade.")
            };
            for (int i = 0; i < 2; ++ i) {
                if (i > 0)
                    ImGui::SameLine(0.f, 5.f);
                const bool on = (m_shape_covers == (i == 1));
                ImGui::PushStyleColor(ImGuiCol_Button,        on ? neo_col(NeoCol::Accent) : neo_col(NeoCol::Surface));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? neo_col(NeoCol::AccentBright) : neo_col(NeoCol::SurfaceHi));
                ImGui::PushStyleColor(ImGuiCol_Text,          on ? neo_col(NeoCol::Ink) : neo_col(NeoCol::TextDim));
                const bool pressed = ImGui::Button((mode_lbl[i] + "##covermode").c_str(),
                                                   ImVec2(3.4f * neo_u(), 0.f));
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", mode_tip[i].c_str());
                if (pressed && ! on) {
                    m_shape_covers  = (i == 1);
                    m_preview_dirty = true;
                    invalidate_patch();
                    m_parent.set_as_dirty();
                }
            }
        }

        if (m_foot_shape == FootShape::Brush) {
            // Una línea de estado y un botón, nada más. El pincel se usa en el 3D, no aquí.
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(_u8L("marks").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(neo_label_col());
            char msg[128];
            if (m_stamps.empty())
                std::snprintf(msg, sizeof(msg), "%s", _u8L("drag on the surface").c_str());
            else
                std::snprintf(msg, sizeof(msg), "%d", int(m_stamps.size()));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  m_stamps.empty() ? neo_col(NeoCol::TextDim) : neo_col(NeoCol::AccentBright));
            ImGui::TextUnformatted(msg);
            ImGui::PopStyleColor();
            if (! m_stamps.empty()) {
                ImGui::SameLine(0.f, 8.f);
                if (ImGui::SmallButton(_u8L("clear").c_str())) {
                    m_stamps.clear();
                    clear_painted();
                    m_brush_seed_facet = -1;
                    m_preview_dirty    = true;
                    invalidate_patch();
                    m_parent.set_as_dirty();
                }
            }
        }

        // NEOTKO_SUPPORTZONES_TAG s301 — EL TOCÓN: UN TAMAÑO Y UNA CUENTA.
        //
        // 🔑 El §9.3 del estudio dejaba abierto qué pasaba con `expand` en el modelo nuevo. La
        // respuesta es que no pasa nada: con el pincel `expand` ya era el TAMAÑO DE LA BROCHA y
        // sigue siéndolo. El tocón necesita el suyo, y es otro mando porque es otra cosa — uno dice
        // cuánto pintas y el otro cuánto pie plantas.
        //
        // ⛔ Y no hay mando de ALTURA (decisión suya): la altura de un tocón no es algo que se juzgue
        // mirándola, sólo tiene que dar unas capas de imán. Vive como constante en la cabecera.
        if (block_tree_mode()) {
            if (neo_row_slider("##stumpsize", _u8L("stump").c_str(), &m_stump_size_mm, 1.f, 40.f, "%.1f mm",
                               (_u8L("How wide each stump is where it stands.") + "\n\n" +
                                _u8L("The column narrows down to this on the last few layers, so a stump much smaller than the area you painted starves it. The log says so if it happens.")).c_str())) {
                m_preview_dirty         = true;
                m_footprint_model_dirty = true;
                m_parent.set_as_dirty();
            }

            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(_u8L("stumps").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(neo_label_col());
            const size_t n_st = all_stumps().size();
            char stmsg[128];
            if (n_st == 0)
                std::snprintf(stmsg, sizeof(stmsg), "%s", _u8L("none yet").c_str());
            else if (m_extra_stumps.empty())
                std::snprintf(stmsg, sizeof(stmsg), "1  %s", _u8L("(plumb, under the area)").c_str());
            else
                std::snprintf(stmsg, sizeof(stmsg), "%d", int(n_st));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  n_st == 0 ? neo_col(NeoCol::TextDim) : neo_col(NeoCol::AccentBright));
            ImGui::TextUnformatted(stmsg);
            ImGui::PopStyleColor();
            if (! m_extra_stumps.empty()) {
                ImGui::SameLine(0.f, 8.f);
                if (ImGui::SmallButton((_u8L("clear") + "##stumps").c_str())) {
                    // ⛔ Sólo los ADICIONALES. El primario es el aterrizaje y se mueve con «Move it
                    // again»; borrarlo desde aquí dejaría el árbol sin ningún pie y sin forma de
                    // recuperarlo salvo repintando.
                    m_extra_stumps.clear();
                    m_preview_dirty         = true;
                    m_footprint_model_dirty = true;
                    m_parent.set_as_dirty();
                }
            }
        }
    }

    if (m_has_target) {
        // El borde, en las dos alturas. Redibuja en vivo, y ahora también en el alzado, así que el
        // par de números se juzga mirando la forma en vez de descifrando dos signos.
        const float inset_max  = float(max_inset_mm());
        const float outset_max = float(max_outset_mm());
        if (m_footprint_shrink_mm > inset_max) m_footprint_shrink_mm = inset_max;
        if (m_footprint_base_mm   > inset_max) m_footprint_base_mm   = inset_max;
        // 🔑 El tope ENCOGE conforme metes borde, y es correcto: el desplazamiento por vértice no
        // sabe colapsar, así que hay un punto a partir del cual la forma se cruzaría.
        std::string taper;
        if (m_footprint_base_mm > m_footprint_shrink_mm)
            taper = _u8L("Tapered: wide where it holds, narrow where it stands. Less material, less to break off.");
        else if (m_footprint_base_mm < m_footprint_shrink_mm)
            taper = _u8L("Flared: wider at the foot than at the patch. Steadier on the bed, more material.");
        else if (m_footprint_shrink_mm != 0.f)
            taper = _u8L("Same all the way up.");
        else
            taper = _u8L("Edge exactly as picked.");

        // 🚨 s289 — un tope POR LADO, no uno simétrico. Meter y sacar no se cruzan por lo mismo ni
        // en el mismo punto: en una huella convexa sacar no tiene tope geométrico ninguno, y
        // aplicarle el del metido —que en una banda estrecha es medio milímetro— era el "es
        // demasiado poco" que él veía. Los dos números salen de la misma fórmula cerrada del borde,
        // cada uno mirando las aristas de su signo.
        const float edge_lo = - std::max(0.1f, outset_max);
        const float edge_hi =   std::max(0.1f, inset_max);
        if (m_footprint_shrink_mm < edge_lo) m_footprint_shrink_mm = edge_lo;
        if (m_footprint_base_mm   < edge_lo) m_footprint_base_mm   = edge_lo;
        if (neo_row_slider("##shrink", _u8L("head").c_str(), &m_footprint_shrink_mm,
                           edge_lo, edge_hi, "%.1f mm", taper.c_str())) {
            m_preview_dirty = true;
            invalidate_patch();
            m_parent.set_as_dirty();
        }
        // ⛔ s301 — el `foot` no se ofrece en el árbol de bloques. Ahí el pie no es el anillo de
        // abajo de un pilar: es el TOCÓN, y tiene su propio mando arriba. Dejar este deslizador
        // sería ofrecer un número que no cambia nada de lo que se ve, que es peor que no ofrecerlo.
        if (! block_tree_mode() &&
            neo_row_slider("##shrinkbase", _u8L("foot").c_str(), &m_footprint_base_mm,
                           edge_lo, edge_hi, "%.1f mm", taper.c_str())) {
            m_preview_dirty = true;
            invalidate_patch();
            m_parent.set_as_dirty();
        }

        // s289 — el volcado de geometría. Sólo aparece con el canal encendido
        // (ORCA_DEBUG_SUPPORTZONES / ORCA_DEBUG_ALL): es una herramienta de depuración y no tiene
        // por qué estar en el panel de nadie más.
        if (NeoDebug::is_enabled(NeoDebug::SUPPORTZONES)) {
            if (ImGui::SmallButton(_u8L("dump geometry").c_str())) {
                TriangleMesh dbg;
                const bool ok = build_pillar_mesh(dbg);
                dump_geometry("panel", ok ? &dbg : nullptr);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Writes the whole decision chain to the SUPPORTZONES log, plus the solid as an .obj you can open anywhere. No 3mf export needed.").c_str());
        }
    }

    if (m_has_target && m_has_landing) {
        // 🔑 s299 — ESTE DESLIZADOR YA NO TIENE CANDADO, y es el cambio que pidió el dueño.
        //
        // Antes su tope era `max_lean_angle_deg()`, o sea `atan(k·ancho / altura_de_capa)`. Con
        // capa 0.32 eso son 42°, y con altura de capa variable el mismo bloque se inclinaba
        // distinto en cada tramo. Ahora el ángulo es el DATO y el motor deriva de él su tope por
        // capa, así que se puede pedir lo que se quiera hasta el máximo duro.
        //
        // Lo que queda de `max_lean_angle_deg()` es la RECOMENDACIÓN: el color del deslizador y lo
        // que dice el tooltip. Pasarse es legítimo, no avisarlo no lo sería.
        const double a_adv = max_lean_angle_deg();
        char tip[320];
        std::snprintf(tip, sizeof(tip), "%s %.0f deg %s %.2f mm %s.\n%s",
                      _u8L("Recommended up to").c_str(), a_adv,
                      _u8L("for this layer height and a support line of").c_str(),
                      support_line_width_mm(),
                      _u8L("wide").c_str(),
                      double(m_lean_angle_deg) > 60.
                          ? _u8L("Above 60 deg: only at low speed.").c_str()
                          : double(m_lean_angle_deg) > 50.
                              ? _u8L("Above 50 deg: needs enough width and top gap.").c_str()
                              : _u8L("Safe range.").c_str());
        if (neo_row_slider("##leanangle", _u8L("lean").c_str(), &m_lean_angle_deg,
                           1.f, float(SupportZones::SUPPORT_ZONE_MAX_LEAN_DEG), "%.0f deg", tip)) {
            m_preview_dirty = true;
            m_reach_model_r = -1.;   // la franja depende del ángulo: se vuelve a construir
            m_parent.set_as_dirty();
        }

        // Dónde apoya, y el pestillo. §4.2-bis es una regla que el usuario tiene que poder LEER: el
        // clic es lo que la eligió, así que el panel dice cuál eligió.
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("##landline", ImVec2(avail * 0.62f, ImGui::GetTextLineHeight() + 6.f));
        const ImVec2 p  = ImGui::GetItemRectMin();
        const ImU32 col = m_landing_locked ? neo_col_u32(NeoCol::Optimal) : neo_col_u32(NeoCol::AccentBright);
        draw_glyph(dl, ImVec2(p.x + 1.f, p.y + 0.1f * neo_u()), 0.85f * neo_u(), Glyph::Landing, col);
        char t[96];
        if (m_landing_on_bed)
            std::snprintf(t, sizeof(t), "%s", _u8L("Lands on the bed").c_str());
        else
            std::snprintf(t, sizeof(t), "%s  z = %.2f", _u8L("Lands on the part").c_str(), m_landing_world_pos.z());
        dl->AddText(ImVec2(p.x + 1.15f * neo_u(), p.y + 0.15f * neo_u()), col, t);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_landing_on_bed
                                        ? _u8L("Lands on the bed · goes straight down from there").c_str()
                                        : _u8L("Lands on the part · stops there").c_str());
        ImGui::SameLine();
        if (editing()) {
            // 🚨 s288, visto por él: mover el aterrizaje EDITANDO reconstruye el pilar de una forma
            // que sale mal. No es el botón, que ya hace lo que dice — es que rehacer el gesto sobre
            // una zona que ya existe destapa algo del camino de reconstrucción que no está resuelto.
            // Apagado a conciencia y con sesión propia pendiente: prefiero un mando que no está a
            // uno que rompe la pieza sin decirlo.
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(_u8L("fixed while editing").c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("While you are editing a zone the landing spot stays where it was. To move it, make a new zone: the two clicks are cheap.").c_str());
        } else if (m_landing_locked) {
            if (ImGui::SmallButton(_u8L("Move it again").c_str())) {
                // 🚨 s288, visto por él editando: quitar el pestillo NO basta. El aterrizaje sólo
                // sigue al cursor cuando `m_landing_pick_mode && ! m_landing_locked` (on_mouse), y
                // al crear funcionaba de carambola porque el modo se quedaba encendido del clic
                // anterior. Editando se entra con el modo APAGADO —el aterrizaje ya está decidido—
                // así que el botón no hacía nada.
                // El botón dice "muévelo otra vez": tiene que dejarte en condiciones de moverlo,
                // sin depender de por dónde hayas llegado hasta él.
                m_landing_locked    = false;
                m_landing_pick_mode = true;
                m_target_pick_mode  = false;   // los dos modos de picking nunca van a la vez
                m_parent.set_as_dirty();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(_u8L("Following the cursor. Click to leave it there.").c_str());
            ImGui::PopStyleColor();
        }
    }

    // --- Lo que va mal, todo junto --------------------------------------------
    render_issue_tray();

    // --- Crear ----------------------------------------------------------------
    ImGui::Spacing();
    {
        // NEOTKO_SUPPORTZONES_TAG s300g — LA TRAVESÍA YA NO SE PUEDE CREAR, SÓLO SE PODÍA VER.
        //
        // 🚨 Esto ya avisaba desde s286 ("It goes through the part") pero el aviso sólo elegía un
        // color: el botón seguía dejando crear la zona. Medido en s300g: un pilar que cruzaba el
        // toro de lado a lado se creó sin que nada lo impidiera, y de ahí salió media tarde de
        // buscar en el motor un fallo que estaba en el editor.
        //
        // El §1 del plan se compromete con "un editor que no miente: no dibuja columnas que luego
        // no salen". Un pilar enterrado en la pieza es exactamente eso, y encima peor que no tener
        // pilar: se imprime y se suelda a la pared.
        const bool crosses = m_has_target && m_has_landing && pillar_crosses_object();
        const bool ready = m_has_target && m_has_landing && m_preview_model.is_initialized() && ! crosses;
        const bool edit  = editing();
        const float bh = 2.3f * neo_u();
        // 🚨 El disparo va con el RETORNO del botón (soltar), no con IsItemClicked (pulsar): crear
        // el pilar es una acción con snapshot de deshacer, y las acciones que modifican el modelo se
        // confirman al soltar, como cualquier botón de ImGui. Que se pueda salir arrastrando fuera
        // sin crear nada no es un detalle menor cuando el ratón está encima del 3D.
        const bool create_pressed = ImGui::InvisibleButton("##create", ImVec2(avail, bh));
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool   hv = ImGui::IsItemHovered() && ready;
        dl->AddRectFilled(a, b, ready ? neo_fade(NeoCol::Accent, hv ? 1.0f : 0.85f)
                                      : neo_fade(NeoCol::Surface, 0.5f), 7.f);
        if (ready)
            dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 7.f, 0, 1.4f);
        const ImU32 ink = ready ? neo_col_u32(NeoCol::Ink) : neo_fade(NeoCol::TextDim, 0.55f);
        const std::string label = edit ? _u8L("Apply changes") : _u8L("Create the pillar");
        const ImVec2      ts    = ImGui::CalcTextSize(label.c_str());
        const float gs = 1.2f * neo_u();
        draw_glyph(dl, ImVec2((a.x + b.x - ts.x) * 0.5f - gs - 0.5f * neo_u(), a.y + (bh - gs) * 0.5f), gs, Glyph::Pillar, ink);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + (bh - ts.y) * 0.5f), ink, label.c_str());
        if (create_pressed && ready) {
            if (edit)
                apply_edit();
            else
                create_pillar();
        }
        if (! ready && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", crosses ? _u8L("This pillar goes through the part: move the landing spot to a side where the way down is clear.").c_str()
                                            : m_has_target ? _u8L("Click the bed, or a surface of the part below the one you picked.").c_str()
                                                 : _u8L("Only surfaces that look downward can be picked, so the top of the part is never taken by mistake.").c_str());
    }

    // El botón de soltar el parche tomado, discreto y al final: se usa poco, pero cuando se necesita
    // hay que encontrarlo. ⛔ Editando no se ofrece: soltar el parche dejaría la edición sin la
    // superficie que la define, y "salir" ya existe arriba y es lo que se quiere en ese caso.
    if (m_has_target && ! editing()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::Text("%s  z = %.2f mm", _u8L("Surface picked:").c_str(), m_target_world_pos.z());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(_u8L("Clear").c_str())) {
            m_has_target       = false;
            m_target_facet_idx = -1;
            m_has_landing      = false;
            m_preview_dirty    = true;
            m_parent.set_as_dirty();
        }
    }
}

} // namespace GUI
} // namespace Slic3r
// NEOTKO_SUPPORTZONES_TAG_END
