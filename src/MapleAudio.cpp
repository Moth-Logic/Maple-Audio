#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include "Transcriber.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static const int kRecordSeconds = 5;
static const char* kWavFile = "loopback_test.wav";
static const char* kModelFile = "ggml-base.bin";

// Mix format memory is owned by the audio client and must be freed with CoTaskMemFree.
using MixFormatPtr = std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>;

// Writes a standard WAV file: a 44-byte header followed by raw 32-bit float samples.
static bool WriteWav(const char* path, const std::vector<float>& samples, const WAVEFORMATEX* fmt) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    const uint16_t channels = fmt->nChannels;
    const uint32_t dataBytes = (uint32_t)(samples.size() * sizeof(float));
    const uint32_t riffSize = 36 + dataBytes;
    const uint32_t fmtSize = 16;
    const uint16_t formatTag = WAVE_FORMAT_IEEE_FLOAT;
    const uint32_t sampleRate = fmt->nSamplesPerSec;
    const uint16_t bits = 32;
    const uint16_t blockAlign = (uint16_t)(channels * bits / 8);
    const uint32_t byteRate = sampleRate * blockAlign;

    file.write("RIFF", 4);
    file.write((const char*)&riffSize, 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write((const char*)&fmtSize, 4);
    file.write((const char*)&formatTag, 2);
    file.write((const char*)&channels, 2);
    file.write((const char*)&sampleRate, 4);
    file.write((const char*)&byteRate, 4);
    file.write((const char*)&blockAlign, 2);
    file.write((const char*)&bits, 2);
    file.write("data", 4);
    file.write((const char*)&dataBytes, 4);
    file.write((const char*)samples.data(), dataBytes);
    return file.good();
}

// The shared-mode mix format is almost always 32-bit float, but it can also be
// wrapped in WAVE_FORMAT_EXTENSIBLE. Anything else would be misread as floats.
static bool IsFloat32(const WAVEFORMATEX* fmt) {
    if (fmt == nullptr) return false;
    if (fmt->nChannels == 0 || fmt->nSamplesPerSec == 0) return false;
    if (fmt->wBitsPerSample != 32) return false;

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    return false;
}

// The README tells people to drop the model in models/ or next to the exe.
static std::string FindModel(const std::string& fileName) {
    const std::string candidates[] = { "models/" + fileName, fileName };
    for (const std::string& candidate : candidates) {
        std::ifstream probe(candidate, std::ios::binary);
        if (probe.good()) return candidate;
    }
    return candidates[0];
}

static int Fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

static int Run() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(hr)) return Fail("Could not create device enumerator");

    // The default playback device. We capture it in loopback mode.
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
    if (FAILED(hr)) return Fail("No default output device");

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return Fail("Could not activate audio client");

    WAVEFORMATEX* rawFormat = nullptr;
    hr = client->GetMixFormat(&rawFormat);
    if (FAILED(hr) || rawFormat == nullptr) return Fail("Could not read the device mix format");
    MixFormatPtr fmt(rawFormat, CoTaskMemFree);

    if (!IsFloat32(fmt.get())) {
        std::cerr << "Unsupported format: expected 32-bit float, got " << fmt->wBitsPerSample
                  << "-bit format tag " << fmt->wFormatTag << "\n";
        return 1;
    }

    // Buffer length is in 100-nanosecond units: 10,000,000 = 1 second.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000, 0, fmt.get(), nullptr);
    if (FAILED(hr)) {
        std::cerr << "Initialize failed (hr=0x" << std::hex << hr << ")\n";
        return 1;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(IID_PPV_ARGS(capture.GetAddressOf()));
    if (FAILED(hr)) return Fail("Could not get capture client");

    std::cout << "Recording " << kRecordSeconds << " seconds from the default output ("
              << fmt->nChannels << " channels at " << fmt->nSamplesPerSec << " Hz)...\n";
    hr = client->Start();
    if (FAILED(hr)) return Fail("Could not start the audio client");

    std::vector<float> samples;
    samples.reserve((size_t)fmt->nSamplesPerSec * fmt->nChannels * kRecordSeconds);
    const auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(kRecordSeconds)) {
        Sleep(10);

        UINT32 packetFrames = 0;
        hr = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            client->Stop();
            std::cerr << "Could not read the capture buffer (hr=0x" << std::hex << hr << ")\n";
            return 1;
        }

        while (packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                client->Stop();
                std::cerr << "Could not get an audio packet (hr=0x" << std::hex << hr << ")\n";
                return 1;
            }

            const size_t count = (size_t)frames * fmt->nChannels;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || data == nullptr) {
                samples.insert(samples.end(), count, 0.0f);
            }
            else {
                const float* src = (const float*)data;
                samples.insert(samples.end(), src, src + count);
            }

            capture->ReleaseBuffer(frames);
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) break;
        }
    }

    client->Stop();

    if (!WriteWav(kWavFile, samples, fmt.get())) {
        std::cerr << "Could not write " << kWavFile << "\n";
        return 1;
    }
    std::cout << "Saved " << kWavFile << " (" << samples.size() / fmt->nChannels
              << " frames at " << fmt->nSamplesPerSec << " Hz)\n";

    if (!TranscribeWavFile(kWavFile, FindModel(kModelFile))) return 1;
    return 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }

    const int result = Run();
    CoUninitialize();
    return result;
}
