#pragma once

// JSON <-> typed structs. Parsing is strict on identity fields: a response
// without one throws KugelAudioError instead of inventing a value.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kugelaudio/types.hpp"

namespace kugelaudio::detail {

using nlohmann::json;

/// Parse a server body; throws KugelAudioError naming `context` when it is
/// not JSON.
json parse_json(const std::string& text, const std::string& context);

/// Serialize; throws ValidationError when a string is not valid UTF-8.
std::string dump_json(const json& value);

Model model_from_json(const json& j);
Voice voice_from_json(const json& j);
VoiceListResponse voice_list_from_json(const json& j);
VoiceDetail voice_detail_from_json(const json& j);
VoiceReference voice_reference_from_json(const json& j);
GeneratedSample generated_sample_from_json(const json& j);
Dictionary dictionary_from_json(const json& j);
DictionaryEntry dictionary_entry_from_json(const json& j);
DictionaryEntryList dictionary_entry_list_from_json(const json& j);
BulkReplaceResult bulk_replace_from_json(const json& j);
WordTimestamp word_timestamp_from_json(const json& j);
std::vector<WordTimestamp> word_timestamps_from_json(const json& j);
SessionUsage usage_from_json(const json& j);
/// The nested `usage` block, else the top-level `total_audio_seconds`
/// (older servers); std::nullopt when neither is present.
std::optional<SessionUsage> usage_from_payload(const json& j);
/// Decode an audio frame (`audio`, `enc`, `idx`, `sr`, `samples`, ...).
AudioChunk audio_chunk_from_json(const json& j);
TurnFinal turn_final_from_json(const json& j);
SessionStats session_stats_from_json(const json& j);
GenerationSettings generation_settings_from_json(const json& j);

json voice_create_metadata(const VoiceCreateRequest& request);
json voice_update_body(const VoiceUpdate& update);
json dictionary_create_body(const DictionaryCreate& request);
json dictionary_update_body(const DictionaryUpdate& update);
json entry_input_body(const DictionaryEntryInput& entry);
json entry_update_body(const DictionaryEntryUpdate& update);

/// The request body of REST generate and `/ws/tts` (cfg_scale clamped).
/// `include_word_timestamps` is false for REST, which rejects the field.
json generate_body(const GenerateRequest& request, bool include_word_timestamps);
/// The sticky config fields of `/ws/tts/stream` (cfg_scale clamped).
json stream_config_body(const StreamConfig& config);
/// The `update_settings` body.
/// @throws ValidationError when `settings` is empty.
json settings_body(const GenerationSettings& settings);

}  // namespace kugelaudio::detail
