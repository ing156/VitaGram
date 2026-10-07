#pragma once
#include <functional>
#include <string>

// Prepares a smaller H.264 MP4 for SceVideodec. Called only from the media
// worker, after the user accepts the conversion prompt.
bool prepare_portrait_video(const std::string& source, std::string& prepared,
                            const std::function<bool()>& cancelled,
                            const std::function<void(int)>& progress,
                            std::string& error);
