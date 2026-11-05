// Box with lid — Primitive Generator example (Neotko fork)
// Prints as two pieces side by side: the box and its lid.

/* [Size] */
// Inside width
width = 60; // [10:200]
// Inside depth
depth = 40; // [10:200]
// Inside height
height = 30; // [5:150]

/* [Walls] */
// Wall thickness
wall = 1.6; // [0.8:0.2:5]
// Corner radius
corner = 4; // [0:20]
// Gap between lid and box, for the fit
fit = 0.2; // [0:0.05:0.6]
// Lid lip height
lip = 4; // [2:15]

/* [Hidden] */
$fn = 48;

module rounded(w, d, h, r) {
    r2 = max(0.01, min(r, w / 2 - 0.01, d / 2 - 0.01));
    linear_extrude(h)
        offset(r = r2) square([w - 2 * r2, d - 2 * r2], center = true);
}

module box() {
    difference() {
        rounded(width + 2 * wall, depth + 2 * wall, height + wall, corner + wall);
        translate([0, 0, wall]) rounded(width, depth, height + 1, corner);
    }
}

module lid() {
    rounded(width + 2 * wall, depth + 2 * wall, wall, corner + wall);
    translate([0, 0, wall - 0.01])
    difference() {
        rounded(width - 2 * fit, depth - 2 * fit, lip, corner - fit);
        translate([0, 0, -1]) rounded(width - 2 * fit - 2 * wall, depth - 2 * fit - 2 * wall, lip + 2, corner - fit - wall);
    }
}

box();
translate([width + 2 * wall + 10, 0, 0]) lid();
