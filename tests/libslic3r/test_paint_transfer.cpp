// A boolean keeps the painting of its inputs.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
using Catch::Approx;

#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PaintTransfer.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

static double area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d p0 = its.vertices[f[0]].cast<double>(), p1 = its.vertices[f[1]].cast<double>(),
                    p2 = its.vertices[f[2]].cast<double>();
        a += 0.5 * (p1 - p0).cross(p2 - p0).norm();
    }
    return a;
}

static double painted_area(const ModelVolume &mv, EnforcerBlockerType state)
{
    return area(mv.mmu_segmentation_facets.get_facets(mv, state));
}

// A: 10 mm cube at the origin, filament 1, with a 2.5 mm strip painted in filament 2 along the
// left edge of its top (x < 2.5). The strip is narrower than the facets, so the painting lives in
// split triangles: the case that is easiest to lose.
// B: 10 mm cube at (5, 3, 2), filament 3, unpainted. It overlaps A and covers part of A's top, but
// not the strip.
struct Scene
{
    Model        model;
    ModelObject *a = nullptr;
    ModelObject *b = nullptr;

    Scene()
    {
        a = model.add_object();
        a->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)), ModelVolumeType::MODEL_PART, false);
        a->add_instance();
        a->config.set_key_value("extruder", new ConfigOptionInt(1));

        ModelVolume     &va = *a->volumes.front();
        TriangleSelector sel(va.mesh());
        sel.resample([](const Vec3f &p) {
            TriangleSelector::SampledState s;
            if (p.z() > 9.999f && p.x() < 2.5f)
                s.state = EnforcerBlockerType::Extruder2;
            return s;
        }, 0.05f);
        va.mmu_segmentation_facets.set(sel);

        b = model.add_object();
        b->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)), ModelVolumeType::MODEL_PART, false);
        b->add_instance()->set_offset(Vec3d(5., 3., 2.));
        b->config.set_key_value("extruder", new ConfigOptionInt(3));
    }

    TriangleMesh world_mesh(const ModelObject *o) const
    {
        TriangleMesh m = o->raw_mesh();
        m.transform(o->instances.front()->get_matrix());
        return m;
    }

    // Runs the boolean and the transfer; the result is a new object whose only volume holds the
    // mesh in world coordinates (identity placement).
    ModelVolume &run(const std::string &op, bool b_is_cutter)
    {
        std::vector<TriangleMesh> out;
        MeshBoolean::mcut::make_boolean(world_mesh(a), world_mesh(b), out, op);
        REQUIRE(! out.empty());
        ModelObject *r = model.add_object();
        r->add_volume(std::move(out.front()), ModelVolumeType::MODEL_PART, false);
        r->add_instance();

        std::vector<PaintTransfer::Source> sources;
        PaintTransfer::append_object_sources(*a, false, sources);
        PaintTransfer::append_object_sources(*b, b_is_cutter, sources);
        ModelVolume &target = *r->volumes.front();
        PaintTransfer::transfer(sources, target, Transform3d::Identity(), 1);
        return target;
    }
};

TEST_CASE("Paint carry-over: the source strip is painted", "[PaintTransfer]")
{
    Scene s;
    REQUIRE(painted_area(*s.a->volumes.front(), EnforcerBlockerType::Extruder2) == Approx(25.).epsilon(0.03));
}

TEST_CASE("Paint carry-over: union keeps the strip and B's filament", "[PaintTransfer]")
{
    Scene        s;
    ModelVolume &r = s.run("UNION", false);
    // The strip is outside B, so all of it survives.
    CHECK(painted_area(r, EnforcerBlockerType::Extruder2) == Approx(25.).epsilon(0.03));
    // B's skin outside A: 600 minus what A swallows = 469 mm2, painted with B's filament.
    CHECK(painted_area(r, EnforcerBlockerType::Extruder3) == Approx(469.).epsilon(0.02));
    // A's unpainted skin stays unpainted (it prints with the base filament).
    CHECK(painted_area(r, EnforcerBlockerType::Extruder1) == Approx(0.).margin(1e-6));
}

TEST_CASE("Paint carry-over: difference leaves the cut walls unpainted", "[PaintTransfer]")
{
    Scene        s;
    ModelVolume &r = s.run("A_NOT_B", true);
    CHECK(painted_area(r, EnforcerBlockerType::Extruder2) == Approx(25.).epsilon(0.03));
    CHECK(painted_area(r, EnforcerBlockerType::Extruder3) == Approx(0.).margin(1e-6));
}
