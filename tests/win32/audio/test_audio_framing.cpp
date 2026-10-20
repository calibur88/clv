/* test_audio_framing.cpp
 *
 * **两面混用**：主体走公开 C ABI（`CLV_AudioInspectFrame` / `CLV_AudioClassifyMetaBlock` / `CLV_AudioSplitMetaBlock` /
 *   `CLV_AudioStripChannelMask` / `CLV_AudioParseVorbisComment` 与 Blob 口，按「函数名 + 左括号」在本件正文里数得 75 处调用点），
 *   另有 4 个实现层函数、6 处调用点（`clv::audio::FrameCrcOk` 三处，`Crc8` / `ParseFrameHead` / `StreamableSubsetOk` 各一处），
 *   再外加 `audio/frame.h` 的界值常量与 `audio/metadata.h` 的 `kStreamInfoSize`——正是这几处直调让本件
 *   **仅静态构建（`CLV_BUILD_SHARED=OFF`）下可链**；本件并非只走公开 ABI，那几处直调就是例外。
 *
 * 音频结构层（帧头自核 / 流信息 / 承载块分类 / 声道掩码剥离 / 注释块视图查询）的用例。
 *
 * 素材来自 `audio_fixtures_p.h`（`scripts/gen_audio_fixtures.py` 的产物），帧字节从真实 FLAC 件按帧边界切出，
 * 期望值由另一份独立实现的位域解析件解出，并与该流的流信息交叉核对。
 * 句柄类函数（编码 / 解码闭环）的用例不在本件，见 `test_audio_codec.cpp`。
 */

#include "CLV_Audio.h"
#include "audio/frame.h"
#include "audio/metadata.h"
#include "audio_fixtures_p.h"

#include "gtest/gtest.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

	using clv::audio::FrameCrcOk;

	const char* kMaskKey = "WAVEFORMATEXTENSIBLE_CHANNEL_MASK";

	bool key_eq(const std::string& a, const char* b)
	{
		std::string up = a;
		for (char& c: up)
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
		std::string ref(b);
		for (char& c: ref)
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
		return up == ref;
	}

	/* 把注释块拆成 {键, 整条原文}，用于比对剥离前后除掩码以外的条目是否逐字节保留。 */
	std::vector<std::pair<std::string, std::string>> walk_comments(const uint8_t* block, size_t size)
	{
		std::vector<std::pair<std::string, std::string>> out;
		if (size < 8u) return out;
		const size_t vendor = static_cast<size_t>(block[0] | (block[1] << 8) | (block[2] << 16) |
												  (static_cast<uint32_t>(block[3]) << 24));
		size_t pos = 4u + vendor;
		if (pos + 4u > size) return out;
		const size_t count = static_cast<size_t>(block[pos] | (block[pos + 1] << 8) | (block[pos + 2] << 16) |
												 (static_cast<uint32_t>(block[pos + 3]) << 24));
		pos += 4u;
		for (size_t i = 0; i < count; ++i)
		{
			if (pos + 4u > size) return out;
			const size_t len = static_cast<size_t>(block[pos] | (block[pos + 1] << 8) | (block[pos + 2] << 16) |
												   (static_cast<uint32_t>(block[pos + 3]) << 24));
			pos += 4u;
			if (pos + len > size) return out;
			std::string item(reinterpret_cast<const char*>(block + pos), len);
			const size_t eq = item.find('=');
			out.emplace_back(eq == std::string::npos ? item : item.substr(0, eq), item);
			pos += len;
		}
		return out;
	}

	/* 造一条定块流的帧头：帧号按扩展 UTF-8 写成 lead 个字节，尾部 CRC-8 现算。
	 * 块大小码 0b0001 = 192 样本、采样率码 0b1001 = 44 100 Hz、声道码 0 = 单声道、位深码 0 = 只在流信息里给。
	 * lead 取 2..7 ⇒ 总长 = 4 + lead + 1 ≥ 7，正好落在帧头最小可解析长度之上。 */
	std::vector<uint8_t> FrameHeadWithNumber(uint64_t number, unsigned lead)
	{
		std::vector<uint8_t> num;
		uint64_t rest = number;
		if (lead == 1u)
		{
			num.push_back(static_cast<uint8_t>(rest & 0x7Fu));
		}
		else
		{
			for (unsigned k = 1u; k < lead; ++k)
			{
				num.insert(num.begin(), static_cast<uint8_t>(0x80u | (rest & 0x3Fu)));
				rest >>= 6u;
			}
			num.insert(num.begin(), static_cast<uint8_t>((0xFFu << (8u - lead)) | (rest & 0x7Fu)));
		}
		std::vector<uint8_t> h = { 0xFFu, 0xF8u, 0x19u, 0x00u };
		h.insert(h.end(), num.begin(), num.end());
		h.push_back(clv::audio::Crc8(h.data(), h.size()));
		// 帧头之后至少还得留得下两字节帧尾校验，否则解析器按「整帧读不进缓冲」拒判，与本用例的判定点无关
		h.push_back(0x00u);
		h.push_back(0x00u);
		return h;
	}

	CLV_AudioFrameHeadC inspect(const unsigned char* bytes, unsigned len)
	{
		CLV_AudioFrameHeadC h;
		std::memset(&h, 0, sizeof(h));
		const CLV_AudioError e = CLV_AudioInspectFrame(bytes, len, &h);
		EXPECT_EQ(e, CLV_AUDIO_OK) << "InspectFrame 返回 " << CLV_AudioStrError(e);
		return h;
	}

	/* 注释块组装：厂商串 + 条目表。掩码剥离的「保留其余条目」在真素材上没有可遍历的对象
	 * （kVorbisMask 只装掩码键那一条），多条目形状靠这里现造，条目按给出的顺序进块。 */
	void PushLE32(std::vector<uint8_t>& block, uint32_t v)
	{
		block.push_back(static_cast<uint8_t>(v & 0xFFu));
		block.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
		block.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
		block.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
	}

	std::vector<uint8_t> BuildCommentBlock(const std::string& vendor, const std::vector<std::string>& items)
	{
		std::vector<uint8_t> block;
		PushLE32(block, static_cast<uint32_t>(vendor.size()));
		block.insert(block.end(), vendor.begin(), vendor.end());
		PushLE32(block, static_cast<uint32_t>(items.size()));
		for (const std::string& s: items)
		{
			PushLE32(block, static_cast<uint32_t>(s.size()));
			block.insert(block.end(), s.begin(), s.end());
		}
		return block;
	}

	uint32_t LE32At(const uint8_t* p)
	{
		return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
	}

	// 条目出参「没被写过」的记号：四个字段都停在填充值上（逐字段比，不按整对象字节比，结构体空隙不在判据里）
	CLV_AudioCommentEntryC SentinelEntry()
	{
		CLV_AudioCommentEntryC e;
		std::memset(&e, 0xFF, sizeof(e));
		return e;
	}

	bool EntryUntouched(const CLV_AudioCommentEntryC& e)
	{
		const auto all_ones = reinterpret_cast<const uint8_t*>(~static_cast<size_t>(0u));
		return e.key == all_ones && e.value == all_ones && e.key_length == 0xFFFFFFFFu && e.value_length == 0xFFFFFFFFu;
	}

	/* 按 src/audio/metadata.h 件头那张位域表现搭一份流信息载荷（34 字节）：
	 *   [0..1] 最小块大小 BE16 | [2..3] 最大块大小 BE16 | [4..6] 最小帧大小 BE24 | [7..9] 最大帧大小 BE24
	 *   [10..17] 采样率 20 位 | 声道数 − 1 取 3 位 | 位深 − 1 取 5 位 | 总样本数 36 位（同一 64 位大字，大端）
	 *   [18..33] MD5（本版本恒零，不参与校验）
	 * 位移与「存的是值 − 1」两处口径照读方那一份算式（src/audio/metadata.cpp 的 ParseStreamInfo）。 */
	std::vector<uint8_t> MakeStreamInfo(uint16_t min_block, uint16_t max_block, uint32_t rate, uint16_t channels,
										uint16_t bit_depth, uint64_t total_samples)
	{
		std::vector<uint8_t> b(clv::audio::kStreamInfoSize, 0u);
		b[0] = static_cast<uint8_t>(min_block >> 8u);
		b[1] = static_cast<uint8_t>(min_block & 0xFFu);
		b[2] = static_cast<uint8_t>(max_block >> 8u);
		b[3] = static_cast<uint8_t>(max_block & 0xFFu);
		const uint64_t packed =
			(static_cast<uint64_t>(rate & 0xFFFFFu) << 44u) | (static_cast<uint64_t>((channels - 1u) & 0x7u) << 41u) |
			(static_cast<uint64_t>((bit_depth - 1u) & 0x1Fu) << 36u) | (total_samples & 0xFFFFFFFFFull);
		for (int i = 0; i < 8; ++i)
			b[10 + i] = static_cast<uint8_t>((packed >> (56u - 8u * static_cast<unsigned>(i))) & 0xFFu);
		return b;
	}

}	 // namespace

TEST(AudioFraming, FixedBlockFrameHeadMatchesStreamInfo)
{
	const CLV_AudioFrameHeadC h = inspect(clvtest::kFrame16FixedBytes, clvtest::kFrame16FixedLen);
	EXPECT_EQ(h.blocksize, clvtest::kFrame16FixedBlocksize);
	EXPECT_EQ(h.sample_rate, clvtest::kFrame16FixedRate);
	EXPECT_EQ(h.channels, clvtest::kFrame16FixedChannels);
	EXPECT_EQ(h.bit_depth, clvtest::kFrame16FixedDepth);
	EXPECT_EQ(h.frame_or_sample_number, clvtest::kFrame16FixedNumber);
	EXPECT_EQ(h.blocking_strategy, 0u);
	EXPECT_EQ(h.header_size, clvtest::kFrame16FixedHdr);
	// 前两字节即帧同步 + 块策略位：固定块流为 0xFFF8
	EXPECT_EQ(clvtest::kFrame16FixedBytes[0], 0xFFu);
	EXPECT_EQ(clvtest::kFrame16FixedBytes[1], 0xF8u);

	/* 用例名承诺的「与流信息对账」在这里补实：kFrame16Fixed 与 kStreamInfo 同出 at1_默认.flac，
	 * 流信息那 34 字节由生成器按流信息块独立解出、与帧头解析不同源 ⇒ 四个字段逐项对上才算对账成立。 */
	EXPECT_EQ(h.sample_rate, clvtest::kStreamInfoRate);
	EXPECT_EQ(h.channels, clvtest::kStreamInfoChannels);
	EXPECT_EQ(h.bit_depth, clvtest::kStreamInfoDepth);
	EXPECT_EQ(clvtest::kStreamInfoMinBlock, clvtest::kStreamInfoMaxBlock) << "本例前提是固定块流（min == max）";
	EXPECT_EQ(h.blocksize, clvtest::kStreamInfoMinBlock);
	EXPECT_EQ(h.frame_or_sample_number, 0ull) << "整首帧的编码号就是首样本号";
}

TEST(AudioFraming, VarBlockFrameSetsBlockingStrategyBit)
{
	const CLV_AudioFrameHeadC h = inspect(clvtest::kFrameVarBlockBytes, clvtest::kFrameVarBlockLen);
	EXPECT_EQ(h.blocking_strategy, 1u);
	EXPECT_EQ(clvtest::kFrameVarBlockBytes[1], 0xF9u);
	EXPECT_EQ(h.blocksize, clvtest::kFrameVarBlockBlocksize);
	EXPECT_EQ(h.bit_depth, clvtest::kFrameVarBlockDepth);
	// 变长块流的编码号是首样本号，与帧号不同源
	EXPECT_EQ(h.frame_or_sample_number, clvtest::kFrameVarBlockNumber);
}

TEST(AudioFraming, DepthAndChannelCodesDecode)
{
	const CLV_AudioFrameHeadC h24 = inspect(clvtest::kFrame24FixedBytes, clvtest::kFrame24FixedLen);
	EXPECT_EQ(h24.bit_depth, 24u);
	const CLV_AudioFrameHeadC h6 = inspect(clvtest::kFrame6ChBytes, clvtest::kFrame6ChLen);
	EXPECT_EQ(h6.channels, 6u);
	EXPECT_EQ(h6.blocksize, clvtest::kFrame6ChBlocksize);
}

TEST(AudioFraming, BigBlocksizeViolatesStreamableSubset)
{
	CLV_AudioFrameHeadC h;
	std::memset(&h, 0, sizeof(h));
	const CLV_AudioError e = CLV_AudioInspectFrame(clvtest::kFrameBig32768Bytes, clvtest::kFrameBig32768Len, &h);
	EXPECT_EQ(e, CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE);
	// 帧头本身是自洽的：32768 = 2^15，块大小码 0b1111 合法，只是越出子集
	clv::audio::FrameHead raw;
	std::memset(&raw, 0, sizeof(raw));
	ASSERT_TRUE(clv::audio::ParseFrameHead(clvtest::kFrameBig32768Bytes, clvtest::kFrameBig32768Len, &raw));
	EXPECT_EQ(raw.blocksize, 32768u);
	EXPECT_FALSE(clv::audio::StreamableSubsetOk(raw.blocksize, raw.sample_rate));
}

TEST(AudioFraming, ReservedAndForbiddenCodesRejected)
{
	std::vector<unsigned char> base(clvtest::kFrame16FixedBytes,
									clvtest::kFrame16FixedBytes + clvtest::kFrame16FixedLen);
	CLV_AudioFrameHeadC h;

	// 同步码后的保留位
	std::vector<unsigned char> a = base;
	a[1] = static_cast<unsigned char>(a[1] | 0x04u);
	EXPECT_EQ(CLV_AudioInspectFrame(a.data(), a.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);

	// 位深后的保留位
	std::vector<unsigned char> b = base;
	b[3] = static_cast<unsigned char>(b[3] | 0x01u);
	EXPECT_EQ(CLV_AudioInspectFrame(b.data(), b.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);

	// 块大小码 0b0000（保留）/ 采样率码 0b1111（禁用）/ 位深码 0b011（保留）/ 声道码 0b1011（保留）
	std::vector<unsigned char> c1 = base;
	c1[2] = static_cast<unsigned char>(c1[2] & 0x0Fu);
	EXPECT_EQ(CLV_AudioInspectFrame(c1.data(), c1.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);
	std::vector<unsigned char> c2 = base;
	c2[2] = static_cast<unsigned char>(c2[2] | 0x0Fu);
	EXPECT_EQ(CLV_AudioInspectFrame(c2.data(), c2.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);
	std::vector<unsigned char> c3 = base;
	c3[3] = static_cast<unsigned char>((c3[3] & 0xF0u) | (3u << 1u));
	EXPECT_EQ(CLV_AudioInspectFrame(c3.data(), c3.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);
	std::vector<unsigned char> c4 = base;
	c4[3] = static_cast<unsigned char>((c4[3] & 0x0Fu) | (0xBu << 4u));
	EXPECT_EQ(CLV_AudioInspectFrame(c4.data(), c4.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);

	// 帧同步本身不符
	std::vector<unsigned char> d = base;
	d[0] = 0xFEu;
	EXPECT_EQ(CLV_AudioInspectFrame(d.data(), d.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);

	// 帧头 CRC-8 被改
	std::vector<unsigned char> e2 = base;
	e2[5] = static_cast<unsigned char>(e2[5] ^ 0xA5u);
	EXPECT_EQ(CLV_AudioInspectFrame(e2.data(), e2.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD);

	// 载荷过短、出参为空、入参为空
	EXPECT_EQ(CLV_AudioInspectFrame(base.data(), 5u, &h), CLV_AUDIO_FRAME_HEADER_BAD);
	EXPECT_EQ(CLV_AudioInspectFrame(base.data(), base.size(), nullptr), CLV_AUDIO_INVALID_ARGUMENT);
	EXPECT_EQ(CLV_AudioInspectFrame(nullptr, 14u, &h), CLV_AUDIO_INVALID_ARGUMENT);
}

TEST(AudioFraming, FrameTailCrcCoversWholeFrame)
{
	std::vector<unsigned char> frame(clvtest::kFrame16FixedBytes,
									 clvtest::kFrame16FixedBytes + clvtest::kFrame16FixedLen);
	ASSERT_TRUE(FrameCrcOk(frame.data(), frame.size()));
	frame[frame.size() - 3u] = static_cast<unsigned char>(frame[frame.size() - 3u] ^ 0x01u);
	EXPECT_FALSE(FrameCrcOk(frame.data(), frame.size()));
	// 帧长不足三个字节谈不上校验
	EXPECT_FALSE(FrameCrcOk(frame.data(), 2u));
}

TEST(AudioFraming, StreamInfoBlockParsesToItsOwnNumbers)
{
	CLV_AudioStreamInfoC si;
	std::memset(&si, 0, sizeof(si));
	ASSERT_EQ(CLV_AudioParseStreamInfo(clvtest::kStreamInfoBytes, clvtest::kStreamInfoLen, &si), CLV_AUDIO_OK);
	EXPECT_EQ(si.min_blocksize, clvtest::kStreamInfoMinBlock);
	EXPECT_EQ(si.max_blocksize, clvtest::kStreamInfoMaxBlock);
	EXPECT_EQ(si.min_framesize, clvtest::kStreamInfoMinFrame);
	EXPECT_EQ(si.max_framesize, clvtest::kStreamInfoMaxFrame);
	EXPECT_EQ(si.sample_rate, clvtest::kStreamInfoRate);
	EXPECT_EQ(si.channels, clvtest::kStreamInfoChannels);
	EXPECT_EQ(si.bit_depth, clvtest::kStreamInfoDepth);
	EXPECT_EQ(si.total_samples, clvtest::kStreamInfoTotalSamples);
	// MD5 头两字节取自素材，逐字节对得上生成器
	EXPECT_EQ(si.md5[0], clvtest::kStreamInfoBytes[18]);
	EXPECT_EQ(si.md5[15], clvtest::kStreamInfoBytes[33]);
}

TEST(AudioFraming, StreamInfoGuardsRejectBadShape)
{
	CLV_AudioStreamInfoC si;
	std::memset(&si, 0, sizeof(si));
	EXPECT_EQ(CLV_AudioParseStreamInfo(clvtest::kStreamInfoBytes, 33u, &si), CLV_AUDIO_VALUE_RANGE);
	EXPECT_EQ(CLV_AudioParseStreamInfo(clvtest::kStreamInfoBytes, 35u, &si), CLV_AUDIO_VALUE_RANGE);
	EXPECT_EQ(CLV_AudioParseStreamInfo(nullptr, 34u, &si), CLV_AUDIO_INVALID_ARGUMENT);

	std::vector<unsigned char> bad(clvtest::kStreamInfoBytes, clvtest::kStreamInfoBytes + 34u);
	bad[1] = 0xFFu;
	bad[3] = 0x00u;	   // 最小块大小 > 最大块大小
	EXPECT_EQ(CLV_AudioParseStreamInfo(bad.data(), bad.size(), &si), CLV_AUDIO_VALUE_RANGE);
}

// 流信息核对的界值都取自 src/audio/frame.h 的具名常量，本件只把它们与公开头的合法域对平一次
static_assert(clv::audio::kSubsetMinBlocksize16 == 64u, "16-bit 档的块大小下界");
static_assert(clv::audio::kSubsetMinBlocksize24 == 72u, "24-bit 档的块大小下界");
static_assert(clv::audio::kSubsetSampleRateRejectFloor == 655355u, "采样率拒档的边界");
static_assert(clv::audio::kStreamInfoSize == 34u, "流信息载荷定长");

/* 流信息块的值域三档（判点见 src/audio/metadata.cpp 的 ParseStreamInfo，公开头的承诺是
 * 「字段越出 FLAC 子集就返错误码」）：位深集合、采样率拒档、块大小下界按位深分档。
 * 采样率这一档只登记被拒那一侧的界：采样率的实际可接受区间由后端决定，超出区间在打开编码器时被拒。 */
TEST(AudioFraming, StreamInfoRejectsOutOfRangeDepthRateAndBlocksizeFloor)
{
	CLV_AudioStreamInfoC si;
	std::memset(&si, 0, sizeof(si));

	// 合法对照档：44100 / 2ch / 16bit / min = max = 4096，与真素材那份流信息同形
	const std::vector<uint8_t> ok16 = MakeStreamInfo(4096u, 4096u, 44100u, 2u, 16u, 442368ull);
	ASSERT_EQ(CLV_AudioParseStreamInfo(ok16.data(), ok16.size(), &si), CLV_AUDIO_OK) << "对照档被误拒";

	/* 判点一（位深集合 = {16, 24}）：8 / 12 / 20 / 32 都落在 5 位可表达域内，
		 * 但不在子集声明的集合里 ⇒ VALUE_RANGE。24-bit 的对照档块大小取 4096（≥ 该档下界 72）。 */
	static const uint16_t kBadBits[] = { 8u, 12u, 20u, 32u };
	for (const uint16_t bits: kBadBits)
	{
		const std::vector<uint8_t> b = MakeStreamInfo(4096u, 4096u, 44100u, 2u, bits, 442368ull);
		SCOPED_TRACE("位深 " + std::to_string(bits));
		EXPECT_EQ(CLV_AudioParseStreamInfo(b.data(), b.size(), &si), CLV_AUDIO_VALUE_RANGE) << "位深集合之外的值被受理";
	}
	static const uint16_t kOkBits[] = { 16u, 24u };
	for (const uint16_t bits: kOkBits)
	{
		const std::vector<uint8_t> b = MakeStreamInfo(4096u, 4096u, 44100u, 2u, bits, 442368ull);
		SCOPED_TRACE("位深 " + std::to_string(bits));
		EXPECT_EQ(CLV_AudioParseStreamInfo(b.data(), b.size(), &si), CLV_AUDIO_OK) << "集合内的合法位深被误拒";
	}

	/* 判点二（采样率拒档）：655354 受理、655355 起拒 —— 界值即 kSubsetSampleRateRejectFloor。
		 * 0 另有先命中的「采样率为 0」判，同样 VALUE_RANGE。 */
	const std::vector<uint8_t> r_below = MakeStreamInfo(4096u, 4096u, 655354u, 2u, 16u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(r_below.data(), r_below.size(), &si), CLV_AUDIO_OK) << "拒档下界内侧被误拒";
	const std::vector<uint8_t> r_at =
		MakeStreamInfo(4096u, 4096u, clv::audio::kSubsetSampleRateRejectFloor, 2u, 16u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(r_at.data(), r_at.size(), &si), CLV_AUDIO_VALUE_RANGE)
		<< "采样率等于拒档边界却没拒";
	const std::vector<uint8_t> r_zero = MakeStreamInfo(4096u, 4096u, 0u, 2u, 16u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(r_zero.data(), r_zero.size(), &si), CLV_AUDIO_VALUE_RANGE);

	/* 判点三（块大小下界按位深分档）：16-bit 64、24-bit 72。
		 * 界值本身受理、界值 − 1 拒；同一个 64 在 24-bit 档低于该档下界 ⇒ 拒，
		 * 这一格证明下界确实随位深走，不是全局取 64。 */
	const std::vector<uint8_t> b16_at =
		MakeStreamInfo(clv::audio::kSubsetMinBlocksize16, 4096u, 44100u, 2u, 16u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(b16_at.data(), b16_at.size(), &si), CLV_AUDIO_OK) << "16-bit 下界本身被误拒";
	const std::vector<uint8_t> b16_below =
		MakeStreamInfo(clv::audio::kSubsetMinBlocksize16 - 1u, 4096u, 44100u, 2u, 16u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(b16_below.data(), b16_below.size(), &si), CLV_AUDIO_VALUE_RANGE)
		<< "16-bit 档低于下界的块大小被受理";
	const std::vector<uint8_t> b24_at =
		MakeStreamInfo(clv::audio::kSubsetMinBlocksize24, 4096u, 44100u, 2u, 24u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(b24_at.data(), b24_at.size(), &si), CLV_AUDIO_OK) << "24-bit 下界本身被误拒";
	const std::vector<uint8_t> b24_below =
		MakeStreamInfo(clv::audio::kSubsetMinBlocksize24 - 1u, 4096u, 44100u, 2u, 24u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(b24_below.data(), b24_below.size(), &si), CLV_AUDIO_VALUE_RANGE)
		<< "24-bit 档低于下界的块大小被受理";
	const std::vector<uint8_t> b24_sixtyfour = MakeStreamInfo(64u, 4096u, 44100u, 2u, 24u, 442368ull);
	EXPECT_EQ(CLV_AudioParseStreamInfo(b24_sixtyfour.data(), b24_sixtyfour.size(), &si), CLV_AUDIO_VALUE_RANGE)
		<< "下界没按位深分档：64 在 24-bit 档仍低于 72";

	// 现搭的载荷与读方那一份位域算式对平：声道与位深的「值 − 1」、总样本数 36 位都按原样回来
	ASSERT_EQ(CLV_AudioParseStreamInfo(ok16.data(), ok16.size(), &si), CLV_AUDIO_OK);
	EXPECT_EQ(si.channels, 2u);
	EXPECT_EQ(si.bit_depth, 16u);
	EXPECT_EQ(si.min_blocksize, 4096u);
	EXPECT_EQ(si.max_blocksize, 4096u);
	EXPECT_EQ(si.sample_rate, 44100u);
	EXPECT_EQ(si.total_samples, 442368ull);
}

/* 注释块视图的自洽校验：两个查询口（取第几条 / 按键名查）在入口比三件事——
 * 条目区起点（8 + 厂商串长度）不大于声明块长、声明块长不小于头部必需字段 8、长度加减在 64 位域里做。
 * 视图本身由 `CLV_AudioParseVorbisComment` 产出，下面几档是在产出之后改视图字段，模拟视图与字节不同档。 */
TEST(AudioFraming, CommentViewWithEntryOffsetBeyondBlocksizeIsRefused)
{
	const std::vector<uint8_t> block = BuildCommentBlock(" vendor", { "TITLE=a" });
	CLV_AudioVorbisCommentC view {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(block.data(), block.size(), &view), CLV_AUDIO_OK);
	ASSERT_EQ(view.comment_count, 1u);

	// 判点①的越界侧：厂商串长度取到声明块长 ⇒ 条目区起点 = 8 + 块长，必然越出块界
	view.vendor_length = view.block_size;
	CLV_AudioCommentEntryC e = SentinelEntry();
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_TRUE(EntryUntouched(e)) << "拒档把半份条目写进了出参";
	// 同一个视图走按键查那一口：同判，两口的校验不是各写一份
	e = SentinelEntry();
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "TITLE", 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_TRUE(EntryUntouched(e)) << "拒档把半份条目写进了出参";

	// 判点②：声明块长不足头部必需字段（4 字节厂商串长度 + 4 字节条数 = 8）
	view = CLV_AudioVorbisCommentC {};
	view.block = block.data();
	view.block_size = 7u;
	view.vendor_length = 0u;
	view.comment_count = 1u;
	e = SentinelEntry();
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "TITLE", 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
}

/* 厂商串长度这一格是 32 位无符号：起点用 64 位算式算，才能同时挡下「不回绕的越界」与
 * 「32 位加法回绕成小数」两支。两支各给一档，界内界外都取现算值得到的那两档。 */
TEST(AudioFraming, CommentViewWithExtremeVendorLengthIsRefused)
{
	const std::vector<uint8_t> block = BuildCommentBlock(" vendor", { "TITLE=a" });
	CLV_AudioVorbisCommentC view {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(block.data(), block.size(), &view), CLV_AUDIO_OK);
	const uint32_t block_size = view.block_size;

	/* 不回绕支：vendor_length = 2^32 − 12 ⇒ 起点 = 2^32 − 4，在 32 位域里也放得下，
	 * 靠「起点 ≤ 声明块长」这一判拒。 */
	static_assert(0xFFFFFFF4ull + 8ull == 0xFFFFFFFCull, "这一档的加法不回绕，越界由起点判挡");
	view.vendor_length = 0xFFFFFFF4u;
	CLV_AudioCommentEntryC e = SentinelEntry();
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "TITLE", 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_TRUE(EntryUntouched(e)) << "拒档把半份条目写进了出参";

	/* 回绕支：vendor_length ≥ 2^32 − 8 ⇒ 起点在 32 位域里回绕成 0..7（比声明块长小，起点判会被它骗过去），
	 * 只有把加法放到 64 位域里才仍判越界。两档各取回绕后落在 0 与 7 的那两侧。 */
	static_assert(static_cast<uint32_t>(0xFFFFFFF8u + 8u) == 0u, "这一档回绕成 0");
	static_assert(static_cast<uint32_t>(0xFFFFFFFFu + 8u) == 7u, "这一档回绕成 7");
	for (const uint32_t len: { 0xFFFFFFF8u, 0xFFFFFFFFu })
	{
		SCOPED_TRACE("vendor_length = " + std::to_string(len));
		view.vendor_length = len;
		e = SentinelEntry();
		EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 0u, &e), CLV_AUDIO_METADATA_MALFORMED)
			<< "回绕后的起点被当成了合法起点";
		EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "TITLE", 0u, &e), CLV_AUDIO_METADATA_MALFORMED);
		EXPECT_TRUE(EntryUntouched(e)) << "拒档把半份条目写进了出参";
		EXPECT_EQ(view.block_size, block_size) << "校验不该改写视图";
	}
}

/* 负向不破例：自洽视图上的正常查询不得被上面那三判带偏——查得到出条目、序号越界返回值域、
 * 键不存在返回「键不存在」而不是「结构不成立」。 */
TEST(AudioFraming, CommentViewOnSelfConsistentPageKeepsItsOwnAnswers)
{
	const std::vector<uint8_t> block = BuildCommentBlock(" reference", { "TITLE=one", "ARTIST=two", "TITLE=three" });
	CLV_AudioVorbisCommentC view {};
	ASSERT_EQ(CLV_AudioParseVorbisComment(block.data(), block.size(), &view), CLV_AUDIO_OK);
	EXPECT_EQ(view.vendor_length, 10u);
	ASSERT_EQ(view.comment_count, 3u);

	CLV_AudioCommentEntryC e {};
	ASSERT_EQ(CLV_AudioVorbisCommentEntryAt(&view, 2u, &e), CLV_AUDIO_OK);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(e.key), e.key_length), "TITLE");
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(e.value), e.value_length), "three");

	// 序号越界走值域那一档：自洽校验过了才轮到它
	CLV_AudioCommentEntryC past = SentinelEntry();
	EXPECT_EQ(CLV_AudioVorbisCommentEntryAt(&view, view.comment_count, &past), CLV_AUDIO_VALUE_RANGE);

	// 键比对不区分大小写，命中数不够与键根本没有都是「键不存在」，不是「结构不成立」
	for (const char* key: { "artist", "ARTIST" })
	{
		SCOPED_TRACE(std::string("键 ") + key);
		e = CLV_AudioCommentEntryC {};
		ASSERT_EQ(CLV_AudioVorbisCommentFindKey(&view, key, 0u, &e), CLV_AUDIO_OK);
		EXPECT_EQ(e.value_length, 3u);
		CLV_AudioCommentEntryC second = SentinelEntry();
		EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, key, 1u, &second), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);
		EXPECT_TRUE(EntryUntouched(second)) << "「键不存在」那一档不该写出参";
	}
	CLV_AudioCommentEntryC absent = SentinelEntry();
	EXPECT_EQ(CLV_AudioVorbisCommentFindKey(&view, "NOSUCHKEY", 0u, &absent), CLV_AUDIO_COMMENT_KEY_NOT_FOUND);
	EXPECT_EQ(EntryUntouched(absent), true) << "「键不存在」那一档不该写出参";
}

TEST(AudioFraming, MetaBlockSplitAndClassify)
{
	std::vector<uint8_t> block;
	block.push_back(4u);
	block.insert(block.end(), clvtest::kVorbisMaskBytes, clvtest::kVorbisMaskBytes + clvtest::kVorbisMaskLen);

	uint8_t type = 0u;
	const uint8_t* data = nullptr;
	size_t size = 0u;
	ASSERT_EQ(CLV_AudioSplitMetaBlock(block.data(), block.size(), &type, &data, &size), CLV_AUDIO_OK);
	EXPECT_EQ(type, 4u);
	EXPECT_EQ(size, clvtest::kVorbisMaskLen);
	// 承载块载荷逐字节照搬原块
	std::vector<uint8_t> got(data, data + size);
	std::vector<uint8_t> want(clvtest::kVorbisMaskBytes, clvtest::kVorbisMaskBytes + clvtest::kVorbisMaskLen);
	EXPECT_EQ(got, want);

	CLV_AudioMetaAction act = CLV_AUDIO_META_STORE;
	ASSERT_EQ(CLV_AudioClassifyMetaBlock(0u, &act), CLV_AUDIO_METADATA_BAD);
	EXPECT_EQ(act, CLV_AUDIO_META_REJECT_STREAMINFO);
	EXPECT_EQ(CLV_AudioClassifyMetaBlock(1u, &act), CLV_AUDIO_OK);
	EXPECT_EQ(act, CLV_AUDIO_META_DROP_PADDING);
	for (uint8_t t = 2u; t <= 6u; ++t)
	{
		ASSERT_EQ(CLV_AudioClassifyMetaBlock(t, &act), CLV_AUDIO_OK) << "type " << t;
		EXPECT_EQ(act, CLV_AUDIO_META_STORE);
	}
	const uint8_t kUnknownTypes[] = { 7u, 8u, 126u };
	for (const uint8_t t: kUnknownTypes)
	{
		ASSERT_EQ(CLV_AudioClassifyMetaBlock(t, &act), CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED) << "type " << t;
		EXPECT_EQ(act, CLV_AUDIO_META_SKIP_UNKNOWN);
	}
	// 承载块至少要有一个字节的首字节位
	uint8_t t2 = 0u;
	const uint8_t* d2 = nullptr;
	size_t n2 = 0u;
	EXPECT_EQ(CLV_AudioSplitMetaBlock(block.data(), 0u, &t2, &d2, &n2), CLV_AUDIO_INVALID_ARGUMENT);
}

TEST(AudioFraming, ChannelMaskEntryIsStrippedAndOthersKept)
{
	CLV_AudioBlob* blob = nullptr;
	uint32_t removed = 0u;
	const CLV_AudioError e =
		CLV_AudioStripChannelMask(clvtest::kVorbisMaskBytes, clvtest::kVorbisMaskLen, &blob, &removed);
	ASSERT_EQ(e, CLV_AUDIO_OK) << CLV_AudioStrError(e);
	ASSERT_NE(blob, nullptr);

	const uint8_t* out = nullptr;
	size_t out_size = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &out, &out_size), CLV_AUDIO_OK);
	ASSERT_GT(out_size, 8u);
	ASSERT_LT(out_size, clvtest::kVorbisMaskLen);
	EXPECT_EQ(removed, clvtest::kVorbisMaskEntries);

	const auto before = walk_comments(clvtest::kVorbisMaskBytes, clvtest::kVorbisMaskLen);
	const auto after = walk_comments(out, out_size);
	/* 真素材的条目区只有掩码键那一条（kVorbisTotalEntries = 1），剥完就是空条目区，
	 * 「其余条目逐字节保留」在这一路上没有可遍历的对象。两件事分开钉：真素材钉
	 * 「剥净 + 厂商串逐字节不动 + 条数字段归零」，保留语义交给下面的多条目合成料（逐条对字节与顺序）。 */
	ASSERT_EQ(before.size(), static_cast<size_t>(clvtest::kVorbisTotalEntries)) << "素材条目数变了，本例前提要重看";
	EXPECT_TRUE(after.empty()) << "掩码键之外本不该有条目";
	for (const auto& item: after) EXPECT_FALSE(key_eq(item.first, kMaskKey)) << "掩码键没被剥掉: " << item.first;

	// 厂商串整段逐字节不动（剥的是条目区，头部跟着变的只有条数字段）
	const size_t vendor = LE32At(out);
	ASSERT_EQ(vendor, static_cast<size_t>(LE32At(clvtest::kVorbisMaskBytes)));
	ASSERT_LT(4u + vendor, out_size);
	for (size_t i = 0u; i < vendor; ++i)
		EXPECT_EQ(out[4u + i], clvtest::kVorbisMaskBytes[4u + i]) << "厂商串第 " << i << " 字节被改了";
	EXPECT_EQ(LE32At(out + 4u + vendor), static_cast<uint32_t>(after.size())) << "条数字段没跟着改";

	CLV_AudioBlobRelease(blob);

	/* 保留语义：三条非掩码 + 两条掩码（大写、小写各一）交错摆放，剥后必须逐字节、逐序对平。
	 * 只比集合不比顺序的话，「顺序不变」这半句仍然没人钉。 */
	const std::vector<std::string> keep = { "TITLE=first", "ARTIST=second", "COMMENT=third" };
	const std::vector<std::string> items = { keep[0], "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x003F", keep[1],
											 "waveformatextensible_channel_mask=0x0007", keep[2] };
	const std::string vendor2 = " vendor text stays as is";
	const std::vector<uint8_t> mix = BuildCommentBlock(vendor2, items);
	CLV_AudioBlob* blob2 = nullptr;
	uint32_t removed2 = 0u;
	const CLV_AudioError e2 = CLV_AudioStripChannelMask(mix.data(), mix.size(), &blob2, &removed2);
	ASSERT_EQ(e2, CLV_AUDIO_OK) << CLV_AudioStrError(e2);
	ASSERT_NE(blob2, nullptr);
	EXPECT_EQ(removed2, 2u) << "两条掩码键（大写 + 小写）都该计入剥离数";
	const uint8_t* out2 = nullptr;
	size_t n2 = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob2, &out2, &n2), CLV_AUDIO_OK);
	const auto kept = walk_comments(out2, n2);
	ASSERT_EQ(kept.size(), keep.size());
	for (size_t i = 0u; i < keep.size(); ++i)
	{
		EXPECT_EQ(kept[i].second, keep[i]) << "第 " << i << " 条保留项不符（顺序或字节）";
		EXPECT_EQ(kept[i].first, keep[i].substr(0, keep[i].find('='))) << "第 " << i << " 条的键名被改写";
	}
	const size_t v2 = LE32At(out2);
	ASSERT_EQ(v2, vendor2.size());
	for (size_t i = 0u; i < v2; ++i) EXPECT_EQ(out2[4u + i], static_cast<uint8_t>(vendor2[i]));
	EXPECT_EQ(LE32At(out2 + 4u + v2), static_cast<uint32_t>(keep.size())) << "条数字段没跟着改";
	size_t want = 4u + vendor2.size() + 4u;
	for (const std::string& s: keep) want += 4u + s.size();
	EXPECT_EQ(n2, want) << "整块长度 = 头部 + 厂商串 + 保留条目各自的长度字段与字节";
	CLV_AudioBlobRelease(blob2);
}

TEST(AudioFraming, LowercaseMaskKeyAlsoStripped)
{
	const std::string vendor = " reference";
	const std::string keep = "TITLE=keep me";
	const std::string mask = "waveformatextensible_channel_mask=0x003F";
	const std::vector<uint8_t> block = BuildCommentBlock(vendor, { keep, mask });

	CLV_AudioBlob* blob = nullptr;
	uint32_t removed = 0u;
	ASSERT_EQ(CLV_AudioStripChannelMask(block.data(), block.size(), &blob, &removed), CLV_AUDIO_OK);
	ASSERT_NE(blob, nullptr);
	EXPECT_EQ(removed, 1u);
	const uint8_t* out = nullptr;
	size_t n = 0u;
	ASSERT_EQ(CLV_AudioBlobGetBytes(blob, &out, &n), CLV_AUDIO_OK);
	const auto after = walk_comments(out, n);
	ASSERT_EQ(after.size(), 1u);
	EXPECT_EQ(after[0].second, keep);
	CLV_AudioBlobRelease(blob);
}

TEST(AudioFraming, MalformedVorbisCommentRejected)
{
	std::vector<uint8_t> block(clvtest::kVorbisMaskBytes, clvtest::kVorbisMaskBytes + clvtest::kVorbisMaskLen);
	// 把第一条注释的长度改成超出块尾
	const size_t vendor = block[0] | (block[1] << 8) | (block[2] << 16) | (static_cast<uint32_t>(block[3]) << 24);
	size_t pos = 4u + vendor + 4u;
	block[pos] = static_cast<uint8_t>(0xFFu);
	block[pos + 1] = static_cast<uint8_t>(0xFFu);
	CLV_AudioBlob* blob = nullptr;
	uint32_t removed = 0u;
	EXPECT_EQ(CLV_AudioStripChannelMask(block.data(), block.size(), &blob, &removed), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_EQ(blob, nullptr);
	// 短于最小结构
	EXPECT_EQ(CLV_AudioStripChannelMask(clvtest::kVorbisMaskBytes, 4u, &blob, &removed), CLV_AUDIO_METADATA_MALFORMED);
	EXPECT_EQ(CLV_AudioStripChannelMask(nullptr, 84u, &blob, &removed), CLV_AUDIO_INVALID_ARGUMENT);
}

TEST(AudioFraming, ErrorCodesStayInsideAudioSegment)
{
	EXPECT_EQ(static_cast<int>(CLV_AUDIO_OK), 0);
	EXPECT_EQ(static_cast<int>(CLV_AUDIO_INVALID_HANDLE), 32);
	EXPECT_EQ(static_cast<int>(CLV_AUDIO_STREAM_REJECTED), 47);
	// 段尾追加的两个解读面码值钉死，未分配区的起点随之下移
	EXPECT_EQ(static_cast<int>(CLV_AUDIO_METADATA_MALFORMED), 48);
	EXPECT_EQ(static_cast<int>(CLV_AUDIO_COMMENT_KEY_NOT_FOUND), 49);
	/* 段内 32..49 逐个要有名字，但 41 与 45 是退场留下的空位（号位留着不复用），
	 * 这两格与 1..31、50..63 同判 unknown——「空位没人占」这句话只有这样才能守住。 */
	for (int v = 32; v <= 49; ++v)
	{
		const char* s = CLV_AudioStrError(static_cast<CLV_AudioError>(v));
		ASSERT_NE(s, nullptr) << "code " << v;
		if (v == 41 || v == 45) EXPECT_STREQ(s, "unknown") << "空位 " << v << " 不该有名字";
		else EXPECT_STRNE(s, "unknown") << "code " << v;
	}
	for (int v = 1; v <= 31; ++v) EXPECT_STREQ(CLV_AudioStrError(static_cast<CLV_AudioError>(v)), "unknown");
	for (int v = 50; v <= 63; ++v) EXPECT_STREQ(CLV_AudioStrError(static_cast<CLV_AudioError>(v)), "unknown");
	// 段外取值同样落 unknown，不会被误当成容器面或文件面的码
	EXPECT_STREQ(CLV_AudioStrError(static_cast<CLV_AudioError>(999)), "unknown");
}

TEST(AudioFraming, StrErrorNamesEveryDefinedCode)
{
	static const CLV_AudioError kAll[] = {
		CLV_AUDIO_OK,
		CLV_AUDIO_INVALID_HANDLE,
		CLV_AUDIO_INVALID_ARGUMENT,
		CLV_AUDIO_VALUE_RANGE,
		CLV_AUDIO_FRAME_HEADER_BAD,
		CLV_AUDIO_FIELD_MISMATCH,
		CLV_AUDIO_BLOCKSIZE_OUT_OF_RANGE,
		CLV_AUDIO_STREAMINFO_ABSENT,
		CLV_AUDIO_METADATA_BAD,
		CLV_AUDIO_BLOCK_TYPE_UNSUPPORTED,
		CLV_AUDIO_TOO_LARGE,
		CLV_AUDIO_STATE_ERROR,
		CLV_AUDIO_OUT_OF_MEMORY,
		CLV_AUDIO_BACKEND_FAILURE,
		CLV_AUDIO_STREAM_REJECTED,
		CLV_AUDIO_METADATA_MALFORMED,
		CLV_AUDIO_COMMENT_KEY_NOT_FOUND,
	};
	for (const CLV_AudioError e: kAll)
	{
		const char* s = CLV_AudioStrError(e);
		ASSERT_NE(s, nullptr);
		EXPECT_STRNE(s, "unknown") << "code " << static_cast<int>(e);
		for (const char* p = s; *p != '\0'; ++p)
		{
			const unsigned char c = static_cast<unsigned char>(*p);
			EXPECT_GE(c, 0x20u) << "必须是可打印 ASCII";
			EXPECT_LT(c, 0x7Fu) << "必须是可打印 ASCII";
		}
	}
}

/* 帧头扩展 UTF-8 编码号的容量档。每档可载位宽 = (7 − lead) + 6 × (lead − 1)，逐档上界
 * 0x7F / 0x7FF / 0xFFFF / 0x1FFFFF / …（算式与逐档读数见 src/audio/frame.cpp 的 ExtUtf8Cap 那组 static_assert）。
 * 两侧都钉：界内值必须受理、落得进更短一档的非最短形式必须拒 —— 上界少算一位会误拒合法帧头，
 * 下界不判会照收多装字节的帧头，两档各红一次才分得开。 */
TEST(AudioFraming, CodedFrameNumberAcceptsEveryLegalBand)
{
	struct Probe
	{
		uint64_t number;
		unsigned lead;
	};

	const Probe kProbes[] = {
		{ 128u,		2u },
		 { 1023u,	 2u },
		   { 1024u,	   2u },
		 { 2047u,	 2u },
		   { 2048u,	   3u },
		{ 32767u,	3u },
		   { 32768u,   3u },
		  { 65535u,	  3u },
		 { 65536u,	 4u },
		{ 2097151u, 4u },
	};
	for (const Probe& p: kProbes)
	{
		SCOPED_TRACE("帧号 " + std::to_string(p.number) + " 写成 " + std::to_string(p.lead) + " 字节");
		const std::vector<uint8_t> head = FrameHeadWithNumber(p.number, p.lead);
		CLV_AudioFrameHeadC h {};
		const CLV_AudioError e = CLV_AudioInspectFrame(head.data(), head.size(), &h);
		ASSERT_EQ(e, CLV_AUDIO_OK) << "合法帧号被判帧头不成立：" << CLV_AudioStrError(e);
		EXPECT_EQ(h.frame_or_sample_number, p.number) << "解回的帧号与写入值不符";
	}
}

/* 最短形式：值落得进更短一档，就不许占着更长的字节数 ——「5 写成两字节」这类多装字节的帧头
 * 由下界那一判挡掉，与上一条用例的上界那一判各挡一侧。 */
TEST(AudioFraming, CodedFrameNumberMustUseShortestForm)
{
	const uint64_t kNonMinimal[][2] = {
		{ 5u,	   2u },
		   { 127u,	  2u },
		{ 128u,	   3u },
		 { 0x7FFu,	3u },
		{ 0x800u,  4u },
		   { 0xFFFFu, 4u },
	};
	for (const auto& row: kNonMinimal)
	{
		const uint64_t number = row[0];
		const unsigned lead = static_cast<unsigned>(row[1]);
		SCOPED_TRACE("帧号 " + std::to_string(number) + " 写成 " + std::to_string(lead) + " 字节（多装）");
		const std::vector<uint8_t> head = FrameHeadWithNumber(number, lead);
		CLV_AudioFrameHeadC h {};
		EXPECT_EQ(CLV_AudioInspectFrame(head.data(), head.size(), &h), CLV_AUDIO_FRAME_HEADER_BAD)
			<< "非最短形式的编码号被判成立";
	}
}
