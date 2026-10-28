# Privacy and local data / Приватность и локальные данные

[← VitaGram](../README.md)

## English

### Privacy and local data

Data is stored in `ux0:data/vitagram/`. The `db/` directory contains your Telegram
session and account data; `files/` holds downloaded media. Cache retention and size
limits are configurable in Settings → Data and storage. Clean cache now applies the current
retention and size limits; it does not force deletion of all media. Do not share
`db/` or delete it to clear photos.

**Settings → Data protection → Data encryption** protects the database, downloaded media, cache indexes
and settings with a text passphrase (at least 10 characters) or a console-button
sequence (12–32 presses). A longer passphrase offers stronger protection against
guessing. Encryption is bound to the console: copying the card to another Vita
does not unlock the vault. **A forgotten key cannot be recovered.** Resetting the
vault erases local account data and requires signing in again.

<details>
<summary>Encryption scope and limits</summary>

Encryption changes restart the app and convert existing files before Telegram
starts. Interrupted or failed conversions resume on the next launch; until
conversion finishes, some files may remain unencrypted. Auto-lock after inactivity
or sleep hides the screen and pauses playback; background work continues, and
decryption keys remain in RAM.

Downloaded files can briefly exist in plain form before sealing. Recordings,
conversion scratch files, explicit exports and diagnostic logs are outside this
protection. Encryption does not hide filenames or sizes and cannot protect data
from software controlling the running console.

While encryption is on, VitaGram also limits what the running session leaves in RAM:
freed memory blocks up to 1 MiB are zeroed, the master key is not kept after
unlocking, and session keys are stored masked. Crash dumps (`ux0:data/psp2core-*.psp2dmp`)
contain decrypted memory; VitaGram's dumps are overwritten and deleted after unlocking
unless a diagnostic log is enabled. Key-entry screens name plugins loaded into VitaGram
from `tai/config.txt` (`*ALL` or `*VITAGRM01`): they can read the key and messages.
Kernel plugins cannot be detected or stopped by the app.

Without data encryption, console-derived keys protect the database/cache and
obfuscate proxy credentials; OpenPSID is not a secret. Downloaded media remains
unencrypted. SOCKS5 and HTTP proxy credentials travel unencrypted to the proxy;
Telegram message traffic remains MTProto-encrypted.

</details>

Diagnostic logs are **off by default**. Enable individual logs in
**Settings → About and diagnostics → Diagnostic logs**. Review logs before sharing; report the app version,
steps to reproduce, media type and any on-screen error without including credentials
or account files. **Settings → About and diagnostics → About** contains project links and privacy notes.

## Русский

### Приватность и локальные данные

Данные находятся в `ux0:data/vitagram/`. Каталог `db/` содержит сессию Telegram
и данные аккаунта, `files/` — загруженные медиа. В подменю «Настройки → Данные и память»
можно задать срок хранения и размер кеша, а также запустить очистку по этим
параметрам: кнопка не удаляет все медиа принудительно. Не публикуйте `db/` и не удаляйте его ради очистки фото.

**Настройки → Защита данных → Шифрование данных** защищает базу, загруженные медиа, индексы кэша
и настройки текстовой фразой (от 10 символов) или последовательностью кнопок
(12–32 нажатия). Длинная фраза устойчивее к перебору. Шифрование привязано
к консоли: перенос карты на другую Vita не разблокирует хранилище.
**Забытый ключ восстановить нельзя.** Сброс хранилища удаляет локальные данные
аккаунта и требует повторного входа.

<details>
<summary>Что защищает шифрование и каковы его ограничения</summary>

Изменение режима шифрования перезапускает приложение и преобразует существующие
файлы до подключения Telegram. Прерванное или неудачное преобразование продолжится
при следующем запуске; до завершения часть файлов может оставаться открытой.
Автоблокировка после бездействия или сна закрывает экран и ставит воспроизведение
на паузу; фоновые операции продолжаются, ключи остаются в оперативной памяти.

Загружаемые файлы кратковременно существуют открытыми до шифрования. Записи,
временные файлы конвертации, явный экспорт и диагностические журналы не защищены
этим режимом. Шифрование не скрывает имена и размеры файлов и не защищает от
программ, контролирующих работающую консоль.

При включённом шифровании VitaGram также сокращает следы сеанса в оперативной памяти:
освобождаемые блоки памяти до 1 МиБ обнуляются, мастер-ключ после разблокировки
не хранится, а сеансовые ключи хранятся в маскированном виде. Дампы падений
(`ux0:data/psp2core-*.psp2dmp`) содержат расшифрованную память; дампы VitaGram
перезаписываются и удаляются после разблокировки, если не включён диагностический журнал.
Экраны ввода ключа показывают плагины, загруженные в VitaGram из `tai/config.txt`
(`*ALL` или `*VITAGRM01`): они могут прочитать ключ и сообщения. Плагины ядра
приложение обнаружить и остановить не может.

Без шифрования данных ключи на основе идентификатора консоли защищают базу/кэши
и обфусцируют реквизиты прокси; OpenPSID не является секретом. Загруженные медиа
остаются открытыми. Логин и пароль SOCKS5/HTTP передаются прокси без шифрования;
сообщения Telegram остаются зашифрованными MTProto.

</details>

Диагностические журналы **выключены по умолчанию**. Отдельные журналы включаются
в **Настройки → О приложении и диагностика → Диагностические логи**. Перед отправкой просмотрите содержимое; в отчёте
укажите версию, шаги воспроизведения, тип медиа и ошибку на экране, исключив
реквизиты и файлы аккаунта. **Настройки → О приложении и диагностика → О проекте** содержит ссылки и сведения
о приватности.
