package transfer

import (
	"encoding/json"
	"sync"
	"time"
)

// Progress is a thread-safe snapshot of the active transfer for the API.
type Progress struct {
	mu         sync.RWMutex
	SessionID  string  `json:"session_id"`
	Filename   string  `json:"filename"`
	TotalSize  int     `json:"total_size"`
	ChunkCount int     `json:"chunk_count"`
	Received   int     `json:"received"`
	ProgressV  float64 `json:"progress"`
	Active     bool    `json:"active"`
	StartedAt  float64 `json:"started_at"`
	ElapsedSec float64 `json:"elapsed_sec"`
}

// NewProgress creates an empty Progress tracker.
func NewProgress() *Progress {
	return &Progress{}
}

// Update replaces the current snapshot atomically.
func (p *Progress) Update(sessionID, filename string, totalSize, chunkCount, received int, progress float64, active bool, startedAt float64) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.SessionID = sessionID
	p.Filename = filename
	p.TotalSize = totalSize
	p.ChunkCount = chunkCount
	p.Received = received
	p.ProgressV = progress
	p.Active = active
	p.StartedAt = startedAt
	if active && startedAt > 0 {
		p.ElapsedSec = float64(time.Now().UnixMilli())/1000.0 - startedAt
	} else {
		p.ElapsedSec = 0
	}
}

// ToJSON returns the progress as a JSON blob.
func (p *Progress) ToJSON() json.RawMessage {
	p.mu.RLock()
	defer p.mu.RUnlock()

	elapsed := p.ElapsedSec
	if p.Active && p.StartedAt > 0 {
		elapsed = float64(time.Now().UnixMilli())/1000.0 - p.StartedAt
	}

	b, _ := json.Marshal(struct {
		SessionID  string  `json:"session_id"`
		Filename   string  `json:"filename"`
		TotalSize  int     `json:"total_size"`
		ChunkCount int     `json:"chunk_count"`
		Received   int     `json:"received"`
		Progress   float64 `json:"progress"`
		Active     bool    `json:"active"`
		StartedAt  float64 `json:"started_at"`
		ElapsedSec float64 `json:"elapsed_sec"`
	}{
		SessionID:  p.SessionID,
		Filename:   p.Filename,
		TotalSize:  p.TotalSize,
		ChunkCount: p.ChunkCount,
		Received:   p.Received,
		Progress:   p.ProgressV,
		Active:     p.Active,
		StartedAt:  p.StartedAt,
		ElapsedSec: elapsed,
	})
	return b
}
