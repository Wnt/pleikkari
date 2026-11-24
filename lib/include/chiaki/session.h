// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_SESSION_H
#define CHIAKI_SESSION_H

#include "streamconnection.h"
#include "common.h"
#include "thread.h"
#include "log.h"
#include "ctrl.h"
#include "rpcrypt.h"
#include "takion.h"
#include "ecdh.h"
#include "audio.h"
#include "controller.h"
#include "stoppipe.h"
#include "remote/holepunch.h"
#include "remote/rudp.h"
#include "regist.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHIAKI_SESSION_PORT 9295

#define CHIAKI_RP_APPLICATION_REASON_REGIST_FAILED		0x80108b09
#define CHIAKI_RP_APPLICATION_REASON_INVALID_PSN_ID		0x80108b02
#define CHIAKI_RP_APPLICATION_REASON_IN_USE				0x80108b10
#define CHIAKI_RP_APPLICATION_REASON_CRASH				0x80108b15
#define CHIAKI_RP_APPLICATION_REASON_RP_VERSION			0x80108b11
#define CHIAKI_RP_APPLICATION_REASON_UNKNOWN			0x80108bff

CHIAKI_EXPORT const char *chiaki_rp_application_reason_string(uint32_t reason);

/**
 * @return RP-Version string or NULL
 */
CHIAKI_EXPORT const char *chiaki_rp_version_string(ChiakiTarget target);

CHIAKI_EXPORT ChiakiTarget chiaki_rp_version_parse(const char *rp_version_str, bool is_ps5);


#define CHIAKI_RP_DID_SIZE 32
#define CHIAKI_SESSION_ID_SIZE_MAX 80
#define CHIAKI_HANDSHAKE_KEY_SIZE 0x10

typedef struct chiaki_connect_video_profile_t
{
	unsigned int width;
	unsigned int height;
	unsigned int max_fps;
	unsigned int bitrate;
	ChiakiCodec codec;
} ChiakiConnectVideoProfile;

/** Android-owned PSN control-plane result. Connected sockets are transferred to the session. */
typedef struct chiaki_remote_connection_info_t
{
	chiaki_socket_t ctrl_sock;
	chiaki_socket_t data_sock;
	uint8_t data1[16];
	uint8_t data2[16];
	uint8_t custom_data1[16];
	char regist_local_ip[INET6_ADDRSTRLEN];
	char selected_addr[INET6_ADDRSTRLEN];
	uint16_t ctrl_port;
} ChiakiRemoteConnectionInfo;

typedef enum {
	// values must not change
	CHIAKI_VIDEO_RESOLUTION_PRESET_360p = 1,
	CHIAKI_VIDEO_RESOLUTION_PRESET_540p = 2,
	CHIAKI_VIDEO_RESOLUTION_PRESET_720p = 3,
	CHIAKI_VIDEO_RESOLUTION_PRESET_1080p = 4
} ChiakiVideoResolutionPreset;

typedef enum {
	// values must not change
	CHIAKI_VIDEO_FPS_PRESET_30 = 30,
	CHIAKI_VIDEO_FPS_PRESET_60 = 60
} ChiakiVideoFPSPreset;

CHIAKI_EXPORT void chiaki_connect_video_profile_preset(ChiakiConnectVideoProfile *profile, ChiakiVideoResolutionPreset resolution, ChiakiVideoFPSPreset fps);

#define CHIAKI_SESSION_AUTH_SIZE 0x10

typedef struct chiaki_connect_info_t
{
	bool ps5;
	const char *host; // null terminated
	char regist_key[CHIAKI_SESSION_AUTH_SIZE]; // must be completely filled (pad with \0)
	uint8_t morning[0x10];
	ChiakiConnectVideoProfile video_profile;
	bool video_profile_auto_downgrade; // Downgrade video_profile if server does not seem to support it.
	bool enable_keyboard;
	bool enable_dualsense;
	ChiakiDisableAudioVideo audio_video_disabled;
	bool auto_regist;
	ChiakiHolepunchSession holepunch_session;
	ChiakiRemoteConnectionInfo *remote_connection;
	chiaki_socket_t *rudp_sock;
	uint8_t psn_account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	double packet_loss_max;
	bool adaptive_loss_report; // false by default; uncap measured loss only during sustained poor conditions
	bool enable_idr_on_fec_failure;
	bool disable_video_packet_reordering;
	uint32_t feedback_state_min_interval_ms; // 0 = default (8ms), minimum time between controller feedback state sends
	uint32_t feedback_stats_log_interval_ms; // 0 = off (default), else emit the general stream-stats line this often
	bool stream_diagnostics_enabled; // false by default; emit one CHIAKI_EVENT_STREAM_STATS per second when true
} ChiakiConnectInfo;


typedef enum {
	CHIAKI_QUIT_REASON_NONE,
	CHIAKI_QUIT_REASON_STOPPED,
	CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN,
	CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED,
	CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE,
	CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH,
	CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH,
	CHIAKI_QUIT_REASON_CTRL_UNKNOWN,
	CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED,
	CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED,
	CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN,
	CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED,
	CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN, // like REMOTE_DISCONNECTED, but because the server shut down
	CHIAKI_QUIT_REASON_PSN_REGIST_FAILED,
	/** PLE-423: nothing arrived from the console on the data socket for
	 * CHIAKI_LINK_WATCHDOG_TIMEOUT_MS while streaming. Appended at the end
	 * deliberately: the Android side hard-codes ordinals of this enum
	 * (SessionHandoffRetry.kt, StreamEndCause.kt), so a value inserted above
	 * would silently repoint them. */
	CHIAKI_QUIT_REASON_STREAM_CONNECTION_TIMEOUT,
} ChiakiQuitReason;

CHIAKI_EXPORT const char *chiaki_quit_reason_string(ChiakiQuitReason reason);

static inline bool chiaki_quit_reason_is_error(ChiakiQuitReason reason)
{
	return reason != CHIAKI_QUIT_REASON_STOPPED && reason != CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN;
}

typedef struct chiaki_quit_event_t
{
	ChiakiQuitReason reason;
	const char *reason_str;
} ChiakiQuitEvent;

typedef struct chiaki_keyboard_event_t
{
	const char *text_str;
} ChiakiKeyboardEvent;

typedef struct chiaki_audio_stream_info_event_t
{
	ChiakiAudioHeader audio_header;
} ChiakiAudioStreamInfoEvent;

typedef struct chiaki_rumble_event_t
{
	uint8_t unknown;
	uint8_t left; // low-frequency
	uint8_t right; // high-frequency
} ChiakiRumbleEvent;

#define CHIAKI_TRIGGER_EFFECTS_MASK_L2 0x01
#define CHIAKI_TRIGGER_EFFECTS_MASK_R2 0x02

/**
 * The console sets the two triggers independently: mask says which of them this event
 * changes (CHIAKI_TRIGGER_EFFECTS_MASK_*). Only a trigger named in the mask may be written
 * to the controller. type and data always hold the current effect of both triggers, so the
 * trigger that is not in the mask reads as what it was last set to.
 */
typedef struct chiaki_trigger_effects_event_t
{
	uint8_t type_left;
	uint8_t type_right;
	uint8_t left[10];
	uint8_t right[10];
	uint8_t mask;
} ChiakiTriggerEffectsEvent;

typedef struct chiaki_video_fec_failure_event_t
{
	int32_t frame_index;
	bool idr_request_sent;
} ChiakiVideoFecFailureEvent;

typedef struct chiaki_stream_stats_event_t
{
	uint64_t interval_ms;
	uint64_t rtt_us; // senkusha's startup ping average; 0 when senkusha failed
	uint64_t stream_frames;
	/**
	 * Session-cumulative (not per window, unlike its neighbours): video frames that never
	 * became decodable -- FEC failed, reference missing, or (PLE-474) no unit of the
	 * frame arrived at all. Each frame index counts once. A frame that never arrived is
	 * only known once a later frame does, so a blackout lands here when the stream
	 * resumes, not while it lasts.
	 */
	uint64_t video_frames_lost;
	/**
	 * PLE-485: session-cumulative, like video_frames_lost above -- frames that arrived
	 * intact and were fully assembled, but were thrown away because they were a P-frame
	 * decoded while the receiver was still waiting for an IDR. Not transport loss, so kept
	 * out of video_frames_lost; nothing reads this yet (that is PLE-486's question).
	 */
	uint64_t video_frames_discarded_for_idr;
	uint64_t video_reorder_timeouts;
	uint64_t video_packet_jitter_us; // PLE-356: frame-boundary delay variation; drives the badge
	uint64_t video_packet_jitter_raw_us; // superseded per-packet EWMA, logged for comparison only
	bool video_packet_jitter_filled; // PLE-403: false while video_packet_jitter_us is one unsmoothed sample
	uint64_t takion_packets_received;
	/**
	 * PLE-475: units missing from frames that partly arrived, not "packets lost" on the
	 * wire. `chiaki_frame_processor_alloc_frame()` only runs once a frame's first unit
	 * arrives, so a frame with zero units received is never instantiated and never
	 * contributes to either side of this count -- a total outage adds nothing here at
	 * all (see `takion_silence_ms` below, which is what can see one).
	 */
	uint64_t takion_partial_frame_units_missing;
	uint64_t feedback_packets;
	uint64_t fec_recovered_packets;
	uint64_t unrecoverable_packets;
	uint64_t feedback_gap_sum_ms;
	uint64_t feedback_gap_count;
	uint64_t feedback_gap_max_ms;
	uint64_t feedback_gaps_over_50_ms;
	bool connection_quality_valid;
	uint64_t target_bitrate_bps;
	uint64_t measured_throughput_bps;
	double console_rtt_raw; // ConnectionQualityPayload.rtt as decoded, unit and meaning unverified
	uint64_t console_rtt_us; // console_rtt_raw read as ms; diagnostics only
	uint64_t server_loss;
	double congestion_measured_loss;
	double congestion_reported_loss;
	uint64_t probe_rtt_us; // last heartbeat -> DATA_ACK round trip on the stream socket, 0 = none yet
	uint64_t probe_rtt_samples;
	uint64_t probe_rtt_unacked;
	uint64_t probe_rtt_ambiguous;
	/**
	 * PLE-464: how long the console's socket was silent, in ms. `takion_silence_ms`
	 * is the silence still running at this poll; `takion_max_receive_gap_ms` is the
	 * longest gap that occurred anywhere inside this window, including one that has
	 * already closed. The second is the one to threshold on -- see
	 * chiaki_takion_take_window_max_receive_gap_ms().
	 *
	 * This exists because `takion_partial_frame_units_missing` cannot see a total outage at all:
	 * it is only ever raised by chiaki_frame_processor_report_packet_stats(), which
	 * runs from chiaki_video_receiver_av_packet() and therefore only when a packet
	 * arrives. Nothing arriving means nothing counted, so `received + lost` is 0 and
	 * the loss rate reads a clean 0 % through a blackout (measured over 920 stats
	 * lines, PLE-404).
	 */
	uint64_t takion_silence_ms;
	uint64_t takion_max_receive_gap_ms;
} ChiakiStreamStatsEvent;

typedef enum {
	CHIAKI_EVENT_CONNECTED,
	CHIAKI_EVENT_LOGIN_PIN_REQUEST,
	CHIAKI_EVENT_HOLEPUNCH,
	CHIAKI_EVENT_REGIST,
	CHIAKI_EVENT_NICKNAME_RECEIVED,
	CHIAKI_EVENT_KEYBOARD_OPEN,
	CHIAKI_EVENT_KEYBOARD_TEXT_CHANGE,
	CHIAKI_EVENT_KEYBOARD_REMOTE_CLOSE,
	CHIAKI_EVENT_RUMBLE,
	CHIAKI_EVENT_QUIT,
	CHIAKI_EVENT_TRIGGER_EFFECTS,
	CHIAKI_EVENT_MOTION_RESET,
	CHIAKI_EVENT_LED_COLOR,
	CHIAKI_EVENT_PLAYER_INDEX,
	CHIAKI_EVENT_HAPTIC_INTENSITY,
	CHIAKI_EVENT_TRIGGER_INTENSITY,
	CHIAKI_EVENT_VIDEO_FEC_FAILURE,
	CHIAKI_EVENT_REMOTE_DATA_SOCKET_NEEDED,
	CHIAKI_EVENT_STREAM_STATS,
} ChiakiEventType;

// PLE-371: snapshot of how the stream connection actually got made, taken once
// when CHIAKI_EVENT_CONNECTED fires (after ctrl + senkusha have run), so mtu_in
// and rtt_us_measured below are already the final measured-or-fallback values.
typedef struct chiaki_connected_event_t
{
	bool relay; // session->holepunch_session || session->remote_connection: PSN data plane, not the LAN. Fires for the Android fork's opt-in "PSN Remote Play" feature (Preferences.psnRemotePlayEnabled, default false) when the console isn't reachable on the LAN -- confirmed 2026-09-21 by an S25-over-VPN capture reaching PsnRemote "stage Streaming". Default (LAN) streaming must never take this path; see PLE-500 and AGENTS.md.
	char peer_host[256]; // address actually dialled; empty for the native-holepunch relay path, which has no single peer address to show
	uint16_t peer_port;
	uint32_t mtu_in;
	uint64_t rtt_us;
	bool measured; // false when mtu_in/rtt_us are the senkusha-failed 1454/1000us fallback (session.c), not a measurement. mtu and rtt share this one fate: they are set together in the same success/failure branch.
} ChiakiConnectedEvent;

typedef struct chiaki_event_t
{
	ChiakiEventType type;
	union
	{
		ChiakiQuitEvent quit;
		ChiakiKeyboardEvent keyboard;
		ChiakiRumbleEvent rumble;
		ChiakiRegisteredHost host;
		ChiakiTriggerEffectsEvent trigger_effects;
		uint8_t led_state[0x3];
		uint8_t player_index;
		struct
		{
			bool pin_incorrect; // false on first request, true if the pin entered before was incorrect
		} login_pin_request;
		struct
		{
			bool finished; // false when punching hole, true when finished
		} data_holepunch;
		ChiakiDualSenseEffectIntensity intensity;
		char server_nickname[0x20];
		ChiakiVideoFecFailureEvent video_fec_failure;
		ChiakiStreamStatsEvent stream_stats;
		ChiakiConnectedEvent connected;
	};
} ChiakiEvent;

typedef void (*ChiakiEventCallback)(ChiakiEvent *event, void *user);

/**
 * buf will always have an allocated padding of at least CHIAKI_VIDEO_BUFFER_PADDING_SIZE after buf_size.
 * frame_index is the 16-bit Takion AV frame serial number and wraps every 65536 frames.
 * frame_ready_time_us is the monotonic time at which the complete frame became available.
 * It is 0 for codec configuration samples that do not represent a completed frame.
 * @return whether the sample was successfully pushed into the decoder. On false, a corrupt frame will be reported to get a new keyframe.
 */
typedef bool (*ChiakiVideoSampleCallback)(uint8_t *buf, size_t buf_size, ChiakiSeqNum16 frame_index,
		uint64_t frame_ready_time_us, int32_t frames_lost, bool frame_recovered, void *user);



typedef struct chiaki_session_t
{
	struct
	{
		bool ps5;
		struct addrinfo *host_addrinfos;
		struct addrinfo *host_addrinfo_selected;
		char hostname[256];
		char regist_key[CHIAKI_RPCRYPT_KEY_SIZE];
		uint8_t morning[CHIAKI_RPCRYPT_KEY_SIZE];
		uint8_t did[CHIAKI_RP_DID_SIZE];
		ChiakiConnectVideoProfile video_profile;
		bool video_profile_auto_downgrade;
		ChiakiDisableAudioVideo disable_audio_video;
		bool enable_keyboard;
		bool enable_dualsense;
		uint8_t psn_account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
		bool enable_idr_on_fec_failure;
		bool disable_video_packet_reordering;
		uint32_t feedback_state_min_interval_ms;
		uint32_t feedback_stats_log_interval_ms;
		bool stream_diagnostics_enabled;
		bool adaptive_loss_report;
	} connect_info;

	ChiakiTarget target;

	uint8_t nonce[CHIAKI_RPCRYPT_KEY_SIZE];
	ChiakiRPCrypt rpcrypt;
	char session_id[CHIAKI_SESSION_ID_SIZE_MAX]; // zero-terminated
	uint8_t handshake_key[CHIAKI_HANDSHAKE_KEY_SIZE];
	uint32_t mtu_in;
	uint32_t mtu_out;
	uint64_t rtt_us;
	bool rtt_us_measured; // false when rtt_us is the senkusha-failed fallback, not a measurement
	bool dontfrag;
	ChiakiECDH ecdh;

	ChiakiQuitReason quit_reason;
	char *quit_reason_str; // additional reason string from remote

	ChiakiEventCallback event_cb;
	void *event_cb_user;
	ChiakiVideoSampleCallback video_sample_cb;
	void *video_sample_cb_user;
	ChiakiAudioSink audio_sink;
	ChiakiAudioSink haptics_sink;
	ChiakiCtrlDisplaySink display_sink;

	ChiakiThread session_thread;

	ChiakiCond state_cond;
	ChiakiMutex state_mutex;
	ChiakiStopPipe stop_pipe;
	bool auto_regist;
	bool should_stop;
	bool ctrl_failed;
	bool ctrl_session_id_received;
	bool ctrl_login_pin_requested;
	bool ctrl_first_heartbeat_received;
	bool login_pin_entered;
	bool psn_regist_succeeded;
	bool stream_connection_switch_received;
	uint8_t *login_pin;
	size_t login_pin_size;

	ChiakiCtrl ctrl;
	ChiakiHolepunchSession holepunch_session;
	bool remote_connection;
	ChiakiRemoteConnectionInfo remote_connection_info;
	ChiakiRudp rudp;

	ChiakiLog *log;

	ChiakiStreamConnection stream_connection;

	ChiakiControllerState controller_state;
} ChiakiSession;

/**
 * PLE-499: MTU Senkusha's failure path falls back to for a local interface MTU
 * (interface_mtu <= 0 means undeterminable). Exposed for host testing; production
 * code reaches it only through the Senkusha-failed branch in session.c.
 */
CHIAKI_EXPORT uint32_t chiaki_session_mtu_fallback(ChiakiLog *log, int interface_mtu);

CHIAKI_EXPORT ChiakiErrorCode chiaki_session_init(ChiakiSession *session, ChiakiConnectInfo *connect_info, ChiakiLog *log);
CHIAKI_EXPORT void chiaki_session_fini(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_start(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_stop(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_join(ChiakiSession *session);

CHIAKI_EXPORT void chiaki_session_send_event(ChiakiSession *session, ChiakiEvent *event);

CHIAKI_EXPORT ChiakiErrorCode chiaki_session_request_idr(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_set_controller_state(ChiakiSession *session, ChiakiControllerState *state);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_set_login_pin(ChiakiSession *session, const uint8_t *pin, size_t pin_size);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_set_stream_connection_switch_received(ChiakiSession *session);
/** Takes ownership of data_sock only when CHIAKI_ERR_SUCCESS is returned. */
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_set_remote_data_socket(ChiakiSession *session, chiaki_socket_t data_sock);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_goto_bed(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_toggle_microphone(ChiakiSession *session, bool muted);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_connect_microphone(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_keyboard_set_text(ChiakiSession *session, const char *text);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_keyboard_reject(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_keyboard_accept(ChiakiSession *session);
CHIAKI_EXPORT ChiakiErrorCode chiaki_session_go_home(ChiakiSession *session);

static inline void chiaki_session_set_event_cb(ChiakiSession *session, ChiakiEventCallback cb, void *user)
{
	session->event_cb = cb;
	session->event_cb_user = user;
}

static inline void chiaki_session_set_video_sample_cb(ChiakiSession *session, ChiakiVideoSampleCallback cb, void *user)
{
	session->video_sample_cb = cb;
	session->video_sample_cb_user = user;
}

/**
 * @param sink contents are copied
 */
static inline void chiaki_session_set_audio_sink(ChiakiSession *session, ChiakiAudioSink *sink)
{
	session->audio_sink = *sink;
}

/**
 * @param sink contents are copied
 */
static inline void chiaki_session_set_haptics_sink(ChiakiSession *session, ChiakiAudioSink *sink)
{
	session->haptics_sink = *sink;
}

/**
 * @param sink contents are copied
 */
static inline void chiaki_session_ctrl_set_display_sink(ChiakiSession *session, ChiakiCtrlDisplaySink *sink)
{
	session->display_sink = *sink;
}

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_SESSION_H
