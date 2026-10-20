/* direct_scale.h
 *
 * B-VOP direct 模式的同位块矢量缩放：
 *
 * ~~~
 * MVF = (TRB × MV_ref) / TRD
 * MVB = MVF − MV_ref
 * ~~~
 *
 * 除法语义 = 整数除法、结果向零截断（与 C++ 整型 `/` 同，两侧不各写一份取整规则）。
 * 乘与除一律按 64 位有符号做，结果按 32 位矢量分量交付；全式只在除法处舍入一次，
 * 故 `MVB` 由已舍入的 `MVF` 派生，不再对 `(TRB − TRD) × MV_ref` 另舍一次。
 *
 * 入口只判两件事：`TRD ≤ 0`（除零与非正时间基）与 `TRB > TRD`。`TRB ≤ 0` 属帧级条件，
 * 由帧级时间基准保证（前向参考 PTS ≤ 本帧 ≤ 后向参考 PTS，两者都取自驻留参考帧），本函数不代判。
 * 同位块是 intra、是 skip（无 MV）或缺位时，调用方传零矢量进来 —— 本函数不认识同位块。
 */
#ifndef CLV_VIDEO_DIRECT_SCALE_H
#define CLV_VIDEO_DIRECT_SCALE_H

#include "macroblock.h"
#include "status.h"

#include <cstdint>

namespace clv
{
	namespace video
	{

		struct DirectScaled
		{
			MotionVector forward;
			MotionVector backward;
		};

		VideoErr DirectScale(MotionVector mv_ref, int64_t trb, int64_t trd, DirectScaled* out);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_DIRECT_SCALE_H
