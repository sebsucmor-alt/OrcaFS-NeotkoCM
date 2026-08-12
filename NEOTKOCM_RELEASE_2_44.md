## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.4.4 — on Snapmaker Orca 2.3.5 — Release Notes (draft, in progress)

> ⚠️ **Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.**

**2.4.4 is an incremental release on top of 2.4.3** (see `NEOTKOCM_RELEASE_2_43.md` and earlier
notes for the full feature set). This notes file is a **running draft** — it grows as sessions land,
until this version ships. Everything remains **opt-in**: at defaults the build behaves like stock
Snapmaker Orca.

---

## What's new in 2.4.4

### Photo Studio comes to the G-code viewer

**Where**: G-code **Preview** → the view-type dropdown → **RealColor** → **Photo mode…** in the legend.

RealColor already got close enough to a real photograph of a printed part that the obvious next
question was why you still had to go back to the Prepare tab to light one. Now you don't. The
lighting of the G-code view is no longer welded into the shader: the key and fill lights can be
aimed, and the part is lit in the room rather than by a fixed lamp bolted to the camera.

![The RealColor G-code view of a printed hotel key tag while the key light is swung around it: the lit and shaded sides of the raised lettering trade places, the sheen along the extrusion lines sweeps across the surface, and the shadows cast by the letters onto the face swing with the light](docs/images/RealColor-Photo.gif)

**The lettering casts shadows onto its own face.** Until now nothing in RealColor knew where the
light was coming from — raised text read as *sunken* rather than as lit from a direction, because
the only shading of that kind was ambient. The object's own mesh is now used to cast a real shadow
onto the printed surface, so embossed text, engraving and overhangs land the way they do on the bed.
Supports, brim and the wipe tower are not part of that mesh and do not cast.

Photo mode is shared with the one in the Prepare tab and turns itself off when you leave the tab it
was opened from, so you never come back to a stripped scene you didn't ask for. Filament finish is
**not** duplicated here — the existing *Filament finish…* window is still the one place that decides
how each material behaves optically.

**And the picture comes out.** *Save PNG…* and *Copy image*, at 1080p, 1440p or 2160p, over a pure
white or a fully transparent background — no bed, no plate, no grid, just the print.

**The cast shadow is carried in the transparency.** Paste the PNG onto any colour and the part still
sits on a soft shadow instead of floating on it. That is the difference between a cutout and a
product shot, and it costs nothing to use: it is the same shadow already on screen.

Two things about the export are worth knowing, because both were found the hard way and both are
invisible until you compare files side by side. The picture is rendered at twice the resolution and
averaged down, which is what keeps the joints between extrusion lines from showing as speckle on a
white background — the dark viewport was hiding them all along. And the screen-space effects are
rescaled for the export, so the photo is not flatter than what you were looking at.

> Export from the **Prepare** tab's Photo Mode is still parked: it frames the shot wrongly and a
> button that writes a broken file is worse than no button. Use *Hide UI for screenshot* there, or
> the G-code viewer's Photo Mode, which does export properly.

### RealColor works past four filaments

**Where**: G-code **Preview** → **RealColor**.

Loading more than four filaments made tools 5 and up render in arbitrary colours, or simply black.
Every colour and TD table in this view was sized for four — the tool count of the machine it was
written for — and the shader read straight past the end of them.

The limit is now sixteen. This matters for two things the view is genuinely good at and could not
do before: **previewing someone else's G-code**, which can arrive with any number of tools, and
**designing with more colours than you intend to print**, keeping the spare tools around until you
decide which ones survive.

---

## Notes

- Photo mode and the G-code viewer's lighting are **opt-in**: with the mode closed, RealColor renders
  exactly as it did in 2.4.3.
- The lighting knobs live in Photo Mode's own window, **not** in RealColor's tuning panel. That panel
  holds values calibrated for colour accuracy and then frozen; mixing a "make it look nicer" control
  in among them is how a colour simulation quietly stops being one.
- Shadows in the G-code viewer are cast by the **object mesh**, so they follow the shape you designed
  rather than the toolpath. Supports, brim and the wipe tower do not cast.

