package main

import (
	"context"
	"embed"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
	"flp-admin/internal/server"
	"flp-admin/internal/telemetry"
	"flp-admin/internal/topology"
	"flp-admin/internal/transfer"
)

//go:embed static
var staticFS embed.FS

//go:embed benchmarks
var benchmarkFS embed.FS

func init() {
	// FIX: Force log timestamps to Singapore Standard Time (UTC+8).
	// The server was logging in UTC, making timestamps confusing when
	// operating from SGT. time.Local affects log.Printf and any code
	// using time.Now() without an explicit location.
	sgt := time.FixedZone("SGT", 8*60*60)
	time.Local = sgt
}

func main() {
	broker := flag.String("broker", "localhost", "MQTT broker hostname")
	port := flag.Int("port", 1883, "MQTT broker port")
	webPort := flag.Int("web-port", 5050, "HTTP dashboard port")
	srWindow := flag.Int("sr-window", 64, "Selective Repeat window size")
	srTimeout := flag.Float64("sr-timeout", 5.0, "Selective Repeat timeout (seconds)")
	mqttUser := flag.String("mqtt-user", "", "MQTT username")
	mqttPass := flag.String("mqtt-pass", "", "MQTT password")
	flag.Parse()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Channels
	metaCh := make(chan mqtt.FileMeta, 16)
	chunkCh := make(chan mqtt.FileChunk, 2048)
	topoCh := make(chan mqtt.TopoMsg, 32)
	metricCh := make(chan mqtt.MetricMsg, 64)

	// Components
	store, err := metrics.NewStore("flp_metrics.db")
	if err != nil {
		log.Fatalf("[main] failed to init metrics store: %v", err)
	}
	defer store.Close()

	topo := topology.NewAggregator()

	mqttClient := mqtt.NewClient(*broker, *port, *mqttUser, *mqttPass, metaCh, chunkCh, topoCh, metricCh)
	if err := mqttClient.Connect(); err != nil {
		log.Fatalf("[main] failed to connect to MQTT: %v", err)
	}
	defer mqttClient.Disconnect()

	progress := transfer.NewProgress()

	// Transfer engine goroutine
	go transfer.RunEngine(ctx, metaCh, chunkCh, mqttClient, store, *srWindow, *srTimeout, progress)

	// Telemetry goroutine
	go telemetry.Run(ctx, topoCh, metricCh, topo, store)

	// Hourly retention cleanup
	go func() {
		ticker := time.NewTicker(1 * time.Hour)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if err := store.Cleanup(); err != nil {
					log.Printf("[main] cleanup error: %v", err)
				}
			}
		}
	}()

	// Inject embedded assets into the server package
	server.StaticFS = staticFS
	server.BenchmarkFS = benchmarkFS

	// HTTP server
	go server.Start(ctx, *webPort, topo, store, mqttClient, progress, *mqttPass)

	fmt.Fprintf(os.Stderr, "[admin] FLP Admin running. Dashboard at http://0.0.0.0:%d/\n", *webPort)

	<-ctx.Done()
	fmt.Fprintln(os.Stderr, "\n[admin] shutting down...")
}
