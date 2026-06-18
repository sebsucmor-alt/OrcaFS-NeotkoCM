// NEOTKO_COLORSTITCH_TAG_START — puente para abrir el editor avanzado de patrón
// (ColorMixPatternDialog, definido file-private en Tab.cpp) desde OTRAS TUs, en
// particular el 3D Painter gizmo (GLGizmoColorMixPainter). El gizmo no puede ver
// la clase del diálogo, así que expone una única función libre. La clave: el
// diálogo TOMA un patrón de entrada y DEVUELVE un patrón — justo "edita el patrón
// de este pase". El llamador del painter consume SOLO el patrón (+ ángulo) y NO
// reescribe los demás knobs (gamma/easing/reps…) a DynamicPrintConfig, para que
// varios ColorMix a distintos Z no se contaminen entre sí.
#ifndef slic3r_GUI_ColorStitchPatternLauncher_hpp_
#define slic3r_GUI_ColorStitchPatternLauncher_hpp_

#include <map>
#include <string>
#include <vector>

class wxWindow;

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {

// Abre ColorMixPatternDialog modal para editar el ColorStitch de UN pase.
//   penu    : false = superficie Top, true = Penultimate (surface_id del diálogo).
//   cfg     : config activo (el diálogo lo necesita para la sección gradiente y
//             para los datos de filamento; se trabaja sobre una COPIA, nunca se
//             ensucia el preset vivo).
//   cur_kv  : override ACTUAL del pase (pass.colormix.kv). Siembra el diálogo para
//             que re-editar reanude el diseño previo (vacío = arranca de la región).
//   out_kv  : en OK, recibe el payload COMPLETO del pase (string del patrón + TODOS
//             los knobs del gradiente role-prefijados: mode/pct/easing/gamma/bandas/
//             tools/angle…). Es lo que el motor lee per-pase (Fill.cpp FASE2), así
//             dos ColorMix en la misma zona difieren de verdad.
// Devuelve true si el usuario aceptó (OK); false si canceló (no tocar el pase).
bool open_colorstitch_pattern_dialog(
    wxWindow*                                 parent,
    const std::vector<std::string>&           fcolors,
    bool                                      penu,
    Slic3r::DynamicPrintConfig*               cfg,
    const std::map<std::string, std::string>& cur_kv,
    std::map<std::string, std::string>&       out_kv);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ColorStitchPatternLauncher_hpp_
// NEOTKO_COLORSTITCH_TAG_END
