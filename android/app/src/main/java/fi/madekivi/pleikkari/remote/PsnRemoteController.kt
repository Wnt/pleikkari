// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
@file:OptIn(kotlin.io.encoding.ExperimentalEncodingApi::class)

package fi.madekivi.pleikkari.remote

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.ReceiveChannel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import fi.madekivi.pleikkari.lib.RegistHost
import java.io.Closeable
import java.security.SecureRandom
import java.util.UUID
import kotlin.io.encoding.Base64

sealed interface PsnNativeStartResult
{
	data class Registered(val host: RegistHost) : PsnNativeStartResult
	data object DataSocketNeeded : PsnNativeStartResult
}

interface PsnRemoteNativeBridge
{
	/** The implementation duplicates/detaches the socket FD and only takes ownership after native success. */
	suspend fun start(control: PsnPunchedSocket, registration: PsnRegistrationMaterial): PsnNativeStartResult
	suspend fun setDataSocket(data: PsnPunchedSocket)
	fun stop() = Unit
}

fun interface PsnRandomBytes
{
	fun next(size: Int): ByteArray
}

/**
 * The PSN control plane. Every public suspend function runs its whole body on [ioDispatcher], so the
 * caller's dispatcher (the main thread, for a ViewModel) never performs a socket read, a DNS lookup or
 * a UDP exchange, whichever collaborator does it (PLE-261, PLE-312). Callbacks out of [nativeBridge]
 * therefore arrive on IO and must post to the UI rather than set it. `PsnRemoteControllerTest`
 * checks each public entry point by name, so a new one has to be covered there too.
 */
class PsnRemoteController(
	private val api: PsnRemoteApi,
	private val pushTransport: PsnPushTransport,
	private val holePuncher: PsnHolePuncher,
	private val nativeBridge: PsnRemoteNativeBridge,
	private val randomBytes: PsnRandomBytes = PsnRandomBytes { size -> ByteArray(size).also(SecureRandom()::nextBytes) },
	private val uuid: () -> String = { UUID.randomUUID().toString() },
	private val json: Json = api.json,
	private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
	/** One line per stage and signalling message, for logcat: a device capture must say where a link stopped. */
	private val trace: (String) -> Unit = {}
) : Closeable
{
	private val _state = MutableStateFlow<PsnRemoteState>(PsnRemoteState.Idle)
	val state: StateFlow<PsnRemoteState> = _state.asStateFlow()

	private var current: PsnRemoteState
		get() = _state.value
		set(value)
		{
			_state.value = value
			trace("stage ${value::class.simpleName}")
		}

	private var push: PsnPushConnection? = null
	private var session: PsnSession? = null
	private var notificationQueue: NotificationQueue? = null
	private val openSockets = mutableListOf<PsnPunchedSocket>()
	private var requestId = 1

	suspend fun listDevices(): List<PsnDevice> = withContext(ioDispatcher) {
		current = PsnRemoteState.ListingDevices
		try
		{
			val devices = api.listDevices()
			current = PsnRemoteState.Devices(devices)
			devices
		}
		catch(error: Throwable)
		{
			current = PsnRemoteState.Failed(error.message ?: "Unable to list PSN consoles", error)
			throw error
		}
	}

	suspend fun connect(device: PsnDevice): Unit = withContext(ioDispatcher) {
		check(session == null) { "A PSN remote session is already active" }
		try
		{
			// Like upstream, have our side of the OFFER ready before the console is started (PLE-319).
			holePuncher.discoverAhead()
			val (created, registration) = prepareConsole(device)

			val control = signalAndPunch(created, device, isData = false)
			openSockets += control
			holePuncher.discoverAhead()
			current = PsnRemoteState.ControlPunched(control.candidate)
			current = PsnRemoteState.NativeStarting
			when(val result = nativeBridge.start(control, registration))
			{
				is PsnNativeStartResult.Registered ->
				{
					holePuncher.discardAhead()
					cleanup(PsnRemoteState.Registered(result.host))
				}
				PsnNativeStartResult.DataSocketNeeded ->
				{
					current = PsnRemoteState.AwaitingDataSocket
					val data = signalAndPunch(created, device, isData = true)
					openSockets += data
					current = PsnRemoteState.DataPunched(data.candidate)
					nativeBridge.setDataSocket(data)
					current = PsnRemoteState.Streaming
				}
			}
		}
		catch(timeout: TimeoutCancellationException)
		{
			val error = timedOut(timeout)
			val failed = PsnRemoteState.Failed(error.message!!, error)
			current = failed
			cleanup(failed)
			throw error
		}
		catch(cancelled: CancellationException)
		{
			current = PsnRemoteState.Cancelling
			cleanup()
			throw cancelled
		}
		catch(error: Throwable)
		{
			val failed = PsnRemoteState.Failed(error.message ?: "PSN remote connection failed", error)
			current = failed
			cleanup(failed)
			throw error
		}
	}

	/** A step's withTimeout expired: a failure to report, never a cancel (see [PsnRemoteTimeoutException]). */
	private fun timedOut(timeout: TimeoutCancellationException) =
		PsnRemoteTimeoutException("The console did not answer in time (timed out during ${current::class.simpleName})", timeout)

	/** Sends the PSN remote-play command, which wakes the console, then closes the temporary session. */
	suspend fun wake(device: PsnDevice): Unit = withContext(ioDispatcher) {
		check(session == null) { "A PSN remote session is already active" }
		try
		{
			prepareConsole(device)
			cleanup(PsnRemoteState.Woken)
		}
		catch(timeout: TimeoutCancellationException)
		{
			val error = timedOut(timeout)
			cleanup(PsnRemoteState.Failed(error.message!!, error))
			throw error
		}
		catch(cancelled: CancellationException)
		{
			cleanup()
			throw cancelled
		}
		catch(error: Throwable)
		{
			val failed = PsnRemoteState.Failed(error.message ?: "Unable to wake PSN console", error)
			cleanup(failed)
			throw error
		}
	}

	suspend fun disconnect(): Unit = withContext(ioDispatcher) {
		current = PsnRemoteState.Cancelling
		cleanup()
	}

	private suspend fun prepareConsole(device: PsnDevice): Pair<PsnSession, PsnRegistrationMaterial>
	{
		current = PsnRemoteState.ResolvingPushServer
		val pushUrl = api.resolvePushWebSocket()
		current = PsnRemoteState.OpeningWebSocket
		val connection = withTimeout(30_000) { pushTransport.open(pushUrl, api.accessToken()) }
		push = connection
		notificationQueue = NotificationQueue(connection.notifications)

		current = PsnRemoteState.CreatingSession
		val created = api.createSession(uuid())
		session = created
		awaitClientJoin()
		current = PsnRemoteState.ClientJoined

		val data1 = randomBytes.next(16)
		val data2 = randomBytes.next(16)
		current = PsnRemoteState.StartingConsole
		api.startConsole(created, device, Base64.Default.encode(data1), Base64.Default.encode(data2))
		val customData1 = awaitConsoleJoin(device)
		current = PsnRemoteState.ConsoleJoined
		return created to PsnRegistrationMaterial(created.accountId, data1, data2, customData1)
	}

	private suspend fun awaitClientJoin() = withTimeout(30_000) {
		var created = false
		var member = false
		while(!created || !member)
		{
			val notification = queue().take { value ->
				value.dataType() == SESSION_CREATED || value.dataType() == MEMBER_CREATED
			}
			when(notification.dataType())
			{
				SESSION_CREATED -> created = true
				MEMBER_CREATED -> member = true
			}
		}
	}

	private suspend fun awaitConsoleJoin(device: PsnDevice): ByteArray = withTimeout(30_000) {
		var joined = false
		var custom: ByteArray? = null
		while(!joined || custom == null)
		{
			val notification = queue().take { value ->
				value.dataType() == MEMBER_CREATED || value.dataType() == CUSTOM_DATA
			}
			when(notification.dataType())
			{
				MEMBER_CREATED ->
				{
					val duid = notification.path("body", "data", "members")?.jsonArray?.firstOrNull()?.jsonObject
						?.get("deviceUniqueId")?.jsonPrimitive?.contentOrNull
					if(duid.equals(device.duid, ignoreCase = true)) joined = true
				}
				CUSTOM_DATA -> custom = decodeCustomData(notification.path("body", "data", "customData1")?.jsonPrimitive?.contentOrNull)
			}
		}
		custom
	}

	private suspend fun signalAndPunch(session: PsnSession, device: PsnDevice, isData: Boolean): PsnPunchedSocket
	{
		current = if(isData) PsnRemoteState.DataSignaling else PsnRemoteState.ControlSignaling
		val consoleOffer = awaitSignal("OFFER")
		val peer = consoleOffer.connRequest ?: throw PsnRemoteProtocolException("Console OFFER did not contain connRequest")
		send(session, device, PsnSignalMessage("RESULT", consoleOffer.reqId))

		val preparation = holePuncher.prepare(peer, session.accountId)
		try
		{
			// Upstream's request ids: `local_req_id` starts at 1 (holepunch.c:783), building the offer
			// consumes one (:2740-2741) and sending it takes the next (:1596-1598), then ACCEPT takes one
			// more (:1647, :1663). Its OFFERs therefore carry 2 and 5 and its ACCEPTs 3 and 6; mirror that
			// so a device capture compares like for like (PLE-327).
			requestId++
			val offerRequestId = requestId++
			send(session, device, PsnSignalMessage("OFFER", offerRequestId, connRequest = preparation.offer))
			awaitSignal("RESULT", offerRequestId)
			current = if(isData) PsnRemoteState.DataProbing else PsnRemoteState.ControlProbing
			val punched = preparation.punch()
			trace("punched ${describe(punched.candidate)}")
			val acceptRequestId = requestId++
			send(session, device, PsnSignalMessage("ACCEPT", acceptRequestId, connRequest = acceptRequest(preparation.offer, peer, punched.candidate)))
			val consoleAccept = awaitSignal("ACCEPT")
			send(session, device, PsnSignalMessage("RESULT", consoleAccept.reqId))
			preparation.settle()
			return punched
		}
		finally { preparation.close() }
	}

	/**
	 * Upstream `wait_for_session_message` (holepunch.c:5387-5447): a TERMINATE ends the exchange, any
	 * other action than the awaited one is logged and dropped (:5433-5440), and `wait_for_session_message_ack`
	 * (:5457-5500) drops a RESULT for another request id the same way. The console re-sends its OFFER about
	 * a second after the first while PSN is still delivering our RESULT (both PLE-327 captures); upstream
	 * never answers that repeat, so neither do we (PLE-313 answered it with a second RESULT).
	 */
	private suspend fun awaitSignal(action: String, reqId: Int? = null): PsnSignalMessage = withTimeout(30_000) {
		while(true)
		{
			val notification = queue().take { it.dataType() == SESSION_MESSAGE }
			val payload = notification.path("body", "data", "sessionMessage", "payload")?.jsonPrimitive?.contentOrNull
				?: throw PsnRemoteProtocolException("PSN signaling notification did not contain payload")
			trace("received raw ${redact(payload)}")
			val message = decodeSignal(json, payload)
			trace("received ${describe(message)}")
			if(message.action == "TERMINATE")
				throw PsnRemoteProtocolException(
					"Console terminated PSN candidate exchange while ${current::class.simpleName} awaited $action (error ${message.error})"
				)
			if(message.action == action && (reqId == null || message.reqId == reqId)) return@withTimeout message
			trace("ignoring ${describe(message)} while awaiting $action" + (reqId?.let { " reqId=$it" } ?: ""))
		}
		@Suppress("UNREACHABLE_CODE")
		error("unreachable")
	}

	private suspend fun send(session: PsnSession, device: PsnDevice, message: PsnSignalMessage)
	{
		trace("sending ${describe(message)}")
		trace("sending raw ${redact(api.signalEnvelope(session, device, message))}")
		api.sendSignal(session, device, message)
	}

	private fun describe(message: PsnSignalMessage): String =
		"${message.action} reqId=${message.reqId} error=${message.error}" + (message.connRequest?.let { request ->
			" sid=${request.sid} peerSid=${request.peerSid} natType=${request.natType} " +
				request.candidate.joinToString(prefix = "[", postfix = "]", transform = ::describe)
		} ?: "")

	private fun describe(candidate: PsnCandidate): String =
		"${candidate.type} ${candidate.addr}:${candidate.port} mapped ${candidate.mappedAddress}:${candidate.mappedPort}"

	private fun decodeCustomData(value: String?): ByteArray
	{
		if(value == null) throw PsnRemoteProtocolException("customData1 notification did not contain data")
		val first = runCatching { Base64.Default.decode(value) }
			.getOrElse { throw PsnRemoteProtocolException("Invalid outer customData1 encoding", it) }
		val second = runCatching { Base64.Default.decode(first.decodeToString()) }
			.getOrElse { throw PsnRemoteProtocolException("Invalid inner customData1 encoding", it) }
		if(second.size !in 16..20) throw PsnRemoteProtocolException("customData1 decoded to ${second.size} bytes")
		return second.copyOf(16)
	}

	private suspend fun cleanup(finalState: PsnRemoteState = PsnRemoteState.Idle) = withContext(NonCancellable) {
		current = PsnRemoteState.DeletingSession
		holePuncher.discardAhead()
		nativeBridge.stop()
		val active = session
		if(active != null) runCatching { withTimeout(3_000) { api.deleteSession(active.sessionId) } }
		push?.close()
		openSockets.forEach { it.socket.close() }
		openSockets.clear()
		push = null
		session = null
		notificationQueue = null
		requestId = 1
		current = finalState
	}

	private fun queue(): NotificationQueue = notificationQueue ?: error("PSN push channel is not open")

	override fun close()
	{
		holePuncher.discardAhead()
		nativeBridge.stop()
		push?.close()
		openSockets.forEach { it.socket.close() }
		openSockets.clear()
		push = null
		session = null
		notificationQueue = null
		requestId = 1
		current = PsnRemoteState.Idle
	}

	private class NotificationQueue(private val channel: ReceiveChannel<JsonObject>)
	{
		private val pending = ArrayDeque<JsonObject>()

		suspend fun take(predicate: (JsonObject) -> Boolean): JsonObject
		{
			val pendingIterator = pending.iterator()
			while(pendingIterator.hasNext())
			{
				val value = pendingIterator.next()
				if(predicate(value))
				{
					pendingIterator.remove()
					return value
				}
			}
			while(true)
			{
				val value = channel.receive()
				if(predicate(value)) return value
				pending += value
			}
		}
	}

	companion object
	{
		private const val SESSION_CREATED = "psn:sessionManager:sys:remotePlaySession:created"
		private const val MEMBER_CREATED = "psn:sessionManager:sys:rps:members:created"
		private const val CUSTOM_DATA = "psn:sessionManager:sys:rps:customData1:updated"
		private const val SESSION_MESSAGE = "psn:sessionManager:sys:rps:sessionMessage:created"

		/**
		 * Upstream `send_accept`: the console's candidate that answered, carrying in its mapped fields the
		 * candidate of ours it reached, with a zero skey, NAT type 0 and no hashed id (PLE-313).
		 */
		internal fun acceptRequest(offer: PsnConnectionRequest, peer: PsnConnectionRequest, selected: PsnCandidate) =
			PsnConnectionRequest(
				sid = offer.sid,
				peerSid = peer.sid,
				skey = Base64.Default.encode(ByteArray(16)),
				natType = 0,
				candidate = listOf(selected),
				localPeerAddr = offer.localPeerAddr,
				localHashedId = ""
			)

		/**
		 * The body of a `ver=1.0, type=text, body=...` payload. Sony's clients write `"localPeerAddr":,` when
		 * they have none, and a RESULT carries `"connRequest":{}` (upstream `short_message_serialize`).
		 */
		internal fun decodeSignal(json: Json, payload: String): PsnSignalMessage
		{
			val body = payload.substringAfter("body=", missingDelimiterValue = "")
				.replace("\"localPeerAddr\":,", "\"localPeerAddr\":{}")
				.replace("\"connRequest\":{}", "\"connRequest\":null")
			if(body.isEmpty()) throw PsnRemoteProtocolException("PSN signaling payload did not contain a body")
			return runCatching { json.decodeFromString<PsnSignalMessage>(body) }
				.getOrElse { throw PsnRemoteProtocolException("Invalid PSN signaling message", it) }
		}

		private val quotedAccountId = Regex("""("accountId\\?"\s*:\s*\\?")[^"\\]*""")
		private val bareAccountId = Regex("""("accountId\\?"\s*:\s*)-?\d+""")

		/**
		 * A signaling envelope or payload with every `accountId` value blanked, whether quoted, bare or
		 * inside the JSON-escaped `body=`. Tokens, `rp_regist_key` and `rp_key` never travel in these
		 * messages (upstream `session_message_envelope_fmt`, `session_connrequest_fmt`), so the account id
		 * is the one secret to strip before the raw JSON goes to logcat (PLE-327).
		 */
		internal fun redact(raw: String): String =
			bareAccountId.replace(quotedAccountId.replace(raw, "$1<redacted>"), "$1\"<redacted>\"")

		private fun JsonObject.dataType(): String? = this["dataType"]?.jsonPrimitive?.contentOrNull

		private fun JsonObject.path(vararg parts: String): kotlinx.serialization.json.JsonElement?
		{
			var current: kotlinx.serialization.json.JsonElement = this
			for(part in parts) current = (current as? JsonObject)?.get(part) ?: return null
			return current
		}
	}
}
