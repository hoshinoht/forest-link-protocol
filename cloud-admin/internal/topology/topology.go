// Package topology maintains the live mesh graph from periodic neighbor reports.
package topology

import (
	"encoding/binary"
	"fmt"
	"sync"
	"time"
)

// neighbor mirrors one entry from the firmware's route_table::serialize().
type neighbor struct {
	Addr           string `json:"addr"`
	RSSI           int8   `json:"rssi"`
	Hops           uint8  `json:"hops"`
	HopsToInternet uint8  `json:"hops_to_internet"`
	ESPNOW         bool   `json:"espnow"` // flag bit 0 = ROUTE_FLAG_ESPNOW
	LoRa           bool   `json:"lora"`   // flag bit 1 = ROUTE_FLAG_LORA
	HasInternet    bool   `json:"has_internet"`
	QueueLoad      uint8  `json:"queue_load"`
}

// heapInfo mirrors the 20-byte heap snapshot from the firmware.
type heapInfo struct {
	FreeInternal uint32 `json:"free_internal"`
	FreePSRAM    uint32 `json:"free_psram"`
	MinInternal  uint32 `json:"min_internal"`
	MinPSRAM     uint32 `json:"min_psram"`
	LargestBlock uint32 `json:"largest_block"`
}

type nodeState struct {
	LastSeen  float64
	Neighbors []neighbor
	Heap      *heapInfo
}

// Aggregator collects topology reports and produces the JSON graph.
type Aggregator struct {
	mu    sync.RWMutex
	nodes map[string]*nodeState
}

// NewAggregator creates an empty topology store.
func NewAggregator() *Aggregator {
	return &Aggregator{
		nodes: make(map[string]*nodeState),
	}
}

func nowSeconds() float64 {
	return float64(time.Now().UnixMilli()) / 1000.0
}

// Update parses a serialised neighbor table and records the node's state.
//
// Wire format (from firmware route_table::serialize):
//
//	[count:1][{addr:2(LE), rssi:1, hops:1, hops_inet:1, flags:1, queue_load:1} × N]
//	flags: bit0 = ESP-NOW reachable, bit1 = LoRa reachable, bit2 = has internet
func (a *Aggregator) Update(nodeID string, payload []byte) {
	if len(payload) < 1 {
		return
	}

	count := int(payload[0])
	neighbors := make([]neighbor, 0, count)

	for i := 0; i < count; i++ {
		off := 1 + i*7
		if off+7 > len(payload) {
			break
		}
		addr := binary.LittleEndian.Uint16(payload[off:])
		rssi := int8(payload[off+2])
		hops := payload[off+3]
		hopsInet := payload[off+4]
		flags := payload[off+5]
		queueLoad := payload[off+6]

		neighbors = append(neighbors, neighbor{
			Addr:           fmt.Sprintf("%04X", addr),
			RSSI:           rssi,
			Hops:           hops,
			HopsToInternet: hopsInet,
			ESPNOW:         flags&0x01 != 0, // FIX: was mislabelled "BLE"
			LoRa:           flags&0x02 != 0,
			HasInternet:    flags&0x04 != 0,
			QueueLoad:      queueLoad,
		})
	}

	a.mu.Lock()
	defer a.mu.Unlock()

	var existingHeap *heapInfo
	if existing, ok := a.nodes[nodeID]; ok {
		existingHeap = existing.Heap
	}

	a.nodes[nodeID] = &nodeState{
		LastSeen:  nowSeconds(),
		Neighbors: neighbors,
		Heap:      existingHeap,
	}
}

// UpdateHeap records a 20-byte heap snapshot for a node.
func (a *Aggregator) UpdateHeap(nodeID string, payload []byte) {
	if len(payload) < 20 {
		return
	}

	heap := &heapInfo{
		FreeInternal: binary.LittleEndian.Uint32(payload[0:]),
		FreePSRAM:    binary.LittleEndian.Uint32(payload[4:]),
		MinInternal:  binary.LittleEndian.Uint32(payload[8:]),
		MinPSRAM:     binary.LittleEndian.Uint32(payload[12:]),
		LargestBlock: binary.LittleEndian.Uint32(payload[16:]),
	}

	a.mu.Lock()
	defer a.mu.Unlock()

	if state, ok := a.nodes[nodeID]; ok {
		state.Heap = heap
	} else {
		a.nodes[nodeID] = &nodeState{
			LastSeen:  nowSeconds(),
			Neighbors: []neighbor{},
			Heap:      heap,
		}
	}
}

// NodeIDs returns all known node IDs.
func (a *Aggregator) NodeIDs() []string {
	a.mu.RLock()
	defer a.mu.RUnlock()
	ids := make([]string, 0, len(a.nodes))
	for id := range a.nodes {
		ids = append(ids, id)
	}
	return ids
}

// ToJSON builds the graph representation consumed by the frontend.
//
// Bug fixes vs. original:
//  1. Transport label: flag bit 0 is ESP-NOW (not BLE). The firmware sets
//     ROUTE_FLAG_ESPNOW (0x01) for ESP-NOW links.
//  2. Inferred nodes: neighbors that haven't reported their own topology
//     still appear as vertices so edges aren't dangling.
func (a *Aggregator) ToJSON() map[string]interface{} {
	a.mu.RLock()
	defer a.mu.RUnlock()

	now := nowSeconds()
	nodes := []map[string]interface{}{}
	edges := []map[string]interface{}{}
	seenEdges := make(map[[2]string]bool)
	seenNodes := make(map[string]bool)

	// First pass: reporting nodes (have sent topology data).
	for nid, info := range a.nodes {
		seenNodes[nid] = true

		status := "online"
		if now-info.LastSeen >= 60 {
			status = "offline"
		}

		minHops := uint8(255)
		for _, n := range info.Neighbors {
			if n.HopsToInternet < minHops {
				minHops = n.HopsToInternet
			}
		}

		nodeEntry := map[string]interface{}{
			"id":               nid,
			"status":           status,
			"hops_to_internet": minHops,
		}
		if info.Heap != nil {
			nodeEntry["heap"] = map[string]interface{}{
				"free_internal": info.Heap.FreeInternal,
				"free_psram":    info.Heap.FreePSRAM,
				"min_internal":  info.Heap.MinInternal,
				"min_psram":     info.Heap.MinPSRAM,
				"largest_block": info.Heap.LargestBlock,
			}
		}
		nodes = append(nodes, nodeEntry)

		for _, n := range info.Neighbors {
			a, b := nid, n.Addr
			if a > b {
				a, b = b, a
			}
			edgeKey := [2]string{a, b}
			if seenEdges[edgeKey] {
				continue
			}
			seenEdges[edgeKey] = true

			// FIX: flag bit 0 is ESP-NOW, not BLE.
			transport := "unknown"
			if n.ESPNOW {
				transport = "ESP-NOW"
			} else if n.LoRa {
				transport = "LoRa"
			}

			edges = append(edges, map[string]interface{}{
				"source":    nid,
				"target":    n.Addr,
				"rssi":      n.RSSI,
				"transport": transport,
			})
		}
	}

	// Second pass: create inferred nodes for neighbors not yet seen.
	// FIX: edges pointed to neighbor addresses that had no vertex entry,
	// causing the frontend to silently drop them or show broken links.
	for _, info := range a.nodes {
		for _, n := range info.Neighbors {
			if seenNodes[n.Addr] {
				continue
			}
			seenNodes[n.Addr] = true
			nodes = append(nodes, map[string]interface{}{
				"id":               n.Addr,
				"status":           "inferred",
				"hops_to_internet": n.HopsToInternet,
			})
		}
	}

	return map[string]interface{}{
		"nodes": nodes,
		"edges": edges,
	}
}
