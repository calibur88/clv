/* audio_fixtures_p.h
 *
 * 由 scripts/gen_audio_fixtures.py 生成，请勿手改，也不要手填里面的数字。
 * 帧字节按帧边界从真实 FLAC 件里切出；期望值由另一份独立实现的位域解析件解出，
 * 并与该流的流信息交叉核对。
 * 重跑：`python scripts/gen_audio_fixtures.py`（参考件与素材目录由命令行参数指定，不在仓库内），
 * 再跑 `scripts/format_all.py --write`。
 *
 * 下面每行的 sha256 只登前 16 位十六进制（= 64 位），且参考件不在仓内盘上：
 * 这两件事叠加使该指纹绑定在本仓不可复核 —— 它能用来辨认「换了一份素材」，
 * 不能用来证明「就是这一份」。逐字节的对账由本文件里的帧字节与用例算式承担。
 *
 *   kFrame16Fixed: at1_默认.flac（611815 B，sha256 82df8bdb5fc190ce）—— 固定块流 16bit / 2ch / 44100，整首帧；首帧 @8322，帧长 14 B，块大小码 12 → 4096
 *   kFrame24Fixed: at1_24bit_源.flac（612202 B，sha256 a90f3cfdf9602023）—— 位深码 0b110 = 24 bit；首帧 @8366，帧长 16 B，块大小码 12 → 4096
 *   kFrameVarBlock: variable.flac（618335 B，sha256 95c7dd414382b8d6）—— 变长块流，块策略位为 1；首帧 @8322，帧长 14 B，块大小码 11 → 2048
 *   kFrameBig32768: at1_lax_b_32768.flac（622143 B，sha256 b5f3b8bf165ee7fa）—— 块大小码 0b1111 = 32768，越流式子集；首帧 @8322，帧长 14 B，块大小码 15 → 32768
 *   kFrame6Ch: at7_6ch_ext.flac（28428 B，sha256 c1c441216d223527）—— 6 声道，注释块里带扩展声道掩码；首帧 @8348，帧长 26 B，块大小码 12 → 4096
 *   kStreamInfo：at1_默认.flac 的流信息块载荷（34 B），min_blk=4096 max_blk=4096 rate=44100 ch=2 bits=16 total=442368
 *   kVorbisMask：at7_6ch_ext.flac 的注释块载荷（84 B），含掩码键 1 条、共 1 条；整文件 sha256 c1c441216d223527
 */
#ifndef CLV_TEST_AUDIO_FIXTURES_P_H
#define CLV_TEST_AUDIO_FIXTURES_P_H

namespace clvtest
{

	/* kFrame16Fixed：整帧（含帧头与帧尾 2 字节 CRC-16） */
	inline constexpr unsigned char kFrame16FixedBytes[] = {
		0xFFu, 0xF8u, 0xC9u, 0x18u, 0x00u, 0xC2u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xB8u, 0xEEu,
	};
	inline constexpr unsigned kFrame16FixedLen = 14u;
	inline constexpr unsigned kFrame16FixedHdr = 6u;
	inline constexpr unsigned kFrame16FixedBlocksize = 4096u;
	inline constexpr unsigned kFrame16FixedRate = 44100u;
	inline constexpr unsigned kFrame16FixedChannels = 2u;
	inline constexpr unsigned kFrame16FixedDepth = 16u;
	inline constexpr unsigned long long kFrame16FixedNumber = 0ull;
	inline constexpr unsigned char kFrame16FixedBlocking = 0u;

	/* kFrame24Fixed：整帧（含帧头与帧尾 2 字节 CRC-16） */
	inline constexpr unsigned char kFrame24FixedBytes[] = {
		0xFFu, 0xF8u, 0xC9u, 0x1Cu, 0x00u, 0x96u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xA0u, 0xEBu,
	};
	inline constexpr unsigned kFrame24FixedLen = 16u;
	inline constexpr unsigned kFrame24FixedHdr = 6u;
	inline constexpr unsigned kFrame24FixedBlocksize = 4096u;
	inline constexpr unsigned kFrame24FixedRate = 44100u;
	inline constexpr unsigned kFrame24FixedChannels = 2u;
	inline constexpr unsigned kFrame24FixedDepth = 24u;
	inline constexpr unsigned long long kFrame24FixedNumber = 0ull;
	inline constexpr unsigned char kFrame24FixedBlocking = 0u;

	/* kFrameVarBlock：整帧（含帧头与帧尾 2 字节 CRC-16） */
	inline constexpr unsigned char kFrameVarBlockBytes[] = {
		0xFFu, 0xF9u, 0xB9u, 0x18u, 0x00u, 0xB3u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xDDu, 0x12u,
	};
	inline constexpr unsigned kFrameVarBlockLen = 14u;
	inline constexpr unsigned kFrameVarBlockHdr = 6u;
	inline constexpr unsigned kFrameVarBlockBlocksize = 2048u;
	inline constexpr unsigned kFrameVarBlockRate = 44100u;
	inline constexpr unsigned kFrameVarBlockChannels = 2u;
	inline constexpr unsigned kFrameVarBlockDepth = 16u;
	inline constexpr unsigned long long kFrameVarBlockNumber = 0ull;
	inline constexpr unsigned char kFrameVarBlockBlocking = 1u;

	/* kFrameBig32768：整帧（含帧头与帧尾 2 字节 CRC-16） */
	inline constexpr unsigned char kFrameBig32768Bytes[] = {
		0xFFu, 0xF8u, 0xF9u, 0x18u, 0x00u, 0x23u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x54u, 0x02u,
	};
	inline constexpr unsigned kFrameBig32768Len = 14u;
	inline constexpr unsigned kFrameBig32768Hdr = 6u;
	inline constexpr unsigned kFrameBig32768Blocksize = 32768u;
	inline constexpr unsigned kFrameBig32768Rate = 44100u;
	inline constexpr unsigned kFrameBig32768Channels = 2u;
	inline constexpr unsigned kFrameBig32768Depth = 16u;
	inline constexpr unsigned long long kFrameBig32768Number = 0ull;
	inline constexpr unsigned char kFrameBig32768Blocking = 0u;

	/* kFrame6Ch：整帧（含帧头与帧尾 2 字节 CRC-16） */
	inline constexpr unsigned char kFrame6ChBytes[] = {
		0xFFu, 0xF8u, 0xC9u, 0x58u, 0x00u, 0x99u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
		0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x3Bu, 0x0Eu,
	};
	inline constexpr unsigned kFrame6ChLen = 26u;
	inline constexpr unsigned kFrame6ChHdr = 6u;
	inline constexpr unsigned kFrame6ChBlocksize = 4096u;
	inline constexpr unsigned kFrame6ChRate = 44100u;
	inline constexpr unsigned kFrame6ChChannels = 6u;
	inline constexpr unsigned kFrame6ChDepth = 16u;
	inline constexpr unsigned long long kFrame6ChNumber = 0ull;
	inline constexpr unsigned char kFrame6ChBlocking = 0u;

	/* 流信息块载荷 34 字节 */
	inline constexpr unsigned char kStreamInfoBytes[] = {
		0x10u, 0x00u, 0x10u, 0x00u, 0x00u, 0x00u, 0x0Eu, 0x00u, 0x29u, 0x6Bu, 0x0Au, 0xC4u,
		0x42u, 0xF0u, 0x00u, 0x06u, 0xC0u, 0x00u, 0x56u, 0x07u, 0x94u, 0x72u, 0x93u, 0x14u,
		0x45u, 0x5Cu, 0xAEu, 0xDFu, 0xB1u, 0xFDu, 0x35u, 0x85u, 0xFBu, 0x6Eu,
	};
	inline constexpr unsigned kStreamInfoLen = 34u;
	inline constexpr unsigned kStreamInfoMinBlock = 4096u;
	inline constexpr unsigned kStreamInfoMaxBlock = 4096u;
	inline constexpr unsigned kStreamInfoMinFrame = 14u;
	inline constexpr unsigned kStreamInfoMaxFrame = 10603u;
	inline constexpr unsigned kStreamInfoRate = 44100u;
	inline constexpr unsigned kStreamInfoChannels = 2u;
	inline constexpr unsigned kStreamInfoDepth = 16u;
	inline constexpr unsigned long long kStreamInfoTotalSamples = 442368ull;

	/* 带扩展声道掩码的 Vorbis 注释块载荷 */
	inline constexpr unsigned char kVorbisMaskBytes[] = {
		0x20u, 0x00u, 0x00u, 0x00u, 0x72u, 0x65u, 0x66u, 0x65u, 0x72u, 0x65u, 0x6Eu, 0x63u, 0x65u, 0x20u,
		0x6Cu, 0x69u, 0x62u, 0x46u, 0x4Cu, 0x41u, 0x43u, 0x20u, 0x31u, 0x2Eu, 0x35u, 0x2Eu, 0x30u, 0x20u,
		0x32u, 0x30u, 0x32u, 0x35u, 0x30u, 0x32u, 0x31u, 0x31u, 0x01u, 0x00u, 0x00u, 0x00u, 0x28u, 0x00u,
		0x00u, 0x00u, 0x57u, 0x41u, 0x56u, 0x45u, 0x46u, 0x4Fu, 0x52u, 0x4Du, 0x41u, 0x54u, 0x45u, 0x58u,
		0x54u, 0x45u, 0x4Eu, 0x53u, 0x49u, 0x42u, 0x4Cu, 0x45u, 0x5Fu, 0x43u, 0x48u, 0x41u, 0x4Eu, 0x4Eu,
		0x45u, 0x4Cu, 0x5Fu, 0x4Du, 0x41u, 0x53u, 0x4Bu, 0x3Du, 0x30u, 0x78u, 0x30u, 0x30u, 0x33u, 0x46u,
	};
	inline constexpr unsigned kVorbisMaskLen = 84u;
	inline constexpr unsigned kVorbisMaskEntries = 1u;
	inline constexpr unsigned kVorbisTotalEntries = 1u;

}	 // namespace clvtest

#endif	  // CLV_TEST_AUDIO_FIXTURES_P_H
