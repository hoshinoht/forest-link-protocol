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
// Rejects sessions that have already completed to prevent stale
// meta re-publishes from restarting finished transfers.
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

	// Reject if this session already completed successfully
	for _, s := range q.Completed {
		if s.SessionID == sessionID {
			log.Printf("[transfer] ignoring meta for already-completed session %s", sessionID)
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

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	// -- Tracing counters (reset each trace interval) --
	var traceChunksNew int       // new (non-duplicate) chunks this interval
	var traceChunksDup int       // duplicate chunks this interval
	var traceBytesRx int         // payload bytes received this interval
	var traceChunksStaged int    // chunks staged (no active session) this interval
	var traceChunksWrongSID int  // chunks filtered (wrong session) this interval
	var traceLastReport float64  // timestamp of last trace report
	const traceIntervalSec = 5.0 // report every N seconds during active transfer

	// processChunk handles a single chunk against the active reassembler.
	// Returns (complete, success): complete=true means transfer finished.
	processChunk := func(chunk mqtt.FileChunk) (bool, bool) {
		seq := int(chunk.SeqNum)
		isNew, recoveredSeq := reassembler.WriteChunk(seq, chunk.Data)
		if isNew {
			if recoveredSeq >= 0 {
				sr.OnChunkReceived(recoveredSeq)
			}
			sr.OnChunkReceived(seq)
			lastChunkTime = float64(time.Now().UnixMilli()) / 1000.0
			traceChunksNew++
			traceBytesRx += len(chunk.Data)
			s := tq.ActiveTransfer
			progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount,
				reassembler.ChunksReceived, reassembler.Progress(), true, s.StartedAt)
		} else {
			traceChunksDup++
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

		// -- Transfer summary trace --
		elapsed := s.CompletedAt - s.StartedAt
		status := "ABORT"
		if success {
			status = "OK"
		}
		nacks := 0
		acks := 0
		base := 0
		if sr != nil {
			nacks = sr.NACKCount
			acks = sr.traceACKsSent
			base = sr.ExpectedBase()
		}
		fecRec := 0
		parityRx := 0
		dataRx := 0
		totalRx := 0
		if reassembler != nil {
			fecRec = reassembler.FECRecoveries
			parityRx = reassembler.ParityReceived
			dataRx = reassembler.DataChunksReceived
			totalRx = reassembler.ChunksReceived
		}
		avgBps := 0.0
		if elapsed > 0 {
			avgBps = float64(s.TotalSize) / elapsed / 1024
		}
		log.Printf("[trace] === SESSION %s %s ===", s.SessionID, status)
		log.Printf("[trace]   file=%s size=%d chunks=%d frag_size=%d",
			s.Filename, s.TotalSize, s.ChunkCount, s.FragmentSize)
		log.Printf("[trace]   elapsed=%.1fs avg=%.1f KB/s", elapsed, avgBps)
		log.Printf("[trace]   rx: data=%d parity=%d total=%d | FEC_recoveries=%d",
			dataRx, parityRx, totalRx, fecRec)
		log.Printf("[trace]   arq: ACKs=%d NACKs=%d base=%d/%d",
			acks, nacks, base, s.ChunkCount)
		log.Printf("[trace]   exit_nodes=%v pending_staged=%d queued=%d",
			s.ExitNodes, len(pendingChunks), len(tq.queue))

		// Reset trace counters for next session
		traceChunksNew = 0
		traceChunksDup = 0
		traceBytesRx = 0
		traceChunksStaged = 0
		traceChunksWrongSID = 0
		traceLastReport = 0

		if success {
			log.Printf("[transfer] completed session %s in %.1fs (%d NACKs)", s.SessionID, elapsed, nacks)
			if err := store.RecordTransfer(s.ToRecord(), nacks); err != nil {
				log.Printf("[transfer] failed to record transfer: %v", err)
			}
			if err := store.RecordBenchmark(s.ToRecord(), nacks); err != nil {
				log.Printf("[transfer] failed to record benchmark: %v", err)
			}
			// Notify exit nodes that the session is complete.
			// This provides session consensus (Coulouris §15.5):
			// the cloud is the authoritative coordinator that
			// terminates the transfer, preventing zombie sessions
			// where the source keeps retransmitting.
			mqttClient.PublishTransferComplete(s.SessionID)
		} else {
			log.Printf("[transfer] aborted session %s (timeout)", s.SessionID)
		}
		// Clear staged chunks on completion to prevent stale data
		// from being replayed into a future session with the same ID
		pendingChunks = pendingChunks[:0]
		tq.CompleteActive()
		setupActive()
	}

	// drainChunks batch-drains up to 64 chunks from chunkCh in one go,
	// avoiding the overhead of re-entering the select loop per chunk.
	const maxBatchSize = 64
	drainChunks := func(first mqtt.FileChunk) {
		batch := make([]mqtt.FileChunk, 0, maxBatchSize)
		batch = append(batch, first)
		for len(batch) < maxBatchSize {
			select {
			case c := <-chunkCh:
				batch = append(batch, c)
			default:
				goto process
			}
		}
	process:
		for _, chunk := range batch {
			// B7 fix: stage chunks if no active session yet
			if tq.ActiveTransfer == nil || reassembler == nil || sr == nil {
				if len(pendingChunks) < maxPendingChunks {
					pendingChunks = append(pendingChunks, chunk)
					traceChunksStaged++
				}
				continue
			}

			// B4 fix: filter stale chunks from wrong session
			chunkSID := fmt.Sprintf("%d", chunk.SessionID)
			if chunkSID != tq.ActiveTransfer.SessionID {
				traceChunksWrongSID++
				continue
			}

			complete, success := processChunk(chunk)
			if complete {
				completeTransfer(success)
				return // transfer done, stop processing batch
			}
		}
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
			drainChunks(chunk)

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
			// -- Periodic trace report --
			if tq.ActiveTransfer != nil && reassembler != nil && sr != nil {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if traceLastReport == 0 {
					traceLastReport = now
				}
				elapsed := now - traceLastReport
				if elapsed >= traceIntervalSec {
					rate := float64(traceChunksNew) / elapsed
					bps := float64(traceBytesRx) / elapsed
					staleSec := now - lastChunkTime
					dataChunkCount := (reassembler.TotalSize + reassembler.ChunkSize - 1) / reassembler.ChunkSize
					log.Printf("[trace] session=%s | +%d new, %d dup, %d wrong_sid, %d staged | %d/%d data (%d/%d total) %.1f%% | %.1f chunks/s %.1f KB/s | idle=%.1fs NACKs=%d base=%d",
						tq.ActiveTransfer.SessionID,
						traceChunksNew, traceChunksDup, traceChunksWrongSID, traceChunksStaged,
						reassembler.DataChunksReceived, dataChunkCount,
						reassembler.ChunksReceived, reassembler.ChunkCount,
						reassembler.Progress()*100,
						rate, bps/1024,
						staleSec, sr.NACKCount, sr.ExpectedBase(),
					)
					traceChunksNew = 0
					traceChunksDup = 0
					traceBytesRx = 0
					traceChunksStaged = 0
					traceChunksWrongSID = 0
					traceLastReport = now
				}
			}
			// D1+B3 fix: stall-based end-to-end NACK bridge.
			// If no chunks arrived for 5s but transfer is incomplete,
			// publish missing seqs so exit nodes can re-request from source.
			// Cooldown: only fire once per 10s to avoid flooding MQTT.
			if sr != nil && reassembler != nil && !reassembler.IsComplete() {
				now := float64(time.Now().UnixMilli()) / 1000.0
				if now-lastStallNACKTime >= 15.0 {
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
