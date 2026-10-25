```
██████╗ ██╗      █████╗ ██╗   ██╗███████╗██████╗ 
██╔══██╗██║     ██╔══██╗╚██╗ ██╔╝██╔════╝██╔══██╗
██████╔╝██║     ███████║ ╚████╔╝ █████╗  ██████╔╝
██╔═══╝ ██║     ██╔══██║  ╚██╔╝  ██╔══╝  ██╔══██╗
██║     ███████╗██║  ██║   ██║   ███████╗██║  ██║
╚═╝     ╚══════╝╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝  ╚═╝
                                                 
██╗      ██████╗  ██████╗ ██╗███╗   ██╗          
██║     ██╔═══██╗██╔════╝ ██║████╗  ██║          
██║     ██║   ██║██║  ███╗██║██╔██╗ ██║          
██║     ██║   ██║██║   ██║██║██║╚██╗██║          
███████╗╚██████╔╝╚██████╔╝██║██║ ╚████║          
╚══════╝ ╚═════╝  ╚═════╝ ╚═╝╚═╝  ╚═══╝   
  ooo,    .---.
 o`  o   /    |\________________
o`   'oooo()  | ________   _   _)
`oo   o` \    |/        | | | |
  `ooo'   `---'         "-" |_|
                                
```
Cluster character location tracking for ARK servers: one character per player across the
cluster, duplicate prevention, timed/permanent bans and `LastSeen` lookups, backed by MariaDB.
Discord webhook logging for logins, transfers, blocked duplicates and moderation.

**Status: complete.** Both versions are tested and running in production. This software will receive only bug fixes for future game state version updates.
No new feature sets will be added beyond what is presented here. 

| Folder | Game | Server API (submodule) | Player key |
|---|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` | EOS ID |
| [`ASE`](ASE/README.md) | ARK: Survival Evolved | [ArkServerApi (AseApi)](https://github.com/ArkServerApi/AseApi) at `ASE/extern/AseApi` | SteamID64 |

Each folder is a self-contained Visual Studio solution. Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/PlayerLogin.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

Each game's cluster uses its own MariaDB database. The table layout is the same apart from the
player key column (`eos_id` on ASA, `steam_id` on ASE). `src/Database.h` is the same in both folders;
`src/PlayerLogin.cpp`, `src/Discord.h` and the project files differ by API. Install, commands,
configuration, behavior and build steps are in each folder's README.
