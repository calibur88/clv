#include "frames.h"

#include <algorithm>
#include <utility>

namespace clv
{
	namespace container
	{

		size_t FrameAssembler::Push(const ParsedPacket& p, std::vector<ReassembledFrame>& emitted)
		{
			const size_t before = emitted.size();

			if (! p.is_fragment)
			{
				// 单包帧自成一站：上一帧若还没等到末片，按残帧出帧，不静默吞内容
				if (open_) CloseOpenFrame(false, 0, emitted);

				ReassembledFrame f;
				f.stream_id = sid_;
				f.is_keyframe = p.is_keyframe;
				f.dts_delta = p.dts_delta;
				f.pts_delta = p.pts_delta;
				f.payload = p.payload;
				f.fragments_received = 1;
				f.fragments_expected = 1;
				f.complete = true;
				st_.frames_emitted++;
				emitted.push_back(std::move(f));
				return emitted.size() - before;
			}

			if (p.fragment_index == 0)
			{
				if (open_) CloseOpenFrame(false, 0, emitted);
				open_ = true;
				key_ = p.is_keyframe;
				dts_ = p.dts_delta;
				pts_ = p.pts_delta;
				next_expected_ = 1;
			}
			else if (! open_)
			{
				// 帧首包缺失 → 该帧的 dts_delta 无人认领，丢残帧 + warn，时间轴不推进
				st_.missing_first_packets++;
				return 0;
			}

			for (const Fragment& f: frags_)
			{
				if (f.index == p.fragment_index)
				{
					st_.duplicate_fragments++;
					if (p.is_last_fragment) CloseOpenFrame(true, p.fragment_index + 1, emitted);
					return emitted.size() - before;
				}
			}

			if (p.fragment_index > next_expected_) st_.fragment_gaps += p.fragment_index - next_expected_;
			if (p.fragment_index + 1 > next_expected_) next_expected_ = p.fragment_index + 1;

			Fragment fr;
			fr.index = p.fragment_index;
			fr.payload = p.payload;
			frags_.push_back(std::move(fr));

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

			// 落位：按 fragment_index 排序后拼接，两片互换也能复原
			std::sort(frags_.begin(), frags_.end(),
					  [](const Fragment& a, const Fragment& b) noexcept { return a.index < b.index; });

			ReassembledFrame f;
			f.stream_id = sid_;
			f.is_keyframe = key_;
			f.dts_delta = dts_;
			f.pts_delta = pts_;
			f.fragments_received = frags_.size();
			f.fragments_expected = got_last ? expected_total : 0;

			size_t total = 0;
			for (const Fragment& fr: frags_) total += fr.payload.size();
			f.payload.reserve(total);
			for (const Fragment& fr: frags_) f.payload.insert(f.payload.end(), fr.payload.begin(), fr.payload.end());

			f.complete = got_last && frags_.size() == expected_total;
			if (got_last && frags_.size() != expected_total)
			{
				// 末片声明的总片数与实收不符：按片序跳变处置（记丢片 + warn），仍不判整文件损坏
				st_.fragment_gaps++;
			}

			st_.frames_emitted++;
			if (! f.complete) st_.frames_incomplete++;
			out.push_back(std::move(f));

			open_ = false;
			key_ = false;
			dts_ = 0;
			pts_ = 0;
			next_expected_ = 0;
			frags_.clear();
		}

	}	 // namespace container
}	 // namespace clv
