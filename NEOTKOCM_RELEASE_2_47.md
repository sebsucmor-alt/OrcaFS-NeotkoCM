## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko 2.4.7 — on Snapmaker Orca 2.3.5 — Release Notes (draft, in progress)

> ⚠️ **Review your generated Gcode before long or production prints, especially if you
> turn on any of the features below.**

**2.4.7 is an incremental release on top of 2.4.6** (see `NEOTKOCM_RELEASE_2_46.md` and earlier
notes for the full feature set).

---

## What's new in 2.4.7

### NeoStroke: cleaner paths and solid letters (still debug mode only)

**Where**: Quality → Wall generator → **NeoStroke**.

> ⚠️ NeoStroke is still not ready for production prints. It is a lot closer than in 2.4.6: on our
> test plates small raised lettering now comes out closed, including the letters that always had a
> hole. Check the Gcode before a long print.

**One switch to unlock it.** Tick **Enable NeoStroke wall generator (unstable)** in Preferences and
that is it. Libre Mode is no longer needed. In 2.4.6 the checkbox was missing from the Preferences
window, so the only way in was starting Orca with `ORCA_DEBUG_NEOSTROKE=1`. It is there now and it
applies straight away. The variable still works too.

**The hole in letters like B and E.** Most of it was not in either engine. It was in the seam between
them. Where the outer wall turns an inside corner it leaves a small pocket, and Classic counts on its
own gap fill to close it. With NeoStroke gap fill is off, and NeoStroke used to start exactly where the
wall ended, so nobody filled the pocket. NeoStroke now reaches under the wall by the same
**Infill/Wall overlap** you already use for infill, so there is one setting for both. 20 % closed most
of it on our plates. 10 % leaves some of the pockets, and 35 % started to push neighbouring letters
into each other.

> 💡 **For lettering, set Infill/Wall overlap to 20 %** in the profile you use for it. The Snapmaker
> profiles ship with 15 %.

**No more flow stops at the end of each stroke.** A stroke with several lines used to stop and restart
the flow at every turn between two lines, and every stop can leave a small dent. The lines are now
joined and printed in one go (**Join line ends (no flow stops)**, on by default). The join runs
straight across the end of the stroke. A tight half circle there, which we tried first, left more of a
notch than stopping did.

**Fewer lines where a stroke narrows.** A stroke used to keep the same number of lines along its whole
length, so where it got thin those lines became hairs the nozzle cannot really print. Now the count
drops where the stroke narrows and no line goes below **Thinnest printable line**. A line that is
dropped tapers out while its neighbours take its room, so there is no gap where it ends. Rings keep
one count all the way round so they stay tidy. (**Fewer lines where it narrows**, on by default.)

**Settings renamed and grouped.** The names now say what you see in the print rather than how the
engine works inside. Some of them:

| Before | Now |
|---|---|
| real minimum bead | Thinnest printable line |
| thinnest bead | Thinnest small detail |
| maximum width | Target line width |
| minimum width | Thinnest line at tips |
| hard bead limit | Widest line allowed |
| width reference | Reference line width |
| curve overlap | Extra flow on wide lines |
| corner hooks | Reach into corners |
| skate over printed lines | Glide over printed lines |

Five of them said "% of nozzle" while they were really a percentage of the reference line width. They
now say "% of reference". The Advanced window is split into groups: line widths, shape, extra flow,
path order and travel, and Classic's wall with its overlap.

**What we tried and left out.** Very wide lines (twice the nozzle) do print on straight runs, but
they over-extrude in curves and circles, and making them overlap each other did not help. Thinner
lines than the default (0.20 mm) and a higher speed made things worse. The defaults are the settings
that came out best across all of it.

**Removed**: the end cap join (replaced by joining the line ends) and the five settings that shaped
the extra flow on wide lines. Those keep the values they always had. Projects that carry them still
open; the old values are simply ignored.

### NeoStroke Preview: see the paths before you print

**Where**: the new **NeoStroke Preview** tool in the 3D view's toolbar (available once NeoStroke is
unlocked in Preferences).

The old preview lived in the Advanced window and read the settings of the tab it was opened from, so on
a plate with per-object settings it showed something different from the Gcode. The new one is a tool on
the plate, and it slices each object with **its own settings**, the same way the real slice does. It
uses the same engine, so what you see is what the Gcode will get.

- **What to look at**: the selected objects, every object inside a rectangle you draw on the bed
  (**Zone**), or an STL, OBJ or 3MF you load only for the preview (**File**). A 3MF keeps each
  object's settings, and the plate is never touched.
- **Which layers**: pick the top layer and how many layers below it to show. The object is cut there so
  the paths sit on top of it.
- **How it looks**: lines are drawn with the room they really take on the layer. Classic's wall is
  grey, NeoStroke is coloured by width, by path, by extra flow or by risk. Gaps are filled in by
  where they are: next to Classic, in the joint between both, or inside NeoStroke. Every path start and
  stop is marked.
- **Numbers that stay on screen**: how much of NeoStroke is a real bead, how many times the flow
  starts, how long a path runs on average, and the gaps. **A/B** freezes a result so you can change a
  setting and compare.
- **Risks**: what our macro photos of the test plates showed that fails. A path that starts or ends
  with nothing on either side, printed faster than about 20 mm/s, stretches and snaps at the tip (it
  held at 15 mm/s and broke at 30 and 60). Two lines 0.6 mm or wider next to each other leave a seam
  between them. A thin line with no neighbours does not come out. Passes over lines already printed are
  shown too.
- **Settings in place**: NeoStroke's settings are inside the tool, with the value in mm next to each
  percentage. Hover one to see a small drawing of what it changes and where it applies on your part.
  Changes go to the selected object, like the object list, with undo.
- **The lock**: turn it on and clicks only move the camera, so you can look around without selecting or
  moving anything by accident.
- **Pile-ups**: where NeoStroke lays more plastic than fits, averaged over a third of a millimetre. That is
  where plastic heaps up and the nozzle drags it. Holes are shown in red, and the view switches sit next to
  the numbers so you can hide the pile-ups and look underneath while you change settings.
- **Hills, grooves and bunched starts** (new after TEST25, where we laid the photos of a printed plate over its
  Gcode and the preview): **Hills**, in blue, show where a little too much plastic runs along a whole stretch and
  rises into a ridge; extra flow in curves does this. **Grooves**, in yellow under Risks, are lines that do not
  overlap their neighbour, which shows as a groove and lets light through against a lamp. **Pits** now also mark
  two or more path starts bunched together: after the travel none of them has pressure yet, and they leave a
  hole. Pile-ups now count extra flow in curves too; before, they could not see it.
- **Warning level and material closure**: two sliders under the view modes. Warning level moves every warning at
  once (higher warns sooner). Material closure is how much two lines must overlap for you not to see a groove:
  glossy PLA needs more, matte PLA spreads and needs less.
- **Presets**: Detail, Standard and Fast in one click (15, 30 and 45 mm/s for NeoStroke, wider lines on
  Fast, extra flow 15 % on all three). With several objects selected, changes go to all of them unless you
  untick it.

**Small gaps between strokes are now closed.** Each stroke used to decide its lines looking only at its own
lane, so the spot where two strokes meet (the stem and the bowl of a P, an inside corner) belonged to
neither. When that spot was a little too narrow for a line it stayed empty, and whether it did depended on
tenths of a millimetre of the shape: the P of a test logo came out with a hole in some layers and not in
others. NeoStroke now looks at all the gaps of a layer once its lines are placed, and the lines next to each
gap widen towards it and shift a little, never past **Widest line allowed**. There is no new line, no new
start and no extra travel. Gaps thinner than 0.06 mm are left alone: the bead closes those by itself.

Areas still left over that are narrower than **Widest shape handled** are filled by NeoStroke with closed
rings instead of going to the infill. Wider areas go to the infill as before.

### A new way to plan NeoStroke's paths

NeoStroke no longer draws each stroke on its own and fills what is left between them. It shares out the whole
width of every section at once: the lines follow the letter like the wall does, the middle line runs along the
centre of the stroke, and a line that is no longer needed thins out between its neighbours instead of stopping.
On our test plates it closes the holes at joints and serifs, keeps more of the plastic in lines the nozzle can
really lay, and the paths come out clean, with no odd or repeated moves. Small raised lettering is where it shows
the most.

Very wide areas (wider than **Widest shape handled**) still use the previous planner, which leaves them to the
infill as before.

**Fewer settings.** The new planner only needs three limits, and they are now the main NeoStroke settings in the
tab: **Widest line allowed**, **Target line width** and **Thinnest printable line**. Five settings of the old
planner are gone from view because the new one does not use them: Thinnest line at tips, Join line ends, Thinnest
small detail, Fewer lines where it narrows and Reach into corners. Projects that carry them still open.

### Three new NeoStroke settings for the path starts and the lines

All three are per object and live in the Advanced window and inside the NeoStroke Preview tool.

- **Overlap between lines**: the new planner lays its lines exactly side by side. They touch but do not
  overlap, and on glossy filament the join can show. This adds the same small amount of plastic to every
  NeoStroke line, so each one reaches a little over its neighbour. Extra flow in curves did the same job, but
  only in tight curves, and there it heaped up. Default 4 %.
- **End paths at junctions**: where several paths meet, they are printed so they end there and start at their
  free end. Starts bunched together leave a hole; ends bunched together close. Same path and same plastic, only
  the direction changes. On by default.
- **Lead-in before each start**: each path starts this far ahead on its own line and runs back to the real start
  before going on. The weak first bit after a travel lands where the line passes again right away, so the real
  start already has pressure. How much you need depends on the filament. Off by default while we test it.

**New defaults** from the test plates: extra flow in curves 0, overlap between lines 4 %, widest line allowed
180 %, target line width 120 %, thinnest printable line 60 %, reference width 0.32 mm. For lettering we print
NeoStroke at 15 mm/s with the Infill/Wall overlap at 20 %; those two live in your profile.

---

## Notes

- NeoStroke changes the Gcode of projects that already use it: the new defaults are on, and the
  Infill/Wall overlap of your profile now applies to it.
- **Review your generated Gcode** before long prints with NeoStroke.

---

## Known issue, still open

**The angle a zone reports does not always match what gets sliced.** Carried over from 2.4.4, where
it is described in full. **Until it is fixed, slice and look at the Gcode preview in RealColor.**
