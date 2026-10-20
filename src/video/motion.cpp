/* motion.cpp
 *
 * 运动补偿的实现：编码器内部重建与解码器都从这里拿预测块，实现只有这一份，
 * 分两处写迟早漂移，而漂移的直接后果是编解两侧不再逐字节全等。
 */
#include "motion.h"

#if defined(__AVX2__)
	#include <immintrin.h>
#endif

namespace clv
{
	namespace video
	{

		namespace
		{

			/* 半像素坐标 → 整像素坐标，向下取整（floor 除 2）。
			 * 用整除式而不是 `>>`：负数右移在标准里是实现定义（MSVC 是算术右移、恰好同义，换编译器不保证），
			 * 而这条换算是同一份代码两侧共用，只需自洽；真正要求跨编译器一致的是 round_half 的值，
			 * 那里也走本函数，故一并写死。 */
			int64_t FloorDiv2(int64_t v) { return (v >= 0) ? v / 2 : -((-v + 1) / 2); }

			/* 代价的子和：一块内按 2 步长取样（4 行 × 4 列 = 16 点）的绝对差之和。 */
			uint64_t SubsampledSadBlock(const uint8_t cur[kBlockCoefs], const uint8_t pred[kBlockCoefs])
			{
				uint64_t sad = 0u;
				for (int r = 0; r < 8; r += 2)
				{
					for (int c = 0; c < 8; c += 2)
					{
						const int i = r * 8 + c;
						const int32_t d = static_cast<int32_t>(cur[i]) - static_cast<int32_t>(pred[i]);
						sad += static_cast<uint64_t>(d >= 0 ? d : -d);
					}
				}
				return sad;
			}

			/* 运动估计的比对面：只看亮度四块 64 点（色度不出场，取了也不进代价）。 */
			uint64_t LumaCost(const PlaneView& src, const PlaneView& ref, uint32_t mb_x, uint32_t mb_y, MotionVector mv)
			{
				uint8_t cur[kBlockCoefs];
				uint8_t pred[kBlockCoefs];
				uint64_t cost = 0u;
				for (int idx = 0; idx < 4; ++idx)
				{
					const BlockGeom g = MacroblockBlockAt(mb_x, mb_y, idx);
					GatherBlock8x8(src, g.plane, g.px, g.py, cur);
					PredictBlock8x8(ref, g.plane, g.px, g.py, mv, pred);
					cost += SubsampledSadBlock(cur, pred);
				}
				return cost;
			}

#if defined(__AVX2__)
			/* 一块 8×8 的运动补偿并行版。返回 false 表示本块落在复制带上，交回标量式。
			 *
			 * 取样坐标的分解：取样位置 = 整像素 ×2 再叠矢量，故
			 *   floor((2·(px+c) + mv.h) / 2) = (px + floor(mv.h / 2)) + c
			 * 奇偶（是否半像素）只看 mv 分量的奇偶，与 c、r 无关 —— 于是八列的列窗整体平移，
			 * 八行的行窗整体平移，逐像素的除法与判奇退化成一个块级偏移。负数一律走 FloorDiv2 的
			 * 向下取整整除式（与标量式同一函数、同一语义），奇偶用 `% 2`（C++11 起向零截断，
			 * 负奇数给 −1，非零即奇）而不是对负数右移。
			 *
			 * 接管条件三条：列窗 ix0..ix0+8 落在 [0, 宽−1]（b / d 模式多取一列）、行窗 iy0..iy0+8
			 * 落在 [0, 高−1]（c / d 模式多取一行）、且 16 字节非对齐装载的读界 ix0+16 ≤ 行距。
			 * 三条同时成立 ⇒ 一个像素都不需要钳位，钳位与它给的重叠取样都退化成平移，
			 * 逐像素与标量式同值；任一不成立就整块交回标量式。
			 *
			 * 四式的算术：整像素 = A；b = (A+B+1)>>1；c = (A+C+1)>>1；d = (A+B+C+D+2)>>2。
			 * avg_epu8 的定义就是「无符号字节相加、加一、右移一位」，与 b / c 两式逐位同值；
			 * d 式先窄化到 16 位道再加（四元和上界 1020，加偏置 1022，不溢出），右移两位后落
			 * [0, 255]，窄回字节时的饱和不参与，与标量的 uint32 逐项相加同值。操作数恒非负，
			 * 故这里的右移全在像素域，不碰矢量域的取整口径。 */
			inline __m128i CompRow(__m128i wa, __m128i wc, __m128i shift, bool half_x, bool half_y)
			{
				if (! half_x)
				{
					if (! half_y) return wa;
					return _mm_avg_epu8(wa, wc);
				}
				const __m128i b = _mm_shuffle_epi8(wa, shift);
				if (! half_y) return _mm_avg_epu8(wa, b);
				const __m128i d = _mm_shuffle_epi8(wc, shift);
				const __m128i s1 = _mm_add_epi16(_mm_cvtepu8_epi16(wa), _mm_cvtepu8_epi16(b));
				const __m128i s2 = _mm_add_epi16(_mm_cvtepu8_epi16(wc), _mm_cvtepu8_epi16(d));
				const __m128i t = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(s1, s2), _mm_set1_epi16(2)), 2);
				return _mm_packus_epi16(t, t);
			}

			bool PredictBlock8x8Simd(const PlaneView& ref, uint32_t plane, uint32_t px, uint32_t py, MotionVector mv,
									 uint8_t out[kBlockCoefs])
			{
				if (ref.p[plane] == nullptr) return false;
				const int64_t w = static_cast<int64_t>(PlaneWidth(ref, plane)) - 1;
				const int64_t h = static_cast<int64_t>(PlaneHeight(ref, plane)) - 1;
				const int64_t pitch = static_cast<int64_t>(ref.stride[plane]);
				const int64_t hv = static_cast<int64_t>(mv.h);
				const int64_t vv = static_cast<int64_t>(mv.v);
				const int64_t ix0 = static_cast<int64_t>(px) + FloorDiv2(hv);
				const int64_t iy0 = static_cast<int64_t>(py) + FloorDiv2(vv);
				if (ix0 < 0 || iy0 < 0) return false;
				if (ix0 + 8 > w || iy0 + 8 > h) return false;
				if (ix0 + 16 > pitch) return false;
				const bool half_x = (hv % 2) != 0;
				const bool half_y = (vv % 2) != 0;
				const size_t pitch_s = static_cast<size_t>(pitch);
				const __m128i shift = _mm_setr_epi8(1, 2, 3, 4, 5, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1);
				const uint8_t* base = ref.p[plane] + static_cast<size_t>(ix0);
				for (int r = 0; r < 8; r += 2)
				{
					const uint8_t* s = base + static_cast<size_t>(iy0 + r) * pitch_s;
					const __m128i w0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
					const __m128i w1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + pitch_s));
					const __m128i w2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 2 * pitch_s));
					const __m128i r0 = CompRow(w0, half_y ? w1 : w0, shift, half_x, half_y);
					const __m128i r1 = CompRow(w1, half_y ? w2 : w1, shift, half_x, half_y);
					/* 两行的低 8 字节拼成 16 字节整写：写点 0 / 16 / 32 / 48，正好覆盖 out 的 64 字节。 */
					_mm_storeu_si128(reinterpret_cast<__m128i*>(out + r * 8), _mm_unpacklo_epi64(r0, r1));
				}
				return true;
			}
#endif	  // __AVX2__

		}	 // namespace

		int32_t RoundHalf(int32_t v) noexcept { return static_cast<int32_t>(FloorDiv2(static_cast<int64_t>(v) + 1)); }

		MotionVector ChromaMotionVector(MotionVector luma) noexcept
		{
			MotionVector c;
			c.h = RoundHalf(luma.h);
			c.v = RoundHalf(luma.v);
			return c;
		}

		/* 一圈 1 像素复制带的实现形式：坐标钳到最外像素。带内取实际像素、带外取最外像素，
		 * 与「另开一块扩边缓冲」逐像素同值，差别只在不需要物理扩边，且钳位对任意远坐标都有定义。 */
		uint8_t SampleReplicated(const PlaneView& v, uint32_t plane, int64_t x, int64_t y) noexcept
		{
			const int64_t w = static_cast<int64_t>(PlaneWidth(v, plane)) - 1;
			const int64_t h = static_cast<int64_t>(PlaneHeight(v, plane)) - 1;
			const int64_t cx = (x < 0) ? 0 : (x > w ? w : x);
			const int64_t cy = (y < 0) ? 0 : (y > h ? h : y);
			const uint8_t* row = v.p[plane] + static_cast<size_t>(cy) * static_cast<size_t>(v.stride[plane]);
			return row[static_cast<size_t>(cx)];
		}

		void PredictBlock8x8(const PlaneView& ref, uint32_t plane, uint32_t px, uint32_t py, MotionVector mv,
							 uint8_t out[kBlockCoefs])
		{
#if defined(__AVX2__)
			if (PredictBlock8x8Simd(ref, plane, px, py, mv, out)) return;
#endif
			for (uint32_t r = 0u; r < 8u; ++r)
			{
				for (uint32_t c = 0u; c < 8u; ++c)
				{
					/* 取样位置以半像素为单位：整像素坐标 ×2 再叠矢量。两项都在 int64 里加，
					 * 矢量分量取满 32 位有符号域也不溢出；越界坐标交给 SampleReplicated 钳到最外像素。 */
					const int64_t hx = 2 * static_cast<int64_t>(px + c) + static_cast<int64_t>(mv.h);
					const int64_t hy = 2 * static_cast<int64_t>(py + r) + static_cast<int64_t>(mv.v);
					const int64_t ix = FloorDiv2(hx);
					const int64_t iy = FloorDiv2(hy);
					const bool hx_half = (hx - 2 * ix) != 0;
					const bool hy_half = (hy - 2 * iy) != 0;

					const uint32_t aa = SampleReplicated(ref, plane, ix, iy);
					uint32_t value = aa;
					if (! hx_half && ! hy_half)
					{
						value = aa;	   // 整像素位置：直接取 A
					}
					else if (hx_half && ! hy_half)
					{
						const uint32_t bb = SampleReplicated(ref, plane, ix + 1, iy);
						value = (aa + bb + 1u) >> 1u;	 // b
					}
					else if (! hx_half && hy_half)
					{
						const uint32_t cc = SampleReplicated(ref, plane, ix, iy + 1);
						value = (aa + cc + 1u) >> 1u;	 // c
					}
					else
					{
						const uint32_t bb = SampleReplicated(ref, plane, ix + 1, iy);
						const uint32_t cc = SampleReplicated(ref, plane, ix, iy + 1);
						const uint32_t dd = SampleReplicated(ref, plane, ix + 1, iy + 1);
						value = (aa + bb + cc + dd + 2u) >> 2u;	   // d
					}
					out[static_cast<size_t>(r) * 8u + c] = static_cast<uint8_t>(value);
				}
			}
		}

		uint8_t BidirectionalAverage(uint8_t fwd, uint8_t back) noexcept
		{
			return static_cast<uint8_t>((static_cast<uint32_t>(fwd) + static_cast<uint32_t>(back) + 1u) >> 1u);
		}

		void BlendMacroblocks(const uint8_t (*pf)[kBlockCoefs], const uint8_t (*pb)[kBlockCoefs],
							  uint8_t (*out)[kBlockCoefs]) noexcept
		{
#if defined(__AVX2__)
			/* 六块按行主序连续排布（形参就是从 uint8_t[6][64] 退化的指针），384 字节 = 12 段 32 字节，
			 * 正好铺满、无尾块。avg_epu8 即「无符号字节相加、加一、右移一位」，
			 * 与 BidirectionalAverage 的 (fwd + back + 1) >> 1 逐位同值。 */
			for (int i = 0; i < kBlocksPerMacroblock * kBlockCoefs; i += 32)
			{
				const __m256i a =
					_mm256_loadu_si256(reinterpret_cast<const __m256i*>(reinterpret_cast<const uint8_t*>(pf) + i));
				const __m256i b =
					_mm256_loadu_si256(reinterpret_cast<const __m256i*>(reinterpret_cast<const uint8_t*>(pb) + i));
				_mm256_storeu_si256(reinterpret_cast<__m256i*>(reinterpret_cast<uint8_t*>(out) + i),
									_mm256_avg_epu8(a, b));
			}
			return;
#else
			for (int idx = 0; idx < kBlocksPerMacroblock; ++idx)
				for (int i = 0; i < kBlockCoefs; ++i) out[idx][i] = BidirectionalAverage(pf[idx][i], pb[idx][i]);
#endif
		}

		void PredictMacroblock(const PlaneView& ref, uint32_t mb_x, uint32_t mb_y, MotionVector mv,
							   uint8_t out[kBlocksPerMacroblock][kBlockCoefs])
		{
			const MotionVector cmv = ChromaMotionVector(mv);
			for (int idx = 0; idx < kBlocksPerMacroblock; ++idx)
			{
				const BlockGeom g = MacroblockBlockAt(mb_x, mb_y, idx);
				/* 同核、不同换算与尺寸：矢量换算走 round_half，块的左上角与尺寸由 BlockGeom 给。 */
				PredictBlock8x8(ref, g.plane, g.px, g.py, (g.plane == 0u) ? mv : cmv, out[idx]);
			}
		}

		VideoErr PredictDirectMacroblock(const PlaneView* fwd, const PlaneView* back, uint32_t mb_x, uint32_t mb_y,
										 const DirectScaled& mv, uint8_t out[kBlocksPerMacroblock][kBlockCoefs])
		{
			if (fwd == nullptr || back == nullptr) return VideoErr::kInvalidArgument;
			uint8_t pf[kBlocksPerMacroblock][kBlockCoefs];
			uint8_t pb[kBlocksPerMacroblock][kBlockCoefs];
			PredictMacroblock(*fwd, mb_x, mb_y, mv.forward, pf);
			PredictMacroblock(*back, mb_x, mb_y, mv.backward, pb);
			BlendMacroblocks(pf, pb, out);
			return VideoErr::kOk;
		}

		MotionVector EstimateMotion(const PlaneView& src, const PlaneView& ref, uint32_t mb_x, uint32_t mb_y) noexcept
		{
			/* 平票取零：零矢量先在趟首取一次代价，之后只有**严格更小**才换主，
			 * 于是代价相同的整片候选（平场、周期图样）留不住它，落到零运动。 */
			MotionVector best {};
			uint64_t best_cost = LumaCost(src, ref, mb_x, mb_y, best);

			/* 第一趟：整像素格 ±kSearchRadiusPel（矢量单位是半像素，故 ×2）。
			 * 候选序固定（dy 升、dx 升），零矢量那格已在趟首取过，这里跳过它。 */
			for (int32_t dy = -kSearchRadiusPel; dy <= kSearchRadiusPel; ++dy)
			{
				for (int32_t dx = -kSearchRadiusPel; dx <= kSearchRadiusPel; ++dx)
				{
					if (dx == 0 && dy == 0) continue;
					const MotionVector mv { dx * 2, dy * 2 };
					const uint64_t cost = LumaCost(src, ref, mb_x, mb_y, mv);
					if (cost < best_cost)
					{
						best_cost = cost;
						best = mv;
					}
				}
			}

			/* 第二趟：在最佳整像素点周围 ±1 半像素精修（含自身；自身平票不更新）。 */
			MotionVector refined = best;
			for (int32_t sy = -1; sy <= 1; ++sy)
			{
				for (int32_t sx = -1; sx <= 1; ++sx)
				{
					const MotionVector mv { best.h + sx, best.v + sy };
					const uint64_t cost = LumaCost(src, ref, mb_x, mb_y, mv);
					if (cost < best_cost)
					{
						best_cost = cost;
						refined = mv;
					}
				}
			}
			return refined;
		}

	}	 // namespace video
}	 // namespace clv

