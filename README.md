[English](#english) | [Русский](#russian)

<a id="english"></a>

# VitaGram — Telegram for PS Vita

An unofficial homebrew Telegram client built with VitaSDK, vita2d and TDLib.
Source version: **0.31.2**. VPK package version: **31.02**. This project is under development.

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

- **D-pad / touch** — select and scroll; **X / tap** — open a chat.
- **L / R** — switch folders; folder tabs also support touch and horizontal scrolling.
- **Triangle** — refresh the list.
- **Start** — settings.
- **Hold Select** — CPU and cache diagnostics.
- **Start + Select** — exit the application.

### Chat

- **Up / Down / vertical swipe** — scroll and load older messages.
- **X / tap a message** — open the expanded view.
- **Short press Square** — compose text; the keyboard opens after releasing the button.
- **Hold Square for at least 0.55 seconds** — open the sticker picker.
- **Triangle** — refresh history.
- **R** — start/stop microphone recording and send the recorded audio message.
- **O** — go back; inside a forum topic, return to the topic list.

### Forum topics

- The topic list opens before the conversation when TDLib identifies a group as a forum.
- **D-pad / touch** — select; **X / tap** — open a topic.
- **Down near the end of the list / Triangle** — load the next batch of 15 topics.
- **O** — return to the chat list.
- Each entry shows its name, unread count and closed-topic marker.

### Expanded post

- **Up / Down** — scroll the post.
- **Left / Right / horizontal swipe** — previous or next post.
- **L / R** — select a photo within an album.
- **X** — open the selected photo or play/pause supported media.
- **Square** — choose a reaction, rather than compose text.
- **O** — go back.

### Fullscreen photo

- **Left / Right** — switch photos; swiping switches photos at the default zoom level.
- **Two fingers** — zoom; **one finger while zoomed in** — pan.
- **Triangle** — reset zoom and position; **O** — go back.
- The image adjusts to the console's orientation. When the console lies face up, the previous orientation is retained. The header and control hints do not rotate.

### Sticker picker

- **L / R** — switch sets; tapping the left/right half of the heading also switches sets.
- **D-pad** — select a sticker and move between pages.
- **X / tap a sticker** — immediately send it to the current chat or topic.
- **O / Square** — close the picker.
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
bash scripts/build-animation-decoder.sh
bash scripts/build-sticker-renderer.sh
```

FFmpeg is built locally in `external/ffmpeg-animation/install`, without replacing SDK packages.
rlottie is built in `external/rlottie-vita/install` and is needed for TGS animations. Building without it is possible, with limited sticker support.
Fonts and license notices are included in the VPK.

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
Версия исходников: **0.24**, версия пакета VPK: **24.00**. Проект находится в разработке.

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

- **Стрелки / тач** — выбор и прокрутка; **X / тап** — открыть чат.
- **L / R** — переключить папку; закладки также поддерживают тач и горизонтальную прокрутку.
- **Треугольник** — обновить список.
- **Start** — настройки.
- **Select (удерживать)** — диагностика CPU и кэша.
- **Start + Select** — выход из приложения.

### Переписка

- **Вверх / вниз / вертикальный свайп** — прокрутка и загрузка более старых сообщений.
- **X / тап по сообщению** — полный просмотр.
- **Квадрат, короткое нажатие** — ввод текста; клавиатура открывается после отпускания кнопки.
- **Квадрат, удержание от 0,55 секунды** — меню отправки стикеров.
- **Треугольник** — обновить историю.
- **O** — назад; из топика возвращает к списку тем.

### Темы форумной группы

- Список открывается перед перепиской, если TDLib определяет группу как форум.
- **Стрелки / тач** — выбор; **X / тап** — открыть тему.
- **Вниз у конца списка / треугольник** — загрузить следующую порцию из 15 тем.
- **O** — вернуться к списку чатов.
- Показываются название, количество непрочитанных и отметка закрытой темы.

### Полный просмотр поста

- **Вверх / вниз** — прокрутка поста.
- **Влево / вправо / горизонтальный свайп** — предыдущий или следующий пост.
- **L / R** — выбрать фото в альбоме.
- **X** — открыть выбранное фото либо включить/приостановить поддерживаемое медиа.
- **Квадрат** — выбор реакции, а не ввод текста.
- **O** — назад.

### Полноэкранное фото

- **Влево / вправо** — сменить фотографию; свайп переключает фото при обычном масштабе.
- **Два пальца** — масштабирование; **один палец при увеличении** — перемещение.
- **Треугольник** — сброс масштаба и смещения; **O** — назад.
- Изображение подстраивается под поворот консоли. При положении экраном вверх сохраняется предыдущая ориентация. Шапка и подсказки не поворачиваются.

### Меню стикеров

- **L / R** — переключить набор; тап по левой/правой половине заголовка также меняет набор.
- **Стрелки** — выбрать стикер, перейти на следующую страницу.
- **X / тап по стикеру** — сразу отправить в текущий чат или топик.
- **O / квадрат** — закрыть меню.
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
bash scripts/build-animation-decoder.sh
bash scripts/build-sticker-renderer.sh
```

FFmpeg собирается локально в `external/ffmpeg-animation/install`, без замены пакетов SDK.
rlottie собирается в `external/rlottie-vita/install` и нужен для TGS-анимаций. Без него сборка возможна с ограниченной поддержкой стикеров.
Шрифты и уведомления о лицензиях включаются в VPK.

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
**Triangle** переключает фронтальную и заднюю камеры до и во время записи.
**X** начинает запись, повторное нажатие останавливает её; лимит — 60 секунд.
После подготовки MP4 нажмите **Square**, чтобы посмотреть запись со звуком,
и **X**, чтобы отправить кружок. **Circle** отменяет запись/подготовку и удаляет черновик.
Отправка идёт в тот чат и топик, где была открыта запись.
Кнопка **R** по-прежнему записывает голосовое сообщение.

Формат: 240×240, до 10 кадров/с, круглая маска, H.264 и моно AAC в MP4.
Кодирование выполняется после записи с индикатором прогресса. Временные кадры и звук
занимают до примерно 55 МБ; они удаляются после подготовки или отмены.
Для этой функции пересоберите локальный FFmpeg скриптом `scripts/build-animation-decoder.sh`
(добавлены AAC encoder и MP4 muxer). H.264 encoder minih264 включён в исходники с лицензией CC0.

Проверки логики записи, маски и запроса TDLib: `bash tests/run-video-tests.sh`.
Полная проверка MP4 с декодированием описана в [tests/README.md](tests/README.md).
Работу камер, микрофона и скорость подготовки файла на физической Vita нужно проверять отдельно.

## MTProxy

Ссылки `tg://proxy?server=…&port=…&secret=…` и `https://t.me/proxy?…` в сообщениях открывают предложение настроить MTProxy.
Откройте сообщение и нажмите на ссылку (или Triangle на выбранной строке ссылки).
Поля заполняются автоматически; подключение меняется только после «Сохранить и применить».
Circle отменяет импорт и возвращает к сообщению.

В **Start → MTProxy → Сканировать QR-код** наведите **заднюю (основную) камеру** на QR-код с такой ссылкой.
После распознавания камера выключится, а приложение покажет те же поля для подтверждения.
Circle закрывает сканер. Неподходящие QR-коды не открывают браузер и не меняют настройки.
Кадры обрабатываются на устройстве, не сохраняются и не отправляются в сеть.
Сканирование доступно и до авторизации; для распознавания используется встроенная библиотека quirc (ISC).

В главном меню нажмите **Start → MTProxy** (доступно также до входа в аккаунт).
Укажите сервер (адрес или имя хоста без `https://`), порт и secret из параметров вашего MTProxy.
Включите «Использовать MTProxy» и выберите «Сохранить и применить».
Для прямого подключения отключите этот переключатель и снова примените настройки.
Кнопка Circle возвращает назад без сохранения новых изменений.

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
