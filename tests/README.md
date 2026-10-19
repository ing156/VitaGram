# Native checks

## Что хранится в tests

- `*_test.cpp`, `*_test.js`, `*_test.py` — код проверок; `run-*.sh` — запуск.
- `*-stubs/`, `stubs/`, `test_language.h` — необходимые заглушки API Vita для ПК.
- [`fixtures/`](fixtures/README.md) — текстовые входные данные, нужные тестам.
  Это не ресурсы приложения; они не включаются в VPK.
- `local/` — личные входные файлы и результаты диагностики, исключённые из Git:
  `media/` (фото, видео, снимки экрана), `logs/`, `dumps/`, `output/`.
  Старые файлы из корня перенесены сюда без изменения содержимого.

Большинство проверок создаёт временные данные в `/tmp` и удаляет их после запуска.
Локальное видео для `run-download-stream-tests.sh` теперь лежит в `local/media/`;
в чистом checkout передайте свой H.264/AAC MP4 аргументом:
`bash tests/run-download-stream-tests.sh /absolute/path/to/video.mp4`.
Аудио для `run-audio-startup-tests.sh` также передаётся аргументами.
Мультимедийные образцы в Git не включаются. Для воспроизводимых проверок
оставляйте необходимые текстовые образцы в `fixtures/`,
а не комментируйте тесты; новый образец нужно явно разрешить в `.gitignore`.


Audio startup diagnostics: enable **Start → Diagnostic logs → Performance** before
opening a music/voice message. Test both Play during downloading and Play on an
already downloaded file; let the sound start (or wait through the delay), then
close playback and allow at least five seconds for the background log writer.
Collect `ux0:data/vitagram/performance.log` and `performance.previous.log` if present.
The `audio_startup` lines correlate `player` and `generation`, with `at_us` on the
same clock as other performance events. UI download/button events use generation 0
and include Telegram's local `file_id`; filenames and media contents are omitted.
`total_us` measures from decode entry, not from Play; subtract `at_us` of `play`
from `first_output_begin` for Play-to-first-submission latency. Pauses/resumes and
ignored Play presses are logged separately. Paired `*_begin`/`*_end` timestamps
separate file attachment, container opening, stream probing, codec initialization,
seeking, audio-port opening, PCM conversion and the first blocking AudioOut call.
First-frame `result` is the sample rate, `decoded_channels`/`decoded_sample_format`
report the decoded layout/format, and `codec_open_begin` reports FFmpeg's codec ID;
other operation-end results retain the API return value. Output timestamps measure
API submission/return, not physical speaker latency. A missing end marker identifies
an unfinished stage; check `audio_startup_events dropped` before drawing conclusions.
`bash tests/run-performance-tests.sh` checks gating, chronology, progress snapshots,
bounded storage, nonblocking producers and background-log integration in ON/OFF builds.

`bash tests/run-audio-startup-tests.sh /path/to/music.mp3 /path/to/music.m4a`
uses native FFmpeg (same setup as the download-stream tests; override
`FFMPEG_NATIVE_BUILD` if needed). Supply audio fixtures larger than 256 KiB.
It checks full decoding/resampling without `avformat_find_stream_info`, matching
PCM against the analyzed baseline and for ordinary/completed-download reads, the first audio block from a
256 KiB prefix before download completion, and a fresh 30-second seek for tracks
longer than 31 seconds. Both reported MP3/M4A files in `release/` pass these checks.
Music now skips stream analysis, so new logs go from `open_input_end` to
`codec_open_begin` without `stream_info_begin`/`stream_info_end`.

`bash tests/run-gxm-guard-tests.sh` checks failed vertex/fragment uniform reservations,
stale output pointers, scene and uniform-write failures, skipped GPU draws and
recovery at the next successful scene. Uses installed VitaSDK headers (set `VITASDK`
if needed), with GXM calls mocked; actual GPU recovery requires a PS Vita check.

`python3 scripts/build-palette-editor.py && node tests/palette_editor_test.js`
checks the offline Windows/Linux editor bundle, JSON/name limits, duplicate keys,
all exported patterns, contrast and palette round trips. Optional Playwright UI
checks cover import/download, Unicode filenames, catalogue selection, all preview
screens and battery states, local catalogue import/replacement and invalid-file
atomicity, and zero external HTTP requests; see
[editor instructions](../tools/palette-editor/README.md).
Border checks cover the unified color list, preserving manual/imported
`border` and `focus` through auto shading, and JSON export/import. Native skin
checks verify that both frame colors change without altering panel centers,
alpha coverage, file reads or texture allocations.
Preview checks exercise eight native views and verify actual draw coordinates:
the five folder tabs, nine 43 px chat rows, status strip, settings viewport,
theme picker and both corner menus. The bundle must contain the unchanged Noto
font and constants from `src/ui_layout.h`; browser tests verify font sizing,
opacity calibration and controller sprites while retaining zero HTTP requests.
History checks include a reply to a visible message: the native date-chip quote,
sender, source text, accent bar, alpha, expanded card and shifted following rows
are checked at 120% and 130% font sizes.
Semantic color checks cover icon/text independence, recolored notification pixels,
active/inactive radio controls in the poll preview, delivery strokes, old-palette
fallbacks, and custom-role presence bits beyond bit 31.

The mask suite also exercises custom theme discovery from
`ux0:data/vitagram/palletes/`: strict bounded JSON, UTF-8 filenames, symlink and
directory rejection, ordering, the 64-theme cap, warm switches without reads or
texture allocation, and separation from the legacy single-palette override.
Vault sweep checks round-trip custom selections by filename, including pre-unlock
hints, reordered numeric IDs and missing-file fallback.

`bash tests/run-ui-mask-tests.sh` runs the real skin loader against PNG masks with
host Vita API stubs and libpng. It checks alpha composition, strict/atomic JSON
palette validation, recoloring without file reads or texture allocations, live
same-size file changes, preset switches, invalid-file fallback, shared battery
sprites and resource cleanup. Address/undefined sanitizers are enabled; system
libpng and Jansson development headers from build-modern are required.
The named presets are loaded from their packaged JSON files; the legacy
override file must not replace them. Vault sweep tests round-trip all sixteen theme
IDs through settings.json and the pre-unlock theme hint, checking light/dark mode.
Mask tests compare the optimized compositor byte-for-byte with the original
formula, report host timings, and verify picker wrapping, scrolling and touch boundaries.
Theme-hint tests cover legacy single digits, two-digit IDs, newlines, invalid
suffixes, overflow and out-of-range values.
Every warm preset switch and swatch lookup must perform zero file reads and zero
GPU texture allocations; VHBB transitions still exercise full loading and cleanup.
Skin checks also inspect rendered corner glyphs and all six battery states for
every masked preset. Battery tones must contrast at least 3:1 with both header
endpoints; live changes to white, black, gray and blue headers repeat the checks
without reads or allocations. Corner ink must contrast at least 4.5:1 with its surface.

`python tests/theme_composition_test.py` checks catalogue color codes, malformed
compositions, contrast rejection, portable recipes, and actual light/dark PNG
generation with the existing UI template. It also checks palette-only exports,
white/alpha mask layers and original icon coverage. It verifies dimensions, nine-slice
insets, atlas coordinates, icon alpha and refusal to overwrite an existing pack.
Use a Python environment with Pillow and CairoSVG; no Vita build or local paint
catalogue is needed. See `assets/VitaGram_UI_Assets_v1/README_RU.md` for usage.

`bash tests/run-scroll-tests.sh` checks held-arrow acceleration, direction and menu
changes, release and resume cancellation, frame stalls, and the limit of one page
per 0.5 seconds in Normal mode across different viewport sizes and frame rates.
All three speed presets are checked against their selected limits of 2, 4 and 6
pages per second. Vault checks verify saved scroll speeds and default fallback.
It also checks left-stick dead-zone hysteresis, D-pad priority, source changes,
and matching acceleration for stick and button holds.

Cache regressions for 0.33.7 block an actual cache read and apply live chat/user
updates and message deletion before it resumes. They also clear the account during
the read and require the late result to be discarded. Old startup indices and
indices without chat positions must trigger rebuilding from the main cache.

Startup checks for 0.33.6 verify the separate lightweight index, atomic interrupted
writes, corrupt-index fallback, incremental UI adoption and live updates between
slices. The transfer harness blocks the full cache read on the real Storage worker
and requires the initial list to appear before releasing it. Offline demand checks
cover history, audio paths, visible avatars, stickers and stale session IDs.

`bash tests/run-media-cache-tests.sh` checks the bounded asynchronous file-validation
queue, replacement and invalidation during stalled stat, duplicate work and overflow.
Transfer tests run the real parser and Storage worker with blocked file I/O: 1002
events, a chat and a new message must reach the UI before storage resumes; a late
file-only result must restore the avatar. These are host regressions, not Vita timings.

`bash tests/run-media-trash-tests.sh` checks the one-time swap of bloated media
directories before TDLib starts, the batched background removal of the old copies,
and flagging a directory above its entry limit for the next start.

`bash tests/run-cache-tests.sh` verifies immutable cache snapshots across chat, poll,
message, folder and sticker mutations; UI updates during a deliberately stalled writer;
interrupted saves preserving the previous file; size limits; and the deliberate exit gesture
that excludes Select-first screenshot input. Transfer checks also exercise continuous-input fairness.

`bash tests/run-performance-tests.sh` checks histogram percentile bounds, interval resets,
queue high-water marks, concurrent accounting including dropped observations, file-event
coalescing byte accounting, log rotation/write failure and the disabled collector build.
Transfer tests additionally verify byte counters against the real worker/parser queues.
These tests do not measure device performance or validate the Vita system counters.

`bash tests/run-history-tests.sh` checks stable selection in mixed photo/video albums,
navigation in both directions, prepended history pages, photo-album merges and deletions.
It uses the application message type and selection code; a configured build-modern
Jansson include directory is required.

`bash tests/run-i18n-tests.sh` checks that every language catalog has the same keys, placeholders and icon tokens, language selection and
English fallback, the `.lang` file format, and that all `tr("key")` keys used in `src/` exist. Tests that compare Russian texts
select it explicitly through `tests/test_language.h`.

`bash tests/run-proxy-tests.sh` checks proxy URLs, real QR decoding and mocked Vita camera lifecycle.
It includes Telegram-style rounded finder rings and data modules with a central
logo, mirrored frames, rotation, perspective and reduced contrast. The styled
renderer follows the radii in Telegram Android's `TelegramQRCodeWriter`;
it is a synthetic regression, not a physical-camera test.

`bash tests/run-video-tests.sh` checks circular YUV cropping, the current TDLib
`inputMessageVideoNote` payload, camera switching, microphone failure, cancellation,
the 60-second recording limit and transfer of file ownership to the upload caller.
It checks bounded video/audio queues, slot ownership during a stalled write, overflow,
draining and abort. A paced 30 FPS camera and 32 ms microphone continue capture during
a two-second disk stall; encoding must wait for the final flush. Injected fwrite,
fclose and setvbuf errors must fail the recording. Idle preview must avoid GPU waits.
Rear-camera tests wait for actual RGB preview pixels and check preserved Y/U/V planes
in recorded frames, with the same resolution and 30 FPS settings on both cameras.
It requires C/C++ compilers, pthreads, a host libjansson.so.4, and the Jansson headers
from a configured build-modern tree. Vita camera/GPU/audio calls are replaced with
stubs. The recorder's encoder is stubbed only in its lifecycle test.

To also run the **real** minih264 + AAC + MP4 encoder/decoder test (and the WebM sticker
test), build the app's FFmpeg release for the host (the Vita libraries are not touched):

```bash
bash scripts/build-native-ffmpeg.sh
export FFMPEG_NATIVE_BUILD="$PWD/external/ffmpeg-native/build"
bash tests/run-video-tests.sh
bash tests/run-webm-tests.sh /absolute/path/to/sticker.webm /absolute/path/to/alpha.webm
```

WebM checks cover user-supplied opaque and alpha samples (local copy: `tests/local/media/alpha.webm`)
(the reported white-background sticker): transparent, opaque and partial-alpha
pixels after RGBA conversion, every frame after looping, and malformed alpha
payload rejection. These use the production alpha decoder; Vita GPU blending
and playback performance still need a physical-device check.

The test decodes every video and audio packet and checks square dimensions,
circle masking, non-silent audio, duration, exact timestamps for 90 frames (including
a one-sample interval and a camera-switch gap), keyframes every 60 frames,
cancellation and truncated input cleanup.

These tests do not verify the physical Vita camera, microphone, GPU, encoder
performance or delivery to Telegram. Test on a device: open a chat → L → X;
switch with Triangle; X to stop; Square to play; X to send. Verify front/rear video,
audio synchronization, cancel/reopen, the 60-second limit, and the destination topic.


`bash tests/run-transfer-tests.sh` runs the production Telegram worker and parser
against a fake TDLib endpoint: 6,000 events while UI processing is stopped,
file-progress coalescing without lost control events, continued receive calls,
request recovery after backpressure, staged startup with a deferred cache writer,
parallel voice/video upload tracking, speed/stall/error states and original chat/topic
payloads. No network requests are made. The startup test requires configured API
settings (only local validation is exercised).

Status checks cover expiry and dismissal of completed events, chat-scoped upload
results, preservation of active uploads and diagnostic history, TDLib open/close
subscriptions, incoming messages after failed uploads, and explicit list refresh.
Additional cases cover list request timeouts, reconnect retries, ignored late replies,
draft position updates, and draft MTProxy checks with latency and a 15-second timeout.

Upload statistics tests cover sent/failed initial responses, confirmation arriving
before its request reply, correlation through sending_id, sticker sends without
invented byte progress, speed from receipt timestamps, stale file snapshots,
recent-operation selection, and separate UI/diagnostic retention. A queue of 1,000
file events must not delay a send confirmation; background events retain service.
Production worker tests also verify that navigation requests bypass congestion,
background requests wait during interaction, and receive() keeps running.

To include the production AudioRecorder with a real Ogg/Opus encoder, set
`FFMPEG_AUDIO_BUILD` to a native FFmpeg build with avformat, avcodec, avutil,
swresample, the opus encoder, ogg muxer and file protocol enabled. The test covers
microphone errors/restart, PCM conversion, Ogg headers/final granule, cancellation
and ownership of files passed to asynchronous uploads. Hardware audio calls are
mocked; Telegram delivery and throughput still require device testing.

Poll checks in `run-transfer-tests.sh` exercise the production message parser and
vote request builder: regular/quiz polls, single/multiple answers, display order vs
API positions, retraction restrictions, duplicate requests, errors, result refresh,
closed-state updates and persistence of updated polls in the UI cache.

Video quality checks in `run-transfer-tests.sh` verify minimum compatible resolution,
size tie-breaking, unsupported codecs, oversized original fallback, cached variants,
unknown sizes, unavailable variants, the actual download file ID and unchanged video notes.

`run-video-tests.sh` also checks converted-video cache keys, short reads and read
failures. `run-vault-tests.sh` verifies that the key stays the same after moving,
encrypting, decrypting and re-encrypting an actual source file, and rejects
unauthenticated samples from a damaged encrypted source.

`bash tests/run-memory-protection-tests.sh` checks the heap-wiping size policy, recognition
of VitaGram's gzip crash dumps by the NUL-terminated title ID (other titles and partial
`.psp2dmp.tmp` files included), overwrite-and-delete of only those dumps, classification of
system, own and injected plugin module paths, and that the masked session keys yield the same
database/file keys and a verifiable state signature after a lock and unlock.
With `FFMPEG_NATIVE_BUILD` set, `run-video-tests.sh` also creates and converts
an eight-second H.264/AAC video, reopens it in cache-only mode, and checks that
no conversion runs again, including after moving and encrypting its files.


`bash tests/run-font-tests.sh` loads the packaged fonts with host FreeType and
rasterizes every character in `ⲕⲗυⲙⲁⲧ` and `の一部 宇宙`, plus Cyrillic/Latin controls.
It exercises the production fallback selector and checks the Japanese face is static
Regular. Requires a host libfreetype.so.6 and FreeType headers (system or VitaSDK).
This verifies glyph coverage/rasterization, not the physical Vita GPU output.

The transfer suite also checks visible sender metadata lookup, small-avatar file IDs,
request deduplication/backoff, download completion/invalidation, missing photos,
refresh of cached metadata and avatars for messages sent on behalf of a chat.


`FFMPEG_NATIVE_BUILD=/path/to/native-ffmpeg bash tests/run-webm-tests.sh /path/to/sticker.webm`
reproduces the old `Open: -22` whitelist rejection on a VP9 WebM fixture, then uses
the production container/codec policy to decode every frame, convert to RGBA,
check timestamps, rewind and decode again. By default the fixture is the user-supplied
`assets/sticker.webm`; it is not included in the VPK. Enable matroska demuxer, VP9
decoder, file protocol, swscale and swresample in the native FFmpeg build.
Physical Vita playback/timing and audio output remain device checks.

Transfer tests also check logout cleanup on disk, rejection of late updates, preserved settings, symlink safety and recovery after interrupted cleanup. Font tests include the exact nickname 𝕭𝖆𝖉𝕯𝖗𝖊𝖆𝖒.

Startup-list tests cover empty initial positions, short loadChats pages, atomic replacement, offline errors, stale replies, separate folders, confirmed removals and end-of-list. Font tests also rasterize the exact name ✧Hehestl | *interrogative hehe*.

Sticker-pack icon tests exercise production selection of PNG/JPEG/WebP thumbnails, rejection of animated covers, fallback to sticker previews, lazy metadata requests with 64-bit set IDs, deduplication, stale-response rejection and download completion/invalidation without changing the open sticker grid.

Планировщик TDLib (0.33.2): `bash tests/run-td-scheduler-tests.sh`. Требуются host CMake, C++17, OpenSSL/zlib development packages и зависимости конфигурации TDLib. Сборка помещается в `/tmp/vitagram-td-native` (переопределяется через `TD_NATIVE_TEST_BUILD`). Тест использует настоящий планировщик с Vita-веткой бюджета, управляемые часы, две очереди и таймер; проверяет порядок, отсутствие потерь и завершение актора с остатком событий. Реальные сокеты Vita этим тестом не проверяются. `run-performance-tests.sh` дополнительно проверяет вложенные TD-этапы и режим OFF.

Логирование TDLib (0.33.3): `bash tests/run-td-logging-tests.sh`, те же host-зависимости и `TD_NATIVE_TEST_BUILD`, что у проверки планировщика. Тест компилирует Vita-ветку настоящего `TsCerr.cpp` и подменяет только OS write: ошибка, нулевой результат, частичная запись, освобождение блокировки после ошибки. Проверяется также отсутствие форматирования DEBUG/INFO при уровне 1 и сохранение ERROR. Тест отправок проверяет настройку уровня до создания клиента на том же потоке; тест статусов — 100% без ложного завершения.

Медиакеш (0.33.4): `bash tests/run-media-cache-tests.sh`. Проверяются 500 повторяющихся ссылок с одним stat, отсутствие I/O в режиме UI, доступность RAM-кеша при заблокированном stat, сброс во время проверки, исчезнувшие файлы и сохранение исходного протокольного ответа для отправок. `run-cache-tests.sh` проверяет приоритет ранних живых данных и удаления перед загрузкой кеша, `run-transfer-tests.sh` — фоновое восстановление без сети, однократный запуск, применение на UI и отказ от позднего снимка после очистки аккаунта.

Изображения (0.33.9): `bash tests/run-image-tests.sh`. Нужны host C++17, pthread и development-пакеты libpng/libjpeg/libwebp с pkg-config. Проверяются реальные RGBA-пиксели, встроенное уменьшение большого JPEG, уменьшение PNG, повреждённые и слишком большие файлы, смена видимых запросов, очистка с повторным путём, пауза и остановка фонового декодера. GPU-загрузку и время кадра проверять на Vita.

Текст (0.33.12): `bash tests/run-text-layout-tests.sh`. Проверяются ключи кеша ширины, сброс, вытеснение LRU, пределы числа записей/байтов, обрезка по границам UTF-8 и логарифмическое число измерений длинной строки. `run-font-tests.sh` проверяет реальное покрытие и растеризацию fallback-шрифтов; холодные задержки и GPU-рисование проверять на Vita.

`bash tests/run-chat-scroll-tests.sh` checks pixel dragging in both chat-list layouts,
partial-row hit testing, viewport edges, button selection visibility, page append,
list shrink and folder reset, plus row motion when the list is replaced (a moved chat
slides from its previous row, mid-slide changes, one-screen limit, no motion after a
folder change). Touch sampling and rendering require a PS Vita check.

`bash tests/run-font-tests.sh` also exercises the production text raster cache
with real FreeType and an in-memory Vita texture stub: exact glyph pixels at
multiple sizes, first-use order independence, padding, stride, native pixel
positions, kerning, measurement without GPU allocations and bounded cache reset.
It does not validate Vita GPU output.

`bash tests/run-photo-tests.sh` checks fullscreen photo rotation: gyro restricted to
0° and ±90°, debounce, flat/touch behavior, manual L/R rotation including 180°,
disabled sampling, sampling failure, and reopening the viewer. It also checks stick
zoom from 100% to 500%, pan direction and bounds, dead zones, frame-independent speed,
stall protection, touch priority and pinch limits. Vita motion and GPU
calls are stubbed; physical orientation and themed button rendering require a device.

`bash tests/run-audio-recording-dialog-tests.sh` checks preview/record/encode/review
states, explicit send, cancellation, microphone-level bounds, repeat playback and
ownership of the prepared voice-message file. Shortcut settings save/load are
covered by the vault-sweep tests.

`bash tests/run-spoiler-tests.sh` checks reveal reset on chat/topic/discussion exit,
app resume, selected reset events and cache invalidation signals. The vault-sweep
tests also check spoiler preference save/load, defaults and invalid value types.

Transfer tests also check video spoiler metadata, separate thumbnail download before consent, full alternative-video download after consent and cancellation when consent resets. Animated spoiler dust requires visual verification on Vita.


`bash tests/run-font-tests.sh` also runs the actual language-font loader with
real Korean/Chinese FreeType faces and host texture/scheduler stubs. A blocked
worker exercises stale completion, latest-selection handling, cancellation to
the default font, cached face/atlas reuse, missing/corrupt assets and shutdown
with an outstanding read. The previous active face and revision must remain
unchanged until commit. Host libwebp.so.7 and WebP headers are required as well.
Check rapid language switching and immediate All chats/Archive translation on Vita.


Language regressions: `run-i18n-tests.sh` checks retained nested/combined text,
raw-string preservation, recipe reset, a worker rendering during a catalog
commit, and every network state in every shipped language without another
hardware poll. `run-transfer-tests.sh` applies all locales to an existing
TelegramClient, checking authorization/status, folders, sticker sections,
custom reaction IDs, polls, documents, media captions, service messages and
reply previews without another TDLib event or outgoing request. Proxy/QR and
video tests also check retained scanner failures and recorder status across
language changes. Rapid switching and glyph appearance still need Vita testing.

Transfer tests cover cached deletion permission reads beyond the refresh interval, preserving rights during refresh, deletion without a second properties request, permission denial and waiting for all loaded album members.

`bash tests/run-upload-picker-tests.sh` checks attachment marking/unmarking for
photos, videos and files, the 10-item limit, path identity across folders and the
latest view, retained selections during refresh/navigation, and explicit review
before sending. Review checks cover Send as the first row, sending from any row,
empty/duplicate-send rejection, file preview/removal offsets and the two Add rows (Multimedia and File). Mixed multimedia browsing checks
photo/video filtering and automatic outgoing types, plus return-menu focus.
It also covers all six sort modes, cycling both ways, preserving
the focused file and marks, equal filenames, and returning to the right folder
after changing sort order in a child directory. The multimedia root shortcut for
latest screenshots must remain first through every sort, refresh and return. Requires VitaSDK headers; filesystem and clock calls are stubbed.
Visual highlighting and physical/touch controls require a PS Vita check.

`bash tests/run-qr-encode-tests.sh` checks QR byte encoding at levels M and H
through version 10, and decodes the actual antialiased login raster with its
rounded patterns and paper-plane badge across different tokens and lengths.
The four-module quiet zone and reduced-size rendering are covered. These are
host checks; phone scanning of the Vita display requires a device check.

`bash tests/run-media-seek-tests.sh` checks fullscreen 10-second seek steps,
hold acceleration, one commit on release, bounds, direction conflicts, stale
message/menu/sleep cancellation and touch-track mapping. The FFmpeg download-stream
suite also checks actual H.264 keyframe pre-roll. These host checks do not validate
controller/touch behavior or decoder startup time on Vita hardware.

`bash tests/run-keyboard-tests.sh` checks the shared vault/text keyboard with
controller and touch events: localized layouts and shift, short/long presses, UTF-8 deletion
and limits, multiline whitespace, submit/cancel and masked secret rendering.
System message input keeps Enter instead of multiline, including direct and
switched Japanese/Korean/Chinese IME routing; custom input retains line breaks.
It also verifies letter/digit previews, their suppression for secrets, the wider
text spacebar and the absence of clear-all through touch or R in ordinary fields.
RU checks cover the 11/11/9 letter rows, е→ё and ь→ъ with touch/controller holds,
uppercase alternatives, delayed release, cancellation, byte limits and previews.
Popup tests exercise the exact 299.999/300 ms boundary, deferred insertion until
release, е/Е→ё/Ё or 4, complete RU number alignment, touch sliding through the
key-to-panel gap, controller Left/Right selection, Circle/Start cancellation,
drag-out/re-entry, repeated release, punctuation choices and password byte limits.
Comma/period remain directly accessible and all digits remain on the symbols layer.
Rendering calls are stubbed; layout and physical input need a PS Vita check.
The vault suite checks persistence and malformed values of the keyboard preference.

Transfer tests also cover delayed Telegram links after chat/topic changes and reopening the same chat, stale topic history, document size updates, download progress after navigation and retries with late errors from an earlier attempt.

Document export tests cover background saving and duplicate comparison with blocked storage while the UI pump continues, repeated completion events, copy progress, cancellation cleanup and account reset.

`bash tests/run-vpk-tests.sh` checks package metadata bounds, rejected title IDs and patch packages, content-ID matching, and homebrew header checksums under ASan/UBSan. Transfer tests check completed export notifications, cached exports and clearing pending offers on logout; vault tests check the default-off installation preference. Native message dialogs and actual installation still need a PS Vita.

Keyboard localization tests cover all 19 shipped catalogs: 15 in-app profiles and
4 native IME routes, carousel selection/cancellation, preserved drafts, remembered
profile choices, console-language defaults, Turkish dotted/dotless case and locale
alternatives. Native IME calls are stubbed: forced languages, UTF-16 drafts,
password options and multiline mode are checked without opening a Vita dialog.

Keyboard case checks cover one-shot Shift, the 350 ms double-tap boundary, Caps
Lock/unlock through touch and controller, cancelled and rejected input, numeric
alternates, deletion, language changes and Turkish case. Context checks include
message starts, sentence punctuation, quotes, Unicode whitespace, newlines, common
abbreviations, decimals, filenames and URL/email interiors; password/exact-input
fields retain manual case. The visual Caps Lock marker still needs device review.

Keyboard cursor checks cover Square backspace, Triangle space, L/R boundaries,
UTF-8 middle edits, newline/alternate insertion, full-draft byte limits and
automatic case at the cursor. Vita rendering and physical feel need device checks.

Keyboard language checks cover EN + console defaults, British English mapping,
unsupported-system fallback, manual lists, the last-language guard, filtered
carousels, direct cycling, native language masks and CJK-only vault fallback.
Settings persistence checks cover migration, malformed arrays and default reset.

Language-key checks cover short-press cycling, the 300 ms threshold, controller
focus retention, touch/controller carousel selection, cancellation, single-language
input, all 19 profiles, seven visible choices, edge scrolling and IME handoff
on release. Switching preserves the draft, cursor and Caps Lock.

Deletion-repeat checks cover immediate input, the 350 ms delay and 60 ms interval,
release at a deadline, touch exit, controller focus changes, masked fields, UTF-8
middle edits, empty input, delayed-frame burst prevention, cancellation, reopening
and simultaneous deletion sources.

Passphrase rendering checks verify on-key controller badges, cursor hints inside
the spacebar, no separate shortcut-help strip, and no exposed passphrase in
either the full keyboard or the vault keyboard-only drawing mode.

`bash tests/run-updater-lifecycle-tests.sh` runs the production updater and separate updater app with mocked HTTPS, graphics and Vita system calls. It checks signed download/staging, signature revalidation at startup, helper installation, application installation/relaunch, cleanup, fallback storage and failures. Telegram VPK installation must remain blocked while a signed update is busy or staged, including pending markers left in either storage folder; explicit local handoff remains available after completion. Test packages use a temporary signing key; the production release key and verifier are unchanged. Actual device promotion still needs a PS Vita.

Transfer tests cover overlapping text styles, UTF-16 offsets after emoji and invalid entity ranges. Text formatting and spoiler controls require visual verification on Vita.

`bash tests/run-transfer-tests.sh` also checks Telegram user colors: built-in and server palettes, light/dark variants, gift accents, live channel changes, reply authors from history/cache/external origins, offline startup and live-over-cache precedence. `bash tests/run-vault-tests.sh` checks that the user-color switch persists and defaults to enabled for missing or invalid values.

`bash tests/run-reaction-tests.sh` checks short Cross on release, the 550 ms
hold threshold, single activation, selection/chat/topic and modal cancellation,
resume gaps, and controller/touch grid mapping with a separate preferred row.
Grid checks include cell edges, non-clickable gaps, empty preferred-row slots,
page offsets and unused cells on the last page.
It also checks short sticker/GIF selection, held Cross/touch previews, release without
sending, and cancellation on changed item/set/search generation/chat/topic, navigation or overlays.
Transfer checks cover full sticker/GIF downloads, completion gating, thumbnail separation
and static/animated sticker format selection. GIF catalog tests check completed full-file
paths for saved and searched GIFs and reject partially downloaded files for playback.
Transfer checks cover preferred/recent ordering, duplicate and Premium filtering,
and the eight-item first-row limit. Physical controls still need a Vita check.

`bash tests/run-radial-menu-tests.sh` checks the 250 ms Square threshold, five
fixed stick sectors, neutral/dead-zone and angular hysteresis, direct L/R modes,
pinned reply IDs, Circle/scope/suspend cancellation, rapid represses and exact-once
confirmation after closing. It checks early-release animation continuity, cached
geometry and actual themed drawing with a bounded frame-pool stub and zero menu
heap allocations over warm frames. Transfer checks cover replies for text,
stickers, inline GIFs, voice, mixed albums/documents, failures and cancellation.
Vita GPU appearance and 60 FPS timing require physical-device verification.

Radial checks also cover repeated L and R toggles in both directions, simultaneous
shoulder presses, rounded contour bounds and curved corners. The real renderer
keeps the 496 px glass wheel and glow inside the viewport, including the badges,
icons and captions, within a bounded GPU frame pool and without warm-frame heap
allocations. `RADIAL_PREVIEW=/tmp/radial.jsonl bash tests/run-radial-menu-tests.sh`
exports native drawing commands for visual review; fonts and compositing on the
physical Vita still require a device check.

Radial smoothing checks bound circle/arc/corner chord errors below 0.05 px,
verify cached unit contour normals, and require one-pixel alpha-coverage strips
on the real rendered shapes while retaining the fixed GPU-pool and zero-heap
checks. Petals, central rings and rounded icon/text boxes use local edge smoothing.

Design checks also cover dark radial glass, adaptive glyph/label contrast,
Light/Dark secondary text and BBR/BRR2 palettes. Palette Studio browser checks
include the ninth radial view, selection, L/R toggles and live theme changes.

Keyboard design regressions verify theme wallpaper/header use, key masks,
independent focus color, readable normal/pressed/disabled labels, accent
glyph contrast and the vault keyboard-only mode. Theme tests keep slots
6/7 assigned to black-blue/black-red without shifting the remaining IDs.

Split-keyboard regressions send real two-contact packets through CustomKeyboard
and Keyboard, including reordered IDs, single-finger tails, jitter, one-sided/
vertical/parallel drags, stale chords, third fingers, all custom layouts,
release typing/submission, repeat deletion and masked vault entry.

Keyboard drawing checks also verify 2 px key borders and one-pixel transparent
coverage fringes on dense rounded contours in both full and split layouts.

`bash tests/run-reaction-tests.sh` also checks quote frame widths and boundaries
for multiline, adjacent and single-line blocks. Transfer checks cover current
reaction counts/selection, sticker/custom emoji set lookup, installation retries,
request deduplication and stale responses after leaving the set.

Chat opening checks in `run-transfer-tests.sh` cover latest/server/device modes, exact read anchors, encrypted-cache position persistence and early-restore precedence, memory-cap acknowledgement, bounded history in both directions, deleted targets, retry and stale responses. `run-vault-tests.sh` covers preference persistence. Loading-screen appearance, physical input and RAM headroom require a Vita check.

Chat opening priority checks cover newer device/server positions, equal or missing markers, and preservation of latest/device-only modes for chats, forum topics and comments.
