# ColorStitch on Monotonic — Penu residual breakage — DEEP-DIVE (arranque s154)

> **ARRANQUE DE SESIÓN (hacer en orden, luego PAUSAR):**
> 1. Leer `memory/MEMORY.md` (reglas críticas) + `memory/session-s153-colorstitch-monotonic.md` (contexto completo de lo hecho).
> 2. Leer este doc entero.
> 3. Releer en código los 3 sitios clave (abajo, §Files).
> 4. **PAUSAR y avisar al usuario** que está listo para el deep-dive → el usuario activará **Ultracode mode** y dará los datos/luz verde. NO seguir con cambios hasta entonces.

---

## Objetivo de la feature
ColorStitch sobre el patrón **Monotonic (continuo)**, no solo Monotonic Line. Gate `colorstitch_monotonic_split` (default OFF). Top funciona; **penu (penultimate) sigue rompiendo en monotonic**. Rectilinear (ambos overlaps) ya funciona perfecto.

## Qué está SHIPEADO y verificado (s153)
- **Gate + UI** `colorstitch_monotonic_split` (bool) bajo "Line distribution mode". (+ `colorstitch_monotonic_replan` enum 0-3 y UI de `neotko_interlayer_nesting_enabled`, ambos del mismo bloque.)
- **Split post-hoc** en `SurfaceColorMix.cpp`: `split_monotonic_path_into_runs()` parte cada path fusionado en runs `[scan][tail]`; el `tail` (arco conector) se re-adjunta en emisión con el color saliente. `RawLine.tail`.
- **Eje de superficie** (no per-path): `MONO_SURF_AXIS` calculado una vez sobre todos los paths y pasado al split (arregló el caso donde un path fragmentado volteaba el eje 90°).
- **Penu area-match** (`PrintObject.cpp:~4432`, `discover_horizontal_shells`): el penu se re-delimita con el overlap del **top** (`top_bottom_infill_wall_overlap`) en vez de `infill_wall_overlap`. Log `penu_grow_to_top`. **VERIFICADO: penu area 3141→3119.138 = top 3119.165.**
- **Instrumentación de debug (DEJAR puesta para el deep-dive):** `MONO_SURF_AXIS`, `MONO_SPLIT`, `MONO_AXIS_DIAG` (fill_dir(ref) vs dominant vs delta, ref_idx/len/ang), `MONO_AXIS_OUTLIERS` (runs >20° off-dominant), `penu_grow_to_top`. Canal `ORCA_DEBUG_COLORMIX` → `/tmp/neotko_colormix.log`.

## EL BUG RESIDUAL (lo que el deep-dive debe resolver)
El area-match **ayudó** pero NO cerró el problema. Evidencia nueva (colormix.log s153, monotonic, post-fix):

- **Caso LIMPIO** (cuando penu area/líneas cuadran con top): penu `n=133` (= top n=133), `fill_dir(ref)=91° dominant=88° delta=2° lanes=114`, tool counts **idénticos** a top (T0:58/T1:75). → cuando penu == top de verdad, **ya sale bien**.
- **Caso CONTAMINADO (residual)**: penu `n=139` (≠ top n=135), `fill_dir(ref)=156° dominant=90° delta=65° ref_idx=86 ref_len=195mm pts=6 ang=156° lanes=157`.

**Patrón:** cuando penu `n == top n` → limpio. Cuando `n` difiere → contaminado. Es decir, queda un caso donde penu sigue generando un nº de runs distinto al top y su run más larga es contaminada (diagonal).

## Causa raíz subyacente (la fragilidad nunca cerrada)
`compute_slot_per_line` ([SurfaceColorMix.hpp:~806]) saca `fill_dir` de **una sola línea (la más larga)** por **endpoints** (`lane_pick_reference` + `lane_direction`). Si la run más larga del penu es una cadena contaminada (connector que huggea el perímetro y mi clasificador Δ⊥ per-segmento NO parte porque cada segmento cruza <½ spacing), el eje se vuelca → `LaneQuant` revuelto.
- **Top tiene las MISMAS runs contaminadas** (ver sus `MONO_AXIS_OUTLIERS`: idx=88 191mm 147°, etc.) pero sobrevive porque su run limpia (254mm vertical) es la más larga.
- **El `dominant` (eje pesado por longitud) que ya calcula `MONO_AXIS_DIAG` es SIEMPRE correcto** (90/91/88) aunque `fill_dir(ref)` salga 156/0.

## CAMINOS para el deep-dive (a decidir con el usuario en Ultracode)
1. **Fix robusto en código compartido** — cambiar `fill_dir` en `compute_slot_per_line` de "línea más larga/endpoints" → **eje dominante pesado por longitud** (doubled-angle sobre todas las runs). Para superficies de una sola dirección (top/rectilíneo/monotonic-line) da resultado idéntico → no rompe lo que va. Es la solución que el `dominant` del diagnóstico prueba correcta en TODOS los casos. **Riesgo:** toca el template que usa todo ColorStitch — el usuario fue cauto aquí, confirmar antes.
2. **Endurecer el clasificador del split** para que no genere las runs contaminadas (partir conectores que cruzan de carril poco a poco). **YA SE INTENTÓ** con índice de carril absoluto por punto y **ROMPIÓ otras cosas** (revertido). Camino delicado.
3. **Investigar por qué el penu residual da `n` distinto al top** aun con area-match: ¿multi-instancia (el log tiene 3 `po=`)? ¿overlap no totalmente igualado en ese slice? ¿el fill del penu genera una cadena extra que el top no? Confirmar correlacionando `penu_grow_to_top` delta con el bloque contaminado.

**Recomendación de partida:** camino 1 (dominante) es el más sólido y barato; el diagnóstico demuestra que `dominant` clava siempre. Pero ANTES, con Ultracode, confirmar §3 (por qué queda el caso n≠top) por si hay algo más en cómo se genera el penu.

## Files clave (releer en arranque)
- `src/libslic3r/SurfaceColorMix.cpp`: `split_monotonic_path_into_runs` (~150), rama monotonic split + `MONO_SURF_AXIS` (~1525), `MONO_AXIS_DIAG` block (~1766), emisión re-attach tail (~1771).
- `src/libslic3r/SurfaceColorMix.hpp`: `compute_slot_per_line` (~790), `lane_pick_reference` (~777), `lane_direction` (~759) ← el `fill_dir` frágil.
- `src/libslic3r/PrintObject.cpp`: penu area-match `penu_grow_to_top` (~4432, `discover_horizontal_shells`).
- `src/libslic3r/Fill/FillBase.cpp`: `connect_infill` + `take`/`take_limited` (origen de los conectores; SOLO lectura).
- `src/libslic3r/Fill/Fill.cpp:866-965`: wiring patrón/anchor top vs penu (referencia).

## Reglas (recordatorio)
- Editar SOLO bajo `CLAUDEDROP/SNAPOFFICIAL/snapmaker-orca-official/src/...`; tras editar `rsync -a` SIN `--delete` a `Downloads/SNAPOFIC/`. NO compilar (lo hace el usuario). El penu ES código del usuario → se puede cambiar.
- Cuando se cierre el bug: quitar la instrumentación `MONO_AXIS_DIAG`/`MONO_AXIS_OUTLIERS` (diagnóstico) si el usuario lo pide.
