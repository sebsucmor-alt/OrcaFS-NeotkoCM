## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.5 (draft) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ ** Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.

**2.3.5 is an incremental release on top of 2.3.4** (see `NEOTKOCM_RELEASE_2_34.md` and earlier notes
for the full feature set). This release has two focuses: **PathBlend gets real shape control** — you
can now decide where a gradient starts, where it ends, and whether it fully takes over the layer
instead of always fading edge-to-edge — and the **Bump Mapping Editor** (built over the last few
sessions) is documented here for the first time, including exactly how to unlock it and why it's kept
behind a deliberate expert gate. On top of that, this build is caught up with 8 more upstream
Snapmaker `main` patches. Everything remains **opt-in**: at defaults the build behaves like stock
Snapmaker Orca.

---

## What's new in 2.3.5

### PathBlend — start/end zone control + a removable ceiling ("Techos") (new)

Until now, a PathBlend gradient always ran the **full width** of the surface: lowest at one edge,
highest at the other, no way to shape it further. You can now control that shape with two new things:

- **Start / end zone.** The ramp can stay flat at its floor for a while before it starts rising, and/or
  reach its top before the surface actually ends — instead of always climbing edge-to-edge. Useful when
  you want the blend concentrated in part of the surface rather than spread across all of it.
- **Ceiling removed ("Techos").** The ramp's top height used to be capped 0.04 mm below the layer
  height, always leaving a thin sliver of the second colour on top. That reserve is gone: the ramp can
  now reach the **full layer height**, meaning the top colour can fully take over a region with **zero**
  of the second material there. This was asked for specifically to get clean, fully-opaque zones with
  **low-TD (translucent) filaments**, where even a thin cap of the wrong colour was enough to show
  through.
- **Cleaner G-code near the ceiling.** As the cap thins toward that new ceiling, it can produce
  extremely thin closing lines (a few thousandths of a millimetre) over an already-solid surface. Those
  are now dropped automatically instead of being printed — printing them was found (by inspecting real
  G-code) to create a small over-extrusion mismatch from ordinary hotend drip. This is a slicer-side
  cleanup, no action needed from you.

**Where to find it:** PathBlend passes (Half or Full) now have a new **`ADV…`** button — in the
**ColorStitch Painter**'s Pro tray, right next to the existing angle/mode controls, and in the
**Sandwich Editor**'s existing **Advanced ⚙** button for PathBlend rows. Both open the same kind of
editor: a small cross-section graph of the layer (left→right = position across the surface, bottom→top
= real height in mm) with **two draggable handles** — the low one sets where the ramp starts and how
low it sits (its floor), the high one sets where it ends and how high it reaches (its ramp end). Drag
them directly, or use the numeric fields next to the graph for exact values. The shaded regions show you
the ramp (bottom tool) and the cap (top tool, Full only) live as you drag.

**Nothing changes if you don't touch it.** With the start/end zone left at its default (full width) and
the ramp end left where it already was, PathBlend prints byte-identical G-code to before — this is a
pure extension, not a rewrite of the underlying gradient model.

> ⚠️ **PathBlend on the Penultimate zone is still restricted to Top only** (known gradient-direction bug
> on multi-stair objects, unrelated to this change — see `WIKI.md` §6e).

### Bump Mapping Editor — documented, and available behind an expert gate

The **Bump Mapping Editor** (`WIKI.md` §10) turns a grayscale image into real physical relief at slice
time — either wrapped onto an object's walls (**All** / **Painter** modes, via Planar / Cylindrical /
Spherical / Cubic projection) or modulating the height of the **top surface** itself, point by point
along every fill line (**Top** mode, "ZBump"). This has been built and print-validated over several
sessions but was never formally documented until now.

**It is intentionally hidden behind a double gate** — see the **How to enable it** section below and
the safety warning that goes with it. This is not a bug or an oversight: some option combinations in
this feature are genuinely capable of producing bad extrusion (see the warning below for specifics).
Gating it is how the feature gets to exist in this build at all without becoming a support burden or a
failed-print risk for people who don't know what they're looking at in the resulting G-code.

**Status, honestly:** wall-texture bump (All/Painter) works on **Arachne walls only** — the Classic
generator is deliberately disabled for it after a real print showed it silently over-extrudes when
adjacent walls have different bump amounts, and NeoArachne isn't wired up for it yet. Top-surface bump
(ZBump) is further along — its known over-extrusion issue in reinforcement passes was found and closed
in a recent session, confirmed by an actual print. Both are usable today by someone who reviews the
G-code; neither is a "flip it on and forget it" feature yet.

### Precision Adaptive Layer Height — point-based variable layer height (new)

A new gizmo (`WIKI.md` §11) replaces the stock "Layers editing" brush with an exact, point-based
curve editor: place control points at an exact Z and height, drag them, and shape the curve between
them with a per-segment **tension** (straight at `0`, a monotone smooth curve at `1` that can never
spike outside the two points' heights). Gated behind **Libre Mode** the same way as Align & Stack
(§7) — icon always visible in the toolbar, disabled until Libre Mode is active — and completely
independent from the stock Variable Layer Height dialog/brush, which is untouched.

While dragging or hovering a point, the affected Z-slice lights up directly on the object (a
translucent band, teal on hover / amber while dragging), the same intent as the stock tool's colour
overlay but implemented as a small 3D highlight rather than the stock texture/shader pipeline.

**Status:** GUI and slicing behaviour verified working end-to-end this session (add/drag/delete
points, tension, re-slice picks up the change), print not yet done. Two bugs were found and fixed
during verification, both structural rather than cosmetic:
- the profile the gizmo wrote was **silently discarded at slice time** because its first entry
  didn't exactly match the engine's fixed first-layer height — the bottom control point is now
  locked to that value so this can't happen;
- dragging a point didn't reliably save because the "drag finished" detection depended on catching
  a single-frame mouse-release flag that the canvas could miss — switched to ImGui's own
  active-item tracking, which doesn't have that gap.

Current limitations: min/max layer height are read-only (no per-object override yet), and reopening
the gizmo on an object with a very dense pre-existing profile (e.g. from the old brush) falls back
to a flat start rather than importing hundreds of points as control points.

### Upstream sync — caught up with Snapmaker `main` (8 patches)

This build's Snapmaker Orca base was compared against the current `main` branch upstream (which
internally already calls itself "2.3.5", though without a git tag yet) and found to be 8 commits
behind. All 8 were reviewed individually (patched, diffed, and cross-checked for overlap with anything
Neotko-specific), applied in three grouped batches, and confirmed compiling and working correctly.
None of them touch Sandwich, NeoTower, the Painter, PathBlend, or Bump Mapping — they're all upstream
Snapmaker fixes:

| # | What it fixes |
|---|----------------|
| #541 | Filament sync — corrected a TPU naming mismatch. |
| #542 | Crash guard — prevents a crash when the extruder count changes during filament sync. |
| #544 | Network test dialog — thread-safety fix. |
| #546 | Crash guard — prevents a crash on an unexpected filament-type mapping during sync. |
| #547 | Crash guard — prevents a crash in the filament temperature-mixing check. |
| #554 | Filament colour sync — two real fixes: correct ordering when filaments change, and a cover-preview refresh after removing a mixed filament. |
| #543 | Top-cover / mixed-filament temperature warnings — restores a compatibility check that a previous patch had accidentally replaced instead of complementing, adds two missing sync calls, and removes some redundant validation-clearing calls that could mask *other* unrelated errors (e.g. a bed-type mismatch). |
| — | Web bundle (`resources/web/flutter_web`) bumped 2.3.21 → 2.3.25. |

Also worth flagging transparently, even though nothing was done about it: Snapmaker is developing a
**substantial rework of the wipe tower** on an unmerged branch (partitioning the tower by filament
adhesiveness category instead of use order) — conceptually similar territory to NeoTower. It has no PR
yet and isn't merged to `main`, so there's nothing to act on. It's being watched; when it lands, it'll
get its own careful, debug-first look before anything is adopted from it (the wipe tower is the most
fragile part of this whole project).

---

## How PathBlend's new controls work (usage guide)

1. Build a PathBlend pass (Half or Full) as usual — Sandwich Editor or ColorStitch Painter, same as
   before.
2. Click **`ADV…`** next to that pass. A small graph opens showing the layer in cross-section.
3. **Drag the low (blue) handle** to set where the ramp starts rising and how low it sits — moving it
   right shrinks the flat starting zone; moving it down lowers the floor.
4. **Drag the high (orange) handle** to set where the ramp finishes and how high it reaches — moving it
   left shrinks the flat ending zone; moving it up raises the ramp's top. Push it all the way to the top
   of the graph for a full "techo": the cap disappears entirely in that zone.
5. Use the numeric fields next to the graph if you need an exact value instead of eyeballing the drag.
6. Close the popup (painter) or press OK (Sandwich Editor's Advanced dialog) — the change is live
   immediately on the next slice.

The same editor, same model, and the same resulting G-code apply whether you edit it from the Painter
or from the Sandwich Editor — they write the same underlying pass data, so switching between the two
tools mid-project is safe.

**A practical use case:** if you're blending into a very translucent (high-TD) filament and a thin cap
used to let the wrong colour show through, drag the ramp-end handle to the very top of the graph in the
zone where you want full coverage — that gives you a true 100%-top-colour region with nothing printing
underneath it there.

---

## How to enable the Bump Mapping Editor (Windows / macOS / Linux)

> 🔒 **Expert-only. Read this whole section before doing anything below.**
>
> Bump Mapping is gated on purpose, behind **two separate locks that must both be open**. This is not
> a convenience toggle — it exists so that people who don't already know how to read the G-code this
> feature produces don't stumble into it by accident. **Some option combinations in this feature are
> known to be capable of producing bad extrusion** — under-supported overhangs from wall displacement,
> silent over-extrusion on the Classic wall generator (why it's disabled there), and visible gaps
> between adjacent walls when their bump amounts differ. If you turn this on, **you are expected to
> inspect the resulting G-code yourself before trusting a print to it**, especially anything long or
> using expensive material. If that sentence doesn't make sense to you, this feature isn't for you yet
> — that's fine, it's not meant to be a mainstream toggle in this build.

**Lock 1 — Libre Mode**, the same two-step gate documented in `WIKI.md` §4a:
1. **Preferences → "Enable Neotko LibreMode (requires restart)"** → turn it on → restart the app.
2. After the restart, click the **"Neotko LM: Off"** button in the toolbar so it reads **"On"**.

**Lock 2 — a debug environment variable**, set *before* the app starts (it's read once at startup and
cached — changing it while Snapmaker Orca is already running has no effect until you restart it):

- `ORCA_DEBUG_TEXTUREBUMP=1` — unlocks the Bump Mapping Editor icon in the left toolbar (All + Painter
  wall-texture modes).
- `ORCA_DEBUG_ZBUMP=1` — **also** needed for the **Top** mode (ZBump, top-surface height relief) to
  actually apply at slice time.
- `ORCA_DEBUG_ALL=1` is a shortcut that turns on every debug channel in the build at once (Bump
  Mapping and everything else) — simpler for a one-off test, noisier (writes extra log files under
  `/tmp/`), and not something to leave set permanently.

With both locks open, the **Bump Mapping Editor** icon appears in the left-side gizmo toolbar.

### macOS

Environment variables set in a Terminal session are **not** inherited by apps launched from Finder or
the Dock — you have to either launch the app's binary directly from that same Terminal, or set the
variable persistently for GUI apps:

```bash
# One-off, this Terminal session only — launches the app directly:
export ORCA_DEBUG_TEXTUREBUMP=1
export ORCA_DEBUG_ZBUMP=1   # only if you need Top/ZBump mode too
"/Applications/Snapmaker Orca.app/Contents/MacOS/Snapmaker_Orca"

# OR — persists for anything launched from Finder/Dock until you undo it or log out:
launchctl setenv ORCA_DEBUG_TEXTUREBUMP 1
launchctl setenv ORCA_DEBUG_ZBUMP 1
# then just open the app normally. To turn it back off:
launchctl unsetenv ORCA_DEBUG_TEXTUREBUMP
launchctl unsetenv ORCA_DEBUG_ZBUMP
```

### Windows

```bat
:: One-off, this Command Prompt session only — launches the app directly:
set ORCA_DEBUG_TEXTUREBUMP=1
set ORCA_DEBUG_ZBUMP=1
"C:\Program Files\Snapmaker Orca\Snapmaker_Orca.exe"

:: OR — persists across reboots (System Properties -> Environment Variables works too):
setx ORCA_DEBUG_TEXTUREBUMP 1
setx ORCA_DEBUG_ZBUMP 1
:: a persisted variable only affects NEW processes started after you set it —
:: close and reopen any shell/Explorer session, then launch the app normally.
```

### Linux

```bash
# One-off, this terminal session only — launches the app directly:
export ORCA_DEBUG_TEXTUREBUMP=1
export ORCA_DEBUG_ZBUMP=1
./Snapmaker_Orca.AppImage    # or wherever your build's binary lives

# OR — if you launch from a desktop menu entry / icon instead of a terminal,
# add an Env= line to its .desktop file's [Desktop Entry] section:
#   Env=ORCA_DEBUG_TEXTUREBUMP=1;ORCA_DEBUG_ZBUMP=1
```

Once you're in, the panel controls themselves are documented in `WIKI.md` §10 (All / Painter / Top
modes, the handles, and the per-control reference tables). Read the **Known limitation** notes there
too — they're not fixed by unlocking the gate.

---

## Credits & how this was built

This fork is a **one-person effort** (Neotko) doing feature design, testing and print verification,
implemented in code together with **Claude (Anthropic)** as the engineering partner, on top of the
official **Snapmaker Orca** base (itself built on **PrusaSlicer/OrcaSlicer**, AGPL-3.0) — with
**Radoux/Radu**, author of the original FullSpectrum fork and now part of the Snapmaker team,
collaborating directly, and **Snapmaker** officially sponsoring the project.

**Bump Mapping Editor specifically** builds on an idea that never made it into any mainstream slicer:
**Poikilos** proposed and prototyped "displacement/bump mapping at slice time" for PrusaSlicer
([prusa3d/PrusaSlicer#8649](https://github.com/prusa3d/PrusaSlicer/issues/8649)), and **undingen**
ported that work toward OrcaSlicer ([SoftFever/OrcaSlicer#7985](https://github.com/SoftFever/OrcaSlicer/pull/7985)).
Neither PR ever merged, and both stopped at the same wall: naive cube-mapping tears visibly at face
seams, and the cylindrical projection needed to fix it was left unfinished. Full attribution, including
exactly which parts are prior art versus new work in this fork, is documented in
`docs/ATTRIBUTION_TEXTURE_BUMP.md` — nothing here copies code from either PR; the general idea (sample
a grayscale image as a slice-time displacement source) is reimplemented from scratch with this fork's
own projection math and safety systems.

---

## What's original here — ideas with no prior art found

A few pieces of this release (and the feature it builds on) aren't ports or adaptations of anything
found elsewhere — they were designed to solve a problem no other slicer's codebase or issue tracker
seems to have addressed:

- **PathBlend's per-scanline staircase gradient itself.** A real, physical Z gradient *within a single
  layer* between two materials — not a density gradient, not a dithered dual-extrusion blend, but the
  nozzle's actual height changing scanline by scanline so one material's ramp and the other's
  complementary cap sum to exactly the layer height at every point. No equivalent was found in any
  mainstream slicer at the time this was built.
- **The slope-limiter** in Bump Mapping — a per-object Z look-ahead that catches, column by column,
  when a texture-driven wall displacement between one layer and the next would imply an overhang steep
  enough that the normal support generator would never see it (because that generator works on the
  planned toolpath, not the perturbed one). No offline mesh-displacement tool needs to solve this,
  because there the overhang lands in the actual mesh and whatever slices it afterward catches it
  normally — this is a problem specific to doing the effect at toolpath level, the same territory Fuzzy
  Skin lives in.
- **The taper-to-zero-at-the-innermost-wall fix** for Bump Mapping's infill safety — displaced walls
  can otherwise invade or disconnect from infill computed on the un-perturbed geometry. The fix scales
  the effect from 100% on the outermost wall down to an exact 0% on the innermost one (which is the
  wall that actually touches infill), so infill never sees a moved wall. Two worse approaches (a fixed
  infill margin, and displacing outward-only) were tried and discarded first.

None of this is meant to overstate the obvious: this build stands on Snapmaker Orca / OrcaSlicer /
PrusaSlicer, and the Bump Mapping *idea* itself has real prior art credited above. What's original is
specific — the exact mechanisms listed here, and (per the WIKI's own credit line) the broader pattern of
this fork inventing surface-quality techniques that later become standard elsewhere, the way Neotko's
earlier "Neosanding" became "Ironing" across Cura, PrusaSlicer, Bambu Studio and OrcaSlicer.

---

## Beta Defaults & Notes

Unchanged from 2.3 through 2.3.4 (Sandwich wipe reserve 10 mm³, Sandwich purge compaction 1.7,
NeoTower tower type Classic / Zigurat taper on, Penultimate top layers 0). PathBlend's new start/end
zone defaults to full-width (0.00 / 1.00) and its ramp-end default is unchanged — **existing profiles
slice identically to before**. Bump Mapping ships gated off by default (see above) — nothing changes
for anyone who doesn't explicitly unlock it.

---

## Work in progress / not yet in this build

- **Bump Mapping Editor** is functional and print-validated for its supported cases (see status above)
  but stays behind its expert gate — Classic wall generator unsupported by design, NeoArachne not wired
  up yet, and adjacent walls with different bump amounts can still show a visible gap (nothing adjusts
  line *width* to compensate yet, only the centerline moves).
- **Precision Adaptive Layer Height** — GUI and slicing verified this session, not yet print-validated.
  Min/max layer height are read-only (no per-object override), and reopening on an object with a very
  dense pre-existing profile (e.g. from the old brush) falls back to a flat start.
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
- **Neoweaving + Monotonic Interlayer Nesting** are **not yet ported** to this base.
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
  surfaces). Top-zone PathBlend — including the new start/end zone and techo controls — works normally.
- **PathBlend's new "techo" trades a thin cap for full coverage — on purpose.** Dragging the ramp-end
  handle to the top of the graph means the second material genuinely stops printing there. That's the
  point, not a bug, when you're fighting a translucent filament.
- **Bump Mapping is expert-only and gate-locked for a reason** — see the whole section above before
  touching it. Read the G-code.
- **TD values are per-machine, not per-print.** Calibrate them so predicted palette swatches match
  reality.
- This is a **draft/beta note** — inspect G-code on important prints, as always.

---

## Backward Compatibility

- **PathBlend**: fully backward compatible. The new start/end zone fields default to full-width and
  round-trip through a bumped blob schema (v2 → v3) — any 3MF or preset saved before this release loads
  with the new fields defaulting to "off" (full-width ramp, same behaviour as before). Nothing about
  existing PathBlend passes changes unless you open the new `ADV…` editor and drag something.
- **Bump Mapping**: additive and gated — objects/profiles that don't use it slice exactly as before,
  gate or no gate.
- ColorStitch remains a **UI rename only** — config keys, 3MF metadata and saved profiles are
  unchanged. Projects and presets from earlier Neotko builds load unchanged.
- New options default to safe/off values and don't affect existing prints unless you opt in.

---

*Neotko FullSpectrum 2.3.5 (draft) — on Snapmaker Orca 2.3.4 · ColorStitch / NeoTower / NeoArachne /
Libre Mode / RealColor View / Bump Mapping Editor*
*Features designed by Neotko · Implementation by Claude (Anthropic) · in collaboration with Snapmaker &
Radoux (FullSpectrum)*

---

**Big thanks to:**
SnapMaker
Leszek
Ratdoux / Radu
Sentientstardust
dennisw
