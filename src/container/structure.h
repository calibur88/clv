/* structure.h
 *
 * 定长结构的编解码：文件头（64）/ 流描述符（68）/ 索引头（16）+ 条目（20）/ 文件尾（16）/ 扩展块。
 * 布局与 CRC 覆盖范围逐项：
 *   文件头 header_crc32 覆盖 [0, 40)；流描述符 crc32 覆盖 [0, 60)；
 *   索引头 header_crc32 覆盖 [0, 8)；索引条目无 CRC；扩展块 crc32 自 ext_magic 起到 crc32 前。
 *
 * 结构级值域校验集中在 ValuesOk 两个函数里：不封合法集上限，只挡非零、u32 上限、
 * 本版强制值（layer_id = 0、vlc_table_id = 0、codec_flags_1 = 0、reserved 段全 0）、first_dts 随 segmented 分档、bit_depth 取值集。
 */
#ifndef CLV_CONTAINER_STRUCTURE_H
#define CLV_CONTAINER_STRUCTURE_H

#include "packet.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace container
	{

		constexpr size_t kFileHeadSize = 64;
		// 头长 = 定长前段 44 + file_ext_offset 8 + reserved 12：把形状钉在算式上，漏一个字段当场编译不过
		static_assert(kFileHeadSize == 44u + 8u + 12u, "文件头字段布局相加必须等于 64 字节");
		constexpr size_t kStreamDescSize = 68;
		constexpr size_t kFileTailSize = 16;
		constexpr size_t kIndexHeadSize = 16;
		constexpr size_t kIndexEntrySize = 20;
		/* 扩展块内的字段偏移成链定义：后一个 = 前一个 + 前一个的宽度。
		 * ext_len 的宽度一改，next_ext_offset 与定长前缀自动跟着走 —— 裸偏移写死时读写两侧漏改一处
		 * 是不报错的静默错位（crc32 与总尺寸另有 static_assert 兜底）。 */
		constexpr size_t kExtTypeOffset = 4;	// magic 'CLVX' 之后
		constexpr size_t kExtVersionOffset = kExtTypeOffset + 1u;
		constexpr size_t kExtLenOffset = kExtVersionOffset + 1u;
		constexpr size_t kExtNextOffset = kExtLenOffset + 4u;	 // ext_len 四字节
		constexpr size_t kExtBlockFixedPrefix = kExtNextOffset + 8u;
		constexpr size_t kExtBlockCrcSize = 4;
		static_assert(kExtBlockFixedPrefix == 18u, "扩展块定长前缀必须是 18 字节");

		// 分段容器：段身份落在文件级扩展块链的段描述块里（载荷定长 48 B，字段偏移见下方 SegBlock）
		constexpr uint8_t kExtTypeEncMeta = 3;	   // 视频参数块（v2 起对视频流必选，见 docs/容器规范.md 的扩展块一节）
		constexpr uint8_t kExtTypeSegBlock = 7;	   // 段描述块
		constexpr uint8_t kExtTypeSegIndex = 8;	   // 段索引表：值已留、本版未实现
		constexpr size_t kSegBlockSize = 48;
		// 段总数未知 = 全 1（与 total_packets 的未知值同族惯例）；0 判值域不成立，不拿来表示未知
		constexpr uint64_t kUnknownSegmentTotal = 0xFFFFFFFFFFFFFFFFull;
		static_assert(kSegBlockSize == 1u + 1u + 8u + 8u + 8u + 8u + 8u + 6u, "段描述块载荷必须是 48 字节");

		// stream_id 是 u8，读写方按 id 建查表时的项数
		constexpr size_t kStreamIdSpace = 256;

		// 容器格式版本：结构版本号写进文件头，读方据此判能不能解析（与工程版本、文档版本分轨）
		constexpr uint16_t kVersionMajorV2 = 2;
		constexpr uint16_t kVersionMinorV2 = 0;
		constexpr uint64_t kUnknownPacketCount = 0xFFFFFFFFFFFFFFFFull;

		enum class HeadFlagBit : uint8_t
		{
			kIndexPresent = 1u << 0,
			kGloballySorted = 1u << 1,
			kStreaming = 1u << 2,
			kSegmented = 1u << 3
		};

		enum class StreamType : uint8_t
		{
			kVideo = 0,
			kAudio = 1,
			kSubtitle = 2
		};

		struct FileHead
		{
			uint16_t version_major = kVersionMajorV2;
			uint16_t version_minor = kVersionMinorV2;
			uint8_t stream_count = 0;
			uint8_t flags = 0;
			uint64_t duration_ticks = 0;
			uint64_t total_packets = 0;
			uint64_t index_offset = 0;
			// 文件级扩展块链首块的绝对偏移，0 = 无；只有 flags 的 kSegmented 置起时才要求非 0
			uint64_t file_ext_offset = 0;
			uint64_t reserved_tail = 0;		// 52..59 的 reserved，本版必须为 0
			uint32_t reserved_tail2 = 0;	// 60..63 的 reserved，本版必须为 0
		};

		struct StreamDesc
		{
			uint8_t stream_id = 0;
			uint8_t stream_type = 0;
			uint8_t codec_id = 0;
			uint8_t codec_version = 0;
			uint64_t timebase_num = 1;
			uint64_t timebase_den = 1;
			uint32_t max_packet_size = 0;
			uint64_t first_dts = 0;
			uint64_t language = 0;
			uint32_t sample_rate = 0;
			uint16_t channels = 0;
			uint16_t bit_depth = 0;
			uint8_t codec_flags_0 = 0;
			uint8_t vlc_table_id = 0;
			uint64_t ext_offset = 0;
			uint8_t codec_flags_1 = 0;
			uint8_t layer_id = 0;
		};

		/* 段描述块的载荷（48 B，小端）。落字段的偏移与宽度照 docs/容器规范.md 的段描述块表：
	 * 0 seg_flags u8 / 1 reserved u8 / 2 seg_index u64 / 10 seg_total u64 / 18 start_ticks u64 / 26 end_ticks u64
	 * / 34 与 42 共 14 字节 reserved。 */
		struct SegBlock
		{
			uint8_t flags = 0;	   // bit0 总数已知 / bit1 段首帧在各视频流上都是关键帧 / bit2 与本段上一段之间有时间断
			uint64_t index = 0;	   // 自 0 起
			uint64_t total = kUnknownSegmentTotal;
			uint64_t start_ticks = 0;
			uint64_t end_ticks = 0;
		};

		struct IndexEntry
		{
			uint8_t stream_id = 0;
			bool is_keyframe = false;
			uint64_t file_offset = 0;
			uint64_t dts = 0;
		};

		// ---- 定长编解码：Encode 一律追加 64 / 16 字节；Decode 需要 n 足够，CRC 不符即失败 ----
		void EncodeFileHead(const FileHead& h, std::vector<uint8_t>& out);
		bool DecodeFileHead(const uint8_t* p, size_t n, FileHead* out);

		void EncodeStreamDesc(const StreamDesc& d, std::vector<uint8_t>& out);
		bool DecodeStreamDesc(const uint8_t* p, size_t n, StreamDesc* out);

		void EncodeIndexHead(uint32_t entry_count, std::vector<uint8_t>& out);
		bool DecodeIndexHead(const uint8_t* p, size_t n, uint32_t* entry_count);

		void EncodeIndexEntry(const IndexEntry& e, std::vector<uint8_t>& out);
		bool DecodeIndexEntry(const uint8_t* p, size_t n, IndexEntry* out);

		void EncodeFileTail(uint64_t total_packets, std::vector<uint8_t>& out);
		bool DecodeFileTail(const uint8_t* p, size_t n, uint64_t* total_packets);

		/* 扩展块。返回线上字节数，0 表示不成立（data 为空但 n 非 0，或 n 超出 ext_len 四字节可表达域）。
	 * next_ext_offset 由写方在整链布局定下来后回填，这里先写 0。 */
		size_t EncodeExtBlock(uint8_t ext_type, uint8_t ext_version, const uint8_t* data, size_t n,
							  std::vector<uint8_t>& out);
		bool DecodeExtBlock(const uint8_t* p, size_t n, uint8_t* ext_type, uint8_t* ext_version,
							std::vector<uint8_t>* data, uint64_t* next_ext_offset);

		// ---- 值域与语义校验 ----
		bool HeadValuesOk(const FileHead& h) noexcept;
		/* segmented = 本文件是不是分段容器的一员（文件头 flags 的 kSegmented）。first_dts 的合法域跟着它走：
	 * 非分段文件仍强制 0（零起点），分段容器的后续段则必须带本段该流首帧的绝对 dts ⇒ 该位为真时 0 之外也合法。 */
		bool DescValuesOk(const StreamDesc& d, bool segmented = false) noexcept;
		void EncodeSegBlock(const SegBlock& s, std::vector<uint8_t>& out);
		bool DecodeSegBlock(const uint8_t* p, size_t n, SegBlock* out) noexcept;

		/* 索引排序比较：先按统一时间轴，再按 stream_id，再按文件内出现顺序（file_offset）。
		 * desc_by_id 以下标 = stream_id 索引，未登记的流位置为 nullptr，这类条目排在最后。
		 * 返回 <0 / 0 / >0。 */
		int CompareIndexEntries(const IndexEntry& a, const IndexEntry& b,
								const std::vector<StreamDesc*>& desc_by_id) noexcept;

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_STRUCTURE_H
