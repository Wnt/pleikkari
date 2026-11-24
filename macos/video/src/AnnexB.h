// Annex-B bitstream helpers: NAL splitting, parameter-set collection,
// access-unit splitting (for raw elementary streams) and HDR10 SEI parsing.
// Pure C++, no Apple frameworks, unit-tested by plk-video-tests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace plk::video::annexb {

enum class Codec { H264, HEVC };

struct Nal {
	const uint8_t *data = nullptr;  // first byte of the NAL header (after the start code)
	size_t size = 0;                // without start code and trailing zero bytes
};

// Appends every NAL unit of an Annex-B buffer (3- or 4-byte start codes) to
// `out`. Bytes before the first start code are ignored.
void splitNals(const uint8_t *buf, size_t size, std::vector<Nal> &out);

int nalType(Codec codec, const Nal &nal);
bool isParameterSet(Codec codec, int type);  // VPS/SPS/PPS or SPS/PPS
bool isVcl(Codec codec, int type);
bool isKeyframe(Codec codec, int type);      // HEVC IRAP (16..23), H.264 IDR (5)
bool isAud(Codec codec, int type);
bool isSei(Codec codec, int type);           // HEVC prefix SEI (39), H.264 SEI (6)
// True for a VCL NAL that starts a new picture (first_slice_segment_in_pic_flag
// for HEVC, first_mb_in_slice == 0 for H.264).
bool isFirstSliceOfPicture(Codec codec, const Nal &nal);

// Guesses the codec from the first parameter set in the buffer. Returns false
// if no parameter set is recognisable.
bool detectCodec(const uint8_t *buf, size_t size, Codec *out);

struct ParameterSets {
	Codec codec = Codec::HEVC;
	// Each entry is a NAL unit without start code, in VPS, SPS, PPS order.
	std::vector<std::vector<uint8_t>> vps, sps, pps;

	bool complete() const;  // has everything the format description needs
	bool operator==(const ParameterSets &o) const {
		return codec == o.codec && vps == o.vps && sps == o.sps && pps == o.pps;
	}
	bool operator!=(const ParameterSets &o) const { return !(*this == o); }
	// Adds a parameter set NAL; a later set with the same type replaces an
	// identical earlier one rather than duplicating it.
	void add(const Nal &nal, int type);
	// Profile byte from the SPS (HEVC general_profile_idc, H.264 profile_idc), or -1.
	int profileIdc() const;
};

// Static HDR metadata found in SEI messages, as the big-endian payloads that
// CAEDRMetadata and the CoreVideo attachments expect.
struct HdrSei {
	bool hasMasteringDisplay = false;
	std::array<uint8_t, 24> masteringDisplay{};  // D.2.28 mastering_display_colour_volume
	bool hasContentLightLevel = false;
	std::array<uint8_t, 4> contentLightLevel{};  // D.2.35 content_light_level_info
	bool operator==(const HdrSei &o) const {
		return hasMasteringDisplay == o.hasMasteringDisplay && masteringDisplay == o.masteringDisplay
			&& hasContentLightLevel == o.hasContentLightLevel && contentLightLevel == o.contentLightLevel;
	}
	bool operator!=(const HdrSei &o) const { return !(*this == o); }
};
// Parses SEI payload types 137 and 144 from one SEI NAL; returns true if
// either was found (fields not present are left untouched).
bool parseHdrSei(Codec codec, const Nal &nal, HdrSei &inout);

// Removes emulation prevention bytes (00 00 03 -> 00 00).
std::vector<uint8_t> unescapeRbsp(const uint8_t *data, size_t size);

// One access unit of a raw elementary stream, re-emitted the way the chiaki
// receiver hands them over: parameter sets as a separate config sample, the
// remaining NALs (SEI, slices; no AUD) as the frame. Both are Annex-B with
// 4-byte start codes; `config` is empty if the AU carried no parameter sets.
struct AccessUnit {
	std::vector<uint8_t> config;
	std::vector<uint8_t> frame;
	bool keyframe = false;
};
std::vector<AccessUnit> splitAccessUnits(Codec codec, const uint8_t *buf, size_t size);

void appendAnnexB(std::vector<uint8_t> &out, const Nal &nal);

}  // namespace plk::video::annexb
