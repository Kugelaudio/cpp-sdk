#pragma once

// Local HTTP and WebSocket servers (IXWebSocket) for the mock-server tests.

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "kugelaudio/options.hpp"

namespace kugelaudio::testing {

/// A recorded HTTP request.
struct RecordedRequest {
  std::string method;
  std::string uri;
  std::map<std::string, std::string> headers;  // lowercase names
  std::string body;
};

inline std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

inline int random_port() {
  static std::mt19937 engine{std::random_device{}()};
  return std::uniform_int_distribution<int>(20000, 60000)(engine);
}

/// Answers every HTTP request with `handler` and records it.
class MockHttpServer {
 public:
  using Handler = std::function<ix::HttpResponsePtr(const RecordedRequest&)>;

  explicit MockHttpServer(Handler handler) : handler_(std::move(handler)) {
    for (int attempt = 0; attempt < 50; ++attempt) {
      port_ = random_port();
      server_ = std::make_unique<ix::HttpServer>(port_, "127.0.0.1");
      if (server_->listen().first) break;
      server_.reset();
    }
    if (!server_) throw std::runtime_error("no free port for the mock HTTP server");
    server_->setOnConnectionCallback(
        [this](ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState>) {
          RecordedRequest recorded{request->method, request->uri, {}, request->body};
          for (const auto& [name, value] : request->headers) recorded.headers[lower(name)] = value;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(recorded);
          }
          return handler_(recorded);
        });
    server_->start();
  }

  ~MockHttpServer() { server_->stop(); }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::vector<RecordedRequest> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  Handler handler_;
  int port_ = 0;
  std::unique_ptr<ix::HttpServer> server_;
  mutable std::mutex mutex_;
  std::vector<RecordedRequest> requests_;
};

/// A response with a JSON body.
inline ix::HttpResponsePtr json_response(int status, const std::string& body,
                                         ix::WebSocketHttpHeaders headers = {}) {
  headers["Content-Type"] = "application/json";
  return std::make_shared<ix::HttpResponse>(status, status < 400 ? "OK" : "Error",
                                            ix::HttpErrorCode::Ok, headers, body);
}

/// One accepted WebSocket connection of the mock server.
struct WsPeer {
  int index = 0;  // 0 for the first connection
  std::string uri;
  std::map<std::string, std::string> headers;  // lowercase names
  ix::WebSocket* socket = nullptr;

  void send(const std::string& text) const { socket->send(text, false); }
  void send_binary(const std::string& data) const { socket->sendBinary(data); }
  void close(int code, const std::string& reason = {}) const {
    socket->close(static_cast<uint16_t>(code), reason);
  }
};

/// A WebSocket server whose behavior is scripted per message.
class MockWsServer {
 public:
  /// Called for every text (or binary) message a client sends.
  using OnMessage = std::function<void(WsPeer& peer, const std::string& message, bool binary)>;

  explicit MockWsServer(OnMessage on_message) : on_message_(std::move(on_message)) {
    for (int attempt = 0; attempt < 50; ++attempt) {
      port_ = random_port();
      server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
      if (server_->listen().first) break;
      server_.reset();
    }
    if (!server_) throw std::runtime_error("no free port for the mock WebSocket server");
    server_->disablePerMessageDeflate();
    server_->setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState> state,
                                               ix::WebSocket& socket,
                                               const ix::WebSocketMessagePtr& message) {
      std::unique_lock<std::mutex> lock(mutex_);
      auto& peer = peers_[state->getId()];
      if (message->type == ix::WebSocketMessageType::Open) {
        peer = std::make_shared<WsPeer>();
        peer->index = connections_++;
        peer->uri = message->openInfo.uri;
        for (const auto& [name, value] : message->openInfo.headers) {
          peer->headers[lower(name)] = value;
        }
        peer->socket = &socket;
        uris_.push_back(peer->uri);
        changed_.notify_all();
        return;
      }
      if (message->type != ix::WebSocketMessageType::Message || !peer) return;
      received_.push_back(message->str);
      auto current = peer;
      changed_.notify_all();
      lock.unlock();
      on_message_(*current, message->str, message->binary);
    });
    server_->start();
  }

  ~MockWsServer() { server_->stop(); }

  /// Base URL for ClientOptions::api_url (the client switches to ws://).
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::vector<std::string> received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
  }

  std::vector<std::string> uris() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return uris_;
  }

  int connections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_;
  }

  /// Wait until `count` messages were received.
  bool wait_received(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return received_.size() >= count; });
  }

  /// Headers of the first handshake.
  std::map<std::string, std::string> first_headers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, peer] : peers_) {
      if (peer && peer->index == 0) return peer->headers;
    }
    return {};
  }

 private:
  OnMessage on_message_;
  int port_ = 0;
  std::unique_ptr<ix::WebSocketServer> server_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<std::string, std::shared_ptr<WsPeer>> peers_;
  std::vector<std::string> received_;
  std::vector<std::string> uris_;
  int connections_ = 0;
};

#ifndef _WIN32
/// Answers every TCP connection with one canned HTTP response, so a
/// WebSocket upgrade can be refused (IXWebSocket's servers always upgrade).
class RawHttpResponder {
 public:
  explicit RawHttpResponder(std::string response) : response_(std::move(response)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 4) != 0) {
      throw std::runtime_error("cannot listen");
    }
    socklen_t length = sizeof(address);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] {
      while (true) {
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) return;
        if (stop_) {
          ::close(client);
          return;
        }
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
          const auto n = ::recv(client, buffer, sizeof(buffer), 0);
          if (n <= 0) break;
          request.append(buffer, static_cast<std::size_t>(n));
        }
        ::send(client, response_.data(), response_.size(), 0);
        ::close(client);
      }
    });
  }

  ~RawHttpResponder() {
    // Wake the blocking accept() with a connection of our own.
    stop_ = true;
    const int waker = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port_));
    ::connect(waker, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    thread_.join();
    ::close(waker);
    ::close(listener_);
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  std::string response_;
  int listener_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
#endif

/// Options for a client against a local mock (telemetry off, short timeout).
inline ClientOptions mock_options(const std::string& url) {
  ClientOptions options;
  options.api_key = "test-key";
  options.api_url = url;
  options.telemetry = false;
  options.timeout = std::chrono::seconds(5);
  // Set KUGELAUDIO_TEST_LOG=1 to see SDK log lines with a millisecond clock.
  options.log_handler = [](LogLevel, const std::string& message) {
    if (std::getenv("KUGELAUDIO_TEST_LOG") == nullptr) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    std::fprintf(stderr, "[%lld] %s\n", static_cast<long long>(ms), message.c_str());
  };
  return options;
}

}  // namespace kugelaudio::testing
