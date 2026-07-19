## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.7 (released) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ ** Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.

**2.3.7 is an incremental release on top of 2.3.6** (see `NEOTKOCM_RELEASE_2_36.md` and earlier notes
for the full feature set). This notes file is a **running draft** — it grows as sessions land, the same
way 2.3.6's did, until this version ships. So far it includes a fix for **tooltip colours in day/light
mode** across Painter Pro Mode, ColorStitch Painter and the Bump Mapping Editor, a first WIP cut of
**NeoWave Support's contact-layer toggle**, an early **untested WIP** of **NeoStitch Interlock**
(Z-axis layer interlocking), and a batch of Sandwich/ColorStitch Painter bug fixes. Everything remains
**opt-in**: at defaults the build behaves like stock Snapmaker Orca.

---

## What's new in 2.3.7

### Expert G-code Reprocessor (basic beta, Libre Mode, PRO/expert-only)

New panel in the G-code Preview (next to the RealColor view selector): layer-ranged G-code
post-processing rules, applied to the exported file right before it's written to disk. Only
visible with Libre Mode on; shows a warning before applying since it edits real G-code.

- **Speed override rules** — force `M220 S<percent>` from layer X to layer Y (or to the end).
  Also fixes a real Snapmaker U1/Klipper issue: WipeTower toolchanges were forcing `M220 S100`
  on every single color change, silently wiping any manual speed override — the gcode is now
  a single clean speed adjustment instead of a constant reset/re-apply.
- **Fan override rules** — force `M106 S<0-255>` from layer X to layer Y (or to the end).
- Also removed `M220 B`/`M220 R` from the U1's toolchange gcode — those are leftover from older
  Marlin-based Snapmaker machines and don't exist on the U1's Klipper firmware.

This is a **basic/quick first cut** — functional and safe to use, but still basic beta: no
temperature or nozzle Z-offset rules yet (Z-offset needs careful toolchange handling, coming
later), and fan control is a hard override, not a smart clamp. Gated to Libre Mode, opt-in only.

### Tooltip colours fixed in day/light mode (bug fix)

Hover tooltips inside **Painter Pro Mode** (F1-F4), the **ColorStitch pattern editor / `ADV…` dialog**,
and the **Bump Mapping Editor** were showing dark text on a dark background in day/light mode — hard or
impossible to read.

**Root cause:** these three surfaces share a common style helper for their windows, and that helper set
the tooltip's *text* colour for light vs. dark mode but never set the tooltip's *background* colour —
so tooltips fell back to a fixed dark background regardless of mode. In dark mode this went unnoticed
(light text on a dark background reads fine); in light mode the same helper's dark-mode-correct text
colour landed on that same fixed dark background, making it unreadable. Fixed by having the shared
helper set the tooltip background explicitly for both modes, the same way it already did for menus and
dropdowns elsewhere in the UI.

**Where it showed up:** any hover tooltip in Painter Pro Mode, the ColorStitch `ADV…` dialog, and the
Bump Mapping Editor, only when the app is in day/light mode. The **Align & Stack** gizmo was never
affected — it doesn't use this shared style path.

**Confirmed fixed** by Neotko across all three surfaces after rebuilding.

**Nothing else changes** — this is a colour-only fix with no config or slicing impact.

### NeoWave Support — contact-layer toggle shipped (new, WIP, Libre Mode only, print-pending)

**NeoWave Support** (§12 of `WIKI.md`) gets its second mechanism: a **contact-layer toggle** that
applies a Neoweave-style Z-oscillation to the object's own bridge fill directly above a support roof —
valleys touch the roof, crests stay in the air, leaving microscopic contact gaps meant to reduce
bonding force for easier support removal. It ships as **three new toggles under Support → Advanced**
(Libre Mode only): an on/off switch plus an amplitude and a period for the wave, independent of the
Sandwich system entirely — turning it on doesn't require painting anything, and it leaves the printed
colour/pattern of that surface untouched, it only modulates Z.

![NeoWave Support: hollow supports, the Wave-Huygens roof (Andersons, Sanchez, Vaneker — Twente University), and the new NeoWeaving low-contact oscillating contact layer](docs/images/NeoWave-ContactLayer.jpeg)

**Design note:** the original plan called for this to be a paintable Sandwich pass; after review it
shipped instead as a plain Support-section toggle that gates on the bridge-fill role wherever supports
meet the model, which is simpler to reason about and works whether or not the object has any Sandwich
painting at all.

**⚠️ WIP — slice-verified, not yet print-validated.** The toggle is confirmed wiring correctly end to
end (log-verified: the wave engine fires on every bridge-fill segment above a support roof, at the
configured amplitude/period) and the resulting G-code looks correct, but this specific mechanism has
never been through a real print yet — treat it as an early preview if you turn it on. Also note: Orca's
G-code **preview doesn't render the Z variation** this produces (same known limitation as the existing
Neoweave top/penu effect and ZBump) — the G-code is correct even though the on-screen preview looks flat.

**Nothing changes if you don't open it.** The toggle defaults off; nothing about supports changes
unless you turn it on.

### NeoStitch Interlock — Z-axis layer interlocking (new, WIP, ⚠️ UNTESTED — preview only, no print yet)

An early, **work-in-progress, completely untested** mechanism that interlocks consecutive layers of a
chosen wall (Outermost / Second / Third / Innermost) **without ever moving Z**: along its own path,
each layer alternates short **notch** segments (the wall deflects inward, leaving a gap at the nominal
position) and **fill** segments (the wall over-extrudes there instead), and the pattern flips phase on
alternating layers so every fill segment lands over the notch of the layer below it — a vertical
"stitch" rather than a brick-style Z offset. Configured per-region under **Strength → NeoStitch
Interlock** (stitch depth/length/period/flow, a fill-speed knob and a fill-margin knob to keep the
over-extrusion contained inside its notch).

![Z NeoStitch Interlayer Lock: alternating notch/fill segments on a cylinder wall, front view and top view showing the phase flip between layers](docs/images/NeoStitch-InterlayerLock.jpeg)

**⚠️ This is genuinely untested — do not rely on it for anything beyond curiosity/preview.** So far it
has only been checked in the **3D preview** (the alternating notches/fills are visible and register
correctly layer to layer, exactly as designed) — **it has never been through a real print**, and one
of its own sub-controls (the reduced fill-speed override) is a **known, confirmed-not-working bug**:
setting it does not currently change the resulting G-code speed for those segments, root cause not yet
found. Expect this section to expand (and the bug above to get fixed) in a later build as the feature
matures. If you turn it on before then, inspect the G-code carefully and don't expect the fill-speed
control to do anything yet.

**Nothing changes if you don't touch it.** Off by default, per-region opt-in like Fuzzy Skin.

### Bug fixes (Sandwich / ColorStitch Painter / Libre Mode)

A batch of bugs found and fixed while stress-testing the Sandwich/ColorStitch Painter pipeline on
multi-piece (Assembled) objects, plus one Libre Mode UI bug:

- **Libre Mode's floating panel could drift off-screen or stop docking.** A leftover window-layout
  state from a previous session (or a different monitor setup) could feed back into itself every time
  Libre Mode was toggled, leaving the Process panel stuck floating in the wrong place or impossible to
  dock. Fixed by validating the saved panel position against the currently connected screens before
  reapplying it, falling back to the default docked position when it doesn't fit. **Confirmed fixed by
  Neotko after rebuilding**, across two rounds (the drift itself, and a follow-up on panel size/persistence).
- **"MixedFilament Object" toggle (ColorStitch Painter → Object) didn't trigger a re-slice.** Turning
  the toggle on correctly changed the object's mode internally but never asked the slicer to recompute
  — you had to make an unrelated change elsewhere to see the effect. Fixed: the toggle now schedules a
  background re-slice the same way its sibling controls in the same gizmo already did. **Confirmed
  fixed by Neotko after rebuilding.**
- **Painting a new colour over an already-painted zone didn't refresh the 3D view.** The stroke's data
  was applied correctly, but the on-screen colour only updated after clicking anywhere in the colour
  palette (even reselecting the same colour). Fixed: finishing a paint stroke now refreshes the
  painted-colour table the same way a palette click already did. **Confirmed fixed by Neotko after
  rebuilding.**
- **Assembling painted pieces that reused the same internal paint "slot" number could print the wrong
  piece's recipe.** Each piece numbers its own painted zones independently (1, 2, 3…); if two Assembled
  pieces both happened to use, say, slot 3 for two *different* recipes, the slicer could resolve both
  to whichever piece's recipe it found first — visually correct in the Painter, wrong in the resulting
  G-code. Fixed with two changes: new pieces being Assembled now get renumbered so no two pieces ever
  reuse the same slot number going forward, and (as a safety net for objects already saved in this
  state) slot lookups at slice time now resolve against the *specific* piece that actually owns the
  paint in that spot instead of "whichever piece has that slot number first." **Confirmed fixed by
  Neotko after rebuilding**, including the edge case of two steps of two different Assembled pieces
  sitting only one layer height apart.
- **Sandwich Stickers could vanish after Assemble.** The sticker data itself survived the merge intact,
  but a housekeeping step that re-centres a newly Assembled object's geometry moved every mesh/volume
  while leaving each sticker's stored position untouched — on a typical multi-piece assembly this
  silently pushed every sticker's effective height above the top of the model, so it never printed
  again (not a data loss, a position drift out of range). Fixed: stickers now get the same re-centring
  shift applied to them as the geometry. **Confirmed fixed by Neotko after rebuilding** — a follow-up
  fix for the mirror case (a sticker not surviving **Split to Objects**, the reverse of Assemble) is
  also in this build but **not yet confirmed** by testing.
- **A sticker appearing to render on only one volume of a multi-volume object turned out to be a
  false alarm.** Re-tested after the fixes above with a multi-volume object (never Assembled — a
  single STL import with many separate mesh bodies) painted with different recipes per volume: every
  sticker stays correct, including with mismatched recipes. **Confirmed by Neotko.**
- **Splitting a painted object (Split → To Objects or To Parts) could silently lose its Sandwich
  recipes (ColorMix/PathBlend/Solid) or Stickers, with no warning.** "To Parts" always re-splits the
  mesh into fresh volumes and never carried the paint data across, no matter the object's shape. "To
  Objects" only loses it for objects made of a **single mesh volume** containing many disconnected
  triangle islands (a common way to import a multi-body STL) — a true multi-volume object (e.g. an
  Assembled multi-piece object) already carries its paint across correctly and is unaffected. A proper
  fix (re-mapping each painted triangle to the piece it ends up in) is a bigger job for a future build;
  for now, both Split commands warn up front when they're about to lose something — "Sandwich Color
  Recipes and Stickers will be lost" — using the same branded dialog style as the rest of the app.
  **Confirmed by Neotko** on both Split to Objects and Split to Parts, including that "To Objects" no
  longer warns unnecessarily on a safe multi-volume split.

---

## Beta Defaults & Notes

Unchanged from 2.3 through 2.3.6 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7,
NeoTower tower type Classic / Zigurat taper on, Penultimate top layers 0). PathBlend's start/end zone
still defaults to full-width (0.00 / 1.00) and its ramp-end default is unchanged. Bump Mapping still
ships gated off by default. The ColorStitch pattern editor's "Slow start" default only applies to a
**newly picked** blend style in the dialog — reopening a saved blend pass keeps whatever transition
shape it was saved with.

---

## Work in progress / not yet in this build

- **Textile weave (ColorStitch pattern editor)** — Twill 2/2 and Twill 3/1 don't yet apply the true
  per-layer diagonal offset; today they print as a static repeat of the base pattern, not an actual
  diagonal. The dialog flags this.
- **Bump Mapping Editor** is functional and print-validated for its supported cases but stays behind its
  expert gate — Classic wall generator unsupported by design, NeoArachne not wired up yet, and adjacent
  walls with different bump amounts can still show a visible gap.
- **Precision Adaptive Layer Height** — GUI and slicing verified, not yet print-validated. Min/max layer
  height are read-only (no per-object override), and reopening on an object with a very dense
  pre-existing profile (e.g. from the old brush) falls back to a flat start.
- **Sandwich Stickers** — wipe tower doesn't reliably toggle between a painted zone and a sticker zone
  on the same object; no dedicated tool icon yet.
- **Bottom Surface** — pattern preview still doesn't render on bottom-facing surfaces in the 3D painter
  view (slice output is correct).
- **RealColor View** — TD cache invalidation when TD is changed without a re-slice not yet fully
  re-verified.
- **MixedFilament Object mode** — pattern preview (not just colour) and a more prominent top-level
  toggle are still not implemented.
- **Variable layer height** is unlocked (under NeoTower + Libre Mode) but still flagged
  **Experimental** — prefer fixed layer height for production multi-tool Sandwich prints and review
  G-code.
- **NeoWave Support (WIP, expert/Libre Mode only)** — a wave-roof support interface, chosen via Support
  type "NeoWave" + Interface pattern "Wave". The core mechanism (roof shape Concentric/Wave, print order
  Smart/ZigZag/Monotonic, a reverse-direction toggle, and a hollow-pillar mode via a wall-loop count) is
  implemented and **print-validated for the Wave roof shape**, which closes cleanly over a hollow pillar
  — Concentric fails over a hollow pillar and is kept only as a comparison option. The second mechanism
  (a Neoweave-style contact layer, now shipped this build as its own Support → Advanced toggle — see
  "What's new" above) is slice-verified but **print-pending** — its first real print is still ahead.
  Roof thickness still borrows the existing "Top interface layers" setting rather than having its own
  key. Work is currently paused between sessions.
- **NeoStitch Interlock (⚠️ new, WIP, UNTESTED)** — see "What's new" above. Preview-verified only, never
  printed, and its fill-speed control is a confirmed no-op for now. Treat anything you see with it on as
  provisional.
- **Monotonic Interlayer Nesting** is **not yet ported** to this base.
- **World-space import** is functional at a basic level; importing as Assembled and splitting in Libre
  Mode is the recommended route.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs are not exposed yet.
- A button/label in the **Sandwich Editor tab** was reported showing black text in light mode — not yet
  confirmed whether this is the same shared-style pattern as the tooltip fix above or a separate/transient
  issue. Flag it again if it's still visible after this build.

---

## Important Reminders (read before slicing a Sandwich)

- **Use Monotonic Line for the top surface pattern** (Quality → Top surface pattern). ColorStitch only
  sequences correctly with Monotonic Line.
- **Enable Penultimate Top Layers to use penultimate effects** (Strength → Top/bottom shells), unless
  you paint a penu recipe (auto-forced per object).
- **PathBlend on the Penultimate zone is still disabled** (gradient-direction bug on multi-stair penu
  surfaces). Top-zone PathBlend — including the start/end zone and techo controls — works normally.
- **PathBlend's "techo" trades a thin cap for full coverage — on purpose.** Dragging the ramp-end
  handle to the top of the graph means the second material genuinely stops printing there. That's the
  point, not a bug, when you're fighting a translucent filament.
- **Bump Mapping is expert-only and gate-locked for a reason** — see `NEOTKOCM_RELEASE_2_35.md` before
  touching it. Read the G-code.
- **TD values are per-machine, not per-print.** Calibrate them so predicted palette swatches match
  reality.
- This is a **draft/beta note** — inspect G-code on important prints, as always.

---

## Backward Compatibility

- **Tooltip colour fix**: visual-only, no config keys or slicing behavior touched.
- **NeoWave Support contact-layer toggle**: new keys, default off, Support-section only — a scene
  without it turned on slices identically to before.
- **NeoStitch Interlock**: new per-region keys, default `Disabled` — an object that doesn't set it
  slices identically to before.
- **Sandwich/Sticker bug fixes**: the Assemble-time slot-renumbering and sticker re-centring fixes only
  change behavior for objects that previously hit the bug (colliding slot numbers, or stickers drifting
  off the model after Assemble); a merge that doesn't hit either condition produces the same result as
  before.
- **Split warning**: purely a UI confirmation dialog shown before a Split that would lose painted data
  — no config keys, no slicing behaviour change. Splitting an unpainted object, or an object whose
  paint already survives the split (true multi-volume objects), shows no dialog and behaves exactly
  as before.
- **Painter Pro Mode ungated from Libre Mode** (2.3.6): the whole F1-F4 section is visible without Libre
  Mode on, but F1-F4 stay individually opt-in and off by default — nothing changes for a paint job that
  doesn't touch them.
- **ColorStitch pattern editor redesign** (2.3.6): dialog-only — no config key renamed, no default
  pattern changed. Existing profiles, 3MF projects and saved ColorStitch passes load and print exactly
  as before; the categorized selector just reads the same `interlayer_colormix_*` keys it always did.
- **PathBlend**: fully backward compatible since 2.3.5. The start/end zone fields default to full-width
  and round-trip through a bumped blob schema (v2 → v3) — any 3MF or preset saved before 2.3.5 loads
  with the new fields defaulting to "off" (full-width ramp, same behaviour as before).
- **Bump Mapping**: additive and gated — objects/profiles that don't use it slice exactly as before,
  gate or no gate.
- ColorStitch remains a **UI rename only** from the earlier ColorMix→ColorStitch pass — config keys,
  3MF metadata and saved profiles are unchanged. Projects and presets from earlier Neotko builds load
  unchanged.
- New options default to safe/off values and don't affect existing prints unless you opt in.

---

*Neotko FullSpectrum 2.3.7 (draft, in progress) — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower /
NeoArachne / Libre Mode / RealColor View / Bump Mapping Editor*
*Features designed by Neotko · Implementation by Claude (Anthropic) · in collaboration with Snapmaker &
Radoux (FullSpectrum)*

---

**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
