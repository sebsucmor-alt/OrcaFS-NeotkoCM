// NEOTKO_NEOSTROKE_TAG C6-anchor (s325)
// El ancla del patinaje. C6 se engancha AQUÍ y en ningún otro sitio.
//
// Por qué existe antes que C6: el patinaje no es un filtro que se pone al final, es una decisión
// sobre CÓMO se va de un camino al siguiente, y quien la toma necesita cosas que sólo existen
// dentro del planificador — qué material hay ya puesto en esta isla, por dónde pasó el último
// cordón, y cuál es el arranque del siguiente. Si eso no se guarda mientras se planifica, C6
// tiene que recalcularlo entero. Así que el planificador lo monta y se lo ofrece a quien quiera
// decidir; hoy nadie decide y todo sale como un viaje normal.
//
// Los tres movimientos que C6 tiene que devolver (fase P, §2.6 del pre-plan):
//   1. patinaje            — se llega por encima del cordón: sin extruir y SIN retraer.
//   2. wipe + patín dirigido — hay trecho: barrido con la retracción y después deslizar HACIA el
//                              destino, sobre material. Ninguno de los cuatro slicers del prior
//                              art hace esto (§9 del pre-plan).
//   3. viaje               — no hay cordón debajo: retracción y salto por el aire.
//
// 🚨 Y la pregunta de diseño que hay que contestar ANTES de escribir C6 (§9): si el patinaje se
// decide aquí, es «una regla que permite patinar donde el plan ya estaba hecho». Lo que Neotko
// describe del combing de S3D es lo contrario: el patinaje CAE SOLO de planificar buscando
// continuidad. Este ancla admite las dos — la segunda usaría `covered` y `last_bead` para
// reordenar, no sólo para clasificar — pero no las da por decididas.
#ifndef slic3r_NeoStrokeLink_hpp_
#define slic3r_NeoStrokeLink_hpp_

#include <functional>
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"

namespace Slic3r { namespace NeoArachne {

struct LinkContext {
    const ExPolygon*  island    = nullptr;  // la isla que se está imprimiendo
    const ExPolygons* covered   = nullptr;  // material YA puesto en esta isla (cordón engordado)
    const Polyline*   last_bead = nullptr;  // el camino que se acaba de imprimir, en orden
    Point             from;                 // dónde está la boquilla
    Point             to;                   // dónde arranca el siguiente camino
    double            nozzle_mm  = 0.4;
    double            outer_w_mm = 0.42;
};

// El recorrido SIN extruir que lleva de `from` a `to`. Vacío = viaje normal (lo de hoy).
// C6 devolverá aquí el patinaje o el wipe dirigido, y tendrá además que enseñarle a
// GCodeGenerator a emitir un movimiento sin extrusión y sin retracción: eso es la otra mitad del
// trabajo y es la parte con más riesgo del plan.
using LinkPlanner = std::function<Polyline(const LinkContext&)>;

// Nulo mientras C6 no exista. Se consulta una vez por salto entre caminos.
const LinkPlanner& link_planner();
void set_link_planner(LinkPlanner p);

}} // namespace Slic3r::NeoArachne

#endif
