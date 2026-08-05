// NeoTower.cpp — Post-slice wipe tower for Neotko FullSpectrum 0.95
// NEOTKO_NEOTOWER_TAG_START

#include "NeoTower.hpp"
#include "NeoTowerZ.hpp"  // NEOTKO_NEOTOWER_TAG — hardening P2
#include "NeoTowerPure.hpp"  // NEOTKO_NEOTOWER_TAG s205 (Fase 2) — pure helpers (testable scope)

#include "libslic3r/Print.hpp"             // Print, PrintObject, MultiPassSubLayer
#include "libslic3r/SurfacePassKind.hpp"   // NEOTKO_SANDWICH_TAG — SurfacePassKind enum (== PathBlend)
#include "libslic3r/PathBlendRuntime.hpp"  // NEOTKO_PATHBLEND_TAG — PathBlendSchedulerRuntime
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
#include <cstring>   // NEOTKO_NEOTOWER_TAG s238 — strstr en el parser de footprint de TCR
#include <sstream>   // NEOTKO_NEOTOWER_TAG s238 — istringstream en el parser de footprint
#include <fstream>   // NEOTKO_NEOTOWER_TAG s102-f — TCR gcode dump

// NEOTKO_NEOTOWER_TAG_START — NeoDebug routing
#include "NeoDebug.hpp"  // NEOTKO_DEBUG_TAG — NeoDebug channels (extracted from
                         // SurfaceColorMix during the Snapmaker 2.3.4 port).

#define NT_LOG(msg) do { if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {  \
    std::ostringstream _nt_oss; _nt_oss << "[NEOTOWER] " << msg;         \
    NeoDebug::write(NeoDebug::WIPETOWER, _nt_oss.str()); } } while(0)

// NEOTKO_NEOTOWER_TAG s204 (Fase 0) — invariant violations (V1-V17) are the DETECTOR of a
// new plan≡emission bug: they must announce themselves in the NORMAL log on EVERY slice, not
// only when ORCA_DEBUG_WIPETOWER is set (a silent detector is no detector). This macro is for
// invariant WARNs ONLY; verbose forensic traces keep using the gated NT_LOG. The channel line
// is preserved so existing log-scraping still sees "[VALIDATE] WARN: ..." when the channel is on.
#define NT_INVARIANT_WARN(msg) do {                                          \
    BOOST_LOG_TRIVIAL(warning) << "[NeoTower][VALIDATE] " << msg;            \
    if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {                            \
        std::ostringstream _nt_iw; _nt_iw << "[NEOTOWER] [VALIDATE] WARN: " << msg; \
        NeoDebug::write(NeoDebug::WIPETOWER, _nt_iw.str()); }                \
} while(0)
// NEOTKO_NEOTOWER_TAG_END

namespace Slic3r {

// ---------------------------------------------------------------------------
// NEOTKO_NEOTOWER_TAG_START — s238: forensics de OCUPACIÓN FÍSICA de la torre.
//
// Motivo: el usuario detectó EN EL GCODE, no en los logs, que dos visitas a la torre
// en el mismo z nominal (la lámina del pass pintado y la capa canónica) depositan cada
// una una capa ENTERA en la misma banda de Z y sobre la misma franja de Y. Los logs
// decían que todo estaba bien: V17 OK, DEPTH_ACCT 0% wasted, SHADOW 0 violations.
//
// Por qué no se veía: toda la instrumentación de la torre medía el PLAN (alturas por
// evento, slots por capa, profundidad en XY) y ninguna medía lo único que importa
// físicamente — cuánto material acaba dentro de un tramo de Z dado. Estos helpers leen
// el gcode YA EMITIDO, que es la única fuente que no puede mentir sobre eso.
struct NtTcrFootprint {
    float height = 0.f;   // ;HEIGHT: declarado = lo que la máquina va a aplastar
    float y_min  = 0.f;   // franja de profundidad REALMENTE extruida (no la reservada)
    float y_max  = 0.f;
    bool  has_h  = false;
    bool  has_y  = false;
};

static NtTcrFootprint nt_tcr_footprint(const std::string& gcode)
{
    NtTcrFootprint fp;
    float cur_y = 0.f;
    bool  cur_y_set = false;
    std::istringstream in(gcode);
    std::string line;
    while (std::getline(in, line)) {
        if (!fp.has_h && line.rfind(";HEIGHT:", 0) == 0) {
            fp.height = float(std::atof(line.c_str() + 8));
            fp.has_h  = true;
            continue;
        }
        if (line.rfind("G1", 0) != 0) continue;
        if (const char* py = std::strstr(line.c_str(), " Y")) {
            cur_y     = float(std::atof(py + 2));
            cur_y_set = true;
        }
        // Sólo cuentan los movimientos que EXTRUYEN: un viaje no ocupa sitio.
        if (!cur_y_set || std::strstr(line.c_str(), " E") == nullptr) continue;
        if (!fp.has_y) { fp.y_min = fp.y_max = cur_y; fp.has_y = true; }
        else           { fp.y_min = std::min(fp.y_min, cur_y);
                         fp.y_max = std::max(fp.y_max, cur_y); }
    }
    return fp;
}
// NEOTKO_NEOTOWER_TAG_END

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
    // NEOTKO_NEOTOWER_TAG s205 (Fase 2) — delegate to the pure helper (testable scope).
    return NeoTowerPure::make_key(z_actual, old_tool, new_tool);
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
    // NEOTKO_NEOTOWER_TAG s104 — the tower type enum is the source of truth
    // (decisión s103: selector estilo wall_type, default Classic). The legacy
    // checkbox key neotko_wipe_tower is kept as a fallback so profiles/3mf
    // saved before s104 do not silently lose NeoTower (the enum loads at its
    // Classic default there).
    // s104 follow-up: the enum is the ONLY truth. The legacy checkbox fallback
    // made "Classic" unselectable for any profile saved with the old checkbox
    // ON (invisible key always won) — observed by the user. Old profiles must
    // re-select Tower type = NeoTower once; the blocking validation in
    // Print::validate() catches sandwich scenes that forget.
    const auto* type_opt = config.option<ConfigOptionEnum<NeoTowerType>>("neotko_tower_type");
    if (!type_opt) {
        NT_LOG("is_enabled(): key 'neotko_tower_type' NOT FOUND in config");
        return false;
    }
    const bool en = (type_opt->value == nttNeoTower);
    NT_LOG("is_enabled() → " << (en ? "TRUE (tower type = NeoTower)" : "FALSE (tower type = Classic)"));
    return en;
}

// NEOTKO_NEOTOWER_TAG s103-bd — "box-in-drawer" extra width.
// The REAL tower box (drawer) grows by 2·perimeter_width so the canonical wall
// ring gets a reserved channel; sandwich/synthetic content keeps the original
// (config) box and ends up inset 1 bead per side. Single source of truth for
// every plate-footprint consumer (Print.cpp bbox/conflict/cone) — must match
// WipeTower2::neo_grow_box_drawer() (which uses m_perimeter_width =
// nozzle_diameter × Width_To_Nozzle_Ratio = 1.25, WipeTower.hpp:281).
// Revert: grep "s103-bd" and remove every tagged block.
float NeoTower::box_drawer_extra_width(const PrintConfig& config)
{
    if (!is_enabled(config))
        return 0.f;
    const float nozzle = config.nozzle_diameter.values.empty()
                             ? 0.4f
                             : static_cast<float>(config.nozzle_diameter.values.front());
    return 2.f * nozzle * 1.25f;
}

// ---------------------------------------------------------------------------
// Public entry point: collect_and_plan()
// ---------------------------------------------------------------------------
void NeoTower::collect_and_plan(const Print& print)
{
    // NEOTKO_DEBUG_TAG s79h — write a session banner into ALL active /tmp/neotko_*.log
    // files so successive slices in the same Orca process are visually separable.
    // Without this, tests run back-to-back concatenate without delimiter and
    // post-mortem becomes guesswork ("which slice produced this WARN?").
    NeoDebug::write_session_banner("collect_and_plan");

    NT_LOG("collect_and_plan() START");
    // NEOTKO_GCODE_REPROCESSOR: cache LibreMode here (Print& is available), consumed later by
    // generate() when it constructs NeoWipeTower — see m_neotko_libre_mode in NeoTower.hpp.
    m_neotko_libre_mode = !print.objects().empty() && print.objects().front()->config().neotko_libre_mode.value;
    collect_all_events(print);
    plan();
    NT_LOG("collect_and_plan() DONE — " << m_events.size() << " real_tc + "
           << m_growth_events.size() << " growth, "
           << m_plan.layers.size() << " planned layers");
    // NEOTKO_NEOTOWER_TAG — hardening P3: validate plan invariants
    this->validate_plan();
}

// ===========================================================================
// NEOTKO_NEOTOWER_TAG s158 — unified toolchange purge-volume resolver.
//
// Historically the wipe_volume was decided by WHERE the TC sat in the plan
// (real-layer → flush matrix; sandwich sublayer → multipass_prime_volume knob),
// not by WHAT the TC physically is. A real colour change inside a ColorStitch /
// MultiPass sublayer therefore reserved only the knob (~5-10 mm³) → toolchange_Wipe
// emitted a single starved line that never climbed the speed ramp
// (WipeTower2.cpp:2245) → hot-end choke on return to the object.
//
// One rule for every site:
//   body TC (sandwich_ctx=false)          → physical (matrix, scalar floor if OOB) — pre-s158 byte-identical
//   sandwich sublayer, old==new           → prime_floor (knob) — unchanged
//   sandwich sublayer, real colour change → max(prime_floor, physical) — unified
//
// prime_floor doubles as the OOB scalar fallback so each caller passes its own
// legacy fallback (body: prime_volume; sandwich: the knob) and reproduces its
// prior result exactly outside the fixed case.
//
// The s78 concern that made the knob authoritative (a full flush per thin
// sublayer overflowing the box + volumetric-flow spike) is now covered by the
// s136 full-volume box reserve and s103 purge compaction — but that interaction
// must be re-checked per-TCR before trusting it on thin sublayers.
// ===========================================================================
float NeoTower::resolve_wipe_volume(int old_tool, int new_tool,
                                    bool sandwich_ctx, float prime_floor) const
{
    // NEOTKO_NEOTOWER_TAG s205 (Fase 2) — delegate to the pure helper (testable scope).
    return NeoTowerPure::resolve_wipe_volume(m_wipe_volumes, old_tool, new_tool,
                                             sandwich_ctx, prime_floor);
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
    // NEOTKO_NEOTOWER_TAG s160c — stash the knob so generate()'s bridge TCs can feed it
    // to resolve_wipe_volume() as the floor (see m_mp_prime_vol decl).
    m_mp_prime_vol = mp_prime_vol;

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
                k.chain_key    = (uint64_t)(uintptr_t)obj * 1000003ull + (uint64_t)li;
                k.pass_idx     = sub.pass_idx;
                k.tool_id      = sub.tool_id;
                k.z_actual     = sub.print_z;
                // NEOTKO_PATHBLEND_TAG — s88. Mirror the GCode-side flag so the
                // wipe tower plan matches the dispatcher emission (otherwise TCR
                // misalignment as in Bug 04/07). PB chains drain atomically.
                // Gated on PathBlendSchedulerRuntime::chain_atomic — MUST stay
                // in sync with GCode.cpp:5290 read of the same toggle.
                k.atomic_chain = PathBlendSchedulerRuntime::get().chain_atomic
                                 && (sub.effect == SurfacePassKind::PathBlend);
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
    // s89 update: GCode.cpp:5277 reorders ALL items of one process_layer call
    // in a SINGLE order_sublayers_by_tool call (not per-plane). For per-scanline
    // PathBlend (s88) all 32 scanlines of one chain share a chain_key and the
    // atomic_chain flag, so a global call lets the drain visit them as one block
    // — which is exactly what the dispatcher does. The previous per-plane logic
    // here was correct for classic MP where each plane has multiple items per
    // chain, but it BROKE for atomic-chain PB by making atomic_chain a no-op
    // (every micro-plane had only 1 item per chain → no drain to chain).
    //
    // Result of the per-plane approach for test04: 1a predicted writer=T3 at
    // end of sublayers, but Fase B (canon, all-in-one) ended at T0 → cap event
    // planned T3→T0, dispatcher entered with T0, asked get_tcr(T0,T3) → MISS.
    //
    // All-in-one keeps the same threading semantics: items remain z_actual-
    // sortable within batches (order_sublayers_by_tool sorts batches by
    // z_actual, then chain_key, then original index), so z-monotonicity within
    // each tool run is preserved.
    auto mp_group_canon_order = [&](float z_nominal, int entering_tool)
        -> std::vector<MultiPassScheduler::SublayerKey> {
        auto it = mp_znom_keys.find(z_nominal);
        if (it == mp_znom_keys.end())
            return {};
        // s89: single global reorder so atomic_chain can drain across micro-
        // z-planes within one chain. Pre-s89 grouped by k.z_actual into a
        // std::map<double, …> first, but with 1e-7 inter-scanline spacing
        // every scanline became its own plane → atomic_chain inert.
        const std::vector<MultiPassScheduler::SublayerKey>& src = it->second;
        // NEOTKO_NEOTOWER_TAG s102 — per-window replay (Fase 7.2). The s89 global
        // call diverged from GCode for multi-plane classic-MP groups: GCode
        // reorders per process_layer call (one Z window of EPSILON=1e-4, see
        // collect_layers_to_print), with the writer tool chaining across calls.
        // PathBlend scanline chains (1e-7 spacing) still land in ONE window, so
        // the s88/s89 atomic-chain behaviour is unchanged for them.
        std::vector<size_t> ord =
            MultiPassScheduler::order_sublayers_by_tool_windowed(src, entering_tool, EPSILON);
        std::vector<MultiPassScheduler::SublayerKey> out;
        out.reserve(ord.size());
        for (size_t idx : ord)
            out.push_back(src[idx]);
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

        // NEOTKO_NEOTOWER_TAG s116-dbg INSTANCE-ID — log the address of the
        // ToolOrdering this loop reads, so 1a_READ rows can be matched against the
        // fill_wipe_tower_partitions() dump (which now logs this=<ptr>). If the
        // pointers differ, the 2.2 has_wt=0 we read here comes from a DIFFERENT
        // ToolOrdering instance than the dump that showed has_wt=1 — not a posterior
        // mutation of one instance.
        NT_LOG("1a_READ INSTANCE tool_ordering=" << static_cast<const void*>(&tool_ordering));

        bool first_real_layer_1a = true; // NEOTKO_NEOTOWER_TAG — first-layer rotation guard
        float last_znom_1a = -1.f;       // NEOTKO_MPSCHEDULER_TAG s79 — current sublayer group
        bool just_exited_sublayer_group = false; // NEOTKO_MPSCHEDULER_TAG s79

        for (const LayerTools& lt : tool_ordering) {
            // NEOTKO_NEOTOWER_TAG s115-dbg — 1a_READ: dump exactly what THIS loop
            // reads per LayerTools, so we can see why 2.2 is skipped while 2.52 is
            // built (both single-tool in l1) and how mp_perim_override_active /
            // wipe_tower_partitions / extruders line up at the band-nominal layers
            // (1.88 / 5.08). This is the read-site truth (vs the fill_wipe_tower_
            // partitions dump, which is a different point in the pipeline).
            {
                std::ostringstream _r;
                _r << "1a_READ z=" << lt.print_z
                   << " has_wt=" << lt.has_wipe_tower
                   << " is_mp_sub=" << lt.is_mp_sublayer
                   << " wipe_parts=" << lt.wipe_tower_partitions
                   << " perim_ov=" << lt.mp_perim_override_active
                   << " ext=" << lt.extruders.size() << "(";
                bool _f = true;
                for (unsigned int _e : lt.extruders) { if (!_f) _r << ","; _r << _e; _f = false; }
                _r << ")";
                NT_LOG(_r.str());
            }
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
                        // NEOTKO s136-dbg — full canonical order so we can confirm whether
                        // mp_group_final_tool's last tool (= 1a rotation entry tool) matches the
                        // real local-z sublayer emission exit (GCode rotates the real layer via
                        // nominal_layer_start_extruder = local_z_phase_b exit). Behavior-neutral.
                        if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {
                            auto _ord = mp_group_canon_order(znom, (int)prev);
                            std::ostringstream _co;
                            _co << "1a CANON_ORDER z_nom=" << znom << " enter=T" << prev << " order=[";
                            // NEOTKO s148-dbg — added pass_idx/chain_key so this can be
                            // cross-checked item-by-item against emission's MP_EMIT_ORDER
                            // (GCode.cpp:5286). If the tool order matches per plane but the
                            // windowed concatenation differs, the partition/entering-tool is
                            // the divergence; if a chain_key spans planes differently, that is.
                            for (const auto& _k : _ord)
                                _co << "T" << _k.tool_id << "/p" << _k.pass_idx
                                    << "/c" << _k.chain_key << "@z" << _k.z_actual << ",";
                            _co << "] final=T" << current_tool;
                            NeoDebug::write(NeoDebug::WIPETOWER, _co.str());
                        }
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
                // NEOTKO_DEBUG_TAG s85 — append_tcr layer-45 diagnosis: also mirror
                // to the WIPETOWER channel so user-supplied logs show the planning
                // (current_tool, rotated order) NeoTower used at this z without
                // requiring ORCA_DEBUG_NEOTOWER to be enabled. Cross-check vs
                // runtime NOMINAL_LAYER_START at the same z: if NeoTower planned
                // current_tool=Ta but runtime enters with writer=Tb, get_tcr(Tb,…)
                // MISSES because the registered event has old=Ta.
                if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {
                    std::ostringstream _ro2;
                    _ro2 << "NT_PLAN_ROTATE z=" << z_nom
                         << " current_tool=T" << current_tool
                         << " rotated=[";
                    for (unsigned int e : rotated_ext) _ro2 << "T" << e << " ";
                    _ro2 << "]";
                    NeoDebug::write(NeoDebug::WIPETOWER, _ro2.str());
                }
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
                // NEOTKO_NEOTOWER_TAG s158 — routed through the unified resolver
                // (sandwich_ctx=false → flush matrix / scalar fallback == the two lines
                // this replaces, byte-identical). Body TCs keep full flush behaviour.
                float wipe_vol = resolve_wipe_volume((int)current_tool, (int)ext_id,
                                                     /*sandwich_ctx=*/false,
                                                     static_cast<float>(cfg.prime_volume));

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
            // NEOTKO_PATHBLEND_TAG — s88. Mirrors SublayerKey::atomic_chain
            // so step5 cross-product can detect when a z is entirely populated
            // by atomic chains (PathBlend) and skip the reverse synthetic
            // (T_cap→T_ramp), which is the phantom that breaks TCR matching.
            bool               atomic    = false;
            int                effect    = 0;   // s115-dbg — (int)sub.effect (SurfacePassKind)
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
                        // NEOTKO_PATHBLEND_TAG — s88. PB sublayers are atomic
                        // chains. Mirror the same gate used in mp_znom_keys
                        // (NeoTower.cpp:243) for consistency.
                        se.atomic    = PathBlendSchedulerRuntime::get().chain_atomic
                                       && (sub.effect == SurfacePassKind::PathBlend);
                        se.effect    = (int)sub.effect;   // s115-dbg
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
                //
                // NEOTKO_NEOTOWER_TAG s158 — SUPERSEDES the knob-only rule above now that
                // old_tool is resolved: the knob becomes a FLOOR, not the sole value. A real
                // colour change in a sandwich sublayer takes max(knob, flush matrix) so it
                // purges — and climbs the wipe speed ramp — like a body change; same-tool
                // transitions still resolve to the knob (unchanged). Concern (b) above
                // (thin-sublayer overflow / flow spike) is now covered by the s136
                // full-volume box reserve + s103 compaction; verify per-TCR before trusting.
                se.wipe_vol = resolve_wipe_volume(se.old_tool, se.new_tool,
                                                  /*sandwich_ctx=*/true, mp_prime_vol);
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
                    // NEOTKO_PATHBLEND_TAG — s88. True iff EVERY real event at
                    // this z is part of a PB atomic chain. When true, step5's
                    // within-z cross-product is skipped: PB atomic chains are
                    // strictly ordered (entry→ramp_tool→…→cap_tool→exit), so
                    // the reverse pair (T_cap→T_ramp) is a phantom that breaks
                    // raw_result/plan slot matching in generate().
                    bool all_atomic = true;   // optimistic; flipped on first non-atomic event
                    int  n_events   = 0;
                    std::set<int> effects;    // s115-dbg — SurfacePassKind ints seen at this z
                };
                std::map<uint64_t, ZInfo> z_info;
                for (const SurfaceEvent& se : surf_events) {
                    const uint64_t zum = z_um64(se.z_actual);
                    auto& info   = z_info[zum];
                    info.z_float   = se.z_actual;
                    info.z_nominal = se.z_nominal;
                    info.h_max     = std::max(info.h_max, se.height);
                    info.tools.insert(se.new_tool);
                    if (!se.atomic) info.all_atomic = false;
                    info.effects.insert(se.effect);   // s115-dbg
                    ++info.n_events;
                }

                // 2. Track existing (z_um, old, new) pairs to avoid duplicates
                std::set<std::tuple<uint64_t, int, int>> existing;
                for (const SurfaceEvent& se : surf_events) {
                    existing.insert({z_um64(se.z_actual), se.old_tool, se.new_tool});
                }

                // 3. Helper: add synthetic event if pair doesn't exist yet.
                // NEOTKO_PATHBLEND_TAG — s88 debug: log the SOURCE of each
                // synthetic so we can identify which generation step (within-z
                // cross-product, entry-from-prev-z, or canon cross-z) injects
                // the phantom T_cap→T_ramp event that breaks the PB atomic
                // chain's TCR alignment. Filter with NT_SYNTH_GEN.
                const size_t base_count = surf_events.size();
                auto maybe_add_synthetic = [&](uint64_t zum, int old_t, int new_t,
                                                const char* src) {
                    if (old_t == new_t) {
                        NT_LOG("NT_SYNTH_GEN src=" << src
                            << " zum=" << zum << " " << old_t << "->" << new_t
                            << " SKIP_IDENTITY");
                        return;
                    }
                    if (existing.count({zum, old_t, new_t})) {
                        NT_LOG("NT_SYNTH_GEN src=" << src
                            << " zum=" << zum << " T" << old_t << "->T" << new_t
                            << " SKIP_EXISTS");
                        return;
                    }
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
                    // NEOTKO_NEOTOWER_TAG s158 — SUPERSEDED: unify via the resolver
                    // (sandwich_ctx=true → max(knob, flush matrix)). maybe_add_synthetic
                    // already returns early for old_t==new_t, so this is always a real
                    // colour transition and takes the flush matrix when it exceeds the knob.
                    se.wipe_vol  = resolve_wipe_volume(old_t, new_t,
                                                       /*sandwich_ctx=*/true, mp_prime_vol);
                    surf_events.push_back(se);
                    NT_LOG("NT_SYNTH_GEN src=" << src
                        << " zum=" << zum << " T" << old_t << "->T" << new_t
                        << " ADDED z=" << info.z_float << " znom=" << info.z_nominal
                        << " h=" << se.height << " vol=" << se.wipe_vol);
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
                        // NEOTKO_PATHBLEND_TAG — s88 debug: dump the tools set
                        // the cross-product will iterate over. If this set is
                        // {T_ramp, T_cap} for a PB atomic chain, the reverse
                        // synthetic (T_cap→T_ramp) is the phantom we want to
                        // eliminate.
                        {
                            std::ostringstream _ts;
                            _ts << "NT_SYNTH_ZINFO zum=" << zum
                                << " znom=" << zn
                                << " z=" << z_info[zum].z_float
                                << " tools={";
                            bool _first = true;
                            for (int t : tools) {
                                if (!_first) _ts << ",";
                                _ts << "T" << t;
                                _first = false;
                            }
                            _ts << "}";
                            // s115-dbg — effect/atomic provenance of this band z.
                            // SurfacePassKind: 0=None 1=Solid 2=ColorMix 3=PathBlend.
                            _ts << " all_atomic=" << z_info[zum].all_atomic
                                << " effects={";
                            bool _ef = true;
                            for (int e : z_info[zum].effects) {
                                if (!_ef) _ts << ",";
                                _ts << e; _ef = false;
                            }
                            _ts << "}";
                            NT_LOG(_ts.str());
                        }

                        // Cross-product within this z:
                        // If tools = {A, B}, generate A→B and B→A (whichever missing).
                        // NEOTKO_PATHBLEND_TAG — s88. SKIP when this z is fully
                        // populated by PB atomic chains. Atomic guarantees the
                        // dispatcher visits tools in a strict, scheduler-defined
                        // order; the reverse synthetic (T_cap→T_ramp) cannot
                        // happen at runtime and only creates a phantom TCR that
                        // breaks generate()'s plan-slot ↔ raw_result matching.
                        // The remaining synthetics (entry_from_prev_z and
                        // canon_cross_z below) still cover legitimate inter-z
                        // transitions.
                        if (z_info[zum].all_atomic) {
                            NT_LOG("NT_SYNTH_GEN src=step5_within_z_xprod zum=" << zum
                                << " SKIP_ALL_ATOMIC tools_count=" << tools.size()
                                << " events_at_z=" << z_info[zum].n_events);
                        } else {
                            for (int a : tools)
                                for (int b : tools)
                                    maybe_add_synthetic(zum, a, b, "step5_within_z_xprod");
                        }

                        // Entry from previous sublayer z within same z_nominal:
                        // GCode enters this z with current_tool from prev_z's tool set
                        if (prev_tools) {
                            for (int old_t : *prev_tools)
                                for (int new_t : tools)
                                    maybe_add_synthetic(zum, old_t, new_t,
                                                        "step5_entry_from_prev_z");
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
                            maybe_add_synthetic(cz, pt, ct, "step6_canon_cross_z");
                    }
                }

                const size_t n_synth = surf_events.size() - base_count;
                if (n_synth > 0)
                    NT_LOG("synthetic cross-product: " << n_synth << " events added");
            }

            // Sort by z_nominal, then z_actual, then pass_idx, then INSERTION ORDER.
            //
            // NEOTKO_NEOTOWER_TAG s237 — BUG A (SUB_PRIME_MISS en BIGTEST-ADAPTIVE).
            // El desempate final NO es cosmético: es la ÚNICA cosa que fija el orden
            // de herramientas del plan cuando un z_nominal tiene varios sublayers con
            // (z_nominal, z_actual, pass_idx, chain_key) IDÉNTICOS — el caso normal de
            // un objeto con 3 buckets ColorMix en el mismo plano (T2/p1, T1/p1, T0/p1
            // @z15.8498). Ahí order_sublayers_by_tool no tiene ninguna decisión que
            // tomar (una sola cadena, todo empatado) y devuelve el orden de ENTRADA
            // tal cual → barajar la entrada = barajar el plan.
            //
            // std::sort NO es estable: con el comparador anterior (empate total en los
            // tres campos) el introsort invertía esos tríos, y el plan salía
            // 3→0, 0→1, 1→2 mientras 1a/emisión hacían 3→2, 2→1, 1→0. Resultado: el par
            // real (3→2) no se sembraba en NINGÚN slot → get_tcr MISS → cambio de color
            // SIN visita a la torre (z=15.8498 · 17.0498 · 18.4498, confirmado en visor).
            // 14.4498 se salvaba sólo porque allí hay dos objetos (dos chain_key) y manda
            // la lógica de cadena en vez del desempate.
            //
            // `seq` = índice de inserción, que para los eventos REALES es exactamente el
            // orden de PrintObject::multipass_sublayers() — la MISMA fuente y el MISMO
            // orden que usa mp_znom_keys (:334) en 1a y que GCode.cpp usa al emitir.
            // Con él el comparador es un orden total y el espejo plan≡1a≡emisión deja de
            // depender del algoritmo de ordenación. Lección s236: misma pregunta → misma
            // constante y mismo ancla.
            {
                std::vector<size_t> seq(surf_events.size());
                for (size_t i = 0; i < seq.size(); ++i)
                    seq[i] = i;
                // Ordenamos los índices y reconstruimos, para poder usar `seq` como
                // último criterio sin tocar la struct SurfaceEvent.
                std::sort(seq.begin(), seq.end(),
                    [&surf_events](size_t ia, size_t ib) {
                        const SurfaceEvent& a = surf_events[ia];
                        const SurfaceEvent& b = surf_events[ib];
                        if (a.z_nominal != b.z_nominal) return a.z_nominal < b.z_nominal;
                        if (a.z_actual  != b.z_actual)  return a.z_actual  < b.z_actual;
                        if (a.pass_idx  != b.pass_idx)  return a.pass_idx  < b.pass_idx;
                        return ia < ib;   // orden canónico de multipass_sublayers()
                    });
                std::vector<SurfaceEvent> reordered;
                reordered.reserve(surf_events.size());
                for (size_t i : seq)
                    reordered.push_back(surf_events[i]);
                surf_events.swap(reordered);
            }

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

            // NEOTKO_NEOTOWER_TAG s204 (Fase 1) — the legacy FusedGroup chain-greedy scheduler
            // was REMOVED: it diverged from GCode's emission order (the plan≡emission bug it
            // caused on test04), and its `use_canon_scheduler` toggle — while emission (GCode.cpp)
            // ALWAYS used the canon order — made the OFF position a latent plan≠emission break.
            // The canon scheduler below is now the ONLY path. History: NEOTOWER.md §6 /
            // docs/FUTURE/NEOTOWER_REFACTOR_PLAN.md.

            size_t sei = 0;
            while (sei < surf_events.size()) {
                const float z_nom = surf_events[sei].z_nominal;
                size_t sej = sei;
                while (sej < surf_events.size() && surf_events[sej].z_nominal == z_nom)
                    ++sej;

                // chain_cur: real tool active antes del z_nominal group (Bug IX).
                // Avanza dentro del z_nominal según los grupos que se vayan emitiendo.
                size_t chain_cur = m_initial_tool;
                if (sei < sej) {
                    auto it = sub_z_to_real_tool.find(z_um64(surf_events[sei].z_actual));
                    if (it != sub_z_to_real_tool.end())
                        chain_cur = it->second;
                }

                // NEOTKO_PATHBLEND_TAG — s89 · s204. Canon-aligned scheduler (the ONLY path).
                // Uses the SAME algorithm GCode.cpp dispatches with
                // (MultiPassScheduler::order_sublayers_by_tool_windowed), so plan order ==
                // emission order by construction — this is what solved the test04 4-cube
                // atomic-chain crash. Fusion of consecutive same-tool siblings is preserved as a
                // post-pass batching so tower compaction across ColorMix-style multi-object
                // lámina is not lost. See NEOTOWER.md §6 and the s88 atomic-chain notes.
                {
                    // Build SublayerKey items from REAL events at this z_nominal.
                    // Synthetics (obj==nullptr) are emitted after as standalone
                    // slots (keyed-only, no chain — dispatcher looks them up via
                    // get_tcr but they don't shift the writer-state).
                    std::vector<MultiPassScheduler::SublayerKey> items;
                    std::vector<size_t>                          item_to_se;
                    for (size_t k = sei; k < sej; ++k) {
                        const SurfaceEvent& se = surf_events[k];
                        if (se.obj == nullptr) continue;
                        MultiPassScheduler::SublayerKey kk;
                        // chain_key IDENTICAL to GCode.cpp:5288 (single source of
                        // truth — diverging here would re-introduce the very bug
                        // this scheduler exists to fix).
                        kk.chain_key    = (uint64_t)(uintptr_t)se.obj * 1000003ull
                                          + (uint64_t)se.layer_idx;
                        kk.pass_idx     = se.pass_idx;
                        kk.tool_id      = se.new_tool;
                        kk.z_actual     = (double)se.z_actual;
                        kk.atomic_chain = se.atomic;
                        items.push_back(kk);
                        item_to_se.push_back(k);
                    }

                    // NEOTKO_NEOTOWER_TAG s102 — per-window replay (Fase 7.2), same
                    // rationale as mp_group_canon_order: must match GCode's
                    // per-process_layer-call reordering or the planned TC chain
                    // (and the synthetic coverage derived from it) diverges from
                    // emission → real-TC pair mismatch + get_tcr MISSes.
                    std::vector<size_t> order =
                        MultiPassScheduler::order_sublayers_by_tool_windowed(items, (int)chain_cur, EPSILON);

                    // NEOTKO_NEOTOWER_TAG s237 — V20: espejo 1a ≡ scheduler.
                    //
                    // 1a (mp_group_canon_order) y este scheduler responden a la MISMA
                    // pregunta —"¿en qué orden de herramientas se emite este z_nominal?"—
                    // con la misma función, pero alimentándola desde dos vectores
                    // distintos. Durante toda s236 discreparon en silencio (BUG A: el par
                    // real no se sembraba → cambio de color sin purga) porque NADIE
                    // comparaba las dos respuestas. Esto es esa comparación: gratis, y
                    // sale por `error` como V19 porque significa gcode sin purga.
                    // No corrige nada — el fix es el desempate por orden de inserción
                    // en el sort de surf_events. Si esto salta, el ancla se ha vuelto a
                    // desincronizar. Ver docs/FUTURE/NEOTOWER_S236_BUG_PLAN.md §1.
                    {
                        const std::vector<MultiPassScheduler::SublayerKey> ref =
                            mp_group_canon_order(z_nom, (int)chain_cur);
                        bool mismatch = (ref.size() != order.size());
                        for (size_t q = 0; !mismatch && q < order.size(); ++q)
                            if (ref[q].tool_id != items[order[q]].tool_id)
                                mismatch = true;
                        if (mismatch) {
                            std::ostringstream _v20;
                            _v20 << "[VALIDATE] V20: canon order 1a != scheduler at z_nom="
                                 << z_nom << " enter=T" << chain_cur << " 1a=[";
                            for (const auto& k : ref) _v20 << "T" << k.tool_id << ",";
                            _v20 << "] sched=[";
                            for (size_t oi : order) _v20 << "T" << items[oi].tool_id << ",";
                            _v20 << "] → un par real quedará sin slot (get_tcr MISS)";
                            BOOST_LOG_TRIVIAL(error) << _v20.str();
                            NT_LOG(_v20.str());
                        }
                    }

                    // Walk the canon order, emitting events with (old, new)
                    // chained from chain_cur. Consecutive items with the same
                    // tool_id form one batch: first item carries the real TC
                    // (vol_max), remaining items become identity members with
                    // z_redirect into the batch z_max. This is the same fusion
                    // semantics as the old FusedGroup multi-member slot, but
                    // applied ONLY to contiguous same-tool runs — non-adjacent
                    // duplicates of (old, new) are NOT fused, which is exactly
                    // what test04 needs (two distinct chains both producing
                    // T3→T1 are distinct toolchanges, must not collapse).
                    int    running = (int)chain_cur;
                    size_t i       = 0;
                    while (i < order.size()) {
                        const int tool = items[order[i]].tool_id;
                        size_t j = i;
                        while (j < order.size() && items[order[j]].tool_id == tool)
                            ++j;
                        // Batch is order[i..j-1], all tool=tool.
                        float z_max   = 0.f;
                        float z_min   = -1.f;   // s115 FALLO 1 — pass START z
                        float lh_max  = 0.f;
                        float vol_max = 0.f;
                        bool  batch_atomic = true;
                        for (size_t b = i; b < j; ++b) {
                            const SurfaceEvent& se = surf_events[item_to_se[order[b]]];
                            z_max   = std::max(z_max,   se.z_actual);
                            if (z_min < 0.f || se.z_actual < z_min) z_min = se.z_actual;
                            lh_max  = std::max(lh_max,  se.height);
                            vol_max = std::max(vol_max, se.wipe_vol);
                            batch_atomic = batch_atomic && items[order[b]].atomic_chain;
                        }
                        const float lh = (lh_max > 0.01f ? lh_max
                                                         : (m_nozzle_diameter * 0.5f));
                        const bool is_real_tc = (running != tool);

                        // NEOTKO_PATHBLEND_TAG s115 — FALLO 1: anchor the PathBlend
                        // tool-entry wipe at the pass START (z_min = first deposit of
                        // this tool in the band), NOT the band top (z_max). The PB ramp
                        // is OUR geometry with a known start/end; deriving the wipe z
                        // from z_max made the toolchange TCR float above the step where
                        // the tool first prints whenever no other tool happened to split
                        // the same-tool run (log1: T0→T3 emitted at 1.8798 while T3 first
                        // deposits at 1.816 → wipe floats ~0.06). Atomic (PB) batches
                        // ONLY — classic MP / sandwich keep z_max (print-verified). Wall
                        // span (start→end) is FALLO 2, handled next bottom-up step.
                        // Revert: grep "s115 FALLO 1".
                        const float z_tc_anchor =
                            (is_real_tc && batch_atomic) ? z_min : z_max;

                        NeoTowerEvent ev;
                        ev.z_nominal    = z_nom;
                        ev.z_actual     = z_tc_anchor;
                        ev.layer_height = lh;
                        ev.old_tool     = (size_t)running;
                        ev.new_tool     = (size_t)tool;
                        ev.wipe_volume  = vol_max;
                        ev.is_sublayer  = true;
                        ev.no_ramming   = true;
                        if (is_real_tc)
                            m_events.push_back(ev);
                        else
                            m_growth_events.push_back(ev);
                        NT_LOG("sublayer CANON_SCHED z_anchor=" << z_tc_anchor
                            << " z_min=" << z_min << " z_max=" << z_max
                            << " old=" << running << " new=" << tool
                            << " z_nom=" << z_nom << " vol=" << vol_max
                            << " batch=" << (j - i)
                            << " is_real=" << is_real_tc
                            << " atomic=" << batch_atomic
                            << (is_real_tc && batch_atomic && z_um64(z_min) != z_um64(z_max)
                                    ? " [s115-START-ANCHOR]" : ""));

                        // z_redirect aliases for batch members at z_actual != anchor.
                        // s115 FALLO 1: anchor is z_min for atomic real TCs (see above);
                        // members above the start redirect DOWN to it. Non-atomic /
                        // finish keep z_max anchor (z_tc_anchor == z_max there).
                        for (size_t b = i; b < j; ++b) {
                            const SurfaceEvent& se = surf_events[item_to_se[order[b]]];
                            if (z_um64(se.z_actual) != z_um64(z_tc_anchor)) {
                                if (is_real_tc) {
                                    uint64_t from_key = make_key(se.z_actual,
                                                                 (size_t)running,
                                                                 (size_t)tool);
                                    uint64_t to_key   = make_key(z_tc_anchor,
                                                                 (size_t)running,
                                                                 (size_t)tool);
                                    m_z_redirect[from_key] = to_key;
                                    NT_LOG("CANON z_redirect: z=" << se.z_actual
                                        << " " << running << "→" << tool
                                        << " → z_fused=" << z_tc_anchor);
                                } else {
                                    m_z_redirect_finish[z_um64(se.z_actual)]
                                        = z_um64(z_tc_anchor);
                                    NT_LOG("CANON z_redirect_finish: z="
                                        << se.z_actual
                                        << " T" << running << "→T" << tool
                                        << " → z_fused=" << z_tc_anchor);
                                }
                            }
                            if (se.obj != nullptr)
                                pass_completed[{se.obj, se.layer_idx, se.pass_idx}]
                                    = z_max;
                        }
                        running = tool;
                        i = j;
                    }

                    // Emit synthetics from this z_nominal block (obj==nullptr).
                    // They are standalone keyed slots — the dispatcher fetches
                    // them via get_tcr by (z, old, new) for cross-z entry
                    // transitions; they do NOT participate in chain threading.
                    for (size_t k = sei; k < sej; ++k) {
                        const SurfaceEvent& se = surf_events[k];
                        if (se.obj != nullptr) continue;
                        const float lh = (se.height > 0.01f ? se.height
                                                            : (m_nozzle_diameter * 0.5f));
                        NeoTowerEvent ev;
                        ev.z_nominal    = z_nom;
                        ev.z_actual     = se.z_actual;
                        ev.layer_height = lh;
                        ev.old_tool     = (size_t)se.old_tool;
                        ev.new_tool     = (size_t)se.new_tool;
                        ev.wipe_volume  = se.wipe_vol;
                        ev.is_sublayer  = true;
                        ev.no_ramming   = true;
                        // NEOTKO_NEOTOWER_TAG s205-5b.2c — obj==nullptr synthetic spare
                        // (cross-product coverage). May be consumed 0× → shadow census.
                        ev.speculative  = true;
                        if (ev.old_tool == ev.new_tool)
                            m_growth_events.push_back(ev);
                        else
                            m_events.push_back(ev);
                        NT_LOG("sublayer CANON_SYNTH z=" << se.z_actual
                            << " old=" << se.old_tool << " new=" << se.new_tool
                            << " (synthetic, no chain shift)");
                    }

                }

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

            // NEOTKO_NEOTOWER_TAG s79g — upper bound for structural emission.
            // Mirrors WipeTower2 tradicional behaviour: that engine relies on
            // wipe_tower_partitions which propagates downward only — layers ABOVE
            // the last toolchange end up with partitions=0 → has_wipe_tower=false
            // → no finish_layer there. NeoTower bypasses partitions and checks
            // lt.has_wipe_tower directly, which the NEOTKO_LIBRE_TAG OR-gate
            // (ToolOrdering.cpp:1542) sets to true on every layer where
            // extruders.size()>1. Result: a thin structural pass on every layer
            // above the last real toolchange — wasted material + tower instability
            // (user-reported: "wipe extra en cada capa tras la última con cambio").
            //
            // Cap by the highest z of any planned real or sublayer event. Above
            // that z nothing needs the tower, so don't emit structural events
            // there. Layers BETWEEN events still emit (the tower must remain
            // physically continuous to support sublayer primes that land on it).
            float last_event_z = 0.f;
            for (const NeoTowerEvent& ev : m_events)
                last_event_z = std::max(last_event_z, ev.z_actual);
            for (const NeoTowerEvent& ev : m_growth_events)
                last_event_z = std::max(last_event_z, ev.z_actual);

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
            // NEOTKO_NEOTOWER_TAG s103 — mirror GCode's initial_extruder_id rule
            // (same s66 fix 1a already has; this generator never got it).
            // first_extruder() said T1 while GCode (non-BBL + wipe tower +
            // !semm_priming) starts on all_extruders().back() = T0 → the first
            // structural event came out as T1→T0, which (a) GCode never
            // dispatches (it requests the identity T0→T0 → get_tcr MISS at
            // z=first_layer, served only by the stale-slot fallback) and
            // (b) poisoned effective_initial detection (the bogus 1→0 was the
            // only min-z event, so chain-start = T1). With the mirrored init
            // the first event is the identity T0→T0 growth event, served via
            // get_finish_layer like every other identity layer.
            size_t struct_tool;
            if (!print.is_BBL_printer() && tool_ordering.has_wipe_tower()
                && !cfg.single_extruder_multi_material_priming.value
                && !tool_ordering.all_extruders().empty()) {
                struct_tool = tool_ordering.all_extruders().back();
            } else {
                struct_tool = tool_ordering.first_extruder();
                if (struct_tool == (size_t)-1)
                    struct_tool = m_initial_tool;
            }
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
                if (!lt.has_wipe_tower)
                    continue;

                // NEOTKO_NEOTOWER_TAG s79g — upper-z cap (see last_event_z block above).
                // Skip real layers above the last planned event; the tower has no
                // reason to exist there and a thin growth pass per layer wastes
                // material and destabilises the freestanding tower top.
                if (z > last_event_z + NeoTowerZ::Z_EPS_GROUP) {
                    NT_LOG("structural SKIP z=" << z
                        << " > last_event_z=" << last_event_z
                        << " (no more events — match WipeTower2 tradicional)");
                    continue;
                }

                // Capture old tool BEFORE updating from this layer's extruders.
                // For the first real layer after a sublayer group, old_struct_tool
                // is the sublayer exit tool (e.g. T2) and struct_tool becomes
                // the object tool (e.g. T1) → generates a real T2→T1 transition.
                const size_t old_struct_tool = struct_tool;

                // NEOTKO_NEOTOWER_TAG s79i — Bug 04 (gap entre objetos apilados).
                // Capas con extruders=[] (p.ej. z=7.88 entre dos cubos con soporte)
                // tienen has_wipe_tower=true (vía partitions propagados) pero no hay
                // extruder al que rotar. Emite identity event (struct_tool sin
                // cambio) para mantener la torre físicamente continua. No tocar
                // first_real_layer_struct: una gap layer no es "primera capa real".
                const bool gap_layer = lt.extruders.empty();
                if (!gap_layer) {
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
                }

                // NEOTKO_NEOTOWER_TAG s86 — Bug WT-extrusion-3rd-layer FIX:
                // Slic3r/Orca ToolOrdering escribe `wipe_tower_layer_height` con la
                // altura acumulada de las capas que agruparía (ej. 0.28+0.20+0.20=0.68
                // en z=0.68 cuando la torre no tendría TC real en z=0.28/0.48).
                // Con NeoTower activo esa agrupación es incorrecta: ya planificamos un
                // structural finish_layer en CADA z real, así que el override acumulado
                // sobre-extruye ese slot 3.4× (vol pasa de 2.19 → 5.78 mm³ → la torre
                // crece como una capa de 0.68 mm y raspa cabezal).
                // Sublayer events (sandwich/CM/PB/MP) NO se ven afectados: usan otras
                // ramas (~líneas 379/540), y las capas con sublayer events son skipped
                // por `event_zs` justo debajo antes de llegar aquí.
                // FIX A — Safe floor (se conserva).
                float lh_raw = static_cast<float>(lt.layer_height);
                const float lh = (lh_raw >= 0.01f) ? lh_raw : (m_nozzle_diameter * 0.5f);
                // NEOTKO_DEBUG_TAG s86 — Bug WT-extrusion-3rd-layer: log LayerTools inputs
                // so we can prove that Slic3r grouped 3 thin layers into a single thick wipe
                // tower layer (lt.wipe_tower_layer_height = z, not 0.2). Expected: at the
                // first layer where the tower has real work (e.g. z=0.68), wipe_tower_layer_height
                // == z (accumulated), while lt.layer_height stays = nominal (0.2). Source-tag
                // = "s86/lh-trace". Retirar tras validar fix.
                NT_LOG("s86/lh-trace z=" << z
                    << " has_wt=" << lt.has_wipe_tower
                    << " wt_lh=" << lt.wipe_tower_layer_height
                    << " lt_lh=" << lt.layer_height
                    << " lh_raw=" << lh_raw
                    << " lh_final=" << lh
                    << " extruders_n=" << lt.extruders.size()
                    << " gap_layer=" << gap_layer
                    << " first_real=" << (int)(!first_real_layer_struct ? 0 : 1)
                    << " old_struct=" << old_struct_tool
                    << " new_struct=" << struct_tool);

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
                    << (old_struct_tool != struct_tool ? " [post-sublayer TC]" : "")
                    << (gap_layer ? " [gap-layer]" : ""));
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
    // NEOTKO_NEOTOWER_TAG s205 (Fase 2) — logic extracted verbatim to
    // NeoTowerPure::dedup_events (testable scope). Kept notes for the record:
    //
    // NEOTKO_NEOTOWER_TAG s79f — bug03 fix: real-vs-sublayer collision.
    // When a PathBlend/MultiPass last sublayer lands at z_actual very close to its
    // parent real-layer Z (e.g. sub z=0.9998, real z=1.0 — both quantize to
    // z_um=1000), the original dedup fused them, keeping the FIRST event (typically
    // the sublayer) and discarding the real. The real-layer TC was then absent from
    // m_tcr_index → emission found no slot → IS_EMPTY_WT suppress →
    // FINAL_LAYER_TC_FALLBACK bare set_extruder WITHOUT purge → contaminated
    // downstream perimeter (escena 03, log line 220: IS_EMPTY_WT ext=T0 need_tc=1).
    // Fix: when a real event collides with a sublayer at same quantized z, PREFER
    // the real (full layer_height + matrix flush volume); the sublayer's smaller
    // purge is absorbed by max(). When both are same kind, original max-volume
    // behaviour preserved.
    // max(), NOT += : two objects generating the same TC T0→T1 at z=0.65 only need
    // the tower to purge ONCE with the worst-case volume — max(vol_A, vol_B).
    // Summing would imply purging per object sequentially, which is not how the
    // wipe tower works. Affects ColorMix (dither) and MultiPass (identical passes).
    NeoTowerPure::dedup_events(m_events);
    NeoTowerPure::dedup_events(m_growth_events);  // NEOTKO_NEOTOWER_TAG — hardening P5

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
    // NEOTKO_NEOTOWER_TAG s205 (Fase 2) — delegate to the pure helper (testable scope).
    return NeoTowerPure::sublayer_slot_height(m_min_layer_height, nominal_layer_height);
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
    m_tcr_index_sub.clear(); // NEOTKO_NEOTOWER_TAG s102 — dual-channel index
    m_finish_layer_index.clear();
    m_merged_tcrs.clear();
    m_merged_index.clear();
    m_emission_order.clear(); // NEOTKO_NEOTOWER_TAG s205-5b.1 — canonical shadow list
    m_shadow_sequence.clear(); // NEOTKO_NEOTOWER_TAG s205-5b.2 — runtime emission census
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
    // NEOTKO_NEOTOWER_TAG s114 — standalone painted-layer detection (by composition).
    // A canonical layer whose z_nominal carries NO real (non-sublayer) event is
    // realised entirely by MultiPass sublayers → it IS the layer (PathBlend /
    // ColorMix / mixed / any gradient shape), not a decoration of a real one.
    // Mark its sublayer events so the s102-h lámina classification below treats
    // them as structural planes (keep wall+grid + advance the emitting-plane
    // tracker) instead of frame-free same-plane decorations. Without this, a run
    // of fully-painted layers left the tower no structural plane → frozen tracker
    // → multi-layer gap → box-in-drawer Fase C flow-boost wall (whiskers, s112-s113).
    // Mark ONLY the band-top sublayer (the single highest z_actual) of a
    // parent-less layer — that is the one that represents the real canonical
    // plane. The intermediate staircase sublayers (z_actual below the band-top)
    // MUST stay synthetic+inset (they are the box-in-drawer shells, §17.3 Fase
    // B); clearing synthetic on them would stack N full-drawer walls in one
    // layer interval ("demasiadas capas de golpe"). The band-top alone becomes
    // the canonical full-height wall.
    // NEOTKO_NEOTOWER_TAG s205 (Fase 2) / s236 — the pure decision lives in
    // NeoTowerPure::mark_standalone_planes (testable scope); see the s236 fix
    // note there for why "inside the 0.02 window" was not the same as "the
    // band-top" once MultiPass puts several sublayers 1 µm apart.
    {
        const size_t _marked = NeoTowerPure::mark_standalone_planes(all_events);
        NT_LOG("STANDALONE_PLANE: marked " << _marked << " band-top sublayer events on "
            << "fully-painted canonical layers (no real-layer parent)");
    }
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

    // Construct one NeoWipeTower instance for the whole print.
    // NEOTKO_NEOWIPETOWER_TAG s205-5a.1 — NeoWipeTower is our own byte-identical copy
    // of WipeTower2 (rename-only); NeoTower drives it exclusively so the classic
    // (non-NeoTower) path keeps using stock WipeTower2 untouched.
    // We use its plan_toolchange() + generate() path to get proper
    // rectangular fill TCRs (finish_layer pattern) — not local_z_tool_change().
    NeoWipeTower wt2(*m_print_config,
                   *m_region_config,
                   m_plate_idx,
                   m_plate_origin,
                   m_wipe_volumes,
                   effective_initial,
                   m_neotko_libre_mode);  // NEOTKO_NEOTOWER_TAG

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

    // NEOTKO_NEOTOWER_TAG s103-bd — grow the real box (drawer). After
    // set_extruder (m_perimeter_width final), before any plan_toolchange so
    // the real-entry wipe-length math sees the grown width. Synthetic entries
    // keep the original box via the s103-bd branches in WipeTower2.
    wt2.neo_grow_box_drawer();

    // NEOTKO_NEOTOWER_TAG s127 — NeoTower towers use the classic in-box wipe, NOT the
    // 2.3.4 gap_wall. gap_wall (default ON via wipe_tower_wall_gap) draws an outer wall
    // ring on every toolchange (margin 5·pw + brim, outside the reserved box) → the
    // "wall/brim/muro on return instead of a classic wipe" + extrusions outside the
    // NeoTower footprint. The fork (2.3.2) had no gap_wall. Gated here → only NeoTower's
    // internal wt2 is affected; stock/Classic 2.3.4 towers keep the config default.
    wt2.set_use_gap_wall(false);

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
    //
    // NEOTKO_NEOTOWER_TAG s236 — MUST be the epsilon plan_toolchange() itself uses
    // to decide "new layer or same layer" (WT_LAYER_Z_EPS = Z_EPS_PLAN), NOT the
    // finer Z_EPS_GROUP. A z-group here IS one wt2 plan layer: wt2_li is
    // incremented once per group and then used as an index into wt2's m_plan
    // (set_tool_override) and into raw_result (TCR lookup). With two different
    // epsilons the two layer counts silently diverge for any Δz in the window
    //     Z_EPS_GROUP (1e-5)  <  Δz  <  Z_EPS_PLAN (1e-4)
    // — NeoTower opens a new group, wt2 merges into the previous layer, and from
    // that point on every wt2_li is one too high. Observed with the PathBlend
    // per-scanline staircase (Δz = 3e-5 between the atomic chain's anchor and the
    // rest of the plane): the last 6 toolchanges mapped to a plan layer that does
    // not exist (tc_idx=-1), got no TCR, and GCode emitted the colour change with
    // NO tower visit at all. Repro pair: lancuak3-A34.3mf (fails, PathBlend angle
    // spreads Z) vs lancuak3-A40.3mf (passes, single Z). V19 below now asserts the
    // two counts agree, so any future drift is loud instead of silent.
    //
    // s49 is preserved: a sublayer sits SUBLAYER_GAP (2e-4) below its real layer,
    // and static_assert(SUBLAYER_GAP > Z_EPS_PLAN) guarantees 2e-4 > 1e-4, so the
    // sublayer/real pair still lands in two groups — in wt2 as well, which is the
    // point. Only sub-0.1 µm neighbours now merge, exactly as wt2 merges them.
    constexpr float NT_WT_EPS = NeoTowerZ::Z_EPS_PLAN;

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

    // NEOTKO_NEOTOWER_TAG s103 — delta-Z height normalization (frame + wipes).
    // Plan entries used to carry the NOMINAL layer height even when the plane
    // physically sits only ~0.1 above the previous emitted tower plane (a
    // staircase shell between two real layers, s102-h). set_layer() derives
    // m_extrusion_flow and the wipe lengths from entry.height, so the canonical
    // layer right after a staircase over-extruded its wall/grid/wipes 2× into a
    // half-size gap, and láminas over-extruded onto the staircase plane too.
    // Fix at the FEED (not as a post-pass over m_plan): plan_toolchange computes
    // each ToolChange's required_depth from layer_height_par at call time, so
    // passing the corrected height here keeps depth reservation == emission
    // (Plan == Emisión; a post-pass would resurrect the s78 overflow).
    // Rule: height = (z − z_prev_emitting_plane) ONLY when that delta is SMALLER
    // than the carried nominal (floored to NOMINAL_LH_MIN). It never grows the
    // height, so sparse-gap behaviour (delta > nominal) is unchanged.
    // Lámina entries (no plane of their own) never advance the tracker; their
    // wipes still get the delta height to their structural support plane.
    float last_emitting_plane_z = 0.f;
    // NEOTKO_NEOTOWER_TAG s205 (Fase 2) — the pure decision lives in
    // NeoTowerPure::eff_layer_height (testable scope); the lambda keeps the
    // reference capture of the emitting-plane tracker and the diagnostic NT_LOG.
    // The log branch is kept identical to pre-extraction (fires exactly when the
    // delta-Z rule applies), so behaviour and the trace are byte-for-byte the same.
    auto  eff_layer_height = [&](float z, float nominal_h) -> float {
        const float eff = NeoTowerPure::eff_layer_height(z, nominal_h, last_emitting_plane_z);
        const float delta = z - last_emitting_plane_z;
        if (delta > NeoTowerZ::Z_EPS_PLAN && delta < nominal_h - NeoTowerZ::Z_EPS_PLAN)
            NT_LOG("DELTA_H z=" << z << " nominal_h=" << nominal_h
                << " → eff_h=" << eff
                << " (support plane z=" << last_emitting_plane_z << ")");
        // NEOTKO_NEOTOWER_TAG s237 — BUG B. Traza propia para el suelo sobre el nominal
        // CRUDO: el caso de BUG B no pasaba por la rama delta, así que `DELTA_H` no se
        // logueaba y la altura envenenada viajaba invisible hasta el `;HEIGHT` del TCR.
        // Si esto aparece, alguien está alimentando alturas por debajo del mínimo físico
        // (p.ej. `lt.wipe_tower_layer_height`, que ya es un delta) — el suelo lo corrige,
        // pero el log dice dónde mirar.
        else if (nominal_h < NeoTowerZ::NOMINAL_LH_MIN - NeoTowerZ::Z_EPS_PLAN)
            NT_LOG("LH_FLOOR z=" << z << " nominal_h=" << nominal_h
                << " → eff_h=" << eff
                << " (por debajo de NOMINAL_LH_MIN; altura de entrada envenenada)");
        // NEOTKO_NEOTOWER_TAG s238 — Z_BUDGET: la decisión de altura, SIEMPRE.
        //
        // Las dos trazas de arriba sólo hablan cuando CORRIGEN algo. El caso que se nos
        // escapó es el contrario: el evento real que sigue a una lámina conserva su altura
        // nominal porque el tracker no avanzó, así que la rama delta no entra y no se
        // escribe una sola línea. Ese silencio se lee como "no hacía falta tocarlo" y es
        // exactamente el error de lectura que quemó s236 (ver lecciones: la ausencia de un
        // log no prueba nada). Un detector que sólo habla cuando actúa no sirve para
        // descartar: aquí se registra el plano de apoyo, el hueco real y qué regla ganó,
        // pase lo que pase. `rule=nominal-kept` con `delta` muy mayor que `eff_h` es la
        // firma del bug de solape.
        NT_LOG("Z_BUDGET z=" << z
            << " nominal_h=" << nominal_h
            << " eff_h=" << eff
            << " support_plane=" << last_emitting_plane_z
            << " delta=" << delta
            << " rule=" << ((delta > NeoTowerZ::Z_EPS_PLAN && delta < nominal_h - NeoTowerZ::Z_EPS_PLAN)
                                ? "delta"
                                : (nominal_h < NeoTowerZ::NOMINAL_LH_MIN - NeoTowerZ::Z_EPS_PLAN
                                       ? "floor" : "nominal-kept"))
            // NEOTKO_NEOTOWER_TAG s240 — firma de la familia B, ya masticada.
            //
            // Cuando el salto hasta el plano de apoyo es MAYOR que la altura que se va a
            // depositar, esta capa no llega abajo y deja aire: `delta - eff_h` milímetros.
            // La regla es correcta a propósito — nunca se agranda una altura (§28: el techo
            // es volumétrico, un hueco se tapa con más pasadas, jamás con más flujo) — pero
            // entonces alguien tiene que planificar esas pasadas, y hoy no las planifica
            // nadie. Medido en BIGTEST-ADAPTIVE: 4 casos con delta≈0.2557 y eff_h≈0.085,
            // que son 0.591 mm de los 0.677 que le faltan a esa torre.
            //
            // Se marca aquí, en el sitio donde se toma la decisión, para no tener que
            // deducirlo restando columnas a mano. V23 PLAN_GAP lo confirma por el otro lado.
            << ((delta > eff + NeoTowerZ::Z_EPS_PLAN)
                    ? ("  [NO_LLEGA_AL_APOYO: faltan " + std::to_string(delta - eff)
                       + " mm — hacen falta pasadas intermedias, NO más flujo (§28)]")
                    : std::string()));
        return eff;
    };
    bool grp_planned_any = false; // any plan_toolchange() issued for current z-group

    // Helper: feed one event to wt2 in chain order, updating bookkeeping.
    auto feed_event = [&](size_t evi) {
        const NeoTowerEvent& ev = all_events[evi];
        event_to_wt2_li[evi] = wt2_li;
        grp_planned_any = true; // NEOTKO_NEOTOWER_TAG s103
        wt2.plan_toolchange(
            ev.z_actual, eff_layer_height(ev.z_actual, ev.layer_height),
            static_cast<unsigned int>(ev.old_tool),
            static_cast<unsigned int>(ev.new_tool),
            ev.wipe_volume,
            ev.no_ramming,      // NEOTKO_MPSCHEDULER_TAG s79b — sandwich TCs skip ramming deposit
            ev.is_sublayer && !ev.standalone_plane,     // NEOTKO_NEOTOWER_TAG s102 — explicit synthetic-entry flag (s114: standalone band-top = canonical, not synthetic)
            // NEOTKO_NEOTOWER_TAG s102-h — lámina (same physical plane as the real
            // layer, frame fully skipped) vs staircase (distinct plane in the gap,
            // keeps wall+grid as the tower's structural shell).
            ev.is_sublayer && !ev.standalone_plane && (ev.z_nominal - ev.z_actual) < NeoTowerZ::SAME_PLANE_MAX_OFF); // s114 standalone
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

        grp_planned_any = false; // NEOTKO_NEOTOWER_TAG s103 — reset per z-group

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
                        // NEOTKO_NEOTOWER_TAG s130 — STARPAINT append_tcr layer-45 fix (A):
                        // mirror the identity branch — force wt2's initial_tool for this plan
                        // layer to ev.old_tool. Without it, wt2 keeps m_current_tool from before
                        // the sublayer excursion (T3 here, not the sublayer exit T1) and generates
                        // a T3→T3 identity entry, while NeoTower indexes the event as T1→T3 →
                        // m_result↔m_tcr_index mismatch → single-tool classifier suppresses the
                        // real TC → get_tcr MISS at the next layer. set_tool_override fixes the
                        // stored initial_tool without injecting a TC (GCode is already at ev.old).
                        wt2.set_tool_override(static_cast<size_t>(wt2_li),
                                              static_cast<unsigned int>(_gev.old_tool));
                        chain_tool = static_cast<int>(_gev.old_tool);
                        last_sublayer_z_nominal = -1.f;  // consumed
                    } else {
                        // Sublayer-to-sublayer (or non-post-sublayer) gap: inject the
                        // bridge at the event's actual z — original behaviour.
                        grp_planned_any = true; // NEOTKO_NEOTOWER_TAG s103
                        wt2.plan_toolchange(_gev.z_actual,
                                            eff_layer_height(_gev.z_actual, _gev.layer_height), // s103 delta-Z
                                            static_cast<unsigned int>(chain_tool),
                                            static_cast<unsigned int>(_gev.old_tool),
                                            // NEOTKO_NEOTOWER_TAG s160c — was 0.f. A bridge is a real
                                            // colour change (chain_tool→ev.old) that must purge, else the
                                            // first object extrusion of the new tool is contaminated
                                            // (ColorStitch bottom TCR[1][0] = single 0.46mm line). Unify
                                            // with the sublayer scheduler (s158): max(knob, matrix).
                                            resolve_wipe_volume(chain_tool, (int)_gev.old_tool,
                                                                /*sandwich_ctx=*/true, m_mp_prime_vol),
                                            /*skip_ramming=*/true,   // NEOTKO_MPSCHEDULER_TAG s79b — sandwich bridge
                                            /*synthetic=*/(_gev.is_sublayer && !_gev.standalone_plane), // NEOTKO_NEOTOWER_TAG s102 — bridge inherits the driving event's plane kind (s114 standalone)
                                            /*same_plane=*/_gev.is_sublayer && !_gev.standalone_plane && // s114 standalone
                                                (_gev.z_nominal - _gev.z_actual) < NeoTowerZ::SAME_PLANE_MAX_OFF); // s102-h
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
                    grp_planned_any = true; // NEOTKO_NEOTOWER_TAG s103
                    wt2.plan_toolchange(bridge_z,
                                        eff_layer_height(bridge_z, all_events[ei].layer_height), // s103 delta-Z
                                        static_cast<unsigned int>(chain_tool),
                                        static_cast<unsigned int>(section1a_initial),
                                        // NEOTKO_NEOTOWER_TAG s160c — was 0.f (bridge purge). Unify with
                                        // the sublayer scheduler (s158): max(knob, matrix). See the
                                        // GROUP_SINGLE bridge above.
                                        resolve_wipe_volume(chain_tool, section1a_initial,
                                                            /*sandwich_ctx=*/true, m_mp_prime_vol),
                                        /*skip_ramming=*/true,   // NEOTKO_MPSCHEDULER_TAG s79b — sandwich bridge
                                        // NEOTKO_NEOTOWER_TAG s102 — bridge-at-nominal lands on the
                                        // canonical (real) plane; bridge-at-actual inherits the
                                        // driving event's plane kind.
                                        /*synthetic=*/(bridge_follows_sublayer ? false : (all_events[ei].is_sublayer && !all_events[ei].standalone_plane)), // s114 standalone
                                        /*same_plane=*/(bridge_follows_sublayer ? false :
                                            (all_events[ei].is_sublayer && !all_events[ei].standalone_plane && // s114 standalone
                                             (all_events[ei].z_nominal - all_events[ei].z_actual) < NeoTowerZ::SAME_PLANE_MAX_OFF))); // s102-h
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

        // NEOTKO_NEOTOWER_TAG s103 — advance the emitting-plane tracker.
        // A z-group leaves a physical tower plane (shell) unless ALL of its
        // events are láminas (same plane as the canonical, frame fully skipped
        // by the s102-h guard — they deposit wipes but no structural plane of
        // their own). Mirrors plan_toolchange's entry-flag AND/demote semantics.
        // grp_planned_any guards the phantom-init edge case (group whose only
        // event was skipped → no wt2 entry exists → nothing was emitted).
        {
            bool grp_emits_plane = false;
            for (size_t k = ei; k < ei_end; ++k) {
                const NeoTowerEvent& ev = all_events[k];
                const bool is_lamina = ev.is_sublayer && !ev.standalone_plane && // s114 standalone
                    (ev.z_nominal - ev.z_actual) < NeoTowerZ::SAME_PLANE_MAX_OFF;
                // NEOTKO_NEOTOWER_TAG s238 — LAMINA: esta clasificación decide si la torre
                // sube de plano, y hasta ahora no dejaba rastro ninguno. Es el nudo del bug
                // de solape: una lámina "no tiene plano propio" a efectos del tracker, pero
                // su purga SÍ recibe altura completa hasta el plano estructural de abajo
                // (ver Z_BUDGET). Con un offset de 0.0002 mm contra un umbral de 0.02, el
                // pass del sandwich entra siempre por aquí como lámina y la capa canónica
                // vuelve a rellenar la misma banda. Loguear `off` junto al umbral deja ver
                // de un vistazo por cuánto se decide.
                NT_LOG("LAMINA z_grp=" << z_grp
                    << " ev_z_nom=" << ev.z_nominal << " ev_z_act=" << ev.z_actual
                    << " off=" << (ev.z_nominal - ev.z_actual)
                    << " thr=" << NeoTowerZ::SAME_PLANE_MAX_OFF
                    << " is_sub=" << (ev.is_sublayer ? 1 : 0)
                    << " standalone=" << (ev.standalone_plane ? 1 : 0)
                    << " → " << (is_lamina ? "LAMINA (sin plano propio)" : "PLANO"));
                if (!is_lamina) { grp_emits_plane = true; break; }
            }
            if (grp_planned_any && grp_emits_plane)
                last_emitting_plane_z = z_grp;
            NT_LOG("TRACKER z_grp=" << z_grp
                << " planned_any=" << (grp_planned_any ? 1 : 0)
                << " emits_plane=" << (grp_emits_plane ? 1 : 0)
                << " → last_emitting_plane_z=" << last_emitting_plane_z
                << (grp_planned_any && grp_emits_plane ? " (AVANZA)" : " (SE QUEDA)"));
        }

        ei = ei_end;
    }

    // -----------------------------------------------------------------------
    // NEOTKO_NEOTOWER_TAG s236 — V19: layer-count bijection (feed side).
    //
    // The whole wt2_li mechanism assumes "one z-group fed here == one plan layer
    // created there". That assumption was never checked, and when it broke the
    // only symptom was a colour change emitted with no tower visit — invisible in
    // the log, visible only in the print. Check it while both numbers are still in
    // scope, and announce unconditionally: a mismatch means TCRs are about to be
    // dropped, which is a correctness failure, not a cosmetic warning.
    // -----------------------------------------------------------------------
    {
        const size_t _fed_groups  = static_cast<size_t>(wt2_li + 1);
        const size_t _plan_layers = wt2.plan_layer_zs().size();
        if (_fed_groups != _plan_layers) {
            std::ostringstream _s;
            _s << "[NeoTower][VALIDATE] V19 LAYER COUNT DIVERGENCE: NeoTower fed "
               << _fed_groups << " z-groups but WipeTower2 built " << _plan_layers
               << " plan layers — every event past the divergence points at a"
               << " non-existent layer and WILL lose its TCR (silent missing purge)."
               << " Check NT_WT_EPS vs plan_toolchange's WT_LAYER_Z_EPS.";
            BOOST_LOG_TRIVIAL(error) << _s.str();
            NT_LOG(_s.str());
        } else {
            NT_LOG("[VALIDATE] V19 layer-count bijection OK (" << _fed_groups
                << " z-groups == " << _plan_layers << " wt2 plan layers)");
        }
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
    m_depth_by_li   = wt2.plan_layer_depths();       // s236 F1 — depth accounting

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

    // NEOTKO_NEOTOWER_TAG s102-f — full TCR gcode dump for frame forensics.
    // The wipetower log carries sizes and markers but not the moves themselves;
    // an unidentified structural contour (user preview: full-tower rectangle by
    // T0 at z≈0.878) cannot be attributed from markers alone. Dump every TCR's
    // gcode to a separate file so the emitting block can be identified offline.
    // Remove (or keep gated) once the frame work closes.
    if (NeoDebug::enabled(NeoDebug::WIPETOWER)) {
        std::ofstream _tcr_dump("/tmp/neotko_wipetower_tcrs.txt", std::ios::trunc);
        if (_tcr_dump.is_open()) {
            // NEOTKO_NEOTOWER_TAG s238 — cabecera con la OCUPACIÓN, no sólo la identidad.
            // El volcado traía el gcode pero ningún agregado, así que para ver que dos TCRs
            // se imprimen encima había que leerlos línea a línea (que es como lo encontró el
            // usuario). Ahora cada cabecera lleva la banda de Z que rellena [z-h, z] y la
            // franja de Y realmente extruida: dos cabeceras consecutivas con bandas Z que se
            // pisan y solape en Y = colisión, a simple vista.
            float _prev_top = -1e9f;
            for (size_t _li = 0; _li < raw_result.size(); ++_li)
                for (size_t _si = 0; _si < raw_result[_li].size(); ++_si) {
                    const auto& _t = raw_result[_li][_si];
                    const NtTcrFootprint _fp = nt_tcr_footprint(_t.gcode);
                    const float _bottom = float(_t.print_z) - _fp.height;
                    const bool  _clash  = _fp.has_h && _prev_top > -1e8f
                                          && _bottom < _prev_top - NeoTowerZ::Z_EPS_PLAN;
                    _tcr_dump << "===== TCR [" << _li << "][" << _si << "]"
                              << " print_z=" << _t.print_z
                              << " initial=T" << _t.initial_tool
                              << " new=T" << _t.new_tool
                              << " bytes=" << _t.gcode.size();
                    if (_fp.has_h)
                        _tcr_dump << " | h=" << _fp.height
                                  << " rellena_Z=[" << _bottom << "," << _t.print_z << "]";
                    if (_fp.has_y)
                        _tcr_dump << " Y=[" << _fp.y_min << "," << _fp.y_max << "]";
                    if (_clash)
                        _tcr_dump << "  <<< COLISION Z: la torre ya estaba llena hasta "
                                  << _prev_top << " (solape "
                                  << (_prev_top - _bottom) << " mm)";
                    _tcr_dump << " =====\n" << _t.gcode << "\n";
                    if (_fp.has_h) _prev_top = std::max(_prev_top, float(_t.print_z));
                }
            for (size_t _li = 0; _li < local_z_result.size(); ++_li)
                for (size_t _si = 0; _si < local_z_result[_li].size(); ++_si) {
                    const auto& _t = local_z_result[_li][_si];
                    _tcr_dump << "===== LOCAL_Z TCR [" << _li << "][" << _si << "]"
                              << " print_z=" << _t.print_z
                              << " initial=T" << _t.initial_tool
                              << " new=T" << _t.new_tool
                              << " bytes=" << _t.gcode.size()
                              << " =====\n" << _t.gcode << "\n";
                }
        }
    }

    // NEOTKO_NEOTOWER_TAG_START — s238: V21, invariante de OCUPACIÓN EN Z.
    //
    // El invariante que faltaba, en una frase: *la torre no puede depositar material dentro
    // de un tramo de Z que ya había rellenado*. V17 valida la altura de CADA evento por
    // separado y por eso dio OK mientras 0.1998 + 0.2 entraban en un hueco de 0.2: validar
    // elemento a elemento no valida el conjunto cuando varios comparten un recurso.
    //
    // Se mide sobre `raw_result`, es decir sobre el gcode YA EMITIDO (`;HEIGHT:` es
    // literalmente lo que la máquina va a aplastar), no sobre el plan. Si plan y emisión
    // divergen, esto ve la emisión — que es la que imprime.
    //
    // Detalle deliberado: el detalle por TCR va al canal (puede haber muchos) y el resumen
    // sale por BOOST_LOG_TRIVIAL(error), misma convención que V19/V20 — un detector mudo no
    // es detector. Con el bug de lámina vigente esto salta en TODO print con pass pintado;
    // es correcto que lo haga, y desaparecerá cuando la lámina comparta presupuesto con su
    // capa canónica en vez de recibir capa entera propia.
    {
        struct _ZBand { float z, h; size_t li, si; };
        std::vector<_ZBand> _bands;
        for (size_t _li = 0; _li < raw_result.size(); ++_li)
            for (size_t _si = 0; _si < raw_result[_li].size(); ++_si) {
                const NtTcrFootprint _fp = nt_tcr_footprint(raw_result[_li][_si].gcode);
                if (_fp.has_h && _fp.height > 0.f)
                    _bands.push_back({ float(raw_result[_li][_si].print_z), _fp.height, _li, _si });
            }
        std::stable_sort(_bands.begin(), _bands.end(),
                         [](const _ZBand& a, const _ZBand& b) { return a.z < b.z; });

        int   _v21_hits  = 0;
        int   _v21_sib   = 0;   // hermanas si>0 — comparten banda de Z POR DISEÑO
        float _v21_worst = 0.f;
        float _filled_to = -1e9f;
        for (const _ZBand& b : _bands) {
            const float _bottom = b.z - b.h;
            // NEOTKO_NEOTOWER_TAG s239b — FALSO POSITIVO estructural de V21, medido en BT/BT-A:
            // 340 de 899 disparos eran `si>0`, es decir la 2ª/3ª purga de la MISMA capa de
            // torre. Todas las TCR de una capa comparten print_z y height, así que rellenan la
            // misma banda de Z por construcción — y se reparten en Y, que es justo el mecanismo
            // correcto (verificado en el dump: z=0.25 → si=1 Y=[86.7,89.95], si=2 Y=[80.1,83.35],
            // disjuntas). V21 sólo mira Z, así que no puede distinguirlas de un solape real y
            // ahogaba la señal. Se cuentan aparte en vez de callarlas: un detector que descarta
            // en silencio es tan malo como uno que no habla.
            if (b.si > 0) { ++_v21_sib; _filled_to = std::max(_filled_to, b.z); continue; }
            if (_filled_to > -1e8f && _bottom < _filled_to - NeoTowerZ::Z_EPS_PLAN) {
                const float _ov = _filled_to - _bottom;
                ++_v21_hits;
                _v21_worst = std::max(_v21_worst, _ov);
                // NEOTKO_NEOTOWER_TAG s239b — clasificar el disparo, que hay tres formas y sólo
                // una es un fallo. `MISMO-PLANO`: lo que ya estaba relleno acaba a menos de
                // SAME_PLANE_MAX_OFF de esta z ⇒ es una lámina de este mismo plano físico. Tras
                // el fix de s239 eso es lo ESPERADO (comparten banda de Z y se reparten en Y);
                // quien juzga ese caso es V22, no V21. `SUELO-LH`: el plano de debajo es otro de
                // verdad y el solape es la tasa de NOMINAL_LH_MIN sobre una microcapa adaptive,
                // legal por la cota de V17.
                const bool _same_plane = (b.z - _filled_to) < NeoTowerZ::SAME_PLANE_MAX_OFF;
                NT_LOG("[VALIDATE] V21 Z-OVERFILL TCR[" << b.li << "][" << b.si << "]"
                    << " z=" << b.z << " h=" << b.h
                    << " rellena=[" << _bottom << "," << b.z << "]"
                    << " pero la torre ya estaba llena hasta " << _filled_to
                    << " → solape=" << _ov << " mm"
                    << (_same_plane ? " [MISMO-PLANO: lamina abajo, lo juzga V22]"
                                    : " [SUELO-LH: plano inferior real, tasa NOMINAL_LH_MIN]"));
            }
            _filled_to = std::max(_filled_to, b.z);
        }
        // NEOTKO_NEOTOWER_TAG s239b — el censo va SIEMPRE al canal, dispare o no, y con las
        // hermanas descontadas explícitamente. Antes sólo hablaba al fallar: eso obliga a leer
        // el silencio, que es justo lo que ha fallado en s237, s238 y otra vez aquí.
        NT_LOG("[VALIDATE] V21 censo: " << _bands.size() << " TCRs | hermanas si>0 omitidas="
            << _v21_sib << " | disparos=" << _v21_hits << " | solape peor=" << _v21_worst
            << " mm (clasificados MISMO-PLANO vs SUELO-LH arriba)");
        if (_v21_hits > 0)
            BOOST_LOG_TRIVIAL(error)
                << "[NeoTower][VALIDATE] V21 Z-OVERFILL: " << _v21_hits
                << " TCR(s) depositan material donde la torre ya estaba llena (solape peor="
                << _v21_worst << " mm). Sobreextrusion en la torre. "
                << "Detalle por TCR en el canal WIPETOWER (ORCA_DEBUG_WIPETOWER=1).";
        else
            NT_LOG("[VALIDATE] V21 Z-occupancy OK (" << _bands.size()
                << " TCRs, ninguna banda de Z se pisa)");
    }
    // NEOTKO_NEOTOWER_TAG_END

    // NEOTKO_NEOTOWER_TAG s205 (Fase 3) — V14 (frame) + V17 (delta-Z height) slice-time
    // invariants, extracted verbatim into validate_result_frame_and_height() for a single
    // named call site. MUST stay here (post-generate): it reads all_events with the
    // generate()-time standalone_plane marking + the freshly built raw_result, neither of
    // which is visible to the const validate_plan(). Pure validation (log-only) — gcode
    // and behaviour unchanged.
    validate_result_frame_and_height(raw_result, all_events);

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
        // NEOTKO_NEOTOWER_TAG s236 — V19 (emission side). Falling through here
        // silently is how the s236 missing-purge bug stayed invisible: the event
        // keeps tc_idx=-1, never registers a TCR, and at emission get_tcr() misses
        // and GCode changes colour without visiting the tower. A real TC pointing
        // outside raw_result is a correctness failure — say so out loud.
        if (li >= static_cast<int>(raw_result.size())) {
            std::ostringstream _s;
            _s << "[NeoTower][VALIDATE] V19 ORPHAN EVENT: z=" << ev.z_actual
               << " " << ev.old_tool << "→" << ev.new_tool
               << " maps to plan layer li=" << li << " but only "
               << raw_result.size() << " layers exist — this toolchange will get"
               << " NO wipe tower visit.";
            BOOST_LOG_TRIVIAL(error) << _s.str();
            NT_LOG(_s.str());
            continue;
        }
        if (li < 0 || raw_result[li].empty())
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

        // NEOTKO_NEOTOWER_TAG s130-dbg — probe: dump the event→wt2 mapping for real TCs to
        // find why some events miss m_tcr_index despite their TCR being generated (STARPAINT
        // append_tcr layer-45). Behavior-neutral logging; gated on the debug channel.
        if (ev.old_tool != ev.new_tool)
            NT_LOG("PHASE3_MAP ei=" << ei << " z=" << ev.z_actual
                << " old=" << ev.old_tool << " new=" << ev.new_tool
                << " is_sub=" << ev.is_sublayer
                << " li=" << li << " tc_idx=" << tc_idx
                << " raw_li_size=" << ((li >= 0 && li < static_cast<int>(raw_result.size())) ? static_cast<int>(raw_result[li].size()) : -1));

        if (li < 0 || li >= static_cast<int>(raw_result.size()) || raw_result[li].empty())
            continue;

        if (ev.old_tool != ev.new_tool) {
            // Real toolchange — map (z, old, new) → raw_result[li][tc_idx].
            if (tc_idx >= 0 && tc_idx < static_cast<int>(raw_result[li].size())) {
                const uint64_t key = make_key(ev.z_actual, ev.old_tool, ev.new_tool);
                // NEOTKO_NEOTOWER_TAG s102 — dual-channel index: sublayer TCs go to
                // m_tcr_index_sub, real-layer TCs to m_tcr_index. The last sub plane
                // and the real plane quantize to the same µm, so the same key may
                // exist in BOTH channels (Hallazgo VII collision) — that is now legal
                // ACROSS channels; get_tcr() disambiguates via sublayer_ctx.
                auto& _chan = ev.is_sublayer ? m_tcr_index_sub : m_tcr_index;
                // V16: a collision WITHIN one channel is still a real bug (two
                // same-channel events sharing (z_um, old, new) → one TCR shadowed).
                {
                    auto _prev = _chan.find(key);
                    if (_prev != _chan.end() &&
                        (_prev->second.first != static_cast<size_t>(li) ||
                         _prev->second.second != static_cast<size_t>(tc_idx))) {
                        NT_INVARIANT_WARN("V16 KEY COLLISION (channel="
                            << (ev.is_sublayer ? "sub" : "real") << ") key=" << key
                            << " z=" << ev.z_actual
                            << " old=" << ev.old_tool << " new=" << ev.new_tool
                            << " overwrites [" << _prev->second.first << "][" << _prev->second.second
                            << "] with [" << li << "][" << tc_idx << "]"
                            << " — emission will cross-wire");
                    }
                }
                _chan[key] = {static_cast<size_t>(li),
                              static_cast<size_t>(tc_idx)};
                // NEOTKO_NEOTOWER_TAG s205-5b.1 — shadow the map into the canonical
                // emission-ordered list (data-only; V18 checks the bijection).
                m_emission_order.push_back(TowerEvent{
                    ev.z_actual, ev.old_tool, ev.new_tool,
                    ev.is_sublayer ? LayerKind::Sublayer : LayerKind::Real,
                    key, static_cast<size_t>(li), static_cast<size_t>(tc_idx),
                    // NEOTKO s205-5b.2c — synthetic sublayer spare (0×→census).
                    ev.speculative});
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
                // NEOTKO_NEOTOWER_TAG s205-5b.1 — shadow structural TCR (T→T).
                m_emission_order.push_back(TowerEvent{
                    ev.z_actual, ev.old_tool, ev.new_tool, LayerKind::Structural,
                    z_key, static_cast<size_t>(li), 0u,
                    // NEOTKO s205-5b.2c — a sublayer-plane structural finish is a spare
                    // (the plane may be covered by prime/TCR at runtime → 0× = census);
                    // a real-layer frame-growth finish (is_sublayer=false) MUST emit.
                    ev.is_sublayer});
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
                // NEOTKO_NEOTOWER_TAG s102 — V16 (bridge variant), see Phase 3 block.
                {
                    auto _prev = m_tcr_index.find(key);
                    if (_prev != m_tcr_index.end() &&
                        (_prev->second.first != static_cast<size_t>(li) ||
                         _prev->second.second != static_cast<size_t>(si))) {
                        NT_INVARIANT_WARN("V16 m_tcr_index KEY COLLISION (bridge) key=" << key
                            << " z=" << br.z << " old=" << br.from_tool << " new=" << br.to_tool
                            << " overwrites [" << _prev->second.first << "][" << _prev->second.second
                            << "] with [" << li << "][" << si << "]");
                    }
                }
                m_tcr_index[key] = {static_cast<size_t>(li), static_cast<size_t>(si)};
                // NEOTKO_NEOTOWER_TAG s205-5b.1 — shadow bridge TCR.
                m_emission_order.push_back(TowerEvent{
                    br.z, static_cast<size_t>(br.from_tool), static_cast<size_t>(br.to_tool),
                    LayerKind::Bridge, key, static_cast<size_t>(li), static_cast<size_t>(si)});
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
                        // NEOTKO_NEOTOWER_TAG s205-5b.1 — shadow merged bridge+real TCR
                        // (li = index into m_merged_tcrs, captured before the push_back).
                        // merged (the copy) is what gets moved; real_tcr stays valid.
                        const size_t merged_li = m_merged_tcrs.size();
                        m_merged_index[merged_key] = merged_li;
                        m_emission_order.push_back(TowerEvent{
                            br.z, static_cast<size_t>(br.from_tool),
                            static_cast<size_t>(real_tcr.new_tool),
                            LayerKind::BridgeMerged, merged_key, merged_li, 0u,
                            // NEOTKO s205-5b.2c — merged is folded away (0×) when the
                            // bridge isn't needed (entered with the right tool) → census.
                            /*speculative=*/true});
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
NeoTower::get_tcr(float z_actual, size_t old_tool, size_t new_tool, bool sublayer_ctx,
                  bool record_consumption) const
{
    // NEOTKO_NEOTOWER_TAG s205-5b.2 — record the resolved slot (get_tcr channel →
    // from_finish=false) for the runtime consumption/order shadow. PEEK callers pass
    // record_consumption=false so decision/diagnostic lookups don't count as emissions.
    auto shadow = [&](bool merged, size_t a, size_t b) {
        if (record_consumption)
            m_shadow_sequence.push_back(ShadowSlot{merged, /*from_finish=*/false, a, b});
    };
    // NEOTKO_NEOTOWER_TAG s102 — dual-channel resolution. Primary channel comes
    // from the caller's context (sublayer prime vs real-layer dispatch); the other
    // channel acts as fallback so non-colliding legacy lookups keep resolving.
    const uint64_t key = make_key(z_actual, old_tool, new_tool);
    const auto& primary  = sublayer_ctx ? m_tcr_index_sub : m_tcr_index;
    const auto& fallback = sublayer_ctx ? m_tcr_index     : m_tcr_index_sub;
    const char* pname    = sublayer_ctx ? "sub" : "real";

    // 1. Direct hit in the context channel.
    if (auto it = primary.find(key); it != primary.end()) {
        const auto [li, si] = it->second;
        NT_LOG("get_tcr HIT(" << pname << ") z=" << z_actual << " old=" << old_tool
            << " new=" << new_tool << " → [" << li << "][" << si << "]");
        shadow(false, li, si);
        return m_result[li][si];
    }

    // 2. Bridge merged TCR (single map — bridge keys are unambiguous).
    // NEOTKO_NEOTOWER_TAG — bridge merged TCR:
    // GCode calls get_tcr(z, chain_tool, final_target) in one shot.
    // When a bridge TC existed for this z, we synthesized a merged TCR
    // combining bridge+real GCode.
    if (auto mit = m_merged_index.find(key); mit != m_merged_index.end()) {
        NT_LOG("get_tcr MERGED z=" << z_actual << " old=" << old_tool
            << " new=" << new_tool
            << " → merged[" << mit->second << "]");
        shadow(true, mit->second, 0);
        return m_merged_tcrs[mit->second];
    }

    // 3. NEOTKO_MPSCHEDULER_TAG — C4: redirect fused sublayer events. Redirect
    //    targets are scheduler-fused SUBLAYER events → resolve in the sub channel
    //    first, then real (pre-s102 behaviour as last resort).
    if (auto redir = m_z_redirect.find(key); redir != m_z_redirect.end()) {
        if (auto it = m_tcr_index_sub.find(redir->second); it != m_tcr_index_sub.end()) {
            const auto [li, si] = it->second;
            NT_LOG("get_tcr REDIRECT(sub) z=" << z_actual << " old=" << old_tool
                << " new=" << new_tool << " → fused [" << li << "][" << si << "]");
            shadow(false, li, si);
            return m_result[li][si];
        }
        if (auto it = m_tcr_index.find(redir->second); it != m_tcr_index.end()) {
            const auto [li, si] = it->second;
            NT_LOG("get_tcr REDIRECT(real) z=" << z_actual << " old=" << old_tool
                << " new=" << new_tool << " → fused [" << li << "][" << si << "]");
            shadow(false, li, si);
            return m_result[li][si];
        }
    }

    // 4. Cross-channel fallback (legacy single-map behaviour). Logged so the
    //    census shows which dispatches still depend on it (chain-gap candidates).
    if (auto it = fallback.find(key); it != fallback.end()) {
        const auto [li, si] = it->second;
        NT_LOG("get_tcr CROSS_CHANNEL(" << pname << "→" << (sublayer_ctx ? "real" : "sub")
            << ") z=" << z_actual << " old=" << old_tool
            << " new=" << new_tool << " → [" << li << "][" << si << "]");
        shadow(false, li, si);
        return m_result[li][si];
    }

    // NEOTKO_NEOTOWER_TAG s237 — BUG C. Una petición IDENTIDAD (old == new) que no
    // resuelve aquí NO es un error: es la ruta de diseño.
    //
    // Las visitas identidad/estructurales no se indexan nunca en el canal de TCRs
    // (m_tcr_index / m_tcr_index_sub) — viven en el canal finish (m_finish_layer_index,
    // vía get_finish_layer) y, en el dispatch de capa real, las resuelve el slot
    // planificado con su propia guarda de identidad en
    // GCode/NeoTowerDispatch.cpp:84 ("if (identity_req) ... fb.initial_tool ==
    // fb.new_tool"). Verificado en BIGTEST-ADAPTIVE: los 17 casos van seguidos de
    // `WT_EMIT_TRACE ... plan_slot=T1->T1` y la torre se emite entera.
    //
    // El sufijo `← ERROR` estaba mal puesto y costó caro: durante s236 esos 17
    // benignos ENMASCARARON los 3 MISS reales de BUG A (`grep MISS` daba 20 y el ruido
    // escondía la señal). Se degrada a una etiqueta propia y greppable — cualquier
    // `← ERROR` que quede en el log es, ahora sí, purga perdida de verdad.
    // Comportamiento IDÉNTICO: sigue devolviendo nullopt, no se toca ninguna ruta de
    // emisión. Ver docs/FUTURE/NEOTOWER_S236_BUG_PLAN.md §3.
    if (old_tool == new_tool) {
        NT_LOG("get_tcr() IDENTITY_NO_TCR z=" << z_actual
            << " old=" << old_tool << " new=" << new_tool
            << " ctx=" << pname
            << " (esperado — se resuelve por canal finish / slot planificado)");
        return std::nullopt;
    }

    NT_LOG("get_tcr() MISS z=" << z_actual
        << " old=" << old_tool << " new=" << new_tool
        << " ctx=" << pname << " ← ERROR");
    return std::nullopt;
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
                // NEOTKO_NEOTOWER_TAG s205-5b.2 — structural emission (from_finish=true).
                m_shadow_sequence.push_back(ShadowSlot{false, /*from_finish=*/true, li, si});
                return m_result[li][si];
            }
        }
        NT_LOG("get_finish_layer() MISS z=" << z << " ← ERROR");
        return std::nullopt;
    }
    const auto [li, si] = it->second;
    NT_LOG("get_finish_layer HIT z=" << z
        << " → [" << li << "][" << si << "]");
    // NEOTKO_NEOTOWER_TAG s205-5b.2 — structural emission (from_finish=true).
    m_shadow_sequence.push_back(ShadowSlot{false, /*from_finish=*/true, li, si});
    return m_result[li][si];
}

// ---------------------------------------------------------------------------
// has_pending_structural() — NEOTKO_NEOTOWER_TAG s240. Ver la cabecera para el porqué.
// Const y MUDA: no toca m_shadow_sequence y no escribe `← ERROR` cuando no hay nada.
// ---------------------------------------------------------------------------
bool NeoTower::has_pending_structural(float z) const
{
    uint64_t key = static_cast<uint64_t>(std::llround(z * 1000.f));
    auto it = m_finish_layer_index.find(key);
    if (it == m_finish_layer_index.end()) {
        auto redir = m_z_redirect_finish.find(key);
        if (redir == m_z_redirect_finish.end())
            return false;                       // no hay finish planificado aquí: normal
        it = m_finish_layer_index.find(redir->second);
        if (it == m_finish_layer_index.end())
            return false;
    }
    const auto [li, si] = it->second;
    if (li >= m_result.size() || si >= m_result[li].size())
        return false;
    // Ya emitido por otra vía (una purga del propio plano, o el REDIRECT/fusión de un plano
    // vecino — así es como z=7.95857 se libra de dejar hueco en BT, §29.7).
    for (const ShadowSlot& s : m_shadow_sequence)
        if (s.from_finish && !s.merged && s.a == li && s.b == si)
            return false;

    // NEOTKO_NEOTOWER_TAG s240b — CALLEJÓN SIN SALIDA, documentado para no repetirlo.
    //
    // Aquí se probó una segunda guarda: emitir sólo si la banda [z−h, z] no estaba ya
    // cubierta por material emitido, recorriendo m_shadow_sequence. La idea era buena —es la
    // lógica de V23— pero **no puede funcionar en línea**: la cobertura de esos planos la
    // aporta la canónica de su capa PADRE, que se emite DESPUÉS (medido en BT-A: siempre
    // ~13 líneas de log más tarde, a una z ligeramente mayor). Una consulta en tiempo de
    // emisión sólo ve el pasado, así que la guarda no filtró ni un caso de 28.
    //
    // Se retiró en vez de dejarla: no aportaba nada y, en una escena donde la canónica sí
    // se emitiera antes, habría suprimido una emisión NECESARIA. "No filtra nada pero puede
    // filtrar mal" es la peor combinación.
    //
    // La cobertura de Z es una propiedad del print ENTERO; quien la juzga es V23, al final.
    // Si hay que reducir emisiones, el criterio tiene que salir del PLAN (que sí se conoce
    // entero antes de emitir), no del registro de consumo a medias.
    return true;
}

// ---------------------------------------------------------------------------
// finalize_shadow() — NEOTKO_NEOTOWER_TAG s205-5b.2 — runtime consumption/order
// shadow. Called once at end of gcode emission (WipeTowerIntegration::finalize).
// Delegates the bijection tally to the pure helper; violations announce on the
// normal log (like V18), census/benign findings stay on the gated channel. This is
// a DETECTOR only — gcode is unchanged whether it fires or not. A standalone Bridge
// emitted 0× (folded into its merged TCR) is expected → census, not a violation.
// ---------------------------------------------------------------------------
// NEOTKO_NEOTOWER_TAG s205-5b.2b — see header. GCode.cpp calls this at the tower
// TCR emission sites that bypass get_tcr()/get_finish_layer() (planned-slot fallback,
// realign, orphan-finish, BBL mirrors), so the runtime shadow sees every emission.
void NeoTower::record_shadow_slot(bool merged, bool from_finish, size_t a, size_t b) const
{
    m_shadow_sequence.push_back(ShadowSlot{merged, from_finish, a, b});
}

void NeoTower::finalize_shadow() const
{
    const auto rep = NeoTowerPure::validate_shadow_consumption(m_emission_order, m_shadow_sequence);
    for (const std::string& msg : rep.violations)
        NT_INVARIANT_WARN(msg);
    for (const std::string& msg : rep.census)
        NT_LOG(msg);
    NT_LOG("[SHADOW] finalize: " << rep.emitted << " emissions vs "
        << m_emission_order.size() << " canonical entries — "
        << rep.violations.size() << " violation(s), " << rep.census.size() << " census");

    report_z_coverage();     // NEOTKO_NEOTOWER_TAG s240 — V23
    report_depth_accounting();
}

// ---------------------------------------------------------------------------
// report_z_coverage() — NEOTKO_NEOTOWER_TAG s240, V23.
//
// EL detector que faltaba: cobertura del eje Z por lo que la torre EMITE de verdad.
//
// WHY: toda la instrumentación previa de la torre mide el PLAN — alturas por evento,
// slots por capa, profundidad reservada. Y el plan casi siempre está bien. En BIGTEST el
// plan cubre el eje Z sin un solo hueco, y aun así el gcode tiene 5 tramos sin material,
// porque 5 entradas planificadas no llegan nunca a escribirse. Ningún invariante V1-V22
// puede ver eso: todos miran el lado de antes.
//
// V23 recorre las bandas [z-altura, z] de los TCR REALMENTE EMITIDOS (m_shadow_sequence,
// que es el registro de consumo en tiempo de emisión) y denuncia cualquier tramo de Z sin
// material. Es literalmente el censo que en s239/s240 hubo que hacer a mano con python
// sobre el gcode, y que habría cazado este bug solo desde s103.
//
// Emite DOS censos, y la diferencia entre ellos es el diagnóstico ya hecho:
//   · PLAN_GAP — hueco que ya existe en el plan. Nadie planificó material ahí.
//                (Familia B: `eff_layer_height` mantiene el nominal ante un salto grande.)
//   · EMIT_GAP — el plan lo cubría pero la emisión no. La entrada existe, su TCR existe y
//                tiene extrusión, y nadie lo despachó.  (Familia A.)
// Un EMIT_GAP que no sea también PLAN_GAP apunta SIEMPRE al lado de emisión; al revés,
// al planificador. Esa separación es la que costó media sesión de s240 hacer a mano.
//
// La altura sale de `nt_tcr_footprint()`, que lee el `;HEIGHT:` del gcode del propio TCR
// — la altura DEPOSITADA, no la nominal del plan (§28.3: con `_bd_wall_mult` no coinciden).
//
// 🚫 Este detector NO autoriza a tapar huecos con más flujo. Ver §28: el límite es
// volumétrico y la torre ya imprime pegada al techo. Un hueco se cierra con las pasadas
// que falten, cada una a su altura real.
//
// PURAMENTE DIAGNÓSTICO: const, sólo escribe log, no puede cambiar un solo G1.
// ---------------------------------------------------------------------------
void NeoTower::report_z_coverage() const
{
    if (m_emission_order.empty())
        return;

    // Una banda de material: [z - height, z]. `li`/`si` sólo para poder señalar al
    // culpable en el mensaje.
    struct Band { float lo = 0.f; float hi = 0.f; size_t li = 0; size_t si = 0; bool emitted = false; };

    // Altura DEPOSITADA de un slot, leída de su propio gcode. Devuelve 0 si el TCR no
    // declara altura o no existe — un slot sin gcode no aporta banda, y eso es
    // precisamente lo que debe salir como hueco, no como cobertura silenciosa.
    auto band_of = [this](const TowerEvent& te, bool emitted) -> Band {
        Band b;
        b.li = te.li; b.si = te.si; b.emitted = emitted;
        const std::string* gc = nullptr;
        if (te.kind == LayerKind::BridgeMerged) {
            if (te.li < m_merged_tcrs.size()) gc = &m_merged_tcrs[te.li].gcode;
        } else if (te.li < m_result.size() && te.si < m_result[te.li].size()) {
            gc = &m_result[te.li][te.si].gcode;
        }
        b.hi = te.z_actual;
        b.lo = te.z_actual;
        if (gc != nullptr) {
            const NtTcrFootprint fp = nt_tcr_footprint(*gc);
            if (fp.has_h && fp.height > 0.f)
                b.lo = te.z_actual - fp.height;
        }
        return b;
    };

    // Qué entradas canónicas se consumieron al menos una vez (mismo criterio que
    // report_depth_accounting: por slot {li,si}, nunca casando floats).
    std::set<std::pair<size_t, size_t>> consumed;
    for (const ShadowSlot& s : m_shadow_sequence)
        consumed.insert({s.a, s.b});

    std::vector<Band> plan_bands, emit_bands;
    plan_bands.reserve(m_emission_order.size());
    for (const TowerEvent& te : m_emission_order) {
        const bool was_emitted = consumed.count({te.li, te.si}) > 0;
        const Band b = band_of(te, was_emitted);
        if (b.hi > b.lo) {
            plan_bands.push_back(b);
            if (was_emitted) emit_bands.push_back(b);
        }
    }

    // Umbral: el plan trabaja en float y dos bandas contiguas pueden diferir en el último
    // bit. 1 µm es la resolución a la que se cuantiza Z en toda la torre (make_key), así
    // que por debajo de eso no hay hueco que discutir.
    const float GAP_EPS = 1e-3f;

    auto sweep = [&](std::vector<Band>& bands, const char* tag) -> std::pair<int, float> {
        std::sort(bands.begin(), bands.end(),
                  [](const Band& a, const Band& b) { return a.lo < b.lo; });
        int   n_gaps = 0;
        float total  = 0.f;
        float top    = 0.f;
        bool  first  = true;
        for (const Band& b : bands) {
            if (!first && b.lo - top > GAP_EPS) {
                ++n_gaps;
                total += b.lo - top;
                NT_INVARIANT_WARN(std::string("V23 ") + tag + ": tramo de Z SIN material ["
                    + std::to_string(top) + ".." + std::to_string(b.lo) + "] = "
                    + std::to_string(b.lo - top) + " mm — la siguiente banda que deposita es"
                    + " li=" + std::to_string(b.li) + " si=" + std::to_string(b.si)
                    + " z=" + std::to_string(b.hi)
                    + (b.emitted ? "" : " (y esa tampoco se emitió)"));
            }
            top   = first ? b.hi : std::max(top, b.hi);
            first = false;
        }
        return {n_gaps, total};
    };

    const auto plan_res = sweep(plan_bands, "PLAN_GAP");
    const auto emit_res = sweep(emit_bands, "EMIT_GAP");

    NT_LOG("V23 COVERAGE: plan=" << plan_bands.size() << " bandas → "
        << plan_res.first << " hueco(s) / " << plan_res.second << " mm"
        << " | emitido=" << emit_bands.size() << " bandas → "
        << emit_res.first << " hueco(s) / " << emit_res.second << " mm"
        << " | " << (plan_bands.size() - emit_bands.size()) << " entrada(s) planificada(s)"
        << " que nadie despachó"
        << ((emit_res.first > plan_res.first)
                ? "  ← el plan cubre más que la emisión: mirar el DESPACHO, no el planificador"
                : ""));
}

// ---------------------------------------------------------------------------
// report_depth_accounting() — NEOTKO_NEOTOWER_TAG s236, Fase F1.
//
// WHY: the tower's XY footprint is set by its deepest plan layer, and that depth
// is the SUM of the depth reserved by every toolchange slot on that layer. The
// planner reserves a slot for each (old,new) pair the plane MIGHT need, because
// the same-colour grouping and the tetris stacking order are only resolved at
// emission time (see NeoTowerEvent::speculative). Whatever the grouping does not
// use is depth already committed — a band of tower that gets built, layer after
// layer, and purged into by nobody.
//
// Until now the only trace of this was the Shadow census ("speculative spare
// emitted 0×"), reported per entry with no notion of what it cost. This turns
// that census into millimetres, per plane, so F2's pruning has a before/after
// number instead of a hunch. Observed on lancuak3-A34.3mf: the governing plane
// reserved 7 slots at ~16.8 mm each (117.35 mm total) with 4 of them logged as
// never emitted.
//
// PURELY DIAGNOSTIC: const, log-only, reads what generate()/emission already
// produced. It cannot change a single G1.
// ---------------------------------------------------------------------------
void NeoTower::report_depth_accounting() const
{
    if (m_emission_order.empty())
        return;

    // Which canonical entries were actually consumed at least once.
    std::set<std::pair<size_t, size_t>> consumed;
    for (const ShadowSlot& s : m_shadow_sequence)
        consumed.insert({s.a, s.b});

    // Accounted per PLAN LAYER (li), never by matching floats: TowerEvent already
    // carries the li that generate() assigned, and m_depth_by_li is dense over the
    // same index.
    struct LayerAcct { size_t reserved = 0; size_t used = 0; size_t firm = 0; float z = 0.f; };
    std::map<size_t, LayerAcct> by_li;

    for (const TowerEvent& te : m_emission_order) {
        LayerAcct& acct = by_li[te.li];
        ++acct.reserved;
        acct.z = te.z_actual;
        // "firm" = not a speculative spare. F2's whole premise is that the
        // emission grouping picks a path of the SAME LENGTH as the firm chain —
        // a spare SUBSTITUTES for a firm entry, it never adds one on top. If that
        // holds, reserving depth for the firm count alone is exactly right and
        // the spares can reuse those boxes. If it is ever false, reserving less
        // would overflow the purge box (mega-extrusions), so this counter is the
        // gate: `used` must never exceed `firm`. Watch it across scenes before
        // any depth is actually pruned.
        if (!te.speculative) ++acct.firm;
        if (consumed.count({te.li, te.si})) ++acct.used;
    }

    size_t total_reserved = 0, total_used = 0;
    size_t worst_li = 0, worst_unused = 0;
    float  worst_depth = 0.f, worst_waste = 0.f, worst_z = 0.f;

    for (const auto& [li, a] : by_li) {
        total_reserved += a.reserved;
        total_used     += a.used;
        if (a.reserved == 0) continue;
        const size_t unused = a.reserved - a.used;
        const float  depth  = li < m_depth_by_li.size() ? m_depth_by_li[li] : 0.f;
        // Slots on one layer sit side by side across the tower's depth, so an
        // even split is a good order-of-magnitude figure. F2 will swap this for
        // the real per-slot required_depth when it needs to prune precisely.
        const float  waste  = depth * (float(unused) / float(a.reserved));
        if (unused > 0) {
            NT_LOG("DEPTH_ACCT li=" << li << " z=" << a.z
                << " reserved=" << a.reserved << " firm=" << a.firm
                << " used=" << a.used
                << " unused=" << unused
                << " layer_depth=" << depth << "mm"
                << " wasted≈" << waste << "mm"
                << (a.used > a.firm ? "  *** F2_UNSAFE: used > firm ***" : ""));
            // "Governing" = the layer whose own reservation is deepest AND has
            // slack; that is the one actually setting the tower footprint.
            if (depth > worst_depth) {
                worst_depth = depth; worst_waste = waste;
                worst_z = a.z; worst_li = li; worst_unused = unused;
            }
        }
    }

    NT_LOG("DEPTH_ACCT TOTAL: " << total_used << "/" << total_reserved
        << " slots used across " << by_li.size() << " plan layers"
        << " | tower depth=" << (m_depth_by_li.empty()
              ? 0.f : *std::max_element(m_depth_by_li.begin(), m_depth_by_li.end())) << "mm"
        << " | deepest layer WITH slack: li=" << worst_li << " z=" << worst_z
        << " depth=" << worst_depth << "mm unused=" << worst_unused
        << " wasted≈" << worst_waste << "mm"
        << " (" << (worst_depth > 0.f ? 100.f * worst_waste / worst_depth : 0.f) << "%)");
}

// NEOTKO_NEOTOWER_TAG s205 (Fase 3) — V14 (frame) + V17 (delta-Z height) slice-time
// invariants, extracted verbatim from generate() (see the call site there for why they
// cannot live in validate_plan(): standalone_plane is marked in generate() and raw_result
// is a generate() local). Pure validation: const, log-only, no gcode/behaviour change.
// (a) V14: no tower FRAME block (brim chamfer / empty grid) on a synthetic sub-layer
//     plane; at most ONE brim block across all TCRs of one nominal layer.
// (b) V17: no TCR extrudes taller than the physical gap to the previous emitting plane.
void NeoTower::validate_result_frame_and_height(
    const std::vector<std::vector<WipeTower::ToolChangeResult>>& raw_result,
    const std::vector<NeoTowerEvent>&                            all_events) const
{
    {
        auto _count_marker = [](const std::string& g, const std::string& m) {
            int n = 0;
            for (size_t p = g.find(m); p != std::string::npos; p = g.find(m, p + m.size()))
                ++n;
            return n;
        };
        // Local µm quantizer — same formula as make_key / collect_all_events' z_um64
        // (that lambda is function-local there and not visible here).
        auto z_um64 = [](float z) -> uint64_t {
            return static_cast<uint64_t>(std::llround(static_cast<double>(z) * 1000.0));
        };
        // A plane is synthetic iff every event on it is a sublayer event (AND
        // semantics — must match WipeTowerInfo::is_synthetic in WipeTower2).
        std::set<uint64_t> _synth_zum, _real_zum, _staircase_zum;
        std::map<uint64_t, uint64_t> _zum_to_znom;
        for (const NeoTowerEvent& _ev : all_events) {
            const uint64_t _zum = z_um64(_ev.z_actual);
            (_ev.is_sublayer ? _synth_zum : _real_zum).insert(_zum);
            // NEOTKO_NEOTOWER_TAG s102-h — staircase planes legitimately carry
            // wall + grid (structural shell); only lámina planes must be frame-free.
            if (_ev.is_sublayer &&
                (_ev.standalone_plane || // s114 standalone painted layer = legit structural shell
                 (_ev.z_nominal - _ev.z_actual) >= NeoTowerZ::SAME_PLANE_MAX_OFF))
                _staircase_zum.insert(_zum);
            _zum_to_znom[_zum] = z_um64(_ev.z_nominal);
        }
        std::map<uint64_t, int> _brims_per_nominal;
        int _v14_warnings = 0;
        for (size_t _li = 0; _li < raw_result.size(); ++_li) {
            for (size_t _si = 0; _si < raw_result[_li].size(); ++_si) {
                const auto&    _tcr  = raw_result[_li][_si];
                const uint64_t _zum  = z_um64(static_cast<float>(_tcr.print_z));
                const bool _is_synth_plane = _synth_zum.count(_zum) && !_real_zum.count(_zum);
                const bool _is_staircase   = _staircase_zum.count(_zum) > 0;
                const int  _brims = _count_marker(_tcr.gcode, "WIPE_TOWER_BRIM_START");
                const int  _grids = _count_marker(_tcr.gcode, "CP EMPTY GRID START");
                // s102-h: brim is illegal on ANY synthetic plane; grid only on
                // lámina (same-plane) ones — staircase planes need it.
                if (_is_synth_plane && (_brims > 0 || (_grids > 0 && !_is_staircase))) {
                    NT_INVARIANT_WARN("V14a FRAME on synthetic plane z=" << _tcr.print_z
                        << " li=" << _li << " si=" << _si
                        << " brims=" << _brims << " grids=" << _grids
                        << " staircase=" << _is_staircase);
                    ++_v14_warnings;
                }
                const auto _zn = _zum_to_znom.find(_zum);
                _brims_per_nominal[_zn != _zum_to_znom.end() ? _zn->second : _zum] += _brims;
            }
        }
        for (const auto& _kv : _brims_per_nominal)
            if (_kv.second > 1) {
                NT_INVARIANT_WARN("V14b " << _kv.second
                    << " brim blocks within nominal layer z=" << (_kv.first / 1000.f));
                ++_v14_warnings;
            }
        if (_v14_warnings == 0)
            NT_LOG("[VALIDATE] V14 frame invariant OK ("
                << raw_result.size() << " plan layers scanned)");

        // NEOTKO_NEOTOWER_TAG s103 — V17: delta-Z height invariant.
        // No TCR may extrude at a height GREATER than the physical gap to the
        // previous emitting tower plane (real or staircase; láminas leave no
        // plane). Over-height = over-extrusion crammed into a smaller gap —
        // exactly the post-staircase wall bug this session fixes. The floor to
        // NOMINAL_LH_MIN is legal, hence the max() in the bound. Pure validation.
        {
            std::vector<uint64_t> _emitting_sorted;
            for (uint64_t _z : _real_zum) _emitting_sorted.push_back(_z);
            for (uint64_t _z : _staircase_zum) _emitting_sorted.push_back(_z);
            std::sort(_emitting_sorted.begin(), _emitting_sorted.end());
            auto _parse_height = [](const std::string& g) -> float {
                const size_t p = g.find(";HEIGHT:");
                return p == std::string::npos ? -1.f
                                              : std::strtof(g.c_str() + p + 8, nullptr);
            };
            int _v17_warnings = 0;
            for (size_t _li = 0; _li < raw_result.size(); ++_li)
                for (size_t _si = 0; _si < raw_result[_li].size(); ++_si) {
                    const auto&    _tcr = raw_result[_li][_si];
                    const float    _h   = _parse_height(_tcr.gcode);
                    if (_h <= 0.f)
                        continue;
                    const uint64_t _zum = z_um64(static_cast<float>(_tcr.print_z));
                    // Previous emitting plane strictly below this TCR's plane.
                    auto _it = std::lower_bound(_emitting_sorted.begin(),
                                                _emitting_sorted.end(), _zum);
                    const float _prev_z = (_it == _emitting_sorted.begin())
                                          ? 0.f
                                          : (*(_it - 1) / 1000.f);
                    const float _gap   = static_cast<float>(_tcr.print_z) - _prev_z;
                    const float _bound = std::max(_gap, NeoTowerZ::NOMINAL_LH_MIN) + 0.002f;
                    if (_h > _bound) {
                        NT_INVARIANT_WARN("V17 OVER-HEIGHT z=" << _tcr.print_z
                            << " li=" << _li << " si=" << _si
                            << " height=" << _h << " gap=" << _gap
                            << " (support plane z=" << _prev_z << ")");
                        ++_v17_warnings;
                    }
                    // NEOTKO_NEOTOWER_TAG s237 — BUG B: V17 sólo miraba por ARRIBA.
                    // Una altura declarada POR DEBAJO del mínimo físico no sobre-extruye,
                    // pero miente: el `;HEIGHT` lo leen el visor, el estimador de tiempo y
                    // el control de ventilación. `TCR [152][0] ;HEIGHT:0.0133076` (13 µm)
                    // vivió sin detector toda s236 porque nadie validaba este lado.
                    // Misma constante y mismo ancla que la cota de arriba.
                    else if (_h < NeoTowerZ::NOMINAL_LH_MIN - 0.002f) {
                        NT_INVARIANT_WARN("V17 UNDER-HEIGHT z=" << _tcr.print_z
                            << " li=" << _li << " si=" << _si
                            << " height=" << _h
                            << " min=" << NeoTowerZ::NOMINAL_LH_MIN
                            << " (altura declarada por debajo del mínimo físico)");
                        ++_v17_warnings;
                    }
                }
            if (_v17_warnings == 0)
                NT_LOG("[VALIDATE] V17 delta-Z height invariant OK");
        }
    }
}

// NEOTKO_NEOTOWER_TAG_START — hardening P3 + P5
void NeoTower::validate_plan() const
{
    using NeoTowerZ::Z_EPS_GROUP;

    int warn_count = 0;
    auto WARN = [&warn_count](const std::string& msg) {
        ++warn_count;
        // NEOTKO_NEOTOWER_TAG s204 (Fase 0) — V1-V13 violations announce unconditionally in the
        // normal log (the DETECTOR must speak on every slice); the channel line is preserved for
        // the verbose forensic scrape when ORCA_DEBUG_WIPETOWER is on.
        BOOST_LOG_TRIVIAL(warning) << "[NeoTower][VALIDATE] " << msg;
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

    // NEOTKO_NEOTOWER_TAG s240 — V4/V5 sólo tienen sentido DESPUÉS de generate().
    //
    // validate_plan() se llama dos veces: al final de collect_and_plan() (línea ~224) y
    // otra vez al final de generate() (línea ~3158). En la PRIMERA los índices
    // (m_tcr_index, m_tcr_index_sub, m_finish_layer_index) están vacíos por construcción
    // — se rellenan en la Fase 3 de generate(). Los invariantes que preguntan "¿tiene este
    // evento su entrada en el índice?" contestan que NO para TODOS los eventos, siempre.
    //
    // Medido en BIGTEST (s240): 380 avisos V4 + 8 V5 en el pase pre-generate, y CERO en el
    // post-generate. O sea el 100% del ruido histórico de V4/V5 era este pase. Ese ruido no
    // es cosmético: es exactamente lo que hizo invisible el bug de los huecos de Z (§28),
    // porque un detector que grita 380 veces contra 5 fallos reales no es un detector. Es
    // la tercera reincidencia del mismo patrón (§25.2, s237 IDENTITY_NO_TCR).
    //
    // No se puede simplemente mover la llamada: el pase temprano SÍ valida lo que ya existe
    // (V1/V2/V3/V7… sobre los eventos y el plan). Lo que se salta aquí es sólo lo que
    // depende de los índices.
    // (V9 y V10-V13 ya se auto-protegen con `!m_tcr_index.empty()` / `!m_result.empty()`;
    // V4 y V5 eran los dos que no lo hacían.)
    const bool _indices_ready = !m_result.empty();
    if (!_indices_ready) {
        NT_LOG("VALIDATE: pase pre-generate — V4/V5 omitidos"
            << " (los índices se construyen en generate() Fase 3;"
            << " evaluarlos aquí daría un falso positivo por evento)");
    }

    // V4: non-identity events (m_events) have tcr index
    // NEOTKO_NEOTOWER_TAG s102 — check the event's own channel (dual-channel index).
    for (size_t i = 0; _indices_ready && i < m_events.size(); ++i) {
        const auto& ev = m_events[i];
        const uint64_t key = make_key(ev.z_actual, ev.old_tool, ev.new_tool);
        const auto& _chan = ev.is_sublayer ? m_tcr_index_sub : m_tcr_index;
        if (_chan.find(key) == _chan.end()) {
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
    for (size_t i = 0; _indices_ready && i < m_growth_events.size(); ++i) {
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
    if (!m_tcr_index.empty() || !m_tcr_index_sub.empty()) {
        // NEOTKO_NEOTOWER_TAG s102 — dual-channel: a redirect target may live in
        // either channel (fused targets are sublayer events → usually sub).
        for (const auto& [from_key, to_key] : m_z_redirect) {
            if (m_tcr_index.find(to_key) == m_tcr_index.end() &&
                m_tcr_index_sub.find(to_key) == m_tcr_index_sub.end()) {
                std::ostringstream oss;
                oss << "V9: m_z_redirect target missing in both tcr index channels"
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

    // NEOTKO_NEOTOWER_TAG_START — hardening P9 (s79e session): cross-map integrity.
    // V10–V13 only run after generate() populated m_result. Detect structural
    // incoherence at slice time so a future regression surfaces here instead of as
    // a runtime get_tcr MISS / append_tcr unexpected during printing.
    if (!m_result.empty()) {
        // V10: every m_merged_index target is in-bounds for m_merged_tcrs.
        for (const auto& [key, idx] : m_merged_index) {
            if (idx >= m_merged_tcrs.size()) {
                std::ostringstream oss;
                oss << "V10: m_merged_index out-of-bounds key=" << key
                    << " idx=" << idx << " merged_tcrs.size=" << m_merged_tcrs.size();
                WARN(oss.str());
            }
        }

        // V11: m_merged_index and m_tcr_index do not share keys (bridge merges would
        // shadow regular TCRs and silently emit the wrong sequence).
        for (const auto& [key, _idx] : m_merged_index) {
            // NEOTKO_NEOTOWER_TAG s102 — check both channels.
            if (m_tcr_index.find(key) != m_tcr_index.end() ||
                m_tcr_index_sub.find(key) != m_tcr_index_sub.end()) {
                std::ostringstream oss;
                oss << "V11: key shared between m_merged_index and a tcr index channel"
                    << " key=" << key;
                WARN(oss.str());
            }
        }

        // V12: tcr index targets are in-bounds for m_result (both channels, s102).
        for (const auto* _map : { &m_tcr_index, &m_tcr_index_sub })
            for (const auto& [key, ls] : *_map) {
                if (ls.first >= m_result.size()
                    || ls.second >= m_result[ls.first].size()) {
                    std::ostringstream oss;
                    oss << "V12: tcr index out-of-bounds key=" << key
                        << " li=" << ls.first << " si=" << ls.second
                        << " (result.size=" << m_result.size()
                        << (ls.first < m_result.size()
                            ? std::string(", layer.size=") + std::to_string(m_result[ls.first].size())
                            : std::string())
                        << ")";
                    WARN(oss.str());
                }
            }

        // V13: m_finish_layer_index targets are in-bounds for m_result.
        for (const auto& [zum, ls] : m_finish_layer_index) {
            if (ls.first >= m_result.size()
                || ls.second >= m_result[ls.first].size()) {
                std::ostringstream oss;
                oss << "V13: m_finish_layer_index out-of-bounds zum=" << zum
                    << " li=" << ls.first << " si=" << ls.second
                    << " (result.size=" << m_result.size()
                    << (ls.first < m_result.size()
                        ? std::string(", layer.size=") + std::to_string(m_result[ls.first].size())
                        : std::string())
                    << ")";
                WARN(oss.str());
            }
        }

        // NEOTKO_NEOTOWER_TAG s205-5b.1 — V18: the canonical emission-order list is
        // a faithful bijection of the four emittable lookup maps (single-source
        // shadow check). Silent when healthy; a warning means the list and the maps
        // disagree — i.e. the future positional-consume model would mis-emit.
        for (const std::string& msg : NeoTowerPure::validate_emission_bijection(
                 m_emission_order, m_tcr_index, m_tcr_index_sub,
                 m_finish_layer_index, m_merged_index)) {
            WARN(msg);
        }
    }
    // NEOTKO_NEOTOWER_TAG_END

    // Summary
    if (warn_count == 0) {
        NT_LOG("[VALIDATE] plan OK ("
            << m_events.size() << " real_tc, "
            << m_growth_events.size() << " growth, "
            << m_tcr_index.size() << " tcr_real, "
            << m_tcr_index_sub.size() << " tcr_sub, "
            << m_finish_layer_index.size() << " finish_layer, "
            << m_z_redirect.size() << " z_redirect, "
            << m_z_redirect_finish.size() << " z_redirect_finish, "
            << m_merged_tcrs.size() << " merged_tcr)");
    } else {
        NT_LOG("[VALIDATE] plan has " << warn_count
            << " warnings — check above lines for details");
    }
}
// NEOTKO_NEOTOWER_TAG_END

} // namespace Slic3r

// NEOTKO_NEOTOWER_TAG_END
