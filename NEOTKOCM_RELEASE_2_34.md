## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.4 (beta) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP)** marker in the UI.

**2.3.4 is an incremental release on top of 2.3.3** (see `NEOTKOCM_RELEASE_2_33.md` and earlier notes for the full feature set and the upstream Snapmaker Orca patch notes, which carry over unchanged). This release is a **UX-focused pass on the Sandwich Painter** — a reorganized "departments" layout, a real PathBlend preview, new tool icons, and a brand-new **SVG Sticker** tool for placing stackable, sandwich-painted decals on a model — plus a batch of smaller fixes across the painter. Everything remains **opt-in**: at defaults the build behaves like stock Snapmaker Orca 2.3.4.

---

## What's new in 2.3.4

### Sandwich Painter — departments redesign (new UX)

The painter gizmo has been reorganized around four clear **departments** instead of one crowded panel: **Palette** (paint/erase/pick + saved recipes + stickers), **Generator** (build gradients and palettes automatically), **Pro** (the full per-pass editor, including Bottom), and **Object** (object-level toggles, including the MixedFilament Object mode live recipe). A persistent header now shows the active recipe's swatch, "+ New" and "Save", and the Select/Paint/Eraser/Pick toolbar is **shared across all departments** so it's always clear which object/recipe you're editing no matter where you are in the panel. The department selector itself is now a proportional segmented bar instead of loose buttons.

This is a pure UI reorganization — the underlying paint/sandwich engine, PathBlend math and saved-profile format are untouched.

### PathBlend preview — now matches the real print geometry (fix)

The PathBlend preview inside the painter's Pro department was drawing a flat left-to-right color gradient that had nothing to do with the actual pass physics. It's been rewritten to mirror the Sandwich Editor's own preview exactly: full-layer background, flat cap above the ramp, and a ramp that follows the real easing curve (Linear / Ease-In / Ease-Out / In-Out). What you see in the painter now **is** what will print — previously it could look "close enough" while silently disagreeing with the Sandwich Editor. The PathBlend fill angle, which existed in the engine but had no UI anywhere, is also now exposed directly in the painter with a live diagonal-hatch overlay so you can see it before slicing.

### ColorStitch Pattern Color generator (rename + rebuild)

The Generator department's old "Mixed (ColorStitch)" palette — a rarely-exercised code path suspected of producing incorrect off-color (e.g. yellow-tinted) swatches — has been replaced with **"ColorStitch Pattern Color"**, built on the same overlapping-pass engine already used by the Gradient/Flat palettes elsewhere in Pro mode. Swatches now reflect the same color math you'd get from the rest of the painter.

### New tool icons + clearer visual language

The Select / Paint / Eraser / Pick toolbar now uses real icons (in both light and dark variants) instead of text buttons, and the shared toolbar is visually split into a "tool" zone and a "danger" zone (the "Erase all painting" action now reads clearly as destructive). Active/inactive states are more legible at a glance.

### Sandwich Stickers — SVG decals with their own sandwich recipe (new)

A new **Sticker** tool in the Palette department: load a single-color SVG and place it as a flat decal on any top-facing surface of an object, with its **own independent Sandwich recipe** (color/passes), separate from whatever is painted underneath. Stickers can be **stacked** — the topmost sticker in the stack wins and occludes what's beneath it (no blending), and the list lets you reorder them and see which one currently "wins" at a given spot.

Once placed, a sticker can be **moved, rotated and scaled** directly against the model, with a colored overlay showing the assigned recipe's color and true footprint so you can line stacked stickers up by eye before slicing. Typical uses: logos, labels, or small color accents that shouldn't inherit the surrounding paint job.

**Status:** shipped and functional — place, move, rotate, scale and paint all confirmed working in testing. Known open issue: switching between a painted zone and a sticker zone on the same object doesn't always trigger the expected wipe-tower toolchange — under active investigation, don't rely on it for prints that mix both on the same layer yet.

### Texture Bump Mapping — coming soon (disabled)

An engine + gizmo for mapping bump/relief textures onto printed surfaces has been in development, but it is **not enabled in this build** — early prints showed over-extrusion issues that need to be resolved first. It's WIP and will land in a future release once it prints cleanly.

---

## Example project — BIGTEST.3mf

A sample project **`BIGTEST.3mf`** ships at the root of this build. It is a worked example of using Passes to build gradients and of checking colour combinations:

- The **staircase printed with T3** is the one carrying the **ColorStitch patterns** — use it to see weave / stripe / gradient behaviour.
- For the clearest colour **mixing**, load the **filament with the lowest TD** (transmission distance) into the tower so blended colours read through.
- The **staircases are deletable** — remove some to test **smaller gradients** quickly.
- **Everything is editable** via the Sandwich 3D / Paint editor — open the painter or the Sandwich editor to retune passes, angles and palettes.

To try the new 2.3.4 pieces: open the Sandwich Painter and explore the new **Palette / Generator / Pro / Object** departments; check the **PathBlend** preview in Pro against the Sandwich Editor to see them now match; and try the new **Sticker** tool in Palette to place, stack and reorder a couple of SVG decals on a top surface.

---

## Beta Defaults & Notes

Unchanged from 2.3 through 2.3.3 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7, NeoTower tower type Classic / Zigurat taper on, Penultimate top layers 0). Default changes only affect **new** profiles. The departments redesign, PathBlend preview fix and Sandwich Stickers are all additive UI/paint-time changes and do not alter existing profiles' slicing output.

---

## Work in progress / not yet in this build

- **Texture Bump Mapping** — engine and gizmo exist but are **disabled**; over-extrusion seen in early prints needs to be fixed first. Coming in a future release.
- **Sandwich Stickers** — wipe tower doesn't reliably toggle between a painted zone and a sticker zone on the same object; no dedicated tool icon yet (uses a text button); dragging directly onto the colored overlay isn't supported yet (use the move/rotate/scale controls in the sticker's row).
- **Bottom Surface** — pattern preview still doesn't render on bottom-facing surfaces in the 3D painter view (slice output is correct).
- **RealColor View** — TD cache invalidation when TD is changed without a re-slice not yet fully re-verified.
- **MixedFilament Object mode** — pattern preview (not just color) and a more prominent top-level toggle are still not implemented.
- **Variable layer height** is unlocked (under NeoTower + Libre Mode) but still flagged **Experimental** — prefer fixed layer height for production multi-tool Sandwich prints and review G-code.
- **Neoweaving + Monotonic Interlayer Nesting** are **not yet ported** to this base.
- **World-space import** is functional at a basic level; importing as Assembled and splitting in Libre Mode is the recommended route.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs (Wall Count Stability / Blend Distance) are not exposed yet.
- Known pre-existing bug (not new in 2.3.4): tooltip colors are hard to read in day/light mode in both the Sandwich Editor and the Sandwich Painter gizmo — a partial fix exists but doesn't cover every tooltip.

---

## Important Reminders (read before slicing a Sandwich)

- **Use Monotonic Line for the top surface pattern** (Quality → Top surface pattern). ColorStitch only sequences correctly with Monotonic Line.
- **Enable Penultimate Top Layers to use penultimate effects** (Strength → Top/bottom shells), unless you paint a penu recipe (auto-forced per object). Works with normal *Ensure vertical shell thickness* presets since 2.3.2 — *Ensure all* is no longer required.
- **PathBlend on the Penultimate zone is still disabled** (gradient-direction bug on multi-stair penu surfaces). Top-zone PathBlend works normally.
- **Bottom Surface is restricted by design**: max 2 solid passes, max 1 ColorStitch pass, PathBlend always FULL (ramp + cap) — this is intentional, not a limitation to work around.
- **Sandwich Stickers stack top-wins, not blended** — if you need two decals over the same spot, reorder them in the list rather than expecting a mix.
- **TD values are per-machine, not per-print.** Calibrate them so the predicted palette swatches — and the RealColor View preview — match reality.
- This is a **beta** — the ColorStitch Painter weave preview and RealColor View especially are WIP/experimental. Inspect G-code on important prints.

---

## Backward Compatibility

- ColorStitch remains a **UI rename only** — config keys, 3MF metadata and saved profiles are unchanged. Projects and presets from earlier Neotko builds (including 2.3 through 2.3.3) load unchanged.
- The departments redesign, PathBlend preview fix, ColorStitch Pattern Color generator, new icons and Sandwich Stickers are all additive and gated: objects/profiles that don't use stickers slice and render exactly as before.
- New options default to safe/off values and don't affect existing prints unless you opt in.

---

*Neotko FullSpectrum 2.3.4 — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower / NeoArachne / Libre Mode / RealColor View*
*Features designed by Neotko · in collaboration with Snapmaker & Radoux (FullSpectrum)*

---

**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
