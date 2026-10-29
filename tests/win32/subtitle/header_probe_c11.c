/* C11 compile probe for include/CLV_Subtitle.h: the public header must parse as C11 with no C++-only
 * constructs. References a few types and constants so the compiler cannot elide the include. */
#include "CLV_Subtitle.h"

int clv_subtitle_header_probe_c11(void)
{
	CLV_SubtitleStyleC style;
	CLV_SubtitleBlockC block;
	CLV_SubtitleParserParamsC pp;
	CLV_SubtitleRendererParamsC rp;
	CLV_SubtitleError e = CLV_SUBTITLE_STYLE_BLOCK_BAD;
	style.struct_size = (uint32_t) sizeof(style);
	block.style_source = (uint64_t) CLV_SUBTITLE_STYLE_ALL;
	pp.struct_size = 0u;
	rp.metric_entry = (uint32_t) CLV_SUBTITLE_METRIC_HB_FT;
	return (int) e + (int) (style.struct_size + block.style_source + pp.struct_size + rp.metric_entry);
}
