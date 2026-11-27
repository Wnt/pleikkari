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
	                     // x = 3: the SDR rendition of a PQ picture that an enhancer is given
	float2 toneMap;      // y > 0: linear light above the knee x is rolled off towards y, the
	                     // display's headroom, both in the units of the scaled linear light
	float2 sdrMap;       // x > 0: a PQ picture, and its linear light (1.0 = 10000 nits) times x
	                     // has SDR white at 1.0; see sdr_rendition(). y is not used
	float2 warp;         // the picture is shown moved by this much of its width and height
	                     // (camera warp, VideoPipeline::setWarp): what moves in at an edge is
	                     // the picture mirrored there
	float2 unusedWarp;
};

constexpr sampler kLinear(filter::linear, address::clamp_to_edge, coord::normalized);

constexpr sampler kNearest(filter::nearest, address::clamp_to_edge, coord::normalized);

// Where in the picture (0...1 each way) the pixel at `uv` of the target looks: the picture
// moved by p.warp, and beyond an edge what lies as far inside it.
//
// Not everything in a picture slides when the camera turns: the character the camera
// follows, a crosshair and a HUD stay where they are on screen. `fixed` says where they are
// (1: fixed), as the pipeline learned it from the pictures, and `centre` the same point for
// point for the middle 2/15 of the picture each way, where a crosshair is. A pixel that
// shows something fixed is not moved, and neither is one that would be given something
// fixed from where it looks: a character is not smeared over what is beside it.
static inline float centre_fixed(float2 uv, texture2d<float> centre)
{
	float2 c = (uv - (0.5 - 1.0 / 15.0)) * 7.5;
	return all(c > 0.0) && all(c < 1.0) ? centre.sample(kLinear, c).r : 0.0;
}

static inline float2 warped(float2 uv, constant Params &p, texture2d<float> fixed, texture2d<float> centre)
{
	if(all(p.warp == 0.0))
		return uv;
	// Towards the edge the picture comes in from, there is nothing beyond it to show: the
	// shift is let go to nothing over the last three times its own size, so the picture is
	// stretched a little there. (The edge repeated outwards drew streaks a twentieth of the
	// picture wide; the picture mirrored there put a second tree at the edge.)
	float2 band = max(abs(p.warp) * 3.0, float2(1e-5));
	float2 from_edge = float2(p.warp.x > 0.0 ? uv.x : 1.0 - uv.x, p.warp.y > 0.0 ? uv.y : 1.0 - uv.y);
	float2 shift = p.warp * smoothstep(float2(0.0), band, from_edge);
	// And towards what is fixed to the screen (`fixed`: 1 at it, 8/255 less for each point
	// of a 192 x 108 map away from it): nothing at it, all of the shift from twice the
	// shift's own length away, at least three points. Never steeper than that, or what is
	// shown would fold over itself.
	float away = (1.0 - fixed.sample(kLinear, uv).r) * (255.0 / 8.0);
	float reach = max(3.0, 2.0 * length(shift * float2(192.0, 108.0)));
	shift *= smoothstep(0.0, reach, away);
	// The middle, point for point (a crosshair): all or nothing, here and where this pixel
	// would fetch from.
	float moved = (1.0 - centre_fixed(uv, centre)) * (1.0 - centre_fixed(uv - shift, centre));
	// And what is fixed at the picture's edge (a HUD; `fixed`'s second value): the same.
	moved *= (1.0 - smoothstep(0.4, 0.6, fixed.sample(kLinear, uv).g)) * (1.0 - smoothstep(0.4, 0.6, fixed.sample(kLinear, uv - shift).g));
	return clamp(uv - shift * moved, 0.0, 1.0);
}

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

// ---- the SDR rendition of an HDR picture ----------------------------------------
//
// An enhancer's network knows SDR pictures. Of a PQ picture it is given this rendition:
// linear light with SDR white at 1.0 in BT.709 primaries, as it is up to the knee and on a
// logarithmic shoulder above it that reaches 1.0 at kSdrPeak times SDR white, then the
// 2.2 gamma. So everything up to about 120 nits looks to the network as it would in an
// SDR stream, and the highlights are packed into the top fifth of the code values.
// Nothing is clamped here: a colour outside BT.709 has a negative component and a
// highlight above the peak a value over 1.0, and both stay what they are in the mix; only
// the copy the enhancer is given is saturated.

constant float kSdrKnee = 0.6;
// k + log(1 + a (x - k)) / a has slope 1 at the knee k, and is 1.0 at x = 1000 / 203
// (1000 nits with SDR white at 203) for a = 9.3.
constant float kSdrShoulder = 9.3;

static inline float3 bt2020_to_bt709(float3 v)
{
	return float3(dot(float3(1.660491, -0.587641, -0.072850), v),
			dot(float3(-0.124550, 1.132900, -0.008349), v),
			dot(float3(-0.018151, -0.100579, 1.118730), v));
}

static inline float3 bt709_to_bt2020(float3 v)
{
	return float3(dot(float3(0.627404, 0.329283, 0.043313), v),
			dot(float3(0.069097, 0.919540, 0.011362), v),
			dot(float3(0.016391, 0.088013, 0.895595), v));
}

// PQ code values to linear light in BT.709 primaries, SDR white at 1.0.
static inline float3 sdr_linear(float3 pq, constant Params &p)
{
	return bt2020_to_bt709(pq_eotf(pq) * p.sdrMap.x);
}

static inline float3 sdr_shoulder(float3 x)
{
	return min(x, kSdrKnee) + log(1.0 + kSdrShoulder * max(x - kSdrKnee, 0.0)) / kSdrShoulder;
}

static inline float3 sdr_encode(float3 t) { return sign(t) * pow(abs(t), 1.0 / 2.2); }
static inline float3 sdr_decode(float3 c) { return sign(c) * pow(abs(c), 2.2); }

static inline float3 sdr_rendition(float3 pq, constant Params &p)
{
	return sdr_encode(sdr_shoulder(sdr_linear(pq, p)));
}

// The stream's own R'G'B' code values: BT.709 gamma for SDR, BT.2020 and PQ for HDR10.
static inline float3 code_rgb(float y, float2 c, constant Params &p)
{
	float4 v = float4(y, c, 1.0);
	return saturate(float3(dot(p.r0, v), dot(p.r1, v), dot(p.r2, v)));
}

// Code values to what the target holds.
static inline float4 finish(float3 rgb, constant Params &p)
{
	if(p.linearize.x > 2.5)
		rgb = saturate(sdr_rendition(rgb, p));
	else if(p.linearize.x > 1.5)
		rgb = pq_inverse_eotf(roll_off(pq_eotf(rgb) * p.linearize.y, p.toneMap));
	else if(p.linearize.x > 0.5)
		rgb = roll_off(pq_eotf(rgb) * p.linearize.y, p.toneMap);
	return float4(rgb, 1.0);
}

static inline float4 to_rgb(float y, float2 c, constant Params &p)
{
	return finish(code_rgb(y, c, p), p);
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
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]])
{
	float2 uv = warped(in.uv, p, tfixed, tcentre) * p.uvScale + p.uvOffset;
	float y = ty.sample(kLinear, uv).r;
	float2 c = tc.sample(kLinear, chroma_uv(uv, p)).rg;
	return to_rgb(y, c, p);
}

fragment float4 fs_bicubic(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]])
{
	float2 uv = warped(in.uv, p, tfixed, tcentre) * p.uvScale + p.uvOffset;
	float y = catmull_rom(ty, uv, p.lumaSize).r;
	float2 c = catmull_rom(tc, chroma_uv(uv, p), p.chromaSize).rg;
	return to_rgb(y, c, p);
}

fragment float4 fs_lanczos(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]])
{
	float2 uv = warped(in.uv, p, tfixed, tcentre) * p.uvScale + p.uvOffset;
	float y = lanczos3(ty, uv, p.lumaSize);
	float2 c = catmull_rom(tc, chroma_uv(uv, p), p.chromaSize).rg;
	return to_rgb(y, c, p);
}

// ---- enhancer mix -----------------------------------------------------------
//
// A FrameEnhancer (neural rendering) is slower than the stream: what it returns is a
// changed copy of an earlier frame, the key frame. Added to the frame on screen is its
// *change*, enhanced - key, and only where the frame on screen still shows what the key
// frame showed; where the picture has moved on, the change belongs to something that is
// no longer there and is left out.
//   texture(2): the key frame as the enhancer was given it: R'G'B', the stream's own code
//               values of an SDR picture, the SDR rendition of a PQ picture
//   texture(3): the enhancer's result for it, same size
// The change is made to the stream's code values, before they become what the target
// holds: the brightness, the tone mapping and the scaler see the enhanced picture.

struct NrParams {
	float4 lumaWeights;  // xyz: weights of R', G', B' in the stream's luma
	float2 keySize;      // size of the key and enhanced textures in texels
	float2 motion;       // difference where the change starts (x) and ends (y) fading out
	float strength;      // how much of the change is added; the fade by age is folded in
	float split;         // > 0: left of this u the picture stays as it is (comparison)
	float guided;        // > 0: the change's tone comes from the grid of texture(4), see below
	float unused;
};

// ---- the guided mix: the enhancer's change as a function of place and brightness --------
//
// A change kept per pixel belongs to where things were in the key frame; laid over a later
// frame it trails behind whatever moved. But most of what the network does to a picture is
// tone: it lifts or lowers areas by how bright they are and where they are. That part can
// be kept as a small table instead, the mean change in each of kGridCols x kGridRows places
// for each of kGridBins brightnesses (a bilateral grid, as in Gharbi et al., "Deep Bilateral
// Learning", 2017), and looked up for any later frame with that frame's own pixel: a bright
// thing that moved takes the bright change with it, and the dark place it left gets the
// dark one. Only the fine detail on top of the tone, the change minus the table's, is still
// tied to the key frame's pixels, and it is added where the picture has not moved.
//   texture(4): the grid, kGridCols * kGridBins wide and kGridRows high: for bin b, column
//               c and row r the texel (b * kGridCols + c, r) holds the sum of the changes
//               (rgb) and of their weights (a)

constant int kGridCols = 32;
constant int kGridRows = 18;
constant int kGridBins = 8;

struct GridParams {
	float4 lumaWeights;  // xyz: weights of R', G', B' in luma
	float blend;         // > 0: this much of the new table, the rest the one before (steadier)
	float unused0, unused1, unused2;
};

// One texel of the table: the weighted mean change of the key frame's pixels around its
// place whose brightness is near its bin. 12 x 12 taps over two cells each way.
fragment float4 fs_grid_build(VSOut in [[stage_in]],
		texture2d<float> tkey [[texture(0)]], texture2d<float> tenh [[texture(1)]],
		texture2d<float> tprev [[texture(2)]], constant GridParams &g [[buffer(0)]])
{
	int ix = clamp(int(in.uv.x * float(kGridCols * kGridBins)), 0, kGridCols * kGridBins - 1);
	int row = clamp(int(in.uv.y * float(kGridRows)), 0, kGridRows - 1);
	int bin = ix / kGridCols;
	int col = ix - bin * kGridCols;
	float centre = float(bin) / float(kGridBins - 1);
	float3 sum = float3(0.0);
	float weights = 0.0;
	const int kTaps = 12;
	for(int j = 0; j < kTaps; j++)
	{
		for(int i = 0; i < kTaps; i++)
		{
			float2 o = (float2(i, j) + 0.5) / float(kTaps) * 2.0 - 1.0;
			float2 uv = (float2(col, row) + 0.5 + o) / float2(kGridCols, kGridRows);
			if(any(uv < float2(0.0)) || any(uv > float2(1.0)))
				continue;
			float3 k = tkey.sample(kLinear, uv).rgb;
			float3 e = tenh.sample(kLinear, uv).rgb;
			float w = (1.0 - abs(o.x)) * (1.0 - abs(o.y))
					* max(0.0, 1.0 - abs(dot(k, g.lumaWeights.xyz) - centre) * float(kGridBins - 1));
			sum += (e - k) * w;
			weights += w;
		}
	}
	float4 now = float4(sum, weights);
	if(g.blend > 0.0)
		now = mix(tprev.read(uint2(ix, row)), now, g.blend);
	return now;
}

// The table's change for a pixel at `uv` whose brightness is `luma`.
static inline float3 grid_change(texture2d<float> tgrid, float2 uv, float luma)
{
	float b = saturate(luma) * float(kGridBins - 1);
	float b0 = min(floor(b), float(kGridBins - 2));
	// Within a bin's own columns: the texels beside them belong to another brightness.
	float u = clamp(uv.x * float(kGridCols), 0.5, float(kGridCols) - 0.5);
	float v = clamp(uv.y * float(kGridRows), 0.5, float(kGridRows) - 0.5) / float(kGridRows);
	float4 lo = tgrid.sample(kLinear, float2((u + b0 * float(kGridCols)) / float(kGridCols * kGridBins), v));
	float4 hi = tgrid.sample(kLinear, float2((u + (b0 + 1.0) * float(kGridCols)) / float(kGridCols * kGridBins), v));
	float4 s = mix(lo, hi, b - b0);
	return s.w > 1e-4 ? s.rgb / s.w : float3(0.0);
}

// Adds `d`, a change in the code values the enhancer works in, to a pixel's code values.
static inline float3 nr_apply(float3 code, float3 d, constant Params &p)
{
	if(p.sdrMap.x <= 0.0)
		return saturate(code + d);
	// A PQ picture: the change belongs to the SDR rendition. It is made there, and the
	// pixel keeps the ratio it had to its rendition: 1 up to the knee, where this is the
	// SDR mix exactly, and more in a highlight, which so changes by the same proportion
	// as its rendition did and keeps its place above SDR white. Nothing here inverts the
	// shoulder, so a small change stays a small change however bright the pixel is.
	if(all(abs(d) < 1e-5))
		return code;
	float3 lin = sdr_linear(code, p);
	float3 t = sdr_shoulder(lin);
	float3 gain = select(float3(1.0), lin / t, lin > kSdrKnee);
	float3 c = sdr_encode(t);
	// A component does not change sign: one that is in BT.709 stays in it.
	float3 changed = max(c + d, min(c, 0.0));
	return pq_inverse_eotf(bt709_to_bt2020(sdr_decode(changed) * gain) / p.sdrMap.x);
}

static inline float4 nr_mix(float3 code, float2 uv, texture2d<float> ty, texture2d<float> tc,
		texture2d<float> tkey, texture2d<float> tenh, texture2d<float> tgrid, constant Params &p, constant NrParams &n)
{
	bool hdr = p.sdrMap.x > 0.0;
	// How far this frame is from the key frame, at the key frame's own resolution: this
	// frame is sampled at the centres of the four key texels around the pixel exactly as
	// the key frame was drawn (fs_bilinear), so an unchanged picture measures zero whatever
	// the scaler and the sizes are. Per channel, weighted like luma: a change of colour
	// alone counts too.
	float2 pos = uv * n.keySize - 0.5;
	float2 first = floor(pos);
	float2 f = pos - first;
	float diff = 0.0;
	// The change, brought to this pixel (after Veyra-NRVideo's residual composite): the
	// enhancer worked on a smaller picture, and each of the four key texels around this
	// pixel has a change of its own. A plain bilinear mix of the four lays a change made to
	// one side of an edge over the other side too. Here a texel counts for less the less its
	// key colour is the colour of this very pixel, taken from the stream at its full
	// resolution, so the change follows the picture's own edges. Measured against a result
	// made at the stream's own size it is about 1 % closer than the plain mix, at the same
	// cost (0.32 ms a frame either way), so it is the only way.
	float3 here = hdr ? saturate(sdr_rendition(code, p)) : code;
	float3 guidedChange = float3(0.0);
	float guidedWeight = 0.0;
	for(int j = 0; j < 2; j++)
	{
		for(int i = 0; i < 2; i++)
		{
			float2 texel = clamp(first + float2(i, j), float2(0.0), n.keySize - 1.0);
			float2 kuv = (texel + 0.5) / n.keySize;
			float2 suv = kuv * p.uvScale + p.uvOffset;
			float3 now = code_rgb(ty.sample(kLinear, suv).r, tc.sample(kLinear, chroma_uv(suv, p)).rg, p);
			if(hdr)
				now = saturate(sdr_rendition(now, p));
			float3 then = tkey.sample(kNearest, kuv).rgb;
			float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
			diff += dot(abs(now - then), n.lumaWeights.xyz) * w;
			float gw = w / (1.0 + 16.0 * dot(abs(here - then), n.lumaWeights.xyz));
			guidedChange += (tenh.sample(kNearest, kuv).rgb - then) * gw;
			guidedWeight += gw;
		}
	}
	float keep = 1.0 - smoothstep(n.motion.x, n.motion.y, diff);
	float3 change = guidedChange / max(guidedWeight, 1e-6);
	if(n.guided > 0.0)
	{
		// Tone from the table, by this frame's own pixel; and of the key frame's pixel here
		// only what its change has beyond the table's, where the picture has not moved.
		float3 keyHere = tkey.sample(kLinear, uv).rgb;
		float3 tone = grid_change(tgrid, uv, dot(here, n.lumaWeights.xyz));
		float3 detail = change - grid_change(tgrid, uv, dot(keyHere, n.lumaWeights.xyz));
		change = tone + keep * detail;
		keep = 1.0;
	}
	if(n.split > 0.0)
	{
		// The comparison: nothing left of the line, and the line itself, in SDR white.
		float edge = uv.x - n.split;
		if(abs(edge) < fwidth(uv.x))
			return finish(hdr ? pq_inverse_eotf(float3(1.0 / p.sdrMap.x)) : float3(1.0), p);
		if(edge < 0.0)
			return finish(code, p);
	}
	return finish(nr_apply(code, change * (n.strength * keep), p), p);
}

fragment float4 fs_bilinear_nr(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		texture2d<float> tkey [[texture(2)]], texture2d<float> tenh [[texture(3)]], texture2d<float> tgrid [[texture(4)]],
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]], constant NrParams &n [[buffer(1)]])
{
	float2 uv = warped(in.uv, p, tfixed, tcentre) * p.uvScale + p.uvOffset;
	float y = ty.sample(kLinear, uv).r;
	float2 c = tc.sample(kLinear, chroma_uv(uv, p)).rg;
	return nr_mix(code_rgb(y, c, p), warped(in.uv, p, tfixed, tcentre), ty, tc, tkey, tenh, tgrid, p, n);
}

fragment float4 fs_bicubic_nr(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		texture2d<float> tkey [[texture(2)]], texture2d<float> tenh [[texture(3)]], texture2d<float> tgrid [[texture(4)]],
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]], constant NrParams &n [[buffer(1)]])
{
	float2 uv = warped(in.uv, p, tfixed, tcentre) * p.uvScale + p.uvOffset;
	float y = catmull_rom(ty, uv, p.lumaSize).r;
	float2 c = catmull_rom(tc, chroma_uv(uv, p), p.chromaSize).rg;
	return nr_mix(code_rgb(y, c, p), warped(in.uv, p, tfixed, tcentre), ty, tc, tkey, tenh, tgrid, p, n);
}

fragment float4 fs_lanczos_nr(VSOut in [[stage_in]],
		texture2d<float> ty [[texture(0)]], texture2d<float> tc [[texture(1)]],
		texture2d<float> tkey [[texture(2)]], texture2d<float> tenh [[texture(3)]], texture2d<float> tgrid [[texture(4)]],
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]], constant NrParams &n [[buffer(1)]])
{
	float2 uv = warped(in.uv, p, tfixed, tcentre) * p.uvScale + p.uvOffset;
	float y = lanczos3(ty, uv, p.lumaSize);
	float2 c = catmull_rom(tc, chroma_uv(uv, p), p.chromaSize).rg;
	return nr_mix(code_rgb(y, c, p), warped(in.uv, p, tfixed, tcentre), ty, tc, tkey, tenh, tgrid, p, n);
}

// 1:1 copy of an RGB texture into the letterbox rect (MetalFX output -> drawable).
// A whole picture in the stream's own R'G'B' code values (frame generation works on such
// pictures and returns one): from here on it is treated as a decoded frame's pixel is.
fragment float4 fs_rgb(VSOut in [[stage_in]], texture2d<float> t [[texture(0)]],
		texture2d<float> tfixed [[texture(6)]], texture2d<float> tcentre [[texture(7)]],
		constant Params &p [[buffer(0)]])
{
	return finish(saturate(t.sample(kLinear, warped(in.uv, p, tfixed, tcentre)).rgb), p);
}

fragment float4 fs_copy(VSOut in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	return float4(t.sample(kNearest, in.uv).rgb, 1.0);
}

// ---- an overlay over the picture (VideoPipeline::setOverlay) ----------------------
//
// texture(0): the overlay, sRGB code values with premultiplied alpha, drawn one to one
// into a viewport of its own size, blended over what the frame's draw left there. What
// the target holds is not always sRGB code values, so the colour is first made what the
// target expects, with the overlay's white at the display's SDR white.

struct OverlayParams {
	// x = 0: the target holds SDR code values, the overlay's go in as they are.
	// x = 1: PQ code values in BT.2020; y is the overlay's white in units of 10000 nits.
	// x = 2: linear light in BT.2020; y is the overlay's white.
	// x = 3: linear light in the overlay's own primaries; y is the overlay's white.
	float2 mode;
	float2 unused;
};

fragment float4 fs_overlay(VSOut in [[stage_in]], texture2d<float> t [[texture(0)]],
		constant OverlayParams &p [[buffer(0)]])
{
	float4 c = t.sample(kNearest, in.uv);
	if(p.mode.x < 0.5 || c.a <= 0.0)
		return c;
	float3 lin = pow(saturate(c.rgb / c.a), 2.2) * p.mode.y;
	if(p.mode.x < 2.5)
		lin = bt709_to_bt2020(lin);
	float3 v = p.mode.x < 1.5 ? pq_inverse_eotf(lin) : lin;
	return float4(v * c.a, c.a);
}
