# Входные данные для тестов

Эти текстовые образцы нужны проверкам и хранятся в Git. Они не входят в VPK.
Личные видео, фото, логи, дампы и результаты рендера размещайте в
`tests/local/` — эта папка исключена из Git.

| Файл | Где используется | Назначение |
| --- | --- | --- |
| `mtproxy-qr.txt` | `run-proxy-tests.sh` | Корректный QR импорта прокси |
| `invalid-proxy-qr.txt` | `run-proxy-tests.sh` | QR с некорректным прокси |
| `unrelated-qr.txt` | `run-proxy-tests.sh` | QR с посторонним содержимым |
| `palettes/bbr.json`, `palettes/brr2.json` | `palette_editor_test.js` | Импорт и сохранение цветовых ролей |

Новые входные файлы добавляются явно в список исключений `.gitignore`,
чтобы случайные медиа не попадали в публикацию.

Мультимедийные входы не хранятся в Git. Передайте свои файлы аргументами:

```bash
bash tests/run-webm-tests.sh /absolute/path/to/sticker.webm /absolute/path/to/alpha.webm
bash tests/run-tgs-policy-tests.sh /absolute/path/to/AnimatedSticker.tgs
```

Локальные копии `alpha.webm` и `AnimatedSticker.tgs` перенесены в
`tests/local/media/`; запуски без аргументов используют их, если они доступны.
Если образца нет, скрипт сообщает об этом и завершает работу без пропуска проверки.
