// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#define _GNU_SOURCE

#include <jni.h>

#include <android/log.h>
#include <android/api-level.h>

#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/session.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/regist.h>

#include <string.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <arpa/inet.h>

#include "video-decoder.h"
#include "video-frame-latency.h"
#include "audio-decoder.h"
#include "audio-output.h"
#include "log.h"
#include "chiaki-jni.h"

static char *strdup_jni(const char *str)
{
	if(!str)
		return NULL;
	char *r = strdup(str);
	if(!r)
		return NULL;
	for(char *c=r; *c; c++)
	{
		if(*c & (1 << 7))
			*c = '?';
	}
	return r;
}

jobject jnistr_from_ascii(JNIEnv *env, const char *str)
{
	if(!str)
		return NULL;
	char *s = strdup_jni(str);
	if(!s)
		return NULL;
	jobject r = E->NewStringUTF(env, s);
	free(s);
	return r;
}

static jbyteArray jnibytearray_create(JNIEnv *env, const uint8_t *buf, size_t buf_size)
{
	jbyteArray r = E->NewByteArray(env, buf_size);
	E->SetByteArrayRegion(env, r, 0, buf_size, (const jbyte *)buf);
	return r;
}

static jobject get_kotlin_global_object(JNIEnv *env, const char *id)
{
	size_t idlen = strlen(id);
	char *sig = malloc(idlen + 3);
	if(!sig)
		return NULL;
	sig[0] = 'L';
	memcpy(sig + 1, id, idlen);
	sig[1 + idlen] = ';';
	sig[1 + idlen + 1] = '\0';
	jclass cls = E->FindClass(env, id);
	jfieldID field_id = E->GetStaticFieldID(env, cls, "INSTANCE", sig);
	jobject r = E->GetStaticObjectField(env, cls, field_id);
	free(sig);
	return r;
}

static ChiakiLog global_log;
JavaVM *global_vm;

// PLE-698: the Go cinema's per-frame latency. One per process, not per session: the cinema thread
// reports its latches through a static JNI call and can outlive the session it streams (StreamVrActivity
// pauses the session before it joins the cinema thread), so it never holds a session pointer.
static AndroidChiakiVideoFrameLatency cinema_frame_latency;
static bool cinema_frame_latency_ready = false;

static int64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Off by default: current behavior is unchanged until a session enables it via
// ConnectInfo.threadPriorityBoostEnabled (see the "Thread priority boost" setting).
static bool g_thread_priority_boost_enabled = false;

// Registered once at load time with chiaki_thread_set_affinity_cb(); the lib core and the
// Android decoder output and presenter threads call chiaki_thread_set_affinity() themselves.
static void android_chiaki_thread_affinity_cb(ChiakiThreadName name, void *user)
{
	(void)user;
	if(!g_thread_priority_boost_enabled)
		return;

	switch(name)
	{
		case CHIAKI_THREAD_NAME_TAKION:
		case CHIAKI_THREAD_NAME_VIDEO_DECODER:
		case CHIAKI_THREAD_NAME_VIDEO_PRESENTER:
			break;
		default:
			return;
	}

	const int nice_value = -10;
	if(setpriority(PRIO_PROCESS, gettid(), nice_value) != 0)
		CHIAKI_LOGW(&global_log, "Failed to set thread priority for thread name %d to nice %d: %s", (int)name, nice_value, strerror(errno));

	cpu_set_t big_cores;
	CPU_ZERO(&big_cores);
	for(int cpu = 4; cpu <= 7; cpu++)
		CPU_SET(cpu, &big_cores);
	if(sched_setaffinity(0, sizeof(big_cores), &big_cores) != 0)
		CHIAKI_LOGW(&global_log, "Failed to set big-core affinity for thread name %d to CPUs 4-7: %s", (int)name, strerror(errno));
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	global_vm = vm;

	android_chiaki_file_log_init(&global_log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, NULL);
	CHIAKI_LOGI(&global_log, "Loading Chiaki Library");
	ChiakiErrorCode err = chiaki_lib_init();
	CHIAKI_LOGI(&global_log, "Chiaki Library Init Result: %s\n", chiaki_error_string(err));
	chiaki_thread_set_affinity_cb(android_chiaki_thread_affinity_cb, NULL);
	cinema_frame_latency_ready = android_chiaki_video_frame_latency_init(&cinema_frame_latency) == CHIAKI_ERR_SUCCESS;
	return JNI_VERSION;
}

JNIEnv *attach_thread_jni()
{
	JNIEnv *env;
	int r = (*global_vm)->GetEnv(global_vm, (void **)&env, JNI_VERSION);
	if(r == JNI_OK)
		return env;

	if((*global_vm)->AttachCurrentThread(global_vm, &env, NULL) == 0)
		return env;

	CHIAKI_LOGE(&global_log, "Failed to get JNIEnv from JavaVM or attach");
	return NULL;
}

JNIEXPORT jstring JNICALL JNI_FCN(errorCodeToString)(JNIEnv *env, jobject obj, jint value)
{
	return E->NewStringUTF(env, chiaki_error_string((ChiakiErrorCode)value));
}

JNIEXPORT jstring JNICALL JNI_FCN(quitReasonToString)(JNIEnv *env, jobject obj, jint value)
{
	return E->NewStringUTF(env, chiaki_quit_reason_string((ChiakiQuitReason)value));
}

JNIEXPORT jboolean JNICALL JNI_FCN(quitReasonIsError)(JNIEnv *env, jobject obj, jint value)
{
	return chiaki_quit_reason_is_error(value);
}

JNIEXPORT jobject JNICALL JNI_FCN(videoProfilePreset)(JNIEnv *env, jobject obj, jint resolution_preset, jint fps_preset, jobject codec)
{
	ChiakiConnectVideoProfile profile = { 0 };
	chiaki_connect_video_profile_preset(&profile, (ChiakiVideoResolutionPreset)resolution_preset, (ChiakiVideoFPSPreset)fps_preset);
	jclass profile_class = E->FindClass(env, BASE_PACKAGE"/ConnectVideoProfile");
	jmethodID profile_ctor = E->GetMethodID(env, profile_class, "<init>", "(IIIIL"BASE_PACKAGE"/Codec;)V");
	return E->NewObject(env, profile_class, profile_ctor, profile.width, profile.height, profile.max_fps, profile.bitrate, codec);
}

typedef struct android_chiaki_session_t
{
	ChiakiSession session;
	ChiakiLog *log;
	jobject java_session;
	jclass java_session_class;
	// Cached on the Java thread in session_create: the CHIAKI_EVENT_REGIST callback runs on the session's
	// own native thread, where FindClass sees only the system classloader and cannot resolve an app class
	// (PLE-332: "Didn't find class com.metallic.chiaki.lib.Target" aborted the process the first time a
	// PSN session ever reached registration).
	jclass java_target_class;
	jmethodID java_target_from_value_meth;
	jclass java_regist_host_class;
	jmethodID java_regist_host_ctor;
	jmethodID java_session_event_connected_meth;
	jmethodID java_session_event_login_pin_request_meth;
	jmethodID java_session_event_quit_meth;
	jmethodID java_session_event_rumble_meth;
	jmethodID java_session_event_remote_data_socket_needed_meth;
	jmethodID java_session_event_registration_success_meth;
	jmethodID java_session_event_stream_stats_meth;
	jmethodID java_session_performance_hint_thread_started_meth;
	jmethodID java_session_performance_hint_report_meth;
	jmethodID java_session_performance_hint_thread_stopped_meth;
	jmethodID java_session_is_adpf_performance_mode_live_meth;
	jmethodID java_session_is_sustained_performance_mode_live_meth;
	jfieldID java_controller_state_buttons;
	jfieldID java_controller_state_l2_state;
	jfieldID java_controller_state_r2_state;
	jfieldID java_controller_state_left_x;
	jfieldID java_controller_state_left_y;
	jfieldID java_controller_state_right_x;
	jfieldID java_controller_state_right_y;
	jfieldID java_controller_state_touches;
	jfieldID java_controller_state_gyro_x;
	jfieldID java_controller_state_gyro_y;
	jfieldID java_controller_state_gyro_z;
	jfieldID java_controller_state_accel_x;
	jfieldID java_controller_state_accel_y;
	jfieldID java_controller_state_accel_z;
	jfieldID java_controller_state_orient_x;
	jfieldID java_controller_state_orient_y;
	jfieldID java_controller_state_orient_z;
	jfieldID java_controller_state_orient_w;
	jfieldID java_controller_touch_x;
	jfieldID java_controller_touch_y;
	jfieldID java_controller_touch_id;

	AndroidChiakiVideoDecoder video_decoder;
	AndroidChiakiAudioDecoder audio_decoder;
	void *audio_output;
	bool performance_mode_enabled;
	bool stream_stats_log_enabled;
} AndroidChiakiSession;

static void clear_performance_hint_exception(JNIEnv *env, AndroidChiakiSession *session, const char *operation)
{
	if(!E->ExceptionCheck(env))
		return;
	E->ExceptionClear(env);
	CHIAKI_LOGW(session->log, "Performance hint %s callback failed", operation);
}

static void android_chiaki_performance_hint_thread_started(void *user, ChiakiThreadName role)
{
	AndroidChiakiSession *session = user;
	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;
	E->CallVoidMethod(env, session->java_session,
			session->java_session_performance_hint_thread_started_meth, (jint)role, (jint)gettid());
	clear_performance_hint_exception(env, session, "thread-start");
	// Keep this native worker attached so per-frame reports do not pay attach/detach overhead.
}

static void android_chiaki_performance_hint_report(void *user, ChiakiThreadName role,
		uint64_t actual_duration_ns)
{
	AndroidChiakiSession *session = user;
	JNIEnv *env = NULL;
	if((*global_vm)->GetEnv(global_vm, (void **)&env, JNI_VERSION) != JNI_OK)
		return;
	E->CallVoidMethod(env, session->java_session,
			session->java_session_performance_hint_report_meth,
			(jint)role, (jlong)actual_duration_ns);
	clear_performance_hint_exception(env, session, "report");
}

static void android_chiaki_performance_hint_thread_stopped(void *user, ChiakiThreadName role)
{
	AndroidChiakiSession *session = user;
	JNIEnv *env = attach_thread_jni();
	if(env)
	{
		E->CallVoidMethod(env, session->java_session,
				session->java_session_performance_hint_thread_stopped_meth, (jint)role);
		clear_performance_hint_exception(env, session, "thread-stop");
	}
	(*global_vm)->DetachCurrentThread(global_vm);
}

static ChiakiErrorCode android_chiaki_video_decoder_request_idr(void *user)
{
	AndroidChiakiSession *session = user;
	return chiaki_session_request_idr(&session->session);
}

static void android_chiaki_event_cb(ChiakiEvent *event, void *user)
{
	AndroidChiakiSession *session = user;

	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;

	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
		{
			jstring peer_host_java = E->NewStringUTF(env, event->connected.peer_host);
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_connected_meth,
							  (jboolean)event->connected.relay,
							  peer_host_java,
							  (jint)event->connected.peer_port,
							  (jlong)event->connected.mtu_in,
							  (jlong)event->connected.rtt_us,
							  (jboolean)event->connected.measured);
			E->DeleteLocalRef(env, peer_host_java);
			break;
		}
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_login_pin_request_meth,
							  (jboolean)event->login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_QUIT:
		{
			char *reason_str = strdup_jni(event->quit.reason_str);
			jstring reason_str_java = reason_str ? E->NewStringUTF(env, reason_str) : NULL;
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_quit_meth,
							  (jint)event->quit.reason,
							  reason_str_java);
			if(reason_str_java)
				E->DeleteLocalRef(env, reason_str_java);
			free(reason_str);
			break;
		}
		case CHIAKI_EVENT_RUMBLE:
			E->CallVoidMethod(env, session->java_session,
							  session->java_session_event_rumble_meth,
							  (jint)event->rumble.left,
							  (jint)event->rumble.right);
			break;
		case CHIAKI_EVENT_REMOTE_DATA_SOCKET_NEEDED:
			E->CallVoidMethod(env, session->java_session,
					session->java_session_event_remote_data_socket_needed_meth);
			break;
		case CHIAKI_EVENT_REGIST:
		{
			ChiakiRegisteredHost *host = &event->host;
			jclass target_class = session->java_target_class;
			jmethodID target_from_value = session->java_target_from_value_meth;
			jobject target = E->CallStaticObjectMethod(env, target_class, target_from_value, (jint)host->target);
			jclass host_class = session->java_regist_host_class;
			jmethodID host_ctor = session->java_regist_host_ctor;
			jobject java_host = E->NewObject(env, host_class, host_ctor,
					target,
					jnistr_from_ascii(env, host->ap_ssid),
					jnistr_from_ascii(env, host->ap_bssid),
					jnistr_from_ascii(env, host->ap_key),
					jnistr_from_ascii(env, host->ap_name),
					jnibytearray_create(env, host->server_mac, sizeof(host->server_mac)),
					jnistr_from_ascii(env, host->server_nickname),
					jnibytearray_create(env, (const uint8_t *)host->rp_regist_key, sizeof(host->rp_regist_key)),
					(jint)host->rp_key_type,
					jnibytearray_create(env, host->rp_key, sizeof(host->rp_key)));
			E->CallVoidMethod(env, session->java_session,
					session->java_session_event_registration_success_meth, java_host);
			break;
		}
		case CHIAKI_EVENT_STREAM_STATS:
		{
			jboolean adpf_live = false;
			jboolean sustained_live = false;
			if(session->performance_mode_enabled)
			{
				adpf_live = E->CallBooleanMethod(env, session->java_session,
						session->java_session_is_adpf_performance_mode_live_meth);
				clear_performance_hint_exception(env, session, "ADPF status");
				sustained_live = E->CallBooleanMethod(env, session->java_session,
						session->java_session_is_sustained_performance_mode_live_meth);
				clear_performance_hint_exception(env, session, "sustained status");
			}
			char performance_status[96] = "";
			if(session->performance_mode_enabled)
				snprintf(performance_status, sizeof(performance_status),
						" | perf sustained=%s adpf=%s",
						sustained_live ? "live" : "off", adpf_live ? "live" : "off");
			AndroidChiakiVideoDiagnostics diagnostics;
			AndroidChiakiAudioDiagnostics audio;
			android_chiaki_video_decoder_get_diagnostics(&session->video_decoder, &diagnostics);
			android_chiaki_audio_output_get_diagnostics(session->audio_output, &audio);
			if(session->stream_stats_log_enabled)
			{
				uint64_t interval_ms = event->stream_stats.interval_ms;
				uint64_t takion_per_s_milli = interval_ms
					? event->stream_stats.takion_packets_received * 1000000ULL / interval_ms : 0;
				uint64_t feedback_per_s_milli = interval_ms
					? event->stream_stats.feedback_packets * 1000000ULL / interval_ms : 0;
				uint64_t takion_expected_per_s_milli = interval_ms
					? (event->stream_stats.takion_packets_received + event->stream_stats.takion_partial_frame_units_missing) * 1000000ULL / interval_ms : 0;
				uint64_t feedback_gap_mean_milli = event->stream_stats.feedback_gap_count
					? event->stream_stats.feedback_gap_sum_ms * 1000ULL / event->stream_stats.feedback_gap_count : 0;
				CHIAKI_LOGI(session->log,
					"Feedback stats: window %llu ms video received %llu decoded %llu"
					" dropped_input %llu dropped_presenter %llu dropped_bounded_age %llu"
					" recovery_flushes %llu recovery_flushed %llu"
					" lost %llu discarded_for_idr %llu reorder_timeouts %llu"
					" packet_jitter_ms %llu.%03llu packet_jitter_raw_ms %llu.%03llu"
					" | per_s takion %llu.%03llu feedback %llu.%03llu"
					" | takion_raw expected_per_s %llu.%03llu received_per_s %llu.%03llu fec_recovered %llu unrecoverable %llu"
					" | feedback_gap max_ms %llu mean_ms %llu.%03llu over_50_ms %llu"
					" | rtt_ms %llu.%03llu audio_latency_ms %s%llu.%03llu"
					" audio_xruns %s%d audio_underruns %llu"
					" audio_fifo_fill_ms %llu.%03llu audio_fifo_capacity_ms %llu.%03llu"
					" | network target_bps %llu measured_bps %llu console_rtt_raw %.6f"
					" probe_rtt_ms %llu.%03llu probe_samples %llu probe_unacked %llu probe_ambiguous %llu"
					" server_loss %llu congestion_loss measured=%.4f reported=%.4f"
					" | takion_silence_ms %llu window_max_gap_ms %llu"
					" | decode mean_ms %llu.%03llu p95_ms %llu.%03llu%s",
					(unsigned long long)interval_ms,
					(unsigned long long)event->stream_stats.stream_frames,
					(unsigned long long)diagnostics.output_frames,
					(unsigned long long)diagnostics.input_frames_dropped,
					(unsigned long long)diagnostics.presenter_frames_dropped,
					(unsigned long long)diagnostics.presenter_bounded_age_frames_dropped,
					(unsigned long long)diagnostics.presenter_recovery_flushes,
					(unsigned long long)diagnostics.presenter_recovery_flushed_frames,
					(unsigned long long)event->stream_stats.video_frames_lost,
					(unsigned long long)event->stream_stats.video_frames_discarded_for_idr,
					(unsigned long long)event->stream_stats.video_reorder_timeouts,
					(unsigned long long)(event->stream_stats.video_packet_jitter_us / 1000),
					(unsigned long long)(event->stream_stats.video_packet_jitter_us % 1000),
					(unsigned long long)(event->stream_stats.video_packet_jitter_raw_us / 1000),
					(unsigned long long)(event->stream_stats.video_packet_jitter_raw_us % 1000),
					(unsigned long long)(takion_per_s_milli / 1000),
					(unsigned long long)(takion_per_s_milli % 1000),
					(unsigned long long)(feedback_per_s_milli / 1000),
					(unsigned long long)(feedback_per_s_milli % 1000),
					(unsigned long long)(takion_expected_per_s_milli / 1000),
					(unsigned long long)(takion_expected_per_s_milli % 1000),
					(unsigned long long)(takion_per_s_milli / 1000),
					(unsigned long long)(takion_per_s_milli % 1000),
					(unsigned long long)event->stream_stats.fec_recovered_packets,
					(unsigned long long)event->stream_stats.unrecoverable_packets,
					(unsigned long long)event->stream_stats.feedback_gap_max_ms,
					(unsigned long long)(feedback_gap_mean_milli / 1000),
					(unsigned long long)(feedback_gap_mean_milli % 1000),
					(unsigned long long)event->stream_stats.feedback_gaps_over_50_ms,
					(unsigned long long)(event->stream_stats.rtt_us / 1000),
					(unsigned long long)(event->stream_stats.rtt_us % 1000),
					audio.latency_valid ? "" : "unavailable/",
					(unsigned long long)(audio.latency_us / 1000),
					(unsigned long long)(audio.latency_us % 1000),
					audio.xruns_valid ? "" : "unavailable/", audio.xruns,
					(unsigned long long)audio.underruns,
					(unsigned long long)(audio.fifo_fill_us / 1000),
					(unsigned long long)(audio.fifo_fill_us % 1000),
					(unsigned long long)(audio.fifo_capacity_us / 1000),
					(unsigned long long)(audio.fifo_capacity_us % 1000),
					(unsigned long long)event->stream_stats.target_bitrate_bps,
					(unsigned long long)event->stream_stats.measured_throughput_bps,
					event->stream_stats.console_rtt_raw,
					(unsigned long long)(event->stream_stats.probe_rtt_us / 1000),
					(unsigned long long)(event->stream_stats.probe_rtt_us % 1000),
					(unsigned long long)event->stream_stats.probe_rtt_samples,
					(unsigned long long)event->stream_stats.probe_rtt_unacked,
					(unsigned long long)event->stream_stats.probe_rtt_ambiguous,
					(unsigned long long)event->stream_stats.server_loss,
					event->stream_stats.congestion_measured_loss,
					event->stream_stats.congestion_reported_loss,
					(unsigned long long)event->stream_stats.takion_silence_ms,
					(unsigned long long)event->stream_stats.takion_max_receive_gap_ms,
					(unsigned long long)(diagnostics.decode_mean_us / 1000),
					(unsigned long long)(diagnostics.decode_mean_us % 1000),
					(unsigned long long)(diagnostics.decode_p95_us / 1000),
					(unsigned long long)(diagnostics.decode_p95_us % 1000), performance_status);
				// PLE-698: the Go cinema's per-frame latency over the same window; only while a cinema records.
				AndroidChiakiVideoFrameLatencyWindow latency_window;
				if(cinema_frame_latency_ready
						&& android_chiaki_video_frame_latency_take_window(&cinema_frame_latency, monotonic_ns(), &latency_window)
						&& (latency_window.decoded || latency_window.latched))
				{
					char latency_line[512];
					android_chiaki_video_frame_latency_format(&latency_window, latency_line, sizeof(latency_line));
					CHIAKI_LOGI(session->log, "%s", latency_line);
				}
			}
			E->CallVoidMethod(env, session->java_session,
					session->java_session_event_stream_stats_meth,
					(jlong)event->stream_stats.interval_ms,
					(jlong)event->stream_stats.rtt_us,
					(jlong)event->stream_stats.stream_frames,
					(jlong)diagnostics.output_frames,
					(jlong)diagnostics.decode_mean_us,
					(jlong)diagnostics.decode_p95_us,
					(jlong)diagnostics.input_frames_dropped,
					(jlong)diagnostics.presenter_frames_dropped,
					(jlong)diagnostics.missed_vsyncs,
					(jlong)event->stream_stats.video_frames_lost,
					(jlong)event->stream_stats.video_reorder_timeouts,
					(jlong)event->stream_stats.video_packet_jitter_us,
					(jlong)event->stream_stats.takion_packets_received,
					(jlong)event->stream_stats.takion_partial_frame_units_missing,
					(jlong)event->stream_stats.feedback_packets,
					(jlong)diagnostics.dejitter_buffer_ns,
					(jlong)diagnostics.cadence_depth_ns,
					(jlong)diagnostics.cadence_target_ns,
					(jlong)diagnostics.cadence_err_p50_ns,
					(jlong)diagnostics.cadence_err_p99_ns,
					(jlong)diagnostics.decode_ewma_ns,
					(jlong)diagnostics.cadence_window_dropped_frames,
					(jlong)diagnostics.vsync_period_ns,
					(jlong)diagnostics.presenter_queue_depth,
					(jlong)audio.latency_us,
					(jlong)audio.xruns,
					(jlong)audio.underruns,
					(jboolean)event->stream_stats.connection_quality_valid,
					(jlong)event->stream_stats.target_bitrate_bps,
					(jlong)event->stream_stats.measured_throughput_bps,
					(jlong)event->stream_stats.console_rtt_us,
					(jlong)event->stream_stats.server_loss,
					(jdouble)event->stream_stats.congestion_measured_loss,
					(jdouble)event->stream_stats.congestion_reported_loss,
					(jboolean)diagnostics.cadence_half_rate_detected,
					(jdouble)event->stream_stats.console_rtt_raw,
					(jlong)event->stream_stats.probe_rtt_us,
					(jlong)event->stream_stats.probe_rtt_samples,
					(jlong)event->stream_stats.probe_rtt_unacked,
					(jlong)event->stream_stats.probe_rtt_ambiguous,
					(jboolean)event->stream_stats.video_packet_jitter_filled,
					(jlong)event->stream_stats.takion_silence_ms,
					(jlong)event->stream_stats.takion_max_receive_gap_ms,
					(jlong)event->stream_stats.video_frames_discarded_for_idr);
			break;
		}
		default:
			break;
	}

	(*global_vm)->DetachCurrentThread(global_vm);
}

static void session_create(JNIEnv *env, jobject result, jobject connect_info_obj, jstring log_file_str,
		jboolean log_verbose, jboolean real_video_timestamps, jboolean decoder_input_thread,
		jint remote_ctrl_fd, jbyteArray psn_account_id_array, jstring selected_addr_string,
		jint ctrl_port, jbyteArray data1_array, jbyteArray data2_array,
		jbyteArray custom_data1_array, jstring local_addr_string, jobject java_session)
{
	AndroidChiakiSession *session = NULL;
	ChiakiLog *log = malloc(sizeof(ChiakiLog));
	const char *log_file = log_file_str ? E->GetStringUTFChars(env, log_file_str, NULL) : NULL;
	android_chiaki_file_log_init(log, log_verbose ? CHIAKI_LOG_ALL : (CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE), log_file);
	if(log_file)
		E->ReleaseStringUTFChars(env, log_file_str, log_file);

	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	char *host_str = NULL;

	jclass result_class = E->GetObjectClass(env, result);

	jclass connect_info_class = E->GetObjectClass(env, connect_info_obj);
	jboolean ps5 = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "ps5", "Z"));
	jboolean decoder_low_latency = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "decoderLowLatencyEnabled", "Z"));
	jboolean thread_priority_boost = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "threadPriorityBoostEnabled", "Z"));
	g_thread_priority_boost_enabled = thread_priority_boost;
	jboolean performance_mode = E->GetBooleanField(env, connect_info_obj,
			E->GetFieldID(env, connect_info_class, "performanceModeEnabled", "Z"));
	jboolean decoder_late_frame_recovery = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "decoderLateFrameRecoveryEnabled", "Z"));
	jint decoder_operating_rate = E->GetIntField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "decoderOperatingRate", "I"));
	jboolean decoder_operating_rate_default = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "decoderOperatingRateDefault", "Z"));
	jboolean decoder_operating_rate_auto = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "decoderOperatingRateAuto", "Z"));
	jboolean decoder_realtime_priority = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "decoderRealtimePriority", "Z"));
	jboolean decoder_qcom_vt_low_latency = E->GetBooleanField(env, connect_info_obj,
			E->GetFieldID(env, connect_info_class, "decoderQcomVtLowLatency", "Z"));
	jint video_timestamp_rate_hz = E->GetIntField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "videoTimestampRateHz", "I"));
	jdouble packet_loss_max = E->GetDoubleField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "packetLossMax", "D"));
	jboolean adaptive_loss_report = E->GetBooleanField(env, connect_info_obj,
			E->GetFieldID(env, connect_info_class, "adaptiveLossReport", "Z"));
	jboolean disable_video_packet_reordering = E->GetBooleanField(env, connect_info_obj,
			E->GetFieldID(env, connect_info_class, "takionVideoPacketReorderingDisabled", "Z"));
	jint feedback_state_min_interval_ms = E->GetIntField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "feedbackStateMinIntervalMs", "I"));
	jint feedback_stats_log_interval_ms = E->GetIntField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "feedbackStatsLogIntervalMs", "I"));
	jboolean stream_diagnostics_enabled = E->GetBooleanField(env, connect_info_obj,
			E->GetFieldID(env, connect_info_class, "streamDiagnosticsEnabled", "Z"));
	jobject presenter_config_obj = E->GetObjectField(env, connect_info_obj,
			E->GetFieldID(env, connect_info_class, "videoPresenterConfig",
					"L"BASE_PACKAGE"/AndroidChiakiVideoPresenterConfig;"));
	jclass presenter_config_class = E->GetObjectClass(env, presenter_config_obj);
	AndroidChiakiVideoPresenterConfig presenter_config = ANDROID_CHIAKI_VIDEO_PRESENTER_CONFIG_DEFAULT;
	presenter_config.pacing_enabled = E->GetBooleanField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "pacingEnabled", "Z"));
	presenter_config.pacing_high_refresh_enabled = E->GetBooleanField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "pacingHighRefreshEnabled", "Z"));
	presenter_config.pacing_mode = (AndroidChiakiVideoPacingMode)E->GetIntField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "pacingMode", "I"));
	presenter_config.presenter_lead = (AndroidChiakiVideoPresenterLead)E->GetIntField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "presenterLead", "I"));
	presenter_config.bounded_age_enabled = E->GetBooleanField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "boundedAgeEnabled", "Z"));
	presenter_config.max_frame_age_periods = (uint32_t)E->GetIntField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "maxFrameAgePeriods", "I"));
	presenter_config.nonblocking_producer = E->GetBooleanField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "nonblockingProducer", "Z"));
	presenter_config.recovery_strategy = (AndroidChiakiVideoRecoveryStrategy)E->GetIntField(env,
			presenter_config_obj, E->GetFieldID(env, presenter_config_class, "recoveryStrategy", "I"));
	presenter_config.dejitter_enabled = E->GetBooleanField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "dejitterEnabled", "Z"));
	presenter_config.dejitter_floor_ms = (uint32_t)E->GetIntField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "dejitterFloorMs", "I"));
	presenter_config.dejitter_cap_ms = (uint32_t)E->GetIntField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "dejitterCapMs", "I"));
	presenter_config.dejitter_queue_age_frames = (uint32_t)E->GetIntField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "dejitterQueueAgeFrames", "I"));
	presenter_config.dejitter_half_rate_enabled = E->GetBooleanField(env, presenter_config_obj,
			E->GetFieldID(env, presenter_config_class, "dejitterHalfRateEnabled", "Z"));
	jint audio_buffer_bursts = E->GetIntField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "audioBufferBursts", "I"));
	jint audio_fifo_ms = E->GetIntField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "audioFifoMs", "I"));
	jboolean auto_register = E->GetBooleanField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "autoRegister", "Z"));
	jstring host_string = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "host", "Ljava/lang/String;"));
	jbyteArray regist_key_array = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "registKey", "[B"));
	jbyteArray morning_array = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "morning", "[B"));
	jobject connect_video_profile_obj = E->GetObjectField(env, connect_info_obj, E->GetFieldID(env, connect_info_class, "videoProfile", "L"BASE_PACKAGE"/ConnectVideoProfile;"));
	jclass connect_video_profile_class = E->GetObjectClass(env, connect_video_profile_obj);

	ChiakiConnectInfo connect_info = { 0 };
	ChiakiRemoteConnectionInfo remote_info = { .ctrl_sock = CHIAKI_INVALID_SOCKET, .data_sock = CHIAKI_INVALID_SOCKET };
	connect_info.ps5 = ps5;
	connect_info.auto_regist = auto_register;
	connect_info.feedback_state_min_interval_ms = (uint32_t)feedback_state_min_interval_ms;
	connect_info.feedback_stats_log_interval_ms = feedback_stats_log_interval_ms > 0 ? (uint32_t)feedback_stats_log_interval_ms : 0;
	bool stream_stats_enabled = stream_diagnostics_enabled || feedback_stats_log_interval_ms > 0;
	connect_info.stream_diagnostics_enabled = stream_diagnostics_enabled;
	CHIAKI_LOGI(log, "Stream diagnostics overlay %s; stats log %s (%s)",
			stream_diagnostics_enabled ? "enabled" : "disabled",
			feedback_stats_log_interval_ms > 0 ? "enabled" : "disabled",
			stream_stats_enabled ? "shared 1 Hz stats event" : "no stats event or periodic JNI traffic");
	connect_info.disable_video_packet_reordering = disable_video_packet_reordering;
	if(remote_ctrl_fd >= 0)
	{
		if(!psn_account_id_array || E->GetArrayLength(env, psn_account_id_array) != CHIAKI_PSN_ACCOUNT_ID_SIZE
				|| !data1_array || E->GetArrayLength(env, data1_array) != sizeof(remote_info.data1)
				|| !data2_array || E->GetArrayLength(env, data2_array) != sizeof(remote_info.data2)
				|| !custom_data1_array || E->GetArrayLength(env, custom_data1_array) != sizeof(remote_info.custom_data1)
				|| !selected_addr_string || !local_addr_string || ctrl_port <= 0 || ctrl_port > UINT16_MAX)
		{
			err = CHIAKI_ERR_INVALID_DATA;
			goto beach;
		}
		remote_info.ctrl_sock = (chiaki_socket_t)remote_ctrl_fd;
		remote_info.ctrl_port = (uint16_t)ctrl_port;
		jbyte *remote_bytes = E->GetByteArrayElements(env, psn_account_id_array, NULL);
		memcpy(connect_info.psn_account_id, remote_bytes, CHIAKI_PSN_ACCOUNT_ID_SIZE);
		E->ReleaseByteArrayElements(env, psn_account_id_array, remote_bytes, JNI_ABORT);
#define COPY_REMOTE_ARRAY(java_array, field) do { \
	remote_bytes = E->GetByteArrayElements(env, (java_array), NULL); \
	memcpy(remote_info.field, remote_bytes, sizeof(remote_info.field)); \
	E->ReleaseByteArrayElements(env, (java_array), remote_bytes, JNI_ABORT); \
} while(0)
		COPY_REMOTE_ARRAY(data1_array, data1);
		COPY_REMOTE_ARRAY(data2_array, data2);
		COPY_REMOTE_ARRAY(custom_data1_array, custom_data1);
#undef COPY_REMOTE_ARRAY
		const char *selected_addr = E->GetStringUTFChars(env, selected_addr_string, NULL);
		const char *local_addr = E->GetStringUTFChars(env, local_addr_string, NULL);
		strncpy(remote_info.selected_addr, selected_addr, sizeof(remote_info.selected_addr) - 1);
		strncpy(remote_info.regist_local_ip, local_addr, sizeof(remote_info.regist_local_ip) - 1);
		E->ReleaseStringUTFChars(env, selected_addr_string, selected_addr);
		E->ReleaseStringUTFChars(env, local_addr_string, local_addr);
		// The PSN registration request carries our own address as its HOST header. A punched
		// DatagramSocket is bound to the wildcard, so Java reports 0.0.0.0 for it; upstream
		// instead sends the LAN address of its advertised LOCAL candidate. Recover the
		// equivalent from the route to the console we actually punched.
		if(!chiaki_regist_local_addr_usable(remote_info.regist_local_ip))
		{
			char derived_local_addr[INET6_ADDRSTRLEN] = { 0 };
			if(chiaki_regist_local_addr_for_peer(remote_info.selected_addr,
					derived_local_addr, sizeof(derived_local_addr)) == CHIAKI_ERR_SUCCESS)
			{
				CHIAKI_LOGI(log, "Remote session had no usable local address; derived one from the route to the console");
				memcpy(remote_info.regist_local_ip, derived_local_addr, sizeof(derived_local_addr));
			}
			else
				CHIAKI_LOGW(log, "Remote session had no usable local address and none could be derived; regist will use its fallback");
		}
		connect_info.remote_connection = &remote_info;
	}

	const char *str_borrow = E->GetStringUTFChars(env, host_string, NULL);
	connect_info.host = host_str = strdup(str_borrow);
	E->ReleaseStringUTFChars(env, host_string, str_borrow);
	if(!connect_info.host)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}

	if(E->GetArrayLength(env, regist_key_array) != sizeof(connect_info.regist_key))
	{
		CHIAKI_LOGE(log, "Regist Key passed from Java has invalid length");
		err = CHIAKI_ERR_INVALID_DATA;
		goto beach;
	}
	jbyte *bytes = E->GetByteArrayElements(env, regist_key_array, NULL);
	memcpy(connect_info.regist_key, bytes, sizeof(connect_info.regist_key));
	E->ReleaseByteArrayElements(env, regist_key_array, bytes, JNI_ABORT);

	if(E->GetArrayLength(env, morning_array) != sizeof(connect_info.morning))
	{
		CHIAKI_LOGE(log, "Morning passed from Java has invalid length");
		err = CHIAKI_ERR_INVALID_DATA;
		goto beach;
	}
	bytes = E->GetByteArrayElements(env, morning_array, NULL);
	memcpy(connect_info.morning, bytes, sizeof(connect_info.morning));
	E->ReleaseByteArrayElements(env, morning_array, bytes, JNI_ABORT);

	connect_info.video_profile.width = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "width", "I"));
	connect_info.video_profile.height = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "height", "I"));
	connect_info.video_profile.max_fps = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "maxFPS", "I"));
	connect_info.video_profile.bitrate = (unsigned int)E->GetIntField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "bitrate", "I"));

	jobject codec_obj = E->GetObjectField(env, connect_video_profile_obj, E->GetFieldID(env, connect_video_profile_class, "codec", "L"BASE_PACKAGE"/Codec;"));
	jclass codec_class = E->GetObjectClass(env, codec_obj);
	jint target_value = E->GetIntField(env, codec_obj, E->GetFieldID(env, codec_class, "value", "I"));
	connect_info.video_profile.codec = (ChiakiCodec)target_value;

	connect_info.video_profile_auto_downgrade = true;
	connect_info.packet_loss_max = (double)packet_loss_max;
	connect_info.adaptive_loss_report = adaptive_loss_report;
	CHIAKI_LOGI(log, "Configured packet loss reported max: %.1f%%", connect_info.packet_loss_max * 100.0);
	if(connect_info.adaptive_loss_report)
		CHIAKI_LOGI(log, "Adaptive loss-report experiment enabled");

	session = CHIAKI_NEW(AndroidChiakiSession);
	if(!session)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}
	memset(session, 0, sizeof(AndroidChiakiSession));
	session->log = log;
	session->performance_mode_enabled = performance_mode;
	session->stream_stats_log_enabled = feedback_stats_log_interval_ms > 0;
	err = android_chiaki_video_decoder_init(&session->video_decoder, log, connect_info.video_profile.width, connect_info.video_profile.height,
			connect_info.video_profile.max_fps, connect_info.ps5 ? connect_info.video_profile.codec : CHIAKI_CODEC_H264,
			decoder_low_latency, real_video_timestamps, decoder_input_thread, decoder_late_frame_recovery,
			(int32_t)decoder_operating_rate, decoder_operating_rate_default,
			decoder_operating_rate_auto, decoder_realtime_priority, decoder_qcom_vt_low_latency,
			video_timestamp_rate_hz > 0 ? (unsigned int)video_timestamp_rate_hz : 0, stream_stats_enabled,
			feedback_stats_log_interval_ms > 0, &presenter_config);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		free(session);
		session = NULL;
		goto beach;
	}
	err = android_chiaki_audio_decoder_init(&session->audio_decoder, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		android_chiaki_video_decoder_fini(&session->video_decoder);
		free(session);
		session = NULL;
		goto beach;
	}

	session->audio_output = android_chiaki_audio_output_new(log,
			audio_buffer_bursts > 0 && audio_buffer_bursts <= 16 ? (uint32_t)audio_buffer_bursts : 0,
			audio_fifo_ms >= 20 && audio_fifo_ms <= 1000 ? (uint32_t)audio_fifo_ms : 171,
			stream_stats_enabled);

	android_chiaki_audio_decoder_set_cb(&session->audio_decoder, android_chiaki_audio_output_settings, android_chiaki_audio_output_frame, session->audio_output);

	err = chiaki_session_init(&session->session, &connect_info, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "JNI ChiakiSession failed to init");
		android_chiaki_video_decoder_fini(&session->video_decoder);
		android_chiaki_audio_decoder_fini(&session->audio_decoder);
		android_chiaki_audio_output_free(session->audio_output);
		free(session);
		session = NULL;
		goto beach;
	}
	android_chiaki_video_decoder_set_request_idr_cb(&session->video_decoder,
			android_chiaki_video_decoder_request_idr, session);

	session->java_session = E->NewGlobalRef(env, java_session);
	session->java_session_class = E->NewGlobalRef(env, E->GetObjectClass(env, session->java_session));
	session->java_target_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/Target"));
	session->java_target_from_value_meth = E->GetStaticMethodID(env, session->java_target_class, "fromValue", "(I)L"BASE_PACKAGE"/Target;");
	session->java_regist_host_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/RegistHost"));
	session->java_regist_host_ctor = E->GetMethodID(env, session->java_regist_host_class, "<init>", "("
			"L"BASE_PACKAGE"/Target;"
			"Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
			"[BLjava/lang/String;[BI[B)V");
	session->java_session_event_connected_meth = E->GetMethodID(env, session->java_session_class, "eventConnected", "(ZLjava/lang/String;IJJZ)V");
	session->java_session_event_login_pin_request_meth = E->GetMethodID(env, session->java_session_class, "eventLoginPinRequest", "(Z)V");
	session->java_session_event_quit_meth = E->GetMethodID(env, session->java_session_class, "eventQuit", "(ILjava/lang/String;)V");
	session->java_session_event_rumble_meth = E->GetMethodID(env, session->java_session_class, "eventRumble", "(II)V");
	session->java_session_event_remote_data_socket_needed_meth = E->GetMethodID(env, session->java_session_class, "eventRemoteDataSocketNeeded", "()V");
	session->java_session_event_registration_success_meth = E->GetMethodID(env, session->java_session_class, "eventRegistrationSuccess", "(L"BASE_PACKAGE"/RegistHost;)V");
	session->java_session_event_stream_stats_meth = E->GetMethodID(env, session->java_session_class,
			"eventStreamStats", "(JJJJJJJJJJJJJJJJJJJJJJJJJJJZJJJJDDZDJJJJZJJJ)V");
	session->java_session_performance_hint_thread_started_meth = E->GetMethodID(env, session->java_session_class, "performanceHintThreadStarted", "(II)V");
	session->java_session_performance_hint_report_meth = E->GetMethodID(env, session->java_session_class, "performanceHintReportActualWorkDuration", "(IJ)V");
	session->java_session_performance_hint_thread_stopped_meth = E->GetMethodID(env, session->java_session_class, "performanceHintThreadStopped", "(I)V");
	session->java_session_is_adpf_performance_mode_live_meth = E->GetMethodID(env, session->java_session_class, "isAdpfPerformanceModeLive", "()Z");
	session->java_session_is_sustained_performance_mode_live_meth = E->GetMethodID(env, session->java_session_class, "isSustainedPerformanceModeLive", "()Z");
	// PLE-698: measurement only, behind the stats log like the Feedback stats line it rides on.
	if(cinema_frame_latency_ready && session->stream_stats_log_enabled)
		android_chiaki_video_presenter_set_frame_latency(&session->video_decoder.presenter, &cinema_frame_latency);
	if(performance_mode && android_get_device_api_level() >= 31)
	{
		android_chiaki_video_presenter_set_performance_hint_callbacks(&session->video_decoder.presenter,
				android_chiaki_performance_hint_thread_started,
				android_chiaki_performance_hint_report,
				android_chiaki_performance_hint_thread_stopped, session);
	}

	jclass controller_state_class = E->FindClass(env, BASE_PACKAGE"/ControllerState");
	session->java_controller_state_buttons = E->GetFieldID(env, controller_state_class, "buttons", "I");
	session->java_controller_state_l2_state = E->GetFieldID(env, controller_state_class, "l2State", "B");
	session->java_controller_state_r2_state = E->GetFieldID(env, controller_state_class, "r2State", "B");
	session->java_controller_state_left_x = E->GetFieldID(env, controller_state_class, "leftX", "S");
	session->java_controller_state_left_y = E->GetFieldID(env, controller_state_class, "leftY", "S");
	session->java_controller_state_right_x = E->GetFieldID(env, controller_state_class, "rightX", "S");
	session->java_controller_state_right_y = E->GetFieldID(env, controller_state_class, "rightY", "S");
	session->java_controller_state_touches = E->GetFieldID(env, controller_state_class, "touches", "[L"BASE_PACKAGE"/ControllerTouch;");
	session->java_controller_state_gyro_x = E->GetFieldID(env, controller_state_class, "gyroX", "F");
	session->java_controller_state_gyro_y = E->GetFieldID(env, controller_state_class, "gyroY", "F");
	session->java_controller_state_gyro_z = E->GetFieldID(env, controller_state_class, "gyroZ", "F");
	session->java_controller_state_accel_x = E->GetFieldID(env, controller_state_class, "accelX", "F");
	session->java_controller_state_accel_y = E->GetFieldID(env, controller_state_class, "accelY", "F");
	session->java_controller_state_accel_z = E->GetFieldID(env, controller_state_class, "accelZ", "F");
	session->java_controller_state_orient_x = E->GetFieldID(env, controller_state_class, "orientX", "F");
	session->java_controller_state_orient_y = E->GetFieldID(env, controller_state_class, "orientY", "F");
	session->java_controller_state_orient_z = E->GetFieldID(env, controller_state_class, "orientZ", "F");
	session->java_controller_state_orient_w = E->GetFieldID(env, controller_state_class, "orientW", "F");

	jclass controller_touch_class = E->FindClass(env, BASE_PACKAGE"/ControllerTouch");
	session->java_controller_touch_x = E->GetFieldID(env, controller_touch_class, "x", "S");
	session->java_controller_touch_y = E->GetFieldID(env, controller_touch_class, "y", "S");
	session->java_controller_touch_id = E->GetFieldID(env, controller_touch_class, "id", "B");

	chiaki_session_set_event_cb(&session->session, android_chiaki_event_cb, session);
	chiaki_session_set_video_sample_cb(&session->session, android_chiaki_video_decoder_video_sample, &session->video_decoder);

	ChiakiAudioSink audio_sink;
	android_chiaki_audio_decoder_get_sink(&session->audio_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&session->session, &audio_sink);

beach:
	if(!session && log)
	{
		android_chiaki_file_log_fini(log);
		free(log);
	}

	free(host_str);
	E->SetIntField(env, result, E->GetFieldID(env, result_class, "errorCode", "I"), (jint)err);
	E->SetLongField(env, result, E->GetFieldID(env, result_class, "ptr", "J"), (jlong)session);
}

JNIEXPORT void JNICALL JNI_FCN(sessionCreate)(JNIEnv *env, jobject obj, jobject result,
		jobject connect_info_obj, jstring log_file_str, jboolean log_verbose,
		jboolean real_video_timestamps, jboolean decoder_input_thread, jobject java_session)
{
	(void)obj;
	session_create(env, result, connect_info_obj, log_file_str, log_verbose,
		real_video_timestamps, decoder_input_thread, -1, NULL, NULL, 0,
		NULL, NULL, NULL, NULL, java_session);
}

JNIEXPORT void JNICALL JNI_FCN(sessionCreateRemote)(JNIEnv *env, jobject obj, jobject result,
		jobject connect_info_obj, jstring log_file_str, jboolean log_verbose,
		jboolean real_video_timestamps, jboolean decoder_input_thread, jint control_fd,
		jbyteArray psn_account_id, jstring selected_addr, jint control_port,
		jbyteArray data1, jbyteArray data2, jbyteArray custom_data1, jstring local_addr,
		jobject java_session)
{
	(void)obj;
	session_create(env, result, connect_info_obj, log_file_str, log_verbose,
		real_video_timestamps, decoder_input_thread, control_fd, psn_account_id,
		selected_addr, control_port, data1, data2, custom_data1, local_addr, java_session);
}

JNIEXPORT void JNICALL JNI_FCN(sessionFree)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	if(!session)
		return;
	CHIAKI_LOGI(session->log, "Shutting down JNI Session");
	chiaki_session_fini(&session->session);
	android_chiaki_video_decoder_fini(&session->video_decoder);
	android_chiaki_audio_decoder_fini(&session->audio_decoder);
	android_chiaki_audio_output_free(session->audio_output);
	E->DeleteGlobalRef(env, session->java_session);
	E->DeleteGlobalRef(env, session->java_regist_host_class);
	E->DeleteGlobalRef(env, session->java_target_class);
	E->DeleteGlobalRef(env, session->java_session_class);
	CHIAKI_LOGI(session->log, "JNI Session has quit");
	android_chiaki_file_log_fini(session->log);
	free(session->log);
	free(session);
}

// PLE-698: the Go cinema turns recording on while it runs with the stats log on, and off when it stops.
JNIEXPORT void JNICALL JNI_FCN(cinemaFrameLatencyEnable)(JNIEnv *env, jobject obj, jboolean enabled)
{
	if(cinema_frame_latency_ready)
		android_chiaki_video_frame_latency_set_enabled(&cinema_frame_latency, enabled, monotonic_ns());
}

// PLE-698: one call per frame the cinema latched; all times are CLOCK_MONOTONIC ns, submit 0 when not shown.
JNIEXPORT void JNICALL JNI_FCN(cinemaFrameLatencyLatched)(JNIEnv *env, jobject obj, jlong buffer_timestamp_ns,
		jlong latched_ns, jlong submitted_ns, jlong predicted_display_ns)
{
	if(cinema_frame_latency_ready)
		android_chiaki_video_frame_latency_record_latched(&cinema_frame_latency, buffer_timestamp_ns,
				latched_ns, submitted_ns, predicted_display_ns);
}

JNIEXPORT jint JNICALL JNI_FCN(sessionStart)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(session->log, "Start JNI Session");
	return chiaki_session_start(&session->session);
}

JNIEXPORT jint JNICALL JNI_FCN(sessionStop)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(session->log, "Stop JNI Session");
	return chiaki_session_stop(&session->session);
}

JNIEXPORT jint JNICALL JNI_FCN(sessionJoin)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	CHIAKI_LOGI(session->log, "Join JNI Session");
	return chiaki_session_join(&session->session);
}

JNIEXPORT jint JNICALL JNI_FCN(sessionSetRemoteDataSocket)(JNIEnv *env, jobject obj, jlong ptr, jint fd)
{
	(void)env;
	(void)obj;
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	if(!session)
		return CHIAKI_ERR_INVALID_DATA;
	return chiaki_session_set_remote_data_socket(&session->session, (chiaki_socket_t)fd);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetSurface)(JNIEnv *env, jobject obj, jlong ptr, jobject surface,
		jint stream_fps, jdouble refresh_hz, jlong app_vsync_offset_ns)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	android_chiaki_video_decoder_set_surface(&session->video_decoder, env, surface,
			(unsigned int)stream_fps, (double)refresh_hz, (int64_t)app_vsync_offset_ns);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetTiming)(JNIEnv *env, jobject obj, jlong ptr,
		jint stream_fps, jdouble refresh_hz, jlong app_vsync_offset_ns)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	android_chiaki_video_decoder_set_timing(&session->video_decoder,
			(unsigned int)stream_fps, (double)refresh_hz, (int64_t)app_vsync_offset_ns);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetPacingMode)(JNIEnv *env, jobject obj, jlong ptr, jint pacing_mode)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	android_chiaki_video_decoder_set_pacing_mode(&session->video_decoder,
			(AndroidChiakiVideoPacingMode)pacing_mode);
}

JNIEXPORT jobject JNICALL JNI_FCN(sessionGetVideoStats)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	AndroidChiakiVideoStats stats;
	android_chiaki_video_decoder_get_stats(&session->video_decoder, &stats);

	jclass stats_class = E->FindClass(env, BASE_PACKAGE"/VideoStats");
	jmethodID constructor = E->GetMethodID(env, stats_class, "<init>", "(JJJJ)V");
	return E->NewObject(env, stats_class, constructor, (jlong)stats.input_frames_dropped,
			(jlong)stats.missed_vsyncs, (jlong)stats.presenter_frames_dropped,
			(jlong)stats.dejitter_buffer_ns);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetControllerState)(JNIEnv *env, jobject obj, jlong ptr, jobject controller_state_java)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	ChiakiControllerState controller_state;
	chiaki_controller_state_set_idle(&controller_state);
	controller_state.buttons = (uint32_t)E->GetIntField(env, controller_state_java, session->java_controller_state_buttons);
	controller_state.l2_state = (uint8_t)E->GetByteField(env, controller_state_java, session->java_controller_state_l2_state);
	controller_state.r2_state = (uint8_t)E->GetByteField(env, controller_state_java, session->java_controller_state_r2_state);
	controller_state.left_x = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_left_x);
	controller_state.left_y = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_left_y);
	controller_state.right_x = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_right_x);
	controller_state.right_y = (int16_t)E->GetShortField(env, controller_state_java, session->java_controller_state_right_y);
	jobjectArray touch_array = E->GetObjectField(env, controller_state_java, session->java_controller_state_touches);
	size_t touch_array_len = (size_t)E->GetArrayLength(env, touch_array);
	for(size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		if(i < touch_array_len)
		{
			jobject touch = E->GetObjectArrayElement(env, touch_array, i);
			controller_state.touches[i].x = (uint16_t)E->GetShortField(env, touch, session->java_controller_touch_x);
			controller_state.touches[i].y = (uint16_t)E->GetShortField(env, touch, session->java_controller_touch_y);
			controller_state.touches[i].id = (int8_t)E->GetByteField(env, touch, session->java_controller_touch_id);
		}
		else
		{
			controller_state.touches[i].x = 0;
			controller_state.touches[i].y = 0;
			controller_state.touches[i].id = -1;
		}
	}
	controller_state.gyro_x = E->GetFloatField(env, controller_state_java, session->java_controller_state_gyro_x);
	controller_state.gyro_y = E->GetFloatField(env, controller_state_java, session->java_controller_state_gyro_y);
	controller_state.gyro_z = E->GetFloatField(env, controller_state_java, session->java_controller_state_gyro_z);
	controller_state.accel_x = E->GetFloatField(env, controller_state_java, session->java_controller_state_accel_x);
	controller_state.accel_y = E->GetFloatField(env, controller_state_java, session->java_controller_state_accel_y);
	controller_state.accel_z = E->GetFloatField(env, controller_state_java, session->java_controller_state_accel_z);
	controller_state.orient_x = E->GetFloatField(env, controller_state_java, session->java_controller_state_orient_x);
	controller_state.orient_y = E->GetFloatField(env, controller_state_java, session->java_controller_state_orient_y);
	controller_state.orient_z = E->GetFloatField(env, controller_state_java, session->java_controller_state_orient_z);
	controller_state.orient_w = E->GetFloatField(env, controller_state_java, session->java_controller_state_orient_w);
	chiaki_session_set_controller_state(&session->session, &controller_state);
}

JNIEXPORT void JNICALL JNI_FCN(sessionSetLoginPin)(JNIEnv *env, jobject obj, jlong ptr, jstring pin_java)
{
	AndroidChiakiSession *session = (AndroidChiakiSession *)ptr;
	const char *pin = E->GetStringUTFChars(env, pin_java, NULL);
	chiaki_session_set_login_pin(&session->session, (const uint8_t *)pin, strlen(pin));
	E->ReleaseStringUTFChars(env, pin_java, pin);
}

typedef struct android_discovery_service_t
{
	ChiakiDiscoveryService service;
	jobject java_service;
	jclass java_service_class;
	jmethodID java_service_hosts_updated_meth;

	jclass host_class;
	jmethodID host_ctor;
	jobject host_state_unknown;
	jobject host_state_ready;
	jobject host_state_standby;
} AndroidDiscoveryService;

static void android_discovery_service_cb(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user)
{
	AndroidDiscoveryService *service = user;

	CHIAKI_LOGI(&global_log, "JNI Discovery Callback got %llu hosts", (unsigned long long)hosts_count);

	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;

	jobjectArray r = E->NewObjectArray(env, hosts_count, service->host_class, NULL);

	for(size_t i=0; i<hosts_count; i++)
	{
		jobject state;
		ChiakiDiscoveryHost *host = hosts + i;
		switch(host->state)
		{
			case CHIAKI_DISCOVERY_HOST_STATE_STANDBY:
				state = service->host_state_standby;
				break;
			case CHIAKI_DISCOVERY_HOST_STATE_READY:
				state = service->host_state_ready;
				break;
			default:
				state = service->host_state_unknown;
				break;
		}

		jobject o = E->NewObject(env, service->host_class, service->host_ctor,
				state,
				host->host_request_port,
				jnistr_from_ascii(env, host->host_addr),
				jnistr_from_ascii(env, host->system_version),
				jnistr_from_ascii(env, host->device_discovery_protocol_version),
				jnistr_from_ascii(env, host->host_name),
				jnistr_from_ascii(env, host->host_type),
				jnistr_from_ascii(env, host->host_id),
				jnistr_from_ascii(env, host->running_app_titleid),
				jnistr_from_ascii(env, host->running_app_name));

		E->SetObjectArrayElement(env, r, i, o);
	}

	E->CallVoidMethod(env, service->java_service, service->java_service_hosts_updated_meth, r);

	(*global_vm)->DetachCurrentThread(global_vm);
}

static ChiakiErrorCode sockaddr_from_java(JNIEnv *env, jobject /*InetSocketAddress*/ sockaddr_obj, struct sockaddr **addr, size_t *addr_size)
{
	jclass sockaddr_class = E->GetObjectClass(env, sockaddr_obj);
	uint16_t port = (uint16_t)E->CallIntMethod(env, sockaddr_obj, E->GetMethodID(env, sockaddr_class, "getPort", "()I"));
	jobject addr_obj = E->CallObjectMethod(env, sockaddr_obj, E->GetMethodID(env, sockaddr_class, "getAddress", "()Ljava/net/InetAddress;"));
	jclass addr_class = E->GetObjectClass(env, addr_obj);
	jbyteArray addr_byte_array = E->CallObjectMethod(env, addr_obj, E->GetMethodID(env, addr_class, "getAddress", "()[B"));
	jsize addr_byte_array_len = E->GetArrayLength(env, addr_byte_array);

	if(addr_byte_array_len == 4)
	{
		struct sockaddr_in *inaddr = CHIAKI_NEW(struct sockaddr_in);
		if(!inaddr)
			return CHIAKI_ERR_MEMORY;
		memset(inaddr, 0, sizeof(*inaddr));
		inaddr->sin_family = AF_INET;
		jbyte *bytes = E->GetByteArrayElements(env, addr_byte_array, NULL);
		memcpy(&inaddr->sin_addr.s_addr, bytes, sizeof(inaddr->sin_addr.s_addr));
		E->ReleaseByteArrayElements(env, addr_byte_array, bytes, JNI_ABORT);
		inaddr->sin_port = htons(port);

		*addr = (struct sockaddr *)inaddr;
		*addr_size = sizeof(*inaddr);
	}
	else if(addr_byte_array_len == 0x10)
	{
		struct sockaddr_in6 *inaddr6 = CHIAKI_NEW(struct sockaddr_in6);
		if(!inaddr6)
			return CHIAKI_ERR_MEMORY;
		memset(inaddr6, 0, sizeof(*inaddr6));
		inaddr6->sin6_family = AF_INET6;
		jbyte *bytes = E->GetByteArrayElements(env, addr_byte_array, NULL);
		memcpy(&inaddr6->sin6_addr.in6_u, bytes, sizeof(inaddr6->sin6_addr.in6_u));
		E->ReleaseByteArrayElements(env, addr_byte_array, bytes, JNI_ABORT);
		inaddr6->sin6_port = htons(port);

		*addr = (struct sockaddr *)inaddr6;
		*addr_size = sizeof(*inaddr6);
	}
	else
		return CHIAKI_ERR_INVALID_DATA;

	return CHIAKI_ERR_SUCCESS;
}

JNIEXPORT void JNICALL JNI_FCN(discoveryServiceCreate)(JNIEnv *env, jobject obj, jobject result, jobject options_obj, jobject java_service)
{
	jclass result_class = E->GetObjectClass(env, result);
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	ChiakiDiscoveryServiceOptions options = { 0 };

	AndroidDiscoveryService *service = CHIAKI_NEW(AndroidDiscoveryService);
	if(!service)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}

	jclass options_class = E->GetObjectClass(env, options_obj);

	options.hosts_max = (size_t)E->GetLongField(env, options_obj, E->GetFieldID(env, options_class, "hostsMax", "J"));
	options.host_drop_pings = (uint64_t)E->GetLongField(env, options_obj, E->GetFieldID(env, options_class, "hostDropPings", "J"));
	options.ping_ms = (uint64_t)E->GetLongField(env, options_obj, E->GetFieldID(env, options_class, "pingMs", "J"));
	options.cb = android_discovery_service_cb;
	options.cb_user = service;

	struct sockaddr *send_addr_tmp = NULL;
	err = sockaddr_from_java(env, E->GetObjectField(env, options_obj, E->GetFieldID(env, options_class, "sendAddr", "Ljava/net/InetSocketAddress;")), &send_addr_tmp, &options.send_addr_size);
	options.send_addr = (struct sockaddr_storage *)send_addr_tmp;
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&global_log, "Failed to get sockaddr from InetSocketAddress");
		goto beach;
	}

	service->java_service = E->NewGlobalRef(env, java_service);
	service->java_service_class = E->GetObjectClass(env, service->java_service);
	service->java_service_hosts_updated_meth = E->GetMethodID(env, service->java_service_class, "hostsUpdated", "([L"BASE_PACKAGE"/DiscoveryHost;)V");

	service->host_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/DiscoveryHost"));
	service->host_ctor = E->GetMethodID(env, service->host_class, "<init>", "("
		"L"BASE_PACKAGE"/DiscoveryHost$State;"
		"S" // hostRequestPort: UShort
		"Ljava/lang/String;" // hostAddr: String?,
		"Ljava/lang/String;" // systemVersion: String?,
		"Ljava/lang/String;" // deviceDiscoveryProtocolVersion: String?,
		"Ljava/lang/String;" // hostName: String?,
		"Ljava/lang/String;" // hostType: String?,
		"Ljava/lang/String;" // hostId: String?,
		"Ljava/lang/String;" // runningAppTitleid: String?,
		"Ljava/lang/String;" // runningAppName: String?
		")V");

	jclass host_state_class = E->FindClass(env, BASE_PACKAGE"/DiscoveryHost$State");
	service->host_state_unknown = E->NewGlobalRef(env, E->GetStaticObjectField(env, host_state_class, E->GetStaticFieldID(env, host_state_class, "UNKNOWN", "L"BASE_PACKAGE"/DiscoveryHost$State;")));
	service->host_state_standby = E->NewGlobalRef(env, E->GetStaticObjectField(env, host_state_class, E->GetStaticFieldID(env, host_state_class, "STANDBY", "L"BASE_PACKAGE"/DiscoveryHost$State;")));
	service->host_state_ready = E->NewGlobalRef(env, E->GetStaticObjectField(env, host_state_class, E->GetStaticFieldID(env, host_state_class, "READY", "L"BASE_PACKAGE"/DiscoveryHost$State;")));


	err = chiaki_discovery_service_init(&service->service, &options, &global_log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&global_log, "Failed to create discovery service (JNI)");
		E->DeleteGlobalRef(env, service->java_service);
		E->DeleteGlobalRef(env, service->host_state_unknown);
		E->DeleteGlobalRef(env, service->host_state_standby);
		E->DeleteGlobalRef(env, service->host_state_ready);
		E->DeleteGlobalRef(env, service->host_class);
		free(service);
		goto beach;
	}

beach:
	free(options.send_addr);
	E->SetIntField(env, result, E->GetFieldID(env, result_class, "errorCode", "I"), (jint)err);
	E->SetLongField(env, result, E->GetFieldID(env, result_class, "ptr", "J"), (jlong)service);
}

JNIEXPORT void JNICALL JNI_FCN(discoveryServiceFree)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidDiscoveryService *service = (AndroidDiscoveryService *)ptr;
	if(!service)
		return;
	chiaki_discovery_service_fini(&service->service);
	E->DeleteGlobalRef(env, service->java_service);
	E->DeleteGlobalRef(env, service->host_state_unknown);
	E->DeleteGlobalRef(env, service->host_state_standby);
	E->DeleteGlobalRef(env, service->host_state_ready);
	E->DeleteGlobalRef(env, service->host_class);
	free(service);
}

JNIEXPORT jint JNICALL JNI_FCN(discoveryServiceWakeup)(JNIEnv *env, jobject obj, jlong ptr, jstring host_string, jlong user_credential, jboolean ps5)
{
	AndroidDiscoveryService *service = (AndroidDiscoveryService *)ptr;
	const char *host = E->GetStringUTFChars(env, host_string, NULL);
	ChiakiErrorCode r = chiaki_discovery_wakeup(&global_log, service ? &service->service.discovery : NULL, host, (uint64_t)user_credential, ps5);
	E->ReleaseStringUTFChars(env, host_string, host);
	return r;
}


typedef struct android_chiaki_regist_t
{
	AndroidChiakiJNILog log;
	ChiakiRegist regist;

	jobject java_regist;
	jmethodID java_regist_event_meth;

	jclass java_target_class;

	jobject java_regist_event_canceled;
	jobject java_regist_event_failed;
	jclass java_regist_event_success_class;
	jmethodID java_regist_event_success_ctor;

	jclass java_regist_host_class;
	jmethodID java_regist_host_ctor;
} AndroidChiakiRegist;

static jobject create_jni_target(JNIEnv *env, jclass target_class, ChiakiTarget target)
{
	jmethodID meth = E->GetStaticMethodID(env, target_class, "fromValue", "(I)L"BASE_PACKAGE"/Target;");
	return E->CallStaticObjectMethod(env, target_class, meth, (jint)target);
}

static void android_chiaki_regist_cb(ChiakiRegistEvent *event, void *user)
{
	AndroidChiakiRegist *regist = user;

	JNIEnv *env = attach_thread_jni();
	if(!env)
		return;

	jobject java_event = NULL;
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
			java_event = regist->java_regist_event_canceled;
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
			java_event = regist->java_regist_event_failed;
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
		{
			ChiakiRegisteredHost *host = event->registered_host;
			jobject java_host = E->NewObject(env, regist->java_regist_host_class, regist->java_regist_host_ctor,
					create_jni_target(env, regist->java_target_class, host->target),
					jnistr_from_ascii(env, host->ap_ssid),
					jnistr_from_ascii(env, host->ap_bssid),
					jnistr_from_ascii(env, host->ap_key),
					jnistr_from_ascii(env, host->ap_name),
					jnibytearray_create(env, host->server_mac, sizeof(host->server_mac)),
					jnistr_from_ascii(env, host->server_nickname),
					jnibytearray_create(env, (const uint8_t *)host->rp_regist_key, sizeof(host->rp_regist_key)),
					(jint)host->rp_key_type,
					jnibytearray_create(env, host->rp_key, sizeof(host->rp_key)));
			java_event = E->NewObject(env, regist->java_regist_event_success_class, regist->java_regist_event_success_ctor, java_host);
			break;
		}
	}

	if(java_event)
		E->CallVoidMethod(env, regist->java_regist, regist->java_regist_event_meth, java_event);

	(*global_vm)->DetachCurrentThread(global_vm);
}

static void android_chiaki_regist_fini_partial(JNIEnv *env, AndroidChiakiRegist *regist)
{
	android_chiaki_jni_log_fini(&regist->log, env);
	E->DeleteGlobalRef(env, regist->java_regist);
	E->DeleteGlobalRef(env, regist->java_target_class);
	E->DeleteGlobalRef(env, regist->java_regist_event_canceled);
	E->DeleteGlobalRef(env, regist->java_regist_event_failed);
	E->DeleteGlobalRef(env, regist->java_regist_event_success_class);
	E->DeleteGlobalRef(env, regist->java_regist_host_class);
}

JNIEXPORT void JNICALL JNI_FCN(registStart)(JNIEnv *env, jobject obj, jobject result, jobject regist_info_obj, jobject log_obj, jobject java_regist)
{
	jclass result_class = E->GetObjectClass(env, result);
	ChiakiErrorCode err = CHIAKI_ERR_SUCCESS;
	AndroidChiakiRegist *regist = CHIAKI_NEW(AndroidChiakiRegist);
	if(!regist)
	{
		err = CHIAKI_ERR_MEMORY;
		goto beach;
	}

	android_chiaki_jni_log_init(&regist->log, env, log_obj);

	regist->java_regist = E->NewGlobalRef(env, java_regist);
	regist->java_regist_event_meth = E->GetMethodID(env, E->GetObjectClass(env, regist->java_regist), "event", "(L"BASE_PACKAGE"/RegistEvent;)V");

	regist->java_target_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/Target"));

	regist->java_regist_event_canceled = E->NewGlobalRef(env, get_kotlin_global_object(env, BASE_PACKAGE"/RegistEventCanceled"));
	regist->java_regist_event_failed = E->NewGlobalRef(env, get_kotlin_global_object(env, BASE_PACKAGE"/RegistEventFailed"));
	regist->java_regist_event_success_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/RegistEventSuccess"));
	regist->java_regist_event_success_ctor = E->GetMethodID(env, regist->java_regist_event_success_class, "<init>", "(L"BASE_PACKAGE"/RegistHost;)V");

	regist->java_regist_host_class = E->NewGlobalRef(env, E->FindClass(env, BASE_PACKAGE"/RegistHost"));
	regist->java_regist_host_ctor = E->GetMethodID(env, regist->java_regist_host_class, "<init>", "("
			  "L"BASE_PACKAGE"/Target;" // target: Target
			  "Ljava/lang/String;" // apSsid: String
			  "Ljava/lang/String;" // apBssid: String
			  "Ljava/lang/String;" // apKey: String
			  "Ljava/lang/String;" // apName: String
			  "[B" // serverMac: ByteArray
			  "Ljava/lang/String;" // serverNickname: String
			  "[B" // rpRegistKey: ByteArray
			  "I" // rpKeyType: UInt
			  "[B" // rpKey: ByteArray
			  ")V");

	jclass regist_info_class = E->GetObjectClass(env, regist_info_obj);

	jobject target_obj = E->GetObjectField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "target", "L"BASE_PACKAGE"/Target;"));
	jclass target_class = E->GetObjectClass(env, target_obj);
	jint target_value = E->GetIntField(env, target_obj, E->GetFieldID(env, target_class, "value", "I"));

	jstring host_string = E->GetObjectField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "host", "Ljava/lang/String;"));
	jboolean broadcast = E->GetBooleanField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "broadcast", "Z"));
	jstring psn_online_id_string = E->GetObjectField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "psnOnlineId", "Ljava/lang/String;"));
	jbyteArray psn_account_id_array = E->GetObjectField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "psnAccountId", "[B"));
	jint pin = E->GetIntField(env, regist_info_obj, E->GetFieldID(env, regist_info_class, "pin", "I"));

	ChiakiRegistInfo regist_info = { 0 };
	regist_info.target = (ChiakiTarget)target_value;
	regist_info.host = E->GetStringUTFChars(env, host_string, NULL);
	regist_info.broadcast = broadcast;
	if(psn_online_id_string)
		regist_info.psn_online_id = E->GetStringUTFChars(env, psn_online_id_string, NULL);
	if(psn_account_id_array && E->GetArrayLength(env, psn_account_id_array) == sizeof(regist_info.psn_account_id))
		E->GetByteArrayRegion(env, psn_account_id_array, 0, sizeof(regist_info.psn_account_id), (jbyte *)regist_info.psn_account_id);
	regist_info.pin = (uint32_t)pin;

	err = chiaki_regist_start(&regist->regist, &regist->log.log, &regist_info, android_chiaki_regist_cb, regist);

	E->ReleaseStringUTFChars(env, host_string, regist_info.host);
	if(regist_info.psn_online_id)
		E->ReleaseStringUTFChars(env, psn_online_id_string, regist_info.psn_online_id);

	if(err != CHIAKI_ERR_SUCCESS)
	{
		android_chiaki_regist_fini_partial(env, regist);
		free(regist);
		regist = NULL;
	}

beach:
	E->SetIntField(env, result, E->GetFieldID(env, result_class, "errorCode", "I"), (jint)err);
	E->SetLongField(env, result, E->GetFieldID(env, result_class, "ptr", "J"), (jlong)regist);
}

JNIEXPORT void JNICALL JNI_FCN(registStop)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiRegist *regist = (AndroidChiakiRegist *)ptr;
	chiaki_regist_stop(&regist->regist);
}

JNIEXPORT void JNICALL JNI_FCN(registFree)(JNIEnv *env, jobject obj, jlong ptr)
{
	AndroidChiakiRegist *regist = (AndroidChiakiRegist *)ptr;
	chiaki_regist_fini(&regist->regist);
	android_chiaki_regist_fini_partial(env, regist);
	free(regist);
}
