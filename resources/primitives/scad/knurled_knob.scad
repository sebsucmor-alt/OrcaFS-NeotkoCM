// Knurled knob — Primitive Generator example (Neotko fork)

/* [Knob] */
// Knob diameter
diameter = 30; // [10:80]
// Knob height
height = 15; // [5:40]
// Number of grip ridges
ridges = 24; // [6:60]
// How deep the ridges go
ridge_depth = 1.2; // [0.4:0.1:4]

/* [Shaft] */
// Shaft hole
shaft = "D shaft"; // [D shaft, round, hex]
// Shaft diameter (hex: across flats)
shaft_d = 6.2; // [2:0.1:15]
// Hole depth
shaft_depth = 12; // [3:40]

/* [Hidden] */
$fn = 72;

module grip() {
    difference() {
        cylinder(d = diameter, h = height);
        for (i = [0 : ridges - 1])
            rotate([0, 0, i * 360 / ridges])
                translate([diameter / 2 + ridge_depth * 0.35, 0, -1])
                    cylinder(d = ridge_depth * 2, h = height + 2, $fn = 16);
        // soft top edge
        translate([0, 0, height - 1])
            difference() {
                cylinder(d = diameter + 2, h = 2);
                cylinder(d1 = diameter, d2 = diameter - 2, h = 1.01);
            }
    }
}

module hole() {
    translate([0, 0, -0.01]) {
        if (shaft == "hex")
            cylinder(d = shaft_d / cos(30), h = shaft_depth, $fn = 6);
        else if (shaft == "round")
            cylinder(d = shaft_d, h = shaft_depth);
        else
            difference() {
                cylinder(d = shaft_d, h = shaft_depth);
                translate([shaft_d / 2 - shaft_d * 0.15, -shaft_d, -1]) cube([shaft_d, 2 * shaft_d, shaft_depth + 2]);
            }
    }
}

difference() { grip(); hole(); }
