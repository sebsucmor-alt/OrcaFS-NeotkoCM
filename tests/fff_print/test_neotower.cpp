// test_neotower.cpp — deterministic regression tests for the NeoTower planner.
//
// NEOTKO_NEOTOWER_TAG s205 (Fase 2 of docs/FUTURE/NEOTOWER_REFACTOR_PLAN.md).
//
// NeoTower is a "second, mirror calculation" of the toolchange sequence that
// GCode.cpp re-derives at emission time (NEOTOWER.md §10 invariant: plan ≡
// emission ≡ TCR). Its whole class of historical bugs (s79f, s88, s89, s102,
// s148, s158) is one divergence or another between those two calculations in a
// corner case. These tests are DETERMINISTIC (fixed input → known output): they
// freeze today's verified-correct behaviour as a non-regression net, so the later
// single-source rewrite (Fase 5) is safe. They are NOT a fuzzer (deferred; see the
// plan). Each TEST_CASE cites the session whose bug it pins.
//
// Everything under test here is PURE (no Print, no slicing): the header-only
// MultiPassScheduler / NeoTowerZ, plus the kernels hoisted to NeoTowerPure.hpp.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>   // s237 — std::pair en los tests de BUG A
#include <vector>

#include "libslic3r/MultiPassScheduler.hpp"
#include "libslic3r/NeoTowerZ.hpp"
#include "libslic3r/NeoTowerPure.hpp"   // pulls NeoTower.hpp (NeoTowerEvent)

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// ===========================================================================
// MultiPassScheduler — the ONE shared function (plan side + emission side call
// it identically, so plan == emission by construction). s79/s88/s89/s102/s148.
// ===========================================================================

namespace {

using MultiPassScheduler::SublayerKey;

// Build one sublayer entry.
SublayerKey sk(uint64_t chain, int pass, int tool, double z, bool atomic = false)
{
    SublayerKey s;
    s.chain_key    = chain;
    s.pass_idx     = pass;
    s.tool_id      = tool;
    s.z_actual     = z;
    s.atomic_chain = atomic;
    return s;
}

// Number of toolchanges in the emitted tool sequence for a given order.
int count_toolchanges(const std::vector<SublayerKey>& items,
                      const std::vector<size_t>&      order,
                      int                             initial_tool)
{
    int tc = 0, cur = initial_tool;
    for (size_t idx : order) {
        if (items[idx].tool_id != cur) { ++tc; cur = items[idx].tool_id; }
    }
    return tc;
}

// True iff `order` is a permutation of [0, items.size()).
bool is_permutation_of_indices(const std::vector<SublayerKey>& items,
                               const std::vector<size_t>&      order)
{
    if (order.size() != items.size()) return false;
    std::vector<char> seen(items.size(), 0);
    for (size_t idx : order) {
        if (idx >= items.size() || seen[idx]) return false;
        seen[idx] = 1;
    }
    return true;
}

// True iff, for every chain, pass_idx values appear in ascending (causal) order.
bool causal_order_respected(const std::vector<SublayerKey>& items,
                            const std::vector<size_t>&      order)
{
    std::vector<uint64_t> keys;
    for (const auto& it : items)
        if (std::find(keys.begin(), keys.end(), it.chain_key) == keys.end())
            keys.push_back(it.chain_key);
    for (uint64_t key : keys) {
        int last_pass = -1;
        for (size_t idx : order) {
            if (items[idx].chain_key != key) continue;
            if (items[idx].pass_idx < last_pass) return false;
            last_pass = items[idx].pass_idx;
        }
    }
    return true;
}

// Position of a (chain_key, pass_idx) item within `order`.
size_t pos_of(const std::vector<SublayerKey>& items,
              const std::vector<size_t>&      order,
              uint64_t chain, int pass)
{
    for (size_t p = 0; p < order.size(); ++p) {
        const auto& it = items[order[p]];
        if (it.chain_key == chain && it.pass_idx == pass) return p;
    }
    return order.size(); // not found
}

} // namespace

TEST_CASE("MultiPassScheduler: empty input yields empty order", "[NeoTower][MPScheduler]")
{
    std::vector<SublayerKey> items;
    REQUIRE(MultiPassScheduler::order_sublayers_by_tool(items, 0).empty());
    REQUIRE(MultiPassScheduler::order_sublayers_by_tool_windowed(items, 0, 1e-4).empty());
}

TEST_CASE("MultiPassScheduler: output is a complete deterministic permutation",
          "[NeoTower][MPScheduler]")
{
    // Two objects, inverted tools per pass (s148 shape).
    std::vector<SublayerKey> items = {
        sk(/*A*/1, 0, 0, 1.00), sk(1, 1, 1, 1.00),
        sk(/*B*/2, 0, 1, 1.00), sk(2, 1, 0, 1.00),
    };
    const auto o1 = MultiPassScheduler::order_sublayers_by_tool(items, 0);
    const auto o2 = MultiPassScheduler::order_sublayers_by_tool(items, 0);

    REQUIRE(is_permutation_of_indices(items, o1));
    REQUIRE(o1 == o2);                       // deterministic
    REQUIRE(causal_order_respected(items, o1));
}

TEST_CASE("MultiPassScheduler: keeps the current tool when it has ready work",
          "[NeoTower][MPScheduler]")
{
    // First emitted item must belong to initial_tool if any ready item uses it
    // (zero-cost toolchange preference).
    std::vector<SublayerKey> items = {
        sk(1, 0, 1, 1.00),  // tool 1
        sk(2, 0, 0, 1.00),  // tool 0  ← initial_tool
    };
    const auto order = MultiPassScheduler::order_sublayers_by_tool(items, 0);
    REQUIRE(items[order.front()].tool_id == 0);
}

TEST_CASE("MultiPassScheduler: groups same-tool work behind one toolchange (s89/s148)",
          "[NeoTower][MPScheduler]")
{
    // Three single-pass chains, tools T0,T1,T0. Naive object-major order
    // (A,B,C) costs 2 toolchanges (T0→T1→T0); the scheduler groups the two T0
    // together for exactly ONE toolchange.
    std::vector<SublayerKey> items = {
        sk(1, 0, 0, 1.00),  // A  T0
        sk(2, 0, 1, 1.00),  // B  T1
        sk(3, 0, 0, 1.00),  // C  T0
    };
    const auto order = MultiPassScheduler::order_sublayers_by_tool(items, 0);

    REQUIRE(count_toolchanges(items, order, 0) == 1);
    // The lone T1 must be emitted last (both T0 grouped first).
    REQUIRE(items[order.back()].tool_id == 1);
}

TEST_CASE("MultiPassScheduler: atomic chain drains consecutive same-tool passes (s88)",
          "[NeoTower][MPScheduler]")
{
    // Chain A has two stacked T0 passes locked in pass order (p1 not ready until
    // p0 emits) plus a T1 tapa; chain B has a single T0. With atomic_chain, A's
    // rampa (p0,p1) drains fully before B's T0 — no cross-object micro-travel.
    auto make = [](bool atomic) {
        return std::vector<SublayerKey>{
            sk(/*A*/1, 0, 0, 1.00, atomic),
            sk(/*A*/1, 1, 0, 1.01, atomic),
            sk(/*A*/1, 2, 1, 1.02, atomic),
            sk(/*B*/2, 0, 0, 1.05, atomic),
        };
    };

    SECTION("atomic: A.p0 and A.p1 are contiguous") {
        auto items = make(true);
        const auto order = MultiPassScheduler::order_sublayers_by_tool(items, 0);
        REQUIRE(is_permutation_of_indices(items, order));
        const size_t a0 = pos_of(items, order, 1, 0);
        const size_t a1 = pos_of(items, order, 1, 1);
        REQUIRE(a1 == a0 + 1);              // drained back-to-back
    }

    SECTION("non-atomic: B.p0 slips between A.p0 and A.p1") {
        auto items = make(false);
        const auto order = MultiPassScheduler::order_sublayers_by_tool(items, 0);
        const size_t a0 = pos_of(items, order, 1, 0);
        const size_t a1 = pos_of(items, order, 1, 1);
        const size_t b0 = pos_of(items, order, 2, 0);
        REQUIRE(a0 < b0);
        REQUIRE(b0 < a1);                   // interleaved when not atomic
    }
}

TEST_CASE("MultiPassScheduler: single window degenerates to the global order (s102)",
          "[NeoTower][MPScheduler]")
{
    // All z within window_eps → one window → windowed == global (bit-for-bit,
    // preserving s88/s89 semantics for PathBlend scanline chains 1e-7 apart).
    std::vector<SublayerKey> items = {
        sk(1, 0, 0, 1.0000000), sk(1, 1, 1, 1.0000001),
        sk(2, 0, 1, 1.0000002), sk(2, 1, 0, 1.0000003),
    };
    const auto global   = MultiPassScheduler::order_sublayers_by_tool(items, 0);
    const auto windowed = MultiPassScheduler::order_sublayers_by_tool_windowed(items, 0, 1e-4);
    REQUIRE(windowed == global);
}

TEST_CASE("MultiPassScheduler: windows chain the running tool across planes (s102)",
          "[NeoTower][MPScheduler]")
{
    // Two classic MP planes ~0.001 apart → two separate windows. The exit tool of
    // window 1 must carry into window 2 (the writer carries over between
    // process_layer calls) — the s102 fix that stopped predicting the wrong
    // real-layer rotation.
    std::vector<SublayerKey> items = {
        sk(1, 0, 0, 1.000),   // window 1: T0
        sk(1, 1, 1, 1.001),   // window 2: T1  (chain-locked behind p0)
    };
    const auto order = MultiPassScheduler::order_sublayers_by_tool_windowed(items, 0, 1e-4);

    REQUIRE(is_permutation_of_indices(items, order));
    REQUIRE(causal_order_respected(items, order));
    // p0 (z=1.000) before p1 (z=1.001): windows are ascending in z.
    REQUIRE(items[order.front()].tool_id == 0);
    REQUIRE(items[order.back()].tool_id == 1);
}

// ===========================================================================
// NeoTowerZ — single source of truth for Z epsilons + quantization.
// ===========================================================================

TEST_CASE("NeoTowerZ: epsilon ordering invariants hold", "[NeoTower][NeoTowerZ]")
{
    // These mirror the header static_asserts; re-stated so a future edit that
    // loosens one shows up as a test failure, not only a compile break.
    STATIC_REQUIRE(NeoTowerZ::NOMINAL_LH_MIN  > NeoTowerZ::SUBLAYER_GAP);
    STATIC_REQUIRE(NeoTowerZ::SUBLAYER_GAP    > NeoTowerZ::Z_EPS_PLAN);
    STATIC_REQUIRE(NeoTowerZ::Z_EPS_PLAN      > NeoTowerZ::Z_EPS_GROUP);
    STATIC_REQUIRE(NeoTowerZ::Z_EPS_FUTURE_TC < NeoTowerZ::NOMINAL_LH_MIN);
    STATIC_REQUIRE(NeoTowerZ::Z_EPS_FUTURE_TC > NeoTowerZ::SUBLAYER_GAP);
    STATIC_REQUIRE(NeoTowerZ::SAME_PLANE_MAX_OFF < NeoTowerZ::NOMINAL_LH_MIN);
    STATIC_REQUIRE(NeoTowerZ::SAME_PLANE_MAX_OFF > 5.f * NeoTowerZ::SUBLAYER_GAP);
}

TEST_CASE("NeoTowerZ: quantization rounds to micron / nanometre keys", "[NeoTower][NeoTowerZ]")
{
    REQUIRE(NeoTowerZ::to_key_um(0.880f) == 880);
    REQUIRE(NeoTowerZ::to_key_um(0.8798f) == 880);   // rounds up
    REQUIRE(NeoTowerZ::to_key_um(0.0004f) == 0);     // rounds down
    REQUIRE(NeoTowerZ::to_nm(0.001f)      == 1000);
}

TEST_CASE("NeoTowerZ: lamina vs staircase classification threshold (s102-h)",
          "[NeoTower][NeoTowerZ]")
{
    // Lámina: same physical plane as the real layer, small offset → skip frame.
    const float lamina_off    = 0.0022f;  // largest observed stacked-lamina offset
    // Staircase: distinct plane in the gap, smallest offset == NOMINAL_LH_MIN.
    const float staircase_off = NeoTowerZ::NOMINAL_LH_MIN;

    REQUIRE(lamina_off    <  NeoTowerZ::SAME_PLANE_MAX_OFF);
    REQUIRE(staircase_off >= NeoTowerZ::SAME_PLANE_MAX_OFF);
}

// ===========================================================================
// NeoTowerPure::make_key — Hallazgo VII (sublayer/real Z collision).
// ===========================================================================

TEST_CASE("NeoTowerPure::make_key packs z and tools", "[NeoTower][Pure]")
{
    // key = round(z*1000) * 10000 + old*100 + new.
    REQUIRE(NeoTowerPure::make_key(1.0f, 2, 3) == 1000ULL * 10000ULL + 2 * 100 + 3);
    REQUIRE(NeoTowerPure::make_key(0.0f, 0, 0) == 0);
}

TEST_CASE("NeoTowerPure::make_key: sublayer and real Z collide by design (Hallazgo VII)",
          "[NeoTower][Pure]")
{
    // sub z=0.7998 and nominal z=0.8 both quantize to 800 µm, so a sub TC and a
    // real TC with the same (old,new) produce IDENTICAL keys — which is exactly
    // why the callers keep them in separate maps (m_tcr_index vs m_tcr_index_sub).
    REQUIRE(NeoTowerPure::make_key(0.7998f, 0, 1) == NeoTowerPure::make_key(0.8f, 0, 1));
    // Distinct (old,new) at the same z stay distinct.
    REQUIRE(NeoTowerPure::make_key(0.8f, 0, 1) != NeoTowerPure::make_key(0.8f, 1, 0));
}

// ===========================================================================
// NeoTowerPure::resolve_wipe_volume — s158 unified purge volume, 3 branches.
// ===========================================================================

TEST_CASE("NeoTowerPure::resolve_wipe_volume: three branches (s158)", "[NeoTower][Pure]")
{
    // matrix[old][new] mm³.
    const std::vector<std::vector<float>> matrix = {{0.f, 10.f}, {20.f, 0.f}};

    SECTION("body TC uses the physical flush matrix (byte-identical to pre-s158)") {
        REQUIRE_THAT(NeoTowerPure::resolve_wipe_volume(matrix, 0, 1, /*sandwich*/false, 5.f),
                     WithinAbs(10.f, 1e-6));
    }
    SECTION("body TC out of matrix bounds falls back to the scalar floor") {
        REQUIRE_THAT(NeoTowerPure::resolve_wipe_volume(matrix, 0, 5, false, 5.f),
                     WithinAbs(5.f, 1e-6));
    }
    SECTION("sandwich same-tool sublayer keeps the prime-volume knob") {
        // matrix[1][1] == 0, but same-tool sublayers reserve the knob unchanged.
        REQUIRE_THAT(NeoTowerPure::resolve_wipe_volume(matrix, 1, 1, /*sandwich*/true, 7.f),
                     WithinAbs(7.f, 1e-6));
    }
    SECTION("sandwich real colour change unifies to max(knob, physical) — the fix") {
        // Knob below physical → physical wins (the under-purge fix).
        REQUIRE_THAT(NeoTowerPure::resolve_wipe_volume(matrix, 0, 1, true, 3.f),
                     WithinAbs(10.f, 1e-6));
        // Knob above physical → knob wins.
        REQUIRE_THAT(NeoTowerPure::resolve_wipe_volume(matrix, 1, 0, true, 30.f),
                     WithinAbs(30.f, 1e-6));
    }
}

// ===========================================================================
// s237 — BUG A: el ORDEN DE ENTRADA es carga estructural, no cosmética.
//
// Cuando un z_nominal tiene varios sublayers del MISMO objeto (el caso normal de
// N buckets ColorMix: mismo chain_key, mismo pass_idx, misma z_actual), el
// scheduler no tiene NINGUNA decisión que tomar: una sola cadena, todo empatado.
// Devuelve el orden de entrada tal cual. Es decir: **quien fija el orden de
// entrada fija el plan de la torre.**
//
// En s237 eso saltó porque el `std::sort` de `surf_events` (NeoTower.cpp:1107)
// NO es estable y empataba en sus tres campos → barajaba esos tríos → el plan
// encadenaba 3→0, 0→1, 1→2 mientras la emisión hacía 3→2, 2→1, 1→0 → el par real
// no se sembraba en ningún slot → get_tcr MISS → **cambio de color SIN purga**
// (contaminación de color, confirmada en visor en BIGTEST-ADAPTIVE).
//
// Estos tests fijan las dos mitades: que el orden de entrada manda (por eso hay
// que anclarlo), y el escenario exacto que falló. Ver NEOTOWER.md §25.
// ===========================================================================

namespace {

// Cadena de pares (old,new) que CANON_SCHED emitiría para un orden dado:
// batches de tool consecutivo, y sólo los cambios REALES de herramienta.
// Espejo de NeoTower.cpp:1220-1313.
std::vector<std::pair<int, int>> chain_pairs(const std::vector<SublayerKey>& items,
                                             const std::vector<size_t>&      order,
                                             int                             initial_tool)
{
    std::vector<std::pair<int, int>> pairs;
    int running = initial_tool;
    for (size_t idx : order) {
        const int tool = items[idx].tool_id;
        if (tool == running) continue;      // batch del mismo tool → sin TC real
        pairs.emplace_back(running, tool);
        running = tool;
    }
    return pairs;
}

} // namespace

TEST_CASE("MultiPassScheduler: fully tied items echo the INPUT order (s237 BUG A)",
          "[NeoTower][MPScheduler]")
{
    // Un objeto, un plano, 3 buckets ColorMix: mismo chain_key, mismo pass_idx,
    // misma z. Empate total → el scheduler no decide nada.
    const uint64_t chain = 42;
    std::vector<SublayerKey> items{
        sk(chain, 1, 2, 15.8498),
        sk(chain, 1, 1, 15.8498),
        sk(chain, 1, 0, 15.8498),
    };
    const auto order = MultiPassScheduler::order_sublayers_by_tool_windowed(
        items, /*initial_tool*/3, NeoTowerZ::Z_EPS_PLAN);

    REQUIRE(is_permutation_of_indices(items, order));
    // La propiedad que importa: sale lo que entró, en el mismo orden.
    REQUIRE(order == std::vector<size_t>{0, 1, 2});

    // Y con la entrada invertida sale invertido — o sea, el orden de entrada
    // DECIDE. Por eso el sort de surf_events necesita un desempate explícito.
    std::vector<SublayerKey> reversed{items[2], items[1], items[0]};
    const auto order_rev = MultiPassScheduler::order_sublayers_by_tool_windowed(
        reversed, /*initial_tool*/3, NeoTowerZ::Z_EPS_PLAN);
    REQUIRE(reversed[order_rev[0]].tool_id == 0);
    REQUIRE(reversed[order_rev[2]].tool_id == 2);
}

TEST_CASE("MultiPassScheduler: BIGTEST-ADAPTIVE z=15.85 plans the pair emission asks for (s237)",
          "[NeoTower][MPScheduler]")
{
    // Escena real: T3 entra arrastrado del sub-plano inferior (z=15.8488, ventana
    // propia), y el plano de arriba tiene los 3 buckets empatados. Entrando en T1.
    const uint64_t chain = 37225600060465230ull;
    const int      enter = 1;

    // Orden CANÓNICO — el de PrintObject::multipass_sublayers(), que es el que usa
    // la emisión (y el espejo 1a). Es el que el fix de s237 preserva.
    std::vector<SublayerKey> canonical{
        sk(chain, 0, 3, 15.8488),
        sk(chain, 1, 2, 15.8498),
        sk(chain, 1, 1, 15.8498),
        sk(chain, 1, 0, 15.8498),
    };
    const auto order = MultiPassScheduler::order_sublayers_by_tool_windowed(
        canonical, enter, NeoTowerZ::Z_EPS_PLAN);
    const auto pairs = chain_pairs(canonical, order, enter);

    // Exactamente lo que emite el gcode verificado: 1→3, 3→2, 2→1, 1→0.
    REQUIRE(pairs == std::vector<std::pair<int, int>>{{1, 3}, {3, 2}, {2, 1}, {1, 0}});

    // El par que la emisión pide y que se perdía. Éste es EL bug de s237.
    REQUIRE(std::find(pairs.begin(), pairs.end(), std::make_pair(3, 2)) != pairs.end());

    // Contraprueba: con los 3 empatados barajados (lo que hacía el std::sort
    // inestable), `3→2` DESAPARECE del plan. Si este REQUIRE empieza a fallar es
    // que alguien quitó el desempate y el bug ha vuelto.
    std::vector<SublayerKey> shuffled{
        canonical[0], canonical[3], canonical[2], canonical[1],
    };
    const auto order_s = MultiPassScheduler::order_sublayers_by_tool_windowed(
        shuffled, enter, NeoTowerZ::Z_EPS_PLAN);
    const auto pairs_s = chain_pairs(shuffled, order_s, enter);
    REQUIRE(pairs_s != pairs);
    REQUIRE(std::find(pairs_s.begin(), pairs_s.end(), std::make_pair(3, 2)) == pairs_s.end());
}

TEST_CASE("MultiPassScheduler: two chains are decided by chain logic, not by the tie-break (s237)",
          "[NeoTower][MPScheduler]")
{
    // z=14.4498 era el plano que acertaba POR CASUALIDAD: al haber dos objetos
    // (dos chain_key) manda el round-robin de cadenas y el desempate deja de ser
    // decisivo. Fijado para que quede claro por qué ese plano no reproducía el bug.
    // FIXTURE REDUCIDA a 4 entradas (el plano real tiene más): lo que se fija aquí
    // es la propiedad —dos cadenas ⇒ el orden de entrada NO decide—, no la
    // secuencia completa de ese plano.
    const int enter = 1;
    std::vector<SublayerKey> items{
        sk(37244490869137479ull, 0, 3, 14.3929),
        sk(37225600060465223ull, 0, 3, 14.4488),
        sk(37244490869137479ull, 1, 1, 14.4498),
        sk(37225600060465223ull, 1, 2, 14.4498),
    };
    const auto order = MultiPassScheduler::order_sublayers_by_tool_windowed(
        items, enter, NeoTowerZ::Z_EPS_PLAN);

    REQUIRE(is_permutation_of_indices(items, order));
    REQUIRE(causal_order_respected(items, order));
    // Con dos cadenas, invertir el orden de los items empatados NO cambia la
    // secuencia de herramientas: la decide la lógica de cadena.
    std::vector<SublayerKey> swapped{items[0], items[1], items[3], items[2]};
    const auto order_sw = MultiPassScheduler::order_sublayers_by_tool_windowed(
        swapped, enter, NeoTowerZ::Z_EPS_PLAN);
    REQUIRE(chain_pairs(items, order, enter) == chain_pairs(swapped, order_sw, enter));
}

// ===========================================================================
// NeoTowerPure::sublayer_slot_height — <=40% of nominal, floored at 0.04 mm.
// ===========================================================================

TEST_CASE("NeoTowerPure::sublayer_slot_height bounds", "[NeoTower][Pure]")
{
    // min_lh below 40% of nominal → min_lh wins.
    REQUIRE_THAT(NeoTowerPure::sublayer_slot_height(0.1f, 0.3f),  WithinAbs(0.1f, 1e-6));
    // 40% of nominal below min_lh → 40% wins.
    REQUIRE_THAT(NeoTowerPure::sublayer_slot_height(0.2f, 0.2f),  WithinAbs(0.08f, 1e-6));
    // Both below the 0.04 mm floor → floor wins.
    REQUIRE_THAT(NeoTowerPure::sublayer_slot_height(0.02f, 0.05f), WithinAbs(0.04f, 1e-6));
}

// ===========================================================================
// NeoTowerPure::eff_layer_height — s103 delta-Z height normalization.
// ===========================================================================

TEST_CASE("NeoTowerPure::eff_layer_height: staircase shrinks, sparse gap unchanged (s103)",
          "[NeoTower][Pure]")
{
    SECTION("staircase plane in a half-size gap uses the delta height") {
        // 0.1 mm above the previous emitting plane, nominal 0.2 → shrink to 0.1.
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(2.1f, 0.2f, /*last_plane*/2.0f),
                     WithinAbs(0.1f, 1e-3));
    }
    SECTION("delta below the 0.04 mm floor is clamped up, never below NOMINAL_LH_MIN") {
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(2.02f, 0.2f, 2.0f),
                     WithinAbs(NeoTowerZ::NOMINAL_LH_MIN, 1e-4));
    }
    SECTION("delta >= nominal (sparse gap) keeps the nominal height unchanged") {
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(0.5f, 0.2f, 0.0f),
                     WithinAbs(0.2f, 1e-6));
    }
    SECTION("no advance (delta ~ 0) keeps the nominal height") {
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(2.0f, 0.2f, 2.0f),
                     WithinAbs(0.2f, 1e-6));
    }
    // NEOTKO_NEOTOWER_TAG s237 — BUG B. El suelo debe aplicarse TAMBIÉN en la rama de
    // retorno crudo. Caso real de BIGTEST-ADAPTIVE: la rama de capa real de 1a alimenta
    // `lt.wipe_tower_layer_height`, que YA es un delta (9.58902 − 9.57571 = 0.0133076).
    // Con el nominal envenenado la condición `delta < nominal_h` NO se cumple, así que
    // antes de s237 salía crudo → `;HEIGHT:0.0133076` en el gcode.
    SECTION("a poisoned nominal below the floor is clamped up (s237, BUG B)") {
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(9.58902f, 0.0133076f,
                                                    /*last_plane*/9.57571f),
                     WithinAbs(NeoTowerZ::NOMINAL_LH_MIN, 1e-4));
    }
    SECTION("the floor also applies with no previous plane (s237)") {
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(0.2f, 0.01f, /*last_plane*/0.2f),
                     WithinAbs(NeoTowerZ::NOMINAL_LH_MIN, 1e-4));
    }
    SECTION("a healthy nominal is untouched by the floor (s237 no-op check)") {
        REQUIRE_THAT(NeoTowerPure::eff_layer_height(0.5f, 0.2f, 0.0f),
                     WithinAbs(0.2f, 1e-6));
    }
}

TEST_CASE("NeoTowerZ: the group epsilon must be wt2's merge epsilon (s236)",
          "[NeoTower][NeoTowerZ]")
{
    // s236 missing-purge bug. NeoTower's z-grouping (NT_WT_EPS) decides how many
    // plan layers it BELIEVES wt2 will build; plan_toolchange's WT_LAYER_Z_EPS
    // decides how many it ACTUALLY builds. They must be the same value, or every
    // Δz between them opens a group that wt2 merges away, and wt2_li drifts by one
    // for the rest of the print — orphaning TCRs and emitting colour changes with
    // no tower visit (repro lancuak3-A34.3mf, Δz = 3e-5).
    //
    // WT_LAYER_Z_EPS is defined as Z_EPS_PLAN in NeoWipeTower.cpp, so this pins
    // the contract from the constants side. Any future attempt to make the
    // grouping "finer" than the merge reopens the bug.
    STATIC_REQUIRE(NeoTowerZ::Z_EPS_PLAN > NeoTowerZ::Z_EPS_GROUP);

    // And the reason using the coarser epsilon is SAFE (preserves s49): a
    // sublayer sits SUBLAYER_GAP below its real layer, which stays above the
    // merge epsilon, so the sublayer/real pair still lands in two plan layers.
    STATIC_REQUIRE(NeoTowerZ::SUBLAYER_GAP > NeoTowerZ::Z_EPS_PLAN);

    // The window that was silently broken: 1e-5 < Δz < 1e-4.
    const float pathblend_spread = 3e-5f;   // A34: 3.4498 - 3.44977
    REQUIRE(pathblend_spread > NeoTowerZ::Z_EPS_GROUP);   // NeoTower split it
    REQUIRE(pathblend_spread < NeoTowerZ::Z_EPS_PLAN);    // wt2 merged it
}

// ===========================================================================
// NeoTowerPure::mark_standalone_planes — s114 band-top, s236 "exactly one".
//
// s236 bug: the marker accepted every sublayer inside the 0.02 mm same-plane
// window instead of the single highest one. MultiPass puts its sublayers
// 0.2-1.2 µm below the nominal, so a two-sublayer painted plane got BOTH marked
// as structural planes; the first then advanced the emitting-plane tracker and
// the second computed eff_layer_height() over a 1 µm delta → clamped to the
// 0.04 mm floor → ~5× the purge lines → tower depth 51.6 mm instead of 20.8 mm.
// Pre-fix, T1/T2/T3 below marked 4/3/4 respectively.
// ===========================================================================

namespace {
// Sublayer/real event carrying BOTH Z levels (the ev() helper below is for the
// dedup tests and leaves z_nominal at 0).
NeoTowerEvent pev(float z_nominal, float z_actual, size_t old_t, size_t new_t, bool sub)
{
    NeoTowerEvent e;
    e.z_nominal   = z_nominal;
    e.z_actual    = z_actual;
    e.old_tool    = old_t;
    e.new_tool    = new_t;
    e.is_sublayer = sub;
    return e;
}
size_t count_marked(const std::vector<NeoTowerEvent>& evts)
{
    return static_cast<size_t>(
        std::count_if(evts.begin(), evts.end(),
                      [](const NeoTowerEvent& e) { return e.standalone_plane; }));
}
} // namespace

TEST_CASE("mark_standalone_planes: two sublayers on a parent-less plane mark ONE plane (s236)",
          "[NeoTower][Pure]")
{
    // The exact geometry of the reproducer scene: nominal 0.65 realised by two
    // MultiPass sub-planes 1 µm apart, two toolchanges on each, and NO real
    // event anywhere at 0.65.
    std::vector<NeoTowerEvent> evts = {
        pev(0.65f, 0.6488f, 0, 2, /*sub*/true),
        pev(0.65f, 0.6488f, 2, 0, /*sub*/true),
        pev(0.65f, 0.6498f, 0, 2, /*sub*/true),
        pev(0.65f, 0.6498f, 2, 0, /*sub*/true),
    };
    const size_t marked = NeoTowerPure::mark_standalone_planes(evts);

    // Both toolchanges of the band-top share ONE physical plane → both flagged.
    // Pre-fix this returned 4: the lower sub-plane was flagged too, and it is
    // the one that poisoned the tracker.
    REQUIRE(marked == 2);
    REQUIRE(evts[0].standalone_plane == false);   // 0.6488 stays a lámina
    REQUIRE(evts[1].standalone_plane == false);
    REQUIRE(evts[2].standalone_plane == true);    // 0.6498 is the band-top
    REQUIRE(evts[3].standalone_plane == true);
    REQUIRE(count_marked(evts) == marked);
}

TEST_CASE("mark_standalone_planes: three sub-planes still mark only the top one (s236)",
          "[NeoTower][Pure]")
{
    // Generalization of T1 — nothing in the code caps the sublayer count, and a
    // 3-pass gradient is a legitimate scene. Pre-fix: 3.
    std::vector<NeoTowerEvent> evts = {
        pev(0.65f, 0.6478f, 0, 2, /*sub*/true),
        pev(0.65f, 0.6488f, 2, 1, /*sub*/true),
        pev(0.65f, 0.6498f, 1, 0, /*sub*/true),
    };
    REQUIRE(NeoTowerPure::mark_standalone_planes(evts) == 1);
    REQUIRE(evts[0].standalone_plane == false);
    REQUIRE(evts[1].standalone_plane == false);
    REQUIRE(evts[2].standalone_plane == true);
}

TEST_CASE("mark_standalone_planes: consecutive painted layers each keep a plane (s112-113)",
          "[NeoTower][Pure]")
{
    // THE REGRESSION GUARD. s114 exists because a RUN of fully-painted layers
    // left the tower with no structural plane at all → frozen tracker →
    // multi-layer gap → flow-boost whiskers. The s236 fix narrows the marking,
    // so it must still yield exactly one plane PER painted layer — never zero.
    std::vector<NeoTowerEvent> evts = {
        pev(0.65f, 0.6488f, 0, 2, /*sub*/true),
        pev(0.65f, 0.6498f, 2, 0, /*sub*/true),
        pev(0.85f, 0.8488f, 0, 2, /*sub*/true),
        pev(0.85f, 0.8498f, 2, 0, /*sub*/true),
    };
    REQUIRE(NeoTowerPure::mark_standalone_planes(evts) == 2);
    REQUIRE(evts[1].standalone_plane == true);    // band-top of layer 0.65
    REQUIRE(evts[3].standalone_plane == true);    // band-top of layer 0.85
    REQUIRE(evts[0].standalone_plane == false);
    REQUIRE(evts[2].standalone_plane == false);
}

TEST_CASE("mark_standalone_planes: a real event on the plane suppresses marking (s114)",
          "[NeoTower][Pure]")
{
    // The pink-cube / MMU-line scenarios: any real (non-sublayer) event at the
    // same z_nominal means the layer is NOT parent-less, so every sublayer is a
    // decoration of it. This behaviour is unchanged by s236 and is what made the
    // bug invisible in every scene that had a second colour at that height.
    std::vector<NeoTowerEvent> evts = {
        pev(0.65f, 0.6488f, 0, 2, /*sub*/true),
        pev(0.65f, 0.6498f, 2, 0, /*sub*/true),
        pev(0.65f, 0.65f,   2, 3, /*sub*/false),   // the cube's real toolchange
    };
    REQUIRE(NeoTowerPure::mark_standalone_planes(evts) == 0);
    REQUIRE(count_marked(evts) == 0);
}

TEST_CASE("mark_standalone_planes: a staircase shell outside the window is never a plane (s102-h)",
          "[NeoTower][Pure]")
{
    // z_actual well below z_nominal = box-in-drawer staircase shell, not a
    // band-top. It must stay synthetic+inset even when it is the ONLY sublayer
    // of a parent-less layer; the 0.02 mm window semantics are untouched by s236.
    std::vector<NeoTowerEvent> evts = {
        pev(0.65f, 0.55f, 0, 2, /*sub*/true),
    };
    REQUIRE(NeoTowerPure::mark_standalone_planes(evts) == 0);
}

// ===========================================================================
// NeoTowerPure::dedup_events — s79f real-over-sublayer promotion + max not sum.
// ===========================================================================

namespace {
NeoTowerEvent ev(float z, size_t old_t, size_t new_t, float vol, bool sub)
{
    NeoTowerEvent e;
    e.z_actual    = z;
    e.old_tool    = old_t;
    e.new_tool    = new_t;
    e.wipe_volume = vol;
    e.is_sublayer = sub;
    return e;
}
} // namespace

TEST_CASE("NeoTowerPure::dedup_events: real event promoted over colliding sublayer (s79f)",
          "[NeoTower][Pure]")
{
    // sub z=0.9998 and real z=1.0 both quantize to z_um=1000 with the same
    // (0→1) pair. The original bug kept the FIRST (sublayer) and dropped the
    // real, starving the canonical layer's purge. Fix: promote to the real,
    // absorbing the sublayer's smaller purge via max().
    std::vector<NeoTowerEvent> evts = {
        ev(0.9998f, 0, 1, 5.f,  /*sub*/true),
        ev(1.0f,    0, 1, 20.f, /*sub*/false),
    };
    NeoTowerPure::dedup_events(evts);

    REQUIRE(evts.size() == 1);
    REQUIRE(evts[0].is_sublayer == false);          // promoted to real
    REQUIRE_THAT(evts[0].z_actual, WithinAbs(1.0f, 1e-6));   // real Z kept
    REQUIRE_THAT(evts[0].wipe_volume, WithinAbs(20.f, 1e-6));
}

TEST_CASE("NeoTowerPure::dedup_events: duplicate purge uses max, never the sum",
          "[NeoTower][Pure]")
{
    // Two objects generating the same TC at one Z: the tower purges ONCE with the
    // worst-case volume (max), not once per object (sum).
    std::vector<NeoTowerEvent> evts = {
        ev(0.65f, 0, 1, 10.f, false),
        ev(0.65f, 0, 1, 15.f, false),
    };
    NeoTowerPure::dedup_events(evts);

    REQUIRE(evts.size() == 1);
    REQUIRE_THAT(evts[0].wipe_volume, WithinAbs(15.f, 1e-6));  // max(10,15), not 25
}

TEST_CASE("NeoTowerPure::dedup_events: distinct keys are all preserved", "[NeoTower][Pure]")
{
    std::vector<NeoTowerEvent> evts = {
        ev(0.60f, 0, 1, 10.f, false),   // distinct z
        ev(0.65f, 0, 1, 10.f, false),
        ev(0.65f, 1, 0, 10.f, false),   // same z, distinct (old,new)
    };
    NeoTowerPure::dedup_events(evts);
    REQUIRE(evts.size() == 3);
}

// ===========================================================================
// NeoTowerPure::validate_emission_bijection — V18 (s205-5b.1).
// The canonical emission-order list must be a faithful bijection of the four
// emittable lookup maps. These freeze the healthy case (silent) and each way the
// shadow can diverge from the maps, which is exactly what the later positional
// consume (5b.3) must not hit.
// ===========================================================================

namespace {

using PairMap  = std::unordered_map<uint64_t, std::pair<size_t, size_t>>;
using IdxMap   = std::unordered_map<uint64_t, size_t>;

// One Real TowerEvent + its m_tcr_index entry, kept in sync.
TowerEvent te_real(float z, size_t o, size_t n, size_t li, size_t si)
{
    return TowerEvent{z, o, n, LayerKind::Real, NeoTowerPure::make_key(z, o, n), li, si};
}

} // namespace

TEST_CASE("V18: a faithful shadow of all four maps is silent", "[NeoTower][V18]")
{
    std::vector<TowerEvent> order;
    PairMap tcr, tcr_sub, finish;
    IdxMap  merged;

    // Real TC.
    {
        auto k = NeoTowerPure::make_key(1.0f, 0, 1);
        order.push_back(TowerEvent{1.0f, 0, 1, LayerKind::Real, k, 3, 2});
        tcr[k] = {3, 2};
    }
    // Sublayer TC (same z_um as a real can collide across channels — legal).
    {
        auto k = NeoTowerPure::make_key(1.0f, 1, 2);
        order.push_back(TowerEvent{1.0f, 1, 2, LayerKind::Sublayer, k, 4, 0});
        tcr_sub[k] = {4, 0};
    }
    // Structural (z-only key).
    {
        uint64_t zk = (uint64_t) std::llround(2.0f * 1000.f);
        order.push_back(TowerEvent{2.0f, 1, 1, LayerKind::Structural, zk, 5, 0});
        finish[zk] = {5, 0};
    }
    // Bridge — shares the tcr_index map with Real.
    {
        auto k = NeoTowerPure::make_key(1.5f, 2, 0);
        order.push_back(TowerEvent{1.5f, 2, 0, LayerKind::Bridge, k, 6, 1});
        tcr[k] = {6, 1};
    }
    // Bridge-merged — index into m_merged_tcrs.
    {
        auto k = NeoTowerPure::make_key(1.5f, 2, 3);
        order.push_back(TowerEvent{1.5f, 2, 3, LayerKind::BridgeMerged, k, 0, 0});
        merged[k] = 0;
    }

    auto v = NeoTowerPure::validate_emission_bijection(order, tcr, tcr_sub, finish, merged);
    REQUIRE(v.empty());
}

TEST_CASE("V18: list entry whose key is absent from its map is flagged", "[NeoTower][V18]")
{
    std::vector<TowerEvent> order = { te_real(1.0f, 0, 1, 0, 0) };
    PairMap tcr, tcr_sub, finish; IdxMap merged;   // tcr is EMPTY — key missing
    auto v = NeoTowerPure::validate_emission_bijection(order, tcr, tcr_sub, finish, merged);
    REQUIRE_FALSE(v.empty());
}

TEST_CASE("V18: target {li,si} mismatch between list and map is flagged", "[NeoTower][V18]")
{
    auto e = te_real(1.0f, 0, 1, 3, 2);
    std::vector<TowerEvent> order = { e };
    PairMap tcr = { {e.key, {9, 9}} };             // map points elsewhere
    PairMap tcr_sub, finish; IdxMap merged;
    auto v = NeoTowerPure::validate_emission_bijection(order, tcr, tcr_sub, finish, merged);
    REQUIRE_FALSE(v.empty());
}

TEST_CASE("V18: orphan map entry with no list entry is flagged (count check)", "[NeoTower][V18]")
{
    auto e = te_real(1.0f, 0, 1, 3, 2);
    std::vector<TowerEvent> order = { e };
    PairMap tcr = { {e.key, {3, 2}}, {NeoTowerPure::make_key(2.f, 0, 1), {7, 0}} }; // extra orphan
    PairMap tcr_sub, finish; IdxMap merged;
    auto v = NeoTowerPure::validate_emission_bijection(order, tcr, tcr_sub, finish, merged);
    REQUIRE_FALSE(v.empty());   // list has 1, map has 2 → count mismatch
}

TEST_CASE("V18: Real and Bridge counts combine into tcr_index", "[NeoTower][V18]")
{
    // One Real + one Bridge, both into tcr_index (size 2). Combined count = 2 → OK.
    auto r = te_real(1.0f, 0, 1, 0, 0);
    auto bk = NeoTowerPure::make_key(1.5f, 2, 0);
    std::vector<TowerEvent> order = { r, TowerEvent{1.5f, 2, 0, LayerKind::Bridge, bk, 1, 0} };
    PairMap tcr = { {r.key, {0, 0}}, {bk, {1, 0}} };
    PairMap tcr_sub, finish; IdxMap merged;
    auto v = NeoTowerPure::validate_emission_bijection(order, tcr, tcr_sub, finish, merged);
    REQUIRE(v.empty());
}

// ===========================================================================
// NeoTowerPure::validate_shadow_consumption — runtime shadow (s205-5b.2).
// Every emitted TCR ↔ exactly one canonical entry; a folded Bridge = census.
// ===========================================================================

namespace {

TowerEvent tev(LayerKind k, size_t li, size_t si)
{
    TowerEvent te; te.kind = k; te.key = 1000 + li * 10 + si; te.li = li; te.si = si; return te;
}
TowerEvent tev_spec(LayerKind k, size_t li, size_t si)   // speculative spare (5b.2c)
{
    TowerEvent te = tev(k, li, si); te.speculative = true; return te;
}
ShadowSlot emit_tcr(size_t li, size_t si)    { return ShadowSlot{false, false, li, si}; }
ShadowSlot emit_merged(size_t idx)           { return ShadowSlot{true,  false, idx, 0}; }
ShadowSlot emit_finish(size_t li, size_t si) { return ShadowSlot{false, true,  li, si}; }

} // namespace

TEST_CASE("Shadow: healthy — each entry emitted once, folded Bridge = census", "[NeoTower][Shadow]")
{
    std::vector<TowerEvent> order = {
        tev(LayerKind::Real,         0, 0),
        tev(LayerKind::Sublayer,     1, 0),
        tev(LayerKind::Structural,   2, 0),
        tev(LayerKind::Bridge,       3, 1),   // folded into its merged → emitted 0×
        tev(LayerKind::BridgeMerged, 0, 0),   // li = index into m_merged_tcrs
    };
    std::vector<ShadowSlot> seq = {
        emit_tcr(0, 0), emit_tcr(1, 0), emit_finish(2, 0), emit_merged(0),
    };
    auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
    REQUIRE(rep.violations.empty());
    REQUIRE(rep.census.size() == 1);   // the standalone Bridge, emitted 0×
    REQUIRE(rep.emitted == 4);
}

TEST_CASE("Shadow: an entry emitted twice is a violation", "[NeoTower][Shadow]")
{
    std::vector<TowerEvent> order = { tev(LayerKind::Real, 0, 0) };
    std::vector<ShadowSlot> seq   = { emit_tcr(0, 0), emit_tcr(0, 0) };
    auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
    REQUIRE_FALSE(rep.violations.empty());
}

TEST_CASE("Shadow: a non-Bridge entry never emitted is a phantom violation", "[NeoTower][Shadow]")
{
    std::vector<TowerEvent> order = { tev(LayerKind::Real, 0, 0) };
    std::vector<ShadowSlot> seq   = {};   // emitted nothing
    auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
    REQUIRE_FALSE(rep.violations.empty());
}

TEST_CASE("Shadow: emitting a slot with no canonical entry is a violation", "[NeoTower][Shadow]")
{
    std::vector<TowerEvent> order = { tev(LayerKind::Real, 0, 0) };
    std::vector<ShadowSlot> seq   = { emit_tcr(0, 0), emit_tcr(9, 9) };  // 9,9 is phantom
    auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
    REQUIRE_FALSE(rep.violations.empty());
}

TEST_CASE("Shadow: Structural and Real at the same slot don't alias (from_finish)", "[NeoTower][Shadow]")
{
    // A finish-channel Structural and a tcr-channel Real can both land on m_result[1][0];
    // from_finish keeps them apart so neither is a false double/phantom.
    std::vector<TowerEvent> order = {
        tev(LayerKind::Real,       1, 0),
        tev(LayerKind::Structural, 1, 0),
    };
    std::vector<ShadowSlot> seq = { emit_tcr(1, 0), emit_finish(1, 0) };
    auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
    REQUIRE(rep.violations.empty());
    REQUIRE(rep.census.empty());
}

TEST_CASE("Shadow: growth/structural fallback emissions clear the 5b.2 phantoms", "[NeoTower][Shadow]")
{
    // Regression for 5b.2b. Structural growth events are dispatched at runtime via the
    // get_tcr-MISS → planned-slot fallback (GCode.cpp), NOT get_finish_layer. Before 5b.2b
    // that path recorded nothing, so every growth entry showed as a "phantom" violation.
    // 5b.2b records each fallback emission as a finish-channel slot (from_finish=true, si=0);
    // once every structural entry is emitted exactly once, the shadow is clean.
    std::vector<TowerEvent> order = {
        tev(LayerKind::Real,       0, 0),
        tev(LayerKind::Structural, 5, 0),   // growth events on several plan layers
        tev(LayerKind::Structural, 6, 0),
        tev(LayerKind::Structural, 7, 0),
    };
    SECTION("all structural growth emitted via the instrumented fallback → clean") {
        std::vector<ShadowSlot> seq = {
            emit_tcr(0, 0), emit_finish(5, 0), emit_finish(6, 0), emit_finish(7, 0),
        };
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE(rep.violations.empty());
        REQUIRE(rep.census.empty());
    }
    SECTION("an uninstrumented growth emission still surfaces as a phantom") {
        // If a future emission path is missed, the entry stays unrecorded → violation.
        std::vector<ShadowSlot> seq = {
            emit_tcr(0, 0), emit_finish(5, 0), emit_finish(6, 0), // (7,0) missing
        };
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE_FALSE(rep.violations.empty());
    }
}

TEST_CASE("Shadow: speculative spares consumed 0× are census, not violations", "[NeoTower][Shadow]")
{
    // 5b.2c. The MP sublayer scheduler seeds synthetic cross-product TCs (and sublayer-
    // plane structural finishes / folded merged) that runtime grouping may legitimately
    // NOT consume. A speculative entry at 0× is expected → census; a NON-speculative one
    // at 0× is still a real phantom violation (the safety net must not be silenced).
    SECTION("speculative spare never consumed → census, zero violations") {
        std::vector<TowerEvent> order = {
            tev(LayerKind::Sublayer,      0, 0),   // real painted sublayer — must emit
            tev_spec(LayerKind::Sublayer, 1, 0),   // synthetic spare — may be 0×
            tev_spec(LayerKind::Structural, 2, 0), // sublayer-plane finish spare
            tev_spec(LayerKind::BridgeMerged, 0, 0),
        };
        std::vector<ShadowSlot> seq = { emit_tcr(0, 0) };   // only the real one emitted
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE(rep.violations.empty());
        REQUIRE(rep.census.size() == 3);   // the 3 spares
    }
    SECTION("speculative spare consumed once → clean (no census, no violation)") {
        std::vector<TowerEvent> order = { tev_spec(LayerKind::Sublayer, 1, 0) };
        std::vector<ShadowSlot> seq   = { emit_tcr(1, 0) };
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE(rep.violations.empty());
        REQUIRE(rep.census.empty());
    }
    SECTION("non-speculative entry at 0× is STILL a violation (safety net holds)") {
        std::vector<TowerEvent> order = {
            tev_spec(LayerKind::Sublayer, 1, 0),   // spare, 0× ok
            tev(LayerKind::Real,          2, 0),   // real, 0× = bug
        };
        std::vector<ShadowSlot> seq = {};
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE(rep.violations.size() == 1);       // only the real Real entry
    }
}

TEST_CASE("Shadow: a colliding-key twin at 0× is census, not a phantom", "[NeoTower][Shadow]")
{
    // 5b.2c. A real sublayer chain TC and a synthetic TC can share make_key(z,old,new) but
    // point at different m_result slots. Only one wins m_tcr_index_sub[key]; get_tcr resolves
    // by key so it emits that one, leaving the twin at 0×. Both encode the same transition →
    // the 0× twin is benign even when it is the NON-speculative (real) one.
    TowerEvent real_tc  = tev(LayerKind::Sublayer, 3, 0);       real_tc.key  = 5000;
    TowerEvent synth_tc = tev_spec(LayerKind::Sublayer, 4, 0);  synth_tc.key = 5000; // twin
    SECTION("synthetic twin wins the map → real twin 0× = census (no violation)") {
        std::vector<TowerEvent> order = { real_tc, synth_tc };
        std::vector<ShadowSlot> seq   = { emit_tcr(4, 0) };      // synth slot emitted
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE(rep.violations.empty());
        REQUIRE(rep.census.size() == 1);                        // the real twin, covered by key
    }
    SECTION("neither twin emitted → still a real phantom (key never covered)") {
        std::vector<TowerEvent> order = { real_tc, synth_tc };
        std::vector<ShadowSlot> seq   = {};
        auto rep = NeoTowerPure::validate_shadow_consumption(order, seq);
        REQUIRE_FALSE(rep.violations.empty());                  // real twin: key 0× → phantom
    }
}
