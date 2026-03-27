// Package telemetry processes topology and metrics messages from the mesh.
package telemetry

import (
	"context"
	"log"

	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
	"flp-admin/internal/topology"
)

// Run processes topology and metrics messages until ctx is cancelled.
func Run(ctx context.Context, topoCh <-chan mqtt.TopoMsg, metricCh <-chan mqtt.MetricMsg, topo *topology.Aggregator, store *metrics.Store) {
	for {
		select {
		case <-ctx.Done():
			return
		case msg := <-topoCh:
			topo.Update(msg.NodeID, msg.Payload)
		case msg := <-metricCh:
			switch msg.Kind {
			case mqtt.MetricKindNode:
				if err := store.RecordNode(msg.NodeID, msg.Payload); err != nil {
					log.Printf("[telemetry] record node error: %v", err)
				}
			case mqtt.MetricKindHeap:
				topo.UpdateHeap(msg.NodeID, msg.Payload)
				if err := store.RecordHeap(msg.NodeID, msg.Payload); err != nil {
					log.Printf("[telemetry] record heap error: %v", err)
				}
			}
		}
	}
}
