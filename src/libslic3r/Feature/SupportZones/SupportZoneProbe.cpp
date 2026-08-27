// NEOTKO_SUPPORTZONES_TAG s284 — F1. See SupportZoneProbe.hpp for the design notes.

#include "SupportZoneProbe.hpp"

#include "../../AABBTreeIndirect.hpp"
#include "../../Model.hpp"
#include "../../TriangleMesh.hpp"
#include "../../NeoDebug.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

// Same pattern as the corridor of F0: an existing NeoDebug channel, never BOOST_LOG_TRIVIAL.
// Read it with:  ORCA_DEBUG_WAVESUPPORT=1  ->  /tmp/neotko_wavesupport.log
#define ZONEPROBE_LOG(body)                                                       \
    do {                                                                          \
        if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::WAVESUPPORT)) {           \
            std::ostringstream _ndbg_;                                            \
            _ndbg_ << std::fixed << std::setprecision(3) << body;                 \
            Slic3r::NeoDebug::write(Slic3r::NeoDebug::WAVESUPPORT, _ndbg_.str()); \
        }                                                                         \
    } while (0)

namespace Slic3r {
namespace SupportZones {

// A facet counts as downward-facing below this normal Z. Deliberately NOT the support threshold
// angle: this is a render/warning filter, and anything that leans downward at all can be caught by
// a support column. Being generous here keeps the warning conservative — it only fires when there
// is genuinely nothing pointing down.
static constexpr float DOWNWARD_NORMAL_Z_MAX = -0.02f;

// Guard rails for the grid step. A 10 mm pillar resolution on a 4 mm block would sample nothing and
// report a false sterile; a 0.1 mm one on a 200 mm block would cost millions of rays for a picture
// nobody can see anyway.
static constexpr int   GRID_MIN_CELLS_PER_AXIS = 3;
static constexpr int   GRID_MAX_CELLS_PER_AXIS = 160;

// 🔑 s286b — el mapa de huecos tiene su propio techo, y mucho más alto. El de arriba está pensado
// para un BLOQUE, donde 160 celdas por eje sobran; el mapa cubre el objeto ENTERO y ahí 160 celdas
// convierten cualquier pieza mediana en un tablero de ajedrez, y el deslizador de detalle no
// serviría de nada porque este clamp lo comería. Sigue habiendo techo: sin él, un paso de 0,1 mm
// en una pieza de 200 son cuatro millones de rayos por zona.
static constexpr int   COVERAGE_MAX_CELLS_PER_AXIS = 600;

namespace {

// Object-local geometry of the model parts, plus its AABB tree.
struct MeshAndTree
{
    indexed_triangle_set its;
    AABBTreeIndirect::Tree3f tree;
    bool empty() const { return its.indices.empty(); }
};

MeshAndTree collect(const ModelObject &object, bool model_parts, const ModelVolume *only)
{
    MeshAndTree out;
    for (const ModelVolume *v : object.volumes) {
        if (only != nullptr) {
            if (v != only)
                continue;
        } else if (! (model_parts && v->is_model_part()))
            continue;
        indexed_triangle_set part = v->mesh().its;
        if (part.indices.empty())
            continue;
        its_transform(part, v->get_matrix());
        its_merge(out.its, part);
    }
    if (! out.its.indices.empty())
        out.tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(out.its.vertices, out.its.indices);
    return out;
}

Vec3f facet_normal(const indexed_triangle_set &its, int face_id)
{
    const auto  &f = its.indices[face_id];
    const auto  &a = its.vertices[f(0)];
    const auto  &b = its.vertices[f(1)];
    const auto  &c = its.vertices[f(2)];
    const Vec3f  n = (b - a).cross(c - a);
    const float  l = n.norm();
    return (l > 0.f) ? Vec3f(n / l) : Vec3f(0.f, 0.f, 0.f);
}

} // namespace

ZoneProbe probe_zone(const ModelObject &object, const ModelVolume &enforcer, float grid_step_mm)
{
    ZoneProbe probe;

    const MeshAndTree zone = collect(object, false, &enforcer);
    if (zone.empty())
        return probe;
    const MeshAndTree parts = collect(object, true, nullptr);
    if (parts.empty())
        return probe;

    // Zone bounding box in the object-local frame.
    Vec3f bb_min = zone.its.vertices.front();
    Vec3f bb_max = bb_min;
    for (const auto &v : zone.its.vertices) {
        bb_min = bb_min.cwiseMin(v);
        bb_max = bb_max.cwiseMax(v);
    }
    const Vec3f size = bb_max - bb_min;
    if (size.x() <= 0.f || size.y() <= 0.f)
        return probe;

    // Clamp the step so a block always gets a usable number of samples in both axes.
    const float span = std::max(size.x(), size.y());
    float step = (grid_step_mm > 0.f) ? grid_step_mm : 2.f;
    step = std::min(step, span / float(GRID_MIN_CELLS_PER_AXIS));
    step = std::max(step, span / float(GRID_MAX_CELLS_PER_AXIS));
    probe.grid_step_mm = step;

    // Cast from below everything so the interval parity of the zone ray is unambiguous.
    float parts_min_z = parts.its.vertices.front().z();
    for (const auto &v : parts.its.vertices)
        parts_min_z = std::min(parts_min_z, v.z());
    const double ray_z0 = double(std::min(bb_min.z(), parts_min_z)) - 1.;
    // ⚠️ The ray-triangle test inside AABBTreeIndirect is SFINAE-gated on Scalar == double (see
    // intersect_triangle there), so the ray has to be double even though the mesh is float.
    const Vec3d  dir(0., 0., 1.);

    std::vector<igl::Hit> hits_zone;
    std::vector<igl::Hit> hits_parts;
    // Half a cell in, so the samples sit at cell centres and a block edge never lands exactly on a
    // ray — grazing a boundary facet is the one case where the parity test is fragile.
    for (float y = bb_min.y() + 0.5f * step; y < bb_max.y(); y += step) {
        for (float x = bb_min.x() + 0.5f * step; x < bb_max.x(); x += step) {
            const Vec3d origin(double(x), double(y), ray_z0);

            hits_zone.clear();
            if (! AABBTreeIndirect::intersect_ray_all_hits(zone.its.vertices, zone.its.indices, zone.tree, origin, dir, hits_zone))
                continue;
            // Entry/exit pairs. An odd tail means the ray grazed the surface or the block is not
            // watertight; dropping it is safer than inventing an interval that runs to infinity.
            const size_t pairs = hits_zone.size() / 2;
            if (pairs == 0)
                continue;
            ++ probe.cells_inside_zone;

            hits_parts.clear();
            if (! AABBTreeIndirect::intersect_ray_all_hits(parts.its.vertices, parts.its.indices, parts.tree, origin, dir, hits_parts))
                continue;

            for (const igl::Hit &hit : hits_parts) {
                const Vec3f n = facet_normal(parts.its, hit.id);
                // 🚨 Render/warning filter only. What NEEDS support is still detect_overhangs().
                if (n.z() >= DOWNWARD_NORMAL_Z_MAX)
                    continue;
                const double z = ray_z0 + double(hit.t);
                bool inside = false;
                for (size_t i = 0; i < pairs; ++ i) {
                    const double z_in  = ray_z0 + double(hits_zone[2 * i].t);
                    const double z_out = ray_z0 + double(hits_zone[2 * i + 1].t);
                    if (z >= z_in && z <= z_out) {
                        inside = true;
                        break;
                    }
                }
                if (inside)
                    probe.lit.push_back({ Vec3f(x, y, float(z)), n });
            }
        }
    }

    ZONEPROBE_LOG("[ZONEPROBE] zone='" << enforcer.name << "' step=" << probe.grid_step_mm << "mm"
        << " cells_in_zone=" << probe.cells_inside_zone
        << " lit=" << probe.lit.size()
        << (probe.sterile() ? "  ← STERILE (this zone will produce nothing)" : ""));
    return probe;
}

CoverageProbe probe_object_coverage(const ModelObject &object, float grid_step_mm, float max_normal_z)
{
    CoverageProbe probe;

    const MeshAndTree parts = collect(object, true, nullptr);
    if (parts.empty())
        return probe;

    // Un árbol POR ZONA, no uno solo con todas fundidas: la prueba de "dentro" es por paridad de
    // entradas/salidas, y dos bloques que se solapan intercalan sus pares y rompen la paridad. Con
    // un árbol por zona cada uno responde por sí mismo y el solape deja de importar. Las zonas son
    // pocas, así que el coste es el mismo.
    std::vector<MeshAndTree> zones;
    for (const ModelVolume *v : object.volumes)
        if (v->is_support_enforcer()) {
            MeshAndTree z = collect(object, false, v);
            if (! z.empty())
                zones.emplace_back(std::move(z));
        }

    Vec3f bb_min = parts.its.vertices.front();
    Vec3f bb_max = bb_min;
    for (const auto &v : parts.its.vertices) {
        bb_min = bb_min.cwiseMin(v);
        bb_max = bb_max.cwiseMax(v);
    }
    const Vec3f size = bb_max - bb_min;
    if (size.x() <= 0.f || size.y() <= 0.f)
        return probe;

    const float span = std::max(size.x(), size.y());
    float step = (grid_step_mm > 0.f) ? grid_step_mm : 2.f;
    step = std::min(step, span / float(GRID_MIN_CELLS_PER_AXIS));
    step = std::max(step, span / float(COVERAGE_MAX_CELLS_PER_AXIS));
    probe.grid_step_mm = step;

    const double ray_z0 = double(bb_min.z()) - 1.;
    const Vec3d  dir(0., 0., 1.);

    std::vector<igl::Hit> hits_parts;
    std::vector<igl::Hit> hits_zone;
    for (float y = bb_min.y() + 0.5f * step; y < bb_max.y(); y += step) {
        for (float x = bb_min.x() + 0.5f * step; x < bb_max.x(); x += step) {
            const Vec3d origin(double(x), double(y), ray_z0);
            hits_parts.clear();
            if (! AABBTreeIndirect::intersect_ray_all_hits(parts.its.vertices, parts.its.indices, parts.tree, origin, dir, hits_parts))
                continue;
            ++ probe.cells;

            for (const igl::Hit &hit : hits_parts) {
                const Vec3f n = facet_normal(parts.its, hit.id);
                // 🚨 Filtro de RENDER. Lo que necesita soporte lo decide detect_overhangs().
                if (n.z() >= DOWNWARD_NORMAL_Z_MAX || n.z() > max_normal_z)
                    continue;
                const double z = ray_z0 + double(hit.t);

                bool inside_any = false;
                for (const MeshAndTree &zone : zones) {
                    hits_zone.clear();
                    if (! AABBTreeIndirect::intersect_ray_all_hits(zone.its.vertices, zone.its.indices, zone.tree, origin, dir, hits_zone))
                        continue;
                    const size_t pairs = hits_zone.size() / 2;
                    for (size_t i = 0; i < pairs && ! inside_any; ++ i) {
                        const double z_in  = ray_z0 + double(hits_zone[2 * i].t);
                        const double z_out = ray_z0 + double(hits_zone[2 * i + 1].t);
                        if (z >= z_in && z <= z_out)
                            inside_any = true;
                    }
                    if (inside_any)
                        break;
                }
                (inside_any ? probe.covered : probe.uncovered).push_back({ Vec3f(x, y, float(z)), n });
            }
        }
    }

    ZONEPROBE_LOG("[ZONEPROBE] coverage step=" << probe.grid_step_mm << "mm"
        << " celdas=" << probe.cells
        << " cogido=" << probe.covered.size()
        << " sin_coger=" << probe.uncovered.size()
        << "  (render only — quien decide sigue siendo detect_overhangs)");
    return probe;
}

std::vector<const ModelVolume*> sterile_zones(const ModelObject &object, float grid_step_mm)
{
    std::vector<const ModelVolume*> sterile;
    for (const ModelVolume *v : object.volumes)
        if (v->is_support_enforcer() && probe_zone(object, *v, grid_step_mm).sterile())
            sterile.push_back(v);
    return sterile;
}

double corridor_reach_mm(double height_mm, double layer_height_mm, double support_line_width_mm)
{
    if (height_mm <= 0. || layer_height_mm <= 0. || support_line_width_mm <= 0.)
        return 0.;
    // One step per layer crossed. Deliberately NOT a closed-form angle: the engine really does
    // clamp per layer, so the reach is a count of steps and the two stay identical by construction.
    const double layers = height_mm / layer_height_mm;
    return layers * corridor_step_mm(support_line_width_mm);
}

} // namespace SupportZones
} // namespace Slic3r
