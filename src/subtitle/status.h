/* 字幕面内部码 → 公开错误码的单一落点。
 *
 * 内部码不另立一套语义：凡是能出到调用方的失败，一律用公开枚举 CLV_SUBTITLE_*；
 * 本头只提供短别名并 static_assert 钉住取值，防止内部件与公开头各写一套值。
 */
#ifndef CLV_SUBTITLE_STATUS_H
#define CLV_SUBTITLE_STATUS_H

#include "CLV_Subtitle.h"

namespace clv
{
	namespace subtitle
	{
		using Err = CLV_SubtitleError;

		inline constexpr Err kOk = CLV_SUBTITLE_OK;
		inline constexpr Err kInvalidArgument = CLV_SUBTITLE_INVALID_ARGUMENT;
		inline constexpr Err kInvalidHandle = CLV_SUBTITLE_INVALID_HANDLE;
		inline constexpr Err kInvalidUtf8 = CLV_SUBTITLE_INVALID_UTF8;
		inline constexpr Err kPacketHeaderBad = CLV_SUBTITLE_PACKET_HEADER_BAD;
		inline constexpr Err kPacketTooLarge = CLV_SUBTITLE_PACKET_TOO_LARGE;
		inline constexpr Err kOutOfMemory = CLV_SUBTITLE_OUT_OF_MEMORY;
		inline constexpr Err kState = CLV_SUBTITLE_STATE;
		inline constexpr Err kBackendFailure = CLV_SUBTITLE_BACKEND_FAILURE;
		inline constexpr Err kRenderFailed = CLV_SUBTITLE_RENDER_FAILED;
		inline constexpr Err kUnsupported = CLV_SUBTITLE_UNSUPPORTED;
		inline constexpr Err kOutputTooSmall = CLV_SUBTITLE_OUTPUT_TOO_SMALL;
		inline constexpr Err kStyleBlockBad = CLV_SUBTITLE_STYLE_BLOCK_BAD;

		static_assert(static_cast<int>(kOk) == 0, "OK 必须为 0");
		static_assert(static_cast<int>(kInvalidArgument) == 106, "段首必须是 106");
		static_assert(static_cast<int>(kStyleBlockBad) == 117, "段尾必须是 117");
		static_assert(static_cast<int>(kPacketHeaderBad) == static_cast<int>(kInvalidArgument) + 3, "106–117 逐项同序");
	}	 // namespace subtitle
}	 // namespace clv

#endif	  // CLV_SUBTITLE_STATUS_H
