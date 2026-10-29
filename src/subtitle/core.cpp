#include "core.h"

#include <cstring>

namespace clv
{
	namespace subtitle
	{
		bool Utf8Valid(const uint8_t* data, size_t size)
		{
			if (data == nullptr && size != 0u) return false;
			size_t i = 0;
			while (i < size)
			{
				const uint8_t b0 = data[i];
				if (b0 < 0x80u)
				{
					i += 1u;
					continue;
				}
				size_t need = 0;
				uint32_t cp = 0;
				uint32_t lower = 0;
				uint32_t upper = 0;
				if (b0 >= 0xC2u && b0 <= 0xDFu)
				{
					need = 1;
					cp = static_cast<uint32_t>(b0 & 0x1Fu);
					lower = 0x80u;
					upper = 0x7FFu;
				}
				else if (b0 >= 0xE0u && b0 <= 0xEFu)
				{
					need = 2;
					cp = static_cast<uint32_t>(b0 & 0x0Fu);
					lower = 0x800u;
					upper = 0xFFFFu;
				}
				else if (b0 >= 0xF0u && b0 <= 0xF4u)
				{
					need = 3;
					cp = static_cast<uint32_t>(b0 & 0x07u);
					lower = 0x10000u;
					upper = 0x10FFFFu;
				}
				else
				{
					return false; /* 0x80–0xC1、0xF5+：overlong 首字节与越界首字节 */
				}

				/* 序列占到 i..i+need，末格须在 size 之内：等号那档就是越界一格读（截断序列） */
				if (i + need >= size) return false;
				for (size_t k = 1; k <= need; ++k)
				{
					const uint8_t bc = data[i + k];
					if ((bc & 0xC0u) != 0x80u) return false;
					cp = (cp << 6) | static_cast<uint32_t>(bc & 0x3Fu);
				}
				if (cp < lower || cp > upper) return false;
				if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
				i += need + 1u;
			}
			return true;
		}

		namespace
		{
			void EmitBytes(const uint8_t* src, size_t len, uint8_t* out, size_t out_cap, size_t* w)
			{
				for (size_t k = 0; k < len; ++k)
				{
					if (*w < out_cap) out[*w] = src[k];
					*w += 1u;
				}
			}

			void EmitSpan(CLV_SubtitleSpanC* spans, size_t span_cap, size_t* ns, uint32_t off, uint32_t len,
						  uint32_t kind)
			{
				if (len == 0u) return; /* 空段不产出 */
				if (*ns < span_cap)
				{
					spans[*ns].off = off;
					spans[*ns].len = len;
					spans[*ns].kind = kind;
					spans[*ns].reserved0 = 0u;
				}
				*ns += 1u;
			}

			/* 一条指令的内容（`[\` 之后、`]` 之前）落成输出 */
			void EmitDirective(const uint8_t* body, size_t len, uint8_t* out, size_t out_cap, size_t* w,
							   CLV_SubtitleSpanC* spans, size_t span_cap, size_t* ns)
			{
				if (len == 0u || body[0] != static_cast<uint8_t>('\\'))
				{
					EmitBytes(body, len, out, out_cap, w);
					return;
				}
				const uint8_t* rest = body + 1u;
				const size_t rest_len = len - 1u;
				size_t name_len = 0;
				while (name_len < rest_len && rest[name_len] != static_cast<uint8_t>(' ')) ++name_len;
				const bool has_param = name_len < rest_len; /* 第一个空格是分隔符，被消费 */
				const uint8_t* param = has_param ? rest + name_len + 1u : rest;
				const size_t param_len = has_param ? rest_len - name_len - 1u : 0u;

				if (name_len == 1u && rest[0] == static_cast<uint8_t>('n') && ! has_param)
				{
					EmitBytes(reinterpret_cast<const uint8_t*>("\n"), 1u, out, out_cap, w);
					return;
				}
				if (name_len == 1u && rest[0] == static_cast<uint8_t>('\\') && ! has_param)
				{
					EmitBytes(reinterpret_cast<const uint8_t*>("\\"), 1u, out, out_cap, w);
					return;
				}
				if (name_len == 5u && std::memcmp(rest, "emoji", 5u) == 0)
				{
					const size_t start = *w;
					EmitBytes(param, param_len, out, out_cap, w);
					EmitSpan(spans, span_cap, ns, static_cast<uint32_t>(start), static_cast<uint32_t>(*w - start),
							 CLV_SUBTITLE_SPAN_EMOJI);
					return;
				}
				EmitBytes(body, len, out, out_cap, w);
			}
		}	 // namespace

		size_t DecodeText(const uint8_t* in, size_t size, uint8_t* out, size_t out_cap, CLV_SubtitleSpanC* spans,
						  size_t span_cap, size_t* span_count)
		{
			size_t w = 0;
			size_t ns = 0;
			if (span_count != nullptr) *span_count = 0u;
			if (in == nullptr) return 0;

			size_t i = 0;
			while (i < size)
			{
				if (! (in[i] == static_cast<uint8_t>('[') && i + 1u < size && in[i + 1u] == static_cast<uint8_t>('\\')))
				{
					EmitBytes(in + i, 1u, out, out_cap, &w);
					i += 1u;
					continue;
				}
				const size_t body_start = i + 2u;
				size_t k = body_start;
				while (k < size && in[k] != static_cast<uint8_t>(']')) ++k;
				if (k >= size) break; /* 无封闭 `]`：丢到本次输入末尾 */
				EmitDirective(in + body_start, k - body_start, out, out_cap, &w, spans, span_cap, &ns);
				i = k + 1u;
			}
			if (span_count != nullptr) *span_count = ns;
			return w;
		}
	}	 // namespace subtitle
}	 // namespace clv
