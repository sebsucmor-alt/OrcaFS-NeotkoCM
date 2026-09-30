// NEOTKO_NEOSTROKE_TAG s338 — NeoStroke por CAMPO.
//
// Port de `docs/TOOLS/strokes/campo/` (campo2.py + caminos.py). El prototipo manda: si algo aquí no da los
// mismos números que él sobre la misma isla, el roto es esto.
//
// Por qué existe: el planificador de TRAZOS decide rama a rama (una rama del esqueleto → k carriles) y lo
// que queda entre dos ramas no es de nadie. De ahí los huecos que van y vienen con la geometría (la P de
// PLAGE, la última «a»), y los parches (ganchos, residuo, bolsitas). Aquí cada SECCIÓN se reparte de golpe:
//   · los carriles son CURVAS DE NIVEL de la distancia al muro, así que en una junta doblan solos;
//   · cada carril lleva un PESO y los pesos de una sección suman su ancho: sin huecos ni doble pasada, y un
//     carril que entra o sale se AFILA entre sus vecinos (la idea de la k v2, que salió bien en el TEST19);
//   · el nº de cordones se decide SOBRE EL EJE, por tramos, con histéresis (sin colas sueltas);
//   · el carril central va por el eje y mide lo que queda libre (en un cruce de tres brazos se ensancha solo);
//   · la costura es un GRAFO: tramos mínimos = impares/2 por componente (camino de Euler).
// TEST23 Z0.68 con el muro de Classic real, hoy → campo: huecos BEACH 0.195 → 0.074 · PLAGE 0.417 → 0.019 ·
// Gyp Sea 0.152 → 0.050 mm²; ninguno donde quepa algo. BEACH frente a Simplify3D: 0.074 contra 0.422.
#ifndef slic3r_NeoStrokeField_hpp_
#define slic3r_NeoStrokeField_hpp_

#include <cstddef>
#include <vector>

#include "../ExPolygon.hpp"
#include "../Point.hpp"

namespace Slic3r { namespace NeoArachne {

// Todos los anchos en SEPARACIÓN (lo que el cordón cubre de verdad), no en `;WIDTH`. Es lo mismo que `w` en todo
// NeoStroke: el conversor de Orca (`VariableWidth.cpp:178`) suma h(1−π/4) él solo al escribir el `;WIDTH`.
struct FieldParams {
    double outer_w  = 0.286;  // banda del muro de Classic (ya con el solape contra el muro descontado)
    double tgt      = 0.35;   // cordón objetivo: el nº de cordones sale de redondear ancho / tgt
    double wmin     = 0.24;   // cordón más fino que se planifica (un tramo no baja de aquí si puede)
    double wmax     = 0.48;   // cordón más gordo
    double hyst     = 1.5;    // mm: un tramo de eje con otro nº de cordones más corto que esto se funde
    double tol_max  = 1.10;   // al fundir, cuánto se puede pasar de `wmax`
    double prune    = 2.5;    // poda: ramita más corta que esto × radio del cruce...
    double punta    = 0.6;    // ...y que MUERE en una esquina (radio en la punta < esto × radio del cruce)
    double snap     = 0.35;   // meseta: fuera de las rampas el nº de cordones queda entero
    double sig_w    = 0.15;   // mm: suavizado del ancho local
    double sig_n    = 0.25;   // mm: suavizado del nº de cordones (largo de las rampas)
    double cell     = 0.01;   // mm: rejilla (sube sola en islas grandes)
    // 🚨 s338 — la frontera de siempre (*Widest shape handled*): una isla con alguna sección más ancha que esto es
    //    una PIEZA, no una letra. El campo no la toca y la hace el planificador viejo, que deja lo ancho al relleno.
    double max_stroke_w = 5.0;
    // 🏁 s340 — MODO BANDA (`neostroke_band_mm`): > 0 = la zona útil se queda en esta franja junto al muro y el centro
    //    va al relleno normal (paso 4 de NeoStroke.cpp, que ya cede todo lo no cubierto). 0 = auto, como siempre.
    double band = 0.0;
    // 🚨 s338 — paso de los carriles. Las curvas de nivel salen con un punto por CELDA (0.01 mm): el remate
    //    (`close_gaps`) mira cada punto y une cada segmento, y en el primer laminado se comió el 90 % del tiempo
    //    (209 de 226 ms en una letra, 100-170 s en una isla grande). El motor viejo trabaja a 0.15.
    double step = 0.10;
    // 🚨 s338 (auditoría) — ~12 vectores de double por celda y un hilo por capa: 1.5 M celdas ≈ 150 MB por hilo en
    //    el peor caso (una isla de ~24×24 mm a 0.02). Más grande → planificador viejo. Una letra típica: ~30 MB.
    size_t max_cells = 1500000;
    // costura
    double cola_min = 0.15;   // puntas de carril más finas que esto se recortan
    double junta_k  = 1.8;    // un conector extruido mide como mucho esto × el ancho
    double doble    = 0.03;   // mm² que un conector puede pisar de lo ya puesto
};

// Un carril o un tramo: polilínea en mm con su SEPARACIÓN punto a punto.
struct FieldLane {
    std::vector<Vec2d>  pts;
    std::vector<double> w;
    bool                closed = false;
};

struct FieldStats {
    size_t cells = 0, pair_lanes = 0, center_lanes = 0, pruned = 0;
    size_t joints = 0, splices = 0, tramos = 0, tails_cut = 0;
    size_t wide_starts = 0;   // s338 — tramos girados para arrancar por su lado gordo
    size_t junction_ends = 0; // s339 — tramos girados para ACABAR en un cruce (`neostroke_end_at_junctions`)
    bool   skipped = false;   // isla demasiado grande para la rejilla: el que llama usa el planificador viejo
    // tiempos por fase, en ms (s338): para saber qué frena si el laminado va lento
    double t_eje = 0., t_campo = 0., t_carriles = 0., t_remate = 0., t_costura = 0.;
};

// 1. Carriles del campo de una isla.
std::vector<FieldLane> field_lanes(const ExPolygon& island, const FieldParams& p, FieldStats& st);
// 1b. Remuestreo de los carriles a `step` mm (el ancho se interpola). Los anillos siguen cerrados.
std::vector<FieldLane> field_resample(const std::vector<FieldLane>& lanes, double step);
// 2. Colas: fuera las puntas más finas que `cola_min`.
std::vector<FieldLane> field_trim_tails(const std::vector<FieldLane>& lanes, double cola_min, FieldStats& st);
// 3. Costura por grafo: carriles → tramos continuos (cada uno, un camino sin cortar el flujo).
std::vector<FieldLane> field_stitch(const ExPolygon& island, const std::vector<FieldLane>& lanes,
                                    const FieldParams& p, FieldStats& st);

}} // namespace Slic3r::NeoArachne

#endif
