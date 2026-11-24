// Unit tests for the bitstream helpers and the dump format (no GPU, no decoder).
#include "AnnexB.h"
#include "Dump.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace plk::video::annexb;

static int failures = 0;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while(0)

static void push(std::vector<uint8_t> &v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b); }

int main()
{
	// HEVC: VPS, SPS, PPS, IDR slice (first slice), P slice x2, one with a
	// 3-byte start code, one with emulation prevention inside.
	std::vector<uint8_t> s;
	push(s, {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0x01});                    // VPS (32)
	push(s, {0, 0, 0, 1, 0x42, 0x01, 0x01, 0x02, 0x20, 0x00});        // SPS (33), profile_idc 2
	push(s, {0, 0, 0, 1, 0x44, 0x01, 0xc1});                          // PPS (34)
	push(s, {0, 0, 0, 1, 0x26, 0x01, 0xaf, 0x11, 0x22});              // IDR_W_RADL (19), first slice
	push(s, {0, 0, 1, 0x02, 0x01, 0xd0, 0x00, 0x00, 0x03, 0x01});     // TRAIL_R (1), first slice
	push(s, {0, 0, 0, 1, 0x02, 0x01, 0x80, 0x55});                    // TRAIL_R, first slice
	push(s, {0, 0, 0, 1, 0x02, 0x01, 0x00, 0x66, 0x00, 0x00});        // second slice of same picture, trailing zeros

	std::vector<Nal> nals;
	splitNals(s.data(), s.size(), nals);
	CHECK(nals.size() == 7);
	CHECK(nalType(Codec::HEVC, nals[0]) == 32);
	CHECK(nalType(Codec::HEVC, nals[3]) == 19);
	CHECK(isKeyframe(Codec::HEVC, 19));
	CHECK(nals[6].size == 4);  // trailing zero bytes trimmed
	CHECK(isFirstSliceOfPicture(Codec::HEVC, nals[3]));
	CHECK(!isFirstSliceOfPicture(Codec::HEVC, nals[6]));

	Codec c;
	CHECK(detectCodec(s.data(), s.size(), &c) && c == Codec::HEVC);

	auto aus = splitAccessUnits(Codec::HEVC, s.data(), s.size());
	CHECK(aus.size() == 3);
	if(aus.size() == 3)
	{
		CHECK(!aus[0].config.empty() && aus[0].keyframe);
		CHECK(aus[1].config.empty() && !aus[1].keyframe);
		std::vector<Nal> f2;
		splitNals(aus[2].frame.data(), aus[2].frame.size(), f2);
		CHECK(f2.size() == 2);  // both slices of the third picture
		ParameterSets ps;
		ps.codec = Codec::HEVC;
		std::vector<Nal> cn;
		splitNals(aus[0].config.data(), aus[0].config.size(), cn);
		for(auto &n : cn)
			ps.add(n, nalType(Codec::HEVC, n));
		CHECK(ps.complete());
		CHECK(ps.profileIdc() == 2);
		ParameterSets ps2 = ps;
		ps2.add(cn[1], 33);  // identical SPS is not duplicated
		CHECK(ps2 == ps);
	}

	// H.264 detection and first-slice logic.
	std::vector<uint8_t> h;
	push(h, {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28});  // SPS, profile 100
	push(h, {0, 0, 0, 1, 0x68, 0xee});              // PPS
	push(h, {0, 0, 0, 1, 0x65, 0x88, 0x84});        // IDR, first_mb 0
	push(h, {0, 0, 0, 1, 0x41, 0x9a, 0x02});        // P, first_mb 0
	CHECK(detectCodec(h.data(), h.size(), &c) && c == Codec::H264);
	auto ha = splitAccessUnits(Codec::H264, h.data(), h.size());
	CHECK(ha.size() == 2 && ha[0].keyframe && !ha[1].keyframe);

	// HDR SEI: prefix SEI with MDCV (137, 24 bytes) and CLL (144, 4 bytes); an
	// emulation prevention byte inside the MDCV payload must be removed.
	std::vector<uint8_t> sei = {0x4e, 0x01, 137, 24};
	uint8_t mdcv[24] = {0x33, 0xc2, 0x86, 0xc4, 0x1d, 0x4c, 0x0b, 0xb8, 0x84, 0xd0, 0x3e, 0x80,
		0x3d, 0x13, 0x40, 0x42, 0x00, 0x98, 0x96, 0x80, 0x00, 0x00, 0x00, 0x01};
	for(int i = 0; i < 24; i++)
	{
		sei.push_back(mdcv[i]);
		if(i == 21)
			sei.push_back(0x03);  // after 00 00 in the payload
	}
	push(sei, {144, 4, 0x03, 0xe8, 0x01, 0x90, 0x80});
	Nal seiNal{sei.data(), sei.size()};
	HdrSei hs;
	CHECK(parseHdrSei(Codec::HEVC, seiNal, hs));
	CHECK(hs.hasMasteringDisplay && hs.hasContentLightLevel);
	CHECK(hs.masteringDisplay[0] == 0x33 && hs.masteringDisplay[23] == 0x01 && hs.masteringDisplay[22] == 0x00);
	CHECK(hs.contentLightLevel[0] == 0x03 && hs.contentLightLevel[1] == 0xe8);

	// Dump round trip.
	std::vector<plk::replay::DumpRecord> recs(2);
	recs[0].flags = plk::replay::kDumpFlagConfig;
	recs[0].payload = {0, 0, 0, 1, 0x40, 0x01};
	recs[1].frameIndex = 65535;
	recs[1].flags = plk::replay::kDumpFlagRecovered;
	recs[1].frameReadyUs = 0x0102030405060708ull;
	recs[1].framesLost = 3;
	recs[1].payload = {0, 0, 1, 0x26, 0x01};
	// Written to the working directory (ctest runs in the build tree).
	char path[] = "plkvdump-test-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);
	std::string err;
	CHECK(plk::replay::writeDump(path, recs, &err));
	std::vector<plk::replay::DumpRecord> back;
	CHECK(plk::replay::readDump(path, back, &err));
	unlink(path);
	CHECK(back.size() == 2);
	if(back.size() == 2)
	{
		CHECK(back[0].isConfig() && back[0].payload == recs[0].payload);
		CHECK(back[1].frameIndex == 65535 && back[1].recovered() && back[1].frameReadyUs == recs[1].frameReadyUs);
		CHECK(back[1].framesLost == 3 && back[1].payload == recs[1].payload);
	}

	if(failures)
	{
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
