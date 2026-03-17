package main

import (
	"encoding/binary"
	"fmt"
	"sync"
	"time"
)

type neighbor struct {
	Addr          string `json:"addr"`
	RSSI          int8   `json:"rssi"`
	Hops          uint8  `json:"hops"`
	HopsToInternet uint8 `json:"hops_to_internet"`
	BLE           bool   `json:"ble"`
	LoRa          bool   `json:"lora"`
	HasInternet   bool   `json:"has_internet"`
}

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

type TopologyAggregator struct {
	mu    sync.RWMutex
	nodes map[string]*nodeState
}

func NewTopologyAggregator() *TopologyAggregator {
	return &TopologyAggregator{
		nodes: make(map[string]*nodeState),
	}
}

func nowSeconds() float64 {
	return float64(time.Now().UnixMilli()) / 1000.0
}

func (ta *TopologyAggregator) Update(nodeID string, payload []byte) {
	if len(payload) < 1 {
		return
	}

	count := int(payload[0])
	neighbors := make([]neighbor, 0, count)

	for i := 0; i < count; i++ {
		off := 1 + i*6
		if off+6 > len(payload) {
			break
		}
		addr := binary.LittleEndian.Uint16(payload[off:])
		rssi := int8(payload[off+2])
		hops := payload[off+3]
		hopsInet := payload[off+4]
		flags := payload[off+5]

		neighbors = append(neighbors, neighbor{
			Addr:           fmt.Sprintf("%04X", addr),
			RSSI:           rssi,
			Hops:           hops,
			HopsToInternet: hopsInet,
			BLE:            flags&0x01 != 0,
			LoRa:           flags&0x02 != 0,
			HasInternet:    flags&0x04 != 0,
		})
	}

	ta.mu.Lock()
	defer ta.mu.Unlock()

	var existingHeap *heapInfo
	if existing, ok := ta.nodes[nodeID]; ok {
		existingHeap = existing.Heap
	}

	ta.nodes[nodeID] = &nodeState{
		LastSeen:  nowSeconds(),
		Neighbors: neighbors,
		Heap:      existingHeap,
	}
}

func (ta *TopologyAggregator) UpdateHeap(nodeID string, payload []byte) {
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

	ta.mu.Lock()
	defer ta.mu.Unlock()

	if state, ok := ta.nodes[nodeID]; ok {
		state.Heap = heap
	} else {
		ta.nodes[nodeID] = &nodeState{
			LastSeen:  nowSeconds(),
			Neighbors: []neighbor{},
			Heap:      heap,
		}
	}
}

func (ta *TopologyAggregator) ToJSON() map[string]interface{} {
	ta.mu.RLock()
	defer ta.mu.RUnlock()

	now := nowSeconds()
	nodes := []map[string]interface{}{}
	edges := []map[string]interface{}{}
	seenEdges := make(map[[2]string]bool)

	for nid, info := range ta.nodes {
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

			transport := "unknown"
			if n.BLE {
				transport = "BLE"
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

	return map[string]interface{}{
		"nodes": nodes,
		"edges": edges,
	}
}
