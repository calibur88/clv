/* motion.h
 *
 * 运动补偿：参考取样与半像素插值，含 B 帧 direct 的两向合成与每宏块单矢量的粒度换算。
 *
 * 这里是编解两侧共用的唯一预测实现：编码器内部重建与解码器都调用这些函数，
 * 各写一份「看着一样」的近似实现即两侧不再逐字节全等。
 *
 * 改动前三条不变量：
 *   ① 同核：亮度与色度共用 b / c / d 三式；差异只在**矢量换算**（round_half）与**块尺寸**（16 / 8）。
 *      禁写第二套插值核 —— 亮度与色度一旦分核，两侧取值再也对不上。
 *   ② 两域取整不互串：矢量域（direct 缩放）走向零截断的整数除法，见 direct_scale.h，
 *      本文件只在**像素域**做带偏置右移（操作数恒非负）。
 *   ③ 粒度：每个帧间宏块只带 1 个矢量，故这里的入口都是「宏块坐标 + 亮度矢量」。
 */
#ifndef CLV_VIDEO_MOTION_H
#define CLV_VIDEO_MOTION_H

#include "block.h"
#include "direct_scale.h"
#include "macroblock.h"
#include "reconstruct.h"

#include <cstdint>

namespace clv
{
	namespace video
	{

		/* 色度矢量换算 round_half(v) = (v + 1) >> 1。
		 * 取值是 floor((v + 1) / 2)，负数也是向下取整（v = -3 → -1，v = -4 → -2）。
		 * 这里用整除式写而不用 `>>`：右移负数在标准里是实现定义（MSVC 恰好是算术右移同义，
		 * 换编译器不保证），而同一条矢量换算两侧必须逐 bit 一致。 */
		int32_t RoundHalf(int32_t v) noexcept;

		MotionVector ChromaMotionVector(MotionVector luma) noexcept;

		/* 边缘复制取样：坐标越出该平面边界时取最外像素（上、下、左、右各复制一格的复制带就是这个取法，
		 * 带内取实际像素、带外取最外像素；色度平面按自身分辨率）。
		 * 复制带属参考缓冲的**取数口径**，不落缓冲、不进码流、裁回显示尺寸时也不带；
		 * 矢量本身一律不裁剪 —— 越界由本函数按上式吸收。
		 * 覆盖域：入参是 int64 坐标，矢量分量取满 32 位有符号域也落进「带外取最外像素」这一支，
		 * 故本函数对全部合法矢量都有定义，不需要再给复制带的物理宽度。 */
		uint8_t SampleReplicated(const PlaneView& v, uint32_t plane, int64_t x, int64_t y) noexcept;

		/* 取一块 8×8 的运动补偿预测。
		 * `mv` 用**该平面自己的半像素单位**（偶值整像素、奇值半像素）；(px, py) 是块左上角的整像素坐标。
		 * 取样坐标 = 整像素坐标 ×2 再叠矢量，全程 int64，故 mv 取到 32 位有符号全域都不溢出。
		 * mv = 0 时逐像素等于 GatherBlock8x8 —— 零运动是本式的退化情形，不需要另一条通路。 */
		void PredictBlock8x8(const PlaneView& ref, uint32_t plane, uint32_t px, uint32_t py, MotionVector mv,
							 uint8_t out[kBlockCoefs]);

		/* 双向平均：P = (Pf + Pb + 1) >> 1。操作数恒非负，故直写右移（像素域）。 */
		uint8_t BidirectionalAverage(uint8_t fwd, uint8_t back) noexcept;

		/* 一整套六块的双向合成本体：out[idx][i] = BidirectionalAverage(pf[idx][i], pb[idx][i])。
		 * direct 与 interpolate 两条路径都走这里 —— 平均式在两处各写一遍迟早漂移。
		 * 形参取「指向一行 64 系数」的指针，故实参既可以是数组本身也可以是已退化的形参。 */
		void BlendMacroblocks(const uint8_t (*pf)[kBlockCoefs], const uint8_t (*pb)[kBlockCoefs],
							  uint8_t (*out)[kBlockCoefs]) noexcept;

		/* 一个宏块的六块预测（0..3 亮度、4 = Cb、5 = Cr）。
		 * 亮度取 (mb_x*16 + qx*8, mb_y*16 + qy*8)，色度取 (mb_x*8, mb_y*8) 并用换算后的矢量，尺寸 8×8。 */
		void PredictMacroblock(const PlaneView& ref, uint32_t mb_x, uint32_t mb_y, MotionVector mv,
							   uint8_t out[kBlocksPerMacroblock][kBlockCoefs]);

		/* B 宏块的双向合成：两向各按自己的矢量取一套六块，逐像素双向平均。
		 * `mv` 是已算好的前向 / 后向矢量（由 direct_scale 的 DirectScale 给出，矢量域的舍入已经完成）。
		 * 任一侧缺席返回 kInvalidArgument —— B 帧缺任一参考在更上一层就整帧丢弃，本函数不代判。 */
		VideoErr PredictDirectMacroblock(const PlaneView* fwd, const PlaneView* back, uint32_t mb_x, uint32_t mb_y,
										 const DirectScaled& mv, uint8_t out[kBlocksPerMacroblock][kBlockCoefs]);

		/* 运动估计（**只有编码侧用**）。
		 * 代价 = 亮度四块的隔点取样 SAD（每块 16 点，共 64 点），搜索窗 = 整像素 ±kSearchRadiusPel 格
		 * 再叠一层半像素精修（±1 半像素）；候选序固定、只在严格更小时换主，故结果是确定的。
		 * **平票取零**：零矢量的代价在趟首先取，之后的候选打平都换不掉它，
		 * 所以平场与周期图样落在零运动而不是窗口左上角那一格。
		 * 矢量本身进码流，解码侧只读不算，故这里的规则改动只影响码率与质量，不影响编解逐字节全等。 */
		MotionVector EstimateMotion(const PlaneView& src, const PlaneView& ref, uint32_t mb_x, uint32_t mb_y) noexcept;

		/* 搜索窗半径（**整像素**格数）。单宏块开销 ≈ (2R+1)^2 × 64 + 8 × 64 次取样，
		 * 控制它就是控制编码耗时（65 535 片的满片档对它敏感）。 */
		inline constexpr int32_t kSearchRadiusPel = 4;

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_MOTION_H

