// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.fail
import org.junit.Test

class FrameGrabFileTest
{
	@Test
	fun defaultAndPlainNames()
	{
		assertEquals("latest.png", FrameGrabFile.sanitizeName(null))
		assertEquals("latest.png", FrameGrabFile.sanitizeName(""))
		assertEquals("menu_1-a.png", FrameGrabFile.sanitizeName("menu_1-a.png"))
	}

	@Test
	fun rejectsPathsAndOtherExtensions()
	{
		for(bad in listOf("../x.png", "a/b.png", ".hidden.png", "x.jpg", "x", "a b.png", "x.png/../y"))
			assertNull(bad, FrameGrabFile.sanitizeName(bad))
	}

	@Test
	fun writesAtomicallyAndOverwrites()
	{
		val dir = File(Files.createTempDirectory("grab").toFile(), "sub")
		val file = FrameGrabFile.writeAtomic(dir, "latest.png") { it.write(byteArrayOf(1, 2, 3)) }
		assertArrayEquals(byteArrayOf(1, 2, 3), file.readBytes())
		FrameGrabFile.writeAtomic(dir, "latest.png") { it.write(byteArrayOf(9)) }
		assertArrayEquals(byteArrayOf(9), file.readBytes())
		assertEquals(listOf("latest.png"), dir.list()!!.toList())
	}

	@Test
	fun failedWriteLeavesOldFileAndNoTemp()
	{
		val dir = Files.createTempDirectory("grab").toFile()
		FrameGrabFile.writeAtomic(dir, "latest.png") { it.write(byteArrayOf(1)) }
		try
		{
			FrameGrabFile.writeAtomic(dir, "latest.png") { it.write(byteArrayOf(2)); throw IllegalStateException("boom") }
			fail()
		}
		catch(e: IllegalStateException) { }
		assertArrayEquals(byteArrayOf(1), File(dir, "latest.png").readBytes())
		assertFalse(File(dir, "latest.png.tmp").exists())
	}
}
