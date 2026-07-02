## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.3 (beta) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **This is a beta.** Several pieces below are usable but still being refined — review your generated G-code before long or production prints. Surfaces that are still in progress show a **(WIP)** marker in the UI.

**2.3.3 is an incremental release on top of 2.3.2** (see `NEOTKOCM_RELEASE_2_32.md`, `NEOTKOCM_RELEASE_2_31.md` and `NEOTKOCM_RELEASE_2_3.md` for the full feature set and the upstream Snapmaker Orca patch notes, which carry over unchanged). 2.3.3 lands three substantial pieces: **Bottom Surface** (paint/sandwich control on the underside of a print, plus the wipe-tower correctness work needed to make it safe), a new **MixedFilament Object mode** in the Sandwich Painter, and an early **RealColor View** in the G-code Viewer (physically-based color mixing preview). Everything remains **opt-in**: at defaults the build behaves like stock Snapmaker Orca 2.3.4.

---

## What's new in 2.3.3

### Bottom Surface — paint and sandwich the underside of your print (new)

You can now target the **bottom-facing surfaces** of an object with the same painting/sandwich machinery previously available only for the top: ColorStitch, PathBlend and solid Sandwich passes, plus a dedicated "Bottom" zone in the Sandwich Painter (Pro tray). Typical uses: coloring the first layer / underside face, controlling the face that lands on supports (a step toward NeoWeaving), and painting a distinct bonding layer between stacked objects.

Bottom surfaces are recognized by **role, not by Z-height**, and fall into three cases handled differently under the hood:
- **Bed-supported bottom** (`stBottom`) — full density, layer-0 hardlock, fully paintable.
- **Bridge / overhang bottom** (`stBottomBridge`) — bridge density/flow is normalized so paint reads correctly instead of printing under-extruded.
- **Stacked-object contact** — painted as a distinct bonding zone (advanced use — e.g. dissimilar-material adhesion).

Restrictions specific to the Bottom zone (by design, for reliability): max 2 solid passes, max 1 ColorStitch pass, and PathBlend always runs in **FULL** mode (ramp + cap) when enabled.

**Wipe-tower correctness work that shipped alongside Bottom Surface** (all gated, byte-identical when Bottom Surface / the affected paths aren't in use):
- Fixed a **Z crash** (extrusions dropping to negative Z, head into the bed) triggered by Bottom Surface's off-by-one plane lookup interacting with a wipe-tower Z re-alignment path.
- Fixed **bridge purge volume** being silently hardcoded to zero on certain wipe-tower toolchange paths, causing under-purged, out-of-spec-looking transitions.
- **Root-caused and fixed a long-standing "phantom tower" bug** inherited from the base fork (previously unresolved for a long time, only ever seen as an intermittent issue tied to supports): painted ColorStitch/MMU surfaces were projected using the wrong transform for **assembled** objects, so the paint silently failed to line up with the sliced surface, degrading to "unpainted" and causing the tower to plan an identity tool-change (no wall, no purge) instead of a real one. Verified by the user on a full assembled plate — walls and painted sandwich now generate correctly everywhere, including fully assembled arrangements.

**Status:** Bottom Surface is **functional** and closes out the wipe-tower correctness chapter that blocked it. Known remaining gap: the painter doesn't yet draw the pattern preview on bottom-facing surfaces in the 3D view (the slice itself is correct) — cosmetic only.

### MixedFilament Object mode — auto-sandwich by transmission distance (new, beta)

A new **object-level toggle** in the Sandwich Painter: turning it on for an object replaces all of that object's top-surface and penultimate painting with an **automatically computed sandwich** (3 solid passes + a forced Perimeter Override) that approximates — via each filament's TD (transmission distance) — the color of the MixedFilament assigned as that object's extruder. In short: assign a MixedFilament to an object, flip the toggle, and the slicer builds the layered color-mixing recipe for you instead of hand-painting it.

The color math is TD-aware (parallel-blend model), not a naive RGB average, so the generated sandwich actually reads correctly once printed and light passes through the layers.

**Status:** beta, compiles clean, logic verified line-by-line against the engine — user-tested and confirmed **"beta muy buena"**. Known polish items for a future release (not yet implemented): a pattern preview (not just color) for the generated sandwich, and moving the toggle out of the "Pro" panel to a top-level control given how significant it is.

### RealColor View — physically-based color mixing in the G-code Viewer (new, early/experimental)

A new **RealColor** render mode in the G-code Viewer's color-mode combo, showing an approximation of how mixed/blended colors will actually look once printed, using depth peeling and a Beer-Lambert light-transmission model instead of flat per-extruder colors. Aimed squarely at ColorStitch / PathBlend / Sandwich users who want to preview color mixing before committing to a print.

This is the newest and least battle-tested feature in this release — implemented and then debugged live against real user screenshots, logs and G-code across several passes. Fixes along the way: the option was invisible on macOS (fixed — was silently falling back to a legacy GL profile that failed a version check); a zoom-dependent shimmer/noise artifact (fixed — the anti-aliasing bias needed to live in camera space, not screen space); thin ColorStitch/PathBlend detail being nearly invisible (fixed — added supersampling to the internal render passes); and medium-TD colors never fully converging to solid the way they should (fixed — corrected the convergence math). One more visual fix (PathBlend "ramp" not showing correct per-segment height) has been implemented and synced but is **pending the user's visual confirmation** after their next rebuild.

**Status:** early/experimental — functional and validated on real prints for most cases, but treat it as a preview aid, not a final-color guarantee, until further confirmed.

---

## Example project — BIGTEST.3mf

A sample project **`BIGTEST.3mf`** ships at the root of this build. It is a worked example of using Passes to build gradients and of checking colour combinations:

- The **staircase printed with T3** is the one carrying the **ColorStitch patterns** — use it to see weave / stripe / gradient behaviour.
- For the clearest colour **mixing**, load the **filament with the lowest TD** (transmission distance) into the tower so blended colours read through.
- The **staircases are deletable** — remove some to test **smaller gradients** quickly.
- **Everything is editable** via the Sandwich 3D / Paint editor — open the painter or the Sandwich editor to retune passes, angles and palettes.

To try the new 2.3.3 pieces: open the **Bottom** zone in the Sandwich Painter's Pro tray to paint the underside of a supported object; assign a **MixedFilament** to an object and flip its **MixedFilament Object mode** toggle to auto-generate a sandwich; and switch the G-code Viewer's color mode to **RealColor** to preview mixed-color output.

---

## Beta Defaults & Notes

Unchanged from 2.3 / 2.3.1 / 2.3.2 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7, NeoTower tower type Classic / Zigurat taper on, Penultimate top layers 0). Default changes only affect **new** profiles. Bottom Surface, MixedFilament Object mode and RealColor View are all **opt-in** and do not change existing profiles' output.

---

## Work in progress / not yet in this build

- **Bottom Surface** — pattern preview doesn't render on bottom-facing surfaces in the 3D painter view yet (slice output is correct).
- **RealColor View** — PathBlend "ramp" per-segment height fix pending final visual confirmation on the user's next rebuild; TD cache invalidation when TD is changed without a re-slice not yet verified.
- **MixedFilament Object mode** — pattern preview and top-level toggle placement not yet implemented.
- **Variable layer height** is unlocked (under NeoTower + Libre Mode) but still flagged **Experimental** — prefer fixed layer height for production multi-tool Sandwich prints and review G-code.
- **Sandwich Painter weave preview** is **WIP** — minor surface-selection microglitch and approximate stripe scale.
- **Neoweaving + Monotonic Interlayer Nesting** are **not yet ported** to this base.
- **World-space import** is functional at a basic level; importing as Assembled and splitting in Libre Mode is the recommended route.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs (Wall Count Stability / Blend Distance) are not exposed yet.
- Known pre-existing bug (not new in 2.3.3): tooltip colors are hard to read in day/light mode in both the Sandwich Editor and the Sandwich Painter gizmo — a partial fix exists but doesn't cover every tooltip.

---

## Important Reminders (read before slicing a Sandwich)

- **Use Monotonic Line for the top surface pattern** (Quality → Top surface pattern). ColorStitch only sequences correctly with Monotonic Line.
- **Enable Penultimate Top Layers to use penultimate effects** (Strength → Top/bottom shells), unless you paint a penu recipe (auto-forced per object). Works with normal *Ensure vertical shell thickness* presets since 2.3.2 — *Ensure all* is no longer required.
- **PathBlend on the Penultimate zone is still disabled** (gradient-direction bug on multi-stair penu surfaces). Top-zone PathBlend works normally.
- **Bottom Surface is restricted by design**: max 2 solid passes, max 1 ColorStitch pass, PathBlend always FULL (ramp + cap) — this is intentional, not a limitation to work around.
- **TD values are per-machine, not per-print.** Calibrate them so the predicted palette swatches — and the new RealColor View preview — match reality.
- This is a **beta** — the ColorStitch Painter weave preview and RealColor View especially are WIP/experimental. Inspect G-code on important prints.

---

## Backward Compatibility

- ColorStitch remains a **UI rename only** — config keys, 3MF metadata and saved profiles are unchanged. Projects and presets from earlier Neotko builds (including 2.3, 2.3.1 and 2.3.2) load unchanged.
- Bottom Surface, MixedFilament Object mode and RealColor View are all additive and gated: objects/profiles that don't use them slice and render exactly as before.
- New options default to safe/off values and don't affect existing prints unless you opt in.

---

*Neotko FullSpectrum 2.3.3 — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower / NeoArachne / Libre Mode / RealColor View*
*Features designed by Neotko · in collaboration with Snapmaker & Radoux (FullSpectrum)*

---

**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
