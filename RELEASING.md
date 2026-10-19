# Releasing VitaGram / Выпуск VitaGram

[README](README.md) · [Build / Сборка](README.md#building) · [Native checks / Проверки на ПК](tests/README.md)

Run the relevant native checks and validate installation and updates on a physical
PS Vita before publishing. Build instructions are in the README.

Перед публикацией выполните нужные проверки на ПК и проверьте установку
и обновление на физической PS Vita. Инструкции сборки находятся в README.

## Versioning / Версии

Keep `VITAGRAM_VERSION` and `VITA_VERSION` in `CMakeLists.txt` consistent.
Documentation-only changes and rebuilds without code changes keep
the version. For a new build with code changes, increment the final component
unless explicitly instructed otherwise; see [AGENTS.md](AGENTS.md).

Согласуйте `VITAGRAM_VERSION` и `VITA_VERSION` в `CMakeLists.txt`.
Правки документации и повторная
сборка без изменений кода не повышают версию. Для новой сборки с изменениями кода
увеличьте последний компонент, если нет другого явного указания; см. [AGENTS.md](AGENTS.md).

## Release tag and files / Тег и файлы релиза

The release title can be arbitrary. Use a numeric version tag such as `vX.Y.Z`,
without suffixes like `-beta`; the version must match `VITAGRAM_VERSION` and the
version passed to the signing script. The updater offers only versions newer
than the installed client. Publish a stable release: drafts and pre-releases
are ignored.

Название релиза может быть любым. Тег — числовая версия вида `vX.Y.Z`, без
суффиксов вроде `-beta`. Версия должна совпадать с `VITAGRAM_VERSION` и аргументом
скрипта подписи. Обновление предлагается только для версии новее установленной.
Публикуйте обычный релиз: черновики и предварительные релизы пропускаются.

The VPK filename can vary, but must end in lowercase `.vpk`. Its signature
must have exactly the same filename plus `.sig`, for example `vitagram.vpk`
and `vitagram.vpk.sig`. Attach both files to the same release. If several signed
VPK pairs are attached, the updater selects the first matching pair; attach
only one signed application package.

Имя VPK может быть любым, но расширение должно быть `.vpk` в нижнем регистре.
Имя подписи — точно такое же, с добавлением `.sig`: например, `vitagram.vpk`
и `vitagram.vpk.sig`. Приложите оба файла к одному релизу. При нескольких
подписанных парах выбирается первая найденная; публикуйте одну подписанную
пару основного приложения.

## Signing and publishing / Подпись и публикация

Set `release_version` to the release's `VITAGRAM_VERSION` from `CMakeLists.txt`,
then sign the package.
Укажите в `release_version` значение `VITAGRAM_VERSION` выпуска из `CMakeLists.txt`
и подпишите пакет:

```bash
release_version="X.Y.Z"
bash scripts/sign-release.sh build-modern/vitagram.vpk "$release_version"
```

Publish a **public** GitHub release in `ing156/VitaGram` tagged `v<release_version>` with `vitagram.vpk` and
`vitagram.vpk.sig`. The private key defaults to
`~/.vitagram-release/update-signing.pem`; keep it out of Git and retain a backup.
Replacing that key requires manually distributing a client with the new public key.
The updater has no GitHub token and cannot access private releases.

Создайте **публичный** релиз в `ing156/VitaGram` с тегом `v<release_version>` и приложите `vitagram.vpk`
и `vitagram.vpk.sig`. Закрытый ключ по умолчанию:
`~/.vitagram-release/update-signing.pem`; не добавляйте его в Git и сохраните копию.
Смена ключа требует ручного распространения клиента с новым открытым ключом.
У приложения нет токена GitHub для доступа к приватным релизам.

## Updater build details / Сборка вспомогательного установщика

The helper **VitaGram Updater** (`VGUPDATE1`) is bundled under `app0:updater/`.
Automatic helper installation needs `assets/updater-head.bin`, obtained from
`ux0:app/VGUPDATE1/sce_sys/package/head.bin` after a one-time VitaShell installation.
If automatic installation fails, the client exports `vitagram-updater.vpk` into
its update directory for manual installation. The helper installs the verified
package with `ScePromoterUtil` and relaunches VitaGram; the updated client removes
the helper after a successful update.

**VitaGram Updater** (`VGUPDATE1`) входит в `app0:updater/`. Для его автоматической
установки нужен `assets/updater-head.bin`, полученный из
`ux0:app/VGUPDATE1/sce_sys/package/head.bin` после однократной установки через
VitaShell. При ошибке клиент копирует `vitagram-updater.vpk` в папку обновления
для ручной установки. Установщик использует `ScePromoterUtil`, запускает VitaGram
и удаляется клиентом после успешного обновления.

The signature covers the version and VPK contents and is checked at download
and again before installation. Package staging prefers `ux0:data/vitagram/update`
and falls back to `ux0:downloads/vitagram-update`; disk checks reserve roughly
2× package size + 16 MiB. A verified VPK is retained for VitaShell if installation fails.

Подпись покрывает версию и содержимое VPK, проверяется после загрузки и перед
установкой. Основная папка — `ux0:data/vitagram/update`, запасная —
`ux0:downloads/vitagram-update`; требуется примерно два размера пакета + 16 МиБ.
При ошибке проверенный VPK остаётся доступен для установки через VitaShell.


## License notices / Лицензионные уведомления

The documentation review found gaps in the VPK license-file recipe. Before
publishing, include the applicable notices for the exact SDK libraries: FreeType,
libwebp, libjpeg-turbo, bzip2, zlib, libpng and the C/C++ runtime, plus the bundled
SQLCipher BSD notice and Mozilla CA bundle MPL-2.0 attribution/source notice.
Some license terms are embedded in headers or supplied by upstream rather than
installed as separate SDK license files. The README credits do not replace
required license texts. Check the actual packaged files, not only the About screen.

For statically linked FFmpeg, retain the corresponding patched library source,
build instructions and materials allowing users to relink with a modified library
(such as application object files), in accordance with
[LGPL-2.1 and FFmpeg's distribution guidance](https://ffmpeg.org/legal.html).
Retain the source for rlottie's MPL-covered files and the CA bundle source notice.
Preserve VHBB attribution, change notices and CC BY-NC-SA terms. Verify the origin
and redistribution terms of any artwork without an explicit license notice.

Проверка документации выявила пробелы в составе лицензионных файлов VPK.
Перед публикацией добавьте уведомления для используемых версий FreeType, libwebp,
libjpeg-turbo, bzip2, zlib, libpng и среды исполнения C/C++, а также BSD-уведомление
SQLCipher и сведения о MPL-2.0 и исходнике CA bundle Mozilla. Часть условий есть
в заголовках или upstream, но не установлена отдельными файлами SDK. Благодарности
в README не заменяют обязательные тексты. Проверьте состав пакета, а не только «О проекте».

При статической линковке FFmpeg сохраните соответствующие исходники с патчами,
инструкции сборки и материалы для перелинковки с изменённой библиотекой
(например, объектные файлы приложения), согласно
[LGPL-2.1 и рекомендациям FFmpeg](https://ffmpeg.org/legal.html).
Сохраните исходники MPL-компонентов rlottie и сведения об исходнике CA bundle.
Не удаляйте авторство, описание изменений и условия CC BY-NC-SA ресурсов VHBB.
Для графики без явной лицензии проверьте происхождение и условия распространения.

## Palette Studio web/offline edition / Веб- и автономный мастер

The Pages workflow publishes the standalone editor from `master`; enable
**Settings → Pages → Source → GitHub Actions** before the first deployment.
See [Palette Studio instructions](tools/palette-editor/README.md#публикация-на-github-pages).
A pull request runs the editor checks without publishing.

Workflow Pages публикует мастер из `master`. Перед первым запуском выберите
**Settings → Pages → Source → GitHub Actions**. Сборка и проверка pull request
не публикуют сайт. Ссылка в README обозначает ожидаемый адрес; считайте её
действующей только после успешного deployment.

For an offline release asset / Для автономного файла релиза:

```bash
python3 scripts/build-palette-editor.py build-modern/palette-site/index.html
node tests/palette_editor_test.js build-modern/palette-site/index.html
cp build-modern/palette-site/index.html build-modern/palette-site/VitaGram-Palette-Studio.html
```

Attach `VitaGram-Palette-Studio.html` alongside the signed application package.
The HTML contains its own fonts, masks, MIT license and font-license notice. Do not attach
local logs, crash dumps, API configuration, account data or signing keys.

Приложите `VitaGram-Palette-Studio.html` к подписанному VPK. В HTML встроены
маски, шрифты, лицензия MIT и уведомление о лицензии шрифта. Не включайте локальные журналы,
дампы, API-конфигурацию, данные аккаунта и ключ подписи.

### Local release preparation / Локальная подготовка выпуска

The prepared notes for the current release are in
[releases/v0.38.07.md](releases/v0.38.07.md). The local bundle is
`release/v0.38.07/`; it is intentionally ignored by Git. Use its `RELEASE.md`
as the GitHub release body and attach `vitagram.vpk`, `vitagram.vpk.sig`,
`VitaGram-Palette-Studio.html` and `SHA256SUMS`. The ZIP is a convenience copy
of the complete bundle, not an additional updater package.

Описание текущего выпуска: [releases/v0.38.07.md](releases/v0.38.07.md).
Локальный комплект: `release/v0.38.07/`; он намеренно не добавляется в Git.
В нём лежат VPK, подпись, автономный мастер, описание, лицензия и SHA256SUMS.
ZIP рядом с каталогом — копия этого комплекта для переноса.

If the release version changes before pushing, update the CMake version fields,
README and release notes, rebuild the VPK/editor, sign with the new app version,
and regenerate SHA256SUMS. Do not reuse a signature from another version or build.
Commit the source files needed by Pages as well as the workflow; a generated HTML
alone does not make the workflow's source dependencies available in a checkout.

Если перед push меняете номер выпуска, согласуйте версии в CMake, README
и описании релиза; пересоберите VPK и мастер, подпишите новой версией приложения
и пересчитайте SHA256SUMS. Старая подпись для другого номера или VPK не подходит.
Добавьте в коммит исходники мастера, маски, палитры, тесты и workflow; локальные
неотслеживаемые файлы не попадут в GitHub Actions автоматически.

### Каталоги цветов в мастере

Текущая сборка встраивает Open Color (MIT, уведомление и полный текст лицензии
в HTML) и Little Greene. Сообщённое пользователем согласие @ElvisSuperStar007
и ссылка на закрытое сообщение зафиксированы в
[источниках и правах](tools/palette-editor/CATALOG-SOURCES.md); текст и полномочия
не проверены. Локальный архив `local-palettes/` не входит в публикацию.
Используйте заново собранные HTML/VPK: старые артефакты могут содержать
исключённые каталоги и предустановки.
