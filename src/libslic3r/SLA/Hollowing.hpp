#ifndef SLA_HOLLOWING_HPP
#define SLA_HOLLOWING_HPP

#include <memory>
#include <functional>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/SLA/JobController.hpp>

namespace Slic3r {

namespace sla {

struct HollowingConfig
{
    double min_thickness    = 2.;
    double quality          = 0.5;
    double closing_distance = 0.5;
    bool enabled = true;
    bool whole_mesh = false; // voxelize without splitting into shells
};

enum HollowingFlags { hfRemoveInsideTriangles = 0x1 };

// All data related to a generated mesh interior. Includes the 3D grid and mesh
// and various metadata. No need to manipulate from outside.
struct Interior;
struct InteriorDeleter { void operator()(Interior *p); };
using  InteriorPtr = std::unique_ptr<Interior, InteriorDeleter>;

indexed_triangle_set &      get_mesh(Interior &interior);
const indexed_triangle_set &get_mesh(const Interior &interior);

struct DrainHole
{
    Vec3f pos;
    Vec3f normal;
    float radius;
    float height;
    bool  failed = false;

    DrainHole()
        : pos(Vec3f::Zero()), normal(Vec3f::UnitZ()), radius(5.f), height(10.f)
    {}

    DrainHole(Vec3f p, Vec3f n, float r, float h, bool fl = false)
        : pos(p), normal(n), radius(r), height(h), failed(fl)
    {}

    DrainHole(const DrainHole& rhs) :
        DrainHole(rhs.pos, rhs.normal, rhs.radius, rhs.height, rhs.failed) {}
    
    bool operator==(const DrainHole &sp) const;
    
    bool operator!=(const DrainHole &sp) const { return !(sp == (*this)); }

    bool is_inside(const Vec3f& pt) const;

    bool get_intersections(const Vec3f& s, const Vec3f& dir,
                           std::array<std::pair<float, Vec3d>, 2>& out) const;
    
    indexed_triangle_set to_mesh() const;
    
    template<class Archive> inline void serialize(Archive &ar)
    {
        ar(pos, normal, radius, height, failed);
    }

    static constexpr size_t steps = 32;
};

using DrainHoles = std::vector<DrainHole>;

constexpr float HoleStickOutLength = 1.f;

InteriorPtr generate_interior(const TriangleMesh &mesh,
                              const HollowingConfig &  = {},
                              const JobController &ctl = {});

// Will do the hollowing
void hollow_mesh(TriangleMesh &mesh, const HollowingConfig &cfg, int flags = 0);

// Hollowing prepared in "interior", merge with original mesh
void hollow_mesh(TriangleMesh &mesh, const Interior &interior, int flags = 0);

void remove_inside_triangles(TriangleMesh &mesh, const Interior &interior,
                             const std::vector<bool> &exclude_mask = {});

double get_distance(const Vec3f &p, const Interior &interior);

template<class T>
FloatingOnly<T> get_distance(const Vec<3, T> &p, const Interior &interior)
{
    return get_distance(Vec3f(p.template cast<float>()), interior);
}

void cut_drainholes(std::vector<ExPolygons> & obj_slices,
                    const std::vector<float> &slicegrid,
                    float                     closing_radius,
                    const sla::DrainHoles &   holes,
                    std::function<void(void)> thr);

// Rebuild a mesh from voxels (Align & Construct > Remesh).
// The whole mesh is voxelized in one go, so open shells, holes and flipped faces come out as one
// closed solid. `offset` moves the surface (+ out, - in). With `hollow`, the inside goes `wall` mm
// under the new surface, smoothed by `smooth` (shrink-and-grow). The opening (`mouth`, clipped to
// the solid `wall` mm in, plus the short `plug` through the face) and the subtraction are done on
// the voxels too: no mesh boolean, so nothing fails on meshes that barely touch.
struct RemeshParams
{
    double voxel  = 0.2;  // mm
    double offset = 0.;   // mm
    bool   hollow = false;
    double wall   = 2.;   // mm
    double smooth = 3.;   // mm
    bool   want_outer = true;  // false: only the inside with its opening (a negative part)
    double adaptivity = 0.1;   // 0 = all triangles the voxel size; higher merges them where the surface is flat
    bool   fast_smooth = true; // the inside on coarser voxels (about Smooth / 10): it is smooth anyway
    const indexed_triangle_set *mouth = nullptr; // world mm, may be null
    const indexed_triangle_set *plug  = nullptr;
    // Called between steps (one OpenVDB step can't be stopped halfway): true = stop.
    std::function<bool()>                  cancel;
    std::function<void(int, const char *)> status; // percent, step
};
struct RemeshResult
{
    indexed_triangle_set outer;  // the new solid (hollow and open when asked)
    indexed_triangle_set cavity; // the inside with its opening, when !want_outer
    bool has_cavity = false;
    bool cancelled  = false;
};
RemeshResult neo_remesh(const indexed_triangle_set &mesh, const RemeshParams &p);

inline void swap_normals(indexed_triangle_set &its)
{
    for (auto &face : its.indices)
        std::swap(face(0), face(2));
}

} // namespace sla
} // namespace Slic3r

#endif // HOLLOWINGFILTER_H
