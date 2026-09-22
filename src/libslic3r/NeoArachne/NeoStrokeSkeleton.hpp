// NEOTKO_NEOSTROKE_TAG C1 (s325)
// NeoStroke — el ESQUELETO de una isla, con radio en cada nodo, y su descomposición en TRAZOS.
//
// Port 1:1 de `docs/TOOLS/strokes/skel.py` (fase P, s324). El prototipo manda: si algo aquí no
// da los mismos números que él sobre la misma capa, el roto es esto, no el prototipo.
//
// Por qué un grafo propio y no el MedialAxis de Orca (ver NEOSTROKE_FASE_C_PREPLAN §2):
// NeoStroke necesita el RADIO en cada nodo y la TOPOLOGÍA (grados, ramas, ciclos) para decidir
// una vez por trazo cuántas líneas lleva. MedialAxis devuelve polilíneas ya cortadas por cruce y
// ya limpiadas a su manera; el grafo se reconstruye desde el Voronoi de un muestreo denso del
// contorno, que es de donde sale el suyo también.
//
// Las trampas que costaron la fase P y que están cableadas aquí (§6 de NEOARACHNE_STROKES_FASE_P):
//   · trampa 1  — las hojas se podan TODAS de una pasada, no de una en una.
//   · trampa 2  — sólo se fusionan CRUCES (grado ≥ 3); por distancia se come el esqueleto.
//   · trampa 12 — una ramita se poda por corta **y además** por no dejar área sin cubrir.
#ifndef slic3r_NeoStrokeSkeleton_hpp_
#define slic3r_NeoStrokeSkeleton_hpp_

#include <cstddef>
#include <vector>

#include "../ExPolygon.hpp"
#include "../Point.hpp"

namespace Slic3r { namespace NeoArachne {

struct SkeletonParams {
    double ds_mm          = 0.06;   // muestreo del contorno para el Voronoi
    double prune_mm       = 1.5;    // poda de ramitas, en veces el radio de su base
    double merge_mm       = 0.25;   // fusión de cruces partidos
    double prune_area_mm2 = 0.25;   // área que puede aportar una ramita y aun así podarse
};

// Una rama entre nodos de grado != 2 (o un ciclo puro: una O sin cruces).
struct StrokeBranch {
    std::vector<Vec2d>  pts;        // mm
    std::vector<double> r;          // mm — radio del disco inscrito en cada punto
    double              length_mm = 0.;
    bool                closed    = false;
    size_t              deg_front = 0;   // grado del nodo inicial
    size_t              deg_back  = 0;   // grado del nodo final
};

struct SkeletonStats {
    size_t nodes       = 0;
    size_t junctions   = 0;   // grado ≥ 3
    size_t leaves      = 0;   // grado 1
    size_t branches    = 0;
    size_t pruned      = 0;   // nodos que se ha llevado la poda
    size_t merged      = 0;   // cruces partidos fusionados
    size_t prune_passes = 0;
};

// El esqueleto de UNA isla (la letra entera, no el residuo: así lo hace el prototipo).
class StrokeSkeleton
{
public:
    StrokeSkeleton(const ExPolygon& island, const SkeletonParams& p);

    const std::vector<StrokeBranch>& branches() const { return m_branches; }
    // El camino más largo del esqueleto (su diámetro), en mm. Para un engorde sin ramas útiles
    // esto es LA línea que hay que imprimir. 🚨 El más largo de verdad no se puede buscar (el
    // esqueleto tiene ciclos y no termina): es Dijkstra al nodo más lejano, y sobre el pedazo
    // MAYOR, o se coge una ramita de dos décimas (trampas 3 y 4).
    std::vector<Vec2d> longest_path() const;
    const SkeletonStats&             stats()    const { return m_stats; }

private:
    // Grafo: nodos por índice, adyacencia por lista ordenada. Los nodos muertos quedan con
    // `m_alive[i] = false` (compactar renumeraría, y la identidad por posición ya ha costado
    // sesiones: ver lesson_identity_by_ordinal_position).
    std::vector<Vec2d>             m_pos;     // mm
    std::vector<std::vector<int>>  m_adj;
    std::vector<bool>              m_alive;
    std::vector<double>            m_r;       // mm, sólo válido tras compute_radii()
    SkeletonStats                  m_stats;
    std::vector<StrokeBranch>      m_branches;

    void build(const ExPolygon& island, double ds_mm);
    void prune(const ExPolygon& island, double factor, double area_tol);
    void merge_junctions(double tol_mm);
    void compute_radii(const ExPolygon& island);
    void extract_branches();

    size_t degree(int i) const { return m_adj[i].size(); }
    void   link(int a, int b);
    void   unlink_node(int i);
};

}} // namespace Slic3r::NeoArachne

#endif
