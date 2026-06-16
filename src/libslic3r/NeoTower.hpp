// NeoTower.hpp — Post-slice wipe tower for Neotko FullSpectrum 0.95
//
// PROBLEM WITH WipeTower2:
//   plan_toolchange() is called layer-by-layer DURING slicing → no global view.
//   MultiPass sublayers require a bolt-on hack (NEOTKO_MULTIPASS_PRIME_TAG +
//   plan_local_z_reserve) because WipeTower2 cannot see sublayer events at all.
//
// NEOTOWER SOLUTION:
//   1. collect_all_events() — runs AFTER ToolOrdering + m_multipass_sublayers are
//      fully built. Sees every toolchange in the print, real layers AND sublayers.
//   2. plan() — groups events by z_nominal, assigns sub-slots for sublayer events.
//      Tower geometry (width/depth) computed globally from max wipe volume.
//   3. generate() — iterates the plan, calls WipeTower2::local_z_tool_change() for
//      each slot. Output is std::vector<std::vector<WipeTower::ToolChangeResult>>,
//      identical in type to WipeTower2::generate() output.
//
// REUSE STRATEGY:
//   WipeTower2::local_z_tool_change(new_tool, cleaning_box, wipe_volume) is a
//   PUBLIC method that accepts an external cleaning_box. NeoTower builds the
//   correct cleaning_box per slot and delegates all GCode generation to it.
//   No duplication of WipeTowerWriter2 or toolchange_Unload/Change/Load/Wipe.
//   No friend declarations needed.
//
// INTEGRATION:
//   - NeoTower is activated when Print config key `neotko_wipe_tower` == true.
//   - Print.cpp::_make_wipe_tower() calls NeoTower::collect_and_plan() instead
//     of the WipeTower2 plan_toolchange loop.
//   - The NEOTKO_MULTIPASS_PRIME_TAG block in Print.cpp is a no-op when active.
//   - GCode.cpp calls neotower.get_tcr(z, old_tool, new_tool) instead of
//     wipe_tower.tool_change(new_tool).
//
// NEOTKO_NEOTOWER_TAG_START

#pragma once

#include <vector>
#include <unordered_map>
#include <optional>
#include <cstddef>
#include <cstdint>

#include "libslic3r/libslic3r.h"           // coordf_t
#include "libslic3r/GCode/WipeTower.hpp"   // WipeTower::ToolChangeResult, box_coordinates
#include "libslic3r/GCode/WipeTower2.hpp"  // WipeTower2 (used for GCode generation)

namespace Slic3r {

class Print;
class PrintConfig;
class PrintRegionConfig;

// ---------------------------------------------------------------------------
// NeoTowerEvent
//
// One toolchange anywhere in the print. Collected from:
//   - LayerTools (real layers, has_wipe_tower == true)  → is_sublayer = false
//   - PrintObject::m_multipass_sublayers                → is_sublayer = true
//
// All events are collected AFTER ToolOrdering is complete.
// ---------------------------------------------------------------------------
struct NeoTowerEvent {
    // Nominal layer Z (the real LayerTools::print_z this event belongs to).
    // For sublayer events this is the parent real layer's Z, not the sub-Z.
    float  z_nominal    = 0.f;

    // Actual Z at which the toolchange physically executes.
    //   Real layer:    z_actual == z_nominal
    //   Sublayer:      z_actual  < z_nominal  (the MultiPass virtual sub-Z)
    float  z_actual     = 0.f;

    // Nominal layer height of the REAL layer (not the sublayer slice height).
    // Used for slot height calculation.
    float  layer_height = 0.f;

    // 0-based physical extruder IDs.
    size_t old_tool     = 0;
    size_t new_tool     = 0;

    // Volume of filament to purge (mm³).
    // Real-layer events: from wipe_volumes matrix * flush_multiplier.
    // Sublayer/prime events: from multipass_prime_volume config.
    float  wipe_volume  = 0.f;

    // True for MultiPass virtual sublayer events.
    bool   is_sublayer  = false;

    // NEOTKO_NEOTOWER_TAG s114 — standalone painted-layer plane. True for a
    // sublayer event whose z_nominal carries NO real (non-sublayer) event: the
    // canonical layer is realised ENTIRELY by MultiPass sublayers (PathBlend /
    // ColorMix / mixed / any gradient shape) → it IS the layer, not a decoration
    // of a real one. The s102-h lámina test (z_nominal-z_actual < SAME_PLANE_MAX_OFF)
    // assumes a real parent builds the frame; for a fully-painted layer there is
    // none, so it must be treated as a structural plane (keep wall+grid, advance
    // the emitting-plane tracker). Marked once in generate() by composition, so
    // the signal is effect/shape-agnostic. Set false on real layers and on
    // sublayers that genuinely decorate a real layer at the same z_nominal.
    bool   standalone_plane = false;

    // NEOTKO_MPSCHEDULER_TAG s79b — sandwich-context TC: the tower visit must still happen
    // (drip control), but the ramming EXTRUSION is skipped so the deposit lands in the
    // before-print wipe instead of the after-print ramming. Set for sublayer band TCs and
    // the post-sandwich perimeter recovery TC. Body real-layer TCs leave this false (they
    // keep full ramming — their box is large enough that ramming doesn't starve the wipe).
    bool   no_ramming   = false;
};

// ---------------------------------------------------------------------------
// NeoTowerSlot
//
// One physical wipe action at a specific Z within a NeoTowerLayer.
// Multiple slots can be stacked within one nominal layer to handle sublayer
// events at different sub-heights.
// ---------------------------------------------------------------------------
struct NeoTowerSlot {
    // Absolute Z of the nozzle when it enters the tower for this action.
    float  z           = 0.f;

    // Height budget for this wipe action (mm).
    //   Nominal slot:   == NeoTowerLayer::layer_height
    //   Sublayer slot:  min_layer_height (>= 0.04 mm, <= layer_height/2)
    float  slot_height = 0.f;

    // 0-based physical extruder IDs.
    size_t old_tool    = 0;
    size_t new_tool    = 0;

    // Volume to purge (mm³).
    float  wipe_volume = 0.f;

    // Index into merged all_events (plan-local) for traceability.
    size_t event_idx   = 0;
};

// ---------------------------------------------------------------------------
// NeoTowerLayer
//
// All wipe actions for one nominal layer, sorted by slot Z ascending.
// The highest-Z slot corresponds to the nominal layer's toolchange (if any);
// lower slots are sublayer events.
// ---------------------------------------------------------------------------
struct NeoTowerLayer {
    // Matches the real LayerTools::print_z.
    float z_nominal    = 0.f;

    // Real layer height.
    float layer_height = 0.f;

    // Slots in ascending Z order. May be empty if only structural pass needed.
    std::vector<NeoTowerSlot> slots;

    // True if this layer needs a structural perimeter even without toolchanges
    // (maintains tower integrity). If false and slots is empty, layer is skipped.
    bool  needs_structure = false;
};

// ---------------------------------------------------------------------------
// NeoTowerPlan
//
// The complete planned tower for the whole print.
// Built by NeoTower::plan(), consumed by NeoTower::generate().
// ---------------------------------------------------------------------------
struct NeoTowerPlan {
    // Tower position: bottom-left corner in print-space mm.
    float tower_x     = 0.f;
    float tower_y     = 0.f;

    // Rectangular footprint: fixed width, decoupled calculated depth.
    // Computed from max wipe volume + perimeter allowance.
    float tower_width = 0.f;
    float tower_depth = 0.f;  // decoupled rectangular depth

    // All layers, ascending Z.
    std::vector<NeoTowerLayer> layers;
};

// ---------------------------------------------------------------------------
// NeoTower
//
// Drop-in planning replacement for WipeTower2.
//
// Lifecycle:
//   NeoTower tower(config, region_config, plate_idx, plate_origin,
//                  wiping_matrix, initial_tool);
//   tower.collect_and_plan(print);               // after process() completes
//   tower.generate(wipe_tower_data.tool_changes); // fill WipeTowerData
//
// GCode.cpp query:
//   auto tcr = tower.get_tcr(z_actual, old_tool, new_tool);
// ---------------------------------------------------------------------------
class NeoTower {
public:
    // Signature mirrors WipeTower2 constructor so Print.cpp can switch with
    // minimal diff. plate_idx / plate_origin used for tower position lookup.
    NeoTower(const PrintConfig&                          config,
             const PrintRegionConfig&                    default_region_config,
             int                                         plate_idx,
             Vec3d                                       plate_origin,
             const std::vector<std::vector<float>>&      wiping_matrix,
             size_t                                       initial_tool);

    // -----------------------------------------------------------------------
    // Phase 1 + 2  (call once, after Print::process() completes)
    // -----------------------------------------------------------------------
    void collect_and_plan(const Print& print);

    // NEOTKO_NEOTOWER_TAG_START — hardening P3
    // Validate plan structure after collect_and_plan(). Logs warnings for
    // invariant violations. V1–V5, V7.
    void validate_plan() const;
    // NEOTKO_NEOTOWER_TAG_END

    // -----------------------------------------------------------------------
    // Phase 3: fill result with ToolChangeResults.
    // result[layer_idx][slot_idx] mirrors WipeTower2::generate() output layout.
    // -----------------------------------------------------------------------
    void generate(std::vector<std::vector<WipeTower::ToolChangeResult>>& result);

    // -----------------------------------------------------------------------
    // TCR lookup for GCode.cpp.
    //
    // Key: (z_actual, old_tool, new_tool).
    //   - Real-layer events: z_actual == LayerTools::print_z
    //   - Sublayer events:   z_actual == MultiPassSubLayer::print_z
    //
    // Returns nullopt only on programming error (missing event).
    // GCode.cpp should BOOST_LOG on nullopt.
    // -----------------------------------------------------------------------
    // NEOTKO_NEOTOWER_TAG s102 — `sublayer_ctx`: lookup channel. The last sublayer
    // plane and the real layer share the same µm-quantized Z by design (sub
    // z=0.8798 and real z=0.88 both → 880 µm), so a sub TC and a real TC with the
    // same (old,new) pair produce IDENTICAL keys. One map cannot disambiguate them
    // (s102 finding: the canonical frame TCR was served to sublayer prime lookups →
    // duplicate brim at 0.878/0.880 and orphaned sub purge). Sublayer prime
    // dispatches pass true; real-layer / recovery dispatches keep the default.
    // Resolution falls back to the other channel (logged CROSS_CHANNEL) so legacy
    // behaviour is preserved wherever the channels do not collide.
    std::optional<WipeTower::ToolChangeResult> get_tcr(float  z_actual,
                                                        size_t old_tool,
                                                        size_t new_tool,
                                                        bool   sublayer_ctx = false) const;

    // -----------------------------------------------------------------------
    // Structural-layer TCR lookup for GCode.cpp.
    //
    // Returns the finish_layer equivalent TCR for a real layer that has no
    // real toolchange (T→T single-filament MP layers). This TCR deposits
    // material in the tower footprint to maintain physical tower integrity
    // between sublayer primes.
    //
    // Key: z_actual only (no tool IDs — structural events always T→T).
    // Returns nullopt if no structural layer was planned at this Z.
    // -----------------------------------------------------------------------
    std::optional<WipeTower::ToolChangeResult> get_finish_layer(float z) const;

    // -----------------------------------------------------------------------
    // Accessors replacing WipeTower2 equivalents in Print.cpp / GCode.cpp
    // -----------------------------------------------------------------------
    float get_depth()      const { return m_plan.tower_depth; }
    float get_width()      const { return m_plan.tower_width; }
    float get_brim_width() const { return m_brim_width; }
    float get_height()     const;
    float get_x()          const { return m_plan.tower_x; }
    float get_y()          const { return m_plan.tower_y; }
    int   get_number_of_toolchanges() const { return m_num_toolchanges; }
    const std::vector<float>& get_used_filament() const { return m_used_filament; }

    // Returns true when the config key neotko_wipe_tower is true.
    // Used by Print.cpp / GCode.cpp to choose between NeoTower and WipeTower2.
    static bool is_enabled(const PrintConfig& config);

    // NEOTKO_NEOTOWER_TAG s103-bd — "box-in-drawer": extra width the REAL tower
    // box grows by (2·perimeter_width; 0 when NeoTower disabled). Every plate
    // footprint consumer in Print.cpp must add this to prime_tower_width.
    static float box_drawer_extra_width(const PrintConfig& config);

private:
    // -----------------------------------------------------------------------
    // Phase 1 — internal
    // Walks ToolOrdering + m_multipass_sublayers, builds m_events.
    // -----------------------------------------------------------------------
    void collect_all_events(const Print& print);

    // -----------------------------------------------------------------------
    // Phase 2 — internal
    // Groups m_events into m_plan.layers with correct slot Z/height assignment.
    // -----------------------------------------------------------------------
    void plan();

    // Compute required wipe depth (mm) for a given volume, slot height, tower width.
    float wipe_depth_for_volume(float wipe_volume, float slot_height) const;

    // Slot height for sublayer events.
    // Returns max(0.04f, min(m_min_layer_height, layer_height * 0.4f))
    float sublayer_slot_height(float nominal_layer_height) const;

    // -----------------------------------------------------------------------
    // Lookup key encoding
    // key = round(z_actual * 1000) * 10000 + old_tool * 100 + new_tool
    // Quantizes z to 1 µm. Supports up to 99 physical tools.
    // -----------------------------------------------------------------------
    static uint64_t make_key(float z_actual, size_t old_tool, size_t new_tool);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    const PrintConfig*                     m_print_config    = nullptr;
    const PrintRegionConfig*               m_region_config   = nullptr;

    // plate_idx and plate_origin stored for WipeTower2 construction in generate().
    int                                    m_plate_idx       = 0;
    Vec3d                                  m_plate_origin    = Vec3d::Zero();

    // Wipe volume matrix [old_tool][new_tool] mm³, pre-multiplied by flush_multiplier.
    std::vector<std::vector<float>>        m_wipe_volumes;

    size_t                                 m_initial_tool    = 0;

    // Phase 1 output.
    // NEOTKO_NEOTOWER_TAG_START — hardening P5
    // Separated event channels:
    //   m_events:        ONLY real TCs (old_tool != new_tool). Indexed by m_tcr_index.
    //   m_growth_events: ONLY identity events (old_tool == new_tool). Indexed by
    //                    m_finish_layer_index.
    // Invariant: every event is in exactly one bucket. validate_plan() V8 checks this.
    std::vector<NeoTowerEvent>             m_events;
    std::vector<NeoTowerEvent>             m_growth_events;
    // NEOTKO_NEOTOWER_TAG_END

    // Phase 2 output.
    NeoTowerPlan                           m_plan;

    // Phase 3 output — m_result[layer_idx][slot_idx].
    std::vector<std::vector<WipeTower::ToolChangeResult>> m_result;

    // O(1) TCR lookup: make_key(...) → {layer_idx, slot_idx} into m_result.
    // NEOTKO_NEOTOWER_TAG s102 — dual channel: m_tcr_index holds REAL-layer TCs,
    // m_tcr_index_sub holds sublayer TCs (ev.is_sublayer). The last sub plane and
    // the real plane quantize to the same µm, so the same (z_um, old, new) key can
    // legitimately exist in BOTH channels pointing at different TCRs (Hallazgo VII
    // collision, s102). get_tcr() picks the channel from its sublayer_ctx argument
    // and cross-falls-back. V16 detects collisions WITHIN a channel (still bugs).
    std::unordered_map<uint64_t, std::pair<size_t, size_t>> m_tcr_index;
    std::unordered_map<uint64_t, std::pair<size_t, size_t>> m_tcr_index_sub;

    // O(1) structural-layer lookup: z_um → {layer_idx, slot_idx} into m_result.
    // Only populated for structural events (old_tool == new_tool, !is_sublayer).
    std::unordered_map<uint64_t, std::pair<size_t, size_t>> m_finish_layer_index;

    // NEOTKO_MPSCHEDULER_TAG — Z redirect for scheduler-fused events.
    // When the scheduler merges multiple sublayer events at distinct z_actual
    // into a single NeoTowerEvent at z_max, GCode still calls get_tcr() with
    // each original z_actual. This map redirects those lookups.
    // key: make_key(z_original, old, new) → value: make_key(z_fused, old, new)
    std::unordered_map<uint64_t, uint64_t> m_z_redirect;
    // Same for identity events: z_um(z_original) → z_um(z_fused)
    std::unordered_map<uint64_t, uint64_t> m_z_redirect_finish;

    // NEOTKO_NEOTOWER_TAG — bridge merged TCRs.
    // When a bridge TC exists (chain_tool ≠ ev.old_tool), generate() produces
    // slot[i]=bridge(chain→old) and slot[i+1]=real(old→new) at the same wt2_li.
    // GCode calls get_tcr(z, chain_tool, new_tool) in one shot — it doesn't know
    // to call the bridge first.  We synthesize a merged TCR: initial=chain_tool,
    // new_tool=real.new_tool, gcode=bridge.gcode+real.gcode.
    // key: make_key(z, bridge.from_tool, real.new_tool) → index into m_merged_tcrs.
    std::vector<WipeTower::ToolChangeResult>      m_merged_tcrs;
    std::unordered_map<uint64_t, size_t>          m_merged_index;

    // Statistics (populated during generate()).
    int                m_num_toolchanges = 0;
    std::vector<float> m_used_filament;
    float              m_brim_width      = 0.f;

    // Geometry constants extracted from config at construction.
    float m_nozzle_diameter    = 0.4f;
    float m_filament_diameter  = 1.75f;
    float m_perimeter_width    = 0.5f;
    float m_min_layer_height   = 0.04f;
};

} // namespace Slic3r

// NEOTKO_NEOTOWER_TAG_END
