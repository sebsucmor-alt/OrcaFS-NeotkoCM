// NEOTKO_SUPPORTZONES_TAG_START — s286, F2.5 skeleton.
// docs/FUTURE/SUPPORT_ZONES_PLAN.md §4-bis (the aimed corridor), §6 F2.5.
//
// The home of the Support Zones feature. This first pass DELIBERATELY CREATES NOTHING: it lists
// the zones that already exist, reports what F1's probe says each one will catch, and implements
// the first half of the creation gesture (pick the surface you want held up). The second pick and
// the loft come next, once the pick itself is proven in the hand.
//
// Why a gizmo of its own rather than a mode inside the FDM supports painter: the tool has to keep
// state while it is open (two picks, the reach, the zone list with its priority) and it will end
// up creating ModelVolumes, which is not what a facet painter does.
//
// Why NOT derived from GLGizmoPainterBase, which was the obvious guess: the only thing we wanted
// from it was the seed fill, and GLGizmoAlignStack already carries a connected coplanar flood-fill
// of its own (build_face_model) that does the same job without dragging in per-volume
// TriangleSelectors or the is_single_full_instance() restriction. This class is its sibling, not
// the painter's.
//
// 🚨 VOCABULARY (s286, owner's call). The reachable set is "reach" and what gets drawn on the bed
// is the "exit strip". The word "cone" is banned: it reads as "this makes cone-shaped supports",
// which is not what it is. "Fan" is banned too, and that one matters more than taste — §8 forbids
// proposing targets with a fan of rays against the mesh (US 9,524,357), and the word should not be
// anywhere near this feature.
//
// 🚨 PATENT BOUNDARY (§8, PATENT_US9524357_ANALYSIS.md). The down-facing normal filter below picks
// a DESTINATION. It must never become the criterion for whether support is needed there; that
// stays detect_overhangs(), a 2D offset between layers, in the backend.
#ifndef slic3r_GLGizmoSupportZones_hpp_
#define slic3r_GLGizmoSupportZones_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/ExPolygon.hpp"
// Por `SupportType` en la firma de `effective_support_type()`. Un enum sin tipo subyacente
// declarado no se puede adelantar, así que o entra la cabecera o la firma miente devolviendo un int.
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace GUI {

class GLGizmoSupportZones : public GLGizmoBase
{
public:
    GLGizmoSupportZones(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent &mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    bool               on_is_activable() const override;
    void               on_render() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;
    void               on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    // 🚨 Declarado AQUÍ ARRIBA a propósito: los miembros de una clase no pueden usar un tipo que se
    // declara más abajo en la misma clase (a diferencia de los cuerpos de función, que sí ven la
    // clase completa). Se declaró junto a su lógica y no compiló — el orden no es cosmético.
    //
    // Qué parte del parche se toma. El seed fill es coplanar, así que en un TECHO PLANO GRANDE se
    // lleva el techo entero y el pilar sale de ese tamaño; con el donut salió bien por casualidad,
    // porque la banda es estrecha. Se resuelve como un recorte exacto en XY dentro de `build_patch()`, no
    // como una geometría aparte: así un pilar redondo sigue siendo el mismo extrude de superficie
    // con menos triángulos dentro, y su techo sigue la curva en vez de ser una tapa plana que
    // dejaría hueco justo en las piezas que más lo necesitan.
    // 🔑 s289 — `Brush` es la cuarta y es de otra familia: las tres primeras recortan alrededor de
    // UN punto (el que pinchas), y `Brush` recorta contra la UNIÓN de todas las marcas que has ido
    // dejando al arrastrar. Por dentro son lo mismo — todas producen `crop_shape()`, una lista de
    // ExPolygons en XY de mundo contra la que se recorta el parche — así que el pilar, el borde,
    // la huella y el aviso no se enteran de cuál está activa.
    enum class FootShape : int { Patch = 0, Round = 1, Square = 2, Brush = 3 };

    // ------------------------------------------------------------------------
    // NEOTKO_SUPPORTZONES_TAG s301 — EL TOCÓN
    // ------------------------------------------------------------------------
    // 🔑 El gesto acordado en s300: «pintamos área, plantamos tocón, el resto crece solo». Un tocón
    // es un prisma corto plantado en la cama (o en una repisa), y NO se une con la cabeza: entre los
    // dos queda un hueco que el motor rellena bajando la columna hacia el tocón más cercano
    // (SupportMaterial.cpp, «GUIADO POR TOCÓN»). El tramo intermedio no se dibuja nunca.
    //
    // 🚨 Declarado aquí arriba por la misma razón que `FootShape`: un miembro no puede usar un tipo
    // que se declara más abajo en la misma clase.
    //
    // ⚠️ El mismo tipo se usa en dos espacios distintos y hay que saber cuál: `m_extra_stumps` está
    // en MUNDO, y `ZoneGesture::stumps` en espacio del OBJETO, como todo lo que se guarda.
    struct StumpSpot
    {
        Vec3d p      { Vec3d::Zero() };
        bool  on_bed { true };
    };

    // ------------------------------------------------------------------------
    // NEOTKO_SUPPORTZONES_TAG s288 — EL GESTO (docs/FUTURE/SUPPORT_ZONES_RELOCK_PLAN.md)
    // ------------------------------------------------------------------------
    // Lo que hizo falta para construir un pilar, guardado en la config del volumen para poder
    // volver a editarlo. Sin esto, una zona creada es una malla y nada más: pasarse dos grados con
    // la inclinación obliga a borrarla y repetir los dos clics.
    //
    // 🔑 EN ESPACIO DEL OBJETO, no en mundo, y por dos razones que pesan las dos:
    //   · en mundo, Orca renormaliza objeto e instancias sola (`ensure_on_bed`,
    //     `center_around_origin`) y el gesto se descoloca sin que nadie lo toque — es exactamente
    //     el bug que se pagó en s286b con la matriz del volumen;
    //   · `MeshRaycaster::get_closest_facet()` trabaja en coordenadas de MALLA y el raycaster de
    //     este gizmo se construye sobre `raw_mesh()`, que es espacio del objeto. Así que el dato
    //     guardado se le pasa tal cual a quien tiene que reencontrar la cara, sin transformar nada.
    //
    // ⛔ NO se guarda el índice de la cara: es un índice a la malla, y cualquier reparación o
    // reimportación lo deja apuntando a otro triángulo. Se guarda punto y normal, que sobreviven.
    //
    // ⚠️ Convención de instancia: como `create_pillar()`, todo se mide contra `instances.front()`.
    // Con varias instancias "el objeto" es ambiguo si nadie fija cuál.
    struct ZoneGesture
    {
        int       version { 1 };   // sin esto, el primer cambio de formato rompe proyectos viejos en silencio
        Vec3d     target_pos    { Vec3d::Zero() };
        Vec3d     target_normal { -Vec3d::UnitZ() };
        Vec3d     landing_pos   { Vec3d::Zero() };
        bool      on_bed        { true };
        FootShape foot_shape    { FootShape::Patch };
        float     foot_size_mm  { 8.f };
        bool      covers        { false };   // s289 — cubrir en vez de recortar
        // s289 — las marcas del pincel, en XY del ESPACIO DEL OBJETO (misma razón que todo lo
        // demás de aquí: el mundo se renormaliza solo). z no hace falta: el recorte es en XY.
        // Vacío en todo lo que no sea `Brush`, y en un gesto v1 leído de un 3mf viejo.
        std::vector<Vec4d> stamps;   // (x, y, radio, trazo)
        // s301 — los tocones ADICIONALES, en espacio del objeto. El primario NO está aquí: es
        // `landing_pos` / `on_bed`, que ya existían y ya se guardaban. Meterlo también en esta
        // lista sería tener dos verdades del mismo tocón.
        std::vector<StumpSpot> stumps;
        float     stump_size_mm { 8.f };
        float     edge_patch_mm { 0.f };
        float     edge_foot_mm  { 0.f };
        float     lean_deg      { 45.f };
        // 🔑 El testigo del candado: el `offset` que el gizmo le dejó puesto al volumen. Si ya no
        // coincide, alguien lo movió por fuera.
        Vec3d     lock_offset   { Vec3d::Zero() };
    };
    static std::string gesture_to_json(const ZoneGesture &g);
    static bool        gesture_from_json(const std::string &text, ZoneGesture &out);

    // ------------------------------------------------------------------------
    // The zone list: one row per SUPPORT_ENFORCER volume of the selected object.
    // ------------------------------------------------------------------------
    // Priority is the position in ModelObject::volumes among enforcers, which is the same number
    // the backend uses to decide who wins an overlap (SupportZoneSlices::priority, §6 F2). One
    // number, one meaning, in the panel and in the slicer.
    struct ZoneRow
    {
        int         volume_idx { -1 };   // index into ModelObject::volumes
        size_t      priority { 0 };      // position among the enforcers only
        std::string name;
        // F1's probe (libslic3r/Feature/SupportZones/SupportZoneProbe.hpp): how many grid cells of
        // this block actually sit over downward-facing surface. `lit == 0` is the silent failure
        // the whole of F1 exists to catch.
        size_t      cells_in_zone { 0 };
        size_t      lit { 0 };
        bool        sterile { true };
        // s287, para la tarjeta: el filamento del TECHO de esta zona (0 = como el objeto). Se lee
        // de la config del volumen, que es su único dueño; aquí sólo se cachea para poder pintar la
        // tapa del pilar en miniatura sin volver a abrir la config en cada frame.
        int         roof_filament { 0 };
        // s288 — el candado. `has_gesture` dice que esta zona nació del gizmo; `locked` dice que
        // además NADIE la ha tocado por fuera desde entonces, que es lo que la hace re-editable.
        // 🔑 Se DERIVA comparando la transformación viva con la que el gizmo dejó escrita, así que
        // no hay que engancharse a los gizmos de mover, rotar y escalar ni escuchar ningún evento.
        bool        has_gesture { false };
        bool        locked      { false };
        ZoneGesture gesture;
    };
    std::vector<ZoneRow> m_zones;
    int                  m_selected_zone = -1;   // index into m_zones, -1 = none
    // Selección diferida por índice de VOLUMEN, para cuando se pide antes de que la fila exista
    // (duplicar crea el volumen y la lista se reconstruye después). -1 = nada pendiente.
    int                  m_select_volume_idx_pending = -1;
    // The probe walks an AABB tree per zone, so it is refreshed on model changes, never per frame.
    bool                 m_zones_dirty = true;

    void rebuild_zone_rows();
    // The object the gizmo is currently working on, or nullptr.
    const ModelObject *current_object() const;
    int                current_object_idx() const;

    // ------------------------------------------------------------------------
    // Pick #1: the surface to hold up.
    // ------------------------------------------------------------------------
    // The gesture is destination first, landing second (owner's call, s286): starting on the bed
    // means fighting the camera, and picking the surface first is what lets the tool paint the
    // legal exit strip before the second click, so an impossible pillar cannot be drawn at all.
    bool m_target_pick_mode = false;

    // Raycasting state. Mirrors GLGizmoAlignStack's face pick: raw_mesh() is object-space, so the
    // world transform is refreshed every call and the mesh/tree only when the object changes.
    std::unique_ptr<MeshRaycaster> m_raycaster;
    int                            m_raycaster_obj_idx = -1;
    Transform3d                    m_world_trafo { Transform3d::Identity() };
    TriangleMesh                   m_mesh;
    std::vector<Vec3f>             m_face_normals;
    std::vector<Vec3i32>           m_face_neighbors;

    // One entry per surface the ray crosses that faces downward, nearest to the camera first.
    // 🔑 This is the answer to "we cannot guess whether they meant the top or the bottom face":
    // we do not guess. The upward faces are not candidates at all (the filter below), and when
    // several downward ones stack up — the underside of a shelf and the underside of the roof
    // above it — the user cycles with the wheel and the panel says which one is live.
    struct Candidate
    {
        int    facet_idx { -1 };
        double distance { 0. };    // along the ray, from the camera
        Vec3d  world_pos { Vec3d::Zero() };
        Vec3d  world_normal { -Vec3d::UnitZ() };
        // Is this surface past the support threshold angle, i.e. one the slicer would decide to
        // support on its own? It changes the COLOUR, never the eligibility (see below).
        bool   past_threshold { false };
    };
    std::vector<Candidate> m_candidates;
    int                    m_candidate_idx = 0;   // which one the wheel has landed on
    // NEOTKO_SUPPORTZONES_TAG s299 — contra qué se decide que la lista sigue valiendo.
    //
    // 🔑 `render_pick_overlays()` llamaba a `update_candidates()` en CADA frame mientras hubiera
    // cursor sobre la pieza, y eso es una consulta al árbol AABB por frame con el ratón quieto.
    // Con el píxel y la matriz guardados, un ratón parado no cuesta nada y uno que se mueve paga
    // exactamente una consulta por movimiento, que es lo que hace falta.
    // NEOTKO_SUPPORTZONES_TAG s299f — pintar fuera (la piel que ves) o dentro (una cavidad).
    //
    // 🔑 Por defecto FUERA, que es lo que uno espera y lo que evita que el pincel se cuele a la cara
    // interior de una pared sin haberlo pedido. DENTRO existe porque sujetar una superficie interna
    // es legítimo, pero tiene que ser una decisión.
    bool                   m_paint_inside = false;
    Vec2d                  m_cand_mouse { Vec2d(-1e9, -1e9) };
    Transform3d            m_cand_trafo { Transform3d::Identity() };
    Vec2d                  m_hover_mouse_pos { Vec2d::Zero() };
    bool                   m_have_hover_pos = false;

    // 🚨 Click bookkeeping, and it is not bureaucracy: a left click on empty space that this gizmo
    // does not consume reaches the canvas, which deselects the object — and on_is_activable()
    // needs an object, so the gizmo closes under the user. That is the "it drops me out when I
    // click on nothing" report from s286. The cure is to act on the mouse UP and consume it, while
    // leaving the DOWN alone so a left drag still orbits the camera.
    Vec2d m_mouse_down_pos { Vec2d::Zero() };
    bool  m_mouse_down = false;
    static constexpr double CLICK_SLOP_PX = 4.0;

    // Confirmed target.
    bool  m_has_target = false;
    int   m_target_facet_idx = -1;
    Vec3d m_target_world_pos { Vec3d::Zero() };
    Vec3d m_target_world_normal { -Vec3d::UnitZ() };

    // Overlays, rebuilt only when the facet they were built for changes.
    GLModel m_hover_model;
    int     m_hover_model_facet = -1;
    bool    m_hover_model_past  = false;  // which side of the threshold the built model was coloured for
    // Con qué recorte y en qué punto se construyeron los overlays: si cambian, hay que rehacerlos.
    Vec2d     m_hover_model_centre { Vec2d::Zero() };
    FootShape m_hover_model_shape  = FootShape::Patch;
    float     m_hover_model_size   = -1.f;
    FootShape m_target_model_shape = FootShape::Patch;
    float     m_target_model_size  = -1.f;
    // s289 — y cuántas marcas de pincel tenía cada overlay: pintar cambia el resaltado sin que la
    // cara ni el centro se muevan.
    size_t    m_hover_model_stamps  = size_t(-1);
    size_t    m_target_model_stamps = size_t(-1);
    GLModel m_target_model;
    int     m_target_model_facet = -1;

    // What can be PICKED: anything that is not effectively a wall. -0.10 is about 6 degrees off
    // vertical, so a 45-degree overhang (normal Z = -0.707) is well inside. Laterals are excluded
    // because a vertical wall needs no support and picking one would silently produce a pillar
    // that holds nothing.
    //
    // 🔑 Deliberately NOT gated on support_threshold_angle, and that is a design decision, not an
    // oversight. Cutting the picking at the threshold would remove exactly the case a manual zone
    // exists for: the long shallow overhang that is under the threshold and still sags. The
    // threshold decides the COLOUR (see past_threshold) so the tool informs without refusing.
    static constexpr float DOWN_FACING_MAX_NORMAL_Z = -0.10f;

    // ------------------------------------------------------------------------
    // Overhang highlight
    // ------------------------------------------------------------------------
    // "Orca shows nothing until you slice" was the real complaint (s286). It is not true: the
    // slope shading that the FDM supports painter offers as "Highlight overhang areas" is exactly
    // this, computed from facet normals with no slicing at all, and it is what lets a user see
    // that a sloped wall needs support and not only a flat ceiling. This gizmo turns it on while
    // it is open instead of reinventing it.
    //
    // 🚨 Render only (§8). Which surfaces actually need support stays detect_overhangs(), a 2D
    // offset between layers in the backend. This shading may never become that criterion.
    bool  m_show_overhangs = true;
    // El mapa de "esto se te ha quedado sin sujetar" (s286b, idea suya). Apagado por defecto: es
    // una pregunta que se hace al final, no el estado en el que se entra.
    bool  m_show_gaps = false;
    // Paso de la rejilla del mapa, en mm. 1 mm por defecto: fino para ver la forma de un hueco y
    // sin llegar a la sopa de puntos.
    // s289 — 0.5 mm por defecto, pedido por él tras usarlo: a 1 mm el mapa cuenta la historia pero
    // no enseña la FORMA del hueco, que es para lo que se mira.
    float m_gap_step_mm = 0.8f;   // s299f — pedido por el dueño tras usarlo: 0.8 es su punto
    // s289 — 30 por defecto (antes 45). ⚠️ Sigue sembrándose de `support_threshold_angle` al abrir
    // el gizmo, así que el preset manda si dice otra cosa: la herramienta y el motor hablando del
    // mismo ángulo vale más que un número fijo.
    float m_overhang_threshold_deg = 30.f;

    // 🔑 s286, in the hand: the canvas slope shading is painted INSIDE the part's own render, so
    // ghosting the part ghosts the marks too, and at a usable opacity (0.11) the highlight sank
    // into the object colour. Drawing our own overlay for the same facets fixes that at the root:
    // it is a separate model, so the part's alpha cannot touch it, and it goes through the same
    // x-ray pass as the picked patch so it reads through walls without orbiting the camera.
    //
    // The canvas shading stays on as well; the two agree because they share the threshold. Outside
    // the gizmo the canvas one is all there is, which is what the owner wanted.
    GLModel m_overhang_model;
    bool    m_overhang_model_dirty = true;
    // Anything past this is not worth drawing as one blob and would cost more than it explains.
    static constexpr size_t OVERHANG_MAP_MAX_FACETS = 200000;
    void build_overhang_model();
    bool  m_threshold_seeded = false;   // seeded once from support_threshold_angle per opening

    void apply_overhang_highlight();
    void restore_overhang_highlight();
    // The normal-Z line matching m_overhang_threshold_deg, in the SAME formula GLCanvas3D uses for
    // the shading, so the colour of a patch can never disagree with the shading under it.
    float overhang_normal_z_cut() const;

    // ------------------------------------------------------------------------
    // Pick #2: where the pillar lands.
    // ------------------------------------------------------------------------
    // Either the bed, or an upward-facing surface of the SAME part — a shelf, the top of a
    // crossbar. Landing on a shelf of the part is ordinary, not exotic: detect_bottom_contacts()
    // resolves it and support_bottom_z_distance (Slicing.cpp:120) is the gap that exists for
    // exactly that, while on the bed there is no gap because there is nothing underneath.
    //
    // ⚠️ NOT the same thing as landing on ANOTHER object of the plate. That one is F5 and the
    // print order is unresolved (cross_object_active(), InstanceContact.hpp:90).
    bool  m_landing_pick_mode = false;
    bool  m_has_landing = false;
    // While false the landing follows the cursor and the pillar is redrawn live; the click latches
    // it so the footprint can then be dialled without the preview running away.
    bool  m_landing_locked = false;
    Vec3d m_landing_world_pos { Vec3d::Zero() };
    // §4.2-bis asks for Stop vs Straighten to be chosen EXPLICITLY, never left to chance. The
    // gesture already carries the answer: land on the bed and the column descends straight down,
    // land on a surface and it stops there. No question to ask the user.
    bool  m_landing_on_bed = true;

    // How far the target footprint is pulled inward, in mm. Picking a face gives you the whole
    // face; this is the "make it smaller" dial, and it redraws live.
    // El borde, en dos alturas. Positivo mete hacia dentro, negativo saca. Interpolados por altura
    // en build_pillar_mesh: rama gorda donde sujeta, fina donde apoya. Idea suya (s286b) tras ver
    // que engordar para agarrar un alero engordaba también el pie, donde sólo es material.
    float m_footprint_shrink_mm = 0.f;   // en el parche (arriba)
    float m_footprint_base_mm   = 0.f;   // en el pie (abajo)

    // The pillar as it would be built, in WORLD coordinates. 🔑 The same mesh the Create button
    // hands to the new volume, so the preview cannot lie about what you are going to get.
    GLModel      m_preview_model;
    bool         m_preview_dirty = true;

    void ensure_raycaster();
    void clear_pick();
    // Where a pillar would land for this cursor position: a surface of the part below the target
    // if there is one, the bed otherwise. Used by both the live preview and the click.
    bool resolve_landing(const Vec2d &mouse_pos, Vec3d &out_pos, bool &out_on_bed);
    // NEOTKO_SUPPORTZONES_TAG s300g — el aterrizaje A PLOMO, que es el que se usa de salida.
    //
    // Mismo criterio que `resolve_landing()` —primero una repisa de la pieza que mire hacia arriba,
    // si no la cama— pero con un rayo vertical bajo el centroide de la huella en vez del rayo de
    // cámara. Sin ratón de por medio: es el aterrizaje que el pilar tendría si nadie lo tocase.
    bool resolve_landing_plumb(Vec3d &out_pos, bool &out_on_bed);
    // Facet indices of the connected coplanar patch grown from `facet_idx`.
    std::vector<int> collect_region(int facet_idx) const;
    // s289 — el LIENZO del pincel: conectada y mirando hacia abajo, sin límite de ángulo contra la
    // semilla. El porqué vive con la definición; en corto: el límite coplanar de 20° es correcto
    // para pinchar una cara y es justo lo que hacía que en una curva no se pudiera pintar nada.
    std::vector<int> collect_paint_region(int facet_idx) const;

    // ------------------------------------------------------------------------
    // La huella: qué parte del parche se toma (s286b)
    // ------------------------------------------------------------------------
    // El porqué de esto vive con la declaración de `FootShape`, arriba del todo.
    // 🔑 s289, decisión suya: se entra en CUADRADO, no en «parche entero». El parche entero es el
    // modo que NO se puede expandir (no hay forma que sustituir), así que entrar por ahí esconde
    // justo lo que hace útil a la herramienta. Entrando en cuadrado el usuario ve el par
    // recortar/cubrir desde el primer segundo y descubre solo que la zona puede crecer.
    FootShape m_foot_shape = FootShape::Square;
    float     m_foot_size_mm = 8.f;   // diámetro del redondo / lado del cuadrado
    // 🔑 s289, idea suya — RECORTAR vs CUBRIR. Recortar es lo de siempre: la forma se INTERSECA con
    // el parche, así que nunca puede ser más grande que la superficie que pinchaste. Cubrir la
    // SUSTITUYE: la sección del pilar es la primitiva, y el techo sigue la superficie donde la hay
    // (un rayo vertical por nodo de rejilla) y se queda en el plano de la cara donde no.
    //
    // Por qué hacía falta: engordar el borde tiene un techo duro — el contorno del parche acaba
    // cruzándose consigo mismo, y por eso el deslizador se queda corto. Con una primitiva ese
    // techo no existe. El precio, dicho por él antes de que nadie lo preguntara, es que el bloque
    // ocupa más área de la que sujeta.
    //
    // ⛔ Por defecto RECORTAR: es lo que hacían las zonas que ya existen, y una zona guardada no
    // puede cambiar de forma al reabrirla.
    bool      m_shape_covers = false;

    // ------------------------------------------------------------------------
    // NEOTKO_SUPPORTZONES_TAG s299 — LA HUELLA DEJA DE SER GEOMETRÍA 3D Y PASA A SER UNA MÁSCARA.
    //
    // 🔑 Lo dijo el dueño y es el cambio de raíz: "pintar un área es pintar un área". Lo de antes
    // era un parche de TRIÁNGULOS DE LA MALLA, recortado triángulo a triángulo contra la forma y
    // desplazado vértice a vértice. De ahí salían los tres problemas que se veían en la mano, y los
    // tres eran el mismo problema:
    //
    //   · **la CPU**: cada marca del pincel rehacía el parche entero. Recorrer todos los triángulos
    //     de la región y cortar cada uno contra una máscara que crece con cada pincelada es
    //     cuadrático en el trazo. Ahora pintar sólo une un disco a un `ExPolygons` y el sólido no
    //     se construye hasta que sueltas el botón;
    //   · **expandir**: el "crecer" era mover cada vértice por su bisectriz, que NO es un offset y
    //     no sabe resolver que el borde se cruce consigo mismo. Por eso al expandir dos veces
    //     salían polígonos imposibles. Ahora crecer es `offset_ex()`, que no puede devolver un
    //     contorno cruzado;
    //   · **el cuadrado era el que mejor iba**, y no por suerte: cuatro vértices convexos son el
    //     único caso donde mover por bisectriz coincide con un offset de verdad.
    //
    // Lo que se conserva a propósito: el techo NO es plano. La z de cada punto sale del mapa de
    // alturas de abajo, así que un círculo grande sobre una zona curva sigue quedando a ras.
    //
    // 🚨 Frontera del §8 intacta: esto elige DÓNDE va el bloque, nunca decide que ahí haga falta
    // soporte. Eso sigue siendo `detect_overhangs()`.
    struct ZoneMask
    {
        // La huella, en XY de MUNDO. Sale de Clipper, así que es válida por construcción: sin
        // auto-intersecciones, con sus islas separadas y sus agujeros donde toque.
        ExPolygons area;

        // El plano de la cara semilla: el respaldo donde el mapa de alturas no tiene nada. Es plano
        // y está en el aire, que es lo que uno espera de un bloque que sobresale del voladizo.
        Vec3d  seed_p { Vec3d::Zero() };
        Vec3d  seed_n { Vec3d(0., 0., -1.) };

        double z_low  { 0. };
        double z_high { 0. };
        bool   ok     { false };
    };

    // --- el mapa de alturas del techo -----------------------------------------------------------
    // 🔑 s299b — cubre SÓLO la caja de lo pintado, con margen, y se muestrea con un rayo vertical
    // por nodo. Unos miles de rayos, y el coste deja de depender del tamaño del objeto: depende del
    // tamaño de la zona, que es lo que el usuario controla.
    //
    // 🚨 La primera versión de s299 lo rasterizaba desde los triángulos de la región ENTERA, y en
    // una pieza de millones de polígonos eso era media espera; la otra media era el flood-fill que
    // ya no hace falta.
    mutable Vec2d              m_hm_origin { Vec2d::Zero() };
    mutable double             m_hm_step   { 0.5 };
    mutable int                m_hm_nx     { 0 };
    mutable int                m_hm_ny     { 0 };
    mutable std::vector<float> m_hm_z;      // NaN donde no hay superficie mirando hacia abajo
    mutable std::vector<Vec3f> m_hm_n;      // normal de quien puso esa z
    // La caja del mapa sólo crece, así que seguir pintando encima no vuelve a muestrear nada.
    mutable Transform3d        m_hm_trafo { Transform3d::Identity() };
    mutable double             m_hm_z_ref { 0. };
    void   ensure_heightmap(const BoundingBox &area_bb, double z_ref) const;
    // s300c — la variante del pincel: sin rayos, desde los triángulos que ya están marcados.
    void   heightmap_from_painted(const BoundingBox &area_bb) const;
    // z de la superficie en un punto XY de mundo, bilineal, con respaldo al plano de la semilla.
    double z_at(const ZoneMask &mk, const Vec2d &p) const;
    // La normal en ese punto, para la corrección del techo por plano tangente.
    Vec3d  n_at(const ZoneMask &mk, const Vec2d &p) const;
    bool build_mask(int seed_facet, const Vec2d &centre_xy, ZoneMask &out) const;
    // La misma, cacheada contra lo único que la cambia. Devuelve nullptr si no hay huella.
    //
    // 🚨 s299 — la clave sigue teniendo casi los mismos campos, pero lo que cuesta una fallada ha
    // cambiado de orden de magnitud. Antes, una marca más invalidaba y recorría la malla entera;
    // ahora el lienzo y el mapa de alturas están cacheados aparte, así que lo único que se rehace
    // es una intersección de Clipper contra la unión de marcas, que ya viene hecha.
    const ZoneMask *mask(int facet_idx, Vec2d centre_xy) const;
    mutable ZoneMask    m_mask_cache;
    mutable int         m_mask_cache_facet = -1;
    mutable int         m_mask_cache_shape = -1;
    mutable float       m_mask_cache_size  = -1.f;
    mutable Vec2d       m_mask_cache_centre { Vec2d(1e9, 1e9) };
    mutable size_t      m_mask_cache_stamp = size_t(-1);
    mutable Transform3d m_mask_cache_trafo { Transform3d::Identity() };
    void                invalidate_patch();

    // El lienzo: la región de la malla que mira hacia abajo, cacheada POR OBJETO y no por cursor.
    // 🔑 s299 — el flood-fill no depende de dónde esté el ratón una vez elegida la semilla, así que
    // rehacerlo con cada marca (hasta 200.000 caras) era trabajo tirado.
    const std::vector<int> &region_for(int seed_facet) const;
    mutable std::vector<int> m_region_cache;
    mutable int              m_region_cache_facet = -1;
    // 🚨 La matriz entra en la clave: el lienzo y su mapa de alturas están en MUNDO, así que mover
    // o rotar el objeto los cambia enteros sin que el índice de la cara se mueva un pelo. Sin esto
    // la caché sobreviviría a un arrastre y el pilar se construiría donde el objeto ESTABA.
    mutable Transform3d      m_region_cache_trafo { Transform3d::Identity() };
    // La proyección XY de esa región, que es el lienzo real donde puede caer la huella.
    mutable ExPolygons       m_region_area_cache;

    // Un anillo del sólido, remuestreado a un número fijo de puntos por longitud de arco. Es lo que
    // permite coser las paredes entre dos anillos que Clipper ha calculado por separado y que por
    // tanto no comparten ni número de vértices ni orden.
    static std::vector<Vec2d> resample_ring(const ExPolygons &area, int n_pts);
    // La silueta en XY del anillo del parche desplazado `edge_mm` por su borde: es literalmente la
    // sección del sólido a esa altura, proyectada. Los dos contornos que usa el panel salen de
    // aquí, así que los dos son el sólido.
    std::vector<Vec2d> ring_outline_world(double edge_mm) const;

    // NEOTKO_SUPPORTZONES_TAG s300 — EL PINCEL PINTA TRIÁNGULOS, NO ÁREA EN XY.
    //
    // 🔑 Idea suya, dicha en lenguaje de Rhino: "yo haría un WireCut de la zona pintada y un extrude
    // surface en el ángulo". Esa es exactamente la mitad que faltaba. Lo de antes marcaba discos en
    // XY y de ahí salían tres fallos que fuimos tapando uno a uno:
    //
    //   · el pincel se colaba a la cara de DENTRO de una pieza hueca, porque el candidato era "el
    //     primer impacto que mira hacia abajo" y ése podía estar detrás de una pared;
    //   · al pasar por encima de un agujero, la marca aparecía AL OTRO LADO y la banda que unía dos
    //     marcas cruzaba el aire;
    //   · y la huella era la proyección del trazo, así que un arco que rodea un agujero salía como
    //     una banda de 99 mm que no se parecía a lo pintado (medido en su log).
    //
    // Los tres se caen solos marcando TRIÁNGULOS: la marca vive en la superficie, y lo que decide
    // hasta dónde llega es la VECINDAD de la malla, no la distancia en pantalla. Al otro lado de un
    // agujero no se llega porque no hay triángulos que conecten.
    //
    // ⚠️ Lo que NO cambia: el sólido se sigue construyendo con Clipper a partir de la proyección de
    // lo pintado (anillos, offsets, la rodilla). Esa mitad funciona y no se toca — el híbrido es
    // pintar en 3D y extruir en 2D.
    // 🚨 s300b — TRES ESTRUCTURAS Y NINGÚN RECORRIDO DE LA MALLA ENTERA. La primera versión de esto
    // se colgaba con su pieza, y por un motivo que no se ve mirando una sola función: cada marca del
    // pincel recorría los millones de triángulos TRES veces (contar los pintados, proyectarlos,
    // dibujarlos) y encima reservaba un `visited` del tamaño de la malla. Multiplicado por cada
    // movimiento del ratón, eso es la ruedita.
    //
    // 🔑 Lo que hace instantáneos a los painters de Orca no es marcar bytes, es no tocar nunca lo
    // que no ha cambiado. Aquí:
    //   · `m_painted`      responde "¿está pintado este triángulo?" en O(1);
    //   · `m_painted_list` es la lista de los que SÍ, y es por donde se itera siempre — son cientos,
    //     no millones. Nadie recorre `m_painted` de punta a punta;
    //   · `m_visit_stamp`  sustituye al `visited` de cada pincelada por un sello que se compara, así
    //     que el recorrido no reserva ni limpia nada.
    std::vector<char>     m_painted;         // 1 por triángulo de m_mesh, 1 = pintado
    std::vector<int>      m_painted_list;    // los índices pintados, sin orden
    size_t                m_painted_count = 0;
    mutable std::vector<uint32_t> m_visit_stamp;
    mutable uint32_t              m_visit_epoch = 0;
    void clear_painted();
    // La proyección, cacheada contra el sello: la piden el sólido, el contorno y los avisos, varias
    // veces por frame, y sólo cambia cuando cambia lo pintado.
    mutable ExPolygons    m_painted_area;
    mutable size_t        m_painted_area_stamp = size_t(-1);
    mutable Transform3d   m_painted_area_trafo { Transform3d::Identity() };
    // Marca los triángulos alcanzables desde `seed` cuya distancia al punto tocado sea menor que el
    // radio, andando SOLO por vecinos. Devuelve cuántos cambiaron.
    size_t paint_facets(int seed, const Vec3d &hit_world, double radius, bool erase);
    // La proyección en XY de lo pintado, que es lo que come el constructor del sólido.
    ExPolygons painted_area_world() const;

    // El registro del gesto: cada toque, en MUNDO y en 3D. Es lo que se guarda para poder reabrir la
    // zona, y lo que se vuelve a reproducir al editarla.
    // ⛔ Ya no hay `stroke`: existía para unir dos marcas seguidas con una cápsula, y las cápsulas se
    // han ido con la proyección. La continuidad la da ahora la malla.
    struct Stamp { Vec3d p { Vec3d::Zero() }; double r { 4. }; };
    std::vector<Stamp> m_stamps;
    // El sello sube con cada cambio de lo pintado y es lo que entra en la clave de la caché.
    size_t     m_stamp_stamp = 0;
    void       repaint_from_stamps();
    bool  m_painting     = false;   // botón abajo y pintando (no orbitando)
    bool  m_paint_erase  = false;   // con Shift: quita en vez de poner
    // Añade (o quita) una marca en la posición del cursor si cae sobre la superficie. Devuelve si
    // hizo algo, para poder pedir un frame sólo cuando cambia.
    bool  paint_at(const Vec2d &mouse_pos, bool erase);
    // La cara semilla del pincel: la primera que se pintó. El parche sigue siendo el flood-fill
    // desde ella, así que pintar NUNCA se salta una esquina hacia otra cara.
    int   m_brush_seed_facet = -1;

    // ⛔ s289 — aquí vivía `shaped_region()`, el recorte por CENTROIDE del triángulo. Se ha
    // quitado, no movido: era el fallo de raíz de las superficies sencillas. Un filtro por
    // centroide tiene la resolución de la malla, así que en una cara de dos triángulos sólo sabía
    // contestar "todo" o "nada". El recorte de verdad vive dentro de `build_patch()`.
    // El contorno XY de la huella, en mm de mundo, ya metido hacia dentro por m_footprint_shrink_mm.
    // ✅ s286b: ya NO es el casco convexo, es el contorno real (unión de los triángulos
    // proyectados). El casco metía la huella en la muesca de un parche en L, y además era lo que
    // obligaba a cerrar las tapas con un abanico, que sólo vale en convexo.
    std::vector<Vec2d> target_footprint_world() const;
    // El contorno DEL PIE, que con el borde progresivo ya no es el mismo que el de arriba. Lo
    // quieren las dos preguntas que van del apoyo: si es demasiado estrecho para imprimirse y si
    // se sale de la cama. Preguntárselo al de arriba avisaría de un pie que no existe.
    std::vector<Vec2d> foot_outline_world() const;
    // 🚨 s299 — ESTOS DOS YA NO SON TOPES GEOMÉTRICOS, son el RANGO DEL DESLIZADOR.
    //
    // Antes eran el punto donde el desplazamiento por vértice se cruzaba consigo mismo, calculado
    // con una condición local que sólo veía la inversión de una arista y no que dos aristas lejanas
    // chocaran. Por eso protegían mal y el borde salía imposible al expandir dos veces.
    //
    // Con `offset_ex()` el colapso no hay que predecirlo: cuando te pasas metiendo, la forma sale
    // VACÍA y el sólido no se construye. Así que esto es sólo cuánto ofrece el mando.
    double max_inset_mm() const;
    double max_outset_mm() const;
    // 🚨 s286b: la huella dejó de ser barata. El casco convexo era un barrido y ya está; el
    // contorno real es flood-fill + unión de N triángulos + Douglas-Peucker, y el PANEL la pide
    // media docena de veces por frame (demasiado estrecha, fuera de la cama, desplazamiento,
    // travesía, alcance, preview). Eso es un flood-fill de la malla por cada pregunta y por cada
    // frame. Se cachea contra lo único que la cambia: qué cara está tomada y cuánto se ha metido
    // hacia dentro. `mutable` porque quien pregunta es const, y con razón: preguntar no cambia
    // nada de lo que se ve.
    mutable std::vector<Vec2d> m_foot_cache;
    mutable int                m_foot_cache_facet = -1;
    mutable float              m_foot_cache_shrink = -1.f;
    mutable int                m_foot_cache_shape  = -1;
    mutable float              m_foot_cache_size   = -1.f;
    // 🚨 El centro entra en la clave y NO es residuo del "repetir" que se quitó: con la huella
    // recortada, pinchar otro punto de LA MISMA cara da otro contorno sin que el índice de la cara
    // cambie. Sin esto la caché devolvería el recorte del punto anterior.
    mutable Vec2d              m_foot_cache_centre { Vec2d(1e9, 1e9) };
    // s299 — el sello de la unión de marcas: pintar una más cambia la huella sin mover nada más.
    mutable size_t             m_foot_cache_stamp = size_t(-1);
    mutable double             m_foot_cache_z_low = 0.;   // el z más bajo del parche, del mismo barrido
    // 🚨 Y la matriz entra en la clave: la huella está en MUNDO, así que mover o rotar el objeto la
    // cambia entera sin que el índice de la cara se mueva un pelo. Sin esto la caché sobreviviría a
    // un arrastre y el pilar se construiría donde el objeto ESTABA.
    mutable Transform3d        m_foot_cache_trafo { Transform3d::Identity() };
    // Builds the skewed prism between the picked patch and the landing spot. Returns false when
    // there is nothing sane to build.
    //
    // 🔑 s301 — ES EL ÚNICO PUNTO DE ENTRADA, y bifurca en su primera línea. Lo llaman el preview,
    // crear, editar y el botón de volcado; despachar aquí es lo que impide que uno de los cuatro se
    // quede con el constructor viejo sin que nadie se entere.
    bool build_pillar_mesh(TriangleMesh &out_world) const;
    // ------------------------------------------------------------------------
    // NEOTKO_SUPPORTZONES_TAG s301 — EL ÁRBOL DE BLOQUES
    // ------------------------------------------------------------------------
    // 🔑 El pincel ES el modo nuevo (decisión suya): pintar marca el OBJETIVO, y la huella deja de
    // salir de proyectar la superficie en planta — que es la causa medida en el §1 del estudio de
    // los 42 picos del techo y del contorno que pasaba de 94 a 292 puntos.
    //
    // ⛔ Círculo, cuadrado y «parche entero» siguen dando el pilar lofteado de siempre. Funcionan,
    // son personalizables y no cuesta mantenerlos.
    bool block_tree_mode() const { return m_foot_shape == FootShape::Brush; }
    // La cabeza (prisma recto sobre lo pintado) más un prisma por tocón, en una sola malla con las
    // dos partes SEPARADAS: ese hueco es lo que el motor reconoce como árbol de bloques.
    bool build_block_tree_mesh(TriangleMesh &out_world) const;
    // Todos los tocones en MUNDO, el primario incluido. Lo piden la malla, el alcance, el aviso de
    // fuera de cama y la sombra del pie: una sola lista para que las cuatro respuestas no se
    // separen.
    std::vector<StumpSpot> all_stumps() const;
    // Los milímetros que separan la tapa del tocón más alto de la base de la cabeza. 🔑 Es EL número
    // de esta feature: si no es positivo no hay hueco, el motor no ve un árbol de bloques y la
    // columna baja como un pilar corriente. Lo leen el log del constructor y el aviso del panel —
    // uno solo, para que no puedan discrepar.
    double block_tree_gap_mm() const;
    // La cota de la cabeza: de dónde a dónde va el prisma. La piden el constructor y el hueco de
    // aquí arriba, y por eso vive aparte — calcularla dos veces es cómo se separan dos números que
    // tenían que ser el mismo.
    bool   block_tree_head_z(double &z_bot, double &z_top) const;
    // Los ADICIONALES. El primario es `m_landing_world_pos` / `m_landing_on_bed`, que ya existen,
    // ya se siembran a plomo (`resolve_landing_plumb`, s300g) y ya se guardan en el gesto.
    std::vector<StumpSpot> m_extra_stumps;
    bool  m_stump_pick_mode = false;
    float m_stump_size_mm   = 8.f;
    // 🔑 Altura FIJA y no un mando (decisión suya): la altura de un tocón no es algo que se juzgue
    // mirándola, sólo tiene que dar unas cuantas capas de imán y de pie. 2 mm son 6 capas a 0.32.
    static constexpr double STUMP_HEIGHT_MM = 2.0;
    void update_preview();
    void render_preview();

    // ------------------------------------------------------------------------
    // The reach, and the exit strip
    // ------------------------------------------------------------------------
    // §3 is a BRAKE, not a control: the lean is a limit, and a corridor drawn steeper than the
    // engine can follow does not make the column lean more, it makes it fall behind. So the tool
    // has to say so BEFORE the click, which is the whole "an editor that draws what will not come
    // out is exactly what §1 promises not to be".
    //
    // 🔑 The radius comes from SupportZones::corridor_reach_mm(), the same function the support
    // generator clamps with. The strip on screen and the brake in the engine are one number.
    //
    // 🚨 Never say "cone" (owner's call, s286): it reads as "this makes cone-shaped supports".
    // And never "fan" — §8 forbids proposing targets with a fan of rays, and the word has no
    // business near this feature.
    double reach_radius_mm(double z_landing) const;
    bool   landing_out_of_reach() const;
    // How far the current landing sits from straight below the target, in mm.
    double landing_offset_mm() const;
    // Where the lean starts: the LOWEST point of the picked patch. Above it the pillar is vertical
    // (it has to wrap a patch that spans height), so that is the only stretch the corridor has to
    // walk sideways in, and the only height the reach may be measured over.
    double lean_top_z() const;

    // ------------------------------------------------------------------------
    // El pilar con rodilla (s286b)
    // ------------------------------------------------------------------------
    // 🔑 El ángulo es el DATO DE ENTRADA, no una consecuencia. El freno del §3 es un tope al
    // desplazamiento por capa, así que un pilar que se inclina a ángulo fijo pide `dz · tan(a)` en
    // cada capa: constante y comparable con el tope de un vistazo. De las tres magnitudes
    // {distancia del pie, altura de la rodilla, ángulo} sólo dos pueden ser libres — decisión suya:
    // el 2º clic sigue eligiendo el SITIO y la rodilla se deduce.
    float m_lean_angle_deg = 45.f;
    bool  m_lean_angle_seeded = false;   // se siembra al fijar el primer parche, no en cada uno

    // La anchura de línea del soporte y la altura de capa que MANDAN, resueltas como las resuelve
    // el motor. Estaban enterradas dentro de reach_radius_mm() y ahora las necesitan tres sitios.
    double support_line_width_mm() const;
    // 🚨 Con ALH, la capa más GRUESA: el tope es por capa, así que es la que se pasa antes.
    double worst_layer_height_mm() const;
    // El techo del deslizador, del dueño único (SupportZoneProbe.hpp).
    double max_lean_angle_deg() const;

    // Pulls the landing back onto the edge of the strip, keeping its direction.
    void   bring_landing_into_reach();

    // ------------------------------------------------------------------------
    // The two ways a drawn pillar can be nonsense even when it is inside the reach
    // ------------------------------------------------------------------------
    // §4-bis.8 A1: a link that tunnels through the part. The reach says nothing about this — you
    // can be well inside the strip and still have the object in the way — and a pillar buried in
    // the model is worse than no pillar, because it prints and welds.
    // NEOTKO_SUPPORTZONES_TAG s300h — LA FORMA DEL EJE DEL PILAR, EN UN SOLO SITIO.
    //
    // 🚨 El pilar NO es un prisma recto del techo al pie: tiene RODILLA. Baja a plomo desde el
    // techo hasta M, se inclina de M a K, y vuelve a plomo de K a B. La recta que une los dos
    // extremos corta por DENTRO de esa ele, y por ahí es justo por donde pasa la pared de la pieza.
    // Sondear a lo largo de esa recta es preguntar por sitios donde el pilar no está.
    //
    // Se calcula aquí, una vez, y lo usan `build_pillar_mesh()` (que construye la malla) y
    // `pillar_crosses_object()` (que la interroga). Tenerlo en dos sitios es exactamente la trampa
    // que este fichero ya documenta con los centroides: dos cuentas del mismo dato se separan.
    struct PillarAxis {
        double z_low { 0. }, z_high { 0. }, z_bot_ext { 0. }, z_knee { 0. };
        Vec2d  c_top { Vec2d::Zero() }, shift { Vec2d::Zero() };
        bool   wraps { false }, has_knee { false };
        // De arriba abajo: T, [M], [K], B. `off` es el desplazamiento XY de ese anillo.
        std::vector<std::pair<double, Vec2d>> rings;
    };
    bool pillar_axis(PillarAxis &out) const;

    bool pillar_crosses_object() const;
    // NEOTKO_SUPPORTZONES_TAG s300g — caché de una sola respuesta, y hace falta.
    //
    // El aviso lo preguntan tres sitios por frame (el color del alzado, la bandeja de avisos y la
    // puerta del botón de crear) y desde s300g sondea el contorno además del eje: 9 sondas × 19
    // muestras. Sin caché serían ~500 raycasts por frame en un panel que se redibuja siempre.
    // La firma es lo único que puede cambiar la respuesta.
    //
    // 🚨 s300i — LA FIRMA LLEVA head Y foot, y su ausencia era un bug con cara de otra cosa.
    // Visto por el dueño: "no me dejaba crear, toqué el ancho de cabeza a negativo y se activó, lo
    // puse a 0.0 y me dejó". El borde cambia la huella, o sea el cuerpo que se sondea — pero no
    // movía ni el techo, ni el pie, ni el número de puntos del contorno, así que la firma no se
    // enteraba y el veredicto se quedaba congelado del cálculo anterior.
    mutable Vec3d m_cross_cache_top   { Vec3d::Zero() };
    mutable Vec3d m_cross_cache_bot   { Vec3d::Zero() };
    mutable size_t m_cross_cache_npts { 0 };
    mutable float m_cross_cache_head  { 0.f };
    mutable float m_cross_cache_foot  { 0.f };
    mutable bool  m_cross_cache_valid { false };
    mutable bool  m_cross_cache_value { false };
    // §4-bis.8 A4: a footprint too narrow to extrude. The corridor would strangle the column and
    // remove_sticks() in project_support_to_grid() would then eat what is left (T7).
    bool footprint_too_narrow() const;
    // The width the footprint is measured against, in mm: a couple of extrusions.
    double min_footprint_width_mm() const;
    // §4-bis.8 A2: the whole point of this tool is to walk the column AWAY from the object, so a
    // foot off the plate is not an exotic case here, it is the natural failure of using it well.
    // Landing on a shelf of the part is exempt: that foot never reaches the bed.
    bool landing_off_bed() const;
    // §4-bis.8 A3 / T5-T6: with any style other than Snug the support grid re-aligns itself to the
    // bounding box of each layer, so a column that should SLIDE comes out in cell-sized steps. The
    // engine already says so in its own log (SupportMaterial.cpp, "support_style no es Snug"); this
    // is the same sentence said where the drawing happens, before the lean is committed.
    //
    // 🔑 Resolved the way SupportGridWrapper's constructor resolves it, not by reading the enum
    // raw: smsDefault AND every tree style collapse to smsGrid down there, so anything that is not
    // literally smsSnug is grid. Reading the raw value would let "Default" look innocent.
    bool support_style_blocks_corridor() const;
    // El tipo de soporte que MANDA para este objeto (override del objeto sobre el preset). Lo
    // quieren dos sitios: el aviso de árbol y la siembra que lo corrige. s287-bis.
    SupportType effective_support_type() const;
    // Y la contrapartida: al abrir el gizmo, dejarlo en Snug para ESTE objeto. Un ajuste por
    // objeto, con snapshot, visible en la lista y borrable desde ella — nunca el preset global.
    void ensure_snug_style();
    // s287 — siembra en la config DEL OBJETO los ajustes de soporte que él tiene medidos e
    // impresos, y sólo las claves que el objeto no tuviera ya dichas. Devuelve si tocó algo.
    // Se llama desde create_pillar(), dentro de su snapshot: el pilar y los ajustes que lo hacen
    // imprimible se deshacen juntos.
    bool seed_support_defaults(ModelObject &mo);
    // NEOTKO_SUPPORTZONES_TAG s304 — el «sólo mis zonas» de s299c, sacado a funcion.
    //
    // 🔑 Lo piden DOS sitios: el interruptor de la seccion de zonas y el aviso del cajon, que
    // ofrece el mismo cambio de un clic en el momento en el que la condicion se da. Un `set_key_value`
    // duplicado en dos sitios es un sitio donde se desincronizan, y esta clave tiene familia
    // (arbol contra normal) que hay que respetar en los dos.
    void apply_only_my_zones(bool only_mine);
    // Sólo para que el panel sepa si el Snug de ahora lo puso él y pueda decirlo.
    bool m_forced_snug = false;

    // ------------------------------------------------------------------------
    // Zone management
    // ------------------------------------------------------------------------
    // "Make one and then duplicate it" was the owner's own framing (s286). Selection-level
    // copy/paste already handles a support enforcer with no type gate, so what is missing is not
    // the machinery, it is having it where the zones are listed.
    void delete_selected_zone();
    // ⛔ Hubo un "repetir a lo largo del parche" (N pilares equiespaciados sobre el eje medido por
    // PCA) y él lo QUITÓ tras probarlo, con razón: en una superficie curva las copias caen donde el
    // parche ya no está bajo ellas, así que dejan de tocar, se cruzan entre sí y el resultado es un
    // amasijo booleano. La precisión de verdad la da recortar la huella y colocar a mano.
    // No reintroducir sin resolver antes que cada copia siga tocando SU trozo de superficie.
    void duplicate_selected_zone();

    GLModel m_reach_model;         // the strip, drawn flat at the landing height (world coords)
    // 🔑 s286b, pedido por él: "cuesta clavar dónde caerá y toca girar la cama". La silueta del PIE
    // dibujada llena en el plano del aterrizaje, más densa que la franja, resuelve eso sin girar
    // nada — es la sombra real del pilar, no una ayuda aproximada.
    GLModel m_footprint_model;
    bool    m_footprint_model_dirty = true;
    void    update_footprint_model();
    double  m_reach_model_z = 1e9; // z it was built for
    double  m_reach_model_r = -1.; // radius it was built for
    Vec2d   m_reach_model_c { Vec2d::Zero() };
    void    update_reach_model();
    void    render_reach();
    // Adds the pillar as a SUPPORT_ENFORCER volume of the current object.
    void create_pillar();
    // s288 — la mitad de `create_pillar()` que NO crea el volumen: mete la malla en espacio del
    // objeto, la centra, coloca el `offset`, y escribe el gesto. Partido para que editar una zona
    // ya existente sea exactamente lo mismo sobre un volumen que ya está.
    void write_pillar_into(ModelVolume &v, TriangleMesh &&world_mesh, const Transform3d &inst,
                           const ZoneGesture &g);
    // El gesto tal y como está AHORA en el panel, en espacio del objeto.
    ZoneGesture current_gesture(const Transform3d &inst) const;
    // s288 — le quita el candado a la zona seleccionada: borra el gesto y con él la posibilidad de
    // editarla desde aquí. Camino de una sola dirección, con aviso, y con snapshot.
    void unlock_selected_zone();

    // ------------------------------------------------------------------------
    // s288 F2 — editar una zona bloqueada
    // ------------------------------------------------------------------------
    // 🔑 Se entra a editar por un botón EXPLÍCITO de la tarjeta, no por seleccionarla. Seleccionar
    // ya significa "enséñamela encendida en el 3D", y si además cargara el gesto se llevaría por
    // delante un pilar nuevo a medio construir. Un panel que hace dos cosas necesita que se vea
    // cuál está haciendo, y que se entre a la segunda a propósito.
    //
    // Se guarda el índice de VOLUMEN y no el de fila: la lista se reconstruye sola y las filas se
    // renumeran, los volúmenes no. Misma razón que `m_select_volume_idx_pending`.
    int  m_editing_volume_idx = -1;
    // La superficie que el gesto describía ya no está donde estaba (malla reparada, objeto
    // cambiado). Se dice y se sale, nunca se reconstruye el pilar en otro sitio.
    bool m_edit_lost_surface = false;

    bool editing() const { return m_editing_volume_idx >= 0; }
    // Carga el gesto de la fila en los campos del panel. false = no se pudo (ver §5 del plan).
    bool begin_edit_zone(int row_idx);
    // Reescribe la malla del volumen que se está editando con lo que hay ahora en el panel.
    void apply_edit();
    void end_edit();
    // Refill m_candidates from the ray under `mouse_pos`, keeping the wheel's choice if it still
    // points at the same surface.
    void update_candidates(const Vec2d &mouse_pos);
    const Candidate *live_candidate() const;
    // Connected coplanar patch grown from `facet_idx`, lifted along each facet's own normal so it
    // does not z-fight with the surface it describes.
    void build_face_model(GLModel &model, int facet_idx, const Vec2d &centre_xy, const ColorRGBA &col);
    // s289 — el LIENZO: toda la superficie mirando hacia abajo conectada con la semilla, dibujada
    // flojita detrás de lo pintado. Es el "hasta aquí puedes" que faltaba.
    // ⛔ s299b — aquí vivían `m_paint_area_model` y `build_paint_area_model()`, el lienzo del
    // pincel. Se han quitado: el lienzo enseñaba hasta dónde dejaba pintar el flood-fill, y el
    // pincel ya no tiene flood-fill. Lo que pintas es la huella.
    void render_pick_overlays();

    // ------------------------------------------------------------------------
    // s289 — el volcado de geometría (NeoDebug::SUPPORTZONES)
    // ------------------------------------------------------------------------
    // 🔑 Existe para no tener que exportar un 3mf cada vez que una huella sale rara: escribe en el
    // canal todo lo que decidió la forma (parche, recorte, borde, inglete, anillos, rodilla) y
    // deja el sólido en un `.obj` que se abre en cualquier visor.
    //
    // Se dispara solo al crear y al editar una zona, y a mano desde el botón del panel — que sólo
    // aparece con el canal encendido, para no meter un botón de depuración en la UI de nadie.
    void dump_geometry(const char *why, const TriangleMesh *solid) const;
    static void dump_obj(const TriangleMesh &m, const std::string &path);

    void render_panel_body();
    // ------------------------------------------------------------------------
    // El panel, s287 (docs/WIP/SUPPORT_ZONES_s286_DEBUG_PLAN.md §5)
    // ------------------------------------------------------------------------
    // 🔑 El alzado: la SECCIÓN del pilar por el plano de la inclinación, a escala uniforme, con la
    // cuña del corredor del motor detrás. Es la pieza que convierte el panel en algo que se mira en
    // vez de leerse, y todo lo que dibuja se lo pide a quien ya tiene el número — no calcula
    // geometría propia, así que no puede discrepar del sólido que se va a construir.
    void render_elevation();
    // La lista de zonas como tarjetas: dibujo del pilar, color del filamento de techo y medidor de
    // capturas. La identidad visual que el dueño echaba en falta cuando eran filas de texto.
    void render_zone_cards();
    // Los tres modos de mirar (voladizos, mapa de huecos, pieza fantasma) como interruptores de
    // icono, con sus deslizadores debajo sólo cuando están encendidos.
    void render_view_strip();
    // Todos los avisos juntos y al final. Repartidos por el panel, cada uno que aparecía desplazaba
    // la columna de controles que estabas usando.
    void render_issue_tray();
    // Los materiales de la zona seleccionada. En su propia función porque el cuerpo del panel ya es
    // largo y porque este bloque tiene salidas tempranas: con un solo filamento no se dibuja nada.
    void render_zone_materials();
    // ⛔ Hubo aquí un par (masa, techo) con casilla de "el techo lleva el suyo". Se quitó tras
    // probarlo: fijarle herramienta al CUERPO del soporte le quita a Orca su vertedero de purga
    // (`is_support_overriddable` exige filamento 0) y hace crecer la torre, a cambio de controlar la
    // parte del soporte a la que menos le importa el material. Sólo se ofrece el TECHO.

    // ------------------------------------------------------------------------
    // See-through
    // ------------------------------------------------------------------------
    // s286, owner's call after using the first build: the clipping plane the painters ship is the
    // wrong tool here. It cuts, so you keep dialling it to reach one surface and then dialling it
    // back for the next, and it hides as much as it reveals. What this tool actually needs is to
    // see EVERYTHING at once — the underside of a handle, the shelf below it, the overhang below
    // that — without hiding anything or touching them one by one.
    //
    // So: ghost the whole part while the gizmo is open. Lowering a GLVolume's alpha is enough,
    // because volumes_to_render() buckets by render_color.is_transparent() live (3DScene.cpp),
    // so a ghosted volume moves itself into the transparent pass. Same trick GLGizmoAlignStack
    // already uses for its face pick; this is its second user, which is the only honest reason to
    // repeat a pattern.
    //
    // ⛔ s299c — aquí vivían `m_see_through` y su opacidad. Se han quitado con la opción: la pieza
    // translúcida bajo un bloque que YA es translúcido no enseñaba el interior, enseñaba dos
    // transparencias peleándose sin profundidad fiable. Sobrevive `m_saved_colors`, que ahora sólo
    // lo usa el resaltado de la zona seleccionada.

    // Original colours, keyed by a stable (object_idx, volume_idx, instance_idx) so we can restore
    // by matching live volumes. Safe across selection changes AND volume rebuilds: a stale id
    // simply finds no match instead of dangling.
    std::map<std::tuple<int, int, int>, ColorRGBA> m_saved_colors;

    // 🚨 Restaura SIEMPRE antes de teñir. Guardar un color ya teñido como "el original" es como un
    // tinte se acumula hasta que la escena se apaga del todo en unos cuantos refrescos.
    // El nombre se conserva porque es por donde pasan todos los sitios que refrescan el color.
    void apply_see_through();
    // Enciende en el 3D la zona seleccionada en la lista.
    void highlight_selected_zone();
    void restore_see_through();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSupportZones_hpp_
// NEOTKO_SUPPORTZONES_TAG_END
