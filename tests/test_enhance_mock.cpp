// Speech-enhancement streaming against a scripted WebSocket server.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "kugelaudio/kugelaudio.hpp"
#include "mock_server.hpp"

namespace ka = kugelaudio;
using ka::testing::MockWsServer;
using ka::testing::WsPeer;
using nlohmann::json;

namespace {

/// Echoes audio back as "enhanced"; `ready` carries a request id in session mode.
MockWsServer::OnMessage enhance_server() {
  return [](WsPeer& peer, const std::string& message, bool binary) {
    if (binary) {
      peer.send_binary(message);
      return;
    }
    const json data = json::parse(message);
    const std::string type = data.value("type", std::string());
    if (type == "config") {
      if (data.value("model", std::string()) != "clarity-1") {
        peer.send(
            R"({"type":"error","error":"Invalid request","error_code":"VALIDATION_ERROR","code":400})");
        peer.close(4000);
        return;
      }
      json ready{{"type", "ready"}, {"sample_rate_hz", 24000}, {"encoding", "pcm_s16le"}};
      if (peer.uri.find("session=1") != std::string::npos) ready["request_id"] = "r1";
      peer.send(ready.dump());
    } else if (type == "end") {
      peer.send(R"({"type":"done","duration_s":0.1})");
      if (peer.uri.find("session=1") == std::string::npos) peer.close(1000);
    }
  };
}

ka::AudioStream two_chunks() {
  return ka::AudioStream(ka::Bytes(16000 * 2 / 5, 1), 16000, 0.1);  // 0.2 s -> two chunks
}

}  // namespace

TEST(EnhanceMock, StreamSendsConfigAudioAndEnd) {
  MockWsServer server(enhance_server());
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::EnhanceOptions options;
  options.model = "clarity-1";
  options.speaker = ka::load_audio(ka::to_wav(ka::Bytes(8, 0), 16000), "speaker.wav");
  ka::Bytes enhanced;
  client.enhance().stream(two_chunks(), options, [&](const ka::Bytes& pcm) {
    enhanced.insert(enhanced.end(), pcm.begin(), pcm.end());
  });
  EXPECT_EQ(enhanced.size(), 6400u);

  const auto received = server.received();
  ASSERT_EQ(received.size(), 4u);  // config, two chunks, end
  const json config = json::parse(received[0]);
  EXPECT_EQ(config["task"], "target_speaker_extraction");
  EXPECT_EQ(config["sample_rate_hz"], 16000);
  EXPECT_EQ(config["encoding"], "pcm_s16le");
  EXPECT_TRUE(config.contains("speaker_wav_b64"));
  EXPECT_EQ(json::parse(received[3]), json({{"type", "end"}}));
  EXPECT_EQ(server.uris().at(0).rfind("/v1/audio/enhance/stream?sdk=cpp", 0), 0u);
}

TEST(EnhanceMock, RawSourceSplitsChunksLongerThanOneSecond) {
  MockWsServer server(enhance_server());
  ka::Client client(ka::testing::mock_options(server.url()));
  bool sent = false;
  ka::PcmSource source = [&]() -> std::optional<ka::Bytes> {
    if (sent) return std::nullopt;
    sent = true;
    return ka::Bytes(8000 * 2 * 3 / 2, 2);  // 1.5 s at 8 kHz
  };
  std::size_t total = 0;
  client.enhance().stream(source, 8000, {"clarity-1", std::nullopt},
                          [&](const ka::Bytes& pcm) { total += pcm.size(); });
  EXPECT_EQ(total, 24000u);
  EXPECT_EQ(server.received().size(), 4u);  // config, 1 s, 0.5 s, end
}

TEST(EnhanceMock, ConfigErrorIsTyped) {
  MockWsServer server(enhance_server());
  ka::Client client(ka::testing::mock_options(server.url()));
  EXPECT_THROW(
      client.enhance().stream(two_chunks(), {"other-model", std::nullopt}, [](const ka::Bytes&) {}),
      ka::ValidationError);
  EXPECT_THROW(client.enhance().stream(two_chunks(), {"", std::nullopt}, [](const ka::Bytes&) {}),
               ka::ValidationError);
}

TEST(EnhanceMock, SessionReusesOneSocket) {
  MockWsServer server(enhance_server());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.enhance().session();
  session.connect();
  std::size_t total = 0;
  for (int i = 0; i < 2; ++i) {
    session.stream(two_chunks(), {"clarity-1", std::nullopt},
                   [&](const ka::Bytes& pcm) { total += pcm.size(); });
  }
  EXPECT_EQ(total, 12800u);
  EXPECT_EQ(server.connections(), 1);
  EXPECT_NE(server.uris().at(0).find("session=1"), std::string::npos);
  session.close();
  EXPECT_THROW(session.stream(two_chunks(), {"clarity-1", std::nullopt}, [](const ka::Bytes&) {}),
               ka::KugelAudioError);
}
