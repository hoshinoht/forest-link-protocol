package transfer

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
// FEC decoder
// ---------------------------------------------------------------------------

type fecSlot struct {
	data     []byte
	present  bool
	isParity bool
}

type fecGroup struct {
	slots [FECGroupSize + 1]fecSlot
	count int
}

// FecDecoder accumulates chunks and attempts single-erasure recovery per group.
type FecDecoder struct {
	groups    map[int]*fecGroup
	recovered map[int][]byte
}

// NewFecDecoder creates a new decoder.
func NewFecDecoder() *FecDecoder {
	return &FecDecoder{
		groups:    make(map[int]*fecGroup),
		recovered: make(map[int][]byte),
	}
}

// Ingest stores a chunk and attempts recovery.
func (f *FecDecoder) Ingest(seq int, data []byte, isParity bool) []byte {
	groupID := seq / (FECGroupSize + 1)
	idx := seq % (FECGroupSize + 1)

	g, ok := f.groups[groupID]
	if !ok {
		g = &fecGroup{}
		f.groups[groupID] = g
	}

	if g.slots[idx].present {
		return nil
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

func (f *FecDecoder) tryRecover(g *fecGroup, groupID int) []byte {
	missing := -1
	for i := 0; i <= FECGroupSize; i++ {
		if !g.slots[i].present {
			if missing != -1 {
				return nil
			}
			missing = i
		}
	}
	if missing == -1 || missing == FECGroupSize {
		return nil
	}

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
	SessionID      string
	Filename       string
	TotalSize      int
	ChunkCount     int
	ChunkSize      int
	ExpectedCRC    uint32
	Buffer         []byte
	Bitmap         []byte
	ChunksReceived int
	fec            *FecDecoder
}

// NewFileReassembler creates a reassembler for the given transfer session.
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

// WriteChunk writes a chunk at the given sequence number. Returns true if new.
func (r *FileReassembler) WriteChunk(seq int, data []byte) bool {
	if seq < 0 || seq >= r.ChunkCount {
		return false
	}

	if r.Bitmap[seq/8]&(1<<uint(seq%8)) != 0 {
		return false
	}

	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	fecActive := r.ChunkCount > dataChunkCount

	if fecActive {
		isParity := (seq % (FECGroupSize + 1)) == FECGroupSize
		recovered := r.fec.Ingest(seq, data, isParity)

		if !isParity {
			group := seq / (FECGroupSize + 1)
			idxInGroup := seq % (FECGroupSize + 1)
			dataIdx := group*FECGroupSize + idxInGroup
			r.writeToBufferAt(dataIdx, data)
		}

		r.Bitmap[seq/8] |= 1 << uint(seq%8)
		r.ChunksReceived++

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
		r.writeToBufferAt(seq, data)
		r.Bitmap[seq/8] |= 1 << uint(seq%8)
		r.ChunksReceived++
	}
	return true
}

func (r *FileReassembler) writeToBufferAt(dataIdx int, data []byte) {
	offset := dataIdx * r.ChunkSize
	end := offset + len(data)
	if end > r.TotalSize {
		end = r.TotalSize
	}
	copy(r.Buffer[offset:end], data)
}

// IsComplete returns true when all data chunks have been received or recovered.
func (r *FileReassembler) IsComplete() bool {
	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	return r.ChunksReceived >= dataChunkCount
}

// VerifyCRC computes CRC32 of the reassembled payload and compares to expected.
func (r *FileReassembler) VerifyCRC() bool {
	actual := crc32.ChecksumIEEE(r.Buffer[:r.TotalSize])
	return actual == r.ExpectedCRC
}

// DiagnoseCRC logs detailed diagnostic info when CRC verification fails.
func (r *FileReassembler) DiagnoseCRC() {
	actual := crc32.ChecksumIEEE(r.Buffer[:r.TotalSize])
	fmt.Printf("[diag] CRC expected=%d (0x%08X) actual=%d (0x%08X)\n",
		r.ExpectedCRC, r.ExpectedCRC, actual, actual)
	fmt.Printf("[diag] Buffer size=%d ChunkSize=%d ChunkCount=%d Received=%d\n",
		r.TotalSize, r.ChunkSize, r.ChunkCount, r.ChunksReceived)

	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	fmt.Printf("[diag] DataChunkCount=%d FECActive=%v\n",
		dataChunkCount, r.ChunkCount > dataChunkCount)

	missing := []int{}
	for seq := 0; seq < r.ChunkCount; seq++ {
		if r.Bitmap[seq/8]&(1<<uint(seq%8)) == 0 {
			missing = append(missing, seq)
		}
	}
	if len(missing) > 0 {
		fmt.Printf("[diag] Missing seqs (%d): %v\n", len(missing), missing)
	} else {
		fmt.Printf("[diag] All %d seqs received\n", r.ChunkCount)
	}

	zeroRuns := 0
	for i := 0; i < r.TotalSize; i += r.ChunkSize {
		end := i + r.ChunkSize
		if end > r.TotalSize {
			end = r.TotalSize
		}
		allZero := true
		for j := i; j < end; j++ {
			if r.Buffer[j] != 0 {
				allZero = false
				break
			}
		}
		if allZero {
			zeroRuns++
			fmt.Printf("[diag] Zero-filled chunk at offset %d (dataIdx=%d)\n", i, i/r.ChunkSize)
		}
	}
	if zeroRuns == 0 {
		fmt.Printf("[diag] No zero-filled gaps in buffer\n")
	}

	n := 64
	if n > r.TotalSize {
		n = r.TotalSize
	}
	fmt.Printf("[diag] First %d bytes: %q\n", n, r.Buffer[:n])

	expected := make([]byte, n)
	for i := 0; i < n; i++ {
		expected[i] = byte('A' + (i % 26))
	}
	fmt.Printf("[diag] Expected first %d: %q\n", n, expected)

	for i := 0; i < r.TotalSize; i++ {
		exp := byte('A' + (i % 26))
		if r.Buffer[i] != exp {
			end := i + 32
			if end > r.TotalSize {
				end = r.TotalSize
			}
			fmt.Printf("[diag] First mismatch at byte %d: got=0x%02X expected=0x%02X (seq~%d offset_in_chunk=%d)\n",
				i, r.Buffer[i], exp, i/r.ChunkSize, i%r.ChunkSize)
			fmt.Printf("[diag] Context [%d..%d]: %q\n", i, end, r.Buffer[i:end])
			break
		}
	}
}

// Save writes the reassembled file to outputDir/<filename>.
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

// Progress returns the fraction of data chunks received (0.0 - 1.0).
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
