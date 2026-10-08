#!/usr/bin/env bash
# Signs a release VPK for the in-app updater.
#   scripts/sign-release.sh build-modern/vitagram.vpk 0.36.0 [private-key.pem]
# Upload BOTH files to the GitHub release tagged v<version>:  <name>.vpk  and  <name>.vpk.sig
# The key defaults to ~/.vitagram-release/update-signing.pem (its public half is src/update_key.h).
set -euo pipefail
if [[ $# -lt 2 ]]; then echo "usage: $0 <file.vpk> <version> [private-key.pem]" >&2; exit 2; fi
vpk=$1
version=${2#v}
key=${3:-$HOME/.vitagram-release/update-signing.pem}
[[ -f $vpk ]] || { echo "no such file: $vpk" >&2; exit 1; }
[[ -f $key ]] || { echo "signing key not found: $key" >&2; exit 1; }
[[ $version =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || { echo "version must look like 0.36.0" >&2; exit 2; }
# The signed message is "vitagram-update-v1\n<version>\n" + VPK bytes (see src/update_core.cpp).
{ printf 'vitagram-update-v1\n%s\n' "$version"; cat -- "$vpk"; } | openssl dgst -sha256 -sign "$key" -out "$vpk.sig"
# Self-check against the public half before anything is uploaded.
pub=$(mktemp); trap 'rm -f "$pub"' EXIT
openssl pkey -in "$key" -pubout -out "$pub"
{ printf 'vitagram-update-v1\n%s\n' "$version"; cat -- "$vpk"; } | openssl dgst -sha256 -verify "$pub" -signature "$vpk.sig" >/dev/null
echo "Signed: $vpk.sig (version $version)"
