#!/usr/bin/env bash
# Root certificates used by the self-updater to verify github.com (packaged as app0:certs/cacert.pem).
# Mozilla's list as published by curl. Refresh it (delete assets/cacert.pem and rerun) when GitHub changes roots.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p assets
destination=assets/cacert.pem
if test -s "$destination"; then exit 0; fi
temporary=$(mktemp "${destination}.download.XXXXXX")
if curl --fail --location --retry 3 https://curl.se/ca/cacert.pem --output "$temporary" && grep -q "BEGIN CERTIFICATE" "$temporary"; then
    mv -- "$temporary" "$destination"
else
    rm -f -- "$temporary"
    exit 1
fi
