# qt-kokoro-tts

![build](https://github.com/hyperverse/qt-kokoro-tts/actions/workflows/build.yml/badge.svg)

A Qt 6 `QTextToSpeech` engine plugin backed by [Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M),
so any Qt application can read text in a neural voice instead of espeak-ng.

Built for listening to books in [sioyek](https://github.com/ahrm/sioyek), but it
is a generic Qt plugin and nothing in it is sioyek-specific.

## Why

Qt on Linux speaks through speech-dispatcher, which in practice means espeak-ng:
intelligible, but tiring over a chapter. Kokoro is an 82M-parameter Apache-2.0
model that scores at the top of CPU TTS quality comparisons (UTMOS ≈ 4.45) and
still runs comfortably faster than real time on a desktop CPU.

Measured on this project's reference machines:

| Backend | Hardware | Speed |
|---|---|---|
| Kokoro (`koko`, ONNX CPU) | Ryzen 7 3800X | 2.6× real time |
| Kokoro (`koko`, ONNX CPU) | Ryzen AI 9 HX 370 | 4.9× real time |
| MOSS-TTS-Local-1.5 (Vulkan) | Radeon 890M iGPU | 0.83× real time — too slow to stream |

The plugin also reports `WordByWordProgress`, which the stock `speechd` engine
does not. Applications that use it for follow-along highlighting or scrolling
(sioyek does) get that behaviour back.

## How it works

`say()` splits the utterance into sentence-sized chunks and fetches them from an
OpenAI-compatible `/v1/audio/speech` endpoint, keeping a couple of chunks ahead
of playback so synthesis overlaps audio. The opening chunk is deliberately short
to keep time-to-first-audio down (~1.7 s on a 3800X). Word boundaries are
estimated from each chunk's measured duration and reported as `sayingWord`.

Any OpenAI-compatible Kokoro server works: the local `koko` binary, or a remote
[Lemonade](https://github.com/lemonade-sdk/lemonade) instance.

## Install

### 1. The Kokoro backend

```bash
./packaging/install-kokoro-backend.sh            # ~375 MB into ~/.local/share/kokoro
systemctl --user enable --now kokoro-server.service
```

The unit binds to `127.0.0.1:8099` only.

### 2. The plugin

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

Or build the Arch package from `packaging/PKGBUILD`.

### 3. Verify

```bash
./build/kokoro-speak --voice Michael "Testing the Kokoro plugin."
```

It prints each word with its character offset as the audio plays.

## Configuration

Read from the engine parameters, or these environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `QT_TTS_KOKORO_URL` | `http://127.0.0.1:8099/v1/audio/speech` | endpoint |
| `QT_TTS_KOKORO_MODEL` | `kokoro` | model name sent to the server |
| `QT_TTS_KOKORO_VOICE` | `af_heart` | default voice |
| `QT_TTS_KOKORO_API_KEY` | *(unset)* | `Authorization: Bearer` if set |

To use a Lemonade server instead of a local one:

```bash
export QT_TTS_KOKORO_URL=http://your-server:13305/api/v1/audio/speech
export QT_TTS_KOKORO_MODEL=kokoro-v1
export QT_TTS_KOKORO_API_KEY=...
```

## Using it from sioyek

Qt resolves `QTextToSpeech`'s default constructor to a hardcoded platform
default (`speechd` on Unix) and offers **no** environment variable or setting to
override it. sioyek only ever asks for the default engine, so it cannot reach
this plugin unmodified.

`patches/sioyek-tts-engine-env.patch` is a one-line change that makes sioyek
honour `SIOYEK_TTS_ENGINE`:

```bash
SIOYEK_TTS_ENGINE=kokoro sioyek
```

There is also a `-DKOKORO_TAKEOVER_SPEECHD=ON` build that registers the plugin
under the `speechd` key, intended to avoid patching anything. It works for
applications that request `"speechd"` explicitly, but **not** for Qt's default
constructor: Qt's default-engine selection walks the plugin list in directory
order and gives up on the first entry that fails to load, which changes once the
key is shadowed. The patch is the reliable route.

## Voices

The 20 American English voices are exposed (`Heart`, `Bella`, `Michael`,
`Onyx`, …). `af_heart` is the default and the strongest all-rounder for
long-form narration.

The British voices are deliberately omitted. `koko` phonemizes them with
espeak-ng's `en-gb` code, which returns nothing and yields a ~0.3 s clip for
every British voice — including via Lemonade. Passing `-l en` instead of
`-l en-gb` makes them work on the command line, but the server picks the
language from the voice prefix, so there is no way to reach them through the
HTTP API today.

## Limitations

- English only.
- Word timings are interpolated from chunk durations, not true forced
  alignment. Good enough to drive scrolling; not frame-accurate. `koko` has a
  `--timestamps` flag, but it emits an empty TSV in the `b17` build.
- `setPitch()` returns `false`; Kokoro has no pitch control. Rate is supported
  and maps to Kokoro's `speed`.

## License

MIT. Kokoro-82M itself is Apache-2.0.
