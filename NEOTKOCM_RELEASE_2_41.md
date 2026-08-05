## Proud to announce that @Snapmaker is officially sponsoring this project!!

Development is conducted in close collaboration with the Snapmaker ecosystem and with Radoux/Radu, author of FullSpectrum and now part of the Snapmaker team.
By Neotko — inventor of Ironing/Neosanding (Ultimaker Cura, PrusaSlicer)

---

# Neotko FullSpectrum 2.4.1 — on Snapmaker Orca 2.3.4 — Release Notes

> ⚠️ **Review your generated G-code before long or production prints, especially if you
> turn on anything marked **expert-only** below.**

**2.4.1 is an incremental release on top of 2.4.0** (see `NEOTKOCM_RELEASE_2_40.md` and earlier
notes for the full feature set). This notes file is a **running draft** — it grows as sessions land,
until this version ships. Everything remains **opt-in**: at defaults the build behaves like stock
Snapmaker Orca.

This release is entirely corrections. Nothing here adds a setting; every item is a case where the
slicer was producing wrong output, or quietly losing your work, and now does not.

---

## What's new in 2.4.1

### Your painted colours no longer lose their recipe

This is the important one, and it was reported from the field: a project that looks painted in the
3D view, slices without a single error, and comes out **completely plain**. On one machine it did
that silently; on another it crashed outright.

The cause is a split that existed since the painter was built. When you paint a surface, two
separate things are stored:

- **the paint itself** — which triangles carry which slot — lives on the model, travels inside the
  3MF, and is covered by undo;
- **the recipe** — what that slot actually prints, the colours and the pass stack — lived in a
  global list that was **not** part of the model and **not** part of undo.

The paint only stores a number pointing at the recipe. If the recipe list lost that entry, the paint
was left pointing at nothing: still visible in the 3D view, completely dead at slice time, and
nothing anywhere told you.

That is exactly what happened. Working colours — the ones the painter creates on the fly, as opposed
to colours you deliberately save to the palette — are garbage-collected once no slot references them.
Two places ran that collection **immediately after clearing the slots they were about to check**:
"Erase all", and freeing a single slot. So the sequence

> paint a surface → erase it → undo

restored the paint but not its recipe, and the project was silently broken from that point on. Saving
it produced a 3MF that will not slice its painted pass on **any** machine, including the one that
made it. Colours saved to the palette were never affected, which is why re-doing the same paint with
a saved colour appeared to "fix" the file.

**What changed:**

- Neither "Erase all" nor "free a slot" collects working colours any more. Nothing leaks — the same
  collection still runs when you close the painter, by which point undo has had its chance to put the
  references back.
- **Saving now audits itself.** Before writing a 3MF, the slicer checks every painted slot in every
  volume of every object against the recipe list. If any paint points at a recipe that is not there,
  it says so loudly in the log instead of writing a broken file in silence.
- **Loading now recovers what it can.** Opening a 3MF whose paint has no recipe no longer leaves that
  paint invisible and unusable. A placeholder entry is created for each orphaned slot, so the slot
  shows up in the painter and can be **re-assigned** to a real recipe. The original recipe is gone —
  it was never written to the file — but your painted geometry is not, and you can put a colour back
  on it without repainting a single triangle.

If you have an old project that prints plain despite looking painted, open it, check the painter for
a slot named "Recovered paint", and assign it the colour you wanted. That is now a two-click repair
instead of starting over.

### The purge tower — five things it was getting wrong

All five are correctness bugs found by auditing the tower against its own G-code. All are verified
against the generated G-code, and one of them against a printed tower.

**The tower was up to 2.45× taller than it needed to be — but only on simple plates.** On a plate
where everything is painted, the tower came out at 51.6 mm where 20.8 mm was correct. The rule that
decides "this is the only thing happening at this height" was accepting *every* sublayer inside a
0.02 mm window instead of only the topmost one. Since the multi-pass engine places its sublayers
0.0002–0.0012 mm apart, they all qualified, each one pushed the height bookkeeping forward, and the
next purge was computed at a fifth of its proper height — five times the lines, five times the
plastic. The counter-intuitive part: **the cleaner your plate, the worse it got.** A single unpainted
cube, or one line of MMU paint at that height, masked the bug completely.

**A colour change could go through with no purge at all.** Switching from a PathBlend pass to a
ColorStitch pass sometimes skipped the tower entirely — the new colour started printing on your part
with the previous colour still in the nozzle. Two pieces of the tower code were asking the same
question ("is this the same layer?") with two different tolerances, 1e-5 against 1e-4. Any layer step
that fell between them desynchronised the tower's layer counter by one, and the purge was scheduled
for a layer that did not exist. It failed silently. The tolerances are now one shared constant, and a
permanent check reports it as an error if the two ever disagree again.

**Purges were being planned for the wrong tool.** On plates with several colour buckets at the same
height, the tower's plan and the actual G-code could disagree about which tool prints first — so the
tower purged for a tool that was not the one about to print. The sort that builds the plan was not a
stable sort, and its tie-breakers all matched in exactly this case, which meant the plan silently
inherited whatever order the input happened to be in. It is now anchored to the same order the
G-code emits, with a check that compares the two at every height.

**Layer heights were wrong with adaptive layer height.** The `;HEIGHT` comment — and the actual
extrusion behind it — could come out as low as 0.0105 mm. The function that clamps layer height to a
sane minimum applied that clamp on only one of its two return paths, so a poisoned value flowed
straight through. The real scope was **50 layers**, not the three that were first visible. Clamped on
both paths now, with a new check for under-height (the existing one only ever tested for
over-height).

**The tower purged twice into the same space where a painted pass met its own layer.** When a
painted pass sits just below a layer's nominal height, the pass fills nearly all of it — 0.1998 mm
of a 0.2 mm layer — and the object's own layer is left two microns thick. The tower has to visit
twice at that height, once for the pass and once for the layer, and both visits were laying a full
purge into the same two-micron gap **and over the same strip of the tower**, so the second one
printed straight onto the first. The mechanism that spreads such visits across different strips was
running correctly; it simply had nowhere to put the second one. The tower reserves its depth with a
rule that made each layer's box big enough to *contain* the pass's box, which is right when the two
sit at different heights, but these two share a height and can only sit side by side — so the
reservation had to be the *sum* of the two, not the larger of them. It is now the sum. A new
permanent check verifies that a pass and its layer never overlap, and reports it as an error if they
ever do. **Expect a deeper tower on plates with painted passes**: that extra depth is space the tower
was always using and never reserving, which is exactly why this bug existed.

**Log noise cleaned up.** Benign "identity" visits to the tower were being tagged as errors, which
masked the real ones. If you read the tower log, `← ERROR` now means an actually lost purge, and
nothing else.

### A painted pass on a single-colour object now gets a purge tower

If your object printed in one filament and you painted a Sandwich pass on it in a *different*
filament, no tower was generated at all. The tool changes still happened — so the painted pass
started with the previous colour still in the nozzle, and the object's own colour came back
contaminated afterwards. Nothing warned you. You had to look at the G-code to find out.

Two separate counts were each seeing half the picture:

- The check that decides "does this print need a tower?" counted only the tools used *inside* the
  painted passes. On a body printed in T3 with a pass painted in T0, every pass is T0, so it counted
  **one** tool and concluded there was no tool change. It never added the tool the body itself prints
  with. It now counts both, which is what "a tool change happens here" actually means. A pass painted
  in the *same* filament as the body still counts as one, and still gets no tower — correctly, since
  there is nothing to purge.
- Even with that fixed, the G-code exporter asks the tool ordering "is there a tower?", and that
  question only ever looked at the **first layer**. That is a safe assumption for a normal
  multi-filament print, where the first tool change happens near the bottom. A painted Sandwich
  breaks it: the object can be a single filament all the way up, with the first tool change appearing
  at 80% of the height. The first layer therefore had no tower, so the whole tower was declared
  absent and the entire NeoTower dispatch went inert. The first layer is now seeded when a later
  layer needs a tower, and the existing layer-continuity rule fills the column upward from there — a
  tower that only existed near the top would be floating in mid-air.

This does not create towers where none is needed: if nothing else asks for a tower, neither change
does anything.

---

## Known issues

- **The recipe list is still not part of undo.** This release closes the two paths that were
  destroying recipes, and adds a net at save and at load, but the underlying split — paint lives on
  the model, recipes live in a global list — is unchanged. Deliberately deleting a palette colour and
  then undoing will still leave paint without its recipe. You will now be told about it when you save,
  and the slot is recoverable when you reload, but the recipe itself is not restored. Making recipes
  travel with the volume (or putting the list in the undo stack) is the real fix and is not in this
  release.
- **The tower still reserves more depth than it uses** — roughly 59% of the reserved depth goes
  unconsumed. This is by design, not a defect: the reservation is speculative because the tower cannot
  know in advance which tool changes will actually be needed. Reclaiming it is a design decision, not
  a bug fix, and the naive version of it (reserving only for "firm" slots) has been tested and
  **rejected** — it produces purges outside the tower box and enormous extrusions.
- **The last tool change of a painted pass purges about a third less than the others.** Measured on a
  six-change plate: five changes at ~16.8 mm³ and the last one at **11.2 mm³**. The path that returns
  the tool to the object after the final painted pass takes its purge volume straight from
  `multipass_prime_volume` instead of going through the same routine as every other change, so it is
  the only one that never gets raised to the profile's `filament_minimal_purge_on_wipe_tower` floor.
  That exception was deliberate — it was added so this event would stop being the one that sized the
  tower footprint — but its premise no longer holds, because every sibling change is now pinned to
  that floor anyway. So it saves no footprint at all and only under-purges, on the change that goes
  back to print the visible top layer. Left as-is for this release rather than changed blind on the
  tower.
- **On a curved or chamfered top, `penultimate_top_layers` is not respected.** A sloped surface
  produces a thin top ring on *every* layer of the slope, and each of those rings claims its own
  penultimate layer — so a setting of 1 can still yield two painted penultimate passes under a domed
  top. Worse, those rings are usually so thin that the perimeters consume them entirely, meaning the
  "top" that spawned the extra penultimate is never printed as a top at all. Flat tops are unaffected.
- **Two spurious tower warnings.** The plan validator reports `V3: chain gap` between consecutive
  *real* tool-change events when a painted sublayer sits between them carrying the transition. The
  chain is intact; the check simply does not look at sublayers. Harmless, but it is the same shape as
  the mislabelled error this release cleaned up, so it will be taught about sublayers before it starts
  masking a real V3.

---

## Notes

- Nothing in this release adds or changes a setting. If you were not hitting any of these bugs, your
  output is unchanged.
- The paint-recipe fixes change what gets written into and read out of a 3MF, but only in the
  direction of writing more and warning more; existing projects load exactly as before unless they
  were already broken, in which case they now load recoverably instead of silently dead.
- The tower fixes **do** change G-code on plates that were hitting them: shorter towers on
  fully-painted plates, an extra purge where one was being skipped, and corrected layer heights under
  adaptive layer height.
- The biggest visible change is on **single-filament objects with a painted pass in another
  filament**: those plates had no tower at all and now grow one, with a purge at every tool change.
  Expect more time and more filament on those prints — that material was previously being deposited
  on your part instead of in a tower. If you print one of these, look the tower over layer by layer
  before committing to a long print: this is the first release where it exists at all for that case.
