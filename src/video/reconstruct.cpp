#include "reconstruct.h"

#include "padding.h"

#include <cstring>

#if defined(__AVX2__)
	#include <immintrin.h>
#endif

namespace clv
{
	namespace video
	{

		namespace
		{

			/* 平面 0 是全分辨率，1 / 2 在两个轴上都是 1/2。 */
			inline uint32_t SubFactor(uint32_t plane) { return plane == 0u ? 1u : 2u; }

#if defined(__AVX2__)
			/* 八道无符号最大值横向归约：先并两个 128 位半区（这一跨只能靠 extract，
			 * shuffle_epi32 的 256 位版本只在各自的 128 位半区内换道，跨不了半区），
			 * 再换 64 位半区、换相邻道，三次取最大后落在道 0。 */
			inline uint32_t MaxU32Across(__m256i m) noexcept
			{
				__m128i x = _mm_max_epu32(_mm256_castsi256_si128(m), _mm256_extracti128_si256(m, 1));
				x = _mm_max_epu32(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(1, 0, 3, 2)));
				x = _mm_max_epu32(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)));
				return static_cast<uint32_t>(_mm_cvtsi128_si32(x));
			}

			/* 一次进出八字节：走 memcpy 而不是 loadl/storel，MSVC 与 GCC 对这两个内建函数的
			 * 形参类型口径不同（__m128i_u / void*），memcpy 两边都成立且都收成一条 movq。 */
			inline __m128i LoadBytes8(const void* p) noexcept
			{
				__m128i v = _mm_setzero_si128();
				std::memcpy(&v, p, 8u);
				return v;
			}

			inline void StoreBytes8(void* p, __m128i v) noexcept { std::memcpy(p, &v, 8u); }

			/* 八道一行的反量化 + 夹 ±lim。幅值式与 DequantizeValue 同式，δ 按 q 的奇偶取 0 / 1；
			 * qf = 0 那一格公式给出首电平 (q − δ) 而非 0，由 sign_epi32 按源符号把该道清成 0。
			 * mabs 只累计幅值，供调用方判 32 位乘法的可表达域。 */
			inline __m256i DequantRow(__m256i v, __m256i qv, __m256i delta, __m256i one, __m256i vlo, __m256i vhi,
									  __m256i& mabs) noexcept
			{
				const __m256i a = _mm256_abs_epi32(v);
				mabs = _mm256_max_epu32(mabs, a);
				const __m256i mag =
					_mm256_sub_epi32(_mm256_mullo_epi32(_mm256_add_epi32(_mm256_slli_epi32(a, 1), one), qv), delta);
				return _mm256_min_epi32(_mm256_max_epi32(_mm256_sign_epi32(mag, v), vlo), vhi);
			}

			/* 蝶形：a ← a + b、b ← a − b。用在「跨寄存器的列向」三档上，一次覆盖八列。 */
			inline void Bfly(__m256i& a, __m256i& b) noexcept
			{
				const __m256i s = _mm256_add_epi32(a, b);
				b = _mm256_sub_epi32(a, b);
				a = s;
			}

			/* 寄存器内的行向蝶形：配对靠一次 vpermd 换成伙伴值 p，加 / 减各算一遍再按道选。
			 * 置位道 = 每对里的「后」一道，该道要的是「前 − 后」= p − v，**不是 v − p** ——
			 * 与标量式 `*pa = a + b; *pb = a - b;` 同序，写反就等于给这一道换了符号。
			 * mask 走模板实参而不是形参：blend 的即席数在 MSVC 里必须是常量表达式。 */
			template<int mask>
			inline __m256i BflyRow(__m256i v, __m256i idx) noexcept
			{
				const __m256i p = _mm256_permutevar8x32_epi32(v, idx);
				return _mm256_blend_epi32(_mm256_add_epi32(v, p), _mm256_sub_epi32(p, v), mask);
			}

			inline __m256i FwhtRow(__m256i x) noexcept
			{
				x = BflyRow<0xAA>(x, _mm256_setr_epi32(1, 0, 3, 2, 5, 4, 7, 6));	// 档 1：(0,1)(2,3)(4,5)(6,7)
				x = BflyRow<0xCC>(x, _mm256_setr_epi32(2, 3, 0, 1, 6, 7, 4, 5));	// 档 2：(0,2)(1,3)(4,6)(5,7)
				x = BflyRow<0xF0>(x, _mm256_setr_epi32(4, 5, 6, 7, 0, 1, 2, 3));	// 档 4：(0,4)(1,5)(2,6)(3,7)
				return x;
			}

			/* 8×8 的 Sylvester FWHT，八行各占一条寄存器：先跨行（列向）三档，再行内三档。
			 * 全程只有加 / 减，整数加减没有舍入 ⇒ 只要各级中间量不溢出，结果与蝶形的配对先后次序无关，
			 * 故与 quant.cpp 的标量蝶形逐位同值。溢出界：输入 |f2| ≤ lim ≤ 16320，
			 * 一档后 ≤ 8 × lim = 130 560，二档后 ≤ 8 × 130 560 = 1 044 480 < 2^31 ⇒ 两条路都不溢出。 */
			inline void Fwht8(__m256i& v0, __m256i& v1, __m256i& v2, __m256i& v3, __m256i& v4, __m256i& v5, __m256i& v6,
							  __m256i& v7) noexcept
			{
				Bfly(v0, v1);
				Bfly(v2, v3);
				Bfly(v4, v5);
				Bfly(v6, v7);
				Bfly(v0, v2);
				Bfly(v1, v3);
				Bfly(v4, v6);
				Bfly(v5, v7);
				Bfly(v0, v4);
				Bfly(v1, v5);
				Bfly(v2, v6);
				Bfly(v3, v7);
				v0 = FwhtRow(v0);
				v1 = FwhtRow(v1);
				v2 = FwhtRow(v2);
				v3 = FwhtRow(v3);
				v4 = FwhtRow(v4);
				v5 = FwhtRow(v5);
				v6 = FwhtRow(v6);
				v7 = FwhtRow(v7);
			}

			/* 一行八道收尾：舍入 → 夹残差域 → 加预测 → 夹像素域 → 窄成 8 字节直接落平面的一行。
			 * 舍入走 int32 而不是 double：变换输出 b 恒是整数，y = b / 64 在双精度里精确可表示，
			 * 于是 floor(y + 0.5) = (b + 32) >> 6、ceil(y − 0.5) = (b + 31) >> 6（负道先补 63 再算术右移），
			 * 与 RoundResidual 逐位同值，且 b 在界内时两边都不溢出。
			 * 窄化两条 packus 的饱和都不参与：夹完恒在 [0, 255]。 */
			inline void RoundStoreRow8(__m256i b, const uint8_t* pred, uint8_t* dst) noexcept
			{
				const __m256i zero = _mm256_setzero_si256();
				const __m256i neg = _mm256_cmpgt_epi32(zero, b);
				const __m256i bias =
					_mm256_sub_epi32(_mm256_set1_epi32(32), _mm256_and_si256(neg, _mm256_set1_epi32(1)));
				__m256i f = _mm256_srai_epi32(_mm256_add_epi32(b, bias), 6);
				f = _mm256_min_epi32(_mm256_max_epi32(f, _mm256_set1_epi32(kResidualClampMin8)),
									 _mm256_set1_epi32(kResidualClampMax8));
				const __m256i p = _mm256_cvtepu8_epi32(LoadBytes8(pred));
				const __m256i d =
					_mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(f, p), zero), _mm256_set1_epi32(kSampleMax8));
				const __m128i w16 = _mm_packus_epi32(_mm256_castsi256_si128(d), _mm256_extracti128_si256(d, 1));
				StoreBytes8(dst, _mm_packus_epi16(w16, w16));
			}

			/* 一块 8×8 从量化系数一路落到平面：反量化 + 夹 ±lim → int32 域反变换 → 舍入 + 夹 + 加预测 + 夹。
			 * 中间不落 f2 与 double 两块暂存，出口也不经 recon 暂存再散写一遍：反量化、反变换与收尾
			 * 全程在寄存器里过，落盘只有八次「一行八字节」的写。
			 * 系数全零时残差恒为 0（DequantizeValue 对 qf = 0 给 0，无损档恒等也给 0），
			 * 变换输出随之全零 ⇒ 重建即预测块，走八次 movq 的复制支，省掉整块乘加与两趟蝶形。
			 * 返回 false = 本块落在 32 位乘法的可表达域之外（只有有损档可能），调用方整块退回标量式。 */
			bool ReconstructBlockSimd(const int32_t qf[kBlockCoefs], uint8_t q, int32_t lim,
									  const uint8_t pred[kBlockCoefs], uint8_t* dst, size_t stride)
			{
				const __m256i vlo = _mm256_set1_epi32(-lim);
				const __m256i vhi = _mm256_set1_epi32(lim);
				__m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf));
				__m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 8));
				__m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 16));
				__m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 24));
				__m256i v4 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 32));
				__m256i v5 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 40));
				__m256i v6 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 48));
				__m256i v7 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(qf + 56));

				const __m256i o01 = _mm256_or_si256(v0, v1), o23 = _mm256_or_si256(v2, v3);
				const __m256i o45 = _mm256_or_si256(v4, v5), o67 = _mm256_or_si256(v6, v7);
				const __m256i acc = _mm256_or_si256(_mm256_or_si256(o01, o23), _mm256_or_si256(o45, o67));
				if (_mm256_testz_si256(acc, acc))
				{
					for (int r = 0; r < kBlockSide; ++r)
						StoreBytes8(dst + static_cast<size_t>(r) * stride, LoadBytes8(pred + r * kBlockSide));
					return true;
				}

				__m256i mabs = _mm256_setzero_si256();
				if (q == kQuantLossless)
				{
					/* 无损档反量化是恒等映射，只做夹取，对全域入参都与标量同值，不设界。 */
					v0 = _mm256_min_epi32(_mm256_max_epi32(v0, vlo), vhi);
					v1 = _mm256_min_epi32(_mm256_max_epi32(v1, vlo), vhi);
					v2 = _mm256_min_epi32(_mm256_max_epi32(v2, vlo), vhi);
					v3 = _mm256_min_epi32(_mm256_max_epi32(v3, vlo), vhi);
					v4 = _mm256_min_epi32(_mm256_max_epi32(v4, vlo), vhi);
					v5 = _mm256_min_epi32(_mm256_max_epi32(v5, vlo), vhi);
					v6 = _mm256_min_epi32(_mm256_max_epi32(v6, vlo), vhi);
					v7 = _mm256_min_epi32(_mm256_max_epi32(v7, vlo), vhi);
				}
				else
				{
					const __m256i qv = _mm256_set1_epi32(static_cast<int32_t>(q));
					const __m256i delta = _mm256_set1_epi32(static_cast<int32_t>((q & 1u) ? 0u : 1u));
					const __m256i one = _mm256_set1_epi32(1);
					v0 = DequantRow(v0, qv, delta, one, vlo, vhi, mabs);
					v1 = DequantRow(v1, qv, delta, one, vlo, vhi, mabs);
					v2 = DequantRow(v2, qv, delta, one, vlo, vhi, mabs);
					v3 = DequantRow(v3, qv, delta, one, vlo, vhi, mabs);
					v4 = DequantRow(v4, qv, delta, one, vlo, vhi, mabs);
					v5 = DequantRow(v5, qv, delta, one, vlo, vhi, mabs);
					v6 = DequantRow(v6, qv, delta, one, vlo, vhi, mabs);
					v7 = DequantRow(v7, qv, delta, one, vlo, vhi, mabs);
					const int64_t hi = (static_cast<int64_t>(INT32_MAX) / static_cast<int64_t>(q) - 1) / 2;
					if (hi < 0 || static_cast<int64_t>(MaxU32Across(mabs)) > hi) return false;
				}

				Fwht8(v0, v1, v2, v3, v4, v5, v6, v7);

				RoundStoreRow8(v0, pred + 0 * kBlockSide, dst + 0 * stride);
				RoundStoreRow8(v1, pred + 1 * kBlockSide, dst + 1 * stride);
				RoundStoreRow8(v2, pred + 2 * kBlockSide, dst + 2 * stride);
				RoundStoreRow8(v3, pred + 3 * kBlockSide, dst + 3 * stride);
				RoundStoreRow8(v4, pred + 4 * kBlockSide, dst + 4 * stride);
				RoundStoreRow8(v5, pred + 5 * kBlockSide, dst + 5 * stride);
				RoundStoreRow8(v6, pred + 6 * kBlockSide, dst + 6 * stride);
				RoundStoreRow8(v7, pred + 7 * kBlockSide, dst + 7 * stride);
				return true;
			}
#endif	  // __AVX2__

		}	 // namespace

		/* 平面尺寸是**全库口径**：取块、重建、运动补偿的插值与边界复制都从这里取维数，
		 * 各自再除一遍迟早漂移（尤其是色度：这里隐含两个轴都严格减半）。 */
		uint32_t PlaneWidth(const PlaneView& v, uint32_t plane) noexcept { return v.width / SubFactor(plane); }

		uint32_t PlaneHeight(const PlaneView& v, uint32_t plane) noexcept { return v.height / SubFactor(plane); }

		/* 六块的左上角：亮度四块按「左上起、行优先」，与块模式位图的 bit5..bit2 同一序。
		 * 这份取块地址的规则全库只有这一处 —— 改它就是同时改了编码端、解码端与插值的块地址。 */
		BlockGeom MacroblockBlockAt(uint32_t mb_x, uint32_t mb_y, int idx) noexcept
		{
			if (idx < 4)
			{
				const uint32_t qx = static_cast<uint32_t>(idx % 2), qy = static_cast<uint32_t>(idx / 2);
				return { 0u, mb_x * kMbSize + qx * 8u, mb_y * kMbSize + qy * 8u };
			}
			return { (idx == 4) ? 1u : 2u, mb_x * (kMbSize / 2u), mb_y * (kMbSize / 2u) };
		}

		void GatherBlock8x8(const PlaneView& v, uint32_t plane, uint32_t px, uint32_t py, uint8_t out[kBlockCoefs])
		{
			for (uint32_t r = 0u; r < 8u; ++r)
			{
				const uint8_t* src =
					v.p[plane] + static_cast<size_t>(py + r) * v.stride[plane] + static_cast<size_t>(px);
#if defined(__AVX2__)
				StoreBytes8(out + r * 8u, LoadBytes8(src));
#else
				for (uint32_t c = 0u; c < 8u; ++c) out[r * 8u + c] = src[c];
#endif
			}
		}

		void ScatterBlock8x8(const PlaneView& v, uint32_t plane, uint32_t px, uint32_t py,
							 const uint8_t in[kBlockCoefs])
		{
			for (uint32_t r = 0u; r < 8u; ++r)
			{
				uint8_t* dst = v.p[plane] + static_cast<size_t>(py + r) * v.stride[plane] + static_cast<size_t>(px);
#if defined(__AVX2__)
				StoreBytes8(dst, LoadBytes8(in + r * 8u));
#else
				for (uint32_t c = 0u; c < 8u; ++c) dst[c] = in[r * 8u + c];
#endif
			}
		}

		/* 编码端的取块与求残并成一步：按行读平面的 8 像素、减同位的预测行，直接落 int32 载体。
		 * 少一次 pix 暂存的写出与读回，两侧减法与原来的逐像素式同值（都在 int32 域，恒不溢出）。 */
		void ResidualBlock8x8(const PlaneView& src, uint32_t plane, uint32_t px, uint32_t py,
							  const uint8_t pred[kBlockCoefs], int32_t x[kBlockCoefs])
		{
			for (uint32_t r = 0u; r < 8u; ++r)
			{
				const uint8_t* s = src.p[plane] + static_cast<size_t>(py + r) * src.stride[plane] + px;
				const uint8_t* p = pred + r * kBlockSide;
				int32_t* d = x + r * kBlockSide;
#if defined(__AVX2__)
				const __m256i vs = _mm256_cvtepu8_epi32(LoadBytes8(s));
				const __m256i vp = _mm256_cvtepu8_epi32(LoadBytes8(p));
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(d), _mm256_sub_epi32(vs, vp));
#else
				for (int c = 0; c < kBlockSide; ++c) d[c] = static_cast<int32_t>(s[c]) - static_cast<int32_t>(p[c]);
#endif
			}
		}

		void FillBlock(uint8_t value, uint8_t out[kBlockCoefs])
		{
			for (int i = 0; i < kBlockCoefs; ++i) out[i] = value;
		}

		/* 一块的重建，编解两侧唯一实现：四级域依次做完，出口直接落平面（不经 recon 暂存再散写）。
		 * 第二级域：反量化后系数夹 ±T_max，帧内 ±8192 / 帧间 ±16320，界由 reconstruct.h 单点派生。
		 * 第三级域：反变换 + 舍入后夹 [−2^bpp, 2^bpp − 1]，8-bit = [−256, 255]。
		 * 第四级域：加预测后夹 [0, 2^bpp − 1]，8-bit = [0, 255]。
		 * AVX2 档里反变换在 int32 域做完（见 Fwht8），标量档仍经 InverseHadamard8 的 double 出口，
		 * 两边逐位同值的依据写在 Fwht8 与 RoundStoreRow8 的注释里。 */
		void ReconstructBlockToPlane(const int32_t qf[kBlockCoefs], uint8_t q, bool intra,
									 const uint8_t pred[kBlockCoefs], const PlaneView& dst, uint32_t plane, uint32_t px,
									 uint32_t py)
		{
			const int32_t lim = intra ? kCoefClampMaxIntra8 : kCoefClampMaxInter8;
			uint8_t* base = dst.p[plane] + static_cast<size_t>(py) * dst.stride[plane] + static_cast<size_t>(px);
			const size_t stride = dst.stride[plane];
#if defined(__AVX2__)
			if (ReconstructBlockSimd(qf, q, lim, pred, base, stride)) return;
#endif
			int32_t f2[kBlockCoefs];
			for (int i = 0; i < kBlockCoefs; ++i)
			{
				int32_t v = DequantizeValue(qf[i], q);
				if (v < -lim) v = -lim;
				if (v > lim) v = lim;
				f2[i] = v;
			}
			double y[kBlockCoefs];
			InverseHadamard8(f2, y);
			/* 标量支的收尾刻意写成「平铺一遍 + 按行拷」两段，而不是逐行嵌套：
			 * 平铺那段的下标是线性的（`y[i]` / `pred[i]` / 本地 `out8[i]`），可被自动向量化；
			 * 嵌套成「行指针 + 列下标」后每道的地址都要现算，实测这一段比平铺慢约 5%。
			 * 落盘仍是八字节的行拷，与向量化支的出口形状一致。 */
			uint8_t out8[kBlockCoefs];
			for (int i = 0; i < kBlockCoefs; ++i)
			{
				int32_t f = RoundResidual(y[i]);
				if (f < kResidualClampMin8) f = kResidualClampMin8;
				if (f > kResidualClampMax8) f = kResidualClampMax8;
				int32_t d = static_cast<int32_t>(pred[i]) + f;
				if (d < 0) d = 0;
				if (d > kSampleMax8) d = kSampleMax8;
				out8[i] = static_cast<uint8_t>(d);
			}
			for (int r = 0; r < kBlockSide; ++r)
				std::memcpy(base + static_cast<size_t>(r) * stride, out8 + r * kBlockSide, kBlockSide);
		}

		/* out 就是行距 = 8 的一块平面，故不必再写第二套四级域算式：把它当平面交给上面那一条。 */
		void ReconstructBlock(const int32_t qf[kBlockCoefs], uint8_t q, bool intra, const uint8_t pred[kBlockCoefs],
							  uint8_t out[kBlockCoefs])
		{
			PlaneView v;
			v.p[0] = out;
			v.stride[0] = kBlockSide;
			ReconstructBlockToPlane(qf, q, intra, pred, v, 0u, 0u, 0u);
		}

		bool CopyPlane(const PlaneView& dst, const PlaneView& src)
		{
			if (dst.width != src.width || dst.height != src.height) return false;
			for (uint32_t plane = 0u; plane < 3u; ++plane)
			{
				if (dst.p[plane] == nullptr || src.p[plane] == nullptr) return false;
				if (dst.stride[plane] < src.stride[plane]) return false;
				const uint32_t w = PlaneWidth(src, plane);
				const uint32_t h = PlaneHeight(src, plane);
				for (uint32_t r = 0u; r < h; ++r)
				{
					std::memcpy(dst.p[plane] + static_cast<size_t>(r) * dst.stride[plane],
								src.p[plane] + static_cast<size_t>(r) * src.stride[plane], w);
				}
			}
			return true;
		}

		void FillPlanesBlack(const PlaneView& v)
		{
			const uint8_t level[3] = { kBlackLuma8, kBlackChroma8, kBlackChroma8 };
			for (uint32_t plane = 0u; plane < 3u; ++plane)
			{
				if (v.p[plane] == nullptr) continue;
				const uint32_t h = PlaneHeight(v, plane);
				for (uint32_t r = 0u; r < h; ++r)
					std::memset(v.p[plane] + static_cast<size_t>(r) * v.stride[plane], level[plane], v.stride[plane]);
			}
		}

	}	 // namespace video
}	 // namespace clv
