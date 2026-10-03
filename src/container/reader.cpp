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

		ContainerReader::ContainerReader(ByteSourceIf& src, const ReaderConfig& cfg) noexcept:
			source_(&src), config_(cfg)
		{
		}

		const StreamDesc* ContainerReader::DescOf(uint8_t stream_id) const noexcept
		{
			return stream_id < desc_by_id_.size() ? desc_by_id_[stream_id] : nullptr;
		}

		void ContainerReader::BuildDescTable() noexcept
		{
			desc_by_id_.assign(kStreamIdSpace, nullptr);
			for (StreamDesc& d: descriptors_) desc_by_id_[d.stream_id] = &d;
		}

		ContainerErr ContainerReader::Open()
		{
			const ContainerErr e = ReadStructures();
			if (e != ContainerErr::kOk) return e;

			const uint64_t size = source_->Size();
			const bool index_present = (head_.flags & static_cast<uint8_t>(HeadFlagBit::kIndexPresent)) != 0;
			area_end_ = index_present ? head_.index_offset : (size >= kFileTailSize ? size - kFileTailSize : size);
			if (area_end_ < area_start_) area_end_ = area_start_;

			// 解析从包区起点开始：文件头与描述符表不是包，否则第一刀就会把结构当包解，
			// 白跑一次重同步
			pos_ = area_start_;
			window_off_ = 0;
			window_.clear();

			if (index_present) ReadIndex();
			return ContainerErr::kOk;
		}

		void ContainerReader::ReadIndex()
		{
			const uint64_t room = head_.index_offset > area_start_ ? head_.index_offset - area_start_ : 0;
			if (! source_->Seek(head_.index_offset)) return;

			size_t got = 0;
			uint8_t ih[kIndexHeadSize];
			if (! source_->Read(ih, kIndexHeadSize, &got) || got != kIndexHeadSize) return;

			uint32_t count = 0;
			if (! DecodeIndexHead(ih, kIndexHeadSize, &count)) return;
			if (count > room / kIndexEntrySize || count > kIndexEntriesCap) return;

			std::vector<uint8_t> blob(static_cast<size_t>(count) * kIndexEntrySize);
			if (! blob.empty() && (! source_->Read(blob.data(), blob.size(), &got) || got != blob.size())) return;

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
				if (CompareIndexEntries(index_[i - 1], index_[i], desc_by_id_) > 0) ok = false;

			for (size_t i = 0; i < index_.size() && ok; ++i)
			{
				const IndexEntry& en = index_[i];
				const StreamDesc* d = DescOf(en.stream_id);
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
			const uint64_t size = source_->Size();
			if (size < kFileHeadSize + kFileTailSize) return ContainerErr::kBadStructure;
			if (! source_->Seek(0)) return ContainerErr::kIoFailed;

			size_t got = 0;
			std::vector<uint8_t> head(kFileHeadSize);
			if (! source_->Read(head.data(), head.size(), &got) || got != head.size()) return ContainerErr::kIoFailed;

			FileHead h;
			if (! DecodeFileHead(head.data(), head.size(), &h)) return ContainerErr::kBadStructure;
			if (! HeadValuesOk(h)) return ContainerErr::kValueRange;
			head_ = h;

			const size_t tbl = static_cast<size_t>(h.stream_count) * kStreamDescSize;
			std::vector<uint8_t> bytes(tbl);
			if (! source_->Read(bytes.data(), bytes.size(), &got) || got != bytes.size())
				return ContainerErr::kIoFailed;

			usable_.assign(kStreamIdSpace, false);
			for (size_t i = 0; i < h.stream_count; ++i)
			{
				StreamDesc d;
				if (! DecodeStreamDesc(bytes.data() + i * kStreamDescSize, kStreamDescSize, &d)) continue;
				if (! DescValuesOk(d) || d.stream_id >= usable_.size() || usable_[d.stream_id]) continue;
				descriptors_.push_back(d);
				usable_[d.stream_id] = true;
			}

			area_start_ = kFileHeadSize + tbl;
			BuildDescTable();
			WalkExtChains();
			return ContainerErr::kOk;
		}

		void ContainerReader::WalkExtChains()
		{
			uint64_t tail = area_start_;
			for (const StreamDesc& d: descriptors_)
			{
				uint64_t at = d.ext_offset;
				for (uint32_t hop = 0; at != 0 && hop < kExtChainCap; ++hop)
				{
					if (at < area_start_ || at >= source_->Size()) return;
					if (! source_->Seek(at)) return;

					size_t got = 0;
					uint8_t prefix[kExtBlockFixedPrefix];
					if (! source_->Read(prefix, sizeof(prefix), &got) || got != sizeof(prefix)) return;
					const uint16_t len = ReadLE16(prefix + 6);

					const size_t whole_size = kExtBlockFixedPrefix + len + kExtBlockCrcSize;
					if (source_->Size() - at < whole_size) return;
					std::vector<uint8_t> whole(whole_size);
					std::copy(prefix, prefix + kExtBlockFixedPrefix, whole.begin());
					if (! source_->Read(whole.data() + kExtBlockFixedPrefix, whole_size - kExtBlockFixedPrefix, &got) ||
						got != whole_size - kExtBlockFixedPrefix)
					{
						return;
					}

					uint8_t type = 0, version = 0;
					uint64_t next = 0;
					std::vector<uint8_t> data;
					if (! DecodeExtBlock(whole.data(), whole.size(), &type, &version, &data, &next))
					{
						stats_.ext_blocks_bad++;
						break;	  // 本链到此为止，包区起点仍按已确认的 tail 算
					}

					ExtRecord rec;
					rec.stream_id = d.stream_id;
					rec.type = type;
					rec.version = version;
					rec.offset = at;
					rec.data = std::move(data);
					ext_records_.push_back(std::move(rec));

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
			if (config_.runtime_packet_limit != 0) return config_.runtime_packet_limit;
			uint64_t widest = 0;
			for (const StreamDesc& d: descriptors_)
				if (static_cast<uint64_t>(d.max_packet_size) > widest) widest = d.max_packet_size;
			return widest + kOuterOverhead;
		}

		uint64_t ContainerReader::OuterLimitFor(uint8_t stream_id) const noexcept
		{
			if (config_.runtime_packet_limit != 0) return config_.runtime_packet_limit;
			const StreamDesc* d = DescOf(stream_id);
			if (d == nullptr) return OuterLimit();
			return static_cast<uint64_t>(d->max_packet_size) + kOuterOverhead;
		}

		bool ContainerReader::Ensure(size_t want)
		{
			if (want > config_.window_max) want = static_cast<size_t>(config_.window_max);
			if (window_off_ >= kCompactAt)
			{
				window_.erase(window_.begin(), window_.begin() + window_off_);
				window_off_ = 0;
			}
			if (window_.size() - window_off_ >= want) return true;

			const uint64_t area_left = area_end_ > pos_ ? area_end_ - pos_ : 0;
			size_t target = want > area_left ? static_cast<size_t>(area_left) : want;
			if (target > config_.window_max) target = static_cast<size_t>(config_.window_max);

			while (window_.size() - window_off_ < target)
			{
				if (! source_->Seek(pos_ + (window_.size() - window_off_))) return false;
				const size_t chunk = std::min(kReadChunk, target - (window_.size() - window_off_));
				const size_t before = window_.size();
				window_.resize(before + chunk);
				size_t got = 0;
				if (! source_->Read(window_.data() + before, chunk, &got))
				{
					window_.resize(before);
					return false;
				}
				window_.resize(before + got);
				if (got == 0) return false;	   // 到包区末尾
			}
			return window_.size() - window_off_ >= want;
		}

		void ContainerReader::Consume(size_t n)
		{
			if (n > window_.size() - window_off_) n = window_.size() - window_off_;
			window_off_ += n;
			pos_ += n;
			if (window_off_ >= kCompactAt || window_off_ == window_.size())
			{
				window_.erase(window_.begin(), window_.begin() + window_off_);
				window_off_ = 0;
			}
		}

		bool ContainerReader::Resync(uint64_t* found)
		{
			stats_.resync_count++;
			if (found == nullptr) return false;

			Ensure(static_cast<size_t>(std::min<uint64_t>(kResyncWindow, config_.window_max)));

			const size_t avail = window_.size() - window_off_;
			for (size_t q = 1; q + 4 <= avail; ++q)
			{
				const uint8_t* at = window_.data() + window_off_ + q;
				stats_.resync_bytes_scanned++;

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

		void ContainerReader::Absorb(const FrameStats& d) noexcept
		{
			stats_.frames_ok += d.frames_emitted;
			stats_.frames_incomplete += d.frames_incomplete;
			stats_.fragment_gaps += d.fragment_gaps;
			stats_.duplicate_fragments += d.duplicate_fragments;
			stats_.missing_first_packets += d.missing_first_packets;
		}

		void ContainerReader::FeedFrames(const ParsedPacket& p)
		{
			if (p.stream_id >= assemblers_.size()) assemblers_.resize(static_cast<size_t>(p.stream_id) + 1u);
			std::unique_ptr<FrameAssembler>& slot = assemblers_[p.stream_id];
			if (slot == nullptr) slot = std::make_unique<FrameAssembler>(p.stream_id);

			const FrameStats before = slot->Stats();
			slot->Push(p, pending_);
			Absorb(FrameAssembler::Delta(before, slot->Stats()));
		}

		void ContainerReader::DrainAssemblers(bool flush_all)
		{
			if (! flush_all) return;
			for (std::unique_ptr<FrameAssembler>& a: assemblers_)
			{
				if (a == nullptr) continue;
				const FrameStats before = a->Stats();
				a->Flush(pending_);
				Absorb(FrameAssembler::Delta(before, a->Stats()));
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
					stats_.truncated = true;
					done_ = true;
					continue;
				}

				// 先把整包字节读进窗口：只保证 4 字节会把「包太大」误判成读不到边界而走重同步
				const uint32_t declared = ReadLE32(window_.data() + window_off_);
				if (declared >= kMinTotalSize && 4ull + declared <= area_end_ - pos_)
					Ensure(static_cast<size_t>(std::min<uint64_t>(4ull + declared, config_.window_max)));

				const size_t avail = window_.size() - window_off_;
				ParsedPacket p;
				const PacketParse r = DecodePacket(window_.data() + window_off_, avail, OuterLimit(), &p);

				if (r == PacketParse::kOk)
				{
					const size_t wire = p.WireSize();
					const uint64_t payload_size = p.payload_size;
					Consume(wire);
					stats_.packets_ok++;

					if (! Whitelisted(p.stream_id))
					{
						// CRC 自证成立但流描述符里没有这条流：按不可识别数据丢掉，不影响其余流
						stats_.dropped_unknown_stream++;
						continue;
					}
					const StreamDesc* d = DescOf(p.stream_id);
					if (d != nullptr && payload_size > static_cast<uint64_t>(d->max_packet_size))
					{
						stats_.dropped_too_large++;
						continue;
					}
					FeedFrames(p);
					continue;
				}

				if (r == PacketParse::kTruncated)
				{
					stats_.truncated = true;
					done_ = true;
					continue;
				}

				const uint32_t ts = ReadLE32(window_.data() + window_off_);
				if (r == PacketParse::kCrcFail || r == PacketParse::kFieldFail)
				{
					// 包边界由 total_size 自证：丢包 + warn + 跳，不重同步
					Consume(static_cast<size_t>(ts) + 4u);
					if (r == PacketParse::kCrcFail) stats_.dropped_crc++;
					else stats_.dropped_field++;
					continue;
				}

				if (r == PacketParse::kTooLarge && static_cast<uint64_t>(ts) + 4ull <= avail)
				{
					Consume(static_cast<size_t>(ts) + 4u);
					stats_.dropped_too_large++;
					continue;
				}

				// 到这儿只剩两种：包头不成立，或超限且跳不动
				uint64_t found = 0;
				if (! Resync(&found))
				{
					stats_.resync_failed++;
					stats_.abandoned = true;
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
