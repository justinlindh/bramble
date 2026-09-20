package main

import (
	"sync"
	"testing"
)

// newTestClient builds a Client the hub can register without a real websocket
// connection: register, unregister, and Broadcast only touch the send channel
// and the client set, never conn.
func newTestClient(bufSize int) *Client {
	return &Client{send: make(chan []byte, bufSize)}
}

func TestHubRegisterUnregisterCount(t *testing.T) {
	h := NewHub(nil)
	a := newTestClient(1)
	b := newTestClient(1)

	if got := h.register(a); got != 1 {
		t.Fatalf("register a: count = %d, want 1", got)
	}
	if got := h.register(b); got != 2 {
		t.Fatalf("register b: count = %d, want 2", got)
	}
	if got := h.unregister(a); got != 1 {
		t.Fatalf("unregister a: count = %d, want 1", got)
	}
	if got := h.unregister(b); got != 0 {
		t.Fatalf("unregister b: count = %d, want 0", got)
	}
}

func TestHubUnregisterClosesSendChannel(t *testing.T) {
	h := NewHub(nil)
	c := newTestClient(1)
	h.register(c)
	h.unregister(c)

	if _, ok := <-c.send; ok {
		t.Fatal("send channel should be closed after unregister")
	}
}

func TestHubUnregisterUnknownClientIsNoop(t *testing.T) {
	h := NewHub(nil)
	c := newTestClient(1)
	// Never registered: unregister must not close its channel or change the
	// count, so a duplicate teardown (readPump's deferred unregister after an
	// explicit one) cannot double-close the send channel.
	if got := h.unregister(c); got != 0 {
		t.Fatalf("unregister of unknown client: count = %d, want 0", got)
	}
	select {
	case <-c.send:
		t.Fatal("send channel of an unregistered client must stay open")
	default:
	}
}

func TestHubBroadcastDeliversToRegisteredClients(t *testing.T) {
	h := NewHub(nil)
	c := newTestClient(1)
	h.register(c)

	msg := []byte("hello")
	h.Broadcast(msg)

	select {
	case got := <-c.send:
		if string(got) != "hello" {
			t.Fatalf("broadcast delivered %q, want %q", got, "hello")
		}
	default:
		t.Fatal("broadcast did not enqueue a message for a registered client")
	}
}

func TestHubBroadcastDropsWhenClientBufferFull(t *testing.T) {
	h := NewHub(nil)
	c := newTestClient(1)
	h.register(c)

	// Fill the one-slot buffer, then broadcast again: the second message must
	// be dropped rather than block the hub on a slow client.
	c.send <- []byte("first")
	h.Broadcast([]byte("second"))

	if got := len(c.send); got != 1 {
		t.Fatalf("client buffer length = %d, want 1 (second message dropped)", got)
	}
	if got := <-c.send; string(got) != "first" {
		t.Fatalf("buffered message = %q, want %q", got, "first")
	}
}

// TestHubConcurrentAccess exercises register, unregister, and Broadcast from
// many goroutines at once. Its value is under `go test -race`: it proves the
// client set and its count are only ever touched under the hub mutex.
func TestHubConcurrentAccess(t *testing.T) {
	h := NewHub(nil)

	const workers = 16
	var wg sync.WaitGroup
	wg.Add(workers)
	for i := 0; i < workers; i++ {
		go func() {
			defer wg.Done()
			for j := 0; j < 100; j++ {
				c := newTestClient(4)
				h.register(c)
				h.Broadcast([]byte("tick"))
				h.unregister(c)
			}
		}()
	}
	wg.Wait()

	if got := len(h.clients); got != 0 {
		t.Fatalf("all clients unregistered, but %d remain", got)
	}
}
