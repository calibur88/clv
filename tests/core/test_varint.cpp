#include "core/varint.h"

#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

	constexpr size_t kBufSize = 10;

}	 // namespace

TEST(CoreVarint, RoundTripsBoundaries)
{
	const uint64_t values[] = { 0ull, 1ull, 127ull, 128ull, 16383ull, 16384ull, 4294967296ull, UINT64_MAX };

	for (const uint64_t value: values)
	{
		uint8_t buf[kBufSize] = {};
		const size_t written = clv::core::EncodeVarint(value, buf);
		ASSERT_GE(written, 1u);
		ASSERT_LE(written, kBufSize);

		uint64_t out = 0;
		const size_t consumed = clv::core::DecodeVarint(buf, written, &out);
		EXPECT_EQ(consumed, written) << "值 " << value;
		EXPECT_EQ(out, value) << "值 " << value;
	}
}

TEST(CoreVarint, EncodedLengths)
{
	uint8_t buf[kBufSize] = {};
	EXPECT_EQ(clv::core::EncodeVarint(0, buf), 1u);
	EXPECT_EQ(clv::core::EncodeVarint(127, buf), 1u);
	EXPECT_EQ(clv::core::EncodeVarint(128, buf), 2u);
	EXPECT_EQ(clv::core::EncodeVarint(16383, buf), 2u);
	EXPECT_EQ(clv::core::EncodeVarint(16384, buf), 3u);
	EXPECT_EQ(clv::core::EncodeVarint(UINT64_MAX, buf), 10u);
}

TEST(CoreVarint, MaxValueByteLayout)
{
	uint8_t buf[kBufSize] = {};
	ASSERT_EQ(clv::core::EncodeVarint(UINT64_MAX, buf), 10u);

	const uint8_t expected[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
	for (size_t i = 0; i < 10; ++i) EXPECT_EQ(buf[i], expected[i]) << "第 " << i << " 字节";
}

TEST(CoreVarint, OverlongTenthByteIsRejectedNotTruncated)
{
	// 前 9 字节全 1，第 10 字节给 0x02：值超出 uint64 量程。静默折返会得到 0x7FFFFFFFFFFFFFFF，
	// 那是把坏数据当好数据，必须拒绝。
	const std::vector<uint8_t> overlong(9, 0xFF);
	std::vector<uint8_t> buf = overlong;
	buf.push_back(0x02);

	uint64_t out = 0xA5A5A5A5u;
	EXPECT_EQ(clv::core::DecodeVarint(buf.data(), buf.size(), &out), 0u);
	EXPECT_EQ(out, 0xA5A5A5A5u) << "失败时不许写出参";
}

TEST(CoreVarint, TenthByteWithContinuationBitIsRejected)
{
	std::vector<uint8_t> buf(10, 0x80);
	uint64_t out = 0;
	EXPECT_EQ(clv::core::DecodeVarint(buf.data(), buf.size(), &out), 0u);
}

TEST(CoreVarint, TruncatedInputIsRejected)
{
	uint8_t full[kBufSize] = {};
	ASSERT_EQ(clv::core::EncodeVarint(UINT64_MAX, full), 10u);

	uint64_t out = 0;
	EXPECT_EQ(clv::core::DecodeVarint(full, 9, &out), 0u) << "少了终止字节";
}

TEST(CoreVarint, LargestEncodingsDecodeToExpectedValues)
{
	const std::vector<uint8_t> max10 = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
	uint64_t out = 0;
	EXPECT_EQ(clv::core::DecodeVarint(max10.data(), max10.size(), &out), 10u);
	EXPECT_EQ(out, UINT64_MAX);

	std::vector<uint8_t> half(9, 0x80);
	half.push_back(0x01);
	EXPECT_EQ(clv::core::DecodeVarint(half.data(), half.size(), &out), 10u);
	EXPECT_EQ(out, 1ull << 63);
}

TEST(CoreVarint, NullPointersAreRejected)
{
	uint8_t buf[kBufSize] = {};
	EXPECT_EQ(clv::core::EncodeVarint(12345, nullptr), 0u);

	uint64_t out = 0;
	EXPECT_EQ(clv::core::DecodeVarint(nullptr, 5, &out), 0u);
	EXPECT_EQ(clv::core::DecodeVarint(buf, 5, nullptr), 0u);
}

TEST(CoreVarint, ZeroLengthInputIsRejected)
{
	uint8_t buf[kBufSize] = {};
	uint64_t out = 0;
	EXPECT_EQ(clv::core::DecodeVarint(buf, 0, &out), 0u);
}
