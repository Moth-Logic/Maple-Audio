#include "Transcriber.h"
#include "whisper.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static const uint32_t kWhisperSampleRate = 16000;
static const uint16_t kFormatIeeeFloat = 3;   // WAVE_FORMAT_IEEE_FLOAT

static uint16_t ReadU16(const unsigned char* p) {
    uint16_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t ReadU32(const unsigned char* p) {
    uint32_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

// Reads the WAV files MapleAudio writes: RIFF/WAVE with a 32-bit float fmt chunk
// and a data chunk. Chunk sizes are honoured so a WAV with extra metadata chunks
// (or a truncated recording) is handled instead of being misread as samples.
static bool ReadFloatWav(const std::string& path, std::vector<float>& samples,
                         uint16_t& channels, uint32_t& sampleRate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    unsigned char header[12];
    if (!file.read((char*)header, sizeof(header))) return false;
    if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        std::cerr << path << " is not a RIFF/WAVE file\n";
        return false;
    }

    bool haveFormat = false;
    while (file.read((char*)header, 8)) {
        const char* chunkId = (const char*)header;
        const uint32_t chunkSize = ReadU32(header + 4);

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                std::cerr << "Malformed fmt chunk in " << path << "\n";
                return false;
            }

            unsigned char fmt[16];
            if (!file.read((char*)fmt, sizeof(fmt))) return false;

            const uint16_t formatTag = ReadU16(fmt + 0);
            channels = ReadU16(fmt + 2);
            sampleRate = ReadU32(fmt + 4);
            const uint16_t bitsPerSample = ReadU16(fmt + 14);

            if (formatTag != kFormatIeeeFloat || bitsPerSample != 32) {
                std::cerr << "Unsupported WAV format: expected 32-bit float, got "
                          << bitsPerSample << "-bit format tag " << formatTag << "\n";
                return false;
            }
            if (channels == 0 || sampleRate == 0) {
                std::cerr << "Invalid WAV format: " << channels << " channels at "
                          << sampleRate << " Hz\n";
                return false;
            }

            haveFormat = true;
            // Skip whatever follows the 16-byte fmt fields, plus the pad byte.
            file.seekg((std::streamoff)chunkSize - 16 + (chunkSize & 1), std::ios::cur);
        }
        else if (std::memcmp(chunkId, "data", 4) == 0) {
            if (!haveFormat) {
                std::cerr << path << " has audio data before its format chunk\n";
                return false;
            }

            // Trust the smaller of the declared size and what is actually in the file.
            const std::streampos dataStart = file.tellg();
            file.seekg(0, std::ios::end);
            const std::streamoff available = (std::streamoff)(file.tellg() - dataStart);
            file.seekg(dataStart, std::ios::beg);

            const std::streamoff toRead =
                std::min<std::streamoff>((std::streamoff)chunkSize, available);
            if (toRead < (std::streamoff)sizeof(float)) {
                std::cerr << path << " contains no audio samples\n";
                return false;
            }

            samples.resize((size_t)(toRead / (std::streamoff)sizeof(float)));
            file.read((char*)samples.data(),
                      (std::streamsize)(samples.size() * sizeof(float)));
            const std::streamsize bytesRead = file.gcount();
            samples.resize((size_t)std::max<std::streamsize>(bytesRead, 0) / sizeof(float));

            if (samples.empty()) {
                std::cerr << path << " contains no audio samples\n";
                return false;
            }
            return true;
        }
        else {
            // Unknown chunk (LIST, fact, ...): skip it, including the pad byte.
            file.seekg((std::streamoff)chunkSize + (chunkSize & 1), std::ios::cur);
        }
    }

    std::cerr << "No audio data chunk found in " << path << "\n";
    return false;
}

// Whisper wants mono audio at 16,000 Hz. This averages the channels together
// and then resamples with simple linear interpolation.
static std::vector<float> ToWhisperFormat(const std::vector<float>& in,
                                          uint16_t channels, uint32_t sampleRate) {
    if (channels == 0 || sampleRate == 0) return {};

    const size_t frames = in.size() / channels;
    if (frames == 0) return {};

    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (uint16_t c = 0; c < channels; ++c) sum += in[i * channels + c];
        mono[i] = sum / channels;
    }

    if (sampleRate == kWhisperSampleRate) return mono;

    const double ratio = (double)sampleRate / kWhisperSampleRate;
    const size_t outCount = (size_t)(mono.size() / ratio);
    std::vector<float> out(outCount);
    for (size_t i = 0; i < outCount; ++i) {
        const double pos = i * ratio;
        const size_t idx = (size_t)pos;
        const double frac = pos - idx;
        const float a = mono[idx];
        const float b = (idx + 1 < mono.size()) ? mono[idx + 1] : a;
        out[i] = (float)(a + (b - a) * frac);
    }
    return out;
}

// Whisper reports segment times in units of 10 ms.
static void PrintTimestamp(int64_t centiseconds) {
    const int64_t totalSeconds = centiseconds / 100;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t seconds = totalSeconds % 60;
    const auto twoDigits = [](int64_t value) {
        return (value < 10 ? std::string("0") : std::string()) + std::to_string(value);
    };

    if (hours > 0) std::cout << hours << ":" << twoDigits(minutes) << ":" << twoDigits(seconds);
    else std::cout << minutes << ":" << twoDigits(seconds);
}

bool TranscribeWavFile(const std::string& wavPath, const std::string& modelPath) {
    std::vector<float> raw;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    if (!ReadFloatWav(wavPath, raw, channels, sampleRate)) {
        std::cerr << "Could not read " << wavPath << "\n";
        return false;
    }

    const std::vector<float> audio = ToWhisperFormat(raw, channels, sampleRate);
    if (audio.empty()) {
        std::cerr << "No usable audio found in " << wavPath << "\n";
        return false;
    }

    std::cout << "Loaded " << audio.size() / (double)kWhisperSampleRate
              << " seconds of audio. Loading model...\n";

    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (!ctx) {
        std::cerr << "Could not load model: " << modelPath
                  << " (put ggml-base.bin in models/ or next to MapleAudio.exe)\n";
        return false;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = "auto";          // detect Spanish or English automatically
    unsigned threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) threadCount = 4;
    wparams.n_threads = (int)std::min(threadCount, 8u);
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;

    std::cout << "Transcribing...\n\n";
    if (whisper_full(ctx, wparams, audio.data(), (int)audio.size()) != 0) {
        std::cerr << "Transcription failed\n";
        whisper_free(ctx);
        return false;
    }

    const int segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < segments; ++i) {
        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
        const char* text = whisper_full_get_segment_text(ctx, i);
        std::cout << "[";
        PrintTimestamp(t0);
        std::cout << "] " << (text != nullptr ? text : "") << "\n";
    }

    if (segments == 0) std::cout << "(No speech detected)\n";

    whisper_free(ctx);
    return true;
}
