#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace arena {

namespace {
LogLevel g_level = LogLevel::Info;
std::mutex g_mutex;
const char *const kNames[] = {"debug", "info", "warn", "error"};
} // namespace

void setLogLevel(LogLevel level) { g_level = level; }
LogLevel logLevel() { return g_level; }

void logf(LogLevel level, const char *fmt, ...) {
    if (level < g_level) {
        return;
    }
#ifdef __ANDROID__
    {
        // logcat has its own time stamps ("adb logcat -s PandaArena").
        static const int kPriority[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        va_list args;
        va_start(args, fmt);
        __android_log_vprint(kPriority[static_cast<int>(level)], "PandaArena", fmt, args);
        va_end(args);
        return;
    }
#endif
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%02d:%02d:%02d.%03d [%s] ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                 kNames[static_cast<int>(level)]);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace arena
