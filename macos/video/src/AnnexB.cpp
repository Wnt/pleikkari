#include "AnnexB.h"

#include <cstring>

namespace plk::video::annexb {

void splitNals(const uint8_t *buf, size_t size, std::vector<Nal> &out)
{
	// Find start codes 00 00 01; a preceding 00 belongs to the 4-byte form and
	// is trimmed from the previous NAL as a trailing zero.
	size_t i = 0;
	size_t nalStart = SIZE_MAX;
	while(i + 3 <= size)
	{
		// Fast skip: a start code needs buf[i+2] in {0, 1}.
		if(buf[i + 2] > 1)
		{
			i += 3;
			continue;
		}
		if(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
		{
			if(nalStart != SIZE_MAX)
			{
				size_t end = i;
				while(end > nalStart && buf[end - 1] == 0)
					end--;
				if(end > nalStart)
					out.push_back({buf + nalStart, end - nalStart});
			}
			i += 3;
			nalStart = i;
			continue;
		}
		i++;
	}
	if(nalStart != SIZE_MAX && nalStart < size)
	{
		size_t end = size;
		while(end > nalStart && buf[end - 1] == 0)
			end--;
		if(end > nalStart)
			out.push_back({buf + nalStart, end - nalStart});
	}
}

int nalType(Codec codec, const Nal &nal)
{
	if(nal.size < 1)
		return -1;
	if(codec == Codec::HEVC)
		return (nal.data[0] >> 1) & 0x3f;
	return nal.data[0] & 0x1f;
}

bool isParameterSet(Codec codec, int type)
{
	if(codec == Codec::HEVC)
		return type == 32 || type == 33 || type == 34;
	return type == 7 || type == 8;
}

bool isVcl(Codec codec, int type)
{
	if(codec == Codec::HEVC)
		return type >= 0 && type <= 31;
	return type >= 1 && type <= 5;
}

bool isKeyframe(Codec codec, int type)
{
	if(codec == Codec::HEVC)
		return type >= 16 && type <= 23;
	return type == 5;
}

bool isAud(Codec codec, int type)
{
	return codec == Codec::HEVC ? type == 35 : type == 9;
}

bool isSei(Codec codec, int type)
{
	return codec == Codec::HEVC ? type == 39 : type == 6;
}

bool isFirstSliceOfPicture(Codec codec, const Nal &nal)
{
	if(codec == Codec::HEVC)
		return nal.size >= 3 && (nal.data[2] & 0x80) != 0;
	// first_mb_in_slice is ue(v); value 0 is the single bit '1'.
	return nal.size >= 2 && (nal.data[1] & 0x80) != 0;
}

bool detectCodec(const uint8_t *buf, size_t size, Codec *out)
{
	std::vector<Nal> nals;
	splitNals(buf, size, nals);
	for(const Nal &n : nals)
	{
		if(n.size < 2 || (n.data[0] & 0x80))
			continue;  // forbidden_zero_bit set: not a NAL header of either codec
		int hevcType = (n.data[0] >> 1) & 0x3f;
		int hevcLayer = ((n.data[0] & 1) << 5) | (n.data[1] >> 3);
		int hevcTid = n.data[1] & 7;
		if((hevcType == 32 || hevcType == 33 || hevcType == 34) && hevcLayer == 0 && hevcTid == 1)
		{
			*out = Codec::HEVC;
			return true;
		}
		int h264Type = n.data[0] & 0x1f;
		int h264RefIdc = (n.data[0] >> 5) & 3;
		if((h264Type == 7 || h264Type == 8) && h264RefIdc != 0)
		{
			*out = Codec::H264;
			return true;
		}
	}
	return false;
}

bool ParameterSets::complete() const
{
	if(codec == Codec::HEVC)
		return !vps.empty() && !sps.empty() && !pps.empty();
	return !sps.empty() && !pps.empty();
}

void ParameterSets::add(const Nal &nal, int type)
{
	std::vector<std::vector<uint8_t>> *list = nullptr;
	if(codec == Codec::HEVC)
		list = type == 32 ? &vps : type == 33 ? &sps : type == 34 ? &pps : nullptr;
	else
		list = type == 7 ? &sps : type == 8 ? &pps : nullptr;
	if(!list)
		return;
	std::vector<uint8_t> v(nal.data, nal.data + nal.size);
	for(const auto &e : *list)
		if(e == v)
			return;
	list->push_back(std::move(v));
}

int ParameterSets::profileIdc() const
{
	if(sps.empty())
		return -1;
	const auto &s = sps.front();
	if(codec == Codec::HEVC)
	{
		// 2-byte NAL header, then vps_id(4) max_sub_layers(3) nesting(1), then
		// profile_space(2) tier(1) profile_idc(5).
		std::vector<uint8_t> r = unescapeRbsp(s.data(), s.size() < 16 ? s.size() : 16);
		return r.size() >= 4 ? (r[3] & 0x1f) : -1;
	}
	return s.size() >= 2 ? s[1] : -1;
}

std::vector<uint8_t> unescapeRbsp(const uint8_t *data, size_t size)
{
	std::vector<uint8_t> out;
	out.reserve(size);
	int zeros = 0;
	for(size_t i = 0; i < size; i++)
	{
		uint8_t b = data[i];
		if(zeros >= 2 && b == 3)
		{
			zeros = 0;
			continue;
		}
		zeros = b == 0 ? zeros + 1 : 0;
		out.push_back(b);
	}
	return out;
}

bool parseHdrSei(Codec codec, const Nal &nal, HdrSei &inout)
{
	size_t header = codec == Codec::HEVC ? 2 : 1;
	if(nal.size <= header || !isSei(codec, nalType(codec, nal)))
		return false;
	std::vector<uint8_t> r = unescapeRbsp(nal.data + header, nal.size - header);
	size_t p = 0;
	bool found = false;
	while(p < r.size())
	{
		if(r[p] == 0x80 && p + 1 == r.size())
			break;  // rbsp_trailing_bits
		uint32_t type = 0, size = 0;
		while(p < r.size() && r[p] == 0xff) { type += 255; p++; }
		if(p >= r.size()) break;
		type += r[p++];
		while(p < r.size() && r[p] == 0xff) { size += 255; p++; }
		if(p >= r.size()) break;
		size += r[p++];
		if(p + size > r.size())
			break;
		if(type == 137 && size >= 24)
		{
			std::memcpy(inout.masteringDisplay.data(), r.data() + p, 24);
			inout.hasMasteringDisplay = true;
			found = true;
		}
		else if(type == 144 && size >= 4)
		{
			std::memcpy(inout.contentLightLevel.data(), r.data() + p, 4);
			inout.hasContentLightLevel = true;
			found = true;
		}
		p += size;
	}
	return found;
}

void appendAnnexB(std::vector<uint8_t> &out, const Nal &nal)
{
	static const uint8_t sc[4] = {0, 0, 0, 1};
	out.insert(out.end(), sc, sc + 4);
	out.insert(out.end(), nal.data, nal.data + nal.size);
}

std::vector<AccessUnit> splitAccessUnits(Codec codec, const uint8_t *buf, size_t size)
{
	std::vector<Nal> nals;
	splitNals(buf, size, nals);
	std::vector<AccessUnit> aus;
	AccessUnit cur;
	bool curHasVcl = false;
	auto flush = [&]() {
		if(curHasVcl || !cur.config.empty())
			aus.push_back(std::move(cur));
		cur = AccessUnit();
		curHasVcl = false;
	};
	for(const Nal &n : nals)
	{
		int t = nalType(codec, n);
		bool vcl = isVcl(codec, t);
		bool startsAu;
		if(vcl)
			startsAu = curHasVcl && isFirstSliceOfPicture(codec, n);
		else if(codec == Codec::HEVC)
			startsAu = curHasVcl && (t == 35 || t == 32 || t == 33 || t == 34 || t == 39 || (t >= 41 && t <= 44));
		else
			startsAu = curHasVcl && (t == 9 || t == 7 || t == 8 || t == 6 || (t >= 14 && t <= 18));
		if(startsAu)
			flush();
		if(isAud(codec, t))
			continue;
		if(isParameterSet(codec, t))
			appendAnnexB(cur.config, n);
		else
			appendAnnexB(cur.frame, n);
		if(vcl)
		{
			curHasVcl = true;
			if(isKeyframe(codec, t))
				cur.keyframe = true;
		}
	}
	flush();
	return aus;
}

}  // namespace plk::video::annexb
