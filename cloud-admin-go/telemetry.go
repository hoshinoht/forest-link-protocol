package main

import (
	"context"
	"log"
)

// RunTelemetry processes topology and metrics messages.
func RunTelemetry(ctx context.Context, topoCh <-chan TopoMsg, metricCh <-chan MetricMsg, topo *TopologyAggregator, metrics *MetricsStore) {
	for {
		select {
		case <-ctx.Done():
			return
		case msg := <-topoCh:
			topo.Update(msg.NodeID, msg.Payload)
		case msg := <-metricCh:
			switch msg.Kind {
			case MetricKindNode:
				if err := metrics.RecordNode(msg.NodeID, msg.Payload); err != nil {
					log.Printf("[Telemetry] record node error: %v", err)
				}
			case MetricKindHeap:
				topo.UpdateHeap(msg.NodeID, msg.Payload)
				if err := metrics.RecordHeap(msg.NodeID, msg.Payload); err != nil {
					log.Printf("[Telemetry] record heap error: %v", err)
				}
			}
		}
	}
}
