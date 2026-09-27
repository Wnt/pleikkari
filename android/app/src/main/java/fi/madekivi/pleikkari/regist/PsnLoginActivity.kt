// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.regist

import android.animation.Animator
import android.animation.ObjectAnimator
import android.animation.PropertyValuesHolder
import android.animation.ValueAnimator
import android.app.Activity
import android.app.PendingIntent
import android.content.ActivityNotFoundException
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import android.view.View
import android.view.animation.AccelerateDecelerateInterpolator
import android.widget.Toast
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.addCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.browser.customtabs.CustomTabsIntent
import androidx.browser.customtabs.CustomTabsService
import androidx.core.content.ContextCompat
import androidx.core.graphics.drawable.toBitmap
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.common.ext.applySystemBarInsets
import fi.madekivi.pleikkari.common.ext.enableAppEdgeToEdge
import fi.madekivi.pleikkari.databinding.ActivityPsnLoginBinding
import kotlinx.coroutines.launch

/**
 * Signs in inside the default browser's Custom Tab, the one surface that can do everything Sony's
 * page asks for: passkeys included, which the app's own WebView can never provide for this package
 * (Sony's assetlinks.json names only Sony's apps). One surface means the address is typed once
 * (PLE-312: starting in the WebView and moving to the tab on the passkey prompt asked for it twice,
 * because the two have separate cookie jars). The tab carries a Finish sign-in button handing the
 * page address back to the app. The WebView remains only for a device with no Custom Tabs browser.
 *
 * Leaving the tab any other way (its X, back, "Open in browser") loses nothing: the app reopens the
 * sign-in, which the browser's session cookie takes straight to a fresh redirect (PLE-323).
 */
class PsnLoginActivity : AppCompatActivity()
{
	companion object
	{
		const val EXTRA_ACCOUNT_ID = "psn_account_id"
		/** The caller can link a console on this network with its PIN, so offer that if sign-in does not finish. */
		const val EXTRA_OFFER_PIN_LINK = "offer_pin_link"
		/** Result code: the user chose the PIN link instead of signing in. */
		const val RESULT_LINK_WITH_PIN = Activity.RESULT_FIRST_USER + 1
		private const val TAG = "PsnLogin"
		private const val STATE_BROWSER_SIGN_IN = "browser_sign_in"
		private const val STATE_BROWSER_LAUNCHED = "browser_launched"
		private const val STATE_BROWSER_PAUSE_OBSERVED = "browser_pause_observed"
		private const val STATE_BROWSER_OPENED_AT = "browser_opened_at"
		private const val STATE_BROWSER_RECOVERIES = "browser_recoveries"
		private const val STATE_EXPLAINER_SHOWING = "explainer_showing"
		private const val STATE_EXPLAINER_REMAINING = "explainer_remaining"
		private const val STATE_EXPLAINER_TAB_ONLY = "explainer_tab_only"
		/** 25 frames a second is smooth for a bar with no digits on it, and costs nothing. */
		private const val EXPLAINER_TICK_MS = 40L
	}

	private lateinit var binding: ActivityPsnLoginBinding
	private var handlingRedirect = false
	private var browserSignIn = false
	private var browserLaunched = false
	private var browserPauseObserved = false
	private var browserOpenedAtMs = 0L
	/** Tabs reopened in a row after the user left one without a code. */
	private var browserRecoveries = 0
	/** Read once: a redirect arriving through onNewIntent replaces the intent without this extra. */
	private var offerPinLink = false
	/** The explainer is in front of the browser launch it will make when its bar fills (PLE-339). */
	private var explainerShowing = false
	private var explainerTabOnly = false
	private var explainerRemainingMs = PSN_SIGN_IN_EXPLAINER_MS
	private var explainerDeadlineMs = 0L
	/** Stopping twice (onPause, then onSaveInstanceState) must not shorten what is left. */
	private var explainerTicking = false
	private val explainerHandler = Handler(Looper.getMainLooper())
	private var highlightPulse: Animator? = null
	private val explainerTick = object : Runnable
	{
		override fun run()
		{
			if(!explainerShowing)
				return
			val remaining = explainerDeadlineMs - SystemClock.elapsedRealtime()
			binding.signInExplainer.signInExplainerProgress.progress =
				psnSignInExplainerProgress(PSN_SIGN_IN_EXPLAINER_MS - remaining)
			if(remaining <= 0L)
				proceedFromSignInExplainer()
			else
				explainerHandler.postDelayed(this, EXPLAINER_TICK_MS)
		}
	}

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)
		enableAppEdgeToEdge()
		offerPinLink = intent.getBooleanExtra(EXTRA_OFFER_PIN_LINK, false)
		binding = ActivityPsnLoginBinding.inflate(layoutInflater)
		setContentView(binding.root)
		binding.root.applySystemBarInsets(top = false)
		binding.toolbar.applySystemBarInsets(left = false, right = false, bottom = false)
		binding.toolbar.setNavigationOnClickListener { finish() }
		binding.continueButton.setOnClickListener {
			browserRecoveries = 0
			beginBrowserSignIn()
		}
		binding.signInExplainer.signInExplainerButton.setOnClickListener { proceedFromSignInExplainer() }
		binding.pinLinkButton.setOnClickListener {
			setResult(RESULT_LINK_WITH_PIN)
			finish()
		}
		configureWebView()
		onBackPressedDispatcher.addCallback(this) {
			if(!browserSignIn && binding.webView.canGoBack() && !handlingRedirect)
				binding.webView.goBack()
			else
				finish()
		}

		if(intent.dataString?.let(::handleRedirect) == true)
			return

		browserSignIn = savedInstanceState?.getBoolean(STATE_BROWSER_SIGN_IN) ?: false
		browserLaunched = savedInstanceState?.getBoolean(STATE_BROWSER_LAUNCHED) ?: false
		browserPauseObserved = savedInstanceState?.getBoolean(STATE_BROWSER_PAUSE_OBSERVED) ?: false
		browserOpenedAtMs = savedInstanceState?.getLong(STATE_BROWSER_OPENED_AT) ?: 0L
		browserRecoveries = savedInstanceState?.getInt(STATE_BROWSER_RECOVERIES) ?: 0
		explainerTabOnly = savedInstanceState?.getBoolean(STATE_EXPLAINER_TAB_ONLY) ?: false
		explainerRemainingMs = savedInstanceState?.getLong(STATE_EXPLAINER_REMAINING) ?: PSN_SIGN_IN_EXPLAINER_MS
		when
		{
			browserSignIn -> showBrowserWaiting()
			// A rotation while the explainer is up keeps its place in the countdown, not its start.
			savedInstanceState?.getBoolean(STATE_EXPLAINER_SHOWING) == true ->
			{
				// The page under the explainer is kept too: the passkey path reaches here from a
				// loaded WebView, which is still the fallback if no browser opens after all.
				binding.webView.restoreState(savedInstanceState)
				showSignInExplainer(explainerTabOnly, explainerRemainingMs)
			}
			savedInstanceState != null ->
			{
				showWebView()
				binding.webView.restoreState(savedInstanceState)
			}
			beginBrowserSignIn(tabOnly = true) -> Unit
			else ->
			{
				showWebView()
				binding.webView.loadUrl(PsnAuth.loginUrl())
			}
		}
	}

	override fun onPause()
	{
		if(browserLaunched)
			browserPauseObserved = true
		// Out of sight the countdown stops: a browser started from the background is dropped by
		// Android 10 and later anyway, and the user has not had his 15 s while looking elsewhere.
		if(explainerShowing)
			stopSignInExplainerTimer()
		super.onPause()
	}

	override fun onResume()
	{
		super.onResume()
		if(explainerShowing)
			startSignInExplainerTimer()
	}

	override fun onWindowFocusChanged(hasFocus: Boolean)
	{
		super.onWindowFocusChanged(hasFocus)
		// The clipboard is only readable with window focus, which arrives after onResume.
		if(!hasFocus || !shouldHandlePsnBrowserReturn(
			browserSignIn,
			handlingRedirect,
			browserLaunched,
			browserPauseObserved
		))
			return
		browserLaunched = false
		browserPauseObserved = false
		if(PsnPendingRedirect.take()?.let(::handleRedirect) != true && !consumeClipboardRedirect())
			recoverBrowserSignIn()
	}

	/** Back from the tab without a code, the usual case for a first-time user: reopen it, don't explain. */
	private fun recoverBrowserSignIn()
	{
		if(PsnBrowserRecovery.enabled && psnBrowserReturnStep(browserRecoveries) == PsnBrowserReturnStep.REOPEN_TAB)
		{
			browserRecoveries++
			Log.i(TAG, "browser returned without a code; reopening the sign-in tab (recovery $browserRecoveries of $PSN_SILENT_BROWSER_RECOVERIES)")
			if(launchBrowserSignIn(tabOnly = true))
				return
		}
		Log.i(TAG, "browser returned without a code; offering the choices")
		showBrowserReturnedWithoutCode()
	}

	override fun onNewIntent(intent: Intent)
	{
		super.onNewIntent(intent)
		setIntent(intent)
		intent.dataString?.let(::handleRedirect)
	}

	private fun configureWebView()
	{
		binding.webView.settings.apply {
			javaScriptEnabled = true
			domStorageEnabled = true
			allowFileAccess = false
			allowContentAccess = false
			mixedContentMode = android.webkit.WebSettings.MIXED_CONTENT_NEVER_ALLOW
		}
		CookieManager.getInstance().apply {
			setAcceptCookie(true)
			setAcceptThirdPartyCookies(binding.webView, true)
		}
		binding.webView.addJavascriptInterface(PasskeyBridge(), PSN_PASSKEY_BRIDGE)
		binding.webView.webChromeClient = object : WebChromeClient()
		{
			override fun onProgressChanged(view: WebView?, newProgress: Int)
			{
				if(!browserSignIn && !handlingRedirect)
				{
					binding.progressBar.isIndeterminate = false
					binding.progressBar.progress = newProgress
					binding.progressBar.visibility = if(newProgress == 100) View.GONE else View.VISIBLE
				}
			}
		}
		binding.webView.webViewClient = object : WebViewClient()
		{
			override fun shouldOverrideUrlLoading(view: WebView?, request: WebResourceRequest): Boolean =
				handleRedirect(request.url.toString())

			@Suppress("DEPRECATION")
			override fun shouldOverrideUrlLoading(view: WebView?, url: String): Boolean =
				handleRedirect(url)

			override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?)
			{
				super.onPageStarted(view, url, favicon)
				if(url?.let(::handleRedirect) != true)
					view?.evaluateJavascript(PSN_PASSKEY_HOOK_JS, null)
			}

			override fun onPageCommitVisible(view: WebView?, url: String?)
			{
				super.onPageCommitVisible(view, url)
				view?.evaluateJavascript(PSN_PASSKEY_HOOK_JS, null)
			}

			override fun onPageFinished(view: WebView?, url: String?)
			{
				super.onPageFinished(view, url)
				view?.evaluateJavascript(PSN_PASSKEY_HOOK_JS, null)
			}
		}
	}

	private inner class PasskeyBridge
	{
		@JavascriptInterface
		fun passkeyRequested()
		{
			runOnUiThread {
				if(browserSignIn || handlingRedirect || isFinishing)
					return@runOnUiThread
				if(!isPsnSignInHost(binding.webView.url?.let { Uri.parse(it).host }))
					return@runOnUiThread
				beginBrowserSignIn()
			}
		}
	}

	/**
	 * The way into the browser for anyone who asked for it: the user pressing Link or Continue, a
	 * retry, or the page asking for a passkey the WebView can never give. The explainer goes first,
	 * because pressing the tab's ✓ is the only way this app ever learns the tab's address and the
	 * user has to be told before the tab covers the screen. Returns false when no browser would
	 * open at all, leaving the WebView as the way in; the silent reopen after a tab came back
	 * without a code (PLE-323) calls [launchBrowserSignIn] directly and stays silent.
	 */
	private fun beginBrowserSignIn(tabOnly: Boolean = false): Boolean
	{
		if(!canOpenBrowserSignIn(tabOnly))
			return false
		showSignInExplainer(tabOnly, PSN_SIGN_IN_EXPLAINER_MS)
		return true
	}

	/** Whether [launchBrowserSignIn] has anything to launch, asked before the explainer commits to it. */
	private fun canOpenBrowserSignIn(tabOnly: Boolean): Boolean
	{
		val uri = Uri.parse(PsnAuth.loginUrl())
		if(findCustomTabsPackage(uri) != null)
			return true
		if(tabOnly)
			return false
		val viewIntent = Intent(Intent.ACTION_VIEW, uri).addCategory(Intent.CATEGORY_BROWSABLE)
		return packageManager.resolveActivity(viewIntent, 0) != null
	}

	/**
	 * The 15 s explainer: the drawn toolbar with its ✓ picked out, a bar with no digits on it, and
	 * a button for the user who is already holding his password. Both ways out are the same launch.
	 */
	private fun showSignInExplainer(tabOnly: Boolean, remainingMs: Long)
	{
		explainerShowing = true
		explainerTabOnly = tabOnly
		explainerRemainingMs = remainingMs.coerceIn(0L, PSN_SIGN_IN_EXPLAINER_MS)
		binding.webView.visibility = View.GONE
		binding.progressBar.visibility = View.GONE
		binding.continueButton.visibility = View.GONE
		binding.pinLinkButton.visibility = View.GONE
		binding.finishHintTextView.visibility = View.GONE
		binding.signInExplainer.signInExplainerProgress.max = PSN_SIGN_IN_EXPLAINER_PROGRESS_MAX
		binding.signInExplainer.signInExplainerProgress.progress =
			psnSignInExplainerProgress(PSN_SIGN_IN_EXPLAINER_MS - explainerRemainingMs)
		binding.signInExplainer.root.visibility = View.VISIBLE
		startHighlightPulse()
		// onResume starts the countdown when the screen is not in front yet (a rotation, onCreate).
		if(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
			startSignInExplainerTimer()
	}

	private fun startSignInExplainerTimer()
	{
		explainerDeadlineMs = SystemClock.elapsedRealtime() + explainerRemainingMs
		explainerHandler.removeCallbacks(explainerTick)
		explainerTicking = true
		explainerTick.run()
	}

	private fun stopSignInExplainerTimer()
	{
		explainerHandler.removeCallbacks(explainerTick)
		if(!explainerTicking)
			return
		explainerTicking = false
		explainerRemainingMs = (explainerDeadlineMs - SystemClock.elapsedRealtime())
			.coerceIn(0L, PSN_SIGN_IN_EXPLAINER_MS)
	}

	/** The bar ran out, or the user pressed Sign in: the same browser launch either way. */
	private fun proceedFromSignInExplainer()
	{
		if(!explainerShowing)
			return
		hideSignInExplainer()
		val tabOnly = explainerTabOnly
		if(launchBrowserSignIn(tabOnly))
			return
		// The browser went away between the explainer and this launch: the WebView is what is left.
		showWebView()
		binding.webView.loadUrl(PsnAuth.loginUrl())
	}

	private fun hideSignInExplainer()
	{
		if(explainerShowing)
			stopSignInExplainerTimer()
		explainerShowing = false
		explainerRemainingMs = PSN_SIGN_IN_EXPLAINER_MS
		stopHighlightPulse()
		binding.signInExplainer.root.visibility = View.GONE
	}

	/**
	 * The ✓ wears a ring at rest; a second ring pulses out of it, unless animations are off
	 * system-wide, in which case the ring stays and only the movement goes (see
	 * [shouldPulseSignInHighlight]).
	 */
	private fun startHighlightPulse()
	{
		stopHighlightPulse()
		val pulse = binding.signInExplainer.signInExplainerCheckPulse
		if(!shouldPulseSignInHighlight(animatorDurationScale()))
		{
			pulse.visibility = View.INVISIBLE
			return
		}
		pulse.visibility = View.VISIBLE
		highlightPulse = ObjectAnimator.ofPropertyValuesHolder(
			pulse,
			PropertyValuesHolder.ofFloat(View.SCALE_X, 1f, 1.7f),
			PropertyValuesHolder.ofFloat(View.SCALE_Y, 1f, 1.7f),
			PropertyValuesHolder.ofFloat(View.ALPHA, 0.9f, 0f)
		).apply {
			duration = 1400L
			repeatCount = ValueAnimator.INFINITE
			interpolator = AccelerateDecelerateInterpolator()
			start()
		}
	}

	private fun stopHighlightPulse()
	{
		highlightPulse?.cancel()
		highlightPulse = null
		binding.signInExplainer.signInExplainerCheckPulse.let {
			it.scaleX = 1f
			it.scaleY = 1f
			it.alpha = 0.9f
			it.visibility = View.INVISIBLE
		}
	}

	private fun animatorDurationScale(): Float =
		Settings.Global.getFloat(contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f)

	/**
	 * Opens the sign-in in a browser: a Custom Tab with the Finish sign-in button, or with [tabOnly]
	 * false any browser at all. Returns false when nothing opened, leaving the WebView as the way in.
	 */
	private fun launchBrowserSignIn(tabOnly: Boolean = false): Boolean
	{
		val uri = Uri.parse(PsnAuth.loginUrl())
		val tabIntent = customTabIntent(uri)
		val intents = listOfNotNull(
			tabIntent,
			Intent(Intent.ACTION_VIEW, uri).addCategory(Intent.CATEGORY_BROWSABLE).takeUnless { tabOnly }
		)
		for(browserIntent in intents)
		{
			try
			{
				markBrowserOpened()
				startActivity(browserIntent)
				binding.webView.stopLoading()
				binding.webView.loadUrl("about:blank")
				browserSignIn = true
				showBrowserWaiting()
				return true
			}
			catch(_: ActivityNotFoundException)
			{
				// The provider disappeared between discovery and launch; try the next one.
			}
		}
		// No browser at all: the WebView is the only way left, even without passkeys.
		browserLaunched = false
		return false
	}

	private fun markBrowserOpened()
	{
		// Pressing the tab's action button is the only way an Android app ever learns a Custom Tab's
		// address (PLE-334: every other route is closed -- our own WebView is refused by both credential
		// providers, and a redirect we could claim is not registered against Sony's client id). So the
		// user has to know which control finishes the job, and once the tab is up this toast is the only
		// surface of ours he can still see. One line, and never an instruction screen.
		Toast.makeText(this, R.string.psn_login_finish_hint, Toast.LENGTH_LONG).show()
		browserLaunched = true
		browserPauseObserved = false
		browserOpenedAtMs = System.currentTimeMillis()
		PsnPendingRedirect.take()
	}

	private fun customTabIntent(uri: Uri): Intent?
	{
		val packageName = findCustomTabsPackage(uri) ?: return null
		// A broadcast, not an activity: Android 14 and later drop an activity PendingIntent sent by a
		// browser that does not opt in to background starts (Firefox 152 does not), while a receiver
		// may open the app because the tab runs in the app's task.
		val finishIntent = PendingIntent.getBroadcast(
			this,
			0,
			Intent(this, PsnRedirectReceiver::class.java),
			// The browser fills in the page address, so the intent has to stay mutable.
			PendingIntent.FLAG_UPDATE_CURRENT or
				if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
		)
		val finishLabel = getString(R.string.action_psn_finish_sign_in)
		val finishIcon = ContextCompat.getDrawable(this, R.drawable.ic_psn_finish_sign_in)!!.toBitmap()
		// The Finish button is the one thing on the tab that returns the code, so it stays on a toolbar
		// that never scrolls away, and the menu carries nothing else of ours to compete with it.
		return CustomTabsIntent.Builder()
			.setShowTitle(true)
			.setUrlBarHidingEnabled(false)
			.setShareState(CustomTabsIntent.SHARE_STATE_OFF)
			.setBookmarksButtonEnabled(false)
			.setDownloadButtonEnabled(false)
			.setActionButton(finishIcon, finishLabel, finishIntent, true)
			.addMenuItem(finishLabel, finishIntent)
			.build()
			.intent
			.setPackage(packageName)
			.setData(uri)
	}

	private fun findCustomTabsPackage(uri: Uri): String?
	{
		val viewIntent = Intent(Intent.ACTION_VIEW, uri).addCategory(Intent.CATEGORY_BROWSABLE)
		val defaultPackage = packageManager.resolveActivity(viewIntent, 0)?.activityInfo?.packageName
		val candidates = packageManager.queryIntentActivities(viewIntent, 0)
			.map { it.activityInfo.packageName }
			.distinct()
			.sortedByDescending { it == defaultPackage }
		return candidates.firstOrNull { packageName ->
			packageManager.resolveService(
				Intent(CustomTabsService.ACTION_CUSTOM_TABS_CONNECTION).setPackage(packageName),
				0
			) != null
		}
	}

	private fun consumeClipboardRedirect(): Boolean
	{
		val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
		val clip = clipboard.primaryClip?.takeIf { it.itemCount > 0 } ?: return false
		val clipTimestamp = if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
			clip.description.timestamp
		else
			null
		if(!isPsnClipFromThisSignIn(clipTimestamp, browserOpenedAtMs))
			return false
		val address = clip.getItemAt(0).coerceToText(this)?.toString().orEmpty()
		if(parsePsnRedirect(address) !is PsnRedirect.Code)
			return false
		return handleRedirect(address)
	}

	private fun handleRedirect(url: String): Boolean
	{
		val redirect = parsePsnRedirect(url)
		if(handlingRedirect && redirect != PsnRedirect.NotRedirect)
			return true
		return when(redirect)
		{
			PsnRedirect.NotRedirect -> false
			// PLE-296/PLE-293: a cancel is not an error, so it gets no dialog -- just the screen the user
			// came from, same as its RESULT_CANCELED default when setResult is never called.
			PsnRedirect.Cancelled -> {
				Log.i(TAG, "sign-in cancelled (error redirect)")
				handlingRedirect = true
				PsnPendingRedirect.take()
				finish()
				true
			}
			PsnRedirect.Invalid -> {
				showError(getString(R.string.psn_login_redirect_invalid))
				true
			}
			is PsnRedirect.Code -> {
				exchangeCode(redirect.value)
				true
			}
		}
	}

	private fun exchangeCode(code: String)
	{
		if(handlingRedirect)
			return
		handlingRedirect = true
		hideSignInExplainer()
		PsnPendingRedirect.take()
		binding.webView.stopLoading()
		binding.webView.visibility = View.GONE
		binding.continueButton.visibility = View.GONE
		binding.pinLinkButton.visibility = View.GONE
		binding.finishHintTextView.visibility = View.GONE
		binding.progressBar.visibility = View.VISIBLE
		binding.progressBar.isIndeterminate = true
		lifecycleScope.launch {
			runCatching {
				PsnAuth.exchangeCode(code).also { result ->
					PsnCredentialStore(this@PsnLoginActivity).putRefreshToken(result.refreshToken)
					Preferences(this@PsnLoginActivity).psnAccountId = result.accountId
				}
			}
				.onSuccess { result ->
					setResult(Activity.RESULT_OK, Intent().putExtra(EXTRA_ACCOUNT_ID, result.accountId))
					finish()
				}
				.onFailure { error ->
					// The raw status (or lack of one, for a dropped connection) stays in logcat; the
					// dialog below never names it (PLE-296).
					Log.w(TAG, "PSN code exchange failed: ${error.javaClass.simpleName}: ${error.message}", error)
					if(error is PsnAuthNetworkException)
						// The code is still good; retrying it (not restarting sign-in) keeps the user's place.
						showError(getString(R.string.psn_login_network_failed)) {
							handlingRedirect = false
							exchangeCode(code)
						}
					else
						showError(getString(R.string.psn_login_link_expired))
				}
		}
	}

	private fun showError(message: String, onRetry: () -> Unit = ::restartLogin)
	{
		handlingRedirect = true
		hideSignInExplainer()
		binding.webView.stopLoading()
		binding.webView.visibility = View.GONE
		binding.continueButton.visibility = View.GONE
		binding.pinLinkButton.visibility = View.GONE
		binding.finishHintTextView.visibility = View.GONE
		binding.progressBar.visibility = View.GONE
		MaterialAlertDialogBuilder(this)
			.setTitle(R.string.psn_login_failed)
			.setMessage(message)
			.setPositiveButton(R.string.action_retry) { _, _ -> onRetry() }
			.setNegativeButton(android.R.string.cancel) { _, _ -> finish() }
			.setOnCancelListener { finish() }
			.show()
	}

	private fun restartLogin()
	{
		handlingRedirect = false
		if(browserSignIn)
			beginBrowserSignIn()
		else
		{
			showWebView()
			binding.webView.clearHistory()
			binding.webView.loadUrl(PsnAuth.loginUrl())
		}
	}

	private fun showWebView()
	{
		hideSignInExplainer()
		binding.continueButton.visibility = View.GONE
		binding.pinLinkButton.visibility = View.GONE
		binding.finishHintTextView.visibility = View.GONE
		binding.webView.visibility = View.VISIBLE
		binding.progressBar.visibility = View.VISIBLE
		binding.progressBar.isIndeterminate = true
	}

	/** Behind the browser tab: nothing to read, nothing to decide. */
	private fun showBrowserWaiting()
	{
		hideSignInExplainer()
		binding.webView.visibility = View.GONE
		binding.progressBar.visibility = View.GONE
		binding.continueButton.visibility = View.GONE
		binding.pinLinkButton.visibility = View.GONE
		binding.finishHintTextView.visibility = View.GONE
	}

	/** The tab came back without a code twice after reopening: sign in again, or link with the PIN (PLE-313). */
	private fun showBrowserReturnedWithoutCode()
	{
		showBrowserWaiting()
		binding.finishHintTextView.visibility = View.VISIBLE
		binding.continueButton.visibility = View.VISIBLE
		if(offerPinLink)
			binding.pinLinkButton.visibility = View.VISIBLE
	}

	override fun onSaveInstanceState(outState: Bundle)
	{
		outState.putBoolean(STATE_BROWSER_SIGN_IN, browserSignIn)
		outState.putBoolean(STATE_BROWSER_LAUNCHED, browserLaunched)
		outState.putBoolean(STATE_BROWSER_PAUSE_OBSERVED, browserPauseObserved)
		outState.putLong(STATE_BROWSER_OPENED_AT, browserOpenedAtMs)
		outState.putInt(STATE_BROWSER_RECOVERIES, browserRecoveries)
		if(explainerShowing)
			stopSignInExplainerTimer()
		outState.putBoolean(STATE_EXPLAINER_SHOWING, explainerShowing)
		outState.putLong(STATE_EXPLAINER_REMAINING, explainerRemainingMs)
		outState.putBoolean(STATE_EXPLAINER_TAB_ONLY, explainerTabOnly)
		if(!browserSignIn)
			binding.webView.saveState(outState)
		super.onSaveInstanceState(outState)
	}

	override fun onDestroy()
	{
		explainerHandler.removeCallbacks(explainerTick)
		highlightPulse?.cancel()
		highlightPulse = null
		binding.webView.stopLoading()
		binding.webView.removeJavascriptInterface(PSN_PASSKEY_BRIDGE)
		binding.webView.webChromeClient = null
		binding.webView.webViewClient = WebViewClient()
		binding.webView.destroy()
		super.onDestroy()
	}
}
