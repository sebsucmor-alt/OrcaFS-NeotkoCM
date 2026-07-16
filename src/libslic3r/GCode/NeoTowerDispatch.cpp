// NEOTKO_NEOTOWER_TAG s205-5b.3a — fork-owned dispatch, extracted VERBATIM from
// GCode.cpp WipeTowerIntegration::tool_change() (the NeoTower real-layer dispatch
// branches). Behaviour-preserving move: the keyed get_tcr lookup, the get_tcr-MISS
// planned-slot fallback, the s136 realign guard, the s160b Z-crash guard and the
// s205-5b.2b shadow records are IDENTICAL to their previous inline form — only their
// physical location changed, from GCode.cpp (upstream-shared) into this fork-owned
// translation unit. The two mirrors (non-BBL append_tcr2 / BBL append_tcr) are kept
// as SEPARATE methods on purpose (NOT unified behind a flag): they differ subtly and
// a "clever" merge is exactly where a divergence bug would be born. See §8.5 of
// docs/FUTURE/NEOTOWER_REFACTOR_PLAN.md. Sublayer-prime dispatch, structural
// (get_finish_layer) dispatch and the stock (non-NeoTower) else branches stay in
// GCode.cpp — they are either already Z-keyed (structural) or upstream code.

#include "GCode.hpp"           // WipeTowerIntegration, GCode, NeoTower (via GCode.hpp)
#include "SurfaceColorMix.hpp" // NEOTKO_LOG / NeoDebug (brings <limits>)

#include <algorithm>
#include <string>
#include <optional>
#include <boost/log/trivial.hpp> // NEOTKO_NEOTOWER_TAG s205-5b.3b — structural/orphan warnings

namespace Slic3r {

// ---------------------------------------------------------------------------
// dispatch_neotower_real_layer_tc — non-BBL real-layer toolchange dispatch.
// Verbatim extraction of the `if (m_neo_tower != nullptr)` branch that used to live
// inline in tool_change() (non-BBL path, append_tcr2). Caller guards on m_neo_tower
// and sets m_last_wipe_tower_print_z after this returns, exactly as before.
// ---------------------------------------------------------------------------
std::string WipeTowerIntegration::dispatch_neotower_real_layer_tc(
    GCode& gcodegen, int extruder_id, double wipe_tower_z)
{
    std::string gcode;
    const float  layer_z  = (float)m_tool_changes[m_layer_idx][m_tool_change_idx].print_z;
    const size_t cur_tool = gcodegen.writer().extruder()
                            ? gcodegen.writer().extruder()->id()
                            : (size_t)extruder_id;
    auto      tcr_opt  = gcodegen.m_neo_tower->get_tcr(layer_z, cur_tool, (size_t)extruder_id);
    const int slot_idx = m_tool_change_idx;
    ++m_tool_change_idx; // always advance — keeps bounds in sync
    // NEOTKO s136-dbg — emission-order trace (behavior-neutral). Captures the
    // real (cur→req) toolchange order GCode requests for each real layer +
    // whether NeoTower's plan has a matching TCR (get_tcr HIT) or the dispatch
    // falls back to the planned slot (MISS → crash candidate). Compare against
    // SINGLE_TOOL_PROBE (plan entries) to localize plan↔emission divergence.
    NEOTKO_LOG(WIPETOWER, "WT_EMIT_TRACE layer=" << m_layer_idx
        << " z=" << layer_z << " slot_idx=" << slot_idx
        << " cur=T" << cur_tool << " req=T" << extruder_id
        << " get_tcr=" << (tcr_opt ? "HIT" : "MISS")
        << (tcr_opt ? (" tcr=T" + std::to_string(tcr_opt->initial_tool)
                       + "->T" + std::to_string(tcr_opt->new_tool))
                    : std::string())
        << " plan_slot=" << ((slot_idx >= 0 && slot_idx < (int)m_tool_changes[m_layer_idx].size())
            ? ("T" + std::to_string(m_tool_changes[m_layer_idx][slot_idx].initial_tool)
               + "->T" + std::to_string(m_tool_changes[m_layer_idx][slot_idx].new_tool))
            : std::string("none")));
    if (tcr_opt) {
        // s104-z plane-realign: a fused/redirected TCR prints at ITS plane,
        // not the requesting layer's z (no-op for plain multitool where
        // tcr.print_z ≈ layer_z).
        // NEOTKO_NEOTOWER_TAG s160b — Z-crash guard. The realign assumes
        // wipe_tower_z is a real plane. With wipe_tower_no_sparse_layers OFF,
        // wipe_tower_z stays the -1 sparse sentinel (append_tcr2:800 resolves
        // it to current_z). Adding (tcr.print_z - layer_z) to -1 produces a
        // bogus negative z (e.g. -1 + 0.2 = -0.8) that slips past the `z==-1`
        // guard → physical G1 Z-0.8 (bed crash). The Bottom-Surface tool
        // sequence triggers this by HITting a higher-plane TCR (get_tcr
        // returns a layer-N+1 slot for a layer-N request). Only realign off a
        // real base plane; the sentinel keeps stock sparse-layer semantics.
        // Verified by ZTRACE/WT_EMIT_TRACE: all 10 [NEG_Z_CRASH] rows are
        // ?→T3 realign HITs with wipe_tower_z==-1. s160 (the no_sparse branch
        // override) did not cover this sentinel path.
        double emit_z = wipe_tower_z;
        if (wipe_tower_z >= 0. && tcr_opt->print_z > layer_z + NeoTowerZ::Z_EPS_PLAN)
            emit_z = wipe_tower_z + ((double)tcr_opt->print_z - (double)layer_z);
        gcode += append_tcr2(gcodegen, *tcr_opt, extruder_id, emit_z);
    } else if (slot_idx >= 0 && slot_idx < (int)m_tool_changes[m_layer_idx].size()) {
        // get_tcr MISS → planned slot. Identity-request guard (fork s104):
        // for an identity request (cur==req) only emit the slot when it IS
        // that layer's identity/structural TCR; never emit a real-TC slot
        // for an identity visit (would purge with the wrong tool pair).
        const auto& fb           = m_tool_changes[m_layer_idx][slot_idx];
        const bool  identity_req = (cur_tool == (size_t)extruder_id);
        if (identity_req) {
            if (fb.initial_tool == fb.new_tool && (int)fb.new_tool == extruder_id) {
                gcode += append_tcr2(gcodegen, fb, extruder_id, wipe_tower_z);
                // NEOTKO_NEOTOWER_TAG s205-5b.2b — shadow the identity/structural
                // fallback emission (finish channel; structural slot is si=0).
                gcodegen.m_neo_tower->record_shadow_slot(false, /*from_finish=*/true, (size_t)m_layer_idx, 0);
            }
        } else if ((int)fb.new_tool == extruder_id) {
            gcode += append_tcr2(gcodegen, fb, extruder_id, wipe_tower_z);
            // NEOTKO_NEOTOWER_TAG s205-5b.2b — shadow the non-identity planned-slot
            // fallback emission (tcr channel, physical slot actually emitted).
            gcodegen.m_neo_tower->record_shadow_slot(false, /*from_finish=*/false, (size_t)m_layer_idx, (size_t)slot_idx);
        } else {
            // NEOTKO s136 — plan↔emisión rotation divergence guard. Local-Z
            // sublayer groups emit via LocalZOrderOptimizer::order_bucket_extruders
            // (ending on the tool shared with the next group), but NeoTower planned
            // the real layer via the *sandwich* mirror order_sublayers_by_tool_
            // windowed → it rotated to a different entry tool, so the positional
            // slot is the wrong tool (e.g. req=T1 but slot is T1->T0) and append_tcr2
            // throws "unexpected toolchange". The stock (non-NeoTower) path already
            // realigns by tool (realign_nominal_toolchange_idx); the NeoTower path
            // did not. Mirror that graceful realign: emit a planned TCR whose
            // new_tool == request so the tower purges to the correct tool instead of
            // crashing. NOTE: this is the crash guard / parity fix; the root fix
            // (NeoTower predicting the local-z group exit the same way emission does)
            // is tracked separately.
            auto it = std::find_if(
                m_tool_changes[m_layer_idx].begin(), m_tool_changes[m_layer_idx].end(),
                [&](const WipeTower::ToolChangeResult& c) { return (int)c.new_tool == extruder_id; });
            if (it != m_tool_changes[m_layer_idx].end()) {
                NEOTKO_LOG(WIPETOWER, "WT_REALIGN_MISS layer=" << m_layer_idx
                    << " req=T" << extruder_id
                    << " slot=T" << fb.initial_tool << "->T" << fb.new_tool
                    << " realigned=T" << it->initial_tool << "->T" << it->new_tool);
                gcode += append_tcr2(gcodegen, *it, extruder_id, wipe_tower_z);
                // NEOTKO_NEOTOWER_TAG s205-5b.2b — shadow the realign emission
                // (physical slot found = distance from layer's slot 0).
                gcodegen.m_neo_tower->record_shadow_slot(false, /*from_finish=*/false,
                    (size_t)m_layer_idx, (size_t)std::distance(m_tool_changes[m_layer_idx].begin(), it));
            } else {
                NEOTKO_LOG(WIPETOWER, "WT_REALIGN_MISS layer=" << m_layer_idx
                    << " req=T" << extruder_id
                    << " NO matching planned TCR — skipping tower purge (no crash)");
            }
        }
    }
    return gcode;
}

// ---------------------------------------------------------------------------
// dispatch_neotower_real_layer_tc_bbl — BBL real-layer toolchange dispatch.
// Verbatim extraction of the BBL mirror branch (append_tcr). Kept SEPARATE from the
// non-BBL method above by design (§8.5): the two differ (append_tcr vs append_tcr2,
// the fallback structure) and must not be merged behind a flag.
// ---------------------------------------------------------------------------
std::string WipeTowerIntegration::dispatch_neotower_real_layer_tc_bbl(
    GCode& gcodegen, int extruder_id, double wipe_tower_z)
{
    std::string gcode;
    const float  layer_z  = (float)m_tool_changes[m_layer_idx][m_tool_change_idx].print_z;
    const size_t cur_tool = gcodegen.writer().extruder()
                            ? gcodegen.writer().extruder()->id()
                            : (size_t)extruder_id;
    auto      tcr_opt  = gcodegen.m_neo_tower->get_tcr(layer_z, cur_tool, (size_t)extruder_id);
    const int slot_idx = m_tool_change_idx;
    ++m_tool_change_idx; // always advance — keeps bounds in sync
    if (tcr_opt) {
        // NEOTKO_NEOTOWER_TAG s160b — Z-crash guard (BBL mirror of the non-BBL
        // branch above). Same -1 sparse sentinel → -0.8 → G1 Z-neg crash. Only
        // realign off a real base plane; the sentinel keeps stock semantics.
        double emit_z = wipe_tower_z;
        if (wipe_tower_z >= 0. && tcr_opt->print_z > layer_z + NeoTowerZ::Z_EPS_PLAN)
            emit_z = wipe_tower_z + ((double)tcr_opt->print_z - (double)layer_z);
        gcode += append_tcr(gcodegen, *tcr_opt, extruder_id, emit_z);
    } else if (slot_idx >= 0 && slot_idx < (int)m_tool_changes[m_layer_idx].size()) {
        const auto& fb           = m_tool_changes[m_layer_idx][slot_idx];
        const bool  identity_req = (cur_tool == (size_t)extruder_id);
        const bool  is_structural = (fb.initial_tool == fb.new_tool && (int)fb.new_tool == extruder_id);
        if (!identity_req || is_structural) {
            gcode += append_tcr(gcodegen, fb, extruder_id, wipe_tower_z);
            // NEOTKO_NEOTOWER_TAG s205-5b.2b — shadow the BBL planned-slot fallback
            // (mirror of the non-BBL branch): identity→structural finish slot (si=0),
            // non-identity→physical tcr slot actually emitted.
            if (identity_req) // is_structural true here
                gcodegen.m_neo_tower->record_shadow_slot(false, /*from_finish=*/true, (size_t)m_layer_idx, 0);
            else
                gcodegen.m_neo_tower->record_shadow_slot(false, /*from_finish=*/false, (size_t)m_layer_idx, (size_t)slot_idx);
        }
    }
    return gcode;
}

// ---------------------------------------------------------------------------
// dispatch_neotower_structural_tc — non-BBL structural finish-layer dispatch.
// Verbatim extraction of the `if (m_neo_tower != nullptr)` structural block that used
// to live inline in tool_change() (non-BBL, append_tcr2). The original did
// `gcode += append_tcr2(...); return gcode;` where `gcode` was provably empty at that
// point, so returning the append_tcr2 string directly is byte-identical. Engaged
// optional = caller returns it from tool_change; nullopt = fall through to ignore_sparse.
// ---------------------------------------------------------------------------
std::optional<std::string> WipeTowerIntegration::dispatch_neotower_structural_tc(
    GCode& gcodegen, int extruder_id)
{
    float layer_z = (float)m_tool_changes[m_layer_idx].front().print_z;
    auto struct_tcr = gcodegen.m_neo_tower->get_finish_layer(layer_z);
    if (struct_tcr) {
        NEOTKO_LOG(WIPETOWER, "TOOL_CHANGE_nonBBL structural: layer="
            << m_layer_idx << " z=" << layer_z);
        std::string gcode = append_tcr2(gcodegen, *struct_tcr, extruder_id, (double)layer_z);
        ++m_tool_change_idx;
        m_last_wipe_tower_print_z = layer_z;
        return gcode;
    }
    // NeoTower active but no structural TCR → fall to suppress.
    BOOST_LOG_TRIVIAL(warning)
        << "[NeoTower] no structural TCR for layer=" << m_layer_idx
        << " z=" << layer_z << " falling back to ignore_sparse";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// dispatch_neotower_structural_tc_bbl — BBL structural finish-layer dispatch.
// BBL mirror (append_tcr). Kept SEPARATE from the non-BBL method above by design (§8.5).
// ---------------------------------------------------------------------------
std::optional<std::string> WipeTowerIntegration::dispatch_neotower_structural_tc_bbl(
    GCode& gcodegen, int extruder_id)
{
    float layer_z = (float)m_tool_changes[m_layer_idx].front().print_z;
    auto struct_tcr = gcodegen.m_neo_tower->get_finish_layer(layer_z);
    if (struct_tcr) {
        NEOTKO_LOG(WIPETOWER, "TOOL_CHANGE_BBL structural: layer="
            << m_layer_idx << " z=" << layer_z);
        std::string gcode = append_tcr(gcodegen, *struct_tcr, extruder_id, (double)layer_z);
        ++m_tool_change_idx;
        m_last_wipe_tower_print_z = layer_z;
        return gcode;
    }
    BOOST_LOG_TRIVIAL(warning)
        << "[NeoTower] BBL no structural TCR for layer=" << m_layer_idx
        << " z=" << layer_z << " falling back to ignore_sparse";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// dispatch_neotower_sublayer_prime — sublayer-prime toolchange dispatch (BBL + non-BBL).
// Verbatim extraction of the NeoTower sublayer-prime block that used to live inline in the
// emit_local_z_unplanned_toolchange lambda of tool_change(). This is the FRAGILE, keyed
// sublayer channel where the 6 pinned baseline divergences live (§8.1): the get_tcr lookup
// (sublayer_ctx=true) and the s104-z plane-realign are UNTOUCHED — this is an EXTRACTION,
// not a flip. The original returned append_tcr(2)(...) directly from the lambda on a HIT;
// here that becomes an engaged optional the caller returns. MISS → std::nullopt so the
// caller falls through to the stock Local-Z path. The BBL/non-BBL ternary was always inline
// here (single block, not a split mirror) so it stays as-is.
// ---------------------------------------------------------------------------
std::optional<std::string> WipeTowerIntegration::dispatch_neotower_sublayer_prime(
    GCode& gcodegen, int extruder_id, double toolchange_print_z, double tower_z,
    bool local_z_sublayer_ctx)
{
    if (gcodegen.m_neo_tower != nullptr && gcodegen.writer().extruder() != nullptr) {
        const size_t current_tool = gcodegen.writer().extruder()->id();
        auto tcr_opt = gcodegen.m_neo_tower->get_tcr(
            float(toolchange_print_z), current_tool, size_t(extruder_id),
            /*sublayer_ctx=*/local_z_sublayer_ctx);
        if (tcr_opt) {
            // s104-z plane-realign: emit a fused/redirected TCR at ITS plane, not the
            // requesting sublayer's z (printing it at the requester's z drops the purge
            // band one plane down onto already-purged strip → double material + hole).
            // Only upward; plain HITs have delta ~0 (verified Paso 0) and are untouched.
            double emit_z = tower_z;
            if ((double)tcr_opt->print_z > toolchange_print_z + (double)NeoTowerZ::Z_EPS_PLAN) {
                emit_z = tower_z + ((double)tcr_opt->print_z - toolchange_print_z);
                NEOTKO_LOG(WIPETOWER, "Z_REALIGN(sub-prime): req_z=" << toolchange_print_z
                    << " tcr.print_z=" << tcr_opt->print_z << " emit_z=" << emit_z);
            }
            NEOTKO_LOG(WIPETOWER, "SUB_PRIME_EMIT z=" << toolchange_print_z
                << " old=" << current_tool << " new=" << extruder_id
                << " emit_z=" << emit_z);
            return gcodegen.is_BBL_Printer()
                       ? append_tcr(gcodegen, *tcr_opt, extruder_id, emit_z)
                       : append_tcr2(gcodegen, *tcr_opt, extruder_id, emit_z);
        }
        NEOTKO_LOG(WIPETOWER, "SUB_PRIME_MISS z=" << toolchange_print_z
            << " old=" << current_tool << " new=" << extruder_id
            << " — falling back to stock Local-Z path");
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// emit_orphan_finish_layers_until_z — s79j Bug 04 residual: emit orphan plan slots
// between the previously visited z (m_orphan_floor_z) and `next_visited_z`. Layers in
// the support air-gap exist in NeoTower's m_tool_changes plan but are absent from
// layers_to_print, so the normal sync_to_z + process_layer pipeline never dispatches
// their TCRs and the wipe tower develops a structural hole. NeoTower-gated → inert for
// Classic (returns immediately when m_neo_tower == nullptr). Companion of sync_to_z
// (GCode.hpp). Declaration lives in GCode.hpp:150; definition MOVED VERBATIM here
// (s205-5b.3b TU-move) from GCode.cpp — behaviour identical, only the TU changed.
// ---------------------------------------------------------------------------
std::string WipeTowerIntegration::emit_orphan_finish_layers_until_z(GCode& gcodegen, float next_visited_z)
{
    std::string gcode;
    if (gcodegen.m_neo_tower == nullptr) {
        m_orphan_floor_z = next_visited_z;
        return gcode;
    }
    const float EPS = NeoTowerZ::Z_EPS_PLAN;
    // First call: no previous floor → bootstrap and emit nothing (avoids spuriously
    // emitting priming-like entries below the first visited layer).
    if (m_orphan_floor_z == std::numeric_limits<float>::lowest()) {
        m_orphan_floor_z = next_visited_z;
        return gcode;
    }
    const float lo = m_orphan_floor_z;
    const float hi = next_visited_z - EPS;
    int highest_emitted_idx = -1;
    for (int i = 0; i < (int)m_tool_changes.size(); ++i) {
        if (m_tool_changes[i].empty()) continue;
        const float pz = (float)m_tool_changes[i].front().print_z;
        if (pz <= lo + EPS) continue;            // already past the floor
        if (pz >= hi)       continue;            // current or future visited
        const WipeTower::ToolChangeResult& tcr = m_tool_changes[i].front();
        NeoDebug::write(NeoDebug::WIPETOWER,
            std::string("ORPHAN_EMIT z=") + std::to_string(pz)
            + " floor=" + std::to_string(lo)
            + " until_z=" + std::to_string(next_visited_z)
            + " layer_idx=" + std::to_string(i)
            + " T" + std::to_string(tcr.initial_tool)
            + "->T" + std::to_string(tcr.new_tool));
        gcode += append_tcr2(gcodegen, tcr, tcr.new_tool, pz);
        // NEOTKO_NEOTOWER_TAG s205-5b.2b — shadow the orphan-finish emission (front slot,
        // si=0). Identity front = structural finish channel; otherwise the tcr channel.
        gcodegen.m_neo_tower->record_shadow_slot(false,
            /*from_finish=*/(tcr.initial_tool == tcr.new_tool), (size_t)i, 0);
        highest_emitted_idx = i;
    }
    if (highest_emitted_idx >= 0) {
        m_layer_idx = highest_emitted_idx;
        m_tool_change_idx = 0;
        if (size_t(m_layer_idx) < m_local_z_tool_change_idx.size())
            m_local_z_tool_change_idx[size_t(m_layer_idx)] = 0;
        if (size_t(m_layer_idx) < m_local_z_reserve_slot_idx.size())
            m_local_z_reserve_slot_idx[size_t(m_layer_idx)] = 0;
    }
    m_orphan_floor_z = next_visited_z;
    return gcode;
}

} // namespace Slic3r
