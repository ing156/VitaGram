[English](#english) | [Русский](#russian)

<a id="english"></a>

# VitaGram — Telegram for PS Vita

An unofficial homebrew Telegram client built with VitaSDK, vita2d and TDLib.
Source version: **0.34.60**. VPK package version: **34.60**. This project is under development.

0.33.0 starts the performance work with bounded diagnostics: hold ![Select](assets/controller-icons/select.svg) for frame/event latency and queue depth;
aggregated measurements are written in the background to `ux0:data/vitagram/performance.log`.
See the [measurement guide](docs/performance-release-0.33.0.md). Subsequent optimization releases use 0.33.x.

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
- Light/dark themes, font size, cache retention settings and account logout.
- Connection indicator, HTTP(S) links opened in the browser, and a startup screen showing initialization stages.

A feature being present in the source does not mean every scenario has been tested on hardware.
The latest sticker and forum-topic changes still require build and PS Vita testing.

## Controls

### Main menu

- **![D-pad](assets/controller-icons/dpad.svg) / touch** — select and scroll; **![X](assets/controller-icons/cross.svg) / tap** — open a chat.
- **L / R** — switch folders; folder tabs also support touch and horizontal scrolling.
- **![Triangle](assets/controller-icons/triangle.svg)** — refresh the list.
- **![Start](assets/controller-icons/start.svg)** — settings.
- **Hold ![Select](assets/controller-icons/select.svg)** — CPU and cache diagnostics.
- **Hold ![Start](assets/controller-icons/start.svg), then press ![Select](assets/controller-icons/select.svg) and hold both for 2 seconds** — exit the application.

### Chat

- **Up / Down / vertical swipe** — scroll and load older messages.
- **![X](assets/controller-icons/cross.svg) / tap a message** — open the expanded view.
- **Short press ![Square](assets/controller-icons/square.svg)** — compose text; the keyboard opens after releasing the button.
- **Hold ![Square](assets/controller-icons/square.svg) for at least 0.55 seconds** — open the sticker picker.
- **![Triangle](assets/controller-icons/triangle.svg)** — refresh history.
- **R** — start/stop microphone recording and send the recorded audio message.
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

- **L / R** — switch sets; tapping the left/right half of the heading also switches sets.
- **![D-pad](assets/controller-icons/dpad.svg)** — select a sticker and move between pages.
- **![X](assets/controller-icons/cross.svg) / tap a sticker** — immediately send it to the current chat or topic.
- **![O](assets/controller-icons/circle.svg) / ![Square](assets/controller-icons/square.svg)** — close the picker.
- Up to 18 stickers per page; only visible previews are downloaded. Animated stickers use static thumbnails in the picker.

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

## Licenses

Project code is licensed under [MIT](LICENSE).
TDLib, VitaSDK, vita2d, FFmpeg, rlottie, fonts and other dependencies have their own licenses.
When distributing the application, retain notices and comply with dependency license requirements, including source availability and relinking where required.

---

<a id="russian"></a>

[English](#english) | Русский

# VitaGram — Telegram для PS Vita

Неофициальный homebrew-клиент Telegram на VitaSDK, vita2d и TDLib.
Версия исходников: **0.34.60**, версия пакета VPK: **34.60**. Проект находится в разработке.

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
- Светлая/тёмная тема, размер шрифта, настройки хранения кэша и выход из аккаунта.
- Индикатор подключения, открытие HTTP(S)-ссылок в браузере, заставка с этапами запуска.

Наличие функции в исходниках не означает проверку всех сценариев на устройстве.
Последние изменения стикеров и топиков требуют проверки сборкой и на PS Vita.

## Управление

### Главное меню

- **![Стрелки](assets/controller-icons/dpad.svg) / тач** — выбор и прокрутка; **![X](assets/controller-icons/cross.svg) / тап** — открыть чат.
- **L / R** — переключить папку; закладки также поддерживают тач и горизонтальную прокрутку.
- **![Треугольник](assets/controller-icons/triangle.svg)** — обновить список.
- **![Start](assets/controller-icons/start.svg)** — настройки.
- **![Select](assets/controller-icons/select.svg) (удерживать)** — диагностика CPU и кэша.
- **Зажать ![Start](assets/controller-icons/start.svg), затем нажать ![Select](assets/controller-icons/select.svg) и держать обе кнопки 2 секунды** — выход из приложения.

### Переписка

- **Вверх / вниз / вертикальный свайп** — прокрутка и загрузка более старых сообщений.
- **![X](assets/controller-icons/cross.svg) / тап по сообщению** — полный просмотр.
- **![Квадрат](assets/controller-icons/square.svg), короткое нажатие** — ввод текста; клавиатура открывается после отпускания кнопки.
- **![Квадрат](assets/controller-icons/square.svg), удержание от 0,55 секунды** — меню отправки стикеров.
- **![Треугольник](assets/controller-icons/triangle.svg)** — обновить историю.
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

- **L / R** — переключить набор; тап по левой/правой половине заголовка также меняет набор.
- **![Стрелки](assets/controller-icons/dpad.svg)** — выбрать стикер, перейти на следующую страницу.
- **![X](assets/controller-icons/cross.svg) / тап по стикеру** — сразу отправить в текущий чат или топик.
- **![O](assets/controller-icons/circle.svg) / ![квадрат](assets/controller-icons/square.svg)** — закрыть меню.
- На странице до 18 стикеров; загружаются только видимые превью. Анимированные стикеры в меню представлены статичными миниатюрами.

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
Удерживайте **![SELECT](assets/controller-icons/select.svg)** для подробностей всех отправок, включая точное число байтов.
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
Secret скрыт на экране, но хранится в файле настроек открытым текстом.
Статус «MTProxy включён» означает, что TDLib приняла настройку; доступность сервера определяется при подключении.

### Проверки импорта MTProxy

На Linux можно запустить `bash tests/run-proxy-tests.sh` без VitaSDK.
Проверяются разбор ссылок и распознавание тестового QR (включая зеркальный), отмена,
повторное открытие и освобождение ресурсов при ошибках камеры. В этих тестах вызовы
камеры и GPU заменены заглушками; работа на физической Vita проверяется отдельно.

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

## Лицензии

Код проекта распространяется на условиях [MIT](LICENSE).
TDLib, VitaSDK, vita2d, FFmpeg, rlottie, шрифты и другие зависимости имеют собственные лицензии.
При распространении сохраняйте уведомления и соблюдайте требования лицензий зависимостей, включая предоставление исходников и возможность перелинковки там, где это требуется.


Анализ запуска, загрузки чатов и распределения потоков:
[docs/performance-analysis-0.31.2.md](docs/performance-analysis-0.31.2.md).
Результаты анализа кода не являются замерами скорости на физической PS Vita.


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


WebM-стикеры и видео VP8/VP9 разрешены в проигрывателе (начиная с 0.31.6).
Исправлен `Open: -22 Invalid argument`: контейнер Matroska/WebM раньше отсутствовал
в списке разрешённых форматов, хотя демультиплексор и декодеры уже входили в FFmpeg.
Аудиопроигрыватель видео также принимает WebM с Opus/Vorbis. Существующие ограничения
размера файла и разрешения остаются в силе.


При выходе из аккаунта (0.31.8) приложение закрывает TDLib и фоновые записи кэша,
затем удаляет базу аккаунта, кэш чатов и медиа, скачанные файлы, записи голосовых
сообщений и журналы отправки. Поздние ответы не восстанавливают старый кэш.
Дождитесь сообщения об окончании удаления; при прерывании очистка повторяется
при следующем запуске до загрузки кэша. Настройки приложения и MTProxy сохраняются.

Шрифт Noto Sans Math добавляет отображение математических алфавитов в никах, включая **𝕭𝖆𝖉𝕯𝖗𝖊𝖆𝖒**, с сохранением начертания символов.

Декодер текста vita2d заменён на UTF-8 с поддержкой четырёхбайтовых символов; это исправляет и отрисовку, и расчёт ширины таких имён.

В 0.31.9 кэшированный список чатов сохраняется до окончания первой загрузки каждой папки и заменяется целиком. Ошибка подключения сохраняет видимый кэш. Noto Sans Symbols 2 добавляет символ ✧, в том числе в имени «✧Hehestl | *interrogative hehe*».

В 0.31.10 иконки наборов используют статичную обложку или миниатюру первого подходящего стикера. При отсутствии обложки метаданные набора запрашиваются только для видимых строк; TGS/WebM не передаются декодеру статичных изображений. Список наборов обновляется после запуска, чтобы не использовать старые ID файлов.

В 0.32.0 добавлена проверка MTProxy без сохранения настроек. Обновления списка чатов восстанавливаются после таймаута и переподключения; нижняя строка сбрасывает завершённые события при новом действии.

В 0.32.1 исправлены состояния отправки и скорость в статистике: стикеры отслеживаются отдельно, завершённые события не перекрывают текущие задачи, скорость учитывает время получения событий. Переключения меню приостанавливают новые фоновые запросы и запись кэша на 150 мс; обработка подтверждений отправки получает приоритет.

В 0.33.0 начат первый этап оптимизации — измерения производительности. При удержании Select видны длительность кадров, задержка событий, размеры очередей и возраст показателей. Фоновый журнал `ux0:data/vitagram/performance.log` содержит агрегаты по UI, TDLib, кешу, запуску и отмене/подготовке медиа; тексты сообщений и секреты в него не записываются. [Описание метрик и сценарии проверки](docs/performance-release-0.33.0.md). Следующие этапы получают версии 0.33.1, 0.33.2 и далее.

В 0.33.1 по результатам замеров на Vita переработано сохранение кеша: неизменяемые записи в снимке вместо полного глубокого копирования под блокировкой, блочная запись с прерыванием и отдельный приоритет Storage. Сохранение откладывается при отправке медиа, активном вводе и большой очереди событий; завершение приложения сохраняет последний готовый снимок без нового полного flush. Список чатов копирует только нужное окно, обычные события получают обработку и при постоянном касании. Выход требует Start → Select с удержанием 2 секунды, поэтому Start при уже удерживаемом Select не закрывает приложение во время системного скриншота. [Разбор логов и проверка](docs/performance-release-0.33.1.md).

В 0.33.2 добавлены мягкий бюджет обработки очередей TDLib и измерения текущего этапа сетевого потока для зависшей отправки. [Изменения, ограничения и проверка на Vita](docs/performance-release-0.33.2.md).

В 0.33.3 отключено подробное синхронное логирование TDLib, устранены повторные попытки записи в недоступный stderr на Vita, исправлен статус при 100% без подтверждения и расширены измерения файлового I/O, вывода логов и подключения сокетов. [Результаты нового прогона](docs/performance-release-0.33.3.md).

В 0.33.4 стартовый кеш восстанавливается в фоне, файловые проверки медиакеша вынесены из UI и выполняются без удержания его mutex. Повторные проверки объединяются, свежие данные Telegram имеют приоритет над поздним снимком. [Изменения и проверка](docs/performance-release-0.33.4.md).

В 0.33.5 файловые проверки также убраны из последовательного парсера событий: ограниченная очередь на потоке Storage обновляет только медиафайлы, не задерживая разбор новых сообщений. [Изменения и проверка](docs/performance-release-0.33.5.md).

В 0.33.6 список чатов восстанавливается из отдельного компактного индекса и применяется порциями. Истории и стикеры не входят в стартовый снимок; добавлены отдельные замеры чтения, JSON-разбора и объединения кеша. [Изменения и проверка](docs/performance-release-0.33.6.md).

В 0.33.7 исправлены регрессии загрузчика кеша: восстановлены позиции чатов, приоритет живых данных и отмена после очистки. Повреждённый стартовый индекс перестраивается автоматически. [Изменения и проверка](docs/performance-release-0.33.7.md).

В 0.33.8 фоновая загрузка кеша уступает вводу и очереди событий, объединение данных вынесено из общей блокировки, а обновления статусов пользователей больше не вызывают публикацию списка чатов. [Изменения и проверка](docs/performance-release-0.33.8.md).

В 0.33.9 декодирование PNG/JPEG/WebP перенесено из кадра интерфейса в фоновый поток с ограниченной очередью и отменой устаревших результатов. [Изменения и проверка](docs/performance-release-0.33.9.md).

В 0.33.10 добавлена ограниченная диагностика типов и размеров медленных событий, операций UI-кеша и пауз фоновой загрузки без записи содержимого сообщений. [Описание и проверка](docs/performance-release-0.33.10.md).

В 0.33.11 исправлены счётчики диагностики на Vita и передача размера JSON; добавлены замеры обработчиков ввода, рисования и переключения медиаплееров. [Описание и проверка](docs/performance-release-0.33.11.md).

В 0.33.12 кешируются повторные измерения текста и выбор fallback-шрифта; длинные строки обрезаются двоичным поиском по границам UTF-8. [Описание и проверка](docs/performance-release-0.33.12.md).

В 0.33.13 проверка востребованных файлов выполняется во время загрузки полного кеша; видимые файлы получают приоритет с сохранением фонового обслуживания. [Описание и проверка](docs/performance-release-0.33.13.md).

В 0.33.14 ответы для обложек стикерпаков сокращаются до одного превью; открытая сетка применяется порциями с отменой при переключении. [Описание и проверка](docs/performance-release-0.33.14.md).

В 0.33.15 неиспользуемые updateStickerSet сокращаются на Parser до передачи на UI; добавлены замеры освобождения JSON. [Описание и проверка](docs/performance-release-0.33.15.md).

В 0.33.16 локальные аватары доступны через лёгкий стартовый индекс; видимые иконки проверяются во время загрузки полного кеша. [Описание и проверка](docs/performance-release-0.33.16.md).

В 0.33.17 события открытого чата обрабатываются приоритетно; стикеры открываются на избранном, полные наборы не загружаются ради обложек. Добавлены кастомные реакции со статическими превью. [Описание и проверка](docs/performance-release-0.33.17.md).

В 0.33.18 возвращено меню стикеров с приоритетом недавних; неиспользуемые обновления стикеров разбираются без полного дерева, повышен приоритет входа в чат и добавлено обновление данных просматриваемого поста. [Описание и проверка](docs/performance-release-0.33.18.md).

В 0.33.23 запасные шрифты и шрифт эмодзи загружаются в фоне (запуск снова не ждёт ~11 МБ шрифтов), а медленные open/stat TDLib записываются с каталогом файла. [Описание и проверка](docs/performance-release-0.33.23.md).

В 0.33.24 раздутые каталоги кэша (аватарки, миниатюры, фото, временные файлы) один раз заменяются пустыми перед стартом TDLib, старые копии удаляются в фоне; TDLib создаёт файлы с именами-счётчиками, аватарки списка не скачиваются заново, пока проверяется сохранённая копия; открытый чат и выбранная папка получают приоритет. [Описание и проверка](docs/performance-release-0.33.24.md).

В 0.33.25 обрезка кэша пользователей и чатов идёт небольшими порциями (без двухсекундного фриза при старте), а фоновое удаление корзины останавливается, когда удаление одного файла занимает дольше 250 мс — остаток `*.trash*` можно удалить с ПК. [Описание и проверка](docs/performance-release-0.33.25.md).

В 0.33.26 ответ со списком наборов стикеров сокращается в потоке Parser (остаются id, название и одна миниатюра), поэтому открытие панели стикеров не вызывает кадра в ~0,3 с. [Описание и проверка](docs/performance-release-0.33.26.md).

В 0.33.27 длинное видео удерживает фоновые загрузки, пока скачивается, зависшая загрузка перезапускается через 20 с без прогресса, а список реакций запрашивается при открытии сообщения. Добавлены метрики `video_ready_us` и `reaction_response_us`. [Описание](docs/performance-release-0.33.27.md).

В 0.33.28 очистка старых каталогов `*.trash*` останавливается при удалении дольше 100 мс (раньше 250 мс) и работает только в окне 20–60 с после старта: в 0.33.26 долгие удаления на 120–170 мс замедляли TDLib и открытие чата до ~1 с. Остатки лучше удалить с ПК. [Описание](docs/performance-release-0.33.28.md).

В 0.33.29 ответ `stickerSets` (~300 КБ) сжимается ещё до разбора JSON: Parser пропускает обложки на уровне грамматики и оставляет только id, название и один превью-файл на набор. В 0.33.28 разбор и освобождение этого ответа занимали 0,4–1,6 с и тормозили TDLib при открытии панели стикеров. [Описание](docs/performance-release-0.33.29.md).

В 0.33.41 новые фото профиля, миниатюры и фото TDLib складывает в подпапки `b<N>/` по 32 файла: на exFAT создание файла сканирует всю папку, и `td_file_meta` рос с ~70 до ~145 мс по мере накопления файлов. Старые плоские каталоги один раз переименовываются в `.trash`, иконки скачиваются заново. [Описание](docs/performance-release-0.33.41.md).

В 0.33.42 ответы `stickerSet`/`stickers` сжимаются до разбора JSON (контуры и списки эмодзи пропускаются), а кэш текстур учитывает реальные 256 КиБ CDRAM на блок и ограничен 32 МиБ: в логах 0.33.41 свободная CDRAM падала с 90 до 31 МБ, а открытие набора стикеров давало подвисания до 200 мс. [Описание](docs/performance-release-0.33.42.md).

В 0.33.45 ответы открытого чата (`history`, `reaction_*`, `forum_*`) обгоняют поток обновлений запуска ещё на входе Parser: поток TDLib определяет их по хвосту сырой строки (`@extra`) и ставит в начало очереди разбора. Раньше первая история ждала разбора ~1500 обновлений (`first_history_raw_wait` до 1,6 с). Не проверено на устройстве.

В 0.33.46 сжатие ответов `stickerSet`/`stickers` собирает файловые объекты стикеров по членам (остаются только читаемые поля `local`/`remote`): в 0.33.45 оно сохраняло по два полных файла на стикер, и парс (~220 мс на 66 КБ) почти не ускорился. Добавлена метрика `sticker_compact_miss` для ответов, отвергнутых сжатием. Не проверено на устройстве.

В 0.33.47 разбор имён ключей и простых значений в сжатии `stickerSet`/`stickers`/`stickerSets` не вызывает Jansson (на каждый ключ шёл `json_loadb`, а ключей тысячи). Не проверено на устройстве.

В 0.33.48 подрезка кэша пользователей и чатов до лимитов (1024 и 2000) выполняется один раз при загрузке на потоке Storage. Раньше кэш на диске был выше лимита, и каждое `updateUser` на старте удаляло по 16 записей на UI-потоке (55–78 мс). Не проверено на устройстве.

В 0.33.49 освобождение деревьев выносилось на поток Storage, но замедляло старт (общий замок `malloc`), в 0.33.50 вынос убран.

В 0.33.50 ответы `updateChatLastMessage`/`updateChatDraftMessage` сжимаются до разбора JSON (остаются только `chat_id`, `positions`, id и `is_outgoing` последнего сообщения), а лишняя кэшированная история выселяется порциями по 8 сообщений вместо одного удара на 100–250 мс. Не проверено на устройстве.

В 0.34.51 ответы `messages` (страницы истории по 30–165 КБ) сжимаются до разбора JSON: из каждого сообщения остаются только читаемые клиентом и кэшем члены верхнего уровня, а из `content` вырезаются `minithumbnail`, `waveform`, `outline`, `link_preview`. Любое отклонение формата идёт прежним путём Jansson. Не проверено на устройстве.

В 0.34.52 добавлены метрики `messages_compact_hit` и `messages_compact_miss` (размер ответа в байтах): по ним в следующем логе видно, срабатывает ли сжатие `messages`. Поведение не менялось.

В 0.34.53 стикеры (`db_stickers`) разложены по подкаталогам `b<N>/` (как фото и миниатюры), старый плоский каталог один раз уходит в корзину при обновлении: поиск файла в каталоге из 150+ записей давал 50–145 мс на вызов. Добавлены метрики `messages_compact_us` (время сжатия ответа `messages`) и `messages_remember_us` (фаза `remember` только для `messages`).

В 0.34.54 сжатие `messages` сужает типизированные медиа: у фото оставлены только `type/width/height/photo`, у `alternative_videos` — `codec/width/height/video`, у файлов — `id/size/expected_size/local/remote`, а `local` — только читаемые поля. Метрика `td_file_meta_us` разделена на `td_file_meta_sticker_us` и `td_file_meta_other_us`, чтобы понять, почему поиск стикера всё ещё ~4 мс.

В 0.34.55 только диагностика: `td_file_meta_other_us` разбит по каталогам (`td_file_meta_temp_us`, `_photos_`, `_thumbs_`, `_db_`), чтобы найти источник медианы ~4 мс; этап `draw` разбит на подэтапы `draw_begin`, `draw_header`, `draw_chat`, `draw_overlays`, `draw_end` (медленные события >50 мс), чтобы найти причину разовых зависаний кадра на 1.3–2.4 с.

В 0.34.56 фоновая загрузка деталей кэша на старте стала мягче: при 8+ событиях в очередях Parser/UI она работает с половинной скважностью (2 мс работы, 2 мс отдыха вместо 80%), а полная пауза включается с 32 событий (было 64). Для `folder_ready_us` добавлены `folder_first_row_us` (время до первой строки) и `folder_requests` (число запросов loadChats), чтобы понять, откуда берутся 37 с при смене папки.
В 0.34.58 убраны три длинных кадровых затыка из лога 0.34.56: (1) `wrap_text` больше не измеряет каждую строку-кандидат целиком (это было квадратично, до 105 мс на длинный пост) — ширина считается суммой ширин слов, а полное измерение остаётся только у границы ±12 px, поэтому кернинг и запасные шрифты решают пограничные случаи как раньше; кэш разметки вырос с 16 до 48 записей; (2) JSON ответов TDLib от 16 КБ на UI-потоке освобождается порциями по ~2.5 мс за кадр (до 8 мс при большой очереди), а не за один раз (`release` 86–91 мс для `messages`/`stickerSet`); освобождение остаётся на том же потоке, чтобы не повторить проблему 0.33.49 с общим замком malloc; (3) снимок UI-кэша пишется не чаще раза в 90 с вместо 30 (запись занимала 1.7–1.9 с на карте, общей с TDLib). Цена: при аварийном выходе теряются изменения кэша за последние до 90 с.
В 0.34.59: (1) сохранение ответа `messages` в UI-кэш (глубокие копии ~1.7 мс/КБ, до 150 мс на UI-потоке) перенесено в поток Parser (`UiCache::rememberMessages`): копии делаются без мьютекса, под ним только вставка и вытеснение, оставляются 60 новейших сообщений чата; UI пропускает такой ответ по метке `@ui_remembered`; при выходе из аккаунта Parser не сохраняет; (2) диагностика замка медиакэша: ожидание или удержание ≥50 мс даёт slow_event `mlw_<функция>_<строка>_by_<владелец>_<строка>` / `mlh_<функция>_<строка>` (для поиска 142 мс `media_lock_us`); (3) сметалка мусора больше не останавливается на первом удалении >100 мс: стоп только при одном удалении >400 мс или трёх подряд >150 мс (раньше каждый запуск чистил один файл).

В 0.34.60: (1) освобождение больших JSON-ответов делится глубже (глубина 3 → 6), чтобы `release stickerSets` не занимал до 310 мс одним кадром; (2) копия списка наборов стикеров в UI-кэш (`saveStickerPacks`, до 139 мс) выполняется на Parser вне мьютекса, UI пропускает повтор по метке `@ui_saved`; (3) `AnimationPlayer::upload` не пишет `video-hardware.log` на SD из UI-потока (раз в 60 кадров и при первом кадре, под мьютексом) — вероятная причина 813 мс `animationplayer_upload`; (4) в строку `device` лога добавлены `cdram_images`, `cdram_glyphs`, `cdram_video` для различения заполнения кэшей и утечки CDRAM.

В 0.33.22 группа больше не ждёт проверки форума перед первой историей (признак форума берётся из обновлений TDLib, неизвестная группа проверяется параллельно), текстовые шрифты загружаются в память, файловый I/O TDLib разделён на sync, open/stat и чтение/запись. [Описание и проверка](docs/performance-release-0.33.22.md).

В 0.33.21 добавлены замеры отправки и получения ответа первой истории, её разбора и проверки форума. [Описание и проверка](docs/performance-release-0.33.21.md).

В 0.33.20 события несут готовые метаданные, бюджет pump растёт при фоновой перегрузке, а loadChats и видимые аватары продолжают загружаться. Добавлены замеры первого отображения сообщений из кеша и сети. [Описание и проверка](docs/performance-release-0.33.20.md).

В 0.33.19 приоритетные события выбираются через индексы без обхода готовой очереди. Добавлены отдельные замеры первого ответа истории. [Описание и проверка](docs/performance-release-0.33.19.md).
