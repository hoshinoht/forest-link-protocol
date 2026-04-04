// Package transfer implements selective-repeat ARQ and file reassembly.
package transfer

import "time"

// SelectiveRepeat implements a receiver-side selective repeat protocol.
// It tracks received chunks via a bitmap and issues NACKs for missing
// chunks within the receive window.
type SelectiveRepeat struct {
	windowSize   int
	timeoutSec   float64
	expectedBase int
	bitmap       []byte
	totalChunks  int
	lastNACKTime map[int]float64
	AckCallback  func(msgType string, seq int)
	NACKCount    int
}

// NewSelectiveRepeat creates a new SR instance.
func NewSelectiveRepeat(windowSize int, timeoutSec float64) *SelectiveRepeat {
	return &SelectiveRepeat{
		windowSize:   windowSize,
		timeoutSec:   timeoutSec,
		lastNACKTime: make(map[int]float64),
	}
}

// StartSession initialises tracking state for a transfer with totalChunks fragments.
func (sr *SelectiveRepeat) StartSession(totalChunks int) {
	sr.totalChunks = totalChunks
	bitmapSize := (totalChunks + 7) / 8
	sr.bitmap = make([]byte, bitmapSize)
	sr.expectedBase = 0
	sr.lastNACKTime = make(map[int]float64)
	sr.NACKCount = 0
}

func (sr *SelectiveRepeat) isReceived(seq int) bool {
	if seq < 0 || seq >= sr.totalChunks {
		return false
	}
	return sr.bitmap[seq/8]&(1<<uint(seq%8)) != 0
}

func (sr *SelectiveRepeat) markReceived(seq int) {
	if seq >= 0 && seq < sr.totalChunks {
		sr.bitmap[seq/8] |= 1 << uint(seq%8)
	}
}

// OnChunkReceived marks seq as received, advances the base, and sends an ACK.
func (sr *SelectiveRepeat) OnChunkReceived(seq int) {
	if seq < 0 || seq >= sr.totalChunks {
		return
	}
	sr.markReceived(seq)

	for sr.expectedBase < sr.totalChunks && sr.isReceived(sr.expectedBase) {
		sr.expectedBase++
	}

	if sr.AckCallback != nil {
		sr.AckCallback("ACK", seq)
	}
}

// CheckTimeouts sends NACKs for missing chunks in the current window,
// with a per-sequence cooldown and per-tick cap to prevent flooding.
func (sr *SelectiveRepeat) CheckTimeouts(maxPerTick int) {
	if sr.bitmap == nil {
		return
	}
	if maxPerTick <= 0 {
		maxPerTick = 8
	}
	now := float64(time.Now().UnixMilli()) / 1000.0
	end := sr.expectedBase + sr.windowSize
	if end > sr.totalChunks {
		end = sr.totalChunks
	}
	// Limit timeout probing to a near-base horizon so we do not spray far-ahead NACKs.
	const maxProbeAhead = 64
	horizon := sr.expectedBase + maxProbeAhead
	if end > horizon {
		end = horizon
	}
	const nackCooldown = 4.0
	sent := 0

	for seq := sr.expectedBase; seq < end; seq++ {
		if sr.isReceived(seq) {
			continue
		}
		lastTime, exists := sr.lastNACKTime[seq]
		if exists && (now-lastTime) < nackCooldown {
			continue
		}
		sr.lastNACKTime[seq] = now
		sr.NACKCount++
		if sr.AckCallback != nil {
			sr.AckCallback("NACK", seq)
		}
		sent++
		if sent >= maxPerTick {
			break
		}
	}
}

// CheckStall returns missing seq numbers when no data has arrived for stallSec.
// D1+B3 fix: enables the cloud to request retransmission for fragments lost
// between exit node and MQTT broker (the end-to-end reliability gap).
func (sr *SelectiveRepeat) CheckStall(lastChunkTime float64, stallSec float64) []int {
	if sr.bitmap == nil {
		return nil
	}
	now := float64(time.Now().UnixMilli()) / 1000.0
	if now-lastChunkTime < stallSec {
		return nil // still receiving, don't fire yet
	}
	const maxGaps = 32
	var missing []int
	for seq := sr.expectedBase; seq < sr.totalChunks && len(missing) < maxGaps; seq++ {
		if !sr.isReceived(seq) {
			missing = append(missing, seq)
		}
	}
	return missing
}

// IsComplete returns true when every chunk has been received.
func (sr *SelectiveRepeat) IsComplete() bool {
	return sr.expectedBase >= sr.totalChunks
}

// Progress returns the fraction of chunks received (0.0 - 1.0).
func (sr *SelectiveRepeat) Progress() float64 {
	if sr.totalChunks == 0 {
		return 0
	}
	count := 0
	for seq := 0; seq < sr.totalChunks; seq++ {
		if sr.isReceived(seq) {
			count++
		}
	}
	return float64(count) / float64(sr.totalChunks)
}
