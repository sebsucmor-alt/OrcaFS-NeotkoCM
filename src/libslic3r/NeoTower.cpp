// NeoTower.cpp — Post-slice wipe tower for Neotko FullSpectrum 0.95
// NEOTKO_NEOTOWER_TAG_START

#include "NeoTower.hpp"

#include "libslic3r/Print.hpp"             // Print, PrintObject, MultiPassSubLayer
#include "libslic3r/PrintConfig.hpp"       // PrintConfig, PrintRegionConfig
#include "libslic3r/GCode/ToolOrdering.hpp" // LayerTools, ToolOrdering

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
    NT_LOG("collect_and_plan() DONE — " << m_events.size() << " events, "
           << m_plan.layers.size() << " planned layers");
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

    const ToolOrdering& tool_ordering = print.get_tool_ordering();
    const PrintConfig&  cfg           = *m_print_config;

    // ------------------------------------------------------------------
    // Collect MultiPass prime volume — max over all objects' regions.
    // Same logic as NEOTKO_MULTIPASS_PRIME_TAG block in Print.cpp.
    // ------------------------------------------------------------------
    float mp_prime_vol = 0.f;
    for (const PrintObject* obj : print.objects()) {
        if (obj->layers().empty()) continue;
        for (const LayerRegion* lr : obj->layers().front()->regions())
            mp_prime_vol = std::max(mp_prime_vol,
                static_cast<float>(lr->region().config().multipass_prime_volume.value));
    }

    // ------------------------------------------------------------------
    // 1a. Real-layer toolchanges from ToolOrdering.
    //
    // Mirrors the plan_toolchange loop in Print.cpp::_make_wipe_tower().
    // We walk consecutive extruder pairs per layer, exactly as WipeTower2
    // would see them.
    // ------------------------------------------------------------------
    {
        size_t current_tool = tool_ordering.first_extruder();
        if (current_tool == (size_t)-1)
            current_tool = m_initial_tool;


        for (const LayerTools& lt : tool_ordering) {
            if (!lt.has_wipe_tower || lt.is_mp_sublayer)
                continue;

            const float z_nom    = static_cast<float>(lt.print_z);
            
            // FIX A — NEOTKO_NEOTOWER_TAG: Use 50% of nozzle diameter as a safe floor
            float lh_raw = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                       ? lt.wipe_tower_layer_height
                                                       : lt.layer_height);
            const float lh = (lh_raw >= 0.01f) ? lh_raw : (m_nozzle_diameter * 0.5f);
            
            bool first_lt = (&lt == &tool_ordering.front());

            for (const unsigned int ext_id : lt.extruders) {
                bool is_first_on_first = first_lt &&
                    ext_id == tool_ordering.all_extruders().back();

                if (!is_first_on_first && ext_id == (unsigned int)current_tool)
                    continue; // no toolchange

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
                m_events.push_back(ev);
                NT_LOG("real-layer event z=" << ev.z_actual
                    << " old=" << ev.old_tool << " new=" << ev.new_tool
                    << " vol=" << ev.wipe_volume << " lh=" << ev.layer_height);

                current_tool = ext_id;
            }
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

    // First pass: build a map from sublayer_z_um → z_nominal of parent real layer.
    // Walk LayerTools in order; the parent real layer of a sublayer group is the
    // first non-sublayer LayerTools that follows all those sublayers in the sequence.
    // (Sublayers are inserted BEFORE their parent real layer in ToolOrdering.)
    auto z_um64 = [](float z) -> uint64_t {
        return static_cast<uint64_t>(std::llround(static_cast<double>(z) * 1000.0));
    };

    // Map sublayer print_z (µm) → parent real-layer print_z
    std::unordered_map<uint64_t, float> sublayer_to_nominal;
    {
        // Collect pending sublayer Zs, flush when we hit a real layer.
        std::vector<float> pending_sub_zs;
        for (const LayerTools& lt : tool_ordering) {
            if (lt.is_mp_sublayer) {
                pending_sub_zs.push_back(static_cast<float>(lt.print_z));
            } else {
                // This real layer is the parent of all pending sublayers.
                float z_nom = static_cast<float>(lt.print_z);
                for (float sz : pending_sub_zs)
                    sublayer_to_nominal[z_um64(sz)] = z_nom;
                pending_sub_zs.clear();
            }
        }
        // Any trailing sublayers with no following real layer: use their own Z as nominal.
        for (float sz : pending_sub_zs)
            sublayer_to_nominal[z_um64(sz)] = sz;
    }

    // Second pass: read sublayer tool data directly from multipass_sublayers().
    //
    // ROOT CAUSE of the previous 0-sublayer-events bug:
    //   ToolOrdering::collect_extruders() intentionally leaves lt.extruders EMPTY
    //   for is_mp_sublayer LayerTools (see ToolOrdering.cpp comment "extruders
    //   intentionally NOT populated — sublayer handler reads sub.tool_id directly").
    //   The old code iterated lt.extruders and found nothing → 0 events emitted.
    //
    // FIX: iterate multipass_sublayers() on each PrintObject directly, the same
    //   source collect_extruders() reads from, to get sub.tool_id and sub.height.
    //
    // De-duplication: multiple objects may share the same (Z, tool_id) — e.g. two
    //   identical MultiPass objects. Use std::map<key, SubInfo> which keeps the
    //   first-seen value and iterates in sorted (Z, tool_id) order.
    {
        struct SubInfo { float z_act; float lh; };
        // Key: {z_um, tool_id} — auto-sorted by Z then tool_id, auto-dedup.
        std::map<std::pair<uint64_t, int>, SubInfo> unique_subs;

        for (const PrintObject* obj : print.objects()) {
            const PrintObject& src = obj->get_shared_object()
                                     ? *obj->get_shared_object() : *obj;
            for (const auto& layer_subs : src.multipass_sublayers()) {
                for (const MultiPassSubLayer& sub : layer_subs) {
                    const float z_act = static_cast<float>(sub.print_z);
                    auto key = std::make_pair(z_um64(z_act), sub.tool_id);
                    // .emplace() is a no-op if key already present → dedup
                    unique_subs.emplace(key, SubInfo{z_act, sub.height});
                }
            }
        }

        // NEOTKO_NEOTOWER_TAG — Bug IX fix: pre-calculate real tool active just before
        // each sublayer z-group. The old single sub_cur init walked only to the FIRST
        // sublayer, so any real toolchanges between sublayer z-groups left sub_cur stale
        // → get_tcr() MISS for z-groups after the first one in multi-filament prints.
        std::map<uint64_t, size_t> sub_z_to_real_tool;
        {
            size_t running = tool_ordering.first_extruder();
            if (running == (size_t)-1) running = m_initial_tool;
            for (const LayerTools& lt : tool_ordering) {
                if (lt.is_mp_sublayer) {
                    const uint64_t z_um = z_um64(static_cast<float>(lt.print_z));
                    sub_z_to_real_tool.emplace(z_um, running); // emplace: keep first value per z
                } else {
                    if (!lt.extruders.empty())
                        running = (size_t)lt.extruders.back();
                }
            }
        }
        // NEOTKO_NEOTOWER_TAG

        if (mp_prime_vol > 0.f) {
            NT_LOG("sublayer pass: " << unique_subs.size() << " unique sub-Z");

            // Emit actual tool transitions old→new (not prime sentinels old==new).
            // unique_subs is sorted by {z_um, tool_id} so the iteration order
            // matches the order GCode calls get_tcr() for these primes.
            // sub_cur resets from sub_z_to_real_tool at NOMINAL group boundaries only,
            // then chains within the group (handles multiple sublayers per nominal Z).
            // BUG IX FIX: group boundary = z_nom change, NOT z_um change.
            // A single nominal layer (e.g. z_nom=10.05) has 3 sublayers with distinct
            // z_um values (9.9166, 9.9832, 10.0498). They must chain T1→T0→T1→T2.
            // Resetting sub_cur at every z_um breaks intra-group chaining.
            size_t sub_cur  = m_initial_tool;
            float  cur_znom = -1.f;
            for (const auto& [key, info] : unique_subs) {
                const size_t tool_id = (size_t)key.second;
                const float  z_act   = info.z_act;

                // FIX A — NEOTKO_NEOTOWER_TAG: Safe floor
                const float  lh      = (info.lh > 0.01f ? info.lh : (m_nozzle_diameter * 0.5f));

                float z_nom = z_act; // fallback: trailing sublayers use own Z
                {
                    auto it = sublayer_to_nominal.find(key.first);
                    if (it != sublayer_to_nominal.end())
                        z_nom = it->second;
                }

                // Reset sub_cur only when entering a new nominal layer group
                if (z_nom != cur_znom) {
                    auto it = sub_z_to_real_tool.find(key.first);
                    sub_cur  = (it != sub_z_to_real_tool.end()) ? it->second : m_initial_tool;
                    cur_znom = z_nom;
                }

                // Use wipe-volume table if available, else mp_prime_vol fallback.
                float wipe_vol = mp_prime_vol;
                if (sub_cur != tool_id
                    && sub_cur < m_wipe_volumes.size()
                    && tool_id  < m_wipe_volumes[sub_cur].size()
                    && m_wipe_volumes[sub_cur][tool_id] > 0.f)
                    wipe_vol = m_wipe_volumes[sub_cur][tool_id];

                NeoTowerEvent ev;
                ev.z_nominal    = z_nom;
                ev.z_actual     = z_act;
                ev.layer_height = lh;
                ev.old_tool     = sub_cur;  // actual current tool before this prime
                ev.new_tool     = tool_id;  // tool being primed
                ev.wipe_volume  = wipe_vol;
                ev.is_sublayer  = true;
                m_events.push_back(ev);
                NT_LOG("sublayer TRANS z=" << z_act
                    << " old=" << sub_cur << " new=" << tool_id
                    << " z_nom=" << z_nom
                    << " lh=" << lh
                    << " vol=" << wipe_vol);

                sub_cur = tool_id; // running tool after this prime
            }
        } else {
            NT_LOG("sublayer pass: mp_prime_vol=0, no prime events emitted"
                << " (" << unique_subs.size() << " unique sub-Z found)");
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
        // Only generate structural layers when there are sublayer events
        // (i.e., MultiPass/ColorMix is active in this print).
        bool has_sublayer_ev = std::any_of(m_events.begin(), m_events.end(),
            [](const NeoTowerEvent& e){ return e.is_sublayer; });

        if (has_sublayer_ev) {
            // Build set of z_um values that already have events from 1a+1b.
            std::set<uint64_t> event_zs;
            for (const NeoTowerEvent& ev : m_events)
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

            // NEOTKO_NEOTOWER_TAG — Sublayer z_um collision set.
            // Real layers whose print_z quantizes to the same µm integer as a
            // sublayer event's z_actual (e.g. z=10.05 and z=10.0498 both → 10050 µm)
            // must be skipped WITHOUT updating struct_tool.
            // If we updated struct_tool before the event_zs skip, the T_exit→T_next
            // transition belonging to the next real layer would be silently absorbed.
            std::set<uint64_t> sublayer_zums;
            for (const NeoTowerEvent& ev : m_events)
                if (ev.is_sublayer)
                    sublayer_zums.insert(z_um64(ev.z_actual));

            // Walk ToolOrdering sequentially, tracking current_tool.
            size_t struct_tool = tool_ordering.first_extruder();
            if (struct_tool == (size_t)-1)
                struct_tool = m_initial_tool;

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

                // Track tool through real layer extruders.
                for (unsigned ext_id : lt.extruders)
                    struct_tool = (size_t)ext_id;

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
                m_events.push_back(ev);
                NT_LOG("structural event z=" << z
                    << " tool=" << struct_tool << " vol=" << struct_vol
                    << " (has_wt=" << lt.has_wipe_tower << ")"
                    << (old_struct_tool != struct_tool ? " [post-sublayer TC]" : ""));
            }
        }
    }

    // This gives a stable ordering for plan() grouping.
    std::sort(m_events.begin(), m_events.end(),
        [](const NeoTowerEvent& a, const NeoTowerEvent& b) {
            if (a.z_actual != b.z_actual) return a.z_actual < b.z_actual;
            if (a.old_tool != b.old_tool) return a.old_tool < b.old_tool;
            return a.new_tool < b.new_tool;
        });

    // Deduplicate exact-key duplicates (same z_actual + old + new).
    // Accumulate wipe_volume instead of erasing, so both objects' purge needs
    // are served by a single tower slot.
    {
        std::vector<NeoTowerEvent> deduped;
        deduped.reserve(m_events.size());
        for (const NeoTowerEvent& ev : m_events) {
            if (!deduped.empty()) {
                NeoTowerEvent& last = deduped.back();
                bool same_key =
                    std::llround(last.z_actual * 1000) == std::llround(ev.z_actual * 1000)
                    && last.old_tool == ev.old_tool
                    && last.new_tool == ev.new_tool;
                if (same_key) {
                    last.wipe_volume += ev.wipe_volume;
                    continue;
                }
            }
            deduped.push_back(ev);
        }
        m_events = std::move(deduped);
    }

    NT_LOG("collect_all_events: " << m_events.size() << " events total ("
        << std::count_if(m_events.begin(), m_events.end(),
               [](const NeoTowerEvent& e){ return e.is_sublayer; })
        << " sublayer)");
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

    if (m_events.empty()) {
        NT_LOG("plan(): no events, tower is empty.");
        return;
    }

    // ------------------------------------------------------------------
    // Compute tower_width from max wipe volume across all events.
    // wipe_depth = max over all events of wipe_depth_for_volume(ev.wipe_volume, slot_h)
    // tower_width = wipe_depth + 2 * perimeter_width  (square → depth == width)
    // ------------------------------------------------------------------
    float max_depth = 0.f;

    // FIX B — NEOTKO_NEOTOWER_TAG:
    // For tower SIZING use at least a safe floor (50% nozzle).
    // Sublayer heights (0.04mm) and residual real-layer heights (0.0002mm)
    // both inflate depth by 5-1000x. Safe nominal height produces correct sizing.
    const float sizing_lh_floor = m_nozzle_diameter * 0.5f;

    for (size_t i = 0; i < m_events.size(); ++i) {
        const NeoTowerEvent& ev = m_events[i];
        
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

    // Final tower_width: max_depth + 2 perimeters, clamped to at least config width.
    float cfg_width = static_cast<float>(m_print_config->prime_tower_width);
    float computed  = max_depth + 2.f * m_perimeter_width;
    m_plan.tower_width = std::max(cfg_width, computed);
    m_plan.tower_depth = m_plan.tower_width; // square

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

    for (size_t ei = 0; ei < m_events.size(); ++ei) {
        const NeoTowerEvent& ev = m_events[ei];
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

    return std::max(2.f * m_perimeter_width, wipe_volume / vol_per_mm);
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
    m_num_toolchanges = 0;

    if (m_events.empty()) return;

    // NEOTKO_NEOTOWER_TAG_START: effective_initial_tool
    // WipeTower2 generates the first-layer brim in the FIRST plan_toolchange()
    // call.  The chain-ordering in Phase 1 starts from chain_tool and feeds that
    // TC first — so WipeTower2 puts the brim content in the TCR for that TC.
    //
    // m_initial_tool comes from ToolOrdering::first_extruder() which can differ
    // from the actual extruder GCode has active when it first calls the tower.
    // GCode's current_tool at layer 0 == old_tool of the first real-layer event.
    // When they differ the brim lands in a TCR that GCode never dispatches
    // (because it queries get_tcr(z, old=GCode_current, ...) → tc_idx that
    // points to the no-brim slot), leaving the first layer with only a thin
    // perimeter instead of the full brim base.
    //
    // Fix: derive effective_initial from the first real-layer TC event and use
    // it for both the WipeTower2 constructor and chain_tool initialisation.
    size_t effective_initial = m_initial_tool;
    for (const NeoTowerEvent& ev : m_events) {
        if (!ev.is_sublayer && ev.old_tool != ev.new_tool) {
            effective_initial = ev.old_tool;
            break;
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
    for (const NeoTowerEvent& ev : m_events) {
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
    constexpr float NT_WT_EPS = 1e-5f;

    std::vector<int> event_to_wt2_li(m_events.size(), -1);
    std::vector<int> event_to_wt2_tc_idx(m_events.size(), -1);

    int   wt2_li       = -1;
    int   chain_tool   = static_cast<int>(effective_initial); // NEOTKO_NEOTOWER_TAG (was m_initial_tool)
    // tc_count_per_wt2_li removed — tc_idx is now derived from raw_result after generate(),
    // not from the feed order.  See "Re-derive tc_idx" block below.

    // Helper: feed one event to wt2 in chain order, updating bookkeeping.
    auto feed_event = [&](size_t evi) {
        const NeoTowerEvent& ev = m_events[evi];
        event_to_wt2_li[evi] = wt2_li;
        wt2.plan_toolchange(
            ev.z_actual, ev.layer_height,
            static_cast<unsigned int>(ev.old_tool),
            static_cast<unsigned int>(ev.new_tool),
            ev.wipe_volume);
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
        // Phantom: old_tool == m_initial_tool in the first z-group.
        bool ph = m_events[abs_evi].old_tool == m_initial_tool;
        if (ph)
            NT_LOG("phantom-init skip z=" << m_events[abs_evi].z_actual
                << " old=" << m_events[abs_evi].old_tool
                << " new=" << m_events[abs_evi].new_tool
                << " (m_initial=" << m_initial_tool
                << " effective=" << effective_initial << ")");
        return ph;
    };
    // NEOTKO_NEOTOWER_TAG_END

    size_t ei = 0;
    while (ei < m_events.size()) {
        const float z_grp = m_events[ei].z_actual;

        // Locate end of this z-group.
        size_t ei_end = ei + 1;
        while (ei_end < m_events.size() &&
               std::abs(m_events[ei_end].z_actual - z_grp) <= NT_WT_EPS)
            ++ei_end;

        ++wt2_li;

        const size_t grp_size = ei_end - ei;
        if (grp_size == 1) {
            // Single event: nothing to chain.
            {
                const NeoTowerEvent& _gev = m_events[ei];
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

                if (_chain_gap) {
                    // NEOTKO_NEOTOWER_TAG: cross-group gap fix.
                    // WipeTower2 will enter plan layer wt2_li with m_current_tool=chain_tool
                    // (the tool left over from the previous z-group), but this event expects
                    // old_tool.  Override m_current_tool at this layer so generate() produces
                    // the correct TCR (T→T finish_layer with correct initial_tool, or T→T'
                    // real TC with correct old_tool → correct gcode and initial_tool in TCR).
                    wt2.set_tool_override(static_cast<size_t>(wt2_li),
                                          static_cast<unsigned int>(_gev.old_tool));
                    // For identity events, feed_event won't update chain_tool (old==new).
                    // Keep chain_tool in sync with the actual post-layer tool state.
                    chain_tool = static_cast<int>(_gev.new_tool);
                    NT_LOG("GROUP_SINGLE: set_tool_override wt2_li=" << wt2_li
                        << " override_tool=" << _gev.old_tool
                        << " chain_tool_updated=" << chain_tool);
                }
            }
            if (!is_phantom_init(ei))  // NEOTKO_NEOTOWER_TAG: skip phantom
                feed_event(ei);
        } else {
            // Multiple events at same z.  Build a valid TC chain starting from
            // chain_tool, then append any unchained (T→T) events at the end.
            NT_LOG("GROUP_MULTI: wt2_li=" << wt2_li
                << " z=" << m_events[ei].z_actual
                << " grp_size=" << grp_size
                << " chain_tool=" << chain_tool);
            std::vector<bool> used(grp_size, false);
            std::vector<size_t> ordered;
            ordered.reserve(grp_size);

            // NEOTKO_NEOTOWER_TAG: pre-mark phantom init events in first z-group.
            for (size_t k = 0; k < grp_size; ++k)
                if (is_phantom_init(ei + k)) used[k] = true;

            // Pass 1: greedy real-TC chain starting from chain_tool.
            int cur = chain_tool;
            bool found_any = true;
            while (found_any && ordered.size() < grp_size) {
                found_any = false;
                for (size_t k = 0; k < grp_size; ++k) {
                    if (used[k]) continue;
                    const NeoTowerEvent& ev = m_events[ei + k];
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

            for (size_t evi : ordered)
                feed_event(evi);
        }

        first_zgrp_done = true;  // NEOTKO_NEOTOWER_TAG: disable phantom guard after first group
        ei = ei_end;
    }

    // -----------------------------------------------------------------------
    // Phase 2: Generate proper rectangular-fill TCRs.
    //   T→T-only plan layers  → raw_result[li] = {finish_layer_tcr}  (1 element)
    //   Layers with N real TCs → raw_result[li] = N elements,
    //                            finish_layer merged into one of them
    // -----------------------------------------------------------------------
    std::vector<std::vector<WipeTower::ToolChangeResult>> raw_result;
    wt2.generate(raw_result);
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
    for (size_t ei = 0; ei < m_events.size(); ++ei) {
        const NeoTowerEvent& ev = m_events[ei];
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
    for (size_t ei = 0; ei < m_events.size(); ++ei) {
        const NeoTowerEvent& ev      = m_events[ei];
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

    result = m_result;

    NT_LOG("generate(): " << m_num_toolchanges
        << " TCRs across " << raw_result.size() << " wt2 plan layers"
        << " (from " << m_events.size() << " events).");
}

// ---------------------------------------------------------------------------
// get_tcr()
// ---------------------------------------------------------------------------
std::optional<WipeTower::ToolChangeResult>
NeoTower::get_tcr(float z_actual, size_t old_tool, size_t new_tool) const
{
    uint64_t key = make_key(z_actual, old_tool, new_tool);
    auto it = m_tcr_index.find(key);
    if (it == m_tcr_index.end()) {
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
    if (it == m_finish_layer_index.end()) {
        NT_LOG("get_finish_layer() MISS z=" << z << " ← ERROR");
        return std::nullopt;
    }
    const auto [li, si] = it->second;
    NT_LOG("get_finish_layer HIT z=" << z
        << " → [" << li << "][" << si << "]");
    return m_result[li][si];
}

} // namespace Slic3r

// NEOTKO_NEOTOWER_TAG_END
