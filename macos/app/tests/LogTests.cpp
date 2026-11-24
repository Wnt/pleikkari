// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Log.h"
#include "TestHarness.h"

#include <chiaki/log.h>

#include <fstream>
#include <regex>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace plk::app;

PLK_TEST(log_line_format_matches_the_qt_session_log)
{
	const std::string line = Log::formatLine('I', "Trigger effects packet, mask 0x2:");
	// triggers.py's HEAD pattern.
	static const std::regex head(
			R"(^\[\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d):(\d{3})\d*\] \[I\] Trigger effects packet, mask (0x[0-9a-f]+|0):)");
	CHECK_MSG(std::regex_search(line, head), "%s", line.c_str());
	CHECK(line.back() == '\n');
	CHECK(line.size() > 30 && line[27] == ']'); // six fractional digits
	CHECK(Log::formatLine('W', "x\n").find("] [W] x\n") != std::string::npos);
}

PLK_TEST(log_file_keeps_every_line_in_order)
{
	const std::string path = plktest::tempDir() + "/app-log.txt";
	unlink(path.c_str());
	Log log;
	REQUIRE(log.open(path));
	CHECK(log.isOpen());
	constexpr int kThreads = 4, kLines = 2000;
	std::vector<std::thread> threads;
	for(int t = 0; t < kThreads; t++)
		threads.emplace_back([&log, t] {
			for(int i = 0; i < kLines; i++)
				log.printf('I', "thread %d line %d", t, i);
		});
	for(auto &t : threads)
		t.join();
	log.flush();
	log.write('E', "last");
	log.close();
	CHECK(!log.isOpen());

	std::ifstream f(path);
	std::string l;
	int count = 0;
	int next[kThreads] = {};
	bool ordered = true;
	std::string last;
	static const std::regex re(R"(\] \[I\] thread (\d) line (\d+)$)");
	while(std::getline(f, l))
	{
		std::smatch m;
		if(std::regex_search(l, m, re))
		{
			const int t = std::stoi(m[1].str()), i = std::stoi(m[2].str());
			if(i != next[t])
				ordered = false;
			next[t] = i + 1;
			count++;
		}
		last = l;
	}
	CHECK(count == kThreads * kLines);
	CHECK(ordered);
	CHECK(last.find("] [E] last") != std::string::npos);
}

PLK_TEST(log_open_failure)
{
	Log log;
	std::string err;
	CHECK(!log.open("/nonexistent-dir/x.log", &err));
	CHECK(!err.empty());
	// Without a file, lines go to stderr and nothing blocks.
	log.write('I', "to stderr");
	log.flush();
	log.close();
}

PLK_TEST(log_trigger_messages_parse_like_triggers_py)
{
	// The lib's trigger effect message (streamconnection.c) through the app log, read back
	// with triggers.py's two patterns.
	const std::string path = plktest::tempDir() + "/app-trigger-log.txt";
	unlink(path.c_str());
	Log log;
	REQUIRE(log.open(path));
	ChiakiLog clog;
	chiaki_log_init(&clog, CHIAKI_LOG_ALL, [](ChiakiLogLevel level, const char *msg, void *user) {
		static_cast<Log *>(user)->write(chiaki_log_level_char(level), msg);
	}, &log);
	uint8_t msg[25];
	for(size_t i = 0; i < sizeof(msg); i++)
		msg[i] = (uint8_t)(0x10 + i);
	msg[3] = 0x02;
	chiaki_log(&clog, CHIAKI_LOG_INFO, "Trigger effects packet, mask %#x:", msg[3]);
	chiaki_log_hexdump(&clog, CHIAKI_LOG_INFO, msg, sizeof(msg));
	log.close();

	static const std::regex head(
			R"(^\[\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d):(\d{3})\d*\] \[I\] Trigger effects packet, mask (0x[0-9a-f]+|0):)");
	static const std::regex row(R"(\] \[I\]\s+([0-9a-f]+) ((?:[0-9a-f]{2} )+))");
	std::ifstream f(path);
	std::string l;
	bool saw_head = false;
	std::vector<uint8_t> bytes;
	while(std::getline(f, l))
	{
		if(std::regex_search(l, head))
		{
			saw_head = true;
			continue;
		}
		std::smatch m;
		if(saw_head && l.find("offset") == std::string::npos && std::regex_search(l, m, row))
		{
			std::string hex = m[2].str();
			for(size_t i = 0; i + 2 <= hex.size(); i += 3)
				bytes.push_back((uint8_t)std::stoi(hex.substr(i, 2), nullptr, 16));
		}
	}
	CHECK(saw_head);
	REQUIRE(bytes.size() == sizeof(msg));
	CHECK(bytes[3] == 0x02 && bytes[24] == msg[24]);
}
