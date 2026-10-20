#include "direct_scale.h"

namespace clv
{
	namespace video
	{

		namespace
		{

			// 这里的 `/`：整数除法，结果向零截断。C++ 的 / 对负数同样是向零截断（C++11 起标准保证）。
			int64_t DivTruncTowardZero(int64_t a, int64_t b) { return a / b; }

		}	 // namespace

		VideoErr DirectScale(MotionVector mv_ref, int64_t trb, int64_t trd, DirectScaled* out)
		{
			if (out == nullptr) return VideoErr::kInvalidArgument;
			if (trd <= 0) return VideoErr::kValueRange;
			if (trb > trd) return VideoErr::kValueRange;

			const int64_t mf_x = DivTruncTowardZero(trb * static_cast<int64_t>(mv_ref.h), trd);
			const int64_t mf_y = DivTruncTowardZero(trb * static_cast<int64_t>(mv_ref.v), trd);
			/* 落回 32 位走全库唯一那式（饱和），与 mvd 残差相加同一口径。
			 * 帧级判据成立时（0 ≤ TRB ≤ TRD）|MVF| ≤ |MV_ref|、|MVB| ≤ |MV_ref|，收口点不会被触到；
			 * 触到即说明调用方给了越界的 TRB / TRD 或同位矢量。 */
			out->forward.h = SaturateVectorComponent(mf_x);
			out->forward.v = SaturateVectorComponent(mf_y);
			out->backward.h = SaturateVectorComponent(mf_x - static_cast<int64_t>(mv_ref.h));
			out->backward.v = SaturateVectorComponent(mf_y - static_cast<int64_t>(mv_ref.v));
			return VideoErr::kOk;
		}

	}	 // namespace video
}	 // namespace clv
