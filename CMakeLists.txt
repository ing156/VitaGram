cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK environment variable first")
  endif()
endif()

project(vitagram LANGUAGES C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VITA_APP_NAME "VitaGram")
set(VITA_TITLEID  "VITAGRM01")
set(VITA_VERSION  "35.01")
set(VITAGRAM_VERSION "0.35.1")
option(VITAGRAM_PERFORMANCE "Collect bounded performance diagnostics" ON)
option(VITAGRAM_TGS_DIAGNOSTIC "Save one rendered TGS frame for diagnosis" OFF)

set(ANIMATION_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg-hardware/install" CACHE PATH "Vita FFmpeg prefix with h264_vita")
foreach(ANIMATION_LIB avformat avcodec swscale swresample avutil)
  if(NOT EXISTS "${ANIMATION_FFMPEG_ROOT}/lib/lib${ANIMATION_LIB}.a")
    message(FATAL_ERROR "Missing hardware decoder. Run bash scripts/build-hardware-video-decoder.sh first.")
  endif()
endforeach()
if(NOT EXISTS "${ANIMATION_FFMPEG_ROOT}/share/COPYING.LGPLv2.1")
  message(FATAL_ERROR "Missing FFmpeg license; run scripts/build-hardware-video-decoder.sh")
endif()

find_path(WEBP_INCLUDE_DIR webp/decode.h)
find_library(WEBP_LIBRARY NAMES webp)
if(NOT WEBP_INCLUDE_DIR OR NOT WEBP_LIBRARY)
  message(FATAL_ERROR "VitaSDK libwebp is required for stickers. Install the VitaSDK libwebp package first.")
endif()
foreach(EMOJI_ASSET NotoColorEmoji.ttf Emoji-LICENSE.txt NotoSans-Regular.ttf NotoSans-LICENSE.txt NotoSansCoptic-Regular.ttf NotoSansCoptic-LICENSE.txt NotoSansJP-Regular.otf NotoSansJP-LICENSE.txt NotoSansMath-Regular.ttf NotoSansMath-LICENSE.txt NotoSansSymbols2-Regular.ttf NotoSansSymbols2-LICENSE.txt)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/${EMOJI_ASSET}")
    message(FATAL_ERROR "Missing assets/${EMOJI_ASSET}. Run bash scripts/fetch-emoji-font.sh and bash scripts/fetch-fallback-fonts.sh first.")
  endif()
endforeach()
# Smaller font copies from scripts/shrink-fonts.sh: 64 px emoji strike (the UI
# draws emoji at about 31 px at most) and JIS-only kanji for the Japanese font.
set(EMOJI_FONT "assets/NotoColorEmoji-64.ttf")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${EMOJI_FONT}")
  message(WARNING "Packaging the full-size emoji font (+~5 MB). Run bash scripts/shrink-fonts.sh and reconfigure.")
  set(EMOJI_FONT "assets/NotoColorEmoji.ttf")
endif()
set(JAPANESE_FONT "assets/NotoSansJP-JIS.otf")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${JAPANESE_FONT}")
  message(WARNING "Packaging the full Japanese font (+~1.6 MB). Run bash scripts/shrink-fonts.sh and reconfigure.")
  set(JAPANESE_FONT "assets/NotoSansJP-Regular.otf")
endif()

set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared jansson" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "Build jansson examples" FORCE)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "Build jansson documentation" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "Skip jansson tests" FORCE)
set(JANSSON_INSTALL OFF CACHE BOOL "Install jansson" FORCE)
set(USE_URANDOM OFF CACHE BOOL "Use /dev/urandom in jansson" FORCE)
add_subdirectory(external/jansson EXCLUDE_FROM_ALL)

set(TDLIB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/tdlib-vita" CACHE PATH "Path to tdlib-vita")
if(NOT EXISTS "${TDLIB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "tdlib-vita not found. Clone https://github.com/v-atamanenko/tdlib-vita into external/tdlib-vita")
endif()

# PSVita TDLib fork. It exports Td::TdJsonStatic in the upstream-style CMake build.
set(__vita__ TRUE CACHE BOOL "Build TDLib for PS Vita" FORCE)
set(TD_ENABLE_TESTS OFF CACHE BOOL "Build TDLib tests" FORCE)
include_directories(BEFORE "${CMAKE_SOURCE_DIR}/src/vita_compat")
add_subdirectory("${TDLIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/tdlib" EXCLUDE_FROM_ALL)

add_library(vitagram_h264 STATIC src/minih264.c)
target_include_directories(vitagram_h264 PUBLIC external/minih264)
target_compile_definitions(vitagram_h264 PUBLIC H264E_MAX_THREADS=0 H264E_SVC_API=0)
target_compile_options(vitagram_h264 PRIVATE -O2 -mfpu=neon -mfloat-abi=hard)

add_library(vitagram_quirc STATIC
  external/quirc/lib/quirc.c
  external/quirc/lib/identify.c
  external/quirc/lib/decode.c
  external/quirc/lib/version_db.c
)
target_include_directories(vitagram_quirc PUBLIC external/quirc/lib)
target_compile_definitions(vitagram_quirc PRIVATE QUIRC_FLOAT_TYPE=float QUIRC_USE_TGMATH)

# Every assets/lang/<code>.lang is packaged as app0:lang/<code>.lang; a new language needs no code changes.
file(GLOB LANGUAGE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/lang/*.lang")
set(LANGUAGE_VPK_FILES)
foreach(LANGUAGE_PATH ${LANGUAGE_FILES})
  get_filename_component(LANGUAGE_NAME "${LANGUAGE_PATH}" NAME)
  list(APPEND LANGUAGE_VPK_FILES FILE "${LANGUAGE_PATH}" "lang/${LANGUAGE_NAME}")
endforeach()

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/i18n.cpp
  src/download_folder_picker.cpp
  src/utf8_decode.cpp
  src/cpu_scheduler.cpp
  src/td_diagnostics.cpp
  src/media_render.cpp
  src/image_decoder.cpp
  src/media_cache.cpp
  src/media_trash.cpp
  src/ui_cache.cpp
  src/animation_player.cpp
  src/portrait_video.cpp
  src/voice_player.cpp
  src/audio_recorder.cpp
  src/telegram_client.cpp
  src/keyboard.cpp
  src/proxy_link.cpp
  src/qr_scanner.cpp
  src/video_note_encoder.cpp
  src/video_note_recorder.cpp
  src/vita_net.cpp
  ${CMAKE_SOURCE_DIR}/external/tdlib-vita/vitaports/td_missing_func_placeholder.cpp
)
set_source_files_properties(src/download_folder_picker.cpp PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(src/portrait_video.cpp src/video_note_encoder.cpp src/qr_scanner.cpp
  PROPERTIES COMPILE_OPTIONS "-Os")

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  ${ANIMATION_FFMPEG_ROOT}/include
  ${WEBP_INCLUDE_DIR}
  ${VITASDK}/arm-vita-eabi/include/freetype2
  external/jansson/src
  ${CMAKE_BINARY_DIR}/external/jansson/include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  VITAGRAM_VERSION="${VITAGRAM_VERSION}"
  VITAGRAM_PERFORMANCE=$<BOOL:${VITAGRAM_PERFORMANCE}>
  VITAGRAM_TGS_DIAGNOSTIC=$<BOOL:${VITAGRAM_TGS_DIAGNOSTIC}>
  VITAGRAM_HARDWARE_VIDEO=1
)

set(RLOTTIE_ROOT "${CMAKE_SOURCE_DIR}/external/rlottie-vita/install" CACHE PATH "Vita rlottie prefix")
if(EXISTS "${RLOTTIE_ROOT}/lib/librlottie.a")
  if(NOT EXISTS "${RLOTTIE_ROOT}/share/rlottie-COPYING")
    message(FATAL_ERROR "Missing rlottie licenses: rerun scripts/build-sticker-renderer.sh")
  endif()
  set(RLOTTIE_VPK_FILES FILE "${RLOTTIE_ROOT}/share/rlottie-COPYING" "assets/rlottie-COPYING.txt")
  file(GLOB RLOTTIE_LICENSES "${RLOTTIE_ROOT}/share/rlottie-licenses/*")
  foreach(LICENSE_PATH ${RLOTTIE_LICENSES})
    get_filename_component(LICENSE_NAME "${LICENSE_PATH}" NAME)
    list(APPEND RLOTTIE_VPK_FILES FILE "${LICENSE_PATH}" "assets/rlottie-${LICENSE_NAME}.txt")
  endforeach()
  target_include_directories(${PROJECT_NAME} PRIVATE "${RLOTTIE_ROOT}/include")
  target_compile_definitions(${PROJECT_NAME} PRIVATE VITAGRAM_RLOTTIE=1)
  target_link_libraries(${PROJECT_NAME} "${RLOTTIE_ROOT}/lib/librlottie.a")
else()
  message(WARNING "TGS playback unavailable: run scripts/build-sticker-renderer.sh and reconfigure")
endif()

target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=utf8_to_ucs2")

target_link_libraries(${PROJECT_NAME}
  Td::TdJsonStatic
  vitagram_quirc
  vitagram_h264
  jansson
  ${ANIMATION_FFMPEG_ROOT}/lib/libavformat.a
  ${ANIMATION_FFMPEG_ROOT}/lib/libavcodec.a
  ${ANIMATION_FFMPEG_ROOT}/lib/libswscale.a
  ${ANIMATION_FFMPEG_ROOT}/lib/libswresample.a
  ${ANIMATION_FFMPEG_ROOT}/lib/libavutil.a
  # Thread runtime is already supplied through TDLib's thread/toolchain linkage.
  # Do not add a second explicit -lpthread here (Vita duplicates vita_osal.o).
  vita2d
  ${WEBP_LIBRARY}
  freetype
  bz2
  png
  jpeg
  z
  m
  c
  SceDisplay_stub
  ScePower_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCamera_stub
  SceRtc_stub
  SceGxm_stub
  SceSysmodule_stub
  SceVideodec_stub
  SceCodecEngine_stub
  SceCtrl_stub
  SceMotion_stub
  SceTouch_stub
  SceRegistryMgr_stub
  ScePgf_stub
  ScePvf_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceNet_stub
  SceNetCtl_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE external/minih264/LICENSE assets/minih264-LICENSE.txt
  FILE external/quirc/LICENSE assets/quirc-LICENSE.txt
  FILE assets/icon0.png sce_sys/icon0.png
  FILE assets/controller-icons/triangle.png assets/controller-icons/triangle.png
  FILE assets/controller-icons/square.png assets/controller-icons/square.png
  FILE assets/controller-icons/cross.png assets/controller-icons/cross.png
  FILE assets/controller-icons/circle.png assets/controller-icons/circle.png
  FILE assets/controller-icons/dpad.png assets/controller-icons/dpad.png
  FILE assets/controller-icons/start.png assets/controller-icons/start.png
  FILE assets/controller-icons/select.png assets/controller-icons/select.png
  FILE ${EMOJI_FONT} assets/NotoColorEmoji.ttf
  FILE assets/Emoji-LICENSE.txt assets/Emoji-LICENSE.txt
  FILE assets/NotoSans-Regular.ttf assets/NotoSans-Regular.ttf
  FILE assets/NotoSans-LICENSE.txt assets/NotoSans-LICENSE.txt
  FILE assets/NotoSansCoptic-Regular.ttf assets/NotoSansCoptic-Regular.ttf
  FILE assets/NotoSansCoptic-LICENSE.txt assets/NotoSansCoptic-LICENSE.txt
  FILE ${JAPANESE_FONT} assets/NotoSansJP-Regular.otf
  FILE assets/NotoSansJP-LICENSE.txt assets/NotoSansJP-LICENSE.txt
  FILE assets/NotoSansMath-Regular.ttf assets/NotoSansMath-Regular.ttf
  FILE assets/NotoSansMath-LICENSE.txt assets/NotoSansMath-LICENSE.txt
  FILE assets/NotoSansSymbols2-Regular.ttf assets/NotoSansSymbols2-Regular.ttf
  FILE assets/NotoSansSymbols2-LICENSE.txt assets/NotoSansSymbols2-LICENSE.txt
  FILE ${ANIMATION_FFMPEG_ROOT}/share/COPYING.LGPLv2.1 assets/FFmpeg-LICENSE.txt
  ${LANGUAGE_VPK_FILES}
  ${RLOTTIE_VPK_FILES}
)
