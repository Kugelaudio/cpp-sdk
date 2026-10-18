// Opt-in live check against production: runs only when KUGELAUDIO_API_KEY is
// set (KUGELAUDIO_VOICE_ID overrides the voice, default 1071). CI never sets it.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "kugelaudio/kugelaudio.hpp"

namespace ka = kugelaudio;

namespace {

const char* api_key() {
  const char* key = std::getenv("KUGELAUDIO_API_KEY");
  return key != nullptr && *key != '\0' ? key : nullptr;
}

std::int64_t voice_id() {
  const char* voice = std::getenv("KUGELAUDIO_VOICE_ID");
  return voice != nullptr && *voice != '\0' ? std::stoll(voice) : 1071;
}

}  // namespace

TEST(Live, GenerateOneSentence) {
  if (api_key() == nullptr) GTEST_SKIP() << "KUGELAUDIO_API_KEY is not set";
  ka::Client client(api_key());
  ka::GenerateRequest request;
  request.text = "Hello from the KugelAudio C++ SDK.";
  request.voice_id = voice_id();
  request.language = "en";
  const auto audio = client.tts().generate(request);
  EXPECT_EQ(audio.encoding, "pcm_s16le");
  EXPECT_GT(audio.duration_ms, 500.0);
  EXPECT_TRUE(audio.request_id.has_value());
}

TEST(Live, StreamOneTurn) {
  if (api_key() == nullptr) GTEST_SKIP() << "KUGELAUDIO_API_KEY is not set";
  ka::Client client(api_key());
  ka::StreamConfig config;
  config.voice_id = voice_id();
  config.language = "en";
  auto session = client.tts().streaming_session(config);
  std::size_t bytes = 0;
  const auto count = [&bytes](const ka::AudioChunk& chunk) { bytes += chunk.audio.size(); };
  session.send("Streaming one turn ", false, count);
  session.send("from the C++ SDK.", false, count);
  session.flush(count);
  EXPECT_GT(bytes, 24000u);  // more than half a second of PCM16 at 24 kHz
  ASSERT_TRUE(session.last_usage().has_value());
  EXPECT_GT(session.last_usage()->audio_seconds, 0.5);
  session.close();
}
