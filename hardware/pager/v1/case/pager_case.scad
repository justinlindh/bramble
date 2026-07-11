// ============================================================================
// Bramble Pager v1 - 3D printable enclosure (parametric, OpenSCAD)
// ----------------------------------------------------------------------------
// A 90s-pager formfactor case for the Bramble Pager v1 PCB (96 x 50 x 1.6 mm).
// Two printed shells (back tub + front cover) closed by 3x M2 self-tapping
// screws that also clamp the PCB, plus printed captive plunger buttons and a
// snap-on belt clip.
//
// COORDINATE SYSTEM
//   The PCB is specified in "board-local" coords: origin at the board top-left
//   corner, x right (0..96), y DOWN (0..50). This model uses the same X, but
//   flips Y so that model +Y is physical "up" (the LoRa/top wall). Every board
//   point is converted with mb(bx, by) = [bx, BOARD_H - by].
//     - board y=0  (top wall, LoRa flex antenna)   -> model Y = BOARD_H (top)
//     - board y=50 (bottom wall, USB-C)             -> model Y = 0     (bottom)
//   Z is the stack axis: Z=0 is the outer back face, +Z points toward the
//   display/front. Everything is built in this frame (no global centering) so
//   the datasheet coordinates drop straight in.
//
// PART SELECTOR: set `part` to one of
//   "back"     - back shell / tub (floor, walls, bosses, battery bay, motor)
//   "front"    - front cover (window, module deck, button holes, GPS pocket)
//   "plunger"  - one captive plunger button (print 3)
//   "clip"     - snap-on belt clip
//   "assembly" - everything positioned together (visual check only, do not print)
// ============================================================================

part = "assembly";   // "back" | "front" | "plunger" | "clip" | "assembly"

DBG_GPS = true;      // debug flags (leave true for production)
DBG_COLLAR = true;

$fn = 48;
eps = 0.01;          // small overlap to keep CSG manifold

// ============================================================================
// 1. PRIMARY PARAMETERS  (edit these to re-fit a revised board)
// ============================================================================

// ---- Board ----
BOARD_W      = 96;     // board X extent (mm)
BOARD_H      = 50;     // board Y extent (mm)
BOARD_TH     = 1.6;    // board thickness
BOARD_R      = 2;      // board corner radius (informational)
PCB_HOLE_D   = 2.2;    // drilled M2 mounting hole in the PCB (clearance for pin)

// ---- Fit / print tolerances (PETG) ----
WALL         = 2.0;    // nominal wall thickness
FLOOR        = 1.8;    // back-shell floor thickness
FIT_CLR      = 0.25;   // sliding-fit clearance for mating printed features
SIDE_CLR     = 0.4;    // gap between board edge and inner wall face

// ---- Rounded corners ----
R_OUT        = 3.5;    // outer vertical-edge radius (spec: >= 3)
R_IN         = 1.5;    // inner cavity vertical-edge radius

// ---- Z stack (back-to-front), all measured from the outer back face Z=0 ----
BAT_H        = 6.5;    // 603450 pouch + foam thickness (lies under the PCB)
BAT_GAP      = 1.0;    // clearance between cell top and PCB bottom (LiPo swell margin)
GLASS_GAP    = 6.0;    // e-paper glass plane above the PCB top. Raised 4.0 -> 6.0 so a
                       // mated MHF1 u.FL (ANT1, board 26,17.5, under the panel) clears
                       // the module underside. Clearance PCB-top to module underside =
                       // GLASS_GAP - MODULE_TH = 4.95mm (a mated u.FL/MHF1 is ~2.5mm, up
                       // to ~4mm with a right-angle coax exit; the coax leaves laterally
                       // through the deck relief, so the vertical stack stays small).
FC_TH        = 2.0;    // front-cover face plate thickness (over the window)

// Derived Z levels ----------------------------------------------------------
z_floor_top  = FLOOR;                       // 1.8  interior floor (battery bay)
z_pcb_bot    = FLOOR + BAT_H + BAT_GAP;      // 9.3  PCB underside (boss tops)
z_pcb_top    = z_pcb_bot + BOARD_TH;         // 10.9 PCB top surface
z_part       = z_pcb_top;                    // 10.9 shell parting plane
z_ceil       = z_pcb_top + GLASS_GAP;        // 16.9 front-cover inner ceiling = glass plane
z_front_out  = z_ceil + FC_TH;              // 18.9 front outer face (<= 19 target)

// ---- Exterior / interior footprint (model coords) ----
int_min = [ -SIDE_CLR,            -SIDE_CLR ];             // [-0.4,-0.4]
int_max = [ BOARD_W + SIDE_CLR,   BOARD_H + SIDE_CLR ];    // [96.4,50.4]
ext_min = [ int_min[0] - WALL,    int_min[1] - WALL ];     // [-2.4,-2.4]
ext_max = [ int_max[0] + WALL,    int_max[1] + WALL ];     // [98.4,52.4]
EXT_W   = ext_max[0] - ext_min[0];   // 100.8
EXT_H   = ext_max[1] - ext_min[1];   // 54.8

// ---- Board -> model coordinate helper ----
function mb(bx, by) = [ bx, BOARD_H - by ];

// ---- Mounting holes (board coords) -> model ----
H_TL = mb(25, 4);    // [25,46]  under the panel: PCB boss + locating pin only (NO screw)
H_TR = mb(92, 4);    // [92,46]  screwed
H_BL = mb(10, 47);   // [10,3]   screwed
H_BR = mb(92, 46);   // [92,4]   screwed
SCREW_HOLES = [ H_TR, H_BL, H_BR ];   // 3-screw shell closure (see report)
PIN_ONLY    = [ H_TL ];               // PCB support without a screw

// ---- Boss / screw geometry ----
BOSS_OD      = 5.0;
PIN_D        = 1.8;    // locating pin into PCB_HOLE_D
PIN_UP       = 1.4;    // pin protrusion above the PCB top (screwed holes)
PIN_UP_PANEL = 0.8;    // shorter protrusion under the panel (H_TL) so the pin stays
                       // >=1mm clear of the module underside; see the pin loop note
SCREW_BORE   = 2.4;    // clearance bore through the back boss (M2 shank)
SCREW_PILOT  = 1.5;    // self-tap pilot in the front post
HEAD_CB_D    = 4.2;    // countersink/counterbore for the screw head in the floor
HEAD_CB_H    = 1.6;
POST_OD      = 6.0;     // front-cover screw post

// ---- Buttons (board coords -> model) ----
SW_BODY      = 5.1;    // switch body footprint (square)
SW_H         = 1.5;    // switch body height above PCB
SW_TRAVEL    = 0.25;   // max switch travel; plunger hard-stops at this
BTN_HOLE_D   = 4.0;    // plunger nub bore through the front face
BTN_NUB_D    = 3.6;    // plunger nub (slides in BTN_HOLE_D)
BTN_FLANGE_D = 5.2;    // retention flange under the face (> BTN_HOLE_D)
BTN_SKIRT_D  = 7.4;    // down-stop skirt OD (lands on bare PCB around the switch)
BTN_PROUD    = 1.0;    // nub protrusion above the outer face
btn_pts = [ mb(36,37.5), mb(48,37.5), mb(60,37.5) ];  // BOOT, DOWN, UP (rev B: DOWN=SW403 middle, UP=SW404 right)

// ---- Other front-face openings (board coords -> model) ----
RESET_PT   = mb(79, 46.3);   RESET_D  = 1.2;
LED_PT     = mb(48, 34);     LED_D    = 2.0;
CHG_PTS    = [ mb(24.5,48.5), mb(31.5,48.5) ];  CHG_D = 1.5;
BUZZ_PT    = mb(71.5, 35.5); BUZZ_D  = 1.0;   BUZZ_PITCH = 2.2;  // 3x hole grid

// ---- Display panel / window ----
PANEL_P0 = mb(22, 30.2);   // -> [22,19.8]  (panel zone min corner)
PANEL_P1 = mb(81.2, 1);    // -> [81.2,49]  (panel zone max corner)
MODULE_TH   = 1.05;        // GDEY0213B74 module thickness
MODULE_CLR  = 0.3;         // pocket clearance around the module outline

// GDEY0213B74 active area vs the 59.20 x 29.20 module glass (datasheet p.7 drawing):
//   long axis  A.A 48.55, glass margins 6.05 (FPC-tail side) / 4.60 (far side)
//              -> active-area center is +0.725 off the module center toward the far edge
//   short axis A.A 23.70, glass margins 2.75 / 2.75 -> centered (0 offset)
// The FPC tail exits a short edge of the glass; the tail-side long margin is the
// larger one. The window is centered on the ACTIVE AREA (not the module outline) and
// grown WIN_REVEAL past the active edge so every active pixel shows with an even glass
// reveal, while the cover still overlaps >=1mm of glass on all sides (asserted below).
// ACT_OFF sign: +X puts the larger (tail-side) margin on the low-X short edge. The
// offset is small (0.725mm) and the cover overlap stays >=1.75mm whichever short edge
// the tail roots at, so a sign flip here is a cosmetic one-liner.
ACTIVE_W    = 48.55;
ACTIVE_H    = 23.70;
ACT_OFF     = [0.725, 0];   // active-area center offset from the module center
WIN_REVEAL  = 1.0;          // glass revealed around the active area (all pixels shown)
ACT_CTR = [ (PANEL_P0[0]+PANEL_P1[0])/2 + ACT_OFF[0],
            (PANEL_P0[1]+PANEL_P1[1])/2 + ACT_OFF[1] ];
WIN_P0  = [ ACT_CTR[0] - ACTIVE_W/2 - WIN_REVEAL, ACT_CTR[1] - ACTIVE_H/2 - WIN_REVEAL ];
WIN_P1  = [ ACT_CTR[0] + ACTIVE_W/2 + WIN_REVEAL, ACT_CTR[1] + ACTIVE_H/2 + WIN_REVEAL ];

// ---- USB-C (bottom wall, board y=50 -> model Y=0 wall) ----
USB_CX      = 48;          // centered on X
USB_SHELL_W = 8.94;
USB_SHELL_H = 3.26;
USB_CLR     = 0.3;         // clearance all around
USB_VC      = z_pcb_top + 1.65;   // opening vertical center above PCB top
USB_THIN    = 1.2;         // locally thinned wall at the port
USB_CHAMF   = 1.4;         // outward 45-deg chamfer depth

// ---- LoRa flex antenna (inner face of the top wall, board y=0 -> model Y max) ----
LORA_LEN    = 79;          // keep this length of the top inner wall flat / rib-free
UFL_LORA    = mb(26, 17.5);   // -> [26,32.5] u.FL landing (routing clearance)

// ---- WROOM antenna overhang pocket (left wall, x=0) ----
// The WROOM antenna tab protrudes 3.0mm past the board edge (U401 tip at board
// x=97.0, measured from the routed layout), riding ON the PCB top plane: the
// tab therefore lives in the FRONT cover's wall zone (z_part..), and the back
// shell only needs the relief for seating margin. The pocket (2.8) is deeper
// than the wall (2.0), so both shells bulge outward locally over the window:
// without the bulge the relief is a through slot, not a pocket.
WROOM_POCKET_LEN = 28;     // along Y
WROOM_POCKET_DEP = 3.4;    // relief depth past the inner wall face (tip clr 0.8mm)
WROOM_POCKET_YC  = 25;     // model Y center of the pocket
WROOM_BULGE_SKIN = 1.2;    // printed skin kept outside the relief
WROOM_OVERHANG   = 3.0;    // antenna tip past board edge (layout-measured)
WROOM_BULGE_Y0   = WROOM_POCKET_YC - WROOM_POCKET_LEN/2 - 2;  // window + shoulders
WROOM_BULGE_LEN  = WROOM_POCKET_LEN + 4;

// ---- GPS active-antenna pocket (front cover, top-right) ----
// The patch is a 12x12x4mm active ceramic GPS antenna. The pocket's left retaining
// wall must clear the e-paper module deck frame, whose outer edge sits at model
// X = PANEL_P1[0]+1.6 = 82.8, so GPS_POCKET_P0[0] >= 82.8. The right edge is nudged
// out to X97.3 (still 1.1mm inside the outer shell) so the 1.0mm walls leave a
// 12.5 x 13.4mm interior -- enough for the 12mm patch with 0.25mm clearance.
GPS_ANT     = [12, 12, 4];  // 12mm active ceramic patch (WxDxH)
GPS_ANT_CLR = 0.25;
GPS_SKIN    = 1.5;          // sky-facing plastic thickness over the patch
GPS_POCKET_P0 = [82.8, 27.5];
GPS_POCKET_P1 = [97.3, 42.5];
GPS_WALL      = 1.0;       // retaining-wall thickness around the patch pocket
// With the 6mm deck the front cavity is deep enough to hold the 4mm patch under the
// face plate, so the GPS bump now reaches the front face (no protrusion, no trapped
// void, no floating skin ledge). GPS_SKIN sets the RF-transparent thickness over it.
z_gps_out   = z_front_out;   // GPS bump top flush with the front outer face (18.9)
UFL_GPS     = mb(92.5, 30.8);  // -> [92.5,19.2]

// ---- Vibration motor (back shell) ----
MOTOR_D     = 10.0;
MOTOR_TH    = 2.7;
MOTOR_PT    = mb(92, 11);   // -> [92,39]
MOTOR_POCKET_CLR = 0.4;

// ---- Buzzer sound port already covered by BUZZ_* above ----

// ---- Battery bay (model coords, 62 x 34 lying under the PCB, left of center) ----
BAT_P0 = [ 4,  8 ];
BAT_P1 = [ 66, 42 ];
BAT_RIB_H = 2.0;    // low retaining ribs around the cell
BAT_WIRE_PT = mb(90.5, 38);  // -> [90.5,12] BATT1, wires exit +x then route to bay

// ---- Shell joining lip (front cover flange that plugs into the tub) ----
LIP_H    = 3.0;
LIP_TH   = 1.2;

// ---- Belt clip (snap-on, rides a T-rail on the back exterior) ----
CLIP_RAIL_YC   = 26;    // model Y center of the vertical rail
CLIP_RAIL_LEN  = 34;
CLIP_STEM_W    = 4.0;
CLIP_HEAD_W    = 8.0;
CLIP_STEM_H    = 2.0;
CLIP_HEAD_H    = 1.6;
CLIP_LEN       = 58;
CLIP_W         = 16;

// ---- Cosmetic recessed label area on the back exterior ----
LABEL_DEPTH = 0.6;

// ============================================================================
// 2. SANITY CHECKS
// ============================================================================
stack_h  = BAT_H + BAT_GAP + BOARD_TH + GLASS_GAP;   // floor-top to glass
cavity_h = z_ceil - z_floor_top;
win_w    = WIN_P1[0]-WIN_P0[0];
win_h    = WIN_P1[1]-WIN_P0[1];
// Cover-bezel overlap of the module glass on each window side.
cover_lo_x = WIN_P0[0] - PANEL_P0[0];
cover_hi_x = PANEL_P1[0] - WIN_P1[0];
cover_lo_y = WIN_P0[1] - PANEL_P0[1];
cover_hi_y = PANEL_P1[1] - WIN_P1[1];
cover_min  = min(cover_lo_x, cover_hi_x, cover_lo_y, cover_hi_y);
// GPS wall vs module pocket: pocket void right edge is at PANEL_P1[0]+MODULE_CLR.
gps_pocket_clr = GPS_POCKET_P0[0] - (PANEL_P1[0] + MODULE_CLR);
// LoRa u.FL under-panel clearance: PCB top to the e-paper module underside.
ufl_clr = GLASS_GAP - MODULE_TH;

echo(str("EXTERNAL  W x H x T = ", EXT_W, " x ", EXT_H, " x ", z_front_out, " mm"));
echo(str("CAVITY height (floor->ceiling) = ", cavity_h, " mm ; stack = ", stack_h,
         " ; BAT swell slack = ", BAT_GAP));
echo(str("WINDOW = ", win_w, " x ", win_h, " mm ; active = ", ACTIVE_W, " x ", ACTIVE_H,
         " ; cover overlap min = ", cover_min, " mm"));
echo(str("GPS pocket-to-module clearance = ", gps_pocket_clr, " mm"));
echo(str("WROOM antenna tip-to-pocket clearance = ",
         (WROOM_POCKET_DEP + (0 - int_min[0])) - WROOM_OVERHANG,
         " mm ; bulge skin = ", WROOM_BULGE_SKIN, " mm (must both be > 0)"));
echo(str("LoRa u.FL under-panel clearance (PCB top -> module underside) = ", ufl_clr, " mm"));
assert(z_front_out <= 19, "external thickness exceeds 19mm target");
assert(cavity_h + eps >= stack_h, "internal height < component stack");
assert(win_w + eps >= ACTIVE_W && win_h + eps >= ACTIVE_H, "window smaller than active area");
assert(cover_min + eps >= 1.0, "cover bezel overlaps <1mm of glass on some window side");
assert(gps_pocket_clr + eps >= 0, "GPS pocket wall overlaps the e-paper module pocket");

// ============================================================================
// 3. GEOMETRY HELPERS
// ============================================================================

// Rounded-vertical-edge box from min corner p0 to max corner p1, Z range z0..z1.
module rbox(p0, p1, z0, z1, r) {
    w = p1[0]-p0[0];
    d = p1[1]-p0[1];
    translate([p0[0]+r, p0[1]+r, z0])
        linear_extrude(z1 - z0)
            offset(r=r) square([w - 2*r, d - 2*r]);
}

// Grid of small holes (buzzer). n across at given pitch, centered on pt.
module hole_row(pt, n, pitch, d, z0, z1) {
    for (i = [0:n-1])
        translate([pt[0] + (i-(n-1)/2)*pitch, pt[1], z0])
            cylinder(d=d, h=z1-z0);
}

// ============================================================================
// 4. BACK SHELL (tub)
// ============================================================================
module back_shell() {
    difference() {
        union() {
            // ---- Outer tub: solid block, hollowed to leave floor + 4 walls ----
            difference() {
                union() {
                    rbox(ext_min, ext_max, 0, z_part, R_OUT);
                    // WROOM bulge: local outward thickening so the relief keeps
                    // a closed skin (pocket back face sits past the outer face)
                    translate([int_min[0] - WROOM_POCKET_DEP - WROOM_BULGE_SKIN,
                               WROOM_BULGE_Y0, 0])
                        cube([WROOM_POCKET_DEP + WROOM_BULGE_SKIN + (int_min[0] - ext_min[0]),
                              WROOM_BULGE_LEN, z_part]);
                }
                // interior cavity (keep the floor below z_floor_top)
                rbox(int_min, int_max, z_floor_top, z_part + eps, R_IN);
                // WROOM antenna overhang relief: deepen the left inner wall locally
                translate([int_min[0] - WROOM_POCKET_DEP, WROOM_POCKET_YC - WROOM_POCKET_LEN/2, z_floor_top])
                    cube([WROOM_POCKET_DEP + eps, WROOM_POCKET_LEN, z_part - z_floor_top + eps]);
            }

            // ---- PCB support bosses (all four holes) ----
            for (h = concat(SCREW_HOLES, PIN_ONLY))
                translate([h[0], h[1], z_floor_top])
                    cylinder(d=BOSS_OD, h=z_pcb_bot - z_floor_top);   // top at PCB underside

            // ---- Locating pins into the PCB holes ----
            // Screwed holes get the full PIN_UP protrusion. H_TL sits under the panel,
            // so it uses the shorter PIN_UP_PANEL to keep the pin tip >=1mm below the
            // module underside (module pocket floor at z_ceil-MODULE_TH-1.0).
            for (h = SCREW_HOLES)
                translate([h[0], h[1], z_pcb_bot])
                    cylinder(d=PIN_D, h=BOARD_TH + PIN_UP);
            for (h = PIN_ONLY)
                translate([h[0], h[1], z_pcb_bot])
                    cylinder(d=PIN_D, h=BOARD_TH + PIN_UP_PANEL);

            // ---- Battery bay retaining ribs (low walls that cradle the cell) ----
            difference() {
                rbox([BAT_P0[0]-BAT_RIB_H, BAT_P0[1]-BAT_RIB_H],
                     [BAT_P1[0]+BAT_RIB_H, BAT_P1[1]+BAT_RIB_H],
                     z_floor_top, z_floor_top + BAT_RIB_H, 1);
                rbox(BAT_P0, BAT_P1, z_floor_top - eps, z_floor_top + BAT_RIB_H + eps, 1);
            }

            // ---- Vibration-motor retaining ring (glued to the shell floor) ----
            difference() {
                translate([MOTOR_PT[0], MOTOR_PT[1], z_floor_top])
                    cylinder(d=MOTOR_D + 2*(MOTOR_POCKET_CLR+1.2), h=MOTOR_TH + 0.6);
                translate([MOTOR_PT[0], MOTOR_PT[1], z_floor_top - eps])
                    cylinder(d=MOTOR_D + 2*MOTOR_POCKET_CLR, h=MOTOR_TH + 0.6 + eps);
            }
        }

        // ---- Screw clearance bores + head counterbores (from the back) ----
        for (h = SCREW_HOLES) {
            translate([h[0], h[1], z_floor_top - eps])
                cylinder(d=SCREW_BORE, h=z_pcb_bot - z_floor_top + 2*eps);
            translate([h[0], h[1], -eps])
                cylinder(d=HEAD_CB_D, h=HEAD_CB_H + eps);   // head recess in the floor
        }

        // ---- Battery wire routing channel: BATT1 exit -> under-board bay ----
        translate([BAT_WIRE_PT[0], BAT_WIRE_PT[1], z_floor_top])
            rotate([0,0,0])
            translate([-(BAT_WIRE_PT[0]-BAT_P1[0]), -1.5, 0])
                cube([BAT_WIRE_PT[0]-BAT_P1[0] + eps, 3, 3]);

        // ---- Motor wire slot toward its JST ----
        translate([MOTOR_PT[0]-6, MOTOR_PT[1]-1.25, z_floor_top])
            cube([6, 2.5, 3]);

        // ---- Recessed label area on the back exterior ----
        translate([ (ext_min[0]+ext_max[0])/2, (ext_min[1]+ext_max[1])/2, -eps ])
            linear_extrude(LABEL_DEPTH + eps)
                offset(r=2) square([EXT_W*0.55, EXT_H*0.42], center=true);

        // ---- USB-C notch in the bottom wall (mostly formed by the front cover;
        //      cut the sliver that lands in the tub rim too) ----
        usb_cut();
    }

    // ---- Belt-clip T-rail on the back exterior (protrudes in -Z) ----
    clip_rail();
}

// Belt-clip mounting T-rail (stem + wider head) on the outer back face.
module clip_rail() {
    y0 = CLIP_RAIL_YC - CLIP_RAIL_LEN/2;
    translate([ (ext_min[0]+ext_max[0])/2, 0, 0 ]) {
        // stem
        translate([-CLIP_STEM_W/2, y0, -CLIP_STEM_H])
            cube([CLIP_STEM_W, CLIP_RAIL_LEN, CLIP_STEM_H + eps]);
        // head (undercut)
        translate([-CLIP_HEAD_W/2, y0, -CLIP_STEM_H - CLIP_HEAD_H])
            cube([CLIP_HEAD_W, CLIP_RAIL_LEN, CLIP_HEAD_H]);
    }
}

// ============================================================================
// 5. FRONT COVER (lid)
// ============================================================================
module front_cover() {
    difference() {
        union() {
            // ---- Lid: solid parting..outer, hollowed under the ceiling ----
            difference() {
                union() {
                    rbox(ext_min, ext_max, z_part, z_front_out, R_OUT);
                    if (DBG_GPS) gps_bump();     // local raised boss over the GPS antenna
                    // WROOM bulge continues across the seam (same footprint as tub)
                    translate([int_min[0] - WROOM_POCKET_DEP - WROOM_BULGE_SKIN,
                               WROOM_BULGE_Y0, z_part])
                        cube([WROOM_POCKET_DEP + WROOM_BULGE_SKIN + (int_min[0] - ext_min[0]),
                              WROOM_BULGE_LEN, z_front_out - z_part]);
                }
                // hollow: remove the interior below the ceiling (leaves side walls)
                rbox(int_min, int_max, z_part - eps, z_ceil, R_IN);
                // WROOM antenna tab relief: the tab rides on the PCB top plane, so
                // its overhang lives in THIS shell's wall zone, not the tub's
                translate([int_min[0] - WROOM_POCKET_DEP, WROOM_POCKET_YC - WROOM_POCKET_LEN/2, z_part - eps])
                    cube([WROOM_POCKET_DEP + eps, WROOM_POCKET_LEN, z_ceil - z_part + eps]);
            }

            // ---- Alignment lip that plugs down into the tub ----
            difference() {
                rbox([int_min[0]+FIT_CLR, int_min[1]+FIT_CLR],
                     [int_max[0]-FIT_CLR, int_max[1]-FIT_CLR],
                     z_part - LIP_H, z_part + eps, R_IN);
                rbox([int_min[0]+FIT_CLR+LIP_TH, int_min[1]+FIT_CLR+LIP_TH],
                     [int_max[0]-FIT_CLR-LIP_TH, int_max[1]-FIT_CLR-LIP_TH],
                     z_part - LIP_H - eps, z_part + 2*eps, R_IN);
            }

            // ---- Module deck frame: pocket walls that locate the e-paper ----
            difference() {
                rbox([PANEL_P0[0]-1.6, PANEL_P0[1]-1.6],
                     [PANEL_P1[0]+1.6, PANEL_P1[1]+1.6],
                     z_ceil - MODULE_TH - 1.0, z_ceil, R_IN);
                rbox([PANEL_P0[0]-MODULE_CLR, PANEL_P0[1]-MODULE_CLR],
                     [PANEL_P1[0]+MODULE_CLR, PANEL_P1[1]+MODULE_CLR],
                     z_ceil - MODULE_TH - 1.0 - eps, z_ceil + eps, R_IN);
            }

            // ---- Front screw posts (self-tap targets), not under the panel ----
            for (h = SCREW_HOLES)
                translate([h[0], h[1], z_part])
                    cylinder(d=POST_OD, h=z_ceil - z_part);

            // ---- Short flange collars: cap the plunger flange so it cannot
            //      drop out the front face (only around the flange, clear of
            //      the wider down-stop skirt below it) ----
            if (DBG_COLLAR) for (p = btn_pts)
                translate([p[0], p[1], z_ceil - 1.4])
                    difference() {
                        cylinder(d=BTN_FLANGE_D + 2.2, h=1.4 + eps);
                        translate([0,0,-eps]) cylinder(d=BTN_FLANGE_D + FIT_CLR, h=1.4 + 3*eps);
                    }
        }

        // ---- Screw pilot bores in the posts ----
        for (h = SCREW_HOLES)
            translate([h[0], h[1], z_part - eps])
                cylinder(d=SCREW_PILOT, h=z_ceil - z_part + 2*eps);

        // ---- Display window (through the face plate), biased onto the active area ----
        rbox(WIN_P0, WIN_P1, z_ceil - eps, z_front_out + eps, R_IN);

        // ---- LoRa u.FL coax relief: a 2mm-wide x 1.5mm-deep channel cut into the
        //      underside of the raised deck, from above ANT1 (UFL_LORA, board 26,17.5)
        //      out to the top inner wall. It notches the deck rim so the mated MHF1
        //      pigtail can exit laterally toward the LoRa flex antenna on the top wall
        //      without being pinched by the deck. Coax routes ANT1 -> top wall flex.
        translate([UFL_LORA[0] - 1, UFL_LORA[1], z_ceil - MODULE_TH - 1.5])
            cube([2, int_max[1] - UFL_LORA[1] + eps, 1.5 + eps]);

        // ---- Button nub holes ----
        for (p = btn_pts)
            translate([p[0], p[1], z_ceil - eps])
                cylinder(d=BTN_HOLE_D, h=FC_TH + 2*eps);

        // ---- RESET pinhole ----
        translate([RESET_PT[0], RESET_PT[1], z_ceil - eps])
            cylinder(d=RESET_D, h=FC_TH + 2*eps);

        // ---- Status LED light-pipe hole ----
        translate([LED_PT[0], LED_PT[1], z_ceil - eps])
            cylinder(d=LED_D, h=FC_TH + 2*eps);

        // ---- Charge LED light-pipe holes ----
        for (c = CHG_PTS)
            translate([c[0], c[1], z_ceil - eps])
                cylinder(d=CHG_D, h=FC_TH + 2*eps);

        // ---- Buzzer 3x1mm sound-hole grid ----
        hole_row(BUZZ_PT, 3, BUZZ_PITCH, BUZZ_D, z_ceil - eps, z_front_out + eps);

        // ---- GPS sky pocket: hollow the raised bump interior, leaving GPS_SKIN
        //      of sky-facing plastic on top and GPS_WALL retaining walls all
        //      round (the cut is INSET from the bump so the walls have real
        //      thickness -- coincident faces would be non-manifold). ----
        rbox([GPS_POCKET_P0[0]+GPS_WALL, GPS_POCKET_P0[1]+GPS_WALL],
             [GPS_POCKET_P1[0]-GPS_WALL, GPS_POCKET_P1[1]-GPS_WALL],
             z_part - eps, z_gps_out - GPS_SKIN, R_IN);

        // ---- USB-C opening (front cover forms the bulk of it) ----
        usb_cut();
    }
}

// Local raised boss on the front cover to give the GPS patch antenna headroom.
module gps_bump() {
    rbox(GPS_POCKET_P0, GPS_POCKET_P1, z_part, z_gps_out, R_IN);
}

// USB-C receptacle opening on the bottom wall (model Y = ext_min[1]).
// Flush opening + 0.3 clearance, wall locally thinned, outward 45-deg chamfer.
module usb_cut() {
    ow = USB_SHELL_W + 2*USB_CLR;
    oh = USB_SHELL_H + 2*USB_CLR;
    z0 = USB_VC - oh/2;
    // straight opening through the wall
    translate([USB_CX - ow/2, ext_min[1] - eps, z0])
        cube([ow, WALL + eps, oh]);
    // local wall thinning (recess the inner face so plugs fully seat)
    translate([USB_CX - ow/2 - 2, int_min[1] - (WALL - USB_THIN), z0 - 1])
        cube([ow + 4, WALL - USB_THIN + eps, oh + 2]);
    // outward chamfer (widen toward the outer face)
    translate([USB_CX, ext_min[1] + USB_CHAMF, USB_VC])
        rotate([90,0,0])
            scale([1, oh/ow, 1])
                cylinder(d1=ow + 2*USB_CHAMF, d2=ow, h=USB_CHAMF + eps);
}

// ============================================================================
// 6. CAPTIVE PLUNGER BUTTON  (print 3; classic pager style)
// ----------------------------------------------------------------------------
// Assembly: the plunger rests on the switch, then the front cover is lowered
// over it (the nub enters the face hole from inside). Retention and travel:
//   * FLANGE (> face hole) under the ceiling  -> cannot pull out the front.
//   * SKIRT lands on bare PCB after SW_TRAVEL  -> HARD bottom-stop so the user
//     cannot crush the switch (switch travel is only 0.25 mm).
//   * central PIP presses the switch actuator.
// ============================================================================
module plunger() {
    // Local frame Z=0 at the PCB top surface (assembled datum).
    z_sw    = SW_H;                 // 1.5  switch top: actuator disk rests here
    z_stop  = SW_TRAVEL;           // 0.25 skirt underside floats here at rest
    z_c     = z_ceil - z_pcb_top;   // 4.0  front-cover inner ceiling
    flange_h = 1.2;
    disk_h   = 0.8;

    difference() {
        union() {
            // skirt + actuator-disk block: a wide cylinder that becomes the
            // hard down-stop (its rim lands on bare PCB after SW_TRAVEL)
            translate([0,0,z_stop])
                cylinder(d=BTN_SKIRT_D, h=(z_sw + disk_h) - z_stop);
            // stem up to the retention flange
            translate([0,0,z_sw + disk_h - eps])
                cylinder(d=3.0, h=(z_c - flange_h) - (z_sw + disk_h) + eps);
            // retention flange (larger than the face hole -> captive)
            translate([0,0,z_c - flange_h])
                cylinder(d=BTN_FLANGE_D, h=flange_h);
            // nub through the face + domed cap, sits BTN_PROUD proud
            translate([0,0,z_c])
                cylinder(d=BTN_NUB_D, h=FC_TH - 0.5);
            translate([0,0,z_c + FC_TH - 0.5])
                sphere(d=BTN_NUB_D);
        }
        // clearance pocket under the disk so the 5.1mm switch body nests inside
        // the skirt; the 0.8mm disk roof rests on the switch top face
        translate([0,0,z_stop - eps])
            cylinder(d=SW_BODY + 0.6, h=(z_sw) - z_stop + eps);
    }
}

// ============================================================================
// 7. SNAP-ON BELT CLIP  (slides onto the back T-rail, integral spring finger)
// ============================================================================
module belt_clip() {
    // Base plate rides the back T-rail via a blind T-groove opening downward
    // (-Z). A C-shaped spring finger cantilevers off the +Y end to grip a belt.
    // Printed as-is: plate flat on the bed, groove down, spring arcing up.
    plate_t   = 4.0;
    slot_stem = CLIP_STEM_W + 2*FIT_CLR;
    slot_head = CLIP_HEAD_W + 2*FIT_CLR;
    arm_t     = 2.4;
    difference() {
        union() {
            // base plate (groove side down at z=0)
            translate([-CLIP_W/2, -CLIP_LEN/2, 0])
                cube([CLIP_W, CLIP_LEN, plate_t]);
            // spring finger: rises from the +Y end, arcs over and back down
            translate([0, CLIP_LEN/2 - arm_t, 0])
                rotate([90,0,90])
                    translate([0,0,-CLIP_W/2])
                        linear_extrude(CLIP_W)
                            spring_profile(arm_t);
        }
        // blind T-groove: head channel (wide) then stem channel, open at z=0,
        // leaving the top of the plate solid
        translate([-slot_head/2, -CLIP_LEN/2 - eps, -eps])
            cube([slot_head, CLIP_LEN + 2*eps, CLIP_HEAD_H + eps]);
        translate([-slot_stem/2, -CLIP_LEN/2 - eps, CLIP_HEAD_H])
            cube([slot_stem, CLIP_LEN + 2*eps, CLIP_STEM_H + eps]);
        // stop wall at the -Y end so the rail seats to a hard datum
        translate([-slot_head/2, -CLIP_LEN/2 - eps, -eps])
            cube([slot_head, 3, plate_t + 2*eps]);
    }
}

// 2D side profile of the spring finger (extruded across the clip width).
// Rises in +Z, arcs 180 deg, returns down with an inturned lip.
module spring_profile(arm_t) {
    rise = plate_z_top() + 16;   // apex height above the plate
    r    = 7;
    difference() {
        union() {
            square([arm_t, rise]);                       // riser
            translate([r, rise]) circle(r=r);            // knuckle
            translate([2*r - arm_t, rise - 12]) square([arm_t, 12]); // return leg
        }
        translate([r, rise]) circle(r=r - arm_t);         // hollow the knuckle
    }
}
function plate_z_top() = 4.0;

// ============================================================================
// 8. ASSEMBLY (visual only)
// ============================================================================
module ghost_pcb() {
    color([0.1,0.4,0.15,0.55])
        rbox([0,0],[BOARD_W,BOARD_H], z_pcb_bot, z_pcb_top, BOARD_R);
}
module ghost_module() {
    color([0.85,0.85,0.9,0.8])
        rbox(PANEL_P0, PANEL_P1, z_ceil - MODULE_TH, z_ceil, 1);
}

module assembly() {
    color([0.25,0.28,0.32]) back_shell();
    ghost_pcb();
    ghost_module();
    color([0.55,0.58,0.62,0.9]) front_cover();
    color([0.9,0.75,0.2]) for (p = btn_pts)
        translate([p[0], p[1], z_pcb_top]) plunger();
    color([0.2,0.2,0.22]) translate([0,0,0]) belt_clip_placed();
}
module belt_clip_placed() {
    // Mirror in Z so the groove opens toward the back face and the clip hangs
    // behind the case, spring finger toward +Y (top). Visual only.
    translate([ (ext_min[0]+ext_max[0])/2, CLIP_RAIL_YC, 0 ])
        mirror([0,0,1]) belt_clip();
}

// ============================================================================
// 9. PART SELECTOR
// ============================================================================
if      (part == "back")     back_shell();
else if (part == "front")    front_cover();
else if (part == "plunger")  plunger();
else if (part == "clip")     belt_clip();
else                         assembly();
