/* bits.h
 *
 * 位级读写。宏块头的 mcbpc / cbpy 是 1..9 bit 的变长码，逐字节缓冲拼不出相邻码字。
 *
 * 约定：先读的位放在高位（MSB-first），写侧同序 —— 位流里不出现位序标记，两侧都按这条读。
 * 一次最多 32 位。写入的位数与补齐到字节边界的位数分开记：BitCount() 只数数据位。
 */
#ifndef CLV_VIDEO_BITS_H
#define CLV_VIDEO_BITS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace clv
{
	namespace video
	{

		class BitWriter
		{
			public:

			/* 取 value 的低 n_bits 位，高位先出。n_bits = 0 不写；越过「一次最多 32 位」这条界也不写，
			 * 拒档与 BitReader::Read 的 `n_bits > 32u` 同值 —— 两侧受理的位宽域是同一个。 */
			void Write(uint32_t value, uint8_t n_bits);
			/* 8 位一组原样写入，不要求当前字节对齐。 */
			void WriteBytes(const uint8_t* p, size_t n);

			size_t BitCount() const noexcept { return data_bits_; }

			/* 不足一字节时用 pad_bit 填满；已对齐则什么都不做。
			 * 未填满的最后一个字节还挂在内部缓冲里，Bytes() 不含它——要拿到全部字节得先 AlignToByte(pad_bit)。 */
			void AlignToByte(uint8_t pad_bit);

			const std::vector<uint8_t>& Bytes() const noexcept { return buf_; }

			private:

			std::vector<uint8_t> buf_;
			uint32_t pending_ = 0u;
			uint8_t pending_bits_ = 0u;
			size_t data_bits_ = 0u;
		};

		class BitReader
		{
			public:

			BitReader(const uint8_t* data, size_t size) noexcept: data_(data), bits_(size * 8u) {}

			bool ReadBit(uint8_t* bit);
			/* 读 n_bits 位，高位先读到 value 的低端。剩余位不足则整次不消费并返回 false。 */
			bool Read(uint32_t* value, uint8_t n_bits);
			bool ReadBytes(uint8_t* dst, size_t n);

			size_t BitsLeft() const noexcept { return pos_ < bits_ ? bits_ - pos_ : 0u; }

			private:

			const uint8_t* data_;
			size_t bits_;
			size_t pos_ = 0u;
		};

	}	 // namespace video
}	 // namespace clv

#endif	  // CLV_VIDEO_BITS_H
