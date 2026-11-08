#pragma once
// Minimal host-side stand-ins for the parts of ESPHome used by gecko_spa.
// They only exist so the component can be compiled with g++ and driven by
// recorded bus traffic in tests/. They are not used in firmware builds.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace esphome {

template<typename T> using optional = std::optional<T>;

// ---- fake clock -----------------------------------------------------------
namespace stub {
inline uint32_t &now_ms() {
  static uint32_t t = 0;
  return t;
}

// ---- captured log lines ---------------------------------------------------
struct LogLine {
  char level;
  std::string text;
};
inline std::vector<LogLine> &log_lines() {
  static std::vector<LogLine> v;
  return v;
}
inline bool &log_echo() {
  static bool e = false;
  return e;
}
inline void log(char level, const char *tag, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_lines().push_back({level, buf});
  if (log_echo())
    std::printf("[%c][%s] %s\n", level, tag, buf);
}

// ---- in-memory preference store -------------------------------------------
inline std::map<uint32_t, std::vector<uint8_t>> &pref_store() {
  static std::map<uint32_t, std::vector<uint8_t>> m;
  return m;
}
inline int &pref_save_count() {
  static int n = 0;
  return n;
}
}  // namespace stub

inline uint32_t millis() { return stub::now_ms(); }

}  // namespace esphome

#define ESP_LOGE(tag, ...) ::esphome::stub::log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::esphome::stub::log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ::esphome::stub::log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ::esphome::stub::log('D', tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) ::esphome::stub::log('V', tag, __VA_ARGS__)
#define ESP_LOGVV(tag, ...) ((void) 0)
