/* quant.h
 *
 * v1 的变换与量化：整数 Hadamard 8×8 + 第二法反分段（电平集 (2n+1)q − δ）。变换 / 量化 / 反量化 /
 * 反变换舍入同源于本库唯一实现，编解两侧共用。
 *
 *   正变换  T = H X Hᵀ，H 是 8×8 Sylvester Hadamard（元素 ±1），全程整数、不归一
 *   反变换  Y = H T H ÷ 64（浮点除法，不是整数右移）
 *   电平集  {0} ∪ {±((2n+1)q − δ)}，δ = 0（q 奇）/ 1（q 偶），步长 2q
 *   正向    取最近电平：先以电平 0 作初选，再按 n0−1 / n0 / n0+1 的次序换误差严格更小的候选，
 *           故平局保留已取的候选，电平 0 恒优先于任何非零候选
 *   反向    QF = 0 → 0；否则 |F''| = (2|QF|+1)q − δ，符号照搬
 *   没有 ±q 级 → 死区宽度即首电平的一半
 *   误差界  max|err| ≤ 1.5q − δ/2；等价整数式 2·max|err| ≤ 3q − δ
 */
#ifndef CLV_VIDEO_QUANT_H
#define CLV_VIDEO_QUANT_H

#include <cstddef>
#include <cstdint>

namespace clv
{
	namespace video
	{

		constexpr int kBlockSide = 8;
		constexpr int kBlockCoefs = 64;

		/* quant_precision 的声明域与缺省：声明位是容器 `ext_type = 3` 扩展块，建句柄处逐条判。 */
		inline constexpr uint8_t kQuantPrecisionMin = 3u;
		inline constexpr uint8_t kQuantPrecisionMax = 9u;
		inline constexpr uint8_t kQuantPrecisionDefault = 7u;

		/* q 与 q' 的上界 = 2^quant_precision − 1 的**唯一推导点**：帧头校验、片头校验、编码入口、
		 * 有效步长的夹取一律经本函数取界，不在任何一处另写字面量。
		 * 入参先夹进声明域 [3, 9] 再算 —— 域外的声明在各自的入口判 VALUE_RANGE，本函数不给第二档判定。
		 * 载体收口在 8 位：`vop_quant` 字段本身只有 1 字节，quant_precision ≥ 8 时上界取不满 2^p − 1，
		 * 收口点只有这一处，q' 的加法侧因此恒不会溢出到 8 位载体之外。 */
		constexpr uint8_t QuantCeiling(uint8_t quant_precision) noexcept
		{
			const uint8_t p = (quant_precision < kQuantPrecisionMin) ?
								  kQuantPrecisionMin :
								  (quant_precision > kQuantPrecisionMax ? kQuantPrecisionMax : quant_precision);
			const unsigned raw = (1u << p) - 1u;
			return (raw > static_cast<unsigned>(UINT8_MAX)) ? static_cast<uint8_t>(UINT8_MAX) :
															  static_cast<uint8_t>(raw);
		}

		/* 下界恒为 1，与 quant_precision 无关（q' = 0 没有反量化式）。 */
		inline constexpr uint8_t kQuantMin = 1u;

		/* 无损模式的专用值：量化步长字段里的 0。它不进任何量化式，只作模式信号，
		 * 于是量化步长的合法域分三段：**0 = 无损 / 1..QuantCeiling = 有损 / 其余 = 域外非法**。 */
		inline constexpr uint8_t kQuantLossless = 0u;

		/* 码流侧量化步长域校验的唯一入口，帧头与片头的读写四处共用，不在任何一处另写字面量。
		 * 公开入口不用本式判定：那里要先看编码模式再定 q 允许的取值。 */
		constexpr bool QuantInStreamDomain(uint8_t q, uint8_t quant_precision) noexcept
		{
			return q == kQuantLossless || (q >= kQuantMin && q <= QuantCeiling(quant_precision));
		}

		/* 本流是否按无损直载：量化步长为 0 即成立，两侧都只经本式判断。 */
		constexpr bool IsLosslessQuant(uint8_t q) noexcept { return q == kQuantLossless; }

		void Hadamard8(const int32_t* x, int32_t* t);
		void InverseHadamard8(const int32_t* f2, double* y);

		/* 反变换输出（double）落到整数残差的唯一舍入式：
		 *   f = (Y >= 0) ? floor(Y + 0.5) : ceil(Y - 0.5)     四舍五入、半程离零
		 * Y 恒是「整数 ÷ 64」，64 = 2^6，双精度下精确可表示，故取整不引入二次误差。
		 * 编解两侧逐字同式 —— 任一侧换式（截断、就近偶、查表改写），两侧就不再逐字节全等。 */
		int32_t RoundResidual(double y) noexcept;

		/* q 必须在 [1, QuantCeiling(quant_precision)]；越界时按原样返回不夹（调用方先用有效量化步长）。 */
		int32_t QuantizeValue(int32_t coef, uint8_t q);
		int32_t DequantizeValue(int32_t qf, uint8_t q);

		void QuantizeBlock(const int32_t* t, uint8_t q, int32_t* qf);
		void DequantizeBlock(const int32_t* qf, uint8_t q, int32_t* f2);

		/* 系数域误差界判定：2 * max|t − f2| ≤ 3q − δ。返回是否守住，并带出实测最大误差。 */
		bool CoefficientBoundOk(const int32_t* t, const int32_t* f2, uint8_t q, int32_t* max_err);

		/* 像素域（反变换后）误差界判定：max|err| ≤ 1.5q − δ/2，按浮点式算。 */
		bool PixelBoundOk(const double* diff, uint8_t q, double* max_err);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_QUANT_H

