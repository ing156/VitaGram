#include "image_decoder.h"
#include "inline_thumbnail.h"
#include "performance.h"
#include "vault.h"
#if defined(__vita__) || defined(VITAGRAM_VIDEO_THUMBNAILS)
#include "video_thumbnail.h"
#endif
#ifdef __vita__
#include "cpu_scheduler.h"
#endif
#include <png.h>
#include <cstdio>
#include <jpeglib.h>
#include <webp/decode.h>
#include <setjmp.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <new>

namespace {
constexpr size_t limit = 16 * 1024 * 1024;
// Header limits checked before libjpeg allocates anything, and a hard cap on libjpeg's own working
// memory: a progressive 64-megapixel JPEG would otherwise ask for hundreds of MB and take the app down.
constexpr unsigned jpeg_max_side = 12000;
constexpr uint64_t jpeg_max_pixels = 36ull * 1000 * 1000;
constexpr long jpeg_memory_limit = 32L * 1024 * 1024;
bool valid(unsigned w, unsigned h) { return w && h && w <= 4096 && h <= 4096 && size_t(w)*h <= limit/4; }
struct JpegError { jpeg_error_mgr base; jmp_buf jump; };
void jpeg_failed(j_common_ptr p) { longjmp(reinterpret_cast<JpegError*>(p->err)->jump, 1); }
// No C++ objects are constructed inside the libjpeg longjmp boundary.
bool jpeg_pixels(const uint8_t* data, size_t size, DecodedImage& out, const std::function<bool()>& proceed) {
    auto* decoder = static_cast<jpeg_decompress_struct*>(std::calloc(1, sizeof(jpeg_decompress_struct)));
    if (!decoder) return false;
    JpegError error{};
    decoder->err = jpeg_std_error(&error.base); error.base.error_exit = jpeg_failed;
    if (setjmp(error.jump)) { jpeg_destroy_decompress(decoder); std::free(decoder); return false; }
    jpeg_create_decompress(decoder); jpeg_mem_src(decoder, data, size);
    decoder->mem->max_memory_to_use = jpeg_memory_limit; // exceeding it raises a normal libjpeg error (longjmp above)
    jpeg_read_header(decoder, TRUE);
    if (!decoder->image_width || !decoder->image_height || decoder->image_width>jpeg_max_side || decoder->image_height>jpeg_max_side || uint64_t(decoder->image_width)*decoder->image_height>jpeg_max_pixels) { jpeg_destroy_decompress(decoder); std::free(decoder); return false; }
    // Vita's screen is 960 pixels wide. Decode large JPEGs at a native reduced scale.
    while (decoder->scale_denom < 8 && (decoder->image_width/decoder->scale_denom > 1024 || decoder->image_height/decoder->scale_denom > 1024)) decoder->scale_denom *= 2;
    decoder->out_color_space = JCS_RGB;
    jpeg_start_decompress(decoder);
    if (!valid(decoder->output_width,decoder->output_height)) { jpeg_destroy_decompress(decoder); std::free(decoder); return false; }
    out.width = decoder->output_width; out.height = decoder->output_height;
    try { out.rgba.resize(size_t(out.width)*out.height*4); }
    catch (...) { jpeg_destroy_decompress(decoder); std::free(decoder); throw; }
    JSAMPARRAY row = (*decoder->mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(decoder), JPOOL_IMAGE, out.width*3, 1);
    while (decoder->output_scanline < decoder->output_height) {
        if (!proceed()) { jpeg_destroy_decompress(decoder); std::free(decoder); return false; }
        unsigned y = decoder->output_scanline;
        jpeg_read_scanlines(decoder, row, 1);
        auto* dst = out.rgba.data()+size_t(y)*out.width*4;
        for (unsigned x=0; x<out.width; ++x) { std::memcpy(dst+x*4, row[0]+x*3, 3); dst[x*4+3]=255; }
    }
    jpeg_finish_decompress(decoder); jpeg_destroy_decompress(decoder); std::free(decoder); return true;
}
}
DecodedImage decode_image(const std::string& path, const std::function<bool()>& proceed) {
    DecodedImage out;
    if (!proceed()) return out;
    std::vector<uint8_t> bytes;
    const bool video = path.compare(0, 12, "video-thumb:") == 0;
    if (path.compare(0, 12, inline_thumbnail::prefix) == 0) {
        if (!inline_thumbnail::decode(path, bytes)) return image_failure("invalid_mini");
    } else {
        vault::Reader f; // plain files and sealed ones alike
        if (!f.open(video ? path.substr(12) : path)) return image_failure("file_open");
        const int64_t size=f.size();
        if (size<=0 || size>int64_t(limit)) return image_failure("file_size");
        bytes.resize(size_t(size));
        for (size_t n=0; n<bytes.size();) {
            size_t count=std::min(size_t(64*1024),bytes.size()-n);
            if (!proceed()) return out;
            if (f.read(bytes.data()+n,count)!=int64_t(count)) return image_failure("file_read");
            n+=count;
        }
        f.close();
    }
    if (video) {
#if defined(__vita__) || defined(VITAGRAM_VIDEO_THUMBNAILS)
        return video_thumbnail::decode(bytes, proceed);
#else
        return {};
#endif
    }
    bool ok=false;
    if (bytes.size()>=8 && !png_sig_cmp(bytes.data(),0,8)) {
        png_image png{}; png.version=PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&png,bytes.data(),bytes.size())) {
            if (valid(png.width,png.height) && proceed()) {
                png.format=PNG_FORMAT_RGBA; out.width=png.width; out.height=png.height;
                try { out.rgba.resize(PNG_IMAGE_SIZE(png)); }
                catch (...) { png_image_free(&png); throw; }
                ok=png_image_finish_read(&png,nullptr,out.rgba.data(),0,nullptr)!=0;
            }
        }
        png_image_free(&png);
    } else if (bytes.size()>=2 && bytes[0]==0xff && bytes[1]==0xd8) {
        ok=jpeg_pixels(bytes.data(),bytes.size(),out,proceed);
    } else {
        int w=0,h=0;
        if (WebPGetInfo(bytes.data(),bytes.size(),&w,&h) && valid(w,h) && proceed()) {
            out.width=w; out.height=h; out.rgba.resize(size_t(w)*h*4);
            ok=WebPDecodeRGBAInto(bytes.data(),bytes.size(),out.rgba.data(),out.rgba.size(),w*4)!=nullptr;
        }
    }
    if (!proceed()) return {};
    if (!ok) return image_failure("image_decode");
    // Bound both the completed CPU result and the next UI upload to 4 MiB.
    if (out.width > 1024 || out.height > 1024) {
        const unsigned longest = std::max(out.width,out.height);
        DecodedImage scaled;
        scaled.width = std::max(1u,out.width*1024/longest);
        scaled.height = std::max(1u,out.height*1024/longest);
        scaled.rgba.resize(size_t(scaled.width)*scaled.height*4);
        for (unsigned y=0;y<scaled.height;++y) {
            if (!proceed()) return {};
            const uint64_t sy = uint64_t(y)*(out.height-1)*256/std::max(1u,scaled.height-1);
            const unsigned y0=sy/256,y1=std::min(y0+1,out.height-1),fy=sy%256;
            for (unsigned x=0;x<scaled.width;++x) {
                const uint64_t sx = uint64_t(x)*(out.width-1)*256/std::max(1u,scaled.width-1);
                const unsigned x0=sx/256,x1=std::min(x0+1,out.width-1),fx=sx%256;
                for (unsigned c=0;c<4;++c) {
                    unsigned top=out.rgba[(size_t(y0)*out.width+x0)*4+c]*(256-fx)+out.rgba[(size_t(y0)*out.width+x1)*4+c]*fx;
                    unsigned bottom=out.rgba[(size_t(y1)*out.width+x0)*4+c]*(256-fx)+out.rgba[(size_t(y1)*out.width+x1)*4+c]*fx;
                    scaled.rgba[(size_t(y)*scaled.width+x)*4+c]=(top*(256-fy)+bottom*fy+32768)/65536;
                }
            }
        }
        return scaled;
    }
    return out;
}
ImageDecoder::ImageDecoder(Decode decode): decode_(std::move(decode)) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
    started_ = pthread_create(&worker_, &attributes, [](void* self)->void* {
        static_cast<ImageDecoder*>(self)->run(); return nullptr;
    }, this) == 0;
    pthread_attr_destroy(&attributes);
    if (!started_) stopped_=true;
}
ImageDecoder::~ImageDecoder() { stop(); }
void ImageDecoder::request(const std::vector<std::string>& visible, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    visible_.assign(visible.begin(),visible.begin()+std::min(size_t(64),visible.size())); paused_=paused;
    if (!ready_path_.empty() && std::find(visible_.begin(),visible_.end(),ready_path_)==visible_.end()) { ready_path_.clear(); ready_={}; }
    wake_.notify_one();
}
bool ImageDecoder::take(std::string& path, DecodedImage& image) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || paused_ || ready_path_.empty()) return false;
    path=std::move(ready_path_); ready_path_.clear(); image=std::move(ready_); ready_={};
    visible_.erase(std::remove(visible_.begin(),visible_.end(),path),visible_.end());
    wake_.notify_one(); return true;
}
void ImageDecoder::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_; visible_.clear(); ready_path_.clear(); ready_={}; wake_.notify_one();
}
void ImageDecoder::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_=true; ++generation_; visible_.clear(); ready_path_.clear(); ready_={}; wake_.notify_one(); }
    if (started_) { pthread_join(worker_, nullptr); started_=false; }
}
void ImageDecoder::run() {
#ifdef __vita__
    cpu_scheduler::Thread scheduling(cpu_scheduler::Role::Storage);
#endif
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
        wake_.wait(lock,[&]{return stopped_ || (!paused_ && ready_path_.empty() && !visible_.empty());});
        if (stopped_) break;
        active_=visible_.front(); const auto path=active_; const auto generation=generation_;
        lock.unlock();
        auto proceed=[&,this] {
            std::unique_lock<std::mutex> guard(mutex_);
            wake_.wait(guard,[&]{return stopped_ || generation!=generation_ || !paused_ || std::find(visible_.begin(),visible_.end(),path)==visible_.end();});
            return !stopped_ && generation==generation_ && std::find(visible_.begin(),visible_.end(),path)!=visible_.end();
        };
        DecodedImage image;
        { performance::Scope metric(performance::Metric::ImageDecode); try { image=decode_(path,proceed); } catch (const std::bad_alloc&) { image=image_failure("no_memory"); } }
        lock.lock(); active_.clear();
        if (!stopped_ && generation==generation_ && std::find(visible_.begin(),visible_.end(),path)!=visible_.end()) { ready_path_=path; ready_=std::move(image); }
    }
}
