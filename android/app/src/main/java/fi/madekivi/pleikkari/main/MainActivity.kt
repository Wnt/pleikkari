// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import android.app.Activity
import android.app.ActivityOptions
import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.util.Log
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.PopupMenu
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.IntentCompat
import androidx.lifecycle.ViewModelProvider
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.*
import fi.madekivi.pleikkari.common.ext.applySystemBarInsets
import fi.madekivi.pleikkari.common.ext.enableAppEdgeToEdge
import fi.madekivi.pleikkari.common.ext.putRevealExtra
import fi.madekivi.pleikkari.common.ext.viewModelFactory
import fi.madekivi.pleikkari.databinding.ActivityMainBinding
import fi.madekivi.pleikkari.discovery.serverMac
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.lib.DiscoveryHost
import fi.madekivi.pleikkari.manualconsole.EditManualConsoleActivity
import fi.madekivi.pleikkari.regist.PsnLoginActivity
import fi.madekivi.pleikkari.regist.RegistActivity
import fi.madekivi.pleikkari.remote.AndroidPsnRemoteClient
import fi.madekivi.pleikkari.remote.PsnDevice
import fi.madekivi.pleikkari.settings.SettingsActivity
import fi.madekivi.pleikkari.stream.GoVrSupport
import fi.madekivi.pleikkari.stream.StreamActivity
import fi.madekivi.pleikkari.stream.ConsoleDiscoveryProbe
import fi.madekivi.pleikkari.stream.StreamEndCause
import fi.madekivi.pleikkari.stream.StreamEndCauseClassifier
import fi.madekivi.pleikkari.stream.StreamEndReason
import fi.madekivi.pleikkari.stream.StreamSummary
import fi.madekivi.pleikkari.stream.StreamSummaryFormatter
import fi.madekivi.pleikkari.stream.StreamSummaryQuality

class MainActivity : AppCompatActivity()
{
	companion object
	{
		const val EXTRA_ONBOARDING_PREVIEW = "onboarding_preview"
		private const val PREVIEW_WELCOME = "welcome"
		private const val PREVIEW_CONSOLES = "consoles"
		private const val PREVIEW_SUMMARY = "summary"
	}

	private lateinit var viewModel: MainViewModel
	private lateinit var binding: ActivityMainBinding
	private lateinit var consoleAdapter: DisplayHostRecyclerViewAdapter
	private lateinit var preferences: Preferences
	private var discoveryMenuItem: MenuItem? = null
	private var localHosts: List<DisplayHost> = emptyList()
	private var psnConsoles: List<PsnConsole> = emptyList()
	private var configuredConsoleCount = -1
	private var pendingRegistrationHost: DisplayHost? = null
	/** An unlinked console found on the network, waiting for the account's console list to link it. */
	private var pendingLinkHost: DisplayHost? = null
	private var pendingAutoPlayAddress: String? = null
	private var pendingAutoPlayJustLinked = false
	private var previewState: String? = null

	private val psnListAllowed: Boolean get() = shouldLoadPsnConsoleList(
		preferences.psnRemotePlayEnabled,
		preferences.psnSignInEnabled,
		preferences.psnAccountId
	)

	private val streamLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
		val summary = result.data?.let {
			IntentCompat.getParcelableExtra(it, StreamActivity.EXTRA_STREAM_SUMMARY, StreamSummary::class.java)
		}
		if(summary != null)
			showStreamSummary(summary)
	}

	private val psnLoginLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
		if(result.resultCode == PsnLoginActivity.RESULT_LINK_WITH_PIN)
		{
			// PLE-323: sign-in did not finish and the user took the PIN link for the console here instead.
			val host = pendingRegistrationHost ?: pinLinkCandidate()
			pendingRegistrationHost = null
			if(host != null)
				showGuidedRegistration(host.host, host.name)
			return@registerForActivityResult
		}
		if(result.resultCode != Activity.RESULT_OK)
			return@registerForActivityResult
		preferences.psnSignInEnabled = true
		preferences.psnRemotePlayEnabled = true
		pendingRegistrationHost?.also { host ->
			pendingRegistrationHost = null
			pendingLinkHost = host
		}
		viewModel.setPsnEnabled(true, psnListAllowed)
		linkPendingHost()
		updateHomeState()
	}

	private val registrationLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
		if(result.resultCode == Activity.RESULT_OK)
		{
			pendingAutoPlayAddress = result.data?.getStringExtra(RegistActivity.EXTRA_REGISTERED_HOST)
			// A console linked with a PIN needs the same moment to settle as a PSN-linked one.
			pendingAutoPlayJustLinked = true
			maybePlayRegisteredHost(localHosts)
		}
	}

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)
		enableAppEdgeToEdge()
		if(BuildConfig.DEBUG && intent.getBooleanExtra(StreamActivity.EXTRA_DIAGNOSTICS_PREVIEW, false))
		{
			startActivity(Intent(this, StreamActivity::class.java).apply {
				putExtra(StreamActivity.EXTRA_DIAGNOSTICS_PREVIEW, true)
			})
			finish()
			return
		}
		binding = ActivityMainBinding.inflate(layoutInflater)
		setContentView(binding.root)
		binding.root.applySystemBarInsets(top = false)
		binding.appBarLayout.applySystemBarInsets(left = false, right = false, bottom = false)
		binding.onboardingLayout.applySystemBarInsets(left = false, right = false, bottom = false)
		preferences = Preferences(this)
		previewState = intent.getStringExtra(EXTRA_ONBOARDING_PREVIEW)
			?.takeIf { BuildConfig.DEBUG && it in setOf(PREVIEW_WELCOME, PREVIEW_CONSOLES, PREVIEW_SUMMARY) }
		setSupportActionBar(binding.toolbar)
		setupQualityPresetChooser()

		binding.addConsoleButton.setOnClickListener { showAddConsoleMenu() }
		HomeActionLayout.install(binding)
		binding.onboardingSignInButton.setOnClickListener { startPsnSignIn() }
		binding.onboardingAddAddressButton.setOnClickListener {
			addManualConsole(binding.onboardingAddAddressButton)
		}
		binding.summaryDismissButton.setOnClickListener {
			binding.streamSummaryCard.visibility = View.GONE
		}
		binding.summaryDuration.labelTextView.setText(R.string.stream_summary_duration)
		binding.summaryLatency.labelTextView.setText(R.string.stream_summary_latency)
		binding.summaryDrops.labelTextView.setText(R.string.stream_summary_drops)
		binding.summaryQuality.labelTextView.setText(R.string.stream_summary_quality)

		viewModel = ViewModelProvider(this, viewModelFactory {
			MainViewModel(
				getDatabase(this),
				preferences,
				LogManager(this),
				AndroidPsnRemoteClient(this)
			)
		})[MainViewModel::class.java]
		if(previewState == PREVIEW_CONSOLES || previewState == PREVIEW_SUMMARY)
			showPreviewConsoles()
		if(previewState == PREVIEW_SUMMARY)
			showStreamSummary(StreamSummary(754_000L, 24.0, 2L, StreamSummaryQuality.GOOD))

		consoleAdapter = DisplayHostRecyclerViewAdapter(
			this::playConsole,
			this::wakeConsole,
			this::editConsole,
			this::deleteConsole
		)
		binding.hostsRecyclerView.adapter = consoleAdapter
		binding.hostsRecyclerView.layoutManager = LinearLayoutManager(this)
		viewModel.displayHosts.observe(this) {
			localHosts = it
			updateHomeState()
			maybePlayRegisteredHost(it)
		}
		viewModel.configuredConsoleCount.observe(this) { count ->
			configuredConsoleCount = count
			updateHomeState()
		}
		viewModel.discoveryActive.observe(this) { active ->
			discoveryMenuItem?.let { updateDiscoveryMenuItem(it, active) }
		}
		viewModel.psnConsoles.observe(this) {
			psnConsoles = it
			updateConsoleList()
		}
		viewModel.psnListState.observe(this) {
			updatePsnListState(it)
			updateHomeState()
			linkPendingHost()
		}
		viewModel.psnAction.observe(this) { consoleAdapter.action = it }
		viewModel.psnProgress.observe(this) { consoleAdapter.progress = it }
		viewModel.psnError.observe(this, this::showPsnActionError)
		viewModel.psnPlayRequest.observe(this) { request ->
			request ?: return@observe
			viewModel.clearPsnPlayRequest()
			val local = request.console.registeredHost?.let { readyLocalHost(it) }
			if(local != null)
				playLocalConsole(local, justLinked = request.justLinked)
			else
				connectPsnConsole(request.console, justLinked = request.justLinked)
		}
		updateHomeState()
	}

	private fun currentHomeState(): OnboardingHomeState = when(previewState)
	{
		PREVIEW_WELCOME -> OnboardingHomeState.WELCOME
		PREVIEW_CONSOLES, PREVIEW_SUMMARY -> OnboardingHomeState.ACCOUNT_CONSOLES
		else -> onboardingHomeState(
			configuredConsoleCount,
			psnListAllowed,
			localHosts.count { it is DiscoveredDisplayHost && it.isPS5 }
		)
	}

	private fun updateHomeState()
	{
		if(previewState == null && configuredConsoleCount < 0)
			return
		val state = currentHomeState()
		val welcome = state == OnboardingHomeState.WELCOME
		val accountConsoles = state == OnboardingHomeState.ACCOUNT_CONSOLES
		binding.onboardingLayout.visibility = if(welcome) View.VISIBLE else View.GONE
		binding.mainContentLayout.visibility = if(welcome) View.GONE else View.VISIBLE
		binding.appBarLayout.visibility = if(welcome) View.GONE else View.VISIBLE
		binding.addConsoleButton.visibility = if(welcome) View.GONE else View.VISIBLE
		if(accountConsoles)
		{
			binding.addConsoleButton.setText(R.string.action_add_by_address)
			binding.addConsoleButton.setOnClickListener { addManualConsole(binding.addConsoleButton) }
		}
		else if(!welcome)
		{
			binding.addConsoleButton.setText(R.string.action_add_console)
			binding.addConsoleButton.setOnClickListener { showAddConsoleMenu() }
		}
		updateConsoleList()
	}

	private fun setupQualityPresetChooser()
	{
		val preferences = Preferences(this)
		val checkedButton = when(preferences.streamQualityPreset)
		{
			Preferences.StreamQualityPreset.BALANCED -> R.id.qualityPresetBalancedButton
			Preferences.StreamQualityPreset.LOW_LATENCY -> R.id.qualityPresetLowLatencyButton
			Preferences.StreamQualityPreset.DATA_SAVER -> R.id.qualityPresetDataSaverButton
		}
		binding.qualityPresetToggleGroup.check(checkedButton)
		binding.qualityPresetBalancedButton.setOnClickListener {
			preferences.applyStreamQualityPreset(Preferences.StreamQualityPreset.BALANCED)
		}
		binding.qualityPresetLowLatencyButton.setOnClickListener {
			preferences.applyStreamQualityPreset(Preferences.StreamQualityPreset.LOW_LATENCY)
		}
		binding.qualityPresetDataSaverButton.setOnClickListener {
			preferences.applyStreamQualityPreset(Preferences.StreamQualityPreset.DATA_SAVER)
		}
	}

	private fun updateConsoleList()
	{
		if(!::consoleAdapter.isInitialized)
			return
		val atTop = binding.hostsRecyclerView.computeVerticalScrollOffset() == 0
		consoleAdapter.consoles = mergeHomeConsoles(localHosts, psnConsoles)
		if(atTop)
			binding.hostsRecyclerView.scrollToPosition(0)
		val listUnavailable = viewModel.psnListState.value is PsnConsoleListState.Error ||
			viewModel.psnListState.value == PsnConsoleListState.Loading
		binding.emptyInfoLayout.visibility =
			if(consoleAdapter.itemCount == 0 && !listUnavailable && currentHomeState() != OnboardingHomeState.WELCOME)
				View.VISIBLE else View.GONE
	}

	private fun updatePsnListState(state: PsnConsoleListState?)
	{
		binding.psnProgressLayout.visibility =
			if(state == PsnConsoleListState.Loading) View.VISIBLE else View.GONE
		val error = state as? PsnConsoleListState.Error
		binding.psnListErrorLayout.visibility = if(error == null) View.GONE else View.VISIBLE
		binding.psnConsolesInfoTextView.text = error?.message
		if(error != null)
		{
			binding.retryPsnListButton.setText(
				if(error.recovery == PsnErrorRecovery.SIGN_IN) R.string.action_psn_sign_in else R.string.action_retry
			)
			binding.retryPsnListButton.setOnClickListener {
				if(error.recovery == PsnErrorRecovery.SIGN_IN) startPsnSignIn() else viewModel.loadPsnConsoles()
			}
		}
		updateConsoleList()
	}

	private fun showPsnActionError(error: PsnActionError?)
	{
		val pinHost = viewModel.lastFailedPsnConsole?.takeIf { error != null }?.let(::unlinkedLocalHost)
		if(error != null && psnFailureStep(error.recovery, consoleOnLan = pinHost != null) == PsnFailureStep.LINK_WITH_PIN)
		{
			// PLE-313: the console is right here, so linking it with its PIN is the next step, not an error.
			viewModel.clearPsnError()
			showGuidedRegistration(pinHost!!.host, pinHost.name)
			return
		}
		binding.psnActionErrorLayout.visibility = if(error == null) View.GONE else View.VISIBLE
		binding.psnActionErrorTextView.text = error?.let { getString(R.string.psn_play_failed, it.message) }
		if(error != null)
		{
			binding.retryPsnActionButton.setText(
				if(error.recovery == PsnErrorRecovery.SIGN_IN) R.string.action_psn_sign_in else R.string.action_retry
			)
			binding.retryPsnActionButton.setOnClickListener {
				viewModel.clearPsnError()
				if(error.recovery == PsnErrorRecovery.SIGN_IN) startPsnSignIn()
				else viewModel.retryLastPsnAction()
			}
		}
	}

	private fun showStreamSummary(summary: StreamSummary)
	{
		binding.summaryDuration.valueTextView.text = StreamSummaryFormatter.duration(summary.durationMillis)
		binding.summaryLatency.valueTextView.text = StreamSummaryFormatter.latency(summary.averageLatencyMillis)
		binding.summaryDrops.valueTextView.text = summary.droppedFrames.toString()
		binding.summaryQuality.valueTextView.setText(when(summary.quality)
		{
			StreamSummaryQuality.GOOD -> R.string.network_quality_good
			StreamSummaryQuality.FAIR -> R.string.network_quality_fair
			StreamSummaryQuality.POOR -> R.string.network_quality_poor
			StreamSummaryQuality.UNKNOWN -> R.string.network_quality_unknown
		})
		binding.summaryEndCause.visibility = View.GONE
		binding.streamSummaryCard.visibility = View.VISIBLE
		// PLE-555: layout-land's mainContentLayout scrolls its top section within a fixed-height
		// Guideline split (PLE-549), so a newly-shown card can land below the visible viewport.
		// requestRectangleOnScreen bubbles up to whichever ancestor can scroll (a no-op if none can).
		binding.streamSummaryCard.post {
			binding.streamSummaryCard.requestRectangleOnScreen(
				Rect(0, 0, binding.streamSummaryCard.width, binding.streamSummaryCard.height), true
			)
		}
		summary.endReason?.let { explainStreamEnd(it) }
	}

	/**
	 * PLE-262: the console ended the stream. Ask it over discovery whether it is still awake
	 * (taken over on the TV) or went to rest, log the verdict for the A/B harness, and name it.
	 */
	private fun explainStreamEnd(reason: StreamEndReason)
	{
		if(!preferences.streamEndCauseProbeEnabled || !StreamEndCauseClassifier.needsProbe(reason))
			return
		Thread({
			val probes = ConsoleDiscoveryProbe.probeSeries(reason.host, reason.ps5)
			val cause = StreamEndCauseClassifier.classify(reason, probes)
			Log.i(StreamEndCauseClassifier.LOG_TAG, StreamEndCauseClassifier.logLine(cause, reason, probes))
			val message = when(cause)
			{
				StreamEndCause.CONSOLE_TAKEOVER -> R.string.stream_summary_end_console_takeover
				StreamEndCause.CONSOLE_REST -> R.string.stream_summary_end_console_rest
				else -> return@Thread
			}
			runOnUiThread {
				if(isDestroyed)
					return@runOnUiThread
				binding.summaryEndCause.setText(message)
				binding.summaryEndCause.visibility = View.VISIBLE
			}
		}, "StreamEndCauseProbe").start()
	}

	private fun showAddConsoleMenu()
	{
		PopupMenu(this, binding.addConsoleButton).also { menu ->
			menu.menuInflater.inflate(R.menu.add_console, menu.menu)
			menu.setOnMenuItemClickListener {
				when(it.itemId)
				{
					R.id.action_register -> showRegistration()
					R.id.action_add_manual -> addManualConsole(binding.addConsoleButton)
					else -> return@setOnMenuItemClickListener false
				}
				true
			}
			menu.show()
		}
	}

	override fun onStart()
	{
		super.onStart()
		if(previewState == null)
			viewModel.setPsnEnabled(preferences.psnRemotePlayEnabled, psnListAllowed)
		viewModel.discoveryManager.resume()
	}

	override fun onStop()
	{
		super.onStop()
		viewModel.discoveryManager.pause()
	}

	override fun onCreateOptionsMenu(menu: Menu): Boolean
	{
		menuInflater.inflate(R.menu.main, menu)
		discoveryMenuItem = menu.findItem(R.id.action_discover).also {
			updateDiscoveryMenuItem(it, viewModel.discoveryActive.value ?: false)
		}
		return true
	}

	private fun updateDiscoveryMenuItem(item: MenuItem, active: Boolean)
	{
		item.isChecked = active
		item.setIcon(if(active) R.drawable.ic_discover_on else R.drawable.ic_discover_off)
	}

	override fun onOptionsItemSelected(item: MenuItem): Boolean = when(item.itemId)
	{
		R.id.action_discover ->
		{
			viewModel.discoveryManager.active = !(viewModel.discoveryActive.value ?: false)
			true
		}
		R.id.action_settings ->
		{
			startActivity(Intent(this, SettingsActivity::class.java))
			true
		}
		else -> super.onOptionsItemSelected(item)
	}

	private fun addManualConsole(source: View)
	{
		Intent(this, EditManualConsoleActivity::class.java).also {
			it.putRevealExtra(source, binding.rootLayout)
			startActivity(it, ActivityOptions.makeSceneTransitionAnimation(this).toBundle())
		}
	}

	private fun showRegistration()
	{
		if(preferences.psnAccountId.isNullOrBlank())
		{
			startPsnSignIn()
			return
		}
		preferences.psnSignInEnabled = true
		preferences.psnRemotePlayEnabled = true
		viewModel.setPsnEnabled(true, psnListAllowed)
		updateHomeState()
	}

	private fun playConsole(console: HomeConsole)
	{
		val psn = console.psnConsole
		when
		{
			console.status == HomeConsoleStatus.REGISTRATION_REQUIRED && psn != null -> playPsnConsole(psn)
			console.displayHost != null -> playLocalConsole(console.displayHost)
			psn != null -> playPsnConsole(psn)
		}
	}

	private fun wakeConsole(console: HomeConsole)
	{
		console.displayHost?.let(::wakeupHost)
	}

	private fun playLocalConsole(host: DisplayHost, justLinked: Boolean = false)
	{
		val registeredHost = host.registeredHost
		if(registeredHost != null && !registeredHost.target.isPS5)
		{
			showUnsupportedRegistration(registeredHost)
			return
		}
		if(registeredHost == null)
		{
			if(host is DiscoveredDisplayHost && !host.isPS5)
			{
				showUnsupportedConsole(host.name ?: host.host)
				return
			}
			if(host is DiscoveredDisplayHost)
			{
				if(preferences.psnAccountId.isNullOrBlank())
				{
					pendingRegistrationHost = host
					startPsnSignIn()
				}
				else
				{
					pendingLinkHost = host
					if(!psnListAllowed)
					{
						preferences.psnSignInEnabled = true
						preferences.psnRemotePlayEnabled = true
					}
					viewModel.setPsnEnabled(true, psnListAllowed)
					linkPendingHost()
				}
			}
			else
				startLegacyRegistration(host)
			return
		}
		val connectInfo = ConnectInfo(
			ps5 = host.isPS5,
			host = host.host,
			registKey = registeredHost.rpRegistKey,
			morning = registeredHost.rpKey,
			videoProfile = preferences.videoProfile,
			decoderLowLatencyEnabled = preferences.decoderLowLatencyEnabled,
			threadPriorityBoostEnabled = preferences.threadPriorityBoostEnabled,
			decoderLateFrameRecoveryEnabled = preferences.decoderLateFrameRecoveryEnabled,
			packetLossMax = preferences.packetLossMax,
			adaptiveLossReport = preferences.adaptiveLossReport,
			takionVideoPacketReorderingDisabled = preferences.takionVideoPacketReorderingDisabled,
			feedbackStateMinIntervalMs = if(preferences.feedbackReducedIntervalEnabled) 4 else 0,
			feedbackStatsLogIntervalMs = preferences.feedbackStatsLogIntervalMs,
			audioBufferBursts = preferences.audioBufferBursts,
			audioFifoMs = preferences.audioFifoMs,
			performanceModeEnabled = preferences.performanceModeEnabled,
			decoderOperatingRate = preferences.decoderOperatingRate,
			decoderOperatingRateDefault = preferences.decoderOperatingRateDefault,
			decoderOperatingRateAuto = preferences.decoderOperatingRateAuto,
			decoderRealtimePriority = preferences.decoderRealtimePriority,
			decoderQcomVtLowLatency = GoDecoderProfile.vtLowLatency(preferences),
			videoTimestampRateHz = preferences.videoTimestampRateHz,
			streamDiagnosticsEnabled = true,
			videoPresenterConfig = preferences.videoPresenterConfig
		)
		streamLauncher.launch(GoVrSupport.streamIntent(this).apply {
			putExtra(StreamActivity.EXTRA_CONNECT_INFO, connectInfo)
			putExtra(StreamActivity.EXTRA_JUST_LINKED, justLinked)
		})
	}

	private fun startPsnSignIn()
	{
		if(previewState != null)
		{
			previewState = PREVIEW_CONSOLES
			showPreviewConsoles()
			updateHomeState()
			return
		}
		psnLoginLauncher.launch(Intent(this, PsnLoginActivity::class.java).apply {
			putExtra(PsnLoginActivity.EXTRA_OFFER_PIN_LINK, (pendingRegistrationHost ?: pinLinkCandidate()) != null)
		})
	}

	/** An unlinked PS5 on this network, which the PIN link can reach without signing in. */
	private fun pinLinkCandidate(): DisplayHost? = localHosts
		.filterIsInstance<DiscoveredDisplayHost>()
		.firstOrNull { it.registeredHost == null && it.isPS5 }

	private fun showPreviewConsoles()
	{
		viewModel.showPsnPreview(listOf(PsnDevice("preview-console", "Living Room PS5")))
	}

	private fun showUnsupportedRegistration(registeredHost: RegisteredHost)
	{
		MaterialAlertDialogBuilder(this)
			.setTitle(R.string.alert_title_ps4_unsupported)
			.setMessage(getString(R.string.alert_message_ps4_registration_unsupported, registeredHost.serverNickname ?: registeredHost.serverMac.toString()))
			.setPositiveButton(R.string.action_delete) { _, _ ->
				viewModel.deleteRegisteredHost(registeredHost)
			}
			.setNegativeButton(R.string.action_keep) { _, _ -> }
			.show()
	}

	private fun showUnsupportedConsole(name: String)
	{
		MaterialAlertDialogBuilder(this)
			.setTitle(R.string.alert_title_ps4_unsupported)
			.setMessage(getString(R.string.alert_message_ps4_console_unsupported, name))
			.setPositiveButton(android.R.string.ok) { _, _ -> }
			.show()
	}

	private fun showGuidedRegistration(host: String, name: String?)
	{
		registrationLauncher.launch(Intent(this, RegistActivity::class.java).apply {
			putExtra(RegistActivity.EXTRA_HOST, host)
			putExtra(RegistActivity.EXTRA_BROADCAST, false)
			putExtra(RegistActivity.EXTRA_CONSOLE_NAME, name)
			putExtra(RegistActivity.EXTRA_GUIDED, true)
			if(previewState != null)
				putExtra(RegistActivity.EXTRA_PREVIEW, true)
		})
	}

	private fun startLegacyRegistration(host: DisplayHost)
	{
		startActivity(Intent(this, RegistActivity::class.java).apply {
			putExtra(RegistActivity.EXTRA_HOST, host.host)
			putExtra(RegistActivity.EXTRA_BROADCAST, false)
			if(host is ManualDisplayHost)
				putExtra(RegistActivity.EXTRA_ASSIGN_MANUAL_HOST_ID, host.manualHost.id)
		})
	}

	private fun playPsnConsole(console: PsnConsole)
	{
		if(previewState != null)
		{
			showGuidedRegistration("192.0.2.1", console.device.name)
			return
		}
		viewModel.playPsnConsole(console)
	}

	/**
	 * Links the console the user tapped before signing in. The account knows the console, so it is
	 * registered over PSN with no PIN; only a console the account does not list asks for the PIN.
	 */
	private fun linkPendingHost()
	{
		val host = pendingLinkHost ?: return
		val listState = viewModel.psnListState.value
		if(listState == PsnConsoleListState.Loading || listState == PsnConsoleListState.Hidden && psnListAllowed)
			return
		pendingLinkHost = null
		val console = psnConsoleNamed(viewModel.currentPsnConsoles(), host.name)
		if(console != null)
			playPsnConsole(console)
		else
			showGuidedRegistration(host.host, host.name)
	}

	/** A console registered over PSN that is awake on this network streams locally, not through PSN. */
	private fun readyLocalHost(registeredHost: RegisteredHost): DisplayHost? = localHosts
		.filterIsInstance<DiscoveredDisplayHost>()
		.firstOrNull { host ->
			host.isPS5 && host.discoveredHost.state == DiscoveryHost.State.READY &&
				host.discoveredHost.serverMac == registeredHost.serverMac
		}
		?.let { DiscoveredDisplayHost(registeredHost, it.discoveredHost) }

	/** The console on this network behind a failed PSN link, which can still be linked with a PIN. */
	private fun unlinkedLocalHost(console: PsnConsole): DisplayHost? = localHosts
		.filterIsInstance<DiscoveredDisplayHost>()
		.firstOrNull { host ->
			host.registeredHost == null && host.isPS5 &&
				psnConsoleNamed(listOf(PsnConsole(console.device, null)), host.name) != null
		}

	private fun maybePlayRegisteredHost(hosts: List<DisplayHost>)
	{
		val address = pendingAutoPlayAddress ?: return
		val host = hosts.firstOrNull { it.host == address && it.registeredHost != null } ?: return
		pendingAutoPlayAddress = null
		val justLinked = pendingAutoPlayJustLinked
		pendingAutoPlayJustLinked = false
		playLocalConsole(host, justLinked = justLinked)
	}

	private fun wakeupHost(host: DisplayHost)
	{
		val registeredHost = host.registeredHost ?: return
		if(!registeredHost.target.isPS5)
		{
			showUnsupportedRegistration(registeredHost)
			return
		}
		viewModel.discoveryManager.sendWakeup(
			host.host,
			registeredHost.rpRegistKey,
			registeredHost.target.isPS5
		)
	}

	private fun connectPsnConsole(console: PsnConsole, justLinked: Boolean = false)
	{
		val registered = console.registeredHost ?: return
		if(!registered.target.isPS5)
		{
			showUnsupportedRegistration(registered)
			return
		}
		streamLauncher.launch(GoVrSupport.streamIntent(this).apply {
			putExtra(StreamActivity.EXTRA_CONNECT_INFO, viewModel.connectInfo(registered))
			putExtra(StreamActivity.EXTRA_PSN_DEVICE, console.device)
			putExtra(StreamActivity.EXTRA_JUST_LINKED, justLinked)
		})
	}

	private fun editConsole(console: HomeConsole)
	{
		val host = console.manualDisplayHost ?: return
		startActivity(Intent(this, EditManualConsoleActivity::class.java).apply {
			putExtra(EditManualConsoleActivity.EXTRA_MANUAL_HOST_ID, host.manualHost.id)
		})
	}

	private fun deleteConsole(console: HomeConsole)
	{
		val host = console.manualDisplayHost ?: return
		MaterialAlertDialogBuilder(this)
			.setMessage(getString(R.string.alert_message_delete_manual_host, host.manualHost.host))
			.setPositiveButton(R.string.action_delete) { _, _ ->
				viewModel.deleteManualHost(host.manualHost)
			}
			.setNegativeButton(R.string.action_keep) { _, _ -> }
			.show()
	}
}
