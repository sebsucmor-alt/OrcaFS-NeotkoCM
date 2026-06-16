# OrcaSlicer FullSpectrum — Neotko Feature Pack · User Guide

> Features conceived and designed by **[Neotko](https://github.com/neotko)** — inventor of *Neosanding*, now known as **Ironing** in OrcaSlicer, PrusaSlicer, Bambu Studio and Cura.

This fork adds a set of surface quality, color blending and workflow features on top of OrcaSlicer FullSpectrum (Snapmaker base). This guide explains what each feature does and how to use it — no programming knowledge required.

---

## Philosophy — the Sandwich

Everything in this fork revolves around one idea: a **Sandwich** of layers you build yourself.

A normal slicer treats the top of your part as one thing — one filament, one pattern, one pass. The Sandwich breaks that open. Each top (and penultimate) surface becomes a stack of independent layers, and you decide what each layer is made of:

- A **stripe pattern** of two filaments — line by line (ColorMix).
- A **stack of full passes** with different tools, angles, speeds and Z heights — like glazes (MultiPass).
- A **continuous gradient** that smoothly fades between two, three or four filaments across the surface (PathBlend).
- A **mechanical interlock** that nests successive layers into each other (Neoweaving).
- And — when you want different recipes on different parts of the same object — a **3D Painter** that lets you brush Sandwich profiles onto specific faces.

All of this is hosted by a single dialog: the **Sandwich dialog** (Quality → ColorMix & Multi-Pass Blend → **Edit…**, internally called the SandwichDialog). One place, two zone cards (Top / Penultimate), pills to pick the effect, advanced dialogs to tune the recipe, a profile system to save your favourite stacks, and a 3D Painter to apply them where you want.

The goal is not to give you "presets that work" — it is to give you a **playground**. Mix, stack, experiment, save profiles, paint them, and see what kind of surface comes out. The features described in this guide are the building blocks; the combinations are yours.

Beyond surface effects, the fork also adds a **new wall-generation engine** — **NeoArachne** (§8) — that sits next to the stock Classic and Arachne engines and lets you pick which one prints each kind of wall on your part, with extra controls for bead width, edge closure and a real-time Preview Lab. It's optional and disabled by default.

It also ships **NeoTower** (§9) — a post-slice wipe-tower planner that sees every toolchange (including Sandwich sub-layer primes) before committing to geometry, giving you a fixed, tidy tower even with adaptive layers, multi-tool scenes and Sandwiches at the same time.

The rest of this document describes each building block in detail.

---

## What's new since 1.9 (this is a WIP beta)

> This build is a **work-in-progress beta**. Some surfaces show a **(WIP Beta)** marker. The features below are usable but still being refined — review your G-code before long prints.

- **ColorStitch — the new name.** The "Surface Color Mixer" family is being rebranded to **ColorStitch** in the UI. The Sandwich dialog now hosts a **ColorStitch Studio** (§1g) and the 3D painter is now the **ColorStitch Painter** (§7c). Internal names, config keys and 3MF data are unchanged, so old projects keep working — only the labels you see are different. You will still see the word *ColorMix* in a few places (e.g. the per-line effect pill); they refer to the same thing.
- **ColorStitch Studio** (§1g): generate whole palettes — **Gradient ramp**, **Flat colour** and **Mixed approximation (predict)** — directly from your loaded filaments and their TD, then click a swatch to load that recipe into the Sandwich.
- **ColorStitch Painter revamp** (§7c): the painter shows those same palettes as **collapsible colour strips**. Pick a swatch and paint — no need to pre-build profiles. A two-tier model keeps working colours separate from the palettes you deliberately save.
- **NeoTower** (§9): tower-type selector, zigurat taper, Sandwich purge compaction, and a unified **SurfaceColorStitch wipe reserve**. NeoTower is what makes adaptive layer height + multi-tool + Sandwich coexist on one tower.
- **Beta defaults / changes:** *SurfaceColorStitch wipe reserve* default raised to **10 mm³**, *Sandwich purge compaction* default **1.7**, and the experimental **Micro Stitch (Neotko)** top/bottom fill pattern is **hidden** in this beta (existing presets that use it still load).

**Refined in 2.1**

- **ColorStitch Painter — Pro mode & live editing** (§7c): the Pro panel **is** the active colour — compose Top/Penu rows (Solid / ColorStitch / PathBlend) with a per-pass Z box and paint with them directly. A new **Pick** (eyedropper) tool reads a recipe straight off the model, **Pin to palette + Name** replaces the old save buttons, editing a linked profile **rewrites it in place**, and destructive-mouse fixes mean **right-click is camera only**.
- **Gradient is now a clean top-only helper** (§1g): the Studio/painter gradient sweeps the A/B top split only — the weave/dither **Pattern** controls were removed and no penultimate ColorStitch is attached (a same-tool gradient now correctly predicts that tool's colour). Add a penu tone yourself if you want one.
- **"Mixed approximation" strip removed from the painter**; **Gradient + Flat** remain. (Mixed approximation is still available in the Sandwich-dialog Studio.)
- **Blend Suggestion retired** (§1e): the old Beer-Lambert "Calculate" panel is gone; the inverse colour match now lives in **ColorStitch Studio → Match ▸** (ΔE2000).
- **Wipe-tower colour fix:** fixed a `;WIDTH:` G-code desync where a same-width tool change after the tower made the viewer render the next colour at the tower's line width.

---

## Table of Contents

1. [Surface Color Mixer](#1-surface-color-mixer)
   - 1a. [ColorMix — per-line color patterns](#1a-colormix--per-line-color-patterns)
   - 1b. [MultiPass Blend — multiple passes per layer](#1b-multipass-blend--multiple-passes-per-layer)
   - 1c. [PathBlend — smooth gradient across the surface](#1c-pathblend--smooth-gradient-across-the-surface)
   - 1d. [Zone and filament filters](#1d-zone-and-filament-filters)
   - 1e. [TD Preview](#1e-td-preview)
   - 1f. [Line Distribution Mode (CM + PB)](#1f-line-distribution-mode-cm--pb)
   - 1g. [ColorStitch Studio — palette generators](#1g-colorstitch-studio--palette-generators)
2. [Neoweaving — mechanical interlocking of layers](#2-neoweaving--mechanical-interlocking-of-layers)
3. [Penultimate Top Layers](#3-penultimate-top-layers)
4. [Monotonic Interlayer Nesting](#4-monotonic-interlayer-nesting)
5. [Libre Mode](#5-libre-mode)
   - 5a. [Floating objects](#5a-floating-objects)
   - 5b. [World-space import](#5b-world-space-import)
   - 5c. [Temporal Link — grouping objects](#5c-temporal-link--grouping-objects)
   - 5d. [Per-volume XY compensation](#5d-per-volume-xy-compensation)
   - 5e. [Bridge infill disable](#5e-bridge-infill-disable)
   - 5f. [Detachable Process Panel](#5f-detachable-process-panel)
6. [S3DFactory — Simplify3D project import](#6-s3dfactory--simplify3d-project-import)
7. [Surface Effect Profiles & 3D Painter](#7-surface-effect-profiles--3d-painter)
   - 7a. [Saving profiles from the dialogs](#7a-saving-profiles-from-the-dialogs)
   - 7b. [Managing profiles (load, update, rename, delete)](#7b-managing-profiles-load-update-rename-delete)
   - 7c. [The ColorStitch Painter gizmo](#7c-the-colorstitch-painter-gizmo)
   - 7d. [Painter mode at slice time](#7d-painter-mode-at-slice-time)
   - 7e. [Penu role autonomy](#7e-penu-role-autonomy)
   - 7f. [Profile persistence and 3MF round-trip](#7f-profile-persistence-and-3mf-round-trip)
   - 7g. [Known limitation — PathBlend on Penu](#7g-known-limitation--pathblend-on-penu)
8. [NeoArachne — alternative wall generator](#8-neoarachne--alternative-wall-generator)
   - 8a. [Turning it on](#8a-turning-it-on)
   - 8b. [Per-feature engine choice](#8b-per-feature-engine-choice)
   - 8c. [Edge Closure controls](#8c-edge-closure-controls)
   - 8d. [NeotkoEdge — stable bead transitions](#8d-neotkoedge--stable-bead-transitions)
   - 8e. [Preview Lab](#8e-preview-lab)
9. [NeoTower — post-slice wipe tower](#9-neotower--post-slice-wipe-tower)
   - 9a. [Tower type](#9a-tower-type)
   - 9b. [Zigurat taper](#9b-zigurat-taper)
   - 9c. [Sandwich purge compaction](#9c-sandwich-purge-compaction)
   - 9d. [SurfaceColorStitch wipe reserve](#9d-surfacecolorstitch-wipe-reserve)
   - 9e. [Adaptive layers × multi-tool × Sandwich](#9e-adaptive-layers--multi-tool--sandwich)

---

## 1. Surface Color Mixer — the Sandwich dialog

The **Sandwich dialog** (historically *Surface Color Mixer*) is the umbrella for everything that happens on **top and penultimate surfaces** when you have more than one filament loaded. It lives under **Quality → ColorMix & Multi-Pass Blend** in the process settings, and has a single "Edit…" button that opens it.

This is the entry point for the whole Sandwich philosophy: pick a recipe per surface, stack effects across surfaces, save the result as a profile, and reuse it across prints or paint it onto specific parts of a model with the 3D Painter (§7). Throughout this guide, **"Sandwich"** refers to the combined output of these effects on a single top + penultimate stack.

### Dialog layout

The dialog has two independent **zone cards** — one for the **Top** surface and one for the **Penultimate** surface. Each zone is configured entirely on its own. Together they form one *Sandwich* over your fill.

At the top of each zone card are four **pill buttons**:

| Pill | Effect |
|------|--------|
| **None** | No effect on this surface — normal single-filament top fill |
| **ColorMix** | Alternates filaments line by line following a repeating pattern |
| **MultiPass** | Reprints the surface 1–3 times, each pass with its own filament, angle, speed and Z |
| **PathBlend** | Creates a smooth color gradient across the surface |

Click a pill to select the effect for that zone. A **mini-preview** below the pills shows a visual representation of what the selected effect will produce. Click **Advanced…** to open the configuration dialog for the selected effect (disabled when None is selected).

Below the two zone cards there is a **TD Preview** section (collapsable) and a compact **Filament** section. These are described in §1d and §1e.

ColorMix and MultiPass are mutually exclusive within the same zone. PathBlend is independent and can be configured for either zone regardless of whether MultiPass is active on the other zone.

---

### 1a. ColorMix — per-line color patterns

**What it does**

ColorMix decides which filament prints each fill line on a top or penultimate surface, line by line. With the right configuration it can produce stripes, dithered gradients, hard color bands, or repeating custom patterns. Every real toolchange goes through the wipe tower exactly as normal.

ColorMix has **five pattern modes**, selectable from the Advanced dialog. Top and Penultimate surfaces are configured independently — each can be in a different mode.

#### Pattern modes

| Mode | What it produces | When to use |
|------|------------------|-------------|
| **0 — Pattern string** (legacy) | You write a string of tool numbers (`1212`, `11223`, `1234`) and the slicer loops it across lines. Line 1 → T1, line 2 → T2, etc. | Repeating geometric patterns where you want exact, predictable stripes. |
| **1 — Linear 2-color (dithered)** | Two filaments distributed across the surface with a percentage split (e.g. 60% T0, 40% T1). The mix uses Bresenham-style dithering so the transition looks smooth, not blocky. | Two-color gradients (sky → ground, light → dark). The most common use. |
| **2 — Linear 3-color (dithered)** | Three filaments at configurable percentages (e.g. 40% / 30% / 30%). T1 is concentrated in the middle of the gradient, T0 and T2 dominate the outer thirds. | Three-stop gradients (sunset / spectrum-like effects). |
| **3 — Custom bands** | Explicit hard-band counts: emit N lines of tool A, then M of tool B, then K of tool C, then L of tool D, cycling. | Repeating multi-band patterns when you want exact line counts per band, not a percentage. |

(Modes 4 and 5 of the **Easing** dropdown are separate — see below.)

#### Gradient shape — Easing curves (Linear modes)

The **Easing** dropdown (Linear 2-color and 3-color modes only) controls how the colour density transitions across the surface:

| Easing | Behaviour |
|--------|-----------|
| **0 — Linear** | Constant ratio across the gradient. Default for crisp stripes. |
| **1 — Ease-In** | Slow start, fast end (t²). Tool B becomes dense at the far edge. |
| **2 — Ease-Out** | Fast start, slow end (1−(1−t)²). Tool B dense at the start. |
| **3 — Ease-In-Out** | Smoothstep — symmetric, gentle at both ends, fast in the middle. Most natural-looking soft transitions. |
| **4 — Gamma** | Custom power curve using the **Gamma** value below. γ = 1.0 = linear, γ > 1 biases toward Tool A, γ < 1 biases toward Tool B. |
| **5 — Hard band** | No dither — clean blocks of A then B with no probabilistic mixing. |

#### Other gradient controls

| Control | What it does |
|---------|-------------|
| **Color overlap** (0.0–1.0) | How much each colour bleeds into its neighbour's zone in Linear 3-color mode. `0.0` = hard 3-zone split. `1.0` = strong overlap (every colour sprinkles throughout). Default `0.6`. |
| **Invert gradient direction** | Reverses the per-line tool sequence after dither/band generation. Use when the slicer's natural fill direction looks mirrored relative to what you want — easier than swapping tools manually. No effect in Pattern-string mode. |
| **Min surface lines** | Surfaces with fewer than N fill lines fall back to a single tool (Tool A) instead of producing a degenerate split. Default `3`. Set to `0` to never fall back. |
| **Min. line length** | Fill lines shorter than this (mm) are skipped — prevents toolchanges on tiny path segments. Default `1.0 mm`. |

#### How to set it up

1. Open **Quality → ColorMix & Multi-Pass Blend → Edit…**
2. Click the **ColorMix** pill for the Top surface, the Penultimate surface, or both.
3. Click **Advanced…** to open the pattern editor.
4. Pick a **Pattern mode** (0–3). The dialog reveals only the controls relevant to that mode.
5. Configure tools, percentages, easing, and gradient options as needed.
6. Set the **Zone** (see §1d), **Min surface lines**, and **Min. line length**.
7. Choose a **Line distribution mode** (see §1f below) for how slots are mapped to physical lines.
8. Click OK and slice.

The **strip preview** in the dialog shows a real-time preview of what the chosen mode will produce on a typical top surface — colors, dither pattern, band ratios all rendered to scale.

**⚠️ Required fill pattern: Monotonic Line**

ColorMix **only works correctly with the MonotonicLine fill pattern**. This is a technical requirement for complex objects.

- **MonotonicLine** ✅ — the slicer pre-splits the surface into individual straight paths before ColorMix runs. Every path is visited and assigned a tool, including irregular, concave, or asymmetric shapes.
- **Monotonic** ⚠️ — works acceptably on simple convex surfaces (a square, a circle). On complex objects with holes, concavities, or disconnected regions, the path ordering is finalised *after* ColorMix assigns tools, so visual toolchanges end up in the wrong sequence.
- **Rectilinear** ⚠️ — same problem. Works on simple shapes; fails visually on anything more complex than a basic rectangle.

**Set your top surface fill pattern to MonotonicLine before enabling ColorMix.** The setting is in Quality → Top surface pattern.

---

### 1b. MultiPass Blend — multiple passes per layer

**What it does**

Instead of printing the top surface once, MultiPass prints it **1, 2, or 3 times** within the same layer. Each pass:
- Uses a **different filament** (optional — you can repeat tools)
- Has its own **fill angle** (e.g. pass 1 at 0°, pass 2 at 90°, creating a cross-hatch)
- Has its own **speed** and **fan** settings
- Has its own **line width ratio** — each pass is narrower so they tile together without over-extruding (the sum of ratios should be around 1.0 for full coverage)
- Prints at a **slightly different Z height** proportional to its width ratio — passes are physically stacked as thin virtual sub-layers within the same nominal layer, which improves adhesion and color separation

**Common uses**
- **Cross-hatch texture**: two passes at perpendicular angles with two different colors
- **Neosanding evolved**: one main pass at full ratio + one glaze pass at low ratio with a silk filament
- **Color blending**: two passes at slight ratios (e.g. 0.5 + 0.5) with different colors, which the eye blends

**How to set it up**

1. Open **Edit…** → click the **MultiPass** pill for the desired zone (Top or Penultimate).
2. Click **Advanced…** to open the MultiPass configuration dialog for that zone.
3. Set the number of passes (1, 2 or 3).
4. For each pass: pick a tool, set the width ratio, angle, fan and speed.
5. The **Σ** indicator in the dialog shows the sum of all ratios — aim for ~1.0 for full coverage.
6. OK → slice.

**Additional per-pass options**

| Option | What it does |
|--------|-------------|
| **GCode start / end** | Custom GCode script injected before or after each individual pass |
| **PA mode / PA value** | Override Pressure Advance for a specific pass (useful for passes with very different speeds or widths) |
| **Vary pattern** | Shifts the fill pattern slightly between passes to reduce moire effects |
| **Prime volume** | Amount of filament (mm³) to prime through the wipe tower before the first sublayer of this zone. Prevents underextrusion at the start of a pass after a toolchange. Default: **5 mm³**. Set to 0 to disable. |

**1-pass mode**: With a single pass at ratio < 1.0, MultiPass deposits a controlled low-flow layer over the existing surface at a precise Z offset. Useful for glazing or experimental material effects without adding a full extra layer.

**Top and Penultimate are independent**: MultiPass on the Top zone and MultiPass on the Penultimate zone each have their own filament, ratio, angle and prime volume settings. Changing one does not affect the other.

---

### 1c. PathBlend — smooth gradient across the surface

**What it does**

PathBlend creates a **continuous color gradient** across the top surface. At one edge of the surface, filament T1 dominates. At the other edge, filament T2 dominates. In between, both filaments are printed at proportional flow, path by path, so the transition is as smooth as the number of fill lines allows.

This is done entirely within a single layer — no extra layers are needed. The slicer adjusts the Z height and extrusion flow of each fill line to blend the two tools.

With 3 or 4 passes, three or four filaments can blend across the surface in sequence.

PathBlend is **independent of MultiPass** — it does not require MultiPass to be enabled. You can activate PathBlend directly from its pill in either zone.

**How to set it up**

1. Open **Edit…** → click the **PathBlend** pill for the desired zone.
2. Click **Advanced…** to open the PathBlend configuration dialog.
3. Pick the number of passes (2–4) and assign a filament to each slot (T1, T2…).
4. Configure the gradient options (see table below).
5. OK → slice.

**PathBlend options**

| Option | What it does |
|--------|-------------|
| **Num passes** | 2, 3, or 4 filaments in the gradient |
| **Min ratio** | Minimum flow percentage of the receding filament at the extreme edges (default 5%). Keeping this above 0 means both filaments are always present to some degree across the full surface. |
| **Max ratio** | Maximum flow of the dominant filament at the peak of its range (51–100%). Reducing this ensures neither filament ever overwhelms the other, keeping a minimum presence of both at all points in the gradient. |
| **Ease mode** | Controls the acceleration curve of the gradient: **Linear** (uniform), **Ease In** (slow start), **Ease Out** (slow end), **Ease In-Out** (slow at both ends, fast in the middle). |
| **Invert gradient** | Flips which tool dominates on which side, without rotating the object. |
| **Fill angle** | Override the fill direction (−1 = auto). |
| **Surface** | Apply to Top only, Penultimate only, or both. |

**Gradient direction**: The gradient always runs across the **Y axis of the build plate** (front-to-back). Rotate your object on the bed to change which direction the gradient crosses the surface.

**Note**: PathBlend works best on surfaces with many fill lines (high infill density, wide surfaces). On very small surfaces with only a handful of lines, the gradient will be coarse. Within-path subdivision for finer gradients is planned for a future version.

**Line distribution**: PathBlend uses the same **Line distribution mode** as ColorMix (see §1f). If your top surface has holes, islands or rotated sub-regions and the gradient looks broken or patchy across the gaps, try setting the mode to **LaneQuant** (2) or **DirCluster** (3).

---

### 1d. Zone and filament filters

These controls appear in the **Filament** section at the bottom of the Sandwich dialog. They apply to whichever effect is active on each surface.

#### Zone — All surfaces vs. Topmost only

On many models, the "top surface" isn't just one layer — it appears on every horizontal face, including intermediate steps and ledges. This is geometrically correct: each flat upward-facing area has its own top surface.

The **Zone** selector lets you restrict the effect:

| Setting | Behaviour |
|---------|-----------|
| **All surfaces** (default) | Effect applies on every top (or penultimate) surface in the model, at any height |
| **Topmost only** | Effect applies only on the very topmost horizontal surface of the object — the one with nothing above it |

This filter is available independently for Top and Penultimate surfaces.

**When to use Topmost only**: When you have a staircase or stepped object and you want the gradient or color pattern only on the top of the whole part, not on every step.

#### Filament filter

On multi-material objects, different regions may already be assigned to different filaments. The **Filament filter** spinner (0–16) lets you apply the surface effect only to regions assigned to a specific filament number.

- `0` = no filter, effect applies to all regions (default)
- `N` = effect applies only to regions whose solid infill filament is N

**Example**: If your object has a red body (filament 1) and a white logo (filament 2), set the filament filter to `1` to apply ColorMix only to the red regions, leaving the white logo untouched.

---

### 1e. TD Preview

The **TD Preview** section (collapsable, at the bottom of the Sandwich dialog) lets you visualize how your filaments will visually combine when one layer sits on top of another.

> **Changed in 2.1:** the old **Blend Suggestion — Beer-Lambert optimizer** panel (legacy "Calculate") has been **retired**. Its job — find the recipe that best matches a target colour — is now done by **Target + Match ▸** in the **ColorStitch Studio** (§1g), which uses the newer ΔE2000 colour-science engine and writes the result straight into the live Sandwich. Use that instead.

#### Transmission Density (TD)

Each filament has a **Transmission Density** (TD) value that describes how opaque it is:

| TD range | Type |
|----------|------|
| 0.1 – 0.5 | Highly opaque — 1–2 passes cover the lower color completely |
| 0.5 – 3.0 | Opaque-translucent — some of the lower layer shows through |
| 3.0 – 7.0 | Translucent — needs several passes to block the lower layer |
| 7.0 – 10+ | Highly translucent — the lower color is almost always visible |

**Low TD = opaque. High TD = translucent.**

There are four **TD sliders** (one per filament slot). These are saved per machine, not per print — they describe your actual filaments, not the current print profile.

#### Color preview

Below the TD sliders, three swatches show:

- **Top swatch** — the blended visual result of the Top surface passes (all filaments and ratios combined, weighted by their TD)
- **Penu swatch** — the blended visual result of the Penultimate surface passes
- **Result swatch** — the final visual result as it will appear on the print: the Penultimate color showing through the Top passes according to their opacity

An **opacity_top** label shows the computed opacity of the Top layer stack (0 = fully transparent, 1 = fully opaque).

> Looking for the old **Calculate** / inverse colour-match? It moved to the **ColorStitch Studio → Target + Match ▸** (§1g). See the note at the top of this section.

---

### 1f. Line Distribution Mode (CM + PB)

**What it does**

The **Line distribution mode** controls *how* the slicer maps your colour assignments (ColorMix slots or PathBlend pass slots) to the **physical fill lines** on a real surface. It does not change the gradient/pattern itself — only how the slots find which lines belong to which "lane" in space.

Different surface geometries need different mappings. A flat rectangle is easy: line index = position. But a top surface with concavities, holes, or rotated sub-regions can have lines printed in an order that doesn't match their spatial position, which makes a gradient look wrong even when the math is correct.

The setting lives in **Quality → Line distribution mode** (Advanced visibility) and affects **both ColorMix and PathBlend** at the same time.

#### The four modes

| Mode | Algorithm | Best for |
|------|-----------|----------|
| **0 — Default** | `slot = line_index % n_slots`. Legacy behaviour. Uses raw print order. | Simple rectangular surfaces. Repeating Pattern-string mode. Anything where print order ≈ spatial order. |
| **1 — GeoSort** | Sort lines geometrically by their centroid position perpendicular to the fill direction *before* assigning slots. | Surfaces where print order is scrambled but the spatial direction is clean. Typical mid-complexity tops. |
| **2 — LaneQuant** | Quantise each line into a discrete "lane" by perpendicular position, then collapse lines in the same lane to the same slot. Most geometric — fragmented stripes stay the same colour. | Surfaces with **holes, concavities, or disconnected sub-regions**. When a stripe is broken into multiple physical fragments by an island, LaneQuant keeps them all the same colour. The recommended default for complex top surfaces. |
| **3 — DirCluster** | Detect groups of lines that share a fill direction (the slicer sometimes rotates direction for sub-regions to avoid bridge angles), cluster them, then run LaneQuant inside each cluster independently. | Surfaces where the fill engine rotated direction within the same layer (e.g. multiple disconnected islands at different angles). Each sub-region keeps its own coherent gradient instead of one global lane grid. |

#### How to choose

There is no single best mode for every print. A rough decision tree:

1. **Plain rectangular tops, simple shapes** → leave as **Default** (0).
2. **Top has a single visible gradient direction but the result looks scrambled** → try **GeoSort** (1).
3. **Top has holes, islands, or concavities; gradient breaks into "wrong colour" patches** → use **LaneQuant** (2). This is the most common upgrade from Default.
4. **Object has multiple disconnected top regions or the slicer rotated fill direction per region** → use **DirCluster** (3).

**Quick rule**: If the gradient looks visually wrong, increase the mode number by one and re-slice. Each step adds geometric awareness at the cost of slightly more compute.

#### Interaction with ColorMix and PathBlend

- **ColorMix**: the lane mode decides which physical line gets which slot from the dither sequence or pattern string. With LaneQuant or DirCluster, fragmented stripes survive across holes.
- **PathBlend**: the lane mode decides each line's `surface_t` (position 0..1 in the gradient). With LaneQuant, two fragments at the same spatial Y get the same `surface_t`, so they print the same flow ratio and the gradient stays coherent across the gap.

**Default mode and PathBlend**: when PathBlend is on with mode = Default (0), the slicer falls back to a Y-axis bounding-box centroid for `surface_t`. This is the safe path and works for most prints, but it produces coarser gradients when the surface has irregular shape. LaneQuant gives noticeably smoother gradients on irregular tops.

**Tip**: the setting is per-print-profile but applies to the **entire object's top + penu surfaces**. If you have mixed-complexity surfaces in one object, pick the mode that satisfies the most complex one — the others won't be hurt.

---

### 1g. ColorStitch Studio — palette generators

> **New in this beta.** The Studio lives inside the Sandwich dialog (ColorStitch Studio panel). It turns "design a recipe by hand" into "pick a colour from a generated palette."

**What it does**

Instead of configuring passes one by one, the Studio **generates a whole strip of colour swatches** from the filaments you have loaded and their TD values, using the same colour-science engine as the TD Preview. Each swatch is a complete Sandwich recipe (a Top/Penu pass stack) with its predicted colour already computed. Click a swatch and that recipe is **loaded into the live Sandwich** (the zone cards repopulate), ready to slice, tweak, or save as a profile.

**The three modes**

| Mode | What it generates |
|------|-------------------|
| **Gradient ramp** | A manual **top-only** ramp between two tools (A → B). You pick the two filaments, the number of **steps** and the **split range**; the Studio sweeps the A/B thickness split across the steps. Clicking a swatch toggles whether it's included when you **Export palette**. *(Changed in 2.1: the gradient is now a pure top-surface helper — the weave/dither **Pattern** controls were removed and no penultimate ColorStitch is attached. If you want a penu tone underneath, add it yourself afterwards.)* |
| **Flat colour (predict)** | Browses the gamut reachable by **stacking solid passes** of your filaments (1–2 solids, swept by thickness). Robust, predictable colours. |
| **Mixed approximation (predict)** | Browses an **extended gamut**: a dithered ColorStitch base (penultimate) plus a translucent solid on top. This reaches colours no single filament can make — the optical average of the dither acts like a new primary. |

**Target + Match**

Pick a **MixedColor target** and press **Match ▸**. The Studio runs an inverse search (minimising ΔE2000) and loads the closest achievable recipe, showing the resulting ΔE. Use it when you have a specific colour in mind rather than browsing.

**Export palette**

The **Export palette…** button turns swatches into saved Surface Effect Profiles (§7) so the painter and profile manager can use them. In Gradient mode it exports the swatches you marked; in the predict modes it exports the whole strip. Give the batch a base **Name** first.

**Live TD**

The strips react to your **TD sliders**: change a filament's TD and the palette regenerates (debounced) with the new predicted colours, because the colour is baked into each recipe.

**Print-friendly minimums**: the predict generators respect minimum printable pass thicknesses (a too-thin top on a visible face prints badly), so the swatches they offer are ones that actually print. The manual Gradient mode lets you go thinner at your own risk.

---

## 2. Neoweaving — mechanical interlocking of layers

**What it does**

Neoweaving alternates the Z height of successive fill lines on each layer. Instead of all lines printing at the same Z, odd lines print at the nominal height and even lines print slightly higher (by the configured *amplitude*). The next layer has the pattern inverted — its elevated lines fit into the gaps left by the layer below.

The result is **mechanical interlocking** between layers: the elevated lines of one layer nestle into the recesses of the layer below, like puzzle pieces. This improves inter-layer adhesion and vibration damping in functional parts, without changing the external dimensions of the object.

This is **not** a visual effect — it's a structural technique. The difference is measurable in layer separation force and in vibration-damping tests.

**How to set it up**

Go to **Quality → Neotko Neoweaving** and enable it. Key parameters:

| Parameter | What it does |
|-----------|-------------|
| **Amplitude** | How much higher the elevated lines go (mm). Typical range: 0.05–0.2 mm. Higher = stronger interlock, but requires printer to handle rapid Z moves. |
| **Filter** | `Top only` (only top surface lines) or `All` (includes all solid infill). Start with Top only. |
| **Penultimate layers** | Also apply Neoweaving to the N layers below the top surface (0 = top only). |
| **Min. line length** | Don't apply Neoweaving to lines shorter than this (avoids useless Z moves on tiny features). |
| **Speed %** | Slow down Neoweaved lines if your printer struggles with rapid Z changes. |

**Note**: Wave mode (sinusoidal Z oscillation within each line) is currently **disabled** — it works correctly but can use excessive memory on large surfaces. Only linear mode (alternating flat lines) is available.

---

## 3. Penultimate Top Layers

**What it does**

The layer just below the top surface — the *penultimate* layer — normally behaves like any other solid infill layer. This feature lets you treat it as its own distinct zone, with:
- Its own infill density (can be higher or lower than normal solid infill)
- Its own extrusion role, which means ColorMix, MultiPass, PathBlend and Neoweaving can be configured independently for it

**Why it matters**

The penultimate layer is the foundation for your top surface. If it's printed at a different density or angle, it changes how the top surface looks and feels. In multi-color printing, applying a different color pattern to the penultimate layer creates a depth effect that shows through the top surface lines.

**How to set it up**

Go to **Strength → Top/bottom shells** and set **Penultimate top layers** (default: 1). Setting it to 0 disables the feature entirely. Setting it to 2 makes the two layers below the top surface behave as penultimate.

You can also set a custom **Penultimate solid infill density** independently from the normal solid infill density.

---

## 4. Monotonic Interlayer Nesting

**What it does**

When printing with Monotonic fill, this feature shifts the fill reference point by half the line spacing on every other layer. The practical effect: the fill lines of layer N sit directly over the *gaps* between lines of layer N−1, rather than sitting on top of them.

This improves inter-layer bonding in monotonic fills and can reduce the visibility of layer lines on top surfaces when viewed from a grazing angle.

This feature is **automatic** — it applies to any surface using Monotonic or MonotonicLine fill pattern and requires no configuration.

---

## 5. Libre Mode

Libre Mode is a runtime toggle that unlocks OrcaSlicer for workflows where the normal physics and constraints get in the way. It is designed for **multi-part assemblies**, **professional workflows**, and **experimental printing**.

**How to toggle it**: There is a **Libre Mode** button in the top toolbar (main window). When active, the button is highlighted and additional controls appear.

Libre Mode affects many behaviors. Here's what changes:

---

### 5a. Floating objects

**Libre Mode OFF** (normal): Objects are always snapped to the build plate. You cannot move an object to a Z position above or below the bed — the slicer forces it down.

**Libre Mode ON**: Objects can exist at **any Z height**, including above the bed (floating) or partially below it. The slicer will still generate GCode for them and issue a warning instead of an error if an object has no initial layer.

**When to use this**: Multi-part assemblies where components need to be printed at specific heights; experimental mid-air printing; printing parts that clip into a pre-existing structure already on the bed.

---

### 5b. World-space import

**Libre Mode OFF** (normal): When you import an STL or project file, the object is automatically re-centered to the origin of the build plate.

**Libre Mode ON**: Objects are imported **exactly where they were in the source file's coordinate system**. If an object was at position X=50, Y=100, Z=30 in the source, it appears there on the build plate.

**When to use this**: Importing assemblies where multiple parts have precise relative positions that must be preserved. Importing from Simplify3D `.factory` files (see §6) where world positions matter.

---

### 5c. Temporal Link — grouping objects

**What it does**

Temporal Link lets you create persistent groups of objects that remember their connection across sessions and when saved to 3MF. It is different from the normal multi-select — linked objects have a permanent relationship stored in the file.

**How to use it**

| Action | How |
|--------|-----|
| **Link objects** | Select 2 or more objects, then press **Ctrl+G** |
| **Select all in a group** | Right-click any object → **Select Grouped**, or press **Ctrl+Shift+G** |
| **Break a link** | Right-click → **Break Link** (removes this object from its group) |
| **Break all links in a group** | Right-click → **Break All Links** |

Links are saved inside the 3MF file. When you reopen the project, the groups are restored automatically.

**When to use this**: Keeping multi-part assemblies together when working with many objects on the bed. Quickly selecting all parts of a mechanical assembly to move or scale them together. Organizing a complex print job with many repeated parts.

> Temporal Link requires **Libre Mode ON** to create new links. Existing links in a file can be used with Libre Mode off, but the link creation controls are hidden.

---

### 5d. Per-volume XY compensation

**What it does**

In a normal OrcaSlicer assembly, XY contour compensation (shrinkage correction) is set per-object. All volumes (parts) within the object share the same compensation value.

With Libre Mode ON and an Assembled object, each **volume** (individual mesh) can have its own XY compensation value, independent of the object-level setting. The slicer applies the *delta* between the volume's value and the object's value to that volume's slices before merging.

**When to use this**: Multi-material assemblies where different materials have different shrinkage. For example, a PETG insert inside a PLA shell — each material needs its own compensation to fit correctly after printing.

**How to set it up**: With Libre Mode ON, select a volume inside an Assembled object. The XY compensation controls appear in the per-volume settings panel.

---

### 5e. Bridge infill disable

**What it does**

The slicer normally detects areas with no support below them and applies *bridge infill* — special settings for printing across gaps. On floating objects (§5a) or unusual geometries, this detection can misfire and apply bridge infill where it is not wanted.

When Libre Mode is ON, bridge infill detection is automatically disabled for the entire object. This prevents unwanted bridging behavior on floating geometry.

This happens automatically — there is no manual switch. When Libre Mode is toggled OFF, normal bridge detection resumes on the next slice.

---

### 5f. Detachable Process Panel

**What it does**

With Libre Mode ON, the Process panel (the settings sidebar) detaches from the main window and floats as an independent panel. On a multi-monitor setup, you can drag it to a second screen and keep the 3D viewport on your main screen unobstructed.

The position and docked/floating state are saved between sessions. When you reopen OrcaSlicer with Libre Mode ON, the panel reopens in the same position.

**How to use it**: Enable Libre Mode — the Process panel automatically detaches. Drag it anywhere. To re-dock it, drag it back to the main window edge or restart with Libre Mode OFF.

---

## 6. S3DFactory — Simplify3D project import

**What it does**

This feature lets you open **Simplify3D `.factory` project files** directly in OrcaSlicer. A `.factory` file is a complete project with multiple objects, their positions, and extruder assignments.

When importing:
- All objects and their 3D positions are preserved
- Extruder (tool) assignments per object are imported
- With **Libre Mode ON**, world-space coordinates are preserved exactly (§5b)
- With **Libre Mode OFF**, objects are re-centered to the build plate origin as usual

**How to use it**: Go to **File → Import → Import 3D model** (or drag and drop). Select your `.factory` file. OrcaSlicer will import all objects from the project.

**When to use this**: Migrating complex multi-part projects from Simplify3D to OrcaSlicer without having to manually re-place every object.

---

## 7. Surface Effect Profiles & 3D Painter

**What it does**

This system lets you **save your Sandwich configurations as named profiles**, then **paint them directly onto specific surfaces of your 3D model** using a brush-based gizmo. Different parts of the same object can have different Sandwiches (e.g. one stair step uses ColorMix with a red/blue stripe pattern, another stair uses MultiPass with three filaments, a third uses PathBlend gradient).

This is where the Sandwich philosophy reaches its full expression: instead of one global recipe per print, every region of every object can carry its own stack of effects, and the whole thing travels inside the 3MF.

Profiles are saved inside the **3MF project file**, so they travel with the print. Painted areas are saved per-volume facet annotations, again persisted in the 3MF.

At slice time, when an object has any painted facets, **painter mode activates**: the preset Surface Color Mixer settings are ignored for that object, and each painted area is rendered using its own profile. Unpainted areas of a painted object get **no surface effect**. Painted areas of an unpainted object fall back to preset mode.

This is the cleanest way to apply **multiple, different surface effects to a single object** without splitting the mesh or duplicating geometry.

---

### 7a. Saving profiles from the dialogs

Every effect dialog has a **"Save as profile…"** button:

| Dialog | What it captures |
|--------|-----------------|
| **Sandwich dialog** (the main dialog) | Captures the **full Sandwich** — whichever effects are currently active in the pills. CM+MP, CM only, MP only, PB only, etc. The saved profile is a bundle of every enabled effect on Top/Penultimate. |
| **MultiPass Advanced** | Captures **MultiPass settings only** (Top + Penu MP keys). No ColorMix or PathBlend in the saved profile. |
| **PathBlend Advanced** | Captures **PathBlend settings only**. No ColorMix or MultiPass in the saved profile. |

**How it works**

1. Configure the effect(s) you want in the dialog (pick pills, click Advanced, set values).
2. Click **Save as profile…** at the bottom of the dialog.
3. Enter a name (e.g. "Red-Blue Stripe", "MP3-Gradient", "PB Linear").
4. A confirmation popup shows how many keys were captured per effect (e.g. `Saved profile #5 (CM:41, MP:53, PB:0 keys)`).

**Effect on the saved profile**

- The profile records **which surfaces (Top, Penu, or Both)** the effect was active on, based on the pill state when you clicked Save. If only the Top pill had MP on, the saved profile applies MP to top only, never to penu.
- Effects with their pill set to **None** are **not** captured.
- The MP and PB dialogs always save the effect they belong to as Top-active by default, since the dialog itself implies intent.

---

### 7b. Managing profiles (load, update, rename, delete)

In the Sandwich dialog, next to "Save as profile…" there is a **Manage profiles…** button. Opens a list dialog with:

| Button | Behaviour |
|--------|-----------|
| **Load into dialog** | Restores the selected profile's values into the Sandwich dialog. The pills, zone settings, filament filter, and Advanced sub-dialog values all switch to match the profile. From here you can tweak and re-save. |
| **Update from current** | Overwrites the selected profile's payloads with whatever is currently active in the Sandwich pills. Effects toggled off in the pills are cleared from the profile. |
| **Rename** | Edit the profile name. |
| **Delete** | Remove the profile from the manager. Painted areas referencing this profile become "orphan" — they will print with no effect (see warning below). |
| **Close** | Dismisses the dialog. |

The profile list shows each profile with a tag `[CM:* PB:* MP:*]` where `*` means the payload is present, `-` means absent. Quick visual check of what each profile contains.

**Orphan warning**: If you delete a profile that has painted areas in your model, the slicer will emit a **non-critical slicing warning** at slice time: *"Object 'X' has painted regions referencing deleted Surface Effect Profile(s)…"*. The slice continues without error — those areas just get no effect. To fix, either re-paint with an existing profile or re-create the deleted profile.

---

### 7c. The ColorStitch Painter gizmo

> **Revamped through this beta** (still marked **(WIP Beta)** in the panel). The painter used to show a plain text list of profiles. It now shows your filaments as **generated colour palettes**, lets you **compose and edit a recipe live** (Pro mode), and **pick** a colour straight off the model.

The painter lives in the **left-side gizmo toolbar** in the 3D view, next to the FuzzySkin, Seam, and MMU segmentation gizmos.

**Tools (top row of the panel)**

| Tool | What it does |
|------|--------------|
| **Select** (Smart Fill) | Click a flat face → flood-fills the coplanar top region with the **active colour**. |
| **Eraser** | Click a painted region → clears it (Smart Fill). |
| **Pick** (eyedropper) | Click a painted face → reads the **actual recipe** under the cursor, loads it as the active colour and links its profile. The quickest way to keep painting with a colour that's already on the model. |

**Mouse rules** — left-click paints / erases / picks with the active tool; **right-click is camera only** (orbit & pan — it never paints or erases); **Shift + left-click** is a one-shot erase regardless of tool. Left-clicking with **no colour selected does nothing** (it won't wipe paint by accident). These replace the old behaviour where moving the camera or clicking an empty slot could destroy paint.

**The panel, top to bottom**

1. **Palette strips** — **collapsible** sections, one per style: **Gradient ramp** and **Flat colour**. Each expands into a horizontally-scrollable strip of swatches generated from your loaded filaments + their TD (the same engine as the ColorStitch Studio, §1g). Hover a swatch to preview its recipe; the strips regenerate when you change filament colours or TD. *(The old "Mixed approximation" strip was removed from the painter in this beta.)*
2. **Pro mode** — a collapsible composer, and **the Pro panel IS the active colour**. Whatever you build here — **Top / Penultimate** rows, each one **Solid / ColorStitch / PathBlend (Half|Full)**, with a per-pass **Z height** box — is exactly what you paint with. A single **Perimeter override** checkbox applies to both zones (the recipe stack is the source of truth, no separate per-zone keys).
3. **Pin to palette + Name** — type a name and **Pin to palette** to promote the active recipe into the **Profiles** list (this replaces the old "Use as paint colour" / "Save palette" buttons). If the active colour is **linked to a saved profile**, editing it in Pro mode **rewrites that profile in place** — every object using it updates live.
4. **Profiles** — the list of **saved** palettes (two-tier model below). Click a saved row to load it into Pro and paint with it; it stays **live-editable**.
5. **Smart-Fill angle** + **Clipping plane** sliders, and **Erase all**.

**Smart-Fill only**: the painter targets coplanar top surfaces, so Select/Eraser use **Smart Fill** (the old Circle / Sphere / Triangle brushes were removed). **Smart-Fill angle**: lower = more selective (only very-coplanar triangles), higher = more inclusive. Default **1.5°** suits flat staircase steps.

**How to paint**

1. **Pick a swatch** (Gradient/Flat) or **compose one in Pro mode** → it becomes the **active colour**. Browsing does not create anything yet.
2. **Click the model surface** to paint. The first paint with a colour materialises a slot for it.
3. Use **Pick** to grab a colour already on the model and keep going, or select another swatch.

#### Two-tier palette model (why the list no longer fills up)

- **Working colours (automatic)** — the colours you actually paint. Created on demand the first time you paint with a recipe, **deduplicated** (re-using a colour reuses its slot), and **garbage-collected** when no painted face uses them anymore (after *Erase all* or closing the gizmo). They do **not** appear in the saved list, so it stays clean. A working colour that occupies a slot is shown with an **amber border**; **right-click → Delete** frees that slot.
- **Saved palettes (deliberate)** — **Pin to palette** promotes the active colour into the **Profiles** list. Saved palettes are named, persistent, survive garbage-collection, and travel in the 3MF.

Up to **15 working colours** can be painted on a single object at once (slot 0 is "unpainted"). Because browsing no longer consumes slots, you can explore the palettes freely.

**Erase**
- **Eraser tool** / **Shift + left-click** — clear the painted region under the cursor with Smart Fill.
- **Erase all** — clears every painted face on the current volume, then garbage-collects the now-unused working colours.

Cleared faces return to "unpainted" — they use preset mode if the object becomes entirely unpainted, or no effect if any paint remains.

**Penultimate painting**: if the recipe you paint declares activity on the **penultimate** zone, the slicer auto-forces 2 penu layers for that object (Penu role autonomy, §7e) so the penu effect actually has a surface to apply on.

---

### 7d. Painter mode at slice time

When an object has **any painted facets**, the slicer auto-switches that object to **painter mode**. The rules are:

| Situation | What applies |
|-----------|--------------|
| Object has zero painted facets | **Preset mode** — Sandwich dialog values apply normally |
| Object has painted facets — painted area | **Painted profile** applies. Preset is fully ignored for that area. |
| Object has painted facets — **unpainted** area | **No effect** applies to that area (preset is suppressed for the whole object). |

This "all or nothing" rule (called *Q1 = absolute*) prevents the unintuitive situation where painted areas mix preset and profile effects. If you want preset behaviour back on an object, simply erase all painted facets from it.

**Per-surface resolution**: For each top/penultimate fill at each layer, the slicer looks up the dominant painted slot in the layer's Z range and uses that profile's payload. If multiple profiles are painted at the same Z (e.g. left half PB34, right half PB12), each region gets its own effect.

**Tool registration**: The wipe-tower planner uses the same painted-profile lookup as the slice itself, so the wipe tower plan and the actual G-code stay in sync — no "unexpected toolchange" errors from divergence.

---

### 7e. Penu role autonomy

The **Penultimate top layers** setting (§3) is normally `0` in the default preset, meaning penultimate surfaces are not classified separately from regular solid infill.

When you paint a profile that has **MultiPass penu enabled** or **ColorMix surface = Both/Penu** on an object, the slicer auto-detects this and **forces** the penultimate classification to 2 layers for that object, regardless of the preset value. This way, the painter's penu profile actually has surfaces to apply on.

You'll see a `PENU_AUTONOMY object='…'` line in the debug log when this kicks in. No user action needed.

---

### 7f. Profile persistence and 3MF round-trip

Everything is saved inside the 3MF project file:

- **The profile manager** (the list of named profiles and their effect payloads) is stored as a project-level metadata entry (`colormix_profiles_b64` — base64-wrapped JSON to survive XML escaping).
- **Per-volume slot tables** (which slot index maps to which profile id) are stored as a per-volume metadata key.
- **Per-triangle paint** (which slot is painted on each facet) is stored as a per-triangle attribute (`paint_colormix`), mirroring how MMU painting works.

When you open a 3MF, all three pieces are restored. The painter list, the painted facets, and the slot→profile mapping all come back exactly as you saved them.

**Sharing**: Profiles only live inside the 3MF they were saved with. There is no global profile library across projects. To reuse a set of profiles, save them in a template 3MF and open it as a starting point.

---

### 7g. Known limitation — PathBlend on Penu

PathBlend's penultimate surface support has a known gradient-direction bug on the second-stair penu surface of multi-stair objects. The slice-time clone path inverts the pass order for the second stair, so pass 0 paints on top of pass 1 instead of underneath, breaking the gradient.

For this reason, **the PathBlend pill is hidden on the Penultimate zone card** in the Sandwich dialog. PathBlend is only selectable on the Top zone. Profiles saved before this restriction was added are loaded with their penu PathBlend intent silently demoted: if penu also has MP or CM enabled, those apply; otherwise penu is set to None.

The underlying engine supports penu PB (the struct, payload, and 3mf I/O are all in place). When the gradient-direction bug is resolved, the pill can be re-enabled.

---

## 8. NeoArachne — alternative wall generator

**What it is**

NeoArachne is a **new wall-generation engine** that lives next to OrcaSlicer's two stock engines (Classic constant-width, Arachne variable-width). It is not a replacement — it is a **mix-and-match layer** on top of them that lets you choose, *per feature* (outer wall, inner walls, gap fill), which underlying engine to use, plus a set of extra knobs that target the failure modes Arachne is known for: width breathing along contours, blobs at thin transitions, and bead-count oscillation on borderline-thickness strokes.

The default NeoArachne configuration — internally called **Neotko Hybrid v2** — uses Classic for the outer wall (clean, predictable surface) and stock Arachne for the inner walls (variable-width with integrated gap fill, which is what makes letters and thin features look good). You can override either choice.

NeoArachne also ships a **Preview Lab** (§8e) that renders the planned wall paths layer by layer **before slicing**, so you can see how the engine will lay down each perimeter, where it places seams, and how it travels — without having to slice and inspect the gcode every time.

**When to consider it**

- You print parts with **thin features** (letters, logos, embossed text) where stock Arachne leaves blobs or where Classic skips gap fill.
- You see **width breathing** on Arachne outers (the outer wall visibly thickens and thins along a curve).
- You want to **control the maximum bead width** explicitly instead of trusting Arachne's auto-derivation.
- You want to **preview the wall plan** without slicing.

NeoArachne is **opt-in**. Existing presets and projects are not affected by this feature.

---

### 8a. Turning it on

1. Open **Quality → Wall generator** (Advanced visibility).
2. The dropdown now has three options: **Classic**, **Arachne**, and **NeoArachne**.
3. Select **NeoArachne**. A new **NeoArachne** section appears further down the Quality tab with all the controls described below.
4. Leave the rest of the controls at their defaults for a first slice — they encode the **Neotko Hybrid v2** recipe and produce solid results on most parts.

---

### 8b. Per-feature engine choice

NeoArachne is not a single engine — it routes each kind of wall through whichever underlying engine you pick. The three dropdowns:

| Setting | What it controls | Options | Default |
|---------|------------------|---------|---------|
| **NA — outer wall source** | Engine for the outer perimeter (the visible surface) | Classic / Arachne (stock) / Arachne (NeotkoEdge) | **Classic** |
| **NA — inner walls source** | Engine for every interior perimeter | Classic / Arachne (stock) / Arachne (NeotkoEdge) | **Arachne (stock)** |
| **NA — gap-fill source** | Dedicated gap-fill pass (only useful when inner=Classic) | Off / Classic / Arachne (stock) / Arachne (NeotkoEdge) | **Off** |

The three engine variants behave like this:

- **Classic** — constant width across the whole pass. Cleanest visible surface, no width breathing, but cannot adapt to thin features without a dedicated gap-fill pass.
- **Arachne (stock)** — variable-width beading with integrated gap fill. Best for interior walls where adaptation matters.
- **Arachne (NeotkoEdge)** — Arachne with the NeotkoEdge bead-count stabiliser layered on top (§8d). Reduces the "breathing" artifact on borderline-thickness strokes at the cost of slightly delayed transitions on very thin features.

**Recipe shortcut: Neotko Hybrid v2** (the default): outer = Classic, inner = Arachne (stock), gap fill = Off. That is what you get the first time you switch wall_generator to NeoArachne.

---

### 8c. Edge Closure controls

These five controls shape how Arachne emits beads when it has to deal with thin features and seam-edge geometry. All values are percentages of nozzle diameter unless noted.

| Control | Range / default | What it does |
|---------|-----------------|--------------|
| **NA — allowed perimeter overlap** | 0–100%, default **0%** | How much the first Arachne inner bead is allowed to overlap the Classic outer perimeter. The structural overlap from line-width math (~11%) already closes the seam at 0%. Raise to 5–15% only if you see a visible seam on letters or curves. Above ~70% causes heavy blobs. Only applies when outer=Classic + inner=Arachne. |
| **Min Line Width** | 5–100% of nozzle, default **40%** | Minimum bead width Arachne can emit. Higher values force thin features to be widened up to this minimum (which causes blobs by accumulation). Recommended range 30–50%. |
| **Max Line Width** | 100–200% of nozzle, default **200%** | Ceiling on bead width. Arachne grows beads naturally up to this cap, then splits into two narrower ones. Default 200% behaves as "auto" — almost never reached in practice. Lower it (e.g. 130–150%) when you want more, narrower beads — useful with fine nominal line widths like 0.24 mm. |
| **Min Feature Threshold** | 1–100% of nozzle, default **10%** | Geometry thinner than this is discarded entirely. Anything between this floor and the Min Line Width gets widened, so keep this low (0–20%) for clean thin-feature handling. Must stay ≤ Min Line Width. |
| **Preserve Thin Edges** | on/off, default **on** | Keeps short closure tails that approach the outer perimeter (cleaner seams). Disable only if you see speckled artifacts from very short segments. |

**How to think about Min vs Max Line Width**

- **Min Line Width** is a *floor*. Higher = thin features get widened = more material per pass = blob risk.
- **Max Line Width** is a *ceiling*. Lower = Arachne splits sooner = more, narrower beads = more transitions.

Most prints don't need to touch either. Reach for them when you see a specific symptom (blobs on thin areas → lower Min; visible "breathing" or fat outers → lower Max).

---

### 8d. NeotkoEdge — stable bead transitions

The "Arachne (NeotkoEdge)" engine variant adds two extra parameters on top of stock Arachne, controlling **how stable the bead count stays along a stroke**.

| Control | Range / default | What it does |
|---------|-----------------|--------------|
| **Wall Count Stability** | 0–100% of outer wall width, default **20%** | Spatial hysteresis applied to bead-count transitions. The N → N+1 bead transition is delayed by this much, so borderline-thickness strokes stop oscillating along their length (the classic Arachne "breathing" artifact). Higher = calmer, but very thin features may stay mono-wall longer. 0 disables. Typical 10–25%. |
| **Wall Blend Distance** | 1–500 mm, default **100 mm** | How smoothly Arachne transitions bead count along the medial axis. Lower (20–50 mm) = sharper, more localised transitions; higher = transitions spread over a longer region. Most use cases work well at 50–100 mm. Reduce when borderline strokes need crisper edges; increase when transitions look visually jarring. |

These controls only have effect when at least one wall (outer or inner) is set to **Arachne (NeotkoEdge)**.

---

### 8e. Preview Lab

The **Preview Lab** is a panel inside the NeoArachne section of the Quality tab that renders the planned wall paths **before you slice**. It is meant for debugging and iteration — see what the engine will lay down without having to slice and open the gcode viewer.

**What it shows**

- The **outer wall** path and every **inner wall** path on the selected layer, in distinct colors.
- The **execution order** of paths (chain order — which one prints first, second, etc.).
- **Seam positions** marked as small dots.
- **Travel moves** drawn as dotted lines, so you can see where the printer would jump dry.
- A **head animation** (small marker) that traces the planned path in real time so you can watch where blobs or sharp travels are likely.

**Controls inside the panel**

| Control | What it does |
|---------|-------------|
| **Layer slider** | Scrub through layers of the currently selected object. |
| **Speed slider** | Adjust the head animation speed (0.02× to 0.5×). |
| **Build mode** | Switches between "ghost" (everything visible) and "printed" (only what has been laid down up to the animation head). Useful to spot occlusions and detect missing paths. |
| **Zoom** | Cmd + scroll on macOS / Alt + scroll on other platforms. |
| **Use selection** | Limit the preview to whichever object you have selected in the 3D viewport. |
| **Dump** | Exports the full plan (geometry input, resolved bead toolpaths, NeoArachne config snapshot) as JSON to a file. Use this when reporting a NeoArachne issue — the dump is what makes off-line reproduction possible. |

**When the preview is most useful**

- You want to check that the **outer wall is one clean continuous loop** (no unexpected breaks).
- You want to see the **chain order** the engine chose (which wall prints first, where the seam is).
- You suspect a thin feature is being **discarded** or **over-widened**; the preview shows the actual beads.
- You're tuning **Max Line Width** and want to compare 130% vs 150% vs 200% without re-slicing.

> **Known limitation:** on some geometries there is a small visual divergence between the Preview Lab and the real slice on the second-to-last interior wall in narrow "waist" regions. If you spot this, the **Dump** button is your friend — share the JSON dump (and ideally the 3MF) so it can be diagnosed.

---

## 9. NeoTower — post-slice wipe tower

**What it is**

NeoTower is an alternative **wipe-tower planner**. The stock planner (WipeTower2) decides the tower geometry up front; NeoTower runs **after slicing**, when every toolchange is already known — including the extra sub-layer primes that Sandwiches and MultiPass insert *inside* a layer. Because it sees the complete toolchange list before committing to any geometry, it can build a **fixed, predictable footprint** that stays in sync with the real G-code, and it understands variable (adaptive) layer heights.

This is what makes the hard combination — **adaptive layer height + multiple tools + a Sandwich**, all at once — actually print on a single tower, which is something other slicers don't do.

**You usually don't have to choose it.** Any scene that uses a Sandwich or MultiPass (single-filament or multi-tool) **auto-promotes to NeoTower** regardless of the setting below. The Tower-type selector matters mainly for plain multi-tool scenes and to expose NeoTower's extra options.

The options live in **Quality → Prime tower**.

---

### 9a. Tower type

| Setting | Behaviour |
|---------|-----------|
| **Classic** (default) | The standard WipeTower2 planner. |
| **NeoTower** | The post-slice planner described above — fixed footprint, delta-Z aware, with the zigurat-taper and purge-compaction options below. |

Sandwich / MultiPass scenes use NeoTower automatically even when this is set to Classic.

---

### 9b. Zigurat taper

**Default: on.** Limits how fast the NeoTower footprint may shrink between consecutive real layers (one perimeter width per side) so that every wall ring rests on the ring below — *wall-on-wall*, which prints cleanly. Disable to **save material and time**, at the cost of rings that are only partially supported by the sparse grid inside the tower.

---

### 9c. Sandwich purge compaction

Sandwich and MultiPass insert thin sub-layer purges into the tower. **Purge compaction** is a flow-boost cap that lets those thin purges be compacted into a **narrower band** by extruding more material per millimetre (the excess hangs into the hollow tower interior), which **reduces the tower footprint**.

- **1.0** = no compaction.
- Values above 1 compact more aggressively.
- **Beta default: 1.7.** Range 1.0–5.0.

Compaction depends on what your material tolerates — confirm with a test print before pushing it high.

---

### 9d. SurfaceColorStitch wipe reserve

The volume (mm³) purged on the wipe tower before each **ColorStitch / MultiPass / PathBlend** sub-layer toolchange (Top + Penultimate). It replaces the old separate top/penultimate prime-volume keys with a **single** reserve.

- **Beta default: 10 mm³** (raised from 5).
- Lower = thinner / shorter tower; higher = better purge.
- Set to **0** to disable. Requires a wipe tower (NeoTower or prime tower) to be active.

> This is the global counterpart of the per-pass **Prime volume** described in the MultiPass dialog (§1b).

---

### 9e. Adaptive layers × multi-tool × Sandwich

Because NeoTower plans from the real, post-slice toolchange list and is delta-Z aware, you can enable **adaptive/variable layer height** on a **multi-tool** print that also uses a **Sandwich** — three things that normally fight each other on the tower — and still get a coherent wipe tower. The planner mirrors the exact emission plan (plan = emission = tower) so there are no "unexpected toolchange" divergences.

This is a recent capability and still being hardened; if you hit a tower artifact with this exact combination, it's worth reporting with the project 3MF.

---

## Quick Reference — Where to find things

| Feature | Location in UI |
|---------|---------------|
| Sandwich dialog (ColorMix / MultiPass / PathBlend) | Quality → ColorMix & Multi-Pass Blend → **Edit…** |
| Line distribution mode (CM + PB) | Quality → **Line distribution mode** (Advanced) |
| ColorMix pattern mode (Linear 2/3, Custom bands, Pattern string) | SCM dialog → **Advanced…** on ColorMix pill |
| Easing / gamma / overlap / invert | ColorMix Advanced dialog (Linear modes only) |
| Effect pill (None/ColorMix/MultiPass/PathBlend) | Sandwich dialog — pill buttons per zone card |
| Advanced config (per effect) | Sandwich dialog → **Advanced…** button |
| TD Preview | Sandwich dialog → **TD Preview** (collapsable) |
| Colour match (inverse ΔE2000) | Sandwich dialog → **ColorStitch Studio → Target + Match ▸** (replaces old Blend Suggestion) |
| ColorStitch Studio (palette generators) | Sandwich dialog → **ColorStitch Studio** panel (Gradient / Flat / Mixed predict) |
| Neoweaving | Quality → Neotko Neoweaving |
| Penultimate layers | Strength → Top/bottom shells → Penultimate top layers |
| Libre Mode toggle | **Top toolbar** (main window button) |
| Temporal Link (group) | Select objects → **Ctrl+G** |
| Temporal Link (select group) | Right-click object → **Select Grouped** or **Ctrl+Shift+G** |
| S3DFactory import | File → Import → Import 3D model → select `.factory` |
| Save as profile | Bottom of SCM / MultiPass / PathBlend dialogs — **Save as profile…** button |
| Manage profiles (load / update / rename / delete) | Sandwich dialog → **Manage profiles…** button |
| 3D Painter | Left-side gizmo toolbar → **ColorStitch Painter** icon |
| Painter — palette strips | Gizmo right panel → collapsible **Gradient / Flat** sections (click swatch = active colour) |
| Painter — compose / edit a recipe | Gizmo right panel → **Pro mode** (Top/Penu rows, live = active colour) |
| Painter — pick colour off the model | Gizmo top row → **Pick** (eyedropper) tool |
| Painter — save a palette | Gizmo right panel → **Pin to palette** + Name (while a colour is active) |
| Painter — pick saved profile | **Profiles** list inside the gizmo's right panel |
| NeoArachne (enable) | Quality → **Wall generator** → select **NeoArachne** |
| NeoArachne per-feature engines | Quality → NeoArachne → **NA — outer wall / inner walls / gap-fill source** |
| Min / Max Line Width | Quality → NeoArachne → **Min Line Width** / **Max Line Width** |
| Edge Closure (overlap, feature, thin edges) | Quality → NeoArachne → allowed overlap / Min Feature Threshold / Preserve Thin Edges |
| NeotkoEdge stability (hysteresis + blend distance) | Quality → NeoArachne → **Wall Count Stability** / **Wall Blend Distance** |
| NeoArachne Preview Lab | Quality → NeoArachne (panel embedded at the bottom of the section) |
| NeoTower (tower type) | Quality → Prime tower → **Tower type** (Classic / NeoTower) |
| Zigurat taper / Sandwich purge compaction | Quality → Prime tower |
| SurfaceColorStitch wipe reserve | Quality → Prime tower → **SurfaceColorStitch wipe reserve** (mm³) |

---

## Frequently Asked Questions

**Q: ColorMix doesn't seem to apply on some layers — why?**

First, confirm your top surface fill pattern is set to **MonotonicLine** — this is the only pattern that works correctly on complex objects (see §1a). Other patterns (Rectilinear, Monotonic) may appear to work on simple square/circular shapes but will produce incorrect toolchange sequencing on anything more complex.

If the pattern is correct, the next most common cause is the **Min. line length** setting. Fill lines shorter than this threshold are skipped. Try lowering it (default is 1.0 mm). Also check the **Zone** setting — if set to "Topmost only", ColorMix will only apply to the very top layer of the whole object.

**Q: I set PathBlend but the gradient looks like just one or two steps, not smooth.**

The smoothness of the gradient depends on how many fill lines the surface has. A small surface with 5 lines can only have 5 gradient steps. Increasing infill density, widening the surface, or using a smaller line width gives the slicer more lines to work with. PathBlend cannot subdivide individual lines (that feature is planned for a future version).

**Q: My top surface has holes or concave shapes and the ColorMix stripes look fragmented or wrong across the gaps.**

Change the **Line distribution mode** (Quality, Advanced) to **LaneQuant (2)** or **DirCluster (3)**. Default mode uses raw print order, which scrambles when the slicer routes lines around an island. LaneQuant quantises lines by spatial position so stripes broken into multiple physical fragments stay the same colour. DirCluster handles the extra case where the slicer rotated fill direction inside sub-regions. See §1f.

**Q: PathBlend gradient looks coarse or "stepped" on an irregular surface.**

Default line distribution falls back to a bounding-box centroid for the gradient position, which can be coarse on non-rectangular shapes. Switch the **Line distribution mode** to **LaneQuant (2)**. Each line is then assigned its own spatial position in the gradient and the transitions become noticeably smoother. See §1f.

**Q: What's the difference between ColorMix's Pattern modes 1, 2, 3?**

- **Pattern string (0)** is the legacy mode where you write `1212` or similar and the slicer loops it.
- **Linear 2-color (1)** is the most common — pick two tools and a percentage split (`pct_a`). Dither makes the transition smooth.
- **Linear 3-color (2)** does the same with three tools and two percentages. The middle tool concentrates in the centre of the surface.
- **Custom bands (3)** lets you say "10 lines of T0, then 5 of T1, then 3 of T2, repeat" — explicit hard-band counts, no dithering.

For a smooth two-colour gradient choose mode 1 with an easing other than Linear. For exact engineering-style stripes choose mode 0 or 3.

**Q: The PathBlend gradient is too abrupt at the edges, I want it to accelerate gradually.**

Use the **Ease mode** option in the PathBlend Advanced dialog. **Ease In-Out** gives the smoothest perceptual result — the gradient starts slow, accelerates in the middle, and slows again at the far edge. **Ease In** or **Ease Out** apply the curve to one side only.

**Q: I want both filaments always present across the full gradient, never fading out completely.**

Use the **Min ratio** slider to set a minimum flow floor for the receding filament. A value of 0.10 (10%) means the receding filament never drops below 10% flow even at the extreme edge. **Max ratio** does the same for the dominant filament — reducing it below 1.0 keeps the receding filament present everywhere in the gradient.

**Q: Neoweaving is making my printer move Z very rapidly — is that normal?**

Yes. Neoweaving requires rapid Z moves between successive lines on the same layer. If your printer's Z axis is slow or has significant inertia, reduce the **Amplitude** or enable the **Speed %** override for neoweaved lines to give it more time. Start with an amplitude of 0.05 mm and work up.

**Q: MultiPass is over-extruding my top surface.**

Check that the sum of all pass **width ratios** adds up to approximately 1.0. If the Σ indicator in the dialog shows a value well above 1.0, reduce the ratios proportionally. The ratios represent what fraction of the surface each pass covers — Σ = 1.0 means full coverage with no overlap.

**Q: Can I use ColorMix and MultiPass at the same time?**

Not on the same zone — they are mutually exclusive per zone (only one pill can be active at a time). However, you can apply ColorMix to the Top zone and MultiPass to the Penultimate zone, or vice versa, since the two zones are configured independently.

**Q: Do I need Libre Mode ON to print normally?**

No. Libre Mode is OFF by default and everything works normally without it. Libre Mode is only needed for the specific advanced workflows described in §5: floating objects, world-space import, temporal linking, per-volume XY compensation, and the detachable panel.

**Q: My `.factory` file imported but all objects are in the wrong position.**

Try enabling Libre Mode before importing. With Libre Mode OFF, the slicer re-centers objects to the build plate, which loses the relative positioning from the Simplify3D project.

**Q: The TD Preview swatches don't match what I see on the printed part.**

TD values are per-machine (per filament spool), not per print profile. Make sure you have calibrated the TD sliders to match your actual filaments. Start by printing a single-color top surface, then a two-color blend, and adjust the TD values until the Result swatch matches the printed result.

**Q: I painted a profile on a stair but at slice time it doesn't apply.**

Check the slicing notification panel for a non-critical warning like *"Object 'X' has painted regions referencing deleted Surface Effect Profile(s)…"*. That means the profile you painted with was deleted from the manager (e.g. you opened a 3MF where the profile no longer exists, or hit Delete in Manage Profiles). Re-create the profile with the same name or re-paint with an existing one.

If there's no warning, confirm the object actually has painted facets (use the painter gizmo — painted triangles are colour-tinted). An object with zero paint falls back to preset mode.

**Q: I painted MultiPass with penu config but only the top surface gets the effect.**

The penu surface needs to exist for the painter to apply its penu profile. By default, **Penultimate top layers** is 0 (no penu surfaces). When you paint a profile that declares penu activity, the slicer auto-forces 2 penu layers for that object (Penu role autonomy, §7e). If you still see top-only, check the `ORCA_DEBUG_PROFILE=1` log for a `PENU_AUTONOMY` line — if absent, the profile's payload may not be marking penu enabled. Re-open the SCM dialog, ensure the penu MP pill is ON, and re-save.

**Q: Why is the PathBlend pill hidden on the Penultimate zone card?**

There is a known gradient-direction bug specific to penu PathBlend on multi-stair objects (the second stair inverts pass order). The pill is hidden until that bug is resolved. The Top zone PathBlend works correctly and is selectable normally. See §7g.

**Q: Can a single object have ColorMix on one part, MultiPass on another, and PathBlend on a third?**

Yes — that is exactly what the 3D Painter is for. Save each effect as its own profile, then paint each face/area of the model with the appropriate profile. At slice time, each painted area renders with its own profile. Unpainted parts of the object get no effect. See §7.

**Q: I opened a 3MF made on another machine and the profiles came back, but my live preset is unchanged. Is that correct?**

Yes. Profiles are project-scoped — they live inside the 3MF only, separate from your preset library. Loading a project restores its profiles into the manager for that session, but does not modify your default preset. Save the project as a template if you want the profiles available as a starting point for new prints.

**Q: What does the ΔE indicator on the ColorStitch Studio's Match mean?**

ΔE is a perceptual color difference measure (the Studio uses ΔE2000). A value below ~5 is generally indistinguishable to the eye. Values above 10–15 indicate the closest achievable recipe will still look visibly different from the target colour — usually because the available filaments cannot reproduce the target, or because the TD values need calibration. *(The old "Blend Suggestion" panel that showed this in the Sandwich dialog was retired in 2.1; the inverse match now lives in the ColorStitch Studio — §1g.)*

**Q: I had a Sandwich set up with only Solid (MultiPass) passes and the wipe tower never appeared. Is that the bug fixed in 1.9?**

Yes. Before 1.9 the wipe-tower gate only looked at a small set of legacy enable flags. A Sandwich with only Solid passes (the classic "T0 → T2 → T0" MultiPass recipe) saved its state in a different place (the Sandwich's own per-zone configuration), so the gate missed it and skipped the tower even when "enable prime tower" was on. Re-slice your project on 1.9 — the tower should now show up wherever a Sandwich is active.

**Q: I want to try NeoArachne but don't want to break my current preset. What's the safest way?**

NeoArachne is fully opt-in. Switch **Quality → Wall generator** to **NeoArachne**, leave all the new controls in the NeoArachne section at their defaults, and slice. The defaults encode the "Neotko Hybrid v2" recipe (Classic outer + stock Arachne inner) which produces solid results on most parts. If you don't like what you see, switch the dropdown back to Arachne or Classic — the NeoArachne controls don't touch any existing setting.

**Q: NeoArachne is leaving blobs in thin features (letters, embossed text).**

Two likely causes:
1. **Min Line Width too high**. The default 40% is calibrated; values above 50% widen thin features and accumulate material. Lower it (try 30–40%) and re-slice.
2. **Min Feature Threshold too high**. Geometry between this floor and Min Line Width gets widened. Keep this at 5–15% so thin features either survive as native thin beads or are cleanly discarded, not widened.

If the blobs persist, open the **Preview Lab** on the affected layer to see exactly what the engine is laying down, and use the **Dump** button to share the plan for diagnosis.

**Q: NeoArachne outer walls look "fat" or visibly breathe in width along curves.**

Two things to try:
1. Lower **Max Line Width** (e.g. from 200% to 130–150%). This forces Arachne to split into more, narrower beads sooner, which limits how wide a single bead can grow on a curve.
2. Switch **NA — inner walls source** to **Arachne (NeotkoEdge)** and raise **Wall Count Stability** to 15–25%. The hysteresis dampens bead-count oscillation along borderline-thickness strokes, which is the usual cause of "breathing".

The outer wall is Classic by default, so its width should already be constant. If the outer itself looks variable, check that **NA — outer wall source** is actually set to **Classic** (the default).

**Q: The NeoArachne Preview Lab shows my walls perfectly but the printed part looks different on one specific layer.**

There is a known visual divergence on the second-to-last interior wall in narrow "waist" regions of some geometries. The Preview Lab and the real slice can disagree there. If you spot this, use the **Dump** button in the Preview Lab to export a JSON dump and share it (with the 3MF) — that's exactly what makes diagnosing the divergence possible.

**Q: "ColorStitch", "Surface Color Mixer", "ColorMix" — are these the same thing?**

Mostly yes. **ColorStitch** is the new UI name for the Surface Color Mixer family (the Studio in the Sandwich dialog and the ColorStitch Painter gizmo). Internal names, config keys and 3MF data were not renamed, so old projects keep working — and you'll still see *ColorMix* as the name of the per-line effect pill. They refer to the same system.

**Q: In the painter, why don't my picked colours show up in the profile list anymore?**

By design (this beta). Clicking a palette swatch sets it as the **active colour** but doesn't save anything — colours you paint with are "working colours" that are created on demand, reused if identical, and cleaned up when no face uses them. The **Profiles** list only shows palettes you deliberately **Pin to palette** (with a name). This keeps the list from filling up with every shade you try. To grab a colour already on the model, use the **Pick** (eyedropper) tool. See §7c.

**Q: I can't find "Micro Stitch (Neotko)" in the top/bottom surface pattern dropdown.**

It's **hidden in this beta** — it was an experimental pattern and is parked while other work stabilises. Presets that already use it still load and slice; the option just isn't offered in the dropdown. It can be re-enabled in a later build.

**Q: My wipe tower changed size / uses more purge than before. Why?**

Two beta default changes: **SurfaceColorStitch wipe reserve** is now **10 mm³** (was 5) and **Sandwich purge compaction** is **1.7** (was 1.0). Both live in **Quality → Prime tower** (§9). Lower the reserve for a thinner tower, or set compaction back to 1.0 to disable footprint compaction. Note: existing saved profiles keep whatever value they had — the new defaults only apply to fresh profiles.

**Q: Can I use adaptive (variable) layer height with multiple tools and a Sandwich at the same time?**

Yes — that's what **NeoTower** (§9) enables, and it's a recent capability of this fork. Sandwich/MultiPass scenes promote to NeoTower automatically. It's still being hardened, so if you see a tower artifact with this exact combination, report it with the project 3MF.

---

*OrcaSlicer FullSpectrum — Neotko Feature Pack*
*Designed by Neotko · Implementation by Claude (Anthropic)*
