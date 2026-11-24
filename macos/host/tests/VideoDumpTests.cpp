// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "TestHarness.h"
#include "VideoDump.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <random>

namespace {

std::vector<uint8_t> readAll(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Rec
{
	std::vector<uint8_t> payload;
	uint16_t frame_index;
	uint64_t ready_us;
	int32_t lost;
	bool recovered;
};

std::vector<Rec> sampleRecords()
{
	return {
		{ { 0x00, 0x00, 0x00, 0x01, 0x40 }, 0, 0, 0, false },                    // config sample
		{ { 0xaa, 0xbb, 0xcc }, 0xbeef, 0x0102030405060708ull, 2, true },         // recovered, losses
		{ {}, 0xffff, 1, -1, false },                                            // empty payload, negative count
	};
}

} // namespace

PLK_TEST(video_dump_layout)
{
	const std::string path = plktest::tempDir() + "/layout.plkvdmp";
	plk::VideoDumpWriter w;
	std::string error;
	REQUIRE(w.open(path, &error));
	for(const Rec &r : sampleRecords())
		CHECK(w.write(r.payload.data(), r.payload.size(), r.frame_index, r.ready_us, r.lost, r.recovered));
	CHECK(w.records() == 3);
	CHECK(w.close());

	const std::vector<uint8_t> f = readAll(path);
	const uint8_t expect[] = {
		'P', 'L', 'K', 'V', 'D', 'M', 'P', '1',
		// record 1: size 5, index 0, flags config, ready 0, lost 0
		0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x01, 0x40,
		// record 2: size 3, index 0xbeef, flags recovered, ready 0x0102030405060708, lost 2
		0x03, 0x00, 0x00, 0x00, 0xef, 0xbe, 0x02, 0x00,
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
		0x02, 0x00, 0x00, 0x00,
		0xaa, 0xbb, 0xcc,
		// record 3: size 0, index 0xffff, no flags, ready 1, lost -1
		0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff,
	};
	CHECK(f.size() == sizeof(expect));
	CHECK(f.size() == sizeof(expect) && memcmp(f.data(), expect, sizeof(expect)) == 0);
}

PLK_TEST(video_dump_async_matches_sync)
{
	std::mt19937 rng(3);
	std::vector<Rec> recs = sampleRecords();
	for(int i = 0; i < 500; i++)
	{
		Rec r;
		r.payload.resize(rng() % 70000);
		for(auto &b : r.payload)
			b = (uint8_t)rng();
		r.frame_index = (uint16_t)i;
		r.ready_us = rng() % 3 ? 1000000ull + (uint64_t)i * 16667 : 0;
		r.lost = (int32_t)(rng() % 4);
		r.recovered = rng() % 5 == 0;
		recs.push_back(std::move(r));
	}
	const std::string sync_path = plktest::tempDir() + "/sync.plkvdmp";
	const std::string async_path = plktest::tempDir() + "/async.plkvdmp";
	{
		plk::VideoDumpWriter w;
		REQUIRE(w.open(sync_path));
		for(const Rec &r : recs)
			w.write(r.payload.data(), r.payload.size(), r.frame_index, r.ready_us, r.lost, r.recovered);
		CHECK(w.close());
	}
	{
		plk::AsyncVideoDumpWriter w;
		REQUIRE(w.open(async_path));
		for(const Rec &r : recs)
			CHECK(w.write(r.payload.data(), r.payload.size(), r.frame_index, r.ready_us, r.lost, r.recovered));
		CHECK(w.close());
		CHECK(w.dropped() == 0);
	}
	const std::vector<uint8_t> a = readAll(sync_path), b = readAll(async_path);
	size_t expect = 8;
	for(const Rec &r : recs)
		expect += plk::kVideoDumpRecordHeaderSize + r.payload.size();
	CHECK(a.size() == expect);
	CHECK(a == b);
}

PLK_TEST(video_dump_open_failure)
{
	plk::VideoDumpWriter w;
	std::string error;
	CHECK(!w.open(plktest::tempDir() + "/no-such-dir/x.plkvdmp", &error));
	CHECK(!error.empty());
	CHECK(!w.write(nullptr, 0, 0, 0, 0, false));
	plk::AsyncVideoDumpWriter a;
	CHECK(!a.open(plktest::tempDir() + "/no-such-dir/y.plkvdmp", &error));
	CHECK(!a.write(nullptr, 0, 0, 1, 0, false));
	CHECK(a.dropped() == 1);
}
