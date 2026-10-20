/* payload.h
 *
 * 一帧的 VOP 载荷 = VOP 帧头 + 宏块序列。帧头字段与 `mb_count` 在 headers.h，
 * 宏块头与块数据定界在 macroblock.h；这里只负责把两段拼起来、再按 mb_count 拆开。
 *
 * 序列长度必须等于帧头里的 mb_count：少一个宏块判码流不完整，
 * 解满后仍有整字节剩余判码流不成立（不足一字节的是载荷尾的补齐位）。
 *
 * `mb_width` = 本帧宏块列数（编码宽度 ÷ 16）：A（左）/ B（上）/ C（右上）三个候选邻居按它取位，
 * 三型帧都要它，传 0 一律 kInvalidArgument。运动矢量只有帧间宏块带（帧内宏块不带），
 * 其预测器状态跨宏块延续、在每次层函数调用内新建 —— 故本参数参与 I / P 与 B 两侧的判定。
 */
#ifndef CLV_VIDEO_PAYLOAD_H
#define CLV_VIDEO_PAYLOAD_H

#include "headers.h"
#include "macroblock.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		struct VopPayload
		{
			VopHeader head;
			std::vector<Macroblock> macroblocks;	// I / P 帧用
			std::vector<BvopMacroblock> bvops;		// B 帧用
		};

		VideoErr EncodeVopPayload(uint8_t quant_precision, uint32_t mb_width, const VopPayload& in,
								  std::vector<uint8_t>* out);

		/* consumed 是整段载荷的字节数（成功即全部吃掉，尾部的补齐位不足一字节也算在内）。 */
		VideoErr DecodeVopPayload(const uint8_t* data, size_t size, uint8_t quant_precision, uint32_t mb_width,
								  VopPayload* out, size_t* consumed);

		/* ---- 多片：一帧的载荷由 N 个片首尾相接组成（N = 容器外层 fragment_index 的个数）----
		 *
		 * 布局：
		 *   N = 1  → 整帧载荷 = VOP 头 + 宏块序列，**不含片头**（单包帧不带片头）。
		 *   N > 1  → 首片 = 片头 + **完整 VOP 头** + 该片宏块；后续片 = 片头 + 该片宏块。
		 * 片界不另立字段：每片片头带 `mb_run`（该片实际宏块数），解完 `mb_run` 个宏块吃到的字节数就是下一片的起点。
		 * 片界预测链 = **重置**：逐片调用层解码器，每片的状态新建后弃用（B 帧的 MV 中值预测器每片从 0 起）。
		 */
		struct DecodedVop
		{
			VopPayload body;
			uint32_t slice_count = 0;	 // 实收片数，与传入的 fragment_count 相等才有成功
		};

		/* 按 slice_mb_count（每片宏块数，编码器配置，不进码流）切宏块序列并逐片编码。
		 * slice_mb_count = 0 或 ≥ 该帧宏块数 → 只出一部，内容即 EncodeVopPayload 的整帧载荷。
		 * 片数上限 65 535，超了判取值不成立。 */
		VideoErr EncodeVopSlices(uint8_t quant_precision, uint32_t mb_width, uint32_t frame_id, uint64_t slice_mb_count,
								 const VopPayload& in, std::vector<std::vector<uint8_t>>* slices);

		/* 整帧载荷解码入口。fragment_count 来自容器 `CLV_FrameC.fragments_expected`：
		 *   = 0 或 > 65 535 → 取值不成立（一帧至少一片是物理事实，0 = 调用方没收齐就喂）；
		 *   片数与片头声明的 fragment_count 不符、各片 mb_run 累加 ≠ 首片 VOP 头的 mb_count、
		 *   或解完后仍有整字节剩余 → 一律 kValueRange（传错可验，不留 UB）。 */
		VideoErr DecodeVopFrame(const uint8_t* data, size_t size, uint8_t quant_precision, uint32_t mb_width,
								uint64_t fragment_count, DecodedVop* out, size_t* consumed);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_PAYLOAD_H
