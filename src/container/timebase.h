/* timebase.h
 *
 * 统一时间轴比较与时间基值域。跨流比较只用整数，不转浮点。
 * 128 位整数没有可移植的类型（MSVC 没有 `__int128`，GCC/Clang 的扩展不通用），
 * 因此自带 64×64→128 的无符号乘法与 u128÷u32 长除，全用 32 位分块拼。
 */
#ifndef CLV_CONTAINER_TIMEBASE_H
#define CLV_CONTAINER_TIMEBASE_H

#include <cstdint>

namespace clv
{
	namespace container
	{

		struct U128
		{
			uint64_t hi = 0;
			uint64_t lo = 0;
		};

		U128 Mul64x64(uint64_t a, uint64_t b) noexcept;
		// 前提：a 的高 64 位 < 2^32（即乘积 < 2^96）。本模块的两次乘法都满足；
		// 超出这个范围时 a.hi * b 会在 64 位里溢出，静默给出错的高位。
		U128 MulU128xU32(const U128& a, uint32_t b) noexcept;
		int CompareU128(const U128& a, const U128& b) noexcept;

		// 一条流上某个时间戳的换算因子：t = dts × num / den
		struct StreamTick
		{
			uint64_t dts = 0;
			uint32_t num = 1;
			uint32_t den = 1;
		};

		// a ≤ b 返回 <= 0，相等返回 0，a > b 返回 > 0
		int CompareUnifiedTick(const StreamTick& a, const StreamTick& b) noexcept;

		// timebase_num / timebase_den 线上各占 8 字节，但值域限 u32 且非零，超出即判结构不成立。
		// 不封合法集上限（哪些取值「合理」属上层策略，容器层只挡不可表示的值）。
		bool TimebaseOk(uint64_t num, uint64_t den) noexcept;

		// 基础时间基：1,081,080,000 ticks/秒。文件头 duration_ticks 用它。
		constexpr uint64_t kBaseTicksPerSecond = 1081080000ull;

		/* 把某流的时间戳换算到全局 tick：ticks = v × num × 1081080000 / den。
	 * 全程整数。乘积 < 2^128；商不落在 u64 内时返回 false。 */
		bool ToGlobalTicks(uint64_t v, uint32_t num, uint32_t den, uint64_t* ticks) noexcept;

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_TIMEBASE_H
