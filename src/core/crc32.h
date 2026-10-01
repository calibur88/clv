/* crc32.h
 *
 * CRC-32/MPEG-2：width 32、poly 0x04C11DB7、init 0xFFFFFFFF、不反射输入也不反射输出、xorout 0。
 * 参数量点："123456789" 的 check 值 0x0376E6E7，residue 恒为 0。
 */
#ifndef CLV_CORE_CRC32_H
#define CLV_CORE_CRC32_H

#include <cstddef>
#include <cstdint>

namespace clv::core
{

	// seed 就是上一段的返回值：分段累加与一次性算完等价。默认值即参数集的 init。
	// size 为 0 时返回 seed 本身（此时 data 可为 NULL）。
	uint32_t Crc32(const void* data, size_t size, uint32_t seed = 0xFFFFFFFFu) noexcept;

}	 // namespace clv::core

#endif	  // CLV_CORE_CRC32_H
