#include "timebase.h"

namespace clv
{
	namespace container
	{

		namespace
		{

			constexpr uint64_t kMask32 = 0xFFFFFFFFull;

		}	 // namespace

		U128 Mul64x64(uint64_t a, uint64_t b) noexcept
		{
			const uint64_t a0 = a & kMask32;
			const uint64_t a1 = a >> 32;
			const uint64_t b0 = b & kMask32;
			const uint64_t b1 = b >> 32;

			const uint64_t p00 = a0 * b0;
			const uint64_t p01 = a0 * b1;
			const uint64_t p10 = a1 * b0;
			const uint64_t p11 = a1 * b1;

			// 中间列最大 3 × (2^32-1)，不溢出 64 位；高出的部分进 hi
			const uint64_t mid = (p00 >> 32) + (p01 & kMask32) + (p10 & kMask32);

			U128 r;
			r.lo = (p00 & kMask32) | (mid << 32);
			r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
			return r;
		}

		U128 MulU128xU32(const U128& a, uint32_t b) noexcept
		{
			const U128 low = Mul64x64(a.lo, b);

			U128 r;
			r.lo = low.lo;
			r.hi = low.hi + a.hi * b;
			return r;
		}

		int CompareU128(const U128& a, const U128& b) noexcept
		{
			if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
			if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
			return 0;
		}

		int CompareUnifiedTick(const StreamTick& a, const StreamTick& b) noexcept
		{
			// 比较 dts_a × num_a / den_a 与 dts_b × num_b / den_b，
			// 交叉相乘后两侧同为 dts × num × 对方 den，量级 (2^64)(2^32)(2^32) < 2^128。
			const U128 lhs = MulU128xU32(Mul64x64(a.dts, a.num), b.den);
			const U128 rhs = MulU128xU32(Mul64x64(b.dts, b.num), a.den);
			return CompareU128(lhs, rhs);
		}

		bool TimebaseOk(uint64_t num, uint64_t den) noexcept
		{
			if (num == 0 || den == 0) return false;
			if (num > 0xFFFFFFFFull) return false;
			if (den > 0xFFFFFFFFull) return false;
			return true;
		}

		bool ToGlobalTicks(uint64_t v, uint32_t num, uint32_t den, uint64_t* ticks) noexcept
		{
			if (ticks == nullptr || num == 0 || den == 0) return false;

			const U128 step1 = Mul64x64(v, num);
			// 1081080000 < 2^31，乘进来仍在 128 位内（v < 2^64、num < 2^32）
			const U128 step2 = MulU128xU32(step1, kBaseTicksPerSecond);

			// u128 ÷ u32：32 位分块做长除，最高位商非零即说明商出了 u64
			const uint32_t limb[4] = { static_cast<uint32_t>(step2.hi >> 32), static_cast<uint32_t>(step2.hi),
									   static_cast<uint32_t>(step2.lo >> 32), static_cast<uint32_t>(step2.lo) };
			uint64_t rem = 0;
			uint64_t q = 0;
			for (int i = 0; i < 4; ++i)
			{
				const uint64_t cur = (rem << 32) | limb[i];	   // rem < den ≤ 2^32，故不溢出
				const uint32_t part = static_cast<uint32_t>(cur / den);
				rem = cur % den;
				if (i == 0 && part != 0) return false;
				q = (q << 32) | part;
			}

			*ticks = q;
			return true;
		}

	}	 // namespace container
}	 // namespace clv
