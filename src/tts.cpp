#include "kugelaudio/tts.hpp"

#include <mutex>

#include "client_core.hpp"
#include "error_mapping.hpp"
#include "frames.hpp"
#include "json_mapping.hpp"
#include "kugelaudio/audio.hpp"
#include "kugelaudio/errors.hpp"

namespace kugelaudio {

namespace detail {

/// The pooled `/ws/tts` connection of one TtsResource.
struct PooledTtsSocket {
  /// Serializes streams, settings updates and connects on the pooled socket.
  std::mutex stream_mutex;
  /// Guards the fields below; never held across a blocking receive.
  std::mutex mutex;
  std::shared_ptr<WsConnection> connection;
  std::string url;
  /// Socket of the stream running now (pooled or its own), for cancel().
  std::shared_ptr<WsConnection> active;
  bool cancel_sent = false;
  /// Defaults set with update_settings(), applied by the client to every
  /// pooled stream (they survive a reconnect, unlike the server's copy).
  GenerationSettings sticky;
};

}  // namespace detail

namespace {

using detail::json;
using detail::WsClosedError;
using detail::WsConnection;
using detail::WsFrame;

std::string ws_tts_url(const detail::ClientCore& core, const std::string& model_id) {
  return core.ws_options("/ws/tts", {{"model_id", model_id}}).url;
}

/// The pooled socket for `model_id`, opening (or replacing) it when needed.
/// Caller holds `pool.stream_mutex`.
std::shared_ptr<WsConnection> pooled(detail::PooledTtsSocket& pool, const detail::ClientCore& core,
                                     const std::string& model_id, detail::diag::Operation* op) {
  const std::string url = ws_tts_url(core, model_id);
  std::shared_ptr<WsConnection> stale;
  {
    std::lock_guard<std::mutex> lock(pool.mutex);
    if (pool.connection && pool.url == url && pool.connection->is_open()) return pool.connection;
    stale = std::move(pool.connection);
    pool.url.clear();
  }
  if (stale) stale->close();
  if (op != nullptr) op->set_stage(detail::diag::kStageHandshake);
  auto fresh = std::shared_ptr<WsConnection>(WsConnection::open(
      core.ws_options("/ws/tts", {{"model_id", model_id}}, core.keepalive_ping_interval)));
  std::lock_guard<std::mutex> lock(pool.mutex);
  pool.connection = fresh;
  pool.url = url;
  return fresh;
}

void drop_pooled(detail::PooledTtsSocket& pool, const std::shared_ptr<WsConnection>& socket) {
  std::shared_ptr<WsConnection> dropped;
  {
    std::lock_guard<std::mutex> lock(pool.mutex);
    if (pool.connection == socket) {
      dropped = std::move(pool.connection);
      pool.url.clear();
    }
  }
  if (dropped) dropped->close();
}

int parse_rate(const std::string& text, const char* source) {
  try {
    std::size_t used = 0;
    const int rate = std::stoi(text, &used);
    if (used == text.size() && rate > 0) return rate;
  } catch (const std::exception&) {
    // KEEP-JUSTIFIED: rethrown below as a typed error naming the source.
  }
  throw KugelAudioError(std::string("Unexpected sample rate in ") + source + ": '" + text + "'.");
}

/// `request` with the pooled socket's sticky defaults filled in. GenerateRequest
/// cannot leave cfg_scale, speed, max_new_tokens or normalize unset, so a field
/// still at its default value takes the sticky value; an explicit other value
/// wins. Without this the request always overrode the server's sticky copy.
GenerateRequest with_sticky(GenerateRequest request, const GenerationSettings& sticky) {
  const GenerateRequest defaults;
  if (sticky.cfg_scale && request.cfg_scale == defaults.cfg_scale) {
    request.cfg_scale = *sticky.cfg_scale;
  }
  if (sticky.speed && request.speed == defaults.speed) request.speed = *sticky.speed;
  if (sticky.max_new_tokens && request.max_new_tokens == defaults.max_new_tokens) {
    request.max_new_tokens = *sticky.max_new_tokens;
  }
  if (sticky.normalize && request.normalize == defaults.normalize) {
    request.normalize = *sticky.normalize;
  }
  if (sticky.temperature && !request.temperature) request.temperature = sticky.temperature;
  if (sticky.language && !request.language) request.language = sticky.language;
  return request;
}

[[noreturn]] void throw_closed(int code, const std::string& reason, detail::diag::Operation& op) {
  op.set_ws_close_code(code);
  std::rethrow_exception(detail::classify_ws_close(code, reason));
}

/// Receive the reply to `body` on `socket` until `final` or `interrupted`.
void run_stream(detail::PooledTtsSocket& pool, const detail::ClientCore& core,
                const std::shared_ptr<WsConnection>& socket, bool pooled_socket,
                const std::string& body, const StreamCallbacks& callbacks,
                const StreamOptions& options, detail::diag::Operation& op, AudioResponse& result) {
  {
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.active = socket;
    pool.cancel_sent = false;
  }
  auto finish = [&pool]() {
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.active.reset();
    return pool.cancel_sent;
  };
  auto drop = [&]() {
    finish();
    if (pooled_socket) drop_pooled(pool, socket);
  };

  op.set_stage(detail::diag::kStageSendingRequest);
  try {
    socket->send_text(body);
  } catch (const WsClosedError& e) {
    drop();
    throw_closed(e.code(), e.reason(), op);
  }
  op.expect_audio();

  bool first_chunk = true;
  std::int64_t received_bytes = 0;
  while (true) {
    const auto frame = socket->receive(core.timeout);
    if (!frame) {
      drop();
      throw ConnectionError("Timed out waiting for audio from KugelAudio.");
    }
    if (frame->kind == WsFrame::Kind::Closed) {
      drop();
      throw_closed(frame->close_code, frame->close_reason, op);
    }
    if (frame->kind != WsFrame::Kind::Text) continue;
    json data;
    try {
      data = detail::parse_json(frame->data, "/ws/tts");
    } catch (...) {
      drop();
      throw;
    }
    if (detail::truthy(data, "error")) {
      finish();
      auto error = detail::classify_ws_frame(data);
      op.mark_server_error(error);
      std::rethrow_exception(error);
    }
    if (detail::truthy(data, "final")) {
      // `total_samples` and `dur_ms` count the native 24 kHz audio before
      // resampling and the speed change; `samples` and `duration_ms` are
      // derived from the delivered bytes below instead.
      result.generation_ms = data.value("gen_ms", 0.0);
      result.rtf = data.value("rtf", 0.0);
      result.usage = detail::usage_from_payload(data);
      break;
    }
    if (detail::truthy(data, "interrupted")) {
      result.interrupted = true;
      break;
    }
    if (detail::truthy(data, "audio")) {
      AudioChunk chunk = detail::audio_chunk_from_json(data);
      op.record_chunk(chunk.audio.size());
      received_bytes += static_cast<std::int64_t>(chunk.audio.size());
      if (first_chunk) {
        result.sample_rate = chunk.sample_rate;
        result.encoding = chunk.encoding;
        first_chunk = false;
      }
      if (callbacks.on_audio) callbacks.on_audio(chunk);
      if (options.collect_audio) {
        result.audio.insert(result.audio.end(), chunk.audio.begin(), chunk.audio.end());
      }
    }
    if (detail::has(data, "word_timestamps")) {
      auto stamps = detail::word_timestamps_from_json(data["word_timestamps"]);
      if (callbacks.on_word_timestamps) callbacks.on_word_timestamps(stamps);
      result.word_timestamps.insert(result.word_timestamps.end(), stamps.begin(), stamps.end());
    }
  }
  result.samples = received_bytes / bytes_per_sample(result.encoding);
  result.duration_ms = result.sample_rate > 0
                           ? 1000.0 * static_cast<double>(result.samples) / result.sample_rate
                           : 0.0;

  const bool cancel_sent = finish();
  if (cancel_sent && !result.interrupted) {
    // A cancel crossed the final frame: consume its ack so the next stream
    // on this socket does not end on a stale `interrupted`.
    while (true) {
      const auto frame = socket->receive(detail::kRecvTimeout);
      if (!frame || frame->kind == WsFrame::Kind::Closed) {
        if (pooled_socket) drop_pooled(pool, socket);
        break;
      }
      if (frame->kind == WsFrame::Kind::Text &&
          detail::truthy(detail::parse_json(frame->data, "/ws/tts"), "interrupted")) {
        break;
      }
    }
  }
}

}  // namespace

TtsResource::TtsResource(std::shared_ptr<detail::ClientCore> core)
    : core_(std::move(core)), pool_(std::make_unique<detail::PooledTtsSocket>()) {}

TtsResource::~TtsResource() { disconnect(); }

AudioResponse TtsResource::generate(const GenerateRequest& request) const {
  if (request.word_timestamps) {
    throw ValidationError(
        "word_timestamps is not supported by the REST endpoint; use tts().stream(), which "
        "returns word timestamps.");
  }
  core_->warn_if_no_language(request.language, request.normalize);
  auto op = core_->diagnostics->operation(std::string(detail::diag::kOpGenerate),
                                          detail::diag::kTransportHttp);
  try {
    detail::HttpRequest http;
    http.method = "POST";
    http.url = core_->url("/v1/tts/generate");
    http.headers = core_->headers();
    http.timeout = core_->timeout;
    http.json_body = detail::dump_json(detail::generate_body(request, false));
    detail::HttpResponse response = core_->send(std::move(http), "/v1/tts/generate");

    AudioResponse result;
    result.encoding = encoding::kPcm16;
    result.sample_rate = request.sample_rate;
    if (request.output_format) {
      // The token carries codec and rate ("ulaw_8000"); headers win below.
      const std::string& token = *request.output_format;
      const auto underscore = token.find('_');
      if (underscore != std::string::npos) {
        const std::string codec = token.substr(0, underscore);
        result.encoding = codec == "ulaw"   ? encoding::kMulaw
                          : codec == "alaw" ? encoding::kAlaw
                                            : encoding::kPcm16;
        result.sample_rate = parse_rate(token.substr(underscore + 1), "output_format");
      }
    }
    if (auto it = response.headers.find("x-sample-rate"); it != response.headers.end()) {
      result.sample_rate = parse_rate(it->second, "X-Sample-Rate");
    }
    if (auto it = response.headers.find("x-audio-format"); it != response.headers.end()) {
      result.encoding = it->second;
    }
    if (auto it = response.headers.find("x-request-id"); it != response.headers.end()) {
      result.request_id = it->second;
    }
    result.audio.assign(response.body.begin(), response.body.end());
    result.samples =
        static_cast<std::int64_t>(result.audio.size()) / bytes_per_sample(result.encoding);
    result.duration_ms = result.sample_rate > 0
                             ? 1000.0 * static_cast<double>(result.samples) / result.sample_rate
                             : 0.0;
    op.record_chunk(result.audio.size());
    op.succeed();
    return result;
  } catch (...) {
    op.fail(std::current_exception());
    throw;
  }
}

AudioResponse TtsResource::stream(const GenerateRequest& request, const StreamCallbacks& callbacks,
                                  const StreamOptions& options) {
  auto op = core_->diagnostics->operation(std::string(detail::diag::kOpStream),
                                          detail::diag::kTransportWebsocket);
  AudioResponse result;
  result.sample_rate = request.sample_rate;
  try {
    if (options.reuse_connection) {
      std::lock_guard<std::mutex> serial(pool_->stream_mutex);
      GenerationSettings sticky;
      {
        std::lock_guard<std::mutex> lock(pool_->mutex);
        sticky = pool_->sticky;
      }
      const GenerateRequest effective = with_sticky(request, sticky);
      const std::string body = detail::dump_json(detail::generate_body(effective, true));
      core_->warn_if_no_language(effective.language, effective.normalize);
      op.set_stage(detail::diag::kStageConnecting);
      auto socket = pooled(*pool_, *core_, request.model_id, &op);
      run_stream(*pool_, *core_, socket, true, body, callbacks, options, op, result);
    } else {
      const std::string body = detail::dump_json(detail::generate_body(request, true));
      core_->warn_if_no_language(request.language, request.normalize);
      op.set_stage(detail::diag::kStageHandshake);
      std::shared_ptr<WsConnection> socket(
          WsConnection::open(core_->ws_options("/ws/tts", {{"model_id", request.model_id}})));
      run_stream(*pool_, *core_, socket, false, body, callbacks, options, op, result);
      socket->close();
    }
  } catch (...) {
    op.fail(std::current_exception());
    throw;
  }
  if (result.interrupted) {
    op.cancel();
  } else {
    op.succeed();
  }
  return result;
}

void TtsResource::cancel() {
  std::shared_ptr<WsConnection> active;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (!pool_->active || pool_->cancel_sent) return;
    pool_->cancel_sent = true;
    active = pool_->active;
  }
  try {
    active->send_text(R"({"cancel":true})");
  } catch (const WsClosedError&) {
    // KEEP-JUSTIFIED: the running stream observes the close and throws it.
  }
}

GenerationSettings TtsResource::update_settings(const GenerationSettings& settings) {
  const std::string body =
      detail::dump_json(json{{"update_settings", detail::settings_body(settings)}});
  std::lock_guard<std::mutex> serial(pool_->stream_mutex);
  std::string model_id = kDefaultModel;
  std::shared_ptr<WsConnection> socket;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (pool_->connection && pool_->connection->is_open()) socket = pool_->connection;
  }
  if (!socket) socket = pooled(*pool_, *core_, model_id, nullptr);
  try {
    socket->send_text(body);
  } catch (const WsClosedError& e) {
    drop_pooled(*pool_, socket);
    std::rethrow_exception(detail::classify_ws_close(e.code(), e.reason()));
  }
  while (true) {
    const auto frame = socket->receive(detail::kRecvTimeout);
    if (!frame) throw KugelAudioError("Timed out waiting for settings_updated acknowledgement.");
    if (frame->kind == WsFrame::Kind::Closed) {
      drop_pooled(*pool_, socket);
      std::rethrow_exception(detail::classify_ws_close(frame->close_code, frame->close_reason));
    }
    if (frame->kind != WsFrame::Kind::Text) continue;
    const json data = detail::parse_json(frame->data, "/ws/tts");
    if (detail::truthy(data, "error")) std::rethrow_exception(detail::classify_ws_frame(data));
    if (detail::truthy(data, "settings_updated")) {
      // Remember only after the server accepted (validated) the change.
      std::lock_guard<std::mutex> lock(pool_->mutex);
      GenerationSettings& sticky = pool_->sticky;
      if (settings.cfg_scale) sticky.cfg_scale = settings.cfg_scale;
      if (settings.temperature) sticky.temperature = settings.temperature;
      if (settings.speed) sticky.speed = settings.speed;
      if (settings.max_new_tokens) sticky.max_new_tokens = settings.max_new_tokens;
      if (settings.language) sticky.language = settings.language;
      if (settings.normalize) sticky.normalize = settings.normalize;
      return detail::generation_settings_from_json(data.value("settings", json::object()));
    }
  }
}

void TtsResource::connect(const std::string& model_id) {
  auto op = core_->diagnostics->operation(std::string(detail::diag::kOpStream),
                                          detail::diag::kTransportWebsocket);
  try {
    std::lock_guard<std::mutex> serial(pool_->stream_mutex);
    op.set_stage(detail::diag::kStageConnecting);
    pooled(*pool_, *core_, model_id, &op);
  } catch (...) {
    op.fail(std::current_exception());
    throw;
  }
  op.succeed();
}

bool TtsResource::is_connected() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->connection && pool_->connection->is_open();
}

void TtsResource::disconnect() {
  if (!pool_) return;
  std::shared_ptr<WsConnection> connection;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    connection = std::move(pool_->connection);
    pool_->url.clear();
  }
  if (connection) connection->close();
}

StreamingSession TtsResource::streaming_session(StreamConfig config,
                                                StreamingSessionCallbacks callbacks) const {
  return StreamingSession(core_, std::move(config), std::move(callbacks));
}

MultiContextSession TtsResource::multi_context_session(MultiContextConfig config,
                                                       MultiContextCallbacks callbacks) const {
  return MultiContextSession(core_, std::move(config), std::move(callbacks));
}

}  // namespace kugelaudio
