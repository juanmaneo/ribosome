#ifndef __RIBOSOME_EXPIRATION_HPP
#define __RIBOSOME_EXPIRATION_HPP

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glog/logging.h>

namespace ioremap { namespace ribosome {

static inline const char *print_time(const std::chrono::system_clock::time_point &tp, char *dst, int dsize)
{
	char str[64];
	struct tm tm;

	auto sec = std::chrono::time_point_cast<std::chrono::seconds>(tp);
	time_t tsec = std::chrono::system_clock::to_time_t(sec);
	long tusec = std::chrono::duration_cast<std::chrono::microseconds>(tp - sec).count();

	localtime_r(&tsec, &tm);
	strftime(str, sizeof(str), "%F %R:%S", &tm);

	snprintf(dst, dsize, "%s.%06lu", str, tusec);
	return dst;
}

class expiration {
public:
	typedef std::function<void ()> callback_t;
	typedef uint64_t token_t;

	expiration() : m_thread(std::bind(&expiration::run, this)) {
	}

	~expiration() {
		stop();
		m_thread.join();
	}

	void stop() {
		m_need_exit = true;
		m_wait.notify_one();

		std::unique_lock<std::mutex> guard(m_lock);
		m_wait.wait(guard, [&] () {return m_completed == true;});

		m_timeouts.clear();
		m_tok2time.clear();
	}

	token_t insert(const std::chrono::system_clock::time_point &expires_at, callback_t callback) {
		std::unique_lock<std::mutex> guard(m_lock);

		control_t ctl;
		ctl.token = ++m_seq;
		ctl.callback = callback;

		auto it = m_timeouts.find(expires_at);
		if (it == m_timeouts.end()) {
			m_timeouts.insert(std::make_pair(expires_at, std::vector<control_t>({ctl})));

			if (VLOG_IS_ON(2)) {
				char buf[128];
				VLOG(2) << "ribosome::expiration::insert" <<
					": expires_at: " << print_time(expires_at, buf, sizeof(buf)) <<
					", token: " << ctl.token <<
					", callback: " << ctl.callback.target_type().name() <<
					", pushed new timeouts vector";
			}
		} else {
			it->second.push_back(ctl);

			if (VLOG_IS_ON(2)) {
				char buf[128];
				VLOG(2) << "ribosome::expiration::insert" <<
					": expires_at: " << print_time(expires_at, buf, sizeof(buf)) <<
					", token: " << ctl.token <<
					", callback: " << ctl.callback.target_type().name() <<
					", timeouts vector size: " << it->second.size();
			}
		}

		m_tok2time[ctl.token] = expires_at;

		guard.unlock();
		m_wait.notify_one();

		return ctl.token;
	}

	callback_t remove(const token_t token) {
		std::unique_lock<std::mutex> guard(m_lock);

		auto etime = m_tok2time.find(token);
		if (etime == m_tok2time.end()) {
			VLOG(2) << "ribosome::expiration::remove" <<
				": token: " << token <<
				", there is no time in tok2time map";
			return callback_t();
		}

		auto expires_at = etime->second;
		m_tok2time.erase(etime);

		auto ctl = m_timeouts.find(expires_at);
		if (ctl == m_timeouts.end()) {
			if (VLOG_IS_ON(2)) {
				char buf[128];
				VLOG(2) << "ribosome::expiration::remove" <<
					": expires_at: " << print_time(expires_at, buf, sizeof(buf)) <<
					", token: " << token <<
					", there are no callbacks in timeouts map";
			}
			return callback_t();
		}

		for (auto it = ctl->second.begin(), end = ctl->second.end(); it != end; ++it) {
			if (it->token == token) {
				auto callback = it->callback;
				ctl->second.erase(it);

				if (VLOG_IS_ON(2)) {
					char buf[128];
					VLOG(2) << "ribosome::expiration::remove" <<
						": expires_at: " << print_time(expires_at, buf, sizeof(buf)) <<
						", token: " << token <<
						", callback: " << callback.operator bool() <<
						", timeouts vector size: " << ctl->second.size();
				}

				if (ctl->second.empty()) {
					m_timeouts.erase(ctl);
				}

				return callback;
			}
		}

		return callback_t();
	}

private:
	token_t m_seq = 0;
	bool m_need_exit = false;
	bool m_completed = false;
	std::condition_variable m_wait;
	std::mutex m_lock;

	typedef struct {
		token_t	token;
		callback_t	callback;
	} control_t;

	expiration(const expiration &other) = delete;

	std::map<std::chrono::system_clock::time_point, std::vector<control_t>> m_timeouts;
	std::unordered_map<token_t, std::chrono::system_clock::time_point> m_tok2time;

	std::thread m_thread;

	void run() {
		while (!m_need_exit) {
			std::unique_lock<std::mutex> guard(m_lock);

			std::chrono::system_clock::time_point next_check = std::chrono::system_clock::now() + std::chrono::seconds(1);
			if (m_timeouts.size()) {
				next_check = m_timeouts.begin()->first;
			}

			m_wait.wait_until(guard, next_check);

			std::vector<control_t> expired;

			// mutex is locked
			while (m_timeouts.size() != 0) {
				auto f = m_timeouts.begin();
				auto timestamp = f->first;
				if (std::chrono::system_clock::now() < timestamp)
					break;

				for (auto &c: f->second) {
					expired.emplace_back(c);
				}

				m_timeouts.erase(f);

				for (auto &ctl: expired) {
					m_tok2time.erase(ctl.token);
				}
			}

			guard.unlock();

			for (auto &ctl: expired) {
				ctl.callback();
			}
		}

		std::unique_lock<std::mutex> guard(m_lock);
		m_completed = true;
		m_wait.notify_all();
	}
};

}} // namespace ioremap::ribosome

#endif // __RIBOSOME_EXPIRATION_HPP
