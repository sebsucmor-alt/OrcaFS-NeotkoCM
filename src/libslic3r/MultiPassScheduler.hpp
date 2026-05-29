#ifndef slic3r_MultiPassScheduler_hpp_
#define slic3r_MultiPassScheduler_hpp_

// NEOTKO_MPSCHEDULER_TAG — s79: canonical tool-grouping order for sandwich sublayers.
//
// Problem: the sandwich (MultiPass/PathBlend/ColorMix, emitted as MultiPassSubLayer
// entries) is dispatched in object-major order, so when two objects assign inverted
// tools per pass the sequence alternates T0→T1→T0→T1 (one wipe per toolchange).
//
// This header provides ONE deterministic ordering function, called identically by:
//   - GCode emission (GCode.cpp process_layer, the `ltps_sorted` sublayer group), and
//   - NeoTower plan (NeoTower.cpp collect_all_events, sublayer scheduler),
// so plan == emission by construction (the established "espejo" pattern, s74/s77).
//
// It groups sublayers by tool while respecting per-chain Z-stacking: within one chain
// (one object's stacked sublayers at the same XY) pass N must print before pass N+1.
// Across chains (independent objects) work of the same tool is merged behind a single
// toolchange — mirror of `choose_ready_extruder` (the dependency_chain_mode scheduler).
//
// Pure algorithm, header-only, no heavy deps (matches LocalZOrderOptimizer.hpp).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Slic3r {
namespace MultiPassScheduler {

// One real sublayer (emission unit) of a single printed layer (one z_nominal group).
struct SublayerKey {
    uint64_t chain_key = 0;  // identity of the stacking chain: hash of (object, layer_id)
    int      pass_idx  = 0;  // 0-based position in the chain (causal: pass0 before pass1)
    int      tool_id   = 0;  // 0-based physical extruder this sublayer prints with
    double   z_actual  = 0.; // sub.print_z — deterministic tie-break within a tool group
};

// Returns a permutation (indices into `items`) in canonical grouped-by-tool emission
// order. Deterministic: identical `items` + `initial_tool` always yield the same order.
//
// Algorithm (greedy, mirror of choose_ready_extruder):
//   1. Build chains by chain_key; within a chain order by pass_idx.
//   2. ready = first not-yet-emitted item of each chain.
//   3. If `current_tool` has ready items, keep it (zero toolchange); else pick the tool
//      maximizing (ready_count, then future_count, then lowest tool_id).
//   4. Emit ALL ready items of the chosen tool (ordered by z_actual, then chain_key,
//      then original index), unlock their chain successors, set current_tool = chosen.
//   5. Repeat until every item is emitted.
inline std::vector<size_t> order_sublayers_by_tool(const std::vector<SublayerKey> &items,
                                                   int                             initial_tool)
{
    const size_t n = items.size();
    std::vector<size_t> order;
    order.reserve(n);
    if (n == 0)
        return order;

    // Stable original index, used as the final deterministic tie-break.
    std::vector<size_t> by_orig(n);
    for (size_t i = 0; i < n; ++i)
        by_orig[i] = i;

    // Group indices into chains keyed by chain_key, each ordered by pass_idx (then z,
    // then original index) so the causal stacking order is unambiguous.
    struct Chain {
        std::vector<size_t> idxs; // item indices, ascending by pass_idx
        size_t              next = 0; // position of the next not-yet-emitted item
    };
    std::vector<Chain> chains;
    {
        // chain_key → chains index (linear scan keeps it dependency-free and is fine
        // for the handful of objects in a layer).
        std::vector<uint64_t> seen_keys;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t key = items[i].chain_key;
            size_t ci = seen_keys.size();
            for (size_t k = 0; k < seen_keys.size(); ++k)
                if (seen_keys[k] == key) { ci = k; break; }
            if (ci == seen_keys.size()) {
                seen_keys.push_back(key);
                chains.emplace_back();
            }
            chains[ci].idxs.push_back(i);
        }
        for (Chain &c : chains) {
            std::sort(c.idxs.begin(), c.idxs.end(), [&](size_t a, size_t b) {
                if (items[a].pass_idx != items[b].pass_idx) return items[a].pass_idx < items[b].pass_idx;
                if (items[a].z_actual != items[b].z_actual) return items[a].z_actual < items[b].z_actual;
                return a < b;
            });
        }
    }

    auto ready_indices = [&]() {
        std::vector<size_t> r;
        for (const Chain &c : chains)
            if (c.next < c.idxs.size())
                r.push_back(c.idxs[c.next]);
        return r;
    };

    int current_tool = initial_tool;
    size_t emitted = 0;
    while (emitted < n) {
        std::vector<size_t> ready = ready_indices();
        if (ready.empty())
            break; // defensive: should not happen (every chain drains)

        // Does current_tool serve any ready item?
        bool current_has_ready = false;
        for (size_t ri : ready)
            if (items[ri].tool_id == current_tool) { current_has_ready = true; break; }

        int chosen = current_tool;
        if (!current_has_ready) {
            // Choose the tool maximizing (ready_count, future_count, -tool_id).
            // future_count = ready + still-locked items of that tool across all chains.
            size_t best_ready = 0, best_future = 0;
            int    best_tool  = -1;
            // Candidate tools = distinct tools among ready items.
            std::vector<int> cand;
            for (size_t ri : ready) {
                const int t = items[ri].tool_id;
                if (std::find(cand.begin(), cand.end(), t) == cand.end())
                    cand.push_back(t);
            }
            std::sort(cand.begin(), cand.end());
            for (int t : cand) {
                size_t rc = 0, fc = 0;
                for (size_t ri : ready)
                    if (items[ri].tool_id == t) ++rc;
                for (size_t i = 0; i < n; ++i)
                    if (items[i].tool_id == t) ++fc; // total demand for this tool (proxy)
                if (best_tool < 0 || rc > best_ready ||
                    (rc == best_ready && fc > best_future) ||
                    (rc == best_ready && fc == best_future && t < best_tool)) {
                    best_ready = rc; best_future = fc; best_tool = t;
                }
            }
            chosen = best_tool;
        }

        // Collect ready items of `chosen`, ordered deterministically.
        std::vector<size_t> batch;
        for (size_t ri : ready)
            if (items[ri].tool_id == chosen)
                batch.push_back(ri);
        std::sort(batch.begin(), batch.end(), [&](size_t a, size_t b) {
            if (items[a].z_actual != items[b].z_actual) return items[a].z_actual < items[b].z_actual;
            if (items[a].chain_key != items[b].chain_key) return items[a].chain_key < items[b].chain_key;
            return a < b;
        });

        for (size_t ri : batch) {
            order.push_back(ri);
            ++emitted;
            // Advance the owning chain.
            for (Chain &c : chains) {
                if (c.next < c.idxs.size() && c.idxs[c.next] == ri) { ++c.next; break; }
            }
        }
        current_tool = chosen;
    }

    // Defensive: append any leftovers (should be none) so the permutation is complete.
    if (order.size() < n) {
        std::vector<char> in_order(n, 0);
        for (size_t idx : order) in_order[idx] = 1;
        for (size_t i = 0; i < n; ++i)
            if (!in_order[i]) order.push_back(i);
    }

    return order;
}

} // namespace MultiPassScheduler
} // namespace Slic3r

#endif // slic3r_MultiPassScheduler_hpp_
