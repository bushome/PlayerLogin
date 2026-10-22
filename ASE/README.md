# PlayerLogin (ASE)

One character per player across the cluster, duplicate prevention, bans and LastSeen lookups,
backed by MariaDB. ASE port of the ASA plugin. Same design and database layout; players are keyed by **SteamID64**.

**Status: complete, in production on the ASE cluster.** Every feature below was tested on a test server and on live
(see [Verified](#verified)).

## Install

1. Create the database and a user (the plugin creates its tables itself):
   ```sql
   CREATE DATABASE IF NOT EXISTS playerlogin CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
   CREATE USER IF NOT EXISTS 'playerlogin'@'localhost' IDENTIFIED BY 'change-me';
   GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX ON playerlogin.* TO 'playerlogin'@'localhost';
   ```
   Use `'playerlogin'@'<ip>'` instead of `localhost` for servers on other machines (you run `skip-name-resolve`).
2. Copy the contents of `ASE/out/PlayerLogin/` (from the build) into `ShooterGame/Binaries/Win64/ArkApi/Plugins/PlayerLogin/` on every ASE server:
   `PlayerLogin.dll`, `PlayerLogin.pdb`, `config.json`, `config.commented.json`, `PluginInfo.json`.
3. Set the MySQL fields in `config.json`. Every server must point at the same database.
4. Start the server and check the plugin log for this line (ASE keeps profiles directly in `Saved\SavedArks`):
   `PlayerLogin: this server is '<key>', profiles in <path>`
   Confirm the key is what you expect and the path is the folder holding that map's `.arkprofile` files.
   If the path is wrong, set `Verification.ProfileDirectory`.

## Configuration

`config.json` sits next to `PlayerLogin.dll`. JSON can't hold comments, so every field is described here, and
`config.commented.json` (in `configs/`, and copied into `out/PlayerLogin/` by the build) is the same file with an explanation
on every setting. It is reference only: the plugin reads `config.json`, which must stay plain JSON with no comments.
Missing fields fall back to the defaults shown.

After editing `config.json`, run `playerlogin.reload` (RCON, or the in-game console as an admin):

- **Applied at once:** `Messages`, `Discord`, and `Verification` `Enabled`, `GraceMinutes`, `StaleEntryDays`,
  `MaxRemovalsPerRun`, `ProfileDirectory`.
- **Need a server restart:** `Mysql`, `StartupDelayMinutes`, `IntervalMinutes`. The reload keeps their
  running values and names any that changed in its reply.
- If `config.json` has a mistake, nothing changes: the reply and the plugin log show the error and the previous settings stay active.
- If `config.json` was broken when the server started, the plugin doesn't start but `playerlogin.reload` still works:
  fix the file and run it to start the plugin without a restart.

### `Mysql`

| Field | Default | Description |
|---|---|---|
| `Host` | `127.0.0.1` | MariaDB host name or IP. |
| `Port` | `3306` | MariaDB port. |
| `User` | *(empty)* | Database user. |
| `Password` | *(empty)* | Database password. Keep real credentials out of version control. |
| `Database` | `playerlogin` | Database name. The plugin creates its tables on first start. |
| `Ssl` | `false` | `false`: plain connection (local or LAN MariaDB without TLS). `true`: require TLS, accepting a self-signed server certificate. MariaDB Connector/C 3.4+ requires TLS by default, so leave this `false` unless your server has TLS set up. |

### `Verification`

| Field | Default | Minimum | Description |
|---|---|---|---|
| `Enabled` | `true` | | Turns the profile check on or off. |
| `StartupDelayMinutes` | `30` | `1` | Wait after boot before the first check, so the save has finished loading. |
| `IntervalMinutes` | `30` | `5` | Time between checks. |
| `GraceMinutes` | `60` | `20` | A record must be untouched this long before it can be removed. Keeps recent logouts and transfers safe. |
| `StaleEntryDays` | `0` | `0` | `0` = off (recommended). Otherwise a record untouched this many days stops locking the player. See [Verification](#verification-owning-server-only). |
| `MaxRemovalsPerRun` | `25` | `1` | Most records one check may remove. Limits the damage from a wrong profile path or a half-restored save. |
| `ProfileDirectory` | *(empty)* | | Folder holding this map's `.arkprofile` files. Empty = detected automatically (ASE: `Saved\SavedArks`, or `Saved\SavedArks\<MapName>` if that folder holds profiles). Check the startup log line and set this only if the detected path is wrong. |

### `Discord`

| Field | Default | Description |
|---|---|---|
| `LoginsWebhook` | *(empty)* | Joined, Left, New character created. |
| `TransfersWebhook` | *(empty)* | Character uploaded, Transfer arrived. |
| `DuplicatesWebhook` | *(empty)* | Creation blocked, Duplicate login blocked, Record removed by verification. |
| `ModerationWebhook` | *(empty)* | Ban, Unban, Record reset, Banned player kicked. |

Each field takes a Discord channel webhook URL. Empty turns that channel off; several fields can share one URL.
See [Discord logging](#discord-logging).

### `Messages`

Text shown to players. Placeholders are filled in by the plugin.

| Field | Placeholders | Shown when |
|---|---|---|
| `CreationBlocked` | `{server}` | They try to create a character while their record is on another server. |
| `DuplicateLogin` | `{server}` | They log in with a copy of their character on a server other than the recorded one. |
| `BannedPermanent` | `{reason}` | A permanently banned player joins, or is banned while online. |
| `BannedTimed` | `{expires}`, `{reason}` | A player with a timed ban joins, or is banned while online. `{expires}` is in UTC. |
| `RecordLoading` | | They try to create a character before their record has loaded (database slow or unreachable). |

Players are identified by SteamID64 in every command and table.

## Server identity

`-ServerKey=` on the command line, or the map name if it isn't set. There is no config setting for it.
Each server must resolve to a distinct value, and it must not change once players are recorded
against it (a renamed key reads as "character is on another server").

## Commands (RCON and in-game admin console)

| Command | Effect |
|---|---|
| `playerlogin.ban <steamID> <hours> <reason>` | Timed ban; `0` hours = permanent. Kicks immediately on this server, within 60 s elsewhere. |
| `playerlogin.unban <steamID>` | Removes the ban. |
| `playerlogin.LastSeen <steamID>` | `<steamID> was last seen on <server> at <time> UTC`, plus `(character uploaded, in transit)` if applicable. `No record for <steamID>` otherwise. |
| `playerlogin.reset <steamID>` | Deletes the player's record. The next server they log into with a character becomes their record. |
| `playerlogin.reload` | Re-reads `config.json`. See [Configuration](#configuration) for what applies at once. |

## Exclusions

```sql
INSERT INTO exclusions (server_key, note) VALUES ('TestingServer', 'admin test box / restores');
```
On an excluded server there's no map lock, no duplicate kick and nothing gets recorded. Bans still apply.
Picked up within 60 s; no restart needed.

## Discord logging

Optional. Paste a channel webhook URL into any of the `Discord` fields in `config.json`; leave a field empty to turn that channel off.
Several fields can share one URL.

| Field | Events |
|---|---|
| `LoginsWebhook` | Joined (with whether a character exists here), Left (with session length), New character created |
| `TransfersWebhook` | Character uploaded, Transfer arrived (from → to) |
| `DuplicatesWebhook` | Character creation blocked, Duplicate character login blocked, Character record removed by verification |
| `ModerationWebhook` | Ban, Unban, Character record reset, Banned player kicked |

Events are queued and each channel posts at most once every 2 s, combining whatever has queued into one message,
so restarts and busy periods stay under Discord's rate limit. Posting runs on the API's request threads, never the
game thread. If Discord is down, failures are logged and the messages are discarded; a channel holds at most 200
pending events. Timestamps are always UTC, whatever time zone the server machine is set to; mentions in names are never pinged.

Player posts carry the character name with PlayerId, `[Steam Name]`, `[SteamID]` and tribe. ASE tribes are per map,
so the tribe shown is the one on the server that posted; the line is left out when the player has no tribe there.

## Behavior

The record is checked at login, before the character spawns (one primary-key query on the game thread).

| Situation on login | Result |
|---|---|
| Banned | Never spawns; kicked ~2 s after joining. ASE shows the reason in its own "Kicked" dialog on the main menu |
| No record, has character here | Recorded here (backfill) |
| No record, no character here | Free to create or download; recorded once the character exists |
| Recorded here | Normal login; record refreshed, in-transit cleared |
| Recorded elsewhere, has a character here | Duplicate copy: never spawns; kicked ~2 s after joining with "registered on X" in ASE's "Kicked" dialog |
| Recorded elsewhere, no character here | Normal flow, no message (they may be arriving by transfer). Downloading a transferred character works (record moves here); creating a character is refused and returns them to the creation screen with the "last seen on X" message |

The plugin never opens the "Download ARK Survivor" screen itself; that only appears through the game's own transfer flow.

On ASE the kick itself carries the reason: the player is returned to the main menu with a "Kicked"
dialog showing the message. Blocked creations (which are not kicks) use the message-of-the-day box
plus a chat message and a notification. If the database doesn't answer at login,
the player spawns normally and the record is applied in the background (in-world kick after 4 s;
uploads are blocked while a kick is pending).

Obelisk upload marks the record in transit. Arrival on any server moves it there.

### Verification (owning server only)

Every `IntervalMinutes` (first run `StartupDelayMinutes` after boot) a server checks only the rows
recorded against itself, and removes a row only when **all** of these hold:
not in transit, player not online, `last_seen` older than `GraceMinutes`, and no
`<steamID>.arkprofile` in the profile folder. The delete is conditional on `last_seen` being unchanged,
so a login racing the check wins. Each removal is written to `verification_log` and the plugin log.

Safety: verification is skipped entirely if the profile folder has no `.arkprofile` files
(wrong path, mid-restore), and removes at most `MaxRemovalsPerRun` rows per pass.

`StaleEntryDays` (0 = off): a record untouched for that many days stops locking the player.
Leave it at 0: a returning player could otherwise create a second character while the old one still exists.
Reset returning players by hand (`playerlogin.reset`), and when retiring a map clear its rows in one go:
`DELETE FROM characters WHERE server_key='OldMap';`

## Build

`ASE/` is a self-contained Visual Studio solution. The server API is the
[ArkServerApi (AseApi)](https://github.com/ArkServerApi/AseApi) submodule at `ASE/extern/AseApi`, which ships a prebuilt
`out_lib/ArkApi.lib`, so there is nothing to build there. Clone with submodules:
```
git clone --recurse-submodules https://github.com/bushome/PlayerLogin.git
```
or, in an existing clone:
```
git submodule update --init --recursive
```
1. Open `ASE/PlayerLogin.sln` in Visual Studio 2026 and build **Release | x64**.
   vcpkg (manifest mode, `vcpkg.json`) installs libmariadb on the first build.
2. The build assembles `ASE/out/PlayerLogin/`: `PlayerLogin.dll`, `PlayerLogin.pdb`, `config.json`,
   `config.commented.json` and `PluginInfo.json`, the whole plugin folder. It also writes `ASE/out/PlayerLogin.dll.ArkApi`
   for the API's hot reload.

### Layout

| Path | Contents |
|---|---|
| `src/` | `PlayerLogin.cpp`, `Database.h` (MariaDB worker), `Discord.h` (webhooks) |
| `configs/` | `config.json`, `config.commented.json`, `PluginInfo.json` |
| `sql/` | `schema.sql` (the plugin creates these tables itself) |
| `extern/AseApi/` | AseApi submodule |
| `out/` | Build output (ignored by git) |

### Build notes (ASE plugins)

- C++20 (AseApi's `UE.h` uses `std::bit_cast`; include `<bit>` before `API/ARK/Ark.h`), Character Set = Unicode, `/MD`,
  vcpkg static + dynamic CRT.
- Include `extern/AseApi/version/Core/Public`; link `extern/AseApi/out_lib/ArkApi.lib`.
- `ARK_GAME` is defined by AseApi's `Ark.h`; don't define it in the project.
- No vcpkg `fmt`: the ASE headers use the fmt bundled with their spdlog.
- ASE API timers can't be cancelled when a plugin unloads, so periodic work runs from one
  `AddOnTimerCallback` that `Plugin_Unload` removes.

## Verified

Tested on a dedicated test server, then on the live cluster. `playerlogin.reload` and the `src/`, `configs/`,
`extern/`, `out/` project layout were added after this testing.


| Area | Result |
|---|---|
| Startup: server key, profile path (`SavedArks` root), database connection | Pass |
| Backfill on login (fresh table and after reset), Discord post with SteamID | Pass |
| Duplicate login: never spawns, kicked ~2 s after joining with the reason in ASE's "Kicked" dialog | Pass |
| Blocked creation: returned to the creation screen; message in the notification, MOTD box and chat | Pass |
| Timed ban (immediate kick while online, blocked on rejoin), permanent ban, unban | Pass |
| `LastSeen` and `reset` over RCON | Pass |
| Exclusions | Pass |
| Verification (tick-driven startup delay, grace, removal, `verification_log`) | Pass |
| Live obelisk transfers: one upload post, one arrival post, record moves | Pass |

Replaced on this cluster: NoDoubleLogin and PlayerActivity.
