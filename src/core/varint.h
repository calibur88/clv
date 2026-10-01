/* varint.h
 *
 * 无符号 LEB128。uint64 最多 10 字节，第 10 字节只允许贡献 1 位。
 */
#ifndef CLV_CORE_VARINT_H
#define CLV_CORE_VARINT_H

#include <cstddef>
#include <cstdint>

namespace clv::core
{

	// 返回写入的字节数（1~10）；buf 至少 10 字节。buf 为 NULL 返回 0。
	size_t EncodeVarint(uint64_t value, uint8_t* buf) noexcept;

	// 返回消费字节数（1~10）；out 只在成功时写入。
	// 失败返回 0：输入不含终止字节（被截断）、编码超过 10 字节、或第 10 字节是 overlong
	// （值超出 uint64 量程）——后两类一律拒绝，不做静默截断。
	size_t DecodeVarint(const uint8_t* buf, size_t size, uint64_t* out) noexcept;

}	 // namespace clv::core

#endif	  // CLV_CORE_VARINT_H
