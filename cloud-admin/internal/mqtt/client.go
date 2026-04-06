package mqtt

import (
	"container/list"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log"
	"strconv"
	"strings"
	"sync"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"
)

// sessionSourceCap bounds how many in-flight (exit|sessionID → source) mappings
// we retain. Chosen generously — a few hundred concurrent transfers is well
// beyond anything the mesh will realistically produce, and each entry is tiny.
const sessionSourceCap = 1024

// sessionSourceLRU is a bounded LRU mapping exit|sessionID keys to source
// node IDs. Previously this was an unbounded map, which leaked one entry per
// completed transfer until process restart. True LRU (move-to-front on Get)
// means in-flight sessions stay hot while completed ones age out the back.
type sessionSourceLRU struct {
	cap   int
	ll    *list.List               // front = most recently used
	items map[string]*list.Element // key -> element in ll
}

type sessionSourceEntry struct {
	key string
	val string
}

func newSessionSourceLRU(capacity int) *sessionSourceLRU {
	return &sessionSourceLRU{
		cap:   capacity,
		ll:    list.New(),
		items: make(map[string]*list.Element, capacity),
	}
}

func (l *sessionSourceLRU) Put(key, val string) {
	if el, ok := l.items[key]; ok {
		el.Value.(*sessionSourceEntry).val = val
		l.ll.MoveToFront(el)
		return
	}
	el := l.ll.PushFront(&sessionSourceEntry{key: key, val: val})
	l.items[key] = el
	if l.ll.Len() > l.cap {
		if back := l.ll.Back(); back != nil {
			ent := back.Value.(*sessionSourceEntry)
			delete(l.items, ent.key)
			l.ll.Remove(back)
		}
	}
}

// Get returns the value and promotes the entry to most-recently-used.
// Mutates the list, so callers must hold a write lock, not a read lock.
func (l *sessionSourceLRU) Get(key string) (string, bool) {
	el, ok := l.items[key]
	if !ok {
		return "", false
	}
	l.ll.MoveToFront(el)
	return el.Value.(*sessionSourceEntry).val, true
}

// rawMsg holds a copied Paho payload for out-of-callback dispatch.
// Paho's default handler runs on a single serialized goroutine per
// connection, so any work we do inside it stalls the entire broker read
// path. We copy the topic+payload and hand it off to a worker immediately.
type rawMsg struct {
	topic   string
	payload []byte
}

// Client wraps paho MQTT and demuxes incoming messages into typed channels.
type Client struct {
	broker   string
	port     int
	username string
	password string
	client   paho.Client
	metaCh   chan<- FileMeta
	chunkCh  chan<- FileChunk
	topoCh   chan<- TopoMsg
	metricCh chan<- MetricMsg

	// rawCh buffers messages between the Paho callback goroutine and the
	// dispatch worker. Sized large so a burst from the WSS tunnel does not
	// immediately force drops on the broker read path.
	rawCh    chan rawMsg
	dispatchDone chan struct{}

	// sourceMu guards sessionSourceByExit. It is a plain Mutex (not RWMutex)
	// because LRU Get mutates the list (move-to-front), so even lookups need
	// a write lock.
	sourceMu            sync.Mutex
	sessionSourceByExit *sessionSourceLRU
}

// NewClient creates a Client that fans out received messages to the provided channels.
func NewClient(broker string, port int, username, password string,
	metaCh chan<- FileMeta, chunkCh chan<- FileChunk,
	topoCh chan<- TopoMsg, metricCh chan<- MetricMsg,
) *Client {
	return &Client{
		broker:       broker,
		port:         port,
		username:     username,
		password:     password,
		metaCh:       metaCh,
		chunkCh:      chunkCh,
		topoCh:       topoCh,
		metricCh:     metricCh,
		rawCh:        make(chan rawMsg, 2048),
		dispatchDone: make(chan struct{}),
		sessionSourceByExit: newSessionSourceLRU(sessionSourceCap),
	}
}

func normalizeNodeID(node string) string {
	n := strings.TrimSpace(strings.ToLower(node))
	n = strings.TrimPrefix(n, "0x")
	if n == "" {
		return ""
	}
	v, err := strconv.ParseUint(n, 16, 16)
	if err != nil {
		return n
	}
	return fmt.Sprintf("%04x", uint16(v))
}

func makeExitSessionKey(exitNode string, sessionID uint16) string {
	return fmt.Sprintf("%s|%d", exitNode, sessionID)
}

// Connect establishes the MQTT connection and subscribes to all FLP topics.
func (c *Client) Connect() error {
	opts := paho.NewClientOptions()
	opts.AddBroker(fmt.Sprintf("tcp://%s:%d", c.broker, c.port))
	opts.SetClientID("flp-admin")
	opts.SetProtocolVersion(4) // MQTTv3.1.1
	opts.SetKeepAlive(60)
	if c.username != "" {
		opts.SetUsername(c.username)
		opts.SetPassword(c.password)
	}
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetConnectRetryInterval(2 * time.Second)
	opts.SetConnectionLostHandler(func(_ paho.Client, err error) {
		log.Printf("[mqtt] connection lost: %v (will auto-reconnect)", err)
	})
	opts.SetDefaultPublishHandler(c.onMessage)
	opts.SetOnConnectHandler(func(cl paho.Client) {
		subs := map[string]byte{
			"flp/+/file/meta": 1,
			"flp/+/file/data": 1,
			"flp/+/status":    0,
			"flp/+/topology":  0,
			"flp/+/metrics":   0,
			"flp/+/heap":      0,
		}
		tok := cl.SubscribeMultiple(subs, nil)
		tok.Wait()
		if tok.Error() != nil {
			log.Printf("[mqtt] subscribe error: %v", tok.Error())
		} else {
			log.Println("[mqtt] subscribed to topics")
		}
	})

	c.client = paho.NewClient(opts)
	tok := c.client.Connect()
	tok.Wait()
	if tok.Error() != nil {
		return fmt.Errorf("mqtt connect: %w", tok.Error())
	}
	log.Printf("[mqtt] connected to %s:%d", c.broker, c.port)

	// Start dispatch worker. Paho's callback thread only needs to copy the
	// payload into rawCh; all JSON/binary decoding happens here.
	go c.dispatchLoop()
	return nil
}

// dispatchLoop drains rawCh and does the actual topic-based demux.
// Runs on its own goroutine so the Paho callback thread is never blocked
// by JSON unmarshaling or a full downstream channel.
func (c *Client) dispatchLoop() {
	defer close(c.dispatchDone)
	for rm := range c.rawCh {
		c.handleMessage(rm.topic, rm.payload)
	}
}

// Disconnect cleanly shuts down the MQTT connection.
func (c *Client) Disconnect() {
	if c.client != nil && c.client.IsConnected() {
		c.client.Disconnect(250)
		log.Println("[mqtt] disconnected")
	}
	// Closing rawCh terminates dispatchLoop. Paho has already drained its
	// callbacks by the time Disconnect(250) returns, so no writer remains.
	close(c.rawCh)
	<-c.dispatchDone
}

// onMessage runs on Paho's serialized callback goroutine. It MUST return
// quickly — we just copy the payload and hand it to the dispatch worker.
func (c *Client) onMessage(_ paho.Client, msg paho.Message) {
	// Paho may reuse the payload slice after this callback returns.
	payload := make([]byte, len(msg.Payload()))
	copy(payload, msg.Payload())
	rm := rawMsg{topic: msg.Topic(), payload: payload}
	select {
	case c.rawCh <- rm:
	default:
		log.Printf("[mqtt] rawCh full, dropping %s (dispatch worker behind)", msg.Topic())
	}
}

// handleMessage runs on the dispatch worker goroutine.
func (c *Client) handleMessage(topic string, payload []byte) {
	parts := strings.Split(topic, "/")
	if len(parts) < 3 {
		log.Printf("[mqtt] unexpected topic format: %s", topic)
		return
	}

	nodeID := parts[1]
	kind := parts[2]

	switch kind {
	case "file":
		if len(parts) < 4 {
			log.Printf("[mqtt] incomplete file topic: %s", topic)
			return
		}
		switch parts[3] {
		case "meta":
			var fm FileMeta
			if err := json.Unmarshal(payload, &fm); err != nil {
				log.Printf("[mqtt] failed to parse file meta from %s: %v", nodeID, err)
				return
			}
			sourceNode := normalizeNodeID(fm.SourceNode)
			if sourceNode == "" {
				sourceNode = nodeID
			}
			fm.NodeID = sourceNode
			if sid64, err := fm.SessionID.Int64(); err == nil {
				sid := uint16(sid64)
				key := makeExitSessionKey(nodeID, sid)
				c.sourceMu.Lock()
				c.sessionSourceByExit.Put(key, sourceNode)
				c.sourceMu.Unlock()
			}
			select {
			case c.metaCh <- fm:
			default:
				log.Printf("[mqtt] metaCh full, dropping file meta from %s", nodeID)
			}

		case "data":
			// B4 fix: wire format is now [session_id:2LE][seq:2LE][data]
			if len(payload) < 4 {
				log.Printf("[mqtt] file data too short from %s (%d bytes)", nodeID, len(payload))
				return
			}
			sessionID := binary.LittleEndian.Uint16(payload[:2])
			seq := binary.LittleEndian.Uint16(payload[2:4])
			// payload is already a private copy — we own it, so slice
			// directly instead of allocating a second buffer.
			data := payload[4:]
			canonicalNode := nodeID
			key := makeExitSessionKey(nodeID, sessionID)
			// LRU Get mutates (move-to-front), so full Lock, not RLock.
			c.sourceMu.Lock()
			if src, ok := c.sessionSourceByExit.Get(key); ok && src != "" {
				canonicalNode = src
			}
			c.sourceMu.Unlock()
			select {
			case c.chunkCh <- FileChunk{NodeID: canonicalNode, SessionID: sessionID, SeqNum: seq, Data: data}:
			default:
				log.Printf("[mqtt] chunkCh full, dropping chunk seq=%d from %s", seq, nodeID)
			}
		}

	case "status":
		log.Printf("[mqtt] status from %s: %s", nodeID, string(payload))

	case "topology":
		select {
		case c.topoCh <- TopoMsg{NodeID: nodeID, Payload: payload}:
		default:
			log.Printf("[mqtt] topoCh full, dropping topology from %s", nodeID)
		}

	case "metrics":
		select {
		case c.metricCh <- MetricMsg{NodeID: nodeID, Kind: MetricKindNode, Payload: payload}:
		default:
			log.Printf("[mqtt] metricCh full, dropping metrics from %s", nodeID)
		}

	case "heap":
		select {
		case c.metricCh <- MetricMsg{NodeID: nodeID, Kind: MetricKindHeap, Payload: payload}:
		default:
			log.Printf("[mqtt] metricCh full, dropping heap from %s", nodeID)
		}

	default:
		log.Printf("[mqtt] unhandled topic: %s", topic)
	}
}

// PublishACK publishes an acknowledgement message to flp/admin/ack/<sessionID>.
// D2 fix: session-scoped topic prevents ambiguity in multi-transfer scenarios.
func (c *Client) PublishACK(sessionID string, msgType string, seq int) {
	payload, _ := json.Marshal(map[string]interface{}{
		"type": msgType,
		"seq":  seq,
	})
	topic := fmt.Sprintf("flp/admin/ack/%s", sessionID)
	tok := c.client.Publish(topic, 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish ack error: %v", tok.Error())
	}
}

// PublishTransferNACK publishes missing seq numbers so exit nodes can re-request
// retransmission from the source. D1+B3 fix: bridges the end-to-end gap.
func (c *Client) PublishTransferNACK(sessionID string, seqs []int) {
	payload, _ := json.Marshal(map[string]interface{}{
		"session_id": sessionID,
		"seqs":       seqs,
	})
	topic := fmt.Sprintf("flp/admin/transfer_nack/%s", sessionID)
	tok := c.client.Publish(topic, 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish transfer_nack error: %v", tok.Error())
	}
}

// PublishTransferComplete publishes a successful end-to-end completion signal
// so the exit node can send TRANSFER_DONE to the source and clear session state.
func (c *Client) PublishTransferComplete(sessionID string) {
	topic := fmt.Sprintf("flp/admin/complete/%s", sessionID)
	tok := c.client.Publish(topic, 1, false, []byte(`{"status":"complete"}`))
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish transfer complete error: %v", tok.Error())
	}
}

// PublishTransferCmd publishes a transfer command to flp/admin/transfer_cmd.
func (c *Client) PublishTransferCmd(nodeID, command, sessionID string) {
	payload, _ := json.Marshal(map[string]string{
		"node_id":    nodeID,
		"command":    command,
		"session_id": sessionID,
	})
	tok := c.client.Publish("flp/admin/transfer_cmd", 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish transfer_cmd error: %v", tok.Error())
	}
}

// PublishCmd publishes raw bytes to flp/admin/cmd.
func (c *Client) PublishCmd(payload []byte) {
	tok := c.client.Publish("flp/admin/cmd", 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish cmd error: %v", tok.Error())
	}
}
