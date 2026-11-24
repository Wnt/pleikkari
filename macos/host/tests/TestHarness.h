// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// A minimal test harness: PLK_TEST(name) registers a case, CHECK records a failure and
// continues, REQUIRE records one and leaves the case, SKIP leaves it as skipped.

#ifndef PLK_HOST_TESTHARNESS_H
#define PLK_HOST_TESTHARNESS_H

#include <cstdio>
#include <string>
#include <vector>

namespace plktest {

struct Case
{
	const char *name;
	void (*fn)();
};

std::vector<Case> &cases();

struct Registrar
{
	Registrar(const char *name, void (*fn)()) { cases().push_back({ name, fn }); }
};

void fail(const char *file, int line, const std::string &what);
void skip(const std::string &why);
/** A measured value worth seeing in the test log. */
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/** A per-run scratch directory (PLK_TEST_TMPDIR, or ./plk-host-tests-tmp). */
const std::string &tempDir();
bool envFlag(const char *name);

} // namespace plktest

#define PLK_TEST(name) \
	static void name(); \
	static ::plktest::Registrar name##_registrar(#name, name); \
	static void name()

#define CHECK(cond) \
	do { \
		if(!(cond)) \
			::plktest::fail(__FILE__, __LINE__, #cond); \
	} while(0)

#define CHECK_MSG(cond, ...) \
	do { \
		if(!(cond)) \
		{ \
			char plk_msg_[512]; \
			snprintf(plk_msg_, sizeof(plk_msg_), __VA_ARGS__); \
			::plktest::fail(__FILE__, __LINE__, std::string(#cond) + " -- " + plk_msg_); \
		} \
	} while(0)

#define REQUIRE(cond) \
	do { \
		if(!(cond)) \
		{ \
			::plktest::fail(__FILE__, __LINE__, #cond); \
			return; \
		} \
	} while(0)

#define SKIP(why) \
	do { \
		::plktest::skip(why); \
		return; \
	} while(0)

#endif // PLK_HOST_TESTHARNESS_H
