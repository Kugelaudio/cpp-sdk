#include <iostream>

#include <kugelaudio/kugelaudio.hpp>

int main() {
  kugelaudio::ClientOptions options;
  options.api_key = "eu-not-a-real-key";
  options.telemetry = false;
  kugelaudio::Client client(options);
  std::cout << "kugelaudio " << kugelaudio::kVersion << " -> " << client.api_url() << '\n';
  return client.api_url() == kugelaudio::kEuApiUrl ? 0 : 1;
}
