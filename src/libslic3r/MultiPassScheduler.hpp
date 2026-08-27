#ifndef slic3r_MultiPassScheduler_hpp_
#define slic3r_MultiPassScheduler_hpp_

// NEOTKO_MPSCHEDULER_TAG — s79: canonical tool-grouping order for sandwich sublayers.
//
// Problem: the sandwich (MultiPass/PathBlend/ColorStitch, emitted as MultiPassSubLayer
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
#include <cstdlib>
#include <vector>

namespace Slic3r {
namespace MultiPassScheduler {

// NEOTKO_MPSCHEDULER_TAG s282b — THE chain identity. One function, called by all
// three sites (GCode.cpp process_layer, NeoTower.cpp collect_all_events, NeoTower.cpp
// step5). Those three used to repeat the arithmetic inline with comments begging the
// next reader not to let them diverge; now divergence is impossible.
//
// WHY island_id exists. A chain is a HARD sequential dependency: the round-robin in
// order_sublayers_by_tool only ever sees the HEAD of each chain, everything behind it
// waits its turn by pass_idx. That is correct when a chain is a real stack, because
// the cap must print on top of its own ramp.
//
// The key used to be (object, layer) alone, whose premise is "one object in one layer
// is one stack". Assemble breaks that premise: five letters welded into one Assembly
// are one object, spatially disjoint, stacked on nothing. They collapsed into ONE
// chain, and because their PathBlend recipes carry opposite tool orders the scheduler
// had no legal move except to alternate. Measured on PathBlend-Angle.3mf: 132 tool
// changes at z=1.45 assembled, 3 for the same file split into five objects — with the
// split carrying TWICE the sublayers. The giant wipe tower was only the bill for those
// 132 purges, never a wipe-tower fault.
//
// island_id (the painted slot tag) restores the premise: same slot = same stack =
// ramp before cap; different slot = disjoint XY = free to reorder. Passing 0
// everywhere reproduces the old single-chain behaviour exactly.
//
// ORCA_PB_ISLAND_CHAINS=0 forces that old behaviour at runtime for A/B in one build.
inline bool island_chains_enabled()
{
    static const bool v = [] {
        if (const char* e = std::getenv("ORCA_PB_ISLAND_CHAINS"))
            return !(e[0] == '0' && e[1] == '\0');
        return true;
    }();
    return v;
}

inline uint64_t make_chain_key(const void* obj, int layer_id, int island_id)
{
    uint64_t h = (uint64_t)(uintptr_t)obj * 1000003ull + (uint64_t)(uint32_t)layer_id;
    if (island_chains_enabled())
        h ^= (uint64_t)(uint32_t)(island_id + 1) * 0x9E3779B97F4A7C15ull;
    return h;
}

// One real sublayer (emission unit) of a single printed layer (one z_nominal group).
struct SublayerKey {
    uint64_t chain_key = 0;  // identity of the stacking chain — ALWAYS via make_chain_key()
    int      pass_idx  = 0;  // 0-based position in the chain (causal: pass0 before pass1)
    int      tool_id   = 0;  // 0-based physical extruder this sublayer prints with
    double   z_actual  = 0.; // sub.print_z — deterministic tie-break within a tool group
    // NEOTKO_PATHBLEND_TAG — s88. When true, the scheduler drains every
    // CONSECUTIVE same-tool entry of this chain in one shot before moving on
    // to the next chain. Used by per-scanline PathBlend so the rampa of one
    // object completes before the rampa of the next — avoids the cross-object
    // micro-travels seen in the multi-cube preview. Toolchange boundaries
    // still split the chain (rampa T_bottom drained per chain, then tapa
    // T_top drained across chains).
    bool     atomic_chain = false;
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
                if (c.next < c.idxs.size() && c.idxs[c.next] == ri) {
                    ++c.next;
                    // NEOTKO_PATHBLEND_TAG — s88 atomic chain drain. If THIS
                    // sublayer is atomic, keep consuming consecutive same-tool
                    // siblings from its chain BEFORE returning control to the
                    // multi-chain round-robin. Effect: a per-scanline PathBlend
                    // rampa of one object emits fully before the next object's
                    // rampa starts — eliminates cross-object micro-travels.
                    // Tool boundary inside the chain (e.g. rampa→tapa) still
                    // breaks the drain naturally (tool_id != chosen).
                    while (items[ri].atomic_chain
                           && c.next < c.idxs.size()
                           && items[c.idxs[c.next]].tool_id == chosen) {
                        const size_t extra = c.idxs[c.next];
                        order.push_back(extra);
                        ++emitted;
                        ++c.next;
                    }
                    break;
                }
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

// NEOTKO_NEOTOWER_TAG s102 — per-process-layer-call replay.
//
// GCode reorders sublayers once per process_layer call, and
// collect_layers_to_print partitions entries into Z windows
// [z_first, z_first + eps] (eps = Slic3r EPSILON = 1e-4, GCode.cpp ~2199
// "Merge numerically very close Z values"). Consequences:
//   - Classic MP planes (~0.001 apart)        → one window per plane.
//   - PathBlend scanline chains (1e-7 apart)  → a single window, so the
//     s88/s89 atomic-chain drain semantics are preserved bit-for-bit.
//
// NeoTower previously replayed the WHOLE z_nominal group in one
// order_sublayers_by_tool call (s89). For multi-plane classic MP groups that
// diverges from GCode's per-call ordering: the predicted group-exit tool (and
// therefore the real-layer rotation and the real TC pair) can differ from what
// the writer actually does — s102 finding: NeoTower planned the real layer as
// [T1,T0]/TC 1→0 while GCode emitted [T0,T1]/TC 0→1, so the canonical frame
// TCR was never dispatched (missing brim at z=0.88) and unpredicted
// transitions inside multi-plane groups produced get_tcr MISSes.
//
// This helper reproduces GCode's effective behaviour: greedy windows over
// ascending z_actual, each window ordered by order_sublayers_by_tool with the
// running tool chained across windows (the writer carries over between
// process_layer calls). Returns the concatenated permutation (indices into
// `items`). With a single window it degenerates to order_sublayers_by_tool —
// identical output to the pre-s102 global call.
inline std::vector<size_t> order_sublayers_by_tool_windowed(
    const std::vector<SublayerKey>& items, int initial_tool, double window_eps)
{
    std::vector<size_t> out;
    const size_t n = items.size();
    if (n == 0)
        return out;
    out.reserve(n);

    // Indices sorted by z_actual; stable so equal-z items keep their original
    // relative order (mirrors collect_layers_to_print's stable z sort).
    std::vector<size_t> by_z(n);
    for (size_t i = 0; i < n; ++i)
        by_z[i] = i;
    std::stable_sort(by_z.begin(), by_z.end(), [&](size_t a, size_t b) {
        return items[a].z_actual < items[b].z_actual;
    });

    int running = initial_tool;
    size_t i = 0;
    while (i < n) {
        const double zmax = items[by_z[i]].z_actual + window_eps;
        size_t j = i + 1;
        while (j < n && items[by_z[j]].z_actual <= zmax)
            ++j;
        std::vector<SublayerKey> wnd;
        wnd.reserve(j - i);
        for (size_t k = i; k < j; ++k)
            wnd.push_back(items[by_z[k]]);
        const std::vector<size_t> ord = order_sublayers_by_tool(wnd, running);
        for (size_t oi : ord)
            out.push_back(by_z[i + oi]);
        if (!ord.empty())
            running = wnd[ord.back()].tool_id;
        i = j;
    }
    return out;
}

} // namespace MultiPassScheduler
} // namespace Slic3r

#endif // slic3r_MultiPassScheduler_hpp_
