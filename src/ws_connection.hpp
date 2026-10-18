#pragma once

// A blocking, pull-style WebSocket over IXWebSocket: the library's own thread
// queues incoming frames; callers receive() them with a timeout and send from
// any thread. One route for every WebSocket of the SDK.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "url.hpp"

namespace ix {
class WebSocket;
}

namespace kugelaudio::detail {

struct WsFrame {
  enum class Kind { Text, Binary, Closed };
  Kind kind = Kind::Text;
  std::string data;
  /// Close code for Kind::Closed (1006 when the connection dropped).
  int close_code = 0;
  std::string close_reason;
};

struct WsConnectOptions {
  std::string url;
  HeaderList headers;
  std::chrono::milliseconds handshake_timeout{std::chrono::seconds(60)};
  /// Ping interval; unset sends no pings.
  std::optional<std::chrono::seconds> ping_interval;
};

/// Thrown by send_*() when the socket is already closed.
class WsClosedError : public std::runtime_error {
 public:
  WsClosedError(int code, std::string reason)
      : std::runtime_error("WebSocket closed"), code_(code), reason_(std::move(reason)) {}
  int code() const noexcept { return code_; }
  const std::string& reason() const noexcept { return reason_; }

 private:
  int code_;
  std::string reason_;
};

class WsConnection {
 public:
  /// Open a connection and complete the handshake.
  /// @throws The typed error of a rejected upgrade (classify_handshake_rejection),
  ///         or ConnectionError when the server could not be reached.
  static std::unique_ptr<WsConnection> open(const WsConnectOptions& options);

  ~WsConnection();
  WsConnection(const WsConnection&) = delete;
  WsConnection& operator=(const WsConnection&) = delete;

  /// Send a text frame. Thread-safe.
  /// @throws WsClosedError when the socket is closed.
  void send_text(const std::string& text);
  /// Send a binary frame. Thread-safe.
  /// @throws WsClosedError when the socket is closed.
  void send_binary(const std::string& data);

  /// The next frame, or std::nullopt after `timeout` without one. After the
  /// socket closed, every call returns the Closed frame at once.
  std::optional<WsFrame> receive(std::chrono::milliseconds timeout);

  /// True until a close was received or sent.
  bool is_open() const;
  /// The close code once the socket closed.
  std::optional<int> close_code() const;
  /// Close (code 1000 by default) and stop the IO thread. Idempotent.
  void close(int code = 1000, const std::string& reason = {});

 private:
  WsConnection();
  void on_frame(WsFrame frame);

  std::unique_ptr<ix::WebSocket> socket_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<WsFrame> frames_;
  std::optional<WsFrame> closed_;
  bool close_queued_ = false;
};

}  // namespace kugelaudio::detail
