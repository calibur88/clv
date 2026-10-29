/* 字幕渲染器：布局（自动折行 / 显式换行）、码位级回退与分段、滚动、描边合成、脏矩形。
 * 一帧的用法：对每个可见块调 Layout，再调一次 Render；Render 之后本帧的布局记录清空。 */
#ifndef CLV_SUBTITLE_RENDER_RENDERER_H
#define CLV_SUBTITLE_RENDER_RENDERER_H

#include "CLV_Subtitle.h"
#include "subtitle/cal/cal.h"
#include "subtitle/status.h"
#include "subtitle/style.h"

#include <map>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace clv
{
	namespace subtitle
	{
		namespace render
		{
			class Renderer
			{
				public:

				static Renderer* Create(uint32_t width, uint32_t height, int metric_kind,
										const CLV_SubtitleStyleC* renderer_default);
				~Renderer();

				Err SetStyleBlock(const uint8_t* payload, size_t size);
				Err AddFontDir(const char* dir);
				Err Layout(const CLV_SubtitleBlockC& block, const CLV_SubtitleLineC* lines, uint32_t line_count,
						   const CLV_SubtitleStyleC* style, int64_t time_ticks);
				Err Render(uint8_t* rgba, size_t stride, uint32_t width, uint32_t height);
				Err DirtyRects(CLV_SubtitleRectC* rects, uint32_t capacity, uint32_t* out_total) const;
				Err Report(CLV_SubtitleRenderStatsC* out) const;

				Renderer(const Renderer&) = delete;
				Renderer& operator=(const Renderer&) = delete;

				private:

				Renderer(uint32_t width, uint32_t height, int metric_kind);
				bool Init();
				struct Impl;
				std::unique_ptr<Impl> impl_;
			};
		}	 // namespace render
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_RENDER_RENDERER_H
