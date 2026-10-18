#pragma once

/// @file client.hpp
/// kugelaudio::Client, the entry point of the SDK.

#include <memory>
#include <string>

#include "kugelaudio/enhance.hpp"
#include "kugelaudio/options.hpp"
#include "kugelaudio/resources.hpp"
#include "kugelaudio/tts.hpp"

namespace kugelaudio {

/// The KugelAudio API client.
///
/// @code
/// kugelaudio::Client client{std::getenv("KUGELAUDIO_API_KEY")};
/// kugelaudio::GenerateRequest request;
/// request.text = "Hello, world!";
/// request.voice_id = 1071;
/// client.tts().generate(request).save_wav("hello.wav");
/// @endcode
///
/// Thread safety: a Client may be shared between threads; every resource
/// method is thread-safe. Sessions are separate objects with their own rules.
/// Resources and sessions keep the client's internals alive, so a session may
/// outlive the Client that created it.
///
/// The destructor calls close() (RAII).
class Client {
 public:
  /// Create a client from an API key with default options.
  /// @throws ValidationError when the key is empty.
  explicit Client(std::string api_key);
  /// Create a client from options.
  /// @throws ValidationError when the key is empty.
  explicit Client(ClientOptions options);
  ~Client();
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  /// `GET /v1/models`.
  ModelsResource& models();
  /// Voice listing and management.
  VoicesResource& voices();
  /// Pronunciation dictionaries.
  DictionariesResource& dictionaries();
  /// Text to speech.
  TtsResource& tts();
  /// Speech enhancement.
  EnhanceResource& enhance();

  /// Pre-establish the pooled TTS WebSocket (same as `tts().connect(model)`).
  void connect(const std::string& model_id = "kugel-3.5");
  /// True when the pooled TTS WebSocket is open.
  bool is_connected() const;

  /// The resolved API base URL, e.g. "https://api.kugelaudio.com".
  const std::string& api_url() const;
  /// The resolved TTS WebSocket base URL.
  const std::string& tts_url() const;

  /// Close the pooled WebSocket and flush diagnostics (waits at most 1 s).
  /// Idempotent; called by the destructor.
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kugelaudio
