#include "reader.h"

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

			// 包头成立所需的最小 total_size；外层最坏开销 41 字节
			constexpr uint32_t kMinTotalSize = 9;
			constexpr uint64_t kOuterOverhead = 41;
			constexpr size_t kReadChunk = 64u * 1024u;
			constexpr size_t kCompactAt = 1u * 1024u * 1024u;
			constexpr uint32_t kExtChainCap = 4096;
			constexpr uint32_t kIndexEntriesCap = 1u << 24;

		}	 // namespace

		ContainerReader::ContainerReader(ByteSourceIf& src, const ReaderConfig& cfg) noexcept: src_(&src), cfg_(cfg) {}

		const StreamDesc* ContainerReader::DescOf(uint8_t stream_id) const noexcept
		{
			for (const StreamDesc& d: descs_)
				if (d.stream_id == stream_id) return &d;
			return nullptr;
		}

		std::vector<StreamDesc*> ContainerReader::DescLookupTable() const
		{
			std::vector<StreamDesc*> tbl(256, nullptr);
			for (const StreamDesc& d: descs_)
				if (d.stream_id < tbl.size()) tbl[d.stream_id] = const_cast<StreamDesc*>(&d);
			return tbl;
		}

		ContainerErr ContainerReader::Open()
		{
			const ContainerErr e = ReadStructures();
			if (e != ContainerErr::Ok) return e;

			const uint64_t size = src_->Size();
			const bool index_present = (head_.flags & static_cast<uint8_t>(HeadFlagBit::kIndexPresent)) != 0;
			area_end_ = index_present ? head_.index_offset : (size >= kFileTailSize ? size - kFileTailSize : size);
			if (area_end_ < area_start_) area_end_ = area_start_;

			// 解析从包区起点开始：文件头与描述符表不是包，否则第一刀就会把结构当包解，
			// 白跑一次重同步
			pos_ = area_start_;
			off_ = 0;
			win_.clear();

			if (index_present) ReadIndex();
			return ContainerErr::Ok;
		}

		void ContainerReader::ReadIndex()
		{
			const uint64_t room = head_.index_offset > area_start_ ? head_.index_offset - area_start_ : 0;
			if (! src_->Seek(head_.index_offset)) return;

			size_t got = 0;
			uint8_t ih[kIndexHeadSize];
			if (! src_->Read(ih, kIndexHeadSize, &got) || got != kIndexHeadSize) return;

			uint32_t count = 0;
			if (! DecodeIndexHead(ih, kIndexHeadSize, &count)) return;
			if (count > room / kIndexEntrySize || count > kIndexEntriesCap) return;

			std::vector<uint8_t> blob(static_cast<size_t>(count) * kIndexEntrySize);
			if (! blob.empty() && (! src_->Read(blob.data(), blob.size(), &got) || got != blob.size())) return;

			const std::vector<StreamDesc*> tbl = DescLookupTable();
			bool ok = true;
			for (uint32_t i = 0; i < count; ++i)
			{
				IndexEntry en;
				if (! DecodeIndexEntry(blob.data() + static_cast<size_t>(i) * kIndexEntrySize, kIndexEntrySize, &en))
				{
					ok = false;
					break;
				}
				if (static_cast<uint64_t>(en.file_offset) < area_start_ ||
					static_cast<uint64_t>(en.file_offset) >= area_end_)
				{
					ok = false;
					break;
				}
				index_.push_back(en);
			}

			// 排序键与写方一致才能拿它做二分；不一致只表示索引不可用，不判文件损坏
			for (size_t i = 1; i < index_.size() && ok; ++i)
				if (CompareIndexEntries(index_[i - 1], index_[i], tbl) > 0) ok = false;

			for (size_t i = 0; i < index_.size() && ok; ++i)
			{
				const IndexEntry& en = index_[i];
				const StreamDesc* d = en.stream_id < tbl.size() ? tbl[en.stream_id] : nullptr;
				uint64_t ticks = en.dts;
				if (d != nullptr && ! ToGlobalTicks(en.dts, static_cast<uint32_t>(d->timebase_num),
													static_cast<uint32_t>(d->timebase_den), &ticks))
				{
					ok = false;
					break;
				}
				entry_ticks_.push_back(ticks);
			}

			if (! ok)
			{
				index_.clear();
				entry_ticks_.clear();
				return;
			}
			index_valid_ = true;
		}

		ContainerErr ContainerReader::ReadStructures()
		{
			const uint64_t size = src_->Size();
			if (size < kFileHeadSize + kFileTailSize) return ContainerErr::BadStructure;
			if (! src_->Seek(0)) return ContainerErr::IoFailed;

			size_t got = 0;
			std::vector<uint8_t> head(kFileHeadSize);
			if (! src_->Read(head.data(), head.size(), &got) || got != head.size()) return ContainerErr::IoFailed;

			FileHead h;
			if (! DecodeFileHead(head.data(), head.size(), &h)) return ContainerErr::BadStructure;
			if (! HeadValuesOk(h)) return ContainerErr::ValueRange;
			head_ = h;

			const size_t tbl = static_cast<size_t>(h.stream_count) * kStreamDescSize;
			std::vector<uint8_t> bytes(tbl);
			if (! src_->Read(bytes.data(), bytes.size(), &got) || got != bytes.size()) return ContainerErr::IoFailed;

			usable_.assign(256, false);
			for (size_t i = 0; i < h.stream_count; ++i)
			{
				StreamDesc d;
				if (! DecodeStreamDesc(bytes.data() + i * kStreamDescSize, kStreamDescSize, &d)) continue;
				if (! DescValuesOk(d) || d.stream_id >= usable_.size() || usable_[d.stream_id]) continue;
				descs_.push_back(d);
				usable_[d.stream_id] = true;
			}

			area_start_ = kFileHeadSize + tbl;
			WalkExtChains();
			return ContainerErr::Ok;
		}

		void ContainerReader::WalkExtChains()
		{
			uint64_t tail = area_start_;
			for (const StreamDesc& d: descs_)
			{
				uint64_t at = d.ext_offset;
				for (uint32_t hop = 0; at != 0 && hop < kExtChainCap; ++hop)
				{
					if (at < area_start_ || at >= src_->Size()) return;
					if (! src_->Seek(at)) return;

					size_t got = 0;
					uint8_t prefix[kExtBlockFixedPrefix];
					if (! src_->Read(prefix, sizeof(prefix), &got) || got != sizeof(prefix)) return;
					const uint16_t len = ReadLE16(prefix + 6);

					const size_t whole_size = kExtBlockFixedPrefix + len + kExtBlockCrcSize;
					if (src_->Size() - at < whole_size) return;
					std::vector<uint8_t> whole(whole_size);
					std::copy(prefix, prefix + kExtBlockFixedPrefix, whole.begin());
					if (! src_->Read(whole.data() + kExtBlockFixedPrefix, whole_size - kExtBlockFixedPrefix, &got) ||
						got != whole_size - kExtBlockFixedPrefix)
					{
						return;
					}

					uint8_t type = 0, version = 0;
					uint64_t next = 0;
					std::vector<uint8_t> data;
					if (! DecodeExtBlock(whole.data(), whole.size(), &type, &version, &data, &next))
					{
						st_.ext_blocks_bad++;
						break;	  // 本链到此为止，包区起点仍按已确认的 tail 算
					}

					ExtRecord rec;
					rec.stream_id = d.stream_id;
					rec.type = type;
					rec.version = version;
					rec.offset = at;
					rec.data = std::move(data);
					exts_.push_back(std::move(rec));

					const uint64_t end = at + whole_size;
					if (end > tail) tail = end;
					if (next != 0 && next <= at) return;	// 链必须向前，防成环
					at = next;
				}
			}
			area_start_ = tail;
		}

		bool ContainerReader::StreamUsable(uint8_t stream_id) const noexcept
		{
			return stream_id < usable_.size() && usable_[stream_id];
		}

		bool ContainerReader::Whitelisted(uint8_t stream_id) const noexcept { return StreamUsable(stream_id); }

		uint64_t ContainerReader::OuterLimit() const noexcept
		{
			if (cfg_.runtime_packet_limit != 0) return cfg_.runtime_packet_limit;
			uint64_t widest = 0;
			for (const StreamDesc& d: descs_)
				if (static_cast<uint64_t>(d.max_packet_size) > widest) widest = d.max_packet_size;
			return widest + kOuterOverhead;
		}

		uint64_t ContainerReader::OuterLimitFor(uint8_t stream_id) const noexcept
		{
			if (cfg_.runtime_packet_limit != 0) return cfg_.runtime_packet_limit;
			const StreamDesc* d = DescOf(stream_id);
			if (d == nullptr) return OuterLimit();
			return static_cast<uint64_t>(d->max_packet_size) + kOuterOverhead;
		}

		bool ContainerReader::Ensure(size_t want)
		{
			if (want > cfg_.window_max) want = static_cast<size_t>(cfg_.window_max);
			if (off_ >= kCompactAt)
			{
				win_.erase(win_.begin(), win_.begin() + off_);
				off_ = 0;
			}
			if (win_.size() - off_ >= want) return true;

			const uint64_t area_left = area_end_ > pos_ ? area_end_ - pos_ : 0;
			size_t target = want > area_left ? static_cast<size_t>(area_left) : want;
			if (target > cfg_.window_max) target = static_cast<size_t>(cfg_.window_max);

			while (win_.size() - off_ < target)
			{
				if (! src_->Seek(pos_ + (win_.size() - off_))) return false;
				const size_t chunk = std::min(kReadChunk, target - (win_.size() - off_));
				const size_t before = win_.size();
				win_.resize(before + chunk);
				size_t got = 0;
				if (! src_->Read(win_.data() + before, chunk, &got))
				{
					win_.resize(before);
					return false;
				}
				win_.resize(before + got);
				if (got == 0) return false;	   // 到包区末尾
			}
			return win_.size() - off_ >= want;
		}

		void ContainerReader::Consume(size_t n)
		{
			if (n > win_.size() - off_) n = win_.size() - off_;
			off_ += n;
			pos_ += n;
			if (off_ >= kCompactAt || off_ == win_.size())
			{
				win_.erase(win_.begin(), win_.begin() + off_);
				off_ = 0;
			}
		}

		bool ContainerReader::Resync(uint64_t* found)
		{
			st_.resync_count++;
			if (found == nullptr) return false;

			Ensure(static_cast<size_t>(std::min<uint64_t>(cfg_.resync_window, cfg_.window_max)));

			const size_t avail = win_.size() - off_;
			for (size_t q = 1; q + 4 <= avail; ++q)
			{
				const uint8_t* at = win_.data() + off_ + q;
				st_.resync_bytes_scanned++;

				const uint32_t ts = ReadLE32(at);
				if (ts < kMinTotalSize) continue;							   // 步 b 下界
				if (static_cast<uint64_t>(q) + 4ull + ts > avail) continue;	   // 步 b：不出包区末尾

				const uint8_t sid = at[4];
				if (! Whitelisted(sid)) continue;								   // 步 c1 白名单
				if (! FlagsFingerprintOk(at[5])) continue;						   // 步 c2 结构指纹
				if (4ull + ts > OuterLimitFor(sid)) continue;					   // 步 b 可选上界
				if (ReadLE32(at + ts) != core::Crc32(at + 4, ts - 4)) continue;	   // 步 d CRC 复核

				*found = q;
				return true;
			}
			return false;
		}

		void ContainerReader::FeedFrames(const ParsedPacket& p)
		{
			if (p.stream_id >= asms_.size()) asms_.resize(static_cast<size_t>(p.stream_id) + 1u);
			std::unique_ptr<FrameAssembler>& slot = asms_[p.stream_id];
			if (slot == nullptr) slot.reset(new FrameAssembler(p.stream_id));

			const FrameStats before = slot->Stats();
			slot->Push(p, pending_);
			const FrameStats& after = slot->Stats();

			st_.frames_ok += after.frames_emitted - before.frames_emitted;
			st_.frames_incomplete += after.frames_incomplete - before.frames_incomplete;
			st_.fragment_gaps += after.fragment_gaps - before.fragment_gaps;
			st_.duplicate_fragments += after.duplicate_fragments - before.duplicate_fragments;
			st_.missing_first_packets += after.missing_first_packets - before.missing_first_packets;
		}

		void ContainerReader::DrainAssemblers(bool flush_all)
		{
			if (! flush_all) return;
			for (std::unique_ptr<FrameAssembler>& a: asms_)
			{
				if (a == nullptr) continue;
				const FrameStats before = a->Stats();
				a->Flush(pending_);
				const FrameStats& after = a->Stats();
				st_.frames_ok += after.frames_emitted - before.frames_emitted;
				st_.frames_incomplete += after.frames_incomplete - before.frames_incomplete;
				st_.fragment_gaps += after.fragment_gaps - before.fragment_gaps;
			}
		}

		bool ContainerReader::NextFrame(ReassembledFrame* out)
		{
			if (out == nullptr) return false;

			for (;;)
			{
				if (! pending_.empty())
				{
					*out = std::move(pending_.front());
					pending_.erase(pending_.begin());
					return true;
				}
				if (done_)
				{
					DrainAssemblers(true);
					if (pending_.empty()) return false;
					continue;
				}

				if (pos_ >= area_end_)
				{
					done_ = true;
					continue;
				}
				if (! Ensure(4))
				{
					st_.truncated = true;
					done_ = true;
					continue;
				}

				// 先把整包字节读进窗口：只保证 4 字节会把「包太大」误判成读不到边界而走重同步
				const uint32_t declared = ReadLE32(win_.data() + off_);
				if (declared >= kMinTotalSize && 4ull + declared <= area_end_ - pos_)
					Ensure(static_cast<size_t>(std::min<uint64_t>(4ull + declared, cfg_.window_max)));

				const size_t avail = win_.size() - off_;
				ParsedPacket p;
				const PacketParse r = DecodePacket(win_.data() + off_, avail, OuterLimit(), &p);

				if (r == PacketParse::kOk)
				{
					const size_t wire = p.WireSize();
					const uint64_t payload_size = p.payload_size;
					Consume(wire);
					st_.packets_ok++;

					if (! Whitelisted(p.stream_id))
					{
						// CRC 自证成立但流描述符里没有这条流：按不可识别数据丢掉，不影响其余流
						st_.dropped_unknown_stream++;
						continue;
					}
					const StreamDesc* d = DescOf(p.stream_id);
					if (d != nullptr && payload_size > static_cast<uint64_t>(d->max_packet_size))
					{
						st_.dropped_too_large++;
						continue;
					}
					FeedFrames(p);
					continue;
				}

				if (r == PacketParse::kTruncated)
				{
					st_.truncated = true;
					done_ = true;
					continue;
				}

				const uint32_t ts = ReadLE32(win_.data() + off_);
				if (r == PacketParse::kCrcFail || r == PacketParse::kFieldFail)
				{
					// 包边界由 total_size 自证：丢包 + warn + 跳，不重同步
					Consume(static_cast<size_t>(ts) + 4u);
					if (r == PacketParse::kCrcFail) st_.dropped_crc++;
					else st_.dropped_field++;
					continue;
				}

				if (r == PacketParse::kTooLarge && static_cast<uint64_t>(ts) + 4ull <= avail)
				{
					Consume(static_cast<size_t>(ts) + 4u);
					st_.dropped_too_large++;
					continue;
				}

				// 到这儿只剩两种：包头不成立，或超限且跳不动
				uint64_t found = 0;
				if (! Resync(&found))
				{
					st_.resync_failed++;
					st_.abandoned = true;
					Consume(avail);	   // 包区是各流共享的单段序列，停不下一条流：游标推到包区末尾
					done_ = true;
					continue;
				}
				Consume(static_cast<size_t>(found));
			}
		}

		size_t ContainerReader::LowerBoundTick(uint64_t target_ticks) const noexcept
		{
			if (! index_valid_) return entry_ticks_.size();
			return static_cast<size_t>(std::lower_bound(entry_ticks_.begin(), entry_ticks_.end(), target_ticks) -
									   entry_ticks_.begin());
		}

	}	 // namespace container
}	 // namespace clv
