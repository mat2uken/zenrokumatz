// Counts video packets against decoded frames, and classifies access units.
//
// cm-verify requires one decoded frame per packet. A stream whose first packet
// only carries parameter sets breaks that equality without being damaged, so
// this probe reports both numbers and how many packets hold a slice.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

static int nal_length_size(const AVCodecParameters* par) {
    if (!par->extradata || par->extradata_size < 5) return 4;
    if (par->codec_id == AV_CODEC_ID_H264) return (par->extradata[4] & 3) + 1;
    return (par->extradata[21] & 3) + 1;
}

// Returns the number of slice-bearing NAL units in one packet.
static int vcl_nals(const AVPacket* packet, AVCodecID codec, int length) {
    const uint8_t* data = packet->data;
    size_t size = (size_t)packet->size, position = 0;
    int count = 0;
    auto is_vcl = [&](int type) {
        if (codec == AV_CODEC_ID_H264) return type == 1 || type == 5;
        return type >= 0 && type <= 31;
    };
    bool prefixed = true;
    for (; position + (size_t)length <= size;) {
        unsigned n = 0;
        for (int k = 0; k < length; ++k) n = (n << 8) | data[position + k];
        if (!n || position + length + 1 >= size) { prefixed = false; break; }
        if (data[position + length] & 0x80) { prefixed = false; break; }
        position += (size_t)length + n;
    }
    if (prefixed) {
        position = 0;
        while (position + (size_t)length <= size) {
            unsigned n = 0;
            for (int k = 0; k < length; ++k) n = (n << 8) | data[position + k];
            position += length;
            if (!n || position >= size) break;
            int type = codec == AV_CODEC_ID_H264 ? (data[position] & 31) : ((data[position] >> 1) & 63);
            if (is_vcl(type)) ++count;
            position += n;
        }
        return count;
    }
    // Annex B fallback: scan start codes.
    for (size_t i = 0; i + 3 < size;) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            int type = codec == AV_CODEC_ID_H264 ? (data[i + 3] & 31) : ((data[i + 3] >> 1) & 63);
            if (is_vcl(type)) ++count;
            i += 4;
        } else {
            ++i;
        }
    }
    return count;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cm-count input.mp4\n");
        return 2;
    }
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, argv[1], nullptr, nullptr) < 0) return 3;
    if (avformat_find_stream_info(format, nullptr) < 0) return 3;
    int vi = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) return 4;
    const AVCodec* codec = avcodec_find_decoder(format->streams[vi]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, format->streams[vi]->codecpar);
    ctx->thread_count = 4;
    if (avcodec_open2(ctx, codec, nullptr) < 0) return 5;
    const int length = nal_length_size(format->streams[vi]->codecpar);

    long packets = 0, vcl_packets = 0, frames = 0, decode_errors = 0;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int rc;
    while ((rc = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == vi) {
            ++packets;
            if (vcl_nals(packet, codec->id, length) > 0) ++vcl_packets;
            if (avcodec_send_packet(ctx, packet) >= 0) {
                int got;
                while ((got = avcodec_receive_frame(ctx, frame)) >= 0) {
                    ++frames;
                    if (frame->decode_error_flags) ++decode_errors;
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frame) >= 0) {
        ++frames;
        if (frame->decode_error_flags) ++decode_errors;
        av_frame_unref(frame);
    }
    printf("{\"packets\":%ld,\"vcl_packets\":%ld,\"frames\":%ld,\"decode_errors\":%ld,"
           "\"codec\":\"%s\"}\n",
           packets, vcl_packets, frames, decode_errors, avcodec_get_name(codec->id));
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&format);
    return 0;
}
