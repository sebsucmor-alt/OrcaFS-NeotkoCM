## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.7 (draft, in progress) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ ** Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.

**2.3.7 is an incremental release on top of 2.3.6** (see `NEOTKOCM_RELEASE_2_36.md` and earlier notes
for the full feature set). This notes file is a **running draft** — it grows as sessions land, the same
way 2.3.6's did, until this version ships. So far it includes a fix for **tooltip colours in day/light
mode** across Painter Pro Mode, ColorStitch Painter and the Bump Mapping Editor. Everything remains
**opt-in**: at defaults the build behaves like stock Snapmaker Orca.

---

## What's new in 2.3.7

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
  — Concentric fails over a hollow pillar and is kept only as a comparison option. Not finished: the
  second mechanism (a Neoweave-style contact layer between roof and part, to reduce bonding force) is
  designed but not wired up yet, and roof thickness still borrows the existing "Top interface layers"
  setting rather than having its own key. Work is currently paused between sessions.
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
