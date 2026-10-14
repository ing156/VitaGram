#pragma once
#include "i18n.h"
#include <functional>
#include <string>

// Finds or prepares a smaller H.264 MP4 for SceVideodec on the media worker.
// cache_only never starts conversion. Otherwise requires the user's acceptance.
bool prepare_portrait_video(const std::string& source, std::string& prepared,
                            const std::function<bool()>& cancelled,
                            const std::function<void(int)>& progress,
                            std::string& error, bool cache_only = false);

bool prepare_portrait_video(const std::string& source, std::string& prepared,
                            const std::function<bool()>& cancelled,
                            const std::function<void(int)>& progress,
                            i18n::Text& error, bool cache_only = false);
