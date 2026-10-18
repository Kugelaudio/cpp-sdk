#include "ws_connection.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <cctype>
#include <map>

#include "error_mapping.hpp"
#include "kugelaudio/errors.hpp"

namespace kugelaudio::detail {

namespace {

void net_init() {
  static std::once_flag once;
  std::call_once(once, [] { ix::initNetSystem(); });
}

Headers lowercase_headers(const ix::WebSocketHttpHeaders& headers) {
  Headers out;
  for (const auto& [name, value] : headers) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    out[key] = value;
  }
  return out;
}

int ceil_seconds(std::chrono::milliseconds timeout) {
  return std::max(1, static_cast<int>((timeout.count() + 999) / 1000));
}

}  // namespace

WsConnection::WsConnection() : socket_(std::make_unique<ix::WebSocket>()) {}

WsConnection::~WsConnection() { close(); }

std::unique_ptr<WsConnection> WsConnection::open(const WsConnectOptions& options) {
  net_init();
  std::unique_ptr<WsConnection> connection(new WsConnection());
  ix::WebSocket& socket = *connection->socket_;
  socket.setUrl(options.url);
  ix::WebSocketHttpHeaders headers;
  for (const auto& [name, value] : options.headers) headers[name] = value;
  socket.setExtraHeaders(headers);
  socket.disableAutomaticReconnection();
  socket.disablePerMessageDeflate();
  if (options.ping_interval)
    socket.setPingInterval(static_cast<int>(options.ping_interval->count()));

  WsConnection* self = connection.get();
  socket.setOnMessageCallback([self](const ix::WebSocketMessagePtr& message) {
    switch (message->type) {
      case ix::WebSocketMessageType::Message:
        self->on_frame(WsFrame{
            message->binary ? WsFrame::Kind::Binary : WsFrame::Kind::Text, message->str, 0, {}});
        break;
      case ix::WebSocketMessageType::Close:
        self->on_frame(
            WsFrame{WsFrame::Kind::Closed,
                    {},
                    message->closeInfo.code == 0 ? kWsCloseAbnormal : message->closeInfo.code,
                    message->closeInfo.reason});
        break;
      case ix::WebSocketMessageType::Error:
        self->on_frame(
            WsFrame{WsFrame::Kind::Closed, {}, kWsCloseAbnormal, message->errorInfo.reason});
        break;
      default:
        break;  // open, ping, pong, fragment: nothing to deliver
    }
  });

  const ix::WebSocketInitResult result = socket.connect(ceil_seconds(options.handshake_timeout));
  if (!result.success) {
    if (result.http_status > 0 && result.http_status != 101) {
      // No message: each class then carries its actionable default text
      // (the rejection body is not read by the WebSocket library).
      std::rethrow_exception(classify_handshake_rejection(
          result.http_status, lowercase_headers(result.headers), std::string()));
    }
    throw ConnectionError("Could not open a WebSocket to KugelAudio: " +
                          redact_queries(result.errorStr) + ". Check network connectivity.");
  }
  socket.start();
  return connection;
}

void WsConnection::on_frame(WsFrame frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (close_queued_) return;  // nothing arrives after a close
    if (frame.kind == WsFrame::Kind::Closed) close_queued_ = true;
    frames_.push_back(std::move(frame));
  }
  changed_.notify_all();
}

void WsConnection::send_text(const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (close_queued_) {
      const WsFrame& last = frames_.back();
      throw WsClosedError(last.close_code, last.close_reason);
    }
  }
  if (!socket_->send(text, /*binary=*/false).success) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (close_queued_) {
      const WsFrame& last = frames_.back();
      throw WsClosedError(last.close_code, last.close_reason);
    }
    throw WsClosedError(kWsCloseAbnormal, "send failed");
  }
}

void WsConnection::send_binary(const std::string& data) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (close_queued_) {
      const WsFrame& last = frames_.back();
      throw WsClosedError(last.close_code, last.close_reason);
    }
  }
  if (!socket_->sendBinary(data).success) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (close_queued_) {
      const WsFrame& last = frames_.back();
      throw WsClosedError(last.close_code, last.close_reason);
    }
    throw WsClosedError(kWsCloseAbnormal, "send failed");
  }
}

std::optional<WsFrame> WsConnection::receive(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!changed_.wait_for(lock, timeout, [this] { return !frames_.empty(); })) {
    return std::nullopt;
  }
  WsFrame frame = frames_.front();
  // The Closed frame is the last one and stays queued, so the close is sticky.
  if (frame.kind != WsFrame::Kind::Closed) frames_.pop_front();
  return frame;
}

bool WsConnection::is_open() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !close_queued_;
}

std::optional<int> WsConnection::close_code() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!close_queued_) return std::nullopt;
  return frames_.back().close_code;
}

void WsConnection::close(int code, const std::string& reason) {
  // Stops and joins the library's IO thread; never called from that thread.
  socket_->stop(static_cast<uint16_t>(code), reason);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!close_queued_) {
      close_queued_ = true;
      frames_.push_back(WsFrame{WsFrame::Kind::Closed, {}, code, reason});
    }
  }
  changed_.notify_all();
}

}  // namespace kugelaudio::detail
