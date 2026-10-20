/* clv_rgb.cpp
 *
 * 帧平面 I420 → RGB24 的实现，公开面是 `CLV_Video.h` 的 `CLV_VideoFrameToRGB24`。
 * 放在库侧而不是显示侧，理由只有一条：这一腿的逐字节结果必须与解码器输出一样可钉，
 * 落在调用方各自实现就会各写一套（增益式、移位取整、色度取样点任一不同都不逐字节等）。
 *
 * 两支同式：`Rgb24FromI420` 在 `__AVX2__` 下按八像素一组接管，否则落到标量式；
 * `Rgb24FromI420Scalar` 恒走标量式，专供用例把两支逐字节对平（依据见两处注释）。
 * 口径：色度 2×2 最近邻、三条增益式、移位向下取整、结果夹 [0, 255]。
 * 本件不分配，故没有抛出源，也就不需要句柄层那套 try / catch 收口。
 */
#include "CLV_Video.h"
#include "rgb.h"

#include <cstdint>
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

			inline uint8_t Clip255(int32_t v) noexcept
			{
				if (v < 0) return 0u;
				if (v > 255) return 255u;
				return static_cast<uint8_t>(v);
			}

			/* ≫ 10 的向下取整版：负值走「先补 1023 再取负」的整除式，与硬件算术移位逐位同值，
			 * 且不依赖「右移负数是实现定义」这一条（MSVC 与 GCC 恰好都是算术移位，换编译器不保证）。 */
			inline int32_t FloorDiv1024(int32_t a) noexcept { return a >= 0 ? a / 1024 : -((-a + 1023) / 1024); }

			/* 一行的标量式：从 x0 起算到行尾，向量化支跑完整组后接在这里。 */
			void RgbRowScalar(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint32_t x0, uint32_t w,
							  uint8_t* out)
			{
				for (uint32_t x = x0; x < w; ++x)
				{
					const int32_t Y = static_cast<int32_t>(y[x]);
					const int32_t U = static_cast<int32_t>(u[x >> 1]) - 128;
					const int32_t V = static_cast<int32_t>(v[x >> 1]) - 128;
					uint8_t* d = out + static_cast<size_t>(x) * 3u;
					d[0] = Clip255(Y + FloorDiv1024(1436 * V));
					d[1] = Clip255(Y - FloorDiv1024(731 * U + 1285 * V));
					d[2] = Clip255(Y + FloorDiv1024(1814 * U));
				}
			}

			void RgbFrameScalar(const uint8_t* y, uint32_t stride_y, const uint8_t* cb, uint32_t stride_cb,
								const uint8_t* cr, uint32_t stride_cr, uint32_t w, uint32_t h, uint8_t* dst)
			{
				for (uint32_t r = 0u; r < h; ++r)
				{
					RgbRowScalar(y + static_cast<size_t>(r) * stride_y, cb + static_cast<size_t>(r >> 1) * stride_cb,
								 cr + static_cast<size_t>(r >> 1) * stride_cr, 0u, w,
								 dst + static_cast<size_t>(r) * static_cast<size_t>(w) * 3u);
				}
			}

#if defined(__AVX2__)
			/* 一次八字节的进出走 memcpy：MSVC 与 GCC 对 loadl / storel 的形参类型口径不同，
			 * memcpy 两边都成立且都收成一条 movq，读界也恰好是 8 字节，不多碰一个字节。 */
			inline __m128i LoadBytes8(const void* p) noexcept
			{
				__m128i s = _mm_setzero_si128();
				std::memcpy(&s, p, 8u);
				return s;
			}

			inline __m128i LoadBytes4(const void* p) noexcept
			{
				uint32_t t = 0u;
				std::memcpy(&t, p, 4u);
				return _mm_cvtsi32_si128(static_cast<int>(t));
			}

			/* 三道各八字节交插成 24 字节（RGB24 的一行片段）。
			 * 每条 shuffle 只能从一个源里挑，故 o0 / o1 各由三次 shuffle 相或而成；掩码里 0x80 的道写 0。
			 * 落盘刻意只写 24 字节：o0 的后四字节是 0，由 o1 的前四字节覆写，o1 只写有效的那 12 字节，
			 * 于是本组恰好覆盖 [dst, dst + 24)，最后一组也不会越出缓冲尾。 */
			inline void StoreGroup24(__m128i r, __m128i g, __m128i b, uint8_t* dst) noexcept
			{
				const __m128i rg = _mm_unpacklo_epi64(r, g);
				const __m128i bb = _mm_unpacklo_epi64(b, b);
				// 前四个像素：R 取 rg 的 0..3，G 取 8..11，B 取 bb 的 0..3
				const __m128i a0 =
					_mm_shuffle_epi8(rg, _mm_setr_epi8(0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, -1, -1, -1, -1));
				const __m128i a1 =
					_mm_shuffle_epi8(rg, _mm_setr_epi8(-1, 8, -1, -1, 9, -1, -1, 10, -1, -1, 11, -1, -1, -1, -1, -1));
				const __m128i a2 =
					_mm_shuffle_epi8(bb, _mm_setr_epi8(-1, -1, 0, -1, -1, 1, -1, -1, 2, -1, -1, 3, -1, -1, -1, -1));
				const __m128i o0 = _mm_or_si128(_mm_or_si128(a0, a1), a2);
				// 后四个像素：同一套挑法，道号各加四
				const __m128i c0 =
					_mm_shuffle_epi8(rg, _mm_setr_epi8(4, -1, -1, 5, -1, -1, 6, -1, -1, 7, -1, -1, -1, -1, -1, -1));
				const __m128i c1 =
					_mm_shuffle_epi8(rg, _mm_setr_epi8(-1, 12, -1, -1, 13, -1, -1, 14, -1, -1, 15, -1, -1, -1, -1, -1));
				const __m128i c2 =
					_mm_shuffle_epi8(bb, _mm_setr_epi8(-1, -1, 4, -1, -1, 5, -1, -1, 6, -1, -1, 7, -1, -1, -1, -1));
				const __m128i o1 = _mm_or_si128(_mm_or_si128(c0, c1), c2);
				std::memcpy(dst, &o0, 16u);
				std::memcpy(dst + 12, &o1, 12u);
			}

			/* 八道夹 [0, 255] 后窄成 8 字节：两道 packus 的饱和都不参与，值已在字节域内。 */
			inline __m128i Pack8(__m256i d) noexcept
			{
				const __m128i w16 = _mm_packus_epi32(_mm256_castsi256_si128(d), _mm256_extracti128_si256(d, 1));
				return _mm_packus_epi16(w16, w16);
			}

			inline __m256i ClampByte(__m256i v) noexcept
			{
				return _mm256_min_epi32(_mm256_max_epi32(v, _mm256_setzero_si256()), _mm256_set1_epi32(255));
			}

			/* 八像素一组：亮度一次进八道，色度四个取样点按 2×1 复制成八道（一次 vpermd）。
			 * 移位用 srai_epi32（硬件即算术移位 = 向下取整），与 FloorDiv1024 逐道同值。
			 * 乘积界：1436 / 1285 / 1814 / 731 乘 127 都在 2^18 内，两两相加的上界 256 052，int32 不溢出。 */
			uint32_t RgbRowSimd(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint32_t w, uint8_t* out)
			{
				const __m256i dup = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
				const __m256i c128 = _mm256_set1_epi32(128);
				const __m256i k1436 = _mm256_set1_epi32(1436);
				const __m256i k731 = _mm256_set1_epi32(731);
				const __m256i k1285 = _mm256_set1_epi32(1285);
				const __m256i k1814 = _mm256_set1_epi32(1814);
				uint32_t x = 0u;
				for (; x + 8u <= w; x += 8u)
				{
					const __m256i yv = _mm256_cvtepu8_epi32(LoadBytes8(y + x));
					const __m256i uv = _mm256_sub_epi32(
						_mm256_permutevar8x32_epi32(_mm256_cvtepu8_epi32(LoadBytes4(u + (x >> 1))), dup), c128);
					const __m256i vv = _mm256_sub_epi32(
						_mm256_permutevar8x32_epi32(_mm256_cvtepu8_epi32(LoadBytes4(v + (x >> 1))), dup), c128);
					const __m256i dr = _mm256_srai_epi32(_mm256_mullo_epi32(vv, k1436), 10);
					const __m256i dg = _mm256_srai_epi32(
						_mm256_add_epi32(_mm256_mullo_epi32(uv, k731), _mm256_mullo_epi32(vv, k1285)), 10);
					const __m256i db = _mm256_srai_epi32(_mm256_mullo_epi32(uv, k1814), 10);
					StoreGroup24(Pack8(ClampByte(_mm256_add_epi32(yv, dr))), Pack8(ClampByte(_mm256_sub_epi32(yv, dg))),
								 Pack8(ClampByte(_mm256_add_epi32(yv, db))), out + static_cast<size_t>(x) * 3u);
				}
				return x;
			}
#endif	  // __AVX2__

		}	 // namespace

		void Rgb24FromI420Scalar(const uint8_t* y, uint32_t stride_y, const uint8_t* cb, uint32_t stride_cb,
								 const uint8_t* cr, uint32_t stride_cr, uint32_t w, uint32_t h, uint8_t* dst)
		{
			RgbFrameScalar(y, stride_y, cb, stride_cb, cr, stride_cr, w, h, dst);
		}

		void Rgb24FromI420(const uint8_t* y, uint32_t stride_y, const uint8_t* cb, uint32_t stride_cb,
						   const uint8_t* cr, uint32_t stride_cr, uint32_t w, uint32_t h, uint8_t* dst)
		{
#if defined(__AVX2__)
			for (uint32_t r = 0u; r < h; ++r)
			{
				const uint8_t* py = y + static_cast<size_t>(r) * stride_y;
				const uint8_t* pu = cb + static_cast<size_t>(r >> 1) * stride_cb;
				const uint8_t* pv = cr + static_cast<size_t>(r >> 1) * stride_cr;
				uint8_t* out = dst + static_cast<size_t>(r) * static_cast<size_t>(w) * 3u;
				RgbRowScalar(py, pu, pv, RgbRowSimd(py, pu, pv, w, out), w, out);
			}
#else
			RgbFrameScalar(y, stride_y, cb, stride_cb, cr, stride_cr, w, h, dst);
#endif
		}

	}	 // namespace video
}	 // namespace clv

extern "C" CLV_VideoError CLV_VideoFrameToRGB24(const CLV_VideoFrameDescC* desc, uint8_t* dst)
{
	if (desc == nullptr || dst == nullptr) return CLV_VIDEO_INVALID_ARGUMENT;
	if (desc->pixel_format != CLV_VIDEO_PIX_YUV420P) return CLV_VIDEO_INVALID_ARGUMENT;
	if (desc->plane[0] == nullptr || desc->plane[1] == nullptr || desc->plane[2] == nullptr)
		return CLV_VIDEO_INVALID_ARGUMENT;
	if (desc->bit_depth != 8u) return CLV_VIDEO_BIT_DEPTH_UNSUPPORTED;
	const uint32_t w = (desc->display_width != 0u) ? desc->display_width : desc->coded_width;
	const uint32_t h = (desc->display_height != 0u) ? desc->display_height : desc->coded_height;
	if (w == 0u || h == 0u) return CLV_VIDEO_VALUE_RANGE;
	const uint32_t cw = (w + 1u) / 2u;
	if (desc->stride[0] < w || desc->stride[1] < cw || desc->stride[2] < cw) return CLV_VIDEO_INVALID_ARGUMENT;
	clv::video::Rgb24FromI420(desc->plane[0], desc->stride[0], desc->plane[1], desc->stride[1], desc->plane[2],
							  desc->stride[2], w, h, dst);
	return CLV_VIDEO_OK;
}
