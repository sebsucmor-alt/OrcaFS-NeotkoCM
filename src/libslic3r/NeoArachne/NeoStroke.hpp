// NEOTKO_NEOSTROKE_TAG C1-C5 (s325)
// NeoStroke — muros interiores planificados POR TRAZOS.
//
// Plan: docs/FUTURE/NEOSTROKE_FASE_C_PREPLAN.md. Resultados y las 15 trampas de la fase P (el
// prototipo en Python, docs/TOOLS/strokes/): docs/FUTURE/NEOARACHNE_STROKES_FASE_P.md.
//
// Qué hace, en una frase: saca el esqueleto de la isla entera, decide UNA VEZ POR TRAZO cuántas
// líneas lleva, traza esas líneas siguiendo el trazo con el ancho variando de forma continua, y
// las cose en caminos largos. El muro exterior lo sigue poniendo Classic.
//
// Fases hechas aquí: C2 (las k líneas), C3 (puntas y remates), C4 (la costura), C5 (el residuo).
// C6 — caudal y los tres movimientos (patinaje, wipe dirigido, viaje) — NO está: toca el emisor
// de G-code y va en su propia sesión. Lo que sí hay es el ancla donde engancha: ver
// `NeoStrokeLink.hpp`.
#ifndef slic3r_NeoStroke_hpp_
#define slic3r_NeoStroke_hpp_

#include "NeoArachneConfig.hpp"

namespace Slic3r {
class PerimeterGenerator;
class PrintRegionConfig;
namespace NeoArachne {

// Muro exterior Classic + interior por trazos. Sustituye a run_classic_spine cuando
// neoarachne_inner_walls = NeoStroke.
void run_neostroke(PerimeterGenerator& g, const Config& cfg, const PrintRegionConfig* original_cfg);

}} // namespace Slic3r::NeoArachne

#endif
