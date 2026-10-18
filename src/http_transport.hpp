#pragma once

// Blocking HTTP over libcurl. One share handle per transport keeps TCP/TLS
// connections, DNS and TLS sessions warm across requests and threads.

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "url.hpp"

namespace kugelaudio::detail {

/// One part of a multipart/form-data body.
struct MultipartPart {
  std::string name;
  /// Set for file parts.
  std::optional<std::string> filename;
  std::string content_type;
  std::string data;
};

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  HeaderList headers;
  /// Sent as the body with `Content-Type: application/json` when set.
  std::optional<std::string> json_body;
  /// Sent as multipart/form-data when non-empty.
  std::vector<MultipartPart> multipart;
  /// Connect timeout, and the longest time without receiving a byte.
  std::chrono::milliseconds timeout{std::chrono::seconds(60)};
};

struct HttpResponse {
  int status = 0;
  /// Lowercase header names.
  std::map<std::string, std::string> headers;
  std::string body;
};

/// A failure before any HTTP response (DNS, TCP, TLS, timeout).
class TransportError : public std::runtime_error {
 public:
  TransportError(const std::string& message, bool timed_out)
      : std::runtime_error(message), timed_out_(timed_out) {}
  bool timed_out() const noexcept { return timed_out_; }

 private:
  bool timed_out_;
};

class HttpTransport {
 public:
  HttpTransport();
  ~HttpTransport();
  HttpTransport(const HttpTransport&) = delete;
  HttpTransport& operator=(const HttpTransport&) = delete;

  /// Perform a request; every HTTP status is a response.
  /// @throws TransportError when no response arrived.
  HttpResponse perform(const HttpRequest& request);

 private:
  struct Share;
  std::unique_ptr<Share> share_;
};

/// One request without a shared connection cache (diagnostics delivery).
HttpResponse perform_once(const HttpRequest& request);

}  // namespace kugelaudio::detail
