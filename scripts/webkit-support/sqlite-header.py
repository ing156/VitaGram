#!/usr/bin/env python3
"""Expose TDLib's prefixed SQLite ABI to WebKit's ordinary SQLite callers."""
import pathlib
import re
import sys

include = pathlib.Path(sys.argv[1])
header = (include / 'tdsqlite3.h').read_text()
declarations = re.sub(r'/\*.*?\*/', '', header, flags=re.S)
names = sorted(set(re.findall(r'\btdsqlite3\w*', declarations)))
assert {'tdsqlite3', 'tdsqlite3_open_v2', 'tdsqlite3_wal_hook'} <= set(names)
(include / 'sqlite3.h').write_text(
    '#pragma once\n#include "tdsqlite3.h"\n' + ''.join(
        f'#define {name[2:]} {name}\n' for name in names))
