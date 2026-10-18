// Feed LLM-style tokens into a streaming session as they arrive and play the
// audio of every finished sentence right away. One turn per assistant reply.
//
//   KUGELAUDIO_API_KEY=... ./stream_input_llm

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include <kugelaudio/kugelaudio.hpp>

int main() {
  const char* key = std::getenv("KUGELAUDIO_API_KEY");
  if (key == nullptr) {
    std::cerr << "Set KUGELAUDIO_API_KEY\n";
    return 1;
  }
  try {
    kugelaudio::Client client{key};
    kugelaudio::StreamConfig config;
    config.voice_id = 1071;
    config.language = "en";
    auto session = client.tts().streaming_session(config);
    session.connect();

    kugelaudio::Bytes pcm;
    const auto play = [&pcm](const kugelaudio::AudioChunk& chunk) {
      pcm.insert(pcm.end(), chunk.audio.begin(), chunk.audio.end());
    };

    // Tokens as an LLM would stream them; send each one, no client buffering.
    const std::vector<std::string> tokens = {"Sure",   ",",      " I", " can",  " help",
                                             " with",  " that",  ".",  " Your", " order",
                                             " ships", " today", "."};
    for (const auto& token : tokens) {
      session.send(token, false, play);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    session.flush(play);  // end the turn: the rest is synthesized, then final + session_closed

    if (const auto usage = session.last_usage()) {
      std::cout << "Turn: " << usage->audio_seconds << " s of audio\n";
    }
    kugelaudio::write_wav("reply.wav", pcm, config.sample_rate);
    session.close();
  } catch (const kugelaudio::KugelAudioError& e) {
    std::cerr << e.type_name() << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
