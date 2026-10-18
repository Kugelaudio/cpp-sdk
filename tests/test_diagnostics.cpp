// Client-error diagnostics: enablement, encoding, golden batch, delivery rules
// and event classification.

#include <gtest/gtest.h>

#include <fstream>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "diagnostics.hpp"
#include "kugelaudio/errors.hpp"
#include "kugelaudio/version.hpp"

namespace ka = kugelaudio;
namespace diag = kugelaudio::detail::diag;
using nlohmann::json;

namespace {

diag::EnvReader env_with(std::optional<std::string> value) {
  return [value](const std::string&) { return value; };
}

/// Records every delivery and answers with a scripted status.
struct FakeSender {
  std::mutex mutex;
  std::vector<std::string> payloads;
  std::vector<ka::detail::HeaderList> headers;
  std::vector<int> statuses;  // consumed front to back; 202 afterwards
  int throws = 0;             // throw this many times first

  diag::Sender sender() {
    return [this](const std::string&, const ka::detail::HeaderList& h, const std::string& body) {
      std::lock_guard<std::mutex> lock(mutex);
      if (throws > 0) {
        --throws;
        throw std::runtime_error("network down");
      }
      payloads.push_back(body);
      headers.push_back(h);
      if (statuses.empty()) return 202;
      const int status = statuses.front();
      statuses.erase(statuses.begin());
      return status;
    };
  }

  std::size_t count() {
    std::lock_guard<std::mutex> lock(mutex);
    return payloads.size();
  }
};

diag::Config enabled_config() {
  return diag::Config::resolve("https://api.kugelaudio.com", true,
                               {{"Authorization", "Bearer k"}, {"X-API-Key", "k"}},
                               env_with(std::nullopt));
}

std::vector<json> records_of(const std::string& payload) {
  const json body = json::parse(payload);
  const auto& records = body["resourceLogs"][0]["scopeLogs"][0]["logRecords"];
  return std::vector<json>(records.begin(), records.end());
}

std::map<std::string, json> attributes_of(const json& record) {
  std::map<std::string, json> out;
  for (const auto& attribute : record["attributes"]) out[attribute["key"]] = attribute["value"];
  return out;
}

}  // namespace

TEST(DiagnosticsConfig, Enablement) {
  const std::string hosted = "https://api.eu.kugelaudio.com";
  const std::string custom = "https://tts.example.com";
  EXPECT_TRUE(diag::Config::resolve(hosted, std::nullopt, {}, env_with(std::nullopt)).enabled);
  EXPECT_FALSE(diag::Config::resolve(custom, std::nullopt, {}, env_with(std::nullopt)).enabled);
  EXPECT_TRUE(diag::Config::resolve(custom, true, {}, env_with(std::nullopt)).enabled);
  EXPECT_FALSE(diag::Config::resolve(hosted, false, {}, env_with(std::nullopt)).enabled);
  // The environment variable overrides the option in both directions.
  EXPECT_FALSE(diag::Config::resolve(hosted, true, {}, env_with(" Off ")).enabled);
  EXPECT_TRUE(diag::Config::resolve(custom, false, {}, env_with("YES")).enabled);
  EXPECT_FALSE(diag::Config::resolve(hosted, std::nullopt, {}, env_with("0")).enabled);
  // Unknown values fall through to the option / default.
  EXPECT_TRUE(diag::Config::resolve(hosted, std::nullopt, {}, env_with("maybe")).enabled);

  const auto config = diag::Config::resolve(hosted + "/", std::nullopt, {}, env_with(std::nullopt));
  EXPECT_EQ(config.url, "https://api.eu.kugelaudio.com/v1/sdk-diagnostics");
  EXPECT_EQ(config.endpoint_kind, "hosted");
  EXPECT_EQ(diag::endpoint_kind_for("https://kugelaudio.com.evil.example"), "custom");
  EXPECT_EQ(diag::endpoint_kind_for("http://127.0.0.1:8000"), "custom");
}

TEST(DiagnosticsEncoding, AllowlistAndTypes) {
  const json encoded =
      json::parse(diag::encode_attributes_json({{"kugel.event", std::string("request_failed")},
                                                {"kugel.http_status", std::int64_t{503}},
                                                {"text", std::string("secret user text")},
                                                {"kugel.http_status", std::int64_t{999}},
                                                {"kugel.error_type", std::int64_t{1}}}));
  ASSERT_EQ(encoded.size(), 2u);
  EXPECT_EQ(encoded[0],
            json({{"key", "kugel.event"}, {"value", {{"stringValue", "request_failed"}}}}));
  EXPECT_EQ(encoded[1], json({{"key", "kugel.http_status"}, {"value", {{"intValue", "503"}}}}));
}

TEST(DiagnosticsEncoding, IdsAndRuntime) {
  const std::string id = diag::new_id();
  EXPECT_EQ(id.size(), 32u);
  EXPECT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_NE(id, diag::new_id());
  EXPECT_EQ(diag::runtime_string().rfind("cpp/", 0), 0u);
}

TEST(DiagnosticsEncoding, GoldenBatch) {
  auto reporter = diag::Reporter::create(enabled_config(), {}, "none", false, "0.1.0");
  const auto common = [](const char* operation_id, const char* event_id) {
    return diag::Attributes{{"kugel.event_id", std::string(event_id)},
                            {"kugel.operation_id", std::string(operation_id)},
                            {"kugel.runtime", std::string("cpp/17.0.0")}};
  };
  auto with = [](diag::Attributes base, diag::Attributes extra) {
    base.insert(base.end(), extra.begin(), extra.end());
    return base;
  };
  const std::uint64_t t = 1760000000000000000ULL;
  std::vector<std::string> records;
  records.push_back(reporter->record(
      "connection_failed",
      with(common("0123456789abcdef0123456789abcdef", "00000000000000000000000000000001"),
           {{"kugel.operation", std::string("stream_session")},
            {"kugel.transport", std::string("websocket")},
            {"kugel.failure_stage", std::string("handshake")},
            {"kugel.elapsed_ms", std::int64_t{412}},
            {"kugel.audio_chunks", std::int64_t{0}},
            {"kugel.audio_bytes", std::int64_t{0}},
            {"kugel.retry_count", std::int64_t{0}},
            {"kugel.outcome", std::string("failed")},
            {"kugel.error_type", std::string("RateLimitError")},
            {"kugel.error_code", std::string("RATE_LIMITED")},
            {"kugel.http_status", std::int64_t{429}},
            {"kugel.server_request_id", std::string("9f2c1b7e4a6d4c8fb0e5a3d719c26f84")}}),
      t));
  records.push_back(reporter->record(
      "request_failed",
      with(common("0123456789abcdef0123456789abcde0", "00000000000000000000000000000002"),
           {{"kugel.operation", std::string("generate")},
            {"kugel.transport", std::string("http")},
            {"kugel.failure_stage", std::string("sending_request")},
            {"kugel.elapsed_ms", std::int64_t{87}},
            {"kugel.audio_chunks", std::int64_t{0}},
            {"kugel.audio_bytes", std::int64_t{0}},
            {"kugel.retry_count", std::int64_t{0}},
            {"kugel.outcome", std::string("failed")},
            {"kugel.error_type", std::string("ValidationError")},
            {"kugel.error_code", std::string("VALIDATION_ERROR")},
            {"kugel.http_status", std::int64_t{400}},
            {"kugel.server_request_id", std::string("4a6d4c8fb0e5a3d719c26f849f2c1b7e")}}),
      t + 1));
  records.push_back(reporter->record(
      "stream_interrupted",
      with(common("0123456789abcdef0123456789abcde1", "00000000000000000000000000000003"),
           {{"kugel.operation", std::string("stream")},
            {"kugel.transport", std::string("websocket")},
            {"kugel.failure_stage", std::string("receiving_audio")},
            {"kugel.elapsed_ms", std::int64_t{1840}},
            {"kugel.audio_chunks", std::int64_t{12}},
            {"kugel.audio_bytes", std::int64_t{96000}},
            {"kugel.retry_count", std::int64_t{0}},
            {"kugel.outcome", std::string("failed")},
            {"kugel.error_type", std::string("ConnectionError")},
            {"kugel.http_status", std::int64_t{503}},
            {"kugel.ws_close_code", std::int64_t{1006}}}),
      t + 2));
  records.push_back(reporter->record(
      "retry_exhausted",
      with(common("0123456789abcdef0123456789abcde2", "00000000000000000000000000000004"),
           {{"kugel.operation", std::string("multi_context")},
            {"kugel.transport", std::string("websocket")},
            {"kugel.failure_stage", std::string("awaiting_first_audio")},
            {"kugel.elapsed_ms", std::int64_t{2210}},
            {"kugel.audio_chunks", std::int64_t{0}},
            {"kugel.audio_bytes", std::int64_t{0}},
            {"kugel.retry_count", std::int64_t{1}},
            {"kugel.outcome", std::string("failed")},
            {"kugel.error_type", std::string("ServerRestartingError")},
            {"kugel.http_status", std::int64_t{503}},
            {"kugel.ws_close_code", std::int64_t{1012}}}),
      t + 3));
  records.push_back(reporter->record(
      "sdk_stats",
      with(common("0123456789abcdef0123456789abcde3", "00000000000000000000000000000005"),
           {{"kugel.success_count", std::int64_t{412}},
            {"kugel.failure_count", std::int64_t{3}},
            {"kugel.cancelled_count", std::int64_t{9}}}),
      t + 4));
  const json produced = json::parse(diag::build_payload(records, "0.1.0"));

  std::ifstream in(std::string(KUGELAUDIO_TEST_FIXTURES) + "/diagnostics_golden_batch.json");
  ASSERT_TRUE(in) << "fixture missing";
  const json golden = json::parse(in);
  EXPECT_EQ(produced, golden) << produced.dump(2);
}

TEST(DiagnosticsReporter, DeliversBatchesOfEightWithAuthHeaders) {
  FakeSender fake;
  auto reporter = diag::Reporter::create(enabled_config(), fake.sender(), "none", false);
  for (int i = 0; i < 10; ++i) reporter->report("request_failed", {});
  EXPECT_EQ(reporter->pending(), 10u);
  reporter->flush(std::chrono::seconds(2));
  ASSERT_EQ(fake.count(), 2u);
  EXPECT_EQ(records_of(fake.payloads[0]).size(), 8u);
  EXPECT_EQ(records_of(fake.payloads[1]).size(), 2u);
  EXPECT_EQ(fake.headers[0][0].first, "Authorization");
  const auto attributes = attributes_of(records_of(fake.payloads[0])[0]);
  EXPECT_EQ(attributes.at("kugel.sdk.name")["stringValue"], "cpp");
  EXPECT_EQ(attributes.at("kugel.sdk.version")["stringValue"], ka::kVersion);
  EXPECT_EQ(attributes.at("kugel.endpoint_kind")["stringValue"], "hosted");
  EXPECT_EQ(attributes.at("kugel.integration")["stringValue"], "none");
  const json resource = json::parse(fake.payloads[0])["resourceLogs"][0]["resource"];
  EXPECT_EQ(resource["attributes"][2]["value"]["stringValue"], "cpp");
  reporter->close();
}

TEST(DiagnosticsReporter, QueueDropsOldest) {
  FakeSender fake;
  auto reporter = diag::Reporter::create(enabled_config(), fake.sender(), "none", false);
  for (int i = 0; i < 70; ++i) {
    reporter->report("request_failed", {{"kugel.retry_count", std::int64_t{i}}});
  }
  EXPECT_EQ(reporter->pending(), diag::kQueueCapacity);
  reporter->flush(std::chrono::seconds(2));
  const auto first = attributes_of(records_of(fake.payloads.at(0))[0]);
  EXPECT_EQ(first.at("kugel.retry_count")["intValue"], "6");
}

TEST(DiagnosticsReporter, ThreeNotFoundSwitchItOff) {
  FakeSender fake;
  fake.statuses = {404, 404, 404};
  auto reporter = diag::Reporter::create(enabled_config(), fake.sender(), "none", false);
  for (int round = 0; round < 3; ++round) {
    reporter->report("request_failed", {});
    reporter->flush(std::chrono::seconds(2));
  }
  EXPECT_TRUE(reporter->route_missing());
  EXPECT_TRUE(reporter->inert());
  reporter->report("request_failed", {});
  EXPECT_EQ(reporter->pending(), 0u);
  EXPECT_EQ(fake.count(), 3u);
}

TEST(DiagnosticsReporter, PayloadTooLargeIsNotRetriedTransportErrorIsRetriedOnce) {
  FakeSender fake;
  fake.statuses = {413};
  auto reporter = diag::Reporter::create(enabled_config(), fake.sender(), "none", false);
  reporter->report("request_failed", {});
  reporter->flush(std::chrono::seconds(2));
  EXPECT_EQ(fake.count(), 1u);

  FakeSender flaky;
  flaky.throws = 1;
  auto second = diag::Reporter::create(enabled_config(), flaky.sender(), "none", false);
  second->report("request_failed", {});
  second->flush(std::chrono::seconds(2));
  EXPECT_EQ(flaky.count(), 1u);

  FakeSender down;
  down.throws = 5;
  auto third = diag::Reporter::create(enabled_config(), down.sender(), "none", false);
  third->report("request_failed", {});
  third->flush(std::chrono::seconds(2));
  EXPECT_EQ(down.count(), 0u);
  EXPECT_EQ(down.throws, 3);  // one try + one retry, then dropped
}

TEST(DiagnosticsReporter, DisabledSendsNothing) {
  FakeSender fake;
  auto config =
      diag::Config::resolve("https://api.kugelaudio.com", false, {}, env_with(std::nullopt));
  auto reporter = diag::Reporter::create(config, fake.sender(), "none", false);
  reporter->report("request_failed", {});
  reporter->note_failure();
  reporter->close();
  EXPECT_EQ(fake.count(), 0u);
}

TEST(DiagnosticsReporter, CloseSendsStats) {
  FakeSender fake;
  auto reporter = diag::Reporter::create(enabled_config(), fake.sender(), "none", false);
  reporter->note_success();
  reporter->note_success();
  reporter->note_cancelled();
  reporter->close(std::chrono::seconds(2));
  ASSERT_EQ(fake.count(), 1u);
  const auto record = records_of(fake.payloads[0])[0];
  EXPECT_EQ(record["body"]["stringValue"], "sdk_stats");
  EXPECT_EQ(record["severityText"], "INFO");
  const auto attributes = attributes_of(record);
  EXPECT_EQ(attributes.at("kugel.success_count")["intValue"], "2");
  EXPECT_EQ(attributes.at("kugel.failure_count")["intValue"], "0");
  EXPECT_EQ(attributes.at("kugel.cancelled_count")["intValue"], "1");
  reporter->close();  // idempotent
  EXPECT_EQ(fake.count(), 1u);
}

TEST(DiagnosticsOperation, EventClassification) {
  FakeSender fake;
  auto reporter = diag::Reporter::create(enabled_config(), fake.sender(), "none", false);
  const auto error = std::make_exception_ptr(ka::ConnectionError("down"));

  auto connect = reporter->operation(std::string("stream"), diag::kTransportWebsocket);
  EXPECT_EQ(connect.stage(), "connecting");
  EXPECT_EQ(connect.event_name(), "connection_failed");

  auto http = reporter->operation(std::string("voices"), diag::kTransportHttp);
  EXPECT_EQ(http.event_name(), "request_failed");

  auto streaming = reporter->operation(std::string("stream"), diag::kTransportWebsocket);
  streaming.expect_audio();
  EXPECT_EQ(streaming.stage(), "awaiting_first_audio");
  streaming.record_chunk(100);
  EXPECT_EQ(streaming.stage(), "receiving_audio");
  EXPECT_EQ(streaming.event_name(), "stream_interrupted");
  streaming.mark_server_error(std::make_exception_ptr(ka::RateLimitError()));
  EXPECT_EQ(streaming.event_name(), "request_failed");
  streaming.record_retry();
  EXPECT_EQ(streaming.event_name(), "retry_exhausted");

  streaming.set_ws_close_code(1006);
  streaming.fail(error);
  streaming.fail(error);  // settled: no second event
  auto cancelled = reporter->operation(std::string("stream"), diag::kTransportWebsocket);
  cancelled.cancel();
  cancelled.fail(error);

  EXPECT_EQ(reporter->pending(), 1u);
  EXPECT_EQ(reporter->counters()["failure"], 1);
  EXPECT_EQ(reporter->counters()["cancelled"], 1);
  reporter->flush(std::chrono::seconds(2));
  const auto attributes = attributes_of(records_of(fake.payloads.at(0))[0]);
  EXPECT_EQ(attributes.at("kugel.event")["stringValue"], "retry_exhausted");
  EXPECT_EQ(attributes.at("kugel.error_type")["stringValue"], "ConnectionError");
  EXPECT_EQ(attributes.at("kugel.audio_chunks")["intValue"], "1");
  EXPECT_EQ(attributes.at("kugel.audio_bytes")["intValue"], "100");
  EXPECT_EQ(attributes.at("kugel.ws_close_code")["intValue"], "1006");
  EXPECT_EQ(attributes.at("kugel.retry_count")["intValue"], "1");
  EXPECT_EQ(attributes.count("kugel.error_message"), 0u);
}
