// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.GLUtils
import android.opengl.Matrix
import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.WindowManager
import fi.madekivi.pleikkari.common.Preferences
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.sin

/**
 * PLE-603 debug harness: draws the selected VR environment as a stereo pair on a flat
 * display with a synthetic picture, the head slowly turning, and logs the renderer's GPU
 * time and geometry counters every 72 frames under the tag VrEnvPreview. Debug builds only.
 *
 *   adb shell am start -n fi.madekivi.pleikkari/.stream.VrEnvironmentPreviewActivity \
 *       --es environment cinema
 *
 * The eye buffers are 1024x1024 each (the Go's suggested eye texture size) regardless of
 * the window, so a frame time measured here is the cost the VrApi activity will pay.
 */
class VrEnvironmentPreviewActivity : Activity()
{
	companion object
	{
		const val EXTRA_ENVIRONMENT = "environment"
		private const val TAG = "VrEnvPreview"
		private const val EYE_SIZE = 1024
	}

	private var surfaceView: GLSurfaceView? = null
	private var renderer: PreviewRenderer? = null

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)
		if(!VrEnvironmentNative.load())
		{
			Log.e(TAG, "pleikkari-vr-environment library unavailable")
			finish()
			return
		}
		val stored = Preferences(this).vrEnvironmentConfig()
		val config = intent.getStringExtra(EXTRA_ENVIRONMENT)
			?.let { stored.copy(environment = VrEnvironmentKind.fromValue(it)) } ?: stored
		window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
		window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
			View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
		val view = GLSurfaceView(this).apply {
			setEGLContextClientVersion(3)
			setEGLConfigChooser(8, 8, 8, 8, 16, 0)
			holder.setFixedSize(EYE_SIZE * 2, EYE_SIZE)
			val r = PreviewRenderer(config)
			renderer = r
			setRenderer(r)
			renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
		}
		surfaceView = view
		setContentView(view)
		Log.i(TAG, "preview of ${config.environment.value}: $config")
	}

	override fun onResume()
	{
		super.onResume()
		surfaceView?.onResume()
	}

	override fun onPause()
	{
		// The GL thread still owns its context here; free the renderer before it goes.
		surfaceView?.queueEvent { renderer?.release() }
		surfaceView?.onPause()
		super.onPause()
	}

	private class PreviewRenderer(private val config: VrEnvironmentConfig) : GLSurfaceView.Renderer
	{
		private var handle = 0L
		private var texture = 0
		private var width = 0
		private var height = 0
		private var frames = 0
		private var lastLogNs = 0L
		private val identity = FloatArray(16).also { Matrix.setIdentityM(it, 0) }
		private val view = FloatArray(16)
		private val projection = FloatArray(16)
		private val head = FloatArray(16)
		private val eye = FloatArray(16)
		private val stats = LongArray(4)
		private val startNs = System.nanoTime()

		override fun onSurfaceCreated(gl: GL10?, eglConfig: EGLConfig?)
		{
			texture = testPicture()
			handle = VrEnvironmentNative.create(config.toNative(), VrEnvironmentNative.TEXTURE_2D)
			if(handle == 0L)
				Log.e(TAG, "environment create failed")
			frames = 0
			lastLogNs = System.nanoTime()
		}

		override fun onSurfaceChanged(gl: GL10?, w: Int, h: Int)
		{
			width = w
			height = h
		}

		override fun onDrawFrame(gl: GL10?)
		{
			if(handle == 0L)
				return
			// A slow head turn of +-35 degrees shows the room without a headset.
			val t = (System.nanoTime() - startNs) / 1e9
			val yawDegrees = 35.0 * sin(t * 0.5)
			VrEnvironmentNative.beginFrame(handle, texture, identity, true, true)
			val eyeWidth = width / 2
			Matrix.perspectiveM(projection, 0, 90f, eyeWidth.toFloat() / height, 0.1f, 200f)
			for(i in 0 until 2)
			{
				GLES20.glViewport(i * eyeWidth, 0, eyeWidth, height)
				// view = inverse(R_y(yaw) * T(eyeOffset))
				Matrix.setIdentityM(head, 0)
				Matrix.rotateM(head, 0, yawDegrees.toFloat(), 0f, 1f, 0f)
				Matrix.setIdentityM(eye, 0)
				Matrix.translateM(eye, 0, if(i == 0) -0.032f else 0.032f, 0f, 0f)
				Matrix.multiplyMM(view, 0, head, 0, eye, 0)
				Matrix.invertM(view, 0, view, 0)
				VrEnvironmentNative.drawEye(handle, view, projection, texture, identity, true)
			}
			if(++frames % 72 == 0)
			{
				val now = System.nanoTime()
				val cpuMs = (now - lastLogNs) / 72 / 1e6
				lastLogNs = now
				VrEnvironmentNative.stats(handle, stats)
				Log.i(TAG, String.format("env=%s eyes=%dx%d gpu=%.2f ms frame=%.2f ms draws=%d tris=%d vertexBytes=%d",
					config.environment.value, eyeWidth, height, stats[0] / 1e6, cpuMs, stats[1], stats[2], stats[3]))
			}
		}

		fun release()
		{
			if(handle != 0L)
			{
				VrEnvironmentNative.destroy(handle)
				handle = 0L
			}
			if(texture != 0)
			{
				GLES20.glDeleteTextures(1, intArrayOf(texture), 0)
				texture = 0
			}
		}

		/** Colour bars with a label, standing in for the decoder's picture. */
		private fun testPicture(): Int
		{
			val bitmap = Bitmap.createBitmap(640, 360, Bitmap.Config.ARGB_8888)
			val canvas = Canvas(bitmap)
			val bars = intArrayOf(Color.rgb(192, 192, 192), Color.rgb(192, 192, 0), Color.rgb(0, 192, 192),
				Color.rgb(0, 192, 0), Color.rgb(192, 0, 192), Color.rgb(192, 0, 0), Color.rgb(0, 0, 192))
			val paint = Paint()
			val barWidth = bitmap.width / bars.size.toFloat()
			bars.forEachIndexed { i, colour ->
				paint.color = colour
				canvas.drawRect(i * barWidth, 0f, (i + 1) * barWidth, bitmap.height * 0.75f, paint)
			}
			paint.color = Color.rgb(30, 30, 30)
			canvas.drawRect(0f, bitmap.height * 0.75f, bitmap.width.toFloat(), bitmap.height.toFloat(), paint)
			paint.color = Color.WHITE
			paint.textSize = 40f
			paint.isAntiAlias = true
			paint.textAlign = Paint.Align.CENTER
			canvas.drawText("PLEIKKARI VR ENVIRONMENT PREVIEW", bitmap.width / 2f, bitmap.height * 0.9f, paint)
			val ids = IntArray(1)
			GLES20.glGenTextures(1, ids, 0)
			GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, ids[0])
			GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
			GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
			GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
			GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
			// Bitmap rows start at the top; flip so v=0 is the bottom like the decoder's transform.
			val flipped = android.graphics.Matrix().apply { preScale(1f, -1f) }
			val upright = Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, flipped, false)
			GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, upright, 0)
			upright.recycle()
			bitmap.recycle()
			GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0)
			return ids[0]
		}
	}
}
