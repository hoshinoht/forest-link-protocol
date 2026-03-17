package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// ---------------------------------------------------------------------------
// Channel message types
// ---------------------------------------------------------------------------

type FileMeta struct {
	NodeID       string      `json:"-"`
	SessionID    json.Number `json:"session_id"`
	Filename     string      `json:"filename"`
	TotalSize    int         `json:"total_size"`
	ChunkCount   int         `json:"chunk_count"`
	CRC32        uint32      `json:"crc32"`
	FragmentSize int         `json:"fragment_size"`
}

type FileChunk struct {
	NodeID string
	SeqNum uint16
	Data   []byte
}

type TopoMsg struct {
	NodeID  string
	Payload []byte
}

type MetricKind int

const (
	MetricKindNode MetricKind = iota
	MetricKindHeap
)

type MetricMsg struct {
	NodeID  string
	Kind    MetricKind
	Payload []byte
}

// ---------------------------------------------------------------------------
// MQTTClient
// ---------------------------------------------------------------------------

type MQTTClient struct {
	broker   string
	port     int
	client   mqtt.Client
	metaCh   chan FileMeta
	chunkCh  chan FileChunk
	topoCh   chan TopoMsg
	metricCh chan MetricMsg
}

func NewMQTTClient(broker string, port int, metaCh chan FileMeta, chunkCh chan FileChunk, topoCh chan TopoMsg, metricCh chan MetricMsg) *MQTTClient {
	return &MQTTClient{
		broker:   broker,
		port:     port,
		metaCh:   metaCh,
		chunkCh:  chunkCh,
		topoCh:   topoCh,
		metricCh: metricCh,
	}
}

func (m *MQTTClient) Connect() error {
	opts := mqtt.NewClientOptions()
	opts.AddBroker(fmt.Sprintf("tcp://%s:%d", m.broker, m.port))
	opts.SetClientID("flp-admin")
	opts.SetProtocolVersion(4) // MQTTv3.1.1
	opts.SetKeepAlive(60)
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetConnectRetryInterval(2 * time.Second)
	opts.SetConnectionLostHandler(func(_ mqtt.Client, err error) {
		log.Printf("[mqtt] connection lost: %v (will auto-reconnect)", err)
	})
	opts.SetDefaultPublishHandler(m.onMessage)
	opts.SetOnConnectHandler(func(c mqtt.Client) {
		// Re-subscribe on every (re)connect so subscriptions survive reconnects
		subs := map[string]byte{
			"flp/+/file/meta": 1,
			"flp/+/file/data": 1,
			"flp/+/status":    0,
			"flp/+/topology":  0,
			"flp/+/metrics":   0,
			"flp/+/heap":      0,
		}
		tok := c.SubscribeMultiple(subs, nil)
		tok.Wait()
		if tok.Error() != nil {
			log.Printf("[mqtt] subscribe error: %v", tok.Error())
		} else {
			log.Println("[mqtt] subscribed to topics")
		}
	})

	m.client = mqtt.NewClient(opts)
	tok := m.client.Connect()
	tok.Wait()
	if tok.Error() != nil {
		return fmt.Errorf("mqtt connect: %w", tok.Error())
	}
	log.Printf("[mqtt] connected to %s:%d", m.broker, m.port)
	return nil
}

func (m *MQTTClient) Disconnect() {
	if m.client != nil && m.client.IsConnected() {
		m.client.Disconnect(250)
		log.Println("[mqtt] disconnected")
	}
}

func (m *MQTTClient) onMessage(_ mqtt.Client, msg mqtt.Message) {
	parts := strings.Split(msg.Topic(), "/")
	// Expect at least: flp / <nodeID> / <kind> [/ <sub>]
	if len(parts) < 3 {
		log.Printf("[mqtt] unexpected topic format: %s", msg.Topic())
		return
	}

	nodeID := parts[1]
	kind := parts[2]

	switch kind {
	case "file":
		if len(parts) < 4 {
			log.Printf("[mqtt] incomplete file topic: %s", msg.Topic())
			return
		}
		sub := parts[3]
		switch sub {
		case "meta":
			var fm FileMeta
			if err := json.Unmarshal(msg.Payload(), &fm); err != nil {
				log.Printf("[mqtt] failed to parse file meta from %s: %v", nodeID, err)
				return
			}
			fm.NodeID = nodeID
			select {
			case m.metaCh <- fm:
			default:
				log.Printf("[mqtt] metaCh full, dropping file meta from %s", nodeID)
			}

		case "data":
			payload := msg.Payload()
			if len(payload) < 2 {
				log.Printf("[mqtt] file data too short from %s", nodeID)
				return
			}
			seq := binary.LittleEndian.Uint16(payload[:2])
			data := make([]byte, len(payload)-2)
			copy(data, payload[2:])
			select {
			case m.chunkCh <- FileChunk{NodeID: nodeID, SeqNum: seq, Data: data}:
			default:
				log.Printf("[mqtt] chunkCh full, dropping chunk seq=%d from %s", seq, nodeID)
			}
		}

	case "status":
		log.Printf("[mqtt] status from %s: %s", nodeID, string(msg.Payload()))

	case "topology":
		payload := make([]byte, len(msg.Payload()))
		copy(payload, msg.Payload())
		select {
		case m.topoCh <- TopoMsg{NodeID: nodeID, Payload: payload}:
		default:
			log.Printf("[mqtt] topoCh full, dropping topology from %s", nodeID)
		}

	case "metrics":
		payload := make([]byte, len(msg.Payload()))
		copy(payload, msg.Payload())
		select {
		case m.metricCh <- MetricMsg{NodeID: nodeID, Kind: MetricKindNode, Payload: payload}:
		default:
			log.Printf("[mqtt] metricCh full, dropping metrics from %s", nodeID)
		}

	case "heap":
		payload := make([]byte, len(msg.Payload()))
		copy(payload, msg.Payload())
		select {
		case m.metricCh <- MetricMsg{NodeID: nodeID, Kind: MetricKindHeap, Payload: payload}:
		default:
			log.Printf("[mqtt] metricCh full, dropping heap from %s", nodeID)
		}

	default:
		log.Printf("[mqtt] unhandled topic: %s", msg.Topic())
	}
}

// PublishACK publishes an acknowledgement message to flp/admin/ack.
func (m *MQTTClient) PublishACK(msgType string, seq int) {
	payload, _ := json.Marshal(map[string]interface{}{
		"type": msgType,
		"seq":  seq,
	})
	tok := m.client.Publish("flp/admin/ack", 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish ack error: %v", tok.Error())
	}
}

// PublishTransferCmd publishes a transfer command to flp/admin/transfer_cmd.
func (m *MQTTClient) PublishTransferCmd(nodeID, command, sessionID string) {
	payload, _ := json.Marshal(map[string]string{
		"node_id":    nodeID,
		"command":    command,
		"session_id": sessionID,
	})
	tok := m.client.Publish("flp/admin/transfer_cmd", 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish transfer_cmd error: %v", tok.Error())
	}
}

// PublishCmd publishes raw bytes to flp/admin/cmd.
func (m *MQTTClient) PublishCmd(payload []byte) {
	tok := m.client.Publish("flp/admin/cmd", 1, false, payload)
	tok.Wait()
	if tok.Error() != nil {
		log.Printf("[mqtt] publish cmd error: %v", tok.Error())
	}
}
