if(NOT EXISTS "${VITAGRAM_WEBKIT_ROOT}/bin/vita-elf-create")
  message(FATAL_ERROR "Missing WebKit PIC converter; rerun scripts/build-webkit-vita.sh")
endif()
set(VITA_ELF_CREATE "${VITAGRAM_WEBKIT_ROOT}/bin/vita-elf-create")
set(WEBKIT_LIBRARIES)
foreach(LIB WebKit WebCore JavaScriptCore PAL WTF Skia wpe-1.0 curl xml2 psl harfbuzz icui18n icuuc icudata)
  if(NOT EXISTS "${VITAGRAM_WEBKIT_ROOT}/lib/lib${LIB}.a")
    message(FATAL_ERROR "Incomplete WebKit prefix: lib${LIB}.a")
  endif()
  list(APPEND WEBKIT_LIBRARIES "${VITAGRAM_WEBKIT_ROOT}/lib/lib${LIB}.a")
endforeach()
if(NOT EXISTS "${VITAGRAM_WEBKIT_ROOT}/share/licenses/WebCore/LICENSE-LGPL-2.1")
  message(FATAL_ERROR "Missing WebKit notices; rerun scripts/stage-webkit-vita.sh")
endif()
# WebCore needs column metadata; its private SQLite must not change TDLib's build flags.
get_target_property(SQLITE_SOURCE_DIR tdsqlite SOURCE_DIR)
get_directory_property(SQLITE_DIRECTORY_DEFINITIONS DIRECTORY "${SQLITE_SOURCE_DIR}" COMPILE_DEFINITIONS)
add_library(webkit-sqlite STATIC "${SQLITE_SOURCE_DIR}/sqlite/sqlite3.c")
target_include_directories(webkit-sqlite PRIVATE $<TARGET_PROPERTY:tdsqlite,INCLUDE_DIRECTORIES>)
target_compile_definitions(webkit-sqlite PRIVATE $<TARGET_PROPERTY:tdsqlite,COMPILE_DEFINITIONS> ${SQLITE_DIRECTORY_DEFINITIONS})
target_compile_options(webkit-sqlite PRIVATE $<TARGET_PROPERTY:tdsqlite,COMPILE_OPTIONS>
  -ffunction-sections -fdata-sections -USQLITE_OMIT_DECLTYPE)
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=crtbegin.o OUTPUT_VARIABLE CRTBEGIN OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=crtend.o OUTPUT_VARIABLE CRTEND OUTPUT_STRIP_TRAILING_WHITESPACE)
add_executable(vitagram-webkit
  ../module.cpp ../../webapp_view.cpp ../WPEBackendVita.cpp ../ProcessProviderVita.cpp
  ../trace.cpp ../threading_vita.cpp ../../memory_hygiene.cpp ../../vita_compat/vita_posix_stubs.cpp)
target_compile_features(vitagram-webkit PRIVATE cxx_std_20)
target_compile_definitions(vitagram-webkit PRIVATE VITAGRAM_WEBKIT_MODULE=1 WebAppView=WebAppEngine
  VITAGRAM_PERFORMANCE=0 EGL_NO_PLATFORM_SPECIFIC_TYPES=1 _GNU_SOURCE=1)
target_include_directories(vitagram-webkit PRIVATE ../.. "${VITAGRAM_WEBKIT_ROOT}/include"
  "${VITAGRAM_WEBKIT_ROOT}/include/wpe-1.0" "${VITASDK}/arm-vita-eabi/include/freetype2"
  "${PROJECT_SOURCE_DIR}/external/jansson/src" "${PROJECT_BINARY_DIR}/external/jansson/include")
target_link_options(vitagram-webkit PRIVATE -nostartfiles -Wl,-e,module_start -Wl,--gc-sections
  -Wl,--wrap=sceKernelCreateThread -Wl,--wrap=free -Wl,--defsym=__sce_headroom=0x10000)
target_link_libraries(vitagram-webkit "${CRTBEGIN}" "-Wl,-(" ${WEBKIT_LIBRARIES} webkit-sqlite jansson
  "${VITAGRAM_OPENSSL_ROOT}/lib/libssl.a" "${VITAGRAM_OPENSSL_ROOT}/lib/libcrypto.a"
  freetype bz2 webpdemux webp png jpeg z m c pthread "-Wl,-)" "${CRTEND}"
  SceKernelThreadMgr_stub SceSysmodule_stub SceNet_stub SceNetCtl_stub)
vita_create_self(webkit.suprx vitagram-webkit UNSAFE CONFIG ../module.yml)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/vitagram-webkit-abi1-r1.zip"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/package-webkit.py"
    "${CMAKE_CURRENT_BINARY_DIR}/webkit.suprx" "${VITAGRAM_WEBKIT_ROOT}/share/licenses"
    "${PROJECT_BINARY_DIR}/vitagram-webkit-abi1-r1.zip" "${VITAGRAM_OPENSSL_ROOT}/share/OpenSSL-LICENSE.txt"
  DEPENDS webkit.suprx-self "${CMAKE_CURRENT_BINARY_DIR}/webkit.suprx.out" "${PROJECT_SOURCE_DIR}/scripts/package-webkit.py")
add_custom_target(webkit-addon ALL DEPENDS "${PROJECT_BINARY_DIR}/vitagram-webkit-abi1-r1.zip")
