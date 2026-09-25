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

Or build and run it in a real Linux container (this is what actually
produced the "on real Linux" findings below):

```sh
docker build -t rtp-streaming-linux .
docker run --rm -it --cap-add=NET_ADMIN --cap-add=NET_RAW rtp-streaming-linux bash
```
`NET_ADMIN`/`NET_RAW` are only needed for `tc`/`tcpdump` inside the
container — plain `docker run --rm -it rtp-streaming-linux bash` is
enough for building/running the pipeline itself.

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

**The relay → receiver hop was a different story, and it reproduces
identically on real Linux** (in the Docker container, not just Windows):
the receiver only decoded 123 of 148 frames before `av_read_frame`
silently stopped returning new packets, ~24s in. `statsservice` shows the
sender→relay hop delivering every packet — on Linux, jitter over that
hop measured **0.7ms** (vs. ~6.5ms on Windows/MinGW — real-time scheduling
is just more consistent under Linux), still 0% loss, still ~330 kbps.
So the loss is strictly between the relay and the decoder, on both
platforms. That ruled out "Windows quirk" as the explanation, so it got
investigated properly instead of just re-documented:

- **`tcpdump -i lo udp port 6002`** during a run captured 205 of the
  ~210 packets `statsservice` relayed (the few missing are capture
  start/stop timing, not loss) — the packets *physically arrive* at the
  receiver's socket. This rules out network- or OS-buffer-level loss
  entirely.
- **`av_log_set_level(AV_LOG_DEBUG)`** on the receiver shows completely
  normal H.264 NAL parsing (SPS/PPS/non-IDR slices) right up to the exact
  line before the process exits — no jitter-buffer warning, no
  "misordered"/"lost"/timeout message anywhere in the log. FFmpeg's own
  RTP demuxer isn't reporting a problem; it just stops.
- **Tested and ruled out**: hypothesized the stall was tied to periodic
  keyframes (GOP boundaries land right around where it stops) and tried
  removing them (`gop_size` large enough for a single keyframe at frame
  0). That made it *worse* — 0 of 148 frames decoded, because a
  late-joining receiver that misses the one keyframe's slice data has
  nothing to fall back on. Periodic keyframes are load-bearing for this
  design, not the cause.

**Current best hypothesis, still open**: something in libavformat's
generic multi-media read loop (used even for a bare `sdp` file input)
gives up — silently, with no logged reason — after a certain point,
despite the RTP layer itself raising no complaint and the data being
available at the socket. Next step would be instrumenting/tracing inside
`libavformat/rtsp.c`'s read loop directly (the generic layer a bare-SDP
open goes through), which wasn't done in this session.

## Network impairment testing (Linux, `tc netem`)

This was actually attempted on real Linux, in the Docker container above,
with `NET_ADMIN` — and hit a different, more fundamental wall:

```
$ tc qdisc add dev lo root netem delay 40ms 10ms loss 5%
Error: Specified qdisc kind is unknown.
$ modprobe sch_netem
modprobe: FATAL: Module sch_netem not found in directory /lib/modules/5.15.167.4-microsoft-standard-WSL2
$ ls /lib/modules/
ls: cannot access '/lib/modules/': No such file or directory
```

Docker Desktop's Linux VM (a trimmed WSL2 kernel) ships **no loadable
kernel modules at all** — `sch_netem` can't be loaded no matter what
capabilities the container has, because the module doesn't exist on the
host kernel. This is a real, verified constraint of Docker Desktop on
Windows specifically, distinct from the frame-loss issue above — not
something `--cap-add` or an apt package can route around. It would work
unmodified on a real Linux machine or a proper Linux VM (not Docker
Desktop's bundled one):

```sh
# Inject 50ms +/-10ms jitter and 2% loss on the interface the RTP
# traffic goes out on:
sudo tc qdisc add dev eth0 root netem delay 50ms 10ms loss 2%

# ...run scripts/run_demo.sh (or point sender/receiver at real hosts)...

sudo tc qdisc del dev eth0 root netem
```

Compare `statsservice`'s reported `jitter_ms`/`loss_percent` against the
injected values to sanity-check the measurement code itself, then vary
the `netem` parameters to characterize how the receiver's decode success
rate degrades — the open issue above is worth specifically checking
against real, controlled loss instead of the unexplained kind.

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
