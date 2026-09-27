package fi.madekivi.pleikkari.session

import android.content.Context
import android.hardware.*
import android.os.Handler
import android.os.Looper
import android.view.*
import java.util.Collections
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.OnLifecycleEvent
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.lib.ControllerState
import fi.madekivi.pleikkari.lib.LatencyProbe

/**
 * PLE-802: thread safety. Key and joystick events arrive on the main thread, or with Go's
 * `stream_go_vr_input_thread` on its GoPadInput thread; sensors, [touchControllerState],
 * [releasePad] and the coalesced flush stay on the main thread. [stateLock] guards the four partial
 * states, [displayRotation], [controllerStateDirty] and every call of [controllerStateChangedCallback]:
 * each change and the state it sends are one critical section. So a read-modify-write of a partial
 * state never loses another thread's change, and the state sent last is always computed after the
 * last change: the console ends on the newest state whichever thread changed it. With a single thread
 * the lock is uncontended and the states sent are the same as before.
 *
 * PLE-830: with `stream_go_vr_input_thread_flush` the coalesced flush of a change made on another
 * looper thread (GoPadInput) runs on that thread's own Choreographer instead of hopping to the main
 * looper. Each looper has its own [FlushScheduler]; a flush sends only if [controllerStateDirty] is
 * still set under [stateLock], so two schedulers never send the same change twice.
 */
class StreamInput(val context: Context, val preferences: Preferences)
{
	companion object
	{
		private const val FRAME_FALLBACK_DELAY_MS = 16L
	}

	/** Called with [stateLock] held, on whichever thread changed the state (see the class comment). */
	@Volatile var controllerStateChangedCallback: ((ControllerState) -> Unit)? = null
	private val stateLock = Any()
	private val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
	private var displayRotation = currentDisplayRotation()
	private val coalesceControllerInput = preferences.controllerInputCoalescingEnabled
	private val triggerAxisResolver = TriggerAxisResolver(
		rangesForDevice = { deviceId ->
			val device = InputDevice.getDevice(deviceId)
			listOf(
				MotionEvent.AXIS_LTRIGGER,
				MotionEvent.AXIS_RTRIGGER,
				MotionEvent.AXIS_BRAKE,
				MotionEvent.AXIS_GAS
			).associateWith { axis -> device?.getMotionRange(axis)?.range }
		},
		fallbackForReportedRanges = preferences.gamepadTriggerFallbackEnabled
	)
	private val mainHandler = Handler(Looper.getMainLooper())
	private val flushOnInputThread = preferences.goVrInputThreadFlush
	private var controllerStateDirty = false
	@Volatile private var frameCallbacksRunning = false

	/** One looper's pending flush; touched only on that looper's thread. */
	private inner class FlushScheduler(looper: Looper)
	{
		val handler = Handler(looper)
		var scheduled = false
		var choreographer: Choreographer? = null
		val frameCallback = Choreographer.FrameCallback { flush() }
		val frameFallback = Runnable { flush() }

		fun schedule()
		{
			if(scheduled)
				return
			scheduled = true
			if(frameCallbacksRunning)
				Choreographer.getInstance().also { choreographer = it }.postFrameCallback(frameCallback)
			else
				handler.postDelayed(frameFallback, FRAME_FALLBACK_DELAY_MS)
		}

		/** The activity paused: vsync may stop, so the pending flush falls back to a delay. */
		fun pause()
		{
			if(!scheduled)
				return
			choreographer?.removeFrameCallback(frameCallback)
			handler.removeCallbacks(frameFallback)
			handler.postDelayed(frameFallback, FRAME_FALLBACK_DELAY_MS)
		}

		fun flush()
		{
			handler.removeCallbacks(frameFallback)
			choreographer?.removeFrameCallback(frameCallback)
			scheduled = false
			flushControllerState()
		}
	}

	private val mainScheduler = FlushScheduler(Looper.getMainLooper())
	private val inputThreadSchedulers = Collections.synchronizedMap(HashMap<Looper, FlushScheduler>())

	val controllerState: ControllerState get() = synchronized(stateLock) { mergedControllerState() }

	private fun mergedControllerState(): ControllerState
	{
		val controllerState = sensorControllerState or keyControllerState or motionControllerState

		when(displayRotation)
		{
			Surface.ROTATION_90 -> {
				controllerState.accelX *= -1.0f
				controllerState.accelZ *= -1.0f
				controllerState.gyroX *= -1.0f
				controllerState.gyroZ *= -1.0f
				controllerState.orientX *= -1.0f
				controllerState.orientZ *= -1.0f
			}
			else -> {}
		}

		// prioritize motion controller's l2 and r2 over key
		// (some controllers send only key, others both but key earlier than full press)
		if(motionControllerState.l2State > 0U)
			controllerState.l2State = motionControllerState.l2State
		if(motionControllerState.r2State > 0U)
			controllerState.r2State = motionControllerState.r2State

		return controllerState or touchControllerState
	}

	private val sensorControllerState = ControllerState() // from Motion Sensors
	private val keyControllerState = ControllerState() // from KeyEvents
	private val motionControllerState = ControllerState() // from MotionEvents
	var touchControllerState = ControllerState()
		get() = synchronized(stateLock) { field }
		set(value)
		{
			synchronized(stateLock)
			{
				field = value
				controllerStateUpdated()
			}
		}

	private val swapCrossMoon = preferences.swapCrossMoon

	private val sensorEventListener = object: SensorEventListener {
		override fun onSensorChanged(event: SensorEvent)
		{
			synchronized(stateLock)
			{
				when(event.sensor.type)
				{
					Sensor.TYPE_ACCELEROMETER -> {
						sensorControllerState.accelX = event.values[1] / SensorManager.GRAVITY_EARTH
						sensorControllerState.accelY = event.values[2] / SensorManager.GRAVITY_EARTH
						sensorControllerState.accelZ = event.values[0] / SensorManager.GRAVITY_EARTH
					}
					Sensor.TYPE_GYROSCOPE -> {
						sensorControllerState.gyroX = event.values[1]
						sensorControllerState.gyroY = event.values[2]
						sensorControllerState.gyroZ = event.values[0]
					}
					Sensor.TYPE_ROTATION_VECTOR -> {
						val q = floatArrayOf(0f, 0f, 0f, 0f)
						SensorManager.getQuaternionFromVector(q, event.values)
						sensorControllerState.orientX = q[2]
						sensorControllerState.orientY = q[3]
						sensorControllerState.orientZ = q[1]
						sensorControllerState.orientW = q[0]
					}
					else -> return
				}
				controllerStateUpdated()
			}
		}

		override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {}
	}

	private val motionLifecycleObserver = object: LifecycleObserver {
		@OnLifecycleEvent(Lifecycle.Event.ON_RESUME)
		fun onResume()
		{
			val samplingPeriodUs = 4000
			val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
			listOfNotNull(
				sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER),
				sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE),
				sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
			).forEach {
				sensorManager.registerListener(sensorEventListener, it, samplingPeriodUs)
			}
		}

		@OnLifecycleEvent(Lifecycle.Event.ON_PAUSE)
		fun onPause()
		{
			val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
			sensorManager.unregisterListener(sensorEventListener)
		}
	}

	private val coalescingLifecycleObserver = object: LifecycleObserver {
		@OnLifecycleEvent(Lifecycle.Event.ON_RESUME)
		fun onResume()
		{
			frameCallbacksRunning = true
		}

		@OnLifecycleEvent(Lifecycle.Event.ON_PAUSE)
		fun onPause()
		{
			frameCallbacksRunning = false
			mainScheduler.pause()
			synchronized(inputThreadSchedulers) {
				for(scheduler in inputThreadSchedulers.values)
					scheduler.handler.post { scheduler.pause() }
			}
		}
	}

	fun observe(lifecycleOwner: LifecycleOwner)
	{
		if(preferences.motionEnabled)
			lifecycleOwner.lifecycle.addObserver(motionLifecycleObserver)
		if(coalesceControllerInput)
			lifecycleOwner.lifecycle.addObserver(coalescingLifecycleObserver)
	}

	@Suppress("DEPRECATION")
	private fun currentDisplayRotation() = windowManager.defaultDisplay.rotation

	fun refreshDisplayRotation()
	{
		val rotation = currentDisplayRotation()
		synchronized(stateLock) { displayRotation = rotation }
	}

	/** With [stateLock] held, right after the change it reports. */
	private fun controllerStateUpdated()
	{
		if(!coalesceControllerInput)
		{
			controllerStateChangedCallback?.let { it(mergedControllerState()) }
			return
		}

		controllerStateDirty = true
		val looper = Looper.myLooper()
		if(looper == Looper.getMainLooper())
			mainScheduler.schedule()
		else if(flushOnInputThread && looper != null)
			inputThreadSchedulers.getOrPut(looper) { FlushScheduler(looper) }.schedule()
		else
			mainHandler.post { mainScheduler.schedule() }
	}

	private fun flushControllerState()
	{
		synchronized(stateLock)
		{
			if(!controllerStateDirty)
				return
			controllerStateDirty = false
			controllerStateChangedCallback?.let { it(mergedControllerState()) }
		}
	}

	fun dispatchKeyEvent(event: KeyEvent): Boolean = synchronized(stateLock) { mapKeyEvent(event) }

	private fun mapKeyEvent(event: KeyEvent): Boolean
	{
		//Log.i("StreamSession", "key event $event")
		if(event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP)
			return false

		val keyCode = event.keyCode
		val action = event.action == KeyEvent.ACTION_DOWN

		// Check for L2/R2 (can be digital or analog, here we handle digital key event)
		if (keyCode == preferences.mappingL2) {
			keyControllerState.l2State = if(action) UByte.MAX_VALUE else 0U
			controllerStateUpdated()
			return true
		}
		if (keyCode == preferences.mappingR2) {
			keyControllerState.r2State = if(action) UByte.MAX_VALUE else 0U
			controllerStateUpdated()
			return true
		}

		val buttonMask: UInt = when(keyCode)
		{
			preferences.mappingCross -> ControllerState.BUTTON_CROSS
			preferences.mappingCircle -> ControllerState.BUTTON_MOON
			preferences.mappingSquare -> ControllerState.BUTTON_BOX
			preferences.mappingTriangle -> ControllerState.BUTTON_PYRAMID
			preferences.mappingL1 -> ControllerState.BUTTON_L1
			preferences.mappingR1 -> ControllerState.BUTTON_R1
			preferences.mappingL3 -> ControllerState.BUTTON_L3
			preferences.mappingR3 -> ControllerState.BUTTON_R3
			preferences.mappingShare -> ControllerState.BUTTON_SHARE
			preferences.mappingOptions -> ControllerState.BUTTON_OPTIONS
			preferences.mappingPs -> ControllerState.BUTTON_PS
			else -> return false
		}

		// PLE-746: the probe times a press (Cross unless PLE-803's mask says otherwise) from its KeyEvent;
		// a no-op unless it runs.
		if(action && event.repeatCount == 0 && LatencyProbe.active)
			LatencyProbe.press(event.eventTime, buttonMask)

		keyControllerState.buttons = keyControllerState.buttons.run {
			if(action) this or buttonMask else this and buttonMask.inv()
		}

		controllerStateUpdated()
		return true
	}

	/**
	 * PLE-722: the Go's VR menu took the pad. The console sees every pad button up and both
	 * sticks and triggers at rest until the pad's next event after the menu closes.
	 */
	fun releasePad() = synchronized(stateLock)
	{
		for(state in listOf(keyControllerState, motionControllerState))
		{
			state.buttons = 0U
			state.l2State = 0U
			state.r2State = 0U
			state.leftX = 0
			state.leftY = 0
			state.rightX = 0
			state.rightY = 0
		}
		controllerStateUpdated()
	}

	/**
	 * PLE-746: debug builds only (StreamVrActivity's DEBUG_GO_VR_INPUT broadcast). Holds [buttons] down
	 * for [holdMs], then releases them: the Go's `input keyevent` cannot send the D-pad, whose presses
	 * only arrive as a gamepad's HAT motion, so parking the PS5 on a Settings toggle needs this.
	 */
	fun debugPress(buttons: UInt, holdMs: Long)
	{
		mainHandler.post {
			synchronized(stateLock)
			{
				keyControllerState.buttons = keyControllerState.buttons or buttons
				controllerStateUpdated()
			}
		}
		mainHandler.postDelayed({
			synchronized(stateLock)
			{
				keyControllerState.buttons = keyControllerState.buttons and buttons.inv()
				controllerStateUpdated()
			}
		}, holdMs)
	}

	fun onGenericMotionEvent(event: MotionEvent): Boolean = synchronized(stateLock) { mapMotionEvent(event) }

	private fun mapMotionEvent(event: MotionEvent): Boolean
	{
		if(event.source and InputDevice.SOURCE_CLASS_JOYSTICK != InputDevice.SOURCE_CLASS_JOYSTICK)
			return false
		fun Float.signedAxis() = (this * Short.MAX_VALUE).toInt().toShort()
		fun Float.unsignedAxis() = (this * UByte.MAX_VALUE.toFloat()).toUInt().toUByte()
		motionControllerState.leftX = event.getAxisValue(MotionEvent.AXIS_X).signedAxis()
		motionControllerState.leftY = event.getAxisValue(MotionEvent.AXIS_Y).signedAxis()
		motionControllerState.rightX = event.getAxisValue(MotionEvent.AXIS_Z).signedAxis()
		motionControllerState.rightY = event.getAxisValue(MotionEvent.AXIS_RZ).signedAxis()
		val triggerAxes = triggerAxisResolver.axesFor(event.deviceId)
		motionControllerState.l2State = event.getAxisValue(triggerAxes.l2).unsignedAxis()
		motionControllerState.r2State = event.getAxisValue(triggerAxes.r2).unsignedAxis()
		motionControllerState.buttons = motionControllerState.buttons.let {
			val dpadX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
			val dpadY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
			val dpadButtons =
				(if(dpadX > 0.5f) ControllerState.BUTTON_DPAD_RIGHT else 0U) or
						(if(dpadX < -0.5f) ControllerState.BUTTON_DPAD_LEFT else 0U) or
						(if(dpadY > 0.5f) ControllerState.BUTTON_DPAD_DOWN else 0U) or
						(if(dpadY < -0.5f) ControllerState.BUTTON_DPAD_UP else 0U)
			it and (ControllerState.BUTTON_DPAD_RIGHT or
					ControllerState.BUTTON_DPAD_LEFT or
					ControllerState.BUTTON_DPAD_DOWN or
					ControllerState.BUTTON_DPAD_UP).inv() or
					dpadButtons
		}
		//Log.i("StreamSession", "motionEvent => $motionControllerState")
		controllerStateUpdated()
		return true
	}
}
