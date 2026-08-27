#include <catch2/catch.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

// Bloque A-bis — see docs/FUTURE/COLORMIX_VIRTUAL_REMAP_ABIS.md
//
// The identity of a virtual filament inside a Sandwich/ColorStitch recipe is its ORDINAL
// POSITION among the live rows, encoded as a single digit '5'..'9' inside a pattern string.
// Nothing stable is stored in the recipe itself. These tests pin down the three defects the
// study found, so that the remap work has a red/green signal to work against.

namespace {

static const std::vector<std::string> k_colours = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
static constexpr size_t               k_num_physical = 4;

// Build a manager with `count` custom virtual rows, each with a distinct stable_id.
static MixedFilamentManager make_manager(size_t count)
{
    MixedFilamentManager mgr;
    for (size_t i = 0; i < count; ++i) {
        mgr.add_custom_filament(1, 2, 50, k_colours);
        MixedFilament &row = mgr.mixed_filaments().back();
        row.stable_id      = uint64_t(1000 + i);
    }
    return mgr;
}

} // namespace

// VREN-02 — the three predicates that decide "this row is a live virtual" must agree.
// Regression for the sidebar Delete button, which used to set `deleted` without clearing
// `enabled`, giving the row a slot in build_filament_id_remap() that the engine never gave it
// and shifting every virtual above it by one.
TEST_CASE("VREN-02 live-row predicate is consistent across counter, engine and remap", "[ColorMixRemap]")
{
    MixedFilamentManager mgr  = make_manager(3);
    auto                &rows = mgr.mixed_filaments();
    REQUIRE(rows.size() == 3);
    REQUIRE(mgr.enabled_count() == 3);

    SECTION("a properly deleted row drops out of every enumeration")
    {
        rows[0].deleted = true;
        rows[0].enabled = false;
        CHECK(mgr.enabled_count() == 2);
        // The first live row is now rows[1], and it must answer to the first virtual id.
        CHECK(mgr.mixed_index_from_filament_id(unsigned(k_num_physical + 1), k_num_physical) == 1);
    }

    SECTION("a row marked deleted but left enabled must behave identically")
    {
        // This is exactly the state the sidebar Delete button used to leave behind.
        rows[0].deleted = true;
        CHECK_FALSE(rows[0].is_live());
        CHECK(mgr.enabled_count() == 2);
        CHECK(mgr.mixed_index_from_filament_id(unsigned(k_num_physical + 1), k_num_physical) == 1);
    }

    SECTION("is_live() is the single owner of the predicate")
    {
        rows[1].enabled = false;
        CHECK_FALSE(rows[1].is_live());
        rows[1].enabled = true;
        rows[1].deleted = true;
        CHECK_FALSE(rows[1].is_live());
        rows[1].deleted = false;
        CHECK(rows[1].is_live());
    }
}

// VREN-01 — disabling a virtual row renumbers every virtual above it, and recipes keep naming
// the old digit. EXPECTED TO FAIL until the recipe remap of A-bis §7 exists: this is the repro.
TEST_CASE("VREN-01 disabling a virtual renumbers the ones above it", "[ColorMixRemap][!mayfail]")
{
    MixedFilamentManager mgr  = make_manager(3);
    auto                &rows = mgr.mixed_filaments();

    // Recipe digit '7' == filament id 7 == third live virtual (4 physical + 3).
    const unsigned int recipe_id = 7;
    const int          before    = mgr.mixed_index_from_filament_id(recipe_id, k_num_physical);
    REQUIRE(before == 2);
    const uint64_t pointed_at = rows[size_t(before)].stable_id;

    // The user disables the FIRST virtual. Nothing about the third row changed.
    rows[0].enabled = false;

    const int after = mgr.mixed_index_from_filament_id(recipe_id, k_num_physical);
    INFO("recipe digit '7' now resolves to row index " << after << " instead of " << before);
    // The recipe still says '7', so it must still mean the same physical row.
    CHECK(after >= 0);
    CHECK(rows[size_t(after)].stable_id == pointed_at);
}

// VREN-03 — the recipe alphabet is a single digit '5'..'9', so only 5 virtuals are expressible
// while the UI allows up to 60. The 6th onwards is unreachable and nothing warns.
// EXPECTED TO FAIL until A-bis §9 decision 2 is taken (UI guard or wider alphabet).
TEST_CASE("VREN-03 virtuals beyond the fifth are not representable in a recipe", "[ColorMixRemap][!mayfail]")
{
    MixedFilamentManager mgr = make_manager(6);
    REQUIRE(mgr.enabled_count() == 6);

    // The first five live virtuals map onto digits '5'..'9'.
    for (unsigned int i = 0; i < 5; ++i) {
        const unsigned int filament_id = unsigned(k_num_physical) + 1 + i;
        REQUIRE(filament_id <= 9);
        CHECK(mgr.mixed_index_from_filament_id(filament_id, k_num_physical) == int(i));
    }

    // The sixth resolves fine internally...
    const unsigned int sixth_id = unsigned(k_num_physical) + 6;
    CHECK(mgr.mixed_index_from_filament_id(sixth_id, k_num_physical) == 5);
    // ...but it needs two characters, so no pattern string can name it.
    INFO("sixth virtual has filament id " << sixth_id << ", which does not fit in one digit");
    CHECK(sixth_id <= 9);
}
