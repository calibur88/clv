/* annex_b_tables_p.h
 *
 * 宏块头用的三张变长码表（mcbpc 两张 + cbpy 一张），是外部标准宏块层码表裁出来的本库版本：
 *   kMcbpcIVop（I-VOP 的 mcbpc）、kMcbpcPVop（P-VOP 的 mcbpc）、
 *   kCbpyFourBlocks（四块全不透明时那张 cbpy，帧内查 intra 列、帧间查 inter 列）。
 * mcbpc 一行给出「宏块类型 + 色度块模式」，码字最长 9 bit；mb_type = -1 是 Stuffing 码。
 * 两张 mcbpc 表不含 `+q` 行（mb_type 1 = inter+q、4 = intra+q）：CLV 的宏块头无条件带
 * 2 bit 宏块级量化偏移，`+q` 与不带 `q` 的差别不存在，留着只会让同一宏块有两种写法。
 * mb_type = 2（inter4v，4MV）与 Stuffing 行必须留在表里：它们判「被排除的工具」，
 * 表外码字判另一档错（kToolExcluded 与 kHeaderBad），两条报错路径不得混。kMcbpcIVopDroppedQRows /
 * kMcbpcPVopDroppedQRows 存的是裁掉的那几行（宏块类型带「+量化偏移」标记的 `+q` 行），不属于 CLV 码流语法，喂进解码器应判表外码字。
 * cbpy 的 intra / inter 两列逐行互为按位取反。
 */
#ifndef CLV_VIDEO_ANNEX_B_TABLES_P_H
#define CLV_VIDEO_ANNEX_B_TABLES_P_H

#include <cstdint>

namespace clv
{
	namespace video
	{

		struct McbpcEntry
		{
			uint8_t code_len;	 // 码字位数，1..9
			uint16_t code;		 // 码字的整数值：低 code_len 位有效，先读的位在高位
			int8_t mb_type;		 // -1 = Stuffing
			/* 色度块模式，2 bit；Stuffing 行为 0。
			 * 本列的取值按 CLV 合成块图的低 2 位读：bit1 = Cb、bit0 = Cr、00 = 两色度块都不在场。
				 * 这与外部原表 cbpc 列的 4:2:0 色度块模式码语义相反（该列 00 = 两块都在场、11 = 无），
				 * 搬过来的只是码字行，列语义随 CLV 的合成位序定 —— 编解两侧同表同合成式，本库位流不与外部解码器互通。 */
			uint8_t cbpc;
		};

		struct CbpyEntry
		{
			uint8_t code_len;	 // 码字位数，2..6
			uint16_t code;		 // 同上
			uint8_t intra;		 // 帧内宏块的 4 bit 亮度块模式
			uint8_t inter;		 // 帧间宏块的 4 bit 亮度块模式
		};

		inline constexpr McbpcEntry kMcbpcIVop[] = {
			{ 1, 0x001, 3,	0 }, // 1
			{ 3, 0x001, 3,	1 }, // 001
			{ 3, 0x002, 3,	2 }, // 010
			{ 3, 0x003, 3,	3 }, // 011
			{ 9, 0x001, -1, 0 }, // 000000001
		};

		inline constexpr McbpcEntry kMcbpcPVop[] = {
			{ 1, 0x001, 0,	0 }, // 1
			{ 4, 0x003, 0,	1 }, // 0011
			{ 4, 0x002, 0,	2 }, // 0010
			{ 6, 0x005, 0,	3 }, // 000101
			{ 3, 0x002, 2,	0 }, // 010
			{ 7, 0x005, 2,	1 }, // 0000101
			{ 7, 0x004, 2,	2 }, // 0000100
			{ 8, 0x005, 2,	3 }, // 00000101
			{ 5, 0x003, 3,	0 }, // 00011
			{ 8, 0x004, 3,	1 }, // 00000100
			{ 8, 0x003, 3,	2 }, // 00000011
			{ 7, 0x003, 3,	3 }, // 0000011
			{ 9, 0x001, -1, 0 }, // 000000001
		};

		// 下面两张表不属于 CLV 码流语法：它们是外部原表里的 `+q` 行，
		// 逐个喂回解码器必须判「表外码字」，不得静默命中别的码。
		// 本库的编码与解码路径都不查这两张表（查的只有上面三张），留在库内是作对照表用：
		// 判档口径要由同一件头里的表来钉，挪到别处就变成两套表、两侧各读各的。
		inline constexpr McbpcEntry kMcbpcIVopDroppedQRows[] = {
			{ 4, 0x001, 4, 0 }, // 0001
			{ 6, 0x001, 4, 1 }, // 000001
			{ 6, 0x002, 4, 2 }, // 000010
			{ 6, 0x003, 4, 3 }, // 000011
		};

		inline constexpr McbpcEntry kMcbpcPVopDroppedQRows[] = {
			{ 3, 0x003, 1, 0 }, // 011
			{ 7, 0x007, 1, 1 }, // 0000111
			{ 7, 0x006, 1, 2 }, // 0000110
			{ 9, 0x005, 1, 3 }, // 000000101
			{ 6, 0x004, 4, 0 }, // 000100
			{ 9, 0x004, 4, 1 }, // 000000100
			{ 9, 0x003, 4, 2 }, // 000000011
			{ 9, 0x002, 4, 3 }, // 000000010
		};

		inline constexpr CbpyEntry kCbpyFourBlocks[] = {
			{ 4, 0x003, 0x0, 0xF }, // 0011
			{ 5, 0x005, 0x1, 0xE }, // 00101
			{ 5, 0x004, 0x2, 0xD }, // 00100
			{ 4, 0x009, 0x3, 0xC }, // 1001
			{ 5, 0x003, 0x4, 0xB }, // 00011
			{ 4, 0x007, 0x5, 0xA }, // 0111
			{ 6, 0x002, 0x6, 0x9 }, // 000010
			{ 4, 0x00B, 0x7, 0x8 }, // 1011
			{ 5, 0x002, 0x8, 0x7 }, // 00010
			{ 6, 0x003, 0x9, 0x6 }, // 000011
			{ 4, 0x005, 0xA, 0x5 }, // 0101
			{ 4, 0x00A, 0xB, 0x4 }, // 1010
			{ 4, 0x004, 0xC, 0x3 }, // 0100
			{ 4, 0x008, 0xD, 0x2 }, // 1000
			{ 4, 0x006, 0xE, 0x1 }, // 0110
			{ 2, 0x003, 0xF, 0x0 }, // 11
		};

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_ANNEX_B_TABLES_P_H
