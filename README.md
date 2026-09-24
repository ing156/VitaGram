# VitaGram — Telegram homebrew client starter for PS Vita

VitaGram is a small **MVP/starter project** for a native PS Vita Telegram client. It uses VitaSDK + vita2d for the UI and the community `tdlib-vita` port for Telegram networking/authentication.

## Implemented in this starter

- TDLib user authorization by phone number
- login code
- 2FA password
- basic registration first-name step
- main chat list
- open a chat and load recent history
- receive new text messages while the app is open
- send text messages
- placeholders for photo/sticker/video/audio/file messages
- simple controller-driven on-screen ASCII keyboard

This is **not yet a production Telegram client**. Media download, notifications, unread counters, avatars, emoji/stickers rendering, groups management, calls, secret chats, QR login and a proper Unicode keyboard still need work.

## Prerequisites

1. Install VitaSDK and its package set.
2. Make sure `libvita2d`, `jansson`, OpenSSL and zlib are installed through VitaSDK/vdpm.
3. Clone the PS Vita TDLib port into this project:

```bash
git clone https://github.com/v-atamanenko/tdlib-vita external/tdlib-vita
```

4. Create a Telegram application at `https://my.telegram.org` → **API development tools**.
5. Edit `src/config.h` and set your own `VITAGRAM_API_ID` and `VITAGRAM_API_HASH`.

Do not publish your personal API credentials in a public repository.

## Build

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The expected output is `build/vitagram.vpk`.

### If TDLib cross-compilation fails

`tdlib-vita` is a Vita-specific fork, but TDLib is a large C++ project and its build can be sensitive to the exact VitaSDK/toolchain revision. Build its native code-generation target first if required, following the fork/upstream TDLib cross-compilation pattern, then rerun the Vita build.

## Controls

### Chat list
- D-pad: select chat
- X: open
- Triangle: refresh
- Start: exit

### Chat
- X: compose message
- O: back
- Triangle: refresh history
- Up/Down: scroll older/newer
- Start: exit

### Keyboard
- D-pad: move cursor
- X: type selected character
- Square: backspace
- Triangle: space
- Select: upper/lowercase
- Start: submit
- O: cancel

## Storage

TDLib data is stored in:

- `ux0:data/vitagram/db`
- `ux0:data/vitagram/files`

Treat these files as private account/session data.

## Architecture

- `src/main.cpp` — Vita event/render loop
- `src/telegram_client.*` — TDLib JSON wrapper, auth, chats, messages
- `src/keyboard.*` — controller-driven keyboard
- `src/vita_net.*` — Vita network initialization
- `src/config.h` — local Telegram API credentials

## Important compatibility note

This starter intentionally targets the API schema in the existing `tdlib-vita` fork, where `setTdlibParameters` contains a nested `parameters` object and the authorization flow includes `authorizationStateWaitEncryptionKey`. Newer upstream TDLib versions changed parts of this schema, so replacing the fork with current upstream TDLib requires adapting the JSON requests.

## License

The starter code in this archive may be used under the MIT License. `tdlib-vita`, TDLib, VitaSDK and vita2d have their own licenses; review them before redistribution.
