#pragma once

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

/*
 * AirPlay audio: AAC-ELD (screen mirroring), AAC-LC or ALAC (audio-only AirPlay)
 * decoded with MediaCodec and played through AAudio in low-latency mode.
 * Audio is played as soon as it is decoded (no sync to the sender's clock);
 * if the output queue grows past `kMaxQueuedMs`, old audio is discarded so
 * the delay never builds up during long gaming sessions.
 */
class AudioPlayer {
public:
    struct Stats {
        uint64_t framesIn = 0;
        int32_t xruns = 0;
        int32_t bufferMs = 0;
        int codecType = 0;
        bool running = false;
    };

    AudioPlayer() = default;
    ~AudioPlayer();

    /* ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (AirPlay "compression type") */
    bool start(int ct);
    void stop();
    void flush();
    void submit(const uint8_t *data, int len);
    /* AirPlay volume in dB: -144 = mute, otherwise -30..0 */
    void setVolumeDb(float db);
    void setEnabled(bool enabled) { enabled_ = enabled; }
    Stats stats();

private:
    static void outputLoop(AudioPlayer *self);
    bool openStreamLocked(int sampleRate, int channels);
    void closeStreamLocked();

    std::mutex mutex_;
    AMediaCodec *codec_ = nullptr;
    AAudioStream *stream_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<float> gain_{1.0f};
    std::atomic<uint64_t> framesIn_{0};
    int ct_ = 0;
    int streamRate_ = 0;
    int streamChannels_ = 0;
    int32_t lastXruns_ = 0;
};
