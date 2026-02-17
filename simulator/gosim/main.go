package main

import (
	"flag"
	"fmt"
	"log"
	"os"
)

func main() {
	headless := flag.Bool("headless", false, "Run in headless mode (process all events immediately)")
	scenario := flag.String("scenario", "", "Path to scenario JSON file")
	flag.Parse()

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

	fmt.Fprintln(os.Stderr, "bramble-gosim: WebSocket server mode not yet implemented")
	fmt.Fprintln(os.Stderr, "Use --headless --scenario <path> for CLI mode")
}
