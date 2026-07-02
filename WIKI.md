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

---

## 1. Surface ColorStitch — the Sandwich Editor

The **Sandwich Editor** is where you build the per-surface effect stack. It lives under **Quality → Surface ColorStitch → Sandwich editor…**.

> **Naming.** The feature family is called **ColorStitch** in the UI. You will still see *ColorMix* in a few internal places — config keys and 3MF data were not renamed, so old projects keep working. They refer to the same system.

### How the editor is laid out

The dialog has two columns — **Top layer** and **Penultimate layer**. Each is an independent **stack of 1–3 passes**, chosen with the **Passes** selector. The passes are stacked as thin virtual sub-layers inside the same nominal layer; you **drag the dividers** between them to split the layer height (each pass gets its own Z share). Per pass you can also move it **up/down** in the stack.

Each pass exposes:
- a **kind** (see §1a),
- a **Z mm** height box,
- an **angle** box (`-1 = auto`; scroll the wheel over the box to rotate). For a PathBlend pass this becomes **ramp end**,
- an **Advanced ⚙** / **Edit…** button to open that pass's detailed settings (a `*` marks non-default values).

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
| **PathBlend Half** | A gradient pass occupying half the height share — fades between filaments across the surface (§1c). |
| **PathBlend Full** | A gradient pass occupying the full height of its slot. |

**MultiPass = multiple Solid passes.** There is no separate "MultiPass" button anymore — you simply add Solid passes and split the height between them with the dividers. Aim for the height shares to add up to the full layer for full coverage.

PathBlend is always a single full-height gradient — when you pick a PathBlend kind the stack collapses to one pass.

---

### 1b. ColorStitch pass — per-line color patterns

A **ColorStitch** pass decides which filament prints each fill line. Open its **Edit gradient…** button to configure it. The pattern is chosen with the **Style** dropdown:

| Style | What it produces |
|-------|------------------|
| **Custom text pattern** | You type a string of colour digits using the colour buttons; the slicer loops it across lines (line 1 → first digit, etc.). For exact, repeating stripes. |
| **Smooth blend — 2 colours** | Two filaments distributed across the surface with a percentage split, dithered so the transition looks smooth. The most common choice. |
| **Smooth blend — 3 colours** | Three filaments at configurable percentages; the middle colour concentrates in the centre. |
| **Stripes — manual band sizes** | Explicit band counts: N lines of Colour 1, M of Colour 2, … repeating. |

**Colours used** — pick **Color 1–4** (each maps to a loaded filament).

**Blend controls** (smooth-blend styles):

| Control | What it does |
|---------|--------------|
| **How much Color 1 / Color 2** | The percentage split. (In 3-colour, *Color 3 fills the rest* automatically.) |
| **Color overlap (soft ← hard zones)** | How much colours bleed into each other in 3-colour blends. |
| **Transition shape** | Even (same density everywhere) · Slow start · Slow end · S-curve (smooth start & end) · Custom shape (set **γ**) · Hard step. |
| **Skip tiny areas** | Surfaces with fewer than N fill lines use Color 1 only. |
| **ColorStitch min. line length** | Fill lines shorter than this (mm) are skipped, so they keep the surrounding colour and avoid toolchanges on tiny segments. Default **0** (don't skip). |
| **Invert direction ⇆** | Reverses the per-line sequence after generation. |
| **Infill angle override** | `-1 = Auto`. Scroll the **mouse wheel over the pass preview bar** to rotate it live — the bar's stripes rotate with it. A **fixed** angle (≥ 0) is now honoured **exactly** in the G-code (the per-layer fill rotation is locked out for that pass), so the print keeps the angle you set. `-1 = Auto` lets the slicer alternate per layer (uniform finish, but the orientation won't match a static preview). |
| **Gradient repetitions** | `1 = single`; higher repeats the pattern across the surface. |

A live **preview** shows the resulting gradient to scale, plus an estimate of how many lines a 60×60 mm surface would have at your filament width.

> **MixedFilament note.** If the pattern uses a MixedFilament digit (5–9) or an active MixedFilament recipe, the gradient options are disabled — *the MixedFilament is the pattern.*

**⚠️ Required fill pattern: Monotonic Line.** ColorStitch only works correctly with the **MonotonicLine** top-surface fill pattern, because the slicer pre-splits the surface into individual straight paths before assigning tools. Monotonic and Rectilinear look fine on simple convex shapes but mis-sequence on complex objects. Set **Quality → Top surface pattern → Monotonic Line** before using ColorStitch.

---

### 1c. PathBlend pass — smooth gradient

A **PathBlend** pass creates a **continuous gradient** across the surface: one filament dominates at one edge, another at the opposite edge, with proportional flow path-by-path in between — all within a single layer. Choose **Half** or **Full** height when picking the kind, set the pass's **ramp end** (its angle box becomes the gradient direction control), and use **Edit…** for the gradient details (number of passes/filaments, min/max flow, easing, invert).

The gradient runs across the build-plate **Y axis** — rotate the object to change direction. PathBlend works best on surfaces with many fill lines; on small surfaces the gradient is coarse. It shares the **Line distribution mode** (§1f) — if a gradient looks broken across holes, try **LaneQuant** or **DirCluster**.

> ⚠️ **PathBlend is the most fragile part of the engine.** Its per-scanline staircase model is validated and must not be disturbed by unrelated changes.

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

**Q: I want to try NeoArachne safely.**

Enable Libre Mode, set **Quality → Wall generator → NeoArachne**, leave the new controls at defaults (Neotko Hybrid v2), and slice. Switch back to Arachne/Classic anytime — the NeoArachne controls don't touch existing settings.

---

*Snapmaker Orca — Neotko FullSpectrum Feature Pack*
*Features designed by Neotko · Implementation by Claude (Anthropic)*
