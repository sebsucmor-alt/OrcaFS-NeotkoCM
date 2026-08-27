// NEOTKO_SUPPORTZONES_TAG s284 — F1: what a support zone is actually going to catch.
//
// docs/FUTURE/SUPPORT_ZONES_PLAN.md §6 F1.
//
// A support enforcer block is opaque today: you draw it, and the only way to find out whether it
// will produce anything is to slice and look. The classic silent failure is a block that swallows a
// SOLID chunk of the object — it looks full, and it generates not a single gram.
//
// 🔑 So the thing worth showing is NOT "where is there object inside the block". It is "where is
// there DOWNWARD-FACING SURFACE inside the block", which is what actually predicts the outcome.
// This module answers exactly that question, once, and two features read the answer:
//   - the zone highlight (render),
//   - the sterile-zone warning (a zone whose answer is the empty set).
//
// Technique, per the plan: no booleans and no re-slicing. A grid of cells inside the block, one
// vertical ray per cell against the AABB tree the painters already use.
//   🔑 The grid step is the "Support Pillar Resolution" of the trick everybody knows, and it
//   already exists in the fork as `support_base_pattern_spacing`. The same number governs what you
//   see and what gets printed, which is the whole point of using it rather than inventing one.
//
// The "inside the block" test is the second ray: the same vertical line is cast against the block
// itself, giving the z intervals where that line is inside it. A downward-facing hit counts when
// its z falls in one of those intervals. That is exact for any block shape — a rotated box, a
// lofted corridor, an imported mesh — without a single boolean operation.
//
// 🚨 THE NORMAL FILTER STAYS STRICTLY IN THE RENDER. What needs support is still decided by
// detect_overhangs() (2D offset between layers, SupportMaterial.cpp). This module chooses what to
// draw and what to warn about; it never decides that support is needed. See plan §8 and
// PATENT_US9524357_ANALYSIS.md §5.2 — this distinction is not cosmetic.
//
// ⚠️ Known limitation of this phase: NEGATIVE_VOLUME parts are not subtracted, so a zone aimed at
// a surface that a negative volume carves away can read as lit when it is not. It is render-only
// and it errs on the optimistic side, which is the harmless direction for a warning that says
// "this zone WILL produce nothing".

#ifndef slic3r_SupportZoneProbe_hpp_
#define slic3r_SupportZoneProbe_hpp_

#include <cmath>
#include <vector>

#include "../../Point.hpp"

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace SupportZones {

// One sampled point of downward-facing object surface found inside a zone.
// Coordinates are in the OBJECT-LOCAL frame (the frame ModelVolume matrices live in), so the
// renderer can draw them under the object's own transform without knowing about instances.
struct LitSample
{
    Vec3f pos;
    // Facet normal of the surface that was hit; its Z is always < 0 here.
    // The render needs the whole vector, not just Z: a marker drawn flat on a steeply leaning
    // overhang pokes straight through the wall, and one drawn exactly ON the surface z-fights with
    // it. Both are fixed by building the marker in the plane of this normal and lifting it slightly
    // along it.
    Vec3f normal;
};

struct ZoneProbe
{
    std::vector<LitSample> lit;
    // Grid step actually used, in mm. Echoed back because it is clamped against the zone size.
    float  grid_step_mm { 0.f };
    // Cells whose vertical line passed through the zone at all. `lit.size()` against this is the
    // honest "how much of this block is doing anything" ratio.
    size_t cells_inside_zone { 0 };

    // 🔑 The whole point of F1: a zone that catches no downward-facing surface will not produce a
    // single gram of support, however full it looks on screen.
    bool sterile() const { return lit.empty(); }
};

// Probes one enforcer volume against the object's model parts.
// `grid_step_mm` is meant to be `support_base_pattern_spacing`; it is clamped so a huge spacing on
// a small block still gets a few samples, and so a tiny spacing on a large block cannot explode.
//
// ⚠️ This builds an AABB tree over the object's parts on every call. Callers that ask per frame
// must cache: the natural key is the object's mesh identity plus the volume transforms. Keeping
// the cache OUT of here is deliberate — the GUI knows when the model changed, this does not.
ZoneProbe probe_zone(const ModelObject &object, const ModelVolume &enforcer, float grid_step_mm);

// NEOTKO_SUPPORTZONES_TAG s286b — "esto se te ha quedado sin sujetar".
//
// La misma pregunta de F1, hecha al revés y sobre el objeto ENTERO en vez de dentro de un bloque:
// qué superficie mirando hacia abajo ya está cogida por alguna zona, y cuál no la coge nadie.
//
// 🔑 Idea del dueño, y es el inverso de Simplify3D: ellos rellenan de soporte por todas partes;
// aquí se enseña el hueco y decide el usuario.
//
// 🚨 FRONTERA DEL §8, y no es cosmética. Esto es RENDER Y SÓLO RENDER. Lo que NECESITA soporte lo
// sigue decidiendo `detect_overhangs()` (offset 2D entre capas, en el motor). El plan lo dice dos
// veces: el iluminado de F1 no puede convertirse en el criterio de soporte, y el filtro por normal
// elige destino, no decide necesidad. Si este mapa pasara a decidir dónde se genera soporte,
// caeríamos dentro de la reivindicación 1 de US 9,524,357 sin querer. En la UI se dice
// **"might need support"**, nunca "needs support".
struct CoverageProbe
{
    // Mirando hacia abajo y DENTRO de alguna zona: ya está cogido.
    std::vector<LitSample> covered;
    // Mirando hacia abajo, pasa el umbral, y no cae dentro de ninguna zona.
    std::vector<LitSample> uncovered;
    float  grid_step_mm { 0.f };
    size_t cells { 0 };
};

// `max_normal_z` es el corte del umbral: cuenta la superficie cuya normal tiene z <= ese valor.
// Se pasa desde fuera a propósito, para que el mapa y el sombreado de voladizos del canvas usen el
// MISMO número y no puedan discrepar en pantalla.
CoverageProbe probe_object_coverage(const ModelObject &object, float grid_step_mm, float max_normal_z);

// Convenience for the warning path: every SUPPORT_ENFORCER volume of the object that catches
// nothing. Returns pointers into `object.volumes`, in list order.
std::vector<const ModelVolume*> sterile_zones(const ModelObject &object, float grid_step_mm);

// NEOTKO_SUPPORTZONES_TAG s286 — the brake of §3, in one place.
//
// 🔑 This lives here and not in SupportMaterial.cpp because BOTH sides need it and they must not
// be allowed to drift: the support generator uses it to clamp how far a column may step sideways
// per layer, and the gizmo uses it to paint the strip you are allowed to land in. If the editor
// and the engine disagreed about this number, the tool would draw links it cannot deliver, which
// is the one thing §1 promises never to do.
//
// The `k` in d_max = k * line_width. §3 wants it as a hidden comDevelop config key one day so it
// can be calibrated against real prints; until that key exists this is its single owner.
constexpr double SUPPORT_CORRIDOR_K = 0.75;

// How far a column may move sideways in ONE layer, in mm. Note it does NOT depend on the layer
// height: it is a per-layer allowance, so under adaptive layer height the resulting angle adapts
// on its own and the clamp stays the right one (measured in s284).
inline double corridor_step_mm(double support_line_width_mm)
{
    return SUPPORT_CORRIDOR_K * support_line_width_mm;
}

// El ángulo más inclinado que el corredor puede seguir, en grados, para una altura de capa dada.
//
// 🔑 s286b — el pilar con rodilla. El freno es un tope al desplazamiento POR CAPA, así que un pilar
// que se inclina a un ángulo fijo pide exactamente `dz · tan(ángulo)` en cada capa: constante, y
// comparable con el tope de un vistazo. Invertir esa relación da el techo del ángulo, y por eso
// vive aquí y no en el gizmo — es el mismo motivo por el que `SUPPORT_CORRIDOR_K` está en este
// fichero: dos consumidores que no pueden discrepar. Si el editor dejara elegir un grado más de lo
// que el motor puede seguir, la columna se quedaría atrás del dibujo.
//
// 🚨 Bajo ALH hay que llamarla con la capa MÁS GRUESA, no con la nominal: el tope es por capa, así
// que una capa gorda con el mismo ángulo pide más desplazamiento y es la que manda.
inline double corridor_max_angle_deg(double layer_height_mm, double support_line_width_mm)
{
    if (layer_height_mm <= 0. || support_line_width_mm <= 0.)
        return 0.;
    // Sin M_PI: no es estándar en MSVC sin _USE_MATH_DEFINES y este header lo incluye todo el mundo.
    constexpr double rad2deg = 57.295779513082320876798154814105;
    return std::atan(corridor_step_mm(support_line_width_mm) / layer_height_mm) * rad2deg;
}

// Total lateral reach gained while descending `height_mm`, in mm. This is the radius of the strip
// on the bed (or on a shelf) from which a given surface can actually be reached.
double corridor_reach_mm(double height_mm, double layer_height_mm, double support_line_width_mm);

} // namespace SupportZones
} // namespace Slic3r

#endif // slic3r_SupportZoneProbe_hpp_
