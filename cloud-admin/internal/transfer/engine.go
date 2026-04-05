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

func transferKey(sessionID, nodeID string) string {
	return completedSessionKey(sessionID, nodeID)
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
	if q.ActiveTransfer != nil &&
		q.ActiveTransfer.SessionID == sessionID &&
		q.ActiveTransfer.NodeID == nodeID {
		q.ActiveTransfer.ExitNodes[nodeID] = true
		return q.ActiveTransfer
	}

	for _, s := range q.queue {
		if s.SessionID == sessionID && s.NodeID == nodeID {
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
	var stallNackRetries int
	recentlyCompletedMeta := make(map[string]time.Time)
	recentlyCompletedSession := make(map[string]time.Time)
	activeMetaLastSeen := make(map[string]float64)

	// Diagnostic counters (reset per active session in setupActive).
	var duplicateChunkDrops int
	var wrongSessionChunkDrops int
	var collidedSessionChunkDrops int
	var stagedChunkCount int
	var stagedOverflowDrops int
	var recentlyCompletedChunkDrops int
	var nextProgressLogPct int
	var lastDiagLogTime float64

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	// processChunk handles a single chunk against the active reassembler.
	// Returns (complete, success): complete=true means transfer finished.
	processChunk := func(chunk mqtt.FileChunk) (bool, bool) {
		seq := int(chunk.SeqNum)
		isNew := reassembler.WriteChunk(seq, chunk.Data)
		if !isNew {
			duplicateChunkDrops++
			if duplicateChunkDrops == 1 || duplicateChunkDrops%32 == 0 {
				log.Printf("[transfer][diag] ignored chunk seq=%d for session %s (duplicate/out-of-range, drops=%d)",
					seq, tq.ActiveTransfer.SessionID, duplicateChunkDrops)
			}
			return false, false
		}

		sr.OnChunkReceived(seq)
		lastChunkTime = float64(time.Now().UnixMilli()) / 1000.0
		s := tq.ActiveTransfer
		progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount,
			reassembler.ChunksReceived, reassembler.Progress(), true, s.StartedAt)

		dataChunkCount := (s.TotalSize + reassembler.ChunkSize - 1) / reassembler.ChunkSize
		pct := int(reassembler.Progress() * 100)
		for nextProgressLogPct > 0 && nextProgressLogPct <= 100 && pct >= nextProgressLogPct {
			log.Printf("[transfer][diag] progress session=%s %d%% (data=%d/%d chunks=%d/%d sr_base=%d pending=%d nacks=%d)",
				s.SessionID,
				nextProgressLogPct,
				reassembler.DataChunksReceived,
				dataChunkCount,
				reassembler.ChunksReceived,
				s.ChunkCount,
				sr.expectedBase,
				len(pendingChunks),
				sr.NACKCount)
			nextProgressLogPct += 10
		}

		if reassembler.IsComplete() {
			if reassembler.VerifyCRC() {
				path, err := reassembler.Save(outputDir, tq.ActiveTransfer.NodeID)
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
		lastStallNACKTime = 0
		stallNackRetries = 0
		duplicateChunkDrops = 0
		wrongSessionChunkDrops = 0
		collidedSessionChunkDrops = 0
		stagedChunkCount = 0
		stagedOverflowDrops = 0
		recentlyCompletedChunkDrops = 0
		nextProgressLogPct = 10
		lastDiagLogTime = 0
		progress.Update(s.SessionID, s.Filename, s.TotalSize, s.ChunkCount, 0, 0, true, s.StartedAt)
		log.Printf("[transfer] started session %s (node=%s file=%s size=%d chunks=%d frag=%d crc=0x%08X pending=%d queued=%d)",
			s.SessionID,
			s.NodeID,
			s.Filename,
			s.TotalSize,
			s.ChunkCount,
			s.FragmentSize,
			s.CRC32,
			len(pendingChunks),
			len(tq.queue))

		// B7 fix: replay staged chunks that match this session
		if len(pendingChunks) > 0 {
			activeKey := transferKey(s.SessionID, s.NodeID)
			kept := pendingChunks[:0]
			replayed := 0
			completed := false
			for _, c := range pendingChunks {
				cSID := fmt.Sprintf("%d", c.SessionID)
				chunkKey := transferKey(cSID, c.NodeID)
				if chunkKey != activeKey || completed {
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
				log.Printf("[transfer] replayed %d staged chunks for session %s (node=%s)", replayed, s.SessionID, s.NodeID)
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
			log.Printf("[transfer][diag] session %s stats: duplicate=%d wrong_session=%d staged=%d staged_overflow=%d completed_drop=%d",
				s.SessionID,
				duplicateChunkDrops,
				wrongSessionChunkDrops,
				stagedChunkCount,
				stagedOverflowDrops,
				recentlyCompletedChunkDrops)
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
			log.Printf("[transfer][diag] session %s stats at abort: duplicate=%d wrong_session=%d staged=%d staged_overflow=%d completed_drop=%d",
				s.SessionID,
				duplicateChunkDrops,
				wrongSessionChunkDrops,
				stagedChunkCount,
				stagedOverflowDrops,
				recentlyCompletedChunkDrops)
		}
		// Drop only staged chunks that belong to the completed transfer key.
		if len(pendingChunks) > 0 {
			completedKey := transferKey(s.SessionID, s.NodeID)
			kept := pendingChunks[:0]
			for _, c := range pendingChunks {
				cSID := fmt.Sprintf("%d", c.SessionID)
				if transferKey(cSID, c.NodeID) == completedKey {
					continue
				}
				kept = append(kept, c)
			}
			pendingChunks = kept
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
			metaTransferKey := transferKey(sessionID, meta.NodeID)
			log.Printf("[transfer][diag] meta node=%s session=%s file=%s size=%d chunks=%d frag=%d crc=0x%08X",
				meta.NodeID,
				sessionID,
				meta.Filename,
				meta.TotalSize,
				meta.ChunkCount,
				meta.FragmentSize,
				meta.CRC32)
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
				now := float64(time.Now().UnixMilli()) / 1000.0
				activeKey := transferKey(tq.ActiveTransfer.SessionID, tq.ActiveTransfer.NodeID)
				if activeKey == metaTransferKey {
					const duplicateMetaSuppressSec = 1.5
					if lastSeen, ok := activeMetaLastSeen[metaTransferKey]; ok && now-lastSeen < duplicateMetaSuppressSec {
						continue
					}
					activeMetaLastSeen[metaTransferKey] = now
					log.Printf("[transfer] merging exit node %s into session %s",
						meta.NodeID, sessionID)
				} else if tq.ActiveTransfer.SessionID == sessionID && tq.ActiveTransfer.NodeID != meta.NodeID {
					// Collision guard: different source node reused the same session ID.
					// Never let this supersede or queue against the currently active source.
					if lastSeen, ok := activeMetaLastSeen[metaTransferKey]; !ok || now-lastSeen >= 2.0 {
						log.Printf("[transfer] ignoring colliding session id %s from %s (active source=%s)",
							sessionID,
							meta.NodeID,
							tq.ActiveTransfer.NodeID)
						activeMetaLastSeen[metaTransferKey] = now
					}
					continue
				} else {
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
			if tq.ActiveTransfer != nil {
				activeMetaLastSeen[transferKey(tq.ActiveTransfer.SessionID, tq.ActiveTransfer.NodeID)] = float64(time.Now().UnixMilli()) / 1000.0
			}
			if tq.ActiveTransfer != nil && sr == nil {
				setupActive()
			}

		case chunk := <-chunkCh:
			chunkSID := fmt.Sprintf("%d", chunk.SessionID)
			chunkKey := completedSessionKey(chunkSID, chunk.NodeID)
			if expiresAt, ok := recentlyCompletedSession[chunkKey]; ok {
				if time.Now().Before(expiresAt) {
					recentlyCompletedChunkDrops++
					if recentlyCompletedChunkDrops == 1 || recentlyCompletedChunkDrops%64 == 0 {
						log.Printf("[transfer][diag] dropping chunk for completed session sid=%s node=%s seq=%d (drops=%d)",
							chunkSID, chunk.NodeID, chunk.SeqNum, recentlyCompletedChunkDrops)
					}
					continue
				}
				delete(recentlyCompletedSession, chunkKey)
			}
			// B7 fix: stage chunks if no active session yet
			if tq.ActiveTransfer == nil || reassembler == nil || sr == nil {
				if len(pendingChunks) < maxPendingChunks {
					pendingChunks = append(pendingChunks, chunk)
					stagedChunkCount++
					if stagedChunkCount == 1 || stagedChunkCount%64 == 0 {
						log.Printf("[transfer][diag] staged chunk sid=%s seq=%d pending=%d/%d",
							chunkSID, chunk.SeqNum, len(pendingChunks), maxPendingChunks)
					}
				} else {
					stagedOverflowDrops++
					if stagedOverflowDrops == 1 || stagedOverflowDrops%16 == 0 {
						log.Printf("[transfer][diag] pending buffer full, dropping chunk sid=%s seq=%d (drops=%d, cap=%d)",
							chunkSID, chunk.SeqNum, stagedOverflowDrops, maxPendingChunks)
					}
				}
				continue
			}

			// Session-ID collision from a different source should not poison wrong-session stats.
			if chunkSID == tq.ActiveTransfer.SessionID && chunk.NodeID != tq.ActiveTransfer.NodeID {
				collidedSessionChunkDrops++
				if collidedSessionChunkDrops == 1 || collidedSessionChunkDrops%64 == 0 {
					log.Printf("[transfer][diag] dropping colliding-session chunk sid=%s node=%s seq=%d (active node=%s drops=%d)",
						chunkSID,
						chunk.NodeID,
						chunk.SeqNum,
						tq.ActiveTransfer.NodeID,
						collidedSessionChunkDrops)
				}
				continue
			}

			// B4 fix: filter stale chunks from wrong session (source node + session ID).
			if chunkSID != tq.ActiveTransfer.SessionID || chunk.NodeID != tq.ActiveTransfer.NodeID {
				wrongSessionChunkDrops++
				if wrongSessionChunkDrops == 1 || wrongSessionChunkDrops%32 == 0 {
					log.Printf("[transfer][diag] dropping wrong-session chunk sid=%s node=%s seq=%d (active=%s/%s drops=%d)",
						chunkSID,
						chunk.NodeID,
						chunk.SeqNum,
						tq.ActiveTransfer.SessionID,
						tq.ActiveTransfer.NodeID,
						wrongSessionChunkDrops)
				}
				continue
			}

			complete, success := processChunk(chunk)
			if complete {
				completeTransfer(success)
			}

		case <-ticker.C:
			nowTime := time.Now()
			now := float64(time.Now().UnixMilli()) / 1000.0
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
				const timeoutNackQuietPeriodSec = 1.5
				if lastChunkTime == 0 || now-lastChunkTime >= timeoutNackQuietPeriodSec {
					sr.CheckTimeouts(6)
				}
			}
			if tq.ActiveTransfer != nil && lastChunkTime > 0 {
				if now-lastChunkTime > transferTimeoutSec {
					completeTransfer(false)
				}
			}
			if tq.ActiveTransfer != nil && sr != nil && reassembler != nil {
				if lastDiagLogTime == 0 || now-lastDiagLogTime >= 5.0 {
					s := tq.ActiveTransfer
					dataChunkCount := (s.TotalSize + reassembler.ChunkSize - 1) / reassembler.ChunkSize
					chunkAge := 0.0
					if lastChunkTime > 0 {
						chunkAge = now - lastChunkTime
					}
					log.Printf("[transfer][diag] live session=%s node=%s progress=%.1f%% data=%d/%d chunks=%d/%d base=%d pending=%d nacks=%d age=%.1fs",
						s.SessionID,
						s.NodeID,
						reassembler.Progress()*100.0,
						reassembler.DataChunksReceived,
						dataChunkCount,
						reassembler.ChunksReceived,
						s.ChunkCount,
						sr.expectedBase,
						len(pendingChunks),
						sr.NACKCount,
						chunkAge)
					lastDiagLogTime = now
				}
			}
			// D1+B3 fix: stall-based end-to-end NACK bridge.
			// If no chunks arrived for 5s but transfer is incomplete,
			// publish missing seqs so exit nodes can re-request from source.
			//
			// Phase-aware cooldown:
			//   - Mid-transfer (<95%): exponential backoff 4s→8s→16s. Mesh
			//     ARQ is still actively filling gaps; we don't want to
			//     NACK-storm while retransmits are in flight.
			//   - Tail phase (>=95%): flat 4s cooldown, no backoff. The
			//     sender's sliding window has evicted slots for these seqs,
			//     so the ONLY recovery path is cloud NACK → OOW retx on
			//     the source. Each missed opportunity here risks hitting
			//     the transfer timeout with chunks still outstanding.
			if sr != nil && reassembler != nil && !reassembler.IsComplete() {
				tailPhase := reassembler.Progress() >= 0.95

				var cooldown float64
				if tailPhase {
					cooldown = 4.0
				} else {
					cooldown = 4.0 * float64(int(1)<<stallNackRetries)
					if cooldown > 16.0 {
						cooldown = 16.0
					}
				}

				if now-lastStallNACKTime >= cooldown {
					if len(sr.CheckStall(lastChunkTime, 5.0)) > 0 {
						// In tail phase, shrink the per-seq cooldown to
						// match the batch cadence so individual seqs
						// become eligible for re-NACK on the next cycle
						// instead of sitting suppressed for 8s.
						perSeqNackCooldownSec := 8.0
						if tailPhase {
							perSeqNackCooldownSec = 4.0
						}
						const maxStallNacksPerTick = 8
						const maxProbeAhead = 64
						filtered := sr.BuildFilteredStallNACKs(maxStallNacksPerTick, maxProbeAhead, perSeqNackCooldownSec)
						if len(filtered) > 0 {
							mqttClient.PublishTransferNACK(tq.ActiveTransfer.SessionID, filtered)
							lastStallNACKTime = now
							if !tailPhase {
								stallNackRetries++
							}
							phase := "mid"
							if tailPhase {
								phase = "tail"
							}
							log.Printf("[transfer] stall detected, published %d filtered NACKs for session %s (seq=%d..%d cooldown=%.1fs per_seq=%.1fs horizon=%d phase=%s)",
								len(filtered),
								tq.ActiveTransfer.SessionID,
								filtered[0],
								filtered[len(filtered)-1],
								cooldown,
								perSeqNackCooldownSec,
								maxProbeAhead,
								phase)
						}
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
