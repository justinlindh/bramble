package main

import (
	"strings"
	"testing"
)

// The canonical NMEA documentation sentence: its published checksum is 6A.
// Guards the XOR-accumulate implementation against off-by-one framing (the
// '$' and '*' must be excluded).
func TestNMEAChecksumCanonicalFixture(t *testing.T) {
	body := "GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W"
	if got := nmeaChecksum(body); got != "6A" {
		t.Fatalf("checksum(%q) = %q, want 6A", body, got)
	}
}

func TestNMEALatLonRoundTrip(t *testing.T) {
	// Origin (x=0, y=0) must format exactly as the documentation origin.
	lat, ns, lon, ew := nmeaDegrees(0, 0)
	if ns != "N" || ew != "E" {
		t.Fatalf("origin hemispheres = %s %s, want N E", ns, ew)
	}
	if !strings.HasPrefix(lat, "4807.0") {
		t.Fatalf("origin lat = %q, want 4807.0xxx", lat)
	}
	if !strings.HasPrefix(lon, "01131.0") {
		t.Fatalf("origin lon = %q, want 01131.0xxx", lon)
	}

	// +y is north: 1113.2 ether units (~1113 m) is about 0.01 degrees, i.e.
	// 0.6 NMEA minutes: 4807.038 -> ~4807.638.
	latN, _, _, _ := nmeaDegrees(0, 1113.2)
	if !strings.HasPrefix(latN, "4807.6") {
		t.Fatalf("lat at y=+1113.2 = %q, want 4807.6xxx", latN)
	}
}

func TestNMEARMCAndGGAShape(t *testing.T) {
	rmc := nmeaRMC(20, 30, 61_500_000) // sim t = 61.5s -> 000101 UTC
	if !strings.HasPrefix(rmc, "$GPRMC,000101,A,") {
		t.Fatalf("rmc = %q, want prefix $GPRMC,000101,A,", rmc)
	}
	gga := nmeaGGA(20, 30, 61_500_000)
	if !strings.HasPrefix(gga, "$GPGGA,000101,") {
		t.Fatalf("gga = %q, want prefix $GPGGA,000101,", gga)
	}
	for _, s := range []string{rmc, gga} {
		star := strings.LastIndexByte(s, '*')
		if star < 0 || len(s)-star != 3 {
			t.Fatalf("sentence %q missing 2-hex checksum suffix", s)
		}
		if got := nmeaChecksum(s[1:star]); got != s[star+1:] {
			t.Fatalf("sentence %q checksum = %s, want %s", s, s[star+1:], got)
		}
	}
	// GGA must report a valid fix (quality 1) and a plausible sats-used
	// count, which is what drives the firmware's satellite stats.
	if !strings.Contains(gga, ",1,08,") {
		t.Fatalf("gga = %q, want fix quality 1 and 08 sats", gga)
	}
}

// The firmware classifies "satellites in view but none tracked" as no signal,
// so a virtual node that emitted GGA without GSV would report a fix alongside
// zero satellites in view. The cycle must be well formed for the firmware's
// per-talker accumulator to commit it.
func TestNMEAGSVCycleShape(t *testing.T) {
	msgs := nmeaGSV()
	if len(msgs) != 3 {
		t.Fatalf("nmeaGSV returned %d messages, want 3", len(msgs))
	}
	for i, s := range msgs {
		star := strings.LastIndexByte(s, '*')
		if star < 0 || len(s)-star != 3 {
			t.Fatalf("sentence %q missing 2-hex checksum suffix", s)
		}
		if got := nmeaChecksum(s[1:star]); got != s[star+1:] {
			t.Fatalf("sentence %q checksum = %s, want %s", s, s[star+1:], got)
		}
		// total_msgs, ascending msg_num, and the same in-view total in every
		// message of the cycle.
		wantPrefix := "$GPGSV,3," + string(rune('1'+i)) + ",11,"
		if !strings.HasPrefix(s, wantPrefix) {
			t.Fatalf("message %d = %q, want prefix %q", i, s, wantPrefix)
		}
	}
	// Four satellite groups in the first two messages, three in the last, so
	// every satellite the in-view total names is actually listed.
	for i, want := range []int{4, 4, 3} {
		got := (strings.Count(msgs[i], ",") - 3) / 4
		if got != want {
			t.Fatalf("message %d carries %d satellite groups, want %d", i, got, want)
		}
	}
}
