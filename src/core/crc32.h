/* crc32.h
 *
 * CRC-32/MPEG-2：width 32、poly 0x04C11DB7、init 0xFFFFFFFF、不反射输入也不反射输出、xorout 0。
 * 参数量点："123456789" 的 check 值 0x0376E6E7；residue 是 0x00000000 —— 这不是巧合：
 * xorout 为 0 且两端不反射，把算得的 CRC 按大端接回消息尾部再跑一遍，寄存器整段抵消。
 * 带 final xor 的同多项式变体（如 CRC-32/BZIP2）residue 不是 0，别把两者的数抄到一起。
 * 本层不靠这条性质做校验（校验走「重算再比」），写下它只为免得后来人把 0 当成笔误去"修"。
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
