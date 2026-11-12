#pragma once
// Fake logger. It checks the format string against its arguments, as
// the real logger does, and counts warnings.
#include <cstdarg>
#include <cstdio>

namespace esphome {
namespace fake {
inline int &warn_count() {
  static int count = 0;
  return count;
}
inline bool &log_echo() {
  static bool echo = false;
  return echo;
}
__attribute__((format(printf, 3, 4))) inline void log(char level, const char *tag, const char *format, ...) {
  if (level == 'W')
    warn_count()++;
  if (!log_echo())
    return;
  va_list args;
  va_start(args, format);
  printf("  [%c][%s] ", level, tag);
  vprintf(format, args);
  printf("\n");
  va_end(args);
}
}  // namespace fake
}  // namespace esphome

#define ESP_LOGD(tag, ...) ::esphome::fake::log('D', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::esphome::fake::log('W', tag, __VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) ::esphome::fake::log('C', tag, __VA_ARGS__)
#define YESNO(b) ((b) ? "YES" : "NO")
#define LOG_UPDATE_INTERVAL(this_) ::esphome::fake::log('C', TAG, "  Update interval: %u ms", static_cast<unsigned>((this_)->get_update_interval()))
