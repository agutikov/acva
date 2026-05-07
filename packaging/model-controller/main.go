// model-controller — M8A Step 5 sidecar that owns the docker-socket
// privilege so acva itself can stay free of it.
//
// REST surface (loopback only — bound to 127.0.0.1):
//
//   POST /llm/load    body: {"file": "<gguf-filename>"}
//   GET  /llm/status  →    {"loaded_file":"…", "alias":"…", "health":"…"}
//
// Internals:
//   - On /llm/load: rewrites packaging/compose/.env in place so
//     ACVA_LLM_MODEL=<file>, then runs `docker compose -p acva
//     --project-directory $COMPOSE_DIR up -d --force-recreate llama`.
//     Polls llama's /health (default http://127.0.0.1:8081/health)
//     until 200 or the deadline hits.
//   - /llm/status returns the controller's last successful load + the
//     llama health check result.
//
// Configuration (env vars):
//   COMPOSE_DIR     compose project root (default: /compose)
//   LLAMA_HEALTH    URL to poll for llama liveness (default
//                   http://llama:8081/health when running inside
//                   compose, or http://127.0.0.1:8081/health on host)
//   LISTEN          listen address (default 127.0.0.1:9877)
//   LOAD_TIMEOUT    seconds to wait for the recreated llama to go
//                   healthy (default 60)
//
// The binary is intentionally tiny (single file, stdlib only) so the
// runtime image is small. Build via the sibling Dockerfile.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"sync"
	"time"
)

const aliasEnvKey = "ACVA_LLM_MODEL"

type loadReq struct {
	File string `json:"file"`
}

type status struct {
	LoadedFile string `json:"loaded_file"`
	Alias      string `json:"alias"`
	Health     string `json:"health"`
}

type controller struct {
	composeDir  string
	llamaHealth string
	loadTimeout time.Duration

	mu      sync.Mutex
	state   status
	loading bool
}

func newController() *controller {
	composeDir := envOr("COMPOSE_DIR", "/compose")
	llamaHealth := envOr("LLAMA_HEALTH", "http://llama:8081/health")
	timeout := 60 * time.Second
	if v := os.Getenv("LOAD_TIMEOUT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			timeout = time.Duration(n) * time.Second
		}
	}
	c := &controller{
		composeDir:  composeDir,
		llamaHealth: llamaHealth,
		loadTimeout: timeout,
	}
	// Best-effort: read whatever's currently in .env so /llm/status
	// reports something useful before any /llm/load arrives.
	if cur, err := readEnvKey(filepath.Join(composeDir, ".env"), aliasEnvKey); err == nil {
		c.state.LoadedFile = cur
	}
	c.refreshHealthLocked()
	return c
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func (c *controller) refreshHealthLocked() {
	c.state.Health = "unknown"
	cli := http.Client{Timeout: 1 * time.Second}
	resp, err := cli.Get(c.llamaHealth)
	if err != nil {
		c.state.Health = "unhealthy"
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode == 200 {
		c.state.Health = "healthy"
	} else {
		c.state.Health = "unhealthy"
	}
}

func (c *controller) handleStatus(w http.ResponseWriter, _ *http.Request) {
	c.mu.Lock()
	c.refreshHealthLocked()
	out := c.state
	c.mu.Unlock()
	writeJSON(w, http.StatusOK, out)
}

func (c *controller) handleLoad(w http.ResponseWriter, r *http.Request) {
	defer r.Body.Close()
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
		return
	}
	var req loadReq
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, "parse body: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.File == "" {
		http.Error(w, "file is required", http.StatusBadRequest)
		return
	}

	c.mu.Lock()
	if c.loading {
		c.mu.Unlock()
		http.Error(w, "another load is in progress", http.StatusConflict)
		return
	}
	if c.state.LoadedFile == req.File && c.state.Health == "healthy" {
		out := c.state
		c.mu.Unlock()
		writeJSON(w, http.StatusOK, out)
		return
	}
	c.loading = true
	c.mu.Unlock()
	defer func() {
		c.mu.Lock()
		c.loading = false
		c.mu.Unlock()
	}()

	envPath := filepath.Join(c.composeDir, ".env")
	if err := writeEnvKey(envPath, aliasEnvKey, req.File); err != nil {
		http.Error(w, "rewrite .env: "+err.Error(), http.StatusInternalServerError)
		return
	}
	log.Printf("model-controller: rewriting .env: %s=%s", aliasEnvKey, req.File)

	ctx, cancel := context.WithTimeout(r.Context(), c.loadTimeout)
	defer cancel()

	cmd := exec.CommandContext(ctx,
		"docker", "compose",
		"-p", "acva",
		"--project-directory", c.composeDir,
		"up", "-d", "--force-recreate", "llama")
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		http.Error(w, "docker compose: "+err.Error(),
			http.StatusInternalServerError)
		return
	}

	// Poll llama's /health until 200 or deadline.
	deadline := time.Now().Add(c.loadTimeout)
	cli := http.Client{Timeout: 1 * time.Second}
	healthy := false
	for time.Now().Before(deadline) {
		resp, err := cli.Get(c.llamaHealth)
		if err == nil {
			io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
			if resp.StatusCode == 200 {
				healthy = true
				break
			}
		}
		select {
		case <-ctx.Done():
			http.Error(w, "deadline waiting for llama health",
				http.StatusGatewayTimeout)
			return
		case <-time.After(500 * time.Millisecond):
		}
	}
	if !healthy {
		http.Error(w, "llama did not report healthy within deadline",
			http.StatusGatewayTimeout)
		return
	}

	// Best-effort alias readback for the response. We deliberately
	// don't trust our cached state here; re-read the env file.
	alias, _ := readEnvKey(envPath, "ACVA_LLM_ALIAS")
	c.mu.Lock()
	c.state = status{
		LoadedFile: req.File,
		Alias:      alias,
		Health:     "healthy",
	}
	out := c.state
	c.mu.Unlock()
	writeJSON(w, http.StatusOK, out)
}

func writeJSON(w http.ResponseWriter, code int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	if err := json.NewEncoder(w).Encode(body); err != nil {
		log.Printf("write json: %v", err)
	}
}

// readEnvKey scans `path` for a line of the form `KEY=value` and
// returns `value` (stripped of surrounding quotes if present).
func readEnvKey(path, key string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	pat := regexp.MustCompile(`(?m)^\s*` + regexp.QuoteMeta(key) + `\s*=\s*(.*?)\s*$`)
	m := pat.FindStringSubmatch(string(data))
	if len(m) < 2 {
		return "", errors.New("key not found")
	}
	v := m[1]
	if len(v) >= 2 && (v[0] == '"' || v[0] == '\'') && v[len(v)-1] == v[0] {
		v = v[1 : len(v)-1]
	}
	return v, nil
}

// writeEnvKey rewrites `path` so the named key has the given value.
// If the key isn't present, appends it. Atomic swap via *.tmp.
func writeEnvKey(path, key, value string) error {
	data, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	src := string(data)
	pat := regexp.MustCompile(`(?m)^\s*` + regexp.QuoteMeta(key) + `\s*=.*$`)
	replacement := key + "=" + value
	var out string
	if pat.MatchString(src) {
		out = pat.ReplaceAllString(src, replacement)
	} else {
		if len(src) > 0 && src[len(src)-1] != '\n' {
			src += "\n"
		}
		out = src + replacement + "\n"
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(out), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func main() {
	listen := envOr("LISTEN", "127.0.0.1:9877")
	c := newController()
	mux := http.NewServeMux()
	mux.HandleFunc("/llm/status", c.handleStatus)
	mux.HandleFunc("/llm/load", c.handleLoad)
	mux.HandleFunc("/health", func(w http.ResponseWriter, _ *http.Request) {
		fmt.Fprintln(w, "ok")
	})

	srv := &http.Server{
		Addr:              listen,
		Handler:           mux,
		ReadHeaderTimeout: 2 * time.Second,
	}
	log.Printf("model-controller listening on %s (compose dir %s, llama %s)",
		listen, c.composeDir, c.llamaHealth)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("listen: %v", err)
	}
}
