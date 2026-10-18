#pragma once

#include <string>
#include <string_view>

#include "kugelaudio/types.hpp"

namespace kugelaudio::detail {

/// Standard base64 (RFC 4648) with padding.
std::string base64_encode(const Bytes& data);

/// Decode standard base64; whitespace is ignored, padding optional.
/// @throws KugelAudioError on a character outside the alphabet.
Bytes base64_decode(std::string_view text);

}  // namespace kugelaudio::detail
