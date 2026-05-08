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
#include <set>
#include <cstdlib>

// NEOTKO_NEOTOWER_TAG_START — debug helper
// ORCA_DEBUG_NEOTOWER=1 or ORCA_DEBUG_ALL=1
namespace {
inline bool neotower_debug_on() {
    static const bool on = (std::getenv("ORCA_DEBUG_NEOTOWER") != nullptr ||
                            std::getenv("ORCA_DEBUG_ALL")      != nullptr);
    return on;
}
} // anon
#define NT_LOG(msg) do { if (neotower_debug_on()) \
    BOOST_LOG_TRIVIAL(debug) << "[NEOTOWER] " << msg; } while(0)
// NEOTKO_NEOTOWER_TAG_END

namespace Slic3r {

// ---------------------------------------------------------------------------
// Key encoding: quantize z to 1 µm, pack with tool IDs.
// Supports up to 99 physical extruders.
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
    if (const auto* opt = config.option<ConfigOptionBool>("neotko_wipe_tower"))
        return opt->value;
    return false;
}

// ---------------------------------------------------------------------------
// Public entry point: collect_and_plan()
// ---------------------------------------------------------------------------
void NeoTower::collect_and_plan(const Print& print)
{
    collect_all_events(print);
    plan();
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
            const float lh       = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                       ? lt.wipe_tower_layer_height
                                                       : lt.layer_height);
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

    // Second pass: walk sequentially, tracking current_tool across ALL LayerTools.
    // Emit events for is_mp_sublayer layers.
    {
        size_t sub_current_tool = tool_ordering.first_extruder();
        if (sub_current_tool == (size_t)-1)
            sub_current_tool = m_initial_tool;

        for (const LayerTools& lt : tool_ordering) {
            const float z_act = static_cast<float>(lt.print_z);
            const float lh    = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                    ? lt.wipe_tower_layer_height
                                                    : lt.layer_height);

            if (!lt.is_mp_sublayer) {
                // Real layer: just advance current_tool through its extruders.
                for (const unsigned int ext_id : lt.extruders) {
                    if (ext_id != (unsigned int)sub_current_tool)
                        sub_current_tool = ext_id;
                }
                continue;
            }

            // Sublayer: every extruder transition needs a tower slot.
            float z_nom = z_act; // fallback
            auto it = sublayer_to_nominal.find(z_um64(z_act));
            if (it != sublayer_to_nominal.end())
                z_nom = it->second;

            // Prime sentinel: first extruder in this sublayer LayerTools.
            // WipeTower2 (NEOTKO_MULTIPASS_PRIME_TAG) reserved a slot for this.
            // Emit old==new so get_tcr() can serve it.
            if (!lt.extruders.empty() && mp_prime_vol > 0.f) {
                size_t prime_tool = (size_t)lt.extruders.front();
                NeoTowerEvent prime_ev;
                prime_ev.z_nominal    = z_nom;
                prime_ev.z_actual     = z_act;
                prime_ev.layer_height = lh;
                prime_ev.old_tool     = prime_tool;
                prime_ev.new_tool     = prime_tool; // sentinel: old==new
                prime_ev.wipe_volume  = mp_prime_vol;
                prime_ev.is_sublayer  = true;
                m_events.push_back(prime_ev);
                NT_LOG("sublayer PRIME z=" << z_act << " tool=" << prime_tool
                    << " vol=" << prime_ev.wipe_volume);
            }

            // Inter-extruder transitions within this sublayer LayerTools.
            for (const unsigned int ext_id : lt.extruders) {
                if (ext_id == (unsigned int)sub_current_tool)
                    continue; // no change

                float wipe_vol = mp_prime_vol; // sublayer uses prime volume by default
                if (sub_current_tool < m_wipe_volumes.size() &&
                    (size_t)ext_id < m_wipe_volumes[sub_current_tool].size() &&
                    m_wipe_volumes[sub_current_tool][ext_id] > 0.f)
                    wipe_vol = m_wipe_volumes[sub_current_tool][ext_id];

                NeoTowerEvent ev;
                ev.z_nominal    = z_nom;
                ev.z_actual     = z_act;
                ev.layer_height = lh;
                ev.old_tool     = sub_current_tool;
                ev.new_tool     = (size_t)ext_id;
                ev.wipe_volume  = wipe_vol;
                ev.is_sublayer  = true;
                m_events.push_back(ev);
                NT_LOG("sublayer TRANS z=" << z_act
                    << " old=" << ev.old_tool << " new=" << ev.new_tool
                    << " vol=" << wipe_vol);

                sub_current_tool = (size_t)ext_id;
            }
        } // for lt
    } // sublayer second pass

    // ------------------------------------------------------------------
    // 1c. Structural layers — real layers with no toolchange events in
    //     prints with MultiPass/ColorMix sublayer activity.
    //
    //     KEY: we do NOT gate on lt.has_wipe_tower. In single-filament MP
    //     scenarios (test 1b) or mixed-object scenarios where only one
    //     object does MP (test 1a layers 25+), has_wipe_tower=0 for those
    //     real layers — yet GCode still calls is_empty_wipe_tower_gcode for
    //     them because WipeTower2 generated T→T entries for them via
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

            // Walk ToolOrdering sequentially, tracking current_tool.
            size_t struct_tool = tool_ordering.first_extruder();
            if (struct_tool == (size_t)-1)
                struct_tool = m_initial_tool;

            for (const LayerTools& lt : tool_ordering) {
                // Track tool through ALL layers (real + sublayer).
                for (unsigned ext_id : lt.extruders)
                    struct_tool = (size_t)ext_id;

                // Sublayers are not structural targets (they get prime events).
                // Layers with no extruders are degenerate — skip.
                if (lt.is_mp_sublayer || lt.extruders.empty())
                    continue;

                const float z  = static_cast<float>(lt.print_z);
                const float lh = static_cast<float>(lt.wipe_tower_layer_height > 0.
                                                     ? lt.wipe_tower_layer_height
                                                     : lt.layer_height);

                // Skip layers that already have toolchange events (e.g. real
                // T0↔T1 changes in 1a layers 0-24).
                if (event_zs.count(z_um64(z)) > 0)
                    continue;

                // Compute structural volume: 2 traversals across tower width.
                // This is the minimum to maintain physical tower integrity.
                float cfg_width = static_cast<float>(m_print_config->prime_tower_width);
                if (cfg_width < 10.f) cfg_width = 30.f;
                // Effective bead cross-section (WipeTower2 formula)
                float eff_width = m_perimeter_width
                                  - lh * (1.f - float(M_PI) / 4.f);
                if (eff_width < 0.01f) eff_width = m_perimeter_width * 0.8f;
                float struct_vol = 2.f * cfg_width * lh * eff_width;

                NeoTowerEvent ev;
                ev.z_nominal    = z;
                ev.z_actual     = z;
                ev.layer_height = lh;
                ev.old_tool     = struct_tool;
                ev.new_tool     = struct_tool; // T→T: structural, no real change
                ev.wipe_volume  = struct_vol;
                ev.is_sublayer  = false;
                m_events.push_back(ev);
                NT_LOG("structural event z=" << z
                    << " tool=" << struct_tool << " vol=" << struct_vol
                    << " (has_wt=" << lt.has_wipe_tower << ")");
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

    BOOST_LOG_TRIVIAL(debug) << "[NeoTower] collect_all_events: "
        << m_events.size() << " events total ("
        << std::count_if(m_events.begin(), m_events.end(),
               [](const NeoTowerEvent& e){ return e.is_sublayer; })
        << " sublayer)";
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
        BOOST_LOG_TRIVIAL(debug) << "[NeoTower] plan(): no events, tower is empty.";
        return;
    }

    // ------------------------------------------------------------------
    // Compute tower_width from max wipe volume across all events.
    // wipe_depth = max over all events of wipe_depth_for_volume(ev.wipe_volume, slot_h)
    // tower_width = wipe_depth + 2 * perimeter_width  (square → depth == width)
    // ------------------------------------------------------------------
    float max_depth = 0.f;
    for (size_t i = 0; i < m_events.size(); ++i) {
        const NeoTowerEvent& ev = m_events[i];
        float sh = ev.is_sublayer
                   ? sublayer_slot_height(ev.layer_height)
                   : ev.layer_height;
        // Use a provisional tower_width for the depth calculation.
        // We iterate once with a rough estimate, then finalize.
        // For the first pass use the config prime_tower_width as seed.
        float provisional_width = static_cast<float>(m_print_config->prime_tower_width);
        if (provisional_width < 10.f) provisional_width = 10.f;
        float d = wipe_depth_for_volume(ev.wipe_volume, sh);
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

    BOOST_LOG_TRIVIAL(debug) << "[NeoTower] plan(): " << m_plan.layers.size()
        << " layers, tower_width=" << m_plan.tower_width << "mm";
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
    // Filament area (mm²) = pi/4 * d²
    const float filament_area = static_cast<float>(M_PI) / 4.f *
                                m_filament_diameter * m_filament_diameter;

    // Effective tower width (usable for wipe, subtracting perimeters).
    const float usable_width = m_plan.tower_width > 0.f
                               ? m_plan.tower_width - 2.f * m_perimeter_width
                               : static_cast<float>(m_print_config->prime_tower_width)
                                 - 2.f * m_perimeter_width;
    if (usable_width <= 0.f) return 10.f; // fallback

    // Extrusion volume per mm of Y travel at this slot_height and line width.
    // vol_per_mm = slot_height * (perimeter_width - slot_height*(1 - pi/4)) * usable_width
    // Simplified to: vol_per_mm_of_depth = perimeter_width * slot_height * usable_width
    const float vol_per_mm = m_perimeter_width * slot_height * usable_width;
    if (vol_per_mm <= 0.f) return 10.f;

    // Volume carried by filament per mm of extrusion = filament_area.
    // wipe_volume (mm³ of filament) → depth in mm.
    const float depth = (wipe_volume / filament_area) / (vol_per_mm / filament_area);
    return std::max(2.f * m_perimeter_width, depth);
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

WipeTower::box_coordinates NeoTower::make_cleaning_box(const NeoTowerSlot& slot) const
{
    // Origin: perimeter_width/2 from left, perimeter_width/2 from bottom of slot.
    const float x0 = m_perimeter_width / 2.f;
    const float y0 = m_perimeter_width / 2.f;
    const float w  = m_plan.tower_width  - m_perimeter_width;
    const float h  = slot.slot_height    - m_perimeter_width;

    // WipeTower::box_coordinates(Vec2f bottom_left, float width, float height)
    return WipeTower::box_coordinates(Vec2f(x0, y0), w, std::max(0.1f, h));
}

WipeTower::ToolChangeResult NeoTower::generate_slot(WipeTower2&         wt2,
                                                     const NeoTowerSlot& slot,
                                                     float               layer_height,
                                                     bool                is_first_layer)
{
    // Configure WipeTower2 for this slot's Z and height.
    wt2.set_layer(
        slot.z,                  // print_z
        slot.slot_height,        // layer_height for extrusion flow
        1,                       // max_tool_changes (just this one)
        is_first_layer,          // unused in set_layer but kept for API
        false                    // not last layer
    );

    wt2.set_current_tool(slot.old_tool);

    const WipeTower::box_coordinates cleaning_box = make_cleaning_box(slot);

    return wt2.local_z_tool_change(slot.new_tool, cleaning_box, slot.wipe_volume);
}

void NeoTower::generate(std::vector<std::vector<WipeTower::ToolChangeResult>>& result)
{
    result.clear();
    m_result.clear();
    m_tcr_index.clear();
    m_finish_layer_index.clear();
    m_num_toolchanges = 0;

    if (m_plan.layers.empty()) return;

    // Construct one WipeTower2 instance, configured for the whole print.
    // It is used purely as a GCode writer — we control set_layer() and
    // set_current_tool() before every call to local_z_tool_change().
    WipeTower2 wt2(*m_print_config,
                   *m_region_config,
                   m_plate_idx,
                   m_plate_origin,
                   m_wipe_volumes,
                   m_initial_tool);

    // Register all extruders that appear in the print so WipeTower2's
    // m_filpar vector is fully populated before we call local_z_tool_change().
    // Collect unique tools from events.
    std::set<size_t> all_tools;
    all_tools.insert(m_initial_tool);
    for (const NeoTowerEvent& ev : m_events) {
        all_tools.insert(ev.old_tool);
        all_tools.insert(ev.new_tool);
    }
    for (size_t t : all_tools)
        wt2.set_extruder(t, *m_print_config);

    const float first_layer_z = m_plan.layers.empty()
                                 ? 0.f
                                 : m_plan.layers.front().z_nominal;

    m_result.resize(m_plan.layers.size());

    for (size_t li = 0; li < m_plan.layers.size(); ++li) {
        const NeoTowerLayer& layer = m_plan.layers[li];
        auto& layer_result = m_result[li];

        bool is_first = (layer.z_nominal <= first_layer_z + 1e-4f);

        for (size_t si = 0; si < layer.slots.size(); ++si) {
            const NeoTowerSlot& slot = layer.slots[si];

            WipeTower::ToolChangeResult tcr =
                generate_slot(wt2, slot, layer.layer_height, is_first && si == 0);

            NT_LOG("generate slot [" << li << "][" << si << "] z=" << slot.z
                << " old=" << slot.old_tool << " new=" << slot.new_tool
                << " gcode_bytes=" << tcr.gcode.size());

            // Build toolchange lookup key.
            uint64_t key = make_key(slot.z, slot.old_tool, slot.new_tool);
            m_tcr_index[key] = {li, si};

            // Structural-layer lookup: keyed by z only (T→T, !is_sublayer).
            // Used by GCode.cpp to dispatch finish_layer equivalent TCRs.
            const NeoTowerEvent& ev = m_events[slot.event_idx];
            if (!ev.is_sublayer && ev.old_tool == ev.new_tool) {
                uint64_t z_key = static_cast<uint64_t>(
                    std::llround(slot.z * 1000.f));
                m_finish_layer_index[z_key] = {li, si};
                NT_LOG("finish_layer index z=" << slot.z
                    << " tool=" << slot.old_tool
                    << " → [" << li << "][" << si << "]");
            }

            layer_result.push_back(std::move(tcr));
            ++m_num_toolchanges;
        }
    }

    // Collect used filament from WipeTower2.
    m_used_filament = wt2.get_used_filament();

    // Hand off to caller (WipeTowerData::tool_changes).
    result = m_result;

    BOOST_LOG_TRIVIAL(debug) << "[NeoTower] generate(): " << m_num_toolchanges
        << " TCRs emitted across " << m_plan.layers.size() << " layers.";
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
        BOOST_LOG_TRIVIAL(warning)
            << "[NeoTower] get_tcr() MISS z=" << z_actual
            << " old=" << old_tool << " new=" << new_tool;
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
        BOOST_LOG_TRIVIAL(warning)
            << "[NeoTower] get_finish_layer() MISS z=" << z;
        return std::nullopt;
    }
    const auto [li, si] = it->second;
    NT_LOG("get_finish_layer HIT z=" << z
        << " → [" << li << "][" << si << "]");
    return m_result[li][si];
}

} // namespace Slic3r

// NEOTKO_NEOTOWER_TAG_END
