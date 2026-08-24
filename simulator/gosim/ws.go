package main

import (
	"encoding/json"
	"log"
	"net/http"
	"sync"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

type Client struct {
	conn *websocket.Conn
	send chan []byte
}

type Hub struct {
	mu      sync.RWMutex
	clients map[*Client]bool
	sim     *Sim
}

func NewHub(sim *Sim) *Hub {
	return &Hub{
		clients: make(map[*Client]bool),
		sim:     sim,
	}
}

// clientSendBuffer is how many broadcast events may queue for one client
// before the hub starts dropping for it.
//
// Sized for a burst, not a steady state: a firmware scenario boots several
// real node processes at once and each streams its whole boot console, which
// is several hundred one-shot events arriving faster than a browser drains
// them. A buffer sized inside that burst drops events from a perfectly
// healthy client, and a dropped console line is invisible: it is the only
// copy, so the UI's console pane silently loses a line and anything reading
// those lines (the playground tour's step milestones) can miss a marker
// that is printed exactly once. Dropping is still the policy for a client
// that has genuinely stopped reading; this only moves the threshold past
// the normal case.
const clientSendBuffer = 4096

// Broadcast sends msg to all connected clients, dropping if slow.
func (h *Hub) Broadcast(msg []byte) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	for c := range h.clients {
		select {
		case c.send <- msg:
		default:
			// slow client, drop
		}
	}
}

func (h *Hub) register(c *Client) {
	h.mu.Lock()
	h.clients[c] = true
	h.mu.Unlock()
}

func (h *Hub) unregister(c *Client) {
	h.mu.Lock()
	if _, ok := h.clients[c]; ok {
		delete(h.clients, c)
		close(c.send)
	}
	h.mu.Unlock()
}

// HandleWS upgrades the connection and starts read/write goroutines.
func (h *Hub) HandleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("ws upgrade: %v", err)
		return
	}

	client := &Client{
		conn: conn,
		send: make(chan []byte, clientSendBuffer),
	}

	// Catch the new client up on the world that already exists before it can
	// receive any live event, so ordering matches a client that had been
	// connected all along (joins first, then whatever happens next). Joins,
	// console lines and framebuffers are all one-shot broadcasts, so without
	// this a browser opened after a scenario loaded shows an empty map
	// indefinitely, which is the normal case for `gosim --playground` and for
	// any page reload of a live session.
	//
	// Written straight to the connection rather than through the send channel:
	// a snapshot is up to a few hundred events for a large scenario, well past
	// the channel's buffer, and the channel drops on a full buffer, which
	// would silently deliver a partial world. Nothing else writes to this
	// connection yet (the client is not registered and writePump is not
	// running), so a direct write here is the only writer.
	for _, evt := range h.sim.SnapshotEvents() {
		if err := conn.WriteMessage(websocket.TextMessage, evt); err != nil {
			conn.Close()
			return
		}
	}

	h.register(client)
	log.Printf("ws client connected (%d total)", len(h.clients))

	go h.writePump(client)
	go h.readPump(client)
}

func (h *Hub) writePump(c *Client) {
	defer c.conn.Close()
	for msg := range c.send {
		if err := c.conn.WriteMessage(websocket.TextMessage, msg); err != nil {
			return
		}
	}
}

func (h *Hub) readPump(c *Client) {
	defer func() {
		h.unregister(c)
		c.conn.Close()
		log.Printf("ws client disconnected (%d remaining)", len(h.clients))
	}()

	for {
		_, msg, err := c.conn.ReadMessage()
		if err != nil {
			return
		}

		var cmd Command
		if err := json.Unmarshal(msg, &cmd); err != nil {
			log.Printf("ws: bad json: %v", err)
			continue
		}

		// Auto-load default scenario if sim is idle
		if h.sim.State() == StateIdle && cmd.Type != "load" && cmd.Type != "start" {
			h.sim.Send(Command{Type: "load", Scenario: "10-node-grid"})
		}

		h.sim.Send(cmd)
	}
}
