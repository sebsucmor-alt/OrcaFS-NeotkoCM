// NEOTKO_NEOARACHNE_TAG v3-spine (s323)
// NeoArachne v3 — "tipo S3D": muros de ancho fijo + ESPINA.
//
// Reglas medidas en G-code de Simplify3D (lámina s3d_probe, s323):
//   1. Los muros son siempre de ancho fijo y sólo van donde caben enteros.
//   2. ANTES de cada muro: si lo que queda cabe ENTERO en una línea de ancho <= techo, no hay más
//      muros ahí; se imprime UNA línea por el eje con ancho = hueco local.
//   3. El ancho mínimo es un SUELO, no un corte: la punta de un hueco se imprime a ese ancho.
//   4. El ancho máximo es un TECHO: la línea nunca se parte en dos.
//   5. En los cruces las ramas se encadenan (Y de 3 ramas → 2 líneas).
// Y una propia (s323, S y `a` de Neotko): una zona cuyo hueco NUNCA llega al suelo no es un hueco,
// es una grieta entre dos pasadas del mismo muro → no se imprime (umbral propio: sliver_mm).
// Ver memory/project_neoarachne_vs_s3d_paths.md.
#ifndef slic3r_NeoArachneSpine_hpp_
#define slic3r_NeoArachneSpine_hpp_

#include <cstddef>
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"

namespace Slic3r {
class PerimeterGenerator;
namespace NeoArachne {

struct SpineParams {
    double floor_mm      = 0.2;   // suelo: un hueco más estrecho se imprime a este ancho
    double ceiling_mm    = 1.2;   // techo: si en algún punto el hueco es más ancho, no es espina
    double min_length_mm = 0.1;   // líneas más cortas se descartan
    // Grieta: zona o línea cuyo hueco nunca llega a este ancho → no se imprime (0 = off). Separado
    // del suelo a propósito (s323): con el mínimo al 10 % las grietas volvían a salir.
    double sliver_mm     = 0.14;
};

struct SpineStats {
    size_t components         = 0;  // zonas recibidas
    size_t spine_components   = 0;  // las que se han impreso como espina
    size_t wide_components    = 0;  // no caben en una línea (no deberían llegar aquí en v3)
    size_t slivers_skipped    = 0;  // grietas que no llegan al suelo, no impresas
    size_t polylines          = 0;  // líneas de espina emitidas
    size_t dropped_short      = 0;  // líneas descartadas por el largo mínimo
    size_t tiny_loops_dropped = 0;  // bucles cerrados más pequeños que su propio ancho
    size_t spurs_pruned       = 0;  // ramitas de punta podadas
    size_t junction_joins     = 0;  // uniones hechas en cruces
    double spine_len_mm       = 0.;
    double spine_area_mm2     = 0.;
    double wide_area_mm2      = 0.;
};

// Regla 2: true si en ningún punto de `ex` cabe un disco de diámetro `ceiling_scaled`.
bool fits_in_one_line(const ExPolygon& ex, double ceiling_scaled);

// Imprime como espina las zonas de `regions` que caben en una línea <= techo. Las rutas van a
// g.gap_fill (erGapFill). Devuelve las zonas consumidas; las que no dan ninguna línea (o no caben)
// se añaden a `rejected` para que el llamador las mande a relleno en vez de dejarlas vacías. Las
// grietas saltadas por sliver_mm NO se devuelven (tampoco las llenaría el relleno).
// `forced`: zonas que van a espina aunque no quepan bajo el techo (el ancho se recorta al techo);
// s323: las colas donde sólo cabría un minibucle. `out`: si no es nulo, las rutas van ahí (el
// llamador las coloca en el orden de los muros) en vez de a g.gap_fill.
ExPolygons run_spine(PerimeterGenerator& g, const ExPolygons& regions, const SpineParams& p, SpineStats* stats,
                     ExPolygons* rejected = nullptr, const ExPolygons* forced = nullptr,
                     ExtrusionEntitiesPtr* out = nullptr);

}} // namespace Slic3r::NeoArachne

#endif
