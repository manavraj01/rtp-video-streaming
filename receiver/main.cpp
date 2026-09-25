// RTP receiver: opens an SDP file describing an incoming RTP stream,
// decodes it, and writes decoded frames out as raw planar YUV (in place
// of an on-screen player — swap write_frame_planes for an SDL/ffplay
// sink to actually display it; see README). Reports per-frame arrival
// gaps as a coarse client-side latency signal alongside the Go stats
// service's RTP-header-level jitter/loss numbers.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>

namespace {

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void die_on_error(int err, const char* what) {
    if (err < 0) {
        char buf[256];
        av_strerror(err, buf, sizeof(buf));
        std::fprintf(stderr, "%s: %s\n", what, buf);
        std::exit(1);
    }
}

void write_frame_planes(std::FILE* out, AVFrame* frame) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    for (int plane = 0; plane < desc->nb_components && frame->data[plane] && frame->linesize[plane]; ++plane) {
        int plane_height = frame->height;
        int plane_width_bytes = frame->width;
        if (plane == 1 || plane == 2) {
            plane_height = (frame->height + 1) / 2;
            plane_width_bytes = (frame->width + 1) / 2;
        }
        for (int row = 0; row < plane_height; ++row) {
            std::fwrite(frame->data[plane] + static_cast<std::size_t>(row) * frame->linesize[plane],
                        1, plane_width_bytes, out);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rtp_receiver <sdp_path> [output.yuv] [max_frames]\n");
        return 1;
    }
    const std::string sdp_path = argv[1];
    const std::string output_path = argc > 2 ? argv[2] : "received.yuv";
    const int max_frames = argc > 3 ? std::atoi(argv[3]) : 0;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,udp,rtp", 0);
    // Bump the UDP socket receive buffer well past the OS default so a
    // brief decode stall (e.g. on a keyframe) can't overflow the kernel
    // buffer and silently drop packets before we ever read them.
    av_dict_set(&opts, "buffer_size", "1000000", 0);

    AVFormatContext* fmt_ctx = nullptr;
    die_on_error(avformat_open_input(&fmt_ctx, sdp_path.c_str(), nullptr, &opts),
                 "avformat_open_input (sdp)");
    die_on_error(avformat_find_stream_info(fmt_ctx, nullptr), "avformat_find_stream_info");

    int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx < 0) {
        std::fprintf(stderr, "no video stream described in SDP\n");
        return 1;
    }
    AVStream* stream = fmt_ctx->streams[video_idx];
    const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, stream->codecpar);
    die_on_error(avcodec_open2(dec_ctx, dec, nullptr), "avcodec_open2");

    std::FILE* out = std::fopen(output_path.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "failed to open output file %s\n", output_path.c_str());
        return 1;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    int64_t frame_count = 0;
    int64_t start_ns = now_ns();
    int64_t last_arrival_ns = start_ns;
    double max_gap_ms = 0.0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx && avcodec_send_packet(dec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                int64_t arrival_ns = now_ns();
                double gap_ms = (arrival_ns - last_arrival_ns) / 1e6;
                if (frame_count > 0 && gap_ms > max_gap_ms) max_gap_ms = gap_ms;
                last_arrival_ns = arrival_ns;

                write_frame_planes(out, frame);
                ++frame_count;
                if (max_frames && frame_count >= max_frames) break;
            }
        }
        av_packet_unref(pkt);
        if (max_frames && frame_count >= max_frames) break;
    }

    double elapsed_s = (now_ns() - start_ns) / 1e9;
    std::printf("== rtp_receiver report ==\n");
    std::printf("frames decoded:      %lld\n", static_cast<long long>(frame_count));
    std::printf("elapsed:             %.2f s\n", elapsed_s);
    std::printf("avg decode FPS:      %.2f\n", elapsed_s > 0 ? frame_count / elapsed_s : 0.0);
    std::printf("max inter-frame gap: %.2f ms\n", max_gap_ms);
    std::printf("output written to:   %s\n", output_path.c_str());

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    std::fclose(out);
    return 0;
}
