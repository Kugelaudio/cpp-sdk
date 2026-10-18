#include "diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "error_mapping.hpp"
#include "http_transport.hpp"
#include "kugelaudio/version.hpp"

namespace kugelaudio::detail::diag {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

enum class Kind { String, Int };

const std::map<std::string, Kind>& allowlist() {
  static const std::map<std::string, Kind> kTypes = {
      {"kugel.event", Kind::String},
      {"kugel.event_id", Kind::String},
      {"kugel.operation_id", Kind::String},
      {"kugel.operation", Kind::String},
      {"kugel.sdk.name", Kind::String},
      {"kugel.sdk.version", Kind::String},
      {"kugel.runtime", Kind::String},
      {"kugel.integration", Kind::String},
      {"kugel.transport", Kind::String},
      {"kugel.failure_stage", Kind::String},
      {"kugel.error_type", Kind::String},
      {"kugel.error_code", Kind::String},
      {"kugel.http_status", Kind::Int},
      {"kugel.ws_close_code", Kind::Int},
      {"kugel.server_request_id", Kind::String},
      {"kugel.elapsed_ms", Kind::Int},
      {"kugel.audio_chunks", Kind::Int},
      {"kugel.audio_bytes", Kind::Int},
      {"kugel.retry_count", Kind::Int},
      {"kugel.outcome", Kind::String},
      {"kugel.endpoint_kind", Kind::String},
      {"kugel.success_count", Kind::Int},
      {"kugel.failure_count", Kind::Int},
      {"kugel.cancelled_count", Kind::Int},
  };
  return kTypes;
}

json encode_attributes(const Attributes& attributes) {
  json out = json::array();
  std::set<std::string> seen;
  for (const auto& [key, value] : attributes) {
    auto type = allowlist().find(key);
    if (type == allowlist().end() || !seen.insert(key).second) continue;
    if (type->second == Kind::Int) {
      if (const auto* number = std::get_if<std::int64_t>(&value)) {
        out.push_back({{"key", key}, {"value", {{"intValue", std::to_string(*number)}}}});
      }
    } else if (const auto* text = std::get_if<std::string>(&value)) {
      out.push_back({{"key", key}, {"value", {{"stringValue", *text}}}});
    }
  }
  return out;
}

void set_default(Attributes& attributes, const std::string& key, Value value) {
  for (const auto& entry : attributes) {
    if (entry.first == key) return;
  }
  attributes.emplace_back(key, std::move(value));
}

int default_sender(const std::string& url, const HeaderList& headers, const std::string& body) {
  HttpRequest request;
  request.method = "POST";
  request.url = url;
  request.headers = headers;
  request.json_body = body;
  request.timeout = kRequestTimeout;
  return perform_once(request).status;
}

std::uint64_t now_unix_nano() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace

// ------------------------------------------------------------------ config

std::string endpoint_kind_for(const std::string& api_url) {
  const std::string host = host_of(api_url);
  const std::string suffix = ".kugelaudio.com";
  if (host.size() > suffix.size() &&
      host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return "hosted";
  }
  return "custom";
}

Config Config::resolve(const std::string& api_url, std::optional<bool> telemetry,
                       HeaderList auth_headers, const EnvReader& env) {
  Config config;
  config.endpoint_kind = endpoint_kind_for(api_url);
  std::optional<std::string> raw;
  if (env) {
    raw = env(kEnvTelemetry);
  } else if (const char* value = std::getenv(kEnvTelemetry)) {
    raw = value;
  }
  std::string normalized = raw.value_or("");
  normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                  [](unsigned char c) { return std::isspace(c); }),
                   normalized.end());
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
    config.enabled = false;
  } else if (normalized == "1" || normalized == "true" || normalized == "on" ||
             normalized == "yes") {
    config.enabled = true;
  } else if (telemetry) {
    config.enabled = *telemetry;
  } else {
    config.enabled = config.endpoint_kind == "hosted";
  }
  config.url = api_url.empty() ? std::string() : strip_trailing_slashes(api_url) + kPath;
  config.auth_headers = std::move(auth_headers);
  return config;
}

std::string runtime_string() {
  std::ostringstream out;
#if defined(__clang__)
  out << "cpp/" << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
  out << "cpp/" << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
  out << "cpp/" << (_MSC_VER / 100) << '.' << (_MSC_VER % 100) << '.' << (_MSC_FULL_VER % 100000);
#else
  out << "cpp/0.0.0";
#endif
  return out.str();
}

std::string new_id() {
  thread_local std::mt19937_64 engine{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream out;
  out << std::hex;
  for (int i = 0; i < 2; ++i) {
    const std::uint64_t part = dist(engine);
    for (int shift = 60; shift >= 0; shift -= 4) out << ((part >> shift) & 0xF);
  }
  return out.str();
}

std::string encode_attributes_json(const Attributes& attributes) {
  return encode_attributes(attributes).dump();
}

std::string build_record(const std::string& event, const Attributes& attributes,
                         std::uint64_t time_unix_nano) {
  const bool stats = event == kEventSdkStats;
  json record{{"timeUnixNano", std::to_string(time_unix_nano)},
              {"severityNumber", stats ? 9 : 17},
              {"severityText", stats ? "INFO" : "ERROR"},
              {"body", {{"stringValue", event}}},
              {"attributes", encode_attributes(attributes)}};
  return record.dump();
}

std::string build_payload(const std::vector<std::string>& records, const std::string& version) {
  json resource{
      {"attributes",
       json::array({{{"key", "service.name"}, {"value", {{"stringValue", "kugelaudio-sdk"}}}},
                    {{"key", "service.version"}, {"value", {{"stringValue", version}}}},
                    {{"key", "telemetry.sdk.language"}, {"value", {{"stringValue", kSdkName}}}}})}};
  json scope{{"name", "kugelaudio.diagnostics"}, {"version", "1"}};
  json log_records = json::array();
  for (const auto& record : records) log_records.push_back(json::parse(record));
  json body{{"resourceLogs",
             json::array({{{"resource", resource},
                           {"scopeLogs",
                            json::array({{{"scope", scope}, {"logRecords", log_records}}})}}})}};
  return body.dump();
}

// ------------------------------------------------------------------- state

struct Reporter::State {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::string> records;
  bool worker_running = false;
  bool closed = false;
  bool flush_requested = false;
  bool in_flight = false;
  bool route_missing = false;
  std::optional<Clock::time_point> deadline;
  int consecutive_not_found = 0;
  std::atomic<std::int64_t> success{0};
  std::atomic<std::int64_t> failure{0};
  std::atomic<std::int64_t> cancelled{0};
  Config config;
  Sender sender;
  std::string version;
};

namespace {

using StatePtr = std::shared_ptr<Reporter::State>;

/// Reporters with records queued, flushed once at process exit.
struct ExitRegistry {
  std::mutex mutex;
  std::vector<std::weak_ptr<Reporter::State>> pending;
};

ExitRegistry& exit_registry();

/// Ask the worker to send everything now; true when there is something to wait for.
bool request_flush(const StatePtr& state);
void wait_drained(const StatePtr& state, Clock::time_point deadline);
void start_worker(const StatePtr& state);

void flush_all_at_exit() {
  // KEEP-JUSTIFIED: telemetry at exit is best-effort and must stay silent.
  try {
    std::vector<StatePtr> states;
    {
      ExitRegistry& registry = exit_registry();
      std::lock_guard<std::mutex> lock(registry.mutex);
      for (auto& weak : registry.pending) {
        if (auto state = weak.lock()) states.push_back(std::move(state));
      }
      registry.pending.clear();
    }
    const auto deadline = Clock::now() + kCloseDeadline;
    std::vector<StatePtr> waiting;
    for (auto& state : states) {
      if (request_flush(state)) waiting.push_back(state);
    }
    for (auto& state : waiting) wait_drained(state, deadline);
  } catch (...) {
  }
}

ExitRegistry& exit_registry() {
  // Leaked on purpose: it must outlive every static destructor and the
  // atexit hook that reads it.
  static ExitRegistry* registry = [] {
    auto* created = new ExitRegistry();
    std::atexit(flush_all_at_exit);
    return created;
  }();
  return *registry;
}

void note_pending(const StatePtr& state) {
  ExitRegistry& registry = exit_registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.pending.erase(std::remove_if(registry.pending.begin(), registry.pending.end(),
                                        [](const auto& weak) { return weak.expired(); }),
                         registry.pending.end());
  for (const auto& weak : registry.pending) {
    if (weak.lock() == state) return;
  }
  registry.pending.push_back(state);
}

void deliver(const StatePtr& state, const std::vector<std::string>& batch) {
  const std::string payload = build_payload(batch, state->version);
  HeaderList headers = state->config.auth_headers;
  for (int attempt = 0; attempt < kMaxSendAttempts; ++attempt) {
    int status = 0;
    try {
      status = state->sender(state->config.url, headers, payload);
    } catch (...) {
      // KEEP-JUSTIFIED: a transport failure is retried once, then dropped;
      // it neither bumps nor resets the 404 streak.
      continue;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (status >= 200 && status < 300) {
      state->consecutive_not_found = 0;
      return;
    }
    if (status == 404) {
      if (++state->consecutive_not_found >= kMaxConsecutiveNotFound) {
        state->route_missing = true;
        state->records.clear();
        state->deadline.reset();
        state->changed.notify_all();
      }
      return;
    }
    state->consecutive_not_found = 0;
    if (status == 413) return;
  }
}

/// Blocks until a batch is due; an empty optional tells the worker to exit.
std::optional<std::vector<std::string>> next_batch(const StatePtr& state) {
  std::unique_lock<std::mutex> lock(state->mutex);
  while (true) {
    if (state->route_missing) {
      state->records.clear();
      state->deadline.reset();
      state->changed.notify_all();
      return std::nullopt;
    }
    if (state->records.empty()) {
      state->flush_requested = false;
      state->deadline.reset();
      state->changed.notify_all();  // wakes flush()/close() waiters
      if (state->closed) return std::nullopt;
      state->changed.wait(lock);
      continue;
    }
    const auto now = Clock::now();
    const bool due = state->records.size() >= kBatchSize || state->flush_requested ||
                     state->closed || (state->deadline && now >= *state->deadline);
    if (due) {
      const std::size_t size = std::min(kBatchSize, state->records.size());
      std::vector<std::string> batch(state->records.begin(), state->records.begin() + size);
      state->records.erase(state->records.begin(), state->records.begin() + size);
      state->deadline = state->records.empty()
                            ? std::nullopt
                            : std::optional<Clock::time_point>(now + kFlushInterval);
      state->in_flight = true;
      return batch;
    }
    if (state->deadline) {
      state->changed.wait_until(lock, *state->deadline);
    } else {
      state->changed.wait_for(lock, kFlushInterval);
    }
  }
}

void run_worker(StatePtr state) {
  while (true) {
    auto batch = next_batch(state);
    if (!batch) break;
    // KEEP-JUSTIFIED: the delivery thread must never throw; a failed batch is dropped.
    try {
      deliver(state, *batch);
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->in_flight = false;
    state->changed.notify_all();
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  state->worker_running = false;
  state->changed.notify_all();
}

void start_worker(const StatePtr& state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->worker_running) return;
    state->worker_running = true;
  }
  // Detached: it owns its state and ends once closed and drained, so it never
  // delays process exit or close() beyond the close deadline.
  std::thread(run_worker, state).detach();
}

bool request_flush(const StatePtr& state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->config.inert() || state->route_missing) return false;
    if (state->records.empty() && !state->in_flight) return false;
    state->flush_requested = true;
    state->changed.notify_all();
  }
  start_worker(state);
  return true;
}

void wait_drained(const StatePtr& state, Clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(state->mutex);
  state->changed.wait_until(lock, deadline,
                            [&] { return state->records.empty() && !state->in_flight; });
}

}  // namespace

// ---------------------------------------------------------------- reporter

std::shared_ptr<Reporter> Reporter::create(Config config, Sender sender, std::string integration,
                                           bool autostart, std::string version) {
  return std::shared_ptr<Reporter>(new Reporter(
      std::move(config), std::move(sender), std::move(integration), autostart, std::move(version)));
}

std::shared_ptr<Reporter> Reporter::disabled() { return create(Config{}); }

Reporter::Reporter(Config config, Sender sender, std::string integration, bool autostart,
                   std::string version)
    : config_(std::move(config)),
      integration_(std::move(integration)),
      version_(version.empty() ? std::string(kVersion) : std::move(version)),
      autostart_(autostart),
      state_(std::make_shared<State>()) {
  state_->config = config_;
  state_->sender = sender ? std::move(sender) : Sender(default_sender);
  state_->version = version_;
}

Reporter::~Reporter() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->closed = true;
  state_->changed.notify_all();
}

Operation Reporter::operation(std::optional<std::string> name, const std::string& transport,
                              std::optional<std::string> stage) {
  return Operation(shared_from_this(), std::move(name), transport, std::move(stage));
}

std::string Reporter::record(const std::string& event, Attributes attributes,
                             std::optional<std::uint64_t> time_unix_nano) const {
  Attributes attrs;
  attrs.emplace_back("kugel.event", event);
  for (auto& entry : attributes) {
    if (entry.first != "kugel.event") attrs.push_back(std::move(entry));
  }
  set_default(attrs, "kugel.event_id", new_id());
  set_default(attrs, "kugel.operation_id", new_id());
  set_default(attrs, "kugel.sdk.name", std::string(kSdkName));
  set_default(attrs, "kugel.sdk.version", version_);
  set_default(attrs, "kugel.runtime", runtime_string());
  set_default(attrs, "kugel.integration", integration_);
  set_default(attrs, "kugel.endpoint_kind", config_.endpoint_kind);
  return build_record(event, attrs, time_unix_nano.value_or(now_unix_nano()));
}

void Reporter::report(const std::string& event, Attributes attributes) noexcept {
  // KEEP-JUSTIFIED: a telemetry failure must never surface to the caller.
  try {
    if (inert()) return;
    std::string encoded = record(event, std::move(attributes));
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->closed) return;
      if (state_->records.size() >= kQueueCapacity) state_->records.pop_front();
      state_->records.push_back(std::move(encoded));
      if (!state_->deadline) state_->deadline = Clock::now() + kFlushInterval;
      state_->changed.notify_all();
    }
    note_pending(state_);
    if (autostart_) ensure_worker();
  } catch (...) {
  }
}

void Reporter::ensure_worker() { start_worker(state_); }

void Reporter::note_success() noexcept { ++state_->success; }
void Reporter::note_failure() noexcept { ++state_->failure; }
void Reporter::note_cancelled() noexcept { ++state_->cancelled; }

std::map<std::string, std::int64_t> Reporter::counters() const {
  return {{"success", state_->success.load()},
          {"failure", state_->failure.load()},
          {"cancelled", state_->cancelled.load()}};
}

void Reporter::flush(std::chrono::milliseconds timeout) noexcept {
  // KEEP-JUSTIFIED: runs from close() and destructors; telemetry stays silent.
  try {
    const auto deadline = Clock::now() + timeout;
    if (request_flush(state_)) wait_drained(state_, deadline);
  } catch (...) {
  }
}

void Reporter::close(std::chrono::milliseconds timeout) noexcept {
  // KEEP-JUSTIFIED: runs from Client::close() and destructors; telemetry stays silent.
  try {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->closed) return;
    }
    const auto success = state_->success.load();
    const auto failure = state_->failure.load();
    const auto cancelled = state_->cancelled.load();
    if (success != 0 || failure != 0 || cancelled != 0) {
      report(kEventSdkStats, {{"kugel.success_count", success},
                              {"kugel.failure_count", failure},
                              {"kugel.cancelled_count", cancelled}});
    }
    const auto deadline = Clock::now() + timeout;
    const bool wait = request_flush(state_);
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->closed = true;
      state_->changed.notify_all();
    }
    if (wait) wait_drained(state_, deadline);
  } catch (...) {
  }
}

std::size_t Reporter::pending() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->records.size();
}

bool Reporter::route_missing() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->route_missing;
}

bool Reporter::inert() const { return config_.inert() || route_missing(); }

// --------------------------------------------------------------- operation

Operation::Operation(std::shared_ptr<Reporter> reporter, std::optional<std::string> name,
                     std::string transport, std::optional<std::string> stage)
    : reporter_(std::move(reporter)),
      name_(std::move(name)),
      transport_(std::move(transport)),
      operation_id_(new_id()),
      started_(Clock::now()),
      stage_(stage ? *stage
                   : (transport_ == kTransportHttp ? kStageSendingRequest : kStageConnecting)) {}

void Operation::expect_audio() {
  stage_ = audio_chunks_ > 0 ? kStageReceivingAudio : kStageAwaitingFirstAudio;
}

void Operation::record_chunk(std::size_t bytes) {
  ++audio_chunks_;
  audio_bytes_ += static_cast<std::int64_t>(bytes);
  if (stage_ == kStageAwaitingFirstAudio) stage_ = kStageReceivingAudio;
}

void Operation::record_retry() {
  ++retry_count_;
  server_error_ = nullptr;
  ws_close_code_.reset();
}

void Operation::succeed() {
  if (settled_) return;
  settled_ = true;
  reporter_->note_success();
}

void Operation::cancel() {
  if (settled_) return;
  settled_ = true;
  reporter_->note_cancelled();
}

void Operation::fail(const std::exception_ptr& error) noexcept {
  if (settled_) return;
  settled_ = true;
  // KEEP-JUSTIFIED: runs on the caller's exception path; the original error
  // is rethrown by the caller and must reach it unchanged.
  try {
    reporter_->note_failure();
    reporter_->report(event_name(), attributes(server_error_ ? server_error_ : error));
  } catch (...) {
  }
}

std::string Operation::event_name() const {
  if (retry_count_ > 0) return kEventRetryExhausted;
  if (server_error_) return kEventRequestFailed;
  if (stage_ == kStageConnecting || stage_ == kStageHandshake) return kEventConnectionFailed;
  if (stage_ == kStageAwaitingFirstAudio || stage_ == kStageReceivingAudio ||
      stage_ == kStageFinalizing) {
    return kEventStreamInterrupted;
  }
  return kEventRequestFailed;
}

Attributes Operation::attributes(const std::exception_ptr& error) const {
  const ErrorSummary summary = summarize(error);
  Attributes attrs;
  attrs.emplace_back("kugel.operation_id", operation_id_);
  if (name_) attrs.emplace_back("kugel.operation", *name_);
  attrs.emplace_back("kugel.transport", transport_);
  attrs.emplace_back("kugel.failure_stage", stage_);
  attrs.emplace_back(
      "kugel.elapsed_ms",
      static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count()));
  attrs.emplace_back("kugel.audio_chunks", audio_chunks_);
  attrs.emplace_back("kugel.audio_bytes", audio_bytes_);
  attrs.emplace_back("kugel.retry_count", static_cast<std::int64_t>(retry_count_));
  attrs.emplace_back("kugel.outcome", std::string("failed"));
  attrs.emplace_back("kugel.error_type", summary.type_name);
  if (summary.details.error_code)
    attrs.emplace_back("kugel.error_code", *summary.details.error_code);
  if (summary.details.status_code) {
    attrs.emplace_back("kugel.http_status",
                       static_cast<std::int64_t>(*summary.details.status_code));
  }
  if (summary.details.request_id) {
    attrs.emplace_back("kugel.server_request_id", *summary.details.request_id);
  }
  if (ws_close_code_) {
    attrs.emplace_back("kugel.ws_close_code", static_cast<std::int64_t>(*ws_close_code_));
  }
  return attrs;
}

}  // namespace kugelaudio::detail::diag
