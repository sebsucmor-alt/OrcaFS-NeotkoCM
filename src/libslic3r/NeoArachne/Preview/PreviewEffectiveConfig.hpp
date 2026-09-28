// NEOTKO_NEOSTROKE_TAG s337 — LA CONFIG DEL OBJETO, montada igual que el laminado de verdad.
//
// El visor de s335 sacaba su config de la PESTAÑA desde la que se abría (`live_merged_config(tab)`):
// abierto desde la pestaña global de Proceso dibujaba cualquier objeto con los ajustes GLOBALES, y en
// las placas de prueba (TEST18-21, todas con ajustes por objeto) enseñaba otra cosa que el G-code.
// Aquí se compone como lo hace `PrintApply.cpp`:
//     config completa (con las claves ESPEJO de `Plater::neotko_full_config()`)
//       → `PrintObject::object_config_from_model_object`      (ajustes del objeto)
//       → `region_config_from_model_volume`                    (objeto + volumen + rango de capas)
// 🚨 La config de entrada TIENE que ser la de `neotko_full_config()`, no la de `full_config()` en
//    crudo: sin las claves espejo el visor vuelve a no ver lo que ve el laminado (regla de s335).
// ⚠️ Lo que NO reproduce: los volúmenes MODIFICADORES (cambian la config de un trozo de la pieza) y la
//    altura de la primera capa en los anchos de la capa 1. El gizmo avisa cuando un objeto tiene
//    modificadores.
#ifndef slic3r_NeoArachne_Preview_PreviewEffectiveConfig_hpp_
#define slic3r_NeoArachne_Preview_PreviewEffectiveConfig_hpp_

#include "PreviewConfigSnapshot.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
class FullPrintConfig;
class ModelObject;
class ModelVolume;
namespace NeoArachne { namespace Preview {

// Las capas de un objeto, las MISMAS que saca el laminado: `generate_object_layers()` sobre el perfil de
// alturas del objeto (altura variable incluida). Pares (abajo, arriba) relativos al objeto y sin balsa;
// el `;Z:` del G-code es `arriba + print_z_offset`.
struct ObjectLayerGrid {
    std::vector<std::pair<double, double>> layers;
    double print_z_offset = 0.0;
    bool   valid() const { return !layers.empty(); }
    // Índice (0-based) de la capa cuyo intervalo contiene z (relativa al objeto); -1 si ninguna.
    int    layer_at(double z_rel) const;
};

ObjectLayerGrid object_layer_grid(const DynamicPrintConfig& full, const ModelObject& mo);

// Config efectiva de un volumen PIEZA en la capa `layer_idx` de `grid`. `layer_height`, el número de capa y
// las marcas de primera/última capa salen de la rejilla; los flujos, de `compute_flows()`.
ConfigSnapshot snapshot_for_volume(const DynamicPrintConfig& full, const ModelObject& mo, const ModelVolume& mv,
                                   const ObjectLayerGrid& grid, int layer_idx);
// Lo mismo con la config completa ya pasada a FullPrintConfig: montarla cuesta, y el gizmo la reutiliza
// para todos los volúmenes y capas de una pasada.
ConfigSnapshot snapshot_for_volume(const FullPrintConfig& fullc, const ModelObject& mo, const ModelVolume& mv,
                                   const ObjectLayerGrid& grid, int layer_idx);

// Los cinco flujos que necesita el generador de perímetros, deducidos de la config y de `layer_height`.
// Es el mismo cálculo que hacía el panel de s335 (`capture_snapshot_from_tab`), ahora en un solo sitio.
void compute_flows(ConfigSnapshot& snap);

// Huella de todo lo que puede cambiar el dibujo de un volumen: las configs de región y objeto enteras,
// los diámetros, la altura de capa y el candado de NeoStroke. Sirve para no relaminar si nada cambió.
// 🚨 Configs ENTERAS a propósito: una lista de claves "relevantes" se queda vieja en cuanto aparece un
//    mando nuevo (le pasó a la del panel viejo, que no lleva ni una `neostroke_*`).
size_t hash_snapshot(const ConfigSnapshot& snap);

// true si este volumen lamina con NeoStroke DE VERDAD: `wall_generator = NeoStroke` y el candado (canal
// NEOSTROKE) abierto. Con el candado cerrado el motor cae a la ruta normal y lo que sale no es NeoStroke.
bool neostroke_active(const ConfigSnapshot& snap);

// El ancho al que se refieren los % de NeoStroke (`neostroke_width_ref`, 0 = el del relleno macizo), en mm.
// Es la misma cuenta que `NeoStroke.cpp` (P.w_ref) sin los env var.
double neostroke_width_ref_mm(const ConfigSnapshot& snap);

}}} // namespace Slic3r::NeoArachne::Preview

#endif
