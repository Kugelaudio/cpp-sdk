#pragma once

/// @file tts.hpp
/// Text to speech: REST generation, one-shot WebSocket streaming, and the
/// factories of the streaming-input and multi-context sessions.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kugelaudio/multi_context_session.hpp"
#include "kugelaudio/streaming_session.hpp"
#include "kugelaudio/types.hpp"

namespace kugelaudio {

namespace detail {
struct ClientCore;
struct PooledTtsSocket;
}  // namespace detail

/// Observers of a one-shot stream; both are optional.
struct StreamCallbacks {
  /// Each audio chunk as it arrives.
  ChunkCallback on_audio;
  /// Each `word_timestamps` frame (when `word_timestamps` is requested).
  WordTimestampsCallback on_word_timestamps;
};

/// Options of `tts().stream()`.
struct StreamOptions {
  /// Reuse the client's pooled WebSocket (lower time to first audio).
  /// Streams on the pooled socket run one at a time; set false to open a
  /// connection of its own (allows parallel streams).
  bool reuse_connection = true;
  /// Also gather the audio into the returned AudioResponse.
  bool collect_audio = true;
};

/// `client.tts()`: speech generation.
///
/// Thread safety: every method may be called from any thread. Streams on the
/// pooled connection are serialized; cancel() may be called from another
/// thread to barge in on a running stream().
class TtsResource {
 public:
  explicit TtsResource(std::shared_ptr<detail::ClientCore> core);
  ~TtsResource();
  TtsResource(const TtsResource&) = delete;
  TtsResource& operator=(const TtsResource&) = delete;

  /// Generate speech over REST (`POST /v1/tts/generate`) and return the whole
  /// audio. `sample_rate`/`encoding` come from the response headers.
  /// @throws ValidationError when `request.word_timestamps` is set (the REST
  ///         endpoint does not support it; use stream()).
  AudioResponse generate(const GenerateRequest& request) const;

  /// Stream speech over the WebSocket (`/ws/tts`): one request, audio chunks
  /// as they are generated, then the final stats and usage.
  /// @return The collected result, including word timestamps, stats and
  ///         usage; `interrupted` is set when cancel() ended the stream.
  AudioResponse stream(const GenerateRequest& request, const StreamCallbacks& callbacks = {},
                       const StreamOptions& options = {});

  /// Barge-in on the stream() running on another thread: the server stops
  /// generating and stream() returns with `interrupted == true`. Does nothing
  /// when no stream is running.
  void cancel();

  /// Set sticky generation defaults for stream() on the pooled `/ws/tts`
  /// connection (`update_settings`). They apply to every later pooled
  /// stream, also after the socket reconnects or switches model; streams
  /// with `reuse_connection = false` do not use them. A request field left
  /// at its default (`cfg_scale` 2.0, `speed` 1.0, `max_new_tokens` 2048,
  /// `normalize` true, `temperature`/`language` unset) takes the sticky
  /// value; any other value in the request wins. Returns the defaults the
  /// server now has in effect.
  /// @throws ValidationError when `settings` is empty or rejected.
  GenerationSettings update_settings(const GenerationSettings& settings);

  /// Open the pooled WebSocket for `model_id` now, so the first stream()
  /// skips the handshake (~300-600 ms).
  void connect(const std::string& model_id = "kugel-3.5");
  /// True when the pooled WebSocket is open.
  bool is_connected() const;
  /// Close the pooled WebSocket.
  void disconnect();

  /// A streaming-input session for LLM tokens (`/ws/tts/stream`).
  StreamingSession streaming_session(StreamConfig config = {},
                                     StreamingSessionCallbacks callbacks = {}) const;
  /// A multi-context session (`/ws/tts/multi`).
  MultiContextSession multi_context_session(MultiContextConfig config = {},
                                            MultiContextCallbacks callbacks = {}) const;

 private:
  std::shared_ptr<detail::ClientCore> core_;
  std::unique_ptr<detail::PooledTtsSocket> pool_;
};

}  // namespace kugelaudio
