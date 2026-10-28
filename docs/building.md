# Build and development / Сборка и разработка

[← VitaGram](../README.md)

Build on Linux or WSL with VitaSDK and its ARM toolchain, CMake (3.16+), Make,
Git, curl, archive tools, a host C/C++ compiler, `gperf` and Python 3.
Vita libraries: vita2d, zlib, libpng, libjpeg, libwebp, FreeType and bzip2.

Сборка выполняется в Linux или WSL. Нужны VitaSDK с ARM toolchain, CMake (3.16+),
Make, Git, curl, средства распаковки, компилятор C/C++ для ПК, `gperf` и Python 3.
Библиотеки для Vita: vita2d, zlib, libpng, libjpeg, libwebp, FreeType и bzip2.

### 1. Checkout and API credentials / Исходники и реквизиты API

Clone with submodules, or initialize them in an existing checkout. TDLib, Jansson
and rlottie are pinned to tested revisions. Build scripts apply the Vita patches;
preserve local dependency changes before updating them.

Клонируйте с подмодулями или инициализируйте их в существующей копии. TDLib,
Jansson и rlottie закреплены на проверенных коммитах; Vita-патчи накладываются
скриптами сборки. Перед обновлением зависимостей сохраните локальные изменения.

```bash
git clone --recurse-submodules https://github.com/ing156/VitaGram.git
cd VitaGram
# For an existing checkout / Для существующей копии:
git submodule update --init
cp -n src/config.local.h.example src/config.local.h
```

Get your own `VITAGRAM_API_ID` and `VITAGRAM_API_HASH` from API development tools
at my.telegram.org and fill them in `src/config.local.h`. `src/config.h` includes
that file; `.gitignore` excludes it. Keep the placeholder example in Git.
Removing credentials from current files does not remove them from Git history.

Получите свои `VITAGRAM_API_ID` и `VITAGRAM_API_HASH` в API development tools
на my.telegram.org и заполните `src/config.local.h`. Он подключается из
`src/config.h` и исключён через `.gitignore`; в Git остаётся шаблон с заглушками.
Удаление реквизитов из текущих файлов не удаляет их из истории Git.

### 2. Prepare dependencies / Подготовка зависимостей

Run from the project root / Выполните из корня проекта:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

bash scripts/fetch-emoji-font.sh
bash scripts/fetch-fallback-fonts.sh
bash scripts/fetch-ca-bundle.sh
bash scripts/build-openssl.sh
bash scripts/prepare-tdlib.sh
bash scripts/shrink-fonts.sh
bash scripts/build-ffmpeg-vita.sh
bash scripts/build-sticker-renderer.sh
```

| Component / Компонент | Install directory / Каталог установки |
| --- | --- |
| OpenSSL 3.5.9 | `external/openssl-vita/install` |
| FFmpeg 7.1.5 | `external/ffmpeg-vita71/install` |
| rlottie | `external/rlottie-vita/install` |
| Font tools / Инструменты шрифтов | `external/font-tools-venv` |

OpenSSL and FFmpeg scripts verify pinned archive hashes and apply Vita patches
without replacing SDK libraries. `prepare-tdlib.sh` builds host generators and
writes TL/MIME sources. rlottie enables TGS animations; without it, sticker
support is reduced. `shrink-fonts.sh` creates smaller packaged font copies and
keeps the originals.

Скрипты OpenSSL и FFmpeg сверяют закреплённые хэши архивов и накладывают Vita-патчи,
не заменяя библиотеки SDK. `prepare-tdlib.sh` собирает генераторы для ПК и создаёт
исходники TL/MIME. rlottie нужен для TGS-анимаций; без него поддержка стикеров
ограничена. `shrink-fonts.sh` создаёт уменьшенные копии шрифтов, сохраняя оригиналы.

Jansson is built during CMake configuration. Fonts, CA certificates, UI assets and
license notices are packaged in the VPK. Rerun TDLib preparation when its submodule
changes and CMake configuration after adding rlottie or reduced fonts.

Jansson собирается при конфигурации CMake. Шрифты, сертификаты, оформление
и лицензии входят в VPK. При смене коммита TDLib повторите подготовку генераторов;
после добавления rlottie или уменьшенных шрифтов — конфигурацию CMake.

The audio player requires FFmpeg’s `avfilter` library with `atempo`, `abuffer`,
`abuffersink` and `aresample`. After updating from a build without filters, rerun
`bash scripts/build-ffmpeg-vita.sh` before building the client.
Для новой аудиопанели нужны фильтры FFmpeg; после обновления старой сборки
зависимости повторите `bash scripts/build-ffmpeg-vita.sh`.

### 3. Configure and build / Конфигурация и сборка

```bash
cmake -S . -B build-modern \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DTD_ENABLE_TESTS=OFF

cmake --build build-modern --parallel "$(nproc)"
```

**Output / Результат: `build-modern/vitagram.vpk`.**
For ordinary client changes, only the build command is needed.
После обычных изменений клиента достаточно команды сборки.

<details>
<summary>Troubleshooting / Типичные ошибки</summary>

- **Compatibility with CMake &lt; 3.5 has been removed:** configure with
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` / передайте этот параметр при конфигурации.
- **VitaSDK libwebp is required:** install Vita headers/libraries, rather than host
  packages / нужны заголовки и библиотеки для Vita.
- **Undefined reference to BZ2_…:** check Vita bzip2 / проверьте bzip2 для Vita.
- **Missing generated TDLib sources:** run `bash scripts/prepare-tdlib.sh` / выполните подготовку TDLib.
- **rlottie patch failure:** inspect script output and preserve local changes;
  avoid resetting dependencies blindly / проверьте вывод и сохраните локальные изменения.
- **UPDATE_APP_TO_LOGIN:** use the prepared modern TDLib / используйте подготовленную современную TDLib.

</details>

### Checks / Проверки

Native regression commands and their requirements are listed in
[tests/README.md](../tests/README.md). Hardware tests must separately cover login,
sending, media playback, cameras, microphone, UI and update installation.

Команды проверок на ПК и зависимости описаны в [tests/README.md](../tests/README.md).
На PS Vita отдельно проверьте вход, отправку, воспроизведение, камеры, микрофон,
интерфейс и установку обновления.

Release signing and publication instructions: [RELEASING.md](../RELEASING.md).
Инструкции подписи и публикации релиза: [RELEASING.md](../RELEASING.md).

### Storage latency probe / Проверка задержек накопителей

Create an empty `ux0:data/vitagram/storage-test.request` and leave VitaGram idle
without playback or uploads. Results are written to
`ux0:data/vitagram/storage-test.log` for `ux0:`, `ur0:` and `uma0:`.
The test uses up to 8 MiB of temporary data per mount and preserves a 512 MiB
free-space reserve. It does not change media storage locations.

Создайте пустой `ux0:data/vitagram/storage-test.request` и оставьте VitaGram без
воспроизведения и отправки файлов. Результаты для `ux0:`, `ur0:` и `uma0:` появятся
в `ux0:data/vitagram/storage-test.log`. Тест использует до 8 МиБ временных данных
на разделе и сохраняет резерв 512 МиБ свободного места. Расположение медиакэша
не меняется. Подробности и ограничения — в [проверках](../tests/README.md).

### Contributing / Участие в разработке

Translations live in `assets/lang/<code>.lang`; start from `en.lang`. Preserve
`{cross}`, `{circle}`, other button tokens and `{0}`, `{1}` placeholders.
New keys must be added to every catalog; run `bash tests/run-i18n-tests.sh`.
English and Russian are reference catalogs. Portuguese (Portugal) and Traditional
Chinese translations would benefit from native review.

Переводы находятся в `assets/lang/<код>.lang`; за основу берите `en.lang`.
Сохраняйте токены кнопок и подстановки. Новые ключи добавляйте во все каталоги,
проверяйте через `bash tests/run-i18n-tests.sh`. Английский и русский — основные
каталоги. Португальский (Португалия) и традиционный китайский требуют проверки носителями.

The main UI is in `src/main.cpp`, Telegram integration in `src/telegram_client.*`,
media playback in `src/animation_player.*` and `src/voice_player.*`, caching in
`src/ui_cache.*` and `src/media_cache.*`, encryption in `src/vault*`, and localization
in `src/i18n.*`. UI assets live in `assets/VitaGram_UI_Assets_v1`; only the active
theme is loaded. `-DVITAGRAM_UI_ALT_BACKGROUND=ON` selects the stronger wave background.
For offline recoloring from JSON catalogues, see the
[asset template and compositions](../assets/VitaGram_UI_Assets_v1/README_RU.md#шаблон-и-композиции-из-json).
Light/Dark now share alpha masks: `ux0:data/vitagram/palette.json` changes their
colors while the app runs. Settings → Interface → Theme offers seven built-in themes:
Light, Dark, VHBB, Little Greene Light/Dark, black-blue and black-red.
A localized “Changing theme…” screen stays visible while switching.
The dropdown shows five-color previews; browsing does not recolor the UI.
Light/Dark preset switches reuse loaded masks and textures; VHBB retains its artwork.
Palette Studio includes Open Color (MIT) and Little Greene, and imports local
catalogue JSON files such as `assets/little-greene.json`. Imported catalogues stay
in the browser session; export the resulting theme to install it on Vita.
Little Greene themes use materials from @ElvisSuperStar007.
See the [source record](../tools/palette-editor/CATALOG-SOURCES.md).

Основной интерфейс — `src/main.cpp`, Telegram — `src/telegram_client.*`, плееры —
`src/animation_player.*` и `src/voice_player.*`, кэши — `src/ui_cache.*` и
`src/media_cache.*`, шифрование — `src/vault*`, локализация — `src/i18n.*`.
Оформление находится в `assets/VitaGram_UI_Assets_v1`; загружается активная тема.
Флаг `-DVITAGRAM_UI_ALT_BACKGROUND=ON` включает более контрастные волнообразные обои.
Перекраска шаблона из JSON-каталогов на ПК описана в разделе
[«Шаблон и композиции»](../assets/VitaGram_UI_Assets_v1/README_RU.md#шаблон-и-композиции-из-json).
Светлая и тёмная темы используют общие альфа-маски: файл
`ux0:data/vitagram/palette.json` меняет цвета во время работы приложения.
В «Настройки → Интерфейс → Тема» доступны семь встроенных тем: Светлая, Тёмная, VHBB,
Little Greene / Светлая и Тёмная, black-blue и black-red.
При переключении показывается «Идёт смена темы…». Выпадающий список показывает
образцы из пяти цветов; просмотр вариантов не меняет оформление.
Светлые и тёмные палитры используют загруженные маски и текстуры; VHBB сохраняет свою графику.
Мастер содержит Open Color (MIT) и Little Greene и позволяет загрузить локальный
JSON-каталог, например `assets/little-greene.json`. Каталог остаётся в сессии
браузера; для Vita экспортируйте полученную тему.
Темы Little Greene — использованы материалы @ElvisSuperStar007.
Подробности — в [записи об источниках](../tools/palette-editor/CATALOG-SOURCES.md).

<details>
<summary>Encryption and media implementation / Реализация шифрования и медиа</summary>

**English**

- The vault derives keys with scrypt (N=2¹⁴, r=8, p=2), binds them to OpenPSID with
  HKDF-SHA256, wraps a random 256-bit master key and seals files with
  ChaCha20-Poly1305 in authenticated 64 KiB chunks. TDLib uses a separate database
  subkey. Vault state is authenticated with HMAC-SHA256. Tests cover RFC vectors
  and comparison with OpenSSL; see `tests/run-vault-tests.sh`.
- Screen locking retains keys for background work. Files decrypt in memory for
  playback; completed downloads are sealed by the background worker. Plaintext
  migration failures are reported rather than treated as completed encryption.
- TGS input is bounded and checked before rlottie parsing; rlottie's embedded
  image loader is disabled. Proxy DNS-over-HTTPS verifies the packaged CA bundle;
  ordinary DNS remains a fallback. The updater verifies TLS certificates as well
  as the package signature.
- Video notes target 240×240 at 30 FPS, H.264 + mono AAC in MP4. Encoding follows
  recording; one minute can require about 157 MB of temporary camera/audio data.
  These targets and performance need physical-device testing.

**Русский**

- Хранилище использует scrypt (N=2¹⁴, r=8, p=2), привязку к OpenPSID через
  HKDF-SHA256, случайный мастер-ключ 256 бит и ChaCha20-Poly1305 с аутентифицированными
  блоками по 64 КиБ. У базы TDLib отдельный подключ; состояние подписано HMAC-SHA256.
  Проверки по RFC и OpenSSL — в `tests/run-vault-tests.sh`.
- Блокировка экрана сохраняет ключи для фоновой работы. Файлы расшифровываются
  в памяти; завершённые загрузки шифруются фоновым потоком. Ошибки преобразования
  показываются пользователю и не считаются завершённым шифрованием.
- TGS проверяются до разбора в rlottie; встроенный загрузчик картинок отключён.
  DNS-over-HTTPS для прокси проверяет сертификаты по упакованному CA bundle,
  обычный DNS остаётся запасным вариантом. Обновление проверяет TLS и подпись пакета.
- Видеокружки: целевые 240×240, 30 FPS, H.264 и моно AAC в MP4. Кодирование выполняется
  после записи; минута может требовать около 157 МБ временных кадров и звука.
  Частоту кадров и производительность нужно проверять на устройстве.

</details>
