#pragma once

// Maps HTTP responses, WebSocket error frames, handshake rejections and close
// codes to the typed errors of errors.hpp. One route for every transport.

#include <exception>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "kugelaudio/errors.hpp"

namespace kugelaudio::detail {

/// Response headers with lowercase names.
using Headers = std::map<std::string, std::string>;

inline constexpr const char* kErrorUnauthorized = "UNAUTHORIZED";
inline constexpr const char* kErrorRateLimited = "RATE_LIMITED";
inline constexpr const char* kErrorInsufficientCredits = "INSUFFICIENT_CREDITS";
inline constexpr const char* kErrorModelUnavailable = "MODEL_UNAVAILABLE";
inline constexpr const char* kErrorValidation = "VALIDATION_ERROR";
inline constexpr const char* kErrorNotFound = "NOT_FOUND";
inline constexpr const char* kErrorMissingVoiceId = "MISSING_VOICE_ID";
inline constexpr const char* kErrorTooManyContexts = "TOO_MANY_CONTEXTS";

inline constexpr int kWsCloseNormal = 1000;
inline constexpr int kWsCloseAbnormal = 1006;
inline constexpr int kWsCloseUnauthorized = 4001;
inline constexpr int kWsCloseInsufficientCredits = 4003;
inline constexpr int kWsCloseRateLimited = 4029;
inline constexpr int kWsCloseModelUnavailable = 4500;
inline constexpr int kWsCloseServiceRestart = 1012;
inline constexpr int kWsCloseTryAgainLater = 1013;

/// True for 1012 / 1013 (rolling deploy).
bool is_restart_close(int code) noexcept;
/// True for the server close codes a caller must see as an error.
bool is_error_close(int code) noexcept;

/// Pick the error class for a status / error_code pair (Python `_build`).
std::exception_ptr build_error(std::optional<int> status, std::optional<std::string> error_code,
                               const std::string& message,
                               std::optional<std::string> request_id = std::nullopt,
                               std::optional<int> retry_after = std::nullopt);

/// The typed error of an HTTP error response: reads the JSON body
/// (`error`/`detail`, `error_code`, `retry_after`), then `Retry-After` and
/// `x-request-id` headers, then the raw body text.
std::exception_ptr classify_http_response(int status, const std::string& body,
                                          const Headers& headers);

/// The typed error of a server WebSocket error frame
/// (`{error, error_code, code, request_id, retry_after}`).
std::exception_ptr classify_ws_frame(const nlohmann::json& frame);

/// The typed error of a rejected WebSocket upgrade. A 403 during the
/// handshake is an AuthenticationError.
std::exception_ptr classify_handshake_rejection(int status, const Headers& headers,
                                                const std::string& message);

/// The typed error of a WebSocket close code and reason.
std::exception_ptr classify_ws_close(std::optional<int> code, const std::string& reason);

/// The class name and details of an error, for diagnostics. A non-SDK
/// exception yields its generic name ("std::exception" or "unknown") and no
/// details.
struct ErrorSummary {
  std::string type_name;
  ErrorDetails details;
};
ErrorSummary summarize(const std::exception_ptr& error);

}  // namespace kugelaudio::detail
