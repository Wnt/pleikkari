// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
@file:OptIn(kotlin.io.encoding.ExperimentalEncodingApi::class)

package fi.madekivi.pleikkari.remote

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.Closeable
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom
import kotlin.io.encoding.Base64

data class PsnPunchedSocket(val socket: DatagramSocket, val candidate: PsnCandidate)

interface PsnPunchPreparation : Closeable
{
	val offer: PsnConnectionRequest

	/**
	 * Finds the console candidate that answers and keeps answering the console's own checks until it
	 * goes quiet. The returned candidate is the console's, with mappedAddr/mappedPort naming the
	 * candidate of ours it reached: that is what ACCEPT carries (upstream `check_candidates`).
	 */
	suspend fun punch(): PsnPunchedSocket

	/** Answers the console's checks that follow the ACCEPT exchange, before native takes the socket. */
	suspend fun settle() = Unit
}

interface PsnHolePuncher
{
	suspend fun prepare(peer: PsnConnectionRequest, accountId: String): PsnPunchPreparation

	/**
	 * Starts the peer-independent part of the next [prepare] (socket and STUN mapping) now, so it
	 * overlaps the wait for the console's OFFER instead of following it (PLE-319). It runs detached from
	 * the caller: a blocking STUN receive cannot be interrupted, and nobody should wait on a discarded one.
	 */
	fun discoverAhead() = Unit

	/** Drops a [discoverAhead] that no [prepare] will consume, closing its socket. */
	fun discardAhead() = Unit
}

object PsnStunCodec
{
	private const val COOKIE = 0x2112A442

	fun request(transactionId: ByteArray): ByteArray
	{
		require(transactionId.size == 12)
		return ByteBuffer.allocate(20).order(ByteOrder.BIG_ENDIAN)
			.putShort(0x0001).putShort(0).putInt(COOKIE).put(transactionId).array()
	}

	fun mappedAddress(response: ByteArray, transactionId: ByteArray): InetSocketAddress
	{
		if(response.size < 20) throw PsnRemoteProtocolException("Short STUN response")
		val buffer = ByteBuffer.wrap(response).order(ByteOrder.BIG_ENDIAN)
		if(buffer.short.toInt() and 0xffff != 0x0101) throw PsnRemoteProtocolException("Unexpected STUN response type")
		val length = buffer.short.toInt() and 0xffff
		if(length + 20 > response.size) throw PsnRemoteProtocolException("Truncated STUN response")
		if(buffer.int != COOKIE) throw PsnRemoteProtocolException("Invalid STUN magic cookie")
		val responseId = ByteArray(12).also(buffer::get)
		if(!responseId.contentEquals(transactionId)) throw PsnRemoteProtocolException("STUN transaction ID mismatch")
		var position = 20
		while(position + 4 <= 20 + length)
		{
			val type = ((response[position].toInt() and 0xff) shl 8) or (response[position + 1].toInt() and 0xff)
			val attributeLength = ((response[position + 2].toInt() and 0xff) shl 8) or (response[position + 3].toInt() and 0xff)
			if(position + 4 + attributeLength > response.size) throw PsnRemoteProtocolException("Truncated STUN attribute")
			if((type == 0x0020 || type == 0x0001) && attributeLength >= 8)
			{
				val family = response[position + 5].toInt() and 0xff
				if(family != 0x01) throw PsnRemoteProtocolException("STUN returned a non-IPv4 address")
				var port = ((response[position + 6].toInt() and 0xff) shl 8) or (response[position + 7].toInt() and 0xff)
				val address = response.copyOfRange(position + 8, position + 12)
				if(type == 0x0020)
				{
					port = port xor (COOKIE ushr 16)
					val cookie = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN).putInt(COOKIE).array()
					for(index in address.indices) address[index] = (address[index].toInt() xor cookie[index].toInt()).toByte()
				}
				return InetSocketAddress(InetAddress.getByAddress(address), port)
			}
			position += 4 + ((attributeLength + 3) and 3.inv())
		}
		throw PsnRemoteProtocolException("STUN response did not contain a mapped address")
	}
}

object PsnCandidateHandshake
{
	const val SIZE = 88
	private const val REQUEST = 0x06000000
	private const val RESPONSE = 0x07000000

	fun request(localHashedId: ByteArray, peerHashedId: ByteArray, localSid: Int, peerSid: Int, requestId: ByteArray): ByteArray =
		packet(REQUEST, localHashedId, peerHashedId, localSid, peerSid, requestId)

	fun response(
		request: ByteArray,
		localHashedId: ByteArray,
		peerHashedId: ByteArray,
		localSid: Int,
		peerSid: Int,
		peerAddress: InetSocketAddress? = null
	): ByteArray
	{
		if(!isRequest(request)) throw PsnRemoteProtocolException("Candidate packet is not a request")
		return packet(RESPONSE, localHashedId, peerHashedId, localSid, peerSid, request.copyOfRange(0x4b, 0x50)).also { response ->
			val address = peerAddress?.address?.address
			if(address != null && address.size == 4)
			{
				val encoded = ByteBuffer.wrap(response).order(ByteOrder.BIG_ENDIAN)
				encoded.putShort(0x50, localSid.toShort())
				encoded.putShort(0x52, peerSid.toShort())
				encoded.putShort(0x54, localSid.toShort())
				for(index in address.indices) response[0x50 + index] = (response[0x50 + index].toInt() xor address[index].toInt()).toByte()
				val port = byteArrayOf((peerAddress.port ushr 8).toByte(), peerAddress.port.toByte())
				for(index in port.indices) response[0x54 + index] = (response[0x54 + index].toInt() xor port[index].toInt()).toByte()
			}
		}
	}

	fun isRequest(packet: ByteArray): Boolean = packet.size == SIZE && type(packet) == REQUEST
	fun isResponse(packet: ByteArray): Boolean = packet.size == SIZE && type(packet) == RESPONSE
	fun isResponse(packet: ByteArray, requestId: ByteArray): Boolean =
		isResponse(packet) && packet.copyOfRange(0x4b, 0x50).contentEquals(requestId)

	private fun packet(type: Int, local: ByteArray, peer: ByteArray, localSid: Int, peerSid: Int, requestId: ByteArray): ByteArray
	{
		require(local.size == 20 && peer.size == 20 && requestId.size == 5)
		return ByteBuffer.allocate(SIZE).order(ByteOrder.BIG_ENDIAN).apply {
			putInt(0, type)
			position(0x04); put(local)
			position(0x24); put(peer)
			putShort(0x44, localSid.toShort())
			putShort(0x46, peerSid.toShort())
			position(0x4b); put(requestId)
		}.array()
	}

	private fun type(packet: ByteArray): Int = ByteBuffer.wrap(packet, 0, 4).order(ByteOrder.BIG_ENDIAN).int
}

/**
 * The STUN servers are kept unresolved until [prepare] runs on [Dispatchers.IO]. `InetSocketAddress(host, port)`
 * resolves the name in the constructor, and this puncher is constructed on the main thread
 * (`AndroidPsnRemoteClient.controller()`), where Android throws NetworkOnMainThreadException for a DNS
 * lookup (PLE-312). Any address given here must be unresolved or literal.
 *
 * One instance serves one PSN session. Like upstream `holepunch.c`, the session keeps one sid and one
 * hashed id for both the control and the data round, and an OFFER names the sid of the console OFFER it
 * is answering. PLE-313 staged that value by one round, so the control OFFER went out with `peerSid=0`;
 * upstream stores the console sid as soon as its OFFER arrives (`holepunch.c:1561`) and sends it back in
 * the same round (`:2760`). The console ignores an OFFER not addressed to its session (PLE-327).
 *
 * The OFFER lists STUN, STATIC, LOCAL, or just STATIC, LOCAL when the STUN-mapped port equals the local
 * one (`:2912-2918`, `:2977`). Upstream's symmetric-NAT port guessing (`stun_port_allocation_test`, the
 * `stun_allocation_increment != 0` branch at `:2828`) is not ported: a NAT that rewrites ports gets the
 * plain three candidates and, if none answers, `PsnUnsupportedNatException`.
 */
class DatagramPsnHolePuncher(
	private val random: SecureRandom = SecureRandom(),
	internal val stunServers: List<InetSocketAddress> = listOf(
		InetSocketAddress.createUnresolved("stun.moonlight-stream.org", 3478),
		InetSocketAddress.createUnresolved("stun.l.google.com", 19302),
		InetSocketAddress.createUnresolved("stun1.l.google.com", 19302),
		InetSocketAddress.createUnresolved("stun2.l.google.com", 19302)
	)
) : PsnHolePuncher
{
	private val localSid by lazy { random.nextInt(0x10000) }
	private val localHash by lazy { ByteArray(20).also(random::nextBytes) }

	private class Discovery(val socket: DatagramSocket, val mapping: InetSocketAddress)

	/**
	 * Upstream builds its OFFER (STUN included) before the console's OFFER is awaited; preparing only after
	 * it arrived cost up to 5 s per STUN server and made our OFFER late (PLE-313's follow-up). The Result
	 * wrapper keeps a STUN failure from cancelling the caller's scope before [prepare] rethrows it.
	 */
	@Volatile private var ahead: Deferred<Result<Discovery>>? = null

	override fun discoverAhead()
	{
		discardAhead()
		ahead = CoroutineScope(Dispatchers.IO).async { runCatching { discover() } }
	}

	override fun discardAhead()
	{
		val pending = ahead ?: return
		ahead = null
		pending.invokeOnCompletion { if(it == null) runCatching { pending.getCompletedResult()?.socket?.close() } }
		pending.cancel()
	}

	@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
	private fun Deferred<Result<Discovery>>.getCompletedResult(): Discovery? = getCompleted().getOrNull()

	private suspend fun discover(): Discovery = withContext(Dispatchers.IO) {
		val socket = DatagramSocket(null).apply {
			reuseAddress = true
			bind(InetSocketAddress(InetAddress.getByName("0.0.0.0"), 0))
		}
		try
		{
			val mapping = discoverMapping(socket)
			ensureActive()
			Discovery(socket, mapping)
		}
		catch(error: Throwable)
		{
			socket.close()
			throw error
		}
	}

	override suspend fun prepare(peer: PsnConnectionRequest, accountId: String): PsnPunchPreparation = withContext(Dispatchers.IO) {
		// Cleared only once awaited, so a cancelled prepare leaves it for discardAhead to close.
		val discovery = ahead?.let { pending -> pending.await().also { ahead = null }.getOrThrow() } ?: discover()
		val socket = discovery.socket
		try
		{
			val mapping = discovery.mapping
			val localAddress = socket.localAddress.takeUnless { it.isAnyLocalAddress } as? Inet4Address
				?: routeAddress(resolve(stunServers.first()))
			val stun = PsnCandidate("STUN", mapping.address.hostAddress ?: "0.0.0.0", port = mapping.port)
			val static = PsnCandidate("STATIC", stun.addr, port = socket.localPort)
			val local = PsnCandidate("LOCAL", localAddress.hostAddress ?: "0.0.0.0", port = socket.localPort)
			// Upstream offers STUN, STATIC, LOCAL (holepunch.c:2977, STUN first so a console behind a
			// symmetric NAT tries it first) unless the NAT kept our local port as the external one: then
			// STUN would duplicate STATIC, so upstream drops it and offers STATIC, LOCAL (:2912-2918, "don't
			// make duplicate STUN candidate"). That is the shape the console itself offers on such a network
			// (its STATIC and LOCAL both :9303 in the PLE-327 captures), and it is the shape our S25 captures
			// broke: local port 53637, STUN-mapped port 53637, three candidates with a duplicate.
			val natKeptOurPort = stun.port == socket.localPort
			val remote = if(natKeptOurPort) static else stun
			val candidates = if(natKeptOurPort) listOf(static, local) else listOf(stun, static, local)
			val offer = PsnConnectionRequest(
				sid = localSid,
				// Upstream stores the console's sid the moment its OFFER arrives and sends it straight
				// back (holepunch.c:1561 then :2760), per round. Staging it for the *next* round left the
				// control OFFER with peerSid=0, which the console ignores: it re-offers, and PLE-327 timed
				// out after 30 s waiting for a RESULT that was never coming.
				peerSid = peer.sid,
				skey = Base64.Default.encode(ByteArray(16)),
				candidate = candidates,
				localPeerAddr = PsnPeerAddress(accountId, "REMOTE_PLAY"),
				localHashedId = Base64.Default.encode(localHash)
			)
			Preparation(socket, offer, peer, localHash, random, local, remote)
		}
		catch(error: Throwable)
		{
			socket.close()
			throw error
		}
	}

	private fun discoverMapping(socket: DatagramSocket): InetSocketAddress
	{
		var lastError: Throwable? = null
		for(server in stunServers)
		{
			try
			{
				val transactionId = ByteArray(12).also(random::nextBytes)
				val request = PsnStunCodec.request(transactionId)
				socket.send(DatagramPacket(request, request.size, resolve(server)))
				socket.soTimeout = 5_000
				val packet = DatagramPacket(ByteArray(1024), 1024)
				socket.receive(packet)
				return PsnStunCodec.mappedAddress(packet.data.copyOf(packet.length), transactionId)
			}
			catch(error: Throwable) { lastError = error }
		}
		throw PsnUnsupportedNatException("No configured STUN server returned an IPv4 mapping: ${lastError?.message}")
	}

	/** DNS happens here, inside [prepare] on IO, never where the puncher is constructed. */
	private fun resolve(server: InetSocketAddress): InetSocketAddress =
		if(server.isUnresolved) InetSocketAddress(InetAddress.getByName(server.hostString), server.port) else server

	private fun routeAddress(server: InetSocketAddress): Inet4Address
	{
		DatagramSocket().use { probe ->
			probe.connect(server)
			return probe.localAddress as? Inet4Address
				?: throw PsnUnsupportedNatException("No IPv4 route to the STUN server")
		}
	}

	internal class Preparation(
		private val socket: DatagramSocket,
		override val offer: PsnConnectionRequest,
		private val peer: PsnConnectionRequest,
		private val localHash: ByteArray,
		private val random: SecureRandom,
		private val localCandidate: PsnCandidate,
		private val remoteCandidate: PsnCandidate
	) : PsnPunchPreparation
	{
		private var transferred = false
		private val buffer = DatagramPacket(ByteArray(1500), 1500)

		private val peerHash: ByteArray by lazy {
			val hash = runCatching { Base64.Default.decode(peer.localHashedId) }
				.getOrElse { throw PsnRemoteProtocolException("Invalid console hashed ID", it) }
			if(hash.size != 20) throw PsnRemoteProtocolException("Console hashed ID is not 20 bytes")
			hash
		}

		override suspend fun punch(): PsnPunchedSocket = withContext(Dispatchers.IO) {
			val known = peer.candidate.mapNotNull { candidate ->
				runCatching { candidate to InetSocketAddress(InetAddress.getByName(candidate.addr), candidate.port) }.getOrNull()
			}
			if(known.isEmpty()) throw PsnUnsupportedNatException("Console did not advertise a usable IPv4 candidate")
			val destinations = known.toMutableList()
			val requestId = ByteArray(5).also(random::nextBytes)
			val request = PsnCandidateHandshake.request(localHash, peerHash, offer.sid, peer.sid, requestId)
			var responded = false
			repeat(SELECT_CANDIDATE_TRIES) {
				for((_, address) in destinations) socket.send(DatagramPacket(request, request.size, address))
				val deadline = System.nanoTime() + SELECT_CANDIDATE_TIMEOUT_MS * 1_000_000L
				while(true)
				{
					val bytes = receive(((deadline - System.nanoTime()) / 1_000_000L).toInt()) ?: break
					val source = buffer.socketAddress as InetSocketAddress
					var candidate = destinations.firstOrNull { it.second == source }?.first
					if(candidate == null)
					{
						// A reply from an address the console did not advertise: its NAT mapped it elsewhere.
						if(destinations.size - known.size >= EXTRA_CANDIDATE_ADDRESSES) continue
						candidate = PsnCandidate("DERIVED", source.address.hostAddress ?: continue, port = source.port)
						destinations += candidate to source
						if(PsnCandidateHandshake.isRequest(bytes))
							socket.send(DatagramPacket(request, request.size, source))
					}
					if(PsnCandidateHandshake.isRequest(bytes))
					{
						respond(bytes, source)
						responded = true
						continue
					}
					if(!PsnCandidateHandshake.isResponse(bytes, requestId)) continue
					socket.connect(source)
					// The console checks our address too; it must be answered before ACCEPT or it gives up.
					val followedUp = answerFollowUps()
					if(!followedUp && !responded)
						throw PsnUnsupportedNatException("The console answered but never reached this device over UDP")
					transferred = true
					return@withContext PsnPunchedSocket(socket, withMapping(candidate, source))
				}
			}
			throw PsnUnsupportedNatException("No console candidate completed the UDP handshake")
		}

		override suspend fun settle() = withContext(Dispatchers.IO) {
			if(transferred && !socket.isClosed) answerFollowUps()
			Unit
		}

		/** Upstream `receive_request_send_response_ps`: answer requests until the console is quiet for a second. */
		private fun answerFollowUps(): Boolean
		{
			var received = false
			val deadline = System.nanoTime() + FOLLOW_UP_LIMIT_MS * 1_000_000L
			while(System.nanoTime() < deadline)
			{
				val bytes = receive(FOLLOW_UP_QUIET_MS) ?: return received
				if(!PsnCandidateHandshake.isRequest(bytes)) continue
				respond(bytes, buffer.socketAddress as InetSocketAddress)
				received = true
			}
			return received
		}

		private fun respond(request: ByteArray, source: InetSocketAddress)
		{
			val response = PsnCandidateHandshake.response(request, localHash, peerHash, offer.sid, peer.sid, source)
			socket.send(DatagramPacket(response, response.size, source))
		}

		private fun receive(timeoutMillis: Int): ByteArray?
		{
			if(timeoutMillis <= 0) return null
			socket.soTimeout = timeoutMillis
			return try
			{
				buffer.length = buffer.data.size
				socket.receive(buffer)
				buffer.data.copyOf(buffer.length)
			}
			catch(_: SocketTimeoutException) { null }
		}

		/** The console reached our LOCAL candidate over the LAN, or our STUN one from outside it. */
		private fun withMapping(candidate: PsnCandidate, source: InetSocketAddress): PsnCandidate
		{
			val lan = candidate.type == "LOCAL" || (candidate.type == "DERIVED" && source.address.isSiteLocalAddress)
			val ours = if(lan) localCandidate else remoteCandidate
			return candidate.copy(mappedAddress = ours.addr, mappedPort = ours.port)
		}

		override fun close()
		{
			if(!transferred) socket.close()
		}
	}

	private companion object
	{
		// holepunch.c: SELECT_CANDIDATE_TRIES, SELECT_CANDIDATE_TIMEOUT_SEC, EXTRA_CANDIDATE_ADDRESSES,
		// WAIT_RESPONSE_TIMEOUT_SEC and SELECT_CANDIDATE_CONNECTION_SEC.
		const val SELECT_CANDIDATE_TRIES = 20
		const val SELECT_CANDIDATE_TIMEOUT_MS = 500
		const val EXTRA_CANDIDATE_ADDRESSES = 3
		const val FOLLOW_UP_QUIET_MS = 1_000
		const val FOLLOW_UP_LIMIT_MS = 5_000
	}
}
