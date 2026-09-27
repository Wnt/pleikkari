// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PLE-603: JNI binding for fi.madekivi.pleikkari.stream.VrEnvironmentNative. The VrApi
// activity (PLE-602) calls the C API from its own native code; this binding exists for
// the debug preview activity and for tests, which drive the renderer from a GLSurfaceView.

#include "vr-environment.h"

#include <jni.h>

#define JNI_METHOD(name) Java_fi_madekivi_pleikkari_stream_VrEnvironmentNative_##name

namespace {

PleikkariVrEnvironment *env_of(jlong handle) { return reinterpret_cast<PleikkariVrEnvironment *>(handle); }

PleikkariVrEnvironmentConfig config_from(jint environment, jfloat distance, jfloat width, jfloat radius,
		jfloat heightOffset, jfloat glow, jfloat roomLight)
{
	PleikkariVrEnvironmentConfig cfg;
	pleikkari_vr_environment_config_default(&cfg);
	cfg.environment = static_cast<PleikkariVrEnvironmentKind>(environment);
	cfg.screen_distance_m = distance;
	cfg.screen_width_m = width;
	cfg.screen_curve_radius_m = radius;
	cfg.screen_height_offset_m = heightOffset;
	cfg.glow = glow;
	cfg.room_light = roomLight;
	pleikkari_vr_environment_config_clamp(&cfg);
	return cfg;
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL JNI_METHOD(create)(JNIEnv *, jclass, jint environment, jfloat distance, jfloat width,
		jfloat radius, jfloat heightOffset, jfloat glow, jfloat roomLight, jint videoTarget)
{
	PleikkariVrEnvironmentConfig cfg = config_from(environment, distance, width, radius, heightOffset, glow, roomLight);
	return reinterpret_cast<jlong>(pleikkari_vr_environment_create(&cfg, static_cast<uint32_t>(videoTarget)));
}

JNIEXPORT void JNICALL JNI_METHOD(setConfig)(JNIEnv *, jclass, jlong handle, jint environment, jfloat distance,
		jfloat width, jfloat radius, jfloat heightOffset, jfloat glow, jfloat roomLight)
{
	PleikkariVrEnvironmentConfig cfg = config_from(environment, distance, width, radius, heightOffset, glow, roomLight);
	pleikkari_vr_environment_set_config(env_of(handle), &cfg);
}

JNIEXPORT void JNICALL JNI_METHOD(beginFrame)(JNIEnv *jni, jclass, jlong handle, jint texture, jfloatArray transform,
		jboolean hasVideo, jboolean newFrame)
{
	float matrix[16];
	jni->GetFloatArrayRegion(transform, 0, 16, matrix);
	if(jni->ExceptionCheck())
		return;
	pleikkari_vr_environment_begin_frame(env_of(handle), static_cast<uint32_t>(texture), matrix, hasVideo ? 1 : 0, newFrame ? 1 : 0);
}

JNIEXPORT void JNICALL JNI_METHOD(drawEye)(JNIEnv *jni, jclass, jlong handle, jfloatArray view, jfloatArray projection,
		jint texture, jfloatArray transform, jboolean hasVideo)
{
	float v[16], p[16], t[16];
	jni->GetFloatArrayRegion(view, 0, 16, v);
	jni->GetFloatArrayRegion(projection, 0, 16, p);
	jni->GetFloatArrayRegion(transform, 0, 16, t);
	if(jni->ExceptionCheck())
		return;
	pleikkari_vr_environment_draw_eye(env_of(handle), v, p, static_cast<uint32_t>(texture), t, hasVideo ? 1 : 0);
}

// PLE-650: debug preview only.
JNIEXPORT void JNICALL JNI_METHOD(debugSetSkyVariant)(JNIEnv *, jclass, jlong handle, jint variant)
{
	pleikkari_vr_environment_debug_set_sky_variant(env_of(handle), variant);
}

// out[0] = gpu ns, out[1] = draw calls, out[2] = triangles, out[3] = room vertex bytes.
JNIEXPORT void JNICALL JNI_METHOD(stats)(JNIEnv *jni, jclass, jlong handle, jlongArray out)
{
	PleikkariVrEnvironmentStats s{};
	pleikkari_vr_environment_stats(env_of(handle), &s);
	jlong values[4] = {static_cast<jlong>(s.gpu_ns), s.draw_calls, s.triangles, s.vertex_bytes};
	jni->SetLongArrayRegion(out, 0, 4, values);
}

JNIEXPORT void JNICALL JNI_METHOD(destroy)(JNIEnv *, jclass, jlong handle)
{
	pleikkari_vr_environment_destroy(env_of(handle));
}

} // extern "C"
