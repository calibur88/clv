/* frame.h
 *
 * FLAC 帧头的自核与解析。位域布局取自帧头一节的四张码表（块大小 / 采样率 / 声道 / 位深）与其中的扩展编码号写法：
 *
 *   byte0     同步码高 8 位（0xFF）
 *   byte1     同步码低 6 位 + 保留位（必须 0）+ 块策略位
 *   byte2     块大小码 4 位 / 采样率码 4 位
 *   byte3     声道码 4 位 / 位深码 3 位 / 保留位 1 位（必须 0）
 *   ...       扩展 UTF-8 编码号（固定块流 = 帧号，变长块流 = 首样本号）
 *   [extra]   块大小码 0b0110 → 8 位、0b0111 → 16 位（存储值 + 1）；
 *             采样率码 0b1100 → 8 位（kHz）、0b1101 → 16 位（Hz）、0b1110 → 16 位（Hz ÷ 10）
 *   byte      帧头 CRC-8：多项式 0x07、初值 0、不反射、无收尾异或，覆盖帧头全部前置字节
 *   帧尾 2 字节 帧 CRC-16：多项式 0x8005、初值 0、不反射，覆盖整帧除 CRC-16 自身
 *
 * 采样率码 0b0000 与位深码 0b000 表示「值只在流信息里给」：本层解析为 0，
 * 由调用方回落流声明——该路径为鲁棒性保留，不参与流式子集合法性判定。
 * 帧长不在帧头里，所以本层只判帧头自洽，不推断帧边界。
 */
#ifndef CLV_AUDIO_FRAME_H
#define CLV_AUDIO_FRAME_H

#include <cstddef>
#include <cstdint>

namespace clv
{
	namespace audio
	{

		/* 流式子集的界值判据：本头是上界三值、下界两值与采样率拒档的唯一存放处，后端适配层与门面只引用这里。
		 * 上界（本库流式子集声明的上界）：这三值是校验上限，越界即判流式子集不成立；
		 * 4608 那条只在 sample_rate ≤ 48 000 时生效（48 kHz 以下才要求块大小不超过一帧的样本数）。 */
		inline constexpr uint32_t kSubsetMaxBlocksize = 16384u;
		inline constexpr uint32_t kSubsetRateThresholdForShortBlock = 48000u;
		inline constexpr uint32_t kSubsetMaxBlocksizeAtLowRate = 4608u;

		/* 块大小下界（本库流式子集声明的下界，按位深分档）：低于下界的块大小一律判「取值不成立」。 */
		inline constexpr uint32_t kSubsetMinBlocksize16 = 64u;	  // 16-bit 时
		inline constexpr uint32_t kSubsetMinBlocksize24 = 72u;	  // 24-bit 时

		/* 采样率的越子集档边界：大于等于本常量的采样率一律被后端在编码器初始化时拒掉
		 * （公开面可探针复现：这一档返回底层失败而非受理）。实际可接受区间的上界在此值以下、由后端决定，
		 * 本头只登记被拒档的边界，不另抄一份后端内部判定值。 */
		inline constexpr uint32_t kSubsetSampleRateRejectFloor = 655355u;

		struct FrameHead
		{
			uint32_t blocksize = 0;		 // 0 = 只给在流信息里
			uint32_t sample_rate = 0;	 // 0 = 只给在流信息里
			uint16_t channels = 0;
			uint16_t bit_depth = 0;			  // 0 = 只给在流信息里
			uint64_t coded_number = 0;		  // 帧号或首样本号
			uint8_t blocking_strategy = 0;	  // 0 = 固定块，1 = 变长块
			uint8_t header_size = 0;		  // 帧头字节数（含 CRC-8）
		};

		/* 解帧头并自核：同步码、两个保留位、码表的保留/禁用值、编码号字节合法性、
		 * 变长与固定块的编号语义、帧头 CRC-8。size 为可用字节数，够不够由内部判。
		 * 返回 false 时 *out 不被写入。 */
		bool ParseFrameHead(const uint8_t* data, size_t size, FrameHead* out);

		/* 整帧 CRC-16 复核：覆盖 [0, frame_size - 2)，末 2 字节为大端校验值。
		 * frame_size < 3（帧头 + 至少 2 字节尾）不成立。 */
		bool FrameCrcOk(const uint8_t* data, size_t frame_size);

		/* 流式子集合法性：块大小 ≤ 16384；采样率 ≤ 48000 时块大小 ≤ 4608；
		 * 采样率与位深的「只在流信息里给」按 0 传入，不参与判定。
		 * 采样率 0 或位深 0 视为未知，返回 true（合法性由流信息那一路判）。 */
		bool StreamableSubsetOk(uint32_t blocksize, uint32_t sample_rate);

		uint8_t Crc8(const uint8_t* data, size_t size);
		uint16_t Crc16(const uint8_t* data, size_t size);

	}	 // namespace audio
}	 // namespace clv

#endif	  // CLV_AUDIO_FRAME_H

