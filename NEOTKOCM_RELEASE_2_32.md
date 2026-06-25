## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.2 (beta) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP)** marker in the UI.

**2.3.2 is an incremental release on top of 2.3.1** (see `NEOTKOCM_RELEASE_2_31.md` and `NEOTKOCM_RELEASE_2_3.md` for the full feature set and the upstream Snapmaker Orca patch notes, which carry over unchanged). The headline of 2.3.2 is a single, important correctness fix: **penultimate ColorStitch / PathBlend effects now generate with normal presets** — you no longer need *Ensure all* to use the penultimate zone. Everything remains **opt-in**: at defaults the build behaves like stock Snapmaker Orca 2.3.4.

---

## What's new in 2.3.2

**Penultimate zone fix — effects no longer blocked unless "Ensure all" (important)**
On 2.3.1 (and earlier), penultimate ColorStitch / PathBlend effects — and penultimate density — were silently **not generated** unless **Strength → Top/bottom shells → Ensure vertical shell thickness** was set to **"Ensure all"**. With the default **"Ensure moderate"** (and every other setting), the penultimate layer was simply absent from the slice: no penultimate weave, no PathBlend ramp, and the penultimate density knob had no effect.

The root cause was an **incomplete port**. The penultimate solid (`stPenultimateInternalSolid`) was only ever **created** inside the *Ensure all* branch of the vertical-shell discovery pass. Stock presets like *Ensure moderate* route through the **horizontal-shell** discovery pass instead, and that pass had only ever been ported to **preserve** an existing penultimate layer — never to **create** one. So unless you happened to be on *Ensure all*, the penultimate layer was never born, and everything downstream (weave / PathBlend / density) had nothing to attach to.

The fix **mirrors the vertical-shell penultimate classification into the horizontal-shell pass**: the fresh top-derived solid at a distance of `[1 … Penultimate top layers]` below each top surface is re-tagged as the penultimate solid. **Only the surface *type* changes — the geometry is byte-identical**, and the rest of the solid is untouched, so the regular (non-penultimate) infill is unaffected. The two discovery paths are mutually exclusive, so there is no risk of creating the penultimate layer twice.

This change is fully **gated**: when **Penultimate top layers = 0** (the default) and no painted penultimate recipe is in play, objects slice **byte-for-byte identically** to before. You only see a difference when you actually ask for penultimate layers — which is exactly the case that was broken. **Verified by the user** from basic to "chaos" multi-stair scenes: penultimate ColorStitch splits and PathBlend now appear in the slice with normal presets, and the penultimate density knob works again.

> **Upgrade note for 2.3.1 users:** you can drop the *Ensure all* workaround. If you set it only to get penultimate effects, you can return *Ensure vertical shell thickness* to its normal value.

---

## Example project — BIGTEST.3mf

A sample project **`BIGTEST.3mf`** ships at the root of this build. It is a worked example of using Passes to build gradients and of checking colour combinations:

- The **staircase printed with T3** is the one carrying the **ColorStitch patterns** — use it to see weave / stripe / gradient behaviour.
- For the clearest colour **mixing**, load the **filament with the lowest TD** (transmission distance) into the tower so blended colours read through.
- The **staircases are deletable** — remove some to test **smaller gradients** quickly.
- **Everything is editable** via the Sandwich 3D / Paint editor — open the painter or the Sandwich editor to retune passes, angles and palettes.

To exercise the 2.3.2 fix specifically: enable **Penultimate top layers** (Strength → Top/bottom shells), paint or assign a penultimate recipe, and slice with a normal **Ensure vertical shell thickness** preset — the penultimate effect now appears without *Ensure all*.

---

## Beta Defaults & Notes

Unchanged from 2.3 / 2.3.1 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7, NeoTower tower type Classic / Zigurat taper on, Penultimate top layers 0). Default changes only affect **new** profiles.

---

## Work in progress / not yet in this build

- **Variable layer height** is unlocked (under NeoTower + Libre Mode) but still flagged **Experimental** — prefer fixed layer height for production multi-tool Sandwich prints and review G-code.
- **Sandwich Painter weave preview** is **WIP** — minor surface-selection microglitch and approximate stripe scale.
- **Neoweaving + Monotonic Interlayer Nesting** are **not yet ported** to this base.
- **World-space import** is functional at a basic level; importing as Assembled and splitting in Libre Mode is the recommended route.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs (Wall Count Stability / Blend Distance) are not exposed yet.

---

## Important Reminders (read before slicing a Sandwich)

- **Use Monotonic Line for the top surface pattern** (Quality → Top surface pattern). ColorStitch only sequences correctly with Monotonic Line.
- **Enable Penultimate Top Layers to use penultimate effects** (Strength → Top/bottom shells), unless you paint a penu recipe (auto-forced per object). As of 2.3.2 this works with normal *Ensure vertical shell thickness* presets — *Ensure all* is no longer required.
- **PathBlend on the Penultimate zone is still disabled** (gradient-direction bug on multi-stair penu surfaces). Top-zone PathBlend works normally.
- **TD values are per-machine, not per-print.** Calibrate them so the predicted palette swatches match reality.
- This is a **beta** — the ColorStitch Painter weave preview especially is WIP. Inspect G-code on important prints.

---

## Backward Compatibility

- ColorStitch remains a **UI rename only** — config keys, 3MF metadata and saved profiles are unchanged. Projects and presets from earlier Neotko builds (including 2.3 and 2.3.1) load unchanged.
- The penultimate fix changes **only the surface type** of an already-present solid; objects without penultimate layers slice byte-for-byte identically to 2.3.1.
- New options default to safe/off values and don't affect existing prints unless you opt in.

---

*Neotko FullSpectrum 2.3.2 — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower / NeoArachne / Libre Mode*
*Features designed by Neotko · in collaboration with Snapmaker & Radoux (FullSpectrum)*

---

**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
