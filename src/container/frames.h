/* frames.h
 *
 * 片序连续性与帧重组。一条流一个实例，由读方按 stream_id 持有。
 *
 * 分片按 fragment_index 落位，不按到达顺序拼（同帧两片互换要能复原）。
 * 落位表只按实际到达的片增长：序号 2^40 的单个片只占一个表项加它自己的载荷，不生成 2^40 个槽。
 */
#ifndef CLV_CONTAINER_FRAMES_H
#define CLV_CONTAINER_FRAMES_H

#include "packet.h"

#include <cstdint>
#include <vector>

namespace clv
{
	namespace container
	{

		struct ReassembledFrame
		{
			uint8_t stream_id = 0;
			bool is_keyframe = false;
			uint64_t dts_delta = 0;
			int64_t pts_delta = 0;
			std::vector<uint8_t> payload;
			uint64_t fragments_received = 0;
			// 0 = 未收到 is_last_fragment（片数未知）；否则 = 末片 fragment_index + 1
			uint64_t fragments_expected = 0;
			// false = 有片缺失：末片比对不符，或没等到末片就开了下一帧
			bool complete = false;
		};

		struct FrameStats
		{
			uint64_t frames_emitted = 0;
			uint64_t frames_incomplete = 0;
			uint64_t fragment_gaps = 0;			   // 片序跳变与末片总数不符
			uint64_t duplicate_fragments = 0;	   // 同帧重复序号
			uint64_t missing_first_packets = 0;	   // 帧首包缺失，残帧丢弃
		};

		class FrameAssembler
		{
			public:

			explicit FrameAssembler(uint8_t stream_id) noexcept: stream_id_(stream_id) {}

			/* 喂入一个已解析的包；把本次产出的帧追加到 emitted（0 / 1 / 2 个）。
			 * 返回值就是追加的帧数。 */
			size_t Push(const ParsedPacket& p, std::vector<ReassembledFrame>& emitted);

			// 流结束或重同步失败时调用：还在攒的残帧按 incomplete 出帧，不静默丢内容
			size_t Flush(std::vector<ReassembledFrame>& emitted);

			const FrameStats& Stats() const noexcept { return stats_; }

			/* 一次喂包引起的计数增量，供上层累加到全局统计：
			 * Delta(喂包前的 Stats(), 喂包后的 Stats())。 */
			static FrameStats Delta(const FrameStats& before, const FrameStats& after) noexcept;

			private:

			struct Fragment
			{
				uint64_t index = 0;
				std::vector<uint8_t> payload;
			};

			// 开新帧前收掉上一帧
			void CloseOpenFrame(bool got_last, uint64_t expected_total, std::vector<ReassembledFrame>& out);

			uint8_t stream_id_ = 0;
			FrameStats stats_;
			bool open_ = false;
			bool is_keyframe_ = false;
			uint64_t dts_ = 0;
			int64_t pts_ = 0;
			uint64_t next_expected_ = 0;
			// 按 index 升序维护（插入即定位），所以重复检测与落位都是二分，收口时不必再排序
			std::vector<Fragment> fragments_;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_FRAMES_H
