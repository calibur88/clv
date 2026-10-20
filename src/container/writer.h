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

		/* 单帧载荷上界 = 2^30 = 1 073 741 824 B。派生：宏块数上界 = 补后像素上界 2^28 ÷ 256 = 1 048 576，
		 * 单宏块线上最坏 = 六块定长系数载体 6 × 120 B（最宽一档 15 位 × 64 系数 ÷ 8）加宏块头、
		 * 运动分量与 block_size varint 的余量，取整到 1024 B；`1 048 576 × 1024 = 2^30`。
		 * 这是一帧（含其全部分片）的字节上限，不是单包上限 —— 单包上界见 reader.h 的 kMaxPacketPayload。 */
		constexpr uint64_t kMaxFramePayload = 1ull << 30;
		static_assert(kMaxFramePayload == 1073741824ull, "单帧载荷上界 = 2^30");

		struct WriterConfig
		{
			uint16_t version_minor = kVersionMinorV2;
			// 分段身份：置 segmented 后写方才会产文件级扩展块链与段描述块，并放开 first_dts 的零起点钉
			bool segmented = false;
			// 段身份（只在 segmented 为真时落字节）；写方用读方同一个 DecodeSegBlock 自校，不另写一套规则
			SegBlock seg;
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

		// 调用方给定的一个分片，数组下标即片序
		struct ChunkInput
		{
			const uint8_t* data = nullptr;
			size_t size = 0;
		};

		struct ChunkFrameInput
		{
			uint8_t stream_id = 0;
			bool is_keyframe = false;
			int64_t dts = 0;
			int64_t pts = 0;
			const ChunkInput* chunks = nullptr;
			uint32_t chunk_count = 0;
		};

		struct WriteSummary
		{
			uint64_t packets = 0;
			uint64_t frames = 0;
			uint64_t index_entries = 0;
			uint64_t duration_ticks = 0;
			uint64_t header_end_offset = 0;	   // 回写文件头之后的游标位置（= 文件头长 64），不是整个文件的长度
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
			ContainerErr WriteFrameChunks(const ChunkFrameInput& in);
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

			// 一片的字节范围。两条写帧口都先把输入切成片，再交给 EmitFrame 统一落盘
			struct Piece
			{
				const uint8_t* data;
				size_t size;
			};

			// 一帧的时间轴与关键帧位：FrameInput 与 ChunkFrameInput 在这里汇流
			struct FrameCall
			{
				uint8_t stream_id;
				bool is_keyframe;
				int64_t dts;
				int64_t pts;
			};

			ContainerErr BeginPackets();
			ContainerErr PatchHead(uint64_t index_offset, uint64_t total_packets, uint64_t duration_ticks,
								   uint8_t flags);
			/* 一片一包的落盘动作：编码 → 写 → 计数，WriteFrame 与 WriteFrameChunks 共用。
			 * bytes 由调用方持有并逐片清空，避免每片重新分配；f 由调用方按片序填好。 */
			ContainerErr EmitPacket(const PacketFields& f, uint32_t max_payload, const uint8_t* data, size_t n,
									std::vector<uint8_t>& bytes);
			/* 时间轴推进：首帧定原点、校验该流 dts 非降、算出 dts_delta 与 pts_delta。
			 * 出错即不写任何字节，两条写帧口共用这一处判定。 */
			ContainerErr AdvanceTimeline(StreamState& s, const FrameCall& fc, uint64_t* dts_delta, int64_t* pts_delta,
										 uint64_t* shifted_out);
			// 一帧的落包动作：推进时间轴 → 逐片一包 → 帧级索引。片边界由调用方给定，这里不再做任何切分
			ContainerErr EmitFrame(StreamState& s, const FrameCall& fc, const std::vector<Piece>& pieces);
			// 帧级索引条目：未开索引区时空转；file_offset 取首包位置，dts 取已平移值
			void PushFrameIndex(uint8_t stream_id, bool is_keyframe, uint64_t file_offset, uint64_t shifted_dts);
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
			uint64_t file_ext_offset_ = 0;	  // 文件级链首块偏移，非分段时一直是 0
			uint64_t packet_area_start_ = 0;
			uint64_t index_area_start_ = 0;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_WRITER_H
