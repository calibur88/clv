#include "core/crc32.h"

#include "gtest/gtest.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

	constexpr uint32_t kPoly = 0x04C11DB7u;
	constexpr uint32_t kInit = 0xFFFFFFFFu;

	// 独立逐位实现：与被测的查表实现不共享代码路径，用来交叉验证
	uint32_t ReferenceCrc(const uint8_t* data, size_t size, uint32_t crc = kInit)
	{
		for (size_t i = 0; i < size; ++i)
		{
			crc ^= static_cast<uint32_t>(data[i]) << 24;
			for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80000000u) != 0u ? (crc << 1) ^ kPoly : crc << 1;
		}
		return crc;
	}

	std::vector<uint8_t> PseudoRandom(size_t count)
	{
		std::vector<uint8_t> out(count);
		uint32_t state = 0x12345678u;
		for (size_t i = 0; i < count; ++i)
		{
			state = state * 1664525u + 1013904223u;
			out[i] = static_cast<uint8_t>(state >> 24);
		}
		return out;
	}

}	 // namespace

TEST(CoreCrc32, Mpeg2CheckVector)
{
	const char text[] = "123456789";
	EXPECT_EQ(clv::core::Crc32(text, sizeof(text) - 1), 0x0376E6E7u);
}

TEST(CoreCrc32, MatchesIndependentBitwiseReference)
{
	const std::vector<uint8_t> data = PseudoRandom(4096);
	EXPECT_EQ(clv::core::Crc32(data.data(), data.size()), ReferenceCrc(data.data(), data.size()));
}

TEST(CoreCrc32, KnownSingleByteVectors)
{
	const uint8_t zero = 0x00;
	const uint8_t letter_a = 'A';
	EXPECT_EQ(clv::core::Crc32(&zero, 1), 0x4E08BFB4u);
	EXPECT_EQ(clv::core::Crc32(&letter_a, 1), 0x7E4FD274u);
}

TEST(CoreCrc32, EmptyInputReturnsSeedItself)
{
	EXPECT_EQ(clv::core::Crc32(nullptr, 0), kInit);
	EXPECT_EQ(clv::core::Crc32("abc", 0, 0x12345678u), 0x12345678u);
}

TEST(CoreCrc32, SegmentedUpdateEqualsOneShot)
{
	const std::vector<uint8_t> data = PseudoRandom(1000);
	const uint32_t one_shot = clv::core::Crc32(data.data(), data.size());

	uint32_t chained = kInit;
	size_t offset = 0;
	while (offset < data.size())
	{
		const size_t step = offset + 377 < data.size() ? 377 : data.size() - offset;
		chained = clv::core::Crc32(data.data() + offset, step, chained);
		offset += step;
	}
	EXPECT_EQ(chained, one_shot);
}

TEST(CoreCrc32, ResidueIsZeroWhenCrcAppendedBigEndian)
{
	const char text[] = "123456789";
	const uint32_t crc = clv::core::Crc32(text, sizeof(text) - 1);

	std::vector<uint8_t> framed(text, text + sizeof(text) - 1);
	framed.push_back(static_cast<uint8_t>(crc >> 24));
	framed.push_back(static_cast<uint8_t>(crc >> 16));
	framed.push_back(static_cast<uint8_t>(crc >> 8));
	framed.push_back(static_cast<uint8_t>(crc));

	EXPECT_EQ(clv::core::Crc32(framed.data(), framed.size()), 0u) << "residue 恒为 0 是这条参数集的性质";
}

TEST(CoreCrc32, ReflectedParameterSetIsRejected)
{
	// 钉住被否决的参数集：反射 + xorout=0xFFFFFFFF（CRC-32/ISO-HDLC）的 check 值是 0xCBF43926。
	// 若实现被改回反射表，本条会失败。
	const char text[] = "123456789";
	EXPECT_NE(clv::core::Crc32(text, sizeof(text) - 1), 0xCBF43926u);
}
