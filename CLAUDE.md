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

**Wire protocol** (`common/Protocol.h`): length-prefixed JSON over TLS (TCP + TLS 1.2+) — 4-byte big-endian length header + compact JSON. `MaxMessageSize` is 1 MiB. Message kind is the string in the `"type"` field (`login`, `register`, `auth_response`, `user_list`, `message`, `message_history`, `get_offline_messages`, `search_user`, `search_user_response`, `mark_messages_read`, `message_ack`, `delivery_ack`, `error`). Adding a message type means updating three places: the `MessageType` enum, the `ProtocolHelper::create*` builder, and `parseMessageType` (unknown types silently map to `MessageType::Error`), plus handlers on server/client. `MainWindow`, `LoginWindow` (client) and `ClientHandler` (server) each do their own buffering/framing of the stream using these helpers. `client/NetworkManager` is currently unused dead code (and is still plain `QTcpSocket`).

**Server**: `ServerCore` owns a `QSslServer` (needs `server.crt`/`server.key`, see `scripts/gen-cert.sh`; plaintext clients are refused at handshake), a `DatabaseServer` (SQLite, `server.db` in the CWD), and a `QMap<userId, ClientHandler*>` of online users. Each connection gets a `ClientHandler` that does auth (login/register), persists messages through `DatabaseServer`, and emits `messageReady` so `ServerCore::sendMessageToUser` routes to the recipient if online; otherwise messages stay undelivered in the DB and are sent via `deliverOfflineMessages` on next login. Single-threaded, event-loop driven.

**Delivery guarantees**: a message becomes `delivered` on the server only after the recipient sends `delivery_ack` (writing to the socket is not enough). Unacked messages are offered again via `get_offline_messages` (batches of 100, ordered by id). The sender attaches a `client_msg_id` (UUID); the server stores it with a unique index per sender, answers with `message_ack` (also for retries) and does not forward duplicates. The client stores outgoing messages locally with `status = 0` until `message_ack`, and resends them after each login (when `user_list` arrives); incoming messages are deduplicated by `(my_nick, server_id)`. Old clients that don't send acks would get every message redelivered at each login.

**Client**: `main.cpp` shows `LoginWindow` (host/port/nick/password), then creates `MainWindow`, which owns a `QSslSocket` (framing is done in `MainWindow` itself) and a `DatabaseClient`. Both windows open their own TLS connection (`LoginWindow` for the auth check, `MainWindow` for the session). Server certificates are self-signed and trusted by pinned SHA-256 fingerprint (`client/TlsTrust`, stored in `QSettings` under `trusted/<host>:<port>`; confirmed by the user on first connect). `QSslSocket::ignoreSslErrors` is sticky per socket, so `TlsTrust` resets it on every connection attempt — keep that or reconnects skip the fingerprint check. `MainWindow` authenticates on connect and auto-reconnects with a 3s timer. Chat history is cached locally in SQLite (`mezzenger_client.db`, CWD) keyed by `user_id` + `my_nick`; unread state is tracked both locally and reported to the server via `mark_messages_read`. `DatabaseClient::initialize` migrates a legacy `messenger_client.db` to the new name on first start. Styling comes from `client/styles.qss` (dark theme), listed in the client sources in CMake.
