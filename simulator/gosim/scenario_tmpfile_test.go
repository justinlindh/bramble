package main

import (
	"os"
	"path/filepath"
	"testing"
)

// writeScenarioFile writes scenarioJSON to a uniquely named file under the
// test's automatically cleaned temp directory and returns the path, failing
// the test on a write error. t.TempDir gives each call its own directory, so
// two writes in one test never collide even with the same prefix.
func writeScenarioFile(t *testing.T, namePrefix, scenarioJSON string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), namePrefix+".json")
	if err := os.WriteFile(path, []byte(scenarioJSON), 0o644); err != nil {
		t.Fatalf("write scenario file: %v", err)
	}
	return path
}

// writeAndRunScenario writes scenarioJSON to a temp file and runs it headless,
// failing the test on any write or run error.
func writeAndRunScenario(t *testing.T, namePrefix, scenarioJSON string) *scenarioRunResult {
	t.Helper()
	result, err := runScenarioHeadless(writeScenarioFile(t, namePrefix, scenarioJSON))
	if err != nil {
		t.Fatalf("runScenarioHeadless: %v", err)
	}
	return result
}
