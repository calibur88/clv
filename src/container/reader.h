/* reader.h
 *
 * 容器读方：严格解析 + 容错处置 + 帧重组 + 索引区校验与二分查询。
 *
 * 处置分档：只有「下一包 READ_HEADER / CHECK_FRAME_HEADER 失败」进重同步；
 * CRC 不过、字段区不成立（含非最短形式、payload_size 仲裁式不符）一律丢包 + 跳 total_size，
 * 片序跳变记丢片后继续解析。重同步窗口内找不回包头 → 放弃包区剩余内容、游标推到包区末尾
 * （v1 包区是各流共享的单段线性序列，停不了单条流），索引区与文件尾照常可读；
 * 任何路径都不判整文件损坏。
 */
#ifndef CLV_CONTAINER_READER_H
#define CLV_CONTAINER_READER_H

#include "frames.h"
#include "io.h"
#include "packet.h"
#include "status.h"
#include "structure.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace clv
{
	namespace container
	{

		struct ReaderConfig
		{
			// 0 = 取默认上限：该流 max_packet_size + 外层最坏 41
			uint64_t runtime_packet_limit = 0;
			// 重同步窗口固定 64 KB，v1 不设可调参数
			uint64_t resync_window = 64ull * 1024ull;
			// 读方一次驻留的窗口上限：包区再大也只在窗口内解析（v0 的实现上限，差异说明项）
			uint64_t window_max = 8ull * 1024ull * 1024ull;
		};

		struct ReaderStats
		{
			uint64_t packets_ok = 0;
			uint64_t dropped_crc = 0;
			uint64_t dropped_field = 0;		   // 字段区 / 最短形式 / 仲裁式
			uint64_t dropped_too_large = 0;	   // 超运行时接受域但跳得动
			uint64_t dropped_unknown_stream = 0;
			uint64_t resync_count = 0;
			uint64_t resync_bytes_scanned = 0;
			uint64_t resync_failed = 0;	   // 窗口内没找回包头
			uint64_t frames_ok = 0;
			uint64_t frames_incomplete = 0;
			uint64_t fragment_gaps = 0;
			uint64_t missing_first_packets = 0;
			uint64_t duplicate_fragments = 0;
			uint64_t ext_blocks_bad = 0;	// 扩展块链里 CRC / 布局不成立的块数
			bool truncated = false;			// 包区末尾不足一个包
			bool abandoned = false;			// 重同步失败，包区剩余内容未读完
		};

		// 一条已解析的扩展块（CodecPrivate / 字幕样式 / STREAMINFO 副本等）
		struct ExtRecord
		{
			uint8_t stream_id = 0;
			uint8_t type = 0;
			uint8_t version = 0;
			uint64_t offset = 0;
			std::vector<uint8_t> data;
		};

		class ContainerReader
		{
			public:

			ContainerReader(ByteSourceIf& src, const ReaderConfig& cfg = ReaderConfig {}) noexcept;

			// 读文件头 + 描述符表。头本身不成立返回 BadStructure；描述符逐条校验，
			// 不过的流标记为不可用（不判整文件损坏），其余流照读。
			ContainerErr Open();

			const FileHead& Head() const noexcept { return head_; }

			const std::vector<StreamDesc>& Descs() const noexcept { return descs_; }

			const std::vector<ExtRecord>& ExtBlocks() const noexcept { return exts_; }

			// 含不可用流：结构读出来了就在，供诊断
			bool StreamUsable(uint8_t stream_id) const noexcept;

			// 产出一个重组帧；false = 没有更多帧（正常到包区末尾，或已放弃）
			bool NextFrame(ReassembledFrame* out);

			const ReaderStats& Stats() const noexcept { return st_; }

			// ---- 索引区 ----
			bool HasIndex() const noexcept { return index_valid_; }

			uint32_t IndexEntryCount() const noexcept { return static_cast<uint32_t>(index_.size()); }

			const std::vector<IndexEntry>& IndexEntries() const noexcept { return index_; }

			// 第一个统一时间轴 >= target_ticks 的条目下标；全部小于 target 时返回 index_.size()
			size_t LowerBoundTick(uint64_t target_ticks) const noexcept;

			private:

			bool Ensure(size_t want);
			void Consume(size_t n);
			bool Whitelisted(uint8_t stream_id) const noexcept;
			// 该流的描述符；不可用或没有返回 nullptr
			const StreamDesc* DescOf(uint8_t stream_id) const noexcept;
			// 下标 = stream_id 的指针表，供索引比较用
			std::vector<StreamDesc*> DescLookupTable() const;
			// 接受上限：0 配值时按最宽一条流的 max_packet_size + 外层最坏 41 兜底
			uint64_t OuterLimit() const noexcept;
			uint64_t OuterLimitFor(uint8_t stream_id) const noexcept;
			// 重同步扫描。成功返回 true，*found 是相对当前包首的字节偏移
			bool Resync(uint64_t* found);
			void FeedFrames(const ParsedPacket& p);
			void DrainAssemblers(bool flush_all);
			ContainerErr ReadStructures();
			void ReadIndex();
			void WalkExtChains();

			ByteSourceIf* src_;
			ReaderConfig cfg_;
			FileHead head_;
			std::vector<StreamDesc> descs_;
			std::vector<ExtRecord> exts_;
			std::vector<bool> usable_;							   // 下标 = stream_id
			std::vector<std::unique_ptr<FrameAssembler>> asms_;	   // 下标 = stream_id

			uint64_t area_start_ = 0;
			uint64_t area_end_ = 0;
			uint64_t pos_ = 0;
			std::vector<uint8_t> win_;
			size_t off_ = 0;

			std::vector<ReassembledFrame> pending_;
			bool done_ = false;

			ReaderStats st_;
			std::vector<IndexEntry> index_;
			std::vector<uint64_t> entry_ticks_;
			bool index_valid_ = false;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_READER_H
