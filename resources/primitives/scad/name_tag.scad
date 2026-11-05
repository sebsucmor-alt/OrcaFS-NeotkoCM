// Name tag keychain — Primitive Generator example (Neotko fork)
// Two pieces with their own colour: the tag and the letters.
// The "part" choice is the usual multi-colour convention: the Primitive Generator draws each
// piece on its own and adds them as parts, each on the filament closest to its colour.

// The text
label = "NEOTKO";
// Letter size
size = 10; // [5:30]
// Tag thickness
base = 2; // [1:0.2:5]
// Letter height above the tag
emboss = 1.2; // [0.2:0.2:4]
// Space around the text
margin = 3; // [1:10]
// Keyring hole
hole = true;
// Hole diameter
hole_d = 5; // [3:10]
// Which piece to draw
part = "all"; // [all, tag:Tag, letters:Letters]

/* [Hidden] */
$fn = 48;
text_w = max(1, len(label)) * size * 0.8; // the room the text gets; resize() makes it fit exactly
w = text_w + 2 * margin;
h = size + 2 * margin;

module tag() {
    difference() {
        linear_extrude(base)
            difference() {
                hull() {
                    translate([h / 2, 0]) circle(d = h);
                    translate([w - h / 2 + (hole ? hole_d + margin : 0), 0]) circle(d = h);
                }
                if (hole) translate([w + hole_d / 2, 0]) circle(d = hole_d);
            }
    }
}

module letters() {
    translate([margin, 0, base])
        linear_extrude(emboss)
            resize([text_w, 0], auto = [false, true])
                text(label, size = size, valign = "center", font = "Liberation Sans:style=Bold");
}

if (part == "all" || part == "tag")     color("gold")  tag();
if (part == "all" || part == "letters") color("black") letters();
