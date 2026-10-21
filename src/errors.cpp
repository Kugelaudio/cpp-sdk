#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "error_mapping.hpp"

namespace kugelaudio {

namespace {

constexpr const char* kApiKeysUrl = "https://www.kugelaudio.com/dashboard";
constexpr const char* kBillingUrl = "https://www.kugelaudio.com/dashboard";

std::string with_request_id(const std::string& message, const ErrorDetails& details) {
  if (details.request_id && !details.request_id->empty()) {
    return message + " (request_id: " + *details.request_id + ")";
  }
  return message;
}

ErrorDetails defaults(ErrorDetails details, int status, const char* error_code) {
  if (!details.status_code) details.status_code = status;
  if (!details.error_code && error_code != nullptr) details.error_code = error_code;
  return details;
}

std::string rate_limit_message(const std::string& message, const ErrorDetails& details) {
  if (!message.empty()) return message;
  if (details.retry_after && *details.retry_after > 0) {
    return "KugelAudio rate limit hit; retry after " + std::to_string(*details.retry_after) + "s.";
  }
  return "KugelAudio rate limit hit; retry shortly.";
}

ErrorDetails restart_defaults(ErrorDetails details) {
  if (!details.status_code) details.status_code = 503;
  if (!details.retry_after) details.retry_after = 1;
  return details;
}

}  // namespace

KugelAudioError::KugelAudioError(const std::string& message, ErrorDetails details)
    : std::runtime_error(with_request_id(message, details)),
      message_(message),
      details_(std::move(details)) {}

AuthenticationError::AuthenticationError(const std::string& message, ErrorDetails details)
    : KugelAudioError(message.empty() ? std::string("KugelAudio rejected the API key. Check it "
                                                    "is current at ") +
                                            kApiKeysUrl + "."
                                      : message,
                      defaults(std::move(details), 401, detail::kErrorUnauthorized)) {}

InsufficientCreditsError::InsufficientCreditsError(const std::string& message, ErrorDetails details)
    : KugelAudioError(message.empty()
                          ? std::string("Your KugelAudio account is out of credits. Top up at ") +
                                kBillingUrl + "."
                          : message,
                      defaults(std::move(details), 402, detail::kErrorInsufficientCredits)) {}

RateLimitError::RateLimitError(const std::string& message, ErrorDetails details)
    : KugelAudioError(rate_limit_message(message, details),
                      defaults(details, 429, detail::kErrorRateLimited)) {}

ValidationError::ValidationError(const std::string& message, ErrorDetails details)
    : KugelAudioError(message, defaults(std::move(details), 400, detail::kErrorValidation)) {}

NotFoundError::NotFoundError(const std::string& message, ErrorDetails details)
    : KugelAudioError(message.empty() ? "Not found." : message,
                      defaults(std::move(details), 404, detail::kErrorNotFound)) {}

ConnectionError::ConnectionError(const std::string& message, ErrorDetails details)
    : KugelAudioError(message, defaults(std::move(details), 503, nullptr)) {}

ServerRestartingError::ServerRestartingError(const std::string& message, ErrorDetails details)
    : ConnectionError(message.empty()
                          ? "KugelAudio replica is restarting (rolling deploy). Reconnect and "
                            "resend the current turn; audio already received is complete up to "
                            "the last chunk."
                          : message,
                      restart_defaults(std::move(details))) {}

namespace detail {

namespace {

std::optional<std::string> header(const Headers& headers, const std::string& lowercase_name) {
  auto it = headers.find(lowercase_name);
  if (it == headers.end() || it->second.empty()) return std::nullopt;
  return it->second;
}

std::optional<int> parse_int(const std::string& text) {
  if (text.empty()) return std::nullopt;
  std::size_t used = 0;
  try {
    int value = std::stoi(text, &used);
    if (used != text.size()) return std::nullopt;
    return value;
  } catch (const std::exception&) {
    // KEEP-JUSTIFIED: a malformed Retry-After is the same as no hint.
    return std::nullopt;
  }
}

std::string trim(std::string text) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

std::string message_from(const nlohmann::json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_array()) {
    std::string joined;
    for (const auto& item : value) {
      if (!joined.empty()) joined += "; ";
      joined += item.is_string() ? item.get<std::string>() : item.dump();
    }
    return joined;
  }
  if (value.is_null()) return {};
  return value.dump();
}

}  // namespace

bool is_restart_close(int code) noexcept {
  return code == kWsCloseServiceRestart || code == kWsCloseTryAgainLater;
}

bool is_error_close(int code) noexcept {
  return code == kWsCloseUnauthorized || code == kWsCloseInsufficientCredits ||
         code == kWsCloseRateLimited || code == kWsCloseModelUnavailable || is_restart_close(code);
}

std::exception_ptr build_error(std::optional<int> status, std::optional<std::string> error_code,
                               const std::string& message, std::optional<std::string> request_id,
                               std::optional<int> retry_after) {
  ErrorDetails details{status, error_code, std::move(request_id), retry_after};
  const std::string code = error_code.value_or("");
  const int http = status.value_or(0);

  if (code == kErrorUnauthorized || http == 401) {
    return std::make_exception_ptr(AuthenticationError(message, details));
  }
  if (code == kErrorInsufficientCredits || http == 402) {
    return std::make_exception_ptr(InsufficientCreditsError(message, details));
  }
  if (code == kErrorRateLimited || code == kErrorTooManyContexts || http == 429) {
    return std::make_exception_ptr(RateLimitError(message, details));
  }
  if (code == kErrorValidation || code == kErrorMissingVoiceId || http == 400) {
    return std::make_exception_ptr(
        ValidationError(message.empty() ? "Request validation failed." : message, details));
  }
  if (code == kErrorModelUnavailable || http == 503) {
    const std::string detail = message.empty() ? "service temporarily unavailable" : message;
    return std::make_exception_ptr(ConnectionError(
        "KugelAudio is temporarily unavailable: " + detail + ". Retry shortly.", details));
  }
  if (code == kErrorNotFound || http == 404) {
    return std::make_exception_ptr(NotFoundError(message, details));
  }
  return std::make_exception_ptr(KugelAudioError(
      message.empty() ? "HTTP " + (status ? std::to_string(*status) : std::string("error"))
                      : message,
      details));
}

std::exception_ptr classify_http_response(int status, const std::string& body,
                                          const Headers& headers) {
  std::optional<std::string> error_code;
  std::string message;
  std::optional<int> retry_after;

  const auto parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_object()) {
    if (auto it = parsed.find("error_code"); it != parsed.end() && it->is_string()) {
      error_code = it->get<std::string>();
    }
    if (auto it = parsed.find("error"); it != parsed.end()) message = message_from(*it);
    if (message.empty()) {
      if (auto it = parsed.find("detail"); it != parsed.end()) message = message_from(*it);
    }
    if (auto it = parsed.find("retry_after"); it != parsed.end() && it->is_number_integer()) {
      retry_after = it->get<int>();
    }
  }
  if (!retry_after) {
    if (auto value = header(headers, "retry-after")) retry_after = parse_int(*value);
  }
  if (message.empty()) message = trim(body);
  return build_error(status, error_code, message, header(headers, "x-request-id"), retry_after);
}

std::exception_ptr classify_ws_frame(const nlohmann::json& frame) {
  std::optional<std::string> error_code;
  std::string message = "Server reported an error.";
  std::optional<int> status;
  std::optional<int> retry_after;
  std::optional<std::string> request_id;
  if (frame.is_object()) {
    if (auto it = frame.find("error_code"); it != frame.end() && it->is_string()) {
      error_code = it->get<std::string>();
    }
    if (auto it = frame.find("error");
        it != frame.end() && it->is_string() && !it->get<std::string>().empty()) {
      message = it->get<std::string>();
    }
    if (auto it = frame.find("code"); it != frame.end() && it->is_number_integer()) {
      status = it->get<int>();
    }
    if (auto it = frame.find("retry_after"); it != frame.end() && it->is_number_integer()) {
      retry_after = it->get<int>();
    }
    if (auto it = frame.find("request_id");
        it != frame.end() && it->is_string() && !it->get<std::string>().empty()) {
      request_id = it->get<std::string>();
    }
  }
  return build_error(status, error_code, message, request_id, retry_after);
}

std::exception_ptr classify_handshake_rejection(int status, const Headers& headers,
                                                const std::string& message) {
  auto request_id = header(headers, "x-request-id");
  std::optional<int> retry_after;
  if (auto value = header(headers, "retry-after")) retry_after = parse_int(*value);
  if (status == 403) {
    return std::make_exception_ptr(
        AuthenticationError({}, ErrorDetails{std::nullopt, std::nullopt, request_id, retry_after}));
  }
  return build_error(status, std::nullopt, message, request_id, retry_after);
}

std::exception_ptr classify_ws_close(std::optional<int> code, const std::string& reason_in) {
  const std::string reason = trim(reason_in);
  const int value = code.value_or(0);
  if (value == kWsCloseUnauthorized) {
    std::string msg =
        std::string("KugelAudio rejected the API key. Check it is current at ") + kApiKeysUrl + ".";
    if (!reason.empty()) msg += " (" + reason + ")";
    return std::make_exception_ptr(AuthenticationError(msg));
  }
  if (value == kWsCloseInsufficientCredits) {
    return std::make_exception_ptr(InsufficientCreditsError());
  }
  if (value == kWsCloseRateLimited) return std::make_exception_ptr(RateLimitError());
  if (value == kWsCloseModelUnavailable) {
    return std::make_exception_ptr(
        ConnectionError("KugelAudio model is temporarily unavailable. Retry shortly." +
                        (reason.empty() ? std::string() : " (" + reason + ")")));
  }
  if (is_restart_close(value)) return std::make_exception_ptr(ServerRestartingError());
  const std::string detail = reason.empty() ? "no reason given" : reason;
  const std::string code_text = code ? " (code " + std::to_string(*code) + ")" : "";
  return std::make_exception_ptr(
      ConnectionError("KugelAudio WebSocket closed by server: " + detail + code_text + "."));
}

ErrorSummary summarize(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const KugelAudioError& e) {
    return {e.type_name(), e.details()};
  } catch (const std::exception&) {
    return {"std::exception", {}};
  } catch (...) {
    return {"unknown", {}};
  }
}

}  // namespace detail
}  // namespace kugelaudio
