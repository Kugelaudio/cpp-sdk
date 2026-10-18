// Two speakers on one WebSocket: each context has its own voice and queue.
//
//   KUGELAUDIO_API_KEY=... ./multi_context

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
    kugelaudio::MultiContextConfig config;
    config.language = "en";
    auto session = client.tts().multi_context_session(config);
    session.create_context("narrator", 1071);
    session.create_context("character", 1072);

    const auto narrator = session.send("narrator", "The story begins on a quiet night.", true);
    const auto character = session.send("character", "Hello, is anyone there?", true);
    std::cout << "narrator: " << narrator.size() << " chunks, character: " << character.size()
              << " chunks\n";

    session.close_context("narrator");
    session.close_context("character");
    for (const auto& [context, usage] : session.context_usage()) {
      std::cout << context << ": " << usage.audio_seconds << " s\n";
    }
    session.close();
  } catch (const kugelaudio::KugelAudioError& e) {
    std::cerr << e.type_name() << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
