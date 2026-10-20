# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

Qt 6.2+, C++17, CMake 3.16+. No tests or linter are configured.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release   # may need -DCMAKE_PREFIX_PATH=<Qt6 dir>
cmake --build . -j
./mezzenger_server [--port 5555]      # default port 5555
./mezzenger_client
```

Two executables from one CMake project: `mezzenger_server` (Core/Network/Sql, no GUI) and `mezzenger_client` (Widgets). There is no shared library; `common/Protocol.h` is header-only and included by both via the `common` include dir. README and UI strings are in Russian.

## Architecture

**Wire protocol** (`common/Protocol.h`): length-prefixed JSON over TCP — 4-byte big-endian length header + compact JSON. `MaxMessageSize` is 1 MiB. Message kind is the string in the `"type"` field (`login`, `register`, `auth_response`, `user_list`, `message`, `message_history`, `get_offline_messages`, `search_user`, `search_user_response`, `mark_messages_read`, `error`). Adding a message type means updating three places: the `MessageType` enum, the `ProtocolHelper::create*` builder, and `parseMessageType` (unknown types silently map to `MessageType::Error`), plus handlers on server/client. Both `NetworkManager` (client) and `ClientHandler` (server) do their own buffering/framing of the stream using these helpers.

**Server**: `ServerCore` owns a `QTcpServer`, a `DatabaseServer` (SQLite, `server.db` in the CWD), and a `QMap<userId, ClientHandler*>` of online users. Each connection gets a `ClientHandler` that does auth (login/register), persists messages through `DatabaseServer`, and emits `messageReady` so `ServerCore::sendMessageToUser` routes to the recipient if online; otherwise messages stay undelivered in the DB and are sent via `deliverOfflineMessages` on next login. Single-threaded, event-loop driven.

**Client**: `main.cpp` shows `LoginWindow` (host/port/nick/password), then creates `MainWindow`, which owns a `NetworkManager` (socket + framing, emits `messageReceived(QJsonObject)`) and a `DatabaseClient`. `MainWindow` authenticates on connect and auto-reconnects with a 3s timer. Chat history is cached locally in SQLite (`mezzenger_client.db`, CWD) keyed by `user_id` + `my_nick`; unread state is tracked both locally and reported to the server via `mark_messages_read`. Note `DatabaseClient::initialize` ignores its `dbPath` argument and always uses the hardcoded filename. Styling comes from `client/styles.qss` (dark theme), listed in the client sources in CMake.
