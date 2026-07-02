#include <catch2/catch.hpp>

#include "libslic3r/ColorSci/ColorSci.hpp"

#include <array>
#include <cmath>
#include <vector>

// NEOTKO_REALCOLOR_TAG: closes the "front-to-back vs bottom-to-top" open question from
// docs/WIP/REALCOLOR_VIEW/04_SHADERS_BEER_LAMBERT.md before trusting realcolor_accum.fs.
//
// ColorSci::blend_stacked() composites layers bottom-to-top (back-to-front from an overhead
// camera), recurrence: acc = layer*(1-t) + acc*t, starting acc = bg.
//
// The GPU depth-peel pass instead produces layers front-to-back (nearest camera first), and
// realcolor_accum.fs accumulates with the standard front-to-back OVER formula:
//   acc += T*(1-t)*layer;  T *= t;    (acc=0, T=1 initially)
//   final = acc + T*bg
//
// This test proves both formulas give the same final color for the same physical stack, so
// the front-to-back GPU accumulate is a correct (not just plausible) port of blend_stacked.

using namespace Slic3r;

namespace {

// C++ mirror of realcolor_accum.fs's Beer-Lambert branch — front-to-back OVER accumulate.
// `layers_front_to_back[0]` = nearest camera ... `.back()` = farthest (bed-ward).
void blend_front_to_back(const std::vector<ColorSci::Layer>& layers_front_to_back,
                          const float bg_rgb[3], float out_rgb[3])
{
    float acc[3] = { 0.f, 0.f, 0.f };
    float T[3] = { 1.f, 1.f, 1.f };
    for (const ColorSci::Layer& lyr : layers_front_to_back) {
        for (int c = 0; c < 3; ++c) {
            const float lc = ColorSci::srgb_to_linear(lyr.rgb[c]);
            const float t = (lyr.td[c] < 1e-6f) ? 0.f : std::pow(0.1f, lyr.ratio / lyr.td[c]);
            acc[c] += T[c] * (1.f - t) * lc;
            T[c] *= t;
        }
    }
    for (int c = 0; c < 3; ++c)
        out_rgb[c] = ColorSci::linear_to_srgb(acc[c] + T[c] * ColorSci::srgb_to_linear(bg_rgb[c]));
}

ColorSci::Layer make_layer(float r, float g, float b, float td, float ratio)
{
    ColorSci::Layer lyr;
    lyr.rgb = { r, g, b };
    lyr.td = { td, td, td };
    lyr.ratio = ratio;
    return lyr;
}

} // namespace

TEST_CASE("RealColor front-to-back accumulate matches ColorSci::blend_stacked", "[RealColor][ColorSci]")
{
    using WithinAbs;

    const float black_bg[3] = { 0.f, 0.f, 0.f };

    SECTION("2 translucent layers, same TD")
    {
        // bottom-to-top physical order: red bottom, blue top
        std::vector<ColorSci::Layer> bottom_to_top = {
            make_layer(0.8f, 0.1f, 0.1f, 1.5f, 0.2f),
            make_layer(0.1f, 0.1f, 0.8f, 1.5f, 0.2f),
        };
        std::vector<ColorSci::Layer> front_to_back = { bottom_to_top[1], bottom_to_top[0] };

        float expected[3];
        ColorSci::blend_stacked(bottom_to_top, black_bg, expected);

        float actual[3];
        blend_front_to_back(front_to_back, black_bg, actual);

        for (int c = 0; c < 3; ++c)
            REQUIRE_THAT(actual[c], WithinAbs(expected[c], 1e-4f));
    }

    SECTION("3 layers, varying TD and thickness")
    {
        std::vector<ColorSci::Layer> bottom_to_top = {
            make_layer(0.9f, 0.9f, 0.2f, 0.6f, 0.15f),
            make_layer(0.2f, 0.7f, 0.9f, 2.0f, 0.3f),
            make_layer(0.5f, 0.1f, 0.5f, 1.0f, 0.1f),
        };
        std::vector<ColorSci::Layer> front_to_back = { bottom_to_top[2], bottom_to_top[1], bottom_to_top[0] };

        float expected[3];
        ColorSci::blend_stacked(bottom_to_top, black_bg, expected);

        float actual[3];
        blend_front_to_back(front_to_back, black_bg, actual);

        for (int c = 0; c < 3; ++c)
            REQUIRE_THAT(actual[c], WithinAbs(expected[c], 1e-4f));
    }

    SECTION("4 layers including an opaque one (td below the 1e-6 clamp)")
    {
        std::vector<ColorSci::Layer> bottom_to_top = {
            make_layer(0.3f, 0.3f, 0.9f, 1.2f, 0.2f),
            make_layer(1.0f, 0.5f, 0.0f, 0.f /* opaque */, 0.2f),
            make_layer(0.1f, 0.9f, 0.1f, 3.0f, 0.25f),
            make_layer(0.9f, 0.9f, 0.9f, 0.8f, 0.18f),
        };
        std::vector<ColorSci::Layer> front_to_back = { bottom_to_top[3], bottom_to_top[2], bottom_to_top[1], bottom_to_top[0] };

        float expected[3];
        ColorSci::blend_stacked(bottom_to_top, black_bg, expected);

        float actual[3];
        blend_front_to_back(front_to_back, black_bg, actual);

        for (int c = 0; c < 3; ++c)
            REQUIRE_THAT(actual[c], WithinAbs(expected[c], 1e-4f));

        // the opaque layer is 2nd-from-bottom: everything below it must be fully hidden —
        // equivalent to blend_stacked() on just the 2 layers above (+opaque) it.
        std::vector<ColorSci::Layer> from_opaque_up = { bottom_to_top[1], bottom_to_top[2], bottom_to_top[3] };
        float expected_from_opaque[3];
        ColorSci::blend_stacked(from_opaque_up, black_bg, expected_from_opaque);
        for (int c = 0; c < 3; ++c)
            REQUIRE_THAT(expected[c], WithinAbs(expected_from_opaque[c], 1e-5f));
    }

    SECTION("single fully-opaque layer reduces to its own color")
    {
        std::vector<ColorSci::Layer> stack = { make_layer(0.4f, 0.6f, 0.2f, 0.f, 0.2f) };

        float expected[3];
        ColorSci::blend_stacked(stack, black_bg, expected);
        REQUIRE_THAT(expected[0], WithinAbs(0.4f, 1e-5f));
        REQUIRE_THAT(expected[1], WithinAbs(0.6f, 1e-5f));
        REQUIRE_THAT(expected[2], WithinAbs(0.2f, 1e-5f));

        float actual[3];
        blend_front_to_back(stack, black_bg, actual);
        for (int c = 0; c < 3; ++c)
            REQUIRE_THAT(actual[c], WithinAbs(expected[c], 1e-5f));
    }
}
