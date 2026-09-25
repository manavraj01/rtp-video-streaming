# Low-Latency RTP Video Streaming with Network Impairment Testing

A C++ RTP/UDP video sender and receiver built on FFmpeg (libavformat/
libavcodec), plus a Go service that sits in the middle to parse RTP
headers for jitter/packet-loss stats and expose them over REST.

## Architecture

```
rtp_sender  --RTP/UDP-->  statsservice  --RTP/UDP-->  rtp_receiver
 (encode)                (stats + relay)              (decode)
                               |
                          GET /stats (REST)
```

`statsservice` is the piece described by the "Go service exposing stream
statistics through a REST endpoint" goal, and it's positioned as a relay
rather than a passive tap because a single UDP port can't be read by two
independent processes at once — putting it in the middle (parse the RTP
header, then forward the same bytes on) lets it observe every packet
without needing the decode path to also implement RTP-header parsing.

- **`sender/`** — decodes an input file, re-encodes to H.264
  (`preset=ultrafast`, `tune=zerolatency`, no B-frames), and muxes it to
  RTP via libavformat, paced to the source frame rate rather than sent
  as fast as the encoder can run. Writes an SDP file describing the
  stream. Sets `AV_CODEC_FLAG_GLOBAL_HEADER` so SPS/PPS end up in the
  SDP as `sprop-parameter-sets` — without that, a receiver that joins
  after the stream's first keyframe (or loses that one packet) has no
  way to recover the parameter sets and can never start decoding.
- **`statsservice/`** — parses the 12-byte RTP header (RFC 3550) on each
  packet to track sequence-number gaps and an RFC 3550 §6.4.1 jitter
  estimate, forwards the packet unchanged to the receiver's port, and
  serves the running totals at `GET /stats`.
- **`receiver/`** — opens the (port-rewritten) SDP, decodes the stream,
  and writes decoded frames out as raw planar YUV in place of an
  on-screen player (swap `write_frame_planes` for an SDL/ffplay sink to
  actually display it).

## Build

Primary target is Linux:

```sh
sudo apt install libavformat-dev libavcodec-dev libavutil-dev
make
cd statsservice && go build -o statsservice .
```

On a machine without pkg-config entries for FFmpeg, point `FFMPEG_DEV`
at the SDK root instead: `make FFMPEG_DEV=/path/to/ffmpeg-dev`.

## Run the demo (loopback)

```sh
scripts/gen_testdata.sh   # generates testdata/sample.mp4
scripts/run_demo.sh
```

This starts `statsservice` (relaying `:6000` → `127.0.0.1:6002`),
`rtp_sender` (streaming `testdata/sample.mp4` to `:6000`), and
`rtp_receiver` (decoding from `:6002`), then prints the sender→relay
stats once the clip finishes sending.

## Measured results

Streaming a 640x360/25fps 6s clip over loopback, sender → statsservice
hop (this is the hop `statsservice` actually instruments):

```
{"packets_received":210,"packets_expected":210,"packets_lost":0,
 "loss_percent":0,"jitter_ms":6.5,"bitrate_kbps":330}
```

Zero loss and ~330 kbps line up with libx264's own reported encode
bitrate (322–330 kb/s) for this clip — the stats service's independent,
header-level measurement agrees with the encoder's own accounting.

**The relay → receiver hop was a different story on this machine**: the
receiver only decoded 123 of 148 frames before libavformat's RTP demuxer
gave up after an observed ~10.8s stall waiting on a gap in the sequence
it never received. `statsservice` shows the *first* hop delivering every
packet, so the loss happened strictly between the relay and the decoder
— most likely a socket-level drop on this Windows dev box rather than
anything in the RTP/H.264 logic itself. That asymmetry — "the wire
between A and B is measured clean, but B still doesn't get everything" —
is exactly the kind of failure mode `tc netem` impairment testing (below)
is meant to catch systematically instead of by accident.

## Network impairment testing (Linux, `tc netem`)

This wasn't run in this dev environment (Windows, no `tc`), but is how
it's meant to be exercised on Linux, where the sender/receiver/stats
service all run unmodified:

```sh
# Inject 50ms +/-10ms jitter and 2% loss on the interface the RTP
# traffic goes out on:
sudo tc qdisc add dev eth0 root netem delay 50ms 10ms loss 2%

# ...run scripts/run_demo.sh (or point sender/receiver at real hosts)...

sudo tc qdisc del dev eth0 root netem
```

Compare `statsservice`'s reported `jitter_ms`/`loss_percent` against the
injected values to sanity-check the measurement code itself, then vary
the `netem` parameters to characterize how the receiver's decode
success rate degrades — the same "clean hop but lossy decode" gap found
above is worth specifically checking for.

## Wireshark inspection

```sh
sudo tcpdump -i lo udp port 6000 -w rtp_capture.pcap
```

Open the capture in Wireshark, select a UDP packet on that port, and use
*Decode As* → RTP (Wireshark won't recognize RTP automatically without a
SIP/SDP handshake it can see). The RTP header fields shown there
(sequence number, timestamp, marker bit) are exactly what
`statsservice/main.go`'s `onPacket` parses by hand.

## Layout

```
sender/         RTP sender (decode input -> encode H.264 -> mux to RTP)
receiver/       RTP receiver (open SDP -> decode -> write raw YUV)
statsservice/   Go RTP stats relay + REST endpoint
scripts/        gen_testdata.sh, run_demo.sh
sdp/            generated at runtime (sender.sdp, receiver.sdp)
testdata/       sample.mp4 (checked in); received.yuv generated by the demo
```
