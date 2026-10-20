/* headers.h
 *
 * 视频载荷的两个头：VOP 帧头与分片片头，以及容器层只搬运不解释的 `ext_type = 3` 元数据块。
 * 这里的字节布局是全库唯一权威：读写都走本文件的 Encode / Decode 函数，别的层不另解释字段。
 *
 * VOP 帧头（偏移自载荷起）：
 *   0  vop_type           0 = I，1 = P，2 = B
 *   1  vop_quant          量化步长 q，三段域：0 = 无损（系数直载变换原值）/ 1..2^quant_precision - 1 = 有损 /
 *                          其余不成立。v1 的有损段就是 [1, 127]
 *   2  vop_fcode_forward  1..7 原值，0 非法
 *   3  vop_fcode_backward 1..7 原值，0 非法，只对 B 帧有意义
 *   4  vop_flags          bit0-2 = intra_dc_vlc_thr（每帧重载）；bit3 = 顶场优先；
 *                          bit4 = 隔行扫描；bit5-7 = 系数编码法（`block.h` 的 CoefCoding）。
 *                          v1 的 bit3 / bit4 必须为 0，bit5-7 只认已赋义的 0 与 1，读到其余取值判帧头不成立
 *   5  vop_dquant         bit0-4 = 带符号的帧级量化偏移（域 -16..+15），与宏块头里的 2 bit 宏块级偏移叠加；
 *                          bit5-7 = reserved，必须为 0
 *   6  mb_count           varint（LEB128，最短形式）
 *   …  宏块数据
 *
 * 分片片头（只在真分片时出现；单包帧不发片头）：
 *   0  frame_id          4 字节小端，避免 2 字节回绕
 *   4  fragment_count    2 字节小端，本片所属帧的总片数，≤ 65535
 *   6  flags             bit0 = 末片，bit1 = 首片，bit2 = 携带 vop_quant，bit3-7 = reserved
 *   7  vop_type
 *   8  vop_quant         仅 bit2 = 1（首片）时出现
 *   9  first_mb          varint，本片第一个宏块序号
 *   …  该片宏块数据
 *   片序不在片头里：它唯一落在容器外层包头的 fragment_index。片头的 bit0 / bit1 是冗余声明，
 *   与容器外层同时出现时以容器外层为权威，两者矛盾即按片头不成立处置。
 */
#ifndef CLV_VIDEO_HEADERS_H
#define CLV_VIDEO_HEADERS_H

#include "block.h"
#include "quant.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		struct VopHeader
		{
			uint8_t type = 0;	 // 0 = I，1 = P，2 = B
			uint8_t quant = 1;
			uint8_t fcode_forward = 1;
			uint8_t fcode_backward = 1;
			uint8_t intra_dc_vlc_thr = 0;	 // 3 bit
			/* 系数编码法：本帧宏块数据里那批系数按哪一法写。它是码流事实、逐帧携带，
			 * 块层与宏块层都不各自解释 —— 读写只在本文件收口，取档由编码器侧的编译期常量定。 */
			CoefCoding coef_coding = CoefCoding::kFixed;
			int8_t dquant = 0;	  // 5 bit 带符号，域 -16..+15
			uint64_t mb_count = 0;
		};

		struct SliceHeader
		{
			uint32_t frame_id = 0;
			uint16_t fragment_count = 0;
			bool is_last = false;
			bool is_first = false;
			bool has_vop_quant = false;
			uint8_t vop_type = 0;
			uint8_t vop_quant = 0;	  // 仅 has_vop_quant 时有意义
			uint64_t first_mb = 0;
			/* 该片实际宏块数（片头变量部分的第二个 varint）。**必须 ≥ 1** —— 0 是空片，编解两侧都判不成立。
			 * 它是码流事实，与编码器参数 `slice_mb_count`（每片的配置上限）不是一回事：
			 * 解码器只认这里的值，对配置值完全不感知。 */
			uint64_t mb_run = 0;
		};

		struct EncMeta
		{
			uint8_t max_b_frames = 0;
			uint8_t quant_precision = 7;
			uint8_t ext_tools = 0;
			uint8_t reserved = 0;
			uint16_t display_width = 0;	   // 0 = 未声明
			uint16_t display_height = 0;
		};

		/* out 会被清空后写入。 */
		bool EncodeVopHeader(const VopHeader& h, uint8_t quant_precision, std::vector<uint8_t>* out);
		bool DecodeVopHeader(const uint8_t* data, size_t size, uint8_t quant_precision, VopHeader* out,
							 size_t* consumed);

		bool EncodeSliceHeader(const SliceHeader& h, uint8_t quant_precision, std::vector<uint8_t>* out);
		/* first_authoritative / last_authoritative 取自容器外层包头：片头的对应位与之矛盾即不成立。
		 * quant_precision 是本流的声明值：片头携带的 vop_quant 与帧头同域，界按 2^quant_precision − 1 取。 */
		bool DecodeSliceHeader(const uint8_t* data, size_t size, uint8_t quant_precision, bool first_authoritative,
							   bool last_authoritative, SliceHeader* out, size_t* consumed);

		bool ParseEncMeta(const uint8_t* data, size_t size, EncMeta* out);

		/* 有效量化步长：q' = clamp(q + d_vop + d_mb, 1, 2^quant_precision - 1)。
		 * d_vop 是帧级偏移（域 -16..+15），d_mb 是宏块级偏移（域 -2..+1），v1 两级叠加，不做逃生路径。 */
		uint8_t EffectiveQuant(uint8_t q, int8_t d_vop, int8_t d_mb, uint8_t quant_precision);

		/* 本帧 / 本片段生效的量化步长，编解两侧唯一的取档口：
		 * 无损流（q = 0）原样给出 0 —— 量化步长、帧级偏移、宏块级偏移在有损式里才有意义，
		 * 无损流里三者都不参与算术，夹取与偏移一律不走。
		 * 有损流照 EffectiveQuant 的两级偏移叠加。任一侧绕过本函数直接取档，
		 * 无损流就会被偏移量折进有损档，两侧不再逐字节全等。 */
		inline uint8_t FrameQuant(uint8_t q, int8_t d_vop, int8_t d_mb, uint8_t quant_precision)
		{
			return IsLosslessQuant(q) ? kQuantLossless : EffectiveQuant(q, d_vop, d_mb, quant_precision);
		}

		/* 片数上限：片头里 fragment_count 是 2 字节，故 ≤ 65 535。
		 * 上限只由字段宽度推出，判定分片数越界处一律引用本常量；
		 * 任一处另写字面量，「分片数刚好越界」的判定就会各处不一致。 */
		inline constexpr uint32_t kMaxFragmentCount = 65535u;

		/* 帧级量化偏移 dquant 的合法域：5 bit 带符号，[−16, 15]。
		 * 与 `macroblock.h` 的宏块级 kMbDquantMin / kMbDquantMax（2 bit，[−2, +1]）是同族两档，
		 * 位宽不同故值域不同，两级偏移不得互换。 */
		inline constexpr int8_t kVopDquantMin = -16;
		inline constexpr int8_t kVopDquantMax = 15;

		/* 保留位掩码：这些位 v1 恒 0，读到非 0 即判码流不成立。
		 * 掩码是格式定义的一部分，散写则「保留位变了」要满仓找。
		 * vop_flags 的 bit5-7 已赋予「系数编码法」语义，不再是保留位，故本掩码只夹 bit3-4；
		 * 那 3 bit 的字段位置与合法取值域由下面三条一起定，读写两侧不各写一份位移。 */
		inline constexpr uint8_t kVopFlagsReservedMask = 0x18u;	   // vop_flags bit3-4
		inline constexpr uint8_t kVopCoefCodingMask = 0xE0u;	   // vop_flags bit5-7
		inline constexpr uint8_t kVopCoefCodingShift = 5u;
		/* 片头 flags 的保留位 = bit3-7，与帧头 vop_flags 是**两个字段、两条位序**，不共用常量：
		 * 帧头的 bit5-7 一旦赋予系数编码法语义，两处对「保留」的定义就此分岔，共用会让片头漏判。 */
		inline constexpr uint8_t kSliceFlagsReservedMask = 0xF8u;	 // 片头 flags bit3-7
		inline constexpr uint8_t kDquantByteReservedMask = 0xE0u;	 // dquant 字节的 bit5-7
		inline constexpr uint8_t kExtToolsReservedMask = 0xFEu;		 // ext_tools bit1-7

		/* 补后尺寸 = 原始尺寸向上取整到 16 的倍数，按 32 位无符号算（原始 65521..65535 → 65536）。 */
		uint32_t CodedSize(uint16_t orig);

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_HEADERS_H

