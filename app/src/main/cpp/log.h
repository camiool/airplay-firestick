#pragma once
#include <android/log.h>

#define AP_TAG "AirPlayFire"
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, AP_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, AP_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, AP_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, AP_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, AP_TAG, __VA_ARGS__)
