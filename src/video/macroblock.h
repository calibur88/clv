/* macroblock.h
 *
 * 宏块头两套：I / P 型（变长码，码表见 `annex_b_tables_p.h`；字段序 = 头标记 → 块模式 → 量化偏移 → 运动矢量残差 → 块数据长度）
 * 与 B-VOP 型（CLV 自研的字节对齐语法）。
 *
 * I / P 字段序：not_coded(1 bit) → mcbpc → cbpy → dquant(2 bit) →〔motion_vector()〕→ block_size(varint)。
 * mcbpc 一张表给两件事：宏块类型 + 色度块模式 cbpc（2 bit）；cbpy 给四个亮度块的块模式（4 bit）。
 * dquant 是宏块级量化偏移，2 bit 二补码（00 = 0，01 = +1，10 = −2，11 = −1），每个已编码宏块都带，
 * 与帧级偏移叠加：q' = clamp(q + d_vop + d_mb, 1, 2^quant_precision - 1)。
 * 外部原表靠该字段区分「+量化偏移」型；CLV 既然无条件带它，mcbpc 表里就不留 `+q` 行（见 annex_b_tables_p.h）。
 *
 * B-VOP 字段序：not_coded(1 B) → mb_type(1 B) → mvd[]（zigzag varint 残差）→ cbpy(varint) → block_size(varint)。
 * mvd 个数从 mb_type 推，不落计数域：0 direct = 0 个，1 interpolate = 2 个，2 backward / 3 forward = 1 个。
 * 残差相对三候选中值预测器，前向 / 后向各一套状态；外部标准的 B-VOP 用的是「同类型最近解码矢量、
 * 每宏块行首清零」那套预测器，这里按中值写，本库位流不与外部解码器互通。direct 只缩放同位块矢量，无残差。
 *
 * 两条失败路径不得混用：mcbpc 表里保留的排除集行（`mb_type = 2` 的 4MV、Stuffing）与 B-VOP 的
 * `mb_type ≥ 4` 判 kToolExcluded（识别出「这是被排除的工具」）；两张表都不含的码字判 kHeaderBad。
 */
#ifndef CLV_VIDEO_MACROBLOCK_H
#define CLV_VIDEO_MACROBLOCK_H

#include "bits.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		// vop_type 与 VopHeader::type 同值：0 = I，1 = P，2 = B
		inline constexpr uint8_t kVopTypeI = 0u;
		inline constexpr uint8_t kVopTypeP = 1u;
		inline constexpr uint8_t kVopTypeB = 2u;

		// 宏块类型名（取自外部原表的 MB type 编号；1 = inter+q 与 4 = intra+q 在 CLV 表里没有行）
		inline constexpr int8_t kMbTypeInter = 0;
		inline constexpr int8_t kMbTypeInter4V = 2;
		inline constexpr int8_t kMbTypeIntra = 3;

		// B-VOP 的 mb_type；4..255 保留
		inline constexpr uint8_t kBvopDirect = 0u;
		inline constexpr uint8_t kBvopInterpolate = 1u;
		inline constexpr uint8_t kBvopBackward = 2u;
		inline constexpr uint8_t kBvopForward = 3u;
		inline constexpr uint8_t kBvopMbTypeMax = 3u;

		// 宏块级量化偏移：2 bit 二补码，域 -2..+1
		inline constexpr int8_t kMbDquantMin = -2;
		inline constexpr int8_t kMbDquantMax = 1;

		/* 帧内宏块查 cbpy 的 intra 列，其余查 inter 列。 */
		bool IsIntraMacroblock(int8_t mb_type) noexcept;

		int8_t MbDquantFromBits(uint8_t two_bits) noexcept;
		uint8_t MbDquantToBits(int8_t d_mb) noexcept;

		/* 运动矢量：**半像素单位**（偶值 = 整像素、奇值 = 半像素），两分量按 32 位有符号交付。
		 * 帧内宏块与 not_coded 宏块不承载它，取默认零值。 */
		struct MotionVector
		{
			int32_t h = 0;
			int32_t v = 0;
		};

		/* 矢量分量落回 32 位的唯一收口式：越上界取 INT32_MAX、越下界取 INT32_MIN，不静默回绕。
		 * direct 缩放的乘除结果与 mvd 残差相加后的绝对矢量都走这一处，两侧不各写一份。 */
		int32_t SaturateVectorComponent(int64_t v) noexcept;

		struct MacroblockHeader
		{
			bool not_coded = false;
			int8_t mb_type = kMbTypeInter;
			uint8_t cbpc = 0u;			 // 色度块模式，2 bit
			uint8_t cbpy = 0u;			 // 四个亮度块的块模式，4 bit
			int8_t dquant = 0;			 // 宏块级量化偏移，域 -2..+1
			uint32_t block_size = 0u;	 // 块数据字节数；not_coded = 1 时必须为 0
			/* 每宏块 1 个运动矢量。
			 * 只在「帧间且 not_coded = 0」时在场并由码流承载（not_coded 与帧内宏块都不带 motion_vector()）：
			 * 帧内是「压根没有矢量」，not_coded 是「矢量恒零且照样参与预测」——两者差别在推进预测器时成立。 */
			MotionVector mv {};
		};

		/* 三候选逐分量取中值。无效候选（越界、缺位、邻居不带该方向）由调用方先置零再参与中值。 */
		MotionVector MedianPredictor(MotionVector a, MotionVector b, MotionVector c) noexcept;

		/* B-VOP 两方向各挂一套预测器状态。 */
		struct BvopPrediction
		{
			MotionVector forward;
			MotionVector backward;
		};

		struct BvopMacroblockHeader
		{
			bool not_coded = false;
			uint8_t mb_type = kBvopDirect;
			MotionVector forward {};	// 绝对矢量：解码时 = 预测 + 残差
			MotionVector backward {};
			uint8_t cbpy = 0u;
			uint32_t block_size = 0u;	 // 块数据字节数；not_coded = 1 时必须为 0
		};

		/* 该 mb_type 带几个方向的 mvd；保留值返回 0。 */
		size_t BvopMvdCount(uint8_t mb_type) noexcept;

		/* 语法：每个**帧间**宏块在 dquant 之后、block_size 之前带一个 `motion_vector()`
		 * —— 两条 zigzag varint 残差，相对三候选中值预测器；帧内宏块与 not_coded 宏块不带。
		 * 预测器状态跨宏块延续、但在每次层函数调用内新建（多片路径逐片调用 ≈ 片界重置），
		 * 故这两个函数要拿到 `pred`，值由各自的层函数按 mb_width 推 —— 调用方不自己算。 */
		VideoErr EncodeMacroblockHeader(BitWriter* bw, uint8_t vop_type, const MacroblockHeader& mb,
										const MotionVector& pred);
		/* 读到排除集或表外码字：*out 不被填成可用值，调用方按拒该帧处置。 */
		VideoErr DecodeMacroblockHeader(BitReader* br, uint8_t vop_type, MacroblockHeader* out,
										const MotionVector& pred);

		VideoErr EncodeBvopMacroblockHeader(BitWriter* bw, const BvopMacroblockHeader& mb, const BvopPrediction& pred);
		VideoErr DecodeBvopMacroblockHeader(BitReader* br, BvopMacroblockHeader* out, const BvopPrediction& pred);

		/* 一个宏块 = 宏块头 + 块数据（块数据长度由头里的 block_size 自定界）。 */
		struct Macroblock
		{
			MacroblockHeader head;
			std::vector<uint8_t> block;
		};

		struct BvopMacroblock
		{
			BvopMacroblockHeader head;
			std::vector<uint8_t> block;
		};

		/* I / P 宏块序列：mb_count 个宏块首尾相接。写方要求 head.block_size == block.size()。
		 * 读方按 mb_count 解满后，剩余整字节视为码流不成立；不足一字节的尾位是补齐位，不校验取值。 */
		VideoErr EncodeMacroblockLayer(uint8_t vop_type, uint32_t mb_width, const std::vector<Macroblock>& mbs,
									   std::vector<uint8_t>* out);
		VideoErr DecodeMacroblockLayer(const uint8_t* data, size_t size, uint8_t vop_type, uint32_t mb_width,
									   uint64_t mb_count, std::vector<Macroblock>* out);

		/* B-VOP 宏块序列：预测器状态跨宏块延续，前向 / 后向各一套。
		 * 候选取同层已解码宏块的 A（左）/ B（上）/ C（右上）三个位置，越界、缺位或该邻居不带该方向时按零参与。
		 * 编解两端走同一个候选与中值实现，不各写一份。 */
		/* 解 **count 个**宏块并回报吃掉的字节数（向上取整到字节，尾位是补齐位）。
		 * 多片载荷的片界不靠额外字段，就靠这个 consumed 推：本片解完，下一片的片头从 consumed 处开始。
		 * 不足 count 个就耗尽 → kPayloadTooSmall。 */
		VideoErr DecodeMacroblockRun(const uint8_t* data, size_t size, uint8_t vop_type, uint32_t mb_width,
									 uint64_t count, std::vector<Macroblock>* out, size_t* consumed);
		VideoErr DecodeBvopRun(const uint8_t* data, size_t size, uint32_t mb_width, uint64_t count,
							   std::vector<BvopMacroblock>* out, size_t* consumed);

		/* 预测器状态在每次调用内新建、调用结束即弃 —— 多片路径逐片调用，等价于「片界重置」。 */
		VideoErr EncodeBvopLayer(uint32_t mb_width, const std::vector<BvopMacroblock>& mbs, std::vector<uint8_t>* out);
		VideoErr DecodeBvopLayer(const uint8_t* data, size_t size, uint32_t mb_width, uint64_t mb_count,
								 std::vector<BvopMacroblock>* out);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_MACROBLOCK_H

