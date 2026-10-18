#include "kugelaudio/streaming_session.hpp"

#include <mutex>
#include <thread>

#include "client_core.hpp"
#include "error_mapping.hpp"
#include "frames.hpp"
#include "json_mapping.hpp"
#include "kugelaudio/errors.hpp"
#include "session_common.hpp"

namespace kugelaudio {

using detail::json;
using detail::WsClosedError;
using detail::WsConnection;
using detail::WsFrame;

struct StreamingSession::Impl {
  Impl(std::shared_ptr<detail::ClientCore> core_in, StreamConfig config_in,
       StreamingSessionCallbacks callbacks_in)
      : core(std::move(core_in)),
        config(std::move(config_in)),
        callbacks(std::move(callbacks_in)),
        turns(core->diagnostics, detail::diag::kOpStreamSession) {}

  std::shared_ptr<detail::ClientCore> core;
  StreamConfig config;
  StreamingSessionCallbacks callbacks;
  detail::TurnTracker turns;

  /// One receiver at a time; cancel() takes it after signalling.
  std::mutex io_mutex;
  /// Guards `ws`, the cancel flags and the last_* results.
  mutable std::mutex mutex;
  std::shared_ptr<WsConnection> ws;
  bool cancel_requested = false;
  bool interrupted_seen = false;

  bool config_sent = false;
  bool session_ended = false;
  SessionStats session_stats;
  std::vector<WordTimestamp> last_stamps;
  std::optional<SessionUsage> last_usage;
  std::optional<TurnFinal> last_final;

  // Replay state for rolling-deploy closes (1012 / 1013).
  std::vector<json> turn_frames;
  bool turn_audio = false;
  bool turn_replayed = false;

  std::shared_ptr<WsConnection> socket() const {
    std::lock_guard<std::mutex> lock(mutex);
    return ws;
  }

  bool cancelling() const {
    std::lock_guard<std::mutex> lock(mutex);
    return cancel_requested;
  }

  void reset_turn() {
    turn_frames.clear();
    turn_audio = false;
    turn_replayed = false;
  }

  /// Drop a dead socket so the next send() reconnects.
  void forget_socket() {
    std::shared_ptr<WsConnection> dead;
    {
      std::lock_guard<std::mutex> lock(mutex);
      dead = std::move(ws);
    }
    config_sent = false;
    if (dead) dead->close();
  }

  void open_socket() {
    auto fresh = std::shared_ptr<WsConnection>(
        WsConnection::open(core->ws_options("/ws/tts/stream", {}, std::chrono::seconds(15))));
    std::lock_guard<std::mutex> lock(mutex);
    ws = std::move(fresh);
  }

  /// Open the socket as an operation of its own.
  void connect() {
    auto op = turns.connect_operation();
    try {
      open_socket();
    } catch (...) {
      op.fail(std::current_exception());
      throw;
    }
    op.succeed();
  }

  void ensure_connected() {
    if (!socket()) connect();
  }

  [[noreturn]] void throw_close(int code, const std::string& reason) {
    if (auto* op = turns.live()) op->set_ws_close_code(code);
    std::rethrow_exception(detail::classify_ws_close(code, reason));
  }

  /// Send one frame of the current turn, remembering it for a replay.
  void send_frame(const json& frame) {
    turn_frames.push_back(frame);
    json message = frame;
    if (!config_sent) {
      core->warn_if_no_language(config.language, config.normalize);
      const json config_fields = detail::stream_config_body(config);
      for (const auto& [key, value] : config_fields.items()) message[key] = value;
      config_sent = true;
    }
    auto ws_now = socket();
    if (!ws_now) {
      open_socket();
      ws_now = socket();
    }
    try {
      ws_now->send_text(detail::dump_json(message));
    } catch (const WsClosedError& e) {
      if (!detail::is_restart_close(e.code())) {
        forget_socket();
        throw_close(e.code(), e.reason());
      }
      recover_from_restart(e.code(), e.reason());
    }
  }

  /// Absorb a rolling-deploy close: replay the silent turn on a new socket.
  /// Returns true when frames were replayed (keep receiving), false when
  /// there was nothing to replay.
  bool recover_from_restart(int code, const std::string& reason) {
    auto error = detail::classify_ws_close(code, reason);
    forget_socket();
    if (turn_audio || turn_replayed) {
      reset_turn();
      if (auto* op = turns.live()) op->set_ws_close_code(code);
      std::rethrow_exception(error);
    }
    std::vector<json> frames = std::move(turn_frames);
    turn_frames.clear();
    if (frames.empty()) return false;
    const auto delay = detail::restart_delay(error);
    core->log(LogLevel::Info, "Server restarting (" + std::to_string(code) +
                                  "); reconnecting and replaying " + std::to_string(frames.size()) +
                                  " frame(s)");
    auto* turn = turns.live();
    if (turn != nullptr) turn->record_retry();
    std::this_thread::sleep_for(delay);
    if (turn != nullptr) turn->set_stage(detail::diag::kStageHandshake);
    open_socket();
    core->log(LogLevel::Debug, "Reconnected after the restart; replaying the turn");
    turn_replayed = true;
    for (const auto& frame : frames) send_frame(frame);
    if (turn != nullptr) turn->expect_audio();
    return true;
  }

  void handle_stamps(const json& data) {
    auto stamps = detail::word_timestamps_from_json(data.at("word_timestamps"));
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_stamps = stamps;
    }
    if (callbacks.on_word_timestamps) callbacks.on_word_timestamps(stamps);
  }

  void handle_warning(const json& data) {
    const std::string text = data.value("warning", std::string());
    core->log(LogLevel::Warning, "KugelAudio: " + text);
    if (callbacks.on_warning) callbacks.on_warning(text);
  }

  void capture_session_stats(const json& data) {
    turns.finish();
    SessionStats stats = detail::session_stats_from_json(data);
    std::lock_guard<std::mutex> lock(mutex);
    session_stats = stats;
    if (stats.usage) last_usage = stats.usage;
  }

  void deliver(const AudioChunk& chunk, const ChunkCallback& on_chunk) {
    turn_audio = true;
    if (auto* op = turns.live()) op->record_chunk(chunk.audio.size());
    if (on_chunk) on_chunk(chunk);
  }

  /// Receive until `chunk_complete` + idle, `session_closed` or a timeout.
  void receive_until_idle(bool wait_for_first, const ChunkCallback& on_chunk) {
    bool generation_active = false;
    bool last_chunk_complete = false;
    bool received_any = false;
    while (true) {
      const auto timeout = last_chunk_complete                     ? detail::kInterChunkIdle
                           : (generation_active || wait_for_first) ? detail::kRecvTimeout
                                                                   : detail::kPollTimeout;
      auto ws_now = socket();
      if (!ws_now) return;
      const auto frame = ws_now->receive(timeout);
      if (!frame) {
        if (last_chunk_complete) return;
        if (generation_active) {
          core->log(LogLevel::Warning,
                    "Timed out waiting for chunk_complete after 30s (generation was active; "
                    "possible server issue)");
        } else if (wait_for_first && !received_any) {
          core->log(LogLevel::Warning, "No reply from KugelAudio within 30s after a flush.");
        }
        return;
      }
      if (frame->kind == WsFrame::Kind::Closed) {
        if (detail::is_restart_close(frame->close_code)) {
          if (!recover_from_restart(frame->close_code, frame->close_reason)) return;
          generation_active = false;
          last_chunk_complete = false;
          continue;
        }
        forget_socket();
        if (frame->close_code == detail::kWsCloseNormal || frame->close_code == 1001) return;
        throw_close(frame->close_code, frame->close_reason);
      }
      if (frame->kind != WsFrame::Kind::Text) continue;
      received_any = true;
      const json data = detail::parse_json(frame->data, "/ws/tts/stream");
      const bool discard = cancelling();
      if (detail::truthy(data, "error")) {
        if (discard) continue;
        turns.raise_frame_error(data);
      }
      if (detail::truthy(data, "interrupted")) {
        std::lock_guard<std::mutex> lock(mutex);
        interrupted_seen = true;
        return;
      }
      if (detail::truthy(data, "warning")) handle_warning(data);
      if (detail::truthy(data, "generation_started")) {
        generation_active = true;
        last_chunk_complete = false;
        continue;
      }
      if (detail::truthy(data, "audio")) {
        generation_active = true;
        last_chunk_complete = false;
        if (!discard) deliver(detail::audio_chunk_from_json(data), on_chunk);
      }
      if (detail::has(data, "word_timestamps")) {
        if (!discard) handle_stamps(data);
        continue;
      }
      if (detail::truthy(data, "final")) {
        // End of audio for the turn; `session_closed` (with usage) follows.
        {
          std::lock_guard<std::mutex> lock(mutex);
          last_final = detail::turn_final_from_json(data);
        }
        turns.finish();
        continue;
      }
      if (detail::truthy(data, "session_closed")) {
        capture_session_stats(data);
        reset_turn();
        return;
      }
      if (detail::truthy(data, "chunk_complete")) last_chunk_complete = true;
    }
  }

  /// Send `close` and receive until `session_closed`; replays on a restart.
  void drain_frames(const ChunkCallback& on_chunk) {
    auto ws_now = socket();
    if (!ws_now || session_ended) return;
    session_ended = true;
    std::optional<std::pair<int, std::string>> closed;
    try {
      ws_now->send_text(R"({"close":true})");
      while (true) {
        const auto frame = ws_now->receive(detail::kRecvTimeout);
        if (!frame) {
          forget_socket();
          throw ConnectionError("Timed out waiting for the end of the turn (session_closed).");
        }
        if (frame->kind == WsFrame::Kind::Closed) {
          closed.emplace(frame->close_code, frame->close_reason);
          break;
        }
        if (frame->kind != WsFrame::Kind::Text) continue;
        const json data = detail::parse_json(frame->data, "/ws/tts/stream");
        if (detail::truthy(data, "error")) turns.raise_frame_error(data);
        if (detail::truthy(data, "warning")) handle_warning(data);
        if (detail::truthy(data, "audio")) deliver(detail::audio_chunk_from_json(data), on_chunk);
        if (detail::has(data, "word_timestamps")) handle_stamps(data);
        if (detail::truthy(data, "final")) {
          std::lock_guard<std::mutex> lock(mutex);
          last_final = detail::turn_final_from_json(data);
        }
        if (detail::truthy(data, "session_closed")) {
          capture_session_stats(data);
          reset_turn();
          return;
        }
      }
    } catch (const WsClosedError& e) {
      closed.emplace(e.code(), e.reason());
    }
    forget_socket();
    if (detail::is_restart_close(closed->first)) {
      // The close handshake never completed: replay the unspoken turn on a
      // fresh socket and drain again; throws when audio already went out.
      session_ended = false;
      if (recover_from_restart(closed->first, closed->second)) drain_frames(on_chunk);
      return;
    }
    throw_close(closed->first, closed->second);
  }

  SessionStats end_session() {
    if (session_ended) {
      std::lock_guard<std::mutex> lock(mutex);
      SessionStats stats = session_stats;
      session_ended = false;
      session_stats = {};
      config_sent = false;
      last_stamps.clear();
      return stats;
    }
    SessionStats stats;
    auto ws_now = socket();
    if (!ws_now) return stats;
    try {
      ws_now->send_text(R"({"close":true})");
      while (true) {
        const auto frame = ws_now->receive(detail::kRecvTimeout);
        if (!frame || frame->kind == WsFrame::Kind::Closed) {
          // The socket died or stayed silent (a restart close lands here too):
          // end_session() discards in-flight audio anyway, so tear down and let
          // the next send() reconnect.
          forget_socket();
          break;
        }
        if (frame->kind != WsFrame::Kind::Text) continue;
        const json data = detail::parse_json(frame->data, "/ws/tts/stream");
        if (detail::truthy(data, "error")) turns.raise_frame_error(data);
        if (detail::truthy(data, "session_closed")) {
          capture_session_stats(data);
          std::lock_guard<std::mutex> lock(mutex);
          stats = session_stats;
          break;
        }
      }
    } catch (const WsClosedError&) {
      forget_socket();
    }
    config_sent = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_stamps.clear();
    }
    reset_turn();
    return stats;
  }
};

StreamingSession::StreamingSession(std::shared_ptr<detail::ClientCore> core, StreamConfig config,
                                   StreamingSessionCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(core), std::move(config), std::move(callbacks))) {}

StreamingSession::~StreamingSession() {
  if (!impl_) return;
  // KEEP-JUSTIFIED: a destructor must not throw; close() errors here mean the
  // socket is already gone, which is the state we want.
  try {
    close();
  } catch (...) {
  }
}

StreamingSession::StreamingSession(StreamingSession&&) noexcept = default;

StreamingSession& StreamingSession::operator=(StreamingSession&& other) noexcept {
  if (this != &other) {
    // KEEP-JUSTIFIED: see the destructor.
    try {
      if (impl_) close();
    } catch (...) {
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

void StreamingSession::connect() {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  impl_->connect();
}

void StreamingSession::send(const std::string& text, bool flush, const ChunkCallback& on_chunk) {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  impl_->ensure_connected();
  auto& op = impl_->turns.turn();
  try {
    op.set_stage(detail::diag::kStageSendingRequest);
    impl_->send_frame(json{{"text", text}, {"flush", flush}});
    // With flush the server is committed to audio, so wait for the first
    // frame; otherwise a partial token may not start synthesis at all.
    if (auto* live = impl_->turns.live()) live->expect_audio();
    impl_->receive_until_idle(flush, on_chunk);
  } catch (...) {
    impl_->turns.fail(std::current_exception());
    throw;
  }
}

std::vector<AudioChunk> StreamingSession::send(const std::string& text, bool flush) {
  std::vector<AudioChunk> chunks;
  send(text, flush, [&chunks](const AudioChunk& chunk) { chunks.push_back(chunk); });
  return chunks;
}

void StreamingSession::flush(const ChunkCallback& on_chunk) {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  if (!impl_->socket()) return;
  auto& op = impl_->turns.turn();
  try {
    op.set_stage(detail::diag::kStageSendingRequest);
    impl_->send_frame(json{{"flush", true}});
    if (auto* live = impl_->turns.live()) live->expect_audio();
    impl_->receive_until_idle(true, on_chunk);
  } catch (...) {
    impl_->turns.fail(std::current_exception());
    throw;
  }
}

std::vector<AudioChunk> StreamingSession::flush() {
  std::vector<AudioChunk> chunks;
  flush([&chunks](const AudioChunk& chunk) { chunks.push_back(chunk); });
  return chunks;
}

void StreamingSession::drain(const ChunkCallback& on_chunk) {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  if (auto* op = impl_->turns.live()) op->set_stage(detail::diag::kStageFinalizing);
  try {
    impl_->drain_frames(on_chunk);
  } catch (...) {
    impl_->turns.fail(std::current_exception());
    throw;
  }
}

std::vector<AudioChunk> StreamingSession::drain() {
  std::vector<AudioChunk> chunks;
  drain([&chunks](const AudioChunk& chunk) { chunks.push_back(chunk); });
  return chunks;
}

SessionStats StreamingSession::end_session() {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  return impl_->end_session();
}

void StreamingSession::cancel() {
  std::shared_ptr<WsConnection> ws_now;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ws) return;
    impl_->cancel_requested = true;
    impl_->interrupted_seen = false;
    ws_now = impl_->ws;
  }
  bool socket_alive = true;
  try {
    ws_now->send_text(R"({"cancel":true})");
  } catch (const WsClosedError&) {
    socket_alive = false;
  }
  // Wait for a send()/flush() on another thread to see `interrupted` and return.
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  // Barge-in is a caller-initiated cancellation of the turn, not a failure.
  impl_->turns.cancel();
  impl_->session_ended = false;
  impl_->reset_turn();
  bool acknowledged = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    acknowledged = impl_->interrupted_seen;
    impl_->session_stats = {};
  }
  if (!socket_alive) {
    if (impl_->socket() == ws_now) impl_->forget_socket();
  } else if (!acknowledged && impl_->socket() == ws_now) {
    // Discard stale audio of the cancelled turn until the ack; a silent
    // server for the whole receive timeout counts as acknowledged.
    while (true) {
      const auto frame = ws_now->receive(detail::kRecvTimeout);
      if (!frame) break;
      if (frame->kind == WsFrame::Kind::Closed) {
        impl_->forget_socket();
        break;
      }
      if (frame->kind != WsFrame::Kind::Text) continue;
      const json data = detail::parse_json(frame->data, "/ws/tts/stream");
      if (detail::truthy(data, "interrupted")) break;
    }
  }
  impl_->config_sent = false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->last_stamps.clear();
  impl_->cancel_requested = false;
  impl_->interrupted_seen = false;
}

void StreamingSession::update_config(StreamConfig config) {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  impl_->config = std::move(config);
  impl_->config_sent = false;
  impl_->reset_turn();
}

GenerationSettings StreamingSession::update_settings(const GenerationSettings& settings) {
  const std::string body =
      detail::dump_json(json{{"update_settings", detail::settings_body(settings)}});
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  impl_->ensure_connected();
  auto ws_now = impl_->socket();
  try {
    ws_now->send_text(body);
  } catch (const WsClosedError& e) {
    impl_->forget_socket();
    std::rethrow_exception(detail::classify_ws_close(e.code(), e.reason()));
  }
  while (true) {
    const auto frame = ws_now->receive(detail::kRecvTimeout);
    if (!frame) throw KugelAudioError("Timed out waiting for settings_updated acknowledgement.");
    if (frame->kind == WsFrame::Kind::Closed) {
      impl_->forget_socket();
      std::rethrow_exception(detail::classify_ws_close(frame->close_code, frame->close_reason));
    }
    if (frame->kind != WsFrame::Kind::Text) continue;
    const json data = detail::parse_json(frame->data, "/ws/tts/stream");
    if (detail::truthy(data, "error")) std::rethrow_exception(detail::classify_ws_frame(data));
    if (detail::truthy(data, "settings_updated")) {
      // Mirror locally only after the server accepted the change.
      if (settings.cfg_scale) impl_->config.cfg_scale = *settings.cfg_scale;
      if (settings.temperature) impl_->config.temperature = settings.temperature;
      if (settings.speed) impl_->config.speed = *settings.speed;
      if (settings.max_new_tokens) impl_->config.max_new_tokens = *settings.max_new_tokens;
      if (settings.language) impl_->config.language = settings.language;
      if (settings.normalize) impl_->config.normalize = *settings.normalize;
      return detail::generation_settings_from_json(data.value("settings", json::object()));
    }
  }
}

SessionStats StreamingSession::close() {
  std::lock_guard<std::mutex> io(impl_->io_mutex);
  if (auto* op = impl_->turns.live()) op->set_stage(detail::diag::kStageFinalizing);
  SessionStats stats;
  try {
    stats = impl_->end_session();
  } catch (...) {
    impl_->turns.fail(std::current_exception());
    impl_->forget_socket();
    throw;
  }
  impl_->forget_socket();
  // A turn the server never closed was abandoned by this close.
  impl_->turns.cancel_all();
  return stats;
}

std::vector<WordTimestamp> StreamingSession::last_word_timestamps() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_stamps;
}

std::optional<SessionUsage> StreamingSession::last_usage() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_usage;
}

std::optional<TurnFinal> StreamingSession::last_final() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_final;
}

bool StreamingSession::is_connected() const {
  auto ws_now = impl_->socket();
  return ws_now && ws_now->is_open();
}

}  // namespace kugelaudio
