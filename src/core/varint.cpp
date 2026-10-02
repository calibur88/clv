#include "varint.h"

namespace clv::core
{

	namespace
	{

		// uint64 的 7 位分组数：ceil(64 / 7)
		constexpr size_t kMaxBytes = 10;

		// value 线上最少要占几个 7 位分组；0 也要占 1 字节
		constexpr size_t ShortestLen(uint64_t value) noexcept
		{
			size_t bits = 0;
			for (uint64_t v = value; v != 0; v >>= 1) ++bits;
			return bits == 0 ? 1 : (bits + 6) / 7;
		}

	}	 // namespace

	size_t EncodeVarint(uint64_t value, uint8_t* buf) noexcept
	{
		if (buf == nullptr) return 0;

		size_t n = 0;
		while (value >= 0x80u)
		{
			buf[n++] = static_cast<uint8_t>(value) | 0x80u;
			value >>= 7;
		}
		buf[n++] = static_cast<uint8_t>(value);
		return n;
	}

	size_t DecodeVarint(const uint8_t* buf, size_t size, uint64_t* out) noexcept
	{
		if (buf == nullptr || out == nullptr) return 0;

		uint64_t value = 0;
		size_t shift = 0;
		for (size_t i = 0; i < size && i < kMaxBytes; ++i)
		{
			const uint8_t byte = buf[i];
			// 第 10 字节的高位只剩 1 位空间：>0x01 就是表示不出来的 overlong 编码，拒绝而非折返
			if (i == kMaxBytes - 1 && (byte & 0x7Fu) > 1u) return 0;

			value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
			if ((byte & 0x80u) == 0)
			{
				*out = value;
				return i + 1;
			}
			shift += 7;
		}
		return 0;	 // 没有终止字节，或输入比编码短
	}

	// 全无符号式：负数走 (u << 1) ^ (0 - 符号位)，避开有符号左移的溢出未定义行为
	size_t EncodeZigZag(int64_t value, uint8_t* buf) noexcept
	{
		const uint64_t u = static_cast<uint64_t>(value);
		return EncodeVarint((u << 1) ^ (0ULL - (u >> 63)), buf);
	}

	size_t DecodeZigZag(const uint8_t* buf, size_t size, int64_t* out) noexcept
	{
		uint64_t u = 0;
		const size_t used = DecodeVarint(buf, size, &u);
		if (used == 0 || out == nullptr) return 0;

		*out = static_cast<int64_t>((u >> 1) ^ (0ULL - (u & 1U)));
		return used;
	}

	bool IsShortestVarint(const uint8_t* buf, size_t used) noexcept
	{
		if (buf == nullptr || used == 0 || used > kMaxBytes) return false;

		uint64_t value = 0;
		if (DecodeVarint(buf, used, &value) != used) return false;

		return ShortestLen(value) == used;
	}

}	 // namespace clv::core
