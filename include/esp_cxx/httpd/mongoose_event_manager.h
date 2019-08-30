#ifndef ESPCXX_HTTPD_EVENT_MANAGER_H_
#define ESPCXX_HTTPD_EVENT_MANAGER_H_

#include <array>
#include <functional>

#include "esp_cxx/event_manager.h"
#include "mongoose.h"

namespace esp_cxx {

class HttpRequest;

class MongooseEventManager : public EventManager {
 public:
  MongooseEventManager();
  ~MongooseEventManager() override;

  // Makes an http connection and asynchronously sends the result to |handler|
  void HttpConnect(std::function<void(HttpRequest)> handler,
                   const std::string& uri,
                   const char* extra_headers = nullptr,
                   const char* post_data = nullptr);

  mg_mgr* underlying_manager() { return &underlying_manager_; }

 private:
  void Poll(int timeout_ms) override;
  void Wake() override;

  mg_mgr underlying_manager_;
};

}  // namespace esp_cxx

#endif  // ESPCXX_HTTPD_EVENT_MANAGER_H_
