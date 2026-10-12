#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <pthread.h>

struct DecodedImage {
    unsigned width = 0, height = 0;
    std::vector<uint8_t> rgba;
    const char* error = nullptr; // Static diagnostic code, never a filename.
};
inline DecodedImage image_failure(const char* error) { DecodedImage out; out.error = error; return out; }
DecodedImage decode_image(const std::string& path, const std::function<bool()>& proceed);

// CPU pixels only. All GPU allocation, upload and destruction remain on UI.
class ImageDecoder {
public:
    using Decode = std::function<DecodedImage(const std::string&, const std::function<bool()>&)>;
    explicit ImageDecoder(Decode decode = decode_image);
    ~ImageDecoder();
    void request(const std::vector<std::string>& visible, bool paused);
    bool take(std::string& path, DecodedImage& image);
    void clear();
    void stop();
private:
    void run();
    Decode decode_;
    std::mutex mutex_;
    std::condition_variable wake_;
    pthread_t worker_{};
    bool started_ = false;
    std::vector<std::string> visible_;
    std::string active_, ready_path_;
    DecodedImage ready_;
    uint64_t generation_ = 0;
    bool stopped_ = false, paused_ = true;
};
