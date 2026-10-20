#include "bits.h"

namespace clv
{
	namespace video
	{

		void BitWriter::Write(uint32_t value, uint8_t n_bits)
		{
			/* 上界与读侧同一条判：BitReader::Read 在入口拒 `n_bits > 32u`（见本件读侧那一段），
			 * 写侧受理同一个界 —— 「一次最多 32 位」是位流自己的约定（bits.h 文件头），
			 * 越界时 `value >> i` 的移位本身也不成立。判并入「0 位不写」这道既有早退，
			 * 不为上界另开一条分支。库内所有写点的位宽都在这条界内（最长的幅值档 16 位）。 */
			if (n_bits == 0u || n_bits > 32u) return;
			for (int i = n_bits - 1; i >= 0; --i)
			{
				pending_ = (pending_ << 1u) | ((value >> i) & 1u);
				++pending_bits_;
				if (pending_bits_ == 8u)
				{
					buf_.push_back(static_cast<uint8_t>(pending_ & 0xFFu));
					pending_ = 0u;
					pending_bits_ = 0u;
				}
			}
			data_bits_ += n_bits;
		}

		void BitWriter::WriteBytes(const uint8_t* p, size_t n)
		{
			if (p == nullptr) return;
			for (size_t i = 0; i < n; ++i) Write(p[i], 8u);
		}

		void BitWriter::AlignToByte(uint8_t pad_bit)
		{
			const size_t data = data_bits_;
			while (pending_bits_ != 0u) Write(pad_bit & 1u, 1u);
			data_bits_ = data;
		}

		bool BitReader::ReadBit(uint8_t* bit)
		{
			if (bit == nullptr) return false;
			if (pos_ >= bits_) return false;
			const size_t byte = pos_ / 8u;
			const size_t idx = 7u - pos_ % 8u;
			*bit = static_cast<uint8_t>((data_[byte] >> idx) & 1u);
			++pos_;
			return true;
		}

		bool BitReader::Read(uint32_t* value, uint8_t n_bits)
		{
			if (value == nullptr) return false;
			if (n_bits > 32u) return false;
			if (BitsLeft() < n_bits) return false;
			uint32_t v = 0u;
			for (uint8_t i = 0; i < n_bits; ++i)
			{
				uint8_t b = 0u;
				ReadBit(&b);
				v = (v << 1u) | b;
			}
			*value = v;
			return true;
		}

		bool BitReader::ReadBytes(uint8_t* dst, size_t n)
		{
			if (dst == nullptr && n != 0u) return false;
			if (BitsLeft() < n * 8u) return false;
			for (size_t i = 0; i < n; ++i)
			{
				uint32_t v = 0u;
				Read(&v, 8u);
				dst[i] = static_cast<uint8_t>(v);
			}
			return true;
		}

	}	 // namespace video
}	 // namespace clv
