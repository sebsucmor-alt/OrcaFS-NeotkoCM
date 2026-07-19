## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development will be conducted in close collaboration with Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum the now official part of the Snapmaker team. So from v1.9 forward expect big things!
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

# How does it work, what is this?

> Check https://sebsucmor-alt.github.io/OrcaFS-NeotkoCM/ For a Basic tutorial and Introduction to the world of delicious Color Sandwiches!

# Snapmaker Orca — Neotko FullSpectrum Feature Pack · User Guide

> Features conceived and designed by **[Neotko](https://github.com/neotko)** — inventor of *Neosanding*, now known as **Ironing** in OrcaSlicer, PrusaSlicer, Bambu Studio and Cura.

This is the **Neotko FullSpectrum** feature pack ported on top of the official **Snapmaker Orca 2.3.4** base. It adds a set of surface-quality, colour-blending, wall-generation and workflow features. Everything here is **opt-in** — with the new options left at their defaults, Snapmaker Orca behaves like the stock build. This guide explains what each feature does and how to use it; no programming knowledge required.

> **About this build.** This is a feature pack meant for **pick & choose** — each block (ColorStitch, Penultimate, Libre Mode, NeoArachne, NeoTower, Align & Stack) can be adopted independently. Some surfaces are marked **(WIP)**: they are usable but still being refined, or not yet wired in this base. Review your G-code before long prints.

---

## Philosophy — the Sandwich

Everything on the colour side of this pack revolves around one idea: a **Sandwich** of passes you build yourself.

A normal slicer treats the top of your part as one thing — one filament, one pattern, printed once. The Sandwich breaks that open. Each surface zone (the **Top layer** and the **Penultimate layer**) becomes a small **stack of passes**, and you decide what each pass is:

- A plain **Solid** pass (a normal solid fill with its own tool, angle and Z) — stack two or three of them and you get the old *MultiPass* glazing/cross-hatch effect.
- A **ColorStitch** pass that decides the filament for each fill line — stripes, dithered blends, hard bands or custom patterns.
- A **PathBlend** pass — a continuous gradient that fades between filaments across the surface.

The two zones together form the Sandwich over your fill. You build it in one place — the **Sandwich Editor** (Quality → Surface ColorStitch → **Sandwich editor…**) — drag dividers to split the layer height between passes, save the result as a profile, and reuse it or **paint** it onto specific faces with the ColorStitch Painter (§6).

The goal is not "presets that work" — it is a **playground**. Mix, stack, experiment, save profiles, paint them, and see what surface comes out.

Beyond surface effects the pack also adds a **new wall-generation engine** — **NeoArachne** (§8) — a **post-slice wipe-tower planner** — **NeoTower** (§9) — and a set of professional/experimental unlocks under **Libre Mode** (§4), including the **Align & Stack** gizmo (§7).

---

## Table of Contents

1. [Surface ColorStitch — the Sandwich Editor](#1-surface-colorstitch--the-sandwich-editor)
   - 1a. [The pass stack & pass kinds](#1a-the-pass-stack--pass-kinds)
   - 1b. [ColorStitch pass — per-line color patterns](#1b-colorstitch-pass--per-line-color-patterns)
   - 1c. [PathBlend pass — smooth gradient](#1c-pathblend-pass--smooth-gradient)
   - 1d. [Zone and filament filters](#1d-zone-and-filament-filters)
   - 1e. [Filament & TD preview](#1e-filament--td-preview)
   - 1f. [Line Distribution Mode](#1f-line-distribution-mode)
   - 1g. [ColorStitch Studio — palette generators](#1g-colorstitch-studio--palette-generators)
2. [Neoweaving + Monotonic Interlayer Nesting (WIP)](#2-neoweaving--monotonic-interlayer-nesting-wip)
3. [Penultimate Top Layers](#3-penultimate-top-layers)
4. [Libre Mode](#4-libre-mode)
   - 4a. [Enabling Libre Mode](#4a-enabling-libre-mode)
   - 4b. [Floating objects](#4b-floating-objects)
   - 4c. [Assembled Boolean mode](#4c-assembled-boolean-mode)
   - 4d. [Per-volume XY compensation](#4d-per-volume-xy-compensation)
   - 4e. [Copy / Paste Process Settings](#4e-copy--paste-process-settings)
   - 4f. [Assembled Parts — full options](#4f-assembled-parts--full-options)
   - 4g. [World-space import (WIP)](#4g-world-space-import-wip)
   - 4h. [Internal-bridge handling](#4h-internal-bridge-handling)
5. [S3DFactory — Simplify3D project import](#5-s3dfactory--simplify3d-project-import)
6. [Surface Effect Profiles & ColorStitch Painter](#6-surface-effect-profiles--colorstitch-painter)
   - 6a. [Saving and managing profiles](#6a-saving-and-managing-profiles)
   - 6b. [The ColorStitch Painter gizmo](#6b-the-colorstitch-painter-gizmo)
   - 6c. [Palette groups, slot cap & Save All](#6c-palette-groups-slot-cap--save-all)
   - 6d. [Painter mode at slice time](#6d-painter-mode-at-slice-time)
   - 6e. [Profile persistence and 3MF round-trip](#6e-profile-persistence-and-3mf-round-trip)
   - 6f. [Weave preview on the painted surface](#6f-weave-preview-on-the-painted-surface)
   - 6g. [MixedFilament Object mode (Beta)](#6g-mixedfilament-object-mode-beta)
7. [Align & Stack — align and stack objects](#7-align--stack--align-and-stack-objects)
8. [NeoArachne — alternative wall generator](#8-neoarachne--alternative-wall-generator)
   - 8a. [Turning it on](#8a-turning-it-on)
   - 8b. [Per-feature engine choice](#8b-per-feature-engine-choice)
   - 8c. [Edge Closure controls](#8c-edge-closure-controls)
   - 8d. [Preview Lab](#8d-preview-lab)
9. [NeoTower — post-slice wipe tower](#9-neotower--post-slice-wipe-tower)
   - 9a. [Tower type](#9a-tower-type)
   - 9b. [Zigurat taper](#9b-zigurat-taper)
   - 9c. [Sandwich purge compaction](#9c-sandwich-purge-compaction)
   - 9d. [Sandwich wipe reserve](#9d-sandwich-wipe-reserve)
   - 9e. [Adaptive layers × multi-tool × Sandwich (WIP)](#9e-adaptive-layers--multi-tool--sandwich-wip)
10. [Bump Mapping Editor — texture-driven wall & top-surface relief](#10-bump-mapping-editor--texture-driven-wall--top-surface-relief)
    - 10a. [All mode — object-wide wall texture](#10a-all-mode--object-wide-wall-texture)
    - 10b. [Painter mode — per-zone texture](#10b-painter-mode--per-zone-texture)
    - 10c. [Top mode — ZBump, top-surface height-map relief](#10c-top-mode--zbump-top-surface-height-map-relief)
11. [Precision Adaptive Layer Height — point-based layer height curve](#11-precision-adaptive-layer-height--point-based-layer-height-curve)
12. [NeoWave Support (WIP) — Wave-Huygens roof + hollow pillar](#12-neowave-support-wip--wave-huygens-roof--hollow-pillar)
13. [Painter Pro Mode — precision tools for the stock Color Painting gizmo](#13-painter-pro-mode--precision-tools-for-the-stock-color-painting-gizmo)
    - 13a. [Brush precision](#13a-brush-precision)
    - 13b. [Paint perimeters only + Extra walls](#13b-paint-perimeters-only--extra-walls)
    - 13c. [Rectangle & Polygon masks](#13c-rectangle--polygon-masks)
14. [NeoStitch Interlock (WIP, ⚠️ UNTESTED) — Z-axis layer interlocking](#14-neostitch-interlock-wip--untested--z-axis-layer-interlocking)

---

## 1. Surface ColorStitch — the Sandwich Editor

The **Sandwich Editor** is where you build the per-surface effect stack. It lives under **Quality → Surface ColorStitch → Sandwich editor…**.

> **Naming.** The feature family is called **ColorStitch** in the UI. You will still see *ColorMix* in a few internal places — config keys and 3MF data were not renamed, so old projects keep working. They refer to the same system.

### How the editor is laid out

The dialog has two columns — **Top layer** and **Penultimate layer**. Each is an independent **stack of 1–3 passes**, chosen with the **Passes** selector. The passes are stacked as thin virtual sub-layers inside the same nominal layer; you **drag the dividers** between them to split the layer height (each pass gets its own Z share). Per pass you can also move it **up/down** in the stack.

Each pass exposes:
- a **kind** (see §1a),
- a **Z mm** height box,
- an **angle** box (`-1 = auto`; scroll the wheel over the box to rotate). For a PathBlend pass this same box is repurposed to show **ramp end** (the top height of the ramp, in mm) instead — PathBlend's own fill angle isn't edited from this box (see §1c),
- an **Advanced ⚙** button to open that pass's detailed settings (a `*` marks non-default values) — for PathBlend this is where the start/end zone editor lives (§1c).

A single **Perimeter override** checkbox per zone *clones the walls into every Solid pass* — useful when you want the perimeter reprinted with each glaze.

At the bottom are the **Filament & TD** preview (§1e) and the **ColorStitch Studio** (§1g).

---

### 1a. The pass stack & pass kinds

Each pass in a zone is one of:

| Kind | What it does |
|------|--------------|
| **None** | Empty slot — no pass here. |
| **Solid** | A normal solid fill pass with its own tool, angle and Z share. **Stack 2–3 Solid passes** and you get the classic *MultiPass* effect: cross-hatch (two angles, two colours), a glaze pass over a base, or an optical colour blend. |
| **ColorStitch** | Decides the filament line by line across the surface — stripes, dithered blends, hard bands or a custom pattern. Configured in the **Edit gradient…** dialog (§1b). |
| **PathBlend Half** | A gradient pass with **no complementary cap** — the ramp climbs from its floor straight up, one tool only (§1c). |
| **PathBlend Full** | A gradient pass with a ramp **and** a complementary cap in a second tool, filling the rest of the layer height (§1c). |

**MultiPass = multiple Solid passes.** There is no separate "MultiPass" button anymore — you simply add Solid passes and split the height between them with the dividers. Aim for the height shares to add up to the full layer for full coverage.

PathBlend is always a single full-height gradient — when you pick a PathBlend kind the stack collapses to one pass.

---

### 1b. ColorStitch pass — per-line color patterns

A **ColorStitch** pass decides which filament prints each fill line. Open its **Edit gradient…** button to configure it — the same editor also opens from the **ColorStitch Painter**'s Pro tray (**`ADV…`** button), so whichever entry point you use, you get the identical dialog.

**Pattern style** picks *where the pattern comes from*. Only one style is active at a time — they're mutually exclusive by design (a custom string, a MixedFilament recipe, a weave, a blend and a set of stripes can't all drive the same pass at once), and a one-line note under the selector always says what the active style does, shown in amber when it overrides everything else:

| Style | What it produces |
|-------|------------------|
| **Custom pattern** | Click filament buttons to build a digit string by hand; the slicer loops it across lines (line 1 → first digit, etc.). For exact, repeating stripes you design yourself. `Clear` / `⌫ Undo digit` / `Invert` live here. |
| **MixedFilament recipe** | Pick one of your MixedFilament combinations (e.g. *F1+F2 50/50*) — it becomes the whole pattern. Only shown when MixedFilament virtual digits are enabled. Switching to another style clears the active recipe. |
| **Textile weave** | Ready-made weave structures generated for you — see below. |
| **Smooth blend — 2 colours** | Two filaments distributed across the surface with a percentage split, dithered so the transition looks smooth. The most common choice. |
| **Smooth blend — 3 colours** | Three filaments at configurable percentages; the middle colour concentrates in the centre. |
| **Stripes — manual band sizes** | Explicit band counts: N lines of Colour 1, M of Colour 2, … repeating. |

**Colours used** — pick **Color 1–4** (each maps to a loaded filament). Only shown for the two blend styles and Stripes — Custom/MixedFilament/Weave patterns already carry their own colours in the digit string.

**Preview** — a horizontal strip plus a square "how it will look" sample, drawn at the pass's infill angle, always visible for **whatever style is active**, including Custom strings and MixedFilament recipes (previously these had no colour preview at all). Blends **stretch to fill the square once**, the same way the slicer spreads a gradient across the whole surface; patterns (Custom, MixedFilament, Weave, Stripes) **tile** as a repeating motif, matching how they actually print.

**Blend controls** (Smooth blend styles):

| Control | What it does |
|---------|--------------|
| **How much Color 1 / Color 2** | The percentage split. (In 3-colour, *Color 3 fills the rest* automatically.) |
| **Color overlap (soft ← hard zones)** | How much colours bleed into each other in 3-colour blends. |
| **Transition shape** | Even (same density everywhere) · **Slow start** (the default when you first pick a blend style) · Slow end · S-curve (smooth start & end) · Custom shape (set **γ**) · Hard step. |
| **Skip tiny areas** | Surfaces with fewer than N fill lines use Color 1 only. |
| **Invert direction ⇆** | Reverses the per-line sequence (blend and stripe styles; Custom/Weave have their own `Invert` button instead). |

**Textile weave** — classic weave structures from real fabric construction, generated as pattern strings from two colours you pick:

| Weave | Pattern | Look |
|-------|---------|------|
| **Plain (tafetán)** | `1212` alternation | Balanced 50/50 mix, neither colour dominates. |
| **Twill 2/2 (sarga)** | `1122` | Diagonal weave — the true per-layer diagonal offset isn't implemented yet, so today it prints as a static repeat (the dialog flags this). |
| **Twill 3/1** | `1112` | One colour dominates, the other marks a diagonal (same offset caveat). |
| **Satin 5 (satén)** | `11112` | One colour covers ~80% of the surface; the other appears as scattered accent points. |
| **Houndstooth** | `1122` top / `2211` penultimate | The classic pattern only appears where Top and Penultimate cross at 90° — the dialog writes the correct half automatically for the surface you're editing and shows you the matching string for the other one. |

Pick **Colour A** / **Colour B** to substitute into the weave; if you set both to the same filament, Colour B is nudged to the next one automatically (a one-colour weave isn't a weave). **Edit as custom pattern…** copies the generated string into the Custom style for hand-tweaking.

**Stripes controls** — for each of Color 1–4: a swatch (following the colour picked above) and a **lines:** count. Bands repeat `[Color 1 × lines, Color 2 × lines, …]` until the surface is filled; 0 lines skips that colour.

**Print options** (apply to every style):

| Control | What it does |
|---------|--------------|
| **Infill angle override** | `-1 = Auto`. Scroll the **mouse wheel over the pass preview bar** to rotate it live — the bar's stripes rotate with it. A **fixed** angle (≥ 0) is now honoured **exactly** in the G-code (the per-layer fill rotation is locked out for that pass), so the print keeps the angle you set. `-1 = Auto` lets the slicer alternate per layer (uniform finish, but the orientation won't match a static preview). |
| **Gradient repetitions** | `1 = single`; higher repeats the pattern across the surface. |
| **ColorStitch min. line length** | Fill lines shorter than this (mm) are skipped, so they keep the surrounding colour and avoid toolchanges on tiny segments. Default **0** (don't skip). |

An estimate of how many lines a 60×60 mm surface would have at your filament width is shown next to the strip preview.

**⚠️ Required fill pattern: Monotonic Line.** ColorStitch only works correctly with the **MonotonicLine** top-surface fill pattern, because the slicer pre-splits the surface into individual straight paths before assigning tools. Monotonic and Rectilinear look fine on simple convex shapes but mis-sequence on complex objects. Set **Quality → Top surface pattern → Monotonic Line** before using ColorStitch.

---

### 1c. PathBlend pass — smooth gradient

A **PathBlend** pass creates a **continuous gradient** across the surface: one filament dominates at one edge, another at the opposite edge (Full mode), with the two flows changing in exact proportion path-by-path in between — a real, physical Z gradient within a single layer, not a dithered pattern. Choose **Half** (ramp only, no cap) or **Full** (ramp + complementary cap) when picking the kind.

**Basic controls**, on the pass's own row:
- **floor** — the ramp's starting height (mm), at the low edge of the surface.
- **ramp end** — the ramp's top height (mm), at the high edge. In **Half** mode this is locked to the full layer height (no cap exists to fill the rest). In **Full** mode you can drag it all the way up to the layer height too — that leaves **zero** of the cap's colour in that area ("techo"), useful when a translucent (high-TD) filament needs full opaque coverage instead of a thin sliver of the wrong colour on top.
- **Mode** — a cycling button through **Linear / Ease In / Ease Out / Ease In-Out**, shaping how quickly the ramp climbs (only used by the older Sandwich-Editor-only engine path; the per-scanline staircase engine that actually prints today ramps linearly in `t` before the start/end zone below is applied).

**`ADV…` — start/end zone editor.** Opens a small cross-section graph of the layer: the horizontal axis is position across the surface, the vertical axis is real height in mm. Two draggable handles set the shape:
- the **low handle** — where the ramp starts to rise, and how low its floor sits;
- the **high handle** — where the ramp finishes rising, and how high its top reaches.

By default the ramp spans the full surface edge-to-edge (the classic behaviour). Drag the low handle right to add a flat "start zone" before the ramp begins climbing; drag the high handle left to add a flat "end zone" after it's done. This is the same editor, same model, in both the **Sandwich Editor**'s `ADV…`/Advanced button and the **ColorStitch Painter**'s Pro tray — whichever one you use, they write the same pass data.

The gradient runs across the build-plate **Y axis** — rotate the object to change direction. PathBlend works best on surfaces with many fill lines; on small surfaces the gradient is coarse. It shares the **Line distribution mode** (§1f) — if a gradient looks broken across holes, try **LaneQuant** or **DirCluster**. In the **ColorStitch Painter** specifically, PathBlend also exposes its own **fill angle** field (`-1 = auto`, or a fixed 0–359° override) next to the Mode button — the Sandwich Editor doesn't have a separate control for this and leaves it on auto.

> ⚠️ **PathBlend is the most fragile part of the engine.** Its per-scanline staircase model — one physical print-height step per fill line, each step resting on the layer below and its neighbours — is validated and must not be disturbed by unrelated changes. The start/end zone and floor/ramp-end controls above are an intentionally **safe** extension of that model: left at their defaults they reproduce the exact same G-code as before. A future, more ambitious idea — letting the ramp rise **and fall** within one pass instead of always climbing — was considered and set aside for now, because the current staircase can't do that without risking unsupported overhangs at the print head; it would need a different, multi-pass engine (closer to the Bump Mapping Editor's approach, §10) to do safely.

---

### 1d. Zone and filament filters

These apply to whichever passes are active on each zone.

**Zone — All surfaces vs. Topmost only.** On many models the "top surface" appears on every horizontal face. *All surfaces* applies the effect everywhere; *Topmost only* restricts it to the single highest horizontal surface. Available independently for Top and Penultimate — use *Topmost only* on stepped objects to colour just the very top.

**Filament filter (0–16).** Apply the effect only to regions assigned to a given filament number. `0` = no filter. Example: red body (filament 1) + white logo (filament 2) → set `1` to leave the logo untouched.

---

### 1e. Filament & TD preview

The **Filament & TD** panel visualizes how passes combine optically. Each filament has a **Transmission Density (TD)** value — **low TD = opaque, high TD = translucent**:

| TD range | Type |
|----------|------|
| 0.1 – 0.5 | Highly opaque — 1–2 passes fully cover the lower colour |
| 0.5 – 3.0 | Opaque-translucent — some lower layer shows through |
| 3.0 – 7.0 | Translucent — needs several passes to block |
| 7.0 – 10+ | Highly translucent — lower colour almost always visible |

Four **TD sliders** (one per filament) are saved **per machine** (`neotko_td_1..4`) — they describe your actual filaments, not the print profile. The preview shows the blended Top result, the Penu result, and the final on-print result (penu showing through the top by opacity), plus a `transmit=` readout.

> The inverse colour-match ("find the recipe closest to a target colour") lives in **ColorStitch Studio → Target + Match ▸** (§1g), using ΔE2000.

---

### 1f. Line Distribution Mode

This controls *how* the slicer maps colour assignments (ColorStitch slots / PathBlend positions) to the **physical fill lines** of a surface. It does not change the pattern — only how slots find which lines belong to which spatial "lane." It lives in **Quality → Surface ColorStitch → Line distribution mode** (directly below *Minimum line length*) and affects both ColorStitch and PathBlend.

| Mode | Best for |
|------|----------|
| **Default** | Raw print order. Simple rectangular surfaces, Custom-text patterns. |
| **GeoSort** | Print order scrambled but the spatial direction is clean. |
| **LaneQuant** | Surfaces with **holes, concavities or disconnected sub-regions** — fragmented stripes stay the same colour. Recommended for complex tops. |
| **DirCluster** | The fill engine rotated direction per sub-region — each region keeps its own coherent gradient. |

**Quick rule**: if the gradient looks wrong, move one mode up and re-slice.

---

### 1g. ColorStitch Studio — palette generators

The **Studio** (bottom of the Sandwich Editor) generates a strip of colour swatches from your loaded filaments + TD, each one a complete pass recipe with its predicted colour. Click a swatch to load it into the live editor. The **Mode** dropdown:

| Mode | What it generates |
|------|-------------------|
| **Gradient ramp** | A manual **top-only** ramp between two tools (**A** visible side, **B** contrast). Set **Steps** and the **Split** (thinnest → thickest top pass, mm; floor 0.04). |
| **Flat color (predict)** | The gamut reachable by stacking solid passes — robust, predictable colours. |
| **Mixed approximation (predict)** | An extended gamut: a dithered ColorStitch base plus a translucent solid on top — colours no single filament can make. |

**Target + Match ▸** — pick a MixedColor target and press **Match**; the Studio runs an inverse search (minimising ΔE2000) and loads the closest achievable recipe, showing the resulting ΔE.

**Name + Export** — turns swatches into saved Surface Effect Profiles (§6). The strips react live to the TD sliders.

---

## 2. Neoweaving + Monotonic Interlayer Nesting (WIP)

> **(WIP — not wired in this build.)** Neoweaving and its companion, Monotonic Interlayer Nesting, are not yet ported to this Snapmaker 2.3.4 base and will arrive in a later release. The description below documents the intended behaviour.

**Neoweaving** alternates the Z height of successive fill lines on each layer: odd lines at the nominal height, even lines slightly higher (by an *amplitude*). The next layer inverts the pattern, so the elevated lines nestle into the recesses below — **mechanical interlocking** between layers, like puzzle pieces. It improves inter-layer adhesion and vibration damping without changing external dimensions. This is a structural technique, not a visual one.

**Monotonic Interlayer Nesting** is the companion that makes Neoweaving clean and controllable: it shifts the monotonic fill reference by half the line spacing on alternate layers, so the lines of layer N sit over the *gaps* of layer N−1. That precise registration is what lets Neoweaving's raised/recessed lines lock together layer to layer — so the two ship together.

---

## 3. Penultimate Top Layers

The layer(s) just below the top surface — the *penultimate* layer — normally behave like ordinary solid infill. This feature classifies them as their own zone so they can carry their own density and their own ColorStitch/PathBlend/Solid passes.

Set it in **Strength → Top/bottom shells**:

- **Penultimate top layers** — number of solid layers below the top treated as penultimate. **Default 0** (feature off); set 1 or 2 to enable (max 20). These use reduced density for a smoother transition and faster printing.
- **Penultimate solid infill density** — density of that layer (default 100%).

When painting (§6), **penu autonomy** auto-forces 2 penultimate layers for an object whose painted recipe declares penu activity, so the effect has a surface to print on.

---

## 4. Libre Mode

Libre Mode unlocks Snapmaker Orca for workflows where the normal constraints get in the way — **multi-part assemblies**, **professional workflows**, **experimental printing**.

### 4a. Enabling Libre Mode

Libre Mode uses a **two-key gate** so it never interferes with normal use:

1. **Master switch** — **Preferences → "Enable Neotko LibreMode (requires restart)"** (off by default). Until this is on, no Libre Mode UI exists at all (Snapmaker sees a stock build).
2. **Active toggle** — once the master switch is on (and after the restart), a side button labelled **"Neotko LM: Off / On"** appears in the toolbar. Click it to toggle the active state.

> ⚠️ Because the master switch builds UI at start-up, enabling it **requires a restart**. Some panels (e.g. full Assembled-parts options, §4f) only appear after that.

While active, Libre Mode also signals the slicer: an object with no first layer becomes a **warning** instead of a hard error, and split/assemble operations are allowed across Z=0.

---

### 4b. Floating objects

With Libre Mode active, objects can sit at **any Z height** — floating above the bed or partly below it — instead of being snapped to the plate. The slicer still generates G-code and warns (instead of erroring) when an object has no initial layer. Use it for assemblies whose parts print at specific heights, or parts that clip into a structure already on the bed.

The floating Z is **preserved across object operations** — copy/paste, *Paste Process Settings*, reload-from-disk, replace-STL, boolean, mesh simplify and *face the camera* no longer snap a floating object back to Z=0. To drop a floating object to the bed on purpose, use the **sinking** column in the object list (that path is left intact).

---

### 4c. Assembled Boolean mode

When you combine parts into an **Assembled** object, the stock slicer performs a boolean union. **Assembled Boolean mode** (right-click an object → toggle; `neotko_assemble_boolean`, default **on**) lets you turn that off **per object**: the parts are sliced and merged *without* a union, so overlapping multi-material geometry (inserts, interlocking colour regions) is kept as separate co-existing meshes instead of being collapsed. This mode is **print-verified**.

---

### 4d. Per-volume XY compensation

Normally XY contour/hole compensation is per-object. With Libre Mode and an Assembled object, each **volume** (individual mesh / part) can have its own XY compensation; the slicer applies the *delta* between the part's value and the object's to that part before merging. Use it for multi-material assemblies whose materials shrink differently (e.g. a PETG insert in a PLA shell). The controls appear in the part settings.

---

### 4e. Copy / Paste Process Settings

Libre Mode adds **Copy Process Settings** (a submenu with **Speed / Quality / Strength** and **All**) and **Paste Process Settings** to the object context menu. Copy one object's process settings and paste a chosen block onto another:

- **All** → replaces the target's process settings.
- A single category (Speed / Quality / Strength) → merges into the target, leaving the others untouched.

Use it to propagate a tuned block across many parts without overwriting their other settings.

---

### 4f. Assembled Parts — full options

In stock OrcaSlicer the **parts** inside an Assembled object expose only a limited subset of settings. With Libre Mode active each part's settings tab exposes the **full option set** (the combined Print-object + Print-region keys), and a **↺ Refresh Part** side button rebuilds the view if it gets out of sync.

> ⚠️ The parts tab is built once at start-up — toggling Libre Mode may need a restart for the full set to appear.

---

### 4g. World-space import (WIP)

> **(WIP — basic functionality works.)** With Libre Mode active, objects can be imported at their **source-file world coordinates** instead of being re-centred on the plate, preserving relative positions across an assembly.

> **Recommended workflow in this build:** rather than relying on world-space import alone, import as an **Assembled** object and then **split in Libre Mode**. That is the reliable route while the world-space path is being finished.

---

### 4h. Internal-bridge handling

On floating objects and unusual geometries, internal-bridge detection can misfire and apply bridging where it isn't wanted. Note that the **stock 2.3.4 default** `internal_bridge_density` is 25% — if a top surface looks unexpectedly filled or empty, that stock setting is usually the cause, separate from Libre Mode. (The older fork's automatic "disable internal bridges" behaviour is deliberately **not** carried as-is here; it caused a layer-count bug and will return later as an explicit opt-in.)

---

## 5. S3DFactory — Simplify3D project import

This pack can open **Simplify3D `.factory` project files** — a complete project with multiple objects, positions and extruder assignments. A `.factory` **always loads as a single Assembled object** (its parts share one world-space layout — base, texts, etc.). Use **File → Import → Import 3D model** (or drag & drop) and pick the `.factory` file.

> **Recommended workflow in this build:** after import, **split in Libre Mode** to recover the individual parts in place. The importer is functional this way; a little positioning fine-tuning is still unfinished, so the assembled→split path is the reliable route.

---

## 6. Surface Effect Profiles & ColorStitch Painter

Save Sandwich configurations as named **profiles**, then **paint them onto specific surfaces** of your model with a brush gizmo — different parts of one object can carry different Sandwiches. Profiles and painted areas are saved inside the **3MF**, so they travel with the print.

At slice time, when an object has any painted facets it switches to **painter mode**: the preset Sandwich settings are ignored for that object and each painted area uses its own profile. This is the cleanest way to apply several different surface effects to one object without splitting the mesh.

---

### 6a. Saving and managing profiles

In the **Sandwich Editor**, **Save as profile…** captures the current pass stacks as a named profile (a popup reports how many keys were captured). **Manage Sandwich Profiles** lets you **Load into dialog**, **Update from current**, **Rename** and **Delete**.

**Orphan warning**: deleting a profile that has painted areas emits a **non-critical slicing warning** (*"…painted regions referencing deleted Surface Effect Profile(s)…"*); the slice continues and those areas just get no effect. Re-paint or re-create the profile to fix.

---

### 6b. The ColorStitch Painter gizmo

The Painter lives in the **left-side gizmo toolbar** of the 3D view.

**Tools (top row)**

| Tool | What it does |
|------|--------------|
| **Paint (smart fill)** | Click a flat face → flood-fills the coplanar top region with the active profile. |
| **Eraser** | Smart-fill removes paint under the cursor. |
| **Pick** | Reads the recipe under the cursor and loads it as the active colour. |

**Mouse rules** — left-click paints/erases/picks with the active tool; **right-click is camera only**; **Shift + left-click** is a one-shot erase. Left-clicking with nothing selected does nothing.

**The panel**

1. **Palette strips** — collapsible **Gradient ramp** and **Flat color** sections, scrollable strips of swatches generated from your filaments + TD (same engine as the Studio, §1g). They regenerate when colours/TD change.
2. **Pro mode** — a collapsible composer, and **the Pro panel IS the active colour**: build **Top / Penultimate** passes (Solid / ColorStitch / PathBlend Half|Full) with a per-pass Z box and a **Perimeter override** checkbox. If the active colour is linked to a saved profile, editing it here rewrites that profile in place.
3. **Pin to palette** — promote the active recipe into the saved **Profiles** library.
4. **Profiles** — saved palettes; click one to load and paint with it.
5. **Smart fill angle** + section-view **clipping** controls, and **Erase all painting**.

**How to paint**: pick a swatch or compose one in Pro mode (it becomes the active colour) → click the surface to paint. Use **Pick** to grab a colour already on the model.

---

### 6c. Palette groups, slot cap & Save All

**Working vs saved.** Colours you paint are *working colours* — created on demand, deduplicated, and garbage-collected when no face uses them (shown with an amber border while occupying a slot). Browsing palettes does **not** consume slots. *Saved* palettes are deliberate, named, and travel in the 3MF.

**Palette groups** — saved palettes are organised into **groups** (up to 10, global). Use **+ New group** to add one and the **Group** selector to switch; deleting a group moves its colours to Group 1.

**Slot cap** — up to **254** painted slots per object (slot 0 = unpainted).

**Save all** — promotes **every unsaved working colour** into the active palette group at once, so a later *Erase all* leaves nothing dangling.

Painting and the slot→profile mapping are recorded for **undo/redo** within the session.

---

### 6d. Painter mode at slice time

| Situation | What applies |
|-----------|--------------|
| Object with zero painted facets | **Preset mode** — Sandwich Editor values apply |
| Painted object — painted area | The **painted profile** applies; preset ignored for that area |
| Painted object — **unpainted** area | **No effect** (preset suppressed for the whole object) |

This "all or nothing" rule prevents mixing preset and painted effects. For each top/penu fill at each layer the slicer uses the dominant painted slot in that Z range, so different regions at the same height each get their own effect. The wipe-tower planner uses the same lookup as the slice, so plan and G-code stay in sync.

---

### 6e. Profile persistence and 3MF round-trip

Everything is saved inside the 3MF: the **profile library** (project-level base64 JSON), the **per-volume slot tables** (slot → profile id), and the **per-triangle paint** (mirroring MMU painting). Opening a 3MF restores all three. Profiles live inside the 3MF only — there is no cross-project library; save a template 3MF to reuse a set.

> **Known limitation — PathBlend on Penu.** The penultimate PathBlend has a gradient-direction bug on multi-stair objects, so the PathBlend pass is restricted to the Top zone for now. The engine supports penu PathBlend; it will be re-enabled when the bug is fixed.

---

### 6f. Weave preview on the painted surface

Painted top surfaces show the **ColorStitch weave directly on the model** — the per-line tool stripes (or dither / gradient / hard bands) instead of a flat swatch colour. The preview is built from the **same per-line sequence the slicer produces** (`build_dithered_tools_*` / `build_custom_bands` / pattern), so the **filament colours, density and pattern match the G-code**. The same sequence builder also drives the small pass strip in the **Pro tray** and the **Sandwich editor**, so the strips and the 3D view stay identical. (The preview is always on now; the old *Preview weave* toggle was retired.)

**Scale fits the painted area at the real line width.** The stripe pitch comes from the **resolved top line width** (config, no slice needed), and the gradient/pattern is scaled to the **painted region's own extent** — computed **per island**: each flat zone (e.g. a stair step) is detected as a connected component (edge-adjacency, so zones that only touch at a corner stay separate) and gets its **own** gradient ramp, just like the slice. Tiled patterns repeat at the real line width (shader wrap), so the stripe width matches the print regardless of zone size.

**Orientation matches the slice for a fixed angle.** The stripes run at the pass's **fixed angle**, and that angle is now **honoured exactly in the G-code**: for a fixed ColorStitch angle the slicer's per-layer fill-angle rotation is **locked out** (internally via the template-angle flag), so every layer keeps the painted orientation. Set the angle by **scrolling the mouse wheel over the pass preview bar** (Pro tray and Sandwich editor) — the bar, the 3D model and the print all rotate together in real time.

> **Auto angle (`-1`).** With auto angle the slicer **alternates the fill direction every layer** (this is what gives a uniform finish), so a static preview cannot match the print. An amber **"auto angle"** tag appears next to **ADV** in that case — set a **fixed angle** (wheel over the bar) to lock the orientation.

> **Remaining limitations (this version).** The stripe scale uses the painted-area projected extent, **not** the exact line count after perimeters/gap-fill are subtracted, so it can differ by a line or two. Islands wider than ~64 lines coarsen in the preview (64-entry shader LUT) — gradients just lower resolution; patterns still tile at real width. Painting is restricted to **upward-facing (top) faces**, matching where the effect actually prints.

---

### 6g. MixedFilament Object mode (Beta)

A **MixedFilament** (Filament Settings → the *MixedFilament* rows built from two of your
loaded filaments) can be assigned to a whole object as its extruder, the same way you'd
assign any normal filament. **MixedFilament Object mode** is a one-click way to make that
object's **top surface and penultimate layer** actually *look like* that MixedFilament's
colour, instead of printing with whatever the default top/penu treatment would be.

**How to use it**: open the **ColorStitch Painter** gizmo (§6b) on an object whose extruder
is a MixedFilament. A new checkbox — **"MixedFilament Object"** — appears above the palette
strips, with a small colour swatch next to it showing the approximated result.

- If the object's extruder is **not** a MixedFilament, the checkbox is greyed out with a
  tooltip telling you to assign one first.
- Turning it **on**:
  - Auto-generates a small sandwich (up to 3 solid passes) that approximates the
    MixedFilament's colour using your other loaded filaments and their **TD** values
    (§1e) — the same colour-matching math the ColorStitch Studio uses.
  - Turns **Perimeter override** on automatically, so the walls get reprinted to match too.
  - **Locks out** manual painting/patterns for that object (the palette strips, zone
    editors and the Perimeter override checkbox grey out) — the object is either "painted
    by hand" or "driven by its MixedFilament," not both at once.
- Turning it **off** restores whatever was painted before (nothing is lost).

> **Beta.** This feature is functional and print-verified in principle, but still young —
> report anything that looks off. Two known rough edges: the swatch shows the **colour**
> only, not a preview of the pattern/passes that will actually print; and the checkbox
> currently lives inside the **Pro mode** panel rather than as a top-level toggle (it may
> move up in a future update, since it changes the whole object's behaviour).

---

## 7. Align & Stack — align and stack objects

**Align & Stack** is a gizmo (left-side gizmo toolbar, **"Align & Stack"**) for aligning and stacking multiple objects against an anchor. Click objects in the scene to add them **in order**: **#1 becomes the anchor** and the rest move toward it; click more to extend the order, or **Reset** to start over. Object selection has been improved over earlier versions for easier picking.

**Two modes:**

| Mode | What it does |
|------|--------------|
| **Place against** | Objects come to rest **touching the chosen face of #1**, chained (#2 on #1, #3 on #2 …). This is the stacking mode — build a vertical sequence of parts. |
| **Align flush** | Same-side faces become **coplanar** with #1 (Illustrator-style alignment). |

A row of **face / centre buttons** picks which face or centre axis to align or stack against (the tooltip changes with the mode). **Z gap (mm)** sets a controllable gap between stacked objects, and **Drop to bed (Z = 0)** drops every ordered object back onto the plate.

> Works together with Libre Mode (§4) for floating/assembled workflows — align or stack the parts, then slice with the arrangement you need.

---

## 8. NeoArachne — alternative wall generator

**NeoArachne** is a wall-generation engine that sits beside the stock Classic and Arachne engines and lets you choose, *per feature* (outer wall, inner walls, gap fill), which underlying engine prints it — plus extra controls targeting Arachne's known failure modes (width breathing, blobs at thin transitions). The default recipe — **Neotko Hybrid v2** — uses Classic for the outer wall and stock Arachne for the inner walls. It ships a **Preview Lab** (§8d) that renders wall paths before slicing.

> **NeoArachne is exposed only under Libre Mode** (§4): the **NeoArachne** entry in the wall-generator dropdown, and its controls, appear only when Libre Mode is active. It is opt-in — existing presets are unaffected.

### 8a. Turning it on

1. Enable **Libre Mode** (§4a).
2. **Quality → Wall generator** (Advanced) now offers **Classic / Arachne / NeoArachne**.
3. Select **NeoArachne** — its controls appear inline below the dropdown. Leave them at defaults for a first slice (the Neotko Hybrid v2 recipe).

### 8b. Per-feature engine choice

Three source dropdowns route each wall kind through an engine:

| Setting | Options |
|---------|---------|
| **NA — outer wall source** | Classic / Arachne (stock) / Arachne (NeotkoEdge) |
| **NA — inner walls source** | Classic / Arachne (stock) / Arachne (NeotkoEdge) |
| **NA — gap-fill source** | Off / Classic / Arachne (stock) / Arachne (NeotkoEdge) |

- **Classic** — constant width, cleanest visible surface, no breathing; can't adapt to thin features without a gap-fill pass.
- **Arachne (stock)** — variable-width beading with integrated gap fill. Best for inner walls.
- **Arachne (NeotkoEdge)** — Arachne with the NeotkoEdge bead-count stabiliser. *(Its extra tuning knobs from the older fork are not exposed in this build yet.)*

The default **Neotko Hybrid v2** = outer Classic, inner Arachne (stock), gap fill Off.

### 8c. Edge Closure controls

| Control | Range / default | What it does |
|---------|-----------------|--------------|
| **NA — allowed perimeter overlap** | 0–100%, default **0%** | How much the first inner Arachne bead may overlap the Classic outer. Raise to 5–15% only if you see a seam; high values blob. |
| **Min Line Width** | 5–100% of nozzle, default **40%** | Minimum bead width Arachne emits. Higher widens thin features (blob risk). 30–50% recommended. |
| **Max Line Width** | 100–200% of nozzle, default **200%** | Ceiling on bead width. Lower (130–150%) for more, narrower beads. |
| **Min Feature Threshold** | 1–100% of nozzle, default **10%** | Geometry thinner than this is discarded. Keep low; must stay ≤ Min Line Width. |
| **Preserve Thin Edges** | on/off, default **on** | Keeps short closure tails near the outer perimeter for cleaner seams. |

### 8d. Preview Lab

A panel inside the NeoArachne section that renders planned wall paths **before slicing**: outer + inner paths in distinct colours, the execution (chain) order, seam dots, travel moves, and a head animation. Controls include a layer slider, animation speed, a ghost/printed build mode, zoom, "use selection," and a **Dump** button that exports the full plan as JSON for off-line diagnosis.

> **Known limitation**: a small visual divergence vs the real slice on the second-to-last inner wall in narrow "waist" regions of some geometries — use **Dump** to share the JSON (and the 3MF) for diagnosis.

---

## 9. NeoTower — post-slice wipe tower

**NeoTower** is an alternative wipe-tower planner. The stock planner decides geometry up front; NeoTower runs **after slicing**, when every toolchange is known — including the sub-layer primes that Sandwiches and Solid-pass stacks insert *inside* a layer. Seeing the full toolchange list first lets it build a **fixed, predictable footprint** that stays in sync with the real G-code and understands variable layer heights. Options live in **Quality → Prime tower**.

> **You usually don't have to choose it.** Any scene that uses a Sandwich or a multi-pass stack **auto-promotes to NeoTower** regardless of the setting below. NeoTower requires a prime tower to be active.

### 9a. Tower type

| Setting | Behaviour |
|---------|-----------|
| **Classic** (default) | The standard WipeTower2 planner. |
| **NeoTower** | The post-slice planner — fixed footprint, delta-Z aware, with the options below. |

### 9b. Zigurat taper

**Default: on.** Limits how fast the footprint may shrink between consecutive real layers (one perimeter width per side) so every wall ring rests on the ring below (*wall-on-wall*). Disable to save material and time, at the cost of rings only partially supported by the sparse interior grid.

### 9c. Sandwich purge compaction

Sandwich / multi-pass purges run at microscopic heights and would inflate the tower. **Purge compaction** is a flow-boost cap that compacts those thin purges into a narrower band (the surplus hangs into the hollow interior), reducing the footprint.

- **1.0** = off. Higher = more aggressive. **Default 1.7.** Range 1.0–5.0. Confirm with a test print.

### 9d. Sandwich wipe reserve

The volume (mm³) purged before each Sandwich sub-layer toolchange (Solid / ColorStitch / PathBlend, Top + Penultimate). The config key is `multipass_prime_volume`, labelled **"Sandwich wipe reserve"**.

- **Default 10 mm³.** Lower = thinner/shorter tower; higher = better purge. Set **0** to disable. Requires a wipe tower active.

### 9e. Variable layer height (Experimental)

Because NeoTower plans from the real post-slice toolchange list and is delta-Z aware, it is the mechanism that lets **adaptive / variable layer height + multiple tools + a Sandwich** coexist on one coherent tower. Stock Orca refuses to slice such scenes; NeoTower can purge each toolchange at the real per-layer height.

A new option **Variable layer height (Experimental)** sits under **Tower type** and is exposed **only with Tower type = NeoTower and Libre Mode active** (it is visible but greyed-out otherwise). **Default: off.** When **on**, the slicer stops blocking:
- scenes that **mix objects with different layer heights**, and
- **adaptive / variable layer height combined with more than one filament**.

> **(Experimental.)** The wipe-tower issue that previously left **empty/short tower layers** (missing "drawers" on real layers, including the *"empty first layer"* abort) is **fixed in 2.3.1** — the tower now stays coherent under variable layer height. The capability is proven and was solid on the **2.2 line (older FS099 fork)**, and is now consistent on this 2.3.4 base too. It is still flagged Experimental: review G-code before long multi-tool runs. The option only takes effect with Tower type = NeoTower.

---

## 10. Bump Mapping Editor — texture-driven wall & top-surface relief

The **Bump Mapping Editor** is one gizmo (`GLGizmoTextureBump`) with three modes, switched via a
bar at the top of its panel: **All**, **Painter**, and **Top**. All three turn a grayscale (or
any) PNG into physical Z relief at slice time — the difference is *where* the relief goes and
*how* it's scoped.

> **Hidden behind a double gate in this build — expert-only, on purpose.** The gizmo only appears
> in the toolbar with **both** (1) **Libre Mode** active (§4a: master switch in Preferences +
> restart, then the toolbar toggle) **and** (2) the debug env var `ORCA_DEBUG_TEXTUREBUMP` set
> before launch; Top mode additionally needs `ORCA_DEBUG_ZBUMP` (or `ORCA_DEBUG_ALL=1`) for ZBump
> to actually apply when slicing. This isn't a placeholder gate — some option combinations here are
> genuinely capable of producing bad extrusion (see the Classic-wall-generator note in §10a and the
> per-mode limitations below), so it's kept off unless you've deliberately opted in and are prepared
> to read the resulting G-code. Full per-OS activation steps and the safety notes that go with them
> are in `NEOTKOCM_RELEASE_2_35.md`.

### 10a. All mode — object-wide wall texture

Applies one texture to every wall of the selected object, projected via **Planar / Cylindrical /
Spherical / Cubic** (Quality → wall texture settings, or the panel's Source/Relief/Transform
sections). Three on-canvas 3D handles adjust it without leaving the viewport:

| Handle | Colour | Drag controls |
|--------|--------|---------------|
| **V** | blue | Scale (tile size, mm) |
| **U** | orange | Repeat count |
| **Yaw ring** | purple | Rotation around the pivot |
| **Pivot** | pink | Pan — moves the tile origin |

A live textured overlay shows the pattern on the object before slicing, so you can line it up by
eye. The real object dims while the gizmo is open so the projection isn't hidden behind it.

> ⚠️ **Wall generator: Arachne only.** Wall-texture bump (All + Painter modes) requires the
> **Arachne** wall generator. It's deliberately disabled on **Classic** — a real test print showed
> Classic silently over-extrudes when adjacent walls carry different bump amounts, and the preview
> hides it while the G-code doesn't. **NeoArachne isn't wired up for it yet either.** Even on
> Arachne, adjacent walls with very different bump amounts can still show a visible gap between
> them (nothing adjusts line *width* to compensate, only the centreline moves) — this is a known,
> open limitation, not something the gate gets you past.

### 10b. Painter mode — per-zone texture

Paint zones on the model (same brush/erase interaction as the other painter gizmos) and assign
each zone its **own** texture, projection mode, scale, and thickness — independent of the
object's base (All-mode) settings and of every other zone. Each zone gets its own overlay preview
and a thumbnail in the zone list so you can tell them apart at a glance.

### 10c. Top mode — ZBump, top-surface height-map relief

A different feature entirely (no shared engine code with All/Painter's wall texture) that
modulates the **Z height of the top-surface fill**, not wall XY. A grayscale image becomes a
literal height field: brighter pixels raise the top surface, sampled point-by-point along every
top-fill line for a real 2D relief (not one flat Z per line).

| Control | What it does |
|---------|--------------|
| **Height (mm)** | Max Z displacement at full-white texture value. Shown in red past a safe-height estimate (0.8× nozzle − layer height) — a warning, not a hard cap; you can go past it and judge from the resulting G-code. |
| **Reinforcement passes** | Splits the total height across multiple stacked passes when it exceeds what one pass can safely reach on its own. |
| **Scale / Repeat** | Physical size of one image tile, and how many times it repeats. |
| **Pan X / Y** | Shifts the tiling phase — drag the on-canvas green handle, or type exact values. |
| **Edge ramp (mm)** | Smoothstep margin from the top-fill's own contour, so the wall itself stays flat. |

The green pan handle rests at the object's own center plus the current offset, and a live
textured overlay (cropped to roughly where top-fill actually starts, inset from the object's
outer edge by its perimeter walls) previews the pattern before slicing — drag the handle or the
numeric fields, both update the same preview live.

> **Preview vs. slice.** The overlay is a GUI-only approximation (perimeter inset is estimated,
> not the engine's exact fill boundary) — always meant as a placement guide, not a pixel-perfect
> match. It's cross-checked against the real slicing math via a calibration log
> (`ORCA_DEBUG_ZBUMP`, `/tmp/neotko_zbump.log`) that logs identical probe points from both the
> GUI and the engine side for direct comparison.

**Known limitation:** the on-canvas scale/rotation handles (in any mode of this gizmo) can
disappear when the print bed renders behind them, depending on camera angle — visible fine from
below. Long-standing, not specific to any one mode; not yet root-caused.

---

## 11. Precision Adaptive Layer Height — point-based layer height curve

A gizmo (left-side toolbar) that replaces the stock "Layers editing" brush with an exact,
point-based curve editor for variable layer height. Instead of clicking and dragging to add/remove
detail (imprecise — you can't say "I want exactly 0.28 mm at Z=14.2 mm"), you place **control
points** at an exact Z and height, drag them, and set a **tension** per segment to shape the curve
between them.

> Like **Align & Stack** (§7), this gizmo is gated behind **Libre Mode** (§4) — its icon is
> **always visible** in the toolbar but stays disabled (greyed out) until Libre Mode is active. It
> never affects the stock Variable Layer Height dialog/brush, which remains untouched and fully
> usable with Libre Mode off.

**How to use it**

1. Enable **Libre Mode** (§4a), select a single object, open the gizmo.
2. The panel shows a graph: height (mm) across, Z across the object's height vertically.
   - The **bottom point** (grey) is the object's fixed first layer — it can't be moved or deleted.
   - The **top point** (amber) can be dragged in height only.
   - **Click anywhere else in the graph** to add a new point; **drag** any non-bottom point to move
     it; **right-click** a point to delete it.
3. With 3+ points, a **tension slider** appears per segment below the graph: `0` = straight line
   (identical to the engine's default linear interpolation), `1` = a smooth curve through the
   segment (monotone — it never overshoots past the two points' heights, so it can't spike outside
   `min`/`max layer height`).
4. While dragging or hovering a point, a translucent band lights up **on the object itself**,
   showing exactly which Z-slice you're affecting — teal on hover, amber while dragging. A small
   label over the point shows the exact Z and layer height live.
5. Every discrete edit (point add/move/delete, tension change, Reset) is a normal undo step and
   triggers a re-slice, same as the stock brush.

**Current limitations (first version)**
- Min/max layer height are **read-only** (from the printer/nozzle) — no per-object override yet.
- Reopening the gizmo on an object that already has a *very* dense profile (e.g. one painted with
  the old stock brush) falls back to a flat 2-point start rather than importing hundreds of points
  as control points.
- No result preview from the classic Adaptive/Smooth buttons — this gizmo is a separate, precise
  path, not a replacement for those.

---

## 12. NeoWave Support (WIP) — Wave-Huygens roof + hollow pillar + contact layer

> **(WIP, paused mid-implementation — Libre Mode only.)** The support roof and hollow pillar
> described below are implemented and **print-validated**. The second mechanism — a **contact-layer
> toggle** (§12a) that ripples the object's own bridge fill directly above the roof — is now shipped
> and **slice-verified**, but has not yet been through a real print (its first print is still ahead).
> Both the support engine and the contact layer are experimental; expect this section to expand in a
> later release.

**NeoWave** is a support type that replaces the interface/roof fill with a *wave-front* pattern —
long, continuous paths that diffract around concavities (ported from a published wave-overhang
algorithm), instead of straight parallel lines. It optionally hollows out the support body itself
(perimeter-only, no infill) underneath that roof, trading material and print time for a support
that still closes cleanly on top.

> **Exposed only under Libre Mode** (§4): the **NeoWave** entry in **Support type**, and the Wave
> roof controls below, appear only when Libre Mode is active.

**Turning it on**

1. Enable **Libre Mode** (§4a).
2. **Support → Support type**: select **NeoWave**.
3. **Support → Interface pattern**: select **Wave** — this is what actually switches the roof fill
   to the wave engine. With any other pattern (Default/Grid/etc.), a NeoWave support prints its roof
   like a normal support.

**Wave roof controls** (appear once Interface pattern = Wave)

| Control | Options / default | What it does |
|---------|-------------------|--------------|
| **Wave roof shape** | Concentric / **Wave** | *Concentric* collapses nested rings inward from the roof boundary. *Wave* sweeps open arcs across the region, diffracting around concavities. **Print-tested: Wave is the one that works — Concentric fails to close cleanly over a hollow pillar (see below).** |
| **Wave roof order** | **Smart** / ZigZag / Monotonic | Print order of the same fronts — the drawn shape doesn't change, only how it's traversed. Smart hooks each front onto whatever is already printed (fewest long travels); ZigZag chains fronts into one continuous path; Monotonic keeps every front separate. |
| **Reverse wave roof order** | on/off, default **off** | Flips which edge (or, for Concentric, which ring) the fill starts from. |
| **Hollow pillar walls** | 0–10, default **0** | Number of perimeter walls for the support body, printed with **no infill** — a hollow pillar. `0` keeps the normal solid body. The first layer always stays solid for bed adhesion. |

> **🖨️ Print result so far**: a hollow pillar (**Hollow pillar walls ≥ 1**) capped with a **Wave**
> roof of **2 interface layers** printed cleanly. The same pillar capped with a **Concentric** roof
> did **not** close properly over the hollow — use **Wave**, which is already the default shape.

### 12a. Contact layer (new, WIP — slice-verified, print-pending)

A separate, independent toggle that ripples the Z of the object's own bridge fill wherever it lands
directly above a support roof — valleys touch the roof, crests stay in the air, leaving intentional
microscopic contact gaps meant to reduce bonding force so the support separates more easily. Unlike
the original idea (a paintable Sandwich effect), this shipped as a **plain on/off toggle under
Support**, independent of the Sandwich/ColorStitch system entirely: it works whether or not the
object has any painting on it, and it never touches colour/pattern — only Z.

| Control | Options / default | What it does |
|---------|-------------------|--------------|
| **Support neoweave contact** | on/off, default **off** | Enables the wave ripple on bridge fill directly above a support roof. |
| **Contact wave amplitude** | mm, default **0.1** | How far the wave deflects (upward only — it never digs into the roof below, by construction). |
| **Contact wave period** | mm, default **0.6** | Distance between successive wave peaks along the fill line. |

> ⚠️ **Slice-verified, print-pending.** The toggle is confirmed to fire correctly wherever bridge
> fill sits over a support roof, and the resulting G-code looks correct — but this specific mechanism
> has never been through a real print. Orca's G-code preview also **doesn't render the Z variation**
> this produces (same known limitation as Neoweave top/penu and ZBump) — the G-code is right even
> though the on-screen preview looks flat.

**Known gaps (this build)**

- No dedicated key yet for roof layer count — it uses the existing **Interface top layers** setting.
- No dedicated UI panel/gizmo yet; all controls above live as plain fields under **Support**.

---

## 13. Painter Pro Mode — precision tools for the stock Color Painting gizmo

> **Not the same gizmo as §6.** This section is about Orca's own **stock multi-material painter** — the **Color Painting** tool in the left-side gizmo toolbar (needs 2+ filaments configured; it's what you'd use to hand-assign filaments to triangles in any Orca build). It is a **different tool** from this pack's own **ColorStitch Painter** (§6), which paints *Sandwich effect profiles*, not raw filament assignment. **Pro Mode** is a collapsible section added to the bottom of the stock Color Painting panel with four precision add-ons on top of the regular brush. It is **always available** — no Libre Mode needed.

The stock brush paints by hand with a circle/sphere cursor, which is naturally imprecise on small or fine details — a click can bleed well past where you meant to paint. Pro Mode's four tools attack that problem from different angles: finer brush subdivision, limiting paint to a thin perimeter ring instead of filling solid, and two "mask" tools that paint an exact area in one shot instead of brushing it by hand.

### 13a. Brush precision

A **Precision** slider (1×–8×) in Pro Mode subdivides the mesh more finely right where you're painting, so the edge of a brush stroke follows the surface more closely instead of stair-stepping at low mesh resolution. Higher values cost more memory/CPU on dense meshes. Default `1×` = identical to a build without Pro Mode.

### 13b. Paint perimeters only + Extra walls

Two checkboxes/fields that change *where* a painted colour actually gets used, without touching the brush itself:

| Control | What it does |
|---------|--------------|
| **Paint perimeters only** | Limits the painted colour to a ring near the painted region's contour (about wall-count × line-width wide) instead of filling the whole painted area solid. The interior reverts to the object's base colour. Reduces wasted filament and colour changes buried in solid infill where nobody will ever see them. |
| **Extra walls** | Adds this many extra perimeter walls to the painted region **only**, on top of the object's normal wall count — for a painted logo or trim that needs a bit more wall depth than the rest of the print. `0` = disabled. |

Turning on **Extra walls** automatically widens the **Paint perimeters only** ring to make room for them — if you raise Extra walls after already enabling the ring, the ring re-widens to match.

> **By design: Extra walls has no effect where the painted colour matches the object's own filament.** If you paint a region with the same colour the object already prints in, there is no visible colour change there anyway — so Extra Walls is skipped for that region automatically, rather than silently doubling up walls in the same physical spot for no visible benefit. This only matters if you're using the painter to add wall thickness rather than colour — paint with a genuinely different colour (even one you don't intend to keep) to force the extra walls, or use a plain modifier mesh instead for pure geometry changes.

### 13c. Rectangle & Polygon masks

Two alternative "paint" tools for when hand-brushing an exact shape is fiddly — you outline an area on screen and it gets painted in one shot with whichever colour is currently active, instead of brushing it by hand.

| Tool | How to use it |
|------|---------------|
| **Rectangle mask** | Enable the checkbox, then click-drag a box anywhere on screen; release to paint everything under it. |
| **Polygon mask** | Enable the checkbox, then **click** to place vertices one at a time. **Click near the first vertex again** (with at least 3 placed) to close the shape and paint it. **Click-drag an existing vertex** to reposition it before closing. **Right-click** cancels and clears the in-progress shape. |

Both tools only paint **front-facing** triangles — the side of the mesh actually facing the camera — so a mask never bleeds through to the back of the object the way a naive screen-space fill would. The two checkboxes are mutually exclusive (turning one on turns the other off), the same way Vertical/Horizontal work elsewhere in this panel. Works with either **Classic** or **Arachne** as the wall generator.

> **Escape doesn't cancel a polygon in progress — right-click does.** Keep that in mind if you're used to Escape backing out of in-progress tools elsewhere in Orca.

---

## 14. NeoStitch Interlock (WIP, ⚠️ UNTESTED) — Z-axis layer interlocking

> ⚠️ **Brand new, work-in-progress, and genuinely UNTESTED — preview-only, never printed.** Everything
> below is confirmed working **in the 3D preview only**. Treat this section as a curiosity/early-look,
> not a feature to rely on. One of its own controls (fill speed, see table below) is a **confirmed
> no-op bug** — turning it doesn't currently change anything in the resulting G-code.

**NeoStitch** interlocks consecutive printed layers of a chosen wall **without ever moving Z**. Along
the wall's own path, each layer alternates short **notch** segments (the wall deflects inward, leaving
a gap at the nominal position) and **fill** segments (the wall over-extrudes there instead to fill the
gap left by the layer above/below). The pattern's phase flips on alternating layers, so a fill segment
always lands over the notch of the layer beneath it — a vertical "stitch" between layers, mechanically
different from a Z-brick-layer interlock (Z itself never moves; only XY position and extrusion width
change).

**Where to find it**: **Strength → NeoStitch Interlock** (per-region setting, same place/pattern as
Fuzzy Skin).

| Control | Default | What it does |
|---------|---------|---------------|
| **NeoStitch Interlock** | **Disabled** | Which wall gets the interlock: Outermost / Second / Third / Innermost wall. |
| **Stitch depth** | 0 mm (**auto** = the wall's own line width) | How far the notch deflects inward. An explicit value overrides the auto. |
| **Stitch length** | 3.0 mm | The flat plateau length of each notch/fill segment. |
| **Ramp length** | 1.0 mm | The transition ramp in/out of each segment (also smooths the pressure/flow change). |
| **Stitch period** | 10.0 mm | Distance between successive notch/fill events along the wall. |
| **Stitch flow** | 100% | Scales the fill segment's extra extrusion; 100% = automatic volume conservation for the notch depth being filled. |
| **Skip bottom layers** | 3 | Extra layers to skip above the object's first layer before the interlock starts. |
| **Fill speed** | 75% | ⚠️ **Known bug — does not currently work.** Intended to slow down only the fill segments (Klipper's pressure advance doesn't like the non-constant flow at full speed); confirmed via G-code inspection that the speed does not actually change yet. Root cause not yet found. |
| **Fill margin** | 1.0 mm | Shrinks the fill segment shorter than its matching notch by this much per side, so the over-extrusion lands *inside* the gap instead of bridging across it. **Confirmed working.** |

**Why "notch"/"fill" instead of moving Z**: the whole point is to interlock layers without changing
layer height or printing outside the normal Z steps — it's a pure XY-deflection + flow-modulation
effect, always fully backward compatible with normal slicing when turned off (**Disabled** by default,
per-region, exactly like Fuzzy Skin).

**Known limitations / open items (this build)**:
- **Fill speed control doesn't work yet** (see table above) — everything else in this section is
  otherwise implemented.
- Only verified visually in the 3D preview — the alternating notch/fill pattern is visible and
  registers correctly from layer to layer, but **no real print has been made with it yet**.
- Only Arachne wall generation is supported (v1); Classic walls are unaffected by this setting.
- Interacts with Fuzzy Skin / Bump Mapping by mutual exclusion, not composition — if either of those
  is active on the same wall, NeoStitch skips that wall rather than stacking effects.
- A cascade to neighbouring walls (so an interior wall "follows" the perturbed contour of the wall
  NeoStitch targets, instead of keeping its original offset) is designed but not implemented.

---

## 15. Expert G-code Reprocessor (basic beta, 2.3.7) — layer-ranged G-code post-processing

> 🧪 **Basic beta — quick first cut, Libre Mode only.** Functional and safe, but minimal: no
> temperature/Z-offset rules yet, fan control is a hard override (not a smart clamp). Edits real
> G-code — a warning shows before every Apply.

**Where to find it**: G-code **Preview**, next to the RealColor view selector, when **Libre Mode**
is on.

Panel with two rule lists, each row: enable checkbox, layer **from**/**to** (`-1` = to the end),
and a value. "+ Add rule at current layer" pre-fills **from** with whatever layer the Preview
slider is on. **Apply** writes the rules and reslices; the G-code is rewritten on export.

- **Speed override rules** — `M220 S<percent>` from layer X to Y.
- **Fan override rules** — `M106 S<0-255>` (raw PWM, not percent) from layer X to Y.

Also fixed alongside this: Snapmaker U1/Klipper toolchanges were forcing `M220 S100` on every
color change (wiping any manual speed override) and emitting `M220 B`/`M220 R` — both leftover
from older Marlin-based Snapmaker machines and meaningless on the U1's Klipper firmware. Both are
gone now; toolchange G-code is simpler and no longer fights a manual speed setting.

---

## Quick Reference — Where to find things

| Feature | Location in UI |
|---------|---------------|
| Sandwich Editor (pass stack, ColorStitch, PathBlend) | Quality → Surface ColorStitch → **Sandwich editor…** |
| ColorStitch pass pattern | Sandwich Editor → a pass set to **ColorStitch** → **Edit gradient…** |
| ColorStitch Studio | Sandwich Editor → **ColorStitch Studio** panel (bottom) |
| Colour match (inverse ΔE2000) | ColorStitch Studio → **Target + Match ▸** |
| Filament & TD preview | Sandwich Editor → **Filament & TD** panel |
| Line distribution mode | Quality → **Surface ColorStitch** → Line distribution mode (below *Minimum line length*) |
| Top surface fill pattern (needed for ColorStitch) | Quality → **Top surface pattern → Monotonic Line** |
| Penultimate layers / density | Strength → Top/bottom shells |
| Neoweaving (WIP) | *Not wired in this build* |
| Libre Mode master switch | **Preferences → Enable Neotko LibreMode (requires restart)** |
| Libre Mode toggle | Toolbar side button **"Neotko LM: On/Off"** |
| Assembled Boolean mode | Right-click an object (Libre Mode) |
| Per-volume XY compensation | Part settings inside an Assembled object (Libre Mode) |
| Copy / Paste Process Settings | Right-click object → Copy/Paste Process Settings (Libre Mode) |
| Assembled Parts full options / ↺ Refresh Part | Part settings tab (Libre Mode) |
| World-space import (WIP) | Import with Libre Mode active *(recommend assembled → split)* |
| S3DFactory import | File → Import → Import 3D model → `.factory` *(loads assembled; split in Libre Mode)* |
| Save / Manage profiles | Sandwich Editor → **Save as profile… / Manage Sandwich Profiles** |
| ColorStitch Painter | Left-side gizmo toolbar |
| Painter tools (Paint / Eraser / Pick) | Painter panel top row |
| Palette groups / Save all / Pin to palette | Painter panel |
| MixedFilament Object mode (Beta) | Painter panel → **Pro mode** → "MixedFilament Object" checkbox |
| Align & Stack | Left-side gizmo toolbar → **Align & Stack** |
| NeoArachne (enable) | Libre Mode → Quality → **Wall generator → NeoArachne** |
| NeoArachne sources / line widths / Preview Lab | Quality → NeoArachne section |
| NeoTower (tower type) | Quality → Prime tower → **Tower type** |
| Zigurat / Sandwich purge compaction / Sandwich wipe reserve | Quality → Prime tower |
| Variable layer height (Experimental) | Quality → Prime tower → **Tower type** = NeoTower (greyed unless Libre Mode) |
| PathBlend start/end zone + techo editor | PathBlend pass → **`ADV…`** (Painter Pro tray or Sandwich Editor's Advanced button) |
| Bump Mapping Editor (All / Painter / Top) | Left-side gizmo toolbar *(expert gate: Libre Mode active **+** `ORCA_DEBUG_TEXTUREBUMP` set, **+** `ORCA_DEBUG_ZBUMP` for Top/ZBump — see `NEOTKOCM_RELEASE_2_35.md`)* |
| Precision Adaptive Layer Height | Left-side gizmo toolbar *(icon always visible, needs Libre Mode active to use — §11)* |
| NeoWave Support (enable) | Libre Mode → Support → **Support type → NeoWave** |
| Wave roof (shape/order/reverse/hollow pillar) | Support → **Interface pattern → Wave** to reveal the controls (§12) |
| NeoWave contact layer (WIP, print-pending) | Support → Advanced → **Support neoweave contact** toggle (§12a) |
| Painter Pro Mode (Precision / Paint perimeters only / Extra walls / Rectangle & Polygon masks) | Left-side gizmo toolbar → **Color Painting** → **Pro Mode** section *(always available, no Libre Mode needed)* |
| NeoStitch Interlock (WIP, ⚠️ untested) | Strength → **NeoStitch Interlock** (§14) |

---

## Frequently Asked Questions

**Q: ColorStitch doesn't apply on some layers — why?**

Confirm the top surface fill pattern is **Monotonic Line** (§1b) — the only pattern that works correctly on complex objects. Then check **ColorStitch min. line length** (short lines are skipped) and the **Zone** setting (*Topmost only* applies to just the top of the object).

**Q: Where did "MultiPass" go?**

It's now built from the pass stack: add **two or three Solid passes** to a zone and split the layer height between them with the dividers. That gives the cross-hatch / glaze / optical-blend effects the old MultiPass produced. See §1a.

**Q: My ColorStitch stripes look fragmented across holes.**

Change **Line distribution mode** (Quality, Advanced) to **LaneQuant** or **DirCluster**. See §1f.

**Q: How do I turn on Libre Mode? I don't see the button.**

Enable the **master switch in Preferences** ("Enable Neotko LibreMode") and **restart**; the **"Neotko LM"** side button then appears. See §4a.

**Q: Do I need Libre Mode to print normally?**

No. It's off by default and the build behaves like stock. It only unlocks the §4 workflows and exposes NeoArachne (§8).

**Q: My `.factory` (or world-space) import put parts in the wrong place.**

A `.factory` always loads as one **Assembled** object; **split it in Libre Mode** to recover the parts in place. Same recommendation for world-space import while it's being finished. See §4g and §5.

**Q: In the Painter, why don't picked colours show in the Profiles list?**

By design — colours you paint are *working colours* (created on demand, cleaned up when unused). Only palettes you deliberately **Pin to palette** appear in the saved list; use **Save all** to promote every unsaved working colour at once. See §6c.

**Q: Can I use adaptive layer height with multiple tools and a Sandwich?**

Yes, as of 2.3.1 — enable **Libre Mode**, set **Tower type = NeoTower**, and turn on **Variable layer height (Experimental)** (Quality → Prime tower). The wipe-tower "missing drawers" issue that made this rough is now fixed (§9e). It is still flagged Experimental, so review G-code before long runs.

**Q: My wipe tower uses more purge than expected.**

Check **Sandwich wipe reserve** (default 10 mm³) and **Sandwich purge compaction** (default 1.7) in **Quality → Prime tower** (§9). Lower the reserve for a thinner tower, or set compaction to 1.0 to disable it.

**Q: The "MixedFilament Object" checkbox is greyed out — why?**

The object's extruder isn't a MixedFilament. Assign one of your MixedFilament rows as the
object's extruder (same as assigning any normal filament), then reopen the Painter — the
checkbox and its colour swatch become active. See §6g.

**Q: Can I make a PathBlend gradient start or end somewhere other than the surface edges — or make it fully opaque at one end?**

Yes — open **`ADV…`** on the PathBlend pass (Painter Pro tray or the Sandwich Editor's Advanced button) and drag the two handles in the graph that opens. The low handle sets where the ramp starts rising (and its floor height); the high handle sets where it finishes (and its top height). Push the high handle all the way to the top for a full "techo" — zero of the cap colour in that zone. See §1c.

**Q: I want to try NeoArachne safely.**

Enable Libre Mode, set **Quality → Wall generator → NeoArachne**, leave the new controls at defaults (Neotko Hybrid v2), and slice. Switch back to Arachne/Classic anytime — the NeoArachne controls don't touch existing settings.

**Q: Is the "Color Painting" gizmo's Pro Mode the same thing as the ColorStitch Painter?**

No — different tools. **Color Painting** (§13) is Orca's own stock multi-material painter, and Pro Mode adds precision brushing/masking on top of it. The **ColorStitch Painter** (§6) is this pack's own gizmo for painting *Sandwich effect profiles* (pass stacks, gradients, patterns). Both live in the left-side toolbar as separate icons.

**Q: I turned on "Extra walls" but the painted area doesn't seem to have more walls.**

Check whether the colour you painted with is the **same** as the object's own filament — Extra Walls is skipped there by design, since it wouldn't be visible anyway (§13b). Paint with a genuinely different colour to see the extra walls take effect.

**Q: My Rectangle/Polygon mask painted something on the far side of the object too.**

It shouldn't — both tools only paint faces pointing toward the camera. Rotate the view to confirm what you're actually seeing is the front-facing paint, not a leak through the back.

---

All of this work is open and free. Fork it, improve it, credit it.

-----

Now all the info from the Original SnapMaker 2.3.4 Readme

-----


<h1> <p "font-size:200px;"> Snapmaker Orca</p> </h1>

[![Build all](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml)
<br>Snapmaker Orca is an open source slicer for FDM printers based on OrcaSlicer.
 


# Download

### Stable Release
📥 **[Download the Latest Stable Release](https://github.com/Snapmaker/OrcaSlicer/releases/latest)**  
Visit our GitHub Releases page for the latest stable version of Snapmaker Slicer, recommended for most users.

# How to install
**Windows**: 
1.  Download the installer for your preferred version from the [releases page](https://github.com/Snapmaker/OrcaSlicer/releases).
    - *For convenience there is also a portable build available.*
    - *If you have troubles to run the build, you might need to install following runtimes:*
      - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://github.com/SoftFever/OrcaSlicer/releases/download/v1.0.10-sf2/MicrosoftEdgeWebView2RuntimeInstallerX64.exe)
          - [Details of this runtime](https://aka.ms/webview2)
          - [Alternative Download Link Hosted by Microsoft](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
      - [vcredist2019_x64](https://github.com/SoftFever/OrcaSlicer/releases/download/v1.0.10-sf2/vcredist2019_x64.exe)
          -  [Alternative Download Link Hosted by Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
          -  This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`

**Mac**:
1. Download the DMG for your computer: `arm64` version for Apple Silicon and `x86_64` for Intel CPU.  
2. Drag Snapmaker_Orca.app to Application folder. 
3. *If you want to run a build from a PR, you also need to follow the instructions below:*  
    <details quarantine>
    - Option 1 (You only need to do this once. After that the app can be opened normally.):
      - Step 1: Hold _cmd_ and right click the app, from the context menu choose **Open**.
      - Step 2: A warning window will pop up, click _Open_  
      
    - Option 2:  
      Execute this command in terminal: `xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app`
      ```console
          softfever@mac:~$ xattr -dr com.apple.quarantine /Applications/Snapmaker_Orca.app
      ```
    - Option 3:  
        - Step 1: open the app, a warning window will pop up  
            ![image](./SoftFever_doc/mac_cant_open.png)  
        - Step 2: in `System Settings` -> `Privacy & Security`, click `Open Anyway`:  
            ![image](./SoftFever_doc/mac_security_setting.png)  
    </details>
    
**Linux (Ubuntu)**:
 1. If you run into trouble executing it, try this command in the terminal:  
    `chmod +x /path_to_appimage/Snapmaker_Orca_Linux.AppImage`
    
# How to compile
- Windows 64-bit  
  - Tools needed: Visual Studio 2019, Cmake, git, git-lfs, Strawberry Perl.
      - You will require cmake version 3.14 or later, which is available [on their website](https://cmake.org/download/).
      - Strawberry Perl is [available on their GitHub repository](https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/).
  - Run `build_release.bat` in `x64 Native Tools Command Prompt for VS 2019`
  - Note: Don't forget to run `git lfs pull` after cloning the repository to download tools on Windows

- Mac 64-bit  
  - Tools needed: Xcode, Cmake, git, gettext, libtool, automake, autoconf, texinfo
      - You can install most of them by running `brew install cmake gettext libtool automake autoconf texinfo`
  - run `build_release_macos.sh`
  - To build and debug in Xcode:
      - run `Xcode.app`
      - open ``build_`arch`/Snapmaker_Orca.Xcodeproj``
      - menu bar: Product => Scheme => Snapmaker_Orca
      - menu bar: Product => Scheme => Edit Scheme...
          - Run => Info tab => Build Configuration: `RelWithDebInfo`
          - Run => Options tab => Document Versions: uncheck `Allow debugging when browsing versions`
      - menu bar: Product => Run

- Ubuntu 
  - Dependencies **Will be auto-installed with the shell script**: `libmspack-dev libgstreamerd-3-dev libsecret-1-dev libwebkit2gtk-4.0-dev libosmesa6-dev libssl-dev libcurl4-openssl-dev eglexternalplatform-dev libudev-dev libdbus-1-dev extra-cmake-modules libgtk2.0-dev libglew-dev libudev-dev libdbus-1-dev cmake git texinfo`
  - run 'sudo ./BuildLinux.sh -u'
  - run './BuildLinux.sh -dsir'


# Note: 
If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.
```
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```


## Some background
Snapmaker Orca is originally forked from Snapmaker_Orca.

Snapmaker_Orca is originally forked from Bambu Studio, it was previously known as BambuStudio-SoftFever.
Bambu Studio is forked from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. 
Orca Slicer incorporates a lot of features from SuperSlicer by @supermerill
Orca Slicer's logo is designed by community member Justin Levine(@freejstnalxndr)  


# License
Snapmaker Orca is licensed under the GNU Affero General Public License, version 3. Orca Slicer is based on Snapmaker_Orca by SoftFever

Orca Slicer is licensed under the GNU Affero General Public License, version 3. Orca Slicer is based on Bambu Studio by BambuLab.

Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

Orca Slicer includes a pressure advance calibration pattern test adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.

The Bambu networking plugin is based on non-free libraries from BambuLab. It is optional to the Orca Slicer and provides extended functionalities for Bambulab printer users.

# Feedback & Contribution
We greatly value feedback and contributions from our users. Your feedback will help us to further develop Snapmaker Orca for our community.
- To submit a bug or feature request, file an issue in GitHub Issues or email us at support@snapmaker.com.
- To contribute some code, make sure you have read and followed our guidelines for contributing.
