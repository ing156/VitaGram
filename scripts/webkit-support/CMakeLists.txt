cmake_minimum_required(VERSION 3.16)
project(wpe_vita LANGUAGES C CXX)
set(WPE "${WPE_SOURCE}")
add_library(wpe-1.0 STATIC
 ${WPE}/src/alloc.c ${WPE}/src/gamepad.c ${WPE}/src/input-xkb.c
 ${WPE}/src/key-unicode.c ${WPE}/src/pasteboard.c ${WPE}/src/pasteboard-generic.cpp
 ${WPE}/src/pasteboard-noop.cpp ${WPE}/src/process.c ${WPE}/src/renderer-backend-egl.c
 ${WPE}/src/renderer-host.c ${WPE}/src/version.c ${WPE}/src/view-backend.c ${WPE}/src/loader-static.c)
target_compile_definitions(wpe-1.0 PRIVATE WPE_COMPILATION=1 EGL_NO_PLATFORM_SPECIFIC_TYPES=1)
target_include_directories(wpe-1.0 PRIVATE ${WPE}/include ${WEBKIT_DEPS_PREFIX}/include)
install(TARGETS wpe-1.0 ARCHIVE DESTINATION lib)
install(DIRECTORY ${WPE}/include/wpe DESTINATION include/wpe-1.0 FILES_MATCHING PATTERN "*.h")
