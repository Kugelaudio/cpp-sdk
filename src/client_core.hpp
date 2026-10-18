#pragma once

// The state shared by a Client, its resources and its sessions: resolved
// endpoints, credentials, the HTTP transport and the diagnostics reporter.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "diagnostics.hpp"
#include "http_transport.hpp"
#include "kugelaudio/options.hpp"
#include "url.hpp"
#include "ws_connection.hpp"

namespace kugelaudio::detail {

struct ClientCore {
  /// Validates the key, resolves endpoints and builds the reporter.
  /// @param sender Diagnostics transport override (tests).
  explicit ClientCore(const ClientOptions& options, diag::Sender sender = {});

  std::string api_key;  // without region prefix
  std::string api_url;
  std::string tts_url;
  std::chrono::milliseconds timeout;
  std::optional<std::chrono::seconds> keepalive_ping_interval;
  LogHandler log_handler;
  mutable HttpTransport http;  // thread-safe; a share handle locks its caches
  std::shared_ptr<diag::Reporter> diagnostics;

  void log(LogLevel level, const std::string& message) const;
  /// One warning per process when normalization runs without a language.
  void warn_if_no_language(const std::optional<std::string>& language, bool normalize) const;

  /// Auth + SDK headers for every HTTP request and WebSocket handshake.
  HeaderList headers() const;
  /// `<api_url><path>`.
  std::string url(const std::string& path) const;

  /// A JSON request with diagnostics; returns the parsed body (null for an
  /// empty body). Throws the typed error for any status >= 400.
  nlohmann::json request(const std::string& method, const std::string& path,
                         const QueryParams& query = {},
                         const std::optional<nlohmann::json>& body = std::nullopt) const;
  /// A multipart request with diagnostics; returns the parsed body.
  nlohmann::json request_multipart(const std::string& method, const std::string& path,
                                   std::vector<MultipartPart> parts) const;
  /// Perform an HTTP request and throw the typed error for status >= 400.
  /// Transport failures become ConnectionError.
  HttpResponse send(HttpRequest request, const std::string& path) const;

  /// Handshake options for a WebSocket path on the TTS host.
  WsConnectOptions ws_options(
      const std::string& path, const QueryParams& query = {},
      std::optional<std::chrono::seconds> ping_interval = std::nullopt) const;
  /// Same as ws_options, on the API host (speech enhancement).
  WsConnectOptions api_ws_options(const std::string& path, const QueryParams& query = {}) const;
};

/// `kugel.operation` for a REST path (`/v1/voices/12` -> `voices`).
std::optional<std::string> operation_for_path(const std::string& path);

}  // namespace kugelaudio::detail
