# Licenses and acknowledgements / Лицензии и благодарности

[← VitaGram](../README.md)

VitaGram's own source code is licensed under [MIT](../LICENSE). Third-party code,
fonts and adapted artwork keep their original licenses. AI assistance does not
change those licenses or the attribution requirements.

Собственный код VitaGram распространяется по [MIT](../LICENSE). Сторонний код,
шрифты и адаптированная графика сохраняют исходные лицензии. Использование AI
не меняет лицензии и требования к указанию авторства.

| Component / Компонент | License / Лицензия |
| --- | --- |
| TDLib and the tdlib-vita port / TDLib и порт tdlib-vita | [Boost Software License 1.0](../external/tdlib-upstream/LICENSE_1_0.txt) |
| SQLCipher bundled with TDLib / SQLCipher в TDLib | [BSD-3-Clause](../external/tdlib-upstream/sqlite/sqlite/LICENSE); SQLite core is [public domain](https://www.sqlite.org/copyright.html) / ядро SQLite — общественное достояние |
| vita2d | [MIT](../assets/vita2d-LICENSE.txt) |
| Jansson | [MIT](../external/jansson/LICENSE) |
| OpenSSL | [Apache-2.0](https://github.com/openssl/openssl/blob/openssl-3.5.9/LICENSE.txt) |
| FFmpeg with the Vita decoder / FFmpeg с Vita-декодером | [LGPL-2.1-or-later](https://ffmpeg.org/legal.html), for the configured build / для используемой конфигурации |
| rlottie | [MIT and component licenses](../external/rlottie-vita/source/COPYING): [MPL-2.0](../external/rlottie-vita/source/licenses/COPYING.MPL), [BSD-3-Clause](../external/rlottie-vita/source/licenses/COPYING.SKIA), [FTL](../external/rlottie-vita/source/licenses/COPYING.FTL), [Pixman](../external/rlottie-vita/source/licenses/COPYING.PIX), [RapidJSON](../external/rlottie-vita/source/licenses/COPYING.RPD) and [stb](../external/rlottie-vita/source/licenses/COPYING.STB) notices |
| FreeType | [FTL or GPL-2.0](https://freetype.org/license.html); the FTL option is used for attribution here / здесь указано авторство по FTL |
| Noto fonts, including CJK and emoji / Шрифты Noto, включая CJK и emoji | [SIL Open Font License 1.1](../assets/NotoSans-LICENSE.txt); additional family notices in `assets/*-LICENSE.txt` / уведомления семейств в `assets/*-LICENSE.txt` |
| zlib | [Zlib](https://zlib.net/zlib_license.html) |
| libpng | [PNG Reference Library License v2](https://github.com/pnggroup/libpng/blob/libpng16/LICENSE) for the installed SDK version / для установленной версии SDK |
| libjpeg-turbo / JPEG | [IJG and BSD-style component licenses](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md), including Zlib / включая Zlib |
| libwebp | [BSD-3-Clause](https://github.com/webmproject/libwebp/blob/main/COPYING) |
| bzip2 / libbzip2 | [bzip2-1.0.6 license](https://sourceware.org/bzip2/manual/manual.html), also used by 1.0.8 / также применяется в 1.0.8 |
| minih264 | [CC0-1.0](../external/minih264/LICENSE) |
| quirc | [ISC](../external/quirc/LICENSE) |
| Mozilla CA certificate bundle supplied by curl / Сертификаты Mozilla из curl | [MPL-2.0](https://curl.se/docs/caextract.html#license) |
| Adapted VHBB panels and header / Панели и шапка VHBB | [CC BY-NC-SA 4.0](../assets/VitaGram_UI_Assets_v1/themes/vhbb/LICENSE.txt), with [attribution and changes](../assets/VitaGram_UI_Assets_v1/themes/vhbb/NOTICE.txt) |
| VitaSDK C/C++ runtime / Среда исполнения C/C++ VitaSDK | [newlib component licenses](https://github.com/vitasdk/newlib/blob/vita/COPYING.NEWLIB); [libstdc++ GPLv3 with GCC Runtime Library Exception 3.1](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html) |

The FFmpeg build has GPL, version-3 and nonfree features disabled. The TDLib
SQLite tree contains SQLCipher code, so its BSD notice must be retained in
addition to recognizing SQLite's public-domain core. rlottie's MPL-2.0 notice
covers its interpolator; a blanket MIT label would be incomplete.

Сборка FFmpeg отключает GPL, version-3 и nonfree-компоненты. В дереве SQLite
из TDLib есть код SQLCipher: нужно сохранять его BSD-уведомление, несмотря
на статус общественного достояния ядра SQLite. У rlottie интерполятор под MPL-2.0;
обозначение всей библиотеки как MIT было бы неполным.

The code's MIT license does not grant unrestricted rights to VHBB artwork.
Those adapted assets require attribution, noncommercial use and the same license
for adaptations, as described by [Creative Commons](https://creativecommons.org/licenses/by-nc-sa/4.0/).
Screenshots can contain third-party names, avatars, stickers, logos and media;
these are not relicensed as VitaGram source code.

MIT для кода не даёт неограниченных прав на графику VHBB. Адаптированные ресурсы
требуют указания авторства, некоммерческого использования и той же лицензии
для производных работ; условия описаны у [Creative Commons](https://creativecommons.org/licenses/by-nc-sa/4.0/).
Имена, аватары, стикеры, логотипы и медиа на скриншотах не становятся частью
собственного кода VitaGram под MIT.

The current VPK recipe includes the project's license and notices for TDLib,
vita2d, Jansson, OpenSSL, FFmpeg, rlottie (when enabled), Noto, minih264, quirc
and VHBB under `app0:assets/` and `app0:ui/themes/vhbb/`. Packaging is not yet a
complete license-notice inventory: see [release checks](../RELEASING.md#license-notices--лицензионные-уведомления).
**Settings → About and diagnostics → About** also lists third-party licenses.

Текущая конфигурация VPK включает лицензию проекта и уведомления TDLib, vita2d,
Jansson, OpenSSL, FFmpeg, rlottie (если включён), Noto, minih264, quirc и VHBB
в `app0:assets/` и `app0:ui/themes/vhbb/`. Перечень упакованных уведомлений пока
неполный; подробности — в [проверках релиза](../RELEASING.md#license-notices--лицензионные-уведомления).
Лицензии зависимостей также перечислены в **Настройки → О приложении и диагностика → О проекте**.

### Acknowledgements / Благодарности

VitaGram builds on the work of these authors and communities. Thank you to every
upstream author, maintainer and contributor, including those named in the linked
license and AUTHORS files.

VitaGram основан на работе этих авторов и сообществ. Спасибо всем разработчикам,
сопровождающим и участникам, в том числе перечисленным в лицензиях и файлах AUTHORS.

- **[Vladislav Atamanenko / Владислав Атаманенко](https://github.com/v-atamanenko)** —
  [tdlib-vita](https://github.com/v-atamanenko/tdlib-vita), the first TDLib port
  for PS Vita and the basis for the POSIX compatibility layer / первый порт TDLib
  на Vita и основа слоя совместимости POSIX.
- **[VitaSDK](https://github.com/vitasdk), GCC and newlib contributors / участники GCC и newlib** —
  the toolchain, runtime and platform headers / инструменты сборки, среда исполнения и заголовки платформы.
- **[Sergi Granell (xerpi)](https://github.com/xerpi/libvita2d) and vita2d contributors / участники vita2d** —
  2D rendering / двумерная отрисовка.
- **[Telegram and TDLib contributors](https://github.com/tdlib/td), [SQLite](https://www.sqlite.org/) and [Zetetic / SQLCipher](https://github.com/sqlcipher/sqlcipher)** —
  Telegram integration and local databases / подключение Telegram и локальные базы.
- **[Petri Lehtinen and Jansson contributors](https://github.com/akheron/jansson)** —
  JSON parsing / разбор JSON.
- **[OpenSSL](https://github.com/openssl/openssl), [FFmpeg](https://ffmpeg.org/), [fish47 / FFmpeg-vita](https://github.com/fish47/FFmpeg-vita) and [MakiseKurisu](https://github.com/MakiseKurisu/vita-sceVideodecInitLibrary-test)** —
  cryptography, media and Vita hardware-decoder work / криптография, медиа и аппаратное декодирование Vita.
- **[Samsung and the rlottie authors](../external/rlottie-vita/source/AUTHORS)**,
  including Subhransu Mohanty and Hermet Park, and the Skia, Pixman, RapidJSON,
  stb and Mozilla contributors — animation and rendering components / анимация и компоненты отрисовки.
- **[The FreeType Project](https://freetype.org/)**, including David Turner,
  Robert Wilhelm and Werner Lemberg, and **[Noto contributors](https://github.com/notofonts)** —
  font rendering, multilingual typefaces and emoji / отрисовка шрифтов, поддержка языков и emoji.
- **[Jean-loup Gailly and Mark Adler / zlib](https://zlib.net/)**,
  **[libpng authors](https://github.com/pnggroup/libpng)** (Guy Eric Schalnat,
  Andreas Dilger, Glenn Randers-Pehrson and Cosmin Truta),
  **[Independent JPEG Group and libjpeg-turbo contributors](https://github.com/libjpeg-turbo/libjpeg-turbo)**,
  **[Google and libwebp contributors](https://github.com/webmproject/libwebp)**,
  and **[Julian Seward and bzip2 contributors](https://sourceware.org/bzip2/)** —
  image decoding and compression / декодирование изображений и сжатие.
- **[lieff / minih264](https://github.com/lieff/minih264)** and
  **[Daniel Beer / quirc](https://github.com/dlbeer/quirc)** — H.264 encoding and
  QR recognition / кодирование H.264 и распознавание QR.
- **[Mozilla](https://www.mozilla.org/) and [Daniel Stenberg / curl contributors](https://curl.se/docs/caextract.html)** —
  the CA certificate store and its PEM distribution / корневые сертификаты и их распространение в PEM.
- **[devnoname120 and VHBB contributors](https://github.com/devnoname120/vhbb)** —
  the artwork adapted for the VHBB theme / графика для адаптированной темы VHBB.
- **[TheOfficialFloW / VitaShell](https://github.com/TheOfficialFloW/VitaShell)** and
  **[VegettoSan / PSVitaAlive](https://github.com/VegettoSan/PSVitaAlive)** —
  examples of Vita package installation and update lifecycle / примеры установки пакетов и самообновления Vita.

Thanks also to the PS Vita homebrew community, testers, translators and everyone
who reports problems or contributes improvements. Credits acknowledge their work
and do not imply endorsement of VitaGram.

Также спасибо сообществу homebrew PS Vita, тестировщикам, переводчикам и всем,
кто сообщает об ошибках и помогает улучшать проект. Благодарность не означает
официального одобрения VitaGram перечисленными авторами.
