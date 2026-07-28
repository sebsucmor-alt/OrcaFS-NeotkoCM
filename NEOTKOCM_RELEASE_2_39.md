## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.3.9 — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.**

**2.3.9 is an incremental release on top of 2.3.8** (see `NEOTKOCM_RELEASE_2_38.md` and earlier
notes for the full feature set). This notes file is a **running draft** — it grows as sessions land,
until this version ships. Everything remains **opt-in**: at defaults the build behaves like stock
Snapmaker Orca.

---

## What's new in 2.3.9

### PerObject Support — support that actually knows the other objects are there

Stock slicers treat every object's support as if that object were alone on the plate. Put two
separate (non-Assembled) objects close enough that their supports share space and the result is a
mess: each object's support is generated blind to the other and grows straight through the neighbor
and through the neighbor's own support. The usual workaround — merge everything into one Assembled
object — changes how the parts slice and isn't what you want when the objects are genuinely
separate. Orca has left this gap unimproved for years.

**PerObject Support** (new checkbox, right under **Enable support**) closes it. With it on, an
object's support treats every *other* object on the plate — its body **and** its already-generated
support — as an obstacle to route around, keeping the normal support/object XY distance from them,
exactly as it already does for its own body. No Assemble, no boolean tricks, no merged object: the
parts stay independent and the support simply stops colliding.

![PerObject Support — tree supports generating across several separate, non-Assembled cubes: each object's support respects the others and still builds correctly, with no Assemble needed](docs/images/Per-Object-Supports.gif)

What it covers:

- **All tree styles** — Default, Tree Slim, Tree Strong, Tree Hybrid **and** Organic. Both of the
  fork's tree engines (the hybrid engine behind Slim/Strong/Hybrid, and the separate Organic
  engine) honor it.
- **Normal / Grid support** — the classic engine avoids neighbors too.
- **NeoWave support** — inherits the same behavior (it's built on the classic engine).
- **Support vs. support** — not just bodies: when two objects' supports would tangle, the second
  one generated routes around the first one's finished support, not only around its body.
- **Moving a part re-solves its neighbors** — nudge one object and every nearby object's support
  regenerates against the new position automatically. (Stock Orca doesn't even invalidate an
  object's *own* support when you move it — it never needed to, because support was a per-object
  silo. PerObject Support adds that dependency edge.)

How to use it: enable support, tick **PerObject Support** on each object that should avoid the
others, and slice. It only takes effect when the plate prints **all objects at once (by layer)** —
in sequential by-object printing the neighbors aren't on the bed yet, so avoidance is inert.

**Trade-off (deliberate, documented):** the wide first-layer base/brim that support engines grow for
bed adhesion is a free outward offset that stock code never clips against anything — so under
PerObject Support that first-layer expansion is dropped to keep bases from spilling across objects.
Supports under this mode grab the bed a little less at the very first layer in exchange for never
colliding. This is a limitation of making an existing, decades-old support engine behave more
realistically than it was designed to, and it only applies when the toggle is on.

> Under the hood this reuses the real cross-instance contact detector shipped in 2.3.8 (below):
> support avoidance and floating-object detection now speak the same geometry.

### Real floating-object detection — measured contact instead of a blind guess

Orca's "this object is floating, enable support" warning used a blind heuristic: *if the first layer
is empty, it's floating.* That's wrong for exactly the case that's becoming common on multi-object
plates — an object resting **on top of another object** has an empty first layer of its own yet
isn't floating at all. Stock Orca either nags you to add support that isn't needed or (worse) its
internal suppression silences genuinely floating islands.

This build replaces the guess with an actual measurement. For an object whose lowest geometry starts
above the bed, each of its instances is checked against the bed **and against the top surface of
every other object's instances** — real Z gap, real XY footprint, per-instance (two copies of the
same object at different spots are judged independently). Contact means it's supported; a real gap
means the warning fires — and now it fires on the right object, per island, only when something is
genuinely hanging.

- An object stacked on another object no longer gets a bogus "floating" warning.
- A genuinely floating island still surfaces the warning — the old blanket suppression that hid real
  floaters is gone.
- The tree-support sharp-tail seed uses the same per-island check, so it stops flagging islands that
  actually rest on a neighbor.

This is the groundwork PerObject Support is built on: once the slicer can *measure* what rests on
what, it can also route support around it.

### NeoWave Support — opinionated, tested defaults; less clutter

Selecting **Type: NeoWave** now locks the support to its intended, tested shape — a **Hollow** base
capped by a **Wave (NeoWave roof)** — instead of exposing the full grab-bag of base/interface
patterns that don't apply to it and only caused confusion. **Base pattern** and **Interface pattern**
are set to Hollow / Wave and greyed out while NeoWave is the type; switch back to a normal support
type and the full choices return. Nothing about the NeoWave engine itself changed — this is purely
removing controls that were never meant to be touched here.

### Align & Stack — see the landing spot before you click it

**Align & Stack** (the gizmo for placing one object against another) gets a viewport-native rework.
Two changes:

- **Capped to two objects.** The gizmo only ever relates an anchor (**#1**) and the object that moves
  against it (**#2**) — a longer ordered chain was never actually used in practice, just one more
  thing to keep track of. Clicking a third object now swaps out #2 instead of extending a chain.
- **Live ghost previews in the 3D view.** With #1 and #2 picked, every possible placement (the 5
  face-touch/flush ops plus the 3 centering ops) is drawn directly in the viewport as a translucent
  wireframe of where #2 would land, computed with the exact same math the click runs — so the
  preview can't lie. Each ghost carries a big, semi-transparent mini-cube icon right at the seam
  between #1 and the ghost: click it to run that placement on the spot. No more decoding which of
  two mirrored X-/X+ icons means "the far side" — you see the actual candidate position and click it.

![Align & Stack — with anchor #1 and object #2 picked, every possible landing spot is drawn as a translucent ghost in the viewport, each with a clickable cube icon at the seam](docs/images/AlignStack.gif)

Prompted by real usage friction: the small isometric icons in the side panel were hard to read in
dark mode, and mirrored icons (X- vs X+, Y- vs Y+) required guessing which one meant "behind."
Seeing the real candidate position removes the guessing entirely.

### True Objects (Gravity) — real floor, honest bridges

Stock slicing quietly assumes an object is alone on the bed: "my layer 0 is the bed", "below me is
only my own previous layer", "support only ever grows from the bed". Every one of those breaks the
moment a **separate** (non-Assembled) object rests on top of another one — the touching face gets
misclassified as a bridge over open air, even though there's solid material right underneath it.

**True Objects** measures what's *really* underneath every surface — the bed, another object, or
genuine air — and slices **by area, not by object**: the part of a face resting on something solid
prints as a normal contact surface with the correct fill angle and speed; the part that's genuinely
unsupported still prints as a real bridge — even when both happen in the same layer of the same face.

What changes with it on:

- A face resting on another object stops being a false bridge — correct fill angle, no bridge
  speed/fan where it doesn't belong.
- An object's own floating first layer, if genuinely hanging over open air, now becomes a real
  bridge instead of always solid.
- Perimeter overhang is measured against the real floor, not just the object's own layer below — a
  wall resting on a neighbor is no longer flagged as overhang.
- Support stops being requested under a face that's actually resting on another object — the
  neighbor's top counts as ground.
- Elephant-foot compensation is never applied to a face that isn't touching the bed, so a stacked
  contact face keeps its true size instead of shrinking for no reason.
- **PerObject Support** (above) is forced on for every object while True Objects is active, without
  touching each object's own saved setting — turn True Objects off and everything reverts exactly.

**One toolbar button now drives both.** True Objects used to be a second toggle next to Libre Mode;
the two of them always moved together in practice, which just made daily use more confusing than it
needed to be. Turning **Libre Mode** on now also turns True Objects on (support avoidance, honest
bridges, the works); turning it off turns both off. Existing projects that already relied on Libre
Mode's old floating behavior were migrated automatically when this changed — nothing that was
floating suddenly drops to the bed.

Pairs naturally with **Align & Stack** (above): align/stack places pieces exactly on top of each
other, True Objects is what makes the slicer treat the touching face honestly afterward.

**Limits (v1):** support still only lands on the bed or the object's own body — it doesn't yet land
*on* another object's top (a future extension). Auto-arrange/auto-orient can still scatter a
hand-placed stack, since it has no concept of "these are meant to stay stacked" — don't run it after
stacking by hand. Only takes effect in by-layer printing (sequential by-object printing has no
guarantee the neighbor exists yet at a given height).

### Snap & Drag — objects rest on the real surface below while you drag them

A follow-up to True Objects: with it on, dragging an object can now make it **rest on whatever it's
really above** instead of leaving it floating wherever you let go of the mouse. Turn it on per
object from the right-click menu — **"Snap & Drag"** — greyed out until True Objects itself is on,
since it's a sub-behaviour of it, not a separate switch.

![Snap & Drag — dragging an object over another makes it rest on the real surface underneath instead of staying where the mouse was released](docs/images/SnapANDDrag.gif)

- **Footprint overlap, not a raycast under the cursor.** A corner that barely grazes a pillar
  doesn't count as resting on it — the overlap has to clear a threshold to engage, and a lower one
  to stay engaged, so the object doesn't flicker up and down when the drag passes near an edge.
- **Real geometry, not the bounding box.** Once a candidate qualifies, its landing height comes
  from a few actual raycasts against that object's mesh — so a hollow box (tall rim, low interior
  floor) lands correctly depending on exactly where the overlap sits, instead of always reading
  rim height as if the object were solid.
- **Highest surface wins**, so it never sinks into a shorter neighbour that also happens to
  overlap; but if an object has multiple instances that would land on pillars of different
  heights, the whole object uses the **lowest** of those targets — one instance shouldn't drag the
  rest of the object up because it happened to be over something tall.
- **Never invents a bed-drop.** If nothing qualifies underneath, the object stays exactly where it
  is — Snap & Drag only ever pulls something down onto a floor it actually found; falling back to
  the bed when nothing is there would undo True Objects' whole point.
- **Landing indicator.** While engaged, a soft layered shadow projects onto the real landing
  surface (a cheap approximation of a contact shadow — a few translucent rings, darker toward the
  centre, no texture or extra shader involved), plus a short vertical marker so the spot stays
  visible even when the shadow itself is hidden under the object from the current camera angle.

**Limits (v1):** vertical (-Z) only — it doesn't help with side-by-side mating inside Assemble
View, which has no single "down" direction to raycast against. No chaining: moving the object
underneath does not drag whatever is resting on it along for the ride.

### Realistic Shading — Phong, ambient occlusion and real cast shadows in Prepare

The Prepare tab's 3D view has always used flat, unshadowed lighting for objects. This build brings
the richer shading RealColor already used for shells in the Preview tab — Phong lighting, fresnel,
and screen-space ambient occlusion in creases/corners — to the normal object view too, plus two
kinds of shadow that stack on top of each other:

- **Contact shadows** where surfaces meet at close range — an object resting on another, a rib
  meeting a wall — darken right where they touch, not just where they meet the bed.
- **A real directional cast shadow.** Unlike the flat, always-on-the-bed silhouette this build used
  to draw, an object's shadow is now computed from an actual light-eye view of the scene, so it
  correctly falls **onto other objects** (not just the bed) and self-shadows a piece's own concave
  geometry — the situations a flattened-to-bed silhouette structurally could never handle.

It's automatic once Libre Mode is on: no extra toggle, and it works while you select, move, and
compare objects. Turn Libre Mode off and the view goes back to the stock flat look — nothing
changes for a normal build.

### Remove Slice Cache — a real manual cache-invalidation escape hatch

The old **"↺ Refresh Part"** side button (a narrow LibreMode tool that pushed a Part's settings onto
its parent Object) is gone — it cluttered the toolbar without pulling its weight. In its place, the
object right-click menu gets **"Remove Slice Cache"**: a general-purpose, always-available action
that discards every cached slice result for the plate and forces the next slice to recompute
everything from scratch. Use it if a slice result looks stale or wrong and you don't want to touch
settings or reopen the project to force a clean recompute.

### Typographic Spacing — real kerning for embossed text, and a font search that works

Stock Orca doesn't compose text, it **drops glyphs**: each letter is placed at a fixed advance and
that's it. The **Char gap** setting it offers is *tracking* — the same shift applied between every
pair of characters — which is not kerning. Kerning is the correction a type designer builds into the
font for specific pairs, so that `AV`, `To` or `Wa` close the diagonal gap that plain advances leave
open. Orca never read it: before this release there was not a single kerning call anywhere in the
codebase.

**Typographic Spacing** adds it. A new **Font kerning** checkbox in the Emboss gizmo's Advanced
section makes embossed text use the kerning pairs designed into the font. **Char gap** stays exactly
as it was and the two are independent — tracking shifts everything, kerning fixes individual pairs.

Under the hood the text layout was rebuilt first: the glyph advance used to be baked into the shape
cache (which is keyed by character only), so a per-*pair* value could not be expressed at all. Text
composition now happens in one place, `Emboss::layout_text()`, which both the geometry and future
typographic controls go through.

**macOS fonts get a fix nobody else ships.** The font library Orca uses reads the Microsoft `kern`
table and OpenType GPOS, but silently ignores the **Apple `kern` version 1.0** table — which is
exactly what the macOS system fonts use, Helvetica among them. On a Mac, kerning would have been
dead for the first fonts anyone reaches for. This release adds a reader for that format, so Apple
and Microsoft kerning tables both work. Across a typical macOS font library (~900 styles), roughly
three quarters now kern; when a font genuinely carries no kerning data the checkbox is disabled and
says **"Font has no kerning data"** rather than sitting there doing nothing.

**Bonus fix — the font search box.** Typing in the font selector made the whole list disappear
instead of filtering it. The search was implemented correctly, but the cached font list (used from
your second launch onward) filled only the names shown on screen and left the list the search box
matches against empty — so any keystroke matched zero fonts. Only the very first launch on a clean
profile ever worked. Fixed: font search now filters as you type. This bug is present in upstream
Orca too.

Everything here is opt-in and stored per style in the project: with **Font kerning** off, embossed
text is byte-for-byte identical to what earlier versions produced.

---

## Notes

- PerObject Support is **off by default**; nothing about a normal single-object (or Assembled) plate
  changes.
- The feature is stored per-object in the project (3mf).
- First public appearance of cross-object support avoidance in this slicer family.
- True Objects is **off by default** for new projects (auto-enabled on upgrade only for projects
  that already had Libre Mode's floating active); nothing about a normal bed-only plate changes.
  It no longer has its own toolbar button — the **Libre Mode** button now drives both.
- Snap & Drag is **off by default** and viewport-only — it never touches PrintConfig and never
  triggers a re-slice by itself; it just decides where a drag ends up.
