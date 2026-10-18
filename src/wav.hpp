#pragma once

// Minimal RIFF/WAVE parsing shared by the audio helpers and enhancement.

#include <cstdint>
#include <optional>
#include <string>

#include "kugelaudio/types.hpp"

namespace kugelaudio::detail {

struct WavInfo {
  /// 1 = PCM, 3 = IEEE float, 6 = a-law, 7 = mu-law (extensible resolved).
  int format = 0;
  int channels = 0;
  int sample_rate = 0;
  int bits_per_sample = 0;
  /// Offset and size of the `data` chunk payload.
  std::size_t data_offset = 0;
  std::size_t data_size = 0;
};

/// Parse the header of a WAV file; std::nullopt (with `error` set) when it is
/// not a readable RIFF/WAVE file.
std::optional<WavInfo> parse_wav(const Bytes& wav, std::string* error = nullptr);

}  // namespace kugelaudio::detail
