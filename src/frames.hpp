#pragma once

// Helpers shared by the WebSocket clients: truthiness of frame fields (the
// server's JSON is read the way the Python SDK reads it) and receive timeouts.

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace kugelaudio::detail {

/// Default wait for a server reply on a session.
inline constexpr std::chrono::milliseconds kRecvTimeout{30000};
/// Short poll after an open-ended send whose text may not end a sentence.
inline constexpr std::chrono::milliseconds kPollTimeout{50};
/// After `chunk_complete`, how long to wait for another sentence to start.
inline constexpr std::chrono::milliseconds kInterChunkIdle{2000};
/// Delay before reconnecting after a rolling-deploy close without a hint.
inline constexpr std::chrono::milliseconds kRestartDelay{1000};

/// True when `key` exists and is truthy (true, non-zero, non-empty).
inline bool truthy(const nlohmann::json& frame, const char* key) {
  if (!frame.is_object()) return false;
  auto it = frame.find(key);
  if (it == frame.end()) return false;
  const auto& v = *it;
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number_integer()) return v.get<long long>() != 0;
  if (v.is_number_float()) return v.get<double>() != 0.0;
  if (v.is_string()) return !v.get<std::string>().empty();
  if (v.is_array() || v.is_object()) return !v.empty();
  return false;
}

/// True when `key` is present (any value).
inline bool has(const nlohmann::json& frame, const char* key) {
  return frame.is_object() && frame.contains(key);
}

}  // namespace kugelaudio::detail
