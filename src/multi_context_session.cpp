#include "kugelaudio/multi_context_session.hpp"

#include <algorithm>
#include <deque>
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

namespace {

/// What next_frame() produced.
struct Incoming {
  enum class Kind { Frame, Timeout, Closed };
  Kind kind = Kind::Timeout;
  json data;
  int close_code = 0;
  std::string close_reason;
};

std::optional<std::string> context_of(const json& data) {
  if (auto it = data.find("context_id"); it != data.end() && it->is_string()) {
    return it->get<std::string>();
  }
  return std::nullopt;
}

}  // namespace

struct MultiContextSession::Impl {
  Impl(std::shared_ptr<detail::ClientCore> core_in, MultiContextConfig config_in,
       MultiContextCallbacks callbacks_in)
      : core(std::move(core_in)),
        config(std::move(config_in)),
        callbacks(std::move(callbacks_in)),
        turns(core->diagnostics, detail::diag::kOpMultiContext) {
    config.cfg_scale = clamp_cfg_scale(config.cfg_scale);
  }

  std::shared_ptr<detail::ClientCore> core;
  MultiContextConfig config;
  MultiContextCallbacks callbacks;
  detail::TurnTracker turns;

  mutable std::mutex mutex;  // guards the observable results below
  std::shared_ptr<WsConnection> ws;
  bool started = false;
  std::set<std::string> contexts;
  std::map<std::string, SessionUsage> usage;
  std::map<std::string, std::vector<WordTimestamp>> stamps;

  /// Frames of a context that arrived while another call was receiving.
  std::map<std::string, std::deque<json>> pending;
  /// Contexts whose late frames are dropped (barge-in in progress).
  std::set<std::string> dropping;
  bool dropping_all = false;

  // Replay state for rolling-deploy closes (1012 / 1013).
  std::vector<std::pair<std::string, std::optional<std::int64_t>>> context_voices;
  std::map<std::string, std::vector<json>> turn_frames;
  std::set<std::string> turn_audio;
  std::set<std::string> turn_replayed;
  bool recovering = false;

  bool alive() const { return ws && ws->is_open(); }

  void open_socket() {
    ws = std::shared_ptr<WsConnection>(
        WsConnection::open(core->ws_options("/ws/tts/multi", {}, std::chrono::seconds(15))));
  }

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

  void end_turn(const std::string& context_id) {
    turn_frames.erase(context_id);
    turn_audio.erase(context_id);
    turn_replayed.erase(context_id);
  }

  void forget_context(const std::string& context_id) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      contexts.erase(context_id);
    }
    context_voices.erase(
        std::remove_if(context_voices.begin(), context_voices.end(),
                       [&](const auto& entry) { return entry.first == context_id; }),
        context_voices.end());
    end_turn(context_id);
    pending.erase(context_id);
  }

  /// Drop every reference to a dead socket. Buffered audio stays deliverable.
  void reset_dead_session() {
    std::shared_ptr<WsConnection> dead = std::move(ws);
    started = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      contexts.clear();
    }
    context_voices.clear();
    turn_frames.clear();
    turn_audio.clear();
    turn_replayed.clear();
    if (dead) dead->close();
  }

  void remember_voice(const std::string& context_id, std::optional<std::int64_t> voice_id) {
    for (const auto& entry : context_voices) {
      if (entry.first == context_id) return;
    }
    context_voices.emplace_back(context_id, voice_id);
  }

  /// Bookkeeping for a frame of any context, as it arrives.
  void on_arrival(const json& data) {
    const auto context_id = context_of(data);
    if (!context_id) return;
    if (detail::truthy(data, "audio")) turn_audio.insert(*context_id);
    if (detail::has(data, "word_timestamps")) {
      auto parsed = detail::word_timestamps_from_json(data.at("word_timestamps"));
      {
        std::lock_guard<std::mutex> lock(mutex);
        stamps[*context_id] = parsed;
      }
      if (callbacks.on_word_timestamps) callbacks.on_word_timestamps(*context_id, parsed);
    }
    if (detail::truthy(data, "context_closed")) {
      if (auto parsed = detail::usage_from_payload(data)) {
        std::lock_guard<std::mutex> lock(mutex);
        usage[*context_id] = *parsed;
      }
    }
    if (detail::truthy(data, "final") || detail::truthy(data, "context_closed")) {
      end_turn(*context_id);
      turns.finish(*context_id);  // the context's final/done frame: its turn succeeded
    }
  }

  /// The next frame for `context_id` (or a session-level frame when unset).
  /// Frames of other contexts are buffered for them.
  Incoming next_frame(const std::optional<std::string>& context_id,
                      std::chrono::milliseconds timeout) {
    if (context_id) {
      auto it = pending.find(*context_id);
      if (it != pending.end() && !it->second.empty()) {
        Incoming in{Incoming::Kind::Frame, std::move(it->second.front()), 0, {}};
        it->second.pop_front();
        return in;
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      if (!ws)
        return Incoming{Incoming::Kind::Closed, {}, detail::kWsCloseAbnormal, "not connected"};
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      const auto frame = ws->receive(std::max(left, std::chrono::milliseconds(0)));
      if (!frame) return Incoming{};
      if (frame->kind == WsFrame::Kind::Closed) {
        return Incoming{Incoming::Kind::Closed, {}, frame->close_code, frame->close_reason};
      }
      if (frame->kind != WsFrame::Kind::Text) continue;
      json data = detail::parse_json(frame->data, "/ws/tts/multi");
      const auto frame_context = context_of(data);
      if (frame_context && (dropping_all || dropping.count(*frame_context) != 0)) continue;
      on_arrival(data);
      if (frame_context == context_id)
        return Incoming{Incoming::Kind::Frame, std::move(data), 0, {}};
      if (!frame_context && detail::truthy(data, "error")) {
        return Incoming{Incoming::Kind::Frame, std::move(data), 0, {}};
      }
      if (frame_context) pending[*frame_context].push_back(std::move(data));
      // A session-level frame nobody is waiting for (a late ack) is dropped.
    }
  }

  json session_fields(const std::string& context_id, std::optional<std::int64_t> voice_id) const {
    json message{{"text", " "},
                 {"context_id", context_id},
                 {"sample_rate", config.sample_rate},
                 {"normalize", config.normalize},
                 {"word_timestamps", config.word_timestamps}};
    if (config.output_format) message["output_format"] = *config.output_format;
    if (config.model_id) message["model_id"] = *config.model_id;
    if (config.language) message["language"] = *config.language;
    if (config.temperature) message["temperature"] = *config.temperature;
    if (config.speed) message["speed"] = *config.speed;
    if (config.project_id) message["project_id"] = *config.project_id;
    if (config.dictionary_ids) message["dictionary_ids"] = *config.dictionary_ids;
    add_voice(message, voice_id);
    return message;
  }

  void add_voice(json& message, std::optional<std::int64_t> voice_id) const {
    const auto effective = voice_id ? voice_id : config.default_voice_id;
    if (effective) {
      message["voice_settings"] = {{"voice_id", *effective},
                                   {"cfg_scale", config.cfg_scale},
                                   {"max_new_tokens", config.max_new_tokens}};
    }
  }

  /// Send a payload, absorbing a rolling-deploy close. Returns true when a
  /// recovery happened (non-replayable payloads must be resent).
  bool ws_send(const json& payload) {
    if (!ws) open_socket();
    try {
      ws->send_text(detail::dump_json(payload));
      return false;
    } catch (const WsClosedError& e) {
      if (detail::is_restart_close(e.code())) {
        recover_from_restart(context_of(payload), e.code(), e.reason());
        return true;
      }
      reset_dead_session();
      if (detail::is_error_close(e.code())) {
        std::rethrow_exception(detail::classify_ws_close(e.code(), e.reason()));
      }
      throw ConnectionError("KugelAudio WebSocket dropped while sending (close code=" +
                            std::to_string(e.code()) + "). Retry on a fresh session.");
    }
  }

  /// Wait for `context_created` of `context_id`.
  void await_created(const std::string& context_id) {
    while (true) {
      Incoming in = next_frame(context_id, detail::kRecvTimeout);
      if (in.kind == Incoming::Kind::Timeout) {
        throw ConnectionError("Timed out waiting for context_created of '" + context_id + "'.");
      }
      if (in.kind == Incoming::Kind::Closed) {
        reset_dead_session();
        std::rethrow_exception(detail::classify_ws_close(in.close_code, in.close_reason));
      }
      if (detail::truthy(in.data, "error")) turns.raise_frame_error(in.data, context_id);
      if (detail::truthy(in.data, "context_created")) {
        std::lock_guard<std::mutex> lock(mutex);
        contexts.insert(context_id);
        return;
      }
    }
  }

  void create_context(const std::string& context_id, std::optional<std::int64_t> voice_id) {
    if (!ws) connect();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (started && contexts.count(context_id) != 0) return;
    }
    remember_voice(context_id, voice_id);
    json message;
    if (!started) {
      // The first context carries the session config; the server has no
      // session_started message.
      core->warn_if_no_language(config.language, config.normalize);
      message = session_fields(context_id, voice_id);
    } else {
      message = json{{"text", " "}, {"context_id", context_id}};
      if (config.model_id) message["model_id"] = *config.model_id;
      add_voice(message, voice_id);
    }
    if (ws_send(message)) return;  // the restart recovery re-created every context
    await_created(context_id);
    started = true;
  }

  bool recover_from_restart(const std::optional<std::string>& context_id, int code,
                            const std::string& reason) {
    auto error = detail::classify_ws_close(code, reason);
    if (recovering || (context_id && (turn_audio.count(*context_id) != 0 ||
                                      turn_replayed.count(*context_id) != 0))) {
      reset_dead_session();
      if (context_id) {
        if (auto* op = turns.live(*context_id)) op->set_ws_close_code(code);
      }
      std::rethrow_exception(error);
    }
    const auto voices = context_voices;
    const auto frames = turn_frames;
    const auto spoken = turn_audio;
    reset_dead_session();
    recovering = true;
    struct Reset {
      bool& flag;
      ~Reset() { flag = false; }
    } reset{recovering};

    detail::diag::Operation* trigger = context_id ? turns.live(*context_id) : nullptr;
    if (trigger != nullptr) trigger->record_retry();
    const auto delay = detail::restart_delay(error);
    core->log(LogLevel::Info, "Server restarting (" + std::to_string(code) +
                                  "); reconnecting and restoring " + std::to_string(voices.size()) +
                                  " context(s)");
    std::this_thread::sleep_for(delay);
    if (trigger != nullptr) trigger->set_stage(detail::diag::kStageHandshake);
    open_socket();
    // Finish every context setup before a replay can produce audio.
    for (const auto& [cid, voice] : voices) create_context(cid, voice);
    bool replayed = false;
    for (const auto& [cid, voice] : voices) {
      if (spoken.count(cid) != 0) {
        core->log(LogLevel::Warning, "Context " + cid +
                                         " lost the rest of its in-flight turn to a server "
                                         "restart; resend it on the new socket");
        continue;
      }
      auto it = frames.find(cid);
      if (it == frames.end() || it->second.empty()) continue;
      turn_replayed.insert(cid);
      auto* op = turns.live(cid);
      if (cid != context_id.value_or("") && op != nullptr) op->record_retry();
      for (const auto& frame : it->second) {
        turn_frames[cid].push_back(frame);
        ws_send(frame);
      }
      if (op != nullptr) op->expect_audio();
      if (context_id && cid == *context_id) replayed = true;
    }
    if (trigger != nullptr) trigger->expect_audio();
    return replayed;
  }

  void deliver(const std::string& context_id, const json& data, const ChunkCallback& on_chunk) {
    AudioChunk chunk = detail::audio_chunk_from_json(data);
    if (auto* op = turns.live(context_id)) op->record_chunk(chunk.audio.size());
    if (on_chunk) on_chunk(chunk);
  }

  void receive_audio(const std::string& context_id, bool wait_for_close,
                     bool wait_for_chunk_complete,
                     std::optional<std::chrono::milliseconds> chunk_complete_idle,
                     const ChunkCallback& on_chunk) {
    bool received_any_audio = false;
    bool last_chunk_complete = false;
    const auto idle = chunk_complete_idle.value_or(detail::kInterChunkIdle);
    while (true) {
      std::chrono::milliseconds timeout = detail::kRecvTimeout;
      if (!wait_for_close) {
        if (last_chunk_complete) {
          timeout = idle;
        } else if (!wait_for_chunk_complete && received_any_audio) {
          timeout = detail::kPollTimeout;
        }
      }
      Incoming in = next_frame(context_id, timeout);
      if (in.kind == Incoming::Kind::Timeout) return;
      if (in.kind == Incoming::Kind::Closed) {
        if (detail::is_restart_close(in.close_code)) {
          if (last_chunk_complete && !wait_for_close) end_turn(context_id);
          if (!recover_from_restart(context_id, in.close_code, in.close_reason)) return;
          received_any_audio = false;
          last_chunk_complete = false;
          continue;
        }
        reset_dead_session();
        if (last_chunk_complete && !wait_for_close) {
          // Every admitted chunk was delivered; the socket died during the
          // idle linger. The next send() reconnects.
          core->log(LogLevel::Warning,
                    "KugelAudio WebSocket closed after chunk_complete (close code=" +
                        std::to_string(in.close_code) + "); the next send() reconnects");
          return;
        }
        if (auto* op = turns.live(context_id)) op->set_ws_close_code(in.close_code);
        if (detail::is_error_close(in.close_code)) {
          std::rethrow_exception(detail::classify_ws_close(in.close_code, in.close_reason));
        }
        throw ConnectionError("KugelAudio WebSocket dropped mid-stream (close code=" +
                              std::to_string(in.close_code) + "). Retry on a fresh session.");
      }
      const json& data = in.data;
      if (detail::truthy(data, "error")) turns.raise_frame_error(data, context_id);
      if (detail::truthy(data, "generation_started")) {
        received_any_audio = false;
        last_chunk_complete = false;
        continue;
      }
      if (detail::truthy(data, "audio")) {
        received_any_audio = true;
        last_chunk_complete = false;
        deliver(context_id, data, on_chunk);
      }
      if (detail::has(data, "word_timestamps")) continue;  // handled on arrival
      if (detail::truthy(data, "chunk_complete")) {
        if (!wait_for_close && idle.count() <= 0) return;
        last_chunk_complete = true;
        continue;
      }
      if (detail::truthy(data, "final")) {
        if (!wait_for_close) return;  // every audio frame before the flush arrived
        continue;
      }
      if (detail::truthy(data, "context_closed") && wait_for_close) return;
    }
  }

  void prepare_live_socket(const std::string& context_id) {
    if (ws && !ws->is_open()) {
      const int code = ws->close_code().value_or(detail::kWsCloseAbnormal);
      if (detail::is_restart_close(code)) {
        // An idle socket closed for a rolling deploy: restore before sending.
        recover_from_restart(context_id, code, {});
      } else {
        reset_dead_session();
        throw ConnectionError(
            "KugelAudio WebSocket is not connected (close code=" + std::to_string(code) +
            "). Contexts were lost; send again to reconnect.");
      }
    }
    if (!ws) connect();
  }
};

MultiContextSession::MultiContextSession(std::shared_ptr<detail::ClientCore> core,
                                         MultiContextConfig config, MultiContextCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(core), std::move(config), std::move(callbacks))) {}

MultiContextSession::~MultiContextSession() {
  if (impl_) close();
}

MultiContextSession::MultiContextSession(MultiContextSession&&) noexcept = default;

MultiContextSession& MultiContextSession::operator=(MultiContextSession&& other) noexcept {
  if (this != &other) {
    if (impl_) close();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

void MultiContextSession::connect() {
  if (!impl_->alive()) {
    impl_->reset_dead_session();
    impl_->connect();
  }
}

void MultiContextSession::prepare_voice(std::int64_t voice_id) {
  if (!impl_->ws) impl_->connect();
  impl_->ws_send(json{{"text", ""},
                      {"voice_id", voice_id},
                      {"model_id", impl_->config.model_id.value_or(kDefaultModel)}});
}

void MultiContextSession::create_context(const std::string& context_id,
                                         std::optional<std::int64_t> voice_id) {
  impl_->create_context(context_id, voice_id);
}

void MultiContextSession::send(const std::string& context_id, const std::string& text, bool flush,
                               const ChunkCallback& on_chunk,
                               std::optional<std::chrono::milliseconds> chunk_complete_idle) {
  impl_->turns.turn(context_id).set_stage(detail::diag::kStageSendingRequest);
  try {
    impl_->prepare_live_socket(context_id);
    bool known = false;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      known = impl_->started && impl_->contexts.count(context_id) != 0;
    }
    if (!known) impl_->create_context(context_id, std::nullopt);
    const json frame{{"text", text}, {"context_id", context_id}, {"flush", flush}};
    impl_->turn_frames[context_id].push_back(frame);
    impl_->ws_send(frame);
    if (auto* op = impl_->turns.live(context_id)) op->expect_audio();
    impl_->receive_audio(context_id, false, flush, chunk_complete_idle, on_chunk);
  } catch (...) {
    impl_->turns.fail(std::current_exception(), context_id);
    throw;
  }
}

std::vector<AudioChunk> MultiContextSession::send(const std::string& context_id,
                                                  const std::string& text, bool flush) {
  std::vector<AudioChunk> chunks;
  send(context_id, text, flush, [&chunks](const AudioChunk& chunk) { chunks.push_back(chunk); });
  return chunks;
}

void MultiContextSession::flush(const std::string& context_id, const ChunkCallback& on_chunk) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->contexts.count(context_id) == 0) return;
  }
  try {
    const json frame{{"flush", true}, {"context_id", context_id}};
    impl_->turn_frames[context_id].push_back(frame);
    impl_->ws_send(frame);
    if (auto* op = impl_->turns.live(context_id)) op->expect_audio();
    impl_->receive_audio(context_id, false, true, std::nullopt, on_chunk);
  } catch (...) {
    impl_->turns.fail(std::current_exception(), context_id);
    throw;
  }
}

std::vector<AudioChunk> MultiContextSession::flush(const std::string& context_id) {
  std::vector<AudioChunk> chunks;
  flush(context_id, [&chunks](const AudioChunk& chunk) { chunks.push_back(chunk); });
  return chunks;
}

void MultiContextSession::close_context(const std::string& context_id, bool immediate,
                                        const ChunkCallback& on_chunk) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->contexts.count(context_id) == 0) return;
  }
  if (immediate) {
    // Barge-in: a caller-initiated cancellation of this context's turn only.
    impl_->turns.cancel(context_id);
    impl_->pending.erase(context_id);
  } else if (auto* op = impl_->turns.live(context_id)) {
    op->set_stage(detail::diag::kStageFinalizing);
  }
  try {
    json message{{"close_context", true}, {"context_id", context_id}};
    if (immediate) message["immediate"] = true;
    if (impl_->ws_send(message)) impl_->ws_send(message);
    impl_->receive_audio(context_id, true, false, std::nullopt, on_chunk);
  } catch (...) {
    impl_->turns.fail(std::current_exception(), context_id);
    throw;
  }
  impl_->forget_context(context_id);
}

std::vector<AudioChunk> MultiContextSession::close_context(const std::string& context_id,
                                                           bool immediate) {
  std::vector<AudioChunk> chunks;
  close_context(context_id, immediate,
                [&chunks](const AudioChunk& chunk) { chunks.push_back(chunk); });
  return chunks;
}

void MultiContextSession::cancel(std::optional<std::string> context_id) {
  if (!impl_->alive()) return;
  json message{{"cancel", true}};
  if (context_id) {
    message["context_id"] = *context_id;
    impl_->turns.cancel(*context_id);
    impl_->pending.erase(*context_id);
    impl_->end_turn(*context_id);
    impl_->dropping.insert(*context_id);
  } else {
    impl_->turns.cancel_all();
    impl_->pending.clear();
    impl_->turn_frames.clear();
    impl_->turn_audio.clear();
    impl_->turn_replayed.clear();
    impl_->dropping_all = true;
  }
  struct Reset {
    Impl& impl;
    ~Reset() {
      impl.dropping.clear();
      impl.dropping_all = false;
    }
  } reset{*impl_};
  impl_->ws_send(message);
  while (true) {
    Incoming in = impl_->next_frame(std::nullopt, detail::kRecvTimeout);
    if (in.kind == Incoming::Kind::Timeout) return;  // a silent server counts as acknowledged
    if (in.kind == Incoming::Kind::Closed) {
      impl_->reset_dead_session();
      std::rethrow_exception(detail::classify_ws_close(in.close_code, in.close_reason));
    }
    if (detail::truthy(in.data, "error"))
      std::rethrow_exception(detail::classify_ws_frame(in.data));
    if (detail::truthy(in.data, "interrupted")) return;
  }
}

void MultiContextSession::keep_alive(const std::string& context_id) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->started || impl_->contexts.count(context_id) == 0) return;
  }
  impl_->ws_send(json{{"text", ""}, {"context_id", context_id}});
}

GenerationSettings MultiContextSession::update_settings(const GenerationSettings& settings) {
  const json message{{"update_settings", detail::settings_body(settings)}};
  if (!impl_->ws) impl_->connect();
  if (impl_->ws_send(message)) impl_->ws_send(message);
  while (true) {
    Incoming in = impl_->next_frame(std::nullopt, detail::kRecvTimeout);
    if (in.kind == Incoming::Kind::Timeout) {
      throw KugelAudioError("Timed out waiting for settings_updated acknowledgement.");
    }
    if (in.kind == Incoming::Kind::Closed) {
      impl_->reset_dead_session();
      std::rethrow_exception(detail::classify_ws_close(in.close_code, in.close_reason));
    }
    if (detail::truthy(in.data, "error"))
      std::rethrow_exception(detail::classify_ws_frame(in.data));
    if (detail::truthy(in.data, "settings_updated")) {
      // Mirror locally only after the server accepted the change.
      auto& c = impl_->config;
      if (settings.cfg_scale) c.cfg_scale = clamp_cfg_scale(*settings.cfg_scale);
      if (settings.temperature) c.temperature = settings.temperature;
      if (settings.speed) c.speed = settings.speed;
      if (settings.max_new_tokens) c.max_new_tokens = *settings.max_new_tokens;
      if (settings.language) c.language = settings.language;
      if (settings.normalize) c.normalize = *settings.normalize;
      return detail::generation_settings_from_json(in.data.value("settings", json::object()));
    }
  }
}

std::optional<double> MultiContextSession::close() {
  std::optional<double> total;
  if (impl_->alive()) {
    // KEEP-JUSTIFIED: close() is teardown; a socket that dies or a server
    // that stays silent here leaves nothing to report beyond the stats.
    try {
      impl_->ws->send_text(R"({"close_socket":true})");
      while (true) {
        Incoming in = impl_->next_frame(std::nullopt, detail::kRecvTimeout);
        if (in.kind != Incoming::Kind::Frame) break;
        if (detail::truthy(in.data, "session_closed")) {
          if (auto it = in.data.find("total_audio_seconds");
              it != in.data.end() && it->is_number()) {
            total = it->get<double>();
          }
          break;
        }
      }
    } catch (const std::exception& e) {
      impl_->core->log(LogLevel::Debug, std::string("multi-context close: ") + e.what());
    }
  }
  impl_->reset_dead_session();
  // Turns the server never finished were abandoned by this close.
  impl_->turns.cancel_all();
  return total;
}

std::optional<SessionUsage> MultiContextSession::usage_for(const std::string& context_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->usage.find(context_id);
  if (it == impl_->usage.end()) return std::nullopt;
  return it->second;
}

std::map<std::string, SessionUsage> MultiContextSession::context_usage() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->usage;
}

std::vector<WordTimestamp> MultiContextSession::word_timestamps(
    const std::string& context_id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->stamps.find(context_id);
  return it == impl_->stamps.end() ? std::vector<WordTimestamp>{} : it->second;
}

std::set<std::string> MultiContextSession::active_contexts() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->contexts;
}

bool MultiContextSession::is_alive() const { return impl_->alive(); }

}  // namespace kugelaudio
