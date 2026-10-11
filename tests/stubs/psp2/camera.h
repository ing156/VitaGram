#pragma once
#include <cstdint>
constexpr int SCE_CAMERA_DEVICE_BACK = 1;
constexpr int SCE_CAMERA_DEVICE_FRONT = 0;
constexpr int SCE_CAMERA_PRIORITY_SHARE = 0;
constexpr int SCE_CAMERA_FORMAT_YUV420_PLANE = 3;
constexpr int SCE_CAMERA_RESOLUTION_640_480 = 1;
constexpr int SCE_CAMERA_RESOLUTION_320_240 = 2;
constexpr int SCE_CAMERA_FRAMERATE_15_FPS = 15;
constexpr int SCE_CAMERA_FRAMERATE_30_FPS = 30;
constexpr int SCE_CAMERA_ERROR_ALREADY_READ = -100;
constexpr int SCE_CAMERA_ERROR_NOT_ACTIVE = -101;
constexpr int SCE_CAMERA_ERROR_TIMEOUT = -102;
struct SceCameraInfo {
    unsigned size, priority, format, resolution, framerate, range, sizeIBase, sizeUBase, sizeVBase;
    void *pIBase, *pUBase, *pVBase;
};
struct SceCameraRead { unsigned size; int mode; uint64_t frame; };
int sceCameraOpen(int, SceCameraInfo*);
int sceCameraStart(int);
int sceCameraRead(int, SceCameraRead*);
int sceCameraStop(int);
int sceCameraClose(int);
