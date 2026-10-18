// Generate one sentence over REST and save it as a WAV file.
//
//   KUGELAUDIO_API_KEY=... ./generate_wav "Hello from KugelAudio!" hello.wav

#include <cstdlib>
#include <iostream>

#include <kugelaudio/kugelaudio.hpp>

int main(int argc, char** argv) {
  const char* key = std::getenv("KUGELAUDIO_API_KEY");
  if (key == nullptr) {
    std::cerr << "Set KUGELAUDIO_API_KEY\n";
    return 1;
  }
  try {
    kugelaudio::Client client{key};
    kugelaudio::GenerateRequest request;
    request.text = argc > 1 ? argv[1] : "Hello from the KugelAudio C++ SDK.";
    request.voice_id = 1071;
    request.language = "en";
    const auto audio = client.tts().generate(request);
    const char* path = argc > 2 ? argv[2] : "output.wav";
    audio.save_wav(path);
    std::cout << "Wrote " << audio.duration_seconds() << " s of audio to " << path << '\n';
  } catch (const kugelaudio::KugelAudioError& e) {
    std::cerr << e.type_name() << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
