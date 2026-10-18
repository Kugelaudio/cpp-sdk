# KugelAudio C++ SDK

Official C++17 SDK for the [KugelAudio](https://kugelaudio.com) text-to-speech API:
REST generation, WebSocket streaming, token-by-token streaming sessions for LLM
agents, multi-context sessions, voices and voice cloning, pronunciation
dictionaries, word timestamps and speech enhancement.

[Get an API key](https://kugelaudio.com/dashboard) · [API reference](https://docs.kugelaudio.com) · [Error codes](https://docs.kugelaudio.com/api-reference/errors)

## Requirements

- A C++17 compiler (GCC 9+, Clang 10+, Apple Clang, MSVC 2019+)
- CMake 3.20+
- [libcurl](https://curl.se/libcurl/) (HTTP), [IXWebSocket](https://github.com/machinezone/IXWebSocket) 12.0.1 (WebSocket, TLS), [nlohmann/json](https://github.com/nlohmann/json) 3.11+ (header-only, private)

## Installation

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(kugelaudio
  GIT_REPOSITORY https://github.com/Kugelaudio/cpp-sdk.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(kugelaudio)

target_link_libraries(my_app PRIVATE kugelaudio::kugelaudio)
```

libcurl must be installed (`apt install libcurl4-openssl-dev`, or the system
library on macOS). IXWebSocket and nlohmann/json are used from your system when
`find_package` finds them, and downloaded at pinned versions otherwise
(`-DKUGELAUDIO_FETCH_DEPS=OFF` turns the download off).

### Installed package (`find_package`)

```bash
cmake -S cpp-sdk -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /opt/kugelaudio
```

```cmake
find_package(kugelaudio 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE kugelaudio::kugelaudio)
```

Configure your project with `-DCMAKE_PREFIX_PATH=/opt/kugelaudio`.

### vcpkg (overlay port)

Each GitHub release carries the port files (`portfile.cmake`, `vcpkg.json`,
`usage`) with the release tarball's SHA-512 filled in. The copy in
`ports/kugelaudio` of a git checkout is a template with a placeholder hash and
does not install.

```bash
mkdir -p ports/kugelaudio
for f in portfile.cmake vcpkg.json usage; do
  curl -fsSL -o "ports/kugelaudio/$f" \
    "https://github.com/Kugelaudio/cpp-sdk/releases/download/v0.1.0/$f"
done
vcpkg install kugelaudio --overlay-ports=ports
```

```cmake
find_package(kugelaudio CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE kugelaudio::kugelaudio)
```

## Quickstart

### Generate speech

```cpp
#include <kugelaudio/kugelaudio.hpp>

int main() {
  kugelaudio::Client client{std::getenv("KUGELAUDIO_API_KEY")};

  kugelaudio::GenerateRequest request;
  request.text = "Hello from KugelAudio!";
  request.voice_id = 1071;     // required: there is no default voice
  request.language = "en";     // set it when the text is not in the voice's language

  kugelaudio::AudioResponse audio = client.tts().generate(request);
  audio.save_wav("hello.wav"); // PCM16, 24 kHz mono by default
}
```

`generate()` uses `POST /v1/tts/generate`. Every
[generation parameter](https://docs.kugelaudio.com/api-reference/tts/generate) is a
field of `GenerateRequest`: `model_id` (`kugel-3.5` by default), `cfg_scale`
(clamped to 1.2-2.5), `temperature`, `max_new_tokens`, `sample_rate`,
`output_format` (for example `ulaw_8000` for telephony), `normalize`, `language`,
`speed`, `project_id` and `dictionary_ids` (unset applies every active dictionary
of the project, an empty vector applies none).

### Stream one request

```cpp
client.connect();  // optional: open the pooled WebSocket at startup

kugelaudio::StreamCallbacks callbacks;
callbacks.on_audio = [](const kugelaudio::AudioChunk& chunk) {
  play(chunk.audio, chunk.sample_rate);  // your audio output
};
request.word_timestamps = true;
kugelaudio::AudioResponse result = client.tts().stream(request, callbacks);
// result.word_timestamps, result.usage (audio seconds and the charge in EUR cents)
```

`stream()` reuses one pooled WebSocket (`/ws/tts`) across calls. From another
thread, `client.tts().cancel()` barges in: the stream returns with
`result.interrupted == true`. `client.tts().update_settings(...)` sets sticky
generation defaults for every later pooled `stream()`, also across reconnects:
a request field left at its default (`speed` 1.0, `cfg_scale` 2.0, ...) takes
the sticky value, any other value wins.

### Stream LLM tokens (one turn per reply)

```cpp
kugelaudio::StreamConfig config;
config.voice_id = 1071;
config.language = "en";
kugelaudio::StreamingSession session = client.tts().streaming_session(config);

for (const std::string& token : llm_tokens) {
  session.send(token, /*flush=*/false, play_chunk);  // no client-side buffering
}
session.flush(play_chunk);  // ends the turn: final, then session_closed with usage
std::optional<kugelaudio::SessionUsage> usage = session.last_usage();
```

The server chunks the text at sentence boundaries; the configuration is sticky
across turns on the same socket. End every turn with `flush()`: a turn left idle
for 5 seconds is ended by the server with a warning (`on_warning`). Call
`session.cancel()` from any thread to barge in, `update_config()` between turns
to switch voice, `update_settings()` to change generation parameters, and
`drain()` before `close()` to keep the tail of a turn.

### Multi-context sessions

```cpp
auto session = client.tts().multi_context_session();
session.create_context("narrator", 1071);
session.create_context("character", 1072);
auto narration = session.send("narrator", "The story begins.", /*flush=*/true);
auto line = session.send("character", "Hello!", /*flush=*/true);
session.close_context("narrator");            // usage_for("narrator") is now set
session.close_context("character", /*immediate=*/true);  // barge-in
```

Up to 20 contexts share one WebSocket. Audio of one context that arrives while
you wait on another is kept and returned by that context's next call.

### Voices, models and dictionaries

```cpp
for (const auto& model : client.models().list()) std::cout << model.id << '\n';

kugelaudio::VoiceListOptions options;
options.language = "de";
auto page = client.voices().list(options);   // page.voices, page.total

kugelaudio::VoiceCreateRequest voice;
voice.name = "My voice";
voice.sex = "female";
voice.speaker_consent = true;  // you are the speaker or have their permission
voice.reference_files = {"reference.wav"};
auto created = client.voices().create(voice);

auto dictionary = client.dictionaries().create({"Brand names"});
client.dictionaries().entries().add(dictionary.id, {"K8s", "kubernetes"});
```

Voices also support `get`, `update`, `remove`, `list_references`,
`add_reference`, `remove_reference`, `publish` and `generate_sample`;
dictionaries support `list`, `get`, `update`, `remove` and
`entries().list/add/update/remove/replace_all`.

### Speech enhancement

```cpp
kugelaudio::EnhanceOptions options;
options.model = "clarity-1";
// options.speaker = kugelaudio::load_audio("speaker.wav");  // keep only this voice

auto clean = client.enhance().generate(kugelaudio::load_audio("noisy.wav"), options);
clean.save("clean.wav");  // mono PCM16, 24 kHz

client.enhance().stream(kugelaudio::load_audio_stream("noisy.wav"), options,
                        [](const kugelaudio::Bytes& pcm) { /* enhanced 24 kHz PCM16 */ });
```

`client.enhance().prewarm()` starts capacity before the first request, and
`client.enhance().session()` keeps one WebSocket warm for many streams.

## Regions

```cpp
kugelaudio::ClientOptions options;
options.api_key = "YOUR_API_KEY";
options.region = kugelaudio::Region::EU;  // https://api.eu.kugelaudio.com
kugelaudio::Client client{options};
```

An `eu-` prefix on the API key selects the EU endpoint too (the prefix is
stripped before authenticating). Priority: `api_url` > `region` > key prefix >
the canonical geo-routed `https://api.kugelaudio.com`. See
[Regions](https://docs.kugelaudio.com/guides/regions).

## Errors

Every error derives from `kugelaudio::KugelAudioError` and carries
`status_code()`, `error_code()`, `request_id()` and `retry_after()`:

| Class | When |
|---|---|
| `AuthenticationError` | 401, 403 on a WebSocket handshake, close code 4001 |
| `InsufficientCreditsError` | 402, close code 4003 |
| `RateLimitError` | 429, `TOO_MANY_CONTEXTS`, close code 4029 |
| `ValidationError` | 400, `VALIDATION_ERROR`, `MISSING_VOICE_ID`, invalid arguments |
| `NotFoundError` | 404 |
| `ConnectionError` | network errors, timeouts, 503, close code 4500 |
| `ServerRestartingError` | close codes 1012 / 1013 (a `ConnectionError`) |

```cpp
try {
  client.tts().generate(request);
} catch (const kugelaudio::RateLimitError& e) {
  retry_in(e.retry_after().value_or(1));
} catch (const kugelaudio::KugelAudioError& e) {
  std::cerr << e.what() << '\n';  // includes the request id for support
}
```

During a rolling deploy, streaming and multi-context sessions reconnect and
replay a turn that has not produced audio yet; otherwise they throw
`ServerRestartingError` and you decide what to resend.

## Thread safety

- `Client` and its REST resources are thread-safe; share one client.
- `tts().stream()` calls on the pooled socket run one at a time; set
  `StreamOptions::reuse_connection = false` for parallel streams.
- A `StreamingSession` is used from one thread at a time, except `cancel()`,
  which may be called from any thread. A `MultiContextSession` is used from one
  thread at a time.
- Callbacks run on the thread that called the method.
- Sessions keep the client's internals alive and close themselves on
  destruction (RAII).

## Diagnostics

The SDK reports its own failures (error class, failure stage, timings, the
server request id) to the KugelAudio API host you already use, so we can find
problems that never reach our servers. Reports never contain text, audio, URLs,
keys or error messages. They are on by default for the hosted API and off for a
custom `api_url`. Turn them off with:

```cpp
options.telemetry = false;
```

or `KUGELAUDIO_TELEMETRY=0` in the environment (which overrides the option in
both directions). Details: [SDK diagnostics](https://docs.kugelaudio.com/sdks/telemetry).

## Logging

Warnings (for example a missing `language` with normalization on) go to
`std::cerr` by default. Set `ClientOptions::log_handler` to route them:

```cpp
options.log_handler = [](kugelaudio::LogLevel level, const std::string& message) { /* ... */ };
```

## Building the tests and examples

```bash
cmake -S . -B build -G Ninja -DKUGELAUDIO_BUILD_TESTS=ON -DKUGELAUDIO_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The unit and mock-server tests need no network. The live tests run only when
`KUGELAUDIO_API_KEY` is set (`KUGELAUDIO_VOICE_ID` picks the voice).

## License

MIT, see [LICENSE](LICENSE).
