#pragma once

/// @file audio.hpp
/// Audio constants, WAV writing, and WAV loading for speech enhancement.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kugelaudio/types.hpp"

namespace kugelaudio {

/// Native generation rate of the TTS models.
inline constexpr int kNativeSampleRate = 24000;
/// Sample rate of enhanced audio.
inline constexpr int kEnhancedSampleRate = 24000;
/// Every `sample_rate` value the TTS API accepts.
inline constexpr int kSupportedSampleRates[] = {8000, 16000, 22050, 24000, 44100};

/// Encodings reported in audio frames (`enc`) and REST `X-Audio-Format`.
namespace encoding {
inline constexpr const char* kPcm16 = "pcm_s16le";
inline constexpr const char* kMulaw = "mulaw";
inline constexpr const char* kAlaw = "alaw";
}  // namespace encoding

/// Combined codec + rate tokens for `output_format`.
namespace output_format {
inline constexpr const char* kPcm8000 = "pcm_8000";
inline constexpr const char* kPcm16000 = "pcm_16000";
inline constexpr const char* kPcm22050 = "pcm_22050";
inline constexpr const char* kPcm24000 = "pcm_24000";
inline constexpr const char* kUlaw8000 = "ulaw_8000";
inline constexpr const char* kAlaw8000 = "alaw_8000";
}  // namespace output_format

/// Language codes accepted for normalization.
inline constexpr const char* kSupportedLanguages[] = {
    "de", "en", "fr", "es", "it", "pt", "nl", "pl", "sv", "da",  "no", "fi", "cs",
    "hu", "ro", "el", "uk", "bg", "tr", "vi", "ar", "hi", "zh",  "ja", "ko", "sk",
    "sl", "hr", "sr", "ru", "he", "fa", "ur", "bn", "ta", "yue", "th", "id", "ms"};

/// Bytes per sample of an encoding: 2 for "pcm_s16le", 1 for "mulaw"/"alaw".
/// @throws ValidationError for an unknown encoding.
int bytes_per_sample(std::string_view encoding);

/// Wrap mono audio in a WAV container.
/// @param audio Raw samples in `encoding` ("pcm_s16le", "mulaw" or "alaw").
/// @param sample_rate Sample rate in Hz.
/// @param encoding Sample encoding; G.711 is written as WAV format 7 / 6.
Bytes to_wav(const Bytes& audio, int sample_rate, std::string_view encoding = encoding::kPcm16);

/// Write mono audio to `path` as a WAV file.
/// @throws KugelAudioError when the file cannot be written.
void write_wav(const std::filesystem::path& path, const Bytes& audio, int sample_rate,
               std::string_view encoding = encoding::kPcm16);

/// A WAV recording loaded with load_audio(), sent to the server as-is.
struct AudioFile {
  /// The WAV file bytes.
  Bytes data;
  /// File name sent with the upload.
  std::string filename = "audio.wav";
  /// Duration in seconds when the header is readable here (16/24/32-bit PCM).
  std::optional<double> duration;
};

/// Load a WAV file for `client.enhance().generate()`. Any WAV the API
/// supports is accepted (PCM 16/24/32-bit or float, mono or stereo,
/// 8-48 kHz, at most 300 s); the server decodes it.
/// @throws ValidationError when the file cannot be read or is empty.
AudioFile load_audio(const std::filesystem::path& path);

/// Same as load_audio(path), for WAV bytes already in memory.
/// @throws ValidationError when `wav_bytes` is empty.
AudioFile load_audio(Bytes wav_bytes, std::string filename = "audio.wav");

/// Mono PCM16 audio served in fixed-length chunks, for
/// `client.enhance().stream()`. Create one with load_audio_stream().
class AudioStream {
 public:
  /// @param pcm Mono PCM16 little-endian samples.
  /// @param sample_rate Sample rate in Hz.
  /// @param chunk_seconds Length of each chunk, in (0, 1].
  /// @throws ValidationError for an invalid chunk length or empty audio.
  AudioStream(Bytes pcm, int sample_rate, double chunk_seconds = 0.1);

  /// Sample rate in Hz.
  int sample_rate() const noexcept { return sample_rate_; }
  /// Duration in seconds.
  double duration() const noexcept;
  /// The audio as consecutive chunks of `chunk_seconds` (the last may be shorter).
  std::vector<Bytes> chunks() const;
  /// All samples.
  const Bytes& pcm() const noexcept { return pcm_; }

 private:
  Bytes pcm_;
  int sample_rate_;
  std::size_t chunk_bytes_;
};

/// Load a 16-bit PCM WAV file for `client.enhance().stream()`. Multi-channel
/// audio is downmixed to mono (average); the sample rate is read from the file.
/// @param chunk_seconds Length of each chunk, greater than 0 and at most 1.
/// @throws ValidationError when the file is not a readable 16-bit PCM WAV.
AudioStream load_audio_stream(const std::filesystem::path& path, double chunk_seconds = 0.1);

/// Same as load_audio_stream(path), for WAV bytes already in memory.
AudioStream load_audio_stream(const Bytes& wav_bytes, double chunk_seconds = 0.1);

}  // namespace kugelaudio
