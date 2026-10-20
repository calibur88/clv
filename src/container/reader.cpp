#include "reader.h"

#include "../core/crc32.h"
#include "../core/log.h"
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

			constexpr size_t kReadChunk = 64u * 1024u;
			constexpr size_t kCompactAt = 1u * 1024u * 1024u;
			/* 扩展块链的跳数上限——读方实现侧护栏，不是格式约束（格式侧只要求链向前、逐块成立）。
			 * 取值依据：一条流的扩展块装的是 CodecPrivate / 字幕样式 / STREAMINFO 副本这类元数据，真实流里以
			 * 个位数计；每块最小 22 B（定长前缀 18 = 4 magic + 1 type + 1 version + 4 ext_len + 8 next，加 crc32 4），
			 * 4096 跳即 22 × 4096 = 90 112 B = 88 KiB 元数据区。走到这个长度仍逐块成立、链还没到头，
			 * 已不是「元数据声明得多」而是链本身不正常，故判 kExtChainTooLong 拒这次打开。
			 * 只这一处定义；改它即改行为，须同步公开头该码的契约句与扩展链长度用例的档位期望。 */
			constexpr uint32_t kMaxExtChainHops = 4096;
			constexpr uint32_t kIndexEntriesCap = 1u << 24;
			/* 单个扩展块载荷的实现侧接受上界，按算式派生、不写魔数：
			 * = 默认读方一次驻留窗口 8 MiB − 扩展块定长前缀 18 − 块尾 CRC 4 = 8 388 586 B。
			 * 立这条是因为 ext_len 扩到 4 字节后，u32 域里任何声明值都会换来同量级的整块分配；
			 * 超过这个界即终止该链并计入块计数：声明长度超出接受域的块同样计入块计数——块计数记的是
			 * 「解析不成立的块数」，含接受域外的那一类。处置口径见 docs/容器规范.md 的扩展块一节。
			 * 它不改变格式对写方的约束，也不构成新的合法取值范围。 */
			constexpr uint64_t kMaxExtPayloadAccept = kDefaultReadWindow - kExtBlockFixedPrefix - kExtBlockCrcSize;
			static_assert(kMaxExtPayloadAccept == 8388586ull, "默认窗口 8 MiB − 定长前缀 18 − CRC 4 = 8 388 586");

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
			// 流式写入的真实包数只落文件尾，这里取回来做独立核对
			ReadFileTail();
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
				if (en.file_offset < area_start_ || en.file_offset >= area_end_)
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
				const bool segmented = (head_.flags & static_cast<uint8_t>(HeadFlagBit::kSegmented)) != 0;
				if (! DescValuesOk(d, segmented) || d.stream_id >= usable_.size() || usable_[d.stream_id]) continue;
				descriptors_.push_back(d);
				usable_[d.stream_id] = true;
			}

			area_start_ = kFileHeadSize + tbl;
			const ContainerErr chain_err = WalkExtChains();
			if (chain_err != ContainerErr::kOk) return chain_err;

			if (Segmented()) ReadSegBlock();	// 段描述块在文件级链里，与逐流链同框不同入口

			/* 视频流的 ext_type = 3 对视频流是必选项：缺该块 ⇒ 这条视频流判不成立，
			 * 从描述符表里摘掉，其余流照常、整文件不判损坏。判点排在走链之后（要先知道链里有什么）、
			 * BuildDescTable 之前（表得按摘完的结果建）——查无该块的流连累不到别的流。 */
			for (size_t i = 0; i < descriptors_.size();)
			{
				const StreamDesc& d = descriptors_[i];
				if (d.stream_type != static_cast<uint8_t>(StreamType::kVideo))
				{
					++i;
					continue;
				}
				bool has_param = false;
				for (const ExtRecord& r: ext_records_)
				{
					if (r.stream_id == d.stream_id && r.type == kExtTypeEncMeta)
					{
						has_param = true;
						break;
					}
				}
				if (has_param)
				{
					++i;
					continue;
				}
				usable_[d.stream_id] = false;
				seg_stats_.video_stream_no_param_block++;
				descriptors_.erase(descriptors_.begin() + static_cast<long>(i));
			}

			BuildDescTable();
			return ContainerErr::kOk;
		}

		void ContainerReader::ReadSegBlock()
		{
			const uint64_t size = source_->Size();
			uint64_t at = head_.file_ext_offset;
			for (uint32_t hop = 0; at != 0 && hop < kMaxExtChainHops; ++hop)
			{
				if (at < area_start_ || at >= size || ! source_->Seek(at))
				{
					seg_stats_.block_missing++;
					return;
				}
				size_t got = 0;
				std::vector<uint8_t> prefix(kExtBlockFixedPrefix);
				if (! source_->Read(prefix.data(), prefix.size(), &got) || got != prefix.size())
				{
					seg_stats_.block_missing++;
					return;
				}
				const uint32_t len = ReadLE32(prefix.data() + kExtLenOffset);
				if (static_cast<uint64_t>(len) > kMaxExtPayloadAccept)
				{
					seg_stats_.block_bad++;	   // 接受域外：不分配，同样计入块计数
					return;
				}
				const size_t whole = kExtBlockFixedPrefix + static_cast<size_t>(len) + kExtBlockCrcSize;
				if (size - at < whole)
				{
					seg_stats_.block_missing++;
					return;
				}
				std::vector<uint8_t> buf(whole);
				std::copy(prefix.begin(), prefix.end(), buf.begin());
				if (! source_->Read(buf.data() + kExtBlockFixedPrefix, whole - kExtBlockFixedPrefix, &got) ||
					got != whole - kExtBlockFixedPrefix)
				{
					seg_stats_.block_missing++;
					return;
				}
				uint8_t type = 0, version = 0;
				uint64_t next = 0;
				std::vector<uint8_t> data;
				if (! DecodeExtBlock(buf.data(), buf.size(), &type, &version, &data, &next))
				{
					seg_stats_.block_bad++;
					return;
				}
				/* 包区起点要一并越过文件级链里的这一块：段描述块与逐流链同属扩展块区，写方把它们都排在包区之前。
				 * 只按逐流链算起点会在段描述块上留出等长的空档，首个包就落不进重同步窗口 —— 载荷一大就整段读不出帧。 */
				const uint64_t block_end = at + whole;
				if (block_end > area_start_) area_start_ = block_end;
				if (type == kExtTypeSegBlock)
				{
					SegBlock sb;
					if (! DecodeSegBlock(data.data(), data.size(), &sb))
					{
						seg_stats_.block_bad++;
						return;
					}
					seg_ = sb;
					seg_have_ = true;
					if ((sb.flags & 4u) != 0) seg_stats_.discontinuities++;	   // bit2 = 与上一段之间声明有间断
					return;													   // 文件级链里只这一块
				}
				at = next;
			}
			seg_stats_.block_missing++;	   // 声明了分段却没在链里走到段描述块
		}

		ContainerErr ContainerReader::WalkExtChains()
		{
			uint64_t tail = area_start_;
			for (const StreamDesc& d: descriptors_)
			{
				uint64_t at = d.ext_offset;
				bool chain_ok = true;
				for (uint32_t hop = 0; at != 0 && hop < kMaxExtChainHops; ++hop)
				{
					if (at < area_start_ || at >= source_->Size())
					{
						chain_ok = false;
						break;
					}
					if (! source_->Seek(at))
					{
						chain_ok = false;
						break;
					}

					size_t got = 0;
					uint8_t prefix[kExtBlockFixedPrefix];
					if (! source_->Read(prefix, sizeof(prefix), &got) || got != sizeof(prefix))
					{
						chain_ok = false;
						break;
					}
					const uint32_t len = ReadLE32(prefix + kExtLenOffset);

					// 接受域判排在分配之前：越界的声明长度不该换来一次同量级的整块分配。
					if (static_cast<uint64_t>(len) > kMaxExtPayloadAccept)
					{
						stats_.ext_blocks_bad++;
						chain_ok = false;
						break;	  // 停链；接受域外也计入块计数，与逐块解析不成立同档
					}

					const size_t whole_size = kExtBlockFixedPrefix + len + kExtBlockCrcSize;
					if (source_->Size() - at < whole_size)
					{
						chain_ok = false;
						break;
					}
					std::vector<uint8_t> whole(whole_size);
					std::copy(prefix, prefix + kExtBlockFixedPrefix, whole.begin());
					if (! source_->Read(whole.data() + kExtBlockFixedPrefix, whole_size - kExtBlockFixedPrefix, &got) ||
						got != whole_size - kExtBlockFixedPrefix)
					{
						chain_ok = false;
						break;
					}

					uint8_t type = 0, version = 0;
					uint64_t next = 0;
					std::vector<uint8_t> data;
					if (! DecodeExtBlock(whole.data(), whole.size(), &type, &version, &data, &next))
					{
						stats_.ext_blocks_bad++;
						chain_ok = false;
						break;	  // 畸形链到此为止，包区起点仍按已确认的 tail 算
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
					if (next != 0 && next <= at)
					{
						chain_ok = false;
						break;	  // 链必须向前，倒退或成环按畸形链停走
					}
					at = next;
				}
				if (at != 0 && chain_ok) return ContainerErr::kExtChainTooLong;
			}
			area_start_ = tail;
			return ContainerErr::kOk;
		}

		bool ContainerReader::StreamUsable(uint8_t stream_id) const noexcept
		{
			return stream_id < usable_.size() && usable_[stream_id];
		}

		void ContainerReader::ReadFileTail()
		{
			const uint64_t size = source_->Size();
			// 从文件末尾倒推 16 字节；包区终点按布局边界规则推定，不用文件尾反推
			if (size < kFileHeadSize + kFileTailSize) return;
			if (! source_->Seek(size - kFileTailSize)) return;

			size_t got = 0;
			uint8_t raw[kFileTailSize];
			if (! source_->Read(raw, sizeof(raw), &got) || got != sizeof(raw)) return;

			uint64_t packets = 0;
			if (! DecodeFileTail(raw, sizeof(raw), &packets)) return;
			tail_seen_ = true;
			tail_total_packets_ = packets;
		}

		uint64_t ContainerReader::TotalPackets() const noexcept
		{
			const bool streaming = (head_.flags & static_cast<uint8_t>(HeadFlagBit::kStreaming)) != 0;
			if (! streaming) return head_.total_packets;
			// 流式写的文件头是未知值，真实包数只落文件尾；尾缺失（截断）时保持未知
			return tail_seen_ ? tail_total_packets_ : kUnknownPacketCount;
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

		bool ContainerReader::SkipOverSizedPacket(uint32_t declared)
		{
			// 只保证读得进的字节：窗口里连 flags 那一字节都没有，或包落在包区之外，都算跳不动
			const uint64_t wire = 4ull + declared;
			if (window_.size() - window_off_ < 6u || ! FlagsFingerprintOk(window_[window_off_ + 5u])) return false;
			if (pos_ + wire > area_end_) return false;

			stats_.dropped_too_large++;
			core::LogWarn("clv container: packet larger than the read window, skipped by its declared size");
			window_.clear();
			window_off_ = 0;
			pos_ += wire;
			if (pos_ >= area_end_) done_ = true;
			return true;
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
				if (ts < kMinTotalSize) continue;							   // 候选包长低于最小 total_size
				if (static_cast<uint64_t>(q) + 4ull + ts > avail) continue;	   // 候选包不出窗口末尾

				const uint8_t sid = at[4];
				if (! Whitelisted(sid)) continue;								   // stream_id 必须是已登记的流
				if (! FlagsFingerprintOk(at[5])) continue;						   // flags 结构指纹不符
				if (4ull + ts > OuterLimitFor(sid)) continue;					   // 候选包长不得超该流的接受上限
				if (ReadLE32(at + ts) != core::Crc32(at + 4, ts - 4)) continue;	   // 候选位置的 CRC 复核

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
				if (declared >= kMinTotalSize && 4ull + declared > config_.window_max && SkipOverSizedPacket(declared))
					continue;
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
						// CRC 与外层结构都成立但流描述符里没有这条流：按不可识别数据丢掉，不影响其余流
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
					// 包边界以 total_size 为准：丢包 + warn + 跳，不重同步
					Consume(static_cast<size_t>(ts) + 4u);
					if (r == PacketParse::kCrcFail)
					{
						stats_.dropped_crc++;
						core::LogWarn("clv container: packet CRC mismatch, packet dropped");
					}
					else
					{
						stats_.dropped_field++;
						core::LogWarn("clv container: packet field area invalid, packet dropped");
					}
					continue;
				}

				if (r == PacketParse::kTooLarge && static_cast<uint64_t>(ts) + 4ull <= avail)
				{
					Consume(static_cast<size_t>(ts) + 4u);
					stats_.dropped_too_large++;
					core::LogWarn("clv container: packet larger than the accepted range, packet dropped");
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

