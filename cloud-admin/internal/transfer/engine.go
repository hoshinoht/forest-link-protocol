package transfer

import (
	"context"
	"fmt"
	"log"
	"time"

	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
)

const (
	transferTimeoutSec   = 60
	maxCompletedSlice    = 1000
	outputDir            = "received_files"
	recentlyCompletedTTL = 30 * time.Second
)

func completedMetaKey(sessionID, nodeID, filename string, totalSize, chunkCount int, crc32Val uint32, fragmentSize int) string {
	return fmt.Sprintf("%s|%s|%s|%d|%d|%d|%d",
		nodeID,
		sessionID,
		filename,
		totalSize,
		chunkCount,
		crc32Val,
		fragmentSize,
	)
}

func completedSessionKey(sessionID, nodeID string) string {
	return fmt.Sprintf("%s|%s", nodeID, sessionID)
}

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

	// B7 fix: staging buffer for chunks that arrive before meta
	const maxPendingChunks = 512
	pendingChunks := make([]mqtt.FileChunk, 0, 256)

	// Stall NACK cooldown: don't flood every second
	var lastStallNACKTime float64
	recentlyCompletedMeta := make(map[string]time.Time)
	recentlyCompletedSession := make(map[string]time.Time)

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	// processChunk handles a single chunk against the active reassembler.
	// Returns (complete, success): complete=true means transfer finished.
	processChunk := func(chunk mqtt.FileChunk) (bool, bool) {
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
				return true, true
			}
			log.Printf("[transfer] CRC mismatch for session %s", tq.ActiveTransfer.SessionID)
			reassembler.DiagnoseCRC()
			return true, false
		}
		return false, false
	}

	// Forward-declare so setupActive and completeTransfer can reference each other.
	var completeTransfer func(bool)

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
			// D2 fix: session-scoped ACK topic
			mqttClient.PublishACK(s.SessionID, msgType, seq)
		}
		sr.StartSession(s.ChunkCount)
		reassembler = NewFileReassembler(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, s.CRC32, s.FragmentSize)
		lastChunkTime = float64(time.Now().UnixMilli()) / 1000.0
		progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, 0, 0, true, s.StartedAt)
		log.Printf("[transfer] started session %s (%s, %d bytes, %d chunks)",
			s.SessionID, s.Filename, s.TotalSize, s.ChunkCount)

		// B7 fix: replay staged chunks that match this session
		if len(pendingChunks) > 0 {
			kept := pendingChunks[:0]
			replayed := 0
			completed := false
			for _, c := range pendingChunks {
				cSID := fmt.Sprintf("%d", c.SessionID)
				if cSID != s.SessionID || completed {
					kept = append(kept, c) // wrong session or already done
					continue
				}
				complete, success := processChunk(c)
				replayed++
				if complete {
					completeTransfer(success)
					completed = true
				}
			}
			if replayed > 0 {
				log.Printf("[transfer] replayed %d staged chunks for session %s", replayed, s.SessionID)
			}
			pendingChunks = kept
		}
	}

	completeTransfer = func(success bool) {
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
			expiresAt := time.Now().Add(recentlyCompletedTTL)
			recentlyCompletedMeta[completedMetaKey(
				s.SessionID,
				s.NodeID,
				s.Filename,
				s.TotalSize,
				s.ChunkCount,
				s.CRC32,
				s.FragmentSize,
			)] = expiresAt
			recentlyCompletedSession[completedSessionKey(s.SessionID, s.NodeID)] = expiresAt
			mqttClient.PublishTransferComplete(s.SessionID)
			if err := store.RecordTransfer(s.ToRecord(), nacks); err != nil {
				log.Printf("[transfer] failed to record transfer: %v", err)
			}
			if err := store.RecordBenchmark(s.ToRecord(), nacks); err != nil {
				log.Printf("[transfer] failed to record benchmark: %v", err)
			}
		} else {
			log.Printf("[transfer] aborted session %s (timeout)", s.SessionID)
		}
		// Clear staged chunks on completion to prevent stale data
		// from being replayed into a future session with the same ID
		pendingChunks = pendingChunks[:0]
		tq.CompleteActive()
		setupActive()
	}

	for {
		select {
		case <-ctx.Done():
			return

		case meta := <-metaCh:
			sessionID := meta.SessionID.String()
			metaKey := completedMetaKey(
				sessionID,
				meta.NodeID,
				meta.Filename,
				meta.TotalSize,
				meta.ChunkCount,
				meta.CRC32,
				meta.FragmentSize,
			)
			if expiresAt, ok := recentlyCompletedMeta[metaKey]; ok {
				if time.Now().Before(expiresAt) {
					log.Printf("[transfer] ignoring duplicate meta for recently completed session %s from %s",
						sessionID, meta.NodeID)
					continue
				}
				delete(recentlyCompletedMeta, metaKey)
			}
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
			chunkSID := fmt.Sprintf("%d", chunk.SessionID)
			chunkKey := completedSessionKey(chunkSID, chunk.NodeID)
			if expiresAt, ok := recentlyCompletedSession[chunkKey]; ok {
				if time.Now().Before(expiresAt) {
					continue
				}
				delete(recentlyCompletedSession, chunkKey)
			}
			// B7 fix: stage chunks if no active session yet
			if tq.ActiveTransfer == nil || reassembler == nil || sr == nil {
				if len(pendingChunks) < maxPendingChunks {
					pendingChunks = append(pendingChunks, chunk)
				}
				continue
			}

			// B4 fix: filter stale chunks from wrong session
			if chunkSID != tq.ActiveTransfer.SessionID {
				continue
			}

			complete, success := processChunk(chunk)
			if complete {
				completeTransfer(success)
			}

		case <-ticker.C:
			nowTime := time.Now()
			for key, expiresAt := range recentlyCompletedMeta {
				if !expiresAt.After(nowTime) {
					delete(recentlyCompletedMeta, key)
				}
			}
			for key, expiresAt := range recentlyCompletedSession {
				if !expiresAt.After(nowTime) {
					delete(recentlyCompletedSession, key)
				}
			}
			if sr != nil {
				sr.CheckTimeouts()
			}
			if tq.ActiveTransfer != nil && lastChunkTime > 0 {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if now-lastChunkTime > transferTimeoutSec {
					completeTransfer(false)
				}
			}
			// D1+B3 fix: stall-based end-to-end NACK bridge.
			// If no chunks arrived for 5s but transfer is incomplete,
			// publish missing seqs so exit nodes can re-request from source.
			// Cooldown: only fire once per 10s to avoid flooding MQTT.
			if sr != nil && reassembler != nil && !reassembler.IsComplete() {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if now-lastStallNACKTime >= 10.0 {
					gaps := sr.CheckStall(lastChunkTime, 5.0)
					if len(gaps) > 0 {
						mqttClient.PublishTransferNACK(tq.ActiveTransfer.SessionID, gaps)
						lastStallNACKTime = now
						log.Printf("[transfer] stall detected, published %d NACKs for session %s",
							len(gaps), tq.ActiveTransfer.SessionID)
					}
				}
			}
			// B7: garbage-collect stale pending chunks (>30s old is impossible
			// since sessions time out at 60s; just cap the buffer)
			if tq.ActiveTransfer == nil && len(pendingChunks) > 0 {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if now-lastChunkTime > 30 {
					if len(pendingChunks) > 0 {
						log.Printf("[transfer] discarding %d orphan staged chunks", len(pendingChunks))
						pendingChunks = pendingChunks[:0]
					}
				}
			}
		}
	}
}
