package mqtt

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"
)

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
}

// NewClient creates a Client that fans out received messages to the provided channels.
func NewClient(broker string, port int, username, password string,
	metaCh chan<- FileMeta, chunkCh chan<- FileChunk,
	topoCh chan<- TopoMsg, metricCh chan<- MetricMsg,
) *Client {
	return &Client{
		broker:   broker,
		port:     port,
		username: username,
		password: password,
		metaCh:   metaCh,
		chunkCh:  chunkCh,
		topoCh:   topoCh,
		metricCh: metricCh,
	}
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
	return nil
}

// Disconnect cleanly shuts down the MQTT connection.
func (c *Client) Disconnect() {
	if c.client != nil && c.client.IsConnected() {
		c.client.Disconnect(250)
		log.Println("[mqtt] disconnected")
	}
}

func (c *Client) onMessage(_ paho.Client, msg paho.Message) {
	parts := strings.Split(msg.Topic(), "/")
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
		switch parts[3] {
		case "meta":
			var fm FileMeta
			if err := json.Unmarshal(msg.Payload(), &fm); err != nil {
				log.Printf("[mqtt] failed to parse file meta from %s: %v", nodeID, err)
				return
			}
			fm.NodeID = nodeID
			select {
			case c.metaCh <- fm:
			default:
				log.Printf("[mqtt] metaCh full, dropping file meta from %s", nodeID)
			}

		case "data":
			payload := msg.Payload()
			// B4 fix: wire format is now [session_id:2LE][seq:2LE][data]
			if len(payload) < 4 {
				log.Printf("[mqtt] file data too short from %s (%d bytes)", nodeID, len(payload))
				return
			}
			sessionID := binary.LittleEndian.Uint16(payload[:2])
			seq := binary.LittleEndian.Uint16(payload[2:4])
			data := make([]byte, len(payload)-4)
			copy(data, payload[4:])
			select {
			case c.chunkCh <- FileChunk{NodeID: nodeID, SessionID: sessionID, SeqNum: seq, Data: data}:
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
		case c.topoCh <- TopoMsg{NodeID: nodeID, Payload: payload}:
		default:
			log.Printf("[mqtt] topoCh full, dropping topology from %s", nodeID)
		}

	case "metrics":
		payload := make([]byte, len(msg.Payload()))
		copy(payload, msg.Payload())
		select {
		case c.metricCh <- MetricMsg{NodeID: nodeID, Kind: MetricKindNode, Payload: payload}:
		default:
			log.Printf("[mqtt] metricCh full, dropping metrics from %s", nodeID)
		}

	case "heap":
		payload := make([]byte, len(msg.Payload()))
		copy(payload, msg.Payload())
		select {
		case c.metricCh <- MetricMsg{NodeID: nodeID, Kind: MetricKindHeap, Payload: payload}:
		default:
			log.Printf("[mqtt] metricCh full, dropping heap from %s", nodeID)
		}

	default:
		log.Printf("[mqtt] unhandled topic: %s", msg.Topic())
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
