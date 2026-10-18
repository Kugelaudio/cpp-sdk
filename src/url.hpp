#pragma once

// API key parsing, endpoint resolution and URL building.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kugelaudio/options.hpp"

namespace kugelaudio::detail {

using QueryParams = std::vector<std::pair<std::string, std::string>>;
using HeaderList = std::vector<std::pair<std::string, std::string>>;

/// Value of the `X-KugelAudio-SDK` header and `sdk` query parameter.
inline constexpr const char* kSdkName = "cpp";

/// Strip an `eu-`/`us-`/`global-` prefix from an API key.
/// @return The clean key and the region the prefix selected, if any.
std::pair<std::string, std::optional<Region>> parse_api_key(const std::string& api_key);

/// The API base URL for a region hint (only EU has its own endpoint).
std::string resolve_region_url(std::optional<Region> region);

/// Endpoint resolution: explicit URL > region > key prefix > default.
/// Trailing slashes are removed.
std::string resolve_api_url(const std::optional<std::string>& api_url, std::optional<Region> region,
                            std::optional<Region> key_region);

/// Remove trailing slashes.
std::string strip_trailing_slashes(std::string url);

/// `https://` -> `wss://`, `http://` -> `ws://`.
std::string to_ws_url(const std::string& http_url);

/// Percent-encode a query component (RFC 3986 unreserved characters kept).
std::string url_encode(std::string_view text);

/// `a=1&b=2`, percent-encoded.
std::string build_query(const QueryParams& params);

/// Append `params` to `url` with `?` or `&`.
std::string append_query(const std::string& url, const QueryParams& params);

/// `sdk=cpp&sdk_version=<version>` as query parameters.
QueryParams sdk_query_params();

/// `X-KugelAudio-SDK`, `X-KugelAudio-SDK-Version`, `User-Agent`.
HeaderList sdk_headers();

/// `Authorization: Bearer <key>` and `X-API-Key: <key>`.
HeaderList auth_headers(const std::string& clean_key);

/// Lowercase host of a URL; empty when it cannot be parsed.
std::string host_of(const std::string& url);

/// Remove any query string from text that may contain URLs (error messages
/// of the WebSocket library), so a credential in a URL is never surfaced.
std::string redact_queries(const std::string& text);

}  // namespace kugelaudio::detail
