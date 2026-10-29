## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko 2.4.9 — on Snapmaker Orca 2.4.0 — Release Notes

> ⚠️ **Review your generated Gcode before long or production prints, especially if you
> turn on any of the features below.**

**2.4.9 is an incremental release on top of 2.4.8** (see `NEOTKOCM_RELEASE_2_48.md` and earlier
notes for the full feature set).

---

## What's new in 2.4.9

### Print order (Experimental, LibreMode)

**Where**: the new **Print order** button in the left toolbar (three numbered dots joined by arrows). Needs
LibreMode on. Works with **By layer** printing.

Orca picks the order of the objects in each layer by itself, by distance, and it can't be changed. Now you can
set it:

- **Click an object**: it takes the next number (1, 2, 3...). **Right click** or **Option + click** puts it back
  to **Free**.
- Every layer prints the numbered objects in that order. Objects that already finished, or haven't started yet,
  are skipped and the rest keep their order. Free objects go after the numbered ones, the way Orca does it.
- **Several filaments**: each filament is printed whole, in your order, and then the next one. Which filament
  goes first in a layer is still Orca's choice (the one that purges least).
- In the 3D view every object shows its number (or **F**) and arrows on the plate go 1 → 2 → 3, so you can read
  the route before slicing.

**Inside an object** (an assembly of several parts): click a numbered assembly again, or use the pencil in its
row, and you are inside it. Its parts get **2.1, 2.2...** and inside the object's turn they print in that order.
**< Back to objects**, or a click outside, takes you out.

- Parts that touch each other in a layer are one piece for the slicer, so in that layer they print together, in
  the turn of the first of them.
- Parts on different filaments already print in their own filament pass.

**The panel**: the list in order, with ▲ ▼ to move a row, the eraser to set it Free and **+** to give the next
number. Hovering a row lights that object or part in the 3D view, and hovering a piece in the 3D view lights its
row. The **lock** at the top stops clicks on the scene from changing anything (no numbering, no leaving an
assembly), handy on a crowded plate; the list keeps working. Every change can be undone with Ctrl+Z.

Changing the order only redoes the G-code, nothing is sliced again. The numbers are saved in the 3mf. With
LibreMode off, or with every object Free, the G-code is the same as before.

### RealColor looks like the real print now

**Where**: the **RealColor** view of the preview.

RealColor now uses the same light model as RealView (our raytraced G-code renderer). Each filament lets through the
colours it doesn't absorb, so a thin yellow over blue turns greenish and a deep colour gets more saturated, the way it
happens on the printed part. Light also spreads a little inside the plastic, and the walls seen at an angle look
thicker, like they do in real life.

**Quality** has two choices, and **Draft** is the default:

- **Draft**: about a second per view. It only looks a few layers deep, so a thick translucent area can look a bit
  off, but the colours are the ones you'll get. Good to find angles and check colours.
- **Full**: looks all the way through the plastic. Very demanding: with a high TD it can take 15–20 s every time
  the view changes.

**Photo mode** always renders at full quality. **Wipe tower** and **Supports** show or hide those in the view.

![Top: the slug before and with the RealView model. Bottom: RealColor with supports, and with supports hidden](docs/images/RealColor-RealView-model.png)

### Sticker colours: the prediction matches what prints

**Where**: Painter → **Stickers** (the **Result** square and the auto recipes), **Pro**, **Palette**.

The colour a recipe predicts now uses the same TD and the same layer thickness as RealColor and RealView. Before,
the prediction saw each thin pass as much more opaque than it is, so on a white part it picked plain filament
colours and never tried mixes. Now a rainbow sticker on white gets its pinks, oranges and greens, and on a coloured
part the base shows through the way it does on the print.

After updating, re-apply your stickers or press **Recompute all** so the recipes are found again with the new
prediction.

With only two thin layers on top (Top and Penultimate) there's a limit to how far a colour can move away from the
part's colour, especially with a high TD. A thin orange over white will look pale on the print too.

### Painter: Shape mask with curves

<!-- PENDIENTE AL HACER PUSH: mandar a Radu y a Snapmaker. Rama `painter-shapes` (RADU_FORK, sobre
align-construct) y parches + README en docs/WIP/PAINTER_SHAPES_COMMIT/. Rama `painter-shapes` también en
SNAPMAKER_ORCA (2.4.0 no tenía Pro Mode: lleva el 80c0eb65b de Radu + los 2 nuevos). Ninguna subida aún. Incluye el arreglo de "negative spacing" con Outer wall = 0. -->

**Where**: Painter → **Pro Mode** → **Shape mask** (it replaces Polygon mask).

Draw a shape on the part and paint exactly what's inside it, with smooth curves instead of straight segments:

- **Click** places a corner point, **click and drag** places a curved one and pulls out its handles, like in a
  vector drawing program.
- Click the first point to close the shape. It doesn't paint yet: it stays on screen, filled with the colour of the
  selected filament, so you can still move points and handles, or click on the outline to add a point.
  **Alt + click** turns a point from corner to curve and back, **Alt + drag** moves one handle on its own.
- **Apply** paints it, **Erase** removes the paint inside, **Cancel** (or right click) drops it. Each Apply is one
  Ctrl+Z.
- **Keep shape after Apply** leaves it on screen, so you can paint the same shape again with another filament or
  nudge it and paint again.
- **Lock view** (on by default) freezes the camera while you draw. The shape lives on the screen, so moving the
  camera would make it miss the part; with the lock off, moving the camera drops an unfinished shape.

The Rectangle and Shape masks also stopped painting through the part: only what you can see gets painted, not
the inside of a hole or a wall hidden behind another bit of the model.

![Shape mask on a Godzilla foot: the curved shape with its handles, closed and ready to Apply](docs/images/Painter-Shape-mask-curves.webp)

![The same foot sliced: two colours painted with one shape, the second one with Keep shape after Apply](docs/images/Painter-Shape-mask-sliced.png)

### Painter: Vector mode and SVG decals

<!-- PENDIENTE: la nota "not tested on
Windows" en el commit para Snapmaker y FS Radu. Estado y detalle: docs/WIP/SVG_DECALS_HANDOFF_27oct2026.md -->

**Where**: the multi-material Painter now has two modes at the top of its panel, **Brush** (key **B**) and
**Vector** (key **V**). Brush is the painter you know. Vector is for shapes and SVG files, placed and painted in
one go.

![Three SVG decals on a skull in Vector mode: a clown face, the Decals list and the panel](docs/images/Decals-clown-anim.webp)

**Load an SVG and paint it in its own colours**

- **Load SVG...** reads the file with the same engine as the Stickers: every colour of the SVG gets its own row,
  and each row picks the filament (or Mixed Filament) it paints with. The eye on a row leaves that colour out,
  so whatever is under it stays as it was.
- Drag inside its frame to move it, and use **Scale** and **Rotate**. **Deform corners** gives you four handles to
  bend it in perspective, like a sticker pressed onto a slanted face. Straight lines stay straight.
- Painting is fast: a multicolour SVG on a model of a million triangles applies in under a second.

![Two clown SVGs and a sticker laid on a head with On surface, before applying: each SVG outlined in blue](docs/images/Decals-clown-preview.webp)

![The same three decals applied: the clown face painted on the head in four filaments](docs/images/Decals-clown-applied.webp)

**Parts: pick any piece of the SVG**

- **Pick parts**, then click a piece of the SVG to select it (Shift + click adds or removes one). The selection
  is outlined in orange.
- The **Selected** row gives those pieces their own filament (or **Colour** to follow their colour row) and an
  eye to switch them off. A piece that is off lets through what the SVG has under it. **Reset** puts them back.
- The **Part list** shows the parts you changed or selected, or all of them. Hovering a row outlines that part.

**Decals: the SVG stays where you put it**

- **Pin to view** keeps the SVG as a **decal** of the object, seen from that camera. It's saved in the 3mf and
  follows the object when you move it.
- Every decal is a row in the **Decals** list: show or hide it, lock it, change the order, and **Apply**,
  **Erase**, **Edit** or **Delete** it. **Apply all** paints them all in order, the ones lower in the list on top.
- **Projection**: **Perspective** projects from the eye you pinned it from, like the camera saw it. **Parallel**
  keeps the same size at any depth.
- **Depth**: **First surface** paints only what the rays meet first. **Through** paints every surface they cross,
  folds and hidden faces too (and the back of the object).
- **On surface** shows every decal laid on the model, exactly where it will paint. Turn it off to see them as
  cards floating around the object.

![The painted skull with the clown decal hidden in the Decals list](docs/images/Decals-skull-applied.webp)

![The clown decal shown with On surface: laid on the skull exactly where it will paint, outlined in blue](docs/images/Decals-skull-preview.webp)

**Move, turn and scale on the model**

- **Move on object**: drag on the model and the decal slides over it, wrapping around the object. You see live
  where it lands.
- **Turn** and **Size**, or with Move on object on: **mouse wheel** to turn, **Shift + wheel** to scale.

**Mirror**

- **Left/Right**, **Front/Back** and **Top/Bottom** paint mirrored copies through the centre of the object. Two
  of them give three copies. The copies follow the decal and show on the model before you apply.
- **Keep readable** places the copies on the other side without flipping them, for text and logos.
- **Mirror centre**: **Pick on object** to set where the mirror planes cross (an orange mark), for a model that
  isn't centred in its box.

![Mirror Left/Right: one ornament SVG placed on the skull, its mirrored copy shown in blue on the other side](docs/images/Decals-henna-mirror.webp)

![The ornament painted twice, mirrored, from one decal](docs/images/Decals-henna-result.webp)

**Draw your own shapes**

- **Draw shape**, right under Brush and Vector, then click on the part: the same curved Shape mask as in Pro
  Mode. **Close shape** and **Apply** it, or **Save as decal** to keep it like an SVG.
- A shape saved as a decal keeps its points: **Edit** it, then **Edit points** to move its points and handles
  again, and **Save** to update the same decal.
- Until you press Draw shape, dragging on the model turns the camera as usual.

**Mirror brush**

- In **Brush** mode, **Left/Right**, **Front/Back** and **Top/Bottom** repeat every brush stroke on the
  mirrored side, through the same orange centre as the decal mirrors. Dashed rings show where the copies land.
  Works with the Circle and Sphere brushes.

**More control over the SVG**

- **Width** and **Height** stretch the SVG along one axis only.
- Small SVGs enlarged a lot now keep smooth curves.

In Brush mode, **Show decals** keeps them on screen as a reference while you paint by hand.

### Cut keeps the painting

**Where**: the **Cut** tool (plane, connectors, dowels, tongue and groove, Cut to parts).

Cutting a painted model used to throw all the painting away. Now every piece keeps it: colour, fuzzy skin,
support and seam painting. The new face of the cut comes out unpainted, with the filament of the part.

- It's fast: a painted skull of 1 million triangles cuts in under half a second of painting work, because
  everything the plane doesn't touch is copied as it is.
- The pink/blue part view (right click while cutting) keeps it too.
- A **Keeping the painting...** notice shows while it works, as in Booleans and Split.
- On Windows, **Fix model** (also the one offered right after a cut) keeps the painting now. ⚠️ Not tested:
  I only have a Mac.

### Remesh (voxels): repair, offset and hollow, keeping the painting

**Where**: **Align & Construct**, section **Remesh (voxels)**. Pick the object (or part) as #1.

Orca's **Fix model** only exists on Windows, and the online fixers throw the painting away (and most of them
are paid). Remesh rebuilds #1 from voxels as one closed solid: holes, loose open shells and flipped faces are
gone, and with **Keep painting** on, the new mesh is painted like the old one. Everything happens on the
voxels, with no mesh boolean, so it works on meshes that are badly broken.

- **Quality**: **Draft** 0.5 mm, **Normal** 0.2, **High** 0.1, **Extreme** 0.05, or type any **Voxel** size
  from 0.02 to 2 mm. Next to it you see a rough estimate of the triangles and memory before you press
  anything. Smaller voxels keep more detail; details thinner than one voxel are lost and sharp edges round off
  by half a voxel. Extreme on a big object takes minutes and gigabytes of memory, and it warns you.
- **Offset**: moves the whole surface. + makes it fatter (thin walls get printable), - makes it thinner (room
  for a fit). 0 just repairs. Big offsets blur the painting a bit.
- **Hollow**: takes the inside away, leaving walls of **Wall** mm. **Smooth** fills the bumps of the inside,
  so it comes out like a bowl and not a copy of every tooth (it fills what is smaller than about twice its
  value). Without a picked face the hollow stays closed inside: the panel says so.
  - **Open through the picked face**, with three ways to open it:
    - **Face**: the flat face you picked, less one wall all round, so a rim stays.
    - **Angle**: spreads from where you clicked to every connected bit of skin that faces less than
      **Angle** degrees away from it, for curved skins. Flat areas open wide, tight curves stop soon, and
      on a rough mesh the edge comes out organic. The green on the model shows what it takes as you change the angle.
    - **Circle**: a round hole of **Diameter** where you clicked, straight into the surface, like a drain hole.
  - **Fast smooth** (on by default): the inside is built on coarser voxels, since it is smoothed anyway. It
    looks the same and a big Smooth takes seconds instead of minutes (High, Smooth 10: 121 s down to 16 s).
    The outside keeps the chosen Quality.
- **Result**: **Replace** #1, a **New object** (#1 stays as it was), or **Neg. part**: hollow without touching
  #1 at all. The inside, with its opening, becomes a negative part of #1's object and the slicer takes it away,
  so you can move it, cut it or delete it to undo. Happy with it? **Mesh Boolean** makes it final and keeps the
  painting.
- **Advanced** (folded, the defaults work):
  - **Max triangles**: voxels give millions of triangles; the result is simplified down to this.
  - **Adaptive**: bigger triangles where the surface is flat, small ones only on curves and details. At 0.1 a
    skull in Draft goes from 570k to 59k triangles, looks the same and slices faster. 0 makes them all the
    voxel size; very high values soften fine detail.
  - **Remove loose bits**: deletes tiny separate pieces left over from hidden geometry inside the model, so the
    result is one clean solid and Orca stops flagging it.

While it works, a window lists every step of the run (done ones ticked, the current one marked, the rest
waiting) with the elapsed time and **Cancel**, which stops within a few seconds.

**How long**: on a painted skull of 850k triangles, Draft takes about 4 s, Normal with a hollow about 23 s and
High with Smooth 10 about 24 s, painting included. Carrying the painting over takes a few seconds.

![Remesh on the painted skull: the panel in Draft](docs/images/Remesh-panel.webp)

![The same skull after Remesh in Draft with Keep painting: one closed solid, painting kept](docs/images/Remesh-keep-painting.webp)

![Close-up: Draft (0.5 mm voxels) against the original. A finer quality keeps the detail](docs/images/Remesh-closeup-draft.webp)

![Close-up of the original](docs/images/Remesh-closeup-original.webp)

![A hollow skull bowl, opened at a height, with its painting](docs/images/Remesh-hollow-bowl.webp)

**A piggy bank out of anything, in two or three clicks**: pick a face, tick **Hollow** and **Open through the picked face**, choose **Circle** and a **Diameter**, and press Remesh. This skull came out hollow, with a 2 mm wall, a 40 mm round hole in the back and its painting kept. Coins go in, and come out, through the same hole.

![The Remesh panel with Hollow, Open through the picked face and Circle 40 mm](docs/images/Remesh-circle-panel.webp)

![The skull cut open: hollow inside, 2 mm wall, a round hole in the back and the painting kept](docs/images/Remesh-circle-skull.webp)

**A real repair**: this painted skull bowl came broken from a boolean. An eyeball that lived inside the
model pokes out through the bowl. Remesh in **High** (0.1 mm), **Adaptive** 0.1, Hollow off, **Replace**: the
surface is rebuilt in about 10 s of voxel work and the painting is carried over. The ball is gone, only a
thin trace of it is left on the wall (with **Remove loose bits**, the loose leftovers go too).

![Before: the painted bowl with an eyeball poking through the inside](docs/images/Remesh-fix-before.webp)

![The settings: High, Adaptive 0.10, Replace, Keep painting](docs/images/Remesh-fix-panel.webp)

![After Remesh: the surface rebuilt, the painting kept](docs/images/Remesh-fix-after.webp)

**Non-destructive hollow**: **Neg. part** gives the inside as a negative part in 2 s. Slice to check it, then
**Mesh Boolean** (Difference) makes it final: the skull keeps its painting.

![Mesh Boolean with the negative part as the tool](docs/images/Remesh-negpart-boolean.webp)

![The result: an open bowl, painting kept](docs/images/Remesh-negpart-result.webp)

**Sliced**: the painting of the remeshed skull, in the preview.

![Painting of the remeshed skull, sliced](docs/images/Remesh-sliced-paint.webp)

**Fast smooth** on and off, High, Wall 2, Smooth 10: the same bowl, 16 s against 121 s of voxel work.

![Fast smooth on](docs/images/Remesh-fast-smooth-on.webp)

![Fast smooth off](docs/images/Remesh-fast-smooth-off.webp)

---

## Fixes in 2.4.9

### TD means millimetres everywhere

The TD you type (Object & TD, Filament finish) is now millimetres everywhere, like HueForge and RealView. Some
places read it as "layers", so TD 5 with 0.2 mm layers acted like 1 mm: five times more opaque than the filament
really is, and different at every layer height. This touched RealColor, the sticker and recipe colours, the painted
view on the part, MixedFilament in sandwich mode and Precision ALH. Your TD values don't change; they just mean what
they say now.

### RealColor: the colours of an opened G-code

Opening a G-code file by itself showed every tool in orange, or with the colours of the last project. Now it uses
the colours written in the G-code. The **Filament finish** list only shows the tools that G-code uses.

### Parts of an object: walls and infill no longer split between two turns

When a plate with holes holds other parts inside them (for example letters sitting in a keychain plate), Orca
could put the plate's walls in the group of one of the letters and its infill in its own. While every piece
printed back to back nobody noticed; with your own order it showed as the plate's infill printing first and its
walls later. Now each line goes to the piece that really contains it. This is only active when parts are
numbered in Print order.

### Object list: deleting several parts at once no longer crashes

Selecting several parts of an object in the object list and pressing Backspace could close the program. It
happened when the selection reached the last parts of the object. Now it deletes them as expected, and if every
part of the object is selected, the whole object goes. This bug comes from Orca itself.

### Multi-material paint with Outer wall = 0 no longer stops the slice

An object whose **Outer wall** line width was set to 0 ("use the default") and had any colour painting stopped
slicing with "Flow::spacing() produced negative spacing. Did you set some extrusion width too small?". One place
read that 0 as a real 0 mm width. Now 0 means the default there too. This bug comes from Orca itself.

## Reference commits

The new painter (Vector mode, SVG decals, Mirror brush), Cut and Windows repair keeping the painting, and the
Remesh section of Align & Construct are also up as clean commits for other forks to take. Same changes, without
the Neotko-only parts (no Sandwich, no ColorStitch, no debug logging). They go on top of the 2.4.8 ones (booleans
and Split keep the painting, Align & Construct). Syntax-checked on macOS; **not tested on Windows**.

- Snapmaker Orca 2.4.0: branch [neotko-2_49-sm240](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/tree/neotko-2_49-sm240)
- FullSpectrum: branch [neotko-2_49-fullspectrum](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/tree/neotko-2_49-fullspectrum)

| Change | Snapmaker Orca 2.4.0 | FullSpectrum |
|---|---|---|
| Painter Pro Mode: precision, surface-only paint, extra walls, masks | [`239eb0fca`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/239eb0fca9cb7ffc4761d4e709945dd545e9a741) | (Radu's own, already in FullSpectrum) |
| Painter Pro Mode: shape mask with curves, Lock view and Apply | [`41e00be69`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/41e00be691e3fd075dfeb4aed4385b7f2e478c19) | [`4c1e9a48f`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/4c1e9a48f7dcb413363ac89b09878ac98f3ecef6) |
| fix: negative spacing abort with multi-material paint and Outer wall = 0 | [`59f7523d2`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/59f7523d2e769262c98b6c4083e4b2a118b2ba3a) | [`487654586`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/487654586c22befeff3ff49c7fbd5d573e5f89ea) |
| Cut tool keeps the painting on every piece | [`8f5eae374`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/8f5eae374cf5a1586ad42fc53490e10f2b20354a) | [`216fc6146`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/216fc61460d883632d905349548ca8f68d579a3e) |
| Windows "Fix model" keeps the painting (UNTESTED) | [`3dbc09ef5`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/3dbc09ef56f84ab490868684688b57200a21841a) | [`4f6501795`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/4f65017953ddd4574c5ab62ad07d78121e362e7c) |
| Align & Construct: Remesh (voxels), with Hollow | [`32d087066`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/32d0870665f7fc2fff7809376a78648fcc5f3083) | [`307f3c9cc`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/307f3c9cc6f353a2da8758d57db9d741831eb966) |
| SvgPaint: SVG zones and shapes for painting | [`7cfd04159`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/7cfd041594c7d3d0c2fcfdee2e5d4088dbfe37fd) | [`374ee9010`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/374ee9010916a137ed276c8ab20af97f727c264f) |
| Paint decals: model, 3mf, and a fast polygon-set cursor | [`e8a531341`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/e8a5313413da660181d4d820febed7bd9b53f98b) | [`cbe0278dd`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/cbe0278dd2b63c7de6ff2939e9889de716f67357) |
| Multi-material painter: Vector mode, SVG decals, Mirror brush | [`5edb3094e`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/5edb3094efc82ebe1e63e1ad623564254b9820d5) | [`2706d0682`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/2706d068276132cbef3ac73f466170d31016e08a) |
| fix: Windows build, rename `far` (a Windows macro) in the decal hit test | [`8eb7b4a43`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/8eb7b4a43e61f0db6d07793a7f78fa7396781819) | [`a0d4cd50f`](https://github.com/sebsucmor-alt/OrcaFS-NeotkoCM/commit/a0d4cd50fb18b67e32c24a5d12e2a7f11add8053) |
