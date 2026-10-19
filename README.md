# Maple Audio

A lightweight Windows desktop tool that records whatever your computer is playing (lectures, meetings, videos) and transcribes it locally, with timestamps. Everything runs on your PC: no cloud, no accounts, no API keys.

> **Status:** early development. Loopback capture, WAV output, and local transcription work, and the thread-safe audio queue that live transcription will use is in place and tested. Live transcription, the UI, and summaries are not built yet.

## Download

There is no packaged release yet. Build it from source with the steps below; a ready-to-run `.exe` will be published here once the first release is built.

## Why

Taking notes while a lecture or meeting is happening means you miss things. Maple Audio records the audio and turns it into text, so you can focus on listening and review the transcript afterward.

## Features

- Captures system audio through WASAPI loopback (no microphone needed to hear what's playing)
- Transcribes locally with [whisper.cpp](https://github.com/ggerganov/whisper.cpp)
- Handles Spanish, English, and automatic language detection
- Prints transcript lines with timestamps
- Validates the capture format and WAV files instead of trusting them, and reports failures with a non-zero exit code
- Thread-safe queue (`ThreadSafeQueue`) for moving audio chunks between the capture thread and a future transcription thread, with tests
- Planned: live transcription while recording, pause/resume, saved transcripts (`.txt` and `.json`), summaries, and a desktop window

## Requirements

- Windows 10 or 11 (64-bit) — the audio capture code is Windows-only (WASAPI)
- Visual Studio 2022 or newer with the **Desktop development with C++** workload
- CMake 3.20 or newer (included with Visual Studio)
- Git (CMake uses it to download whisper.cpp)
- A Whisper model file, see below

## Getting the Whisper model

The model is not included in the repository because it's about 140 MB. Download `ggml-base.bin` from:

https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin

Place it in the `models/` folder of the project, or next to `MapleAudio.exe`. The program checks `models/ggml-base.bin` first, then `ggml-base.bin` in the working directory.

## Building

### From Visual Studio

1. Open the project folder in Visual Studio (**File > Open > CMake...** and select `CMakeLists.txt`).
2. Select the `x64-debug` preset (or `x64-release`).
3. Build with **Build > Build All** (or `Ctrl+Shift+B`).

The first build takes a few minutes because CMake downloads and compiles whisper.cpp.

### From a developer command prompt

```bat
cmake --preset x64-debug
cmake --build out\build\x64-debug
```

Build output goes to `out/build/<preset>/`, which is ignored by git. Only the Windows presets are usable today; the source uses WASAPI and links `ole32`.

## Tests

The queue has a self-contained test executable (no test framework required):

```bat
ctest --test-dir out\build\x64-debug --output-on-failure
```

Or run `queue_tests.exe` from `out\build\x64-debug` directly. The tests cover FIFO order, blocking and non-blocking pops, closing while a consumer waits, draining queued items after close, rejecting pushes after close, move-only payloads, and a multi-producer/multi-consumer stress run.

## Running

Run `MapleAudio.exe` from Visual Studio, or from the terminal in the folder that contains the model:

```bat
MapleAudio.exe
```

Play something through your speakers or headphones before you start. The program records 5 seconds of the default output, saves it as `loopback_test.wav`, and prints the transcript. The recording is 32-bit float at the device's mix format (often 44.1 or 48 kHz, stereo or more channels); it is downmixed and resampled to 16 kHz mono before transcription.

## Project structure

```text
Maple-Audio/
├── CMakeLists.txt
├── CMakePresets.json      # x64-debug / x64-release build presets
├── LICENSE
├── README.md
├── models/                # Whisper model files (not committed)
├── src/
│   ├── MapleAudio.cpp     # Entry point: capture, WAV writing, orchestration
│   ├── Transcriber.h      # Whisper wrapper
│   ├── Transcriber.cpp
│   └── Queue/
│       └── ThreadSafeQueue.h
└── tests/
    └── QueueTests.cpp
```

## How it works

1. **Capture:** WASAPI opens the default output device in loopback mode, which copies the sound that's being played. The mix format is checked to be 32-bit float before any samples are read.
2. **Convert:** Whisper needs 16 kHz mono audio, so the recording is downmixed and resampled.
3. **Transcribe:** Whisper runs locally on your CPU and returns text segments with timestamps.

### The audio queue

`ThreadSafeQueue<T>` is an unbounded FIFO built for one production rule: the capture loop must never wait. `Push` never blocks, `Pop` blocks until an item arrives, and `TryPop` is non-blocking. `Close` wakes every waiting consumer but still delivers items already queued, so the last chunk of a session is not dropped. This is the piece live transcription will be built on.

## Privacy

Audio never leaves your computer. Transcription runs locally. Recording other people may require their consent depending on where you are and the context, so check the rules that apply to you.

## Roadmap

- [x] Record system audio (loopback)
- [x] Local transcription with whisper.cpp
- [x] Thread-safe audio queue with tests
- [ ] Live transcription in chunks while recording
- [ ] Single-file `.exe` (static linking)
- [ ] Pause, resume, and stop
- [ ] Save transcripts to disk
- [ ] Summaries of selected text
- [ ] Desktop window (Qt)

## License

This project is released into the public domain under the [Unlicense](https://unlicense.org/). See the `LICENSE` file for details.

Note: whisper.cpp is MIT-licensed. Keep its license notice if you distribute it bundled in the `.exe`.
