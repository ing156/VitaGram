#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-i18n-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/src" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' \
  "$root/src/i18n.cpp" "$root/tests/i18n_test.cpp" -o "$build_dir/i18n_test"
"$build_dir/i18n_test"
# Every literal key used by the sources must exist in the English catalog.
python3 -I - "$root" <<'PY'
import glob, re, sys
root = sys.argv[1]
catalog = {line.split(" = ", 1)[0] for line in open(root + "/assets/lang/en.lang", encoding="utf-8")
           if " = " in line and not line.startswith(("#", "@"))}
used, missing = set(), []
for path in glob.glob(root + "/src/*.cpp") + glob.glob(root + "/src/*.h"):
    if path.endswith(("i18n.cpp", "i18n.h")):
        continue
    for key in re.findall(r'\btr\(\s*"([^"]+)"', open(path, encoding="utf-8").read()):
        used.add(key)
        if key not in catalog:
            missing.append((path.rsplit("/", 1)[1], key))
if missing:
    sys.exit("Keys missing from assets/lang/en.lang: %s" % missing)
print("i18n: %d source keys present in en.lang: PASS" % len(used))
PY
