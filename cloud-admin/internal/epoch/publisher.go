// Package epoch publishes the canonical FLP hop schedule anchor on the
// retained MQTT topic flp/admin/epoch.
//
// Devices use this as the ground truth for an FTSP-style logical clock:
// whenever any node manages to reach MQTT it pulls the anchor and floods
// it through the mesh, where it propagates by a max-register CRDT merge
// rule (highest incarnation wins, tiebreak by epoch). The HMAC channel
// schedule is then a pure function of (Seed, Epoch, SlotMs) so converged
// nodes compute the same WiFi/LoRa frequencies independently.
//
// cloud-admin owns three pieces of state for this:
//   - incarnation: bumped on every cloud-admin boot, persisted in
//     flp_state. Acts as the top-level fence on the device side.
//   - hop_seed: 32 random bytes generated once on first run, persisted
//     forever. Rotation is intentionally out of scope for now.
//   - epoch: derived from wall clock as (now_ms / slot_ms). cloud-admin
//     restarts do not reset this — the incarnation handles that — so the
//     slot counter advances monotonically across reboots.
package epoch

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"log"
	"sync"
	"time"

	"flp-admin/internal/metrics"
	"flp-admin/internal/mqtt"
)

// SlotMs is the hop slot duration. Conservative for the first cut: 30 s
// gives ~60 hours of clock-drift margin on a 50 ppm XTAL, more than
// enough for an unattended forest deployment with intermittent uplink.
const SlotMs uint32 = 30000

// PublishIntervalMs is how often we re-publish the retained epoch
// message. The message is retained, so subscribers always have the latest
// value immediately on subscribe; the periodic re-publish exists so that
// any anchor staleness on the broker (or a bounce of the broker) does not
// leave devices stuck on a value that is now hours old.
const PublishIntervalMs = 1000

// Pre-provisioned hop sets. Devices must agree on these constants too.
// Not in the wire message yet — they go in the wire message so a future
// rotation does not require reflashing devices.
var (
	WifiChannels = []uint8{1, 6, 11}
	LoraFreqsHz  = []uint32{2_403_000_000, 2_425_000_000, 2_479_000_000}
)

const (
	stateKeyIncarnation = "incarnation"
	stateKeyHopSeed     = "hop_seed"
	hopSeedSize         = 32
)

// Publisher owns the incarnation+seed state and runs the periodic
// publish loop. Construct via New, then call Run inside a goroutine.
// Snapshot is safe for concurrent reads (e.g. by the HTTP debug handler).
type Publisher struct {
	store *metrics.Store
	mqtt  *mqtt.Client

	incarnation uint32
	seed        [hopSeedSize]byte
	seedHex     string

	mu   sync.RWMutex
	last Snapshot
}

// Snapshot is a read-only view of what the publisher last published. The
// debug HTTP handler returns it as JSON.
type Snapshot struct {
	Epoch         uint64 `json:"epoch"`
	Incarnation   uint32 `json:"incarnation"`
	SeedSHA256    string `json:"seed_sha256"` // hex of sha256(seed) for fingerprinting
	SlotMs        uint32 `json:"slot_ms"`
	PublishedAtMs int64  `json:"published_at_ms"`
}

// New constructs a Publisher, loading or initialising both the
// incarnation counter and the hop seed in flp_state. Incarnation is
// bumped on every successful construction; the seed is generated once
// (via crypto/rand) on the first run and reused forever.
func New(store *metrics.Store, mqttClient *mqtt.Client) (*Publisher, error) {
	p := &Publisher{store: store, mqtt: mqttClient}

	if err := p.loadAndBumpIncarnation(); err != nil {
		return nil, fmt.Errorf("incarnation: %w", err)
	}
	if err := p.loadOrGenerateSeed(); err != nil {
		return nil, fmt.Errorf("hop seed: %w", err)
	}

	hash := sha256.Sum256(p.seed[:])
	log.Printf("[epoch] incarnation=%d seed_sha256=%s slot_ms=%d",
		p.incarnation, hex.EncodeToString(hash[:8]), SlotMs)
	return p, nil
}

func (p *Publisher) loadAndBumpIncarnation() error {
	v, err := p.store.GetState(stateKeyIncarnation)
	if err != nil {
		return err
	}
	var prev uint32
	if len(v) == 4 {
		prev = binary.LittleEndian.Uint32(v)
	}
	p.incarnation = prev + 1
	out := make([]byte, 4)
	binary.LittleEndian.PutUint32(out, p.incarnation)
	return p.store.SetState(stateKeyIncarnation, out)
}

func (p *Publisher) loadOrGenerateSeed() error {
	v, err := p.store.GetState(stateKeyHopSeed)
	if err != nil {
		return err
	}
	if len(v) == hopSeedSize {
		copy(p.seed[:], v)
		p.seedHex = hex.EncodeToString(p.seed[:])
		return nil
	}
	// First run (or corrupt) — generate a fresh seed and persist it.
	if _, err := rand.Read(p.seed[:]); err != nil {
		return fmt.Errorf("rand.Read: %w", err)
	}
	p.seedHex = hex.EncodeToString(p.seed[:])
	if err := p.store.SetState(stateKeyHopSeed, p.seed[:]); err != nil {
		return fmt.Errorf("persist seed: %w", err)
	}
	log.Printf("[epoch] generated new hop seed (32 bytes) on first run")
	return nil
}

// currentEpoch derives the slot counter from wall clock. cloud-admin
// reboots do not reset this — the incarnation field handles fencing —
// so devices that had been running on inertia keep their slot alignment
// when cloud comes back.
func currentEpoch(nowMs int64) uint64 {
	return uint64(nowMs) / uint64(SlotMs)
}

// Snapshot returns the most recently published anchor for HTTP debug.
func (p *Publisher) Snapshot() Snapshot {
	p.mu.RLock()
	defer p.mu.RUnlock()
	return p.last
}

// Run is the publish loop. It re-publishes the retained anchor every
// PublishIntervalMs until ctx is cancelled. Run blocks; call as `go p.Run(ctx)`.
func (p *Publisher) Run(ctx context.Context) {
	hash := sha256.Sum256(p.seed[:])
	seedHashHex := hex.EncodeToString(hash[:])

	publish := func() {
		nowMs := time.Now().UnixMilli()
		msg := mqtt.EpochMsg{
			Epoch:         currentEpoch(nowMs),
			Incarnation:   p.incarnation,
			SeedHex:       p.seedHex,
			SlotMs:        SlotMs,
			WifiChannels:  WifiChannels,
			LoraFreqsHz:   LoraFreqsHz,
			PublishedAtMs: nowMs,
		}
		p.mqtt.PublishEpoch(msg)
		p.mu.Lock()
		p.last = Snapshot{
			Epoch:         msg.Epoch,
			Incarnation:   msg.Incarnation,
			SeedSHA256:    seedHashHex,
			SlotMs:        msg.SlotMs,
			PublishedAtMs: msg.PublishedAtMs,
		}
		p.mu.Unlock()
	}

	publish() // immediate first publish so subscribers do not wait a full tick
	ticker := time.NewTicker(time.Duration(PublishIntervalMs) * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			publish()
		}
	}
}
