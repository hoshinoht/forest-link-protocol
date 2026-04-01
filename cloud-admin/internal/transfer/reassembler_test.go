package transfer

import "testing"

func TestWriteChunkReportsRecoveredSeqAndAdvancesSRBase(t *testing.T) {
	const chunkSize = 4
	dataChunks := [][]byte{
		[]byte("AAAA"),
		[]byte("BBBB"),
		[]byte("CCCC"),
		[]byte("DDDD"),
		[]byte("EEEE"),
		[]byte("FFFF"),
		[]byte("GGGG"),
	}

	parity := make([]byte, chunkSize)
	for _, chunk := range dataChunks {
		for i := range chunkSize {
			parity[i] ^= chunk[i]
		}
	}

	// 7 data + 1 parity = 8 total chunks (FEC group size = 7)
	r := NewFileReassembler("session", "file.bin", len(dataChunks)*chunkSize, 8, 0, chunkSize)
	sr := NewSelectiveRepeat(8, 1)
	sr.StartSession(8)

	// Send data chunks 1-6, skipping chunk 0 to test FEC recovery
	for seq := 1; seq <= 6; seq++ {
		isNew := r.WriteChunk(seq, dataChunks[seq])
		if !isNew {
			t.Fatalf("seq %d should be new", seq)
		}
		sr.OnChunkReceived(seq)
	}

	// Before recovery: chunk 0 is still missing, SR should not be complete
	if sr.IsComplete() {
		t.Fatal("SR should not be complete before parity")
	}

	// Send parity chunk (seq 7) — this should trigger FEC recovery of chunk 0
	isNew := r.WriteChunk(7, parity)
	if !isNew {
		t.Fatal("parity chunk should be new")
	}

	// After FEC recovery: all 7 data chunks should be received
	// (6 originals + 1 recovered from parity XOR)
	if r.DataChunksReceived != 7 {
		t.Fatalf("DataChunksReceived = %d, want 7", r.DataChunksReceived)
	}
	if !r.IsComplete() {
		t.Fatal("reassembler should be complete after FEC recovery")
	}

	// Verify recovered data matches original chunk 0
	got := r.Buffer[:chunkSize]
	want := dataChunks[0]
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("recovered chunk 0 mismatch at byte %d: got=%q want=%q", i, got, want)
		}
	}
}
