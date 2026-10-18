#include "url.hpp"

#include <algorithm>
#include <cctype>

#include "kugelaudio/errors.hpp"
#include "kugelaudio/version.hpp"

namespace kugelaudio {

Region parse_region(std::string_view region) {
  if (region == "eu") return Region::EU;
  if (region == "us") return Region::US;
  if (region == "global") return Region::Global;
  throw ValidationError("Invalid region '" + std::string(region) +
                        "'. Must be one of: eu, us, global");
}

const char* to_string(Region region) noexcept {
  switch (region) {
    case Region::EU:
      return "eu";
    case Region::US:
      return "us";
    case Region::Global:
      return "global";
  }
  return "global";
}

namespace detail {

std::pair<std::string, std::optional<Region>> parse_api_key(const std::string& api_key) {
  for (Region region : {Region::EU, Region::US, Region::Global}) {
    const std::string prefix = std::string(to_string(region)) + "-";
    if (api_key.rfind(prefix, 0) == 0) return {api_key.substr(prefix.size()), region};
  }
  return {api_key, std::nullopt};
}

std::string resolve_region_url(std::optional<Region> region) {
  if (region && *region == Region::EU) return kEuApiUrl;
  return kDefaultApiUrl;
}

std::string strip_trailing_slashes(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

std::string resolve_api_url(const std::optional<std::string>& api_url, std::optional<Region> region,
                            std::optional<Region> key_region) {
  if (api_url && !api_url->empty()) return strip_trailing_slashes(*api_url);
  return resolve_region_url(region ? region : key_region);
}

std::string to_ws_url(const std::string& http_url) {
  if (http_url.rfind("https://", 0) == 0) return "wss://" + http_url.substr(8);
  if (http_url.rfind("http://", 0) == 0) return "ws://" + http_url.substr(7);
  return http_url;
}

std::string url_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

std::string build_query(const QueryParams& params) {
  std::string out;
  for (const auto& [key, value] : params) {
    if (!out.empty()) out.push_back('&');
    out += url_encode(key) + "=" + url_encode(value);
  }
  return out;
}

std::string append_query(const std::string& url, const QueryParams& params) {
  if (params.empty()) return url;
  return url + (url.find('?') == std::string::npos ? "?" : "&") + build_query(params);
}

QueryParams sdk_query_params() { return {{"sdk", kSdkName}, {"sdk_version", kVersion}}; }

HeaderList sdk_headers() {
  return {{"X-KugelAudio-SDK", kSdkName},
          {"X-KugelAudio-SDK-Version", kVersion},
          {"User-Agent", std::string("kugelaudio-cpp/") + kVersion}};
}

HeaderList auth_headers(const std::string& clean_key) {
  return {{"Authorization", "Bearer " + clean_key}, {"X-API-Key", clean_key}};
}

std::string host_of(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return {};
  std::string rest = url.substr(scheme + 3);
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
  if (!rest.empty() && rest.front() == '[') {
    rest = rest.substr(
        1, rest.find(']') == std::string::npos ? std::string::npos : rest.find(']') - 1);
  } else if (const auto colon = rest.find(':'); colon != std::string::npos) {
    rest = rest.substr(0, colon);
  }
  std::transform(rest.begin(), rest.end(), rest.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return rest;
}

std::string redact_queries(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool in_query = false;
  for (char c : text) {
    if (c == '?') {
      in_query = true;
      out += "?<redacted>";
      continue;
    }
    if (in_query && (c == ' ' || c == '\n' || c == ',' || c == '"')) in_query = false;
    if (!in_query) out.push_back(c);
  }
  return out;
}

}  // namespace detail
}  // namespace kugelaudio
