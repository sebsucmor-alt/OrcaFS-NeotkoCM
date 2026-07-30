## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.4.0 — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.**

**2.4.0 is an incremental release on top of 2.3.9** (see `NEOTKOCM_RELEASE_2_39.md` and earlier
notes for the full feature set). This notes file is a **running draft** — it grows as sessions land,
until this version ships. Everything remains **opt-in**: at defaults the build behaves like stock
Snapmaker Orca.

---

## What's new in 2.4.0

### Clean flat surfaces in the 3D view — the "jagged shading" is gone

If you have ever loaded a CAD part, looked at a large flat face, and seen faint grey streaks or
chevron patterns crawling across it as you orbited the camera — patterns that follow the mesh
triangles and vanish the moment you snap to an exact Top view — that is fixed.

There were two separate causes, and both are addressed.

**1. Ambient occlusion was shadowing flat faces against themselves.** With Libre Mode's realistic
shading on, the ambient occlusion pass decided whether a neighbouring pixel was an occluder by
comparing raw depth: anything nearer to the camera counted. That test is only correct when the
surface faces the camera head-on. Tilt a large flat face and every neighbouring pixel on its far
side is genuinely deeper *while lying in exactly the same plane* — so the face permanently shaded
itself, in a pattern modulated by the mesh triangles. It disappeared in exact Top view for the same
reason it appeared everywhere else: a flat face seen straight down has constant depth, so there was
nothing to mis-measure.

Ambient occlusion now compares each sample against the depth it *would* have if it were coplanar
with the pixel being shaded, using the surface's own screen-space slope. A perfectly flat surface
self-occludes exactly zero at any viewing angle, while real creases, corners and neighbouring parts
still darken exactly as before. You keep the full depth and contact shading — nothing had to be
turned down to make the artifact go away.

**2. Badly tessellated meshes got noisy shading normals.** Slicers derive a triangle's shading
normal from its three corners. That is exact for a healthy triangle and close to meaningless for a
sliver — the needle-thin triangles CAD meshers love to emit when they fan-triangulate a flat face,
sometimes microns wide and tens of millimetres long. The angular error grows as the triangle gets
thinner, so a face tiled with slivers ends up with every sliver pointing somewhere slightly
different.

New preference, **Preferences → General → "Smooth shading of imported meshes"** (**on by default**):
shading normals are now averaged across each smooth surface, weighted by triangle area, with a
crease angle that keeps genuine sharp edges sharp. Because slivers have almost no area, their bad
normals contribute almost nothing and the noise cancels itself out. As a bonus, curved surfaces —
fillets, rounded rims, bores — look noticeably smoother instead of faceted.

This affects **shading only**. Picking, painting, supports and slicing all still see the exact same
mesh they saw before; nothing about your G-code changes. Turn the preference off and you are back to
the previous look immediately.

---

### Bridging infill extra expansion — anchor your bridges before they cross

A new **Quality → Bridging → "Bridging infill extra expansion"** setting (mm, default **0**).

Orca already grows every bridge region a little way into the surrounding area so the strand has
somewhere to land, but the amount is hard-wired and derived from your **wall count** — raise
`wall_loops` for stiffness and you silently change the anchoring of every bridge in the part. This
setting adds millimetres on top of that automatic amount, and lets you set bridge anchoring
independently of how many walls you use.

What it does in practice: the bridge takes over the neighbouring region of the same part, so the
nozzle is already extruding over solid, supported material before it reaches open air. A strand that
starts anchored behaves very differently from one that starts in mid-air.

Turn it up far enough and the bridge claims the whole surrounding region, so the entire layer prints
as one continuous bridge. That is a legitimate and useful way to use it — it removes the seam artefact
where the bridged area meets the supported area. **There is no cap**: set it as high as you need
(999 mm is accepted, and "cover the entire layer" is a normal request).

![Bridging infill extra expansion, off vs on. Off: the bridge stops where the supported area begins and that area prints as ordinary solid infill at its own angle, leaving a visible boundary between the two. On: the bridge has taken over the neighbouring supported region, so the whole span prints as one continuous bridge in a single direction](docs/images/Bridging%20Infill%20Extra%20Expansion.png)

- Fully additive: at **0** the slicer behaves exactly as before, byte for byte.
- The custom **bridge angle** is honoured on the expanded region too.
- Only external bridges are affected.

---

### Painted Bottom surfaces — full pass stack, and no more hidden switch

The Bottom zone of a painted Sandwich recipe used to be clamped to a **single pass** unless you found
and ticked a "Supported bottom — control" checkbox. That checkbox is gone, and so is the clamp.

Bottom now uses the **whole authored stack** everywhere — bed contact, faces resting on supports, and
real bridges alike — exactly like Top and Penultimate. Pass #1 always keeps bridge flow, speed and
fan, so the layer that crosses open air is still printed as a bridge; the passes above it print as
controlled solid.

The **fill angle** is now honoured on bridges as well. Note that the default bridge angle is planned
to cross perpendicular to its anchors, so rotating it can leave line ends unsupported — bridges are
calibration territory, and this build now trusts you with them instead of quietly overriding you.

**PathBlend on a real bridge** is the one exception. Its ramp/cap staircase subdivides the layer
height, which is fine on solid ground but not over air when **Thick external bridges** is on — there
a bridge is modelled as a round strand and each band would end up with a fraction of its cross
section. In that case PathBlend is automatically converted to **MultiPass using the same colours**,
and the painter tells you so in yellow before you slice. With Thick external bridges off (the
default) PathBlend is left untouched.

---

### MixedFilament Object mode now governs the whole object

With **MixedFilament Object** enabled, the painter greys out and tells you the object is governed by
the MixedFilament recipe. That was only half true: the engine replaced the Top and Penultimate zones
but left the **Bottom** zone resolving to whatever painted recipe was underneath, so an object could
end up printing under two different recipes at once.

The auto-generated recipe now includes a Bottom zone, and the engine applies it. What the interface
promises and what the slicer does are the same thing again.

---

### Sandwich painter — one panel instead of two

The Pro panel used to hide the Bottom zone behind a Top Surface / Bottom Surface switch. Both buttons
are gone: **Top → Penultimate → Bottom** are now edited one after another in a single place, in the
same order they are printed.

The **Recipe | Result** preview moved to the top of the panel where those buttons used to be, and the
**(TD)** grid moved out of Pro entirely — it now lives in the **Object & TD** department (renamed from
"Object"), where it is always visible. Previously TD was drawn at the bottom of that department behind
two early exits, so it was unreachable unless the object already had a MixedFilament assigned and the
mode switched on. It is a project-wide setting and had no business being gated that way.

---

### ColorStitch now works on continuous Monotonic by default

"ColorStitch on Monotonic (continuous)" is no longer a setting — it is always on. This is what lets
ColorStitch work on **Monotonic**, **Rectilinear** and **Hilbert Curve**, and it also works on
**Concentric**, **Octogram** and **Archimedean** (the line distribution is not always uniform there,
but the results are worth having). Projects saved with the old setting turned off pick up the new
behaviour automatically.

**Monotonic Line Replan** and **Monotonic Interlayer Nesting** are now hidden unless you are in
**Develop** mode. Both are research knobs rather than everyday settings.

---

### PathBlend no longer comes out flat depending on the fill pattern

A PathBlend gradient is built as a **staircase**: one physical height per fill line. That quietly
assumed the surface pattern hands the lines over **one at a time** — which `Monotonic line` does, and
plain `Monotonic` does not: it chains its lines into one long zigzag. When that happened the entire
surface arrived as a single line, received a single height, and the "gradient" printed as a flat pass
with **no Z change at all**.

It showed up most clearly on **bottom surfaces**, whose default pattern is `Monotonic`, next to a top
surface set to `Monotonic line` that looked perfect with the very same recipe — but the top would have
broken identically had it been set to `Monotonic`. PathBlend now requests unchained lines for its own
ramp and cap fills, so **the gradient is independent of the surface pattern you picked**, on Top,
Penultimate and Bottom alike.

---

### The painter, gone over end to end

This release spends a session on how the ColorStitch Painter *behaves* — not how it looks. The
individual pieces are small; together they remove most of the friction that made the tool feel
harder than it is.

**Your colour stops disappearing.** After a slice — or after switching tabs, or reloading a project —
the colour swatch kept showing your colour while the brush had quietly stopped being able to paint
with it. Clicking the model simply did nothing, and the only way out was to re-pick the colour from
the palette without knowing that was needed. That is fixed at the root, and the panel now **states the
brush's real condition** next to the swatch: **slot N** (armed on this object), **ready** (chosen, it
will take a slot on the first stroke) or **no colour** (clicking will not paint).

**Painting under MixedFilament Object mode no longer wastes your time.** The panel greyed out, but the
brush in the 3D view kept working — on an object whose painting the engine deliberately ignores. You
could spend paint slots and trigger re-slices for nothing, silently. The lock now covers the brush and
sticker placement; Select and the eyedropper still work.

**Opening the painter with nothing selected is no longer a dead end.** It starts in **Select** so
clicking objects does what the panel says it does, and clicking an object that is not in your set now
**adds it** instead of doing nothing at all.

**Duplicate — at four levels.** Editing a colour in Pro rewrites the profile *in place*, everywhere it
is already painted, so "take this colour and vary it a bit" used to destroy the original with no
obvious way around it. Now there is **Duplicate** next to Save; **right-click a saved swatch** for
Duplicate / Save / Delete; **right-click a pass's preview bar** to duplicate, reorder or delete it;
and a **copy to:** row under each zone copies a whole stack onto another zone (Top → Penultimate /
Bottom and back), translating the ColorStitch pattern and normalising it to the Bottom rules on the
way.

**Pro feels fluid.** Dragging a divider or a number used to schedule a re-slice and rebuild the
on-model weave on **every frame** of the drag. That work is now committed **once, when you release** —
the same rule the TD sliders already followed. Previews still track the drag live.

**You can see what an object is spending.** A new **In use on this object** list shows every paint slot
with its real colour, its name and how many facets it covers, plus **Use** (make it active) and
**Free** (erase it here and release the slot). Until now that information only existed in a debug log.

**The Bottom zone is previewed on the model.** Every on-model preview read the Top recipe only, so a
colour whose Bottom carried its own ColorStitch or PathBlend showed up flat, or wearing the Top's
colour. Upward and downward facets are now previewed from their own zone, each with its own islands
and scale.

**Also:** the swatch you picked in the Generator is highlighted like a saved one; the colour on the
model and the colour in the panel are computed the same way, so changing a TD updates both instead of
half the screen; **Erase all painting** asks first and says how many objects it will clear; the brush's
**Smart fill angle** and the **section view** slider moved out of the Palette tab, since the brush works
from any tab; deleting a profile now clears its slots across the **whole project**, not just the active
object; and the pattern preview no longer flickers when you sweep the cursor between objects.

### Your painting no longer disappears when you close the painter

A painted Sandwich used to exist **only while the ColorStitch Painter was open**. Close the gizmo and
the object went back to a single flat colour — nothing on the plate said which parts were painted,
with what, or how the effect would land. To find out you reopened the painter, one object at a time.
Plate thumbnails were blind to it too.

Painted objects are now **drawn painted in the normal 3D view**, with the **full weave**: the same
per-line stripes, dithers, hard bands and PathBlend gradients as inside the painter, island by
island, at the real line width, composed against the object's actual base colour and its TD. It is
literally the same code drawing in both places, so the two cannot drift apart.

Two honest differences: the **lighting** is the normal one (or Libre Mode's realistic shading with
its AO and shadows) rather than the painter's own shader, so the same bands sit under a different
light; and **facets too small to form an island** take the slot's flat colour outside, where the
painter falls back to a whole-object weave.

A checkbox, **Keep paint visible outside this gizmo**, sits with the other view aids at the bottom
of the painter. On by default, project-wide.

**If the same object also has MMU painting**, both are now drawn, each on its own faces — Sandwich
first, MMU on top of the faces it owns. That is the precedence the slicer will use once the two
share an object properly. Today the slicer still **ignores** MMU on the faces the Sandwich paints,
so painting both on the same face means the 3D view is now **showing you a conflict instead of
hiding it**: that MMU colour will not print as an MMU top, while its perimeters will. Keep the two
on different faces for now.

---

### Where is this colour applied? — the painter answers in the 3D view

The panel always knew which colour was active. The 3D view never said **where that colour already
is** — and with two similar colours on a plate, that question had no answer but squinting at the mesh.

**Highlight active colour** outlines the painted region of one slot right on the model: the boundary
of the area, so it frames it without covering the weave you are inspecting, plus a faint box per
painted island and a badge with the slot number. The outline is also drawn **through** the object as a
ghost, so a zone facing away still shows without orbiting blind.

**The colours mean something.** The zone chips in the Pro tray and the highlight now share one code:
**green is Top** (the Penultimate is a darker green — it is the layer under the same top surface, not
another thing), **orange is Bottom**. A recipe that paints both zones is obvious at a glance: green
above, orange below. Each badge carries the slot number, a disc in the **colour of the slot** and a
ring and wedge in the **zone colour** — pointing up for a top island, down for a bottom one.

**Hover a swatch — or a row of *In use on this object* — and the highlight follows it** instead of the
active colour: "show me where this one is". A counter next to the checkbox reads `s2 — 137` or `s2 —
not painted here`, because "it is active and I see nothing" has two very different causes.

### Painted colours on assembled objects were invisible in the painter

On an object made of several parts — what **Assemble** produces — painting with a colour that already
had a slot recorded the facets but **not the colour itself** on the other parts. The part sliced
perfectly while the painter showed it **grey**, the eyedropper found no recipe there, and there was
nothing to highlight. Paint slots are **per part**; what identifies a colour across parts is its
profile, and that is now respected everywhere — painting, eyedropper, highlight and preview. Objects
already in that state are **repaired when opened** in the painter: orphan paint recovers its colour
from the sibling part that still had it.

Two more data losses closed on the way. **Save** built a saved palette from Top + Penultimate only, so
a recipe with a **Bottom** zone was saved without it — silently, in the gesture meant to preserve your
work; the Bottom now travels with the colour and counts when Save looks for an identical palette (two
recipes that match on top and differ underneath are different colours). And a pass switched to
**Solid** used to keep its ColorStitch or PathBlend payload inside: the engine sliced Solid, which is
correct, while the recipe still looked like an effect and the preview came out flat. Switching to Solid
now clears the payload, and a pass already in that state offers a **`!CS` / `!PB`** button to restore it.

**Also:** the tool row moved to its own line (the panel had grown too wide); pass rows got a **wider
thickness bar** with easier drag handles, and **`^` `v` reorder** next to the remove button — set apart
from it, since one adjusts and the other destroys; and the palette grid no longer reshuffles itself as
the cursor sweeps between objects.

---

### Snap & Drag grows up — the plate is a floor again, and you can see what it is reading

Snap & Drag (2.3.9) rests a dragged object on whatever real surface is underneath it. Three things
were missing, and all three are in.

![Snap & Drag: an object dragged over the plate lands on it, with the landing zone highlighted under
the hovering object and the sample points that decided the height](docs/images/SnapDrag-Bed.gif)

**The plate counts as a floor again.** New sub-option under **right-click → Snap & Drag: Allow Bed**,
on by default. With it on, an object dragged over empty space lands on the plate, the way any slicer
behaves. Turn it off and you get the previous behaviour: only other objects catch you, and an object
over nothing keeps floating where you dropped it — which is what you want when you are assembling
something in mid-air. The bed never competes with a real object: it is a floor of height zero, and
the highest surface under your footprint always wins.

**You can see where it is going, and what it is measuring.** While you drag, the object now hovers a
little above its landing spot instead of sitting flat on it, so the surface underneath stays visible.
In that gap you get the corner marks of the box where the object will come to rest, the exact zone
being read as the height highlighted on the surface it was found on, and a translucent column
standing between the two. The colour tells you what caught you: cyan for another object, amber for
the plate. The fat dots are the actual sample points the height was taken from — when a large part
refuses to catch a thin rim, that is where you see why: the samples went through the hole, not onto
the rim.

**Dragging several objects at once no longer flattens a stack.** A selection is resolved by stacks
now: anything standing on another member of the same selection travels with it and keeps its exact
height, while anything with nothing of its own underneath falls on its own. Pick up a stack of three
plus a loose box and move them together — the stack lands intact, the loose box drops to the plate,
in the same drag.

Snap & Drag stays a sub-behaviour of **True Objects** and is off until you enable it; with True
Objects off, nothing here is reachable and placement is stock.

---

### MMU paint and Sandwich effects share a surface instead of fighting over it

Paint an MMU colour on a face that also carries a Sandwich effect and, until now, the two disagreed.
The Sandwich won the fill and the MMU won the walls, so the painted region printed with the Sandwich's
colours inside a perimeter of a different colour — a ring of dirt around every MMU shape, on exactly
the face you were trying to make look good.

They now split the surface. **Where you painted MMU, the MMU rules** — that area prints flat, with its
own filament, walls and fill matching. Everywhere else the Sandwich applies exactly as before. One
region, one border, no leftovers.

![A cube assembled from two painted parts and a domed part, both carrying Sandwich effects, with MMU
paint crossing them: each MMU shape prints in its own solid colour while the Sandwich patterns fill
the surface around it](docs/images/Sandwich-MMU-mix.png)

The split is geometric, not a global switch, so it works at any scale: an MMU shape that sits entirely
inside a painted zone takes just that shape; a stripe that crosses the zone and continues past it takes
only the part that overlaps; a shape landing where there was no effect anyway changes nothing. It
applies to top, penultimate and bottom surfaces, on flat and curved faces alike, and on objects
assembled from several painted parts.

Two things worth knowing before you rely on it:

- **In the MMU area you get no Sandwich effect.** That is the trade: the surface prints plain there.
  If you want the effect, do not paint MMU over it.
- **"Perimeter override" reaches past the painted area.** That option clones the walls into every
  Solid pass, and walls are not clipped to the shape you painted — they loop around the whole region,
  so the effect shows up outside your painted zone, MMU areas included. Nothing breaks; it just looks
  confusing in the preview. The Sandwich gizmo now says so under the checkbox.

### See your Sandwich effects while you paint MMU

The MMU painter used to show you a bare object. Everything the Sandwich applies to that same
surface — the colours, the weave, the gradients — was invisible the moment you opened the gizmo,
which is precisely when you need it: you are deciding where MMU paint may go, and the two share the
surface now.

Your Sandwich painting is now **drawn inside the MMU painter**, with the full weave, the same way it
is drawn in the normal 3D view. Where both paints meet, MMU is shown on top — the same precedence
the slicer uses, so what you see is what will print. Everything you have not painted keeps its
normal filament colour, exactly as before.

A checkbox, **Show Sandwich effects**, appears in the MMU panel only on objects that actually carry
Sandwich painting. On by default, project-wide.

---

### Both painters now warn you where the two overlap

Where MMU paint and Sandwich paint land on the same area, MMU wins and that area prints flat, with
no Sandwich effect. That was documented but not visible — you found out by reading the G-code.

Both gizmos now say it, in amber, and only when it is actually happening:

- In the **Sandwich painter**: `⚠ ~34% of this paint is under MMU paint — flat there, no effect`.
- In the **MMU painter**: the same figure, from the other side.

The percentage is deliberately a percentage and not an area in mm². It is computed per original mesh
facet and is an **upper bound** — exact when a facet is fully covered by both paints, generous when
each paint takes a different piece of the same facet. It answers the question you actually have
("how much of my effect am I losing?") without inviting more trust than the number can carry.

Do not confuse it with the **Perimeter override** warning already in the panel: that one says the
effect reaches **outside** what you painted. This one says part of what you painted **will not carry
the effect**. Both can be on at once, and they mean different things.

---

### Gradients: a missing colour, and a mirrored axis

Two independent faults in how a gradient is laid across a surface, both fixed.

**A three-colour gradient could print with only two colours.** With **Line distribution mode** set to
**LaneQuant**, the last colour of the recipe silently disappeared from the G-code — and the other two
were redistributed, so the gradient stopped being a gradient. The mode indexed the colour pattern by
the raw physical lane number, and when a surface produced fewer lanes than the pattern had entries,
the tail of the pattern was simply unreachable. In a gradient the last colour lives in the tail.
Lanes are now normalised against the range actually observed, so the whole pattern is always covered.
**DirCluster** had the identical fault and is fixed the same way.

**The gradient ran backwards on some objects.** Two objects with the same recipe on the same plate
could print their gradients mirrored with respect to each other, and with respect to what the painter
showed. The engine measured the gradient axis from the geometry instead of using the angle you
authored, and near the 0°/180° boundary two nearly parallel surfaces landed on opposite sides. The
engine now uses **the angle you set**, the same one the painter previews — so the painter and the
slicer cannot disagree, and every object on the plate shares one axis. When the angle is left on
auto, the measured axis is canonicalised so it can no longer flip.

**Known limitation, unchanged:** where a surface is split — by MMU paint, or by anything else — each
piece currently restarts the gradient instead of continuing it. The line spacing stays continuous
across the boundary; the colours restart. If you cross a ColorStitch or PathBlend zone with MMU
paint, expect the pattern to begin again on the far side.

---

### Slicing an assembled plate with MMU paint no longer takes forever

Painting MMU across several objects and then assembling them could leave the slice stuck at around
35% for minutes, with the application showing as unresponsive. It always finished eventually, and
cancelling always worked — but it was unusable.

Two causes, and the first one affects everybody:

**A diagnostic was running with its log switched off.** The instrumentation that compares the MMU and
Sandwich footprints of a layer built both footprints in full **before** checking whether its debug
channel was even enabled. Since the feature shipped, every user with Sandwich painting has been
paying for a measurement that was then thrown away. It now checks first and costs nothing when off.

**Merging painted triangles was quadratic.** Both footprints are built by unioning one polygon per
painted triangle. On a single object that is a few hundred; on four assembled objects crossed by an
MMU stroke it is tens of thousands, and the geometry library bogged down. The union is now done in
batches and then merged — the same result, because union is associative, at a fraction of the cost.
The per-layer footprint is also computed once per band instead of once per fill region.

---

### Known issue — Sandwich Stickers are rough

Stickers (the SVG shapes you can drop on a flat top face, carrying a Sandwich recipe) **work and
slice correctly**, but they are the least finished part of the painter and they did not get the
attention the rest of it got this release. If you have not used them, this is why they are tucked
away in the Palette tab.

- **You cannot see a sticker anywhere.** Not in the normal 3D view, not inside the MMU painter, and
  not even inside the Sandwich painter itself unless you are actively editing that particular
  sticker. You place it, it vanishes, and you find out what it did in the preview or the G-code.
- **A sticker overrides everything under it, without saying so** — hand-painted zones *and* MMU
  paint. It is applied last, over whatever survived the other rules, so it takes the surface back
  from both. The warnings added this release do not cover it.
- **Top surfaces only.** Bottom stickers were never implemented.
- The wipe tower does not switch between a painted tower and a sticker one.

Nothing here is new in 2.4.0 — it is longstanding, and it is being written down properly for the
first time. All of it is being addressed together in a **Mask-Painting** section planned for a
future release, where stickers and a drawing tool become one feature instead of a half-hidden one.
Until then, treat stickers as experimental.

---

## Notes

- Both shading fixes are on by default and need no setup. The ambient-occlusion fix applies when Libre
  Mode is enabled (that is where the realistic shading pipeline runs); the smooth-shading preference
  applies everywhere.
- "Smooth shading of imported meshes" can be toggled at any time — the scene reloads on the spot,
  no restart needed.
- The shading work changes nothing in sliced output. The bridging and Sandwich changes above **do**
  affect G-code, but only when you use the features involved: with "Bridging infill extra expansion"
  at its default of 0 and no painted Bottom zone, output is unchanged.
