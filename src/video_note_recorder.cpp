#include "performance.h"
#include "i18n.h"
#include "video_note_recorder.h"
#include "video_note_frame.h"
#include "video_note_capture_buffer.h"
#include "video_note_encoder.h"
#include "cpu_scheduler.h"
#include <vita2d.h>
#include <psp2/camera.h>
#include <psp2/audioin.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace video_note;
namespace {
constexpr int Grain = 512;
static_assert(Grain==CaptureBuffer::AudioGrain,"Audio queue block size");
std::string camera_error(const std::string& text, int rc) {
    char code[24]; std::snprintf(code,sizeof(code)," (0x%08X)",(unsigned)rc); return text+code;
}
}
VideoNoteRecorder::VideoNoteRecorder() = default;
VideoNoteRecorder::~VideoNoteRecorder() { cancel(); pthread_mutex_destroy(&mutex_); }
void VideoNoteRecorder::setStatus(const std::string& value) {
    pthread_mutex_lock(&mutex_); status_=value; pthread_mutex_unlock(&mutex_);
}
std::string VideoNoteRecorder::status() const {
    pthread_mutex_lock(&mutex_); std::string value=status_; pthread_mutex_unlock(&mutex_); return value;
}
void VideoNoteRecorder::fail(const std::string& value) { setStatus(value); state_=State::Error; }
int VideoNoteRecorder::seconds() const { return (samples_.load()+SampleRate-1)/SampleRate; }

bool VideoNoteRecorder::open(const std::string& path) {
    if (active() || path.empty()) return false;
    cancel();
    path_=path; raw_path_=path+".frames"; pcm_path_=path+".pcm";
    samples_=0; device_=SCE_CAMERA_DEVICE_FRONT; progress_=0; level_=0;
    cancel_=begin_=finish_=switch_=false;
    audio_stop_=audio_done_=audio_error_=false;
    writer_error_=false;
    preview_pixels_.assign(Side*Side,0); preview_ready_=false;
    vita2d_wait_rendering_done();
    texture_=vita2d_create_empty_texture(Side,Side);
    if (!texture_) { fail(tr("vnr.err.camera_memory")); return false; }
    std::memset(vita2d_texture_get_datap(texture_),0,vita2d_texture_get_stride(texture_)*Side);
    setStatus(tr("vnr.opening_front")); state_=State::Opening;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,2*1024*1024);
    int rc=pthread_create(&thread_,&attr,entry,this); pthread_attr_destroy(&attr);
    if (rc) { fail(tr("vnr.start_failed")); return false; }
    thread_started_=true; return true;
}
void VideoNoteRecorder::record() { if (state()==State::Preview) begin_=true; }
void VideoNoteRecorder::finish() { if (state()==State::Recording) finish_=true; }
void VideoNoteRecorder::switchCamera() {
    if (state()==State::Preview || state()==State::Recording) switch_=true;
}
void VideoNoteRecorder::cancel() {
    performance::Scope measurement(performance::Metric::VideoCancel);
    cancel_=true; audio_stop_=true;
    if (thread_started_) { pthread_join(thread_,nullptr); thread_started_=false; }
    closeCamera();
    if (texture_) { vita2d_wait_rendering_done(); vita2d_free_texture(texture_); texture_=nullptr; }
    if (!raw_path_.empty()) std::remove(raw_path_.c_str());
    if (!pcm_path_.empty()) std::remove(pcm_path_.c_str());
    if (!path_.empty()) std::remove(path_.c_str());
    path_.clear(); raw_path_.clear(); pcm_path_.clear(); preview_pixels_.clear(); preview_ready_=false;
    state_=State::Closed;
}
void VideoNoteRecorder::releaseFile() {
    if (state()!=State::Review) return;
    // The encoder is finished before publishing Review. Join before clearing its paths.
    if (thread_started_) { pthread_join(thread_,nullptr); thread_started_=false; }
    path_.clear(); cancel();
}

bool VideoNoteRecorder::openCamera(int device) {
    memory_=sceKernelAllocMemBlock("VitaGramVideoNote",SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,1024*1024,nullptr);
    if (memory_<0) { setStatus(camera_error(tr("cam.memory"),memory_)); return false; }
    void* data=nullptr; int rc=sceKernelGetMemBlockBase(memory_,&data);
    if (rc<0) { setStatus(camera_error(tr("cam.buffer"),rc)); closeCamera(); return false; }
    camera_=static_cast<uint8_t*>(data);
    device_=device;
    SceCameraInfo info{}; info.size=sizeof(info);
    info.priority=SCE_CAMERA_PRIORITY_SHARE; info.format=SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.resolution=SCE_CAMERA_RESOLUTION_320_240; info.framerate=SCE_CAMERA_FRAMERATE_30_FPS; info.range=1;
    info.sizeIBase=320*240; info.sizeUBase=info.sizeVBase=320*240/4;
    info.pIBase=camera_; info.pUBase=camera_+info.sizeIBase; info.pVBase=camera_+info.sizeIBase+info.sizeUBase;
    rc=sceCameraOpen(device,&info);
    if (rc<0) { setStatus(camera_error(tr("cam.open_failed"),rc)); closeCamera(); return false; }
    camera_open_=true; rc=sceCameraStart(device);
    if (rc<0) { setStatus(camera_error(tr("cam.start_failed"),rc)); closeCamera(); return false; }
    camera_started_=true; return true;
}
void VideoNoteRecorder::closeCamera() {
    if (camera_started_) { sceCameraStop(device_); camera_started_=false; }
    if (camera_open_) { sceCameraClose(device_); camera_open_=false; }
    if (memory_>=0) { sceKernelFreeMemBlock(memory_); memory_=-1; }
    camera_=nullptr;
}
void* VideoNoteRecorder::entry(void* self) { static_cast<VideoNoteRecorder*>(self)->run(); return nullptr; }
void* VideoNoteRecorder::audioEntry(void* self) { static_cast<VideoNoteRecorder*>(self)->captureAudio(); return nullptr; }
void* VideoNoteRecorder::writerEntry(void* self) { static_cast<VideoNoteRecorder*>(self)->writeCapture(); return nullptr; }

void VideoNoteRecorder::writeCapture() {
    cpu_scheduler::Thread role(cpu_scheduler::Role::Storage);
    FILE* raw=std::fopen(raw_path_.c_str(),"wb");
    FILE* pcm=std::fopen(pcm_path_.c_str(),"wb");
    auto fail_write=[&](const char* key) {
        setStatus(tr(key)); writer_error_=true; capture_->abort();
    };
    if (!raw || !pcm) fail_write(raw ? "vnr.audio_file_failed" : "vnr.record_file_space");
    // Supply owned memory and check success: the old NULL-buffer request could
    // fall back to a smaller allocation. These buffers remain alive until close.
    else if (std::setvbuf(raw,capture_->video_stdio.data(),_IOFBF,capture_->video_stdio.size()) ||
             std::setvbuf(pcm,capture_->audio_stdio.data(),_IOFBF,capture_->audio_stdio.size()))
        fail_write("vnr.record_file_space");
    CaptureBuffer::Item item{};
    while (!writer_error_ && !cancel_ && capture_->next(item)) {
        performance::Scope measurement(performance::Metric::VideoCaptureWrite);
        FILE* file=item.audio ? pcm : raw;
        bool ok=item.audio || std::fwrite(&item.pts,sizeof(item.pts),1,file)==1;
        if (ok) ok=std::fwrite(item.data,1,item.bytes,file)==item.bytes;
        capture_->complete(item);
        if (!ok) fail_write(item.audio ? "vnr.audio_save_space" : "vnr.frame_save_space");
    }
    // The final stdio flush must finish before the encoder opens these files.
    if (raw) {
        performance::Scope measurement(performance::Metric::VideoCaptureWrite);
        if (std::fclose(raw)) fail_write("vnr.video_save_error");
    }
    if (pcm) {
        performance::Scope measurement(performance::Metric::VideoCaptureWrite);
        if (std::fclose(pcm)) fail_write("vnr.audio_save_error");
    }
}

void VideoNoteRecorder::captureAudio() {
    cpu_scheduler::Thread role(cpu_scheduler::Role::Audio);
    cpu_scheduler::Media workload(cpu_scheduler::Role::Audio);
    int port=sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE,Grain,SampleRate,SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    if (port<0) {
        setStatus(tr("rec.mic_unavailable"));
        audio_error_=true; audio_done_=true; return;
    }
    int16_t pcm[Grain];
    while (!cancel_ && !audio_stop_ && !writer_error_ && samples_<SampleRate*MaxSeconds) {
        int rc=sceAudioInInput(port,pcm);
        if (rc<0) { setStatus(tr("rec.mic_read_error")); audio_error_=true; break; }
        int count=rc>0 && rc<=Grain ? rc : Grain;
        count=std::min<uint32_t>(count,SampleRate*MaxSeconds-samples_.load());
        int peak=0; for (int i=0;i<count;++i) peak=std::max(peak,std::abs((int)pcm[i]));
        level_=std::min(100,peak*100/32768);
        auto queued=capture_->audio(pcm,count);
        if (queued!=CaptureBuffer::Result::Queued) {
            if (queued==CaptureBuffer::Result::Full) setStatus(tr("vnr.audio_save_space"));
            audio_error_=true; break;
        }
        samples_.fetch_add(count);
    }
    sceAudioInReleasePort(port); audio_done_=true;
}

void VideoNoteRecorder::run() {
    cpu_scheduler::Thread role(cpu_scheduler::Role::Video);
    cpu_scheduler::Media workload(cpu_scheduler::Role::Video);
    if (!openCamera(SCE_CAMERA_DEVICE_FRONT)) { state_=State::Error; return; }
    state_=State::Preview; setStatus(tr("vnr.hint_preview"));
    std::vector<uint8_t> frame(FrameBytes);
    std::vector<uint8_t> camera_copy(320*240*3/2);
    std::vector<uint32_t> preview_frame(Side*Side);
    uint64_t last_frame=UINT64_MAX;
    uint32_t last_pts=0;
    unsigned frames=0;
    bool error=false;
    while (!cancel_) {
        if (switch_.exchange(false)) {
            int previous=device_; closeCamera();
            if (!openCamera(1-previous)) {
                if (!openCamera(previous)) { error=true; break; }
                setStatus(tr("vnr.other_camera_unavailable"));
            } else setStatus(state()==State::Recording ? tr("rec.recording") : tr("vnr.hint_preview"));
            last_frame=UINT64_MAX;
        }
        if (begin_.exchange(false) && state()==State::Preview) {
            try { capture_.reset(new CaptureBuffer()); }
            catch (const std::bad_alloc&) { setStatus(tr("vnr.err.camera_memory")); error=true; break; }
            setStatus(tr("rec.recording"));
            pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,128*1024);
            int rc=pthread_create(&writer_thread_,&attr,writerEntry,this);
            if (!rc) { writer_started_=true; rc=pthread_create(&audio_thread_,&attr,audioEntry,this); }
            pthread_attr_destroy(&attr);
            if (rc) { setStatus(tr("vnr.mic_start_failed")); error=true; break; }
            audio_started_=true; state_=State::Recording; last_frame=UINT64_MAX;
        }
        if (state()==State::Recording && (finish_ || audio_done_ || writer_error_)) break;
        SceCameraRead read{}; read.size=sizeof(read); read.mode=1;
        performance::Scope read_measurement(performance::Metric::VideoCaptureRead);
        int rc=sceCameraRead(device_,&read);
        read_measurement.finish();
        if (rc>=0 && read.frame!=last_frame) {
            // Avoid thousands of individual reads from uncached DMA memory.
            performance::Scope crop_measurement(performance::Metric::VideoCaptureCrop);
            last_frame=read.frame; std::memcpy(camera_copy.data(),camera_,camera_copy.size());
            crop(camera_copy.data(),frame.data()); crop_measurement.finish();
            performance::Scope preview_measurement(performance::Metric::VideoCapturePreview);
            rgba(frame.data(),preview_frame.data());
            pthread_mutex_lock(&mutex_);
            preview_pixels_.swap(preview_frame); preview_ready_=true;
            pthread_mutex_unlock(&mutex_);
            preview_measurement.finish();
            uint32_t clock=samples_.load();
            // Anchor the first image at audio time zero. Subsequent camera samples
            // use the microphone sample clock, so switching cameras cannot drift A/V.
            uint32_t pts=clock>Grain ? clock-Grain : 0;
            // Keep every new camera image. The microphone clock advances in 32 ms
            // blocks; a deadline measured from the last image discards good frames.
            // Polling jitter can put two images in one audio block: keep their PTS
            // strictly increasing, without extending past the recorded audio.
            if (frames) pts=std::max(pts,last_pts+1);
            if (state()==State::Recording && (!frames || pts<clock) && clock<SampleRate*MaxSeconds) {
                if (!frames) pts=0;
                auto queued=capture_->video(pts,frame.data());
                if (queued==CaptureBuffer::Result::Stopped) { error=true; break; }
                if (queued==CaptureBuffer::Result::Queued) { ++frames; last_pts=pts; }
            }
        } else if (rc<0 && rc!=(int)SCE_CAMERA_ERROR_ALREADY_READ && rc!=(int)SCE_CAMERA_ERROR_NOT_ACTIVE && rc!=(int)SCE_CAMERA_ERROR_TIMEOUT) {
            setStatus(camera_error(tr("cam.error"),rc)); error=true; break;
        }
        sceKernelDelayThread(2000);
    }
    audio_stop_=true;
    if (audio_started_) { pthread_join(audio_thread_,nullptr); audio_started_=false; }
    closeCamera();
    if (capture_) {
        if (cancel_ || error || audio_error_ || writer_error_) capture_->abort();
        else { state_=State::Encoding; setStatus(tr("vnr.encoding")); capture_->finish(); }
        if (writer_started_) { pthread_join(writer_thread_,nullptr); writer_started_=false; }
        auto stats=capture_->stats();
        performance::observe(performance::Metric::VideoCaptureQueuePeak,stats.video_peak);
        performance::observe(performance::Metric::AudioCaptureQueuePeak,stats.audio_peak);
        performance::observe(performance::Metric::VideoCaptureDropped,stats.dropped);
        capture_.reset();
    }
    if (cancel_) return;
    if (error || audio_error_ || writer_error_) { state_=State::Error; return; }
    if (!frames || samples_<SampleRate) { fail(tr("vnr.too_short")); return; }
    state_=State::Encoding; setStatus(tr("vnr.encoding"));
    std::string message;
    performance::Scope encode_measurement(performance::Metric::VideoEncode);
    bool ok=encode_video_note(raw_path_,pcm_path_,path_,samples_,cancel_,progress_,message);
    encode_measurement.finish();
    std::remove(raw_path_.c_str()); std::remove(pcm_path_.c_str());
    if (cancel_) return;
    if (!ok) { fail(tr("vnr.video_error",{message})); return; }
    setStatus(tr("vnr.ready")); state_=State::Review;
}
void VideoNoteRecorder::upload() {
    if (!texture_) return;
    // UI runs at 60 Hz while the camera supplies 30 Hz. Do not wait on the GPU
    // at all on iterations that have no fresh preview image.
    if (pthread_mutex_trylock(&mutex_)) return;
    bool ready=preview_ready_; pthread_mutex_unlock(&mutex_);
    if (!ready) return;
    performance::Scope wait_measurement(performance::Metric::VideoCaptureGpuWait);
    vita2d_wait_rendering_done();
    wait_measurement.finish();
    pthread_mutex_lock(&mutex_);
    if (preview_ready_) {
        auto* data=static_cast<uint8_t*>(vita2d_texture_get_datap(texture_));
        unsigned stride=vita2d_texture_get_stride(texture_);
        for (int y=0;y<Side;++y) std::memcpy(data+y*stride,preview_pixels_.data()+y*Side,Side*4);
        preview_ready_=false;
    }
    pthread_mutex_unlock(&mutex_);
}
