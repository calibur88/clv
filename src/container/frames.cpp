#include "frames.h"

#include <algorithm>
#include <utility>

namespace clv
{
	namespace container
	{

		FrameStats FrameAssembler::Delta(const FrameStats& before, const FrameStats& after) noexcept
		{
			FrameStats d;
			d.frames_emitted = after.frames_emitted - before.frames_emitted;
			d.frames_incomplete = after.frames_incomplete - before.frames_incomplete;
			d.fragment_gaps = after.fragment_gaps - before.fragment_gaps;
			d.duplicate_fragments = after.duplicate_fragments - before.duplicate_fragments;
			d.missing_first_packets = after.missing_first_packets - before.missing_first_packets;
			return d;
		}

		size_t FrameAssembler::Push(const ParsedPacket& p, std::vector<ReassembledFrame>& emitted)
		{
			const size_t before = emitted.size();

			if (! p.is_fragment)
			{
				// 单包帧自成一站：上一帧若还没等到末片，按残帧出帧，不静默吞内容
				if (open_) CloseOpenFrame(false, 0, emitted);

				ReassembledFrame f;
				f.stream_id = stream_id_;
				f.is_keyframe = p.is_keyframe;
				f.dts_delta = p.dts_delta;
				f.pts_delta = p.pts_delta;
				f.payload = p.payload;
				f.fragments_received = 1;
				f.fragments_expected = 1;
				f.complete = true;
				stats_.frames_emitted++;
				emitted.push_back(std::move(f));
				return emitted.size() - before;
			}

			if (p.fragment_index == 0)
			{
				if (open_) CloseOpenFrame(false, 0, emitted);
				open_ = true;
				is_keyframe_ = p.is_keyframe;
				dts_ = p.dts_delta;
				pts_ = p.pts_delta;
				next_expected_ = 1;
			}
			else if (! open_)
			{
				// 帧首包缺失 → 该帧的 dts_delta 无人认领，丢残帧 + warn，时间轴不推进
				stats_.missing_first_packets++;
				return 0;
			}

			// fragments_ 按 index 升序维护，重复检测与插入位置一次二分拿到
			const auto at = std::lower_bound(fragments_.begin(), fragments_.end(), p.fragment_index,
											 [](const Fragment& f, uint64_t i) noexcept { return f.index < i; });
			if (at != fragments_.end() && at->index == p.fragment_index)
			{
				stats_.duplicate_fragments++;
				if (p.is_last_fragment) CloseOpenFrame(true, p.fragment_index + 1, emitted);
				return emitted.size() - before;
			}

			if (p.fragment_index > next_expected_) stats_.fragment_gaps += p.fragment_index - next_expected_;
			if (p.fragment_index + 1 > next_expected_) next_expected_ = p.fragment_index + 1;

			Fragment fr;
			fr.index = p.fragment_index;
			fr.payload = p.payload;
			fragments_.insert(at, std::move(fr));

			if (p.is_last_fragment) CloseOpenFrame(true, p.fragment_index + 1, emitted);

			return emitted.size() - before;
		}

		size_t FrameAssembler::Flush(std::vector<ReassembledFrame>& emitted)
		{
			const size_t before = emitted.size();
			if (open_) CloseOpenFrame(false, 0, emitted);
			return emitted.size() - before;
		}

		void FrameAssembler::CloseOpenFrame(bool got_last, uint64_t expected_total, std::vector<ReassembledFrame>& out)
		{
			if (! open_) return;

			ReassembledFrame f;
			f.stream_id = stream_id_;
			f.is_keyframe = is_keyframe_;
			f.dts_delta = dts_;
			f.pts_delta = pts_;
			f.fragments_received = fragments_.size();
			f.fragments_expected = got_last ? expected_total : 0;

			size_t total = 0;
			for (const Fragment& fr: fragments_) total += fr.payload.size();
			f.payload.reserve(total);
			for (Fragment& fr: fragments_)
			{
				f.payload.insert(f.payload.end(), std::make_move_iterator(fr.payload.begin()),
								 std::make_move_iterator(fr.payload.end()));
			}

			f.complete = got_last && fragments_.size() == expected_total;
			if (got_last && fragments_.size() != expected_total)
			{
				// 末片声明的总片数与实收不符：按片序跳变处置（记丢片 + warn），仍不判整文件损坏
				stats_.fragment_gaps++;
			}

			stats_.frames_emitted++;
			if (! f.complete) stats_.frames_incomplete++;
			out.push_back(std::move(f));

			open_ = false;
			is_keyframe_ = false;
			dts_ = 0;
			pts_ = 0;
			next_expected_ = 0;
			fragments_.clear();
		}

	}	 // namespace container
}	 // namespace clv
