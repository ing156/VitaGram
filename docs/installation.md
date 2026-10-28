# Installation and updates / Установка и обновления

[← VitaGram](../README.md)

## English

### Installation and first launch

You need a homebrew-enabled PS Vita with VitaShell, internet access and enough
free space for the package and cached media.

1. Download `vitagram.vpk` from a published release and copy it to the console.
2. Install the VPK through VitaShell and launch **VitaGram** from LiveArea.
3. Choose whether to encrypt local data. Keep the chosen unlock key safe.
4. Sign in with your phone number, Telegram login code and, if enabled, your
   two-step verification password. Or tap **Sign in with a QR code** (or press
   **Square**) and scan the Vita screen from Telegram on your phone:
   **Settings → Devices → Link Desktop Device**. If enabled, two-step verification
   still requires your password. The login QR uses rounded modules and a central
   paper-plane badge with High error correction.

If a proxy is needed for login, press **Start** on the authorization screen to
configure it. Select the proxy type and enter its details, then choose
**Save and apply**. A proxy link or QR import also requires this confirmation.

The interface follows the console's language, falling back to English.
Change it manually in **Settings → Interface → Language**. English, Russian, German, French,
Spanish, Italian, Dutch, Portuguese (Portugal/Brazil), Swedish, Danish, Norwegian,
Finnish, Polish, Turkish, Japanese, Korean and Simplified/Traditional Chinese are
included. British English uses the English catalog. Korean and Chinese chat text
uses the corresponding font while that interface language is selected.

### Updates

**Settings → Updates and installation → Update** checks for and installs a newer release. Automatic updates
are enabled by default and begin about a minute after launch. With a Telegram
proxy enabled, only manual checking is available: updates connect to GitHub directly.

GitHub updates install only signed packages. VitaGram restarts after installation and
preserves settings and account data. If manual installation is needed, follow
the instructions shown in settings.

VPK files saved from chats can be installed directly after enabling **Settings → Updates and installation → Allow VPK installation** (off by default). Each installation requires confirmation and a warning about console access. The saved package is kept on cancellation or failure. For a VitaGram VPK, the package is then handed directly to the separate updater: VitaGram closes, the updater installs it and launches VitaGram again. Local packages are explicitly trusted by the user; automatic GitHub updates still require release signatures. Packages are limited to 512 MiB unpacked.

## Русский

### Установка и первый запуск

Нужны PS Vita с поддержкой homebrew и VitaShell, доступ в интернет и свободное
место для пакета и медиакэша.

1. Скачайте `vitagram.vpk` из опубликованного релиза и перенесите на консоль.
2. Установите VPK через VitaShell и запустите **VitaGram** из LiveArea.
3. Выберите, шифровать ли локальные данные. Сохраните выбранный ключ разблокировки.
4. Войдите по номеру телефона, коду Telegram и, если включён, паролю двухэтапной
   аутентификации. Или нажмите **«Войти по QR-коду»** на экране (либо кнопку
   **□**) и отсканируйте экран Vita с телефона в Telegram:
   **Настройки → Устройства → Подключить устройство**. Если включена двухэтапная
   проверка, затем потребуется её пароль. QR для входа имеет скруглённый узор
   и значок самолётика в центре; используется высокий уровень коррекции ошибок.

Если для входа нужен прокси, нажмите **Start** на экране авторизации.
Выберите тип, заполните данные и нажмите **«Сохранить и применить»**.
Импорт по ссылке или QR-коду тоже требует этого подтверждения.

Язык интерфейса выбирается по языку консоли; если каталога нет, используется
английский. Ручной выбор — **Настройки → Интерфейс → Язык**. Включены английский, русский,
немецкий, французский, испанский, итальянский, нидерландский, португальский
(Португалия/Бразилия), шведский, датский, норвежский, финский, польский, турецкий,
японский, корейский, упрощённый и традиционный китайский. Британский английский
использует английский каталог. Корейский и китайский текст в чатах использует
соответствующий шрифт при выборе этого языка интерфейса.

### Обновления

**Настройки → Обновления и установка → Обновление** проверяет и устанавливает новый релиз. Автоматическое
обновление включено по умолчанию и начинается примерно через минуту после запуска.
При включённом прокси Telegram доступна только ручная проверка: обновления
подключаются к GitHub напрямую.

При обновлении с GitHub устанавливаются только подписанные пакеты. После установки VitaGram перезапускается;
настройки и данные аккаунта сохраняются. Если нужна ручная установка, следуйте
инструкции в настройках.

VPK из чатов можно устанавливать после включения **Настройки → Обновления и установка → Разрешить установку VPK** (по умолчанию выключено). Перед каждой установкой появляются подтверждение и предупреждение о доступе к консоли. При отмене или ошибке скачанный файл сохраняется. VPK самого VitaGram затем передаётся отдельному установщику без дополнительного вопроса о закрытии: клиент закрывается, установщик заменяет приложение и запускает его снова. Доверие к локальному пакету подтверждает пользователь; автоматические обновления с GitHub по-прежнему требуют подписи релиза. Поддерживаются пакеты размером до 512 МиБ после распаковки.
