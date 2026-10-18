// JSON mapping of requests and responses, frame decoding, base64 and WAV.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "base64.hpp"
#include "json_mapping.hpp"
#include "kugelaudio/kugelaudio.hpp"
#include "wav.hpp"

namespace ka = kugelaudio;
namespace kd = kugelaudio::detail;
using nlohmann::json;

TEST(Mapping, GenerateBodyOmitsUnsetAndClampsCfg) {
  ka::GenerateRequest request;
  request.text = "Hallo";
  request.voice_id = 1071;
  request.cfg_scale = 9.0;
  const json rest = kd::generate_body(request, false);
  EXPECT_EQ(rest["cfg_scale"], 2.5);
  EXPECT_EQ(rest["model_id"], "kugel-3.5");
  EXPECT_EQ(rest["voice_id"], 1071);
  EXPECT_FALSE(rest.contains("word_timestamps"));
  EXPECT_FALSE(rest.contains("language"));
  EXPECT_FALSE(rest.contains("dictionary_ids"));
  EXPECT_FALSE(rest.contains("temperature"));

  request.cfg_scale = 0.1;
  request.dictionary_ids = std::vector<std::int64_t>{};
  request.temperature = 0.0;
  request.word_timestamps = true;
  const json ws = kd::generate_body(request, true);
  EXPECT_EQ(ws["cfg_scale"], 1.2);
  EXPECT_TRUE(ws["dictionary_ids"].is_array());
  EXPECT_TRUE(ws["dictionary_ids"].empty());  // [] is an explicit opt-out
  EXPECT_EQ(ws["temperature"], 0.0);
  EXPECT_EQ(ws["word_timestamps"], true);
}

TEST(Mapping, StreamConfigBody) {
  ka::StreamConfig config;
  config.voice_id = 7;
  config.language = "de";
  config.chunk_length_schedule = std::vector<int>{50, 100};
  const json body = kd::stream_config_body(config);
  EXPECT_EQ(body["voice_id"], 7);
  EXPECT_EQ(body["flush_timeout_ms"], 500);
  EXPECT_EQ(body["max_buffer_length"], 1000);
  EXPECT_EQ(body["chunk_length_schedule"], json::array({50, 100}));
  EXPECT_FALSE(body.contains("model_id"));
}

TEST(Mapping, SettingsBodyRejectsEmpty) {
  EXPECT_THROW(kd::settings_body({}), ka::ValidationError);
  ka::GenerationSettings settings;
  settings.speed = 1.1;
  settings.normalize = false;
  const json body = kd::settings_body(settings);
  EXPECT_EQ(body, json({{"speed", 1.1}, {"normalize", false}}));
}

TEST(Mapping, VoiceResponses) {
  const json list = json::parse(R"({
    "voices": [{"id": 1071, "name": "Anna", "category": "premade", "sex": "female",
                "supported_languages": ["de", "en"], "is_public": true, "verified": true}],
    "total": 1, "limit": 20, "offset": 0})");
  const auto parsed = kd::voice_list_from_json(list);
  ASSERT_EQ(parsed.voices.size(), 1u);
  EXPECT_EQ(parsed.voices[0].id, 1071);
  EXPECT_EQ(parsed.voices[0].category, "premade");
  EXPECT_EQ(parsed.voices[0].supported_languages.size(), 2u);
  EXPECT_EQ(parsed.voices[0].quality, "mid");
  EXPECT_FALSE(parsed.voices[0].description.has_value());
  EXPECT_EQ(parsed.total, 1);

  // An identity field is never invented.
  EXPECT_THROW(kd::voice_from_json(json{{"name", "x"}}), ka::KugelAudioError);
  EXPECT_THROW(kd::voice_list_from_json(json{{"voices", json::array()}}), ka::KugelAudioError);
}

TEST(Mapping, DictionaryResponses) {
  const auto entry = kd::dictionary_entry_from_json(json::parse(
      R"({"id":3,"dictionary_id":1,"word":"K8s","replacement":"kubernetes","ipa":null})"));
  EXPECT_EQ(entry.word, "K8s");
  EXPECT_FALSE(entry.ipa.has_value());
  EXPECT_FALSE(entry.case_sensitive);
  const auto result =
      kd::bulk_replace_from_json(json{{"upserted", 2}, {"deleted", 1}, {"total", 2}});
  EXPECT_EQ(result.deleted, 1);
  const json input = kd::entry_input_body({"Postgres", "post-gres", std::nullopt, true});
  EXPECT_EQ(input,
            json({{"word", "Postgres"}, {"replacement", "post-gres"}, {"case_sensitive", true}}));
}

TEST(Frames, AudioChunkDecoding) {
  const json frame = json::parse(
      R"({"audio":"AAECAw==","enc":"pcm_s16le","idx":4,"sr":24000,"samples":2,"chunk_id":1,"context_id":"a"})");
  const auto chunk = kd::audio_chunk_from_json(frame);
  EXPECT_EQ(chunk.audio, (ka::Bytes{0, 1, 2, 3}));
  EXPECT_EQ(chunk.index, 4);
  EXPECT_EQ(chunk.samples, 2);
  EXPECT_EQ(chunk.chunk_id, 1);
  EXPECT_EQ(chunk.context_id, "a");
  EXPECT_DOUBLE_EQ(chunk.duration_seconds(), 2.0 / 24000);
  EXPECT_THROW(kd::audio_chunk_from_json(json{{"audio", "AA=="}}), ka::KugelAudioError);
}

TEST(Frames, WordTimestampsAndUsage) {
  const auto stamps = kd::word_timestamps_from_json(json::parse(
      R"([{"word":"Hello","start_ms":0,"end_ms":320,"char_start":0,"char_end":5,"score":1.0}])"));
  ASSERT_EQ(stamps.size(), 1u);
  EXPECT_EQ(stamps[0].duration_ms(), 320);

  const auto usage = kd::usage_from_payload(json::parse(
      R"({"session_closed":true,"usage":{"audio_seconds":5.4,"characters":142,"cost_cents":null,"cost_unavailable":true}})"));
  ASSERT_TRUE(usage.has_value());
  EXPECT_DOUBLE_EQ(usage->audio_seconds, 5.4);
  EXPECT_FALSE(usage->cost_available());
  EXPECT_EQ(usage->characters, 142);

  const auto legacy = kd::usage_from_payload(json{{"total_audio_seconds", 2.5}});
  ASSERT_TRUE(legacy.has_value());
  EXPECT_DOUBLE_EQ(legacy->audio_seconds, 2.5);
  EXPECT_FALSE(kd::usage_from_payload(json{{"final", true}}).has_value());
}

TEST(Base64, RoundTrip) {
  for (std::size_t n = 0; n < 10; ++n) {
    ka::Bytes data;
    for (std::size_t i = 0; i < n; ++i) data.push_back(static_cast<std::uint8_t>(i * 37 + 1));
    EXPECT_EQ(kd::base64_decode(kd::base64_encode(data)), data);
  }
  EXPECT_EQ(kd::base64_encode({'M', 'a', 'n'}), "TWFu");
  EXPECT_THROW(kd::base64_decode("!!!"), ka::KugelAudioError);
}

TEST(Wav, WriteAndParsePcm16) {
  const ka::Bytes pcm{1, 0, 2, 0, 3, 0, 4, 0};
  const ka::Bytes wav = ka::to_wav(pcm, 16000);
  ASSERT_EQ(wav.size(), 44u + pcm.size());
  const auto info = kd::parse_wav(wav);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->format, 1);
  EXPECT_EQ(info->channels, 1);
  EXPECT_EQ(info->sample_rate, 16000);
  EXPECT_EQ(info->bits_per_sample, 16);
  EXPECT_EQ(info->data_size, pcm.size());

  const auto mulaw = ka::to_wav({0xFF, 0x7F}, 8000, ka::encoding::kMulaw);
  const auto mulaw_info = kd::parse_wav(mulaw);
  ASSERT_TRUE(mulaw_info.has_value());
  EXPECT_EQ(mulaw_info->format, 7);
  EXPECT_EQ(mulaw_info->bits_per_sample, 8);
  EXPECT_EQ(ka::bytes_per_sample("alaw"), 1);
  EXPECT_THROW(ka::bytes_per_sample("mp3"), ka::ValidationError);
}

TEST(Wav, LoadAudioAndStream) {
  // Stereo PCM16: frames (100, 300) and (-3, 0) average to 200 and -2 (floor).
  ka::Bytes stereo_pcm{100, 0, 44, 1, 0xFD, 0xFF, 0, 0};
  ka::Bytes wav = ka::to_wav(stereo_pcm, 8000);
  wav[22] = 2;  // channels
  wav[32] = 4;  // block align
  wav[28] = 0x00;
  wav[29] = 0x7D;
  wav[30] = 0;
  wav[31] = 0;  // byte rate 32000
  const auto file = ka::load_audio(wav, "stereo.wav");
  ASSERT_TRUE(file.duration.has_value());
  EXPECT_DOUBLE_EQ(*file.duration, 2.0 / 8000);
  EXPECT_EQ(file.filename, "stereo.wav");

  const auto stream = ka::load_audio_stream(wav, 0.1);
  EXPECT_EQ(stream.sample_rate(), 8000);
  EXPECT_EQ(stream.pcm(), (ka::Bytes{200, 0, 0xFE, 0xFF}));
  EXPECT_EQ(stream.chunks().size(), 1u);

  EXPECT_THROW(ka::load_audio(ka::Bytes{}), ka::ValidationError);
  EXPECT_THROW(ka::load_audio_stream(wav, 0.0), ka::ValidationError);
  EXPECT_THROW(ka::load_audio_stream(ka::Bytes{1, 2, 3}), ka::ValidationError);
  EXPECT_THROW(ka::load_audio(std::filesystem::path("/nonexistent/kugelaudio.wav")),
               ka::ValidationError);
}

TEST(Wav, AudioStreamChunks) {
  ka::Bytes pcm(16000 * 2 * 3 / 2, 0);  // 1.5 s at 16 kHz
  ka::AudioStream stream(pcm, 16000, 1.0);
  const auto chunks = stream.chunks();
  ASSERT_EQ(chunks.size(), 2u);
  EXPECT_EQ(chunks[0].size(), 32000u);
  EXPECT_EQ(chunks[1].size(), 16000u);
  EXPECT_DOUBLE_EQ(stream.duration(), 1.5);
}

TEST(Wav, EnhancedAudioRoundTrip) {
  ka::EnhancedAudio audio{{1, 0, 2, 0}, 24000};
  const auto parsed = ka::EnhancedAudio::from_wav(audio.wav());
  EXPECT_EQ(parsed.audio, audio.audio);
  EXPECT_EQ(parsed.sample_rate, 24000);
  EXPECT_THROW(ka::EnhancedAudio::from_wav({1, 2, 3}), ka::KugelAudioError);

  const auto path = std::filesystem::temp_directory_path() / "kugelaudio-test-enhanced.wav";
  audio.save(path);
  EXPECT_EQ(std::filesystem::file_size(path), 48u);
  std::filesystem::remove(path);
}

TEST(Wav, AudioResponseSave) {
  ka::AudioResponse response;
  response.audio = {0, 0, 1, 0};
  response.sample_rate = 22050;
  const auto wav = response.to_wav();
  const auto info = kd::parse_wav(wav);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->sample_rate, 22050);
}
