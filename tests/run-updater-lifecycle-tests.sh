#!/usr/bin/env bash
# Runs the real updater and helper on the host with mocked HTTPS and Vita system calls.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-updater-lifecycle.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/fixtures" "$work/runtime"
openssl ecparam -name prime256v1 -genkey -noout -out "$work/key.pem" 2>/dev/null
openssl ec -in "$work/key.pem" -pubout -out "$work/pub.pem" 2>/dev/null
python3 -I - "$work" <<'PY'
import json, pathlib, struct, sys, zipfile
work = pathlib.Path(sys.argv[1]); fixtures = work / 'fixtures'
# The production core is copied byte-for-byte so its quoted key include resolves to a temporary
# test-only key. Neither the source tree's release key nor the signature verifier is modified.
(work / 'update_key.h').write_text('#define VITAGRAM_UPDATE_PUBLIC_KEY ' + json.dumps((work / 'pub.pem').read_text()) + '\n')
def sfo(title_id):
    fields = {'CATEGORY':'gd', 'TITLE':'VitaGram', 'TITLE_ID':title_id}
    entries = bytearray(); keys = bytearray(); values = bytearray()
    for key, value in fields.items():
        data = value.encode() + b'\0'
        entries += struct.pack('<HHIII', len(keys), 0x204, len(data), len(data), len(values))
        keys += key.encode() + b'\0'; values += data
    k = 20 + len(entries)
    return struct.pack('<IIIII', 0x46535000, 0x101, k, k + len(keys), len(fields)) + entries + keys + values
for name, title in [('good.vpk','VITAGRM01'), ('helper.vpk','VGUPDATE1')]:
    with zipfile.ZipFile(fixtures / name, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('eboot.bin', b'E' * 256)
        z.writestr('sce_sys/param.sfo', sfo(title))
head = bytearray(0x200)
for offset, value in [(8,0x100),(0x10,0x80),(0xd0,0xc0),(0xd4,0x180),(0xe8,0x1e0)]:
    struct.pack_into('>I', head, offset, value)
(fixtures / 'head.bin').write_bytes(head)
(fixtures / 'release.json').write_text(json.dumps({'tag_name':'9.9.9','assets':[
    {'name':'vitagram.vpk','browser_download_url':'https://fixture/vitagram.vpk','size':(fixtures / 'good.vpk').stat().st_size},
    {'name':'vitagram.vpk.sig','browser_download_url':'https://fixture/vitagram.vpk.sig'}]}))
PY
{ printf 'vitagram-update-v1\n9.9.9\n'; cat "$work/fixtures/good.vpk"; } | openssl dgst -sha256 -sign "$work/key.pem" -out "$work/fixtures/good.sig"
cp "$root/src/update_core.cpp" "$work/update_core.cpp"
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -I "$root/tests/updater-stubs" -I "$root/src" -I "$root/build-modern/external/jansson/include")
"${CXX:-c++}" "${flags[@]}" -D__vita__ -c "$root/src/updater.cpp" -o "$work/updater.o"
"${CXX:-c++}" "${flags[@]}" -Dmain=updater_app_main -c "$root/src/updater_app/main.cpp" -o "$work/helper.o"
"${CXX:-c++}" "${flags[@]}" "$root/tests/updater_lifecycle_test.cpp" "$work/update_core.cpp" \
    "$root/src/vpk_package.cpp" "$work/updater.o" "$work/helper.o" -lcrypto -lz -l:libjansson.so.4 -o "$work/lifecycle"
(cd "$work/runtime" && "$work/lifecycle" "$work/fixtures")
