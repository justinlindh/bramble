package main

import "testing"

// blitText draws text into fb at (x,y) exactly as the firmware's
// display_draw_text does (font6x8, 6px advance, bit row of column col), so the
// tests search a framebuffer built by the same rule the real panel uses.
func blitText(fb []byte, x, y int, text string) {
	for i := 0; i < len(text); i++ {
		c := text[i]
		if c >= 0x20 && c <= 0x7E {
			glyph := font6x8[c-0x20]
			for col := 0; col < 6; col++ {
				bits := glyph[col]
				for row := 0; row < 8; row++ {
					if bits&(1<<uint(row)) != 0 {
						setFBPixel(fb, x+col, y+row)
					}
				}
			}
		}
		x += 6
		if x >= fbWidth {
			break
		}
	}
}

func setFBPixel(fb []byte, x, y int) {
	if x < 0 || x >= fbWidth || y < 0 || y >= fbHeight {
		return
	}
	fb[y*fbStride+x/8] |= 0x80 >> (uint(x) & 7)
}

func blankFB() []byte { return make([]byte, fbSize) }

// invertFB flips every pixel in-place: models a white-text-on-black-ink screen.
func invertFB(fb []byte) []byte {
	out := make([]byte, len(fb))
	for i := range fb {
		out[i] = ^fb[i]
	}
	return out
}

// TestScreenContainsMatchesRenderedText is the positive control: a string blit
// with the firmware's own routine is found, including at a substring boundary.
func TestScreenContainsMatchesRenderedText(t *testing.T) {
	fb := blankFB()
	blitText(fb, 10, 40, "HELLO WORLD")

	for _, want := range []string{"HELLO WORLD", "HELLO", "WORLD", "LO WO"} {
		if !screenContains(fb, want) {
			t.Errorf("screenContains(fb, %q) = false, want true", want)
		}
	}
}

// TestScreenContainsRejectsAbsentText is the deliberately-failing-then-green
// proof the harness demands: the matcher must reject a string that is NOT on the
// screen, including a one-glyph near miss, so a scenario cannot pass by accident.
func TestScreenContainsRejectsAbsentText(t *testing.T) {
	fb := blankFB()
	blitText(fb, 10, 40, "HELLO WORLD")

	for _, absent := range []string{"GOODBYE", "HELXO", "HELL0", "WORLE", "hello", ""} {
		if screenContains(fb, absent) {
			t.Errorf("screenContains(fb, %q) = true, want false (string is absent)", absent)
		}
	}

	// A blank screen contains no text at all.
	if screenContains(blankFB(), "A") {
		t.Error("blank framebuffer must not match any text")
	}
}

// TestScreenContainsInvertedPolarity: white text on a black-inked background
// (the pager's alert/banner style) still matches.
func TestScreenContainsInvertedPolarity(t *testing.T) {
	fb := blankFB()
	blitText(fb, 8, 8, "ALERT")
	inv := invertFB(fb)
	if !screenContains(inv, "ALERT") {
		t.Error("inverted (white-on-black) text should match")
	}
	if screenContains(inv, "ALARM") {
		t.Error("inverted near-miss must still be rejected")
	}
}

// TestScreenContainsRotated180: a panel drawn rotated (display_set_rotated_180)
// is searched in its upright form, so the string is still found.
func TestScreenContainsRotated180(t *testing.T) {
	fb := blankFB()
	blitText(fb, 30, 50, "PAGE")
	rot := rotate180(fb)
	if !screenContains(rot, "PAGE") {
		t.Error("text on a 180-rotated panel should match")
	}
}

// TestScreenContainsWiderThanPanel: a string too wide for the panel cannot match
// (guards the width clamp from producing a false positive).
func TestScreenContainsWiderThanPanel(t *testing.T) {
	fb := blankFB()
	long := ""
	for i := 0; i < 60; i++ { // 60*6 = 360 px > 250
		long += "X"
	}
	blitText(fb, 0, 0, long)
	if screenContains(fb, long) {
		t.Error("a string wider than the panel must not report a full match")
	}
}
