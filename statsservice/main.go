// Command statsservice sits between an RTP sender and receiver: it
// listens on the sender's target UDP port, parses each RTP packet's
// header (RFC 3550) to track sequence-number gaps (packet loss) and
// inter-arrival jitter, relays the packet unmodified to the receiver's
// port, and exposes the running stats over a REST endpoint.
package main

import (
	"encoding/json"
	"flag"
	"log"
	"net"
	"net/http"
	"sync"
	"time"
)

const rtpClockRate = 90000 // standard clock rate for video payloads (RFC 3551)

type stats struct {
	mu sync.Mutex

	ssrc            uint32
	haveFirst       bool
	firstSeq        uint16
	highestSeq      uint16
	cycles          uint32 // how many times the 16-bit sequence number has wrapped
	packetsReceived uint64
	bytesReceived   uint64

	jitter          float64 // RFC 3550 6.4.1 running estimate, in RTP timestamp units
	prevTransit     int64
	havePrevTransit bool

	firstPacketAt time.Time
	lastPacketAt  time.Time
}

type statsSnapshot struct {
	SSRC            uint32  `json:"ssrc"`
	PacketsReceived uint64  `json:"packets_received"`
	PacketsExpected uint64  `json:"packets_expected"`
	PacketsLost     int64   `json:"packets_lost"`
	LossPercent     float64 `json:"loss_percent"`
	JitterMs        float64 `json:"jitter_ms"`
	BitrateKbps     float64 `json:"bitrate_kbps"`
	LastPacketAgoMs float64 `json:"last_packet_ago_ms"`
}

func (s *stats) onPacket(payload []byte, arrival time.Time) {
	if len(payload) < 12 {
		return // too short to be a valid RTP header
	}
	seq := uint16(payload[2])<<8 | uint16(payload[3])
	ts := uint32(payload[4])<<24 | uint32(payload[5])<<16 | uint32(payload[6])<<8 | uint32(payload[7])
	ssrc := uint32(payload[8])<<24 | uint32(payload[9])<<16 | uint32(payload[10])<<8 | uint32(payload[11])

	s.mu.Lock()
	defer s.mu.Unlock()

	s.ssrc = ssrc
	s.packetsReceived++
	s.bytesReceived += uint64(len(payload))
	s.lastPacketAt = arrival

	if !s.haveFirst {
		s.haveFirst = true
		s.firstSeq = seq
		s.highestSeq = seq
		s.firstPacketAt = arrival
	} else if seq < s.highestSeq && s.highestSeq-seq > 0x8000 {
		// Sequence number wrapped around 65535 -> 0.
		s.cycles++
		s.highestSeq = seq
	} else if seq > s.highestSeq {
		s.highestSeq = seq
	}

	// RFC 3550 A.8: jitter is the mean deviation of the difference in
	// packet spacing between sender and receiver, measured in the same
	// units as the RTP timestamp.
	arrivalRTPUnits := int64(arrival.UnixNano()) / (int64(time.Second) / rtpClockRate)
	transit := arrivalRTPUnits - int64(ts)
	if s.havePrevTransit {
		d := transit - s.prevTransit
		if d < 0 {
			d = -d
		}
		s.jitter += (float64(d) - s.jitter) / 16.0
	}
	s.prevTransit = transit
	s.havePrevTransit = true
}

func (s *stats) snapshot() statsSnapshot {
	s.mu.Lock()
	defer s.mu.Unlock()

	expected := uint64(0)
	if s.haveFirst {
		extHighest := uint64(s.cycles)<<16 | uint64(s.highestSeq)
		extFirst := uint64(s.firstSeq)
		expected = extHighest - extFirst + 1
	}
	lost := int64(expected) - int64(s.packetsReceived)
	lossPct := 0.0
	if expected > 0 {
		lossPct = 100 * float64(lost) / float64(expected)
	}

	elapsed := time.Since(s.lastPacketAt)

	// Average bitrate over the life of the stream so far.
	bitrate := 0.0
	if streamAge := s.lastPacketAt.Sub(s.firstPacketAt).Seconds(); streamAge > 0 {
		bitrate = (float64(s.bytesReceived) * 8 / 1000) / streamAge
	}

	return statsSnapshot{
		SSRC:            s.ssrc,
		PacketsReceived: s.packetsReceived,
		PacketsExpected: expected,
		PacketsLost:     lost,
		LossPercent:     lossPct,
		JitterMs:        s.jitter / (rtpClockRate / 1000.0),
		BitrateKbps:     bitrate,
		LastPacketAgoMs: float64(elapsed.Microseconds()) / 1000.0,
	}
}

func main() {
	listenAddr := flag.String("listen-rtp", ":6000", "UDP address to receive the RTP stream on")
	relayAddr := flag.String("relay-to", "127.0.0.1:6002", "UDP address to relay packets to (the decoding receiver)")
	httpAddr := flag.String("http", ":8090", "HTTP listen address for the /stats endpoint")
	flag.Parse()

	udpAddr, err := net.ResolveUDPAddr("udp", *listenAddr)
	if err != nil {
		log.Fatal(err)
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		log.Fatal(err)
	}

	relayUDPAddr, err := net.ResolveUDPAddr("udp", *relayAddr)
	if err != nil {
		log.Fatal(err)
	}
	relayConn, err := net.DialUDP("udp", nil, relayUDPAddr)
	if err != nil {
		log.Fatal(err)
	}

	s := &stats{}

	go func() {
		buf := make([]byte, 65535)
		for {
			n, _, err := conn.ReadFromUDP(buf)
			if err != nil {
				log.Printf("read error: %v", err)
				continue
			}
			arrival := time.Now()
			s.onPacket(buf[:n], arrival)
			if _, err := relayConn.Write(buf[:n]); err != nil {
				log.Printf("relay error: %v", err)
			}
		}
	}()

	http.HandleFunc("/stats", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(s.snapshot())
	})
	http.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})

	log.Printf("statsservice: relaying %s -> %s, REST stats on %s/stats", *listenAddr, *relayAddr, *httpAddr)
	log.Fatal(http.ListenAndServe(*httpAddr, nil))
}
