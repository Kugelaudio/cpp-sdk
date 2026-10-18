// Remove background noise from a WAV file, over REST and in real time.
//
//   KUGELAUDIO_API_KEY=... ./enhance_file noisy.wav clean.wav

#include <cstdlib>
#include <iostream>

#include <kugelaudio/kugelaudio.hpp>

int main(int argc, char** argv) {
  const char* key = std::getenv("KUGELAUDIO_API_KEY");
  if (key == nullptr || argc < 3) {
    std::cerr << "Usage: KUGELAUDIO_API_KEY=... enhance_file <in.wav> <out.wav>\n";
    return 1;
  }
  try {
    kugelaudio::Client client{key};
    client.enhance().prewarm();  // start capacity before the first request

    kugelaudio::EnhanceOptions options;
    options.model = "clarity-1";

    // The whole recording at once (any supported WAV, at most 300 s).
    const auto result = client.enhance().generate(kugelaudio::load_audio(argv[1]), options);
    result.save(argv[2]);
    std::cout << "Enhanced " << result.duration() << " s into " << argv[2] << '\n';

    // The same file in real time (16-bit PCM WAV), as chunks come back.
    kugelaudio::Bytes live;
    client.enhance().stream(
        kugelaudio::load_audio_stream(argv[1]), options,
        [&live](const kugelaudio::Bytes& pcm) { live.insert(live.end(), pcm.begin(), pcm.end()); });
    std::cout << "Streamed " << live.size() / 2 << " enhanced samples at 24 kHz\n";
  } catch (const kugelaudio::KugelAudioError& e) {
    std::cerr << e.type_name() << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
