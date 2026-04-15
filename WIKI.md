# OrcaSlicer FullSpectrum — Neotko Feature Pack · User Guide

> Features conceived and designed by **[Neotko](https://github.com/neotko)** — inventor of *Neosanding*, now known as **Ironing** in OrcaSlicer, PrusaSlicer, Bambu Studio and Cura.

This fork adds a set of surface quality, color blending and workflow features on top of OrcaSlicer FullSpectrum (Snapmaker base). This guide explains what each feature does and how to use it — no programming knowledge required.

---

## Table of Contents

1. [Surface Color Mixer](#1-surface-color-mixer)
   - 1a. [ColorMix — per-line color patterns](#1a-colormix--per-line-color-patterns)
   - 1b. [MultiPass Blend — multiple passes per layer](#1b-multipass-blend--multiple-passes-per-layer)
   - 1c. [PathBlend — smooth gradient across the surface](#1c-pathblend--smooth-gradient-across-the-surface)
   - 1d. [Zone and filament filters](#1d-zone-and-filament-filters)
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

The Surface Color Mixer is the umbrella feature for everything that happens on **top and penultimate surfaces** when you have more than one filament loaded. It lives under **Quality → ColorMix & Multi-Pass Blend** in the process settings, and has a single "Edit…" button that opens a dialog where you configure everything.

Three different effects are available. You pick one per surface (Top and Penultimate independently):

| Effect | What it does |
|--------|-------------|
| **ColorMix** | Alternates filaments line by line following a repeating pattern you define |
| **MultiPass** | Reprints the same surface multiple times, each pass with its own filament, angle, speed and fan |
| **PathBlend** | Creates a smooth color gradient across the surface by blending two or more filaments proportionally |

ColorMix and MultiPass are mutually exclusive (you can only have one active at a time). PathBlend works on top of MultiPass and can be used alongside it.

---

### 1a. ColorMix — per-line color patterns

**What it does**

ColorMix alternates which filament prints each fill line on a top or penultimate surface. You define a *pattern* — for example `1212` — and the slicer loops that pattern across all lines in the surface. Line 1 → T1, line 2 → T2, line 3 → T1, line 4 → T2, and so on. Each real toolchange goes through the wipe tower exactly as normal.

The result is a striped or woven color effect on the top surface, without any manual toolchange programming.

**How to set it up**

1. Open **Quality → ColorMix & Multi-Pass Blend → Edit Color Patterns…**
2. In the dialog, choose **ColorMix** for the Top surface, the Penultimate surface, or both.
3. Click the colored filament buttons to build your pattern visually. The pattern is just a string of tool numbers (`1`, `2`, `3`…) that repeats.
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

Instead of printing the top surface once, MultiPass prints it **2 or 3 times** (configurable) within the same layer. Each pass:
- Uses a **different filament** (optional — you can repeat tools)
- Has its own **fill angle** (e.g. pass 1 at 0°, pass 2 at 90°, creating a cross-hatch)
- Has its own **speed** and **fan** settings
- Has its own **line width ratio** — each pass is narrower so they tile together without over-extruding (the sum of ratios should be around 1.0 for full coverage)

**Common uses**
- **Cross-hatch texture**: two passes at perpendicular angles with two different colors
- **Neosanding evolved**: one main pass at full ratio + one glaze pass at low ratio with a silk filament
- **Color blending**: two passes at slight ratios (e.g. 0.5 + 0.5) with different colors, which the eye blends

**How to set it up**

1. Open **Edit…** → choose **MultiPass** for the desired surface.
2. Set the number of passes (1, 2 or 3).
3. For each pass: pick a tool, set the width ratio, angle, fan and speed.
4. The Σ indicator in the dialog shows the sum of all ratios — aim for ~1.0 for full coverage.
5. OK → slice.

**1-pass mode**: With a single pass at ratio < 1.0, MultiPass deposits a controlled low-flow layer over the existing surface. Useful for glazing or experimental material effects without adding a full extra layer.

---

### 1c. PathBlend — smooth gradient across the surface

**What it does**

PathBlend creates a **continuous color gradient** across the top surface. At one edge of the surface, filament T1 dominates. At the other edge, filament T2 dominates. In between, both filaments are printed at proportional flow, path by path, so the transition is as smooth as the number of fill lines allows.

This is done entirely within a single layer — no extra layers are needed. The slicer adjusts the Z height and extrusion flow of each fill line to blend the two tools.

With 3 or 4 passes, three or four filaments can blend across the surface in sequence.

**Gradient direction**: The gradient always runs across the **Y axis of the build plate** (front-to-back). Rotate your object on the bed to change which direction the gradient crosses the surface.

**Invert gradient**: The "Invert gradient" toggle flips which tool dominates on which side, without rotating the object.

**How to set it up**

1. Open **Edit…** → choose **PathBlend** for the desired surface.
2. Pick the number of passes (2 = two tools; 3 or 4 = more tools, more complex gradient).
3. Assign a filament to each pass slot (T1, T2…).
4. Set **Min ratio** — the minimum flow percentage of the receding filament at the extreme edges (default 5%). Keeping this above 0 means both filaments are always present to some degree across the full surface.
5. Use **Fill angle** to override the fill direction if needed (−1 = auto).
6. OK → slice.

**Note**: PathBlend works best on surfaces with many fill lines (high infill density, wide surfaces). On very small surfaces with only a handful of lines, the gradient will be very coarse (just a few steps).

---

### 1d. Zone and filament filters

These filters appear in the **Color Mixer settings** box at the bottom of the Edit dialog. They apply to whichever effect is active (ColorMix, MultiPass, or PathBlend) on each surface.

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

**Q: Neoweaving is making my printer move Z very rapidly — is that normal?**

Yes. Neoweaving requires rapid Z moves between successive lines on the same layer. If your printer's Z axis is slow or has significant inertia, reduce the **Amplitude** or enable the **Speed %** override for neoweaved lines to give it more time. Start with an amplitude of 0.05 mm and work up.

**Q: MultiPass is over-extruding my top surface.**

Check that the sum of all pass **width ratios** adds up to approximately 1.0. If the Σ indicator in the dialog shows a value well above 1.0, reduce the ratios proportionally. The ratios represent what fraction of the surface each pass covers — Σ = 1.0 means full coverage with no overlap.

**Q: Can I use ColorMix and MultiPass at the same time?**

Not on the same surface — they are mutually exclusive per surface. However, you can apply ColorMix to the top surface and MultiPass to the penultimate surface, or vice versa, since the two surfaces are configured independently.

**Q: Do I need Libre Mode ON to print normally?**

No. Libre Mode is OFF by default and everything works normally without it. Libre Mode is only needed for the specific advanced workflows described in §5: floating objects, world-space import, temporal linking, per-volume XY compensation, and the detachable panel.

**Q: My `.factory` file imported but all objects are in the wrong position.**

Try enabling Libre Mode before importing. With Libre Mode OFF, the slicer re-centers objects to the build plate, which loses the relative positioning from the Simplify3D project.

---

*OrcaSlicer FullSpectrum — Neotko Feature Pack*
*Designed by Neotko · Implementation by Claude (Anthropic)*
