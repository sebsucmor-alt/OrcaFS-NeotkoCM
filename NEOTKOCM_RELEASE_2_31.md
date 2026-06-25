## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.1 (beta) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP)** marker in the UI.

> 🔧 **Superseded by 2.3.2.** This release has a **known penultimate-zone limitation**: penultimate ColorStitch / PathBlend effects (and penultimate density) only generate when **Strength → Top/bottom shells → Ensure vertical shell thickness = "Ensure all"**; with the default ("Ensure moderate") the penultimate layer is silently skipped. If you rely on penultimate effects on 2.3.1, set *Ensure all* as a workaround — or move to **2.3.2**, which fixes it (see `NEOTKOCM_RELEASE_2_32.md`).

**2.3.1 is an incremental release on top of 2.3** (see `NEOTKOCM_RELEASE_2_3.md` for the full re-platforming notes and the upstream Snapmaker Orca 2.3.5 beta patch set, which carry over unchanged). The headline of 2.3.1 is a **wipe-tower correctness fix** plus the **Sandwich Painter weave preview** and the **Variable Layer Height (Experimental)** unlock under NeoTower + Libre Mode. Everything remains **opt-in**: at defaults the build behaves like stock Snapmaker Orca 2.3.4.

---

## What's new in 2.3.1

**Wipe-tower fix — missing "drawers" and the "empty first layer" abort (important)**
On multi-object Sandwich / ColorStitch scenes with NeoTower, some real layers were not getting their wipe-tower **"drawer"** (the per-layer purge box), and deleting objects could starve the tower of so many drawers that a layer came out empty and the slice **aborted** with *"Wipe tower generation failed, possibly due to empty first layer."*

The root cause was a **plan-vs-emission divergence**: when several ColorStitch sub-layers of the same object land at the **same Z** (the multi-tool lamina), the G-code emitter ordered them with a **non-stable sort keyed only on Z**, so the relative order — and therefore the *tool the layer exits on* — was build-dependent. NeoTower predicts that exit tool **semantically**, so plan and emission disagreed, the tower realignment lookup missed, and the purge for that layer was silently skipped (a lost drawer). The fix is a **one-line deterministic tie-break** (Z, then object index, then layer index) — exactly the order NeoTower already plans — so emission now matches the plan **by construction**. Single-tool layers and real layers are unaffected; the change only reorders within a single drawer, so the "no box ever crosses two drawers" invariant is preserved (no mega-extrusions). **Verified by the user** on the previously-failing scenes (uniform, coherent tower, no abort).

**Sandwich Painter — ColorStitch weave preview (WIP)**
Painted top surfaces now show the **ColorStitch weave directly on the model** — the actual per-line tool stripes / dither / gradient / hard bands — instead of a flat swatch colour. The preview is built from the **same per-line sequence the slicer produces**, so filament colours, density and pattern match the G-code. The same builder also feeds the Pro-tray pass strip and the Sandwich editor strip, so all three stay identical.
- **Scale matches the print** — stripe pitch comes from the resolved top **line width**, and each painted **island** (connected flat zone) gets its own gradient ramp scaled to its own extent, just like the slice. Tiled patterns repeat at the real line width.
- **Angle wheel** — set the pass angle by **scrolling the mouse wheel over the pass preview bar** (Pro tray and Sandwich editor); the bar, the 3D model and the print rotate together. For a **fixed** angle the slicer's per-layer fill rotation is locked out so every layer keeps the painted orientation. Auto angle (`-1`) shows an amber **"auto angle"** tag, since a static preview can't match the alternating fill.
> **WIP note.** The preview is mostly accurate but has a **minor surface-selection microglitch** on some geometry, and the stripe scale uses the painted-area extent (not the exact post-perimeter line count), so it can differ by a line or two. Islands wider than ~64 lines coarsen in the preview. Inspect G-code on important prints.

**Variable Layer Height (Experimental) — under NeoTower + Libre Mode**
A new option **Variable layer height (Experimental)** sits under **Tower type** and is exposed **only with Tower type = NeoTower and Libre Mode active** (visible but greyed-out otherwise). **Default: off.** When **on**, the slicer stops blocking scenes that **mix objects with different layer heights** and **adaptive / variable layer height combined with more than one filament** — NeoTower purges each toolchange at the real per-layer height. The wipe-tower "missing drawers" issue that previously made this rough is fixed by the change above, so the tower now stays coherent under variable layer height. Still flagged **Experimental** — review G-code before long multi-tool runs.

**Perimeter-override double-wall fix**
Fixed a double perimeter (two walls printed over each other at the same extrusion) in perimeter-override blocks, introduced while porting to the 2.3.4 base — the per-pass perimeter suppression guard was restored. Print-verified.

**UX / polish**
- **Dark-mode Sandwich editor** — the editor now follows the app colour mode correctly (it was reading the OS appearance on macOS instead of the app's dark/light setting).
- **Painter combos in light mode** — popup/header/text colours adapt correctly.
- **Line distribution mode** moved from the Sandwich dialog into the Tab (**Quality → Surface ColorStitch**, below *Minimum line length*).
- **Auto-promote to NeoTower when you paint** — starting a ColorStitch paint job switches the tower type to NeoTower automatically.
- Minimum-line-length fallbacks relaxed (1.0 → 0.0).

---

## Example project — BIGTEST.3mf

A sample project **`BIGTEST.3mf`** ships at the root of this build. It is the test scene used to validate the wipe-tower fix above, and it doubles as a **worked example of using Passes to build gradients** and of checking colour combinations:

- The **staircase printed with T3** is the one carrying the **ColorStitch patterns** — use it to see weave / stripe / gradient behaviour.
- For the clearest colour **mixing**, load the **filament with the lowest TD** (transmission distance) into the tower so blended colours read through.
- The **staircases are deletable** — remove some to test **smaller gradients** quickly.
- **Everything is editable** via the Sandwich 3D / Paint editor — open the painter or the Sandwich editor to retune passes, angles and palettes.

---

## Beta Defaults & Notes

Unchanged from 2.3 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7, NeoTower tower type Classic / Zigurat taper on, Penultimate top layers 0). Default changes only affect **new** profiles.

---

## Work in progress / not yet in this build

- **Variable layer height** is unlocked (above) but still flagged **Experimental** — prefer fixed layer height for production multi-tool Sandwich prints and review G-code.
- **Sandwich Painter weave preview** is **WIP** — minor surface-selection microglitch and approximate stripe scale (above).
- **Neoweaving + Monotonic Interlayer Nesting** are **not yet ported** to this base.
- **World-space import** is functional at a basic level; importing as Assembled and splitting in Libre Mode is the recommended route.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs (Wall Count Stability / Blend Distance) are not exposed yet.
- **Penultimate effects require *Ensure all*** (known issue, fixed in 2.3.2) — see the banner at the top of this file.

---

## Important Reminders (read before slicing a Sandwich)

- **Use Monotonic Line for the top surface pattern** (Quality → Top surface pattern). ColorStitch only sequences correctly with Monotonic Line.
- **Enable Penultimate Top Layers to use penultimate effects** (Strength → Top/bottom shells), unless you paint a penu recipe (auto-forced per object).
- **PathBlend on the Penultimate zone is still disabled** (gradient-direction bug on multi-stair penu surfaces). Top-zone PathBlend works normally.
- **TD values are per-machine, not per-print.** Calibrate them so the predicted palette swatches match reality.
- This is a **beta** — the ColorStitch Painter weave preview especially is WIP. Inspect G-code on important prints.

---

## Backward Compatibility

- ColorStitch remains a **UI rename only** — config keys, 3MF metadata and saved profiles are unchanged. Projects and presets from earlier Neotko builds (including 2.3) load unchanged.
- New options default to safe/off values and don't affect existing prints unless you opt in.

---

*Neotko FullSpectrum 2.3.1 — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower / NeoArachne / Libre Mode*
*Features designed by Neotko · in collaboration with Snapmaker & Radoux (FullSpectrum)*

---

**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
