#include "structure.h"

#include "../core/crc32.h"
#include "byte_io.h"
#include "timebase.h"

namespace clv
{
	namespace container
	{

		namespace
		{

			const uint8_t kFileMagic[8] = { 'C', 'L', 'V', 'F', 'I', 'L', 'E', 0 };
			const uint8_t kTailMagic[8] = { 'C', 'L', 'V', 'E', 'N', 'D', 0, 0 };
			const uint8_t kExtMagic[4] = { 'C', 'L', 'V', 'X' };

			/* 流描述符的**逐字段偏移**，成链定义：后一个 = 前一个 + 前一个的宽度。
			 * 这样改任何一个字段的宽度，下游偏移自动跟着走 —— 裸偏移写死时，改了写侧忘了改读侧
			 * 是**不报错**的静默错位。CRC 与总尺寸另外钉成 static_assert，改结构即编译失败。
			 * 字段定义见 docs/容器规范.md 的流描述符一节；读方按这条链取字段，写方按字段顺序追加，读写两侧必须同序。 */
			constexpr size_t kSdStreamId = 0u;
			constexpr size_t kSdStreamType = kSdStreamId + 1u;
			constexpr size_t kSdCodecId = kSdStreamType + 1u;
			constexpr size_t kSdCodecVersion = kSdCodecId + 1u;
			constexpr size_t kSdTimebaseNum = kSdCodecVersion + 1u;
			constexpr size_t kSdTimebaseDen = kSdTimebaseNum + 8u;
			constexpr size_t kSdMaxPacketSize = kSdTimebaseDen + 8u;
			constexpr size_t kSdFirstDts = kSdMaxPacketSize + 4u;
			constexpr size_t kSdLanguage = kSdFirstDts + 8u;
			constexpr size_t kSdSampleRate = kSdLanguage + 8u;
			constexpr size_t kSdChannels = kSdSampleRate + 4u;
			constexpr size_t kSdBitDepth = kSdChannels + 2u;
			constexpr size_t kSdCodecFlags0 = kSdBitDepth + 2u;
			constexpr size_t kSdVlcTableId = kSdCodecFlags0 + 1u;
			constexpr size_t kSdExtOffset = kSdVlcTableId + 1u;
			constexpr size_t kSdCodecFlags1 = kSdExtOffset + 8u;
			constexpr size_t kSdLayerId = kSdCodecFlags1 + 1u;
			constexpr size_t kSdCrc32 = kSdLayerId + 1u;
			constexpr size_t kSdReserved = kSdCrc32 + 4u;
			constexpr size_t kSdTotal = kSdReserved + 4u;
			static_assert(kSdCrc32 == 60u, "流描述符的 crc32 偏移必须是 60");
			static_assert(kSdTotal == kStreamDescSize, "逐字段偏移链的合计必须等于结构尺寸");

			bool MagicOk(const uint8_t* p, const uint8_t* m, size_t n) noexcept
			{
				for (size_t i = 0; i < n; ++i)
					if (p[i] != m[i]) return false;
				return true;
			}

			bool BitDepthOk(uint8_t stream_type, uint16_t bit_depth) noexcept
			{
				switch (stream_type)
				{
				case static_cast<uint8_t>(StreamType::kVideo):
					return bit_depth == 8 || bit_depth == 10 || bit_depth == 12;
				case static_cast<uint8_t>(StreamType::kAudio): return bit_depth == 16 || bit_depth == 24;
				case static_cast<uint8_t>(StreamType::kSubtitle): return bit_depth == 0;
				default: return false;
				}
			}

			StreamTick ToTick(uint64_t dts, const StreamDesc& d) noexcept
			{
				StreamTick t;
				t.dts = dts;
				t.num = static_cast<uint32_t>(d.timebase_num);
				t.den = static_cast<uint32_t>(d.timebase_den);
				return t;
			}

		}	 // namespace

		void EncodeFileHead(const FileHead& h, std::vector<uint8_t>& out)
		{
			const size_t base = out.size();
			Sink s(out);
			s.Bytes(kFileMagic, 8);
			s.LE16(h.version_major);
			s.LE16(h.version_minor);
			s.U8(h.stream_count);
			s.U8(h.flags);
			s.LE16(0);	  // 14 reserved
			s.LE64(h.duration_ticks);
			s.LE64(h.total_packets);
			s.LE64(h.index_offset);
			s.LE32(0);					  // 40 header_crc32 占位
			s.LE64(h.file_ext_offset);	  // 44
			s.LE64(h.reserved_tail);	  // 52..59 reserved
			s.LE32(h.reserved_tail2);	  // 60..63 reserved

			const uint32_t crc = core::Crc32(out.data() + base, 40);
			WriteLE32(out.data() + base + 40, crc);
		}

		bool DecodeFileHead(const uint8_t* p, size_t n, FileHead* out)
		{
			if (p == nullptr || out == nullptr || n < kFileHeadSize) return false;
			if (! MagicOk(p, kFileMagic, 8)) return false;
			if (ReadLE32(p + 40) != core::Crc32(p, 40)) return false;

			*out = FileHead {};
			out->version_major = ReadLE16(p + 8);
			out->version_minor = ReadLE16(p + 10);
			out->stream_count = p[12];
			out->flags = p[13];
			out->duration_ticks = ReadLE64(p + 16);
			out->total_packets = ReadLE64(p + 24);
			out->index_offset = ReadLE64(p + 32);
			out->file_ext_offset = ReadLE64(p + 44);
			out->reserved_tail = ReadLE64(p + 52);
			out->reserved_tail2 = ReadLE32(p + 60);
			return true;
		}

		void EncodeStreamDesc(const StreamDesc& d, std::vector<uint8_t>& out)
		{
			const size_t base = out.size();
			Sink s(out);
			s.U8(d.stream_id);
			s.U8(d.stream_type);
			s.U8(d.codec_id);
			s.U8(d.codec_version);
			s.LE64(d.timebase_num);
			s.LE64(d.timebase_den);
			s.LE32(d.max_packet_size);
			s.LE64(d.first_dts);
			s.LE64(d.language);
			s.LE32(d.sample_rate);
			s.LE16(d.channels);
			s.LE16(d.bit_depth);
			s.U8(d.codec_flags_0);
			s.U8(d.vlc_table_id);
			s.LE64(d.ext_offset);
			s.U8(d.codec_flags_1);
			s.U8(d.layer_id);
			s.LE32(0);	  // crc32 占位
			s.LE32(0);	  // reserved

			WriteLE32(out.data() + base + kSdCrc32, core::Crc32(out.data() + base, kSdCrc32));
		}

		bool DecodeStreamDesc(const uint8_t* p, size_t n, StreamDesc* out)
		{
			if (p == nullptr || out == nullptr || n < kStreamDescSize) return false;
			if (ReadLE32(p + kSdCrc32) != core::Crc32(p, kSdCrc32)) return false;

			*out = StreamDesc {};
			out->stream_id = p[kSdStreamId];
			out->stream_type = p[kSdStreamType];
			out->codec_id = p[kSdCodecId];
			out->codec_version = p[kSdCodecVersion];
			out->timebase_num = ReadLE64(p + kSdTimebaseNum);
			out->timebase_den = ReadLE64(p + kSdTimebaseDen);
			out->max_packet_size = ReadLE32(p + kSdMaxPacketSize);
			out->first_dts = ReadLE64(p + kSdFirstDts);
			out->language = ReadLE64(p + kSdLanguage);
			out->sample_rate = ReadLE32(p + kSdSampleRate);
			out->channels = ReadLE16(p + kSdChannels);
			out->bit_depth = ReadLE16(p + kSdBitDepth);
			out->codec_flags_0 = p[kSdCodecFlags0];
			out->vlc_table_id = p[kSdVlcTableId];
			out->ext_offset = ReadLE64(p + kSdExtOffset);
			out->codec_flags_1 = p[kSdCodecFlags1];
			out->layer_id = p[kSdLayerId];
			return true;
		}

		void EncodeIndexHead(uint32_t entry_count, std::vector<uint8_t>& out)
		{
			const size_t base = out.size();
			Sink s(out);
			s.LE32(entry_count);
			s.LE32(0);	  // 4 reserved
			s.LE32(0);	  // 8 header_crc32 占位
			s.LE32(0);	  // 12 reserved
			WriteLE32(out.data() + base + 8, core::Crc32(out.data() + base, 8));
		}

		bool DecodeIndexHead(const uint8_t* p, size_t n, uint32_t* entry_count)
		{
			if (p == nullptr || entry_count == nullptr || n < kIndexHeadSize) return false;
			if (ReadLE32(p + 8) != core::Crc32(p, 8)) return false;
			*entry_count = ReadLE32(p);
			return true;
		}

		void EncodeIndexEntry(const IndexEntry& e, std::vector<uint8_t>& out)
		{
			Sink s(out);
			s.U8(e.stream_id);
			s.U8(e.is_keyframe ? 1u : 0u);
			s.LE16(0);	  // 2 reserved
			s.LE64(e.file_offset);
			s.LE64(e.dts);
		}

		bool DecodeIndexEntry(const uint8_t* p, size_t n, IndexEntry* out)
		{
			if (p == nullptr || out == nullptr || n < kIndexEntrySize) return false;
			*out = IndexEntry {};
			out->stream_id = p[0];
			out->is_keyframe = (p[1] & 1u) != 0;
			out->file_offset = ReadLE64(p + 4);
			out->dts = ReadLE64(p + 12);
			return true;
		}

		void EncodeFileTail(uint64_t total_packets, std::vector<uint8_t>& out)
		{
			Sink s(out);
			s.Bytes(kTailMagic, 8);
			s.LE64(total_packets);
		}

		bool DecodeFileTail(const uint8_t* p, size_t n, uint64_t* total_packets)
		{
			if (p == nullptr || total_packets == nullptr || n < kFileTailSize) return false;
			if (! MagicOk(p, kTailMagic, 8)) return false;
			*total_packets = ReadLE64(p + 8);
			return true;
		}

		size_t EncodeExtBlock(uint8_t ext_type, uint8_t ext_version, const uint8_t* data, size_t n,
							  std::vector<uint8_t>& out)
		{
			if (n > 0xFFFFFFFFull) return 0;
			if (n != 0 && data == nullptr) return 0;

			const size_t base = out.size();
			Sink s(out);
			s.Bytes(kExtMagic, 4);
			s.U8(ext_type);
			s.U8(ext_version);
			s.LE32(static_cast<uint32_t>(n));
			s.LE64(0);	  // next_ext_offset：整链布局定下后由写方回填
			s.Bytes(data, n);
			s.LE32(0);	  // crc32 占位

			const size_t covered = n + kExtBlockFixedPrefix;
			WriteLE32(out.data() + base + covered, core::Crc32(out.data() + base, covered));
			return covered + kExtBlockCrcSize;
		}

		bool DecodeExtBlock(const uint8_t* p, size_t n, uint8_t* ext_type, uint8_t* ext_version,
							std::vector<uint8_t>* data, uint64_t* next_ext_offset)
		{
			if (p == nullptr || n < kExtBlockFixedPrefix + kExtBlockCrcSize) return false;
			if (! MagicOk(p, kExtMagic, 4)) return false;

			const uint32_t len = ReadLE32(p + kExtLenOffset);
			if (n < kExtBlockFixedPrefix + len + kExtBlockCrcSize) return false;

			const size_t covered = kExtBlockFixedPrefix + len;
			if (ReadLE32(p + covered) != core::Crc32(p, covered)) return false;

			if (ext_type != nullptr) *ext_type = p[kExtTypeOffset];
			if (ext_version != nullptr) *ext_version = p[kExtVersionOffset];
			if (next_ext_offset != nullptr) *next_ext_offset = ReadLE64(p + kExtNextOffset);
			if (data != nullptr) data->assign(p + kExtBlockFixedPrefix, p + kExtBlockFixedPrefix + len);
			return true;
		}

		bool HeadValuesOk(const FileHead& h) noexcept
		{
			if (h.version_major != kVersionMajorV2) return false;
			if (h.stream_count == 0) return false;
			if ((h.flags & 0xF0u) != 0) return false;							// bit4-7 reserved
			if (h.reserved_tail != 0 || h.reserved_tail2 != 0) return false;	// 52..63 必须全 0
			const bool segmented = (h.flags & static_cast<uint8_t>(HeadFlagBit::kSegmented)) != 0;
			// 两边都钉：segmented 时入口必须指向一块，非 segmented 时这里必须是 0（不能携带无主的偏移）
			if (segmented && h.file_ext_offset == 0) return false;
			if (! segmented && h.file_ext_offset != 0) return false;
			const bool streaming = (h.flags & static_cast<uint8_t>(HeadFlagBit::kStreaming)) != 0;
			if (! streaming && h.total_packets == kUnknownPacketCount) return false;
			const bool index_present = (h.flags & static_cast<uint8_t>(HeadFlagBit::kIndexPresent)) != 0;
			if (! index_present && h.index_offset != 0) return false;
			return true;
		}

		bool DescValuesOk(const StreamDesc& d, bool segmented) noexcept
		{
			if (d.stream_type > static_cast<uint8_t>(StreamType::kSubtitle)) return false;
			if (! TimebaseOk(d.timebase_num, d.timebase_den)) return false;
			if (! segmented && d.first_dts != 0) return false;	  // 零起点；分段容器的后续段取该段首帧的绝对 dts
			if (d.max_packet_size == 0) return false;
			if (! BitDepthOk(d.stream_type, d.bit_depth)) return false;
			if (d.layer_id != 0) return false;		   // 本版 = 0
			if (d.vlc_table_id != 0) return false;	   // 本版仅允许 0
			if (d.codec_flags_1 != 0) return false;	   // 预留，本版 = 0
			if (d.stream_type == static_cast<uint8_t>(StreamType::kAudio))
			{
				if (d.sample_rate == 0) return false;
				if (d.channels == 0) return false;
			}
			return true;
		}

		void EncodeSegBlock(const SegBlock& s, std::vector<uint8_t>& out)
		{
			Sink sk(out);
			sk.U8(s.flags);
			sk.U8(0);	 // 1 reserved
			sk.LE64(s.index);
			sk.LE64(s.total);
			sk.LE64(s.start_ticks);
			sk.LE64(s.end_ticks);
			for (int i = 0; i < 14; ++i) sk.U8(0);	  // 34..47 reserved；块在文件里的位移由扩展块编码管
		}

		bool DecodeSegBlock(const uint8_t* p, size_t n, SegBlock* out) noexcept
		{
			if (p == nullptr || out == nullptr || n < kSegBlockSize) return false;
			for (int i = 34; i < 48; ++i)
				if (p[i] != 0) return false;		  // reserved 段非 0 即不成立
			if ((p[0] & 0xF8u) != 0) return false;	  // seg_flags bit3-7 reserved
			if (p[1] != 0) return false;			  // 1 reserved
			// 判点全部排在写出参之前：不成立时不留一套写了一半的出参
			const uint64_t index = ReadLE64(p + 2);
			const uint64_t total = ReadLE64(p + 10);
			const uint64_t start_ticks = ReadLE64(p + 18);
			const uint64_t end_ticks = ReadLE64(p + 26);
			if (total == 0) return false;											// 0 不合法（未知用全 1）
			if ((p[0] & 1u) != 0 && total == kUnknownSegmentTotal) return false;	// bit0 说已知却填全 1
			if ((p[0] & 1u) == 0 && total != kUnknownSegmentTotal) return false;	// bit0 说未知却填了数
			if (end_ticks < start_ticks) return false;
			*out = SegBlock {};
			out->flags = p[0];
			out->index = index;
			out->total = total;
			out->start_ticks = start_ticks;
			out->end_ticks = end_ticks;
			return true;
		}

		int CompareIndexEntries(const IndexEntry& a, const IndexEntry& b,
								const std::vector<StreamDesc*>& desc_by_id) noexcept
		{
			const StreamDesc* da = a.stream_id < desc_by_id.size() ? desc_by_id[a.stream_id] : nullptr;
			const StreamDesc* db = b.stream_id < desc_by_id.size() ? desc_by_id[b.stream_id] : nullptr;

			// 描述符缺失的条目排最后：单位不明的 dts 不参与统一时间轴换算
			if (da == nullptr || db == nullptr)
			{
				if (da == nullptr && db != nullptr) return 1;
				if (da != nullptr && db == nullptr) return -1;
			}
			else
			{
				const int cmp = CompareUnifiedTick(ToTick(a.dts, *da), ToTick(b.dts, *db));
				if (cmp != 0) return cmp;
			}

			if (a.stream_id != b.stream_id) return a.stream_id < b.stream_id ? -1 : 1;
			// 排序键就这三档：统一时间轴 → 流号 → 文件内出现顺序。写方建的索引里一条条目占一个首包偏移，
			// file_offset 两两不等，同流同偏移的两条目只能等价（不再拿条目自己的 dts 分先后）
			if (a.file_offset != b.file_offset) return a.file_offset < b.file_offset ? -1 : 1;
			return 0;
		}

	}	 // namespace container
}	 // namespace clv

