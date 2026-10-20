/* dpb.h
 *
 * 参考帧管理与输出序。两个队列相互独立，语义不同：
 *   参考缓冲（DPB）：只存非 B 帧，v1 非分层 B 结构下容量恒为 2，与 max_b_frames 无关；
 *                    满且要进新 I/P 时，驱逐解码序最旧的一帧（FIFO 头）并放掉参考窗那一次持有，
 *                    不得按显示序 / POC 驱逐。
 *   待输出队列：按显示序（PTS 升序）排；每解一帧 push，队列长度达到 max_b_frames + 2 时
 *              弹出其中 PTS 最小的一帧；流结束时按 PTS 升序清空。
 *   弹出 ≠ 出参考缓冲，驱逐 ≠ 出输出队列。
 * 持有只有一份账，记在帧对象上（见 `kFrameHoldBytesMeasured` 那段口径）：本类经 `FrameHolders`
 * 这两个口加减，不自建第二张按槽号的计数表 —— 在参考窗里的帧自然计数不为零，不另设参考窗保护分支。
 * seek / 丢包重置：清空两个队列并放掉本类加过的持有。本类不持时间基准 —— 成员只有 max_b_frames、
 *              解码序号与两个队列；DTS / PTS 基准在门面与容器侧。
 */
#ifndef CLV_VIDEO_DPB_H
#define CLV_VIDEO_DPB_H

#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		/* 参考缓冲容量恒 2，与 max_b_frames 无关：v1 非分层 B 只引用解码序上相邻的前后两个 I / P 锚点，
		 * 按 max_b_frames 放大容量只增内存、不增多解得出的帧（max_b_frames 影响的是待输出
		 * 队列的弹出阈值 max_b_frames + 2，不是这里的容量）。后续若引入分层 B，容量与驱逐规则都要重定义。 */
		inline constexpr size_t kDpbCapacity = 2u;

		/* 单帧常驻字节（实测，1920×1080 全 I 帧，含池与对齐开销）：全 I 序列每多持一帧就多付这么多。
		 * 可核算的像素账：亮度 1920 × 1080 = 2 073 600 B，两路色度各 960 × 540 = 518 400 B，
		 * 合计 3 110 400 B；本常量比它高 225 860 B，那部分是帧对象、逐宏块矢量场与分配器对齐余量。 */
		inline constexpr uint64_t kFrameHoldBytesMeasured = 3336260ull;
		/* 按本常量外推一帧不归还的量级：30 帧率一小时 108 000 帧 ⇒ 3336260 × 108000 = 360 316 080 000 B
		 * ≈ 336 GiB。这条算式钉在这里，是为了让「池必须可回收」这件事可以随时复算。 */
		static_assert(kFrameHoldBytesMeasured * 108000ull == 360316080000ull, "一小时不归还的外推算式与常量同源");

		/* 参考缓冲 maxlen：池上界式里的那个 maxlen，与上面的容量同值，只换个用途的名字。 */
		inline constexpr uint64_t kDpbMaxlen = static_cast<uint64_t>(kDpbCapacity);
		static_assert(kDpbMaxlen == static_cast<uint64_t>(kDpbCapacity), "上界式的 maxlen 就是参考缓冲容量");

		/* 帧池常驻量的上界口径（门面那侧的池按这条判，回收实现也按这条验收）：
		 *   占用 ≤（kDpbMaxlen + 调用方未释放数）×（帧像素字节 + 每帧附加开销）+ 基线
		 * 帧像素字节 + 每帧附加开销合并成上面那个实测常量；基线 = 起池阈值那一档的在途帧 + 句柄自身。
		 * 稳态目标 = 参考缓冲 maxlen × 每帧持有字节 + 常数，与解出的帧数无关。
		 * 池饿时（无空闲槽可取）允许槽位数超过 maxlen，超出部分由未归还数封顶。 */

		/* 解码侧参考窗持有的落点：I / P 进参考缓冲加一次，逐出 / 冲刷 / 重置相应放掉。
		 * 计数本身存在帧对象上，本接口只是加减的那只手 —— 这里不再留一份按槽号的账。
		 * 待输出队列与待取队列那一次持有由门面在建帧时加上（架上持有），本类不重复计。 */
		class FrameHolders
		{
			public:

			virtual ~FrameHolders() = default;

			virtual void Acquire(int id) = 0;
			virtual void Release(int id) = 0;
		};

		struct OutputItem
		{
			int id = -1;
			uint8_t type = 0;
			int64_t pts = 0;
		};

		class Scheduler
		{
			public:

			/* `preroll_frames` = 公开参数 `CLV_VideoStreamParamsC::preroll_frames` 的受理值，
			 * 0 = 未声明。阈值只升不降（地板 = `max_b_frames + 2`，重排所需深度），依据见 dpb.cpp 构造处。 */
			Scheduler(uint8_t max_b_frames, uint8_t preroll_frames, FrameHolders* holders);

			/* 参考缓冲容量：v1 恒 2（见文件头 `kDpbCapacity`）。 */
			size_t DpbCapacity() const noexcept;

			/* 解出一帧后登记。返回本次离开待输出队列的帧（0 或 1 个）。
			 * type 取载荷层的帧型编码：0 = I，1 = P，2 = B。 */
			std::vector<OutputItem> PushDecoded(int id, uint8_t type, int64_t pts);

			/* 流结束：把队列里剩下的按 PTS 升序全部放出，并释放参考缓冲——
			 * 没有后续帧再引用它们，两个队列一起放空。 */
			std::vector<OutputItem> Flush();

			/* seek / 丢包重置：两个队列一起清，本类加过的持有相应放掉。 */
			void Reset();

			size_t DpbCount() const noexcept { return dpb_.size(); }

			size_t QueueCount() const noexcept { return queue_.size(); }

			/* 受理后的出帧阈值与重排地板：给用例钉「只升不降」这一条，不参与解码路径。 */
			size_t Threshold() const noexcept { return threshold_; }

			size_t ReorderFloor() const noexcept { return reorder_floor_; }

			bool InDpb(int id) const noexcept;
			bool InQueue(int id) const noexcept;

			private:

			struct DpbSlot
			{
				int id = -1;
				uint64_t decode_seq = 0u;
			};

			void EvictOldestDpb();

			uint8_t max_b_frames_;
			FrameHolders* holders_;
			/* 重排地板与受理后的出帧阈值：构造期算一次，`PushDecoded` 只读不重算。 */
			size_t reorder_floor_ = 2u;
			size_t threshold_ = 2u;
			uint64_t next_seq_ = 1u;
			std::vector<DpbSlot> dpb_;
			std::vector<OutputItem> queue_;	   // 始终按 PTS 升序
		};

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_DPB_H

