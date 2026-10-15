// NEOTKO_GRAVITY_TAG s362 — las capas de un objeto que NO toca la cama empiezan en su propia base.
//
// Orca lamina cada objeto con una rejilla que nace en la cama (primera capa + saltos fijos) y no
// mira dónde empieza la pieza. Una pieza apoyada sobre otra arranca entonces donde le pille la
// rejilla: queda un hueco de hasta media capa bajo ella (y sale como puente), o se hunde en la de
// abajo. El apaño a mano era tocar la altura de capa con decimales (0,21 en vez de 0,2) hasta que
// una frontera cayera en la base; se rompía al mover o cambiar cualquier cosa.
//
// Esto lo hace solo y sin deformar la pieza: el tramo VACÍO entre la primera capa y la base se
// reparte en capas iguales que acaban exactas en la base, y desde ahí la pieza usa sus alturas de
// capa de siempre. Con «Precise Z height» además ajusta las últimas capas REALES para acabar en la
// altura de la pieza (el de Orca ajusta las 5 últimas de la rejilla, vacías incluidas, y en una
// pieza baja volvía a mover la base).
//
// Único llamador: generate_object_layers() (Slicing.cpp). Sólo actúa si
// SlicingParameters::neotko_floating_bottom_z > 0, que Print::apply() rellena únicamente con
// «Stacked objects: real layers» (s363), True Objects y sin raft.
#ifndef slic3r_GravityLayering_hpp_
#define slic3r_GravityLayering_hpp_

#include "libslic3r/libslic3r.h"

#include <vector>

namespace Slic3r {

struct SlicingParameters;

namespace Gravity {

// `layers` = pares (z de abajo, z de arriba) tal como los devuelve generate_object_layers().
// Devuelve true si el objeto es flotante y la serie se ha reescrito (el llamador NO debe pasar
// después el «Precise Z height» de Orca); false = no tocado.
bool land_layers_on_base(const SlicingParameters &slicing_params, std::vector<coordf_t> &layers,
                         bool precise_top);

// s363 — objeto que SÍ toca la cama y tiene otro apoyado en su techo
// (SlicingParameters::neotko_land_top): ajusta sus últimas capas, nunca la primera, para acabar en
// la altura real de la pieza. Devuelve true si la serie acaba en el techo.
bool land_layers_on_top(const SlicingParameters &slicing_params, std::vector<coordf_t> &layers);

} // namespace Gravity
} // namespace Slic3r

#endif // slic3r_GravityLayering_hpp_
