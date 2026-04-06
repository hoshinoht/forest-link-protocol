// Package server provides the HTTP dashboard and REST API.
package server

import (
	"context"
	"crypto/subtle"
	"embed"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"flp-admin/internal/epoch"
	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
	"flp-admin/internal/topology"
	"flp-admin/internal/transfer"
)

// Assets must be set by the main package before calling Start.
var StaticFS embed.FS

func jsonResponse(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(data); err != nil {
		log.Printf("[http] json encode error: %v", err)
	}
}

func errorResponse(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func basicAuth(next http.Handler, password string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, pass, ok := r.BasicAuth()
		if !ok || subtle.ConstantTimeCompare([]byte(pass), []byte(password)) != 1 {
			w.Header().Set("WWW-Authenticate", `Basic realm="FLP Admin"`)
			http.Error(w, "Unauthorized", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// Start registers HTTP handlers and runs the server until ctx is cancelled.
func Start(ctx context.Context, port int, topo *topology.Aggregator, store *metrics.Store, mqttClient *mqtt.Client, progress *transfer.Progress, epochPub *epoch.Publisher, adminPass string) {
	mux := http.NewServeMux()

	staticSub, err := fs.Sub(StaticFS, "static")
	if err != nil {
		log.Fatalf("[http] failed to create static sub-fs: %v", err)
	}
	mux.Handle("/", http.FileServer(http.FS(staticSub)))

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

	// GET /api/epoch — debug snapshot of the most recently published hop
	// schedule anchor. Used to verify the publisher is alive and to compare
	// against device-side anchor logs when measuring convergence.
	mux.HandleFunc("/api/epoch", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		if epochPub == nil {
			errorResponse(w, "epoch publisher not running", http.StatusServiceUnavailable)
			return
		}
		jsonResponse(w, epochPub.Snapshot())
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
		if store != nil {
			data, err := store.QueryNode(nodeID, rangeSec)
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
		if store != nil {
			data, err := store.QueryTransfers()
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
		if store != nil {
			data, err := store.QueryHeap(nodeID, rangeSec)
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
			hexStr := body.Data
			hexStr = strings.TrimPrefix(hexStr, "0x")
			hexStr = strings.TrimPrefix(hexStr, "0X")
			if len(hexStr)%2 != 0 {
				hexStr = "0" + hexStr
			}
			dataBytes, err = hex.DecodeString(hexStr)
			if err != nil {
				errorResponse(w, "data must be valid hex", http.StatusBadRequest)
				return
			}
		}
		if len(dataBytes) > 64 {
			errorResponse(w, "data exceeds 64-byte firmware limit", http.StatusBadRequest)
			return
		}

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
			Topic   string `json:"topic"`
			Data    string `json:"data"`
			Message string `json:"message"`
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
		if body.Message != "" {
			// Plain text message mode
			dataBytes = []byte(body.Message)
		} else if body.Data != "" {
			// Hex data mode
			hexStr := body.Data
			hexStr = strings.TrimPrefix(hexStr, "0x")
			hexStr = strings.TrimPrefix(hexStr, "0X")
			if len(hexStr)%2 != 0 {
				hexStr = "0" + hexStr
			}
			dataBytes, err = hex.DecodeString(hexStr)
			if err != nil {
				errorResponse(w, "data must be valid hex", http.StatusBadRequest)
				return
			}
		}

		topicBytes := []byte(body.Topic)
		if len(topicBytes)+len(dataBytes) > 64 {
			errorResponse(w, "topic+data exceeds 64-byte firmware limit", http.StatusBadRequest)
			return
		}
		payload := make([]byte, 4+len(topicBytes)+len(dataBytes))
		binary.LittleEndian.PutUint16(payload[0:2], uint16(targetAddr))
		payload[2] = 0x10
		payload[3] = byte(len(topicBytes))
		copy(payload[4:], topicBytes)
		copy(payload[4+len(topicBytes):], dataBytes)

		mqttClient.PublishCmd(payload)
		result := map[string]interface{}{"ok": true, "target": nodeID, "topic": body.Topic, "bytes": len(dataBytes)}
		if body.Message != "" {
			result["message"] = body.Message
		} else if body.Data != "" {
			result["data"] = body.Data
		}
		jsonResponse(w, result)
	})


	// GET /api/active-transfer
	mux.HandleFunc("/api/active-transfer", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			errorResponse(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write(progress.ToJSON())
	})


	var handler http.Handler = mux
	if adminPass != "" {
		handler = basicAuth(mux, adminPass)
		log.Printf("[http] Basic Auth enabled (user: admin)")
	}

	srv := &http.Server{
		Addr:    fmt.Sprintf(":%d", port),
		Handler: handler,
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
