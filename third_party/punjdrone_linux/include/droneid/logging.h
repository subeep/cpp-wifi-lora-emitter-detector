// droneid/logging.h — tiny leveled logger to stderr. Header-only.
// GPL-3.0-or-later.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace droneid {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

inline LogLevel& log_level() {
  static LogLevel level = LogLevel::Info;
  return level;
}

inline const char* level_str(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}

inline void log_msg(LogLevel l, const char* fmt, ...) {
  if (static_cast<int>(l) < static_cast<int>(log_level())) return;
  static std::mutex mtx;
  std::lock_guard<std::mutex> guard(mtx);
  char ts[16];
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
  std::fprintf(stderr, "%s [%-5s] ", ts, level_str(l));
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "\n");
}

}  // namespace droneid

#define LOG_TRACE(...) ::droneid::log_msg(::droneid::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) ::droneid::log_msg(::droneid::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  ::droneid::log_msg(::droneid::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  ::droneid::log_msg(::droneid::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::droneid::log_msg(::droneid::LogLevel::Error, __VA_ARGS__)
