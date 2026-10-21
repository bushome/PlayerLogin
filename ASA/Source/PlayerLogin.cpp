// PlayerLogin - cluster character location tracking for ARK: Survival Ascended (AsaApi)
//
//  - One character per player across the cluster: the server holding their character is
//    recorded in MariaDB; creating a character anywhere else is blocked.
//  - Duplicate prevention: logging in with a stale copy of the character on any server other
//    than the recorded one gets the player kicked (covers map restores from archives).
//  - Backfill: a player with no record who logs in with an existing character is recorded there.
//  - Obelisk transfers: upload marks the record "in transit", arrival moves it.
//  - Bans (timed / permanent) enforced at login and by a periodic sweep.
//  - Profile verification: each server removes records it owns whose character profile is gone.
//
// Threading: every SQL statement from hooks runs on one background worker thread (DbWorker).
// Results come back to the game thread through GameQueue, drained by a 1 s game-thread timer.
// Admin/RCON commands use a second connection on the game thread (single-row primary-key
// statements, issued only when an admin or the Discord bot runs a command).

#include <API/ARK/Ark.h>
#include <Timer.h>
#include <Logger/Logger.h>
#include <json.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Database.h"

#pragma comment(lib, "AsaApi.lib")

namespace fs = std::filesystem;
using namespace PlayerLogin;

// ---------------------------------------------------------------------------------------------
// Config and state
// ---------------------------------------------------------------------------------------------

namespace
{
	struct Messages
	{
		std::string creation_blocked = "Your character was last seen on {server}. Log in there to play it.";
		std::string duplicate_login = "Your character is registered on {server}. You can only log in with it there.";
		std::string banned_permanent = "You are banned from this cluster. Reason: {reason}";
		std::string banned_timed = "You are banned until {expires} UTC. Reason: {reason}";
		std::string record_loading = "Checking your character record. Try again in a moment.";
	};

	struct VerifyConfig
	{
		bool enabled = true;
		int startup_delay_min = 30;
		int interval_min = 30;
		int grace_min = 60;
		int stale_entry_days = 0;  // 0 = off
		int max_removals_per_run = 25;
		std::string profile_dir;   // empty = auto
	};

	struct Config
	{
		DbConfig db;
		std::string server_key_override;
		VerifyConfig verify;
		Messages msg;
	};

	// What we know about one online player (game thread only).
	struct PlayerEntry
	{
		bool loaded = false;           // record query has come back
		bool kicked = false;           // ban or duplicate kick issued; never claim
		bool had_data_at_login = false;// a character profile existed on this server when they joined
		bool has_row = false;
		std::string row_server;
		unsigned long long row_player_id = 0;
		bool row_in_transit = false;
	};

	Config g_cfg;
	std::string g_server_key;         // identity of this server in the database
	fs::path g_profile_dir;           // where this server's .arkprofile files live
	std::atomic<bool> g_alive{ false };
	bool g_this_excluded = false;     // game thread

	std::unique_ptr<DbWorker> g_worker;
	std::unique_ptr<Db> g_cmd_db;     // game-thread connection for admin/RCON commands
	GameQueue g_game_queue;
	std::unordered_map<std::string, PlayerEntry> g_players; // keyed by lower-case EOS ID

	// -----------------------------------------------------------------------------------------
	// Small helpers
	// -----------------------------------------------------------------------------------------

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	std::string ToStd(const FString& s) { return s.ToString(); }
	FString ToF(const std::string& s) { return FString::FromStringUTF8(s); }

	bool IsEosId(const std::string& s)
	{
		if (s.size() != 32)
			return false;
		return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
	}

	std::string Replace(std::string text, const std::string& token, const std::string& value)
	{
		for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
			text.replace(pos, token.size(), value);
		return text;
	}

	std::string EosOf(AController* pc)
	{
		if (!pc)
			return {};
		return Lower(ToStd(AsaApi::IApiUtils::GetEOSIDFromController(pc)));
	}

	unsigned long long ToU64(const std::optional<std::string>& v)
	{
		if (!v || v->empty())
			return 0;
		try { return std::stoull(*v); }
		catch (...) { return 0; }
	}

	long long ToI64(const std::optional<std::string>& v)
	{
		if (!v || v->empty())
			return 0;
		try { return std::stoll(*v); }
		catch (...) { return 0; }
	}

	AShooterGameMode* GameMode() { return AsaApi::GetApiUtils().GetShooterGameMode(); }

	void Kick(AShooterPlayerController* pc, const std::string& reason)
	{
		if (!pc || !GameMode())
			return;
		Log::GetLog()->info("Kicking {}: {}", EosOf(pc), reason);
		GameMode()->KickPlayerController(pc, ToF(reason));
	}

	void Notify(AShooterPlayerController* pc, const std::string& text)
	{
		if (!pc)
			return;
		const FString msg = ToF(text);
		AsaApi::GetApiUtils().SendNotification(pc, FLinearColor(1.0f, 0.35f, 0.35f, 1.0f), 1.3f, 12.0f, nullptr, L"{}", *msg);
		AsaApi::GetApiUtils().SendServerMessage(pc, FLinearColor(1.0f, 0.35f, 0.35f, 1.0f), L"{}", *msg);
	}

	std::string BanMessage(long long expires, const std::string& expires_text, const std::string& reason)
	{
		const std::string base = expires == 0 ? g_cfg.msg.banned_permanent : g_cfg.msg.banned_timed;
		return Replace(Replace(base, "{reason}", reason), "{expires}", expires_text);
	}

	// Directory of this DLL (the plugin folder).
	fs::path PluginDir()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&PluginDir), &module);
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(module, buf, MAX_PATH);
		return fs::path(buf).parent_path();
	}

	// -ServerKey=Value from the process command line, if present.
	std::string ServerKeyFromCommandLine()
	{
		const std::wstring cmd = GetCommandLineW();
		std::wstring lower = cmd;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
		const std::wstring needle = L"-serverkey=";
		const size_t pos = lower.find(needle);
		if (pos == std::wstring::npos)
			return {};

		size_t start = pos + needle.size();
		std::wstring value;
		if (start < cmd.size() && cmd[start] == L'"')
		{
			const size_t end = cmd.find(L'"', start + 1);
			value = cmd.substr(start + 1, end == std::wstring::npos ? std::wstring::npos : end - start - 1);
		}
		else
		{
			const size_t end = cmd.find_first_of(L" \t\"?", start);
			value = cmd.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
		}
		return AsaApi::Tools::Utf8Encode(value);
	}

	std::string MapName()
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return {};
		FString name;
		world->GetMapName(&name);
		return ToStd(name);
	}

	// -----------------------------------------------------------------------------------------
	// Config loading
	// -----------------------------------------------------------------------------------------

	void LoadConfig()
	{
		const fs::path path = PluginDir() / "config.json";
		std::ifstream file(path);
		if (!file.is_open())
			throw std::runtime_error("Can't open " + path.string());

		nlohmann::json j;
		file >> j;

		const auto& m = j.at("Mysql");
		g_cfg.db.host = m.value("Host", "127.0.0.1");
		g_cfg.db.port = m.value("Port", 3306u);
		g_cfg.db.user = m.value("User", "");
		g_cfg.db.password = m.value("Password", "");
		g_cfg.db.database = m.value("Database", "playerlogin");

		g_cfg.server_key_override = j.value("ServerKeyOverride", "");

		if (j.contains("Verification"))
		{
			const auto& v = j["Verification"];
			g_cfg.verify.enabled = v.value("Enabled", true);
			g_cfg.verify.startup_delay_min = std::max(1, v.value("StartupDelayMinutes", 30));
			g_cfg.verify.interval_min = std::max(5, v.value("IntervalMinutes", 30));
			g_cfg.verify.grace_min = std::max(20, v.value("GraceMinutes", 60));
			g_cfg.verify.stale_entry_days = std::max(0, v.value("StaleEntryDays", 0));
			g_cfg.verify.max_removals_per_run = std::max(1, v.value("MaxRemovalsPerRun", 25));
			g_cfg.verify.profile_dir = v.value("ProfileDirectory", "");
		}

		if (j.contains("Messages"))
		{
			const auto& msg = j["Messages"];
			g_cfg.msg.creation_blocked = msg.value("CreationBlocked", g_cfg.msg.creation_blocked);
			g_cfg.msg.duplicate_login = msg.value("DuplicateLogin", g_cfg.msg.duplicate_login);
			g_cfg.msg.banned_permanent = msg.value("BannedPermanent", g_cfg.msg.banned_permanent);
			g_cfg.msg.banned_timed = msg.value("BannedTimed", g_cfg.msg.banned_timed);
			g_cfg.msg.record_loading = msg.value("RecordLoading", g_cfg.msg.record_loading);
		}
	}

	// -----------------------------------------------------------------------------------------
	// Database jobs (worker thread)
	// -----------------------------------------------------------------------------------------

	const char* kSchema[] = {
		"CREATE TABLE IF NOT EXISTS characters ("
		" eos_id VARCHAR(64) NOT NULL, server_key VARCHAR(64) NOT NULL, player_id BIGINT UNSIGNED NULL,"
		" in_transit TINYINT(1) NOT NULL DEFAULT 0, last_seen BIGINT NOT NULL, created_at BIGINT NOT NULL,"
		" PRIMARY KEY (eos_id), KEY idx_verify (server_key, in_transit, last_seen)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

		"CREATE TABLE IF NOT EXISTS bans ("
		" eos_id VARCHAR(64) NOT NULL, expires BIGINT NOT NULL DEFAULT 0, reason VARCHAR(255) NOT NULL DEFAULT '',"
		" banned_at BIGINT NOT NULL, banned_by VARCHAR(64) NOT NULL DEFAULT '', PRIMARY KEY (eos_id)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

		"CREATE TABLE IF NOT EXISTS exclusions ("
		" server_key VARCHAR(64) NOT NULL, note VARCHAR(255) NOT NULL DEFAULT '', PRIMARY KEY (server_key)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

		"CREATE TABLE IF NOT EXISTS verification_log ("
		" id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, eos_id VARCHAR(64) NOT NULL, server_key VARCHAR(64) NOT NULL,"
		" player_id BIGINT UNSIGNED NULL, last_seen BIGINT NOT NULL, removed_at BIGINT NOT NULL, reason VARCHAR(255) NOT NULL,"
		" PRIMARY KEY (id), KEY idx_eos (eos_id)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	};

	// Shared SQL for "this character now lives on this server".
	std::string ClaimSql(Db& db, const std::string& eos, unsigned long long player_id)
	{
		const std::string key = db.Esc(g_server_key);
		const std::string pid = player_id ? std::to_string(player_id) : "NULL";
		return "INSERT INTO characters (eos_id, server_key, player_id, in_transit, last_seen, created_at) VALUES ('"
			+ db.Esc(eos) + "','" + key + "'," + pid + ",0,UNIX_TIMESTAMP(),UNIX_TIMESTAMP())"
			" ON DUPLICATE KEY UPDATE server_key=VALUES(server_key),"
			" player_id=COALESCE(VALUES(player_id), player_id), in_transit=0, last_seen=UNIX_TIMESTAMP()";
	}

	void PostClaim(const std::string& eos, unsigned long long player_id, const char* why)
	{
		if (!g_worker)
			return;
		g_worker->Post([eos, player_id, why](Db& db)
		{
			if (db.Exec(ClaimSql(db, eos, player_id)) < 0)
				Log::GetLog()->error("Claim failed for {}: {}", eos, db.LastError());
			else
				Log::GetLog()->info("{} recorded on {} ({})", eos, g_server_key, why);
		});
	}

	// Record (or re-record) a player here and update the cache.
	void Claim(PlayerEntry& entry, const std::string& eos, unsigned long long player_id, const char* why)
	{
		entry.has_row = true;
		entry.row_server = g_server_key;
		entry.row_in_transit = false;
		if (player_id)
			entry.row_player_id = player_id;
		PostClaim(eos, player_id, why);
	}

	// -----------------------------------------------------------------------------------------
	// Login processing
	// -----------------------------------------------------------------------------------------

	struct LoginResult
	{
		bool ok = false;
		bool excluded = false;
		bool has_row = false;
		std::string server;
		unsigned long long player_id = 0;
		bool in_transit = false;
		long long last_seen = 0;
		long long now = 0;
		bool banned = false;
		long long ban_expires = 0;
		std::string ban_expires_text;
		std::string ban_reason;
	};

	// Game thread: act on a player's record once it has loaded.
	void OnRecordLoaded(const std::string& eos, const LoginResult& r)
	{
		auto it = g_players.find(eos);
		if (it == g_players.end())
			return; // already logged out
		PlayerEntry& entry = it->second;
		AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(ToF(eos));
		if (!pc)
			return;

		if (!r.ok)
		{
			// Leave entry unloaded: creation stays blocked with the "checking" message, and the
			// reconcile timer retries the load. Never kick on a database error.
			return;
		}

		g_this_excluded = r.excluded;

		entry.loaded = true;

		if (r.banned)
		{
			entry.kicked = true;
			Kick(pc, BanMessage(r.ban_expires, r.ban_expires_text, r.ban_reason));
			return;
		}

		entry.has_row = r.has_row;
		entry.row_server = r.server;
		entry.row_player_id = r.player_id;
		entry.row_in_transit = r.in_transit;

		if (r.excluded)
			return; // exempt server: no recording, no map lock

		// Optional backstop: an entry untouched for StaleEntryDays no longer locks the player.
		if (entry.has_row && entry.row_server != g_server_key && g_cfg.verify.stale_entry_days > 0
			&& r.now - r.last_seen > static_cast<long long>(g_cfg.verify.stale_entry_days) * 86400)
		{
			Log::GetLog()->info("{}: entry on {} is older than {} days, treating as expired",
				eos, entry.row_server, g_cfg.verify.stale_entry_days);
			entry.has_row = false;
		}

		if (!entry.has_row)
		{
			// Backfill: existing character here and no record anywhere -> it's theirs.
			if (entry.had_data_at_login)
				Claim(entry, eos, AsaApi::IApiUtils::GetPlayerID(pc), "backfill on login");
			return; // no character: creation or download will claim
		}

		if (entry.row_server == g_server_key)
		{
			// Their home server. Re-assert the row (upsert also restores it if verification
			// removed it moments ago) and clear any in-transit flag from a failed upload.
			if (entry.had_data_at_login)
				Claim(entry, eos, AsaApi::IApiUtils::GetPlayerID(pc), "login");
			return;
		}

		// Recorded somewhere else.
		if (entry.had_data_at_login)
		{
			// A copy of their character exists here (old save, restored archive): duplicate.
			entry.kicked = true;
			Kick(pc, Replace(g_cfg.msg.duplicate_login, "{server}", entry.row_server));
			return;
		}
		// No character here: they may download a transferred character; creation stays blocked.
	}

	void QueueRecordLoad(const std::string& eos)
	{
		if (!g_worker)
			return;
		g_worker->Post([eos](Db& db)
		{
			LoginResult r;
			const std::string e = db.Esc(eos);
			const std::string key = db.Esc(g_server_key);

			auto row = db.Query("SELECT server_key, player_id, in_transit, last_seen, UNIX_TIMESTAMP() FROM characters WHERE eos_id='" + e + "'");
			auto ban = db.Query("SELECT expires, reason, DATE_FORMAT(FROM_UNIXTIME(expires), '%Y-%m-%d %H:%i') FROM bans"
				" WHERE eos_id='" + e + "' AND (expires=0 OR expires>UNIX_TIMESTAMP())");
			auto excl = db.Query("SELECT 1 FROM exclusions WHERE server_key='" + key + "'");
			auto now = db.Query("SELECT UNIX_TIMESTAMP()");

			if (row && ban && excl && now)
			{
				r.ok = true;
				r.excluded = !excl->empty();
				r.now = now->empty() ? 0 : ToI64((*now)[0][0]);
				if (!row->empty())
				{
					const DbRow& x = (*row)[0];
					r.has_row = true;
					r.server = x[0].value_or("");
					r.player_id = ToU64(x[1]);
					r.in_transit = ToI64(x[2]) != 0;
					r.last_seen = ToI64(x[3]);
				}
				if (!ban->empty())
				{
					const DbRow& b = (*ban)[0];
					r.banned = true;
					r.ban_expires = ToI64(b[0]);
					r.ban_reason = b[1].value_or("");
					r.ban_expires_text = b[2].value_or("");
				}
			}
			else
			{
				Log::GetLog()->error("Record load failed for {}: {}", eos, db.LastError());
			}

			g_game_queue.Post([eos, r] { OnRecordLoaded(eos, r); });
		});
	}

	// -----------------------------------------------------------------------------------------
	// Periodic work
	// -----------------------------------------------------------------------------------------

	template <typename Fn>
	void ForEachOnlinePlayer(Fn&& fn)
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return;
		for (TWeakObjectPtr<APlayerController> weak : world->PlayerControllerListField())
		{
			auto* pc = static_cast<AShooterPlayerController*>(weak.Get());
			if (pc)
				fn(pc);
		}
	}

	// Every 5 s: record characters that appeared through creation or a transfer download,
	// and retry record loads that failed.
	void Reconcile()
	{
		ForEachOnlinePlayer([](AShooterPlayerController* pc)
		{
			const std::string eos = EosOf(pc);
			if (eos.empty())
				return;
			auto it = g_players.find(eos);
			if (it == g_players.end())
				return;
			PlayerEntry& entry = it->second;

			if (!entry.loaded)
			{
				QueueRecordLoad(eos);
				return;
			}
			if (entry.kicked || g_this_excluded || !pc->GetPlayerCharacter())
				return;

			const bool home_here = entry.has_row && entry.row_server == g_server_key && !entry.row_in_transit;
			if (home_here)
				return;

			// Recorded elsewhere but joined with a character here = duplicate, handled at login.
			if (entry.has_row && entry.row_server != g_server_key && entry.had_data_at_login)
				return;

			// Character exists here now: created (allowed) or downloaded via obelisk.
			Claim(entry, eos, AsaApi::IApiUtils::GetPlayerID(pc), entry.has_row ? "transfer arrival" : "new character");
		});
	}

	// Every 60 s: refresh exclusions and kick any online player banned since they joined.
	void Sweep()
	{
		if (!g_worker)
			return;
		g_worker->Post([](Db& db)
		{
			auto excl = db.Query("SELECT 1 FROM exclusions WHERE server_key='" + db.Esc(g_server_key) + "'");
			auto bans = db.Query("SELECT eos_id, expires, reason, DATE_FORMAT(FROM_UNIXTIME(expires), '%Y-%m-%d %H:%i')"
				" FROM bans WHERE expires=0 OR expires>UNIX_TIMESTAMP()");
			if (!excl || !bans)
				return;

			const bool excluded = !excl->empty();
			std::unordered_map<std::string, std::string> active;
			for (const DbRow& b : *bans)
				active[Lower(b[0].value_or(""))] = BanMessage(ToI64(b[1]), b[3].value_or(""), b[2].value_or(""));

			g_game_queue.Post([excluded, active]
			{
				g_this_excluded = excluded;
				if (active.empty())
					return;
				std::vector<std::pair<AShooterPlayerController*, std::string>> kicks;
				ForEachOnlinePlayer([&](AShooterPlayerController* pc)
				{
					auto it = active.find(EosOf(pc));
					if (it != active.end())
						kicks.emplace_back(pc, it->second);
				});
				for (auto& [pc, msg] : kicks)
				{
					auto it = g_players.find(EosOf(pc));
					if (it != g_players.end())
						it->second.kicked = true;
					Kick(pc, msg);
				}
			});
		});
	}

	// Every IntervalMinutes: remove this server's records whose character profile is gone.
	void Verify()
	{
		if (!g_worker || !g_cfg.verify.enabled || g_this_excluded)
			return;

		std::unordered_set<std::string> online;
		ForEachOnlinePlayer([&](AShooterPlayerController* pc) { online.insert(EosOf(pc)); });

		const fs::path dir = g_profile_dir;
		const VerifyConfig vc = g_cfg.verify;

		g_worker->Post([online, dir, vc](Db& db)
		{
			// Safety: never act on a profile folder that doesn't look like a live save.
			std::error_code ec;
			bool has_profiles = false;
			if (fs::is_directory(dir, ec))
			{
				for (const auto& f : fs::directory_iterator(dir, ec))
				{
					if (f.path().extension() == ".arkprofile")
					{
						has_profiles = true;
						break;
					}
				}
			}
			if (!has_profiles)
			{
				Log::GetLog()->warn("Verification skipped: no .arkprofile files in {}", dir.string());
				return;
			}

			const std::string key = db.Esc(g_server_key);
			auto rows = db.Query("SELECT eos_id, player_id, last_seen FROM characters WHERE server_key='" + key
				+ "' AND in_transit=0 AND last_seen < UNIX_TIMESTAMP() - " + std::to_string(vc.grace_min * 60));
			if (!rows)
			{
				Log::GetLog()->error("Verification query failed: {}", db.LastError());
				return;
			}

			int removed = 0, candidates = 0;
			for (const DbRow& r : *rows)
			{
				const std::string eos = Lower(r[0].value_or(""));
				if (eos.empty() || online.count(eos))
					continue;
				if (fs::exists(dir / (eos + ".arkprofile"), ec))
					continue;

				++candidates;
				if (removed >= vc.max_removals_per_run)
					continue;

				const std::string last_seen = std::to_string(ToI64(r[2]));
				const std::string pid = r[1] ? *r[1] : "NULL";
				// Conditional delete: only if nothing touched the row since we read it.
				const long long n = db.Exec("DELETE FROM characters WHERE eos_id='" + db.Esc(eos) + "' AND server_key='" + key
					+ "' AND in_transit=0 AND last_seen=" + last_seen);
				if (n == 1)
				{
					++removed;
					db.Exec("INSERT INTO verification_log (eos_id, server_key, player_id, last_seen, removed_at, reason) VALUES ('"
						+ db.Esc(eos) + "','" + key + "'," + pid + "," + last_seen + ",UNIX_TIMESTAMP(),'no profile in save')");
					Log::GetLog()->warn("Verification removed {} (PlayerId {}) from {}: no profile in save", eos, pid, g_server_key);
				}
			}

			if (candidates > removed)
				Log::GetLog()->warn("Verification: {} more missing profiles left for the next run (MaxRemovalsPerRun={})",
					candidates - removed, vc.max_removals_per_run);
		});
	}
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

DECLARE_HOOK(AShooterGameMode_HandleNewPlayer_Implementation, bool, AShooterGameMode*, AShooterPlayerController*, UPrimalPlayerData*, AShooterCharacter*, bool);
DECLARE_HOOK(AShooterGameMode_Logout, void, AShooterGameMode*, AController*);
DECLARE_HOOK(AShooterPlayerState_ServerRequestCreateNewPlayer_Implementation, void, AShooterPlayerState*, FPrimalPlayerCharacterConfigStructReplicated*);
DECLARE_HOOK(AShooterPlayerController_ServerUploadCurrentCharacterAndItems_Implementation, void, AShooterPlayerController*, UPrimalInventoryComponent*, int);

bool Hook_AShooterGameMode_HandleNewPlayer_Implementation(AShooterGameMode* _this, AShooterPlayerController* pc,
	UPrimalPlayerData* player_data, AShooterCharacter* character, bool is_from_login)
{
	const bool result = AShooterGameMode_HandleNewPlayer_Implementation_original(_this, pc, player_data, character, is_from_login);

	// g_server_key is empty until the world is ready; anyone joining before that is picked up
	// by InitWorldDependent's sweep of online players.
	if (g_alive && is_from_login && pc && !g_server_key.empty())
	{
		const std::string eos = EosOf(pc);
		if (!eos.empty())
		{
			PlayerEntry entry;
			entry.had_data_at_login = player_data != nullptr;
			g_players[eos] = entry;
			QueueRecordLoad(eos);
		}
	}
	return result;
}

void Hook_AShooterGameMode_Logout(AShooterGameMode* _this, AController* exiting)
{
	if (g_alive && exiting)
	{
		const std::string eos = EosOf(exiting);
		auto it = g_players.find(eos);
		if (it != g_players.end())
		{
			const PlayerEntry& entry = it->second;
			if (!g_this_excluded && entry.has_row && entry.row_server == g_server_key && g_worker)
			{
				// Touch last_seen so the verification grace window starts at logout.
				g_worker->Post([eos](Db& db)
				{
					db.Exec("UPDATE characters SET last_seen=UNIX_TIMESTAMP() WHERE eos_id='" + db.Esc(eos)
						+ "' AND server_key='" + db.Esc(g_server_key) + "'");
				});
			}
			g_players.erase(it);
		}
	}
	AShooterGameMode_Logout_original(_this, exiting);
}

void Hook_AShooterPlayerState_ServerRequestCreateNewPlayer_Implementation(AShooterPlayerState* _this,
	FPrimalPlayerCharacterConfigStructReplicated* config)
{
	AShooterPlayerController* pc = (g_alive && _this) ? _this->GetShooterController() : nullptr;
	if (pc && !g_this_excluded)
	{
		const std::string eos = EosOf(pc);
		auto it = g_players.find(eos);

		if (it == g_players.end() || !it->second.loaded)
		{
			Notify(pc, g_cfg.msg.record_loading);
			pc->ClientShowCharacterCreationUI(true);
			return; // blocked until we know where their character is
		}

		const PlayerEntry& entry = it->second;
		if (entry.has_row && entry.row_server != g_server_key)
		{
			Log::GetLog()->info("Blocked character creation for {} (recorded on {})", eos, entry.row_server);
			Notify(pc, Replace(g_cfg.msg.creation_blocked, "{server}", entry.row_server));
			pc->ClientShowCharacterCreationUI(true);
			return;
		}
	}

	AShooterPlayerState_ServerRequestCreateNewPlayer_Implementation_original(_this, config);
	// The new character is recorded by Reconcile once its pawn exists.
}

void Hook_AShooterPlayerController_ServerUploadCurrentCharacterAndItems_Implementation(AShooterPlayerController* _this,
	UPrimalInventoryComponent* inventory, int for_club_ark)
{
	AShooterPlayerController_ServerUploadCurrentCharacterAndItems_Implementation_original(_this, inventory, for_club_ark);

	if (!g_alive || !_this || g_this_excluded || !g_worker)
		return;

	const std::string eos = EosOf(_this);
	auto it = g_players.find(eos);
	if (it == g_players.end() || !it->second.has_row || it->second.row_server != g_server_key)
		return;

	// Observer only: mark the record in transit so verification leaves it alone until the
	// character arrives somewhere. If the upload failed, the next login here clears the flag.
	it->second.row_in_transit = true;
	g_worker->Post([eos](Db& db)
	{
		db.Exec("UPDATE characters SET in_transit=1, last_seen=UNIX_TIMESTAMP() WHERE eos_id='" + db.Esc(eos)
			+ "' AND server_key='" + db.Esc(g_server_key) + "'");
		Log::GetLog()->info("{} uploaded from {} (in transit)", eos, g_server_key);
	});
}

// ---------------------------------------------------------------------------------------------
// Commands (RCON + admin console). Single-row statements on the game-thread connection.
// ---------------------------------------------------------------------------------------------

namespace
{
	std::vector<std::string> Split(const FString& body)
	{
		TArray<FString> parts;
		body.ParseIntoArray(parts, L" ", true);
		std::vector<std::string> out;
		for (const FString& p : parts)
			out.push_back(ToStd(p));
		return out;
	}

	Db& CmdDb()
	{
		if (!g_cmd_db)
			g_cmd_db = std::make_unique<Db>(g_cfg.db);
		return *g_cmd_db;
	}

	std::string CmdBan(const std::vector<std::string>& a, const std::string& by)
	{
		if (a.size() < 4)
			return "Usage: playerlogin.ban <eosID> <hours, 0 = permanent> <reason>";
		const std::string eos = Lower(a[1]);
		if (!IsEosId(eos))
			return "Invalid EOS ID: " + a[1];

		long long hours = 0;
		try { hours = std::stoll(a[2]); }
		catch (...) { return "Invalid hours: " + a[2]; }
		if (hours < 0)
			return "Hours must be 0 (permanent) or more";

		std::string reason;
		for (size_t i = 3; i < a.size(); ++i)
			reason += (i > 3 ? " " : "") + a[i];
		if (reason.size() > 255)
			reason.resize(255);

		Db& db = CmdDb();
		const std::string expires = hours == 0 ? "0" : "UNIX_TIMESTAMP() + " + std::to_string(hours * 3600);
		const long long n = db.Exec("INSERT INTO bans (eos_id, expires, reason, banned_at, banned_by) VALUES ('" + eos + "',"
			+ expires + ",'" + db.Esc(reason) + "',UNIX_TIMESTAMP(),'" + db.Esc(by) + "')"
			" ON DUPLICATE KEY UPDATE expires=VALUES(expires), reason=VALUES(reason), banned_at=VALUES(banned_at), banned_by=VALUES(banned_by)");
		if (n < 0)
			return "Database error: " + db.LastError();

		// Kick right away if they're on this server; other servers pick it up within a minute.
		if (AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(ToF(eos)))
		{
			std::string expires_text;
			if (hours > 0)
			{
				auto r = db.Query("SELECT DATE_FORMAT(FROM_UNIXTIME(expires), '%Y-%m-%d %H:%i') FROM bans WHERE eos_id='" + eos + "'");
				if (r && !r->empty())
					expires_text = (*r)[0][0].value_or("");
			}
			Kick(pc, BanMessage(hours == 0 ? 0 : 1, expires_text, reason));
		}

		Log::GetLog()->info("{} banned {} ({}): {}", by, eos, hours == 0 ? "permanent" : std::to_string(hours) + "h", reason);
		return hours == 0
			? "Banned " + eos + " permanently: " + reason
			: "Banned " + eos + " for " + std::to_string(hours) + " hours: " + reason;
	}

	std::string CmdUnban(const std::vector<std::string>& a, const std::string& by)
	{
		if (a.size() < 2)
			return "Usage: playerlogin.unban <eosID>";
		const std::string eos = Lower(a[1]);
		if (!IsEosId(eos))
			return "Invalid EOS ID: " + a[1];

		Db& db = CmdDb();
		const long long n = db.Exec("DELETE FROM bans WHERE eos_id='" + eos + "'");
		if (n < 0)
			return "Database error: " + db.LastError();
		Log::GetLog()->info("{} unbanned {}", by, eos);
		return n > 0 ? "Unbanned " + eos : "No ban found for " + eos;
	}

	std::string CmdLastSeen(const std::vector<std::string>& a)
	{
		if (a.size() < 2)
			return "Usage: playerlogin.LastSeen <eosID>";
		const std::string eos = Lower(a[1]);
		if (!IsEosId(eos))
			return "Invalid EOS ID: " + a[1];

		Db& db = CmdDb();
		auto r = db.Query("SELECT server_key, in_transit, DATE_FORMAT(FROM_UNIXTIME(last_seen), '%Y-%m-%d %H:%i')"
			" FROM characters WHERE eos_id='" + eos + "'");
		if (!r)
			return "Database error: " + db.LastError();
		if (r->empty())
			return "No record for " + eos;

		const DbRow& x = (*r)[0];
		std::string out = eos + " was last seen on " + x[0].value_or("?") + " at " + x[2].value_or("?") + " UTC";
		if (ToI64(x[1]) != 0)
			out += " (character uploaded, in transit)";
		return out;
	}

	std::string CmdReset(const std::vector<std::string>& a, const std::string& by)
	{
		if (a.size() < 2)
			return "Usage: playerlogin.reset <eosID>";
		const std::string eos = Lower(a[1]);
		if (!IsEosId(eos))
			return "Invalid EOS ID: " + a[1];

		Db& db = CmdDb();
		const long long n = db.Exec("DELETE FROM characters WHERE eos_id='" + eos + "'");
		if (n < 0)
			return "Database error: " + db.LastError();

		auto it = g_players.find(eos);
		if (it != g_players.end())
		{
			it->second.has_row = false;
			it->second.row_server.clear();
			it->second.row_in_transit = false;
		}
		Log::GetLog()->info("{} reset the character record for {}", by, eos);
		return n > 0 ? "Reset " + eos + ": next server they log into with a character becomes their record"
			: "No record for " + eos;
	}

	std::string Dispatch(const FString& body, const std::string& by)
	{
		const auto args = Split(body);
		if (args.empty())
			return {};
		const std::string cmd = Lower(args[0]);
		try
		{
			if (cmd == "playerlogin.ban") return CmdBan(args, by);
			if (cmd == "playerlogin.unban") return CmdUnban(args, by);
			if (cmd == "playerlogin.lastseen") return CmdLastSeen(args);
			if (cmd == "playerlogin.reset") return CmdReset(args, by);
		}
		catch (const std::exception& e)
		{
			return std::string("Error: ") + e.what();
		}
		return {};
	}

	const wchar_t* kCommands[] = { L"playerlogin.ban", L"playerlogin.unban", L"playerlogin.LastSeen", L"playerlogin.reset" };

	void OnRcon(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		FString reply = ToF(Dispatch(packet->Body, "rcon"));
		conn->SendMessageW(packet->Id, 0, &reply);
	}

	void OnConsole(APlayerController* player, FString* cmd, bool)
	{
		auto* pc = static_cast<AShooterPlayerController*>(player);
		if (!pc || !pc->bIsAdmin().Get())
			return;
		const std::string reply = Dispatch(*cmd, "admin:" + EosOf(pc));
		const FString msg = ToF(reply);
		AsaApi::GetApiUtils().SendServerMessage(pc, FLinearColor(0.4f, 1.0f, 0.4f, 1.0f), L"{}", *msg);
	}
}

// ---------------------------------------------------------------------------------------------
// Load / unload
// ---------------------------------------------------------------------------------------------

namespace
{
	const char* kHookCreate = "AShooterPlayerState.ServerRequestCreateNewPlayer_Implementation(FPrimalPlayerCharacterConfigStructReplicated)";
	const char* kHookLogin = "AShooterGameMode.HandleNewPlayer_Implementation(AShooterPlayerController*,UPrimalPlayerData*,AShooterCharacter*,bool)";
	const char* kHookLogout = "AShooterGameMode.Logout(AController*)";
	const char* kHookUpload = "AShooterPlayerController.ServerUploadCurrentCharacterAndItems_Implementation(UPrimalInventoryComponent*,int)";

	// Server identity and profile folder need the world, which isn't ready during Plugin_Init
	// on a cold start. This runs once the world exists.
	void InitWorldDependent()
	{
		if (!g_cfg.server_key_override.empty())
			g_server_key = g_cfg.server_key_override;
		else if (const std::string key = ServerKeyFromCommandLine(); !key.empty())
			g_server_key = key;
		else
			g_server_key = MapName();

		if (!g_cfg.verify.profile_dir.empty())
			g_profile_dir = fs::path(g_cfg.verify.profile_dir);
		else
			g_profile_dir = fs::path(AsaApi::Tools::GetCurrentDir()) / ".." / ".." / "Saved" / "SavedArks" / MapName();
		g_profile_dir = g_profile_dir.lexically_normal();

		Log::GetLog()->info("PlayerLogin: this server is '{}', profiles in {}", g_server_key, g_profile_dir.string());

		auto& timer = API::Timer::Get();
		timer.RecurringExecute(L"PlayerLogin_Drain", [] { if (g_alive) g_game_queue.Drain(); }, 1, -1, false);
		timer.RecurringExecute(L"PlayerLogin_Reconcile", [] { if (g_alive) Reconcile(); }, 5, -1, false);
		timer.RecurringExecute(L"PlayerLogin_Sweep", [] { if (g_alive) Sweep(); }, 60, -1, false);
		timer.DelayExecute(L"PlayerLogin_VerifyStart", []
		{
			if (!g_alive)
				return;
			Verify();
			API::Timer::Get().RecurringExecute(L"PlayerLogin_Verify", [] { if (g_alive) Verify(); },
				g_cfg.verify.interval_min * 60, -1, false);
		}, g_cfg.verify.startup_delay_min * 60);

		// Players already online after a hot reload get their records loaded now.
		ForEachOnlinePlayer([](AShooterPlayerController* pc)
		{
			const std::string eos = EosOf(pc);
			if (eos.empty() || g_players.count(eos))
				return;
			PlayerEntry entry;
			entry.had_data_at_login = pc->GetPlayerCharacter() != nullptr;
			g_players[eos] = entry;
			QueueRecordLoad(eos);
		});
	}

	void WaitForWorld()
	{
		if (AsaApi::GetApiUtils().GetStatus() == AsaApi::ServerStatus::Ready && AsaApi::GetApiUtils().GetWorld())
		{
			InitWorldDependent();
			return;
		}
		API::Timer::Get().DelayExecute(L"PlayerLogin_WaitWorld", [] { if (g_alive) WaitForWorld(); }, 5);
	}
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init("PlayerLogin");

	try
	{
		LoadConfig();
	}
	catch (const std::exception& e)
	{
		Log::GetLog()->critical("PlayerLogin: config error, plugin disabled: {}", e.what());
		return;
	}

	g_worker = std::make_unique<DbWorker>(g_cfg.db);
	g_worker->Start();
	g_worker->Post([](Db& db)
	{
		for (const char* sql : kSchema)
		{
			if (db.Exec(sql) < 0)
				Log::GetLog()->error("PlayerLogin: schema setup failed: {}", db.LastError());
		}
	});

	g_alive = true;

	auto& hooks = AsaApi::GetHooks();
	hooks.SetHook(kHookLogin, &Hook_AShooterGameMode_HandleNewPlayer_Implementation, &AShooterGameMode_HandleNewPlayer_Implementation_original);
	hooks.SetHook(kHookLogout, &Hook_AShooterGameMode_Logout, &AShooterGameMode_Logout_original);
	hooks.SetHook(kHookCreate, &Hook_AShooterPlayerState_ServerRequestCreateNewPlayer_Implementation, &AShooterPlayerState_ServerRequestCreateNewPlayer_Implementation_original);
	hooks.SetHook(kHookUpload, &Hook_AShooterPlayerController_ServerUploadCurrentCharacterAndItems_Implementation, &AShooterPlayerController_ServerUploadCurrentCharacterAndItems_Implementation_original);

	auto& commands = AsaApi::GetCommands();
	for (const wchar_t* c : kCommands)
	{
		commands.AddRconCommand(c, &OnRcon);
		commands.AddConsoleCommand(c, &OnConsole);
	}

	WaitForWorld();
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	if (!g_alive)
		return; // config failed at load: nothing was hooked or registered
	g_alive = false;

	auto& hooks = AsaApi::GetHooks();
	hooks.DisableHook(kHookLogin, &Hook_AShooterGameMode_HandleNewPlayer_Implementation);
	hooks.DisableHook(kHookLogout, &Hook_AShooterGameMode_Logout);
	hooks.DisableHook(kHookCreate, &Hook_AShooterPlayerState_ServerRequestCreateNewPlayer_Implementation);
	hooks.DisableHook(kHookUpload, &Hook_AShooterPlayerController_ServerUploadCurrentCharacterAndItems_Implementation);

	auto& commands = AsaApi::GetCommands();
	for (const wchar_t* c : kCommands)
	{
		commands.RemoveRconCommand(c);
		commands.RemoveConsoleCommand(c);
	}

	API::Timer::Get().UnloadAllTimers();

	// Flushes queued writes (logout timestamps etc.) before returning.
	if (g_worker)
	{
		g_worker->Stop();
		g_worker.reset();
	}
	g_cmd_db.reset();
	g_game_queue.Clear();
	g_players.clear();
}
