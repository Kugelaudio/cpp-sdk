#pragma once

/// @file errors.hpp
/// The exception hierarchy. Every error the SDK throws derives from
/// kugelaudio::KugelAudioError, and the subclasses map to the server's
/// `error_code` field, HTTP status and WebSocket close code, so callers can
/// `catch (const kugelaudio::RateLimitError&)` without parsing messages.

#include <optional>
#include <stdexcept>
#include <string>

namespace kugelaudio {

/// Structured details carried by every KugelAudioError.
struct ErrorDetails {
  /// HTTP status (or the HTTP-style `code` of a WebSocket error frame).
  std::optional<int> status_code;
  /// Machine-readable server error code, e.g. "RATE_LIMITED".
  std::optional<std::string> error_code;
  /// Correlation id of the call: the `x-request-id` header or the
  /// `request_id` field of a WebSocket error frame. Quote it to support.
  std::optional<std::string> request_id;
  /// Seconds to wait before retrying (429 / 503 / restart), when known.
  std::optional<int> retry_after;
};

/// Base class of every error the SDK throws.
class KugelAudioError : public std::runtime_error {
 public:
  /// @param message Human-readable, actionable text.
  /// @param details Status, error code, request id and retry hint.
  explicit KugelAudioError(const std::string& message, ErrorDetails details = {});

  /// The message without the appended request id.
  const std::string& message() const noexcept { return message_; }
  /// HTTP status, if known.
  const std::optional<int>& status_code() const noexcept { return details_.status_code; }
  /// Server `error_code`, if known.
  const std::optional<std::string>& error_code() const noexcept { return details_.error_code; }
  /// Server request id, if known.
  const std::optional<std::string>& request_id() const noexcept { return details_.request_id; }
  /// Retry hint in seconds, if known.
  const std::optional<int>& retry_after() const noexcept { return details_.retry_after; }
  /// All structured details at once.
  const ErrorDetails& details() const noexcept { return details_; }
  /// The class name, e.g. "RateLimitError" (used by diagnostics).
  virtual const char* type_name() const noexcept { return "KugelAudioError"; }

 private:
  std::string message_;
  ErrorDetails details_;
};

/// The API key was missing, malformed or rejected (401, 403 on a WebSocket
/// handshake, close code 4001).
class AuthenticationError : public KugelAudioError {
 public:
  /// @param message Empty for the default actionable message.
  explicit AuthenticationError(const std::string& message = {}, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "AuthenticationError"; }
};

/// The organization is out of credits (402, close code 4003).
class InsufficientCreditsError : public KugelAudioError {
 public:
  /// @param message Empty for the default actionable message.
  explicit InsufficientCreditsError(const std::string& message = {}, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "InsufficientCreditsError"; }
};

/// The per-organization rate or concurrency limit was hit (429, close code
/// 4029, TOO_MANY_CONTEXTS). retry_after() carries the server's hint.
class RateLimitError : public KugelAudioError {
 public:
  /// @param message Empty for a default message that includes retry_after.
  explicit RateLimitError(const std::string& message = {}, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "RateLimitError"; }
};

/// The request was rejected as invalid (400, VALIDATION_ERROR,
/// MISSING_VOICE_ID), or the SDK rejected an argument before sending it.
class ValidationError : public KugelAudioError {
 public:
  explicit ValidationError(const std::string& message, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "ValidationError"; }
};

/// A referenced resource does not exist or is not visible to the caller (404).
class NotFoundError : public KugelAudioError {
 public:
  /// @param message Empty for "Not found.".
  explicit NotFoundError(const std::string& message = {}, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "NotFoundError"; }
};

/// The SDK could not reach KugelAudio: network error, timeout, a dropped
/// WebSocket, or a temporarily unavailable model (503, close code 4500).
class ConnectionError : public KugelAudioError {
 public:
  explicit ConnectionError(const std::string& message, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "ConnectionError"; }
};

/// The replica serving a WebSocket is restarting (close code 1012 / 1013).
///
/// Streaming and multi-context sessions absorb this while the turn is still
/// silent: they reconnect after retry_after() and replay the turn once. The
/// error reaches the caller only when audio of the turn was already delivered
/// (a replay would repeat it) or the turn was replayed once already.
/// Audio received before it is complete up to the last chunk; reconnect and
/// resend the turn that was in flight.
class ServerRestartingError : public ConnectionError {
 public:
  /// @param message Empty for the default message. retry_after defaults to 1.
  explicit ServerRestartingError(const std::string& message = {}, ErrorDetails details = {});
  const char* type_name() const noexcept override { return "ServerRestartingError"; }
};

}  // namespace kugelaudio
