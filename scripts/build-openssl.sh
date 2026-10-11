#!/usr/bin/env bash
# Builds OpenSSL 3.5 LTS for the PS Vita into external/openssl-vita/install.
# Replaces the SDK's OpenSSL 1.0.2 (2016) for TDLib and the updater; nothing is written to the SDK.
set -euo pipefail
: "${VITASDK:?Set VITASDK to your existing VitaSDK directory}"
export PATH="$VITASDK/bin:$PATH"
command -v arm-vita-eabi-gcc >/dev/null
command -v perl >/dev/null
cd "$(dirname "$0")/.."
project_dir="$PWD"
version=3.5.9
# SHA-256 of the release tarball. Its signature (openssl-$version.tar.gz.asc) was checked against
# the OpenSSL signing key B146 647E 45A7 B339 47AB 226B 2A2C 87D1 6169 2D40 when this was pinned.
sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
root="$project_dir/external/openssl-vita"
archive="$root/openssl-$version.tar.gz"
mkdir -p "$root"
if ! test -f "$archive"; then
    download=$(mktemp "$root/openssl-download.XXXXXX")
    curl --fail --location --retry 3 \
        "https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz" --output "$download"
    mv -- "$download" "$archive"
fi
echo "$sha256  $archive" | sha256sum --check --quiet

# Always start from the pristine release so the patch applies to exactly what was verified.
source_dir="$root/openssl-$version"
rm -rf -- "$source_dir"
tar -xzf "$archive" -C "$root"
patch --directory="$source_dir" --strip=1 --forward --input="$project_dir/scripts/openssl-vita.patch"

build_dir="$root/build-$version"
rm -rf -- "$build_dir" "$root/install"
mkdir -p "$build_dir"
cd "$build_dir"
# Static libraries only. Disabled: everything VitaGram and TDLib do not use (apps, engines, QUIC,
# DTLS, CMS/TS/CMP, binary curves, legacy provider) - less code reachable from the network, and
# the subsystems behind most post-1.1.1 advisories. no-atexit: no cleanup racing TDLib threads at exit.
"$source_dir/Configure" vita-cross --config="$project_dir/scripts/openssl-vita.conf" \
    --prefix="$root/install" --libdir=lib --openssldir=/nonexistent \
    --with-rand-seed=getrandom \
    no-shared no-asm no-dso no-module no-engine no-apps no-tests no-docs no-atexit \
    no-async no-afalgeng no-ktls no-quic no-dtls no-sctp no-srtp no-ssl3 no-comp \
    no-ec2m no-legacy no-cms no-ts no-cmp no-srp no-http no-ui-console no-secure-memory \
    no-thread-pool no-default-thread-pool
make -j"$(nproc)" build_libs
make install_dev
mkdir -p "$root/install/share"
cp "$source_dir/LICENSE.txt" "$root/install/share/OpenSSL-LICENSE.txt"
printf '\nOpenSSL %s installed locally in %s\n' "$version" "$root/install"
