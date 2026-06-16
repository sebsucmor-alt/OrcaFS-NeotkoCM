## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development will be conducted in close collaboration with Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum the now official part of the Snapmaker team. So from v1.9 forward expect big things!
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko SurfaceColorMix 2.1 (beta) + FullSpectrum v0.99 — Release Notes

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP Beta)** marker in the UI.

**2.1 is a refinement release on top of 2.0.** It does not add a new headline feature — instead it sharpens the parts of the ColorStitch workflow that were still rough: the **3D painter** (the biggest change), the **gradient / colour-prediction model**, and the **colour-match tooling**. Everything from 2.0 (ColorStitch Studio, NeoTower, the world-first adaptive × multi-tool × Sandwich tower) is unchanged and carried forward — see the 2.0 notes for those.

---

## What's new in 2.1

**ColorStitch Painter — Pro mode, live editing, and an eyedropper**
The painter is where most of the work went. It is still **(WIP Beta)**, but it is now a real composer instead of a swatch picker:

- **Pro mode is the active colour.** Whatever you build in the Pro panel — **Top / Penultimate** rows, each one **Solid / ColorStitch / PathBlend (Half|Full)**, with a per-pass **Z height** box — is exactly what you paint with. No more "build a profile, then select it".
- **Live binding by id.** Clicking a saved swatch loads it into Pro and **links** it; editing then **rewrites that profile in place**, so every object using it updates. **Pin to palette + Name** replaces the old "Use as paint colour" / "Save" buttons.
- **Pick (eyedropper) tool.** Click a painted face to read the **actual recipe** under the cursor and continue painting with it.
- **Destructive-mouse fixes.** **Right-click is now camera only** (orbit/pan never paints or erases), and left-clicking with no colour selected no longer wipes paint. These removed the accidental-erase traps from 2.0.
- **Slot cleanup.** Auto-created "working colours" that occupy a slot are shown with an amber border and can be removed with **right-click → Delete**, freeing the slot.
- **Penultimate painting works.** Painting a recipe that declares penu activity auto-forces 2 penultimate layers for that object, so the penu effect actually has a surface.
- **Perimeter override is a single option** driven by the recipe stack (one checkbox covers both zones), consistent between the painter and the Sandwich dialog.

**Gradient is now a clean top-only helper (colour-prediction fix)**
The Gradient ramp (in both the ColorStitch Studio and the painter) was built on top of the "Mixed approximation" model, which attached a penultimate ColorStitch dither to every ramp step. That dither pulled in tools the ramp never selected, so the predicted swatch colour drifted — most visibly, a *same-tool* gradient (yellow → yellow) predicted a colour that wasn't yellow. In 2.1 the gradient is a **pure top-surface sweep** of the A/B split:

- The **weave / dither Pattern** controls were removed from the gradient.
- **No penultimate ColorStitch is attached** to gradient recipes. A same-tool gradient now correctly predicts that tool's colour, both in the preview **and** in the slice.
- If you want a penu tone under the gradient, add it yourself afterwards — the gradient is a helper to keep the common case simple.

**"Mixed approximation" removed from the painter**
The painter now offers **Gradient + Flat** colour strips. (Mixed approximation is still available in the Sandwich-dialog ColorStitch Studio for the extended-gamut workflow.)

**Blend Suggestion retired → ColorStitch Studio Match**
The legacy **Blend Suggestion — Beer-Lambert optimizer** panel in the Sandwich dialog has been removed. Its job — find the recipe that best matches a target colour — is now handled by **ColorStitch Studio → Target + Match ▸**, which uses the newer **ΔE2000** colour-science engine and writes the result directly into the live Sandwich. The MixedColor target pool is shared, so nothing is lost.

**Wipe-tower colour fix (`;WIDTH:` desync)**
Fixed a G-code annotation bug where a tool change whose line width matched the previous one — right after the wipe tower — failed to re-emit the `;WIDTH:` comment. The viewer (OrcaSlicer and Simplify) then rendered that segment at the **tower's** line width, making a correctly-extruded colour look over-/under-sized. The extrusion itself was always correct; only the annotation (and therefore the preview) was wrong.

---

## Beta Defaults & Changes

Unchanged from 2.0 — listed here for convenience:

- **SurfaceColorStitch wipe reserve** default **10 mm³**.
- **Sandwich purge compaction** default **1.7**.
- The experimental **Micro Stitch (Neotko)** top/bottom surface fill pattern remains **hidden** in this beta (presets that already use it still load and slice).
- Default changes only affect **new** profiles — existing saved profiles keep whatever values they had.

---

## Important Reminders (read before slicing a Sandwich)

- **Use MonotonicLine for the top surface pattern.** ColorMix only sequences correctly with **MonotonicLine** (Quality → Top surface pattern). Plain Monotonic / Rectilinear may look fine on a simple square but produce wrong toolchange order on complex shapes.
- **Enable Penultimate Top Layers to use penultimate effects.** Penultimate surfaces only exist if **Strength → Top/bottom shells → Penultimate top layers** is above 0. (When you *paint* a profile that declares penu activity, the slicer auto-forces 2 penu layers for that object — but for global preset use you set it yourself.)
- **PathBlend on the Penultimate zone is still disabled** — a known gradient-direction bug on multi-stair penu surfaces. The PathBlend pill is hidden on the Penu card; Top-zone PathBlend works normally.
- **TD values are per-machine, not per-print.** They describe your actual filaments. Calibrate them (single colour → two-colour blend) so the predicted palette swatches match reality.
- This is a **beta** — the ColorStitch Painter especially is WIP. Inspect G-code on important prints.

---

## Backward Compatibility

- Existing projects and presets **load unchanged**. ColorStitch remains a UI rename only — config keys, 3MF metadata (`colormix_profiles_b64`, per-volume slot tables, `paint_colormix` facet attributes) and saved profiles are all the same format.
- **Gradient profiles generated before 2.1** keep the old penultimate ColorStitch they were saved with; the top-only behaviour applies to gradients you generate from 2.1 on. Re-generate from the Studio if you want the cleaned-up form.
- NeoArachne, NeoTower and the Sandwich features behave as in 2.0 unless you opt in.
- Some `.3mf` files created with much older FullSpectrum builds may still need a re-save, as noted since 1.9.

---

*OrcaSlicer FullSpectrum — Neotko Feature Pack · ColorStitch / NeoTower*
*Features designed by Neotko · in collaboration with Snapmaker & Radoux (FullSpectrum)*

---


**Big thanks to:**
SnapMaker 
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
