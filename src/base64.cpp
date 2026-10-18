#include "base64.hpp"

#include <array>
#include <cstdint>

#include "kugelaudio/errors.hpp"

namespace kugelaudio::detail {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<int, 256> make_reverse() {
  std::array<int, 256> table{};
  for (auto& v : table) v = -1;
  for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kAlphabet[i])] = i;
  return table;
}

constexpr std::array<int, 256> kReverse = make_reverse();

}  // namespace

std::string base64_encode(const Bytes& data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back(kAlphabet[n & 63]);
  }
  const std::size_t rest = data.size() - i;
  if (rest == 1) {
    const std::uint32_t n = data[i] << 16;
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out += "==";
  } else if (rest == 2) {
    const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(kAlphabet[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

Bytes base64_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 4 * 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int value = kReverse[static_cast<unsigned char>(c)];
    if (value < 0) throw KugelAudioError("Malformed base64 audio from the server.");
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace kugelaudio::detail
