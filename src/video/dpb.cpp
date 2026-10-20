#include "dpb.h"

#include <algorithm>

namespace clv
{
	namespace video
	{

		/* 出帧阈值在构造期算一次，之后每帧只读：
		 * 地板 = `max_b_frames + 2` 是**重排需要的深度**（双向预测的显示序落后于解码序），
		 * 声明的起池深度只能把它往上推 —— 往下推就等于在显示序排好之前吐帧，违反出帧契约。
		 * 0 = 未声明 ⇒ 取地板，与既有行为一字不差。 */
		Scheduler::Scheduler(uint8_t max_b_frames, uint8_t preroll_frames, FrameHolders* holders):
			max_b_frames_(max_b_frames), holders_(holders)
		{
			reorder_floor_ = static_cast<size_t>(max_b_frames) + 2u;
			threshold_ = (static_cast<size_t>(preroll_frames) > reorder_floor_) ? static_cast<size_t>(preroll_frames) :
																				  reorder_floor_;
		}

		size_t Scheduler::DpbCapacity() const noexcept { return kDpbCapacity; }

		bool Scheduler::InDpb(int id) const noexcept
		{
			for (const DpbSlot& s: dpb_)
				if (s.id == id) return true;
			return false;
		}

		bool Scheduler::InQueue(int id) const noexcept
		{
			for (const OutputItem& o: queue_)
				if (o.id == id) return true;
			return false;
		}

		/* 逐出 = 放掉参考窗那一次持有；这帧是否还在输出队列里与逐出无关。
		 * 计数归零时由池那侧把槽位收成空闲位，不在这里做。 */
		void Scheduler::EvictOldestDpb()
		{
			if (dpb_.empty()) return;
			size_t oldest = 0u;
			for (size_t i = 1u; i < dpb_.size(); ++i)
				if (dpb_[i].decode_seq < dpb_[oldest].decode_seq) oldest = i;
			const int id = dpb_[oldest].id;
			dpb_.erase(dpb_.begin() + static_cast<long>(oldest));
			if (holders_ != nullptr) holders_->Release(id);
		}

		std::vector<OutputItem> Scheduler::PushDecoded(int id, uint8_t type, int64_t pts)
		{
			std::vector<OutputItem> emitted;

			if (type != 2u)
			{
				// I / P 入参考缓冲；满了就先驱逐解码序最旧的一帧（驱逐不影响它是否还在输出队列）
				if (dpb_.size() >= DpbCapacity()) EvictOldestDpb();
				dpb_.push_back(DpbSlot { id, next_seq_ });
				if (holders_ != nullptr) holders_->Acquire(id);	   // 参考窗那一次持有
			}
			++next_seq_;

			OutputItem item;
			item.id = id;
			item.type = type;
			item.pts = pts;
			queue_.insert(std::lower_bound(queue_.begin(), queue_.end(), item.pts,
										   [](const OutputItem& o, int64_t p) { return o.pts < p; }),
						  item);
			// 待输出队列不另计一次持有：架上那一次从建帧起就跟着这帧走，弹出交接给待取队列时也不变。

			if (queue_.size() >= threshold_)
			{
				// 弹出 PTS 最小者：只是出输出队列，不驱逐参考帧
				const OutputItem head = queue_.front();
				queue_.erase(queue_.begin());
				emitted.push_back(head);
			}
			return emitted;
		}

		std::vector<OutputItem> Scheduler::Flush()
		{
			std::vector<OutputItem> emitted;
			while (! queue_.empty())
			{
				const OutputItem head = queue_.front();
				queue_.erase(queue_.begin());
				emitted.push_back(head);
			}
			// 流结束：没有后续帧再引用参考缓冲，参考窗那些持有全部放掉。
			if (holders_ != nullptr)
				for (const DpbSlot& s: dpb_) holders_->Release(s.id);
			dpb_.clear();
			return emitted;
		}

		void Scheduler::Reset()
		{
			if (holders_ != nullptr)
				for (const DpbSlot& s: dpb_) holders_->Release(s.id);
			queue_.clear();
			dpb_.clear();
		}

	}	 // namespace video
}	 // namespace clv
