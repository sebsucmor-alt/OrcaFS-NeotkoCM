# OrcaSlicer FullSpectrum — Neotko Feature Pack

> All features in this pack were **conceived and designed by [Neotko](https://github.com/neotko)** — creator of *Neosanding* (now known as **Ironing** in OrcaSlicer, PrusaSlicer, Bambu Studio and Cura).
> Implementation assistance: Claude (Anthropic).

This document is a **synthetic, enumerated index** of every feature this fork adds on top of vanilla OrcaSlicer. For the end-user guide (how each control behaves, screenshots, FAQs) see [WIKI.md](WIKI.md). For internal implementation notes, see the `docs/` folder.

---

## 1 · Surface Color Mixer (umbrella)

A single dialog (Quality → ColorMix & Multi-Pass Blend → Edit…) that hosts four mutually-arranged surface effects across two independent zones (Top / Penultimate).

### 1.1 — ColorMix
- Per-line tool assignment across top / penultimate surfaces.
- 5 pattern modes: Pattern string, Linear 2-color, Linear 3-color, Custom bands, Hard-band.
- 6 easing curves: Linear, Ease-In, Ease-Out, Ease-In-Out, Gamma, Hard band.
- Bresenham-style dithering for smooth percentage splits.
- Color overlap control, gradient invert, min-line filters.
- Requires **MonotonicLine** fill pattern for correct results on complex geometry.

### 1.2 — MultiPass Blend
- 1–3 passes per layer, each with independent tool, fill angle, width ratio, fan PWM, speed %, GCode pre/post, Pressure Advance override.
- Vary-pattern, sort-by-ratio.
- Per-pass prime volume through wipe tower.
- 1-pass mode for glaze / sub-flow experimental layers.
- Sub-layer Z stacking for adhesion and color separation.

### 1.3 — PathBlend
- Continuous Z + flow gradient across the surface within a single layer.
- 2/3/4 passes (2/3/4 filaments blended).
- Min ratio, max ratio, 4 easing modes, invert, fill-angle override.
- Per-scanline staircase model (canonical s88+).
- Continuous chain dispatcher + atomic chain scheduler (no micro Z-hops, no cross-object travel artifacts).
- Synthetic cross-product suppression on NeoTower.
- Z-step compression for single-event wipe tower scheduling.
- Advanced runtime toggles (canon scheduler, dispatcher behaviour) exposed in UI gear ⚙.

### 1.4 — Zone & Filament Filters (shared)
- **Zone** selector per surface: All surfaces / Topmost only.
- **Filament filter** (0–16): restrict effect to a specific filament's regions in MMU objects.

### 1.5 — TD Preview & Blend Suggestion
- Transmission Density sliders (per filament slot, saved per machine).
- Live blended swatches: Top stack, Penu stack, Result through opacity.
- **Calculate B** — exhaustive ΔE-minimising search to reverse-engineer a MultiPass recipe for a target mixed color.
- ΔE indicator with traffic-light feedback (green / orange / red).

### 1.6 — Line Distribution Mode
- 4 algorithms for mapping color slots to physical fill lines: Default, GeoSort, LaneQuant, DirCluster.
- Applies to both ColorMix and PathBlend.
- Fixes coloring on holes, concavities, disconnected sub-regions, rotated sub-region fills.

### 1.7 — Surface Color Mixer dialog UX
- Two-zone card layout with mini-preview per effect.
- Reorder buttons (▲/▼) for passes without destroying gradients or blobs.
- Lane mode panel.
- "Mode: Linear" with advanced gear ⚙ for runtime toggles.
- Persistence of advanced toggles via `app_config` (label `*` when modified).
- TD Preview as collapsable section.

---

## 2 · 3D Painter (Surface Effect Profiles)

### 2.1 — Profile system
- Save any Surface Color Mixer / MultiPass / PathBlend configuration as a named profile.
- Profiles travel inside the 3MF project (no global library).
- Per-volume slot tables; up to 15 different profiles per object.
- Profile manager: Load, Update, Rename, Delete; payload tags `[CM:* PB:* MP:*]`.
- Orphan-paint warning at slice time when a referenced profile is missing (non-critical, slice continues).

### 2.2 — Painter gizmo
- Left-side gizmo toolbar entry, parallel to MMU / Fuzzy / Seam painters.
- Brushes: Circle, Sphere, Triangle, Smart Fill (default).
- Configurable Smart Fill angle threshold.
- Scrollable profile list as brush palette.
- **Erase mode** checkbox (s90) — left-click erases under the active brush.
- **Shift + Left-click** quick erase.
- **Erase all** for full clear of a volume.

### 2.3 — Slice-time behaviour
- Painter mode activates whenever any facets are painted on an object.
- Painted area → its profile applies (preset ignored).
- Unpainted area of a painted object → no effect.
- Per-layer dominant-slot lookup; multiple profiles co-exist at the same Z.
- Wipe-tower planner and GCode dispatcher share the same lookup (no `unexpected toolchange` drift).
- **Penu role autonomy**: penultimate layers are auto-forced to 2 when a painted profile declares penu activity.
- Paint copied on object duplicate.

### 2.4 — 3MF round-trip
- Profile manager payload, per-volume slot maps, and per-triangle paint all persist in the project file.

---

## 3 · Neoweaving

- Alternating-Z fill on successive lines; pattern inverts layer-to-layer so peaks nest into valleys.
- Mechanical interlocking — measurable improvement in inter-layer adhesion and vibration damping.
- Per-zone filter (Top only / All solid infill).
- Penultimate-layer extension (apply to N layers below top).
- Amplitude, min-length, and speed % controls.
- Linear mode only (Wave mode disabled until memory regression resolved).
- Per-object tristate override for sparse infill.
- Fill-angle lock across affected layers for maximum interdigitation.

---

## 4 · Penultimate Top Layers

- Dedicated extrusion role (`erPenultimateInfill`) for the N layers immediately below the top surface.
- Independent infill density.
- Surface effects (ColorMix / MultiPass / PathBlend / Neoweaving) can target penu independently.
- Enables depth / show-through effects with translucent top layers.

---

## 5 · Monotonic Interlayer Nesting

- Automatic half-line-spacing shift on odd layers for Monotonic / MonotonicLine fills.
- Layer-N lines sit over layer-(N−1) gaps.
- Improves bonding and reduces grazing-angle visibility of layer lines.
- Zero configuration.

---

## 6 · Libre Mode

A runtime toggle that unlocks OrcaSlicer for multi-part assembly, professional, and experimental workflows.

### 6.1 — Floating objects
- Objects at arbitrary Z (above bed or partially below it).
- Missing-initial-layer downgraded from error to warning.

### 6.2 — World-space import
- STL / .factory imports preserve source-file XYZ instead of re-centering to the bed.

### 6.3 — Temporal Link
- Persistent object groups stored in the 3MF.
- Ctrl+G to link, Ctrl+Shift+G to select group, Break Link / Break All Links.
- Restored automatically on project reopen.

### 6.4 — Per-volume XY compensation
- Independent contour/hole compensation per volume inside Assembled objects.
- Delta applied to VolumeSlices before region merge.
- Useful for multi-material assemblies with differing shrinkage.

### 6.5 — Bridge infill disable
- Auto-disables bridge detection while Libre Mode is on, avoiding misfires on floating geometry.

### 6.6 — Detachable Process Panel
- Process panel floats as an independent dockable window.
- Per-mode layout persisted across sessions (normal vs. Libre).
- Multi-monitor friendly.

### 6.7 — Surface Density UI
- Top / bottom surface density controls exposed for direct per-object override.

---

## 7 · S3DFactory Import

- Direct import of Simplify3D `.factory` project files.
- Preserves multi-object layout and per-object extruder assignments.
- World-space coordinates preserved when Libre Mode is on; re-centered otherwise.

---

## 8 · Workflow UX additions

- **Less-Used Toggle** — hides rarely-used options; second button restores them.
- **Refresh Part** button in the top toolbar (Libre Mode active).
- **Save as profile** buttons on every effect dialog (SCM, MultiPass, PathBlend).
- **Manage profiles** dialog (load / update / rename / delete).
- Unified "ColorMix & Multi-Pass Blend" optgroup with mutually-exclusive pills and gated Advanced buttons.

---

## Known Limitations (current beta)

For the active bug list shipped with the beta, see [docs/INNERCHAT/KNOWN_ISSUES.md](docs/INNERCHAT/KNOWN_ISSUES.md). Summary:

- MMU painted regions disable Sandwich on the whole object (per-region fix pending).
- PathBlend on Penultimate is hidden in the UI (engine works; second-stair gradient-direction bug).
- Neoweaving Wave mode disabled (memory regression).
- Minor air-gap on stacked-cube supports at certain Z values.
- Inverted fuzzy skin shipped with `[0]→[1]` workaround pending upstream diff.

---

## About Neotko

**Neotko** is a Spanish maker, hacker, and 3D printing researcher whose work on layer mechanics has quietly shaped how modern slicers approach surface quality.

In the early days of FDM development, Neotko invented and published **Neosanding** — a technique where the nozzle makes a final low-flow ironing pass over top surfaces to flatten layer lines. The idea spread through the RepRap community and was eventually adopted by PrusaSlicer under the name **Ironing**, then picked up verbatim by Bambu Studio, OrcaSlicer, and Cura. It is now a standard feature used by millions of printers worldwide. Neotko received no credit in any of those implementations.

This feature pack is Neotko's continued work on the same frontier: what happens at the boundary between layers and on top surfaces.

- **Neoweaving** — structural Z-interlock between layers.
- **Surface ColorMix** — per-line toolchange patterns on top surfaces.
- **MultiPass Blend** — multiple full-contribution passes per layer with per-pass tool, angle, fan, speed.
- **PathBlend** — first open-source continuous intra-layer Z+flow material gradient on FDM.
- **3D Painter** — paint surface effects directly on geometry, persisted in 3MF.
- **Libre Mode** — physics-free professional and assembly workflows.

All of this work is open and free. Fork it, improve it, credit it.
