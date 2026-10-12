# Native checks

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
bash tests/run-webm-tests.sh
```

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
list shrink and folder reset. Touch sampling and rendering require a PS Vita check.

`bash tests/run-font-tests.sh` also exercises the production text raster cache
with real FreeType and an in-memory Vita texture stub: exact glyph pixels at
multiple sizes, first-use order independence, padding, stride, native pixel
positions, kerning, measurement without GPU allocations and bounded cache reset.
It does not validate Vita GPU output.

`bash tests/run-photo-tests.sh` checks fullscreen photo rotation: gyro restricted to
0° and ±90°, debounce, flat/touch behavior, manual L/R rotation including 180°,
disabled sampling, sampling failure, and reopening the viewer. Vita motion and GPU
calls are stubbed; physical orientation and themed button rendering require a device.
