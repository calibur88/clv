/* 文本层：整包 UTF-8 校验与 `[\` 转义解码。
 *
 * UTF-8 判的是整包：一包里任一处不合法即整包弃用，判 CLV_SUBTITLE_INVALID_UTF8（108）。
 * 转义词法 = `[\` 前缀 + `\` 指令标记 + 指令名 + 参数，读到第一个 `]`；
 * `n` → U+000A，`\` → 字面 `\`，`emoji <序列>` → 整段参数标成 emoji 段，其余原样输出。
 * 找不到 `]` ⇒ 从 `[\` 起丢到本次输入末尾，静默、不计数。
 */
#ifndef CLV_SUBTITLE_CORE_H
#define CLV_SUBTITLE_CORE_H

#include "status.h"

#include <stddef.h>
#include <stdint.h>

namespace clv
{
	namespace subtitle
	{
		bool Utf8Valid(const uint8_t* data, size_t size);

		/* 返回「无限容量下会产出」的字节数；写入按 out_cap 截断，不越界。
		 * 段的偏移与长度同口径（无限容量下的值），*span_count 是产出的段数。 */
		size_t DecodeText(const uint8_t* in, size_t size, uint8_t* out, size_t out_cap, CLV_SubtitleSpanC* spans,
						  size_t span_cap, size_t* span_count);
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_CORE_H
