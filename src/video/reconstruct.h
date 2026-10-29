/* reconstruct.h
 *
 * 宏块像素字节 ↔ 帧平面之间的搬运。编码端取字节、解码端回写字节用的是同一套布局规则，
 * 分两处写迟早漂移，故合一处。
 *
 * 布局口径（8-bit、YUV420P、一个宏块 = 16×16 亮度 + 两个 8×8 色度）：
 *   段内顺序 = Y 按行 16 行 × 16 字节 → U 按行 8 行 × 8 字节 → V 同上，共 384 字节。
 *   平面按行距寻址，行距由调用方给出（本库一律 32 字节对齐）。
 *
 * 编解两侧的逐块写入都走 ReconstructBlockToPlane（反量化 + 反变换 + 加预测 + 夹取，出口直接落在平面上）；
 * `ReconstructBlock` 是同一套算式取行距 8 的特例，给只持一块缓冲的调用方（含钉域界的用例）用，不是解码路径。
 * ScatterBlock8x8 只做像素直搬，供 not_coded 宏块整块拷贝，两者的行距寻址是同一套。
 */
#ifndef CLV_VIDEO_RECONSTRUCT_H
#define CLV_VIDEO_RECONSTRUCT_H

#include "quant.h"

#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		/* 一个宏块的边长（像素）。v1 只有 16×16 一种块结构。 */
		inline constexpr uint32_t kMbSize = 16u;

		/* 单帧重建缓冲的像素上界，按补后尺寸的宽 × 高算：越界只在分配之前拒，不钳到界内继续跑。
		 * 依据是三行可核算的账：
		 *   分配路径只有一处 = clv_video.cpp 的 AllocPlanes，一帧三平面合计
		 *     stride0 × coded_h + 2 × (stride0 / 2) × (coded_h / 2) ≈ 1.5 字节 / 像素（4:2:0）；
		 *     行距按 32 字节对齐，每行最多宽出 31 字节，故合计还要加 2 × 31 × coded_h 的余量上界
		 *     （亮度 31 × coded_h，两个半高色度各 31 × coded_h / 2）。
		 *   同时在场的帧缓冲 = 解码 DPB 恒 2 帧（dpb.h 的 kDpbCapacity）+ 编码端 ref / cur 2 帧 = 4 帧。
		 *   32 位构建下 size_t 是 32 位，4 帧 × 1.5 字节 / 像素 必须落在 2 GiB（= 2^31，int32 的正数界）内
		 *     ⇒ 像素上界 = 2^31 / 6 = 357 913 938，向下取 2 的幂 ⇒ 2^28。
		 * 尺寸字段的理论上限是 65 536 × 65 536 = 2^32 像素，本界比它低 16 倍（2^32 / 2^28 = 16）：
		 * 取的是「真能分配出来 + 索引不回绕」的容量，不是字段能写多大的理论值。
		 * 界内时行偏移 `行 × 行距` 也落得进 32 位 size_t，故各处的行偏移算式不必再比一次这条界。 */
		inline constexpr uint64_t kMaxReconstructPixels = 1ull << 28;

		/* 补后宽 × 补后高是否落在 kMaxReconstructPixels 内：**积一律先转 64 位**再比界 ——
		 * 32 位积在 65 536 × 65 536 处正好回绕成 0，比界随之失效。
		 * 界值只在上方的常量定义处出现一次，判定处一律经本函数，不另写积与比较式。 */
		constexpr bool CodedPixelsWithinBound(uint32_t coded_width, uint32_t coded_height) noexcept
		{
			return static_cast<uint64_t>(coded_width) * static_cast<uint64_t>(coded_height) <= kMaxReconstructPixels;
		}

		/* 三平面视图。stride 由库给出，width / height 是**补后尺寸**（coded_*）。
		 * width / height 与 stride 同源于一次分配，而分配之前比过 kMaxReconstructPixels，
		 * 故本视图里的行偏移 `行 × 行距` 在 32 位 size_t 里不回绕。 */
		struct PlaneView
		{
			uint8_t* p[3] = { nullptr, nullptr, nullptr };
			uint32_t stride[3] = { 0u, 0u, 0u };
			uint32_t width = 0u;
			uint32_t height = 0u;
		};

		/* 平面 0 是全分辨率，1 / 2 在两个轴上都是 1/2。 */
		uint32_t PlaneWidth(const PlaneView& v, uint32_t plane) noexcept;
		uint32_t PlaneHeight(const PlaneView& v, uint32_t plane) noexcept;

		/* 宏块内第 idx 块（0..3 亮度、4 = Cb、5 = Cr）在平面内的左上角像素坐标。
		 * 亮度四块按「左上起、行优先」，与合成块模式的 bit5..bit2 同一序。
		 * 这个取法是全库唯一口径：reconstruct / motion / 编码 / 解码四处共用，
		 * 谁改了这里就是改了四处的块地址。 */
		struct BlockGeom
		{
			uint32_t plane;
			uint32_t px;
			uint32_t py;
		};

		BlockGeom MacroblockBlockAt(uint32_t mb_x, uint32_t mb_y, int idx) noexcept;

		/* ---- 块级像素搬运与重建 ---- */

		/* 取平面上一块 8×8 的行主序 64 像素。(px, py) 是该块左上角在平面内的像素坐标。 */
		void GatherBlock8x8(const PlaneView& v, uint32_t plane, uint32_t px, uint32_t py, uint8_t out[kBlockCoefs]);

		/* 把一块 8×8 的行主序 64 像素写回平面。 */
		void ScatterBlock8x8(const PlaneView& v, uint32_t plane, uint32_t px, uint32_t py,
							 const uint8_t in[kBlockCoefs]);

		/* 取平面上一块 8×8 并与同位的预测块相减，结果落 int32 载体（编码端的残差一步）。 */
		void ResidualBlock8x8(const PlaneView& src, uint32_t plane, uint32_t px, uint32_t py,
							  const uint8_t pred[kBlockCoefs], int32_t x[kBlockCoefs]);

		/* 用常量填满一块（帧内宏块的预测块取 kIntraPredLevel8 = 2^(bpp−1)）。 */
		void FillBlock(uint8_t value, uint8_t out[kBlockCoefs]);

		/* 位深的全库单点：v1 只做 8-bit（建句柄处判非 8 即拒解），四级饱和域与帧内预测电平全部由它派生。
		 * 名字后缀是派生结果所属的位深，不是第二处字面量。 */
		inline constexpr uint32_t kBpp = 8u;

		/* 四级饱和域的字面量，**全部由 kBpp 派生**，禁在别处另写像素域或系数域的界：
		 *   kCoefClampMax*  = 反量化后系数域  ±T_max，按块类型两档（见下）
		 *   kResidualClamp* = 反变换残差域    [−2^bpp, 2^bpp − 1]
		 *   kSampleMax8     = 像素域上界      2^bpp − 1
		 * 第三级与第四级**不是同一域**（残差可负、像素不可负），故分开命名 —— 名字不同即防混用。 */

		/* 第二级域按块类型取两档，两档就是变换输入域自己给出的 T_max —— 正变换不归一（T = H X Hᵀ），
		 * 块和上界 = 系数个数 × 变换输入幅度上界：
		 *   帧内 64 × 2^(bpp−1) = 8192（输入 = 像素 − 128 ∈ [−128, 127]）
		 *   帧间 64 × (2^bpp − 1) = 16320（输入 = 源 − 预测 ∈ [−255, 255]）
		 * 与 block.cpp 两张位宽档表同源，且两侧同用一个 `intra` 值取档：帧内 q'=1 档 |QF| ≤ 4095
		 * ⇒ f' = 2×4095+1 = 8191 ≤ 8192，帧间 q'=1 档 |QF| ≤ 8159 ⇒ f' = 16319 ≤ 16320。
		 * **界为什么不是 ±2^(bpp+3)**：那个界只在**归一化**变换下成立（DC = (1/8)ΣΣf ⇒ 8-bit 最坏
		 * |AC| = 2048）。本库变换不归一，DC = 64 × 残差均值，取 ±2^(bpp+3) 等于把残差均值限死在
		 * ±2048 ÷ 64 = ±31.98，而帧内预测电平恒 128 ⇒ 亮度出 [96, 160] 的块全被砍 DC。
		 * **残余的一档不咬不到**：q > 1 时电平集 (2n+1)q − δ 会越过 |T| 至多 1.5q（帧内 q=127 时
		 * f' 可达 8382 > 8192），这类越域值仍被本档夹掉，代价上界 1.5q ÷ 64 个残差级；平块实测
		 * q ≤ 32 全档位精确，q = 64 / 127 的 ±1~±2 落在同一上界内，未分辨出夹取与量化各自的份额。
		 * 越域值现行处置是「一律夹取、不拒解」；换档动的是本条口径自身，不在这里顺手改。 */
		inline constexpr int32_t kCoefClampMaxIntra8 = kBlockCoefs * static_cast<int32_t>(1u << (kBpp - 1u));
		inline constexpr int32_t kCoefClampMaxInter8 = kBlockCoefs * static_cast<int32_t>((1u << kBpp) - 1u);
		inline constexpr int32_t kResidualClampMin8 = -(1 << kBpp);
		inline constexpr int32_t kResidualClampMax8 = (1 << kBpp) - 1;
		inline constexpr int32_t kSampleMax8 = (1 << kBpp) - 1;

		/* 帧内宏块的预测块电平 = 2^(bpp−1)：intra 时 d = 2^(bpp−1) + f，预测块恒为该电平。
		 * 是**格式口径**不是算式常数，散在多处的话改口径要处处同改。 */
		inline constexpr uint8_t kIntraPredLevel8 = static_cast<uint8_t>(1u << (kBpp - 1u));

		/* 一块的重建，编解两侧逐字同式的唯一实现：
		 *   反量化(DequantizeBlock) → 夹 ±T_max（帧内 ±8192 / 帧间 ±16320，按 `intra` 取档）
		 *   → 反变换(InverseHadamard8, int32) → 舍入(RoundResidual, 整数) → 夹 [−2^bpp, 2^bpp − 1]（[−256, 255]）
		 *   → 加预测块 pred → 夹 [0, 2^bpp − 1]（[0, 255]）。
		 * `pred` 帧内恒为 128、帧间是运动补偿预测块（零矢量即取同位块）。v1 只有 bpp = 8。
		 * `intra` 只影响第一级夹取取哪一档，不改其余三级的域。 */
		void ReconstructBlock(const int32_t qf[kBlockCoefs], uint8_t q, bool intra, const uint8_t pred[kBlockCoefs],
							  uint8_t out[kBlockCoefs]);

		/* 同一套四级域算式，出口直接落在平面上：八道一行 = 平面里的一行 8 像素，
		 * 不经 `out` 暂存再散写一遍。`ReconstructBlock` 就是本式取行距 8 的特例，不另写一份。 */
		void ReconstructBlockToPlane(const int32_t qf[kBlockCoefs], uint8_t q, bool intra,
									 const uint8_t pred[kBlockCoefs], const PlaneView& dst, uint32_t plane, uint32_t px,
									 uint32_t py);

		/* 整帧拷贝：dst 与 src 尺寸或行距不一致时返回 false。 */
		bool CopyPlane(const PlaneView& dst, const PlaneView& src);

		/* 三平面按黑电平填满：luma = 16，chroma = 128（8-bit 有限范围的取值）。 */
		void FillPlanesBlack(const PlaneView& v);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_RECONSTRUCT_H

