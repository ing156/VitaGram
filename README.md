[English](#english) | [Русский](#russian)

<a id="english"></a>

# VitaGram — Telegram for PS Vita

An unofficial homebrew Telegram client built with VitaSDK, vita2d and TDLib.
Source version: **0.36**. VPK package version: **36.00**. This project is under development.

**0.36 is the first public release.**

Performance measurements are written in the background to `ux0:data/vitagram/performance.log`.
The Select diagnostics window has been removed; Select alone has no action.

## Features

- Sign in with a phone number, Telegram code and two-step verification password.
- Native Vita keyboard, Cyrillic text and color emoji.
- Non-archived chats, Telegram folders, incremental loading and list updates.
- Cached chat lists, history and media for faster reopening.
- Text messages, participant names, date separators, timestamps and delivery states.
- Online/last-seen status in private chats, respecting Telegram privacy settings.
- Photos, albums, expanded posts and navigation between posts.
- Centered fullscreen photos, automatic image rotation and pinch-to-zoom up to 6×.
- Static and animated stickers; send from recent, favorite and installed sticker sets.
- Standard emoji reactions, voice messages, audio and supported animations.
- Microphone recording and sending audio messages from the current chat.
- Forum groups: topic lists, separate topic history, text and sticker sending to the selected topic.
- Light/dark themes, font size, cache retention settings, interface language and account logout.
- Connection indicator, HTTP(S) links opened in the browser, and a startup screen showing initialization stages.
- Self-update from GitHub Releases (Start → *Update*; also checked automatically about a minute after launch). The release `.vpk` is verified against a built-in public key before it is installed — see the Russian section «Обновление через GitHub» below.
- Proxy support: MTProxy, SOCKS5 and HTTPS (HTTP CONNECT) — Start → Proxy; `tg://socks` / `t.me/socks` links and QR codes are imported too. Credentials are stored in plain text in `settings.json`.

A feature being present in the source does not mean every scenario has been tested on hardware.
The latest sticker and forum-topic changes still require build and PS Vita testing.

## Controls

The current bindings and proposed improvements are reviewed in [the control audit](docs/controls-analysis.md) (Russian).

### Main menu

- **![D-pad](assets/controller-icons/dpad.svg) / touch** — select and scroll; **![X](assets/controller-icons/cross.svg) / tap** — open a chat.
- **L / R** — switch folders; folder tabs also support touch and horizontal scrolling.
- **![Triangle](assets/controller-icons/triangle.svg)** — refresh the list.
- **![Start](assets/controller-icons/start.svg)** — settings.
- **Hold ![Start](assets/controller-icons/start.svg), then press ![Select](assets/controller-icons/select.svg) and hold both for 2 seconds** — exit the application.

### Chat

- **Up / Down / vertical swipe** — scroll and load older messages.
- **![X](assets/controller-icons/cross.svg) / tap a message** — open the expanded view.
- **Short press ![Square](assets/controller-icons/square.svg)** — compose text; the keyboard opens after releasing the button.
- **Hold ![Square](assets/controller-icons/square.svg) for at least 0.55 seconds** — open the sticker picker.
- **![Triangle](assets/controller-icons/triangle.svg)** — refresh history.
- **L** — open the video-note recorder.
- **R** — start/stop microphone recording; the recorded audio is sent automatically after preparation.
- **![O](assets/controller-icons/circle.svg)** — go back; inside a forum topic, return to the topic list.

### Forum topics

- The topic list opens before the conversation when TDLib identifies a group as a forum.
- **![D-pad](assets/controller-icons/dpad.svg) / touch** — select; **![X](assets/controller-icons/cross.svg) / tap** — open a topic.
- **Down near the end of the list / ![Triangle](assets/controller-icons/triangle.svg)** — load the next batch of 15 topics.
- **![O](assets/controller-icons/circle.svg)** — return to the chat list.
- Each entry shows its name, unread count and closed-topic marker.

### Expanded post

- **Up / Down** — scroll the post.
- **Left / Right / horizontal swipe** — previous or next post.
- **L / R** — select a photo within an album.
- **![X](assets/controller-icons/cross.svg)** — open the selected photo or play/pause supported media.
- **![Square](assets/controller-icons/square.svg)** — choose a reaction, rather than compose text.
- **![O](assets/controller-icons/circle.svg)** — go back.

### Fullscreen photo

- **Left / Right** — switch photos; swiping switches photos at the default zoom level.
- **Two fingers** — zoom; **one finger while zoomed in** — pan.
- **![Triangle](assets/controller-icons/triangle.svg)** — reset zoom and position; **![O](assets/controller-icons/circle.svg)** — go back.
- The image adjusts to the console's orientation. When the console lies face up, the previous orientation is retained. The header and control hints do not rotate.

### Sticker picker

- The picker opens a list of sets: **Up / Down** — select a set; **![X](assets/controller-icons/cross.svg) / tap** — open it.
- **![D-pad](assets/controller-icons/dpad.svg)** — select a sticker and move between pages.
- **![X](assets/controller-icons/cross.svg) / tap a sticker** — immediately send it to the current chat or topic.
- In a sticker grid, **![O](assets/controller-icons/circle.svg)** — return to sets; **![Square](assets/controller-icons/square.svg)** — close the picker. In the set list, **![O](assets/controller-icons/circle.svg)** — close the picker.
- Up to 18 stickers per page; only visible previews are downloaded. Animated stickers use static thumbnails in the picker.

## Languages

The interface language follows the console's system language; if no matching language file exists, English is used.
Press ![Start](assets/controller-icons/start.svg) on the main screen and change **Language** (the first row) to pick a language manually or return to *Automatic*.
The choice is stored in `settings.json` (`language`; empty means automatic).
Shipped languages: all official PS Vita system languages — English, Russian, German, French, Spanish, Italian, Dutch, Portuguese (Portugal and Brazil), Swedish, Danish, Norwegian, Finnish, Polish, Turkish, Japanese, Korean, Simplified and Traditional Chinese (British English uses the English file). The Portuguese (Portugal) and Traditional Chinese texts were adapted from the Brazilian and Simplified versions and deserve a native review.
Korean and Chinese use their own font (`assets/NotoSans{KR,SC,TC}-Regular.otf`, subsets built by `scripts/shrink-fonts.sh`), loaded only while that interface language is selected; Korean/Chinese text in chats is therefore drawn only in those languages.

All texts live in `assets/lang/<code>.lang` and are packaged as `app0:lang/`. To add or fix a language, copy `assets/lang/en.lang`, translate the values and rebuild — no code changes are needed, and the new language appears in the menu automatically.
Missing keys fall back to English. Controller buttons are written as tokens such as `{cross}`, `{circle}` or `{dpad}` and are replaced by the icons when the file is loaded, so translations change only the text, never the icons. `{0}`, `{1}`… are values inserted by the program.
New texts in code use `tr("key")` / `tr("key", {args})` from `src/i18n.h`; add the key to every `.lang` file. `bash tests/run-i18n-tests.sh` checks that all catalogs have the same keys and placeholders and that every key used in the sources exists.
Characters outside the packaged fonts (for example Arabic) need a suitable font from `assets/` first. The very first startup screen stays in English.

## Building

Build on Linux or WSL. You need VitaSDK with its ARM toolchain, CMake, Make, Git, curl and archive extraction tools.
Vita dependencies: vita2d, OpenSSL, zlib, libpng, libjpeg, libwebp, FreeType and bzip2. Jansson is built from `external/jansson`.

### TDLib compatibility

The current client targets the modern TDLib schema in the prepared `external/tdlib-upstream` source tree.
Use a **Vita-patched version**, not an unmodified upstream clone: POSIX compatibility, file locking, SQLite and build support have been adapted separately.
There is not yet a single script that reproduces all these changes against an arbitrary upstream revision.

The `external/tdlib-vita` directory is also required: CMake uses its Vita compatibility functions.
CMake defaults to the older fork; explicitly set `TDLIB_SOURCE_DIR` for the current client.
Do not replace prepared dependencies without preserving local changes.

### Preparation

Obtain your own `VITAGRAM_API_ID` and `VITAGRAM_API_HASH` from API development tools at my.telegram.org.
Do not publish personal API credentials or session data.

#### Keeping API credentials out of Git

If your checkout still defines these values directly in `src/config.h`, perform the following migration before publishing it. These instructions do not automatically change the configuration files.

1. Move the real values into `src/config.local.h`:

   ```cpp
   #pragma once
   #define VITAGRAM_API_ID 123456
   #define VITAGRAM_API_HASH "your_api_hash"
   ```

   The values above are placeholders; use your own credentials locally.

2. Replace only the two credential definitions in `src/config.h` with the following include, keeping all other settings unchanged:

   ```cpp
   #include "config.local.h"
   ```

3. Add this entry to the repository-root `.gitignore`:

   ```gitignore
   /src/config.local.h
   ```

4. Commit a public template named `src/config.local.h.example` with no real credentials:

   ```cpp
   #pragma once
   #define VITAGRAM_API_ID 0
   #define VITAGRAM_API_HASH ""
   ```

After cloning a repository configured this way, create the local file and fill in your credentials:

```bash
cp -n src/config.local.h.example src/config.local.h
```

Keep the sanitized `src/config.h`, the example file and `.gitignore` in Git. Do not add the real local file, including with `git add -f`.
If `src/config.local.h` is already tracked, `.gitignore` alone will not exclude it. Remove it from the index while keeping the local copy:

```bash
git rm --cached -- src/config.local.h
```

Before committing, review the staged changes locally with `git diff --cached` and verify the ignore rule with `git check-ignore -v src/config.local.h`. Do not share output containing credentials.
Removing secrets from the current files does **not** remove them from earlier commits. If real values were committed, clean the history before publishing; if already published, treat them as exposed. Coordinate any history rewrite with other contributors.

From the project root:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

bash scripts/fetch-emoji-font.sh
bash scripts/fetch-fallback-fonts.sh
bash scripts/fetch-ca-bundle.sh
bash scripts/shrink-fonts.sh
bash scripts/build-hardware-video-decoder.sh
bash scripts/build-sticker-renderer.sh
```

Clone the `vita` branch of `fish47/FFmpeg-vita` into `external/ffmpeg-vita` at commit `3736b0c78da9553b2098dbb0ade0a2e0ddd1753f` before running the hardware decoder build script. FFmpeg is built locally in `external/ffmpeg-hardware/install`, without replacing SDK packages.
rlottie is built in `external/rlottie-vita/install` and is needed for TGS animations. Building without it is possible, with limited sticker support.
Fonts and license notices are included in the VPK.
`shrink-fonts.sh` creates smaller packaged copies of the two largest fonts; the upstream files are not modified:

- `assets/NotoColorEmoji-64.ttf`: emoji bitmaps reduced from 136 to 64 px (the UI draws them at about 31 px at most). About 5 MB smaller in the VPK and about 53 KiB less texture memory per emoji.
- `assets/NotoSansJP-JIS.otf`: kanji limited to JIS X 0208, JIS X 0213 plane 1 and the Windows CP932 extensions (7788 characters, modern Japanese including names); kana, punctuation, full-width forms and symbols are kept. About 1.6 MB smaller. Rarer kanji and Chinese-only hanzi of the upstream font are no longer displayed.

The script installs fontTools, Pillow and imagequant into `external/font-tools-venv` (Python 3 and internet access required) and skips results that are up to date. Without these files CMake packages the full-size fonts and prints a warning. Rerun CMake configuration after creating them.

### Configure and build the VPK

```bash
cmake -S . -B build-modern \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DTDLIB_SOURCE_DIR="$PWD/external/tdlib-upstream" \
  -DTD_ENABLE_TESTS=OFF

cmake --build build-modern -j$(nproc)
```

Output: `build-modern/vitagram.vpk`.
For ordinary client changes, only the final command is needed; dependencies do not need rebuilding each time.
Rerun CMake configuration after adding rlottie.

### Common errors

- **Compatibility with CMake < 3.5 has been removed** — pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` during configuration.
- **VitaSDK libwebp is required** — install the WebP library and headers for Vita, not the host Linux system.
- **Undefined reference to BZ2_…** — check the Vita bzip2 library and ensure the main `CMakeLists.txt` is up to date.
- **photo_view.h: No such file or directory** — copy new headers along with `.cpp` files when transferring an update.
- **rlottie patch failure** — preserve local changes and inspect the build script output; do not blindly reset the dependency tree.
- **UPDATE_APP_TO_LOGIN** — the old TDLib fork may be incompatible with Telegram login; use the prepared modern TDLib.

## Cache and private data

- `ux0:data/vitagram/db` — TDLib data, session and media cache index.
- `ux0:data/vitagram/files` — downloaded media.
- `ux0:data/vitagram/settings.json` — application settings.

TDLib stores some thumbnails and stickers in subdirectories of `db`; the client handles these paths.
The media cache matches files by their persistent Telegram identifier, checks file existence and size, and saves its index across launches.
Retention and size limits are configurable in settings. Cleanup runs in the background during idle periods rather than blocking startup access to chats.

Sticker images and thumbnails are cached. Full animations are downloaded for playback.
**Sticker-set and forum-topic lists are still requested from Telegram and do not have a separate persistent menu cache.** Cached images therefore do not eliminate the wait for list responses.

The `db` directory contains private account information. Do not publish it or delete it just to clear images.

## Performance and limitations

UI, playback, response processing and background operations have different priorities. Requests for the open chat and selected media take priority over background loading.
TDLib calls run on a single networking thread because of limitations in the port; there is no separate networking core for each chat.
The additional CPU core is used only after a successful access check; installing an unlock plugin alone does not guarantee access.

- Video, video notes and some MP4 files have limited support; playback of every file is not guaranteed.
- Calls, secret chats, group administration and paid/custom reactions are not supported by this interface.
- Creating/editing topics and searching for/installing new sticker sets are not implemented yet.
- Exact presence information depends on Telegram privacy settings.

## Main source files

- `src/main.cpp` — interface, input and rendering.
- `src/telegram_client.*` — authentication, request queue, chats, topics, messages, stickers and reactions.
- `src/photo_view.h` — photo zoom, panning and orientation.
- `src/keyboard.*` — native Vita keyboard.
- `src/media_render.*` — fonts, emoji and WebP.
- `src/animation_player.*`, `src/voice_player.*` — media playback.
- `src/media_cache.*`, `src/ui_cache.*` — media and UI caches.
- `src/cpu_scheduler.*` — thread priorities and load distribution.
- `src/settings.h`, `src/vita_net.*` — settings and network state.
- `src/i18n.*`, `assets/lang/*.lang` — localization.

## Licenses

Project code is licensed under [MIT](LICENSE).
TDLib, VitaSDK, vita2d, FFmpeg, rlottie, fonts and other dependencies have their own licenses.
When distributing the application, retain notices and comply with dependency license requirements, including source availability and relinking where required.

---

<a id="russian"></a>

[English](#english) | Русский

# VitaGram — Telegram для PS Vita

Неофициальный homebrew-клиент Telegram на VitaSDK, vita2d и TDLib.
Версия исходников: **0.36**, версия пакета VPK: **36.00**. Проект находится в разработке.

**0.36 — первый публичный релиз.**

## Возможности

- Авторизация по телефону, коду Telegram и паролю двухэтапной аутентификации.
- Системная клавиатура Vita, кириллица и цветные emoji.
- Чаты без архива, папки Telegram, постепенная загрузка и обновление списка.
- Кэш чатов, истории и медиа для повторного открытия.
- Текстовые сообщения, имена участников, разделители дней, время и статусы отправки.
- Статус собеседника в личном чате с учётом скрытого времени посещения.
- Фото, альбомы, полный просмотр поста и переключение между постами.
- Центрирование полноэкранных фото, автоповорот изображения, pinch-to-zoom до 6×.
- Статичные и анимированные стикеры; отправка из недавних, избранных и установленных наборов.
- Обычные emoji-реакции, воспроизведение голосовых, аудио и поддерживаемых анимаций.
- Темы форумных групп: список топиков, отдельная история, отправка текста и стикеров в выбранную тему.
- Светлая/тёмная тема, размер шрифта, настройки хранения кэша, язык интерфейса и выход из аккаунта.
- Индикатор подключения, открытие HTTP(S)-ссылок в браузере, заставка с этапами запуска.

Наличие функции в исходниках не означает проверку всех сценариев на устройстве.
Последние изменения стикеров и топиков требуют проверки сборкой и на PS Vita.

## Управление

Окно диагностики на Select удалено; отдельное нажатие Select ничего не делает.
[Анализ текущего управления и варианты развития](docs/controls-analysis.md).

### Главное меню

- **![Стрелки](assets/controller-icons/dpad.svg) / тач** — выбор и прокрутка; **![X](assets/controller-icons/cross.svg) / тап** — открыть чат.
- **L / R** — переключить папку; закладки также поддерживают тач и горизонтальную прокрутку.
- **![Треугольник](assets/controller-icons/triangle.svg)** — обновить список.
- **![Start](assets/controller-icons/start.svg)** — настройки.
- **Зажать ![Start](assets/controller-icons/start.svg), затем нажать ![Select](assets/controller-icons/select.svg) и держать обе кнопки 2 секунды** — выход из приложения.

### Переписка

- **Вверх / вниз / вертикальный свайп** — прокрутка и загрузка более старых сообщений.
- **![X](assets/controller-icons/cross.svg) / тап по сообщению** — полный просмотр.
- **![Квадрат](assets/controller-icons/square.svg), короткое нажатие** — ввод текста; клавиатура открывается после отпускания кнопки.
- **![Квадрат](assets/controller-icons/square.svg), удержание от 0,55 секунды** — меню отправки стикеров.
- **![Треугольник](assets/controller-icons/triangle.svg)** — обновить историю.
- **L** — открыть запись видеокружка.
- **R** — начать/остановить запись голоса; после подготовки запись отправляется автоматически.
- **![O](assets/controller-icons/circle.svg)** — назад; из топика возвращает к списку тем.

### Темы форумной группы

- Список открывается перед перепиской, если TDLib определяет группу как форум.
- **![Стрелки](assets/controller-icons/dpad.svg) / тач** — выбор; **![X](assets/controller-icons/cross.svg) / тап** — открыть тему.
- **Вниз у конца списка / ![треугольник](assets/controller-icons/triangle.svg)** — загрузить следующую порцию из 15 тем.
- **![O](assets/controller-icons/circle.svg)** — вернуться к списку чатов.
- Показываются название, количество непрочитанных и отметка закрытой темы.

### Полный просмотр поста

- **Вверх / вниз** — прокрутка поста.
- **Влево / вправо / горизонтальный свайп** — предыдущий или следующий пост.
- **L / R** — выбрать фото в альбоме.
- **![X](assets/controller-icons/cross.svg)** — открыть выбранное фото либо включить/приостановить поддерживаемое медиа.
- **![Квадрат](assets/controller-icons/square.svg)** — выбор реакции, а не ввод текста.
- **![O](assets/controller-icons/circle.svg)** — назад.

### Полноэкранное фото

- **Влево / вправо** — сменить фотографию; свайп переключает фото при обычном масштабе.
- **Два пальца** — масштабирование; **один палец при увеличении** — перемещение.
- **![Треугольник](assets/controller-icons/triangle.svg)** — сброс масштаба и смещения; **![O](assets/controller-icons/circle.svg)** — назад.
- Изображение подстраивается под поворот консоли. При положении экраном вверх сохраняется предыдущая ориентация. Шапка и подсказки не поворачиваются.

### Меню стикеров

- Сначала открывается список наборов: **вверх / вниз** — выбрать набор; **![X](assets/controller-icons/cross.svg) / тап** — открыть его.
- **![Стрелки](assets/controller-icons/dpad.svg)** — выбрать стикер, перейти на следующую страницу.
- **![X](assets/controller-icons/cross.svg) / тап по стикеру** — сразу отправить в текущий чат или топик.
- В сетке **![O](assets/controller-icons/circle.svg)** — вернуться к наборам; **![квадрат](assets/controller-icons/square.svg)** — закрыть меню. В списке наборов **![O](assets/controller-icons/circle.svg)** закрывает меню.
- На странице до 18 стикеров; загружаются только видимые превью. Анимированные стикеры в меню представлены статичными миниатюрами.

## Языки

Язык интерфейса выбирается по системному языку консоли; если подходящего языкового файла нет, используется английский.
На главном экране нажмите ![Start](assets/controller-icons/start.svg) и измените **Язык** (первая строка), чтобы выбрать язык вручную или вернуться к *Автоматически*.
Выбор хранится в `settings.json` (`language`; пустое значение — автоматически).
Поставляются все официальные системные языки PS Vita: английский, русский, немецкий, французский, испанский, итальянский, нидерландский, португальский (Португалия и Бразилия), шведский, датский, норвежский, финский, польский, турецкий, японский, корейский, китайский упрощённый и традиционный (британский английский использует английский файл). Тексты для португальского (Португалия) и традиционного китайского адаптированы из бразильского и упрощённого вариантов и требуют проверки носителями.
Корейский и китайский используют собственный шрифт (`assets/NotoSans{KR,SC,TC}-Regular.otf`, подмножества собирает `scripts/shrink-fonts.sh`), который загружается только при выбранном этом языке интерфейса; поэтому корейский и китайский текст в чатах рисуется только в этих языках.

Все тексты находятся в `assets/lang/<код>.lang` и упаковываются в `app0:lang/`. Чтобы добавить или поправить язык, скопируйте `assets/lang/en.lang`, переведите значения и пересоберите пакет — менять код не нужно, новый язык появится в меню сам.
Недостающие ключи берутся из английского файла. Кнопки консоли записаны токенами вроде `{cross}`, `{circle}`, `{dpad}` и заменяются значками при загрузке файла, поэтому перевод меняет только текст, но не значки. `{0}`, `{1}`… — значения, которые подставляет программа.
Новые тексты в коде пишутся через `tr("ключ")` / `tr("ключ", {аргументы})` из `src/i18n.h`; ключ добавляется во все `.lang`. `bash tests/run-i18n-tests.sh` проверяет совпадение ключей и подстановок во всех каталогах и наличие каждого используемого в коде ключа.
Для символов вне упакованных шрифтов (например, арабский) сначала нужен подходящий шрифт в `assets/`. Самая первая заставка остаётся на английском.

## Сборка

Сборка выполняется в Linux или WSL. Нужны VitaSDK с ARM toolchain, CMake, Make, Git, curl и инструменты распаковки архивов.
Зависимости для Vita: vita2d, OpenSSL, zlib, libpng, libjpeg, libwebp, FreeType и bzip2. Jansson собирается из `external/jansson`.

### Совместимость TDLib

Текущий код ориентируется на современную схему TDLib из подготовленного дерева `external/tdlib-upstream`.
Нужна **версия с правками порта для Vita**, а не чистый upstream-клон: совместимость POSIX, файловых блокировок, SQLite и сборки дорабатывалась отдельно.
Единого скрипта для воспроизведения всех этих правок на произвольной версии upstream пока нет.

Каталог `external/tdlib-vita` также необходим: CMake использует из него Vita-совместимые функции.
По умолчанию CMake выбирает старый форк; для текущего клиента явно передавайте `TDLIB_SOURCE_DIR`.
Не заменяйте подготовленные зависимости без сохранения локальных изменений.

### Подготовка

Получите собственные `VITAGRAM_API_ID` и `VITAGRAM_API_HASH` в разделе API development tools на my.telegram.org.
Не публикуйте личные реквизиты API и данные сессии.

#### Как исключить API-реквизиты из Git

Если значения в вашей копии проекта ещё определены прямо в `src/config.h`, перед публикацией выполните перенос ниже. Эта инструкция сама по себе не изменяет файлы конфигурации.

1. Перенесите реальные значения в `src/config.local.h`:

   ```cpp
   #pragma once
   #define VITAGRAM_API_ID 123456
   #define VITAGRAM_API_HASH "your_api_hash"
   ```

   Значения выше — примеры; локально укажите собственные реквизиты.

2. В `src/config.h` замените только два определения реквизитов на подключение локального файла. Остальные настройки оставьте без изменений:

   ```cpp
   #include "config.local.h"
   ```

3. Добавьте в корневой `.gitignore` репозитория:

   ```gitignore
   /src/config.local.h
   ```

4. Добавьте в репозиторий публичный шаблон `src/config.local.h.example` без настоящих данных:

   ```cpp
   #pragma once
   #define VITAGRAM_API_ID 0
   #define VITAGRAM_API_HASH ""
   ```

После клонирования подготовленного таким образом репозитория создайте локальный файл и заполните реквизиты:

```bash
cp -n src/config.local.h.example src/config.local.h
```

В Git должны остаться очищенный `src/config.h`, шаблон и `.gitignore`. Реальный локальный файл не добавляйте, в том числе через `git add -f`.
Если `src/config.local.h` уже отслеживается, одного `.gitignore` недостаточно. Уберите файл из индекса, сохранив локальную копию:

```bash
git rm --cached -- src/config.local.h
```

Перед коммитом локально проверьте подготовленные изменения через `git diff --cached` и правило исключения через `git check-ignore -v src/config.local.h`. Не публикуйте вывод с реквизитами.
Удаление секретов из текущих файлов **не удаляет их из предыдущих коммитов**. Если реальные значения уже закоммичены, очистите историю перед публикацией; если опубликованы — считайте их раскрытыми. Перезапись общей истории согласуйте с другими участниками.

Из корня проекта:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

bash scripts/fetch-emoji-font.sh
bash scripts/fetch-fallback-fonts.sh
bash scripts/fetch-ca-bundle.sh
bash scripts/shrink-fonts.sh
bash scripts/build-hardware-video-decoder.sh
bash scripts/build-sticker-renderer.sh
```

Перед сборкой аппаратного декодера клонируйте ветку `vita` репозитория `fish47/FFmpeg-vita` в `external/ffmpeg-vita` и переключитесь на коммит `3736b0c78da9553b2098dbb0ade0a2e0ddd1753f`. FFmpeg собирается локально в `external/ffmpeg-hardware/install`, без замены пакетов SDK.
rlottie собирается в `external/rlottie-vita/install` и нужен для TGS-анимаций. Без него сборка возможна с ограниченной поддержкой стикеров.
Шрифты и уведомления о лицензиях включаются в VPK.
`shrink-fonts.sh` создаёт уменьшенные копии двух самых больших шрифтов для VPK; исходные файлы не изменяются:

- `assets/NotoColorEmoji-64.ttf`: битмапы emoji уменьшены со 136 до 64 px (интерфейс рисует их не крупнее ~31 px). Около 5 МБ экономии в VPK и около 53 КиБ видеопамяти на каждый emoji.
- `assets/NotoSansJP-JIS.otf`: кандзи ограничены JIS X 0208, первой плоскостью JIS X 0213 и расширениями Windows CP932 (7788 знаков — современный японский, включая имена); кана, пунктуация, полноширинные формы и символы сохраняются. Около 1,6 МБ экономии. Редкие кандзи и иероглифы, встречающиеся только в китайском, больше не отображаются.

Скрипт ставит fontTools, Pillow и imagequant в `external/font-tools-venv` (нужны Python 3 и доступ в интернет) и не пересоздаёт актуальные результаты. Без этих файлов CMake упаковывает полноразмерные шрифты и выводит предупреждение. После их создания повторите конфигурацию CMake.

### Конфигурация и сборка VPK

```bash
cmake -S . -B build-modern \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DTDLIB_SOURCE_DIR="$PWD/external/tdlib-upstream" \
  -DTD_ENABLE_TESTS=OFF

cmake --build build-modern -j$(nproc)
```

Результат: `build-modern/vitagram.vpk`.
После обычных изменений клиента достаточно последней команды; зависимости каждый раз пересобирать не требуется.
После добавления rlottie повторите конфигурацию CMake.

### Типичные ошибки

- **Compatibility with CMake < 3.5 has been removed** — передайте `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` при конфигурации.
- **VitaSDK libwebp is required** — нужны библиотека и заголовки WebP для Vita, не хостовой Linux-системы.
- **Undefined reference to BZ2_…** — проверьте наличие Vita-библиотеки bzip2 и актуального основного `CMakeLists.txt`.
- **photo_view.h: No such file or directory** — при переносе обновления копируйте новые заголовки вместе с `.cpp`.
- **Ошибка применения rlottie-патча** — сохраните локальные изменения и проверьте вывод скрипта сборки; не сбрасывайте дерево зависимости вслепую.
- **UPDATE_APP_TO_LOGIN** — старый форк TDLib может быть несовместим с входом Telegram; требуется подготовленная современная TDLib.

## Запись видеокружков

В открытом чате нажмите **L**: появится предпросмотр с фронтальной камеры.
**![Triangle](assets/controller-icons/triangle.svg)** переключает фронтальную и заднюю камеры до и во время записи.
**![X](assets/controller-icons/cross.svg)** начинает запись, повторное нажатие останавливает её; лимит — 60 секунд.
После подготовки MP4 нажмите **![Square](assets/controller-icons/square.svg)**, чтобы посмотреть запись со звуком,
и **![X](assets/controller-icons/cross.svg)**, чтобы отправить кружок. **![Circle](assets/controller-icons/circle.svg)** отменяет запись/подготовку и удаляет черновик.
Отправка идёт в тот чат и топик, где была открыта запись.
Кнопка **R** по-прежнему записывает голосовое сообщение.

Для каждой отправки показываются объём, процент, скорость и приблизительное оставшееся
время. После 5 секунд без изменения объёма отображается время без прогресса;
загрузка файла и подтверждение отправки Telegram — разные этапы.
Журнал: `ux0:data/vitagram/media-upload.log` (с ротацией примерно после 1 МиБ).
Аудио сохраняет исходный чат и тему даже при переходе в другой чат во время конвертации.


Формат: 240×240, до 10 кадров/с, круглая маска, H.264 и моно AAC в MP4.
Кодирование выполняется после записи с индикатором прогресса. Временные кадры и звук
занимают до примерно 55 МБ; они удаляются после подготовки или отмены.
Для этой функции пересоберите локальный FFmpeg скриптом `scripts/build-hardware-video-decoder.sh`
(добавлены AAC encoder и MP4 muxer). H.264 encoder minih264 включён в исходники с лицензией CC0.

Проверки логики записи, маски и запроса TDLib: `bash tests/run-video-tests.sh`.
Полная проверка MP4 с декодированием описана в [tests/README.md](tests/README.md).
Работу камер, микрофона и скорость подготовки файла на физической Vita нужно проверять отдельно.

## MTProxy

Ссылки `tg://proxy?server=…&port=…&secret=…` и `https://t.me/proxy?…` в сообщениях открывают предложение настроить MTProxy.
Откройте сообщение и нажмите на ссылку (или ![Triangle](assets/controller-icons/triangle.svg) на выбранной строке ссылки).
Поля заполняются автоматически; подключение меняется только после «Сохранить и применить».
![Circle](assets/controller-icons/circle.svg) отменяет импорт и возвращает к сообщению.

В **![Start](assets/controller-icons/start.svg) → MTProxy → Сканировать QR-код** наведите **заднюю (основную) камеру** на QR-код с такой ссылкой.
После распознавания камера выключится, а приложение покажет те же поля для подтверждения.
![Circle](assets/controller-icons/circle.svg) закрывает сканер. Неподходящие QR-коды не открывают браузер и не меняют настройки.
Кадры обрабатываются на устройстве, не сохраняются и не отправляются в сеть.
Сканирование доступно и до авторизации; для распознавания используется встроенная библиотека quirc (ISC).

На начальном экране авторизации нажмите **![Start](assets/controller-icons/start.svg)** или коснитесь **«Настроить MTProxy»** — откроются настройки прокси. **![O](assets/controller-icons/circle.svg)** возвращает к авторизации. Для ввода номера, кода или пароля нажмите **![X](assets/controller-icons/cross.svg) («Продолжить вход»)**; после отмены клавиатуры настройки снова доступны.
В списке чатов настройки доступны через **![Start](assets/controller-icons/start.svg) → MTProxy**.
Укажите сервер (адрес или имя хоста без `https://`), порт и secret из параметров вашего MTProxy.
Включите «Использовать MTProxy» и выберите «Сохранить и применить».
Для прямого подключения отключите этот переключатель и снова примените настройки.
Кнопка ![Circle](assets/controller-icons/circle.svg) возвращает назад без сохранения новых изменений.

Настройки сохраняются в `ux0:data/vitagram/settings.json` и применяются при следующем запуске автоматически.
Secret скрыт на экране и хранится в файле настроек в зашифрованном виде (см. «Защита секретов»).
### SOCKS5 и HTTPS-прокси

Помимо MTProxy поддерживаются **SOCKS5** и **HTTPS** (HTTP CONNECT) прокси. В настройках прокси строка **Тип** переключает MTProxy / SOCKS5 / HTTPS (**![X](assets/controller-icons/cross.svg)** или ←/→).
Для SOCKS5 и HTTPS вместо secret показываются необязательные поля **Логин** и **Пароль**; пароль скрыт на экране, но хранится в `settings.json` открытым текстом (`proxy_type`, `proxy_username`, `proxy_password`).
Ссылки `tg://socks?server=…&port=…&user=…&pass=…` и `https://t.me/socks?…` (в сообщениях и QR) импортируются как SOCKS5. Для HTTPS-прокси ссылок Telegram нет — введите данные вручную.
«Проверить подключение» (`pingProxy`) работает для всех типов. Для проверки на Linux: `bash tests/run-proxy-tests.sh`.

### Защита секретов

`proxy_secret`, `proxy_username` и `proxy_password` в `settings.json` хранятся зашифрованными (AES-256-GCM, префикс `enc1:`), ключ выводится из OpenPSID консоли. Старые открытые значения шифруются при первом запуске. База TDLib тоже шифруется ключом консоли: существующая база перешифровывается один раз после входа, признак — файл `db/.encrypted-v1`.
Это защищает от копирования файлов на другое устройство (бэкап, пересылка, утёкший архив), но не от кода, запущенного на этой же консоли. Если идентификатор консоли недоступен, значения остаются открытыми. После переноса `db` на другую консоль TDLib не откроется: удалите `ux0:data/vitagram/db` и войдите заново. Проверки: `bash tests/run-secure-tests.sh`.

Статус «MTProxy включён» означает, что TDLib приняла настройку; доступность сервера определяется при подключении.

### Проверки импорта MTProxy

На Linux можно запустить `bash tests/run-proxy-tests.sh` без VitaSDK.
Проверяются разбор ссылок и распознавание тестового QR (включая зеркальный), отмена,
повторное открытие и освобождение ресурсов при ошибках камеры. В этих тестах вызовы
камеры и GPU заменены заглушками; работа на физической Vita проверяется отдельно.

## Обновление через GitHub

**Start → Обновление** проверяет последний релиз репозитория `ing156/VitaGram` (`VITAGRAM_UPDATE_REPO`). Если версия новее установленной, **![X](assets/controller-icons/cross.svg)** скачивает и устанавливает её; во время загрузки тот же пункт отменяет её. Строка «Проверять и ставить обновления при запуске» (включена по умолчанию) через ~1 минуту после старта сама проверяет релиз, скачивает, проверяет подпись и устанавливает его, затем перезапускает приложение (через 3 с после установки и только когда открыт список чатов: не во время ввода, записи или в открытом чате). Ручная проверка из настроек тоже перезапускает приложение после установки. Сама установка выполняется при следующем запуске, пока занято минимум памяти (установщик в работающем приложении падал с `0x8001000C` = нехватка памяти); при сбое причина показывается в настройках. Автоустановка пробуется один раз за запуск; при ошибке в настройках появится причина. Настройки и база не затрагиваются.

Безопасность: соединение проверяется по сертификатам из `app0:certs/cacert.pem` (`scripts/fetch-ca-bundle.sh`). Независимо от TLS, пакет принимается только с подписью ECDSA P-256 (`*.vpk.sig`), проверяемой встроенным открытым ключом `src/update_key.h`; подписывается версия + содержимое VPK, поэтому старый подписанный пакет нельзя выдать за новый. Релиз без `.vpk.sig`, черновики и pre-release игнорируются. Если установка не удалась, проверенный VPK остаётся в `ux0:data/vitagram/update/update.vpk` — его можно поставить через VitaShell.

Выпуск релиза:

1. Поднимите `VITA_VERSION` и `VITAGRAM_VERSION` в `CMakeLists.txt`, соберите VPK.
2. `bash scripts/sign-release.sh build-modern/vitagram.vpk 0.36.0` — создаст `vitagram.vpk.sig` (закрытый ключ по умолчанию: `~/.vitagram-release/update-signing.pem`; **не кладите его в Git** и сохраните резервную копию — без него клиенты уже не примут обновления; смена ключа требует выпуска вручную).
3. Создайте **публичный** релиз с тегом `v0.36.0` и приложите оба файла: `*.vpk` и `*.vpk.sig`. Приложение не имеет токена GitHub, поэтому приватный репозиторий и его релизы недоступны.

Установка использует `ScePromoterUtil` и `sce_sys/package/head.bin` уже установленной копии, поэтому работает, если приложение само было установлено через VPK-установщик. Проверки логики (версии, разбор релиза, HTTP, подпись, распаковка): `bash tests/run-update-tests.sh`. Установка на самой Vita требует проверки на устройстве.

## Кэш и приватные данные

- `ux0:data/vitagram/db` — данные TDLib, сессия и индекс медиакэша.
- `ux0:data/vitagram/files` — загруженные медиа.
- `ux0:data/vitagram/settings.json` — настройки приложения.

Часть миниатюр и стикеров TDLib хранит в подкаталогах `db`; клиент учитывает эти пути.
Медиакэш сопоставляет файлы по постоянному идентификатору Telegram, проверяет наличие и размер файла и сохраняет индекс между запусками.
Срок хранения и ограничение объёма задаются в настройках. Очистка запускается в фоне в период простоя, а не блокирует вход в чаты при старте.

Картинки стикеров и миниатюры кэшируются. Полные анимации загружаются для просмотра.
**Списки наборов стикеров и топиков пока запрашиваются через Telegram и не имеют отдельного постоянного кэша меню.** Поэтому сохранённые картинки не исключают ожидания ответа при открытии списка.

Данные в `db` содержат приватную информацию аккаунта. Не публикуйте их и не удаляйте ради очистки картинок.

## Производительность и ограничения

UI, воспроизведение, обработка ответов и фоновые операции имеют разные приоритеты. Запросы открытого чата и выбранного медиа приоритетнее фоновой загрузки.
Обращения к TDLib выполняются одним сетевым потоком из-за ограничений используемого порта: отдельного сетевого ядра на каждый чат нет.
Дополнительное ядро используется после успешной проверки доступа; наличие плагина разблокировки само по себе не гарантирует доступ.

- Видео, видеокружки и отдельные MP4 поддерживаются ограниченно; успешное воспроизведение всех файлов не гарантируется.
- Звонки, секретные чаты, управление группами и платные/пользовательские реакции не поддерживаются этим интерфейсом.
- Создание и редактирование топиков, поиск и установка новых наборов стикеров пока не реализованы.
- Точные статусы присутствия зависят от настроек приватности Telegram.

## Основные файлы

- `src/main.cpp` — интерфейс, управление и отрисовка.
- `src/telegram_client.*` — авторизация, очередь запросов, чаты, темы, сообщения, стикеры и реакции.
- `src/photo_view.h` — масштабирование, перемещение и ориентация фото.
- `src/keyboard.*` — системная клавиатура Vita.
- `src/media_render.*` — шрифты, emoji и WebP.
- `src/animation_player.*`, `src/voice_player.*` — воспроизведение медиа.
- `src/media_cache.*`, `src/ui_cache.*` — кэш медиа и интерфейса.
- `src/cpu_scheduler.*` — приоритеты потоков и распределение нагрузки.
- `src/settings.h`, `src/vita_net.*` — настройки и состояние сети.
- `src/i18n.*`, `assets/lang/*.lang` — локализация.

## Лицензии

Код проекта распространяется на условиях [MIT](LICENSE).
TDLib, VitaSDK, vita2d, FFmpeg, rlottie, шрифты и другие зависимости имеют собственные лицензии.
При распространении сохраняйте уведомления и соблюдайте требования лицензий зависимостей, включая предоставление исходников и возможность перелинковки там, где это требуется.


## Опросы

Откройте сообщение с опросом кнопкой **![X](assets/controller-icons/cross.svg)**. **↑/↓** выбирают строку, **![X](assets/controller-icons/cross.svg)** отмечает
вариант, **![Triangle](assets/controller-icons/triangle.svg)** отправляет выбранные ответы. В обычном опросе **![Square](assets/controller-icons/square.svg)**
отзывает голос, если изменение ответа разрешено. **![O](assets/controller-icons/circle.svg)** возвращает к сообщениям.
Варианты и отправка также доступны касанием. Поддерживаются множественный выбор,
викторины, правильные ответы и пояснения, результаты, закрытые опросы и ошибки
голосования. После ответа результаты запрашиваются заново у Telegram.
Создание новых опросов и просмотр списка проголосовавших пока не реализованы.


## Качество загружаемого видео

Обычные видео автоматически используют минимальное разрешение среди доступных
совместимых вариантов Telegram; при одинаковом разрешении выбирается меньший
файл, если размеры известны. Поддерживаются варианты H.264, VP8 и VP9;
H.265/AV1 не выбираются, поскольку их декодеры не включены в сборку.
Загружается выбранный видеофайл, а не HLS-плейлист. Размеры и ограничения проверяются
для выбранного варианта. Во время загрузки показывается разрешение.
Если уменьшенной совместимой версии нет, используется исходник с прежними лимитами
720p по площади кадра и 128 МиБ. Кружки и GIF загружаются в предоставленном качестве:
списка альтернатив у них нет. Приложение не может запросить произвольные 144p/240p
или уменьшить сетевой трафик локальным перекодированием уже скачанного файла.


## Имена и аватары отправителей

Текст использует подбор шрифта для каждого символа: Noto Sans, Noto Sans Coptic и
Noto Sans JP Regular. Поддерживаются смешанные имена вроде `ⲕⲗυⲙⲁⲧ` и `の一部 宇宙`;
измерение ширины и отрисовка используют один и тот же выбор шрифта.
Это не означает поддержку всех письменностей и сложных emoji-последовательностей.
Шрифты и SIL OFL лицензии входят в VPK; для восстановления файлов выполните
`bash scripts/fetch-fallback-fonts.sh`.

Рядом с именами отправителей во входящих сообщениях групп показываются аватары
18×18 пикселей. Маленькие версии фотографий запрашиваются для видимых сообщений;
до загрузки или при отсутствии фото отображается цветной кружок. Поддерживаются
отправители-пользователи и сообщения от имени чатов/каналов. Данные сохраняются
в кэше; устаревшие идентификаторы файлов не используются после перезапуска.


WebM-стикеры и видео VP8/VP9 разрешены в проигрывателе.
Исправлен `Open: -22 Invalid argument`: контейнер Matroska/WebM раньше отсутствовал
в списке разрешённых форматов, хотя демультиплексор и декодеры уже входили в FFmpeg.
Аудиопроигрыватель видео также принимает WebM с Opus/Vorbis. Существующие ограничения
размера файла и разрешения остаются в силе.


При выходе из аккаунта приложение закрывает TDLib и фоновые записи кэша,
затем удаляет базу аккаунта, кэш чатов и медиа, скачанные файлы, записи голосовых
сообщений и журналы отправки. Поздние ответы не восстанавливают старый кэш.
Дождитесь сообщения об окончании удаления; при прерывании очистка повторяется
при следующем запуске до загрузки кэша. Настройки приложения и MTProxy сохраняются.

Шрифт Noto Sans Math добавляет отображение математических алфавитов в никах.
