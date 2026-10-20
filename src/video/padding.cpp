#include "padding.h"

#include "headers.h"

#include <cstring>

namespace clv
{
	namespace video
	{

		namespace
		{

			struct PlaneSize
			{
				uint32_t display_w;
				uint32_t display_h;
				uint32_t coded_w;
				uint32_t coded_h;
			};

			PlaneSize LumaSize(const FrameGeometry& g)
			{
				return { g.display_width, g.display_height, g.coded_width, g.coded_height };
			}

			PlaneSize ChromaSize(const FrameGeometry& g)
			{
				return { (g.display_width + 1u) / 2u, (g.display_height + 1u) / 2u, g.coded_width / 2u,
						 g.coded_height / 2u };
			}

			bool GeometryOk(const FrameGeometry& g)
			{
				return g.display_width >= 1u && g.display_height >= 1u && g.coded_width >= g.display_width &&
					   g.coded_height >= g.display_height && (g.coded_width % 16u) == 0u &&
					   (g.coded_height % 16u) == 0u;
			}

		}	 // namespace

		bool MakeFrameGeometry(uint16_t display_width, uint16_t display_height, FrameGeometry* out)
		{
			if (out == nullptr) return false;
			if (display_width == 0u || display_height == 0u) return false;
			FrameGeometry g;
			g.display_width = display_width;
			g.display_height = display_height;
			g.coded_width = CodedSize(display_width);
			g.coded_height = CodedSize(display_height);
			if (! GeometryOk(g)) return false;
			*out = g;
			return true;
		}

		VideoErr FillPadding(const FrameGeometry& g, const PlaneBuf planes[3], uint8_t bit_depth)
		{
			if (planes == nullptr) return VideoErr::kInvalidArgument;
			if (bit_depth != 8u) return VideoErr::kBitDepthUnsupported;
			if (! GeometryOk(g)) return VideoErr::kInvalidArgument;

			for (int p = 0; p < 3; ++p)
			{
				const PlaneSize s = (p == 0) ? LumaSize(g) : ChromaSize(g);
				const uint8_t fill = (p == 0) ? kBlackLuma8 : kBlackChroma8;
				if (planes[p].data == nullptr || planes[p].stride < s.coded_w) return VideoErr::kInvalidArgument;
				for (uint32_t y = 0; y < s.coded_h; ++y)
				{
					uint8_t* row = planes[p].data + static_cast<size_t>(y) * planes[p].stride;
					if (y >= s.display_h)
					{
						std::memset(row, fill, s.coded_w);
						continue;
					}
					if (s.coded_w > s.display_w) std::memset(row + s.display_w, fill, s.coded_w - s.display_w);
				}
			}
			return VideoErr::kOk;
		}

		VideoErr CropToDisplay(const FrameGeometry& g, const PlaneBuf src[3], const PlaneBuf dst[3], uint8_t bit_depth)
		{
			if (src == nullptr || dst == nullptr) return VideoErr::kInvalidArgument;
			if (bit_depth != 8u) return VideoErr::kBitDepthUnsupported;
			if (! GeometryOk(g)) return VideoErr::kInvalidArgument;

			for (int p = 0; p < 3; ++p)
			{
				const PlaneSize s = (p == 0) ? LumaSize(g) : ChromaSize(g);
				if (src[p].data == nullptr || dst[p].data == nullptr) return VideoErr::kInvalidArgument;
				if (src[p].stride < s.coded_w || dst[p].stride < s.display_w) return VideoErr::kOutputTooSmall;
				for (uint32_t y = 0; y < s.display_h; ++y)
				{
					const uint8_t* sr = src[p].data + static_cast<size_t>(y) * src[p].stride;
					uint8_t* dr = dst[p].data + static_cast<size_t>(y) * dst[p].stride;
					std::memcpy(dr, sr, s.display_w);
				}
			}
			return VideoErr::kOk;
		}

	}	 // namespace video
}	 // namespace clv
