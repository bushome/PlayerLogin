#pragma once
// Discord webhook logging. Events are queued per channel from any thread; a game-thread timer
// flushes each channel at most once every 2 s, combining queued events into one message.
// The HTTP POST itself runs on AsaApi's request threads, so nothing here ever waits on Discord.

#include <Logger/Logger.h>
#include <Requests.h>
#include <json.hpp>

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace PlayerLogin
{
	enum class Channel { Logins, Transfers, Duplicates, Moderation };

	class Discord
	{
	public:
		void SetUrl(Channel c, std::string url)
		{
			std::lock_guard lock(mutex_);
			queues_[c].url = std::move(url);
		}

		bool Enabled(Channel c)
		{
			std::lock_guard lock(mutex_);
			auto it = queues_.find(c);
			return it != queues_.end() && !it->second.url.empty();
		}

		// Thread-safe. `block` is one event, already formatted as lines.
		void Post(Channel c, std::string block)
		{
			std::lock_guard lock(mutex_);
			auto& q = queues_[c];
			if (q.url.empty())
				return;
			if (q.pending.size() >= kMaxQueued)
			{
				++q.dropped;
				return;
			}
			q.pending.push_back(std::move(block));
		}

		// Game thread, every 2 s: one POST per channel with something queued.
		void Flush()
		{
			std::vector<std::pair<std::string, std::string>> sends; // url, payload
			{
				std::lock_guard lock(mutex_);
				for (auto& [channel, q] : queues_)
				{
					if (q.url.empty() || (q.pending.empty() && q.dropped == 0))
						continue;

					std::string body;
					while (!q.pending.empty())
					{
						const std::string& next = q.pending.front();
						const std::string piece = "```\n" + next + "\n```";
						if (body.size() + piece.size() + 1 > kMaxContent)
						{
							if (body.empty())
							{
								// Single oversized event: truncate rather than drop.
								body = "```\n" + next.substr(0, kMaxContent - 12) + "\n```";
								q.pending.pop_front();
							}
							break;
						}
						body += (body.empty() ? "" : "\n") + piece;
						q.pending.pop_front();
					}
					if (q.dropped > 0 && body.size() + 64 < kMaxContent)
					{
						body += "\n(" + std::to_string(q.dropped) + " events dropped: queue full)";
						q.dropped = 0;
					}
					if (body.empty())
						continue;

					nlohmann::json payload;
					payload["content"] = body;
					payload["allowed_mentions"] = { { "parse", nlohmann::json::array() } }; // never ping anyone
					sends.emplace_back(q.url, payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
				}
			}

			for (auto& [url, data] : sends)
			{
				API::Requests::Get().CreatePostRequest(url,
					[](bool ok, std::string result, std::unordered_map<std::string, std::string>)
					{
						if (!ok)
							Log::GetLog()->warn("Discord webhook post failed: {}", result.substr(0, 200));
					},
					data, "application/json");
			}
		}

		void Clear()
		{
			std::lock_guard lock(mutex_);
			queues_.clear();
		}

	private:
		static constexpr size_t kMaxQueued = 200;   // per channel; beyond this, count and drop
		static constexpr size_t kMaxContent = 1900; // Discord limit is 2000 characters

		struct Queue
		{
			std::string url;
			std::deque<std::string> pending;
			size_t dropped = 0;
		};

		std::mutex mutex_;
		std::unordered_map<Channel, Queue> queues_;
	};
}
