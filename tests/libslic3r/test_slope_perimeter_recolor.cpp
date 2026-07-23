// NEOTKO_ALHCOLOR_TAG — Fase 5.0 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md §7bis)
#include <catch2/catch.hpp>

#include "libslic3r/ColorSci/SlopePerimeterRecolor.hpp"
#include "libslic3r/ColorSci/ColorSci.hpp"

#include <cmath>

using namespace Slic3r;
using namespace Slic3r::ColorSci;
using Catch::Matchers::WithinAbs;

namespace {

Material mat_opaque(float r, float g, float b)
{
    Material m;
    m.rgb = { r, g, b };
    m.td  = { 0.f, 0.f, 0.f };
    return m;
}

} // namespace

TEST_CASE("compute_exposed_perimeter_count matches the d=layer_height*tan(alpha) geometry", "[SlopePerimeterRecolor]")
{
    const double w = 0.45; // perimeter_width_mm
    const int wall_loops = 3;

    SECTION("vertical wall (n_sin=1, n_cos=0) -> d=0 -> nothing exposed")
    {
        REQUIRE(compute_exposed_perimeter_count(0.2, 0.0, 1.0, w, wall_loops) == 0);
    }

    SECTION("near-flat facet (n_sin~0) clamps to wall_loops instead of dividing by zero")
    {
        REQUIRE(compute_exposed_perimeter_count(0.2, 1.0, 0.0, w, wall_loops) == wall_loops);
    }

    SECTION("moderate slope: d < w -> 0 interior rings exposed (only the external ring shows)")
    {
        const double n_cos = std::sin(20.0 * M_PI / 180.0);
        const double n_sin = std::cos(20.0 * M_PI / 180.0);
        REQUIRE(compute_ledge_depth_mm(0.2, n_cos, n_sin) < w);
        REQUIRE(compute_exposed_perimeter_count(0.2, n_cos, n_sin, w, wall_loops) == 0);
    }

    SECTION("steep slope (alpha=70deg, plan's own worked example): 1 interior ring exposed at 0.2mm")
    {
        const double alpha = 70.0 * M_PI / 180.0;
        const double n_cos = std::sin(alpha);
        const double n_sin = std::cos(alpha);
        REQUIRE(compute_exposed_perimeter_count(0.2, n_cos, n_sin, w, wall_loops) == 1);
    }

    SECTION("thinner adaptive layer height covers the same slope back to 0 exposed")
    {
        const double alpha = 70.0 * M_PI / 180.0;
        const double n_cos = std::sin(alpha);
        const double n_sin = std::cos(alpha);
        REQUIRE(compute_exposed_perimeter_count(0.07, n_cos, n_sin, w, wall_loops) == 0);
    }

    SECTION("result never exceeds wall_loops even at extreme d")
    {
        const double alpha = 89.0 * M_PI / 180.0;
        const double n_cos = std::sin(alpha);
        const double n_sin = std::cos(alpha);
        REQUIRE(compute_exposed_perimeter_count(1.0, n_cos, n_sin, w, wall_loops) == wall_loops);
    }

    SECTION("degenerate inputs return 0, never negative or NaN-derived")
    {
        REQUIRE(compute_exposed_perimeter_count(0.2, 0.5, 0.5, w, 0) == 0);
        REQUIRE(compute_exposed_perimeter_count(0.2, 0.5, 0.5, 0.0, wall_loops) == 0);
        REQUIRE(compute_exposed_perimeter_count(0.0, 0.5, 0.5, w, wall_loops) == 0);
    }
}

TEST_CASE("compute_ledge_depth_mm agrees with the ring count it feeds", "[SlopePerimeterRecolor]")
{
    const double alpha = 70.0 * M_PI / 180.0;
    const double n_cos = std::sin(alpha);
    const double n_sin = std::cos(alpha);
    const double d = compute_ledge_depth_mm(0.2, n_cos, n_sin);
    // d = 0.2 * tan(70deg) ~ 0.55mm; with w=0.45 that's floor(d/w)=1 interior ring — the
    // same answer compute_exposed_perimeter_count gives from the raw inputs.
    REQUIRE_THAT(d, WithinAbs(0.2 * std::tan(alpha), 1e-9));
    REQUIRE(int(std::floor(d / 0.45)) == compute_exposed_perimeter_count(0.2, n_cos, n_sin, 0.45, 3));
}

TEST_CASE("resolve_perimeter_colors is a no-op on a clean layer", "[SlopePerimeterRecolor]")
{
    Material mats[4] = { mat_opaque(1.f, 0.f, 0.f), mat_opaque(0.f, 1.f, 0.f),
                         mat_opaque(0.f, 0.f, 1.f), mat_opaque(1.f, 1.f, 1.f) };
    const float target_rgb[3] = { 1.f, 0.f, 0.f };
    const Lab target = rgb_to_lab(target_rgb);

    // d below the perimeter width: only the external ring shows.
    const PerimeterColorPlan plan = resolve_perimeter_colors(0.3, 0.45, 3, target, { 0, 1, 2, 3 }, mats);
    REQUIRE(plan.tool_per_perimeter.empty());
    REQUIRE_THAT(plan.delta_e, WithinAbs(0.0, 1e-9));
}

TEST_CASE("resolve_perimeter_colors picks the exact matching tool when one exists", "[SlopePerimeterRecolor]")
{
    // Tool 0 is a pure, opaque match for the target color; with d=1.5w two rings are visible
    // (external fully, interior half) and the plan should choose tool 0 for both, since any
    // other choice mixes in a worse color and raises dE2000 above 0.
    Material mats[4] = { mat_opaque(0.2f, 0.6f, 0.8f), mat_opaque(1.f, 0.f, 0.f),
                         mat_opaque(0.f, 1.f, 0.f), mat_opaque(0.f, 0.f, 1.f) };
    const float target_rgb[3] = { 0.2f, 0.6f, 0.8f };
    const Lab target = rgb_to_lab(target_rgb);

    const PerimeterColorPlan plan = resolve_perimeter_colors(1.5 * 0.45, 0.45, 3, target, { 0, 1, 2, 3 }, mats);
    REQUIRE(plan.tool_per_perimeter.size() == 2);
    REQUIRE(plan.tool_per_perimeter[0] == 0);
    REQUIRE(plan.tool_per_perimeter[1] == 0);
    REQUIRE_THAT(plan.delta_e, WithinAbs(0.0, 1e-3));
}

TEST_CASE("resolve_perimeter_colors mixes two opaque tools by visible area when only a mix reaches the target", "[SlopePerimeterRecolor]")
{
    // Opaque red + opaque blue, d=2w -> two rings at exactly 50% visible area each: a
    // {red, blue} assignment area-blends to (0.5, 0, 0.5), a purple neither tool alone can
    // reach. The area-weighted model (user decision s222) must find that mixed assignment;
    // an opacity-only blend would too here, but the point is opaque tools DO mix by area.
    Material mats[4] = { mat_opaque(1.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 1.f),
                         mat_opaque(0.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 0.f) };
    const float target_rgb[3] = { 0.5f, 0.f, 0.5f };
    const Lab target = rgb_to_lab(target_rgb);

    const PerimeterColorPlan plan = resolve_perimeter_colors(2.0 * 0.45, 0.45, 3, target, { 0, 1 }, mats);
    REQUIRE(plan.tool_per_perimeter.size() == 2);
    REQUIRE(plan.tool_per_perimeter[0] != plan.tool_per_perimeter[1]);
    REQUIRE_THAT(plan.delta_e, WithinAbs(0.0, 1e-2));
}

TEST_CASE("a barely-exposed boundary ring cannot skew the chosen colors", "[SlopePerimeterRecolor]")
{
    // d just past w: the interior ring is a sliver (vis ~ 2% of the ledge). The visible
    // color is essentially the external ring's, so with an exact-match tool available the
    // plan must keep the external ring on it and report a near-zero delta_e — the case a
    // uniform per-ring weighting would get wrong (it would let the sliver pull the blend
    // 50/50 and possibly recolor the external ring to compensate).
    Material mats[4] = { mat_opaque(1.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 1.f),
                         mat_opaque(0.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 0.f) };
    const float target_rgb[3] = { 1.f, 0.f, 0.f }; // pure red, tool 0
    const Lab target = rgb_to_lab(target_rgb);

    const double w = 0.45;
    const PerimeterColorPlan plan = resolve_perimeter_colors(w * 1.02, w, 3, target, { 0, 1 }, mats);
    REQUIRE(plan.tool_per_perimeter.size() == 2);
    REQUIRE(plan.tool_per_perimeter[0] == 0);
    REQUIRE(plan.delta_e < 2.0); // sliver of blue barely moves the blend off pure red
}

TEST_CASE("visible ring count is capped by wall_loops", "[SlopePerimeterRecolor]")
{
    Material mats[4] = { mat_opaque(1.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 1.f),
                         mat_opaque(0.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 0.f) };
    const float target_rgb[3] = { 1.f, 0.f, 0.f };
    const Lab target = rgb_to_lab(target_rgb);

    // d spans 5 ring widths but the object only prints 2 walls.
    const PerimeterColorPlan plan = resolve_perimeter_colors(5.0 * 0.45, 0.45, 2, target, { 0, 1 }, mats);
    REQUIRE(plan.tool_per_perimeter.size() == 2);
}

TEST_CASE("resolve_perimeter_colors falls back to tool 0 when candidate_tools is empty", "[SlopePerimeterRecolor]")
{
    Material mats[4] = { mat_opaque(0.3f, 0.3f, 0.3f), mat_opaque(1.f, 1.f, 1.f),
                         mat_opaque(0.f, 0.f, 0.f), mat_opaque(0.f, 0.f, 0.f) };
    const float target_rgb[3] = { 0.3f, 0.3f, 0.3f };
    const Lab target = rgb_to_lab(target_rgb);

    const PerimeterColorPlan plan = resolve_perimeter_colors(1.5 * 0.45, 0.45, 3, target, {}, mats);
    REQUIRE(plan.tool_per_perimeter.size() == 2);
    REQUIRE(plan.tool_per_perimeter[0] == 0);
    REQUIRE(plan.tool_per_perimeter[1] == 0);
}
