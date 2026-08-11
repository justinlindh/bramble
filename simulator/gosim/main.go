package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// playgroundScenario is the fleet --playground boots: three real firmware
// pagers in a line, two of them out of range of each other, booting
// unprovisioned so the guided tour can teach the fail-closed state. The name
// is a scenario file under the scenarios directory (simulator/scenarios).
const playgroundScenario = "emu-playground"

// uiConfig is what GET /api/ui-config answers. The browser UI asks for it once
// at startup to decide whether to show the guided tour, so `make playground`
// is a single command rather than a command plus a URL the user has to
// remember to decorate. Tour stays false for an ordinary `make run`, which is
// the simulator UI every other scenario is driven from.
type uiConfig struct {
	// Tour enables the guided tour overlay (simulator/ui/src/tour).
	Tour bool `json:"tour"`
	// Scenario is the scenario this process booted itself into, "" when it
	// booted unscenarioed and is waiting for the UI to pick one.
	Scenario string `json:"scenario"`
}

func findDir(candidates []string) string {
	for _, d := range candidates {
		if info, err := os.Stat(d); err == nil && info.IsDir() {
			abs, _ := filepath.Abs(d)
			return abs
		}
	}
	return ""
}

func main() {
	// Subcommands come before the server flags. `screen-assert` replays a
	// headless event log and asserts rendered-screen content (cmd_screen_assert.go),
	// the OCR-free check the scenario suite gates on. `twin` imports a real
	// deployment's bramble.exportTopology documents and reports what the
	// reconstructed mesh carries and where it is fragile (cmd_twin.go,
	// ../../docs/digital-twin.md).
	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "screen-assert":
			os.Exit(runScreenAssert(os.Args[2:]))
		case "twin":
			os.Exit(runTwin(os.Args[2:]))
		}
	}

	port := flag.Int("port", 3000, "HTTP server port")
	uiDir := flag.String("ui", "", "Path to UI static files")
	scenarioDir := flag.String("scenarios", "", "Path to scenarios")
	headless := flag.Bool("headless", false, "Run headless mode")
	scenario := flag.String("scenario", "", "Scenario file (headless)")
	noCollisions := flag.Bool("no-collisions", false,
		"Disable the collision/half-duplex model (ideal parallel channel; for baseline comparisons)")
	emuListen := flag.String("emu-listen", "",
		"emu-link unix socket path for external firmware nodes (Task 7); "+
			"when set, the broker starts for every scenario, not only those declaring firmware nodes")
	gateway := flag.String("gateway", "",
		"serial device of a PHY-passthrough gateway node (e.g. /dev/ttyUSB0); bridges the real "+
			"RF channel into the ether (emulator/DESIGN.md section 10). Pair with "+
			"--emu-listen so the gateway and the virtual nodes share a known broker socket")
	playground := flag.Bool("playground", false,
		"boot straight into the "+playgroundScenario+" scenario with the guided tour enabled "+
			"(the zero-hardware first-contact entry point; see docs/playground.md)")
	flag.Parse()

	disableCollisionModel = *noCollisions
	emuListenPath = *emuListen

	// Auto-detect scenarios dir
	if *scenarioDir == "" {
		*scenarioDir = findDir([]string{"../scenarios", "scenarios", "/scenarios"})
	}

	// Headless mode
	if *headless {
		if *scenario == "" {
			fmt.Fprintln(os.Stderr, "error: --scenario required in headless mode")
			os.Exit(1)
		}
		if err := RunHeadless(*scenario); err != nil {
			log.Fatal(err)
		}
		return
	}

	// Auto-detect UI dir
	if *uiDir == "" {
		*uiDir = findDir([]string{"../ui/dist", "ui/dist", "/ui"})
	}

	if *scenarioDir != "" {
		log.Printf("scenarios: %s", *scenarioDir)
	}
	if *uiDir != "" {
		log.Printf("ui: %s", *uiDir)
	}

	// Create sim + hub
	sim, err := NewSim(*scenarioDir, nil, false)
	if err != nil {
		log.Fatal(err)
	}
	hub := NewHub(sim)
	sim.broadcast = hub.Broadcast
	sim.Start()
	defer sim.Stop()

	// PHY passthrough gateway (../../emulator/DESIGN.md section 10): bridge a
	// real serial-attached node's RF channel into the ether. It dials the
	// broker's emu-link socket like any other node, so it needs a known path;
	// --emu-listen sets one, otherwise fall back to this process's default
	// socket.
	if *gateway != "" {
		brokerPath := *emuListen
		if brokerPath == "" {
			brokerPath = defaultEmuSocketPath()
		}
		log.Printf("gateway: bridging %s into the ether at %s", *gateway, brokerPath)
		go RunGatewaySupervised(*gateway, brokerPath)
	}

	// Playground: boot straight into the scenario instead of waiting for a
	// human to pick one. Sent through the same command channel the UI's
	// ScenarioLoader uses, so a firmware scenario auto-starts here exactly as
	// it does on the interactive load path (see Sim.handleCommand).
	if *playground {
		log.Printf("playground: loading scenario %q with the guided tour enabled", playgroundScenario)
		sim.Send(Command{Type: "start", Scenario: playgroundScenario})
	}

	// Routes
	mux := http.NewServeMux()

	// UI configuration: what this process wants the browser to do on load.
	mux.HandleFunc("/api/ui-config", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		cfg := uiConfig{}
		if *playground {
			cfg.Tour = true
			cfg.Scenario = playgroundScenario
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(cfg)
	})

	// WebSocket endpoint
	mux.HandleFunc("/ws", hub.HandleWS)

	// Scenario API
	mux.HandleFunc("/api/scenarios", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if *scenarioDir == "" {
			json.NewEncoder(w).Encode([]string{})
			return
		}
		entries, err := os.ReadDir(*scenarioDir)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		var files []string
		for _, e := range entries {
			if !e.IsDir() && strings.HasSuffix(e.Name(), ".json") {
				files = append(files, strings.TrimSuffix(e.Name(), ".json"))
			}
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(files)
	})

	mux.HandleFunc("/api/scenarios/upload", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if *scenarioDir == "" {
			http.Error(w, "no scenarios directory", http.StatusInternalServerError)
			return
		}
		r.ParseMultipartForm(10 << 20)
		file, header, err := r.FormFile("file")
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		defer file.Close()

		dst, err := os.Create(filepath.Join(*scenarioDir, header.Filename))
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		defer dst.Close()
		io.Copy(dst, file)

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{"status": "ok", "filename": header.Filename})
	})

	// Root handler: WebSocket upgrade or static files
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		// Check for WebSocket upgrade
		if r.Header.Get("Upgrade") == "websocket" {
			hub.HandleWS(w, r)
			return
		}

		// Serve static files
		if *uiDir == "" {
			if r.URL.Path == "/" {
				fmt.Fprintf(w, "bramble-gosim running. Connect via WebSocket.")
			} else {
				http.NotFound(w, r)
			}
			return
		}

		// Try to serve the file
		path := filepath.Join(*uiDir, r.URL.Path)
		if info, err := os.Stat(path); err == nil && !info.IsDir() {
			http.ServeFile(w, r, path)
			return
		}

		// SPA fallback
		index := filepath.Join(*uiDir, "index.html")
		if _, err := os.Stat(index); err == nil {
			http.ServeFile(w, r, index)
			return
		}

		http.NotFound(w, r)
	})

	addr := fmt.Sprintf(":%d", *port)
	log.Printf("listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, mux))
}
