# Keyboard languages / Языки клавиатуры

The language key cycles to the next enabled language on a short press. Holding
for 300 ms opens a popup carousel above the key, using the same release gesture
as character alternatives. Slide to a badge or hold Cross and select with
Left/Right; Circle cancels without switching. A long press initially selects the
current language. Up to seven badges fit at once; holding a finger at an edge
scrolls larger sets after 350 ms, then every 180 ms. Controller selection wraps
and scrolls to the selected badge. A single enabled language leaves the key idle.
Settings labels are translated in all 19 interface catalogs.
Settings → Keyboard contains keyboard type and input
languages. The default list is English plus the supported console language, or
English alone; British English maps to the English profile. Manual lists are
saved across restarts and stay independent of the interface language. The last
input language cannot be disabled. The default-list row resets manual choices.
The carousel lists only enabled profiles. The initial choice follows the console
language when it is enabled, falling back to the first enabled profile;
manual selection is retained for subsequent inputs while the app is running.
Switching a profile keeps the current text and cursor.
Square deletes the character before the cursor; Triangle inserts a space; L/R
move left/right by a UTF-8 code point. Backspace deletes immediately, then repeats
after 350 ms and every 60 ms while Square, touch Delete or Cross on Delete stays
held. Release, leaving the touch key, focus movement or another action cancels
the repeat. A delayed frame deletes at most one character. The preview follows the cursor through
wrapped lines. Symbols are selected with the on-screen ?123 key. The same
shortcuts apply to masked passphrases; clearing them is a touch/controller key
action only. Passphrase entry has no separate shortcut-help strip: Square,
Triangle, Circle and Start badges are inside their action keys, and L/R cursor
hints sit on the spacebar. Automatic case uses the
text before the insertion point.

Короткое нажатие клавиши языка переключает следующий включённый язык.
Удержание 300 мс открывает всплывающую карусель над клавишей: сдвиньте палец
или выберите стрелками влево/вправо при зажатой ×, затем отпустите. ○ отменяет
выбор. Изначально выбран текущий язык; одновременно видно до семи вариантов.
Для остальных задержите палец у края панели; на кнопках список прокручивается
вслед за выбором. Один включённый язык не переключается.
Подписи настроек переведены во всех 19 каталогах. «Настройки → Клавиатура» содержит
выбор типа клавиатуры и языков ввода. По умолчанию включены EN + поддерживаемый
язык системы, иначе — только EN. Британский английский использует профиль EN.
Список сохраняется между запусками независимо от языка интерфейса; последний
язык отключить нельзя. Пункт набора по умолчанию сбрасывает ручной выбор.
В карусели отображаются только включённые языки. Начальный язык
соответствует системному, если он включён, иначе — первому включённому;
ручной выбор сохраняется для следующих вводов до закрытия приложения.
Смена раскладки сохраняет текст и курсор. □ удаляет символ перед курсором,
△ вставляет пробел, L/R перемещают курсор влево/вправо. Удержание □, экранной
клавиши удаления или × на ней повторяет удаление через 350 мс, затем каждые
60 мс. Отпускание, уход с клавиши или другое действие отменяет повтор. Вставка и удаление
работают в середине текста, включая кириллицу и emoji. Окно прокручивается
к курсору; регистр определяется по тексту перед ним. Символы доступны через
клавишу ?123. В паролях действуют те же кнопки; R больше не очищает пароль.
Отдельной строки подсказок управления нет: □, △, ○ и Start показаны на
соответствующих клавишах, L/R — на пробеле.

| Language / Язык | In-app layout / Встроенная раскладка | Examples of held variants / Примеры удержания |
| --- | --- | --- |
| English | QWERTY | Accented Latin vowels, æ, œ, ß |
| Русский | ЙЦУКЕН, 11/11/9 | е → ё / 4; ь → ъ |
| Deutsch | QWERTZ | ä, ö, ü, ß / ẞ |
| Français | AZERTY | à, â, æ, é, è, ê, ë, œ, ç; apostrophe on the last row |
| Español | QWERTY + ñ | á, é, í, ó, ú, ü, ñ; ¿ / ¡ on punctuation |
| Italiano | QWERTY | à, è, é, ì, ò, ó, ù, ə, ɜ |
| Nederlands | QWERTY | Accented vowels, ĳ / Ĳ |
| Português (PT / BR) | QWERTY, two profiles | ã, õ, á, â, ê, ó, ô, ç |
| Svenska | QWERTY + å / ö / ä | a → ä / å; o → ö |
| Dansk | QWERTY + å / æ / ø | a → å / æ; e → é; o → ø |
| Norsk | QWERTY + å / ø / æ | a → å / æ; e → é; o → ø |
| Suomi | QWERTY + å / ö / ä | ä, å, ö, š, ž |
| Polski | QWERTY | ą, ę, ó, ś, ń, ć, ż, ź, ł |
| Türkçe | Turkish Q | i ↔ ı; İ / I; ç, ğ, ö, ş, ü, â, î, û |
| 日本語 / 한국어 / 简体中文 / 繁體中文 | System IME / Системный ввод | Full composition and candidates are handled by Vita |

Native handoff explicitly selects JA, KO, ZH-CN or ZH-TW and transfers the current
UTF-16 draft. The system keyboard's language mask also includes Turkish, British
English and Brazilian Portuguese. The vault's passphrase screen never invokes
system IME: its carousel filters the enabled list to in-app profiles, falling back
to English if none remain. Before vault unlock, encrypted preferences are not
available, so the default console-language pair is used.

При переходе в системный ввод явно выбирается нужный язык и переносится текущий
текст. Экран ключа хранилища оставляет из выбранных языков только встроенные
раскладки, при пустом результате использует английскую. До разблокировки
зашифрованные настройки недоступны, поэтому применяется набор по умолчанию.

Shift is one-shot and is consumed by a successful character insertion, including
an alternate or digit; spaces retain it. Double-tapping Shift within 350 ms locks
Caps Lock; another tap unlocks it. Caps Lock is visibly underlined and survives
deletions, spaces, newlines and language changes. Cancelled choices and insertions
rejected by the length limit do not spend one-shot Shift.

Automatic uppercase applies to message composition only. It follows the current
text after edits: empty input, a newline, or sentence-ending `.`, `!`, `?`, `…`
followed by whitespace. Quotes/brackets, nonbreaking spaces and Unicode sentence
punctuation are recognized. Common titles/address abbreviations (`Dr.`, `т.е.`,
`ул.` and similar) suppress it; decimals, filenames and URL/email interiors do not
count as sentence boundaries. This is a conservative text heuristic, not a full
language parser. A Shift tap can override an automatically suggested capital.
Passwords and exact-input fields use manual case throughout. French automatic
uppercase changes letters, leaving the apostrophe available; its shifted question
mark requires explicit Shift.

Long press remains 300 ms. A letter's accents and its digit share one popup;
release commits exactly one selected option. Shift uses explicit Unicode case
pairs, including Turkish i → İ and ı → I. Larger lists show abbreviated hints;
the full list remains in the popup. Layout geometry limits the longest list to
the display width. Secret entry never shows enlarged typed characters.

## Source review / Сверка

Character relationships were checked against the public HeliBoard
[locale tables](https://github.com/HeliBorg/HeliBoard/tree/main/app/src/main/assets/locale_key_texts)
(`da`, `de`, `es`, `fi`, `fr`, `it`, `nb`, `nl`, `pl`, `pt`, `sv`, `tr`, `ru`),
the common `more_popups_main` Latin set, and its
[QWERTY/QWERTZ/AZERTY layouts](https://github.com/HeliBorg/HeliBoard/tree/main/app/src/main/assets/layouts/main).
VitaGram retains its previously requested modern Russian е → ё / 4 and ь → ъ
choices rather than adding HeliBoard's historical letters. Quote variants are
available on the symbol layer for the locales that define them. Android source
implementation, dictionaries and composition engines were not copied.

Validation: `ASAN_OPTIONS=detect_leaks=0 bash tests/run-keyboard-tests.sh` and
`bash tests/run-i18n-tests.sh`. The font coverage check also verifies new Latin
and Cyrillic layout glyphs against the packaged font faces. Real Vita IME
availability, touch precision and GPU rendering still need device verification.

## Custom .lang files / Пользовательские .lang

Copy a catalog from `assets/lang/` to `ux0:data/vitagram/lang/<code>.lang`
(`/data/vitagram/lang` on the memory card), then restart VitaGram. Set `@code`
to the filename without `.lang` and `@name` to the name shown in Settings →
Language. A new code adds an interface language; a matching code overrides the
bundled catalog. Missing translations retain the bundled language's text, then
fall back to English. Catalogs use UTF-8; keep controller tokens and placeholders.

Скопируйте готовый `.lang` в `ux0:data/vitagram/lang/<код>.lang` и перезапустите
приложение. `@code` должен совпадать с именем файла без расширения, `@name` —
название в списке языков. Новый код добавляет язык, существующий заменяет его
переводы и указанные данные клавиатуры. Пропущенные строки берутся из встроенного
каталога, затем из английского. Для нового языка удобнее копировать `en.lang`.

Keyboard data uses `@keyboard.*` directives, independent of the selected interface:

- `badge`: short language label; `rows`: three letter rows separated by `|`.
- `symbols.0`, `symbols.1`, `symbols.2`: the three symbol rows. Escape a literal
  backslash as `\\`, using the ordinary `.lang` escaping rules.
- `upper.lower`, `upper.upper`: matching sequences of lowercase/uppercase Unicode
  characters, one pair per position. Include the letters used in held alternatives.
- `popup.<character>`: alternatives for that character, one Unicode character per
  option, including punctuation (`popup.,`, `popup..`).
- `shift.<character>`: replacement for an explicitly shifted key (French apostrophe).
- `digits`: held digits for the top letter row; `digit_start`: first column, 0 or 1.

Keep all keyboard directives when copying a catalog. Letter rows accept 1–12
characters each, symbol rows 1–14, and popups at most 11 options plus a held digit.
Invalid layouts are skipped; an English recovery keyboard remains available.
New profiles use the in-app keyboard; Vita's native IME supports only its built-in
languages. Fonts must contain the added characters; a `.lang` does not install fonts.
Files are read at startup (at most 64 catalogs, 512 KiB per file).

Раскладка появится в «Настройки → Клавиатура → Языки ввода», если сохранены
директивы `@keyboard.*`. Буквенные ряды задаются через `|`, дополнительные символы —
в `popup.<символ>`, пары регистра — одинаковыми по длине строками `upper.lower`
и `upper.upper`. Пользовательская раскладка работает во встроенной клавиатуре;
системная клавиатура ограничена языками прошивки Vita. Для новых символов нужен
подходящий шрифт. Проверка отображения и ввода на настоящей Vita обязательна.
