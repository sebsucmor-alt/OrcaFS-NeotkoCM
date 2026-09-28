// NEOTKO_NEOSTROKE_TAG s337 — las herramientas de medida de s336, dentro del visor.
//
// En s336 todas las decisiones de 2_47 salieron de medir el G-code con scripts de Python
// (`docs/TOOLS/strokes/medida/s336_*.py`). Esto es la transcripción, sobre el resultado del visor en vez de
// sobre el G-code, para tener las mismas cifras ANTES de imprimir:
//   · `s336_zonas_por_caja.py`  → mm de interior, % con cordón real (sep ≥ 0.25), arranques, mm/arranque, mm³
//   · `s336_capa_huella.py`     → la huella real de cada tramo: sep = w − h·(1 − π/4), remates redondos
//   · `s336_huecos_junta.py`    → los huecos, clasificados por lo que tocan: sólo Classic / JUNTA / sólo NS
//   · `s336_capa_recorridos.py` → los recorridos: cada tramo continuo, con su arranque y su parada
// 🚨 Criterio de los scripts, respetado: "interior" = lo que puso NeoStroke; el ancho de las cifras es el
//    NOMINAL (el de `;WIDTH:`), que es lo que leían ellos. La huella que se DIBUJA usa el de caudal.
// Puro cálculo, sin GUI; se llama desde el hilo de trabajo del gizmo.
#ifndef slic3r_NeoArachne_Preview_PreviewMetrics_hpp_
#define slic3r_NeoArachne_Preview_PreviewMetrics_hpp_

#include "PreviewResult.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r { namespace NeoArachne { namespace Preview {

// s337b — Hole y InfillPocket (TEST23, la P de PLAGE): lo que NeoStroke no cubre y CABE un cordón se lo pasa a
// `fill_surfaces`, el relleno normal. En letras son bolsitas pequeñas donde el relleno macizo no mete ni una línea: en
// el G-code del logotipo del TEST23 no hay NI UNA línea de relleno, y la P sale con agujero. El visor no lamina el
// relleno, así que marca esas bolsitas aparte. Hole = hueco grande (> gap_max) que no es de nadie.
enum class GapKind : unsigned char { ClassicOnly, Joint, NeoStrokeOnly, Hole, InfillPocket };
constexpr int kGapKinds = 5;

struct Gap {
    ExPolygon poly;     // coordenadas escaladas, las del corte
    GapKind   kind = GapKind::ClassicOnly;
    double    area_mm2 = 0.0;
};

// Un recorrido: tramos con extrusión seguidos, sin salto, del mismo dueño (Classic / NeoStroke).
// Arranque = `start`, parada = `end`. Es lo que en s332 se llamó "cortes de flujo".
struct Run {
    Point  start, end;
    bool   neostroke = false;
    double length_mm = 0.0;
};

struct LayerMetrics {
    double interior_mm    = 0.0;   // mm de NeoStroke
    double interior_ok_mm = 0.0;   // de ellos, con sep ≥ umbral (cordón real)
    double classic_mm     = 0.0;
    size_t starts         = 0;     // arranques de NeoStroke
    double vol_mm3        = 0.0;   // Σ l·w·h de NeoStroke (nominal, como el script)
    // s337 — regla de vecinos (`docs/WIP/NEOSTROKE_BEAD_MODEL_DATA.md` §4, confianza MEDIA, 1 comparación): el
    // mínimo de 0.25 sólo rompe la línea SUELTA o de borde. Una fina con vecinos a los dos lados la aplastan y
    // se llena (las puntas de las medias lunas de TEST21 salen cerradas en la foto).
    double thin_loose_mm     = 0.0;  // NS con sep < umbral y sin vecino a algún lado: la que falla
    double thin_squeezed_mm  = 0.0;  // NS con sep < umbral pero con vecinos a los dos lados
    // s337b — RIESGOS calibrados con las macros del TEST22 (`docs/WIP/TEST22_frames/INDEX.md`):
    size_t tip_risk_count    = 0;    // recorridos que arrancan/paran en un tramo suelto a más de la velocidad límite
    double tip_risk_mm       = 0.0;
    double seam_risk_mm      = 0.0;  // cordón ancho (≥ seam_sep_mm) pegado a otro ancho: raja (TEST22 Z6)
    double double_pass_mm    = 0.0;  // tramo cuyo centro cae dentro de la huella de uno anterior («ironing»)
    double excess_mm2        = 0.0;  // s337b — área con más plástico del que cabe («batiburrillo»)
    // s338 — NUDO DE PUNTAS (TEST24, la onda: un hueco que se apila en Z): un recorrido que ARRANCA (viene de un viaje:
    // retracción y presión que aún no ha llegado) a menos de `knot_mm` de otro arranque o de una parada. Tres puntas
    // afiladas juntas y con la presión corta dejan un hoyo, y como la geometría es la misma capa a capa, se apila.
    size_t knot_count        = 0;
    // s339 — TEST25 (fotos contra G-code, `campo/t25/`): lo que el visor no veía.
    double contact_mm        = 0.0;  // NS que no llega a solaparse con su vecino (cordón o muro): surco, luz a contraluz
    double sustained_mm2     = 0.0;  // sobra SOSTENIDA (> ~105 % en 1 mm): la montaña de la extrusión extra en curva
    size_t gap_count[kGapKinds] = {0, 0, 0, 0, 0};   // por GapKind
    double gap_mm2[kGapKinds]   = {0.0, 0.0, 0.0, 0.0, 0.0};

    double pct_ok() const        { return interior_mm > 0.0 ? 100.0 * interior_ok_mm / interior_mm : 0.0; }
    double mm_per_start() const  { return starts > 0 ? interior_mm / double(starts) : 0.0; }
    double gap_total_mm2() const { double t = 0.0; for (double g : gap_mm2) t += g; return t; }
    void   add(const LayerMetrics& o);
};

struct MetricsOptions {
    double sep_threshold_mm = 0.25;   // el criterio de s332: por debajo no es un cordón de verdad
    double gap_max_mm2      = 0.3;    // s337b — por encima ya no es un hueco entre cordones: es un AGUJERO (GapKind::Hole)
    double gap_min_mm2      = 1e-4;   // astillas numéricas de Clipper, no huecos
    double pocket_max_mm2   = 3.0;    // s337b — zona de relleno más pequeña que esto = bolsita que el relleno no imprime
    double neighbour_mm     = 0.10;   // hasta dónde se busca vecino a cada lado, pasado el borde de la línea
    bool   compute_gaps     = true;
    // s337b — riesgos. TEST22: la punta de la cuña (línea suelta de 0.32, arranque) aguanta a 15 mm/s y se rompe
    // a 30 y a 60 ⇒ el límite está entre las dos; 20 es el punto de partida, no una medida.
    double speed_mm_s       = 0.0;    // velocidad de NeoStroke (muro interior) de este corte; 0 = desconocida
    double tip_speed_limit  = 20.0;
    double tip_zone_mm      = 1.5;    // cuánto del principio/final de un recorrido cuenta como «punta»
    // Z5 (0,3 % de la longitud ≥ 0.6) sin rajas; Z6 (24 % ≥ 0.6) con rajas en el anillo.
    double seam_sep_mm      = 0.60;
    // s338 — nudo de puntas: distancia entre un arranque y otra punta (arranque o parada) para contarlos juntos.
    // TEST24 (onda, Z1.08): dos arranques y una parada a 0.23 mm entre sí → hoyo en la foto.
    double knot_mm          = 0.40;
    int    knot_others      = 2;      // puntas de otros recorridos que tiene que haber cerca (el TEST24: dos)
    double knot_thin        = 0.85;   // arranque «en cola»: ancho al arrancar < esto × el de su primer 0.5 mm
    // s339 — TEST25: el agujero de arriba de los anillos (las 8 zonas) son 2-3 ARRANQUES anchos juntos, que la regla de
    // la cola fina no ve. Racimo = puntas de NS encadenadas a menos de `knot_chain_mm`; riesgo si trae ≥ `knot_starts`
    // arranques. Las paradas juntas NO dejan agujero en las fotos (llegan con presión de sobra).
    double knot_chain_mm    = 0.60;
    int    knot_starts      = 2;
    // s339 — CONTACTO: holgura con el vecino de cada lado = distancia entre ejes − (sepA + sepB)/2 (huella de caudal).
    // Los carriles se planean TANGENTES (holgura 0): el visor los daba por cubiertos y en la foto salen surcos y, a
    // contraluz, líneas por donde pasa la luz (TEST25 zona 1: 41 % de las muestras sin tocar). Riesgo si la holgura es
    // > −closure_mm, o sea, si no se pisan al menos `closure_mm`.
    bool   compute_contact  = true;
    double contact_step_mm  = 0.04;   // una muestra cada tanto por eje
    double contact_reach_mm = 1.20;   // hasta dónde se busca vecino
    // s339 — los dos MANDOS del gizmo:
    //  · closure_mm = CIERRE del material: cuánto tienen que pisarse dos cordones para que no quede surco. Un PLA
    //    brillante (los tests en blanco + magenta brillo) pide más; un matte, que se extiende, menos (o negativo).
    //  · level = NIVEL DE AVISO (1 = 100 %): > 1 avisa antes (más marcas), < 1 sólo lo claro. Mueve todos los umbrales.
    double closure_mm       = -0.01;  // el gizmo lo pisa; −0,01 = sólo los que de verdad no se tocan
    double level            = 1.0;
    // s337b — BATIBURRILLO: el volumen REAL de cada tramo (su caudal) repartido sobre una rejilla fina de la capa.
    // Donde cae más plástico del que cabe (celda × altura de capa) sobra: se amontona, se arrastra con la cabeza.
    // Suma los solapes de verdad: extra flow en curvas, doble pasada, U que se pisan, junta con el muro.
    bool   compute_excess   = true;
    double excess_cell_mm   = 0.05;
    double excess_ratio     = 1.30;   // umbral: 130 % de lo que cabe (mando en el gizmo)
    // s339 — la MONTAÑA: la misma densidad (contra el ancho NOMINAL), promediada en 1 mm, por encima de ~105 %. La
    // extrusión extra en curva mete un 2-17 % de más a lo largo de todo el cordón: en 0,35 mm y al 130 % no se ve nunca,
    // y en la foto es la media luna levantada del anillo (zonas 4, 5, 6 del TEST25).
    double sustained_box_mm = 1.00;
    double sustained_ratio  = 1.05;
};

// Marcas por tramo de `PreviewResult::ordered_segments` (mismo índice). Se COMBINAN (bits).
enum SegFlag : unsigned char {
    sfNone         = 0,
    sfThinLoose    = 1,    // < 0.25 sin vecino a algún lado: no sale
    sfThinSqueezed = 2,    // < 0.25 entre vecinos: se aplasta y llena
    sfTipRisk      = 4,    // punta de recorrido suelta a velocidad alta: se estira y se rompe (TEST22 Z4/Z7)
    sfSeamRisk     = 8,    // cordón ancho junto a otro ancho: raja (TEST22 Z6)
    sfDoublePass   = 16,   // pasa por encima de lo ya puesto en esta capa: doble relieve (TEST22 Z3)
    sfKnotRisk     = 32,   // s338 — arranque pegado a otras puntas: hoyo que se apila en Z (TEST24, la onda)
    sfContact      = 64,   // s339 — no se pisa con su vecino: surco / luz a contraluz (TEST25 zona 1)
};

struct MetricsResult {
    LayerMetrics      m;
    std::vector<unsigned char> seg_flags;   // SegFlag por tramo; vacío si no se calculó
    std::vector<Gap>  gaps;
    std::vector<Run>  runs;
    std::vector<Point> excess_cells;        // centros (escalados) de las celdas con exceso
    std::vector<Point> sustained_cells;     // s339 — celdas de sobra sostenida (la montaña)
    double             excess_cell_mm = 0.0;
};

// sep de un tramo: lo que ocupa de lado a lado sobre la capa. `width` = ancho del cordón, `height` = capa.
inline double footprint_sep(double width, double height)
{
    return width - height * (1.0 - 0.25 * PI);
}

MetricsResult compute_layer_metrics(const PreviewResult& r, const MetricsOptions& opt = MetricsOptions());

}}} // namespace Slic3r::NeoArachne::Preview

#endif
