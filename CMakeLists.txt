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
set(VITA_VERSION  "27.00")
set(VITAGRAM_VERSION "0.27.0")

set(ANIMATION_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg-animation/install" CACHE PATH "Minimal Vita FFmpeg prefix")
foreach(ANIMATION_LIB avformat avcodec swscale swresample avutil)
  if(NOT EXISTS "${ANIMATION_FFMPEG_ROOT}/lib/lib${ANIMATION_LIB}.a")
    message(FATAL_ERROR "Missing animation decoder. Run bash scripts/build-animation-decoder.sh in WSL first.")
  endif()
endforeach()
if(NOT EXISTS "${ANIMATION_FFMPEG_ROOT}/share/COPYING.LGPLv2.1")
  message(FATAL_ERROR "Missing FFmpeg license; run scripts/build-animation-decoder.sh")
endif()

find_path(WEBP_INCLUDE_DIR webp/decode.h)
find_library(WEBP_LIBRARY NAMES webp)
if(NOT WEBP_INCLUDE_DIR OR NOT WEBP_LIBRARY)
  message(FATAL_ERROR "VitaSDK libwebp is required for stickers. Install the VitaSDK libwebp package first.")
endif()
foreach(EMOJI_ASSET NotoColorEmoji.ttf Emoji-LICENSE.txt NotoSans-Regular.ttf NotoSans-LICENSE.txt)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/${EMOJI_ASSET}")
    message(FATAL_ERROR "Missing assets/${EMOJI_ASSET}. Run bash scripts/fetch-emoji-font.sh first.")
  endif()
endforeach()

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

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/cpu_scheduler.cpp
  src/media_render.cpp
  src/media_cache.cpp
  src/ui_cache.cpp
  src/animation_player.cpp
  src/voice_player.cpp
  src/audio_recorder.cpp
  src/telegram_client.cpp
  src/keyboard.cpp
  src/vita_net.cpp
  ${CMAKE_SOURCE_DIR}/external/tdlib-vita/vitaports/td_missing_func_placeholder.cpp
)

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

target_link_libraries(${PROJECT_NAME}
  Td::TdJsonStatic
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
  SceAudio_stub
  SceAudioIn_stub
  SceRtc_stub
  SceGxm_stub
  SceSysmodule_stub
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
  FILE assets/icon0.png sce_sys/icon0.png
  FILE assets/NotoColorEmoji.ttf assets/NotoColorEmoji.ttf
  FILE assets/Emoji-LICENSE.txt assets/Emoji-LICENSE.txt
  FILE assets/NotoSans-Regular.ttf assets/NotoSans-Regular.ttf
  FILE assets/NotoSans-LICENSE.txt assets/NotoSans-LICENSE.txt
  FILE ${ANIMATION_FFMPEG_ROOT}/share/COPYING.LGPLv2.1 assets/FFmpeg-LICENSE.txt
  ${RLOTTIE_VPK_FILES}
)
