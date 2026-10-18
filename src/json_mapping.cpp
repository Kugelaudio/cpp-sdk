#include "json_mapping.hpp"

#include <algorithm>

#include "base64.hpp"
#include "kugelaudio/errors.hpp"

namespace kugelaudio {

double clamp_cfg_scale(double cfg_scale) noexcept {
  return std::min(kMaxCfgScale, std::max(kMinCfgScale, cfg_scale));
}

namespace detail {

namespace {

[[noreturn]] void missing(const char* key) {
  throw KugelAudioError(std::string("Unexpected response from KugelAudio: missing or invalid "
                                    "field '") +
                        key + "'.");
}

const json& field(const json& j, const char* key) {
  if (!j.is_object()) missing(key);
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) missing(key);
  return *it;
}

std::string req_str(const json& j, const char* key) {
  const json& v = field(j, key);
  if (!v.is_string()) missing(key);
  return v.get<std::string>();
}

std::int64_t req_int(const json& j, const char* key) {
  const json& v = field(j, key);
  if (!v.is_number()) missing(key);
  return v.get<std::int64_t>();
}

std::optional<std::string> opt_str(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

std::string str_or(const json& j, const char* key, const std::string& fallback) {
  return opt_str(j, key).value_or(fallback);
}

template <typename T>
T num_or(const json& j, const char* key, T fallback) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return fallback;
  return it->get<T>();
}

template <typename T>
std::optional<T> opt_num(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return std::nullopt;
  return it->get<T>();
}

bool bool_or(const json& j, const char* key, bool fallback) {
  auto it = j.find(key);
  if (it == j.end() || !it->is_boolean()) return fallback;
  return it->get<bool>();
}

std::vector<std::string> str_list(const json& j, const char* key) {
  std::vector<std::string> out;
  auto it = j.find(key);
  if (it == j.end() || !it->is_array()) return out;
  for (const auto& item : *it) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
  return out;
}

const json& array_field(const json& j, const char* key) {
  static const json kEmpty = json::array();
  if (!j.is_object()) missing(key);
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return kEmpty;
  if (!it->is_array()) missing(key);
  return *it;
}

}  // namespace

json parse_json(const std::string& text, const std::string& context) {
  json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    throw KugelAudioError("Unexpected response from KugelAudio (" + context + "): not JSON.");
  }
  return parsed;
}

std::string dump_json(const json& value) {
  try {
    return value.dump();
  } catch (const json::type_error&) {
    throw ValidationError("Text must be valid UTF-8.");
  }
}

Model model_from_json(const json& j) {
  Model m;
  m.id = req_str(j, "id");
  m.name = req_str(j, "name");
  m.description = str_or(j, "description", "");
  m.parameters = str_or(j, "parameters", "");
  m.max_input_length = num_or<int>(j, "max_input_length", 5000);
  m.sample_rate = num_or<int>(j, "sample_rate", 24000);
  return m;
}

Voice voice_from_json(const json& j) {
  Voice v;
  v.id = req_int(j, "id");
  v.name = req_str(j, "name");
  v.description = opt_str(j, "description");
  v.category = opt_str(j, "category");
  v.sex = opt_str(j, "sex");
  v.age = opt_str(j, "age");
  v.quality = str_or(j, "quality", "mid");
  v.supported_languages = str_list(j, "supported_languages");
  v.sample_text = opt_str(j, "sample_text");
  v.avatar_url = opt_str(j, "avatar_url");
  v.sample_url = opt_str(j, "sample_url");
  v.is_public = bool_or(j, "is_public", false);
  v.verified = bool_or(j, "verified", false);
  return v;
}

VoiceListResponse voice_list_from_json(const json& j) {
  VoiceListResponse r;
  for (const auto& item : array_field(j, "voices")) r.voices.push_back(voice_from_json(item));
  r.total = req_int(j, "total");
  r.limit = req_int(j, "limit");
  r.offset = req_int(j, "offset");
  return r;
}

VoiceDetail voice_detail_from_json(const json& j) {
  VoiceDetail v;
  v.id = req_int(j, "id");
  v.name = req_str(j, "name");
  v.description = str_or(j, "description", "");
  v.generative_voice_description = str_or(j, "generative_voice_description", "");
  v.supported_languages = str_list(j, "supported_languages");
  v.category = opt_str(j, "category");
  v.age = opt_str(j, "age");
  v.sex = opt_str(j, "sex");
  v.quality = str_or(j, "quality", "mid");
  v.is_public = bool_or(j, "is_public", false);
  v.verified = bool_or(j, "verified", false);
  v.pending_verification = bool_or(j, "pending_verification", false);
  v.sample_url = opt_str(j, "sample_url");
  v.avatar_url = opt_str(j, "avatar_url");
  v.sample_text = str_or(j, "sample_text", "");
  return v;
}

VoiceReference voice_reference_from_json(const json& j) {
  VoiceReference r;
  r.id = req_int(j, "id");
  r.voice_id = req_int(j, "voice_id");
  r.name = str_or(j, "name", "");
  r.reference_text = str_or(j, "reference_text", "");
  r.s3_path = str_or(j, "s3_path", "");
  r.audio_url = opt_str(j, "audio_url");
  r.is_generated = bool_or(j, "is_generated", false);
  return r;
}

GeneratedSample generated_sample_from_json(const json& j) {
  return GeneratedSample{req_str(j, "sample_s3_path"), opt_str(j, "sample_url")};
}

Dictionary dictionary_from_json(const json& j) {
  Dictionary d;
  d.id = req_int(j, "id");
  d.project_id = req_int(j, "project_id");
  d.name = req_str(j, "name");
  d.description = opt_str(j, "description");
  d.language = opt_str(j, "language");
  d.is_active = bool_or(j, "is_active", true);
  d.created_at = opt_str(j, "created_at");
  d.updated_at = opt_str(j, "updated_at");
  return d;
}

DictionaryEntry dictionary_entry_from_json(const json& j) {
  DictionaryEntry e;
  e.id = req_int(j, "id");
  e.dictionary_id = req_int(j, "dictionary_id");
  e.word = req_str(j, "word");
  e.replacement = req_str(j, "replacement");
  e.ipa = opt_str(j, "ipa");
  e.case_sensitive = bool_or(j, "case_sensitive", false);
  e.created_at = opt_str(j, "created_at");
  e.updated_at = opt_str(j, "updated_at");
  return e;
}

DictionaryEntryList dictionary_entry_list_from_json(const json& j) {
  DictionaryEntryList r;
  for (const auto& item : array_field(j, "entries")) {
    r.entries.push_back(dictionary_entry_from_json(item));
  }
  r.total = req_int(j, "total");
  r.limit = req_int(j, "limit");
  r.offset = req_int(j, "offset");
  return r;
}

BulkReplaceResult bulk_replace_from_json(const json& j) {
  return BulkReplaceResult{req_int(j, "upserted"), req_int(j, "deleted"), req_int(j, "total")};
}

WordTimestamp word_timestamp_from_json(const json& j) {
  WordTimestamp w;
  w.word = req_str(j, "word");
  w.start_ms = req_int(j, "start_ms");
  w.end_ms = req_int(j, "end_ms");
  w.char_start = req_int(j, "char_start");
  w.char_end = req_int(j, "char_end");
  w.score = num_or<double>(j, "score", 1.0);
  return w;
}

std::vector<WordTimestamp> word_timestamps_from_json(const json& j) {
  std::vector<WordTimestamp> out;
  if (!j.is_array()) missing("word_timestamps");
  for (const auto& item : j) out.push_back(word_timestamp_from_json(item));
  return out;
}

SessionUsage usage_from_json(const json& j) {
  SessionUsage u;
  u.audio_seconds = num_or<double>(j, "audio_seconds", 0.0);
  u.cost_cents = opt_num<double>(j, "cost_cents");
  u.currency = opt_str(j, "currency");
  u.characters = opt_num<std::int64_t>(j, "characters");
  u.model_id = opt_str(j, "model_id");
  return u;
}

std::optional<SessionUsage> usage_from_payload(const json& j) {
  if (!j.is_object()) return std::nullopt;
  if (auto it = j.find("usage"); it != j.end() && it->is_object()) return usage_from_json(*it);
  if (j.contains("total_audio_seconds")) {
    SessionUsage u;
    u.audio_seconds = num_or<double>(j, "total_audio_seconds", 0.0);
    return u;
  }
  return std::nullopt;
}

AudioChunk audio_chunk_from_json(const json& j) {
  AudioChunk c;
  c.audio = base64_decode(req_str(j, "audio"));
  c.encoding = str_or(j, "enc", "pcm_s16le");
  c.index = req_int(j, "idx");
  c.sample_rate = static_cast<int>(req_int(j, "sr"));
  c.samples = req_int(j, "samples");
  c.chunk_id = opt_num<std::int64_t>(j, "chunk_id");
  c.context_id = opt_str(j, "context_id");
  return c;
}

TurnFinal turn_final_from_json(const json& j) {
  TurnFinal f;
  f.total_audio_seconds = num_or<double>(j, "total_audio_seconds", 0.0);
  f.total_text_chunks = num_or<std::int64_t>(j, "total_text_chunks", 0);
  f.total_audio_chunks = num_or<std::int64_t>(j, "total_audio_chunks", 0);
  return f;
}

SessionStats session_stats_from_json(const json& j) {
  SessionStats s;
  s.total_audio_seconds = num_or<double>(j, "total_audio_seconds", 0.0);
  s.total_text_chunks = num_or<std::int64_t>(j, "total_text_chunks", 0);
  s.total_audio_chunks = num_or<std::int64_t>(j, "total_audio_chunks", 0);
  s.usage = usage_from_payload(j);
  s.received = true;
  return s;
}

GenerationSettings generation_settings_from_json(const json& j) {
  GenerationSettings s;
  if (!j.is_object()) return s;
  s.cfg_scale = opt_num<double>(j, "cfg_scale");
  s.temperature = opt_num<double>(j, "temperature");
  s.speed = opt_num<double>(j, "speed");
  s.max_new_tokens = opt_num<int>(j, "max_new_tokens");
  s.language = opt_str(j, "language");
  if (auto it = j.find("normalize"); it != j.end() && it->is_boolean()) {
    s.normalize = it->get<bool>();
  }
  return s;
}

json voice_create_metadata(const VoiceCreateRequest& r) {
  return json{{"name", r.name},
              {"sex", r.sex},
              {"description", r.description},
              {"category", r.category},
              {"age", r.age},
              {"quality", r.quality},
              {"supported_languages", r.supported_languages.empty() ? std::vector<std::string>{"en"}
                                                                    : r.supported_languages},
              {"is_public", r.is_public},
              {"sample_text", r.sample_text},
              {"speaker_consent", r.speaker_consent}};
}

json voice_update_body(const VoiceUpdate& u) {
  json body = json::object();
  if (u.name) body["name"] = *u.name;
  if (u.description) body["description"] = *u.description;
  if (u.category) body["category"] = *u.category;
  if (u.age) body["age"] = *u.age;
  if (u.sex) body["sex"] = *u.sex;
  if (u.quality) body["quality"] = *u.quality;
  if (u.supported_languages) body["supported_languages"] = *u.supported_languages;
  if (u.is_public) body["is_public"] = *u.is_public;
  if (u.sample_text) body["sample_text"] = *u.sample_text;
  return body;
}

json dictionary_create_body(const DictionaryCreate& r) {
  json body{{"name", r.name}};
  if (r.description) body["description"] = *r.description;
  if (r.language) body["language"] = *r.language;
  return body;
}

json dictionary_update_body(const DictionaryUpdate& u) {
  json body = json::object();
  if (u.name) body["name"] = *u.name;
  if (u.description) body["description"] = *u.description;
  if (u.language) body["language"] = *u.language;
  if (u.is_active) body["is_active"] = *u.is_active;
  return body;
}

json entry_input_body(const DictionaryEntryInput& e) {
  json body{{"word", e.word}, {"replacement", e.replacement}, {"case_sensitive", e.case_sensitive}};
  if (e.ipa) body["ipa"] = *e.ipa;
  return body;
}

json entry_update_body(const DictionaryEntryUpdate& u) {
  json body = json::object();
  if (u.word) body["word"] = *u.word;
  if (u.replacement) body["replacement"] = *u.replacement;
  if (u.ipa) body["ipa"] = *u.ipa;
  if (u.case_sensitive) body["case_sensitive"] = *u.case_sensitive;
  return body;
}

json generate_body(const GenerateRequest& r, bool include_word_timestamps) {
  json body{{"text", r.text},
            {"model_id", r.model_id},
            {"cfg_scale", clamp_cfg_scale(r.cfg_scale)},
            {"max_new_tokens", r.max_new_tokens},
            {"sample_rate", r.sample_rate},
            {"normalize", r.normalize},
            {"speed", r.speed}};
  if (include_word_timestamps) body["word_timestamps"] = r.word_timestamps;
  if (r.voice_id) body["voice_id"] = *r.voice_id;
  if (r.output_format) body["output_format"] = *r.output_format;
  if (r.language) body["language"] = *r.language;
  if (r.temperature) body["temperature"] = *r.temperature;
  if (r.project_id) body["project_id"] = *r.project_id;
  // [] is an explicit opt-out and must be sent; only unset is omitted.
  if (r.dictionary_ids) body["dictionary_ids"] = *r.dictionary_ids;
  return body;
}

json stream_config_body(const StreamConfig& c) {
  json body{{"cfg_scale", clamp_cfg_scale(c.cfg_scale)},
            {"max_new_tokens", c.max_new_tokens},
            {"sample_rate", c.sample_rate},
            {"flush_timeout_ms", c.flush_timeout_ms},
            {"max_buffer_length", c.max_buffer_length},
            {"normalize", c.normalize},
            {"word_timestamps", c.word_timestamps},
            {"speed", c.speed}};
  if (c.voice_id) body["voice_id"] = *c.voice_id;
  if (c.model_id) body["model_id"] = *c.model_id;
  if (c.output_format) body["output_format"] = *c.output_format;
  if (c.language) body["language"] = *c.language;
  if (c.chunk_length_schedule) body["chunk_length_schedule"] = *c.chunk_length_schedule;
  if (c.auto_mode) body["auto_mode"] = *c.auto_mode;
  if (c.temperature) body["temperature"] = *c.temperature;
  if (c.project_id) body["project_id"] = *c.project_id;
  if (c.dictionary_ids) body["dictionary_ids"] = *c.dictionary_ids;
  return body;
}

json settings_body(const GenerationSettings& s) {
  if (s.empty()) {
    throw ValidationError(
        "update_settings requires at least one parameter to change (one of cfg_scale, "
        "temperature, max_new_tokens, language, normalize, speed)");
  }
  json body = json::object();
  if (s.cfg_scale) body["cfg_scale"] = *s.cfg_scale;
  if (s.temperature) body["temperature"] = *s.temperature;
  if (s.speed) body["speed"] = *s.speed;
  if (s.max_new_tokens) body["max_new_tokens"] = *s.max_new_tokens;
  if (s.language) body["language"] = *s.language;
  if (s.normalize) body["normalize"] = *s.normalize;
  return body;
}

}  // namespace detail
}  // namespace kugelaudio
