// PLKVDMP1 video dump format (written by the instrumented Qt client, read by
// plk-video-replay). All little-endian:
//   file:   8 bytes magic "PLKVDMP1"
//   record: uint32 payload_size
//           uint16 frame_index      Takion frame serial, wraps at 65536
//           uint16 flags            bit0 = config sample, bit1 = recovered after loss
//           uint64 frame_ready_us   receiver completion time (CLOCK_MONOTONIC); 0 for config
//           int32  frames_lost      frames lost before this one
//           uint8  payload[payload_size]   Annex-B
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plk::replay {

struct DumpRecord {
	uint16_t frameIndex = 0;
	uint16_t flags = 0;
	uint64_t frameReadyUs = 0;
	int32_t framesLost = 0;
	std::vector<uint8_t> payload;

	bool isConfig() const { return flags & 1; }
	bool recovered() const { return flags & 2; }
};

constexpr uint16_t kDumpFlagConfig = 1;
constexpr uint16_t kDumpFlagRecovered = 2;

bool readDump(const std::string &path, std::vector<DumpRecord> &out, std::string *error);
bool writeDump(const std::string &path, const std::vector<DumpRecord> &records, std::string *error);

}  // namespace plk::replay
