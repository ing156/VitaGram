#include "vault.h"
#include "secure_store.h"
#include "vault_crypto.h"
#include <jansson.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vault {
namespace {
using namespace vault_crypto;

constexpr size_t kHeader = 24, kChunk = 65536, kWindow = 4; // kWindow: chunks fetched per read
constexpr uint8_t kMagic[4] = {0x89, 'V', 'G', 'V'};
constexpr uint32_t kScryptLog = 14, kScryptR = 8, kScryptP = 2;

struct Config {
    State state = State::Undecided;
    bool valid = false, bind = false, disabling = false, migrating = false;
    Method method = Method::Buttons;
    uint32_t log_n = kScryptLog, r = kScryptR, p = kScryptP;
    uint8_t salt[32] = {}, nonce[12] = {}, wrapped[48] = {};
};

std::string g_path = "ux0:data/vitagram/vault.json";
Config g_config;
std::atomic<bool> g_unlocked{false}, g_sealing{false};
uint8_t g_master[32], g_file_key[32];
std::string g_db_key;
int g_fails = 0;
std::chrono::steady_clock::time_point g_lock_until;

std::string aad_for(const Config& c) {
    return "VitaGram vault v1|" + std::to_string(c.log_n) + "|" + std::to_string(c.r) + "|" + std::to_string(c.p) + "|" +
           (c.bind ? "bind" : "free") + (c.method == Method::Text ? "|text" : "");
}

bool write_config_json(json_t* root) {
    std::string temp = g_path + ".tmp";
    bool ok = json_dump_file(root, temp.c_str(), JSON_COMPACT) == 0;
    json_decref(root);
    if (ok && std::rename(temp.c_str(), g_path.c_str()) != 0) { std::remove(temp.c_str()); ok = false; }
    return ok;
}

bool save_config() {
    if (g_config.state == State::Off) return write_config_json(json_pack("{s:i,s:s}", "v", 1, "mode", "off"));
    json_t* root = json_object();
    json_object_set_new(root, "v", json_integer(1));
    json_object_set_new(root, "mode", json_string(g_config.disabling ? "disabling" : "on"));
    json_object_set_new(root, "kdf", json_pack("{s:i,s:i,s:i}", "n", (int)g_config.log_n, "r", (int)g_config.r, "p", (int)g_config.p));
    json_object_set_new(root, "bind", json_boolean(g_config.bind));
    json_object_set_new(root, "method", json_string(g_config.method == Method::Text ? "text" : "buttons"));
    json_object_set_new(root, "migrating", json_boolean(g_config.migrating));
    json_object_set_new(root, "salt", json_string(to_hex(g_config.salt, 32).c_str()));
    json_object_set_new(root, "nonce", json_string(to_hex(g_config.nonce, 12).c_str()));
    json_object_set_new(root, "wrapped", json_string(to_hex(g_config.wrapped, 48).c_str()));
    return write_config_json(root);
}

struct Ticks { Progress progress; void* context; int total, done, last; };
void tick(void* opaque) {
    auto* t = static_cast<Ticks*>(opaque);
    int percent = std::min(99, ++t->done * 100 / std::max(1, t->total));
    if (t->progress && percent != t->last) { t->last = percent; t->progress(t->context, percent); }
}

Result derive_kek(const Pin& pin, const Config& c, uint8_t kek[32], Progress progress, void* context) {
    // Without the console id the binding cannot be rebuilt (other console, or id unreadable).
    uint8_t binding[32];
    bool have_binding = secure_store::console_binding(binding);
    if (c.bind && !have_binding) return Result::Unavailable;
    std::vector<uint8_t> password{'V', 'G', (uint8_t)(c.method == Method::Text ? 'T' : 'P'), (uint8_t)(c.method == Method::Text ? 'X' : 'N'), '1'};
    password.insert(password.end(), pin.begin(), pin.end());
    uint8_t stretched[32];
    Ticks ticks{progress, context, (int)(128 * c.p), 0, -1};
    if (progress) progress(context, 0);
    bool ok = scrypt(password.data(), password.size(), c.salt, 32, 1u << c.log_n, c.r, c.p, stretched, 32, tick, &ticks);
    wipe(password.data(), password.size());
    if (!ok) return Result::NoMemory;
    uint8_t ikm[64];
    std::memcpy(ikm, stretched, 32);
    size_t ikm_len = 32;
    if (c.bind) { std::memcpy(ikm + 32, binding, 32); ikm_len = 64; }
    hkdf(ikm, ikm_len, "VitaGram vault KEK v1", aad_for(c), kek, 32);
    wipe(stretched, 32); wipe(ikm, sizeof(ikm)); wipe(binding, 32);
    if (progress) progress(context, 100);
    return Result::Ok;
}

void install_master(const uint8_t master[32]) {
    std::memcpy(g_master, master, 32);
    hkdf(g_master, 32, "", "VitaGram file key v1", g_file_key, 32);
    uint8_t db[32];
    hkdf(g_master, 32, "", "VitaGram tdlib key v1", db, 32);
    g_db_key = to_hex(db, 32);
    wipe(db, 32);
    g_unlocked = true;
    g_sealing = !g_config.disabling;
    g_config.state = State::Unlocked;
}

bool valid_pin_symbols(const Pin& pin) {
    return std::all_of(pin.begin(), pin.end(), [](uint8_t s) { return s < kSymbols; });
}

Result open_wrapped(const Pin& pin, uint8_t master[32], Progress progress, void* context) {
    if (!g_config.valid) return Result::Error;
    if (pin.empty() || (g_config.method == Method::Buttons && !valid_pin_symbols(pin))) return Result::WrongPin;
    uint8_t kek[32];
    Result r = derive_kek(pin, g_config, kek, progress, context);
    if (r != Result::Ok) return r;
    std::string aad = aad_for(g_config);
    bool ok = aead_open(kek, g_config.nonce, (const uint8_t*)aad.data(), aad.size(), g_config.wrapped, 32,
                        g_config.wrapped + 32, master);
    wipe(kek, 32);
    if (!ok) {
        ++g_fails;
        if (g_fails >= 5) {
            int seconds = std::min(600, 30 << std::min(g_fails - 5, 5));
            g_lock_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        }
        return Result::WrongPin;
    }
    g_fails = 0;
    return Result::Ok;
}

Result wrap_master(const uint8_t master[32], const Pin& pin, Method method, Config& c, Progress progress, void* context) {
    c.method = method;
    if (!random_bytes(c.salt, 32) || !random_bytes(c.nonce, 12)) return Result::Error;
    c.bind = secure_store::available();
    c.log_n = kScryptLog; c.r = kScryptR; c.p = kScryptP;
    uint8_t kek[32];
    Result r = derive_kek(pin, c, kek, progress, context);
    if (r != Result::Ok) return r;
    std::string aad = aad_for(c);
    aead_seal(kek, c.nonce, (const uint8_t*)aad.data(), aad.size(), master, 32, c.wrapped, c.wrapped + 32);
    wipe(kek, 32);
    c.valid = true;
    return Result::Ok;
}

std::string chunk_aad(const uint8_t header[kHeader]) { return std::string((const char*)header, kHeader); }

void chunk_nonce(uint8_t nonce[12], uint64_t index, bool last) {
    std::memset(nonce, 0, 12);
    for (int i = 0; i < 8; ++i) nonce[i] = (uint8_t)(index >> (8 * i));
    nonce[8] = last ? 1 : 0;
}

void file_subkey(const uint8_t header[kHeader], uint8_t out[32]) {
    std::string label = "VGfile1";
    label.append((const char*)header + 8, 16);
    hmac_sha256(g_file_key, 32, label.data(), label.size(), out);
}

bool header_ok(const uint8_t* h) { return !std::memcmp(h, kMagic, 4) && h[4] == 1 && h[5] == 1 && h[6] == 16; }
}  // namespace

void set_config_path(const std::string& path) { g_path = path; }

void load() {
    g_config = Config();
    json_error_t error{};
    json_t* root = json_load_file(g_path.c_str(), 0, &error);
    if (!root) {
        struct stat info{};
        // Missing file: never asked. Present but unreadable: stay locked rather than silently
        // treating sealed data as plain.
        g_config.state = stat(g_path.c_str(), &info) ? State::Undecided : State::Locked;
        return;
    }
    const char* mode = json_string_value(json_object_get(root, "mode"));
    if (mode && !std::strcmp(mode, "off")) g_config.state = State::Off;
    else if (mode && (!std::strcmp(mode, "on") || !std::strcmp(mode, "disabling"))) {
        g_config.state = State::Locked;
        g_config.disabling = !std::strcmp(mode, "disabling");
        json_t* kdf = json_object_get(root, "kdf");
        auto number = [&](const char* key, uint32_t fallback) {
            json_t* v = json_object_get(kdf, key);
            return json_is_integer(v) ? (uint32_t)json_integer_value(v) : fallback;
        };
        g_config.log_n = number("n", 0); g_config.r = number("r", 0); g_config.p = number("p", 0);
        g_config.bind = json_is_true(json_object_get(root, "bind"));
        const char* method = json_string_value(json_object_get(root, "method"));
        g_config.method = method && !std::strcmp(method, "text") ? Method::Text : Method::Buttons;
        g_config.migrating = json_is_true(json_object_get(root, "migrating"));
        const char* salt = json_string_value(json_object_get(root, "salt"));
        const char* nonce = json_string_value(json_object_get(root, "nonce"));
        const char* wrapped = json_string_value(json_object_get(root, "wrapped"));
        // Parameters are bounded so a damaged file cannot demand gigabytes.
        g_config.valid = salt && nonce && wrapped && g_config.log_n >= 10 && g_config.log_n <= 18 &&
                         g_config.r >= 1 && g_config.r <= 16 && g_config.p >= 1 && g_config.p <= 4 &&
                         from_hex(salt, g_config.salt, 32) && from_hex(nonce, g_config.nonce, 12) &&
                         from_hex(wrapped, g_config.wrapped, 48);
    } else g_config.state = State::Locked; // unknown mode: refuse to guess
    json_decref(root);
}

State state() { return g_config.state; }
bool enabled() { return g_config.state == State::Locked || g_config.state == State::Unlocked; }
bool unlocked() { return g_unlocked; }
bool sealing() { return g_sealing; }
bool disabling() { return enabled() && g_config.disabling; }
bool migration_pending() { return enabled() && g_config.migrating; }
void set_migration_pending(bool pending) {
    if (!enabled() || g_config.migrating == pending) return;
    g_config.migrating = pending;
    save_config();
}

bool pin_acceptable(const Pin& pin, std::string* why) {
    auto fail = [&](const char* reason) { if (why) *why = reason; return false; };
    if (pin.size() < kMinPin) return fail("short");
    if (pin.size() > kMaxPin || !valid_pin_symbols(pin)) return fail("invalid");
    bool seen[kSymbols] = {};
    int distinct = 0;
    for (uint8_t s : pin) if (!seen[s]) { seen[s] = true; ++distinct; }
    if (distinct < 3) return fail("weak");
    // A short block repeated to the end (e.g. Up Down Up Down ...) is guessed first.
    for (size_t period = 1; period <= 3; ++period) {
        bool repeats = true;
        for (size_t i = period; i < pin.size() && repeats; ++i) repeats = pin[i] == pin[i - period];
        if (repeats) return fail("weak");
    }
    return true;
}

int pin_strength_bits(const Pin& pin) { return (int)pin.size() * 3; }

namespace {
size_t code_points(const Pin& text) {
    size_t n = 0;
    for (uint8_t b : text) if ((b & 0xC0) != 0x80) ++n;
    return n;
}
}

bool secret_acceptable(const Pin& secret, Method method, std::string* why) {
    if (method == Method::Buttons) return pin_acceptable(secret, why);
    auto fail = [&](const char* reason) { if (why) *why = reason; return false; };
    if (secret.size() > kMaxText) return fail("invalid");
    for (uint8_t b : secret) if (b < 0x20 || b == 0x7F) return fail("invalid");
    if (code_points(secret) < kMinText) return fail("short");
    bool seen[256] = {};
    int distinct = 0;
    for (uint8_t b : secret) if (!seen[b]) { seen[b] = true; ++distinct; }
    if (distinct < 5) return fail("weak");
    for (size_t period = 1; period <= 3; ++period) {
        bool repeats = true;
        for (size_t i = period; i < secret.size() && repeats; ++i) repeats = secret[i] == secret[i - period];
        if (repeats) return fail("weak");
    }
    return true;
}

int secret_strength_bits(const Pin& secret, Method method) {
    if (method == Method::Buttons) return pin_strength_bits(secret);
    bool lower = false, upper = false, digit = false, symbol = false, other = false;
    for (uint8_t b : secret) {
        if (b >= 'a' && b <= 'z') lower = true;
        else if (b >= 'A' && b <= 'Z') upper = true;
        else if (b >= '0' && b <= '9') digit = true;
        else if (b >= 0x80) other = true;
        else symbol = true;
    }
    int pool = (lower ? 26 : 0) + (upper ? 26 : 0) + (digit ? 10 : 0) + (symbol ? 33 : 0) + (other ? 64 : 0);
    if (pool < 2) return 0;
    double bits = (double)code_points(secret) * std::log2((double)pool) / 2.0;
    return (int)std::min(bits, 128.0);
}

Method method() { return g_config.method; }

Result enable(const Pin& pin, Method method, Progress progress, void* context) {
    if (!secret_acceptable(pin, method)) return Result::TooShort;
    uint8_t master[32];
    if (!random_bytes(master, 32)) return Result::Error;
    Config next;
    next.state = State::Unlocked;
    next.migrating = true;
    Result r = wrap_master(master, pin, method, next, progress, context);
    if (r != Result::Ok) { wipe(master, 32); return r; }
    Config previous = g_config;
    g_config = next;
    if (!save_config()) { g_config = previous; wipe(master, 32); return Result::Error; }
    install_master(master);
    wipe(master, 32);
    g_fails = 0;
    return Result::Ok;
}

Result unlock(const Pin& pin, Progress progress, void* context) {
    if (g_unlocked) return Result::Ok;
    if (g_config.state != State::Locked) return Result::Error;
    if (lockout_seconds() > 0) return Result::WrongPin;
    uint8_t master[32];
    Result r = open_wrapped(pin, master, progress, context);
    if (r != Result::Ok) return r;
    install_master(master);
    wipe(master, 32);
    return Result::Ok;
}

Result verify_pin(const Pin& pin, Progress progress, void* context) {
    if (!enabled()) return Result::Error;
    if (lockout_seconds() > 0) return Result::WrongPin;
    uint8_t master[32];
    Result r = open_wrapped(pin, master, progress, context);
    wipe(master, 32);
    return r;
}

Result change_pin(const Pin& old_pin, const Pin& new_pin, Method new_method, Progress progress, void* context) {
    if (!g_unlocked) return Result::Error;
    if (!secret_acceptable(new_pin, new_method)) return Result::TooShort;
    uint8_t master[32];
    Result r = open_wrapped(old_pin, master, progress, context);
    if (r != Result::Ok) return r;
    Config next = g_config;
    r = wrap_master(master, new_pin, new_method, next, progress, context);
    wipe(master, 32);
    if (r != Result::Ok) return r;
    Config previous = g_config;
    g_config = next;
    if (!save_config()) { g_config = previous; return Result::Error; }
    return Result::Ok;
}

Result begin_disable(const Pin& pin, Progress progress, void* context) {
    uint8_t master[32];
    if (g_config.disabling && g_unlocked) return Result::Ok;
    Result r = open_wrapped(pin, master, progress, context);
    if (r != Result::Ok) return r;
    g_config.disabling = true;
    g_config.migrating = true;
    if (!save_config()) { g_config.disabling = false; wipe(master, 32); return Result::Error; }
    if (!g_unlocked) install_master(master);
    g_sealing = false; // from now on files are written plain
    wipe(master, 32);
    return Result::Ok;
}

bool finish_disable() {
    if (!g_config.disabling) return false;
    Config previous = g_config;
    g_config = Config();
    g_config.state = State::Off;
    if (!save_config()) { g_config = previous; return false; }
    lock();
    g_config.state = State::Off;
    return true;
}

bool decline() {
    if (g_config.state != State::Undecided) return false;
    g_config.state = State::Off;
    if (!save_config()) { g_config.state = State::Undecided; return false; }
    return true;
}

bool reset() {
    lock();
    std::remove(g_path.c_str());
    std::remove((g_path + ".tmp").c_str());
    g_config = Config();
    g_fails = 0; g_lock_until = {};
    return true;
}

void lock() {
    g_unlocked = false; g_sealing = false;
    wipe(g_master, 32); wipe(g_file_key, 32);
    if (!g_db_key.empty()) { wipe(&g_db_key[0], g_db_key.size()); g_db_key.clear(); }
    if (g_config.state == State::Unlocked) g_config.state = State::Locked;
}

int lockout_seconds() {
    auto now = std::chrono::steady_clock::now();
    if (now >= g_lock_until) return 0;
    return (int)std::chrono::duration_cast<std::chrono::seconds>(g_lock_until - now).count() + 1;
}

std::string database_key() { return g_unlocked ? g_db_key : std::string(); }

bool file_key(uint8_t out[32]) {
    if (!g_unlocked) return false;
    std::memcpy(out, g_file_key, 32);
    return true;
}

// ---- container sizes ---------------------------------------------------------------------
int64_t sealed_size(int64_t plain) {
    if (plain < 0) return -1;
    int64_t chunks = std::max<int64_t>(1, (plain + (int64_t)kChunk - 1) / (int64_t)kChunk);
    return (int64_t)kHeader + plain + 16 * chunks;
}

int64_t plain_size_from_sealed(int64_t sealed) {
    if (sealed < (int64_t)kHeader + 16) return -1;
    int64_t payload = sealed - (int64_t)kHeader, stride = (int64_t)kChunk + 16;
    int64_t chunks = (payload + stride - 1) / stride;
    int64_t plain = payload - 16 * chunks;
    return plain >= 0 && sealed_size(plain) == sealed ? plain : -1;
}

bool is_sealed(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t head[4] = {};
    bool sealed = std::fread(head, 1, 4, f) == 4 && !std::memcmp(head, kMagic, 4);
    std::fclose(f);
    return sealed;
}

int64_t plain_size(const std::string& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) || !S_ISREG(info.st_mode)) return -1;
    if (info.st_size < (int64_t)kHeader + 16 || !is_sealed(path)) return info.st_size;
    return plain_size_from_sealed(info.st_size);
}

// ---- Reader --------------------------------------------------------------------------------
bool Reader::open(const std::string& path) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;
    // Plain POSIX reads: one system call per request instead of stdio's small internal buffer,
    // which made sealed video playback crawl on the memory card.
    file_size_ = ::lseek(fd_, 0, SEEK_END);
    if (file_size_ < 0 || ::lseek(fd_, 0, SEEK_SET) != 0) { close(); return false; }
    file_at_ = 0;
    uint8_t head[kHeader];
    int64_t got = (int64_t)std::min<int64_t>(file_size_, (int64_t)kHeader);
    if (got > 0 && !read_at(0, head, (size_t)got)) { close(); return false; }
    if (got >= 4 && !std::memcmp(head, kMagic, 4)) {
        int64_t plain = got == (int64_t)kHeader && header_ok(head) ? plain_size_from_sealed(file_size_) : -1;
        uint8_t master_check[32];
        if (plain < 0 || !file_key(master_check)) { close(); return false; }
        wipe(master_check, 32);
        std::memcpy(header_, head, kHeader);
        file_subkey(header_, key_);
        sealed_ = true; size_ = plain;
        chunk_.assign(kChunk, 0);
        raw_.assign((kChunk + 16) * kWindow, 0);
    } else {
        sealed_ = false; size_ = file_size_;
        if (::lseek(fd_, 0, SEEK_SET) != 0) { close(); return false; }
        file_at_ = 0;
    }
    pos_ = 0; chunk_index_ = -1; raw_first_ = -1; raw_chunks_ = 0;
    return true;
}

void Reader::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    wipe(key_, sizeof(key_));
    if (!chunk_.empty()) wipe(chunk_.data(), chunk_.size());
    if (!raw_.empty()) wipe(raw_.data(), raw_.size());
    chunk_.clear(); raw_.clear(); sealed_ = false; size_ = pos_ = 0; chunk_index_ = -1; raw_first_ = -1; raw_chunks_ = 0; file_at_ = -1;
}

bool Reader::read_at(int64_t offset, void* buffer, size_t length) {
    if (offset != file_at_) {
        if (::lseek(fd_, (off_t)offset, SEEK_SET) != (off_t)offset) { file_at_ = -1; return false; }
        file_at_ = offset;
    }
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < length) {
        ssize_t n = ::read(fd_, out + done, length - done);
        if (n <= 0) { file_at_ = -1; return false; }
        done += (size_t)n;
        file_at_ += n;
    }
    return true;
}

bool Reader::load_chunk(int64_t index) {
    if (index == chunk_index_) return true;
    const auto started = std::chrono::steady_clock::now();
    const int64_t chunks = std::max<int64_t>(1, (size_ + (int64_t)kChunk - 1) / (int64_t)kChunk);
    if (index < 0 || index >= chunks) return false;
    const int64_t stride = (int64_t)kChunk + 16;
    if (index < raw_first_ || index >= raw_first_ + raw_chunks_) {
        // Read several chunks with one request: fewer round trips to the card.
        const int64_t offset = (int64_t)kHeader + index * stride;
        // Read ahead only while the stream is sequential; after a jump (or when a demuxer skips
        // streams) fetch just the chunk that is needed.
        const int64_t ahead = (raw_first_ >= 0 && index == raw_first_ + raw_chunks_) ? (int64_t)kWindow : 1;
        const int64_t length = std::min<int64_t>(stride * ahead, file_size_ - offset);
        if (length < 16) { chunk_index_ = -1; return false; }
        const auto io_started = std::chrono::steady_clock::now();
        if (!read_at(offset, raw_.data(), (size_t)length)) { chunk_index_ = -1; raw_first_ = -1; raw_chunks_ = 0; return false; }
        stat_io_micros_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - io_started).count();
        raw_first_ = index; raw_chunks_ = (length + stride - 1) / stride;
        stat_bytes_ += length;
    }
    const int64_t offset_in_file = (int64_t)kHeader + index * stride;
    const int64_t length = std::min<int64_t>(stride, file_size_ - offset_in_file);
    const uint8_t* cipher = raw_.data() + (index - raw_first_) * stride;
    uint8_t nonce[12];
    chunk_nonce(nonce, (uint64_t)index, index == chunks - 1);
    const size_t body = (size_t)length - 16;
    if (length < 16 || !aead_open(key_, nonce, header_, kHeader, cipher, body, cipher + body, chunk_.data())) {
        chunk_index_ = -1;
        return false;
    }
    chunk_index_ = index; chunk_length_ = (int64_t)body;
    stat_micros_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    return true;
}

int64_t Reader::seek(int64_t offset, int whence) {
    if (fd_ < 0) return -1;
    int64_t target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? pos_ + offset : size_ + offset;
    if (target < 0) return -1;
    pos_ = target;
    return pos_;
}

int64_t Reader::read(void* buffer, size_t length) {
    if (fd_ < 0) return -1;
    if (pos_ >= size_ || !length) return 0;
    size_t want = (size_t)std::min<int64_t>((int64_t)length, size_ - pos_);
    if (!sealed_) {
        if (!read_at(pos_, buffer, want)) return -1;
        pos_ += (int64_t)want;
        return (int64_t)want;
    }
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < want) {
        int64_t index = pos_ / (int64_t)kChunk;
        if (!load_chunk(index)) return done ? (int64_t)done : -1;
        size_t within = (size_t)(pos_ - index * (int64_t)kChunk);
        size_t n = std::min<size_t>(want - done, (size_t)chunk_length_ - within);
        if (!n) break;
        std::memcpy(out + done, chunk_.data() + within, n);
        done += n; pos_ += (int64_t)n;
    }
    return (int64_t)done;
}

bool read_file(const std::string& path, size_t max_bytes, std::string& out, bool (*proceed)(void*), void* context) {
    out.clear();
    Reader reader;
    if (!reader.open(path) || reader.size() < 0 || (uint64_t)reader.size() > max_bytes) return false;
    out.resize((size_t)reader.size());
    size_t done = 0;
    while (done < out.size()) {
        if (proceed && !proceed(context)) { out.clear(); return false; }
        int64_t n = reader.read(&out[done], std::min<size_t>(65536, out.size() - done));
        if (n <= 0) { out.clear(); return false; }
        done += (size_t)n;
    }
    return true;
}

// ---- Writer --------------------------------------------------------------------------------
bool Writer::open(const std::string& path, bool seal) {
    abort();
    uint8_t master_check[32];
    if (seal && !file_key(master_check)) return false;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    sealed_ = seal; failed_ = false; chunk_counter_ = 0;
    buffer_.clear();
    if (sealed_) {
        std::memcpy(header_, kMagic, 4);
        header_[4] = 1; header_[5] = 1; header_[6] = 16; header_[7] = 0;
        if (!random_bytes(header_ + 8, 16)) { abort(); return false; }
        file_subkey(header_, key_);
        buffer_.reserve(kChunk + 16);
        if (std::fwrite(header_, 1, kHeader, file_) != kHeader) failed_ = true;
    }
    return true;
}

bool Writer::emit_chunk(bool last) {
    uint8_t nonce[12], tag[16];
    chunk_nonce(nonce, chunk_counter_++, last);
    std::string aad = chunk_aad(header_);
    aead_seal(key_, nonce, (const uint8_t*)aad.data(), aad.size(), buffer_.data(), buffer_.size(), buffer_.data(), tag);
    bool ok = (buffer_.empty() || std::fwrite(buffer_.data(), 1, buffer_.size(), file_) == buffer_.size()) &&
              std::fwrite(tag, 1, 16, file_) == 16;
    buffer_.clear();
    if (!ok) failed_ = true;
    return ok;
}

bool Writer::write(const void* data, size_t length) {
    if (!file_ || failed_) return false;
    if (!sealed_) {
        if (length && std::fwrite(data, 1, length, file_) != length) failed_ = true;
        return !failed_;
    }
    auto* in = static_cast<const uint8_t*>(data);
    while (length) {
        // A full chunk is only sealed once more data follows: the final chunk carries the "last" flag.
        if (buffer_.size() == kChunk && !emit_chunk(false)) return false;
        size_t n = std::min(length, kChunk - buffer_.size());
        buffer_.insert(buffer_.end(), in, in + n);
        in += n; length -= n;
    }
    return !failed_;
}

bool Writer::close() {
    if (!file_) return false;
    bool ok = !failed_;
    if (sealed_ && ok) ok = emit_chunk(true);
    if (std::fflush(file_) != 0) ok = false;
    if (std::fclose(file_) != 0) ok = false;
    file_ = nullptr;
    if (!buffer_.empty()) wipe(buffer_.data(), buffer_.size());
    buffer_.clear();
    wipe(key_, sizeof(key_));
    return ok;
}

void Writer::abort() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    if (!buffer_.empty()) wipe(buffer_.data(), buffer_.size());
    buffer_.clear();
    wipe(key_, sizeof(key_));
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string temp = path + ".tmp";
    Writer writer;
    if (!writer.open(temp)) return false;
    if (!writer.write(data.data(), data.size()) || !writer.close()) { std::remove(temp.c_str()); return false; }
    if (std::rename(temp.c_str(), path.c_str()) != 0) { std::remove(temp.c_str()); return false; }
    return true;
}

namespace {
Convert convert(const std::string& path, bool to_sealed, bool (*proceed)(void*), void* context) {
    Reader reader;
    if (!reader.open(path)) return Convert::Failed;
    if (reader.sealed() == to_sealed) return Convert::Unchanged;
    std::string temp = path + ".vgs";
    Writer writer;
    if (!writer.open(temp, to_sealed)) return Convert::Failed;
    std::vector<uint8_t> block(65536);
    for (;;) {
        if (proceed && !proceed(context)) { writer.abort(); std::remove(temp.c_str()); return Convert::Interrupted; }
        int64_t n = reader.read(block.data(), block.size());
        if (n < 0) { writer.abort(); std::remove(temp.c_str()); return Convert::Failed; }
        if (n == 0) break;
        if (!writer.write(block.data(), (size_t)n)) { writer.abort(); std::remove(temp.c_str()); return Convert::Failed; }
    }
    wipe(block.data(), block.size());
    if (!writer.close()) { std::remove(temp.c_str()); return Convert::Failed; }
    reader.close();
    if (std::rename(temp.c_str(), path.c_str()) != 0) { std::remove(temp.c_str()); return Convert::Failed; }
    return Convert::Done;
}
}  // namespace

Convert seal_file(const std::string& path, bool (*proceed)(void*), void* context) {
    if (!sealing()) return Convert::Failed;
    return convert(path, true, proceed, context);
}

Convert unseal_file(const std::string& path, bool (*proceed)(void*), void* context) {
    if (!unlocked()) return Convert::Failed;
    return convert(path, false, proceed, context);
}
}
