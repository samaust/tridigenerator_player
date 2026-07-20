#pragma once

#ifndef LOG_TAG
#define LOG_TAG "tridigenerator_player"
#endif

#if defined(ANDROID)
#include <android/log.h>
// Logging functions, in increasing order of priority
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGV(...) do { std::fprintf(stderr, "V/%s: ", LOG_TAG); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define LOGI(...) do { std::fprintf(stderr, "I/%s: ", LOG_TAG); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define LOGW(...) do { std::fprintf(stderr, "W/%s: ", LOG_TAG); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define LOGE(...) do { std::fprintf(stderr, "E/%s: ", LOG_TAG); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif
