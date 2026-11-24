// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-host-tests [substring ...]: runs every case whose name contains one of the
// arguments (all cases without arguments). No hardware, no network: the session tests
// talk to 127.0.0.1 only, the audio tests drive the render callback themselves.

#include "TestHarness.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace plktest {

namespace {

int g_failures_in_case = 0;
bool g_skipped = false;
std::string g_skip_reason;

} // namespace

std::vector<Case> &cases()
{
	static std::vector<Case> c;
	return c;
}

void fail(const char *file, int line, const std::string &what)
{
	const char *base = strrchr(file, '/');
	fprintf(stdout, "    FAILED %s:%d: %s\n", base ? base + 1 : file, line, what.c_str());
	g_failures_in_case++;
}

void skip(const std::string &why)
{
	g_skipped = true;
	g_skip_reason = why;
}

void note(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("    note: ", stdout);
	vfprintf(stdout, fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

const std::string &tempDir()
{
	static std::string dir = [] {
		const char *env = getenv("PLK_TEST_TMPDIR");
		std::string d = env && *env ? env : "plk-host-tests-tmp";
		mkdir(d.c_str(), 0700);
		return d;
	}();
	return dir;
}

bool envFlag(const char *name)
{
	const char *v = getenv(name);
	return v && *v && strcmp(v, "0") != 0;
}

} // namespace plktest

int main(int argc, char **argv)
{
	int failed = 0, passed = 0, skipped = 0;
	for(const auto &c : plktest::cases())
	{
		if(argc > 1)
		{
			bool match = false;
			for(int i = 1; i < argc; i++)
				match = match || strstr(c.name, argv[i]);
			if(!match)
				continue;
		}
		plktest::g_failures_in_case = 0;
		plktest::g_skipped = false;
		fprintf(stdout, "[ RUN  ] %s\n", c.name);
		fflush(stdout);
		const auto t0 = std::chrono::steady_clock::now();
		try
		{
			c.fn();
		}
		catch(const std::exception &e)
		{
			plktest::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if(plktest::g_failures_in_case)
		{
			failed++;
			fprintf(stdout, "[ FAIL ] %s (%.0f ms)\n", c.name, ms);
		}
		else if(plktest::g_skipped)
		{
			skipped++;
			fprintf(stdout, "[ SKIP ] %s: %s\n", c.name, plktest::g_skip_reason.c_str());
		}
		else
		{
			passed++;
			fprintf(stdout, "[  OK  ] %s (%.0f ms)\n", c.name, ms);
		}
		fflush(stdout);
	}
	fprintf(stdout, "\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
	return failed ? 1 : 0;
}
