/* test_subtitle_packet.cpp —— 字幕包外层的判序与平移。

本件吃实现层头（tests 拿 ${CMAKE_SOURCE_DIR}/src 的包含路径），不碰公开句柄：
包外层的判序是内部契约，公开面只经由解析器体现。 */

#include "subtitle/packet.h"

#include "gtest/gtest.h"
#include <cstring>
#include <vector>

using clv::subtitle::EncodePacket;
using clv::subtitle::Err;
using clv::subtitle::kInvalidArgument;
using clv::subtitle::kMaxPacketSize;
using clv::subtitle::kOk;
using clv::subtitle::kOutputTooSmall;
using clv::subtitle::kPacketHeaderBad;
using clv::subtitle::kPacketTooLarge;
using clv::subtitle::PacketHeader;
using clv::subtitle::ParsePacketHeader;

namespace
{
	std::vector<uint8_t> MakePacket(int64_t start, int64_t end, const char* text, bool with_pos, bool with_align,
									uint8_t align, int32_t px, int32_t py)
	{
		PacketHeader h {};
		h.start_ticks = start;
		h.end_ticks = end;
		h.has_position = with_pos ? 1u : 0u;
		h.has_alignment = with_align ? 1u : 0u;
		h.alignment = align;
		h.flags = static_cast<uint8_t>((with_pos ? 1u : 0u) | ((with_align ? 1u : 0u) << 1));
		h.position_x = px;
		h.position_y = py;
		std::vector<uint8_t> buf(64 + std::strlen(text));
		size_t n = 0;
		EXPECT_EQ(
			EncodePacket(&h, reinterpret_cast<const uint8_t*>(text), std::strlen(text), buf.data(), buf.size(), &n),
			kOk);
		buf.resize(n);
		return buf;
	}
}	 // namespace

TEST(SubtitlePacket, RoundTripsAndTranslatesToContainerPts)
{
	const std::vector<uint8_t> pkt = MakePacket(1000, 3000, "hello", true, true, 2, -4, 7);
	PacketHeader h {};
	// 容器 pts 5000 ⇒ 平移 +4000
	ASSERT_EQ(ParsePacketHeader(pkt.data(), pkt.size(), 5000, 0, &h), kOk);
	EXPECT_EQ(h.start_ticks, 5000);
	EXPECT_EQ(h.end_ticks, 7000);
	EXPECT_EQ(h.delta_ticks, 4000);
	EXPECT_EQ(h.text_len, 5u);
	EXPECT_EQ(h.header_len, clv::subtitle::kPacketHeaderFixedLen + 8u + 1u);
	EXPECT_EQ(h.position_x, -4);
	EXPECT_EQ(h.position_y, 7);
	EXPECT_EQ(h.alignment, 2u);
}

TEST(SubtitlePacket, RejectsStructuralDefectsInFixedOrder)
{
	const std::vector<uint8_t> good = MakePacket(0, 10, "xy", false, false, 0, 0, 0);
	PacketHeader h {};

	// 定长头不全
	EXPECT_EQ(ParsePacketHeader(good.data(), 20, 0, 0, &h), kPacketHeaderBad);
	// 长度不符（多一个尾随字节）
	std::vector<uint8_t> tail = good;
	tail.push_back(0);
	EXPECT_EQ(ParsePacketHeader(tail.data(), tail.size(), 0, 0, &h), kPacketHeaderBad);
	// flags 高位
	std::vector<uint8_t> badflags = good;
	badflags[20] = 0x04u;
	EXPECT_EQ(ParsePacketHeader(badflags.data(), badflags.size(), 0, 0, &h), kPacketHeaderBad);
	// end <= start：先造合法包，再把 end 改写成与 start 相等（编码侧会拒，故手改字节）
	std::vector<uint8_t> badtime = MakePacket(10, 20, "xy", false, false, 0, 0, 0);
	badtime[8] = 10;	// 小端：end = 10，与 start 相等
	for (int i = 9; i < 16; ++i) badtime[static_cast<size_t>(i)] = 0;
	EXPECT_EQ(ParsePacketHeader(badtime.data(), badtime.size(), 0, 0, &h), kPacketHeaderBad);
	// 对齐枚举越界：先造合法包，再把对齐字节改成 3（编码侧会拒 3，所以要手改字节）
	std::vector<uint8_t> badalign = MakePacket(0, 10, "xy", false, true, 2, 0, 0);
	badalign[clv::subtitle::kPacketHeaderFixedLen] = 3;
	EXPECT_EQ(ParsePacketHeader(badalign.data(), badalign.size(), 0, 0, &h), kPacketHeaderBad);
	// 空指针
	EXPECT_EQ(ParsePacketHeader(nullptr, 0, 0, 0, &h), kInvalidArgument);
}

TEST(SubtitlePacket, EnforcesPacketSizeLimit)
{
	// 头写成一个合法但超限的包：header_len = 21，text_len = kMaxPacketSize（21 + text > 4096）
	const uint32_t text = kMaxPacketSize;
	const size_t size = clv::subtitle::kPacketHeaderFixedLen + text;
	std::vector<uint8_t> big(size, 0);
	big[16] = static_cast<uint8_t>(text);
	big[17] = static_cast<uint8_t>(text >> 8);
	big[18] = static_cast<uint8_t>(text >> 16);
	big[19] = static_cast<uint8_t>(text >> 24);
	big[8] = 10;	// end
	PacketHeader h {};
	EXPECT_EQ(ParsePacketHeader(big.data(), size, 0, 0, &h), kPacketTooLarge);
}

TEST(SubtitlePacket, EncodeReportsNeededLengthOnSmallCapacity)
{
	PacketHeader h {};
	h.start_ticks = 0;
	h.end_ticks = 100;
	size_t need = 0;
	uint8_t one = 0;
	EXPECT_EQ(EncodePacket(&h, reinterpret_cast<const uint8_t*>("ab"), 2, &one, 0, &need), kOutputTooSmall);
	EXPECT_EQ(need, clv::subtitle::kPacketHeaderFixedLen + 2u);
	std::vector<uint8_t> buf(need);
	EXPECT_EQ(EncodePacket(&h, reinterpret_cast<const uint8_t*>("ab"), 2, buf.data(), buf.size(), &need), kOk);
	EXPECT_EQ(need, buf.size());
}
