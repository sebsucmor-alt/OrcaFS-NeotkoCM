// NEOTKO_NEOTOWER_TAG_START — hardening P2
// Single source of truth for Z epsilons across MultiPass / NeoTower / WipeTower2.
//
// Invariants (validated by static_assert at the bottom):
//   NOMINAL_LH_MIN  > SUBLAYER_GAP                  (real layers can't fuse)
//   SUBLAYER_GAP    > Z_EPS_PLAN  > Z_EPS_GROUP     (sublayer↔real stay separate)
//   Z_EPS_FUTURE_TC < NOMINAL_LH_MIN                (suppress doesn't trigger
//                                                    between real layers)
//
// Reference values:
//   - Minimum real LH:           0.04 mm
//   - sub.print_z gap to real:   0.0002 mm (= 2 * EPSILON, Fill.cpp:1469)
#ifndef NEOTOWER_Z_HPP
#define NEOTOWER_Z_HPP

#include <cstdint>
#include <cmath>

namespace Slic3r {
namespace NeoTowerZ {

constexpr float NOMINAL_LH_MIN = 0.04f;

// Distance between sublayer print_z (nominal - 2*EPSILON) and parent nominal.
constexpr float SUBLAYER_GAP = 2e-4f;

// WipeTower2 plan_toolchange merge epsilon. Must be < SUBLAYER_GAP (Bug 6).
constexpr float Z_EPS_PLAN = 1e-4f;

// NeoTower collect_all_events same-z-group epsilon. Must be < Z_EPS_PLAN.
constexpr float Z_EPS_GROUP = 1e-5f;

// suppress_finish_layer_if_future_layer threshold. Must be < NOMINAL_LH_MIN
// and > SUBLAYER_GAP.
constexpr float Z_EPS_FUTURE_TC = 5e-4f;

inline int64_t to_key_um(float z) {
    return static_cast<int64_t>(std::llround(static_cast<double>(z) * 1000.0));
}

inline int64_t to_nm(float z) {
    return static_cast<int64_t>(std::llround(static_cast<double>(z) * 1e6));
}

// Invariants — compile-time validated.
static_assert(NOMINAL_LH_MIN  > SUBLAYER_GAP,
              "Real LH minimum must be larger than sublayer gap");
static_assert(SUBLAYER_GAP    > Z_EPS_PLAN,
              "Sublayer gap must exceed plan epsilon to prevent fusion (Bug 6)");
static_assert(Z_EPS_PLAN      > Z_EPS_GROUP,
              "Plan epsilon must exceed group epsilon");
static_assert(Z_EPS_FUTURE_TC < NOMINAL_LH_MIN,
              "Future-TC threshold cannot exceed minimum real LH");
static_assert(Z_EPS_FUTURE_TC > SUBLAYER_GAP,
              "Future-TC threshold must distinguish sublayer↔real gap");

} // namespace NeoTowerZ
} // namespace Slic3r

#endif // NEOTOWER_Z_HPP
// NEOTKO_NEOTOWER_TAG_END
