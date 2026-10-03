/* writer.h
 *
 * 容器写方：结构布局（头 / 描述符表 / 扩展块链 / 包区 / 索引区 / 尾）、帧分片、索引条目、末尾回填。
 *
 * 时间轴：调用方给该流原始时间轴上的绝对 dts / pts（B 帧前瞻可以是负的），
 * 写方把整条流平移使首包 dts = 0，读方不承担平移。
 * pts_delta 恒等于「本帧 PTS - 本帧 DTS」，平移量在相减时抵消，故直接取原始 pts - dts。
 */
#ifndef CLV_CONTAINER_WRITER_H
#define CLV_CONTAINER_WRITER_H

#include "io.h"
#include "packet.h"
#include "status.h"
#include "structure.h"

#include <cstdint>
#include <vector>

namespace clv
{
	namespace container
	{

		struct WriterConfig
		{
			uint16_t version_minor = kVersionMinorV1;
			bool index_present = true;
			bool globally_sorted = false;
			// 流式：文件头 total_packets 写未知值，真实值只落文件尾
			bool streaming = false;
			// 0 = 整帧一包；否则按该字节数切分片，且不得大于该流的 max_packet_size
			uint32_t fragment_chunk_size = 0;
		};

		struct FrameInput
		{
			uint8_t stream_id = 0;
			bool is_keyframe = false;
			int64_t dts = 0;
			int64_t pts = 0;
			const uint8_t* payload = nullptr;
			size_t payload_size = 0;
		};

		struct WriteSummary
		{
			uint64_t packets = 0;
			uint64_t frames = 0;
			uint64_t index_entries = 0;
			uint64_t duration_ticks = 0;
			uint64_t file_size = 0;
			uint64_t packet_area_start = 0;
			uint64_t index_area_start = 0;
		};

		class ContainerWriter
		{
			public:

			ContainerWriter(ByteSinkIf& sink, const WriterConfig& cfg) noexcept;

			// 出第一帧之前调用
			ContainerErr AddStream(const StreamDesc& d);
			ContainerErr AddExtBlock(uint8_t stream_id, uint8_t ext_type, uint8_t ext_version, const uint8_t* data,
									 size_t n);

			ContainerErr WriteFrame(const FrameInput& in);
			ContainerErr Finish(WriteSummary* out);

			// 该流被平移掉的原始起点（首帧 dts）；没出过帧返回 0
			int64_t StreamShift(uint8_t stream_id) const noexcept;

			private:

			enum class Phase : unsigned char
			{
				kStreams,
				kPackets,
				kDone
			};

			struct ExtItem
			{
				uint8_t stream_id = 0;
				uint8_t type = 0;
				uint8_t version = 0;
				std::vector<uint8_t> data;
			};

			struct StreamState
			{
				StreamDesc desc;
				bool has_origin = false;
				int64_t origin = 0;
				int64_t last_dts = 0;
				int64_t last_pts = 0;
			};

			ContainerErr BeginPackets();
			ContainerErr PatchHead(uint64_t index_offset, uint64_t total_packets, uint64_t duration_ticks,
								   uint8_t flags);
			StreamState* Find(uint8_t stream_id) noexcept;
			const StreamState* Find(uint8_t stream_id) const noexcept;

			ByteSinkIf* sink_;
			WriterConfig config_;
			Phase phase_ = Phase::kStreams;
			std::vector<StreamState> streams_;
			// 下标 = stream_id，值 = streams_ 下标，-1 表示未登记；push_back 不改变已有下标，故各阶段都可用
			std::vector<int> by_id_;
			std::vector<ExtItem> ext_items_;
			std::vector<IndexEntry> index_;
			uint64_t packets_ = 0;
			uint64_t frames_ = 0;
			uint64_t packet_area_start_ = 0;
			uint64_t index_area_start_ = 0;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_WRITER_H
