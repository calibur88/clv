/* status.h
 *
 * 视频层的内部返回码。与公开头 include/CLV_Video.h 的 CLV_VideoError 逐项同序，
 * 门面层直接转值，不做映射表；顺序由门面里的 static_assert 钉住。
 * 成功是 0，失败码整段落进视频段（64 起），段内只追加不改序、不复用，不得跨头比对数值。
 *
 * 同一种不成立形状在编码侧与解码侧走不同码，是分域不是漂移：
 *   编码侧的输入是**调用方递过来的结构体**，字段互相矛盾 = 入参不成立 → kValueRange；
 *   解码侧的输入是**码流字节**，同一形状出现在位流里 = 码流不成立 → kHeaderBad。
 * 两侧对同一形状的判定条件本身必须逐条一致（例如 `not_coded = 1` 时 `cbpy` 与 `block_size` 都须为 0），
 * 只有出口码按域分档。新增判据时两侧同批改，别只改一边。
 */
#ifndef CLV_VIDEO_STATUS_H
#define CLV_VIDEO_STATUS_H

namespace clv
{
	namespace video
	{

		enum class VideoErr : unsigned char
		{
			kOk = 0,
			kInvalidHandle = 64,
			kInvalidArgument,
			kValueRange,
			kPayloadTooSmall,
			kHeaderBad,
			kQuantOutOfRange,
			kBitDepthUnsupported,
			kToolExcluded,
			kToolListAbsent,
			kSliceHeaderBad = 74,
			kSliceSequence,
			kRefFrameMissing,
			kOutputTooSmall,
			kStateError = 79,
			kOutOfMemory,
			/* 81 / 82 两格留白不复用（两档语义在本版本都没有产出点），所以段尾显式给值 */
			kStreamRejected = 83,
			/* 量化越界的两侧分档：kQuantOutOfRange 只由编码器声明入口产（default_quant 越界），
			 * 逐调用侧（Encode 的 q 越界、无损模式带非零 q）走本码。两侧都判参数不成立但读者要能分开：
			 * 声明侧 = 这套句柄根本不该建，逐调用侧 = 单帧请求被拒、句柄与流都不受影响。 */
			kQuantOutOfRangeCall = 84,
			/* 载荷长度不符（含过长与过短）：段尾追加，长度**超出**该块规定定长那一支产本码；
			 * 短于定长仍走 kPayloadTooSmall（那个名字钉死的是「太短」）。 */
			kPayloadLengthMismatch = 85
		};

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_STATUS_H
