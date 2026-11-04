#pragma once

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/*
 * Hardware H.264 / H.265 decoder tuned for minimum latency:
 *  - frames are queued the moment they arrive from the network (no jitter buffer,
 *    no timestamp-based scheduling),
 *  - the output side always shows only the newest decoded frame and drops any
 *    older frame that is still waiting (so latency never accumulates),
 *  - the platform "low-latency" decoder mode is requested when available.
 */
class VideoDecoder {
public:
    using SizeCallback = std::function<void(int width, int height)>;

    struct Stats {
        uint64_t framesIn = 0;
        uint64_t framesRendered = 0;
        uint64_t framesDropped = 0;
        uint64_t avgDecodeUs = 0;  // exponential moving average of queue->output time
        int width = 0;
        int height = 0;
        bool h265 = false;
        bool lowLatencyMode = false;
    };

    VideoDecoder() = default;
    ~VideoDecoder();

    void setMaxSize(int width, int height);
    void setSizeCallback(SizeCallback cb);

    /* Window may be null (decoding is then paused). Takes its own reference. */
    void setWindow(ANativeWindow *window);

    /* Called from the AirPlay mirror thread with one Annex-B access unit. */
    void submit(const uint8_t *data, int len, bool h265);

    /* Release the codec (e.g. client disconnected). Parameter sets are forgotten. */
    void reset();

    Stats stats();

private:
    struct Instance;

    bool startCodecLocked(bool h265, const std::vector<uint8_t> &paramSets);
    void stopCodecLocked();
    static void outputLoop(VideoDecoder *self, Instance *inst);

    std::mutex mutex_;
    ANativeWindow *window_ = nullptr;
    Instance *inst_ = nullptr;
    std::vector<uint8_t> paramSets_;
    bool h265_ = false;
    int maxWidth_ = 1920;
    int maxHeight_ = 1080;
    SizeCallback sizeCb_;

    std::atomic<uint64_t> framesIn_{0};
    std::atomic<uint64_t> framesRendered_{0};
    std::atomic<uint64_t> framesDropped_{0};
    std::atomic<uint64_t> avgDecodeUs_{0};
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<bool> lowLatency_{false};
};
