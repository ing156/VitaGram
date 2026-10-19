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
# VitaSDK requires ##.##; the package version matches the final two source components.
set(VITA_VERSION  "38.09")
set(VITAGRAM_VERSION "0.38.09")
option(VITAGRAM_PERFORMANCE "Collect bounded performance diagnostics" ON)
option(VITAGRAM_TGS_DIAGNOSTIC "Save one rendered TGS frame for diagnosis" OFF)
option(VITAGRAM_PICKER_LOG "Write browsed folders and file names to ux0:data/vitagram/picker.log (plain text)" OFF)

# FFmpeg for all media: FFmpeg 7.1 LTS + scripts/ffmpeg-vita-7.1.patch (the Vita hardware H.264
# decoder), built by scripts/build-ffmpeg-vita.sh. VITAGRAM_FFMPEG_ROOT overrides the prefix.
set(VITAGRAM_FFMPEG_ROOT "" CACHE PATH "Override the FFmpeg install prefix")
unset(VITAGRAM_FFMPEG CACHE) # older build directories cached the removed legacy/7.1 switch
set(FFMPEG_BUILD_SCRIPT "scripts/build-ffmpeg-vita.sh")
set(ANIMATION_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/external/ffmpeg-vita71/install")
if(VITAGRAM_FFMPEG_ROOT)
  set(ANIMATION_FFMPEG_ROOT "${VITAGRAM_FFMPEG_ROOT}")
endif()
unset(ANIMATION_FFMPEG_ROOT CACHE) # an older build directory cached the legacy prefix here
message(STATUS "FFmpeg: ${ANIMATION_FFMPEG_ROOT}")
foreach(ANIMATION_LIB avformat avcodec swscale swresample avutil)
  if(NOT EXISTS "${ANIMATION_FFMPEG_ROOT}/lib/lib${ANIMATION_LIB}.a")
    message(FATAL_ERROR "Missing FFmpeg with the Vita hardware decoder. Run bash ${FFMPEG_BUILD_SCRIPT} first.")
  endif()
endforeach()
if(NOT EXISTS "${ANIMATION_FFMPEG_ROOT}/share/COPYING.LGPLv2.1")
  message(FATAL_ERROR "Missing FFmpeg license; run bash ${FFMPEG_BUILD_SCRIPT}")
endif()

find_path(WEBP_INCLUDE_DIR webp/decode.h)
find_library(WEBP_LIBRARY NAMES webp)
if(NOT WEBP_INCLUDE_DIR OR NOT WEBP_LIBRARY)
  message(FATAL_ERROR "VitaSDK libwebp is required for stickers. Install the VitaSDK libwebp package first.")
endif()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/cacert.pem")
  message(FATAL_ERROR "Missing assets/cacert.pem (update checks need it). Run bash scripts/fetch-ca-bundle.sh first.")
endif()
foreach(EMOJI_ASSET NotoColorEmoji_b.ttf Emoji-LICENSE.txt NotoSans-Regular.ttf NotoSans-LICENSE.txt NotoSansCoptic-Regular.ttf NotoSansCoptic-LICENSE.txt NotoSansJP-Regular.otf NotoSansJP-LICENSE.txt NotoSansMath-Regular.ttf NotoSansMath-LICENSE.txt NotoSansSymbols2-Regular.ttf NotoSansSymbols2-LICENSE.txt)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/${EMOJI_ASSET}")
    message(FATAL_ERROR "Missing assets/${EMOJI_ASSET}. Run bash scripts/fetch-emoji-font.sh and bash scripts/fetch-fallback-fonts.sh first.")
  endif()
endforeach()
# Smaller font copies from scripts/shrink-fonts.sh: 64 px emoji strike (the UI
# draws emoji at about 31 px at most) and JIS-only kanji for the Japanese font.
set(EMOJI_FONT "assets/NotoColorEmoji_b-64.ttf")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${EMOJI_FONT}")
  message(WARNING "Packaging the full-size emoji font (+~5 MB). Run bash scripts/shrink-fonts.sh and reconfigure.")
  set(EMOJI_FONT "assets/NotoColorEmoji_b.ttf")
endif()
set(JAPANESE_FONT "assets/NotoSansJP-JIS.otf")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${JAPANESE_FONT}")
  message(WARNING "Packaging the full Japanese font (+~1.6 MB). Run bash scripts/shrink-fonts.sh and reconfigure.")
  set(JAPANESE_FONT "assets/NotoSansJP-Regular.otf")
endif()

foreach(CJK_FONT KR SC TC)
  set(CJK_${CJK_FONT}_FONT "assets/NotoSans${CJK_FONT}-Subset.otf")
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${CJK_${CJK_FONT}_FONT}")
    message(WARNING "Packaging the full-size NotoSans${CJK_FONT} font. Run bash scripts/fetch-fallback-fonts.sh and bash scripts/shrink-fonts.sh, then reconfigure.")
    set(CJK_${CJK_FONT}_FONT "assets/NotoSans${CJK_FONT}-Regular.otf")
  endif()
endforeach()

# Pinned dependencies are git submodules (.gitmodules); VitaGram's changes to them live in
# scripts/*-vita.patch and are applied here once, so a fresh clone builds the same code.
find_package(Git REQUIRED)
function(vitagram_apply_patch dir patch)
  if(NOT EXISTS "${dir}/.git")
    message(FATAL_ERROR "${dir} is missing. Run: git submodule update --init")
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" apply --reverse --check "${patch}"
                  RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
  if(already_applied EQUAL 0)
    return()
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" apply "${patch}"
                  RESULT_VARIABLE applied ERROR_VARIABLE apply_error)
  if(NOT applied EQUAL 0)
    message(FATAL_ERROR "Cannot apply ${patch} to ${dir}:\n${apply_error}")
  endif()
  message(STATUS "Applied ${patch}")
endfunction()
vitagram_apply_patch("${CMAKE_SOURCE_DIR}/external/jansson" "${CMAKE_SOURCE_DIR}/scripts/jansson-vita.patch")

set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared jansson" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "Build jansson examples" FORCE)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "Build jansson documentation" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "Skip jansson tests" FORCE)
set(JANSSON_INSTALL OFF CACHE BOOL "Install jansson" FORCE)
set(USE_URANDOM OFF CACHE BOOL "Use /dev/urandom in jansson" FORCE)
add_subdirectory(external/jansson EXCLUDE_FROM_ALL)

# OpenSSL 3.5 LTS built by scripts/build-openssl.sh replaces the SDK's OpenSSL 1.0.2 (2016) for
# TDLib and the app. Its headers go first so nothing compiles against the SDK copy.
set(VITAGRAM_OPENSSL_ROOT "${CMAKE_SOURCE_DIR}/external/openssl-vita/install" CACHE PATH "Vita OpenSSL 3.5 prefix")
if(NOT EXISTS "${VITAGRAM_OPENSSL_ROOT}/lib/libcrypto.a" OR NOT EXISTS "${VITAGRAM_OPENSSL_ROOT}/include/openssl/opensslv.h")
  message(FATAL_ERROR "OpenSSL 3.5 for the Vita not found in ${VITAGRAM_OPENSSL_ROOT}. Run: bash scripts/build-openssl.sh")
endif()
set(OPENSSL_ROOT_DIR "${VITAGRAM_OPENSSL_ROOT}")
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OPENSSL_INCLUDE_DIR "${VITAGRAM_OPENSSL_ROOT}/include" CACHE PATH "OpenSSL headers" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${VITAGRAM_OPENSSL_ROOT}/lib/libcrypto.a" CACHE FILEPATH "OpenSSL libcrypto" FORCE)
set(OPENSSL_SSL_LIBRARY "${VITAGRAM_OPENSSL_ROOT}/lib/libssl.a" CACHE FILEPATH "OpenSSL libssl" FORCE)
include_directories(BEFORE "${VITAGRAM_OPENSSL_ROOT}/include")

set(TDLIB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/tdlib-upstream" CACHE PATH "Path to the Vita-patched TDLib")
if(NOT EXISTS "${TDLIB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "TDLib not found in ${TDLIB_SOURCE_DIR}. Run: git submodule update --init")
endif()

# Vita-patched TDLib. It exports Td::TdJsonStatic in the upstream-style CMake build.
set(__vita__ TRUE CACHE BOOL "Build TDLib for PS Vita" FORCE)
set(TD_ENABLE_TESTS OFF CACHE BOOL "Build TDLib tests" FORCE)
include_directories(BEFORE "${CMAKE_SOURCE_DIR}/src/vita_compat")
if(TDLIB_SOURCE_DIR STREQUAL "${CMAKE_SOURCE_DIR}/external/tdlib-upstream")
  vitagram_apply_patch("${TDLIB_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/scripts/tdlib-vita.patch")
endif()
if(NOT EXISTS "${TDLIB_SOURCE_DIR}/td/generate/auto/td/telegram/td_api.h" OR
   NOT EXISTS "${TDLIB_SOURCE_DIR}/tdutils/generate/auto/mime_type_to_extension.cpp")
  message(FATAL_ERROR "TDLib's generated sources are missing. Run once on the host: bash scripts/prepare-tdlib.sh")
endif()
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

# Themed L/R glyphs use sprite rectangles from the UI pack manifest.
set(PHOTO_UI_ROOT "${CMAKE_SOURCE_DIR}/assets/VitaGram_UI_Assets_v1")
set(PHOTO_UI_VPK_FILES) # The shared manifest is packaged once by UI_VPK_FILES below.
foreach(PHOTO_UI_THEME dark light vhbb)
  list(APPEND PHOTO_UI_VPK_FILES FILE "${PHOTO_UI_ROOT}/themes/${PHOTO_UI_THEME}/ps_buttons_atlas.png"
    ui/themes/${PHOTO_UI_THEME}/ps_buttons_atlas.png)
endforeach()

# PNG UI skin: ship all themes, but keep only the selected theme in RAM.
# PNG UI skin (assets/VitaGram_UI_Assets_v1): the manifest and only the textures src/ui_skin.cpp loads,
# packaged as app0:ui/. The optional stronger wave wallpaper replaces the default one with
# -DVITAGRAM_UI_ALT_BACKGROUND=ON.
option(VITAGRAM_UI_ALT_BACKGROUND "Use background_alt_960x544.png as the wallpaper" OFF)
set(UI_ASSET_ROOT "${CMAKE_SOURCE_DIR}/assets/VitaGram_UI_Assets_v1")
set(UI_SLICES panel row row_focused tab_normal tab_active menu menu_item_focus date_chip media_frame focus_ring
    progress_track progress_fill scroll_thumb dialog)
set(UI_BACKGROUND background_960x544.png)
if(VITAGRAM_UI_ALT_BACKGROUND)
  set(UI_BACKGROUND background_alt_960x544.png)
endif()
set(UI_VPK_FILES)
list(APPEND UI_VPK_FILES FILE "${UI_ASSET_ROOT}/manifest.json" ui/manifest.json)
file(GLOB_RECURSE UI_MASK_FILES CONFIGURE_DEPENDS "${UI_ASSET_ROOT}/masks/*.png" "${UI_ASSET_ROOT}/masks/manifest.json")
foreach(UI_MASK_FILE ${UI_MASK_FILES})
  file(RELATIVE_PATH UI_MASK_PATH "${UI_ASSET_ROOT}" "${UI_MASK_FILE}")
  list(APPEND UI_VPK_FILES FILE "${UI_MASK_FILE}" "ui/${UI_MASK_PATH}")
endforeach()
list(APPEND UI_VPK_FILES FILE "${UI_ASSET_ROOT}/themes/vhbb/NOTICE.txt" ui/themes/vhbb/NOTICE.txt
    FILE "${UI_ASSET_ROOT}/themes/vhbb/LICENSE.txt" ui/themes/vhbb/LICENSE.txt)
list(APPEND UI_VPK_FILES FILE "${CMAKE_SOURCE_DIR}/assets/ui_extra/corner_buttons.png" ui/corner_buttons.png)
foreach(UI_GLOW_THEME dark light)
  list(APPEND UI_VPK_FILES FILE "${UI_ASSET_ROOT}/themes/${UI_GLOW_THEME}/fixed/focus_edge_glow_16x128.png"
    ui/themes/${UI_GLOW_THEME}/fixed/focus_edge_glow_16x128.png)
endforeach()
foreach(UI_THEME dark light vhbb)
  set(UI_THEME_DIR "${UI_ASSET_ROOT}/themes/${UI_THEME}")
  foreach(UI_SLICE ${UI_SLICES})
    list(APPEND UI_VPK_FILES FILE "${UI_THEME_DIR}/9slice/${UI_SLICE}.png" ui/themes/${UI_THEME}/9slice/${UI_SLICE}.png)
  endforeach()
  foreach(UI_FIXED ${UI_BACKGROUND} header_960x40.png active_indicator_128x4.png)
    list(APPEND UI_VPK_FILES FILE "${UI_THEME_DIR}/fixed/${UI_FIXED}" ui/themes/${UI_THEME}/fixed/${UI_FIXED})
  endforeach()
  list(APPEND UI_VPK_FILES FILE "${UI_THEME_DIR}/ui_icons_atlas.png" ui/themes/${UI_THEME}/ui_icons_atlas.png)
  if(NOT DEFINED PHOTO_UI_VPK_FILES)
    list(APPEND UI_VPK_FILES FILE "${UI_THEME_DIR}/ps_buttons_atlas.png" ui/themes/${UI_THEME}/ps_buttons_atlas.png)
  endif()
endforeach()
foreach(UI_PALETTE little-greene-light little-greene-dark)
  list(APPEND UI_VPK_FILES FILE "${UI_ASSET_ROOT}/palettes/${UI_PALETTE}.json" "ui/palettes/${UI_PALETTE}.json")
endforeach()
foreach(UI_PALETTE black-blue black-red)
  list(APPEND UI_VPK_FILES FILE "${UI_ASSET_ROOT}/palettes/${UI_PALETTE}/palette.json" "ui/palettes/${UI_PALETTE}/palette.json")
endforeach()
foreach(UI_PATH ${UI_VPK_FILES})
  if(NOT UI_PATH STREQUAL "FILE" AND IS_ABSOLUTE "${UI_PATH}" AND NOT EXISTS "${UI_PATH}")
    message(FATAL_ERROR "Missing UI asset ${UI_PATH}")
  endif()
endforeach()
if(VITAGRAM_UI_ALT_BACKGROUND)
  set(UI_DEFINITIONS VITAGRAM_UI_ALT_BACKGROUND=1)
endif()

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/gxm_guard.cpp
  src/ui_battery.cpp
  src/i18n.cpp
  src/download_folder_picker.cpp
  src/upload_picker.cpp
  src/utf8_decode.cpp
  src/cpu_scheduler.cpp
  src/td_diagnostics.cpp
  src/media_render.cpp
  src/ui_skin.cpp
  src/text_font.cpp
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
  src/custom_keyboard.cpp
  src/keyboard_layout.cpp
  src/keyboard_case.cpp
  src/proxy_link.cpp
  src/secure_store.cpp
  src/vault_crypto.cpp
  src/vault.cpp
  src/vault_sweep.cpp
  src/vault_avio.cpp
  src/vault_readahead.cpp
  src/vault_ui.cpp
  src/memory_hygiene.cpp
  src/crash_dump.cpp
  src/process_modules.cpp
  src/qr_scanner.cpp
  src/qr_encode.cpp
  src/qr_render.cpp
  src/video_note_encoder.cpp
  src/video_note_recorder.cpp
  src/vita_net.cpp
  src/update_core.cpp
  src/https_client.cpp
  src/updater.cpp
  src/vpk_package.cpp
  src/vpk_installer.cpp
  src/package_promote.cpp
  src/vita_compat/vita_posix_stubs.cpp
)
# Decryption runs while video plays: keep the ChaCha20/Poly1305 loops fully optimized.
set_source_files_properties(src/vault_crypto.cpp PROPERTIES COMPILE_OPTIONS "-O3")
set_source_files_properties(src/download_folder_picker.cpp PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(src/upload_picker.cpp PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(src/main.cpp src/ui_skin.cpp src/vault_ui.cpp PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(src/portrait_video.cpp src/video_note_encoder.cpp src/qr_scanner.cpp src/update_core.cpp src/https_client.cpp src/updater.cpp
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
  ${UI_DEFINITIONS}
  VITAGRAM_VERSION="${VITAGRAM_VERSION}"
  VITAGRAM_PERFORMANCE=$<BOOL:${VITAGRAM_PERFORMANCE}>
  VITAGRAM_TGS_DIAGNOSTIC=$<BOOL:${VITAGRAM_TGS_DIAGNOSTIC}>
  VITAGRAM_PICKER_LOG=$<BOOL:${VITAGRAM_PICKER_LOG}>
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
foreach(GXM_CALL BeginScene ReserveVertexDefaultUniformBuffer ReserveFragmentDefaultUniformBuffer SetUniformDataF Draw)
  target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=sceGxm${GXM_CALL}")
endforeach()
# Every free() zeroes the block first while the vault is on (src/memory_hygiene.h).
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=free")
# The prepared VitaSDK linker script reserves this gap for vita-elf-create's
# SCE metadata, whose size can exceed the incidental padding after code.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--defsym=__sce_headroom=0x10000")

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
  ScePromoterUtil_stub
)

# VitaGram Updater: a separate tiny application that installs a staged update while VitaGram is not
# running (the console cannot replace a running application); the scheme PSVitaAlive's self-updater uses.
# Its own directory keeps vita_create_vpk()'s accumulated flags apart from the main package.
add_subdirectory(src/updater_app)
# head.bin of the updater app as the console's installer wrote it (ux0:app/VGUPDATE1/sce_sys/package/head.bin
# after installing vitagram-updater.vpk once with VitaShell). Otherwise the installer regenerates
# a header for the updater title from VitaGram's installed homebrew header.
set(UPDATER_HEAD_FILES)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/updater-head.bin")
  list(APPEND UPDATER_HEAD_FILES FILE "${CMAKE_SOURCE_DIR}/assets/updater-head.bin" updater/head.bin)
endif()

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE external/minih264/LICENSE assets/minih264-LICENSE.txt
  FILE external/quirc/LICENSE assets/quirc-LICENSE.txt
  FILE assets/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_BINARY_DIR}/src/updater_app/vitagram-updater.vpk updater/vitagram-updater.vpk
  ${UPDATER_HEAD_FILES}
  FILE assets/cacert.pem certs/cacert.pem
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
  FILE ${CJK_KR_FONT} assets/NotoSansKR-Regular.otf
  FILE ${CJK_SC_FONT} assets/NotoSansSC-Regular.otf
  FILE ${CJK_TC_FONT} assets/NotoSansTC-Regular.otf
  FILE assets/NotoSansMath-Regular.ttf assets/NotoSansMath-Regular.ttf
  FILE assets/NotoSansMath-LICENSE.txt assets/NotoSansMath-LICENSE.txt
  FILE assets/NotoSansSymbols2-Regular.ttf assets/NotoSansSymbols2-Regular.ttf
  FILE assets/NotoSansSymbols2-LICENSE.txt assets/NotoSansSymbols2-LICENSE.txt
  FILE ${ANIMATION_FFMPEG_ROOT}/share/COPYING.LGPLv2.1 assets/FFmpeg-LICENSE.txt
  FILE LICENSE assets/VitaGram-LICENSE.txt
  FILE external/tdlib-upstream/LICENSE_1_0.txt assets/TDLib-LICENSE.txt
  FILE external/jansson/LICENSE assets/Jansson-LICENSE.txt
  FILE ${VITAGRAM_OPENSSL_ROOT}/share/OpenSSL-LICENSE.txt assets/OpenSSL-LICENSE.txt
  FILE assets/vita2d-LICENSE.txt assets/vita2d-LICENSE.txt
  FILE assets/VitaGram_UI_Assets_v1/themes/dark/buttons/L.png ui/themes/dark/buttons/L.png
  FILE assets/VitaGram_UI_Assets_v1/themes/dark/buttons/R.png ui/themes/dark/buttons/R.png
  ${PHOTO_UI_VPK_FILES}
  ${LANGUAGE_VPK_FILES}
  ${UI_VPK_FILES}
  ${RLOTTIE_VPK_FILES}
)
add_dependencies(${PROJECT_NAME}.vpk-vpk vitagram-updater.vpk-vpk) # the updater's VPK is packaged inside
