## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko 2.4.6 — on Snapmaker Orca 2.3.5 — Release Notes

> ⚠️ **Review your generated Gcode before long or production prints, especially if you
> turn on any of the features below.**

**2.4.6 is an incremental release on top of 2.4.5** (see `NEOTKOCM_RELEASE_2_45.md` and earlier
notes for the full feature set).

---

## What's new in 2.4.6

### PathBlend follows your lines on any shape

**Where**: nothing to turn on. Every PathBlend pass, new or already in a project, takes it.

A PathBlend ramp has to climb across the fill lines, because each line carries one height. To find
that direction the engine used to look at where the middle of every line fell and take the direction
those middles spread out in. On a square or a round part that is the same thing as across the lines.
On a long or irregular part it is not.

Measured on a keychain with lines at 45 degrees, the ramp ran 80 degrees away from where it should,
almost along the lines themselves. That is the hard diagonal band some of you saw where a smooth fade
should have been. A long lid was 38 degrees off and an irregular blob 26.

The direction now comes from the lines themselves, turned 90 degrees. Measured after the change on six
test parts, all with lines at 45 degrees: the ramp runs at 135 on every one of them.

### A hole no longer splits one PathBlend line into two heights

When a hole cuts a fill line in two, both pieces now get the same height. Before, each piece was placed
on its own, so the two halves of one line could land at different heights right next to the hole.

Measured on 20 and 60 mm squares with a hole: no line anywhere gets two heights.

### Patterns keep their size and line up between layers

**Where**: Custom pattern, MixedFilament recipe and Textile weave, in the ColorStitch recipe dialog.
Nothing to turn on.

A pattern used to be dealt out line by line in the order the lines were printed. A hole or a bit of
embossed text splits a line in two, the count grows, and everything after it shifts along. Now each
line takes the colour of the spot where it lands, measured from the object. Holes stop shifting the
pattern, a short pattern repeats across the surface instead of being stretched over it once, and the
top layer and the one below it line up line by line when they share the angle.

Measured on a 1122 pattern: the top and the layer below come out with the same colour on the same
lines, and the pattern repeats every 1.43 mm, which is four times the real line spacing.

### One way to build gradients and stripes

The older ways of spreading a gradient or a set of stripes across the lines counted lines, and a
line count is not a size. They are gone, together with the settings that picked between them:

- **Line distribution mode** is removed. Every gradient and pattern now works from the real position
  of each line.
- **Monotonic Line Replan** is removed from the settings and fixed at the value that was giving the
  cleanest corners.
- **Gradient scale** now has two choices: fit the gradient to the surface, or repeat it every so many
  millimetres.
- **Stripes are set in millimetres.** The option to set them as a number of lines is gone.

### Older projects are converted when you open them

A project made before this version is updated as it opens, and a notice tells you how many gradients
and stripe recipes were converted. Gradients keep their shape. Stripes that were set in lines become
millimetres, measured with the default top surface line width, so their size can differ slightly
from what the old version printed. If you need the old behaviour for a project, open it in 2.4.5.

### The Sandwich editor is gone, and a recipe goes where you paint it

**Where**: the ColorStitch Painter. The Sandwich editor button in Quality → Surface ColorStitch is gone.

Up to 2.4.5 there were two places to build a Sandwich. The Painter, where a recipe goes on the faces you
paint, and the Sandwich editor in the print settings, where a recipe went on every top surface of the object
by itself. Two systems doing one job in two different ways was hard to explain and harder to keep working, so
now there is one: a recipe is always a palette entry, and it prints where you paint it.

A project that used the old editor opens with its recipe moved into the palette as **From Sandwich editor**
(one more for each object that had its own), the old setting switched off, and a notice telling you. Paint it
again where you want it; until you do, those surfaces print plain. What you had already painted stays as it was.

A process preset that still carries an old recipe gets it switched off when the app starts, and a notice names
the preset. A preset has no palette to keep the recipe in, so it cannot be moved for you. Save the preset and
the notice goes away.

A few things lived only in that editor and went with it:

- **The ColorStitch Studio.** Its Gradient ramp, pattern and Flat color strips are in the Painter's Generator
  department. **Target + Match**, the search for the recipe closest to a colour you pick, did not come across.
  A browser version of it still runs on the tour's TD page.
- **Topmost only** and **Filament filter**. Painting does both jobs now.
- The **Filament & TD** panel. The TD sliders are in the Painter's Object & TD department, the same four numbers.

### No more "WIP Beta" on the Painter

With the old ColorStitch system retired and the Sandwich editor folded into it, the Painter is the way to do
all of this, and the beta label is off.

### The Painter panel, reorganised

**Where**: the ColorStitch Painter, all four departments.

The panel grew one piece at a time and it showed: three different styles, buttons scattered around,
warnings popping up in the middle of the controls. Everything it did before it still does. It's just
laid out with some sense now.

- The active colour sits on a card at the top, with its state (slot, ready or no colour) and New, Save
  and Duplicate as icons.
- The tools are icons, Sticker included, and so are the department tabs.
- In **Pro**, the top of the panel shows the three zones stacked the way they sit in the part, each pass
  drawn with its real stripes or ramp, next to the colour they make together.
- Each zone is a card. Copy and clear live in its header, and copy opens a small menu with the two
  places it can go.
- A pass picks its type with four icons. Move up, move down and remove sit at the end of its row. An
  angle left on auto reads **auto** in violet, the same violet as the outline in the 3D view.
- **Add pass** is a dashed button at the end of the zone, and it goes away once the zone has three passes.
- In **Palette**, groups are chips with their count and Save all sits next to them. Each swatch shows its
  effect (stripes for ColorStitch, a ramp for PathBlend), a notch if it isn't saved yet and a dot if it's
  used on this object.
- **TD** lives only in Object & TD now, one row per filament.
- In **Generator** the three strips have the same size and show every swatch. No more scrolling sideways.
- In **Brush & view** the sliders carry their value inside. Ctrl+click one to type a number.
- All the warnings sit together at the bottom of the panel.

### Fold away the parts of the settings you are not using (LibreMode)

**Where**: the Process tab, both Global and Objects, with LibreMode on.

Quality alone has nine sections and most days you only touch one. Click the header of any section
(Walls, Infill, Line width, whatever) and it folds; click it again and it comes back. A folded section
is just its title line, so you can see at a glance what is open. Alt+click a header folds or unfolds
every section on that page at once.

What you fold stays folded: between pages, between presets, and after closing Orca. Global and Objects
share the same state, so a section you fold in one is folded in the other. If a folded section has
something changed inside it, a small dot appears at the end of its title line, so nothing hides from you.

Searching for a setting that lives in a folded section opens it, shows you the field, and folds it back
when you leave the page.

With LibreMode off everything shows as before, and whatever you had folded is waiting for you when you
turn it back on.

### The NeoArachne preview has moved

The wall path preview used to sit inline under *Wall generator*. It took up half the page, and it only
ever lit up when the wall generator was NeoArachne, so most of the time it was a blank panel pushing
everything else down. It is gone from that page.

The same panel is now the path viewer inside **NeoStroke → Advanced options…**, showing NeoStroke's
paths instead. Nothing was lost: it is the same code, with the same layer slider, speed, build mode,
zoom and **Dump** button.

On the way it also picked up the fix it needed: it changes height when it appears and again when its
drawing is ready, and it now asks the window for the space that takes instead of painting over
whatever is underneath.

### Fixed: the stripe bar in Pro showed a gradient split in half

The bar under each ColorStitch pass drew a gradient cut in two, with its ends swapped. The print and the 3D
view were fine. The bar now reads the pattern from one end to the other, in the same direction as the slice.

### Fixed: "copy to" now copies the whole ColorStitch recipe

In the Painter's Pro department, copying a zone to Penultimate (and back to Top) used to bring over a ColorStitch
pass with only its two colours and the angle. Whether it was stripes or a gradient, the band widths, the
gradient scale, the transition shape, invert, repetitions and colours 3 and 4 were all lost, so the copy came
out as a plain two colour dither. Now the whole recipe travels. Solid passes and copies between Top and Bottom
were already fine. A pass you copied before this fix keeps the broken recipe, so copy it again.

### The painted preview in Prepare now matches the Gcode

The stripes and gradients you see on the model, inside the Painter and outside it, are now placed the same
way the slicer places them.

- **Bands start where they print.** Stripes in mm and patterns used to start counting from the edge of each
  painted area, so the first band could sit a little off. They now count from the same point as the slicer.
- **Rotated and scaled parts.** Turning a part on the plate used to turn the preview stripes with it, while
  the print keeps them fixed to the bed. Scaling changed the stripe size on screen. Both now match the print.
  Scaling a part also refreshes the preview outside the Painter right away; before, you had to apply the
  profile again.
- **Gradients with a real size.** A gradient set to repeat every so many mm, or with repetitions, now shows
  that on the model. Before, the preview stretched it once over the whole area.
- **PathBlend** uses the same lanes, size and start and end zones as the print, and runs the ramp in the
  same direction. Between 90 and 180 degrees some ramps may look flipped compared to the previous build; the
  new one is what prints.
- **Top and Penultimate together.** When both layers carry an effect, each at its own angle (say 45 and
  135), the preview now shows both, one seen through the other with the TD of each filament, the same
  physics as the colour swatch. As a result a painted top now looks like the colour it prints, where
  before it showed the pure filament colours.

### NeoArachne: Classic walls with a single line inside, for fine lettering (experimental)

With **Wall generator = NeoArachne** and **NA — inner walls source = Classic** (now the default), the walls
keep a constant width and only go where they fit. Before each inner wall the slicer checks what is left
inside. If it fits in one line no wider than the maximum, it becomes **one line along the middle of the gap,
with a width that follows the gap**, the way Simplify3D prints single extrusions. That replaces the short
cross-hatched infill strokes and the scraps of gap fill that used to fill narrow pockets in small letters,
so there are fewer paths and fewer starts and stops.

- The line is printed with the walls of its letter. With walls printed from the inside out it goes first,
  then the inner walls, then the outer one, so the nozzle no longer comes back inside after the outer wall.
- Its ends taper down to the minimum width at the tip of a pocket instead of stopping short.
- Branches of the line are joined where they meet, so a Y shape prints as two lines.

New settings, shown when inner walls are Classic:

- **NA — single-line fill**: on by default. Off gives plain Classic walls and Classic gap fill.
- **NA — single-line min width**: the thinnest the line gets. Where the gap is narrower, the line is still
  printed at this width, so the tips of a pocket get filled.
- **NA — single-line max width**: the widest the line gets. It is never split in two. The limits are
  wide on purpose (up to 1000 %); Orca still slows wide lines down to the filament's max volumetric speed.
- **NA — single-line min length**: shorter lines are dropped.
- **NA — single-line: slivers below**: a gap that never gets this wide is a crack between two passes of
  the same wall, usually inside the thin tail of a letter, and is left empty. 0 fills every crack.

**Where it helps and where it does not yet.** It is meant for thin strokes, where it is cleaner than Classic.
On bold lettering the gap inside the outline can be 1 to 1.5 mm, and a single line cannot fill that
well: with a high minimum it leaves gaps, with a low one the line gets very wide. For bold text keep using
Classic or Arachne for now. A version that lays several lines along each stroke is being planned.

### NeoStroke: walls planned as strokes, for lettering (debug mode only)

**Where**: Quality → Wall generator → **NeoStroke**. Needs Libre Mode **and** the NeoStroke switch in
Preferences.

> ⚠️ **This one is not ready to print with, and it is published anyway.** NeoStroke is here so people
> can look at it, read its toolpaths and help find where it breaks. Prints with it are not stable. It
> takes a real understanding of how the wall engine works to get a usable result, and checking the
> G-code before you print is part of using it. If that is not what you are after, leave it alone.

Every wall generator builds a wall as a loop and then works inwards, loop after loop. On a letter
1 mm wide that runs out of room after the outer wall, and what is left over goes to gap fill, which
breaks it into short strokes with a start and a stop on each one. That is where the holes in small
raised lettering come from.

NeoStroke keeps the outer wall from Classic and drops the loops. It takes the middle line of the
shape and lays beads along each stroke of the letter, as many as fit side by side, with the width
following the room there is. The stem of a T gets one long path down the stem instead of a ring
around it.

Three controls sit under the dropdown: the thinnest a bead may be, the widest it may ever get, and
whether the turn of a U is closed. Everything else is behind **Advanced options…**, which opens the
remaining fifteen next to a live drawing of the paths, so a change can be judged without slicing.
The defaults are the ones from the best test plate so far.

**How to unlock it.** Turn on Libre Mode, then tick **Enable NeoStroke wall generator (unstable)** in
Preferences, right under the Libre Mode switch. It applies straight away. If you prefer, starting
Orca with `ORCA_DEBUG_NEOSTROKE=1` does the same thing. Pick NeoStroke without both keys and you get
a warning explaining why, and the offer to put the setting back. The engine checks too, so a project
file carrying the setting falls back to the normal path rather than printing with an engine you did
not unlock.

**The path viewer now draws what lands, not what is labelled.** Each bead is drawn at the width its
flow implies. Two settings could change the material without changing the stated width, so raising
them used to do nothing on screen while changing the print. The dump prints both numbers, `w=` and
`flow_w=`, so it can still be checked against the G-code.

**The layer slider is a layer number now.** It reads `Layer 14/15 · Z 2.85 mm`, and that Z is the
same number the G-code writes in its `;Z:` line, so a layer on screen can be put next to the same
layer in the G-code without any arithmetic. It only offers layers whose slicing plane falls inside
the object. Before, the top of the slider could ask for a layer the object does not reach, and the
preview quietly showed the middle of the object instead, which is the fullest cross section there
is: the last layer looked solid on screen while the G-code had holes in it.

**Picking which islands to preview.** The viewer plans at most 8 separate islands at a time, and a
plate of letters passes that immediately. When it does, it now draws the islands and lets you click
the ones you want instead of refusing the slice.

**What is measured.** On thin strokes it covers ground the other engines leave empty, with fewer
separate paths. A letter comes out clean when roughly 95% of its interior is covered by a real bead,
and comes out with holes below about 60%. That number is written to
`/tmp/neotko_logs/neostroke.log` on every layer, so it can be read without printing.

**What is not settled.** The paths are continuous on screen, but the G-code still breaks into more
short moves than it should in places, and a break in the flow shows on the part. It has not been
through enough shapes and nozzle sizes to say what it does outside small lettering. For bold text,
Classic or Arachne is still the answer.

---

## Notes

- **This changes the Gcode of existing PathBlend projects, on purpose.** Square and round parts barely
  move, because the old measure was already close there. Long and irregular parts change the most,
  and those are the ones that were coming out wrong.
- **For a pattern to line up between the top and the layer below, give both the same number of
  walls.** Turn off *Only one wall on top surfaces*, or match them another way. The slicer spaces the
  lines of each solid region so that a whole number of them fits, and a different number of walls
  gives a different region, so the lines land in different places.
- The gradient editor (ADV) works as before. Floor, ramp end and the start and end zones mean exactly
  what they meant.
- **The painted preview in Prepare is now in step with the Gcode** (see above). Two small limits remain: copies of
  one object rotated differently all show the stripes of the first copy, and a third effect pass in the same
  stack is drawn as a flat colour. RealColor is still the view to check after slicing.

---

## Known issue, still open

**The angle a zone reports does not always match what gets sliced.** Carried over from 2.4.4, where
it is described in full. With the angle left on auto the painter had to pick one direction to draw
while the slicer flips between two. The default angle is a real 45 rather than auto, and zones still
on auto are called out with a violet outline, which covers most of what people were running into.
Cases where the number shown and the printed result disagree are still being tracked.

**Until it is fixed, slice and look at the Gcode preview in RealColor.** RealColor draws the effect
from the toolpaths that were actually generated, so for anything to do with angles and gradients it
is the view to trust.
