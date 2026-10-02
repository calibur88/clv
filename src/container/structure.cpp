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
			s.LE32(0);								 // 40 header_crc32 占位
			for (int i = 0; i < 20; ++i) s.U8(0);	 // 44..63 reserved

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
			s.LE32(d.ext_offset);
			s.U8(d.codec_flags_1);
			s.U8(d.layer_id);
			s.LE32(0);	  // 56 crc32 占位
			s.LE32(0);	  // 60 reserved

			WriteLE32(out.data() + base + 56, core::Crc32(out.data() + base, 56));
		}

		bool DecodeStreamDesc(const uint8_t* p, size_t n, StreamDesc* out)
		{
			if (p == nullptr || out == nullptr || n < kStreamDescSize) return false;
			if (ReadLE32(p + 56) != core::Crc32(p, 56)) return false;

			*out = StreamDesc {};
			out->stream_id = p[0];
			out->stream_type = p[1];
			out->codec_id = p[2];
			out->codec_version = p[3];
			out->timebase_num = ReadLE64(p + 4);
			out->timebase_den = ReadLE64(p + 12);
			out->max_packet_size = ReadLE32(p + 20);
			out->first_dts = ReadLE64(p + 24);
			out->language = ReadLE64(p + 32);
			out->sample_rate = ReadLE32(p + 40);
			out->channels = ReadLE16(p + 44);
			out->bit_depth = ReadLE16(p + 46);
			out->codec_flags_0 = p[48];
			out->vlc_table_id = p[49];
			out->ext_offset = ReadLE32(p + 50);
			out->codec_flags_1 = p[54];
			out->layer_id = p[55];
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
			s.LE32(e.file_offset);
			s.LE64(e.dts);
		}

		bool DecodeIndexEntry(const uint8_t* p, size_t n, IndexEntry* out)
		{
			if (p == nullptr || out == nullptr || n < kIndexEntrySize) return false;
			*out = IndexEntry {};
			out->stream_id = p[0];
			out->is_keyframe = (p[1] & 1u) != 0;
			out->file_offset = ReadLE32(p + 4);
			out->dts = ReadLE64(p + 8);
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
			if (n > 0xFFFFu) return 0;
			if (n != 0 && data == nullptr) return 0;

			const size_t base = out.size();
			Sink s(out);
			s.Bytes(kExtMagic, 4);
			s.U8(ext_type);
			s.U8(ext_version);
			s.LE16(static_cast<uint16_t>(n));
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

			const uint16_t len = ReadLE16(p + 6);
			if (n < kExtBlockFixedPrefix + len + kExtBlockCrcSize) return false;

			const size_t covered = kExtBlockFixedPrefix + len;
			if (ReadLE32(p + covered) != core::Crc32(p, covered)) return false;

			if (ext_type != nullptr) *ext_type = p[4];
			if (ext_version != nullptr) *ext_version = p[5];
			if (next_ext_offset != nullptr) *next_ext_offset = ReadLE64(p + 8);
			if (data != nullptr) data->assign(p + kExtBlockFixedPrefix, p + kExtBlockFixedPrefix + len);
			return true;
		}

		bool HeadValuesOk(const FileHead& h) noexcept
		{
			if (h.version_major != kVersionMajorV1) return false;
			if (h.stream_count == 0) return false;
			if ((h.flags & 0xF8u) != 0) return false;	 // bit3-7 reserved
			const bool streaming = (h.flags & static_cast<uint8_t>(HeadFlagBit::kStreaming)) != 0;
			if (! streaming && h.total_packets == kUnknownPacketCount) return false;
			const bool index_present = (h.flags & static_cast<uint8_t>(HeadFlagBit::kIndexPresent)) != 0;
			if (! index_present && h.index_offset != 0) return false;
			return true;
		}

		bool DescValuesOk(const StreamDesc& d) noexcept
		{
			if (d.stream_type > static_cast<uint8_t>(StreamType::kSubtitle)) return false;
			if (! TimebaseOk(d.timebase_num, d.timebase_den)) return false;
			if (d.first_dts != 0) return false;	   // v1 强制 0
			if (d.max_packet_size == 0) return false;
			if (! BitDepthOk(d.stream_type, d.bit_depth)) return false;
			if (d.layer_id != 0) return false;		   // v1 = 0
			if (d.vlc_table_id != 0) return false;	   // v1 仅允许 0
			if (d.codec_flags_1 != 0) return false;	   // 预留，v1 = 0
			if (d.stream_type == static_cast<uint8_t>(StreamType::kAudio))
			{
				if (d.sample_rate == 0) return false;
				if (d.channels == 0) return false;
			}
			return true;
		}

		int CompareIndexEntries(const IndexEntry& a, const IndexEntry& b,
								const std::vector<StreamDesc*>& desc_by_id) noexcept
		{
			const size_t ida = a.stream_id < desc_by_id.size() ? a.stream_id : 0xFFFFu;
			const size_t idb = b.stream_id < desc_by_id.size() ? b.stream_id : 0xFFFFu;
			const StreamDesc* da = ida == 0xFFFFu ? nullptr : desc_by_id[a.stream_id];
			const StreamDesc* db = idb == 0xFFFFu ? nullptr : desc_by_id[b.stream_id];

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
			if (a.file_offset != b.file_offset) return a.file_offset < b.file_offset ? -1 : 1;
			if (a.dts != b.dts) return a.dts < b.dts ? -1 : 1;
			return 0;
		}

	}	 // namespace container
}	 // namespace clv
