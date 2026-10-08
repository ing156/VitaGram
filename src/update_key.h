#pragma once
// Public half of the release signing key (private key: ~/.vitagram-release/update-signing.pem on the
// maintainer's machine, never in Git). Replace it when rotating keys; see scripts/sign-release.sh.
#define VITAGRAM_UPDATE_PUBLIC_KEY \
    "-----BEGIN PUBLIC KEY-----\n" \
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVFVdmF4dtJFiQRdjSGcaGVeLk5jC\n" \
    "GUwcZwXHhu3pt2GjPWycovcLHCwgZllVXmhRNEKovtGQNt7Ywsj6nvPr1w==\n" \
    "-----END PUBLIC KEY-----\n"
