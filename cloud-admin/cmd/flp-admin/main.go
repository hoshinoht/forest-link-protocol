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

	"flp-admin/internal/epoch"
	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
	"flp-admin/internal/server"
	"flp-admin/internal/telemetry"
	"flp-admin/internal/topology"
	"flp-admin/internal/transfer"
)

//go:embed static
var staticFS embed.FS


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
	srWindow := flag.Int("sr-window", 8, "Selective Repeat window size")
	srTimeout := flag.Float64("sr-timeout", 5.0, "Selective Repeat timeout (seconds)")
	mqttUser := flag.String("mqtt-user", "", "MQTT username")
	mqttPass := flag.String("mqtt-pass", "", "MQTT password")
	flag.Parse()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Channels. Sized generously: the WSS MQTT tunnel can burst, and the
	// Paho read path will drop on a full channel — larger buffers give the
	// transfer engine room to catch up before we start losing chunks.
	metaCh := make(chan mqtt.FileMeta, 64)
	chunkCh := make(chan mqtt.FileChunk, 1024)
	topoCh := make(chan mqtt.TopoMsg, 64)
	metricCh := make(chan mqtt.MetricMsg, 128)

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

	// Epoch publisher: publishes the retained flp/admin/epoch anchor every
	// second so devices reaching MQTT can slave their FTSP-style logical
	// clock to it. Bumps the cloud-admin reboot incarnation on construction.
	epochPub, err := epoch.New(store, mqttClient)
	if err != nil {
		log.Fatalf("[main] failed to init epoch publisher: %v", err)
	}
	go epochPub.Run(ctx)

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

	// HTTP server
	go server.Start(ctx, *webPort, topo, store, mqttClient, progress, epochPub, *mqttPass)

	fmt.Fprintf(os.Stderr, "[admin] FLP Admin running. Dashboard at http://0.0.0.0:%d/\n", *webPort)

	<-ctx.Done()
	fmt.Fprintln(os.Stderr, "\n[admin] shutting down...")
}
