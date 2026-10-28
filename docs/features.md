# Features / Возможности

[← VitaGram](../README.md)

## English

### Screenshots

| Chats | Conversation | Video |
| --- | --- | --- |
| ![Chat list](../media/readme/chats-en.webp) | ![Messages and stickers](../media/readme/conversation-en.webp) | ![Video playback](../media/readme/media-en.webp) |

*English text in these illustrations was translated from the original Russian screenshots.*

### Features

- **Custom theme packs:** install one theme or a whole pack at once: extract/copy
  all theme `.json` files directly into `ux0:data/vitagram/palletes/`, restart,
  and select a theme by filename in Settings. Up to 64 valid files are loaded;
  subfolders, ZIP archives and single colour-catalogue JSON files are not imported. Use [Palette Studio in your browser](https://ing156.github.io/VitaGram/) to edit
  colors, preview the interface and download a JSON theme. The studio supports all 19 app languages. An
  [offline HTML edition and publishing instructions](../tools/palette-editor/README.md)
  are also available. The web address becomes available after GitHub Pages is enabled.

- **Chats:** private conversations, groups, channels, Telegram folders, archive,
  search, forum topics and channel comments. Chat lists, history and media are cached.
- **Opening chats:** Settings → Chats → When opening a chat selects the latest message, the synchronized Telegram read position, or the last message read on this Vita (default: latest). Read positions load before showing the message. Loading offers cancellation/retry; distant positions show a memory warning, then load a bounded history window with browsing in both directions. Local positions are separate for chats, forum topics and comments. The synchronized mode uses the newer of the Telegram and device read positions, so reopening never moves behind progress already read on this Vita.
- **Bots:** a command menu appears only when commands or a Mini App menu button are provided by the bot. Text reply keyboards, inline callback/URL buttons and callback alerts are supported. Tap buttons directly; Select opens the full button panel, L/R switches its sections, and Left/Right changes pages. Command lists slide up from the bottom; selecting a command or text reply sends a message. Commands, text replies and callback buttons move focus to the latest message; new bot replies remain visible while the user stays at the bottom. A bot menu containing only a Mini App invokes its launch directly and uses the bot-provided title. The panel follows the current palette and font size. The experimental [WebKit viewer](../docs/webkit-vita.md) is a separately downloaded, signed add-on: the base VPK contains no engine. Opening a Mini App checks for a compatible add-on and offers its download size before installation. Ordinary bot buttons need no add-on. Software rendering and frame/IPC buffers stay in RAM, including with storage encryption. Loading the module and Mini Apps still require device testing; remote installation requires the separately published signed add-on release. Buttons requesting personal data, payments or other special actions are not yet supported; they do not silently send text or personal data.
- **Messages:** consecutive messages from the same sender share compact spacing and connected corners regardless of the time gap; the sender header appears once, the tail only on the last bubble, and each message keeps its own timestamp and focus outline. A new day or service message breaks the group. Bold, italic, underline, strikethrough, code/quote highlighting and text spoilers (open the message, then press Cross or tap the hidden line to confirm reveal). Text, replies, forwarding, permission-aware deletion, standard emoji
  reactions (hold Cross for 550 ms on a message; frequent reactions occupy the first row) and voting in polls, including quizzes and multiple-choice polls.
  Links with Telegram preview data show a card with the site, title, description and image.
- **Photos and files:** the Multimedia attachment option selects photos and videos together; albums, attachment batches of up to 10 items, fullscreen
  photos with rotation and 1–5× zoom (touch or right stick up/down), left-stick panning
  while zoomed, and saving files to a download folder. Attachment review offers
  Add text / Edit text; the shared text is sent once as a file or album caption.
- **Stickers and GIFs:** static and animated stickers, recent/favorite/installed
  sticker sets, saved GIFs and GIF search. In the sticker or GIF grid, hold Cross or
  touch an item for 550 ms to preview it. Animations start automatically and loop;
  release to close without sending. A short press sends the item.
  Open a sticker/custom emoji set from the right menu or hold the item in a message.
  In a sticker preview, slide to the Open set button and release. Uninstalled sets
  offer Add set (Triangle or touch).
- **Audio and video:** supported media playback, progressive playback while downloading,
  voice-message recording and video notes from the front or rear camera.
  Voice, audio and video-note playback continues on other screens: a Telegram-style control bar spans the bottom of the conversation pane, reserving space above it for messages and chat actions, and a video note also floats as a small circle you can drag anywhere and tap to pause. Audio controls include play/pause, draggable seeking within the downloaded part, volume/mute and pitch-preserving 0.5–2× speed. Music adds previous/next, shuffle and repeat off/list/track. Tap the speaker for the volume slider; tap its speaker to mute/restore. L+R focuses the player, Left/Right selects a control, Cross activates it, Up/Down adjusts volume/speed or seeks 10 seconds, and Circle returns to the chat. Previous restarts after three seconds, otherwise selects the preceding track. Up to 64 loaded music tracks around the chosen message form a playlist that survives changing chats. Player controls use the active palette; queue controls and speed are disabled for video notes (volume, pause and seeking remain available). Playback settings last for the app session. Music tracks of a chat play as a playlist: the next track starts when one ends and keeps downloading in the background. Music continues when another chat is opened or another voice message is selected; only pressing play on another message replaces it. Music and video with sound (clips and video notes) share one audio output: starting one pauses the other.
  Fullscreen video: Left/Right selects 10-second seek steps; holding repeats faster
  after two and four seconds. The time and cursor update while held, and releasing
  applies the target and restores the prior play/pause state. Tap or drag the
  progress track to seek, including while the controls are hidden.
- **User colors:** Settings → Interface → Use user colors in the interface (enabled by default). Telegram colors are used for sender names, empty avatars, forwards, link previews and reply headers; replies use the original author’s color.
- **Split keyboard:** spread two fingers horizontally across the built-in keyboard to move the keys towards the edges; pinch them inward to restore the full layout. Settings → Controls and input → Keyboard → Keyboard spread selects Small (the original spacing), Medium (default), or Wide. The choice survives restarts. Theme colors and the wide spacebar are preserved.
- **Display mode:** Settings → Interface → Display mode switches between Classic (default) and Combined. Combined keeps a compact, single-line chat list on the left, history on the right and folders hidden behind a 12 px strip at the left edge. Swipe the strip right or tap it to reveal folders with a 180 ms slide that pushes only the content below the fixed top header without shrinking text. With folders folded, all screens have zero translation; only the chat list reserves space for the edge strip, keeping the message pane and right edge in their fixed bounds. Swipe left, tap outside or use the return control to close; tap a folder to select it and close. Long folder lists scroll vertically. From the focused chat list, Left opens folders; Up/Down selects, Cross confirms and Right/Circle closes. With folders hidden, the top header shows the active folder name and updates when switching with L/R. Classic retains its top folder tabs. Search, attachment selection, stickers, GIFs, reactions and recording open in separate windows over the Combined layout, keeping the background chat geometry and text size. Tool windows have a return button and own input; chat actions beneath them are disabled. A touch drag scrolls the pane where it began, independently of controller focus, and keeps that pane until release. Left or Circle focuses the chat list; Right returns to history; Cross opens the selected chat. Start opens settings while the chat list has focus. Touch a chat to switch directly. Chat loading/retry, forum topics, context menus and individual posts stay inside the message pane. Post text wraps to its width without scaling the font; media controls and touch targets follow the same geometry. Explicit fullscreen photo and video viewers use the whole display. Combined mode hides and disables the bottom-left attachment corner across chat tools; hold Square to choose an attachment. The choice survives restarts.
- **Interface:** native Vita keyboard or the localized in-app keyboard
  (Settings → Controls and input → Keyboard → Keyboard type), Cyrillic and color emoji, Light/Dark/VHBB themes, font size
  and interface language settings. Hold D-pad up/down or the left stick to scroll
  lists with acceleration. Settings → Controls and input → Scroll speed selects Normal,
  Fast or Very fast: up to 1, 2 or 3 pages per 0.5 seconds. Normal is the default.
  Dark is the initial theme. In a chat, tap Square to compose text; hold it for
  250 ms to open the radial menu. The wheel slides in and unfolds over 350 ms.
  The left stick selects Multimedia (up), GIFs (upper right), File (lower right),
  Voice (lower left) or Stickers (upper left); neutral selects text. While holding,
  either L or R toggles between a new message and a reply to the captured message.
  Both controls stay visible in the center. The 496 px wheel uses rounded glass
  petals, blue/theme-accent glow, icons and detail captions, with a 216 px center.
  Release Square to confirm after a 180 ms closing animation, or Circle to cancel.
  Reply context follows text, stickers, GIFs, voice and attachment batches until
  sending or cancellation. The in-app message window supports line breaks and
  sending with Send or Start; the system keyboard remains the default.
  Hold a key for 300 ms to open alternatives, slide to a choice and release to insert.
  Square deletes before the cursor, Triangle inserts a space, and L/R move the cursor.
  Hold Square, the on-screen Delete key, or Cross on Delete to repeat deletion
  after 350 ms, then every 60 ms. Releasing or leaving the key stops it.
  With buttons, hold Cross and select with Left/Right; Circle cancels the choice.
  Russian е offers ё and 4, ь offers ъ (also in uppercase); numbers 1–0 align to ц–х.
  Comma/period beside the spacebar offer punctuation choices; ?123 shows all symbols.
  Shift applies to one entered character; double-tap within 350 ms locks Caps Lock,
  and another tap unlocks it. Messages start with a capital and capitalize after
  sentence-ending punctuation plus whitespace or a newline. Search, credentials
  and other exact-input fields do not use automatic capitalization.
  Settings → Controls and input → Keyboard → Input languages selects the enabled languages. The default
  is English plus the supported console language, or English alone. The language
  key cycles enabled languages on a short press. Hold it for 300 ms to choose
  from a popup carousel, slide or use Left/Right while holding Cross, then release.
  Large sets scroll at the popup edges; Circle cancels selection.
  The 15 Latin/Cyrillic profiles use language-specific
  layouts, accents and case rules. Japanese, Korean and both Chinese variants use
  the system IME for composition, preserving the current draft. The initial profile
  follows the console language when enabled; an explicit choice is remembered until
  the app closes. Input-language preferences survive restarts independently of the UI language.
  Vault passphrases always use the in-app keyboard; controller hints are placed
  directly on its keys. See [keyboard layouts](../docs/keyboard-layouts.md).
- **Connection and privacy:** MTProxy, SOCKS5 and HTTP CONNECT proxies, proxy-link
  and QR import, optional data encryption and automatic screen locking.
- **Updates:** signed VPK packages from GitHub Releases, with manual and automatic
  update checks.

### Custom fonts

Copy `.ttf` or `.otf` files directly into `ux0:data/vitagram/fonts/` (up to 8 MiB per file,
64 entries), then choose **Settings → Interface → Font**. The folder is scanned at startup and each
opening of the picker; no restart is needed to discover newly copied files. Only the selected
font is loaded, in the background when selected. The filename is saved for the next launch.
Built-in fonts supply missing characters; emoji and controller icons keep their own renderer.
Choose **Default** to restore the built-in fonts. Unreadable, corrupt, bitmap-only or
non-Unicode fonts cannot replace the current font. A missing or invalid saved font falls
back to the default at startup. Font files remain ordinary, unencrypted user resources.

### Limitations

- Media support is bounded by codecs, resolution and file size. The player supports
  H.264, VP8 and VP9; H.265/AV1 decoders are not included. The playback menu → Quality selects a compatible Telegram variant for the current video.
  Settings → Data and storage → Maximum video quality limits the choices to Minimum (default), up to 360p,
  480p or 720p. Switching quality keeps the position and play/pause state; fallback video limits are
  720p by pixel area and 1 GiB. Video is read incrementally; the size limit reserves headroom for Vita’s file offsets, including encrypted-file overhead.
- Progressive audio/video can start after a readable 256 KiB prefix, but some MP4
  files need a complete download. While a video streams, seeking is limited to the
  already downloaded part; portrait-video preparation requires the completed file,
  and prepared portrait copies have additional seek restrictions.
- Calls, secret chats, group administration, paid/custom reactions, creating polls
  or editing forum topics, and searching for sticker sets are not implemented.
- Presence follows Telegram privacy settings. Sticker-set and topic lists still
  need Telegram responses; cached previews do not provide fully offline menus.

## Русский

### Скриншоты

| Чаты | Переписка | Видео |
| --- | --- | --- |
| ![Список чатов](../media/readme/chats.webp) | ![Сообщения и стикеры](../media/readme/conversation.webp) | ![Воспроизведение видео](../media/readme/media.webp) |

### Возможности

- **Свои темы и наборы:** можно установить одну тему или сразу набор:
  распакуйте/скопируйте все файлы тем `.json` прямо в `ux0:data/vitagram/palletes/`,
  перезапустите приложение и выберите тему по имени файла в настройках.
  Загружаются до 64 корректных файлов; вложенные папки, ZIP и единый JSON-каталог
  цветов не импортируются. [Откройте мастер дизайна в браузере](https://ing156.github.io/VitaGram/),
  настройте цвета, проверьте предпросмотр и скачайте JSON темы. Мастер поддерживает
  все 19 языков приложения. Доступны также
  [автономный HTML и инструкция публикации](../tools/palette-editor/README.md).
  Веб-адрес заработает после включения GitHub Pages.

- **Чаты:** личная переписка, группы, каналы, папки Telegram, архив, поиск,
  темы форумных групп и комментарии каналов. Списки чатов, история и медиа кэшируются.
- **Открытие чатов:** Настройки → Чаты → При открытии окна чата: последнее сообщение, общая отметка прочитанного Telegram или последнее прочитанное на этой Vita (по умолчанию — последнее сообщение). Чат открывается после загрузки нужной позиции; во время ожидания доступна отмена, при ошибке — повтор. Для далёкой позиции показывается предупреждение о памяти, после закрытия которого загружается ограниченный участок истории с перелистыванием в обе стороны. Локальные отметки хранятся отдельно для чатов, тем форума и комментариев. В режиме общей отметки Telegram выбирается более новая из отметок Telegram и устройства, чтобы при повторном открытии не возвращаться за уже прочитанное на Vita.
- **Боты:** меню появляется только при наличии команд или кнопки мини-приложения от бота. Поддерживаются текстовые ответные клавиатуры, inline callback/URL-кнопки и уведомления в ответ на нажатие. Кнопки доступны касанием; Select открывает полную панель, L/R переключает разделы, влево/вправо — страницы. Список команд выезжает снизу; выбор команды или текстовой ответной кнопки отправляет сообщение. После команды, текстовой или callback-кнопки фокус переходит к последнему сообщению; новые ответы бота остаются видны, пока пользователь находится внизу чата. Единственная кнопка мини-приложения вызывает запуск сразу и использует название от бота. Панель следует текущей палитре и размеру шрифта. Экспериментальный [WebKit](../docs/webkit-vita.md) вынесен в отдельно скачиваемое подписанное дополнение: основной VPK не содержит движок. При открытии мини-приложения клиент проверяет совместимость и предлагает скачать дополнение с указанием размера. Обычные кнопки ботов обходятся без него. Программная отрисовка и кадры/IPC остаются в RAM, в том числе при шифровании. Загрузка модуля и Mini Apps ещё требуют проверки на Vita; для сетевой установки нужен отдельно опубликованный подписанный выпуск дополнения. Запросы персональных данных, платежи и другие специальные действия пока не поддерживаются; такие кнопки не отправляют вместо этого текст или персональные данные.
- **Сообщения:** жирный текст, курсив, подчёркивание, зачёркивание, выделение кода/цитат и текстовые спойлеры (откройте сообщение и нажмите × либо коснитесь скрытой строки для подтверждения). Текст, ответы, пересылка, удаление с проверкой прав, обычные
  emoji-реакции (удерживайте × на сообщении 550 мс; частые реакции — в первой полосе) и голосование в опросах, включая викторины и множественный выбор.
  Ссылки с данными предпросмотра от Telegram показывают карточку сайта с заголовком, описанием и изображением.
- **Фото и файлы:** единый пункт «Мультимедиа» для выбора фото и видео; альбомы, отправка до 10 вложений за раз, полноэкранные фото
  с поворотом и масштабом 100–500% (жестом или правым стиком вверх/вниз), перемещением
  левым стиком при увеличении, сохранение файлов в папку загрузок. Перед отправкой
  вложений пункт «Добавить текст» / «Изменить текст» открывает редактор; общий текст
  отправляется один раз как подпись к файлу или альбому.
- **Стикеры и GIF:** статичные и анимированные стикеры, недавние, избранные
  и установленные наборы, сохранённые GIF и поиск анимаций. В сетке стикеров или GIF
  удерживайте × или касание элемента 550 мс для предпросмотра. Анимация запускается
  автоматически и повторяется; отпускание закрывает окно без отправки.
  Короткое нажатие отправляет выбранный элемент. Набор стикера или пользовательского
  эмодзи открывается из правого меню или удержанием в сообщении. В предпросмотре
  стикера сдвиньте палец на кнопку открытия набора и отпустите. Неустановленный
  набор можно добавить кнопкой △ или касанием.
- **Аудио и видео:** воспроизведение поддерживаемых медиа, просмотр во время
  загрузки, запись голоса и видеокружков с фронтальной или задней камеры.
  Голос, аудио и видеокружки продолжают играть на других экранах: внизу области переписки появляется панель в стиле Telegram; сообщения и кнопки чата располагаются выше неё, а кружок ещё и отображается маленьким кругом, который можно перетащить в любое место и коснуться для паузы. Аудиопанель предлагает пуск/паузу, перемотку касанием или перетаскиванием в пределах загруженной части, громкость с отключением звука и скорость 0,5–2× без изменения высоты голоса. Для музыки доступны предыдущий/следующий трек, перемешивание и повтор: выключен / список / трек. Касание динамика открывает ползунок громкости; динамик в открытой панели отключает и восстанавливает звук. L+R переводит фокус на плеер; влево/вправо выбирает действие, крестик выполняет его, вверх/вниз меняет громкость/скорость либо перематывает на 10 секунд; круг возвращает управление чату. Предыдущий трек после трёх секунд сначала возвращает в начало. Очередь содержит до 64 загруженных в историю музыкальных сообщений вокруг выбранного и сохраняется при смене чата. Палитра плеера следует теме. У видеокружков недоступны музыкальная очередь и скорость; громкость, пауза и перемотка сохраняются. Настройки воспроизведения действуют до закрытия приложения. Музыка чата играет как плейлист: следующий трек стартует после окончания предыдущего и догружается в фоне. Музыка не прерывается при переходе в другой чат или выборе другого голосового сообщения; её заменяет только запуск другого. Музыка и видео со звуком (ролики и кружки) делят один звуковой выход: запуск одного ставит на паузу другое.
- **Цвета пользователей:** переключатель «Учитывать цвета пользователей при построении интерфейса» расположен в «Настройки → Интерфейс» сразу под темой и включён по умолчанию. Цвета Telegram используются для имён, аватаров без фото, пересылок, предпросмотра ссылок и цитат ответов. Цитата использует цвет автора исходного сообщения.
- **Раздельная клавиатура:** разведите два пальца по горизонтали на встроенной клавиатуре, чтобы сдвинуть клавиши к краям. Сведение возвращает обычную раскладку. В «Настройки → Управление и ввод → Клавиатура → Раздвигание клавиатуры» доступны небольшое (прежняя геометрия), среднее (по умолчанию) и широкое раздвигание; выбор сохраняется после перезапуска. Цвета темы и широкий пробел сохраняются.
- **Интерфейс:** системная клавиатура Vita или локализованная встроенная клавиатура
  («Настройки → Управление и ввод → Клавиатура»), кириллица и цветные emoji, темы Светлая/Тёмная/VHBB,
  настройка размера шрифта и языка. Удержание стрелок вверх/вниз или левого стика прокручивает списки с ускорением. «Настройки → Управление и ввод → Скорость прокрутки»:
  «Норма», «Быстрая» и «Очень быстрая» — до 1, 2 и 3 страниц за 0,5 секунды.
  По умолчанию выбрана «Норма», тема тёмная. В чате короткое □ открывает текст;
  удержание 250 мс — радиальное меню с выездом и раскрытием за 350 мс.
  Левый стик выбирает «Мультимедиа» сверху, GIF справа сверху, «Файл» справа снизу,
  «Голосовое» слева снизу или «Стикеры» слева сверху; нейтральное положение — текст.
  При удержании любая из кнопок L/R переключает «Новое ↔ Ответ» на зафиксированное
  сообщение; обе подсказки постоянно видны в центре. Колесо диаметром 496 px
  использует скруглённые стеклянные лепестки, свечение цвета акцента темы,
  иконки и вторые строки подписей; диаметр центральной области — 216 px.
  Отпустите □ для подтверждения и закрытия за 180 мс; ○ отменяет выбор.
  Контекст ответа сохраняется для текста, стикеров, GIF, голоса и вложений до
  отправки или отмены. Встроенное окно сообщения поддерживает
  переносы строк и отправку кнопкой «Отправить» или Start.
  По умолчанию клавиатура системная. Удержание клавиши 300 мс открывает варианты:
  сдвиньте палец к нужному и отпустите. □ удаляет символ перед курсором,
  △ вставляет пробел, L/R перемещают курсор. Удержание □, экранной клавиши
  удаления или × на ней повторяет удаление через 350 мс, затем каждые 60 мс.
  Отпускание или уход с клавиши прекращает удаление. На кнопках — удерживайте × и выбирайте
  стрелками влево/вправо; ○ отменяет выбор. У «е» доступны «ё» и «4», у «ь» — «ъ»,
  в том числе в верхнем регистре. Цифры 1–0 расположены на «ц»–«х».
  Запятая и точка рядом с пробелом также предлагают варианты; все символы — в ?123.
  Shift действует на один введённый символ; двойное нажатие за 350 мс включает
  Caps Lock, следующее отключает. В сообщениях заглавная буква ставится в начале,
  после конца предложения с пробелом и после переноса строки. Поиск, пароли и
  остальные поля точного ввода используют ручной регистр.
  В «Настройки → Управление и ввод → Клавиатура» выбираются тип клавиатуры и языки ввода. По умолчанию
  включены английский и поддерживаемый язык системы, иначе — только английский.
  Короткое нажатие клавиши языка переключает на следующий включённый язык.
  Удержание 300 мс открывает карусель: сдвиньте палец или выбирайте стрелками
  влево/вправо при зажатой ×, затем отпустите; ○ отменяет выбор. Длинный список
  прокручивается у краёв панели. Для 15 языков на латинице и кириллице доступны
  свои раскладки, диакритика и правила регистра. Японский, корейский и оба китайских
  варианта используют системный ввод с сохранением набранного текста. Начальная
  раскладка соответствует языку системы, если он включён; ручной выбор запоминается
  до закрытия приложения. Список языков сохраняется между запусками независимо от языка интерфейса.
  Пароли хранилища всегда вводятся встроенной клавиатурой; подсказки управления
  расположены прямо на её клавишах. [Список раскладок](../docs/keyboard-layouts.md).
- **Подключение и приватность:** MTProxy, SOCKS5 и HTTP CONNECT, импорт прокси
  по ссылке и QR-коду, необязательное шифрование данных и автоблокировка экрана.
- **Обновления:** подписанные VPK из GitHub Releases, ручная и автоматическая проверка.

### Собственные шрифты

Скопируйте `.ttf` или `.otf` непосредственно в `ux0:data/vitagram/fonts/` (до 8 МиБ на
файл, до 64 вариантов), затем выберите **Настройки → Интерфейс → Шрифт**. Папка сканируется при
запуске и каждом открытии списка: новые файлы доступны без перезапуска. При выборе
шрифт загружается в фоне; имя выбранного файла сохраняется для следующего запуска.
Недостающие символы берутся из встроенных шрифтов; emoji и значки кнопок сохраняют
свою отрисовку. Вариант **Стандартный** возвращает встроенные шрифты. Ошибочный,
нечитаемый, растровый или не поддерживающий Unicode файл не заменяет рабочий шрифт.
Если сохранённый файл удалён или повреждён, при запуске используется стандартный.
Файлы шрифтов остаются обычными пользовательскими ресурсами без шифрования.

### Ограничения

- Поддержка медиа зависит от кодека, разрешения и размера. Есть H.264, VP8 и VP9;
  декодеры H.265/AV1 не включены. В меню проигрывания → Качество выбирается вариант текущего видео.
  Настройки → Данные и память → Максимальное качество видео ограничивает выбор: минимальное (по умолчанию),
  до 360p, 480p или 720p. Смена качества сохраняет позицию и состояние воспроизведения; при его отсутствии действуют лимиты исходника: 720p по площади
  кадра и 1 ГиБ. Видео читается по частям; предел размера оставляет запас для файловых смещений Vita, включая накладные расходы шифрования.
- Просмотр видео/аудио может начаться после непрерывных 256 КиБ от начала файла,
  но некоторым MP4 нужна полная загрузка. Во время потокового просмотра перемотка
  доступна в пределах уже загруженной части; подготовка вертикального видео требует
  докачки, у подготовленных вертикальных копий есть дополнительные ограничения перемотки.
- В полноэкранном видео влево/вправо выбирает позицию с шагом 10 секунд. Удержание
  ускоряет повтор через 2 и 4 секунды. Отметка и время обновляются при удержании,
  переход выполняется после отпускания с сохранением прежнего режима воспроизведения.
  Касание или перетаскивание по шкале тоже перематывает, даже если панель скрыта.
- Не реализованы звонки, секретные чаты, управление группами, платные/пользовательские
  реакции, создание опросов, редактирование тем форума и поиск наборов стикеров.
- Статусы присутствия зависят от приватности Telegram. Списки наборов стикеров
  и тем требуют ответа сервера; кэш миниатюр не делает эти меню полностью офлайн.

### Settings navigation / Навигация по настройкам

Settings now has eight sections: Interface, Chats, Controls and input, Data and
storage, Connection, Data protection, Updates and installation, and About and
diagnostics. Log out is a separate final row. Cache limits and cleanup are directly
in Data and storage. Circle or the on-screen Back control returns to the previous
menu and restores its selection and scroll position.

В настройках восемь разделов: «Интерфейс», «Чаты», «Управление и ввод»,
«Данные и память», «Подключение», «Защита данных», «Обновления и установка»,
«О приложении и диагностика». Выход из аккаунта — отдельная последняя строка.
Лимиты и очистка кэша находятся прямо в «Данные и память». Круг или экранная
кнопка «Назад» возвращает в предыдущее меню с восстановлением выбора и прокрутки.

### Переход к дате / Jump to date

Дата первого видимого сообщения закреплена над перепиской. Нажмите на неё или на разделитель дней, чтобы открыть календарь. L/R меняют месяц, стрелки выбирают день, {cross} переходит к последнему доступному сообщению не позднее выбранного дня, {circle} закрывает календарь. Можно нажать день на экране. Будущие даты недоступны; начало календаря — август 2013 года. Загружается небольшое окно истории, а не вся переписка. Доступ к удалённым сообщениям и скрытой истории определяется Telegram. В темах и комментариях поиск ограничен текущим обсуждением.

Tap the floating date or a day separator to open the themed calendar. L/R change months, the D-pad selects a day, Cross jumps and Circle returns. Future days are disabled; the calendar starts in August 2013. A bounded history window opens at the last available message no later than the selected day; Telegram controls history availability. Topics and comments keep their current scope.
