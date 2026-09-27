// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.common

import android.content.Context
import android.content.SharedPreferences
import androidx.annotation.StringRes
import androidx.preference.PreferenceManager
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.stream.VrEnvironmentConfig
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import fi.madekivi.pleikkari.lib.AndroidChiakiVideoPresenterConfig
import fi.madekivi.pleikkari.lib.Codec
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.lib.ConnectVideoProfile
import fi.madekivi.pleikkari.lib.VideoFPSPreset
import fi.madekivi.pleikkari.lib.VideoResolutionPreset
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.math.max
import kotlin.math.min

class Preferences(context: Context)
{
	enum class Resolution(val value: String, @StringRes val title: Int, val preset: VideoResolutionPreset)
	{
		RES_360P("360p", R.string.preferences_resolution_title_360p, VideoResolutionPreset.RES_360P),
		RES_540P("540p", R.string.preferences_resolution_title_540p, VideoResolutionPreset.RES_540P),
		RES_720P("720p", R.string.preferences_resolution_title_720p, VideoResolutionPreset.RES_720P),
		RES_1080P("1080p", R.string.preferences_resolution_title_1080p, VideoResolutionPreset.RES_1080P),
	}

	enum class FPS(val value: String, @StringRes val title: Int, val preset: VideoFPSPreset)
	{
		FPS_30("30", R.string.preferences_fps_title_30, VideoFPSPreset.FPS_30),
		FPS_60("60", R.string.preferences_fps_title_60, VideoFPSPreset.FPS_60)
	}

	enum class DisplayRefreshRateMode(val value: String, @StringRes val title: Int)
	{
		SYSTEM_DEFAULT("system_default", R.string.preferences_display_refresh_rate_title_system_default),
		MATCH_STREAM("match_stream", R.string.preferences_display_refresh_rate_title_match_stream),
		HIGHEST("highest", R.string.preferences_display_refresh_rate_title_highest)
	}

	enum class VideoPacingMode(val value: String, val nativeValue: Int, @StringRes val title: Int)
	{
		LOWEST_LATENCY("lowest_latency", 1, R.string.preferences_video_pacing_mode_title_lowest_latency),
		BALANCED("balanced", 2, R.string.preferences_video_pacing_mode_title_balanced),
		SMOOTHEST("smoothest", 3, R.string.preferences_video_pacing_mode_title_smoothest)
	}

	enum class VideoPresenterLead(val value: String, val nativeValue: Int, @StringRes val title: Int)
	{
		TWO_MS("2ms", 0, R.string.preferences_video_presenter_lead_title_2ms),
		HALF_VSYNC("half_vsync", 1, R.string.preferences_video_presenter_lead_title_half_vsync)
	}

	enum class VideoRecoveryStrategy(val value: String, val nativeValue: Int, @StringRes val title: Int)
	{
		TIMELINE_SHIFT("timeline_shift", 0, R.string.preferences_video_recovery_strategy_title_timeline_shift),
		FLUSH("flush", 1, R.string.preferences_video_recovery_strategy_title_flush)
	}

	enum class Codec(val value: String, @StringRes val title: Int, val codec: fi.madekivi.pleikkari.lib.Codec)
	{
		CODEC_H264("h264", R.string.preferences_codec_title_h264, fi.madekivi.pleikkari.lib.Codec.CODEC_H264),
		CODEC_H265("h265", R.string.preferences_codec_title_h265, fi.madekivi.pleikkari.lib.Codec.CODEC_H265)
	}

	enum class StreamQualityPreset(
		val value: String,
		@StringRes val title: Int,
		val resolution: Resolution,
		val fps: FPS,
		val codec: Codec,
		val bitrate: Int? = null,
		val decoderOperatingRateDefault: Boolean = true,
		val decoderOperatingRateAuto: Boolean = true,
		val decoderOperatingRate: Int = 0,
		val decoderInputThreadEnabled: Boolean = true,
		val debandingEnabled: Boolean = false
	)
	{
		BALANCED("balanced", R.string.stream_quality_preset_balanced, Resolution.RES_1080P, FPS.FPS_60, Codec.CODEC_H265),
		LOW_LATENCY("low_latency", R.string.stream_quality_preset_low_latency, Resolution.RES_1080P, FPS.FPS_60, Codec.CODEC_H265, bitrate = 10000),
		DATA_SAVER("data_saver", R.string.stream_quality_preset_data_saver, Resolution.RES_540P, FPS.FPS_30, Codec.CODEC_H264)
	}

	companion object
	{
		/** PLE-753: the rooms' MSAA sample counts on offer; 4 is PLE-615's, 1 turns MSAA off. */
		val goVrRoomMsaaChoices = listOf(4, 2, 1)
		const val GO_VR_ROOM_MSAA_DEFAULT = 4
		val resolutionDefault = Resolution.RES_1080P
		val resolutionAll = Resolution.values()
		val fpsDefault = FPS.FPS_60
		val fpsAll = FPS.values()
		val displayRefreshRateModeDefault = DisplayRefreshRateMode.SYSTEM_DEFAULT
		val displayRefreshRateModeAll = DisplayRefreshRateMode.values()
		val videoPacingModeDefault = VideoPacingMode.BALANCED
		val videoPacingModeAll = VideoPacingMode.values()
		val videoPresenterLeadDefault = VideoPresenterLead.TWO_MS
		val videoPresenterLeadAll = VideoPresenterLead.values()
		const val videoPacingMaxFrameAgePeriodsDefault = 2
		const val videoDejitterFloorMsDefault = 12
		const val videoDejitterCapMsDefault = 32
		const val videoDejitterQueueAgeFramesDefault = 2
		val videoRecoveryStrategyDefault = VideoRecoveryStrategy.FLUSH
		val videoRecoveryStrategyAll = VideoRecoveryStrategy.values()
		val codecDefault = Codec.CODEC_H265
		val codecAll = Codec.values()
		val streamQualityPresetDefault = StreamQualityPreset.LOW_LATENCY
		const val packetLossMaxPercentDefault = 5
		const val audioBufferBurstsDefault = 0
		const val audioFifoMsDefault = 171
	}

	internal val sharedPreferences = PreferenceManager.getDefaultSharedPreferences(context)
	private val sharedPreferenceChangeListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
		when(key)
		{
			resolutionKey -> _bitrateAutoFlow.value = bitrateAuto
		}
	}.also { sharedPreferences.registerOnSharedPreferenceChangeListener(it) }

	private val resources = context.resources

	val discoveryEnabledKey get() = resources.getString(R.string.preferences_discovery_enabled_key)
	var discoveryEnabled
		get() = sharedPreferences.getBoolean(discoveryEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(discoveryEnabledKey, value).apply() }

	val psnSignInEnabledKey get() = resources.getString(R.string.preferences_psn_sign_in_enabled_key)
	var psnSignInEnabled
		get() = sharedPreferences.getBoolean(psnSignInEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(psnSignInEnabledKey, value).apply() }

	val psnRemotePlayEnabledKey get() = resources.getString(R.string.preferences_psn_remote_play_enabled_key)
	var psnRemotePlayEnabled
		get() = sharedPreferences.getBoolean(psnRemotePlayEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(psnRemotePlayEnabledKey, value).apply() }

	private val psnAccountIdKey get() = resources.getString(R.string.preferences_psn_account_id_key)
	var psnAccountId: String?
		get() = sharedPreferences.getString(psnAccountIdKey, null)
		set(value) { sharedPreferences.edit().putString(psnAccountIdKey, value).apply() }

	val onScreenControlsEnabledKey get() = resources.getString(R.string.preferences_on_screen_controls_enabled_key)
	var onScreenControlsEnabled
		get() = sharedPreferences.getBoolean(onScreenControlsEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(onScreenControlsEnabledKey, value).apply() }


	val rumbleEnabledKey get() = resources.getString(R.string.preferences_rumble_enabled_key)
	var rumbleEnabled
		get() = sharedPreferences.getBoolean(rumbleEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(rumbleEnabledKey, value).apply() }

	val motionEnabledKey get() = resources.getString(R.string.preferences_motion_enabled_key)
	var motionEnabled
		get() = sharedPreferences.getBoolean(motionEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(motionEnabledKey, value).apply() }

	val buttonHapticEnabledKey get() = resources.getString(R.string.preferences_button_haptic_enabled_key)
	var buttonHapticEnabled
		get() = sharedPreferences.getBoolean(buttonHapticEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(buttonHapticEnabledKey, value).apply() }

	val logVerboseKey get() = resources.getString(R.string.preferences_log_verbose_key)
	var logVerbose
		get() = sharedPreferences.getBoolean(logVerboseKey, false)
		set(value) { sharedPreferences.edit().putBoolean(logVerboseKey, value).apply() }

	val swapCrossMoonKey get() = resources.getString(R.string.preferences_swap_cross_moon_key)
	var swapCrossMoon
		get() = sharedPreferences.getBoolean(swapCrossMoonKey, false)
		set(value) { sharedPreferences.edit().putBoolean(swapCrossMoonKey, value).apply() }

	val debandingEnabledKey get() = resources.getString(R.string.preferences_debanding_key)
	var debandingEnabled
		get() = sharedPreferences.getBoolean(debandingEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(debandingEnabledKey, value).apply() }

	// Default false: keeps today's RENDERMODE_CONTINUOUSLY behavior unless a user opts in,
	// so the change can be A/B tested rather than silently altering the deband render cadence.
	val debandRenderWhenDirtyEnabledKey get() = resources.getString(R.string.preferences_debanding_render_when_dirty_key)
	var debandRenderWhenDirtyEnabled
		get() = sharedPreferences.getBoolean(debandRenderWhenDirtyEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(debandRenderWhenDirtyEnabledKey, value).apply() }

	val realVideoTimestampsKey get() = resources.getString(R.string.preferences_real_video_timestamps_key)
	var realVideoTimestamps
		get() = sharedPreferences.getBoolean(realVideoTimestampsKey, false)
		set(value) { sharedPreferences.edit().putBoolean(realVideoTimestampsKey, value).apply() }

	val decoderLowLatencyEnabledKey get() = resources.getString(R.string.preferences_decoder_low_latency_enabled_key)
	var decoderLowLatencyEnabled
		get() = sharedPreferences.getBoolean(decoderLowLatencyEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(decoderLowLatencyEnabledKey, value).apply() }

	val feedbackReducedIntervalEnabledKey get() = resources.getString(R.string.preferences_feedback_reduced_interval_enabled_key)
	var feedbackReducedIntervalEnabled
		get() = sharedPreferences.getBoolean(feedbackReducedIntervalEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(feedbackReducedIntervalEnabledKey, value).apply() }

	val feedbackStatsLogEnabledKey get() = resources.getString(R.string.preferences_feedback_stats_log_enabled_key)
	var feedbackStatsLogEnabled
		get() = sharedPreferences.getBoolean(feedbackStatsLogEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(feedbackStatsLogEnabledKey, value).apply() }

	/** Window of the general per-stream stats line in the session log; 0 when the setting is off. */
	val feedbackStatsLogIntervalMs get() = if(feedbackStatsLogEnabled) 1000 else 0

	val senkushaFallbackNoticeEnabledKey get() = resources.getString(R.string.preferences_senkusha_fallback_notice_enabled_key)
	/** PLE-509: A/B switch for a one-shot toast telling the user Senkusha failed and a
	 * fallback MTU/RTT is in use. Off by default so today's silence stays the baseline. */
	var senkushaFallbackNoticeEnabled
		get() = sharedPreferences.getBoolean(senkushaFallbackNoticeEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(senkushaFallbackNoticeEnabledKey, value).apply() }

	val streamEndCauseProbeEnabledKey get() = resources.getString(R.string.preferences_stream_end_cause_probe_enabled_key)
	/** PLE-262: probe the console after it ends a stream, to name a takeover on the TV. Runs only after the stream ended. */
	var streamEndCauseProbeEnabled
		get() = sharedPreferences.getBoolean(streamEndCauseProbeEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(streamEndCauseProbeEnabledKey, value).apply() }

	val streamDiagnosticsOverlayEnabledKey get() = resources.getString(R.string.preferences_stream_diagnostics_overlay_enabled_key)
	var streamDiagnosticsOverlayEnabled
		get() = sharedPreferences.getBoolean(streamDiagnosticsOverlayEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(streamDiagnosticsOverlayEnabledKey, value).apply() }

	fun validateAudioBufferBursts(bursts: Int) = max(0, min(16, bursts))
	val audioBufferBurstsKey get() = resources.getString(R.string.preferences_audio_buffer_bursts_key)
	var audioBufferBursts
		get() = validateAudioBufferBursts(sharedPreferences.getInt(audioBufferBurstsKey, audioBufferBurstsDefault))
		set(value) { sharedPreferences.edit().putInt(audioBufferBurstsKey, validateAudioBufferBursts(value)).apply() }

	fun validateAudioFifoMs(fifoMs: Int) = max(20, min(1000, fifoMs))
	val audioFifoMsKey get() = resources.getString(R.string.preferences_audio_fifo_ms_key)
	var audioFifoMs
		get() = validateAudioFifoMs(sharedPreferences.getInt(audioFifoMsKey, audioFifoMsDefault))
		set(value) { sharedPreferences.edit().putInt(audioFifoMsKey, validateAudioFifoMs(value)).apply() }

	val decoderInputThreadEnabledKey get() = resources.getString(R.string.preferences_decoder_input_thread_enabled_key)
	var decoderInputThreadEnabled
		get() = sharedPreferences.getBoolean(decoderInputThreadEnabledKey, true)
		set(value) { sharedPreferences.edit().putBoolean(decoderInputThreadEnabledKey, value).apply() }

	val threadPriorityBoostEnabledKey get() = resources.getString(R.string.preferences_thread_priority_boost_enabled_key)
	var threadPriorityBoostEnabled
		get() = sharedPreferences.getBoolean(threadPriorityBoostEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(threadPriorityBoostEnabledKey, value).apply() }

	val performanceModeEnabledKey get() = resources.getString(R.string.preferences_performance_mode_enabled_key)
	var performanceModeEnabled
		get() = sharedPreferences.getBoolean(performanceModeEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(performanceModeEnabledKey, value).apply() }

	val wifiLowLatencyLockEnabledKey get() = resources.getString(R.string.preferences_wifi_low_latency_lock_enabled_key)
	var wifiLowLatencyLockEnabled
		get() = sharedPreferences.getBoolean(wifiLowLatencyLockEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(wifiLowLatencyLockEnabledKey, value).apply() }

	val takionVideoPacketReorderingDisabledKey get() = resources.getString(R.string.preferences_takion_video_packet_reordering_disabled_key)
	var takionVideoPacketReorderingDisabled
		get() = sharedPreferences.getBoolean(takionVideoPacketReorderingDisabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(takionVideoPacketReorderingDisabledKey, value).apply() }

	val decoderLateFrameRecoveryEnabledKey get() = resources.getString(R.string.preferences_decoder_late_frame_recovery_enabled_key)
	var decoderLateFrameRecoveryEnabled
		get() = sharedPreferences.getBoolean(decoderLateFrameRecoveryEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(decoderLateFrameRecoveryEnabledKey, value).apply() }

	// PLE-689: the native cinema is the product on the Go; the Oculus TV panel path is the opt-out.
	val goVrEnabledKey get() = "stream_go_vr_enabled"
	var goVrEnabled
		get() = sharedPreferences.getBoolean(goVrEnabledKey, GoDecoderProfile.eligible())
		set(value) { sharedPreferences.edit().putBoolean(goVrEnabledKey, value).apply() }

	// PLE-690: the console the Go Library's VR launch streams from when more than one is registered.
	val lastConsoleMacKey get() = "last_console_mac"
	var lastConsoleMac: MacAddress?
		get() = if(sharedPreferences.contains(lastConsoleMacKey)) MacAddress(sharedPreferences.getLong(lastConsoleMacKey, 0L)) else null
		set(value) { sharedPreferences.edit().apply { if(value == null) remove(lastConsoleMacKey) else putLong(lastConsoleMacKey, value.value) }.apply() }

	// PLE-636: native Go cinema at 60 Hz for a 60 fps stream; off keeps PLE-602's 72 Hz.
	/**
	 * PLE-722: the Go's VR UI toolkit menu (panels, laser pointer, pad focus; docs/design/vr-ui.md).
	 * On by default (operator ruling: on the Go the VR UI is the product); off keeps PLE-602's
	 * head-locked strip menu, for the head-loop A/B.
	 */
	val goVrUiKey get() = "stream_go_vr_ui"
	var goVrUi
		get() = sharedPreferences.getBoolean(goVrUiKey, true)
		set(value) { sharedPreferences.edit().putBoolean(goVrUiKey, value).apply() }

	val goVrMatch60HzKey get() = "stream_go_vr_match_60hz"
	var goVrMatch60Hz
		get() = sharedPreferences.getBoolean(goVrMatch60HzKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrMatch60HzKey, value).apply() }

	val goVrRoomHighGpuKey get() = "stream_go_vr_room_high_gpu"
	var goVrRoomHighGpu
		get() = sharedPreferences.getBoolean(goVrRoomHighGpuKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrRoomHighGpuKey, value).apply() }

	// PLE-673: A/B the cinema's frame-available listener on its own thread instead of the main looper.
	val goVrFrameListenerThreadKey get() = "stream_go_vr_frame_listener_thread"
	var goVrFrameListenerThread
		get() = sharedPreferences.getBoolean(goVrFrameListenerThreadKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrFrameListenerThreadKey, value).apply() }

	// PLE-715: A/B the cinema starting each frame just before VrApi's release instead of right after the last.
	val goVrLateStartKey get() = "stream_go_vr_late_start"
	var goVrLateStart
		get() = sharedPreferences.getBoolean(goVrLateStartKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrLateStartKey, value).apply() }

	// PLE-753: A/B keeping VrApi at lead 0 (hold after a late frame, drain a lead of 1) with frames still
	// starting right after the last submit returns; applies with a room too, unlike the late start.
	val goVrHoldDrainKey get() = "stream_go_vr_hold_drain"
	var goVrHoldDrain
		get() = sharedPreferences.getBoolean(goVrHoldDrainKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrHoldDrainKey, value).apply() }

	// PLE-753: A/B the rooms' MSAA sample count (PLE-615's 4x by default; 2x or off cut their GPU time).
	val goVrRoomMsaaKey get() = "stream_go_vr_room_msaa"
	var goVrRoomMsaa: Int
		get() = sharedPreferences.getString(goVrRoomMsaaKey, null)?.toIntOrNull()?.takeIf { it in goVrRoomMsaaChoices }
			?: GO_VR_ROOM_MSAA_DEFAULT
		set(value) { sharedPreferences.edit().putString(goVrRoomMsaaKey, value.toString()).apply() }

	// PLE-755: A/B a warm-up draw of the cinema's eye images before the first submit.
	val goVrWarmUpKey get() = "stream_go_vr_warm_up"
	var goVrWarmUp
		get() = sharedPreferences.getBoolean(goVrWarmUpKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrWarmUpKey, value).apply() }

	// PLE-746: the Go cinema's input-to-photon probe (per-frame GPU luma, presses.csv/frames.csv, atrace
	// markers). Measurement only; its frame join also needs stream_feedback_stats_log.
	val goVrLatencyProbeKey get() = "stream_go_vr_latency_probe"
	var goVrLatencyProbe
		get() = sharedPreferences.getBoolean(goVrLatencyProbeKey, false)
		set(value) { sharedPreferences.edit().putBoolean(goVrLatencyProbeKey, value).apply() }

	val videoPacingEnabledKey get() = resources.getString(R.string.preferences_video_pacing_enabled_key)
	var videoPacingEnabled
		get() = sharedPreferences.getBoolean(videoPacingEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(videoPacingEnabledKey, value).apply() }

	val videoPacingHighRefreshEnabledKey get() = resources.getString(R.string.preferences_video_pacing_high_refresh_enabled_key)
	var videoPacingHighRefreshEnabled
		get() = sharedPreferences.getBoolean(videoPacingHighRefreshEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(videoPacingHighRefreshEnabledKey, value).apply() }

	val videoDejitterEnabledKey get() = resources.getString(R.string.preferences_video_dejitter_enabled_key)
	var videoDejitterEnabled
		get() = sharedPreferences.getBoolean(videoDejitterEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(videoDejitterEnabledKey, value).apply() }

	val videoDejitterHalfRateEnabledKey get() = resources.getString(R.string.preferences_video_dejitter_half_rate_enabled_key)
	var videoDejitterHalfRateEnabled
		get() = sharedPreferences.getBoolean(videoDejitterHalfRateEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(videoDejitterHalfRateEnabledKey, value).apply() }

	val videoPacingBoundedAgeEnabledKey get() = resources.getString(R.string.preferences_video_pacing_bounded_age_enabled_key)
	var videoPacingBoundedAgeEnabled
		get() = sharedPreferences.getBoolean(videoPacingBoundedAgeEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(videoPacingBoundedAgeEnabledKey, value).apply() }

	val videoPresenterNonblockingProducerKey get() = resources.getString(R.string.preferences_video_presenter_nonblocking_producer_key)
	var videoPresenterNonblockingProducer
		get() = sharedPreferences.getBoolean(videoPresenterNonblockingProducerKey, false)
		set(value) { sharedPreferences.edit().putBoolean(videoPresenterNonblockingProducerKey, value).apply() }

	val controllerInputCoalescingEnabledKey get() = resources.getString(R.string.preferences_controller_input_coalescing_enabled_key)
	var controllerInputCoalescingEnabled
		get() = sharedPreferences.getBoolean(controllerInputCoalescingEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(controllerInputCoalescingEnabledKey, value).apply() }

	val gamepadUnbufferedDispatchEnabledKey get() = resources.getString(R.string.preferences_gamepad_unbuffered_dispatch_enabled_key)
	var gamepadUnbufferedDispatchEnabled
		get() = sharedPreferences.getBoolean(gamepadUnbufferedDispatchEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(gamepadUnbufferedDispatchEnabledKey, value).apply() }

	val gamepadTriggerFallbackEnabledKey get() = resources.getString(R.string.preferences_gamepad_trigger_fallback_enabled_key)
	var gamepadTriggerFallbackEnabled
		get() = sharedPreferences.getBoolean(gamepadTriggerFallbackEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(gamepadTriggerFallbackEnabledKey, value).apply() }

	val touchscreenTouchpadEnabledKey get() = "preferences_touchscreen_touchpad_enabled"
	var touchscreenTouchpadEnabled
		get() = sharedPreferences.getBoolean(touchscreenTouchpadEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(touchscreenTouchpadEnabledKey, value).apply() }

	val streamWindowOptimizationsEnabledKey get() = "preferences_stream_window_optimizations_enabled"
	var streamWindowOptimizationsEnabled
		get() = sharedPreferences.getBoolean(streamWindowOptimizationsEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(streamWindowOptimizationsEnabledKey, value).apply() }

	// PLE-12: coalesce touch-controls redraws to once per vsync via postInvalidateOnAnimation,
	// and request unbuffered input dispatch, instead of invalidating on every touch sample.
	// Default false preserves today's per-sample invalidate() behaviour.
	val coalesceTouchRedrawEnabledKey get() = "preferences_coalesce_touch_redraw_enabled"
	var coalesceTouchRedrawEnabled
		get() = sharedPreferences.getBoolean(coalesceTouchRedrawEnabledKey, false)
		set(value) { sharedPreferences.edit().putBoolean(coalesceTouchRedrawEnabledKey, value).apply() }

	// Mapping Keys
	fun getMappingKey(buttonName: String) = "mapping_$buttonName"
	
	private fun getMapping(buttonName: String, default: Int): Int {
		return sharedPreferences.getString(getMappingKey(buttonName), default.toString())?.toIntOrNull() ?: default
	}
	
	private fun setMapping(buttonName: String, value: Int) {
		sharedPreferences.edit().putString(getMappingKey(buttonName), value.toString()).apply()
	}

	var mappingCross: Int 
		get() = getMapping("cross", 96)
		set(value) = setMapping("cross", value)
		
	var mappingCircle: Int
		get() = getMapping("circle", 97)
		set(value) = setMapping("circle", value)
		
	var mappingSquare: Int
		get() = getMapping("square", 99)
		set(value) = setMapping("square", value)
		
	var mappingTriangle: Int
		get() = getMapping("triangle", 100)
		set(value) = setMapping("triangle", value)
		
	var mappingL1: Int
		get() = getMapping("l1", 102)
		set(value) = setMapping("l1", value)
		
	var mappingR1: Int
		get() = getMapping("r1", 103)
		set(value) = setMapping("r1", value)
		
	var mappingL2: Int
		get() = getMapping("l2", 104)
		set(value) = setMapping("l2", value)
		
	var mappingR2: Int
		get() = getMapping("r2", 105)
		set(value) = setMapping("r2", value)
		
	var mappingL3: Int
		get() = getMapping("l3", 106)
		set(value) = setMapping("l3", value)
		
	var mappingR3: Int
		get() = getMapping("r3", 107)
		set(value) = setMapping("r3", value)
		
	var mappingOptions: Int
		get() = getMapping("options", 108)
		set(value) = setMapping("options", value)
		
	var mappingShare: Int
		get() = getMapping("share", 109)
		set(value) = setMapping("share", value)
		
	var mappingPs: Int
		get() = getMapping("ps", 110)
		set(value) = setMapping("ps", value)

	val sharpnessIntensityKey get() = "preferences_sharpness_intensity"
	var sharpnessIntensity: Float
		get() = sharedPreferences.getInt(sharpnessIntensityKey, 0).toFloat() / 100f
		set(value) { sharedPreferences.edit().putInt(sharpnessIntensityKey, (value * 100f).toInt()).apply() }


	// PLE-603: the Oculus Go cinema's environment and screen geometry (stream/VrEnvironment.kt).
	// Lengths are stored in centimetres and levels in percent so the SeekBar settings can hold
	// them as ints; VrEnvironmentConfig.toNative() converts. Only offered on the Go
	// (VrEnvironmentSupport.offered); every default reproduces PLE-602's plain screen.
	val vrEnvironmentKey get() = resources.getString(R.string.preferences_vr_environment_key)
	var vrEnvironment
		get() = VrEnvironmentKind.fromValue(sharedPreferences.getString(vrEnvironmentKey, null))
		set(value) { sharedPreferences.edit().putString(vrEnvironmentKey, value.value).apply() }

	val vrScreenDistanceCmKey get() = resources.getString(R.string.preferences_vr_screen_distance_cm_key)
	var vrScreenDistanceCm
		get() = sharedPreferences.getInt(vrScreenDistanceCmKey, VrEnvironmentConfig.SCREEN_DISTANCE_CM_DEFAULT)
		set(value) { sharedPreferences.edit().putInt(vrScreenDistanceCmKey, value).apply() }

	val vrScreenWidthCmKey get() = resources.getString(R.string.preferences_vr_screen_width_cm_key)
	var vrScreenWidthCm
		get() = sharedPreferences.getInt(vrScreenWidthCmKey, VrEnvironmentConfig.SCREEN_WIDTH_CM_DEFAULT)
		set(value) { sharedPreferences.edit().putInt(vrScreenWidthCmKey, value).apply() }

	val vrScreenCurveRadiusCmKey get() = resources.getString(R.string.preferences_vr_screen_curve_radius_cm_key)
	var vrScreenCurveRadiusCm
		get() = sharedPreferences.getInt(vrScreenCurveRadiusCmKey, VrEnvironmentConfig.SCREEN_CURVE_RADIUS_CM_DEFAULT)
		set(value) { sharedPreferences.edit().putInt(vrScreenCurveRadiusCmKey, value).apply() }

	val vrScreenHeightOffsetCmKey get() = resources.getString(R.string.preferences_vr_screen_height_offset_cm_key)
	var vrScreenHeightOffsetCm
		get() = sharedPreferences.getInt(vrScreenHeightOffsetCmKey, VrEnvironmentConfig.SCREEN_HEIGHT_OFFSET_CM_DEFAULT)
		set(value) { sharedPreferences.edit().putInt(vrScreenHeightOffsetCmKey, value).apply() }

	val vrGlowPercentKey get() = resources.getString(R.string.preferences_vr_glow_percent_key)
	var vrGlowPercent
		get() = sharedPreferences.getInt(vrGlowPercentKey, VrEnvironmentConfig.GLOW_PERCENT_DEFAULT)
		set(value) { sharedPreferences.edit().putInt(vrGlowPercentKey, value).apply() }

	val vrRoomLightPercentKey get() = resources.getString(R.string.preferences_vr_room_light_percent_key)
	var vrRoomLightPercent
		get() = sharedPreferences.getInt(vrRoomLightPercentKey, VrEnvironmentConfig.ROOM_LIGHT_PERCENT_DEFAULT)
		set(value) { sharedPreferences.edit().putInt(vrRoomLightPercentKey, value).apply() }

	/** The clamped configuration the VR activity hands to the native environment renderer. */
	fun vrEnvironmentConfig() = VrEnvironmentConfig(
		environment = vrEnvironment,
		screenDistanceCm = vrScreenDistanceCm,
		screenWidthCm = vrScreenWidthCm,
		screenCurveRadiusCm = vrScreenCurveRadiusCm,
		screenHeightOffsetCm = vrScreenHeightOffsetCm,
		glowPercent = vrGlowPercent,
		roomLightPercent = vrRoomLightPercent
	).clamped()

	val resolutionKey get() = resources.getString(R.string.preferences_resolution_key)
	var resolution
		get() = sharedPreferences.getString(resolutionKey, resolutionDefault.value)?.let { value ->
			Resolution.values().firstOrNull { it.value == value }
		} ?: resolutionDefault
		set(value) { sharedPreferences.edit().putString(resolutionKey, value.value).apply() }

	val fpsKey get() = resources.getString(R.string.preferences_fps_key)
	var fps
		get() = sharedPreferences.getString(fpsKey, fpsDefault.value)?.let { value ->
			FPS.values().firstOrNull { it.value == value }
		}  ?: fpsDefault
		set(value) { sharedPreferences.edit().putString(fpsKey, value.value).apply() }

	val displayRefreshRateModeKey get() = resources.getString(R.string.preferences_display_refresh_rate_key)
	var displayRefreshRateMode
		get() = sharedPreferences.getString(displayRefreshRateModeKey, displayRefreshRateModeDefault.value)?.let { value ->
			DisplayRefreshRateMode.values().firstOrNull { it.value == value }
		} ?: displayRefreshRateModeDefault
		set(value) { sharedPreferences.edit().putString(displayRefreshRateModeKey, value.value).apply() }

	val videoPacingModeKey get() = resources.getString(R.string.preferences_video_pacing_mode_key)
	var videoPacingMode
		get() = sharedPreferences.getString(videoPacingModeKey, videoPacingModeDefault.value)?.let { value ->
			VideoPacingMode.values().firstOrNull { it.value == value }
		} ?: videoPacingModeDefault
		set(value) { sharedPreferences.edit().putString(videoPacingModeKey, value.value).apply() }

	val videoPresenterLeadKey get() = resources.getString(R.string.preferences_video_presenter_lead_key)
	var videoPresenterLead
		get() = sharedPreferences.getString(videoPresenterLeadKey, videoPresenterLeadDefault.value)?.let { value ->
			VideoPresenterLead.values().firstOrNull { it.value == value }
		} ?: videoPresenterLeadDefault
		set(value) { sharedPreferences.edit().putString(videoPresenterLeadKey, value.value).apply() }

	val videoRecoveryStrategyKey get() = resources.getString(R.string.preferences_video_recovery_strategy_key)
	var videoRecoveryStrategy
		get() = sharedPreferences.getString(videoRecoveryStrategyKey, videoRecoveryStrategyDefault.value)?.let { value ->
			VideoRecoveryStrategy.values().firstOrNull { it.value == value }
		} ?: videoRecoveryStrategyDefault
		set(value) { sharedPreferences.edit().putString(videoRecoveryStrategyKey, value.value).apply() }

	fun validateVideoPacingMaxFrameAgePeriods(periods: Int) = max(1, min(10, periods))
	val videoPacingMaxFrameAgePeriodsKey get() = resources.getString(R.string.preferences_video_pacing_max_frame_age_periods_key)
	var videoPacingMaxFrameAgePeriods
		get() = validateVideoPacingMaxFrameAgePeriods(sharedPreferences.getInt(
			videoPacingMaxFrameAgePeriodsKey, videoPacingMaxFrameAgePeriodsDefault))
		set(value) { sharedPreferences.edit().putInt(videoPacingMaxFrameAgePeriodsKey,
			validateVideoPacingMaxFrameAgePeriods(value)).apply() }

	fun validateVideoDejitterDepthMs(depthMs: Int) = max(1, min(256, depthMs))
	val videoDejitterFloorMsKey get() = resources.getString(R.string.preferences_video_dejitter_floor_ms_key)
	var videoDejitterFloorMs
		get() = validateVideoDejitterDepthMs(sharedPreferences.getInt(
			videoDejitterFloorMsKey, videoDejitterFloorMsDefault))
		set(value) { sharedPreferences.edit().putInt(videoDejitterFloorMsKey,
			validateVideoDejitterDepthMs(value)).apply() }

	val videoDejitterCapMsKey get() = resources.getString(R.string.preferences_video_dejitter_cap_ms_key)
	var videoDejitterCapMs
		get() = validateVideoDejitterDepthMs(sharedPreferences.getInt(
			videoDejitterCapMsKey, videoDejitterCapMsDefault))
		set(value) { sharedPreferences.edit().putInt(videoDejitterCapMsKey,
			validateVideoDejitterDepthMs(value)).apply() }

	fun validateVideoDejitterQueueAgeFrames(frames: Int) = max(0, min(10, frames))
	val videoDejitterQueueAgeFramesKey get() = resources.getString(R.string.preferences_video_dejitter_queue_age_frames_key)
	var videoDejitterQueueAgeFrames
		get() = validateVideoDejitterQueueAgeFrames(sharedPreferences.getInt(
			videoDejitterQueueAgeFramesKey, videoDejitterQueueAgeFramesDefault))
		set(value) { sharedPreferences.edit().putInt(videoDejitterQueueAgeFramesKey,
			validateVideoDejitterQueueAgeFrames(value)).apply() }

	val videoPresenterConfig get(): AndroidChiakiVideoPresenterConfig
	{
		val capMs = videoDejitterCapMs
		val floorMs = min(videoDejitterFloorMs, capMs)
		return AndroidChiakiVideoPresenterConfig(
		pacingEnabled = videoPacingEnabled,
		pacingHighRefreshEnabled = videoPacingHighRefreshEnabled,
		pacingMode = videoPacingMode.nativeValue,
		presenterLead = videoPresenterLead.nativeValue,
		boundedAgeEnabled = videoPacingBoundedAgeEnabled,
		maxFrameAgePeriods = videoPacingMaxFrameAgePeriods,
		nonblockingProducer = videoPresenterNonblockingProducer,
		recoveryStrategy = videoRecoveryStrategy.nativeValue,
		dejitterEnabled = videoDejitterEnabled,
		dejitterFloorMs = floorMs,
		dejitterCapMs = capMs,
		dejitterQueueAgeFrames = videoDejitterQueueAgeFrames,
		dejitterHalfRateEnabled = videoDejitterHalfRateEnabled
		)
	}

	fun validateBitrate(bitrate: Int) = max(2000, min(100000, bitrate))
	val bitrateKey get() = resources.getString(R.string.preferences_bitrate_key)
	var bitrate
		get() = sharedPreferences.getInt(bitrateKey, 0).let { if(it == 0) null else validateBitrate(it) }
		set(value) { sharedPreferences.edit().putInt(bitrateKey, if(value != null) validateBitrate(value) else 0).apply() }
	val bitrateAuto get() = videoProfileDefaultBitrate.bitrate
	private val _bitrateAutoFlow by lazy { MutableStateFlow(bitrateAuto) }
	val bitrateAutoFlow: StateFlow<Int> get() = _bitrateAutoFlow.asStateFlow()

	fun validatePacketLossMaxPercent(percent: Int) = max(0, min(100, percent))
	val packetLossMaxPercentKey get() = resources.getString(R.string.preferences_packet_loss_max_percent_key)
	var packetLossMaxPercent
		get() = validatePacketLossMaxPercent(sharedPreferences.getInt(packetLossMaxPercentKey, packetLossMaxPercentDefault))
		set(value) { sharedPreferences.edit().putInt(packetLossMaxPercentKey, validatePacketLossMaxPercent(value)).apply() }
	val packetLossMax get() = packetLossMaxPercent / 100.0

	val adaptiveLossReportKey get() = resources.getString(R.string.preferences_adaptive_loss_report_key)
	var adaptiveLossReport
		get() = sharedPreferences.getBoolean(adaptiveLossReportKey, false)
		set(value) { sharedPreferences.edit().putBoolean(adaptiveLossReportKey, value).apply() }

	// PLE-75 / PLE-167 decoder experiments; 0 / false = the codec's own defaults.
	// PLE-116: ceiling 4000 so the 960 / 1920 sweep is reachable (1000 silently clamped 1920 before).
	fun validateDecoderOperatingRate(rate: Int) = max(0, min(4000, rate))
	val decoderOperatingRateKey get() = resources.getString(R.string.preferences_decoder_operating_rate_key)
	var decoderOperatingRate
		get() = validateDecoderOperatingRate(sharedPreferences.getInt(decoderOperatingRateKey, 0))
		set(value) { sharedPreferences.edit().putInt(decoderOperatingRateKey, validateDecoderOperatingRate(value)).apply() }

	val decoderOperatingRateDefaultKey get() = resources.getString(R.string.preferences_decoder_operating_rate_default_key)
	var decoderOperatingRateDefault
		get() = sharedPreferences.getBoolean(decoderOperatingRateDefaultKey, true)
		set(value) { sharedPreferences.edit().putBoolean(decoderOperatingRateDefaultKey, value).apply() }

	// PLE-635: Qualcomm vt-low-latency on the Oculus Go (GoDecoderProfile); off keeps today's
	// display-order decoder output. Ignored on every other device.
	val decoderQcomVtLowLatencyKey get() = resources.getString(R.string.preferences_decoder_qcom_vt_low_latency_key)
	var decoderQcomVtLowLatency
		get() = sharedPreferences.getBoolean(decoderQcomVtLowLatencyKey, false)
		set(value) { sharedPreferences.edit().putBoolean(decoderQcomVtLowLatencyKey, value).apply() }

	// PLE-75: with frame-index timestamps on and no explicit operating rate, request 960 (default on;
	// off reproduces the 14 ms decode latency of AB round 2).
	val decoderOperatingRateAutoKey get() = resources.getString(R.string.preferences_decoder_operating_rate_auto_key)
	var decoderOperatingRateAuto
		get() = sharedPreferences.getBoolean(decoderOperatingRateAutoKey, true)
		set(value) { sharedPreferences.edit().putBoolean(decoderOperatingRateAutoKey, value).apply() }

	val decoderRealtimePriorityKey get() = resources.getString(R.string.preferences_decoder_realtime_priority_key)
	var decoderRealtimePriority
		get() = sharedPreferences.getBoolean(decoderRealtimePriorityKey, false)
		set(value) { sharedPreferences.edit().putBoolean(decoderRealtimePriorityKey, value).apply() }

	fun validateVideoTimestampRateHz(rate: Int) = max(0, min(100000, rate))
	val videoTimestampRateHzKey get() = resources.getString(R.string.preferences_video_timestamp_rate_hz_key)
	var videoTimestampRateHz
		get() = validateVideoTimestampRateHz(sharedPreferences.getInt(videoTimestampRateHzKey, 0))
		set(value) { sharedPreferences.edit().putInt(videoTimestampRateHzKey, validateVideoTimestampRateHz(value)).apply() }

	val codecKey get() = resources.getString(R.string.preferences_codec_key)
	var codec
		get() = sharedPreferences.getString(codecKey, codecDefault.value)?.let { value ->
			Codec.values().firstOrNull { it.value == value }
		}  ?: codecDefault
		set(value) { sharedPreferences.edit().putString(codecKey, value.value).apply() }

	val streamQualityPresetKey get() = resources.getString(R.string.preferences_stream_quality_preset_key)
	var streamQualityPreset
		get() = sharedPreferences.getString(streamQualityPresetKey, streamQualityPresetDefault.value)?.let { value ->
			StreamQualityPreset.values().firstOrNull { it.value == value }
		} ?: streamQualityPresetDefault
		private set(value) { sharedPreferences.edit().putString(streamQualityPresetKey, value.value).apply() }

	/**
	 * Applies only the choices represented by a named preset. Experimental pacing, network, input,
	 * and recovery settings remain untouched so selecting a quality preset cannot silently opt into
	 * an unproven experiment.
	 */
	fun applyStreamQualityPreset(preset: StreamQualityPreset)
	{
		sharedPreferences.edit()
			.putString(streamQualityPresetKey, preset.value)
			.putString(resolutionKey, preset.resolution.value)
			.putString(fpsKey, preset.fps.value)
			.putString(codecKey, preset.codec.value)
			.putInt(bitrateKey, preset.bitrate ?: 0)
			.putBoolean(decoderOperatingRateDefaultKey, preset.decoderOperatingRateDefault)
			.putBoolean(decoderOperatingRateAutoKey, preset.decoderOperatingRateAuto)
			.putInt(decoderOperatingRateKey, preset.decoderOperatingRate)
			.putBoolean(decoderInputThreadEnabledKey, preset.decoderInputThreadEnabled)
			.putBoolean(debandingEnabledKey, preset.debandingEnabled)
			.apply()
	}

	private val videoProfileDefaultBitrate get() = ConnectVideoProfile.preset(resolution.preset, fps.preset, codec.codec)
	val videoProfile get() = videoProfileDefaultBitrate.let {
		val bitrate = bitrate
		if(bitrate == null)
			it
		else
			it.copy(bitrate = bitrate)
	}

	/**
	 * The session settings for a stream from [registeredHost] at [host] ("" when the PSN remote path
	 * picks the address). One builder for Home's local and PSN connects and the Go Library entry (PLE-690).
	 */
	fun connectInfo(host: String, registeredHost: RegisteredHost?, ps5: Boolean = true, autoRegister: Boolean = false) = ConnectInfo(
		ps5 = ps5,
		host = host,
		registKey = registeredHost?.rpRegistKey ?: ByteArray(16),
		morning = registeredHost?.rpKey ?: ByteArray(16),
		videoProfile = videoProfile,
		decoderLowLatencyEnabled = decoderLowLatencyEnabled,
		threadPriorityBoostEnabled = threadPriorityBoostEnabled,
		decoderLateFrameRecoveryEnabled = decoderLateFrameRecoveryEnabled,
		packetLossMax = packetLossMax,
		adaptiveLossReport = adaptiveLossReport,
		takionVideoPacketReorderingDisabled = takionVideoPacketReorderingDisabled,
		feedbackStateMinIntervalMs = if(feedbackReducedIntervalEnabled) 4 else 0,
		feedbackStatsLogIntervalMs = feedbackStatsLogIntervalMs,
		audioBufferBursts = audioBufferBursts,
		audioFifoMs = audioFifoMs,
		autoRegister = autoRegister,
		performanceModeEnabled = performanceModeEnabled,
		decoderOperatingRate = decoderOperatingRate,
		decoderOperatingRateDefault = decoderOperatingRateDefault,
		decoderOperatingRateAuto = decoderOperatingRateAuto,
		decoderRealtimePriority = decoderRealtimePriority,
		decoderQcomVtLowLatency = GoDecoderProfile.vtLowLatency(this),
		videoTimestampRateHz = videoTimestampRateHz,
		// The Home session summary consumes the same 1 Hz counters as the optional overlay.
		streamDiagnosticsEnabled = true,
		videoPresenterConfig = videoPresenterConfig
	)
}
