package main

import (
	"bytes"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

// uploadRequest builds a POST multipart/form-data request carrying one "file"
// part with the given filename and body, matching what the UI's scenario
// uploader sends.
func uploadRequest(t *testing.T, filename, body string) *http.Request {
	t.Helper()
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	part, err := mw.CreateFormFile("file", filename)
	if err != nil {
		t.Fatalf("CreateFormFile: %v", err)
	}
	if _, err := part.Write([]byte(body)); err != nil {
		t.Fatalf("write part: %v", err)
	}
	if err := mw.Close(); err != nil {
		t.Fatalf("close writer: %v", err)
	}
	req := httptest.NewRequest(http.MethodPost, "/api/scenarios/upload", &buf)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	return req
}

func TestScenarioUploadStoresJSON(t *testing.T) {
	dir := t.TempDir()
	rec := httptest.NewRecorder()
	scenarioUploadHandler(dir)(rec, uploadRequest(t, "grid.json", `{"nodes":[]}`))

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body %q)", rec.Code, rec.Body.String())
	}
	got, err := os.ReadFile(filepath.Join(dir, "grid.json"))
	if err != nil {
		t.Fatalf("stored file: %v", err)
	}
	if string(got) != `{"nodes":[]}` {
		t.Fatalf("stored content = %q, want the uploaded body", got)
	}
}

// siblingDirs creates "scenarios" and "outside" side by side under one temp
// dir, so an escape from scenarios/ is observable in outside/.
func siblingDirs(t *testing.T) (scenarioDir, outside string) {
	t.Helper()
	dir := t.TempDir()
	scenarioDir = filepath.Join(dir, "scenarios")
	outside = filepath.Join(dir, "outside")
	for _, d := range []string{scenarioDir, outside} {
		if err := os.Mkdir(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	return scenarioDir, outside
}

func TestScenarioUploadConfinesTraversalName(t *testing.T) {
	scenarioDir, outside := siblingDirs(t)

	rec := httptest.NewRecorder()
	scenarioUploadHandler(scenarioDir)(rec, uploadRequest(t, "../outside/evil.json", "x"))

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body %q)", rec.Code, rec.Body.String())
	}
	if _, err := os.Stat(filepath.Join(outside, "evil.json")); !os.IsNotExist(err) {
		t.Fatalf("traversal escaped: file written into sibling directory (err %v)", err)
	}
	if _, err := os.Stat(filepath.Join(scenarioDir, "evil.json")); err != nil {
		t.Fatalf("file not stored inside scenarios dir: %v", err)
	}
}

func TestScenarioUploadRefusesEscapingSymlink(t *testing.T) {
	scenarioDir, outside := siblingDirs(t)
	target := filepath.Join(outside, "evil.json")
	if err := os.Symlink(target, filepath.Join(scenarioDir, "evil.json")); err != nil {
		t.Fatal(err)
	}

	rec := httptest.NewRecorder()
	scenarioUploadHandler(scenarioDir)(rec, uploadRequest(t, "evil.json", "x"))

	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", rec.Code)
	}
	if _, err := os.Stat(target); !os.IsNotExist(err) {
		t.Fatalf("symlink escaped: file written outside scenarios dir (err %v)", err)
	}
}

func TestIsScenarioName(t *testing.T) {
	for name, want := range map[string]bool{
		"10-node-grid":    true,
		"":                false,
		"..":              false,
		"../outside/grid": false,
		"sub/grid":        false,
		`sub\grid`:        false,
		"/etc/passwd":     false,
	} {
		if got := isScenarioName(name); got != want {
			t.Errorf("isScenarioName(%q) = %v, want %v", name, got, want)
		}
	}
}

func TestCmdLoadRefusesPathScenario(t *testing.T) {
	scenarioDir, outside := siblingDirs(t)
	grid, err := os.ReadFile(filepath.Join("..", "scenarios", "3-node-linear.json"))
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{filepath.Join(scenarioDir, "grid.json"), filepath.Join(outside, "grid.json")} {
		if err := os.WriteFile(p, grid, 0o644); err != nil {
			t.Fatal(err)
		}
	}

	sim, err := NewSim(scenarioDir, func([]byte) {}, false, "", false)
	if err != nil {
		t.Fatal(err)
	}
	sim.startPipeReader()
	defer sim.restoreStdout()
	sim.mu.Lock()
	defer sim.mu.Unlock()

	for _, name := range []string{"../outside/grid", filepath.Join(outside, "grid.json")} {
		sim.cmdLoad(Command{Scenario: name})
		if sim.state == StateLoaded {
			t.Fatalf("cmdLoad(%q) loaded a scenario from outside scenarioDir", name)
		}
	}
	sim.cmdLoad(Command{Scenario: "grid"})
	if sim.state != StateLoaded {
		t.Fatalf("state = %v after loading a bare name, want loaded", sim.state)
	}
}

func TestScenarioUploadRejectsNonJSON(t *testing.T) {
	dir := t.TempDir()
	rec := httptest.NewRecorder()
	scenarioUploadHandler(dir)(rec, uploadRequest(t, "notes.txt", "x"))

	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	if entries, _ := os.ReadDir(dir); len(entries) != 0 {
		t.Fatalf("rejected upload still wrote %d file(s)", len(entries))
	}
}

func TestScenarioUploadRejectsBadRequests(t *testing.T) {
	dir := t.TempDir()

	// Wrong method.
	rec := httptest.NewRecorder()
	scenarioUploadHandler(dir)(rec, httptest.NewRequest(http.MethodGet, "/api/scenarios/upload", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET status = %d, want 405", rec.Code)
	}

	// No scenarios directory configured.
	rec = httptest.NewRecorder()
	scenarioUploadHandler("")(rec, uploadRequest(t, "grid.json", "x"))
	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("empty-dir status = %d, want 500", rec.Code)
	}
}
