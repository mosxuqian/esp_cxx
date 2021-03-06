#include "esp_cxx/logging.h"

#include <cassert>
#include <string>

namespace esp_cxx {

namespace {

static std::function<void(std::string_view)> g_on_log_cb;
vprintf_like_t g_orig_vprintf;
std::string g_device_id;

int SyslogFormatFilter(const char *format, va_list args) {
  int retval = g_orig_vprintf(format, args);
  time_t rawtime;
  time (&rawtime);
  struct tm *ptm = gmtime(&rawtime);
  size_t pos = 0;
  static char buf[512];

  // Format: <22>Oct 12 13:49:10 [devid] ledstrip "
  pos += strftime(&buf[pos], sizeof(buf) - pos, "<22>%FT%TZ ", ptm);
  assert(pos < sizeof(buf));

  pos += snprintf(&buf[pos], sizeof(buf) - pos, "%s ledstrip ", g_device_id.c_str());
  assert(pos < sizeof(buf));

  vsnprintf(&buf[pos], sizeof(buf) - pos, format, args);
  g_on_log_cb(buf);

  return retval;
}

}  // namespace

void SetLogFilter(std::function<void(std::string_view)> on_log, std::string_view device_id) {
  g_on_log_cb = std::move(on_log);
  g_device_id = device_id.empty() ? "unset" : device_id;
  if (!g_orig_vprintf) {
    g_orig_vprintf = esp_log_set_vprintf(&SyslogFormatFilter);
  }
}

}  // namespace esp_cxx
