// Receta por PROFUNDIDAD (Depth recipe) del Sticker, F1+F2.
// Receta por PROFUNDIDAD: bajo la huella de un color del sticker, capas enteras de los
// filamentos de color apiladas sobre la base, tantas como hagan falta (hasta un máximo en mm).
//
// Modelo = RealView Ray SIN difusión (decidido por Sebas el 22-oct-2026; REALCOLOR_LESSONS_FROM_REALVIEW.md
// §7.3 con D = 0): TD por canal con el `w` de RealView y composición de ColorSci::blend_stacked.
// Puro (ColorSci + std).
#ifndef slic3r_ColorSci_DepthStack_hpp_
#define slic3r_ColorSci_DepthStack_hpp_

#include <array>
#include <string>
#include <vector>

namespace Slic3r {
namespace DepthStack {

// F1 — TD por canal a partir del TD de un número (mm, el de Snapmaker/HueForge) y del color sRGB [0..1].
// Cada canal se apaga según lo que el filamento NO refleja; la media de los tres sigue cumpliendo el TD.
//   k = −ln(max(lineal, 0.02)) + 0.05;  w = 0.15 + 0.85·k/media(k);  td_c = td / w
// (RealView: t = 10^(−grosor/td·w) = 0.1^(grosor/td_c)). td <= 0 → opaco (0,0,0).
std::array<float, 3> td_per_channel(float td_mm, const std::array<float, 3>& rgb);

struct Filament {
    int                  tool  = 0;     // 0-based
    std::array<float, 3> rgb { 1.f, 1.f, 1.f };  // sRGB [0..1]
    float                td_mm = 0.f;   // TD de un número, en mm
};

// Bloque de capas enteras de un filamento. La receta va de ABAJO a ARRIBA.
struct Block {
    int tool   = 0;
    int layers = 0;
};

struct Options {
    float layer_mm     = 0.2f;   // altura de capa DEL OBJETO
    float max_depth_mm = 5.f;    // máximo del usuario (tests/pruebas); el motor lo recorta a lo disponible
    float stop_de      = 2.f;    // ΔE2000 que se da por bueno: para en la profundidad mínima que lo cumple
    bool  fixed_order  = true;   // true: `colours` en su orden (abajo→arriba) para TODOS los colores del
                                 // sticker → pocos filamentos por capa. false: prueba todos los órdenes.
    bool  lupa         = false;  // 7-nov-2026: color con predict_lupa (luz que entra y rebota) en vez de predict
    float lupa_force   = 1.f;    // cuánto apaga el color (×K/S). 10-nov-2026: 1,0 con S al canal más claro (antes 1,2 con la media)
};

struct Result {
    std::vector<Block>   blocks;          // abajo → arriba; vacía = la base sola
    std::array<float, 3> rgb { 0.f, 0.f, 0.f };  // color predicho, sRGB
    float                delta_e  = 1e9f;
    float                depth_mm = 0.f;  // capas usadas × layer_mm
    std::string          warning;         // "" o el motivo por el que no se debería ofrecer
};

// Color de una receta: `base` (opaca, el objeto) y encima los bloques.
std::array<float, 3> predict(const std::vector<Block>& blocks,
                             const std::vector<Filament>& colours,
                             const Filament& base,
                             float layer_mm);

// 10-nov-2026 — EL material «pigmento», una sola cuenta para todo el fork (RealColor «medio KM», Depth recipe
// «Lupa»; la Lupa 3D y RealView Physical hacen lo mismo con su propia matemática, ver
// docs/WIP/REALCOLOR_PIGMENTO_10NOV/MODELO_PIGMENTO_10NOV.md). La base del plástico ESPARCE igual en los tres
// canales (S, por mm) y el pigmento ABSORBE por canal (K/S, del color en grueso). S se calibra para que el
// canal MÁS CLARO deje pasar el 10 % a través del TD (antes era la media de los tres canales).
struct KMMedium {
    std::array<float, 3> ks   { 0.f, 0.f, 0.f };   // K/S por canal
    std::array<float, 3> rinf { 1.f, 1.f, 1.f };   // reflexión de un bloque grueso por canal (lineal)
    float                S = 0.f;                  // esparcido por mm (0 si td <= 0: opaco)
};
KMMedium km_medium(const std::array<float, 3>& rgb, float td_mm, float force = 1.f);

// 7-nov-2026 — «Lupa»: la luz entra, se esparce y se absorbe dentro de cada capa (dos flujos, Kubelka-Munk,
// el equivalente en una dimensión de la lupa CPU de docs/WIP/LUPA_CPU_PROTO). Color = lo que devuelve un bloque
// grueso; TD = grosor al que pasa el 10 % de la luz; `force` apaga más el color (1,2 = la lupa 3D ×5, la que casó con la Skull02 impresa).
std::array<float, 3> predict_lupa(const std::vector<Block>& blocks,
                                  const std::vector<Filament>& colours,
                                  const Filament& base,
                                  float layer_mm,
                                  float force = 1.f);

// F2 — mejor receta para `target` (sRGB [0..1]). `colours` = filamentos de color (≥3 pedido por Sebas;
// con menos se calcula igual pero `warning` lo dice). Búsqueda por bloques: cuántas capas de cada filamento,
// sum ≤ max_depth_mm/layer_mm. Devuelve la de MENOS profundidad con ΔE ≤ stop_de; si ninguna llega, la de
// menor ΔE (a igualdad, la menos profunda).
Result solve(const std::array<float, 3>& target,
             const std::vector<Filament>& colours,
             const Filament& base,
             const Options& opt);

} // namespace DepthStack
} // namespace Slic3r

#endif // slic3r_ColorSci_DepthStack_hpp_
