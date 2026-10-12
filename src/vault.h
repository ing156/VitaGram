#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Optional encryption of everything VitaGram stores on the memory card, unlocked by a key made
// of console buttons.
//
// Key hierarchy
//   PIN (12..32 presses of D-pad/cross/circle/triangle/square, 3 bits each), or a passphrase
//     -> scrypt(N=2^14, r=8, p=2, random 32-byte salt)         memory-hard, ~16 MiB, ~1-2 s on Vita
//     -> HKDF-SHA256 with the console binding (OpenPSID hash)   a copied card alone is useless
//     -> KEK; the random 256-bit master key (MK) is stored AEAD-wrapped under it (vault.json).
//   Changing the PIN re-wraps MK only; no data is re-encrypted. MK -> HKDF -> separate file and
//   TDLib database keys, and a state key that signs vault.json's mode and migration direction, so
//   an edited file cannot turn the owner's next unlock into a decryption. "off" cannot be signed
//   (there is no key then); a file switched to "off" from outside only stops new data being sealed.
//
// File container ("VGV1", one per file): 24-byte header (magic, version, cipher, chunk size,
// 16-byte random salt) then 64 KiB chunks of ChaCha20-Poly1305. The per-file key is
// HMAC(file key, salt); the nonce is the chunk number plus a "last chunk" flag and the header is
// authenticated data, so chunks cannot be reordered, cut off, appended or moved between files
// undetected. Fixed-size chunks give O(1) random access for video seeking.
//
// What cannot be protected: file names, sizes and timestamps; plaintext that must exist for the
// platform (a download in progress, a recording until TDLib has uploaded it, files the user
// explicitly saved to the download folder); a console whose RAM or running app an attacker controls.
namespace vault {
// The secret: for Method::Buttons each element is 0..7 (Up Down Left Right Cross Circle Triangle Square),
// for Method::Text the UTF-8 bytes of the passphrase.
using Pin = std::vector<uint8_t>;
constexpr int kSymbols = 8;
constexpr size_t kMinPin = 12, kMaxPin = 32;

// How the key is entered: console buttons (quick) or a typed passphrase (stronger).
enum class Method { Buttons, Text };
constexpr size_t kMinText = 10, kMaxText = 128; // characters / bytes of a passphrase

enum class State {
    Undecided, // never asked (new install or an update from a version without the vault)
    Off,       // the user declined
    Locked,    // enabled, key not entered yet
    Unlocked,
};
enum class Result { Ok, WrongPin, TooShort, Unavailable, NoMemory, Error };
// Called with 0..100 while the key is derived; use it to draw a progress bar.
using Progress = void (*)(void* context, int percent);

// Test hook / relocation of the configuration file. Default: ux0:data/vitagram/vault.json.
void set_config_path(const std::string& path);
void load();
State state();
inline bool undecided() { return state() == State::Undecided; }
// A vault exists (locked or unlocked), including while it is being switched off.
bool enabled();
bool unlocked();
// Newly written data must be sealed (unlocked and not being switched off).
bool sealing();
// The user chose to switch encryption off; existing files are being returned to plain text.
bool disabling();
// Files written before the vault was enabled (or sealed copies after disabling) still wait for
// the migration sweep.
bool migration_pending();
void set_migration_pending(bool pending);

Method method(); // of the current vault; Buttons when there is none
Result enable(const Pin& pin, Method method = Method::Buttons, Progress progress = nullptr, void* context = nullptr);
// Also checks the signed security state in vault.json (mode, migration direction); see state_was_reset.
Result unlock(const Pin& pin, Progress progress = nullptr, void* context = nullptr);
// After unlock(): the state was edited outside VitaGram (or was an unsigned "disabling" from an old
// version) and was reset to "encryption on, re-check every file". Tell the user.
bool state_was_reset();
// After unlock(): the state carried no valid signature (edited, stripped, or written by a version
// before signing - indistinguishable). Caches and the "settings were saved" flag are not trusted then.
bool state_unauthenticated();
// settings.json has been saved under this (authenticated) vault, so it must exist and be readable.
bool settings_expected();
// Called after settings.json was written sealed; records that in the signed state.
void note_settings_saved();
// Checks a key without changing any state (used before asking for a new one).
Result verify_pin(const Pin& pin, Progress progress = nullptr, void* context = nullptr);
Result change_pin(const Pin& old_pin, const Pin& new_pin, Method new_method = Method::Buttons, Progress progress = nullptr, void* context = nullptr);
// Verifies the key and starts switching off: the vault stays unlocked (reading sealed files,
// writing plain ones) until finish_disable().
Result begin_disable(const Pin& pin, Progress progress = nullptr, void* context = nullptr);
bool finish_disable();
bool decline();
// "Forgot the key": removes the vault configuration; the caller has to erase the sealed data.
bool reset();
void lock();
// Seconds the UI should refuse another attempt after repeated wrong keys (0 = none).
int lockout_seconds();
// Pin quality: 3 bits per press. Rejects runs of one button and trivial sequences.
bool pin_acceptable(const Pin& pin, std::string* why = nullptr);
int pin_strength_bits(const Pin& pin);
// Same for either method. Text strength is a deliberately cautious estimate (human-chosen text is
// far from random): half of length * log2(character pool).
bool secret_acceptable(const Pin& secret, Method method, std::string* why = nullptr);
int secret_strength_bits(const Pin& secret, Method method);

// 64 hex characters for TDLib's database_encryption_key; "" when not unlocked.
std::string database_key();
// 32-byte data key for sealed JSON in tests and helpers; false when not unlocked.
bool file_key(uint8_t out[32]);

// ---- sealed file I/O ---------------------------------------------------------------------
// Size of a sealed file holding `plain` bytes, and the inverse (-1 for impossible sizes).
int64_t sealed_size(int64_t plain);
int64_t plain_size_from_sealed(int64_t sealed);
// True when the file starts with the vault magic (sealed with either key).
bool is_sealed(const std::string& path);
// True only for files sealed with the vault's own key (not console-key caches).
bool is_vault_sealed(const std::string& path);
// Plaintext size of a file whether sealed or not (-1 when missing).
int64_t plain_size(const std::string& path);

// What a reader accepts. `Vault`: only files sealed with the vault's own key - for application state
// (settings, caches) that must not be replaced by plain or console-key files while the vault is on.
enum class Require { Any, Vault };
// With the vault on (and not being switched off), settings.json must be sealed with the vault key.
bool settings_must_be_sealed();
// Same for the caches, which the migration converts: only once no migration is pending.
bool caches_must_be_sealed();

// Reads plain files and sealed ones alike (Require::Any). Sealed files need an unlocked vault.
class Reader {
public:
    Reader() = default;
    ~Reader() { close(); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    bool open(const std::string& path, Require require = Require::Any);
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool sealed() const { return sealed_; }
    // Sealed with the console key (a cache written while the vault was off).
    bool console_sealed() const { return console_; }
    int64_t size() const { return size_; }
    int64_t tell() const { return pos_; }
    // Diagnostics: ciphertext bytes read and microseconds spent reading + decrypting them.
    int64_t stat_bytes() const { return stat_bytes_; }
    int64_t stat_micros() const { return stat_micros_; }       // reading + decrypting
    int64_t stat_io_micros() const { return stat_io_micros_; } // of which waiting for the card
    // fseek semantics; returns the new offset or -1.
    int64_t seek(int64_t offset, int whence);
    // Bytes read, 0 at the end, -1 on error or when authentication fails.
    int64_t read(void* buffer, size_t length);
private:
    bool load_chunk(int64_t index);
    bool read_at(int64_t offset, void* buffer, size_t length);
    int fd_ = -1;
    bool sealed_ = false, console_ = false;
    int64_t size_ = 0, pos_ = 0, file_size_ = 0, chunk_index_ = -1, chunk_length_ = 0, file_at_ = -1;
    int64_t raw_first_ = -1, raw_chunks_ = 0;
    int64_t stat_bytes_ = 0, stat_micros_ = 0, stat_io_micros_ = 0;
    uint8_t header_[24] = {};
    uint8_t key_[32] = {};
    std::vector<uint8_t> chunk_;  // decrypted chunk
    std::vector<uint8_t> raw_;    // read-ahead window of sealed chunks
};

// Reads a whole file (at most `max_bytes`) through Reader. False when missing, too large or corrupt.
bool read_file(const std::string& path, size_t max_bytes, std::string& out,
               bool (*proceed)(void*) = nullptr, void* context = nullptr, Require require = Require::Any);

// Writes plain or sealed data. close() must succeed for the data to count.
class Writer {
public:
    Writer() = default;
    ~Writer() { abort(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    // seal defaults to sealing(): new files are sealed whenever the vault is on. console_key seals
    // with the console-bound key instead (caches while the vault is off).
    bool open(const std::string& path, bool seal, bool console_key = false);
    // A locked vault refuses to write at all rather than fall back to plain text.
    bool open(const std::string& path) { return (!enabled() || unlocked()) && open(path, sealing()); }
    // Caches: the vault key when the vault is on, otherwise the console key (plain without a console id).
    bool open_cache(const std::string& path);
    bool write(const void* data, size_t length);
    bool close();
    void abort();
    bool is_open() const { return file_ != nullptr; }
    // For callers that already batch their writes into large blocks.
    void set_unbuffered() { if (file_) std::setvbuf(file_, nullptr, _IONBF, 0); }
private:
    bool emit_chunk(bool last);
    FILE* file_ = nullptr;
    bool sealed_ = false, failed_ = false;
    uint64_t chunk_counter_ = 0;
    uint8_t header_[24] = {};
    uint8_t key_[32] = {};
    std::vector<uint8_t> buffer_;
};

// Writes `data` atomically (temporary file + rename), sealed when the vault is on.
bool write_file_atomic(const std::string& path, const std::string& data);
// Converts one file in place (temporary file + rename). Already converted files are left alone.
enum class Convert { Done, Unchanged, Failed, Interrupted };
Convert seal_file(const std::string& path, bool (*proceed)(void*) = nullptr, void* context = nullptr);
Convert unseal_file(const std::string& path, bool (*proceed)(void*) = nullptr, void* context = nullptr);
}
