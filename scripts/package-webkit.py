#!/usr/bin/env python3
"""Package the optional engine; sign the result with scripts/sign-webkit.sh."""
import pathlib
import sys
import zipfile


def package(module, notices, output, openssl_notice):
    module, notices, output = map(pathlib.Path, (module, notices, output))
    if not module.is_file() or not (notices / 'WebCore/LICENSE-LGPL-2.1').is_file():
        raise ValueError('Missing engine or WebKit license notices')
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(module, 'webkit.suprx')
        archive.writestr('compatibility', 'webkit-abi1-r1\n')
        for path in sorted(notices.rglob('*')):
            if path.is_file():
                archive.write(path, 'licenses/' + path.relative_to(notices).as_posix())
        root = pathlib.Path(__file__).resolve().parents[1]
        for name, path in {
            'VitaGram': root / 'LICENSE',
            'Vita-adapter': root / 'src/webview/LICENSE',
            'Jansson': root / 'external/jansson/LICENSE',
            'SQLCipher': root / 'external/tdlib-upstream/sqlite/sqlite/LICENSE',
            'OpenSSL': pathlib.Path(openssl_notice),
        }.items():
            archive.write(path, 'licenses/' + name + '/LICENSE')


if __name__ == '__main__':
    package(*sys.argv[1:])
