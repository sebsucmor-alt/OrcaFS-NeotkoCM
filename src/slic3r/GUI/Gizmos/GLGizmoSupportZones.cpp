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
const ColorRGBA kPaintAreaCol (0.35f, 0.80f, 1.00f, 0.22f);

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
    // SelectionInfo only. The ObjectClipper was here in the first build and came back out: see
    // apply_see_through() for why a cut is the wrong instrument for this particular tool.
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo));
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
    m_hover_model_facet = m_target_model_facet = -1; // force the overlays to rebuild
    m_overhang_model_dirty = true;
    m_candidates.clear();
}

void GLGizmoSupportZones::clear_pick()
{
    m_target_pick_mode  = false;
    m_landing_pick_mode = false;
    m_has_target        = false;
    m_has_landing       = false;
    m_landing_locked    = false;
    m_mouse_down        = false;
    m_painting          = false;
    m_paint_area_model.reset();
    m_paint_area_facet  = -1;
    m_stamps.clear();
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

    const int n_facets = int(m_mesh.its.indices.size());
    for (const AABBMesh::hit_result &h : hits) {
        if (! h.is_hit())
            continue;
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
            // paredes por dentro del pilar. Se acepta, y `build_patch()` le da la vuelta al
            // triángulo para que el parche quede orientado a una sola cara.
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
    // ✅ s289 — y ahora es literalmente el mismo `PatchGeom` que va a acabar siendo el techo del
    // sólido: recortado exacto, soldado y orientado. Antes el resaltado usaba el filtro por
    // centroide y el pilar también, así que los dos mentían a la vez y a nadie le chirriaba.
    const PatchGeom *pg = patch(facet_idx, centre_xy);
    if (pg == nullptr)
        return;

    // 🚨 `PatchGeom` está en MUNDO y estos overlays se dibujan con la matriz del objeto puesta en
    // el shader (`view_model_matrix * m_world_trafo`), así que hay que volver. Dibujarlos en mundo
    // con esa matriz puesta los colocaría el doble de lejos, que es el bug clásico de esta familia.
    const Transform3d inv = m_world_trafo.inverse();
    const double      lift = 0.10;   // por la normal de cada vértice, contra el z-fighting

    indexed_triangle_set its;
    its.vertices.reserve(pg->V.size());
    its.indices.reserve(pg->F.size());
    for (size_t i = 0; i < pg->V.size(); ++ i) {
        const Vec3d w = inv * (pg->V[i] + pg->vnormal[i] * lift);
        its.vertices.emplace_back(float(w.x()), float(w.y()), float(w.z()));
    }
    its.indices = pg->F;
    if (its.indices.empty())
        return;
    model.init_from(its);
    model.set_color(col);
}

// s289 — el lienzo dibujado. Sin esto el pincel es adivinar: pintas, se corta, y no hay forma de
// saber por qué. Con el lienzo encendido la respuesta está en pantalla antes de empezar.
void GLGizmoSupportZones::build_paint_area_model(int seed_facet)
{
    m_paint_area_model.reset();
    const std::vector<int> region = collect_paint_region(seed_facet);
    if (region.empty())
        return;
    const float lift = 0.06f;   // por debajo del 0.10 de las marcas: el lienzo va DETRÁS
    indexed_triangle_set its;
    its.vertices.reserve(region.size() * 3);
    its.indices.reserve(region.size());
    int base = 0;
    for (int f : region) {
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
    if (its.indices.empty())
        return;
    m_paint_area_model.init_from(its);
    m_paint_area_model.set_color(kPaintAreaCol);
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

    // El lienzo del pincel: se enseña mientras el pincel está elegido y el paso 1 armado, o
    // mientras se está pintando. Fuera de eso estorbaría.
    {
        const bool want = (m_foot_shape == FootShape::Brush) && (m_target_pick_mode || m_painting);
        const int  seed = (m_brush_seed_facet >= 0) ? m_brush_seed_facet : hover_face;
        if (! want || seed < 0) {
            if (m_paint_area_facet != -1) {
                m_paint_area_model.reset();
                m_paint_area_facet = -1;
            }
        } else if (seed != m_paint_area_facet) {
            build_paint_area_model(seed);
            m_paint_area_facet = seed;
        }
    }

    if (m_overhang_model_dirty)
        build_overhang_model();

    if (! m_hover_model.is_initialized() && ! m_target_model.is_initialized()
        && ! m_paint_area_model.is_initialized() && ! m_overhang_model.is_initialized())
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
        // El lienzo primero y siempre por debajo: es el fondo sobre el que se pinta.
        if (m_paint_area_model.is_initialized()) {
            ColorRGBA c = kPaintAreaCol; c[3] *= alpha_scale;
            m_paint_area_model.set_color(c);
            m_paint_area_model.render();
        }
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

bool GLGizmoSupportZones::on_mouse(const wxMouseEvent &mouse_event)
{
    const bool picking = m_target_pick_mode || (m_landing_pick_mode && m_has_target);
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
                ++ m_stroke_id;
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
                    m_lean_angle_deg    = float(std::min(45., max_lean_angle_deg()));
                    m_lean_angle_seeded = true;
                }
                // Hand the gesture straight to step 2 instead of making the user find a second
                // checkbox.
                m_target_pick_mode    = false;
                m_landing_pick_mode   = true;
                m_landing_locked      = false;
                m_has_landing         = false;
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

// NEOTKO_SUPPORTZONES_TAG s289 — EL PARCHE, UNA SOLA VEZ
// -----------------------------------------------------------------------------
// La auditoría de la geometría (§ "cómo se crea el objeto") encontró que la huella tenía DOS
// implementaciones que no podían coincidir, y que las dos dependían de la teselación de la malla.
// Todo lo que sigue es una sola: `build_patch()` produce el parche recortado, orientado, soldado y
// con su borde; el contorno 2D es la SILUETA de ese parche, y el sólido son sus anillos. El porqué
// de cada pieza vive con la declaración de `PatchGeom` en la cabecera.

// 🚨 Nada de `M_PI`: no es estándar y en MSVC hace falta `_USE_MATH_DEFINES` antes de <cmath>.
// Es una de las trampas que ya costó una sesión en este árbol.
// 🚨 Y va a nivel de fichero, NO dentro de la función: MSVC exige capturar una constante local
// dentro de una lambda sin captura por defecto (C3493), mientras que clang/gcc la dejan pasar.
static constexpr double PI_D = 3.14159265358979323846;

ExPolygons GLGizmoSupportZones::crop_shape(const Vec2d &centre_xy) const
{
    ExPolygons out;
    auto disc = [](const Vec2d &c, double r) {
        Points pts;
        const int seg = 48;
        pts.reserve(seg);
        for (int i = 0; i < seg; ++ i) {
            const double a = 2.0 * PI_D * double(i) / double(seg);
            pts.emplace_back(scaled<coord_t>(c.x() + r * std::cos(a)), scaled<coord_t>(c.y() + r * std::sin(a)));
        }
        return Polygon(std::move(pts));
    };

    switch (m_foot_shape) {
    case FootShape::Patch:
        return out;   // sin recorte
    case FootShape::Round: {
        if (m_foot_size_mm <= 0.f)
            return out;
        out.emplace_back(disc(centre_xy, 0.5 * double(m_foot_size_mm)));
        return out;
    }
    case FootShape::Square: {
        if (m_foot_size_mm <= 0.f)
            return out;
        const double h = 0.5 * double(m_foot_size_mm);
        Points pts { { scaled<coord_t>(centre_xy.x() - h), scaled<coord_t>(centre_xy.y() - h) },
                     { scaled<coord_t>(centre_xy.x() + h), scaled<coord_t>(centre_xy.y() - h) },
                     { scaled<coord_t>(centre_xy.x() + h), scaled<coord_t>(centre_xy.y() + h) },
                     { scaled<coord_t>(centre_xy.x() - h), scaled<coord_t>(centre_xy.y() + h) } };
        out.emplace_back(Polygon(std::move(pts)));
        return out;
    }
    case FootShape::Brush: {
        // 🔑 La unión de todas las marcas. Es donde el recorte deja de ser una forma y pasa a ser
        // lo que has pintado — y por eso el recorte tenía que dejar de ser convexo y pasar por
        // Clipper: una unión de discos tiene muescas, y un recorte por semiplanos no sabe hacerlas.
        if (m_stamps.empty())
            return out;
        Polygons all;
        all.reserve(m_stamps.size() * 2);
        for (size_t i = 0; i < m_stamps.size(); ++ i) {
            const Stamp &st = m_stamps[i];
            all.emplace_back(disc(st.c, st.r));
            // 🔑 La CÁPSULA entre esta marca y la anterior del mismo trazo. Sin ella, un ratón
            // rápido deja el rosario de puntos que él vio; con ella el trazo es una banda continua
            // pase lo que pase con la cadencia del sistema operativo.
            if (i == 0)
                continue;
            const Stamp &pv = m_stamps[i - 1];
            if (pv.stroke != st.stroke)
                continue;
            const Vec2d d = st.c - pv.c;
            const double l = d.norm();
            if (l < 1e-6)
                continue;
            const Vec2d u = d / l;
            const Vec2d n(- u.y(), u.x());
            const Vec2d a0 = pv.c + n * pv.r, a1 = pv.c - n * pv.r;
            const Vec2d b0 = st.c + n * st.r, b1 = st.c - n * st.r;
            Points q { { scaled<coord_t>(a0.x()), scaled<coord_t>(a0.y()) },
                       { scaled<coord_t>(b0.x()), scaled<coord_t>(b0.y()) },
                       { scaled<coord_t>(b1.x()), scaled<coord_t>(b1.y()) },
                       { scaled<coord_t>(a1.x()), scaled<coord_t>(a1.y()) } };
            Polygon band(std::move(q));
            if (! band.is_counter_clockwise())
                band.reverse();
            all.emplace_back(std::move(band));
        }
        return union_ex(all);
    }
    }
    return out;
}

bool GLGizmoSupportZones::build_patch(int facet_idx, const Vec2d &centre_xy, PatchGeom &out) const
{
    out = PatchGeom();
    const int n_facets = int(m_mesh.its.indices.size());
    if (facet_idx < 0 || facet_idx >= n_facets || int(m_face_normals.size()) != n_facets)
        return false;

    // El pincel pinta sobre TODA la superficie que mira hacia abajo; el resto de formas siguen
    // atadas a la cara que pinchaste, que es lo que uno espera de "pincha y te doy esa cara".
    const std::vector<int> region = (m_foot_shape == FootShape::Brush)
        ? collect_paint_region(facet_idx)
        : collect_region(facet_idx);
    if (region.empty())
        return false;

    // Soldadura POR POSICIÓN, no por índice. `raw_mesh()` es un `merge()` de volúmenes y la malla
    // de origen puede traer costuras sin soldar: con índices, una costura interior se cuenta como
    // borde y el pilar sale con paredes por dentro.
    std::map<std::tuple<long long, long long, long long>, int> weld;
    auto add_vertex = [&](const Vec3d &w) {
        const auto key = std::make_tuple(std::llround(w.x() * 10000.0),
                                         std::llround(w.y() * 10000.0),
                                         std::llround(w.z() * 10000.0));
        auto it = weld.find(key);
        if (it != weld.end())
            return it->second;
        const int idx = int(out.V.size());
        weld.emplace(key, idx);
        out.V.push_back(w);
        return idx;
    };

    const ExPolygons crop = crop_shape(centre_xy);
    const BoundingBox crop_bb = crop.empty() ? BoundingBox() : get_extents(crop);

    // ------------------------------------------------------------------------------------------
    // s289 — DE DÓNDE SALEN LOS TRIÁNGULOS DEL TECHO
    // ------------------------------------------------------------------------------------------
    // Dos orígenes y un solo camino después. En «recortar» son los triángulos de la MALLA (el
    // parche de siempre); en «cubrir» son una rejilla sobre la forma pedida, con la z de cada nodo
    // sacada de un rayo vertical contra la pieza.
    //
    // 🔑 Idea suya, y es la buena: "en vez de crecer podamos transformar la zona de soporte o base
    // que toca por un círculo grande o un cuadrado pequeño, así crece según queremos y no según la
    // forma original". Eso es esto. Crecer el contorno del parche siempre acaba cruzándose porque
    // la forma manda; sustituir el contorno por una primitiva no tiene ese techo, y a cambio el
    // bloque ocupa más área — que él ya ha dicho que es el precio que quiere pagar.
    //
    // El techo sigue siendo la superficie DE VERDAD donde la hay (por eso el rayo, y no un plano),
    // así que un círculo grande sobre una zona curva sigue quedando a ras. Donde no hay pieza
    // debajo, la z cae al plano de la cara semilla: es plano, está en el aire, y es exactamente lo
    // que uno espera de un bloque que sobresale del voladizo.
    std::vector<std::array<Vec3d, 3>> src;

    // El plano de la cara pinchada, que es el respaldo cuando el rayo no encuentra nada.
    Vec3d seed_p { Vec3d::Zero() }, seed_n { 0., 0., -1. };
    {
        const Vec3i32 t0 = m_mesh.its.indices[facet_idx];
        seed_p = (m_world_trafo * m_mesh.its.vertices[t0[0]].cast<double>()
                + m_world_trafo * m_mesh.its.vertices[t0[1]].cast<double>()
                + m_world_trafo * m_mesh.its.vertices[t0[2]].cast<double>()) / 3.;
        Vec3d n = m_world_trafo.linear().inverse().transpose() * m_face_normals[facet_idx].cast<double>();
        if (n.norm() > 1e-12) {
            n.normalize();
            if (n.z() > 0.)
                n = - n;
            seed_n = n;
        }
    }

    if (crop.empty() || ! m_shape_covers) {
        src.reserve(region.size());
        for (int f : region) {
            const Vec3i32 t = m_mesh.its.indices[f];
            src.push_back({ m_world_trafo * m_mesh.its.vertices[t[0]].cast<double>(),
                            m_world_trafo * m_mesh.its.vertices[t[1]].cast<double>(),
                            m_world_trafo * m_mesh.its.vertices[t[2]].cast<double>() });
        }
    } else {
        // Un rayo vertical por nodo de la rejilla: la superficie que MIRA HACIA ABAJO más cercana
        // en z a la cara pinchada. "La más cercana" y no "la primera" porque una pieza con varios
        // pisos tiene varias, y la que interesa es la que estás sujetando.
        const Transform3d inv  = m_world_trafo.inverse();
        const AABBMesh   *tree = m_raycaster ? &m_raycaster->get_aabb_mesh() : nullptr;
        const double      z_ref = seed_p.z();
        auto sample_z = [&](double x, double y) {
            auto on_plane = [&]() {
                if (std::abs(seed_n.z()) < 1e-6)
                    return z_ref;
                return (seed_n.dot(seed_p) - seed_n.x() * x - seed_n.y() * y) / seed_n.z();
            };
            if (tree == nullptr)
                return on_plane();
            const Vec3d src_w { x, y, z_ref + 1000. };
            const Vec3d dir_w { 0., 0., -1. };
            std::vector<AABBMesh::hit_result> hits = tree->query_ray_hits(inv * src_w, inv.linear() * dir_w);
            double best = std::numeric_limits<double>::max();
            bool   found = false;
            for (const AABBMesh::hit_result &h : hits) {
                if (! h.is_hit())
                    continue;
                const int f = h.face();
                if (f < 0 || f >= int(m_face_normals.size()))
                    continue;
                Vec3d wn = m_world_trafo.linear().inverse().transpose() * m_face_normals[f].cast<double>();
                if (wn.norm() < 1e-12)
                    continue;
                wn.normalize();
                if (wn.z() > double(DOWN_FACING_MAX_NORMAL_Z))
                    continue;   // no mira hacia abajo
                const double zw = (m_world_trafo * h.position()).z();
                if (! found || std::abs(zw - z_ref) < std::abs(best - z_ref)) {
                    best  = zw;
                    found = true;
                }
            }
            return found ? best : on_plane();
        };

        const double x0 = unscaled<double>(double(crop_bb.min.x())) - 0.001;
        const double y0 = unscaled<double>(double(crop_bb.min.y())) - 0.001;
        const double x1 = unscaled<double>(double(crop_bb.max.x())) + 0.001;
        const double y1 = unscaled<double>(double(crop_bb.max.y())) + 0.001;
        double step = std::clamp(double(m_foot_size_mm) / 24.0, 0.4, 2.0);
        int nx = std::max(1, int(std::ceil((x1 - x0) / step)));
        int ny = std::max(1, int(std::ceil((y1 - y0) / step)));
        // Tope de celdas: una forma de 200 mm con paso fino son cientos de miles de rayos, y esto
        // se recalcula al mover el deslizador. El paso cede antes que la interactividad.
        constexpr int MAX_CELLS = 40000;
        while (nx * ny > MAX_CELLS) {
            step *= 1.5;
            nx = std::max(1, int(std::ceil((x1 - x0) / step)));
            ny = std::max(1, int(std::ceil((y1 - y0) / step)));
        }
        std::vector<double> zg(size_t(nx + 1) * size_t(ny + 1));
        for (int j = 0; j <= ny; ++ j)
            for (int i = 0; i <= nx; ++ i)
                zg[size_t(j) * size_t(nx + 1) + size_t(i)] = sample_z(x0 + step * i, y0 + step * j);
        auto node = [&](int i, int j) {
            return Vec3d(x0 + step * i, y0 + step * j, zg[size_t(j) * size_t(nx + 1) + size_t(i)]);
        };
        src.reserve(size_t(nx) * size_t(ny) * 2);
        for (int j = 0; j < ny; ++ j)
            for (int i = 0; i < nx; ++ i) {
                // Winding para que la normal salga hacia abajo, que es lo que espera todo lo de
                // debajo. El bucle de abajo lo comprueba igualmente, pero mejor no darle trabajo.
                src.push_back({ node(i, j), node(i + 1, j + 1), node(i + 1, j) });
                src.push_back({ node(i, j), node(i, j + 1), node(i + 1, j + 1) });
            }
    }

    // Área proyectada de los triángulos ANTES de unirlos: contra el área de la unión dice si el
    // parche se pliega sobre sí mismo visto desde arriba.
    double area_sum = 0.;
    Polygons projected;

    for (const std::array<Vec3d, 3> &tri3 : src) {
        Vec3d A = tri3[0];
        Vec3d B = tri3[1];
        Vec3d C = tri3[2];
        Vec3d n = (B - A).cross(C - A);
        const double nl = n.norm();
        if (nl < 1e-12)
            continue;                       // triángulo degenerado en la propia malla
        n /= nl;
        // 🔑 Winding normalizado de una vez: todo el parche mira hacia ABAJO. A partir de aquí, el
        // sentido del borde, el "hacia dentro" y las tapas son ciertos sin depender de la malla.
        if (n.z() > 0.) {
            std::swap(B, C);
            n = - n;
        }
        if (std::abs(n.z()) < 1e-9)
            continue;                       // de canto: en XY es una línea, no aporta nada al prisma

        if (crop.empty()) {
            const int a = add_vertex(A), b = add_vertex(B), c = add_vertex(C);
            if (a != b && b != c && a != c)
                out.F.emplace_back(a, b, c);
            Points pp { { scaled<coord_t>(A.x()), scaled<coord_t>(A.y()) },
                        { scaled<coord_t>(B.x()), scaled<coord_t>(B.y()) },
                        { scaled<coord_t>(C.x()), scaled<coord_t>(C.y()) } };
            Polygon pg(std::move(pp));
            area_sum += std::abs(pg.area());
            if (! pg.is_counter_clockwise())
                pg.reverse();
            projected.emplace_back(std::move(pg));
            continue;
        }

        // ---- recorte EXACTO en XY -------------------------------------------------------------
        // 🚨 Aquí es donde se arregla el fallo que se veía en las superficies sencillas. El filtro
        // de antes era "¿cae el centroide del triángulo dentro de la forma?", y en una cara de dos
        // triángulos de 20 mm eso sólo sabe contestar "todo" o "nada": pedías un pie de 8 mm y te
        // salía media pieza. Ahora el triángulo se corta de verdad contra la forma y se vuelve a
        // triangular, así que la resolución de la huella es la del mando y no la de la malla.
        Points pp { { scaled<coord_t>(A.x()), scaled<coord_t>(A.y()) },
                    { scaled<coord_t>(B.x()), scaled<coord_t>(B.y()) },
                    { scaled<coord_t>(C.x()), scaled<coord_t>(C.y()) } };
        Polygon tri_xy(std::move(pp));
        if (tri_xy.area() == 0.)
            continue;
        if (! tri_xy.is_counter_clockwise())
            tri_xy.reverse();
        BoundingBox tbb = get_extents(Polygons{ tri_xy });
        if (! tbb.overlap(crop_bb))
            continue;                       // ni lo toca

        // El plano del triángulo, para devolverle la z a cada punto recortado.
        const double d_plane = n.dot(A);
        auto z_at = [&](const Point &p) {
            const double x = unscaled<double>(p.x()), y = unscaled<double>(p.y());
            return (d_plane - n.x() * x - n.y() * y) / n.z();
        };

        // Atajo: si los tres vértices están dentro, el triángulo entra tal cual. Ahorra una llamada
        // a Clipper por cada triángulo del interior, que en una malla densa es casi todo.
        // ⚠️ Un triángulo enorme podría puentear una muesca del recorte con sus tres vértices
        // dentro. Pide una concavidad más estrecha que un triángulo, y el resultado es un puente
        // convexo de fracción de milímetro en un bloque de soporte: se acepta a sabiendas.
        bool all_in = true;
        for (const Point &q : tri_xy.points) {
            bool in = false;
            for (const ExPolygon &ep : crop)
                if (ep.contains(q)) { in = true; break; }
            if (! in) { all_in = false; break; }
        }
        if (all_in) {
            const int a = add_vertex(A), b = add_vertex(B), c = add_vertex(C);
            if (a != b && b != c && a != c)
                out.F.emplace_back(a, b, c);
            area_sum += std::abs(tri_xy.area());
            projected.emplace_back(tri_xy);
            continue;
        }

        const ExPolygons piece = intersection_ex(ExPolygons{ ExPolygon(tri_xy) }, crop);
        for (const ExPolygon &ep : piece) {
            if (ep.contour.points.size() < 3)
                continue;
            const Points pts = to_points(ep);
            const std::vector<Vec3i32> idx = Triangulation::triangulate(ep);
            if (idx.empty())
                continue;
            std::vector<int> local;
            local.reserve(pts.size());
            for (const Point &q : pts)
                local.push_back(add_vertex(Vec3d(unscaled<double>(q.x()), unscaled<double>(q.y()), z_at(q))));
            for (const Vec3i32 &ti : idx) {
                if (ti[0] < 0 || ti[1] < 0 || ti[2] < 0 ||
                    ti[0] >= int(local.size()) || ti[1] >= int(local.size()) || ti[2] >= int(local.size()))
                    continue;
                const int a = local[ti[0]], b = local[ti[1]], c = local[ti[2]];
                if (a == b || b == c || a == c)
                    continue;
                // `Triangulation` devuelve el sentido de la ExPolygon (CCW en XY, normal +Z); el
                // parche mira hacia abajo, así que van del revés.
                out.F.emplace_back(a, c, b);
            }
            area_sum += std::abs(ep.contour.area());
            Polygon c2 = ep.contour;
            if (! c2.is_counter_clockwise())
                c2.reverse();
            projected.emplace_back(std::move(c2));
        }
    }

    if (out.F.empty() || out.V.size() < 3)
        return false;

    // ---- normales por vértice ------------------------------------------------------------------
    out.vnormal.assign(out.V.size(), Vec3d::Zero());
    for (const Vec3i32 &t : out.F) {
        const Vec3d n = (out.V[t[1]] - out.V[t[0]]).cross(out.V[t[2]] - out.V[t[0]]);   // sin normalizar: pondera por área
        for (int k = 0; k < 3; ++ k)
            out.vnormal[t[k]] += n;
    }
    for (Vec3d &n : out.vnormal) {
        // 🚨 Sin ternario: `normalized()` devuelve una expresión de Eigen con otra alineación que
        // `Vec3d` y el compilador no sabe a cuál convertir (lessons_code_traps).
        if (n.norm() > 1e-12)
            n.normalize();
        else
            n = Vec3d(0., 0., -1.);
    }

    // ---- el borde ------------------------------------------------------------------------------
    std::map<std::pair<int, int>, int> edge_count;
    for (const Vec3i32 &t : out.F)
        for (int k = 0; k < 3; ++ k)
            ++ edge_count[std::minmax(t[k], t[(k + 1) % 3])];
    for (const Vec3i32 &t : out.F)
        for (int k = 0; k < 3; ++ k) {
            const int a = t[k], b = t[(k + 1) % 3];
            if (edge_count[std::minmax(a, b)] == 1)
                out.border.emplace_back(a, b);
        }
    if (out.border.empty())
        return false;   // una región sin borde es una cáscara cerrada, no un parche

    // ---- "hacia dentro" CON INGLETE ------------------------------------------------------------
    // 🔑 Lo de antes era la media de las normales de las aristas, normalizada. En una esquina de
    // 90° eso mide 1 cuando el inglete pide √2, así que los lados se metían lo pedido y la esquina
    // se quedaba corta: la huella se redondeaba y no coincidía con el contorno que dibuja el panel.
    // Con 400 vértices (el torus) no se ve; con 4 (un rectángulo) es todo el efecto.
    //
    // El parche mira hacia abajo, así que visto desde ARRIBA su recorrido es horario y el interior
    // queda a la DERECHA de cada arista: la normal interior de una arista `d` es `(d.y, -d.x)`.
    std::vector<Vec2d> n_in(out.V.size(), Vec2d::Zero()), n_out(out.V.size(), Vec2d::Zero());
    std::vector<int>   cnt_in(out.V.size(), 0), cnt_out(out.V.size(), 0);
    for (const auto &e : out.border) {
        const Vec2d d = (out.V[e.second] - out.V[e.first]).head<2>();
        if (d.norm() < 1e-9)
            continue;
        Vec2d nn(d.y(), - d.x());
        nn.normalize();
        n_out[e.first]  += nn; ++ cnt_out[e.first];
        n_in [e.second] += nn; ++ cnt_in [e.second];
    }
    out.inward.assign(out.V.size(), Vec2d::Zero());
    for (size_t i = 0; i < out.V.size(); ++ i) {
        if (cnt_in[i] == 0 && cnt_out[i] == 0)
            continue;
        if (cnt_in[i] == 1 && cnt_out[i] == 1) {
            Vec2d a = n_in[i], b = n_out[i];
            Vec2d m = a + b;
            const double ml = m.norm();
            if (ml < 1e-6) {
                out.inward[i] = Vec2d::Zero();   // pincho de 180°: no hay dirección que valga
                continue;
            }
            m /= ml;
            const double c = m.dot(a);           // cos(θ/2)
            // El tope del inglete: en un pico muy agudo el factor se va al infinito y el vértice
            // saldría disparado. 4 es el mismo orden que usa cualquier offsetter antes de cortar.
            out.inward[i] = m * std::min(4.0, (c > 1e-3) ? (1.0 / c) : 4.0);
        } else {
            Vec2d m = n_in[i] + n_out[i];
            if (m.norm() > 1e-9)
                m.normalize();
            out.inward[i] = m;                   // vértice pinzado: sin inglete, media y ya está
        }
    }

    // ---- el tope real del borde ----------------------------------------------------------------
    // Cuánto se puede meter antes de que la forma se cruce consigo misma. Sale en forma CERRADA:
    // una arista se da la vuelta cuando su dirección invertida, y eso es un producto escalar.
    //   d(t) = d0 + t·(inward_b − inward_a)  ⇒  d0·d(t) = |d0|² + t·(d0·Δ) = 0
    double t_in  = std::numeric_limits<double>::max();
    double t_out = std::numeric_limits<double>::max();
    for (const auto &e : out.border) {
        const Vec2d d0 = (out.V[e.second] - out.V[e.first]).head<2>();
        const double l2 = d0.squaredNorm();
        if (l2 < 1e-18)
            continue;
        const double den = d0.dot(out.inward[e.second] - out.inward[e.first]);
        if (den < -1e-12)
            t_in  = std::min(t_in,  l2 / (- den));   // se cruza METIENDO
        else if (den > 1e-12)
            t_out = std::min(t_out, l2 / den);       // se cruza SACANDO
    }
    // Sin arista que limite por ese lado, el tope es de cordura y no de geometría: la mitad del
    // lado mayor de la huella para el metido (más allá no queda nada), y un palmo generoso para el
    // crecido, que es donde él pedía sitio.
    const BoundingBox bb = get_extents(projected);
    const double half_span = 0.5 * unscaled<double>(double(std::max(bb.size().x(), bb.size().y())));
    if (t_in  == std::numeric_limits<double>::max())
        t_in  = half_span;
    if (t_out == std::numeric_limits<double>::max())
        t_out = std::max(50.0, half_span);
    // El margen es para no dejar al usuario justo en el borde del colapso, donde la forma es una
    // astilla y el pilar no imprimiría nada.
    out.max_inset  = std::max(0., t_in  * 0.9);
    out.max_outset = std::max(0., t_out * 0.9);

    // ---- se pliega sobre sí mismo? -------------------------------------------------------------
    if (! projected.empty()) {
        double union_area = 0.;
        for (const ExPolygon &ep : union_ex(projected))
            union_area += ep.area();
        out.folded = (union_area > 0.) && (area_sum > union_area * 1.05);
    }

    out.z_low  = std::numeric_limits<double>::max();
    out.z_high = std::numeric_limits<double>::lowest();
    for (const Vec3d &w : out.V) {
        out.z_low  = std::min(out.z_low,  w.z());
        out.z_high = std::max(out.z_high, w.z());
    }
    out.ok = true;
    return true;
}

void GLGizmoSupportZones::invalidate_patch()
{
    m_patch_cache_facet = -1;
    m_foot_cache_facet  = -1;
}

const GLGizmoSupportZones::PatchGeom *GLGizmoSupportZones::patch(int facet_idx, Vec2d centre_xy) const
{
    // 🚨 El centro y la matriz entran en la clave por lo mismo que ya entraban en la de la huella:
    // el recorte se mueve con el cursor dentro de la MISMA cara, y el parche está en MUNDO, así que
    // mover el objeto lo cambia entero sin que el índice de la cara se mueva un pelo.
    // 🔑 s289 — con el pincel la semilla NO es la cara bajo el cursor: es la primera que se pintó.
    // Así el parche sigue siendo el flood-fill desde ella y pintar nunca se salta una esquina hacia
    // otra cara; y de paso todos los sitios que ya llamaban con la cara del cursor siguen valiendo
    // sin tocarlos, que es lo que mantiene el resto del panel ajeno al modo.
    if (m_foot_shape == FootShape::Brush && m_brush_seed_facet >= 0 && ! m_stamps.empty()) {
        facet_idx = m_brush_seed_facet;
        centre_xy = m_stamps.front().c;
    }
    // El modo cubrir va DENTRO de la clave de forma (+100): cambia la geometría entera y no tener
    // que añadir un campo a las dos cachés es exactamente lo que evita olvidarse de una.
    const int shape_key = int(m_foot_shape) + (m_shape_covers ? 100 : 0);
    if (m_patch_cache_facet == facet_idx && m_patch_cache_shape == shape_key
        && m_patch_cache_size == m_foot_size_mm && m_patch_cache_centre == centre_xy
        && m_patch_cache_stamps == m_stamps.size()
        && m_patch_cache_trafo.isApprox(m_world_trafo))
        return m_patch_cache.ok ? &m_patch_cache : nullptr;

    build_patch(facet_idx, centre_xy, m_patch_cache);
    m_patch_cache_facet  = facet_idx;
    m_patch_cache_shape  = shape_key;
    m_patch_cache_size   = m_foot_size_mm;
    m_patch_cache_centre = centre_xy;
    m_patch_cache_stamps = m_stamps.size();
    m_patch_cache_trafo  = m_world_trafo;
    return m_patch_cache.ok ? &m_patch_cache : nullptr;
}

std::vector<Vec2d> GLGizmoSupportZones::ring_outline_world(double edge_mm) const
{
    // 🔑 La SILUETA DEL SÓLIDO, no un contorno paralelo calculado aparte. Se proyectan los mismos
    // vértices desplazados que van a acabar en la malla, así que el contorno que dibuja el panel y
    // la forma que se imprime son la misma cosa por construcción — que era justo la tercera mentira
    // que encontró la auditoría.
    std::vector<Vec2d> out;
    if (! m_has_target)
        return out;
    const PatchGeom *pg = patch(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    if (pg == nullptr)
        return out;
    const double e = std::min(edge_mm, pg->max_inset);

    Polygons tris;
    tris.reserve(pg->F.size());
    for (const Vec3i32 &t : pg->F) {
        Points p;
        p.reserve(3);
        for (int k = 0; k < 3; ++ k) {
            const Vec2d w = pg->V[t[k]].head<2>() + pg->inward[t[k]] * e;
            p.emplace_back(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
        }
        Polygon q(std::move(p));
        if (q.area() == 0.)
            continue;
        if (! q.is_counter_clockwise())
            q.reverse();
        tris.emplace_back(std::move(q));
    }
    if (tris.empty())
        return out;

    // El micro-offset cierra las costuras de un píxel que el redondeo a entero deja entre
    // triángulos vecinos y que sobrevivirían a la unión como astillas.
    ExPolygons islands = union_ex(offset(tris, scaled<float>(0.002f)));
    if (islands.empty())
        return out;
    size_t best = 0;
    for (size_t i = 1; i < islands.size(); ++ i)
        if (std::abs(islands[i].contour.area()) > std::abs(islands[best].contour.area()))
            best = i;
    Polygon outline = islands[best].contour;
    // ⚠️ Los agujeros se tiran a propósito: el pilar es un prisma macizo, y un bloque de soporte
    // con un agujero sólo complica el corredor para nada — el techo que hay sobre el agujero
    // también quiere sujeción.
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
        && m_foot_cache_centre == centre && m_foot_cache_stamps == m_stamps.size()
        && m_foot_cache_trafo.isApprox(m_world_trafo))
        return m_foot_cache;

    const PatchGeom *pg = patch(m_target_facet_idx, centre);
    if (pg == nullptr)
        return out;
    // El z más bajo del parche (donde empieza la inclinación) sale del mismo sitio que todo lo
    // demás, así que no puede discrepar de la malla.
    m_foot_cache_z_low     = pg->z_low;
    m_foot_cache_max_inset = pg->max_inset;
    out = ring_outline_world(double(m_footprint_shrink_mm));

    m_foot_cache        = out;
    m_foot_cache_facet  = m_target_facet_idx;
    m_foot_cache_shrink = m_footprint_shrink_mm;
    m_foot_cache_shape  = int(m_foot_shape) + (m_shape_covers ? 100 : 0);
    m_foot_cache_size   = m_foot_size_mm;
    m_foot_cache_centre = centre;
    m_foot_cache_stamps = m_stamps.size();
    m_foot_cache_trafo  = m_world_trafo;
    return out;
}

double GLGizmoSupportZones::max_inset_mm() const
{
    if (! m_has_target)
        return 0.;
    const PatchGeom *pg = patch(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    return (pg == nullptr) ? 0. : pg->max_inset;
}

double GLGizmoSupportZones::max_outset_mm() const
{
    if (! m_has_target)
        return 0.;
    const PatchGeom *pg = patch(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
    return (pg == nullptr) ? 0. : pg->max_outset;
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
    const Vec2d p { live->world_pos.x(), live->world_pos.y() };
    const double r = std::max(0.2, 0.5 * double(m_foot_size_mm));

    if (erase) {
        const size_t before = m_stamps.size();
        m_stamps.erase(std::remove_if(m_stamps.begin(), m_stamps.end(),
                                      [&](const Stamp &st) { return (st.c - p).norm() <= r; }),
                       m_stamps.end());
        if (m_stamps.size() == before)
            return false;
        if (m_stamps.empty()) {
            // Borrado del todo: se suelta también la semilla, para que la siguiente pincelada pueda
            // empezar en otra cara sin obligar a salir y volver a entrar en el paso 1.
            m_brush_seed_facet = -1;
            m_has_target       = false;
        }
    } else {
        // Una marca por cada medio radio recorrido: menos es sopa de polígonos y más deja el trazo
        // con festones. Es la misma cadencia que usa cualquier pincel de mapa de bits.
        // Con la cápsula uniendo marcas consecutivas la cadencia ya no tiene que garantizar solape,
        // así que puede ser MÁS espaciada: menos polígonos que unir, menos CPU, y el trazo sale
        // igual de continuo. Era una de las cosas que él notaba que costaban.
        if (! m_stamps.empty() && (m_stamps.back().c - p).norm() < 0.8 * r
            && std::abs(m_stamps.back().r - r) < 1e-6)
            return false;
        if (m_stamps.empty()) {
            m_brush_seed_facet    = live->facet_idx;
            m_target_facet_idx    = live->facet_idx;
            m_target_world_pos    = live->world_pos;
            m_target_world_normal = live->world_normal;
            // El parche queda tomado con la primera pincelada, así que el paso 2 se enciende solo.
            // ⛔ Y NO se salta al paso 2 como hace el clic: con el pincel se sigue pintando hasta
            // que uno dice que ha terminado. Avanzar solo aquí obligaría a volver al paso 1 para
            // cada trazo, que es justo el UX que él pidió no romper.
            m_has_target = true;
            if (! m_lean_angle_seeded) {
                m_lean_angle_deg    = float(std::min(45., max_lean_angle_deg()));
                m_lean_angle_seeded = true;
            }
        }
        m_stamps.push_back({ p, r, m_stroke_id });
    }

    invalidate_patch();
    m_preview_dirty         = true;
    m_footprint_model_dirty = true;
    m_hover_model_facet     = -1;   // que el resaltado se rehaga con lo recién pintado
    m_target_model_facet    = -1;
    return true;
}

bool GLGizmoSupportZones::build_pillar_mesh(TriangleMesh &out_world) const
{
    if (! m_has_target || ! m_has_landing)
        return false;
    const Vec2d centre  = Vec2d(m_target_world_pos.x(), m_target_world_pos.y());
    const Vec3d landing = m_landing_world_pos;

    // 🔑 s286b, dicho por él y es la forma correcta de pensarlo: esto es un EXTRUDE DE SUPERFICIE,
    // no una silueta extruida. Antes se aplastaba el parche a XY, se unían los triángulos, y se
    // levantaba ese contorno plano. Eso convierte una banda curva en un prisma gordo con techo
    // plano que se traga media pieza. Lo que se quiere es lo de Rhino: coger los polígonos DE LA
    // SUPERFICIE y barrerlos. El techo del sólido es entonces el parche de verdad, con su curva,
    // y las paredes salen de su borde.
    //
    // ✅ s289 — el parche, su borde y su "hacia dentro" ya no se calculan aquí: vienen de
    // `PatchGeom`, que es también de donde sale el contorno que dibuja el panel. Una sola verdad.
    const PatchGeom *pg = patch(m_target_facet_idx, centre);
    if (pg == nullptr)
        return false;
    const std::vector<Vec3d>              &top    = pg->V;
    const std::vector<Vec3i32>            &tris   = pg->F;
    const std::vector<std::pair<int,int>> &border = pg->border;
    const int nv = int(top.size());
    if (nv < 3 || tris.empty() || border.empty())
        return false;

    const double z_low  = pg->z_low;
    const double z_high = pg->z_high;

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

    const double z_bottom  = landing.z();
    const double z_bot_ext = m_landing_on_bed ? -0.5 : z_bottom;
    if (z_high - z_bot_ext < 0.2)
        return false;   // nothing worth building
    const Vec2d shift = Vec2d(landing.x(), landing.y()) - c_top;

    // 🔑 s286b — EL PILAR CON RODILLA, y es un cambio de qué manda. Antes la inclinación se repartía
    // entre todas las capas que hubiera, así que el `v` por capa salía de dividir el desplazamiento
    // total entre ellas y sólo se sabía si cabía DESPUÉS. Ahora el ángulo es el dato de entrada:
    // cada capa del tramo inclinado pide exactamente `dz · tan(ángulo)`, constante, comparable con
    // el tope del §3 de un vistazo, y el caso imposible deja de poder dibujarse en vez de avisarse.
    //
    // Cuatro anillos, y cada uno responde a algo distinto:
    //   T  el parche mismo, subido medio milímetro para que la superficie quede DENTRO del bloque
    //      y nunca justo en una frontera de capa (con ALH esa frontera se mueve, §4-bis.1);
    //   M  la cota más baja del parche, en vertical bajo T: aquí acaba lo que ENVUELVE el parche.
    //      Inclinar ahí dentro dejaría la parte media de una banda fuera del bloque — el donut;
    //   K  LA RODILLA: aquí se ha consumido TODO el desplazamiento, a `v` constante desde M;
    //   B  el pie, tras el tramo de bajada, que es vertical y por tanto no gasta corredor.
    constexpr double ROOF_LIFT_MM = 0.5;
    const bool   wraps    = (z_low > z_bot_ext + 0.2);
    const double offset   = shift.norm();
    constexpr double deg2rad = 0.01745329251994329576923690768489;
    const double tan_a    = std::tan(std::max(0.05, double(m_lean_angle_deg)) * deg2rad);
    const double lean_top = wraps ? z_low : (z_high + ROOF_LIFT_MM);
    // Lo que le cuesta al ángulo recorrer esa distancia. A plomo no cuesta nada y no hay rodilla.
    const double z_knee   = (offset < 1e-6) ? z_bot_ext : (lean_top - offset / tan_a);
    // 🚨 La rodilla por debajo del pie es el nuevo "fuera de alcance": ese ángulo no cubre esa
    // distancia en la altura que hay. Se rechaza aquí y no se dibuja nada, que es la promesa del §1.
    if (z_knee < z_bot_ext - 1e-6)
        return false;
    // Una rodilla pegada al pie no es una rodilla: sería una banda de altura cero.
    const bool has_knee = (offset > 1e-6) && (z_knee > z_bot_ext + 0.2);

    struct Ring { double z; Vec2d off; };
    std::vector<Ring> rings;
    rings.push_back({ z_high + ROOF_LIFT_MM, Vec2d(0., 0.) });           // T  (z real por vértice)
    if (wraps)
        rings.push_back({ z_low, Vec2d(0., 0.) });                       // M
    if (has_knee)
        rings.push_back({ z_knee, shift });                              // K
    rings.push_back({ z_bot_ext, shift });                               // B

    // 🔑 s286b, idea suya: el borde no tiene por qué valer lo mismo arriba que abajo. Al engordar
    // para agarrar un alero, engordaba también el pie, y ahí no hace ninguna falta — sólo material.
    // Con un valor en el parche y otro en el pie, interpolados POR ALTURA, el pilar sale cónico: la
    // rama gorda donde sujeta y fina donde apoya. Es el mando de "cuánto material y cuánta
    // resistencia" que él pedía, y de paso el codo crece solo, porque la rodilla es un anillo más y
    // le toca su fracción.
    //
    // 🚨 ¿No rompe esto la sección constante del §4-bis.1? No, y el motivo importa: el corredor no
    // mide ÁREA, mide desplazamiento del CENTROIDE. Un crecimiento simétrico deja el centroide
    // donde estaba, así que `v` sigue constante. Lo que sí movería el centroide es un crecimiento
    // desigual — y por eso esto se aplica por la normal del borde y no por un lado.
    const double z_span = std::max(1e-6, (z_high + ROOF_LIFT_MM) - z_bot_ext);
    // El tope del colapso, aplicado también aquí: el deslizador ya no deja pasarse, pero un valor
    // guardado de antes o una huella que ha cambiado de forma sí pueden, y una malla del revés no
    // se avisa sola.
    // 🚨 s289 — y ahora también por abajo. El tope de antes era `std::min(v, cap)`, que sólo frena
    // el METIDO; la EXPANSIÓN (valores negativos) no tenía freno ninguno, y es justo la dirección
    // que cruza los lados de una muesca en un parche cóncavo. El inglete la empeora, además, porque
    // multiplica por 1/cos(θ/2). Se acota igual de simétrica.
    const double inset_cap  = pg->max_inset;
    const double outset_cap = pg->max_outset;
    auto edge_at = [&](double z) {
        const double t = std::clamp(((z - z_bot_ext) / z_span), 0., 1.);
        const double v = double(m_footprint_base_mm) + (double(m_footprint_shrink_mm) - double(m_footprint_base_mm)) * t;
        return std::clamp(v, - outset_cap, inset_cap);
    };

    // La cota del anillo que va justo debajo del techo. El techo sigue la superficie y encima se
    // corrige en Z por el plano tangente, así que puede bajar; si baja por debajo del anillo de
    // abajo, esa pareja de triángulos de pared sale del revés. Aquí es donde se le pone el suelo.
    const double z_under_roof = (rings.size() > 1) ? rings[1].z : z_bot_ext;

    indexed_triangle_set its;
    const int nblocks = int(rings.size());
    its.vertices.reserve(size_t(nblocks) * size_t(nv));
    for (int r = 0; r < nblocks; ++ r) {
        const Ring &ring = rings[r];
        for (int i = 0; i < nv; ++ i) {
            const Vec3d &w = top[i];
            // Sólo el anillo de arriba lleva la z de cada vértice: es la superficie de verdad, con
            // su curva. Los demás son planos.
            double      z = (r == 0) ? w.z() + ROOF_LIFT_MM : ring.z;
            const Vec2d e = pg->inward[i] * edge_at(z);
            // Sólo el techo sigue la superficie, así que sólo él se corrige en Z. Los demás anillos
            // son planos por construcción y ahí la corrección no significaría nada.
            //
            // 🔑 s286b, visto por él: "expande de manera muy lineal x/y y falla en las curvas a
            // tocarlos; tendría que crecer en Z en menor medida, pero sí, para compensar". Exacto,
            // y hay respuesta cerrada: el techo del sólido ES la superficie, así que al mover un
            // vértice del borde en XY hay que mantenerlo en el PLANO TANGENTE de ese vértice:
            //     dz = -(n.x·Δx + n.y·Δy) / n.z
            // que en una cara horizontal (n = (0,0,-1)) da cero y crece con la pendiente. "En menor
            // medida" es literalmente el cociente por n.z.
            if (r == 0 && e.squaredNorm() > 0.) {
                const Vec3d &n = pg->vnormal[i];
                if (std::abs(n.z()) > 1e-3)
                    z += - (n.x() * e.x() + n.y() * e.y()) / n.z();
                z = std::max(z, z_under_roof + 0.05);
            }
            its.vertices.emplace_back(float(w.x() + ring.off.x() + e.x()),
                                      float(w.y() + ring.off.y() + e.y()), float(z));
        }
    }
    const int bot_base = (nblocks - 1) * nv;

    // Tapas. Los triángulos del parche miran hacia ABAJO (para eso se eligen), así que como techo
    // del sólido van invertidos, y aplastados en el plano del aterrizaje sirven de suelo tal cual.
    for (const Vec3i32 &t : tris) {
        its.indices.emplace_back(t[0], t[2], t[1]);                                     // techo
        its.indices.emplace_back(bot_base + t[0], bot_base + t[1], bot_base + t[2]);     // suelo
    }
    // Paredes, una banda por cada par de bloques consecutivos.
    for (int blk = 0; blk + 1 < nblocks; ++ blk) {
        const int lo = blk * nv, hi = (blk + 1) * nv;
        for (const auto &e : border) {
            const int a = e.first, b = e.second;
            // Recorrido horario visto desde arriba ⇒ el exterior queda a la izquierda de la
            // arista, que es justo lo que da esta pareja de triángulos.
            its.indices.emplace_back(lo + a, lo + b, hi + b);
            its.indices.emplace_back(lo + a, hi + b, hi + a);
        }
    }

    // s289 — fuera los triángulos de área cero antes de entregar nada. Una arista de borde que en
    // XY es un punto (una pared vertical del parche) genera un par degenerado por cada banda, y a
    // un visor le da igual pero a un booleano o a un `its_volume` no.
    {
        std::vector<Vec3i32> keep;
        keep.reserve(its.indices.size());
        for (const Vec3i32 &t : its.indices) {
            const Vec3f &A = its.vertices[t[0]], &B = its.vertices[t[1]], &C = its.vertices[t[2]];
            if ((B - A).cross(C - A).norm() > 1e-10f)
                keep.push_back(t);
        }
        its.indices = std::move(keep);
    }
    if (its.indices.empty())
        return false;

    // Red de seguridad barata: si el sólido sale del revés (una huella degenerada puede darle la
    // vuelta a todo), se voltea entero en vez de entregar una malla con las normales hacia dentro.
    if (its_volume(its) < 0.f)
        its_flip_triangles(its);

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
    // ✅ s289 — la silueta del anillo DE ABAJO, sacada del mismo parche que el sólido. Antes esto
    // era el contorno de arriba pasado otra vez por `offset()` de Clipper, y eso ya son dos
    // operaciones distintas sobre dos formas distintas: el pie que se comprobaba (¿cabe en la cama?
    // ¿es demasiado estrecho?) no era el pie que se imprimía.
    return ring_outline_world(double(m_footprint_base_mm));
}

bool GLGizmoSupportZones::footprint_too_narrow() const
{
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
    if (! m_has_target || ! m_has_landing || ! m_landing_on_bed)
        return false;

    const BuildVolume &bv = wxGetApp().plater()->build_volume();
    if (! bv.valid())
        return false;
    const Polygon &bed = bv.polygon();
    if (bed.points.size() < 3)
        return false;

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
    if (! m_has_target || ! m_has_landing || ! m_raycaster)
        return false;
    const std::vector<Vec2d> foot = target_footprint_world();
    if (foot.empty())
        return false;
    Vec2d c { 0., 0. };
    for (const Vec2d &p : foot)
        c += p;
    c /= double(foot.size());

    // The axis of the pillar, in world.
    const Vec3d top(c.x(), c.y(), m_target_world_pos.z());
    const Vec3d bot = m_landing_world_pos;

    // 🔑 Both ends touch geometry ON PURPOSE — the top is under the surface being held up, the
    // bottom may rest on a shelf — so the ends are skipped and only the span in between is asked
    // about. Without that the answer would be "yes, always".
    const Transform3d inv  = m_world_trafo.inverse();
    const AABBMesh   &tree = m_raycaster->get_aabb_mesh();
    const int         n_samples = 24;
    int               inside = 0;
    for (int i = 1; i < n_samples; ++ i) {
        const double t = double(i) / double(n_samples);
        if (t < 0.10 || t > 0.90)
            continue;
        const Vec3d w = top + (bot - top) * t;
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
    return inside >= 3;
}

// -----------------------------------------------------------------------------
// Zone management
// -----------------------------------------------------------------------------

void GLGizmoSupportZones::delete_selected_zone()
{
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

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete support zone"));
    mo->delete_volume(size_t(vi));
    m_selected_zone = -1;
    m_zones_dirty   = true;
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    wxGetApp().plater()->update();
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
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
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
        if (g.version != 1 && g.version != 2)
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
    g.stamps.clear();
    g.stamps.reserve(m_stamps.size());
    for (const Stamp &st : m_stamps) {
        const Vec3d o = inv * Vec3d(st.c.x(), st.c.y(), 0.);
        g.stamps.emplace_back(o.x(), o.y(), st.r, double(st.stroke));
    }
    if (! g.stamps.empty())
        g.version = 2;
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
    // s289 — las marcas vuelven a mundo y con ellas la semilla, para que editar una zona pintada
    // enseñe el mismo trazo y no el parche entero.
    m_stamps.clear();
    for (const Vec4d &q : g.stamps) {
        const Vec3d w = inst * Vec3d(q.x(), q.y(), 0.);
        m_stamps.push_back({ Vec2d(w.x(), w.y()), q.z(), int(q.w()) });
        m_stroke_id = std::max(m_stroke_id, int(q.w()));
    }
    m_brush_seed_facet    = m_stamps.empty() ? -1 : facet;
    m_footprint_shrink_mm = g.edge_patch_mm;
    m_footprint_base_mm   = g.edge_foot_mm;
    m_lean_angle_deg      = g.lean_deg;

    // 🚨 P2 del plan, resuelto por el lado seguro: el tope del ángulo sale de milímetros por capa,
    // así que si el objeto se escaló —o si cambió la altura de capa o la boquilla desde que se
    // creó— un ángulo perfectamente válido entonces puede estar fuera de rango ahora. Se recorta y
    // se deja que el alzado enseñe el ángulo de verdad, en vez de dibujar uno que el motor no puede
    // seguir.
    const double a_max = max_lean_angle_deg();
    if (double(m_lean_angle_deg) > a_max)
        m_lean_angle_deg = float(a_max);

    m_has_target        = true;
    m_has_landing       = true;
    m_landing_locked    = true;   // el aterrizaje ya está decidido: no persigue al cursor
    m_target_pick_mode  = false;
    m_landing_pick_mode = false;
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
        std::snprintf(b, sizeof(b), "brush     %d marks, seed facet %d", int(m_stamps.size()), m_brush_seed_facet);
        L(b);
        for (size_t i = 0; i < m_stamps.size() && i < 200; ++ i) {
            std::snprintf(b, sizeof(b), "  mark %3d  (%.3f %.3f)  r=%.3f", int(i), m_stamps[i].c.x(), m_stamps[i].c.y(), m_stamps[i].r);
            L(b);
        }
    }

    const PatchGeom *pg = m_has_target
        ? patch(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()))
        : nullptr;
    if (pg == nullptr) {
        L("patch     NONE — no hay parche que extruir (revisa el recorte: puede que no toque la región)");
    } else {
        // Cuántas aristas están compartidas por más de dos triángulos: si esto no es 0 el parche no
        // es una superficie de verdad y las paredes van a salir cruzadas.
        std::map<std::pair<int,int>, int> ec;
        for (const Vec3i32 &t : pg->F)
            for (int k = 0; k < 3; ++ k)
                ++ ec[std::minmax(t[k], t[(k + 1) % 3])];
        int nonmanifold = 0;
        for (const auto &e : ec)
            if (e.second > 2)
                ++ nonmanifold;
        // Bucles del borde: 1 es lo normal; más de 1 significa agujeros dentro del parche.
        std::map<int, int> nxt;
        for (const auto &e : pg->border)
            nxt[e.first] = e.second;
        std::set<int> seen;
        int loops = 0;
        for (const auto &e : pg->border) {
            if (seen.count(e.first))
                continue;
            int v = e.first, guard = 0;
            while (nxt.count(v) && ! seen.count(v) && guard ++ < 1000000) {
                seen.insert(v);
                v = nxt[v];
            }
            ++ loops;
        }
        double inw_max = 0.;
        for (const Vec2d &n : pg->inward)
            inw_max = std::max(inw_max, n.norm());
        std::snprintf(b, sizeof(b), "patch     verts=%d  tris=%d  border edges=%d  loops=%d  non-manifold edges=%d",
                      int(pg->V.size()), int(pg->F.size()), int(pg->border.size()), loops, nonmanifold);
        L(b);
        std::snprintf(b, sizeof(b), "patch     z_low=%.3f  z_high=%.3f  max_inset=%.3f  max_outset=%.3f mm  miter max=%.2f  folded=%s",
                      pg->z_low, pg->z_high, pg->max_inset, pg->max_outset, inw_max,
                      pg->folded ? "YES (el suelo se solapa consigo mismo)" : "no");
        L(b);
        if (loops > 1)
            L("patch     ⚠️ mas de un bucle de borde: el parche tiene agujeros, el pilar llevara paredes por dentro");
        if (nonmanifold > 0)
            L("patch     🚨 aristas no-manifold: la region no es una superficie simple");
    }

    const std::vector<Vec2d> top  = target_footprint_world();
    const std::vector<Vec2d> foot = foot_outline_world();
    std::snprintf(b, sizeof(b), "outline   top pts=%d  foot pts=%d  too narrow=%d  off bed=%d  crosses object=%d",
                  int(top.size()), int(foot.size()), int(footprint_too_narrow()),
                  int(landing_off_bed()), int(pillar_crosses_object()));
    L(b);

    if (solid == nullptr) {
        L("solid     NONE — build_pillar_mesh() ha dicho que no. Mira la rodilla y el alcance de arriba.");
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
    seed("support_top_z_distance",       new ConfigOptionFloat(0.1));
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

void GLGizmoSupportZones::apply_see_through()
{
    // 🚨 Restore FIRST, always. Otherwise the second call saves an already-ghosted colour as "the
    // original" and the scene fades a bit more on every refresh until it is gone.
    restore_see_through();

    // The switch that stops the ghosted part from writing depth (3DScene.hpp). Without it a
    // see-through part still occludes everything drawn after it, and blends against itself in mesh
    // order, which is the "two drawings clashing" look.
    m_parent.get_volumes().set_transparent_depth_write(! (m_see_through && get_state() == On));

    if (get_state() != On)
        return;

    // Only the printed geometry is ghosted. Support enforcers and the other modifiers are already
    // drawn translucent and carry their own meaning: ghosting them too would erase the very blocks
    // this gizmo is about. (is_modifier is set from !is_model_part(), GLCanvas3D.cpp:3123.)
    if (m_see_through)
        for (GLVolume *v : m_parent.get_volumes().volumes) {
            if (v == nullptr || v->is_modifier || v->is_wipe_tower)
                continue;
            const ColorRGBA orig = v->color;
            ColorRGBA out = orig;
            out[3] = m_see_through_alpha;
            m_saved_colors.emplace(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()), orig);
            v->set_color(out);
        }

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
//   · la cuña fantasma  = ±(z_low - z)·tan(ángulo MÁXIMO), o sea el corredor del motor
//     (max_lean_angle_deg → SupportZoneProbe). Lo que cae fuera de la cuña, el motor no lo sigue.
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
        if (del_hit) {
            delete_selected_zone();
            return;
        }
        if (dup_hit) {
            duplicate_selected_zone();
            return;
        }
        if (unlock_hit) {
            unlock_selected_zone();
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
    ImGui::SameLine(0.f, 6.f);
    if (neo_glyph_toggle("##vxray", sz, m_see_through, Glyph::Xray,
                         _u8L("See through the part").c_str())) {
        m_see_through = ! m_see_through;
        apply_see_through();
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
    if (m_see_through) {
        if (neo_row_slider("##stalpha", _u8L("ghost").c_str(), &m_see_through_alpha, 0.05f, 0.75f, "%.2f",
                           _u8L("See through the part").c_str()))
            apply_see_through();
    }

    // 🚨 s287 — el mando de los marcadores SALE de dentro del bloque del fantasma. En s286b vivía
    // ahí porque los dos números se afinaban juntos, y el efecto secundario fue que la rejilla y el
    // mapa se leían como parte de la transparencia del objeto: bajabas el fantasma y los puntos se
    // iban con él. Son cosas distintas — el fantasma es la PIEZA, esto son los MARCADORES — y ahora
    // se manda en cada una por separado, con el fantasma encendido o apagado.
    if (m_show_gaps || m_see_through) {
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
void GLGizmoSupportZones::render_issue_tray()
{
    // s289 — el parche que se pliega sobre sí mismo visto desde arriba. El suelo del sólido es esa
    // misma triangulación aplastada, así que se solapa consigo mismo y el pie sale cruzado. No se
    // arregla en silencio: se dice, porque la salida es del usuario (recortar más pequeño), no de
    // la geometría.
    if (m_has_target) {
        const PatchGeom *pg = patch(m_target_facet_idx, Vec2d(m_target_world_pos.x(), m_target_world_pos.y()));
        if (pg != nullptr && pg->folded)
            neo_warn_row("##w_folded", _u8L("The patch folds over itself").c_str(),
                         _u8L("The patch folds over itself seen from above, so the foot of the pillar overlaps itself. Crop it smaller, or pick a flatter part of the surface.").c_str());
    }

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

    if (landing_out_of_reach()) {
        // Ámbar ganado: este enlace pide más inclinación de la que el motor puede seguir, y la
        // columna se quedaría detrás del dibujo. Decirlo aquí es la diferencia entre una herramienta
        // que informa y una que miente.
        neo_warn_row("##w_reach", _u8L("Too far for this lean").c_str(),
                     _u8L("Too far for this lean: the knee would come out below the bed. Steepen the lean, or bring the foot closer.").c_str());
        if (ImGui::SmallButton(_u8L("Bring it into reach").c_str()))
            bring_landing_into_reach();
    }

    // El árbol, dicho ANTES de crear nada. Es un aviso con fecha de caducidad a propósito: crear el
    // pilar lo arregla, y el aviso cuenta que lo va a arreglar en vez de pedirle al usuario que
    // vaya a buscar el ajuste.
    if (is_tree(effective_support_type()))
        neo_warn_row("##w_tree", _u8L("Support type is Tree").c_str(),
                     _u8L("The tree generator does not know about the corridor, so the lean and the knee you draw here would not come out. Creating the pillar switches this object to Normal, keeping whether it was auto or manual. It is an object setting: undo it, or remove it from the object list.").c_str());

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
    if (m_zones.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("No support zones yet. A zone is a support enforcer volume: add one from the object list, or pick a surface below.").c_str());
        ImGui::PopStyleColor();
    } else {
        render_zone_cards();
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
        const float bw = (avail - 6.f) * 0.5f;
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
            if (m_landing_pick_mode)
                m_target_pick_mode = false;
            m_parent.set_as_dirty();
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
        const double a_max = max_lean_angle_deg();
        char v[48], tip[256];

        std::snprintf(v, sizeof(v), "%.0f°", double(m_lean_angle_deg));
        std::snprintf(tip, sizeof(tip), "%s %.0f deg  ·  %s %.2f mm %s",
                      _u8L("At most").c_str(), a_max,
                      _u8L("the support can only step").c_str(),
                      SupportZones::corridor_step_mm(support_line_width_mm()),
                      _u8L("per layer").c_str());
        neo_stat_tile("##t_lean", tw, _u8L("LEAN").c_str(), v,
                      double(m_lean_angle_deg) >= a_max - 0.5 ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright),
                      float(double(m_lean_angle_deg) / std::max(1., a_max)), tip);

        ImGui::SameLine(0.f, 6.f);
        if (! m_has_landing) {
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
        // s289 — cuatro, y la cuarta es el pincel. Las tres primeras recortan alrededor del punto
        // que pinchaste; el pincel recorta contra lo que hayas ido marcando al arrastrar. Por
        // dentro todas producen lo mismo (`crop_shape()`), así que el resto del panel ni se entera.
        const Glyph       shape_g[4] = { Glyph::Patch, Glyph::Round, Glyph::Square, Glyph::Brush };
        const std::string shape_t[4] = { _u8L("Whole patch"), _u8L("Round"), _u8L("Square"), _u8L("Paint") };
        const std::string cut_tip    = _u8L("Cut around the point you picked. The roof still follows the surface, so it stays flush on a curve.");
        const std::string brush_tip  = _u8L("Drag on the surface to mark the area you want held up. Shift-drag rubs it out. The mark is the size of the brush, so a small brush draws a narrow strip.");
        for (int i = 0; i < 4; ++ i) {
            if (i > 0)
                ImGui::SameLine(0.f, 5.f);
            char id[16];
            std::snprintf(id, sizeof(id), "##shape%d", i);
            if (neo_glyph_toggle(id, 1.6f * neo_u(), int(m_foot_shape) == i, shape_g[i],
                                 i == 0 ? shape_t[i].c_str()
                                        : (shape_t[i] + "\n\n" + (i == 3 ? brush_tip : cut_tip)).c_str())
                && int(m_foot_shape) != i) {
                m_foot_shape       = FootShape(i);
                // Cambiar de forma no borra lo pintado: se puede ir y volver al pincel sin perder
                // el trazo, que es lo que uno espera de un modo y no de un botón destructivo.
                m_preview_dirty    = true;
                invalidate_patch();
                m_parent.set_as_dirty();
            }
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
                    m_brush_seed_facet = -1;
                    m_preview_dirty    = true;
                    invalidate_patch();
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
        if (neo_row_slider("##shrinkbase", _u8L("foot").c_str(), &m_footprint_base_mm,
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
        // 🔑 Este deslizador es el dato de entrada del pilar. Su tope NO lo pone el gusto del
        // usuario: sale de `d_max` (SupportZoneProbe.hpp), o sea de cuánto puede moverse el soporte
        // de una capa a la siguiente.
        const double a_max = max_lean_angle_deg();
        if (m_lean_angle_deg > float(a_max))
            m_lean_angle_deg = float(a_max);
        char tip[256];
        std::snprintf(tip, sizeof(tip), "%s %.0f deg  ·  %s %.2f mm %s",
                      _u8L("At most").c_str(), a_max,
                      _u8L("the support can only step").c_str(),
                      SupportZones::corridor_step_mm(support_line_width_mm()),
                      _u8L("per layer").c_str());
        if (neo_row_slider("##leanangle", _u8L("lean").c_str(), &m_lean_angle_deg,
                           1.f, float(std::max(2., a_max)), "%.0f deg", tip)) {
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
        const bool ready = m_has_target && m_has_landing && m_preview_model.is_initialized();
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
            ImGui::SetTooltip("%s", m_has_target ? _u8L("Click the bed, or a surface of the part below the one you picked.").c_str()
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
