/* structure.h
 *
 * 定长结构的编解码：文件头（64）/ 流描述符（64）/ 索引头（16）+ 条目（16）/ 文件尾（16）/ 扩展块。
 * 布局与 CRC 覆盖范围逐项：
 *   文件头 header_crc32 覆盖 [0, 40)；流描述符 crc32 覆盖 [0, 56)；
 *   索引头 header_crc32 覆盖 [0, 8)；索引条目无 CRC；扩展块 crc32 自 ext_magic 起到 crc32 前。
 *
 * 结构级值域校验集中在 ValuesOk 两个函数里：不封合法集上限，只挡非零、u32 上限、
 * v1 强制值（first_dts = 0、layer_id = 0、vlc_table_id = 0、codec_flags_1 = 0）与 bit_depth 取值集。
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
		constexpr size_t kStreamDescSize = 64;
		constexpr size_t kFileTailSize = 16;
		constexpr size_t kIndexHeadSize = 16;
		constexpr size_t kIndexEntrySize = 16;
		constexpr size_t kExtBlockFixedPrefix = 4 + 1 + 1 + 2 + 8;	  // magic..next_ext_offset
		constexpr size_t kExtBlockCrcSize = 4;

		constexpr uint16_t kVersionMajorV1 = 1;
		constexpr uint16_t kVersionMinorV1 = 0;
		constexpr uint64_t kUnknownPacketCount = 0xFFFFFFFFFFFFFFFFull;

		enum class HeadFlagBit : uint8_t
		{
			kIndexPresent = 1u << 0,
			kGloballySorted = 1u << 1,
			kStreaming = 1u << 2
		};

		enum class StreamType : uint8_t
		{
			kVideo = 0,
			kAudio = 1,
			kSubtitle = 2
		};

		struct FileHead
		{
			uint16_t version_major = kVersionMajorV1;
			uint16_t version_minor = kVersionMinorV1;
			uint8_t stream_count = 0;
			uint8_t flags = 0;
			uint64_t duration_ticks = 0;
			uint64_t total_packets = 0;
			uint64_t index_offset = 0;
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
			uint32_t ext_offset = 0;
			uint8_t codec_flags_1 = 0;
			uint8_t layer_id = 0;
		};

		struct IndexEntry
		{
			uint8_t stream_id = 0;
			bool is_keyframe = false;
			uint32_t file_offset = 0;
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

		/* 扩展块。返回线上字节数，0 表示不成立（data 为空但 n 非 0，或 n > 0xFFFF——ext_len 只有 2 字节）。
	 * next_ext_offset 由写方在整链布局定下来后回填，这里先写 0。 */
		size_t EncodeExtBlock(uint8_t ext_type, uint8_t ext_version, const uint8_t* data, size_t n,
							  std::vector<uint8_t>& out);
		bool DecodeExtBlock(const uint8_t* p, size_t n, uint8_t* ext_type, uint8_t* ext_version,
							std::vector<uint8_t>* data, uint64_t* next_ext_offset);

		// ---- 值域与语义校验 ----
		bool HeadValuesOk(const FileHead& h) noexcept;
		bool DescValuesOk(const StreamDesc& d) noexcept;

		/* 索引排序比较：先按统一时间轴，再按 stream_id，再按文件内出现顺序（file_offset）。
		 * descs 必须按 stream_id 索引（数组长度 = 最大 stream_id + 1），找不到的流排在最后。
		 * 返回 <0 / 0 / >0。 */
		int CompareIndexEntries(const IndexEntry& a, const IndexEntry& b,
								const std::vector<StreamDesc*>& desc_by_id) noexcept;

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_STRUCTURE_H
