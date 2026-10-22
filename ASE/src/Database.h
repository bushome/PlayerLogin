#pragma once
// Thin MariaDB Connector/C wrapper plus a single background worker thread.
// Every query in the plugin runs on the worker; hooks only queue jobs.

#include <mysql/mysql.h> // vcpkg libmariadb installs headers under include/mysql

#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace PlayerLogin
{
	struct DbConfig
	{
		std::string host = "127.0.0.1";
		unsigned int port = 3306;
		std::string user;
		std::string password;
		std::string database;
		bool ssl = false; // false: plain connection (local / LAN MariaDB without TLS)
	};

	using DbRow = std::vector<std::optional<std::string>>;

	class Db
	{
	public:
		explicit Db(DbConfig cfg) : cfg_(std::move(cfg)) {}
		~Db() { Close(); }

		Db(const Db&) = delete;
		Db& operator=(const Db&) = delete;

		const std::string& LastError() const { return last_error_; }

		bool Connect()
		{
			Close();
			conn_ = mysql_init(nullptr);
			if (!conn_)
			{
				last_error_ = "mysql_init failed";
				return false;
			}

			unsigned int timeout = 5;
			mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
			mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &timeout);
			mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
			mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

			// Connector/C 3.4+ requires TLS and verifies the server certificate by default, which fails
			// against a MariaDB server without TLS ("SSL is required, but the server does not support it").
			// Ssl=false: never use TLS. Ssl=true: require TLS but accept a self-signed server certificate.
			my_bool enforce = cfg_.ssl ? 1 : 0;
			my_bool verify = 0;
			mysql_optionsv(conn_, MYSQL_OPT_SSL_ENFORCE, &enforce);
			mysql_optionsv(conn_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);

			if (!mysql_real_connect(conn_, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(),
				cfg_.database.c_str(), cfg_.port, nullptr, 0))
			{
				last_error_ = mysql_error(conn_);
				Close();
				return false;
			}
			// All formatted timestamps are UTC, on every (re)connect.
			const char* tz = "SET time_zone='+00:00'";
			mysql_real_query(conn_, tz, static_cast<unsigned long>(strlen(tz)));
			return true;
		}

		void Close()
		{
			if (conn_)
			{
				mysql_close(conn_);
				conn_ = nullptr;
			}
		}

		std::string Esc(const std::string& in)
		{
			if (!conn_ && !Connect())
				return {};
			std::string out(in.size() * 2 + 1, '\0');
			const unsigned long n = mysql_real_escape_string(conn_, out.data(), in.c_str(),
				static_cast<unsigned long>(in.size()));
			out.resize(n);
			return out;
		}

		// Statement without a result set. Returns affected rows, or -1 on error.
		long long Exec(const std::string& sql)
		{
			if (!Run(sql))
				return -1;
			// Drain any unexpected result set.
			if (MYSQL_RES* res = mysql_store_result(conn_))
				mysql_free_result(res);
			return static_cast<long long>(mysql_affected_rows(conn_));
		}

		// Statement with a result set. Returns nullopt on error.
		std::optional<std::vector<DbRow>> Query(const std::string& sql)
		{
			if (!Run(sql))
				return std::nullopt;

			MYSQL_RES* res = mysql_store_result(conn_);
			std::vector<DbRow> rows;
			if (!res)
			{
				if (mysql_field_count(conn_) == 0)
					return rows;
				last_error_ = mysql_error(conn_);
				return std::nullopt;
			}

			const unsigned int cols = mysql_num_fields(res);
			while (MYSQL_ROW r = mysql_fetch_row(res))
			{
				unsigned long* lens = mysql_fetch_lengths(res);
				DbRow row;
				row.reserve(cols);
				for (unsigned int i = 0; i < cols; ++i)
				{
					if (r[i])
						row.emplace_back(std::string(r[i], lens[i]));
					else
						row.emplace_back(std::nullopt);
				}
				rows.push_back(std::move(row));
			}
			mysql_free_result(res);
			return rows;
		}

	private:
		// Runs a statement, reconnecting once if the server went away.
		bool Run(const std::string& sql)
		{
			for (int attempt = 0; attempt < 2; ++attempt)
			{
				if (!conn_ && !Connect())
					return false;

				if (mysql_real_query(conn_, sql.c_str(), static_cast<unsigned long>(sql.size())) == 0)
					return true;

				const unsigned int err = mysql_errno(conn_);
				last_error_ = mysql_error(conn_);
				// 2006 = server gone away, 2013 = lost connection, 4031 = idle timeout disconnect
				if (err == 2006 || err == 2013 || err == 4031)
				{
					Close();
					continue;
				}
				return false;
			}
			return false;
		}

		DbConfig cfg_;
		MYSQL* conn_ = nullptr;
		std::string last_error_;
	};

	// Single background thread that owns the connection and runs jobs in order.
	// Ordering matters: a logout write queued after a login write must land after it.
	class DbWorker
	{
	public:
		using Job = std::function<void(Db&)>;

		explicit DbWorker(DbConfig cfg) : db_(std::move(cfg)) {}
		~DbWorker() { Stop(); }

		void Start()
		{
			stop_ = false;
			thread_ = std::thread([this] { Loop(); });
		}

		void Stop()
		{
			{
				std::lock_guard lock(mutex_);
				if (stop_)
					return;
				stop_ = true;
			}
			cv_.notify_all();
			if (thread_.joinable())
				thread_.join();
			db_.Close();
		}

		void Post(Job job)
		{
			{
				std::lock_guard lock(mutex_);
				if (stop_)
					return;
				jobs_.push_back(std::move(job));
			}
			cv_.notify_one();
		}

	private:
		void Loop()
		{
			// mysql_thread_init/end are required for Connector/C use on a non-main thread.
			mysql_thread_init();
			for (;;)
			{
				Job job;
				{
					std::unique_lock lock(mutex_);
					cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
					if (jobs_.empty())
						break; // stop requested and queue drained
					job = std::move(jobs_.front());
					jobs_.pop_front();
				}
				try
				{
					job(db_);
				}
				catch (...)
				{
					// A failing job must never take the worker down.
				}
			}
			db_.Close();
			mysql_thread_end();
		}

		Db db_;
		std::thread thread_;
		std::mutex mutex_;
		std::condition_variable cv_;
		std::deque<Job> jobs_;
		bool stop_ = true;
	};

	// Results going back to the game thread. Drained by a game-thread timer.
	class GameQueue
	{
	public:
		void Post(std::function<void()> fn)
		{
			std::lock_guard lock(mutex_);
			items_.push_back(std::move(fn));
		}

		void Drain()
		{
			std::vector<std::function<void()>> local;
			{
				std::lock_guard lock(mutex_);
				local.swap(items_);
			}
			for (auto& fn : local)
			{
				try { fn(); }
				catch (...) {}
			}
		}

		void Clear()
		{
			std::lock_guard lock(mutex_);
			items_.clear();
		}

	private:
		std::mutex mutex_;
		std::vector<std::function<void()>> items_;
	};
}
