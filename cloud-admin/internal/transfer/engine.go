package transfer

import (
	"context"
	"log"
	"time"

	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
)

const (
	transferTimeoutSec = 60
	maxCompletedSlice  = 1000
	outputDir          = "received_files"
)

// Session tracks the state of a single file transfer.
type Session struct {
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

// ToRecord converts a Session to a metrics.TransferRecord.
func (s *Session) ToRecord() *metrics.TransferRecord {
	return &metrics.TransferRecord{
		SessionID:    s.SessionID,
		NodeID:       s.NodeID,
		Filename:     s.Filename,
		TotalSize:    s.TotalSize,
		ChunkCount:   s.ChunkCount,
		FragmentSize: s.FragmentSize,
		StartedAt:    s.StartedAt,
		CompletedAt:  s.CompletedAt,
		ExitNodes:    s.ExitNodes,
	}
}

// Queue manages a FIFO queue of pending transfers with at most one
// active transfer at a time.
type Queue struct {
	queue          []*Session
	ActiveTransfer *Session
	Completed      []*Session
	CmdCallback    func(nodeID, command, sessionID string)
}

// NewQueue creates an empty queue.
func NewQueue() *Queue {
	return &Queue{}
}

// Enqueue adds a transfer or merges into an existing session.
func (q *Queue) Enqueue(sessionID, nodeID, filename string, totalSize, chunkCount int, crc32Val uint32, fragmentSize int) *Session {
	if q.ActiveTransfer != nil && q.ActiveTransfer.SessionID == sessionID {
		q.ActiveTransfer.ExitNodes[nodeID] = true
		return q.ActiveTransfer
	}

	for _, s := range q.queue {
		if s.SessionID == sessionID {
			s.ExitNodes[nodeID] = true
			return s
		}
	}

	session := &Session{
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
func (q *Queue) StartNext() *Session {
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
func (q *Queue) CompleteActive() {
	if q.ActiveTransfer == nil {
		return
	}
	q.ActiveTransfer.CompletedAt = float64(time.Now().UnixMilli()) / 1000.0
	q.Completed = append(q.Completed, q.ActiveTransfer)

	if len(q.Completed) > maxCompletedSlice {
		q.Completed = q.Completed[len(q.Completed)-maxCompletedSlice:]
	}

	q.ActiveTransfer = nil
	q.StartNext()
}

// RunEngine is the main goroutine that orchestrates file transfers.
func RunEngine(
	ctx context.Context,
	metaCh <-chan mqtt.FileMeta,
	chunkCh <-chan mqtt.FileChunk,
	mqttClient *mqtt.Client,
	store *metrics.Store,
	srWindow int,
	srTimeout float64,
	progress *Progress,
) {
	tq := NewQueue()
	tq.CmdCallback = func(nodeID, command, sessionID string) {
		mqttClient.PublishTransferCmd(nodeID, command, sessionID)
	}

	var sr *SelectiveRepeat
	var reassembler *FileReassembler
	var lastChunkTime float64

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	setupActive := func() {
		s := tq.ActiveTransfer
		if s == nil {
			sr = nil
			reassembler = nil
			progress.Update("", "", 0, 0, 0, 0, false, 0)
			return
		}
		sr = NewSelectiveRepeat(srWindow, srTimeout)
		sr.AckCallback = func(msgType string, seq int) {
			mqttClient.PublishACK(msgType, seq)
		}
		sr.StartSession(s.ChunkCount)
		reassembler = NewFileReassembler(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, s.CRC32, s.FragmentSize)
		lastChunkTime = float64(time.Now().UnixMilli()) / 1000.0
		progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, 0, 0, true, s.StartedAt)
		log.Printf("[transfer] started session %s (%s, %d bytes, %d chunks)",
			s.SessionID, s.Filename, s.TotalSize, s.ChunkCount)
	}

	completeTransfer := func(success bool) {
		s := tq.ActiveTransfer
		if s == nil {
			return
		}
		progress.Update("", "", 0, 0, 0, 0, false, 0)
		s.CompletedAt = float64(time.Now().UnixMilli()) / 1000.0
		if success {
			elapsed := s.CompletedAt - s.StartedAt
			nacks := 0
			if sr != nil {
				nacks = sr.NACKCount
			}
			log.Printf("[transfer] completed session %s in %.1fs (%d NACKs)", s.SessionID, elapsed, nacks)
			if err := store.RecordTransfer(s.ToRecord(), nacks); err != nil {
				log.Printf("[transfer] failed to record transfer: %v", err)
			}
			if err := store.RecordBenchmark(s.ToRecord(), nacks); err != nil {
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
					log.Printf("[transfer] merging exit node %s into session %s",
						meta.NodeID, sessionID)
				} else {
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
			if tq.ActiveTransfer != nil && lastChunkTime > 0 {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if now-lastChunkTime > transferTimeoutSec {
					completeTransfer(false)
				}
			}
		}
	}
}
