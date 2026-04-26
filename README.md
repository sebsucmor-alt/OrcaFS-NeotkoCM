# OrcaSlicer FullSpectrum — Neotko Feature Pack · User Guide

> Features conceived and designed by **[Neotko](https://github.com/neotko)** — inventor of *Neosanding*, now known as **Ironing** in OrcaSlicer, PrusaSlicer, Bambu Studio and Cura.

This fork adds a set of surface quality, color blending and workflow features on top of OrcaSlicer FullSpectrum (Snapmaker base). This guide explains what each feature does and how to use it — no programming knowledge required.

<a href="https://www.buymeacoffee.com/Neotko">
  <img src="https://cdn.buymeacoffee.com/buttons/v2/default-red.png" alt="Buy Me a Coffee" height="60" width="217">
</a>

---

## Table of Contents

1. [Surface Color Mixer](#1-surface-color-mixer)
   - 1a. [ColorMix — per-line color patterns](#1a-colormix--per-line-color-patterns)
   - 1b. [MultiPass Blend — multiple passes per layer](#1b-multipass-blend--multiple-passes-per-layer)
   - 1c. [PathBlend — smooth gradient across the surface](#1c-pathblend--smooth-gradient-across-the-surface)
   - 1d. [Zone and filament filters](#1d-zone-and-filament-filters)
   - 1e. [TD Preview and Blend Suggestion](#1e-td-preview-and-blend-suggestion)
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

---

## 1. Surface Color Mixer

The Surface Color Mixer is the umbrella feature for everything that happens on **top and penultimate surfaces** when you have more than one filament loaded. It lives under **Quality → ColorMix & Multi-Pass Blend** in the process settings, and has a single "Edit…" button that opens the dialog.

### Dialog layout

The dialog has two independent **zone cards** — one for the **Top** surface and one for the **Penultimate** surface. Each zone is configured entirely on its own.

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

ColorMix alternates which filament prints each fill line on a top or penultimate surface. You define a *pattern* — for example `1212` — and the slicer loops that pattern across all lines in the surface. Line 1 → T1, line 2 → T2, line 3 → T1, line 4 → T2, and so on. Each real toolchange goes through the wipe tower exactly as normal.

The result is a striped or woven color effect on the top surface, without any manual toolchange programming.

**How to set it up**

1. Open **Quality → ColorMix & Multi-Pass Blend → Edit…**
2. In the dialog, click the **ColorMix** pill for the Top surface, the Penultimate surface, or both.
3. Click **Advanced…** to open the pattern editor. Click the colored filament buttons to build your pattern visually. The pattern is just a string of tool numbers (`1`, `2`, `3`…) that repeats.
4. Set the **Zone** (see §1d) and **Min. line length** if needed.
5. Click OK and slice normally.

**⚠️ Required fill pattern: Monotonic Line**

ColorMix **only works correctly with the MonotonicLine fill pattern**. This is not a preference — it is a technical requirement for complex objects.

- **MonotonicLine** ✅ — the slicer pre-splits the surface into individual straight paths before ColorMix runs. Every path in every region of the surface is visited and assigned a tool, including irregular, concave, or asymmetric shapes.
- **Monotonic** ⚠️ — works acceptably on simple convex surfaces (a square, a circle). On complex objects with holes, concavities, or multiple disconnected regions, the path ordering is finalized *after* ColorMix assigns tools, so toolchanges end up in the wrong sequence visually.
- **Rectilinear** ⚠️ — similar problem. Works on simple shapes but fails visually on anything more complex than a basic rectangle.

**Set your top surface fill pattern to MonotonicLine before enabling ColorMix.** The setting is in Quality → Top surface pattern.

**Min. line length**: Very short fill lines (from curved or complex surfaces) are skipped to avoid pointless toolchanges. Default is **1.0 mm**. You can raise it if you want fewer toolchanges on small features, or lower it toward 0 to apply the pattern everywhere.

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

---

### 1d. Zone and filament filters

These controls appear in the **Filament** section at the bottom of the Surface Color Mixer dialog. They apply to whichever effect is active on each surface.

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

### 1e. TD Preview and Blend Suggestion

The **TD Preview** section (collapsable, at the bottom of the Surface Color Mixer dialog) lets you visualize and calculate how your filaments will visually combine when one layer sits on top of another.

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

#### Blend Suggestion — Calculate

The **Calculate** button computes a suggested MultiPass configuration to reproduce a target mixed color as closely as possible.

**How it works:**
1. Select a **MixedColor** target from the dropdown (these are the virtual blended colors you have created in the mixer).
2. The target color's recipe (which filaments and in what proportions) is read.
3. Using the Beer-Lambert inverse formula, the slicer computes what **width ratios** each pass needs so that the combined optical result (at the current TD values) matches the target color.
4. The suggested passes are split between Top and Penultimate zones if both are active, or assigned to Top only.
5. A **ΔE** swatch shows how close the calculated result is to the target: green = very close, orange = acceptable, red = significant difference.

Click **Apply** to write the suggested ratios and tool assignments into the current MultiPass configuration. You can then fine-tune from there.

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

## Quick Reference — Where to find things

| Feature | Location in UI |
|---------|---------------|
| ColorMix / MultiPass / PathBlend | Quality → ColorMix & Multi-Pass Blend → **Edit…** |
| Effect pill (None/ColorMix/MultiPass/PathBlend) | Surface Color Mixer dialog — pill buttons per zone card |
| Advanced config (per effect) | Surface Color Mixer dialog → **Advanced…** button |
| TD Preview + Blend Suggestion | Surface Color Mixer dialog → **TD Preview** (collapsable) |
| Neoweaving | Quality → Neotko Neoweaving |
| Penultimate layers | Strength → Top/bottom shells → Penultimate top layers |
| Libre Mode toggle | **Top toolbar** (main window button) |
| Temporal Link (group) | Select objects → **Ctrl+G** |
| Temporal Link (select group) | Right-click object → **Select Grouped** or **Ctrl+Shift+G** |
| S3DFactory import | File → Import → Import 3D model → select `.factory` |

---

## Frequently Asked Questions

**Q: ColorMix doesn't seem to apply on some layers — why?**

First, confirm your top surface fill pattern is set to **MonotonicLine** — this is the only pattern that works correctly on complex objects (see §1a). Other patterns (Rectilinear, Monotonic) may appear to work on simple square/circular shapes but will produce incorrect toolchange sequencing on anything more complex.

If the pattern is correct, the next most common cause is the **Min. line length** setting. Fill lines shorter than this threshold are skipped. Try lowering it (default is 1.0 mm). Also check the **Zone** setting — if set to "Topmost only", ColorMix will only apply to the very top layer of the whole object.

**Q: I set PathBlend but the gradient looks like just one or two steps, not smooth.**

The smoothness of the gradient depends on how many fill lines the surface has. A small surface with 5 lines can only have 5 gradient steps. Increasing infill density, widening the surface, or using a smaller line width gives the slicer more lines to work with. PathBlend cannot subdivide individual lines (that feature is planned for a future version).

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

**Q: What does the ΔE indicator on the Blend Suggestion mean?**

ΔE is a perceptual color difference measure. A value below ~5 is generally indistinguishable to the eye. Values above 10–15 indicate the suggested ratios will produce a visible difference from the target color — usually because the available filaments cannot accurately reproduce the target, or because the TD values need calibration.

All of this work is open and free. Fork it, improve it, credit it.

-----

Now all the info from the Original FullSpectrum 0.95 (beta)

<h1> <p "font-size:200px;"> Full Spectrum</p> </h1>

### A Snapmaker Orca Fork with Mixed-Color Filament Support

[![Build all](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml/badge.svg?branch=main)](https://github.com/Snapmaker/OrcaSlicer/actions/workflows/build_all.yml)

---

## ☕ Support Development

If you find this fun or interesting!

<a href="https://www.buymeacoffee.com/ratdoux" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" style="height: 60px !important;width: 217px !important;" ></a>

---

## ⚠️ **IMPORTANT DISCLAIMER** ⚠️

**This fork is currently in active development and has NOT been tested on actual hardware! **

- **Not Production Ready**: The mixed-color filament feature is experimental and untested
- **No U1 Access**: Development is being done without access to a Snapmaker U1 printer
- **Help Needed**: If you have a U1 and are willing to test this fork, please reach out!
- **Use at Your Own Risk**: This software may produce incorrect G-code or unexpected behavior

**I am actively seeking testers with Snapmaker U1 printers to help validate and improve this feature.**

---

**Full Spectrum** is an open source slicer for FDM printers based on Snapmaker Orca and OrcaSlicer, optimized for Snapmaker's U1 multi-color 3D printer with independent tool heads. This fork adds support for virtual mixed-color filaments, enabling you to create new colors by alternating layers between physical filaments.
 


# Download

### Stable Release
📥 **[Download the Latest Stable Release](https://github.com/ratdoux/OrcaSlicer-FullSpectrum/releases)**  
Visit our GitHub Releases page for the latest stable version of Full Spectrum, recommended for most users.

# Features

## Mixed-Color Filaments
Full Spectrum includes support for **virtual mixed-color filaments** designed for the Snapmaker U1 multi-color printer with independent print heads.

### How It Works
- **Create new colors by mixing**: Combine two physical filaments to create a new color appearance through layer alternation
- **Example**: One layer of red + one layer of green = apparent yellow color
- **Customizable ratios**: Adjust the alternation pattern (e.g., 2:1 ratio = two layers of filament A, one layer of filament B)

### Features
- Automatic generation of all possible color combinations from your loaded filaments
- Visual preview showing the additive color blend
- Enable/disable individual mixed filaments
- Per-layer resolution control with customizable ratios
- Seamless integration with the existing filament management system

### Using Mixed Filaments
1. Load 2 or more physical filaments in your printer
2. The "Mixed Colors" panel will automatically appear in the sidebar
3. Each combination shows:
   - Color preview swatch
   - Component filaments (e.g., "Filament 1 + Filament 2")
   - Layer ratio controls (spin controls for fine-tuning)
   - Enable/disable checkbox
4. Mixed filaments can be assigned to objects just like physical filaments
5. During slicing, the mixed filament resolves to alternating layers of its components

### Dithering Settings
Full Spectrum includes advanced dithering controls to fine-tune the layer alternation behavior for mixed filaments. These settings are found in **Others → Dithering** in the print settings:

#### Dithering Cadence Height A & B
- **What it does**: Controls the height (in mm) of each alternating segment for the two component filaments
- **Cadence Height A**: The height of layers using the first filament in the mix
- **Cadence Height B**: The height of layers using the second filament in the mix
- **Example**: Setting A=0.3mm and B=0.15mm creates a 2:1 ratio pattern where you get twice as much of filament A as filament B
- **Use case**: Fine-tune color intensity by adjusting the relative amounts of each component color

#### Dithering Step Size
- **What it does**: Defines the Z-height increment (in mm) for each dithering step
- **Purpose**: Controls the resolution of the layer alternation pattern
- **Default**: Typically matches your layer height setting
- **Advanced usage**: Set smaller values for smoother color transitions, or larger values for more distinct color banding
- **Compatibility**: Must be compatible with your printer's Z-axis resolution

These settings give you precise control over how your mixed colors appear in the final print, allowing you to achieve different visual effects from the same filament combinations.

### Technical Details
- Virtual filament IDs start after physical filaments (e.g., with 4 physical filaments, first mixed ID is 5)
- Layer-based alternation is computed during tool ordering
- Works with all existing features: supports, infill, and multi-material painting

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
**Full Spectrum** is forked from Snapmaker Orca, which is originally forked from Orca Slicer by SoftFever.

Orca Slicer was originally forked from Bambu Studio, it was previously known as BambuStudio-SoftFever.
Bambu Studio is forked from [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community. 
Orca Slicer incorporates a lot of features from SuperSlicer by @supermerill
Orca Slicer's logo is designed by community member Justin Levine(@freejstnalxndr)

## Acknowledgements
Special thanks to [u/Aceman11100](https://www.reddit.com/user/Aceman11100/) for the inspiration and idea behind the mixed-color filament feature!  


# License
Full Spectrum is licensed under the GNU Affero General Public License, version 3. Full Spectrum is based on Snapmaker Orca.

Snapmaker Orca is licensed under the GNU Affero General Public License, version 3. Snapmaker Orca is based on Orca Slicer by SoftFever.

Orca Slicer is licensed under the GNU Affero General Public License, version 3. Orca Slicer is based on Bambu Studio by BambuLab.

Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

Orca Slicer includes a pressure advance calibration pattern test adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.

The Bambu networking plugin is based on non-free libraries from BambuLab. It is optional to the Orca Slicer and provides extended functionalities for Bambulab printer users.

Filament color blending is powered by [FilamentMixer](https://github.com/justinh-rahb/filament-mixer), an openly licensed library.

# Feedback & Contribution
We greatly value feedback and contributions from our users. Your feedback will help us to further develop Full Spectrum for our community.
- To submit a bug or feature request, file an issue in GitHub Issues.
- To contribute some code, make sure you have read and followed our guidelines for contributing.
