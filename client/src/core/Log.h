// Minimal levelled logger (stderr, with wall-clock timestamps).
#pragma once

#include <cstdarg>

namespace arena {

enum class LogLevel { Debug = 0, Info, Warn, Error };

void setLogLevel(LogLevel level);
LogLevel logLevel();

#if defined(__GNUC__) || defined(__clang__)
#define ARENA_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define ARENA_PRINTF(a, b)
#endif

void logf(LogLevel level, const char *fmt, ...) ARENA_PRINTF(2, 3);

} // namespace arena

#define LOG_DEBUG(...) ::arena::logf(::arena::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::arena::logf(::arena::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::arena::logf(::arena::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::arena::logf(::arena::LogLevel::Error, __VA_ARGS__)
