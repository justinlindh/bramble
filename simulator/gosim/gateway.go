package main

// Gateway is the serial-attached PHY-passthrough bridge (DESIGN.md section 10):
// a real hardware node reached over serial whose radio becomes one more member
// of the emulated ether. Frames the hardware receives from the real mesh enter
// the collision model as receptions; broker frames destined for air go out the
// gateway's real radio. This makes a channel message from the physical fleet
// arrive on the virtual pagers, and a reply composed on a virtual pager reach
// the physical fleet.
//
// Task 9 implements it. This file is the typed skeleton only: no logic yet, so
// the broker and scenario wiring already have a concrete type to reference.
type Gateway struct {
	broker *Broker
	device string // serial device path, e.g. /dev/ttyUSB0
}

// NewGateway returns a gateway bound to the broker and serial device. Task 9
// gives it its read/write loops and passthrough framing.
func NewGateway(broker *Broker, device string) *Gateway {
	return &Gateway{broker: broker, device: device}
}
