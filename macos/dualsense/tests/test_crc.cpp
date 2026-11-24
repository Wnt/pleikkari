// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// CRC32 and the Bluetooth seeds. Expected values from Python's zlib.crc32 (see
// gen_vectors notes in test_output.cpp), not from this implementation.

#include "Protocol.h"
#include "test.h"

#include <cstring>

using namespace plk::dualsense::protocol;

TEST(crc32_check_value)
{
	const char *s = "123456789";
	CHECK_EQ(Crc32((const uint8_t *)s, strlen(s)), 0xCBF43926u);
	CHECK_EQ(Crc32(nullptr, 0), 0u);
}

TEST(crc32_continuation_equals_concatenation)
{
	const char *s = "123456789";
	uint32_t part = Crc32((const uint8_t *)s, 4);
	CHECK_EQ(Crc32((const uint8_t *)s + 4, 5, part), 0xCBF43926u);
}

TEST(crc32_bluetooth_seeds)
{
	uint8_t d[32];
	for(int i = 0; i < 32; i++)
		d[i] = (uint8_t)i;
	// zlib.crc32(bytes([seed]) + bytes(range(32)))
	CHECK_EQ(BtCrc32(kCrcSeedInput, d, sizeof(d)), 0xD4A882A9u);
	CHECK_EQ(BtCrc32(kCrcSeedOutput, d, sizeof(d)), 0x8FBF33BCu);
	CHECK_EQ(BtCrc32(kCrcSeedFeature, d, sizeof(d)), 0xB94DA34Fu);
}

TEST(crc32_store_and_validate)
{
	uint8_t r[78] = { 0x31, 0x40, 0x10, 0x01, 0x02 };
	BtCrcStore(kCrcSeedOutput, r, sizeof(r));
	CHECK(BtCrcValid(kCrcSeedOutput, r, sizeof(r)));
	CHECK(!BtCrcValid(kCrcSeedInput, r, sizeof(r)));
	r[10] ^= 0x01;
	CHECK(!BtCrcValid(kCrcSeedOutput, r, sizeof(r)));
	CHECK(!BtCrcValid(kCrcSeedOutput, r, 4));
}
