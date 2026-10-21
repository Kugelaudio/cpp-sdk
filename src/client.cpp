#include "kugelaudio/client.hpp"

#include <atomic>
#include <iostream>

#include "client_core.hpp"
#include "error_mapping.hpp"
#include "json_mapping.hpp"
#include "kugelaudio/errors.hpp"

namespace kugelaudio {

namespace detail {

namespace {

std::atomic<bool> g_language_warning_logged{false};

std::string format_seconds(std::chrono::milliseconds duration) {
  const auto ms = duration.count();
  if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
  return std::to_string(ms) + "ms";
}

}  // namespace

std::optional<std::string> operation_for_path(const std::string& path) {
  std::string trimmed = path;
  while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(0, 1);
  if (trimmed.rfind("v1/", 0) == 0) trimmed = trimmed.substr(3);
  const std::string head = trimmed.substr(0, trimmed.find('/'));
  if (head == "models") return std::string(diag::kOpModels);
  if (head == "voices") return std::string(diag::kOpVoices);
  if (head == "dictionaries") return std::string(diag::kOpDictionaries);
  if (head == "tts") return std::string(diag::kOpGenerate);
  return std::nullopt;
}

ClientCore::ClientCore(const ClientOptions& options, diag::Sender sender)
    : timeout(options.timeout),
      keepalive_ping_interval(options.keepalive_ping_interval),
      log_handler(options.log_handler) {
  if (options.api_key.empty()) {
    throw ValidationError(
        "KugelAudio API key is missing. Pass it to the client. Get a key at "
        "https://www.kugelaudio.com/dashboard.");
  }
  auto [clean_key, key_region] = parse_api_key(options.api_key);
  api_key = std::move(clean_key);
  api_url = resolve_api_url(options.api_url, options.region, key_region);
  tts_url = strip_trailing_slashes(options.tts_url && !options.tts_url->empty() ? *options.tts_url
                                                                                : api_url);
  diagnostics = diag::Reporter::create(
      diag::Config::resolve(api_url, options.telemetry, auth_headers(api_key)), std::move(sender));
}

void ClientCore::log(LogLevel level, const std::string& message) const {
  if (log_handler) {
    log_handler(level, message);
  } else if (level == LogLevel::Warning) {
    std::cerr << "kugelaudio: " << message << '\n';
  }
}

void ClientCore::warn_if_no_language(const std::optional<std::string>& language,
                                     bool normalize) const {
  if (language || !normalize) return;
  if (g_language_warning_logged.exchange(true)) return;
  log(LogLevel::Warning,
      "No 'language' set with normalization enabled: the server normalizes in the voice's "
      "primary language (English if it has none). Set language (e.g. \"de\") when the text "
      "is in another language.");
}

HeaderList ClientCore::headers() const {
  HeaderList out = auth_headers(api_key);
  for (auto& header : sdk_headers()) out.push_back(std::move(header));
  return out;
}

std::string ClientCore::url(const std::string& path) const { return api_url + path; }

HttpResponse ClientCore::send(HttpRequest request, const std::string& path) const {
  HttpResponse response;
  try {
    response = http.perform(request);
  } catch (const TransportError& e) {
    if (e.timed_out()) {
      throw ConnectionError("Request to " + request.method + " " + path + " timed out after " +
                            format_seconds(timeout) + ".");
    }
    throw ConnectionError("Could not reach KugelAudio at " + request.url + ": " + e.what() +
                          ". Check network connectivity.");
  }
  if (response.status >= 400) {
    std::rethrow_exception(
        classify_http_response(response.status, response.body, response.headers));
  }
  return response;
}

nlohmann::json ClientCore::request(const std::string& method, const std::string& path,
                                   const QueryParams& query,
                                   const std::optional<nlohmann::json>& body) const {
  auto op = diagnostics->operation(operation_for_path(path), diag::kTransportHttp,
                                   std::string(diag::kStageSendingRequest));
  try {
    HttpRequest request;
    request.method = method;
    request.url = append_query(url(path), query);
    request.headers = headers();
    request.timeout = timeout;
    if (body) request.json_body = dump_json(*body);
    const HttpResponse response = send(std::move(request), path);
    nlohmann::json parsed = (response.status == 204 || response.body.empty())
                                ? nlohmann::json()
                                : parse_json(response.body, method + " " + path);
    op.succeed();
    return parsed;
  } catch (...) {
    op.fail(std::current_exception());
    throw;
  }
}

nlohmann::json ClientCore::request_multipart(const std::string& method, const std::string& path,
                                             std::vector<MultipartPart> parts) const {
  auto op = diagnostics->operation(operation_for_path(path), diag::kTransportHttp,
                                   std::string(diag::kStageSendingRequest));
  try {
    HttpRequest request;
    request.method = method;
    request.url = url(path);
    request.headers = headers();
    request.timeout = timeout;
    request.multipart = std::move(parts);
    const HttpResponse response = send(std::move(request), path);
    nlohmann::json parsed = parse_json(response.body, method + " " + path);
    op.succeed();
    return parsed;
  } catch (...) {
    op.fail(std::current_exception());
    throw;
  }
}

WsConnectOptions ClientCore::ws_options(const std::string& path, const QueryParams& query,
                                        std::optional<std::chrono::seconds> ping_interval) const {
  QueryParams params = query;
  for (auto& param : sdk_query_params()) params.push_back(std::move(param));
  WsConnectOptions options;
  options.url = append_query(to_ws_url(tts_url) + path, params);
  options.headers = headers();
  options.handshake_timeout = timeout;
  options.ping_interval = ping_interval;
  return options;
}

WsConnectOptions ClientCore::api_ws_options(const std::string& path,
                                            const QueryParams& query) const {
  QueryParams params = query;
  for (auto& param : sdk_query_params()) params.push_back(std::move(param));
  WsConnectOptions options;
  options.url = append_query(to_ws_url(api_url) + path, params);
  options.headers = headers();
  options.handshake_timeout = timeout;
  return options;
}

}  // namespace detail

struct Client::Impl {
  explicit Impl(const ClientOptions& options)
      : core(std::make_shared<detail::ClientCore>(options)),
        models(core),
        voices(core),
        dictionaries(core),
        tts(core),
        enhance(core) {}

  std::shared_ptr<detail::ClientCore> core;
  ModelsResource models;
  VoicesResource voices;
  DictionariesResource dictionaries;
  TtsResource tts;
  EnhanceResource enhance;
  bool closed = false;
};

namespace {

ClientOptions options_from_key(std::string api_key) {
  ClientOptions options;
  options.api_key = std::move(api_key);
  return options;
}

}  // namespace

Client::Client(std::string api_key) : Client(options_from_key(std::move(api_key))) {}

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>(options)) {}

Client::~Client() {
  // KEEP-JUSTIFIED: a destructor must not throw; close() only releases
  // sockets and flushes telemetry.
  try {
    close();
  } catch (...) {
  }
}

Client::Client(Client&&) noexcept = default;

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    // KEEP-JUSTIFIED: see the destructor; the replaced client is released first.
    try {
      close();
    } catch (...) {
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ModelsResource& Client::models() { return impl_->models; }
VoicesResource& Client::voices() { return impl_->voices; }
DictionariesResource& Client::dictionaries() { return impl_->dictionaries; }
TtsResource& Client::tts() { return impl_->tts; }
EnhanceResource& Client::enhance() { return impl_->enhance; }

void Client::connect(const std::string& model_id) { impl_->tts.connect(model_id); }
bool Client::is_connected() const { return impl_->tts.is_connected(); }
const std::string& Client::api_url() const { return impl_->core->api_url; }
const std::string& Client::tts_url() const { return impl_->core->tts_url; }

void Client::close() {
  if (!impl_ || impl_->closed) return;
  impl_->closed = true;
  impl_->tts.disconnect();
  impl_->core->diagnostics->close();
}

}  // namespace kugelaudio
