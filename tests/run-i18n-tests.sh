#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/vitagram-i18n-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -I "$root/src" -DVITAGRAM_LANG_DIR='"'"$root/assets/lang"'"' \
  "$root/src/i18n.cpp" "$root/tests/i18n_test.cpp" -o "$build_dir/i18n_test"
"$build_dir/i18n_test"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I "$root/tests/net-stubs" -I "$root/src" \
  "$root/src/i18n.cpp" "$root/src/vita_net.cpp" "$root/tests/network_language_test.cpp" -o "$build_dir/network_language"
"$build_dir/network_language" "$root/assets/lang"
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
    for key in re.findall(r'\b(?:tr|i18n::text)\(\s*"([^"]+)"', open(path, encoding="utf-8").read()):
        used.add(key)
        if key not in catalog:
            missing.append((path.rsplit("/", 1)[1], key))
if missing:
    sys.exit("Keys missing from assets/lang/en.lang: %s" % missing)
print("i18n: %d source keys present in en.lang: PASS" % len(used))
PY
# Every character of every catalog must exist in the packaged fonts (needs the fontTools venv from shrink-fonts.sh).
if [[ -x "$root/external/font-tools-venv/bin/python" && -s "$root/assets/NotoSansKR-Subset.otf" ]]; then
  "$root/external/font-tools-venv/bin/python" -I - "$root" <<'PY'
import glob, os, sys
from fontTools.ttLib import TTFont
root = sys.argv[1] + "/assets/"
cmap = lambda name: set(TTFont(root + name).getBestCmap())
common = set().union(*[cmap(f) for f in ("NotoSans-Regular.ttf", "NotoSansCoptic-Regular.ttf", "NotoSansJP-JIS.otf",
                                         "NotoSansMath-Regular.ttf", "NotoSansSymbols2-Regular.ttf")])
extra = {"ko": "NotoSansKR-Subset.otf", "zh-Hans": "NotoSansSC-Subset.otf", "zh-Hant": "NotoSansTC-Subset.otf"}
bad = []
for path in sorted(glob.glob(root + "lang/*.lang")):
    code = os.path.basename(path)[:-5]
    chars = common | (cmap(extra[code]) if code in extra else set())
    text = "".join(l.partition(" = ")[2] for l in open(path, encoding="utf-8") if not l.startswith("#"))
    bad += [(code, c) for c in sorted(set(text)) if ord(c) > 0x7F and ord(c) not in chars and not 0xE000 <= ord(c) <= 0xE006]
if bad:
    sys.exit("Characters missing from the packaged fonts: %s" % bad)
print("i18n: all catalog characters are covered by the packaged fonts: PASS")
PY
fi
