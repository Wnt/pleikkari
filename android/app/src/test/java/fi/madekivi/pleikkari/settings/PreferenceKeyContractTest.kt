// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.settings

import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

class PreferenceKeyContractTest
{
	@Test
	fun developerPreferenceResourceRetainsEveryLegacyKey()
	{
		val preferenceFile = File("src/main/res/xml/preferences.xml")
		val document = DocumentBuilderFactory.newInstance().apply {
			isNamespaceAware = true
		}.newDocumentBuilder().parse(preferenceFile)
		val nodes = document.documentElement.getElementsByTagName("*")
		val actual = buildSet {
			for(index in 0 until nodes.length)
			{
				val key = nodes.item(index).attributes
					?.getNamedItemNS("http://schemas.android.com/apk/res-auto", "key")
					?.nodeValue
				if(!key.isNullOrBlank())
					add(key)
			}
		}

		assertEquals(LEGACY_KEYS, actual)
	}

	private companion object
	{
		val LEGACY_KEYS = setOf(
			"@string/preferences_adaptive_loss_report_key",
			"@string/preferences_audio_buffer_bursts_key",
			"@string/preferences_audio_fifo_ms_key",
			"@string/preferences_bitrate_key",
			"@string/preferences_button_haptic_enabled_key",
			"@string/preferences_codec_key",
			"@string/preferences_controller_input_coalescing_enabled_key",
			"@string/preferences_debanding_key",
			"@string/preferences_debanding_render_when_dirty_key",
			"@string/preferences_decoder_input_thread_enabled_key",
			"@string/preferences_decoder_late_frame_recovery_enabled_key",
			"@string/preferences_decoder_low_latency_enabled_key",
			"@string/preferences_decoder_operating_rate_auto_key",
			"@string/preferences_decoder_operating_rate_default_key",
			"@string/preferences_decoder_operating_rate_key",
			"@string/preferences_decoder_qcom_vt_low_latency_key",
			"@string/preferences_decoder_realtime_priority_key",
			"@string/preferences_display_refresh_rate_key",
			"@string/preferences_export_settings_key",
			"@string/preferences_feedback_reduced_interval_enabled_key",
			"@string/preferences_feedback_stats_log_enabled_key",
			"@string/preferences_fps_key",
			"@string/preferences_gamepad_trigger_fallback_enabled_key",
			"@string/preferences_gamepad_unbuffered_dispatch_enabled_key",
			"@string/preferences_idr_on_fec_failure_enabled_key",
			"@string/preferences_import_settings_key",
			"@string/preferences_log_verbose_key",
			"@string/preferences_motion_enabled_key",
			"@string/preferences_packet_loss_max_percent_key",
			"@string/preferences_performance_mode_enabled_key",
			"@string/preferences_psn_remote_play_enabled_key",
			"@string/preferences_psn_sign_in_enabled_key",
			"@string/preferences_real_video_timestamps_key",
			"@string/preferences_resolution_key",
			"@string/preferences_rumble_enabled_key",
			"@string/preferences_senkusha_fallback_notice_enabled_key",
			"@string/preferences_stream_diagnostics_overlay_enabled_key",
			"@string/preferences_stream_end_cause_probe_enabled_key",
			"@string/preferences_swap_cross_moon_key",
			"@string/preferences_takion_video_packet_reordering_disabled_key",
			"@string/preferences_thread_priority_boost_enabled_key",
			"@string/preferences_video_dejitter_cap_ms_key",
			"@string/preferences_video_dejitter_enabled_key",
			"@string/preferences_video_dejitter_floor_ms_key",
			"@string/preferences_video_dejitter_half_rate_enabled_key",
			"@string/preferences_video_dejitter_queue_age_frames_key",
			"@string/preferences_video_pacing_bounded_age_enabled_key",
			"@string/preferences_video_pacing_enabled_key",
			"@string/preferences_video_pacing_high_refresh_enabled_key",
			"@string/preferences_video_pacing_max_frame_age_periods_key",
			"@string/preferences_video_pacing_mode_key",
			"@string/preferences_video_presenter_lead_key",
			"@string/preferences_video_presenter_nonblocking_producer_key",
			"@string/preferences_video_recovery_strategy_key",
			"@string/preferences_video_timestamp_rate_hz_key",
			"@string/preferences_wifi_low_latency_lock_enabled_key",
			"category_export",
			"category_general",
			"category_mapping",
			"category_stream",
			"category_video_deband",
			"logs",
			"mapping_circle",
			"mapping_cross",
			"mapping_l1",
			"mapping_l2",
			"mapping_l3",
			"mapping_options",
			"mapping_ps",
			"mapping_r1",
			"mapping_r2",
			"mapping_r3",
			"mapping_share",
			"mapping_square",
			"mapping_triangle",
			"preferences_coalesce_touch_redraw_enabled",
			"preferences_stream_window_optimizations_enabled",
			"preferences_touchscreen_touchpad_enabled",
			"registered_hosts"
		)
	}
}
