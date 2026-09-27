// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
@file:OptIn(kotlin.io.encoding.ExperimentalEncodingApi::class)

package fi.madekivi.pleikkari.remote

import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread
import kotlin.io.encoding.Base64

class PsnUdpPuncherTest
{
	/**
	 * PLE-312: `InetSocketAddress(host, port)` resolves in the constructor. The puncher is built on
	 * the main thread, where that lookup threw NetworkOnMainThreadException and killed every Play.
	 */
	@Test fun defaultStunServersAreNotResolvedAtConstruction()
	{
		val puncher = DatagramPsnHolePuncher()
		assertEquals(4, puncher.stunServers.size)
		assertTrue("resolved at construction: ${puncher.stunServers}", puncher.stunServers.all { it.isUnresolved })
	}

	@Test fun prepareResolvesTheStunServerOffTheCallerThread() = runBlocking {
		val stun = DatagramSocket(0, InetAddress.getLoopbackAddress())
		val threads = recordedThreads()
		val responder = thread(name = "fake-stun", isDaemon = true) {
			val packet = DatagramPacket(ByteArray(64), 64)
			runCatching {
				stun.receive(packet)
				val request = packet.data.copyOf(packet.length)
				val transactionId = request.copyOfRange(8, 20)
				val source = packet.socketAddress as InetSocketAddress
				stun.send(DatagramPacket(bindingResponse(transactionId), 32, source))
			}
		}
		val puncher = DatagramPsnHolePuncher(
			stunServers = listOf(InetSocketAddress.createUnresolved("localhost", stun.localPort))
		)
		val peer = PsnConnectionRequest(
			sid = 7, peerSid = 0, skey = Base64.Default.encode(ByteArray(16)),
			candidate = listOf(PsnCandidate("STUN", "192.0.2.10", port = 41000)),
			localHashedId = Base64.Default.encode(ByteArray(20))
		)
		val executor = callerExecutor()
		try
		{
			val preparation = runBlocking(executor.asCoroutineDispatcher()) {
				val prepared = puncher.prepare(peer, "12345678901234567")
				recordThread(threads)
				prepared
			}
			preparation.use {
				val mapped = it.offer.candidate.first { candidate -> candidate.type == "STUN" }
				assertEquals("127.0.0.1", mapped.addr)
				assertEquals(45678, mapped.port)
			}
		}
		finally
		{
			executor.shutdownNow()
			stun.close()
			responder.join(2_000)
		}
		// The caller thread only ran the coroutine's own continuation, never the STUN exchange.
		assertEquals(listOf(threads.first()), threads.onCallerThread())
	}

	/**
	 * PLE-327: upstream keeps one sid and hashed id per PSN session, lists STUN, STATIC, LOCAL when the NAT
	 * rewrote our port (`holepunch.c:2977`), and names in each OFFER the sid of the console OFFER that round
	 * is answering (`:1561` then `:2760`). PLE-313 staged that value by a round, which sent the control OFFER
	 * with `peerSid=0`; the console then ignores it, re-offers, and the session times out after 30 s (seen on
	 * the S25, PLE-327).
	 */
	@Test fun offersShareTheSessionIdentityAndNameTheAnsweredConsoleSid() = runBlocking {
		val stun = fakeStun(answers = 2)
		try
		{
			val puncher = DatagramPsnHolePuncher(stunServers = listOf(InetSocketAddress("127.0.0.1", stun.first.localPort)))
			val control = puncher.prepare(consoleOffer(sid = 4567, port = 1), "12345678901234567").use { it.offer }
			val data = puncher.prepare(consoleOffer(sid = 5678, port = 1), "12345678901234567").use { it.offer }
			assertEquals(4567, control.peerSid)
			assertEquals(5678, data.peerSid)
			assertEquals(control.sid, data.sid)
			assertEquals(control.localHashedId, data.localHashedId)
			assertEquals(listOf("STUN", "STATIC", "LOCAL"), control.candidate.map { it.type })
			assertEquals(45678, control.candidate.first { it.type == "STUN" }.port)
			assertTrue(control.candidate.first { it.type == "STATIC" }.port != 45678)
			assertEquals(2, control.natType)
		}
		finally { stun.first.close(); stun.second.join(2_000) }
	}

	/**
	 * PLE-319: discovery started ahead of the console OFFER is what the next prepare offers, so our OFFER
	 * no longer waits on STUN. The fake answers once: a second STUN round would find no answer.
	 */
	@Test fun prepareUsesTheDiscoveryStartedAhead() = runBlocking {
		val stun = fakeStun(answers = 1)
		try
		{
			val puncher = DatagramPsnHolePuncher(stunServers = listOf(InetSocketAddress("127.0.0.1", stun.first.localPort)))
			puncher.discoverAhead()
			val offer = puncher.prepare(consoleOffer(sid = 4567, port = 1), "12345678901234567").use { it.offer }
			assertEquals(45678, offer.candidate.first { it.type == "STUN" }.port)
			puncher.discoverAhead()
			puncher.discardAhead()
		}
		finally { stun.first.close(); stun.second.join(2_000) }
	}

	/**
	 * PLE-327: when the NAT keeps our local port as the external one, upstream drops the STUN candidate
	 * because it would duplicate STATIC (`holepunch.c:2912-2918`) and offers STATIC, LOCAL, the same shape the
	 * console offers on such a network. Both S25 captures sent the duplicate (STUN :53637, STATIC :53637).
	 */
	@Test fun dropsTheStunCandidateWhenTheNatKeptOurPort() = runBlocking {
		val stun = fakeStun(answers = 1, keepPort = true)
		try
		{
			val puncher = DatagramPsnHolePuncher(stunServers = listOf(InetSocketAddress("127.0.0.1", stun.first.localPort)))
			val offer = puncher.prepare(consoleOffer(sid = 4567, port = 1), "12345678901234567").use { it.offer }
			assertEquals(listOf("STATIC", "LOCAL"), offer.candidate.map { it.type })
			val static = offer.candidate.first()
			val local = offer.candidate.last()
			assertEquals("127.0.0.1", static.addr)
			assertEquals(local.port, static.port)
			assertEquals(listOf("0.0.0.0", 0), listOf(static.mappedAddress, static.mappedPort))
		}
		finally { stun.first.close(); stun.second.join(2_000) }
	}

	/**
	 * PLE-313: the console checks our address as well as answering ours, and gives up on the exchange
	 * if those checks go unanswered. The punch must answer the console's follow-up request after its
	 * own check succeeded, and report the console candidate with the candidate of ours it reached.
	 */
	@Test fun punchAnswersTheConsolesFollowUpChecksAndMapsTheCandidate() = runBlocking {
		val stun = fakeStun(answers = 1)
		val console = DatagramSocket(0, InetAddress.getByName("127.0.0.1"))
		val consoleHash = ByteArray(20) { 9 }
		val responsesToConsole = java.util.concurrent.atomic.AtomicInteger()
		val consoleThread = thread(name = "fake-console", isDaemon = true) {
			runCatching {
				console.soTimeout = 5_000
				val packet = DatagramPacket(ByteArray(1500), 1500)
				console.receive(packet)
				val request = packet.data.copyOf(packet.length)
				val phone = packet.socketAddress
				val consoleRequest = { id: Byte -> PsnCandidateHandshake.request(consoleHash, request.copyOfRange(0x04, 0x18), 4567, 0, ByteArray(5) { id }) }
				console.send(DatagramPacket(consoleRequest(1), 88, phone))
				val response = PsnCandidateHandshake.response(request, consoleHash, request.copyOfRange(0x04, 0x18), 4567, 0)
				console.send(DatagramPacket(response, 88, phone))
				Thread.sleep(200)
				console.send(DatagramPacket(consoleRequest(2), 88, phone))
				while(true)
				{
					console.receive(packet)
					if(PsnCandidateHandshake.isResponse(packet.data.copyOf(packet.length))) responsesToConsole.incrementAndGet()
				}
			}
		}
		try
		{
			val puncher = DatagramPsnHolePuncher(stunServers = listOf(InetSocketAddress("127.0.0.1", stun.first.localPort)))
			puncher.prepare(consoleOffer(sid = 4567, port = console.localPort, hash = consoleHash), "12345678901234567").use { preparation ->
				val ourLocal = preparation.offer.candidate.first { it.type == "LOCAL" }
				val punched = preparation.punch()
				try
				{
					assertEquals(PsnCandidate("LOCAL", "127.0.0.1", ourLocal.addr, console.localPort, ourLocal.port), punched.candidate)
					assertEquals(2, responsesToConsole.get())
				}
				finally { punched.socket.close() }
			}
		}
		finally
		{
			console.close()
			stun.first.close()
			consoleThread.join(2_000)
			stun.second.join(2_000)
		}
	}

	private fun consoleOffer(sid: Int, port: Int, hash: ByteArray = ByteArray(20)) = PsnConnectionRequest(
		sid = sid, peerSid = 0, skey = Base64.Default.encode(ByteArray(16)),
		candidate = listOf(PsnCandidate("LOCAL", "127.0.0.1", port = port)),
		localHashedId = Base64.Default.encode(hash)
	)

	/** A STUN server mapping every request to 127.0.0.1:45678, or to the port it came from with [keepPort]. */
	private fun fakeStun(answers: Int, keepPort: Boolean = false): Pair<DatagramSocket, Thread>
	{
		val stun = DatagramSocket(0, InetAddress.getByName("127.0.0.1"))
		return stun to thread(name = "fake-stun", isDaemon = true) {
			val packet = DatagramPacket(ByteArray(64), 64)
			runCatching {
				repeat(answers) {
					stun.receive(packet)
					val transactionId = packet.data.copyOfRange(8, 20)
					val source = packet.socketAddress as InetSocketAddress
					val mappedPort = if(keepPort) source.port else 45678
					stun.send(DatagramPacket(bindingResponse(transactionId, mappedPort), 32, source))
				}
			}
		}
	}

	/** A STUN binding success with one XOR-MAPPED-ADDRESS of 127.0.0.1:[mappedPort]. */
	private fun bindingResponse(transactionId: ByteArray, mappedPort: Int = 45678): ByteArray =
		ByteBuffer.allocate(32).order(ByteOrder.BIG_ENDIAN).apply {
			putShort(0x0101)
			putShort(12)
			putInt(0x2112a442)
			put(transactionId)
			putShort(0x0020)
			putShort(8)
			put(0)
			put(1)
			putShort((mappedPort xor 0x2112).toShort())
			put(byteArrayOf((127 xor 0x21).toByte(), 0x12, 0xa4.toByte(), (1 xor 0x42).toByte()))
		}.array()
}
