// NEOTKO_ALHCOLOR_TAG — Fase 0 (PRECISION_ALH_ADAPT_TO_COLOR_PLAN.md)
#include <catch2/catch.hpp>

#include "libslic3r/ColorSci/ColorHeightEnvelope.hpp"
#include "libslic3r/ColorSci/ColorSci.hpp"

#include <cmath>

using namespace Slic3r;
using namespace Slic3r::ColorSci;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Build a Material with a uniform scalar TD across channels.
Material mat_with_td(float td)
{
    Material m;
    m.rgb = { 0.5f, 0.5f, 0.5f };
    m.td  = { td, td, td };
    return m;
}

} // namespace

TEST_CASE("min_thickness_for_opacity inverts slice_opacity", "[ColorHeightEnvelope]")
{
    // Round-trip: pick a TD and a target opacity, compute the required
    // thickness, feed it back through slice_opacity, and expect the target.
    const double ref_h = 0.2;
    const float  op    = 0.90f;

    SECTION("translucent material, mid TD")
    {
        Material m = mat_with_td(0.5f); // td_mm = 0.5 * 0.2 = 0.10 mm
        const double t_mm = min_thickness_for_opacity_mm(m, 0, op, ref_h);
        REQUIRE(t_mm > 0.0);

        // slice_opacity works in ratio units: ratio = t_mm / ref_h.
        float out_op[3];
        slice_opacity(m, float(t_mm / ref_h), out_op);
        REQUIRE_THAT(out_op[0], WithinAbs(op, 1e-4));
    }

    SECTION("opaque material (td ~ 0) needs no thickness")
    {
        Material m = mat_with_td(0.f);
        const double t_mm = min_thickness_for_opacity_mm(m, 0, op, ref_h);
        REQUIRE_THAT(t_mm, WithinAbs(0.0, 1e-9));
    }

    SECTION("higher TD (more translucent) needs proportionally more thickness")
    {
        const double t_lo = min_thickness_for_opacity_mm(mat_with_td(0.3f), 0, op, ref_h);
        const double t_hi = min_thickness_for_opacity_mm(mat_with_td(0.6f), 0, op, ref_h);
        // TD doubled → required thickness doubles (linear in td).
        REQUIRE_THAT(t_hi, WithinRel(2.0 * t_lo, 1e-4));
    }
}

TEST_CASE("envelope passthrough when no color painted", "[ColorHeightEnvelope]")
{
    ColorHeightContext ctx;
    ctx.painted_tools.clear();
    ctx.nozzle_min_height_mm = 0.04;
    ctx.nozzle_max_height_mm = 0.28;
    ctx.td_reference_height_mm = 0.2;

    const ColorHeightEnvelope env = compute_color_height_envelope(ctx);
    REQUIRE(env.passthrough);
    REQUIRE_FALSE(env.conflict);
    REQUIRE_THAT(env.h_min, WithinAbs(0.04, 1e-9));
    REQUIRE_THAT(env.h_max, WithinAbs(0.28, 1e-9));
}

TEST_CASE("envelope floor rises with translucency", "[ColorHeightEnvelope]")
{
    ColorHeightContext ctx;
    ctx.painted_tools = { 0 };
    ctx.mats[0] = mat_with_td(0.5f);
    ctx.td_reference_height_mm = 0.2;
    ctx.nozzle_min_height_mm = 0.04;
    ctx.nozzle_max_height_mm = 0.30;

    // mix_band_upper high enough not to be the binding constraint here.
    const ColorHeightEnvelope env = compute_color_height_envelope(ctx, 0.90f, 0.30);
    REQUIRE_FALSE(env.passthrough);

    // Fidelity floor must be the analytic thickness for 90% opacity.
    const double expected_floor = min_thickness_for_opacity_mm(ctx.mats[0], 0, 0.90f, 0.2);
    REQUIRE_THAT(env.h_min, WithinAbs(std::max(0.04, expected_floor), 1e-4));
    REQUIRE(env.h_min > 0.04); // translucent → floor above nozzle min
    REQUIRE(env.h_opt <= env.h_max + 1e-9);
    REQUIRE(env.h_opt >= env.h_min - 1e-9);
}

TEST_CASE("worst-case tool drives the floor among several painted tools", "[ColorHeightEnvelope]")
{
    ColorHeightContext ctx;
    ctx.painted_tools = { 0, 1 };
    ctx.mats[0] = mat_with_td(0.2f); // fairly opaque
    ctx.mats[1] = mat_with_td(0.6f); // most translucent → should win
    ctx.td_reference_height_mm = 0.2;
    ctx.nozzle_min_height_mm = 0.04;
    ctx.nozzle_max_height_mm = 0.40;

    const ColorHeightEnvelope env = compute_color_height_envelope(ctx, 0.90f, 0.40);
    const double floor_translucent = min_thickness_for_opacity_mm(ctx.mats[1], 0, 0.90f, 0.2);
    REQUIRE_THAT(env.h_min, WithinAbs(floor_translucent, 1e-4));
}

TEST_CASE("sandwich passes reserve top-surface height", "[ColorHeightEnvelope]")
{
    ColorHeightContext ctx;
    ctx.painted_tools = { 0 };
    ctx.mats[0] = mat_with_td(0.05f); // near-opaque → fidelity floor tiny
    ctx.td_reference_height_mm = 0.2;
    ctx.nozzle_min_height_mm = 0.04;
    ctx.nozzle_max_height_mm = 0.40;
    ctx.sandwich_passes = 3;          // 3-pass auto-sandwich on this top surface
    ctx.min_pass_height_mm = 0.04;
    ctx.min_top_visible_mm = 0.05;

    const ColorHeightEnvelope env = compute_color_height_envelope(ctx, 0.90f, 0.40);
    // (3-1) fillers * 0.04 + 1 visible * 0.05 = 0.13 mm minimum.
    REQUIRE_THAT(env.h_min, WithinAbs(0.13, 1e-4));
}

TEST_CASE("conflict when fidelity exceeds the pattern ceiling", "[ColorHeightEnvelope]")
{
    ColorHeightContext ctx;
    ctx.painted_tools = { 0 };
    ctx.mats[0] = mat_with_td(1.0f); // very translucent → large floor
    ctx.td_reference_height_mm = 0.2;
    ctx.nozzle_min_height_mm = 0.04;
    ctx.nozzle_max_height_mm = 0.40;

    // Tiny mix band ceiling forces fidelity floor > ceiling → conflict.
    const ColorHeightEnvelope env = compute_color_height_envelope(ctx, 0.95f, 0.08);
    REQUIRE(env.conflict);
    // Fidelity wins: h_max opened up to h_min (clamped to nozzle max).
    REQUIRE_THAT(env.h_max, WithinAbs(env.h_min, 1e-6));
    REQUIRE(env.h_max <= ctx.nozzle_max_height_mm + 1e-9);
}
