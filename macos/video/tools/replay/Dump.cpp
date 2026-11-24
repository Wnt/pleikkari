#include "Dump.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace plk::replay {

namespace {
constexpr char kMagic[8] = {'P', 'L', 'K', 'V', 'D', 'M', 'P', '1'};
constexpr size_t kHeader = 20;

uint64_t le(const uint8_t *p, int n)
{
	uint64_t v = 0;
	for(int i = n - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}
void putLe(uint8_t *p, uint64_t v, int n)
{
	for(int i = 0; i < n; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
struct FileCloser {
	void operator()(FILE *f) const { if(f) fclose(f); }
};
}  // namespace

bool readDump(const std::string &path, std::vector<DumpRecord> &out, std::string *error)
{
	std::unique_ptr<FILE, FileCloser> f(fopen(path.c_str(), "rb"));
	if(!f)
	{
		if(error) *error = "cannot open " + path;
		return false;
	}
	char magic[8];
	if(fread(magic, 1, 8, f.get()) != 8 || std::memcmp(magic, kMagic, 8) != 0)
	{
		if(error) *error = "not a PLKVDMP1 file: " + path;
		return false;
	}
	uint8_t h[kHeader];
	for(;;)
	{
		size_t n = fread(h, 1, kHeader, f.get());
		if(n == 0)
			break;
		if(n != kHeader)
		{
			if(error) *error = "truncated record header at record " + std::to_string(out.size());
			return !out.empty();  // keep what was complete (a dump cut off by a crash)
		}
		DumpRecord r;
		uint32_t size = (uint32_t)le(h, 4);
		r.frameIndex = (uint16_t)le(h + 4, 2);
		r.flags = (uint16_t)le(h + 6, 2);
		r.frameReadyUs = le(h + 8, 8);
		r.framesLost = (int32_t)(uint32_t)le(h + 16, 4);
		if(size > (64u << 20))
		{
			if(error) *error = "implausible payload size " + std::to_string(size);
			return false;
		}
		r.payload.resize(size);
		if(fread(r.payload.data(), 1, size, f.get()) != size)
		{
			if(error) *error = "truncated payload at record " + std::to_string(out.size());
			return !out.empty();
		}
		out.push_back(std::move(r));
	}
	return true;
}

bool writeDump(const std::string &path, const std::vector<DumpRecord> &records, std::string *error)
{
	std::unique_ptr<FILE, FileCloser> f(fopen(path.c_str(), "wb"));
	if(!f)
	{
		if(error) *error = "cannot create " + path;
		return false;
	}
	fwrite(kMagic, 1, 8, f.get());
	uint8_t h[kHeader];
	for(const auto &r : records)
	{
		putLe(h, r.payload.size(), 4);
		putLe(h + 4, r.frameIndex, 2);
		putLe(h + 6, r.flags, 2);
		putLe(h + 8, r.frameReadyUs, 8);
		putLe(h + 16, (uint32_t)r.framesLost, 4);
		if(fwrite(h, 1, kHeader, f.get()) != kHeader
			|| fwrite(r.payload.data(), 1, r.payload.size(), f.get()) != r.payload.size())
		{
			if(error) *error = "write failed: " + path;
			return false;
		}
	}
	return true;
}

}  // namespace plk::replay
