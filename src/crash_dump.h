#pragma once
#include <string>

// Crash dumps of VitaGram, removed while the data vault is on.
//
// When an application crashes, the system writes ux0:data/psp2core-<time>-<id>-eboot.bin.psp2dmp: a
// gzip-compressed ELF core file with thread stacks and parts of the heap - decrypted TDLib responses,
// message text, key material on the stack. It lies on the card unencrypted, outside the vault, and the
// file name does not say which application crashed ("eboot.bin" for every homebrew title); the
// title id appears as the main thread's name near the start of the core file.
namespace crash_dump {
// psp2core-*.psp2dmp (including the system's unfinished *.psp2dmp.tmp).
bool is_dump_name(const std::string& name);
// The dump was written for `title_id` (checked in the first 64 KiB of decompressed data).
bool belongs_to(const std::string& path, const char* title_id);
// Overwrites with zeros and deletes every dump of `title_id` in `directory`. Returns how many were removed.
int purge(const std::string& directory, const char* title_id);
}
