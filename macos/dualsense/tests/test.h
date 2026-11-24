// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// A minimal test harness: TEST(name) registers a function, CHECK* records failures
// and keeps going. No dependencies.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace t {

struct Case {
	const char *name;
	void (*fn)();
};

std::vector<Case> &Registry();
void Fail(const char *file, int line, const std::string &what);

struct Register {
	Register(const char *name, void (*fn)()) { Registry().push_back({ name, fn }); }
};

template <class T> std::string Show(const T &v)
{
	if constexpr(std::is_enum_v<T>)
		return std::to_string((long long)v);
	else if constexpr(std::is_integral_v<T>)
		return std::to_string((long long)v) + " (0x" + [&] {
			char b[32];
			snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
			return std::string(b);
		}() + ")";
	else if constexpr(std::is_floating_point_v<T>)
		return std::to_string(v);
	else if constexpr(std::is_convertible_v<T, std::string>)
		return std::string(v);
	else
		return "?";
}

std::vector<uint8_t> FromHex(const std::string &hex);
std::string ToHex(const uint8_t *data, size_t size);
inline std::string ToHex(const std::vector<uint8_t> &v) { return ToHex(v.data(), v.size()); }

// Fixture files: one report per line, "<time_us> <hex>" or "<hex>"; '#' comments.
struct FixtureLine {
	uint64_t time_us;
	std::vector<uint8_t> bytes;
};
std::vector<FixtureLine> LoadFixture(const std::string &name);

} // namespace t

#define TEST(name) \
	static void name(); \
	static t::Register name##_register(#name, name); \
	static void name()

#define CHECK(cond) \
	do { \
		if(!(cond)) \
			t::Fail(__FILE__, __LINE__, #cond); \
	} while(0)

#define CHECK_EQ(a, b) \
	do { \
		const auto va_ = (a); \
		const auto vb_ = (b); \
		if(!(va_ == vb_)) \
			t::Fail(__FILE__, __LINE__, std::string(#a " == " #b ": ") + t::Show(va_) + " vs " + t::Show(vb_)); \
	} while(0)

#define CHECK_NEAR(a, b, eps) \
	do { \
		const double va_ = (a); \
		const double vb_ = (b); \
		if(!(va_ - vb_ <= (eps) && vb_ - va_ <= (eps))) \
			t::Fail(__FILE__, __LINE__, std::string(#a " ~= " #b ": ") + std::to_string(va_) + " vs " + std::to_string(vb_)); \
	} while(0)
