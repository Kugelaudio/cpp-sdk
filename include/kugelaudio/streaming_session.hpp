#pragma once

/// @file streaming_session.hpp
/// Token-by-token text input with audio output (`/ws/tts/stream`), for LLM
/// agents. One WebSocket carries many turns; a turn ends with flush().

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kugelaudio/types.hpp"

namespace kugelaudio {

namespace detail {
struct ClientCore;
}

/// Receives audio chunks as they arrive, on the calling thread.
using ChunkCallback = std::function<void(const AudioChunk& chunk)>;
/// Receives word timestamps as they arrive.
using WordTimestampsCallback = std::function<void(const std::vector<WordTimestamp>& stamps)>;

/// Optional observers of a StreamingSession.
struct StreamingSessionCallbacks {
  /// Called each time the server sends word timestamps.
  WordTimestampsCallback on_word_timestamps;
  /// Called with the text of a server `warning` frame (e.g. the idle
  /// auto-flush). Warnings are also logged at LogLevel::Warning.
  std::function<void(const std::string& warning)> on_warning;
};

/// A streaming-input session on `/ws/tts/stream`.
///
/// Create it with `client.tts().streaming_session(config)`. Feed LLM tokens
/// with send() as they arrive and end each turn with flush(); the server
/// chunks the text at sentence boundaries and streams audio back. The
/// configuration is sticky across turns on the same socket.
///
/// The destructor closes the session (RAII); audio still in flight at that
/// point is discarded, so flush() or drain() first.
///
/// Thread safety: call the methods from one thread at a time, except
/// cancel(), which may be called from any thread to barge in on a send() or
/// flush() that is blocked receiving audio.
///
/// Rolling deploys: when the server restarts (close code 1012/1013) before
/// any audio of the current turn arrived, the session reconnects and replays
/// the turn once; otherwise ServerRestartingError is thrown.
class StreamingSession {
 public:
  /// Prefer `client.tts().streaming_session(config)`.
  StreamingSession(std::shared_ptr<detail::ClientCore> core, StreamConfig config,
                   StreamingSessionCallbacks callbacks = {});
  ~StreamingSession();
  StreamingSession(StreamingSession&&) noexcept;
  StreamingSession& operator=(StreamingSession&&) noexcept;
  StreamingSession(const StreamingSession&) = delete;
  StreamingSession& operator=(const StreamingSession&) = delete;

  /// Open the WebSocket now, so the first send() skips the handshake.
  void connect();

  /// Send text of the current turn and receive the audio it produced so far.
  /// Without `flush`, returns after a short poll when the text did not
  /// complete a sentence yet.
  /// @param text Text or LLM token(s); no client-side buffering needed.
  /// @param flush End the text with a flush (the server synthesizes the rest).
  /// @param on_chunk Receives each audio chunk as it arrives.
  void send(const std::string& text, bool flush, const ChunkCallback& on_chunk);
  /// Same as send(text, flush, on_chunk), collecting the chunks.
  std::vector<AudioChunk> send(const std::string& text, bool flush = false);

  /// Flush the buffered text and receive its audio (the turn ends with
  /// `final` and `session_closed`; see last_final() and last_usage()).
  void flush(const ChunkCallback& on_chunk);
  /// Same as flush(on_chunk), collecting the chunks.
  std::vector<AudioChunk> flush();

  /// Signal end of input (`close`) and receive every remaining chunk until
  /// `session_closed`. The next send() starts a fresh turn on the same socket.
  void drain(const ChunkCallback& on_chunk);
  /// Same as drain(on_chunk), collecting the chunks.
  std::vector<AudioChunk> drain();

  /// End the current turn but keep the WebSocket open. Audio still in flight
  /// is discarded (use drain() to keep it). Returns the turn's stats.
  SessionStats end_session();

  /// Barge-in: stop generating the current turn immediately and drop its
  /// buffered text. Returns once the server acknowledged with `interrupted`.
  /// Thread-safe: may be called while another thread is inside send()/flush(),
  /// which then returns early without the cancelled audio.
  void cancel();

  /// Replace the configuration for the next turn (e.g. another voice). Call
  /// between turns; it is sent with the next text.
  void update_config(StreamConfig config);

  /// Change generation parameters without reconnecting; applies to the next
  /// turn. Returns the parameters now in effect (the server's echo).
  /// @throws ValidationError when `settings` is empty or rejected.
  GenerationSettings update_settings(const GenerationSettings& settings);

  /// End the session and close the WebSocket. Returns the last turn's stats.
  SessionStats close();

  /// Word timestamps of the most recent `word_timestamps` frame.
  std::vector<WordTimestamp> last_word_timestamps() const;
  /// Usage of the most recently closed turn.
  std::optional<SessionUsage> last_usage() const;
  /// End-of-audio stats of the most recently completed turn.
  std::optional<TurnFinal> last_final() const;
  /// True while the WebSocket is open.
  bool is_connected() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kugelaudio
