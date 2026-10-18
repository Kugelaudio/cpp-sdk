#pragma once

// Client-error diagnostics: an OTLP/HTTP-JSON log reporter and the Operation
// seam the public entry points wrap themselves in. Same contract as the
// Python, JavaScript and Java SDKs: attribute allowlist, enablement order,
// delivery bounds. Never sends text, audio, URLs, keys or error messages, and
// never surfaces an error or a log line above debug.

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "url.hpp"

namespace kugelaudio::detail::diag {

inline constexpr const char* kPath = "/v1/sdk-diagnostics";
inline constexpr const char* kEnvTelemetry = "KUGELAUDIO_TELEMETRY";
inline constexpr std::size_t kQueueCapacity = 64;
inline constexpr std::size_t kBatchSize = 8;
inline constexpr auto kFlushInterval = std::chrono::seconds(5);
inline constexpr auto kRequestTimeout = std::chrono::seconds(3);
inline constexpr int kMaxSendAttempts = 2;
inline constexpr auto kCloseDeadline = std::chrono::seconds(1);
inline constexpr int kMaxConsecutiveNotFound = 3;

// kugel.event
inline constexpr const char* kEventConnectionFailed = "connection_failed";
inline constexpr const char* kEventRequestFailed = "request_failed";
inline constexpr const char* kEventStreamInterrupted = "stream_interrupted";
inline constexpr const char* kEventRetryExhausted = "retry_exhausted";
inline constexpr const char* kEventSdkStats = "sdk_stats";

// kugel.operation
inline constexpr const char* kOpGenerate = "generate";
inline constexpr const char* kOpStream = "stream";
inline constexpr const char* kOpStreamSession = "stream_session";
inline constexpr const char* kOpMultiContext = "multi_context";
inline constexpr const char* kOpVoices = "voices";
inline constexpr const char* kOpDictionaries = "dictionaries";
inline constexpr const char* kOpModels = "models";

// kugel.failure_stage
inline constexpr const char* kStageConnecting = "connecting";
inline constexpr const char* kStageHandshake = "handshake";
inline constexpr const char* kStageSendingRequest = "sending_request";
inline constexpr const char* kStageAwaitingFirstAudio = "awaiting_first_audio";
inline constexpr const char* kStageReceivingAudio = "receiving_audio";
inline constexpr const char* kStageFinalizing = "finalizing";

inline constexpr const char* kTransportHttp = "http";
inline constexpr const char* kTransportWebsocket = "websocket";

/// A record attribute value: string or int.
using Value = std::variant<std::string, std::int64_t>;
/// Ordered attributes (encoding order is insertion order).
using Attributes = std::vector<std::pair<std::string, Value>>;

/// Delivers one payload; returns the HTTP status. Throws only when no
/// response arrived (DNS, timeout, reset).
using Sender =
    std::function<int(const std::string& url, const HeaderList& headers, const std::string& body)>;

/// Reads an environment variable; std::nullopt when unset.
using EnvReader = std::function<std::optional<std::string>(const std::string& name)>;

/// Resolved enablement and target.
struct Config {
  bool enabled = false;
  std::string url;
  HeaderList auth_headers;
  std::string endpoint_kind = "custom";

  bool inert() const { return !enabled || url.empty(); }

  /// First match wins: env `KUGELAUDIO_TELEMETRY`, the `telemetry` option,
  /// then enabled only for hosts ending in `.kugelaudio.com`.
  static Config resolve(const std::string& api_url, std::optional<bool> telemetry,
                        HeaderList auth_headers, const EnvReader& env = {});
};

/// `hosted` when the host ends in `.kugelaudio.com`, else `custom`.
std::string endpoint_kind_for(const std::string& api_url);
/// `cpp/<compiler major.minor.patch>`.
std::string runtime_string();
/// 32 lowercase hex characters.
std::string new_id();

/// Encode attributes as OTLP JSON, dropping keys outside the allowlist.
std::string encode_attributes_json(const Attributes& attributes);
/// One OTLP log record as JSON text.
std::string build_record(const std::string& event, const Attributes& attributes,
                         std::uint64_t time_unix_nano);
/// The OTLP/HTTP JSON body for `records` (each a JSON object text).
std::string build_payload(const std::vector<std::string>& records, const std::string& version);

class Reporter;

/// One unit of work that independently succeeds or fails; reports at most
/// one event. The first succeed/fail/cancel settles it.
class Operation {
 public:
  Operation(std::shared_ptr<Reporter> reporter, std::optional<std::string> name,
            std::string transport, std::optional<std::string> stage = std::nullopt);

  void set_stage(const std::string& stage) { stage_ = stage; }
  const std::string& stage() const { return stage_; }
  /// Enter the receive phase: awaiting first audio, or receiving more.
  void expect_audio();
  void record_chunk(std::size_t bytes);
  /// A new attempt (reconnect): earlier attempt details reset.
  void record_retry();
  /// The server answered with an error frame (`request_failed`).
  void mark_server_error(std::exception_ptr error) { server_error_ = std::move(error); }
  void set_ws_close_code(int code) { ws_close_code_ = code; }

  void succeed();
  /// Caller-initiated cancellation: counted in sdk_stats, never an event.
  void cancel();
  /// Report one event for `error`; never throws.
  void fail(const std::exception_ptr& error) noexcept;
  bool settled() const { return settled_; }
  int retry_count() const { return retry_count_; }
  const std::string& operation_id() const { return operation_id_; }

  /// The event name `fail` would report now.
  std::string event_name() const;
  /// The attributes `fail` would report for `error`.
  Attributes attributes(const std::exception_ptr& error) const;

 private:
  std::shared_ptr<Reporter> reporter_;
  std::optional<std::string> name_;
  std::string transport_;
  std::string operation_id_;
  std::chrono::steady_clock::time_point started_;
  std::string stage_;
  std::int64_t audio_chunks_ = 0;
  std::int64_t audio_bytes_ = 0;
  int retry_count_ = 0;
  std::optional<int> ws_close_code_;
  std::exception_ptr server_error_;
  bool settled_ = false;
};

/// Bounded, off-path reporter; one per client.
class Reporter : public std::enable_shared_from_this<Reporter> {
 public:
  /// @param sender Delivery transport; empty uses libcurl.
  /// @param autostart Start the delivery thread on the first event (tests
  ///        pass false and call flush()).
  static std::shared_ptr<Reporter> create(Config config, Sender sender = {},
                                          std::string integration = "none", bool autostart = true,
                                          std::string version = {});
  ~Reporter();

  /// A disabled reporter that never sends.
  static std::shared_ptr<Reporter> disabled();

  Operation operation(std::optional<std::string> name, const std::string& transport,
                      std::optional<std::string> stage = std::nullopt);

  /// The OTLP record (JSON text) this reporter would queue for `event`.
  std::string record(const std::string& event, Attributes attributes,
                     std::optional<std::uint64_t> time_unix_nano = std::nullopt) const;
  /// Queue one event. Never throws, never blocks on the network.
  void report(const std::string& event, Attributes attributes) noexcept;

  void note_success() noexcept;
  void note_failure() noexcept;
  void note_cancelled() noexcept;
  std::map<std::string, std::int64_t> counters() const;

  /// Hand everything queued to the worker and wait at most `timeout`.
  void flush(std::chrono::milliseconds timeout = kCloseDeadline) noexcept;
  /// Queue sdk_stats, flush, wait at most `timeout`. Idempotent.
  void close(std::chrono::milliseconds timeout = kCloseDeadline) noexcept;

  std::size_t pending() const;
  bool route_missing() const;
  bool inert() const;
  const Config& config() const { return config_; }

  struct State;

 private:
  Reporter(Config config, Sender sender, std::string integration, bool autostart,
           std::string version);
  void ensure_worker();

  Config config_;
  std::string integration_;
  std::string version_;
  bool autostart_;
  std::shared_ptr<State> state_;
};

}  // namespace kugelaudio::detail::diag
