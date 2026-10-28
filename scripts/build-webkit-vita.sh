#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${VITASDK:=/usr/local/vitasdk}"
work=${VITAGRAM_WEBKIT_WORK:-$root/external/webkit-vita-work}
mkdir -p "$work"
work=$(realpath "$work")
deps="$work/deps"
mkdir -p "$deps/include" "$deps/lib"
jobs=$(nproc)
for tool in cmake ruby bison flex unifdef make patch curl tar sha256sum python3; do command -v "$tool" >/dev/null; done
test -s "$root/build-modern/tdlib/sqlite/libtdsqlite.a" || { echo 'Build VitaGram in build-modern first (TDLib SQLite is reused).' >&2; exit 1; }
openssl="$root/external/openssl-vita/install"
test -s "$openssl/lib/libcrypto.a"
fetch() {
    local file="$work/$1" url=$2 hash=$3
    if [[ ! -f "$file" ]]; then curl --fail --silent --show-error --location --retry 3 "$url" -o "$file.download"; mv "$file.download" "$file"; fi
    printf '%s  %s\n' "$hash" "$file" | sha256sum --check --status
}
unpack() { if [[ ! -d "$work/$2" ]]; then mkdir -p "$work/$2"; tar -xf "$work/$1" -C "$work/$2" --strip-components=1; fi; }
fetch port.tar.gz https://codeload.github.com/ArchUserDesu/vita-stream-browser/tar.gz/243a2e39965f8a3ad7d709b4d72b2fba7f01c1a9 7252a4de894acce8b6db99aa4de32a63c79fbf9f5d73b2b72ac69abbea9d8191
unpack port.tar.gz port
# Keep the patched PIC converter local; the installed VitaSDK is unchanged.
fetch vita-toolchain.tar.gz https://codeload.github.com/vitasdk/vita-toolchain/tar.gz/eacff34d18e9872a78c0e520e1997ef71900ebb4 bd307a48cbd37a55960f2d2610f26d3268018cc9606d45542feb1b7d8c9ad06e
fetch libelf.tar.gz https://repository.timesys.com/buildsources/l/libelf/libelf-0.8.13/libelf-0.8.13.tar.gz 591a9b4ec81c1f2042a97aa60564e0cb79d041c52faa7416acb38bc95bd2c76d
fetch libyaml.tar.gz https://pyyaml.org/download/libyaml/yaml-0.2.5.tar.gz c642ae9b75fee120b2d96c712538bd2cf283228d2337df2cf2988e3c02678ef4
for name in vita-toolchain libelf libyaml; do unpack "$name.tar.gz" "$name"; done
if [[ ! -f "$work/vita-toolchain/.vitagram-pic" ]]; then
    patch --batch -d "$work/vita-toolchain" -p1 < "$work/port/browser/patches/vita-toolchain-large-pic.patch"
    touch "$work/vita-toolchain/.vitagram-pic"
fi
host_deps="$work/host-deps"
if [[ ! -s "$host_deps/lib/libelf.a" ]]; then
    (cd "$work/libelf"
     CC="${HOST_CC:-cc}" CFLAGS='-O2 -std=gnu99 -Wno-error=implicit-int -Wno-error=implicit-function-declaration -Wno-error=int-conversion' ./configure --prefix="$host_deps" --disable-shared
     make -j"$jobs"
     make install)
fi
if [[ ! -s "$host_deps/lib/libyaml.a" ]]; then
    (cd "$work/libyaml"
     CC="${HOST_CC:-cc}" CFLAGS='-O2' ./configure --prefix="$host_deps" --disable-shared
     make -j"$jobs"
     make install)
fi
tool_sources=(
    "$work/vita-toolchain/src/vita-elf-create/"{vita-elf-create,elf-create-argp,vita-elf,elf-defs,elf-utils,sce-elf}.c
    "$work/vita-toolchain/src/utils/"{varray,yamlemitter,strndup,yamltree,yamltreeutil,sha256}.c
    "$work/vita-toolchain/src/"{vita-export-parse,vita-import,vita-import-parse}.c
)
"${HOST_CC:-cc}" -O2 -std=gnu99 -fno-strict-aliasing -DHAVE_STRNDUP \
    -I"$work/vita-toolchain/src" -I"$host_deps/include" -I"$host_deps/include/libelf" \
    "${tool_sources[@]}" "$host_deps/lib/libelf.a" "$host_deps/lib/libyaml.a" -o "$work/vita-elf-create"
python3 "$work/vita-toolchain/test/test_elf_create.py" "$work/vita-elf-create"
fetch webkit.tar.xz https://deb.debian.org/debian/pool/main/w/webkit2gtk/webkit2gtk_2.50.6.orig.tar.xz 2b281abf8894ffc6172152e5660b75eeeedbe1cc43d6783d09dc79f7c865bb42
unpack webkit.tar.xz source
mkdir -p "$work/source/Source/WTF/wtf/vita"
cmp -s "$root/src/webview/VitaThreading.h" "$work/source/Source/WTF/wtf/vita/VitaThreading.h" || \
    cp "$root/src/webview/VitaThreading.h" "$work/source/Source/WTF/wtf/vita/"
cmp -s "$root/src/webview/VitaSharedMemory.h" "$work/source/Source/WTF/wtf/vita/VitaSharedMemory.h" || \
    cp "$root/src/webview/VitaSharedMemory.h" "$work/source/Source/WTF/wtf/vita/"
integration_hash=$(sha256sum "$root/scripts/webkit-vita-integration.patch")
integration_hash=${integration_hash%% *}
if [[ -f "$work/source/.vitagram-port" && $(< "$work/source/.vitagram-port") != "$integration_hash" ]]; then
    echo "Cached WebKit source uses another integration patch. Use a fresh VITAGRAM_WEBKIT_WORK directory." >&2
    exit 2
fi
if [[ ! -f "$work/source/.vitagram-port" ]]; then
    patch --batch -d "$work/source" -p1 < "$work/port/browser/patches/webkit-2.50.6-vita-full.patch"
    patch --batch -d "$work/source" -p1 < "$root/scripts/webkit-vita-integration.patch"
    printf '%s\n' "$integration_hash" > "$work/source/.vitagram-port"
fi
fetch icu.tgz https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-src.tgz 818a80712ed3caacd9b652305e01afc7fa167e6f2e94996da44b90c2ab604ce1
if [[ ! -d "$work/port/third_party/icu-vita-full/icu" ]]; then
    mkdir -p "$work/port/third_party/icu-vita-full"
    tar -xf "$work/icu.tgz" -C "$work/port/third_party/icu-vita-full"
fi
icu_source="$work/port/third_party/icu-vita-full/icu/source"
cp "$icu_source/config/mh-linux" "$icu_source/config/mh-unknown"
JOBS="$jobs" bash "$work/port/tools/build-icu-full-vita.sh"
icu="$work/port/deps/icu-vita-full"
fetch harfbuzz.tar.gz https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/refs/tags/14.6.0 eca8581e5ad8e21dbda31556c0363f764b1a754c9c1bb33ca7cd56ca1c07ddc9
fetch libxml2.tar.xz https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz 98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
fetch libpsl.tar.gz https://github.com/rockdaboot/libpsl/releases/download/0.23.3/libpsl-0.23.3.tar.gz 93941f85a1e7bd593fa94f299233cb5dfc91cd144fd9a78a6ceb75001c5b03be
fetch curl.tar.xz https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
fetch libwpe.tar.gz https://codeload.github.com/WebPlatformForEmbedded/libwpe/tar.gz/refs/tags/1.16.3 4a2ddb2e9831d6c7208a5cc04fa82335c9a13512414762911467e05f43263b97
for name in harfbuzz libpsl libwpe; do unpack "$name.tar.gz" "$name"; done
for name in libxml2 curl; do unpack "$name.tar.xz" "$name"; done
configure() {
    local source=$1 build=$2; shift 2
    cmake -S "$source" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
        -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF "$@"
    cmake --build "$build" --parallel "$jobs"
    cmake --install "$build"
}
configure "$work/harfbuzz" "$work/harfbuzz-build" -DHB_BUILD_SUBSET=OFF -DHB_BUILD_RASTER=OFF -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF -DHB_HAVE_FREETYPE=ON
configure "$work/libxml2" "$work/libxml2-build" -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_MODULES=OFF
if [[ ! -f "$work/libpsl/Makefile" ]]; then
    (cd "$work/libpsl" && CC="$VITASDK/bin/arm-vita-eabi-gcc" AR="$VITASDK/bin/arm-vita-eabi-ar" RANLIB="$VITASDK/bin/arm-vita-eabi-ranlib" \
     LIBICU_CFLAGS="-I$icu/include" LIBICU_LIBS="-L$icu/lib -licui18n -licuuc -licudata -lstdc++ -lm" \
     ./configure --host=arm-vita-eabi --prefix="$deps" --disable-shared --enable-static --enable-runtime=libicu --disable-gtk-doc --disable-man --disable-nls)
fi
make -C "$work/libpsl" -j"$jobs"
make -C "$work/libpsl" install
configure "$work/curl" "$work/curl-build" -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=ON \
    -DOPENSSL_INCLUDE_DIR="$openssl/include" -DOPENSSL_CRYPTO_LIBRARY="$openssl/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$openssl/lib/libssl.a" \
    -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_ZSTD=OFF -DCURL_BROTLI=OFF \
    -DENABLE_IPV6=OFF -DHAVE_PIPE2=OFF -DHAVE_PIPE=OFF -DHAVE_SOCKETPAIR=OFF
cp -R "$root/scripts/webkit-support/EGL" "$root/scripts/webkit-support/KHR" "$deps/include/"
# libwpe's Git tag normally generates this header through Meson.
cp "$root/scripts/webkit-support/libwpe-version.h" "$work/libwpe/include/wpe/"
configure "$root/scripts/webkit-support" "$work/wpe-build" -DWPE_SOURCE="$work/libwpe" -DWEBKIT_DEPS_PREFIX="$deps"
cp "$root/scripts/webkit-support/wpe-1.0/wpe/vita.h" "$deps/include/wpe-1.0/wpe/"
cp "$root/scripts/webkit-support/harfbuzz/hb-icu.h" "$deps/include/harfbuzz/"
cp "$root/build-modern/tdlib/sqlite/libtdsqlite.a" "$deps/lib/libsqlite3.a"
cp "$root/external/tdlib-upstream/sqlite/sqlite/sqlite3.h" "$deps/include/tdsqlite3.h"
# Keep WebKit on the same SQLite implementation as TDLib, including its ABI prefix.
python3 "$root/scripts/webkit-support/sqlite-header.py" "$deps/include"
cmake -S "$work/source" -B "$work/build" -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DPORT=Vita -DVITA_BUILD_WEBKIT=ON -DVITA_BROWSER_DEPS="$deps" -DVITA_BROWSER_APP_ROOT= \
    -DCMAKE_C_FLAGS_RELEASE='-Os -DNDEBUG' -DCMAKE_CXX_FLAGS_RELEASE='-Os -DNDEBUG' \
    -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_C_FLAGS='-D_GNU_SOURCE=1 -DEGL_NO_PLATFORM_SPECIFIC_TYPES=1' \
    -DCMAKE_CXX_FLAGS='-D_GNU_SOURCE=1 -DEGL_NO_PLATFORM_SPECIFIC_TYPES=1' \
    -DICU_INCLUDE_DIR="$icu/include" -DICU_DATA_LIBRARY_RELEASE="$icu/lib/libicudata.a" \
    -DICU_I18N_LIBRARY_RELEASE="$icu/lib/libicui18n.a" -DICU_UC_LIBRARY_RELEASE="$icu/lib/libicuuc.a" \
    -DCURL_INCLUDE_DIR="$deps/include" -DCURL_LIBRARY_RELEASE="$deps/lib/libcurl.a" \
    -DHarfBuzz_INCLUDE_DIR="$deps/include/harfbuzz" -DHarfBuzz_LIBRARY="$deps/lib/libharfbuzz.a" \
    -DOPENSSL_INCLUDE_DIR="$openssl/include" -DOPENSSL_CRYPTO_LIBRARY="$openssl/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$openssl/lib/libssl.a"
cmake --build "$work/build" --target WebKit --parallel "$jobs"
bash "$root/scripts/stage-webkit-vita.sh" "$work/build" "$deps" "$icu" "$root/external/webkit-vita"
