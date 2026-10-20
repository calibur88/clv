/* rgb.h
 *
 * 帧平面 I420 → RGB24 的实现层口径。公开面是 `CLV_Video.h` 的 `CLV_VideoFrameToRGB24`，
 * 本头只放两件事：入参拆平后的实际算式，以及一条**恒走标量式**的入口。
 *
 * 为什么要第二条入口：向量化由 `__AVX2__` 决定，同一个二进制里只有一条支路在跑，
 * 「SIMD 支与标量支逐字节同值」这条声称不能靠同一个入口自己跟自己比 —— 必须有一处
 * 不受编译开关影响的参照实现，用例才能把两支放在一起逐字节对。
 */
#ifndef CLV_VIDEO_RGB_H
#define CLV_VIDEO_RGB_H

#include <cstdint>

namespace clv
{
	namespace video
	{

		/* 一帧的转换。`w × h` 是输出尺寸（紧密排列，一行 3w 字节、行间不留填充），
		 * 三个源平面各按自己的行距寻址；色度按 2×2 最近邻（像素 (r,c) 取 (r/2, c/2)）。
		 * 增益式与移位取整口径见 `CLV_Video.h` 的入口注释，两支同式。 */
		void Rgb24FromI420(const uint8_t* y, uint32_t stride_y, const uint8_t* cb, uint32_t stride_cb,
						   const uint8_t* cr, uint32_t stride_cr, uint32_t w, uint32_t h, uint8_t* dst);

		/* 恒走标量式的那一支，签名与上面同：不受 `__AVX2__` 影响，供用例做逐字节对平。 */
		void Rgb24FromI420Scalar(const uint8_t* y, uint32_t stride_y, const uint8_t* cb, uint32_t stride_cb,
								 const uint8_t* cr, uint32_t stride_cr, uint32_t w, uint32_t h, uint8_t* dst);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_RGB_H
