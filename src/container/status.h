/* status.h
 *
 * 容器层的内部返回码。与公开头 include/CLV_Container.h 的 CLV_ContainerError 逐项同序，
 * 门面层直接 static_cast，不做映射表。
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
			kValueRange,	  // 字段值域校验不过（非零 / u32 上限 / v1 强制值）
			kTooLarge,		  // 超写方侧上限：payload > max_packet_size、流数 > 255、扩展块数据 > 0xFFFF、偏移 > 4 GiB
			kStreamNotFound,
			kStateError,	// 调用顺序不成立（如出包后再加流）
			kOutOfMemory,
			kUnsupported
		};

	}	 // namespace container
}	 // namespace clv

#endif	  // CLV_CONTAINER_STATUS_H
