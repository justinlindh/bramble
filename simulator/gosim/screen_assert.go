package main

// Screen assertion for headless scenarios: given a node's framebuffer (the
// opaque 3904-byte payload of an emu-link "fb" message) and an expected ASCII
// string, decide whether that string is rendered on the panel. There is no OCR:
// the expected string is rasterized with the exact glyph table and blit rule the
// firmware uses (font6x8 + display_draw_text in components/display/display_virt.c)
// and the framebuffer is searched for a pixel-exact occurrence of that bitmap.
//
// Framebuffer layout (ssd1680_engine.h): 250x122, 1bpp, row-major, 32 bytes per
// row, MSB = leftmost pixel, a set bit = black ink. Six pad bits sit in the LSBs
// of each row's last byte. This is the logical framebuffer the engine returns,
// which display_pixel writes through display_draw_text, so a string drawn by the
// firmware lands here byte-for-byte identically to what renderGlyphs produces.

const (
	fbWidth  = 250
	fbHeight = 122
	fbStride = 32
	fbSize   = fbStride * fbHeight // 3904
)

// fbPixel reports whether the pixel at (x,y) is inked in fb. Out-of-range is
// treated as not inked (the same clip the engine applies on writes).
func fbPixel(fb []byte, x, y int) bool {
	if x < 0 || x >= fbWidth || y < 0 || y >= fbHeight {
		return false
	}
	idx := y*fbStride + x/8
	if idx >= len(fb) {
		return false
	}
	return fb[idx]&(0x80>>(uint(x)&7)) != 0
}

// renderGlyphs rasterizes text into a width-by-8 boolean bitmap using the exact
// blit of display_draw_text: for each character, glyph = font6x8[c-0x20], and
// pixel (col,row) is set when bit row of column col is set (bit 0 = top row).
// Characters advance 6 px; non-printable characters render as a blank cell (the
// firmware skips drawing them but still advances). The result is column-major:
// out[x][y]. An empty or all-blank render returns ok=false so a caller never
// "matches" nothing.
func renderGlyphs(text string) (out [][8]bool, ok bool) {
	if text == "" {
		return nil, false
	}
	w := 6 * len(text)
	if w > fbWidth {
		// The string is wider than the panel; the firmware itself would clip it,
		// so it can never fully render. Report un-renderable rather than matching
		// a truncated prefix.
		return nil, false
	}
	out = make([][8]bool, w)
	anyInk := false
	for i := 0; i < len(text) && i*6 < w; i++ {
		c := text[i]
		if c < 0x20 || c > 0x7E {
			continue // blank cell, matches display_draw_text's skip-but-advance
		}
		glyph := font6x8[c-0x20]
		for col := 0; col < 6; col++ {
			x := i*6 + col
			if x >= w {
				break
			}
			bits := glyph[col]
			for row := 0; row < 8; row++ {
				if bits&(1<<uint(row)) != 0 {
					out[x][row] = true
					anyInk = true
				}
			}
		}
	}
	return out, anyInk
}

// screenContains reports whether the rendered form of text appears anywhere in
// the framebuffer. It slides the rasterized bitmap over every position and
// requires a pixel-exact match of the whole glyph box (both inked and blank
// pixels), which rejects a near-miss string rather than matching loosely. To
// stay robust to how a given screen is drawn it accepts either ink polarity
// (dark text on light or the inverse) and either panel orientation (upright or
// the firmware's optional 180-degree rotation).
func screenContains(fb []byte, text string) bool {
	pat, ok := renderGlyphs(text)
	if !ok {
		return false
	}
	if matchAnyPolarity(fb, pat) {
		return true
	}
	return matchAnyPolarity(rotate180(fb), pat)
}

// matchAnyPolarity slides pat over fb, trying both a direct match (inked pattern
// pixel == inked fb pixel) and an inverted match (for white-on-black regions).
func matchAnyPolarity(fb []byte, pat [][8]bool) bool {
	return slideMatch(fb, pat, false) || slideMatch(fb, pat, true)
}

// slideMatch returns true if pat occurs in fb at some offset. When invert is
// true the fb pixel is negated before comparison (light text on dark ink).
func slideMatch(fb []byte, pat [][8]bool, invert bool) bool {
	w := len(pat)
	if w == 0 || w > fbWidth {
		return false
	}
	const h = 8
	for oy := 0; oy <= fbHeight-h; oy++ {
		for ox := 0; ox <= fbWidth-w; ox++ {
			if boxMatches(fb, pat, ox, oy, invert) {
				return true
			}
		}
	}
	return false
}

// boxMatches checks a pixel-exact match of pat at (ox,oy).
func boxMatches(fb []byte, pat [][8]bool, ox, oy int, invert bool) bool {
	for px := 0; px < len(pat); px++ {
		for py := 0; py < 8; py++ {
			ink := fbPixel(fb, ox+px, oy+py)
			if invert {
				ink = !ink
			}
			if ink != pat[px][py] {
				return false
			}
		}
	}
	return true
}

// rotate180 returns a copy of fb rotated 180 degrees, so a panel the firmware
// drew rotated (display_set_rotated_180) is searched in its upright form. A
// short/oversized buffer is returned unchanged (the caller's match simply fails).
func rotate180(fb []byte) []byte {
	if len(fb) < fbSize {
		return fb
	}
	out := make([]byte, fbSize)
	for y := 0; y < fbHeight; y++ {
		for x := 0; x < fbWidth; x++ {
			if fbPixel(fb, x, y) {
				rx, ry := fbWidth-1-x, fbHeight-1-y
				out[ry*fbStride+rx/8] |= 0x80 >> (uint(rx) & 7)
			}
		}
	}
	return out
}
