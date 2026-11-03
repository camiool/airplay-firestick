/*
 * JNI bridge between the UxPlay AirPlay core (C) and the Android app (Kotlin).
 *
 * Responsibilities:
 *  - start/stop the AirPlay (RAOP) server and its mDNS advertisement,
 *  - route mirrored video to VideoDecoder and audio to AudioPlayer,
 *  - forward connection events to the UI,
 *  - supervise the session (lost-client detection, reset after disconnect),
 *    replacing the GLib main loop used by the desktop UxPlay program.
 */
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_player.h"
#include "dnssd_shim/dns_sd.h"
#include "log.h"
#include "video_decoder.h"

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
}

namespace {

constexpr int kMissedFeedbackLimitSec = 15;

JavaVM *g_vm = nullptr;
pthread_key_t g_envKey;

JNIEnv *getEnv() {
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(g_envKey, env);  // detach automatically when the thread exits
    return env;
}

void detachThread(void *) {
    if (g_vm) g_vm->DetachCurrentThread();
}

void clearException(JNIEnv *env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

struct Receiver {
    // Java side
    jobject listener = nullptr;  // global ref
    jmethodID midRegister = nullptr;
    jmethodID midUnregister = nullptr;
    jmethodID midClient = nullptr;
    jmethodID midConnections = nullptr;
    jmethodID midMirroring = nullptr;
    jmethodID midVideoSize = nullptr;

    // AirPlay core
    raop_t *raop = nullptr;
    dnssd_t *dnssd = nullptr;
    unsigned short port = 0;

    VideoDecoder video;
    AudioPlayer audio;

    std::atomic<int> openConnections{0};
    std::atomic<int> missedFeedback{0};
    std::atomic<bool> resetHttpd{false};
    std::atomic<bool> running{false};
    std::mutex supMutex;
    std::condition_variable supCv;
    std::thread supervisor;
};

std::mutex g_mutex;  // guards g_receiver lifetime
Receiver *g_receiver = nullptr;

/* ---------------- Java callbacks ---------------- */

void notifyConnections(Receiver *r, int n) {
    JNIEnv *env = getEnv();
    if (!env || !r->listener) return;
    env->CallVoidMethod(r->listener, r->midConnections, n);
    clearException(env);
}

void notifyMirroring(Receiver *r, bool active) {
    JNIEnv *env = getEnv();
    if (!env || !r->listener) return;
    env->CallVoidMethod(r->listener, r->midMirroring, active ? JNI_TRUE : JNI_FALSE);
    clearException(env);
}

/* ---------------- dns_sd shim hooks ---------------- */

intptr_t shimRegister(const char *name, const char *regtype, uint16_t port, const uint8_t *txt,
                      uint16_t txtLen) {
    Receiver *r = g_receiver;
    JNIEnv *env = getEnv();
    if (!r || !env) return 0;

    std::vector<std::string> keys, values;
    for (uint16_t pos = 0; pos < txtLen;) {
        uint8_t len = txt[pos];
        std::string entry(reinterpret_cast<const char *>(txt + pos + 1), len);
        pos = static_cast<uint16_t>(pos + 1 + len);
        size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            keys.push_back(entry);
            values.emplace_back();
        } else {
            keys.push_back(entry.substr(0, eq));
            values.push_back(entry.substr(eq + 1));
        }
    }
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray jkeys = env->NewObjectArray(static_cast<jsize>(keys.size()), strCls, nullptr);
    jobjectArray jvals = env->NewObjectArray(static_cast<jsize>(values.size()), strCls, nullptr);
    for (size_t i = 0; i < keys.size(); ++i) {
        jstring k = env->NewStringUTF(keys[i].c_str());
        jstring v = env->NewStringUTF(values[i].c_str());
        env->SetObjectArrayElement(jkeys, static_cast<jsize>(i), k);
        env->SetObjectArrayElement(jvals, static_cast<jsize>(i), v);
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(v);
    }
    jstring jname = env->NewStringUTF(name);
    jstring jtype = env->NewStringUTF(regtype);
    jint handle = env->CallIntMethod(r->listener, r->midRegister, jname, jtype,
                                     static_cast<jint>(port), jkeys, jvals);
    clearException(env);
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(jtype);
    env->DeleteLocalRef(jkeys);
    env->DeleteLocalRef(jvals);
    env->DeleteLocalRef(strCls);
    LOGI("mDNS register %s (%s) port %u -> handle %d", name, regtype, port, handle);
    return handle;
}

void shimUnregister(intptr_t handle) {
    Receiver *r = g_receiver;
    JNIEnv *env = getEnv();
    if (!r || !env) return;
    env->CallVoidMethod(r->listener, r->midUnregister, static_cast<jint>(handle));
    clearException(env);
}

/* ---------------- raop callbacks ---------------- */

Receiver *R(void *cls) { return static_cast<Receiver *>(cls); }

void cb_conn_init(void *cls) {
    int n = ++R(cls)->openConnections;
    LOGI("Open connections: %d", n);
    notifyConnections(R(cls), n);
}

void cb_conn_destroy(void *cls) {
    Receiver *r = R(cls);
    int n = --r->openConnections;
    LOGI("Open connections: %d", n);
    if (n <= 0) {
        r->openConnections = 0;
        r->audio.stop();
    }
    notifyConnections(r, n < 0 ? 0 : n);
}

void cb_conn_feedback(void *cls) { R(cls)->missedFeedback = 0; }

void cb_conn_reset(void *cls, int reason) {
    LOGW("conn_reset reason=%d", reason);
    Receiver *r = R(cls);
    r->resetHttpd = true;
    r->supCv.notify_all();
}

void cb_video_reset(void *cls, reset_type_t type) {
    LOGI("video_reset type=%d", static_cast<int>(type));
    Receiver *r = R(cls);
    r->video.reset();
    notifyMirroring(r, false);
}

int cb_video_set_codec(void *cls, video_codec_t codec) {
    LOGI("Video codec: %s", codec == VIDEO_CODEC_H265 ? "H.265" : "H.264");
    return 0;
}

void cb_video_process(void *cls, raop_ntp_t *, video_decode_struct *data) {
    R(cls)->video.submit(data->data, data->data_len, data->is_h265);
}

void cb_audio_process(void *cls, raop_ntp_t *, audio_decode_struct *data) {
    R(cls)->audio.submit(data->data, data->data_len);
}

void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen,
                         bool *isMedia, uint64_t *audioFormat) {
    LOGI("Audio format ct=%d spf=%d usingScreen=%d isMedia=%d", *ct, *spf, *usingScreen, *isMedia);
    R(cls)->audio.start(*ct);
}

void cb_audio_flush(void *cls) { R(cls)->audio.flush(); }
void cb_video_flush(void *) {}
void cb_video_pause(void *cls) { LOGI("video paused by client"); }
void cb_video_resume(void *cls) { LOGI("video resumed by client"); }
double cb_audio_set_client_volume(void *) { return 0.0; }
void cb_audio_set_volume(void *cls, float volume) { R(cls)->audio.setVolumeDb(volume); }
void cb_audio_set_metadata(void *, const void *, int) {}
void cb_audio_set_coverart(void *, const void *, int) {}
void cb_audio_stop_coverart_rendering(void *) {}
void cb_audio_set_progress(void *, uint32_t *, uint32_t *, uint32_t *) {}
void cb_export_dacp(void *, const char *, const char *) {}

void cb_video_report_size(void *cls, float *ws, float *hs, float *w, float *h) {
    LOGI("Client reports video size: source %.0fx%.0f, stream %.0fx%.0f", *ws, *hs, *w, *h);
}

void cb_mirror_video_running(void *cls, bool running) {
    LOGI("Mirroring %s", running ? "started" : "stopped");
    if (!running) R(cls)->video.reset();
    notifyMirroring(R(cls), running);
}

void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit) {
    LOGI("Connection request from %s (%s) id=%s", name, model, deviceid);
    *admit = true;
    Receiver *r = R(cls);
    JNIEnv *env = getEnv();
    if (env && r->listener) {
        jstring jn = env->NewStringUTF(name ? name : "");
        jstring jm = env->NewStringUTF(model ? model : "");
        env->CallVoidMethod(r->listener, r->midClient, jn, jm);
        clearException(env);
        env->DeleteLocalRef(jn);
        env->DeleteLocalRef(jm);
    }
}

/* HLS ("AirPlay video" from YouTube etc.) is not advertised; these are inert stubs. */
void cb_on_video_play(void *, const char *, const float) {}
void cb_on_video_scrub(void *, const float) {}
void cb_on_video_rate(void *, const float) {}
void cb_on_video_stop(void *) {}
void cb_on_video_acquire_playback_info(void *, playback_info_t *) {}
float cb_on_video_playlist_remove(void *) { return 0.0f; }

void cb_log(void *, int level, const char *msg) {
    int prio = ANDROID_LOG_DEBUG;
    switch (level) {
        case LOGGER_EMERG:
        case LOGGER_ALERT:
        case LOGGER_CRIT:
        case LOGGER_ERR: prio = ANDROID_LOG_ERROR; break;
        case LOGGER_WARNING: prio = ANDROID_LOG_WARN; break;
        case LOGGER_NOTICE:
        case LOGGER_INFO: prio = ANDROID_LOG_INFO; break;
        default: prio = ANDROID_LOG_DEBUG; break;
    }
    __android_log_print(prio, "UxPlay", "%s", msg);
}

/* ---------------- supervisor ---------------- */

void supervisorLoop(Receiver *r) {
    auto lastTick = std::chrono::steady_clock::now();
    while (r->running.load()) {
        {
            std::unique_lock<std::mutex> lk(r->supMutex);
            r->supCv.wait_for(lk, std::chrono::milliseconds(250),
                              [r] { return !r->running.load() || r->resetHttpd.load(); });
        }
        if (!r->running.load()) break;

        auto now = std::chrono::steady_clock::now();
        if (now - lastTick >= std::chrono::seconds(1)) {
            lastTick = now;
            if (r->openConnections.load() > 0) {
                int missed = ++r->missedFeedback;
                if (missed > kMissedFeedbackLimitSec) {
                    LOGW("No heartbeat from client for %d s: resetting", missed);
                    r->resetHttpd = true;
                }
            } else {
                r->missedFeedback = 0;
            }
        }

        if (r->resetHttpd.exchange(false)) {
            LOGI("Resetting AirPlay server");
            r->video.reset();
            r->audio.stop();
            notifyMirroring(r, false);
            raop_stop_httpd(r->raop);
            raop_remove_known_connections(r->raop);
            unsigned short port = r->port;
            raop_start_httpd(r->raop, &port);
            raop_set_port(r->raop, port);
            r->openConnections = 0;
            r->missedFeedback = 0;
            notifyConnections(r, 0);
        }
    }
}

void destroyReceiver(Receiver *r, JNIEnv *env) {
    if (!r) return;
    r->running = false;
    r->supCv.notify_all();
    if (r->supervisor.joinable()) r->supervisor.join();
    // Same order as UxPlay: stop the server first, then withdraw the mDNS records.
    if (r->raop) {
        raop_destroy(r->raop);
        r->raop = nullptr;
    }
    if (r->dnssd) {
        dnssd_unregister_raop(r->dnssd);
        dnssd_unregister_airplay(r->dnssd);
        dnssd_destroy(r->dnssd);
        r->dnssd = nullptr;
    }
    r->audio.stop();
    r->video.reset();
    if (r->listener) env->DeleteGlobalRef(r->listener);
    delete r;
}

std::string jstr(JNIEnv *env, jstring s) {
    if (!s) return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    pthread_key_create(&g_envKey, detachThread);
    dnssd_shim_set_hooks(shimRegister, shimUnregister);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_camiool_airplayfire_AirPlayNative_nativeStart(JNIEnv *env, jclass, jobject listener,
                                                       jstring jname, jbyteArray jhw, jint width,
                                                       jint height, jint fps, jboolean h265,
                                                       jstring jkeyfile, jboolean audioEnabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_receiver) {
        LOGW("nativeStart: already running");
        return JNI_TRUE;
    }
    auto *r = new Receiver();
    r->listener = env->NewGlobalRef(listener);
    jclass cls = env->GetObjectClass(listener);
    r->midRegister = env->GetMethodID(cls, "registerService",
                                      "(Ljava/lang/String;Ljava/lang/String;I[Ljava/lang/String;[Ljava/lang/String;)I");
    r->midUnregister = env->GetMethodID(cls, "unregisterService", "(I)V");
    r->midClient = env->GetMethodID(cls, "onClientRequest", "(Ljava/lang/String;Ljava/lang/String;)V");
    r->midConnections = env->GetMethodID(cls, "onConnectionsChanged", "(I)V");
    r->midMirroring = env->GetMethodID(cls, "onMirroringChanged", "(Z)V");
    r->midVideoSize = env->GetMethodID(cls, "onVideoSize", "(II)V");
    env->DeleteLocalRef(cls);
    if (!r->midRegister || !r->midUnregister || !r->midClient || !r->midConnections ||
        !r->midMirroring || !r->midVideoSize) {
        clearException(env);
        LOGE("Listener is missing callback methods");
        env->DeleteGlobalRef(r->listener);
        delete r;
        return JNI_FALSE;
    }
    g_receiver = r;  // needed by the dns_sd hooks during registration

    std::string name = jstr(env, jname);
    std::string keyfile = jstr(env, jkeyfile);
    std::vector<char> hw(6, 0);
    if (jhw && env->GetArrayLength(jhw) == 6) {
        env->GetByteArrayRegion(jhw, 0, 6, reinterpret_cast<jbyte *>(hw.data()));
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", hw[0] & 0xff, hw[1] & 0xff,
             hw[2] & 0xff, hw[3] & 0xff, hw[4] & 0xff, hw[5] & 0xff);

    r->audio.setEnabled(audioEnabled == JNI_TRUE);
    r->video.setMaxSize(width, height);
    Receiver *rr = r;
    r->video.setSizeCallback([rr](int w, int h) {
        JNIEnv *e = getEnv();
        if (!e || !rr->listener) return;
        e->CallVoidMethod(rr->listener, rr->midVideoSize, w, h);
        clearException(e);
    });

    auto fail = [&](const char *what) {
        LOGE("nativeStart failed: %s", what);
        g_receiver = nullptr;
        destroyReceiver(r, env);
        return JNI_FALSE;
    };

    /* --- dnssd (TXT records) --- */
    int err = 0;
    r->dnssd = dnssd_init(name.c_str(), static_cast<int>(name.size()), hw.data(),
                          static_cast<int>(hw.size()), &err, 0);
    if (err || !r->dnssd) return fail("dnssd_init");
    dnssd_set_airplay_features(r->dnssd, 0, 0);   // no HLS "AirPlay video"
    dnssd_set_airplay_features(r->dnssd, 4, 0);
    dnssd_set_airplay_features(r->dnssd, 42, h265 ? 1 : 0);  // ScreenMultiCodec (H.265 / 4K)

    /* --- raop server --- */
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cls = r;
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.video_report_size = cb_video_report_size;
    cbs.audio_set_metadata = cb_audio_set_metadata;
    cbs.audio_set_coverart = cb_audio_set_coverart;
    cbs.audio_stop_coverart_rendering = cb_audio_stop_coverart_rendering;
    cbs.audio_set_progress = cb_audio_set_progress;
    cbs.report_client_request = cb_report_client_request;
    cbs.export_dacp = cb_export_dacp;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec = cb_video_set_codec;
    cbs.mirror_video_running = cb_mirror_video_running;
    cbs.on_video_play = cb_on_video_play;
    cbs.on_video_scrub = cb_on_video_scrub;
    cbs.on_video_rate = cb_on_video_rate;
    cbs.on_video_stop = cb_on_video_stop;
    cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;
    cbs.on_video_playlist_remove = cb_on_video_playlist_remove;

    r->raop = raop_init(&cbs);
    if (!r->raop) return fail("raop_init");
    raop_set_log_callback(r->raop, cb_log, nullptr);
    raop_set_log_level(r->raop, LOGGER_INFO);
    if (raop_init2(r->raop, 1, mac, keyfile.c_str())) {
        free(r->raop);  // as UxPlay does: raop_init2 failed before httpd existed
        r->raop = nullptr;
        return fail("raop_init2");
    }
    raop_set_plist(r->raop, "width", width);
    raop_set_plist(r->raop, "height", height);
    raop_set_plist(r->raop, "refreshRate", fps);
    raop_set_plist(r->raop, "maxFPS", fps);
    raop_set_plist(r->raop, "overscanned", 0);

    unsigned short tcp[3] = {0, 0, 0};
    unsigned short udp[3] = {0, 0, 0};
    raop_set_tcp_ports(r->raop, tcp);
    raop_set_udp_ports(r->raop, udp);
    unsigned short port = raop_get_port(r->raop);
    if (raop_start_httpd(r->raop, &port) < 0) return fail("raop_start_httpd");
    raop_set_port(r->raop, port);
    r->port = port;
    raop_set_dnssd(r->raop, r->dnssd);

    /* --- mDNS advertisement (through NsdManager) --- */
    if (dnssd_register_raop(r->dnssd, port) != 0) return fail("register _raop._tcp");
    if (dnssd_register_airplay(r->dnssd, port) != 0) return fail("register _airplay._tcp");

    r->running = true;
    r->supervisor = std::thread(supervisorLoop, r);
    LOGI("AirPlay receiver \"%s\" running on port %u (%dx%d@%d, h265=%d)", name.c_str(), port,
         width, height, fps, h265 ? 1 : 0);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_camiool_airplayfire_AirPlayNative_nativeStop(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Receiver *r = g_receiver;
    if (!r) return;
    destroyReceiver(r, env);
    g_receiver = nullptr;
    LOGI("AirPlay receiver stopped");
}

extern "C" JNIEXPORT void JNICALL
Java_com_camiool_airplayfire_AirPlayNative_nativeSetSurface(JNIEnv *env, jclass, jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_receiver) return;
    ANativeWindow *win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    g_receiver->video.setWindow(win);  // takes its own reference
    if (win) ANativeWindow_release(win);
}

/* Returns: [framesIn, rendered, dropped, avgDecodeUs, width, height, h265, lowLatency,
 *           audioType, audioBufferMs, audioXruns, connections] */
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_camiool_airplayfire_AirPlayNative_nativeGetStats(JNIEnv *env, jclass) {
    jlong vals[12] = {0};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_receiver) {
            auto v = g_receiver->video.stats();
            auto a = g_receiver->audio.stats();
            vals[0] = static_cast<jlong>(v.framesIn);
            vals[1] = static_cast<jlong>(v.framesRendered);
            vals[2] = static_cast<jlong>(v.framesDropped);
            vals[3] = static_cast<jlong>(v.avgDecodeUs);
            vals[4] = v.width;
            vals[5] = v.height;
            vals[6] = v.h265 ? 1 : 0;
            vals[7] = v.lowLatencyMode ? 1 : 0;
            vals[8] = a.codecType;
            vals[9] = a.bufferMs;
            vals[10] = a.xruns;
            vals[11] = g_receiver->openConnections.load();
        }
    }
    jlongArray arr = env->NewLongArray(12);
    env->SetLongArrayRegion(arr, 0, 12, vals);
    return arr;
}
