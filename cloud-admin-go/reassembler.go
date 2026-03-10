package main

import (
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
)

const (
	DefaultChunkSize = 500
	FECGroupSize     = 7
)

// ---------------------------------------------------------------------------
// FecDecoder
// ---------------------------------------------------------------------------

type fecSlot struct {
	data     []byte
	present  bool
	isParity bool
}

type fecGroup struct {
	slots [FECGroupSize + 1]fecSlot // indices 0..FECGroupSize; last is parity
	count int
}

// FecDecoder accumulates chunks and attempts single-erasure recovery per group.
type FecDecoder struct {
	groups    map[int]*fecGroup
	recovered map[int][]byte // seq → recovered data
}

// NewFecDecoder creates a new decoder.
func NewFecDecoder() *FecDecoder {
	return &FecDecoder{
		groups:    make(map[int]*fecGroup),
		recovered: make(map[int][]byte),
	}
}

// Ingest stores a chunk (data or parity) and attempts recovery when the group
// has exactly FECGroupSize slots filled.
func (f *FecDecoder) Ingest(seq int, data []byte, isParity bool) []byte {
	groupID := seq / (FECGroupSize + 1)
	idx := seq % (FECGroupSize + 1)

	g, ok := f.groups[groupID]
	if !ok {
		g = &fecGroup{}
		f.groups[groupID] = g
	}

	if g.slots[idx].present {
		return nil // duplicate
	}

	buf := make([]byte, len(data))
	copy(buf, data)
	g.slots[idx] = fecSlot{data: buf, present: true, isParity: isParity}
	g.count++

	if g.count == FECGroupSize {
		return f.tryRecover(g, groupID)
	}
	return nil
}

// tryRecover attempts to XOR-recover the single missing slot.
func (f *FecDecoder) tryRecover(g *fecGroup, groupID int) []byte {
	missing := -1
	for i := 0; i <= FECGroupSize; i++ {
		if !g.slots[i].present {
			if missing != -1 {
				return nil // more than one missing
			}
			missing = i
		}
	}
	if missing == -1 {
		return nil // nothing to recover
	}

	// If the missing slot is the parity slot, we don't need to recover it.
	if missing == FECGroupSize {
		return nil
	}

	// Determine max length across present slots.
	maxLen := 0
	for i := 0; i <= FECGroupSize; i++ {
		if g.slots[i].present && len(g.slots[i].data) > maxLen {
			maxLen = len(g.slots[i].data)
		}
	}

	recovered := make([]byte, maxLen)
	for i := 0; i <= FECGroupSize; i++ {
		if i == missing {
			continue
		}
		for j := 0; j < len(g.slots[i].data); j++ {
			recovered[j] ^= g.slots[i].data[j]
		}
	}

	g.slots[missing] = fecSlot{data: recovered, present: true}
	g.count++

	recoveredSeq := groupID*(FECGroupSize+1) + missing
	f.recovered[recoveredSeq] = recovered
	return recovered
}

// ---------------------------------------------------------------------------
// FileReassembler
// ---------------------------------------------------------------------------

// FileReassembler collects chunks into a contiguous buffer and optionally
// uses FEC to recover lost data fragments.
type FileReassembler struct {
	SessionID     string
	Filename      string
	TotalSize     int
	ChunkCount    int
	ChunkSize     int
	ExpectedCRC   uint32
	Buffer        []byte
	Bitmap        []byte
	ChunksReceived int
	fec           *FecDecoder
}

// NewFileReassembler creates a reassembler for the given transfer session parameters.
func NewFileReassembler(sessionID, filename string, totalSize, chunkCount int, expectedCRC uint32, fragmentSize int) *FileReassembler {
	cs := fragmentSize
	if cs <= 0 {
		cs = DefaultChunkSize
	}
	return &FileReassembler{
		SessionID:   sessionID,
		Filename:    filename,
		TotalSize:   totalSize,
		ChunkCount:  chunkCount,
		ChunkSize:   cs,
		ExpectedCRC: expectedCRC,
		Buffer:      make([]byte, totalSize),
		Bitmap:      make([]byte, (chunkCount+7)/8),
		fec:         NewFecDecoder(),
	}
}

// WriteChunk writes a chunk at the given sequence number. Returns true if the
// chunk was new (not a duplicate).
func (r *FileReassembler) WriteChunk(seq int, data []byte) bool {
	if seq < 0 || seq >= r.ChunkCount {
		return false
	}

	// Check duplicate.
	if r.Bitmap[seq/8]&(1<<uint(seq%8)) != 0 {
		return false
	}

	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	fecActive := r.ChunkCount > dataChunkCount

	if fecActive {
		isParity := (seq % (FECGroupSize + 1)) == FECGroupSize
		recovered := r.fec.Ingest(seq, data, isParity)

		if !isParity {
			// Map seq to data index (skip parity slots in sequence)
			group := seq / (FECGroupSize + 1)
			idxInGroup := seq % (FECGroupSize + 1)
			dataIdx := group*FECGroupSize + idxInGroup
			r.writeToBufferAt(dataIdx, data)
		}

		// Mark this seq as received.
		r.Bitmap[seq/8] |= 1 << uint(seq%8)
		r.ChunksReceived++

		// If FEC recovered a chunk, write it too.
		if recovered != nil {
			for recSeq, recData := range r.fec.recovered {
				if r.Bitmap[recSeq/8]&(1<<uint(recSeq%8)) == 0 {
					recGroup := recSeq / (FECGroupSize + 1)
					recIdx := recSeq % (FECGroupSize + 1)
					recDataIdx := recGroup*FECGroupSize + recIdx
					r.writeToBufferAt(recDataIdx, recData)
					r.Bitmap[recSeq/8] |= 1 << uint(recSeq%8)
					r.ChunksReceived++
				}
			}
		}
	} else {
		// No FEC — straight write.
		r.writeToBufferAt(seq, data)
		r.Bitmap[seq/8] |= 1 << uint(seq%8)
		r.ChunksReceived++
	}
	return true
}

// writeToBufferAt copies chunk data into the buffer at the given data-index offset.
func (r *FileReassembler) writeToBufferAt(dataIdx int, data []byte) {
	offset := dataIdx * r.ChunkSize
	end := offset + len(data)
	if end > r.TotalSize {
		end = r.TotalSize
	}
	copy(r.Buffer[offset:end], data)
}

// IsComplete returns true when all data chunks have been received (or recovered).
// Fix: compare against data chunk count, not total chunk count (which includes
// FEC parity chunks).
func (r *FileReassembler) IsComplete() bool {
	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	return r.ChunksReceived >= dataChunkCount
}

// VerifyCRC computes CRC32 of the reassembled payload and compares to expected.
func (r *FileReassembler) VerifyCRC() bool {
	actual := crc32.ChecksumIEEE(r.Buffer[:r.TotalSize])
	return actual == r.ExpectedCRC
}

// Save writes the reassembled file to outputDir/<filename> and returns the
// full path.
func (r *FileReassembler) Save(outputDir string) (string, error) {
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		return "", fmt.Errorf("create output dir: %w", err)
	}
	path := filepath.Join(outputDir, r.Filename)
	if err := os.WriteFile(path, r.Buffer[:r.TotalSize], 0o644); err != nil {
		return "", fmt.Errorf("write file: %w", err)
	}
	return path, nil
}

// Progress returns the fraction of data chunks received (0.0 – 1.0).
func (r *FileReassembler) Progress() float64 {
	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	if dataChunkCount == 0 {
		return 0
	}
	received := 0
	for seq := 0; seq < dataChunkCount; seq++ {
		if r.Bitmap[seq/8]&(1<<uint(seq%8)) != 0 {
			received++
		}
	}
	return float64(received) / float64(dataChunkCount)
}
