package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	broker := flag.String("broker", "localhost", "MQTT broker hostname")
	port := flag.Int("port", 1883, "MQTT broker port")
	webPort := flag.Int("web-port", 5050, "HTTP dashboard port")
	srWindow := flag.Int("sr-window", 8, "Selective Repeat window size")
	srTimeout := flag.Float64("sr-timeout", 5.0, "Selective Repeat timeout (seconds)")
	flag.Parse()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Channels
	metaCh := make(chan FileMeta, 16)
	chunkCh := make(chan FileChunk, 256)
	topoCh := make(chan TopoMsg, 32)
	metricCh := make(chan MetricMsg, 64)

	// Components
	metrics, err := NewMetricsStore("flp_metrics.db")
	if err != nil {
		log.Fatalf("[Main] Failed to init metrics store: %v", err)
	}
	defer metrics.Close()

	topo := NewTopologyAggregator()

	mqttClient := NewMQTTClient(*broker, *port, metaCh, chunkCh, topoCh, metricCh)
	if err := mqttClient.Connect(); err != nil {
		log.Fatalf("[Main] Failed to connect to MQTT: %v", err)
	}
	defer mqttClient.Disconnect()

	progress := NewTransferProgress()

	// Transfer engine goroutine
	go RunTransferEngine(ctx, metaCh, chunkCh, mqttClient, metrics, *srWindow, *srTimeout, progress)

	// Telemetry goroutine
	go RunTelemetry(ctx, topoCh, metricCh, topo, metrics)

	// Hourly retention cleanup
	go func() {
		ticker := time.NewTicker(1 * time.Hour)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if err := metrics.Cleanup(); err != nil {
					log.Printf("[Main] Cleanup error: %v", err)
				}
			}
		}
	}()

	// HTTP server
	go StartHTTPServer(ctx, *webPort, topo, metrics, mqttClient, progress)

	fmt.Fprintf(os.Stderr, "[Admin] FLP Admin running. Dashboard at http://0.0.0.0:%d/\n", *webPort)

	<-ctx.Done()
	fmt.Fprintln(os.Stderr, "\n[Admin] Shutting down...")
}
