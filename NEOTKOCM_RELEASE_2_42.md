## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.4.2 — on Snapmaker Orca 2.3.5 — Release Notes

> ⚠️ **Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.**

**2.4.2 is an incremental release on top of 2.4.1** (see `NEOTKOCM_RELEASE_2_41.md` and earlier
notes for the full feature set). Everything remains **opt-in**: at defaults the build behaves like
stock Snapmaker Orca.

---

## What's new in 2.4.2

### The Color Mixing list can be collapsed

On a plate with several filaments, the sidebar fills up with auto-generated colour mixes. With four
filaments you get six of them, and the list pushes the thing you actually came to look at — your real
filaments — down and out of view. There was no way to put it away: the section was always open, and
the only time it disappeared was when there was nothing in it at all.

Clicking the **"Color Mixing"** header now collapses and expands the list, the same way the
"Filament Management" and "Mixed Filaments" headers already worked. The header itself stays visible
with its add and remove buttons, so the section is always one click from coming back.

The state is remembered between sessions — collapse it once and it stays collapsed next time you open
the slicer. It starts expanded, so nothing changes for you unless you decide to put it away.

Clicking the **+** and **−** buttons in the header still does what it always did; only clicks on the
title area toggle the section.

Nothing about the mixes themselves changed — this is purely how much room the list takes up on
screen. The mixes are still generated, still slice identically, and collapsing the list has no effect
on your G-code.

---

### The purge tower was leaving gaps in itself

If you looked closely at a tall purge tower — most easily with **variable layer height** turned on —
you could find bands where the wall simply had nothing in it. Thin slots of air, a fraction of a
millimetre each, at heights where the tower should have been solid. A tower with air in its wall is
weaker, and a weak purge tower is one that can be knocked over by the toolhead halfway through a long
multi-colour print.

There were two separate causes, and both are fixed.

**The tower wasn't visiting every layer it needed to.** It decides how often to stop by and add
material, and it was allowed to skip ahead by the thickest layer your printer can print — but it only
ever lays down a layer as thick as the print is actually using at that height. With variable layer
height those two numbers drift far apart: it was allowed to skip a quarter of a millimetre, then put
down eight hundredths. The rest was air. It now paces itself by what it will actually deposit, so it
visits every layer it has to.

**Some layers were planned and then never printed.** On layers that changed colour without needing a
purge, the tower's structural fill was worked out correctly and then never made it into the G-code.

The important part of the fix: the gaps are closed by **printing the passes that were missing, each
at its real height** — not by pushing more plastic through the nozzle on the passes that already
existed. Extrusion is limited by how fast filament can melt, and the tower already runs at that
ceiling on its fastest moves. Overfeeding it to paper over a gap is how you get a jam.

Your tower will have a few more layers than before in the affected stretch, and use a little more
filament. It prints at exactly the same speeds and flow rates as before, and comes out the same size
— the tower's footprint and depth are unchanged.

This is on by default and needs no setting. It affects any multi-colour print with a purge tower;
you'll notice it most with variable layer height, which is where the gaps were widest.

---

### Photo Mode — send a client a picture without opening a renderer

**Where**: the **camera button** on the plate's icon column, in **Prepare**. Needs **Libre Mode** on.

The everyday problem this solves: a client asks what the part looks like in a different colour. You
change it, and now you need a picture. Opening a real renderer for that is a five-minute detour for a
thirty-second question — so you end up sending a screenshot of the slicer, gantry, grid, logo and all.

Photo Mode turns the Prepare viewport into a small photo studio. The scene stays exactly the object
you already have on the plate; what changes is everything around it.

**A studio, instead of a print bed.** Choose **Lightbox** and the bed, grid, logo, plate numbers and
toolbars go away, replaced by a **cyclorama** — a floor that curves up into a back wall with no seam,
the same white sweep a product photographer shoots against. **Backdrop** gives you the floor alone,
for top-down shots. **Print bed** leaves everything as it is, for when the bed is the context you
want the client to see.

**Lights you can actually aim.** Three of them — key, fill and rim — each with a **draggable ball**
rather than three number boxes: drag toward where you want the light, and the shadow follows. The
light lives in the room, not on the camera, so you can orbit around the object and the lighting stays
put. Six presets do most of the work: **Neutral** (identical to the normal viewport), **Studio
3-point**, **Softbox top**, **Rim / backlight**, **Flat catalog** and **Dramatic**.

**A material per filament slot.** Each slot gets a look of its own — **Plastic**, **Glossy / resin**,
**Matte PLA**, **Rubber / TPU**, **Metal** or **Silk** — chosen from a list that shows each slot's
real colour beside it. On a painted multi-colour object the material follows the **colour**, not the
part, so one keyring body can be matte on the stripes and metal on the lettering. Plastic is the
default and is pixel-for-pixel the shading you already had.

**An environment to reflect.** Optional, off by default. It builds a virtual room out of your own
three lights, which the objects then reflect — move the key light and its reflection moves with it.
This is what separates a convincing metal from grey plastic, and there is a **rotate room** control
for aiming a reflection without moving the lighting.

**A floor that reflects.** At **High** or **Ultra** quality the objects are mirrored in the floor,
stronger at grazing angles than face-on, with an adjustable strength. It is a real reflection of the
scene, not a screen-space approximation, so it does not break down at the object's outline.

**Quality has a dial.** **Normal** is the everyday interactive setting. **High** and **Ultra** raise
the shadow map to 4096 and 8192 pixels and widen the shadow's soft edge to match, and they are what
unlock the floor reflection. Ultra is heavy by design — on an M4 with a fairly complex model and the
reflection on it runs at 10–15 fps, which is fine for framing a still and not meant for modelling.

**Getting the picture out.** For now: **Hide UI for screenshot** clears every control — including the
panel itself — for a few seconds so you can grab the frame with your system's screenshot tool, then
brings it all back. Esc cancels. Direct **Save PNG** and **Copy to clipboard** are written but
**disabled in this release**: the off-screen render still frames the shot wrongly, and a button that
produces a broken file is worse than no button. They will come back on once that is fixed.

**Your setup is yours.** Lights, materials, stage, environment and quality can be saved as named
presets under **My presets**, stored with the application and **not** inside the 3MF — a lighting
setup is your preference, it should follow you across projects instead of riding along inside a model
file you share and print.

Photo Mode changes **nothing** about your model or your G-code. It is a viewing mode; Esc, the camera
button, or switching to Preview leaves it.

---

### The G-code preview can be trimmed from the start, not just the end

**Where**: the **horizontal slider** at the bottom of **Preview**.

The slider that walks through a layer's moves only ever had one handle, at the end. You could say
"show me everything up to here", never "show me only from here to here". So to look at one purge on
the wipe tower, or one colour stitch, you had to drag the whole layer into view and then hunt for the
few centimetres you cared about, with everything printed before it still on screen.

It now has **two handles**. The one on the right is the one you already know. The new one on the left
sets where the view *starts* — drag it in and everything before it disappears, leaving only the stretch
between the two.

![The G-code preview's move slider with two handles: the left one trims the start of the range and the right one the end, isolating a stretch of moves inside a layer](docs/images/gcode%20start-end.gif)

Everything that used to follow the right-hand handle now follows **whichever handle you last grabbed**:
the virtual toolhead jumps to it, and the G-code text window scrolls to that line. Grab the left
handle and you are looking at the first move of your window; grab the right one and nothing has
changed from before.

The two handles stop against each other instead of pushing, so you cannot collapse the range by
accident, and they keep a move's gap between them so you can always tell which one you are holding.

**At its default position the left handle changes nothing.** Leave it alone and the preview behaves
exactly as it always has — including the stacked-layers view, where every layer below the current one
stays on screen in grey. Only once you move it does it begin to trim. Re-slicing or loading new G-code
returns it to the start.

---

### RealColor got a pass over how it looks

**Where**: G-code **Preview** → the view-type dropdown → **RealColor**.

RealColor is the view that stops drawing each extrusion in its filament's raw colour and instead
composites the surface the way your eye will resolve it, so a Sandwich stack reads as the one blended
tone it will really have instead of as stripes. That has been in the pack for a while. What changed in
2.4.2 is how convincing it looks.

![Side by side on the same G-code: on the left the traditional preview, where every extrusion is drawn in its filament's raw colour and each surface reads as hard diagonal stripes of blue, pink and orange; on the right RealColor, where those stripes resolve into the single blended tone the print will actually have](docs/images/RealColor.png)

**The surfaces stopped being see-through.** The preview used to draw each extrusion at its exact
nominal width, laid edge to edge with no overlap. A real extrusion squashes against the one next to it
and the two merge — there is no seam. In the preview there was, and through those seams you could see
straight down to the print bed, logo and all. Extrusions are now drawn slightly swollen sideways, so
neighbouring lines touch the way they do on a real part. Surfaces read as solid.

**The light comes from above now.** The studio lighting RealColor uses was built around the wrong "up"
axis, so the sky of its environment fell sideways instead of overhead. Ambient light and reflections
were arriving from the wrong place. They now come from above the bed, like they should.

**Not everything reflects the same any more.** An ironed top, an external wall, a bridge and a support
are four different finishes on a real print, and RealColor now shades them as four different finishes
instead of one uniform plastic. The first layer gets its own treatment too — squashed flat against the
build plate, it comes out nearly glossy.

**And the whole image has a bit more life in it.** With the light coming from the right place and each
surface reflecting on its own terms, the picture could take slightly more contrast rather than less —
so it now gets it, and the filament colours are left at full strength.

**Changing the view angle no longer stalls.** RealColor recomputes its composite whenever the camera
moves, which is expensive on a complex model. Dragging to rotate already knew to draw cheaply while
you moved — but the mouse wheel and the view cube did not, so zooming or jumping to a top/front/side
view could lock the preview up for a moment on a heavy plate. They now behave like dragging does: the
view stays responsive while you move, and the full-quality image comes back a moment after you stop.

---

### It no longer shares a settings folder with the official Snapmaker Orca

If you had both this build and the official Snapmaker Orca installed, they were quietly using the same
folder for everything: the same settings file, the same custom filaments, the same printer and process
presets. Neither app knew the other existed. Whichever one you closed last wrote its version of your
settings over the other's. People hit this as filaments that vanished, presets that reverted to
something they had not chosen, or preferences that would not stay put — and the usual workaround was
to give up on the installer and run the portable build instead.

This build keeps its own folder. Your two slicers stop overwriting each other and each remembers what
you told it.

**You do not have to move anything by hand.** The first time you launch it, your existing settings,
presets and custom filaments are copied across from the old shared folder automatically, so you should
open it and find everything where you left it. The old folder is left exactly as it was — nothing is
deleted, the official Snapmaker Orca is unaffected, and going back to an older build of this one still
finds its data. Logs and caches are not copied; those rebuild themselves.

Two things worth knowing. Because the copy happens once, on first launch, any change you make in the
official Snapmaker Orca after that point stays over there — the folders are independent from now on.
And if you run the portable build, nothing changes for you at all: it keeps using the `data_dir` folder
sitting next to the executable, as it always has.

---

### The Windows installer stops treading on the official Snapmaker Orca

Several people reported that installing this build interfered with their official Snapmaker Orca
install. Four separate causes, all now fixed.

**Uninstalling deleted the official app's desktop shortcut.** The uninstaller removed a shortcut named
"Snapmaker Orca" — which is the official app's, not ours. It now only removes its own.

**Uninstalling could close the official app mid-print-preparation.** It shut down every running process
called `snapmaker-orca.exe`, and both apps ship an executable with that name. It now checks only the
one inside its own install folder, so the official app is left running.

**Uninstalling broke "open in slicer" web links for the official app.** The two apps share the
`snapmaker-orca://` and `Snapmaker_Orca://` link handlers, and our uninstaller deleted them outright,
so links stopped working even for people who had deliberately kept the official app. It now removes a
handler only if it is still pointing at our own program.

**Installing hijacked those same links.** We used to claim them unconditionally, so installing this
build silently redirected the official app's web links to us. We now register our own `neotkocm://`
handler, and claim the shared ones only when nothing else has. Incoming links of every flavour are
still accepted, so nothing stops working either way.

On top of that, the Windows file association for `.3mf` and `.stl` used an identifier shared
byte-for-byte with the official app, so whichever one you last ticked "associate files" in took the
other's associations with it. Each app now has its own, and they can coexist.

None of this affects macOS or Linux, and none of it changes slicing or G-code.

---

### Opening a G-code file works again

Dropping a `.gcode` file onto the window, or opening one from the menu, had become unreliable. The
toolpaths would flash up for a fraction of a second and vanish, leaving you staring at an empty
plate. Sometimes it was worse: the progress bar froze partway through "Generating geometry vertex
data", an error box appeared, and on some machines the application simply closed itself.

Which of the three you got depended, oddly, on how big the file was — small G-code files failed
hardest. That turned out to be the clue.

While it builds the preview, the slicer shows you a progress bar. Updating that bar lets the
application stay responsive, which also means it goes off and deals with whatever else is waiting to
be done. One of those pending jobs decided the plate looked empty — which it technically is, since a
G-code file has no model behind it — and helpfully cleared the toolpaths. It was clearing them
*while they were still being built*. With a large file the interruption arrived after the work had
finished and you just lost the picture; with a small one it landed in the middle and took the whole
load down with it.

Loading a G-code file now holds everything else off until it is done, so nothing can pull the floor
out from under it, and the "empty plate" cleanup knows the difference between a plate with nothing on
it and a plate showing a G-code file.

Two smaller things were fixed alongside it. Opening a G-code file no longer needlessly invalidates
your slice — previously it could quietly throw away a perfectly good sliced result and force a
re-slice. And a G-code file that uses more extruders than your current printer profile has
configured no longer aborts the load; the cost estimate simply skips the filaments it knows nothing
about.

If a G-code file does fail to load now, you get a message saying what went wrong instead of the
application disappearing.

---

## Notes

- **The Snapmaker Orca base moves from 2.3.4 to 2.3.5.** No upstream code changed in this build — every
  patch from Snapmaker's 2.3.5 was already integrated in earlier releases; only the version number
  itself had been held back. The About box and the installer now report 2.3.5.
- **This release changes generated G-code** for multi-colour prints that use the purge tower — see
  the purge tower fix above. Everything else is identical to 2.4.1.
- **Photo Mode is presentation only.** It does not touch the model, the settings or the G-code, and
  it is gated behind Libre Mode.
- **The preview's range slider is a viewing control.** It changes what you see, never what is sliced
  or printed, and at its default position it behaves exactly as the single-handle slider did.
- **The RealColor changes are cosmetic.** They alter how the preview is drawn and nothing else — not
  the model, not the settings, not the G-code. It also remains what it always was: an approximated
  optical simulation, and only as accurate as the TD values you gave each filament.
