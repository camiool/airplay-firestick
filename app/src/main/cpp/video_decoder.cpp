#include "video_decoder.h"

#include "log.h"

#include <media/NdkMediaFormat.h>

#include <cstring>
#include <ctime>

namespace {

constexpr const char *kMimeAvc = "video/avc";
constexpr const char *kMimeHevc = "video/hevc";
// How long submit() may block waiting for a free decoder input buffer.
constexpr int kInputWaitMs = 1500;

int64_t nowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

struct Nal {
    size_t start;  // offset of the start code
    size_t payload;  // offset of the first byte after the start code
    size_t end;  // one past the last byte
};

std::vector<Nal> splitAnnexB(const uint8_t *d, size_t len) {
    std::vector<Nal> nals;
    auto findStart = [&](size_t from, size_t *scLen) -> size_t {
        for (size_t k = from; k + 3 <= len; ++k) {
            if (d[k] == 0 && d[k + 1] == 0) {
                if (d[k + 2] == 1) {
                    *scLen = 3;
                    return k;
                }
                if (k + 4 <= len && d[k + 2] == 0 && d[k + 3] == 1) {
                    *scLen = 4;
                    return k;
                }
            }
        }
        *scLen = 0;
        return len;
    };
    size_t sc = 0;
    size_t pos = findStart(0, &sc);
    while (pos < len) {
        Nal n{};
        n.start = pos;
        n.payload = pos + sc;
        size_t nextSc = 0;
        size_t next = findStart(n.payload, &nextSc);
        n.end = next;
        if (n.payload < n.end) nals.push_back(n);
        pos = next;
        sc = nextSc;
    }
    return nals;
}

int nalType(uint8_t header, bool h265) {
    return h265 ? (header >> 1) & 0x3f : header & 0x1f;
}

bool isParamSet(int type, bool h265) {
    return h265 ? (type == 32 || type == 33 || type == 34) : (type == 7 || type == 8);
}

const uint8_t kStartCode[4] = {0, 0, 0, 1};

void appendNal(std::vector<uint8_t> &out, const uint8_t *d, const Nal &n) {
    out.insert(out.end(), kStartCode, kStartCode + 4);
    out.insert(out.end(), d + n.payload, d + n.end);
}

}  // namespace

struct VideoDecoder::Instance {
    AMediaCodec *codec = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
};

VideoDecoder::~VideoDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopCodecLocked();
    if (window_) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}

void VideoDecoder::setMaxSize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxWidth_ = width;
    maxHeight_ = height;
}

void VideoDecoder::setSizeCallback(SizeCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    sizeCb_ = std::move(cb);
}

void VideoDecoder::setWindow(ANativeWindow *window) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window == window_) return;
    stopCodecLocked();
    if (window_) ANativeWindow_release(window_);
    window_ = window;
    if (window_) {
        ANativeWindow_acquire(window_);
        // Restart immediately if we already know the stream parameters; the
        // picture recovers on the next key frame.
        if (!paramSets_.empty()) startCodecLocked(h265_, paramSets_);
    }
}

void VideoDecoder::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopCodecLocked();
    paramSets_.clear();
    width_ = 0;
    height_ = 0;
}

VideoDecoder::Stats VideoDecoder::stats() {
    Stats s;
    s.framesIn = framesIn_.load();
    s.framesRendered = framesRendered_.load();
    s.framesDropped = framesDropped_.load();
    s.avgDecodeUs = avgDecodeUs_.load();
    s.width = width_.load();
    s.height = height_.load();
    s.lowLatencyMode = lowLatency_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    s.h265 = h265_;
    return s;
}

bool VideoDecoder::startCodecLocked(bool h265, const std::vector<uint8_t> &paramSets) {
    stopCodecLocked();
    if (!window_) return false;

    const char *mime = h265 ? kMimeHevc : kMimeAvc;

    // csd buffers
    std::vector<uint8_t> csd0, csd1;
    {
        auto nals = splitAnnexB(paramSets.data(), paramSets.size());
        for (const auto &n : nals) {
            int t = nalType(paramSets[n.payload], h265);
            if (!h265 && t == 8) {
                appendNal(csd1, paramSets.data(), n);
            } else {
                appendNal(csd0, paramSets.data(), n);
            }
        }
    }

    // Try with all low-latency hints first, then progressively plainer formats
    // in case a vendor decoder rejects an unknown key.
    for (int attempt = 0; attempt < 2; ++attempt) {
        AMediaCodec *codec = AMediaCodec_createDecoderByType(mime);
        if (!codec) {
            LOGE("No decoder for %s", mime);
            return false;
        }
        AMediaFormat *fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, maxWidth_);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, maxHeight_);
        AMediaFormat_setInt32(fmt, "max-width", maxWidth_);
        AMediaFormat_setInt32(fmt, "max-height", maxHeight_);
        // Key frames of fast-moving game scenes can be large; leave ample room so
        // no frame is ever rejected as too big for the input buffer.
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                              maxWidth_ * maxHeight_ * 3 / 2);
        if (!csd0.empty()) AMediaFormat_setBuffer(fmt, "csd-0", csd0.data(), csd0.size());
        if (!csd1.empty()) AMediaFormat_setBuffer(fmt, "csd-1", csd1.data(), csd1.size());
        if (attempt == 0) {
            AMediaFormat_setInt32(fmt, "low-latency", 1);               // Android 11+
            AMediaFormat_setInt32(fmt, "priority", 0);                  // realtime
            AMediaFormat_setInt32(fmt, "vendor.low-latency.enable", 1); // Qualcomm
            AMediaFormat_setInt32(fmt, "vdec-lowlatency", 1);           // MediaTek (OMX)
            AMediaFormat_setInt32(fmt, "vendor.mtk.vdec.lowlatency.mode", 1); // MediaTek (C2)
        }
        media_status_t st = AMediaCodec_configure(codec, fmt, window_, nullptr, 0);
        AMediaFormat_delete(fmt);
        if (st == AMEDIA_OK) st = AMediaCodec_start(codec);
        if (st != AMEDIA_OK) {
            LOGW("Decoder configure/start failed (attempt %d, status %d)", attempt, st);
            AMediaCodec_delete(codec);
            continue;
        }
        lowLatency_ = (attempt == 0);
        auto *inst = new Instance();
        inst->codec = codec;
        inst->running = true;
        inst->thread = std::thread(&VideoDecoder::outputLoop, this, inst);
        inst_ = inst;
        LOGI("Video decoder started: %s, max %dx%d, low-latency hints %s", mime, maxWidth_,
             maxHeight_, attempt == 0 ? "on" : "off");
        return true;
    }
    return false;
}

void VideoDecoder::stopCodecLocked() {
    if (!inst_) return;
    inst_->running = false;
    if (inst_->thread.joinable()) inst_->thread.join();
    AMediaCodec_stop(inst_->codec);
    AMediaCodec_delete(inst_->codec);
    delete inst_;
    inst_ = nullptr;
}

void VideoDecoder::submit(const uint8_t *data, int len, bool h265) {
    if (!data || len <= 4) return;
    // UxPlay marks undecryptable packets by overwriting the first start-code byte.
    if (data[0] != 0) return;
    framesIn_++;

    auto nals = splitAnnexB(data, static_cast<size_t>(len));
    if (nals.empty()) return;

    std::vector<uint8_t> params;
    std::vector<uint8_t> frame;
    frame.reserve(static_cast<size_t>(len));
    for (const auto &n : nals) {
        int t = nalType(data[n.payload], h265);
        if (isParamSet(t, h265)) {
            appendNal(params, data, n);
        } else {
            appendNal(frame, data, n);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!params.empty() && (params != paramSets_ || h265 != h265_ || !inst_)) {
        // New stream or resolution/orientation change: (re)create the decoder.
        // The access unit carrying new parameter sets is always a key frame.
        paramSets_ = params;
        h265_ = h265;
        startCodecLocked(h265, paramSets_);
    }
    if (!inst_ || frame.empty()) return;

    AMediaCodec *codec = inst_->codec;
    // Never drop an input frame unless the decoder is really stuck: iOS sends key
    // frames only on stream (re)starts and rotation changes, so every later frame
    // references the one before it. A single missing frame corrupts the picture
    // (blocky/garbled pixels) until the next key frame, which may never come.
    // The MediaTek decoder on Fire TV needs well over 100 ms to accept input
    // right after a restart (e.g. when a game switches to landscape).
    ssize_t idx = -1;
    for (int waitedMs = 0; waitedMs < kInputWaitMs && idx < 0; waitedMs += 50) {
        idx = AMediaCodec_dequeueInputBuffer(codec, 50000);
        if (idx < 0 && idx != AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;  // codec error
    }
    if (idx < 0) {
        LOGW("No decoder input buffer available after %d ms (status %zd), dropping frame",
             kInputWaitMs, idx);
        framesDropped_++;
        return;
    }
    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(idx), &cap);
    if (!buf || cap < frame.size()) {
        LOGW("Decoder input buffer too small (%zu < %zu)", cap, frame.size());
        AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(idx), 0, 0, nowUs(), 0);
        framesDropped_++;
        return;
    }
    memcpy(buf, frame.data(), frame.size());
    AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(idx), 0, frame.size(), nowUs(), 0);
}

void VideoDecoder::outputLoop(VideoDecoder *self, Instance *inst) {
    AMediaCodec *codec = inst->codec;

    auto handleFormat = [&]() {
        AMediaFormat *fmt = AMediaCodec_getOutputFormat(codec);
        if (!fmt) return;
        int32_t w = 0, h = 0;
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
        int32_t l = 0, t = 0, r = 0, b = 0;
        if (AMediaFormat_getRect(fmt, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b)) {
            w = r - l + 1;
            h = b - t + 1;
        }
        AMediaFormat_delete(fmt);
        LOGI("Video output format: %dx%d", w, h);
        if (w > 0 && h > 0) {
            self->width_ = w;
            self->height_ = h;
            SizeCallback cb;
            {
                // sizeCb_ is only written before streaming starts; copy without the
                // decoder mutex (which may be held by stopCodecLocked joining us).
                cb = self->sizeCb_;
            }
            if (cb) cb(w, h);
        }
    };

    while (inst->running.load()) {
        AMediaCodecBufferInfo info{};
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 20000);
        if (idx >= 0) {
            // Skip ahead to the newest decoded frame, dropping stale ones.
            for (;;) {
                AMediaCodecBufferInfo next{};
                ssize_t idx2 = AMediaCodec_dequeueOutputBuffer(codec, &next, 0);
                if (idx2 >= 0) {
                    AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), false);
                    self->framesDropped_++;
                    idx = idx2;
                    info = next;
                } else if (idx2 == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    handleFormat();
                } else {
                    break;
                }
            }
            int64_t lat = nowUs() - info.presentationTimeUs;
            if (lat >= 0 && lat < 2000000) {
                uint64_t prev = self->avgDecodeUs_.load();
                uint64_t ema = prev == 0 ? static_cast<uint64_t>(lat)
                                         : (prev * 15 + static_cast<uint64_t>(lat)) / 16;
                self->avgDecodeUs_ = ema;
            }
            bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), !eos);
            if (!eos) self->framesRendered_++;
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            handleFormat();
        }
        // TRY_AGAIN_LATER / OUTPUT_BUFFERS_CHANGED: just loop.
    }
}
