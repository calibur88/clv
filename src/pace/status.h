/* status.h
 *
 * 节器件层的内部返回码。与公开头 include/CLV_Pace.h 的 `CLV_PaceError` 在 96..101 这一段逐项同序，
 * 门面层直接 `static_cast`，不做映射表；同序由门面里的 `static_assert` 钉住。
 *
 * 值不是从 1 连续排：成功是 0，失败码整段落进节器件段（96 起），段内只追加不改序、不复用，
 * 且不得跨头比对数值（各面的号段互不相干这条由各头自己声明）。
 */
#ifndef CLV_PACE_STATUS_H
#define CLV_PACE_STATUS_H

namespace clv
{
	namespace pace
	{

		enum class PaceErr : unsigned char
		{
			kOk = 0,
			kInvalidHandle = 96,
			kInvalidArgument,
			kValueRange,
			kStateError,
			kClockSkew,
			kOutOfMemory,
		};

	}	 // namespace pace

}	 // namespace clv

#endif	  // CLV_PACE_STATUS_H
