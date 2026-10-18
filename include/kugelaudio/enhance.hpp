#pragma once

/// @file enhance.hpp
/// Speech enhancement: remove background noise, or keep only one voice
/// (`/v1/audio/enhance` and the `/v1/audio/enhance/stream` WebSocket).
/// Enhancement removes noise; passing a speaker sample keeps only that voice.
/// Output is always mono PCM16 at 24 kHz with the input's duration.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "kugelaudio/audio.hpp"
#include "kugelaudio/types.hpp"

namespace kugelaudio {

namespace detail {
struct ClientCore;
}

/// Enhanced audio: mono PCM16 (little-endian).
struct EnhancedAudio {
  /// Raw mono PCM16 samples.
  Bytes audio;
  int sample_rate = kEnhancedSampleRate;
  /// Duration in seconds.
  double duration() const noexcept;
  /// The audio as WAV file bytes.
  Bytes wav() const;
  /// Write the audio to `path` as a WAV file.
  void save(const std::filesystem::path& path) const;
  /// Parse a mono PCM16 WAV response.
  /// @throws KugelAudioError when `wav` is not a mono PCM16 WAV.
  static EnhancedAudio from_wav(const Bytes& wav);
};

/// What to enhance with.
struct EnhanceOptions {
  /// Enhancement model, e.g. "clarity-1". Required.
  std::string model;
  /// A clean 2-8 s sample of one voice: only that voice is kept
  /// (`target_speaker_extraction`); unset removes noise (`noise_removal`).
  std::optional<AudioFile> speaker;
};

/// Pulls the next raw mono PCM16 chunk of a live input; return std::nullopt
/// at the end of the audio. Called on a sender thread.
using PcmSource = std::function<std::optional<Bytes>()>;
/// Receives each enhanced chunk (mono PCM16, 24 kHz) on the calling thread.
using PcmCallback = std::function<void(const Bytes& pcm)>;

/// One warm WebSocket that carries enhancement streams one after another
/// (`session=1`), from `client.enhance().session()`. Each stream sends its own
/// config, so task, speaker and sample rate may change between streams; each
/// is admitted and billed as a request of its own. The socket is replaced
/// transparently when the server closed it (60 s idle, one hour lifetime,
/// restart); a server without session support gets one connection per stream.
///
/// Thread safety: streams on one session run one at a time (a second call
/// waits). close() may be called from any thread. The destructor closes it.
class EnhanceSession {
 public:
  /// Prefer `client.enhance().session()`.
  explicit EnhanceSession(std::shared_ptr<detail::ClientCore> core);
  ~EnhanceSession();
  EnhanceSession(EnhanceSession&&) noexcept;
  EnhanceSession& operator=(EnhanceSession&&) noexcept;
  EnhanceSession(const EnhanceSession&) = delete;
  EnhanceSession& operator=(const EnhanceSession&) = delete;

  /// Open the socket now, so the next stream skips the connection setup.
  void connect();
  /// Enhance one audio on the session's socket; see EnhanceResource::stream().
  void stream(const AudioStream& audio, const EnhanceOptions& options, const PcmCallback& on_pcm);
  /// Enhance live raw mono PCM16 at `sample_rate` on the session's socket.
  void stream(const PcmSource& source, int sample_rate, const EnhanceOptions& options,
              const PcmCallback& on_pcm);
  /// Close the socket; the session cannot stream afterwards.
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// `client.enhance()`. Every method is blocking and thread-safe.
class EnhanceResource {
 public:
  explicit EnhanceResource(std::shared_ptr<detail::ClientCore> core);

  /// Enhance a recording over REST (`POST /v1/audio/enhance`).
  /// @param audio From load_audio(): any supported WAV, at most 300 s.
  EnhancedAudio generate(const AudioFile& audio, const EnhanceOptions& options) const;

  /// Ready enhancement before the first request (`POST /v1/audio/enhance/warmup`):
  /// starts capacity on the server; not billed. Never throws: a failure is
  /// logged as a warning and the first real request raises its own error.
  void prewarm() const;

  /// Enhance audio in real time over the WebSocket; `on_pcm` receives the
  /// enhanced chunks as they arrive. Input is sent from a sender thread.
  void stream(const AudioStream& audio, const EnhanceOptions& options,
              const PcmCallback& on_pcm) const;
  /// Enhance live raw mono PCM16 at `sample_rate` (8-48 kHz). Chunks longer
  /// than one second are split.
  void stream(const PcmSource& source, int sample_rate, const EnhanceOptions& options,
              const PcmCallback& on_pcm) const;

  /// A session: one warm WebSocket for many streams.
  EnhanceSession session() const;

 private:
  std::shared_ptr<detail::ClientCore> core_;
};

}  // namespace kugelaudio
