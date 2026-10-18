// Stream one request over the WebSocket: audio chunks arrive while the rest is
// still being generated, then the final stats and usage.
//
//   KUGELAUDIO_API_KEY=... ./streaming_turn

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
    client.connect();  // warm the WebSocket so the first request starts faster

    kugelaudio::GenerateRequest request;
    request.text = "Streaming audio starts playing before the sentence is finished.";
    request.voice_id = 1071;
    request.language = "en";
    request.word_timestamps = true;

    kugelaudio::StreamCallbacks callbacks;
    callbacks.on_audio = [](const kugelaudio::AudioChunk& chunk) {
      std::cout << "chunk " << chunk.index << ": " << chunk.samples << " samples\n";
      // Hand chunk.audio (PCM16, chunk.sample_rate Hz) to your audio device here.
    };
    const auto result = client.tts().stream(request, callbacks);
    for (const auto& word : result.word_timestamps) {
      std::cout << word.word << " " << word.start_ms << "-" << word.end_ms << " ms\n";
    }
    if (result.usage && result.usage->cost_cents) {
      std::cout << "Charged " << *result.usage->cost_cents << " EUR cents\n";
    }
    result.save_wav("streamed.wav");
  } catch (const kugelaudio::KugelAudioError& e) {
    std::cerr << e.type_name() << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
