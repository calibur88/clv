/* padding.h
 *
 * 补边与裁回。显示尺寸不是 16 的倍数时按 `ceil16` 补到编码尺寸：只补右与下，填充取黑电平
 * （8-bit 有限范围 luma = 16 / chroma = 128），填充块照常参与编码；显示前按原始尺寸裁回。
 * 补后尺寸不落字节，一律从原始尺寸推导。
 *
 * 只支持 8-bit：更高位深的黑电平取值与每样本字节数尚未定义，非 8-bit 一律 kBitDepthUnsupported。
 * 平面顺序是 YUV420P：0 = Y，1 = U，2 = V；色度平面在两个轴上都是亮度的一半（编码尺寸是 16 的倍数，
 * 故整除；显示尺寸折半向上取整）。
 */
#ifndef CLV_VIDEO_PADDING_H
#define CLV_VIDEO_PADDING_H

#include "status.h"

#include <cstdint>

namespace clv
{
	namespace video
	{

		// 8-bit 有限范围的黑电平
		inline constexpr uint8_t kBlackLuma8 = 16u;
		inline constexpr uint8_t kBlackChroma8 = 128u;

		struct FrameGeometry
		{
			uint32_t display_width = 0u;
			uint32_t display_height = 0u;
			uint32_t coded_width = 0u;
			uint32_t coded_height = 0u;
		};

		struct PlaneBuf
		{
			uint8_t* data = nullptr;
			uint32_t stride = 0u;	 // 行距，字节
		};

		/* coded = ceil16(display)，按 32 位无符号推；原始尺寸为 0 返回 false。 */
		bool MakeFrameGeometry(uint16_t display_width, uint16_t display_height, FrameGeometry* out);

		/* 把 planes 三平面里编码尺寸减显示尺寸的那圈（右列 + 下行）填成黑电平。 */
		VideoErr FillPadding(const FrameGeometry& g, const PlaneBuf planes[3], uint8_t bit_depth);

		/* 把 src 的显示区域逐行拷进 dst 的左上角；dst 的可用区域必须容得下显示尺寸。 */
		VideoErr CropToDisplay(const FrameGeometry& g, const PlaneBuf src[3], const PlaneBuf dst[3], uint8_t bit_depth);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_PADDING_H
