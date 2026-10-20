/* status.h
 *
 * 音频层的内部返回码。与公开头 include/CLV_Audio.h 的 CLV_AudioError 在 32..47 这一段逐项同序，
 * 门面层直接 static_cast，不做映射表；顺序由门面里的 static_assert 钉住。
 *
 * 同序的范围要说准：内部枚举只覆盖「实现层会产出的码」，止于 47 = 拒解；公开段尾追加的 48 / 49
 * 只在门面（承载块解读口）直接产出，不经内部枚举，因此不在本表范围内——这两格的数值由门面显式断言钉。
 * 41 / 45 是公开段的留白号位（不复用），实现里无产出点，内部枚举同样不设对应项。
 *
 * 值不是从 1 连续排：成功是 0，失败码整段落进音频段（32 起），
 * 段内只追加不改序、不复用，且不得跨头比对数值。
 */
#ifndef CLV_AUDIO_STATUS_H
#define CLV_AUDIO_STATUS_H

namespace clv
{
	namespace audio
	{

		enum class AudioErr : unsigned char
		{
			kOk = 0,
			kInvalidHandle = 32,
			kInvalidArgument,
			kValueRange,
			kFrameHeaderBad,
			kFieldMismatch,
			kBlocksizeOutOfRange,
			kStreamInfoAbsent,
			kMetadataBad,
			kBlockTypeUnsupported,
			kTooLarge = 42,
			kStateError,
			kOutOfMemory,
			kBackendFailure = 46,
			kStreamRejected
		};

	}	 // namespace audio
}	 // namespace clv

#endif	  // CLV_AUDIO_STATUS_H
