/* status.h
 *
 * 容器层的内部返回码。与公开头 include/CLV_Container.h 的 CLV_ContainerError 的前段（0..8）逐项同序，
 * 门面层直接 static_cast，不做映射表；顺序错位由门面侧的 static_assert 在编译期断掉。
 * 公开码 OUT_OF_MEMORY / UNSUPPORTED 两项没有内部对应项：容器实现层不产出它们。
 * 内部尾项 kExtChainTooLong 是例外：它对应公开枚举再往后一位的 CLV_CONTAINER_EXT_CHAIN_TOO_LONG
 * （公开侧只能在枚举尾部追加，两者序号对不上），由门面侧 ToApi() 显式映射，并由 static_assert 钉住这一对。
 */
#ifndef CLV_CONTAINER_STATUS_H
#define CLV_CONTAINER_STATUS_H

namespace clv
{
	namespace container
	{

		enum class ContainerErr : unsigned char
		{
			kOk = 0,
			kInvalidHandle,
			kInvalidArgument,
			kIoFailed,
			kBadStructure,	  // 文件头 / 描述符 / 索引头的 magic 或 CRC 不成立
			kValueRange,	  // 字段值域校验不过（非零 / u32 上限 / 本版强制值）
			kTooLarge,	  // 超写方侧上限：整帧或 WriteFrameChunks 单片 payload > max_packet_size（两处同档）、单包 payload 超读方可读上界、整帧载荷超单帧上界、流数 > 255、扩展块载荷超 ext_len 四字节可表达域
			kStreamNotFound,
			kStateError,	// 调用顺序不成立（如出包后再加流）
			/* 扩展块链逐块都成立、链路一直向前，但跳数到达 kMaxExtChainHops 仍没走完：只能判链太长，
			 * 读方不再往下认。畸形链不走这一档——分层见 reader.h 的处置口径与 WalkExtChains 的实现。 */
			kExtChainTooLong
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_STATUS_H
