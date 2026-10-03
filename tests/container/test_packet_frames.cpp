#include "container/frames.h"
#include "container/packet.h"
#include "core/crc32.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <string>
#include <vector>

namespace
{

	using clv::container::EncodePacket;
	using clv::container::FrameAssembler;
	using clv::container::PacketFields;
	using clv::container::PacketParse;
	using clv::container::ParsedPacket;
	using clv::container::ReassembledFrame;

	constexpr uint64_t kMaxPayload = 1u << 20;
	constexpr uint64_t kLimit = (1u << 20) + 41;

	std::vector<uint8_t> Build(const PacketFields& f, size_t payload_size, uint64_t max_payload = kMaxPayload)
	{
		std::vector<uint8_t> payload(payload_size, 0x5A);
		std::vector<uint8_t> out;
		const size_t wire = EncodePacket(f, payload.data(), payload_size, max_payload, out);
		if (wire == 0) out.clear();
		return out;
	}

	PacketFields Base()
	{
		PacketFields f;
		f.stream_id = 1;
		f.dts_delta = 100;
		f.pts_delta = 0;
		return f;
	}

	void AppendCrc(std::vector<uint8_t>& body)
	{
		// body 已含 4 字节 crc 占位，重算覆盖 [4, total_size)
		const uint32_t total = static_cast<uint32_t>(body.size()) - 4u;
		const uint32_t crc = clv::core::Crc32(body.data() + 4, total - 4);
		for (int i = 0; i < 4; ++i) body[4 + total - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
	}

	// 造一个字段区含非最短 varint 的包：payload_size = 0 却占两字节，CRC 按实际字节重算，
	// 于是只有最短形式检查能挡下它（包边界本身是自证的）
	std::vector<uint8_t> BuildNonShortestPacket(uint8_t stream_id)
	{
		std::vector<uint8_t> body;
		body.push_back(stream_id);
		body.push_back(0x08);	 // flags: has_pts = 1
		body.push_back(0x64);	 // dts_delta = 100
		body.push_back(0x00);	 // pts_delta = 0
		body.push_back(0x80);	 // payload_size = 0，非最短：占两字节
		body.push_back(0x00);
		body.push_back(0x00);			  // ext_len_varint = 0
		body.insert(body.end(), 4, 0);	  // crc32 占位

		std::vector<uint8_t> bytes;
		const uint32_t total = static_cast<uint32_t>(body.size());
		for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(total >> (8 * i)));
		bytes.insert(bytes.end(), body.begin(), body.end());
		AppendCrc(bytes);
		return bytes;
	}

}	 // namespace

TEST(ContainerPacket, RoundTripsSinglePacketFrame)
{
	const std::vector<uint8_t> bytes = Build(Base(), 300);
	ASSERT_FALSE(bytes.empty());

	ParsedPacket p;
	ASSERT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kOk);
	EXPECT_EQ(p.stream_id, 1);
	EXPECT_EQ(p.dts_delta, 100u);
	EXPECT_EQ(p.pts_delta, 0);
	EXPECT_EQ(p.payload_size, 300u);
	EXPECT_TRUE(p.IsFrameFirst());
	EXPECT_FALSE(p.is_fragment);
	EXPECT_EQ(p.payload.size(), 300u);
	EXPECT_EQ(p.WireSize(), bytes.size());
}

TEST(ContainerPacket, NegativePtsDeltaSurvivesZigZag)
{
	PacketFields f = Base();
	f.pts_delta = -45090045;	// open-GOP：PTS 落在 DTS 之前
	const std::vector<uint8_t> bytes = Build(f, 16);
	ASSERT_FALSE(bytes.empty());

	ParsedPacket p;
	ASSERT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kOk);
	EXPECT_EQ(p.pts_delta, -45090045);
}

TEST(ContainerPacket, FragmentPacketsAreNotFrameFirstAndCarryNoPts)
{
	PacketFields first = Base();
	first.is_fragment = true;
	first.fragment_index = 0;
	first.is_last_fragment = false;
	first.pts_delta = 77;

	std::vector<uint8_t> stream;
	const size_t first_wire = EncodePacket(first, nullptr, 0, kMaxPayload, stream);
	ASSERT_GT(first_wire, 0u);

	const size_t head_size = stream.size();
	PacketFields second = first;
	second.fragment_index = 1;
	second.is_last_fragment = true;
	ASSERT_GT(EncodePacket(second, nullptr, 0, kMaxPayload, stream), 0u);

	ParsedPacket a, b;
	ASSERT_EQ(DecodePacket(stream.data(), head_size, kLimit, &a), PacketParse::kOk);
	ASSERT_TRUE(a.IsFrameFirst());
	EXPECT_EQ(a.pts_delta, 77);

	const uint8_t* body = stream.data() + head_size;
	const size_t body_len = stream.size() - head_size;
	ASSERT_EQ(DecodePacket(body, body_len, kLimit, &b), PacketParse::kOk);
	EXPECT_FALSE(b.IsFrameFirst());
	EXPECT_EQ(b.fragment_index, 1u);
	EXPECT_TRUE(b.is_last_fragment);
	// 后续分片不带 pts_delta，同载荷下应比帧首包短
	EXPECT_LT(b.total_size, a.total_size);
}

TEST(ContainerPacket, RejectsNonShortestVarintAsFieldFailure)
{
	const std::vector<uint8_t> bytes = BuildNonShortestPacket(1);

	ParsedPacket p;
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kFieldFail);
}

TEST(ContainerPacket, FailedPacketIsSkippedByDecodedTotalSize)
{
	// 双断言：① 非最短形式的包判字段区失败；② 跳包长度取解码出的 total_size——
	// 跳完正好落在下一包第一个字节上，下一包照常解出，证明没按字节形态猜边界
	const std::vector<uint8_t> bad = BuildNonShortestPacket(1);
	const std::vector<uint8_t> next = Build(Base(), 64);
	ASSERT_FALSE(next.empty());

	std::vector<uint8_t> stream = bad;
	stream.insert(stream.end(), next.begin(), next.end());

	ParsedPacket p;
	ASSERT_EQ(DecodePacket(stream.data(), stream.size(), kLimit, &p), PacketParse::kFieldFail);
	const size_t skip = static_cast<size_t>(p.total_size) + 4u;
	EXPECT_EQ(skip, bad.size());

	ParsedPacket q;
	ASSERT_EQ(DecodePacket(stream.data() + skip, stream.size() - skip, kLimit, &q), PacketParse::kOk);
	EXPECT_EQ(q.stream_id, 1);
	EXPECT_EQ(q.payload_size, 64u);
	EXPECT_EQ(q.WireSize(), next.size());
}

TEST(ContainerPacket, ResyncsOnHeadLevelProblems)
{
	std::vector<uint8_t> bytes = Build(Base(), 64);
	ASSERT_FALSE(bytes.empty());
	ParsedPacket p;

	bytes[5] |= 0x10;	 // has_fec = 1 → 指纹不符
	AppendCrc(bytes);
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kHeadInvalid);

	bytes = Build(Base(), 64);
	bytes[5] &= static_cast<uint8_t>(~0x08);	// has_pts = 0 → 指纹不符
	AppendCrc(bytes);
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kHeadInvalid);

	bytes = Build(Base(), 64);
	bytes[5] |= 0x20;	 // reserved bit5 → 指纹不符
	AppendCrc(bytes);
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kHeadInvalid);

	bytes = Build(Base(), 64);
	bytes[0] = 2;
	bytes[1] = 0;
	bytes[2] = 0;
	bytes[3] = 0;	 // total_size = 2 < 9
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kHeadInvalid);
}

TEST(ContainerPacket, DropsPacketOnCrcFailure)
{
	std::vector<uint8_t> bytes = Build(Base(), 64);
	ASSERT_FALSE(bytes.empty());
	bytes.back() ^= 0xFF;	 // 只坏 CRC，不动包头

	ParsedPacket p;
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), kLimit, &p), PacketParse::kCrcFail);
}

TEST(ContainerPacket, DropsPacketWhenOuterLimitExceeded)
{
	const std::vector<uint8_t> bytes = Build(Base(), 64);
	ASSERT_FALSE(bytes.empty());

	ParsedPacket p;
	EXPECT_EQ(DecodePacket(bytes.data(), bytes.size(), 10ull, &p), PacketParse::kTooLarge);
}

TEST(ContainerPacket, WriterRefusesPayloadOverMax)
{
	PacketFields f = Base();
	std::vector<uint8_t> out;
	EXPECT_EQ(EncodePacket(f, nullptr, 100, 64, out), 0u);
}

TEST(ContainerPacket, LastFragmentFlagNeedsFragment)
{
	PacketFields f = Base();
	f.is_last_fragment = true;	  // 未分片却声称末片
	std::vector<uint8_t> out;
	EXPECT_EQ(EncodePacket(f, nullptr, 0, kMaxPayload, out), 0u);
}

TEST(ContainerFrames, EmitsSinglePacketFramesImmediately)
{
	FrameAssembler a(0);
	std::vector<ReassembledFrame> out;

	ParsedPacket p;
	p.stream_id = 0;
	p.payload.assign(8, 0x11);
	p.dts_delta = 5;
	p.pts_delta = -1;
	ASSERT_EQ(a.Push(p, out), 1u);
	ASSERT_EQ(out.size(), 1u);
	EXPECT_TRUE(out[0].complete);
	EXPECT_EQ(out[0].pts_delta, -1);
	EXPECT_EQ(out[0].payload.size(), 8u);
}

TEST(ContainerFrames, PlacesFragmentsByIndexNotArrivalOrder)
{
	FrameAssembler a(0);
	std::vector<ReassembledFrame> out;

	auto frag = [](uint64_t index, uint8_t fill, bool last)
	{
		ParsedPacket p;
		p.stream_id = 0;
		p.is_fragment = true;
		p.fragment_index = index;
		p.is_last_fragment = last;
		p.payload.assign(4, fill);
		return p;
	};

	a.Push(frag(0, 0xA0, false), out);
	a.Push(frag(2, 0xA2, false), out);	  // 跳变：记一次丢片
	a.Push(frag(1, 0xA1, false), out);	  // 迟到的片按序号落回原位
	a.Push(frag(3, 0xA3, true), out);	  // 末片收口

	ASSERT_EQ(out.size(), 1u);
	EXPECT_TRUE(out[0].complete);
	EXPECT_EQ(out[0].fragments_received, 4u);
	EXPECT_EQ(out[0].fragments_expected, 4u);
	ASSERT_EQ(out[0].payload.size(), 16u);
	EXPECT_EQ(out[0].payload[0], 0xA0);
	EXPECT_EQ(out[0].payload[4], 0xA1);
	EXPECT_EQ(out[0].payload[8], 0xA2);
	EXPECT_EQ(out[0].payload[12], 0xA3);
	EXPECT_EQ(a.Stats().fragment_gaps, 1u);
}

TEST(ContainerFrames, DropsTailWithoutFirstPacket)
{
	FrameAssembler a(0);
	std::vector<ReassembledFrame> out;

	ParsedPacket p;
	p.stream_id = 0;
	p.is_fragment = true;
	p.fragment_index = 3;
	p.payload.assign(4, 0xBB);
	ASSERT_EQ(a.Push(p, out), 0u);
	EXPECT_EQ(a.Stats().missing_first_packets, 1u);
}

TEST(ContainerFrames, LastFragmentMismatchMarksIncomplete)
{
	FrameAssembler a(0);
	std::vector<ReassembledFrame> out;

	auto frag = [](uint64_t index, bool last)
	{
		ParsedPacket p;
		p.stream_id = 0;
		p.is_fragment = true;
		p.fragment_index = index;
		p.is_last_fragment = last;
		p.payload.assign(2, 0xCC);
		return p;
	};

	a.Push(frag(0, false), out);
	a.Push(frag(4, true), out);	   // 声明五片，实收两片

	ASSERT_EQ(out.size(), 1u);
	EXPECT_FALSE(out[0].complete);
	EXPECT_EQ(out[0].fragments_expected, 5u);
	EXPECT_EQ(out[0].fragments_received, 2u);
	EXPECT_EQ(a.Stats().fragment_gaps, 4u);	   // 跳变 3 + 末片比对 1
}

TEST(ContainerFrames, HugeFragmentIndexCostsOneSlotOnly)
{
	// 序号取到 2^40 也只按实际到达的片增长，不按序号值分配
	FrameAssembler a(0);
	std::vector<ReassembledFrame> out;

	auto frag = [](uint64_t index, bool last)
	{
		ParsedPacket p;
		p.stream_id = 0;
		p.is_fragment = true;
		p.fragment_index = index;
		p.is_last_fragment = last;
		p.payload.assign(2, 0xDD);
		return p;
	};

	a.Push(frag(0, false), out);
	a.Push(frag(1ull << 40, true), out);

	ASSERT_EQ(out.size(), 1u);
	EXPECT_FALSE(out[0].complete);
	EXPECT_EQ(out[0].fragments_received, 2u);
	EXPECT_EQ(out[0].payload.size(), 4u);
}

TEST(ContainerFrames, FlushEmitsUnfinishedFrame)
{
	FrameAssembler a(0);
	std::vector<ReassembledFrame> out;

	ParsedPacket p;
	p.stream_id = 0;
	p.is_fragment = true;
	p.fragment_index = 0;
	p.payload.assign(3, 0xEE);
	a.Push(p, out);
	EXPECT_TRUE(out.empty());

	ASSERT_EQ(a.Flush(out), 1u);
	ASSERT_EQ(out.size(), 1u);
	EXPECT_FALSE(out[0].complete);
	EXPECT_EQ(out[0].fragments_expected, 0u);
}
