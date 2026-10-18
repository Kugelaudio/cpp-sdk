#pragma once

/// @file options.hpp
/// Client configuration: API key, endpoint selection, timeouts, telemetry and
/// logging.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace kugelaudio {

/// Default canonical, geo-routed API endpoint.
inline constexpr const char* kDefaultApiUrl = "https://api.kugelaudio.com";
/// Direct EU endpoint.
inline constexpr const char* kEuApiUrl = "https://api.eu.kugelaudio.com";
/// Default text-to-speech model.
inline constexpr const char* kDefaultModel = "kugel-3.5";

/// Region hint. Only Region::EU has a dedicated endpoint; US and Global are
/// accepted compatibility hints that use the canonical endpoint.
enum class Region { EU, US, Global };

/// Parse "eu", "us" or "global".
/// @throws ValidationError for any other value.
Region parse_region(std::string_view region);

/// The lowercase name of a region ("eu", "us", "global").
const char* to_string(Region region) noexcept;

/// Severity of an SDK log message.
enum class LogLevel { Debug, Info, Warning };

/// Receives SDK log messages. Called from the thread that produced them.
using LogHandler = std::function<void(LogLevel level, const std::string& message)>;

/// Everything a kugelaudio::Client can be configured with.
///
/// Endpoint priority: `api_url` > `region` > an `eu-`/`us-`/`global-` API key
/// prefix > the canonical endpoint.
struct ClientOptions {
  /// Your API key, optionally prefixed with `eu-` to select the EU endpoint
  /// (the prefix is stripped before authenticating).
  std::string api_key;
  /// API base URL; takes precedence over `region` and any key prefix.
  std::optional<std::string> api_url;
  /// TTS WebSocket base URL; defaults to the API URL (the API proxies it).
  std::optional<std::string> tts_url;
  /// Region hint; takes precedence over an API-key prefix.
  std::optional<Region> region;
  /// HTTP request timeout, WebSocket handshake timeout and the longest wait
  /// for the next frame of a one-shot stream.
  std::chrono::milliseconds timeout{std::chrono::seconds(60)};
  /// Interval of WebSocket ping frames on the pooled TTS connection;
  /// `std::nullopt` disables them.
  std::optional<std::chrono::seconds> keepalive_ping_interval{std::chrono::seconds(20)};
  /// Opt in or out of anonymous client-error diagnostics. `std::nullopt`
  /// enables them only for the hosted `*.kugelaudio.com` API. The
  /// `KUGELAUDIO_TELEMETRY` environment variable (`1/true/on/yes` or
  /// `0/false/off/no`) overrides this field. Reports never contain text,
  /// audio, URLs, keys or error messages.
  std::optional<bool> telemetry;
  /// Receives SDK warnings (and debug/info messages). When empty, warnings
  /// are written to `std::cerr` and everything else is dropped.
  LogHandler log_handler;
};

}  // namespace kugelaudio
