// Sharp PC-1500 / PC-1600 memory slot cover
// Parametric rebuild of the Tinkercad design "Sharp PC-1500/1600 memory slot cover 52.7mm".
// All dimensions in mm.
//
// Axes (same as in Tinkercad, ruler at the corner of the large plate):
//   X = width  (40.9 side)
//   Y = length (52.7 side); latch near Y = 0, clips at the far end
//   Z = height; print orientation, small plate on the bed
// Mounted, the cover is turned over: clips and latch go into the calculator,
// and the small plate faces outward.

/* [Large plate (top in print orientation)] */
plate_w = 40.9;
plate_l = 52.7;
plate_t = 1.0;

/* [Small plate (bottom in print orientation, centered)] */
small_w = 39.6;
small_l = 51.6;
small_t = 0.5;

/* [Clips at the far end] */
clip_w        = 2.7;   // along X
clip_l        = 3.9;   // along Y, total length
clip_t        = 1.3;   // thickness
clip_overhang = 1.5;   // how far the clips stick out past the plate edge
clip_inset    = 0.1;   // distance from the long sides of the large plate

/* [Latch] */
latch_w       = 4.0;   // along X, centered
latch_y       = 3.2;   // distance of the stem's inner face from the plate edge
stem_t        = 1.5;   // stem thickness along Y
stem_h        = 7.0;   // stem height above the large plate
slope_base    = 1.0;   // slope wedge: extra thickness at the base, tapering to 0 at the top
barb_depth    = 1.1;   // barb: how far it sticks out toward the plate edge
barb_h        = 1.7;   // barb: height of its vertical face

/* [Fingernail groove (in front of the latch)] */
groove_w = 15.0;  // along X, centered
groove_l = 1.6;   // in from the plate edge, along Y
groove_d = 0.4;   // depth below the top of the large plate

eps = 0.01;

top_z = small_t + plate_t;  // top surface of the large plate

// Small plate, centered under the large plate
translate([(plate_w - small_w) / 2, (plate_l - small_l) / 2, 0])
    cube([small_w, small_l, small_t]);

// Large plate with the fingernail groove
difference() {
    translate([0, 0, small_t])
        cube([plate_w, plate_l, plate_t]);
    translate([(plate_w - groove_w) / 2, -eps, top_z - groove_d])
        cube([groove_w, groove_l + eps, groove_d + eps]);
}

// Clips
for (x = [clip_inset, plate_w - clip_inset - clip_w])
    translate([x, plate_l + clip_overhang - clip_l, top_z])
        cube([clip_w, clip_l, clip_t]);

// Latch: stem, slope wedge and barb as one profile in the Y-Z plane,
// extruded along X. multmatrix maps (a, b, c) -> (c + x0, a, b).
latch_profile = [
    [latch_y,                       top_z],
    [latch_y + stem_t + slope_base, top_z],
    [latch_y + stem_t,              top_z + stem_h],
    [latch_y - barb_depth,          top_z + stem_h],
    [latch_y,                       top_z + stem_h - barb_h],
];
multmatrix([[0, 0, 1, (plate_w - latch_w) / 2],
            [1, 0, 0, 0],
            [0, 1, 0, 0]])
    linear_extrude(height = latch_w)
        polygon(latch_profile);
