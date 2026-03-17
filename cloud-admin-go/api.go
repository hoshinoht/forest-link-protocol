package main

import (
	"context"
	"embed"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"
)

//go:embed static
var staticFS embed.FS

//go:embed benchmarks
var benchmarkFS embed.FS

// jsonResponse writes data as JSON with the appropriate Content-Type header.
func jsonResponse(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(data); err != nil {
		log.Printf("[http] json encode error: %v", err)
	}
}

// errorResponse writes an error JSON response with the given HTTP status code.
func errorResponse(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// StartHTTPServer registers HTTP handlers and runs the server until ctx is cancelled.
func StartHTTPServer(ctx context.Context, port int, topo *TopologyAggregator, metrics *MetricsStore, mqttClient *MQTTClient) {
	mux := http.NewServeMux()

	// GET / — serve static/index.html (and other static assets)
	staticSub, err := fs.Sub(staticFS, "static")
	if err != nil {
		log.Fatalf("[http] failed to create static sub-fs: %v", err)
	}
	fileServer := http.FileServer(http.FS(staticSub))
	mux.Handle("/", fileServer)

	// GET /api/topology
	mux.HandleFunc("/api/topology", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if topo != nil {
			jsonResponse(w, topo.ToJSON())
		} else {
			jsonResponse(w, map[string]interface{}{"nodes": []interface{}{}, "edges": []interface{}{}})
		}
	})

	// GET /api/metrics/<node_id>
	mux.HandleFunc("/api/metrics/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		nodeID := strings.TrimPrefix(r.URL.Path, "/api/metrics/")
		if nodeID == "" {
			errorResponse(w, "node_id required", http.StatusBadRequest)
			return
		}
		rangeSec := 3600
		if v := r.URL.Query().Get("range"); v != "" {
			if parsed, err := strconv.Atoi(v); err == nil {
				rangeSec = parsed
			}
		}
		if metrics != nil {
			data, err := metrics.QueryNode(nodeID, rangeSec)
			if err != nil {
				errorResponse(w, err.Error(), http.StatusInternalServerError)
				return
			}
			jsonResponse(w, data)
		} else {
			jsonResponse(w, []interface{}{})
		}
	})

	// GET /api/transfers
	mux.HandleFunc("/api/transfers", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if metrics != nil {
			data, err := metrics.QueryTransfers()
			if err != nil {
				errorResponse(w, err.Error(), http.StatusInternalServerError)
				return
			}
			jsonResponse(w, data)
		} else {
			jsonResponse(w, []interface{}{})
		}
	})

	// GET /api/heap/<node_id>
	mux.HandleFunc("/api/heap/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		nodeID := strings.TrimPrefix(r.URL.Path, "/api/heap/")
		if nodeID == "" {
			errorResponse(w, "node_id required", http.StatusBadRequest)
			return
		}
		rangeSec := 3600
		if v := r.URL.Query().Get("range"); v != "" {
			if parsed, err := strconv.Atoi(v); err == nil {
				rangeSec = parsed
			}
		}
		if metrics != nil {
			data, err := metrics.QueryHeap(nodeID, rangeSec)
			if err != nil {
				errorResponse(w, err.Error(), http.StatusInternalServerError)
				return
			}
			jsonResponse(w, data)
		} else {
			jsonResponse(w, []interface{}{})
		}
	})

	// POST /api/cmd/<node_id>
	mux.HandleFunc("/api/cmd/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		nodeID := strings.TrimPrefix(r.URL.Path, "/api/cmd/")
		if nodeID == "" {
			errorResponse(w, "node_id required", http.StatusBadRequest)
			return
		}

		// Validate node_id: must be hex, 1-4 chars
		if len(nodeID) < 1 || len(nodeID) > 4 {
			errorResponse(w, "node_id must be 1-4 hex characters", http.StatusBadRequest)
			return
		}
		targetAddr, err := strconv.ParseUint(nodeID, 16, 16)
		if err != nil {
			errorResponse(w, "node_id must be valid hex", http.StatusBadRequest)
			return
		}

		var body struct {
			Cmd  *int   `json:"cmd"`
			Data string `json:"data"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			errorResponse(w, "invalid JSON body", http.StatusBadRequest)
			return
		}

		cmdID := 1
		if body.Cmd != nil {
			cmdID = *body.Cmd
		}
		if cmdID < 1 || cmdID > 255 {
			errorResponse(w, "cmd must be 1-255", http.StatusBadRequest)
			return
		}

		var dataBytes []byte
		if body.Data != "" {
			dataBytes, err = hex.DecodeString(body.Data)
			if err != nil {
				errorResponse(w, "data must be valid hex", http.StatusBadRequest)
				return
			}
		}

		// Build binary: [target:2LE][cmd_id:1][data:N]
		payload := make([]byte, 3+len(dataBytes))
		binary.LittleEndian.PutUint16(payload[0:2], uint16(targetAddr))
		payload[2] = byte(cmdID)
		copy(payload[3:], dataBytes)

		mqttClient.PublishCmd(payload)
		jsonResponse(w, map[string]interface{}{"ok": true, "target": nodeID, "cmd": cmdID})
	})

	// POST /api/topic_msg/<node_id>
	mux.HandleFunc("/api/topic_msg/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		nodeID := strings.TrimPrefix(r.URL.Path, "/api/topic_msg/")
		if nodeID == "" {
			errorResponse(w, "node_id required", http.StatusBadRequest)
			return
		}

		// Validate node_id: must be hex, 1-4 chars
		if len(nodeID) < 1 || len(nodeID) > 4 {
			errorResponse(w, "node_id must be 1-4 hex characters", http.StatusBadRequest)
			return
		}
		targetAddr, err := strconv.ParseUint(nodeID, 16, 16)
		if err != nil {
			errorResponse(w, "node_id must be valid hex", http.StatusBadRequest)
			return
		}

		var body struct {
			Topic string `json:"topic"`
			Data  string `json:"data"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			errorResponse(w, "invalid JSON body", http.StatusBadRequest)
			return
		}
		if body.Topic == "" {
			errorResponse(w, "topic required", http.StatusBadRequest)
			return
		}
		if len(body.Topic) > 31 {
			errorResponse(w, "topic length must be <= 31", http.StatusBadRequest)
			return
		}

		var dataBytes []byte
		if body.Data != "" {
			dataBytes, err = hex.DecodeString(body.Data)
			if err != nil {
				errorResponse(w, "data must be valid hex", http.StatusBadRequest)
				return
			}
		}

		topicBytes := []byte(body.Topic)
		// Build binary: [target:2LE][cmd_id=0x10:1][topic_len:1][topic:N][payload:M]
		payload := make([]byte, 4+len(topicBytes)+len(dataBytes))
		binary.LittleEndian.PutUint16(payload[0:2], uint16(targetAddr))
		payload[2] = 0x10
		payload[3] = byte(len(topicBytes))
		copy(payload[4:], topicBytes)
		copy(payload[4+len(topicBytes):], dataBytes)

		mqttClient.PublishCmd(payload)
		jsonResponse(w, map[string]interface{}{"ok": true, "target": nodeID, "topic": body.Topic})
	})

	// GET /api/benchmarks
	mux.HandleFunc("/api/benchmarks", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if metrics != nil {
			data, err := metrics.QueryBenchmarks()
			if err != nil {
				errorResponse(w, err.Error(), http.StatusInternalServerError)
				return
			}
			jsonResponse(w, data)
		} else {
			jsonResponse(w, []interface{}{})
		}
	})

	// GET /api/comparison
	mux.HandleFunc("/api/comparison", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		var benchmarks []interface{}

		// Read benchmark JSON files from embedded FS
		entries, err := benchmarkFS.ReadDir("benchmarks")
		if err == nil {
			// Sort entries by name for deterministic order
			sort.Slice(entries, func(i, j int) bool {
				return entries[i].Name() < entries[j].Name()
			})
			for _, entry := range entries {
				if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
					continue
				}
				data, err := benchmarkFS.ReadFile("benchmarks/" + entry.Name())
				if err != nil {
					log.Printf("[http] failed to read benchmark %s: %v", entry.Name(), err)
					continue
				}
				var parsed interface{}
				if err := json.Unmarshal(data, &parsed); err != nil {
					log.Printf("[http] failed to parse benchmark %s: %v", entry.Name(), err)
					continue
				}
				benchmarks = append(benchmarks, parsed)
			}
		}

		// Prepend FLP derived metrics if available
		if metrics != nil {
			flp, err := metrics.DerivedMetrics()
			if err != nil {
				log.Printf("[http] derived metrics error: %v", err)
			} else if flp != nil {
				benchmarks = append([]interface{}{flp}, benchmarks...)
			}
		}

		if benchmarks == nil {
			benchmarks = []interface{}{}
		}
		jsonResponse(w, benchmarks)
	})

	srv := &http.Server{
		Addr:    fmt.Sprintf(":%d", port),
		Handler: mux,
	}

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdownCtx); err != nil {
			log.Printf("[http] shutdown error: %v", err)
		}
	}()

	log.Printf("[http] starting server on :%d", port)
	if err := srv.ListenAndServe(); err != http.ErrServerClosed {
		log.Printf("[http] server error: %v", err)
	}
}
