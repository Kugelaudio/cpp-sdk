// One-shot streaming on /ws/tts against a local WebSocket server.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <nlohmann/json.hpp>

#include "base64.hpp"
#include "kugelaudio/kugelaudio.hpp"
#include "mock_server.hpp"

namespace ka = kugelaudio;
using ka::testing::MockHttpServer;
using ka::testing::MockWsServer;
using ka::testing::WsPeer;
using nlohmann::json;

namespace {

std::string audio_frame(int idx, const ka::Bytes& pcm, const char* context = nullptr) {
  json frame{{"audio", ka::detail::base64_encode(pcm)},
             {"enc", "pcm_s16le"},
             {"idx", idx},
             {"sr", 24000},
             {"samples", static_cast<int>(pcm.size() / 2)},
             {"chunk_id", 0}};
  if (context != nullptr) frame["context_id"] = context;
  return frame.dump();
}

}  // namespace

TEST(TtsStreamMock, StreamsChunksTimestampsAndUsageOnPooledSocket) {
  MockWsServer server([](WsPeer& peer, const std::string& message, bool) {
    const json request = json::parse(message);
    if (!request.contains("text")) return;
    peer.send(audio_frame(0, {1, 0, 2, 0}));
    peer.send(audio_frame(1, {3, 0}));
    peer.send(
        R"({"word_timestamps":[{"word":"Hi","start_ms":0,"end_ms":100,"char_start":0,"char_end":2}],"chunk_id":0})");
    peer.send(
        R"({"final":true,"chunks":2,"total_samples":3,"dur_ms":0.125,"gen_ms":10,"rtf":0.08,"usage":{"audio_seconds":0.000125,"characters":2,"cost_cents":0.01,"currency":"eur","model_id":"kugel-3.5"}})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "Hi";
  request.voice_id = 1071;
  request.language = "en";
  request.word_timestamps = true;

  std::vector<ka::AudioChunk> chunks;
  std::vector<ka::WordTimestamp> stamps;
  ka::StreamCallbacks callbacks;
  callbacks.on_audio = [&](const ka::AudioChunk& chunk) { chunks.push_back(chunk); };
  callbacks.on_word_timestamps = [&](const std::vector<ka::WordTimestamp>& s) { stamps = s; };
  const auto result = client.tts().stream(request, callbacks);
  ASSERT_EQ(chunks.size(), 2u);
  EXPECT_EQ(result.audio, (ka::Bytes{1, 0, 2, 0, 3, 0}));
  EXPECT_EQ(result.samples, 3);
  EXPECT_DOUBLE_EQ(result.generation_ms, 10.0);
  ASSERT_TRUE(result.usage.has_value());
  EXPECT_DOUBLE_EQ(*result.usage->cost_cents, 0.01);
  EXPECT_EQ(result.word_timestamps.size(), 1u);
  EXPECT_EQ(stamps.at(0).word, "Hi");
  EXPECT_FALSE(result.interrupted);
  EXPECT_TRUE(client.is_connected());

  // The second request reuses the pooled socket.
  client.tts().stream(request);
  EXPECT_EQ(server.connections(), 1);

  const auto uri = server.uris().at(0);
  EXPECT_EQ(uri.rfind("/ws/tts?model_id=kugel-3.5&sdk=cpp&sdk_version=", 0), 0u) << uri;
  EXPECT_EQ(uri.find("api_key"), std::string::npos);
  EXPECT_EQ(server.first_headers().at("authorization"), "Bearer test-key");
  const json sent = json::parse(server.received().at(0));
  EXPECT_EQ(sent["word_timestamps"], true);
  EXPECT_EQ(sent["voice_id"], 1071);
}

TEST(TtsStreamMock, ConnectWarmsTheSocket) {
  MockWsServer server([](WsPeer&, const std::string&, bool) {});
  ka::Client client(ka::testing::mock_options(server.url()));
  EXPECT_FALSE(client.is_connected());
  client.connect();
  EXPECT_TRUE(client.is_connected());
  EXPECT_EQ(server.connections(), 1);
  client.tts().disconnect();
  EXPECT_FALSE(client.is_connected());
}

TEST(TtsStreamMock, ErrorFrameIsTypedAndSocketStaysUsable) {
  std::atomic<int> requests{0};
  MockWsServer server([&](WsPeer& peer, const std::string&, bool) {
    if (requests++ == 0) {
      peer.send(
          R"({"error":"voice_id is required","error_code":"MISSING_VOICE_ID","code":400,"request_id":"ws-req"})");
      return;
    }
    peer.send(audio_frame(0, {9, 0}));
    peer.send(R"({"final":true})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "No voice";
  try {
    client.tts().stream(request);
    FAIL() << "expected ValidationError";
  } catch (const ka::ValidationError& e) {
    EXPECT_EQ(e.error_code(), "MISSING_VOICE_ID");
    EXPECT_EQ(e.request_id(), "ws-req");
  }
  request.voice_id = 1;
  EXPECT_EQ(client.tts().stream(request).audio, (ka::Bytes{9, 0}));
  EXPECT_EQ(server.connections(), 1);
}

TEST(TtsStreamMock, CloseCodeIsTypedAndPoolIsDropped) {
  MockWsServer server([](WsPeer& peer, const std::string&, bool) { peer.close(4003, "credits"); });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "Hello";
  request.voice_id = 1;
  EXPECT_THROW(client.tts().stream(request), ka::InsufficientCreditsError);
  EXPECT_FALSE(client.is_connected());
}

TEST(TtsStreamMock, CancelEndsTheStreamWithInterrupted) {
  MockWsServer server([](WsPeer& peer, const std::string& message, bool) {
    const json data = json::parse(message);
    if (data.value("cancel", false)) {
      peer.send(R"({"interrupted":true})");
      return;
    }
    peer.send(audio_frame(0, {1, 0}));  // then keep "generating" until cancelled
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "A long answer";
  request.voice_id = 1;
  ka::StreamCallbacks callbacks;
  callbacks.on_audio = [&](const ka::AudioChunk&) { client.tts().cancel(); };
  const auto result = client.tts().stream(request, callbacks);
  EXPECT_TRUE(result.interrupted);
  EXPECT_EQ(result.audio.size(), 2u);

  // Barge-in from another thread while the stream waits for audio.
  std::thread barge([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.tts().cancel();
  });
  const auto second = client.tts().stream(request);
  barge.join();
  EXPECT_TRUE(second.interrupted);
  EXPECT_EQ(server.connections(), 1);
}

TEST(TtsStreamMock, UpdateSettingsReturnsEffectiveSettings) {
  MockWsServer server([](WsPeer& peer, const std::string& message, bool) {
    const json data = json::parse(message);
    if (data.contains("update_settings")) {
      json settings = data["update_settings"];
      settings["max_new_tokens"] = 2048;
      peer.send(json{{"settings_updated", true}, {"settings", settings}}.dump());
    }
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerationSettings settings;
  settings.speed = 1.1;
  settings.language = "de";
  const auto effective = client.tts().update_settings(settings);
  EXPECT_EQ(effective.speed, 1.1);
  EXPECT_EQ(effective.language, "de");
  EXPECT_EQ(effective.max_new_tokens, 2048);
  EXPECT_THROW(client.tts().update_settings({}), ka::ValidationError);
}

// Regression: `samples` came from the final frame's `total_samples`, which
// counts native 24 kHz samples, so 8 kHz / mu-law streams reported a 3x
// (or 1.5x at 16 kHz) duration_seconds(); `dur_ms` is native too, and both
// ignore `speed`.
TEST(TtsStreamMock, SamplesCountTheDeliveredRateNotTheNativeOne) {
  MockWsServer server([](WsPeer& peer, const std::string&, bool) {
    const ka::Bytes mulaw{0xFF, 0x7F, 0x80, 0x00};
    peer.send(json{{"audio", ka::detail::base64_encode(mulaw)},
                   {"enc", "mulaw"},
                   {"idx", 0},
                   {"sr", 8000},
                   {"samples", 4}}
                  .dump());
    peer.send(R"({"final":true,"chunks":1,"total_samples":12,"dur_ms":1.5})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "Telephony";
  request.voice_id = 1;
  request.language = "en";
  request.output_format = "ulaw_8000";
  const auto result = client.tts().stream(request);
  EXPECT_EQ(result.encoding, "mulaw");
  EXPECT_EQ(result.sample_rate, 8000);
  EXPECT_EQ(result.samples, 4);
  EXPECT_DOUBLE_EQ(result.duration_seconds(), 4.0 / 8000);
  EXPECT_DOUBLE_EQ(result.duration_ms, 0.5);
}

// Regression: stream() always sent cfg_scale, speed, max_new_tokens and
// normalize, so the server's sticky defaults from update_settings never
// applied, and they were lost when the pooled socket reconnected.
TEST(TtsStreamMock, UpdateSettingsAppliesToLaterPooledStreams) {
  MockWsServer server([](WsPeer& peer, const std::string& message, bool) {
    const json data = json::parse(message);
    if (data.contains("update_settings")) {
      peer.send(json{{"settings_updated", true}, {"settings", data["update_settings"]}}.dump());
      return;
    }
    peer.send(audio_frame(0, {1, 0}));
    peer.send(R"({"final":true})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerationSettings settings;
  settings.speed = 1.2;
  settings.cfg_scale = 1.5;
  settings.language = "de";
  client.tts().update_settings(settings);

  ka::GenerateRequest request;
  request.text = "Hallo";
  request.voice_id = 1;
  client.tts().stream(request);  // fields at their defaults take the sticky values
  ka::GenerateRequest own = request;
  own.speed = 0.9;
  own.language = "en";
  client.tts().stream(own);  // explicit values win
  client.tts().disconnect();
  client.tts().stream(request);  // a reconnected pool keeps the sticky values
  ka::StreamOptions separate;
  separate.reuse_connection = false;
  client.tts().stream(request, {}, separate);  // not the pooled connection

  const auto sent = server.received();
  ASSERT_EQ(sent.size(), 5u);
  const json first = json::parse(sent.at(1));
  EXPECT_EQ(first["speed"], 1.2);
  EXPECT_EQ(first["cfg_scale"], 1.5);
  EXPECT_EQ(first["language"], "de");
  const json explicit_values = json::parse(sent.at(2));
  EXPECT_EQ(explicit_values["speed"], 0.9);
  EXPECT_EQ(explicit_values["cfg_scale"], 1.5);
  EXPECT_EQ(explicit_values["language"], "en");
  const json reconnected = json::parse(sent.at(3));
  EXPECT_EQ(reconnected["speed"], 1.2);
  EXPECT_EQ(reconnected["language"], "de");
  const json unpooled = json::parse(sent.at(4));
  EXPECT_EQ(unpooled["speed"], 1.0);
  EXPECT_FALSE(unpooled.contains("language"));
  EXPECT_EQ(server.connections(), 3);
}

TEST(TtsStreamMock, OwnConnectionWhenNotReusing) {
  MockWsServer server([](WsPeer& peer, const std::string&, bool) {
    peer.send(audio_frame(0, {5, 0}));
    peer.send(R"({"final":true})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "Hello";
  request.voice_id = 1;
  ka::StreamOptions options;
  options.reuse_connection = false;
  client.tts().stream(request, {}, options);
  client.tts().stream(request, {}, options);
  EXPECT_EQ(server.connections(), 2);
  EXPECT_FALSE(client.is_connected());
}

#ifndef _WIN32
TEST(TtsStreamMock, HandshakeRejectionIsTyped) {
  const std::string body = R"({"error":"Invalid API key","error_code":"UNAUTHORIZED","code":401})";
  ka::testing::RawHttpResponder server(
      "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nx-request-id: hs-1\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "Hello";
  try {
    client.tts().stream(request);
    FAIL() << "expected AuthenticationError";
  } catch (const ka::AuthenticationError& e) {
    EXPECT_EQ(e.request_id(), "hs-1");
  }
  EXPECT_THROW(client.connect(), ka::AuthenticationError);
}
#endif
