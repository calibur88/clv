#include "core/varint.h"

#include "gtest/gtest.h"
#include <cstdint>

namespace
{

	constexpr size_t kBufSize = 10;

}	 // namespace

TEST(CoreVarintSigned, ZigZagRoundTripsBoundaries)
{
	const int64_t values[] = { 0,		 1,			-1,		   63,		  -64,			 64,		   -65,
							   45090045, -45090045, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1 };

	for (const int64_t value: values)
	{
		uint8_t buf[kBufSize] = {};
		const size_t written = clv::core::EncodeZigZag(value, buf);
		ASSERT_GE(written, 1u) << "值 " << value;
		ASSERT_LE(written, kBufSize) << "值 " << value;

		int64_t out = 0;
		const size_t consumed = clv::core::DecodeZigZag(buf, written, &out);
		EXPECT_EQ(consumed, written) << "值 " << value;
		EXPECT_EQ(out, value) << "值 " << value;
	}
}

TEST(CoreVarintSigned, ZigZagWireBytesMatchSpecMapping)
{
	// zigzag = (n << 1) ^ (n >> 63)，线上是该无符号值的 LEB128
	uint8_t buf[kBufSize] = {};

	EXPECT_EQ(clv::core::EncodeZigZag(0, buf), 1u);
	EXPECT_EQ(buf[0], 0x00);

	EXPECT_EQ(clv::core::EncodeZigZag(-1, buf), 1u);
	EXPECT_EQ(buf[0], 0x01);

	EXPECT_EQ(clv::core::EncodeZigZag(1, buf), 1u);
	EXPECT_EQ(buf[0], 0x02);

	EXPECT_EQ(clv::core::EncodeZigZag(INT64_MIN, buf), 10u);
	EXPECT_EQ(buf[9], 0x01u);
}

TEST(CoreVarintSigned, ShortestFormAcceptsMinimalEncodings)
{
	uint8_t buf[kBufSize] = {};

	const size_t one = clv::core::EncodeVarint(0, buf);
	EXPECT_TRUE(clv::core::IsShortestVarint(buf, one));

	const size_t two = clv::core::EncodeVarint(128, buf);
	EXPECT_EQ(two, 2u);
	EXPECT_TRUE(clv::core::IsShortestVarint(buf, two));

	const size_t max64 = clv::core::EncodeVarint(UINT64_MAX, buf);
	EXPECT_EQ(max64, 10u);
	EXPECT_TRUE(clv::core::IsShortestVarint(buf, max64));
}

TEST(CoreVarintSigned, ShortestFormRejectsPaddedEncodings)
{
	// 0 写成两字节：解码能读回 0，但不是最短形式
	const uint8_t padded_zero[] = { 0x80, 0x00 };
	EXPECT_FALSE(clv::core::IsShortestVarint(padded_zero, sizeof(padded_zero)));

	// 128 写成三字节
	const uint8_t padded_128[] = { 0x80, 0x81, 0x00 };
	EXPECT_FALSE(clv::core::IsShortestVarint(padded_128, sizeof(padded_128)));

	const uint8_t* nulls[] = { nullptr };
	EXPECT_FALSE(clv::core::IsShortestVarint(nulls[0], 2));
	EXPECT_FALSE(clv::core::IsShortestVarint(padded_zero, 0));

	const uint8_t no_terminator[] = { 0x80, 0x80 };
	EXPECT_FALSE(clv::core::IsShortestVarint(no_terminator, 1));
}
