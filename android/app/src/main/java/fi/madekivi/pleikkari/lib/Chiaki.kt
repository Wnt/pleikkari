package fi.madekivi.pleikkari.lib

import android.content.Context
import android.os.Parcelable
import android.util.Log
import android.view.Surface
import kotlinx.parcelize.Parcelize
import java.lang.Exception
import java.net.InetSocketAddress
import kotlin.math.abs

enum class Target(val value: Int)
{
	PS4_UNKNOWN(0),
	PS4_8(800),
	PS4_9(900),
	PS4_10(1000),
	PS5_UNKNOWN(1000000),
	PS5_1(1000100);

	companion object
	{
		/** The only target this fork registers: it is PS5 only. */
		val REGISTRATION = PS5_1

		/** Unknown values resolve to PS5; stored PS4 values still decode as PS4 so the app can flag them as unsupported. */
		@JvmStatic
		fun fromValue(value: Int) = values().firstOrNull { it.value == value } ?: REGISTRATION
	}

	val isPS5 get() = value >= PS5_UNKNOWN.value
}

enum class VideoResolutionPreset(val value: Int)
{
	RES_360P(1),
	RES_540P(2),
	RES_720P(3),
	RES_1080P(4)
}

enum class VideoFPSPreset(val value: Int)
{
	FPS_30(30),
	FPS_60(60)
}

enum class Codec(val value: Int)
{
	CODEC_H264(0),
	CODEC_H265(1),
	CODEC_H265_HDR(2)
}

@Parcelize
data class ConnectVideoProfile(
	val width: Int,
	val height: Int,
	val maxFPS: Int,
	val bitrate: Int,
	val codec: Codec
): Parcelable
{
	companion object
	{
		fun preset(resolutionPreset: VideoResolutionPreset, fpsPreset: VideoFPSPreset, codec: Codec)
				= ChiakiNative.videoProfilePreset(resolutionPreset.value, fpsPreset.value, codec)
	}
}

@Parcelize
data class AndroidChiakiVideoPresenterConfig(
	val pacingEnabled: Boolean = false,
	val pacingHighRefreshEnabled: Boolean = false,
	val pacingMode: Int = 2,
	val presenterLead: Int = 0,
	val boundedAgeEnabled: Boolean = false,
	val maxFrameAgePeriods: Int = 2,
	val nonblockingProducer: Boolean = false,
	val recoveryStrategy: Int = 0,
	val dejitterEnabled: Boolean = false,
	val dejitterFloorMs: Int = 12,
	val dejitterCapMs: Int = 32,
	val dejitterQueueAgeFrames: Int = 2,
	val dejitterHalfRateEnabled: Boolean = false
): Parcelable

@Parcelize
data class ConnectInfo(
	val ps5: Boolean,
	val host: String,
	val registKey: ByteArray,
	val morning: ByteArray,
	val videoProfile: ConnectVideoProfile,
	val decoderLowLatencyEnabled: Boolean,
	val threadPriorityBoostEnabled: Boolean,
	val decoderLateFrameRecoveryEnabled: Boolean,
	val packetLossMax: Double,
	val adaptiveLossReport: Boolean = false,
	val takionVideoPacketReorderingDisabled: Boolean,
	val feedbackStateMinIntervalMs: Int = 0,
	val feedbackStatsLogIntervalMs: Int = 0,
	val audioBufferBursts: Int = 0,
	val audioFifoMs: Int = 171,
	val autoRegister: Boolean = false,
	val performanceModeEnabled: Boolean = false,
	val decoderOperatingRate: Int = 0,
	val decoderOperatingRateDefault: Boolean = false,
	val decoderOperatingRateAuto: Boolean = true,
	val decoderRealtimePriority: Boolean = false,
	/** PLE-635: ask the Oculus Go's OMX.qcom decoder for vt-low-latency (decode-order output). */
	val decoderQcomVtLowLatency: Boolean = false,
	val videoTimestampRateHz: Int = 0,
	val streamDiagnosticsEnabled: Boolean = false,
	val videoPresenterConfig: AndroidChiakiVideoPresenterConfig = AndroidChiakiVideoPresenterConfig()
): Parcelable

data class NativeRemoteConnection(
	val controlFd: Int,
	val psnAccountId: ByteArray,
	val selectedAddress: String,
	val controlPort: Int,
	val data1: ByteArray,
	val data2: ByteArray,
	val customData1: ByteArray,
	val localAddress: String
)

data class VideoStats(
	val decoderInputFramesDropped: Long,
	val missedVsyncs: Long,
	val presenterFramesDropped: Long,
	val dejitterBufferNanos: Long
)

private class ChiakiNative
{
	data class CreateResult(var errorCode: Int, var ptr: Long)
	companion object
	{
		init
		{
			System.loadLibrary("chiaki-jni")
		}
		@JvmStatic external fun errorCodeToString(value: Int): String
		@JvmStatic external fun quitReasonToString(value: Int): String
		@JvmStatic external fun quitReasonIsError(value: Int): Boolean
		@JvmStatic external fun videoProfilePreset(resolutionPreset: Int, fpsPreset: Int, codec: Codec): ConnectVideoProfile
		@JvmStatic external fun sessionCreate(result: CreateResult, connectInfo: ConnectInfo, logFile: String?, logVerbose: Boolean, realVideoTimestamps: Boolean, decoderInputThread: Boolean, javaSession: Session)
		@JvmStatic external fun sessionCreateRemote(result: CreateResult, connectInfo: ConnectInfo, logFile: String?, logVerbose: Boolean, realVideoTimestamps: Boolean, decoderInputThread: Boolean,
			controlFd: Int, psnAccountId: ByteArray, selectedAddress: String, controlPort: Int,
			data1: ByteArray, data2: ByteArray, customData1: ByteArray, localAddress: String, javaSession: Session)
		@JvmStatic external fun sessionFree(ptr: Long)
		@JvmStatic external fun sessionStart(ptr: Long): Int
		@JvmStatic external fun sessionStop(ptr: Long): Int
		@JvmStatic external fun sessionJoin(ptr: Long): Int
		@JvmStatic external fun sessionSetRemoteDataSocket(ptr: Long, fd: Int): Int
		@JvmStatic external fun sessionSetSurface(ptr: Long, surface: Surface?, streamFps: Int,
			refreshHz: Double, appVsyncOffsetNanos: Long)
		@JvmStatic external fun sessionSetTiming(ptr: Long, streamFps: Int, refreshHz: Double,
			appVsyncOffsetNanos: Long)
		@JvmStatic external fun sessionSetPacingMode(ptr: Long, pacingMode: Int)
		@JvmStatic external fun sessionGetVideoStats(ptr: Long): VideoStats
		@JvmStatic external fun sessionSetControllerState(ptr: Long, controllerState: ControllerState)
		@JvmStatic external fun sessionSetLoginPin(ptr: Long, pin: String)
		@JvmStatic external fun discoveryServiceCreate(result: CreateResult, options: DiscoveryServiceOptions, javaService: DiscoveryService)
		@JvmStatic external fun discoveryServiceFree(ptr: Long)
		@JvmStatic external fun discoveryServiceWakeup(ptr: Long, host: String, userCredential: Long, ps5: Boolean)
		@JvmStatic external fun registStart(result: CreateResult, registInfo: RegistInfo, javaLog: ChiakiLog, javaRegist: Regist)
		@JvmStatic external fun registStop(ptr: Long)
		@JvmStatic external fun registFree(ptr: Long)
	}
}

class ErrorCode(val value: Int)
{
	override fun toString() = ChiakiNative.errorCodeToString(value)
	var isSuccess = value == 0
}

class ChiakiLog(val levelMask: Int, val callback: (level: Int, text: String) -> Unit)
{
	companion object
	{
		fun formatLog(level: Int, text: String) =
			"[${when(level)
				{
					Level.DEBUG.value -> "D"
					Level.VERBOSE.value -> "V"
					Level.INFO.value -> "I"
					Level.WARNING.value -> "W"
					Level.ERROR.value -> "E"
					else -> "?"
				}
			}] $text"
	}

	enum class Level(val value: Int)
	{
		DEBUG(1 shl 4),
		VERBOSE(1 shl 3),
		INFO(1 shl 2),
		WARNING(1 shl 1),
		ERROR(1 shl 0),
		ALL(0.inv())
	}

	private fun log(level: Int, text: String)
	{
		callback(level, text)
	}

	fun d(text: String) = log(Level.DEBUG.value, text)
	fun v(text: String) = log(Level.VERBOSE.value, text)
	fun i(text: String) = log(Level.INFO.value, text)
	fun w(text: String) = log(Level.WARNING.value, text)
	fun e(text: String) = log(Level.ERROR.value, text)
}

private fun maxAbs(a: Short, b: Short) = if(abs(a.toInt()) > abs(b.toInt())) a else b

private val CONTROLLER_TOUCHES_MAX = 2 // must be the same as CHIAKI_CONTROLLER_TOUCHES_MAX

data class ControllerTouch(
	var x: UShort = 0U,
	var y: UShort = 0U,
	var id: Byte = -1 // -1 = up
)

data class ControllerState constructor(
	var buttons: UInt = 0U,
	var l2State: UByte = 0U,
	var r2State: UByte = 0U,
	var leftX: Short = 0,
	var leftY: Short = 0,
	var rightX: Short = 0,
	var rightY: Short = 0,
	private var touchIdNext: UByte = 0U,
	var touches: Array<ControllerTouch> = arrayOf(ControllerTouch(), ControllerTouch()),
	var gyroX: Float = 0.0f,
	var gyroY: Float = 0.0f,
	var gyroZ: Float = 0.0f,
	var accelX: Float = 0.0f,
	var accelY: Float = 1.0f,
	var accelZ: Float = 0.0f,
	var orientX: Float = 0.0f,
	var orientY: Float = 0.0f,
	var orientZ: Float = 0.0f,
	var orientW: Float = 1.0f
){
	companion object
	{
		val BUTTON_CROSS 		= (1 shl 0).toUInt()
		val BUTTON_MOON 		= (1 shl 1).toUInt()
		val BUTTON_BOX 			= (1 shl 2).toUInt()
		val BUTTON_PYRAMID 		= (1 shl 3).toUInt()
		val BUTTON_DPAD_LEFT 	= (1 shl 4).toUInt()
		val BUTTON_DPAD_RIGHT	= (1 shl 5).toUInt()
		val BUTTON_DPAD_UP 		= (1 shl 6).toUInt()
		val BUTTON_DPAD_DOWN 	= (1 shl 7).toUInt()
		val BUTTON_L1 			= (1 shl 8).toUInt()
		val BUTTON_R1 			= (1 shl 9).toUInt()
		val BUTTON_L3			= (1 shl 10).toUInt()
		val BUTTON_R3			= (1 shl 11).toUInt()
		val BUTTON_OPTIONS		= (1 shl 12).toUInt()
		val BUTTON_SHARE 		= (1 shl 13).toUInt()
		val BUTTON_TOUCHPAD		= (1 shl 14).toUInt()
		val BUTTON_PS			= (1 shl 15).toUInt()
		val TOUCHPAD_WIDTH: UShort = 1920U
		val TOUCHPAD_HEIGHT: UShort = 942U
	}

	infix fun or(o: ControllerState) = ControllerState(
		buttons = buttons or o.buttons,
		l2State = maxOf(l2State, o.l2State),
		r2State = maxOf(r2State, o.r2State),
		leftX = maxAbs(leftX, o.leftX),
		leftY = maxAbs(leftY, o.leftY),
		rightX = maxAbs(rightX, o.rightX),
		rightY = maxAbs(rightY, o.rightY),
		touches = touches.zip(o.touches) { a, b -> if(a.id >= 0) a else b }.toTypedArray(),
		gyroX = gyroX,
		gyroY = gyroY,
		gyroZ = gyroZ,
		accelX = accelX,
		accelY = accelY,
		accelZ = accelZ,
		orientX = orientX,
		orientY = orientY,
		orientZ = orientZ,
		orientW = orientW
	)

	override fun equals(other: Any?): Boolean
	{
		if(this === other) return true
		if(javaClass != other?.javaClass) return false

		other as ControllerState

		if(buttons != other.buttons) return false
		if(l2State != other.l2State) return false
		if(r2State != other.r2State) return false
		if(leftX != other.leftX) return false
		if(leftY != other.leftY) return false
		if(rightX != other.rightX) return false
		if(rightY != other.rightY) return false
		if(touchIdNext != other.touchIdNext) return false
		if(!touches.contentEquals(other.touches)) return false
		if(gyroX != other.gyroX) return false
		if(gyroY != other.gyroY) return false
		if(gyroZ != other.gyroZ) return false
		if(accelX != other.accelX) return false
		if(accelY != other.accelY) return false
		if(accelZ != other.accelZ) return false
		if(orientX != other.orientX) return false
		if(orientY != other.orientY) return false
		if(orientZ != other.orientZ) return false
		if(orientW != other.orientW) return false

		return true
	}

	override fun hashCode(): Int
	{
		var result = buttons.hashCode()
		result = 31 * result + l2State.hashCode()
		result = 31 * result + r2State.hashCode()
		result = 31 * result + leftX
		result = 31 * result + leftY
		result = 31 * result + rightX
		result = 31 * result + rightY
		result = 31 * result + touchIdNext.hashCode()
		result = 31 * result + touches.contentHashCode()
		result = 31 * result + gyroX.hashCode()
		result = 31 * result + gyroY.hashCode()
		result = 31 * result + gyroZ.hashCode()
		result = 31 * result + accelX.hashCode()
		result = 31 * result + accelY.hashCode()
		result = 31 * result + accelZ.hashCode()
		result = 31 * result + orientX.hashCode()
		result = 31 * result + orientY.hashCode()
		result = 31 * result + orientZ.hashCode()
		result = 31 * result + orientW.hashCode()
		return result
	}

	fun startTouch(x: UShort, y: UShort): UByte? =
		touches
			.find { it.id < 0 }
			?.also {
				it.id = touchIdNext.toByte()
				it.x = x
				it.y = y
				touchIdNext = ((touchIdNext + 1U) and 0x7fU).toUByte()
			}?.id?.toUByte()

	fun stopTouch(id: UByte)
	{
		touches.find {
			it.id >= 0 && it.id == id.toByte()
		}?.let {
			it.id = -1
		}
	}

	fun setTouchPos(id: UByte, x: UShort, y: UShort): Boolean
		= touches.find {
			it.id >= 0 && it.id == id.toByte()
		}?.let {
			val r = it.x != x || it.y != y
			it.x = x
			it.y = y
			r
		} ?: false
}

class QuitReason(val value: Int)
{
	override fun toString() = ChiakiNative.quitReasonToString(value)

	val isError = ChiakiNative.quitReasonIsError(value)
}

sealed class Event
data class ConnectedEvent(
	/** session->holepunch_session || session->remote_connection: the PSN data plane, not the LAN. */
	val relay: Boolean,
	/** Address actually dialled; empty for the native-holepunch relay path (no single peer address). */
	val peerHost: String,
	val peerPort: Int,
	val mtuIn: Long,
	val rttUs: Long,
	/** False when mtuIn/rttUs are the senkusha-failed fallback (1454 / 1000us), not a measurement. */
	val measured: Boolean
): Event()
data class LoginPinRequestEvent(val pinIncorrect: Boolean): Event()
data class QuitEvent(val reason: QuitReason, val reasonString: String?): Event()
data class RumbleEvent(val left: UByte, val right: UByte): Event()
object RemoteDataSocketNeededEvent: Event()
data class RegistrationEvent(val host: RegistHost): Event()
data class StreamStatsEvent(
	val intervalMillis: Long,
	val rttMicros: Long,
	val streamFrames: Long,
	val decoderFrames: Long,
	val decodeMeanMicros: Long,
	val decodeP95Micros: Long,
	val decoderInputFramesDropped: Long,
	val presenterFramesDropped: Long,
	val missedVsyncs: Long,
	val videoFramesLost: Long,
	val reorderQueueTimeouts: Long,
	val videoPacketJitterMicros: Long,
	val takionPacketsReceived: Long,
	val takionPartialFrameUnitsMissing: Long,
	val feedbackPackets: Long,
	val dejitterBufferNanos: Long,
	val cadenceDepthNanos: Long,
	val cadenceTargetNanos: Long,
	val cadenceErrP50Nanos: Long,
	val cadenceErrP99Nanos: Long,
	val decodeEwmaNanos: Long,
	val cadenceWindowDrops: Long,
	val vsyncPeriodNanos: Long,
	val presenterQueueDepth: Long,
	val audioLatencyMicros: Long,
	val audioXruns: Long,
	val audioUnderruns: Long,
	val connectionQualityValid: Boolean = false,
	val targetBitrateBps: Long = 0,
	val measuredThroughputBps: Long = 0,
	/** ConnectionQualityPayload.rtt read as milliseconds. The console derives it by
	 * means we cannot see and its unit is unverified (PLE-343): diagnostics only. */
	val consoleRttMicros: Long = 0,
	val serverLoss: Long = 0,
	val congestionMeasuredLoss: Double = 0.0,
	val congestionReportedLoss: Double = 0.0,
	val cadenceHalfRateDetected: Boolean = false,
	/** The same console field exactly as decoded, for the capture log. */
	val consoleRttRaw: Double = 0.0,
	/** Our own round trip: 1 Hz heartbeat to console DATA_ACK on the stream socket. 0 = none yet. */
	val probeRttMicros: Long = 0,
	val probeRttSamples: Long = 0,
	val probeRttUnacked: Long = 0,
	val probeRttAmbiguous: Long = 0,
	/** PLE-403: false while videoPacketJitterMicros is one unsmoothed sample rather than an
	 * average -- see the comment on CHIAKI_TAKION_VIDEO_JITTER_FILL_SAMPLES. Defaults true so
	 * a caller not exercising warm-up (every existing test) keeps today's behaviour. */
	val videoPacketJitterFilled: Boolean = true,
	/** PLE-464: ms of silence on the console's socket still running at this 1 Hz poll. */
	val takionSilenceMillis: Long = 0,
	/** PLE-464: the longest gap between two inbound datagrams anywhere inside this window,
	 * in ms -- including one that opened and closed between two polls, which
	 * [takionSilenceMillis] cannot see. This is the classifier's only view of a total
	 * outage: [takionPartialFrameUnitsMissing] counts units missing from frames that partly arrived, so
	 * a second in which *nothing* arrived registers as 0 received and 0 lost, i.e. clean. */
	val takionMaxReceiveGapMillis: Long = 0,
	/** PLE-485: session-cumulative count of frames that arrived intact and were fully
	 * assembled, but discarded because they were a P-frame decoded while still waiting for
	 * an IDR. Not transport loss, so kept out of [videoFramesLost]. Nothing reads this yet
	 * -- whether the classifier should is PLE-486's question. */
	val videoFramesDiscardedForIdr: Long = 0
): Event()
{
	/** The round trip we can defend: the in-stream probe, else senkusha's startup ping, else nothing. */
	val measuredRttMicros: Long get() = if(probeRttMicros > 0L) probeRttMicros else rttMicros
	val measuredRttSource: String get() = when
	{
		probeRttMicros > 0L -> "probe"
		rttMicros > 0L -> "startup"
		else -> "none"
	}
}

class CreateError(val errorCode: ErrorCode): Exception("Failed to create a native object: $errorCode")

class Session(connectInfo: ConnectInfo, logFile: String?, logVerbose: Boolean, realVideoTimestamps: Boolean = false,
	decoderInputThread: Boolean = false, remoteConnection: NativeRemoteConnection? = null,
	context: Context? = null)
{
	interface EventCallback
	{
		fun sessionEvent(event: Event)
	}

	private var nativePtr: Long
	private val performanceHints = PerformanceHints.create(
		context,
		connectInfo.performanceModeEnabled,
		connectInfo.videoProfile.maxFPS
	)
	var eventCallback: ((event: Event) -> Unit)? = null

	init
	{
		val result = ChiakiNative.CreateResult(0, 0)
		if(remoteConnection == null)
			ChiakiNative.sessionCreate(result, connectInfo, logFile, logVerbose, realVideoTimestamps, decoderInputThread, this)
		else
			ChiakiNative.sessionCreateRemote(
				result, connectInfo, logFile, logVerbose, realVideoTimestamps, decoderInputThread,
				remoteConnection.controlFd, remoteConnection.psnAccountId,
				remoteConnection.selectedAddress, remoteConnection.controlPort,
				remoteConnection.data1, remoteConnection.data2, remoteConnection.customData1,
				remoteConnection.localAddress, this
			)
		val errorCode = ErrorCode(result.errorCode)
		if(!errorCode.isSuccess)
			throw CreateError(errorCode)
		nativePtr = result.ptr
	}

	fun start() = ErrorCode(ChiakiNative.sessionStart(nativePtr))
	fun stop() = ErrorCode(ChiakiNative.sessionStop(nativePtr))

	fun dispose(join: Boolean = true)
	{
		if(nativePtr == 0L)
			return
		if(join)
			ChiakiNative.sessionJoin(nativePtr)
		ChiakiNative.sessionFree(nativePtr)
		nativePtr = 0L
		performanceHints.close()
	}

	@Suppress("unused") // Called from the native decoder and presenter threads.
	private fun performanceHintThreadStarted(role: Int, tid: Int) = performanceHints.threadStarted(role, tid)

	@Suppress("unused") // Called from the native decoder and presenter threads.
	private fun performanceHintReportActualWorkDuration(role: Int, durationNanos: Long) =
		performanceHints.reportActualWorkDuration(role, durationNanos)

	@Suppress("unused") // Called from the native decoder and presenter threads.
	private fun performanceHintThreadStopped(role: Int) = performanceHints.threadStopped(role)

	@Suppress("unused") // Read by the native 1 Hz diagnostics logger.
	private fun isAdpfPerformanceModeLive() = performanceHints.isActive

	val adpfPerformanceModeLive get() = performanceHints.isActive

	@Volatile private var sustainedPerformanceModeLive = false

	@Suppress("unused") // Read by the native 1 Hz diagnostics logger.
	private fun isSustainedPerformanceModeLive() = sustainedPerformanceModeLive

	fun setSustainedPerformanceModeLive(live: Boolean)
	{
		sustainedPerformanceModeLive = live
	}

	private fun event(event: Event)
	{
		eventCallback?.let { it(event) }
	}

	private fun eventConnected(relay: Boolean, peerHost: String, peerPort: Int, mtuIn: Long, rttUs: Long, measured: Boolean)
	{
		event(ConnectedEvent(relay, peerHost, peerPort, mtuIn, rttUs, measured))
	}

	private fun eventLoginPinRequest(pinIncorrect: Boolean)
	{
		event(LoginPinRequestEvent(pinIncorrect))
	}

	private fun eventQuit(reasonValue: Int, reasonString: String?)
	{
		event(QuitEvent(QuitReason(reasonValue), reasonString))
	}

	private fun eventRumble(left: Int, right: Int)
	{
		event(RumbleEvent(left.toUByte(), right.toUByte()))
	}

	private fun eventRemoteDataSocketNeeded()
	{
		event(RemoteDataSocketNeededEvent)
	}

	private fun eventRegistrationSuccess(host: RegistHost)
	{
		event(RegistrationEvent(host))
	}

	@Suppress("LongParameterList", "unused") // Called from the single native 1 Hz stats event.
	private fun eventStreamStats(
		intervalMillis: Long,
		rttMicros: Long,
		streamFrames: Long,
		decoderFrames: Long,
		decodeMeanMicros: Long,
		decodeP95Micros: Long,
		decoderInputFramesDropped: Long,
		presenterFramesDropped: Long,
		missedVsyncs: Long,
		videoFramesLost: Long,
		reorderQueueTimeouts: Long,
		videoPacketJitterMicros: Long,
		takionPacketsReceived: Long,
		takionPartialFrameUnitsMissing: Long,
		feedbackPackets: Long,
		dejitterBufferNanos: Long,
		cadenceDepthNanos: Long,
		cadenceTargetNanos: Long,
		cadenceErrP50Nanos: Long,
		cadenceErrP99Nanos: Long,
		decodeEwmaNanos: Long,
		cadenceWindowDrops: Long,
		vsyncPeriodNanos: Long,
		presenterQueueDepth: Long,
		audioLatencyMicros: Long,
		audioXruns: Long,
		audioUnderruns: Long,
		connectionQualityValid: Boolean,
		targetBitrateBps: Long,
		measuredThroughputBps: Long,
		consoleRttMicros: Long,
		serverLoss: Long,
		congestionMeasuredLoss: Double,
		congestionReportedLoss: Double,
		cadenceHalfRateDetected: Boolean,
		consoleRttRaw: Double,
		probeRttMicros: Long,
		probeRttSamples: Long,
		probeRttUnacked: Long,
		probeRttAmbiguous: Long,
		videoPacketJitterFilled: Boolean,
		takionSilenceMillis: Long,
		takionMaxReceiveGapMillis: Long,
		videoFramesDiscardedForIdr: Long
	)
	{
		event(StreamStatsEvent(
			intervalMillis = intervalMillis,
			rttMicros = rttMicros,
			streamFrames = streamFrames,
			decoderFrames = decoderFrames,
			decodeMeanMicros = decodeMeanMicros,
			decodeP95Micros = decodeP95Micros,
			decoderInputFramesDropped = decoderInputFramesDropped,
			presenterFramesDropped = presenterFramesDropped,
			missedVsyncs = missedVsyncs,
			videoFramesLost = videoFramesLost,
			reorderQueueTimeouts = reorderQueueTimeouts,
			videoPacketJitterMicros = videoPacketJitterMicros,
			takionPacketsReceived = takionPacketsReceived,
			takionPartialFrameUnitsMissing = takionPartialFrameUnitsMissing,
			feedbackPackets = feedbackPackets,
			dejitterBufferNanos = dejitterBufferNanos,
			cadenceDepthNanos = cadenceDepthNanos,
			cadenceTargetNanos = cadenceTargetNanos,
			cadenceErrP50Nanos = cadenceErrP50Nanos,
			cadenceErrP99Nanos = cadenceErrP99Nanos,
			decodeEwmaNanos = decodeEwmaNanos,
			cadenceWindowDrops = cadenceWindowDrops,
			vsyncPeriodNanos = vsyncPeriodNanos,
			presenterQueueDepth = presenterQueueDepth,
			audioLatencyMicros = audioLatencyMicros,
			audioXruns = audioXruns,
			audioUnderruns = audioUnderruns,
			connectionQualityValid = connectionQualityValid,
			targetBitrateBps = targetBitrateBps,
			measuredThroughputBps = measuredThroughputBps,
			consoleRttMicros = consoleRttMicros,
			serverLoss = serverLoss,
			congestionMeasuredLoss = congestionMeasuredLoss,
			congestionReportedLoss = congestionReportedLoss,
			cadenceHalfRateDetected = cadenceHalfRateDetected,
			consoleRttRaw = consoleRttRaw,
			probeRttMicros = probeRttMicros,
			probeRttSamples = probeRttSamples,
			probeRttUnacked = probeRttUnacked,
			probeRttAmbiguous = probeRttAmbiguous,
			videoPacketJitterFilled = videoPacketJitterFilled,
			takionSilenceMillis = takionSilenceMillis,
			takionMaxReceiveGapMillis = takionMaxReceiveGapMillis,
			videoFramesDiscardedForIdr = videoFramesDiscardedForIdr
		))
	}

	/** Native takes ownership of fd only when the returned error is successful. */
	fun setRemoteDataSocket(fd: Int) = ErrorCode(ChiakiNative.sessionSetRemoteDataSocket(nativePtr, fd))

	fun setSurface(surface: Surface?, streamFps: Int, refreshHz: Double, appVsyncOffsetNanos: Long)
	{
		ChiakiNative.sessionSetSurface(nativePtr, surface, streamFps, refreshHz, appVsyncOffsetNanos)
	}

	fun setTiming(streamFps: Int, refreshHz: Double, appVsyncOffsetNanos: Long)
	{
		ChiakiNative.sessionSetTiming(nativePtr, streamFps, refreshHz, appVsyncOffsetNanos)
	}

	fun setPacingMode(pacingMode: Int) = ChiakiNative.sessionSetPacingMode(nativePtr, pacingMode)

	fun getVideoStats() = ChiakiNative.sessionGetVideoStats(nativePtr)

	fun setControllerState(controllerState: ControllerState)
	{
		ChiakiNative.sessionSetControllerState(nativePtr, controllerState)
	}

	fun setLoginPin(pin: String)
	{
		ChiakiNative.sessionSetLoginPin(nativePtr, pin)
	}
}

data class DiscoveryHost(
	val state: State,
	val hostRequestPort: UShort,
	val hostAddr: String?,
	val systemVersion: String?,
	val deviceDiscoveryProtocolVersion: String?,
	val hostName: String?,
	val hostType: String?,
	val hostId: String?,
	val runningAppTitleid: String?,
	val runningAppName: String?)
{
	enum class State
	{
		UNKNOWN,
		READY,
		STANDBY
	}
	
	val isPS5 get() = deviceDiscoveryProtocolVersion == "00030010"
}


data class DiscoveryServiceOptions(
	val hostsMax: ULong,
	val hostDropPings: ULong,
	val pingMs: ULong,
	val sendAddr: InetSocketAddress
)

class DiscoveryService(
	options: DiscoveryServiceOptions,
	val callback: ((hosts: List<DiscoveryHost>) -> Unit)?)
{
	companion object
	{
		fun wakeup(service: DiscoveryService?, host: String, userCredential: ULong, ps5: Boolean) =
			ChiakiNative.discoveryServiceWakeup(service?.nativePtr ?: 0, host, userCredential.toLong(), ps5)
	}

	private var nativePtr: Long

	init
	{
		val result = ChiakiNative.CreateResult(0, 0)
		ChiakiNative.discoveryServiceCreate(result, options, this)
		val errorCode = ErrorCode(result.errorCode)
		if(!errorCode.isSuccess)
			throw CreateError(errorCode)
		nativePtr = result.ptr
	}

	fun dispose()
	{
		if(nativePtr == 0L)
			return
		ChiakiNative.discoveryServiceFree(nativePtr)
		nativePtr = 0L
	}

	private fun hostsUpdated(hosts: Array<DiscoveryHost>)
	{
		val hostsList = hosts.toList()
		Log.i("Chiaki", "got hosts from native: $hostsList")
		callback?.let { it(hostsList) }
	}

}

@Parcelize
data class RegistInfo(
	val target: Target,
	val host: String,
	val broadcast: Boolean,
	val psnOnlineId: String?,
	val psnAccountId: ByteArray?,
	val pin: Int
): Parcelable
{
	companion object
	{
		const val ACCOUNT_ID_SIZE = 8

		/** New registrations have no target choice: always PS5, identified by PSN account ID. */
		fun forPS5(host: String, broadcast: Boolean, psnAccountId: ByteArray, pin: Int) =
			RegistInfo(Target.REGISTRATION, host, broadcast, null, psnAccountId, pin)
	}
}

data class RegistHost(
	val target: Target,
	val apSsid: String,
	val apBssid: String,
	val apKey: String,
	val apName: String,
	val serverMac: ByteArray,
	val serverNickname: String,
	val rpRegistKey: ByteArray,
	val rpKeyType: UInt,
	val rpKey: ByteArray
)

sealed class RegistEvent
object RegistEventCanceled: RegistEvent()
object RegistEventFailed: RegistEvent()
class RegistEventSuccess(val host: RegistHost): RegistEvent()

class Regist(
	info: RegistInfo,
	log: ChiakiLog,
	val callback: (RegistEvent) -> Unit
)
{
	private var nativePtr: Long

	init
	{
		val result = ChiakiNative.CreateResult(0, 0)
		ChiakiNative.registStart(result, info, log, this)
		val errorCode = ErrorCode(result.errorCode)
		if(!errorCode.isSuccess)
			throw CreateError(errorCode)
		nativePtr = result.ptr
	}

	fun stop()
	{
		ChiakiNative.registStop(nativePtr)
	}

	fun dispose()
	{
		if(nativePtr == 0L)
			return
		ChiakiNative.registFree(nativePtr)
		nativePtr = 0L
	}

	private fun event(event: RegistEvent)
	{
		callback(event)
	}
}
