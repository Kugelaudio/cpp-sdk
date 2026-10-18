#include "kugelaudio/enhance.hpp"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>

#include "base64.hpp"
#include "client_core.hpp"
#include "error_mapping.hpp"
#include "json_mapping.hpp"
#include "kugelaudio/errors.hpp"
#include "wav.hpp"

namespace kugelaudio {

using detail::json;
using detail::WsClosedError;
using detail::WsConnection;
using detail::WsFrame;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kTaskNoiseRemoval = "noise_removal";
constexpr const char* kTaskTargetSpeaker = "target_speaker_extraction";
constexpr const char* kEnhancePath = "/v1/audio/enhance";
constexpr const char* kStreamPath = "/v1/audio/enhance/stream";
constexpr const char* kWarmupPath = "/v1/audio/enhance/warmup";
constexpr const char* kNotReady = "The enhancement stream did not become ready in time.";
// The server closes a session after 60 s without a config and at one hour;
// a socket is not reused past these, a little before the server's limits.
constexpr auto kSessionIdle = std::chrono::seconds(55);
constexpr auto kSessionLifetime = std::chrono::seconds(3540);
// What a server without session mode sends on a socket left without a config.
constexpr int kIdleFrameCode = 408;
constexpr auto kSenderJoinTimeout = std::chrono::seconds(5);

void check_model(const std::string& model) {
  if (model.find_first_not_of(" \t\r\n") == std::string::npos) {
    throw ValidationError("model must be a non-empty string, e.g. model=\"clarity-1\".");
  }
}

json stream_config(const EnhanceOptions& options, int sample_rate) {
  check_model(options.model);
  if (sample_rate <= 0) throw ValidationError("sample_rate must be positive.");
  json config{{"type", "config"},
              {"model", options.model},
              {"task", kTaskNoiseRemoval},
              {"sample_rate_hz", sample_rate},
              {"encoding", "pcm_s16le"}};
  if (options.speaker) {
    config["task"] = kTaskTargetSpeaker;
    config["speaker_wav_b64"] = detail::base64_encode(options.speaker->data);
  }
  return config;
}

PcmSource source_of(const AudioStream& audio) {
  auto chunks = std::make_shared<std::vector<Bytes>>(audio.chunks());
  auto next = std::make_shared<std::size_t>(0);
  return [chunks, next]() -> std::optional<Bytes> {
    if (*next >= chunks->size()) return std::nullopt;
    return (*chunks)[(*next)++];
  };
}

[[noreturn]] void throw_closed(int code, const std::string& reason) {
  std::rethrow_exception(detail::classify_ws_close(code, reason));
}

/// Handle a text frame of the stream: throws on `error`, true on `done`.
bool is_done(const std::string& text) {
  const json data = detail::parse_json(text, kStreamPath);
  if (data.value("type", std::string()) == "error") {
    std::rethrow_exception(detail::classify_ws_frame(data));
  }
  return data.value("type", std::string()) == "done";
}

/// Thrown by begin() when a reused session socket turned out stale.
struct StaleSocket {};

/// Send `config` and wait for `ready`; returns it.
json begin(WsConnection& ws, const json& config, std::chrono::milliseconds timeout, bool reused) {
  try {
    ws.send_text(detail::dump_json(config));
  } catch (const WsClosedError& e) {
    if (reused) throw StaleSocket{};
    throw_closed(e.code(), e.reason());
  }
  const auto frame = ws.receive(timeout);
  if (!frame) throw ConnectionError(kNotReady);
  if (frame->kind == WsFrame::Kind::Closed) {
    if (reused) throw StaleSocket{};
    throw_closed(frame->close_code, frame->close_reason);
  }
  if (frame->kind != WsFrame::Kind::Text) {
    throw ConnectionError("Unexpected first message from the enhancement stream.");
  }
  const json data = detail::parse_json(frame->data, kStreamPath);
  const std::string type = data.value("type", std::string());
  if (type == "error") {
    if (reused && data.value("code", 0) == kIdleFrameCode) throw StaleSocket{};
    std::rethrow_exception(detail::classify_ws_frame(data));
  }
  if (type != "ready")
    throw ConnectionError("Unexpected first message from the enhancement stream.");
  return data;
}

struct SenderState {
  std::atomic<bool> stop{false};
  std::exception_ptr failure;
  std::promise<void> done;
};

/// After `ready`: send the audio and `end` from a sender thread, deliver the
/// enhanced chunks until `done`. Leaves the socket open.
void exchange(const std::shared_ptr<WsConnection>& ws, const json& config, PcmSource source,
              const PcmCallback& on_pcm) {
  const std::size_t max_bytes =
      static_cast<std::size_t>(config.at("sample_rate_hz").get<int>()) * 2;
  auto state = std::make_shared<SenderState>();
  std::future<void> finished = state->done.get_future();
  std::thread sender([ws, state, source = std::move(source), max_bytes]() {
    try {
      while (!state->stop.load()) {
        std::optional<Bytes> chunk = source();
        if (!chunk) break;
        for (std::size_t start = 0; start < chunk->size() && !state->stop.load();
             start += max_bytes) {
          const std::size_t end = std::min(chunk->size(), start + max_bytes);
          ws->send_binary(std::string(chunk->begin() + static_cast<std::ptrdiff_t>(start),
                                      chunk->begin() + static_cast<std::ptrdiff_t>(end)));
        }
      }
      if (!state->stop.load()) ws->send_text(R"({"type":"end"})");
    } catch (const WsClosedError&) {
      // KEEP-JUSTIFIED: the receive side reports why the server closed.
    } catch (...) {
      state->failure = std::current_exception();
      ws->close();  // wakes the receiver, which rethrows the failure
    }
    state->done.set_value();
  });
  auto stop_sender = [&]() {
    state->stop = true;
    if (finished.wait_for(kSenderJoinTimeout) == std::future_status::ready) {
      sender.join();
    } else {
      // The sender is blocked inside the caller's source; it owns its state
      // and exits on its next send to the closed socket.
      ws->close();
      sender.detach();
    }
  };
  try {
    while (true) {
      const auto frame = ws->receive(std::chrono::hours(24));
      if (!frame) continue;
      if (frame->kind == WsFrame::Kind::Closed) {
        finished.wait_for(kSenderJoinTimeout);
        if (state->failure) std::rethrow_exception(state->failure);
        throw_closed(frame->close_code, frame->close_reason);
      }
      if (frame->kind == WsFrame::Kind::Binary) {
        on_pcm(Bytes(frame->data.begin(), frame->data.end()));
      } else if (is_done(frame->data)) {
        break;
      }
    }
  } catch (...) {
    stop_sender();
    throw;
  }
  stop_sender();
}

void run_one_shot(const detail::ClientCore& core, const json& config, PcmSource source,
                  const PcmCallback& on_pcm) {
  auto ws = std::shared_ptr<WsConnection>(WsConnection::open(core.api_ws_options(kStreamPath)));
  try {
    begin(*ws, config, core.timeout, false);
    exchange(ws, config, std::move(source), on_pcm);
  } catch (...) {
    ws->close();
    throw;
  }
  ws->close();
}

}  // namespace

// ------------------------------------------------------------ EnhancedAudio

double EnhancedAudio::duration() const noexcept {
  return sample_rate > 0 ? static_cast<double>(audio.size() / 2) / sample_rate : 0.0;
}

Bytes EnhancedAudio::wav() const { return to_wav(audio, sample_rate); }

void EnhancedAudio::save(const std::filesystem::path& path) const {
  write_wav(path, audio, sample_rate);
}

EnhancedAudio EnhancedAudio::from_wav(const Bytes& wav) {
  std::string error;
  const auto info = detail::parse_wav(wav, &error);
  if (!info) {
    throw KugelAudioError("Unexpected enhancement response: not a WAV file (" + error + ").");
  }
  if (info->channels != 1 || info->bits_per_sample != 16 || info->format != 1) {
    throw KugelAudioError("Unexpected enhancement response: expected mono PCM16 WAV.");
  }
  EnhancedAudio out;
  out.sample_rate = info->sample_rate;
  out.audio.assign(wav.begin() + static_cast<std::ptrdiff_t>(info->data_offset),
                   wav.begin() + static_cast<std::ptrdiff_t>(info->data_offset + info->data_size));
  return out;
}

// --------------------------------------------------------- EnhanceResource

EnhanceResource::EnhanceResource(std::shared_ptr<detail::ClientCore> core)
    : core_(std::move(core)) {}

EnhancedAudio EnhanceResource::generate(const AudioFile& audio,
                                        const EnhanceOptions& options) const {
  check_model(options.model);
  if (audio.data.empty()) throw ValidationError("Audio must not be empty.");
  std::vector<detail::MultipartPart> parts;
  parts.push_back(
      {"file", audio.filename, "audio/wav", std::string(audio.data.begin(), audio.data.end())});
  parts.push_back({"model", std::nullopt, "text/plain", options.model});
  parts.push_back({"task", std::nullopt, "text/plain",
                   options.speaker ? kTaskTargetSpeaker : kTaskNoiseRemoval});
  if (options.speaker) {
    parts.push_back({"speaker", options.speaker->filename, "audio/wav",
                     std::string(options.speaker->data.begin(), options.speaker->data.end())});
  }
  detail::HttpRequest request;
  request.method = "POST";
  request.url = core_->url(kEnhancePath);
  request.headers = core_->headers();
  request.timeout = core_->timeout;
  request.multipart = std::move(parts);
  const detail::HttpResponse response = core_->send(std::move(request), kEnhancePath);
  return EnhancedAudio::from_wav(Bytes(response.body.begin(), response.body.end()));
}

void EnhanceResource::prewarm() const {
  detail::HttpRequest request;
  request.method = "POST";
  request.url = core_->url(kWarmupPath);
  request.headers = core_->headers();
  request.timeout = core_->timeout;
  try {
    const detail::HttpResponse response = core_->http.perform(request);
    if (response.status < 200 || response.status >= 300) {
      // KEEP-JUSTIFIED: warming is an optimisation; the first real request
      // gets the same answer (bad key, no model access) as its own typed error.
      core_->log(LogLevel::Warning, "The enhancement warm-up was refused with HTTP " +
                                        std::to_string(response.status) +
                                        "; the first request may wait for a cold start.");
    }
  } catch (const detail::TransportError& e) {
    // KEEP-JUSTIFIED: warming is an optimisation; the first real request
    // connects on its own and raises its own typed error.
    core_->log(LogLevel::Warning, std::string("Could not prewarm the enhancement connection (") +
                                      e.what() + "); the first request will connect instead.");
  }
}

void EnhanceResource::stream(const AudioStream& audio, const EnhanceOptions& options,
                             const PcmCallback& on_pcm) const {
  run_one_shot(*core_, stream_config(options, audio.sample_rate()), source_of(audio), on_pcm);
}

void EnhanceResource::stream(const PcmSource& source, int sample_rate,
                             const EnhanceOptions& options, const PcmCallback& on_pcm) const {
  run_one_shot(*core_, stream_config(options, sample_rate), source, on_pcm);
}

EnhanceSession EnhanceResource::session() const { return EnhanceSession(core_); }

// ----------------------------------------------------------- EnhanceSession

struct EnhanceSession::Impl {
  explicit Impl(std::shared_ptr<detail::ClientCore> core_in) : core(std::move(core_in)) {}

  std::shared_ptr<detail::ClientCore> core;
  std::mutex stream_mutex;  // one stream at a time
  std::mutex mutex;         // guards ws and closed
  std::shared_ptr<WsConnection> ws;
  Clock::time_point opened_at;
  Clock::time_point idle_since;
  bool one_shot = false;
  bool closed = false;

  void drop() {
    std::shared_ptr<WsConnection> old;
    {
      std::lock_guard<std::mutex> lock(mutex);
      old = std::move(ws);
    }
    if (old) old->close();
  }

  bool reusable() const {
    const auto now = Clock::now();
    return ws && ws->is_open() && now - idle_since < kSessionIdle &&
           now - opened_at < kSessionLifetime;
  }

  /// The session socket and whether it was open already; a null socket means
  /// this stream must connect on its own.
  std::pair<std::shared_ptr<WsConnection>, bool> socket() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed) throw KugelAudioError("The enhancement session is closed.");
      if (one_shot) return {nullptr, false};
      if (reusable()) return {ws, true};
    }
    drop();
    std::shared_ptr<WsConnection> fresh;
    try {
      fresh = std::shared_ptr<WsConnection>(
          WsConnection::open(core->api_ws_options(kStreamPath, {{"session", "1"}})));
    } catch (const RateLimitError& e) {
      if (e.retry_after()) throw;
      // The open-session limit (429 without Retry-After): stream on a
      // connection of its own.
      core->log(LogLevel::Info, std::string("The enhancement session was refused (") + e.what() +
                                    "); streaming on a connection of its own.");
      return {nullptr, false};
    } catch (const ValidationError& e) {
      // A server that does not know sessions (400).
      one_shot = true;
      core->log(LogLevel::Info, std::string("The enhancement session was refused (") + e.what() +
                                    "); streaming on a connection of its own.");
      return {nullptr, false};
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (closed) {
      fresh->close();
      throw KugelAudioError("The enhancement session is closed.");
    }
    ws = fresh;
    opened_at = idle_since = Clock::now();
    return {fresh, false};
  }

  void run(const json& config, PcmSource source, const PcmCallback& on_pcm) {
    std::lock_guard<std::mutex> serial(stream_mutex);
    auto [socket_now, reused] = socket();
    if (!socket_now) {
      run_one_shot(*core, config, std::move(source), on_pcm);
      return;
    }
    json ready;
    try {
      try {
        ready = begin(*socket_now, config, core->timeout, reused);
      } catch (const StaleSocket&) {
        drop();
        auto reopened = socket();
        socket_now = reopened.first;
        if (!socket_now) throw ConnectionError("The enhancement session could not be reopened.");
        ready = begin(*socket_now, config, core->timeout, false);
      }
    } catch (const RateLimitError&) {
      // A rate-limit refusal of the config leaves the socket open.
      if (!socket_now->is_open()) drop();
      throw;
    } catch (...) {
      // Refused or failed mid-config: the server's state for this socket is
      // unknown, so it is not reused.
      drop();
      throw;
    }
    if (!ready.contains("request_id")) {
      // The server ignored session=1: it closes after this audio.
      core->log(LogLevel::Info,
                "The enhancement server does not keep sessions open; using one connection per "
                "stream.");
      one_shot = true;
    }
    try {
      exchange(socket_now, config, std::move(source), on_pcm);
    } catch (...) {
      drop();
      throw;
    }
    if (one_shot) {
      drop();
    } else {
      idle_since = Clock::now();
    }
  }
};

EnhanceSession::EnhanceSession(std::shared_ptr<detail::ClientCore> core)
    : impl_(std::make_unique<Impl>(std::move(core))) {}

EnhanceSession::~EnhanceSession() {
  if (impl_) close();
}

EnhanceSession::EnhanceSession(EnhanceSession&&) noexcept = default;

EnhanceSession& EnhanceSession::operator=(EnhanceSession&& other) noexcept {
  if (this != &other) {
    if (impl_) close();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

void EnhanceSession::connect() {
  std::lock_guard<std::mutex> serial(impl_->stream_mutex);
  impl_->socket();
}

void EnhanceSession::stream(const AudioStream& audio, const EnhanceOptions& options,
                            const PcmCallback& on_pcm) {
  impl_->run(stream_config(options, audio.sample_rate()), source_of(audio), on_pcm);
}

void EnhanceSession::stream(const PcmSource& source, int sample_rate, const EnhanceOptions& options,
                            const PcmCallback& on_pcm) {
  impl_->run(stream_config(options, sample_rate), source, on_pcm);
}

void EnhanceSession::close() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed = true;
  }
  impl_->drop();
}

}  // namespace kugelaudio
