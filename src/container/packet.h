/* packet.h
 *
 * 数据包编解码。分片帧的每片都自带完整外层头，字段序与判据：
 *
 *   total_size(LE32) | stream_id | flags | [fragment_index] | dts_delta | [pts_delta]
 *   | payload_size | ext_len | payload | ext_data | crc32
 *
 * total_size 自 stream_id 起算、含 crc32、不含自身；线上包占 4 + total_size。
 * crc32 覆盖包内相对区间 [4, total_size)，即 stream_id 起、到 crc32 前。
 * fragment_index 仅 is_fragment = 1 的包携带；帧首包 = 未分片包或 fragment_index = 0 的包。
 * pts_delta 仅帧首包携带，后续片不带；音频与字幕的 pts_delta 恒 0 但照样写出来。
 * flags bit3 has_pts 各片一律置 1（冗余位），bit4 has_fec 与 bit5-7 必须为 0。
 */
#ifndef CLV_CONTAINER_PACKET_H
#define CLV_CONTAINER_PACKET_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace container
	{

		enum class PacketFlagBit : uint8_t
		{
			kKeyframe = 1u << 0,
			kFragment = 1u << 1,
			kLastFragment = 1u << 2,
			kHasPts = 1u << 3,
			kHasFec = 1u << 4
		};

		// 读方处置分档。只有 kHeadInvalid 与「跳不动」的 kTooLarge 进重同步；
		// kCrcFail / kFieldFail / 可跳的 kTooLarge 一律丢包 + warn + 按 total_size 跳，不触发重同步
		enum class PacketParse : uint8_t
		{
			kOk = 0,
			kTruncated,		 // 该位置放不下整包：由上层判是文件尾还是读失败
			kHeadInvalid,	 // total_size < 9 或读不到边界，或 flags 指纹不符 → 包头不成立
			kTooLarge,		 // 包合法但超出运行时接受上限，能跳就跳，跳不动才重同步
			kCrcFail,		 // CRC 不过 → 丢本包 + warn + 跳 total_size
			kFieldFail,		 // 字段区解析失败 / 非最短形式 / payload_size 仲裁式不符 → 同上
		};

		// 写方输入
		struct PacketFields
		{
			uint8_t stream_id = 0;
			bool is_keyframe = false;
			bool is_fragment = false;
			bool is_last_fragment = false;
			uint64_t fragment_index = 0;
			uint64_t dts_delta = 0;
			int64_t pts_delta = 0;
			std::vector<uint8_t> ext;
		};

		// 读方输出
		struct ParsedPacket
		{
			uint32_t total_size = 0;
			uint8_t stream_id = 0;
			uint8_t flags = 0;
			bool is_keyframe = false;
			bool is_fragment = false;
			bool is_last_fragment = false;
			uint64_t fragment_index = 0;
			uint64_t dts_delta = 0;
			int64_t pts_delta = 0;
			uint64_t payload_size = 0;
			uint64_t ext_len = 0;
			std::vector<uint8_t> payload;
			std::vector<uint8_t> ext;

			bool IsFrameFirst() const noexcept { return ! is_fragment || fragment_index == 0; }

			// 线上占用：total_size 不含自身
			size_t WireSize() const noexcept { return static_cast<size_t>(total_size) + 4u; }
		};

		/* 编码整包追加到 out（out 不清空，便于连续写多包）。
	 * 返回线上字节数（4 + total_size）；返回 0 表示入参不成立：
	 * payload 与 payload_size 不匹配、payload_size 超 max_payload（写方上限，不含外层）、
	 * 或分片标志与 fragment_index 组合不成立（未分片包不得声称 is_last_fragment）。 */
		size_t EncodePacket(const PacketFields& f, const uint8_t* payload, size_t payload_size, uint64_t max_payload,
							std::vector<uint8_t>& out);

		/* 严格解析一个包。buf 指向 total_size 的首字节，avail 是该位置起可读的字节数，
	 * outer_limit 是读方运行时的接受上限（含外层，按该流 max_packet_size + 最坏外层开销算）。 */
		PacketParse DecodePacket(const uint8_t* buf, size_t avail, uint64_t outer_limit, ParsedPacket* out);

		// flags 结构指纹：bit3 必须 1，bit4 与 bit5-7 必须 0
		bool FlagsFingerprintOk(uint8_t flags) noexcept;

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_PACKET_H
