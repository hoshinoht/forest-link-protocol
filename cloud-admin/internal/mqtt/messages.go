// Package mqtt provides the MQTT client and channel message types for FLP.
package mqtt

import "encoding/json"

// FileMeta is received on flp/+/file/meta.
type FileMeta struct {
	NodeID       string      `json:"-"`
	SessionID    json.Number `json:"session_id"`
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
