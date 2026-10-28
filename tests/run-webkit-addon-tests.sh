#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-webkit-addon.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
openssl ecparam -name prime256v1 -genkey -noout -out "$work/key.pem" 2>/dev/null
openssl pkey -in "$work/key.pem" -pubout -out "$work/pub.pem"
python3 - "$root" "$work" <<'PY'
import json, pathlib, subprocess, sys, zipfile
root, work = map(pathlib.Path, sys.argv[1:])
# Replace only the embedded trust root in the test copy; verification uses production code.
source = (root / 'src/update_core.cpp').read_text().replace('#include "update_key.h"', '#include "test_key.h"')
(work / 'update_core.cpp').write_text(source)
(work / 'test_key.h').write_text('#define VITAGRAM_UPDATE_PUBLIC_KEY ' + json.dumps((work / 'pub.pem').read_text()))
for kind in ['good', 'incompatible']:
    directory = work / kind
    directory.mkdir()
    with zipfile.ZipFile(directory / 'addon.zip', 'w') as archive:
        archive.writestr('compatibility', 'webkit-abi1-r1\n' if kind == 'good' else 'wrong-abi\n')
        archive.writestr('webkit.suprx', 'TEST MODULE')
        archive.writestr('licenses/WebCore/LICENSE-LGPL-2.1', 'TEST NOTICE')
# Exercise the release packager and check that ABI/ID agree with the loader header.
source = work / 'source'
(source / 'licenses/WebCore').mkdir(parents=True)
(source / 'webkit.suprx').write_bytes(b'TEST MODULE')
(source / 'licenses/WebCore/LICENSE-LGPL-2.1').write_text('TEST NOTICE')
(source / 'OpenSSL-LICENSE.txt').write_text('TEST NOTICE')
subprocess.run([sys.executable, str(root / 'scripts/package-webkit.py'), str(source / 'webkit.suprx'),
                str(source / 'licenses'), str(work / 'good/addon.zip'), str(source / 'OpenSSL-LICENSE.txt')], check=True)
with zipfile.ZipFile(work / 'good/addon.zip') as archive:
    identity = archive.read('compatibility').decode().strip()
    assert '#define VITAGRAM_WEBKIT_ID "' + identity + '"' in (root / 'src/webview/module_api.h').read_text()
    assert archive.read('webkit.suprx') == b'TEST MODULE'
PY
for kind in good incompatible; do
  bash "$root/scripts/sign-webkit.sh" "$work/$kind/addon.zip" "$work/key.pem" >/dev/null
  mv "$work/$kind/addon.zip.sig" "$work/$kind/addon.sig"
done
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -I "$root/src" -I "$root/external/jansson/src" \
  -I "$root/build-modern/external/jansson/include" "$root/src/webkit_download.cpp" "$work/update_core.cpp" \
  "$root/tests/webkit_download_test.cpp" -Wl,--wrap=pthread_create -lcrypto -lz -l:libjansson.so.4 -o "$work/test"
for scenario in good missing offline tampered cancel incompatible thread-error; do
  mkdir "$work/run-$scenario"
  fixture=good
  if [[ $scenario == incompatible ]]; then fixture=incompatible; fi
  (cd "$work/run-$scenario" && "$work/test" "$work/$fixture" "$scenario")
done
mkdir -p "$work/stubs/psp2/kernel"
cat > "$work/stubs/psp2/kernel/modulemgr.h" <<'EOF'
#pragma once
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_FAILED 2
#define SCE_KERNEL_STOP_CANCEL 1
#define SCE_KERNEL_STOP_SUCCESS 0
using SceSize = unsigned;
extern "C" int module_start(SceSize, void*);
int sceKernelLoadStartModule(const char*, unsigned, void*, int, void*, int*);
EOF
cat > "$work/stubs/vita2d.h" <<'EOF'
#pragma once
struct vita2d_texture {};
enum SceGxmTextureFormat { TestFormat };
extern "C" vita2d_texture* vita2d_create_empty_texture_format(unsigned, unsigned, SceGxmTextureFormat);
extern "C" void vita2d_free_texture(vita2d_texture*);
extern "C" void* vita2d_texture_get_datap(const vita2d_texture*);
extern "C" unsigned vita2d_texture_get_stride(const vita2d_texture*);
EOF
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$work/stubs" -I "$root/src" \
  "$root/src/webapp_loader.cpp" "$root/src/memory_hygiene.cpp" "$root/tests/webkit_loader_test.cpp" -o "$work/loader"
"$work/loader"
"$work/loader" bad-abi
echo "WebKit loader: absent engine, verification gate, ABI rejection, callbacks and single load passed"
cat > "$work/stubs/psp2/kernel/sysmem.h" <<'EOF'
#pragma once
struct SceKernelFreeMemorySizeInfo { int size, size_user, size_cdram, size_phycont; };
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo*);
EOF
cat > "$work/stubs/webapp_view.h" <<'EOF'
#pragma once
#include "proxy_config.h"
#include <string>
struct vita2d_texture;
int pthread_init();
class WebAppView {
    std::string error_;
public:
    bool open(const std::string&, bool, bool, const ProxyConfig&) { return true; }
    void close() {} void shutdown() {} bool active() const { return false; } void pump() {}
    void move(int, int) {} void click(int, int, bool) {} void scroll(int, int) {} void orientation(int, bool) {}
    int rotation() const { return 0; } bool fullscreen() const { return false; } void fontScale(float) {}
    void evaluate(const std::string&) {} void event(const std::string&, const std::string&) {}
    std::string takeEvents() { return {}; } std::string takeLink() { return {}; }
    const std::string& error() const { return error_; } bool loading() const { return false; }
    vita2d_texture* texture() const { return nullptr; }
};
EOF
# Keep the host's CRT _init/_fini distinct from the module's no-op entrypoints.
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -D_init=webkit_test_init -D_fini=webkit_test_fini \
  -I "$work/stubs" -I "$root/src" "$root/src/webview/module.cpp" "$root/tests/webkit_module_test.cpp" -o "$work/module"
"$work/module"
echo "WebKit module: invalid ABI and low memory fail before runtime initialization; live module cannot unload"
