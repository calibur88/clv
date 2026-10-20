#include "quant.h"

#include <cmath>

namespace clv
{
	namespace video
	{

		namespace
		{

			/* 8 点 Sylvester Hadamard 的原位蝶形：H 对称，所以「按行」与「按列」各跑一遍就是
			 * T = H X Hᵀ 与 Y = H T H。 */
			void fwht8_stride(int32_t* v, int stride)
			{
				for (int len = 1; len < kBlockSide; len <<= 1)
				{
					for (int base = 0; base < kBlockSide; base += len << 1)
					{
						for (int j = 0; j < len; ++j)
						{
							int32_t* pa = v + (base + j) * stride;
							int32_t* pb = v + (base + j + len) * stride;
							const int32_t a = *pa;
							const int32_t b = *pb;
							*pa = a + b;
							*pb = a - b;
						}
					}
				}
			}

			int64_t level_of(int64_t n, uint8_t q, uint8_t delta)
			{
				if (n <= 0) return 0;
				return (2 * n + 1) * static_cast<int64_t>(q) - static_cast<int64_t>(delta);
			}

		}	 // namespace

		void Hadamard8(const int32_t* x, int32_t* t)
		{
			int32_t buf[kBlockCoefs];
			for (int i = 0; i < kBlockCoefs; ++i) buf[i] = x[i];
			for (int c = 0; c < kBlockSide; ++c) fwht8_stride(buf + c, kBlockSide);		   // 按列混合 = H X
			for (int r = 0; r < kBlockSide; ++r) fwht8_stride(buf + r * kBlockSide, 1);	   // 按行混合 = (H X) Hᵀ
			for (int i = 0; i < kBlockCoefs; ++i) t[i] = buf[i];
		}

		void InverseHadamard8(const int32_t* f2, double* y)
		{
			int32_t buf[kBlockCoefs];
			for (int i = 0; i < kBlockCoefs; ++i) buf[i] = f2[i];
			for (int c = 0; c < kBlockSide; ++c) fwht8_stride(buf + c, kBlockSide);
			for (int r = 0; r < kBlockSide; ++r) fwht8_stride(buf + r * kBlockSide, 1);
			for (int i = 0; i < kBlockCoefs; ++i) y[i] = static_cast<double>(buf[i]) / 64.0;
		}

		int32_t RoundResidual(double y) noexcept
		{
			return static_cast<int32_t>((y >= 0.0) ? std::floor(y + 0.5) : std::ceil(y - 0.5));
		}

		int32_t QuantizeValue(int32_t coef, uint8_t q)
		{
			// 无损模式（q = 0）下量化是恒等映射：系数按整数变换结果原值直载，不进电平集。
			// 这里不给「0 折成 1」的退路——折了就把无损请求悄悄编成最细的有损档。
			if (q == kQuantLossless) return coef;
			const uint8_t delta = (q & 1u) ? 0u : 1u;
			const int64_t a = coef < 0 ? -static_cast<int64_t>(coef) : static_cast<int64_t>(coef);
			if (a == 0) return 0;

			// 最近电平的候选起点：n0 = rint((a + δ − q) ÷ 2q)，半程取偶
			const double raw = static_cast<double>(a + static_cast<int64_t>(delta) - static_cast<int64_t>(q)) /
							   static_cast<double>(2 * static_cast<int64_t>(q));
			int64_t n0 = static_cast<int64_t>(std::nearbyint(raw));
			if (n0 < 0) n0 = 0;

			int64_t best_n = 0;
			int64_t best_err = a;	 // 电平 0 的误差
			/* 候选窗 ±1 由误差界 max|err| <= 1.5q - delta/2 保证，不是可调参数；窗宽一改该界即失守。 */
			for (int k = -1; k <= 1; ++k)
			{
				int64_t cand = n0 + k;
				if (cand < 0) cand = 0;
				const int64_t err = (cand <= 0) ? a : std::llabs(a - level_of(cand, q, delta));
				if (err < best_err)	   // 严格小于：平局保留先出现的候选
				{
					best_err = err;
					best_n = cand;
				}
			}
			if (best_n == 0) return 0;
			return coef < 0 ? -static_cast<int32_t>(best_n) : static_cast<int32_t>(best_n);
		}

		int32_t DequantizeValue(int32_t qf, uint8_t q)
		{
			// 无损模式（q = 0）下反量化同样是恒等映射，与量化侧逐字对称
			if (q == kQuantLossless) return qf;
			if (qf == 0) return 0;
			const uint8_t delta = (q & 1u) ? 0u : 1u;
			const int64_t a = qf < 0 ? -static_cast<int64_t>(qf) : static_cast<int64_t>(qf);
			const int64_t mag = (2 * a + 1) * static_cast<int64_t>(q) - static_cast<int64_t>(delta);
			/* 管线内到不了这里：系数受载体位宽与量化输入的夹取域约束（幅值上界 16 383，q ≤ 127
			* ⇒ mag ≤ 4 152 704 < 2^31）。本函数是命名空间口的公开算法件，越界入参不在管线保证之内，
			* 所以按 int32 可表达域夹住，不让高位被截掉后给出一个看着合法的错值。 */
			const int64_t clamped = mag > static_cast<int64_t>(INT32_MAX) ? static_cast<int64_t>(INT32_MAX) : mag;
			const int32_t v = static_cast<int32_t>(clamped);
			return qf < 0 ? -v : v;
		}

		void QuantizeBlock(const int32_t* t, uint8_t q, int32_t* qf)
		{
			for (int i = 0; i < kBlockCoefs; ++i) qf[i] = QuantizeValue(t[i], q);
		}

		void DequantizeBlock(const int32_t* qf, uint8_t q, int32_t* f2)
		{
			for (int i = 0; i < kBlockCoefs; ++i) f2[i] = DequantizeValue(qf[i], q);
		}

		bool CoefficientBoundOk(const int32_t* t, const int32_t* f2, uint8_t q, int32_t* max_err)
		{
			int32_t worst = 0;
			for (int i = 0; i < kBlockCoefs; ++i)
			{
				const int64_t e = std::llabs(static_cast<int64_t>(t[i]) - static_cast<int64_t>(f2[i]));
				if (e > worst) worst = static_cast<int32_t>(e);
			}
			if (max_err != nullptr) *max_err = worst;
			const int64_t delta = (q & 1u) ? 0 : 1;
			// max|err| ≤ 1.5q − δ/2 的等价整数式
			return 2 * static_cast<int64_t>(worst) <= 3 * static_cast<int64_t>(q) - delta;
		}

		bool PixelBoundOk(const double* diff, uint8_t q, double* max_err)
		{
			double worst = 0.0;
			for (int i = 0; i < kBlockCoefs; ++i)
			{
				const double e = std::fabs(diff[i]);
				if (e > worst) worst = e;
			}
			if (max_err != nullptr) *max_err = worst;
			const double delta = (q & 1u) ? 0.0 : 1.0;
			return worst <= 1.5 * static_cast<double>(q) - 0.5 * delta;
		}

	}	 // namespace video
}	 // namespace clv

