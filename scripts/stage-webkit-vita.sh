#!/usr/bin/env bash
set -euo pipefail
# Flatten WebKit's thin archives: a staged prefix must not depend on temporary objects.
if [[ $# != 4 ]]; then
  echo "Usage: $0 WEBKIT_BUILD DEPS_PREFIX ICU_PREFIX DESTINATION" >&2
  exit 2
fi
build=$(realpath "$1")
deps=$(realpath "$2")
icu=$(realpath "$3")
mkdir -p "$4"
prefix=$(realpath "$4")
case "$build$deps$icu$prefix" in *[$' \t\r\n']*) echo "MRI archive staging requires paths without whitespace" >&2; exit 2;; esac
: "${VITASDK:=/usr/local/vitasdk}"
mkdir -p "$prefix/lib" "$prefix/include" "$prefix/bin"
cp "$(dirname "$build")/vita-elf-create" "$prefix/bin/"
for name in WebKit WebCore JavaScriptCore PAL WTF Skia; do
  test -s "$build/lib/lib$name.a"
  "$VITASDK/bin/arm-vita-eabi-ar" -M <<MRI
create $prefix/lib/lib$name.a
addlib $build/lib/lib$name.a
save
end
MRI
done
for name in wpe-1.0 curl xml2 psl harfbuzz; do cp "$deps/lib/lib$name.a" "$prefix/lib/"; done
for name in icui18n icuuc icudata; do cp "$icu/lib/lib$name.a" "$prefix/lib/"; done
cp -RL "$build/WebKit/Headers/WebKit" "$prefix/include/"
cp -RL "$build/JavaScriptCore/Headers/JavaScriptCore" "$prefix/include/"
cp -R "$deps/include/wpe-1.0" "$deps/include/EGL" "$deps/include/KHR" "$prefix/include/"
python3 - "$build" "$prefix" <<'PY'
import pathlib, shutil, sys
work = pathlib.Path(sys.argv[1]).parent
destination = pathlib.Path(sys.argv[2]) / 'share/licenses'
sources = {
    'WebCore': work / 'source/Source/WebCore',
    'JavaScriptCore': work / 'source/Source/JavaScriptCore',
    'Skia': work / 'source/Source/ThirdParty/skia',
    'ICU': work / 'port/third_party/icu-vita-full/icu',
    'Vita-port': work / 'port',
}
sources.update((name, work / name) for name in ('harfbuzz', 'libxml2', 'libpsl', 'libwpe', 'curl'))
for name, source in sources.items():
    target = destination / name
    target.mkdir(parents=True, exist_ok=True)
    found = False
    for path in source.iterdir():
        if not path.name.lower().startswith(('copying', 'copyright', 'license', 'notice', 'third_party_notices')):
            continue
        if path.is_file(): shutil.copy2(path, target / path.name)
        elif path.is_dir(): shutil.copytree(path, target / path.name, dirs_exist_ok=True)
        found = True
    if not found: raise SystemExit(f'Missing license notices: {source}')
PY
echo "Staged experimental Vita WebKit: $prefix"
