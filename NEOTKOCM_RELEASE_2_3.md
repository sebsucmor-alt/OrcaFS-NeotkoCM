## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3 (beta) — on Snapmaker Orca 2.3.4 — Release Notes

> 🆕 **Superseded by 2.3.1** — see `NEOTKOCM_RELEASE_2_31.md` for the incremental release that adds the Sandwich Painter weave preview, the Variable Layer Height (Experimental) unlock, and an important wipe-tower correctness fix. The notes below remain the canonical description of the 2.3 re-platforming base.

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP)** marker in the UI.

**Base build:** Snapmaker Orca **2.3.4 nightly** (commit **`8597556`**, 2026-06-17) with the complete **2.3.5 beta** patch set integrated on top (upstream `main` **`4d77d7f`**, 2026-06-23). See *Upstream Snapmaker Orca 2.3.5 (beta) — integrated* below for the full list.

**2.3 is a re-platforming release.** The headline is not a new effect — it is that the **entire Neotko feature pack now runs on the official Snapmaker Orca 2.3.4 base**, instead of the older FullSpectrum 0.99 fork. The whole stack — Surface ColorStitch (the Sandwich), the ColorStitch Painter, NeoTower, NeoArachne — was ported onto 2.3.4 so the features can be adopted **à la carte** on a current, official base. On top of the port, **Libre Mode** graduates to a full professional-workflow tier, and **Align & Stack** joins as a first-class gizmo. Everything is **opt-in**: with the new options at their defaults, the build behaves like stock Snapmaker Orca 2.3.4.

---

## What's new in 2.3

**Ported to the official Snapmaker Orca 2.3.4 base**
The complete pack now lives on top of upstream 2.3.4 (build `8597556`) rather than the FullSpectrum 0.99 fork. This brings every stock 2.3.4 improvement and keeps the Neotko features as cleanly-gated additions for pick-and-choose adoption. Surface ColorStitch, the Painter, NeoTower and NeoArachne were re-verified on the new base.

**Surface ColorStitch — the Sandwich, as a pass stack**
The Sandwich Editor (Quality → Surface ColorStitch → **Sandwich editor…**) builds each surface as a **stack of 1–3 passes** per zone (Top layer / Penultimate layer). Each pass is **Solid / ColorStitch / PathBlend (Half|Full)**, with its own Z share (drag the dividers) and angle. *MultiPass is simply two or three stacked **Solid** passes.* The per-line **ColorStitch** editor uses plain-language controls (Smooth blend 2/3 colours, Stripes with manual band sizes, Custom text pattern, transition-shape easing), and the **ColorStitch Studio** generates Gradient / Flat / Mixed-approximation palettes with **Target + Match ▸** (ΔE2000).

**Libre Mode — professional workflow tier**
Libre Mode is now a real assembly/professional toolkit, gated behind a master switch in **Preferences → "Enable Neotko LibreMode (requires restart)"** and toggled from the **"Neotko LM"** toolbar button:
- **Floating objects** — place parts at any Z (no forced bed snap); empty first layer becomes a warning, not an error.
- **Assembled Boolean mode** (per object) — turn the boolean union off so overlapping multi-material geometry is kept as separate co-existing meshes (print-verified).
- **Per-volume XY compensation** — each part of an Assembled object can carry its own hole/contour compensation for materials that shrink differently.
- **Copy / Paste Process Settings** by block — copy a tuned Speed / Quality / Strength block (or All) and paste it onto other objects (All replaces, a single block merges).
- **Assembled Parts — full options** — parts inside an Assembled object expose the complete option set, with a **↺ Refresh Part** button.

**Align & Stack — new gizmo**
A new left-toolbar gizmo for aligning and stacking objects against an anchor. Click objects in order (**#1 is the anchor**), then choose **Place against** (rest touching a chosen face of #1, chained — the stacking mode) or **Align flush** (same-side faces coplanar). Includes a controllable **Z gap** and a **Drop to bed** action. Object selection was improved for easier picking.

**NeoArachne — alternative wall generator (under Libre Mode)**
The NeoArachne wall engine (per-feature Classic / Arachne (stock) / Arachne (NeotkoEdge) routing, Edge-Closure controls, and the before-slice **Preview Lab**) is ported and exposed in **Quality → Wall generator** when Libre Mode is active. Default recipe = Neotko Hybrid v2 (Classic outer + stock Arachne inner).

**ColorStitch Painter — palette groups, Save All, bigger slot budget**
- **Palette groups** — organise saved palettes into up to 10 global groups (**+ New group**, group selector).
- **Save all** — promote every unsaved working colour into the active group at once, so *Erase all* leaves nothing dangling.
- **Slot cap raised to 254** painted slots per object.
- Painting and the slot→profile mapping are now covered by **undo/redo**.
- Pro mode (the active colour is what you paint), **Pick** eyedropper, **Pin to palette**, two-tier working/saved model and right-click-camera-only all carry forward from 2.1.

**S3DFactory — Simplify3D `.factory` import**
`.factory` projects open directly (File → Import → Import 3D model). They load as a single **Assembled** object preserving the world-space layout; split in Libre Mode to recover the parts in place.

---

## Upstream Snapmaker Orca 2.3.5 (beta) — integrated

The full 2.3.5 beta patch set from upstream `main` (`4d77d7f`, 2026-06-23) is folded into this release on top of the 2.3.4 nightly base, so every official 2.3.5 improvement ships alongside the Neotko pack. Patches are referenced by their upstream PR number:

- **Filament Sync v2** — #527, #528, #535, #538 (sync dialog, layout, picker offset)
- **Filament Color Library & adaptation** — #520, #529
- **Top cover detection** (enclosure with ABS/PETG extraction) — #504, #521
- **Redesigned splash screen** — #505
- **Web bundle / resources** — #533, #513
- **MQTT** — #508
- **Misc fixes** — #518 (2.3.5-30 bugfix), #525 (logging), #530 (macOS build), #536 (filament profiles), #539 (compile fix), #540 (Linux build)
- **#537 (About) — partial:** only the `MIN_FIRM_VER` bump to 1.5.0 was taken; the app version is intentionally **not** bumped to upstream's number.

**Wipe-tower filament-waste fix — upstream #501 (adopted).**
On multi-tool prints the slicer auto-selected a dedicated wipe filament, a side effect of which was that **every other filament got treated as *soluble*** — adding a PVA-style extra **solid** purge on the tower slice of the layer *before* each tool change. On PLA/PETG that solid floor is pure waste (≈10.3 mm vs the normal 6.4 mm sparse slice in our tests — roughly +60% on each pre-toolchange slice). Adopting upstream #501 removes the auto-force: non-soluble filaments no longer trigger the extra purge, the genuine tool-change purge is untouched, and tool-change ordering now matches upstream. **Sandwich prints benefit most** (many tool changes). *Note: this was an upstream mainstream bug — the soluble flag was meant only for true soluble supports (PVA), which need the extra purge because they degrade in the hot end over time; it should never have fired on PLA.*

---

## Beta Defaults & Notes

- **Sandwich wipe reserve** (`multipass_prime_volume`) default **10 mm³** — the single unified purge reserve before each Sandwich sub-layer toolchange (Solid / ColorStitch / PathBlend, Top + Penultimate).
- **Sandwich purge compaction** default **1.7** (1.0 = off).
- **NeoTower** tower type default **Classic**; **Zigurat taper** default **on**. Sandwich / multi-pass scenes auto-promote to NeoTower regardless.
- **Penultimate top layers** default **0** — set it above 0 (Strength → Top/bottom shells) to use penultimate effects globally. Painting a penu recipe auto-forces 2 penu layers for that object.
- Default changes only affect **new** profiles — existing saved profiles keep their values.

---

## Work in progress / not yet in this build

- **Adaptive (variable) layer height is locked by default** here, and the adaptive × multi-tool × Sandwich combination is not yet unlocked on the 2.3.4 base. It is proven and works on the 2.2 line (FullSpectrum 0.99 fork); unlocking it on 2.3.4 is still being finished. Use a fixed layer height for multi-tool Sandwich prints for now. *(Unlocked as Experimental in 2.3.1 — see `NEOTKOCM_RELEASE_2_31.md`.)*
- **Neoweaving + Monotonic Interlayer Nesting** are **not yet ported** to this base (coming in a later release).
- **World-space import** is functional at a basic level; for assemblies, importing as Assembled and splitting in Libre Mode is the recommended route while it is finished.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs (Wall Count Stability / Blend Distance) are not exposed yet.

---

## Important Reminders (read before slicing a Sandwich)

- **Use Monotonic Line for the top surface pattern.** ColorStitch only sequences correctly with **Monotonic Line** (Quality → Top surface pattern). Plain Monotonic / Rectilinear may look fine on a simple square but produce wrong toolchange order on complex shapes.
- **Enable Penultimate Top Layers to use penultimate effects** (Strength → Top/bottom shells → Penultimate top layers > 0), unless you paint a penu recipe (auto-forced per object).
- **PathBlend on the Penultimate zone is still disabled** — a known gradient-direction bug on multi-stair penu surfaces. Top-zone PathBlend works normally.
- **TD values are per-machine, not per-print.** Calibrate them so the predicted palette swatches match reality.
- This is a **beta** — the ColorStitch Painter especially is WIP. Inspect G-code on important prints.

---

## Backward Compatibility

- ColorStitch remains a **UI rename only** — config keys, 3MF metadata (`colormix_profiles_b64`, per-volume slot tables, `paint_colormix` facet attributes) and saved profiles are all the same format. Projects and presets from earlier Neotko builds load unchanged.
- New options (Libre Mode, NeoArachne, NeoTower, palette groups) default to safe/off values and don't affect existing prints unless you opt in.
- Some `.3mf` files created with much older FullSpectrum builds may still need a re-save, as noted since 1.9.

---

*Neotko FullSpectrum — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower / NeoArachne / Libre Mode*
*Features designed by Neotko · in collaboration with Snapmaker & Radoux (FullSpectrum)*

---


**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
