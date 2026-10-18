#include "kugelaudio/audio.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "kugelaudio/errors.hpp"
#include "wav.hpp"

namespace kugelaudio {

namespace detail {

namespace {

std::uint32_t read_u32(const Bytes& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) |
         (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t read_u16(const Bytes& b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

bool tag_at(const Bytes& b, std::size_t at, const char* tag) {
  return at + 4 <= b.size() && std::memcmp(&b[at], tag, 4) == 0;
}

std::optional<WavInfo> fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return std::nullopt;
}

}  // namespace

std::optional<WavInfo> parse_wav(const Bytes& wav, std::string* error) {
  if (wav.size() < 12 || !tag_at(wav, 0, "RIFF") || !tag_at(wav, 8, "WAVE")) {
    return fail(error, "not a RIFF/WAVE file");
  }
  WavInfo info;
  bool have_fmt = false;
  std::size_t at = 12;
  while (at + 8 <= wav.size()) {
    const std::uint32_t size = read_u32(wav, at + 4);
    const std::size_t body = at + 8;
    if (tag_at(wav, at, "fmt ")) {
      if (size < 16 || body + 16 > wav.size()) return fail(error, "truncated fmt chunk");
      info.format = read_u16(wav, body);
      info.channels = read_u16(wav, body + 2);
      info.sample_rate = static_cast<int>(read_u32(wav, body + 4));
      info.bits_per_sample = read_u16(wav, body + 14);
      if (info.format == 0xFFFE && size >= 40 && body + 26 <= wav.size()) {
        info.format = read_u16(wav, body + 24);  // sub-format GUID starts with the format tag
      }
      have_fmt = true;
    } else if (tag_at(wav, at, "data")) {
      if (!have_fmt) return fail(error, "data chunk before fmt chunk");
      info.data_offset = body;
      info.data_size = std::min<std::size_t>(size, wav.size() - body);
      return info;
    }
    at = body + size + (size & 1U);
  }
  return fail(error, "no data chunk");
}

}  // namespace detail

namespace {

void put_u32(Bytes& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_u16(Bytes& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_tag(Bytes& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

Bytes read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ValidationError("Cannot read audio file " + path.string() + ".");
  Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) throw ValidationError("Cannot read audio file " + path.string() + ".");
  return data;
}

constexpr const char* kFormatHint =
    "Use a 16-bit PCM WAV, or client.enhance().generate(load_audio(...)), which accepts any "
    "supported WAV.";

}  // namespace

int bytes_per_sample(std::string_view enc) {
  if (enc == encoding::kPcm16) return 2;
  if (enc == encoding::kMulaw || enc == encoding::kAlaw) return 1;
  throw ValidationError("Unknown audio encoding '" + std::string(enc) + "'.");
}

Bytes to_wav(const Bytes& audio, int sample_rate, std::string_view enc) {
  const int width = bytes_per_sample(enc);
  const std::uint16_t format = enc == encoding::kPcm16 ? 1 : (enc == encoding::kMulaw ? 7 : 6);
  const std::uint32_t fmt_size = format == 1 ? 16 : 18;
  const auto data_size = static_cast<std::uint32_t>(audio.size());
  Bytes out;
  out.reserve(audio.size() + 46);
  put_tag(out, "RIFF");
  put_u32(out, 4 + (8 + fmt_size) + (8 + data_size));
  put_tag(out, "WAVE");
  put_tag(out, "fmt ");
  put_u32(out, fmt_size);
  put_u16(out, format);
  put_u16(out, 1);  // mono
  put_u32(out, static_cast<std::uint32_t>(sample_rate));
  put_u32(out, static_cast<std::uint32_t>(sample_rate * width));
  put_u16(out, static_cast<std::uint16_t>(width));
  put_u16(out, static_cast<std::uint16_t>(8 * width));
  if (fmt_size == 18) put_u16(out, 0);
  put_tag(out, "data");
  put_u32(out, data_size);
  out.insert(out.end(), audio.begin(), audio.end());
  return out;
}

void write_wav(const std::filesystem::path& path, const Bytes& audio, int sample_rate,
               std::string_view enc) {
  const Bytes wav = to_wav(audio, sample_rate, enc);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  out.close();
  if (!out) throw KugelAudioError("Cannot write WAV file " + path.string() + ".");
}

Bytes AudioResponse::to_wav() const { return kugelaudio::to_wav(audio, sample_rate, encoding); }

void AudioResponse::save_wav(const std::filesystem::path& path) const {
  write_wav(path, audio, sample_rate, encoding);
}

AudioFile load_audio(Bytes wav_bytes, std::string filename) {
  if (wav_bytes.empty()) throw ValidationError("Audio must not be empty.");
  AudioFile file;
  if (auto info = detail::parse_wav(wav_bytes)) {
    // Duration only for integer PCM; the server decodes every other format.
    if (info->format == 1 && info->channels > 0 && info->bits_per_sample > 0 &&
        info->sample_rate > 0) {
      const auto frame = static_cast<std::size_t>(info->channels * info->bits_per_sample / 8);
      if (frame > 0) {
        file.duration = static_cast<double>(info->data_size / frame) / info->sample_rate;
      }
    }
  }
  file.data = std::move(wav_bytes);
  file.filename = std::move(filename);
  return file;
}

AudioFile load_audio(const std::filesystem::path& path) {
  return load_audio(read_all(path), path.filename().string());
}

AudioStream::AudioStream(Bytes pcm, int sample_rate, double chunk_seconds)
    : pcm_(std::move(pcm)), sample_rate_(sample_rate), chunk_bytes_(2) {
  if (!(chunk_seconds > 0.0 && chunk_seconds <= 1.0)) {
    throw ValidationError("chunk_seconds must be greater than 0 and at most 1.");
  }
  if (sample_rate <= 0) throw ValidationError("sample_rate must be positive.");
  if (pcm_.empty()) throw ValidationError("Audio must not be empty.");
  chunk_bytes_ =
      std::max<std::size_t>(1, static_cast<std::size_t>(sample_rate * chunk_seconds)) * 2;
}

double AudioStream::duration() const noexcept {
  return static_cast<double>(pcm_.size() / 2) / sample_rate_;
}

std::vector<Bytes> AudioStream::chunks() const {
  std::vector<Bytes> out;
  for (std::size_t start = 0; start < pcm_.size(); start += chunk_bytes_) {
    const std::size_t end = std::min(pcm_.size(), start + chunk_bytes_);
    out.emplace_back(pcm_.begin() + static_cast<std::ptrdiff_t>(start),
                     pcm_.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return out;
}

AudioStream load_audio_stream(const Bytes& wav, double chunk_seconds) {
  if (!(chunk_seconds > 0.0 && chunk_seconds <= 1.0)) {
    throw ValidationError("chunk_seconds must be greater than 0 and at most 1.");
  }
  if (wav.empty()) throw ValidationError("Audio must not be empty.");
  std::string error;
  auto info = detail::parse_wav(wav, &error);
  if (!info) throw ValidationError("Cannot read the WAV file (" + error + "). " + kFormatHint);
  if (info->format != 1 || info->bits_per_sample != 16) {
    throw ValidationError("The WAV file is " + std::to_string(info->bits_per_sample) + "-bit" +
                          (info->format == 1 ? "" : " non-PCM") + ". " + kFormatHint);
  }
  if (info->channels < 1)
    throw ValidationError("The WAV file has no channels. " + std::string(kFormatHint));
  const std::size_t frame = static_cast<std::size_t>(info->channels) * 2;
  const std::size_t frames = info->data_size / frame;
  if (frames == 0) throw ValidationError("Audio must not be empty.");
  const std::uint8_t* data = wav.data() + info->data_offset;
  Bytes mono;
  mono.reserve(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    // Multi-channel input is averaged into mono (documented in audio.hpp).
    long sum = 0;
    for (int c = 0; c < info->channels; ++c) {
      const std::uint8_t* s = data + i * frame + static_cast<std::size_t>(c) * 2;
      sum += static_cast<std::int16_t>(static_cast<std::uint16_t>(s[0] | (s[1] << 8)));
    }
    long avg = sum / info->channels;
    if (sum < 0 && sum % info->channels != 0) avg -= 1;  // floor, like Python's //
    const auto sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(avg));
    mono.push_back(static_cast<std::uint8_t>(sample & 0xFF));
    mono.push_back(static_cast<std::uint8_t>(sample >> 8));
  }
  return AudioStream(std::move(mono), info->sample_rate, chunk_seconds);
}

AudioStream load_audio_stream(const std::filesystem::path& path, double chunk_seconds) {
  return load_audio_stream(read_all(path), chunk_seconds);
}

}  // namespace kugelaudio
