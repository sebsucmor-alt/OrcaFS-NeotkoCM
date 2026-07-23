## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.8 (draft, in progress) — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ ** Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.

**2.3.8 is an incremental release on top of 2.3.7** (see `NEOTKOCM_RELEASE_2_37.md` and earlier notes
for the full feature set). This notes file is a **running draft** — it grows as sessions land, until
this version ships. Everything remains **opt-in**: at defaults the build behaves like stock Snapmaker
Orca.

---

## What's new in 2.3.8

### Expert G-code Reprocessor — by-tool mode, flow override, Z-offset (Libre Mode, PRO/expert-only)

Builds on the basic beta from 2.3.7 (layer-ranged `M220`/fan override). **Print-verified**: a real
multi-tool print (Sotogrande golf set, 5x) ran clean with these rules active, and the exported
G-code was checked line-by-line afterward — no orphaned inserts, no stray resets, boundaries land
exactly where intended.

- **By-tool mode, per rule** — every rule can now be scoped to a single tool instead of the whole
  print. When a by-tool rule's tool isn't active, its effect is automatically reverted; it
  re-applies the moment that tool comes back, all without touching the wipe tower's own toolchange
  G-code. A global fan rule and a by-tool speed rule can be active at the same time — mode is a
  property of each rule, not a single switch for the whole panel.
- **Flow override rules (`M221 S<20-200>`)** — new rule type, same by-tool/global behaviour as
  speed.
- **Z-offset rules (`SET_GCODE_OFFSET`)** — implemented (was deferred in 2.3.7 pending careful
  toolchange handling). Restore-before/apply-after every toolchange when scoped by-tool. Clamped
  to ±0.3mm, 0.01mm steps — keeps a bad value in "underextrusion / overly aggressive ironing"
  territory, not anything that could physically damage the machine.
- **No more "Apply" button** — every edit saves immediately; a master ON/OFF switch replaces the
  old per-click warning (the warning now shows once, when you turn the whole panel on). Also fixes
  a 2.3.7 rough edge where applying a rule change could force a full re-slice before Export
  G-code re-enabled.

### Expert G-code Reprocessor — visual chart editor + "Avoid Wipetower" (Libre Mode, PRO/expert-only)

The plain rule list from above is gone. The panel is now a single interactive chart, found in
G-code Preview's view-type dropdown (the same one **RealColor** lives in) as **"Gcode
Reprocessor"** — no more separate floating window.

![Gcode Reprocessor — GLOBAL/BY TOOL toggle, per-tool bars, a Flow rule with Avoid Wipetower's gold glow active](docs/images/ReProcesor.png)

- **GLOBAL / BY TOOL toggle** switches the chart between rules that apply everywhere and rules
  scoped to one tool, each with its own column (**T0, T1, T2, ...**).
- **Drag either endpoint** of a rule's bar to change its layer range (dragging the top endpoint
  all the way up snaps to "to the end of the file"); **click its value badge** (in the text
  summary below the chart) to type an exact number; **right-click empty space** to add a rule,
  choosing its type from a color-coded menu; **right-click an existing point** to delete that
  rule or toggle "Avoid Wipetower."
- A rule whose range no longer exists (object resized shorter after the rule was created) shows
  its dot pinned to the chart's edge in gray rather than drawing off-screen — still fully
  draggable and deletable from there.
- **"Avoid Wipetower"** — the limitation noted below is fixed. Any rule can now independently
  exclude wipe-tower G-code from its active range (shown as a permanent gold glow on its bar),
  splitting the range around a purge that falls in the middle rather than skipping the whole
  thing. **G-code-verified**: checked directly against an exported file with the option on —
  confirmed the rule's insert/rewrite never lands inside or next to a wipe-tower block.

(Historical note, now resolved: a first pass of this feature found that a rule's range could land
inside wipe-tower purge G-code on machines where the tower fires between layers for drip control,
not just at toolchanges — confirmed happening harmlessly on a flow rule during the print-verified
test above. "Avoid Wipetower" above is the fix.)

### Painter Pro Mode — Surface depth: project a painted design INTO the object (+ per-color controls)

Paint a design on the top (or bottom) of an object with the stock Color Painting gizmo, and the new
**Surface depth** field in Pro Mode extends it into the object as **solid material of the painted
color**, following the painted silhouette exactly, for as many layers as you choose (0–20 extra
layers past the painted surface; 0 = off). The shape projects **straight** down/up — same size
layer after layer, only trimmed where the object's real geometry changes — and genuinely prints as
solid infill of that color, surrounded by the layer's normal sparse infill. Areas that were already
solid (top shells, Sandwich internals) are never double-counted or overwritten.

![Surface depth — the same painted design projected straight into two test cubes, seen in preview](docs/images/Paint-Depth.gif)

And with the new **Per color** checkbox, "Extra walls" and "Surface depth" switch from one global
value to a **per-color table** — one row per filament with its color swatch plus a Walls (0–8) and
a Depth (0–20) field, 0 = use the global value. A silver logo can get 8 extra walls and 5 layers of
depth while the red mark next to it gets 2 and 20. Clicking a swatch in the table also selects that
color for painting, with the same highlight as the filament strip at the top.

![Per color — the Pro Mode table assigning different Walls and Depth per painted color](docs/images/Paint-Depth-Basic.png)

> **Known edge case (not a Surface depth bug — suspected stock-pipeline gap):** a MixedFilament
> blend that **ends in the same color as the object's own filament** skips its whole blend chain —
> the top layer is (correctly) not painted, but the lower blend layers, where the mix would resolve
> to a different color, are not generated either. Workaround: don't end the blend on the object's
> base color.

### Precision ALH — "Adapt to Color" + Slope Pattern Recolor (Libre Mode, WIP/experimental)

Two new color-aware extensions for the **Precision Adaptive Layer Height** gizmo, born from one
observation: layer height and MixedFilament color are not independent — the height you slice at
changes how the color reads on the printed part.

**Adapt to Color** (checkbox in the gizmo): analyzes the object's color setup and shades the
height editor with the ranges where that color actually works. The red zones mark heights that
break it: a **pattern-resolution ceiling** (above ~the mix band, a Cycle/gradient pattern gets too
coarse to read as a blend) and — only where the object really has a Sandwich top surface — a
**color-fidelity floor** driven by the filaments' TD (translucency): thinner than this and a
translucent pass washes out over what's below. A green line marks the optimal height per Z, and
**Snap to optimal** rewrites your whole curve to it in one click. Dragging stays free — the
guidance is visual (an out-of-range point turns orange) and the profile is hard-sanitized only at
commit, so the slicer never receives a color-unsafe height.

**Slope Pattern Recolor** (checkbox "Slope recolor", opt-in): fixes a problem no stock slicer
addresses. On a sloped surface, each layer's contour steps inward and the step ledge exposes the
**interior perimeter rings** — which normally print in whatever color the layer happens to use, so
a clean MixedFilament banding turns into noise exactly where the geometry gets interesting. With
the toggle on, the gizmo detects the slope bands of the mesh, computes how many interior rings each
band exposes at your committed layer heights (`d = layer_height × tan(slope)` vs. perimeter
width), and stores a per-object recolor plan. At slice time the engine applies it: the **external
perimeter keeps the pattern's own per-layer alternation** (that rhythm IS the pattern), while the
exposed interior rings print a side-by-side mix of the recipe's components chosen (ΔE2000) to match
the recipe's intended blend color — the ledge fills with the mix instead of a random solid. Works
with Cycle, gradients and manual patterns; verified against RealColor, the improvement on sloped
mixed-color surfaces is dramatic. The violet shading in the height editor shows where slopes expose
rings at the current heights, and the panel reports the count plus the suggested ring colors for
the focused point.

![Adaptive Slope MixedFilament — the same model in RealColor gcode preview (TD values): default on the left, everything on + Sandwich auto TD on the right](docs/images/Adaptive-Slope-MixedFilament.png)

Best results on surfaces with one dominant slope; objects mixing two very different slopes in the
same height range currently share one plan per band (the steeper one wins) — refinement planned.
Everything is per-object, stored in the project (3mf), undoable, and fully off unless you opt in.

> WIP status: gcode-verified and RealColor-verified; broad print testing still in progress.

### NeoTower — fix: "Variable layer height (Experimental)" now persists

The experimental NeoTower option was missing from the preset serialization list, so it silently
reset to off on every project load. It now saves and loads with the project/preset like every other
NeoTower option.

### NeoArachne — hybrid Classic/Arachne wall generator: Working 1.0 (Libre Mode, PRO/expert-only)

`wall_generator` gains a third option alongside Classic and Arachne: **NeoArachne**, a hybrid that
lets Classic and Arachne each do what they're best at — Classic's constant width on the outer wall,
Arachne's variable-width beading on everything inside it. Extensively print-tested over the past
month; the controls aren't the friendliest yet, but the math underneath is solid and reliable.

- **Wall source per feature** — independent selectors for outer wall / inner walls / gap fill, each
  choosing Classic, stock Arachne, or "Arachne (NeotkoEdge)" (the improved variant below). Combos
  that would break the beading solver (e.g. Arachne outer + Classic inner) are blocked by a
  validator that auto-aligns them instead.
- **Edge Closure** — S3D-style thin-wall handling: allowed overlap, min/max bead width, min feature
  size, and an option to preserve short closure tails that stock Arachne normally discards.
- **NeotkoEdge beading (pin outer width)** — stock Arachne already keeps the outer wall a constant
  width once a region has 3+ beads; **Pin Outer Wall Width** (new this release, on by default)
  extends that same constant-width behaviour down to the thinnest regions (1-2 beads), where stock
  Arachne otherwise lets the outer width "breathe" with thickness. Disable it to fall back to stock
  behaviour there.
- **Wall Count Stability (bead-count hysteresis)** — deadbands the 1↔2, 2↔3... bead-count
  transitions so borderline-thickness strokes stop flickering between counts along their length.
- **Wall Blend Distance (transition filter distance)** — the SkeletalTrapezoidation transition
  smoothing distance is now user-configurable instead of a stock 100mm hardcode, so small parts
  (text, thin features) get proportionally sharper transitions.

All NeoArachne controls live in the existing "Wall generator" section and only appear once
`wall_generator` is set to NeoArachne with Libre Mode on.

### Snapmaker upstream sync — audited through 2026-06-29 (`origin/main` @ `c98c6d3`)

Full audit of the 28 commits Snapmaker landed on `main` between 2026-06-17 and
2026-06-29. Result: the engine is at parity, no outstanding patch pending. Two small
wipe tower bugfixes were genuinely missing and are now applied (Classic + NeoTower
paths):

- **Ramming distance clamp** (`toolchange_Unload`) — the ramming segment length is now
  capped to the remaining segment budget (`std::min(x - e_done, remaining)`) instead of
  running unclamped. Matches upstream commit `4143994c9` ("revert wipe tower
  filament", PR #501).
- **Gap-wall travel simplification** (`toolchange_Change`, Classic mode only) — removed
  a special-case nozzle travel path tied to `wipe_tower_wall_gap`; NeoTower-driven
  towers were unaffected either way (they already forced this off). The `wipe_tower_wall_type`
  Rib/Cone wall-generation feature itself is untouched and still fully functional —
  only the travel-routing nuance from this one PR was reverted, matching upstream.

Filament Sync v2 and the Top Cover / filament temperature-mixing detection feature
were also checked — both already present in this build (Filament Sync v2 verbatim,
Top Cover as an independently-built equivalent), no porting needed.
