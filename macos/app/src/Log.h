// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The app's log: one file, the Qt app's session log line format, written off the hot path.
//
//   [2026-11-24 22:39:12:123456] [I] message
//
// The date stamp is local time with microseconds (the Qt log prints "zzzzzz"; the analysis
// scripts read the first three digits as milliseconds). The level letter is the lib's
// (chiaki_log_level_char): D, V, I, W, E.
//
// Any thread may log. A line is formatted on the caller's thread and handed to one writer
// thread (QoS utility) through a short mutex; the caller never waits for the disk. Without
// a file the lines go to stderr, synchronously.

#ifndef PLK_APP_LOG_H
#define PLK_APP_LOG_H

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plk::app {

class Log
{
public:
	static Log &instance();

	Log() = default;
	~Log();
	Log(const Log &) = delete;
	Log &operator=(const Log &) = delete;

	/** Append to `path` (created if needed). Lines logged before this went to stderr. */
	bool open(const std::string &path, std::string *error = nullptr);
	/** Also copy every line to stderr (the default when no file is open). */
	void setMirrorToStderr(bool mirror);
	/** Write everything queued and close the file; later lines go to stderr. */
	void close();
	/** Wait until everything logged so far is in the file. */
	void flush();

	void write(char level, const char *message);
	void printf(char level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	void vprintf(char level, const char *fmt, va_list ap);

	bool isOpen() const;
	const std::string &path() const { return path_; }

	/** "[2026-11-24 22:39:12:123456] [I] message\n" for the current time. */
	static std::string formatLine(char level, const char *message);

private:
	void run();

	mutable std::mutex mutex_;
	std::condition_variable cond_;
	std::condition_variable drained_;
	std::vector<std::string> queue_;
	uint64_t queued_ = 0;
	uint64_t written_ = 0;
	FILE *file_ = nullptr;
	std::string path_;
	bool mirror_ = false;
	bool stop_ = false;
	std::thread thread_;
};

} // namespace plk::app

#define PLK_LOGI(...) ::plk::app::Log::instance().printf('I', __VA_ARGS__)
#define PLK_LOGW(...) ::plk::app::Log::instance().printf('W', __VA_ARGS__)
#define PLK_LOGE(...) ::plk::app::Log::instance().printf('E', __VA_ARGS__)
#define PLK_LOGD(...) ::plk::app::Log::instance().printf('D', __VA_ARGS__)

#endif // PLK_APP_LOG_H
