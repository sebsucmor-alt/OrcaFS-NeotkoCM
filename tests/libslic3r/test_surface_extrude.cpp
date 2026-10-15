// NEOTKO_CONSTRUCT_TAG s363 — Construct: la cara plana elegida, su contorno y la lamina extruida.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
using Catch::Approx;

#include "libslic3r/NeotkoSurfaceExtrude.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// 20 x 10 plate, 2 high, with a 4 x 4 hole through it.
static indexed_triangle_set plate_with_hole()
{
    ExPolygon ex;
    ex.contour.points = { Point::new_scale(0., 0.), Point::new_scale(20., 0.),
                          Point::new_scale(20., 10.), Point::new_scale(0., 10.) };
    Polygon hole;
    hole.points = { Point::new_scale(3., 3.), Point::new_scale(3., 7.),
                    Point::new_scale(7., 7.), Point::new_scale(7., 3.) };
    ex.holes.emplace_back(hole);
    return slices_to_mesh(std::vector<ExPolygons>{ { ex } }, 0., 2., 2.);
}

static int top_facet(const indexed_triangle_set &its, const std::vector<Vec3f> &normals)
{
    for (int f = 0; f < (int)its.indices.size(); ++f)
        if (normals[f].normalized().z() > 0.99f)
            return f;
    return -1;
}

TEST_CASE("Surface extrude: top face of a plate with a hole", "[Construct]")
{
    const indexed_triangle_set its       = plate_with_hole();
    const std::vector<Vec3f>   normals   = its_face_normals(its);
    const std::vector<Vec3i32> neighbors = its_face_neighbors(its);
    const int seed = top_facet(its, normals);
    REQUIRE(seed >= 0);

    const std::vector<int> region = SurfaceExtrude::coplanar_region(normals, neighbors, seed, 0.9994f);
    REQUIRE(!region.empty());
    for (int f : region)
        CHECK(normals[f].normalized().z() > 0.99f);

    double z = -1.;
    const ExPolygons outline = SurfaceExtrude::region_outline(its, region, Transform3d::Identity(), &z);
    REQUIRE(outline.size() == 1);
    CHECK(outline.front().holes.size() == 1);
    CHECK(z == Approx(2.));
    const double area_mm2 = outline.front().area() * SCALING_FACTOR * SCALING_FACTOR;
    CHECK(area_mm2 == Approx(20. * 10. - 4. * 4.).epsilon(0.01));

    const indexed_triangle_set sheet = SurfaceExtrude::extrude_outline(outline, 0., z, 0.05);
    REQUIRE(!sheet.indices.empty());
    CHECK(its_num_open_edges(sheet) == 0);
    CHECK(its_volume(sheet) == Approx(area_mm2 * 0.05).epsilon(0.01));

    // Shrinking by 1 mm: outer 18 x 8, hole grows to 6 x 6.
    const indexed_triangle_set shrunk = SurfaceExtrude::extrude_outline(outline, -1., z, 0.05);
    CHECK(its_volume(shrunk) == Approx((18. * 8. - 6. * 6.) * 0.05).epsilon(0.02));

    // s363c — a curve that is a straight line gives the plain sheet; one that pulls the top in
    // by 1 mm gives less volume than the plain one and still a closed solid.
    std::vector<SurfaceExtrude::CurveNode> curve(2);
    curve[0].t = 0.; curve[0].ht = 0.5;
    curve[1].t = 1.; curve[1].ht = 1.;
    const indexed_triangle_set straight = SurfaceExtrude::extrude_outline_curved(outline, 0., z, 2., curve, false);
    CHECK(its_volume(straight) == Approx(area_mm2 * 2.).epsilon(0.01));
    curve[0].he = -0.5; curve[1].e = -1.;
    const indexed_triangle_set tapered = SurfaceExtrude::extrude_outline_curved(outline, 0., z, 2., curve, false);
    CHECK(its_num_open_edges(tapered) == 0);
    CHECK(its_volume(tapered) < its_volume(straight));
    CHECK(its_volume(tapered) > (18. * 8. - 6. * 6.) * 2.);

    // An offset that eats the whole shape gives nothing.
    CHECK(SurfaceExtrude::extrude_outline(outline, -20., z, 0.05).indices.empty());
}
