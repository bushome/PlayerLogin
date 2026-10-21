-- PlayerLogin schema (MariaDB 10.5+)
-- The plugin creates these tables itself on startup (CREATE TABLE IF NOT EXISTS),
-- so running this file by hand is optional. Create the database and user first:
--
--   CREATE DATABASE IF NOT EXISTS playerlogin CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
--   CREATE USER IF NOT EXISTS 'playerlogin'@'localhost' IDENTIFIED BY 'change-me';
--   GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX ON playerlogin.* TO 'playerlogin'@'localhost';
--
-- All timestamps are unix seconds taken from the database clock (UNIX_TIMESTAMP()),
-- so every server in the cluster agrees on "now".

-- One row per player: the server their character currently belongs to.
CREATE TABLE IF NOT EXISTS characters (
    eos_id      VARCHAR(64)     NOT NULL,
    server_key  VARCHAR(64)     NOT NULL,             -- -ServerKey= value, or map name fallback
    player_id   BIGINT UNSIGNED NULL,                 -- ARK PlayerDataID of the character
    in_transit  TINYINT(1)      NOT NULL DEFAULT 0,   -- 1 after an obelisk upload, cleared on arrival
    last_seen   BIGINT          NOT NULL,             -- updated on login, logout, claim
    created_at  BIGINT          NOT NULL,
    PRIMARY KEY (eos_id),
    KEY idx_verify (server_key, in_transit, last_seen)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Bans. expires = 0 means permanent.
CREATE TABLE IF NOT EXISTS bans (
    eos_id      VARCHAR(64)  NOT NULL,
    expires     BIGINT       NOT NULL DEFAULT 0,
    reason      VARCHAR(255) NOT NULL DEFAULT '',
    banned_at   BIGINT       NOT NULL,
    banned_by   VARCHAR(64)  NOT NULL DEFAULT '',
    PRIMARY KEY (eos_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Servers exempt from the map lock and duplicate rules (bans still apply there).
-- Add a server:  INSERT INTO exclusions (server_key, note) VALUES ('TestingServer', 'admin test box');
CREATE TABLE IF NOT EXISTS exclusions (
    server_key  VARCHAR(64)  NOT NULL,
    note        VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (server_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Every row removed by profile verification, so you know which map and
-- roughly when a character went missing before searching archives.
CREATE TABLE IF NOT EXISTS verification_log (
    id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    eos_id      VARCHAR(64)     NOT NULL,
    server_key  VARCHAR(64)     NOT NULL,
    player_id   BIGINT UNSIGNED NULL,
    last_seen   BIGINT          NOT NULL,
    removed_at  BIGINT          NOT NULL,
    reason      VARCHAR(255)    NOT NULL,
    PRIMARY KEY (id),
    KEY idx_eos (eos_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
