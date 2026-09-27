// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.settings

import android.app.Activity
import android.content.Intent
import android.content.res.Resources
import android.os.Bundle
import android.text.InputType
import androidx.lifecycle.Observer
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.preference.*
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.GoDecoderProfile
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.common.exportAndShareAllSettings
import fi.madekivi.pleikkari.common.ext.viewModelFactory
import fi.madekivi.pleikkari.common.getDatabase
import fi.madekivi.pleikkari.common.importSettingsFromUri
import fi.madekivi.pleikkari.stream.GoVrSupport
import fi.madekivi.pleikkari.stream.VideoTimestampSource
import fi.madekivi.pleikkari.stream.VrEnvironmentConfig
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import fi.madekivi.pleikkari.stream.VrEnvironmentSupport
import fi.madekivi.pleikkari.stream.videoTimestampSource

class DataStore(val preferences: Preferences): PreferenceDataStore()
{
	override fun getBoolean(key: String?, defValue: Boolean) = when(key)
	{
		preferences.goVrEnabledKey -> preferences.goVrEnabled
		// PLE-722: the Go-only switches below are added in code; without these they never stored a change.
		preferences.goVrUiKey -> preferences.goVrUi
		preferences.goVrMatch60HzKey -> preferences.goVrMatch60Hz
		preferences.goVrRoomHighGpuKey -> preferences.goVrRoomHighGpu
		preferences.goVrFrameListenerThreadKey -> preferences.goVrFrameListenerThread
		preferences.goVrLatchOnSignalKey -> preferences.goVrLatchOnSignal
		preferences.goVrLateStartKey -> preferences.goVrLateStart
		preferences.goVrWarmUpKey -> preferences.goVrWarmUp
		preferences.logVerboseKey -> preferences.logVerbose
		preferences.swapCrossMoonKey -> preferences.swapCrossMoon
		preferences.rumbleEnabledKey -> preferences.rumbleEnabled
		preferences.motionEnabledKey -> preferences.motionEnabled
		preferences.buttonHapticEnabledKey -> preferences.buttonHapticEnabled
		preferences.debandingEnabledKey -> preferences.debandingEnabled
		preferences.debandRenderWhenDirtyEnabledKey -> preferences.debandRenderWhenDirtyEnabled
		preferences.realVideoTimestampsKey -> preferences.realVideoTimestamps
		preferences.decoderLowLatencyEnabledKey -> preferences.decoderLowLatencyEnabled
		preferences.decoderOperatingRateDefaultKey -> preferences.decoderOperatingRateDefault
		preferences.decoderOperatingRateAutoKey -> preferences.decoderOperatingRateAuto
		preferences.decoderRealtimePriorityKey -> preferences.decoderRealtimePriority
		preferences.decoderQcomVtLowLatencyKey -> preferences.decoderQcomVtLowLatency
		preferences.feedbackReducedIntervalEnabledKey -> preferences.feedbackReducedIntervalEnabled
		preferences.feedbackStatsLogEnabledKey -> preferences.feedbackStatsLogEnabled
		preferences.streamEndCauseProbeEnabledKey -> preferences.streamEndCauseProbeEnabled
		preferences.decoderInputThreadEnabledKey -> preferences.decoderInputThreadEnabled
		preferences.threadPriorityBoostEnabledKey -> preferences.threadPriorityBoostEnabled
		preferences.takionVideoPacketReorderingDisabledKey -> preferences.takionVideoPacketReorderingDisabled
		preferences.adaptiveLossReportKey -> preferences.adaptiveLossReport
		preferences.decoderLateFrameRecoveryEnabledKey -> preferences.decoderLateFrameRecoveryEnabled
		preferences.videoPacingEnabledKey -> preferences.videoPacingEnabled
		preferences.videoPacingHighRefreshEnabledKey -> preferences.videoPacingHighRefreshEnabled
		preferences.videoDejitterEnabledKey -> preferences.videoDejitterEnabled
		preferences.videoPacingBoundedAgeEnabledKey -> preferences.videoPacingBoundedAgeEnabled
		preferences.videoPresenterNonblockingProducerKey -> preferences.videoPresenterNonblockingProducer
		preferences.controllerInputCoalescingEnabledKey -> preferences.controllerInputCoalescingEnabled
		preferences.gamepadUnbufferedDispatchEnabledKey -> preferences.gamepadUnbufferedDispatchEnabled
		preferences.gamepadTriggerFallbackEnabledKey -> preferences.gamepadTriggerFallbackEnabled
		preferences.touchscreenTouchpadEnabledKey -> preferences.touchscreenTouchpadEnabled
		preferences.coalesceTouchRedrawEnabledKey -> preferences.coalesceTouchRedrawEnabled
		preferences.streamWindowOptimizationsEnabledKey -> preferences.streamWindowOptimizationsEnabled
		preferences.psnSignInEnabledKey -> preferences.psnSignInEnabled
		preferences.psnRemotePlayEnabledKey -> preferences.psnRemotePlayEnabled
		preferences.performanceModeEnabledKey -> preferences.performanceModeEnabled
		preferences.wifiLowLatencyLockEnabledKey -> preferences.wifiLowLatencyLockEnabled
		preferences.videoDejitterHalfRateEnabledKey -> preferences.videoDejitterHalfRateEnabled
		preferences.streamDiagnosticsOverlayEnabledKey -> preferences.streamDiagnosticsOverlayEnabled
		else -> defValue
	}

	override fun putBoolean(key: String?, value: Boolean)
	{
		when(key)
		{
			preferences.goVrEnabledKey -> preferences.goVrEnabled = value
			preferences.goVrUiKey -> preferences.goVrUi = value
			preferences.goVrMatch60HzKey -> preferences.goVrMatch60Hz = value
			preferences.goVrRoomHighGpuKey -> preferences.goVrRoomHighGpu = value
			preferences.goVrFrameListenerThreadKey -> preferences.goVrFrameListenerThread = value
			preferences.goVrLatchOnSignalKey -> preferences.goVrLatchOnSignal = value
			preferences.goVrLateStartKey -> preferences.goVrLateStart = value
			preferences.goVrWarmUpKey -> preferences.goVrWarmUp = value
			preferences.logVerboseKey -> preferences.logVerbose = value
			preferences.swapCrossMoonKey -> preferences.swapCrossMoon = value
			preferences.rumbleEnabledKey -> preferences.rumbleEnabled = value
			preferences.motionEnabledKey -> preferences.motionEnabled = value
			preferences.buttonHapticEnabledKey -> preferences.buttonHapticEnabled = value
			preferences.debandingEnabledKey -> preferences.debandingEnabled = value
			preferences.debandRenderWhenDirtyEnabledKey -> preferences.debandRenderWhenDirtyEnabled = value
			preferences.realVideoTimestampsKey -> preferences.realVideoTimestamps = value
			preferences.decoderLowLatencyEnabledKey -> preferences.decoderLowLatencyEnabled = value
			preferences.decoderOperatingRateDefaultKey -> preferences.decoderOperatingRateDefault = value
			preferences.decoderOperatingRateAutoKey -> preferences.decoderOperatingRateAuto = value
			preferences.decoderRealtimePriorityKey -> preferences.decoderRealtimePriority = value
			preferences.decoderQcomVtLowLatencyKey -> preferences.decoderQcomVtLowLatency = value
			preferences.feedbackReducedIntervalEnabledKey -> preferences.feedbackReducedIntervalEnabled = value
			preferences.feedbackStatsLogEnabledKey -> preferences.feedbackStatsLogEnabled = value
			preferences.streamEndCauseProbeEnabledKey -> preferences.streamEndCauseProbeEnabled = value
			preferences.decoderInputThreadEnabledKey -> preferences.decoderInputThreadEnabled = value
			preferences.threadPriorityBoostEnabledKey -> preferences.threadPriorityBoostEnabled = value
			preferences.takionVideoPacketReorderingDisabledKey -> preferences.takionVideoPacketReorderingDisabled = value
			preferences.adaptiveLossReportKey -> preferences.adaptiveLossReport = value
			preferences.decoderLateFrameRecoveryEnabledKey -> preferences.decoderLateFrameRecoveryEnabled = value
			preferences.videoPacingEnabledKey -> preferences.videoPacingEnabled = value
			preferences.videoPacingHighRefreshEnabledKey -> preferences.videoPacingHighRefreshEnabled = value
			preferences.videoDejitterEnabledKey -> preferences.videoDejitterEnabled = value
			preferences.videoPacingBoundedAgeEnabledKey -> preferences.videoPacingBoundedAgeEnabled = value
			preferences.videoPresenterNonblockingProducerKey -> preferences.videoPresenterNonblockingProducer = value
			preferences.controllerInputCoalescingEnabledKey -> preferences.controllerInputCoalescingEnabled = value
			preferences.gamepadUnbufferedDispatchEnabledKey -> preferences.gamepadUnbufferedDispatchEnabled = value
			preferences.gamepadTriggerFallbackEnabledKey -> preferences.gamepadTriggerFallbackEnabled = value
			preferences.touchscreenTouchpadEnabledKey -> preferences.touchscreenTouchpadEnabled = value
			preferences.coalesceTouchRedrawEnabledKey -> preferences.coalesceTouchRedrawEnabled = value
			preferences.streamWindowOptimizationsEnabledKey -> preferences.streamWindowOptimizationsEnabled = value
			preferences.psnSignInEnabledKey -> preferences.psnSignInEnabled = value
			preferences.psnRemotePlayEnabledKey -> preferences.psnRemotePlayEnabled = value
			preferences.performanceModeEnabledKey -> preferences.performanceModeEnabled = value
			preferences.wifiLowLatencyLockEnabledKey -> preferences.wifiLowLatencyLockEnabled = value
			preferences.videoDejitterHalfRateEnabledKey -> preferences.videoDejitterHalfRateEnabled = value
			preferences.streamDiagnosticsOverlayEnabledKey -> preferences.streamDiagnosticsOverlayEnabled = value
		}
	}

	override fun getInt(key: String?, defValue: Int) = when(key)
	{
		preferences.vrScreenDistanceCmKey -> preferences.vrScreenDistanceCm
		preferences.vrScreenWidthCmKey -> preferences.vrScreenWidthCm
		preferences.vrScreenCurveRadiusCmKey -> preferences.vrScreenCurveRadiusCm
		preferences.vrScreenHeightOffsetCmKey -> preferences.vrScreenHeightOffsetCm
		preferences.vrGlowPercentKey -> preferences.vrGlowPercent
		preferences.vrRoomLightPercentKey -> preferences.vrRoomLightPercent
		else -> defValue
	}

	override fun putInt(key: String?, value: Int)
	{
		when(key)
		{
			preferences.vrScreenDistanceCmKey -> preferences.vrScreenDistanceCm = value
			preferences.vrScreenWidthCmKey -> preferences.vrScreenWidthCm = value
			preferences.vrScreenCurveRadiusCmKey -> preferences.vrScreenCurveRadiusCm = value
			preferences.vrScreenHeightOffsetCmKey -> preferences.vrScreenHeightOffsetCm = value
			preferences.vrGlowPercentKey -> preferences.vrGlowPercent = value
			preferences.vrRoomLightPercentKey -> preferences.vrRoomLightPercent = value
		}
	}

	override fun getString(key: String, defValue: String?) = when
	{
		key == preferences.resolutionKey -> preferences.resolution.value
		key == preferences.fpsKey -> preferences.fps.value
		key == preferences.displayRefreshRateModeKey -> preferences.displayRefreshRateMode.value
		key == preferences.videoPacingModeKey -> preferences.videoPacingMode.value
		key == preferences.videoPresenterLeadKey -> preferences.videoPresenterLead.value
		key == preferences.videoPacingMaxFrameAgePeriodsKey -> preferences.videoPacingMaxFrameAgePeriods.toString()
		key == preferences.videoDejitterFloorMsKey -> preferences.videoDejitterFloorMs.toString()
		key == preferences.videoDejitterCapMsKey -> preferences.videoDejitterCapMs.toString()
		key == preferences.videoDejitterQueueAgeFramesKey -> preferences.videoDejitterQueueAgeFrames.toString()
		key == preferences.videoRecoveryStrategyKey -> preferences.videoRecoveryStrategy.value
		key == preferences.bitrateKey -> preferences.bitrate?.toString() ?: ""
		key == preferences.packetLossMaxPercentKey -> preferences.packetLossMaxPercent.toString()
		key == preferences.decoderOperatingRateKey -> preferences.decoderOperatingRate.toString()
		key == preferences.videoTimestampRateHzKey -> preferences.videoTimestampRateHz.toString()
		key == preferences.audioBufferBurstsKey -> preferences.audioBufferBursts.toString()
		key == preferences.audioFifoMsKey -> preferences.audioFifoMs.toString()
		key == preferences.codecKey -> preferences.codec.value
		key == preferences.vrEnvironmentKey -> preferences.vrEnvironment.value
		key.startsWith("mapping_") -> preferences.sharedPreferences.getString(key, defValue)
		else -> defValue
	}

	override fun putString(key: String, value: String?)
	{
		when
		{
			key == preferences.resolutionKey ->
			{
				val resolution = Preferences.Resolution.values().firstOrNull { it.value == value } ?: return
				preferences.resolution = resolution
			}
			key == preferences.fpsKey ->
			{
				val fps = Preferences.FPS.values().firstOrNull { it.value == value } ?: return
				preferences.fps = fps
			}
			key == preferences.displayRefreshRateModeKey ->
			{
				val mode = Preferences.DisplayRefreshRateMode.values().firstOrNull { it.value == value } ?: return
				preferences.displayRefreshRateMode = mode
			}
			key == preferences.videoPacingModeKey ->
			{
				val mode = Preferences.VideoPacingMode.values().firstOrNull { it.value == value } ?: return
				preferences.videoPacingMode = mode
			}
			key == preferences.videoPresenterLeadKey ->
			{
				val lead = Preferences.VideoPresenterLead.values().firstOrNull { it.value == value } ?: return
				preferences.videoPresenterLead = lead
			}
			key == preferences.videoRecoveryStrategyKey ->
			{
				val strategy = Preferences.VideoRecoveryStrategy.values().firstOrNull { it.value == value } ?: return
				preferences.videoRecoveryStrategy = strategy
			}
			key == preferences.videoPacingMaxFrameAgePeriodsKey ->
			{
				value?.toIntOrNull()?.let { preferences.videoPacingMaxFrameAgePeriods = it }
			}
			key == preferences.videoDejitterFloorMsKey ->
			{
				value?.toIntOrNull()?.let { preferences.videoDejitterFloorMs = it }
			}
			key == preferences.videoDejitterCapMsKey ->
			{
				value?.toIntOrNull()?.let { preferences.videoDejitterCapMs = it }
			}
			key == preferences.videoDejitterQueueAgeFramesKey ->
			{
				value?.toIntOrNull()?.let { preferences.videoDejitterQueueAgeFrames = it }
			}
			key == preferences.bitrateKey -> preferences.bitrate = value?.toIntOrNull()
			key == preferences.packetLossMaxPercentKey ->
			{
				value?.toIntOrNull()?.let { preferences.packetLossMaxPercent = it }
			}
			key == preferences.decoderOperatingRateKey ->
			{
				value?.toIntOrNull()?.let { preferences.decoderOperatingRate = it }
			}
			key == preferences.videoTimestampRateHzKey ->
			{
				value?.toIntOrNull()?.let { preferences.videoTimestampRateHz = it }
			}
			key == preferences.audioBufferBurstsKey ->
			{
				value?.toIntOrNull()?.let { preferences.audioBufferBursts = it }
			}
			key == preferences.audioFifoMsKey ->
			{
				value?.toIntOrNull()?.let { preferences.audioFifoMs = it }
			}
			key == preferences.codecKey ->
			{
				val codec = Preferences.Codec.values().firstOrNull { it.value == value } ?: return
				preferences.codec = codec
			}
			key == preferences.vrEnvironmentKey -> preferences.vrEnvironment = VrEnvironmentKind.fromValue(value)
			key.startsWith("mapping_") -> 
			{
				preferences.sharedPreferences.edit().putString(key, value).apply()
			}
		}
	}
}

open class SettingsFragment: PreferenceFragmentCompat(), TitleFragment
{
	companion object
	{
		private const val PICK_SETTINGS_JSON_REQUEST = 1
	}

	protected open val preferenceResource = R.xml.preferences_user

	override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?)
	{
		val context = context ?: return

		val viewModel = ViewModelProvider(this, viewModelFactory { SettingsViewModel(getDatabase(context), Preferences(context)) })
			.get(SettingsViewModel::class.java)

		val preferences = viewModel.preferences
		preferenceManager.preferenceDataStore = DataStore(preferences)
		setPreferencesFromResource(preferenceResource, rootKey)
		if(preferenceResource == R.xml.preferences_user && GoVrSupport.available())
		{
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrEnabledKey
				title = getString(R.string.go_vr_title)
				summary = getString(R.string.go_vr_summary)
				isChecked = preferences.goVrEnabled
				// PLE-690: the Library's launch follows the switch (off: Oculus TV, as before).
				setOnPreferenceChangeListener { _, value ->
					GoVrSupport.syncLibraryEntry(context, value as Boolean)
					true
				}
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrUiKey
				title = getString(R.string.go_vr_ui_title)
				summary = getString(R.string.go_vr_ui_summary)
				isChecked = preferences.goVrUi
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrMatch60HzKey
				title = getString(R.string.go_vr_match_60hz_title)
				summary = getString(R.string.go_vr_match_60hz_summary)
				isChecked = preferences.goVrMatch60Hz
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrRoomHighGpuKey
				title = getString(R.string.go_vr_room_high_gpu_title)
				summary = getString(R.string.go_vr_room_high_gpu_summary)
				isChecked = preferences.goVrRoomHighGpu
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrFrameListenerThreadKey
				title = getString(R.string.go_vr_frame_listener_thread_title)
				summary = getString(R.string.go_vr_frame_listener_thread_summary)
				isChecked = preferences.goVrFrameListenerThread
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrLateStartKey
				title = getString(R.string.go_vr_late_start_title)
				summary = getString(R.string.go_vr_late_start_summary)
				isChecked = preferences.goVrLateStart
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrLatchOnSignalKey
				title = getString(R.string.go_vr_latch_on_signal_title)
				summary = getString(R.string.go_vr_latch_on_signal_summary)
				isChecked = preferences.goVrLatchOnSignal
			})
			preferenceScreen.addPreference(SwitchPreferenceCompat(context).apply {
				key = preferences.goVrWarmUpKey
				title = getString(R.string.go_vr_warm_up_title)
				summary = getString(R.string.go_vr_warm_up_summary)
				isChecked = preferences.goVrWarmUp
			})
		}

		preferenceScreen.findPreference<Preference>(preferences.decoderQcomVtLowLatencyKey)?.isVisible =
			GoDecoderProfile.eligible()

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_resolution_key))?.let {
			it.entryValues = Preferences.resolutionAll.map { res -> res.value }.toTypedArray()
			it.entries = Preferences.resolutionAll.map { res -> getString(res.title) }.toTypedArray()
		}

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_fps_key))?.let {
			it.entryValues = Preferences.fpsAll.map { fps -> fps.value }.toTypedArray()
			it.entries = Preferences.fpsAll.map { fps -> getString(fps.title) }.toTypedArray()
		}

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_display_refresh_rate_key))?.let {
			it.entryValues = Preferences.displayRefreshRateModeAll.map { mode -> mode.value }.toTypedArray()
			it.entries = Preferences.displayRefreshRateModeAll.map { mode -> getString(mode.title) }.toTypedArray()
		}

		preferenceScreen.findPreference<SwitchPreference>(getString(R.string.preferences_real_video_timestamps_key))?.let {
			it.summaryProvider = Preference.SummaryProvider<SwitchPreference> {
				when(videoTimestampSource(preferences.realVideoTimestamps, preferences.videoPacingEnabled))
				{
					VideoTimestampSource.FRAME_INDEX ->
						getString(R.string.preferences_real_video_timestamps_summary_frame_index)
					VideoTimestampSource.SYNTHETIC_COUNTER ->
						getString(R.string.preferences_real_video_timestamps_summary_synthetic)
				}
			}
		}

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_video_pacing_mode_key))?.let {
			it.entryValues = Preferences.videoPacingModeAll.map { mode -> mode.value }.toTypedArray()
			it.entries = Preferences.videoPacingModeAll.map { mode -> getString(mode.title) }.toTypedArray()
		}

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_video_presenter_lead_key))?.let {
			it.entryValues = Preferences.videoPresenterLeadAll.map { lead -> lead.value }.toTypedArray()
			it.entries = Preferences.videoPresenterLeadAll.map { lead -> getString(lead.title) }.toTypedArray()
		}

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_video_recovery_strategy_key))?.let {
			it.entryValues = Preferences.videoRecoveryStrategyAll.map { strategy -> strategy.value }.toTypedArray()
			it.entries = Preferences.videoRecoveryStrategyAll.map { strategy -> getString(strategy.title) }.toTypedArray()
		}

		preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_video_pacing_max_frame_age_periods_key))?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				getString(R.string.preferences_video_pacing_max_frame_age_periods_value,
					preferences.videoPacingMaxFrameAgePeriods)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.videoPacingMaxFrameAgePeriods.toString())
			}
		}

		listOf(
			R.string.preferences_video_dejitter_floor_ms_key to { preferences.videoDejitterFloorMs },
			R.string.preferences_video_dejitter_cap_ms_key to { preferences.videoDejitterCapMs }
		).forEach { (key, value) ->
			preferenceScreen.findPreference<EditTextPreference>(getString(key))?.let {
				it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
					getString(R.string.preferences_video_dejitter_depth_ms_value, value())
				}
				it.setOnBindEditTextListener { editText ->
					editText.inputType = InputType.TYPE_CLASS_NUMBER
					editText.setText(value().toString())
				}
			}
		}

		preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_video_dejitter_queue_age_frames_key))?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				getString(R.string.preferences_video_dejitter_queue_age_frames_value,
					preferences.videoDejitterQueueAgeFrames)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.videoDejitterQueueAgeFrames.toString())
			}
		}

		val bitratePreference = preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_bitrate_key))
		val bitrateSummaryProvider = Preference.SummaryProvider<EditTextPreference> {
			preferences.bitrate?.toString() ?: getString(R.string.preferences_bitrate_auto, preferences.bitrateAuto)
		}
		bitratePreference?.let {
			it.summaryProvider = bitrateSummaryProvider
			it.setOnBindEditTextListener { editText ->
				editText.hint = getString(R.string.preferences_bitrate_auto, preferences.bitrateAuto)
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.bitrate?.toString() ?: "")
			}
		}
		viewModel.bitrateAuto.observe(this, Observer {
			bitratePreference?.summaryProvider = bitrateSummaryProvider
		})

		val packetLossMaxPercentPreference = preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_packet_loss_max_percent_key))
		packetLossMaxPercentPreference?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				getString(R.string.preferences_packet_loss_max_percent_value, preferences.packetLossMaxPercent)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.packetLossMaxPercent.toString())
			}
		}

		preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_decoder_operating_rate_key))?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				getString(R.string.preferences_decoder_operating_rate_value, preferences.decoderOperatingRate)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.decoderOperatingRate.toString())
			}
		}

		preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_video_timestamp_rate_hz_key))?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				getString(R.string.preferences_video_timestamp_rate_hz_value, preferences.videoTimestampRateHz)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.videoTimestampRateHz.toString())
			}
		}

		val audioBufferBurstsPreference = preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_audio_buffer_bursts_key))
		audioBufferBurstsPreference?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				if(preferences.audioBufferBursts == 0) getString(R.string.preferences_audio_buffer_bursts_default)
				else getString(R.string.preferences_audio_buffer_bursts_value, preferences.audioBufferBursts)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.audioBufferBursts.toString())
			}
		}

		val audioFifoMsPreference = preferenceScreen.findPreference<EditTextPreference>(getString(R.string.preferences_audio_fifo_ms_key))
		audioFifoMsPreference?.let {
			it.summaryProvider = Preference.SummaryProvider<EditTextPreference> {
				getString(R.string.preferences_audio_fifo_ms_value, preferences.audioFifoMs)
			}
			it.setOnBindEditTextListener { editText ->
				editText.inputType = InputType.TYPE_CLASS_NUMBER
				editText.setText(preferences.audioFifoMs.toString())
			}
		}

		if(preferenceResource == R.xml.preferences_user && VrEnvironmentSupport.offered())
			addVrEnvironmentPreferences(preferences)

		preferenceScreen.findPreference<ListPreference>(getString(R.string.preferences_codec_key))?.let {
			it.entryValues = Preferences.codecAll.map { codec -> codec.value }.toTypedArray()
			it.entries = Preferences.codecAll.map { codec -> getString(codec.title) }.toTypedArray()
		}

		val registeredHostsPreference = preferenceScreen.findPreference<Preference>("registered_hosts")
		viewModel.registeredHostsCount.observe(this, Observer {
			registeredHostsPreference?.summary = getString(R.string.preferences_registered_hosts_summary, it)
		})

		preferenceScreen.findPreference<Preference>(getString(R.string.preferences_export_settings_key))?.setOnPreferenceClickListener { exportSettings(); true }
		preferenceScreen.findPreference<Preference>(getString(R.string.preferences_import_settings_key))?.setOnPreferenceClickListener { importSettings(); true }

		onPreferencesCreated(preferences)
	}

	/**
	 * PLE-603: the Oculus Go cinema's environment picker and screen geometry. Built in code
	 * rather than XML because the category exists only where the VR activity does
	 * (Build.DEVICE == "pacific"); the phone's settings screen is unchanged.
	 */
	private fun addVrEnvironmentPreferences(preferences: Preferences)
	{
		val context = context ?: return
		val category = PreferenceCategory(context).apply {
			key = "category_vr_environment"
			title = getString(R.string.preferences_category_title_vr_environment)
		}
		preferenceScreen.addPreference(category)
		category.addPreference(ListPreference(context).apply {
			key = preferences.vrEnvironmentKey
			title = getString(R.string.preferences_vr_environment_title)
			dialogTitle = title
			entryValues = VrEnvironmentKind.values().map { it.value }.toTypedArray()
			entries = VrEnvironmentKind.values().map { getString(it.title) }.toTypedArray()
			setDefaultValue(VrEnvironmentKind.default.value)
			summaryProvider = ListPreference.SimpleSummaryProvider.getInstance()
		})
		fun slider(keyName: String, titleRes: Int, minValue: Int, maxValue: Int, defaultValue: Int) =
			SeekBarPreference(context).apply {
				key = keyName
				title = getString(titleRes)
				min = minValue
				max = maxValue
				seekBarIncrement = if(maxValue - minValue > 200) 10 else 1
				showSeekBarValue = true
				isAdjustable = true
				setDefaultValue(defaultValue)
			}
		category.addPreference(slider(preferences.vrScreenDistanceCmKey, R.string.preferences_vr_screen_distance_title,
			VrEnvironmentConfig.SCREEN_DISTANCE_CM_MIN, VrEnvironmentConfig.SCREEN_DISTANCE_CM_MAX, VrEnvironmentConfig.SCREEN_DISTANCE_CM_DEFAULT))
		category.addPreference(slider(preferences.vrScreenWidthCmKey, R.string.preferences_vr_screen_width_title,
			VrEnvironmentConfig.SCREEN_WIDTH_CM_MIN, VrEnvironmentConfig.SCREEN_WIDTH_CM_MAX, VrEnvironmentConfig.SCREEN_WIDTH_CM_DEFAULT))
		category.addPreference(slider(preferences.vrScreenCurveRadiusCmKey, R.string.preferences_vr_screen_curve_radius_title,
			0, VrEnvironmentConfig.SCREEN_CURVE_RADIUS_CM_MAX, VrEnvironmentConfig.SCREEN_CURVE_RADIUS_CM_DEFAULT))
		category.addPreference(slider(preferences.vrScreenHeightOffsetCmKey, R.string.preferences_vr_screen_height_offset_title,
			-VrEnvironmentConfig.SCREEN_HEIGHT_OFFSET_CM_MAX, VrEnvironmentConfig.SCREEN_HEIGHT_OFFSET_CM_MAX, VrEnvironmentConfig.SCREEN_HEIGHT_OFFSET_CM_DEFAULT))
		category.addPreference(slider(preferences.vrGlowPercentKey, R.string.preferences_vr_glow_title,
			0, 100, VrEnvironmentConfig.GLOW_PERCENT_DEFAULT))
		category.addPreference(slider(preferences.vrRoomLightPercentKey, R.string.preferences_vr_room_light_title,
			0, 100, VrEnvironmentConfig.ROOM_LIGHT_PERCENT_DEFAULT))
	}

	protected open fun onPreferencesCreated(preferences: Preferences)
	{
		var versionTapCount = 0
		preferenceScreen.findPreference<Preference>("about_version")?.let { versionPreference ->
			versionPreference.summary = BuildConfig.VERSION_NAME
			versionPreference.setOnPreferenceClickListener {
				versionTapCount++
				if(versionTapCount >= 5)
				{
					versionTapCount = 0
					(activity as? SettingsActivity)?.openDeveloperSettings()
				}
				true
			}
		}
	}

	open override fun getTitle(resources: Resources): String = resources.getString(R.string.title_settings)

	private fun exportSettings()
	{
		val activity = activity ?: return
		exportAndShareAllSettings(activity, lifecycleScope)
	}

	private fun importSettings()
	{
		val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
			addCategory(Intent.CATEGORY_OPENABLE)
			type = "application/json"
		}
		startActivityForResult(intent, PICK_SETTINGS_JSON_REQUEST)
	}

	override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?)
	{
		if(requestCode == PICK_SETTINGS_JSON_REQUEST && resultCode == Activity.RESULT_OK)
		{
			val activity = activity ?: return
			data?.data?.also {
				importSettingsFromUri(activity, it, lifecycleScope)
			}
		}
	}
}

class ControllerSettingsFragment: SettingsFragment()
{
	override val preferenceResource = R.xml.preferences_controller

	override fun getTitle(resources: Resources): String = resources.getString(R.string.title_controller_settings)
}

class ControllerMappingSettingsFragment: SettingsFragment()
{
	override val preferenceResource = R.xml.preferences_controller_mapping

	override fun getTitle(resources: Resources): String = resources.getString(R.string.title_controller_mapping)
}

class DeveloperSettingsFragment: SettingsFragment()
{
	override val preferenceResource = R.xml.preferences

	override fun onPreferencesCreated(preferences: Preferences)
	{
		listOf(
			"registered_hosts",
			getString(R.string.preferences_psn_sign_in_enabled_key),
			getString(R.string.preferences_swap_cross_moon_key),
			getString(R.string.preferences_rumble_enabled_key),
			preferences.touchscreenTouchpadEnabledKey,
			getString(R.string.preferences_motion_enabled_key),
			getString(R.string.preferences_button_haptic_enabled_key),
			"category_mapping"
		).forEach { key ->
			preferenceScreen.findPreference<Preference>(key)?.isVisible = false
		}
	}

	override fun getTitle(resources: Resources): String = resources.getString(R.string.title_developer_settings)
}
