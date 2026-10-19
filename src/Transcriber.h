#pragma once
#include <string>

// Reads a WAV file written by MapleAudio, converts it to what Whisper expects,
// runs the model, and prints each segment with its timestamp.
bool TranscribeWavFile(const std::string& wavPath, const std::string& modelPath);