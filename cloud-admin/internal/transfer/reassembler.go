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

// fecRecovery holds a single recovered fragment returned inline from Ingest.
// B5 fix: replaces the unbounded recovered map with a single return value.
type fecRecovery struct {
	Seq  int
	Data []byte
}

// FecDecoder accumulates chunks and attempts single-erasure recovery per group.
type FecDecoder struct {
	groups map[int]*fecGroup
}

// NewFecDecoder creates a new decoder.
func NewFecDecoder() *FecDecoder {
	return &FecDecoder{
		groups: make(map[int]*fecGroup),
	}
}

// Ingest stores a chunk and attempts recovery.
// B5 fix: returns the recovered fragment inline instead of accumulating in a map.
func (f *FecDecoder) Ingest(seq int, data []byte, isParity bool) (rec *fecRecovery) {
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

func (f *FecDecoder) tryRecover(g *fecGroup, groupID int) *fecRecovery {
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
	return &fecRecovery{Seq: recoveredSeq, Data: recovered}
}

// ---------------------------------------------------------------------------
// FileReassembler
// ---------------------------------------------------------------------------

// FileReassembler collects chunks into a contiguous buffer and optionally
// uses FEC to recover lost data fragments.
type FileReassembler struct {
	SessionID          string
	Filename           string
	TotalSize          int
	ChunkCount         int
	ChunkSize          int
	ExpectedCRC        uint32
	Buffer             []byte
	Bitmap             []byte
	ChunksReceived     int // total chunks (data + parity), for progress display
	DataChunksReceived int // B1 fix: data-only counter, for completion check
	fec                *FecDecoder
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

		// B5 fix: Ingest returns a single recovered fragment inline
		rec := r.fec.Ingest(seq, data, isParity)

		if !isParity {
			group := seq / (FECGroupSize + 1)
			idxInGroup := seq % (FECGroupSize + 1)
			dataIdx := group*FECGroupSize + idxInGroup
			r.writeToBufferAt(dataIdx, data)
			r.DataChunksReceived++ // B1 fix: only count data chunks
		}

		r.Bitmap[seq/8] |= 1 << uint(seq%8)
		r.ChunksReceived++

		// B5 fix: process the single recovered fragment inline
		if rec != nil {
			if r.Bitmap[rec.Seq/8]&(1<<uint(rec.Seq%8)) == 0 {
				recGroup := rec.Seq / (FECGroupSize + 1)
				recIdx := rec.Seq % (FECGroupSize + 1)
				recDataIdx := recGroup*FECGroupSize + recIdx
				r.writeToBufferAt(recDataIdx, rec.Data)
				r.Bitmap[rec.Seq/8] |= 1 << uint(rec.Seq%8)
				r.ChunksReceived++
				r.DataChunksReceived++ // recovered fragments are always data
			}
		}
	} else {
		r.writeToBufferAt(seq, data)
		r.Bitmap[seq/8] |= 1 << uint(seq%8)
		r.ChunksReceived++
		r.DataChunksReceived++ // B1 fix: no FEC, every chunk is data
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
// B1 fix: uses DataChunksReceived (excludes parity) instead of ChunksReceived.
func (r *FileReassembler) IsComplete() bool {
	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	return r.DataChunksReceived >= dataChunkCount
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

	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	fecActive := r.ChunkCount > dataChunkCount
	fmt.Printf("[diag] Buffer=%d bytes ChunkSize=%d ChunkCount=%d DataChunks=%d FEC=%v Received=%d DataReceived=%d\n",
		r.TotalSize, r.ChunkSize, r.ChunkCount, dataChunkCount, fecActive, r.ChunksReceived, r.DataChunksReceived)

	// Missing seqs
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

	// For each missing data seq, show the buffer region it maps to
	for _, seq := range missing {
		isParity := fecActive && (seq%(FECGroupSize+1) == FECGroupSize)
		if isParity {
			continue // parity doesn't map to buffer
		}
		var dataIdx int
		if fecActive {
			group := seq / (FECGroupSize + 1)
			idxInGroup := seq % (FECGroupSize + 1)
			dataIdx = group*FECGroupSize + idxInGroup
		} else {
			dataIdx = seq
		}
		offset := dataIdx * r.ChunkSize
		end := offset + r.ChunkSize
		if end > r.TotalSize {
			end = r.TotalSize
		}
		if offset >= r.TotalSize {
			fmt.Printf("[diag] Missing seq=%d -> dataIdx=%d (beyond EOF)\n", seq, dataIdx)
			continue
		}
		region := r.Buffer[offset:end]
		allZero := true
		for _, b := range region {
			if b != 0 {
				allZero = false
				break
			}
		}
		snip := 32
		if snip > len(region) {
			snip = len(region)
		}
		fmt.Printf("[diag] Missing seq=%d -> dataIdx=%d offset=%d..%d zero=%v first32=%q\n",
			seq, dataIdx, offset, end, allZero, region[:snip])
	}

	// Zero-filled chunk scan
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

	// Head and tail context
	n := 64
	if n > r.TotalSize {
		n = r.TotalSize
	}
	fmt.Printf("[diag] First %d bytes: %q\n", n, r.Buffer[:n])

	tailStart := r.TotalSize - r.ChunkSize
	if tailStart < 0 {
		tailStart = 0
	}
	tailSnip := 64
	if tailSnip > r.TotalSize-tailStart {
		tailSnip = r.TotalSize - tailStart
	}
	fmt.Printf("[diag] Last %d bytes (offset %d): %q\n", tailSnip, tailStart, r.Buffer[tailStart:tailStart+tailSnip])
}

// Save writes the reassembled file to outputDir/<filename>.
func (r *FileReassembler) Save(outputDir string) (string, error) {
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		return "", fmt.Errorf("create output dir: %w", err)
	}
	// Sanitize: strip directory components to prevent path traversal.
	safe := filepath.Base(r.Filename)
	if safe == "." || safe == "/" {
		safe = fmt.Sprintf("session_%s.bin", r.SessionID)
	}
	path := filepath.Join(outputDir, safe)
	if err := os.WriteFile(path, r.Buffer[:r.TotalSize], 0o644); err != nil {
		return "", fmt.Errorf("write file: %w", err)
	}
	return path, nil
}

// Progress returns the fraction of data chunks received (0.0 - 1.0).
// B6 fix: uses DataChunksReceived counter instead of scanning bitmap positions
// that don't correspond to data seqs under FEC.
func (r *FileReassembler) Progress() float64 {
	dataChunkCount := (r.TotalSize + r.ChunkSize - 1) / r.ChunkSize
	if dataChunkCount == 0 {
		return 0
	}
	return float64(r.DataChunksReceived) / float64(dataChunkCount)
}
