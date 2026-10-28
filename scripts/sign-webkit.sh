#!/usr/bin/env bash
# Sign the optional engine with the existing release key, using its own signature domain.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then echo "usage: $0 <vitagram-webkit-abi1-r1.zip> [private-key.pem]" >&2; exit 2; fi
archive=$1
key=${2:-$HOME/.vitagram-release/update-signing.pem}
[[ -f $archive && -f $key ]] || { echo "archive or signing key missing" >&2; exit 1; }
{ printf 'vitagram-webkit-v1\nwebkit-abi1-r1\n'; cat -- "$archive"; } | openssl dgst -sha256 -sign "$key" -out "$archive.sig"
pub=$(mktemp)
trap 'rm -f "$pub"' EXIT
openssl pkey -in "$key" -pubout -out "$pub"
{ printf 'vitagram-webkit-v1\nwebkit-abi1-r1\n'; cat -- "$archive"; } | openssl dgst -sha256 -verify "$pub" -signature "$archive.sig" >/dev/null
echo "Signed: $archive.sig (webkit-abi1-r1)"
