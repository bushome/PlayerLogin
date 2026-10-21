# PlayerLogin (ASA)

One character per player across the cluster, duplicate prevention, bans and LastSeen lookups,
backed by MariaDB. Replaces the parts of Lethal Login this cluster uses.

## Install

1. Create the database and a user (the plugin creates its tables itself):
   ```sql
   CREATE DATABASE IF NOT EXISTS playerlogin CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
   CREATE USER IF NOT EXISTS 'playerlogin'@'localhost' IDENTIFIED BY 'change-me';
   GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX ON playerlogin.* TO 'playerlogin'@'localhost';
   ```
   Use `'playerlogin'@'<ip>'` instead of `localhost` for servers on other machines (you run `skip-name-resolve`).
2. Copy `PlayerLogin.dll`, `config.json`, `PluginInfo.json` into `ArkApi/Plugins/PlayerLogin/` on every server.
3. Set the MySQL fields in `config.json`. Every server must point at the same database.
4. Start the server and check the plugin log for this line:
   `PlayerLogin: this server is '<key>', profiles in <path>`
   Confirm the key is what you expect and the path is the folder holding that map's `.arkprofile` files.
   If the path is wrong, set `Verification.ProfileDirectory`.

## Server identity

`ServerKeyOverride` (config) → `-ServerKey=` on the command line → map name.
Each server must resolve to a distinct value, and it must not change once players are recorded
against it (a renamed key reads as "character is on another server").

## Commands (RCON and in-game admin console)

| Command | Effect |
|---|---|
| `playerlogin.ban <eosID> <hours> <reason>` | Timed ban; `0` hours = permanent. Kicks immediately on this server, within 60 s elsewhere. |
| `playerlogin.unban <eosID>` | Removes the ban. |
| `playerlogin.LastSeen <eosID>` | `<eos> was last seen on <server> at <time> UTC`, plus `(character uploaded, in transit)` if applicable. `No record for <eos>` otherwise. |
| `playerlogin.reset <eosID>` | Deletes the player's record. The next server they log into with a character becomes their record. |

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
so restarts and busy periods stay under Discord's rate limit. Posting runs on AsaApi's request threads, never the
game thread. If Discord is down, failures are logged and the messages are discarded; a channel holds at most 200
pending events. Timestamps are the server's local time; mentions in names are never pinged.

## Behavior

The record is checked at login, before the character spawns (one primary-key query on the game thread).

| Situation on login | Result |
|---|---|
| Banned | Never spawns. Held on character creation; a "Login refused" dialog shows the reason and OK returns them to the main menu (server kick ~23 s later if left open) |
| No record, has character here | Recorded here (backfill) |
| No record, no character here | Free to create or download; recorded once the character exists |
| Recorded here | Normal login; record refreshed, in-transit cleared |
| Recorded elsewhere, has a character here | Duplicate copy: never spawns. Held on character creation; "Login refused" dialog with "registered on X", OK returns them to the main menu (server kick ~23 s later if left open) |
| Recorded elsewhere, no character here | Normal flow, no message (they may be arriving by transfer). Downloading a transferred character works (record moves here); creating a character is refused and returns them to the creation screen with the "last seen on X" message |

The plugin never opens the "Download ARK Survivor" screen itself; that only appears through the game's own transfer flow.

ASA's kick drops the client with "lost connection" and no reason, so the reason is always shown first:
a center-screen dialog (the game's own notification dialog) plus a chat message. Blocked creations get a
"Character creation blocked" dialog whose OK keeps them on the creation screen. If the database doesn't answer at login,
the player spawns normally and the record is applied in the background (in-world kick after 4 s;
uploads are blocked while a kick is pending).

Obelisk upload marks the record in transit. Arrival on any server moves it there.

### Verification (owning server only)

Every `IntervalMinutes` (first run `StartupDelayMinutes` after boot) a server checks only the rows
recorded against itself, and removes a row only when **all** of these hold:
not in transit, player not online, `last_seen` older than `GraceMinutes`, and no
`<eosID>.arkprofile` in the profile folder. The delete is conditional on `last_seen` being unchanged,
so a login racing the check wins. Each removal is written to `verification_log` and the plugin log.

Safety: verification is skipped entirely if the profile folder has no `.arkprofile` files
(wrong path, mid-restore), and removes at most `MaxRemovalsPerRun` rows per pass.

`StaleEntryDays` (0 = off): a record untouched for that many days stops locking the player,
e.g. records pointing at a retired server.

## Build

AsaApi is a git submodule at `ASA/AsaApi`. After cloning this repo:
```
git submodule update --init --recursive
```
1. Build `ASA/AsaApi/AsaApi.sln` as **Release | x64** (produces `ASA/AsaApi/x64/Release/AsaApi.lib`).
2. Open `ASA/PlayerLogin.sln` in Visual Studio 2026 and build **Release | x64**.
   vcpkg (manifest mode, `vcpkg.json`) installs libmariadb and fmt on the first build.
3. Output: `ASA/x64/Release/` holds `PlayerLogin.dll`, `config.json` and `PluginInfo.json`, the whole plugin folder.

To build against a newer AsaApi: `cd ASA/AsaApi && git pull`, rebuild it, then commit the submodule bump.

### Build notes (apply to any AsaApi plugin)

- VS 2026's bundled vcpkg requires `builtin-baseline` in `vcpkg.json`. Use the same baseline as AsaApi's own `vcpkg.json` so shared libraries (fmt) are the same version.
- Project settings: Character Set = Unicode (AsaApi's UE headers need a 2-byte TCHAR), C++20, `/MD`, vcpkg "Use Static Libraries" + "Use Dynamic CRT" (= `x64-windows-static-md`, so dependencies link into the plugin DLL).
- AsaApi's `UnrealString.h` includes `<fmt/format.h>`: add `fmt` to `vcpkg.json`.
- Don't define `WIN32_LEAN_AND_MEAN`; AsaApi's `Base.h` already does.
- Include dirs: `AsaApi/AsaApi/Core/Public` and `AsaApi/AsaApi/Core/Public/API/UE`; link `AsaApi.lib` from `AsaApi/x64/Release`.

## Test checklist (dedicated test server, two maps in a test cluster)

Things that depend on game behavior I couldn't run here, in order of importance:

1. **New vs. existing player detection.** Brand-new EOS ID logs in: log must *not* show a backfill line until the
   character is created. Existing character logs in with no row: log shows `recorded on <key> (backfill on login)`.
   This relies on `HandleNewPlayer`'s player-data argument being null for players with no profile on the map.
2. **Creation block.** Player recorded on map A tries to create on map B: notification shown, they stay on the
   creation/download screen (not stuck on a loading spinner).
3. **Transfer.** Upload on A → row shows `in_transit=1`. Download on B → row moves to B within ~5 s, `in_transit=0`.
4. **Duplicate.** Copy a player's `.arkprofile` onto map B while their row says A → login on B kicks with the duplicate message.
5. **Profile path.** Confirm the startup log path, then delete a test profile, wait past grace, and confirm
   the `verification_log` entry.
6. **Bans.** Timed and permanent, RCON and console; ban a player online on the *other* map and confirm the kick within 60 s.
7. **LastSeen** from the crosschat bot's RCON connection.
