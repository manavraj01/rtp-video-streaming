// RTP sender: decodes an input video file, re-encodes it to H.264, and
// streams it over RTP/UDP in real time (paced to the source frame rate,
// rather than blasted out as fast as the encoder can go). Writes an SDP
// file describing the stream so a receiver can open it without
// out-of-band signaling.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

namespace {

void die_on_error(int err, const char* what) {
    if (err < 0) {
        char buf[256];
        av_strerror(err, buf, sizeof(buf));
        std::fprintf(stderr, "%s: %s\n", what, buf);
        std::exit(1);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: rtp_sender <input> <rtp://host:port> <sdp_out_path> [max_frames]\n");
        return 1;
    }
    const std::string input_path = argv[1];
    const std::string rtp_url = argv[2];
    const std::string sdp_path = argv[3];
    const int max_frames = argc > 4 ? std::atoi(argv[4]) : 0;

    // --- Decoder for the input file ---
    AVFormatContext* in_fmt = nullptr;
    die_on_error(avformat_open_input(&in_fmt, input_path.c_str(), nullptr, nullptr),
                 "avformat_open_input");
    die_on_error(avformat_find_stream_info(in_fmt, nullptr), "avformat_find_stream_info");

    int video_idx = av_find_best_stream(in_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx < 0) {
        std::fprintf(stderr, "no video stream in input\n");
        return 1;
    }
    AVStream* in_stream = in_fmt->streams[video_idx];
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);
    die_on_error(avcodec_open2(dec_ctx, dec, nullptr), "avcodec_open2 (decoder)");

    AVRational frame_rate = av_guess_frame_rate(in_fmt, in_stream, nullptr);
    if (frame_rate.num == 0) frame_rate = {30, 1};

    // --- H.264 encoder ---
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVCodecContext* enc_ctx = avcodec_alloc_context3(enc);
    enc_ctx->width = dec_ctx->width;
    enc_ctx->height = dec_ctx->height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = av_inv_q(frame_rate);
    enc_ctx->framerate = frame_rate;
    enc_ctx->gop_size = static_cast<int>(frame_rate.num / frame_rate.den); // ~1 keyframe/sec
    enc_ctx->max_b_frames = 0; // B-frames add latency we don't want for "low-latency" streaming
    av_opt_set(enc_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);
    // Puts SPS/PPS in enc_ctx->extradata instead of only inline in the
    // bitstream. The RTP muxer folds that into the SDP as
    // sprop-parameter-sets, so a receiver has the parameter sets before
    // the stream even starts — it doesn't depend on catching one
    // specific in-band NAL, which a late join or a lost packet would
    // otherwise miss with no way to recover mid-stream.
    enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    die_on_error(avcodec_open2(enc_ctx, enc, nullptr), "avcodec_open2 (encoder)");

    // --- RTP output ---
    AVFormatContext* out_fmt = nullptr;
    die_on_error(avformat_alloc_output_context2(&out_fmt, nullptr, "rtp", rtp_url.c_str()),
                 "avformat_alloc_output_context2");
    AVStream* out_stream = avformat_new_stream(out_fmt, nullptr);
    die_on_error(avcodec_parameters_from_context(out_stream->codecpar, enc_ctx),
                 "avcodec_parameters_from_context");
    out_stream->time_base = enc_ctx->time_base;

    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        die_on_error(avio_open(&out_fmt->pb, rtp_url.c_str(), AVIO_FLAG_WRITE), "avio_open");
    }
    die_on_error(avformat_write_header(out_fmt, nullptr), "avformat_write_header");

    // Dump the negotiated SDP so a receiver can build its own (port-rewritten) copy.
    {
        char sdp_buf[4096];
        av_sdp_create(&out_fmt, 1, sdp_buf, sizeof(sdp_buf));
        std::FILE* sdp_file = std::fopen(sdp_path.c_str(), "w");
        if (sdp_file) {
            std::fwrite(sdp_buf, 1, std::strlen(sdp_buf), sdp_file);
            std::fclose(sdp_file);
        }
        std::printf("wrote SDP to %s\n", sdp_path.c_str());
    }

    AVPacket* in_pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVPacket* out_pkt = av_packet_alloc();

    int64_t frame_count = 0;
    int64_t stream_start_us = av_gettime();
    const double frame_period_us = 1e6 * enc_ctx->time_base.num / enc_ctx->time_base.den;

    while (av_read_frame(in_fmt, in_pkt) >= 0) {
        if (in_pkt->stream_index == video_idx && avcodec_send_packet(dec_ctx, in_pkt) == 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                frame->pts = frame_count;

                if (avcodec_send_frame(enc_ctx, frame) == 0) {
                    while (avcodec_receive_packet(enc_ctx, out_pkt) == 0) {
                        av_packet_rescale_ts(out_pkt, enc_ctx->time_base, out_stream->time_base);
                        out_pkt->stream_index = out_stream->index;
                        av_interleaved_write_frame(out_fmt, out_pkt);
                    }
                }

                // Pace to the source frame rate: without this, the RTP
                // muxer would emit the whole clip as fast as the encoder
                // can run, which defeats the point of a "live" stream.
                int64_t target_us = stream_start_us + static_cast<int64_t>(frame_count * frame_period_us);
                int64_t now_us = av_gettime();
                if (target_us > now_us) {
                    std::this_thread::sleep_for(std::chrono::microseconds(target_us - now_us));
                }

                ++frame_count;
                if (max_frames && frame_count >= max_frames) break;
            }
        }
        av_packet_unref(in_pkt);
        if (max_frames && frame_count >= max_frames) break;
    }

    av_write_trailer(out_fmt);

    std::printf("== rtp_sender report ==\n");
    std::printf("frames sent: %lld\n", static_cast<long long>(frame_count));
    std::printf("elapsed:     %.2f s\n", (av_gettime() - stream_start_us) / 1e6);

    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);
    av_frame_free(&frame);
    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_fmt->pb);
    avformat_free_context(out_fmt);
    avformat_close_input(&in_fmt);
    return 0;
}
