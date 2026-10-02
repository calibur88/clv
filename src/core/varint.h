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

	// zigzag 有符号 varint：(n << 1) ^ (n >> 63)，n 是有符号 64 位。
	// 编进线上的是 zigzag 后的无符号值，故字节数与量程同 Decode/EncodeVarint。
	size_t EncodeZigZag(int64_t value, uint8_t* buf) noexcept;
	size_t DecodeZigZag(const uint8_t* buf, size_t size, int64_t* out) noexcept;

	// 该编码是否用了最少字节数（最短形式）。used 是 DecodeVarint 消费的字节数（1~10）。
	// 判定与 DecodeVarint 分开：解码只拒「第 10 字节规则内超量程」的一般 overlong 不拒，
	// 最短形式检查归容器层包校验，这里只提供原语。
	bool IsShortestVarint(const uint8_t* buf, size_t used) noexcept;

}	 // namespace clv::core

#endif	  // CLV_CORE_VARINT_H
