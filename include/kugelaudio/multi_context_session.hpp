#pragma once

/// @file multi_context_session.hpp
/// Up to 20 independent audio streams over one WebSocket (`/ws/tts/multi`).

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kugelaudio/streaming_session.hpp"
#include "kugelaudio/types.hpp"

namespace kugelaudio {

namespace detail {
struct ClientCore;
}

/// Optional observers of a MultiContextSession.
struct MultiContextCallbacks {
  /// Called each time the server sends word timestamps for a context.
  std::function<void(const std::string& context_id, const std::vector<WordTimestamp>& stamps)>
      on_word_timestamps;
};

/// A multi-context session on `/ws/tts/multi`: each context has its own
/// voice, text buffer and generation queue (multi-speaker dialogue,
/// pre-buffering the next utterance while one plays).
///
/// Create it with `client.tts().multi_context_session(config)`. Audio of a
/// context that arrives while you are waiting on another context is kept and
/// returned by that context's next send(), flush() or close_context(), so no
/// audio is lost.
///
/// Thread safety: call the methods from one thread at a time. The
/// destructor closes the session (RAII).
///
/// Rolling deploys: on close code 1012/1013 the session reconnects,
/// re-creates every context and replays each context's unspoken turn once;
/// a context whose turn already produced audio gets ServerRestartingError.
class MultiContextSession {
 public:
  /// Prefer `client.tts().multi_context_session(config)`.
  MultiContextSession(std::shared_ptr<detail::ClientCore> core, MultiContextConfig config,
                      MultiContextCallbacks callbacks = {});
  ~MultiContextSession();
  MultiContextSession(MultiContextSession&&) noexcept;
  MultiContextSession& operator=(MultiContextSession&&) noexcept;
  MultiContextSession(const MultiContextSession&) = delete;
  MultiContextSession& operator=(const MultiContextSession&) = delete;

  /// Open the WebSocket now.
  void connect();

  /// Warm up a voice before any text exists (no context, no charge).
  void prepare_voice(std::int64_t voice_id);

  /// Create a context. Waits for `context_created`. Does nothing when the
  /// context already exists.
  /// @param voice_id Voice of this context; defaults to the session's
  ///        `default_voice_id`.
  /// @throws RateLimitError when 20 contexts are already open.
  void create_context(const std::string& context_id,
                      std::optional<std::int64_t> voice_id = std::nullopt);

  /// Send text to a context (created on first use) and receive its audio.
  /// @param flush End the text with a flush; waits for the context's `final`.
  /// @param chunk_complete_idle How long to wait after `chunk_complete` for
  ///        another sentence; zero returns right after the first one.
  void send(const std::string& context_id, const std::string& text, bool flush,
            const ChunkCallback& on_chunk,
            std::optional<std::chrono::milliseconds> chunk_complete_idle = std::nullopt);
  /// Same as send(..., on_chunk, ...), collecting the chunks.
  std::vector<AudioChunk> send(const std::string& context_id, const std::string& text,
                               bool flush = false);

  /// Flush a context's buffer and receive the remaining audio.
  void flush(const std::string& context_id, const ChunkCallback& on_chunk);
  /// Same as flush(context_id, on_chunk), collecting the chunks.
  std::vector<AudioChunk> flush(const std::string& context_id);

  /// Close a context and receive its remaining audio. With `immediate`, this
  /// is a barge-in: in-flight generation stops and buffered text is dropped.
  /// The context's usage is then available from usage_for().
  void close_context(const std::string& context_id, bool immediate, const ChunkCallback& on_chunk);
  /// Same as close_context(..., on_chunk), collecting the chunks.
  std::vector<AudioChunk> close_context(const std::string& context_id, bool immediate = false);

  /// Barge-in on one context, or on every live context when `context_id` is
  /// unset (`{"cancel": true}`); waits for `interrupted`.
  void cancel(std::optional<std::string> context_id = std::nullopt);

  /// Reset a context's 20 s inactivity timeout without generating audio.
  void keep_alive(const std::string& context_id);

  /// Change the session's generation parameters; applies to contexts
  /// started afterwards. Returns the parameters now in effect.
  GenerationSettings update_settings(const GenerationSettings& settings);

  /// Close every context and the WebSocket; returns total audio seconds of
  /// the session when the server reported it.
  std::optional<double> close();

  /// Usage of a closed context, if reported.
  std::optional<SessionUsage> usage_for(const std::string& context_id) const;
  /// Usage of every closed context.
  std::map<std::string, SessionUsage> context_usage() const;
  /// Most recent word timestamps of a context.
  std::vector<WordTimestamp> word_timestamps(const std::string& context_id) const;
  /// Contexts currently open on this socket.
  std::set<std::string> active_contexts() const;
  /// True while the WebSocket is open.
  bool is_alive() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kugelaudio
