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
		send: make(chan []byte, 256),
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
