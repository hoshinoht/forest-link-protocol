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

	r := NewFileReassembler("session", "file.bin", len(dataChunks)*chunkSize, 8, 0, chunkSize)
	sr := NewSelectiveRepeat(8, 1)
	sr.StartSession(8)

	for seq := 1; seq <= 6; seq++ {
		isNew, recoveredSeq := r.WriteChunk(seq, dataChunks[seq])
		if !isNew {
			t.Fatalf("seq %d should be new", seq)
		}
		if recoveredSeq != -1 {
			t.Fatalf("seq %d unexpectedly recovered seq %d", seq, recoveredSeq)
		}
		sr.OnChunkReceived(seq)
	}

	if got := sr.ExpectedBase(); got != 0 {
		t.Fatalf("base before recovery = %d, want 0", got)
	}

	isNew, recoveredSeq := r.WriteChunk(7, parity)
	if !isNew {
		t.Fatal("parity chunk should be new")
	}
	if recoveredSeq != 0 {
		t.Fatalf("recovered seq = %d, want 0", recoveredSeq)
	}

	sr.OnChunkReceived(recoveredSeq)
	sr.OnChunkReceived(7)

	if got := sr.ExpectedBase(); got != 8 {
		t.Fatalf("base after recovery = %d, want 8", got)
	}
	if r.FECRecoveries != 1 {
		t.Fatalf("FEC recoveries = %d, want 1", r.FECRecoveries)
	}
}
