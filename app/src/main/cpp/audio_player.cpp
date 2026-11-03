#include "audio_player.h"

#include "log.h"

#include <media/NdkMediaFormat.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

constexpr int kMaxQueuedMs = 120;

int64_t nowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

// AudioSpecificConfig for AAC-ELD, 44.1 kHz, stereo, 480 samples/frame (as sent by iOS).
const uint8_t kAacEldConfig[] = {0xF8, 0xE8, 0x50, 0x00};
// AudioSpecificConfig for AAC-LC, 44.1 kHz, stereo.
const uint8_t kAacLcConfig[] = {0x12, 0x10};
// ALACSpecificConfig: 352 frames/packet, 16 bit, 2 ch, 44.1 kHz.
const uint8_t kAlacConfig[] = {0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a,
                               0x0e, 0x02, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};

}  // namespace

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start(int ct) {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    const char *mime = nullptr;
    const uint8_t *csd = nullptr;
    size_t csdLen = 0;
    switch (ct) {
        case 8:
            mime = "audio/mp4a-latm";
            csd = kAacEldConfig;
            csdLen = sizeof(kAacEldConfig);
            break;
        case 4:
            mime = "audio/mp4a-latm";
            csd = kAacLcConfig;
            csdLen = sizeof(kAacLcConfig);
            break;
        case 2:
            mime = "audio/alac";
            csd = kAlacConfig;
            csdLen = sizeof(kAlacConfig);
            break;
        default:
            LOGW("Unsupported AirPlay audio type ct=%d", ct);
            return false;
    }
    AMediaCodec *codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) {
        LOGW("No audio decoder for %s (ct=%d); audio disabled for this session", mime, ct);
        return false;
    }
    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, 44100);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    if (ct == 8) AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_AAC_PROFILE, 39);
    if (ct == 4) AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_AAC_PROFILE, 2);
    AMediaFormat_setBuffer(fmt, "csd-0", csd, csdLen);
    media_status_t st = AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st == AMEDIA_OK) st = AMediaCodec_start(codec);
    if (st != AMEDIA_OK) {
        LOGW("Audio decoder configure/start failed: %d", st);
        AMediaCodec_delete(codec);
        return false;
    }
    codec_ = codec;
    ct_ = ct;
    running_ = true;
    thread_ = std::thread(&AudioPlayer::outputLoop, this);
    LOGI("Audio started (ct=%d, %s)", ct, mime);
    return true;
}

void AudioPlayer::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!codec_) return;
        running_ = false;
        t = std::move(thread_);
    }
    if (t.joinable()) t.join();
    std::lock_guard<std::mutex> lock(mutex_);
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    closeStreamLocked();
    ct_ = 0;
}

void AudioPlayer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_) {
        AAudioStream_requestPause(stream_);
        AAudioStream_requestFlush(stream_);
        AAudioStream_requestStart(stream_);
    }
}

void AudioPlayer::setVolumeDb(float db) {
    float gain;
    if (db <= -144.0f || db <= -30.0f) {
        gain = 0.0f;
    } else if (db >= 0.0f) {
        gain = 1.0f;
    } else {
        // Map the AirPlay slider (-30..0 dB) to a perceptual curve.
        float frac = (30.0f + db) / 30.0f;
        gain = frac * frac;
    }
    gain_ = gain;
}

AudioPlayer::Stats AudioPlayer::stats() {
    Stats s;
    s.framesIn = framesIn_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    s.codecType = ct_;
    s.running = codec_ != nullptr;
    if (stream_) {
        s.xruns = AAudioStream_getXRunCount(stream_);
        int64_t queued = AAudioStream_getFramesWritten(stream_) - AAudioStream_getFramesRead(stream_);
        if (streamRate_ > 0) s.bufferMs = static_cast<int32_t>(queued * 1000 / streamRate_);
    }
    return s;
}

void AudioPlayer::submit(const uint8_t *data, int len) {
    if (!data || len <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!codec_) return;
    framesIn_++;
    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 20000);
    if (idx < 0) return;
    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
    if (!buf || cap < static_cast<size_t>(len)) {
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, nowUs(), 0);
        return;
    }
    memcpy(buf, data, static_cast<size_t>(len));
    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, static_cast<size_t>(len), nowUs(), 0);
}

bool AudioPlayer::openStreamLocked(int sampleRate, int channels) {
    closeStreamLocked();
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, sampleRate);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStream *s = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(r));
        return false;
    }
    int32_t burst = AAudioStream_getFramesPerBurst(s);
    if (burst > 0) AAudioStream_setBufferSizeInFrames(s, burst * 4);
    AAudioStream_requestStart(s);
    stream_ = s;
    streamRate_ = AAudioStream_getSampleRate(s);
    streamChannels_ = AAudioStream_getChannelCount(s);
    lastXruns_ = 0;
    LOGI("AAudio stream: %d Hz, %d ch, burst %d, buffer %d", streamRate_, streamChannels_, burst,
         AAudioStream_getBufferSizeInFrames(s));
    return true;
}

void AudioPlayer::closeStreamLocked() {
    if (stream_) {
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
    streamRate_ = 0;
    streamChannels_ = 0;
}

void AudioPlayer::outputLoop(AudioPlayer *self) {
    int sampleRate = 44100;
    int channels = 2;
    std::vector<int16_t> pcm;
    while (self->running_.load()) {
        AMediaCodec *codec;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            codec = self->codec_;
        }
        if (!codec) break;
        AMediaCodecBufferInfo info{};
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 20000);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(codec);
            if (fmt) {
                AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sampleRate);
                AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
                AMediaFormat_delete(fmt);
                LOGI("Audio output format: %d Hz, %d ch", sampleRate, channels);
            }
            continue;
        }
        if (idx < 0) continue;

        size_t cap = 0;
        uint8_t *out = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(idx), &cap);
        size_t bytes = static_cast<size_t>(info.size);
        if (out && bytes > 0 && self->enabled_.load()) {
            pcm.resize(bytes / sizeof(int16_t));
            memcpy(pcm.data(), out + info.offset, pcm.size() * sizeof(int16_t));
            float g = self->gain_.load();
            if (g < 0.999f) {
                for (auto &smp : pcm) smp = static_cast<int16_t>(static_cast<float>(smp) * g);
            }
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (!self->stream_ || self->streamRate_ != sampleRate || self->streamChannels_ != channels) {
                self->openStreamLocked(sampleRate, channels);
            }
            AAudioStream *s = self->stream_;
            if (s) {
                int32_t frames = static_cast<int32_t>(pcm.size() / static_cast<size_t>(channels));
                int64_t queued = AAudioStream_getFramesWritten(s) - AAudioStream_getFramesRead(s);
                int64_t maxQueued = static_cast<int64_t>(self->streamRate_) * kMaxQueuedMs / 1000;
                if (queued > maxQueued) {
                    // Too much delay accumulated (e.g. Wi-Fi burst): drop this chunk to catch up.
                } else {
                    AAudioStream_write(s, pcm.data(), frames, 10 * 1000000LL);
                }
                // Grow the device buffer a little if we keep underrunning.
                int32_t xr = AAudioStream_getXRunCount(s);
                if (xr > self->lastXruns_) {
                    self->lastXruns_ = xr;
                    int32_t burst = AAudioStream_getFramesPerBurst(s);
                    int32_t size = AAudioStream_getBufferSizeInFrames(s);
                    int32_t capFrames = AAudioStream_getBufferCapacityInFrames(s);
                    int32_t maxSize = self->streamRate_ * 60 / 1000;  // never more than ~60 ms
                    if (burst > 0 && size + burst <= capFrames && size + burst <= maxSize) {
                        AAudioStream_setBufferSizeInFrames(s, size + burst);
                    }
                }
            }
        }
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), false);
    }
}
