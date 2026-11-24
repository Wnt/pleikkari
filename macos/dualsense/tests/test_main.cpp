// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "test.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace t {

namespace {
int g_failures = 0;
const char *g_current = "";
} // namespace

std::vector<Case> &Registry()
{
	static std::vector<Case> cases;
	return cases;
}

void Fail(const char *file, int line, const std::string &what)
{
	g_failures++;
	fprintf(stderr, "  FAIL %s (%s:%d): %s\n", g_current, file, line, what.c_str());
}

std::vector<uint8_t> FromHex(const std::string &hex)
{
	std::vector<uint8_t> out;
	auto nibble = [](char c) -> int {
		if(c >= '0' && c <= '9') return c - '0';
		if(c >= 'a' && c <= 'f') return c - 'a' + 10;
		if(c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for(size_t i = 0; i + 1 < hex.size(); i += 2)
	{
		int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
		if(hi < 0 || lo < 0)
			break;
		out.push_back((uint8_t)(hi << 4 | lo));
	}
	return out;
}

std::string ToHex(const uint8_t *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	for(size_t i = 0; i < size; i++)
	{
		s.push_back(digits[data[i] >> 4]);
		s.push_back(digits[data[i] & 0xF]);
	}
	return s;
}

std::vector<FixtureLine> LoadFixture(const std::string &name)
{
	std::vector<FixtureLine> out;
	std::ifstream f(std::string(PLK_DS_FIXTURES) + "/" + name);
	if(!f)
	{
		Fail(__FILE__, __LINE__, "missing fixture " + name);
		return out;
	}
	std::string line;
	while(std::getline(f, line))
	{
		if(line.empty() || line[0] == '#')
			continue;
		std::istringstream ss(line);
		std::string a, b;
		ss >> a >> b;
		FixtureLine fl{};
		if(b.empty())
			fl.bytes = FromHex(a);
		else
		{
			fl.time_us = std::stoull(a);
			fl.bytes = FromHex(b);
		}
		out.push_back(std::move(fl));
	}
	return out;
}

} // namespace t

int main(int argc, char **argv)
{
	int run = 0;
	for(const t::Case &c : t::Registry())
	{
		if(argc > 1 && !strstr(c.name, argv[1]))
			continue;
		t::g_current = c.name;
		const int before = t::g_failures;
		c.fn();
		run++;
		printf("%s %s\n", t::g_failures == before ? "ok  " : "FAIL", c.name);
	}
	printf("%d tests, %d failed checks\n", run, t::g_failures);
	return t::g_failures ? 1 : 0;
}
