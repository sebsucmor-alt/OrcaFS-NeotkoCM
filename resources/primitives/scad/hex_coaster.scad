// Honeycomb coaster — Primitive Generator example (Neotko fork)

// Coaster diameter
diameter = 95; // [40:200]
// Thickness
thickness = 4; // [1.2:0.2:10]
// Rim width
rim = 4; // [1:15]
// Honeycomb cell size (across flats)
cell = 9; // [3:30]
// Wall between cells
cell_wall = 1.6; // [0.8:0.2:5]
// Solid floor under the honeycomb
floor_thickness = 0.8; // [0:0.2:3]

/* [Hidden] */
$fn = 96;

module honeycomb(r) {
    dx = cell + cell_wall;
    dy = dx * sqrt(3) / 2;
    n = ceil(r / dy) + 1;
    for (j = [-n : n], i = [-n : n])
        translate([i * dx + (j % 2) * dx / 2, j * dy, 0])
            cylinder(d = cell / cos(30), h = thickness + 2, $fn = 6);
}

difference() {
    cylinder(d = diameter, h = thickness);
    translate([0, 0, floor_thickness])
        intersection() {
            cylinder(d = diameter - 2 * rim, h = thickness);
            honeycomb(diameter / 2);
        }
}
