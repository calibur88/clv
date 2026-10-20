/* block.h
 *
 * 块数据 = 量化后变换系数的载体。两种系数编码法并存，用哪一法是码流事实，由帧头 `vop_flags` bit5-7 给出。
 *
 *   块存在性  块模式位图（cbpy 4 bit + cbpc 2 bit 合成 6 bit，bit5..2 = Y0..Y3，bit1 = Cb，bit0 = Cr）
 *   系数顺序  自然行序 n = 0..63，(u, v) = (n % 8, n / 8)，n = 0 是 DC；不做 zigzag
 *   编码法    法 0（kFixed）= 每系数定宽直写，零照样占位；
 *             法 1（kEobEg）= 每块 `all_zero` 1 bit + `eob` 6 bit + 槽 0..eob 逐槽的 Exp-Golomb(order-0) 幅值，
 *                             幅值为 0 的槽不写符号位；`eob` = 自然行序里最后一个非零系数的下标
 *   每系数位  法 0：bits(q') 位 = 1 bit 符号（0 正 1 负）+ (bits(q') − 1) bit 幅值 = |QF|；
 *             法 1：符号位只在非零槽出现，幅值走变宽，bits(q') 只用来定 |QF| 的上界
 *   位宽来源  bits(q') = ceil(log2(QF_max(q') + 1)) + 1，按 q' 与块型（帧内 / 帧间）查七档表，
 *             **位宽不进码流**，两侧同式自算
 *   上界      |QF| ≤ QF_max(q')，越界判取值不成立（拒该宏块）
 *   非零      位图为 1 的块：法 0 读出 64 个全零判取值不成立（全零块的位必为 0）；
 *             法 1 的语法允许 all_zero = 1 写出整块全零，但 eob 位上的系数必须非零，否则判取值不成立
 *   块界      法 0：present_blocks × 单块字节数，present_blocks = 位图中 1 的个数；与宏块头 block_size 必须相等。
 *             法 1：块边界由 all_zero / eob 自终止，MB 级长度仍由宏块头 block_size varint 显式给出，
 *             解码侧校验 size ≤ present × BlockBytesMax（present = 0 时只接受 size = 0）
 *
 * 七档表由探针全域扫出并复核：帧内 7 档（起点 1/3/5/9/17/33/65）、
 * 帧间 7 档（起点 1/2/4/8/16/32/64）；除首档两侧同为 1 之外，帧间各档起点比帧内同序档低 1、位宽恰多 1。
 *
 * 每 MB 的块数据都以字节边界结束（写侧末尾统一对齐），位位置不跨宏块携带、更不跨片 ——
 * 片是各自独立的字节缓冲，片界因此是缓冲区边界而不是位位置，这条是片级并行能成立的前提。
 */
#ifndef CLV_VIDEO_BLOCK_H
#define CLV_VIDEO_BLOCK_H

#include "bits.h"
#include "quant.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		/* 一个宏块的块数：四个亮度块 + Cb + Cr。 */
		inline constexpr int kBlocksPerMacroblock = 6;

		/* 合成块模式的位序：bit5..bit2 = 亮度块 0..3，bit1 = Cb，bit0 = Cr。 */
		inline constexpr uint8_t kLumaPatternShift = 2u;
		inline constexpr uint8_t kChromaPatternMask = 0x03u;
		inline constexpr uint8_t kPatternMask6 = 0x3Fu;

		/* 系数编码法：值即 `vop_flags` bit5-7 里的那 3 bit，两侧不另建映射表。
		 * 2..7 未赋义，解码侧读到即拒 —— 留白位不复用，取值一旦写进码流就是格式。 */
		enum class CoefCoding : uint8_t
		{
			kFixed = 0u,	// 定宽直写，零也算载荷
			kEobEg = 1u,	// EOB 截尾 + Exp-Golomb 幅值，不带零游程
		};

		/* 已赋义的法顶格：字段里读到大于它的值就判帧头不成立。 */
		inline constexpr uint8_t kCoefCodingMax = 1u;

		/* 编码器写出哪一法 = 编译期事实，不占公开面入参（CMake 项 CLV_COEF_CODING 把同一个值
		 * 发给本仓每个 target，含 tests 与 demo；两侧取值不一致会让库内编出的流与使用侧看到的
		 * 常量分岔，故 CMake 侧对取值做 0 / 1 校验）。下面的默认值只兜不经 CMake 的裸编译。 */
#ifndef CLV_COEF_CODING
	#define CLV_COEF_CODING 1
#endif
		static_assert(CLV_COEF_CODING == 0 || CLV_COEF_CODING == 1, "CLV_COEF_CODING 只有 0 = 定宽 / 1 = EOB+EG 两档");
		inline constexpr CoefCoding kEncoderCoefCoding =
			(CLV_COEF_CODING == 0) ? CoefCoding::kFixed : CoefCoding::kEobEg;

		/* 每系数位宽（含 1 bit 符号）。intra = 帧内表，否则帧间表。两法的 bits(q') 同值：它定的是幅值域，不是载体。 */
		uint8_t BlockBits(uint8_t q, bool intra) noexcept;

		/* |QF| 上界（四级饱和域的第一级）。 */
		int32_t BlockQfMax(uint8_t q, bool intra) noexcept;

		/* 法 0 的单块字节数 = 64 × bits(q') ÷ 8。 */
		uint32_t BlockBytes(uint8_t q, bool intra) noexcept;

		/* 本法下单块的**最坏**比特数。变长只在平均意义上更短：Exp-Golomb 遇满幅系数比定宽还长
		 * （EG(QF_max) + 符号 对定宽 bits），所以 MB 级上界必须按本法重算，
		 * 沿用定宽上界会把稠密内容判成码流不成立。法 0 下恒等于 64 × bits(q')。 */
		uint32_t BlockBitsMax(uint8_t q, bool intra, CoefCoding coding) noexcept;

		/* 本法下单块的**最坏**字节数 = 向上取整到字节。法 0 下与 BlockBytes 恒等（64 × bits 是 8 的倍数）。 */
		uint32_t BlockBytesMax(uint8_t q, bool intra, CoefCoding coding) noexcept;

		/* 6 bit 块模式里 1 的个数 = 本宏块在场块数。 */
		int PresentBlockCount(uint8_t pattern) noexcept;

		/* 把一块的 64 个 QF 按本法的位序写进 bw。法 0 含符号位、不做零省略；法 1 写 all_zero + eob + EG 幅值。
		 * 调用方须保证 |QF| ≤ BlockQfMax(q, intra)，本函数不做上界检查（写侧的值来自量化器，越界在上游就判）。 */
		void EncodeBlock(const int32_t qf[kBlockCoefs], uint8_t q, bool intra, CoefCoding coding, BitWriter* bw);

		/* 从 br 按本法读一块的 64 个 QF。位不够 → kPayloadTooSmall；|QF| 越上界 → kValueRange；
		 * 法 0 读到 64 个全零 → kValueRange（本函数只解位图为 1 的块，全零块的位必为 0）；
		 * 法 1 读到 eob 位上是零 → kValueRange（eob 定义就是最后一个非零的下标）。 */
		VideoErr DecodeBlock(BitReader* br, uint8_t q, bool intra, CoefCoding coding, int32_t qf[kBlockCoefs]);

		/* 按块模式把在场块依次写进 out（块序 = 位序从 bit5 到 bit0），末尾统一对齐到字节。
		 * qf[i] 为第 i 块（0..3 亮度、4 = Cb、5 = Cr）的 64 个系数；模式位为 0 的块不读不写。 */
		VideoErr EncodeBlockData(uint8_t pattern, const int32_t qf[kBlocksPerMacroblock][kBlockCoefs], uint8_t q,
								 bool intra, CoefCoding coding, std::vector<uint8_t>* out);

		/* EncodeBlockData 的逆。模式位为 0 的块不占字节，出参 qf 里对应项恒被清零（按零残差处置）。
		 * 法 0：size 必须恰等于 present × BlockBytes，解完不得有剩余位；不等判 kValueRange。
		 * 法 1：size 只保证不超过 present × BlockBytesMax（位图全 0 的 MB 只接受 size = 0），
		 *       解完允许剩 0..7 位补齐，剩整字节说明与本法自定的块界矛盾，判 kValueRange。 */
		VideoErr DecodeBlockData(uint8_t pattern, const uint8_t* data, size_t size, uint8_t q, bool intra,
								 CoefCoding coding, int32_t qf[kBlocksPerMacroblock][kBlockCoefs]);

		/* 块数据字节数自校验：法 0 按恰等、法 1 按本法上界，present = 0 时两法都只接受 0。 */
		bool BlockSizeMatches(uint8_t pattern, uint32_t block_size, uint8_t q, bool intra, CoefCoding coding) noexcept;

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_BLOCK_H
