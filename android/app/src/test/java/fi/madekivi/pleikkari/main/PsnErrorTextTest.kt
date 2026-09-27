package fi.madekivi.pleikkari.main

import org.junit.Assert.assertEquals
import org.junit.Test

class PsnErrorTextTest
{
	@Test
	fun messageIsShownAsIs()
	{
		assertEquals("boom", psnErrorText(IllegalStateException("boom"), "Fallback"))
	}

	@Test
	fun messageLessErrorNamesItsType()
	{
		assertEquals("Fallback (NullPointerException)", psnErrorText(NullPointerException(), "Fallback"))
		assertEquals("Fallback (IllegalStateException)", psnErrorText(IllegalStateException(" "), "Fallback"))
	}
}
