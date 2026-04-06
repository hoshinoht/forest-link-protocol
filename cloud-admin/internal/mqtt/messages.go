// Package mqtt provides the MQTT client and channel message types for FLP.
package mqtt

import "encoding/json"

// FileMeta is received on flp/+/file/meta.
type FileMeta struct {
	NodeID       string      `json:"-"`
	SessionID    json.Number `json:"session_id"`
	SourceNode   string      `json:"src_node,omitempty"`
	Filename     string      `json:"filename"`
	TotalSize    int         `json:"total_size"`
	ChunkCount   int         `json:"chunk_count"`
	CRC32        uint32      `json:"crc32"`
	FragmentSize int         `json:"fragment_size"`
}

// FileChunk is received on flp/+/file/data.
type FileChunk struct {
	NodeID    string
	SessionID uint16 // B4 fix: session ID from wire format
	SeqNum    uint16
	Data      []byte
}

// TopoMsg carries raw topology bytes from flp/+/topology.
type TopoMsg struct {
	NodeID  string
	Payload []byte
}

// MetricKind distinguishes node-level vs heap-level metric payloads.
type MetricKind int

const (
	MetricKindNode MetricKind = iota
	MetricKindHeap
)

// MetricMsg carries raw metric bytes from flp/+/metrics or flp/+/heap.
type MetricMsg struct {
	NodeID  string
	Kind    MetricKind
	Payload []byte
}

// EpochMsg is the canonical hop schedule anchor published on the retained
// topic flp/admin/epoch. It is the ground truth that the device-side
// FTSP-style logical clock slaves to whenever any node manages to reach
// MQTT. The schedule is then a pure function of (Seed, Epoch, SlotMs).
//
// Wire format is JSON for ease of debugging — devices that subscribe to
// flp/admin/epoch decode this struct, persist Seed to NVS, and update
// their local TimeAnchor.
type EpochMsg struct {
	Epoch         uint64   `json:"epoch"`           // monotonic slot counter (now_ms / slot_ms)
	Incarnation   uint32   `json:"incarnation"`     // cloud-admin reboot counter, persisted in flp_state
	SeedHex       string   `json:"seed_hex"`        // 64 hex chars (32 random bytes), generated once
	SlotMs        uint32   `json:"slot_ms"`         // slot duration; constant for now (30000)
	WifiChannels  []uint8  `json:"wifi_channels"`   // {1, 6, 11} for 2.4 GHz non-overlapping
	LoraFreqsHz   []uint32 `json:"lora_freqs_hz"`   // 2.4 GHz LoRa frequency hop set
	PublishedAtMs int64    `json:"published_at_ms"` // cloud-admin wall clock at publish time
}
