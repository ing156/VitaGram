#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace vpk {
struct Metadata { std::string title_id, title, content_id; };
// Strict bounded parsing of untrusted package metadata; only application packages.
bool parse_sfo(const std::vector<uint8_t>& bytes, Metadata& metadata);
// Retarget an installed homebrew package header and recompute its format checksums.
bool make_head(std::vector<uint8_t>& head, const Metadata& metadata);
bool filename(const std::string& name);
}
