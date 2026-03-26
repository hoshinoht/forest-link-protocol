package main

import (
	"context"
	"encoding/json"
	"log"
	"sync"
	"time"
)

// ---------------------------------------------------------------------------
// TransferProgress — thread-safe snapshot of the active transfer for the API
// ---------------------------------------------------------------------------

type TransferProgress struct {
	mu         sync.RWMutex
	SessionID  string  `json:"session_id"`
	Filename   string  `json:"filename"`
	TotalSize  int     `json:"total_size"`
	ChunkCount int     `json:"chunk_count"`
	Received   int     `json:"received"`
	Progress   float64 `json:"progress"`
	Active     bool    `json:"active"`
	StartedAt  float64 `json:"started_at"`
	ElapsedSec float64 `json:"elapsed_sec"`
}

func NewTransferProgress() *TransferProgress {
	return &TransferProgress{}
}

func (p *TransferProgress) Update(sessionID, filename string, totalSize, chunkCount, received int, progress float64, active bool, startedAt float64) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.SessionID = sessionID
	p.Filename = filename
	p.TotalSize = totalSize
	p.ChunkCount = chunkCount
	p.Received = received
	p.Progress = progress
	p.Active = active
	p.StartedAt = startedAt
	if active && startedAt > 0 {
		p.ElapsedSec = float64(time.Now().UnixMilli())/1000.0 - startedAt
	} else {
		p.ElapsedSec = 0
	}
}

func (p *TransferProgress) ToJSON() json.RawMessage {
	p.mu.RLock()
	defer p.mu.RUnlock()

	elapsed := p.ElapsedSec
	if p.Active && p.StartedAt > 0 {
		elapsed = float64(time.Now().UnixMilli())/1000.0 - p.StartedAt
	}

	// Anonymous struct avoids copying the mutex via *p
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
		Progress:   p.Progress,
		Active:     p.Active,
		StartedAt:  p.StartedAt,
		ElapsedSec: elapsed,
	})
	return b
}

const (
	transferTimeoutSec = 60
	maxCompletedSlice  = 1000
	outputDir          = "received_files"
)

// ---------------------------------------------------------------------------
// TransferSession
// ---------------------------------------------------------------------------

// TransferSession tracks the state of a single file transfer.
type TransferSession struct {
	SessionID    string
	NodeID       string
	Filename     string
	TotalSize    int
	ChunkCount   int
	CRC32        uint32
	FragmentSize int
	StartedAt    float64
	CompletedAt  float64
	ExitNodes    map[string]bool
}

// ---------------------------------------------------------------------------
// TransferQueue
// ---------------------------------------------------------------------------

// TransferQueue manages a FIFO queue of pending transfers with at most one
// active transfer at a time. Duplicate session IDs are merged by adding the
// reporting node to the exit-node set.
type TransferQueue struct {
	queue          []*TransferSession
	ActiveTransfer *TransferSession
	Completed      []*TransferSession
	CmdCallback    func(nodeID, command, sessionID string)
}

// NewTransferQueue creates an empty queue.
func NewTransferQueue() *TransferQueue {
	return &TransferQueue{}
}

// Enqueue adds a transfer or merges into an existing session.
func (q *TransferQueue) Enqueue(sessionID, nodeID, filename string, totalSize, chunkCount int, crc32Val uint32, fragmentSize int) *TransferSession {
	// Merge into active transfer if same session.
	if q.ActiveTransfer != nil && q.ActiveTransfer.SessionID == sessionID {
		q.ActiveTransfer.ExitNodes[nodeID] = true
		return q.ActiveTransfer
	}

	// Merge into queued session.
	for _, s := range q.queue {
		if s.SessionID == sessionID {
			s.ExitNodes[nodeID] = true
			return s
		}
	}

	// New session.
	session := &TransferSession{
		SessionID:    sessionID,
		NodeID:       nodeID,
		Filename:     filename,
		TotalSize:    totalSize,
		ChunkCount:   chunkCount,
		CRC32:        crc32Val,
		FragmentSize: fragmentSize,
		ExitNodes:    map[string]bool{nodeID: true},
	}
	q.queue = append(q.queue, session)

	if q.ActiveTransfer == nil {
		q.StartNext()
	}
	return session
}

// StartNext pops the next queued transfer and makes it active.
func (q *TransferQueue) StartNext() *TransferSession {
	if q.ActiveTransfer != nil || len(q.queue) == 0 {
		return nil
	}
	q.ActiveTransfer = q.queue[0]
	q.queue = q.queue[1:]
	q.ActiveTransfer.StartedAt = float64(time.Now().UnixMilli()) / 1000.0

	if q.CmdCallback != nil {
		q.CmdCallback(q.ActiveTransfer.NodeID, "START", q.ActiveTransfer.SessionID)
	}
	return q.ActiveTransfer
}

// CompleteActive marks the current transfer as completed and starts the next.
func (q *TransferQueue) CompleteActive() {
	if q.ActiveTransfer == nil {
		return
	}
	q.ActiveTransfer.CompletedAt = float64(time.Now().UnixMilli()) / 1000.0
	q.Completed = append(q.Completed, q.ActiveTransfer)

	// Cap completed history.
	if len(q.Completed) > maxCompletedSlice {
		q.Completed = q.Completed[len(q.Completed)-maxCompletedSlice:]
	}

	q.ActiveTransfer = nil
	q.StartNext()
}

// ---------------------------------------------------------------------------
// RunTransferEngine
// ---------------------------------------------------------------------------

// RunTransferEngine is the main goroutine that orchestrates file transfers.
// It reads meta and chunk messages, drives the selective-repeat protocol,
// reassembles files, and records completed transfers in the metrics store.
func RunTransferEngine(
	ctx context.Context,
	metaCh <-chan FileMeta,
	chunkCh <-chan FileChunk,
	mqtt *MQTTClient,
	metrics *MetricsStore,
	srWindow int,
	srTimeout float64,
	progress *TransferProgress,
) {
	tq := NewTransferQueue()
	tq.CmdCallback = func(nodeID, command, sessionID string) {
		mqtt.PublishTransferCmd(nodeID, command, sessionID)
	}

	var sr *CloudSelectiveRepeat
	var reassembler *FileReassembler
	var lastChunkTime float64

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	// setupActive configures the SR and reassembler for the current active transfer.
	setupActive := func() {
		s := tq.ActiveTransfer
		if s == nil {
			sr = nil
			reassembler = nil
			progress.Update("", "", 0, 0, 0, 0, false, 0)
			return
		}
		sr = NewCloudSelectiveRepeat(srWindow, srTimeout)
		sr.ackCallback = func(msgType string, seq int) {
			mqtt.PublishACK(msgType, seq)
		}
		sr.StartSession(s.ChunkCount)
		reassembler = NewFileReassembler(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, s.CRC32, s.FragmentSize)
		lastChunkTime = float64(time.Now().UnixMilli()) / 1000.0
		progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, 0, 0, true, s.StartedAt)
		log.Printf("[transfer] started session %s (%s, %d bytes, %d chunks)",
			s.SessionID, s.Filename, s.TotalSize, s.ChunkCount)
	}

	// completeTransfer finalises the current transfer.
	completeTransfer := func(success bool) {
		s := tq.ActiveTransfer
		if s == nil {
			return
		}
		progress.Update("", "", 0, 0, 0, 0, false, 0)
		// Set CompletedAt BEFORE recording metrics (was 0 when metrics ran)
		s.CompletedAt = float64(time.Now().UnixMilli()) / 1000.0
		if success {
			elapsed := s.CompletedAt - s.StartedAt
			nacks := 0
			if sr != nil {
				nacks = sr.NACKCount
			}
			log.Printf("[transfer] completed session %s in %.1fs (%d NACKs)", s.SessionID, elapsed, nacks)
			if err := metrics.RecordTransfer(s, nacks); err != nil {
				log.Printf("[transfer] failed to record transfer: %v", err)
			}
			if err := metrics.RecordBenchmark(s, nacks); err != nil {
				log.Printf("[transfer] failed to record benchmark: %v", err)
			}
		} else {
			log.Printf("[transfer] aborted session %s (timeout)", s.SessionID)
		}
		tq.CompleteActive()
		setupActive()
	}

	for {
		select {
		case <-ctx.Done():
			return

		case meta := <-metaCh:
			sessionID := meta.SessionID.String()
			if tq.ActiveTransfer != nil {
				if tq.ActiveTransfer.SessionID == sessionID {
					// Same session from another exit node — merge (multi-exit)
					log.Printf("[transfer] merging exit node %s into session %s",
						meta.NodeID, sessionID)
				} else {
					// Different session ID while one is active.
					// Only supersede if active transfer is stale (no chunks
					// for 10s) — meaning the source died. If active is
					// healthy, queue the new one so we don't kill a working
					// transfer when two sensors start simultaneously.
					now := float64(time.Now().UnixMilli()) / 1000.0
					staleSec := now - lastChunkTime
					if staleSec > 10.0 || lastChunkTime == 0 {
						log.Printf("[transfer] superseding stale session %s (no data for %.0fs) with %s",
							tq.ActiveTransfer.SessionID, staleSec, sessionID)
						completeTransfer(false)
					} else {
						log.Printf("[transfer] session %s queued (active session %s still receiving)",
							sessionID, tq.ActiveTransfer.SessionID)
					}
				}
			}
			tq.Enqueue(sessionID, meta.NodeID, meta.Filename, meta.TotalSize, meta.ChunkCount, meta.CRC32, meta.FragmentSize)
			// If this enqueue made a new active transfer, set it up.
			if tq.ActiveTransfer != nil && sr == nil {
				setupActive()
			}

		case chunk := <-chunkCh:
			if tq.ActiveTransfer == nil || reassembler == nil || sr == nil {
				continue
			}
			seq := int(chunk.SeqNum)
			isNew := reassembler.WriteChunk(seq, chunk.Data)
			if isNew {
				sr.OnChunkReceived(seq)
				lastChunkTime = float64(time.Now().UnixMilli()) / 1000.0
				s := tq.ActiveTransfer
				progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount,
					reassembler.ChunksReceived, reassembler.Progress(), true, s.StartedAt)
			}

			// Check for completion.
			if reassembler.IsComplete() {
				if reassembler.VerifyCRC() {
					path, err := reassembler.Save(outputDir)
					if err != nil {
						log.Printf("[transfer] failed to save file: %v", err)
					} else {
						log.Printf("[transfer] saved %s", path)
					}
					completeTransfer(true)
				} else {
					log.Printf("[transfer] CRC mismatch for session %s", tq.ActiveTransfer.SessionID)
					reassembler.DiagnoseCRC()
					completeTransfer(false)
				}
			}

		case <-ticker.C:
			if sr != nil {
				sr.CheckTimeouts()
			}
			// Transfer timeout — no chunk for 60s.
			if tq.ActiveTransfer != nil && lastChunkTime > 0 {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if now-lastChunkTime > transferTimeoutSec {
					completeTransfer(false)
				}
			}
		}
	}
}
