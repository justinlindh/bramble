package main

// NMEA synthesis for virtual GPS: while a firmware node's GPS power gate is
// on (see handleGpsGate), the broker feeds it RMC+GGA sentences derived from
// the node's scenario slot position, on the simulation clock. This is the
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

// One sentence pair per simulated second, the cadence of a real 1 Hz GNSS.
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
// The date field is the canonical fixture date, which the firmware reads as
// the UTC calendar date behind its status-bar clock.
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
