# PlayerLogin

Cluster character location tracking for ARK servers: one character per player across the
cluster, duplicate prevention, timed/permanent bans and `LastSeen` lookups, backed by MariaDB.

| Folder | Game | API |
|---|---|---|
| [`ASA/`](ASA/) | ARK: Survival Ascended | AsaApi |
| [`ASE/`](ASE/) | ARK: Survival Evolved | ArkApi (port in progress) |

Both versions share the same database schema, so they can point at one MariaDB instance
(use a separate database per game, since the clusters are separate).
