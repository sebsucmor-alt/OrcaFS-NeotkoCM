## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko 2.4.5 — on Snapmaker Orca 2.3.5 — Release Notes

> ⚠️ **Review your generated Gcode before long or production prints, especially if you
> turn on any of the features below.**

**2.4.5 is an incremental release on top of 2.4.4** (see `NEOTKOCM_RELEASE_2_44.md` and earlier
notes for the full feature set). Everything is still something you turn on: at defaults the build behaves like
stock Snapmaker Orca.

---

## What's new in 2.4.5

### Hotends that have finished their work now switch off

**Where**: the new **Bed and Nozzle Extras** button in the sidebar, right under Bed type. Off by
default.

On a tool changer a hotend that has done its part stays hot until the print ends. Print something
where three tools work through the first hour of a twenty hour job, and those three sit there
holding temperature for the remaining nineteen, heating nothing and printing nothing.

With this on, once the Gcode shows a tool has no extrusion left, its standby command is set to 0 °C
and it stops heating for the rest of the job.

The reason that matters day to day is **the filament**. The length parked inside a hot nozzle keeps
cooking for every hour it waits, and that is where the scorched first centimetres after a long idle
come from. The heater is a wear part as well, and hours spent holding a temperature nobody asked for
are hours off its life.

Then there is the power. On one machine it is a small number, easy to shrug off. Multiply it by every
tool changer in the world running long multicolour jobs and it stops being small. What settles it
either way is that those watts buy nothing: a hotend that will not print again for hours contributes
nothing to the part by staying hot, and switching it off costs no print time at all.

**A tool is only switched off when nothing later in the file uses it again**, so this never makes a
print wait for a reheat. The decision is taken on the finished Gcode rather than on the plan, so the
tool visits added by the wipe tower are counted too. Tools that are never used at all are
already left cold by the printer's own start Gcode and are not affected.

### Extra Energy Save mode

**Where**: the second checkbox in the same dialog, under the first. Also off by default, and it only
does anything with the one above turned on.

The rule above waits until a tool is finished for good. This one also switches a tool off **while it
waits**. A tool that prints on layer 2 and is not needed again until layer 200 holds idle temperature
for that entire stretch, which on a tall plate is hours.

**It costs no print time**, and that comes out of how the mechanism works rather than out of a
promise. Orca already schedules the preheat of the next tool ahead of the toolchange, and it already
removes an earlier cooldown when it finds one inside that window. So a tool coming back too soon to
be worth cooling simply keeps its heat, the shutdown is taken back out, and nothing changes. Past
that window the shutdown stands, and preheating starts far enough ahead that the tool is at
temperature when it is picked up: on the U1 that is 30 seconds of lead against a ramp measured at 22,
so **eight seconds of margin**. The print measured below confirms it holds in practice.

If you want more margin than those eight seconds, the knob is **Preheat time** in Print settings, and
raising it widens this rule with it. They are the same number.

**It needs Ooze prevention enabled** in Print settings, because what gets rewritten is the standby
temperature command that setting emits. The Snapmaker U1 profiles ship with it on. If your profile
has it off, the dialog says so before you tick anything, and the slice tells you again rather than
letting you believe hotends are being switched off when they are not.

### Measured on a real print

Checked on the **Rick and Morty planters** that Adult Swim gives away, which is a good test because
it is an ordinary print rather than something built to prove a point: four colours used the way
people actually use them, and a bit over ten hours on the plate.
[Model on Cults3D](https://cults3d.com/es/modelo-3d/arte/rick-and-morty-planters-adult-swim). One of
its colours was dropped to bring it down to four, which is what the U1 has.

| | |
|---|---|
| Print time | 10.2 h, 4 colours |
| Tools switched off | 860 times |
| ...of those, for good | 3 |
| Spread across tools | 274 / 122 / 340 / 124 |
| Gap before the tool came back | 69 % under 2 min, 31 % between 2 and 5 min |
| **T1 went cold with** | **5.5 h of printing still to go** |
| T3 went cold with | 1.2 h still to go |
| Time added to the print | none |

The three marked "for good" are the ones the feature exists for. **T1 finishes its part before the
halfway mark, and would have spent the remaining five and a half hours at idle temperature**, which
is the exact complaint that started this, and it is found without anyone configuring anything.

The rest are parks handled by Extra Energy Save. Worth knowing how they behave: of every cooldown
emitted on this print, **128 were taken back out by the preheat** because the tool was needed again
too soon to be worth it, and that number is **identical with the feature on and off**. Turning this on
does not change what Orca decides to cancel, it only takes the survivors down to zero.

Adding up every stretch a hotend spent switched off comes to a few tens of watt hours on this one
print. Treat that as a ceiling rather than a figure: over the shorter gaps a nozzle does not fully
cool before the preheat brings it back, so the real saving is under it. The two long ones are whole.

### The wall an overhang lands on gets slowed down too

**Where**: Print settings → Speed → Overhang speed → **Slow down inner walls next to overhangs**, in
LibreMode. Off by default.

Slowing an overhang down is only half of the job. The outer wall of an overhang is printed onto the
inner wall next to it, and that inner wall went down at full inner wall speed a few seconds earlier,
because the slicer correctly sees it as fully supported. It is. The wall it has to hold up is not.

So the anchor is the fastest, least controlled bead in the area, and everything the overhang settings
do is spent landing carefully onto it. If the layer below came out perfect the overhang above is well
supported and none of this matters, which is exactly the assumption worth removing: this makes it
more likely that the wall underneath was printed properly in the first place.

With this on, the inner wall is graded as if it were the wall next to it. Orca already decides how
much to slow a wall down by measuring how far each point sits from the edge of what was printed
underneath. This walks that measurement one wall outwards, by the percentage you set: at 100% the
inner wall is treated exactly as the overhanging wall it has to support, at 50% halfway there. The
overhang fan follows for free, because the fan reads the same measurement.

![Four panels. Top left and top right are the same overhanging nose of a part in the Gcode speed view, off and on: on the left only the outermost line is blue with the wall behind it at full speed, on the right the band of blue widens to take in the wall behind it and the tip of the nose slows further. Bottom right is the SLOWINNER test plate, two copies of a row of overhang steps from 70 down to 45 degrees. Bottom left are photographs of the printed result, the underside of the steps](docs/images/Inner-Slowdown.png)

**Reach** is how far outwards to look, as a percentage of line width. 100% is the wall immediately
next to this one; the default of 200% looks two line widths out, so it still finds the overhang where
it starts a little beyond that wall.

The defaults, 30% slowdown over 200% reach, are deliberately gentle. Enough to make the anchor more
likely to be there, not enough for the speed change to show up on the wall.

**It is printed.** The SLOWINNER plate in the picture is a row of overhang steps from 70 down to 45
degrees, printed twice, once with the box off and once with it on, everything else the same. The
underside of the steps is visibly better on the second one, and the difference is larger than the
size of the change would suggest. That is the whole point of it. The overhang comes out better
because the bead it lands on was printed better.

One number to keep it honest. On the first model this was measured against, 96% of the inner wall
running within 1.3 mm of an overhang was already being slowed by Orca on its own. The window this
works in is shallow slopes, where the inner wall really is sitting on solid material at full speed
while the outer one is already over air.

Away from an overhang it costs nothing at all. A wall with solid material under it sits far inside
that boundary, and moving the measurement one line width does not change anything about how it is
graded. There is no region to detect and nothing to switch on for it: the geometry decides.

It needs **Slow down for overhang**, whose machinery this feeds, and it has no effect when walls
print outer wall first, since then the inner wall goes down after the overhang and there is nothing
left to anchor.

Someone asked upstream for this kind of control and the request closed without an answer
([OrcaSlicer #3891](https://github.com/OrcaSlicer/OrcaSlicer/issues/3891)), so if it sounds like a
private obsession, it is not just mine.

### Spiral lift no longer runs off the bed

**Where**: nothing to switch on. This one is always active.

A spiral lift is a full circle. When the nozzle lifts on a retract with this hop style, it does not
go straight up, it climbs a helix around a point set one radius to the side of where it started. The
radius comes out of the hop height and the travel slope, and on the U1 defaults (0.4 mm hop, 3 degree
slope) it is 1.22 mm. Since the circle is centred one radius away, the nozzle sweeps out to roughly
2.4 mm from the retract point, in whatever direction the next travel happened to be heading.

Nothing about that is a problem in the middle of the plate. It becomes one when a retract lands
within a couple of millimetres of the bed edge, because then part of the circle is outside the
printable area. The U1 halts the print on any out of bounds move, which is the right thing for a
machine to do and no comfort at all when it happens on layer 480 of a 27 cm tower.

The code that emits the spiral has carried a note about this since the Bambu Studio lineage, saying
the arc ought to be checked against the bed and the lift downgraded when it does not fit. The note
sat there and the check was never written. Upstream OrcaSlicer has since written it, as
`spiral_lift_fits_printable_area`, falling back to a straight lift for the arcs that do not fit. That
is upstream's patch and this build carries it, ported back onto the 2.3.x base Snapmaker Orca sits on
so you get it now rather than at the next rebase.

**What that means for you**: an arc that would leave the printable area is emitted as a straight
lift, which moves no X or Y at all and therefore cannot leave the bed under any geometry. The print
survives. Nothing needs configuring.

### Telling you where, and why, it happened

The check above is upstream's. What is added here is that the build tells you what it found, because
a straight lift is not free and you should get to decide about it.

A spiral hop sweeps the nozzle sideways as it climbs, and that wipe is part of why the hop style
exists: with some materials it is what keeps a string from following the travel. A straight lift
gives that up. So the spots where an arc got degraded print safely and print slightly dirtier, and
that trade is yours to make, not the slicer's to make quietly.

After a slice you get one of three answers:

- **Nothing at all**, when the print contains no spiral lift. There is nothing to report.
- **A confirmation** that every spiral lift in the file fits inside the printable area. It fades on
  its own. This is the one you want to see, and it is the reason the message exists: a warning that
  only ever appears when something is wrong leaves you guessing on every other print.
- **A count**, when some had to be straightened: how many out of how many, and the Z height of the
  first. From there you can decide whether to move the part in from the edge, lower the hop, or leave
  it be.

The old warning in Prepare has been kept, and corrected. It used to announce a possible collision from
the object's bounding box alone, with a fixed 3.5 mm margin, whether or not spiral lift was even
enabled. It now stays quiet unless the hop style can actually produce a spiral, it uses the real
envelope for your hop and slope instead of a round number someone picked, and it says what it
actually knows: you are close to the edge, slice to find out.

### Checking it yourself

You do not have to take the count on trust. Every spiral lift in the Gcode is a `G17` followed by a
`G3` carrying `I` and `J`. Those two are the offset from the current position to the centre of the
circle, so the radius is their hypotenuse and the centre is your position plus the offset. Add and
subtract the radius from the centre and compare against your printable area, which for the U1 is
0.5 to 270.5 in X and 1 to 271 in Y. Everything left in the file should clear it.

One trap worth knowing: those `spiral lift Z` and `normal lift Z` comments only exist when Gcode
comments are on. With them off the moves are still there and the comments are not, so grepping for
the words finds nothing and it looks like no lifts happened. Go by `G17` and `G3`.

The test this was signed off on is four towers just under 27 cm, one per corner, four tools, 1244
layers. **912 spiral lifts survived, none of them outside the printable area, and the tightest one
clears the boundary by 0.010 mm.** That last number is the interesting one. It says the check is
exact rather than cautious: it degrades what genuinely does not fit and leaves everything else alone,
down to ten microns. A guard with margin to spare would have straightened several hundred more arcs
and cost you their wipe for nothing.

### A place for settings that are yours, not the profile's

The button above is the first tenant of **Bed and Nozzle Extras**, and the reason it exists is worth
a paragraph.

Everything in there is a preference of yours about how your machine should behave, and none of it
belongs to a print profile. Put a switch like this in a profile and it lasts until you change
filament preset, and it fights you every time Snapmaker publishes new profiles. So these live with
the application, next to the Bed type selector that already worked this way. Set it once. It survives
profile updates, it never marks a preset as modified, and it has nothing to collide with.

The button says how many extras are on, so nothing in here is ever changing your Gcode in silence.

### Variable layer height survives a resize

**Where**: nothing to switch on. It applies both to the Precision ALH editor and to the stock layer
height brush.

A variable layer height profile is stored as a list of heights against Z, in millimetres from the
base of the object. Scale the object taller or shorter and every Z in that list still points at the
old height, so the last entry no longer lands on top of the object. Two separate pieces of code read
that as a corrupt profile and threw it away: the slicer cleared it and regenerated a flat one, and
the Precision ALH editor reseeded a flat curve the next time you opened it. Between them, resizing an
object quietly cost you the whole curve, and the editor then wrote that loss back into the project.
Shrinking was worse than growing. The editor kept the curve, moved only its top point down to the new
height, and left every other point above the roof of the object.

Both of them now rescale the profile instead of discarding it. The Z of every point is stretched or
squeezed to the new height and the layer heights themselves are left alone, so a 0.12 layer is still
a 0.12 layer and what changes is between which heights it applies. The editor tells you when it has
done this, so numbers moving under you are never a surprise. A profile that is genuinely damaged
rather than merely out of date is still discarded, same as before.

Scaling the curve by hand, to pull the detail towards the bottom or spread it out with the top pinned
where it is, is a different job and is not in this build.

### Typing a layer number in the Gcode Post-Processor

**Where**: the rule chart in the Gcode Post-Processor. Click a layer number in the summary line under
the chart, or right click the point itself.

Rules are placed by dragging their endpoints up and down the chart. The chart is around 200 pixels
tall and the whole print has to fit inside it, so on a 1200 layer print one pixel is worth six layers
and there are numbers you simply cannot land on with a mouse.

The two layer numbers in each rule's summary line are buttons now. Press one and a field opens where
you type the layer. The upper one also carries a **to END** tick for rules that should run to the end
of the file, and typing the last layer means the same thing, exactly as dragging a point to the top
of the chart always did. That same field is the first thing in the popup you get from right clicking
a point, for whichever end you clicked, with delete still at the bottom in red. Dragging behaves as
it always has, and both routes clamp the range the same way, so there is only one answer to what
counts as a valid range.

### Support Zones: block trees, and a foot placed for you

**Where**: the **Support Zones** gizmo on the left toolbar. Needs Libre Mode. Nothing here is on by
default, and a plate with no zone drawn slices exactly as before.

Two changes, and the second one only happens if you ask for it by painting.

**The foot is placed for you now.** Making a zone used to be two compulsory clicks, and until you
made the second one the foot followed the cursor. That is how a pillar ended up 40 mm to one side of
the thing it was holding without anyone asking for it: measured on the mesh, the foot travelled
40.6 mm sideways in 32.9 mm of height, which is the lean angle exactly, applied to a distance nobody
chose. The landing is now dropped plumb under the middle of what you picked, and **Move it again**
is there when you want it somewhere else.

**Painting a zone now builds a block tree.** Aimed pillars work on simple parts and stop working on
real ones, and the reason is one thing: the footprint came from flattening the surface into plan
view. On the inside of a curve that goes vertical and then turns back, one spot on the plate has
several heights, so the outline crosses to the far side of the hole. Growing a circle inside a torus
took its outline from 94 points to 292 and tripled the slope of its roof, and 42 of the 512 points on
that roof ended up more than a millimetre away from the point next to them, the worst by 7.56 mm
across a gap of 0.49 mm.

So the shape changed rather than being smoothed. Paint the area you want held up, a stump is planted
plumb underneath, and **the stretch between them is not drawn at all**: the slicer walks the column
down through the gap, moving it toward the nearest stump and closing it onto that stump once it
arrives, spending at most one lean step per layer on the two together. The roof of the head is flat,
so the slope that caused all of the above is now zero rather than small.

None of the walking is new. The corridor has been stepping columns sideways layer by layer since
2.4.4, with the same budget recomputed from the real layer height. What changed is where it reads the
direction from. It used to read it from the shape of the block; now it reads it from the stump.

**One stump is enough almost always**, and you get it for free. When it is not enough the panel says
so, naming the layer and how far short the column fell, and the answer is to plant another. Two stumps
split the area between them so a long or forked region comes down as two legs. A block tree does not
branch: one stump grows one trunk.

**Also in the tool.** The square footprint used to vanish from the row the moment you picked any
other shape, with no way back short of reopening an older zone. All four shapes stay on the row now.

**Two things the tool now tells you, because both of them cost a session to find.**

The first is the support style. Painting a zone with the style on **Grid** or **Default** snaps the
column to the support grid, so it cannot come out thinner than one whole cell, which is about 2.8 mm
at the default spacing and 5.3 mm at 5 mm, and it steps sideways a cell at a time instead of sliding.
It prints, and for some shapes that chunkiness is useful, so it is a notice at slice time and not a
veto. The tool already puts the style on **Snug** for the object when you open it, which is what the
printed parts were made with.

The second is that automatic support stays on unless you turn it off. A zone adds to what the slicer
already decided to hold up, so support you did not draw is the overhang detection doing its job. The
**only my zones** switch at the top of the zone list turns it off for that object, and the panel now
offers the same switch as a one click fix while the condition holds. It stays a switch: someone who
wants both should keep both.

**Printed.** A vase with two extruders, Precision Adapter Layer Height, and height ramps running
from 0.32 mm down to 0.08 mm, held up by two painted stumps. Hours of it, and the stumps came out
solid.

**On the gap over the roof.** Planting a zone seeds a few support settings on the object, and the
top Z distance among them is now **0.2 mm**, the same value the default profile uses. It used to be
seeded at 0.1 and on that print the roof welded itself to the part here and there. The width of the
zone has nothing to do with it: what decides it is how well the overhang of that layer comes out, and
a layer that sits a little high closes the gap on its own. There is a second reason to leave it at
0.2 with adaptive layer height, which is that the support roof is not sliced at the height the part
is using there, so the gap you ask for and the gap you get are not the same number. Making the roof
follow the part's layer height is on the list and is not in this release.

### The wipe tower stops building on layers that are not there

**Where**: nothing to turn on. It applies whenever the prime tower is on and the objects on the
plate do not share one layer grid, which on this build means adaptive layer height mixed with a
fixed one.

Adaptive layer height together with a tool changer and a prime tower is something this fork does and
other slicers do not, so this is ours to get right. Upstream Orca still carries the guard that
refuses the two together. We took that guard out to make the combination possible, and taking it out
exposed something the code underneath was never asked to handle.

Put two objects on the plate, one on adaptive layer height and one at a fixed height, and give them
different filaments so the tower appears. The tower has to visit every height where either object
has a layer, so its own layer heights are whatever gaps the two grids leave between them. Where the
grids nearly line up those gaps are tiny, and a tower layer of two hundredths of a millimetre carries
almost no material. The tower came out as loose threads instead of a solid block, and on a long print
it collapsed under itself.

Two separate things were wrong, and both are fixed.

**The first is the one that broke prints.** When one object sits higher than the plate, the code that
works out where the objects start kept the bottom of the last object in the list rather than the
lowest one. Every height below that value was then treated as being underneath the model, and the
tower was built on all of them, including heights that belong to the raised object's grid before its
geometry begins. In the test plate that was 75 of the 112 layers in the tower's base: layers planted
on the grid of an object that is not there yet. The real layers of the object that is there arrived
with two to six hundredths of space left instead of their full 0.32, and got padded up to the
minimum, which is where both the missing density and the nozzle dragging came from.

Measured on that plate before the fix: 42% of the tower's layers were under 0.08 mm, they accounted
for 22% of its height, and each of them was laid at a third of the cross section of a healthy layer
while travelling nearly twice as far. The base is now 37 layers of 0.32, which is what the object
asked for.

**The second is smaller and only shows on tall prints.** Where the two grids land on exactly the same
height, which in the test happened every 4.80 mm, the height of a layer was written by whichever
object came last, even when that object's layer at that height is empty. An empty layer is grid, not
geometry, and it was overruling an object that actually prints. The tower then laid a 0.15 layer into
a 0.32 gap and floated 0.17 mm above its own support. An empty layer no longer sets the height of a
level that a printing layer has already claimed.

Both fixes are in the layer planning that runs before the tower, so NeoTower itself is untouched, and
a plate whose objects share one layer grid slices exactly as it did before.

### If you use adaptive layer height with a tool changer, turn the ramming down

This is a setting, not a bug, and it is worth knowing before you start a long print.

**Multi-tool ramming flow** is a volumetric number. The printer is told how many cubic millimetres per
second to push, and the linear speed the nozzle actually moves at comes out of dividing that by the
cross section of the bead it is laying. Cross section is width times layer height, so the thinner the
layer, the faster the move has to be to deliver the same flow.

On the tower that matters more than anywhere else, because the tower's layers are the gaps between two
layer grids and some of them are very thin. At the default 25 mm³/s, a tower layer 0.04 mm tall works
out at **630 mm/s**. The same setting on a 0.2 mm layer is a perfectly reasonable 126.

Nothing clamps it on the way. Orca does not, and neither did we.

**Drop Multi-tool ramming flow to 5 mm³/s** if you are running adaptive layer height with more than
one tool. That brings the fastest move on the same plate down to about 220 mm/s. The fix above removes
most of the very thin tower layers, so this is less dramatic than it was, but the ceiling is still
there and 25 is aimed at ordinary layers rather than at what a tower ends up with.

### Eighteen fixes from Snapmaker 2.3.6 and 2.3.7

Snapmaker took 2.3.6 out of beta and 2.3.7 is being built in the open alongside it. Eighteen of their
fixes are in this build. They are theirs, so the short version, by pull request number:

| PR | What it fixes |
|---|---|
| #728 | Tree supports hung the slicer at 70% when the support base pattern spacing worked out to zero |
| #784 | Crash slicing a multi material plate with a raft after switching supports off |
| #805 | Changing nozzle corrupted the purge volume matrix |
| #722 | Crash reloading a Gcode while the previous one was still loading |
| #753 | Crashes on shutdown and while the interface rebuilds itself |
| #743 | Gcode from stock OrcaSlicer would not open, and crashed the Filament view |
| #765 | Paths only partly inside the selected layer range were drawn in full |
| #810 | Small area flow compensation ran with its own checkbox off |
| #769 | The sidebar nozzle box did not refresh when the printer changed from elsewhere |
| #735 | A failed printer connection could take the application down with it |
| #741 | The sign in token was cut at `?` only, so `&` and `#` carried junk into it |
| #740 | Syncing filaments from a printer reporting none left you at zero, then crashed |
| #729 | Filament profiles under a path with non-ASCII characters would not load on Windows |
| #750 · #752 · #754 · #756 · #757 · #751 | Bounds and formatting hardening across the slicer and the viewer |
| #762 | The version string in a `Snapmaker_Orca` 3mf tag was read four characters short |
| #721 | ABS and ASA on the 0.4 nozzle were not marked heat resistant |
| #778 | Official filament colours are drawn as a vertical gradient, bottom to top, for the new FullSpectrum filaments |
| #795 | Picking a colour for a preset that is no longer compatible |

Three of them carry a decision of ours on top:

- **#762** also changes what we write. Saved 3mf files said `BambuStudio` in their generator field,
  inherited from a compatibility shim far upstream. They now say `Snapmaker_Orca`. Reading is
  unaffected either way, so projects carrying either tag still open.
- **#778** brings a transmission distance value for each official colour, which this build reads and
  stores and nothing uses. RealColor keeps running on your own TD values and will keep doing so.
- **#805** stops the purge recalculation outright when the matrix does not match the filament count,
  where Snapmaker only write a line to the log.

---

## Notes

- **Both checkboxes are opt in.** Left alone, the exported Gcode is byte identical to 2.4.4.
- Extra Energy Save does nothing on its own; it widens the rule of the checkbox above it, and greys
  out while that one is off.
- Switching it on invalidates the exported Gcode but **not** the slice, so it exports again without
  reslicing the plate.
- **Fixed**: with the hotend switch on, a single tool print used to end with a warning saying no tool
  could be switched off. It was telling the truth and it was useless. A print with one tool never
  changes tool, so it never emits the standby command this works on, so there was never anything to
  switch off in the first place. The warning now only appears when the print actually uses more than
  one tool, which is the only case where it means something went wrong.
- The layer height rescale needs nothing enabled. A project made before this build gets its curve
  remapped the first time it is sliced or opened in the editor after a resize, and the profile stored
  in the 3mf is left untouched.
- A typed layer number is saved as soon as you type it, while a dragged one is saved when you let go.
  Both land in the same rule.
- **Block trees are print verified**, on the vase described above. The aimed pillar that 2.4.4
  shipped is unchanged and stays print verified. Both live in the same tool and the footprint you
  pick is what chooses between them.
- These supports ask you to think, and that is the point of them. The tool measures, draws and warns;
  it does not decide. Automatic supports are still one checkbox away and are untouched by any of this.
- **The wipe tower fixes need nothing enabled and cannot be turned off.** They only change the slice
  when the objects on the plate disagree about layer height. If every object shares one grid, which is
  the ordinary case, the exported Gcode is the same as before.
- **The tower fixes are print verified**, on the same 14 hour plate that failed before them. What
  was measured after them on the failing plate: the tower's base back to 37 layers of 0.32, no layer left floating
  above its support, and no purge lost anywhere in the multi material regression plate.
- Very thin tower layers still exist higher up, where both objects are real and their grids genuinely
  interleave. They print, and they are what the ramming note above is about. Making the tower skip a
  level too thin to be worth printing is being looked at and is not in this release.
- The spiral lift check is always on and is upstream OrcaSlicer's work rather than a switch of ours,
  so it is not listed with the opt in features above. It changes the exported Gcode only for lifts
  that would have left the printable area, which on a plate with nothing near the edge means not at
  all.

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
