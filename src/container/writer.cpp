#include "writer.h"

#include "../core/crc32.h"
#include "byte_io.h"
#include "reader.h"	   // 单包可被读回的上界与外层开销都声明在那里，写侧的早失败判取同一个数
#include "timebase.h"

#include <algorithm>
#include <utility>

namespace clv
{
	namespace container
	{

		namespace
		{

			void PatchExtBlock(std::vector<uint8_t>& block, size_t data_len, uint64_t next)
			{
				WriteLE64(block.data() + kExtNextOffset, next);
				const size_t covered = kExtBlockFixedPrefix + data_len;
				WriteLE32(block.data() + covered, core::Crc32(block.data(), covered));
			}

		}	 // namespace

		ContainerWriter::ContainerWriter(ByteSinkIf& sink, const WriterConfig& cfg) noexcept:
			sink_(&sink), config_(cfg), by_id_(kStreamIdSpace, -1)
		{
		}

		ContainerWriter::StreamState* ContainerWriter::Find(uint8_t stream_id) noexcept
		{
			const int at = by_id_[stream_id];
			return at < 0 ? nullptr : &streams_[static_cast<size_t>(at)];
		}

		const ContainerWriter::StreamState* ContainerWriter::Find(uint8_t stream_id) const noexcept
		{
			const int at = by_id_[stream_id];
			return at < 0 ? nullptr : &streams_[static_cast<size_t>(at)];
		}

		ContainerErr ContainerWriter::AddStream(const StreamDesc& d)
		{
			if (phase_ != Phase::kStreams) return ContainerErr::kStateError;
			if (! DescValuesOk(d, config_.segmented)) return ContainerErr::kValueRange;
			/* 首段的零起点钉只在写方判：段 0 就是整条逻辑序列的开头，该流 first_dts 必须为 0；
			 * 放开它的是分段容器的后续段（不平移 ⇒ first_dts = 本段该流首帧的绝对 dts）。
			 * 读方不补这一道拒档，否则在盘的首段文件会被判成坏。 */
			if (config_.segmented && config_.seg.index == 0u && d.first_dts != 0) return ContainerErr::kValueRange;
			if (streams_.size() >= kStreamIdSpace - 1u) return ContainerErr::kTooLarge;
			if (static_cast<uint64_t>(d.max_packet_size) > kMaxPacketPayload) return ContainerErr::kTooLarge;
			if (by_id_[d.stream_id] >= 0) return ContainerErr::kInvalidArgument;

			by_id_[d.stream_id] = static_cast<int>(streams_.size());
			StreamState st;
			st.desc = d;
			streams_.push_back(st);
			return ContainerErr::kOk;
		}

		ContainerErr ContainerWriter::AddExtBlock(uint8_t stream_id, uint8_t ext_type, uint8_t ext_version,
												  const uint8_t* data, size_t n)
		{
			if (phase_ != Phase::kStreams) return ContainerErr::kStateError;
			if (n > 0xFFFFFFFFull) return ContainerErr::kTooLarge;
			if (n != 0 && data == nullptr) return ContainerErr::kInvalidArgument;
			if (Find(stream_id) == nullptr) return ContainerErr::kStreamNotFound;

			ExtItem item;
			item.stream_id = stream_id;
			item.type = ext_type;
			item.version = ext_version;
			if (n != 0) item.data.assign(data, data + n);
			ext_items_.push_back(std::move(item));
			return ContainerErr::kOk;
		}

		ContainerErr ContainerWriter::BeginPackets()
		{
			if (streams_.empty()) return ContainerErr::kInvalidArgument;

			// 布局：文件头 64 | 描述符表 68×N（`kStreamDescSize`）| 扩展块区 | 包区
			const uint64_t ext_area_start = kFileHeadSize + kStreamDescSize * streams_.size();

			// 扩展块按流分组、组内保持插入序；先算好偏移，再写描述符（描述符里要带 ext_offset）
			std::vector<std::vector<const ExtItem*>> groups(kStreamIdSpace);
			for (const ExtItem& item: ext_items_) groups[item.stream_id].push_back(&item);

			std::vector<std::vector<uint8_t>> ext_blobs;
			uint64_t block_at = ext_area_start;

			for (StreamState& s: streams_)
			{
				const std::vector<const ExtItem*>& mine = groups[s.desc.stream_id];

				s.desc.ext_offset = 0;
				if (! mine.empty()) s.desc.ext_offset = block_at;

				for (size_t idx = 0; idx < mine.size(); ++idx)
				{
					const ExtItem& item = *mine[idx];
					std::vector<uint8_t> block;
					const size_t made =
						EncodeExtBlock(item.type, item.version, item.data.empty() ? nullptr : item.data.data(),
									   item.data.size(), block);
					if (made == 0) return ContainerErr::kInvalidArgument;

					uint64_t next = 0;	  // 组尾即链尾
					if (idx + 1 < mine.size()) next = block_at + made;
					PatchExtBlock(block, item.data.size(), next);
					ext_blobs.push_back(std::move(block));
					block_at += made;
				}
			}

			if (config_.segmented)
			{
				std::vector<uint8_t> payload;
				EncodeSegBlock(config_.seg, payload);
				// 写前先自校：把写出去的载荷丢回读方那个 DecodeSegBlock，过了才落盘
				SegBlock back;
				if (! DecodeSegBlock(payload.data(), payload.size(), &back)) return ContainerErr::kValueRange;

				std::vector<uint8_t> block;
				if (EncodeExtBlock(kExtTypeSegBlock, 1, payload.data(), payload.size(), block) == 0)
					return ContainerErr::kInvalidArgument;
				PatchExtBlock(block, payload.size(), 0);	// 文件级链里只这一块，链尾 = 0
				file_ext_offset_ = block_at;
				ext_blobs.push_back(std::move(block));
				block_at += kExtBlockFixedPrefix + payload.size() + kExtBlockCrcSize;
			}

			std::vector<uint8_t> desc_area;
			for (const StreamState& s: streams_) EncodeStreamDesc(s.desc, desc_area);

			FileHead head;
			head.version_major = kVersionMajorV2;
			head.version_minor = config_.version_minor;
			head.stream_count = static_cast<uint8_t>(streams_.size());
			head.flags = 0;	   // 真值与 total_packets / index_offset 一起在 Finish 回填
			std::vector<uint8_t> head_bytes;
			EncodeFileHead(head, head_bytes);

			if (! sink_->Write(head_bytes.data(), head_bytes.size())) return ContainerErr::kIoFailed;
			if (! sink_->Write(desc_area.data(), desc_area.size())) return ContainerErr::kIoFailed;
			for (const std::vector<uint8_t>& b: ext_blobs)
				if (! sink_->Write(b.data(), b.size())) return ContainerErr::kIoFailed;

			packet_area_start_ = sink_->Tell();
			phase_ = Phase::kPackets;
			return ContainerErr::kOk;
		}

		ContainerErr ContainerWriter::WriteFrame(const FrameInput& in)
		{
			if (phase_ == Phase::kDone) return ContainerErr::kStateError;
			if (in.payload_size != 0 && in.payload == nullptr) return ContainerErr::kInvalidArgument;
			if (phase_ == Phase::kStreams)
			{
				const ContainerErr e = BeginPackets();
				if (e != ContainerErr::kOk) return e;
			}

			StreamState* s = Find(in.stream_id);
			if (s == nullptr) return ContainerErr::kStreamNotFound;
			if (in.payload_size > s->desc.max_packet_size) return ContainerErr::kTooLarge;

			// 片边界由容器按 limit 切：limit 取该流上限与 fragment_chunk_size 里较窄的一个
			uint64_t limit = s->desc.max_packet_size;
			if (config_.fragment_chunk_size != 0 && config_.fragment_chunk_size < limit)
				limit = config_.fragment_chunk_size;
			const uint64_t count = in.payload_size > limit ? (in.payload_size + limit - 1) / limit : 1;

			std::vector<Piece> pieces(static_cast<size_t>(count));
			size_t done = 0;
			for (uint64_t i = 0; i < count; ++i)
			{
				const size_t left = in.payload_size - done;
				const size_t take = left < static_cast<size_t>(limit) ? left : static_cast<size_t>(limit);
				pieces[static_cast<size_t>(i)] = Piece { in.payload + done, take };
				done += take;
			}
			if (done != in.payload_size) return ContainerErr::kInvalidArgument;

			const FrameCall fc { in.stream_id, in.is_keyframe, in.dts, in.pts };
			return EmitFrame(*s, fc, pieces);
		}

		ContainerErr ContainerWriter::WriteFrameChunks(const ChunkFrameInput& in)
		{
			if (phase_ == Phase::kDone) return ContainerErr::kStateError;
			if (in.chunk_count == 0 || in.chunks == nullptr) return ContainerErr::kInvalidArgument;

			if (phase_ == Phase::kStreams)
			{
				const ContainerErr e = BeginPackets();
				if (e != ContainerErr::kOk) return e;
			}

			StreamState* s = Find(in.stream_id);
			if (s == nullptr) return ContainerErr::kStreamNotFound;

			// 逐片校验与合计校验全部先于落盘，出错时一片都不写
			std::vector<Piece> pieces(in.chunk_count);
			uint64_t total = 0;
			for (uint32_t i = 0; i < in.chunk_count; ++i)
			{
				const ChunkInput& c = in.chunks[i];
				if (c.size == 0 || c.data == nullptr) return ContainerErr::kInvalidArgument;
				if (c.size > s->desc.max_packet_size) return ContainerErr::kTooLarge;
				total += c.size;
				if (total < c.size) return ContainerErr::kTooLarge;	   // 合计溢出
				pieces[i] = Piece { c.data, c.size };
			}
			// 一帧（含其全部分片）的载荷合计上界，派生见 writer.h 的 kMaxFramePayload
			if (total > kMaxFramePayload) return ContainerErr::kTooLarge;

			const FrameCall fc { in.stream_id, in.is_keyframe, in.dts, in.pts };
			return EmitFrame(*s, fc, pieces);
		}

		ContainerErr ContainerWriter::AdvanceTimeline(StreamState& s, const FrameCall& fc, uint64_t* dts_delta,
													  int64_t* pts_delta, uint64_t* shifted_out)
		{
			if (! s.has_origin)
			{
				s.has_origin = true;
				/* 平移基准取法：零起点（非分段文件、以及分段容器的首段）按首帧 dts 把整条流拉回 0；
				 * 分段容器的后续段不平移 —— 落盘 dts 就是绝对值，且必须与该流声明的 first_dts 同值
				 * （后续段不平移的口径，见 docs/容器规范.md 的流描述符与段描述块两节；这样条目天然跨段可比较，读方不做减法）。
				 * 两处口径互相依赖：这里与 DescValuesOk 的零起点钉必须同轮改，漏一处就是段 1 起打不开或整段时间轴歪。 */
				const bool absolute_timeline = config_.segmented && s.desc.first_dts != 0;
				if (absolute_timeline && static_cast<uint64_t>(fc.dts) != s.desc.first_dts)
					return ContainerErr::kValueRange;
				s.origin = absolute_timeline ? 0 : fc.dts;
			}
			const int64_t shifted = fc.dts - s.origin;
			if (shifted < 0 || shifted < s.last_dts) return ContainerErr::kInvalidArgument;

			*dts_delta = static_cast<uint64_t>(shifted - s.last_dts);
			*pts_delta = fc.pts - fc.dts;	 // 平移量在相减时抵消
			s.last_dts = shifted;
			s.last_pts = fc.pts;
			*shifted_out = static_cast<uint64_t>(shifted);
			return ContainerErr::kOk;
		}

		/* 一帧的落包动作：时间轴推进 → 逐片一包 → 帧级索引。两条写帧口都汇到这里，
		 * 片边界与片数由调用方给定的 pieces 决定，本函数不再做任何切分。 */
		ContainerErr ContainerWriter::EmitFrame(StreamState& s, const FrameCall& fc, const std::vector<Piece>& pieces)
		{
			uint64_t dts_delta = 0;
			int64_t pts_delta = 0;
			uint64_t shifted = 0;
			const ContainerErr te = AdvanceTimeline(s, fc, &dts_delta, &pts_delta, &shifted);
			if (te != ContainerErr::kOk) return te;

			const uint64_t first_packet_offset = sink_->Tell();

			PacketFields f;
			f.stream_id = fc.stream_id;
			f.is_keyframe = fc.is_keyframe;
			f.is_fragment = pieces.size() > 1;
			f.dts_delta = dts_delta;
			f.pts_delta = pts_delta;

			std::vector<uint8_t> bytes;
			for (size_t i = 0; i < pieces.size(); ++i)
			{
				f.fragment_index = i;
				f.is_last_fragment = pieces.size() > 1 && i + 1 == pieces.size();
				bytes.clear();
				const ContainerErr pe = EmitPacket(f, s.desc.max_packet_size, pieces[i].data, pieces[i].size, bytes);
				if (pe != ContainerErr::kOk) return pe;
			}

			frames_++;
			PushFrameIndex(fc.stream_id, fc.is_keyframe, first_packet_offset, shifted);
			return ContainerErr::kOk;
		}

		ContainerErr ContainerWriter::Finish(WriteSummary* out)
		{
			if (phase_ == Phase::kDone) return ContainerErr::kStateError;
			if (phase_ == Phase::kStreams)
			{
				const ContainerErr e = BeginPackets();
				if (e != ContainerErr::kOk) return e;
			}

			uint8_t flags = 0;
			if (config_.index_present) flags |= static_cast<uint8_t>(HeadFlagBit::kIndexPresent);
			if (config_.globally_sorted) flags |= static_cast<uint8_t>(HeadFlagBit::kGloballySorted);
			if (config_.streaming) flags |= static_cast<uint8_t>(HeadFlagBit::kStreaming);
			if (config_.segmented) flags |= static_cast<uint8_t>(HeadFlagBit::kSegmented);

			// duration_ticks：各流末帧已平移 PTS 的最大值，换算到全局 tick；
			// 无索引区时它写 0 表示未知，不靠它推断时长（包数另按 streaming 决定头里写真实值还是未知值）
			uint64_t duration = 0;
			if (config_.index_present)
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
						return ContainerErr::kValueRange;
					}
					if (ticks > duration) duration = ticks;
				}
			}

			uint32_t entries = 0;
			uint64_t index_offset = 0;
			if (config_.index_present)
			{
				std::vector<StreamDesc*> by_id(kStreamIdSpace, nullptr);
				for (StreamState& s: streams_) by_id[s.desc.stream_id] = &s.desc;

				std::sort(index_.begin(), index_.end(), [&by_id](const IndexEntry& a, const IndexEntry& b) noexcept
						  { return CompareIndexEntries(a, b, by_id) < 0; });

				index_area_start_ = sink_->Tell();
				index_offset = index_area_start_;
				/* 索引头里的 entry_count 是 u32：条目数越出该域即拒这次收尾，**不做静默截断**——
				 * 截断会写出一个自洽但少报条目的索引，读方按声明值走查就永远看不见被截掉的那一段。
				 * 判点排在编码与落盘之前，一片索引字节都不写。 */
				if (index_.size() > 0xFFFFFFFFull) return ContainerErr::kTooLarge;
				entries = static_cast<uint32_t>(index_.size());

				std::vector<uint8_t> blob;
				blob.reserve(kIndexHeadSize + index_.size() * kIndexEntrySize);
				EncodeIndexHead(entries, blob);
				for (const IndexEntry& e: index_) EncodeIndexEntry(e, blob);
				if (! sink_->Write(blob.data(), blob.size())) return ContainerErr::kIoFailed;
			}

			std::vector<uint8_t> tail;
			EncodeFileTail(packets_, tail);
			if (! sink_->Write(tail.data(), tail.size())) return ContainerErr::kIoFailed;

			const uint64_t head_packets = config_.streaming ? kUnknownPacketCount : packets_;
			const ContainerErr pe = PatchHead(index_offset, head_packets, duration, flags);
			if (pe != ContainerErr::kOk) return pe;

			phase_ = Phase::kDone;
			if (out != nullptr)
			{
				out->packets = packets_;
				out->frames = frames_;
				out->index_entries = entries;
				out->duration_ticks = duration;
				out->header_end_offset = sink_->Tell();
				out->packet_area_start = packet_area_start_;
				out->index_area_start = index_area_start_;
			}
			return ContainerErr::kOk;
		}

		ContainerErr ContainerWriter::PatchHead(uint64_t index_offset, uint64_t total_packets, uint64_t duration_ticks,
												uint8_t flags)
		{
			FileHead head;
			head.version_major = kVersionMajorV2;
			head.version_minor = config_.version_minor;
			head.stream_count = static_cast<uint8_t>(streams_.size());
			head.flags = flags;
			head.duration_ticks = duration_ticks;
			head.total_packets = total_packets;
			head.index_offset = index_offset;
			head.file_ext_offset = file_ext_offset_;
			if (! HeadValuesOk(head)) return ContainerErr::kValueRange;

			std::vector<uint8_t> bytes;
			EncodeFileHead(head, bytes);
			if (! sink_->Seek(0)) return ContainerErr::kIoFailed;
			return sink_->Write(bytes.data(), bytes.size()) ? ContainerErr::kOk : ContainerErr::kIoFailed;
		}

		ContainerErr ContainerWriter::EmitPacket(const PacketFields& f, uint32_t max_payload, const uint8_t* data,
												 size_t n, std::vector<uint8_t>& bytes)
		{
			if (EncodePacket(f, data, n, max_payload, bytes) == 0) return ContainerErr::kInvalidArgument;
			if (! sink_->Write(bytes.data(), bytes.size())) return ContainerErr::kIoFailed;
			packets_++;
			return ContainerErr::kOk;
		}

		void ContainerWriter::PushFrameIndex(uint8_t stream_id, bool is_keyframe, uint64_t file_offset,
											 uint64_t shifted_dts)
		{
			if (! config_.index_present) return;
			IndexEntry e;
			e.stream_id = stream_id;
			e.is_keyframe = is_keyframe;
			e.file_offset = file_offset;
			e.dts = shifted_dts;
			index_.push_back(e);
		}

		int64_t ContainerWriter::StreamShift(uint8_t stream_id) const noexcept
		{
			const StreamState* s = Find(stream_id);
			if (s == nullptr || ! s->has_origin) return 0;
			return s->origin;
		}

	}	 // namespace container
}	 // namespace clv
