// API key parsing, endpoint resolution, URL building and error mapping.

#include <gtest/gtest.h>

#include "error_mapping.hpp"
#include "kugelaudio/kugelaudio.hpp"
#include "url.hpp"

namespace ka = kugelaudio;
namespace kd = kugelaudio::detail;

namespace {

template <typename Expected>
Expected rethrow_as(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const Expected& e) {
    return e;
  } catch (const std::exception& e) {
    ADD_FAILURE() << "unexpected error type: " << e.what();
  }
  throw std::logic_error("unreachable");
}

}  // namespace

TEST(ApiKey, StripsRegionPrefixes) {
  EXPECT_EQ(kd::parse_api_key("eu-abc").first, "abc");
  EXPECT_EQ(kd::parse_api_key("eu-abc").second, ka::Region::EU);
  EXPECT_EQ(kd::parse_api_key("us-abc").second, ka::Region::US);
  EXPECT_EQ(kd::parse_api_key("global-abc").second, ka::Region::Global);
  const auto plain = kd::parse_api_key("sk-kug-proj_123");
  EXPECT_EQ(plain.first, "sk-kug-proj_123");
  EXPECT_FALSE(plain.second.has_value());
  const auto project = kd::parse_api_key("eu-sk-kug-proj_123");
  EXPECT_EQ(project.first, "sk-kug-proj_123");
  EXPECT_EQ(project.second, ka::Region::EU);
}

TEST(Endpoints, ResolutionPriority) {
  EXPECT_EQ(kd::resolve_api_url(std::nullopt, std::nullopt, std::nullopt), ka::kDefaultApiUrl);
  EXPECT_EQ(kd::resolve_api_url(std::nullopt, std::nullopt, ka::Region::EU), ka::kEuApiUrl);
  EXPECT_EQ(kd::resolve_api_url(std::nullopt, ka::Region::US, ka::Region::EU), ka::kDefaultApiUrl);
  EXPECT_EQ(kd::resolve_api_url(std::nullopt, ka::Region::EU, std::nullopt), ka::kEuApiUrl);
  EXPECT_EQ(
      kd::resolve_api_url(std::string("https://onprem.example/"), ka::Region::EU, ka::Region::EU),
      "https://onprem.example");
  EXPECT_EQ(kd::resolve_api_url(std::nullopt, ka::Region::Global, std::nullopt),
            ka::kDefaultApiUrl);
}

TEST(Endpoints, ClientUsesKeyPrefixAndTtsUrl) {
  ka::ClientOptions options;
  options.api_key = "eu-secret";
  options.telemetry = false;
  ka::Client eu(options);
  EXPECT_EQ(eu.api_url(), "https://api.eu.kugelaudio.com");
  EXPECT_EQ(eu.tts_url(), "https://api.eu.kugelaudio.com");

  options.api_url = "http://localhost:9000/";
  options.tts_url = "http://localhost:9001/";
  ka::Client custom(options);
  EXPECT_EQ(custom.api_url(), "http://localhost:9000");
  EXPECT_EQ(custom.tts_url(), "http://localhost:9001");
}

TEST(Endpoints, ParseRegion) {
  EXPECT_EQ(ka::parse_region("eu"), ka::Region::EU);
  EXPECT_THROW(ka::parse_region("mars"), ka::ValidationError);
}

TEST(Client, EmptyKeyIsValidationError) {
  EXPECT_THROW({ ka::Client client{std::string()}; }, ka::ValidationError);
}

TEST(Urls, WebSocketAndQuery) {
  EXPECT_EQ(kd::to_ws_url("https://api.kugelaudio.com"), "wss://api.kugelaudio.com");
  EXPECT_EQ(kd::to_ws_url("http://127.0.0.1:8080"), "ws://127.0.0.1:8080");
  EXPECT_EQ(kd::url_encode("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
  EXPECT_EQ(kd::append_query("https://x/ws", {{"a", "1"}}), "https://x/ws?a=1");
  EXPECT_EQ(kd::append_query("https://x/ws?a=1", {{"b", "2"}}), "https://x/ws?a=1&b=2");
  const auto sdk = kd::build_query(kd::sdk_query_params());
  EXPECT_EQ(sdk, std::string("sdk=cpp&sdk_version=") + ka::kVersion);
  EXPECT_EQ(kd::host_of("https://API.eu.kugelaudio.com:443/v1"), "api.eu.kugelaudio.com");
  EXPECT_EQ(kd::host_of("http://[::1]:80/x"), "::1");
  EXPECT_EQ(kd::redact_queries("connect to wss://h/ws?api_key=secret failed"),
            "connect to wss://h/ws?<redacted> failed");
}

TEST(Urls, Headers) {
  const auto headers = kd::sdk_headers();
  ASSERT_EQ(headers.size(), 3u);
  EXPECT_EQ(headers[0].second, "cpp");
  EXPECT_EQ(headers[2].second, std::string("kugelaudio-cpp/") + ka::kVersion);
  const auto auth = kd::auth_headers("k");
  EXPECT_EQ(auth[0].second, "Bearer k");
  EXPECT_EQ(auth[1].second, "k");
}

TEST(Errors, HttpStatusMapping) {
  const kd::Headers none;
  EXPECT_NO_THROW(rethrow_as<ka::AuthenticationError>(kd::classify_http_response(401, "", none)));
  EXPECT_NO_THROW(
      rethrow_as<ka::InsufficientCreditsError>(kd::classify_http_response(402, "", none)));
  EXPECT_NO_THROW(rethrow_as<ka::NotFoundError>(kd::classify_http_response(404, "", none)));
  EXPECT_NO_THROW(rethrow_as<ka::ValidationError>(kd::classify_http_response(400, "", none)));
  EXPECT_NO_THROW(rethrow_as<ka::ConnectionError>(kd::classify_http_response(503, "", none)));
  const auto generic = rethrow_as<ka::KugelAudioError>(kd::classify_http_response(500, "", none));
  EXPECT_STREQ(generic.type_name(), "KugelAudioError");
  EXPECT_EQ(generic.message(), "HTTP 500");
}

TEST(Errors, RateLimitReadsBodyAndHeaders) {
  const auto error = rethrow_as<ka::RateLimitError>(kd::classify_http_response(
      429, R"({"error":"Rate limit exceeded","error_code":"RATE_LIMITED","code":429})",
      {{"retry-after", "7"}, {"x-request-id", "req-1"}}));
  EXPECT_EQ(error.message(), "Rate limit exceeded");
  EXPECT_EQ(error.retry_after(), 7);
  EXPECT_EQ(error.request_id(), "req-1");
  EXPECT_EQ(error.status_code(), 429);
  EXPECT_EQ(error.error_code(), "RATE_LIMITED");
  EXPECT_STREQ(error.what(), "Rate limit exceeded (request_id: req-1)");
}

TEST(Errors, ErrorCodeWinsOverStatus) {
  EXPECT_NO_THROW(rethrow_as<ka::ValidationError>(kd::classify_http_response(
      400, R"({"error":"voice_id is required","error_code":"MISSING_VOICE_ID"})", {})));
  EXPECT_NO_THROW(rethrow_as<ka::RateLimitError>(kd::classify_http_response(
      200, R"({"error":"too many","error_code":"TOO_MANY_CONTEXTS"})", {})));
  const auto unavailable = rethrow_as<ka::ConnectionError>(kd::classify_http_response(
      503, R"({"error":"busy","error_code":"MODEL_UNAVAILABLE"})", {{"retry-after", "30"}}));
  EXPECT_EQ(unavailable.message(), "KugelAudio is temporarily unavailable: busy. Retry shortly.");
  EXPECT_EQ(unavailable.retry_after(), 30);
}

TEST(Errors, FastApiDetailListAndRawText) {
  const auto detail = rethrow_as<ka::ValidationError>(
      kd::classify_http_response(400, R"({"detail":["a","b"]})", {}));
  EXPECT_EQ(detail.message(), "a; b");
  const auto raw =
      rethrow_as<ka::KugelAudioError>(kd::classify_http_response(502, "  bad gateway \n", {}));
  EXPECT_EQ(raw.message(), "bad gateway");
}

TEST(Errors, AuthenticationDefaultMessage) {
  const auto error = rethrow_as<ka::AuthenticationError>(kd::classify_http_response(401, "", {}));
  EXPECT_NE(error.message().find("app.kugelaudio.com/settings/api-keys"), std::string::npos);
  EXPECT_EQ(error.error_code(), "UNAUTHORIZED");
}

TEST(Errors, WebSocketFrames) {
  const auto frame = nlohmann::json::parse(
      R"({"error":"Insufficient credits","error_code":"INSUFFICIENT_CREDITS","code":402,"request_id":"abc"})");
  const auto error = rethrow_as<ka::InsufficientCreditsError>(kd::classify_ws_frame(frame));
  EXPECT_EQ(error.request_id(), "abc");
  const auto bare =
      rethrow_as<ka::KugelAudioError>(kd::classify_ws_frame(nlohmann::json::object()));
  EXPECT_EQ(bare.message(), "Server reported an error.");
}

TEST(Errors, HandshakeRejection) {
  const auto forbidden = rethrow_as<ka::AuthenticationError>(
      kd::classify_handshake_rejection(403, {{"x-request-id", "r"}}, "refused"));
  EXPECT_EQ(forbidden.request_id(), "r");
  const auto limited = rethrow_as<ka::RateLimitError>(
      kd::classify_handshake_rejection(429, {{"retry-after", "3"}}, ""));
  EXPECT_EQ(limited.retry_after(), 3);
}

TEST(Errors, CloseCodes) {
  EXPECT_NO_THROW(rethrow_as<ka::AuthenticationError>(kd::classify_ws_close(4001, "bad key")));
  EXPECT_NO_THROW(rethrow_as<ka::InsufficientCreditsError>(kd::classify_ws_close(4003, "")));
  EXPECT_NO_THROW(rethrow_as<ka::RateLimitError>(kd::classify_ws_close(4029, "")));
  EXPECT_NO_THROW(rethrow_as<ka::ConnectionError>(kd::classify_ws_close(4500, "")));
  const auto restart = rethrow_as<ka::ServerRestartingError>(kd::classify_ws_close(1012, ""));
  EXPECT_EQ(restart.retry_after(), 1);
  EXPECT_NO_THROW(rethrow_as<ka::ServerRestartingError>(kd::classify_ws_close(1013, "")));
  const auto generic = rethrow_as<ka::ConnectionError>(kd::classify_ws_close(1011, ""));
  EXPECT_EQ(generic.message(),
            "KugelAudio WebSocket closed by server: no reason given (code 1011).");
  EXPECT_TRUE(kd::is_restart_close(1012));
  EXPECT_FALSE(kd::is_error_close(1000));
  EXPECT_TRUE(kd::is_error_close(4001));
}

TEST(Errors, HierarchyCatchesAsBase) {
  try {
    throw ka::ServerRestartingError();
  } catch (const ka::ConnectionError& e) {
    EXPECT_STREQ(e.type_name(), "ServerRestartingError");
  }
}
