#!/usr/bin/env bash
# Host tests for the self-updater core: versions, release parsing, HTTP framing, signature, zip extraction.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-update-tests.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/src" -I "$root/external/jansson/src" \
  -I "$root/build-modern/external/jansson/include" \
  "$root/src/update_core.cpp" "$root/tests/update_test.cpp" -lcrypto -lz -l:libjansson.so.4 -o "$work/update_test"
python3 -I - "$work" <<'PY'
import sys, zipfile
d = sys.argv[1]
with zipfile.ZipFile(d + "/good.zip", "w") as z:
    z.writestr("eboot.bin", "EBOOT", zipfile.ZIP_STORED)
    z.writestr("sce_sys/param.sfo", "p" * 100000, zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo("lang/"), "")
    z.writestr("lang/en.lang", "stored", zipfile.ZIP_STORED)
with zipfile.ZipFile(d + "/slip.zip", "w") as z:
    z.writestr("../evil", "x")
# Claims one uncompressed byte (local and central headers) but inflates to 1 MiB.
import struct
with zipfile.ZipFile(d + "/underreported.zip", "w", compression=zipfile.ZIP_DEFLATED) as z:
    z.writestr("blob", b"A" * (1024 * 1024))
u = bytearray(open(d + "/underreported.zip", "rb").read())
struct.pack_into("<I", u, 22, 1)
struct.pack_into("<I", u, u.index(b"PK\x01\x02") + 24, 1)
open(d + "/underreported.zip", "wb").write(u)
data = open(d + "/good.zip", "rb").read()
open(d + "/corrupt.zip", "wb").write(data[:-30] + b"\0" * 30)
PY
cp "$root/assets/cacert.pem" "$work/cacert.pem"
openssl ecparam -name prime256v1 -genkey -noout -out "$work/key.pem" 2>/dev/null
openssl ec -in "$work/key.pem" -pubout -out "$work/pub.pem" 2>/dev/null
{ printf 'vitagram-update-v1\n9.9.9\n'; cat "$work/good.zip"; } | openssl dgst -sha256 -sign "$work/key.pem" -out "$work/good.sig"
"$work/update_test" "$work"
