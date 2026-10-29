/* 字幕包载荷外层：定长头 + 可选位置 + 可选对齐 + 文本，判序先中先判、一次只出一个码。 */
#ifndef CLV_SUBTITLE_PACKET_H
#define CLV_SUBTITLE_PACKET_H

#include "status.h"

#include <stddef.h>
#include <stdint.h>

namespace clv
{
	namespace subtitle
	{
		inline constexpr uint32_t kPacketHeaderFixedLen = 21u;
		inline constexpr uint32_t kMaxPacketSize = 4096u;

		struct PacketHeader
		{
			int64_t start_ticks; /* 按容器 pts 平移后 */
			int64_t end_ticks;
			int64_t delta_ticks; /* container_pts − 线上 start_time */
			uint32_t text_len;
			uint32_t header_len;
			uint8_t flags;
			uint8_t has_position;
			uint8_t has_alignment;
			uint8_t alignment;
			int32_t position_x;
			int32_t position_y;
		};

		/* 解析一包；`max_packet_bytes` 是上限（0 表示取 4096）。 */
		Err ParsePacketHeader(const uint8_t* payload, size_t size, int64_t container_pts, uint32_t max_packet_bytes,
							  PacketHeader* out);

		/* 反向写出（造测试件用）；容量不足返 kOutputTooSmall 并把所需长度写进 out_size。 */
		Err EncodePacket(const PacketHeader* header, const uint8_t* text, size_t text_len, uint8_t* out,
						 size_t capacity, size_t* out_size);
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_PACKET_H
