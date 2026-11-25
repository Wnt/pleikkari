// Video shaders. Compiled at runtime by MTLDevice newLibraryWithSource (the
// build embeds this file as a string), so no offline Metal toolchain is needed.
//
// Input: the decoder's two planes wrapped zero-copy as textures
//   texture(0): luma   (R8Unorm or R16Unorm with 10 bits in the MSBs)
//   texture(1): chroma (RG8Unorm or RG16Unorm), half resolution
// Output: non-linear R'G'B' in the stream's own primaries and transfer
// (BT.709 for SDR, BT.2020 + PQ for HDR10); the CAMetalLayer colour space tells
// the compositor what the values mean, so no transfer math happens here.

#include <metal_stdlib>
using namespace metal;

struct VSOut {
	float4 pos [[position]];
	float2 uv;
};

// One oversized triangle covering the viewport; uv (0,0) is the top-left.
vertex VSOut vs_fullscreen(uint vid [[vertex_id]])
{
	float2 p = float2((vid << 1) & 2, vid & 2);
	VSOut o;
	o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
	o.uv = float2(p.x, 1.0 - p.y);
	return o;
}

struct Params {
	// rgb = (dot(r0, v), dot(r1, v), dot(r2, v)) with v = (Y, Cb, Cr, 1) in raw
	// normalized texture units: range expansion, bit-depth container scale and
	// the YCbCr matrix are all folded into these rows on the CPU.
	float4 r0, r1, r2;
	float2 lumaSize;     // luma texture size in texels
	float2 chromaSize;   // chroma texture size in texels
	float2 uvScale;      // visible (clean) rect size / luma texture size
	float2 uvOffset;     // visible rect origin / luma texture size
	float2 chromaShift;  // chroma siting correction, in chroma texels
	float2 linearize;    // x = 0: code values as they are. x = 1: PQ to linear light, times y
	                     // (HdrOutput::ExtendedLinear). x = 2: PQ to linear light, times y, back
	                     // to PQ (a PQ drawable whose picture is scaled or tone mapped here).
	float2 toneMap;      // y > 0: linear light above the knee x is rolled off towards y, the
	                     // display's headroom, both in the units of the scaled linear light
};

constexpr sampler kLinear(filter::linear, address::clamp_to_edge, coord::normalized);
constexpr sampler kNearest(filter::nearest, address::clamp_to_edge, coord::normalized);

// SMPTE ST 2084 EOTF: PQ code value -> linear light, 1.0 = 10000 nits.
static inline float3 pq_eotf(float3 e)
{
	const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	float3 p = pow(e, 1.0 / m2);
	return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

// The inverse: linear light, 1.0 = 10000 nits -> PQ code value.
static inline float3 pq_inverse_eotf(float3 l)
{
	const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	float3 p = pow(saturate(l), m1);
	return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// Highlights the display cannot show: the brightest channel follows a curve that leaves
// the knee with slope 1 and approaches the headroom, and the other two keep their ratio
// to it, so a highlight loses brightness and not its colour.
static inline float3 roll_off(float3 lin, float2 tm)
{
	float m = max(lin.r, max(lin.g, lin.b));
	if(tm.y > 0.0 && m > tm.x)
	{
		float d = tm.y - tm.x;
		lin *= (tm.x + d * (1.0 - exp(-(m - tm.x) / d))) / m;
	}
	return lin;
}

static inline float4 to_rgb(float y, float2 c, constant Params &p)
{
	float4 v = float4(y, c, 1.0);
	float3 rgb = saturate(float3(dot(p.r0, v), dot(p.r1, v), dot(p.r2, v)));
	if(p.linearize.x > 1.5)
		rgb = pq_inverse_eotf(roll_off(pq_eotf(rgb) * p.linearize.y, p.toneMap));
	else if(p.linearize.x > 0.5)
		rgb = roll_off(pq_eotf(rgb) * p.linearize.y, p.toneMap);
	return float4(rgb, 1.0);
}

static inline float2 chroma_uv(float2 uv, constant Params &p)
{
	return uv + p.chromaShift / p.chromaSize;
}

// Catmull-Rom with 9 bilinear taps (weights folded into the sample position of
// the two middle taps per axis).
static inline float4 catmull_rom(texture2d<float> tex, float2 uv, float2 size)
{
	float2 pos = uv * size;
	float2 t1 = floor(pos - 0.5) + 0.5;
	float2 f = pos - t1;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 t0 = (t1 - 1.0) / size;
	float2 t3 = (t1 + 2.0) / size;
	float2 t12 = (t1 + w2 / w12) / size;
	float4 r = 0.0;
	r += tex.sample(kLinear, float2(t0.x, t0.y)) * (w0.x * w0.y);
	r += tex.sample(kLinear, float2(t12.x, t0.y)) * (w12.x * w0.y);
	r += tex.sample(kLinear, float2(t3.x, t0.y)) * (w3.x * w0.y);
	r += tex.sample(kLinear, float2(t0.x, t12.y)) * (w0.x * w12.y);
	r += tex.sample(kLinear, float2(t12.x, t12.y)) * (w12.x * w12.y);
	r += tex.sample(kLinear, float2(t3.x, t12.y)) * (w3.x * w12.y);
	r += tex.sample(kLinear, float2(t0.x, t3.y)) * (w0.x * w3.y);
	r += tex.sample(kLinear, float2(t12.x, t3.y)) * (w12.x * w3.y);
	r += tex.sample(kLinear, float2(t3.x, t3.y)) * (w3.x * w3.y);
	return r;
}

static inline float lanczos3_weight(float x)
{
	x = abs(x);
	if(x < 1e-5)
		return 1.0;
	if(x >= 3.0)
		return 0.0;
	float px = M_PI_F * x;
	return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

// Lanczos-3, 6x6 taps, clamped to the 2x2 neighbourhood (anti-ringing).
static inline float lanczos3(texture2d<float> tex, float2 uv, float2 size)
{
	float2 pos = uv * size - 0.5;
	float2 base = floor(pos);
	float2 f = pos - base;
	float wx[6], wy[6];
	float sx = 0.0, sy = 0.0;
	for(int i = 0; i < 6; i++)
	{
		wx[i] = lanczos3_weight(f.x - float(i - 2));
		wy[i] = lanczos3_weight(f.y - float(i - 2));
		sx += wx[i];
		sy += wy[i];
	}
	float acc = 0.0;
	float lo = 1.0, hi = 0.0;
	for(int j = 0; j < 6; j++)
	{
		float row = 0.0;
		float v = (base.y + float(j - 2) + 0.5) / size.y;
		for(int i = 0; i < 6; i++)
		{
			float s = tex.sample(kNearest, float2((base.x + float(i - 2) + 0.5) / size.x, v)).r;
			row += s * wx[i];
			if((i == 2 || i == 3) && (j == 2 || j == 3))
			{
				lo = min(lo, s);
				hi = max(hi, s);
			}
		}
		acc += row * wy[j];
	}
	return clamp(acc / (sx * sy), lo, hi);
}

fragment float4 fs_bilinear(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		constant Params &p [[buffer(0)]])
{
	float2 uv = in.uv * p.uvScale + p.uvOffset;
	float y = ty.sample(kLinear, uv).r;
	float2 c = tc.sample(kLinear, chroma_uv(uv, p)).rg;
	return to_rgb(y, c, p);
}

fragment float4 fs_bicubic(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		constant Params &p [[buffer(0)]])
{
	float2 uv = in.uv * p.uvScale + p.uvOffset;
	float y = catmull_rom(ty, uv, p.lumaSize).r;
	float2 c = catmull_rom(tc, chroma_uv(uv, p), p.chromaSize).rg;
	return to_rgb(y, c, p);
}

fragment float4 fs_lanczos(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		constant Params &p [[buffer(0)]])
{
	float2 uv = in.uv * p.uvScale + p.uvOffset;
	float y = lanczos3(ty, uv, p.lumaSize);
	float2 c = catmull_rom(tc, chroma_uv(uv, p), p.chromaSize).rg;
	return to_rgb(y, c, p);
}

// 1:1 copy of an RGB texture into the letterbox rect (MetalFX output -> drawable).
fragment float4 fs_copy(VSOut in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	return float4(t.sample(kNearest, in.uv).rgb, 1.0);
}
