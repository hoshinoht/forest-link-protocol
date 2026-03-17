package main

import (
	"database/sql"
	"encoding/binary"
	"fmt"
	"math"
	"time"

	_ "modernc.org/sqlite"
)

// MetricsStore provides telemetry storage backed by SQLite.
// It maintains separate write and read connections for concurrent access.
type MetricsStore struct {
	writeDB *sql.DB
	readDB  *sql.DB
}

// NewMetricsStore opens write and read connections to dbPath and initialises tables/indexes.
func NewMetricsStore(dbPath string) (*MetricsStore, error) {
	writeDB, err := sql.Open("sqlite", dbPath+"?_journal_mode=WAL&_busy_timeout=5000")
	if err != nil {
		return nil, fmt.Errorf("open write db: %w", err)
	}
	writeDB.SetMaxOpenConns(1)

	readDB, err := sql.Open("sqlite", dbPath+"?mode=ro&_journal_mode=WAL&_busy_timeout=5000")
	if err != nil {
		writeDB.Close()
		return nil, fmt.Errorf("open read db: %w", err)
	}

	m := &MetricsStore{writeDB: writeDB, readDB: readDB}
	if err := m.initDB(); err != nil {
		writeDB.Close()
		readDB.Close()
		return nil, err
	}
	return m, nil
}

func (m *MetricsStore) initDB() error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS node_metrics (
			timestamp REAL, node_id TEXT, transport TEXT,
			tx INTEGER, rx INTEGER, fail INTEGER, retransmit INTEGER,
			latency_ms INTEGER, duty_cycle_ms INTEGER
		)`,
		`CREATE TABLE IF NOT EXISTS transfer_metrics (
			session_id TEXT, start REAL, end REAL, size_bytes INTEGER,
			chunks INTEGER, retransmits INTEGER, goodput_bps REAL
		)`,
		`CREATE TABLE IF NOT EXISTS heap_metrics (
			timestamp REAL, node_id TEXT,
			free_internal INTEGER, free_psram INTEGER,
			min_internal INTEGER, min_psram INTEGER,
			largest_block INTEGER
		)`,
		`CREATE TABLE IF NOT EXISTS benchmark_results (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			session_id TEXT UNIQUE,
			timestamp REAL, filename TEXT,
			file_size INTEGER, chunk_count INTEGER, fragment_size INTEGER,
			duration_sec REAL, goodput_bps REAL, retransmits INTEGER,
			exit_node_count INTEGER, pdr REAL, latency_ms REAL,
			range_m INTEGER DEFAULT 1000, notes TEXT
		)`,
		`CREATE INDEX IF NOT EXISTS idx_node_metrics_node_ts ON node_metrics (node_id, timestamp)`,
		`CREATE INDEX IF NOT EXISTS idx_heap_metrics_node_ts ON heap_metrics (node_id, timestamp)`,
		`CREATE INDEX IF NOT EXISTS idx_transfer_metrics_start ON transfer_metrics (start)`,
	}
	for _, s := range stmts {
		if _, err := m.writeDB.Exec(s); err != nil {
			return fmt.Errorf("init db: %w", err)
		}
	}
	return nil
}

// RecordNode parses a 48-byte payload containing BLE and LoRa counters and inserts two rows.
func (m *MetricsStore) RecordNode(nodeID string, payload []byte) error {
	if len(payload) < 48 {
		return nil
	}
	now := float64(time.Now().UnixMilli()) / 1000.0
	transports := []string{"BLE", "LoRa"}

	tx, err := m.writeDB.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	for i, transport := range transports {
		off := i * 24
		// Fields in wire order: tx, fail, latency, retx, rx, duty
		txVal := binary.LittleEndian.Uint32(payload[off:])
		fail := binary.LittleEndian.Uint32(payload[off+4:])
		latency := binary.LittleEndian.Uint32(payload[off+8:])
		retx := binary.LittleEndian.Uint32(payload[off+12:])
		rx := binary.LittleEndian.Uint32(payload[off+16:])
		duty := binary.LittleEndian.Uint32(payload[off+20:])

		_, err := tx.Exec(
			"INSERT INTO node_metrics VALUES (?,?,?,?,?,?,?,?,?)",
			now, nodeID, transport, txVal, rx, fail, retx, latency, duty,
		)
		if err != nil {
			return err
		}
	}
	return tx.Commit()
}

// RecordHeap parses a 20-byte payload of heap statistics and inserts one row.
func (m *MetricsStore) RecordHeap(nodeID string, payload []byte) error {
	if len(payload) < 20 {
		return nil
	}
	now := float64(time.Now().UnixMilli()) / 1000.0
	freeInt := binary.LittleEndian.Uint32(payload[0:])
	freePs := binary.LittleEndian.Uint32(payload[4:])
	minInt := binary.LittleEndian.Uint32(payload[8:])
	minPs := binary.LittleEndian.Uint32(payload[12:])
	largest := binary.LittleEndian.Uint32(payload[16:])

	_, err := m.writeDB.Exec(
		"INSERT INTO heap_metrics VALUES (?,?,?,?,?,?,?)",
		now, nodeID, freeInt, freePs, minInt, minPs, largest,
	)
	return err
}

// RecordTransfer records a completed file transfer, computing goodput.
func (m *MetricsStore) RecordTransfer(session *TransferSession, retransmits int) error {
	elapsed := session.CompletedAt - session.StartedAt
	var goodput float64
	if elapsed > 0 {
		goodput = float64(session.TotalSize) * 8.0 / elapsed
	}
	_, err := m.writeDB.Exec(
		"INSERT INTO transfer_metrics VALUES (?,?,?,?,?,?,?)",
		session.SessionID, session.StartedAt, session.CompletedAt,
		session.TotalSize, session.ChunkCount, retransmits, goodput,
	)
	return err
}

// QueryNode returns node metrics for the given node within the last rangeSec seconds.
func (m *MetricsStore) QueryNode(nodeID string, rangeSec int) ([]map[string]interface{}, error) {
	since := float64(time.Now().UnixMilli())/1000.0 - float64(rangeSec)
	rows, err := m.readDB.Query(
		"SELECT timestamp, node_id, transport, tx, rx, fail, retransmit, latency_ms, duty_cycle_ms FROM node_metrics WHERE node_id=? AND timestamp>? ORDER BY timestamp",
		nodeID, since,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRows(rows)
}

// QueryHeap returns heap metrics for the given node within the last rangeSec seconds.
func (m *MetricsStore) QueryHeap(nodeID string, rangeSec int) ([]map[string]interface{}, error) {
	since := float64(time.Now().UnixMilli())/1000.0 - float64(rangeSec)
	rows, err := m.readDB.Query(
		"SELECT timestamp, node_id, free_internal, free_psram, min_internal, min_psram, largest_block FROM heap_metrics WHERE node_id=? AND timestamp>? ORDER BY timestamp",
		nodeID, since,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRows(rows)
}

// QueryTransfers returns the 100 most recent transfer records.
func (m *MetricsStore) QueryTransfers() ([]map[string]interface{}, error) {
	rows, err := m.readDB.Query(
		"SELECT session_id, start, end, size_bytes, chunks, retransmits, goodput_bps FROM transfer_metrics ORDER BY start DESC LIMIT 100",
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRows(rows)
}

// RecordBenchmark snapshots a self-contained benchmark row from a completed transfer.
// PDR and latency are captured from the current node_metrics so the benchmark
// row remains valid after the ephemeral telemetry is cleaned up.
func (m *MetricsStore) RecordBenchmark(session *TransferSession, retransmits int) error {
	elapsed := session.CompletedAt - session.StartedAt
	var goodput float64
	if elapsed > 0 {
		goodput = float64(session.TotalSize) * 8.0 / elapsed
	}

	// Snapshot PDR from node_metrics.
	var totalTx, totalFail sql.NullInt64
	var avgLatency sql.NullFloat64
	_ = m.readDB.QueryRow(
		"SELECT SUM(tx), SUM(fail), AVG(latency_ms) FROM node_metrics",
	).Scan(&totalTx, &totalFail, &avgLatency)

	pdr := 1.0
	if totalTx.Valid && totalTx.Int64 > 0 {
		pdr = 1.0 - float64(totalFail.Int64)/float64(totalTx.Int64)
	}
	lat := 0.0
	if avgLatency.Valid {
		lat = avgLatency.Float64
	}

	now := float64(time.Now().UnixMilli()) / 1000.0
	_, err := m.writeDB.Exec(
		`INSERT OR IGNORE INTO benchmark_results
			(session_id, timestamp, filename, file_size, chunk_count, fragment_size,
			 duration_sec, goodput_bps, retransmits, exit_node_count, pdr, latency_ms, range_m, notes)
		VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		session.SessionID, now, session.Filename, session.TotalSize,
		session.ChunkCount, session.FragmentSize, elapsed, goodput,
		retransmits, len(session.ExitNodes), pdr, lat, 1000,
		"Auto-captured from live transfer",
	)
	return err
}

// QueryBenchmarks returns all persistent benchmark rows, newest first.
func (m *MetricsStore) QueryBenchmarks() ([]map[string]interface{}, error) {
	rows, err := m.readDB.Query(
		`SELECT id, session_id, timestamp, filename, file_size, chunk_count, fragment_size,
			duration_sec, goodput_bps, retransmits, exit_node_count, pdr, latency_ms, range_m, notes
		FROM benchmark_results ORDER BY timestamp DESC`,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRows(rows)
}

// DerivedMetrics computes aggregate protocol metrics from persistent benchmark_results.
func (m *MetricsStore) DerivedMetrics() (map[string]interface{}, error) {
	var avgGoodput, avgPDR, avgLatency sql.NullFloat64
	var count int64
	err := m.readDB.QueryRow(
		"SELECT AVG(goodput_bps), AVG(pdr), AVG(latency_ms), COUNT(*) FROM benchmark_results",
	).Scan(&avgGoodput, &avgPDR, &avgLatency, &count)
	if err != nil {
		return nil, err
	}
	if count == 0 {
		return nil, nil
	}

	pdr := 1.0
	if avgPDR.Valid {
		pdr = avgPDR.Float64
	}
	lat := 0.0
	if avgLatency.Valid {
		lat = math.Round(avgLatency.Float64*100) / 100
	}

	return map[string]interface{}{
		"name":           "FLP v3.7",
		"pdr":            math.Round(pdr*10000) / 10000,
		"throughput_bps": math.Round(avgGoodput.Float64*100) / 100,
		"latency_ms":     lat,
		"range_m":        1000,
		"notes":          fmt.Sprintf("Aggregated from %d benchmark runs", count),
	}, nil
}

// Cleanup deletes metrics older than 24 hours.
func (m *MetricsStore) Cleanup() error {
	cutoff := float64(time.Now().UnixMilli())/1000.0 - 86400
	for _, table := range []string{"node_metrics", "heap_metrics"} {
		if _, err := m.writeDB.Exec(
			fmt.Sprintf("DELETE FROM %s WHERE timestamp < ?", table), cutoff,
		); err != nil {
			return err
		}
	}
	if _, err := m.writeDB.Exec("DELETE FROM transfer_metrics WHERE start < ?", cutoff); err != nil {
		return err
	}
	return nil
}

// Close closes both database connections.
func (m *MetricsStore) Close() error {
	wErr := m.writeDB.Close()
	rErr := m.readDB.Close()
	if wErr != nil {
		return wErr
	}
	return rErr
}

// scanRows converts sql.Rows into a slice of maps keyed by column name.
func scanRows(rows *sql.Rows) ([]map[string]interface{}, error) {
	cols, err := rows.Columns()
	if err != nil {
		return nil, err
	}
	var results []map[string]interface{}
	for rows.Next() {
		vals := make([]interface{}, len(cols))
		ptrs := make([]interface{}, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err := rows.Scan(ptrs...); err != nil {
			return nil, err
		}
		row := make(map[string]interface{}, len(cols))
		for i, col := range cols {
			row[col] = vals[i]
		}
		results = append(results, row)
	}
	if results == nil {
		results = []map[string]interface{}{}
	}
	return results, rows.Err()
}
