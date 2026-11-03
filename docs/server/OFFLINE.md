# Offline mode

The port never connects to anything: no Konami servers, no account, no internet access.

## How the game handles its server

Every action that changes player data is a request class (`bsNetReq*`, 168 of them). The requests, their fields and their responses are catalogued in [ENDPOINTS.md](ENDPOINTS.md), generated from the game's metadata by `tools/server/gen_endpoint_catalog.py`.

In the Apple Arcade release each request class also has `EmulateServerProcess`, which computes the response from the save on the device (`gdUserSaveData`), and the client takes that path. The game data it needs (quests, drops, rewards, summons, shops, level curves) is in the master tables shipped with the game.

## The safety net

The network path is `bsSession.SendRequest`, a coroutine that POSTs an encrypted MessagePack form to `<server>/<name>.php` and hands the response to a `RecvCallback(www, version, bytes, bodyTop, header)`.

The recompiler replaces that coroutine's `MoveNext` (`bsSession.<SendRequest>d__22`, `--override 0x182b004=LocalServerMoveNext`) with `recomp/server/local_server.cpp`, which:

1. decodes the request's MessagePack parameters (`recomp/server/msgpack.cpp`),
2. runs a handler registered for the endpoint, if any, or answers with an empty body,
3. calls the request's `RecvCallback` with a successful header (`result` 0, the current time, session `offline`), as the HTTP path does after parsing the response,
4. logs the endpoint to `gosr_server.log`.

No handlers are registered at present: in normal play no request reaches this path. If one ever does, the log names it.
