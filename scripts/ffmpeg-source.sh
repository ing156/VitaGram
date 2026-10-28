# Sourced by build-ffmpeg-vita.sh and build-native-ffmpeg.sh: fetches the pinned FFmpeg release and
# prepares a pristine, Vita-patched source tree in "$1" (removed and re-extracted on every call).
#
# FFmpeg 7.1 'Péter' is supported at least until September 2027. The SHA-256 below was pinned after
# checking ffmpeg-$ffmpeg_version.tar.xz.asc against the FFmpeg release signing key
# FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8 (https://ffmpeg.org/ffmpeg-devel.asc).
ffmpeg_version=7.1.5
ffmpeg_sha256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

prepare_ffmpeg_source() {
    local destination="$1"
    local cache="$project_dir/external/ffmpeg-src"
    local archive="$cache/ffmpeg-$ffmpeg_version.tar.xz"
    mkdir -p "$cache"
    if ! test -f "$archive"; then
        local download
        download=$(mktemp "$cache/ffmpeg-download.XXXXXX")
        curl --fail --location --retry 3 "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" --output "$download"
        mv -- "$download" "$archive"
    fi
    echo "$ffmpeg_sha256  $archive" | sha256sum --check --quiet
    rm -rf -- "$destination"
    mkdir -p "$destination"
    tar -xJf "$archive" -C "$destination" --strip-components=1
    # The Vita H.264 hardware decoder (from fish47/FFmpeg-vita, ported to 7.1) and VitaGram's
    # portrait-video diagnostics. Applied to the verified release only.
    patch --directory="$destination" --strip=1 --forward --silent --input="$project_dir/scripts/ffmpeg-vita-7.1.patch"
    patch --directory="$destination" --strip=1 --forward --silent --input="$project_dir/scripts/ffmpeg-vita-portrait-diagnostics.patch"
    patch --directory="$destination" --strip=1 --forward --silent --input="$project_dir/scripts/ffmpeg-vita-drain-pictures.patch"
}
