// pagerFace.ts
//
// Face geometry for the virtual pager, derived DIRECTLY from the physical case
// model: hardware/pager/v1/case/pager_case.scad. Every constant below carries
// the scad symbol it comes from so the SVG face stays faithful to the enclosure
// (do not hand-tune these; re-derive from the scad if the case changes).
//
// SCAD COORDINATE SYSTEM (from the scad header):
//   Board-local: origin board top-left, x right (0..96), y DOWN (0..50).
//   Model: same X, Y flipped up  ->  mb(bx,by) = [bx, BOARD_H - by].
//   The scad exterior footprint (model coords) spans
//     ext_min = [-2.4,-2.4], ext_max = [98.4,52.4].
//
// FACE (SVG) COORDINATE SYSTEM:
//   SVG y is DOWN, so we map model -> face with the exterior top-left at (0,0):
//     fx = mx - ext_min.x        (== mx + 2.4)
//     fy = ext_max.y - my        (flip; model +Y up -> SVG down)
//   viewBox is "0 0 EXT_W EXT_H" in millimetres.

// ---- Board (scad: BOARD_W, BOARD_H) ----
const BOARD_W = 96;
const BOARD_H = 50;

// ---- Fit / tolerances (scad: WALL, SIDE_CLR) ----
const WALL = 2.0;
const SIDE_CLR = 0.4;

// ---- Exterior footprint (scad: ext_min, ext_max, EXT_W, EXT_H) ----
const EXT_MIN: Point = { x: -SIDE_CLR - WALL, y: -SIDE_CLR - WALL }; // [-2.4,-2.4]
const EXT_MAX: Point = { x: BOARD_W + SIDE_CLR + WALL, y: BOARD_H + SIDE_CLR + WALL }; // [98.4,52.4]
export const EXT_W = EXT_MAX.x - EXT_MIN.x; // 100.8
export const EXT_H = EXT_MAX.y - EXT_MIN.y; // 54.8

// ---- Outer vertical-edge radius (scad: R_OUT) ----
export const R_OUT = 3.5;

export interface Point {
  x: number;
  y: number;
}
export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

// scad: function mb(bx,by) = [bx, BOARD_H - by]  (board -> model coords)
function mb(bx: number, by: number): Point {
  return { x: bx, y: BOARD_H - by };
}

// model -> face (SVG) coords, flipping Y so model "up" is SVG "up".
function m2f(p: Point): Point {
  return { x: p.x - EXT_MIN.x, y: EXT_MAX.y - p.y };
}

// A face-space rect from two model corners (order-independent).
function faceRect(p0: Point, p1: Point): Rect {
  const a = m2f(p0);
  const b = m2f(p1);
  return {
    x: Math.min(a.x, b.x),
    y: Math.min(a.y, b.y),
    w: Math.abs(a.x - b.x),
    h: Math.abs(a.y - b.y),
  };
}

// ---- Body outline: full exterior footprint (face space) ----
export const BODY: Rect = { x: 0, y: 0, w: EXT_W, h: EXT_H };

// ---- Display panel window (scad: WIN_P0 / WIN_P1) ----
// scad: PANEL_P0 = mb(22,30.2); PANEL_P1 = mb(81.2,1);
//       ACT_OFF = [0.725,0]; ACTIVE_W = 48.55; ACTIVE_H = 23.70; WIN_REVEAL = 1.0;
//       ACT_CTR = [(P0.x+P1.x)/2 + ACT_OFF.x, (P0.y+P1.y)/2 + ACT_OFF.y];
//       WIN_P0 = ACT_CTR - [ACTIVE_W/2+WIN_REVEAL, ACTIVE_H/2+WIN_REVEAL];
//       WIN_P1 = ACT_CTR + [ACTIVE_W/2+WIN_REVEAL, ACTIVE_H/2+WIN_REVEAL];
const PANEL_P0 = mb(22, 30.2);
const PANEL_P1 = mb(81.2, 1);
const ACT_OFF: Point = { x: 0.725, y: 0 };
const ACTIVE_W = 48.55; // scad: ACTIVE_W (GDEY0213B74 A.A long axis, mm)
const ACTIVE_H = 23.7; // scad: ACTIVE_H (A.A short axis, mm)
const WIN_REVEAL = 1.0; // scad: WIN_REVEAL (glass revealed past the active area)
const ACT_CTR: Point = {
  x: (PANEL_P0.x + PANEL_P1.x) / 2 + ACT_OFF.x,
  y: (PANEL_P0.y + PANEL_P1.y) / 2 + ACT_OFF.y,
};
const WIN_P0: Point = { x: ACT_CTR.x - ACTIVE_W / 2 - WIN_REVEAL, y: ACT_CTR.y - ACTIVE_H / 2 - WIN_REVEAL };
const WIN_P1: Point = { x: ACT_CTR.x + ACTIVE_W / 2 + WIN_REVEAL, y: ACT_CTR.y + ACTIVE_H / 2 + WIN_REVEAL };

// The cover cutout (glass reveal) and, inside it, the exact active-pixel area
// where the 250x122 framebuffer canvas is placed.
export const WINDOW: Rect = faceRect(WIN_P0, WIN_P1);
export const ACTIVE: Rect = faceRect(
  { x: ACT_CTR.x - ACTIVE_W / 2, y: ACT_CTR.y - ACTIVE_H / 2 },
  { x: ACT_CTR.x + ACTIVE_W / 2, y: ACT_CTR.y + ACTIVE_H / 2 },
);

// ---- Face buttons (scad: btn_pts = [mb(36,37.5), mb(48,37.5), mb(60,37.5)] ----
// scad comment labels them "BOOT, UP, DOWN"; BOOT doubles as SELECT in firmware.
// scad: BTN_HOLE_D = 4.0 (nub bore), BTN_FLANGE_D = 5.2 (retention flange).
export const BTN_HOLE_D = 4.0;
export const BTN_FLANGE_D = 5.2;
export type ButtonId = 'select' | 'up' | 'down';
export interface FaceButton {
  id: ButtonId;
  label: string;
  center: Point; // face space
  r: number; // plunger nub radius (face space)
}
const BTN_PTS = [mb(36, 37.5), mb(48, 37.5), mb(60, 37.5)];
export const BUTTONS: FaceButton[] = [
  { id: 'select', label: 'SEL', center: m2f(BTN_PTS[0]), r: BTN_HOLE_D / 2 },
  { id: 'up', label: 'UP', center: m2f(BTN_PTS[1]), r: BTN_HOLE_D / 2 },
  { id: 'down', label: 'DN', center: m2f(BTN_PTS[2]), r: BTN_HOLE_D / 2 },
];

// ---- LED aperture (scad: LED_PT = mb(48,34); LED_D = 2.0) ----
export const LED: Point & { r: number } = { ...m2f(mb(48, 34)), r: 2.0 / 2 };

// ---- RESET pinhole (scad: RESET_PT = mb(79,46.3); RESET_D = 1.2), a corner ----
export const RESET: Point & { r: number } = { ...m2f(mb(79, 46.3)), r: 1.2 / 2 };

// ---- Buzzer sound port (scad: BUZZ_PT = mb(71.5,35.5); BUZZ_D = 1.0) ----
export const BUZZER: Point & { r: number } = { ...m2f(mb(71.5, 35.5)), r: 1.0 / 2 };

// ---- USB-C opening on the bottom wall (scad: USB_CX, USB_SHELL_W) ----
// scad: USB_CX = 48 (centered on X), USB_SHELL_W = 8.94; the port is on the
// board y=50 wall, which flips to the model Y=0 (bottom) edge -> face bottom.
const USB_CX = 48;
const USB_SHELL_W = 8.94;
export const USB: Rect = {
  x: m2f({ x: USB_CX - USB_SHELL_W / 2, y: 0 }).x,
  y: EXT_H - 1.6, // notch sits on the bottom exterior edge
  w: USB_SHELL_W,
  h: 3.2,
};
