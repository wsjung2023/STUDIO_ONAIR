// Verification tool: decode the produced screen.mp4, dump a few frames as
// openable 24-bit BMP images, and compute per-frame content statistics
// (mean, standard deviation, and unique-color count) proving the frames are
// NOT a single flat colour. Uses only the audited FFmpeg libraries (no
// ffmpeg.exe, which the vcpkg build does not ship).

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeBmp(const fs::path& path, const std::uint8_t* rgb, int width, int height,
              int stride) {
    const int rowBytes = width * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int imageSize = (rowBytes + pad) * height;
    const int fileSize = 54 + imageSize;
    std::vector<std::uint8_t> header(54, 0);
    header[0] = 'B';
    header[1] = 'M';
    auto put32 = [&](int off, std::uint32_t v) {
        header[off] = v & 0xFF;
        header[off + 1] = (v >> 8) & 0xFF;
        header[off + 2] = (v >> 16) & 0xFF;
        header[off + 3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&](int off, std::uint16_t v) {
        header[off] = v & 0xFF;
        header[off + 1] = (v >> 8) & 0xFF;
    };
    put32(2, static_cast<std::uint32_t>(fileSize));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));
    put16(26, 1);
    put16(28, 24);
    put32(34, static_cast<std::uint32_t>(imageSize));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), 54);
    std::vector<std::uint8_t> row(rowBytes + pad, 0);
    // BMP is bottom-up; source RGB24 is top-down.
    for (int y = height - 1; y >= 0; --y) {
        const std::uint8_t* src = rgb + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 3 + 2];  // B
            row[x * 3 + 1] = src[x * 3 + 1];  // G
            row[x * 3 + 2] = src[x * 3 + 0];  // R
        }
        out.write(reinterpret_cast<const char*>(row.data()), rowBytes + pad);
    }
}

// Encode one RGB24 frame to a real .png using libavcodec's PNG encoder.
bool writePng(const fs::path& path, const std::uint8_t* rgb, int width, int height,
              int stride) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) return false;
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_RGB24;
    ctx->time_base = AVRational{1, 1};
    bool ok = false;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_open2(ctx, codec, nullptr) >= 0 && frame != nullptr &&
        pkt != nullptr) {
        frame->format = AV_PIX_FMT_RGB24;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) >= 0) {
            for (int y = 0; y < height; ++y) {
                std::copy_n(rgb + static_cast<std::size_t>(y) * stride, width * 3,
                            frame->data[0] + static_cast<std::size_t>(y) *
                                                 frame->linesize[0]);
            }
            if (avcodec_send_frame(ctx, frame) >= 0 &&
                avcodec_send_frame(ctx, nullptr) >= 0 &&
                avcodec_receive_packet(ctx, pkt) >= 0) {
                std::ofstream out(path, std::ios::binary);
                out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
                ok = true;
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

struct Stats {
    double mean{0};
    double stddev{0};
    std::size_t uniqueColors{0};
};

Stats computeStats(const std::uint8_t* rgb, int width, int height, int stride) {
    double sum = 0;
    double sumSq = 0;
    const double n = static_cast<double>(width) * height * 3.0;
    std::unordered_set<std::uint32_t> colors;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = rgb + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t r = src[x * 3 + 0];
            const std::uint8_t g = src[x * 3 + 1];
            const std::uint8_t b = src[x * 3 + 2];
            sum += r + g + b;
            sumSq += static_cast<double>(r) * r + static_cast<double>(g) * g +
                     static_cast<double>(b) * b;
            colors.insert((static_cast<std::uint32_t>(r) << 16) |
                          (static_cast<std::uint32_t>(g) << 8) | b);
        }
    }
    Stats s;
    s.mean = sum / n;
    s.stddev = std::sqrt(std::max(0.0, sumSq / n - s.mean * s.mean));
    s.uniqueColors = colors.size();
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <mp4> <outdir>\n", argv[0]);
        return 2;
    }
    const fs::path mp4 = argv[1];
    const fs::path outDir = argv[2];

    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, mp4.string().c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
        std::fprintf(stderr, "open input failed\n");
        return 1;
    }
    const int vs = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        std::fprintf(stderr, "no video stream\n");
        avformat_close_input(&in);
        return 1;
    }
    const AVCodec* dec = avcodec_find_decoder(in->streams[vs]->codecpar->codec_id);
    AVCodecContext* dc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dc, in->streams[vs]->codecpar);
    if (avcodec_open2(dc, dec, nullptr) < 0) {
        std::fprintf(stderr, "decoder open failed\n");
        return 1;
    }

    const int width = dc->width;
    const int height = dc->height;
    SwsContext* sws = nullptr;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    std::uint8_t* dstData[4] = {rgb.data(), nullptr, nullptr, nullptr};
    int dstLines[4] = {width * 3, 0, 0, 0};

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int index = 0;
    // Frame indices to dump (spread across the take).
    const std::vector<int> wanted = {5, 70, 140};
    int rc = 0;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(dc, pkt) >= 0) {
            while (avcodec_receive_frame(dc, frame) >= 0) {
                bool dump = false;
                for (int w : wanted) if (w == index) dump = true;
                if (dump) {
                    sws = sws_getCachedContext(
                        sws, width, height,
                        static_cast<AVPixelFormat>(frame->format), width, height,
                        AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    sws_scale(sws, frame->data, frame->linesize, 0, height, dstData,
                              dstLines);
                    const auto stats = computeStats(rgb.data(), width, height, width * 3);
                    char stem[64];
                    std::snprintf(stem, sizeof(stem), "frame_%03d", index);
                    const auto bmpPath = outDir / (std::string{stem} + ".bmp");
                    const auto pngPath = outDir / (std::string{stem} + ".png");
                    writeBmp(bmpPath, rgb.data(), width, height, width * 3);
                    const bool png = writePng(pngPath, rgb.data(), width, height,
                                              width * 3);
                    std::printf(
                        "frame %3d: mean=%.2f stddev=%.2f uniqueColors=%zu -> %s%s\n",
                        index, stats.mean, stats.stddev, stats.uniqueColors,
                        pngPath.string().c_str(), png ? "" : " (png FAILED, bmp only)");
                }
                ++index;
            }
        }
        av_packet_unref(pkt);
    }
    std::printf("decoded %d frames total\n", index);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&dc);
    avformat_close_input(&in);
    return rc;
}
