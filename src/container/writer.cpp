#include "writer.h"

#include "../core/crc32.h"
#include "byte_io.h"
#include "timebase.h"

#include <algorithm>
#include <utility>

namespace clv
{
	namespace container
	{

		namespace
		{

			// file_offset 与 ext_offset 线上都是 u32，超出即报错而不是回绕
			constexpr uint64_t kMaxU32Offset = 0xFFFFFFFFull;

			// 扩展块内 next_ext_offset 的偏移：magic 4 + type 1 + version 1 + len 2
			constexpr size_t kExtNextOffset = 8;

			void PatchExtBlock(std::vector<uint8_t>& block, size_t data_len, uint64_t next)
			{
				WriteLE64(block.data() + kExtNextOffset, next);
				const size_t covered = kExtBlockFixedPrefix + data_len;
				WriteLE32(block.data() + covered, core::Crc32(block.data(), covered));
			}

		}	 // namespace

		ContainerWriter::ContainerWriter(ByteSinkIf& sink, const WriterConfig& cfg) noexcept: sink_(&sink), cfg_(cfg) {}

		ContainerWriter::StreamState* ContainerWriter::Find(uint8_t stream_id) noexcept
		{
			for (StreamState& s: streams_)
				if (s.desc.stream_id == stream_id) return &s;
			return nullptr;
		}

		const ContainerWriter::StreamState* ContainerWriter::Find(uint8_t stream_id) const noexcept
		{
			for (const StreamState& s: streams_)
				if (s.desc.stream_id == stream_id) return &s;
			return nullptr;
		}

		ContainerErr ContainerWriter::AddStream(const StreamDesc& d)
		{
			if (phase_ != Phase::kStreams) return ContainerErr::StateError;
			if (! DescValuesOk(d)) return ContainerErr::ValueRange;
			if (streams_.size() >= 255) return ContainerErr::TooLarge;
			if (Find(d.stream_id) != nullptr) return ContainerErr::InvalidArgument;

			StreamState st;
			st.desc = d;
			streams_.push_back(st);
			return ContainerErr::Ok;
		}

		ContainerErr ContainerWriter::AddExtBlock(uint8_t stream_id, uint8_t ext_type, uint8_t ext_version,
												  const uint8_t* data, size_t n)
		{
			if (phase_ != Phase::kStreams) return ContainerErr::StateError;
			if (n > 0xFFFFu) return ContainerErr::TooLarge;
			if (n != 0 && data == nullptr) return ContainerErr::InvalidArgument;
			if (Find(stream_id) == nullptr) return ContainerErr::StreamNotFound;

			ExtItem item;
			item.stream_id = stream_id;
			item.type = ext_type;
			item.version = ext_version;
			if (n != 0) item.data.assign(data, data + n);
			exts_.push_back(std::move(item));
			return ContainerErr::Ok;
		}

		ContainerErr ContainerWriter::BeginPackets()
		{
			if (streams_.empty()) return ContainerErr::InvalidArgument;

			// 布局：文件头 64 | 描述符表 64×N | 扩展块区 | 包区
			const uint64_t ext_area_start = kFileHeadSize + kStreamDescSize * streams_.size();

			// 扩展块按流分组、组内保持插入序；先算好偏移，再写描述符（描述符里要带 ext_offset）
			std::vector<std::vector<uint8_t>> ext_blobs;
			uint64_t block_at = ext_area_start;

			for (StreamState& s: streams_)
			{
				std::vector<const ExtItem*> mine;
				for (const ExtItem& item: exts_)
					if (item.stream_id == s.desc.stream_id) mine.push_back(&item);

				s.desc.ext_offset = 0;
				if (! mine.empty())
				{
					if (block_at > kMaxU32Offset) return ContainerErr::TooLarge;
					s.desc.ext_offset = static_cast<uint32_t>(block_at);
				}

				for (size_t idx = 0; idx < mine.size(); ++idx)
				{
					const ExtItem& item = *mine[idx];
					std::vector<uint8_t> block;
					const size_t made =
						EncodeExtBlock(item.type, item.version, item.data.empty() ? nullptr : item.data.data(),
									   item.data.size(), block);
					if (made == 0) return ContainerErr::InvalidArgument;

					uint64_t next = 0;	  // 组尾即链尾
					if (idx + 1 < mine.size())
					{
						if (block_at + made > kMaxU32Offset) return ContainerErr::TooLarge;
						next = block_at + made;
					}
					PatchExtBlock(block, item.data.size(), next);
					ext_blobs.push_back(std::move(block));
					block_at += made;
				}
			}

			std::vector<uint8_t> desc_area;
			for (const StreamState& s: streams_) EncodeStreamDesc(s.desc, desc_area);

			FileHead head;
			head.version_major = kVersionMajorV1;
			head.version_minor = cfg_.version_minor;
			head.stream_count = static_cast<uint8_t>(streams_.size());
			head.flags = 0;	   // 真值与 total_packets / index_offset 一起在 Finish 回填
			std::vector<uint8_t> head_bytes;
			EncodeFileHead(head, head_bytes);

			if (! sink_->Write(head_bytes.data(), head_bytes.size())) return ContainerErr::IoFailed;
			if (! sink_->Write(desc_area.data(), desc_area.size())) return ContainerErr::IoFailed;
			for (const std::vector<uint8_t>& b: ext_blobs)
				if (! sink_->Write(b.data(), b.size())) return ContainerErr::IoFailed;

			packet_area_start_ = sink_->Tell();
			phase_ = Phase::kPackets;
			return ContainerErr::Ok;
		}

		ContainerErr ContainerWriter::WriteFrame(const FrameInput& in)
		{
			if (phase_ == Phase::kDone) return ContainerErr::StateError;
			if (in.payload_size != 0 && in.payload == nullptr) return ContainerErr::InvalidArgument;
			if (phase_ == Phase::kStreams)
			{
				const ContainerErr e = BeginPackets();
				if (e != ContainerErr::Ok) return e;
			}

			StreamState* s = Find(in.stream_id);
			if (s == nullptr) return ContainerErr::StreamNotFound;
			if (in.payload_size > s->desc.max_packet_size) return ContainerErr::TooLarge;

			if (! s->has_origin)
			{
				s->has_origin = true;
				s->origin = in.dts;	   // 整条流按首帧 dts 平移，读方不做平移
			}
			const int64_t shifted = in.dts - s->origin;
			if (shifted < 0 || shifted < s->last_dts) return ContainerErr::InvalidArgument;

			const uint64_t dts_delta = static_cast<uint64_t>(shifted - s->last_dts);
			const int64_t pts_delta = in.pts - in.dts;	  // 平移量在相减时抵消
			s->last_dts = shifted;
			s->last_pts = in.pts;

			uint64_t limit = s->desc.max_packet_size;
			if (cfg_.fragment_chunk_size != 0 && cfg_.fragment_chunk_size < limit) limit = cfg_.fragment_chunk_size;

			const uint64_t fragments = in.payload_size > limit ? (in.payload_size + limit - 1) / limit : 1;
			const uint64_t first_packet_offset = sink_->Tell();
			if (first_packet_offset > kMaxU32Offset) return ContainerErr::TooLarge;

			PacketFields f;
			f.stream_id = in.stream_id;
			f.is_keyframe = in.is_keyframe;
			f.is_fragment = fragments > 1;
			f.dts_delta = dts_delta;
			f.pts_delta = pts_delta;

			std::vector<uint8_t> bytes;
			uint64_t done = 0;
			for (uint64_t i = 0; i < fragments; ++i)
			{
				const uint64_t left = in.payload_size - done;
				const size_t take = left < limit ? static_cast<size_t>(left) : static_cast<size_t>(limit);
				f.fragment_index = i;
				f.is_last_fragment = fragments > 1 && (i + 1 == fragments);
				bytes.clear();
				if (EncodePacket(f, in.payload + done, take, s->desc.max_packet_size, bytes) == 0)
					return ContainerErr::InvalidArgument;
				if (! sink_->Write(bytes.data(), bytes.size())) return ContainerErr::IoFailed;
				done += take;
				packets_++;
			}
			if (done != in.payload_size) return ContainerErr::InvalidArgument;

			frames_++;
			if (cfg_.index_present)
			{
				IndexEntry e;
				e.stream_id = in.stream_id;
				e.is_keyframe = in.is_keyframe;
				e.file_offset = static_cast<uint32_t>(first_packet_offset);
				e.dts = static_cast<uint64_t>(shifted);
				index_.push_back(e);
			}
			return ContainerErr::Ok;
		}

		ContainerErr ContainerWriter::Finish(WriteSummary* out)
		{
			if (phase_ == Phase::kDone) return ContainerErr::StateError;
			if (phase_ == Phase::kStreams)
			{
				const ContainerErr e = BeginPackets();
				if (e != ContainerErr::Ok) return e;
			}

			uint8_t flags = 0;
			if (cfg_.index_present) flags |= static_cast<uint8_t>(HeadFlagBit::kIndexPresent);
			if (cfg_.globally_sorted) flags |= static_cast<uint8_t>(HeadFlagBit::kGloballySorted);
			if (cfg_.streaming) flags |= static_cast<uint8_t>(HeadFlagBit::kStreaming);

			// duration_ticks：各流末帧已平移 PTS 的最大值，换算到全局 tick；
			// 无索引区时 total_packets 写 0 表示未知，不靠它推断时长
			uint64_t duration = 0;
			if (cfg_.index_present)
			{
				for (const StreamState& s: streams_)
				{
					if (! s.has_origin) continue;
					if (s.last_pts < s.origin) continue;
					const uint64_t shifted_pts = static_cast<uint64_t>(s.last_pts - s.origin);
					uint64_t ticks = 0;
					if (! ToGlobalTicks(shifted_pts, static_cast<uint32_t>(s.desc.timebase_num),
										static_cast<uint32_t>(s.desc.timebase_den), &ticks))
					{
						return ContainerErr::ValueRange;
					}
					if (ticks > duration) duration = ticks;
				}
			}

			uint32_t entries = 0;
			uint64_t index_offset = 0;
			if (cfg_.index_present)
			{
				uint8_t max_id = 0;
				for (const StreamState& s: streams_)
					if (s.desc.stream_id > max_id) max_id = s.desc.stream_id;

				std::vector<StreamDesc*> by_id(static_cast<size_t>(max_id) + 1u, nullptr);
				for (StreamState& s: streams_) by_id[s.desc.stream_id] = &s.desc;

				std::sort(index_.begin(), index_.end(), [&by_id](const IndexEntry& a, const IndexEntry& b) noexcept
						  { return CompareIndexEntries(a, b, by_id) < 0; });

				index_area_start_ = sink_->Tell();
				index_offset = index_area_start_;
				entries = static_cast<uint32_t>(index_.size());

				std::vector<uint8_t> blob;
				blob.reserve(kIndexHeadSize + index_.size() * kIndexEntrySize);
				EncodeIndexHead(entries, blob);
				for (const IndexEntry& e: index_) EncodeIndexEntry(e, blob);
				if (! sink_->Write(blob.data(), blob.size())) return ContainerErr::IoFailed;
			}

			std::vector<uint8_t> tail;
			EncodeFileTail(packets_, tail);
			if (! sink_->Write(tail.data(), tail.size())) return ContainerErr::IoFailed;

			const uint64_t head_packets = cfg_.streaming ? kUnknownPacketCount : packets_;
			const ContainerErr pe = PatchHead(index_offset, head_packets, duration, flags);
			if (pe != ContainerErr::Ok) return pe;

			phase_ = Phase::kDone;
			if (out != nullptr)
			{
				out->packets = packets_;
				out->frames = frames_;
				out->index_entries = entries;
				out->duration_ticks = duration;
				out->file_size = sink_->Tell();
				out->packet_area_start = packet_area_start_;
				out->index_area_start = index_area_start_;
			}
			return ContainerErr::Ok;
		}

		ContainerErr ContainerWriter::PatchHead(uint64_t index_offset, uint64_t total_packets, uint64_t duration_ticks,
												uint8_t flags)
		{
			FileHead head;
			head.version_major = kVersionMajorV1;
			head.version_minor = cfg_.version_minor;
			head.stream_count = static_cast<uint8_t>(streams_.size());
			head.flags = flags;
			head.duration_ticks = duration_ticks;
			head.total_packets = total_packets;
			head.index_offset = index_offset;
			if (! HeadValuesOk(head)) return ContainerErr::ValueRange;

			std::vector<uint8_t> bytes;
			EncodeFileHead(head, bytes);
			if (! sink_->Seek(0)) return ContainerErr::IoFailed;
			return sink_->Write(bytes.data(), bytes.size()) ? ContainerErr::Ok : ContainerErr::IoFailed;
		}

		int64_t ContainerWriter::StreamShift(uint8_t stream_id) const noexcept
		{
			const StreamState* s = Find(stream_id);
			if (s == nullptr || ! s->has_origin) return 0;
			return s->origin;
		}

	}	 // namespace container
}	 // namespace clv
