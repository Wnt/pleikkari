// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import android.graphics.*
import android.opengl.GLES20
import android.opengl.GLUtils
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.*
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.core.content.IntentCompat
import androidx.lifecycle.ViewModelProvider
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.common.ext.viewModelFactory
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.remote.PsnDevice
import fi.madekivi.pleikkari.session.*
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean

/** VrApi owns the display; Android's SurfaceView is only the native-window handoff. */
class StreamVrActivity : ComponentActivity(), SurfaceHolder.Callback {
    private var model: StreamViewModel? = null
    private var cinema: CinemaThread? = null
    private var windowSurface: Surface? = null
    private var resumed = false
    private var failed = false
    private var statusText = ""
    private val main = Handler(Looper.getMainLooper())
    private var preview = false
    private var previewEnvironment: String? = null
    private var environmentSamples = DEFAULT_ENVIRONMENT_SAMPLES

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val info = IntentCompat.getParcelableExtra(intent, StreamActivity.EXTRA_CONNECT_INFO, ConnectInfo::class.java)
        // PLE-623: debug builds only. The real VrApi cinema with no console and a synthetic
        // picture, so the Go's cinema and environment cost can be read from adb (README).
        preview = BuildConfig.DEBUG && intent.getBooleanExtra(EXTRA_VR_CINEMA_PREVIEW, false)
        previewEnvironment = if(preview) intent.getStringExtra(EXTRA_ENVIRONMENT) else null
        if(preview) environmentSamples = intent.getIntExtra(EXTRA_ENVIRONMENT_MSAA, DEFAULT_ENVIRONMENT_SAMPLES)
        if(!GoVrSupport.available() || !preview && (!Preferences(this).goVrEnabled || info == null)) {
            finish()
            return
        }
        if(!preview && info != null) createModel(info)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        setContentView(SurfaceView(this).apply { holder.addCallback(this@StreamVrActivity) })
    }

    private fun createModel(info: ConnectInfo) {
        val device = IntentCompat.getParcelableExtra(intent, StreamActivity.EXTRA_PSN_DEVICE, PsnDevice::class.java)
        val justLinked = intent.getBooleanExtra(StreamActivity.EXTRA_JUST_LINKED, false)
        model = ViewModelProvider(this, viewModelFactory {
            StreamViewModel(application, info, device, justLinked = justLinked)
        })[StreamViewModel::class.java].also { vm ->
            vm.input.observe(this)
            vm.session.state.observe(this) { state ->
                statusText = when(state) {
                    StreamStateConnected -> ""
                    is StreamStateQuit, is StreamStateCreateError, is StreamStateRemoteError -> getString(R.string.go_vr_disconnected)
                    is StreamStateLoginPinRequest -> getString(R.string.go_vr_pin)
                    else -> getString(R.string.go_vr_connecting)
                }
                cinema?.status = statusText
            }
        }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        startCinema()
    }

    override fun onPause() {
        resumed = false
        stopCinema()
        super.onPause()
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        stopCinema()
        windowSurface = holder.surface
        startCinema()
    }
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopCinema() // LeaveVrMode must finish before returning the window to Android.
        windowSurface = null
    }

    private fun startCinema() {
        val vm = model
        if(vm == null && !preview) return
        val surface = windowSurface?.takeIf { it.isValid } ?: return
        if(!resumed || failed || cinema != null) return
        cinema = CinemaThread(surface, vm?.connectInfo).also { it.status = statusText; it.start() }
    }

    private fun stopCinema() {
        val thread = cinema ?: return
        // Stop the producer and detach before releasing its SurfaceTexture on the GL thread.
        thread.running.set(false)
        model?.pause()
        model?.session?.detachSurface()
        thread.detached.countDown()
        thread.join()
        cinema = null
    }

    private fun cinemaFailed(error: Throwable) {
        Log.e("GoCinema", "Cannot start/continue VR cinema", error)
        failed = true
        stopCinema()
        Toast.makeText(this, R.string.go_vr_failed, Toast.LENGTH_LONG).show()
        finish() // Return to the existing Oculus TV task; never relaunch onto display 0.
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Bluetooth pads retain the existing StreamInput mapping, including their Back key.
        val gamepad = event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_JOYSTICK)
        if(!gamepad && event.keyCode == KeyEvent.KEYCODE_BACK) {
            // The Go remote is polled by VrApi. Do not toggle twice if Android also
            // delivers its Back event; its press edge will open the cinema menu.
            return true
        }
        return model?.input?.dispatchKeyEvent(event) == true || super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean =
        model?.input?.onGenericMotionEvent(event) == true || super.onGenericMotionEvent(event)

    /** All EGL, VrApi and SurfaceTexture consumer calls are confined to this thread. */
    private inner class CinemaThread(private val surface: Surface, private val info: ConnectInfo?) : Thread("GoCinema") {
        val running = AtomicBoolean(true)
        val detached = CountDownLatch(1)
        @Volatile var status = ""

        override fun run() {
            var native = 0L
            var texture: SurfaceTexture? = null
            var decoder: Surface? = null
            try {
                // PLE-636: 60 Hz only when the setting is on and the stream is 60 fps.
                val refreshHz = if(Preferences(this@StreamVrActivity).goVrMatch60Hz && info?.videoProfile?.maxFPS == 60) 60f else 72f
                native = VrCinemaNative.create(this@StreamVrActivity, surface, refreshHz, environmentSamples)
                check(native != 0L) { "VrApi/EGL initialization or $refreshHz Hz request failed (see GoCinema log)" }
                // PLE-603: the room around the screen; "plain" (the default) leaves the native path as it was.
                // PLE-652: A/B a higher GPU clock while a room is drawn; off keeps GPU level 2.
                if(Preferences(this@StreamVrActivity).goVrRoomHighGpu) VrCinemaNative.setRoomGpuLevel(native, 4)
                val stored = Preferences(this@StreamVrActivity).vrEnvironmentConfig()
                val environment = previewEnvironment?.let { stored.copy(environment = VrEnvironmentKind.fromValue(it)) } ?: stored
                environment.toNative().let {
                    VrCinemaNative.setEnvironment(native, it.environment, it.screenDistanceM, it.screenWidthM,
                        it.screenCurveRadiusM, it.screenHeightOffsetM, it.glow, it.roomLight)
                }
                val frameReady = AtomicBoolean(false)
                val consumer = SurfaceTexture(VrCinemaNative.videoTexture(native))
                texture = consumer
                consumer.setDefaultBufferSize(info?.videoProfile?.width ?: PREVIEW_WIDTH, info?.videoProfile?.height ?: PREVIEW_HEIGHT)
                consumer.setOnFrameAvailableListener({ frameReady.set(true) }, main)
                val output = Surface(consumer)
                decoder = output
                val picture = if(preview) PreviewPicture(output) else null
                if(picture != null)
                    Log.i("GoCinema", "Debug preview: no console; synthetic ${PREVIEW_WIDTH}x$PREVIEW_HEIGHT picture at 60 fps, environment ${environment.environment.value}")
                main.post {
                    if(cinema === this && running.get() && resumed) {
                        model?.session?.attachToSurface(output)
                        model?.session?.updateDisplayTiming(refreshHz.toDouble(), 0L) // PLE-654: the panel's real rate
                        model?.resume()
                    }
                }
                val transform = FloatArray(16)
                android.opengl.Matrix.setIdentityM(transform, 0)
                var hasFrame = false
                var menu = false
                var previousText: String? = null
                // PLE-654: decoder frames latched vs eye frames submitted, logged per window.
                var windowStartNs = System.nanoTime()
                var latched = 0
                var submitted = 0
                var shown = 0
                while(running.get()) {
                    val input = VrCinemaNative.input(native)
                    if(input and MENU != 0) menu = !menu
                    if(input and CLICK != 0) {
                        if(!menu || input and CENTRE != 0) VrCinemaNative.recentre(native)
                        if(menu && input and RIGHT != 0) {
                            main.post { if(cinema === this) finish() }
                        }
                        menu = false
                    }
                    picture?.post()
                    val newFrame = frameReady.getAndSet(false)
                    if(newFrame) {
                        consumer.updateTexImage()
                        consumer.getTransformMatrix(transform)
                        hasFrame = true
                        latched++
                        picture?.consumed()
                    }
                    val text = if(menu) getString(R.string.go_vr_menu) + "\n\n" +
                        getString(R.string.go_vr_resume) + "     |     " + getString(R.string.go_vr_recentre) +
                        "     |     " + getString(R.string.go_vr_disconnect) + "\n\n" + getString(R.string.go_vr_menu_help)
                    else status
                    if(text != previousText) {
                        uploadText(VrCinemaNative.messageTexture(native), text)
                        previousText = text
                    }
                    val video = hasFrame && text.isEmpty()
                    check(VrCinemaNative.draw(native, transform, video, menu, newFrame) >= 0) {
                        "vrapi_SubmitFrame2 failed"
                    }
                    submitted++
                    if(video) shown++
                    val nowNs = System.nanoTime()
                    if(nowNs - windowStartNs >= VIDEO_STATS_WINDOW_NS) {
                        val seconds = (nowNs - windowStartNs) / 1e9
                        Log.i("GoCinema", String.format(Locale.US,
                            "Cinema video: %d decoder frames latched in %.1f s (%.1f fps), %d of %d submitted frames showed video",
                            latched, seconds, latched / seconds, shown, submitted))
                        windowStartNs = nowNs
                        latched = 0
                        submitted = 0
                        shown = 0
                    }
                }
            } catch(error: Exception) {
                main.post { if(cinema === this) cinemaFailed(error) }
            } catch(error: LinkageError) {
                main.post { if(cinema === this) cinemaFailed(error) }
            } finally {
                // An error may reach here while the decoder still holds the output. The main
                // thread stops it before signalling detached; never free a live producer's target.
                detached.await()
                decoder?.release()
                texture?.release()
                if(native != 0L) VrCinemaNative.destroy(native)
            }
        }

        private fun uploadText(texture: Int, text: String) {
            val bitmap = Bitmap.createBitmap(1536, 864, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            canvas.drawColor(Color.rgb(12, 15, 22))
            val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE; textSize = 36f; textAlign = Paint.Align.CENTER
            }
            text.split('\n').forEachIndexed { i, line -> canvas.drawText(line, 768f, 300f + i * 64f, paint) }
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0)
            bitmap.recycle()
        }
    }

    /**
     * PLE-623 debug preview: a moving test picture queued into the decoder's SurfaceTexture at
     * 60 fps, so the environment's glow refresh and picture sampling run as in a stream. Drawn
     * on the cinema thread, one frame in flight at a time, so it never blocks on its consumer.
     */
    private class PreviewPicture(private val output: Surface) {
        private val bars = intArrayOf(Color.WHITE, Color.YELLOW, Color.CYAN, Color.GREEN, Color.MAGENTA, Color.RED, Color.BLUE)
        private val paint = Paint()
        private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textSize = PREVIEW_HEIGHT / 12f }
        private var frames = 0L
        private var lastNs = 0L
        private var inFlight = false

        fun post() {
            val now = System.nanoTime()
            if(inFlight || now - lastNs < 1_000_000_000L / 60) return
            lastNs = now
            val canvas = output.lockHardwareCanvas()
            try {
                val w = PREVIEW_WIDTH.toFloat()
                val h = PREVIEW_HEIGHT.toFloat()
                val bar = w / bars.size
                bars.forEachIndexed { i, color -> paint.color = color; canvas.drawRect(i * bar, 0f, (i + 1) * bar, h * 0.75f, paint) }
                paint.color = Color.rgb(24, 24, 32)
                canvas.drawRect(0f, h * 0.75f, w, h, paint)
                val x = (frames % 120) / 120f * w
                paint.color = Color.WHITE
                canvas.drawRect(x, h * 0.75f, x + w / 40f, h, paint)
                canvas.drawText("Pleikkari VR cinema preview   frame $frames", w * 0.05f, h * 0.92f, text)
            } finally {
                output.unlockCanvasAndPost(canvas)
            }
            frames++
            inFlight = true
        }

        fun consumed() {
            inFlight = false
        }
    }

    companion object {
        // JNI poll result: Back toggles menu; touchpad click recentres or picks its horizontal third.
        private const val MENU = 1
        private const val CLICK = 2
        private const val CENTRE = 4
        private const val RIGHT = 8
        /** PLE-623: debug builds only; see [PreviewPicture] and third_party/ovr_sdk_mobile/README.md. */
        const val EXTRA_VR_CINEMA_PREVIEW = "vr_cinema_preview"
        /** With the preview: plain, void, cinema or terrace instead of the stored setting. */
        const val EXTRA_ENVIRONMENT = "environment"
        /** PLE-653: with the preview, the rooms' MSAA sample count (1 turns MSAA off) instead of PLE-615's 4x. */
        const val EXTRA_ENVIRONMENT_MSAA = "environment_msaa"
        private const val DEFAULT_ENVIRONMENT_SAMPLES = 4
        private const val PREVIEW_WIDTH = 1280
        private const val PREVIEW_HEIGHT = 720
        private const val VIDEO_STATS_WINDOW_NS = 5_000_000_000L
    }
}

internal object VrCinemaNative {
    external fun create(activity: android.app.Activity, surface: Surface, refreshHz: Float, environmentSamples: Int): Long
    external fun videoTexture(handle: Long): Int
    external fun messageTexture(handle: Long): Int
    external fun input(handle: Long): Int
    external fun recentre(handle: Long)
    external fun draw(handle: Long, transform: FloatArray, video: Boolean, menu: Boolean, newFrame: Boolean): Int
    /** PLE-603: [VrEnvironmentNativeConfig] fields; environment 0 (plain) removes the room. */
    external fun setRoomGpuLevel(handle: Long, level: Int)
    external fun setEnvironment(handle: Long, environment: Int, distance: Float, width: Float, radius: Float,
        heightOffset: Float, glow: Float, roomLight: Float)
    external fun destroy(handle: Long)
}
