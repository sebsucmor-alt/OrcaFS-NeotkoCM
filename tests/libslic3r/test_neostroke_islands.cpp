// NEOTKO_NEOSTROKE_TAG s342 — ajustes por isla: lector y escritor de `neostroke_island_overrides`.
// Upstream Snapmaker #749: migrado de Catch2 v2 a v3.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
using Catch::Approx;

#include "libslic3r/NeoArachne/NeoStrokeIslands.hpp"

using namespace Slic3r::NeoArachne;

TEST_CASE("NeoStroke island overrides: round trip", "[NeoStroke][islands]")
{
    std::vector<NsIslandOverride> in(2);
    in[0].name = "A";
    in[0].x    = 12.5;
    in[0].y    = -3.25;
    in[0].set("neostroke_band_mm", "1.2");
    in[0].set("neostroke_variable_k", "0");
    in[1].name = "Island 2";
    in[1].x    = 0.;
    in[1].y    = 40.;

    const std::string text = write_island_overrides(in);
    size_t skipped = 99;
    const std::vector<NsIslandOverride> out = parse_island_overrides(text, &skipped);

    REQUIRE(skipped == 0);
    REQUIRE(out.size() == 2);
    CHECK(out[0].name == "A");
    CHECK(out[0].x == Approx(12.5));
    CHECK(out[0].y == Approx(-3.25));
    REQUIRE(out[0].values.size() == 2);
    REQUIRE(out[0].find("neostroke_band_mm") != nullptr);
    CHECK(*out[0].find("neostroke_band_mm") == "1.2");
    CHECK(out[1].name == "Island 2");
    CHECK(out[1].values.empty());
    CHECK(write_island_overrides(out) == text);
}

TEST_CASE("NeoStroke island overrides: tolerant reader", "[NeoStroke][islands]")
{
    size_t skipped = 0;
    const std::string text =
        "\n"
        "B|1|2|neostroke_band_mm=0.8;neostroke_skate=1;basura\n"   // skate es del objeto; «basura» no es clave
        "mal|x|2|neostroke_band_mm=1\n"                             // x no es un número
        "corta|1\n"                                                 // faltan campos
        "C|3|4\n";
    const std::vector<NsIslandOverride> out = parse_island_overrides(text, &skipped);
    REQUIRE(out.size() == 2);
    CHECK(out[0].name == "B");
    CHECK(out[0].values.size() == 1);
    CHECK(out[0].find("neostroke_skate") == nullptr);
    CHECK(out[1].name == "C");
    CHECK(skipped == 4);
}

TEST_CASE("NeoStroke island overrides: names cannot break the format", "[NeoStroke][islands]")
{
    std::vector<NsIslandOverride> in(1);
    in[0].name = "a|b;c=d\ne";
    in[0].set("neostroke_lead_in", "0.4");
    const std::vector<NsIslandOverride> out = parse_island_overrides(write_island_overrides(in));
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "abcde");
    CHECK(out[0].values.size() == 1);
}

TEST_CASE("NeoStroke island overrides: set and erase", "[NeoStroke][islands]")
{
    NsIslandOverride o;
    o.set("neostroke_band_mm", "1");
    o.set("neostroke_band_mm", "2");
    REQUIRE(o.values.size() == 1);
    CHECK(*o.find("neostroke_band_mm") == "2");
    o.erase("neostroke_band_mm");
    CHECK(o.values.empty());
    CHECK(is_island_override_key("neostroke_lead_in"));
    CHECK_FALSE(is_island_override_key("neostroke_width_ref"));
}
