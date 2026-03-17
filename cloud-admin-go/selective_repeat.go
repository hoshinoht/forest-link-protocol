package main

import "time"

// CloudSelectiveRepeat implements a receiver-side selective repeat protocol
// for the cloud admin. It tracks received chunks via a bitmap and issues
// NACKs for missing chunks within the receive window.
//
// Bug fix vs Python version: NACKs are decoupled from ACKs. OnChunkReceived
// only ACKs; CheckTimeouts is the sole NACK source, with a per-sequence
// cooldown to prevent NACK flooding.
type CloudSelectiveRepeat struct {
	windowSize   int
	timeoutSec   float64
	expectedBase int
	bitmap       []byte
	totalChunks  int
	lastNACKTime map[int]float64 // per-seq NACK cooldown
	ackCallback  func(msgType string, seq int)
	NACKCount    int // total NACKs sent during this session
}

// NewCloudSelectiveRepeat creates a new SR instance.
func NewCloudSelectiveRepeat(windowSize int, timeoutSec float64) *CloudSelectiveRepeat {
	return &CloudSelectiveRepeat{
		windowSize:   windowSize,
		timeoutSec:   timeoutSec,
		lastNACKTime: make(map[int]float64),
	}
}

// StartSession initialises tracking state for a transfer with totalChunks fragments.
func (sr *CloudSelectiveRepeat) StartSession(totalChunks int) {
	sr.totalChunks = totalChunks
	bitmapSize := (totalChunks + 7) / 8
	sr.bitmap = make([]byte, bitmapSize)
	sr.expectedBase = 0
	sr.lastNACKTime = make(map[int]float64)
	sr.NACKCount = 0
}

// isReceived checks whether the bit for seq is set.
func (sr *CloudSelectiveRepeat) isReceived(seq int) bool {
	if seq < 0 || seq >= sr.totalChunks {
		return false
	}
	return sr.bitmap[seq/8]&(1<<uint(seq%8)) != 0
}

// markReceived sets the bit for seq.
func (sr *CloudSelectiveRepeat) markReceived(seq int) {
	if seq >= 0 && seq < sr.totalChunks {
		sr.bitmap[seq/8] |= 1 << uint(seq%8)
	}
}

// OnChunkReceived marks seq as received, advances the base pointer, and
// sends an ACK via the callback. It does NOT send NACKs — that is handled
// exclusively by CheckTimeouts.
func (sr *CloudSelectiveRepeat) OnChunkReceived(seq int) {
	if seq < 0 || seq >= sr.totalChunks {
		return
	}
	sr.markReceived(seq)

	// Advance base past contiguous received range.
	for sr.expectedBase < sr.totalChunks && sr.isReceived(sr.expectedBase) {
		sr.expectedBase++
	}

	// ACK this chunk.
	if sr.ackCallback != nil {
		sr.ackCallback("ACK", seq)
	}
}

// CheckTimeouts sends NACKs for missing chunks in the current window, but
// only if at least 2 seconds have elapsed since the last NACK for that seq.
func (sr *CloudSelectiveRepeat) CheckTimeouts() {
	if sr.bitmap == nil {
		return
	}
	now := float64(time.Now().UnixMilli()) / 1000.0
	end := sr.expectedBase + sr.windowSize
	if end > sr.totalChunks {
		end = sr.totalChunks
	}
	const nackCooldown = 2.0 // seconds

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
		if sr.ackCallback != nil {
			sr.ackCallback("NACK", seq)
		}
	}
}

// IsComplete returns true when every chunk has been received.
func (sr *CloudSelectiveRepeat) IsComplete() bool {
	return sr.expectedBase >= sr.totalChunks
}

// Progress returns the fraction of chunks received (0.0 – 1.0).
func (sr *CloudSelectiveRepeat) Progress() float64 {
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
