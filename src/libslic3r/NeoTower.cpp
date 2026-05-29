// NeoTower.cpp — Post-slice wipe tower for Neotko FullSpectrum 0.95
// NEOTKO_NEOTOWER_TAG_START

#include "NeoTower.hpp"
#include "NeoTowerZ.hpp"  // NEOTKO_NEOTOWER_TAG — hardening P2

#include "libslic3r/Print.hpp"             // Print, PrintObject, MultiPassSubLayer
#include "libslic3r/PrintConfig.hpp"       // PrintConfig, PrintRegionConfig
#include "libslic3r/GCode/ToolOrdering.hpp" // LayerTools, ToolOrdering
#include "libslic3r/MultiPassScheduler.hpp" // NEOTKO_MPSCHEDULER_TAG s79 — canonical grouped-by-tool order

#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <cstdlib>

// NEOTKO_NEOTOWER_TAG_START — NeoDebug routing
#include "SurfaceColorMix.hpp"  // NEOTKO_NEOTOWER_TAG — NeoDebug

#define NT_LOG(msg) do { if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {  \
    std::ostringstream _nt_oss; _nt_oss << "[NEOTOWER] " << msg;         \
    NeoDebug::write(NeoDebug::WIPETOWER, _nt_oss.str()); } } while(0)
// NEOTKO_NEOTOWER_TAG_END

namespace Slic3r {

// ---------------------------------------------------------------------------
// Key encoding: quantize z to 1 µm, pack with tool IDs.
// Supports up to 99 physical extruders.
//
// NEOTKO_NEOTOWER_TAG — Hallazgo VII: Z collision note.
// The last sublayer and the real layer at the same nominal Z quantize to the same
// z_um (e.g. sub_z=0.7998 and nominal=0.8 both → 800 µm). Collisions are safe
// today because m_tcr_index and m_finish_layer_index are separate maps, and within
// m_tcr_index every event at the same z_um has a distinct (old_tool, new_tool) pair.
// INVARIANT: no two sublayer events at the same Z may share the same (old, new) pair.
// ---------------------------------------------------------------------------
uint64_t NeoTower::make_key(float z_actual, size_t old_tool, size_t new_tool)
{
    assert(old_tool < 100 && new_tool < 100);
    uint64_t z_um = static_cast<uint64_t>(std::llround(z_actual * 1000.f));
    return z_um * 10000ULL + old_tool * 100ULL + new_tool;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
NeoTower::NeoTower(const PrintConfig&                     config,
                   const PrintRegionConfig&               default_region_config,
                   int                                    plate_idx,
                   Vec3d                                  plate_origin,
                   const std::vector<std::vector<float>>& wiping_matrix,
                   size_t                                 initial_tool)
    : m_print_config   (&config)
    , m_region_config  (&default_region_config)
    , m_plate_idx      (plate_idx)
    , m_plate_origin   (plate_origin)
    , m_wipe_volumes   (wiping_matrix)
    , m_initial_tool   (initial_tool)
{
    // Nozzle geometry — first extruder is representative.
    if (!config.nozzle_diameter.values.empty())
        m_nozzle_diameter = static_cast<float>(config.nozzle_diameter.values[0]);

    if (!config.filament_diameter.values.empty())
        m_filament_diameter = static_cast<float>(config.filament_diameter.values[0]);

    // Perimeter width matches WipeTower2's Width_To_Nozzle_Ratio = 1.25.
    m_perimeter_width = m_nozzle_diameter * 1.25f;

    // Min layer height from config.
    // min_layer_height is ConfigOptionFloats (per-extruder) in this fork.
    if (!config.min_layer_height.values.empty())
        m_min_layer_height = static_cast<float>(config.min_layer_height.values[0]);
    if (m_min_layer_height < 0.04f)
        m_min_layer_height = 0.04f;

    // Tower X/Y — assign directly to m_plan (m_tower_x/y are not declared members).
    // wipe_tower_x/y are ConfigOptionFloats indexed by plate_idx.
    m_plan.tower_x = static_cast<float>(config.wipe_tower_x.get_at(plate_idx))
                     + static_cast<float>(plate_origin(0));
    m_plan.tower_y = static_cast<float>(config.wipe_tower_y.get_at(plate_idx))
                     + static_cast<float>(plate_origin(1));

    // Brim width — prime_tower_brim_width is ConfigOptionFloat (scalar).
    m_brim_width = static_cast<float>(config.prime_tower_brim_width.value);
}

// ---------------------------------------------------------------------------
// is_enabled()
// ---------------------------------------------------------------------------
bool NeoTower::is_enabled(const PrintConfig& config)
{
    // Config key added in PrintConfig.hpp/cpp: neotko_wipe_tower (ConfigOptionBool).
    const auto* opt = config.option<ConfigOptionBool>("neotko_wipe_tower");
    if (!opt) {
        NT_LOG("is_enabled(): key 'neotko_wipe_tower' NOT FOUND in config");
        return false;
    }
    NT_LOG("is_enabled() → " << (opt->value ? "TRUE" : "FALSE"));
    return opt->value;
}

// ---------------------------------------------------------------------------
// Public entry point: collect_and_plan()
// ---------------------------------------------------------------------------
void NeoTower::collect_and_plan(const Print& print)
{
    NT_LOG("collect_and_plan() START");
    collect_all_events(print);
    plan();
    NT_LOG("collect_and_plan() DONE — " << m_events.size() << " real_tc + "
           << m_growth_events.size() << " growth, "
           << m_plan.layers.size() << " planned layers");
    // NEOTKO_NEOTOWER_TAG — hardening P3: validate plan invariants
    this->validate_plan();
}

// ===========================================================================
// PHASE 1 — collect_all_events()
//
// Walk the fully-built ToolOrdering and m_multipass_sublayers.
// Emit one NeoTowerEvent per toolchange (real layers) and per sublayer
// toolchange (MultiPass).
// ===========================================================================
void NeoTower::collect_all_events(const Print& print)
{
    m_events.clear();
    m_growth_events.clear();  // NEOTKO_NEOTOWER_TAG — hardening P5
    m_z_redirect.clear();          // NEOTKO_MPSCHEDULER_TAG
    m_z_redirect_finish.clear();   // NEOTKO_MPSCHEDULER_TAG

    const ToolOrdering& tool_ordering = print.get_tool_ordering();
    const PrintConfig&  cfg           = *m_print_config;

    // ------------------------------------------------------------------
    // Collect MultiPass prime volume — max over all objects' regions.
    // Same logic as NEOTKO_MULTIPASS_PRIME_TAG block in Print.cpp.
    // ------------------------------------------------------------------
    float mp_prime_vol = 0.f;
    for (const PrintObject* obj : print.objects()) {
        const PrintObject& src = obj->get_shared_object()
                                 ? *obj->get_shared_object() : *obj;
        if (src.multipass_sublayers().empty()) continue;   // ← solo objetos con MP activo
        if (src.layers().empty()) continue;
        for (const LayerRegion* lr : src.layers().front()->regions())
            mp_prime_vol = std::max(mp_prime_vol,
                static_cast<float>(lr->region().config().multipass_prime_volume.value));
    }

    // z_um64 lambda: convert float Z to micron-integer key (used throughout).
    auto z_um64 = [](float z) -> uint64_t {
        return static_cast<uint64_t>(std::llround(static_cast<double>(z) * 1000.0));
    };

    // ------------------------------------------------------------------
    // NEOTKO_NEOTOWER_TAG — Pre-build sublayer z → exit tool map.
    //
    // Sublayer LayerTools entries have intentionally EMPTY lt.extruders
    // (see ToolOrdering.cpp line ~1167: "extruders intentionally NOT
    // populated — sublayer handler reads sub.tool_id directly").
    // Empty extruders → first_extruder() skips sublayers → correct
    // wipe tower initial tool.
    //
    // But NeoTower section 1a and sub_z_to_real_tool need to track the
    // writer's tool through sublayer entries to apply the correct rotation
    // at the next real layer.  This map provides the sublayer tool data
    // from the canonical source: PrintObject::multipass_sublayers().
    //
    // Key:   z_um64 of sublayer print_z
    // Value: tool_id at that z (last-write-wins for multi-object same-z)
    // ------------------------------------------------------------------
    std::map<uint64_t, size_t> sublayer_z_tool;
    for (const PrintObject* obj : print.objects()) {
        const PrintObject& src = obj->get_shared_object()
                                 ? *obj->get_shared_object() : *obj;
        for (const auto& layer_subs : src.multipass_sublayers()) {
            for (const MultiPassSubLayer& sub : layer_subs) {
                const uint64_t z_um = z_um64(static_cast<float>(sub.print_z));
                sublayer_z_tool[z_um] = static_cast<size_t>(sub.tool_id);
            }
        }
    }
    NT_LOG("sublayer_z_tool map: " << sublayer_z_tool.size() << " entries");
    for (const auto& [z, tool] : sublayer_z_tool)
        NT_LOG("  z_um=" << z << " → T" << tool);

    // ------------------------------------------------------------------
    // NEOTKO_MPSCHEDULER_TAG s79 — canonical grouped-by-tool order (espejo).
    //
    // Map sublayer print_z (µm) → parent real-layer print_z (z_nominal).
    // Sublayers are inserted BEFORE their parent real layer in ToolOrdering,
    // so flush the pending sublayer Zs when we reach the next real layer.
    // (Moved EARLIER than its old 1b position so the 1a tracking loop and the
    //  sublayer scheduler can both group sublayers by z_nominal.)
    // ------------------------------------------------------------------
    std::unordered_map<uint64_t, float> sublayer_to_nominal;
    {
        std::vector<float> pending_sub_zs;
        for (const LayerTools& lt : tool_ordering) {
            if (lt.is_mp_sublayer) {
                pending_sub_zs.push_back(static_cast<float>(lt.print_z));
            } else {
                float z_nom = static_cast<float>(lt.print_z);
                for (float sz : pending_sub_zs)
                    sublayer_to_nominal[z_um64(sz)] = z_nom;
                pending_sub_zs.clear();
            }
        }
        for (float sz : pending_sub_zs)
            sublayer_to_nominal[z_um64(sz)] = sz;
    }

    // Per-z_nominal SublayerKey lists, built from the canonical source
    // (PrintObject::multipass_sublayers()) — the IDENTICAL input GCode uses to
    // reorder its mp_sublayer emission. chain_key identifies one stacking chain
    // (one object's stacked passes at the same XY) so pass N precedes pass N+1.
    std::map<float, std::vector<MultiPassScheduler::SublayerKey>> mp_znom_keys;
    for (const PrintObject* obj : print.objects()) {
        const PrintObject& src = obj->get_shared_object()
                                 ? *obj->get_shared_object() : *obj;
        const auto& all_subs = src.multipass_sublayers();
        for (int li = 0; li < (int)all_subs.size(); ++li) {
            for (const MultiPassSubLayer& sub : all_subs[li]) {
                MultiPassScheduler::SublayerKey k;
                k.chain_key = (uint64_t)(uintptr_t)obj * 1000003ull + (uint64_t)li;
                k.pass_idx  = sub.pass_idx;
                k.tool_id   = sub.tool_id;
                k.z_actual  = sub.print_z;
                float znom = k.z_actual;
                auto itn = sublayer_to_nominal.find(z_um64(static_cast<float>(k.z_actual)));
                if (itn != sublayer_to_nominal.end())
                    znom = itn->second;
                mp_znom_keys[znom].push_back(k);
            }
        }
    }

    // Canonical grouped-by-tool order for one z_nominal group, given the writer
    // tool ENTERING the group. Deterministic mirror of GCode's emission reorder.
    //
    // CRITICAL: GCode emits ONE process_layer per z_actual PLANE (collect_layers_to_print
    // merges by print_z), travelling in Z between planes. So the canonical order is
    // Z-PLANE-MAJOR (planes ascending) and tool-grouped WITHIN each plane, threading the
    // writer tool from one plane's end into the next plane's start. This is z-monotonic
    // by construction, so the per-z GCode calls and this whole-group replay agree, and
    // the cross-z transitions are exactly the plane boundaries.
    auto mp_group_canon_order = [&](float z_nominal, int entering_tool)
        -> std::vector<MultiPassScheduler::SublayerKey> {
        auto it = mp_znom_keys.find(z_nominal);
        if (it == mp_znom_keys.end())
            return {};
        // Group this z_nominal's sublayers into z_actual planes, ascending.
        std::map<double, std::vector<MultiPassScheduler::SublayerKey>> planes;
        for (const auto& k : it->second)
            planes[k.z_actual].push_back(k);
        std::vector<MultiPassScheduler::SublayerKey> out;
        int cur = entering_tool;
        for (auto& [z, plane_items] : planes) {
            std::vector<size_t> ord =
                MultiPassScheduler::order_sublayers_by_tool(plane_items, cur);
            for (size_t idx : ord)
                out.push_back(plane_items[idx]);
            if (!ord.empty())
                cur = plane_items[ord.back()].tool_id;
        }
        return out;
    };
    // Final writer tool a z_nominal group leaves the writer on, given entry tool.
    auto mp_group_final_tool = [&](float z_nominal, int entering_tool) -> int {
        std::vector<MultiPassScheduler::SublayerKey> ord =
            mp_group_canon_order(z_nominal, entering_tool);
        return ord.empty() ? entering_tool : ord.back().tool_id;
    };

    // ------------------------------------------------------------------
    // 1a. Real-layer toolchanges from ToolOrdering.
    //
    // Mirrors the plan_toolchange loop in Print.cpp::_make_wipe_tower().
    // We walk consecutive extruder pairs per layer, exactly as WipeTower2
    // would see them.
    // ------------------------------------------------------------------
    {
        // NEOTKO_NEOTOWER_TAG — brim fix Bug B (s66): mirror GCode's initial_extruder_id logic.
        // GCode.cpp (line ~2913) sets:
        //   initial_extruder_id = (!is_bbl && has_wt && !single_extruder_priming)
        //                         ? all_extruders().back()   // non-BBL default: start on last extruder
        //                         : first_extruder()
        // For non-BBL with wipe tower, the writer starts on all_extruders().back(), NOT first_extruder().
        // NeoTower was using first_extruder() → planned T0→T3 as [0][0].
        // GCode arrives with writer=T3 → calls get_tcr(z, T3, T0) → hits [0][1] → brim in [0][0] lost.
        // Fix: use the same initial tool as GCode so the first planned TC matches what GCode dispatches.
        const bool is_bbl_1a      = print.is_BBL_printer();
        const bool has_wt_1a      = tool_ordering.has_wipe_tower();
        const bool semm_priming   = cfg.single_extruder_multi_material_priming.value;
        size_t current_tool;
        if (!is_bbl_1a && has_wt_1a && !semm_priming && !tool_ordering.all_extruders().empty())
            current_tool = tool_ordering.all_extruders().back();
        else {
            current_tool = tool_ordering.first_extruder();
            if (current_tool == (size_t)-1)
                current_tool = m_initial_tool;
        }
        NT_LOG("1a initial current_tool=T" << current_tool
            << " is_bbl=" << is_bbl_1a << " has_wt=" << has_wt_1a
            << " semm=" << semm_priming
            << " all_ext_back=" << (tool_ordering.all_extruders().empty() ? -1
                                    : (int)tool_ordering.all_extruders().back())
            << " first_ext=" << (int)tool_ordering.first_extruder());

        bool first_real_layer_1a = true; // NEOTKO_NEOTOWER_TAG — first-layer rotation guard
        float last_znom_1a = -1.f;       // NEOTKO_MPSCHEDULER_TAG s79 — current sublayer group
        bool just_exited_sublayer_group = false; // NEOTKO_MPSCHEDULER_TAG s79

        for (const LayerTools& lt : tool_ordering) {
            if (!lt.has_wipe_tower || lt.is_mp_sublayer) {
                // NEOTKO_NEOTOWER_TAG — Track tool through sublayers.
                // Sublayer groups change the writer's tool between real layers.
                // GCode.cpp rotates layer_extruders based on the writer's current
                // tool (nominal_layer_start_extruder), which includes sublayer exits.
                // NeoTower must track this to apply the same rotation.
                //
                // NOTE: lt.extruders is intentionally EMPTY for sublayer entries
                // (ToolOrdering preserves first_extruder() correctness).
                // Use sublayer_z_tool map (built from multipass_sublayers) instead.
                if (lt.is_mp_sublayer) {
                    // NEOTKO_MPSCHEDULER_TAG s79 — the writer tool a sublayer GROUP
                    // leaves on is the LAST tool of the canonical grouped-by-tool
                    // emission, NOT sublayer_z_tool[highest z]. Compute it ONCE per
                    // z_nominal group (on its first entry, using the entering tool as
                    // initial_tool) so this matches GCode's reordered emission exactly.
                    const uint64_t sub_zum = z_um64(static_cast<float>(lt.print_z));
                    float znom = static_cast<float>(lt.print_z);
                    {
                        auto itn = sublayer_to_nominal.find(sub_zum);
                        if (itn != sublayer_to_nominal.end()) znom = itn->second;
                    }
                    if (znom != last_znom_1a) {
                        size_t prev = current_tool;
                        current_tool = (size_t)mp_group_final_tool(znom, (int)current_tool);
                        last_znom_1a = znom;
                        just_exited_sublayer_group = true;
                        NT_LOG("1a SUBLAYER_GROUP_FINAL z=" << lt.print_z
                            << " z_nom=" << znom
                            << " current_tool: T" << prev << " → T" << current_tool
                            << " (canonical grouped-by-tool end)");
                    } else {
                        NT_LOG("1a SUBLAYER_TRACK z=" << lt.print_z
                            << " z_um=" << sub_zum << " (same group, current_tool=T"
                            << current_tool << ")");
                    }
                } else {
                    // NEOTKO_MPSCHEDULER_TAG s79 — post-sandwich perimeter recovery.
                    // A real layer with has_wipe_tower=false immediately after a sandwich
                    // group: GCode's structural/real-TC machinery is skipped here (no plan
                    // slot), yet the writer sits on the group's canonical exit tool (e.g.
                    // T3) while this layer's perimeter needs another tool (e.g. T1). Without
                    // a planned TCR, GCode bare-switches → contaminated perimeter (the s71 /
                    // FINAL_LAYER bug). Emit a real exit→perim TCR keyed at z_nominal so
                    // get_tcr(z_nominal, exit, perim) HITs and GCode can PURGE it. Keyed
                    // lookup → no new plan layer, no m_layer_idx shift.
                    if (just_exited_sublayer_group && !lt.extruders.empty()) {
                        const bool cur_in_layer =
                            std::find(lt.extruders.begin(), lt.extruders.end(),
                                      (unsigned int)current_tool) != lt.extruders.end();
                        if (!cur_in_layer) {
                            const size_t perim = (size_t)lt.extruders.front();
                            const float  z     = static_cast<float>(lt.print_z);
                            float lh_raw = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                                  ? lt.wipe_tower_layer_height
                                                                  : lt.layer_height);
                            const float lh = (lh_raw >= 0.01f) ? lh_raw
                                                               : (m_nozzle_diameter * 0.5f);
                            // NEOTKO_MPSCHEDULER_TAG s79 — minimal purge, NOT the flush
                            // matrix. The machine's toolchange macro performs the real
                            // filament swap; the tower visit here only controls drip and
                            // lays the structural frame. Using the flush matrix made this
                            // the single largest deposit (~15 mm³), inflating the tower
                            // footprint. Match the sandwich-band knob (~5 mm³) so it stops
                            // driving the footprint size.
                            float wipe_vol = (mp_prime_vol > 0.f)
                                             ? mp_prime_vol
                                             : static_cast<float>(cfg.prime_volume);

                            NeoTowerEvent ev;
                            ev.z_nominal    = z;
                            ev.z_actual     = z;
                            ev.layer_height = lh;
                            ev.old_tool     = current_tool;
                            ev.new_tool     = perim;
                            ev.wipe_volume  = wipe_vol;
                            // is_sublayer=false: this is a real-layer (nominal-z) TC, same
                            // convention as the structural post-sublayer TC (line ~1215).
                            // Keeps it out of sublayer_zums / sub_group_exit_tool bookkeeping
                            // while still joining the z_nominal plan layer (grouped by z_nominal,
                            // not is_sublayer) → no new plan layer, no m_layer_idx shift.
                            ev.is_sublayer  = false;
                            // NEOTKO_MPSCHEDULER_TAG s79b — sandwich→perimeter recovery is a
                            // sandwich-context TC: visit the tower (drip) but skip ramming so
                            // the purge lands before the perimeter print, not after.
                            ev.no_ramming   = true;
                            m_events.push_back(ev);
                            NT_LOG("1a POST_SANDWICH_RECOVERY z=" << z
                                << " old=T" << current_tool << " new=T" << perim
                                << " vol=" << wipe_vol << " (has_wt=false perimeter purge)");
                            current_tool = perim;
                        }
                    }
                    just_exited_sublayer_group = false;
                    NT_LOG("1a SKIP z=" << lt.print_z
                        << " is_mp_sub=" << lt.is_mp_sublayer
                        << " has_wt=" << lt.has_wipe_tower
                        << " ext_size=" << lt.extruders.size()
                        << " current_tool=T" << current_tool);
                }
                continue;
            }

            // NEOTKO_MPSCHEDULER_TAG s79 — a has_wt=true real layer's own rotation+loop
            // below already emits the exit→perim transition, so clear the recovery flag.
            just_exited_sublayer_group = false;

            const float z_nom    = static_cast<float>(lt.print_z);
            NT_LOG("1a REAL_LAYER z=" << z_nom
                << " has_wt=" << lt.has_wipe_tower
                << " is_mp_sub=" << lt.is_mp_sublayer
                << " first=" << first_real_layer_1a
                << " current_tool=T" << current_tool
                << " ext=[");
            for (unsigned int e : lt.extruders)
                NT_LOG("  T" << e);
            NT_LOG("]");

            // FIX A — NEOTKO_NEOTOWER_TAG: Use 50% of nozzle diameter as a safe floor
            float lh_raw = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                       ? lt.wipe_tower_layer_height
                                                       : lt.layer_height);
            const float lh = (lh_raw >= 0.01f) ? lh_raw : (m_nozzle_diameter * 0.5f);

            bool first_lt = (&lt == &tool_ordering.front());

            // NEOTKO_NEOTOWER_TAG — Match GCode.cpp extruder dispatch order.
            // GCode.cpp (line ~6874) rotates layer_extruders to put the current
            // writer tool first via std::rotate. This avoids an unnecessary initial
            // toolchange and changes the entire TC sequence (different (old,new) pairs,
            // different count, different end-of-layer tool).
            // NeoTower must apply the same rotation so that the planned events match
            // what GCode.cpp will actually dispatch.  Without this, get_tcr() misses
            // on layers with 4+ extruders where the rotation reorders significantly
            // (e.g. z=6.45 with [T0,T1,T2,T3] + current=T2 → rotated [T2,T3,T0,T1]).
            //
            // EXCEPTION: On the first real layer, GCode does NOT rotate because
            // m_writer.extruder() is nullptr → nominal_layer_start_extruder = -1
            // → the rotation condition fails.  Layer_extruders stays in sorted
            // ascending order.  NeoTower must match this: skip rotation on the
            // first real layer.
            std::vector<unsigned int> rotated_ext(lt.extruders.begin(), lt.extruders.end());
            // NEOTKO_PATHBLEND_TAG — Fase 5 s77 migración: Bug E gate REVERTED.
            // The s73 Bug E gate (`&& !lt.has_pathblend_chain`) mirrored GCode's
            // Bug C std::rotate suppression. Both are gone now: PathBlend no longer
            // registers tools into lt.extruders (it is compiled into sublayers), so
            // the real-layer rotation can never reorder a PB ramp/cap pair. NeoTower
            // again mirrors GCode's unconditional rotation (Bug C was reverted too).
            if (!first_real_layer_1a) {
                auto rot_it = std::find(rotated_ext.begin(), rotated_ext.end(),
                                        (unsigned int)current_tool);
                if (rot_it != rotated_ext.end())
                    std::rotate(rotated_ext.begin(), rot_it, rotated_ext.end());
            }
            // NEOTKO_NEOTOWER_TAG — s78 diag (perimeter-color bug): log the rotated
            // order + the free-first current_tool so we can confirm whether a real
            // wall TC is being skipped because its tool collides with the preceding
            // PathBlend cap sublayer tool. Zero behavior change.
            {
                std::ostringstream _ro;
                _ro << "1a ROTATE z=" << z_nom << " current_tool=T" << current_tool
                    << " rotated=[";
                for (unsigned int e : rotated_ext) _ro << "T" << e << " ";
                _ro << "]";
                NT_LOG(_ro.str());
            }

            for (const unsigned int ext_id : rotated_ext) {
                // NEOTKO_NEOTOWER_TAG — brim fix Bug B (s66): is_first_on_first removed.
                // Previously this flag forced a TC for all_extruders().back() even when
                // ext_id == current_tool, generating a phantom T0→T3 that GCode never
                // dispatches (GCode starts on T3 and only calls T3→T0).
                // Now current_tool is initialized to all_extruders().back() for non-BBL,
                // so the normal skip (ext_id == current_tool) correctly suppresses T3→T3
                // and the loop generates exactly the TCs GCode will dispatch.
                if (ext_id == (unsigned int)current_tool) {
                    // NEOTKO_NEOTOWER_TAG — s78 diag: this is the "free-first" skip.
                    // If a real wall tool equals current_tool (e.g. the PathBlend cap
                    // sublayer tool), NO toolchange event is planned for it — correct
                    // only if GCode prints that tool first. With per-object emission
                    // the wall may print after other objects → missing TC → wrong
                    // perimeter color (s71 bug).
                    NT_LOG("1a SKIP_FREE_FIRST z=" << z_nom << " ext=T" << ext_id
                        << " (== current_tool T" << current_tool << ", no TC planned)");
                    continue; // no toolchange
                }

                // Wipe volume: use the matrix passed at construction (already built from
                // flush_volumes_matrix * flush_multiplier by WipeTower2::extract_wipe_volumes).
                // Fallback to prime_volume (scalar) when matrix is empty or out of bounds.
                float wipe_vol = static_cast<float>(cfg.prime_volume);
                if (current_tool < m_wipe_volumes.size() &&
                    ext_id < m_wipe_volumes[current_tool].size())
                    wipe_vol = m_wipe_volumes[current_tool][ext_id];

                NeoTowerEvent ev;
                ev.z_nominal    = z_nom;
                ev.z_actual     = z_nom;
                ev.layer_height = lh;
                ev.old_tool     = current_tool;
                ev.new_tool     = ext_id;
                ev.wipe_volume  = wipe_vol;
                ev.is_sublayer  = false;
                // NEOTKO_NEOTOWER_TAG — hardening P5: route to correct channel
                if (ev.old_tool == ev.new_tool)
                    m_growth_events.push_back(ev);
                else
                    m_events.push_back(ev);
                NT_LOG("real-layer event z=" << ev.z_actual
                    << " old=" << ev.old_tool << " new=" << ev.new_tool
                    << " vol=" << ev.wipe_volume << " lh=" << ev.layer_height);

                current_tool = ext_id;
            }
            first_real_layer_1a = false;
        }
    }

    // ------------------------------------------------------------------
    // 1b. MultiPass sublayer toolchanges from ToolOrdering.
    //
    // ARCHITECTURE NOTE (MULTIPASS.md §Virtual Sublayer Architecture):
    //   Each MultiPass pass is a LayerTools entry with is_mp_sublayer=true
    //   and has_object=false inserted into ToolOrdering by ToolOrdering.cpp.
    //   m_multipass_sublayers on PrintObject is NOT where this data lives —
    //   it is the LayerTools chain that carries it.
    //
    // Walk ToolOrdering sequentially. For is_mp_sublayer layers:
    //   - Track current_tool across ALL LayerTools (real + sublayer) to get
    //     accurate old_tool at each sublayer toolchange.
    //   - Each extruder transition in a sublayer LayerTools is a tower event.
    //   - The parent real-layer Z (z_nominal) is the next non-sublayer
    //     LayerTools with the same or higher print_z.
    //
    // Prime sentinels (old==new) come from the WipeTower2 plan_local_z_reserve
    // logic already exercised in Print.cpp (NEOTKO_MULTIPASS_PRIME_TAG).
    // NeoTower emits them here as well so get_tcr() can serve them.
    // ------------------------------------------------------------------

    // (sublayer_to_nominal is built earlier now — see NEOTKO_MPSCHEDULER_TAG s79.)

    // Second pass: read sublayer tool data directly from multipass_sublayers().
    //
    // NEOTKO_MPSCHEDULER_TAG — Scheduler de oportunidad (C2+C3).
    // Reemplaza la antigua dedup {z_um,tool_id} + chaining lineal.
    // Agrupa eventos sublayer por (old_tool, new_tool) dentro de cada z_nominal,
    // respetando prerequisitos de passes (pass_idx > 0 requiere pass_idx-1 completado).
    // Resultado: O(N_tools) wipe slots por capa nominal en vez de O(N_objetos × passes).
    //
    // sub_z_to_real_tool (Bug IX fix) se preserva como fuente de old_tool para pass_idx==0.
    // Routing P5 (identity→m_growth_events, real→m_events) se preserva.
    {
        // NEOTKO_NEOTOWER_TAG — Bug IX fix: pre-calculate real tool active just before
        // each sublayer z-group.
        std::map<uint64_t, size_t> sub_z_to_real_tool;
        {
            size_t running = tool_ordering.first_extruder();
            if (running == (size_t)-1) running = m_initial_tool;
            bool first_real_layer_sub = true; // NEOTKO_NEOTOWER_TAG — first-layer rotation guard
            float last_znom_sub = -1.f;       // NEOTKO_MPSCHEDULER_TAG s79 — current sublayer group
            for (const LayerTools& lt : tool_ordering) {
                if (lt.is_mp_sublayer) {
                    const uint64_t z_um = z_um64(static_cast<float>(lt.print_z));
                    // emplace BEFORE advancing: records the writer tool ENTERING this z.
                    // For the FIRST z of a group this is the group's entering tool, which
                    // chain_cur (sublayer scheduler) and pass0 old_tool rely on.
                    sub_z_to_real_tool.emplace(z_um, running);
                    // NEOTKO_MPSCHEDULER_TAG s79 — advance `running` to the tool the
                    // sublayer GROUP leaves the writer on = LAST tool of the canonical
                    // grouped-by-tool emission (same espejo as GCode). Computed ONCE per
                    // z_nominal group; intermediate z entries within the group keep it.
                    float znom = static_cast<float>(lt.print_z);
                    {
                        auto itn = sublayer_to_nominal.find(z_um);
                        if (itn != sublayer_to_nominal.end()) znom = itn->second;
                    }
                    if (znom != last_znom_sub) {
                        running = (size_t)mp_group_final_tool(znom, (int)running);
                        last_znom_sub = znom;
                    }
                } else {
                    if (!lt.extruders.empty()) {
                        // NEOTKO_NEOTOWER_TAG — Match GCode.cpp rotation.
                        // GCode.cpp rotates layer_extruders to put current tool first,
                        // so the last dispatched tool is NOT .back() but the element
                        // just before current_tool in the sorted extruder list.
                        // Formula: rotated = [cur, cur+1, ..., last, first, ..., cur-1]
                        //          last dispatched = extruders[(idx_of_cur - 1 + n) % n]
                        //
                        // EXCEPTION: first real layer — GCode does NOT rotate (writer
                        // is nullptr), so last dispatched = .back() (sorted ascending).
                        if (!first_real_layer_sub) {
                            auto it = std::find(lt.extruders.begin(), lt.extruders.end(),
                                                (unsigned int)running);
                            if (it != lt.extruders.end()) {
                                size_t n   = lt.extruders.size();
                                size_t idx = (size_t)std::distance(lt.extruders.begin(), it);
                                running = (size_t)lt.extruders[(idx + n - 1) % n];
                            } else {
                                running = (size_t)lt.extruders.back();
                            }
                        } else {
                            // First real layer: no rotation, last = .back()
                            running = (size_t)lt.extruders.back();
                        }
                        first_real_layer_sub = false;
                    }
                }
            }
        }

        // NEOTKO_MPSCHEDULER_TAG — C2: SurfaceEvent struct (local to this scope)
        struct SurfaceEvent {
            const PrintObject* obj       = nullptr;
            int                layer_idx = -1;    // index into obj->multipass_sublayers()
            int                pass_idx  = -1;    // position within MultiPassConfig
            float              z_actual  = 0.f;   // sub.print_z (with -2*EPSILON on last pass)
            float              z_nominal = 0.f;   // parent real-layer Z
            float              height    = 0.f;   // sub.height
            int                old_tool  = 0;
            int                new_tool  = 0;
            float              wipe_vol  = 0.f;
        };

        if (mp_prime_vol > 0.f) {
            // ── Fase A: Construir vector<SurfaceEvent> ──────────────────────
            std::vector<SurfaceEvent> surf_events;

            for (const PrintObject* obj : print.objects()) {
                const PrintObject& src = obj->get_shared_object()
                                         ? *obj->get_shared_object() : *obj;
                const auto& all_layer_subs = src.multipass_sublayers();
                for (int li = 0; li < (int)all_layer_subs.size(); ++li) {
                    const auto& layer_subs = all_layer_subs[li];
                    for (const MultiPassSubLayer& sub : layer_subs) {
                        SurfaceEvent se;
                        se.obj       = obj;
                        se.layer_idx = li;
                        se.pass_idx  = sub.pass_idx;
                        se.z_actual  = static_cast<float>(sub.print_z);
                        se.height    = sub.height;
                        se.new_tool  = sub.tool_id;
                        // z_nominal from sublayer_to_nominal map
                        {
                            auto it = sublayer_to_nominal.find(z_um64(se.z_actual));
                            se.z_nominal = (it != sublayer_to_nominal.end())
                                           ? it->second : se.z_actual;
                        }
                        // old_tool: for pass_idx==0, use sub_z_to_real_tool (Bug IX).
                        // For pass_idx>0: the old_tool is the tool of the previous pass.
                        // We'll resolve old_tool for pass_idx>0 in a second sweep below.
                        if (se.pass_idx == 0) {
                            auto it = sub_z_to_real_tool.find(z_um64(se.z_actual));
                            se.old_tool = (it != sub_z_to_real_tool.end())
                                          ? (int)it->second : (int)m_initial_tool;
                        }
                        // wipe volume
                        se.wipe_vol = mp_prime_vol;
                        surf_events.push_back(se);
                    }
                }
            }

            // Resolve old_tool for pass_idx > 0: it's the new_tool of pass_idx-1
            // for the same (obj, layer_idx).
            // Build a lookup: (obj_ptr, layer_idx, pass_idx) → new_tool
            std::map<std::tuple<const PrintObject*, int, int>, int> pass_tool;
            for (const SurfaceEvent& se : surf_events)
                pass_tool[{se.obj, se.layer_idx, se.pass_idx}] = se.new_tool;
            for (SurfaceEvent& se : surf_events) {
                if (se.pass_idx > 0) {
                    auto it = pass_tool.find({se.obj, se.layer_idx, se.pass_idx - 1});
                    if (it != pass_tool.end())
                        se.old_tool = it->second;
                    else
                        se.old_tool = (int)m_initial_tool; // safety fallback
                }
                // NEOTKO_NEOTOWER_TAG — s78 fix (sandwich purge knob): sublayer tool
                // changes are transitions BETWEEN passes of the SAME surface (sandwich),
                // not full inter-color object changes. They must purge
                // `multipass_prime_volume` (the "SurfaceColorMix wipe reserve" UI knob,
                // already set into se.wipe_vol at construction), NOT flush_volumes_matrix.
                // The old matrix override (a) made the knob DEAD — any populated flush
                // matrix always won — and (b) forced a full ~15 mm³ flush per thin
                // sublayer → wipe-tower overflow + volumetric-flow spike. Keeping
                // se.wipe_vol = mp_prime_vol makes the knob authoritative and tunable.
                // (Real object-body toolchanges still use the flush matrix elsewhere.)
            }

            // ── Fase A2: Synthetic cross-product + entry events ─────────────
            // NEOTKO_MPSCHEDULER_TAG: GCode primes multiple tools at each sublayer z
            // in round-robin.  The scheduler only emits pass-chain transitions, but
            // GCode also needs:
            //   1) Reverse TCs (T_a→T_b AND T_b→T_a) when both are primed at same z
            //   2) Entry TCs from tools active at the PREVIOUS sublayer z
            // Without these, get_tcr() returns MISS → bare set_extruder() → no purge.
            {
                // 1. Collect per-z metadata: float z, z_nominal, max height, tool set
                struct ZInfo {
                    float z_float   = 0.f;
                    float z_nominal = 0.f;
                    float h_max     = 0.f;
                    std::set<int> tools;  // new_tool values (= tools being primed)
                };
                std::map<uint64_t, ZInfo> z_info;
                for (const SurfaceEvent& se : surf_events) {
                    const uint64_t zum = z_um64(se.z_actual);
                    auto& info   = z_info[zum];
                    info.z_float   = se.z_actual;
                    info.z_nominal = se.z_nominal;
                    info.h_max     = std::max(info.h_max, se.height);
                    info.tools.insert(se.new_tool);
                }

                // 2. Track existing (z_um, old, new) pairs to avoid duplicates
                std::set<std::tuple<uint64_t, int, int>> existing;
                for (const SurfaceEvent& se : surf_events) {
                    existing.insert({z_um64(se.z_actual), se.old_tool, se.new_tool});
                }

                // 3. Helper: add synthetic event if pair doesn't exist yet
                const size_t base_count = surf_events.size();
                auto maybe_add_synthetic = [&](uint64_t zum, int old_t, int new_t) {
                    if (old_t == new_t) return;
                    if (existing.count({zum, old_t, new_t})) return;
                    existing.insert({zum, old_t, new_t});
                    const auto& info = z_info[zum];
                    SurfaceEvent se;
                    se.obj       = nullptr;    // synthetic — not tied to any object
                    se.layer_idx = -1;
                    se.pass_idx  = 0;          // always ready (no prerequisites)
                    se.z_actual  = info.z_float;
                    se.z_nominal = info.z_nominal;
                    se.height    = (info.h_max > 0.01f) ? info.h_max
                                                        : (m_nozzle_diameter * 0.5f);
                    se.old_tool  = old_t;
                    se.new_tool  = new_t;
                    // NEOTKO_NEOTOWER_TAG — s78 fix (sandwich purge knob): synthetic
                    // cross-product sublayer events also purge the reserve knob, not the
                    // flush matrix (same rationale as the real sublayer events above).
                    se.wipe_vol  = mp_prime_vol;
                    surf_events.push_back(se);
                };

                // 4. Group z_ums by z_nominal (ascending)
                std::map<float, std::vector<uint64_t>> znom_groups;
                for (const auto& [zum, info] : z_info)
                    znom_groups[info.z_nominal].push_back(zum);
                for (auto& [zn, zums] : znom_groups)
                    std::sort(zums.begin(), zums.end());

                // 5. Generate synthetics
                for (const auto& [zn, zums] : znom_groups) {
                    const std::set<int>* prev_tools = nullptr;
                    for (uint64_t zum : zums) {
                        const auto& tools = z_info[zum].tools;

                        // Cross-product within this z:
                        // If tools = {A, B}, generate A→B and B→A (whichever missing)
                        for (int a : tools)
                            for (int b : tools)
                                maybe_add_synthetic(zum, a, b);

                        // Entry from previous sublayer z within same z_nominal:
                        // GCode enters this z with current_tool from prev_z's tool set
                        if (prev_tools) {
                            for (int old_t : *prev_tools)
                                for (int new_t : tools)
                                    maybe_add_synthetic(zum, old_t, new_t);
                        }

                        prev_tools = &tools;
                    }
                }

                // 6. NEOTKO_MPSCHEDULER_TAG s79 — canonical-order cross-z coverage.
                // The existing cross-product (steps 1-5) covers within-z all-pairs and
                // ASCENDING-adjacent z entries. But GCode now emits in canonical
                // grouped-by-tool order, which interleaves tools ACROSS z_actuals (it
                // may jump z1→z0, or skip a z). Each such consecutive transition is a
                // get_tcr(cur_z, prev_tool, cur_tool) call that must resolve. Replay the
                // exact canonical order (same espejo as GCode) and ensure every cross-z
                // transition it produces has a synthetic.
                for (const auto& [zn, zums] : znom_groups) {
                    if (zums.empty()) continue;
                    int entering = (int)m_initial_tool;
                    {
                        auto it = sub_z_to_real_tool.find(zums.front());
                        if (it != sub_z_to_real_tool.end()) entering = (int)it->second;
                    }
                    std::vector<MultiPassScheduler::SublayerKey> ord =
                        mp_group_canon_order(zn, entering);
                    for (size_t k = 1; k < ord.size(); ++k) {
                        const int   pt = ord[k - 1].tool_id;
                        const int   ct = ord[k].tool_id;
                        const uint64_t pz = z_um64(static_cast<float>(ord[k - 1].z_actual));
                        const uint64_t cz = z_um64(static_cast<float>(ord[k].z_actual));
                        if (pz != cz && pt != ct)
                            maybe_add_synthetic(cz, pt, ct);
                    }
                }

                const size_t n_synth = surf_events.size() - base_count;
                if (n_synth > 0)
                    NT_LOG("synthetic cross-product: " << n_synth << " events added");
            }

            // Sort by z_nominal, then z_actual, then pass_idx
            std::sort(surf_events.begin(), surf_events.end(),
                [](const SurfaceEvent& a, const SurfaceEvent& b) {
                    if (a.z_nominal != b.z_nominal) return a.z_nominal < b.z_nominal;
                    if (a.z_actual  != b.z_actual)  return a.z_actual  < b.z_actual;
                    return a.pass_idx < b.pass_idx;
                });

            NT_LOG("sublayer scheduler: " << surf_events.size() << " SurfaceEvents built");

            // ── Fase B: Scheduler de oportunidad ────────────────────────────
            //
            // DISEÑO (rev 2): agrupar por (old_tool, new_tool) preservando semántica
            // per-objeto.  Mejoras sobre rev 1:
            //
            //   1) Volumen MAX (no SUM): N objetos con misma (old,new) comparten UN
            //      slot del tamaño máximo necesario.  Reduce área de torre ~Nx.
            //
            //   2) Agrupación por par (old, new): los bridges TC en generate() Phase 1
            //      (section1a_initial detection) funcionan como originalmente diseñados.
            //      Los olds per-objeto se preservan en cada evento emitido.
            //
            //   3) Alias keys naturales: todos los miembros de un grupo comparten
            //      (old, new), así el redirect (z_actual, old, new)→(z_max, old, new)
            //      es exacto para cada miembro, sin chain_cur intermedio inventado.
            //
            //   4) Chain ordering greedy: arranca en chain_cur (sub_z_to_real_tool,
            //      Bug IX), encadena grupos consecutivos en la cadena, deja los
            //      huérfanos al final → minimiza bridges TC.
            //
            //   5) Prereq inner loop: pass1 events se desbloquean cuando pass0 del
            //      mismo (obj, layer) termina dentro del mismo z_nominal.
            //
            // Casos cubiertos:
            //   - 2 objetos same MP, same base    → 1 grupo (T_real, T0)        ✓
            //   - 2 objetos same MP, dif. bases  → 2 grupos (T_a, T0), (T_b, T0) ✓
            //   - 2 objetos dif. MP, same base   → 2 grupos (T_real, T0), (T_real, T2) ✓
            //   - Escalonados z_actual cercano    → fusión por z_nominal,
            //                                       redirect alias per-z_actual ✓

            std::map<std::tuple<const PrintObject*, int, int>, float> pass_completed;

            struct FusedGroup {
                int   old_tool = 0;
                int   new_tool = 0;
                float z_max    = 0.f;
                float lh_max   = 0.f;
                float vol_max  = 0.f;   // MAX, no SUM — un wipe sirve a todos
                std::vector<size_t> member_idxs;
            };

            size_t sei = 0;
            while (sei < surf_events.size()) {
                const float z_nom = surf_events[sei].z_nominal;
                size_t sej = sei;
                while (sej < surf_events.size() && surf_events[sej].z_nominal == z_nom)
                    ++sej;

                std::vector<bool> served(sej - sei, false);

                // chain_cur: real tool active antes del z_nominal group (Bug IX).
                // Avanza dentro del z_nominal según los grupos que se vayan emitiendo.
                size_t chain_cur = m_initial_tool;
                if (sei < sej) {
                    auto it = sub_z_to_real_tool.find(z_um64(surf_events[sei].z_actual));
                    if (it != sub_z_to_real_tool.end())
                        chain_cur = it->second;
                }

                bool any_emitted_round = true;
                while (any_emitted_round) {
                    any_emitted_round = false;

                    // Recolectar ready unserved (prereq satisfecho).
                    std::vector<size_t> ready_idxs;
                    for (size_t k = sei; k < sej; ++k) {
                        if (served[k - sei]) continue;
                        const SurfaceEvent& se = surf_events[k];
                        if (se.pass_idx == 0) {
                            ready_idxs.push_back(k);
                        } else {
                            auto it = pass_completed.find({se.obj, se.layer_idx, se.pass_idx - 1});
                            if (it != pass_completed.end())
                                ready_idxs.push_back(k);
                        }
                    }
                    if (ready_idxs.empty()) break;

                    // Agrupar por par (old, new). Miembros del mismo par comparten slot.
                    std::map<std::pair<int,int>, FusedGroup> groups_by_pair;
                    for (size_t ri : ready_idxs) {
                        const SurfaceEvent& se = surf_events[ri];
                        auto& g = groups_by_pair[{se.old_tool, se.new_tool}];
                        g.old_tool = se.old_tool;
                        g.new_tool = se.new_tool;
                        g.z_max    = std::max(g.z_max, se.z_actual);
                        g.lh_max   = std::max(g.lh_max, se.height);
                        g.vol_max  = std::max(g.vol_max, se.wipe_vol);
                        g.member_idxs.push_back(ri);
                    }

                    // Ordering:
                    //   Pass 1 (chain): greedy desde chain_cur — minimiza bridges.
                    //   Pass 2 (orphans): real TCs no encadenadas, ordenadas determinísticamente.
                    //   Pass 3 (identities): old==new al final (prime sentinels).
                    std::vector<FusedGroup*> ordered;
                    std::set<std::pair<int,int>> taken;
                    {
                        int cur = (int)chain_cur;
                        bool progress = true;
                        while (progress) {
                            progress = false;
                            for (auto& [key, g] : groups_by_pair) {
                                if (taken.count(key)) continue;
                                if (g.old_tool == g.new_tool) continue; // identity → pass 3
                                if (g.old_tool == cur) {
                                    ordered.push_back(&g);
                                    taken.insert(key);
                                    cur = g.new_tool;
                                    progress = true;
                                    break;
                                }
                            }
                        }
                    }
                    // Orphans real TCs.
                    std::vector<FusedGroup*> orphans_real;
                    std::vector<FusedGroup*> identities;
                    for (auto& [key, g] : groups_by_pair) {
                        if (taken.count(key)) continue;
                        if (g.old_tool == g.new_tool) identities.push_back(&g);
                        else                          orphans_real.push_back(&g);
                    }
                    auto cmp = [](const FusedGroup* a, const FusedGroup* b) {
                        if (a->z_max != b->z_max)     return a->z_max     < b->z_max;
                        if (a->old_tool != b->old_tool) return a->old_tool < b->old_tool;
                        return a->new_tool < b->new_tool;
                    };
                    std::sort(orphans_real.begin(), orphans_real.end(), cmp);
                    std::sort(identities.begin(),   identities.end(),   cmp);
                    for (FusedGroup* g : orphans_real) ordered.push_back(g);
                    for (FusedGroup* g : identities)   ordered.push_back(g);

                    // Emit.
                    for (FusedGroup* gp : ordered) {
                        const FusedGroup& g = *gp;
                        const float lh = (g.lh_max > 0.01f ? g.lh_max : (m_nozzle_diameter * 0.5f));

                        NeoTowerEvent ev;
                        ev.z_nominal    = z_nom;
                        ev.z_actual     = g.z_max;
                        ev.layer_height = lh;
                        ev.old_tool     = (size_t)g.old_tool;
                        ev.new_tool     = (size_t)g.new_tool;
                        ev.wipe_volume  = g.vol_max;
                        ev.is_sublayer  = true;
                        ev.no_ramming   = true; // NEOTKO_MPSCHEDULER_TAG s79b — sandwich band TC: visit yes, ramming deposit no
                        if (ev.old_tool == ev.new_tool)
                            m_growth_events.push_back(ev);
                        else
                            m_events.push_back(ev);
                        NT_LOG("sublayer SCHED z_max=" << g.z_max
                            << " old=" << g.old_tool << " new=" << g.new_tool
                            << " z_nom=" << z_nom << " vol=" << g.vol_max
                            << " members=" << g.member_idxs.size());

                        // Alias por miembro: cada uno comparte (old, new) con el grupo,
                        // así el redirect es exacto.
                        for (size_t mi : g.member_idxs) {
                            const SurfaceEvent& se = surf_events[mi];
                            served[mi - sei] = true;
                            // NEOTKO_MPSCHEDULER_TAG — s58 fix:
                            // Route redirects to the correct map by event type:
                            //   - Real TCs (old != new) → m_z_redirect (used by get_tcr())
                            //   - Identity events (old == new) → m_z_redirect_finish
                            //                                    (used by get_finish_layer())
                            // Mixing identities into m_z_redirect caused spurious V9 warnings
                            // because their targets live in m_finish_layer_index, not m_tcr_index.
                            if (z_um64(se.z_actual) != z_um64(g.z_max)) {
                                if (g.old_tool != g.new_tool) {
                                    uint64_t from_key = make_key(se.z_actual, g.old_tool, g.new_tool);
                                    uint64_t to_key   = make_key(g.z_max,    g.old_tool, g.new_tool);
                                    m_z_redirect[from_key] = to_key;
                                    NT_LOG("z_redirect: z=" << se.z_actual
                                        << " " << g.old_tool << "→" << g.new_tool
                                        << " → z_fused=" << g.z_max);
                                } else {
                                    m_z_redirect_finish[z_um64(se.z_actual)] = z_um64(g.z_max);
                                    NT_LOG("z_redirect_finish: z=" << se.z_actual
                                        << " T" << g.old_tool << "→T" << g.new_tool
                                        << " → z_fused=" << g.z_max);
                                }
                            }
                            // Only track pass_completed for real (non-synthetic) events.
                            // Synthetics have obj=nullptr — their key {nullptr,-1,0} is
                            // harmless but pollutes the map.  Skip for cleanliness.
                            if (se.obj != nullptr)
                                pass_completed[{se.obj, se.layer_idx, se.pass_idx}] = g.z_max;
                        }

                        // Avanzar chain_cur a la nueva posición real de la torre.
                        // Esto refleja el estado tras esta emisión: si fue chain, avanza
                        // naturalmente; si fue orphan, el bridge en generate() Phase 1
                        // hace el detour y la torre acaba en g.new_tool igualmente.
                        chain_cur = (size_t)g.new_tool;
                        any_emitted_round = true;
                    }
                } // inner while

                sei = sej;
            } // z_nominal groups

            NT_LOG("sublayer scheduler done: " << pass_completed.size() << " passes completed"
                << ", " << m_z_redirect.size() << " z_redirects");

        } else {
            NT_LOG("sublayer pass: mp_prime_vol=0, no prime events emitted");
        }
    } // sublayer second pass

    // ------------------------------------------------------------------
    // 1c. Structural layers — real layers with no toolchange events in
    //     prints with MultiPass/ColorMix sublayer activity.
    //
    //     KEY: we do NOT gate on lt.has_wipe_tower. In single-filament MP
    //     scenarios (test 1b) or mixed-object scenarios where only one
    //     object does MP (test 1a layers 25+), has_wipe_tower=0 for those
    //     real layers — yet GCode still calls is_empty_wipe_tower_gcode for
    //     them porque WipeTower2 generated T→T entries for them via
    //     plan_toolchange propagation. NeoTower must generate structural
    //     TCRs for ALL real layers when sublayer events exist.
    //
    //     Without this: tower has no GCode between z=0 and first sublayer
    //     prime → primes print in the air.
    // ------------------------------------------------------------------
    {
        // Generate structural layers whenever ANY event exists in the plan.
        // NEOTKO_NEOTOWER_TAG — s58 fix: previously gated on sublayer events only,
        // missing the case of MixedColor (and similar) that produces real TCs
        // across layers including T→T repeats (same tool as previous layer).
        // Without structural events for those T→T layers, the tower has holes.
        //
        // Rationale: if there is at least one event, the tower exists physically.
        // It must remain continuous between the first and last event z, so every
        // real layer with has_wt=true that lacks an explicit event needs a
        // structural growth event to maintain physical integrity.
        //
        // If there are no events at all (single-tool print), this block is
        // correctly skipped — no tower needed.
        const bool has_any_ev = !m_events.empty() || !m_growth_events.empty();

        if (has_any_ev) {
            // Build set of z_um values that already have events from 1a+1b.
            std::set<uint64_t> event_zs;
            for (const NeoTowerEvent& ev : m_events)
                event_zs.insert(z_um64(ev.z_actual));
            for (const NeoTowerEvent& ev : m_growth_events)
                event_zs.insert(z_um64(ev.z_actual));

            // NEOTKO_NEOTOWER_TAG — Sublayer exit tool tracking.
            // Sublayer LayerTools entries have empty extruders, so the normal
            // "for (ext_id) struct_tool=ext_id" loop never advances struct_tool
            // through a sublayer group. After a group ending on T2, struct_tool
            // would still be T1 → the first structural layer after the group
            // gets old=T1 new=T1 instead of old=T2 new=T1 → WT_MISMATCH.
            // Fix: build a map z_nom_um→last_sub_tool from existing sublayer
            // events, then update struct_tool when iterating sublayer entries.
            std::unordered_map<uint64_t, size_t> sub_group_exit_tool;
            for (const NeoTowerEvent& ev : m_events) {
                if (ev.is_sublayer)
                    sub_group_exit_tool[z_um64(ev.z_nominal)] = ev.new_tool;
            }
            for (const NeoTowerEvent& ev : m_growth_events) {
                if (ev.is_sublayer)
                    sub_group_exit_tool[z_um64(ev.z_nominal)] = ev.new_tool;
            }

            // NEOTKO_NEOTOWER_TAG — Sublayer z_um collision set.
            std::set<uint64_t> sublayer_zums;
            for (const NeoTowerEvent& ev : m_events)
                if (ev.is_sublayer)
                    sublayer_zums.insert(z_um64(ev.z_actual));
            for (const NeoTowerEvent& ev : m_growth_events)
                if (ev.is_sublayer)
                    sublayer_zums.insert(z_um64(ev.z_actual));

            // Walk ToolOrdering sequentially, tracking current_tool.
            size_t struct_tool = tool_ordering.first_extruder();
            if (struct_tool == (size_t)-1)
                struct_tool = m_initial_tool;
            bool first_real_layer_struct = true; // NEOTKO_NEOTOWER_TAG — first-layer rotation guard

            for (const LayerTools& lt : tool_ordering) {
                // Sublayers are not structural targets (they get prime events).
                // Update struct_tool to the exit tool of each sublayer group so
                // the first real layer after the group transitions correctly.
                if (lt.is_mp_sublayer) {
                    auto it_nom = sublayer_to_nominal.find(
                        z_um64(static_cast<float>(lt.print_z)));
                    if (it_nom != sublayer_to_nominal.end()) {
                        auto it_exit = sub_group_exit_tool.find(
                            z_um64(it_nom->second));
                        if (it_exit != sub_group_exit_tool.end())
                            struct_tool = it_exit->second;
                    }
                    continue;
                }

                const float    z    = static_cast<float>(lt.print_z);
                const uint64_t z_um = z_um64(z);

                // NEOTKO_NEOTOWER_TAG — Sublayer z_um collision guard:
                // Skip this real layer entirely (no struct_tool update) when its
                // z_um matches a sublayer event's z_actual.  The sublayer group
                // already encoded the tool sequence up to T_exit; updating
                // struct_tool here from lt.extruders would silently absorb the
                // pending T_exit→T_next transition.
                if (sublayer_zums.count(z_um) > 0)
                    continue;

                // NEOTKO_NEOTOWER_TAG: Skip has_wipe_tower=false layers WITHOUT
                // updating struct_tool.
                //
                // WHY (index alignment):
                //   GCode.cpp advances m_layer_idx via next_layer() ONLY for
                //   layers where layer_tools.has_wipe_tower == true.  Including
                //   has_wt=false layers in m_tool_changes creates index lag.
                //
                // WHY (struct_tool integrity):
                //   The wipe tower doesn't fire at has_wt=false layers — its
                //   physical tool state doesn't change there.  Updating
                //   struct_tool from lt.extruders at a has_wt=false layer (e.g.
                //   z=10.25 from a non-MP object that continues past a sublayer
                //   group) clobbers the T_exit value we preserved from the last
                //   sublayer.  Result: the first has_wt=true layer after the
                //   group sees old_struct_tool=T1 instead of T2 → plans T1→T1
                //   instead of T2→T1 → WT_MISMATCH crash.
                if (!lt.has_wipe_tower || lt.extruders.empty())
                    continue;

                // Capture old tool BEFORE updating from this layer's extruders.
                // For the first real layer after a sublayer group, old_struct_tool
                // is the sublayer exit tool (e.g. T2) and struct_tool becomes
                // the object tool (e.g. T1) → generates a real T2→T1 transition.
                const size_t old_struct_tool = struct_tool;

                // Track tool through real layer extruders, respecting rotation.
                // GCode.cpp rotates extruders to put current tool first → the last
                // dispatched tool is the element just before struct_tool in sorted order.
                //
                // EXCEPTION: first real layer — GCode does NOT rotate (writer is
                // nullptr), so last dispatched = .back() (sorted ascending).
                if (!first_real_layer_struct) {
                    auto it = std::find(lt.extruders.begin(), lt.extruders.end(),
                                        (unsigned int)struct_tool);
                    if (it != lt.extruders.end()) {
                        size_t n   = lt.extruders.size();
                        size_t idx = (size_t)std::distance(lt.extruders.begin(), it);
                        struct_tool = (size_t)lt.extruders[(idx + n - 1) % n];
                    } else {
                        struct_tool = (size_t)lt.extruders.back();
                    }
                } else {
                    // First real layer: no rotation, last = .back()
                    struct_tool = (size_t)lt.extruders.back();
                }
                first_real_layer_struct = false;

                // FIX A — NEOTKO_NEOTOWER_TAG: Safe floor
                float lh_raw = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                     ? lt.wipe_tower_layer_height
                                                     : lt.layer_height);
                const float lh = (lh_raw >= 0.01f) ? lh_raw : (m_nozzle_diameter * 0.5f);

                // Skip layers that already have genuine toolchange events from 1a
                // (real T0↔T1 changes).  Unlike sublayer collisions, struct_tool
                // was correctly updated above so the running tool stays accurate.
                if (event_zs.count(z_um) > 0)
                    continue;

                // Compute structural volume: 2 traversals across tower width.
                // This is the minimum to maintain physical tower integrity.
                // For real tool transitions (old≠new after a sublayer group),
                // use the wipe-volume table if available.
                float cfg_width = static_cast<float>(m_print_config->prime_tower_width);
                if (cfg_width < 10.f) cfg_width = 30.f;
                // Effective bead cross-section (WipeTower2 formula)
                float eff_width = m_perimeter_width
                                  - lh * (1.f - float(M_PI) / 4.f);
                if (eff_width < 0.01f) eff_width = m_perimeter_width * 0.8f;
                float struct_vol = 2.f * cfg_width * lh * eff_width;
                if (old_struct_tool != struct_tool
                    && old_struct_tool < m_wipe_volumes.size()
                    && struct_tool  < m_wipe_volumes[old_struct_tool].size()
                    && m_wipe_volumes[old_struct_tool][struct_tool] > 0.f)
                    struct_vol = m_wipe_volumes[old_struct_tool][struct_tool];

                NeoTowerEvent ev;
                ev.z_nominal    = z;
                ev.z_actual     = z;
                ev.layer_height = lh;
                ev.old_tool     = old_struct_tool;
                ev.new_tool     = struct_tool; // T→T structural, or T_exit→T_obj after sublayer group
                ev.wipe_volume  = struct_vol;
                ev.is_sublayer  = false;
                // NEOTKO_NEOTOWER_TAG — hardening P5: route to correct channel
                if (ev.old_tool == ev.new_tool)
                    m_growth_events.push_back(ev);
                else
                    m_events.push_back(ev);
                NT_LOG("structural event z=" << z
                    << " tool=" << struct_tool << " vol=" << struct_vol
                    << " (has_wt=" << lt.has_wipe_tower << ")"
                    << (old_struct_tool != struct_tool ? " [post-sublayer TC]" : ""));
            }
        }
    }

    // This gives a stable ordering for plan() grouping.
    auto sort_events = [](std::vector<NeoTowerEvent>& evts) {
        std::sort(evts.begin(), evts.end(),
            [](const NeoTowerEvent& a, const NeoTowerEvent& b) {
                if (a.z_actual != b.z_actual) return a.z_actual < b.z_actual;
                if (a.old_tool != b.old_tool) return a.old_tool < b.old_tool;
                return a.new_tool < b.new_tool;
            });
    };
    sort_events(m_events);
    sort_events(m_growth_events);  // NEOTKO_NEOTOWER_TAG — hardening P5

    // Deduplicate exact-key duplicates (same z_actual + old + new).
    // Accumulate wipe_volume instead of erasing, so both objects' purge needs
    // are served by a single tower slot.
    auto dedup_events = [](std::vector<NeoTowerEvent>& evts) {
        std::vector<NeoTowerEvent> deduped;
        deduped.reserve(evts.size());
        for (const NeoTowerEvent& ev : evts) {
            if (!deduped.empty()) {
                NeoTowerEvent& last = deduped.back();
                bool same_key =
                    std::llround(last.z_actual * 1000) == std::llround(ev.z_actual * 1000)
                    && last.old_tool == ev.old_tool
                    && last.new_tool == ev.new_tool;
                if (same_key) {
                    last.wipe_volume = std::max(last.wipe_volume, ev.wipe_volume);
                    //last.wipe_volume += ev.wipe_volume; WIPETOWER EDIT FIX
                    //La lógica correcta: cuando dos objetos distintos generan el mismo TC T0→T1 en z=0.65, la wipe tower solo necesita purgar una vez con el volumen suficiente para el peor caso — max(vol_obj_A, vol_obj_B). Sumar implicaría que el nozzle necesita purgar por ambos objetos secuencialmente, lo cual no es el comportamiento del wipe tower.
                    //Esto afecta tanto a ColorMix (múltiples objetos con dither en el mismo layer generan eventos idénticos) como a MultiPass (múltiples objetos con las mismas passes generan sublayer TCs idénticos). Ambos casos degeneran con += cuando hay más de un objeto en el print.
                    continue;
                }
            }
            deduped.push_back(ev);
        }
        evts = std::move(deduped);
    };
    dedup_events(m_events);
    dedup_events(m_growth_events);  // NEOTKO_NEOTOWER_TAG — hardening P5

    NT_LOG("collect_all_events: " << m_events.size() << " real_tc + "
        << m_growth_events.size() << " growth ("
        << std::count_if(m_events.begin(), m_events.end(),
               [](const NeoTowerEvent& e){ return e.is_sublayer; })
        << " sublayer in real_tc)");
}

// ===========================================================================
// PHASE 2 — plan()
//
// Group events into NeoTowerLayers.
// Compute tower geometry (square width/depth) from max wipe volume.
// Assign slot heights for sublayer events.
// ===========================================================================
void NeoTower::plan()
{
    const float saved_x = m_plan.tower_x;
    const float saved_y = m_plan.tower_y;
    m_plan = NeoTowerPlan{};
    m_plan.tower_x = saved_x;
    m_plan.tower_y = saved_y;

    if (m_events.empty() && m_growth_events.empty()) {
        NT_LOG("plan(): no events, tower is empty.");
        return;
    }

    // NEOTKO_NEOTOWER_TAG_START — hardening P5: merge both channels for planning
    std::vector<NeoTowerEvent> all_events;
    all_events.reserve(m_events.size() + m_growth_events.size());
    all_events.insert(all_events.end(), m_events.begin(), m_events.end());
    all_events.insert(all_events.end(), m_growth_events.begin(), m_growth_events.end());
    std::sort(all_events.begin(), all_events.end(),
        [](const NeoTowerEvent& a, const NeoTowerEvent& b) {
            if (std::abs(a.z_nominal - b.z_nominal) > 1e-5f)
                return a.z_nominal < b.z_nominal;
            return a.z_actual < b.z_actual;
        });
    // NEOTKO_NEOTOWER_TAG_END

    // ------------------------------------------------------------------
    // Compute tower_width from max wipe volume across all events.
    // wipe_depth = max over all events of wipe_depth_for_volume(ev.wipe_volume, slot_h)
    // tower_width = wipe_depth + 2 * perimeter_width  (square → depth == width)
    // ------------------------------------------------------------------
    float max_depth = 0.f;

    // NEOTKO_NEOTOWER_TAG — s78 fix (purge overflow): size the footprint at the REAL
    // min layer height (m_min_layer_height ≥ 0.04), NOT nozzle×0.5 (~0.2). The s67 0.2
    // floor under-reserved the footprint 5× vs the actual thin-sublayer extrusion →
    // purge overran the tower. Reserve must match extrusion (mirror of the WipeTower2
    // height_for_depth fix). Residual 0.0002 heights are already floored to 0.2 by the
    // event builder (line ~279), so 0.04 here never explodes; sandwich purge volume is
    // the small reserve knob, so real-height sizing stays reasonable (no giant tower).
    const float sizing_lh_floor = m_min_layer_height;

    for (size_t i = 0; i < all_events.size(); ++i) {
        const NeoTowerEvent& ev = all_events[i];
        
        // Use the maximum of actual event height or our sizing floor.
        float sh_for_sizing = std::max(ev.layer_height, sizing_lh_floor);
        
        // Use a provisional tower_width for the depth calculation.
        // We iterate once with a rough estimate, then finalize.
        // For the first pass use the config prime_tower_width as seed.
        float provisional_width = static_cast<float>(m_print_config->prime_tower_width);
        if (provisional_width < 10.f) provisional_width = 10.f;
        
        float d = wipe_depth_for_volume(ev.wipe_volume, sh_for_sizing);
        if (d > max_depth) max_depth = d;
    }

    // Final tower dimensions: decoupled width (from config) and depth (from calculations).
    float cfg_width = static_cast<float>(m_print_config->prime_tower_width);
    float computed_depth = max_depth + 2.f * m_perimeter_width;
    m_plan.tower_width = cfg_width;
    m_plan.tower_depth = computed_depth; // rectangular - decoupled from width

    // ------------------------------------------------------------------
    // Group events by z_nominal into NeoTowerLayers.
    // Within each layer, sublayer events become sub-slots at their z_actual.
    // The nominal-layer event (z_actual == z_nominal) is the top slot.
    // ------------------------------------------------------------------

    // Build a map from z_nominal → layer index for fast lookup.
    // We walk events in z_actual order; group by z_nominal.
    std::unordered_map<uint64_t, size_t> layer_map; // z_nominal_um → layer index

    auto z_key = [](float z) -> uint64_t {
        return static_cast<uint64_t>(std::llround(z * 1000.f));
    };

    for (size_t ei = 0; ei < all_events.size(); ++ei) {
        const NeoTowerEvent& ev = all_events[ei];
        uint64_t nom_key = z_key(ev.z_nominal);

        if (layer_map.find(nom_key) == layer_map.end()) {
            NeoTowerLayer layer;
            layer.z_nominal    = ev.z_nominal;
            layer.layer_height = ev.layer_height;
            layer.needs_structure = true;
            layer_map[nom_key] = m_plan.layers.size();
            m_plan.layers.push_back(std::move(layer));
        }

        NeoTowerLayer& layer = m_plan.layers[layer_map[nom_key]];

        NeoTowerSlot slot;
        slot.z           = ev.z_actual;
        slot.old_tool    = ev.old_tool;
        slot.new_tool    = ev.new_tool;
        slot.wipe_volume = ev.wipe_volume;
        slot.event_idx   = ei;
        slot.slot_height = ev.is_sublayer
                           ? sublayer_slot_height(ev.layer_height)
                           : ev.layer_height;

        layer.slots.push_back(slot);
        NT_LOG("plan slot: layer_z=" << layer.z_nominal
            << " slot_z=" << slot.z << " old=" << slot.old_tool
            << " new=" << slot.new_tool << " vol=" << slot.wipe_volume
            << " h=" << slot.slot_height << (ev.is_sublayer ? " [sub]" : " [real]"));
    }   // for ei

    // Sort layers by z_nominal ascending.
    std::sort(m_plan.layers.begin(), m_plan.layers.end(),
        [](const NeoTowerLayer& a, const NeoTowerLayer& b) {
            return a.z_nominal < b.z_nominal;
        });

    // Sort slots within each layer by z ascending (sublayers first, nominal last).
    for (NeoTowerLayer& layer : m_plan.layers) {
        std::sort(layer.slots.begin(), layer.slots.end(),
            [](const NeoTowerSlot& a, const NeoTowerSlot& b) {
                return a.z < b.z;
            });
    }

    NT_LOG("plan(): " << m_plan.layers.size()
        << " layers, tower_width=" << m_plan.tower_width << "mm");
}

// ---------------------------------------------------------------------------
// wipe_depth_for_volume() — geometry helper
//
// Mirrors WipeTower2's internal calculation for how much Y-depth is needed
// to extrude a given volume of filament in a slot of the given height.
// Simplified: depth = volume / (width * slot_height * extrusion_efficiency)
// ---------------------------------------------------------------------------
float NeoTower::wipe_depth_for_volume(float wipe_volume, float slot_height) const
{
    // Effective tower width (usable for wipe, subtracting perimeters).
    const float usable_width = m_plan.tower_width > 0.f
                               ? m_plan.tower_width - 2.f * m_perimeter_width
                               : static_cast<float>(m_print_config->prime_tower_width)
                                 - 2.f * m_perimeter_width;
    if (usable_width <= 0.f) return 10.f;

    // Stadium bead cross-section: A = H * (W - H*(1 - pi/4))
    // NEOTKO_NEOTOWER_TAG — Hallazgo VI: old formula omitted the stadium correction
    // term, causing depth underestimation of 1.8–8.9% depending on slot_height.
    constexpr float k = 1.f - static_cast<float>(M_PI) / 4.f;  // 0.2146
    const float vol_per_mm = slot_height * (m_perimeter_width - slot_height * k)
                             * usable_width;
    if (vol_per_mm <= 0.f) return 10.f;

    // NEOTKO_NEOTOWER_TAG: Fix unit mismatch.
    // wipe_volume / vol_per_mm gives the dimensionless count of lines required.
    // To convert it to a physical depth in mm, we must multiply by the line spacing
    // (perimeter_width * extra_spacing_wipe). Without this, the calculated depth
    // was inflated by a factor of 1 / perimeter_width (typically 2.0x).
    const float extra_spacing_wipe = (static_cast<float>(m_print_config->wipe_tower_extra_spacing.value) / 100.f) *
                                     (static_cast<float>(m_print_config->wipe_tower_extra_flow.value) / 100.f);
    const float line_spacing = m_perimeter_width * extra_spacing_wipe;
    const float calculated_depth = (wipe_volume / vol_per_mm) * line_spacing;

    return std::max(2.f * m_perimeter_width, calculated_depth);
}

// ---------------------------------------------------------------------------
// sublayer_slot_height()
// ---------------------------------------------------------------------------
float NeoTower::sublayer_slot_height(float nominal_layer_height) const
{
    // A sublayer slot gets at most 40% of the nominal layer height,
    // bounded below by m_min_layer_height (>= 0.04 mm).
    float h = std::min(m_min_layer_height, nominal_layer_height * 0.4f);
    return std::max(0.04f, h);
}

// ---------------------------------------------------------------------------
// get_height()
// ---------------------------------------------------------------------------
float NeoTower::get_height() const
{
    if (m_plan.layers.empty()) return 0.f;
    const NeoTowerLayer& top = m_plan.layers.back();
    if (!top.slots.empty())
        return top.slots.back().z + top.slots.back().slot_height;
    return top.z_nominal + top.layer_height;
}

// ===========================================================================
// PHASE 3 — generate()
//
// Iterate m_plan.layers. For each slot, call wt2.set_layer() + local_z_tool_change().
// Build m_result and m_tcr_index.
// ===========================================================================

void NeoTower::generate(std::vector<std::vector<WipeTower::ToolChangeResult>>& result)
{
    result.clear();
    m_result.clear();
    m_tcr_index.clear();
    m_finish_layer_index.clear();
    m_merged_tcrs.clear();
    m_merged_index.clear();
    // NEOTKO_MPSCHEDULER_TAG: z_redirect maps are populated in collect_all_events(),
    // NOT cleared here — they must persist through generate() for get_tcr()/get_finish_layer().
    m_num_toolchanges = 0;

    if (m_events.empty() && m_growth_events.empty()) return;

    // NEOTKO_NEOTOWER_TAG_START — hardening P5: merge channels for generate
    std::vector<NeoTowerEvent> all_events;
    all_events.reserve(m_events.size() + m_growth_events.size());
    all_events.insert(all_events.end(), m_events.begin(), m_events.end());
    all_events.insert(all_events.end(), m_growth_events.begin(), m_growth_events.end());
    std::sort(all_events.begin(), all_events.end(),
        [](const NeoTowerEvent& a, const NeoTowerEvent& b) {
            if (std::abs(a.z_nominal - b.z_nominal) > 1e-5f)
                return a.z_nominal < b.z_nominal;
            return a.z_actual < b.z_actual;
        });
    // NEOTKO_NEOTOWER_TAG_END

    // NEOTKO_NEOTOWER_TAG_START: effective_initial_tool
    // WipeTower2 generates the first-layer brim in the FIRST plan_toolchange()
    // call.  The chain-ordering in Phase 1 starts from chain_tool and feeds that
    // TC first — so WipeTower2 puts the brim content in the TCR for that TC.
    //
    // m_initial_tool comes from ToolOrdering::first_extruder() which can differ
    // from the actual extruder GCode has active when it first calls the tower.
    // GCode's current_tool at layer 0 is the chain-start of the first real layer:
    // the tool whose old_tool does NOT appear as new_tool of any other event at
    // the same (minimum) z.  That tool is the one GCode starts on, so the brim
    // TCR must have it as initial_tool.
    //
    // IMPORTANT: do NOT derive effective_initial from m_events[0].old_tool after
    // sort_events().  sort_events() sorts by (z_actual, old_tool, new_tool), so
    // it puts T0→T2 before T3→T0 at the same z, making effective_initial=T0
    // even when GCode starts at T3.  The chain-start detection below is
    // order-independent and always produces the correct result.
    size_t effective_initial = m_initial_tool;
    {
        // Find the minimum z among non-sublayer, old!=new events.
        float min_z = std::numeric_limits<float>::max();
        for (const NeoTowerEvent& ev : m_events)
            if (!ev.is_sublayer && ev.old_tool != ev.new_tool)
                min_z = std::min(min_z, ev.z_actual);

        if (min_z < std::numeric_limits<float>::max()) {
            // Collect new_tool values at min_z (= "consumed" tools — someone
            // transitions INTO them, so they are NOT the chain start).
            std::set<size_t> consumed;
            for (const NeoTowerEvent& ev : m_events)
                if (!ev.is_sublayer && ev.old_tool != ev.new_tool
                        && std::abs(ev.z_actual - min_z) < 1e-5f)
                    consumed.insert(ev.new_tool);

            // The chain-start old_tool is the one NOT consumed by any other
            // event at min_z.  There is exactly one such tool in a valid chain.
            for (const NeoTowerEvent& ev : m_events) {
                if (!ev.is_sublayer && ev.old_tool != ev.new_tool
                        && std::abs(ev.z_actual - min_z) < 1e-5f
                        && consumed.find(ev.old_tool) == consumed.end()) {
                    effective_initial = ev.old_tool;
                    break;
                }
            }
        }
    }
    // NEOTKO_NEOTOWER_TAG_END

    // Construct one WipeTower2 instance for the whole print.
    // We use its plan_toolchange() + generate() path to get proper
    // rectangular fill TCRs (finish_layer pattern) — not local_z_tool_change().
    WipeTower2 wt2(*m_print_config,
                   *m_region_config,
                   m_plate_idx,
                   m_plate_origin,
                   m_wipe_volumes,
                   effective_initial);  // NEOTKO_NEOTOWER_TAG

    // Register all extruders so WipeTower2's m_filpar is fully populated.
    std::set<size_t> all_tools;
    all_tools.insert(0);             // ← AÑADIR ESTO
    // WipeTower2::filament_area() = m_filpar[0].filament_area (hardcoded).
    // T0 MUST always be initialized regardless of which tools are actually used.
    all_tools.insert(effective_initial);
    for (const NeoTowerEvent& ev : all_events) {
        all_tools.insert(ev.old_tool);
        all_tools.insert(ev.new_tool);
    }
    for (size_t t : all_tools)
        wt2.set_extruder(t, *m_print_config);

    // -----------------------------------------------------------------------
    // Phase 1: Feed plan to WipeTower2 via plan_toolchange().
    //
    // Events are sorted by z_actual (then by old_tool for dedup stability).
    // WipeTower2 groups events at the same z into one plan layer and tracks
    // m_current_tool continuously across layers.  If same-z events are fed in
    // arbitrary order the internal tool state diverges → TC at the next layer
    // appears as T→T (no-op) → neotko_is_single_tool_mp_layer fires falsely.
    //
    // Fix: within each same-z group we chain events so that each TC's old_tool
    // matches the tool state left by the previous TC (greedy chain-building).
    // T→T structural events don't alter state and are processed last.
    //
    // We track:
    //   event_to_wt2_li[ei]     — which WipeTower2 plan layer index
    //   event_to_wt2_tc_idx[ei] — position within that layer's raw_result[li]
    //                             (-1 for T→T structural / sublayer T→T events)
    // -----------------------------------------------------------------------
    // NEOTKO_FIX s49: was 1e-3f (0.001mm). The last sublayer in a group sits ~0.0002mm below
    // the nominal real-layer Z (e.g. sublayer z=9.8498 vs real z=9.85, diff=0.0002mm).
    // 1e-3f > 0.0002mm → they merged into ONE z-group → one fewer m_result[] entry per pair.
    // GCode's m_layer_idx advances separately for every has_wipe_tower LayerTools entry,
    // causing m_layer_idx to desync (+1 per merged pair) → wrong TCR lookups → crash.
    // 1e-5f (0.00001mm): 0.0002mm gap > 1e-5 so sublayer/real stay separate z-groups.
    // True float duplicates differ by < 1e-6 < 1e-5 so identical Z values still merge.
    // NEOTKO_NEOTOWER_TAG — hardening P2: use central epsilon
    constexpr float NT_WT_EPS = NeoTowerZ::Z_EPS_GROUP;

    std::vector<int> event_to_wt2_li(all_events.size(), -1);
    std::vector<int> event_to_wt2_tc_idx(all_events.size(), -1);

    // NEOTKO_NEOTOWER_TAG: bridge TCs injected when chain_tool ≠ ev.old_tool for a real TC.
    // GCode dispatches get_tcr(z, chain_tool, ev.old_tool) via WipeTowerPurge path.
    // We must plan that TC in wt2 and register its TCR in m_tcr_index.
    // at_nominal_z: true when bridge follows sublayer events and is planned at
    // z_nominal (separate wt2 plan layer from the real event).  False for bridges
    // at the same z as the real event (merged into one wt2 plan layer).
    struct BridgeTc { float z; int from_tool; int to_tool; int wt2_li; bool at_nominal_z; };
    std::vector<BridgeTc> bridge_tcs;

    int   wt2_li       = -1;
    int   chain_tool   = static_cast<int>(effective_initial); // NEOTKO_NEOTOWER_TAG (was m_initial_tool)
    // tc_count_per_wt2_li removed — tc_idx is now derived from raw_result after generate(),
    // not from the feed order.  See "Re-derive tc_idx" block below.

    // Helper: feed one event to wt2 in chain order, updating bookkeeping.
    auto feed_event = [&](size_t evi) {
        const NeoTowerEvent& ev = all_events[evi];
        event_to_wt2_li[evi] = wt2_li;
        wt2.plan_toolchange(
            ev.z_actual, ev.layer_height,
            static_cast<unsigned int>(ev.old_tool),
            static_cast<unsigned int>(ev.new_tool),
            ev.wipe_volume,
            ev.no_ramming); // NEOTKO_MPSCHEDULER_TAG s79b — sandwich TCs skip ramming deposit
        if (ev.old_tool != ev.new_tool) {
            // event_to_wt2_tc_idx will be filled after wt2.generate() — see below.
            chain_tool = static_cast<int>(ev.new_tool);
        }
    };

    // NEOTKO_NEOTOWER_TAG_START: phantom-init skip
    // When m_initial_tool != effective_initial, collect_all_events() emits
    // a phantom TC (old=m_initial_tool → new=X) at the first real z-group.
    // This TC arises from the is_first_on_first exception in collect_all_events:
    // it mirrors WipeTower2's "start from initial_tool" convention, but GCode
    // actually starts from effective_initial — so GCode never dispatches this TC.
    //
    // Feeding the phantom to WT2 would make WT2's m_current_tool end at
    // effective_initial after layer 0 (phantom is last), but GCode's
    // current_tool ends at effective_initial's TC new_tool → state divergence
    // → crash at layer_idx=2.
    //
    // Fix: in the first z-group, pre-mark phantom events as used (skip them).
    // After the first z-group is processed, first_zgrp_done=true disables the
    // guard for all subsequent layers (where phantoms cannot appear).
    bool first_zgrp_done = false;

    auto is_phantom_init = [&](size_t abs_evi) -> bool {
        if (first_zgrp_done) return false;
        if (m_initial_tool == effective_initial) return false;
        // NEOTKO_NEOTOWER_TAG — bug fix: only skip the specific phantom TC:
        //   old=m_initial_tool → new=effective_initial
        // This is the one TC that GCode never dispatches because it starts at
        // effective_initial, not m_initial_tool.  All other TCs with
        // old==m_initial_tool (e.g. T1→T2 in a multi-tool layer) are legitimate
        // and must NOT be skipped — skipping them causes get_tcr() MISS and
        // "unexpected toolchange" when NeoTower's wt2 omits them from the plan.
        const NeoTowerEvent& ev = all_events[abs_evi];
        bool ph = (ev.old_tool == m_initial_tool && ev.new_tool == effective_initial);
        if (ph)
            NT_LOG("phantom-init skip z=" << ev.z_actual
                << " old=" << ev.old_tool
                << " new=" << ev.new_tool
                << " (m_initial=" << m_initial_tool
                << " effective=" << effective_initial << ")");
        return ph;
    };
    // NEOTKO_NEOTOWER_TAG_END

    // NEOTKO_NEOTOWER_TAG: track the z_nominal of the most recent sublayer group.
    // When a bridge TC follows sublayer events, plan the bridge at this z_nominal
    // (the parent real layer z) instead of the next real event's z_actual.
    // This creates a separate wt2 plan layer for the bridge, matching the z at which
    // GCode dispatches get_tcr() from WipeTowerPurge (fixes Bug 00_).
    float last_sublayer_z_nominal = -1.f;

    size_t ei = 0;
    while (ei < all_events.size()) {
        const float z_grp = all_events[ei].z_actual;

        // Locate end of this z-group.
        size_t ei_end = ei + 1;
        while (ei_end < all_events.size() &&
               std::abs(all_events[ei_end].z_actual - z_grp) <= NT_WT_EPS)
            ++ei_end;

        ++wt2_li;

        const size_t grp_size = ei_end - ei;
        if (grp_size == 1) {
            // Single event: nothing to chain.
            {
                const NeoTowerEvent& _gev = all_events[ei];
                const bool _is_identity   = (_gev.old_tool == _gev.new_tool);
                const bool _chain_gap     = (chain_tool != static_cast<int>(_gev.old_tool));
                NT_LOG("GROUP_SINGLE: wt2_li=" << wt2_li
                    << " z=" << _gev.z_actual
                    << " old=" << _gev.old_tool
                    << " new=" << _gev.new_tool
                    << " chain_tool=" << chain_tool
                    << " is_sublayer=" << _gev.is_sublayer
                    << (_is_identity  ? " [IDENTITY]"                    : "")
                    << (_chain_gap    ? " [GAP: chain_tool!=old_tool]"   : ""));

                if (_chain_gap && _is_identity) {
                    // NEOTKO_NEOTOWER_TAG: cross-group gap fix for identity (T→T) events.
                    // WipeTower2 enters this plan layer with m_current_tool=chain_tool, but
                    // the finish_layer TCR must carry the correct initial_tool (ev.old_tool).
                    // set_tool_override fixes the initial_tool without injecting a real TC.
                    wt2.set_tool_override(static_cast<size_t>(wt2_li),
                                          static_cast<unsigned int>(_gev.old_tool));
                    // For identity events, feed_event won't update chain_tool (old==new).
                    chain_tool = static_cast<int>(_gev.new_tool);
                    NT_LOG("GROUP_SINGLE: set_tool_override wt2_li=" << wt2_li
                        << " override_tool=" << _gev.old_tool
                        << " chain_tool_updated=" << chain_tool);
                } else if (_chain_gap && !_is_identity) {
                    // NEOTKO_NEOTOWER_TAG: bridge TC injection for real-TC gaps.
                    // When MpGroupState ends with WipeTowerPurge, GCode calls
                    //   get_tcr(z, chain_tool, ev.old_tool)
                    // expecting NeoTower to handle the chain_tool→ev.old_tool purge.
                    // Inject this bridge TC into wt2's plan so generate() produces a TCR
                    // for it, then register it in m_tcr_index (Phase 3 below).
                    // No set_tool_override needed: the bridge feeds the chain naturally.
                    //
                    // NEOTKO_NEOTOWER_TAG — bridge-at-nominal-z (Bug 00_ fix):
                    // When bridge follows sublayer events AND the current event is a
                    // real layer (not sublayer), GCode dispatches the bridge at the
                    // sublayer group's z_nominal (the parent real layer z), NOT at the
                    // next real event's z_actual.  Plan the bridge at z_nominal so it
                    // gets its own wt2 plan layer.
                    // CRITICAL: do NOT apply this for sublayer-to-sublayer bridges!
                    // Consecutive sublayer z-groups with chain gaps must use z_actual
                    // for the bridge, not z_nominal — otherwise wt2_li desyncs and
                    // all subsequent events get wrong plan layer assignments.
                    const bool bridge_follows_sublayer =
                        (last_sublayer_z_nominal > 0.f && !_gev.is_sublayer);
                    if (bridge_follows_sublayer) {
                        // NEOTKO_NEOTOWER_TAG — s78 fix (append_tcr THROW on multi-object
                        // ColorMix: A=gradient + B=inverted gradient + C=plain on top).
                        // For the real event right after a sublayer group, ev.old_tool is
                        // AUTHORITATIVE — 1a tracked it from the actual sublayer exit (the
                        // GCode writer's tool at the nominal layer, via sublayer_z_tool).
                        // The greedy multi-group reorder above can leave `chain_tool` on a
                        // DIFFERENT tool than GCode actually ends on (GCode emits buckets in
                        // object/spatial order, NOT NeoTower's chain order). That stale
                        // chain_tool previously injected a SPURIOUS bridge chain_tool→ev.old
                        // merged into the real layer ([bridge, real] = 2 TCs); GCode mean-
                        // while reconciles the writer via MP_GROUP_END (BareRecover) to
                        // ev.old→ev.new, so the bridge is never dispatched → leftover plan
                        // entry → "append_tcr unexpected toolchange" THROW. Fix: trust
                        // ev.old_tool, inject NO bridge (GCode is already at ev.old, so a
                        // get_tcr(ev.old,ev.old) purge would be identity anyway).
                        NT_LOG("GROUP_SINGLE: post-sublayer real event — trust ev.old=T"
                            << _gev.old_tool << " (stale chain_tool=T" << chain_tool
                            << "), bridge SUPPRESSED");
                        chain_tool = static_cast<int>(_gev.old_tool);
                        last_sublayer_z_nominal = -1.f;  // consumed
                    } else {
                        // Sublayer-to-sublayer (or non-post-sublayer) gap: inject the
                        // bridge at the event's actual z — original behaviour.
                        wt2.plan_toolchange(_gev.z_actual, _gev.layer_height,
                                            static_cast<unsigned int>(chain_tool),
                                            static_cast<unsigned int>(_gev.old_tool),
                                            0.f,
                                            /*skip_ramming=*/true); // NEOTKO_MPSCHEDULER_TAG s79b — sandwich bridge
                        bridge_tcs.push_back({_gev.z_actual, chain_tool,
                                              static_cast<int>(_gev.old_tool), wt2_li,
                                              false});
                        NT_LOG("GROUP_SINGLE: bridge TC z=" << _gev.z_actual
                            << " (at actual) chain=" << chain_tool
                            << " → ev.old=" << _gev.old_tool);
                    }
                    // chain_tool will be updated to ev.new_tool by feed_event() below.
                }
            }
            if (!is_phantom_init(ei))  // NEOTKO_NEOTOWER_TAG: skip phantom
                feed_event(ei);
        } else {
            // Multiple events at same z.  Build a valid TC chain starting from
            // chain_tool, then append any unchained (T→T) events at the end.
            NT_LOG("GROUP_MULTI: wt2_li=" << wt2_li
                << " z=" << all_events[ei].z_actual
                << " grp_size=" << grp_size
                << " chain_tool=" << chain_tool);
            std::vector<bool> used(grp_size, false);
            std::vector<size_t> ordered;
            ordered.reserve(grp_size);

            // NEOTKO_NEOTOWER_TAG: do NOT pre-mark phantom events in GROUP_MULTI.
            // In GROUP_MULTI, when m_initial_tool ≠ effective_initial and the first
            // z-group has BOTH the "phantom" TC (old=m_initial → new=effective) AND a
            // real TC from another object with the same (old, new) pair, the phantom
            // pre-marking incorrectly skips the real TC — causing a get_tcr() miss and
            // crash later.
            //
            // The is_phantom_init guard in GROUP_SINGLE is still correct (single-event
            // layers where the only event is the phantom can safely be skipped).
            //
            // In GROUP_MULTI, the greedy chain starting from chain_tool=effective_initial
            // naturally orders events as [real-TC-1, real-TC-2, ...] so that wt2 receives
            // the first TC's initial_tool = effective_initial.  When effective_initial ==
            // plan.front().initial_tool, wt2 initialises m_current_tool to effective_initial
            // and generates NO phantom internally — the phantom only appears when
            // m_current_tool ≠ first_TC.initial_tool, which the greedy ordering prevents.

            // NEOTKO_NEOTOWER_TAG: bridge TC injection for GROUP_MULTI.
            // Find section1a_initial: the old_tool that is NOT the new_tool of any
            // other real TC in the group.  This is the true ToolOrdering chain start —
            // the tool section 1a had as current_tool when it entered this layer.
            // When chain_tool ≠ section1a_initial (WipeTowerPurge scenario), GCode
            // dispatches a bridge TC (chain_tool → section1a_initial) before the
            // regular layer TCs.  Plan that bridge in wt2 so get_tcr() can serve it.
            {
                std::set<unsigned> grp_new_tools;
                for (size_t k = 0; k < grp_size; ++k) {
                    if (used[k]) continue;
                    const NeoTowerEvent& ev = all_events[ei + k];
                    // NEOTKO_NEOTOWER_TAG — brim fix: phantom events (m_initial→effective)
                    // must not influence section1a_initial detection.  Exclude them from
                    // grp_new_tools so they don't hide the real chain-start tool.
                    if (ev.old_tool != ev.new_tool && !is_phantom_init(ei + k))
                        grp_new_tools.insert(static_cast<unsigned>(ev.new_tool));
                }
                int section1a_initial = chain_tool;
                for (size_t k = 0; k < grp_size; ++k) {
                    if (used[k]) continue;
                    const NeoTowerEvent& ev = all_events[ei + k];
                    if (ev.old_tool == ev.new_tool) continue; // skip identity
                    // NEOTKO_NEOTOWER_TAG — brim fix: phantom is not a real chain start.
                    if (is_phantom_init(ei + k)) continue;
                    if (grp_new_tools.find(static_cast<unsigned>(ev.old_tool)) == grp_new_tools.end()) {
                        section1a_initial = static_cast<int>(ev.old_tool);
                        break;
                    }
                }
                if (section1a_initial != chain_tool) {
                    // NEOTKO_NEOTOWER_TAG — bridge-at-nominal-z (Bug 00_ fix):
                    // Same logic as GROUP_SINGLE: when bridge follows sublayers
                    // AND current group is a real layer (not sublayer), plan at
                    // z_nominal.  Sublayer-to-sublayer bridges use z_actual.
                    const bool bridge_follows_sublayer =
                        (last_sublayer_z_nominal > 0.f && !all_events[ei].is_sublayer);
                    const float bridge_z = bridge_follows_sublayer
                                           ? last_sublayer_z_nominal
                                           : all_events[ei].z_actual;
                    wt2.plan_toolchange(bridge_z, all_events[ei].layer_height,
                                        static_cast<unsigned int>(chain_tool),
                                        static_cast<unsigned int>(section1a_initial),
                                        0.f,
                                        /*skip_ramming=*/true); // NEOTKO_MPSCHEDULER_TAG s79b — sandwich bridge
                    bridge_tcs.push_back({bridge_z, chain_tool,
                                          section1a_initial, wt2_li,
                                          bridge_follows_sublayer});
                    NT_LOG("GROUP_MULTI: bridge TC z=" << bridge_z
                        << (bridge_follows_sublayer ? " (at nominal)" : " (at actual)")
                        << " chain=" << chain_tool << " → s1a_init=" << section1a_initial);
                    if (bridge_follows_sublayer) {
                        // Only create a separate wt2 plan layer when bridge_z differs
                        // from the real event z.  When they match, WipeTower2 merges
                        // them into one layer — incrementing wt2_li would cause drift.
                        if (std::abs(bridge_z - all_events[ei].z_actual) > NT_WT_EPS) {
                            ++wt2_li;
                            NT_LOG("GROUP_MULTI: bridge separate layer → wt2_li=" << wt2_li);
                        } else {
                            NT_LOG("GROUP_MULTI: bridge same-z as real (" << bridge_z
                                << " ≈ " << all_events[ei].z_actual << ") → no wt2_li++");
                        }
                        last_sublayer_z_nominal = -1.f;  // consumed
                    }
                    chain_tool = section1a_initial; // advance chain so Pass 1 starts correctly
                }
            }

            // Pass 1: greedy real-TC chain starting from chain_tool (= section1a_initial
            // after bridge injection, or the original chain_tool if no bridge needed).
            int cur = chain_tool;
            bool found_any = true;
            while (found_any && ordered.size() < grp_size) {
                found_any = false;
                for (size_t k = 0; k < grp_size; ++k) {
                    if (used[k]) continue;
                    const NeoTowerEvent& ev = all_events[ei + k];
                    if (ev.old_tool == ev.new_tool) continue; // T→T: defer
                    if (static_cast<int>(ev.old_tool) == cur) {
                        ordered.push_back(ei + k);
                        used[k] = true;
                        cur = static_cast<int>(ev.new_tool);
                        found_any = true;
                        break;
                    }
                }
            }
            // Pass 2: append T→T and any unchained events.
            for (size_t k = 0; k < grp_size; ++k) {
                if (!used[k]) ordered.push_back(ei + k);
            }

            // NEOTKO_NEOTOWER_TAG — brim fix: skip phantom init events so wt2's
            // m_current_tool stays consistent and the first real TC at layer 0
            // retains the brim-generating position (si=0 of wt2_li=0).
            for (size_t evi : ordered)
                if (!is_phantom_init(evi))
                    feed_event(evi);
        }

        first_zgrp_done = true;  // NEOTKO_NEOTOWER_TAG: disable phantom guard after first group

        // NEOTKO_NEOTOWER_TAG: update sublayer z_nominal tracker.
        // If ANY event in this z-group is a sublayer, record its z_nominal.
        // This is used by the bridge-at-nominal-z logic: when the next z-group
        // has a bridge gap AND follows sublayers, the bridge is planned at z_nominal
        // rather than at the next event's z_actual.
        // Reset to -1 when the z-group is a non-sublayer (real) layer.
        {
            bool grp_has_sublayer = false;
            for (size_t k = ei; k < ei_end; ++k) {
                if (all_events[k].is_sublayer) {
                    last_sublayer_z_nominal = all_events[k].z_nominal;
                    grp_has_sublayer = true;
                    break;
                }
            }
            if (!grp_has_sublayer)
                last_sublayer_z_nominal = -1.f;
        }

        ei = ei_end;
    }

    // -----------------------------------------------------------------------
    // Phase 2: Generate proper rectangular-fill TCRs.
    //   T→T-only plan layers  → raw_result[li] = {finish_layer_tcr}  (1 element)
    //   Layers with N real TCs → raw_result[li] = N elements,
    //                            finish_layer merged into one of them
    // -----------------------------------------------------------------------
    std::vector<std::vector<WipeTower::ToolChangeResult>> raw_result;
    std::vector<std::vector<WipeTower::ToolChangeResult>> local_z_result;
    wt2.generate(raw_result, local_z_result);
    m_used_filament = wt2.get_used_filament();

    // NEOTKO_DBG: dump all generated TCRs to confirm m_current_tool state per layer
    for (int _li = 0; _li < static_cast<int>(raw_result.size()); ++_li) {
        for (int _si = 0; _si < static_cast<int>(raw_result[_li].size()); ++_si) {
            NT_LOG("WT2_RAW: li=" << _li
                << " si=" << _si
                << " z=" << raw_result[_li][_si].print_z
                << " initial=" << raw_result[_li][_si].initial_tool
                << " new="     << raw_result[_li][_si].new_tool);
        }
    }

    // m_result stores WipeTower2-indexed TCRs; get_tcr/get_finish_layer
    // look up m_result[wt2_li][tc_idx] via the maps built below.
    m_result = raw_result;

    // -----------------------------------------------------------------------
    // Re-derive tc_idx from raw_result (wt2 output order != feed order).
    //
    // wt2.generate() produces TCRs in its own internal order, which may differ
    // from the order NeoTower fed plan_toolchange().  The feed order is
    // chain-corrected (greedy), but wt2 may re-sort within a layer.
    //
    // Strategy: for each real-TC event (old != new), search raw_result[li]
    // for a TCR whose (initial_tool, new_tool) matches (ev.old_tool, ev.new_tool).
    // This is O(N*M) where M is TCRs per layer — typically ≤ 4, so negligible.
    //
    // Ambiguity: two events with the same (old, new) at the same z are impossible
    // after deduplication (dedup accumulates volume → single event).
    // -----------------------------------------------------------------------
    for (size_t ei = 0; ei < all_events.size(); ++ei) {
        const NeoTowerEvent& ev = all_events[ei];
        if (ev.old_tool == ev.new_tool) continue; // structural T→T: handled below
        const int li = event_to_wt2_li[ei];
        if (li < 0 || li >= static_cast<int>(raw_result.size()) || raw_result[li].empty())
            continue;

        bool found = false;
        for (int si = 0; si < static_cast<int>(raw_result[li].size()); ++si) {
            if (static_cast<size_t>(raw_result[li][si].initial_tool) == ev.old_tool &&
                static_cast<size_t>(raw_result[li][si].new_tool)     == ev.new_tool) {
                event_to_wt2_tc_idx[ei] = si;
                found = true;
                break;
            }
        }
        if (!found) {
                    // GAP fallback: set_tool_override changed initial_tool in the TCR,
                    // so the exact (initial==ev.old_tool) match above failed.
                    // Search only by new_tool — there can be at most one TCR per new_tool
                    // within a layer after deduplication.
                    for (int si = 0; si < static_cast<int>(raw_result[li].size()); ++si) {
                        if (static_cast<size_t>(raw_result[li][si].new_tool) == ev.new_tool) {
                            event_to_wt2_tc_idx[ei] = si;
                            found = true;
                            NT_LOG("generate GAP-FALLBACK: z=" << ev.z_actual
                                << " ev.old=" << ev.old_tool
                                << " raw_initial=" << raw_result[li][si].initial_tool
                                << " new=" << ev.new_tool);
                            break;
                        }
                    }
                    if (!found)
                        NT_LOG("generate WARNING: no raw_result match for z=" << ev.z_actual
                            << " old=" << ev.old_tool << " new=" << ev.new_tool
                            << " in li=" << li
                            << " (raw_result[li].size()=" << raw_result[li].size() << ")"
                            << " — dumping layer TCRs:");
                    for (int si = 0; si < static_cast<int>(raw_result[li].size()); ++si) {
                        NT_LOG("  raw_result[" << li << "][" << si << "]"
                            << " initial=" << raw_result[li][si].initial_tool
                            << " new=" << raw_result[li][si].new_tool);
                    }
                }
    }

    // -----------------------------------------------------------------------
    // Phase 3: Build lookup indices using corrected tc_idx values.
    // -----------------------------------------------------------------------
    for (size_t ei = 0; ei < all_events.size(); ++ei) {
        const NeoTowerEvent& ev      = all_events[ei];
        const int            li      = event_to_wt2_li[ei];
        const int            tc_idx  = event_to_wt2_tc_idx[ei];

        if (li < 0 || li >= static_cast<int>(raw_result.size()) || raw_result[li].empty())
            continue;

        if (ev.old_tool != ev.new_tool) {
            // Real toolchange — map (z, old, new) → raw_result[li][tc_idx].
            if (tc_idx >= 0 && tc_idx < static_cast<int>(raw_result[li].size())) {
                const uint64_t key = make_key(ev.z_actual, ev.old_tool, ev.new_tool);
                m_tcr_index[key] = {static_cast<size_t>(li),
                                    static_cast<size_t>(tc_idx)};
                NT_LOG("generate tcr [" << li << "][" << tc_idx << "]"
                    << " z=" << ev.z_actual
                    << " old=" << ev.old_tool << " new=" << ev.new_tool
                    << " gcode_bytes=" << raw_result[li][tc_idx].gcode.size());
            }
            ++m_num_toolchanges;

        } else {
            // T→T layer — structural fill. Register for get_finish_layer() lookup.
            // NEOTKO_NEOTOWER_TAG: include identity-sublayer events (is_sublayer=true,
            // old==new) in addition to non-sublayer structural events.  Identity-sublayer
            // gap layers are fixed by set_tool_override() above, producing a T0→T0
            // finish_layer TCR with the correct initial_tool.  Without registering them
            // here, get_finish_layer() would MISS at z=14.9166 → no structural fill →
            // tower gap at sublayer group boundaries.
            // Only the first T→T event at each z sets the index.
            const uint64_t z_key = static_cast<uint64_t>(
                std::llround(ev.z_actual * 1000.f));
            if (m_finish_layer_index.find(z_key) == m_finish_layer_index.end()) {
                m_finish_layer_index[z_key] = {static_cast<size_t>(li), 0u};
                NT_LOG("finish_layer index z=" << ev.z_actual
                    << " tool=" << ev.old_tool
                    << " is_sublayer=" << ev.is_sublayer
                    << " → [" << li << "][0]"
                    << " gcode_bytes=" << raw_result[li][0].gcode.size());
            }
            ++m_num_toolchanges;
        }
    }

    // NEOTKO_NEOTOWER_TAG: register bridge TCs in m_tcr_index.
    // Each bridge was fed to wt2 before its originating event, so its TCR is in
    // raw_result[br.wt2_li] with initial_tool==br.from_tool, new_tool==br.to_tool.
    //
    // ALSO register a merged key(z, from_tool, real.new_tool) → synthetic TCR
    // that fuses slot[bridge_si] + slot[bridge_si+1] into one.  This is needed
    // because GCode calls get_tcr(z, chain_tool, final_target) in a single shot —
    // it doesn't know the bridge exists.  The merged TCR executes bridge+real
    // GCode back-to-back and ends with final_target loaded.
    for (const BridgeTc& br : bridge_tcs) {
        const int li = br.wt2_li;
        if (li < 0 || li >= static_cast<int>(raw_result.size()) || raw_result[li].empty())
            continue;
        bool found = false;
        for (int si = 0; si < static_cast<int>(raw_result[li].size()); ++si) {
            if (static_cast<size_t>(raw_result[li][si].initial_tool) == static_cast<size_t>(br.from_tool) &&
                static_cast<size_t>(raw_result[li][si].new_tool)     == static_cast<size_t>(br.to_tool)) {
                // Register bridge key (z, from, to) → slot[si] in m_tcr_index.
                const uint64_t key = make_key(br.z,
                                              static_cast<size_t>(br.from_tool),
                                              static_cast<size_t>(br.to_tool));
                m_tcr_index[key] = {static_cast<size_t>(li), static_cast<size_t>(si)};
                NT_LOG("generate bridge-TCR registered z=" << br.z
                    << " old=" << br.from_tool << " new=" << br.to_tool
                    << " → [" << li << "][" << si << "]"
                    << (br.at_nominal_z ? " (at_nominal_z — separate layer)" : ""));

                // NEOTKO_NEOTOWER_TAG — bridge merged TCR:
                // GCode calls get_tcr(z, chain_tool, real_new_tool) in one shot.
                // Build a merged TCR = bridge[si] + real[si+1] so the call succeeds.
                //
                // SKIP when at_nominal_z: bridge and real event are in separate wt2
                // plan layers (different z values).  GCode dispatches them as separate
                // get_tcr() calls at their respective z values — no merging needed.
                // Merging would be wrong: raw_result[li][si+1] would be the NEXT
                // layer's first TCR, not the real event following this bridge.
                if (!br.at_nominal_z) {
                    const int next_si = si + 1;
                    if (next_si < static_cast<int>(raw_result[li].size())) {
                        const WipeTower::ToolChangeResult& bridge_tcr = raw_result[li][si];
                        const WipeTower::ToolChangeResult& real_tcr   = raw_result[li][next_si];
                        WipeTower::ToolChangeResult merged = bridge_tcr; // copy bridge as base
                        merged.new_tool      = real_tcr.new_tool;
                        merged.gcode        += real_tcr.gcode;
                        merged.end_pos       = real_tcr.end_pos;
                        merged.elapsed_time += real_tcr.elapsed_time;
                        merged.purge_volume += real_tcr.purge_volume;
                        merged.wipe_path     = real_tcr.wipe_path;
                        merged.extrusions.insert(merged.extrusions.end(),
                                                 real_tcr.extrusions.begin(),
                                                 real_tcr.extrusions.end());
                        const uint64_t merged_key = make_key(br.z,
                                                             static_cast<size_t>(br.from_tool),
                                                             static_cast<size_t>(real_tcr.new_tool));
                        m_merged_index[merged_key] = m_merged_tcrs.size();
                        m_merged_tcrs.push_back(std::move(merged));
                        NT_LOG("generate bridge-MERGED registered z=" << br.z
                            << " old=" << br.from_tool << " new=" << real_tcr.new_tool
                            << " [" << li << "][" << si << "+" << next_si << "]");
                    }
                }

                found = true;
                break;
            }
        }
        if (!found)
            NT_LOG("generate bridge-TCR WARN: no raw_result match z=" << br.z
                << " old=" << br.from_tool << " new=" << br.to_tool
                << " li=" << li);
    }

    result = m_result;

    NT_LOG("generate(): " << m_num_toolchanges
        << " TCRs across " << raw_result.size() << " wt2 plan layers"
        << " (from " << m_events.size() << " real_tc + "
        << m_growth_events.size() << " growth events).");

    // NEOTKO_MPSCHEDULER_TAG: re-validate after generate() so V9 can check z_redirect targets
    this->validate_plan();
}

// ---------------------------------------------------------------------------
// get_tcr()
// ---------------------------------------------------------------------------
std::optional<WipeTower::ToolChangeResult>
NeoTower::get_tcr(float z_actual, size_t old_tool, size_t new_tool) const
{
    uint64_t key = make_key(z_actual, old_tool, new_tool);
    auto it = m_tcr_index.find(key);
    // NEOTKO_MPSCHEDULER_TAG — C4: redirect fused sublayer events
    if (it == m_tcr_index.end()) {
        // NEOTKO_NEOTOWER_TAG — bridge merged TCR:
        // GCode calls get_tcr(z, chain_tool, final_target) in one shot.
        // When a bridge TC existed for this z, we synthesized a merged TCR
        // combining bridge+real GCode.  Check that map first.
        {
            auto mit = m_merged_index.find(key);
            if (mit != m_merged_index.end()) {
                NT_LOG("get_tcr MERGED z=" << z_actual << " old=" << old_tool
                    << " new=" << new_tool
                    << " → merged[" << mit->second << "]");
                return m_merged_tcrs[mit->second];
            }
        }
        auto redir = m_z_redirect.find(key);
        if (redir != m_z_redirect.end()) {
            it = m_tcr_index.find(redir->second);
            if (it != m_tcr_index.end()) {
                const auto [li, si] = it->second;
                NT_LOG("get_tcr REDIRECT z=" << z_actual << " old=" << old_tool
                    << " new=" << new_tool << " → fused [" << li << "][" << si << "]");
                return m_result[li][si];
            }
        }
        NT_LOG("get_tcr() MISS z=" << z_actual
            << " old=" << old_tool << " new=" << new_tool << " ← ERROR");
        return std::nullopt;
    }
    const auto [li, si] = it->second;
    NT_LOG("get_tcr HIT z=" << z_actual << " old=" << old_tool
        << " new=" << new_tool << " → [" << li << "][" << si << "]");
    return m_result[li][si];
}

// ---------------------------------------------------------------------------
// get_finish_layer()
// ---------------------------------------------------------------------------
std::optional<WipeTower::ToolChangeResult>
NeoTower::get_finish_layer(float z) const
{
    uint64_t key = static_cast<uint64_t>(std::llround(z * 1000.f));
    auto it = m_finish_layer_index.find(key);
    // NEOTKO_MPSCHEDULER_TAG — C4: redirect fused identity events
    if (it == m_finish_layer_index.end()) {
        auto redir = m_z_redirect_finish.find(key);
        if (redir != m_z_redirect_finish.end()) {
            it = m_finish_layer_index.find(redir->second);
            if (it != m_finish_layer_index.end()) {
                const auto [li, si] = it->second;
                NT_LOG("get_finish_layer REDIRECT z=" << z
                    << " → fused [" << li << "][" << si << "]");
                return m_result[li][si];
            }
        }
        NT_LOG("get_finish_layer() MISS z=" << z << " ← ERROR");
        return std::nullopt;
    }
    const auto [li, si] = it->second;
    NT_LOG("get_finish_layer HIT z=" << z
        << " → [" << li << "][" << si << "]");
    return m_result[li][si];
}

// NEOTKO_NEOTOWER_TAG_START — hardening P3 + P5
void NeoTower::validate_plan() const
{
    using NeoTowerZ::Z_EPS_GROUP;

    int warn_count = 0;
    auto WARN = [&warn_count](const std::string& msg) {
        ++warn_count;
        if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {
            NeoDebug::write(NeoDebug::WIPETOWER, "[VALIDATE] WARN: " + msg);
        }
    };

    // Merge both channels for cross-channel validations (V1, V2, V7)
    std::vector<const NeoTowerEvent*> all_ev;
    all_ev.reserve(m_events.size() + m_growth_events.size());
    for (const auto& ev : m_events)         all_ev.push_back(&ev);
    for (const auto& ev : m_growth_events)  all_ev.push_back(&ev);
    std::sort(all_ev.begin(), all_ev.end(),
        [](const NeoTowerEvent* a, const NeoTowerEvent* b) {
            if (std::abs(a->z_nominal - b->z_nominal) > 1e-5f)
                return a->z_nominal < b->z_nominal;
            return a->z_actual < b->z_actual;
        });

    // V1: all events sorted ascending by z_nominal
    for (size_t i = 1; i < all_ev.size(); ++i) {
        if (all_ev[i]->z_nominal + Z_EPS_GROUP < all_ev[i-1]->z_nominal) {
            std::ostringstream oss;
            oss << "V1: events not sorted at idx=" << i
                << " prev_z=" << all_ev[i-1]->z_nominal
                << " cur_z="  << all_ev[i]->z_nominal;
            WARN(oss.str());
        }
    }

    // V2: sublayer events precede real-layer events within same z_nominal
    for (size_t i = 1; i < all_ev.size(); ++i) {
        const auto* prev = all_ev[i-1];
        const auto* cur  = all_ev[i];
        if (std::abs(cur->z_nominal - prev->z_nominal) < Z_EPS_GROUP) {
            if (!prev->is_sublayer && cur->is_sublayer) {
                std::ostringstream oss;
                oss << "V2: real event before sublayer at z_nominal=" << cur->z_nominal
                    << " idx=" << i;
                WARN(oss.str());
            }
        }
    }

    // V3: real-TC chain consistency (m_events only — real TCs)
    size_t prev_real = SIZE_MAX;
    for (size_t i = 0; i < m_events.size(); ++i) {
        const auto& ev = m_events[i];
        if (ev.is_sublayer) continue;
        // m_events only has real TCs (old!=new) after P5, so no identity filter needed

        if (prev_real != SIZE_MAX) {
            if (m_events[prev_real].new_tool != ev.old_tool) {
                std::ostringstream oss;
                oss << "V3: chain gap real[" << prev_real << "].new=T"
                    << m_events[prev_real].new_tool
                    << " → real[" << i << "].old=T" << ev.old_tool
                    << " at z=" << ev.z_nominal;
                WARN(oss.str());
            }
        }
        prev_real = i;
    }

    // V4: non-identity events (m_events) have tcr index
    for (size_t i = 0; i < m_events.size(); ++i) {
        const auto& ev = m_events[i];
        const uint64_t key = make_key(ev.z_actual, ev.old_tool, ev.new_tool);
        if (m_tcr_index.find(key) == m_tcr_index.end()) {
            std::ostringstream oss;
            oss << "V4: missing m_tcr_index for event[" << i
                << "] z=" << ev.z_actual
                << " " << ev.old_tool << "→" << ev.new_tool;
            WARN(oss.str());
        }
    }

    // V5: identity events (m_growth_events) have finish_layer index
    // NEOTKO_NEOTOWER_TAG — s58 fix: align key with the actual runtime lookup.
    // m_finish_layer_index is keyed by z_um (see generate() line ~1778 and
    // get_finish_layer() line ~1924), not by make_key(z, old, new).  Using the
    // wrong key here produced spurious V5 warnings on every identity event.
    // Also consult m_z_redirect_finish when the z is a fused alias.
    for (size_t i = 0; i < m_growth_events.size(); ++i) {
        const auto& ev = m_growth_events[i];
        const uint64_t z_key = static_cast<uint64_t>(
            std::llround(ev.z_actual * 1000.f));
        bool found = m_finish_layer_index.find(z_key) != m_finish_layer_index.end();
        if (!found) {
            auto redir = m_z_redirect_finish.find(z_key);
            if (redir != m_z_redirect_finish.end())
                found = m_finish_layer_index.find(redir->second)
                        != m_finish_layer_index.end();
        }
        if (!found) {
            std::ostringstream oss;
            oss << "V5: missing m_finish_layer_index for identity event["
                << i << "] z=" << ev.z_actual << " T" << ev.old_tool << "→T"
                << ev.new_tool;
            WARN(oss.str());
        }
    }

    // V7: per-z-group sublayer validation (uses merged all_ev)
    // Post-scheduler: sublayer events within a z_nominal are fused groups, NOT a
    // linear chain. Validate that no two sublayer events at the same z_actual share
    // the same (old_tool, new_tool) pair (would collide in make_key).
    {
        size_t vi = 0;
        while (vi < all_ev.size()) {
            const float z_nom = all_ev[vi]->z_nominal;
            size_t vj = vi;
            while (vj < all_ev.size() &&
                   std::abs(all_ev[vj]->z_nominal - z_nom) < Z_EPS_GROUP)
                ++vj;
            // [vi, vj) is one z-group. Check for key collisions among sublayers.
            std::set<uint64_t> sub_keys;
            for (size_t k = vi; k < vj; ++k) {
                const auto* ev = all_ev[k];
                if (!ev->is_sublayer) continue;
                uint64_t sk = make_key(ev->z_actual, ev->old_tool, ev->new_tool);
                if (sub_keys.count(sk) > 0) {
                    std::ostringstream oss;
                    oss << "V7: sublayer key collision at z_nominal=" << z_nom
                        << " z_actual=" << ev->z_actual
                        << " " << ev->old_tool << "→" << ev->new_tool;
                    WARN(oss.str());
                }
                sub_keys.insert(sk);
            }
            vi = vj;
        }
    }

    // V8 (P5): channel integrity — m_events has only real TCs, m_growth_events only identities
    for (const auto& ev : m_events) {
        if (ev.old_tool == ev.new_tool) {
            std::ostringstream oss;
            oss << "V8: m_events contains identity event z=" << ev.z_actual
                << " T" << ev.old_tool;
            WARN(oss.str());
        }
    }
    for (const auto& ev : m_growth_events) {
        if (ev.old_tool != ev.new_tool) {
            std::ostringstream oss;
            oss << "V8: m_growth_events contains non-identity z=" << ev.z_actual
                << " " << ev.old_tool << "→" << ev.new_tool;
            WARN(oss.str());
        }
    }

    // V9 (Scheduler): every m_z_redirect target exists in m_tcr_index.
    // Only runs after generate() has populated m_tcr_index.
    if (!m_tcr_index.empty()) {
        for (const auto& [from_key, to_key] : m_z_redirect) {
            if (m_tcr_index.find(to_key) == m_tcr_index.end()) {
                std::ostringstream oss;
                oss << "V9: m_z_redirect target missing in m_tcr_index"
                    << " from_key=" << from_key << " to_key=" << to_key;
                WARN(oss.str());
            }
        }
        for (const auto& [from_zum, to_zum] : m_z_redirect_finish) {
            if (m_finish_layer_index.find(to_zum) == m_finish_layer_index.end()) {
                std::ostringstream oss;
                oss << "V9: m_z_redirect_finish target missing in m_finish_layer_index"
                    << " from_zum=" << from_zum << " to_zum=" << to_zum;
                WARN(oss.str());
            }
        }
    }

    // Summary
    if (warn_count == 0) {
        NT_LOG("[VALIDATE] plan OK ("
            << m_events.size() << " real_tc, "
            << m_growth_events.size() << " growth, "
            << m_tcr_index.size() << " tcr, "
            << m_finish_layer_index.size() << " finish_layer, "
            << m_z_redirect.size() << " z_redirect, "
            << m_z_redirect_finish.size() << " z_redirect_finish)");
    } else {
        NT_LOG("[VALIDATE] plan has " << warn_count
            << " warnings — check above lines for details");
    }
}
// NEOTKO_NEOTOWER_TAG_END

} // namespace Slic3r

// NEOTKO_NEOTOWER_TAG_END
