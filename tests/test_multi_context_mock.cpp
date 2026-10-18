// Multi-context sessions on /ws/tts/multi against a scripted server.

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "base64.hpp"
#include "kugelaudio/kugelaudio.hpp"
#include "mock_server.hpp"

namespace ka = kugelaudio;
using ka::testing::MockWsServer;
using ka::testing::WsPeer;
using nlohmann::json;

namespace {

std::string audio(const std::string& context, int idx) {
  return json{{"audio", ka::detail::base64_encode({static_cast<std::uint8_t>(idx), 0})},
              {"enc", "pcm_s16le"},
              {"idx", idx},
              {"sr", 24000},
              {"samples", 1},
              {"chunk_id", 0},
              {"context_id", context}}
      .dump();
}

std::string frame(const char* key, const std::string& context, json extra = json::object()) {
  extra[key] = true;
  extra["context_id"] = context;
  return extra.dump();
}

/// The /ws/tts/multi protocol: context creation, flush -> audio + final,
/// close_context -> (audio + final) + context_closed with usage.
struct MultiServer {
  std::mutex mutex;
  std::map<int, std::set<std::string>> created;      // per connection
  std::function<bool(WsPeer&, const json&)> before;  // return true to stop

  MockWsServer::OnMessage handler() {
    return [this](WsPeer& peer, const std::string& message, bool) {
      const json data = json::parse(message);
      if (before && before(peer, data)) return;
      if (data.contains("update_settings")) {
        peer.send(json{{"settings_updated", true}, {"settings", data["update_settings"]}}.dump());
        return;
      }
      if (data.value("cancel", false)) {
        peer.send(R"({"interrupted":true})");
        return;
      }
      if (data.value("close_socket", false)) {
        peer.send(R"({"session_closed":true,"total_audio_seconds":1.5})");
        return;
      }
      const std::string context = data.value("context_id", std::string());
      if (context.empty()) return;  // voice preparation
      if (data.value("close_context", false)) {
        if (!data.value("immediate", false)) {
          peer.send(audio(context, 9));
          peer.send(frame("final", context));
        }
        peer.send(frame("context_closed", context,
                        {{"usage",
                          {{"audio_seconds", 0.3},
                           {"cost_cents", 0.05},
                           {"currency", "eur"},
                           {"model_id", "kugel-3.5"}}}}));
        std::lock_guard<std::mutex> lock(mutex);
        created[peer.index].erase(context);
        return;
      }
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        fresh = created[peer.index].insert(context).second;
      }
      if (fresh) peer.send(frame("context_created", context));
      if (data.value("flush", false)) {
        peer.send(frame("generation_started", context));
        peer.send(audio(context, 1));
        peer.send(frame("chunk_complete", context));
        peer.send(frame("final", context));
      }
    };
  }
};

ka::MultiContextConfig session_config() {
  ka::MultiContextConfig config;
  config.default_voice_id = 1071;
  config.language = "en";
  config.word_timestamps = true;
  return config;
}

}  // namespace

TEST(MultiContextMock, ContextsSendFlushAndCloseWithUsage) {
  MultiServer script;
  MockWsServer server(script.handler());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().multi_context_session(session_config());
  session.connect();
  session.prepare_voice(1071);
  session.create_context("narrator");
  session.create_context("character", 1072);
  EXPECT_EQ(session.active_contexts(), (std::set<std::string>{"narrator", "character"}));

  const auto narrator = session.send("narrator", "The story begins.", true);
  ASSERT_EQ(narrator.size(), 1u);
  EXPECT_EQ(narrator[0].context_id, "narrator");
  const auto tail = session.close_context("narrator");
  ASSERT_EQ(tail.size(), 1u);
  EXPECT_EQ(tail[0].index, 9);
  ASSERT_TRUE(session.usage_for("narrator").has_value());
  EXPECT_DOUBLE_EQ(*session.usage_for("narrator")->cost_cents, 0.05);
  EXPECT_EQ(session.active_contexts(), (std::set<std::string>{"character"}));

  EXPECT_EQ(session.close(), 1.5);
  EXPECT_FALSE(session.is_alive());

  const auto received = server.received();
  EXPECT_EQ(json::parse(received[0]),
            json({{"text", ""}, {"voice_id", 1071}, {"model_id", "kugel-3.5"}}));
  const json first = json::parse(received[1]);
  EXPECT_EQ(first["context_id"], "narrator");
  EXPECT_EQ(first["voice_settings"]["voice_id"], 1071);
  EXPECT_EQ(first["language"], "en");
  EXPECT_EQ(first["word_timestamps"], true);
  const json second = json::parse(received[2]);
  EXPECT_EQ(second["voice_settings"]["voice_id"], 1072);
  EXPECT_FALSE(second.contains("language"));  // session fields only on the first context
  EXPECT_EQ(json::parse(received.back()), json({{"close_socket", true}}));
}

TEST(MultiContextMock, AudioOfAnotherContextIsKeptForIt) {
  MultiServer script;
  script.before = [](WsPeer& peer, const json& data) {
    if (data.value("context_id", std::string()) == "a" && data.value("flush", false)) {
      // Audio of context b, pre-buffered while a is being served.
      peer.send(audio("b", 5));
    }
    return false;
  };
  MockWsServer server(script.handler());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().multi_context_session(session_config());
  session.create_context("a");
  session.create_context("b");
  const auto a = session.send("a", "First.", true);
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].context_id, "a");
  const auto b = session.close_context("b");
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b[0].index, 5);  // the buffered chunk comes first
  EXPECT_EQ(b[1].index, 9);
}

TEST(MultiContextMock, ImmediateCloseAndCancel) {
  MultiServer script;
  MockWsServer server(script.handler());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().multi_context_session(session_config());
  session.create_context("a");
  EXPECT_TRUE(session.close_context("a", true).empty());
  session.create_context("b");
  session.cancel("b");
  session.cancel();
  ka::GenerationSettings settings;
  settings.speed = 0.9;
  EXPECT_EQ(session.update_settings(settings).speed, 0.9);
  const auto received = server.received();
  bool immediate_seen = false;
  bool cancel_seen = false;
  for (const auto& message : received) {
    const json data = json::parse(message);
    if (data.value("immediate", false)) immediate_seen = true;
    if (data.value("cancel", false) && data.value("context_id", std::string()) == "b") {
      cancel_seen = true;
    }
  }
  EXPECT_TRUE(immediate_seen);
  EXPECT_TRUE(cancel_seen);
}

TEST(MultiContextMock, TooManyContextsIsRateLimit) {
  MultiServer script;
  script.before = [](WsPeer& peer, const json& data) {
    if (data.value("context_id", std::string()) == "overflow") {
      peer.send(
          R"json({"error":"Too many concurrent contexts (max 20)","error_code":"TOO_MANY_CONTEXTS","code":429,"context_id":"overflow"})json");
      return true;
    }
    return false;
  };
  MockWsServer server(script.handler());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().multi_context_session(session_config());
  session.create_context("ok");
  EXPECT_THROW(session.create_context("overflow"), ka::RateLimitError);
  EXPECT_TRUE(session.is_alive());  // a per-context error keeps the socket
}

TEST(MultiContextMock, RestartRestoresContextsAndReplaysTheSilentTurn) {
  MultiServer script;
  script.before = [](WsPeer& peer, const json& data) {
    if (peer.index == 0 && data.value("text", std::string()) == "Hello there.") {
      peer.close(1012, "restart");
      return true;
    }
    return false;
  };
  MockWsServer server(script.handler());
  ka::Client client(ka::testing::mock_options(server.url()));
  auto session = client.tts().multi_context_session(session_config());
  session.create_context("a");
  session.create_context("b", 1072);
  const auto chunks = session.send("a", "Hello there.", true);
  ASSERT_EQ(chunks.size(), 1u);
  EXPECT_EQ(chunks[0].context_id, "a");
  EXPECT_EQ(server.connections(), 2);
  EXPECT_EQ(session.active_contexts(), (std::set<std::string>{"a", "b"}));

  // On the new socket: both contexts re-created (b keeps its voice), then the replay.
  const auto received = server.received();
  std::vector<json> tail;
  for (std::size_t i = received.size() - 3; i < received.size(); ++i) {
    tail.push_back(json::parse(received[i]));
  }
  EXPECT_EQ(tail[0]["context_id"], "a");
  EXPECT_EQ(tail[1]["context_id"], "b");
  EXPECT_EQ(tail[1]["voice_settings"]["voice_id"], 1072);
  EXPECT_EQ(tail[2]["text"], "Hello there.");
}
