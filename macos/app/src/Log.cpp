// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Log.h"

#include <pthread.h>
#include <pthread/qos.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace plk::app {

Log &Log::instance()
{
	// Never destroyed: threads that outlive main() (library teardown, Metal handlers) may
	// still log.
	static Log *log = new Log();
	return *log;
}

Log::~Log()
{
	close();
}

std::string Log::formatLine(char level, const char *message)
{
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	struct tm tm;
	time_t secs = tv.tv_sec;
	localtime_r(&secs, &tm);
	char stamp[64];
	snprintf(stamp, sizeof(stamp), "[%04d-%02d-%02d %02d:%02d:%02d:%06d] [%c] ", tm.tm_year + 1900, tm.tm_mon + 1,
			tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)tv.tv_usec, level);
	std::string line(stamp);
	line += message ? message : "";
	if(line.empty() || line.back() != '\n')
		line += '\n';
	return line;
}

bool Log::open(const std::string &path, std::string *error)
{
	close();
	FILE *f = fopen(path.c_str(), "a");
	if(!f)
	{
		if(error)
			*error = "cannot open " + path + ": " + strerror(errno);
		return false;
	}
	setvbuf(f, nullptr, _IOFBF, 1 << 16);
	std::lock_guard<std::mutex> lk(mutex_);
	file_ = f;
	path_ = path;
	stop_ = false;
	thread_ = std::thread([this] { run(); });
	return true;
}

void Log::setMirrorToStderr(bool mirror)
{
	std::lock_guard<std::mutex> lk(mutex_);
	mirror_ = mirror;
}

bool Log::isOpen() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	return file_ != nullptr;
}

void Log::close()
{
	std::thread t;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if(!thread_.joinable())
			return;
		stop_ = true;
		t = std::move(thread_);
	}
	cond_.notify_all();
	t.join();
	std::lock_guard<std::mutex> lk(mutex_);
	if(file_)
	{
		// Lines that arrived after the writer's last batch.
		for(const auto &l : queue_)
			fwrite(l.data(), 1, l.size(), file_);
		written_ += queue_.size();
		queue_.clear();
		fclose(file_);
		file_ = nullptr;
	}
	drained_.notify_all();
}

void Log::flush()
{
	std::unique_lock<std::mutex> lk(mutex_);
	if(!file_ || !thread_.joinable())
	{
		fflush(stderr);
		return;
	}
	const uint64_t target = queued_;
	cond_.notify_all();
	drained_.wait_for(lk, std::chrono::seconds(2), [&] { return written_ >= target || stop_; });
}

void Log::write(char level, const char *message)
{
	std::string line = formatLine(level, message);
	std::unique_lock<std::mutex> lk(mutex_);
	if(mirror_ || !file_)
	{
		fputs(line.c_str(), stderr);
		if(!file_)
			return;
	}
	queue_.push_back(std::move(line));
	queued_++;
	lk.unlock();
	cond_.notify_one();
}

void Log::vprintf(char level, const char *fmt, va_list ap)
{
	char buf[1024];
	va_list copy;
	va_copy(copy, ap);
	int n = vsnprintf(buf, sizeof(buf), fmt, copy);
	va_end(copy);
	if(n < 0)
		return;
	if((size_t)n < sizeof(buf))
	{
		write(level, buf);
		return;
	}
	std::string big((size_t)n + 1, '\0');
	vsnprintf(&big[0], big.size(), fmt, ap);
	big.resize((size_t)n);
	write(level, big.c_str());
}

void Log::printf(char level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(level, fmt, ap);
	va_end(ap);
}

void Log::run()
{
	pthread_setname_np("plk.log");
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
	std::vector<std::string> batch;
	std::unique_lock<std::mutex> lk(mutex_);
	for(;;)
	{
		cond_.wait(lk, [this] { return stop_ || !queue_.empty(); });
		batch.swap(queue_);
		FILE *f = file_;
		const bool stopping = stop_;
		lk.unlock();
		for(const auto &l : batch)
			fwrite(l.data(), 1, l.size(), f);
		fflush(f);
		const uint64_t n = batch.size();
		batch.clear();
		lk.lock();
		written_ += n;
		drained_.notify_all();
		if(stopping && queue_.empty())
			break;
	}
}

} // namespace plk::app
