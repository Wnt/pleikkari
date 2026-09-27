// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PLE-603: renders every VR environment to PNG on the build host through a surfaceless
// Mesa EGL context, and (with --check) asserts the picture facts a reviewer would look
// for. This is the off-headset proof the ticket allows when the Oculus Mobile SDK and
// PLE-602's activity are not available.
//
//   vr-environment-render --out DIR [--size 1024] [--check] [--env NAME]
//
// Output: DIR/<env>-yaw<deg>.png, a stereo pair side by side (left eye left), plus one
// line per environment with the draw-call and triangle counts.

#include "vr-environment.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;

// ---------------------------------------------------------------- minimal PNG writer
bool write_png(const std::string &path, int width, int height, const std::vector<uint8_t> &rgba)
{
	std::vector<uint8_t> raw;
	raw.reserve((width * 4 + 1) * height);
	// GL rows start at the bottom; PNG rows start at the top.
	for(int y = height - 1; y >= 0; --y)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgba.begin() + size_t(y) * width * 4, rgba.begin() + size_t(y + 1) * width * 4);
	}
	uLongf compressedSize = compressBound(raw.size());
	std::vector<uint8_t> compressed(compressedSize);
	if(compress2(compressed.data(), &compressedSize, raw.data(), raw.size(), 6) != Z_OK)
		return false;
	compressed.resize(compressedSize);

	FILE *f = fopen(path.c_str(), "wb");
	if(!f)
		return false;
	auto be32 = [](uint32_t v, uint8_t out[4]) {
		out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v;
	};
	auto chunk = [&](const char *type, const uint8_t *data, size_t size) {
		uint8_t len[4];
		be32(static_cast<uint32_t>(size), len);
		fwrite(len, 1, 4, f);
		fwrite(type, 1, 4, f);
		if(size) fwrite(data, 1, size, f);
		uLong crc = crc32(0L, reinterpret_cast<const Bytef *>(type), 4);
		if(size) crc = crc32(crc, data, size);
		uint8_t crcBytes[4];
		be32(static_cast<uint32_t>(crc), crcBytes);
		fwrite(crcBytes, 1, 4, f);
	};
	static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	fwrite(signature, 1, 8, f);
	uint8_t ihdr[13];
	be32(width, ihdr);
	be32(height, ihdr + 4);
	ihdr[8] = 8;  // bit depth
	ihdr[9] = 6;  // RGBA
	ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	chunk("IHDR", ihdr, sizeof(ihdr));
	chunk("IDAT", compressed.data(), compressed.size());
	chunk("IEND", nullptr, 0);
	fclose(f);
	return true;
}

// ---------------------------------------------------------------- matrices (column-major)
void identity(float m[16])
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void multiply(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for(int c = 0; c < 4; ++c)
		for(int rw = 0; rw < 4; ++rw)
			r[c * 4 + rw] = a[rw] * b[c * 4] + a[4 + rw] * b[c * 4 + 1] + a[8 + rw] * b[c * 4 + 2] + a[12 + rw] * b[c * 4 + 3];
	memcpy(out, r, sizeof(r));
}

void perspective(float m[16], float fovYDegrees, float aspect, float near, float far)
{
	identity(m);
	float f = 1.0f / std::tan(fovYDegrees * kPi / 360.0f);
	m[0] = f / aspect;
	m[5] = f;
	m[10] = (far + near) / (near - far);
	m[11] = -1.0f;
	m[14] = 2.0f * far * near / (near - far);
	m[15] = 0.0f;
}

// View matrix for a head yawed by `yaw` (radians, positive = looking left) with the eye
// displaced by `eyeX` metres in head space: inverse of R_y(yaw) * T(eyeX).
void eye_view(float m[16], float yaw, float eyeX)
{
	float rot[16], trans[16];
	identity(rot);
	rot[0] = std::cos(-yaw);
	rot[2] = -std::sin(-yaw);
	rot[8] = std::sin(-yaw);
	rot[10] = std::cos(-yaw);
	identity(trans);
	trans[12] = -eyeX;
	multiply(m, trans, rot);
}

// ---------------------------------------------------------------- test picture
// Colour bars with a solid green centre patch and a red left / blue right third, so the
// directional glow on the walls is visible and --check has known colours to look for.
GLuint make_test_texture(int w, int h)
{
	std::vector<uint8_t> px(size_t(w) * h * 4);
	const uint8_t bars[7][3] = {{192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0}, {192, 0, 192}, {192, 0, 0}, {0, 0, 192}};
	for(int y = 0; y < h; ++y)
		for(int x = 0; x < w; ++x)
		{
			uint8_t *p = px.data() + (size_t(y) * w + x) * 4;
			const uint8_t *c = bars[x * 7 / w];
			if(y < h / 4)
			{
				uint8_t g = uint8_t(255 * x / w);
				c = nullptr;
				p[0] = g; p[1] = g; p[2] = g;
			}
			if(c) { p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; }
			if(x < w / 3 && y > h / 3) { p[0] = 210; p[1] = 30; p[2] = 30; }
			if(x > 2 * w / 3 && y > h / 3) { p[0] = 30; p[1] = 40; p[2] = 220; }
			if(std::abs(x - w / 2) < w / 12 && std::abs(y - h / 2) < h / 8) { p[0] = 0; p[1] = 200; p[2] = 0; }
			p[3] = 255;
		}
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

struct Check {
	int failures = 0;
	void expect(bool ok, const char *what)
	{
		if(!ok)
		{
			++failures;
			fprintf(stderr, "CHECK FAIL: %s\n", what);
		}
		else
			printf("check ok: %s\n", what);
	}
};

const uint8_t *pixel(const std::vector<uint8_t> &img, int stride, int x, int y)
{
	return img.data() + (size_t(y) * stride + x) * 4;
}

bool is_black(const uint8_t *p) { return p[0] < 4 && p[1] < 4 && p[2] < 4; }

} // namespace

int main(int argc, char **argv)
{
	std::string outDir = ".";
	int size = 1024;
	bool check = false;
	std::string only;
	for(int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		if(a == "--out" && i + 1 < argc) outDir = argv[++i];
		else if(a == "--size" && i + 1 < argc) size = atoi(argv[++i]);
		else if(a == "--check") check = true;
		else if(a == "--env" && i + 1 < argc) only = argv[++i];
		else
		{
			fprintf(stderr, "usage: %s [--out DIR] [--size N] [--check] [--env NAME]\n", argv[0]);
			return 2;
		}
	}

	// EGL: surfaceless Mesa platform first, the default display as a fallback.
	EGLDisplay display = EGL_NO_DISPLAY;
	auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	if(getPlatformDisplay)
		display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
	if(display == EGL_NO_DISPLAY)
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if(!eglInitialize(display, nullptr, nullptr))
	{
		fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLConfig config;
	EGLint count = 0;
	if(!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0)
	{
		fprintf(stderr, "no GLES3 pbuffer config\n");
		return 1;
	}
	const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
	const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	EGLSurface pbuffer = eglCreatePbufferSurface(display, config, pbufferAttribs);
	if(context == EGL_NO_CONTEXT || pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(display, pbuffer, pbuffer, context))
	{
		fprintf(stderr, "context/pbuffer failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("renderer: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	// Eye framebuffer: colour only, like PLE-602's swapchain; the module attaches its
	// own depth renderbuffer while it draws.
	GLuint colour = 0, fbo = 0;
	glGenTextures(1, &colour);
	glBindTexture(GL_TEXTURE_2D, colour);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, size, size);
	glBindTexture(GL_TEXTURE_2D, 0);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		fprintf(stderr, "eye framebuffer incomplete\n");
		return 1;
	}
	GLuint video = make_test_texture(320, 180);
	float texTransform[16];
	identity(texTransform);
	float projection[16];
	perspective(projection, 90.0f, 1.0f, 0.1f, 200.0f);

	Check checks;
	const float yaws[] = {0.0f, 40.0f};
	std::vector<uint8_t> eyeImage(size_t(size) * size * 4), pair(size_t(size) * 2 * size * 4);
	for(int kind = 0; kind < PLEIKKARI_VR_ENVIRONMENT_COUNT; ++kind)
	{
		const char *name = pleikkari_vr_environment_name(static_cast<PleikkariVrEnvironmentKind>(kind));
		if(!only.empty() && only != name)
			continue;
		PleikkariVrEnvironmentConfig cfg;
		pleikkari_vr_environment_config_default(&cfg);
		cfg.environment = static_cast<PleikkariVrEnvironmentKind>(kind);
		PleikkariVrEnvironment *env = pleikkari_vr_environment_create(&cfg, GL_TEXTURE_2D);
		if(!env)
		{
			fprintf(stderr, "create failed for %s\n", name);
			return 1;
		}
		// PLE-650: PLEIKKARI_SKY_VARIANT renders a debug sky draw (see vr-environment.h).
		if(const char *variant = getenv("PLEIKKARI_SKY_VARIANT"))
			pleikkari_vr_environment_debug_set_sky_variant(env, atoi(variant));
		// Let the glow map's moving average settle, as it would a fraction of a second
		// into a stream.
		for(int i = 0; i < 24; ++i)
			pleikkari_vr_environment_begin_frame(env, video, texTransform, 1, 1);

		for(float yawDeg : yaws)
		{
			for(int eye = 0; eye < 2; ++eye)
			{
				float view[16];
				eye_view(view, yawDeg * kPi / 180.0f, eye == 0 ? -0.032f : 0.032f);
				glBindFramebuffer(GL_FRAMEBUFFER, fbo);
				glViewport(0, 0, size, size);
				if(eye == 0)
					pleikkari_vr_environment_begin_frame(env, video, texTransform, 1, 1);
				pleikkari_vr_environment_draw_eye(env, view, projection, video, texTransform, 1);
				glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, eyeImage.data());
				for(int y = 0; y < size; ++y)
					memcpy(pair.data() + (size_t(y) * size * 2 + eye * size) * 4, eyeImage.data() + size_t(y) * size * 4, size_t(size) * 4);

				if(check && yawDeg == 0.0f && eye == 0)
				{
					// GL rows: y=0 is the bottom. The screen centre is straight ahead.
					const uint8_t *centre = pixel(eyeImage, size, size / 2 + size / 60, size / 2);
					char what[160];
					snprintf(what, sizeof(what), "%s: screen centre shows the green test patch (%d,%d,%d)", name, centre[0], centre[1], centre[2]);
					checks.expect(centre[1] > 120 && centre[0] < 60 && centre[2] < 60, what);
					const uint8_t *corner = pixel(eyeImage, size, 8, size - 8);
					const uint8_t *lowLeft = pixel(eyeImage, size, 8, 8);
					const uint8_t *midLeft = pixel(eyeImage, size, 4, size / 2);
					if(kind == PLEIKKARI_VR_ENVIRONMENT_PLAIN)
					{
						checks.expect(is_black(corner) && is_black(lowLeft) && is_black(midLeft), "plain: surroundings are black");
					}
					else
					{
						snprintf(what, sizeof(what), "%s: surroundings are lit, not black (%d,%d,%d / %d,%d,%d)", name,
								lowLeft[0], lowLeft[1], lowLeft[2], midLeft[0], midLeft[1], midLeft[2]);
						checks.expect(!is_black(lowLeft) || !is_black(midLeft), what);
						snprintf(what, sizeof(what), "%s: surroundings stay darker than the picture", name);
						checks.expect(lowLeft[1] < 120 && midLeft[1] < 120, what);
					}
				}
				if(check && yawDeg != 0.0f && eye == 0 && kind == PLEIKKARI_VR_ENVIRONMENT_CINEMA)
				{
					// Looking 40 degrees left: the left third of the frame is the wall
					// beside the picture, lit warm by the red block on that side.
					const uint8_t *wall = pixel(eyeImage, size, size / 6, size / 2);
					char what[160];
					snprintf(what, sizeof(what), "cinema: left wall picks up the red side of the picture (%d,%d,%d)", wall[0], wall[1], wall[2]);
					checks.expect(!is_black(wall) && wall[0] >= wall[2], what);
				}
			}
			GLenum err = glGetError();
			char what[128];
			snprintf(what, sizeof(what), "%s yaw %.0f: no GL error (0x%x)", name, yawDeg, err);
			if(check)
				checks.expect(err == GL_NO_ERROR, what);
			char file[512];
			snprintf(file, sizeof(file), "%s/%s-yaw%.0f.png", outDir.c_str(), name, yawDeg);
			if(!write_png(file, size * 2, size, pair))
			{
				fprintf(stderr, "cannot write %s\n", file);
				return 1;
			}
			printf("wrote %s\n", file);
		}
		glFinish();
		PleikkariVrEnvironmentStats stats{};
		pleikkari_vr_environment_stats(env, &stats);
		printf("stats %s: draw calls %u, triangles %u, room vertex bytes %u, gpu %.3f ms (llvmpipe, not a device number)\n",
				name, stats.draw_calls, stats.triangles, stats.vertex_bytes, stats.gpu_ns / 1e6);
		pleikkari_vr_environment_destroy(env);
	}
	if(check)
	{
		printf("%s: %d check failure(s)\n", checks.failures ? "FAIL" : "PASS", checks.failures);
		return checks.failures ? 1 : 0;
	}
	return 0;
}
