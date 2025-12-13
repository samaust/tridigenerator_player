#pragma once

#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FrameLoader", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FrameLoader", __VA_ARGS__)
