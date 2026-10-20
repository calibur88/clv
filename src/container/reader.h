/* reader.h
 *
 * 容器读方：严格解析 + 容错处置 + 帧重组 + 索引区校验与二分查询。
 *
 * 处置分档：只有包头不成立（total_size 不成立或 flags 结构指纹不符）才进重同步；
 * CRC 不过、字段区不成立（含非最短形式、payload_size 与剩余长度不符）一律丢包 + 跳 total_size，
 * 片序跳变记丢片后继续解析。重同步窗口内找不回包头 → 放弃包区剩余内容、游标推到包区末尾
 * （本版包区是各流共享的单段线性序列，停不了单条流），索引区与文件尾照常可读；
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

		/* 读方一次驻留的窗口默认上限。公开面没有改它的口，所以它同时是「单包最大能读回多少」的事实界；
		 * 写侧的单包上界（kMaxPacketPayload）由它派生，读写两侧共用这一个声明域。 */
		constexpr uint64_t kDefaultReadWindow = 8ull * 1024ull * 1024ull;

		/* 外层单包最坏开销 41 B = total_size 4 + stream_id 1 + flags 1 + crc32 4 + 各 varint 最坏 31；
		 * 改包结构时必须重算。读方的接受上限与写方的单包上界都取它。 */
		constexpr uint64_t kOuterOverhead = 41ull;

		/* 写侧单包载荷上界 = 默认读方窗口减去外层最坏开销。声明载荷超过它就写得出读不回，
		 * 所以在落盘之前判 kTooLarge，而不是等读侧报截断。 */
		constexpr uint64_t kMaxPacketPayload = kDefaultReadWindow - kOuterOverhead;
		static_assert(kMaxPacketPayload == 8388567ull, "默认窗口 8 MiB 减外层最坏 41 = 8 388 567");

		struct ReaderConfig
		{
			// 0 = 取默认上限：OuterLimit() 走的是**所有已登记流里最宽那条**的 max_packet_size + 外层最坏 41；
			// 逐条流自己的那条界另有 OuterLimitFor(stream_id)，只在重同步的候选复核里用。
			uint64_t runtime_packet_limit = 0;
			// 读方一次驻留的窗口上限：包区再大也只在窗口内解析（实现上限，不是格式约束）
			uint64_t window_max = kDefaultReadWindow;
		};

		// 重同步扫描窗口：格式侧定死 64 KB，不作为可调项暴露
		constexpr uint64_t kResyncWindow = 64ull * 1024ull;

		struct ReaderStats
		{
			uint64_t packets_ok = 0;
			uint64_t dropped_crc = 0;
			uint64_t dropped_field = 0;	   // 字段区 / 最短形式 / 长度核对不成立
			// 一档三名，全都在「整包太长、按声明长度跳得动」这一族里：① 外层 total_size 超运行时接受域；
			// ② 解出来的 payload_size 超该流 max_packet_size；③ 包在窗口内但超出驻留上限，按声明长度跳过（跳不动走重同步档）。
			uint64_t dropped_too_large = 0;
			uint64_t dropped_unknown_stream = 0;
			uint64_t resync_count = 0;
			uint64_t resync_bytes_scanned = 0;
			uint64_t resync_failed = 0;	   // 窗口内没找回包头
			uint64_t frames_ok = 0;
			uint64_t frames_incomplete = 0;
			uint64_t fragment_gaps = 0;
			uint64_t missing_first_packets = 0;
			uint64_t duplicate_fragments = 0;
			uint64_t ext_blocks_bad = 0;	// 扩展块链里解析不成立的块数：声明长度超出接受域的块同样计入本数
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

			/* 读文件头 + 描述符表 + 文件尾。头本身不成立返回 BadStructure；描述符逐条校验，
			 * 不成立的那条直接丢弃（不进 Descs()、不进白名单），不判整文件损坏，其余流照读。
			 * 「结构读出来了但标记为不可用」的逐条诊断视图本版不提供。 */
			ContainerErr Open();

			const FileHead& Head() const noexcept { return head_; }

			const std::vector<StreamDesc>& Descs() const noexcept { return descriptors_; }

			const std::vector<ExtRecord>& ExtBlocks() const noexcept { return ext_records_; }

			/* 该 id 在不在白名单。坏条目在 Open 就被丢弃，所以 Descs() 里的每条都返回 true、
			 * 其余一律 false：这里区分不出「描述符读出来了但不可用」与「根本没有这条流」。 */
			bool StreamUsable(uint8_t stream_id) const noexcept;

			// 产出一个重组帧；false = 没有更多帧（正常到包区末尾，或已放弃）
			bool NextFrame(ReassembledFrame* out);

			/* 分段容器的观测计数。段标识本身落在 header_crc32 覆盖之外（docs/容器规范.md 的段描述块一节把这一条
			 * 记为已知弱化），失效的实测主形态是静默丢失 ⇒ 缺失 / 被拒 / 声明有间断都必须是可查的数，不得只走 warn。 */
			struct SegmentStats
			{
				uint64_t block_missing = 0;					 // 声明了 segmented 却没拿到可用的段描述块
				uint64_t block_bad = 0;						 // 解析不成立的块数：声明长度超出接受域的块同样计入本数
				uint64_t discontinuities = 0;				 // 段块 bit2 声明与上一段之间有间断
				uint64_t video_stream_no_param_block = 0;	 // 视频流缺 ext_type = 3 而在打开时被丢弃的条数
			};

			const ReaderStats& Stats() const noexcept { return stats_; }

			const SegmentStats& SegStats() const noexcept { return seg_stats_; }

			// 段身份；非分段容器、或段块拿不到时为 nullptr
			const SegBlock* Seg() const noexcept { return seg_have_ ? &seg_ : nullptr; }

			// ---- 索引区 ----
			bool HasIndex() const noexcept { return index_valid_; }

			// 本文件是不是分段容器的一员（文件头 flags 的 bit3）
			bool Segmented() const noexcept
			{
				return (head_.flags & static_cast<uint8_t>(HeadFlagBit::kSegmented)) != 0;
			}

			uint32_t IndexEntryCount() const noexcept { return static_cast<uint32_t>(index_.size()); }

			const std::vector<IndexEntry>& IndexEntries() const noexcept { return index_; }

			// 第一个统一时间轴 >= target_ticks 的条目下标；全部小于 target 时返回 index_.size()
			size_t LowerBoundTick(uint64_t target_ticks) const noexcept;

			// ---- 文件尾（末 16 字节）----
			// 尾 magic 与布局成立才为 true；缺失只按「无尾」处置，不判文件损坏
			bool HasFileTail() const noexcept { return tail_seen_; }

			// 文件尾声明的 total_packets；HasFileTail() 为 false 时无意义（保持 0）
			uint64_t TailTotalPackets() const noexcept { return tail_total_packets_; }

			/* 包数的最终口径：streaming = 1 时真实值只落文件尾，有尾以尾为准、无尾（截断）为未知；
			 * streaming = 0 时以文件头声明值为准。Head().total_packets 始终是文件头的原样字段。 */
			uint64_t TotalPackets() const noexcept;

			private:

			bool Ensure(size_t want);
			void Consume(size_t n);
			/* 整包长度超出窗口上限、永远读不进窗口时的一档：按声明长度跳过去并计 dropped_too_large。
			 * 读不进整包既不代表包头不成立，也不代表包区到此为止。跳不动（指纹不符或超出包区）返回 false。 */
			bool SkipOverSizedPacket(uint32_t declared);
			bool Whitelisted(uint8_t stream_id) const noexcept;
			// 该流的描述符；不可用或没有返回 nullptr。走 desc_by_id_，O(1)
			const StreamDesc* DescOf(uint8_t stream_id) const noexcept;
			// 建 / 重建 desc_by_id_：Open 读完描述符表后调一次，热路径只查表不做线性扫
			void BuildDescTable() noexcept;
			// 接受上限：0 配值时按最宽一条流的 max_packet_size + 外层最坏 41 兜底
			uint64_t OuterLimit() const noexcept;
			uint64_t OuterLimitFor(uint8_t stream_id) const noexcept;
			// 重同步扫描。成功返回 true，*found 是相对当前包首的字节偏移
			bool Resync(uint64_t* found);
			void FeedFrames(const ParsedPacket& p);
			void DrainAssemblers(bool flush_all);
			// 把一次喂包的计数增量累加进全局统计
			void Absorb(const FrameStats& d) noexcept;
			ContainerErr ReadStructures();
			void ReadIndex();
			/* 走各流扩展块链并定包区起点。链逐块成立、链路向前，却在跳数上限处仍没到头 → kExtChainTooLong
			 * （包区起点定不出来，整次打开拒）。中途某块不成立只停走该链并计入 ext_blocks_bad，仍返回 kOk，
			 * 与「畸形链停走继续」的分层口径一致。 */
			ContainerErr WalkExtChains();
			// 走文件级链取段描述块（只在头里 segmented 置起时调）
			void ReadSegBlock();
			// 读末 16 字节：无索引区时包区终点按文件长度推定，包数只能靠文件尾独立核对
			void ReadFileTail();

			ByteSourceIf* source_;
			ReaderConfig config_;
			FileHead head_;
			std::vector<StreamDesc> descriptors_;
			std::vector<ExtRecord> ext_records_;
			std::vector<bool> usable_;									 // 下标 = stream_id
			std::vector<StreamDesc*> desc_by_id_;						 // 下标 = stream_id；元素指向 descriptors_ 内
			std::vector<std::unique_ptr<FrameAssembler>> assemblers_;	 // 下标 = stream_id

			uint64_t area_start_ = 0;
			uint64_t area_end_ = 0;
			uint64_t pos_ = 0;
			std::vector<uint8_t> window_;
			size_t window_off_ = 0;

			// 一次喂包最多产出 2 帧，且 NextFrame 每轮先取走一帧，所以这里始终只有个位数条目
			std::vector<ReassembledFrame> pending_;
			bool done_ = false;

			ReaderStats stats_;
			SegmentStats seg_stats_;
			SegBlock seg_;
			bool seg_have_ = false;
			std::vector<IndexEntry> index_;
			std::vector<uint64_t> entry_ticks_;
			bool index_valid_ = false;
			// 文件尾只作包数的第二出处与独立核对，不参与包区边界推定
			bool tail_seen_ = false;
			uint64_t tail_total_packets_ = 0;
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_READER_H
