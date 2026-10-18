// Streaming-input sessions on /ws/tts/stream against a scripted server.

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "base64.hpp"
#include "kugelaudio/kugelaudio.hpp"
#include "mock_server.hpp"

namespace ka = kugelaudio;
using ka::testing::MockWsServer;
using ka::testing::WsPeer;
using nlohmann::json;

namespace {

std::string audio(int idx) {
  return json{{"audio", ka::detail::base64_encode({static_cast<std::uint8_t>(idx), 0})},
              {"enc", "pcm_s16le"},
              {"idx", idx},
              {"sr", 24000},
              {"samples", 1},
              {"chunk_id", 0}}
      .dump();
}

void end_turn(WsPeer& peer, int idx) {
  peer.send(audio(idx));
  peer.send(R"({"chunk_complete":true,"chunk_id":0,"audio_seconds":0.1,"gen_ms":5})");
  peer.send(
      R"({"final":true,"total_audio_seconds":0.1,"total_text_chunks":1,"total_audio_chunks":1})");
  peer.send(
      R"({"session_closed":true,"total_audio_seconds":0.1,"total_text_chunks":1,"total_audio_chunks":1,"usage":{"audio_seconds":0.1,"characters":12,"cost_cents":0.02,"currency":"eur","model_id":"kugel-3.5"}})");
}

/// A server that speaks the turn protocol: sentences end with ".", a flush
/// or close ends the turn, cancel is acknowledged.
MockWsServer::OnMessage turn_server() {
  return [](WsPeer& peer, const std::string& message, bool) {
    const json data = json::parse(message);
    if (data.contains("update_settings")) {
      peer.send(json{{"settings_updated", true}, {"settings", data["update_settings"]}}.dump());
      return;
    }
    if (data.value("cancel", false)) {
      peer.send(R"({"interrupted":true})");
      return;
    }
    if (data.value("close", false) || data.value("flush", false)) {
      end_turn(peer, 7);
      return;
    }
    const std::string text = data.value("text", std::string());
    if (!text.empty() && text.back() == '.') {
      peer.send(R"({"generation_started":true,"chunk_id":0,"text":"..."})");
      peer.send(audio(1));
      peer.send(R"({"chunk_complete":true,"chunk_id":0})");
    }
  };
}

ka::StreamConfig config() {
  ka::StreamConfig c;
  c.voice_id = 1071;
  c.language = "en";
  return c;
}

}  // namespace

TEST(StreamingSessionMock, TokensThenFlushEndTheTurn) {
  MockWsServer server(turn_server());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().streaming_session(config());
  session.connect();
  EXPECT_TRUE(session.send("Hello, ").empty());  // no sentence yet: returns after a short poll
  const auto sentence = session.send("world.");
  ASSERT_EQ(sentence.size(), 1u);
  EXPECT_EQ(sentence[0].index, 1);
  const auto tail = session.flush();
  ASSERT_EQ(tail.size(), 1u);
  ASSERT_TRUE(session.last_usage().has_value());
  EXPECT_DOUBLE_EQ(*session.last_usage()->cost_cents, 0.02);
  ASSERT_TRUE(session.last_final().has_value());
  EXPECT_EQ(session.last_final()->total_audio_chunks, 1);

  // Turn two on the same socket; the config is sticky and not resent.
  EXPECT_EQ(session.send("Again.", true).size(), 1u);
  EXPECT_EQ(server.connections(), 1);

  const auto received = server.received();
  const json first = json::parse(received.at(0));
  EXPECT_EQ(first["text"], "Hello, ");
  EXPECT_EQ(first["voice_id"], 1071);
  EXPECT_EQ(first["language"], "en");
  EXPECT_EQ(first["flush_timeout_ms"], 500);
  EXPECT_FALSE(json::parse(received.at(1)).contains("voice_id"));
  EXPECT_EQ(json::parse(received.at(2)), json({{"flush", true}}));
  EXPECT_EQ(server.uris().at(0).rfind("/ws/tts/stream?sdk=cpp", 0), 0u);
  session.close();
  EXPECT_FALSE(session.is_connected());
}

TEST(StreamingSessionMock, DrainKeepsTheTail) {
  MockWsServer server(turn_server());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().streaming_session(config());
  session.send("Partial");
  const auto tail = session.drain();
  ASSERT_EQ(tail.size(), 1u);
  EXPECT_EQ(tail[0].index, 7);
  const auto stats = session.end_session();  // returns what drain captured
  EXPECT_TRUE(stats.received);
  ASSERT_TRUE(stats.usage.has_value());
  EXPECT_EQ(stats.usage->characters, 12);
  EXPECT_EQ(json::parse(server.received().back()), json({{"close", true}}));
}

TEST(StreamingSessionMock, CancelFromAnotherThreadAndConfigIsResent) {
  std::atomic<bool> hold{true};
  MockWsServer server([&](WsPeer& peer, const std::string& message, bool) {
    const json data = json::parse(message);
    if (data.value("cancel", false)) {
      peer.send(R"({"interrupted":true})");
      return;
    }
    if (data.value("flush", false) && hold.exchange(false)) {
      peer.send(R"({"generation_started":true})");
      peer.send(audio(1));  // then keep generating until cancelled
      return;
    }
    end_turn(peer, 2);
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().streaming_session(config());
  std::thread barge([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    session.cancel();
  });
  const auto before = std::chrono::steady_clock::now();
  const auto chunks = session.send("A long answer that the user interrupts.", true);
  barge.join();
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(5));
  EXPECT_EQ(chunks.size(), 1u);

  const auto next = session.send("Next turn.", true);
  EXPECT_EQ(next.size(), 1u);
  const auto received = server.received();
  EXPECT_EQ(json::parse(received.at(1)), json({{"cancel", true}}));
  EXPECT_EQ(json::parse(received.at(2))["voice_id"], 1071);  // config resent after a barge-in
}

TEST(StreamingSessionMock, RestartBeforeAudioIsReplayed) {
  MockWsServer server([](WsPeer& peer, const std::string& message, bool) {
    if (peer.index == 0) {
      peer.close(1012, "restart");
      return;
    }
    const json data = json::parse(message);
    if (data.value("flush", false) || data.value("close", false)) end_turn(peer, 3);
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().streaming_session(config());
  const auto chunks = session.send("Hello world.", true);
  ASSERT_EQ(chunks.size(), 1u);
  EXPECT_EQ(chunks[0].index, 3);
  EXPECT_EQ(server.connections(), 2);
  const auto received = server.received();
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(json::parse(received[1])["text"], "Hello world.");
  EXPECT_EQ(json::parse(received[1])["voice_id"], 1071);  // config replayed with the turn
}

TEST(StreamingSessionMock, RestartAfterAudioRaises) {
  MockWsServer server([](WsPeer& peer, const std::string&, bool) {
    peer.send(audio(1));
    peer.close(1012, "restart");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().streaming_session(config());
  std::vector<ka::AudioChunk> chunks;
  EXPECT_THROW(session.send("Hello world.", true,
                            [&](const ka::AudioChunk& chunk) { chunks.push_back(chunk); }),
               ka::ServerRestartingError);
  EXPECT_EQ(chunks.size(), 1u);
  EXPECT_EQ(server.connections(), 1);
}

TEST(StreamingSessionMock, ErrorFrameAndUpdateSettings) {
  MockWsServer server([](WsPeer& peer, const std::string& message, bool) {
    const json data = json::parse(message);
    if (data.contains("update_settings")) {
      peer.send(json{{"settings_updated", true}, {"settings", data["update_settings"]}}.dump());
      return;
    }
    peer.send(
        R"({"error":"Rate limit exceeded","error_code":"RATE_LIMITED","code":429,"retry_after":4})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().streaming_session(config());
  try {
    session.send("Hi.", true);
    FAIL() << "expected RateLimitError";
  } catch (const ka::RateLimitError& e) {
    EXPECT_EQ(e.retry_after(), 4);
  }
  ka::GenerationSettings settings;
  settings.cfg_scale = 1.5;
  settings.temperature = 0.3;
  const auto effective = session.update_settings(settings);
  EXPECT_EQ(effective.cfg_scale, 1.5);
  EXPECT_EQ(effective.temperature, 0.3);
  EXPECT_THROW(session.update_settings({}), ka::ValidationError);
}

TEST(StreamingSessionMock, WarningAndWordTimestampCallbacks) {
  MockWsServer server([](WsPeer& peer, const std::string&, bool) {
    peer.send(R"({"warning":"Turn ended after 5s of inactivity."})");
    peer.send(audio(1));
    peer.send(
        R"({"word_timestamps":[{"word":"Hi","start_ms":0,"end_ms":50,"char_start":0,"char_end":2}]})");
    peer.send(R"({"final":true})");
    peer.send(R"({"session_closed":true,"total_audio_seconds":0.05})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  std::vector<std::string> warnings;
  std::vector<ka::WordTimestamp> stamps;
  ka::StreamingSessionCallbacks callbacks;
  callbacks.on_warning = [&](const std::string& text) { warnings.push_back(text); };
  callbacks.on_word_timestamps = [&](const std::vector<ka::WordTimestamp>& s) { stamps = s; };
  auto session = client.tts().streaming_session(config(), callbacks);
  session.send("Hi", true);
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_EQ(stamps.at(0).word, "Hi");
  EXPECT_EQ(session.last_word_timestamps().size(), 1u);
  ASSERT_TRUE(session.last_usage().has_value());  // legacy total_audio_seconds
  EXPECT_DOUBLE_EQ(session.last_usage()->audio_seconds, 0.05);
}
