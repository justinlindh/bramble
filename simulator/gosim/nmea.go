package main

// NMEA synthesis for virtual GPS: while a firmware node's GPS power gate is
// on (see handleGpsGate), the broker feeds it RMC+GGA+GSV sentences derived
// from the node's scenario slot position, on the simulation clock. This is the
// "later task" the gpsgate hook in extnode.go reserved: it is what exercises
// the firmware's gps_virt path (nmea parse, fix state, fix callback) in the
// emulator, where previously no scenario ever drove GPS.
//
// The coordinate origin is the canonical NMEA documentation position
// (48 deg 07.038' N, 011 deg 31.000' E, the fixture every NMEA reference and
// test/test_nmea_parser.c use); slot x/y offset from it in ether units
// treated as meters (+x east, +y north). Placeholder coordinates only, per
// the repo rule against real-world positions in fixtures.

import (
	"fmt"
	"math"
)

// One sentence set per simulated second, the cadence of a real 1 Hz GNSS.
const nmeaFeedIntervalUs = 1_000_000

const (
	nmeaOriginLatDeg = 48.0 + 7.038/60.0
	nmeaOriginLonDeg = 11.0 + 31.000/60.0
	metersPerDegLat  = 111320.0
)

// nmeaChecksum XOR-accumulates the sentence body (everything between '$' and
// '*') and returns the two-hex-digit NMEA checksum.
func nmeaChecksum(body string) string {
	var cs byte
	for i := 0; i < len(body); i++ {
		cs ^= body[i]
	}
	return fmt.Sprintf("%02X", cs)
}

// nmeaDegrees maps a slot position to NMEA ddmm.mmm / dddmm.mmm coordinate
// fields with hemisphere indicators.
func nmeaDegrees(x, y float32) (lat, ns, lon, ew string) {
	latDeg := nmeaOriginLatDeg + float64(y)/metersPerDegLat
	lonDeg := nmeaOriginLonDeg + float64(x)/(metersPerDegLat*math.Cos(nmeaOriginLatDeg*math.Pi/180))

	ns, ew = "N", "E"
	if latDeg < 0 {
		ns, latDeg = "S", -latDeg
	}
	if lonDeg < 0 {
		ew, lonDeg = "W", -lonDeg
	}
	latWhole := int(latDeg)
	lonWhole := int(lonDeg)
	lat = fmt.Sprintf("%02d%06.3f", latWhole, (latDeg-float64(latWhole))*60)
	lon = fmt.Sprintf("%03d%06.3f", lonWhole, (lonDeg-float64(lonWhole))*60)
	return lat, ns, lon, ew
}

// nmeaTimeUTC renders the simulation clock as an NMEA hhmmss field. Purely
// sim-time derived, so a scenario replays identically.
func nmeaTimeUTC(simUs uint64) string {
	secs := simUs / 1_000_000
	return fmt.Sprintf("%02d%02d%02d", secs/3600%24, secs/60%60, secs%60)
}

// nmeaRMC builds a valid-fix RMC sentence for a slot position at a sim time.
// The date field is the canonical fixture date; nothing downstream reads it.
func nmeaRMC(x, y float32, simUs uint64) string {
	lat, ns, lon, ew := nmeaDegrees(x, y)
	body := fmt.Sprintf("GPRMC,%s,A,%s,%s,%s,%s,0.0,0.0,230394,,", nmeaTimeUTC(simUs), lat, ns, lon, ew)
	return "$" + body + "*" + nmeaChecksum(body)
}

// nmeaGGA builds a fix-quality-1 GGA sentence (8 satellites, fixed HDOP and
// altitude), which is what drives the firmware's sats-used stat and altitude.
func nmeaGGA(x, y float32, simUs uint64) string {
	lat, ns, lon, ew := nmeaDegrees(x, y)
	body := fmt.Sprintf("GPGGA,%s,%s,%s,%s,%s,1,08,0.9,100.0,M,46.9,M,,", nmeaTimeUTC(simUs), lat, ns, lon, ew)
	return "$" + body + "*" + nmeaChecksum(body)
}

// nmeaGSVSats is the fixture sky: eleven satellites, each with a PRN,
// elevation, azimuth and carrier-to-noise ratio. All eleven report a nonzero
// C/N0, so a virtual node is coherently "tracking eleven, using eight" rather
// than reporting satellites used with none in view. Fixed values keep a
// scenario replayable; the best C/N0 is 45 dB-Hz.
var nmeaGSVSats = [11][4]int{
	{10, 63, 137, 42}, {7, 61, 308, 40}, {5, 59, 169, 38}, {30, 54, 42, 45},
	{8, 45, 210, 35}, {13, 40, 95, 33}, {15, 36, 275, 31}, {18, 28, 15, 29},
	{20, 22, 190, 27}, {23, 17, 130, 24}, {27, 11, 305, 20},
}

// nmeaGSV builds one $GPGSV cycle: three messages covering the eleven
// satellites in nmeaGSVSats, four groups per message and three in the last.
// The satellites-in-view total is repeated in every message of the cycle, as
// a real receiver does. The cycle is time invariant, so unlike RMC and GGA it
// takes no simulation clock.
func nmeaGSV() []string {
	const perMsg = 4
	total := (len(nmeaGSVSats) + perMsg - 1) / perMsg
	out := make([]string, 0, total)
	for msg := 0; msg < total; msg++ {
		body := fmt.Sprintf("GPGSV,%d,%d,%02d", total, msg+1, len(nmeaGSVSats))
		for i := msg * perMsg; i < (msg+1)*perMsg && i < len(nmeaGSVSats); i++ {
			s := nmeaGSVSats[i]
			body += fmt.Sprintf(",%02d,%02d,%03d,%02d", s[0], s[1], s[2], s[3])
		}
		out = append(out, "$"+body+"*"+nmeaChecksum(body))
	}
	return out
}
