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

## Behavior

| Situation on login | Result |
|---|---|
| Banned | Kicked with the ban reason |
| No record, has character here | Recorded here (backfill) |
| No record, no character here | Free to create or download; recorded once the character exists |
| Recorded here | Normal login; record refreshed, in-transit cleared |
| Recorded elsewhere, has a character here | Duplicate copy: kicked, "registered on X" |
| Recorded elsewhere, no character here | Can download a transferred character (record moves here); creation blocked, "last seen on X" |

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

Requires the AsaApi repo (headers) and its Release `AsaApi.lib`, plus vcpkg.
```
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DASAAPI_DIR=C:/src/AsaApi -DASAAPI_LIB=C:/src/AsaApi/x64/Release/AsaApi.lib
cmake --build build --config Release
```
libmariadb is linked statically, so no extra DLLs ship with the plugin.

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
