// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PLE-603: original ambient environments around the curved stream screen. See
// vr-environment.h for the contract and docs/design/vr-environments.md for the study
// these choices come from. Everything here is procedural: no asset files, no textures
// besides the 8x8 glow map rendered from the video each frame.

#include "vr-environment.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VrEnvironment", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VrEnvironment", __VA_ARGS__)
#else
#define LOGE(...) do { fprintf(stderr, "VrEnvironment: " __VA_ARGS__); fputc('\n', stderr); } while(0)
#define LOGI(...) do { fprintf(stderr, "VrEnvironment: " __VA_ARGS__); fputc('\n', stderr); } while(0)
#endif

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kGlowSize = 8;
// Skybox lerps its frame colour at 8/s; at 60 new frames per second that is 0.13 per frame.
constexpr float kGlowBlend = 0.13f;
constexpr int kTimerRing = 4;

// Screen limits. The height follows from width / aspect.
constexpr float kMinDistance = 0.8f, kMaxDistance = 12.0f;
constexpr float kMinWidth = 0.5f, kMaxWidth = 16.0f;
constexpr float kMaxHeightOffset = 2.0f;

// ---------------------------------------------------------------- small vector math
struct Vec3 {
	float x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a) { float l = length(a); return l > 0 ? a * (1.0f / l) : Vec3{0, 0, 1}; }

void mat4_multiply(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for(int c = 0; c < 4; ++c)
		for(int rw = 0; rw < 4; ++rw)
			r[c * 4 + rw] = a[0 * 4 + rw] * b[c * 4 + 0] + a[1 * 4 + rw] * b[c * 4 + 1] +
					a[2 * 4 + rw] * b[c * 4 + 2] + a[3 * 4 + rw] * b[c * 4 + 3];
	memcpy(out, r, sizeof(r));
}

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// Deterministic hash for procedural placement (stars, seat shade variation).
uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return x;
}
float hash01(uint32_t x) { return (hash32(x) & 0xffffff) / 16777216.0f; }

// ---------------------------------------------------------------- GL helpers
GLuint compile(GLenum type, const char *source)
{
	GLuint id = glCreateShader(type);
	glShaderSource(id, 1, &source, nullptr);
	glCompileShader(id);
	GLint ok = 0;
	glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
	if(!ok)
	{
		char log[2048];
		glGetShaderInfoLog(id, sizeof(log), nullptr, log);
		LOGE("shader compile failed: %s", log);
		glDeleteShader(id);
		return 0;
	}
	return id;
}

GLuint link(const char *vs, const char *fs)
{
	GLuint v = compile(GL_VERTEX_SHADER, vs);
	GLuint f = compile(GL_FRAGMENT_SHADER, fs);
	if(!v || !f)
	{
		if(v) glDeleteShader(v);
		if(f) glDeleteShader(f);
		return 0;
	}
	GLuint p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glLinkProgram(p);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if(!ok)
	{
		char log[2048];
		glGetProgramInfoLog(p, sizeof(log), nullptr, log);
		LOGE("program link failed: %s", log);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

// ---------------------------------------------------------------- shaders
// Room geometry: per-vertex lighting. Static room light is baked into the vertex
// (aLight.x), the screen's contribution is computed here from the screen rectangle and
// the 8x8 glow map, so it follows the configured screen size and distance and takes the
// colour of the part of the picture nearest to the vertex. Smooth gradients are dithered
// in the fragment shader; without that the Go's panel shows visible banding in dark rooms.
const char *kRoomVertex = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec3 aAlbedo;
layout(location = 3) in vec2 aLight;
uniform mat4 uViewProj;
uniform vec3 uScreenCenter;
uniform vec3 uScreenRight;
uniform vec3 uScreenUp;
uniform vec3 uScreenNormal;
uniform sampler2D uGlow;
uniform float uGlowStrength;
uniform float uRoomLight;
uniform float uAmbient;
out mediump vec3 vColor;
void main() {
	gl_Position = uViewProj * vec4(aPos, 1.0);
	vec3 d = aPos - uScreenCenter;
	float rl2 = dot(uScreenRight, uScreenRight);
	float ul2 = dot(uScreenUp, uScreenUp);
	float sx = clamp(dot(d, uScreenRight) / rl2, -1.0, 1.0);
	float sy = clamp(dot(d, uScreenUp) / ul2, -1.0, 1.0);
	vec3 p = uScreenCenter + uScreenRight * sx + uScreenUp * sy;
	vec3 toScreen = p - aPos;
	float dist2 = max(dot(toScreen, toScreen), 0.3);
	vec3 L = toScreen * inversesqrt(dist2);
	float facing = max(dot(-L, uScreenNormal), 0.0);
	float lambert = max(dot(aNrm, L), 0.0);
	float area = 4.0 * sqrt(rl2 * ul2);
	float screenLight = lambert * facing * area / (dist2 * 3.14159);
	// Skybox keeps a 0.15 grey floor under its frame colour so a black frame never
	// switches the room off; the same floor here, then a boost like its x50 / x2 layers.
	vec3 glow = max(textureLod(uGlow, vec2(sx, -sy) * 0.5 + 0.5, 0.0).rgb, vec3(0.12));
	vec3 warm = vec3(1.0, 0.86, 0.72);
	vec3 light = vec3(uAmbient) + aLight.x * uRoomLight * warm + glow * screenLight * uGlowStrength * 4.0;
	vColor = aAlbedo * light + aAlbedo * aLight.y * (1.5 + 1.5 * uRoomLight);
}
)";

const char *kRoomFragment = R"(#version 300 es
precision mediump float;
in mediump vec3 vColor;
out vec4 fragColor;
void main() {
	// Interleaved-gradient dither, +-0.5/255, breaks up banding on the dark walls.
	float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	fragColor = vec4(vColor + (n - 0.5) / 255.0, 1.0);
}
)";

// Sky dome and terrace floor haze: per-pixel gradient from the view direction, tinted by
// the average picture colour so the void breathes with the game.
const char *kSkyVertex = R"(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
out mediump vec3 vDir;
void main() {
	vDir = aPos;
	vec4 clip = uViewProj * vec4(aPos, 1.0);
	gl_Position = clip.xyww;
}
)";

const char *kSkyFragment = R"(#version 300 es
precision mediump float;
in mediump vec3 vDir;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uGround;
uniform vec3 uGlowAverage;
uniform float uGlowStrength;
uniform float uHorizonWidth;
out vec4 fragColor;
void main() {
	vec3 d = normalize(vDir);
	float up = d.y;
	float band = exp(-abs(up) / uHorizonWidth);
	vec3 sky = mix(uZenith, uHorizon, band);
	vec3 ground = mix(uGround, uHorizon, band);
	vec3 c = up >= 0.0 ? sky : ground;
	// The picture faces +z from -z; light the hemisphere in front of the viewer more.
	float front = clamp(-d.z, 0.0, 1.0);
	c += uGlowAverage * uGlowStrength * 0.25 * front * band;
	float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	fragColor = vec4(c + (n - 0.5) / 255.0, 1.0);
}
)";

const char *kStarVertex = R"(#version 300 es
layout(location = 0) in vec4 aPosSize;
uniform mat4 uViewProj;
out mediump float vBright;
void main() {
	vec4 clip = uViewProj * vec4(aPosSize.xyz, 1.0);
	gl_Position = clip.xyww;
	gl_PointSize = aPosSize.w;
	vBright = aPosSize.w * 0.35;
}
)";

const char *kStarFragment = R"(#version 300 es
precision mediump float;
in mediump float vBright;
uniform float uBrightness;
out vec4 fragColor;
void main() {
	vec2 p = gl_PointCoord * 2.0 - 1.0;
	float a = clamp(1.0 - dot(p, p), 0.0, 1.0);
	fragColor = vec4(vec3(0.9, 0.93, 1.0) * a * vBright * uBrightness, 1.0);
}
)";

// Screen glow halo behind the picture: additive, soft radial falloff.
const char *kHaloVertex = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uViewProj;
out mediump vec2 vUv;
void main() {
	gl_Position = uViewProj * vec4(aPos, 1.0);
	vUv = aUv;
}
)";

const char *kHaloFragment = R"(#version 300 es
precision mediump float;
in mediump vec2 vUv;
uniform sampler2D uGlow;
uniform float uStrength;
out vec4 fragColor;
void main() {
	vec2 q = vUv * 2.0 - 1.0;
	float r = length(q);
	float a = smoothstep(1.0, 0.35, r);
	vec3 c = textureLod(uGlow, clamp(vUv, 0.0, 1.0), 1.0).rgb;
	fragColor = vec4(c * a * uStrength, 1.0);
}
)";

// The picture itself. The sampler type is chosen at compile time.
const char *kScreenVertex = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uViewProj;
uniform mat4 uTexTransform;
out highp vec2 vUv;
void main() {
	gl_Position = uViewProj * vec4(aPos, 1.0);
	vUv = (uTexTransform * vec4(aUv, 0.0, 1.0)).xy;
}
)";

const char *kScreenFragmentExternal = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uVideo;
uniform int uHasVideo;
uniform vec3 uIdle;
in highp vec2 vUv;
out vec4 fragColor;
void main() {
	fragColor = uHasVideo != 0 ? vec4(texture(uVideo, vUv).rgb, 1.0) : vec4(uIdle, 1.0);
}
)";

const char *kScreenFragment2D = R"(#version 300 es
precision mediump float;
uniform sampler2D uVideo;
uniform int uHasVideo;
uniform vec3 uIdle;
in highp vec2 vUv;
out vec4 fragColor;
void main() {
	fragColor = uHasVideo != 0 ? vec4(texture(uVideo, vUv).rgb, 1.0) : vec4(uIdle, 1.0);
}
)";

// 8x8 downsample of the picture: each output texel averages a 4x4 grid of taps across
// its cell, so the whole frame contributes. Blended into the previous map with constant
// alpha for the temporal smoothing. One fullscreen triangle, 64 fragments.
const char *kGlowVertex = R"(#version 300 es
out highp vec2 vUv;
void main() {
	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
	vUv = p * 0.5 + 0.5;
	gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char *kGlowFragmentExternal = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uVideo;
uniform mat4 uTexTransform;
in highp vec2 vUv;
out vec4 fragColor;
void main() {
	vec3 sum = vec3(0.0);
	for(int y = 0; y < 4; ++y)
		for(int x = 0; x < 4; ++x) {
			vec2 uv = vUv + (vec2(float(x), float(y)) + 0.5 - 2.0) / 32.0;
			sum += texture(uVideo, (uTexTransform * vec4(uv, 0.0, 1.0)).xy).rgb;
		}
	fragColor = vec4(sum / 16.0, 1.0);
}
)";

const char *kGlowFragment2D = R"(#version 300 es
precision mediump float;
uniform sampler2D uVideo;
uniform mat4 uTexTransform;
in highp vec2 vUv;
out vec4 fragColor;
void main() {
	vec3 sum = vec3(0.0);
	for(int y = 0; y < 4; ++y)
		for(int x = 0; x < 4; ++x) {
			vec2 uv = vUv + (vec2(float(x), float(y)) + 0.5 - 2.0) / 32.0;
			sum += texture(uVideo, (uTexTransform * vec4(uv, 0.0, 1.0)).xy).rgb;
		}
	fragColor = vec4(sum / 16.0, 1.0);
}
)";

// ---------------------------------------------------------------- mesh building
struct RoomVertex {
	float px, py, pz;
	float nx, ny, nz;
	float r, g, b;
	float roomLight, emissive;
};

struct Rgb {
	float r, g, b;
};

struct Light {
	Vec3 position;
	float intensity;
};

// Collects triangles for the room program. Baked room light is evaluated per vertex from
// a list of point lights (sconces, aisle strips) with a Lambert term and inverse-square
// falloff, which is what a lightmap bake would give for a room with no bounce.
class RoomBuilder
{
public:
	std::vector<RoomVertex> vertices;
	std::vector<uint16_t> indices;
	std::vector<Light> lights;

	void vertex(Vec3 p, Vec3 n, Rgb c, float emissive)
	{
		float light = 0.0f;
		for(const Light &l : lights)
		{
			Vec3 d = l.position - p;
			float d2 = std::max(dot(d, d), 0.05f);
			float lambert = std::max(dot(n, d * (1.0f / std::sqrt(d2))), 0.0f);
			light += l.intensity * lambert / d2;
		}
		vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, c.r, c.g, c.b, std::min(light, 4.0f), emissive});
	}

	// Quad a-b-c-d counter-clockwise as seen from the side the normal points to.
	void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Rgb color, float emissive = 0.0f)
	{
		Vec3 n = normalize(cross(b - a, c - a));
		uint16_t base = static_cast<uint16_t>(vertices.size());
		vertex(a, n, color, emissive);
		vertex(b, n, color, emissive);
		vertex(c, n, color, emissive);
		vertex(d, n, color, emissive);
		indices.insert(indices.end(), {base, uint16_t(base + 1), uint16_t(base + 2), base, uint16_t(base + 2), uint16_t(base + 3)});
	}

	// Subdivided quad so the per-vertex lighting stays smooth on large surfaces.
	void grid(Vec3 origin, Vec3 du, Vec3 dv, int nu, int nv, Rgb color, float emissive = 0.0f)
	{
		for(int i = 0; i < nu; ++i)
			for(int j = 0; j < nv; ++j)
			{
				Vec3 a = origin + du * (float(i) / nu) + dv * (float(j) / nv);
				Vec3 b = origin + du * (float(i + 1) / nu) + dv * (float(j) / nv);
				Vec3 c = origin + du * (float(i + 1) / nu) + dv * (float(j + 1) / nv);
				Vec3 d = origin + du * (float(i) / nu) + dv * (float(j + 1) / nv);
				quad(a, b, c, d, color, emissive);
			}
	}

	// Axis-aligned box, outward normals. min/max corners.
	void box(Vec3 lo, Vec3 hi, Rgb color, float emissive = 0.0f)
	{
		Vec3 p000{lo.x, lo.y, lo.z}, p100{hi.x, lo.y, lo.z}, p010{lo.x, hi.y, lo.z}, p110{hi.x, hi.y, lo.z};
		Vec3 p001{lo.x, lo.y, hi.z}, p101{hi.x, lo.y, hi.z}, p011{lo.x, hi.y, hi.z}, p111{hi.x, hi.y, hi.z};
		quad(p001, p101, p111, p011, color, emissive); // +z
		quad(p100, p000, p010, p110, color, emissive); // -z
		quad(p101, p100, p110, p111, color, emissive); // +x
		quad(p000, p001, p011, p010, color, emissive); // -x
		quad(p010, p011, p111, p110, color, emissive); // +y
		quad(p000, p100, p101, p001, color, emissive); // -y
	}

	size_t triangles() const { return indices.size() / 3; }
};

struct ScreenVertex {
	float px, py, pz, u, v;
};

// The picture surface: a vertical cylinder section (or a flat quad when radius is 0).
// The arc is centred on the screen centre; the cylinder axis sits behind the screen at
// distance radius, so radius == distance puts the viewer on the axis.
void build_screen(const PleikkariVrEnvironmentConfig &cfg, std::vector<ScreenVertex> &out, float inflate,
		float zOffset, std::vector<Vec3> *corners)
{
	const int segments = 48;
	const float width = cfg.screen_width_m * inflate;
	const float height = cfg.screen_width_m / cfg.screen_aspect * inflate;
	const float cy = cfg.screen_height_offset_m;
	const float cz = -cfg.screen_distance_m - zOffset;
	const float radius = cfg.screen_curve_radius_m;
	out.clear();
	for(int i = 0; i <= segments; ++i)
	{
		float u = float(i) / segments;
		float x, z;
		if(radius > 0.0f)
		{
			float angle = (u - 0.5f) * (width / radius);
			x = radius * std::sin(angle);
			z = cz + radius - radius * std::cos(angle);
		}
		else
		{
			x = (u - 0.5f) * width;
			z = cz;
		}
		// Triangle strip: bottom then top. UV v=0 at the bottom (GL convention for the
		// SurfaceTexture transform).
		out.push_back({x, cy - height * 0.5f, z, u, 0.0f});
		out.push_back({x, cy + height * 0.5f, z, u, 1.0f});
	}
	if(corners)
	{
		corners->clear();
		corners->push_back({out.front().px, out.front().py, out.front().pz});
		corners->push_back({out.back().px, out.back().py, out.back().pz});
	}
}

// ---------------------------------------------------------------- environments
struct Palette {
	Rgb wall, floor, ceiling, seat, seatFrame, trim, stage, sconce, aisle;
};

Palette cinema_palette()
{
	return {
		{0.34f, 0.19f, 0.17f}, // wall: dark plum fabric
		{0.22f, 0.11f, 0.11f}, // floor: dark carpet
		{0.14f, 0.12f, 0.12f}, // ceiling
		{0.46f, 0.10f, 0.13f}, // seat cushion: burgundy
		{0.18f, 0.16f, 0.16f}, // seat frame and armrests
		{0.30f, 0.22f, 0.14f}, // wood trim
		{0.09f, 0.07f, 0.07f}, // stage front
		{1.00f, 0.75f, 0.45f}, // sconce emissive
		{0.55f, 0.65f, 1.00f}, // aisle strip emissive (cool)
	};
}

// A theatre seat facing -z (toward the screen), origin at the front centre of the cushion
// at floor level.
void seat(RoomBuilder &b, Vec3 at, const Palette &p, uint32_t salt)
{
	float shade = 0.85f + 0.3f * hash01(salt);
	Rgb cushion{p.seat.r * shade, p.seat.g * shade, p.seat.b * shade};
	const float w = 0.52f, depth = 0.50f, seatH = 0.44f, backH = 1.05f;
	// pedestal
	b.box({at.x - w * 0.5f + 0.06f, at.y, at.z}, {at.x + w * 0.5f - 0.06f, at.y + seatH - 0.10f, at.z + depth}, p.seatFrame);
	// cushion
	b.box({at.x - w * 0.5f, at.y + seatH - 0.10f, at.z}, {at.x + w * 0.5f, at.y + seatH, at.z + depth}, cushion);
	// backrest, slightly reclined by being a box a little further back at the top
	b.box({at.x - w * 0.5f, at.y + seatH, at.z + depth - 0.10f}, {at.x + w * 0.5f, at.y + backH, at.z + depth + 0.04f}, cushion);
	// armrests
	b.box({at.x - w * 0.5f - 0.05f, at.y + seatH, at.z + 0.05f}, {at.x - w * 0.5f + 0.02f, at.y + seatH + 0.22f, at.z + depth}, p.seatFrame);
	b.box({at.x + w * 0.5f - 0.02f, at.y + seatH, at.z + 0.05f}, {at.x + w * 0.5f + 0.05f, at.y + seatH + 0.22f, at.z + depth}, p.seatFrame);
}

// The cinema hall. Real metres, viewer seated at the origin (eye 1.2 m above the row's
// floor), in the front row of the seating block so no seat ever crosses the picture.
// Numbers come from the study note: Skybox's hall is a scaled-up multiplex; ours is a
// small screening room sized around the configured screen.
void build_cinema(RoomBuilder &b, const PleikkariVrEnvironmentConfig &cfg)
{
	const Palette p = cinema_palette();
	const float eye = 1.2f;                       // seated eye height above the row floor
	const float rowFloor = -eye;                   // y of the viewer's row floor
	const float screenHalfW = cfg.screen_width_m * 0.5f;
	const float screenHalfH = screenHalfW / cfg.screen_aspect;
	const float screenBottom = cfg.screen_height_offset_m - screenHalfH;
	const float halfW = std::max(screenHalfW + 2.5f, 5.0f);   // hall half width
	const float screenWallZ = -(cfg.screen_distance_m + 1.0f); // wall behind the picture
	const float rearZ = 8.0f;
	const float ceilingY = std::max(cfg.screen_height_offset_m + screenHalfH + 1.6f, rowFloor + 5.0f);
	const float stageFloorY = std::min(rowFloor - 0.6f, screenBottom - 1.2f); // pit floor in front
	const int rowsBehind = 4;
	const float rowPitch = 1.15f, rowRise = 0.32f, seatPitch = 0.62f;

	// Lights: sconces on both side walls every 2.5 m, a dim centre house light, aisle
	// strips along the row steps. Evaluated per vertex while geometry is added, so add
	// them first.
	std::vector<Vec3> sconces;
	for(float z = screenWallZ + 2.0f; z < rearZ - 0.5f; z += 2.5f)
	{
		sconces.push_back({-halfW + 0.05f, rowFloor + 2.2f, z});
		sconces.push_back({halfW - 0.05f, rowFloor + 2.2f, z});
	}
	for(const Vec3 &s : sconces)
		b.lights.push_back({s, 2.6f});
	b.lights.push_back({{0.0f, ceilingY - 0.3f, 2.0f}, 3.5f});
	for(int r = 0; r <= rowsBehind; ++r)
		b.lights.push_back({{0.0f, rowFloor + r * rowRise + 0.05f, 0.4f + r * rowPitch}, 0.6f});

	// Floor: pit in front of the viewer's row (down to the stage), the viewer's row, and
	// stepped rows behind.
	// Winding: grid(origin, du, dv) faces along cross(du, dv); floors take (z, x) for +y,
	// the ceiling (z, -x) for -y, the left wall (y, z) for +x and the right wall (y, -z).
	const float pitFront = screenWallZ + 0.6f;
	b.grid({-halfW, stageFloorY, pitFront}, {0, 0, -0.6f - pitFront}, {2 * halfW, 0, 0}, 8, 16, p.floor);
	// step from the pit up to the viewer's row
	b.quad({-halfW, stageFloorY, -0.6f}, {halfW, stageFloorY, -0.6f}, {halfW, rowFloor, -0.6f}, {-halfW, rowFloor, -0.6f}, p.trim);
	for(int r = 0; r <= rowsBehind; ++r)
	{
		float y = rowFloor + r * rowRise;
		float z0 = -0.6f + r * rowPitch;
		b.grid({-halfW, y, z0}, {0, 0, rowPitch}, {2 * halfW, 0, 0}, 2, 16, p.floor);
		if(r < rowsBehind)
		{
			float z1 = z0 + rowPitch;
			// riser, faces the screen (-z), and the aisle strip on it: a 6 cm bar, chunky
			// enough not to shimmer at 1024 px per eye.
			b.quad({halfW, y, z1}, {-halfW, y, z1}, {-halfW, y + rowRise, z1}, {halfW, y + rowRise, z1}, p.trim);
			for(float x = -halfW + 0.3f; x < halfW - 0.3f; x += 2.0f)
				b.box({x, y + 0.02f, z1 - 0.03f}, {x + 0.35f, y + 0.08f, z1 + 0.005f}, p.aisle, 0.6f);
		}
	}
	// back platform behind the last row and the rear wall
	float lastY = rowFloor + rowsBehind * rowRise;
	float lastZ = -0.6f + (rowsBehind + 1) * rowPitch;
	b.grid({-halfW, lastY, lastZ}, {0, 0, rearZ - lastZ}, {2 * halfW, 0, 0}, 4, 16, p.floor);
	b.grid({halfW, lastY, rearZ}, {-2 * halfW, 0, 0}, {0, ceilingY - lastY, 0}, 16, 8, p.wall);

	// Stage front and the wall behind the picture (curtain: vertical stripes of two
	// shades, the cheapest way to read as fabric without a texture).
	b.box({-halfW, stageFloorY, screenWallZ}, {halfW, screenBottom - 0.35f, pitFront}, p.stage);
	const float stripe = 0.45f;
	int k = 0;
	for(float x = -halfW; x < halfW - 1e-4f; x += stripe, ++k)
	{
		float x1 = std::min(x + stripe, halfW);
		Rgb c = (k & 1) ? Rgb{p.wall.r * 0.75f, p.wall.g * 0.75f, p.wall.b * 0.75f} : p.wall;
		b.grid({x, screenBottom - 0.35f, screenWallZ}, {x1 - x, 0, 0}, {0, ceilingY - (screenBottom - 0.35f), 0}, 1, 10, c);
	}
	// Side walls (normals point inward), tessellated 0.5 m for the sconce and screen light.
	int nz = int((rearZ - screenWallZ) / 0.5f);
	int ny = int((ceilingY - stageFloorY) / 0.5f);
	b.grid({-halfW, stageFloorY, screenWallZ}, {0, ceilingY - stageFloorY, 0}, {0, 0, rearZ - screenWallZ}, ny, nz, p.wall);
	b.grid({halfW, stageFloorY, rearZ}, {0, ceilingY - stageFloorY, 0}, {0, 0, screenWallZ - rearZ}, ny, nz, p.wall);
	// Ceiling (normal down) with a faint house-light disc.
	b.grid({halfW, ceilingY, screenWallZ}, {0, 0, rearZ - screenWallZ}, {-2 * halfW, 0, 0}, nz, 16, p.ceiling);
	b.box({-0.6f, ceilingY - 0.04f, 1.4f}, {0.6f, ceilingY - 0.01f, 2.6f}, {1.0f, 0.9f, 0.8f}, 0.25f);
	// Sconces: a small warm emissive plate on each wall.
	for(const Vec3 &s : sconces)
	{
		float inward = s.x < 0 ? 1.0f : -1.0f;
		Vec3 lo{std::min(s.x, s.x + inward * 0.06f), s.y - 0.18f, s.z - 0.08f};
		Vec3 hi{std::max(s.x, s.x + inward * 0.06f), s.y + 0.18f, s.z + 0.08f};
		b.box(lo, hi, p.sconce, 0.9f);
	}
	// Seats: the viewer's row (an empty seat at the origin: its cushion and armrests are
	// below the eye line and give the parallax cue of sitting), then rows behind.
	uint32_t salt = 1;
	for(int r = 0; r <= rowsBehind; ++r)
	{
		float y = rowFloor + r * rowRise;
		float z = -0.35f + r * rowPitch;
		int count = int((halfW - 0.6f) / seatPitch);
		for(int i = -count; i <= count; ++i)
			seat(b, {i * seatPitch, y, z}, p, salt++);
	}
	// A dark bezel behind the picture so the video edge lands on a matte frame, not on
	// a lit curtain (Skybox's screens have the same black border).
	(void)screenHalfH;
}

// Terrace: a dark stone floor to the horizon. The sky and stars are separate passes.
void build_terrace(RoomBuilder &b, const PleikkariVrEnvironmentConfig &cfg)
{
	const float floorY = -1.2f;
	const Rgb stone{0.30f, 0.30f, 0.32f};
	const Rgb edge{0.18f, 0.18f, 0.20f};
	// A pair of low path lights at the sides so the floor is not pitch black off-screen.
	b.lights.push_back({{-3.0f, floorY + 0.3f, 1.5f}, 1.2f});
	b.lights.push_back({{3.0f, floorY + 0.3f, 1.5f}, 1.2f});
	const float extent = 14.0f;
	b.grid({-extent, floorY, -extent}, {0, 0, 2 * extent}, {2 * extent, 0, 0}, 28, 28, stone);
	// Parapet at the far edge behind the picture, low enough to sit under the screen.
	float parapetTop = std::min(floorY + 0.9f, cfg.screen_height_offset_m - cfg.screen_width_m / cfg.screen_aspect * 0.5f - 0.2f);
	if(parapetTop > floorY + 0.2f)
		b.box({-extent, floorY, -extent - 0.3f}, {extent, parapetTop, -extent}, edge);
	// Path lights as small warm posts.
	b.box({-3.08f, floorY, 1.42f}, {-2.92f, floorY + 0.45f, 1.58f}, {1.0f, 0.8f, 0.55f}, 0.5f);
	b.box({2.92f, floorY, 1.42f}, {3.08f, floorY + 0.45f, 1.58f}, {1.0f, 0.8f, 0.55f}, 0.5f);
}

// The bezel is room geometry that follows the screen: a matte frame 3% of the width
// around the picture, 2 cm behind it.
void build_bezel(RoomBuilder &b, const PleikkariVrEnvironmentConfig &cfg)
{
	std::vector<ScreenVertex> inner, outer;
	build_screen(cfg, inner, 1.0f, 0.02f, nullptr);
	build_screen(cfg, outer, 1.06f, 0.02f, nullptr);
	const Rgb bezel{0.02f, 0.02f, 0.02f};
	auto v = [](const ScreenVertex &s) { return Vec3{s.px, s.py, s.pz}; };
	// Fill the whole outer strip behind the picture (drawn before the screen; the screen
	// then covers the centre with depth testing).
	for(size_t i = 0; i + 3 < outer.size(); i += 2)
		b.quad(v(outer[i]), v(outer[i + 2]), v(outer[i + 3]), v(outer[i + 1]), bezel);
	(void)inner;
}

std::vector<float> build_dome(int slices, int stacks, float radius)
{
	std::vector<float> v;
	for(int j = 0; j < stacks; ++j)
	{
		float t0 = -kPi * 0.5f + kPi * j / stacks, t1 = -kPi * 0.5f + kPi * (j + 1) / stacks;
		for(int i = 0; i < slices; ++i)
		{
			float p0 = 2 * kPi * i / slices, p1 = 2 * kPi * (i + 1) / slices;
			auto pt = [&](float t, float p) {
				return Vec3{radius * std::cos(t) * std::sin(p), radius * std::sin(t), radius * std::cos(t) * std::cos(p)};
			};
			Vec3 a = pt(t0, p0), b = pt(t0, p1), c = pt(t1, p1), d = pt(t1, p0);
			// inside-facing winding (seen from the centre)
			for(Vec3 q : {a, c, b, a, d, c})
				v.insert(v.end(), {q.x, q.y, q.z});
		}
	}
	return v;
}

std::vector<float> build_stars(int count, float radius)
{
	std::vector<float> v;
	for(int i = 0; i < count; ++i)
	{
		float u = hash01(i * 3 + 11), w = hash01(i * 3 + 12), s = hash01(i * 3 + 13);
		float elevation = std::asin(0.02f + 0.98f * u);      // above the horizon
		float azimuth = 2 * kPi * w;
		Vec3 p{radius * std::cos(elevation) * std::sin(azimuth), radius * std::sin(elevation), radius * std::cos(elevation) * std::cos(azimuth)};
		float size = 1.5f + 2.5f * s * s;
		v.insert(v.end(), {p.x, p.y, p.z, size});
	}
	return v;
}

} // namespace

// ---------------------------------------------------------------- the object
struct PleikkariVrEnvironment
{
	PleikkariVrEnvironmentConfig config{};
	GLenum videoTarget = GL_TEXTURE_2D;

	GLuint roomProgram = 0, skyProgram = 0, starProgram = 0, haloProgram = 0, screenProgram = 0, glowProgram = 0;
	GLuint roomVao = 0, roomVbo = 0, roomIbo = 0;
	GLuint skyVao = 0, skyVbo = 0;
	GLuint starVao = 0, starVbo = 0;
	GLuint haloVao = 0, haloVbo = 0;
	GLuint screenVao = 0, screenVbo = 0;
	GLuint glowVao = 0;
	GLuint glowTexture = 0, glowFbo = 0;
	GLuint depthRenderbuffer = 0;
	int depthWidth = 0, depthHeight = 0;

	GLsizei roomIndexCount = 0, skyVertexCount = 0, starCount = 0, screenVertexCount = 0;
	size_t roomVertexBytes = 0;
	bool glowPrimed = false;
	bool frameOpen = false;
	int eyeInFrame = 0;

	// Uniform locations
	struct {
		GLint viewProj, screenCenter, screenRight, screenUp, screenNormal, glow, glowStrength, roomLight, ambient;
	} room{};
	struct {
		GLint viewProj, zenith, horizon, ground, glowAverage, glowStrength, horizonWidth;
	} sky{};
	struct {
		GLint viewProj, brightness;
	} star{};
	struct {
		GLint viewProj, glow, strength;
	} halo{};
	struct {
		GLint viewProj, texTransform, video, hasVideo, idle;
	} screen{};
	struct {
		GLint video, texTransform;
	} glowU{};

	// Screen rectangle for the lighting model (flat approximation of the arc).
	Vec3 screenCenter{}, screenRight{}, screenUp{}, screenNormal{};

	// Timer queries
	bool timerAvailable = false;
	bool timerActive = false;
	GLuint timerQueries[kTimerRing] = {};
	int timerIndex = 0;
	int timerFrames = 0;
	PleikkariVrEnvironmentStats stats{};
	uint32_t frameDrawCalls = 0, frameTriangles = 0;

	PFNGLGENQUERIESEXTPROC pGenQueries = nullptr;
	PFNGLDELETEQUERIESEXTPROC pDeleteQueries = nullptr;
	PFNGLBEGINQUERYEXTPROC pBeginQuery = nullptr;
	PFNGLENDQUERYEXTPROC pEndQuery = nullptr;
	PFNGLGETQUERYOBJECTUIVEXTPROC pGetQueryObjectuiv = nullptr;
	PFNGLGETQUERYOBJECTUI64VEXTPROC pGetQueryObjectui64v = nullptr;
	PFNGLGETINTEGER64VPROC pGetInteger64v = nullptr;

	bool init();
	void rebuild_geometry();
	void draw_call(GLsizei triangles) { ++frameDrawCalls; frameTriangles += triangles; }
	void timer_begin();
	void timer_end();
	void timer_collect();
	void ensure_depth(GLint width, GLint height);
};

namespace {

bool has_extension(const char *name)
{
	const char *ext = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
	return ext && strstr(ext, name);
}

} // namespace

bool PleikkariVrEnvironment::init()
{
	const bool external = videoTarget == GL_TEXTURE_EXTERNAL_OES;
	roomProgram = link(kRoomVertex, kRoomFragment);
	skyProgram = link(kSkyVertex, kSkyFragment);
	starProgram = link(kStarVertex, kStarFragment);
	haloProgram = link(kHaloVertex, kHaloFragment);
	screenProgram = link(kScreenVertex, external ? kScreenFragmentExternal : kScreenFragment2D);
	glowProgram = link(kGlowVertex, external ? kGlowFragmentExternal : kGlowFragment2D);
	if(!roomProgram || !skyProgram || !starProgram || !haloProgram || !screenProgram || !glowProgram)
		return false;

	room.viewProj = glGetUniformLocation(roomProgram, "uViewProj");
	room.screenCenter = glGetUniformLocation(roomProgram, "uScreenCenter");
	room.screenRight = glGetUniformLocation(roomProgram, "uScreenRight");
	room.screenUp = glGetUniformLocation(roomProgram, "uScreenUp");
	room.screenNormal = glGetUniformLocation(roomProgram, "uScreenNormal");
	room.glow = glGetUniformLocation(roomProgram, "uGlow");
	room.glowStrength = glGetUniformLocation(roomProgram, "uGlowStrength");
	room.roomLight = glGetUniformLocation(roomProgram, "uRoomLight");
	room.ambient = glGetUniformLocation(roomProgram, "uAmbient");
	sky.viewProj = glGetUniformLocation(skyProgram, "uViewProj");
	sky.zenith = glGetUniformLocation(skyProgram, "uZenith");
	sky.horizon = glGetUniformLocation(skyProgram, "uHorizon");
	sky.ground = glGetUniformLocation(skyProgram, "uGround");
	sky.glowAverage = glGetUniformLocation(skyProgram, "uGlowAverage");
	sky.glowStrength = glGetUniformLocation(skyProgram, "uGlowStrength");
	sky.horizonWidth = glGetUniformLocation(skyProgram, "uHorizonWidth");
	star.viewProj = glGetUniformLocation(starProgram, "uViewProj");
	star.brightness = glGetUniformLocation(starProgram, "uBrightness");
	halo.viewProj = glGetUniformLocation(haloProgram, "uViewProj");
	halo.glow = glGetUniformLocation(haloProgram, "uGlow");
	halo.strength = glGetUniformLocation(haloProgram, "uStrength");
	screen.viewProj = glGetUniformLocation(screenProgram, "uViewProj");
	screen.texTransform = glGetUniformLocation(screenProgram, "uTexTransform");
	screen.video = glGetUniformLocation(screenProgram, "uVideo");
	screen.hasVideo = glGetUniformLocation(screenProgram, "uHasVideo");
	screen.idle = glGetUniformLocation(screenProgram, "uIdle");
	glowU.video = glGetUniformLocation(glowProgram, "uVideo");
	glowU.texTransform = glGetUniformLocation(glowProgram, "uTexTransform");

	// Glow map: 8x8 RGBA8 with mips (LOD 3 is the frame average), linear, clamped.
	glGenTextures(1, &glowTexture);
	glBindTexture(GL_TEXTURE_2D, glowTexture);
	glTexStorage2D(GL_TEXTURE_2D, 4, GL_RGBA8, kGlowSize, kGlowSize);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	{
		// Start dark grey rather than black, like Skybox's 0.15 floor: a black room
		// before the first frame reads as "broken".
		std::vector<uint8_t> grey(kGlowSize * kGlowSize * 4, 40);
		for(size_t i = 3; i < grey.size(); i += 4) grey[i] = 255;
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGlowSize, kGlowSize, GL_RGBA, GL_UNSIGNED_BYTE, grey.data());
		glGenerateMipmap(GL_TEXTURE_2D);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	GLint previousFbo = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFbo);
	glGenFramebuffers(1, &glowFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, glowFbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glowTexture, 0);
	const bool glowComplete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFbo);
	if(!glowComplete)
	{
		LOGE("glow framebuffer incomplete");
		return false;
	}
	glGenVertexArrays(1, &glowVao);

	glGenVertexArrays(1, &roomVao);
	glGenBuffers(1, &roomVbo);
	glGenBuffers(1, &roomIbo);
	glGenVertexArrays(1, &skyVao);
	glGenBuffers(1, &skyVbo);
	glGenVertexArrays(1, &starVao);
	glGenBuffers(1, &starVbo);
	glGenVertexArrays(1, &haloVao);
	glGenBuffers(1, &haloVbo);
	glGenVertexArrays(1, &screenVao);
	glGenBuffers(1, &screenVbo);

	// Static dome and stars.
	{
		std::vector<float> dome = build_dome(32, 12, 60.0f);
		skyVertexCount = static_cast<GLsizei>(dome.size() / 3);
		glBindVertexArray(skyVao);
		glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
		glBufferData(GL_ARRAY_BUFFER, dome.size() * sizeof(float), dome.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
		std::vector<float> stars = build_stars(900, 59.0f);
		starCount = static_cast<GLsizei>(stars.size() / 4);
		glBindVertexArray(starVao);
		glBindBuffer(GL_ARRAY_BUFFER, starVbo);
		glBufferData(GL_ARRAY_BUFFER, stars.size() * sizeof(float), stars.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
		glBindVertexArray(0);
	}

	// Timer queries, when the driver offers them (Adreno and Mesa both do).
	if(has_extension("GL_EXT_disjoint_timer_query"))
	{
		pGenQueries = reinterpret_cast<PFNGLGENQUERIESEXTPROC>(eglGetProcAddress("glGenQueriesEXT"));
		pDeleteQueries = reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(eglGetProcAddress("glDeleteQueriesEXT"));
		pBeginQuery = reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(eglGetProcAddress("glBeginQueryEXT"));
		pEndQuery = reinterpret_cast<PFNGLENDQUERYEXTPROC>(eglGetProcAddress("glEndQueryEXT"));
		pGetQueryObjectuiv = reinterpret_cast<PFNGLGETQUERYOBJECTUIVEXTPROC>(eglGetProcAddress("glGetQueryObjectuivEXT"));
		pGetQueryObjectui64v = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
		pGetInteger64v = reinterpret_cast<PFNGLGETINTEGER64VPROC>(eglGetProcAddress("glGetInteger64v"));
		timerAvailable = pGenQueries && pDeleteQueries && pBeginQuery && pEndQuery && pGetQueryObjectuiv && pGetQueryObjectui64v;
		if(timerAvailable)
			pGenQueries(kTimerRing, timerQueries);
	}

	rebuild_geometry();
	GLenum err = glGetError();
	if(err != GL_NO_ERROR)
		LOGE("GL error 0x%x after init", err);
	return err == GL_NO_ERROR;
}

void PleikkariVrEnvironment::rebuild_geometry()
{
	RoomBuilder b;
	switch(config.environment)
	{
		case PLEIKKARI_VR_ENVIRONMENT_CINEMA:
			build_cinema(b, config);
			build_bezel(b, config);
			break;
		case PLEIKKARI_VR_ENVIRONMENT_TERRACE:
			build_terrace(b, config);
			build_bezel(b, config);
			break;
		case PLEIKKARI_VR_ENVIRONMENT_VOID:
			build_bezel(b, config);
			break;
		default:
			break;
	}
	if(b.vertices.size() > 65535)
		LOGE("room mesh has %zu vertices, more than 16-bit indices allow", b.vertices.size());
	roomIndexCount = static_cast<GLsizei>(b.indices.size());
	roomVertexBytes = b.vertices.size() * sizeof(RoomVertex);
	glBindVertexArray(roomVao);
	glBindBuffer(GL_ARRAY_BUFFER, roomVbo);
	glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(roomVertexBytes), b.vertices.empty() ? nullptr : b.vertices.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, roomIbo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(b.indices.size() * sizeof(uint16_t)), b.indices.empty() ? nullptr : b.indices.data(), GL_STATIC_DRAW);
	const GLsizei stride = sizeof(RoomVertex);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(6 * sizeof(float)));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(9 * sizeof(float)));

	// Screen strip.
	std::vector<ScreenVertex> strip;
	std::vector<Vec3> corners;
	build_screen(config, strip, 1.0f, 0.0f, &corners);
	screenVertexCount = static_cast<GLsizei>(strip.size());
	glBindVertexArray(screenVao);
	glBindBuffer(GL_ARRAY_BUFFER, screenVbo);
	glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(strip.size() * sizeof(ScreenVertex)), strip.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ScreenVertex), reinterpret_cast<void *>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenVertex), reinterpret_cast<void *>(3 * sizeof(float)));

	// Halo quad: 1.6x the picture, 5 cm behind its plane.
	{
		const float hw = config.screen_width_m * 0.8f;
		const float hh = hw / config.screen_aspect;
		const float cy = config.screen_height_offset_m;
		const float cz = -config.screen_distance_m - 0.05f;
		const float quad[] = {
			-hw, cy - hh, cz, 0, 0,  hw, cy - hh, cz, 1, 0,  -hw, cy + hh, cz, 0, 1,
			 hw, cy - hh, cz, 1, 0,  hw, cy + hh, cz, 1, 1,  -hw, cy + hh, cz, 0, 1,
		};
		glBindVertexArray(haloVao);
		glBindBuffer(GL_ARRAY_BUFFER, haloVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
	}
	glBindVertexArray(0);

	// Flat rectangle through the arc's end points for the lighting model.
	const float halfH = config.screen_width_m / config.screen_aspect * 0.5f;
	screenCenter = {0.0f, config.screen_height_offset_m, -config.screen_distance_m};
	Vec3 leftEdge = corners.empty() ? Vec3{-config.screen_width_m * 0.5f, 0, -config.screen_distance_m} : corners[0];
	Vec3 rightEdge = corners.empty() ? Vec3{config.screen_width_m * 0.5f, 0, -config.screen_distance_m} : corners[1];
	screenRight = {(rightEdge.x - leftEdge.x) * 0.5f, 0.0f, 0.0f};
	screenUp = {0.0f, halfH, 0.0f};
	screenNormal = {0.0f, 0.0f, 1.0f};
}

void PleikkariVrEnvironment::ensure_depth(GLint width, GLint height)
{
	if(depthRenderbuffer && depthWidth == width && depthHeight == height)
		return;
	if(!depthRenderbuffer)
		glGenRenderbuffers(1, &depthRenderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	depthWidth = width;
	depthHeight = height;
}

void PleikkariVrEnvironment::timer_begin()
{
	if(!timerAvailable || timerActive)
		return;
	timer_collect();
	pBeginQuery(GL_TIME_ELAPSED_EXT, timerQueries[timerIndex]);
	timerActive = true;
}

void PleikkariVrEnvironment::timer_end()
{
	if(!timerAvailable || !timerActive)
		return;
	pEndQuery(GL_TIME_ELAPSED_EXT);
	timerActive = false;
	timerIndex = (timerIndex + 1) % kTimerRing;
	if(timerFrames < kTimerRing)
		++timerFrames;
}

void PleikkariVrEnvironment::timer_collect()
{
	if(!timerAvailable || timerFrames < kTimerRing - 1)
		return;
	// The oldest query in the ring is the one about to be reused.
	GLuint q = timerQueries[timerIndex];
	GLuint available = 0;
	pGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &available);
	if(!available)
		return;
	GLint disjoint = 0;
	glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
	GLuint64 ns = 0;
	pGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
	if(!disjoint)
		stats.gpu_ns = ns;
}

// ---------------------------------------------------------------- C API
extern "C" {

void pleikkari_vr_environment_config_default(PleikkariVrEnvironmentConfig *config)
{
	if(!config)
		return;
	config->environment = PLEIKKARI_VR_ENVIRONMENT_PLAIN;
	config->screen_distance_m = 3.0f;
	config->screen_width_m = 3.0f * 1.4f; // 80 degrees of arc at 3 m, PLE-602's strip
	config->screen_curve_radius_m = 3.0f;
	config->screen_height_offset_m = 0.0f;
	config->glow = 0.6f;
	config->room_light = 0.35f;
	config->screen_aspect = 16.0f / 9.0f;
}

void pleikkari_vr_environment_config_clamp(PleikkariVrEnvironmentConfig *config)
{
	if(!config)
		return;
	if(config->environment < 0 || config->environment >= PLEIKKARI_VR_ENVIRONMENT_COUNT)
		config->environment = PLEIKKARI_VR_ENVIRONMENT_PLAIN;
	auto clampf = [](float v, float lo, float hi) { return std::isfinite(v) ? std::min(std::max(v, lo), hi) : lo; };
	config->screen_distance_m = clampf(config->screen_distance_m, kMinDistance, kMaxDistance);
	config->screen_width_m = clampf(config->screen_width_m, kMinWidth, kMaxWidth);
	config->screen_aspect = clampf(config->screen_aspect, 1.0f, 3.0f);
	config->screen_height_offset_m = clampf(config->screen_height_offset_m, -kMaxHeightOffset, kMaxHeightOffset);
	config->glow = clampf(config->glow, 0.0f, 1.0f);
	config->room_light = clampf(config->room_light, 0.0f, 1.0f);
	if(config->screen_curve_radius_m <= 0.0f || !std::isfinite(config->screen_curve_radius_m))
		config->screen_curve_radius_m = 0.0f;
	else
		// The arc may not wrap past a half circle, and a radius below half the width
		// would fold the picture behind itself.
		config->screen_curve_radius_m = std::max(config->screen_curve_radius_m, config->screen_width_m / kPi);
}

PleikkariVrEnvironment *pleikkari_vr_environment_create(const PleikkariVrEnvironmentConfig *config, uint32_t video_target)
{
	if(video_target != GL_TEXTURE_2D && video_target != GL_TEXTURE_EXTERNAL_OES)
	{
		LOGE("unsupported video texture target 0x%x", video_target);
		return nullptr;
	}
	PleikkariVrEnvironment *env = new PleikkariVrEnvironment();
	if(config)
		env->config = *config;
	else
		pleikkari_vr_environment_config_default(&env->config);
	pleikkari_vr_environment_config_clamp(&env->config);
	env->videoTarget = video_target;
	if(!env->init())
	{
		pleikkari_vr_environment_destroy(env);
		return nullptr;
	}
	LOGI("environment %s, screen %.2f m wide at %.2f m, curve radius %.2f m, %d room triangles",
			pleikkari_vr_environment_name(env->config.environment), env->config.screen_width_m,
			env->config.screen_distance_m, env->config.screen_curve_radius_m, env->roomIndexCount / 3);
	return env;
}

void pleikkari_vr_environment_set_config(PleikkariVrEnvironment *env, const PleikkariVrEnvironmentConfig *config)
{
	if(!env || !config)
		return;
	PleikkariVrEnvironmentConfig next = *config;
	pleikkari_vr_environment_config_clamp(&next);
	const bool geometryChanged = memcmp(&next, &env->config, sizeof(next)) != 0 &&
			(next.environment != env->config.environment || next.screen_distance_m != env->config.screen_distance_m ||
			 next.screen_width_m != env->config.screen_width_m || next.screen_curve_radius_m != env->config.screen_curve_radius_m ||
			 next.screen_height_offset_m != env->config.screen_height_offset_m || next.screen_aspect != env->config.screen_aspect);
	env->config = next;
	if(geometryChanged)
		env->rebuild_geometry();
}

void pleikkari_vr_environment_get_config(const PleikkariVrEnvironment *env, PleikkariVrEnvironmentConfig *config)
{
	if(env && config)
		*config = env->config;
}

void pleikkari_vr_environment_begin_frame(PleikkariVrEnvironment *env, uint32_t video_texture, const float video_transform[16],
		int has_video, int new_frame)
{
	if(!env)
		return;
	if(!env->frameOpen)
	{
		env->frameDrawCalls = 0;
		env->frameTriangles = 0;
		env->eyeInFrame = 0;
		env->frameOpen = true;
		env->timer_begin();
	}
	const bool wantsGlow = env->config.environment != PLEIKKARI_VR_ENVIRONMENT_PLAIN && env->config.glow > 0.0f;
	if(!wantsGlow || !has_video || !video_texture)
		return;
	if(!new_frame && env->glowPrimed)
		return;
	GLint previousFbo = 0, previousViewport[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFbo);
	glGetIntegerv(GL_VIEWPORT, previousViewport);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, env->glowFbo);
	glViewport(0, 0, kGlowSize, kGlowSize);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	if(env->glowPrimed)
	{
		glEnable(GL_BLEND);
		glBlendColor(0, 0, 0, kGlowBlend);
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
	}
	else
		glDisable(GL_BLEND);
	glUseProgram(env->glowProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(env->videoTarget, video_texture);
	glUniform1i(env->glowU.video, 0);
	glUniformMatrix4fv(env->glowU.texTransform, 1, GL_FALSE, video_transform ? video_transform : kIdentity);
	glBindVertexArray(env->glowVao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	env->draw_call(1);
	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glBindTexture(env->videoTarget, 0);
	glBindTexture(GL_TEXTURE_2D, env->glowTexture);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFbo);
	glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
	env->glowPrimed = true;
}

void pleikkari_vr_environment_draw_eye(PleikkariVrEnvironment *env, const float view[16], const float projection[16],
		uint32_t video_texture, const float video_transform[16], int has_video)
{
	if(!env || !view || !projection)
		return;
	if(!env->frameOpen)
	{
		// Caller skipped begin_frame: still count and time this frame.
		env->frameDrawCalls = 0;
		env->frameTriangles = 0;
		env->eyeInFrame = 0;
		env->frameOpen = true;
		env->timer_begin();
	}
	float viewProj[16];
	mat4_multiply(viewProj, projection, view);
	const PleikkariVrEnvironmentConfig &cfg = env->config;
	const bool plain = cfg.environment == PLEIKKARI_VR_ENVIRONMENT_PLAIN;

	GLint fbo = 0, viewport[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	if(plain)
	{
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		// Depth for the room. A framebuffer object gets our renderbuffer attached for
		// the duration of the eye; the default framebuffer must bring its own depth.
		if(fbo != 0)
		{
			env->ensure_depth(viewport[0] + viewport[2], viewport[1] + viewport[3]);
			glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, env->depthRenderbuffer);
		}
		glDepthMask(GL_TRUE);
		glClearColor(0, 0, 0, 1);
		glClearDepthf(1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		const bool sky = cfg.environment == PLEIKKARI_VR_ENVIRONMENT_VOID || cfg.environment == PLEIKKARI_VR_ENVIRONMENT_TERRACE;
		if(sky)
		{
			glDisable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);
			glDisable(GL_CULL_FACE);
			glUseProgram(env->skyProgram);
			glUniformMatrix4fv(env->sky.viewProj, 1, GL_FALSE, viewProj);
			glActiveTexture(GL_TEXTURE0);
			const float rl = cfg.room_light;
			if(cfg.environment == PLEIKKARI_VR_ENVIRONMENT_VOID)
			{
				glUniform3f(env->sky.zenith, 0.006f, 0.008f, 0.016f);
				glUniform3f(env->sky.horizon, 0.05f + 0.06f * rl, 0.06f + 0.07f * rl, 0.10f + 0.10f * rl);
				glUniform3f(env->sky.ground, 0.014f, 0.014f, 0.020f);
				glUniform1f(env->sky.horizonWidth, 0.25f);
			}
			else
			{
				glUniform3f(env->sky.zenith, 0.008f, 0.010f, 0.026f);
				glUniform3f(env->sky.horizon, 0.11f + 0.08f * rl, 0.075f + 0.05f * rl, 0.065f + 0.04f * rl);
				glUniform3f(env->sky.ground, 0.016f, 0.016f, 0.016f);
				glUniform1f(env->sky.horizonWidth, 0.12f);
			}
			// Average picture colour from the glow map's top mip, read on the CPU side of
			// the shader as a uniform would need a readback; sample it in the shader instead.
			glUniform3f(env->sky.glowAverage, 1.0f, 1.0f, 1.0f);
			glUniform1f(env->sky.glowStrength, 0.0f);
			glBindVertexArray(env->skyVao);
			glDrawArrays(GL_TRIANGLES, 0, env->skyVertexCount);
			env->draw_call(env->skyVertexCount / 3);
			if(cfg.environment == PLEIKKARI_VR_ENVIRONMENT_TERRACE)
			{
				glUseProgram(env->starProgram);
				glUniformMatrix4fv(env->star.viewProj, 1, GL_FALSE, viewProj);
				glUniform1f(env->star.brightness, 0.6f + 0.4f * cfg.room_light);
				glBindVertexArray(env->starVao);
				glDrawArrays(GL_POINTS, 0, env->starCount);
				env->draw_call(0);
			}
			// Halo behind the picture, additive.
			if(cfg.glow > 0.0f)
			{
				glEnable(GL_BLEND);
				glBlendFunc(GL_ONE, GL_ONE);
				glUseProgram(env->haloProgram);
				glUniformMatrix4fv(env->halo.viewProj, 1, GL_FALSE, viewProj);
				glBindTexture(GL_TEXTURE_2D, env->glowTexture);
				glUniform1i(env->halo.glow, 0);
				glUniform1f(env->halo.strength, 0.55f * cfg.glow);
				glBindVertexArray(env->haloVao);
				glDrawArrays(GL_TRIANGLES, 0, 6);
				env->draw_call(2);
				glDisable(GL_BLEND);
			}
			glDepthMask(GL_TRUE);
		}

		if(env->roomIndexCount > 0)
		{
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LEQUAL);
			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);
			glFrontFace(GL_CCW);
			glUseProgram(env->roomProgram);
			glUniformMatrix4fv(env->room.viewProj, 1, GL_FALSE, viewProj);
			glUniform3f(env->room.screenCenter, env->screenCenter.x, env->screenCenter.y, env->screenCenter.z);
			glUniform3f(env->room.screenRight, env->screenRight.x, env->screenRight.y, env->screenRight.z);
			glUniform3f(env->room.screenUp, env->screenUp.x, env->screenUp.y, env->screenUp.z);
			glUniform3f(env->room.screenNormal, env->screenNormal.x, env->screenNormal.y, env->screenNormal.z);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, env->glowTexture);
			glUniform1i(env->room.glow, 0);
			glUniform1f(env->room.glowStrength, has_video ? 1.6f * cfg.glow : 0.0f);
			glUniform1f(env->room.roomLight, cfg.room_light);
			glUniform1f(env->room.ambient, 0.05f + 0.12f * cfg.room_light);
			glBindVertexArray(env->roomVao);
			glDrawElements(GL_TRIANGLES, env->roomIndexCount, GL_UNSIGNED_SHORT, nullptr);
			env->draw_call(env->roomIndexCount / 3);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		glDisable(GL_CULL_FACE);
	}

	// The picture, last, on top of the bezel (depth test on when there is depth).
	glUseProgram(env->screenProgram);
	glUniformMatrix4fv(env->screen.viewProj, 1, GL_FALSE, viewProj);
	glUniformMatrix4fv(env->screen.texTransform, 1, GL_FALSE, video_transform ? video_transform : kIdentity);
	glActiveTexture(GL_TEXTURE0);
	if(has_video && video_texture)
		glBindTexture(env->videoTarget, video_texture);
	glUniform1i(env->screen.video, 0);
	glUniform1i(env->screen.hasVideo, has_video && video_texture ? 1 : 0);
	glUniform3f(env->screen.idle, 0.03f, 0.035f, 0.05f);
	glBindVertexArray(env->screenVao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, env->screenVertexCount);
	env->draw_call(env->screenVertexCount - 2);
	glBindVertexArray(0);
	if(has_video && video_texture)
		glBindTexture(env->videoTarget, 0);

	if(!plain && fbo != 0)
	{
		// Depth is never needed after the eye: tell the tiler not to write it back, then
		// hand the caller's framebuffer back as it came.
		const GLenum attachment = GL_DEPTH_ATTACHMENT;
		glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
		glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	}
	glDisable(GL_DEPTH_TEST);

	if(++env->eyeInFrame >= 2)
	{
		env->timer_end();
		env->frameOpen = false;
		env->stats.draw_calls = env->frameDrawCalls;
		env->stats.triangles = env->frameTriangles;
		env->stats.vertex_bytes = static_cast<uint32_t>(env->roomVertexBytes);
	}
}

void pleikkari_vr_environment_stats(PleikkariVrEnvironment *env, PleikkariVrEnvironmentStats *stats)
{
	if(!env || !stats)
		return;
	env->timer_collect();
	*stats = env->stats;
}

void pleikkari_vr_environment_destroy(PleikkariVrEnvironment *env)
{
	if(!env)
		return;
	if(env->timerAvailable && env->pDeleteQueries)
		env->pDeleteQueries(kTimerRing, env->timerQueries);
	GLuint programs[] = {env->roomProgram, env->skyProgram, env->starProgram, env->haloProgram, env->screenProgram, env->glowProgram};
	for(GLuint p : programs)
		if(p) glDeleteProgram(p);
	GLuint vaos[] = {env->roomVao, env->skyVao, env->starVao, env->haloVao, env->screenVao, env->glowVao};
	glDeleteVertexArrays(6, vaos);
	GLuint buffers[] = {env->roomVbo, env->roomIbo, env->skyVbo, env->starVbo, env->haloVbo, env->screenVbo};
	glDeleteBuffers(6, buffers);
	if(env->glowFbo) glDeleteFramebuffers(1, &env->glowFbo);
	if(env->glowTexture) glDeleteTextures(1, &env->glowTexture);
	if(env->depthRenderbuffer) glDeleteRenderbuffers(1, &env->depthRenderbuffer);
	delete env;
}

const char *pleikkari_vr_environment_name(PleikkariVrEnvironmentKind kind)
{
	switch(kind)
	{
		case PLEIKKARI_VR_ENVIRONMENT_PLAIN: return "plain";
		case PLEIKKARI_VR_ENVIRONMENT_VOID: return "void";
		case PLEIKKARI_VR_ENVIRONMENT_CINEMA: return "cinema";
		case PLEIKKARI_VR_ENVIRONMENT_TERRACE: return "terrace";
		default: return "unknown";
	}
}

} // extern "C"
