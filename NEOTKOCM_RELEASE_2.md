## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development will be conducted in close collaboration with Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum the now official part of the Snapmaker team. So from v1.9 forward expect big things!
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko SurfaceColorMix 2.0 (beta) + FullSpectrum v0.99 — Release Notes

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP Beta)** marker in the UI.

This release continues straight from 1.9 (NeoArachne + the pure-MultiPass wipe-tower fix). The headline of 2.0 is the move from a single "Surface Color Mixer" into a full **ColorStitch** workflow — generate palettes, paint them, and let a smarter wipe tower keep everything in sync.

---

## Major Features

**ColorStitch — the new name for the Surface Color Mixer family**
The Surface Color Mixer is being rebranded to **ColorStitch** across the UI: the Sandwich dialog now hosts a **ColorStitch Studio**, and the 3D painter is now the **ColorStitch Painter**. Internal names, config keys and 3MF data are unchanged — old projects and presets keep working, and you will still see the word *ColorMix* in a few places (e.g. the per-line effect pill). They refer to the same system.

**ColorStitch Studio — palette generators (inside the Sandwich dialog)**
Instead of building recipes pass by pass, the Studio generates whole strips of colour swatches from your loaded filaments and their TD values, using the same colour-science engine as the TD Preview. Three modes:
- **Gradient ramp** — a manual A → B ramp (pick two tools, weave pattern, steps, split range).
- **Flat colour (predict)** — browses the gamut reachable by stacking solid passes. Robust, predictable.
- **Mixed approximation (predict)** — browses an *extended* gamut: a dithered ColorStitch base plus a translucent solid on top, reaching colours no single filament can make.

Click a swatch to load that complete recipe into the live Sandwich. **Target + Match** runs an inverse ΔE2000 search to hit a specific colour, and **Export palette…** turns swatches into reusable Surface Effect Profiles. The strips react live to the TD sliders.

**ColorStitch Painter — revamped (WIP / semi phase)**
> The painter is the most in-progress part of this release — it is marked **(WIP Beta)** in the panel and will keep evolving (eyedropper / pick-from-face and multi-object painting are coming).

The painter no longer shows a plain text list of profiles. It now shows your filaments as **generated colour palettes** — three collapsible strips (Mixed / Gradient / Flat), the same as the Studio. Pick a swatch (it becomes the **active colour**) and paint a flat top surface with **Smart Fill**.

A new **two-tier palette model** fixes the old "list fills up with every shade" problem:
- **Working colours (automatic)** — created on demand the first time you paint with a swatch, deduplicated, and garbage-collected when no painted face uses them anymore. They don't clutter the saved list.
- **Saved palettes (deliberate)** — press **Save palette** to promote the active colour into the **Profiles** list. Those are named, persistent, and travel in the 3MF.

The old Circle / Sphere / Triangle brushes were removed — the ColorStitch Painter is Smart-Fill only, which is what flat top surfaces need.

**NeoTower — post-slice wipe-tower planner**
A new wipe-tower planner that runs **after slicing**, when every toolchange (including the sub-layer primes that Sandwiches and MultiPass insert *inside* a layer) is already known. Because it plans from the complete, real toolchange list, it builds a **fixed, predictable footprint** that stays in sync with the G-code and understands variable layer heights. New options under **Quality → Prime tower**:
- **Tower type** — Classic (stock WipeTower2) or NeoTower.
- **Zigurat taper** (default on) — keeps each wall ring resting on the one below (wall-on-wall); disable to save material/time.
- **Sandwich purge compaction** (default **1.7**) — compacts thin sub-layer purges into a narrower band to shrink the tower footprint (1.0 = off).
- **SurfaceColorStitch wipe reserve** (default **10 mm³**) — single, unified purge reserve before each ColorStitch / MultiPass / PathBlend sub-layer toolchange (replaces the old separate top/penu prime-volume keys).

Sandwich / MultiPass scenes auto-promote to NeoTower regardless of the Tower-type setting.

**World-first: adaptive layers × multi-tool × Sandwich on one tower**
Because NeoTower plans from the real post-slice toolchange list and is delta-Z aware, you can now combine **adaptive/variable layer height + multiple tools + a Sandwich** — three things that normally fight each other on the wipe tower — and still get a coherent tower (plan = emission = tower, no "unexpected toolchange" divergences). A **Tetris-style purge compaction** further shrinks the tower footprint. This combination is still being hardened.

---

## Beta Defaults & Changes

- **SurfaceColorStitch wipe reserve** default raised **5 → 10 mm³**.
- **Sandwich purge compaction** default **1.0 → 1.7**.
- The experimental **Micro Stitch (Neotko)** top/bottom surface fill pattern is **hidden** in this beta (presets that already use it still load and slice; it just isn't offered in the dropdown).
- Default changes only affect **new** profiles — existing saved profiles keep whatever values they had.

---

## Important Reminders (read before slicing a Sandwich)

- **Use MonotonicLine for the top surface pattern.** ColorMix only sequences correctly with **MonotonicLine** (Quality → Top surface pattern). Plain Monotonic / Rectilinear may look fine on a simple square but produce wrong toolchange order on complex shapes.
- **Enable Penultimate Top Layers to use penultimate effects.** Penultimate surfaces only exist if **Strength → Top/bottom shells → Penultimate top layers** is set above 0. If you configure a ColorStitch / MultiPass effect on the *Penu* surface but leave this at 0, there's no penu surface to apply it to. (When you *paint* a profile that declares penu activity, the slicer auto-forces 2 penu layers for that object — but for global preset use you set it yourself.)
- **PathBlend on the Penultimate zone is still disabled** — a known gradient-direction bug on multi-stair penu surfaces. The PathBlend pill is hidden on the Penu card; Top-zone PathBlend works normally.
- **TD values are per-machine, not per-print.** They describe your actual filaments. Calibrate them (single colour → two-colour blend) so the predicted palette swatches match reality.
- This is a **beta** — the ColorStitch Painter especially is WIP. Inspect G-code on important prints.

---

## Backward Compatibility

- Existing projects and presets **load unchanged**. ColorStitch is a UI rename only — config keys, 3MF metadata (`colormix_profiles_b64`, per-volume slot tables, `paint_colormix` facet attributes) and saved profiles are all the same format.
- New options (Tower type, zigurat, purge compaction, wipe reserve) default to safe values; NeoArachne and the Sandwich features behave as in 1.9 unless you opt in.
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
