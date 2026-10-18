// REST resources against a local HTTP server.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "kugelaudio/kugelaudio.hpp"
#include "mock_server.hpp"

namespace ka = kugelaudio;
using ka::testing::json_response;
using ka::testing::MockHttpServer;
using ka::testing::RecordedRequest;
using nlohmann::json;

TEST(RestMock, ModelsListSendsAuthAndSdkHeaders) {
  MockHttpServer server([](const RecordedRequest&) {
    return json_response(200, R"({"models":[{"id":"kugel-3.5","name":"Kugel 3.5"}]})");
  });
  ka::ClientOptions options = ka::testing::mock_options(server.url());
  options.api_key = "eu-secret";  // the prefix is stripped, the explicit URL wins
  ka::Client client(options);
  const auto models = client.models().list();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models[0].id, "kugel-3.5");
  EXPECT_EQ(models[0].sample_rate, 24000);

  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].method, "GET");
  EXPECT_EQ(requests[0].uri, "/v1/models");
  EXPECT_EQ(requests[0].headers.at("authorization"), "Bearer secret");
  EXPECT_EQ(requests[0].headers.at("x-api-key"), "secret");
  EXPECT_EQ(requests[0].headers.at("x-kugelaudio-sdk"), "cpp");
  EXPECT_EQ(requests[0].headers.at("x-kugelaudio-sdk-version"), ka::kVersion);
  EXPECT_EQ(requests[0].headers.at("user-agent"), std::string("kugelaudio-cpp/") + ka::kVersion);
}

TEST(RestMock, VoicesListQueryAndGet) {
  MockHttpServer server([](const RecordedRequest& request) {
    if (request.uri.rfind("/v1/voices?", 0) == 0) {
      return json_response(200,
                           R"({"voices":[{"id":1,"name":"A"}],"total":1,"limit":5,"offset":10})");
    }
    return json_response(200, R"({"id":1071,"name":"Anna","pending_verification":true})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::VoiceListOptions options;
  options.language = "de";
  options.limit = 5;
  options.offset = 10;
  const auto page = client.voices().list(options);
  EXPECT_EQ(page.voices.size(), 1u);
  EXPECT_EQ(page.offset, 10);
  const auto voice = client.voices().get(1071);
  EXPECT_EQ(voice.name, "Anna");
  EXPECT_TRUE(voice.pending_verification);

  const auto requests = server.requests();
  EXPECT_EQ(requests[0].uri, "/v1/voices?limit=5&offset=10&language=de&include_public=true");
  EXPECT_EQ(requests[1].uri, "/v1/voices/1071");
}

TEST(RestMock, VoiceCreateIsMultipartWithConsent) {
  const auto reference = std::filesystem::temp_directory_path() / "kugelaudio-test-reference.wav";
  ka::write_wav(reference, ka::Bytes(320, 0), 16000);
  MockHttpServer server(
      [](const RecordedRequest&) { return json_response(201, R"({"id":5,"name":"Mine"})"); });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::VoiceCreateRequest request;
  request.name = "Mine";
  request.sex = "female";
  request.speaker_consent = true;
  request.reference_files = {reference};
  const auto created = client.voices().create(request);
  EXPECT_EQ(created.id, 5);
  std::filesystem::remove(reference);

  const auto sent = server.requests().at(0);
  EXPECT_EQ(sent.method, "POST");
  EXPECT_NE(sent.headers.at("content-type").find("multipart/form-data"), std::string::npos);
  EXPECT_NE(sent.body.find("name=\"metadata\""), std::string::npos);
  EXPECT_NE(sent.body.find("\"speaker_consent\":true"), std::string::npos);
  EXPECT_NE(sent.body.find("\"supported_languages\":[\"en\"]"), std::string::npos);
  EXPECT_NE(sent.body.find("name=\"files\"; filename=\"kugelaudio-test-reference.wav\""),
            std::string::npos);
  EXPECT_THROW(
      {
        request.reference_files = {"/nonexistent/ref.wav"};
        client.voices().create(request);
      },
      ka::ValidationError);
}

TEST(RestMock, VoiceUpdateDeleteReferencesPublishSample) {
  MockHttpServer server([](const RecordedRequest& request) {
    if (request.method == "DELETE") return json_response(204, "");
    if (request.uri == "/v1/voices/9/references" && request.method == "GET") {
      return json_response(200, R"([{"id":1,"voice_id":9,"name":"r.wav"}])");
    }
    if (request.uri == "/v1/voices/9/references") {
      return json_response(201, R"({"id":2,"voice_id":9,"reference_text":"hi"})");
    }
    if (request.uri == "/v1/voices/9/generate-sample") {
      return json_response(200, R"({"sample_s3_path":"s/9.wav","sample_url":"https://x"})");
    }
    return json_response(200, R"({"id":9,"name":"New"})");
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::VoiceUpdate update;
  update.name = "New";
  update.is_public = false;
  EXPECT_EQ(client.voices().update(9, update).name, "New");
  client.voices().remove(9);
  EXPECT_EQ(client.voices().list_references(9).at(0).name, "r.wav");
  const auto file = std::filesystem::temp_directory_path() / "kugelaudio-test-ref2.wav";
  ka::write_wav(file, ka::Bytes(64, 0), 16000);
  EXPECT_EQ(client.voices().add_reference(9, file, true, "hi").reference_text, "hi");
  std::filesystem::remove(file);
  client.voices().remove_reference(9, 2);
  EXPECT_EQ(client.voices().publish(9).id, 9);
  EXPECT_EQ(client.voices().generate_sample(9).sample_url, "https://x");

  const auto requests = server.requests();
  EXPECT_EQ(requests[0].method, "PATCH");
  EXPECT_EQ(json::parse(requests[0].body), json({{"name", "New"}, {"is_public", false}}));
  EXPECT_EQ(requests[1].method, "DELETE");
  EXPECT_NE(
      requests[3].body.find("name=\"speaker_consent\"\r\nContent-Type: text/plain\r\n\r\ntrue"),
      std::string::npos)
      << requests[3].body;
  EXPECT_EQ(requests[4].uri, "/v1/voices/9/references/2");
  EXPECT_EQ(requests[5].uri, "/v1/voices/9/publish");
}

TEST(RestMock, DictionariesAndEntries) {
  MockHttpServer server([](const RecordedRequest& request) {
    const std::string dict = R"({"id":1,"project_id":3,"name":"Brands"})";
    if (request.uri.rfind("/v1/dictionaries?", 0) == 0 && request.method == "GET") {
      return json_response(200, "{\"dictionaries\":[" + dict + "]}");
    }
    if (request.uri.find("/entries") != std::string::npos) {
      if (request.method == "PUT")
        return json_response(200, R"({"upserted":2,"deleted":1,"total":2})");
      if (request.method == "GET") {
        return json_response(
            200,
            R"({"entries":[{"id":4,"dictionary_id":1,"word":"K8s","replacement":"kubernetes"}],"total":1,"limit":100,"offset":0})");
      }
      if (request.method == "DELETE") return json_response(204, "");
      return json_response(200,
                           R"({"id":4,"dictionary_id":1,"word":"K8s","replacement":"kubernetes"})");
    }
    if (request.method == "DELETE") return json_response(204, "");
    return json_response(200, dict);
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  auto& dictionaries = client.dictionaries();
  EXPECT_EQ(dictionaries.list(3).at(0).name, "Brands");
  EXPECT_EQ(dictionaries.create({"Brands", std::nullopt, std::string("en")}).project_id, 3);
  EXPECT_EQ(dictionaries.get(1).id, 1);
  ka::DictionaryUpdate update;
  update.is_active = false;
  dictionaries.update(1, update, 3);
  dictionaries.remove(1);
  ka::DictionaryEntryListOptions list_options;
  list_options.search = "k8";
  EXPECT_EQ(dictionaries.entries().list(1, list_options).entries.at(0).word, "K8s");
  dictionaries.entries().add(1, {"K8s", "kubernetes", std::nullopt, false});
  ka::DictionaryEntryUpdate entry_update;
  entry_update.ipa = "k";
  dictionaries.entries().update(1, 4, entry_update);
  dictionaries.entries().remove(1, 4);
  const auto replaced = dictionaries.entries().replace_all(
      1, {{"Postgres", "post-gres", std::nullopt, false}, {"K8s", "kubernetes", "x", true}}, 3);
  EXPECT_EQ(replaced.upserted, 2);

  const auto requests = server.requests();
  EXPECT_EQ(requests[0].uri, "/v1/dictionaries?project_id=3");
  EXPECT_EQ(json::parse(requests[1].body), json({{"name", "Brands"}, {"language", "en"}}));
  EXPECT_EQ(requests[3].uri, "/v1/dictionaries/1?project_id=3");
  EXPECT_EQ(json::parse(requests[3].body), json({{"is_active", false}}));
  EXPECT_EQ(requests[5].uri, "/v1/dictionaries/1/entries?limit=100&offset=0&search=k8");
  EXPECT_EQ(requests[9].method, "PUT");
  EXPECT_EQ(requests[9].uri, "/v1/dictionaries/1/entries?project_id=3");
  EXPECT_EQ(json::parse(requests[9].body)["entries"].size(), 2u);
}

TEST(RestMock, ErrorsAreTyped) {
  MockHttpServer server([](const RecordedRequest& request) {
    ix::WebSocketHttpHeaders headers{{"x-request-id", "req-42"}, {"Retry-After", "9"}};
    if (request.uri == "/v1/voices/1") {
      return json_response(404, R"({"error":"Voice not found","error_code":"NOT_FOUND"})", headers);
    }
    if (request.uri == "/v1/models") {
      return json_response(429, R"({"error":"Rate limit exceeded","error_code":"RATE_LIMITED"})",
                           headers);
    }
    return json_response(401, R"({"error":"Invalid API key","error_code":"UNAUTHORIZED"})",
                         headers);
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  try {
    client.voices().get(1);
    FAIL() << "expected NotFoundError";
  } catch (const ka::NotFoundError& e) {
    EXPECT_EQ(e.message(), "Voice not found");
    EXPECT_EQ(e.request_id(), "req-42");
  }
  try {
    client.models().list();
    FAIL() << "expected RateLimitError";
  } catch (const ka::RateLimitError& e) {
    EXPECT_EQ(e.retry_after(), 9);
  }
  EXPECT_THROW(client.dictionaries().list(), ka::AuthenticationError);
}

TEST(RestMock, UnreachableServerIsConnectionError) {
  ka::ClientOptions options = ka::testing::mock_options("http://127.0.0.1:9");
  options.timeout = std::chrono::seconds(2);
  ka::Client client(options);
  EXPECT_THROW(client.models().list(), ka::ConnectionError);
}

TEST(RestMock, GenerateReturnsAudioAndHeaders) {
  MockHttpServer server([](const RecordedRequest&) {
    ix::WebSocketHttpHeaders headers{{"Content-Type", "audio/basic"},
                                     {"X-Sample-Rate", "8000"},
                                     {"X-Audio-Format", "mulaw"},
                                     {"x-request-id", "gen-1"}};
    return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok, headers,
                                              std::string(800, '\x7f'));
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  ka::GenerateRequest request;
  request.text = "Your code is 4 8 1 5.";
  request.voice_id = 1071;
  request.output_format = ka::output_format::kUlaw8000;
  request.language = "en";
  const auto audio = client.tts().generate(request);
  EXPECT_EQ(audio.sample_rate, 8000);
  EXPECT_EQ(audio.encoding, "mulaw");
  EXPECT_EQ(audio.samples, 800);
  EXPECT_DOUBLE_EQ(audio.duration_ms, 100.0);
  EXPECT_EQ(audio.request_id, "gen-1");

  const auto sent = server.requests().at(0);
  EXPECT_EQ(sent.uri, "/v1/tts/generate");
  const json body = json::parse(sent.body);
  EXPECT_EQ(body["output_format"], "ulaw_8000");
  EXPECT_FALSE(body.contains("word_timestamps"));

  request.word_timestamps = true;
  EXPECT_THROW(client.tts().generate(request), ka::ValidationError);
}

TEST(RestMock, EnhanceGenerateAndPrewarm) {
  MockHttpServer server([](const RecordedRequest& request) {
    if (request.uri == "/v1/audio/enhance/warmup")
      return json_response(202, R"({"status":"warming"})");
    const ka::Bytes wav = ka::to_wav(ka::Bytes{1, 0, 2, 0}, 24000);
    return std::make_shared<ix::HttpResponse>(
        200, "OK", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{{"Content-Type", "audio/wav"}},
        std::string(wav.begin(), wav.end()));
  });
  ka::Client client(ka::testing::mock_options(server.url()));
  client.enhance().prewarm();
  ka::EnhanceOptions options;
  options.model = "clarity-1";
  options.speaker = ka::load_audio(ka::to_wav(ka::Bytes(64, 0), 16000), "speaker.wav");
  const auto result = client.enhance().generate(
      ka::load_audio(ka::to_wav(ka::Bytes(64, 0), 16000), "in.wav"), options);
  EXPECT_EQ(result.audio, (ka::Bytes{1, 0, 2, 0}));
  EXPECT_EQ(result.sample_rate, 24000);

  const auto requests = server.requests();
  EXPECT_EQ(requests[0].uri, "/v1/audio/enhance/warmup");
  EXPECT_EQ(requests[1].uri, "/v1/audio/enhance");
  EXPECT_NE(requests[1].body.find("target_speaker_extraction"), std::string::npos);
  EXPECT_NE(requests[1].body.find("filename=\"speaker.wav\""), std::string::npos);
  options.model = " ";
  EXPECT_THROW(client.enhance().generate(ka::load_audio(ka::Bytes{1}), options),
               ka::ValidationError);
}

TEST(RestMock, PrewarmNeverThrows) {
  MockHttpServer server([](const RecordedRequest&) { return json_response(401, "{}"); });
  ka::Client client(ka::testing::mock_options(server.url()));
  EXPECT_NO_THROW(client.enhance().prewarm());
  ka::Client unreachable(ka::testing::mock_options("http://127.0.0.1:9"));
  EXPECT_NO_THROW(unreachable.enhance().prewarm());
}
