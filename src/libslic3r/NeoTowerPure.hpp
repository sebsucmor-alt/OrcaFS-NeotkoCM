// NeoTowerPure.hpp — pure, side-effect-free helpers extracted from NeoTower.cpp.
// NEOTKO_NEOTOWER_TAG s205 (Fase 2) — testable scope.
//
// WHY THIS FILE EXISTS:
//   The NeoTower planner is a "second mirror calculation" of the toolchange
//   sequence that GCode.cpp re-derives at emission time (NEOTOWER.md §10). Its
//   whole class of historical bugs is one divergence or another between those two
//   calculations in some corner case. Fase 2 of the refactor plan
//   (docs/FUTURE/NEOTOWER_REFACTOR_PLAN.md) freezes today's *correct* behaviour as
//   deterministic Catch2 tests — the safety net that makes the later single-source
//   rewrite (Fase 5) safe.
//
//   Several of the pure decisions with the richest bug history (s158 purge volume,
//   s79f real-vs-sublayer promotion, s103 delta-Z height, Hallazgo VII key
//   collision) lived as `private` members or local lambdas inside NeoTower.cpp,
//   i.e. unreachable from a unit test. This header hoists exactly those pure kernels
//   to file scope so a test can call them directly. The NeoTower members now
//   DELEGATE to these functions — behaviour is byte-identical (no logic changed,
//   only relocated), so gcode is unaffected.
//
//   This header is header-only and depends only on NeoTowerZ.hpp (Z epsilons) and
//   NeoTower.hpp (the NeoTowerEvent struct, needed by dedup_events).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NeoTower.hpp"    // NeoTowerEvent, TowerEvent, LayerKind
#include "NeoTowerZ.hpp"   // Z epsilons (eff_layer_height)

namespace Slic3r {
namespace NeoTowerPure {

// ---------------------------------------------------------------------------
// make_key — quantize z to 1 µm, pack with tool IDs. Supports up to 99 tools.
// Mirror of NeoTower::make_key (which delegates here). See Hallazgo VII note in
// NeoTower.cpp: sub_z and its parent nominal quantize to the same z_um, so the
// same (z_um, old, new) key can legitimately exist in the real and sublayer
// channels pointing at different TCRs — the callers keep those in separate maps.
// ---------------------------------------------------------------------------
inline uint64_t make_key(float z_actual, size_t old_tool, size_t new_tool)
{
    uint64_t z_um = static_cast<uint64_t>(std::llround(z_actual * 1000.f));
    return z_um * 10000ULL + old_tool * 100ULL + new_tool;
}

// ---------------------------------------------------------------------------
// resolve_wipe_volume — NEOTKO_NEOTOWER_TAG s158 unified purge-volume resolver.
// One rule for every toolchange site (see NeoTower.cpp header comment for the
// full rationale):
//   body TC (sandwich_ctx=false)          → physical (matrix, scalar floor if OOB)
//   sandwich sublayer, old==new           → prime_floor (the knob)
//   sandwich sublayer, real colour change → max(prime_floor, physical)  ← the fix
// prime_floor doubles as the OOB scalar fallback so each caller reproduces its
// own pre-s158 result exactly outside the fixed case.
// ---------------------------------------------------------------------------
inline float resolve_wipe_volume(const std::vector<std::vector<float>>& wipe_volumes,
                                 int old_tool, int new_tool,
                                 bool sandwich_ctx, float prime_floor)
{
    float physical = prime_floor;   // scalar fallback when the flush matrix is empty / OOB
    if (old_tool >= 0 && (size_t)old_tool < wipe_volumes.size()
        && new_tool >= 0 && (size_t)new_tool < wipe_volumes[old_tool].size())
        physical = wipe_volumes[old_tool][new_tool];
    if (!sandwich_ctx)
        return physical;                        // body TC — byte-identical to pre-s158
    if (old_tool == new_tool)
        return prime_floor;                     // same-tool sublayer — unchanged (knob)
    return std::max(prime_floor, physical);     // real colour change in a sublayer — unified
}

// ---------------------------------------------------------------------------
// sublayer_slot_height — a sublayer slot gets at most 40% of the nominal layer
// height, bounded below by min_layer_height (itself floored to 0.04 mm at
// NeoTower construction).
// ---------------------------------------------------------------------------
inline float sublayer_slot_height(float min_layer_height, float nominal_layer_height)
{
    float h = std::min(min_layer_height, nominal_layer_height * 0.4f);
    return std::max(0.04f, h);
}

// ---------------------------------------------------------------------------
// eff_layer_height — NEOTKO_NEOTOWER_TAG s103 delta-Z height normalization.
// A staircase shell plane sitting only ~0.1 above the previous emitted tower
// plane must NOT carry the full nominal height (it would over-extrude 2× into a
// half-size gap). Rule: use (z − last_emitting_plane_z), floored to
// NOMINAL_LH_MIN, ONLY when that delta is strictly smaller than the nominal
// (minus plan epsilon). It never grows the height, so sparse-gap behaviour
// (delta > nominal) is unchanged. The caller keeps the diagnostic NT_LOG.
// ---------------------------------------------------------------------------
// NEOTKO_NEOTOWER_TAG s237 — BUG B: el suelo NOMINAL_LH_MIN se aplicaba SÓLO en la
// rama delta. La rama de retorno crudo dejaba pasar cualquier `nominal_h`, y con
// adaptive layer height llega envenenado: la rama de capa real de 1a
// (`NeoTower.cpp:595`) usa `lt.wipe_tower_layer_height`, que aguas arriba YA es un
// delta al plano de torre anterior. Observado en BIGTEST-ADAPTIVE:
// `real-layer event z=9.58902 lh=0.0133076` (= 9.58902 − 9.57571). Como
// `delta < nominal_h` es falso cuando el propio nominal ES el delta, se devolvía
// crudo y `DELTA_H` ni siquiera se logueaba → el TCR declaraba `;HEIGHT:0.0133076`
// (13 µm) mientras extruía lo de una capa normal.
//
// El suelo es de la MISMA constante y el MISMO ancla en las dos ramas: ninguna capa
// de torre puede declarar menos de lo que la máquina puede depositar. Es exactamente
// lo que V17 ya daba por legal en su cota (`max(_gap, NOMINAL_LH_MIN)`), y lo que sus
// hermanos del mismo plano ya recibían: en z=9.58902 los sublayers salían con
// h=0.04/0.0434/0.05 (suelo aplicado) y sólo el evento real se iba a 0.0133.
// No-op para toda capa sana (nominal ≥ 0.04). Ver docs/NEOTOWER.md §26.
inline float eff_layer_height(float z, float nominal_h, float last_emitting_plane_z)
{
    const float delta = z - last_emitting_plane_z;
    if (delta > NeoTowerZ::Z_EPS_PLAN && delta < nominal_h - NeoTowerZ::Z_EPS_PLAN)
        return std::max(delta, NeoTowerZ::NOMINAL_LH_MIN);
    return std::max(nominal_h, NeoTowerZ::NOMINAL_LH_MIN);
}

// ---------------------------------------------------------------------------
// mark_standalone_planes — NEOTKO_NEOTOWER_TAG s114, extracted + fixed in s236.
//
// A canonical layer whose z_nominal carries NO real (non-sublayer) event is
// realised entirely by MultiPass sublayers (PathBlend / ColorMix / mixed / any
// gradient shape) → it IS the layer, not a decoration of a real one. Its
// band-top sublayer must be treated as a structural plane (keep wall+grid,
// advance the emitting-plane tracker) instead of a frame-free same-plane
// lámina. Without this, a run of fully-painted layers left the tower no
// structural plane → frozen tracker → multi-layer gap → box-in-drawer Fase C
// flow-boost wall (whiskers, s112-s113).
//
// s236 FIX — exactly ONE plane per parent-less z_nominal.
//   The original filter marked every sublayer inside a SAME_PLANE_MAX_OFF
//   (0.02 mm) window below z_nominal. MultiPass puts its sublayers 0.0002-0.0012
//   below the nominal, so ALL of them fell inside the window and were marked
//   (observed: `marked 4` on a two-sublayer plane). Each one then advanced
//   last_emitting_plane_z, so the next sublayer 1 µm above it got
//   eff_layer_height() = max(0.001, NOMINAL_LH_MIN) = 0.04 instead of its true
//   height over the real plane below → the wipe needed ~5× the lines → tower
//   depth 51.6 mm where the same scene with any real event at that z produced
//   20.8 mm.
//   The band-top is the SINGLE highest z_actual of the group; the sublayers
//   under it stay láminas of the real plane below, which is what they physically
//   are. Ties (several toolchanges on the same physical sub-plane) are all
//   marked — they share one plane, not one each.
//
// Pure: reads and writes only `evts`. Tested in test_neotower.cpp.
// ---------------------------------------------------------------------------
inline size_t mark_standalone_planes(std::vector<NeoTowerEvent>& evts)
{
    auto zum = [](float z) -> uint64_t {
        return static_cast<uint64_t>(std::llround(static_cast<double>(z) * 1000.0));
    };

    // Every z_nominal that carries at least one real (non-sublayer) event.
    std::set<uint64_t> znom_with_real;
    for (const NeoTowerEvent& ev : evts)
        if (!ev.is_sublayer) znom_with_real.insert(zum(ev.z_nominal));

    // Per parent-less z_nominal, the highest z_actual still inside the
    // same-plane window. That single sub-plane is the canonical band-top.
    std::map<uint64_t, uint64_t> band_top; // z_nominal(µm) → z_actual(µm)
    for (const NeoTowerEvent& ev : evts) {
        if (!ev.is_sublayer) continue;
        const uint64_t zn = zum(ev.z_nominal);
        if (znom_with_real.count(zn)) continue;
        if (!(ev.z_nominal - ev.z_actual < NeoTowerZ::SAME_PLANE_MAX_OFF)) continue;
        const uint64_t za = zum(ev.z_actual);
        auto it = band_top.find(zn);
        if (it == band_top.end() || za > it->second) band_top[zn] = za;
    }

    size_t marked = 0;
    for (NeoTowerEvent& ev : evts) {
        if (!ev.is_sublayer) continue;
        auto it = band_top.find(zum(ev.z_nominal));
        if (it != band_top.end() && zum(ev.z_actual) == it->second) {
            ev.standalone_plane = true;
            ++marked;
        }
    }
    return marked;
}

// ---------------------------------------------------------------------------
// dedup_events — collapse exact-key duplicates (same quantized z + old + new).
// Assumes evts is already sorted by (z_actual, old_tool, new_tool).
//
// NEOTKO_NEOTOWER_TAG s79f — bug03 fix: when a real event collides with a
// sublayer at the same quantized z, PREFER the real (full layer_height + matrix
// flush volume); the sublayer's smaller purge is absorbed by max(). When both
// are the same kind (two objects with the identical pass pair at one Z) the
// original max-volume behaviour is preserved. max(), not += : the tower purges
// once with enough volume for the worst case, not once per object.
// ---------------------------------------------------------------------------
inline void dedup_events(std::vector<NeoTowerEvent>& evts)
{
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
                if (last.is_sublayer && !ev.is_sublayer) {
                    const float merged_vol = std::max(last.wipe_volume, ev.wipe_volume);
                    last             = ev;             // promote to real
                    last.wipe_volume = merged_vol;
                } else {
                    last.wipe_volume = std::max(last.wipe_volume, ev.wipe_volume);
                }
                continue;
            }
        }
        deduped.push_back(ev);
    }
    evts = std::move(deduped);
}

// ---------------------------------------------------------------------------
// validate_emission_bijection — NEOTKO_NEOTOWER_TAG s205-5b.1 — V18.
// Verify m_emission_order is a faithful bijection of the four EMITTABLE lookup
// maps (m_z_redirect / m_z_redirect_finish are pure key→key aliases, not TCRs,
// excluded by design). Two directions:
//   forward — every TowerEvent resolves in its own channel to the SAME target.
//   counts  — per-channel entry counts equal the map sizes (catches orphan map
//             entries and shadowed/duplicate list entries, e.g. a key collision).
// Real and Bridge both register into tcr_index, so their combined count must
// equal tcr_index.size(). Returns human-readable violations; empty = healthy
// (V18 stays silent). Pure: reads only its arguments. Tested in test_neotower.cpp.
// ---------------------------------------------------------------------------
inline std::vector<std::string> validate_emission_bijection(
    const std::vector<TowerEvent>&                                 emission_order,
    const std::unordered_map<uint64_t, std::pair<size_t, size_t>>& tcr_index,
    const std::unordered_map<uint64_t, std::pair<size_t, size_t>>& tcr_index_sub,
    const std::unordered_map<uint64_t, std::pair<size_t, size_t>>& finish_layer_index,
    const std::unordered_map<uint64_t, size_t>&                    merged_index)
{
    std::vector<std::string> viol;
    size_t n_real = 0, n_sub = 0, n_struct = 0, n_bridge = 0, n_merged = 0;

    auto check_slot = [&](const char* ch, const TowerEvent& te,
                          const std::unordered_map<uint64_t, std::pair<size_t, size_t>>& m) {
        auto it = m.find(te.key);
        if (it == m.end())
            viol.push_back(std::string("V18: ") + ch + " key not in map key=" + std::to_string(te.key));
        else if (it->second.first != te.li || it->second.second != te.si)
            viol.push_back(std::string("V18: ") + ch + " target mismatch key=" + std::to_string(te.key)
                           + " list=[" + std::to_string(te.li) + "][" + std::to_string(te.si) + "]"
                           + " map=[" + std::to_string(it->second.first) + "][" + std::to_string(it->second.second) + "]");
    };

    for (const TowerEvent& te : emission_order) {
        switch (te.kind) {
        case LayerKind::Real:         ++n_real;   check_slot("Real", te, tcr_index);              break;
        case LayerKind::Sublayer:     ++n_sub;    check_slot("Sublayer", te, tcr_index_sub);      break;
        case LayerKind::Structural:   ++n_struct; check_slot("Structural", te, finish_layer_index); break;
        case LayerKind::Bridge:       ++n_bridge; check_slot("Bridge", te, tcr_index);            break;
        case LayerKind::BridgeMerged: {
            ++n_merged;
            auto it = merged_index.find(te.key);
            if (it == merged_index.end())
                viol.push_back("V18: BridgeMerged key not in merged_index key=" + std::to_string(te.key));
            else if (it->second != te.li)
                viol.push_back("V18: BridgeMerged idx mismatch key=" + std::to_string(te.key)
                               + " list=" + std::to_string(te.li) + " map=" + std::to_string(it->second));
            break;
        }
        }
    }

    if (n_real + n_bridge != tcr_index.size())
        viol.push_back("V18: count mismatch tcr_index list(Real+Bridge)=" + std::to_string(n_real + n_bridge)
                       + " map=" + std::to_string(tcr_index.size()));
    if (n_sub != tcr_index_sub.size())
        viol.push_back("V18: count mismatch tcr_index_sub list=" + std::to_string(n_sub)
                       + " map=" + std::to_string(tcr_index_sub.size()));
    if (n_struct != finish_layer_index.size())
        viol.push_back("V18: count mismatch finish_layer_index list=" + std::to_string(n_struct)
                       + " map=" + std::to_string(finish_layer_index.size()));
    if (n_merged != merged_index.size())
        viol.push_back("V18: count mismatch merged_index list=" + std::to_string(n_merged)
                       + " map=" + std::to_string(merged_index.size()));

    return viol;
}

// ---------------------------------------------------------------------------
// validate_shadow_consumption — NEOTKO_NEOTOWER_TAG s205-5b.2.
// Runtime counterpart of V18: given the canonical emission list and the ORDERED
// slots GCode actually emitted (shadow_sequence), verify each emitted slot maps to
// exactly one canonical entry and each canonical entry was emitted exactly once.
// Splits findings into `violations` (real divergence → WARN) and `census`
// (expected/benign → info log). Notably a standalone Bridge entry that was folded
// into its merged TCR is emitted 0× BY DESIGN → census, never a violation.
// Pure: reads only its arguments. Tested in test_neotower.cpp.
// ---------------------------------------------------------------------------
struct ShadowReport {
    std::vector<std::string> violations;  // real divergence → NT_INVARIANT_WARN
    std::vector<std::string> census;      // expected/benign → NT_LOG
    size_t                   emitted = 0; // total shadow hits seen
};

inline ShadowReport validate_shadow_consumption(
    const std::vector<TowerEvent>&  emission_order,
    const std::vector<ShadowSlot>&  shadow_sequence)
{
    ShadowReport rep;
    rep.emitted = shadow_sequence.size();

    // Slot identity = (from_finish, merged, a, b). from_finish separates the
    // finish channel (Structural) from the tcr channel at a shared m_result slot.
    using Slot = std::tuple<bool, bool, size_t, size_t>;
    auto slot_of_event = [](const TowerEvent& te) -> Slot {
        const bool merged      = (te.kind == LayerKind::BridgeMerged);
        const bool from_finish = (te.kind == LayerKind::Structural);
        return Slot{from_finish, merged, te.li, merged ? size_t(0) : te.si};
    };

    std::map<Slot, size_t>  slot_to_idx;               // canonical slot → emission idx
    std::vector<size_t>     consumed(emission_order.size(), 0);

    for (size_t i = 0; i < emission_order.size(); ++i) {
        auto ins = slot_to_idx.emplace(slot_of_event(emission_order[i]), i);
        if (!ins.second)
            rep.census.push_back("SHADOW: two canonical entries alias one slot (idx "
                + std::to_string(ins.first->second) + " & " + std::to_string(i) + ")");
    }

    for (const ShadowSlot& s : shadow_sequence) {
        auto it = slot_to_idx.find(Slot{s.from_finish, s.merged, s.a, s.merged ? size_t(0) : s.b});
        if (it == slot_to_idx.end())
            rep.violations.push_back("SHADOW: emitted a slot with no canonical entry (from_finish="
                + std::to_string(s.from_finish) + " merged=" + std::to_string(s.merged)
                + " a=" + std::to_string(s.a) + " b=" + std::to_string(s.b) + ")");
        else
            ++consumed[it->second];
    }

    // NEOTKO_NEOTOWER_TAG s205-5b.2c — key-collision coverage. A (channel, key) can hold
    // TWO canonical entries pointing at different m_result slots: a real sublayer chain TC
    // and a synthetic cross-product TC seeded for the same (z, old, new) transition. Only
    // one wins the lookup map (m_tcr_index_sub[key]); GCode resolves get_tcr BY KEY so it
    // emits that one, leaving the twin at 0×. Both encode the SAME transition → the twin is
    // benign. Channel = which lookup map the entry lives in (Real+Bridge share m_tcr_index).
    auto channel_of = [](LayerKind k) -> int {
        switch (k) {
            case LayerKind::Sublayer:     return 1;
            case LayerKind::Structural:   return 2;
            case LayerKind::BridgeMerged: return 3;
            default:                      return 0; // Real + Bridge → m_tcr_index
        }
    };
    std::map<std::pair<int, uint64_t>, size_t> key_consumed; // (channel,key) → total emitted
    for (size_t i = 0; i < emission_order.size(); ++i)
        key_consumed[{channel_of(emission_order[i].kind), emission_order[i].key}] += consumed[i];

    for (size_t i = 0; i < emission_order.size(); ++i) {
        const TowerEvent& te = emission_order[i];
        if (consumed[i] > 1) {
            rep.violations.push_back("SHADOW: canonical entry emitted " + std::to_string(consumed[i])
                + "× (double-emit) kind=" + std::to_string(int(te.kind)) + " key=" + std::to_string(te.key));
        } else if (consumed[i] == 0) {
            if (te.kind == LayerKind::Bridge)
                rep.census.push_back("SHADOW: standalone Bridge emitted 0× (folded into merged) key="
                    + std::to_string(te.key));                       // expected — not a violation
            // NEOTKO_NEOTOWER_TAG s240 — un STRUCTURAL a 0× NO es un repuesto especulativo.
            //
            // El resto de entradas especulativas son SUSTITUIBLES: una TC sintética del
            // producto cruzado que no se usa significa que la agrupación eligió otra cadena
            // que hace la MISMA transición, así que el material se deposita igual. Un
            // finish estructural no tiene sustituto: es el único relleno de la torre en su
            // banda de Z. Si no se emite, ahí queda aire — no hay otra entrada que lo tape.
            //
            // Marcarlo como `speculative` (NeoTower.cpp Fase 3, donde el flag se hereda de
            // `ev.is_sublayer`) lo enterraba entre los repuestos benignos. Medido en BIGTEST
            // (s240): las 5 líneas kind=2 de este censo eran los 5 huecos de Z reales de la
            // torre, y las otras 33 eran ruido legítimo. El detector acertó desde el
            // principio; lo que falló fue la clasificación. Es la misma enfermedad que
            // `_na_synth_any` en §28.5: la bandera que apaga el arreglo apagaba el detector.
            //
            // No se promociona a `violations` aquí porque hay un caso legítimo (§27): una
            // lámina y su canónica comparten banda de Z repartiéndose la Y, así que el
            // finish de la lámina puede no emitirse sin dejar hueco. Quien juzga eso es V23,
            // que mide la cobertura real de lo EMITIDO. Esta línea le da a V23 los
            // sospechosos con nombre y apellidos, separados del ruido.
            else if (te.speculative && te.kind == LayerKind::Structural)
                rep.census.push_back("SHADOW: STRUCTURAL_NEVER_EMITTED — relleno estructural sin sustituto"
                    " (candidato a hueco de Z; V23 dictamina) z=" + std::to_string(te.z_actual)
                    + " li=" + std::to_string(te.li) + " key=" + std::to_string(te.key));
            else if (te.speculative)
                // NEOTKO_NEOTOWER_TAG s205-5b.2c — speculative spare (synthetic cross-product
                // TC, or folded BridgeMerged): the plan seeds it but the same-colour grouping
                // decided at emission may legitimately pick a different entry chain → 0× is
                // expected. Census, not a phantom violation. A NON-speculative entry at 0× is
                // still a real violation (the safety net holds).
                rep.census.push_back("SHADOW: speculative spare emitted 0× (grouping chose another path) kind="
                    + std::to_string(int(te.kind)) + " key=" + std::to_string(te.key));
            else if (key_consumed[{channel_of(te.kind), te.key}] > 0)
                // NEOTKO_NEOTOWER_TAG s205-5b.2c — the transition (channel,key) WAS emitted
                // through a colliding twin slot (real↔synthetic same (z,old,new)); this slot
                // lost the lookup-map race but its twin covered the toolchange. Benign census.
                rep.census.push_back("SHADOW: entry 0× but its (channel,key) was emitted via a colliding twin kind="
                    + std::to_string(int(te.kind)) + " key=" + std::to_string(te.key));
            else
                rep.violations.push_back("SHADOW: canonical entry never emitted (phantom) kind="
                    + std::to_string(int(te.kind)) + " key=" + std::to_string(te.key));
        }
    }

    return rep;
}

} // namespace NeoTowerPure
} // namespace Slic3r
