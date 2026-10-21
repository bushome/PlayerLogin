# PlayerLogin

Cluster character location tracking for ARK servers: one character per player across the
cluster, duplicate prevention, timed/permanent bans and `LastSeen` lookups, backed by MariaDB.
Discord webhook logging for logins, transfers, blocked duplicates and moderation.

**Status: complete.** Both versions are tested and running in production.

| Folder | Game | API | Player key | Status |
|---|---|---|---|---|
| [`ASA/`](ASA/) | ARK: Survival Ascended | AsaApi (submodule) | EOS ID | Complete, live |
| [`ASE/`](ASE/) | ARK: Survival Evolved | ARK Server API (submodule) | SteamID64 | Complete, live |

Each game's cluster uses its own MariaDB database. The table layout is the same apart from the
player key column (`eos_id` on ASA, `steam_id` on ASE). See each folder's README for install,
commands, configuration, behavior and build steps.
