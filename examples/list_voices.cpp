// List the German voices visible to your API key, page by page.
//
//   KUGELAUDIO_API_KEY=... ./list_voices

#include <cstdlib>
#include <iostream>

#include <kugelaudio/kugelaudio.hpp>

int main() {
  const char* key = std::getenv("KUGELAUDIO_API_KEY");
  if (key == nullptr) {
    std::cerr << "Set KUGELAUDIO_API_KEY\n";
    return 1;
  }
  try {
    kugelaudio::Client client{key};
    for (const auto& model : client.models().list()) {
      std::cout << "model " << model.id << " (" << model.name << ")\n";
    }
    kugelaudio::VoiceListOptions options;
    options.language = "de";
    options.limit = 50;
    while (true) {
      const auto page = client.voices().list(options);
      for (const auto& voice : page.voices) {
        std::cout << voice.id << "  " << voice.name << "  " << voice.category.value_or("-") << '\n';
      }
      options.offset += static_cast<int>(page.voices.size());
      if (page.voices.empty() || options.offset >= page.total) break;
    }
  } catch (const kugelaudio::KugelAudioError& e) {
    std::cerr << e.type_name() << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
