## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.6 (draft, in progress) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ ** Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.

**2.3.6 is an incremental release on top of 2.3.5** (see `NEOTKOCM_RELEASE_2_35.md` and earlier notes
for the full feature set). This notes file is a **running draft** — it grows as sessions land, the same
way 2.3.5's did, until this version ships. So far it includes a redesign of the **ColorStitch pattern
editor**, the standard MMU painter's new **Pro Mode** toolset, and early work on **NeoWave Support**.
Everything remains **opt-in**: at defaults the build behaves like stock Snapmaker Orca.

---

## What's new in 2.3.6

### Painter Pro Mode — full F1-F4 toolset shipped, no longer gated behind Libre Mode (new)

The standard Snapmaker Orca **MMU painter** (the plain multi-material brush, not the fork's 3D
ColorMix Painter) gets a **`Pro Mode`** section with four tools, all shipped and now a **permanent
part of the build** — previously this whole section required Libre Mode to be on; it no longer does.

- **F1 — Paint perimeters only.** Restricts a painted zone to a perimeter-width ring instead of
  flooding the whole cross-section, using the existing `mmu_segmented_region_max_width` mechanism.
- **F2 — Extra walls in the painted zone.** Adds extra perimeter loops just to the painted region,
  independent of the rest of the object's wall count.
- **F3 — Brush precision.** A slider that scales how finely a stroke subdivides the mesh, so small
  brushes paint small areas instead of ballooning into much larger triangles.
- **F4 — Rectangle and free-form polygon masks (new this release).** Drag a rectangle, or click to
  place polygon vertices (click near the first vertex to close, click-drag an existing vertex to
  move it, right-click to cancel) — both show a live on-screen overlay while you build them and paint
  the enclosed area in one shot on release/close.

**Bug fixed alongside:** F1 + F2 together weren't sizing the painted ring wide enough for the extra
walls F2 asked for, so F2 silently did nothing whenever F1 was also on — fixed by having F1's width
calculation account for F2's extra wall count, kept in sync if you change "Extra walls" afterward.

**Where to find it:** Prepare tab → paint on a MMU-capable object → the paint gizmo's left panel →
**`Pro Mode`** (always visible now, was Libre-Mode-only before).

**Nothing changes if you don't open it.** F1-F4 are all off by default; a paint job that doesn't touch
Pro Mode slices identically to stock behavior.

### ColorStitch pattern editor — categorized pattern styles + textile weaves + unified preview (new)

The **`Edit gradient…`** / **`ADV…`** dialog that configures a ColorStitch pass — the same dialog
whether you open it from the **Sandwich Editor** or the **ColorStitch Painter**'s Pro tray — has been
redesigned. Instead of one flat list of controls, you now pick a **Pattern style** first, and only that
style's controls show:

- **Custom pattern** — build a digit string by hand from filament buttons, same as before.
- **MixedFilament recipe** — pick a MixedFilament combination as the whole pattern.
- **Textile weave (new)** — ready-made weave structures (Plain, Twill 2/2, Twill 3/1, Satin 5,
  Houndstooth) generated from two colours you pick, based on real fabric construction. Houndstooth
  writes the correct half automatically for Top vs Penultimate and shows you the matching string for
  the other surface.
- **Smooth blend — 2 / 3 colours** and **Stripes — manual band sizes** — same controls as before,
  reorganized into their own panel; new blends now default their transition shape to **Slow start**
  instead of Even.

A single **preview** — a strip plus a square "how it will look" sample at the pass's fill angle — is
now always visible and reflects **whatever style is active**, including custom strings and
MixedFilament recipes, which previously had no colour preview at all. Blends stretch to fill the square
once, the way the slicer actually spreads a gradient across a surface; patterns tile as a repeating
motif, the way they actually print.

**Where to find it:** unchanged — **Sandwich Editor** → a pass set to **ColorStitch** → **Edit
gradient…**, or **ColorStitch Painter** Pro tray → **`ADV…`**. See `WIKI.md` §1b.

**Nothing changes if you don't touch it.** No config key was renamed and no default pattern changed —
this is a dialog-only redesign; existing profiles, 3MF projects and saved passes load and print exactly
as before.

---

## Beta Defaults & Notes

Unchanged from 2.3 through 2.3.5 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7,
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
- **NeoWave Support (new, WIP, expert/Libre Mode only)** — a wave-roof support interface, chosen via
  Support type "NeoWave" + Interface pattern "Wave". The core mechanism (roof shape Concentric/Wave,
  print order Smart/ZigZag/Monotonic, a reverse-direction toggle, and a hollow-pillar mode via a wall-
  loop count) is implemented and **print-validated for the Wave roof shape**, which closes cleanly over
  a hollow pillar — Concentric fails over a hollow pillar and is kept only as a comparison option. Not
  finished: the second mechanism (a Neoweave-style contact layer between roof and part, to reduce
  bonding force) is designed but not wired up yet, and roof thickness still borrows the existing
  "Top interface layers" setting rather than having its own key. Work is currently paused between
  sessions, picking back up with the contact-layer mechanism next.
- **Monotonic Interlayer Nesting** is **not yet ported** to this base.
- **World-space import** is functional at a basic level; importing as Assembled and splitting in Libre
  Mode is the recommended route.
- **NeotkoEdge** is selectable as a wall source, but its extra tuning knobs are not exposed yet.
- Known pre-existing bug: tooltip colours are hard to read in day/light mode in both the Sandwich
  Editor and the Sandwich Painter gizmo.

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

- **Painter Pro Mode ungated from Libre Mode**: the whole F1-F4 section is now visible without Libre
  Mode on, but F1-F4 stay individually opt-in and off by default — nothing changes for a paint job that
  doesn't touch them.
- **ColorStitch pattern editor redesign**: dialog-only — no config key renamed, no default pattern
  changed. Existing profiles, 3MF projects and saved ColorStitch passes load and print exactly as
  before; the new categorized selector just reads the same `interlayer_colormix_*` keys it always did.
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

*Neotko FullSpectrum 2.3.6 (draft, in progress) — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower /
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
