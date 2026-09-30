// NEOTKO_NEOSTROKE_TAG s342 — AJUSTES POR ISLA (plan: docs/WIP/NEOSTROKE_AJUSTES_POR_ISLA_PLAN.md, camino B).
//
// Cada ajuste por isla es un ANCLA: un punto dentro de la isla + los ajustes de NeoStroke que cambia. En cada capa,
// la isla que CONTIENE el punto usa esos ajustes (el mismo criterio «por PUNTO, nunca por índice» del selector de
// islas del visor). Viaja en la clave de texto `neostroke_island_overrides`, una isla por línea:
//
//     nombre|x|y|clave=valor;clave=valor
//
// x, y en mm, en COORDENADAS DEL OBJETO (las del ModelObject, sin la instancia). El motor las pasa al marco de
// laminado con `trafo_centered()`.
//
// 🚨 Fichero LIGERO a propósito (sólo la std): lo pueden llamar PrintConfig/Preset/GUI sin arrastrar el motor
//    (lección `printconfig_link_drags_heavy_objects`).
#ifndef slic3r_NeoStrokeIslands_hpp_
#define slic3r_NeoStrokeIslands_hpp_

#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace NeoArachne {

struct NsIslandOverride {
    std::string                                      name;
    double                                           x = 0.;
    double                                           y = 0.;
    std::vector<std::pair<std::string, std::string>> values;   // clave de config → valor en texto, en orden

    // nullptr si la isla no cambia esa clave.
    const std::string* find(const std::string& key) const;
    void               set(const std::string& key, const std::string& value);   // añade o pisa
    void               erase(const std::string& key);
};

// Claves que se pueden cambiar por isla. Todo lo demás es del OBJETO (el patín, el orden entre capas, la
// referencia de anchos y la pared Classic) y el lector lo descarta.
const std::vector<std::string>& island_override_keys();
bool                            is_island_override_key(const std::string& key);

// Tolerante: líneas vacías, mal formadas o claves fuera de la lista se saltan. `skipped` (opcional) cuenta lo
// descartado, para avisar en la UI o en la sonda.
std::vector<NsIslandOverride> parse_island_overrides(const std::string& text, size_t* skipped = nullptr);
// Inverso exacto de `parse_island_overrides` para lo que éste acepta. Los nombres pierden `|`, `;`, `=` y saltos
// de línea (no hay escape: son nombres de letra, no texto libre).
std::string                   write_island_overrides(const std::vector<NsIslandOverride>& list);

}} // namespace Slic3r::NeoArachne

#endif
