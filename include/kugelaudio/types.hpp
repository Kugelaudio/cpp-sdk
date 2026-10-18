#pragma once

/// @file types.hpp
/// Typed requests and responses of the REST and WebSocket APIs.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kugelaudio {

/// Raw audio bytes (PCM16 little-endian unless the encoding says otherwise).
using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------- models

/// A text-to-speech model, from `client.models().list()`.
struct Model {
  std::string id;
  std::string name;
  std::string description;
  std::string parameters;
  int max_input_length = 5000;
  int sample_rate = 24000;
};

// ---------------------------------------------------------------- voices

/// Voice categories the API accepts and returns. Kept as strings so a
/// category added on the server never breaks parsing.
namespace voice_category {
inline constexpr const char* kConversational = "conversational";
inline constexpr const char* kNarrativeStory = "narrative_story";
inline constexpr const char* kCharactersAnimation = "characters_animation";
inline constexpr const char* kSocialMedia = "social_media";
inline constexpr const char* kEntertainmentTv = "entertainment_tv";
inline constexpr const char* kAdvertisement = "advertisement";
inline constexpr const char* kInformativeEducational = "informative_educational";
/// Returned for a voice with no category set.
inline constexpr const char* kCloned = "cloned";
}  // namespace voice_category

/// A voice as listed by `client.voices().list()`.
struct Voice {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> category;
  std::optional<std::string> sex;
  std::optional<std::string> age;
  std::string quality = "mid";
  std::vector<std::string> supported_languages;
  std::optional<std::string> sample_text;
  std::optional<std::string> avatar_url;
  std::optional<std::string> sample_url;
  bool is_public = false;
  bool verified = false;
};

/// One page of voices.
struct VoiceListResponse {
  std::vector<Voice> voices;
  std::int64_t total = 0;
  std::int64_t limit = 0;
  std::int64_t offset = 0;
};

/// Filters and paging for `client.voices().list()`.
struct VoiceListOptions {
  /// Language code filter, e.g. "de".
  std::optional<std::string> language;
  /// Include public voices.
  bool include_public = true;
  /// Page size (at most 100).
  int limit = 20;
  /// Number of voices to skip.
  int offset = 0;
};

/// Extended voice information returned by the voice management endpoints.
struct VoiceDetail {
  std::int64_t id = 0;
  std::string name;
  std::string description;
  std::string generative_voice_description;
  std::vector<std::string> supported_languages;
  std::optional<std::string> category;
  std::optional<std::string> age;
  std::optional<std::string> sex;
  std::string quality = "mid";
  bool is_public = false;
  bool verified = false;
  bool pending_verification = false;
  std::optional<std::string> sample_url;
  std::optional<std::string> avatar_url;
  std::string sample_text;
};

/// Input of `client.voices().create()` (multipart metadata + reference files).
struct VoiceCreateRequest {
  std::string name;
  /// "male" or "female".
  std::string sex;
  /// Must be `true`: you confirm that you are the speaker of the recordings or
  /// have the speaker's permission. The API rejects the request otherwise
  /// (ValidationError with error code SPEAKER_CONSENT_REQUIRED).
  bool speaker_consent = false;
  std::string description;
  std::string category = voice_category::kConversational;
  /// "young", "middle_age" or "old".
  std::string age = "middle_age";
  /// "low", "mid" or "high".
  std::string quality = "mid";
  std::vector<std::string> supported_languages{"en"};
  bool is_public = false;
  /// Text used for automatic sample generation.
  std::string sample_text;
  /// Reference recordings to upload (WAV).
  std::vector<std::filesystem::path> reference_files;
};

/// Fields to change with `client.voices().update()`; unset fields are kept.
struct VoiceUpdate {
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> category;
  std::optional<std::string> age;
  std::optional<std::string> sex;
  std::optional<std::string> quality;
  std::optional<std::vector<std::string>> supported_languages;
  std::optional<bool> is_public;
  std::optional<std::string> sample_text;
};

/// Reference recording metadata of a voice.
struct VoiceReference {
  std::int64_t id = 0;
  std::int64_t voice_id = 0;
  std::string name;
  std::string reference_text;
  std::string s3_path;
  std::optional<std::string> audio_url;
  bool is_generated = false;
};

/// Stored preview sample from `client.voices().generate_sample()`.
struct GeneratedSample {
  std::string sample_s3_path;
  /// Signed, time-limited URL; empty when the server could not sign one.
  std::optional<std::string> sample_url;
};

// ---------------------------------------------------------- dictionaries

/// A per-project pronunciation dictionary.
struct Dictionary {
  std::int64_t id = 0;
  std::int64_t project_id = 0;
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> language;
  bool is_active = true;
  std::optional<std::string> created_at;
  std::optional<std::string> updated_at;
};

/// Input of `client.dictionaries().create()`.
struct DictionaryCreate {
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> language;
};

/// Fields to change with `client.dictionaries().update()`.
struct DictionaryUpdate {
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> language;
  std::optional<bool> is_active;
};

/// A word -> replacement / IPA mapping inside a dictionary.
struct DictionaryEntry {
  std::int64_t id = 0;
  std::int64_t dictionary_id = 0;
  std::string word;
  std::string replacement;
  std::optional<std::string> ipa;
  bool case_sensitive = false;
  std::optional<std::string> created_at;
  std::optional<std::string> updated_at;
};

/// Input of `entries().add()` and one item of `entries().replace_all()`.
struct DictionaryEntryInput {
  std::string word;
  std::string replacement;
  std::optional<std::string> ipa;
  bool case_sensitive = false;
};

/// Fields to change with `entries().update()`.
struct DictionaryEntryUpdate {
  std::optional<std::string> word;
  std::optional<std::string> replacement;
  std::optional<std::string> ipa;
  std::optional<bool> case_sensitive;
};

/// Filters and paging for `entries().list()`.
struct DictionaryEntryListOptions {
  /// Case-insensitive substring filter on `word`.
  std::optional<std::string> search;
  /// Page size, 1-500.
  int limit = 100;
  int offset = 0;
};

/// One page of dictionary entries.
struct DictionaryEntryList {
  std::vector<DictionaryEntry> entries;
  std::int64_t total = 0;
  std::int64_t limit = 0;
  std::int64_t offset = 0;
};

/// Counts returned by `entries().replace_all()`.
struct BulkReplaceResult {
  std::int64_t upserted = 0;
  std::int64_t deleted = 0;
  std::int64_t total = 0;
};

// ------------------------------------------------------------------- TTS

/// Accepted classifier-free guidance band; values outside are clamped.
inline constexpr double kMinCfgScale = 1.2;
/// Accepted classifier-free guidance band; values outside are clamped.
inline constexpr double kMaxCfgScale = 2.5;

/// Clamp `cfg_scale` into [kMinCfgScale, kMaxCfgScale].
double clamp_cfg_scale(double cfg_scale) noexcept;

/// One text-to-speech request (REST `generate` and WebSocket `stream`).
///
/// Field meanings and ranges: https://docs.kugelaudio.com/api-reference/tts/generate
struct GenerateRequest {
  /// Text to synthesize (2-10,000 characters). Supports `<break>`, `<spell>`
  /// and `<prosody rate>` tags.
  std::string text;
  /// "kugel-3.5" (default) or "kugel-3".
  std::string model_id = "kugel-3.5";
  /// Required by the API: there is no default voice.
  std::optional<std::int64_t> voice_id;
  /// Classifier-free guidance, clamped to [1.2, 2.5].
  double cfg_scale = 2.0;
  /// Sampling variance in [0, 1]; unset uses the engine default.
  std::optional<double> temperature;
  /// Maximum tokens to generate, 1-2048.
  int max_new_tokens = 2048;
  /// 8000, 16000, 22050, 24000 or 44100.
  int sample_rate = 24000;
  /// Combined codec + rate token, e.g. "ulaw_8000" (see audio.hpp).
  std::optional<std::string> output_format;
  /// Convert numbers, dates and symbols to spoken words.
  bool normalize = true;
  /// ISO 639-1 code for normalization; set it when the text is not in the
  /// voice's primary language (the language is not detected from the text).
  std::optional<std::string> language;
  /// Request word-level timestamps. WebSocket only: `tts().generate()` (REST)
  /// rejects it, use `tts().stream()`.
  bool word_timestamps = false;
  /// Playback speed multiplier, 0.8-1.2 (pitch preserving).
  double speed = 1.0;
  /// Unset: all active dictionaries of the project apply. Empty: none apply.
  /// A list: exactly those dictionaries apply.
  std::optional<std::vector<std::int64_t>> dictionary_ids;
  /// Project whose dictionaries apply; defaults to the API key's project.
  std::optional<std::int64_t> project_id;
};

/// One word with its time boundaries.
struct WordTimestamp {
  std::string word;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::int64_t char_start = 0;
  std::int64_t char_end = 0;
  /// Compatibility field, currently always 1.0.
  double score = 1.0;
  /// Duration of the word in milliseconds.
  std::int64_t duration_ms() const noexcept { return end_ms - start_ms; }
};

/// Usage reported for a request, turn (`session_closed`) or context
/// (`context_closed`).
struct SessionUsage {
  /// Audio generated, the unit that is billed.
  double audio_seconds = 0.0;
  /// Actual charge in EUR cents; unset when it could not be determined
  /// (never a misleading 0).
  std::optional<double> cost_cents;
  std::optional<std::string> currency;
  std::optional<std::int64_t> characters;
  std::optional<std::string> model_id;
  /// True when an authoritative charge was returned.
  bool cost_available() const noexcept { return cost_cents.has_value(); }
};

/// One audio chunk received over a WebSocket.
struct AudioChunk {
  /// Decoded audio bytes (encoding per `encoding`).
  Bytes audio;
  /// "pcm_s16le", "mulaw" or "alaw".
  std::string encoding = "pcm_s16le";
  /// Chunk index (`idx`).
  std::int64_t index = 0;
  int sample_rate = 24000;
  std::int64_t samples = 0;
  /// Server text chunk id (`chunk_id`), when present.
  std::optional<std::int64_t> chunk_id;
  /// Context id on multi-context sessions.
  std::optional<std::string> context_id;
  /// Duration in seconds.
  double duration_seconds() const noexcept {
    return sample_rate > 0 ? static_cast<double>(samples) / sample_rate : 0.0;
  }
};

/// A complete synthesis result: from `tts().generate()` (REST) or
/// `tts().stream()` (WebSocket, with stats and usage).
struct AudioResponse {
  Bytes audio;
  int sample_rate = 24000;
  /// "pcm_s16le", "mulaw" or "alaw".
  std::string encoding = "pcm_s16le";
  /// Samples delivered at `sample_rate` (after resampling and `speed`).
  std::int64_t samples = 0;
  /// Duration of the delivered audio in milliseconds.
  double duration_ms = 0.0;
  /// Server generation time (WebSocket `final`); 0 for REST.
  double generation_ms = 0.0;
  /// Real-time factor (WebSocket `final`); 0 for REST.
  double rtf = 0.0;
  /// Populated when word timestamps were requested (WebSocket only).
  std::vector<WordTimestamp> word_timestamps;
  /// Usage of the request (WebSocket `final`), when reported.
  std::optional<SessionUsage> usage;
  /// `x-request-id` of the REST response.
  std::optional<std::string> request_id;
  /// True when the stream ended with an `interrupted` ack (barge-in).
  bool interrupted = false;
  /// Duration in seconds.
  double duration_seconds() const noexcept {
    return sample_rate > 0 ? static_cast<double>(samples) / sample_rate : 0.0;
  }
  /// The audio as a WAV file (PCM16, mu-law or a-law per `encoding`).
  Bytes to_wav() const;
  /// Write the audio to `path` as a WAV file.
  void save_wav(const std::filesystem::path& path) const;
};

/// Generation parameters changeable mid-connection with `update_settings`.
/// Only set fields are sent; the server echoes all parameters in effect.
struct GenerationSettings {
  std::optional<double> cfg_scale;
  std::optional<double> temperature;
  std::optional<double> speed;
  std::optional<int> max_new_tokens;
  std::optional<std::string> language;
  std::optional<bool> normalize;
  /// True when no field is set.
  bool empty() const noexcept {
    return !cfg_scale && !temperature && !speed && !max_new_tokens && !language && !normalize;
  }
};

/// Sticky configuration of a streaming-input session (`/ws/tts/stream`).
struct StreamConfig {
  std::optional<std::int64_t> voice_id;
  std::optional<std::string> model_id;
  double cfg_scale = 2.0;
  std::optional<double> temperature;
  int max_new_tokens = 2048;
  int sample_rate = 24000;
  /// Combined codec + rate token, e.g. "ulaw_8000"; set once per session.
  std::optional<std::string> output_format;
  /// Longest wait for the next token after an ambiguous period.
  int flush_timeout_ms = 500;
  /// Maximum buffered characters before a forced flush.
  int max_buffer_length = 1000;
  bool normalize = true;
  std::optional<std::string> language;
  bool word_timestamps = false;
  /// Minimum buffer sizes (characters) before each successive auto chunk.
  std::optional<std::vector<int>> chunk_length_schedule;
  /// Start generating at the very first clean sentence boundary.
  std::optional<bool> auto_mode;
  double speed = 1.0;
  std::optional<std::vector<std::int64_t>> dictionary_ids;
  std::optional<std::int64_t> project_id;
};

/// End-of-audio stats of a gracefully completed turn (the `final` frame).
struct TurnFinal {
  double total_audio_seconds = 0.0;
  std::int64_t total_text_chunks = 0;
  std::int64_t total_audio_chunks = 0;
};

/// The `session_closed` frame of a turn: stats and usage.
struct SessionStats {
  double total_audio_seconds = 0.0;
  std::int64_t total_text_chunks = 0;
  std::int64_t total_audio_chunks = 0;
  std::optional<SessionUsage> usage;
  /// False when no `session_closed` frame was received (e.g. nothing to end).
  bool received = false;
};

/// Session-level configuration of a multi-context session (`/ws/tts/multi`).
struct MultiContextConfig {
  /// Voice of contexts created without their own voice.
  std::optional<std::int64_t> default_voice_id;
  std::optional<std::string> model_id;
  int sample_rate = 24000;
  std::optional<std::string> output_format;
  double cfg_scale = 2.0;
  std::optional<double> temperature;
  int max_new_tokens = 2048;
  bool normalize = true;
  std::optional<std::string> language;
  std::optional<std::vector<std::int64_t>> dictionary_ids;
  std::optional<std::int64_t> project_id;
  bool word_timestamps = false;
  /// Playback speed multiplier, 0.8-1.2; unset uses the server default (1.0).
  std::optional<double> speed;
};

}  // namespace kugelaudio
