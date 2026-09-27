#pragma once
#include <atomic>
#include <cstdint>
#include <string>
// Frames: repeated uint32_t audio-sample timestamp + 240x240 YUV420.
// PCM: signed 16-bit mono, 16000 Hz. Output is H.264 + AAC in MP4.
bool encode_video_note(const std::string& frames, const std::string& pcm, const std::string& output,
                       uint32_t samples, const std::atomic<bool>& cancel, std::atomic<int>& progress,
                       std::string& error);
