// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The recording of a session that restarts as another stream: every kind of stream in a
// file of its own, each starting with its config sample, so that each replays by itself.

#include "TestHarness.h"
#include "VideoPath.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

using namespace plk;
using namespace plk::app;

namespace {

struct Record
{
	bool config = false;
	uint16_t index = 0;
	std::vector<uint8_t> payload;
};

// The records of a PLKVDMP1 file; `ok` is false when it is not one or ends inside a record.
std::vector<Record> readDump(const std::string &path, bool *ok)
{
	std::vector<Record> records;
	*ok = false;
	FILE *f = fopen(path.c_str(), "rb");
	if(!f)
		return records;
	char magic[8];
	bool good = fread(magic, 1, 8, f) == 8 && !memcmp(magic, kVideoDumpMagic, 8);
	while(good)
	{
		uint8_t header[kVideoDumpRecordHeaderSize];
		const size_t got = fread(header, 1, sizeof(header), f);
		if(got == 0)
			break;
		if(got != sizeof(header))
		{
			good = false;
			break;
		}
		Record r;
		const uint32_t size = (uint32_t)header[0] | ((uint32_t)header[1] << 8) | ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
		r.index = (uint16_t)(header[4] | (header[5] << 8));
		r.config = (header[6] & kVideoDumpFlagConfig) != 0;
		r.payload.resize(size);
		good = fread(r.payload.data(), 1, size, f) == size;
		if(good)
			records.push_back(std::move(r));
	}
	fclose(f);
	*ok = good;
	return records;
}

bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

} // namespace

PLK_TEST(video_path_dump_segment_names)
{
	CHECK(dumpSegmentPath("runs/video.plkv", 2, "sdr") == "runs/video-2-sdr.plkv");
	CHECK(dumpSegmentPath("/a.b/video.plkv", 3, "hdr") == "/a.b/video-3-hdr.plkv");
	CHECK(dumpSegmentPath("video.plkv", 2, "") == "video-2.plkv");
	// No extension: a dot in a directory's name or at the start of the file's is not one.
	CHECK(dumpSegmentPath("/a.b/video", 2, "sdr") == "/a.b/video-2-sdr");
	CHECK(dumpSegmentPath("runs/.video", 2, "sdr") == "runs/.video-2-sdr");
}

PLK_TEST(video_path_records_each_stream_to_its_own_file)
{
	char dir[] = "/tmp/plk-videopath-XXXXXX";
	CHECK(mkdtemp(dir) != nullptr);
	const std::string first = std::string(dir) + "/video.plkv";
	const std::string second = std::string(dir) + "/video-2-sdr.plkv";
	const std::string third = std::string(dir) + "/video-3-hdr.plkv";

	VideoPath path(nullptr);
	std::string error;
	CHECK(path.openDump(first, &error));
	path.setDumpLabel("hdr");
	uint8_t config[4] = {0, 0, 0, 1}, frame[6] = {0, 0, 0, 1, 2, 3};
	uint64_t now = 1000;
	auto sendConfig = [&](uint8_t tag) {
		config[3] = tag;
		path.onVideoSample(config, sizeof(config), 0, 0, 0, false);
	};
	auto sendFrame = [&](uint16_t index) { path.onVideoSample(frame, sizeof(frame), index, now += 16667, 0, false); };

	// The first stream. Its config sample again, before a frame or after some (a session
	// that reconnected as it was, a file with its parameter sets repeated), starts nothing new.
	sendConfig(1);
	sendConfig(1);
	sendFrame(0);
	sendFrame(1);
	sendConfig(1);
	sendFrame(2);
	CHECK(!exists(second));
	// H: the session restarts in SDR, and the new stream brings its own config sample.
	path.setDumpLabel("sdr");
	sendFrame(3);  // the old stream's last frame, still on its way
	sendConfig(2);
	sendFrame(0);
	sendFrame(1);
	// And back.
	path.setDumpLabel("hdr");
	sendConfig(3);
	sendFrame(0);
	path.closeDump();
	// Closed: nothing more is written, and nothing breaks.
	sendConfig(4);
	sendFrame(1);
	CHECK(path.dumpDropped() == 0);

	bool ok = false;
	std::vector<Record> records = readDump(first, &ok);
	CHECK(ok && records.size() == 7);
	CHECK(records.size() == 7 && records[0].config && records[0].payload[3] == 1 && records[1].config && !records[2].config
			&& records[4].config && records[6].index == 3);
	records = readDump(second, &ok);
	CHECK(ok && records.size() == 3);
	CHECK(records.size() == 3 && records[0].config && records[0].payload[3] == 2 && !records[1].config && records[2].index == 1
			&& records[2].payload.size() == sizeof(frame));
	records = readDump(third, &ok);
	CHECK(ok && records.size() == 2);
	CHECK(records.size() == 2 && records[0].config && records[0].payload[3] == 3 && !records[1].config);
	CHECK(!exists(std::string(dir) + "/video-4-hdr.plkv"));

	unlink(first.c_str());
	unlink(second.c_str());
	unlink(third.c_str());
	rmdir(dir);
}
